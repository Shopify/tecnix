# Tecnix Tracing

*Where a cold evaluation spends its time and memory, which targets that spending belongs to, and what would actually get faster if you removed something.*

**Status:** Phase 1 (§6) is implemented: `tecnix-trace` writes the file described in §4, with the views of §4.3. Implementation notes and the places where the build deviates from the original plan are in [`plan.md`](plan.md).

## What you get

You add one option to a `tecnixTargets` evaluation you would run anyway:

```sh
nix eval ... --option tecnix-trace /tmp/tecnix-trace --expr '
  builtins.tecnixTargets (base // { targets = [ "//services/api" ]; includeDependencies = true; })'
```

It writes one SQLite file. The file carries the raw data — every value computed, which target computed it, which targets later reused it — and a few views that answer the questions directly:

```sh
sqlite3 /tmp/tecnix-trace/tecnix-trace-12345.sqlite "select * from floor limit 20"
```

| view | tells you |
|---|---|
| `floor` | how much of the run is work needed by more than one target, what it is made of, and how many targets need each piece |
| `own` | what each target costs *by itself*, with none of the shared work counted against it |
| `consumers` | for each shared value, which targets need it — so you know whether removing a use would save anything |

Every number comes in nanoseconds and in bytes. Timelines, flamegraphs and dependency graphs are constructable from the same tables (§4.4) but are not part of this plan. The guide to running it and reading the output is [`usage.md`](usage.md); the rest of this document is why it is needed, what it records, and how it is built.

**Reading guide.** §1–3 explain the problem, what gets recorded, and how to read the results. §4 is the file. §5 is how it works inside the evaluator, for implementers and reviewers. §6 is the build plan and §7 the review checklist. "The explainer" and "the walkthrough" refer to the two documents in `plans/tecnix-target-eval-caching/`; the example repository throughout is the same one they use.

---

## 1. The problem

### 1.1 The trap

A cold run over many thousands of targets is slow. You want it faster, so you record a flamegraph (`--eval-profiler flamegraph`), find the widest frame, and optimize it. The run is not faster. You try the next widest frame. Same result.

This is not bad luck. It is a property of how Nix evaluates, and the flamegraph is structurally unable to warn you about it.

### 1.2 Why: the first one to need something pays for it

Nix evaluates lazily and remembers every result. A value — the result of importing a file, calling a function, building an attribute set — is computed the first time something needs it, and every later use gets the finished value for free. In a run over many targets, the targets share enormous amounts of this work: the resolver, the standard library, every common helper.

A flamegraph records time against whichever call was running when the time was spent. So all of the shared work is recorded against **the first target that happened to need it**. Every later target that needs the same value gets it in nanoseconds and records nothing.

Concretely, with the explainer's example repository — where, for this document, both targets import `lib/util.nix` and only `//services/api` imports `lib/heavy.nix`:

```
what the flamegraph reports            what actually happened
──────────────────────────────         ─────────────────────────────────────────────
resolver              50 ms            paid once, needed by both (the "floor")
//services/api       550 ms              resolver              50 ms
  import lib/util    120 ms              import lib/util      120 ms
  import lib/heavy   400 ms            each target's own work
  target.nix          30 ms              //services/api       430 ms  (heavy 400 + 30)
//services/web        20 ms              //services/web        20 ms
```

Both columns add up to the same 620 ms, but they lead to different decisions. The flamegraph says `//services/api` costs 550 ms. The truth is that api's own work is 430 ms, and 120 ms of what was charged to it is `lib/util.nix`, which `//services/web` needs just as much.

Two consequences:

- **Optimizing a *use* of shared work saves nothing.** If api stopped importing `lib/util.nix`, web would import it instead. The 120 ms moves; it does not disappear. The only ways to remove it are to make `util.nix` itself cheaper, or to remove *every* use.
- **The wrong targets look expensive.** With many thousands of targets, the first few dozen in evaluation order absorb the entire floor. Any per-target cost read off a flamegraph is wrong for exactly the targets that look worst.

Tecnix's eval cache makes this matter more, not less. The cache stores *target* results, not the intermediate values they were built from. A run that misses on a few dozen targets still evaluates the resolver, the standard library and every common helper from scratch: it pays the floor plus those targets' own work. Knowing the floor — how big it is and what it is made of — is the difference between "optimize the shared libraries" and "optimize the per-target templates" being the right answer.

### 1.3 The two questions

1. **What is dominating?** Of the total, how much is shared floor — and what is it made of — versus each target's own work?
2. **For anything expensive, who needs it?** Every target, one area of the repository, or one target that probably shouldn't? That decides whether removing a use saves anything, and whether the thing is cullable.

### 1.4 What exists today

| tool | what it attributes | why it cannot answer §1.3 |
|---|---|---|
| `--eval-profiler flamegraph` | sampled time → function-call stacks | charges shared work to the first caller; one shadow stack for all threads, so it garbles under parallel evaluation |
| `--trace-function-calls` | every function call, with timestamps | same attribution, as text |
| `NIX_SHOW_STATS`, `NIX_COUNT_CALLS` | totals; call counts per function | no time per anything, no per-target, no sharing |
| `tecnixTargets` per-target timing at `-v` | wall time per target | order-dependent, for exactly the reason in §1.2 |

