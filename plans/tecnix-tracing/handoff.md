# Tecnix Tracing — Implementation Handoff

*For the agent or engineer building Phase 1. Code-anchored: where each piece goes, what it must preserve, and what will bite.*

Read first, in this order: [`README.md`](README.md) (the design — §2, §4.2–4.3 and §5 are the contract), [`usage.md`](usage.md) (the user-facing interface you are building to), then the tracker internals this sits on: `plans/tecnix-target-eval-caching/explainer.md` §6 and `walkthrough.md`. Line numbers below are as of this writing; re-`rg` before trusting them.

Build: `nix develop -c meson compile -C build`. Unit tests: `meson test -C build nix-expr-tests` (gtest; the Tecnix ones are `src/libexpr-tests/tecnix-*.cc`). Functional: `tests/functional/tecnix/builtins.sh` shows exactly how the builtins are invoked. Before committing: `./maintainers/format.sh`.

---

## 0. What you are building, in one paragraph

A tracing variant of the frame the source-dependency tracker already pushes for every thunk force. At frame open it takes an id, a source position, and three counters; at close it appends one ~72-byte record to a per-thread chunk and feeds its parent's accumulators. A second address-keyed side table maps every finished value to the record that produced it, written exactly once, at publish. The `isFinished()` branch and the copy hook use that table to record which roots consumed values other roots computed. When the outermost `tecnixTargets`/`tecnixTargetNames` call returns, everything is written into one SQLite file with four views. Nothing is written to disk, formatted, locked or allocated on the evaluation threads except a new chunk when one fills.

---

## 1. Pieces, in build order

Each step leaves the tree compiling and has a checkpoint. Do them in order; later steps assume earlier ones.

### 1.1 Setting

`src/libexpr/include/nix/expr/eval-settings.hh`, beside `tecnixEvalCache` (~line 508):

```cpp
Setting<Path> tecnixTrace{this, "", "tecnix-trace", R"(
  Directory to write a Tecnix evaluation trace into. Empty (the default) disables tracing.
  See plans/tecnix-tracing/usage.md.
)"};
```

Empty string means off. This is the only setting. No `tecnix-trace-links`, no thresholds, no sampling rate.

### 1.2 Session and storage — `src/libexpr/tecnix/trace.{hh,cc}` (new)

Owned by `EvalState::TecnixEvalData` (`src/libexpr/tecnix/eval-data.hh`, ~line 50) as `std::unique_ptr<TraceSession>`, created lazily on the first tracked builtin call when `settings.tecnixTrace` is non-empty.

```cpp
struct TraceRecord {          // ~72 bytes; keep POD, keep it trivially copyable
    uint64_t id;              // (myEvalThreadId << 32) | per-thread counter — 40 bits used
    uint64_t parent;          // 0 = none
    uint32_t root;            // root id, 24 bits used
    uint32_t key;             // PosIdx raw value, or import key with the top bit set
    uint8_t  kind;            // 0 value, 1 root, 2 wait
    int64_t  start, dur, gcAlloc, selfDur, selfGcAlloc;   // ns and bytes
};
struct TraceReuse { uint32_t root; uint64_t producer; };

struct TraceThreadBuffer {    // one per evaluation thread, owned by the session
    std::vector<std::vector<TraceRecord>> chunks;   // fixed chunk size, e.g. 1<<18 records
    std::vector<TraceReuse> reuses;
    uint32_t nextCounter = 1;
};
```

Index thread buffers by `myEvalThreadId` (`thread_local uint32_t`, `src/libexpr/parallel-eval.cc:210`; declared in `eval-inline.hh:100`), sized to `state.executor->evalCores + 1` — or grow under a mutex on first sight of a new thread id, which only happens once per thread. Do **not** hold the buffer in a `thread_local` with a destructor: worker threads outlive and predate sessions, and multiple `EvalState`s exist in tests. A plain-array lookup by thread id from the session is one load and has no lifetime problem.

