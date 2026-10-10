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
#include "nix/util/terminal.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/ingestion-stats.hh"
#include "nix/expr/tecnix/access-set-graph.hh"
#include "nix/expr/tecnix/eval-cache.hh"
#include "nix/expr/tecnix/memo-value.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/util/finally.hh"
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
// builtins.tecnixMemoize f k
// `builtins.tecnixMemoize f` is `f`, memoized: calling it with a string `k`
// returns `f k`, evaluated at most once per EvalState for that `f` and `k`
// (once under tracking, once without). Under tracking, that evaluation
// records its source accesses into a label stored alongside the value; every
// later tracked consumer records that label as a child of its own frame (and
// inherits the value's label through the value-copy hooks), so all consumers
// share the same tracked dependencies without re-evaluating `f k`.
// ============================================================================

/* Calls whose `f k` evaluation is in progress on this thread. Re-entering one
   (zone A loading itself transitively) can only be a genuine cycle; the guard
   turns it into a catchable error instead of unbounded recursion. The key
   views borrow from the calls' own arguments, which outlive their entries. */
static thread_local std::vector<EvalTecnixMemoizeKey::Lookup> tecnixMemoizeInProgress;

static void prim_tecnixMemoize(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto * function = args[0];
    auto key = state.forceStringNoCtx(
        *args[1], pos, "while evaluating the key passed to a function memoized by builtins.tecnixMemoize");
    EvalTecnixMemoizeKey::Lookup lookup{function, key};

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
        trackingCtx ? *state.tecnixEvalData().trackedTecnixMemoizeCache : *state.tecnixEvalData().tecnixMemoizeCache;

    Value * result = nullptr;
    auto sourceDeps = emptyEvalSourceAccessSetId;
    if (memoCache.cvisit(lookup, [&](const auto & entry) {
            result = *entry.second.value;
            sourceDeps = entry.second.sourceDeps;
        })) {
        // Replay into this consumer the sources the entry's evaluation read.
        if (trackingCtx)
            recordTrackedSourceAccessSetDependency(*trackingCtx, sourceDeps);
        v = *result;
        return;
    }

    for (auto & inProgress : tecnixMemoizeInProgress)
        if (EvalTecnixMemoizeKey::Equal{}(inProgress, lookup))
            // A value that needs itself: fail as infinite recursion does, which
            // builtins.tryEval cannot catch.
            state
                .error<InfiniteRecursionError>("builtins.tecnixMemoize: circular evaluation detected for key '%s'", key)
                .atPos(pos)
                .debugThrow();

    tecnixMemoizeInProgress.push_back(lookup);
    Finally popInProgress([&]() { tecnixMemoizeInProgress.pop_back(); });

    // Evaluate `f k` outside any map bucket lock, so re-entrant misses (zone A
    // loading zone B) can insert their own entries without deadlock. Under
    // tracking, scope the evaluation so its accesses intern into one reusable
    // label that every later consumer records.
    //
    // `f` gets a fresh copy of the key, not `args[1]`: that value carries the
    // label of however this caller computed `k` (already recorded for this
    // caller by the force above), and `f` forcing it inside the scope would
    // store that label with the shared entry and replay it into every later
    // caller. The key is a string without context, so the copy is equal.
    auto * keyArg = state.allocValue();
    keyArg->mkString(key, state.mem);
    result = state.allocValue();
    if (trackingCtx) {
        TrackedSourceDepsScope scope(*trackingCtx);
        state.callFunction(*function, *keyArg, *result, pos);
        state.forceValue(*result, pos);
        sourceDeps = scope.finish(result);
    } else {
        state.callFunction(*function, *keyArg, *result, pos);
        state.forceValue(*result, pos);
    }

    // Insert, but if a parallel evaluation raced ahead, reuse its entry so the
    // table keeps a single shared value.
    memoCache.try_emplace_and_cvisit(
        EvalTecnixMemoizeKey{function, std::string(key)},
        EvalTecnixMemoizeEntry{},
        [&](auto & entry) {
            entry.second.function = RootValue(function);
            entry.second.value = RootValue(result);
            entry.second.sourceDeps = sourceDeps;
        },
        [&](const auto & entry) {
            result = *entry.second.value;
            sourceDeps = entry.second.sourceDeps;
        });

    if (trackingCtx)
        recordTrackedSourceAccessSetDependency(*trackingCtx, sourceDeps);
    v = *result;
}

