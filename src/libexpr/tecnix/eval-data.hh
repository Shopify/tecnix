#pragma once
///@file

#include "nix/expr/eval.hh"
#include "nix/expr/root-value.hh"
#include "nix/expr/tecnix/access-set-graph.hh"

#include <boost/container_hash/hash.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

struct ParsedFileCacheEntry;
using EvalParsedFileCache = boost::concurrent_flat_map<SourcePath, std::shared_ptr<ParsedFileCacheEntry>>;

struct EvalImportResolutionCacheEntry
{
    SourcePath resolvedPath;
    EvalSourceAccessSetId sourceDeps = emptyEvalSourceAccessSetId;
};

using EvalImportResolutionCache = boost::concurrent_flat_map<SourcePath, EvalImportResolutionCacheEntry>;
using EvalFileCache = boost::concurrent_flat_map<SourcePath, RootValue>;
using EvalWorldTreeShaCache = boost::concurrent_flat_map<std::string, Hash>;

/**
 * A resolver module applied to `args`, together with the source-access set
 * recorded while applying it. The label has to be kept alongside the value:
 * the accesses happen once, when the module is first built, but every tracking
 * context that reuses the module must still record them, or the resolver file
 * itself silently drops out of the dependency closure it is supposed to
 * invalidate.
 */
struct EvalTecnixModuleCacheEntry
{
    RootValue value;
    EvalSourceAccessSetId sourceDeps = emptyEvalSourceAccessSetId;
};

using EvalTecnixModuleCache = boost::concurrent_flat_map<std::string, EvalTecnixModuleCacheEntry>;

/**
 * A `builtins.tecnixMemoize` table key: the memoized function, identified by
 * the address of its value, and the key it was called with. `Lookup` is the
 * non-owning form, so a hit does not copy the key.
 */
struct EvalTecnixMemoizeKey
{
    const Value * function;
    std::string key;

    struct Lookup
    {
        const Value * function;
        std::string_view key;
    };

    struct Hash
    {
        using is_transparent = void;

        std::size_t operator()(const Lookup & k) const noexcept
        {
            std::size_t seed = 0;
            boost::hash_combine(seed, k.function);
            boost::hash_combine(seed, k.key);
            return seed;
        }

        std::size_t operator()(const EvalTecnixMemoizeKey & k) const noexcept
        {
            return (*this)(Lookup{k.function, k.key});
        }
    };

    struct Equal
    {
        using is_transparent = void;

        bool operator()(const auto & a, const auto & b) const noexcept
        {
            return a.function == b.function && a.key == b.key;
        }
    };
};

/**
 * A memoized `builtins.tecnixMemoize` result: the shared finished value of
 * `f k` plus, in the tracked table, the source-access-set label recorded the
 * one time it was evaluated. Every later tracked consumer records
 * `sourceDeps` as a child of its own frame (and inherits the value's label
 * through the normal value-copy hooks), so the shared value's tracked
 * dependencies propagate to all consumers without re-evaluating `f k`.
 */
struct EvalTecnixMemoizeEntry
{
    /** Roots `f`, so that while this entry exists no other value can be
        allocated at `f`'s address and inherit the entry. */
    RootValue function;
    RootValue value;
    EvalSourceAccessSetId sourceDeps = emptyEvalSourceAccessSetId;
};

using EvalTecnixMemoizeCache = boost::concurrent_flat_map<
    EvalTecnixMemoizeKey,
    EvalTecnixMemoizeEntry,
    EvalTecnixMemoizeKey::Hash,
    EvalTecnixMemoizeKey::Equal>;

struct EvalState::TecnixEvalData
{
    /**
     * A cache that maps paths to "resolved" paths for importing Nix
     * expressions, i.e. `/foo` to `/foo/default.nix`.
     */
    const ref<EvalImportResolutionCache> importResolutionCache = make_ref<EvalImportResolutionCache>();

    /**
     * Shared import-resolution cache for tracked Tecnix evaluation. Entries also
     * carry the source-deps label recorded while resolving so cache hits can
     * replay symlink/default-resolution provenance into each target.
     */
    const ref<EvalImportResolutionCache> trackedImportResolutionCache = make_ref<EvalImportResolutionCache>();

    /**
     * A cache from resolved paths to parsed expressions. This is safe to share
     * across tracking contexts because evaluated values/thunks remain isolated.
     */
    const ref<EvalParsedFileCache> parsedFileCache = make_ref<EvalParsedFileCache>();

    /**
     * Canonical flat source access-set graph for Tecnix dependency tracking.
     */
    const ref<EvalSourceAccessSetGraph> sourceAccessSetGraph = make_ref<EvalSourceAccessSetGraph>();

    /**
     * Shared evaluated-file cache for tracked Tecnix dependency discovery.
     * Kept separate from the normal evaluator cache so untracked eval cannot
     * prewarm entries that lack source-deps labels.
     */
    const ref<EvalFileCache> trackedFileEvalCache = make_ref<EvalFileCache>();

    struct TectonixContext
    {
        std::string gitDir;
        std::string rev;
        std::string checkoutPath;
    };

