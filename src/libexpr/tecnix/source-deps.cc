#include "nix/expr/eval.hh"
#include "nix/expr/tecnix/access-set-graph.hh"
#include "tecnix/eval-data.hh"
#include "nix/util/strings-inline.hh"
#include "nix/util/util.hh"

#include <algorithm>
#include <span>

#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif

namespace nix {

[[gnu::tls_model("initial-exec")]] thread_local TecnixThreadState currentTecnixThreadState;

/**
 * The value-label directory: constant-initialized zeroed storage, so it is
 * usable from the very first dynamic initializer without ordering concerns.
 * Chunks are 1 GiB sparse mappings covering 4 GiB of address space each
 * (one 32-bit slot per 16-byte-aligned value cell), installed on the first
 * nonzero label store in their region and never freed.
 */
std::atomic<uint32_t *> tecnixValueLabelDir[tecnixValueLabelDirSize];

uint32_t * tecnixInstallValueLabelChunk(size_t dirIndex)
{
    static_assert(sizeof(void *) == 8, "the Tecnix value-label table requires a 64-bit address space");

    size_t chunkBytes = (size_t{1} << 32) / 16 * sizeof(uint32_t);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void * mem = mmap(nullptr, chunkBytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "nix: failed to map a Tecnix value-label chunk\n");
        abort();
    }

    auto * chunk = static_cast<uint32_t *>(mem);
    uint32_t * expected = nullptr;
    if (!tecnixValueLabelDir[dirIndex].compare_exchange_strong(
            expected, chunk, std::memory_order_release, std::memory_order_acquire)) {
        munmap(mem, chunkBytes);
        return expected;
    }
    return chunk;
}

void tecnixValueLabelOutOfRange(const void * value)
{
    fprintf(stderr, "nix: Tecnix value label store outside the covered address range: %p\n", value);
    abort();
}

std::vector<std::string> parseGitPorcelainZDirtyPaths(std::string_view output)
{
    std::vector<std::string> paths;
    size_t pos = 0;
    while (pos < output.size()) {
        auto nulPos = output.find('\0', pos);
        if (nulPos == std::string_view::npos)
            break;

        auto entry = output.substr(pos, nulPos - pos);
        pos = nulPos + 1;

        // Git porcelain v1 -z format is "XY PATH\0", with an extra
        // original-path record only when the X column is R/C. Keep both names
        // dirty so source reads of either side see the checkout overlay.
        if (entry.size() < 4 || entry[2] != ' ')
            continue;

        paths.emplace_back(entry.substr(3));

        if (entry[0] == 'R' || entry[0] == 'C') {
            auto nextNul = output.find('\0', pos);
            if (nextNul == std::string_view::npos)
                break;

            auto originalPath = output.substr(pos, nextNul - pos);
            pos = nextNul + 1;
            if (!originalPath.empty())
                paths.emplace_back(originalPath);
        }
    }
    return paths;
}

static uint64_t hashSourceAccessIds(std::span<const EvalSourceAccessId> items)
{
    uint64_t hash = 1469598103934665603ULL;
    for (auto item : items) {
        hash ^= item;
        hash *= 1099511628211ULL;
    }
    return hash;
}

EvalSourceAccessSetGraph::EvalSourceAccessSetGraph() = default;

void EvalSourceAccessSetGraph::enable()
{
    if (enabled.load(std::memory_order_acquire))
        return; // enable is one-way; contexts re-enter here on every construction

    std::lock_guard lock(mutex);
    if (enabled.load(std::memory_order_acquire))
        return;

    accesses.emplace_back();
    accessSets.push_back(EvalSourceAccessSetNode{});
    enabled.store(true, std::memory_order_release);
}

EvalSourceAccessId EvalSourceAccessSetGraph::internAccess(std::string_view path)
{
    if (!enabled.load(std::memory_order_acquire))
        return emptyEvalSourceAccessId;

    EvalSourceAccessId id = emptyEvalSourceAccessId;
    if (accessIds.cvisit(path, [&](const auto & kv) { id = kv.second; }))
        return id;

    std::lock_guard lock(mutex);
    if (accessIds.cvisit(path, [&](const auto & kv) { id = kv.second; }))
        return id;

    id = static_cast<EvalSourceAccessId>(accesses.size());
    accesses.emplace_back(path);
    accessIds.try_emplace_and_cvisit(
        accesses.back(), id, [&](const auto & kv) { id = kv.second; }, [&](const auto & kv) { id = kv.second; });
    return id;
}