Chunks are ordinary `std::vector` — `malloc`, not the GC heap; Boehm does not scan them and must not.

**Checkpoint:** nothing observable yet; compiles.

### 1.3 The frame variant — `include/nix/expr/tecnix/force-value.hh`

`forceValueTracked` (all of it, ~45 lines) is the whole hot path. Today it has three branches: finished → record label dependency; not a thunk/app (i.e. pending/awaited — another thread is computing it) → `v.force` and record; thunk/app → push `TrackedSourceDepsFrame`, `v.force`, publish/merge in a `Finally`.

Add to `TrackingContext` (`include/nix/expr/tecnix/source-deps.hh`) a `TraceRoot * trace = nullptr;`. In `forceValueTracked`, when `trackingCtx.trace` is set — one predictable branch — use a frame struct that extends the tracker frame with:

```cpp
uint64_t id, aliasOf = 0;  uint32_t key;
int64_t t0, gc0, a0, childDur = 0, childAlloc = 0;
```

**At open** (before `v.force`):
- `id` from the thread buffer's counter, thread id in the high bits.
- `key`: `v.isThunk() ? v.thunk().expr->getPos().get() : pos.get()`; if that is `noPos`, use `pos` (the force site — the argument `forceValueTracked` already receives). If `currentTecnixThreadState.pendingImportKey` is set (§1.8), use it and clear it. Do **not** store `Expr *`: `evalFile`'s `ExprParseFile` is a stack object (`eval.cc:1219`).
- `t0 = steady_clock::now()`, `gc0 = traceGcPauseTotal.load(relaxed)`, `a0 = currentTecnixThreadState.bytesAllocated`.

**At close** — inside the existing `Finally`, after `mergeUnpublishedTrackedSourceDepsFrame`:
- `dur = (now − t0) − (gcTotal − gc0)`; `gcAlloc = a − a0`; `selfDur = dur − childDur`; likewise bytes.
- Add `dur`/`gcAlloc` to the *parent* tracing frame's accumulators (find it via `frame.previous`, walking past scope frames whose `value == nullptr`; if none, the root's record — see §1.4).
- `kind = frame.published ? value : wait`. `published` is set only by the finishing thread's finish hook (`source-deps.cc:606`), so `false` here means this thread's thunk lost the CAS inside `Value::force` (`eval-inline.hh:116`) and waited.
- Append the record. The `Finally` body must not throw; it runs during unwinding when the thunk raised.

**The pending/awaited branch** (`!(v.isThunk() || v.isApp())`): time `v.force` the same way and append a `kind = wait` record with `parent` = the current tracing frame and `key = pos`. It has no frame of its own; feed the parent's accumulators directly.

Do not touch `eval.hh`'s dispatch shim; the untracked path stays byte-identical.

**Checkpoint:** with a temporary debug dump, a small tracked eval produces plausible records with parents that close after children.

### 1.4 Roots

`TrackingContext` is constructed at three places in `src/libexpr/primops/tecnix.cc`: `evalTargetDependencies` (~890, per target — the target string is in scope), `prepareTrackedResolveFunction` (~860, name `resolver`), and the discovery miss path (~595, name `discovery`). Give the constructor a `std::string_view rootName` (or a `beginTraceRoot(name)` called immediately after) — this is Tecnix code, change it freely.

When tracing is on: allocate a root id (24 bits; a global counter in the session), open a record of kind `root` with `parent` = the currently open tracing frame's record if any (nested `tecnixTargets` from a resolver — `ActiveTrackingContext` already saves `previousTrackingCtx`, `source-deps.cc:~729`), and remember it as the context's root record. Close it in the `TrackingContext` destructor, feeding the enclosing frame's accumulators like any child. Record `(root id, record id, name)` for the `roots` table.

Root-level self time is real: `evalTargetDependencies` calls `state.callFunction(resolveFn, …)` directly (~898) — the resolver body for this target runs under no thunk frame, only the root record.

