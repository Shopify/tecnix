#!/usr/bin/env bash
# builtins.tecnixPersistentMemo: `f key`, kept across evaluations inside cached
# `builtins.tecnixTargets` evaluation, and reused only when its stored source
# closure (what computing it read, plus the Nix code read by then) still
# matches the tree and the store paths in its string contexts are valid.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

WORLD="$TEST_ROOT/memo-world"
createGitRepo "$WORLD"
mkdir -p "$WORLD/.meta"
echo '{ "//zones/a": { "id": "W-000001" } }' > "$WORLD/.meta/manifest.json"

echo "inside one" > "$WORLD/inside.txt"
echo "outside one" > "$WORLD/outside.txt"
echo "a" > "$WORLD/flag.txt"

# The memoized function's code lives in its own file, imported before the
# call: editing it must invalidate the row though `f` never reads the file
# inside the memo.
cat > "$WORLD/lib.nix" <<'EOF'
{
  record = k: builtins.trace "computing ${k}" {
    inherit k;
    inside = builtins.readFile ./inside.txt;
    n = 1;
    f = 1.5;
    yes = true;
    nothing = null;
    list = [ "a" 2 { b = 3; } ];
    # A store path with context: a derivation instantiated inside the memo.
    drv = (derivation { name = "memo-drv"; system = "x"; builder = "/bin/sh"; }).drvPath;
  };
  notData = k: builtins.trace "computing fn ${k}" { fn = x: x; };
}
EOF

cat > "$WORLD/resolve.nix" <<'EOF'
args:
let
  lib = import ./lib.nix;
  drv = name: suffix: {
    drvPath = "/nix/store/00000000000000000000000000000000-${name}-${builtins.hashString "sha256" suffix}.drv";
    outputName = "out";
  };
  # A function whose code nothing labels: the resolver file was read before
  # this target's evaluation began, and the lambda reads nothing. Only the
  # stored Nix files make an edit here invalidate its row.
  inline = k: builtins.trace "computing inline ${k}" { m = 1; };
  # Functions chosen by reading a data file, forced before the memo call: the
  # read labels the attribute set, not the functions in it.
  picked =
    let fns = v: { compute = k: { inherit v; }; project = x: x; load = x: x; }; in
    if builtins.readFile ./flag.txt == "a\n" then fns "a" else fns "b";
  # The projecting form: the value holds a function; what is stored is data.
  projecting = {
    compute = k: builtins.trace "computing proj ${k}" { n = 1; f = x: x + 1; };
    project = v: { inherit (v) n; };
    load = d: builtins.trace "loading proj" { inherit (d) n; f = x: x + 1; };
  };
in
{
  allTargetNames = [ "t" "fn" "inline" "picked" "proj" ];
  resolve = name:
    let
      # Read outside the memo, so editing it re-evaluates the target but
      # leaves the memo row valid.
      outside = builtins.readFile ./outside.txt;
    in
    if name == "t" then
      let
        r = builtins.tecnixPersistentMemo "records" "a" lib.record;
        shared = builtins.tecnixPersistentMemo "records" "a" lib.record;
        rendered = builtins.toJSON {
          inherit (r) k inside n f yes nothing list;
          ctx = builtins.attrNames (builtins.getContext r.drv);
          same = r == shared;
        };
      in
      builtins.trace "result ${rendered}" (drv name (outside + rendered))
    else if name == "picked" then
      let r = builtins.seq picked (builtins.tecnixPersistentMemo "picked" "a" picked); in
      builtins.trace "picked ${r.v}" (drv name (outside + r.v))
    else if name == "proj" then
      let r = builtins.tecnixPersistentMemo "proj" "a" projecting; in
      builtins.trace "proj ${toString (r.f r.n)}" (drv name (outside + toString r.n))
    else if name == "inline" then
      let r = builtins.tecnixPersistentMemo "inline" "a" inline; in
      builtins.trace "inline ${toString r.m}" (drv name (outside + toString r.m))
    else
      let r = builtins.tecnixPersistentMemo "fns" "a" lib.notData; in
      builtins.seq (r.fn 1) (drv name outside);
}
EOF

commit_world() {
    git -C "$WORLD" add -A
    git -C "$WORLD" commit -q -m "$1"
    git -C "$WORLD" rev-parse HEAD
}

CACHE_HOME="$TEST_ROOT/memo-cache-home"
DEFAULT_ARGS='args = { system = "test-system"; };'