bool EvalSourceAccessSetGraph::accessSetEquals(
    EvalSourceAccessSetId id, const std::vector<EvalSourceAccessId> & items) const
{
    if (id == emptyEvalSourceAccessSetId || id >= accessSets.size())
        return items.empty();

    auto node = accessSets[id];
    if (node.count != items.size())
        return false;
    return std::equal(
        items.begin(),
        items.end(),
        accessSetItems.begin() + node.first,
        accessSetItems.begin() + node.first + node.count);
}

EvalSourceAccessSetId EvalSourceAccessSetGraph::internAccessSet(
    std::span<const EvalSourceAccessId> directAccesses, std::span<const EvalSourceAccessSetId> children)
{
    if (!enabled.load(std::memory_order_acquire))
        return emptyEvalSourceAccessSetId;

    size_t directCount = 0;
    EvalSourceAccessId singleDirect = emptyEvalSourceAccessId;
    for (auto access : directAccesses) {
        if (access == emptyEvalSourceAccessId)
            continue;
        directCount++;
        singleDirect = access;
    }

    size_t childCount = 0;
    EvalSourceAccessSetId singleChild = emptyEvalSourceAccessSetId;
    EvalSourceAccessSetId pairChildA = emptyEvalSourceAccessSetId;
    EvalSourceAccessSetId pairChildB = emptyEvalSourceAccessSetId;
    for (auto child : children) {
        if (child == emptyEvalSourceAccessSetId)
            continue;
        childCount++;
        singleChild = child;
        if (childCount == 1)
            pairChildA = child;
        else if (childCount == 2)
            pairChildB = child;
    }

    if (directCount == 0 && childCount == 0)
        return emptyEvalSourceAccessSetId;
    if (directCount == 0 && childCount == 1)
        return singleChild;

    auto internSingleton = [&](EvalSourceAccessId access) {
        if (access == emptyEvalSourceAccessId)
            return emptyEvalSourceAccessSetId;
        if (access < singletonAccessSets.size())
            if (auto existing = singletonAccessSets[access]; existing != emptyEvalSourceAccessSetId)
                return existing;

        auto first = static_cast<uint32_t>(accessSetItems.size());
        accessSetItems.push_back(access);
        auto id = static_cast<EvalSourceAccessSetId>(accessSets.size());
        accessSets.push_back(EvalSourceAccessSetNode{.first = first, .count = 1});
        if (access >= singletonAccessSets.size())
            singletonAccessSets.resize(access + 1, emptyEvalSourceAccessSetId);
        singletonAccessSets[access] = id;
        return id;
    };

    std::lock_guard lock(mutex);
    if (childCount == 0 && directCount == 1)
        return internSingleton(singleDirect);

    bool hasPairUnionKey = false;
    uint64_t pairUnionKey = 0;
    if (directCount == 0 && childCount == 2) {
        auto a = std::min(pairChildA, pairChildB);
        auto b = std::max(pairChildA, pairChildB);
        if (a == b)
            return a;
        pairUnionKey = (uint64_t{a} << 32) | uint64_t{b};
        if (auto existing = pairUnionAccessSets.find(pairUnionKey); existing != pairUnionAccessSets.end())
            return existing->second;
        hasPairUnionKey = true;
    }

    static thread_local std::vector<EvalSourceAccessId> items;

    auto buildItems = [&] {
        size_t itemCount = directCount;
        for (auto child : children)
            if (child != emptyEvalSourceAccessSetId && child < accessSets.size())
                itemCount += accessSets[child].count;

        items.clear();
        items.reserve(itemCount);

        for (auto access : directAccesses)
            if (access != emptyEvalSourceAccessId)
                items.push_back(access);
        for (auto child : children) {
            if (child == emptyEvalSourceAccessSetId || child >= accessSets.size())
                continue;
            auto node = accessSets[child];
            items.insert(
                items.end(), accessSetItems.begin() + node.first, accessSetItems.begin() + node.first + node.count);
        }

        std::sort(items.begin(), items.end());
        items.erase(std::unique(items.begin(), items.end()), items.end());
        return hashSourceAccessIds(items);
    };

    auto lookupAccessSet = [&](uint64_t hash) -> EvalSourceAccessSetId {
        if (auto head = accessSetIdsByHash.find(hash); head != accessSetIdsByHash.end())
            for (auto id = head->second; id != emptyEvalSourceAccessSetId; id = accessSets[id].nextWithSameHash)
                if (accessSetEquals(id, items))
                    return id;
        return emptyEvalSourceAccessSetId;
    };

    auto hash = buildItems();
    if (items.empty())
        return emptyEvalSourceAccessSetId;
    if (items.size() == 1)
        return internSingleton(items.front());
    if (auto existing = lookupAccessSet(hash); existing != emptyEvalSourceAccessSetId) {
        if (hasPairUnionKey)
            pairUnionAccessSets.emplace(pairUnionKey, existing);
        return existing;
    }

    auto first = static_cast<uint32_t>(accessSetItems.size());
    auto count = static_cast<uint32_t>(items.size());
    accessSetItems.insert(accessSetItems.end(), items.begin(), items.end());

    auto id = static_cast<EvalSourceAccessSetId>(accessSets.size());
    auto next = emptyEvalSourceAccessSetId;
    if (auto head = accessSetIdsByHash.find(hash); head != accessSetIdsByHash.end()) {
        next = head->second;
        head->second = id;
    } else {
        accessSetIdsByHash.emplace(hash, id);
    }
    accessSets.push_back(
        EvalSourceAccessSetNode{
            .first = first,
            .count = count,
            .nextWithSameHash = next,
            .hash = hash,
        });
    if (hasPairUnionKey)
        pairUnionAccessSets.emplace(pairUnionKey, id);
    return id;
}

