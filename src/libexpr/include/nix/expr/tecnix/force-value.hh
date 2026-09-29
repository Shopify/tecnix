#pragma once

///@file

#include "nix/expr/tecnix/trace.hh"

namespace nix {

[[gnu::always_inline]] inline void
forceValueTracked(EvalState & state, Value & v, const PosIdx pos, TrackingContext & trackingCtx)
{
    auto recordPublishedDependencies = [&]() {
        auto sourceAccessSet = v.trackedSourceAccessSet();
        if (sourceAccessSet != emptyEvalSourceAccessSetId)
            recordTrackedSourceAccessSetDependency(trackingCtx, sourceAccessSet);
    };

    /* Tracing adds one predicted branch per force to the tracked path and
       nothing to the untracked one; the tracing bodies stay out of line. */
    auto * trace = trackingCtx.trace.get();

    if (v.isFinished()) {
        recordPublishedDependencies();
        if (trace) [[unlikely]]
            traceFinishedValue(*trace, v);
        if (v.isFailed())
            v.force(state, pos);
        return;
    }

    if (!(v.isThunk() || v.isApp())) {
        if (trace) [[unlikely]]
            traceWaitedForce(state, v, pos, *trace);
        else
            v.force(state, pos);
        recordPublishedDependencies();
        return;
    }

    TracedSourceDepsFrame frame(trackingCtx, &v, currentTecnixThreadState.sourceDepsFrame);
    auto * previousFrame = currentTecnixThreadState.sourceDepsFrame;
    auto * previousPublishValue = currentTecnixThreadState.valueDependencyPublishValue;
    currentTecnixThreadState.sourceDepsFrame = &frame;
    currentTecnixThreadState.valueDependencyPublishValue = &v;
    if (trace) [[unlikely]]
        traceFrameOpen(*trace, frame, v, pos);
    Finally restoreTrackedValueForceFrame([&]() {
        currentTecnixThreadState.sourceDepsFrame = previousFrame;
        currentTecnixThreadState.valueDependencyPublishValue = previousPublishValue;
        mergeUnpublishedTrackedSourceDepsFrame(frame);
        if (trace) [[unlikely]]
            traceFrameClose(*trace, frame);
    });

    v.force(state, pos);
    if (!frame.published)
        recordPublishedDependencies();
}

} // namespace nix
