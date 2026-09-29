# Using Tecnix Tracing

*What you run, what comes out, and what to do with it.*

**Status:** this is the intended interface for the tool planned in [`README.md`](README.md). Commands, table and view names here are the contract the implementation is built to. For *why* it works and how it is built, read the README.

---

## In one screen

**You run** one `nix eval` that you would run anyway, plus one option naming a directory:

```sh
--option tecnix-trace /tmp/tecnix-trace
```

**You get** one SQLite file, with the raw data and four views that answer the questions:

```sh
sqlite3 /tmp/tecnix-trace/tecnix-trace-12345.sqlite "select * from floor limit 20"
```

**It tells you** three things a flamegraph cannot:

1. how much of the run is a **floor** shared between targets, versus each target's **own** work, and what the floor is made of;
2. for anything expensive, **which targets need it** — so you know whether removing a use would save anything;
3. per target, a cost that is **its own** — inflated by nothing another target also needed.

---

## 1. Turning it on

Tracing records **tracked** evaluation — the mode Tecnix uses to compute source closures — and setting `tecnix-trace` turns tracking on for every `builtins.tecnixTargets` and `builtins.tecnixTargetNames` call, whether or not `includeDependencies` is set or the eval cache is on. (Without a trace, tracking only runs when something needs the closure.)

A complete invocation, tracing a cold evaluation of two targets:

```sh
nix eval --json \
  --extra-experimental-features nix-command \
  --option lazy-trees true \
  --option tecnix-eval-cache false \
  --option tecnix-trace /tmp/tecnix-trace \
  --expr '
    let
      base = {
        gitDir       = "/path/to/repo/.git";
        checkoutPath = "/path/to/repo";
        rev          = "<commit>";
        resolver     = "build/resolver/resolve.nix";
        args         = { system = "x86_64-linux"; };
      };
    in builtins.tecnixTargets (base // {
      targets = [ "//services/api" "//services/web" ];
      includeDependencies = true;
      includeTargets = false;
    })'
```

The parts that matter for tracing:

| option | why |
|---|---|
| `--option tecnix-trace <dir>` | turns tracing on and says where to write. Empty (the default) means off, at zero cost. |
| `--option tecnix-eval-cache false` | forces a **cold** run: every target is evaluated, so every target is traced. Leave the cache **on** to trace only the targets that actually miss (recipe 3.2). |
| `includeDependencies = true` | also returns each target's source closure; not needed for tracing. |
| `includeTargets = false` | optional; skips building the target values in the output so the JSON result stays small. |
| `-v` | prints one line per target as it is evaluated, including `dependency cache miss, evaluating '…'` / `dependency cache hit for '…'` — useful to see what a run with the cache on actually evaluated. |

To evaluate in parallel, add `--extra-experimental-features 'nix-command parallel-eval' --eval-cores N`. Tracing is per-thread and handles it; see §4 for how waiting shows up.

To trace **everything**, use discovery to get the target list:

```nix
builtins.tecnixTargets (base // {
  targets = builtins.tecnixTargetNames base;
  includeDependencies = true;
  includeTargets = false;
})
```

Discovery is itself traced and appears as a root named `discovery`; the resolver import appears as a root named `resolver`.

---

## 2. What comes out

One file, `tecnix-trace-<pid>.sqlite`, written when the outermost tracing call finishes (also when it fails with an evaluation error). It is an ordinary SQLite database.

### 2.1 Tables

