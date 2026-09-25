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
    /** NAR bytes of copied paths. Dry-run hashing does not report a size. */
    uint64_t bytesCopied = 0;
    double secondsIngesting = 0;
};

/**
 * Snapshot of this process's counters, indexed by `FetchToStoreCaller`.
 */
std::vector<FetchToStoreStats> getFetchToStoreStats();

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
