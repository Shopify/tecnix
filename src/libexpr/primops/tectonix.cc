#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetch-to-store.hh"

#include <nlohmann/json.hpp>
#include <set>

namespace nix {

// Helper to get cached manifest JSON (avoids repeated parsing)
static const nlohmann::json & getManifest(EvalState & state)
{
    return getManifestJson(state);
}

// Helper to validate that a zone path exists in the manifest
static void validateZonePath(EvalState & state, const PosIdx pos, std::string_view zonePath)
{
    auto & manifest = getManifest(state);
    if (!manifest.contains(std::string(zonePath)))
        state.error<EvalError>("'%s' is not a zone root (must be an exact path from the manifest)", zonePath)
            .atPos(pos)
            .debugThrow();
}

// ============================================================================
// builtins.worldManifest
// Returns path -> zone metadata mapping from //.meta/manifest.json
// ============================================================================
static void prim_worldManifest(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(".meta/manifest.json");

    auto json = getManifest(state);

    auto attrs = state.buildBindings(json.size());
    for (auto & [path, value] : json.items()) {
        if (!value.contains("id") || !value.at("id").is_string())
            throw Error("zone '%s' in manifest has missing or non-string 'id' field", path);
        auto idStr = value.at("id").get<std::string>();

        auto zoneAttrs = state.buildBindings(1);
        zoneAttrs.alloc("id").mkString(idStr, state.mem);
        attrs.alloc(state.symbols.create(path)).mkAttrs(zoneAttrs);
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_worldManifest({
    .name = "__unsafeTectonixInternalManifest",
    .args = {},
    .doc = R"(
      Get the world manifest as a Nix attrset mapping zone paths to zone metadata.

      Example: `builtins.unsafeTectonixInternalManifest."//areas/tools/dev".id` returns `"W-123456"`.

      Uses `--tectonix-git-dir` (defaults to `~/world/git`) and requires
      `--tectonix-git-sha` to be set.
    )",
    .impl = prim_worldManifest,
});

// ============================================================================
// builtins.worldManifestInverted
// Returns zoneId -> path mapping (inverse of worldManifest)
// ============================================================================
static void prim_worldManifestInverted(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(".meta/manifest.json");

    auto json = getManifest(state);

    // Track seen IDs to detect duplicates
    std::set<std::string> seenIds;

    auto attrs = state.buildBindings(json.size());
    for (auto & [path, value] : json.items()) {
        if (!value.contains("id") || !value.at("id").is_string())
            throw Error("zone '%s' in manifest has missing or non-string 'id' field", path);
        auto idStr = value.at("id").get<std::string>();

        if (!seenIds.insert(idStr).second)
            throw Error("duplicate zone ID '%s' in manifest (zone '%s')", idStr, path);

        attrs.alloc(state.symbols.create(idStr)).mkString(path, state.mem);
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_worldManifestInverted({
    .name = "__unsafeTectonixInternalManifestInverted",
    .args = {},
    .doc = R"(
      Get the inverted world manifest as a Nix attrset mapping zone IDs to zone paths.

      Example: `builtins.unsafeTectonixInternalManifestInverted."W-123456"` returns `"//areas/tools/dev"`.

      Uses `--tectonix-git-dir` (defaults to `~/world/git`) and requires
      `--tectonix-git-sha` to be set.
    )",
    .impl = prim_worldManifestInverted,
});

// ============================================================================
// builtins.tectonixManifestEntry zonePath
// Returns null or { id = "W-xxxxxx" } for a single zone, recording only that
// entry as a tracked dependency (synthetic path .meta/manifest.json#<zonePath>)
// instead of the whole manifest file.
// ============================================================================
static void prim_tectonixManifestEntry(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'zonePath' argument to builtins.tectonixManifestEntry");
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(".meta/manifest.json#" + std::string(zonePath));
    auto & manifest = getManifest(state);
    auto it = manifest.find(std::string(zonePath));
    if (it == manifest.end() || !it->is_object() || !it->contains("id") || !(*it).at("id").is_string()) {
        v.mkNull();
        return;
    }
    auto zoneAttrs = state.buildBindings(1);
    zoneAttrs.alloc("id").mkString((*it).at("id").get<std::string>(), state.mem);
    v.mkAttrs(zoneAttrs);
}

static RegisterPrimOp primop_tectonixManifestEntry({
    .name = "__tectonixManifestEntry",
    .args = {"zonePath"},
    .doc = R"(
      Get a single zone's manifest entry as a Nix attrset { id = "W-xxxxxx"; },
      or null if the zone does not exist. Records only that entry as a Tecnix
      tracked dependency (synthetic path .meta/manifest.json#<zonePath>), not
      the whole manifest file.
      Example: `builtins.tectonixManifestEntry "//areas/tools/dev"` returns `{ id = "W-123456"; }`.
    )",
    .impl = prim_tectonixManifestEntry,
});

// ============================================================================
// builtins.tectonixManifestKeys
// Returns the sorted list of zone paths (manifest keys), recording only the
// key set as a tracked dependency (synthetic path .meta/manifest.json#keys).
// ============================================================================
static void prim_tectonixManifestKeys(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(".meta/manifest.json#keys");
    auto & manifest = getManifest(state);
    std::vector<std::string> keys;
    keys.reserve(manifest.size());
    for (auto & [path, value] : manifest.items())
        keys.push_back(path);
    std::sort(keys.begin(), keys.end());
    auto list = state.buildList(keys.size());
    for (size_t i = 0; i < keys.size(); i++) {
        auto * val = state.allocValue();
        val->mkString(keys[i], state.mem);
        list[i] = val;
    }
    v.mkList(list);
}

static RegisterPrimOp primop_tectonixManifestKeys({
    .name = "__tectonixManifestKeys",
    .args = {},
    .doc = R"(
      Get the sorted list of zone paths (manifest keys). Records only the key
      set as a Tecnix tracked dependency (synthetic path .meta/manifest.json#keys),
      not the whole manifest file. Only world-wide folds that enumerate every
      zone should use this; per-target resolution should use tectonixManifestEntry.
    )",
    .impl = prim_tectonixManifestKeys,
});

// ============================================================================
// builtins.tectonixManifestIdToPath zoneId
// Returns null or the zone path for a zone ID, recording only that id lookup
// as a tracked dependency (synthetic path .meta/manifest.json#id/<zoneId>).
// ============================================================================
static void prim_tectonixManifestIdToPath(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zoneId = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'zoneId' argument to builtins.tectonixManifestIdToPath");
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(".meta/manifest.json#id/" + std::string(zoneId));
    auto & manifest = getManifest(state);
    for (auto & [path, value] : manifest.items()) {
        if (value.contains("id") && value.at("id").is_string() && value.at("id").get<std::string>() == zoneId) {
            v.mkString(path, state.mem);
            return;
        }
    }
    v.mkNull();
}

static RegisterPrimOp primop_tectonixManifestIdToPath({
    .name = "__tectonixManifestIdToPath",
    .args = {"zoneId"},
    .doc = R"(
      Get the zone path for a zone ID, or null if not found. Records only that
      id lookup as a Tecnix tracked dependency (synthetic path
      .meta/manifest.json#id/<zoneId>), not the whole manifest file.
      Example: `builtins.tectonixManifestIdToPath "W-123456"` returns `"//areas/tools/dev"`.
    )",
    .impl = prim_tectonixManifestIdToPath,
});

// Re-entrancy guard for `builtins.tectonixMemo`: keys whose `f key` evaluation
// is currently in progress on this thread. Tracked evaluation is
// single-threaded, so a same-key re-entry can only be a genuine cycle (zone A
// loading itself transitively); the guard turns it into a catchable error
// instead of unbounded recursion through fresh thunks each call.
static thread_local std::vector<std::string> tectonixMemoInProgress;

// ============================================================================
// builtins.tectonixMemo namespace key f
// Evaluates `f key` once per EvalState and returns the shared value. While
// tracked, the one evaluation records its source accesses into a label stored
// alongside the value; every later consumer records that label as a child of
// its own frame (and inherits the value's label through the value-copy hooks),
// so all consumers share the same tracked dependencies without re-evaluating
// `f key`.
// ============================================================================
static void prim_tectonixMemo(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto ns =
        state.forceStringNoCtx(*args[0], pos, "while evaluating the 'namespace' argument to builtins.tectonixMemo");
    auto keyStr = state.forceStringNoCtx(*args[1], pos, "while evaluating the 'key' argument to builtins.tectonixMemo");

    auto cacheKey = std::string(ns) + '\0' + std::string(keyStr);
    auto * trackingCtx = currentTecnixThreadState.trackingContext;

    /* A result computed outside tracking carries no label, and the lazy parts
       of a tracked result must only ever be forced under tracking, so tracked
       and untracked calls never share a table: a tracked call reusing an
       untracked result would record none of its sources, and an untracked call
       forcing part of a tracked result would leave that part unlabelled for
       every later tracked consumer. Untracked results are still memoized, in
       their own table: `f` is typically recursive (zone A loads zone B), so
       building them afresh would re-walk everything they share. */
    auto & memoCache =
        trackingCtx ? *state.tecnixEvalData().trackedTecnixMemoCache : *state.tecnixEvalData().tecnixMemoCache;

    Value * stored = nullptr;
    EvalSourceAccessSetId sourceDeps = emptyEvalSourceAccessSetId;

    bool hit = false;
    memoCache.cvisit(cacheKey, [&](auto & i) {
        stored = *i.second.value;
        sourceDeps = i.second.sourceDeps;
        hit = true;
    });
    if (hit) {
        // Replay into this consumer the sources the entry's evaluation read.
        if (trackingCtx)
            recordTrackedSourceAccessSetDependency(*trackingCtx, sourceDeps);
        v = *stored;
        return;
    }

    // Miss. Guard against same-key re-entry (a cycle through `f`).
    for (const auto & inProgress : tectonixMemoInProgress) {
        if (inProgress == cacheKey)
            // AssertionError so builtins.tryEval can catch a circular zone
            // dependency the same way it catches a missing zone, instead of
            // crashing the whole evaluation.
            state
                .error<AssertionError>(
                    "builtins.tectonixMemo: circular evaluation detected for namespace '%s' key '%s'", ns, keyStr)
                .atPos(pos)
                .debugThrow();
    }

    tectonixMemoInProgress.push_back(cacheKey);
    Finally popInProgress([&]() { tectonixMemoInProgress.pop_back(); });

    // Evaluate `f key` outside any map bucket lock so re-entrant misses (zone A
    // loading zone B) can insert their own entries without deadlock. Under
    // tracking, scope the evaluation so its accesses intern into one reusable
    // label that every later consumer records.
    if (trackingCtx) {
        TrackedSourceDepsScope scope(*trackingCtx);
        stored = state.allocValue();
        state.callFunction(*args[2], *args[1], *stored, pos);
        state.forceValue(*stored, pos);
        sourceDeps = scope.finish(stored);
    } else {
        stored = state.allocValue();
        state.callFunction(*args[2], *args[1], *stored, pos);
        state.forceValue(*stored, pos);
    }

    // Insert, but if another evaluation raced ahead (parallel evaluation),
    // reuse its entry so the table keeps a single shared value.
    memoCache.try_emplace_and_cvisit(
        cacheKey,
        EvalTecnixMemoCacheEntry{},
        [&](auto & i) {
            i.second.value = RootValue(stored);
            i.second.sourceDeps = sourceDeps;
        },
        [&](auto & i) {
            stored = *i.second.value;
            sourceDeps = i.second.sourceDeps;
        });

    if (trackingCtx)
        recordTrackedSourceAccessSetDependency(*trackingCtx, sourceDeps);
    v = *stored;
}

static RegisterPrimOp primop_tectonixMemo({
    .name = "__tectonixMemo",
    .args = {"namespace", "key", "f"},
    .doc = R"(
      Evaluate `f key` once per evaluation and return the shared value. Later
      calls with the same `namespace` and `key` return the cached value without
      re-invoking `f`, and (under Tecnix source tracking) every consumer
      inherits the cached value's recorded source dependencies, so the work and
      the tracked closure are both shared.

      `namespace` and `key` must be strings; `f` is called as `f key`. A
      self-referential `f` (one whose evaluation re-enters `tectonixMemo` with
      the same namespace and key) throws a catchable circular-evaluation error.

      Results computed outside Tecnix source tracking and results computed
      under it are kept apart: neither is ever returned to the other kind of
      call.

      Example:
      `builtins.tectonixMemo "zones" "//a/b" (path: loadZone path)`
      evaluates `loadZone "//a/b"` once and shares the result.
    )",
    .impl = prim_tectonixMemo,
});

static std::string normalizeTrackedRepoPath(std::string_view path);

// ============================================================================
// builtins.unsafeTectonixInternalTreeSha worldPath
// Returns the git tree SHA for a world path
// ============================================================================
static void prim_unsafeTectonixInternalTreeSha(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto worldPath = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'worldPath' argument to builtins.unsafeTectonixInternalTreeSha");

    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(normalizeTrackedRepoPath(worldPath));

    auto sha = getWorldTreeSha(state, worldPath);
    v.mkString(sha.gitRev(), state.mem);
}

static RegisterPrimOp primop_unsafeTectonixInternalTreeSha({
    .name = "__unsafeTectonixInternalTreeSha",
    .args = {"worldPath"},
    .doc = R"(
      Get the git tree SHA for a path in the world repository.

      Example: `builtins.unsafeTectonixInternalTreeSha "//areas/tools/tec"` returns the tree SHA
      for that zone.

      Uses `--tectonix-git-dir` (defaults to `~/world/git`) and requires
      `--tectonix-git-sha` to be set.
    )",
    .impl = prim_unsafeTectonixInternalTreeSha,
});

// ============================================================================
// builtins.unsafeTectonixInternalTree treeSha
// Returns a store path containing the tree contents
// ============================================================================
static void prim_unsafeTectonixInternalTree(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto treeSha = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'treeSha' argument to builtins.unsafeTectonixInternalTree");

    auto repo = getWorldRepo(state);
    auto hash = Hash::parseNonSRIUnprefixed(treeSha, HashAlgorithm::SHA1);

    if (!repo->hasObject(hash))
        state.error<EvalError>("tree SHA '%s' not found in world repository", treeSha).atPos(pos).debugThrow();

    // exportIgnore=false: This is raw tree access by SHA, used for low-level operations.
    // Unlike zone accessors (which use exportIgnore=true to honor .gitattributes for
    // filtered zone content), this provides unfiltered access to exact tree contents.
    GitAccessorOptions opts{.exportIgnore = false, .smudgeLfs = false};
    auto accessor = repo->getAccessor(hash, opts, "world-tree");

    FetchToStoreCallerScope callerScope(FetchToStoreCaller::TectonixTree);
    auto storePath = fetchToStore(
        state.fetchSettings,
        *state.store,
        SourcePath(accessor, CanonPath::root),
        FetchMode::Copy,
        "world-tree-" + std::string(treeSha).substr(0, 12));

    state.allowAndSetStorePathString(storePath, v);
}

static RegisterPrimOp primop_unsafeTectonixInternalTree({
    .name = "__unsafeTectonixInternalTree",
    .args = {"treeSha"},
    .doc = R"(
      Fetch a git tree by SHA from the world repository and return it as a store path.

      Example: `builtins.unsafeTectonixInternalTree "abc123..."` returns `/nix/store/...-world-tree-abc123`.

      Uses `--tectonix-git-dir` (defaults to `~/world/git`).
    )",
    .impl = prim_unsafeTectonixInternalTree,
});

