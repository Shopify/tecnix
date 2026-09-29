#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/tecnix/trace.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"

namespace nix {

/**
 * Tracing unit tests: the recorder and the slot table, exercised through
 * `forceValue` under tracing `TrackingContext`s and read back with
 * `tecnixTraceSnapshot`. Nothing here asserts on a duration.
 */
class TecnixTracingTest : public LibExprTest
{
protected:
    std::filesystem::path traceDir;

    TecnixTracingTest(unsigned evalCores = 1)
        : LibExprTest(openStore("dummy://"), [&](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            settings.evalCores = evalCores;
            return settings;
        })
    {
        traceDir = createTempDir();
        evalSettings.tecnixTrace = traceDir.string();
        EXPECT_NE(ensureTecnixTraceSession(state), nullptr);
    }

    ~TecnixTracingTest() override
    {
        std::filesystem::remove_all(traceDir);
    }

    static uint64_t slot(const Value & v)
    {
        return tecnixValueTraceLoad(&v, std::memory_order_acquire);
    }

    Value * attr(Value & attrs, std::string_view name)
    {
        auto * a = attrs.attrs()->get(state.symbols.create(name));
        if (!a)
            throw Error("test attrset has no attribute '%s'", name);
        return a->value;
    }

    static const TraceRecord * record(const TraceSnapshot & s, uint64_t id)
    {
        for (auto & r : s.records)
            if (r.id == id)
                return &r;
        return nullptr;
    }

    static std::vector<TraceRecord> ofKind(const TraceSnapshot & s, TraceRecord::Kind kind)
    {
        std::vector<TraceRecord> out;
        for (auto & r : s.records)
            if (r.kind == kind)
                out.push_back(r);
        return out;
    }

    static uint32_t rootId(const TraceSnapshot & s, std::string_view name)
    {
        for (auto & [id, n] : s.roots)
            if (n == name)
                return id;
        return 0;
    }
};

TEST_F(TecnixTracingTest, forcedThunkGetsRecordAndSlotAtPublish)
{
    auto attrs = eval("{ a = 1 + 1; }", false);
    state.forceValue(attrs, noPos);
    auto * a = attr(attrs, "a");
    ASSERT_TRUE(a->isThunk());
    EXPECT_EQ(slot(*a), 0u);

    TrackingContext ctx(state, "t1");
    ASSERT_TRUE(ctx.trace);
    {
        ActiveTrackingContext active(ctx);
        state.forceValue(*a, noPos);
    }

    auto s = tecnixTraceSnapshot(state);
    auto values = ofKind(s, TraceRecord::Value);
    ASSERT_EQ(values.size(), 1u);
    auto & r = values[0];
    EXPECT_EQ(slot(*a), tecnixTracePackSlot(r.id, r.root));
    EXPECT_EQ(rootId(s, "t1"), r.root);
    EXPECT_EQ(r.parent, ctx.trace->record);
    EXPECT_EQ(r.keyKind, TraceRecord::Pos);
    EXPECT_NE(r.key, 0u);
    EXPECT_GE(r.dur, r.selfDur);
    EXPECT_GE(r.gcAlloc, r.selfGcAlloc);
    EXPECT_TRUE(s.reuses.empty()) << "a same-root use is not a reuse";
}

TEST_F(TecnixTracingTest, recordsNestAndFeedTheirParent)
{
    auto attrs = eval("{ outer = let x = 1 + 1; y = x + 1; in y + x; }", false);
    state.forceValue(attrs, noPos);
    auto * outer = attr(attrs, "outer");

    uint64_t rootRecord = 0;
    {
        TrackingContext ctx(state, "t1");
        rootRecord = ctx.trace->record;
        ActiveTrackingContext active(ctx);
        state.forceValue(*outer, noPos);
    }

    auto s = tecnixTraceSnapshot(state);
    auto values = ofKind(s, TraceRecord::Value);
    ASSERT_GE(values.size(), 3u) << "outer, y and x";
    auto outerSlot = slot(*outer);
    auto * outerRecord = record(s, tecnixTraceSlotRecord(outerSlot));
    ASSERT_NE(outerRecord, nullptr);
    EXPECT_EQ(outerRecord->parent, rootRecord);

    int64_t childDur = 0, childAlloc = 0;
    size_t children = 0;
    for (auto & r : values) {
        if (r.parent != outerRecord->id)
            continue;
        children++;
        childDur += r.dur;
        childAlloc += r.gcAlloc;
        EXPECT_GE(r.start, outerRecord->start);
    }
    EXPECT_GE(children, 1u);
    EXPECT_EQ(outerRecord->selfDur, std::max<int64_t>(0, outerRecord->dur - childDur));
    EXPECT_EQ(outerRecord->selfGcAlloc, outerRecord->gcAlloc - childAlloc);

    // The root closed after everything and accounts for its children.
    auto roots = ofKind(s, TraceRecord::Root);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].id, rootRecord);
    EXPECT_EQ(roots[0].parent, 0u);
    EXPECT_GE(roots[0].dur, outerRecord->dur);
    EXPECT_EQ(roots[0].gcAlloc - roots[0].selfGcAlloc, outerRecord->gcAlloc);
}

