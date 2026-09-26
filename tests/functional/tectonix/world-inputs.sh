#!/usr/bin/env bash
# World inputs functional test: eval a derivation using a World input,
# build it with a provider, and verify the output. Also verify that a
# second build with an unchanged oid is a no-op (cache hit).
#
# The eval side (tests 1-6) runs everywhere; the builder side (7-10)
# runs in a Linux sandbox with a chroot store, no root needed.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Create test world
TEST_WORLD="$TEST_ROOT/world"
create_test_world "$TEST_WORLD"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

# Common nix eval options for World inputs
evalOpts=(
    --no-pure-eval
    --extra-experimental-features 'nix-command world-inputs'
    --option tectonix-git-dir "$TEST_WORLD/.git"
    --option tectonix-git-sha "$HEAD_SHA"
)

# -- Test 1: builtins.tectonixWorldInput returns the view path --
TREE_OID=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalTreeSha "//areas/tools/dev"')
echo "Tree oid: $TREE_OID"

VIEW_PATH=$(nix eval --raw "${evalOpts[@]}" \
    --expr 'builtins.tectonixWorldInput "//areas/tools/dev"')
echo "World input path: $VIEW_PATH"
EXPECTED="/nix/var/tectonix/world/$TREE_OID"
[[ "$VIEW_PATH" == "$EXPECTED" ]] || fail "Expected $EXPECTED, got $VIEW_PATH"
echo "PASS: tectonixWorldInput returns correct view path"

# -- Test 2: derivationStrict collects __worldInputs and adds world-inputs feature --
evalExpr='
  let src = builtins.tectonixWorldInput "//areas/tools/dev";
  in (derivation {
    name = "wi-test";
    builder = "/bin/sh";
    system = builtins.currentSystem;
    args = [ "-c" "find ${src} -type f | sort > $out" ];
  })
'

DRV_PATH=$(nix eval --raw "${evalOpts[@]}" --expr "($evalExpr).drvPath")
echo "drvPath: $DRV_PATH"

# Check __worldInputs in the drv (raw ATerm contains it)
grep -q "__worldInputs" "$DRV_PATH" || fail "__worldInputs not in drv env"
echo "PASS: __worldInputs in drv env"

# Check world-inputs in requiredSystemFeatures
grep -q "world-inputs" "$DRV_PATH" || fail "world-inputs not in requiredSystemFeatures"
echo "PASS: world-inputs in requiredSystemFeatures"

# Check no source store path in inputSrcs (the zone source should NOT be there)
# The drv should not reference any /nix/store/*-source path for the zone
if grep -q "areas-tools-dev" "$DRV_PATH"; then
    fail "drv should not contain zone source store path"
fi
echo "PASS: no zone source store path in drv"

# -- Test 3: drvPath changes when oid changes --
echo "new content" > "$TEST_WORLD/areas/tools/dev/NEW_FILE.txt"
git -C "$TEST_WORLD" add -A && git -C "$TEST_WORLD" commit -m "Add file" --quiet
NEW_SHA=$(get_head_sha "$TEST_WORLD")

NEW_DRV=$(nix eval --raw \
    --no-pure-eval \
    --extra-experimental-features 'nix-command world-inputs' \
    --option tectonix-git-dir "$TEST_WORLD/.git" \
    --option tectonix-git-sha "$NEW_SHA" \
    --expr "($evalExpr).drvPath")

[[ "$DRV_PATH" != "$NEW_DRV" ]] || fail "drvPath should change when oid changes"
echo "PASS: drvPath changed when oid changed"

# -- Test 4: drvPath unchanged for unrelated commit --
git -C "$TEST_WORLD" reset --hard "$HEAD_SHA" --quiet
echo "unrelated" > "$TEST_WORLD/areas/platform/core/unrelated.txt"
git -C "$TEST_WORLD" add -A && git -C "$TEST_WORLD" commit -m "Unrelated" --quiet
UNREL_SHA=$(get_head_sha "$TEST_WORLD")

