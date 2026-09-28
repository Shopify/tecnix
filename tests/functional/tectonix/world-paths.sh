#!/usr/bin/env bash
# tectonix-world-input-paths: World paths interpolated into strings, and builtins.path over World
# paths (filtered or not), become World inputs instead of store copies. The eval side runs
# everywhere; the output guard needs the Linux builder (as in world-inputs.sh).

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TEST_WORLD="$TEST_ROOT/world-paths"
rm -rf "$TEST_WORLD"
git init -q "$TEST_WORLD"
cd "$TEST_WORLD"
mkdir -p .meta system/tectonix areas/tools/dev/src/target areas/tools/dev/docs
echo '{ "//areas/tools/dev": { "id": "W-000001" } }' > .meta/manifest.json
cat > system/tectonix/resolve.nix <<'EOF'
{ system, shell }:
let
  targets = import ../../areas/tools/dev/targets.nix { inherit system shell; };
in
{
  resolve = target: targets.${builtins.elemAt (builtins.match "//areas/tools/dev:(.*)" target) 0};
  allTargetNames = map (n: "//areas/tools/dev:${n}") (builtins.attrNames targets);
}
EOF
cat > areas/tools/dev/targets.nix <<'EOF'
{ system, shell }:
let
  mk = name: script: derivation { inherit name system; builder = shell; args = [ "-c" script ]; };
  filtered = builtins.path {
    path = ./.;
    name = "src";
    filter = p: t: baseNameOf p != "junk.txt" && !(t == "directory" && baseNameOf p == "target");
  };
  again = builtins.path { path = filtered; name = "again"; filter = p: t: baseNameOf p != "docs"; };
in
{
  dir = { s = "${./.}"; };
  file = { s = "${./README.md}"; };
  subdir = { s = "${./src}"; };
  filtered = {
    s = filtered;
    names = builtins.attrNames (builtins.readDir filtered);
    readme = builtins.readFile "${filtered}/README.md";
  };
  again = { s = again; names = builtins.attrNames (builtins.readDir again); };
  copy = { drvPath = (mk "copy" "read -r l < ${./src}/main.txt; echo \"$l\" > $out").drvPath; };
  leak = { drvPath = (mk "leak" "echo ${./README.md} > $out").drvPath; };
}
EOF
echo "hello" > areas/tools/dev/README.md
echo "junk" > areas/tools/dev/junk.txt
echo "main" > areas/tools/dev/src/main.txt
echo "built" > areas/tools/dev/src/target/out.txt
echo "doc" > areas/tools/dev/docs/guide.md
git add -A && git -c user.name=t -c user.email=t@t commit -q -m init
HEAD_SHA=$(git rev-parse HEAD)
cd - >/dev/null

