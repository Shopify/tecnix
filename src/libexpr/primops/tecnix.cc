/**
 * The public Tecnix builtins (`builtins.tecnixTargets`,
 * `builtins.tecnixTargetNames`, and the internal source-deps scope
 * builtins): argument parsing, canonical args-key JSON, and per-target
 * orchestration over the tracked accessors (tecnix/repo-accessor.cc) and
 * the persistent cache (tecnix/eval-cache.cc).
 */

#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/tecnix/access-set-graph.hh"
#include "nix/expr/tecnix/eval-cache.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/store/store-api.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

// ============================================================================
// builtins.tecnixInternalSourceDepsScope value
// Evaluates a lazy value under a reusable source-deps scope for Tecnix
// dependency tracking. Outside tracking eval, this is an identity.
// ============================================================================
static void prim_tecnixInternalSourceDepsScope(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx) {
        TrackedSourceDepsScope scope(*ctx);
        state.forceValue(*args[0], pos);
        v = *args[0];
        scope.finish(&v);
        return;
    }

    state.forceValue(*args[0], pos);
    v = *args[0];
}

static RegisterPrimOp primop_tecnixInternalSourceDepsScope({
    .name = "__tecnixInternalSourceDepsScope",
    .args = {"value"},
    .doc = R"(
      Mark `value` as being evaluated under a reusable Tecnix source-deps scope.

      This is an internal exact-dependency-tracking primitive. It behaves like
      `value`, but while tracked Tecnix evaluation forces the wrapper, source
      accesses are collected into a label that later reuse of the already-forced
      wrapper can inherit.
    )",
    .impl = prim_tecnixInternalSourceDepsScope,
});

static Value * makeTecnixSourceDepsScopeApplication(EvalState & state, Value * value)
{
    auto * scoped = state.allocValue();
    scoped->mkApp(&state.getBuiltin("tecnixInternalSourceDepsScope"), value);
    return scoped;
}

// ============================================================================
// builtins.tecnixInternalSourceDepsAttrs attrs
// Returns an attrset whose values are lazily wrapped in source-deps scopes.
// ============================================================================
static void prim_tecnixInternalSourceDepsAttrs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the 'attrs' argument to builtins.tecnixInternalSourceDepsAttrs");

    auto bindings = state.buildBindings(args[0]->attrs()->size());
    for (auto & attr : *args[0]->attrs())
        bindings.insert(attr.name, makeTecnixSourceDepsScopeApplication(state, attr.value), attr.pos);
    v.mkAttrs(bindings);
}

static RegisterPrimOp primop_tecnixInternalSourceDepsAttrs({
    .name = "__tecnixInternalSourceDepsAttrs",
    .args = {"attrs"},
    .doc = R"(
      Internal Tecnix helper: return an attrset whose values are lazily wrapped
      in `tecnixInternalSourceDepsScope`.
    )",
    .impl = prim_tecnixInternalSourceDepsAttrs,
});

// ============================================================================
// builtins.tecnixInternalSourceDepsList list
// Returns a list whose elements are lazily wrapped in source-deps scopes.
// ============================================================================
static void prim_tecnixInternalSourceDepsList(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the 'list' argument to builtins.tecnixInternalSourceDepsList");

    auto list = state.buildList(args[0]->listSize());
    size_t index = 0;
    for (auto * elem : args[0]->listView())
        list[index++] = makeTecnixSourceDepsScopeApplication(state, elem);
    v.mkList(list);
}

static RegisterPrimOp primop_tecnixInternalSourceDepsList({
    .name = "__tecnixInternalSourceDepsList",
    .args = {"list"},
    .doc = R"(
      Internal Tecnix helper: return a list whose elements are lazily wrapped in
      `tecnixInternalSourceDepsScope`.
    )",
    .impl = prim_tecnixInternalSourceDepsList,
});

// ============================================================================
// Shared helpers for Tecnix target evaluation and dependency-path tracking.
// ============================================================================

/**
 * Resolve the git SHA to use: explicit rev attr > checkout HEAD > error.
 */
static std::string
resolveRev(EvalState & state, const PosIdx pos, const Bindings & attrs, const std::string & checkoutPath)
{
    // Check for explicit rev attr
    auto revAttr = attrs.get(state.symbols.create("rev"));
    if (revAttr) {
        auto sha = state.forceStringNoCtx(*revAttr->value, pos, "while evaluating the 'rev' argument");
        if (!sha.empty())
            return std::string(sha);
    }

    // Try to read HEAD from checkout.
    if (!checkoutPath.empty()) {
        try {
            return resolveCheckoutHeadRev(checkoutPath);
        } catch (Error & e) {
            state
                .error<EvalError>(
                    "could not determine git SHA from checkoutPath '%s': %s; set 'rev' or provide a valid 'checkoutPath'",
                    checkoutPath,
                    e.what())
                .atPos(pos)
                .debugThrow();
        }
    }

    state.error<EvalError>("could not determine git SHA: set 'rev' or provide a valid 'checkoutPath'")
        .atPos(pos)
        .debugThrow();
}

struct TecnixArgs
{
    std::string gitDir;
    std::string resolver;
    std::string rev;
    std::string checkoutPath;
    Value * resolverArgs = nullptr;
    std::string argsKey;
    std::vector<std::string> targets;
    /** The caller asked for the tracked source closure (`includeDependencies`),
        so it must be computed even when the eval cache would not need it. */
    bool requireDependencies = false;
};

/** The persistent-cache row family these arguments address. */
static TecnixCacheScope cacheScope(const TecnixArgs & args)
{
    return {args.gitDir, args.resolver, args.argsKey};
}

static const Bindings & forceTecnixBuiltinAttrs(EvalState & state, const PosIdx pos, Value ** args)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument to a tecnix builtin");
    return *args[0]->attrs();
}

