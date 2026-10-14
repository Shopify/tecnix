# `builtins.tecnixQueryFile`: fine-grained dependencies on data files

Status: implemented on branch `tecnix-query-file` (§6 names the code). Replaces the three World-specific manifest builtins from #72
(`tectonixManifestEntry`, `tectonixManifestKeys`, `tectonixManifestIdToPath`)
and their `.meta/manifest.json#` synthetic keys with one builtin that queries
any JSON or TOML file in the repo, and a fingerprint format that records what
was asked and what the answer was. Tecnix stops knowing anything about World.

## 1. Why

#72 made the mechanism: a target that looks up one zone depends on that
lookup's answer, not on the manifest's blob oid, so adding a zone no longer
invalidates every target. Only three facts in it are World's: the file path,
"top-level keys are zone paths", and "`id` is the field to look up by".
Pulling those into arguments gives a builtin that also serves `Cargo.toml`,
`package.json`, generated lock data, and any config file World wants
fine-grained dependencies on.

The constraint that fixes the design: **validation is a pure function of the
stored row and the tree, with no evaluation** (explainer §4.1, §8). Warm runs
skip the evaluator entirely, and the cache layer cannot see the revision. So a
query is a fixed operation tecnix can recompute from the file's bytes alone.

## 2. The model: a fingerprint records an observation of a path

