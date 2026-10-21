#pragma once
///@file

#include "nix/expr/tecnix/thread-state.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/ref.hh"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

class EvalState;
struct Value;

using EvalSourceAccessId = uint32_t;
using EvalSourceAccessSetId = uint32_t;
static constexpr EvalSourceAccessId emptyEvalSourceAccessId = 0;
static constexpr EvalSourceAccessSetId emptyEvalSourceAccessSetId = 0;

class EvalSourceAccessSetGraph;
struct TrackingContext;

struct EvalSourceAccessSetStats
{
    size_t accesses = 0;
    size_t accessSets = 0;
    size_t accessSetItems = 0;
};

void enableSourceAccessSetTracking(EvalState & state);
EvalSourceAccessSetStats trackedSourceAccessSetStats(const EvalState & state);
ref<EvalSourceAccessSetGraph> trackedSourceAccessSetGraph(const EvalState & state);
EvalSourceAccessSetId publishTrackedSourceAccessSetDependencies(
    EvalSourceAccessSetGraph & graph,
    Value & v,
    std::span<const EvalSourceAccessId> directAccesses,
    std::span<const EvalSourceAccessSetId> children);
void recordTrackedSourceAccessSetAccess(EvalSourceAccessId access);
void recordTrackedSourceAccessSetDependency(TrackingContext & trackingCtx, EvalSourceAccessSetId accessSet);
void mergeUnpublishedTrackedSourceDepsFrame(TrackedSourceDepsFrame & frame);
[[gnu::always_inline]] inline void
forceValueTracked(EvalState & state, Value & v, PosIdx pos, TrackingContext & trackingCtx);

std::vector<std::string> parseGitPorcelainZDirtyPaths(std::string_view output);

/**
 * A stack-resident accumulator for one bracketed region of evaluation: the
 * force of one value (`value` set) or a source-deps scope / target root
 * (`value` null). Collects direct path accesses and inherited child labels;
 * interned into one set id when the region publishes.
 *
 * Frames are strictly LIFO within a tracking context, so their entries do not
 * need per-frame containers: they live in two stacks owned by the context and
 * a frame stores only the stack sizes at entry (its "watermarks"). A frame's
 * entries are whatever sits above its marks.
 *
 * This makes the two operations that dominate tracked evaluation nearly free.
 * Entering and leaving a region that records nothing -- the majority of forces
 * -- costs two loads and two compares rather than constructing and destroying
 * two containers. Merging an unpublished frame into its parent costs nothing
 * at all, because the entries are already contiguous inside the parent's own
 * region; leaving them in place *is* the merge. Publishing truncates back to
 * the marks and pushes the single interned id in their place.
 *
 * The frame is trivially destructible, so it emits no destructor.
 */
struct TrackedSourceDepsFrame
{
    TrackingContext & trackingCtx;
    Value * value = nullptr;
    /** Size of the context's access stack when this frame was entered. */
    uint32_t accessBase = 0;
    /** Size of the context's child stack when this frame was entered. */
    uint32_t childBase = 0;
    EvalSourceAccessSetId accessSet = emptyEvalSourceAccessSetId;
    TrackedSourceDepsFrame * previous = nullptr;
    TrackedSourceDepsFrame * nearestValueForceFrame = nullptr;
    bool published = false;

    TrackedSourceDepsFrame(
        TrackingContext & trackingCtx, Value * value = nullptr, TrackedSourceDepsFrame * previous = nullptr);

    /** The direct accesses recorded into this frame, as a view into the context stack. */
    std::span<const EvalSourceAccessId> directSourceAccessSetAccesses() const;
    /** The child labels recorded into this frame, as a view into the context stack. */
    std::span<const EvalSourceAccessSetId> childSourceAccessSets() const;
};

/**
 * Tracks file/directory accesses during Tecnix target resolution and
 * target-name discovery for cache invalidation. Paths are repo-relative
 * (e.g. "areas/core/shopify/default.nix").
 *
 * Tracking contexts are thread-confined: a context is created, recorded
 * into, snapshotted, and destroyed on one thread, so it needs no locking.
 * Tracked evaluation must not spawn parallel evaluation work (enforced in
 * EvalState::makeWork); the only cross-thread dependency channel is the
 * published label on a finished value.
 *
 * Tracking contexts must use the EvalState-owned source-access graph. Inline
 * Value labels are graph-local IDs, so constructing a context with a private
 * graph would silently interpret copied/forced value labels as the wrong paths.
 *
 * Constructing a context enables the graph, establishing the invariant the
 * hot paths rely on: a live context implies an enabled graph.
 */
struct TrackingContext
{
    ref<EvalSourceAccessSetGraph> sourceAccessSetGraph;
    /**
     * Backing storage for every frame in this context. Declared before
     * `rootFrame` so they are constructed before it reads their sizes.
     */
    std::vector<EvalSourceAccessId> frameAccessStack;
    std::vector<EvalSourceAccessSetId> frameChildStack;
    TrackedSourceDepsFrame rootFrame;

    // Always captures the EvalState-owned source-access graph; no foreign graph constructor exists.
    explicit TrackingContext(EvalState & state);

    void recordAccess(std::string_view path);
};

struct ActiveTrackingContext
{
    TrackingContext & trackingCtx;
    TrackingContext * previousTrackingCtx = nullptr;
    TrackedSourceDepsFrame * previousFrame = nullptr;

    explicit ActiveTrackingContext(TrackingContext & trackingCtx);
    ActiveTrackingContext(const ActiveTrackingContext &) = delete;
    ActiveTrackingContext & operator=(const ActiveTrackingContext &) = delete;
    ~ActiveTrackingContext();
};

struct TrackedSourceDepsScope
{
    TrackedSourceDepsFrame frame;
    TrackedSourceDepsFrame * previousFrame = nullptr;

    explicit TrackedSourceDepsScope(TrackingContext & trackingCtx);
    TrackedSourceDepsScope(const TrackedSourceDepsScope &) = delete;
    TrackedSourceDepsScope & operator=(const TrackedSourceDepsScope &) = delete;
    ~TrackedSourceDepsScope();

    EvalSourceAccessSetId finish(Value * publishValue = nullptr);
};

} // namespace nix