// ============================================================================
// builtins.unsafeTectonixInternalZoneSrc zonePath
// Returns a store path containing the zone source
// With lazy-trees enabled, returns a virtual store path that is only
// materialized when used as a derivation input.
// ============================================================================
static std::string normalizeTrackedRepoPath(std::string_view path)
{
    std::string result(path);
    if (hasPrefix(result, "//"))
        result = result.substr(2);
    else if (hasPrefix(result, "/"))
        result = result.substr(1);
    return result;
}

static void prim_unsafeTectonixInternalZoneSrc(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'zonePath' argument to builtins.unsafeTectonixInternalZoneSrc");

    validateZonePath(state, pos, zonePath);
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(normalizeTrackedRepoPath(zonePath));

    auto storePath = getLegacyTectonixZoneStorePath(state, zonePath);
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

      Uses `--tectonix-git-dir` (defaults to `~/world/git`) and requires
      `--tectonix-git-sha` to be set.
    )",
    .impl = prim_unsafeTectonixInternalZoneSrc,
});

// ============================================================================
// builtins.unsafeTectonixInternalZonePath zonePath
// Same as unsafeTectonixInternalZoneSrc but returns a path instead of a string.
// ============================================================================
static void prim_unsafeTectonixInternalZonePath(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'zonePath' argument to builtins.unsafeTectonixInternalZonePath");

    validateZonePath(state, pos, zonePath);
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx)
        ctx->recordAccess(normalizeTrackedRepoPath(zonePath));

    auto storePath = getLegacyTectonixZoneStorePath(state, zonePath);
    state.allowPath(storePath);
    v.mkPath(state.storePath(storePath), state.mem);
}