static void parseTecnixRepoArgs(EvalState & state, const PosIdx pos, const Bindings & attrs, TecnixArgs & result)
{
    auto gitDirAttr = attrs.get(state.symbols.create("gitDir"));
    if (!gitDirAttr)
        state.error<EvalError>("'gitDir' attribute required").atPos(pos).debugThrow();
    result.gitDir =
        std::string(state.forceStringNoCtx(*gitDirAttr->value, pos, "while evaluating the 'gitDir' argument"));

    auto resolverAttr = attrs.get(state.symbols.create("resolver"));
    if (!resolverAttr)
        state.error<EvalError>("'resolver' attribute required").atPos(pos).debugThrow();
    result.resolver =
        std::string(state.forceStringNoCtx(*resolverAttr->value, pos, "while evaluating the 'resolver' argument"));

    auto checkoutPathAttr = attrs.get(state.symbols.create("checkoutPath"));
    if (checkoutPathAttr)
        result.checkoutPath = std::string(
            state.forceStringNoCtx(*checkoutPathAttr->value, pos, "while evaluating the 'checkoutPath' argument"));

    result.rev = resolveRev(state, pos, attrs, result.checkoutPath);
}

/**
 * Canonical JSON encoding of the caller's `args`, used as the cache key
 * (`argsKey`). Caching is sound only because this encoding is injective on
 * the values it accepts: the resolver receives the same value, so results can
 * depend on `args` only through content that is, by construction, the key.
 *
 * Deliberately NOT `printValueAsJSON`, whose coercions break injectivity or
 * purity: derivation attrsets serialize as their `outPath`, `__toString`
 * attrsets coerce to strings (either lets distinct args collide on one key,
 * turning a key collision into a stale cache hit), string context is dropped,
 * paths are copied to the store as a side effect, and floats serialize
 * ambiguously. This function instead rejects everything whose encoding would
 * lose information: only null, bool, int, context-free string, list, and
 * plain attrset are accepted.
 */
static nlohmann::json canonicalJsonFromValue(EvalState & state, Value & value, const PosIdx pos)
{
    state.forceValue(value, pos);

    switch (value.type()) {
    case nNull:
        return nullptr;
    case nBool:
        return value.boolean();
    case nInt:
        return value.integer().value;
    case nString: {
        auto string = state.forceStringNoCtx(value, pos, "while converting the 'args' argument to canonical JSON");
        return std::string(string);
    }
    case nList: {
        auto result = nlohmann::json::array();
        for (auto elem : value.listView())
            result.push_back(canonicalJsonFromValue(state, *elem, pos));
        return result;
    }
    case nAttrs: {
        auto result = nlohmann::json::object();
        for (auto & attr : value.attrs()->lexicographicOrder(state.symbols)) {
            result.emplace(state.symbols[attr->name], canonicalJsonFromValue(state, *attr->value, attr->pos));
        }
        return result;
    }
    case nFloat:
    case nPath:
    case nThunk:
    case nFailed:
    case nFunction:
    case nExternal:
        state
            .error<EvalError>(
                "'args' must be JSON-convertible (null, bool, int, string without context, list, or attrset)")
            .atPos(pos)
            .debugThrow();
    }

    unreachable();
}

static std::pair<Value *, std::string>
parseTecnixResolverArgsValue(EvalState & state, const PosIdx pos, const Bindings & attrs)
{
    auto resolverArgsAttr = attrs.get(state.s.args);
    if (!resolverArgsAttr)
        state.error<EvalError>("'args' attribute required").atPos(pos).debugThrow();

    auto canonicalJson = canonicalJsonFromValue(state, *resolverArgsAttr->value, pos).dump();
    return {resolverArgsAttr->value, std::move(canonicalJson)};
}

static std::vector<std::string> parseTecnixTargets(EvalState & state, const PosIdx pos, const Bindings & attrs)
{
    auto targetsAttr = attrs.get(state.symbols.create("targets"));
    if (!targetsAttr)
        state.error<EvalError>("'targets' attribute required").atPos(pos).debugThrow();

    state.forceList(*targetsAttr->value, pos, "while evaluating the 'targets' argument");
    std::vector<std::string> targets;
    targets.reserve(targetsAttr->value->listSize());
    for (auto elem : targetsAttr->value->listView()) {
        auto targetId = state.forceStringNoCtx(*elem, pos, "while evaluating a target id");
        if (targetId == tecnixTargetNamesCacheKey)
            state.error<EvalError>("tecnix target id '%s' is reserved", targetId).atPos(pos).debugThrow();
        targets.push_back(std::string(targetId));
    }
    return targets;
}

static TecnixArgs parseTecnixArgs(EvalState & state, const PosIdx pos, Value ** args, bool withTargets)
{
    auto & attrs = forceTecnixBuiltinAttrs(state, pos, args);
    auto [resolverArgs, argsKey] = parseTecnixResolverArgsValue(state, pos, attrs);

    TecnixArgs result;
    parseTecnixRepoArgs(state, pos, attrs, result);
    result.resolverArgs = resolverArgs;
    result.argsKey = std::move(argsKey);
    if (withTargets)
        result.targets = parseTecnixTargets(state, pos, attrs);
    return result;
}

static bool getTecnixBoolAttr(
    EvalState & state,
    const PosIdx pos,
    Value ** args,
    const Symbol & name,
    std::string_view context,
    bool defaultValue = false)
{
    auto & attrs = forceTecnixBuiltinAttrs(state, pos, args);
    if (auto attr = attrs.get(name))
        return state.forceBool(*attr->value, pos, context);
    return defaultValue;
}

static void requireTecnixTargets(EvalState & state, const PosIdx pos, const TecnixArgs & args)
{
    if (args.targets.empty())
        state.error<EvalError>("'targets' attribute must contain at least one target reference")
            .atPos(pos)
            .debugThrow();
}

