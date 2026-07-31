#pragma once
///@file

#include "nix/expr/eval.hh"
#include "nix/expr/root-value.hh"
#include "nix/expr/tecnix/access-set-graph.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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