SYSTEM=$(nix eval --raw --impure --extra-experimental-features nix-command --expr builtins.currentSystem)
targetsExpr() { # <extra tecnixTargets args>
    echo "let names = [ \"dir\" \"file\" \"subdir\" \"filtered\" \"again\" \"copy\" \"leak\" ];
      t = builtins.tecnixTargets {
        gitDir = \"$TEST_WORLD/.git\"; resolver = \"system/tectonix/resolve.nix\"; rev = \"$HEAD_SHA\";
        args = { system = \"$SYSTEM\"; shell = \"$SHELL\"; };
        targets = map (n: \"//areas/tools/dev:\${n}\") names; $1 };
    in builtins.listToAttrs (map (n: { name = n; value = t.\"//areas/tools/dev:\${n}\"; }) names)"
}
evalTargets() { # <world-input-paths true|false> [extra args]
    nix eval --json --extra-experimental-features 'nix-command world-inputs' \
        --option lazy-trees true --option tecnix-eval-cache false \
        --option tectonix-world-input-paths "$1" --expr "$(targetsExpr "${2:-}")"
}
wrap() { # <mode> <type> <oid> <name>: the one-entry tree a World input names
    printf '%s %s %s\t%s\n' "$1" "$2" "$3" "$4" | git -C "$TEST_WORLD" mktree
}
treeOf() { git -C "$TEST_WORLD" rev-parse "HEAD:$1"; }

# -- Interpolation and builtins.path become World inputs --
ON=$(evalTargets true)
V=/nix/var/tectonix/world
[[ $(jq -r .dir.s <<< "$ON") == "$V/$(wrap 040000 tree "$(treeOf areas/tools/dev)" dev)/dev" ]] \
    || fail "\${./.} should be the World input of the zone directory under its name: $(jq -r .dir.s <<< "$ON")"
[[ $(jq -r .file.s <<< "$ON") == "$V/$(wrap 100644 blob "$(treeOf areas/tools/dev/README.md)" README.md)/README.md" ]] \
    || fail "\${./README.md} should be the World input of the file under its name: $(jq -r .file.s <<< "$ON")"
[[ $(jq -r .subdir.s <<< "$ON") == "$V/$(wrap 040000 tree "$(treeOf areas/tools/dev/src)" src)/src" ]] \
    || fail "\${./src} should be the World input of the subdirectory: $(jq -r .subdir.s <<< "$ON")"
echo "PASS: interpolated World paths are World inputs named like their store copies"

assert_jq() { jq -e "$2" <<< "$1" >/dev/null || fail "$3: $(jq -c . <<< "$1")"; }
assert_jq "$ON" '.filtered.names == ["README.md", "docs", "src", "targets.nix"] and .filtered.readme == "hello\n"' \
    "builtins.path with a filter should drop junk.txt and src/target, and be readable at eval time"
FILTERED=$(jq -r .filtered.s <<< "$ON")
[[ $FILTERED == $V/*/src ]] || fail "a filtered builtins.path should end in its name: $FILTERED"
W=$(basename "$(dirname "$FILTERED")")
git -C "$TEST_WORLD" cat-file -e "$W^{tree}" || fail "the filtered tree should be written to the World repository"
[[ "$(git -C "$TEST_WORLD" ls-tree -r --name-only "$W" | LC_ALL=C sort)" == "$(printf 'src/README.md\nsrc/docs/guide.md\nsrc/src/main.txt\nsrc/targets.nix')" ]] \
    || fail "the filtered tree should hold exactly the kept files: $(git -C "$TEST_WORLD" ls-tree -r --name-only "$W")"
assert_jq "$ON" '.again.names == ["README.md", "src", "targets.nix"]' \
    "builtins.path over a World input view should filter it again"
echo "PASS: builtins.path with a filter is a World input for the filtered tree (also over a view)"

DRV=$(jq -r .copy.drvPath <<< "$ON")
grep -q '__worldInputs' "$DRV" || fail "a derivation using an interpolated World path should declare __worldInputs"
grep -qF "$(wrap 040000 tree "$(treeOf areas/tools/dev/src)" src)" "$DRV" || fail "__worldInputs should name the wrapped oid"
grep -q '//areas/tools/dev/src' "$DRV" || fail "__worldInputs should name the World path"
if ls "$NIX_STORE_DIR" | grep -qE -- '-(dev|src|README\.md)$'; then
    fail "nothing should be copied into the store: $(ls "$NIX_STORE_DIR" | grep -E -- '-(dev|src|README\.md)$')"
fi
echo "PASS: derivations declare the World inputs, and no source reaches the store"

DEPS=$(nix eval --json --extra-experimental-features 'nix-command world-inputs' \
    --option lazy-trees true --option tecnix-eval-cache false --option tectonix-world-input-paths true \
    --expr "map (r: builtins.attrNames r.dependencies) (builtins.tecnixTargets {
      gitDir = \"$TEST_WORLD/.git\"; resolver = \"system/tectonix/resolve.nix\"; rev = \"$HEAD_SHA\";
      args = { system = \"$SYSTEM\"; shell = \"$SHELL\"; };
      targets = [ \"//areas/tools/dev:copy\" ]; includeDependencies = true; includeTargets = false; })")
jq -e '.[0] | index("areas/tools/dev/src") != null' <<< "$DEPS" >/dev/null \
    || fail "the target should still depend on the interpolated directory: $DEPS"
echo "PASS: the source dependency is still recorded"

# -- Off by default: the same paths are copied into the store --
OFF=$(evalTargets false)
[[ $(jq -r .dir.s <<< "$OFF") == "$NIX_STORE_DIR"/*-dev ]] || fail "with the setting off, \${./.} should be a store copy"
[[ $(jq -r .filtered.s <<< "$OFF") == "$NIX_STORE_DIR"/*-src ]] || fail "with the setting off, builtins.path should copy"
echo "PASS: tectonix-world-input-paths off keeps store copies"

# -- Dirty content is not committed World content: it is copied --
echo "changed" > "$TEST_WORLD/areas/tools/dev/README.md"
DIRTY=$(evalTargets true "checkoutPath = \"$TEST_WORLD\";")
[[ $(jq -r .file.s <<< "$DIRTY") == "$NIX_STORE_DIR"/*-README.md ]] || fail "a dirty file should be copied: $(jq -r .file.s <<< "$DIRTY")"
[[ $(jq -r .dir.s <<< "$DIRTY") == "$NIX_STORE_DIR"/*-dev ]] || fail "a directory with dirty content should be copied"
[[ $(jq -r .subdir.s <<< "$DIRTY") == $V/*/src ]] || fail "a clean subdirectory of a dirty zone should still be a World input"
git -C "$TEST_WORLD" checkout -q -- areas/tools/dev/README.md
echo "PASS: dirty content falls back to a store copy; clean parts stay World inputs"

# -- Builder side: outputs must not refer to a view (Linux sandbox, as in world-inputs.sh) --
if [[ "$(uname -s)" != "Linux" ]] || ! canUseSandbox || [[ ! $SHELL =~ /nix/store ]]; then
    echo "SKIP: output guard test needs the Linux sandbox and a \$SHELL from /nix/store."
    echo "All World paths tests passed! (builder side skipped on this platform)"
    exit 0
fi
requiresUnprivilegedUserNamespaces

PROVIDER="$TEST_ROOT/wp-provider.sh"
echo "#!$SHELL" > "$PROVIDER"
cat >> "$PROVIDER" << 'P'
set -euo pipefail
oid="$1"; dst="$2"; git_dir="${WORLD_INPUTS_GIT_DIR:?}"
tmp="${dst}.tmp.$$"; mkdir -p "$tmp"
git -C "$git_dir" ls-tree -r --full-tree -z "$oid" | while IFS=$'\t' read -r -d '' meta path; do
    read -r mode _ blob <<< "$meta"
    mkdir -p "$tmp/$(dirname "$path")"
    git -C "$git_dir" cat-file blob "$blob" > "$tmp/$path"
    if [[ $mode == 100755 ]]; then chmod +x "$tmp/$path"; fi
done
chmod -R a-w "$tmp"; mv "$tmp" "$dst"
P
chmod +x "$PROVIDER"
export WORLD_INPUTS_GIT_DIR="$TEST_WORLD/.git"
chmod -R u+w "$TEST_ROOT/wp-store" 2>/dev/null || true
rm -rf "$TEST_ROOT/wp-store" "$TEST_ROOT/wp-views"
export NIX_STORE_DIR=/my/store NIX_REMOTE="$TEST_ROOT/wp-store"
buildOpts=(
    --extra-experimental-features 'nix-command world-inputs'
    --option sandbox true --option sandbox-paths /nix/store
    --option system-features world-inputs
    --option tectonix-world-view-root "$TEST_ROOT/wp-views"
    --option world-inputs-provider "$PROVIDER"
)
ON=$(evalTargets true)
OUT=$(nix build "$(jq -r .copy.drvPath <<< "$ON")^out" "${buildOpts[@]}" --print-out-paths --no-link)
[[ "$(nix store cat "$OUT")" == "main" ]] || fail "a build should read the interpolated World path from its view"
echo "PASS: a build reads an interpolated World path from its view"
LEAK=$(nix build "$(jq -r .leak.drvPath <<< "$ON")^out" "${buildOpts[@]}" --no-link 2>&1 || true)
grepQuiet "refers to a World input view" <<< "$LEAK" || fail "an output naming a view should be rejected: $LEAK"
echo "PASS: an output that refers to a view is rejected"

echo "All World paths tests passed!"