static RegisterPrimOp primop_unsafeTectonixInternalZonePath({
    .name = "__unsafeTectonixInternalZonePath",
    .args = {"zonePath"},
    .doc = R"(
      Get the source of a zone as a path value.

      With `lazy-trees = true`, returns a virtual store path that is only
      materialized when used as a derivation input (devirtualized).

      In source-available mode with uncommitted changes, uses checkout content
      (always eager for dirty zones).

      Example: `builtins.unsafeTectonixInternalZonePath "//areas/tools/tec"`

      Uses `--tectonix-git-dir` (defaults to `~/world/git`) and requires
      `--tectonix-git-sha` to be set.
    )",
    .impl = prim_unsafeTectonixInternalZonePath,
});

// ============================================================================
// builtins.unsafeTectonixInternalSparseCheckoutRoots
// Returns list of zone IDs in sparse checkout
// ============================================================================
static void throwIfTrackedLegacyCheckoutStateBuiltin(EvalState & state, const PosIdx pos, std::string_view builtinName)
{
    if (currentTecnixThreadState.trackingContext)
        state
            .error<EvalError>(
                "legacy compatibility builtin builtins.%s exposes checkout-local state and cannot be used during Tecnix dependency tracking",
                builtinName)
            .atPos(pos)
            .debugThrow();
}