UNREL_DRV=$(nix eval --raw \
    --no-pure-eval \
    --extra-experimental-features 'nix-command world-inputs' \
    --option tectonix-git-dir "$TEST_WORLD/.git" \
    --option tectonix-git-sha "$UNREL_SHA" \
    --expr "($evalExpr).drvPath")

[[ "$DRV_PATH" == "$UNREL_DRV" ]] || fail "drvPath should not change for unrelated commit"
echo "PASS: drvPath unchanged for unrelated commit"

# Reset to original HEAD
git -C "$TEST_WORLD" reset --hard "$HEAD_SHA" --quiet

# -- Test 5: readFile of World input is refused --
echo "Testing readFile refusal..."
READFILE_OUT=$(nix eval --raw "${evalOpts[@]}" \
    --expr 'let src = builtins.tectonixWorldInput "//areas/tools/dev"; in builtins.readFile src' 2>&1 || true)
echo "$READFILE_OUT" | grep -qi "world" && echo "PASS: readFile refused with World message" \
    || fail "readFile should refuse World input with clear message: $READFILE_OUT"

# -- Test 6: toFile with World input is refused --
echo "Testing toFile refusal..."
TOFILE_OUT=$(nix eval --raw "${evalOpts[@]}" \
    --expr 'let src = builtins.tectonixWorldInput "//areas/tools/dev"; in builtins.toFile "test" src' 2>&1 || true)
echo "$TOFILE_OUT" | grep -qi "world" && echo "PASS: toFile refused with World message" \
    || fail "toFile should refuse World input: $TOFILE_OUT"

# -- Builder side (tests 7-10). The drv always names the canonical
#    `/nix/var/tectonix/world/<oid>`; the provider materializes the view under
#    `tectonix-world-view-root` on the host, and the Linux sandbox bind-mounts
#    it onto the canonical path. That needs no root: build in a chroot store
#    with the sandbox on (the same setup as linux-sandbox.sh). Elsewhere the
#    builder side is skipped and tests 1-6 stand.
if [[ "$(uname -s)" != "Linux" ]] || ! canUseSandbox || [[ ! $SHELL =~ /nix/store ]]; then
    echo "SKIP: builder-side tests (7-10) need the Linux sandbox and a \$SHELL from /nix/store. Eval-side tests 1-6 passed."
    echo "All World inputs tests passed! (builder-side skipped on this platform)"
    exit 0
fi
requiresUnprivilegedUserNamespaces

# Provider: <oid> <dst>. Writes the tree from git objects, read-only, with an
# atomic rename, and logs each call so tests can count them.
PROVIDER="$TEST_ROOT/provider.sh"
PROVIDER_LOG="$TEST_ROOT/provider-calls"
: > "$PROVIDER_LOG"
cat > "$PROVIDER" << 'P'
#!/usr/bin/env bash
set -euo pipefail
oid="$1"; dst="$2"; git_dir="${WORLD_INPUTS_GIT_DIR:?}"
echo "$oid" >> "${WORLD_INPUTS_PROVIDER_LOG:?}"
tmp="${dst}.tmp.$$"; mkdir -p "$tmp"
git -C "$git_dir" ls-tree -r --full-tree -z "$oid" | while IFS=$'\t' read -r -d '' meta path; do
    read -r mode _ blob <<< "$meta"
    mkdir -p "$tmp/$(dirname "$path")"
    case "$mode" in
        120000) ln -s "$(git -C "$git_dir" cat-file blob "$blob")" "$tmp/$path" ;;
        160000) ;;  # gitlink: skip
        100755) git -C "$git_dir" cat-file blob "$blob" > "$tmp/$path"; chmod +x "$tmp/$path" ;;
        *) git -C "$git_dir" cat-file blob "$blob" > "$tmp/$path" ;;
    esac
done
chmod -R a-w "$tmp"
mv "$tmp" "$dst"
P
chmod +x "$PROVIDER"
export WORLD_INPUTS_GIT_DIR="$TEST_WORLD/.git" WORLD_INPUTS_PROVIDER_LOG="$PROVIDER_LOG"

