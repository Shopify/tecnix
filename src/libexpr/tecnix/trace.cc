/**
 * Tecnix evaluation tracing (plans/tecnix-tracing/README.md): the recorder
 * behind `forceValueTracked`'s tracing branches, the value-slot side table,
 * the per-thread buffers, the GC-pause clock, the background writer that
 * streams records into the trace file, and the end-of-call dump.
 *
 * Nothing here formats a string, takes a lock or touches disk on an
 * evaluation thread, other than handing a full chunk to the writer (one
 * mutex acquisition per 256 Ki records). Root creation and import-key
 * interning take the session mutex; both happen once per target or per
 * file, not per force.
 */

#include "tecnix/trace-session.hh"
#include "tecnix/eval-data.hh"

#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/fmt.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <chrono>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif

namespace nix {

/* --- The slot table --------------------------------------------------- */

std::atomic<uint64_t *> tecnixValueTraceDir[tecnixValueLabelDirSize];
std::atomic<bool> tecnixTraceEverEnabled{false};

uint64_t * tecnixInstallValueTraceChunk(size_t dirIndex)
{
    static_assert(sizeof(void *) == 8, "the Tecnix trace slot table requires a 64-bit address space");

    size_t chunkBytes = (size_t{1} << 32) / 16 * sizeof(uint64_t);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void * mem = mmap(nullptr, chunkBytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "nix: failed to map a Tecnix trace slot chunk\n");
        abort();
    }

    auto * chunk = static_cast<uint64_t *>(mem);
    uint64_t * expected = nullptr;
    if (!tecnixValueTraceDir[dirIndex].compare_exchange_strong(
            expected, chunk, std::memory_order_release, std::memory_order_acquire)) {
        munmap(mem, chunkBytes);
        return expected;
    }
    return chunk;
}

/* --- Clock and GC pauses ---------------------------------------------- */

int64_t traceNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 * Nanoseconds spent in garbage collection so far, process-wide. Frames read
 * it at open and close so every record excludes the pauses that fell inside
 * it, which keeps `dur - sum(children)` consistent.
 */
static std::atomic<int64_t> gcPauseTotal{0};

struct GcPause
{
    int64_t start, end;
};

/**
 * Every pause, for the `gc` table. Preallocated: the callback runs under the
 * collector's lock with the world stopped and must not allocate. When full,
 * the total keeps counting and rows are dropped. Published through
 * `gcPauseCount` so the dump can read a prefix while a collection may run.
 */
static std::vector<GcPause> gcPauses;
static std::atomic<size_t> gcPauseCount{0};
static constexpr size_t maxGcPauses = size_t{1} << 20;

#if NIX_USE_BOEHMGC
static int64_t gcPauseStart = 0;
static GC_on_collection_event_proc previousGcEventProc = nullptr;

static void GC_CALLBACK onGcEvent(GC_EventType event)
{
    if (event == GC_EVENT_START)
        gcPauseStart = traceNow();
    else if (event == GC_EVENT_END) {
        auto end = traceNow();
        gcPauseTotal.fetch_add(end - gcPauseStart, std::memory_order_relaxed);
        auto n = gcPauseCount.load(std::memory_order_relaxed);
        if (n < gcPauses.capacity()) {
            gcPauses.push_back({gcPauseStart, end}); // within capacity: no allocation
            gcPauseCount.store(n + 1, std::memory_order_release);
        }
    }
    if (previousGcEventProc)
        previousGcEventProc(event);
}
#endif

static void installGcHooks()
{
    static std::once_flag once;
    std::call_once(once, [] {
        gcPauses.reserve(maxGcPauses);
#if NIX_USE_BOEHMGC
        previousGcEventProc = GC_get_on_collection_event();
        GC_set_on_collection_event(onGcEvent);
#endif
    });
}

static int64_t clampNonNegative(int64_t x) noexcept
{
    return x < 0 ? 0 : x;
}

/* --- Buffers ------------------------------------------------------------ */

void TraceThreadBuffer::append(const TraceRecord & record) noexcept
{
    if (current.size() >= chunkRecords) {
        /* Hand the full chunk to the writer. `enqueue` may block briefly if
           the writer is behind, and may throw if memory is exhausted, in
           which case the chunk is lost and counted. */
        std::vector<TraceRecord> full = std::move(current);
        current = std::vector<TraceRecord>();
        try {
            session.writer.enqueue(std::move(full));
        } catch (...) {
            dropped += chunkRecords;
        }
    }
    if (current.capacity() == 0) {
        try {
            current.reserve(chunkRecords);
        } catch (...) {
            dropped++;
            return;
        }
    }
    current.push_back(record); // capacity reserved: cannot throw
}