static void
prim_unsafeTectonixInternalSparseCheckoutRoots(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    throwIfTrackedLegacyCheckoutStateBuiltin(state, pos, "unsafeTectonixInternalSparseCheckoutRoots");

    auto & roots = getTectonixSparseCheckoutRoots(state);

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
    .impl = prim_unsafeTectonixInternalSparseCheckoutRoots,
});

// ============================================================================
// builtins.unsafeTectonixInternalDirtyZones
// Returns map of zone paths to dirty status
// ============================================================================
static void prim_unsafeTectonixInternalDirtyZones(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    throwIfTrackedLegacyCheckoutStateBuiltin(state, pos, "unsafeTectonixInternalDirtyZones");

    auto & dirtyZones = getTectonixDirtyZones(state);

    auto attrs = state.buildBindings(dirtyZones.size());
    for (const auto & [zonePath, info] : dirtyZones) {
        attrs.alloc(state.symbols.create(zonePath)).mkBool(info.dirty);
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
    .impl = prim_unsafeTectonixInternalDirtyZones,
});

// ============================================================================
// builtins.__unsafeTectonixInternalZoneIsDirty zonePath
// Returns whether a given zone is dirty in the checkout
// ============================================================================
static void prim_unsafeTectonixInternalZoneIsDirty(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    throwIfTrackedLegacyCheckoutStateBuiltin(state, pos, "unsafeTectonixInternalZoneIsDirty");

    auto zonePath = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'zonePath' argument to builtins.__unsafeTectonixInternalZoneIsDirty");

    validateZonePath(state, pos, zonePath);

    bool isDirty = false;
    if (isTectonixSourceAvailable(state)) {
        auto & dirtyZones = getTectonixDirtyZones(state);
        auto it = dirtyZones.find(std::string(zonePath));
        isDirty = it != dirtyZones.end() && it->second.dirty;
    }

    v.mkBool(isDirty);
}