TEST_F(TecnixTracingTest, aliasesKeepTheProducersIdentity)
{
    /* `b` and `c` are selections whose results are copies of `v` (a plain
       `b = a` would share a's cell outright). */
    auto attrs = eval("rec { a = { v = 1 + 1; }; b = a.v; c = b; d = a.v; }", false);
    state.forceValue(attrs, noPos);
    auto * a = attr(attrs, "a");
    auto * b = attr(attrs, "b");
    auto * d = attr(attrs, "d");

    uint64_t rootRecord = 0;
    {
        TrackingContext ctx(state, "t1");
        rootRecord = ctx.trace->record;
        ActiveTrackingContext active(ctx);
        state.forceValue(*b, noPos); // forces b -> a, v; b's value is a copy of v
        state.forceValue(*d, noPos); // a and v finished: d is a copy of v
    }

    auto * v = attr(*a, "v");
    ASSERT_NE(slot(*v), 0u);
    EXPECT_EQ(slot(*b), slot(*v)) << "b's value was copied from v";
    EXPECT_EQ(slot(*d), slot(*v));
    EXPECT_EQ(slot(*attr(attrs, "c")), slot(*v)) << "c shares b's cell";

    auto s = tecnixTraceSnapshot(state);
    EXPECT_EQ(ofKind(s, TraceRecord::Value).size(), 4u) << "b, a, v, d: aliases are records of their own work";
    auto * vRecord = record(s, tecnixTraceSlotRecord(slot(*v)));
    ASSERT_NE(vRecord, nullptr);
    auto * bRecord = record(s, vRecord->parent);
    ASSERT_NE(bRecord, nullptr) << "v was first needed while computing b";
    EXPECT_EQ(bRecord->parent, rootRecord);
    EXPECT_NE(bRecord->id, vRecord->id);
}

TEST_F(TecnixTracingTest, copiesOutsideAFrameKeepTheSlot)
{
    auto attrs = eval("{ a = 1 + 1; }", false);
    state.forceValue(attrs, noPos);
    auto * a = attr(attrs, "a");

    TrackingContext ctx(state, "t1");
    ActiveTrackingContext active(ctx);
    state.forceValue(*a, noPos);
    ASSERT_NE(slot(*a), 0u);

    Value copy;
    copy = *a;
    EXPECT_EQ(slot(copy), slot(*a));
}

TEST_F(TecnixTracingTest, recycledCellDoesNotInheritTheSlot)
{
    auto attrs = eval("{ a = 1 + 1; }", false);
    state.forceValue(attrs, noPos);
    auto * a = attr(attrs, "a");

    TrackingContext ctx(state, "t1");
    ActiveTrackingContext active(ctx);
    state.forceValue(*a, noPos);
    ASSERT_NE(slot(*a), 0u);

    // The cell is reused for a new value: the finish hook clears the slot.
    a->mkInt(5);
    EXPECT_EQ(slot(*a), 0u);

    // Also for a value finished outside any tracking context.
    Value plain;
    plain = *attr(attrs, "a");
    EXPECT_EQ(slot(plain), 0u);
}