void TraceThreadBuffer::appendReuse(TraceReuse reuse) noexcept
{
    try {
        reuses.push_back(reuse);
    } catch (...) {
        dropped++;
    }
}

/* --- Session ------------------------------------------------------------ */

TraceSession::TraceSession(EvalState & state, std::filesystem::path dir)
    : state(state)
    , dir(dir)
    , epoch(traceNow())
    , writer(dir / fmt("tecnix-trace-%d.sqlite", getpid()), epoch)
{
    installGcHooks();
    tecnixTraceEverEnabled.store(true, std::memory_order_relaxed);
}

TraceThreadBuffer & TraceSession::bufferForThisThread()
{
    std::lock_guard lock(mutex);
    auto & buffer = buffers[myEvalThreadId];
    if (!buffer) {
        if (buffers.size() > maxThreads) {
            buffers.erase(myEvalThreadId);
            throw Error("Tecnix tracing supports at most %d evaluation threads", maxThreads);
        }
        buffer = std::make_unique<TraceThreadBuffer>(*this, static_cast<uint32_t>(buffers.size() - 1));
    }
    return *buffer;
}

uint32_t TraceSession::registerRoot(uint64_t record, std::string_view name)
{
    std::lock_guard lock(mutex);
    auto id = nextRoot++;
    if (id >= (1u << 24))
        throw Error("Tecnix tracing supports at most %d roots", (1u << 24) - 1);
    roots.push_back({.id = id, .record = record, .name = std::string(name)});
    return id;
}

uint32_t TraceSession::internImportKey(std::string_view path)
{
    std::lock_guard lock(mutex);
    if (auto it = importKeyIds.find(std::string(path)); it != importKeyIds.end())
        return it->second;
    auto id = static_cast<uint32_t>(importKeys.size());
    importKeys.emplace_back(path);
    importKeyIds.emplace(std::string(path), id);
    return id;
}

void TraceSession::addPhase(uint32_t call, std::string name, int64_t start, int64_t end)
{
    std::lock_guard lock(mutex);
    phases.push_back({.call = call, .name = std::move(name), .start = start, .end = end});
}

std::filesystem::path TraceSession::filePath() const
{
    return writer.path;
}

TraceSession * tecnixTraceSession(EvalState & state)
{
    return state.tecnixEvalData().traceSession.get();
}

TraceSession * ensureTecnixTraceSession(EvalState & state)
{
    auto & data = state.tecnixEvalData();
    if (!data.traceSession) {
        std::string dir = state.settings.tecnixTrace;
        if (dir.empty())
            return nullptr;
        data.traceSession = std::make_unique<TraceSession>(state, std::filesystem::path(dir));
    }
    return data.traceSession.get();
}

TraceSnapshot tecnixTraceSnapshot(EvalState & state)
{
    TraceSnapshot snapshot;
    auto * session = tecnixTraceSession(state);
    if (!session)
        return snapshot;
    std::lock_guard lock(session->mutex);
    for (auto & [_, buffer] : session->buffers) {
        snapshot.records.insert(snapshot.records.end(), buffer->current.begin(), buffer->current.end());
        snapshot.reuses.insert(snapshot.reuses.end(), buffer->reuses.begin(), buffer->reuses.end());
    }
    for (auto & root : session->roots)
        snapshot.roots.emplace_back(root.id, root.name);
    snapshot.importKeys = session->importKeys;
    return snapshot;
}

void tecnixTraceDump(EvalState & state)
{
    if (auto * session = tecnixTraceSession(state))
        session->dump();
}

/* --- Roots -------------------------------------------------------------- */

TraceRoot::TraceRoot(TraceSession & session, std::string_view name)
    : session(session)
    , buffer(session.bufferForThisThread())
{
    /* The context owning this root has not been activated yet, so the
       thread's current context is the one this root nests inside, if any
       (a resolver that itself calls `tecnixTargets`). */
    auto * enclosing = currentTecnixThreadState.trackingContext;
    parentRoot = enclosing ? enclosing->trace.get() : nullptr;
    parentFrame = parentRoot ? parentRoot->current : nullptr;
    parentRecord = parentFrame ? parentFrame->id : parentRoot ? parentRoot->record : 0;

    record = buffer.nextId();
    root = session.registerRoot(record, name);

    gc0 = gcPauseTotal.load(std::memory_order_relaxed);
    a0 = currentTecnixThreadState.bytesAllocated;
    t0 = traceNow();
}