/**
 * Configure the repository context used by Tecnix evaluation.
 *
 * The settings names are still `tectonix-*` for CLI compatibility, but new
 * Tecnix primops use the full-repo accessor mounted from this context.
 *
 * Must be called before getResolveFunction().
 *
 * A single EvalState has lazy, cached repository accessors, so all Tecnix calls
 * in that state must use the same repository context.
 */
static void configureTecnixRepoContext(EvalState & state, const TecnixArgs & args)
{
    configureTectonixContext(state, args.gitDir, args.rev, args.checkoutPath);
}

/**
 * Whether source-access tracking should run at all.
 *
 * Tracking exists to compute the dependency closure. That closure has exactly
 * two consumers: the eval cache, which keys rows on it, and an explicit
 * `includeDependencies` request, which returns it. With neither, tracking
 * every force, interning the sets and fingerprinting the paths is pure
 * overhead. Gating here also makes "not tracking" a whole-evaluation property,
 * which is what lets the resolver module be shared in that mode.
 */
static bool tecnixSourceTrackingEnabled(const EvalState & state, const TecnixArgs & tArgs)
{
    return tArgs.requireDependencies || (state.settings.pureEval && state.settings.tecnixEvalCache);
}

/** Keyspace separator for modules built without tracking; see
    getTecnixModuleValue. The embedded NUL keeps it disjoint from any resolver
    path, which cannot contain one. */
static constexpr std::string_view untrackedTecnixModuleKeyPrefix{"untracked\0", 10};

/**
 * Import the explicit resolver file from the git repo and return a value from
 * the attrset produced by calling it with `args` (e.g. `resolve` or
 * `allTargetNames`).
 *
 * Requires configureTecnixRepoContext() to have been called first.
 */
static Value &
getTecnixModuleValue(EvalState & state, const PosIdx pos, const TecnixArgs & tArgs, std::string_view attrName)
{
    if (!tArgs.resolverArgs)
        state.error<EvalError>("missing Tecnix resolver args").atPos(pos).debugThrow();

    auto buildModule = [&]() -> Value * {
        // Get resolver file path from the lazily-mounted Tecnix repo accessor.
        auto resolverPath = getTecnixRepoPath(state, tArgs.resolver);
        auto modulePath = SourcePath(state.rootFS, CanonPath(resolverPath));

        // Import the resolver file (a function taking the opaque `args` value) and call it.
        auto * moduleFn = state.allocValue();
        state.evalFile(modulePath, *moduleFn);

        auto * moduleVal = state.allocValue();
        state.callFunction(*moduleFn, *tArgs.resolverArgs, *moduleVal, pos);
        state.forceAttrs(*moduleVal, pos, "while evaluating tecnix module");
        return moduleVal;
    };

    auto * trackingCtx = currentTecnixThreadState.trackingContext;

    Value * moduleVal = nullptr;

    if (!trackingCtx && tecnixSourceTrackingEnabled(state, tArgs)) {
        /* Tracking is enabled for this evaluation but this particular build is
           untracked, so it has no label to replay. Caching it would let a later
           tracked caller reuse a module whose accesses were never recorded. */
        moduleVal = buildModule();
    } else if (!trackingCtx) {
        /* This call does not track, so the module it builds carries no label.
           Share it only with other untracked calls: tracking is decided per
           call (`includeDependencies` turns it on with the cache off), so a
           tracked call later in the same evaluation must not inherit a module
           whose accesses were never recorded -- it would silently drop the
           resolver's own files from every target's closure. Hence the separate
           keyspace rather than a shared entry. */
        auto & moduleCache = *state.tecnixEvalData().tecnixModuleCache;
        auto cacheKey = std::string(untrackedTecnixModuleKeyPrefix) + tArgs.resolver + '\0' + tArgs.argsKey;
        moduleCache.try_emplace_and_cvisit(
            cacheKey,
            EvalTecnixModuleCacheEntry{},
            [&](auto & i) {
                moduleVal = buildModule();
                i.second.value = RootValue(moduleVal);
                i.second.sourceDeps = emptyEvalSourceAccessSetId;
            },
            [&](auto & i) { moduleVal = *i.second.value; });
    } else {
        /* Reuse the applied module for this (resolver, args) pair. Without this
           every builtin re-applies the resolver and gets a fresh, wholly
           unevaluated attrset, so discovering target names and then resolving
           them walks the zone graph twice over. */
        auto & moduleCache = *state.tecnixEvalData().tecnixModuleCache;
        auto cacheKey = tArgs.resolver + '\0' + tArgs.argsKey;

        auto sourceDeps = emptyEvalSourceAccessSetId;
        bool hit = false;
        moduleCache.cvisit(cacheKey, [&](auto & i) {
            moduleVal = *i.second.value;
            sourceDeps = i.second.sourceDeps;
            hit = true;
        });

        if (!hit) {
            /* Scope the build so the accesses it makes — importing the resolver
               above all — are interned into one set that later contexts can
               replay, instead of only landing in whichever frame happened to be
               current the first time. */
            TrackedSourceDepsScope moduleScope(*trackingCtx);
            moduleVal = buildModule();
            sourceDeps = moduleScope.finish(moduleVal);

            moduleCache.try_emplace_and_cvisit(
                cacheKey,
                EvalTecnixModuleCacheEntry{},
                [&](auto & i) {
                    i.second.value = RootValue(moduleVal);
                    i.second.sourceDeps = sourceDeps;
                },
                [&](auto & i) {
                    moduleVal = *i.second.value;
                    sourceDeps = i.second.sourceDeps;
                });
        } else {
            recordTrackedSourceAccessSetDependency(*trackingCtx, sourceDeps);
        }
    }

    auto attr = moduleVal->attrs()->get(state.symbols.create(attrName));
    if (!attr)
        state.error<EvalError>("tecnix module must have a '%s' attribute", attrName).atPos(pos).debugThrow();

    return *attr->value;
}

