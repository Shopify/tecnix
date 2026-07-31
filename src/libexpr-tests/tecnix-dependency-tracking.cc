#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/tecnix/access-set-graph.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/serialise.hh"

namespace nix {

class ScopedEnv
{
    std::string name;
    std::optional<std::string> oldValue;

public:
    ScopedEnv(std::string name, std::string value)
        : name(std::move(name))
    {
        if (auto * existing = std::getenv(this->name.c_str()))
            oldValue = std::string(existing);
        setenv(this->name.c_str(), value.c_str(), 1);
    }

    ~ScopedEnv()
    {
        if (oldValue)
            setenv(name.c_str(), oldValue->c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

static bool containsPath(const std::vector<std::string> & paths, std::string_view expected)
{
    return std::find(paths.begin(), paths.end(), expected) != paths.end();
}

static std::vector<std::string>
flattenFrame(const ref<EvalSourceAccessSetGraph> & graph, const TrackedSourceDepsFrame & frame)
{
    std::vector<EvalSourceAccessId> direct(
        frame.directSourceAccessSetAccesses.begin(), frame.directSourceAccessSetAccesses.end());
    std::vector<EvalSourceAccessSetId> children(frame.childSourceAccessSets.begin(), frame.childSourceAccessSets.end());
    return graph->flatten(direct, children);
}

class TrackingMemorySourceAccessor : public SourceAccessor
{
    std::map<std::string, std::string> files;

    static std::string key(const CanonPath & path)
    {
        return path.isRoot() ? std::string{} : std::string(path.rel());
    }

    void track(const CanonPath & path) const
    {
        if (path.isRoot())
            return;
        if (auto * ctx = currentTecnixThreadState.trackingContext)
            ctx->recordAccess(key(path));
    }

public:
    explicit TrackingMemorySourceAccessor(std::map<std::string, std::string> files)
        : files(std::move(files))
    {
        setPathDisplay("memory:");
    }

    bool tracksEvalAccesses(const CanonPath &) override
    {
        return true;
    }

    void recordEvalAccess(const CanonPath & path) override
    {
        track(path);
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        track(path);
        auto it = files.find(key(path));
        if (it == files.end())
            throw FileNotFound("path '%s' does not exist", path.abs());
        sizeCallback(it->second.size());
        sink(it->second);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        if (path.isRoot())
            return Stat{.type = tDirectory};

        auto pathKey = key(path);
        if (auto it = files.find(pathKey); it != files.end())
            return Stat{.type = tRegular, .fileSize = it->second.size()};

        auto dirPrefix = pathKey + "/";
        for (auto & [file, _] : files)
            if (file.starts_with(dirPrefix))
                return Stat{.type = tDirectory};

        return std::nullopt;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        track(path);
        if (!maybeLstat(path) || maybeLstat(path)->type != tDirectory)
            throw FileNotFound("path '%s' does not exist", path.abs());

        DirEntries entries;
        auto dirKey = key(path);
        auto prefix = dirKey.empty() ? std::string{} : dirKey + "/";
        for (auto & [file, _] : files) {
            if (!file.starts_with(prefix))
                continue;
            auto rest = file.substr(prefix.size());
            auto slash = rest.find('/');
            auto name = slash == std::string::npos ? rest : rest.substr(0, slash);
            entries.emplace(name, slash == std::string::npos ? DirEntry{tRegular} : DirEntry{tDirectory});
        }
        return entries;
    }

    std::string readLink(const CanonPath & path) override
    {
        track(path);
        throw NotASymlink("path '%s' is not a symlink", path.abs());
    }
};

class ScopedTrackingContext
{
    ActiveTrackingContext active;

public:
    explicit ScopedTrackingContext(TrackingContext & context)
        : active(context)
    {
    }
};

class ScopedTrackedValueForceFrame
{
    TrackedSourceDepsFrame * oldFrame;
    const void * oldPublishValue;

public:
    explicit ScopedTrackedValueForceFrame(TrackedSourceDepsFrame & frame)
        : oldFrame(currentTecnixThreadState.sourceDepsFrame)
        , oldPublishValue(currentTecnixThreadState.valueDependencyPublishValue)
    {
        currentTecnixThreadState.sourceDepsFrame = &frame;
        currentTecnixThreadState.valueDependencyPublishValue = frame.value;
    }

    ~ScopedTrackedValueForceFrame()
    {
        currentTecnixThreadState.sourceDepsFrame = oldFrame;
        currentTecnixThreadState.valueDependencyPublishValue = oldPublishValue;
    }
};

class TecnixValueProvenanceTest : public LibExprTest
{
protected:
    EvalSourceAccessSetId publishSingleAccess(Value & value, std::string_view path)
    {
        enableSourceAccessSetTracking(state);
        auto access = trackedSourceAccessSetGraph(state)->internAccess(std::string(path));
        std::array<EvalSourceAccessId, 1> direct{access};
        return publishTrackedSourceAccessSetDependencies(
            *trackedSourceAccessSetGraph(state),
            value,
            std::span<const EvalSourceAccessId>(direct),
            std::span<const EvalSourceAccessSetId>());
    }
};

TEST(TecnixSourceAccessSetGraph, disabledGraphReturnsEmptyIdsAndNoFlattenedAccesses)
{
    EvalSourceAccessSetGraph graph;

    auto access = graph.internAccess("a.nix");
    EXPECT_EQ(access, emptyEvalSourceAccessId);

    std::vector<EvalSourceAccessId> direct{access};
    std::vector<EvalSourceAccessSetId> children;
    auto set = graph.internAccessSet(direct, children);
    EXPECT_EQ(set, emptyEvalSourceAccessSetId);
    EXPECT_TRUE(graph.flatten(direct, {set}).empty());
}

TEST(TecnixSourceAccessSetGraph, accessIdsAreInternedByPath)
{
    EvalSourceAccessSetGraph graph;
    graph.enable();

    auto read = graph.internAccess("a.nix");
    auto readAgain = graph.internAccess("a.nix");
    auto other = graph.internAccess("b.nix");

    EXPECT_NE(read, emptyEvalSourceAccessId);
    EXPECT_EQ(read, readAgain);
    EXPECT_NE(read, other);

    EXPECT_EQ(graph.access(read), "a.nix");
}

TEST(TecnixSourceAccessSetGraph, internedSetsAreCanonicalAndFlattenTransitiveClosures)
{
    EvalSourceAccessSetGraph graph;
    graph.enable();

    auto a = graph.internAccess("a.nix");
    auto b = graph.internAccess("b.nix");
    auto c = graph.internAccess("c.nix");

    auto setAB = graph.internAccessSet(std::vector<EvalSourceAccessId>{b, a, a}, {});
    auto setBA = graph.internAccessSet(std::vector<EvalSourceAccessId>{a, b}, {});
    EXPECT_EQ(setAB, setBA);

    auto setC = graph.internAccessSet(std::vector<EvalSourceAccessId>{c}, {});
    auto setABC =
        graph.internAccessSet(std::vector<EvalSourceAccessId>{a}, std::vector<EvalSourceAccessSetId>{setAB, setC});
    auto setABCAgain = graph.internAccessSet(std::vector<EvalSourceAccessId>{c, b, a}, {});
    EXPECT_EQ(setABC, setABCAgain);

    auto flattened = graph.flatten({}, {setABC});
    EXPECT_EQ(flattened, (std::vector<std::string>{"a.nix", "b.nix", "c.nix"}));

    auto directPlusEdge = graph.flatten(std::vector<EvalSourceAccessId>{c, a}, {setAB});
    EXPECT_EQ(directPlusEdge, (std::vector<std::string>{"c.nix", "a.nix", "b.nix"}));
}

TEST_F(TecnixValueProvenanceTest, overwritingValueClearsStaleAccessSetMapping)
{
    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);

    Value value;
    value.mkInt(1);
    auto originalSet = publishSingleAccess(value, "old-source.nix");
    ASSERT_NE(originalSet, emptyEvalSourceAccessSetId);
    ASSERT_EQ(value.trackedSourceAccessSet(), originalSet);

    Value frameValue;
    frameValue.mkInt(0);
    TrackedSourceDepsFrame frame(trackingCtx, &frameValue, currentTecnixThreadState.sourceDepsFrame);
    ScopedTrackedValueForceFrame scopedFrame(frame);

    value.mkInt(2);

    EXPECT_EQ(value.trackedSourceAccessSet(), emptyEvalSourceAccessSetId);
}

TEST_F(TecnixValueProvenanceTest, copyingFinishedValuePublishesAccessSetOnDestination)
{
    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);

    Value source;
    source.mkInt(1);
    auto sourceSet = publishSingleAccess(source, "copied-source.nix");
    ASSERT_NE(sourceSet, emptyEvalSourceAccessSetId);

    Value destination;
    Value frameValue;
    frameValue.mkInt(0);
    TrackedSourceDepsFrame frame(trackingCtx, &frameValue, currentTecnixThreadState.sourceDepsFrame);
    ScopedTrackedValueForceFrame scopedFrame(frame);

    destination = source;

    auto copiedSet = destination.trackedSourceAccessSet();
    ASSERT_NE(copiedSet, emptyEvalSourceAccessSetId);
    EXPECT_EQ(
        trackedSourceAccessSetGraph(state)->flatten({}, {copiedSet}), (std::vector<std::string>{"copied-source.nix"}));
}

TEST_F(TecnixValueProvenanceTest, copyingIntoCurrentForceFramePublishesCopiedAccessSet)
{
    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);