| table | one row per |
|---|---|
| `roots` | target evaluated — plus `resolver` and `discovery`. `record` is the root's own record. |
| `records` | value computed for the first time (or a `root`, a `wait`, a `fetch`, a `store`): which source position (`expr`), which root, who needed it first (`parent`), when (`start`), time and GC-heap bytes including what it needed (`dur`, `gc_alloc`) and excluding it (`self_dur`, `self_gc_alloc`). `fetch` is a fetcher call (cacheable I/O), `store` a store write (`.drv`, source copy); both are children of the thunk that did them and are never counted as evaluation. |
| `reuses` | finished value a root used that a *different* root computed (`root`, `producer`) |
| `exprs` | source position: `file, line, col`; imports have a file and no line |
| `gc` | garbage-collection pause |
| `phases` | non-evaluation work in a call: discovery, evaluation, fingerprinting, cache write, dump |
| `meta` | run-level facts: the call's `gitDir`, `rev` and `resolver`, thread count, wall time |

Times are nanoseconds from the start of the run; garbage-collection pauses are already excluded. Bytes are GC-heap bytes (§4).

### 2.2 Views

| view | question it answers |
|---|---|
| `shared` | which records were needed by more than one root? (the ids; the other views use it) |
| `chunk`, `chunk_weight`, `chunk_consumer(s)` | the building blocks: each shared record with the directly reused record it belongs to, each such chunk's cost, and the roots that need it (including the one that computed it) |
| `floor` | how much of the run is work needed by more than one target, what is it made of, and how many targets need each piece? |
| `own` | how much does each target cost *by itself*? |
| `consumers` | for each directly reused value, which targets need it (including the one that computed it)? |
| `target_costs` | per target: `own_dur`; the shared work it needs (`shared_dur`) and that work amortised over each chunk's consumers (`shared_dur_amortized`, so `total_dur_amortized = own + share` and the totals of all targets add up to the run); the I/O it paid (`fetch_dur`, `store_dur`) and the same amortised (`fetch_dur_amortized`, `store_dur_amortized`: own I/O in full plus an equal share of I/O inside shared chunks). Same for bytes. |

The views are plain SQL stored in the file; `select sql from sqlite_master where type = 'view'` shows them, and they are reproduced in README §4.3. Copy and change them freely.

### 2.3 Reading each view

Numbers below are from the README's worked example: two targets, `//services/api` first, both importing `lib/util.nix`, only api importing `lib/heavy.nix`.

**`floor`** — shared work, grouped by where it came from, largest first.

```
file                          line  self_dur      self_gc_alloc  consumers
lib/util.nix                  -     120000000     8493465        2
build/resolver/resolve.nix    -      50000000     2097152        2
```

"Floor" means needed by *at least two* of the traced targets — the definition under which floor plus own adds up to the whole run. Not all of it is paid by every run: `consumers` is how many targets need it. If that is nearly all of them, it is infrastructure — every run pays it, no single target can remove it, making it cheaper helps everyone. If it is three out of two thousand, it is a narrow shared library that only runs touching those three pay for, and that those three could stop needing.

**`own`** — each target's own work.

```
root              own_dur     own_gc_alloc
//services/api    430000000   31457280
//services/web     20000000    1048576
```

`own` is the cost that belongs to the target and nothing else — what a flamegraph would have told you if the first target to need something did not get charged for it. Sort by it to find where per-target optimization pays.

Sanity check: the sum of `floor.self_dur` plus the sum of `own.own_dur` is the whole run's evaluation time.

**`consumers`** — one row per shared value with the roots that need it.

```
producer  file            dur         self_dur    roots
4         lib/util.nix    120000000   120000000   //services/api,//services/web
```

Two things to do with it. **Stragglers:** group a value's consumers by target prefix — if a value is needed by 217 `//services/*` targets and 2 `//docs/*` targets, the two are almost certainly accidental (a `with` over a large set, an over-broad import), and fixing them removes the value from the floor for every run that does not touch services and shrinks two source closures. **Two readings, kept apart:** a value's `self_dur` is what you save by making its *implementation* faster; its `dur` is what you save by *eliminating the need* for it — but only if you eliminate every consumer. Making `heavy.nix` 30 % faster saves 120 ms; removing api's need for it saves 400 ms; removing web's need for `util.nix` saves nothing, because api still needs it.