static Value & getResolveFunction(EvalState & state, const PosIdx pos, const TecnixArgs & tArgs)
{
    auto & fn = getTecnixModuleValue(state, pos, tArgs, "resolve");
    state.forceFunction(fn, pos, "while evaluating the 'resolve' attribute of tecnix module");
    return fn;
}

struct SourceAccessSetSnapshot
{
    std::vector<EvalSourceAccessId> directAccesses;
    std::vector<EvalSourceAccessSetId> accessSetEdges;
};

static SourceAccessSetSnapshot snapshotSourceAccessSetTracking(const TrackingContext & ctx)
{
    // Tracking contexts are thread-confined: the snapshot runs on the thread
    // that owns the context, after its evaluation has completed.
    SourceAccessSetSnapshot snapshot;
    auto rootAccesses = ctx.rootFrame.directSourceAccessSetAccesses();
    auto rootChildren = ctx.rootFrame.childSourceAccessSets();
    snapshot.directAccesses.assign(rootAccesses.begin(), rootAccesses.end());
    snapshot.accessSetEdges.assign(rootChildren.begin(), rootChildren.end());
    return snapshot;
}

static std::vector<std::string> collectSourceAccessSetTrackedPaths(
    const ref<EvalSourceAccessSetGraph> & sourceAccessSetGraph, const SourceAccessSetSnapshot & snapshot)
{
    // `flatten` yields unique paths; sort for deterministic closure output.
    auto paths = sourceAccessSetGraph->flatten(snapshot.directAccesses, snapshot.accessSetEdges);
    std::sort(paths.begin(), paths.end());
    return paths;
}

static std::vector<std::string> collectSourceAccessSetTrackedPaths(const TrackingContext & ctx)
{
    if (!ctx.sourceAccessSetGraph->isEnabled())
        throw Error("Tecnix source access-set tracking was not enabled");
    return collectSourceAccessSetTrackedPaths(ctx.sourceAccessSetGraph, snapshotSourceAccessSetTracking(ctx));
}

static Value * dependencyAttrsToValue(EvalState & state, const DependencyClosure & dependencies)
{
    auto attrs = state.buildBindings(dependencies.size());
    for (auto & dependency : dependencies) {
        auto * fingerprintValue = state.allocValue();
        fingerprintValue->mkString(dependency.fingerprint, state.mem);
        attrs.insert(state.symbols.create(dependency.path), fingerprintValue);
    }
    auto * val = state.allocValue();
    val->mkAttrs(attrs);

    return val;
}

static std::vector<std::string> evalTargetNamesOnly(EvalState & state, const PosIdx pos, const TecnixArgs & tArgs)
{
    auto & allTargetNames = getTecnixModuleValue(state, pos, tArgs, "allTargetNames");
    state.forceList(allTargetNames, pos, "while evaluating all target names");

    std::vector<std::string> targetNames;
    for (auto elem : allTargetNames.listView()) {
        auto targetName = state.forceStringNoCtx(*elem, pos, "while evaluating a target id");
        targetNames.push_back(std::string(targetName));
    }
    return targetNames;
}

struct TecnixDiscoveryResult
{
    std::vector<std::string> targetNames;
    /** The freshly evaluated closure; set on a cache miss. */
    DependencyClosure dependencies;
    /** The proven cached closure; set on a cache hit. */
    std::optional<ValidatedDependencyBlob> dependencyBlob;
};

/**
 * Discover target names through the same cache pipeline as target
 * dependencies: one reserved key, the same lookup and validation, the same
 * tracked evaluation on a miss, and the same upsert, with the discovered
 * names carried as the candidate payload.
 */
static TecnixDiscoveryResult discoverTecnixTargetNames(
    EvalState & state, const PosIdx pos, const TecnixArgs & tArgs, DependencyFingerprintCache & fingerprintCache)
{
    bool useCache = state.settings.pureEval && state.settings.tecnixEvalCache;
    bool track = tecnixSourceTrackingEnabled(state, tArgs);

    std::string cacheKey{tecnixTargetNamesCacheKey};
    if (useCache) {
        auto hits = lookupValidatedDependencyBlobs(
            state, cacheScope(tArgs), std::span<const std::string>{&cacheKey, 1}, fingerprintCache);
        if (hits[0]) {
            if (auto payload = hits[0]->payload()) {
                try {
                    auto targetNames = nlohmann::json::parse(*payload).get<std::vector<std::string>>();
                    printTalkative("tecnixTargetNames: discovery cache hit");
                    return {std::move(targetNames), {}, std::move(hits[0])};
                } catch (const nlohmann::json::exception &) {
                    // A malformed payload is a cache miss, never an error.
                }
            }
        }
    }

    printTalkative("tecnixTargetNames: discovery cache miss, evaluating");
    std::optional<TrackingContext> trackingCtx;
    std::vector<std::string> targetNames;
    if (track) {
        trackingCtx.emplace(state);
        ActiveTrackingContext activeTrackingCtx(*trackingCtx);
        targetNames = evalTargetNamesOnly(state, pos, tArgs);
    } else {
        targetNames = evalTargetNamesOnly(state, pos, tArgs);
    }
    DependencyClosure dependencies;
    if (trackingCtx) {
        auto trackedPaths = collectSourceAccessSetTrackedPaths(*trackingCtx);
        dependencies = dependencyFingerprints(getTecnixRepoAccessor(state), trackedPaths, fingerprintCache);
    }

    if (useCache && !dependencies.empty()) {
        std::vector<TecnixDependencyUpsert> upserts;
        upserts.push_back({cacheKey, &dependencies, nlohmann::json(targetNames).dump()});
        upsertDependencyClosures(cacheScope(tArgs), upserts);
    }

    return {std::move(targetNames), std::move(dependencies), std::nullopt};
}

