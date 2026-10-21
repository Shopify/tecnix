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

    /* Published before the chunk pointer, and therefore before the label store
       that this chunk is being installed for: a reader that can observe any
       label can also observe the flag. */

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

/* Hashes a canonical `internAccessSet` input. The direct ids and the child set
   ids live in different id spaces, so they are separated by a marker that
   cannot occur in either: without it `({a}, {})` and `({}, {a})` would collide
   into the same bucket (harmless, but it would cost a chain walk on every
   lookup). */
static uint64_t hashAccessSetInputKey(
    std::span<const EvalSourceAccessId> directAccesses, std::span<const EvalSourceAccessSetId> children)
{
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](uint64_t word) {
        hash ^= word;
        hash *= 1099511628211ULL;
    };

    for (auto access : directAccesses)
        mix(access);
    mix(~uint64_t{0});
    for (auto child : children)
        mix(child);
    return hash;
}

/* Copies the non-empty ids of `in` into `out`, sorted and deduplicated, so that
   frames which recorded the same ids in a different order (or more than once,
   which the frame appenders only partially suppress) produce the same memo
   key. */
template<typename Id>
static void canonicaliseAccessSetInput(std::vector<Id> & out, std::span<const Id> in)
{
    out.clear();
    for (auto id : in)
        if (id != 0)
            out.push_back(id);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
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
    inputKeys.push_back(EvalSourceAccessSetInputNode{}); // index 0 is the "no entry" sentinel
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

bool EvalSourceAccessSetGraph::inputKeyEquals(
    uint32_t id,
    std::span<const EvalSourceAccessId> directAccesses,
    std::span<const EvalSourceAccessSetId> children) const
{
    auto node = inputKeys[id];
    if (node.directCount != directAccesses.size() || node.childCount != children.size())
        return false;

    auto * items = inputKeyItems.data() + node.first;
    return std::equal(directAccesses.begin(), directAccesses.end(), items)
           && std::equal(children.begin(), children.end(), items + node.directCount);
}

EvalSourceAccessSetId EvalSourceAccessSetGraph::lookupInputKey(
    uint64_t hash,
    std::span<const EvalSourceAccessId> directAccesses,
    std::span<const EvalSourceAccessSetId> children,
    bool & found) const
{
    found = false;
    auto head = inputKeyIdsByHash.find(hash);
    if (head == inputKeyIdsByHash.end())
        return emptyEvalSourceAccessSetId;

    for (auto id = head->second; id != 0; id = inputKeys[id].nextWithSameHash) {
        if (!inputKeyEquals(id, directAccesses, children))
            continue;
        found = true;
        return inputKeys[id].accessSet;
    }
    return emptyEvalSourceAccessSetId;
}

void EvalSourceAccessSetGraph::rememberInputKey(
    uint64_t hash,
    std::span<const EvalSourceAccessId> directAccesses,
    std::span<const EvalSourceAccessSetId> children,
    EvalSourceAccessSetId accessSet)
{
    /* The memo is a pure accelerator: past the cap we simply stop adding
       entries and fall back to flattening, rather than let a pathological
       evaluation grow it without bound. */
    constexpr size_t maxInputKeys = size_t{1} << 20;
    if (inputKeys.size() >= maxInputKeys)
        return;

    auto first = static_cast<uint32_t>(inputKeyItems.size());
    inputKeyItems.insert(inputKeyItems.end(), directAccesses.begin(), directAccesses.end());
    inputKeyItems.insert(inputKeyItems.end(), children.begin(), children.end());

    auto id = static_cast<uint32_t>(inputKeys.size());
    uint32_t next = 0;
    if (auto head = inputKeyIdsByHash.find(hash); head != inputKeyIdsByHash.end()) {
        next = head->second;
        head->second = id;
    } else {
        inputKeyIdsByHash.emplace(hash, id);
    }
    inputKeys.push_back(
        EvalSourceAccessSetInputNode{
            .first = first,
            .directCount = static_cast<uint32_t>(directAccesses.size()),
            .childCount = static_cast<uint32_t>(children.size()),
            .nextWithSameHash = next,
            .hash = hash,
            .accessSet = accessSet,
        });
}

EvalSourceAccessSetId EvalSourceAccessSetGraph::internAccessSet(
    std::span<const EvalSourceAccessId> directAccesses, std::span<const EvalSourceAccessSetId> children)
{
    if (!enabled.load(std::memory_order_acquire))
        return emptyEvalSourceAccessSetId;

    /* Canonicalise the input before anything else: it is what the memo below is
       keyed on, and it is tiny (one frame's direct accesses and child edges)
       compared to the transitive union those children stand for. */
    static thread_local std::vector<EvalSourceAccessId> keyDirect;
    static thread_local std::vector<EvalSourceAccessSetId> keyChildren;
    canonicaliseAccessSetInput(keyDirect, directAccesses);
    canonicaliseAccessSetInput(keyChildren, children);

    if (keyDirect.empty()) {
        if (keyChildren.empty())
            return emptyEvalSourceAccessSetId;
        if (keyChildren.size() == 1)
            return keyChildren.front();
    }

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
    if (keyChildren.empty() && keyDirect.size() == 1)
        return internSingleton(keyDirect.front());

    /* Memoise on the canonical input. Publishing the same input tuple over and
       over is the normal case — the same thunks and imports are re-forced under
       every target — and without this each repeat pays a full flatten, sort and
       dedup of the transitive union just to discover, via `accessSetIdsByHash`,
       that the resulting set already exists. The union is unbounded (sets here
       average ~160 members and target roots are far larger); the input is a
       handful of ids. */
    auto inputKeyHash = hashAccessSetInputKey(keyDirect, keyChildren);
    bool memoised = false;
    if (auto existing = lookupInputKey(inputKeyHash, keyDirect, keyChildren, memoised); memoised)
        return existing;

    static thread_local std::vector<EvalSourceAccessId> items;

    auto buildItems = [&] {
        size_t itemCount = keyDirect.size();
        for (auto child : keyChildren)
            if (child < accessSets.size())
                itemCount += accessSets[child].count;

        items.clear();
        items.reserve(itemCount);
        items.insert(items.end(), keyDirect.begin(), keyDirect.end());
        for (auto child : keyChildren) {
            if (child >= accessSets.size())
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
    if (items.empty()) {
        rememberInputKey(inputKeyHash, keyDirect, keyChildren, emptyEvalSourceAccessSetId);
        return emptyEvalSourceAccessSetId;
    }
    if (items.size() == 1) {
        auto singleton = internSingleton(items.front());
        rememberInputKey(inputKeyHash, keyDirect, keyChildren, singleton);
        return singleton;
    }
    if (auto existing = lookupAccessSet(hash); existing != emptyEvalSourceAccessSetId) {
        rememberInputKey(inputKeyHash, keyDirect, keyChildren, existing);
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
    rememberInputKey(inputKeyHash, keyDirect, keyChildren, id);
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

/* Frames suppress duplicates against a bounded window of recent appends rather
   than against the whole frame: `internAccessSet` sorts and fully dedupes at
   publish time, so exhaustive per-append deduplication would be redundant work
   (and quadratic for large scope frames, e.g. the resolver import scope).

   The window is deliberately wider than the last entry. Duplicates arrive
   interleaved, not just consecutively — a loop that forces a handful of values
   round-robin, or a scope that re-reads two or three paths in rotation, defeats
   a size-1 check entirely. Every duplicate that survives into the frame inflates
   the publish: a larger input to hash and compare, and, on a memo miss, more
   member lists to concatenate and sort. Scanning a fixed window keeps the append
   O(1) while collapsing the common patterns. */
/* Duplicate suppression compares against the frame's own last entry only, never
   against entries below its watermark: those belong to the parent, and skipping
   a push because the *parent* already holds the id would leave this frame with
   no dependencies of its own, so its value would be published without a label. */
static void addFrameAccess(TrackedSourceDepsFrame & frame, EvalSourceAccessId access)
{
    if (access == emptyEvalSourceAccessId)
        return;

    auto & ids = frame.trackingCtx.frameAccessStack;
    if (ids.size() > frame.accessBase && ids.back() == access)
        return;
    ids.push_back(access);
}

static void addFrameChild(TrackedSourceDepsFrame & frame, EvalSourceAccessSetId child)
{
    if (child == emptyEvalSourceAccessSetId)
        return;

    auto & ids = frame.trackingCtx.frameChildStack;
    if (ids.size() > frame.childBase && ids.back() == child)
        return;
    ids.push_back(child);
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
    return frame.trackingCtx.frameAccessStack.size() > frame.accessBase
           || frame.trackingCtx.frameChildStack.size() > frame.childBase;
}

static EvalSourceAccessSetId internFrameAccessSet(TrackedSourceDepsFrame & frame)
{
    if (!frameHasSourceDeps(frame))
        return emptyEvalSourceAccessSetId;

    return frame.trackingCtx.sourceAccessSetGraph->internAccessSet(
        frame.directSourceAccessSetAccesses(), frame.childSourceAccessSets());
}

/* Truncate the frame's region and leave `accessSet` in its place, so the frame's
   contribution to its parent is exactly the one interned id. Recording the marks
   as the new bases lets frame teardown restore the stacks with a plain compare. */
static void collapseFrameToAccessSet(TrackedSourceDepsFrame & frame, EvalSourceAccessSetId accessSet)
{
    auto & accesses = frame.trackingCtx.frameAccessStack;
    auto & children = frame.trackingCtx.frameChildStack;

    if (accesses.size() > frame.accessBase)
        accesses.resize(frame.accessBase);
    if (children.size() > frame.childBase)
        children.resize(frame.childBase);

    if (accessSet != emptyEvalSourceAccessSetId) {
        auto * parent = frame.previous ? frame.previous : &frame.trackingCtx.rootFrame;
        if (parent != &frame)
            addFrameChild(*parent, accessSet);
        else
            addFrameChild(frame, accessSet);
    }

    frame.accessBase = accesses.size();
    frame.childBase = children.size();
}

void mergeUnpublishedTrackedSourceDepsFrame(TrackedSourceDepsFrame & frame)
{
    /* An unpublished frame needs no merge: its entries already sit contiguously
       inside the parent's region, so leaving them in place is the merge.

       A published frame has already been collapsed to its interned id, and its
       bases moved past it. Anything recorded after the publish is discarded --
       matching the copy-based implementation, where a published frame's entries
       were simply never copied out. */
    if (!frame.published)
        return;

    auto & accesses = frame.trackingCtx.frameAccessStack;
    auto & children = frame.trackingCtx.frameChildStack;
    if (accesses.size() > frame.accessBase)
        accesses.resize(frame.accessBase);
    if (children.size() > frame.childBase)
        children.resize(frame.childBase);
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

    auto directAccesses = frame->directSourceAccessSetAccesses();
    auto children = frame->childSourceAccessSets();

    if (directAccesses.empty() && children.empty()) {
        frame->published = true;
        return;
    }

    EvalSourceAccessSetId sourceAccessSet = emptyEvalSourceAccessSetId;
    if (directAccesses.empty() && children.size() == 1) {
        sourceAccessSet = children[0];
        if (sourceAccessSet != emptyEvalSourceAccessSetId)
            frame->value->setTrackedSourceAccessSet(sourceAccessSet);
    } else {
        sourceAccessSet = publishTrackedSourceAccessSetDependencies(
            *frame->trackingCtx.sourceAccessSetGraph, *frame->value, directAccesses, children);
    }
    collapseFrameToAccessSet(*frame, sourceAccessSet);
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

std::span<const EvalSourceAccessId> TrackedSourceDepsFrame::directSourceAccessSetAccesses() const
{
    const auto & ids = trackingCtx.frameAccessStack;
    if (ids.size() <= accessBase)
        return {};
    return {ids.data() + accessBase, ids.size() - accessBase};
}

std::span<const EvalSourceAccessSetId> TrackedSourceDepsFrame::childSourceAccessSets() const
{
    const auto & ids = trackingCtx.frameChildStack;
    if (ids.size() <= childBase)
        return {};
    return {ids.data() + childBase, ids.size() - childBase};
}

TrackedSourceDepsFrame::TrackedSourceDepsFrame(
    TrackingContext & trackingCtx, Value * value, TrackedSourceDepsFrame * previous)
    : trackingCtx(trackingCtx)
    , value(value)
    , accessBase(trackingCtx.frameAccessStack.size())
    , childBase(trackingCtx.frameChildStack.size())
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
    /* Sized to hold a deep force chain without reallocating mid-evaluation;
       frames hold indices, not pointers, so a reallocation would be correct,
       just wasteful. */
    frameAccessStack.reserve(4096);
    frameChildStack.reserve(4096);

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
    collapseFrameToAccessSet(frame, frame.accessSet);

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