TraceRoot::~TraceRoot()
{
    auto now = traceNow();
    auto gc = gcPauseTotal.load(std::memory_order_relaxed);
    auto alloc = static_cast<int64_t>(currentTecnixThreadState.bytesAllocated - a0);
    auto dur = clampNonNegative((now - t0) - (gc - gc0));

    buffer.append(
        TraceRecord{
            .id = record,
            .parent = parentRecord,
            .root = root,
            .key = 0,
            .kind = TraceRecord::Root,
            .keyKind = TraceRecord::NoKey,
            .start = t0,
            .dur = dur,
            .gcAlloc = alloc,
            .selfDur = clampNonNegative(dur - childDur),
            .selfGcAlloc = alloc - childAlloc,
        });

    if (parentFrame) {
        parentFrame->childDur += dur;
        parentFrame->childAlloc += alloc;
    } else if (parentRoot) {
        parentRoot->childDur += dur;
        parentRoot->childAlloc += alloc;
    }
}

/* --- Hot path ----------------------------------------------------------- */

/**
 * A use of a finished value by this root. Records one reuse per (root,
 * producer) when the value was produced by another root; nothing is written
 * to shared memory here.
 */
static void traceReuseCheck(TraceRoot & root, const Value & v) noexcept
{
    auto slot = tecnixValueTraceLoad(&v, std::memory_order_acquire);
    if (!slot || tecnixTraceSlotRoot(slot) == root.root)
        return;
    auto producer = tecnixTraceSlotRecord(slot);
    bool inserted = false;
    try {
        inserted = root.seen.insert(producer).second;
    } catch (...) {
        root.buffer.dropped++;
        return;
    }
    if (inserted)
        root.buffer.appendReuse({.root = root.root, .producer = producer});
}

/** Link a node under the open frame and start its clocks. */
static void openTraceNode(TraceRoot & root, TraceFrameData & t, PosIdx pos) noexcept
{
    t.parent = root.current;
    root.current = &t;
    t.id = root.buffer.nextId();
    t.aliasOf = 0;
    t.expr = nullptr;
    t.appFun = nullptr;
    t.pos = pos;
    t.importKey = 0;
    t.childDur = 0;
    t.childAlloc = 0;
    t.gc0 = gcPauseTotal.load(std::memory_order_relaxed);
    t.a0 = currentTecnixThreadState.bytesAllocated;
    t.t0 = traceNow();
}

/** Append the node's record, feed the parent's accumulators, unlink. */
static void closeTraceNode(
    TraceRoot & root, TraceFrameData & t, TraceRecord::Kind kind, uint32_t key, TraceRecord::KeyKind keyKind) noexcept
{
    auto now = traceNow();
    auto gc = gcPauseTotal.load(std::memory_order_relaxed);
    auto alloc = static_cast<int64_t>(currentTecnixThreadState.bytesAllocated - t.a0);
    auto dur = clampNonNegative((now - t.t0) - (gc - t.gc0));

    root.buffer.append(
        TraceRecord{
            .id = t.id,
            .parent = t.parent ? t.parent->id : root.record,
            .root = root.root,
            .key = key,
            .kind = kind,
            .keyKind = keyKind,
            .start = t.t0,
            .dur = dur,
            .gcAlloc = alloc,
            .selfDur = clampNonNegative(dur - t.childDur),
            .selfGcAlloc = alloc - t.childAlloc,
        });

    if (t.parent) {
        t.parent->childDur += dur;
        t.parent->childAlloc += alloc;
    } else {
        root.childDur += dur;
        root.childAlloc += alloc;
    }
    root.current = t.parent;
}

void traceFrameOpen(TraceRoot & root, TracedSourceDepsFrame & frame, const Value & v, PosIdx pos) noexcept
{
    auto & t = frame.trace;
    openTraceNode(root, t, pos);
    // Racy reads of words another thread's finish may overwrite; they are
    // only dereferenced at close if this frame published.
    t.expr = v.isThunk() ? v.thunk().expr : nullptr;
    t.appFun = v.isApp() ? v.app().left : nullptr;
    t.importKey = std::exchange(currentTecnixThreadState.pendingImportKey, 0);
}

void traceFrameClose(TraceRoot & root, TracedSourceDepsFrame & frame) noexcept
{
    auto & t = frame.trace;
    auto kind = frame.published ? TraceRecord::Value : TraceRecord::Wait;

    if (t.importKey) {
        closeTraceNode(root, t, kind, t.importKey, TraceRecord::Import);
    } else {
        /* `published` means this thread owned the thunk from before the racy
           reads at open until it finished, so `expr` is its expression and
           `appFun` the function it applied (forced by then, if the call got
           that far). A primop application keys on the force site. */
        auto pos = noPos;
        if (frame.published) {
            if (t.expr)
                pos = t.expr->getPos();
            else if (t.appFun && t.appFun->isLambda())
                pos = t.appFun->lambda().fun->getPos();
        }
        if (!pos)
            pos = t.pos;
        closeTraceNode(root, t, kind, pos.get(), pos ? TraceRecord::Pos : TraceRecord::NoKey);
    }

    /* Unpublished: the thunk was finished by another thread (we waited) --
       a use of a value some other root may have computed. */
    if (!frame.published && frame.value)
        traceReuseCheck(root, *frame.value);
}