static Value * targetRefToValue(EvalState & state, const std::string & target)
{
    auto * val = state.allocValue();
    val->mkString(target, state.mem);
    return val;
}

// ============================================================================
// builtins.tecnixTargets { gitDir, resolver, args, targets = [ target-id ... ], ... }
// Resolves opaque target IDs via module contract.
// ============================================================================
static void finishTecnixFutures(std::vector<std::future<void>> && futures)
{
    std::exception_ptr ex;
    std::exception_ptr interrupted;
    size_t secondaryErrors = 0;
    size_t secondaryInterrupts = 0;

    for (auto & future : futures) {
        try {
            future.get();
        } catch (const Interrupted &) {
            if (!interrupted)
                interrupted = std::current_exception();
            else
                secondaryInterrupts++;
        } catch (...) {
            if (!ex)
                ex = std::current_exception();
            else
                secondaryErrors++;
        }
    }

    if (secondaryErrors || secondaryInterrupts) {
        warn(
            "tecnix: %d additional parallel evaluation(s) failed and %d were interrupted; rethrowing the first error",
            secondaryErrors,
            secondaryInterrupts);
    }

    if (ex)
        std::rethrow_exception(ex);
    if (interrupted)
        std::rethrow_exception(interrupted);
}

template<typename EvalOne>
static void
evalTecnixIndices(EvalState & state, const std::vector<size_t> & indices, EvalOne evalOne, bool allowParallel = true)
{
    if (indices.empty())
        return;

    if (allowParallel && state.executor->enabled && !Executor::amWorkerThread && indices.size() > 1) {
        Executor::WorkItems work;
        for (auto i : indices)
            state.addWork(work, 0, [&, i]() { evalOne(i); });
        finishTecnixFutures(state.executor->spawn(std::move(work)));
        return;
    }

    for (auto i : indices)
        evalOne(i);
}

/**
 * Force the target value's `drvPath` attribute (on the miss path this is the
 * force that learns the target's closure) and return the printed drv path,
 * or "" when the value is not a derivation-shaped attrset.
 */
static std::string_view forceTargetDrvPath(EvalState & state, Value & targetValue, const PosIdx pos)
{
    state.forceValue(targetValue, pos);
    if (targetValue.type() != nAttrs)
        return "";

    auto drvPathAttr = targetValue.attrs()->get(state.s.drvPath);
    if (!drvPathAttr)
        return "";

    NixStringContext context;
    return state.forceString(
        *drvPathAttr->value, context, pos, "while evaluating the 'drvPath' attribute of a tecnix target");
}

// Versioned target payload: magic, drvPath, NUL, outputName. The two fields
// are read directly from the candidate's bytes on a hit.
static constexpr std::string_view targetValuePayloadPrefix{"TXTV1\0", 6};

static void prim_tecnixTargetsWithDependencies(
    EvalState & state, const PosIdx pos, Value ** args, Value & v, TecnixArgs && tArgs, bool includeTargets);

static void prim_tecnixTargetsCached(EvalState & state, const PosIdx pos, Value & v, const TecnixArgs & tArgs);

static void prim_tecnixTargets(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto tArgs = parseTecnixArgs(state, pos, args, true);
    requireTecnixTargets(state, pos, tArgs);
    configureTecnixRepoContext(state, tArgs);

    auto includeDependencies = getTecnixBoolAttr(
        state,
        pos,
        args,
        state.symbols.create("includeDependencies"),
        "while evaluating the 'includeDependencies' argument to builtins.tecnixTargets");
    tArgs.requireDependencies = includeDependencies;

    if (includeDependencies) {
        auto includeTargets = getTecnixBoolAttr(
            state,
            pos,
            args,
            state.symbols.create("includeTargets"),
            "while evaluating the 'includeTargets' argument to builtins.tecnixTargets",
            true);
        prim_tecnixTargetsWithDependencies(state, pos, args, v, std::move(tArgs), includeTargets);
        return;
    }

    if (state.settings.pureEval && state.settings.tecnixEvalCache) {
        prim_tecnixTargetsCached(state, pos, v, tArgs);
        return;
    }

    printTalkative(
        "tecnixTargets: evaluating %d target ref(s)%s, eval cores %d",
        tArgs.targets.size(),
        state.executor->enabled && !Executor::amWorkerThread && tArgs.targets.size() > 1 ? " in parallel"
                                                                                         : " sequentially",
        state.executor->evalCores);

    auto & resolveFn = getResolveFunction(state, pos, tArgs);

    /* These cells are the only reference to each target's result until the
       output bindings are built, so they must live in GC-scanned storage: a
       plain std::vector's heap buffer is invisible to the conservative
       collector, which would recycle the cells mid-evaluation (observed as
       the ValueStorage::finish pdThunk panic, or as silently corrupted
       results). */
    ValueVector values(tArgs.targets.size());
    for (size_t i = 0; i < tArgs.targets.size(); i++)
        values[i] = state.allocValue();

    std::vector<size_t> indices;
    indices.reserve(tArgs.targets.size());
    for (size_t i = 0; i < tArgs.targets.size(); i++)
        indices.push_back(i);

    auto evalTarget = [&](size_t i) {
        auto & target = tArgs.targets[i];
        auto started = std::chrono::steady_clock::now();
        printTalkative(
            "tecnixTargets: start evaluating '%s' on %s thread", target, Executor::amWorkerThread ? "worker" : "main");

        auto * targetArg = state.allocValue();
        targetArg->mkString(target, state.mem);
        state.callFunction(resolveFn, *targetArg, *values[i], pos);
        forceTargetDrvPath(state, *values[i], pos);

        auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        printTalkative("tecnixTargets: finished '%s' in %d ms", target, elapsedMs);
    };

    evalTecnixIndices(state, indices, evalTarget);

    auto rootAttrs = state.buildBindings(tArgs.targets.size());
    for (size_t i = 0; i < tArgs.targets.size(); i++)
        rootAttrs.insert(state.symbols.create(tArgs.targets[i]), values[i]);
    v.mkAttrs(rootAttrs);
}