None of them has the two ideas that are needed: a notion of *which target is being evaluated*, and a record of *when an already-finished value is reused*. Tecnix's source-dependency tracker has both — it uses them to build source closures — which is why tracing can be built on top of it cheaply (§5).

---

## 2. What tracing records

Tracing is a mode of tracked evaluation: it rides on the `TrackingContext` the tracker creates per target, and setting `tecnix-trace` turns tracking on for every `tecnixTargets` and `tecnixTargetNames` call (otherwise tracking runs only when the closure is needed: `includeDependencies = true` or the eval cache). It writes down three kinds of things.

**A root** is one target's evaluation, start to finish. Roots are what the eval cache and the tracker already treat as the unit of work; one root is one `TrackingContext`. The resolver import and target discovery are roots of their own, named `resolver` and `discovery`.

**A record** is one value being computed for the first time — one thunk being forced — or a root, or a wait. It carries:

| field | meaning |
|---|---|
| `id` | unique in the trace |
| `parent` | the record that was being computed when this one started — who needed it first |
| `root` | the root it happened inside |
| `expr` | where in the source it came from: the thunk's expression position; for a delayed function application, the applied lambda's position; a file for an import; the force site when none of those has a position |
| `kind` | `value`; `root` (the root itself); `wait` (this thread waited for another thread to finish the value; §3.4); `fetch` (a fetcher call — `fetchGit`, `fetchTree`, `fetchurl`, `fetchTarball`, …: I/O, and cacheable); `store` (a store write on the evaluator's behalf: `derivationStrict`'s `.drv`, a `builtins.path`/string-coercion source copy). Fetch and store records are children of the frame that did the I/O, so that frame's own time is evaluation only, and no cost view counts them as evaluation. |
| `start`, `dur` | when it began, and how long it took *including* everything it needed. Garbage-collection pauses are excluded. |
| `gc_alloc` | bytes allocated on the evaluator's GC heap while computing it, including everything it needed |
| `self_dur`, `self_gc_alloc` | the same two, *excluding* the records it forced — this value's own work |

Each root is itself a record of kind `root`. That gives work done directly by the root — the resolver's function body for this target, which is called rather than forced — somewhere to land, and it handles nesting: a root started inside another root's evaluation (a resolver that itself calls `tecnixTargets`) is a child of the record that was open, so its cost is excluded from that record's own work like any other child's.

**A reuse** says "root R used value P, which was already finished." Reuses are the thing the flamegraph does not have. A reuse is written once per (root, value): if a target touches the standard library ten thousand times, one row is recorded.

### 2.1 The example, traced

Sequential evaluation, `//services/api` first. Numbers are illustrative.

```
id  parent  root            kind    expr                          dur      self_dur
1   -       resolver        root    -                              50 ms     0 ms
2   1       resolver        value   build/resolver/resolve.nix     50 ms    50 ms
3   -       //services/api  root    -                             550 ms    30 ms
4   3       //services/api  value   lib/util.nix                  120 ms   120 ms
5   3       //services/api  value   lib/heavy.nix                 400 ms   400 ms
6   -       //services/web  root    -                              20 ms    20 ms

reuses (root, producer)
//services/api  2       the resolver
//services/web  2       the resolver
//services/web  4       lib/util.nix — finished during api's run; web got it free
```

Everything in §1.2's right-hand column is now derivable:

- Record 4 was reused by a root other than the one that computed it, so it is **shared**. So is record 2. Records 3, 5 and 6 were never reused by anyone else, so they are their root's **own** work.
- Floor = 50 + 120 = 170 ms. api's own work = 30 + 400 = 430 ms. web's own work = 20 ms.
- Consumers of `lib/util.nix`: {api, web}. Consumers of `lib/heavy.nix`: {api}.

Had web run first, records 4 and 6 would swap which root computed `util.nix` — and every derived number above would be identical. That is the point: **which root paid is an accident of ordering; which roots needed is not.** Everything the file reports is built from the second.

### 2.2 The sharing rule

A record is **shared** if it — *or any record above it in its parent chain* — was reused by a root other than the one that computed it. Otherwise it is **own** work of its root.

The parent-chain clause matters. When `//services/web` reuses `lib/util.nix` (record 4), it also depends on everything record 4 needed in order to compute itself — every record whose parent chain leads to 4. web never touched those directly; it got them as part of the finished value. The rule propagates "shared" downward so they are counted correctly.

Most Nix values are aliases of other values: `x = lib.foo`, `pkgs.python3`, a function returning one of its arguments — each is a thunk whose result is a *copy* of a value that already existed. If an alias counted as new work, the expensive value would stay attributed to whichever target first computed it while its aliases were what everyone else "reused", and the classification would be wrong for the most common shape of code there is. Tracing avoids this by making a copied value keep the identity of the value it was copied from (§5.3): reusing an alias is recorded as reusing the original. `lib/util.nix` reached through any number of `let` bindings is still record 4.

