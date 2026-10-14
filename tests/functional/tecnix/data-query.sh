#!/usr/bin/env bash
# `builtins.tecnixQueryFile` and the memoized zone loader.
#
# A target that queries a data file depends on the answers it saw, not on the
# file: its closure entry for the file is a `query:` fingerprint listing one
# observation per distinct query. So adding an unrelated zone to the manifest, or
# reformatting a TOML file, leaves such a target's eval-cache row valid, while
# a target that read the file whole still depends on the whole file. A function
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

WORLD="$TEST_ROOT/query-world"
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

write_config() {
    # $1: the jobs value; $2: a comment line
    cat > "$WORLD/config.toml" <<EOF
# $2
[build]
jobs = $1
EOF
}

mkdir -p "$WORLD/.meta"
write_manifest ""
write_config 4 "build settings"
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
  manifest = q: builtins.tecnixQueryFile ({ path = ./.meta/manifest.json; } // q);
  config = q: builtins.tecnixQueryFile ({ path = ./config.toml; } // q);
  entry = name: manifest { op = "get"; at = [ "//zones/${name}" ]; };
  zonePaths = manifest { op = "keys"; at = [ ]; };
  pathsForId = id: manifest { op = "find"; at = [ ]; where = "id"; value = id; };
  # Two consumers of one memoized load: the load reads shared.txt once.
  loaded = builtins.tecnixMemoize (_: builtins.readFile ./shared.txt) "shared";
  # Memoized lookups that every consumer calls afresh, so every consumer after
  # the first is a memo hit rather than a force of one shared thunk.
  zoneId = builtins.tecnixMemoize (name: (entry name).id);
  zoneRecord = builtins.tecnixMemoize (name: { id = (entry name).id; });
in
{
  allTargetNames = [
    "a" "b" "keys" "id" "both" "m1" "m2" "whole" "za1" "za2" "not-a-zone" "missing"
    "toml-get" "toml-has" "toml-whole"
    "peek" "fill-record" "prefilled" "record-field" "agree" "key1" "key2"
  ];
  resolve = name:
    if name == "whole" then drv name (toString (builtins.length (builtins.attrNames (builtins.fromJSON (builtins.readFile ./.meta/manifest.json)))))
    else if name == "keys" then drv name (toString (builtins.length zonePaths))
    else if name == "id" then drv name (
      let paths = pathsForId "W-000001"; in if paths == [ ] then "absent" else builtins.head paths
    )
    # Two queries on one file: one closure entry, two observations.
    else if name == "both" then drv name "${(entry "a").id}-${if manifest { op = "has"; at = [ "//zones/b" ]; } then "b" else "no-b"}"
    else if name == "m1" || name == "m2" then drv name (builtins.hashString "sha256" loaded)
    else if name == "za1" || name == "za2" || name == "prefilled" then drv name (zoneId "a")
    # Hands both memoized lookups, unforced, to whoever reads the value.
    else if name == "peek" then drv name "peek" // { zoneId = zoneId "a"; recordId = (zoneRecord "a").id; }
    # Fills the memoized record without forcing its field.
    else if name == "fill-record" then builtins.seq (zoneRecord "a") (drv name "record")
    else if name == "record-field" then drv name (zoneRecord "a").id
    # What the query builtin reports about zone a, beside what reading
    # .meta/manifest.json itself shows.
    else if name == "agree" then
      let
        manifest' = builtins.fromJSON (builtins.readFile ./.meta/manifest.json);
        id = manifest'."//zones/a".id;
      in drv name "agree" // {
        seen = {
          builtins = { inherit (entry "a") id; keys = zonePaths; paths = pathsForId id; };
          file = { inherit id; keys = builtins.attrNames manifest'; paths = [ "//zones/a" ]; };
        };
      }
    # Any key can be asked about; there is nothing special about "keys".
    else if name == "not-a-zone" then drv name (if manifest { op = "get"; at = [ "keys" ]; } == null then "none" else "some")
    # A file that does not exist answers like an empty one, and the answer is recorded.
    else if name == "missing" then drv name (if builtins.tecnixQueryFile { path = ./optional.json; op = "has"; at = [ ]; } then "present" else "absent")
    else if name == "toml-get" then drv name (toString (config { op = "get"; at = [ "build" "jobs" ]; }))
    else if name == "toml-has" then drv name (if config { op = "has"; at = [ "build" "cache" ]; } then "cached" else "uncached")
    else if name == "toml-whole" then drv name (builtins.hashString "sha256" (builtins.readFile ./config.toml))
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

TARGETS='[ "a" "b" "keys" "id" "both" "m1" "m2" "whole" "za1" "za2" "not-a-zone" "missing" "toml-get" "toml-has" "toml-whole" ]'
ALL_TARGETS=(a b keys id both m1 m2 whole za1 za2 not-a-zone missing toml-get toml-has toml-whole)

# Dependency sets (target -> path -> fingerprint) of every target at $1.
# Extra nix flags follow. With WITH_CHECKOUT set, the checkout's uncommitted
# changes overlay the rev.
deps_at() {
    local rev="$1"
    shift
    local checkout=""
    [[ -n "${WITH_CHECKOUT-}" ]] && checkout="checkoutPath = \"$WORLD\";"
    nix eval --json -v \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tectonix-git-dir "$WORLD/.git" \
        --option tectonix-git-sha "$rev" \
        "$@" \
        --expr "builtins.listToAttrs (map (r: { name = r.target; value = r.dependencies; }) (builtins.tecnixTargets {
          gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; args = { system = \"test-system\"; }; $checkout
          rev = \"$rev\"; targets = $TARGETS; includeDependencies = true; includeTargets = false; }))"
}

nocache_deps_at() {
    deps_at "$1" --option tecnix-eval-cache false
}

CACHE_HOME="$TEST_ROOT/query-cache-home"
cached_deps_at() {
    XDG_CACHE_HOME="$CACHE_HOME" deps_at "$1" --option tecnix-eval-cache true --pure-eval
}

# `hits <err-file> <target...>` asserts each target was served from the cache;
# `misses` asserts each was re-evaluated. `hits_except <err-file> <miss...>`
# asserts the given targets missed and every other target hit.
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
hits_except() {
    local err="$1"; shift
    misses "$err" "$@"
    local t m
    for t in "${ALL_TARGETS[@]}"; do
        for m in "$@"; do
            [[ "$t" == "$m" ]] && continue 2
        done
        hits "$err" "$t"
    done
}

echo "Testing what each target depends on..."
deps1=$(nocache_deps_at "$REV1")
assert_jq "$deps1" '.a | keys == [".meta/manifest.json", "resolve.nix"]' \
    "a should depend on the manifest (by query) and the resolver"
assert_jq "$deps1" '.a[".meta/manifest.json"] == "query:json\tget\t//zones/a\t{\"id\":\"W-000001\"}"' \
    "a's manifest entry should record its own query and answer"
assert_jq "$deps1" '.b[".meta/manifest.json"] == "query:json\tget\t//zones/b\t{\"id\":\"W-000002\"}"' \
    "b's manifest entry should record its own query and answer"
assert_jq "$deps1" '.keys[".meta/manifest.json"] == "query:json\tkeys\t[\"//zones/a\",\"//zones/b\"]"' \
    "keys should record the key set"
assert_jq "$deps1" '.id[".meta/manifest.json"] == "query:json\tfind\tid\tW-000001\t[\"//zones/a\"]"' \
    "id should record the id lookup"
assert_jq "$deps1" '.both[".meta/manifest.json"] == "query:json\tget\t//zones/a\t{\"id\":\"W-000001\"}\njson\thas\t//zones/b\ttrue"' \
    "two queries on one file should be two sorted observations in one entry"
assert_jq "$deps1" '."not-a-zone"[".meta/manifest.json"] == "query:json\tget\tkeys\tabsent"' \
    "an absent key should be recorded absent"
assert_jq "$deps1" '.missing | keys == ["optional.json", "resolve.nix"] and .["optional.json"] == "query:json\thas\tfalse"' \
    "a query on a missing file should record its answer"
assert_jq "$deps1" '."toml-get"["config.toml"] == "query:toml\tget\tbuild\tjobs\t4"' \
    "a TOML query should record its answer"
assert_jq "$deps1" '."toml-has"["config.toml"] == "query:toml\thas\tbuild\tcache\tfalse"' \
    "a TOML existence query should record its answer"
assert_jq "$deps1" '[.whole[".meta/manifest.json"], ."toml-whole"["config.toml"]] | all(startswith("git:"))' \
    "reading a file whole should still depend on the whole file"
assert_jq "$deps1" '.m1 == .m2 and (.m1 | has("shared.txt"))' \
    "both consumers of a memoized load should inherit the load's dependencies"
assert_jq "$deps1" '[.za1, .za2 | .[".meta/manifest.json"] == "query:json\tget\t//zones/a\t{\"id\":\"W-000001\"}"] | all' \
    "a memo hit should replay the query recorded when its entry was filled"
# How a caller computed a memo key is its own dependency, never the shared
# entry's. Either target may fill the entry first, so check both.
via=$(TARGETS='[ "key1" "key2" ]' nocache_deps_at "$REV1")
assert_jq "$via" '(.key1 | keys) == [".meta/manifest.json", "key1.txt", "resolve.nix"]
    and (.key2 | keys) == [".meta/manifest.json", "key2.txt", "resolve.nix"]' \
    "a memo caller should depend on the file it read the key from, and on no other caller's"

echo "Testing argument validation..."
bad_query() {
    # $1: the query attrset; expects an error mentioning $2
    local out
    if out=$(nix eval --json --impure --extra-experimental-features nix-command \
        --expr "builtins.tecnixQueryFile ($1)" 2>&1); then
        fail "expected $1 to fail"
    fi
    grepQuiet "$2" <<< "$out" || { echo "$out" >&2; fail "error for $1 should mention $2"; }
}
bad_query "{ path = $WORLD/config.toml; op = \"select\"; at = [ ]; }" "unknown op 'select'"
bad_query "{ path = $WORLD/config.toml; op = \"get\"; }" "missing attribute 'at'"
bad_query "{ path = $WORLD/config.toml; op = \"get\"; at = [ ]; where = \"x\"; }" "only for op 'find'"
bad_query "{ path = $WORLD/config.toml; op = \"find\"; at = [ ]; }" "requires 'where' and 'value'"
bad_query "{ path = $WORLD/config.toml; op = \"get\"; at = [ \"a\tb\" ]; }" "may not contain a tab"
bad_query "{ path = $WORLD/config.toml; op = \"get\"; at = [ ]; format = \"yaml\"; }" "unknown format 'yaml'"
bad_query "{ path = $WORLD/shared.txt; op = \"get\"; at = [ ]; }" "cannot infer the format"
bad_query "{ path = $WORLD/config.toml; op = \"get\"; at = [ ]; extra = 1; }" "unexpected attribute 'extra'"
bad_query "{ path = $WORLD/config.toml; op = \"get\"; at = [ ]; format = \"json\"; }" "while parsing"

echo "Testing the builtin outside tracking..."
# Outside the repo accessor a query reads like `readFile`: a path pure
# evaluation forbids is an error, not an absent file.
expectStderr 1 nix eval --json --extra-experimental-features nix-command \
    --expr "builtins.tecnixQueryFile { path = $WORLD/config.toml; op = \"has\"; at = [ ]; }" \
    | grepQuiet "forbidden in pure evaluation mode"
plain=$(nix eval --json --impure --extra-experimental-features nix-command --expr "{
  entry = builtins.tecnixQueryFile { path = $WORLD/.meta/manifest.json; op = \"get\"; at = [ \"//zones/a\" ]; };
  keys = builtins.tecnixQueryFile { path = $WORLD/.meta/manifest.json; op = \"keys\"; at = [ ]; };
  found = builtins.tecnixQueryFile { path = $WORLD/.meta/manifest.json; op = \"find\"; at = [ ]; where = \"id\"; value = \"W-000002\"; };
  jobs = builtins.tecnixQueryFile { path = $WORLD/config.toml; op = \"get\"; at = [ \"build\" \"jobs\" ]; };
  table = builtins.tecnixQueryFile { path = \"$WORLD/config.toml\"; op = \"get\"; at = [ \"build\" ]; };
  missing = builtins.tecnixQueryFile { path = $WORLD/optional.json; op = \"get\"; at = [ \"x\" ]; };
}")
assert_jq "$plain" '. == { entry: { id: "W-000001" }, keys: ["//zones/a", "//zones/b"], found: ["//zones/b"], jobs: 4, table: { jobs: 4 }, missing: null }' \
    "the builtin should answer plain queries like fromJSON (readFile ...) would"

echo "Testing the eval cache across commits..."
cached_deps_at "$REV1" > "$TEST_ROOT/cold.json" 2> "$TEST_ROOT/cold.err"
misses "$TEST_ROOT/cold.err" "${ALL_TARGETS[@]}"
[[ "$(jq -S . < "$TEST_ROOT/cold.json")" == "$(jq -S . <<< "$deps1")" ]] \
    || fail "the cached evaluation should report the same dependencies"

# Adding an unrelated zone changes the manifest file and the key set. Only the
# targets that enumerate the keys or read the whole file are invalidated.
write_manifest '  "//zones/c": { "id": "W-000003" },'
REV2=$(commit_world "add an unrelated zone")
cached_deps_at "$REV2" > /dev/null 2> "$TEST_ROOT/add-zone.err"
hits_except "$TEST_ROOT/add-zone.err" keys whole

# Changing one zone's entry invalidates that zone's target and nothing else:
# `both` only asked whether b exists, and it still does.
sed -i.bak 's/W-000002/W-000009/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV3=$(commit_world "change b's id")
cached_deps_at "$REV3" > "$TEST_ROOT/change-b.json" 2> "$TEST_ROOT/change-b.err"
hits_except "$TEST_ROOT/change-b.err" b whole
assert_jq "$(cat "$TEST_ROOT/change-b.json")" '.b[".meta/manifest.json"] == "query:json\tget\t//zones/b\t{\"id\":\"W-000009\"}"' \
    "b's recorded observation should follow its entry"

# An id lookup is invalidated when the id moves, and only then.
sed -i.bak 's/W-000001/W-000010/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV4=$(commit_world "renumber a")
cached_deps_at "$REV4" > /dev/null 2> "$TEST_ROOT/renumber-a.err"
hits_except "$TEST_ROOT/renumber-a.err" a id both whole za1 za2

# A memoized load's dependencies reach every consumer: changing what the load
# read invalidates both, not just whichever consumer evaluated it first.
echo "shared two" > "$WORLD/shared.txt"
REV5=$(commit_world "change the shared file")
cached_deps_at "$REV5" > /dev/null 2> "$TEST_ROOT/shared.err"
hits_except "$TEST_ROOT/shared.err" m1 m2

# A file nothing read leaves every row valid.
echo "unrelated two" > "$WORLD/unrelated.txt"
REV6=$(commit_world "change an unrelated file")
cached_deps_at "$REV6" > /dev/null 2> "$TEST_ROOT/unrelated.err"
hits_except "$TEST_ROOT/unrelated.err"

# Reformatting a data file changes its object but no answer.
write_config 4 "build settings, now with a longer comment"
REV7=$(commit_world "comment in config.toml")
cached_deps_at "$REV7" > /dev/null 2> "$TEST_ROOT/toml-comment.err"
hits_except "$TEST_ROOT/toml-comment.err" toml-whole

# Changing a queried value invalidates exactly the query that saw it.
write_config 8 "build settings, now with a longer comment"
REV8=$(commit_world "more jobs")
cached_deps_at "$REV8" > /dev/null 2> "$TEST_ROOT/toml-value.err"
hits_except "$TEST_ROOT/toml-value.err" toml-get toml-whole

# A file whose absence was observed invalidates its observer when it appears.
echo '{}' > "$WORLD/optional.json"
REV9=$(commit_world "add the optional file")
cached_deps_at "$REV9" > "$TEST_ROOT/optional.json" 2> "$TEST_ROOT/optional.err"
hits_except "$TEST_ROOT/optional.err" missing
assert_jq "$(cat "$TEST_ROOT/optional.json")" '.missing["optional.json"] == "query:json\thas\ttrue"' \
    "the observer should now record the file's presence"

# A query is answered from the checkout's uncommitted content, and the
# observation follows it, like any dirty read.
write_config 16 "uncommitted"
WITH_CHECKOUT=1 cached_deps_at "$REV9" > "$TEST_ROOT/dirty.json" 2> "$TEST_ROOT/dirty.err"
hits_except "$TEST_ROOT/dirty.err" toml-get toml-whole
assert_jq "$(cat "$TEST_ROOT/dirty.json")" '."toml-get"["config.toml"] == "query:toml\tget\tbuild\tjobs\t16"' \
    "a dirty data file should be queried as it is on disk"
git -C "$WORLD" checkout -q -- config.toml

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
PREFILLED_DEPS='[.prefilled, ."record-field" | .[".meta/manifest.json"] == "query:json\tget\t//zones/a\t{\"id\":\"W-000010\"}"] | all'
# First with tracking from includeDependencies alone, then through the eval
# cache, where a missing dependency turns into a stale warm hit.
assert_jq "$(prefill_deps_at "$REV9" --option tecnix-eval-cache false)" "$PREFILLED_DEPS" \
    "tracked targets should depend on the query behind memoized values first touched outside tracking"

PREFILL_CACHE_HOME="$TEST_ROOT/query-prefill-cache-home"
XDG_CACHE_HOME="$PREFILL_CACHE_HOME" prefill_deps_at "$REV9" --option tecnix-eval-cache true --pure-eval \
    > "$TEST_ROOT/prefill-cold.json" 2> "$TEST_ROOT/prefill-cold.err"
misses "$TEST_ROOT/prefill-cold.err" prefilled record-field
assert_jq "$(cat "$TEST_ROOT/prefill-cold.json")" "$PREFILLED_DEPS" \
    "cached tracked targets should depend on the query behind memoized values first touched outside tracking"

# The reviewer's reproduction: renumber zone a, and the warm run must
# re-evaluate instead of serving the old id.
sed -i.bak 's/W-000010/W-000011/' "$WORLD/.meta/manifest.json" && rm "$WORLD/.meta/manifest.json.bak"
REV10=$(commit_world "renumber a again")
XDG_CACHE_HOME="$PREFILL_CACHE_HOME" prefill_deps_at "$REV10" --option tecnix-eval-cache true --pure-eval \
    > /dev/null 2> "$TEST_ROOT/prefill-warm.err"
misses "$TEST_ROOT/prefill-warm.err" prefilled record-field

# The builtin reads the file through the repo accessor, like every file the
# resolver reads and like the validator re-checking its observations. So evaluating
# an older rev from a checkout whose HEAD has moved on shows it that rev's
# manifest, not the working tree's.
echo "Testing the builtin at an older rev of a checkout..."
seen=$(nix eval --json \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --option tecnix-eval-cache false \
    --expr "(builtins.tecnixTargets { gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; args = { system = \"test-system\"; };
      rev = \"$REV1\"; checkoutPath = \"$WORLD\"; targets = [ \"agree\" ]; }).agree.seen")
assert_jq "$seen" '.file.id == "W-000001"' "the resolver should read the evaluated rev's manifest"
assert_jq "$seen" '.builtins == .file' "the builtin should see the manifest the resolver reads"

echo "All data query tests passed!"
