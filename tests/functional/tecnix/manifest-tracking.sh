#!/usr/bin/env bash
# Per-entry manifest tracking and the memoized zone loader.
#
# A target that reads one zone's manifest entry (`tectonixManifestEntry`), the
# key set (`tectonixManifestKeys`) or one id (`tectonixManifestIdToPath`)
# depends on exactly that, not on the whole `.meta/manifest.json`. So adding an
# unrelated zone leaves such a target's eval-cache row valid. A function
# memoized with `tecnixMemoize` shares one evaluation, and the source
# dependencies recorded during it, among every consumer, but never shares a
# result computed outside tracking with a tracked target (or the other way
# round).

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
# Two files naming zone a, one memo key read from each by its own target.
printf a > "$WORLD/key1.txt"
printf a > "$WORLD/key2.txt"

cat > "$WORLD/resolve.nix" <<'EOF'
args:
let
  drv = name: suffix: {
    drvPath = "/nix/store/00000000000000000000000000000000-${name}-${suffix}.drv";
    outputName = "out";
  };
  entry = name: builtins.tectonixManifestEntry "//zones/${name}";
  # Two consumers of one memoized load: the load reads shared.txt once.
  loaded = builtins.tecnixMemoize (_: builtins.readFile ./shared.txt) "shared";
  # Memoized lookups that every consumer calls afresh, so every consumer after
  # the first is a memo hit rather than a force of one shared thunk.
  zoneId = builtins.tecnixMemoize (name: (entry name).id);
  zoneRecord = builtins.tecnixMemoize (name: { id = (entry name).id; });
in
{
  allTargetNames = [ "a" "b" "keys" "id" "m1" "m2" "whole" "za1" "za2" "peek" "fill-record" "prefilled" "record-field" "agree" "not-a-zone" "key1" "key2" ];
  resolve = name:
    if name == "whole" then drv name (toString (builtins.length (builtins.attrNames builtins.unsafeTectonixInternalManifest)))
    else if name == "keys" then drv name (toString (builtins.length (builtins.tectonixManifestKeys)))
    else if name == "id" then drv name (
      let path = builtins.tectonixManifestIdToPath "W-000001"; in if path == null then "absent" else path
    )
    else if name == "m1" || name == "m2" then drv name (builtins.hashString "sha256" loaded)
    else if name == "za1" || name == "za2" || name == "prefilled" then drv name (zoneId "a")
    # Hands both memoized lookups, unforced, to whoever reads the value.
    else if name == "peek" then drv name "peek" // { zoneId = zoneId "a"; recordId = (zoneRecord "a").id; }
    # Fills the memoized record without forcing its field.
    else if name == "fill-record" then builtins.seq (zoneRecord "a") (drv name "record")
    else if name == "record-field" then drv name (zoneRecord "a").id
    # What the manifest builtins report about zone a, beside what reading
    # .meta/manifest.json itself shows.
    else if name == "agree" then
      let
        manifest = builtins.fromJSON (builtins.readFile ./.meta/manifest.json);
        id = manifest."//zones/a".id;
      in drv name "agree" // {
        seen = {
          builtins = { inherit (entry "a") id; keys = builtins.tectonixManifestKeys; path = builtins.tectonixManifestIdToPath id; };
          file = { inherit id; keys = builtins.attrNames manifest; path = "//zones/a"; };
        };
      }
    # Every manifest key starts with `//`; "keys" is not one.
    else if name == "not-a-zone" then drv name (if builtins.tectonixManifestEntry "keys" == null then "none" else "some")
    else if name == "key1" || name == "key2" then drv name (zoneId (builtins.readFile ./${name}.txt))
    else drv name (entry name).id;
}
EOF

commit_world() {
    git -C "$WORLD" add -A
    git -C "$WORLD" commit -q -m "$1"
    git -C "$WORLD" rev-parse HEAD
}

REV1=$(commit_world "two zones")

TARGETS='[ "a" "b" "keys" "id" "m1" "m2" "whole" "za1" "za2" ]'

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
assert_jq "$deps1" '[.a, .b, .keys, .id, .m1, .m2, .za1, .za2 | has(".meta/manifest.json")] | any | not' \
    "the per-entry targets should not depend on the whole manifest file"