TEST_F(TecnixTracingTest, crossRootUsesAreRecordedOncePerProducer)
{
    auto attrs = eval("rec { a = { v = 1 + 1; }; b = a.v; own = 2 + 2; }", false);
    state.forceValue(attrs, noPos);
    auto * b = attr(attrs, "b");
    auto * own = attr(attrs, "own");

    {
        TrackingContext ctx(state, "producer");
        ActiveTrackingContext active(ctx);
        state.forceValue(*b, noPos); // computes b, a and v
    }
    auto * v = attr(*attr(attrs, "a"), "v");
    {
        TrackingContext ctx(state, "consumer");
        ActiveTrackingContext active(ctx);
        state.forceValue(*b, noPos);   // finished alias: a reuse of v, the producer
        state.forceValue(*b, noPos);   // same producer again: no second row
        state.forceValue(*v, noPos);   // v itself: still the same producer
        state.forceValue(*own, noPos); // this root's own work: no row
    }

    auto s = tecnixTraceSnapshot(state);
    ASSERT_EQ(s.reuses.size(), 1u);
    EXPECT_EQ(s.reuses[0].root, rootId(s, "consumer"));
    EXPECT_EQ(s.reuses[0].producer, tecnixTraceSlotRecord(slot(*v)));
    EXPECT_EQ(tecnixTraceSlotRoot(slot(*v)), rootId(s, "producer"));
    EXPECT_EQ(tecnixTraceSlotRoot(slot(*own)), rootId(s, "consumer"));
}

TEST_F(TecnixTracingTest, nestedRootIsAChildOfTheOpenFrame)
{
    auto attrs = eval("{ a = 1 + 1; }", false);
    state.forceValue(attrs, noPos);
    auto * a = attr(attrs, "a");

    TrackingContext outer(state, "outer");
    ActiveTrackingContext activeOuter(outer);
    uint64_t innerRecord = 0;
    {
        // A root opened while another is active (a resolver calling a
        // Tecnix builtin) hangs off the enclosing root.
        TrackingContext inner(state, "inner");
        innerRecord = inner.trace->record;
        EXPECT_EQ(inner.trace->parentRecord, outer.trace->record);
        ActiveTrackingContext activeInner(inner);
        state.forceValue(*a, noPos);
    }

    auto s = tecnixTraceSnapshot(state);
    auto * inner = record(s, innerRecord);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->kind, TraceRecord::Root);
    EXPECT_EQ(inner->parent, outer.trace->record);
    EXPECT_EQ(outer.trace->childDur, inner->dur) << "the inner root fed the outer root's accumulators";
}

TEST_F(TecnixTracingTest, importsKeyOnTheFile)
{
    auto dir = createTempDir();
    auto file = dir + "/lib.nix";
    writeFile(file, "{ v = 40 + 2; }");

    TrackingContext ctx(state, "t1");
    {
        ActiveTrackingContext active(ctx);
        Value v;
        state.evalFile(state.rootPath(CanonPath(file)), v);
        state.forceValue(v, noPos);
    }

    auto s = tecnixTraceSnapshot(state);
    std::vector<TraceRecord> imports;
    for (auto & r : s.records)
        if (r.keyKind == TraceRecord::Import)
            imports.push_back(r);
    ASSERT_EQ(imports.size(), 1u);
    ASSERT_LT(imports[0].key, s.importKeys.size());
    EXPECT_TRUE(s.importKeys[imports[0].key].ends_with("/lib.nix")) << s.importKeys[imports[0].key];
    EXPECT_EQ(imports[0].kind, TraceRecord::Value);
    std::filesystem::remove_all(dir);
}

TEST_F(TecnixTracingTest, dumpWritesAQueryableFile)
{
    auto attrs = eval("rec { a = { v = 1 + 1; }; b = a.v; }", false);
    state.forceValue(attrs, noPos);
    {
        TrackingContext ctx(state, "producer");
        ActiveTrackingContext active(ctx);
        state.forceValue(*attr(attrs, "b"), noPos);
    }
    {
        TrackingContext ctx(state, "consumer");
        ActiveTrackingContext active(ctx);
        state.forceValue(*attr(attrs, "b"), noPos);
    }
    auto snapshot = tecnixTraceSnapshot(state);
    tecnixTraceDump(state);
    EXPECT_TRUE(tecnixTraceSnapshot(state).records.empty()) << "dumped rows are released";

    std::vector<std::filesystem::path> files;
    for (auto & entry : std::filesystem::directory_iterator(traceDir))
        files.push_back(entry.path());
    ASSERT_EQ(files.size(), 1u);

    SQLite db(files[0], {.useWAL = true});
    auto count = [&](const std::string & sql) -> int64_t {
        SQLiteStmt stmt(db, sql);
        auto use = stmt.use();
        if (!use.next()) {
            ADD_FAILURE() << "no row: " << sql;
            return -1;
        }
        return use.getInt(0);
    };
    EXPECT_EQ(count("select count(*) from records"), (int64_t) snapshot.records.size());
    EXPECT_EQ(count("select count(*) from roots"), 2);
    EXPECT_EQ(count("select count(*) from reuses"), 1);
    EXPECT_EQ(count("select count(*) from records where kind = 'root'"), 2);
    EXPECT_EQ(
        count("select count(*) from records r where r.expr is not null and r.expr not in (select id from exprs)"), 0)
        << "every key resolves to an exprs row";
    // The views parse and agree with the tables: `a` was reused by `consumer`.
    EXPECT_EQ(count("select count(*) from shared"), 1);
    EXPECT_EQ(count("select count(*) from consumers"), 1);
    EXPECT_EQ(count("select consumers from floor"), 2);
    EXPECT_EQ(count("select count(*) from own"), 2);
    EXPECT_EQ(count("select count(*) from target_costs"), 2);
    EXPECT_EQ(
        count(
            "select abs(sum(total_dur_amortized) - (select sum(self_dur) from records where kind in ('value', 'root'))) "
            "<= (select count(*) from roots) from target_costs"),
        1)
        << "amortised totals partition the run (to per-root rounding)";
    EXPECT_EQ(count("select count(*) from meta where key = 'format_version' and value = '2'"), 1);

    // A second dump appends without duplicating schema objects.
    {
        auto later = eval("{ c = 3 + 3; }");
        TrackingContext ctx(state, "later");
        ActiveTrackingContext active(ctx);
        state.forceValue(*attr(later, "c"), noPos);
    }
    tecnixTraceDump(state);
    EXPECT_EQ(count("select count(*) from roots"), 3);
    EXPECT_EQ(count("select count(*) from meta where key = 'dumps' and value = '2'"), 1);
}

