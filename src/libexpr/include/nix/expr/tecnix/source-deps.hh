#pragma once
///@file

#include "nix/expr/tecnix/thread-state.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/ref.hh"

#include <boost/container/small_vector.hpp>

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

using EvalSourceAccessIdFrameVector = boost::container::small_vector<EvalSourceAccessId, 1>;
using EvalSourceAccessSetIdFrameVector = boost::container::small_vector<EvalSourceAccessSetId, 2>;

/**
 * A stack-resident accumulator for one bracketed region of evaluation: the
 * force of one value (`value` set) or a source-deps scope / target root
 * (`value` null). Collects direct path accesses and inherited child labels;
 * interned into one set id when the region publishes.
 */
struct TrackedSourceDepsFrame
{
    TrackingContext & trackingCtx;
    Value * value = nullptr;
    EvalSourceAccessIdFrameVector directSourceAccessSetAccesses;
    EvalSourceAccessSetIdFrameVector childSourceAccessSets;
    EvalSourceAccessSetId accessSet = emptyEvalSourceAccessSetId;
    TrackedSourceDepsFrame * previous = nullptr;
    TrackedSourceDepsFrame * nearestValueForceFrame = nullptr;
    bool published = false;

    TrackedSourceDepsFrame(
        TrackingContext & trackingCtx, Value * value = nullptr, TrackedSourceDepsFrame * previous = nullptr);
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