EvalSourceAccessSetId EvalSourceAccessSetGraph::internAccessSet(
    const std::vector<EvalSourceAccessId> & directAccesses, const std::vector<EvalSourceAccessSetId> & children)
{
    return internAccessSet(
        std::span<const EvalSourceAccessId>(directAccesses.data(), directAccesses.size()),
        std::span<const EvalSourceAccessSetId>(children.data(), children.size()));
}

std::string EvalSourceAccessSetGraph::access(EvalSourceAccessId id) const
{
    if (!enabled.load(std::memory_order_acquire))
        return {};

    std::lock_guard lock(mutex);
    if (id == emptyEvalSourceAccessId || id >= accesses.size())
        return {};
    return accesses[id];
}

std::vector<std::string> EvalSourceAccessSetGraph::flatten(
    const std::vector<EvalSourceAccessId> & directAccesses,
    const std::vector<EvalSourceAccessSetId> & accessSetEdges) const
{
    if (!enabled.load(std::memory_order_acquire))
        return {};

    std::lock_guard lock(mutex);

    if (nextFlattenGeneration == 0) {
        std::fill(seenAccessGenerations.begin(), seenAccessGenerations.end(), 0);
        nextFlattenGeneration = 1;
    }
    auto generation = nextFlattenGeneration++;

    if (seenAccessGenerations.size() < accesses.size())
        seenAccessGenerations.resize(accesses.size(), 0);

    std::vector<EvalSourceAccessId> flattenedAccesses;
    flattenedAccesses.reserve(directAccesses.size());

    auto addAccess = [&](EvalSourceAccessId access) {
        if (access == emptyEvalSourceAccessId || access >= seenAccessGenerations.size())
            return;
        if (seenAccessGenerations[access] == generation)
            return;
        seenAccessGenerations[access] = generation;
        flattenedAccesses.push_back(access);
    };

    for (auto access : directAccesses)
        addAccess(access);

    for (auto accessSet : accessSetEdges) {
        if (accessSet == emptyEvalSourceAccessSetId || accessSet >= accessSets.size())
            continue;
        auto node = accessSets[accessSet];
        for (uint32_t i = 0; i < node.count; i++)
            addAccess(accessSetItems[node.first + i]);
    }

    std::vector<std::string> result;
    result.reserve(flattenedAccesses.size());
    for (auto accessId : flattenedAccesses)
        result.push_back(accesses[accessId]); // addAccess only admits valid non-empty ids
    return result;
}

