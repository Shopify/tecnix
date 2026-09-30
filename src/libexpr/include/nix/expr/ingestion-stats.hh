#pragma once
///@file

#include "nix/util/pos-idx.hh"

#include <cstdint>
#include <string>

namespace nix {

/**
 * Statistics only (`NIX_SHOW_STATS`): attribute source ingestion (a
 * `fetchToStore` that hashes or copies a path into the store) to the Tecnix
 * target evaluated on this thread and to the Nix expression position that
 * caused it.
 *
 * The per-target figures are the work done while a target was being evaluated,
 * not the size of its source dependencies. A source path shared by several
 * targets is charged to whichever target's evaluation forced it first; the
 * others hit the in-memory cache and are charged nothing. Under parallel
 * evaluation "first" is a race, so a shared path can move between targets
 * from run to run. The dependency tracer, by contrast, records every target
 * that touched a path; use it, not these figures, to ask what a target
 * depends on. Per-site figures behave the same way; per-caller totals count
 * every ingestion exactly once and are not affected.
 */

/**
 * Charge ingestion on this thread to `target` while the scope is alive.
 * Scopes nest; the innermost wins. `target` must outlive the scope.
 */
struct IngestionTargetScope
{
    explicit IngestionTargetScope(const std::string & target);
    ~IngestionTargetScope();
    IngestionTargetScope(const IngestionTargetScope &) = delete;
    IngestionTargetScope & operator=(const IngestionTargetScope &) = delete;

private:
    const std::string * previous;
};

/**
 * Charge the ingestion this thread performs while the scope is alive to `pos`
 * (and to the current target). Nested scopes are charged only for their own
 * work: an enclosing scope excludes what inner scopes already recorded.
 */
struct IngestionSiteScope
{
    explicit IngestionSiteScope(PosIdx pos);
    ~IngestionSiteScope();
    IngestionSiteScope(const IngestionSiteScope &) = delete;
    IngestionSiteScope & operator=(const IngestionSiteScope &) = delete;

private:
    PosIdx pos;
    uint64_t ingestions, bytes, nanos;
    uint64_t innerIngestions, innerBytes, innerNanos;
};

} // namespace nix
