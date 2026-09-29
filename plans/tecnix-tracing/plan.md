# Tecnix Tracing — Plan of Attack

*Working notes for implementing Phase 1 from [`handoff.md`](handoff.md). This records the build order, the checkpoints, and the places where the implementation deliberately differs from the handoff and why. Read the handoff first; this only adds to it.*

**Status:** steps 1–7 of §3 are done and the tracer has run over the whole monorepo twice (uncommitted on `tecnix-eval-cache-history`); §5 has the measurements. Since the first monorepo run: `fetch`/`store` record kinds (I/O kept apart from evaluation), the streaming writer, chunk-based (linear) views with `target_costs`, `tecnix-trace` implying tracking, and a target-level `contrib/tecnix-trace.py dot`. Left: the slot/label cache-line co-location if the ~1.8× overhead on thunk-dense evaluations matters; a DuckDB path in the contrib script.

## 0. Environment

On macOS 27 the pinned `libgit2` (patched in `packaging/dependencies.nix`, so never in a binary cache) failed its own test suite: the OS (27.0; 26 was fine) resolves `rmdir("a/")` through a symlink `a` and removes the *target* (older releases ignored the slash; Linux refuses with ENOTDIR), and `git_futils_rmdir_r` receives directory paths with trailing slashes from checkout. That is a real libgit2 bug (force checkout over a symlink on a case-insensitive FS deletes the link's target), fixed by `packaging/patches/libgit2-rmdir-dont-follow-trailing-slash-symlink.patch` (trim trailing slashes before `lstat`/`rmdir`, as `git_futils_mkdir` already does). Upstream `main` has the same code; the patch is upstreamable. With it, `nix develop` works and the full libgit2 suite passes here.

If a build dir was configured against an older libgit2, meson keeps the first `PKG_CONFIG_PATH` it saw: `nix develop -c meson configure build -Dpkg_config_path="$PKG_CONFIG_PATH"`.

```sh
nix develop -c meson compile -C build
nix develop -c meson test -C build nix-expr-tests --print-errorlogs
nix develop -c meson test -C build --suite tecnix --print-errorlogs   # functional: builtins.sh, gc.sh
nix develop -c ./maintainers/format.sh
```

Baseline (branch `tecnix-eval-cache-history`, HEAD `931ff9cc8`): everything compiles, unit + tecnix functional suites green.

## 1. Deviations from the handoff

Each is small, and each closes a correctness hole or an oracle-test hole; none changes the schema or the user-facing contract.

1. **Thread buffer via the root, not per-force thread-id lookup.** Tracking contexts are thread-confined, so `TraceRoot` acquires its `TraceThreadBuffer *` once, at `TrackingContext` construction (session mutex, keyed by `myEvalThreadId`), and every append goes through `trackingCtx.trace->buffer`. No TLS or array lookup on the force path, and no dependence on the range of `myEvalThreadId` (a process-global counter). Ids are `(session thread index << 32) | per-thread counter`; the index is dense and capped at 255 (throw at acquisition if exceeded — that is > 255 evaluation threads).

2. **The thunk position read is racy; defer the dereference.** `v.thunk().expr` reads `p1`, which another thread may overwrite at `finish` between our `isThunk()` check and the read. Store the raw `Expr *` at open (never dereferenced), and call `getPos()` at close **only if `frame.published`** — then our thread owned the thunk from before our read until finish, so the pointer is the thunk's expression, and `Expr`s live in `EvalMemory::exprs` (pmr pool) for the state's lifetime. A frame that lost the race (a `wait`) keys on the force-site `pos`. Import keys (`ExprParseFile` on the stack) take priority and are never dereferenced.

3. **Two more reuse-check sites.** Besides the `isFinished()` branch and the copy hook, run the reuse check (a) in the pending/awaited branch after `v.force` returns, and (b) at close when `!frame.published` (our thunk lost the CAS and another root computed it). Without these, sequential and parallel runs record different consumer sets — parallel would miss every "waited for it" use — and the oracle fails.

4. **Restate the oracle's invariants.** Raw `reuses (root, producer)` pairs and record ids are *not* order-invariant: which root computes a shared value flips with target order and thread count. What is invariant, and what the tests assert:
   - `count(*) where kind = 'value'`;
   - the multiset of `(file, line, col, self_gc_alloc)` over `value` records (resolved through `exprs`; raw `PosIdx` values depend on parse order);
   - `consumers` (file, line, col, roots), `floor` (minus durations) and the exprs of `shared` — **on a fixture without README §2.2's sibling case**. With it, `shared` is genuinely order-dependent: whether `x` is a child of `y` depends on which of the two siblings forced it first, and a same-root use writes no row. That is the accepted imprecision; the README now says so.
   The `consumers` view includes the computing root, not only the reusers, via a `shared_consumers` helper view that carries the consuming root down the parent chain; `floor.consumers` is computed the same way.

5. **Session lifetime = `EvalState` lifetime; dump-and-clear.** Slots hold `(record id, root)`; if the session were destroyed after each dump, stale slots on live values would name ids from a dead session. Instead one session per state, ids monotonic across calls, each outermost return dumps the rows appended since the last dump and frees the chunks. A stale slot is then a valid reference to an earlier row in the same file — exactly "one process appends to one file".

6. **Finish-hook cost when tracing is off.** `tecnixValueFinishHook` runs for every finish in the process, tracked or not. Gate the new slot clear behind a process-global `tecnixTraceEverEnabled` (relaxed load, `[[unlikely]]`) rather than a second directory probe, so untraced processes pay one predicted branch. The flag is set on first session creation and never cleared (per §5, slots may outlive a call).

7. **Frame shape.** `struct TracedSourceDepsFrame final : TrackedSourceDepsFrame` with an uninitialised POD tail; `forceValueTracked` always constructs it (stack bytes are free) and calls out-of-line `traceFrameOpen`/`traceFrameClose` under `if (trackingCtx.trace) [[unlikely]]`. `forceValueTracked` is `always_inline` into every `forceValue` site, so the tracing body must not be inlined there. Parent tracing frame = `trace->current` (explicit stack in the root), no walk over scope frames. `publishTrackedValueDependencies` reaches the frame via `static_cast` when `trackingCtx.trace` is set — every value frame in a tracing context is one of these by construction.

8. **Key kind is a byte, not a top bit.** `TraceRecord` is 72 bytes with either layout (padding); `uint8_t keyKind` avoids assuming `PosIdx < 2^31`. Stable `exprs.id`: `pos.get()` for positions, `(1 << 32) | index` for imports.

   **Delayed applications key on the applied lambda.** A `tApp` thunk (`map f xs`, `genList f n`, …) has no expression; keying it on the force site alone left `genList`'s element applications with no position at all when forced from a `noPos` site. The record now keys on `left->lambda().fun->getPos()` — read raw at open, dereferenced at close only if the frame published (same discipline as `expr`; the frame is on the stack, so the collector keeps `left` alive). Primop applications fall back to the force site.

9. **`evalFile`.** `tecnixTraceNoteImport(*trackingCtx, resolvedEntry->resolvedPath)` before both forces of the file thunk (the cache-hit branch and the fresh-thunk branch): two guarded call sites, four lines. The helper only sets `pendingImportKey` when the context is tracing, so it can never leak into a non-tracing force. File names in the trace are repository-relative: positions live on the `storeFS` mount of the Tecnix repo accessor, so the mount store path is stripped (the accessor's own display prefix is `«unknown»`).

10. **`dur` clamped at 0.** A frame opened between `GC_EVENT_START` and `GC_EVENT_END` (the world runs during Boehm's finish phase) would subtract a pause it did not fully see. Clamp `dur` and `self_dur` at 0 and note it; the alternative (PRE_STOP_WORLD/POST_START_WORLD) is a one-line switch if the error matters.

11. **Dump only if there is something new** (roots, records or phases since the last dump). A plain untracked `tecnixTargets` therefore leaves the directory empty, as `usage.md` §5 promises; a cache-hit-only run still gets its `phases`.

12. **Dump holds the session mutex.** Buffer acquisition (root creation) takes the same mutex, so a call that starts on another thread while the outermost call is dumping waits, then appends to drained buffers that its own guard will dump.

Decisions from handoff §3: chunk size 1 << 18 records; root counter, import-key map, roots list and phases live in the session; `steady_clock::now()`; per-call meta is not needed because `configureTecnixRepoContext` pins one repo context per `EvalState` (`resolver` is taken from the first call, `phases.call` distinguishes calls).

## 2. Files

| file | change |
|---|---|
| `include/nix/expr/eval-settings.hh` | `tecnixTrace` (upstream, 1 setting) |
| `include/nix/expr/eval-inline.hh` | `bytesAllocated +=` in `allocBytes`, `allocValue` (GC branch only — the non-GC branch goes through `allocBytes`), `allocEnv` size-1 branch (upstream, 3 lines) |
| `eval.cc` | `tecnixTraceNoteImport(...)` in `evalFile` (upstream, 1–2 lines) |
| `include/nix/expr/tecnix/thread-state.hh` | `uint64_t bytesAllocated`, `uint32_t pendingImportKey` |
| `include/nix/expr/tecnix/trace.hh` *(new)* | slot table (8-byte, mirrors the label table), `TraceRecord`, `TraceReuse`, `TracedSourceDepsFrame`, `TraceRoot`, out-of-line hot-path entry points, `tecnixTraceEverEnabled` |
| `include/nix/expr/tecnix/source-deps.hh` | `TrackingContext`: `std::unique_ptr<TraceRoot> trace`, `rootName` ctor param (defaulted so tests compile), out-of-line dtor |
| `include/nix/expr/tecnix/force-value.hh` | the three branches call the tracer under `if (trackingCtx.trace)` |
| `include/nix/expr/tecnix/value-hooks.hh` | finish hook clears the trace slot (gated) |
| `tecnix/source-deps.cc` | publish/copy hooks write slots and run the reuse check; `TrackingContext` ctor creates the root, dtor closes it |
| `tecnix/trace-session.hh` *(new, private)* | `TraceSession`: thread buffers, counters, import keys, roots, phases, GC callback, dump |
| `tecnix/trace.cc` *(new)* | everything above plus the SQLite dump, indexes, views |
| `tecnix/eval-data.hh` | `std::unique_ptr<TraceSession> traceSession` |
| `primops/tecnix.cc` | RAII call guard (first statement of both public primops), root names, phases, meta |
| `libexpr/meson.build`, `include/nix/expr/meson.build` | new sources/headers |
| `src/libexpr-tests/tecnix-tracing.cc`, `meson.build` | unit tests |
| `tests/functional/tecnix/tracing.sh`, `meson.build` | functional tests over a README §2.1-shaped repo |

## 3. Build order and checkpoints

Each step compiles and passes the existing suites. All of 1–7 are done; 8 is partly done (see §5).

1. **Setting, counters, thread state.** §1.1, §1.7 of the handoff, thread-state fields. *Checkpoint:* compiles; `NIX_SHOW_STATS` unchanged.
2. **Session skeleton.** `trace-session.hh`/`trace.cc`: buffers, id assignment, import keys, GC callback (preallocated pause vector, global pause total), roots/phases vectors; owned by `TecnixEvalData`; call guard in both primops creating it when the setting is non-empty (no dump yet). *Checkpoint:* compiles; a unit test creates a session and reads back an empty drain.
3. **Frame variant and roots.** `TracedSourceDepsFrame`, `TraceRoot`, `forceValueTracked` branches, root records from `TrackingContext`. *Checkpoint:* unit test evaluates a small expression under a tracing context and asserts: records close after their children, parents nest, root record present, `wait` count is 0 sequentially.
4. **Slot table, publish, copy, reuse.** §1.5–1.6 plus the two extra reuse sites. *Checkpoint:* provenance-style unit tests — force then copy → equal slots; alias via copy into the frame's value → `aliasOf`; cell reuse clears; two roots → exactly one reuse row per producer.
5. **Import keys.** `evalFile` line, `pendingImportKey` consumption in all three branches. *Checkpoint:* an imported file's record resolves to `(file, "", "")`.
6. **Dump.** SQLite writer (single connection, `synchronous=off`, prepared statements, commit per 1 M rows, indexes and views last, `if not exists` everywhere), `exprs` resolution once per distinct key, `phases`, `meta`, error-path dump via the guard. *Checkpoint:* `sqlite3 trace.sqlite "select * from floor"` on the functional fixture shows `lib/util.nix` and the resolver with `consumers = 2`, and `own` has one row per target.
7. **Tests.** The five in handoff §1.11 with the invariants of §1.4 above, sequential and `--eval-cores 2`.
8. **Before PR.** `NIX_SHOW_STATS=1` with and without `tecnix-trace` (also `BM_TecnixTrackedEvalUncached` with tracing on — `tecnix-eval-bench.cc` is a ready-made harness); `git diff --stat` on the three upstream files; `./maintainers/format.sh`.

## 4. Open questions for review

- `consumers`/`floor` including the computing root (§1.4) — a small departure from README §4.3's SQL; the README should be updated to match once the views are run against real data.

## 5. Measurements

### The monorepo (`tec architect get-all-targets --filter=all`, cache off, `eval-cores 0` → 11 threads, macOS 27, M-series)

| | untraced | traced, run 1 (dump at end) | traced, run 2 (streaming writer, fetch/store split) |
|---|---|---|---|
| wall | 248 s | 27 min | 617 s |
| CPU | 1402 s | 49 min | 2550 s |
| peak RSS | 14.5 GB | (not measured; ~18 GB of records alone) | 13.3 GB |
| records | — | 255 M | 255 M (value 255.3 M, store 1.2 M, root 12.6 K, wait 334, fetch 189) |
| finishing the file | — | 18 min (indexes maintained during insert) | 154 s (index builds) |

Targets: 12,562; output identical with and without tracing. Thread-time in run 2: value 2533 s (own 1415 s + shared 1120 s), **store 331 s** (1.14 M `.drv` writes ≈ 250 s; one 45 s `builtins.path` of a machine's source tree), fetch 15 s (189 calls, concentrated in two personal machine configs' pinned nixpkgs), **wait 375 s** (334 waits; threads blocked on values other threads were computing, mostly the second nixpkgs). Sharing is narrow: 68 % of shared work is needed by exactly two targets (a zone's `package`/`sorbet`/`rubocop`/`tarball` clique), <0.3 % by more than 50.

Overhead: evaluation ≈ 1.8× untraced — this run averages one record per microsecond, the README's "extreme regime". Analysis at this size needs DuckDB (§4.3 of the README): copying the tables takes ~3 min, the chunk walk ~3 min; `sqlite3` does not finish on the 21 GB file.

### The synthetic worlds

Synthetic world in the shape of `tecnix-eval-bench.cc`: 1000 targets, 15 shared libs, pure expression evaluation (no store work), `--eval-cores 1`, cache off, `NIX_SHOW_STATS=1`, three runs each.

| | cpuTime | notes |
|---|---|---|
| `tecnix-trace` unset | 2.20–2.35 s | 2.7 M memoization hits, 651 K values allocated |
| set | 2.98–3.06 s | 226 K value records, 19 K reuses, 1001 roots; 19 MB file |
| of which the dump | 0.27 s | 1.2 µs per row, at the end of the call |

Hot-path overhead ≈ 0.53 s ≈ 24 % on this workload, which is the README's "pure expression evaluation with sub-microsecond thunks" regime (its prediction: 15–25 %). A/B stubs (temporary, removed) split it roughly as: reuse check on memoization hits ~0.25 s (≈90 ns per hit: a second side-table cache miss beside the tracker's label load), frame open/close and record append ~0.13 s, the remainder the per-finish slot clear and per-copy slot propagation. Real Tecnix targets spend ~10× more per thunk (derivations, hashing, store paths), so single digits are expected there; **not yet measured on the real repository** — do that before optimising.

If it matters: the one structural optimisation is to put the tracer slot in the same cache line as the label (a 16-byte cell entry when tracing is enabled at state creation) so a memoization hit pays one miss instead of two. The frame cost is already near the floor (two clock reads, one append).

## 6. Test inventory

- `src/libexpr-tests/tecnix-tracing.cc` (10 tests): record and slot at publish; nesting and parent accumulators; aliases keep the producer's identity (select-copies, since `b = a` shares the cell outright); copies outside a frame; recycled cells; one reuse per (root, producer); nested roots; import keys; the SQLite dump and views; and a two-thread wait through a FIFO-backed `readFile`.
- `tests/functional/tecnix/tracing.sh`: the README §2.1 repository; roots, positions, phases, aliases, `floor.consumers = 2`; the oracle over sequential / `--eval-cores 2` / permuted targets; waits carry no work; closures byte-identical with tracing on and off; the error path; untracked calls write nothing; two outermost calls append to one file; a nested call dumps once.
