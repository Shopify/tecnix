#!/usr/bin/env bash
# builtins.tecnixMemoize: `g = builtins.tecnixMemoize f` is `f`, with each
# `f k` evaluated once per evaluation and shared. What tracked calls record, and
# that tracked and untracked results stay apart, is in manifest-tracking.sh.

# shellcheck disable=SC2016
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

memo_eval() {
    nix eval --raw --extra-experimental-features 'nix-command' --expr "$1"
}

# How often `f` ran for `k`, per the `computing <k>` traces in file $1.
computed() {
    grep -c "trace: computing $2\$" "$1" || true
}

out=$(memo_eval 'builtins.tecnixMemoize (x: x + "!") "k"')
[[ "$out" == "k!" ]] || fail "g k should be f k, got '$out'"

# Every call for a key after the first reuses its result; each key is computed once.
memo_eval 'let g = builtins.tecnixMemoize (x: builtins.trace "computing ${x}" (x + "!"));
  in builtins.concatStringsSep "|" [ (g "k") (g "k") (g "k2") (g "k") ]' \
    > "$TEST_ROOT/once.out" 2> "$TEST_ROOT/once.err"
[[ "$(cat "$TEST_ROOT/once.out")" == "k!|k!|k2!|k!" ]] || fail "unexpected results: $(cat "$TEST_ROOT/once.out")"
[[ "$(computed "$TEST_ROOT/once.err" k)" == 1 ]] || fail "f should run once for 'k'"
[[ "$(computed "$TEST_ROOT/once.err" k2)" == 1 ]] || fail "f should run once for 'k2'"

# Results belong to the function: another function never sees them, even for an equal key.
out=$(memo_eval 'let g1 = builtins.tecnixMemoize (x: x + "!"); g2 = builtins.tecnixMemoize (x: x + "?");
  in builtins.seq (g1 "k") "${g1 "k"}|${g2 "k"}"')
[[ "$out" == "k!|k?" ]] || fail "each function should get its own results, got '$out'"

# Every g made from the same value of f shares them.
memo_eval 'let f = x: builtins.trace "computing ${x}" (x + "!"); g1 = builtins.tecnixMemoize f; g2 = builtins.tecnixMemoize f;
  in "${g1 "k"}|${g2 "k"}"' > "$TEST_ROOT/same-f.out" 2> "$TEST_ROOT/same-f.err"
[[ "$(cat "$TEST_ROOT/same-f.out")" == "k!|k!" ]] || fail "unexpected results: $(cat "$TEST_ROOT/same-f.out")"
[[ "$(computed "$TEST_ROOT/same-f.err" k)" == 1 ]] || fail "memoizing the same f twice should share results"

# Only strings are keys.
expectStderr 1 nix eval --extra-experimental-features 'nix-command' --expr 'builtins.tecnixMemoize (x: x) 1' \
    | grepQuiet "expected a string but found an integer"

# A call that needs its own result throws, catchably, instead of recursing forever.
out=$(memo_eval 'let g = builtins.tecnixMemoize (k: g k); in if (builtins.tryEval (g "a")).success then "no-throw" else "threw"')
[[ "$out" == "threw" ]] || fail "a circular call should throw a catchable error, got '$out'"

# Re-entering for a different key is not a cycle.
out=$(memo_eval 'let g = builtins.tecnixMemoize (k: if k == "root" then g "child" else k + "-val"); in g "root"')
[[ "$out" == "child-val" ]] || fail "expected 'child-val', got '$out'"
