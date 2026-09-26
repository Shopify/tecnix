#!/usr/bin/env bash
# World inputs functional test: eval a derivation using a World input,
# build it with a provider, and verify the output. Also verify that a
# second build with an unchanged oid is a no-op (cache hit).
#
# Runs on macOS (sandbox off). Linux sandbox testing commands are in
# the report.

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

# -- Tests 7-9 exercise the builder-side provider, which materializes views
#    at the canonical `/nix/var/tectonix/world/<oid>`. Without a sandbox that
#    remaps a host view root onto the canonical path, that directory must be
#    host-writable, i.e. this needs Linux + root (or a chroot store on the
#    lab). Skip cleanly elsewhere; see the report for the Linux commands.
if [[ "$(uname -s)" != "Linux" ]] || ! mkdir -p /nix/var/tectonix/world 2>/dev/null; then
    echo "SKIP: builder-side tests (7-9) require Linux with a writable /nix/var/tectonix/world (root, or a chroot store). Eval-side tests 1-6 passed."
    echo "All World inputs tests passed! (builder-side skipped on this platform)"
    exit 0
fi

# -- Test 7: Build with provider --
echo "Testing build with provider..."

# Create a provider script that materializes the tree using git objects
PROVIDER="$TEST_ROOT/provider.sh"
cat > "$PROVIDER" << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
oid="$1"; dst="$2"; git_dir="${WORLD_INPUTS_GIT_DIR:?}"
tmp="${dst}.tmp.$$"; mkdir -p "$tmp"
git -C "$git_dir" ls-tree -r --full-tree -z "$oid" | while IFS=$'\t' read -r -d '' meta path; do
    mode=$(printf '%s' "$meta" | awk '{print $1}'); type=$(printf '%s' "$meta" | awk '{print $2}'); blob=$(printf '%s' "$meta" | awk '{print $3}')
    mkdir -p "$tmp/$(dirname "$path")"
    case "$mode" in
        120000) ln -s "$(git -C "$git_dir" cat-file blob "$blob")" "$tmp/$path" ;;
        160000) ;;  # gitlink: skip
        100755) git -C "$git_dir" cat-file blob "$blob" > "$tmp/$path"; chmod +x "$tmp/$path" ;;
        *) git -C "$git_dir" cat-file blob "$blob" > "$tmp/$path" ;;
    esac
done
chmod -R a-w "$tmp" 2>/dev/null || true
mv "$tmp" "$dst"
EOF
chmod +x "$PROVIDER"

# Use a scratch store to avoid the system daemon (which lacks our builder changes)
SCRATCH_STORE="$TEST_ROOT/store"
mkdir -p "$SCRATCH_STORE"

BUILD_OUT=$(nix build "$DRV_PATH" \
    --extra-experimental-features 'nix-command world-inputs' \
    --option world-inputs-provider "$PROVIDER" \
    --store "local?root=$SCRATCH_STORE" \
    --print-out-paths --no-link 2>&1) || {
    echo "Build failed: $BUILD_OUT"
    fail "Build with provider failed"
}

OUT_PATH=$(echo "$BUILD_OUT" | tail -1)
echo "Build output: $OUT_PATH"
[[ -f "$OUT_PATH" ]] || fail "Output file missing"
echo "Output:"; cat "$OUT_PATH"

grep -q "zone.nix" "$OUT_PATH" || fail "Output should contain zone.nix"
grep -q "README.md" "$OUT_PATH" || fail "Output should contain README.md"
echo "PASS: build output contains expected view files"

# -- Test 8: Second build is a no-op (cache hit) --
SECOND=$(nix build "$DRV_PATH" \
    --extra-experimental-features 'nix-command world-inputs' \
    --option world-inputs-provider "$PROVIDER" \
    --store "local?root=$SCRATCH_STORE" \
    --print-out-paths --no-link 2>&1) || fail "Second build failed"
SECOND_OUT=$(echo "$SECOND" | tail -1)
[[ "$OUT_PATH" == "$SECOND_OUT" ]] || fail "Cache hit should give same path"
echo "PASS: second build is a no-op"

# -- Test 9: Build fails without provider --
echo "Testing build without provider..."
FAIL_OUT=$(nix build "$DRV_PATH" \
    --extra-experimental-features 'nix-command world-inputs' \
    --store "local?root=$SCRATCH_STORE" \
    --print-out-paths --no-link 2>&1 || true)
echo "$FAIL_OUT" | grep -qi "world-inputs-provider" \
    && echo "PASS: build fails without provider" \
    || fail "Build without provider should mention world-inputs-provider: $FAIL_OUT"

echo "All World inputs tests passed!"