static RegisterPrimOp primop_tecnixMemoize({
    .name = "__tecnixMemoize",
    .args = {"f", "k"},
    .doc = R"(
      Memoize the function `f`: `builtins.tecnixMemoize f` is a function `g`
      such that `g k` is always `f k`, but each result is computed once and
      then shared. Bind `g` once and call it wherever `f k` is needed.

      Results are keyed by `f` and `k`. `f` is identified by its value (the
      same binding), not by what it computes: every `g` made from the same
      value of `f` shares its results, and a different function (another
      system's loader, say) never sees them, even for equal keys. `k` must be
      a string without string context; two keys are equal when they are the
      same string. `f` is called with a string equal to `k`.

      Results live as long as the evaluator (one `EvalState`) and are never
      freed. A call whose `f k` throws stores nothing, so the next call
      evaluates `f k` again.

      Under Tecnix source tracking, the evaluation of `f k` records the
      sources it reads, and every tracked call that reuses the result records
      them too, so each target depends on everything the shared value was
      computed from. The sources a caller read to compute `k` count for that
      caller only; they are never stored with the shared result. Tracked and
      untracked calls keep separate results, each computed once: neither kind
      of call is ever handed the other's, since an untracked result has no
      sources recorded, and forcing parts of a tracked result outside tracking
      would lose theirs.

      If computing `f k` calls `g k` again, evaluation fails as it does for
      infinite recursion, and `builtins.tryEval` cannot catch the error.

      Example:

      ```nix
      let
        zone = builtins.tecnixMemoize (zonePath: import ./load-zone.nix zonePath);
      in
        [ (zone "//areas/tools/dev") (zone "//areas/tools/dev") ]
      ```

      evaluates `import ./load-zone.nix "//areas/tools/dev"` once.
    )",
    .impl = prim_tecnixMemoize,
});

// ============================================================================
// builtins.tecnixPersistentMemo ns key f
// `f key`, kept across evaluations: inside a cached `builtins.tecnixTargets`
// evaluation the deeply forced result is stored as data, with the source
// closure it was computed from, in the target cache's database, and a later
// evaluation that proves that closure against its own tree reuses it.
// ============================================================================

/**
 * The row family one cached `tecnixTargets` call stores its memo rows in: its
 * own (resolver, argsKey) scope, set apart from target rows by a reserved
 * resolver prefix, and tied to this evaluator's version, since a row holds
 * evaluation *results* (derivation paths among them) rather than just a
 * closure.
 *
 * The parts are joined with U+001F, not NUL: the cache binds `resolver` and
 * `argsKey` as SQLite text up to the first NUL, so a NUL would drop the
 * resolver and the versions from the stored scope. `argsKey` is canonical
 * JSON, which escapes control characters, so the join stays unambiguous.
 */
struct TecnixMemoRowFamily
{
    std::string resolver;
    std::string argsKey;
    DependencyFingerprintCache * fingerprintCache;
    size_t historyLimit;

    /** Results already produced in this call, so repeated calls in one
        target neither re-read nor re-evaluate. Keyed by row key. */
    std::mutex lock;
    std::unordered_map<std::string, std::pair<RootValue, EvalSourceAccessSetId>> values;

    TecnixMemoRowFamily(
        std::string_view resolver_,
        std::string_view argsKey_,
        DependencyFingerprintCache & fingerprintCache_,
        size_t historyLimit_)
        : resolver(std::string("__tecnixPersistentMemo\x1f") + std::string(resolver_))
        , argsKey(std::string(argsKey_) + "\x1fnix=" + nixVersion + "\x1ftecnix=" + tecnixVersion)
        , fingerprintCache(&fingerprintCache_)
        , historyLimit(historyLimit_)
    {
        assert(resolver.find('\0') == std::string::npos && argsKey.find('\0') == std::string::npos);
    }
};

/** The target this thread is evaluating for a cached `tecnixTargets` call, if any. */
struct TecnixMemoTarget
{
    TecnixMemoRowFamily * family = nullptr;
    std::string_view target;
};

static thread_local TecnixMemoTarget currentTecnixMemoTarget;

/**
 * A memo row is keyed by the enclosing target as well as `ns` and `key`: `f`
 * can close over values derived from the target id (the zone's own path, say)
 * that are not source reads, so only the target id makes them part of the key.
 */