assert_jq "$deps1" '.whole | has(".meta/manifest.json")' \
    "reading the whole manifest (builtins.unsafeTectonixInternalManifest) should still depend on the file"
assert_jq "$deps1" '.a[".meta/manifest.json#//zones/a"] != .b[".meta/manifest.json#//zones/b"]' \
    "entry fingerprints should differ between zones"
assert_jq "$deps1" '.m1 == .m2 and (.m1 | has("shared.txt"))' \
    "both consumers of a memoized load should inherit the load's dependencies"
assert_jq "$deps1" '[.za1, .za2 | keys == [".meta/manifest.json#//zones/a", "resolve.nix"]] | all' \
    "a memo hit should replay the dependencies recorded when its entry was filled"
# An argument that is not a zone path could spell another key form (here
# `#keys`, the key set), so it records the whole manifest instead.
not_a_zone=$(TARGETS='[ "not-a-zone" ]' nocache_deps_at "$REV1")
assert_jq "$not_a_zone" '."not-a-zone" | keys == [".meta/manifest.json", "resolve.nix"]' \
    "looking up a non-zone path should depend on the whole manifest"
# How a caller computed a memo key is its own dependency, never the shared
# entry's. Either target may fill the entry first, so check both.
via=$(TARGETS='[ "key1" "key2" ]' nocache_deps_at "$REV1")
assert_jq "$via" '(.key1 | keys) == [".meta/manifest.json#//zones/a", "key1.txt", "resolve.nix"]
    and (.key2 | keys) == [".meta/manifest.json#//zones/a", "key2.txt", "resolve.nix"]' \
    "a memo caller should depend on the file it read the key from, and on no other caller's"

echo "Testing the eval cache across commits..."
cached_deps_at "$REV1" > "$TEST_ROOT/cold.json" 2> "$TEST_ROOT/cold.err"
misses "$TEST_ROOT/cold.err" a b keys id m1 m2 whole za1 za2
[[ "$(jq -S . < "$TEST_ROOT/cold.json")" == "$(jq -S . <<< "$deps1")" ]] \
    || fail "the cached evaluation should report the same dependencies"

# Adding an unrelated zone changes the manifest file and the key set. Only the
# targets that enumerate the keys or read the whole file are invalidated.
write_manifest '  "//zones/c": { "id": "W-000003" },'
REV2=$(commit_world "add an unrelated zone")
cached_deps_at "$REV2" > /dev/null 2> "$TEST_ROOT/add-zone.err"
hits "$TEST_ROOT/add-zone.err" a b id m1 m2 za1 za2
misses "$TEST_ROOT/add-zone.err" keys whole

# Changing one zone's entry invalidates that zone's target and nothing else.
sed -i.bak 's/W-000002/W-000009/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV3=$(commit_world "change b's id")
cached_deps_at "$REV3" > "$TEST_ROOT/change-b.json" 2> "$TEST_ROOT/change-b.err"
hits "$TEST_ROOT/change-b.err" a id keys m1 m2 za1 za2
misses "$TEST_ROOT/change-b.err" b whole
assert_jq "$(cat "$TEST_ROOT/change-b.json")" \
    ".b[\".meta/manifest.json#//zones/b\"] != $(jq '.b[".meta/manifest.json#//zones/b"]' <<< "$deps1")" \
    "b's recorded entry fingerprint should follow its entry"

# An id lookup is invalidated when the id moves, and only then.
sed -i.bak 's/W-000001/W-000010/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV4=$(commit_world "renumber a")
cached_deps_at "$REV4" > /dev/null 2> "$TEST_ROOT/renumber-a.err"
hits "$TEST_ROOT/renumber-a.err" b keys m1 m2
misses "$TEST_ROOT/renumber-a.err" a id whole za1 za2

# A memoized load's dependencies reach every consumer: changing what the load
# read invalidates both, not just whichever consumer evaluated it first.
echo "shared two" > "$WORLD/shared.txt"
REV5=$(commit_world "change the shared file")
cached_deps_at "$REV5" > /dev/null 2> "$TEST_ROOT/shared.err"
hits "$TEST_ROOT/shared.err" a b id keys whole za1 za2
misses "$TEST_ROOT/shared.err" m1 m2

