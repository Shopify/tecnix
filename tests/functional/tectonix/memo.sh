#!/usr/bin/env bash
# Test builtins.tectonixMemo: evaluate `f key` once per EvalState and share the
# result (and its tracked source deps) across consumers within one evaluation.

# shellcheck disable=SC2016
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TEST_WORLD="$TEST_ROOT/world"
create_test_world "$TEST_WORLD"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

# 1. Correctness: tectonixMemo returns f key.
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.tectonixMemo "ns" "k" (x: x + "!")')
[[ "$out" == "k!" ]] || fail "expected 'k!', got '$out'"
echo "correctness ok"

# 2. Within-process memoization: force the first call (caching it), then a
#    second call with the same (namespace,key) but a different f must return the
#    cached value, proving the second f was never invoked.
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let a = builtins.tectonixMemo "ns" "k" (x: x + "!"); b = builtins.tectonixMemo "ns" "k" (x: "WRONG"); in builtins.seq a b')
[[ "$out" == "k!" ]] || fail "expected cached 'k!', got '$out'"
echo "memoization (same key, same eval) ok"

# 3. Distinct keys evaluate independently within one eval.
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let a = builtins.tectonixMemo "ns" "k" (x: x + "!"); b = builtins.tectonixMemo "ns" "k2" (x: x + "!"); in "${a}|${b}"')
[[ "$out" == "k!|k2!" ]] || fail "expected 'k!|k2!', got '$out'"
echo "distinct key ok"

# 4. Distinct namespaces are independent within one eval.
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let a = builtins.tectonixMemo "ns" "k" (x: x + "!"); b = builtins.tectonixMemo "ns2" "k" (x: x + "?"); in "${a}|${b}"')
[[ "$out" == "k!|k?" ]] || fail "expected 'k!|k?', got '$out'"
echo "distinct namespace ok"

# 5. Sharing: two consumers of the same memo entry get equal values.
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let f = x: x + "-shared"; a = builtins.tectonixMemo "share" "z" f; b = builtins.tectonixMemo "share" "z" f; in builtins.seq a (if a == b then a else "MISMATCH")')
[[ "$out" == "z-shared" ]] || fail "expected 'z-shared', got '$out'"
echo "sharing ok"

# 6. The memoized value can be an attrset and is shared structurally.
out=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let f = x: { path = x; tag = "node"; }; a = builtins.tectonixMemo "attrs" "//a" f; b = builtins.tectonixMemo "attrs" "//a" f; in builtins.seq a { a = a; sameRef = a == b; }')
echo "$out" | grepQuiet '"sameRef":true'
echo "$out" | grepQuiet '"path":"//a"'
echo "attrset sharing ok"

# 7. Circular self-reference throws a catchable error (not a crash).
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let f = k: builtins.tectonixMemo "cyc" k f; r = builtins.tryEval (builtins.tectonixMemo "cyc" "a" f); in if r.success then "NO-THROW" else "THREW"')
[[ "$out" == "THREW" ]] || fail "expected circular memo to throw, got '$out'"
echo "cycle detection ok"

# 8. Re-entrant miss for a DIFFERENT key (a dependency) works: zone-like
#    recursion through distinct keys resolves without a false cycle error.
out=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'let load = k: if k == "root" then builtins.tectonixMemo "dep" "child" load else k + "-val"; in builtins.tectonixMemo "dep" "root" load')
[[ "$out" == "child-val" ]] || fail "expected 'child-val', got '$out'"
echo "re-entrant distinct-key ok"

echo "tectonixMemo test passed!"