# Chroot store with a store dir other than /nix/store, so the host's
# /nix/store (and $SHELL in it) can be bind-mounted into the sandbox.
chmod -R u+w "$TEST_ROOT/store0" 2>/dev/null || true
rm -rf "$TEST_ROOT/store0" "$TEST_ROOT/views"
export NIX_STORE_DIR=/my/store NIX_REMOTE="$TEST_ROOT/store0"
VIEW_ROOT="$TEST_ROOT/views"

# A builder that lists the view's files with bash builtins only.
wiDrv() {
    nix eval --raw "${evalOpts[@]}" --expr '
      let src = builtins.tectonixWorldInput "//areas/tools/dev";
      in (derivation {
        name = "'"$1"'";
        builder = builtins.getEnv "SHELL";
        system = builtins.currentSystem;
        args = [ "-c" "shopt -s globstar dotglob; for f in ${src}/**; do [[ -f $f ]] && echo \"\${f#${src}/}\"; done > $out" ];
      }).drvPath'
}
buildOpts=(
    --extra-experimental-features 'nix-command world-inputs'
    --option sandbox true --option sandbox-paths /nix/store
    --option system-features world-inputs
    --option tectonix-world-view-root "$VIEW_ROOT"
)
calls() { wc -l < "$PROVIDER_LOG"; }

# -- Test 7: build with the provider in the sandbox --
DRV_A=$(wiDrv wi-a)
OUT_A=$(nix build "$DRV_A^out" "${buildOpts[@]}" --option world-inputs-provider "$PROVIDER" --print-out-paths --no-link)
LIST_A=$(nix store cat "$OUT_A")
grep -qx "zone.nix" <<< "$LIST_A" || fail "output should list zone.nix: $LIST_A"
grep -qx "README.md" <<< "$LIST_A" || fail "output should list README.md: $LIST_A"
[[ -d "$VIEW_ROOT/$TREE_OID" ]] || fail "view not materialized at $VIEW_ROOT/$TREE_OID"
[[ $(calls) -eq 1 ]] || fail "expected 1 provider call, got $(calls)"
[[ ! -w "$VIEW_ROOT/$TREE_OID" ]] || fail "view should be read-only"
echo "PASS: sandboxed build sees the view at the canonical path"

# -- Test 8: second build is a no-op: same output, no provider call --
OUT_A2=$(nix build "$DRV_A^out" "${buildOpts[@]}" --option world-inputs-provider "$PROVIDER" --print-out-paths --no-link)
[[ "$OUT_A" == "$OUT_A2" ]] || fail "second build should give the same path"
[[ $(calls) -eq 1 ]] || fail "second build should not call the provider (calls: $(calls))"
echo "PASS: second build is a no-op"

# -- Test 9: derivations sharing an oid, in one build and after the view
#    exists, all see it; the provider still ran only once for that oid --
DRV_B=$(wiDrv wi-b); DRV_C=$(wiDrv wi-c)
OUTS=$(nix build "$DRV_B^out" "$DRV_C^out" "${buildOpts[@]}" --option world-inputs-provider "$PROVIDER" --print-out-paths --no-link)
for o in $OUTS; do
    [[ "$(nix store cat "$o")" == "$LIST_A" ]] || fail "$o should list the same files as $OUT_A"
done
[[ $(calls) -eq 1 ]] || fail "an existing view should be reused, not re-provided (calls: $(calls))"
echo "PASS: an existing view is mapped into every derivation that declares it"

# -- Test 10: building without a provider fails with a clear message --
DRV_D=$(wiDrv wi-d)
FAIL_OUT=$(nix build "$DRV_D^out" "${buildOpts[@]}" --no-link 2>&1 || true)
echo "$FAIL_OUT" | grep -q "world-inputs-provider" \
    || fail "build without provider should mention world-inputs-provider: $FAIL_OUT"
echo "PASS: build fails without provider"

echo "All World inputs tests passed!"
