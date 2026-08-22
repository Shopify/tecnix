#!/usr/bin/env bash
# Regression test: Value cells owned by the Tecnix target builtins must stay
# reachable by the conservative garbage collector for their whole lifetime.
#
# prim_tecnixTargets pre-allocates one result cell per target, and the
# dependency path holds worker-produced target values until the coordinator
# consumes them. Both used to keep the only reference to those cells in plain
# std::vector heap buffers, which Boehm GC does not scan. Under allocation
# pressure the collector recycled the cells mid-call; a recycled cell that had
# become another thread's thunk crashed at the ValueStorage::finish pdThunk
# tripwire ("Unexpected condition in ... finish(...)"), and a recycled cell
# holding an unrelated finished value produced silently wrong results.
#
# This test evaluates many allocation-heavy targets with a tiny initial GC
# heap (forcing frequent collections during the call) and asserts the exact
# expected content of every target, sequentially and in parallel, with and
# without dependency records.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

requireGit

NTARGETS=400

GC_WORLD="$TEST_ROOT/tecnix-gc-world"

create_tecnix_gc_test_world() {
    local dir="$1"

    git init -q "$dir"

    mkdir -p "$dir/system/tectonix"
    cat > "$dir/system/tectonix/resolve.nix" << 'RESOLVE_EOF'
{ n }:
let
  # Allocation-heavy per-target computation: enough thunks, strings, and
  # lists per target to force GC cycles while the builtin is mid-call.
  mkTarget = target:
    let
      junk = builtins.genList (i: {
        s = "x-${target}-${toString i}";
        l = builtins.genList (j: j) 30;
      }) 300;
    in {
      name = target;
      marker = builtins.concatStringsSep "," (map (x: x.s) junk);
      drvPath = "/nix/store/00000000000000000000000000000000-${target}.drv";
    };
in
{
  resolve = mkTarget;
  allTargetNames = builtins.genList (i: "t${toString i}") n;
}
RESOLVE_EOF

    git -C "$dir" -c user.email=test@example.com -c user.name=Test add -A
    git -C "$dir" -c user.email=test@example.com -c user.name=Test commit -qm 'tecnix gc test world'
}

create_tecnix_gc_test_world "$GC_WORLD"
GC_HEAD_SHA=$(git -C "$GC_WORLD" rev-parse HEAD)

gc_args() {
    cat <<EOF
{
  gitDir = "$GC_WORLD/.git";
  resolver = "system/tectonix/resolve.nix";
  args = { n = $NTARGETS; };
  rev = "$GC_HEAD_SHA";
  targets = builtins.genList (i: "t\${toString i}") $NTARGETS;
}
EOF
}

# A tiny initial heap makes initGC skip its large pre-expansion, so the
# collector runs many times during the tecnixTargets call. Without rooted
# result cells this crashed or corrupted results on nearly every run.
gc_eval_json() {
    local expr="$1"
    shift
    GC_INITIAL_HEAP_SIZE=1000000 nix eval --json \
        --option lazy-trees true \
        --option tecnix-eval-cache false \
        "$@" \
        --expr "$expr"
}

gc_eval_json_sequential() {
    gc_eval_json "$1" --extra-experimental-features 'nix-command'
}

gc_eval_json_parallel() {
    gc_eval_json "$1" --extra-experimental-features 'nix-command parallel-eval' --eval-cores 4
}

# Every target's result must be intact and its own: name equals the target id,
# and the marker string carries the target id through its full length. A
# recycled or cross-wired result cell fails these checks even when it does not
# crash.
assert_gc_targets_intact() {
    local json="$1" mode="$2"
    local filter
    filter="length == $NTARGETS and (to_entries | all(
      .key as \$k
      | .value.name == \$k
      and (.value.marker | startswith(\"x-\(\$k)-0,\"))
      and (.value.marker | endswith(\"x-\(\$k)-299\"))))"
    if ! jq -e "$filter" >/dev/null <<< "$json"; then
        printf '%.2000s\n' "$json" >&2
        fail "tecnixTargets under GC pressure returned wrong results ($mode)"
    fi
}

echo "Testing tecnixTargets result cells survive GC (sequential)..."
seq_targets=$(gc_eval_json_sequential "builtins.tecnixTargets ($(gc_args))")
assert_gc_targets_intact "$seq_targets" "sequential"

echo "Testing tecnixTargets result cells survive GC (parallel)..."
par_targets=$(gc_eval_json_parallel "builtins.tecnixTargets ($(gc_args))")
assert_gc_targets_intact "$par_targets" "parallel"

[[ "$seq_targets" == "$par_targets" ]] || fail "sequential and parallel tecnixTargets output should be identical"

# The dependency path holds each worker-produced target value until the
# coordinator assembles the records; those cells must be rooted across the
# fingerprinting work in between.
echo "Testing tecnixTargets includeDependencies target values survive GC (parallel)..."
dep_records=$(gc_eval_json_parallel "
  builtins.listToAttrs (map (record: {
    name = record.target;
    value = record.value // { deps = record.dependencies; };
  }) (builtins.tecnixTargets (($(gc_args)) // { includeDependencies = true; })))")
assert_gc_targets_intact "$dep_records" "includeDependencies"
if ! jq -e '[.[] | .deps | has("system/tectonix/resolve.nix")] | all' >/dev/null <<< "$dep_records"; then
    fail "includeDependencies records should carry the resolver dependency for every target"
fi