One case is deliberately left imprecise, so it is written down here: a record inside api that *used* a sibling's finished result to compute something new — `y = x.a + x.b` with `x` already finished — and was then reused by web. web depends on `x`, but the api-internal use of `x` is not a reuse row (reuses are per root), so `x` stays attributed to api. A consequence worth knowing: in this case `shared` depends on evaluation order — had `x` first been forced *under* `y`, it would be `y`'s child and shared through the parent chain. Everything else in the file is order-invariant (the tests check the value multiset, `consumers`, `floor` and `shared` across sequential, parallel and permuted runs on a repository without the sibling pattern). Recording reuses per consuming record would make this exact; it is a later option (§6), and the tables are shaped to take it.

### 2.3 No thresholds

Every forced thunk produces a record. There is **no minimum cost below which records are dropped**: a threshold would push cheap-but-shared work back onto whichever target forced it first — the disease of §1.2 in miniature, concentrated on the earliest targets — and the error would be bounded per record but unbounded in total. Aggregation and top-N cutoffs happen in queries, after classification.

---

## 3. Reading the results

### 3.1 The floor

`select * from floor` — shared records, grouped by where they came from, largest first:

```
file                          line  self_dur  self_gc_alloc  consumers  of
lib/util.nix                  -     120 ms    8.1 MB         2          2
build/resolver/resolve.nix    -      50 ms    2.0 MB         2          2
```

"Floor" means *needed by at least two of the traced targets*. That is the definition that makes floor plus own add up to the whole run. It does not mean every run pays it: `consumers` says how universal a value is. `2 of 2` — at scale, the standard library import, the resolver, the common helpers — is infrastructure that every run pays and no single target can remove. `3 of 2000` is a narrow shared library that only runs touching those three pay for, and that those three could stop needing.

### 3.2 Each target's own work

`select * from own`:

```
root              own_dur   own_gc_alloc
//services/api    430 ms    31 MB
//services/web     20 ms     1 MB
```

"Own" is the number that belongs to the target itself: nothing in it is inflated by work someone else also needed. Sorted by own cost, this is where per-target optimization effort should go.

### 3.3 Who needs it

`select * from consumers` — one row per shared value with the roots that need it. Two things to do with it:

**Stragglers.** Group a value's consumers by target prefix:

```
lib/testing.nix    3.1 s    consumers: 219
  //services/*   217
  //docs/*         2      ← probably incidental
```

If two `//docs` targets pull in the service test harness, they are almost certainly doing so by accident — a `with` over a large set, an over-broad import. Fix those two and `testing.nix` drops out of the floor for every miss set that does not touch services, and out of two source closures that were being invalidated by test-harness changes for no reason. This list cannot be produced by anything else.

**Two readings, kept apart.** For an expensive value, its *own* cost is what you save by making its *implementation* faster. Its *inclusive* cost is what you save by *eliminating the need* for it — but only if you eliminate every consumer. Making `heavy.nix` 30 % faster saves 120 ms. Removing api's need for it saves 400 ms. Removing web's need for `util.nix` saves nothing, because api still needs it.

### 3.4 Nanoseconds and bytes

Every record carries both, and every view shows both. They fail differently.

- **Time** is what you care about, but it is noisy, and three things pollute it. A garbage-collection pause lands in whatever record happens to be running — that is not that record's fault, so pause time is excluded from every record's `dur` as it is recorded (§5.5). Under parallel evaluation, a thread that needs a value another thread is already computing *waits*; the wait becomes a `wait` record so the value's cost is counted once, on the thread that computed it. I/O the evaluator does for a thunk — fetching a source (`fetch`), writing a derivation or copying a source into the store (`store`) — becomes a child record of its own kind, so it is visible per target (`target_costs.fetch_dur`, `store_dur`) and never mistaken for evaluation; fetches in particular are cacheable and vary with the network. Other waiting — for the parser's cache, for the tracker's own locks — is not separated and lands in self time.
- **Bytes** are deterministic. The same evaluation allocates the same bytes in the same records regardless of thread count, ordering or machine load. That makes bytes the right column for comparing two runs — "this change made `//services/api` allocate 12 % more" is a stable signal; the same statement about milliseconds is not — and a strong proxy for GC cost, which in large evaluations is a fifth to a third of the time. Precisely, `gc_alloc` is bytes allocated on the evaluator's GC heap through `EvalMemory`: values, environments, attribute sets, lists, strings. It does not count the parsed syntax tree, C++ temporaries, or the tracker's own bookkeeping, so a parse-heavy import under-reports.

---

## 4. The file

### 4.1 Why SQLite

The output has two jobs — be cheap and safe to write from the evaluator, and be easy to ask questions of — and no viewer format does both. SQLite is already linked and already used by the eval cache through `nix/store/sqlite.hh`, so it adds no dependency; it is ~40 bytes per record, which holds at the scale of everything; and it is queryable with nothing installed. Whether a record is shared is only known when the last root finishes, so any viewer format would have had to be derived afterwards anyway. The alternatives, for the record:

| format | new dependency | bytes/record | queryable without tooling | scales to 10⁸–10⁹ |
|---|---|---|---|---|
| **SQLite** | **none** | **~40** | **`sqlite3`, DuckDB, every language** | **yes** |
| Chrome trace JSON | none | 150–200 | DuckDB | no |
| Perfetto protobuf | hand-rolled encoder | 30–40 | `trace_processor` only | yes |
| OpenTelemetry | none for file output | 200+ | needs a collector and backend | no |

