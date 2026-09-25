#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/environment-variables.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

#include <atomic>
#include <chrono>

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

struct AtomicFetchToStoreStats
{
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> memoryCacheHits{0};
    std::atomic<uint64_t> persistentCacheHits{0};
    std::atomic<uint64_t> ingestions{0};
    std::atomic<uint64_t> dryRunIngestions{0};
    std::atomic<uint64_t> filteredIngestions{0};
    std::atomic<uint64_t> bytesCopied{0};
    std::atomic<uint64_t> nanosIngesting{0};
};

constexpr size_t nFetchToStoreCallers = static_cast<size_t>(FetchToStoreCaller::Count);

AtomicFetchToStoreStats fetchToStoreStats[nFetchToStoreCallers];

thread_local FetchToStoreCaller currentFetchToStoreCaller = FetchToStoreCaller::Other;

} // namespace

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
            .calls = s.calls.load(),
            .memoryCacheHits = s.memoryCacheHits.load(),
            .persistentCacheHits = s.persistentCacheHits.load(),
            .ingestions = s.ingestions.load(),
            .dryRunIngestions = s.dryRunIngestions.load(),
            .filteredIngestions = s.filteredIngestions.load(),
            .bytesCopied = s.bytesCopied.load(),
            .secondsIngesting = static_cast<double>(s.nanosIngesting.load()) / 1e9,
        };
    }
    return out;
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
    stats.calls++;

    auto [subpath, fingerprint] = path.accessor->getFingerprint(path.path);

    auto srcToStoreKey = std::make_tuple(path, method.raw, std::string(name));

    if (!filter) {
        auto dstPathCached = getConcurrent(settings.srcToStore->cache, srcToStoreKey);
        if (dstPathCached && (mode == FetchMode::DryRun || std::get<2>(*dstPathCached) == FetchMode::Copy)) {
            stats.memoryCacheHits++;
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
                stats.persistentCacheHits++;
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

    auto filter2 = filter ? *filter : defaultPathFilter;

    auto ingestStart = std::chrono::steady_clock::now();

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
                  stats.bytesCopied += info->narSize;
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

    stats.ingestions++;
    if (mode == FetchMode::DryRun)
        stats.dryRunIngestions++;
    if (filter)
        stats.filteredIngestions++;
    stats.nanosIngesting += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ingestStart).count());

    if (cacheKey)
        settings.getCache()->upsert(*cacheKey, {{"hash", hash.to_string(HashFormat::SRI, true)}});

    if (!filter)
        settings.srcToStore->cache.insert_or_assign(srcToStoreKey, std::make_tuple(storePath, hash, mode));

    return {storePath, hash};
}

} // namespace nix