static RegisterPrimOp primop_unsafeTectonixInternalZoneIsDirty({
    .name = "__unsafeTectonixInternalZoneIsDirty",
    .args = {"zonePath"},
    .doc = R"(
      Get whether a zone is in the sparse checkout and whether it is dirty.

      Example: `builtins.unsafeTectonixInternalZoneIsDirty "//areas/tools/tec"`

      Uses `--tectonix-git-dir` (defaults to `~/world/git`).
    )",
    .impl = prim_unsafeTectonixInternalZoneIsDirty,
});

// ============================================================================
// builtins.__unsafeTectonixInternalZoneRoot zonePath
// Returns an zone root path in sparse checkout
// ============================================================================
static void prim_unsafeTectonixInternalZoneRoot(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    throwIfTrackedLegacyCheckoutStateBuiltin(state, pos, "unsafeTectonixInternalZoneRoot");

    auto zonePath = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the 'zonePath' argument to builtins.__unsafeTectonixInternalZoneRoot");

    validateZonePath(state, pos, zonePath);

    std::string zone(zonePath);
    if (hasPrefix(zone, "//"))
        zone = zone.substr(2);

    auto checkoutPath = state.settings.tectonixCheckoutPath.get();
    auto fullPath = std::filesystem::path(checkoutPath) / zone;

    if (std::filesystem::exists(fullPath) && !state.settings.pureEval) {
        v.mkString(fullPath.string(), state.mem);
    } else {
        // Zone not accessible in checkout
        v.mkNull();
    }
}