### 4.2 Tables

`<tecnix-trace>/tecnix-trace-<pid>.sqlite`:

| table | columns | one row per |
|---|---|---|
| `roots` | `id, record, name` | target evaluated (plus `resolver` and `discovery`); `record` is the root's own record |
| `records` | `id, parent, root, expr, kind, start, dur, gc_alloc, self_dur, self_gc_alloc` | value computed for the first time, root, or wait |
| `reuses` | `root, producer` | finished value used by a root other than the one that computed it |
| `gc` | `start, end` | garbage-collection pause |
| `phases` | `call, name, start, end` | non-evaluation work in a builtin call: discovery, evaluation, fingerprinting, cache write, dump |
| `exprs` | `id, file, line, col` | distinct source position seen; imports have a file and no line |
| `meta` | `key, value` | run-level fact: format version, the call's `gitDir`/`rev`/`resolver`, thread count, wall time |

Record ids are 64-bit — the thread in the high bits, a per-thread counter below — so a billion records do not overflow and a child can name its parent without coordination. Times are nanoseconds from the start of the run; `gc_alloc` is GC-heap bytes (§3.4). One process making several tracked calls appends to one file.

### 4.3 Views

The dump creates the views below, so the questions of §3 need no tooling; they are ordinary SQL a consumer can read, copy or replace, and they are **linear in the trace**. Every shared record is assigned to a *chunk* — its nearest ancestor that some other root used directly — by one walk down from the directly reused records (`chunk`); everything beneath a reused record was needed to compute it and is shared too, and the walk stops where the next directly reused record starts a chunk of its own. A chunk's consumers are the roots that used its producer plus the root that computed it (`chunk_consumer`, `chunk_consumers`); its weight is the self cost of its records (`chunk_weight`). (An earlier formulation propagated (record, root) pairs down the whole tree; a record needed by every target then contributes one pair per target, which is quadratic and does not finish at 10⁸ records.)

```sql
create view producers as select distinct producer as id from reuses;

create view chunk as
with recursive w(chunk, id) as (
    select id, id from producers
    union all
    select w.chunk, r.id from records r join w on r.parent = w.id
    where r.kind <> 'root' and r.id not in (select id from producers)
)
select chunk, id from w;

create view shared as select id from chunk;

create view chunk_weight as
select c.chunk, sum(r.self_dur) as self_dur, sum(r.self_gc_alloc) as self_gc_alloc, count(*) as records
from chunk c join records r on r.id = c.id where r.kind = 'value' group by c.chunk;

create view chunk_consumer as
select producer as chunk, root from reuses
union
select r.id, r.root from records r where r.id in (select id from producers);

create view chunk_consumers as select chunk, count(*) as consumers from chunk_consumer group by chunk;

-- shared work by the position of the chunk's producer, with how many targets need it
create view floor as
with agg as (select p.expr, sum(w.self_dur) as self_dur, sum(w.self_gc_alloc) as self_gc_alloc, count(*) as chunks
             from chunk_weight w join records p on p.id = w.chunk group by p.expr),
     cons as (select p.expr, count(distinct c.root) as consumers
              from chunk_consumer c join records p on p.id = c.chunk group by p.expr)
select e.file, e.line, e.col, agg.self_dur, agg.self_gc_alloc, agg.chunks, cons.consumers
from agg left join exprs e on e.id = agg.expr join cons on cons.expr is agg.expr
order by agg.self_dur desc;

create view own as
select ro.name as root, sum(r.self_dur) as own_dur, sum(r.self_gc_alloc) as own_gc_alloc
from records r join roots ro on ro.id = r.root
where r.kind in ('value', 'root') and r.id not in (select id from shared)
group by ro.id order by own_dur desc;

-- every directly reused value with the roots that need it (computing root included)
create view consumers as
select p.id as producer, e.file, e.line, e.col, p.dur, p.self_dur, w.self_dur as chunk_self_dur,
       (select group_concat(name, ',') from
           (select ro.name from chunk_consumer c join roots ro on ro.id = c.root where c.chunk = p.id order by ro.name)) as roots
from producers d join records p on p.id = d.id left join exprs e on e.id = p.expr join chunk_weight w on w.chunk = p.id
order by p.dur desc;

-- the per-target answer: own work; the shared work it needs, in full and amortised over each
-- chunk's consumers (the amortised totals of all targets add up to the run); its fetch and store I/O
create view target_costs as
with own_by_root as (select r.root, sum(r.self_dur) as own_dur, sum(r.self_gc_alloc) as own_gc_alloc
                     from records r where r.kind in ('value', 'root') and r.id not in (select id from shared) group by r.root),
     shared_by_root as (select c.root, sum(w.self_dur) as shared_dur, sum(w.self_dur * 1.0 / cc.consumers) as shared_dur_amortized,
                               sum(w.self_gc_alloc) as shared_gc_alloc, sum(w.self_gc_alloc * 1.0 / cc.consumers) as shared_gc_alloc_amortized
                        from chunk_consumer c join chunk_weight w on w.chunk = c.chunk join chunk_consumers cc on cc.chunk = c.chunk group by c.root),
     io_by_root as (select r.root, sum(case when r.kind = 'fetch' then r.self_dur else 0 end) as fetch_dur,
                           sum(case when r.kind = 'store' then r.self_dur else 0 end) as store_dur,
                           sum(case when r.kind = 'fetch' and r.id not in (select id from shared) then r.self_dur else 0 end) as own_fetch_dur,
                           sum(case when r.kind = 'store' and r.id not in (select id from shared) then r.self_dur else 0 end) as own_store_dur
                    from records r where r.kind in ('fetch', 'store') group by r.root),
     shared_io_by_root as (select c.root, sum(io.fetch_dur * 1.0 / cc.consumers) as fetch_dur_amortized, sum(io.store_dur * 1.0 / cc.consumers) as store_dur_amortized
                           from chunk_consumer c join chunk_io io on io.chunk = c.chunk join chunk_consumers cc on cc.chunk = c.chunk group by c.root)
select ro.id as root, ro.name,
       coalesce(o.own_dur, 0) as own_dur, coalesce(s.shared_dur, 0) as shared_dur,
       cast(round(coalesce(s.shared_dur_amortized, 0)) as integer) as shared_dur_amortized,
       coalesce(o.own_dur, 0) + cast(round(coalesce(s.shared_dur_amortized, 0)) as integer) as total_dur_amortized,
       coalesce(io.fetch_dur, 0) as fetch_dur, coalesce(io.store_dur, 0) as store_dur,
       coalesce(io.own_fetch_dur, 0) + cast(round(coalesce(sio.fetch_dur_amortized, 0)) as integer) as fetch_dur_amortized,
       coalesce(io.own_store_dur, 0) + cast(round(coalesce(sio.store_dur_amortized, 0)) as integer) as store_dur_amortized,
       coalesce(o.own_gc_alloc, 0) as own_gc_alloc, coalesce(s.shared_gc_alloc, 0) as shared_gc_alloc,
       cast(round(coalesce(s.shared_gc_alloc_amortized, 0)) as integer) as shared_gc_alloc_amortized
from roots ro left join own_by_root o on o.root = ro.id left join shared_by_root s on s.root = ro.id
left join io_by_root io on io.root = ro.id left join shared_io_by_root sio on sio.root = ro.id
order by total_dur_amortized desc;
```