void traceIoOpen(TraceRoot & root, TraceFrameData & data, PosIdx pos) noexcept
{
    openTraceNode(root, data, pos);
}

void traceIoClose(TraceRoot & root, TraceFrameData & data, TraceRecord::Kind kind) noexcept
{
    closeTraceNode(root, data, kind, data.pos.get(), data.pos ? TraceRecord::Pos : TraceRecord::NoKey);
}

void traceFinishedValue(TraceRoot & root, const Value & v) noexcept
{
    currentTecnixThreadState.pendingImportKey = 0; // no frame opens for a finished value
    traceReuseCheck(root, v);
}

void traceWaitedForce(EvalState & state, Value & v, PosIdx pos, TraceRoot & root)
{
    currentTecnixThreadState.pendingImportKey = 0;
    TraceFrameData wait;
    openTraceNode(root, wait, pos);
    Finally recordWait([&]() noexcept {
        closeTraceNode(root, wait, TraceRecord::Wait, pos.get(), pos ? TraceRecord::Pos : TraceRecord::NoKey);
    });
    v.force(state, pos);
    traceReuseCheck(root, v);
}

void tracePublish(TraceRoot & root, TracedSourceDepsFrame & frame) noexcept
{
    auto & t = frame.trace;
    auto slot = t.aliasOf ? t.aliasOf : tecnixTracePackSlot(t.id, root.root);
    tecnixValueTraceStore(frame.value, slot, std::memory_order_release);
}

void traceCopyIntoFrame(TraceRoot & root, TracedSourceDepsFrame & frame, const Value & src) noexcept
{
    frame.trace.aliasOf = tecnixValueTraceLoad(&src, std::memory_order_acquire);
    traceReuseCheck(root, src);
}

void traceCopy(const Value & dst, const Value & src) noexcept
{
    auto slot = tecnixValueTraceLoad(&src, std::memory_order_acquire);
    if (slot)
        tecnixValueTraceStore(&dst, slot, std::memory_order_release);
}

/**
 * How a source path is named in the trace: repository-relative for paths
 * under the Tecnix repo mount (whose accessor would otherwise render them as
 * `«unknown»/...`), the usual rendering for anything else.
 *
 * The mount path is set once, on the coordinator, before any tracked
 * evaluation can import through it; workers only run afterwards, handed
 * their work through the executor's queue.
 */
static std::string traceSourcePathName(EvalState & state, const SourcePath & path)
{
    if (auto & mount = state.tecnixEvalData().tecnixRepoMountStorePath) {
        auto prefix = state.store->printStorePath(*mount) + "/";
        auto abs = path.path.abs();
        if (abs.starts_with(prefix))
            return abs.substr(prefix.size());
    }
    return path.to_string();
}

void tecnixTraceNoteImport(TrackingContext & trackingCtx, const SourcePath & path)
{
    if (auto * trace = trackingCtx.trace.get())
        currentTecnixThreadState.pendingImportKey =
            trace->session.internImportKey(traceSourcePathName(trace->session.state, path));
}

/* --- Calls and phases --------------------------------------------------- */

TecnixTraceCall::TecnixTraceCall(EvalState & state)
    : session(ensureTecnixTraceSession(state))
{
    if (!session)
        return;
    session->callDepth.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(session->mutex);
    call = session->nextCall++;
}

TecnixTraceCall::~TecnixTraceCall()
{
    if (!session)
        return;
    if (session->callDepth.fetch_sub(1, std::memory_order_acq_rel) == 1)
        session->dump();
}

void TecnixTraceCall::noteArgs(
    std::string_view gitDir, std::string_view rev, std::string_view checkoutPath, std::string_view resolver)
{
    if (!session)
        return;
    std::lock_guard lock(session->mutex);
    if (session->gitDir.empty())
        session->gitDir = gitDir;
    if (session->rev.empty())
        session->rev = rev;
    if (session->checkoutPath.empty())
        session->checkoutPath = checkoutPath;
    if (session->resolver.empty())
        session->resolver = resolver;
}

