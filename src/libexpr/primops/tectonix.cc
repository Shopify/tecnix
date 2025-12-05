#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetch-to-store.hh"

#include <nlohmann/json.hpp>

namespace nix {

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
// builtins.unsafeTectonixInternalFile path
// Returns file contents as a string
// ============================================================================
static void prim_unsafeTectonixInternalFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto worldPath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'path' argument to builtins.unsafeTectonixInternalFile");

    // Normalize path (remove leading //)
    std::string path(worldPath);
    if (hasPrefix(path, "//"))
        path = path.substr(2);

    auto fullPath = CanonPath("/" + path);

    // In source-available mode, check checkout first
    if (state.isTectonixSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.tectonixCheckoutPath.get();
            auto checkoutFullPath = CanonPath(checkoutPath + fullPath.abs());
            if ((*checkoutAccessor)->pathExists(checkoutFullPath)) {
                auto content = (*checkoutAccessor)->readFile(checkoutFullPath);
                v.mkString(content, state.mem);
                return;
            }
        }
    }

    // Fall back to git
    auto accessor = state.getWorldGitAccessor();
    if (!accessor->pathExists(fullPath))
        state.error<EvalError>("path '%s' does not exist in world", fullPath)
            .atPos(pos).debugThrow();

    auto content = accessor->readFile(fullPath);
    v.mkString(content, state.mem);
}

static RegisterPrimOp primop_unsafeTectonixInternalFile({
    .name = "__unsafeTectonixInternalFile",
    .args = {"path"},
    .doc = R"(
      Read a file from the world repository.

      In source-available mode (--tectonix-checkout-path set), prefers checkout files.

      Example: `builtins.unsafeTectonixInternalFile "//areas/tools/tec/zone.nix"`

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_unsafeTectonixInternalFile,
});

// ============================================================================
// builtins.worldZoneFile zonePath pathInZone
// Returns file contents as a string
// ============================================================================
static void prim_worldZoneFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.worldZoneFile");
    auto pathInZone = state.forceStringNoCtx(*args[1], pos,
        "while evaluating the 'pathInZone' argument to builtins.worldZoneFile");

    // Normalize zone path (remove leading //)
    std::string zone(zonePath);
    if (hasPrefix(zone, "//"))
        zone = zone.substr(2);

    auto fullPath = CanonPath("/" + zone + "/" + std::string(pathInZone));

    // In source-available mode, check checkout first
    if (state.isTectonixSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.tectonixCheckoutPath.get();
            auto checkoutFullPath = CanonPath(checkoutPath + fullPath.abs());
            if ((*checkoutAccessor)->pathExists(checkoutFullPath)) {
                auto content = (*checkoutAccessor)->readFile(checkoutFullPath);
                v.mkString(content, state.mem);
                return;
            }
        }
    }

    // Fall back to git
    auto accessor = state.getWorldGitAccessor();
    if (!accessor->pathExists(fullPath))
        state.error<EvalError>("path '%s' does not exist in world", fullPath)
            .atPos(pos).debugThrow();

    auto content = accessor->readFile(fullPath);
    v.mkString(content, state.mem);
}

static RegisterPrimOp primop_worldZoneFile({
    .name = "worldZoneFile",
    .args = {"zonePath", "pathInZone"},
    .doc = R"(
      Read a file from a zone in the world repository.

      In source-available mode (--tectonix-checkout-path set), prefers checkout files.

      Example: `builtins.worldZoneFile "//areas/tools/tec" "zone.nix"`

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_worldZoneFile,
});