static std::string tecnixMemoRowKey(std::string_view target, std::string_view ns, std::string_view key)
{
    std::string rowKey;
    rowKey.reserve(target.size() + ns.size() + key.size() + 2);
    rowKey.append(target);
    rowKey.push_back('\0');
    rowKey.append(ns);
    rowKey.push_back('\0');
    rowKey.append(key);
    return rowKey;
}

/** Intern a stored closure's paths as one access set of `trackingCtx`'s graph. */
static EvalSourceAccessSetId internTrackedPaths(TrackingContext & trackingCtx, const std::vector<std::string> & paths)
{
    std::vector<EvalSourceAccessId> ids;
    ids.reserve(paths.size());
    for (auto & path : paths)
        ids.push_back(trackingCtx.sourceAccessSetGraph->internAccess(path));
    return trackingCtx.sourceAccessSetGraph->internAccessSet(ids, {});
}

/**
 * A stored result, if one proves out: its closure matches this tree (checked
 * by the lookup), it decodes, and every store path its string contexts name
 * is still valid. Anything less is a miss.
 */
static Value * lookupPersistentMemo(
    EvalState & state,
    TrackingContext & trackingCtx,
    TecnixMemoRowFamily & family,
    const std::string & rowKey,
    EvalSourceAccessSetId & sourceDeps)
{
    std::optional<std::string> payload;
    std::vector<std::string> closure;
    std::array<std::string, 1> keys{rowKey};
    lookupCachedDependencies(
        state,
        TecnixCacheScope{family.resolver, family.argsKey},
        keys,
        *family.fingerprintCache,
        [&](size_t, const DependencyCacheHit & hit) {
            if (auto p = hit.payload()) {
                payload.emplace(*p);
                closure = hit.paths();
            }
        });
    if (!payload)
        return nullptr;

    auto decoded = deserializeTecnixMemoValue(state, *payload);
    if (!decoded)
        return nullptr;
    if (!decoded->storePaths.empty()
        && state.store->queryValidPaths(decoded->storePaths).size() != decoded->storePaths.size())
        return nullptr;

    sourceDeps = internTrackedPaths(trackingCtx, closure);
    if (sourceDeps != emptyEvalSourceAccessSetId)
        decoded->value->setTrackedSourceAccessSet(sourceDeps);
    return decoded->value;
}

/**
 * Store a fresh result. Its closure is what its evaluation read plus every Nix
 * file read so far (see `recordTecnixCodeFile`). A result that is not data, or
 * a closure that can't be fingerprinted, is simply not stored.
 */
static void storePersistentMemo(
    EvalState & state,
    TecnixMemoRowFamily & family,
    const std::string & rowKey,
    Value & result,
    EvalSourceAccessSetId sourceDeps)
{
    std::string payload;
    try {
        payload = serializeTecnixMemoValue(state, result);
    } catch (TecnixMemoUnserializable & e) {
        printTalkative("tecnixPersistentMemo: not storing '%s': %s", rowKey, e.msg());
        return;
    }

    auto paths = sourceDeps == emptyEvalSourceAccessSetId
                     ? std::vector<std::string>{}
                     : trackedSourceAccessSetGraph(state)->flatten({}, {sourceDeps});
    auto code = tecnixCodeFiles();
    paths.insert(paths.end(), code.begin(), code.end());
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    DependencyClosure dependencies;
    try {
        dependencies = dependencyFingerprints(getTecnixRepoAccessor(state), paths, *family.fingerprintCache);
    } catch (Error & e) {
        printTalkative("tecnixPersistentMemo: not storing '%s': %s", rowKey, e.msg());
        return;
    }

    std::vector<TecnixDependencyUpsert> upserts{{rowKey, &dependencies, std::move(payload)}};
    upsertDependencyClosures(TecnixCacheScope{family.resolver, family.argsKey}, upserts, family.historyLimit);
}

/**
 * What a `tecnixPersistentMemo` call computes, stores and returns. The plain
 * form (`f`) stores and returns `f key` itself; the projecting form
 * (`{ compute, project, load }`) computes the real value, stores `project`
 * of it as data, and turns stored data back into a value with `load`.
 */
struct TecnixMemoFunctions
{
    Value * compute;
    Value * project = nullptr;
    Value * load = nullptr;
};