### 1.5 The side table and the single write — `include/nix/expr/tecnix/value-hooks.hh`, `source-deps.cc`

Add a second table with the label table's exact structure (`tecnixValueLabelDir` + `tecnixInstallValueLabelChunk`, `value-hooks.hh:23–70`): same directory, same demand-paged chunks, but an 8-byte slot per 16-byte cell — `(record id << 24) | root`. Template or copy the load/store/clear helpers; keep `Value`'s size untouched (the `static_assert` in `value-layout-tripwire.hh` will tell you).

Three hooks already fire in the right places; add the tracer's behaviour beside the label's:

| hook | today | add |
|---|---|---|
| `tecnixValueFinishHook` (`value.hh:685`, first line of `ValueStorage::finish`) | clears the label slot; if this is the current frame's value, calls `publishTrackedValueDependencies` | clear the tracer slot too |
| `publishTrackedValueDependencies` (`source-deps.cc:606`) | writes the label — **but returns early without writing when the frame has no dependencies (line 615)** | write the tracer slot **before** that early return: `frame.aliasOf ? frame.aliasOf : pack(frame.id, root)`. Every value the frame computed gets a slot, dependencies or not |
| `copyTrackedValueDependencies` (before-finish, `source-deps.cc:~640`) | when `dst` is the current frame's value, adds `src`'s label to the frame | when `dst` is the current frame's value: `frame.aliasOf = tracerSlot(src)`; and run the reuse check of §1.6 on `src` |
| `publishCopiedValueDependencies` (after-finish, `source-deps.cc:~657`) | for copies *not* into the current frame's value, publishes `src`'s label onto `dst`; returns early if the label is empty | write `tracerSlot(dst) = tracerSlot(src)` **regardless of the label being empty** |

Why publish and nowhere else: `finish` runs the hook before `p0.exchange(…, release)` (`value.hh:685–690`), so a slot written there is visible to any thread that sees the value finished. A "store at frame close if empty" rule leaves a window between another thread's finish and its close where a concurrent reuse loads zero and is lost — nondeterministic, and it will show up as a flaky oracle test. Wait frames never publish and so never write.

Copies happen constantly (`Value::operator=` at `value.hh:926–936` is the path); `aliasOf` is what makes `let x = lib.foo` resolve to `lib.foo`'s record (README §2.2).

**Checkpoint:** unit test in the style of `TecnixValueProvenanceTest` (`tecnix-dependency-tracking.cc:176`): force a thunk, copy the result into another cell, assert both slots hold the same record id.

### 1.6 Reuse

In the `isFinished()` branch of `forceValueTracked`, and from the before-finish copy hook when `dst` is the frame's value:

```cpp
auto s = tracerSlot(&v);                 // 0: literal / primop-built / computed outside tracking → nothing
if (!s || (s & 0xffffff) == ctx.root) return;   // same-root use is not a reuse
if (ctx.trace->seen.insert(s >> 24).second)     // per-root hash set, lives in the TrackingContext
    threadBuffer.reuses.push_back({ctx.root, s >> 24});
```

Never write the slot here. The hash set (`boost::unordered_flat_set<uint64_t>` — Boost.Unordered is already a dependency) is freed with the context, so its memory is bounded by the distinct shared values one root touches. Rows are exact: no `distinct` anywhere downstream.

### 1.7 Allocation counter

`include/nix/expr/tecnix/thread-state.hh`: add `uint64_t bytesAllocated = 0;` to `TecnixThreadState`. Then three one-liners in `include/nix/expr/eval-inline.hh`, mirroring the existing `stats.nrValues++`:

- `EvalMemory::allocBytes` (line 16): `currentTecnixThreadState.bytesAllocated += n;`
- `EvalMemory::allocValue` (line 30): `+= sizeof(Value);` — it uses `GC_malloc_many`, bypassing `allocBytes`.
- `EvalMemory::allocEnv` (line 62), the `size == 1` branch only: `+= sizeof(Env) + sizeof(Value *);` — the other branch goes through `allocBytes`.