/**
 * Waits: one root forces a thunk another thread is already computing. The
 * thunk reads a FIFO, so the computing thread blocks until the test writes
 * to it; the other thread must wait for the value rather than compute it.
 */
class TecnixTracingParallelTest : public TecnixTracingTest
{
protected:
    TecnixTracingParallelTest()
        : TecnixTracingTest(2)
    {
    }
};

TEST_F(TecnixTracingParallelTest, waitsAreRecordedAsChildrenWithoutWork)
{
    ASSERT_TRUE(state.executor->enabled);

    auto dir = createTempDir();
    auto fifo = dir + "/fifo";
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);

    auto attrs = eval(fmt("{ x = builtins.readFile \"%s\"; }", fifo), false);
    state.forceValue(attrs, noPos);
    auto * x = attr(attrs, "x");

    // A worker computes `x` under its own root; it blocks in readFile.
    Executor::WorkItems work;
    state.addWork(work, 0, [&]() {
        TrackingContext ctx(state, "computer");
        ActiveTrackingContext active(ctx);
        state.forceValue(*x, noPos);
    });
    auto futures = state.executor->spawn(std::move(work));

    // Give the worker time to claim the thunk, then write the FIFO once this
    // thread is (very likely) waiting on it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::thread writer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int fd = open(fifo.c_str(), O_WRONLY);
        ASSERT_GE(fd, 0);
        (void) !::write(fd, "hello", 5);
        close(fd);
    });
    {
        TrackingContext ctx(state, "waiter");
        ActiveTrackingContext active(ctx);
        state.forceValue(*x, noPos);
    }
    writer.join();
    for (auto & f : futures)
        ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    for (auto & f : futures)
        f.get();

    auto s = tecnixTraceSnapshot(state);
    auto values = ofKind(s, TraceRecord::Value);
    auto waits = ofKind(s, TraceRecord::Wait);
    ASSERT_EQ(values.size(), 1u) << "the value was computed once";
    ASSERT_EQ(waits.size(), 1u) << "and waited for once";
    EXPECT_NE(values[0].root, waits[0].root);
    EXPECT_EQ(waits[0].gcAlloc, 0) << "waits carry no work";
    EXPECT_EQ(waits[0].selfGcAlloc, 0);
    // Whichever root waited recorded a reuse of the record that computed x.
    ASSERT_EQ(s.reuses.size(), 1u);
    EXPECT_EQ(s.reuses[0].root, waits[0].root);
    EXPECT_EQ(s.reuses[0].producer, values[0].id);
    EXPECT_EQ(tecnixTraceSlotRecord(slot(*x)), values[0].id);
    // The wait is a child of the waiting root, not of the value.
    auto * waitRoot = record(s, waits[0].parent);
    ASSERT_NE(waitRoot, nullptr);
    EXPECT_EQ(waitRoot->kind, TraceRecord::Root);
    EXPECT_EQ(waitRoot->root, waits[0].root);
    std::filesystem::remove_all(dir);
}

} // namespace nix
