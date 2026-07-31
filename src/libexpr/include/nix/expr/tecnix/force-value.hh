#pragma once

///@file

namespace nix {

[[gnu::always_inline]] inline void
forceValueTracked(EvalState & state, Value & v, const PosIdx pos, TrackingContext & trackingCtx)
{
    auto recordPublishedDependencies = [&]() {
        auto sourceAccessSet = v.trackedSourceAccessSet();
        if (sourceAccessSet != emptyEvalSourceAccessSetId)
            recordTrackedSourceAccessSetDependency(trackingCtx, sourceAccessSet);
    };

    if (v.isFinished()) {
        recordPublishedDependencies();
        if (v.isFailed())
            v.force(state, pos);
        return;
    }

    if (!(v.isThunk() || v.isApp())) {
        v.force(state, pos);
        recordPublishedDependencies();
        return;
    }

    TrackedSourceDepsFrame frame(trackingCtx, &v, currentTecnixThreadState.sourceDepsFrame);
    auto * previousFrame = currentTecnixThreadState.sourceDepsFrame;
    auto * previousPublishValue = currentTecnixThreadState.valueDependencyPublishValue;
    currentTecnixThreadState.sourceDepsFrame = &frame;
    currentTecnixThreadState.valueDependencyPublishValue = &v;
    Finally restoreTrackedValueForceFrame([&]() {
        currentTecnixThreadState.sourceDepsFrame = previousFrame;
        currentTecnixThreadState.valueDependencyPublishValue = previousPublishValue;
        mergeUnpublishedTrackedSourceDepsFrame(frame);
    });

    v.force(state, pos);
    if (!frame.published)
        recordPublishedDependencies();
}

} // namespace nix