// ============================================================================
// builtins.unsafeTectonixInternalZoneSrc zonePath
// Returns a store path containing the zone source
// ============================================================================
static void prim_unsafeTectonixInternalZoneSrc(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.unsafeTectonixInternalZoneSrc");

    // Normalize zone path
    std::string zone(zonePath);
    if (hasPrefix(zone, "//"))
        zone = zone.substr(2);

    auto fullPath = CanonPath("/" + zone);
    std::string name = "zone-" + replaceStrings(zone, "/", "-");

    // In source-available mode with dirty zone, use checkout
    auto & dirtyZones = state.getTectonixDirtyZones();
    auto it = dirtyZones.find(std::string(zonePath));
    bool isDirty = it != dirtyZones.end() && it->second;
    if (state.isTectonixSourceAvailable() && isDirty) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (!checkoutAccessor)
            state.error<EvalError>("checkout accessor not available").atPos(pos).debugThrow();

        auto checkoutPath = state.settings.tectonixCheckoutPath.get();
        auto checkoutFullPath = CanonPath(checkoutPath + fullPath.abs());

        auto storePath = fetchToStore(
            state.fetchSettings,
            *state.store,
            SourcePath(*checkoutAccessor, checkoutFullPath),
            FetchMode::Copy,
            name);

        state.allowAndSetStorePathString(storePath, v);
    } else {
        // Use git content
        auto treeSha = state.getWorldTreeSha(zonePath);
        auto repo = state.getWorldRepo();
        GitAccessorOptions opts{.exportIgnore = true, .smudgeLfs = false};
        auto accessor = repo->getAccessor(treeSha, opts, "world-zone");

        auto storePath = fetchToStore(
            state.fetchSettings,
            *state.store,
            SourcePath(accessor, CanonPath::root),
            FetchMode::Copy,
            name);

        state.allowAndSetStorePathString(storePath, v);
    }
}

static RegisterPrimOp primop_unsafeTectonixInternalZoneSrc({
    .name = "__unsafeTectonixInternalZoneSrc",
    .args = {"zonePath"},
    .doc = R"(
      Get the source of a zone as a store path.

      In source-available mode with uncommitted changes, uses checkout content.
      Otherwise uses git content.

      Example: `builtins.unsafeTectonixInternalZoneSrc "//areas/tools/tec"`

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_unsafeTectonixInternalZoneSrc,
});

// ============================================================================
// builtins.unsafeTectonixInternalDir zonePath pathInZone
// Returns directory listing as attrset
// ============================================================================
static void prim_unsafeTectonixInternalDir(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.unsafeTectonixInternalDir");
    auto pathInZone = state.forceStringNoCtx(*args[1], pos,
        "while evaluating the 'pathInZone' argument to builtins.unsafeTectonixInternalDir");

    // Normalize path
    std::string zone(zonePath);
    if (hasPrefix(zone, "//"))
        zone = zone.substr(2);

    auto fullPath = CanonPath("/" + zone + "/" + std::string(pathInZone));

    // Determine which accessor to use
    ref<SourceAccessor> accessor = state.getWorldGitAccessor();
    CanonPath accessPath = fullPath;

    if (state.isTectonixSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.tectonixCheckoutPath.get();
            auto checkoutFullPath = CanonPath(checkoutPath + fullPath.abs());
            if ((*checkoutAccessor)->pathExists(checkoutFullPath)) {
                accessor = *checkoutAccessor;
                accessPath = checkoutFullPath;
            }
        }
    }

    if (!accessor->pathExists(accessPath))
        state.error<EvalError>("path '%s' does not exist in world", fullPath)
            .atPos(pos).debugThrow();

    auto entries = accessor->readDirectory(accessPath);

    auto attrs = state.buildBindings(entries.size());
    for (auto & [name, typeOpt] : entries) {
        const char * typeStr;
        if (!typeOpt) {
            typeStr = "unknown";
        } else {
            switch (*typeOpt) {
                case SourceAccessor::Type::tRegular: typeStr = "regular"; break;
                case SourceAccessor::Type::tDirectory: typeStr = "directory"; break;
                case SourceAccessor::Type::tSymlink: typeStr = "symlink"; break;
                case SourceAccessor::Type::tChar:
                case SourceAccessor::Type::tBlock:
                case SourceAccessor::Type::tSocket:
                case SourceAccessor::Type::tFifo:
                case SourceAccessor::Type::tUnknown:
                    typeStr = "unknown"; break;
            }
        }
        attrs.alloc(state.symbols.create(name)).mkString(typeStr, state.mem);
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_unsafeTectonixInternalDir({
    .name = "__unsafeTectonixInternalDir",
    .args = {"zonePath", "pathInZone"},
    .doc = R"(
      List directory contents from the world repository.

      Returns an attrset mapping names to types ("regular", "directory", "symlink").

      Example: `builtins.unsafeTectonixInternalDir "//areas/tools/tec" "src"`

      Requires `--tectonix-git-dir` and `--tectonix-git-sha` to be set.
    )",
    .fun = prim_unsafeTectonixInternalDir,
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

} // namespace nix