TecnixTracePhase::TecnixTracePhase(EvalState & state, uint32_t call, const char * name)
    : session(tecnixTraceSession(state))
    , call(call)
    , name(name)
    , start(session ? traceNow() : 0)
{
}

TecnixTracePhase::~TecnixTracePhase()
{
    if (!session)
        return;
    try {
        session->addPhase(call, name, start, traceNow());
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

/* --- The file ----------------------------------------------------------- */

static const char * traceSchema = R"sql(
create table if not exists roots (
    id      integer primary key,
    record  integer not null,
    name    text not null
);
create table if not exists records (
    id            integer primary key,
    parent        integer not null,
    root          integer not null,
    expr          integer,
    kind          text not null,
    start         integer not null,
    dur           integer not null,
    gc_alloc      integer not null,
    self_dur      integer not null,
    self_gc_alloc integer not null
);
create table if not exists reuses (
    root     integer not null,
    producer integer not null
);
create table if not exists gc (
    start integer not null,
    end   integer not null
);
create table if not exists phases (
    call  integer not null,
    name  text not null,
    start integer not null,
    end   integer not null
);
create table if not exists exprs (
    id   integer primary key,
    file text,
    line integer,
    col  integer
);
create table if not exists meta (
    key   text primary key,
    value text
);
)sql";

/* Indexes are built by the dump, after the bulk of the rows is in, and
   dropped again before the next call's records stream in: maintaining them
   during the insert made it four times slower. */
static const char * traceIndexNames[] = {"records_parent", "records_root", "reuses_producer"};

static const char * traceIndexes = R"sql(
create index if not exists records_parent on records(parent);
create index if not exists records_root on records(root);
create index if not exists reuses_producer on reuses(producer);
)sql";

/* The views answer the README's questions without tooling, and are linear
   in the trace: every shared record is assigned to a *chunk* -- its nearest
   ancestor that some other root used directly -- by one walk down from the
   directly reused records, and everything is aggregated per chunk. A chunk's
   consumers are the roots that used its producer plus the root that computed
   it. `target_costs` is the per-target answer: own work, the shared work the
   target needs, that work amortised over each chunk's consumers (so the
   amortised totals add up to the run), and the fetch and store I/O it paid
   -- both as paid and amortised the same way, so I/O inside a shared chunk
   is split among the chunk's consumers too. Fetch, store and wait records
   never count as evaluation. */
static const char * traceViews = R"sql(
create view if not exists producers as
select distinct producer as id from reuses;

create view if not exists chunk as
with recursive w(chunk, id) as (
    select id, id from producers
    union all
    select w.chunk, r.id from records r join w on r.parent = w.id
    where r.kind <> 'root' and r.id not in (select id from producers)
)
select chunk, id from w;

create view if not exists shared as
select id from chunk;

create view if not exists chunk_weight as
select c.chunk, sum(r.self_dur) as self_dur, sum(r.self_gc_alloc) as self_gc_alloc, count(*) as records
from chunk c join records r on r.id = c.id
where r.kind = 'value'
group by c.chunk;

create view if not exists chunk_io as
select c.chunk,
       sum(case when r.kind = 'fetch' then r.self_dur else 0 end) as fetch_dur,
       sum(case when r.kind = 'store' then r.self_dur else 0 end) as store_dur
from chunk c join records r on r.id = c.id
where r.kind in ('fetch', 'store')
group by c.chunk;

create view if not exists chunk_consumer as
select producer as chunk, root from reuses
union
select r.id, r.root from records r where r.id in (select id from producers);

create view if not exists chunk_consumers as
select chunk, count(*) as consumers from chunk_consumer group by chunk;

create view if not exists floor as
with agg as (
    select p.expr, sum(w.self_dur) as self_dur, sum(w.self_gc_alloc) as self_gc_alloc, count(*) as chunks
    from chunk_weight w join records p on p.id = w.chunk
    group by p.expr
),
cons as (
    select p.expr, count(distinct c.root) as consumers
    from chunk_consumer c join records p on p.id = c.chunk
    group by p.expr
)
select e.file, e.line, e.col, agg.self_dur, agg.self_gc_alloc, agg.chunks, cons.consumers
from agg
left join exprs e on e.id = agg.expr
join cons on cons.expr is agg.expr
order by agg.self_dur desc;

create view if not exists own as
select ro.name as root, sum(r.self_dur) as own_dur, sum(r.self_gc_alloc) as own_gc_alloc
from records r join roots ro on ro.id = r.root
where r.kind in ('value', 'root') and r.id not in (select id from shared)
group by ro.id
order by own_dur desc;