(`chunk_io` sums each chunk's `fetch`/`store` records the way `chunk_weight` sums its `value` records.) `target_costs` is the per-target answer in one row: what the target costs by itself (`own_dur`), how much shared work it leans on (`shared_dur`), its fair share of that work (`shared_dur_amortized`), and the I/O it paid (`fetch_dur`, `store_dur`) — also amortised the same way (`fetch_dur_amortized`, `store_dur_amortized`), so an I/O ranking uses the same unique-plus-equal-share weight. `contrib/tecnix-trace.py dot` draws it at target level: targets, edges weighted by the shared work two targets both need, and one "infrastructure" node for work needed by more than *K* targets.

Stragglers are `consumers` grouped by a prefix of the root name; the prefix rule belongs to the consumer. `wait` records are excluded from every cost sum by the `kind` filters.

For traces of everything, run the same SQL in DuckDB: attach the file (`ATTACH 'x.sqlite' (TYPE sqlite)`), copy `records`, `reuses`, `roots` and `exprs` into native tables (about three minutes for 2.5 × 10⁸ records), and run the view definitions there (`contrib/tecnix-trace.py dot --print-sql` prints them) — the chunk walk then takes about three minutes; `sqlite3` on a 20 GB file does not finish it in reasonable time.

### 4.4 Constructable, not built

Everything else people might want is a transformation of these tables. The first three below exist as `contrib/tecnix-trace.py` (`speedscope`, `perfetto`, `folded --shared`; standard-library Python, not part of Nix — see `usage.md` §2.4); the rest is left to whoever wants it:

- **A timeline** for Perfetto or speedscope: one Chrome trace event per record — `name` from `exprs`, `ts` = `start`, `dur`, the thread from the id's high bits — is a short script, and `gc` gives the pause rows.
- **A flamegraph whose widths are causal**: walk parent chains into collapsed stacks weighted by `self_dur` or `self_gc_alloc`, and move every subtree rooted at a `shared` record to the top level under `[shared]`. Between targets that is exact. Within a target the nesting is still who-needed-it-first; making it exact there needs reuses recorded per consuming record and a dominator tree over them — the later option of §6.
- **A dependency graph**: the top-N records by `dur` and the `reuses` among them, as DOT.

### 4.5 Sizes

The number that matters is how many thunks a cold run forces — `nrThunks` in `NIX_SHOW_STATS`, which counts thunks *created* and so bounds records from above. It is not known for a real repository yet and is the first thing the plan measures (§6). Working assumptions: one target or a small miss set is 10⁶–10⁷ records; everything is 10⁸–10⁹. On disk, roughly 50 bytes per record plus 12 per reuse: 0.6 GB at 10⁷, 6 GB at 10⁸. In memory during the run, see §5.7.

---

## 5. Inside the evaluator

This section assumes the explainer's §6 — labels, frames, tracking contexts — and the walkthrough. Tracing adds no new concept to that machinery. It attaches a different payload to the same events.

### 5.1 Where the events already are

| tracing needs | already exists | where |
|---|---|---|
| start and end of computing a value | the frame pushed and popped by `forceValueTracked` for every thunk force | `include/nix/expr/tecnix/force-value.hh` |
| which root is running | the `TrackingContext`, one per target, confined to one thread | `primops/tecnix.cc` (`evalTargetDependencies`, `prepareTrackedResolveFunction`) |
| "this value was already finished" | the `isFinished()` branch of `forceValueTracked`, which already loads the value's label from the side table | `force-value.hh` |
| "this thread finished the value" vs "another thread did" | `frame.published`, set only by the finishing thread's finish hook | `tecnix/source-deps.cc` (`publishTrackedValueDependencies`) |
| "this value was produced by copying that one" | the before-finish copy hook, when the destination is the current frame's value | `source-deps.cc` (`copyTrackedValueDependencies`) |
| the moment a value becomes visible to other threads, with its label | the publish inside the finish hook, ordered before the value reads as finished | `include/nix/expr/tecnix/value-hooks.hh` |
| per-value side data that survives copies and cell reuse | the label table: address-keyed, demand-paged, cleared by the finish hook | `value-hooks.hh` |
| per-thread state | `currentTecnixThreadState` | `include/nix/expr/tecnix/thread-state.hh` |

### 5.2 The frame, when tracing

When a `TrackingContext` is created with tracing on, `forceValueTracked` takes a tracing variant of its frame. Per frame it adds: the record id, a source key, the clock, the GC-pause total and the thread's allocation total read at open, two accumulators for children's time and allocation, and `aliasOf` (§5.3).

- **At open**, before `v.force`: assign the id from the thread's counter; read `steady_clock::now()`, the global GC-pause total (§5.5) and the thread's allocation total; take the source key — the thunk's expression position (`expr->getPos()`), or the position the force was requested from when the expression has none or the value is a delayed application. A file evaluation's thunk has a stack-allocated expression with no position; `evalFile` places the file's path in thread state just before forcing it, and the frame keys on that instead.
- **At close**: read the three counters again; `dur = Δclock − ΔgcPause`, `gc_alloc = Δalloc`; `self_dur = dur − childDur`, likewise for bytes; add `dur` and `gc_alloc` to the parent frame's accumulators; kind is `value` if `frame.published` and `wait` otherwise (§5.5); append the record to the thread's chunk.

Creating a `TrackingContext` opens a record of kind `root` whose parent is whatever frame is open — normally none; the enclosing target's frame when a resolver calls `tecnixTargets` — and destroying it closes the record, feeding the enclosing frame's accumulators like any child.

Nothing here formats text, takes a lock or allocates, except when a thread's chunk fills and a new one is taken.

### 5.3 The side table and the one write

The label table stores one 32-bit label per 16-byte value cell in a sparse, demand-paged table keyed by address. Tracing adds a second table with the same directory structure and an 8-byte slot holding the id of the record that produced the cell's value (40 bits) and the root that computed it (24 bits). It is cleared by the finish hook like the label, and **written exactly once, at publish** — inside the finish hook, before the value is observable as finished, at the point the tracker publishes the label:

- for a value the frame computed: `pack(frame.id, frame.root)`;
- for a value the frame produced by *copying* an existing one: the source's slot. The before-finish copy hook already knows this case (the destination is the current frame's value); it stashes the source's slot in `frame.aliasOf`, and publish writes `aliasOf` instead of the frame's own id. This is what makes an alias *be* its original (§2.2), and it composes — a copy of a copy still names the producer;
- for a copy made outside any frame — into an attribute set, a cache, a function argument — the copy's own publish writes the source's slot.