    mutable std::mutex tectonixContextMutex;
    mutable std::optional<TectonixContext> tectonixContext;

    /** Lazy-initialized git repository for world builtins (thread-safe via once_flag) */
    mutable std::once_flag worldRepoFlag;
    mutable std::optional<ref<GitRepo>> worldRepo;

    /** Lazy-initialized source accessor for world git content (thread-safe via once_flag) */
    mutable std::once_flag worldGitAccessorFlag;
    mutable std::optional<ref<SourceAccessor>> worldGitAccessor;

    /**
     * Repo-wide source accessor with dirty overlay. Lazily created.
     * All file reads during Tecnix evaluation go through this single accessor,
     * so tracked paths are naturally repo-relative.
     */
    mutable std::once_flag tecnixRepoAccessorFlag;
    mutable std::optional<ref<SourceAccessor>> tecnixRepoAccessor;

    /**
     * Virtual store path where the Tecnix repo-wide accessor is lazily mounted.
     * All repo subtree store paths are subpaths of this mount.
     */
    mutable std::once_flag tecnixRepoMountFlag;
    mutable std::optional<StorePath> tecnixRepoMountStorePath;

    /** Cache: world path → tree SHA (lazy computed, cached at each path level) */
    const ref<EvalWorldTreeShaCache> worldTreeShaCache = make_ref<EvalWorldTreeShaCache>();

    /**
     * The resolver module applied to `args`, keyed by resolver path and the
     * canonical `args` encoding.
     *
     * Every Tecnix builtin needs the same thing: the resolver file imported and
     * called with `args`. Importing is already cached, but the *application*
     * was not, so each builtin got its own copy of the returned attrset and
     * therefore its own unevaluated copy of everything hanging off it. A run
     * that discovers target names and then resolves them consequently walked
     * the zone graph twice, which measured as ~24% more thunks and function
     * calls than resolving alone.
     *
     * `argsKey` is a canonical, injective encoding of `args` (see
     * `canonicalJsonFromValue`), so an equal key means the resolver would be
     * applied to an equal value and the result is interchangeable. Sharing the
     * applied module across tracking contexts is sound for the same reason
     * sharing `trackedFileEvalCache` is: whichever context first forces a thunk
     * publishes its source-access label onto the finished value, and a later
     * force in another context picks that label up via `forceValueTracked`.
     */
    const ref<EvalTecnixModuleCache> tecnixModuleCache = make_ref<EvalTecnixModuleCache>();

    /**
     * Memoization tables for `builtins.tecnixMemoize`, keyed by the memoized
     * function and the key. Each entry holds the single shared result of
     * `f k` and, in the tracked table, its recorded source-access-set label,
     * so the (potentially expensive) `f k` evaluation runs once per EvalState
     * and every tracked consumer inherits the same tracked dependencies.
     * Tracked and untracked calls use separate tables, like
     * `trackedFileEvalCache` and `fileEvalCache`, so a result never crosses
     * between them. Misses evaluate outside any bucket lock, so re-entrant
     * misses (zone A loading zone B) never deadlock.
     */
    const ref<EvalTecnixMemoizeCache> tecnixMemoizeCache = make_ref<EvalTecnixMemoizeCache>();
    const ref<EvalTecnixMemoizeCache> trackedTecnixMemoizeCache = make_ref<EvalTecnixMemoizeCache>();

    /** Lazy-initialized set of zone IDs in sparse checkout (thread-safe via once_flag) */
    mutable std::once_flag tectonixSparseCheckoutRootsFlag;
    mutable std::set<std::string> tectonixSparseCheckoutRoots;

    /** Lazy-initialized map of zone path → dirty info (thread-safe via once_flag) */
    mutable std::once_flag tectonixDirtyZonesFlag;
    mutable std::map<std::string, ZoneDirtyInfo> tectonixDirtyZones;

    /** Cached manifest content (thread-safe via once_flag) */
    mutable std::once_flag tectonixManifestFlag;
    mutable std::string tectonixManifestContent;

    /** Cached parsed manifest JSON (thread-safe via once_flag) */
    mutable std::once_flag tectonixManifestJsonFlag;
    mutable std::unique_ptr<nlohmann::json> tectonixManifestJson;

    /**
     * Cache tree SHA → virtual store path for lazy zone mounts.
     * Thread-safe for eval-cores > 1.
     */
    mutable SharedSync<std::map<Hash, StorePath>> tectonixZoneCache_;

    /**
     * Cache zone path → virtual store path for lazy checkout zone mounts.
     * Thread-safe for eval-cores > 1.
     */
    mutable SharedSync<std::map<std::string, StorePath>> tectonixCheckoutZoneCache_;

    /**
     * Lazily-connected worldtree daemon control connection (zone tree SHAs +
     * dirty set), or null when the socket is unset or the evaluation targets an
     * immutable historical FUSE view rather than the mutable root checkout.
     */
    mutable std::once_flag worldtreeControlConnFlag;
    mutable std::shared_ptr<WorldtreeConn> worldtreeControlConn_;
};

} // namespace nix
