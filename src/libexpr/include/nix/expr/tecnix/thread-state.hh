#pragma once

///@file

#include "nix/expr/ingestion-stats.hh"

#include <string>

namespace nix {

struct TrackingContext;
struct TrackedSourceDepsFrame;
struct TecnixMemoizeInProgressFrame;

/**
 * Tecnix state of the evaluation running on this thread. It belongs to
 * that evaluation, not to the thread (most of it points into the
 * evaluation's own stack frames): `Executor::runFiber()` swaps it in and
 * out together with the fiber.
 */
struct TecnixThreadState
{
    TrackingContext * trackingContext = nullptr;
    TrackedSourceDepsFrame * sourceDepsFrame = nullptr;
    const void * valueDependencyPublishValue = nullptr;
    TecnixMemoizeInProgressFrame * tecnixMemoizeInProgress = nullptr;

    /**
     * Statistics only: the target that ingestion is charged to (see
     * `IngestionTargetScope`), and the ingestion that site scopes have
     * already charged, so that enclosing scopes can exclude it (see
     * `IngestionSiteScope`).
     */
    const std::string * ingestionTarget = nullptr;
    IngestionCounts ingestionChargedByScopes;
};

[[gnu::tls_model("initial-exec")]] extern thread_local TecnixThreadState currentTecnixThreadState;

} // namespace nix