Writing only at publish is not a style choice. A rule like "store at frame close if the slot is empty" leaves a window between another thread's finish and its frame close during which a concurrent reuse would load an empty slot and be silently lost — nondeterminism that would surface as a flaky test. Publish happens before the value is visible, so any thread that sees the value finished sees its record. Wait frames never publish and therefore never write.

Memory: 8 bytes per value cell touched under tracing, mapped on demand — twice the label table.

### 5.4 Reuse

Two places see a finished value being used: the `isFinished()` branch of `forceValueTracked`, and the before-finish copy hook when the frame's own value is produced by copying (a cache handing back a stored value without forcing it — the same class of event the tracker already treats as "any cache that skips work must replay provenance"). In both: load the source's slot. If it is zero — a literal, a value built directly by a primop, something computed outside tracking — there is nothing to record. If its root is the current root, skip: same-root use is not a reuse. Otherwise insert the record id into the `TrackingContext`'s hash set of producers already recorded; if it was new, append `(root, producer)`.

Nothing is written to shared memory on the reuse path, so a hot shared value hammered by every thread costs a slot load and a compare — the hash set is consulted only for cross-root uses, and its memory is bounded by the distinct shared values one root touches and freed with the root. The rows are exact and deterministic: no duplicates, nothing to `distinct` away.

### 5.5 Waits, GC, threads

