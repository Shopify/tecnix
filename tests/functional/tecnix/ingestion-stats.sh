#!/usr/bin/env bash
# NIX_SHOW_STATS attributes source ingestion (`fetchToStore`) to the evaluator
# code path that asked for it, to the Tecnix target being evaluated, and to the
# Nix expression position that caused it. It is statistics only.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TEST_WORLD="$TEST_ROOT/tecnix-world"
create_tecnix_builtin_test_world "$TEST_WORLD"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

assert_jq() {
    local json="$1"
    local filter="$2"
    local message="$3"
    if ! jq -e "$filter" >/dev/null <<< "$json"; then
        echo "$json" >&2
        fail "$message"
    fi
}

# `srcdir` coerces a path to a string (`src = ../src-dir`), `filteredSrc` calls
# `builtins.path` with a filter, and `alpha` reads no source at all.
targets_expr() {
    cat <<EOF
builtins.tecnixTargets {
  gitDir = "$TEST_WORLD/.git";
  resolver = "system/tectonix/resolve.nix";
  args = { system = "test-system"; };
  rev = "$HEAD_SHA";
  checkoutPath = "$TEST_WORLD";
  includeDependencies = true;
  includeTargets = false;
  targets = [
    "//areas/app/web:srcdir"
    "//areas/app/web:filteredSrc"
    "//areas/app/web:alpha"
  ];
}
EOF
}

eval_targets() {
    nix eval --json \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tecnix-eval-cache false \
        --expr "$(targets_expr)"
}

# The first evaluation is cold: nothing has been ingested into this test's store.
STATS_FILE="$TEST_ROOT/target-stats.json"
with_stats=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$STATS_FILE" eval_targets)
stats=$(cat "$STATS_FILE")

echo "Testing that statistics do not change evaluation results..."
plain=$(eval_targets)
[[ "$plain" == "$with_stats" ]] || fail "NIX_SHOW_STATS changed the evaluation result"

echo "Testing fetchToStore counters by caller..."
assert_jq "$stats" '.fetchToStore | keys == ["builtinsPath", "coercedPath", "devirtualize", "other", "tectonixTree", "tectonixZone"]' \
    "fetchToStore should report one entry per caller"
assert_jq "$stats" '.fetchToStore.coercedPath | .ingestions == 1 and .bytesCopied > 0 and .filteredIngestions == 0' \
    "the coerced path (srcdir) should be one unfiltered ingestion"
assert_jq "$stats" '.fetchToStore.builtinsPath | .ingestions == 1 and .bytesCopied > 0 and .filteredIngestions == 1' \
    "builtins.path with a filter (filteredSrc) should be one filtered ingestion"
assert_jq "$stats" '.fetchToStore.other.calls == 0 and .fetchToStore.tectonixZone.calls == 0' \
    "no ingestion should be charged to callers that did not run"

echo "Testing fetchToStore attribution by target..."
assert_jq "$stats" '.fetchToStoreByTarget | keys == ["//areas/app/web:filteredSrc", "//areas/app/web:srcdir"]' \
    "only targets that caused ingestion should be listed"
assert_jq "$stats" '.fetchToStoreByTarget["//areas/app/web:srcdir"].bytesCopied == .fetchToStore.coercedPath.bytesCopied' \
    "srcdir should be charged the coerced path's bytes"
assert_jq "$stats" '.fetchToStoreByTarget["//areas/app/web:filteredSrc"].bytesCopied == .fetchToStore.builtinsPath.bytesCopied' \
    "filteredSrc should be charged builtins.path's bytes"

echo "Testing that a warm evaluation counts cache hits, not ingestions..."
WARM_STATS_FILE="$TEST_ROOT/warm-stats.json"
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$WARM_STATS_FILE" eval_targets >/dev/null
warm=$(cat "$WARM_STATS_FILE")
assert_jq "$warm" '.fetchToStore.coercedPath | .calls == 1 and .ingestions == 0 and .persistentCacheHits == 1' \
    "a repeated coerced path should be a persistent cache hit, not an ingestion"
# A filtered ingestion bypasses both caches, so filteredSrc still pays every run.
assert_jq "$warm" '.fetchToStore.builtinsPath | .calls == 1 and .ingestions == 1 and .persistentCacheHits == 0' \
    "a filtered builtins.path should ingest again"
assert_jq "$warm" '.fetchToStoreByTarget | keys == ["//areas/app/web:filteredSrc"]' \
    "only the target that ingested again should be charged"

echo "Testing fetchToStore attribution by expression position..."
SITE_STATS_FILE="$TEST_ROOT/site-stats.json"
big="$TEST_WORLD/areas/app/web/targets"
small="$TEST_WORLD/areas/app/web/src-dir"
# Line 2 interpolates the larger directory into a string, line 3 the smaller one.
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$SITE_STATS_FILE" nix eval --raw --impure \
    --extra-experimental-features 'nix-command' \
    --expr "let
  a = \"\${$big}\";
  b = \"\${$small}\";
in a + b" >/dev/null
sites=$(cat "$SITE_STATS_FILE")
assert_jq "$sites" '.fetchToStoreBySite | length == 2 and (map(.line) == [2, 3])' \
    "each interpolation should be its own site, largest first"
assert_jq "$sites" '.fetchToStoreBySite | all(.ingestions == 1 and .bytesCopied > 0) and .[0].bytesCopied > .[1].bytesCopied' \
    "sites should be ordered by bytes ingested"
assert_jq "$sites" '.fetchToStoreByTarget | keys == ["(no target)"] and .["(no target)"].ingestions == 2' \
    "ingestion outside tecnixTargets should be charged to '(no target)'"

echo "Testing that an evaluation without source ingestion reports none..."
NONE_STATS_FILE="$TEST_ROOT/none-stats.json"
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$NONE_STATS_FILE" nix eval --raw --impure \
    --extra-experimental-features 'nix-command' \
    --expr '"no paths here"' >/dev/null
assert_jq "$(cat "$NONE_STATS_FILE")" '.fetchToStoreBySite == [] and .fetchToStoreByTarget == {} and .fetchToStore.coercedPath.calls == 0' \
    "an evaluation that ingests nothing should report empty attribution"

echo "All ingestion statistics tests passed!"