static TecnixMemoFunctions parseTecnixMemoFunctions(EvalState & state, const PosIdx pos, Value & arg)
{
    state.forceValue(arg, pos);
    if (arg.type() != nAttrs)
        return {.compute = &arg};
    auto get = [&](std::string_view name) {
        auto * attr = arg.attrs()->get(state.symbols.create(name));
        if (!attr)
            state
                .error<EvalError>(
                    "builtins.tecnixPersistentMemo: the attribute set form needs 'compute', 'project' and 'load'; '%s' is missing",
                    name)
                .atPos(pos)
                .debugThrow();
        return attr->value;
    };
    return {.compute = get("compute"), .project = get("project"), .load = get("load")};
}

static void prim_tecnixPersistentMemo(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto ns =
        state.forceStringNoCtx(*args[0], pos, "while evaluating the namespace passed to builtins.tecnixPersistentMemo");
    auto key =
        state.forceStringNoCtx(*args[1], pos, "while evaluating the key passed to builtins.tecnixPersistentMemo");
    auto functions = parseTecnixMemoFunctions(state, pos, *args[2]);

    // A fresh key, for the same reason as in tecnixMemoize: `f` must not
    // inherit the label of however the caller computed it.
    auto * keyArg = state.allocValue();
    keyArg->mkString(key, state.mem);

    auto * trackingCtx = currentTecnixThreadState.trackingContext;
    auto memoTarget = currentTecnixMemoTarget;
    if (!trackingCtx || !memoTarget.family) {
        // Not inside a cached target evaluation: just the value, with no
        // projection to pay for.
        state.callFunction(*functions.compute, *keyArg, v, pos);
        return;
    }
    auto & family = *memoTarget.family;
    auto rowKey = tecnixMemoRowKey(memoTarget.target, ns, key);

    {
        std::lock_guard guard(family.lock);
        if (auto i = family.values.find(rowKey); i != family.values.end()) {
            recordTrackedSourceAccessSetDependency(*trackingCtx, i->second.second);
            v = **i->second.first;
            return;
        }
    }

    auto sourceDeps = emptyEvalSourceAccessSetId;
    Value * result = nullptr;
    if (auto * stored = lookupPersistentMemo(state, *trackingCtx, family, rowKey, sourceDeps)) {
        printTalkative("tecnixPersistentMemo: hit for '%s' / '%s' in '%s'", ns, key, memoTarget.target);
        recordTrackedSourceAccessSetDependency(*trackingCtx, sourceDeps);
        if (functions.load) {
            result = state.allocValue();
            state.callFunction(*functions.load, *stored, *result, pos);
        } else
            result = stored;
    } else {
        printTalkative("tecnixPersistentMemo: miss for '%s' / '%s' in '%s'", ns, key, memoTarget.target);
        result = state.allocValue();
        auto * data = result;
        {
            TrackedSourceDepsScope scope(*trackingCtx);
            // The functions were forced before this scope began (to tell the
            // two forms apart); whatever their own evaluation read belongs to
            // this result too.
            for (auto * fn : {args[2], functions.compute, functions.project, functions.load})
                if (fn)
                    if (auto set = fn->trackedSourceAccessSet(); set != emptyEvalSourceAccessSetId)
                        recordTrackedSourceAccessSetDependency(*trackingCtx, set);
            state.callFunction(*functions.compute, *keyArg, *result, pos);
            if (functions.project) {
                data = state.allocValue();
                state.callFunction(*functions.project, *result, *data, pos);
            }
            // Deeply, so the closure is complete and the stored value is data.
            state.forceValueDeep(*data);
            // Records the closure into this caller's frame too.
            sourceDeps = scope.finish(result);
        }
        storePersistentMemo(state, family, rowKey, *data, sourceDeps);
    }

    {
        std::lock_guard guard(family.lock);
        family.values.try_emplace(rowKey, RootValue(result), sourceDeps);
    }
    v = *result;
}

