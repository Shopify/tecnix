#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetch-to-store.hh"

#include <nlohmann/json.hpp>

namespace nix {

// ============================================================================
// Zone Path Parsing Infrastructure
// ============================================================================

/**
 * Result of peeling a zone path into host and local components.
 *
 * For top-level zones like "//a/b/c", hostPath is nullopt and localPath is "//a/b/c".
 * For internal zones like "//a/b/_internal/c", hostPath is "//a/b" and localPath is "c".
 * For nested internal zones like "//a/_internal/b/_internal/c", hostPath is "//a/_internal/b" and localPath is "c".
 */
struct PeeledZonePath {
    std::optional<std::string> hostPath;  // nullopt for top-level zones
    std::string localPath;                 // The path to look up in manifest

    bool isInternal() const { return hostPath.has_value(); }
};

/**
 * Peel a zone path to extract the innermost internal zone layer.
 *
 * Uses rfind to find the rightmost "/_internal/" marker:
 * - peel("//a/b/c") → {nullopt, "//a/b/c"} — top-level
 * - peel("//a/b/_internal/c") → {"//a/b", "c"} — one level of nesting
 * - peel("//a/_internal/b/_internal/c") → {"//a/_internal/b", "c"} — recursive host
 */
static PeeledZonePath peelZonePath(std::string_view path) {
    constexpr std::string_view marker = "/_internal/";
    auto pos = path.rfind(marker);

    if (pos == std::string_view::npos) {
        return {.hostPath = std::nullopt, .localPath = std::string(path)};
    }

    return {
        .hostPath = std::string(path.substr(0, pos)),
        .localPath = std::string(path.substr(pos + marker.size()))
    };
}

// ============================================================================
// Internal Manifest Reading
// ============================================================================

/**
 * Read the internal manifest from a host zone's tree.
 *
 * Internal manifests are located at `_internal/manifest.json` within the host zone
 * and contain relative paths (no // prefix) mapping to zone IDs.
 *
 * Example internal manifest:
 * {
 *   "helpers": {"id": "W-def000"},
 *   "test-utils": {"id": "W-def001"},
 *   "deeply/nested/thing": {"id": "W-def002"}
 * }
 *
 * @param state The evaluation state
 * @param hostTreeSha The tree SHA of the host zone
 * @return The parsed internal manifest JSON, or nullopt if no internal manifest exists
 */
static std::optional<nlohmann::json> readInternalManifest(
    EvalState & state,
    const Hash & hostTreeSha)
{
    auto repo = state.getWorldRepo();
    GitAccessorOptions opts{.exportIgnore = false, .smudgeLfs = false};
    auto accessor = repo->getAccessor(hostTreeSha, opts, "host");

    auto manifestPath = CanonPath("_internal/manifest.json");
    if (!accessor->pathExists(manifestPath))
        return std::nullopt;

    return nlohmann::json::parse(accessor->readFile(manifestPath));
}

// Helper to read the manifest JSON content
static std::string readManifestContent(EvalState & state, const PosIdx pos)
{
    auto fullPath = CanonPath("/.meta/manifest.json");

    // In source-available mode, check checkout first
    if (state.isTectonixSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.tectonixCheckoutPath.get();
            auto checkoutFullPath = CanonPath(checkoutPath + fullPath.abs());
            if ((*checkoutAccessor)->pathExists(checkoutFullPath)) {
                return (*checkoutAccessor)->readFile(checkoutFullPath);
            }
        }
    }

    // Fall back to git
    auto accessor = state.getWorldGitAccessor();
    if (!accessor->pathExists(fullPath))
        state.error<EvalError>("manifest.json does not exist at //.meta/manifest.json in world")
            .atPos(pos).debugThrow();

    return accessor->readFile(fullPath);
}

