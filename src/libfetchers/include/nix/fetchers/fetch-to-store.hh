#pragma once

#include "nix/util/source-path.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/repair-flag.hh"
#include "nix/util/file-content-address.hh"
#include "nix/fetchers/cache.hh"

#include <cstdint>
#include <string_view>
#include <vector>

namespace nix {

enum struct FetchMode { DryRun, Copy };

/**
 * Which evaluator code path asked `fetchToStore` for a source path. Only used
 * for statistics (`NIX_SHOW_STATS`), to tell source ingestion caused by
 * coercing a path value to a string apart from lazy-tree devirtualization,
 * `builtins.path`/`builtins.filterSource`, and the Tectonix zone/tree builtins.
 */
enum struct FetchToStoreCaller : unsigned int {
    Other = 0,
    CoercedPath,
    Devirtualize,
    BuiltinsPath,
    TectonixZone,
    TectonixTree,
    Count,
};

std::string_view fetchToStoreCallerName(FetchToStoreCaller caller);

/**
 * Attribute the `fetchToStore` calls this thread makes while the scope is
 * alive to `caller`. Scopes nest; the innermost one wins.
 */
struct FetchToStoreCallerScope
{
    explicit FetchToStoreCallerScope(FetchToStoreCaller caller);
    ~FetchToStoreCallerScope();
    FetchToStoreCallerScope(const FetchToStoreCallerScope &) = delete;
    FetchToStoreCallerScope & operator=(const FetchToStoreCallerScope &) = delete;

private:
    FetchToStoreCaller previous;
};

struct FetchToStoreStats
{
    uint64_t calls = 0;
    uint64_t memoryCacheHits = 0;
    uint64_t persistentCacheHits = 0;
    /** Paths hashed (dry run) or copied into the store: the O(bytes) work. */
    uint64_t ingestions = 0;
    uint64_t dryRunIngestions = 0;
    uint64_t filteredIngestions = 0;
    /**
     * NAR size of the paths passed to `addToStore`, whether or not the store
     * already held them: it depends only on content, so it is comparable
     * across evaluations and across warm and cold stores. Dry-run hashing
     * does not report a size, so it adds nothing here.
     */
    uint64_t bytesIngested = 0;
    /** Wall time hashing or copying, excluding time spent in a path filter. */
    double secondsIngesting = 0;
    /**
     * Wall time spent inside the path filter callback (for `builtins.path`
     * this is Nix evaluation), including anything the filter itself evaluates.
     */
    double secondsFiltering = 0;
};

/**
 * Snapshot of this process's counters, indexed by `FetchToStoreCaller`.
 */
std::vector<FetchToStoreStats> getFetchToStoreStats();

/**
 * Running totals of the ingestion work (hashing or copying a path into the
 * store) done by the calling thread, across all callers. Evaluator code
 * snapshots these around a call to charge the ingestion it caused to a call
 * site or target.
 */
struct FetchToStoreThreadTotals
{
    uint64_t ingestions = 0;
    uint64_t bytesIngested = 0;
    uint64_t nanosIngesting = 0;
};

FetchToStoreThreadTotals getFetchToStoreThreadTotals();

/**
 * Whether `NIX_SHOW_STATS` is set. Until it is, none of the counters above are
 * maintained (same gate as `Counter::enabled`), so the `fetchToStore` hot path
 * pays one predictable branch.
 */
bool fetchToStoreStatsEnabled();

/**
 * Copy the `path` to the Nix store.
 */
StorePath fetchToStore(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name = "source",
    ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
    PathFilter * filter = nullptr,
    RepairFlag repair = NoRepair);

std::pair<StorePath, Hash> fetchToStore2(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name = "source",
    ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
    PathFilter * filter = nullptr,
    RepairFlag repair = NoRepair);

fetchers::Cache::Key
makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path);

} // namespace nix