static RegisterPrimOp primop_tecnixTargets({
    .name = "__tecnixTargets",
    .args = {"attrs"},
    .doc = R"(
      Resolve Tecnix target references via the resolver module. Input `targets`
      is a list of opaque target ID strings. By default, returns an attrset
      keyed by those same strings. With `includeDependencies = true`, returns
      an ordered list of `{ target, value, dependencies }` records, where
      `dependencies` is an attrset of `path = fingerprint`. Add
      `includeTargets = false` to omit `value` from each record.

      Under pure evaluation with `tecnix-eval-cache` enabled, a target whose
      stored source closure still matches the current tree and whose cached
      recipe and selected output remain available is answered from the cache
      without evaluation. Such values preserve the selected output and its
      Nix string context using the imported derivation shape; other resolver
      attributes are not preserved on a value-cache hit.
    )",
    .impl = prim_tecnixTargets,
});

struct TargetDependencyResult
{
    DependencyClosure dependencies;
    std::optional<ValidatedDependencyBlob> dependencyBlob;
    std::optional<SourceAccessSetSnapshot> sourceAccessSetSnapshot;
    Value * targetValue = nullptr;
    /** Miss: versioned drvPath/outputName payload; empty for an unsupported
        target value. Both fields are forced under source tracking. */
    std::string targetPayload;
    bool cacheNeedsUpsert = false;
};

/**
 * `targetValue` may be the only reference to a worker-evaluated target value
 * until the coordinator assembles the output records, so the results buffer
 * must be GC-scanned (see the ValueVector comment in prim_tecnixTargets).
 */
using TargetDependencyResults =
    std::vector<std::optional<TargetDependencyResult>, traceable_allocator<std::optional<TargetDependencyResult>>>;

struct PreparedTrackedResolveFunction
{
    Value * resolveFn;
    EvalSourceAccessSetId sourceDeps = emptyEvalSourceAccessSetId;
};

static PreparedTrackedResolveFunction
prepareTrackedResolveFunction(EvalState & state, const PosIdx pos, const TecnixArgs & tArgs)
{
    if (!tecnixSourceTrackingEnabled(state, tArgs))
        return {
            .resolveFn = &getResolveFunction(state, pos, tArgs),
            .sourceDeps = emptyEvalSourceAccessSetId,
        };

    TrackingContext trackingCtx(state);
    ActiveTrackingContext activeTrackingCtx(trackingCtx);

    TrackedSourceDepsScope sourceDepsScope(trackingCtx);
    auto & resolveFn = getResolveFunction(state, pos, tArgs);
    auto sourceDeps = sourceDepsScope.finish(&resolveFn);

    return {
        .resolveFn = &resolveFn,
        .sourceDeps = sourceDeps,
    };
}

static TargetDependencyResult evalTargetDependencies(
    EvalState & state,
    const PosIdx pos,
    Value & resolveFn,
    EvalSourceAccessSetId resolveSourceDeps,
    const std::string & target,
    bool keepTargetValue,
    bool track,
    bool cachePayload)
{
    auto started = std::chrono::steady_clock::now();
    printTalkative(
        "tecnixTargets dependencies: start evaluating '%s' on %s thread",
        target,
        Executor::amWorkerThread ? "worker" : "main");

    std::optional<TrackingContext> trackingCtx;
    if (track) {
        trackingCtx.emplace(state);
        if (resolveSourceDeps != emptyEvalSourceAccessSetId)
            recordTrackedSourceAccessSetDependency(*trackingCtx, resolveSourceDeps);
    }
    Value * targetValue = nullptr;
    std::string targetPayload;
    {
        std::optional<ActiveTrackingContext> activeTrackingCtx;
        if (trackingCtx)
            activeTrackingCtx.emplace(*trackingCtx);

        auto * targetArg = state.allocValue();
        targetArg->mkString(target, state.mem);
        auto * resolveResult = state.allocValue();
        state.callFunction(resolveFn, *targetArg, *resolveResult, pos);
        auto drvPath = forceTargetDrvPath(state, *resolveResult, pos);
        if (!drvPath.empty()) {
            if (auto outputNameAttr = resolveResult->attrs()->get(state.s.outputName)) {
                state.forceValue(*outputNameAttr->value, pos);
                if (outputNameAttr->value->type() == nString) {
                    NixStringContext context;
                    auto outputName = state.forceString(
                        *outputNameAttr->value,
                        context,
                        pos,
                        "while evaluating the selected output of a tecnix target");
                    if (cachePayload && context.empty() && !outputName.empty()
                        && drvPath.find('\0') == std::string_view::npos
                        && outputName.find('\0') == std::string_view::npos) {
                        targetPayload.reserve(targetValuePayloadPrefix.size() + drvPath.size() + 1 + outputName.size());
                        targetPayload.append(targetValuePayloadPrefix);
                        targetPayload.append(drvPath);
                        targetPayload.push_back('\0');
                        targetPayload.append(outputName);
                    }
                }
            }
        }
        if (keepTargetValue)
            targetValue = resolveResult;
    }

    std::optional<SourceAccessSetSnapshot> snapshot;
    if (trackingCtx)
        snapshot = snapshotSourceAccessSetTracking(*trackingCtx);
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    printTalkative(
        "tecnixTargets dependencies: finished '%s' in %d ms with access-set dependency snapshot", target, elapsedMs);
    return {
        .dependencies = {},
        .sourceAccessSetSnapshot = std::move(snapshot),
        .targetValue = targetValue,
        .targetPayload = std::move(targetPayload),
    };
}