create view if not exists consumers as
select p.id as producer, e.file, e.line, e.col, p.dur, p.self_dur, w.self_dur as chunk_self_dur,
       (select group_concat(name, ',') from
           (select ro.name from chunk_consumer c join roots ro on ro.id = c.root where c.chunk = p.id order by ro.name))
           as roots
from producers d
join records p on p.id = d.id
left join exprs e on e.id = p.expr
join chunk_weight w on w.chunk = p.id
order by p.dur desc;

create view if not exists target_costs as
with own_by_root as (
    select r.root, sum(r.self_dur) as own_dur, sum(r.self_gc_alloc) as own_gc_alloc
    from records r
    where r.kind in ('value', 'root') and r.id not in (select id from shared)
    group by r.root
),
shared_by_root as (
    select c.root,
           sum(w.self_dur) as shared_dur,
           sum(w.self_dur * 1.0 / cc.consumers) as shared_dur_amortized,
           sum(w.self_gc_alloc) as shared_gc_alloc,
           sum(w.self_gc_alloc * 1.0 / cc.consumers) as shared_gc_alloc_amortized
    from chunk_consumer c
    join chunk_weight w on w.chunk = c.chunk
    join chunk_consumers cc on cc.chunk = c.chunk
    group by c.root
),
io_by_root as (
    select r.root,
           sum(case when r.kind = 'fetch' then r.self_dur else 0 end) as fetch_dur,
           sum(case when r.kind = 'store' then r.self_dur else 0 end) as store_dur,
           sum(case when r.kind = 'fetch' and r.id not in (select id from shared) then r.self_dur else 0 end) as own_fetch_dur,
           sum(case when r.kind = 'store' and r.id not in (select id from shared) then r.self_dur else 0 end) as own_store_dur
    from records r
    where r.kind in ('fetch', 'store')
    group by r.root
),
shared_io_by_root as (
    select c.root,
           sum(io.fetch_dur * 1.0 / cc.consumers) as fetch_dur_amortized,
           sum(io.store_dur * 1.0 / cc.consumers) as store_dur_amortized
    from chunk_consumer c
    join chunk_io io on io.chunk = c.chunk
    join chunk_consumers cc on cc.chunk = c.chunk
    group by c.root
)
select ro.id as root, ro.name,
       coalesce(o.own_dur, 0) as own_dur,
       coalesce(s.shared_dur, 0) as shared_dur,
       cast(round(coalesce(s.shared_dur_amortized, 0)) as integer) as shared_dur_amortized,
       coalesce(o.own_dur, 0) + cast(round(coalesce(s.shared_dur_amortized, 0)) as integer) as total_dur_amortized,
       coalesce(io.fetch_dur, 0) as fetch_dur,
       coalesce(io.store_dur, 0) as store_dur,
       coalesce(io.own_fetch_dur, 0) + cast(round(coalesce(sio.fetch_dur_amortized, 0)) as integer) as fetch_dur_amortized,
       coalesce(io.own_store_dur, 0) + cast(round(coalesce(sio.store_dur_amortized, 0)) as integer) as store_dur_amortized,
       coalesce(o.own_gc_alloc, 0) as own_gc_alloc,
       coalesce(s.shared_gc_alloc, 0) as shared_gc_alloc,
       cast(round(coalesce(s.shared_gc_alloc_amortized, 0)) as integer) as shared_gc_alloc_amortized
from roots ro
left join own_by_root o on o.root = ro.id
left join shared_by_root s on s.root = ro.id
left join io_by_root io on io.root = ro.id
left join shared_io_by_root sio on sio.root = ro.id
order by total_dur_amortized desc;
)sql";

static const char * traceKindName(TraceRecord::Kind kind)
{
    switch (kind) {
    case TraceRecord::Value:
        return "value";
    case TraceRecord::Root:
        return "root";
    case TraceRecord::Wait:
        return "wait";
    case TraceRecord::Fetch:
        return "fetch";
    case TraceRecord::Store:
        return "store";
    }
    return "?";
}

/** Stable `exprs.id` for a record key, or 0 for none. */
static uint64_t traceExprId(const TraceRecord & r)
{
    switch (r.keyKind) {
    case TraceRecord::NoKey:
        return 0;
    case TraceRecord::Pos:
        return r.key;
    case TraceRecord::Import:
        return (uint64_t{1} << 32) | r.key;
    }
    return 0;
}

/* --- Writer ------------------------------------------------------------- */

TraceWriter::TraceWriter(std::filesystem::path path, int64_t epoch)
    : path(std::move(path))
    , epoch(epoch)
    , thread([this] { run(); })
{
}

TraceWriter::~TraceWriter()
{
    {
        std::lock_guard lock(mutex);
        stop = true;
    }
    cv.notify_all();
    if (thread.joinable())
        thread.join();
}