EvalSourceAccessSetStats EvalSourceAccessSetGraph::stats() const
{
    if (!enabled.load(std::memory_order_acquire))
        return EvalSourceAccessSetStats{};

    std::lock_guard lock(mutex);
    return EvalSourceAccessSetStats{
        .accesses = accesses.empty() ? 0 : accesses.size() - 1,
        .accessSets = accessSets.empty() ? 0 : accessSets.size() - 1,
        .accessSetItems = accessSetItems.size(),
    };
}

/* Frames only suppress *consecutive* duplicates: `internAccessSet` sorts and
   fully dedupes at publish time, so per-append deduplication would be
   redundant work (and quadratic for large scope frames, e.g. the resolver
   import scope). The consecutive check catches the common pattern of a loop
   re-reading one path or re-forcing one value. */
static void addFrameAccess(TrackedSourceDepsFrame & frame, EvalSourceAccessId access)
{
    if (access == emptyEvalSourceAccessId)
        return;

    auto size = frame.directSourceAccessSetAccesses.size();
    if (size == 0 || frame.directSourceAccessSetAccesses.data()[size - 1] != access)
        frame.directSourceAccessSetAccesses.push_back(access);
}

static void addFrameChild(TrackedSourceDepsFrame & frame, EvalSourceAccessSetId child)
{
    if (child == emptyEvalSourceAccessSetId)
        return;

    auto size = frame.childSourceAccessSets.size();
    if (size == 0 || frame.childSourceAccessSets.data()[size - 1] != child)
        frame.childSourceAccessSets.push_back(child);
}

static void addToParentFrame(TrackedSourceDepsFrame & parent, EvalSourceAccessSetId accessSet)
{
    if (accessSet == emptyEvalSourceAccessSetId)
        return;

    addFrameChild(parent, accessSet);
}

static void addToCurrentFrame(TrackingContext & trackingCtx, EvalSourceAccessSetId accessSet)
{
    if (accessSet == emptyEvalSourceAccessSetId)
        return;

    if (auto * frame = currentTecnixThreadState.sourceDepsFrame) {
        addFrameChild(*frame, accessSet);
        return;
    }

    addFrameChild(trackingCtx.rootFrame, accessSet);
}

static bool frameHasSourceDeps(const TrackedSourceDepsFrame & frame)
{
    return !frame.directSourceAccessSetAccesses.empty() || !frame.childSourceAccessSets.empty();
}

static EvalSourceAccessSetId internFrameAccessSet(TrackedSourceDepsFrame & frame)
{
    if (!frameHasSourceDeps(frame))
        return emptyEvalSourceAccessSetId;

    return frame.trackingCtx.sourceAccessSetGraph->internAccessSet(
        std::span<const EvalSourceAccessId>(
            frame.directSourceAccessSetAccesses.data(), frame.directSourceAccessSetAccesses.size()),
        std::span<const EvalSourceAccessSetId>(frame.childSourceAccessSets.data(), frame.childSourceAccessSets.size()));
}

static void mergeFrameIntoParent(TrackedSourceDepsFrame & frame)
{
    auto * parent = frame.previous ? frame.previous : &frame.trackingCtx.rootFrame;
    if (parent == &frame)
        return;

    for (auto access : frame.directSourceAccessSetAccesses)
        addFrameAccess(*parent, access);
    for (auto child : frame.childSourceAccessSets)
        addFrameChild(*parent, child);
}

void mergeUnpublishedTrackedSourceDepsFrame(TrackedSourceDepsFrame & frame)
{
    if (!frame.published)
        mergeFrameIntoParent(frame);
}

void recordTrackedSourceAccessSetAccess(EvalSourceAccessId access)
{
    if (access == emptyEvalSourceAccessId)
        return;

    if (auto * frame = currentTecnixThreadState.sourceDepsFrame)
        addFrameAccess(*frame, access);
}

