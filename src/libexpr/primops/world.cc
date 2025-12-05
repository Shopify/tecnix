#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/json-to-value.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetch-to-store.hh"

namespace nix {

// ============================================================================
// builtins.worldManifest
// Returns the parsed manifest.json from //.meta
// ============================================================================
static void prim_worldManifest(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto fullPath = CanonPath("/.meta/manifest.json");

    // In source-available mode, check checkout first
    if (state.isWorldSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.worldCheckoutPath.get();
            auto checkoutFullPath = CanonPath(checkoutPath + fullPath.abs());
            if ((*checkoutAccessor)->pathExists(checkoutFullPath)) {
                auto content = (*checkoutAccessor)->readFile(checkoutFullPath);
                parseJSON(state, content, v);
                return;
            }
        }
    }

    // Fall back to git
    auto accessor = state.getWorldGitAccessor();
    if (!accessor->pathExists(fullPath))
        state.error<EvalError>("manifest.json does not exist at //.meta/manifest.json in world")
            .atPos(pos).debugThrow();

    auto content = accessor->readFile(fullPath);
    parseJSON(state, content, v);
}

static RegisterPrimOp primop_worldManifest({
    .name = "worldManifest",
    .args = {},
    .doc = R"(
      Get the world manifest as a Nix attrset.

      Reads and parses //.meta/manifest.json from the world repository.

      Requires `--world-git-dir` and `--world-sha` to be set.
    )",
    .fun = prim_worldManifest,
});

// ============================================================================
// builtins.worldTreeSha worldPath
// Returns the git tree SHA for a world path
// ============================================================================
static void prim_worldTreeSha(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto worldPath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'worldPath' argument to builtins.worldTreeSha");

    auto sha = state.getWorldTreeSha(worldPath);
    v.mkString(sha.gitRev(), state.mem);
}

static RegisterPrimOp primop_worldTreeSha({
    .name = "worldTreeSha",
    .args = {"worldPath"},
    .doc = R"(
      Get the git tree SHA for a path in the world repository.

      Example: `builtins.worldTreeSha "//areas/tools/tec"` returns the tree SHA
      for that zone.

      Requires `--world-git-dir` and `--world-sha` to be set.
    )",
    .fun = prim_worldTreeSha,
});

// ============================================================================
// builtins.worldTree treeSha
// Returns a store path containing the tree contents
// ============================================================================
static void prim_worldTree(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto treeSha = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'treeSha' argument to builtins.worldTree");

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

static RegisterPrimOp primop_worldTree({
    .name = "worldTree",
    .args = {"treeSha"},
    .doc = R"(
      Fetch a git tree by SHA from the world repository and return it as a store path.

      Example: `builtins.worldTree "abc123..."` returns `/nix/store/...-world-tree-abc123`.

      Requires `--world-git-dir` to be set.
    )",
    .fun = prim_worldTree,
});

// ============================================================================
// builtins.worldFile path
// Returns file contents as a string
// ============================================================================
static void prim_worldFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto worldPath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'path' argument to builtins.worldFile");

    // Normalize path (remove leading //)
    std::string path(worldPath);
    if (hasPrefix(path, "//"))
        path = path.substr(2);

    auto fullPath = CanonPath("/" + path);

    // In source-available mode, check checkout first
    if (state.isWorldSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.worldCheckoutPath.get();
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

static RegisterPrimOp primop_worldFile({
    .name = "worldFile",
    .args = {"path"},
    .doc = R"(
      Read a file from the world repository.

      In source-available mode (--world-checkout-path set), prefers checkout files.

      Example: `builtins.worldFile "//areas/tools/tec/zone.nix"`

      Requires `--world-git-dir` and `--world-sha` to be set.
    )",
    .fun = prim_worldFile,
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
    if (state.isWorldSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.worldCheckoutPath.get();
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

      In source-available mode (--world-checkout-path set), prefers checkout files.

      Example: `builtins.worldZoneFile "//areas/tools/tec" "zone.nix"`

      Requires `--world-git-dir` and `--world-sha` to be set.
    )",
    .fun = prim_worldZoneFile,
});

// ============================================================================
// builtins.worldZoneSrc zonePath
// Returns a store path containing the zone source
// ============================================================================
static void prim_worldZoneSrc(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.worldZoneSrc");

    // Normalize zone path
    std::string zone(zonePath);
    if (hasPrefix(zone, "//"))
        zone = zone.substr(2);

    auto fullPath = CanonPath("/" + zone);
    std::string name = "zone-" + replaceStrings(zone, "/", "-");

    // In source-available mode with dirty zone, use checkout
    if (state.isWorldSourceAvailable() && state.isZoneDirty(zonePath)) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (!checkoutAccessor)
            state.error<EvalError>("checkout accessor not available").atPos(pos).debugThrow();

        auto checkoutPath = state.settings.worldCheckoutPath.get();
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

static RegisterPrimOp primop_worldZoneSrc({
    .name = "worldZoneSrc",
    .args = {"zonePath"},
    .doc = R"(
      Get the source of a zone as a store path.

      In source-available mode with uncommitted changes, uses checkout content.
      Otherwise uses git content.

      Example: `builtins.worldZoneSrc "//areas/tools/tec"`

      Requires `--world-git-dir` and `--world-sha` to be set.
    )",
    .fun = prim_worldZoneSrc,
});

// ============================================================================
// builtins.worldDir zonePath pathInZone
// Returns directory listing as attrset
// ============================================================================
static void prim_worldDir(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos,
        "while evaluating the 'zonePath' argument to builtins.worldDir");
    auto pathInZone = state.forceStringNoCtx(*args[1], pos,
        "while evaluating the 'pathInZone' argument to builtins.worldDir");

    // Normalize path
    std::string zone(zonePath);
    if (hasPrefix(zone, "//"))
        zone = zone.substr(2);

    auto fullPath = CanonPath("/" + zone + "/" + std::string(pathInZone));

    // Determine which accessor to use
    ref<SourceAccessor> accessor = state.getWorldGitAccessor();
    CanonPath accessPath = fullPath;

    if (state.isWorldSourceAvailable()) {
        auto checkoutAccessor = state.getWorldCheckoutAccessor();
        if (checkoutAccessor) {
            auto checkoutPath = state.settings.worldCheckoutPath.get();
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

static RegisterPrimOp primop_worldDir({
    .name = "worldDir",
    .args = {"zonePath", "pathInZone"},
    .doc = R"(
      List directory contents from the world repository.

      Returns an attrset mapping names to types ("regular", "directory", "symlink").

      Example: `builtins.worldDir "//areas/tools/tec" "src"`

      Requires `--world-git-dir` and `--world-sha` to be set.
    )",
    .fun = prim_worldDir,
});

} // namespace nix