Unconditional; a TLS add with the `initial-exec` model is one instruction. Attribute sets (`attr-set.cc:21`), lists (`eval.cc:979`), strings (`eval.cc:98`) all already go through `allocBytes`. That is the entire upstream footprint besides §1.1 and §1.8.

### 1.8 Import keys

`eval.cc:1232`, the tracked block of `evalFile`:

```cpp
TrackedSourceDepsScope sourceDepsScope(*trackingCtx);
currentTecnixThreadState.pendingImportKey = internImportKey(resolvedEntry->resolvedPath);  // add
forceValue(*vExpr, noPos);
```

`internImportKey` lives in the tracer: a mutex-guarded map from path string to `0x80000000 | index` — imports are once per file, so the lock and the string are fine. The frame open consumes and clears the key; **also clear it in the other two branches of `forceValueTracked`** (a cache hit means `*vExpr` is already finished and no frame opens). Resolve to `(file, "", "")` at dump.

### 1.9 GC

From Tecnix code, when the session is created: `GC_set_on_collection_event(cb)`. On `GC_EVENT_START` stash `now()`; on `GC_EVENT_END` add the delta to a global `std::atomic<int64_t> traceGcPauseTotal` and append `(start, end)` to a **preallocated** vector (reserve, e.g., 1 M entries at session start; if full, keep counting the total and drop the row). The callback runs with the world stopped: no allocation, no locks, no logging. Boehm is initialised in `src/libexpr/eval-gc.cc:164`; you do not need to touch it.

### 1.10 The dump — `tecnix/trace.cc`

**Trigger.** An RAII guard as the *first* statement of `prim_tecnixTargets` and `prim_tecnixTargetNames` (`tecnix.cc:~711`, `~1202`): increments a session depth on construction, decrements on destruction, and when it returns to zero runs the dump. First statement so it is destroyed last, after every `TrackingContext` in the call has closed its root record. The destructor runs on the exception path too — that is the "evaluation errors still produce a trace" guarantee — so wrap the dump in `try { … } catch (...) { ignoreExceptionInDestructor(); }` and never let it throw. Worker threads are idle by then (`evalTecnixIndices` waits for all work items), so reading their buffers from the coordinator is safe.

**Writing.** Use `nix/store/sqlite.hh` the way `tecnix/eval-cache.cc:84–110, 247–261` does: `SQLite db(path, {.useWAL = false})`, `db.exec("pragma synchronous=off")`, one `SQLiteStmt` per table created once, `stmt.use()(a)(b)…exec()` per row, `db.exec("begin")`/`("commit")` every ~1 M rows, `create index` and the four `create view` statements from README §4.3 last (`create … if not exists` — one process may dump several times into the same file; per-thread counters and the root counter persist across calls so ids stay unique). Path: `<tecnixTrace>/tecnix-trace-<pid>.sqlite`.

**Resolving keys.** For a `PosIdx` key: `auto p = state.positions[PosIdx(key)];` then `std::get_if<SourcePath>(&p.origin)` → `path->to_string()`, `p.line`, `p.column` — the exact pattern `printStatistics` uses (`eval.cc:~3160`). Import keys come from §1.8's map. Do this once per distinct key.

**`phases`.** Time the sections of `evaluateTecnixTargetDependencies` (`tecnix.cc:1040`: cache lookup, evaluation, `finalizeSourceAccessSetDependencies`, cache upsert) and the discovery equivalent, plus the dump itself; a handful of rows per call.

**`meta`.** Format version, `gitDir`/`rev`/`resolver` from `TecnixArgs`, thread count, wall time.

**Checkpoint:** `sqlite3 trace.sqlite "select * from floor"` on the synthetic repository from `tests/functional/tecnix/` gives the README §2.1 shape: the resolver and the shared import in `floor`, one row per target in `own`.

