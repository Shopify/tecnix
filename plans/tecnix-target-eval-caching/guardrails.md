# Tecnix target-eval cache guardrails

Use this as a review checklist for source-dependency tracking and target-eval cache changes. A change should satisfy every guardrail below, or explicitly amend the guardrail as part of the same work.

## Correctness oracle

- **Dependency output must be identical across evaluation modes.**
  - Isolated single-target deps, shared sequential deps, shared parallel deps, warm-cache deps, and target-discovery deps must agree.

- **No under-tracking. No cross-target contamination. No unproven over-tracking.**
  - A target's closure must contain exactly the source observations that can affect that target.
  - Today those observations are represented as path fingerprints; future formats may represent narrower observed properties when they can prove the same oracle.

- **Do not hide tracking bugs by disabling or resetting evaluator behavior.**
  - Fix provenance propagation rather than avoiding memoization, sharing, or parallelism.

## Source-dependency model

- **Values carry source-deps labels; targets inherit labels.**
  - Read logs are not sufficient because Nix memoization can reuse a value without repeating the read that produced it.

- **Every source observation that can affect evaluation is a dependency.**
  - File reads, directory reads, existence checks, symlink reads, and negative lookups must be represented.
  - Recording happens at the lowest layer that knows the observation is semantic: reads self-record in the Tecnix source accessor; existence/type checks are recorded by their primop call sites (`pathExists`, `readFileType`) via `recordEvalAccess`, because accessor-level stat tracking would over-track plumbing (symlink/import resolution, store copies). A new primop observing existence or type without a read must record the access itself.
  - Present clean paths fingerprint as `git:<sha>;mode=<mode>`; dirty present paths add `;dirty=<sha256>`.
  - Negative lookups fingerprint as `absent` or `absent;dirty=<sha256>`.
  - Directory listings may eventually be tracked by their complete returned child name/type map, including absence of other child names, rather than a full tree fingerprint; cache validation must still prove the exact observed result.

- **Value labels must describe current value contents, never stale history.**
  - Allocation, overwrite, copy, move, and force/memoization paths must preserve or clear labels correctly.

- **Tracking contexts are thread-confined.**
  - A context is created, recorded into, snapshotted, and destroyed on one thread; the only cross-thread dependency channel is the published label on a finished value.
  - Tracked evaluation must not spawn parallel evaluation work: detached prefetch sites skip spawning under tracking, and the work-item factory fails loudly otherwise (work items capture only owned state).

- **Dirty source state must fail closed.**
  - Dirty status is captured as one coherent evaluation snapshot.
  - If dirty status or fingerprinting cannot certify the source state, do not accept a cache hit.

- **Repo-root source access remains unrepresentable unless the closure format grows an explicit representation.**
  - Until then, repo-root access must fail closed.

## Value lifetime and GC

- **Every live `Value *` must be reachable by the conservative collector for its whole lifetime.**
  - Boehm GC scans thread stacks, GC-heap objects, and explicitly traceable allocations — not ordinary malloc'd buffers. A plain `std::vector<Value *>` (or any heap container holding `Value *`, directly or inside a struct) is invisible to it.
  - Builtin orchestration that pre-allocates result cells, or holds worker-produced values until a coordinator consumes them, must keep those pointers in GC-visible storage: `ValueVector`, a `traceable_allocator` container, `RootValue`, or a live stack frame.
  - A collected-and-recycled cell surfaces as the `ValueStorage::finish` pdThunk panic at best, and as silently wrong target values at worst (the fatal failure mode). `tests/functional/tecnix/gc.sh` holds this guardrail under forced GC pressure.

## Cache validity

- **A persistent cache hit requires one complete stored closure candidate to match current fingerprints.**
  - Partial matches are misses.
  - Candidate validation may short-circuit on mismatch, but acceptance requires the whole candidate.

- **Never trust commit identity for cache acceptance.**
  - No cache validity by `rev`.
  - No per-commit cache key.
  - No per-commit/rev fast path.
  - Changed-path or tree-diff data may filter affected-target output, but must not accept cache rows.

- **Unknown or malformed cache data is a miss.**
  - Cache data is an optimization; bad rows must not produce stale answers.

- **Miss evaluation must learn a fresh proof.**
  - A miss evaluates under tracking, finalizes the observed closure, fingerprints it, and stores it for future validation.

## Cache history and storage

- **History is bounded candidate history, not repository history.**
  - Candidate slots represent distinct source-closure alternatives for a logical key, not commits.
  - Duplicate closure content should not consume another slot.

- **Logical correctness remains per target even when physical storage is shared.**
  - Sharding and shared dictionaries must not let one target's closure or payload satisfy another target.

- **Target discovery is cached with the same proof rules as target dependencies.**
  - A stale target list is as wrong as a stale target closure.

- **The cache hit path must use the stored row directly.**
  - SQLite blob bytes should open into a bounds-checked view used for validation.
  - Do not rebuild a heap object graph, decoded index, old packed JSON trie, or normalized SQL dependency graph on the hit path.

- **Public dependency output remains path-to-fingerprint data.**
  - Internal storage may use IDs/dictionaries, but the public dependency shape remains compact `path = fingerprint` entries.

## Public API boundaries

- **Keep cache history out of the public Tecnix API.**
  - Public builtins remain `builtins.tecnixTargetNames` and `builtins.tecnixTargets`.

- **Dependency-only queries must not force target values.**
  - Continue to support `includeDependencies = true; includeTargets = false;`.

- **A dependency-cache hit is not a target-value cache hit.**
  - If callers ask for target values, those values still need evaluation unless a separate value-cache proof exists.

- **Keep legacy `unsafeTectonixInternal*` compatibility isolated from the new Tecnix cache/history design.**

## Development cache policy

- **Do not add migrations for unshipped development cache formats.**
  - During development, incompatible local rows should miss or be wiped and rebuilt.

- **Keep the current development blob marker fixed unless the cache format becomes a shipped compatibility contract.**
