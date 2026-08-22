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
 * Import the explicit resolver file from the git repo and return a value from
 * the attrset produced by calling it with `args` (e.g. `resolve` or
 * `allTargetNames`).
 *
 * Requires configureTecnixRepoContext() to have been called first.
 */
static Value &
getTecnixModuleValue(EvalState & state, const PosIdx pos, const TecnixArgs & tArgs, std::string_view attrName)
{
    // Get resolver file path from the lazily-mounted Tecnix repo accessor.
    auto resolverPath = getTecnixRepoPath(state, tArgs.resolver);
    auto modulePath = SourcePath(state.rootFS, CanonPath(resolverPath));

    // Import the resolver file (a function taking the opaque `args` value) and call it.
    auto * moduleFn = state.allocValue();
    state.evalFile(modulePath, *moduleFn);

    if (!tArgs.resolverArgs)
        state.error<EvalError>("missing Tecnix resolver args").atPos(pos).debugThrow();

    auto * moduleVal = state.allocValue();
    state.callFunction(*moduleFn, *tArgs.resolverArgs, *moduleVal, pos);
    state.forceAttrs(*moduleVal, pos, "while evaluating tecnix module");

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
    snapshot.directAccesses.assign(
        ctx.rootFrame.directSourceAccessSetAccesses.begin(), ctx.rootFrame.directSourceAccessSetAccesses.end());
    snapshot.accessSetEdges.assign(
        ctx.rootFrame.childSourceAccessSets.begin(), ctx.rootFrame.childSourceAccessSets.end());
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
    TrackingContext trackingCtx(state);
    std::vector<std::string> targetNames;
    {
        ActiveTrackingContext activeTrackingCtx(trackingCtx);
        targetNames = evalTargetNamesOnly(state, pos, tArgs);
    }
    auto trackedPaths = collectSourceAccessSetTrackedPaths(trackingCtx);
    auto dependencies = dependencyFingerprints(getTecnixRepoAccessor(state), trackedPaths, fingerprintCache);

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

static void forceTargetDrvPath(EvalState & state, Value & targetValue, const PosIdx pos)
{
    state.forceValue(targetValue, pos);
    if (targetValue.type() != nAttrs)
        return;

    auto drvPathAttr = targetValue.attrs()->get(state.s.drvPath);
    if (!drvPathAttr)
        return;

    NixStringContext context;
    state.forceString(*drvPathAttr->value, context, pos, "while evaluating the 'drvPath' attribute of a tecnix target");
}

static void prim_tecnixTargetsWithDependencies(
    EvalState & state, const PosIdx pos, Value ** args, Value & v, TecnixArgs && tArgs, bool includeTargets);

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
    )",
    .impl = prim_tecnixTargets,
});

struct TargetDependencyResult
{
    DependencyClosure dependencies;
    std::optional<ValidatedDependencyBlob> dependencyBlob;
    std::optional<SourceAccessSetSnapshot> sourceAccessSetSnapshot;
    Value * targetValue = nullptr;
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
    bool keepTargetValue)
{
    auto started = std::chrono::steady_clock::now();
    printTalkative(
        "tecnixTargets dependencies: start evaluating '%s' on %s thread",
        target,
        Executor::amWorkerThread ? "worker" : "main");

    TrackingContext trackingCtx(state);
    if (resolveSourceDeps != emptyEvalSourceAccessSetId)
        recordTrackedSourceAccessSetDependency(trackingCtx, resolveSourceDeps);
    Value * targetValue = nullptr;
    {
        ActiveTrackingContext activeTrackingCtx(trackingCtx);

        auto * targetArg = state.allocValue();
        targetArg->mkString(target, state.mem);
        auto * resolveResult = state.allocValue();
        state.callFunction(resolveFn, *targetArg, *resolveResult, pos);
        forceTargetDrvPath(state, *resolveResult, pos);
        if (keepTargetValue)
            targetValue = resolveResult;
    }

    auto snapshot = snapshotSourceAccessSetTracking(trackingCtx);
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    printTalkative(
        "tecnixTargets dependencies: finished '%s' in %d ms with access-set dependency snapshot", target, elapsedMs);
    return {
        .dependencies = {},
        .sourceAccessSetSnapshot = std::move(snapshot),
        .targetValue = targetValue,
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
        for (size_t i = 0; i < hits.size(); i++) {
            if (!hits[i]) {
                printTalkative("tecnixTargets dependencies: dependency cache miss, evaluating '%s'", args.targets[i]);
                continue;
            }
            printTalkative("tecnixTargets dependencies: dependency cache hit for '%s'", args.targets[i]);
            results[i].emplace();
            results[i]->dependencyBlob = std::move(hits[i]);
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
                state, pos, *preparedResolve.resolveFn, preparedResolve.sourceDeps, target, keepTargetValues);
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
                    upserts.push_back({args.targets[i], &results[i]->dependencies, {}});
            }
            upsertDependencyClosures(cacheScope(args), upserts);
        }
    }

    return results;
}

static void prim_tecnixTargetsWithDependencies(
    EvalState & state, const PosIdx pos, Value **, Value & v, TecnixArgs && tArgs, bool includeTargets)
{
    DependencyFingerprintCache fingerprintCache;
    auto results = evaluateTecnixTargetDependencies(state, pos, tArgs, fingerprintCache, includeTargets);
    printTecnixAccessSetStats(state, "tecnixTargets");

    if (includeTargets) {
        std::vector<size_t> missingTargetValueIndices;
        missingTargetValueIndices.reserve(tArgs.targets.size());
        for (size_t i = 0; i < tArgs.targets.size(); i++) {
            assert(results[i]);
            if (!results[i]->targetValue)
                missingTargetValueIndices.push_back(i);
        }

        if (!missingTargetValueIndices.empty()) {
            auto & resolveFn = getResolveFunction(state, pos, tArgs);
            auto evalTarget = [&](size_t i) {
                auto * targetValue = state.allocValue();
                auto * targetArg = state.allocValue();
                targetArg->mkString(tArgs.targets[i], state.mem);
                state.callFunction(resolveFn, *targetArg, *targetValue, pos);
                forceTargetDrvPath(state, *targetValue, pos);
                results[i]->targetValue = targetValue;
            };
            evalTecnixIndices(state, missingTargetValueIndices, evalTarget);
        }
    }

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