- **Waits.** Two sources. The branch of `forceValueTracked` that forces a value another thread is already computing opens no frame; time the `v.force` call and append a `wait` record as a child of the current frame. And a frame whose thunk passed `isThunk()` but lost the race inside `Value::force` waited too: it closes with `published == false` — only the finishing thread's finish hook publishes — and is written as a `wait`. Either way the wait is a child, so the parent's self cost excludes it and the value's cost is counted once, on the thread that computed it. Under `tecnix-parallel-dependencies` thousands of targets will wait on the same handful of shared values; without this the floor would be counted once per waiter. Other waiting — the parse cache's `once_flag`, blocking visits on the concurrent maps, the interning graph's mutex — is not separated and lands in self time.
- **GC.** Boehm's collection-event callback (`GC_set_on_collection_event`) reports the start and end of each collection. The tracer installs it when tracing starts — from Tecnix code; no upstream change — and keeps one global total of nanoseconds spent in collections, updated at each end. Frames read the total at open and close, so every record's `dur` excludes pauses exactly, for every record that was open — which is what keeps `dur − Σ children` consistent. Each pause is also a row in `gc`. Bytes are unaffected.
- **Threads.** Tracking contexts are thread-confined and tracked evaluation never spawns parallel work (explainer §6.4), so every frame stack belongs to one thread and record ids carry the thread. The only cross-thread traffic is the side-table slot, which uses the same atomics as the label slot.

### 5.6 Holding and dumping

Each thread appends records to its own `std::vector` of fixed-size chunks — ordinary `malloc` memory, not the GC heap, so nothing scans it. Nothing touches disk during evaluation.

Records do not wait for the end of the run: when a thread's chunk fills (256 Ki records, 18 MiB), it is handed to a **writer thread** that inserts it into the file — one transaction per chunk, `synchronous=OFF`, no journal, a large page cache — while evaluation continues, so a run holds a few chunks in memory rather than all of them (a full-monorepo run of 2.5 × 10⁸ records peaked at 13 GB RSS *including* the evaluation heap, against 14.5 GB for the same run untraced). The writer keeps up with about 10⁶ records per second; if evaluation outruns it, appending threads block briefly on a bounded queue. Indexes are dropped while records stream in and rebuilt at the end, because maintaining them during the insert made it four times slower.

The end-of-call **dump** runs when the *outermost* tracked builtin call returns — `tecnixTargetNames` and `tecnixTargets` are separate calls and can nest, so "the last root" is not otherwise a point in time — and on that call's error path, because a failing run is often the one being profiled. It flushes the partial chunks through the writer, waits for it to drain, then writes roots, reuses, GC pauses, phases, the resolved `exprs` (once per distinct key, through the position table) and `meta`, and creates the indexes and the views of §4.3. The index builds dominate: about 150 s for 2.5 × 10⁸ records. The hot path never sees a string. A hard crash loses whatever has not been streamed yet, plus the small tables.

The parts of a call that are not evaluation — discovery, fingerprinting, the cache upsert, the dump itself — are timed into `phases`, so "why was this run slow" has an answer even when the answer is not in the records.

### 5.7 Cost, off and on

