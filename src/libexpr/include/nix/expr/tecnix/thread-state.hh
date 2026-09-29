#pragma once

///@file

#include <cstdint>

namespace nix {

struct TrackingContext;
struct TrackedSourceDepsFrame;

struct TecnixThreadState
{
    TrackingContext * trackingContext = nullptr;
    TrackedSourceDepsFrame * sourceDepsFrame = nullptr;
    const void * valueDependencyPublishValue = nullptr;

    /**
     * Bytes this thread has allocated on the evaluator's GC heap through
     * `EvalMemory` (values, environments, attribute sets, lists, strings).
     * Maintained unconditionally -- one thread-local add per allocation --
     * and read by tracing frames at open and close.
     */
    uint64_t bytesAllocated = 0;

    /**
     * Trace key for the next tracing frame opened on this thread, set by
     * `evalFile` just before it forces a file's thunk: that thunk's
     * expression is a stack object with no position, so the frame keys on
     * the file instead. 0 means none. Consumed (cleared) by whichever branch
     * of `forceValueTracked` runs next, whether or not a frame opens.
     */
    uint32_t pendingImportKey = 0;
};

[[gnu::tls_model("initial-exec")]] extern thread_local TecnixThreadState currentTecnixThreadState;

} // namespace nix