void TraceWriter::enqueue(std::vector<TraceRecord> chunk)
{
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return queue.size() < maxQueuedChunks || stop; });
    queue.push_back(std::move(chunk));
    lock.unlock();
    cv.notify_all();
}

void TraceWriter::drain()
{
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return (queue.empty() && !writing) || stop; });
}

void TraceWriter::ensureOpen()
{
    if (db)
        return;
    createDirs(path.parent_path());
    db.emplace(path, SQLite::Settings{.useWAL = true});
    db->exec("pragma synchronous = off");
    db->exec("pragma journal_mode = off");
    db->exec("pragma cache_size = -524288"); // 512 MiB of B-tree pages during the bulk insert and index builds
    db->exec(traceSchema);
    insertRecord.emplace(
        *db,
        "insert into records (id, parent, root, expr, kind, start, dur, gc_alloc, self_dur, self_gc_alloc) "
        "values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    // A file from an earlier dump (same pid, e.g. a re-opened session) may already be indexed.
    SQLiteStmt hasIndex(*db, "select count(*) from sqlite_master where type = 'index' and name = ?");
    auto use = hasIndex.use().apply(traceIndexNames[0]);
    indexed = use.next() && use.getInt(0) > 0;
}

void TraceWriter::writeChunk(const std::vector<TraceRecord> & chunk)
{
    ensureOpen();
    if (indexed) {
        for (auto * name : traceIndexNames)
            db->exec(fmt("drop index if exists %s", name));
        indexed = false;
    }
    db->exec("begin");
    for (auto & r : chunk) {
        auto exprId = traceExprId(r);
        if (exprId)
            exprKeys.insert(exprId);
        auto use = insertRecord->use();
        use.apply(static_cast<int64_t>(r.id)).apply(static_cast<int64_t>(r.parent)).apply(r.root);
        if (exprId)
            use.apply(static_cast<int64_t>(exprId));
        else
            use.bind();
        use.apply(traceKindName(r.kind))
            .apply(r.start - epoch)
            .apply(r.dur)
            .apply(r.gcAlloc)
            .apply(r.selfDur)
            .apply(r.selfGcAlloc)
            .exec();
    }
    db->exec("commit");
    rowsWritten += chunk.size();
}

void TraceWriter::run()
{
    while (true) {
        std::vector<TraceRecord> chunk;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return stop || !queue.empty(); });
            if (queue.empty())
                return; // stop, and nothing left
            chunk = std::move(queue.front());
            queue.pop_front();
            writing = true;
        }
        cv.notify_all(); // an appender blocked on a full queue may continue
        try {
            writeChunk(chunk);
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!error)
                error = std::current_exception();
        }
        {
            std::lock_guard lock(mutex);
            writing = false;
        }
        cv.notify_all();
    }
}

/* --- Dump --------------------------------------------------------------- */

struct TraceExprRow
{
    std::string file;
    std::optional<uint32_t> line, col;
};

/** Resolve a position or import key to a row of `exprs`. */
static TraceExprRow resolveTraceExpr(EvalState & state, const TraceSession & session, uint64_t exprId)
{
    if (exprId >> 32) {
        auto index = static_cast<uint32_t>(exprId);
        return {.file = index < session.importKeys.size() ? session.importKeys[index] : ""};
    }

    auto pos = state.positions[PosIdx(static_cast<uint32_t>(exprId))];
    TraceExprRow row;
    if (auto * path = std::get_if<SourcePath>(&pos.origin))
        row.file = traceSourcePathName(state, *path);
    else if (std::get_if<Pos::Stdin>(&pos.origin))
        row.file = "«stdin»";
    else if (std::get_if<Pos::String>(&pos.origin))
        row.file = "«string»";
    if (pos) {
        row.line = pos.line;
        row.col = pos.column;
    }
    return row;
}