void recordTrackedSourceAccessSetDependency(TrackingContext & trackingCtx, EvalSourceAccessSetId accessSet)
{
    addToCurrentFrame(trackingCtx, accessSet);
}

static TrackedSourceDepsFrame * currentTrackedValueForceFrame(const void * value = nullptr)
{
    auto * frame = currentTecnixThreadState.sourceDepsFrame;
    auto * valueFrame = frame ? frame->nearestValueForceFrame : nullptr;
    if (!valueFrame || (value && valueFrame->value != value))
        return nullptr;
    return valueFrame;
}

void publishTrackedValueDependencies(const void * value)
{
    auto * frame = currentTrackedValueForceFrame(value);
    if (!frame || frame->published)
        return;

    auto directCount = frame->directSourceAccessSetAccesses.size();
    auto childCount = frame->childSourceAccessSets.size();

    if (!frameHasSourceDeps(*frame)) {
        frame->published = true;
        return;
    }

    EvalSourceAccessSetId sourceAccessSet = emptyEvalSourceAccessSetId;
    if (directCount == 0 && childCount == 1) {
        sourceAccessSet = frame->childSourceAccessSets.data()[0];
        if (sourceAccessSet != emptyEvalSourceAccessSetId)
            frame->value->setTrackedSourceAccessSet(sourceAccessSet);
    } else {
        sourceAccessSet = publishTrackedSourceAccessSetDependencies(
            *frame->trackingCtx.sourceAccessSetGraph,
            *frame->value,
            std::span<const EvalSourceAccessId>(
                frame->directSourceAccessSetAccesses.data(), frame->directSourceAccessSetAccesses.size()),
            std::span<const EvalSourceAccessSetId>(
                frame->childSourceAccessSets.data(), frame->childSourceAccessSets.size()));
    }
    if (sourceAccessSet != emptyEvalSourceAccessSetId) {
        if (frame->previous)
            addToParentFrame(*frame->previous, sourceAccessSet);
        else
            addToCurrentFrame(frame->trackingCtx, sourceAccessSet);
    }
    frame->accessSet = sourceAccessSet;
    frame->published = true;
}

// Copying a finished Value must also copy its provenance. If the copy is the
// value currently being forced, add the source set to that force frame before
// finish() publishes it. Non-current destinations are published after finish()
// by publishCopiedValueDependencies().
void copyTrackedValueDependencies(void * dst, const void * src)
{
    auto * trackingCtx = currentTecnixThreadState.trackingContext;
    if (!trackingCtx || dst == src)
        return;

    auto * currentValueFrame = currentTrackedValueForceFrame(dst);
    if (!currentValueFrame)
        return;

    auto accessSet = static_cast<const Value *>(src)->trackedSourceAccessSet();
    if (accessSet == emptyEvalSourceAccessSetId)
        return;

    addFrameChild(*currentValueFrame, accessSet);
}

void publishCopiedValueDependencies(void * dst, const void * src)
{
    auto * trackingCtx = currentTecnixThreadState.trackingContext;
    if (!trackingCtx || dst == src)
        return;

    auto * currentValueFrame = currentTrackedValueForceFrame(dst);
    if (currentValueFrame)
        return;

    auto accessSet = static_cast<const Value *>(src)->trackedSourceAccessSet();
    if (accessSet == emptyEvalSourceAccessSetId)
        return;

    auto * dstValue = static_cast<Value *>(dst);
    std::array<EvalSourceAccessSetId, 1> children{accessSet};
    publishTrackedSourceAccessSetDependencies(
        *trackingCtx->sourceAccessSetGraph,
        *dstValue,
        std::span<const EvalSourceAccessId>{},
        std::span<const EvalSourceAccessSetId>(children));
}

TrackedSourceDepsFrame::TrackedSourceDepsFrame(
    TrackingContext & trackingCtx, Value * value, TrackedSourceDepsFrame * previous)
    : trackingCtx(trackingCtx)
    , value(value)
    , previous(previous)
    , nearestValueForceFrame(
          value      ? this
          : previous ? previous->nearestValueForceFrame
                     : nullptr)
{
}