# A file nothing read leaves every row valid.
echo "unrelated two" > "$WORLD/unrelated.txt"
REV6=$(commit_world "change an unrelated file")
cached_deps_at "$REV6" > /dev/null 2> "$TEST_ROOT/unrelated.err"
hits "$TEST_ROOT/unrelated.err" a b keys id m1 m2 whole za1 za2

# A memoized result computed outside tracking must never reach a tracked
# target without its dependencies, in either direction: an untracked call that
# comes first, and an untracked call that forces part of an entry a tracked
# target filled. `peek` hands both lookups, unforced, to the top-level
# expression, which forces them outside tracking before the tracked targets
# run in the same evaluation. Extra nix flags follow the rev.
prefill_deps_at() {
    local rev="$1"
    shift
    nix eval --json -v \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tectonix-git-dir "$WORLD/.git" \
        --option tectonix-git-sha "$rev" \
        "$@" \
        --expr "let
          call = includeTargets: targets: builtins.tecnixTargets {
            gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; args = { system = \"test-system\"; };
            rev = \"$rev\"; inherit targets includeTargets; includeDependencies = true; };
          peek = (builtins.head (call true [ \"peek\" ])).value;
          outsideTracking = builtins.seq peek.zoneId (builtins.seq (call false [ \"fill-record\" ]) peek.recordId);
        in builtins.seq outsideTracking (builtins.listToAttrs (map (r: { name = r.target; value = r.dependencies; })
          (call false [ \"prefilled\" \"record-field\" ])))"
}

echo "Testing memoized results computed outside tracking..."
PREFILLED_DEPS='[.prefilled, ."record-field" | keys == [".meta/manifest.json#//zones/a", "resolve.nix"]] | all'
# First with tracking from includeDependencies alone, then through the eval
# cache, where a missing dependency turns into a stale warm hit.
assert_jq "$(prefill_deps_at "$REV6" --option tecnix-eval-cache false)" "$PREFILLED_DEPS" \
    "tracked targets should depend on the entry behind memoized values first touched outside tracking"

PREFILL_CACHE_HOME="$TEST_ROOT/manifest-prefill-cache-home"
XDG_CACHE_HOME="$PREFILL_CACHE_HOME" prefill_deps_at "$REV6" --option tecnix-eval-cache true --pure-eval \
    > "$TEST_ROOT/prefill-cold.json" 2> "$TEST_ROOT/prefill-cold.err"
misses "$TEST_ROOT/prefill-cold.err" prefilled record-field
assert_jq "$(cat "$TEST_ROOT/prefill-cold.json")" "$PREFILLED_DEPS" \
    "cached tracked targets should depend on the entry behind memoized values first touched outside tracking"

# The reviewer's reproduction: renumber zone a, and the warm run must
# re-evaluate instead of serving the old id.
sed -i.bak 's/W-000010/W-000011/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV7=$(commit_world "renumber a again")
XDG_CACHE_HOME="$PREFILL_CACHE_HOME" prefill_deps_at "$REV7" --option tecnix-eval-cache true --pure-eval \
    > /dev/null 2> "$TEST_ROOT/prefill-warm.err"
misses "$TEST_ROOT/prefill-warm.err" prefilled record-field

# The manifest builtins read .meta/manifest.json through the repo accessor,
# like every file the resolver reads and like the fingerprints of their
# dependency keys. So evaluating an older rev from a checkout whose HEAD has
# moved on shows them that rev's manifest, not the working tree's.
echo "Testing the manifest builtins at an older rev of a checkout..."
seen=$(nix eval --json \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --option tecnix-eval-cache false \
    --expr "(builtins.tecnixTargets { gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; args = { system = \"test-system\"; };
      rev = \"$REV1\"; checkoutPath = \"$WORLD\"; targets = [ \"agree\" ]; }).agree.seen")
assert_jq "$seen" '.file.id == "W-000001"' "the resolver should read the evaluated rev's manifest"
assert_jq "$seen" '.builtins == .file' "the manifest builtins should see the manifest the resolver reads"

echo "All manifest tracking tests passed!"
