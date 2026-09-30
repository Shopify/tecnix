#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/environment-variables.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

#include <atomic>
#include <chrono>
#include <new>

namespace nix {

struct SrcToStore
{
    boost::concurrent_flat_map<
        std::tuple<SourcePath, ContentAddressMethod::Raw, std::string>,
        std::tuple<StorePath, Hash, FetchMode>>
        cache;
};

ref<SrcToStore> fetchers::Settings::createSrcToStore()
{
    return make_ref<SrcToStore>();
}

fetchers::Cache::Key
makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path)
{
    return fetchers::Cache::Key{
        "sourcePathToHash",
        {{"fingerprint", std::string(fingerprint)}, {"method", std::string{method.render()}}, {"path", path.abs()}}};
}

namespace {

/**
 * Per-caller counters. Aligned to a cache line so callers do not contend with
 * each other, and only touched when `NIX_SHOW_STATS` is set.
 */
struct alignas(std::hardware_destructive_interference_size) AtomicFetchToStoreStats
{
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> memoryCacheHits{0};
    std::atomic<uint64_t> persistentCacheHits{0};
    std::atomic<uint64_t> ingestions{0};
    std::atomic<uint64_t> dryRunIngestions{0};
    std::atomic<uint64_t> filteredIngestions{0};
    std::atomic<uint64_t> bytesIngested{0};
    std::atomic<uint64_t> nanosIngesting{0};
    std::atomic<uint64_t> nanosFiltering{0};
};

constexpr size_t nFetchToStoreCallers = static_cast<size_t>(FetchToStoreCaller::Count);

AtomicFetchToStoreStats fetchToStoreStats[nFetchToStoreCallers];

/** Same gate as `Counter::enabled` in libexpr, which libfetchers cannot reach. */
const bool statsEnabled = getEnv("NIX_SHOW_STATS").value_or("0") != "0";

/** Increment a statistics counter. Callers check `statsEnabled` first. */
void bump(std::atomic<uint64_t> & counter, uint64_t n = 1)
{
    counter.fetch_add(n, std::memory_order_relaxed);
}

uint64_t nanosSince(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

thread_local FetchToStoreCaller currentFetchToStoreCaller = FetchToStoreCaller::Other;

thread_local FetchToStoreThreadTotals fetchToStoreThreadTotals;

} // namespace

bool fetchToStoreStatsEnabled()
{
    return statsEnabled;
}

std::string_view fetchToStoreCallerName(FetchToStoreCaller caller)
{
    switch (caller) {
    case FetchToStoreCaller::CoercedPath:
        return "coercedPath";
    case FetchToStoreCaller::Devirtualize:
        return "devirtualize";
    case FetchToStoreCaller::BuiltinsPath:
        return "builtinsPath";
    case FetchToStoreCaller::TectonixZone:
        return "tectonixZone";
    case FetchToStoreCaller::TectonixTree:
        return "tectonixTree";
    case FetchToStoreCaller::Other:
    case FetchToStoreCaller::Count:
        return "other";
    }
    unreachable();
}

FetchToStoreCallerScope::FetchToStoreCallerScope(FetchToStoreCaller caller)
    : previous(currentFetchToStoreCaller)
{
    currentFetchToStoreCaller = caller;
}

FetchToStoreCallerScope::~FetchToStoreCallerScope()
{
    currentFetchToStoreCaller = previous;
}

std::vector<FetchToStoreStats> getFetchToStoreStats()
{
    std::vector<FetchToStoreStats> out(nFetchToStoreCallers);
    for (size_t i = 0; i < nFetchToStoreCallers; ++i) {
        auto & s = fetchToStoreStats[i];
        out[i] = FetchToStoreStats{
            .calls = s.calls.load(std::memory_order_relaxed),
            .memoryCacheHits = s.memoryCacheHits.load(std::memory_order_relaxed),
            .persistentCacheHits = s.persistentCacheHits.load(std::memory_order_relaxed),
            .ingestions = s.ingestions.load(std::memory_order_relaxed),
            .dryRunIngestions = s.dryRunIngestions.load(std::memory_order_relaxed),
            .filteredIngestions = s.filteredIngestions.load(std::memory_order_relaxed),
            .bytesIngested = s.bytesIngested.load(std::memory_order_relaxed),
            .secondsIngesting = static_cast<double>(s.nanosIngesting.load(std::memory_order_relaxed)) / 1e9,
            .secondsFiltering = static_cast<double>(s.nanosFiltering.load(std::memory_order_relaxed)) / 1e9,
        };
    }
    return out;
}

FetchToStoreThreadTotals getFetchToStoreThreadTotals()
{
    return fetchToStoreThreadTotals;
}

StorePath fetchToStore(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name,
    ContentAddressMethod method,
    PathFilter * filter,
    RepairFlag repair)
{
    return fetchToStore2(settings, store, path, mode, name, method, filter, repair).first;
}

std::pair<StorePath, Hash> fetchToStore2(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name,
    ContentAddressMethod method,
    PathFilter * filter,
    RepairFlag repair)
{
    // Always call getFingerprint, even when a filter is present or the
    // in-memory srcToStore cache below may hit, so source accessors can
    // record the access: a cache hit absorbs the physical read, and skipping
    // recording here would silently drop the path from the dependency
    // closure of any tracked evaluation after the first.
    auto & stats = fetchToStoreStats[static_cast<size_t>(currentFetchToStoreCaller)];
    if (statsEnabled)
        bump(stats.calls);

    auto [subpath, fingerprint] = path.accessor->getFingerprint(path.path);

    auto srcToStoreKey = std::make_tuple(path, method.raw, std::string(name));

    if (!filter) {
        auto dstPathCached = getConcurrent(settings.srcToStore->cache, srcToStoreKey);
        if (dstPathCached && (mode == FetchMode::DryRun || std::get<2>(*dstPathCached) == FetchMode::Copy)) {
            if (statsEnabled)
                bump(stats.memoryCacheHits);
            return std::make_pair(std::get<0>(*dstPathCached), std::get<1>(*dstPathCached));
        }
    }

    std::optional<fetchers::Cache::Key> cacheKey;

    // Do not persistently cache filtered paths: the filter predicate is not
    // part of the cache key.
    if (fingerprint && !filter) {
        cacheKey = makeSourcePathToHashCacheKey(*fingerprint, method, subpath);
        if (auto res = settings.getCache()->lookup(*cacheKey)) {
            auto hash = Hash::parseSRI(fetchers::getStrAttr(*res, "hash"));
            auto storePath =
                store.makeFixedOutputPathFromCA(name, ContentAddressWithReferences::fromParts(method, hash, {}));

            /* Add a temproot before the call to isValidPath to prevent accidental GC in case the
               input is cached. Note that this must be done before to avoid races. */
            if (mode != FetchMode::DryRun)
                store.addTempRoot(storePath);

            if (mode == FetchMode::DryRun || store.maybeQueryPathInfo(storePath)) {
                debug(
                    "source path '%s' cache hit in '%s' (hash '%s')",
                    path,
                    store.printStorePath(storePath),
                    hash.to_string(HashFormat::SRI, true));
                settings.srcToStore->cache.insert_or_assign(srcToStoreKey, std::make_tuple(storePath, hash, mode));
                if (statsEnabled)
                    bump(stats.persistentCacheHits);
                return {storePath, hash};
            }
            debug("source path '%s' not in store", path);
        }
    } else if (filter) {
        debug("source path '%s' has a filter; skipping persistent source-path cache", path);
    } else {
        static auto barf = getEnv("_NIX_TEST_BARF_ON_UNCACHEABLE").value_or("") == "1";
        if (barf && !(path.to_string().starts_with("/") || path.to_string().starts_with("«path:/")))
            throw Error("source path '%s' is uncacheable (filter=%d)", path, (bool) filter);
        debug("source path '%s' is uncacheable", path);
    }

    Activity act(
        *logger,
        lvlChatty,
        actUnknown,
        fmt(mode == FetchMode::DryRun ? "hashing '%s'" : "copying '%s' to the store", path));

    PathFilter filter2 = filter ? *filter : defaultPathFilter;

    // Time the filter callback separately: for `builtins.path` it is Nix
    // evaluation (and may itself ingest), not hashing or copying.
    uint64_t filterNanos = 0;
    if (statsEnabled && filter)
        filter2 = [&, inner = std::move(filter2)](const std::string & p) {
            auto filterStart = std::chrono::steady_clock::now();
            bool keep = inner(p);
            filterNanos += nanosSince(filterStart);
            return keep;
        };

    auto ingestStart = statsEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    auto [storePath, hash] =
        mode == FetchMode::DryRun
            ? [&]() {
                  auto [storePath, hash] =
                      store.computeStorePath(name, path, method, HashAlgorithm::SHA256, {}, filter2);
                  debug(
                      "hashed '%s' to '%s' (hash '%s')",
                      path,
                      store.printStorePath(storePath),
                      hash.to_string(HashFormat::SRI, true));
                  return std::make_pair(storePath, hash);
              }()
            : [&]() {
                  // FIXME: ideally addToStore() would return the hash
                  // right away (like computeStorePath()).
                  auto storePath = store.addToStore(name, path, method, HashAlgorithm::SHA256, {}, filter2, repair);
                  auto info = store.queryPathInfo(storePath);
                  if (statsEnabled) {
                      bump(stats.bytesIngested, info->narSize);
                      fetchToStoreThreadTotals.bytesIngested += info->narSize;
                  }
                  assert(info->references.empty());
                  auto hash = method == ContentAddressMethod::Raw::NixArchive ? info->narHash : ({
                      if (!info->ca || info->ca->method != method)
                          throw Error("path '%s' lacks a CA field", store.printStorePath(storePath));
                      info->ca->hash;
                  });
                  printMsg(
                      lvlChatty,
                      "copied source '%s' -> '%s' (hash '%s')",
                      path,
                      store.printStorePath(storePath),
                      hash.to_string(HashFormat::SRI, true));
                  return std::make_pair(storePath, hash);
              }();

    if (statsEnabled) {
        bump(stats.ingestions);
        if (mode == FetchMode::DryRun)
            bump(stats.dryRunIngestions);
        if (filter)
            bump(stats.filteredIngestions);
        // The filter ran inside the window measured above: report it on its own.
        auto nanos = nanosSince(ingestStart) - filterNanos;
        bump(stats.nanosIngesting, nanos);
        bump(stats.nanosFiltering, filterNanos);
        fetchToStoreThreadTotals.ingestions++;
        fetchToStoreThreadTotals.nanosIngesting += nanos;
    }

    if (cacheKey)
        settings.getCache()->upsert(*cacheKey, {{"hash", hash.to_string(HashFormat::SRI, true)}});

    if (!filter)
        settings.srcToStore->cache.insert_or_assign(srcToStoreKey, std::make_tuple(storePath, hash, mode));

    return {storePath, hash};
}

} // namespace nix