# Evaluate targets $2 at rev $1 with the cache on; stderr goes to $3. $4, if
# given, replaces `args` (and may add `memoArgs`).
eval_at() {
    local rev="$1" targets="$2" err="$3" argsAttrs="${4:-$DEFAULT_ARGS}"
    XDG_CACHE_HOME="$CACHE_HOME" nix eval --json -v \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tectonix-git-dir "$WORLD/.git" \
        --option tectonix-git-sha "$rev" \
        --option tecnix-eval-cache true --pure-eval \
        --expr "builtins.length (builtins.tecnixTargets {
          gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; $argsAttrs
          rev = \"$rev\"; targets = $targets; includeDependencies = true; includeTargets = false; })" \
        > /dev/null 2> "$err"
}

computed() {
    grep -c "trace: computing $2\$" "$1" || true
}

result_of() {
    grep "trace: result " "$1" | sed 's/.*trace: result //'
}

echo "First evaluation: the memo misses, computes once, and stores the row"
REV1=$(commit_world "one")
eval_at "$REV1" '[ "t" ]' "$TEST_ROOT/memo1.err"
[[ "$(computed "$TEST_ROOT/memo1.err" a)" == 1 ]] || fail "f should run once ($(cat "$TEST_ROOT/memo1.err"))"
grepQuiet "tecnixPersistentMemo: miss for 'records' / 'a' in 't'" "$TEST_ROOT/memo1.err" || fail "expected a memo miss"
result1=$(result_of "$TEST_ROOT/memo1.err")
jq -e '.inside == "inside one\n" and .n == 1 and .f == 1.5 and .yes == true and .nothing == null
      and .list == ["a", 2, {"b": 3}] and (.ctx | length) == 1 and .same == true' <<< "$result1" >/dev/null \
    || fail "unexpected first result: $result1"

echo "A read outside the memo changes: the target re-evaluates, the memo hits"
echo "outside two" > "$WORLD/outside.txt"
REV2=$(commit_world "outside")
eval_at "$REV2" '[ "t" ]' "$TEST_ROOT/memo2.err"
grepQuiet "dependency cache miss, evaluating 't'" "$TEST_ROOT/memo2.err" || fail "t should be re-evaluated"
grepQuiet "tecnixPersistentMemo: hit for 'records' / 'a' in 't'" "$TEST_ROOT/memo2.err" || fail "expected a memo hit ($(cat "$TEST_ROOT/memo2.err"))"
[[ "$(computed "$TEST_ROOT/memo2.err" a)" == 0 ]] || fail "a memo hit must not run f"
[[ "$(result_of "$TEST_ROOT/memo2.err")" == "$result1" ]] || fail "a hit should give the stored value, with its context"

echo "A file the memo read changes: the memo misses and sees the new content"
echo "inside two" > "$WORLD/inside.txt"
REV3=$(commit_world "inside")
eval_at "$REV3" '[ "t" ]' "$TEST_ROOT/memo3.err"
[[ "$(computed "$TEST_ROOT/memo3.err" a)" == 1 ]] || fail "f should run again after its input changed"
jq -e '.inside == "inside two\n"' <<< "$(result_of "$TEST_ROOT/memo3.err")" >/dev/null || fail "stale memo value"

echo "The memoized function's code changes: the memo misses"
sed -i.bak 's/n = 1;/n = 2;/' "$WORLD/lib.nix" && rm "$WORLD/lib.nix.bak"
REV4=$(commit_world "code")
eval_at "$REV4" '[ "t" ]' "$TEST_ROOT/memo4.err"
[[ "$(computed "$TEST_ROOT/memo4.err" a)" == 1 ]] || fail "a code change must invalidate the memo"
jq -e '.n == 2' <<< "$(result_of "$TEST_ROOT/memo4.err")" >/dev/null || fail "stale memo value after a code change"

echo "A stored value whose context names a store path that is gone is a miss"
echo "outside three" > "$WORLD/outside.txt"
REV5=$(commit_world "outside again")
drvPath=$(jq -r '.ctx[0]' <<< "$(result_of "$TEST_ROOT/memo4.err")")
[[ -e "$drvPath" ]] || fail "the memo's derivation should be in the store"
nix-store --delete "$drvPath" >/dev/null 2>&1 || fail "could not delete $drvPath"
eval_at "$REV5" '[ "t" ]' "$TEST_ROOT/memo5.err"
[[ "$(computed "$TEST_ROOT/memo5.err" a)" == 1 ]] || fail "a missing context path must make the row unusable"
[[ -e "$drvPath" ]] || fail "re-evaluating should instantiate the derivation again"

echo "A result that is not data is returned but never stored"
eval_at "$REV5" '[ "fn" ]' "$TEST_ROOT/memo6.err"
grepQuiet "tecnixPersistentMemo: not storing" "$TEST_ROOT/memo6.err" || fail "a function result should not be stored"
echo "outside four" > "$WORLD/outside.txt"
REV6=$(commit_world "outside four")
eval_at "$REV6" '[ "fn" ]' "$TEST_ROOT/memo7.err"
[[ "$(computed "$TEST_ROOT/memo7.err" "fn a")" == 1 ]] || fail "an unstored result is computed every time"