**`target_costs`** — the per-target answer in one row:

```
name              own_dur     shared_dur  shared_dur_amortized  fetch_dur  store_dur
//services/api    430000000   170000000   85000000              0          12000000
//services/web     20000000   170000000   85000000              0          0
```

`own_dur` is the target's own work; `shared_dur` all the shared work it needs (the whole floor, here); `shared_dur_amortized` its fair share of that work, so `own + share` over all targets adds up to the run. `fetch_dur` is time in fetchers (network; cacheable) and `store_dur` time writing derivations and copying sources into the store — both real, neither evaluation. They are given as paid (charged to the target under which the I/O happened) and as `*_amortized` (own I/O in full plus an equal share of the I/O inside shared chunks), so ranking by I/O uses the same unique-plus-share weight as ranking by evaluation.

### 2.4 Your own queries

The views are a starting point. The tables are small enough to reason about:

```sh
# the twenty most expensive positions by own work, everything included
sqlite3 trace.sqlite "
  select e.file, e.line, sum(r.self_dur)/1e6 as ms, count(*) as n
  from records r join exprs e on e.id = r.expr
  where r.kind = 'value'
  group by e.id order by ms desc limit 20"

# what did one target compute itself, top down
sqlite3 trace.sqlite "
  select r.id, r.parent, e.file, e.line, r.dur/1e6 as ms, r.self_dur/1e6 as self_ms
  from records r join exprs e on e.id = r.expr
  where r.root = (select id from roots where name = '//services/api')
  order by r.start"
```

`meta` holds the call's `gitDir` and `rev`, so a consumer can print the source line for any position from the checkout.

For a trace of everything (10⁸ records and more), `sqlite3` will not finish the views; use DuckDB: `ATTACH 'trace.sqlite' AS t (TYPE sqlite)`, copy `records`, `reuses`, `roots`, `exprs` into native tables (~3 min for 2.5 × 10⁸ records), and run the views' SQL there (`./contrib/tecnix-trace.py dot --print-sql` prints it; the definitions are in README §4.3). `target_costs` for the whole monorepo then takes about three minutes.

Timelines and flamegraphs are conversions of these tables, kept out of Nix itself in `contrib/tecnix-trace.py` (standard-library Python, run it with `./contrib/tecnix-trace.py`):

```sh
# one evented profile per thread (or --split root): time order, left-heavy and
# sandwich views at https://www.speedscope.app or `nix-shell -p speedscope`
./contrib/tecnix-trace.py speedscope trace.sqlite -o trace.speedscope.json

# a timeline with a track per evaluation thread and one for GC pauses, for
# https://ui.perfetto.dev -- the one to use for parallel runs
./contrib/tecnix-trace.py perfetto trace.sqlite -o trace.json

# collapsed stacks weighted by self time (or --weight self_gc_alloc); with
# --shared, work needed by several targets is hoisted under `[shared]` so the
# widths are causal between targets instead of first-payer
./contrib/tecnix-trace.py folded trace.sqlite --shared > trace.folded
nix-shell -p flamegraph --run "flamegraph.pl trace.folded > trace.svg"
```

```sh
# the attribution graph, at target level: targets (own, amortised share, fetch,
# store), edges = shared work two targets both need, one "infrastructure" node
# for work needed by more than --max-consumers targets
./contrib/tecnix-trace.py dot trace.sqlite --top 20 | dot -Tsvg > trace.svg
```