- **Off** (`tecnix-trace` empty, the default): the untracked path of `forceValue` is unchanged. The tracked path gains one well-predicted branch per force — "is tracing on for this context?" The allocation counter is one thread-local addition per allocation, unconditional, costing no more than the existing `stats.nrValues++` check.
- **On, per forced thunk**: two `steady_clock` reads, four counter loads, two adds into the parent frame, a ~72-byte chunk append — roughly 60–80 ns. **Per reuse**: one slot load and a compare, plus a hash-set insert only when the use crosses roots — 10–20 ns for the common case, mostly the slot's cache miss. Nothing else runs on the evaluation threads; SQLite does not exist until the run is over.
- **What that is relative to** depends on how fast thunks are. An evaluation that spends 10 µs per forced thunk — reading sources, writing derivations — sees under 1 %. At 1 µs per thunk, 5–8 %. Only pure expression evaluation with sub-microsecond thunks (nixpkgs is the extreme) reaches 15–25 %. Phase 0's `nrThunks` and `cpuTime` give the ratio directly.
- **Measured** (`plan.md` §5). Synthetic 1000-target world of pure expression evaluation (2.7 M memoization hits, 226 K records, 2.2 s): the hot path costs 24 %. The real monorepo, all 12,562 targets, cache off, 11 threads: 248 s untraced against 617 s traced, of which about 165 s is finishing the file (index builds), so evaluation itself runs about 1.8× slower — this evaluation is thunk-dense (2.5 × 10⁸ records in ~250 s, one per microsecond), i.e. the extreme regime of the previous bullet, not the 10 µs-per-thunk one. Of the hot-path cost about half is the reuse check on memoization hits (a second side-table cache miss beside the tracker's label load), a quarter the frames, a quarter the per-finish slot clear and per-copy slot propagation. The one structural saving left is to put the tracer's slot in the label's cache line.
- **Memory, held until the end**: ~72 bytes per record, 8 per reuse, plus the side table at 8 bytes per value cell and one small hash set per live root.

  | | 10⁷ records | 10⁸ records |
  |---|---|---|
  | records | 0.7 GB | 7 GB |
  | side table | 0.1–0.8 GB | ~0.8 GB |
  | **peak extra memory** | **~1–1.5 GB** | **~8 GB** |
  | **end-of-run dump** | **~10 s** | **~100 s** |

### 5.8 Footprint in upstream files

Everything above lives in `src/libexpr/tecnix/` and `include/nix/expr/tecnix/`, except:

| upstream file | change |
|---|---|
| `eval-inline.hh` | a one-line counter bump in `allocBytes`, in `allocValue`, and in the size-1 branch of `allocEnv`. Everything else — attribute sets, lists, strings, string contexts — already goes through `allocBytes` |
| `eval.cc` | one line in `evalFile`'s existing Tecnix block, handing the file path to thread state before the file's thunk is forced |
| `eval-settings.hh` | `tecnix-trace` (directory; empty means off), beside the other `tecnix-*` settings |

Nothing in `value.hh` or `eval.hh` changes. The existing hooks and dispatch shim are sufficient, and SQLite is written through the wrapper the eval cache already uses.

---

## 6. Plan

### Phase 0 — Measure

Run one cold all-targets evaluation with `NIX_SHOW_STATS=1` and record `nrThunks`, `nrValues`, `cpuTime` and the GC fraction. `nrThunks` bounds the record count: with `cpuTime` it gives the hot-path overhead ratio (§5.7), sizes the in-memory records and the side table, and says whether the end-of-run dump is seconds or a tail. Under parallel evaluation also note `waitingTime` — a large fraction is already evidence of a large floor.

### Phase 1 — Recorder and file

The frame variant (§5.2), the side table written at publish (§5.3), reuse recording (§5.4), waits and GC (§5.5), the allocation counter, the in-memory chunks and end-of-run dump with indexes and views (§5.6), the error-path dump, and the setting.

Tests, in the style of the existing dependency-tracking suite over synthetic repositories — all deterministic, none asserting on timing:

- **Oracle.** In one process, for the same targets, `count(*) where kind = 'value'`, the multiset of `(expr, self_gc_alloc)` over `value` records, the set of `reuses` rows, and the set of `shared` ids must be identical across sequential evaluation, parallel evaluation, and a permuted target order. Which thread or root *computed* a shared value may differ; nothing derived may.
- **Aliases resolve to their producer.** Reusing `let x = lib.foo` from another root yields a reuse of the record that computed `lib.foo`, and `lib.foo` is in `shared`.
- **Waits carry no work.** Every `wait` record has `gc_alloc = 0`.
- **Closures are untouched.** Source closures produced with tracing on are byte-identical to closures produced with it off.
- **Cell reuse.** A value whose cell is recycled does not inherit the previous occupant's slot.

*After this phase:* `sqlite3` against the trace of a small repository answers every question in §3.

### Phase 2 — Everything

A full cold all-targets trace. Publish `floor` and `own`; compare the floor against what people believed — the differences are the first optimization targets. If Phase 0 put the record count where the end-of-run dump is a long tail, the same insert code moves to a background thread that drains chunks during the run; nothing about the hot path or the schema changes.

### Later, if wanted

- **Reuses per consuming record**, as a second setting: makes the sibling case of §2.2 exact and gives the graph a dominator tree can be computed over, for exact within-target attribution. `reuses` gains a `consumer` column; nothing else changes.
- **The source-closure angle**: a `label` column on records (the value's closure id, one load at close) gives, per record, the paths it *adds* to its root's closure; joined with how often those paths change in git history, cost × churn is the expected cost per run — the metric that ranks what to optimize for warm-ish runs, and the one thing here only Tecnix can compute.
- **Not this tool**: a retention (heap) profiler — "what is holding memory live" — is a dominator tree over the object graph, which frames cannot see; it needs a heap walk and is a separate thing. `gc_alloc` is the flow side of memory, not the stock side.

---

## 7. Guardrails

Review checklist for tracing changes, in the manner of `plans/tecnix-target-eval-caching/guardrails.md`. A change satisfies every item or amends the item in the same change.

- **Off means off.** With `tecnix-trace` empty, the untracked force path is unchanged and the tracked path pays one predictable branch.
- **Nothing on the hot path allocates, locks, formats or touches disk.** Chunk append, slot write at publish, clock, counters. SQLite exists only after the run.
- **Every forced thunk produces a record.** No thresholds in the recorder; cutoffs belong to queries.
- **Tracing never writes tracker state.** It reads frames; it never touches the access stacks, the interning graph, or the closures.
- **The slot is written once, at publish.** Never at close, never on reuse.
- **Evaluation errors still produce a trace.**
- **Upstream footprint stays within §5.8.**

---

## Glossary

| term | meaning |
|---|---|
| **root** | one target's evaluation from start to finish; one `TrackingContext`. The resolver import and target discovery are also roots. |
| **record** | one value computed for the first time — one thunk forced — or a root, or a wait; with who needed it first, which root, how long, how many bytes. |
| **reuse** | a root using a value that a different root computed. |
| **shared** | a record that some root other than the one that computed it depended on, directly or through its parent chain. |
| **own** | a record that only its own root depended on. |
| **floor** | the total cost of shared records — those needed by at least two traced targets. Reported with how many targets need each; only the ones needed by nearly all are paid by every run. |
| **alias** | a value produced by copying an existing one; it carries the original's record id, so reusing it is reusing the original. |