    Value source;
    source.mkInt(1);
    auto sourceSet = publishSingleAccess(source, "current-frame-copy-source.nix");
    ASSERT_NE(sourceSet, emptyEvalSourceAccessSetId);

    Value destination;
    TrackedSourceDepsFrame frame(trackingCtx, &destination, currentTecnixThreadState.sourceDepsFrame);
    ScopedTrackedValueForceFrame scopedFrame(frame);

    destination = source;

    auto copiedSet = destination.trackedSourceAccessSet();
    ASSERT_NE(copiedSet, emptyEvalSourceAccessSetId);
    EXPECT_EQ(
        trackedSourceAccessSetGraph(state)->flatten({}, {copiedSet}),
        (std::vector<std::string>{"current-frame-copy-source.nix"}));
}

TEST_F(TecnixValueProvenanceTest, tryEvalCaughtThrowRecordsSourceAccessesThatInfluenceResult)
{
    enableSourceAccessSetTracking(state);
    auto accessor = make_ref<TrackingMemorySourceAccessor>(std::map<std::string, std::string>{
        {"main.nix", "builtins.tryEval (import ./throws.nix)"},
        {"throws.nix", "throw (builtins.readFile ./dep.txt)"},
        {"dep.txt", "boom"},
    });

    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);

    Value result;
    state.evalFile(SourcePath(accessor, CanonPath("/main.nix")), result, false);
    state.forceAttrs(result, noPos, "while testing caught tryEval dependency tracking");

