#!/usr/bin/env bash
# Tecnix evaluation tracing (`tecnix-trace`): the recorder's output over a
# small repository shaped like plans/tecnix-tracing/README.md §2.1 -- two
# targets sharing `lib/util.nix`, one of them alone importing `lib/heavy.nix`,
# both aliasing `lib.foo` through a `let`.
#
# Everything asserted here is deterministic: counts, positions, bytes and
# consumer sets. No duration is compared. The fixture deliberately has no
# value that one root uses through two of its own siblings (README §2.2's
# accepted imprecision), so `shared` and `floor` are order-invariant here.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

requireGit

TRACE_WORLD="$TEST_ROOT/tecnix-trace-world"

create_tecnix_trace_test_world() {
    local dir="$1"

    git init -q "$dir"
    mkdir -p "$dir/build/resolver" "$dir/lib" "$dir/services/api" "$dir/services/web"

    cat > "$dir/build/resolver/resolve.nix" << 'RESOLVE_EOF'
args:
let
  lib = import ../../lib/util.nix;
  targets = {
    "//services/api" = import ../../services/api/target.nix { inherit args lib; };
    "//services/web" = import ../../services/web/target.nix { inherit args lib; };
  };
in {
  allTargetNames = builtins.attrNames targets;
  resolve = id: targets.${id};
}
RESOLVE_EOF

    # Line numbers below are asserted on: `foo` is defined on line 5.
    cat > "$dir/lib/util.nix" << 'UTIL_EOF'
let
  table = builtins.genList (i: { n = i; s = builtins.toString (i * i); }) 50;
in
{
  foo = builtins.concatStringsSep "," (map (x: x.s) table);
  size = builtins.length table;
  double = x: x * 2;
}
UTIL_EOF

    cat > "$dir/lib/heavy.nix" << 'HEAVY_EOF'
{ lib }:
let big = builtins.genList (i: lib.double i) 500;
in { sum = builtins.foldl' (a: b: a + b) 0 big; }
HEAVY_EOF

    cat > "$dir/services/api/target.nix" << 'API_EOF'
{ args, lib }:
let
  heavy = import ../../lib/heavy.nix { inherit lib; };
  x = lib.foo;
in {
  name = "api";
  drvPath = "/nix/store/00000000000000000000000000000000-api-${builtins.toString heavy.sum}-${builtins.substring 0 8 (builtins.hashString "sha256" x)}.drv";
}
API_EOF

    cat > "$dir/services/web/target.nix" << 'WEB_EOF'
{ args, lib }:
let
  x = lib.foo;
  # a source copy into the store: I/O the trace keeps apart from evaluation
  util = builtins.path { path = ../../lib/util.nix; name = "util"; };
in {
  name = "web";
  drvPath = "/nix/store/00000000000000000000000000000000-web-${builtins.substring 0 8 (builtins.hashString "sha256" x)}-${baseNameOf util}.drv";
}
WEB_EOF

    git -C "$dir" add -A
    git -C "$dir" -c user.email=test@example.com -c user.name=Test -c commit.gpgsign=false commit -q -m "trace world"
}

create_tecnix_trace_test_world "$TRACE_WORLD"
TRACE_HEAD=$(get_head_sha "$TRACE_WORLD")

trace_base_args="{ gitDir = \"$TRACE_WORLD/.git\"; checkoutPath = \"$TRACE_WORLD\"; rev = \"$TRACE_HEAD\"; resolver = \"build/resolver/resolve.nix\"; args = { system = \"test-system\"; }; }"

# tecnix_trace_eval <trace-dir|""> <expr> [extra nix args...]
tecnix_trace_eval() {
    local traceDir="$1" expr="$2"
    shift 2
    local -a traceOpt=()
    if [[ -n "$traceDir" ]]; then
        traceOpt=(--option tecnix-trace "$traceDir")
    fi
    nix eval --json \
        --extra-experimental-features 'nix-command parallel-eval' \
        --option lazy-trees true \
        --option tecnix-eval-cache false \
        "${traceOpt[@]}" \
        "$@" \
        --expr "$expr"
}

# The one file a run writes, or fail.
trace_file() {
    local dir="$1"
    local files=("$dir"/tecnix-trace-*.sqlite)
    [[ ${#files[@]} -eq 1 && -f "${files[0]}" ]] || fail "expected exactly one trace file in $dir, found: ${files[*]}"
    echo "${files[0]}"
}

q() {
    sqlite3 "$1" "$2"
}

assert_q() {
    local db="$1" sql="$2" expected="$3" message="$4"
    local actual
    actual=$(q "$db" "$sql")
    if [[ "$actual" != "$expected" ]]; then
        echo "query: $sql" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        fail "$message"
    fi
}

targets_expr() {
    echo "builtins.tecnixTargets (($trace_base_args) // { targets = [ $1 ]; includeDependencies = true; includeTargets = false; })"
}

# ---------------------------------------------------------------------------
echo "Testing a sequential trace..."
SEQ_DIR="$TEST_ROOT/trace-seq"
seq_out=$(tecnix_trace_eval "$SEQ_DIR" "$(targets_expr '"//services/api" "//services/web"')")
SEQ_DB=$(trace_file "$SEQ_DIR")

assert_q "$SEQ_DB" "select value from meta where key = 'format_version'" "2" "meta carries the format version"
assert_q "$SEQ_DB" "select value from meta where key = 'resolver'" "build/resolver/resolve.nix" "meta carries the resolver"
assert_q "$SEQ_DB" "select value from meta where key = 'rev'" "$TRACE_HEAD" "meta carries the rev"
assert_q "$SEQ_DB" "select group_concat(name, ' ') from (select name from roots order by id)" \
    "resolver //services/api //services/web" "one root per target plus the resolver, in evaluation order"
assert_q "$SEQ_DB" "select count(*) from records where kind = 'root'" "3" "every root has a record"
assert_q "$SEQ_DB" "select count(*) from records where kind = 'wait'" "0" "a sequential run never waits"
assert_q "$SEQ_DB" "select count(*) from records r where r.expr is not null and r.expr not in (select id from exprs)" "0" \
    "every record key resolves to an exprs row"
assert_q "$SEQ_DB" "select count(*) from records where kind = 'value' and expr is null" "0" \
    "every value record has a position (thunk expression, applied lambda, or force site)"
assert_q "$SEQ_DB" "select count(*) from exprs where file = 'lib/util.nix' and line is null" "1" \
    "the import of lib/util.nix is keyed on the file, repo-relative"
assert_q "$SEQ_DB" "select count(*) from phases where name in ('evaluate', 'fingerprint', 'dump')" "3" \
    "the non-evaluation phases of the call are timed"

# Aliases resolve to their producer: both targets bind `x = lib.foo`, and the
# reuse names the record that computed `foo` (lib/util.nix line 5), which is
# therefore shared. The resolver's `resolve` lambda is needed by all three roots.
assert_q "$SEQ_DB" "select roots from consumers where file = 'lib/util.nix' and line = 5 and col = 9" \
    "//services/api,//services/web" "lib.foo, reached only through aliases, is consumed by both targets"
assert_q "$SEQ_DB" "select roots from consumers where file = 'lib/util.nix' and line is null" \
    "//services/api,//services/web" "the lib/util.nix import is consumed by both targets"
assert_q "$SEQ_DB" "select roots from consumers where file = 'build/resolver/resolve.nix' and line = 10" \
    "//services/api,//services/web,resolver" "the resolve lambda is consumed by every root"
assert_q "$SEQ_DB" "select consumers from floor where file = 'lib/util.nix' and line is null" "2" \
    "floor.consumers counts the computing root and the reusing root"
assert_q "$SEQ_DB" "select count(*) from floor where file = 'lib/heavy.nix'" "0" "heavy.nix is one target's own work, not floor"
assert_q "$SEQ_DB" "select count(*) from own" "3" "own has one row per root"
# target_costs: own + the shared work each target needs, amortised over consumers.
assert_q "$SEQ_DB" "select count(*) from target_costs" "3" "target_costs has one row per root"
assert_q "$SEQ_DB" "select count(*) from target_costs t join own o on o.root = t.name where o.own_dur <> t.own_dur or o.own_gc_alloc <> t.own_gc_alloc" "0" \
    "target_costs.own agrees with own"
assert_q "$SEQ_DB" "select count(distinct shared_gc_alloc) from target_costs where name like '//services/%'" "1" \
    "both targets need the same shared work (util.nix, foo, the resolver's targets and resolve)"
assert_q "$SEQ_DB" "select shared_gc_alloc > 0 from target_costs where name = '//services/web'" "1" "web's shared work is not empty"
assert_q "$SEQ_DB" "select abs(sum(total_dur_amortized) - (select sum(self_dur) from records where kind in ('value', 'root'))) <= (select count(*) from roots) from target_costs" "1" \
    "amortised totals partition the run (to rounding)"
assert_q "$SEQ_DB" "select sum(own_gc_alloc) + sum(shared_gc_alloc_amortized) = (select sum(self_gc_alloc) from records where kind in ('value', 'root')) from target_costs" "1" \
    "amortised bytes partition the run exactly"
# I/O is recorded as `store`/`fetch` children of the frame that did it, and
# never counts as evaluation: the copy of util.nix into the store belongs to
# web, shows up in target_costs.store_dur, and is excluded from own_dur.
assert_q "$SEQ_DB" "select count(*) from records where kind = 'store'" "1" "builtins.path copied one source into the store"
assert_q "$SEQ_DB" "select ro.name from records r join roots ro on ro.id = r.root where r.kind = 'store'" "//services/web" "the store record belongs to the target that copied"
assert_q "$SEQ_DB" "select store_dur > 0 and fetch_dur = 0 from target_costs where name = '//services/web'" "1" "web paid store I/O and no fetch"
assert_q "$SEQ_DB" "select count(*) from records p join records c on c.parent = p.id where c.kind = 'store' and p.kind = 'value'" "1" \
    "the store record is a child of the thunk that copied"
assert_q "$SEQ_DB" "select count(*) from records where kind = 'store' and id in (select id from shared)" "0" "web's copy is its own"
assert_q "$SEQ_DB" "select store_dur_amortized = store_dur and fetch_dur_amortized = 0 from target_costs where name = '//services/web'" "1" \
    "own I/O is charged in full to the target that paid it"
assert_q "$SEQ_DB" "select sum(store_dur_amortized) = (select sum(self_dur) from records where kind = 'store') from target_costs" "1" \
    "amortised store I/O partitions the run's store I/O"
assert_q "$SEQ_DB" "select count(*) from shared s join records r on r.id = s.id join exprs e on e.id = r.expr where e.file = 'services/web/target.nix' or e.file = 'services/api/target.nix'" "0" \
    "nothing under a target file is shared"
# Every reuse crosses roots.
assert_q "$SEQ_DB" "select count(*) from reuses u join records p on p.id = u.producer where p.root = u.root" "0" \
    "a same-root use is never a reuse"

# ---------------------------------------------------------------------------
echo "Testing the oracle: parallel and permuted runs derive the same trace..."
PAR_DIR="$TEST_ROOT/trace-par"
par_out=$(tecnix_trace_eval "$PAR_DIR" "$(targets_expr '"//services/api" "//services/web"')" --eval-cores 2)
PAR_DB=$(trace_file "$PAR_DIR")
PERM_DIR="$TEST_ROOT/trace-perm"
tecnix_trace_eval "$PERM_DIR" "$(targets_expr '"//services/web" "//services/api"')" > /dev/null
PERM_DB=$(trace_file "$PERM_DIR")

# assert_same_derivation <what> <sql> <db>: the query gives the same rows for
# the sequential run and for <db>. Which root computed a value is an accident
# of ordering; nothing derived is.
assert_same_derivation() {
    local what="$1" sql="$2" db="$3"
    if ! diff -u <(q "$SEQ_DB" "$sql") <(q "$db" "$sql"); then
        fail "$what must not depend on evaluation order ($db)"
    fi
}
for db in "$PAR_DB" "$PERM_DB"; do
    assert_same_derivation "the multiset of (position, self_gc_alloc) over value records" \
        "select e.file, e.line, e.col, r.self_gc_alloc from records r left join exprs e on e.id = r.expr where r.kind = 'value' order by 1, 2, 3, 4" "$db"
    assert_same_derivation "consumers" "select file, line, col, roots from consumers order by 1, 2, 3" "$db"
    assert_same_derivation "floor" "select file, line, col, self_gc_alloc, consumers from floor order by 1, 2, 3, 4" "$db"
    assert_same_derivation "shared" \
        "select e.file, e.line, e.col from shared s join records r on r.id = s.id left join exprs e on e.id = r.expr order by 1, 2, 3" "$db"
    assert_q "$db" "select group_concat(name, ' ') from (select name from roots order by name)" \
        "//services/api //services/web resolver" "same roots ($db)"
    assert_q "$db" "select count(*) from own" "3" "own has one row per root ($db)"
done
assert_q "$PAR_DB" "select count(*) from records where kind = 'wait' and (gc_alloc <> 0 or self_gc_alloc <> 0)" "0" \
    "waits carry no work"
assert_q "$PAR_DB" "select count(distinct id >> 32) >= 2 from records" "1" \
    "records from different threads carry the thread in their high bits"

# ---------------------------------------------------------------------------
echo "Testing that closures are untouched by tracing..."
plain_out=$(tecnix_trace_eval "" "$(targets_expr '"//services/api" "//services/web"')")
if ! diff -u <(jq -S . <<< "$plain_out") <(jq -S . <<< "$seq_out"); then
    fail "includeDependencies output must be byte-identical with and without tecnix-trace"
fi
if ! diff -u <(jq -S . <<< "$plain_out") <(jq -S . <<< "$par_out"); then
    fail "includeDependencies output must be identical under parallel evaluation with tracing"
fi

# ---------------------------------------------------------------------------
echo "Testing that an evaluation error still produces a trace..."
ERR_DIR="$TEST_ROOT/trace-err"
if tecnix_trace_eval "$ERR_DIR" "$(targets_expr '"//services/api" "//services/missing"')" 2> "$TEST_ROOT/trace-err.log"; then
    fail "evaluating a missing target should fail"
fi
grepQuiet "attribute '\"//services/missing\"' missing" "$TEST_ROOT/trace-err.log"
ERR_DB=$(trace_file "$ERR_DIR")
assert_q "$ERR_DB" "select count(*) from roots where name = '//services/missing'" "1" "the failing target has a root"
assert_q "$ERR_DB" "select count(*) from records where kind = 'root'" "3" "every root record was closed on the error path"
assert_q "$ERR_DB" "select count(*) from records where kind = 'value'" "$(q "$ERR_DB" "select count(*) from records where kind = 'value'")" \
    "the trace is queryable"

# ---------------------------------------------------------------------------
echo "Testing that tecnix-trace makes a plain call tracked..."
# Without includeDependencies and with the cache off, tecnixTargets is not
# tracked -- unless a trace was asked for, since the trace rides on tracking.
PLAIN_DIR="$TEST_ROOT/trace-plain"
plain_targets=$(tecnix_trace_eval "$PLAIN_DIR" "builtins.tecnixTargets (($trace_base_args) // { targets = [ \"//services/api\" \"//services/web\" ]; })")
PLAIN_DB=$(trace_file "$PLAIN_DIR")
assert_q "$PLAIN_DB" "select group_concat(name, ' ') from (select name from roots order by id)" \
    "resolver //services/api //services/web" "a plain call is traced, one root per target"
assert_q "$PLAIN_DB" "select count(*) from phases where name = 'fingerprint'" "0" "no closure is fingerprinted when nobody needs it"
untraced_targets=$(tecnix_trace_eval "" "builtins.tecnixTargets (($trace_base_args) // { targets = [ \"//services/api\" \"//services/web\" ]; })")
if ! diff -u <(jq -S . <<< "$untraced_targets") <(jq -S . <<< "$plain_targets"); then
    fail "a plain call's result must not change when it is traced"
fi
if ! jq -e '.["//services/api"].drvPath | startswith("/nix/store/00000000000000000000000000000000-api-")' > /dev/null <<< "$plain_targets"; then
    fail "the traced plain call returns the target values"
fi

echo "Testing that several calls in one process append to one file..."
# `seq` completes discovery before the targets call starts: two outermost
# calls, two dumps, one file.
MULTI_DIR="$TEST_ROOT/trace-multi"
tecnix_trace_eval "$MULTI_DIR" "
  builtins.seq (builtins.tecnixTargetNames (($trace_base_args) // { includeDependencies = true; }))
    (builtins.tecnixTargets (($trace_base_args) // { targets = [ \"//services/api\" \"//services/web\" ]; includeDependencies = true; includeTargets = false; }))" > /dev/null
MULTI_DB=$(trace_file "$MULTI_DIR")
assert_q "$MULTI_DB" "select value from meta where key = 'dumps'" "2" "two outermost calls, two dumps into one file"
assert_q "$MULTI_DB" "select group_concat(name, ' ') from (select name from roots order by id)" \
    "discovery resolver //services/api //services/web" "discovery is a root of its own, before the targets"
assert_q "$MULTI_DB" "select count(*) from records where kind = 'root'" "4" "every root closed"
assert_q "$MULTI_DB" "select count(*) from (select id from records group by id having count(*) > 1)" "0" \
    "record ids stay unique across dumps"
assert_q "$MULTI_DB" "select count(*) from phases where name = 'discovery'" "1" "discovery is a phase of its call"
assert_q "$MULTI_DB" "select count(distinct call) from phases" "2" "each call's phases carry its id"
# The resolver module is applied once per process: discovery computed the
# `targets` set (attrNames), and the later call's targets select from it.
assert_q "$MULTI_DB" "select roots from consumers where file = 'build/resolver/resolve.nix' and line = 4" \
    "//services/api,//services/web,discovery" "a later call's roots reuse an earlier call's records"

echo "Testing that a call nested in another call's arguments is one trace..."
# Forcing `names` happens while the targets call parses its arguments, so
# discovery runs inside it: the dump waits for the outermost call.
NESTED_DIR="$TEST_ROOT/trace-nested"
tecnix_trace_eval "$NESTED_DIR" "
  let names = (builtins.tecnixTargetNames (($trace_base_args) // { includeDependencies = true; })).targets;
  in builtins.tecnixTargets (($trace_base_args) // { targets = names; includeDependencies = true; includeTargets = false; })" > /dev/null
NESTED_DB=$(trace_file "$NESTED_DIR")
assert_q "$NESTED_DB" "select value from meta where key = 'dumps'" "1" "the nested call does not dump on its own"
assert_q "$NESTED_DB" "select count(*) from records where kind = 'root'" "4" "both calls' roots are in the one dump"
assert_q "$NESTED_DB" "select count(distinct call) from phases" "2" "phases distinguish the nested call"

echo "All tecnix tracing tests passed"