static RegisterPrimOp primop_unsafeTectonixInternalZoneRoot({
    .name = "__unsafeTectonixInternalZoneRoot",
    .args = {"zonePath"},
    .doc = R"(
      Get the root of a zone in sparse checkout, if available.

      With `lazy-trees = true`, returns a virtual store path that is only
      materialized when used as a derivation input (devirtualized).

      In source-available mode with uncommitted changes, uses checkout content
      (always eager for dirty zones).

      Example: `builtins.unsafeTectonixInternalZoneRoot "//areas/tools/tec"`

      Uses `--tectonix-git-dir` (defaults to `~/world/git`).
    )",
    .impl = prim_unsafeTectonixInternalZoneRoot,
});

// ============================================================================
// builtins.unsafeTectonixInternalGitSha
// Returns the git commit SHA that tectonix is evaluating against
// ============================================================================
static void prim_unsafeTectonixInternalGitSha(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto & sha = requireTectonixGitSha(state);
    v.mkString(sha, state.mem);
}

static RegisterPrimOp primop_unsafeTectonixInternalGitSha({
    .name = "__unsafeTectonixInternalGitSha",
    .args = {},
    .doc = R"(
      Get the git commit SHA that tectonix is evaluating against.

      Returns the value of `--tectonix-git-sha` as a string.

      Example: `builtins.unsafeTectonixInternalGitSha` returns `"abc123def456..."`.

      Requires `--tectonix-git-sha` to be set.
    )",
    .impl = prim_unsafeTectonixInternalGitSha,
});

} // namespace nix