static void finalizeSourceAccessSetDependencies(
    EvalState & state, TargetDependencyResults & results, DependencyFingerprintCache & fingerprintCache)
{
    if (!trackedSourceAccessSetGraph(state)->isEnabled())
        return;

    for (auto & maybeResult : results) {
        if (!maybeResult || !maybeResult->sourceAccessSetSnapshot)
            continue;

        auto trackedPaths = collectSourceAccessSetTrackedPaths(
            trackedSourceAccessSetGraph(state), *maybeResult->sourceAccessSetSnapshot);
        auto dependencies = dependencyFingerprints(getTecnixRepoAccessor(state), trackedPaths, fingerprintCache);
        maybeResult->dependencies = std::move(dependencies);
        maybeResult->sourceAccessSetSnapshot.reset();
    }
}

static void printTecnixAccessSetStats(EvalState & state, std::string_view opName)
{
    auto sourceAccessSetStats = trackedSourceAccessSetStats(state);
    printTalkative(
        "%s: source access-set graph has %d access id(s), %d access set(s), %d access set item(s)",
        opName,
        sourceAccessSetStats.accesses,
        sourceAccessSetStats.accessSets,
        sourceAccessSetStats.accessSetItems);
}

/**
 * Materialize usable target payloads before classifying hits. An unsupported
 * payload, absent recipe, or missing selected output is an ordinary miss.
 */
static ValueVector lookupCachedTargetValues(
    EvalState & state, const PosIdx pos, const std::vector<std::optional<ValidatedDependencyBlob>> & hits)
{
    struct CachedTarget
    {
        StorePath drvPath;
        std::string_view outputName;
    };

    std::vector<std::optional<CachedTarget>> targets(hits.size());
    ValueVector values(hits.size());
    StorePathSet candidates;
    for (size_t i = 0; i < hits.size(); i++) {
        if (!hits[i])
            continue;
        auto payload = hits[i]->payload();
        if (!payload || !payload->starts_with(targetValuePayloadPrefix))
            continue;
        auto fields = payload->substr(targetValuePayloadPrefix.size());
        auto separator = fields.find('\0');
        if (separator == std::string_view::npos)
            continue;
        auto outputName = fields.substr(separator + 1);
        if (outputName.empty() || outputName.find('\0') != std::string_view::npos)
            continue;
        auto storePath = state.store->maybeParseStorePath(fields.substr(0, separator));
        if (!storePath || !storePath->isDerivation())
            continue;
        candidates.insert(*storePath);
        targets[i].emplace(CachedTarget{std::move(*storePath), outputName});
    }
    if (candidates.empty())
        return values;

    auto valid = state.store->queryValidPaths(candidates);
    size_t valueHits = 0;
    for (size_t i = 0; i < targets.size(); i++) {
        if (!targets[i] || !valid.count(targets[i]->drvPath))
            continue;
        auto & target = *targets[i];
        auto * imported = state.allocValue();
        derivationToValue(
            state,
            pos,
            SourcePath(state.rootFS, CanonPath(state.store->printStorePath(target.drvPath))),
            target.drvPath,
            *imported);
        auto output = imported->attrs()->get(state.symbols.create(target.outputName));
        if (!output)
            continue;
        state.forceValue(*output->value, pos);
        if (output->value->type() != nAttrs)
            continue;
        values[i] = output->value;
        valueHits++;
    }
    if (valueHits)
        printTalkative("tecnixTargets: %d target value(s) served from the cache", valueHits);
    return values;
}

static TargetDependencyResults evaluateTecnixTargetDependencies(
    EvalState & state,
    const PosIdx pos,
    const TecnixArgs & args,
    DependencyFingerprintCache & fingerprintCache,
    bool keepTargetValues = false)
{
    bool useCache = state.settings.pureEval && state.settings.tecnixEvalCache;
    printTalkative(
        "tecnixTargets dependencies: planning %d target ref(s), dependency cache %s, eval cores %d",
        args.targets.size(),
        useCache ? "enabled" : "disabled",
        state.executor->evalCores);

    TargetDependencyResults results(args.targets.size());
    std::vector<size_t> misses;
    size_t cacheHits = 0;

    if (useCache) {
        auto hits = lookupValidatedDependencyBlobs(
            state,
            cacheScope(args),
            std::span<const std::string>{args.targets.data(), args.targets.size()},
            fingerprintCache);
        // Target values need a supported payload and a locally available
        // recipe exposing the selected output. Anything else re-evaluates.
        ValueVector cachedValues;
        if (keepTargetValues)
            cachedValues = lookupCachedTargetValues(state, pos, hits);
        for (size_t i = 0; i < hits.size(); i++) {
            if (!hits[i]) {
                printTalkative("tecnixTargets dependencies: dependency cache miss, evaluating '%s'", args.targets[i]);
                continue;
            }
            if (keepTargetValues && !cachedValues[i]) {
                printTalkative(
                    "tecnixTargets dependencies: dependency cache hit for '%s' has no valid target value, evaluating",
                    args.targets[i]);
                continue;
            }
            printTalkative("tecnixTargets dependencies: dependency cache hit for '%s'", args.targets[i]);
            results[i].emplace();
            results[i]->dependencyBlob = std::move(hits[i]);
            if (keepTargetValues)
                results[i]->targetValue = cachedValues[i];
            cacheHits++;
        }
    }

    for (size_t i = 0; i < args.targets.size(); i++) {
        if (!results[i])
            misses.push_back(i);
    }

    bool allowParallelDependencies = state.settings.tecnixParallelDependencies && state.executor->enabled
                                     && state.executor->evalCores > 1 && !Executor::amWorkerThread && misses.size() > 1;
    printTalkative(
        "tecnixTargets dependencies: %d cache hit(s), %d target ref(s) to evaluate%s",
        cacheHits,
        misses.size(),
        allowParallelDependencies ? " in parallel" : " sequentially");

    if (!misses.empty()) {
        auto preparedResolve = prepareTrackedResolveFunction(state, pos, args);

        auto evalMiss = [&](size_t i) {
            auto & target = args.targets[i];
            results[i] = evalTargetDependencies(
                state,
                pos,
                *preparedResolve.resolveFn,
                preparedResolve.sourceDeps,
                target,
                keepTargetValues,
                tecnixSourceTrackingEnabled(state, args),
                useCache);
            if (results[i])
                results[i]->cacheNeedsUpsert = true;
        };

        evalTecnixIndices(state, misses, evalMiss, allowParallelDependencies);
        finalizeSourceAccessSetDependencies(state, results, fingerprintCache);

        if (useCache) {
            std::vector<TecnixDependencyUpsert> upserts;
            upserts.reserve(misses.size());
            for (auto i : misses) {
                if (results[i] && results[i]->cacheNeedsUpsert)
                    upserts.push_back(
                        {args.targets[i], &results[i]->dependencies, std::move(results[i]->targetPayload)});
            }
            upsertDependencyClosures(cacheScope(args), upserts);
        }
    }

    return results;
}

