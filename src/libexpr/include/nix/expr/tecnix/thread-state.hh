#pragma once

///@file

namespace nix {

struct TrackingContext;
struct TrackedSourceDepsFrame;

struct TecnixThreadState
{
    TrackingContext * trackingContext = nullptr;
    TrackedSourceDepsFrame * sourceDepsFrame = nullptr;
    const void * valueDependencyPublishValue = nullptr;
};

[[gnu::tls_model("initial-exec")]] extern thread_local TecnixThreadState currentTecnixThreadState;

} // namespace nix