// ============================================================================
// builtins.worldManifest
// Returns path -> zoneId mapping from //.meta/manifest.json
// ============================================================================
static void prim_worldManifest(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto content = readManifestContent(state, pos);
    auto json = nlohmann::json::parse(content);

    auto attrs = state.buildBindings(json.size());
    for (auto & [path, value] : json.items()) {
        auto & id = value.at("id");
        attrs.alloc(state.symbols.create(path)).mkString(id.get<std::string>(), state.mem);
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_worldManifest({
    .name = "__unsafeTectonixInternalManifest",
    .args = {},
    .doc = R"(
      Get the world manifest as a Nix attrset mapping zone paths to zone IDs.

      Example: `builtins.unsafeTectonixInternalManifest."//areas/tools/dev"` returns `"W-123456"`.

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_worldManifest,
});

// ============================================================================
// builtins.worldManifestInverted
// Returns zoneId -> path mapping (inverse of worldManifest)
// ============================================================================
static void prim_worldManifestInverted(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto content = readManifestContent(state, pos);
    auto json = nlohmann::json::parse(content);

    auto attrs = state.buildBindings(json.size());
    for (auto & [path, value] : json.items()) {
        auto & id = value.at("id");
        attrs.alloc(state.symbols.create(id.get<std::string>())).mkString(path, state.mem);
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_worldManifestInverted({
    .name = "__unsafeTectonixInternalManifestInverted",
    .args = {},
    .doc = R"(
      Get the inverted world manifest as a Nix attrset mapping zone IDs to zone paths.

      Example: `builtins.unsafeTectonixInternalManifestInverted."W-123456"` returns `"//areas/tools/dev"`.

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_worldManifestInverted,
});

// ============================================================================
// builtins.unsafeTectonixInternalTreeSha worldPath
// Returns the git tree SHA for a world path
// ============================================================================
static void prim_unsafeTectonixInternalTreeSha(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto worldPath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'worldPath' argument to builtins.unsafeTectonixInternalTreeSha");

    auto sha = state.getWorldTreeSha(worldPath);
    v.mkString(sha.gitRev(), state.mem);
}

static RegisterPrimOp primop_unsafeTectonixInternalTreeSha({
    .name = "__unsafeTectonixInternalTreeSha",
    .args = {"worldPath"},
    .doc = R"(
      Get the git tree SHA for a path in the world repository.

      Example: `builtins.unsafeTectonixInternalTreeSha "//areas/tools/tec"` returns the tree SHA
      for that zone.

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_unsafeTectonixInternalTreeSha,
});

// ============================================================================
// builtins.unsafeTectonixInternalTree treeSha
// Returns a store path containing the tree contents
// ============================================================================
static void prim_unsafeTectonixInternalTree(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto treeSha = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'treeSha' argument to builtins.unsafeTectonixInternalTree");

    auto repo = state.getWorldRepo();
    auto hash = Hash::parseNonSRIUnprefixed(treeSha, HashAlgorithm::SHA1);

    if (!repo->hasObject(hash))
        state.error<EvalError>("tree SHA '%s' not found in world repository", treeSha)
            .atPos(pos).debugThrow();

    GitAccessorOptions opts{.exportIgnore = false, .smudgeLfs = false};
    auto accessor = repo->getAccessor(hash, opts, "world-tree");

    auto storePath = fetchToStore(
        state.fetchSettings,
        *state.store,
        SourcePath(accessor, CanonPath::root),
        FetchMode::Copy,
        "world-tree-" + std::string(treeSha).substr(0, 8));

    state.allowAndSetStorePathString(storePath, v);
}

static RegisterPrimOp primop_unsafeTectonixInternalTree({
    .name = "__unsafeTectonixInternalTree",
    .args = {"treeSha"},
    .doc = R"(
      Fetch a git tree by SHA from the world repository and return it as a store path.

      Example: `builtins.unsafeTectonixInternalTree "abc123..."` returns `/nix/store/...-world-tree-abc123`.

      Requires `--tectonix-git-dir` to be set.
    )",
    .fun = prim_unsafeTectonixInternalTree,
});

// ============================================================================
// builtins.unsafeTectonixInternalZoneSrc zonePath
// Returns a store path containing the zone source
// With lazy-trees enabled, returns a virtual store path that is only
// materialized when used as a derivation input.
// ============================================================================
static void prim_unsafeTectonixInternalZoneSrc(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.unsafeTectonixInternalZoneSrc");

    auto storePath = state.getZoneStorePath(zonePath);
    state.allowAndSetStorePathString(storePath, v);
}

static RegisterPrimOp primop_unsafeTectonixInternalZoneSrc({
    .name = "__unsafeTectonixInternalZoneSrc",
    .args = {"zonePath"},
    .doc = R"(
      Get the source of a zone as a store path.

      With `lazy-trees = true`, returns a virtual store path that is only
      materialized when used as a derivation input (devirtualized).

      In source-available mode with uncommitted changes, uses checkout content
      (always eager for dirty zones).

      Example: `builtins.unsafeTectonixInternalZoneSrc "//areas/tools/tec"`

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_unsafeTectonixInternalZoneSrc,
});

// ============================================================================
// builtins.unsafeTectonixInternalSparseCheckoutRoots
// Returns list of zone IDs in sparse checkout
// ============================================================================
static void prim_unsafeTectonixInternalSparseCheckoutRoots(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & roots = state.getTectonixSparseCheckoutRoots();

    auto list = state.buildList(roots.size());
    size_t i = 0;
    for (const auto & root : roots) {
        (list[i++] = state.allocValue())->mkString(root, state.mem);
    }
    v.mkList(list);
}

static RegisterPrimOp primop_unsafeTectonixInternalSparseCheckoutRoots({
    .name = "__unsafeTectonixInternalSparseCheckoutRoots",
    .args = {},
    .doc = R"(
      Get the list of zone IDs that are in the sparse checkout.

      Returns an empty list if not in source-available mode or if no
      sparse-checkout-roots file exists.

      Example: `builtins.unsafeTectonixInternalSparseCheckoutRoots` returns `["W-000000" "W-1337af" ...]`.

      Requires `--tectonix-checkout-path` to be set.
    )",
    .fun = prim_unsafeTectonixInternalSparseCheckoutRoots,
});

// ============================================================================
// builtins.unsafeTectonixInternalDirtyZones
// Returns map of zone paths to dirty status
// ============================================================================
static void prim_unsafeTectonixInternalDirtyZones(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & dirtyZones = state.getTectonixDirtyZones();

    auto attrs = state.buildBindings(dirtyZones.size());
    for (const auto & [zonePath, dirty] : dirtyZones) {
        attrs.alloc(state.symbols.create(zonePath)).mkBool(dirty);
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_unsafeTectonixInternalDirtyZones({
    .name = "__unsafeTectonixInternalDirtyZones",
    .args = {},
    .doc = R"(
      Get the dirty status of zones in the sparse checkout.

      Returns an attrset mapping zone paths to booleans indicating whether
      the zone has uncommitted changes.

      Only includes zones that are in the sparse checkout.

      Example: `builtins.unsafeTectonixInternalDirtyZones."//areas/tools/dev"` returns `true` or `false`.

      Requires `--tectonix-checkout-path` to be set.
    )",
    .fun = prim_unsafeTectonixInternalDirtyZones,
});

// ============================================================================
// builtins.__unsafeTectonixInternalZone zonePath
// Returns an attrset with zone info (flake-like interface)
// ============================================================================
static void prim_unsafeTectonixInternalZone(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.__unsafeTectonixInternalZone");

    // Peel the zone path to determine if it's top-level or internal
    auto peeled = peelZonePath(zonePath);

    // Validate that zonePath exists in the appropriate manifest
    if (!peeled.isInternal()) {
        // Top-level zone: check root manifest
        auto content = readManifestContent(state, pos);
        auto manifest = nlohmann::json::parse(content);
        if (!manifest.contains(std::string(zonePath)))
            state.error<EvalError>("'%s' is not a zone root (must be an exact path from the manifest)", zonePath)
                .atPos(pos).debugThrow();
    } else {
        // Internal zone: resolve host zone and check its internal manifest
        auto hostTreeSha = state.getWorldTreeSha(*peeled.hostPath);
        auto internalManifest = readInternalManifest(state, hostTreeSha);

        if (!internalManifest)
            state.error<EvalError>("zone '%s' has no internal manifest", *peeled.hostPath)
                .atPos(pos).debugThrow();

        if (!internalManifest->contains(peeled.localPath))
            state.error<EvalError>("'%s' is not an internal zone of '%s'",
                peeled.localPath, *peeled.hostPath)
                .atPos(pos).debugThrow();
    }

    // Get tree SHA (handles recursion internally)
    auto treeSha = state.getWorldTreeSha(zonePath);

    // Check dirty status
    bool isDirty = false;
    if (state.isTectonixSourceAvailable()) {
        auto & dirtyZones = state.getTectonixDirtyZones();
        auto it = dirtyZones.find(std::string(zonePath));
        isDirty = it != dirtyZones.end() && it->second;
    }

    auto storePath = state.getZoneStorePath(zonePath);
    auto storePathStr = state.store->printStorePath(storePath);

    // Build result attrset (like fetchTree)
    auto attrs = state.buildBindings(5);

    // outPath: string with context (for use as derivation src)
    attrs.alloc("outPath").mkString(storePathStr, {
        NixStringContextElem::Opaque{storePath}
    }, state.mem);

    // root: path value (for reading files without devirtualization)
    attrs.alloc("root").mkPath(
        state.rootPath(CanonPath(storePathStr)), state.mem);

    attrs.alloc("treeSha").mkString(treeSha.gitRev(), state.mem);
    attrs.alloc("zonePath").mkString(zonePath, state.mem);
    attrs.alloc("dirty").mkBool(isDirty);

    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_unsafeTectonixInternalZone({
    .name = "__unsafeTectonixInternalZone",
    .args = {"zonePath"},
    .doc = R"(
      Get a zone from the world repository.

      Returns an attrset with:
      - outPath: Store path string with context (for use as derivation src)
      - root: Path value for reading files (no devirtualization)
      - treeSha: Git tree SHA for this zone
      - zonePath: The zone path argument
      - dirty: Whether the zone has uncommitted changes

      With `lazy-trees = true`, the zone is mounted lazily. Use `root` to
      read files without triggering a copy to the store:

          let zone = builtins.__unsafeTectonixInternalZone "//areas/tools/tec";
          in import (zone.root + "/zone.nix")

      Use `outPath` as derivation src (triggers copy at build time):

          mkDerivation { src = zone.outPath; }

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_unsafeTectonixInternalZone,
});

} // namespace nix