/**
 * The cached form of plain `builtins.tecnixTargets`: prove stored source
 * closures, answer proven targets from their selected-output payloads, and
 * evaluate (and cache) the rest.
 */
static void prim_tecnixTargetsCached(EvalState & state, const PosIdx pos, Value & v, const TecnixArgs & tArgs)
{
    DependencyFingerprintCache fingerprintCache;
    auto results = evaluateTecnixTargetDependencies(state, pos, tArgs, fingerprintCache, /*keepTargetValues=*/true);
    printTecnixAccessSetStats(state, "tecnixTargets");

    auto rootAttrs = state.buildBindings(tArgs.targets.size());
    for (size_t i = 0; i < tArgs.targets.size(); i++)
        rootAttrs.insert(state.symbols.create(tArgs.targets[i]), results[i]->targetValue);
    v.mkAttrs(rootAttrs);
}

static void prim_tecnixTargetsWithDependencies(
    EvalState & state, const PosIdx pos, Value **, Value & v, TecnixArgs && tArgs, bool includeTargets)
{
    DependencyFingerprintCache fingerprintCache;
    auto results = evaluateTecnixTargetDependencies(state, pos, tArgs, fingerprintCache, includeTargets);
    printTecnixAccessSetStats(state, "tecnixTargets");
    auto list = state.buildList(tArgs.targets.size());
    for (size_t i = 0; i < tArgs.targets.size(); i++) {
        auto & result = *results[i];
        auto * targetValue = state.allocValue();
        targetValue->mkString(tArgs.targets[i], state.mem);
        auto * dependenciesValue = result.dependencyBlob ? result.dependencyBlob->toValue(state)
                                                         : dependencyAttrsToValue(state, result.dependencies);

        auto attrs = state.buildBindings(includeTargets ? 3 : 2);
        attrs.insert(state.symbols.create("target"), targetValue);
        if (includeTargets)
            attrs.insert(state.symbols.create("value"), result.targetValue);
        attrs.insert(state.symbols.create("dependencies"), dependenciesValue);

        auto * recordValue = state.allocValue();
        recordValue->mkAttrs(attrs);
        list[i] = recordValue;
    }

    v.mkList(list);
}

// ============================================================================
// builtins.tecnixTargetNames { gitDir, resolver, args, ... }
// Discovers fully-qualified target names via the Tecnix module contract.
// Caches the discovered names using the same path -> fingerprint validation as
// dependency discovery, so repeated discovery for an unchanged source is cheap.
// ============================================================================
static void prim_tecnixTargetNames(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto dArgs = parseTecnixArgs(state, pos, args, false);
    configureTecnixRepoContext(state, dArgs);

    auto includeDependencies = getTecnixBoolAttr(
        state,
        pos,
        args,
        state.symbols.create("includeDependencies"),
        "while evaluating the 'includeDependencies' argument to builtins.tecnixTargetNames");
    dArgs.requireDependencies = includeDependencies;

    DependencyFingerprintCache fingerprintCache;
    auto result = discoverTecnixTargetNames(state, pos, dArgs, fingerprintCache);

    auto list = state.buildList(result.targetNames.size());
    for (size_t i = 0; i < result.targetNames.size(); i++)
        list[i] = targetRefToValue(state, result.targetNames[i]);

    if (!includeDependencies) {
        v.mkList(list);
        return;
    }

    auto * targetsValue = state.allocValue();
    targetsValue->mkList(list);
    auto * dependenciesValue = result.dependencyBlob ? result.dependencyBlob->toValue(state)
                                                     : dependencyAttrsToValue(state, result.dependencies);
    auto rootAttrs = state.buildBindings(2);
    rootAttrs.insert(state.symbols.create("targets"), targetsValue);
    rootAttrs.insert(state.symbols.create("dependencies"), dependenciesValue);
    v.mkAttrs(rootAttrs);
}

static RegisterPrimOp primop_tecnixTargetNames({
    .name = "__tecnixTargetNames",
    .args = {"attrs"},
    .doc = R"(
      Discover Tecnix target references by passing `args` to the resolver. By
      default, returns a flat list of opaque target ID strings supplied by the
      resolver. With `includeDependencies = true`, returns
      `{ targets = [ ... ]; dependencies = { path = fingerprint; ... }; }`.
      `args` must be JSON-convertible for cache keying.
    )",
    .impl = prim_tecnixTargetNames,
});

} // namespace nix