### 1.11 Tests

Unit (`src/libexpr-tests/tecnix-tracing.cc`, new; register in `src/libexpr-tests/meson.build`) and functional (`tests/functional/tecnix/tracing.sh`, new; add to `tests/functional/tecnix/meson.build`). The functional harness (`builtins.sh:1–100`) already builds a synthetic git repo and invokes the builtins with `--option tecnix-eval-cache false`; add `--option tecnix-trace "$TEST_ROOT/trace"` and query the file with `sqlite3` (provided to functional tests already, per the git log).

All deterministic; no assertion on any duration:

1. **Oracle.** Same targets, one process: `select count(*) from records where kind='value'`, the multiset of `(expr, self_gc_alloc)` over `value` records, the set of `reuses` rows, and the set of `shared` ids — identical for sequential, `--eval-cores 2`, and a permuted `targets` list.
2. **Aliases.** A target reusing another target's `let x = lib.foo` produces a reuse whose producer is the record that computed `lib.foo`, and that record is in `shared`.
3. **Waits carry no work.** `select count(*) from records where kind='wait' and gc_alloc <> 0` is 0.
4. **Closures untouched.** `includeDependencies` output byte-identical with `tecnix-trace` set and unset.
5. **Cell reuse.** Unit test: force a thunk into a cell, let it die, reuse the cell for a new value; the slot must not carry the old id (the finish hook clears it — assert it).

---

## 2. Do not

- **Do not add a threshold, sampling, or a minimum duration to the recorder.** Every forced thunk gets a record. README §2.3 explains why a threshold is not a safe approximation here.
- **Do not write the slot at frame close or on the reuse path.** Only at publish. See §1.5.
- **Do not store `Expr *` or `Value *` in records.** `PosIdx` only.
- **Do not format strings, take locks, or allocate on the hot path**, other than a new chunk. Resolution, SQL and text happen in the dump.
- **Do not use `thread_local` objects with destructors for buffers.** Session-owned, indexed by `myEvalThreadId`.
- **Do not touch `value.hh`, `eval.hh`, or the tracker's frames' semantics.** The tracer reads `published`, `previous`, `value`; it never writes to the access stacks or the interning graph. Closures with tracing on must be byte-identical to closures with it off (test 4).
- **Do not use per-row transactions** or leave `synchronous` on. ~1 µs/row is the target; 1 ms/row is what fsync-per-row gives you.
- **Do not make `Value` bigger.** The layout tripwire will fail the build; that is correct.

---

## 3. Left to you

Small decisions the plan does not fix; pick and note them in the PR:

- Chunk size (256 K records ≈ 18 MB is reasonable); whether the root-id counter and import-key map live in the session (they should) or the state.
- Whether the tracing frame is a subclass of `TrackedSourceDepsFrame` or a parallel struct held beside it. Subclassing keeps `frame.previous` walks simple.
- Exact SQL of the four views — README §4.3 is the intent and has not been run against real data. `floor.consumers` should read `2` for `lib/util.nix` in the synthetic repo; make that a test.
- `steady_clock::now()` is fine; if profiling ever shows the two reads, that is the moment to consider raw counters, not before.

## 4. Before opening the PR

- The five tests pass, sequential and parallel.
- `NIX_SHOW_STATS=1` on the same eval with and without `tecnix-trace`: note `cpuTime` both ways and `nrThunks`; put the overhead ratio in the PR description (README §5.7 predicts single digits for this workload).
- Upstream footprint is exactly §1.1, §1.7 and §1.8 — three counter lines, one line in `evalFile`, one setting. `git diff --stat -- src/libexpr/include/nix/expr/eval-inline.hh src/libexpr/eval.cc src/libexpr/include/nix/expr/eval-settings.hh` should be tiny; everything else under `src/libexpr/tecnix/` and `include/nix/expr/tecnix/`.
- `./maintainers/format.sh`.