A source closure is `path → fingerprint`. Today's fingerprints already record
what evaluation observed: `git:<oid>;mode=<m>` ("the object at this path was
exactly this"), `absent` ("nothing was here"), `;dirty=<h>` ("and the
uncommitted content hashed to this"). Validation asks whether the current tree
still yields each stored observation; for these, that is string equality with
the current fingerprint.

This proposal adds one more kind of observation, narrower than the oid:

```
query:<line>\n<line>…      the file, parsed as a data format, answers these
                           queries this way
```

The key stays the repo path. A target that read the file wholesale records the
oid, as today; a target that only queried it records the sorted set of
`query → answer` lines it observed; a target that did both records the oid,
which subsumes the queries. One path, one entry, however many queries.

This is the "narrower observed property" format the guardrails anticipate
("future formats may represent narrower observed properties when they can
prove the same oracle"), and #72's synthetic keys were the deviation from it:
they moved the observation into the key, which is why they needed a key
grammar, a `#` special case in validation, and a tec change. With the
observation back in the value:

- keys are repo paths, period; explainer §4.2 and the `.meta/manifest.json#`
  rule go away;
- `tec query affected-targets` is correct unchanged: a manifest edit matches
  the key `.meta/manifest.json`, so every target that queried it is selected
  (conservatively, as it is for every path today). #2123770 becomes
  unnecessary;
- the public shape stays `path = fingerprint` (guardrail), and the TXDC row
  format is untouched.

## 3. The builtin

```nix
builtins.tecnixQueryFile {
  path = ../../.meta/manifest.json;   # path (or string) naming a file
  format = "json";                    # "json" | "toml"; default: from the extension
  op = "get";                         # "get" | "has" | "keys" | "find"
  at = [ "//areas/tools/dev" ];       # list of object keys, root-down
  # find only:
  # where = "id";                     # field of each child of `at` to compare
  # value = "W-123456";               # string
}
```

| op | returns | when `at` does not resolve |
|---|---|---|
| `get` | the value at `at`, as `fromJSON` would produce it | `null` |
| `has` | `true` if `at` resolves (a present `null` counts) | `false` |
| `keys` | sorted list of the object's attribute names | `null` (also when not an object) |
| `find` | sorted list of child keys `k` of the object at `at` whose field `where` is a string equal to `value` | `[ ]` |

`at` is a list, not a dotted string: zone paths contain `/` and `.`. Segments,
`where` and `value` are strings in v1; array indices, nested `where` paths and
non-string `find` values can be added as new ops or a tagged segment form later
without disturbing the format (§5 explains why).

A missing file is not an error: every op sees an absent document (`get` →
`null`, `has` → `false`, `keys` → `null`, `find` → `[ ]`). An unparseable file
is an evaluation error, like `fromJSON`. TOML is parsed with the toml11 parser
`fromTOML` uses, into the same tree shape; datetimes become
`{ _type = "timestamp"; value = "<normalized>"; }` unconditionally.

Outside the Tecnix repo accessor (a plain `nix eval` in a checkout) the
builtin is `fromJSON (readFile path)` plus the selection, and the read records
whatever a normal read of that path records. The same resolver code works
tracked and untracked.

One builtin with an attrset: one slot in the fixed-size base environment
(`baseEnvCapacity`, which #72 ran into; this is net −2 builtins), and a new
operation is a new `op` value. `has` stays because it is finer than `get` (a
zone-root check would otherwise be invalidated by an id change); `find` stays
because it is the generic form of the id → path lookup, which otherwise
depends on the whole file.

## 4. Recording, flattening, validating

### 4.1 Recording (eval time)

The builtin resolves `path` (symlinks like `readFile`), checks that it lies
under the Tecnix repo mount, reads the file **unrecorded**
(`TecnixSourceAccessor::readFileUnrecorded`), parses it once per evaluation
(a concurrent map on `TecnixEvalData`, replacing `tecnixManifestJson`),
evaluates the op, and records one observation into the current frame:

```
<file>\0<line>          e.g.  .meta/manifest.json\0json\tget\t//areas/tools/dev\t{"id":"W-c10e84"}
```

through the ordinary `TrackingContext::recordAccess`. The observation carries
the answer the target actually saw, built into a thread-local scratch buffer;
`internAccess` allocates only on first sight, like a path. From here on the
label machinery does everything it does for paths: memoized values carry it,
`tecnixMemoize` replays it, parallel workers keep it thread-confined.

### 4.2 Flattening (end of a target's evaluation)

`dependencyFingerprints` receives the flattened observation strings. It
partitions them: plain paths fingerprint as today; `\0` observations are
grouped by file. A file with a plain observation gets the oid and its query
lines are dropped (the oid subsumes them). A file with only query lines gets
`query:` + the lines, deduplicated, sorted, joined with `\n`. Output is a
`DependencyClosure` as before; nothing downstream (rows, `includeDependencies`,
tec) changes.

### 4.3 Validation (warm lookup)

`candidateMatches` currently computes the path's current fingerprint and
compares strings. It becomes `fingerprintHolds(path, stored)`:

- stored does not begin with `query:` → current fingerprint equals stored, as
  today, memoized per path;
- stored begins with `query:` → split into lines on `\n` and fields on `\t`
  (`string_view`s, no allocation), and for each line: parse the file (once per
  run, per file, shared across threads on the per-run
  `DependencyFingerprintCache`; read unrecorded), evaluate the op, encode the
  answer, compare with the line's answer. Memoized per `(path, line)`, so a
  `has //areas` shared by thousands of targets is evaluated once. A line that
  does not parse, an unknown op or format, or a file that does not parse →
  `false` (a miss; fail closed).

The hit path still works directly on the stored row's bytes; the only heap
structure is the parsed document, one per file per run, which plays the role
the git object store plays for oids.

### 4.4 What this does not change

Fingerprints are computed by one C++ function on both sides
(`evaluateQuery` → `encodeAnswer`), so int-vs-float, duplicate keys (nlohmann:
last wins), sorting and unicode cannot disagree between eval and validation.
Old rows (keys `.meta/manifest.json#…`, fingerprints `manifest-entry:…`) are
plain paths that don't exist → `absent` ≠ stored → a miss. No TXDC version
bump; `formatIsPinned` says so if it's wrong.

## 5. Formats

### 5.1 Observation line

```
<format> \t <op> \t <arg…> \t <answer>
```

- `format` ∈ `json`, `toml`; `op` ∈ `get`, `has`, `keys`, `find`.
- Fixed-arity args come first (`find`'s `where` and `value`), then the `at`
  segments, then the answer. The **answer is always the last field**, so a
  variable number of `at` segments is unambiguous, including zero
  (`json\tkeys\tsha256:…`).

Per op:

```
json\tget\t<at…>\t<answer>
json\thas\t<at…>\ttrue|false
json\tkeys\t<at…>\t<answer>
json\tfind\t<where>\t<value>\t<at…>\t<answer>
```

`where` is one field name in v1 (it covers the id lookup); a nested `where`
path would be a second variable-length run and needs a count or a new op,
which is why it is left out until something needs it.

String arguments may not contain `\t`, `\n` or NUL (an evaluation error; no
config key, zone path, crate name or id does). Compact JSON never contains
them raw, so answers are safe without escaping. This is what makes parsing a
`split`, and why strings-only arguments in v1 cost nothing later: a tagged
segment form (`i:0`) or a new op fits the same grammar.

### 5.2 Answer

```
absent                 get/keys on a path that does not resolve
<compact JSON>         when ≤ 64 bytes: {"id":"W-c10e84"}, true, ["//areas/tools/dev"]
sha256:<hex>           otherwise (the 1.5k-entry key list)
```

A bare `absent` cannot collide with JSON (no JSON value starts with `a`). The
64-byte bound is a constant; it only trades row bytes for readability.

### 5.3 Fingerprint

```
query:<line>\n<line>…      lines deduplicated and sorted bytewise
```

Readable in `nix eval --json` output (`\t`, `\n` escapes), opaque to everything
that only compares strings, and `tec` sees the path it already knows.

## 6. Implementation

### 6.1 Pure core: `src/libexpr/tecnix/data-query.{hh,cc}`

No `EvalState`, no accessor, no tracking. The header is the normative
statement of the format. In outline:

```cpp
enum class DataFormat { Json, Toml };
enum class DataQueryOp { Get, Has, Keys, Find };

struct DataQuery {                       // one line, minus the answer; views borrow
    DataFormat format; DataQueryOp op;
    std::vector<std::string_view> at;
    std::string_view where, value;       // Find only
};

class ParsedDataFile {                   // shared per evaluation / per validation run
    static std::shared_ptr<const ParsedDataFile> parse(DataFormat, std::string_view);  // throws DataFileParseError
    static std::shared_ptr<const ParsedDataFile> absent();
    std::shared_ptr<const FindIndex> findIndex(const DataQuery &) const;  // (at, where) → value → keys, lazy
};

DataQueryAnswer evaluateDataQuery(const ParsedDataFile &, const DataQuery &);   // optional<json>; nullopt = absent
void appendDataQueryAnswer(std::string &, const DataQueryAnswer &);            // absent | compact JSON ≤ 64 | sha256:
void appendDataObservation(std::string &, const DataQuery &, std::string_view answer);
std::optional<DataObservation> parseDataObservation(std::string_view line);

std::string dataQueryFingerprint(std::vector<std::string_view> lines);          // "query:" + sorted, deduplicated
std::optional<std::vector<std::string_view>> dataQueryFingerprintLines(std::string_view);
void appendDataAccess(std::string &, std::string_view file, std::string_view line);   // <file>\0<line>
std::optional<DataAccess> splitDataAccess(std::string_view);
```

TOML goes through toml11 into the same `nlohmann::json` tree shape
`fromTOML` produces (`fromTOML.cc` itself is untouched).

### 6.2 Flatten and validation: `tecnix/eval-cache.cc`

- `dependencyFingerprints` partitions observations and groups queries by file
  (§4.2).
- `dependencyFingerprintHolds(accessor, path, stored, cache)` replaces the
  string equality in `candidateMatches`. Object fingerprints are memoized per
  path as before; observations per `(path, line)` in the same thread-local
  cache; parsed documents live on the per-run `DependencyFingerprintCache`
  (`DependencyDataFiles`, a `Sync<map>` keyed by `<path>\0<format>`, shared
  across threads; a null entry is a file that does not parse).
- Validation reads the data file through `readSourceFileUnrecorded`
  (`source-accessors.hh`), which uses `TecnixSourceAccessor::readFileUnrecorded`
  when it can and a plain read otherwise (unit tests pass a
  `MemorySourceAccessor`). Today validation runs before the target's tracking
  context exists, so a recorded read would be harmless, but #80 validates
  memo rows *inside* a tracked evaluation, where it would put the whole file
  back into the enclosing target's closure.
- The manifest thread-locals and `manifestPathFingerprint` are gone.

### 6.3 The builtin: `primops/tecnix-query.cc`

Argument validation naming the argument in every error; `realisePath` like
`readFile`; `tecnixRepoRelativePath` (`source-accessors.hh`) decides whether
the file is under the repo accessor's mount; the per-evaluation document cache
is `TecnixEvalData::dataFileCache`; the answer becomes a value through
`parseJSON` of its compact dump; the observation is built in thread-local
scratch buffers and recorded only when tracking is active and the file is in
the repo. Outside the repo the read is an ordinary one (`lstat`, not
`maybeLstat`: a path pure evaluation forbids must throw as `readFile` would,
not read as absent).

### 6.4 Removals

`tectonixManifestEntry`, `tectonixManifestKeys`, `tectonixManifestIdToPath`
and docs; `getTecnixManifestJson` and `TecnixEvalData::tecnixManifestJson{,Flag}`.
Nothing on World `main` calls the three builtins; Burke's open stack
feature-detects them and falls back to whole-file reads. The
`unsafeTectonixInternalManifest*` builtins stay: World's non-repo-rooted path
uses them, and they correctly record the whole file.

### 6.5 Tests

Unit, `src/libexpr-tests/tecnix-data-query.cc` (replaces
`tecnix-manifest-fingerprint.cc`):

- line round-trips; rejected characters; answer encoding thresholds
- op semantics including every absent case
- `fingerprintHolds`: a stored `get` holds when other entries change and key
  order changes; fails when its entry changes, appears or disappears; `keys`
  holds unless the key set changes; `find` holds unless the match set
  changes; the same tree as JSON and as TOML; reformat and comments hold;
  missing file: stored `absent`/`false` answers hold until the queried entry
  appears; unparseable file never holds; unknown op or format never holds
- grouping: oid subsumes query lines; lines dedupe and sort; two files, two
  entries
- the parsed-document cache follows the accessor across
  `DependencyFingerprintCache` generations (today's
  `FollowsTheAccessorAcrossCaches`)
- `find` via the index equals a naive scan on a generated 2k-entry document

Functional, `tests/functional/tecnix/data-query.sh` (replaces
`manifest-tracking.sh`, keeping its memoize cases): a JSON manifest and a TOML
file; one target per op, one that queries twice (two lines, one entry), one
that reads the whole file (oid only); across commits: adding an entry hits
`get`/`has`/`find` and misses `keys` and the whole-file reader; changing one
entry misses only that entry's `get` and the `find` that pointed at it; a
comment-only TOML edit hits everything but the whole-file reader; a dirty
working-tree edit is seen by the value and by validation; an older rev of a
moved-on checkout reads that rev's file; tracked and untracked evaluations
give identical values; `base-env.sh` still passes.

Mutation checks: dropping the unrecorded read makes a query target's closure
contain the oid (the test checks it is absent); dropping the subsumption emits both.

Bench: `tecnix-eval-bench.cc` gains validating 1.5k `get` lines plus 1.5k
`find` lines against a 90 KB document.

### 6.6 Docs

Explainer §4: fingerprints record observations; the vocabulary gains `query:`;
§4.2 is deleted. Guardrails: "Public dependency output remains
path-to-fingerprint data" gains "a fingerprint may record a narrower
observation than the object id; validation must prove the observed result".
The builtin's `.doc` is the normative statement of §3 and §5.

## 7. World and tec

Everything funnels through the one file Burke's #2121412 introduces,
`manifest-access.nix`:

```nix
let
  q = args: builtins.tecnixQueryFile ({ path = ../../.meta/manifest.json; format = "json"; } // args);
in {
  entry      = zonePath: q { op = "get";  at = [ zonePath ]; };          # { id = …; } or null
  hasZone    = zonePath: q { op = "has";  at = [ zonePath ]; };
  zonePaths  =           q { op = "keys"; at = [ ]; };
  pathsForId = id:       q { op = "find"; at = [ ]; where = "id"; value = id; };
}
```

with the existing fallbacks (whole-file `fromJSON (readFile …)` when
`builtins ? tecnixQueryFile` is false; `unsafeTectonixInternalManifest*` when
not repo-rooted). `__invertedManifestById.${id}` becomes `pathsForId id`
(`[ ]` → not found, `[ p ]` → `p`, more → duplicate, as the inverted builtin
throws today). `isZone` in per-crate Rust becomes `hasZone`. The
`repoRooted = pathExists ../../.meta/manifest.json` check still records the
oid; #2117071 moves it, or `q { op = "has"; at = [ ]; }` is an existence
check that depends on no content.

Landing order: tecnix PR → `tecnix-package.nix` bump → `manifest-access.nix`.
No tec change is required. Later, a precise `affected-targets` is one small
builtin, `builtins.tecnixFingerprintsHold { gitDir; rev; closure; }`, that
runs §4.3's `fingerprintHolds` over a stored closure at a revision; tec never
parses a fingerprint.

Housekeeping: the in-repo `//system/tecnix` copy is pinned to a September fork
commit without #72 and would have to catch up before anyone pointed the
overlay at it.

## 8. Cost

Eval side, per call: force a small attrset, a map lookup, the op, a Value
conversion, one scratch-buffer write, one `internAccess` (allocates on first
sight only). Validation: each file parsed once per run and shared across
threads (stand-in measurement: 0.4 ms for the 91 KB manifest); each distinct
`(path, line)` evaluated once (~3 ms for all 1.5k entries); `find` O(1) after
a one-time index. Rows: a query entry's string is the target's sorted lines,
so two targets share it only when they asked exactly the same questions;
~100–200 bytes per target against today's ~187-entry closures. Not doing now:
a persisted `(file oid, line) → holds` memo; it needs no format change if it
is ever wanted.

## 9. Interaction with #80 (`tecnixPersistentMemo`)

Validation reads must be unrecorded (§6.2). On a memo hit, #80 replays the
row's closure into the enclosing frame; for a `query:` entry that means
recording each line as a `<file>\0<line>` observation, so the enclosing target
records the queries, not the oid. Queries and memos stay complementary:
queries make rows depend on less, memos make misses cheaper.

## 10. Rejected shapes

- **Synthetic keys** (#72's `.meta/manifest.json#…`, and a generic
  `/<json>` or `//<file>#<query>` form). Moves the observation into the key,
  so it needs an exact key grammar, a special case in validation that parses
  keys, and a tec change to map keys back to files. The observation already
  lives in the value for every other fingerprint.
- **A Nix function as the query, with early cutoff.** Validation would
  evaluate Nix for every query whose inputs changed, needs an evaluator at the
  revision inside the cache layer, and `affected-targets` could no longer
  decide without evaluating. Belongs in #80's memo, not in target validation.
- **Auto-tracking `fromJSON` attribute selection.** Hooks in the evaluator's
  hottest path, precision depends on how calling code is written, and id
  lookups still depend on the whole file.