    auto dependencies = flattenFrame(trackedSourceAccessSetGraph(state), trackingCtx.rootFrame);

    EXPECT_TRUE(containsPath(dependencies, "main.nix"));
    EXPECT_TRUE(containsPath(dependencies, "throws.nix"));
    EXPECT_TRUE(containsPath(dependencies, "dep.txt"));

    auto resultSet = result.trackedSourceAccessSet();
    ASSERT_NE(resultSet, emptyEvalSourceAccessSetId);
    auto resultDependencies = trackedSourceAccessSetGraph(state)->flatten({}, {resultSet});
    EXPECT_TRUE(containsPath(resultDependencies, "main.nix"));
    EXPECT_TRUE(containsPath(resultDependencies, "throws.nix"));
    EXPECT_TRUE(containsPath(resultDependencies, "dep.txt"));
}

TEST_F(TecnixValueProvenanceTest, tryEvalShallowSuccessDoesNotRecordUnforcedThrowingAttribute)
{
    enableSourceAccessSetTracking(state);
    auto accessor = make_ref<TrackingMemorySourceAccessor>(std::map<std::string, std::string>{
        {"main.nix", "builtins.tryEval { x = import ./throws.nix; }"},
        {"throws.nix", "throw (builtins.readFile ./dep.txt)"},
        {"dep.txt", "boom"},
    });

    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);

    Value result;
    state.evalFile(SourcePath(accessor, CanonPath("/main.nix")), result, false);
    state.forceAttrs(result, noPos, "while testing shallow tryEval dependency tracking");

    auto dependencies = flattenFrame(trackedSourceAccessSetGraph(state), trackingCtx.rootFrame);

    EXPECT_TRUE(containsPath(dependencies, "main.nix"));
    EXPECT_FALSE(containsPath(dependencies, "throws.nix"));
    EXPECT_FALSE(containsPath(dependencies, "dep.txt"));

    auto resultSet = result.trackedSourceAccessSet();
    ASSERT_NE(resultSet, emptyEvalSourceAccessSetId);
    auto resultDependencies = trackedSourceAccessSetGraph(state)->flatten({}, {resultSet});
    EXPECT_TRUE(containsPath(resultDependencies, "main.nix"));
    EXPECT_FALSE(containsPath(resultDependencies, "throws.nix"));
    EXPECT_FALSE(containsPath(resultDependencies, "dep.txt"));
}

TEST_F(TecnixValueProvenanceTest, fileCacheResetPreservesGraphLabels)
{
    // Value labels are graph-local IDs and values survive a file-cache reset
    // (e.g. a repl reload). The graph must never be cleared while an EvalState
    // is alive, or surviving labels would resolve to re-minted IDs and hence
    // the wrong paths.
    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);

    Value value;
    value.mkInt(1);
    auto set = publishSingleAccess(value, "kept-source.nix");
    ASSERT_NE(set, emptyEvalSourceAccessSetId);

    state.resetFileCache();

    auto label = value.trackedSourceAccessSet();
    ASSERT_EQ(label, set);
    auto resolved = trackedSourceAccessSetGraph(state)->flatten({}, {label});
    EXPECT_TRUE(containsPath(resolved, "kept-source.nix"));
}

TEST_F(TecnixValueProvenanceTest, spawningParallelWorkUnderTrackingThrows)
{
    // Tracking contexts are thread-confined and work items capture only owned
    // state. Spawning parallel evaluation work under an active tracking
    // context must fail loudly instead of capturing a dangling context.
    Executor::WorkItems work;
    state.addWork(work, 0, []() {});
    EXPECT_EQ(work.size(), 1u);

    TrackingContext trackingCtx(state);
    ScopedTrackingContext scopedContext(trackingCtx);
    EXPECT_THROW(state.addWork(work, 0, []() {}), Error);
}

} // namespace nix
