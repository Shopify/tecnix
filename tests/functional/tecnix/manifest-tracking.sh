#!/usr/bin/env bash
# Per-entry manifest tracking and the memoized zone loader.
#
# A target that reads one zone's manifest entry (`tectonixManifestEntry`), the
# key set (`tectonixManifestKeys`) or one id (`tectonixManifestIdToPath`)
# depends on exactly that, not on the whole `.meta/manifest.json`. So adding an
# unrelated zone leaves such a target's eval-cache row valid. `tectonixMemo`
# shares one evaluation, and the source dependencies recorded during it, among
# every consumer.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

assert_jq() {
    local json="$1"
    local filter="$2"
    local message="$3"
    if ! jq -e "$filter" >/dev/null <<< "$json"; then
        echo "$json" >&2
        fail "$message"
    fi
}

WORLD="$TEST_ROOT/manifest-world"
createGitRepo "$WORLD"

write_manifest() {
    # $1: extra manifest lines, already comma-terminated
    cat > "$WORLD/.meta/manifest.json" <<EOF
{
$1
  "//zones/a": { "id": "W-000001" },
  "//zones/b": { "id": "W-000002" }
}
EOF
}

mkdir -p "$WORLD/.meta"
write_manifest ""
echo "shared one" > "$WORLD/shared.txt"
echo "unrelated one" > "$WORLD/unrelated.txt"

cat > "$WORLD/resolve.nix" <<'EOF'
args:
let
  drv = name: suffix: {
    drvPath = "/nix/store/00000000000000000000000000000000-${name}-${suffix}.drv";
    outputName = "out";
  };
  entry = name: builtins.tectonixManifestEntry "//zones/${name}";
  # Two consumers of one memoized load: the load reads shared.txt once.
  loaded = builtins.tectonixMemo "loader" "shared" (_: builtins.readFile ./shared.txt);
in
{
  allTargetNames = [ "a" "b" "keys" "id" "m1" "m2" "whole" ];
  resolve = name:
    if name == "whole" then drv name (toString (builtins.length (builtins.attrNames builtins.unsafeTectonixInternalManifest)))
    else if name == "keys" then drv name (toString (builtins.length (builtins.tectonixManifestKeys)))
    else if name == "id" then drv name (
      let path = builtins.tectonixManifestIdToPath "W-000001"; in if path == null then "absent" else path
    )
    else if name == "m1" || name == "m2" then drv name (builtins.hashString "sha256" loaded)
    else drv name (entry name).id;
}
EOF

commit_world() {
    git -C "$WORLD" add -A
    git -C "$WORLD" commit -q -m "$1"
    git -C "$WORLD" rev-parse HEAD
}

REV1=$(commit_world "two zones")

TARGETS='[ "a" "b" "keys" "id" "m1" "m2" "whole" ]'