void TraceSession::dump() noexcept
{
    auto dumpStart = traceNow();
    std::lock_guard lock(mutex);

    /* Everything still buffered goes through the writer too, so the records
       table is complete once it has drained. */
    try {
        for (auto & [_, buffer] : buffers) {
            if (buffer->current.empty())
                continue;
            std::vector<TraceRecord> rest = std::move(buffer->current);
            buffer->current = std::vector<TraceRecord>();
            writer.enqueue(std::move(rest));
        }
        writer.drain();
    } catch (...) {
        ignoreExceptionInDestructor();
    }

    uint64_t records = 0;
    {
        std::lock_guard writerLock(writer.mutex);
        records = writer.rowsWritten - rowsAtLastDump;
        rowsAtLastDump = writer.rowsWritten;
    }
    bool anything = records || !roots.empty() || !phases.empty();
    for (auto & [_, buffer] : buffers)
        anything = anything || !buffer->reuses.empty();
    if (!anything)
        return;

    try {
        writer.ensureOpen();
        auto & db = *writer.db;

        SQLiteStmt insertRoot(db, "insert into roots (id, record, name) values (?, ?, ?)");
        SQLiteStmt insertReuse(db, "insert into reuses (root, producer) values (?, ?)");
        SQLiteStmt insertGc(db, "insert into gc (start, end) values (?, ?)");
        SQLiteStmt insertPhase(db, "insert into phases (call, name, start, end) values (?, ?, ?, ?)");
        SQLiteStmt insertExpr(db, "insert or ignore into exprs (id, file, line, col) values (?, ?, ?, ?)");
        SQLiteStmt insertMeta(db, "insert or replace into meta (key, value) values (?, ?)");

        db.exec("begin");

        for (auto & root : roots)
            insertRoot.use().apply(root.id).apply(static_cast<int64_t>(root.record)).apply(root.name).exec();

        uint64_t reuses = 0, dropped = 0;
        for (auto & [_, buffer] : buffers) {
            for (auto & u : buffer->reuses) {
                insertReuse.use().apply(u.root).apply(static_cast<int64_t>(u.producer)).exec();
                reuses++;
            }
            dropped += buffer->dropped;
        }

        auto gcCount = gcPauseCount.load(std::memory_order_acquire);
        for (size_t i = dumpedGcPauses; i < gcCount; i++) {
            if (gcPauses[i].end < epoch)
                continue;
            insertGc.use().apply(gcPauses[i].start - epoch).apply(gcPauses[i].end - epoch).exec();
        }
        dumpedGcPauses = gcCount;

        for (auto & phase : phases)
            insertPhase.use()
                .apply(phase.call)
                .apply(phase.name)
                .apply(phase.start - epoch)
                .apply(phase.end - epoch)
                .exec();

        for (auto exprId : writer.exprKeys) {
            if (dumpedExprs.contains(exprId))
                continue;
            auto row = resolveTraceExpr(state, *this, exprId);
            auto use = insertExpr.use();
            use.apply(static_cast<int64_t>(exprId)).apply(row.file);
            if (row.line)
                use.apply(static_cast<int64_t>(*row.line));
            else
                use.bind();
            if (row.col)
                use.apply(static_cast<int64_t>(*row.col));
            else
                use.bind();
            use.exec();
            dumpedExprs.insert(exprId);
        }
        writer.exprKeys.clear();

        auto meta = [&](std::string_view key, std::string value) { insertMeta.use().apply(key).apply(value).exec(); };
        meta("format_version", "2");
        meta("pid", std::to_string(getpid()));
        meta("eval_cores", std::to_string(state.executor->evalCores));
        meta("git_dir", gitDir);
        meta("rev", rev);
        meta("checkout_path", checkoutPath);
        meta("resolver", resolver);
        meta("dumps", std::to_string(dumps + 1));
        meta("records_dropped", std::to_string(dropped));
        db.exec("commit");

        // The indexes are most of the finishing time; the dump phase covers them.
        db.exec(traceIndexes);
        writer.indexed = true;
        db.exec(traceViews);
        auto now = traceNow();
        meta("wall_time_ns", std::to_string(now - epoch));
        insertPhase.use()
            .apply(static_cast<int64_t>(nextCall - 1))
            .apply("dump")
            .apply(dumpStart - epoch)
            .apply(now - epoch)
            .exec();
        dumps++;

        printTalkative(
            "tecnix trace: wrote %d record(s), %d reuse(s), %d root(s) to %s; finishing the file took %d ms",
            records,
            reuses,
            roots.size(),
            writer.path.string(),
            (traceNow() - dumpStart) / 1'000'000);
        if (dropped)
            warn("tecnix trace: %d record(s) were dropped because memory could not be allocated", dropped);
        std::exception_ptr error;
        {
            std::lock_guard writerLock(writer.mutex);
            error = std::exchange(writer.error, nullptr);
        }
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (std::exception & e) {
                warn("tecnix trace: writing records failed: %s", e.what());
            }
        }
    } catch (...) {
        ignoreExceptionInDestructor();
    }

    /* Dumped or not, the rows are gone: ids stay unique, so a later dump
       appends cleanly, and a failed dump is not retried. */
    for (auto & [_, buffer] : buffers) {
        buffer->reuses.clear();
        buffer->reuses.shrink_to_fit();
        buffer->dropped = 0;
    }
    roots.clear();
    phases.clear();
}

} // namespace nix