static RegisterPrimOp primop_tecnixPersistentMemo({
    .name = "__tecnixPersistentMemo",
    .args = {"ns", "key", "f"},
    .doc = R"(
      Return `f key`, reusing it from an earlier evaluation when that is
      provably the same value.

      Outside a cached `builtins.tecnixTargets` evaluation (pure evaluation
      with `tecnix-eval-cache` and `tecnix-persistent-memo`), this is just
      `f key`.

      Inside one, the result is looked up under the target being evaluated,
      `ns` and `key` (both strings without context). A stored result is used
      only if every source path it was computed from still has the same
      fingerprint in the evaluated tree, and every store path in its string
      contexts is still valid. The stored closure covers what computing it
      read, plus every Nix file the evaluator had read when it was stored, so
      a change to the code that computes it invalidates it too. The value's
      sources are recorded for the target either way, so the target's own
      cache entry stays exact.

      Otherwise `f key` is evaluated, forced deeply, and stored, provided it
      is data: null, booleans, numbers, strings (with their contexts), lists
      and attribute sets. A result holding a function or a path is returned
      as usual and not stored.

      For a value that isn't data (a derivation, say), pass
      `{ compute, project, load }` as `f` instead: the value is `compute key`;
      what is stored is `project` of it, which must be data; and a stored
      result comes back as `load` of the stored data. Outside a cached target
      evaluation, and on a miss, it is `compute key` itself, so `project` and
      `load` must agree with it on whatever the caller reads.

      `ns` and `key` together must name what is computed for this target:
      two calls in one target with the same `ns` and `key` share one result.
    )",
    .impl = prim_tecnixPersistentMemo,
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
    /** The key memo rows are scoped by: `memoArgs`' canonical JSON if given,
        else `argsKey`. */
    std::string memoArgsKey;
    std::vector<std::string> targets;
    /** The caller asked for the tracked source closure (`includeDependencies`),
        so it must be computed even when the eval cache would not need it. */
    bool requireDependencies = false;
    /** Record a target's evaluation error in its result record instead of failing the whole
        call (`keepGoing`, with `includeDependencies`). */
    bool keepGoing = false;
};

/** The persistent-cache row family these arguments address. */
static TecnixCacheScope cacheScope(const TecnixArgs & args)
{
    return {args.resolver, args.argsKey};
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
    if (auto memoArgsAttr = attrs.get(state.symbols.create("memoArgs")))
        result.memoArgsKey = canonicalJsonFromValue(state, *memoArgsAttr->value, memoArgsAttr->pos).dump();
    else
        result.memoArgsKey = argsKey;
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
    /** The proven cached closure as `path = fingerprint` attrs; set on a cache
        hit when the caller asked for dependencies. */
    Value * cachedDependencies = nullptr;
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
        std::optional<TecnixDiscoveryResult> cached;
        lookupCachedDependencies(
            state,
            cacheScope(tArgs),
            std::span<const std::string>{&cacheKey, 1},
            fingerprintCache,
            [&](size_t, const DependencyCacheHit & hit) {
                auto payload = hit.payload();
                if (!payload)
                    return;
                std::vector<std::string> targetNames;
                try {
                    targetNames = nlohmann::json::parse(*payload).get<std::vector<std::string>>();
                } catch (const nlohmann::json::exception &) {
                    // A malformed payload is a cache miss, never an error.
                    return;
                }
                cached = TecnixDiscoveryResult{.targetNames = std::move(targetNames)};
                if (tArgs.requireDependencies)
                    cached->cachedDependencies = hit.toValue(state);
            });
        if (cached) {
            printTalkative("tecnixTargetNames: discovery cache hit");
            return std::move(*cached);
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
        upsertDependencyClosures(cacheScope(tArgs), upserts, state.settings.tecnixEvalCacheHistory);
    }

    return {.targetNames = std::move(targetNames), .dependencies = std::move(dependencies)};
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
    tArgs.keepGoing = getTecnixBoolAttr(
        state,
        pos,
        args,
        state.symbols.create("keepGoing"),
        "while evaluating the 'keepGoing' argument to builtins.tecnixTargets");
    if (tArgs.keepGoing && !includeDependencies)
        state.error<EvalError>("builtins.tecnixTargets: 'keepGoing' requires 'includeDependencies = true'")
            .atPos(pos)
            .debugThrow();

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
      `includeTargets = false` to omit `value` from each record. With
      `keepGoing = true` (requires `includeDependencies`), a target whose
      evaluation fails yields `{ target, error }` instead of failing the call.

      Under pure evaluation with `tecnix-eval-cache` enabled, a target whose
      stored source closure still matches the current tree and whose cached
      recipe and selected output remain available is answered from the cache
      without evaluation. Such values preserve the selected output and its
      Nix string context using the imported derivation shape; other resolver
      attributes are not preserved on a value-cache hit.

      Target rows are keyed by `args`. `builtins.tecnixPersistentMemo` rows
      made while evaluating the targets are keyed by `memoArgs` instead, when
      given: memoized values are then shared by calls whose `args` differ
      outside `memoArgs`. The caller vouches that what differs (local state
      passed to the targets, say) reaches no memoized value except through
      its memo key.
    )",
    .impl = prim_tecnixTargets,
});