# Dependency sets (target -> path -> fingerprint) of every target at $1.
# Extra nix flags follow.
deps_at() {
    local rev="$1"
    shift
    nix eval --json -v \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tectonix-git-dir "$WORLD/.git" \
        --option tectonix-git-sha "$rev" \
        "$@" \
        --expr "builtins.listToAttrs (map (r: { name = r.target; value = r.dependencies; }) (builtins.tecnixTargets {
          gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; args = { system = \"test-system\"; };
          rev = \"$rev\"; targets = $TARGETS; includeDependencies = true; includeTargets = false; }))"
}

nocache_deps_at() {
    deps_at "$1" --option tecnix-eval-cache false
}

CACHE_HOME="$TEST_ROOT/manifest-cache-home"
cached_deps_at() {
    XDG_CACHE_HOME="$CACHE_HOME" deps_at "$1" --option tecnix-eval-cache true --pure-eval
}

# `hits <err-file> <target...>` asserts each target was served from the cache;
# `misses` asserts each was re-evaluated.
hits() {
    local err="$1"; shift
    for t in "$@"; do
        grepQuiet "dependency cache hit for '$t'" "$err" || fail "$t should be a cache hit ($err)"
    done
}
misses() {
    local err="$1"; shift
    for t in "$@"; do
        grepQuiet "dependency cache miss, evaluating '$t'" "$err" || fail "$t should be re-evaluated ($err)"
    done
}

echo "Testing what each target depends on..."
deps1=$(nocache_deps_at "$REV1")
assert_jq "$deps1" '.a | keys == [".meta/manifest.json#//zones/a", "resolve.nix"]' \
    "a should depend on its own manifest entry, not the whole manifest"
assert_jq "$deps1" '.b | keys == [".meta/manifest.json#//zones/b", "resolve.nix"]' \
    "b should depend on its own manifest entry, not the whole manifest"
assert_jq "$deps1" '.keys | keys == [".meta/manifest.json#keys", "resolve.nix"]' \
    "keys should depend on the key set only"
assert_jq "$deps1" '.id | keys == [".meta/manifest.json#id/W-000001", "resolve.nix"]' \
    "id should depend on one id lookup only"
assert_jq "$deps1" '[.a, .b, .keys, .id, .m1, .m2 | has(".meta/manifest.json")] | any | not' \
    "the per-entry targets should not depend on the whole manifest file"
assert_jq "$deps1" '.whole | has(".meta/manifest.json")' \
    "reading the whole manifest (builtins.unsafeTectonixInternalManifest) should still depend on the file"
assert_jq "$deps1" '.a[".meta/manifest.json#//zones/a"] != .b[".meta/manifest.json#//zones/b"]' \
    "entry fingerprints should differ between zones"
assert_jq "$deps1" '.m1 == .m2 and (.m1 | has("shared.txt"))' \
    "both consumers of a memoized load should inherit the load's dependencies"

echo "Testing the eval cache across commits..."
cached_deps_at "$REV1" > "$TEST_ROOT/cold.json" 2> "$TEST_ROOT/cold.err"
misses "$TEST_ROOT/cold.err" a b keys id m1 m2 whole
[[ "$(jq -S . < "$TEST_ROOT/cold.json")" == "$(jq -S . <<< "$deps1")" ]] \
    || fail "the cached evaluation should report the same dependencies"

# Adding an unrelated zone changes the manifest file and the key set. Only the
# targets that enumerate the keys or read the whole file are invalidated.
write_manifest '  "//zones/c": { "id": "W-000003" },'
REV2=$(commit_world "add an unrelated zone")
cached_deps_at "$REV2" > /dev/null 2> "$TEST_ROOT/add-zone.err"
hits "$TEST_ROOT/add-zone.err" a b id m1 m2
misses "$TEST_ROOT/add-zone.err" keys whole

# Changing one zone's entry invalidates that zone's target and nothing else.
sed -i.bak 's/W-000002/W-000009/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV3=$(commit_world "change b's id")
cached_deps_at "$REV3" > "$TEST_ROOT/change-b.json" 2> "$TEST_ROOT/change-b.err"
hits "$TEST_ROOT/change-b.err" a id keys m1 m2
misses "$TEST_ROOT/change-b.err" b whole
assert_jq "$(cat "$TEST_ROOT/change-b.json")" \
    ".b[\".meta/manifest.json#//zones/b\"] != $(jq '.b[".meta/manifest.json#//zones/b"]' <<< "$deps1")" \
    "b's recorded entry fingerprint should follow its entry"

# An id lookup is invalidated when the id moves, and only then.
sed -i.bak 's/W-000001/W-000010/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV4=$(commit_world "renumber a")
cached_deps_at "$REV4" > /dev/null 2> "$TEST_ROOT/renumber-a.err"
hits "$TEST_ROOT/renumber-a.err" b keys m1 m2
misses "$TEST_ROOT/renumber-a.err" a id whole

# A memoized load's dependencies reach every consumer: changing what the load
# read invalidates both, not just whichever consumer evaluated it first.
echo "shared two" > "$WORLD/shared.txt"
REV5=$(commit_world "change the shared file")
cached_deps_at "$REV5" > /dev/null 2> "$TEST_ROOT/shared.err"
hits "$TEST_ROOT/shared.err" a b id keys whole
misses "$TEST_ROOT/shared.err" m1 m2

# A file nothing read leaves every row valid.
echo "unrelated two" > "$WORLD/unrelated.txt"
REV6=$(commit_world "change an unrelated file")
cached_deps_at "$REV6" > /dev/null 2> "$TEST_ROOT/unrelated.err"
hits "$TEST_ROOT/unrelated.err" a b keys id m1 m2 whole

echo "All manifest tracking tests passed!"