TrackingContext::TrackingContext(EvalState & evalState)
    : sourceAccessSetGraph(trackedSourceAccessSetGraph(evalState))
    , rootFrame(*this)
{
    // Establish the invariant every hot path relies on: a live TrackingContext
    // implies an enabled graph, so forcing and the value hooks never re-check.
    sourceAccessSetGraph->enable();
}

void TrackingContext::recordAccess(std::string_view path)
{
    auto accessSetAccessId = sourceAccessSetGraph->internAccess(path);
    recordTrackedSourceAccessSetAccess(accessSetAccessId);
}

ActiveTrackingContext::ActiveTrackingContext(TrackingContext & trackingCtx)
    : trackingCtx(trackingCtx)
    , previousTrackingCtx(currentTecnixThreadState.trackingContext)
    , previousFrame(currentTecnixThreadState.sourceDepsFrame)
{
    currentTecnixThreadState.trackingContext = &trackingCtx;
    currentTecnixThreadState.sourceDepsFrame = &trackingCtx.rootFrame;
}

ActiveTrackingContext::~ActiveTrackingContext()
{
    if (currentTecnixThreadState.sourceDepsFrame == &trackingCtx.rootFrame)
        currentTecnixThreadState.sourceDepsFrame = previousFrame;
    currentTecnixThreadState.trackingContext = previousTrackingCtx;
}

TrackedSourceDepsScope::TrackedSourceDepsScope(TrackingContext & trackingCtx)
    : frame(trackingCtx, nullptr, currentTecnixThreadState.sourceDepsFrame)
    , previousFrame(currentTecnixThreadState.sourceDepsFrame)
{
    currentTecnixThreadState.sourceDepsFrame = &frame;
}

TrackedSourceDepsScope::~TrackedSourceDepsScope()
{
    if (currentTecnixThreadState.sourceDepsFrame == &frame)
        currentTecnixThreadState.sourceDepsFrame = previousFrame;
    mergeUnpublishedTrackedSourceDepsFrame(frame);
}

EvalSourceAccessSetId TrackedSourceDepsScope::finish(Value * publishValue)
{
    if (frame.published)
        return frame.accessSet;

    if (currentTecnixThreadState.sourceDepsFrame == &frame)
        currentTecnixThreadState.sourceDepsFrame = previousFrame;

    frame.accessSet = internFrameAccessSet(frame);
    if (frame.accessSet != emptyEvalSourceAccessSetId) {
        if (frame.previous)
            addToParentFrame(*frame.previous, frame.accessSet);
        else
            addToCurrentFrame(frame.trackingCtx, frame.accessSet);
    }

    if (publishValue && frame.accessSet != emptyEvalSourceAccessSetId)
        publishValue->setTrackedSourceAccessSet(frame.accessSet);
    frame.published = true;
    return frame.accessSet;
}

static EvalState::TecnixEvalData * sourceDepsData(EvalState & state)
{
    return &state.tecnixEvalData();
}

static const EvalState::TecnixEvalData * sourceDepsData(const EvalState & state)
{
    return &state.tecnixEvalData();
}

void enableSourceAccessSetTracking(EvalState & state)
{
    sourceDepsData(state)->sourceAccessSetGraph->enable();
}

EvalSourceAccessSetStats trackedSourceAccessSetStats(const EvalState & state)
{
    return sourceDepsData(state)->sourceAccessSetGraph->stats();
}

ref<EvalSourceAccessSetGraph> trackedSourceAccessSetGraph(const EvalState & state)
{
    return sourceDepsData(state)->sourceAccessSetGraph;
}

EvalSourceAccessSetId publishTrackedSourceAccessSetDependencies(
    EvalSourceAccessSetGraph & graph,
    Value & v,
    std::span<const EvalSourceAccessId> directAccesses,
    std::span<const EvalSourceAccessSetId> children)
{
    auto accessSet = graph.internAccessSet(directAccesses, children);
    if (accessSet != emptyEvalSourceAccessSetId)
        v.setTrackedSourceAccessSet(accessSet);
    return accessSet;
}

} // namespace nix