`--root NAME` restricts any of them to one target (for `dot`, to those targets' nodes); `--mark-shared` prefixes shared records' names. At 2 × 10⁵ records the outputs are 3–50 MB and take a couple of seconds; beyond a few million records use the SQL views instead. Dependency graphs (README §4.4) are left to whoever wants them.

---

## 3. Recipes

### 3.1 "What is dominating a cold run?"

Trace everything with the cache off (§1, last snippet). Read `floor` first: if its total is most of the run, optimize what is in it. If it is small, read `own`: the cost is in the targets — or, more likely, in shared *template* code whose values are each target's own even though the code is common. The first query in §2.4 shows that: a position with a huge total and a count in the thousands is a template.

### 3.2 "Why was this run slow?"

Keep the cache **on** so that only the targets that miss are evaluated — exactly what the slow run did — and add `-v` to see which they were. The floor for *this* miss set is what is shared among *these* targets: a value one of them needed alone is that target's own cost here, even if two thousand other targets would share it on a full run. If `floor` plus `own` does not account for the wall time, look at `phases`: fingerprinting the source closures, writing the cache and writing the trace itself happen outside evaluation, and a slow run is sometimes slow there.

### 3.3 "Is this one target expensive, and is it its fault?"

```nix
targets = [ "//services/api" ]; includeDependencies = true;
```

`own` is the target's fault. `consumers` lists the shared values it leans on that it does not control. The second query in §2.4 walks what it computed itself, top down.

### 3.4 "Who needs this expensive thing?"

Find it in `consumers`. If nearly everything needs it, it is infrastructure: make it cheaper, and do not bother removing individual uses. If a few targets need it, group them by prefix — the outliers are the ones to fix. If exactly one target needs it, it is not in `consumers` at all; it is that target's own cost.

### 3.5 "Did my change make things worse?"

Trace the same targets before and after, and compare `own.own_gc_alloc`. Bytes are deterministic — identical runs allocate identical bytes — so a difference is real. Compare `own_dur` only as a hint.

---

## 4. Reading the numbers correctly

- **Time and bytes are both always present.** Time is what you care about but is noisy. Bytes are exact and repeatable, and a good proxy for garbage-collection cost. Use bytes to *compare* runs and to *rank* stably; use time to know how much a thing matters in seconds. "Bytes" means bytes allocated on the evaluator's GC heap — values, environments, attribute sets, lists, strings. Parsing a file is not counted, so a big import's bytes understate its cost; its time does not.
- **Garbage-collection pauses** are already excluded from every time in the file. Without that, a pause would be blamed on whatever happened to be running.
- **Waiting is not cost.** Under parallel evaluation, when one thread needs a value another thread is computing, it waits. That time is a `wait` record — a child of whatever was running — and is *not* added to the value's cost; otherwise a value ten threads waited on would be counted ten times. The views exclude `wait` records from every sum.
- **Own versus shared depends on what you traced.** A value is shared if more than one *traced* target needed it. Tracing everything gives the global picture; tracing a miss set gives that run's picture. Both are correct; they answer different questions (§3.1 versus §3.2).
- **Which target computed a shared value is an accident of ordering** and is never reported as a cost. Which targets *needed* it is not an accident, and every view is built from that.

---

## 5. Things that will confuse you the first time

- **Cache hits are not traced.** A target served from the cache is not evaluated, so there is nothing to record. Turn the cache off to trace a cold run; leave it on to trace only real misses.
- **Records stream into the file during the run; the rest arrives at the end.** The file is complete when the outermost tracing call finishes or fails with an evaluation error; finishing it (indexes, views) takes about a minute per 10⁸ records. A run that is killed loses the records not yet streamed and all of the small tables.
- **Overhead and memory.** A run with tracing on is between a quarter slower (the synthetic tests) and 1.8× slower (the whole monorepo, whose thunks average a microsecond); memory is bounded by a few in-flight chunks plus a side table proportional to the value heap (the whole monorepo peaked at 13 GB including the evaluation heap itself). Do not leave `tecnix-trace` set in a `nix.conf`.
- **Aliases are their originals.** `let x = lib.foo` does not appear as a separate expensive thing; using `x` from another target is recorded as using `lib.foo`. If a value you expected to see in `consumers` is missing, look for what it was a copy of.
- **Traces contain target names and repository-relative source paths.** Treat them like logs.