echo "An edit to code nothing labels (the resolver's own lambda) still misses"
eval_at "$REV6" '[ "inline" ]' "$TEST_ROOT/memo8.err"
[[ "$(computed "$TEST_ROOT/memo8.err" "inline a")" == 1 ]] || fail "the inline memo should compute once"
echo "outside five" > "$WORLD/outside.txt"
REV7=$(commit_world "outside five")
eval_at "$REV7" '[ "inline" ]' "$TEST_ROOT/memo9.err"
[[ "$(computed "$TEST_ROOT/memo9.err" "inline a")" == 0 ]] || fail "an unrelated edit should leave the inline memo valid"
sed -i.bak 's/m = 1;/m = 2;/' "$WORLD/resolve.nix" && rm "$WORLD/resolve.nix.bak"
REV8=$(commit_world "inline code")
eval_at "$REV8" '[ "inline" ]' "$TEST_ROOT/memo10.err"
grepQuiet "trace: inline 2" "$TEST_ROOT/memo10.err" || fail "an edit to the memoized lambda's code served a stale row ($(cat "$TEST_ROOT/memo10.err"))"

echo "A function forced before the call still counts what it read"
REV9=$REV8
eval_at "$REV9" '[ "picked" ]' "$TEST_ROOT/memo11.err"
grepQuiet "trace: picked a" "$TEST_ROOT/memo11.err" || fail "unexpected picked value"
echo "b" > "$WORLD/flag.txt"
REV10=$(commit_world "flag")
eval_at "$REV10" '[ "picked" ]' "$TEST_ROOT/memo12.err"
grepQuiet "trace: picked b" "$TEST_ROOT/memo12.err" || fail "a change to what chose the function served a stale row ($(cat "$TEST_ROOT/memo12.err"))"

echo "The projecting form stores project's data and returns load's value on a hit"
eval_at "$REV10" '[ "proj" ]' "$TEST_ROOT/memo13.err"
[[ "$(computed "$TEST_ROOT/memo13.err" "proj a")" == 1 ]] || fail "proj should be computed on a miss"
grepQuiet "trace: proj 2" "$TEST_ROOT/memo13.err" || fail "a miss returns compute's value"
! grepQuiet "trace: loading proj" "$TEST_ROOT/memo13.err" || fail "a miss should not call load"
echo "outside six" > "$WORLD/outside.txt"
REV11=$(commit_world "outside six")
eval_at "$REV11" '[ "proj" ]' "$TEST_ROOT/memo14.err"
[[ "$(computed "$TEST_ROOT/memo14.err" "proj a")" == 0 ]] || fail "proj should hit"
grepQuiet "trace: loading proj" "$TEST_ROOT/memo14.err" || fail "a hit returns load's value"
grepQuiet "trace: proj 2" "$TEST_ROOT/memo14.err" || fail "load's value should behave like compute's"
out=$(nix eval --raw --extra-experimental-features 'nix-command' \
    --expr 'toString (builtins.tecnixPersistentMemo "ns" "k" { compute = k: 3; project = throw "no"; load = throw "no"; })')
[[ "$out" == "3" ]] || fail "outside cached evaluation the projecting form is compute key, got '$out'"

echo "memoArgs: rows are shared across args that differ outside it, and only then"
eval_at "$REV11" '[ "t" ]' "$TEST_ROOT/memo15.err"
eval_at "$REV11" '[ "t" ]' "$TEST_ROOT/memo16.err" \
    'args = { system = "test-system"; state = "x"; }; memoArgs = { system = "test-system"; };'
grepQuiet "dependency cache miss, evaluating 't'" "$TEST_ROOT/memo16.err" || fail "other args should miss the target row"
grepQuiet "tecnixPersistentMemo: hit for 'records' / 'a' in 't'" "$TEST_ROOT/memo16.err" \
    || fail "equal memoArgs should share the memo row ($(cat "$TEST_ROOT/memo16.err"))"
eval_at "$REV11" '[ "t" ]' "$TEST_ROOT/memo17.err" 'args = { system = "test-system"; state = "y"; };'
grepQuiet "tecnixPersistentMemo: miss for 'records' / 'a' in 't'" "$TEST_ROOT/memo17.err" \
    || fail "without memoArgs, other args should not share memo rows"

echo "Outside cached target evaluation it is just f key"
out=$(nix eval --raw --extra-experimental-features 'nix-command' \
    --expr 'builtins.tecnixPersistentMemo "ns" "k" (k: k + "!")')
[[ "$out" == "k!" ]] || fail "expected 'k!', got '$out'"