/** A hit's parsed target payload: the recipe and selected output the cached
    target value is served from once the drv is verified present in the store. */
struct CachedTargetPayload
{
    StorePath drvPath;
    std::string outputName;
};

struct TargetDependencyResult
{
    DependencyClosure dependencies;
    /** Hit: the proven candidate's closure as `path = fingerprint` attrs, built
        while its cache row was loaded, when the caller asked for dependencies. */
    Value * cachedDependencies = nullptr;
    std::optional<SourceAccessSetSnapshot> sourceAccessSetSnapshot;
    Value * targetValue = nullptr;
    /** Hit: the candidate's parsed payload, copied out while its cache row was
        loaded. */
    std::optional<CachedTargetPayload> cachedTarget;
    /** Miss: versioned drvPath/outputName payload; empty for an unsupported
        target value. Both fields are forced under source tracking. */
    std::string targetPayload;
    bool cacheNeedsUpsert = false;
};

/**
 * `targetValue` may be the only reference to a worker-evaluated target value
 * until the coordinator assembles the output records, and `cachedDependencies`
 * the only reference to attrs built during the cache lookup, so the results
 * buffer must be GC-scanned (see the ValueVector comment in prim_tecnixTargets).
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
    TecnixMemoRowFamily * memoFamily = nullptr)
{
    auto started = std::chrono::steady_clock::now();
    IngestionTargetScope ingestionTargetScope(target);
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

        // Persistent memo calls made while evaluating this target store
        // their rows under it (see tecnixPersistentMemo).
        auto previousMemoTarget = currentTecnixMemoTarget;
        if (trackingCtx && memoFamily)
            currentTecnixMemoTarget = {.family = memoFamily, .target = target};
        Finally restoreMemoTarget([&]() { currentTecnixMemoTarget = previousMemoTarget; });

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
                    if (context.empty() && !outputName.empty() && drvPath.find('\0') == std::string_view::npos
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
 * Parse a candidate's versioned target payload, copying both fields out of the
 * cache row. An unsupported payload is nullopt (an ordinary miss).
 */
static std::optional<CachedTargetPayload>
parseCachedTargetPayload(EvalState & state, std::optional<std::string_view> payload)
{
    if (!payload || !payload->starts_with(targetValuePayloadPrefix))
        return std::nullopt;
    auto fields = payload->substr(targetValuePayloadPrefix.size());
    auto separator = fields.find('\0');
    if (separator == std::string_view::npos)
        return std::nullopt;
    auto outputName = fields.substr(separator + 1);
    if (outputName.empty() || outputName.find('\0') != std::string_view::npos)
        return std::nullopt;
    auto storePath = state.store->maybeParseStorePath(fields.substr(0, separator));
    if (!storePath || !storePath->isDerivation())
        return std::nullopt;
    return CachedTargetPayload{.drvPath = std::move(*storePath), .outputName = std::string(outputName)};
}

/**
 * Materialize target values for hits whose parsed payload names a recipe still
 * present in the store (one store query for all of them) and exposing the
 * selected output. An absent recipe or missing selected output leaves the hit
 * without a value (an ordinary miss).
 */
static void materializeCachedTargetValues(EvalState & state, const PosIdx pos, TargetDependencyResults & results)
{
    StorePathSet candidates;
    for (auto & result : results)
        if (result && result->cachedTarget)
            candidates.insert(result->cachedTarget->drvPath);
    if (candidates.empty())
        return;

    auto valid = state.store->queryValidPaths(candidates);
    size_t valueHits = 0;
    for (auto & maybeResult : results) {
        if (!maybeResult || !maybeResult->cachedTarget)
            continue;
        auto & target = *maybeResult->cachedTarget;
        if (!valid.count(target.drvPath))
            continue;
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
        maybeResult->targetValue = output->value;
        valueHits++;
    }
    if (valueHits)
        printTalkative("tecnixTargets: %d target value(s) served from the cache", valueHits);
}

