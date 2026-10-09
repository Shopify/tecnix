#pragma once

///@file

namespace nix {

struct TrackingContext;
struct TrackedSourceDepsFrame;
struct TecnixMemoizeInProgressFrame;

/**
 * Tecnix state of the evaluation running on this thread. All of it points
 * into that evaluation's own stack frames, so it belongs to the fiber or
 * thread doing the evaluation: `Executor::runFiber()` swaps it in and out
 * together with the fiber.
 */
struct TecnixThreadState
{
    TrackingContext * trackingContext = nullptr;
    TrackedSourceDepsFrame * sourceDepsFrame = nullptr;
    const void * valueDependencyPublishValue = nullptr;
    TecnixMemoizeInProgressFrame * tecnixMemoizeInProgress = nullptr;
};

[[gnu::tls_model("initial-exec")]] extern thread_local TecnixThreadState currentTecnixThreadState;

} // namespace nix