static TargetDependencyResults evaluateTecnixTargetDependencies(
    EvalState & state,
    const PosIdx pos,
    const TecnixArgs & args,
    DependencyFingerprintCache & fingerprintCache,
    bool keepTargetValues = false,
    std::vector<std::string> * errors = nullptr)
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
        // Each hit is copied out of its cache row while the row is loaded:
        // the dependency attrs if the caller wants them, and the parsed target
        // payload if target values are wanted. `results` is GC-scanned, so the
        // attrs are reachable from the moment they are built.
        lookupCachedDependencies(
            state,
            cacheScope(args),
            std::span<const std::string>{args.targets.data(), args.targets.size()},
            fingerprintCache,
            [&](size_t i, const DependencyCacheHit & hit) {
                results[i].emplace();
                // Built before the target value is verified below: a hit whose
                // recipe has since been collected wastes this, which is rare
                // and cheaper than keeping the row around to build it afterwards.
                if (args.requireDependencies)
                    results[i]->cachedDependencies = hit.toValue(state);
                if (keepTargetValues)
                    results[i]->cachedTarget = parseCachedTargetPayload(state, hit.payload());
            });

        // Target values need a supported payload and a locally available
        // recipe exposing the selected output. Anything else is an ordinary
        // miss and is re-evaluated (which re-learns the payload).
        if (keepTargetValues)
            materializeCachedTargetValues(state, pos, results);
        for (size_t i = 0; i < results.size(); i++) {
            if (!results[i]) {
                printTalkative("tecnixTargets dependencies: dependency cache miss, evaluating '%s'", args.targets[i]);
                continue;
            }
            if (keepTargetValues && !results[i]->targetValue) {
                printTalkative(
                    "tecnixTargets dependencies: dependency cache hit for '%s' has no valid target value, evaluating",
                    args.targets[i]);
                results[i].reset();
                continue;
            }
            printTalkative("tecnixTargets dependencies: dependency cache hit for '%s'", args.targets[i]);
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

        std::optional<TecnixMemoRowFamily> memoFamily;
        if (useCache && state.settings.tecnixPersistentMemo)
            memoFamily.emplace(
                args.resolver, args.memoArgsKey, fingerprintCache, state.settings.tecnixEvalCacheHistory);

        auto evalMiss = [&](size_t i) {
            auto & target = args.targets[i];
            try {
                results[i] = evalTargetDependencies(
                    state,
                    pos,
                    *preparedResolve.resolveFn,
                    preparedResolve.sourceDeps,
                    target,
                    keepTargetValues,
                    tecnixSourceTrackingEnabled(state, args),
                    memoFamily ? &*memoFamily : nullptr);
            } catch (Error & e) {
                // Interrupted is not an Error, so interrupts still stop the call.
                if (!errors)
                    throw;
                (*errors)[i] = filterANSIEscapes(e.what(), true);
                results[i].reset();
                return;
            }
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
            upsertDependencyClosures(cacheScope(args), upserts, state.settings.tecnixEvalCacheHistory);
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
    std::vector<std::string> errors(tArgs.keepGoing ? tArgs.targets.size() : 0);
    auto results = evaluateTecnixTargetDependencies(
        state, pos, tArgs, fingerprintCache, includeTargets, tArgs.keepGoing ? &errors : nullptr);
    printTecnixAccessSetStats(state, "tecnixTargets");
    auto list = state.buildList(tArgs.targets.size());
    for (size_t i = 0; i < tArgs.targets.size(); i++) {
        auto * targetValue = state.allocValue();
        targetValue->mkString(tArgs.targets[i], state.mem);
        if (!results[i]) {
            // Only keepGoing leaves a result empty: the target failed, and its record says why.
            assert(tArgs.keepGoing);
            auto * errorValue = state.allocValue();
            errorValue->mkString(errors[i], state.mem);
            auto attrs = state.buildBindings(2);
            attrs.insert(state.symbols.create("target"), targetValue);
            attrs.insert(state.symbols.create("error"), errorValue);
            auto * recordValue = state.allocValue();
            recordValue->mkAttrs(attrs);
            list[i] = recordValue;
            continue;
        }
        auto & result = *results[i];
        // A hit carries its closure (dependencies were requested); a miss evaluated one.
        assert(result.cachedDependencies || result.cacheNeedsUpsert);
        auto * dependenciesValue =
            result.cachedDependencies ? result.cachedDependencies : dependencyAttrsToValue(state, result.dependencies);

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
    // A hit carries its closure (dependencies were requested); a miss tracked at least the resolver.
    assert(result.cachedDependencies || !result.dependencies.empty());
    auto * dependenciesValue =
        result.cachedDependencies ? result.cachedDependencies : dependencyAttrsToValue(state, result.dependencies);
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
