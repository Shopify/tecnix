#!/usr/bin/env bash
# Tests for the public Tecnix builtins: tecnixTargetNames and tecnixTargets.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TEST_WORLD="$TEST_ROOT/tecnix-world"
create_tecnix_builtin_test_world "$TEST_WORLD"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

TEST_WORLD_OTHER="$TEST_ROOT/tecnix-world-other"
create_tecnix_builtin_test_world "$TEST_WORLD_OTHER"
HEAD_SHA_OTHER=$(get_head_sha "$TEST_WORLD_OTHER")

tecnix_args() {
    cat <<EOF
{
  gitDir = "$TEST_WORLD/.git";
  resolver = "system/tectonix/resolve.nix";
  args = { system = "test-system"; };
  rev = "$HEAD_SHA";
  checkoutPath = "$TEST_WORLD";
}
EOF
}

tecnix_other_args() {
    cat <<EOF
{
  gitDir = "$TEST_WORLD_OTHER/.git";
  resolver = "system/tectonix/resolve.nix";
  args = { system = "test-system"; };
  rev = "$HEAD_SHA_OTHER";
  checkoutPath = "$TEST_WORLD_OTHER";
}
EOF
}

rewrite_tecnix_test_expr() {
    local expr="$1"
    cat <<EOF
let
  tecnixTargetDependencyPathSet = args:
    builtins.listToAttrs (map (record: {
      name = record.target;
      value = record.dependencies;
    }) (builtins.tecnixTargets (args // { includeDependencies = true; includeTargets = false; })));
  tecnixTargetNameDependencyPathSet = args:
    (builtins.tecnixTargetNames (args // { includeDependencies = true; })).dependencies;
in
  $expr
EOF
}

tecnix_eval_json_no_cache() {
    local expr
    expr=$(rewrite_tecnix_test_expr "$1")
    nix eval --json \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tecnix-eval-cache false \
        --expr "$expr"
}

tecnix_eval_json_parallel_no_cache() {
    local expr
    expr=$(rewrite_tecnix_test_expr "$1")
    nix eval --json \
        --extra-experimental-features 'nix-command parallel-eval' \
        --eval-cores 2 \
        --option lazy-trees true \
        --option tecnix-eval-cache false \
        --expr "$expr"
}

tecnix_eval_json_access_set_no_cache() {
    local expr
    expr=$(rewrite_tecnix_test_expr "$1")
    nix eval --json \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tecnix-eval-cache false \
        --expr "$expr"
}

tecnix_eval_json_with_settings() {
    local expr="$1"
    nix eval --json \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tectonix-git-dir "$TEST_WORLD/.git" \
        --option tectonix-git-sha "$HEAD_SHA" \
        --option tectonix-checkout-path "$TEST_WORLD" \
        --expr "$expr"
}

assert_jq() {
    local json="$1"
    local filter="$2"
    local message="$3"
    if ! jq -e "$filter" >/dev/null <<< "$json"; then
        echo "$json" >&2
        fail "$message"
    fi
}

assert_json_equal() {
    local actual="$1"
    local expected="$2"
    local message="$3"
    if ! diff -u <(jq -S . <<< "$expected") <(jq -S . <<< "$actual"); then
        fail "$message"
    fi
}

assert_target_dependency_paths() {
    local json="$1"
    local target="$2"
    local message="$3"
    local expected actual
    expected=$(cat)
    actual=$(jq -r --arg target "$target" '.[$target] | keys[]' <<< "$json")
    if ! diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"); then
        fail "$message"
    fi
}

assert_clean_dependency_paths() {
    local deps="$1"

    assert_json_equal "$(jq -c 'keys' <<< "$deps")" '["//areas/app/web:alpha","//areas/app/web:beta","//areas/app/web:closureChainA","//areas/app/web:closureChainB","//areas/app/web:closureMiddleUser","//areas/app/web:existsCheck","//areas/app/web:fileTypeCheck","//areas/app/web:nestedOptional","//areas/app/web:optional","//areas/app/web:readDirCheck","//areas/app/web:readFileCheck","//areas/app/web:readFileSharedA","//areas/app/web:readFileSharedB","//areas/app/web:resolverModuleUser","//areas/app/web:sharedExistsA","//areas/app/web:sharedExistsB","//areas/app/web:sharedReadDirA","//areas/app/web:sharedReadDirB","//areas/app/web:srcdir","//areas/app/web:symlinked","//areas/app/web:treeShaCheck","//areas/lib/shared:gamma"]' \
        "clean dependency output should have the exact expected target keys"

    assert_target_dependency_paths "$deps" "//areas/app/web:alpha" "alpha dependency paths should be exact" <<'EOF'
areas/app/web/common.nix
areas/app/web/targets.nix
areas/app/web/targets/alpha.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:beta" "beta dependency paths should be exact" <<'EOF'
areas/app/web/common.nix
areas/app/web/targets.nix
areas/app/web/targets/beta.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:srcdir" "srcdir dependency paths should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/srcdir.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:symlinked" "symlinked dependency paths should be exact" <<'EOF'
areas/app/web/common.nix
areas/app/web/targets.nix
areas/app/web/targets/alpha.nix
areas/app/web/targets/current.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:optional" "optional dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/optional-marker.nix
areas/app/web/targets/optional.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:existsCheck" "existsCheck dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/exists-check.nix
areas/app/web/targets/exists-marker
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:fileTypeCheck" "fileTypeCheck dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/exists-marker
areas/app/web/targets/file-type-check.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:readDirCheck" "readDirCheck dependency paths should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/read-dir-check.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:readFileCheck" "readFileCheck dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/read-file-check.nix
areas/app/web/targets/read-file-marker.txt
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:readFileSharedA" "readFileSharedA dependency paths should be exact" <<'EOF'
areas/app/web/shared-read-file.txt
areas/app/web/targets.nix
areas/app/web/targets/read-file-shared.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:readFileSharedB" "readFileSharedB dependency paths should be exact" <<'EOF'
areas/app/web/shared-read-file.txt
areas/app/web/targets.nix
areas/app/web/targets/read-file-shared.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:sharedExistsA" "sharedExistsA dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/exists-marker
areas/app/web/targets/shared-exists.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:sharedExistsB" "sharedExistsB dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/exists-marker
areas/app/web/targets/shared-exists.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:sharedReadDirA" "sharedReadDirA dependency paths should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/shared-read-dir.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:sharedReadDirB" "sharedReadDirB dependency paths should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/shared-read-dir.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:closureMiddleUser" "closureMiddleUser dependency paths should be exact" <<'EOF'
areas/app/web/closure-leaf.txt
areas/app/web/targets.nix
areas/app/web/targets/closure-middle.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:closureChainA" "closureChainA dependency paths should be exact" <<'EOF'
areas/app/web/closure-leaf.txt
areas/app/web/targets.nix
areas/app/web/targets/closure-chain.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:closureChainB" "closureChainB dependency paths should be exact" <<'EOF'
areas/app/web/closure-leaf.txt
areas/app/web/targets.nix
areas/app/web/targets/closure-chain.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:resolverModuleUser" "resolverModuleUser dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/resolver-module-user.nix
system/tectonix/resolve.nix
system/tectonix/resolver-module.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:nestedOptional" "nestedOptional dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/nested-optional.nix
areas/app/web/untracked-dir/nested-marker.nix
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/app/web:treeShaCheck" "treeShaCheck dependency paths should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/tree-sha-check.nix
areas/lib/shared
system/tectonix/resolve.nix
EOF

    assert_target_dependency_paths "$deps" "//areas/lib/shared:gamma" "gamma dependency paths should be exact" <<'EOF'
areas/lib/shared/common.nix
areas/lib/shared/targets.nix
areas/lib/shared/targets/gamma.nix
system/tectonix/resolve.nix
EOF
}

base_args=$(tecnix_args)
other_args=$(tecnix_other_args)

echo "Testing tecnixTargetNames..."
target_names=$(tecnix_eval_json_no_cache "builtins.tecnixTargetNames ($base_args)")
assert_jq "$target_names" \
    '. == ["//areas/app/web:alpha", "//areas/app/web:beta", "//areas/app/web:srcdir", "//areas/app/web:symlinked", "//areas/app/web:optional", "//areas/app/web:existsCheck", "//areas/app/web:fileTypeCheck", "//areas/app/web:readDirCheck", "//areas/app/web:readFileCheck", "//areas/app/web:readFileSharedA", "//areas/app/web:readFileSharedB", "//areas/app/web:sharedExistsA", "//areas/app/web:sharedExistsB", "//areas/app/web:sharedReadDirA", "//areas/app/web:sharedReadDirB", "//areas/app/web:closureMiddleUser", "//areas/app/web:closureChainA", "//areas/app/web:closureChainB", "//areas/app/web:resolverModuleUser", "//areas/app/web:nestedOptional", "//areas/app/web:treeShaCheck", "//areas/lib/shared:gamma"]' \
    "tecnixTargetNames should return the expected flat target list"

# Target discovery has its own dependency graph, useful for debugging why the
# discovered target list is or is not reusable.
echo "Testing tecnixTargetNames includeDependencies..."
target_name_deps=$(tecnix_eval_json_no_cache "tecnixTargetNameDependencyPathSet ($base_args)")
assert_json_equal "$(jq -c 'keys' <<< "$target_name_deps")" '[".meta/manifest.json","system/tectonix/resolve.nix"]' \
    "tecnixTargetNames includeDependencies should have the exact expected dependency paths"
assert_jq "$target_name_deps" '."system/tectonix/resolve.nix" | startswith("git:")' \
    "tecnixTargetNames includeDependencies should include the resolver fingerprint"
assert_jq "$target_name_deps" '.".meta/manifest.json" | startswith("git:")' \
    "tecnixTargetNames includeDependencies should include the manifest fingerprint"

# The rebased resolver contract keeps zone.src as a string-valued store path
# while exposing zone.srcPath as a path value for path arithmetic.
echo "Testing zone source builtin result types..."
zone_source_types=$(tecnix_eval_json_with_settings '{ src = builtins.typeOf (builtins.unsafeTectonixInternalZoneSrc "//areas/app/web"); path = builtins.typeOf (builtins.unsafeTectonixInternalZonePath "//areas/app/web"); }')
assert_jq "$zone_source_types" '.src == "string" and .path == "path"' \
    "zone source builtins should preserve the resolver-facing string/path split"

# Filtered source-path cache keys must not ignore the filter predicate. Use the
# same path name for both filtered paths so a stale persistent hit would return
# the first filter's store path for the second filter.
echo "Testing filtered zone source paths do not share a persistent cache key..."
filtered_zone_source=$(tecnix_eval_json_with_settings '
let
  src = builtins.unsafeTectonixInternalZoneSrc "//areas/app/web";
  onlyCommon = builtins.path {
    name = "filtered-zone-source";
    path = src;
    filter = path: type: type == "directory" || builtins.baseNameOf path == "common.nix";
  };
  onlyZone = builtins.path {
    name = "filtered-zone-source";
    path = src;
    filter = path: type: type == "directory" || builtins.baseNameOf path == "zone.nix";
  };
in {
  commonHasCommon = builtins.pathExists (onlyCommon + "/common.nix");
  commonHasZone = builtins.pathExists (onlyCommon + "/zone.nix");
  zoneHasCommon = builtins.pathExists (onlyZone + "/common.nix");
  zoneHasZone = builtins.pathExists (onlyZone + "/zone.nix");
}')
assert_jq "$filtered_zone_source" '.commonHasCommon and (.commonHasZone | not) and (.zoneHasCommon | not) and .zoneHasZone' \
    "different filters over one fingerprinted source should produce different filtered paths"

# A single EvalState has lazy repository accessors. Reusing it for a different
# repo context must fail rather than silently reusing the first context.
echo "Testing Tecnix EvalState rejects repo context changes..."
context_mismatch_err="$TEST_ROOT/tecnix-context-mismatch.err"
expect 1 nix eval --json \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --option tecnix-eval-cache false \
    --expr "let first = builtins.tecnixTargetNames ($base_args); in builtins.deepSeq first (builtins.tecnixTargetNames ($other_args))" \
    >/dev/null 2>"$context_mismatch_err"
grepQuiet "already configured" < "$context_mismatch_err"

# Source-available eval must know the dirty overlay. If git status fails, do not
# continue as if the checkout were clean.
echo "Testing dirty checkout status failure is fatal..."
bad_checkout_err="$TEST_ROOT/tecnix-bad-checkout.err"
bad_checkout_expr=$(rewrite_tecnix_test_expr "tecnixTargetDependencyPathSet (($base_args) // { checkoutPath = \"$TEST_ROOT/not-a-checkout\"; targets = [ \"//areas/app/web:alpha\" ]; })")
expect 1 nix eval --json \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --option tecnix-eval-cache false \
    --expr "$bad_checkout_expr" \
    >/dev/null 2>"$bad_checkout_err"
grepQuiet "not-a-checkout" < "$bad_checkout_err"

# `rev` may be omitted when `checkoutPath` names a git checkout: it defaults
# to the checkout's HEAD (resolveCheckoutHeadRev). Pin both sides of the
# fallback: HEAD resolution matches an explicit rev, and a checkout that
# cannot answer fails with guidance rather than proceeding.
echo "Testing omitted rev defaults to the checkout's HEAD..."
no_rev_args="{ gitDir = \"$TEST_WORLD/.git\"; resolver = \"system/tectonix/resolve.nix\"; args = { system = \"test-system\"; }; checkoutPath = \"$TEST_WORLD\"; }"
no_rev_names=$(tecnix_eval_json_no_cache "builtins.tecnixTargetNames ($no_rev_args)")
explicit_rev_names=$(tecnix_eval_json_no_cache "builtins.tecnixTargetNames ($base_args)")
assert_json_equal "$no_rev_names" "$explicit_rev_names" \
    "omitting rev should resolve the checkout's HEAD and match an explicit rev"

echo "Testing omitted rev with an invalid checkout fails with guidance..."
no_rev_bad_checkout_err="$TEST_ROOT/tecnix-no-rev-bad-checkout.err"
expect 1 nix eval --json \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --option tecnix-eval-cache false \
    --expr "builtins.tecnixTargetNames { gitDir = \"$TEST_WORLD/.git\"; resolver = \"system/tectonix/resolve.nix\"; args = { }; checkoutPath = \"$TEST_ROOT/not-a-checkout\"; }" \
    >/dev/null 2>"$no_rev_bad_checkout_err"
grepQuiet "could not determine git SHA" < "$no_rev_bad_checkout_err"

# Repo-root directory listings have no explicit source-path representation in
# the source closure yet. Fail closed instead of returning under-tracked deps.
echo "Testing repo-root source access fails closed..."
repo_root_access_err="$TEST_ROOT/tecnix-repo-root-access.err"
repo_root_access_expr=$(rewrite_tecnix_test_expr "tecnixTargetDependencyPathSet (($base_args) // { resolver = \"system/repo-root-read-dir/resolve.nix\"; targets = [ \"//repo:rootReadDir\" ]; })")
expect 1 nix eval --json \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --option tecnix-eval-cache false \
    --expr "$repo_root_access_expr" \
    >/dev/null 2>"$repo_root_access_err"
grepQuiet "repo-root source access" < "$repo_root_access_err"

# These internal helpers are identity/lazy source-deps wrappers outside tracked
# Tecnix dependency evaluation.
echo "Testing internal source-deps wrapper builtins..."
source_deps_wrappers=$(tecnix_eval_json_no_cache '{ scope = builtins.tecnixInternalSourceDepsScope { x = 1; }; attrs = builtins.tecnixInternalSourceDepsAttrs { a = 2; b = 3; }; list = builtins.tecnixInternalSourceDepsList [ 4 5 ]; }')
assert_jq "$source_deps_wrappers" '.scope.x == 1 and .attrs.a == 2 and .attrs.b == 3 and .list == [4, 5]' \
    "internal source-deps wrapper builtins should preserve values"

echo "Testing tecnixTargets on a clean worktree..."
clean_targets=$(tecnix_eval_json_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:beta\" \"//areas/app/web:alpha\" ]; })")
assert_jq "$clean_targets" 'length == 2' "tecnixTargets should return two results"
assert_jq "$clean_targets" '."//areas/app/web:beta".name == "beta" and ."//areas/app/web:beta".marker == "beta:clean-web-common:test-system" and ."//areas/app/web:beta".resolvedBy == "clean-resolver"' \
    "tecnixTargets should resolve beta and use the clean resolver"
assert_jq "$clean_targets" '."//areas/app/web:alpha".name == "alpha" and ."//areas/app/web:alpha".marker == "alpha:clean-web-common:test-system" and ."//areas/app/web:alpha".resolvedBy == "clean-resolver"' \
    "tecnixTargets should resolve alpha"

# tecnixTargets can evaluate targets in parallel when eval cores are enabled.
echo "Testing parallel tecnixTargets evaluation..."
parallel_targets=$(tecnix_eval_json_parallel_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:beta\" \"//areas/app/web:alpha\" ]; })")
assert_jq "$parallel_targets" '."//areas/app/web:beta".name == "beta" and ."//areas/app/web:alpha".name == "alpha"' \
    "parallel tecnixTargets should resolve each requested target"

echo "Testing tecnixTargets includeDependencies for overlapping clean targets..."
clean_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; })")
assert_jq "$clean_deps" 'has("//areas/app/web:alpha") and has("//areas/app/web:beta")' \
    "tecnixTargets includeDependencies should return dependencies keyed by target"
assert_jq "$clean_deps" '(."//areas/app/web:alpha" | has("system/tectonix/resolve.nix"))' \
    "alpha deps should include the resolver"
assert_jq "$clean_deps" '(."//areas/app/web:beta" | has("system/tectonix/resolve.nix"))' \
    "beta deps should include the resolver"
assert_jq "$clean_deps" '(."//areas/app/web:alpha"."system/tectonix/resolve.nix" | startswith("git:")) and (."//areas/app/web:alpha"."areas/app/web/common.nix" | startswith("git:"))' \
    "dependency entries should include source fingerprints"
assert_jq "$clean_deps" '(."//areas/app/web:alpha" | has("areas/app/web") | not) and (."//areas/app/web:beta" | has("areas/app/web") | not)' \
    "ordinary resolver path literals should not add the broad zone root as an eval input"
assert_jq "$clean_deps" '(."//areas/app/web:alpha" | has("areas/app/web/targets.nix") and has("areas/app/web/common.nix") and has("areas/app/web/targets/alpha.nix"))' \
    "alpha deps should include shared and alpha-specific files"
assert_jq "$clean_deps" '(."//areas/app/web:beta" | has("areas/app/web/targets.nix") and has("areas/app/web/common.nix") and has("areas/app/web/targets/beta.nix"))' \
    "beta deps should include shared and beta-specific files"
assert_jq "$clean_deps" '(."//areas/app/web:alpha" | has("areas/app/web/targets/beta.nix") | not)' \
    "alpha deps should not include beta-specific files"
assert_jq "$clean_deps" '(."//areas/app/web:beta" | has("areas/app/web/targets/alpha.nix") | not)' \
    "beta deps should not include alpha-specific files"

# tecnixTargets includeDependencies can evaluate target misses in parallel when eval cores
# are enabled. It should still produce isolated per-target dependency graphs.
echo "Testing parallel tecnixTargets includeDependencies evaluation..."
parallel_deps=$(tecnix_eval_json_parallel_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; })")
assert_jq "$parallel_deps" '( ."//areas/app/web:alpha" | has("areas/app/web/common.nix") and has("areas/app/web/targets/alpha.nix") and (has("areas/app/web/targets/beta.nix") | not) )' \
    "parallel alpha deps should include alpha-specific files only"
assert_jq "$parallel_deps" '( ."//areas/app/web:beta" | has("areas/app/web/common.nix") and has("areas/app/web/targets/beta.nix") and (has("areas/app/web/targets/alpha.nix") | not) )' \
    "parallel beta deps should include beta-specific files only"

# Source-deps scopes must be target-local. These two targets intentionally use
# the same scope key with different source files; mutable global replay would
# leak owner-collision-b.txt into A or vice versa.
echo "Testing source-deps scope isolation..."
owner_collision_deps=$(tecnix_eval_json_parallel_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:ownerCollisionA\" \"//areas/app/web:ownerCollisionB\" ]; })")
assert_target_dependency_paths "$owner_collision_deps" "//areas/app/web:ownerCollisionA" "ownerCollisionA deps should not include ownerCollisionB files" <<'EOF'
areas/app/web/common.nix
areas/app/web/owner-collision-a.txt
areas/app/web/targets.nix
areas/app/web/targets/owner-collision.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$owner_collision_deps" "//areas/app/web:ownerCollisionB" "ownerCollisionB deps should not include ownerCollisionA files" <<'EOF'
areas/app/web/common.nix
areas/app/web/owner-collision-b.txt
areas/app/web/targets.nix
areas/app/web/targets/owner-collision.nix
system/tectonix/resolve.nix
EOF

# Reusing an already-forced source-deps-scoped value must publish its source
# provenance into the later target. This is the small form of the sequential
# all-target undertracking bug seen in large repos.
echo "Testing source-deps scoped value reuse for sequential target reuse..."
shared_owner_isolated=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:sharedOwnerB\" ]; })")
shared_owner_pair=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:sharedOwnerA\" \"//areas/app/web:sharedOwnerB\" ]; })")
if ! diff -u \
    <(jq -S '."//areas/app/web:sharedOwnerB"' <<< "$shared_owner_isolated") \
    <(jq -S '."//areas/app/web:sharedOwnerB"' <<< "$shared_owner_pair"); then
    fail "sharedOwnerB deps should match isolated deps after sharedOwnerA preforces the source-deps-scoped value"
fi

# Reusing an already-forced imported function result must also replay the
# imported file and downstream readFile provenance into the later target.
echo "Testing imported shared builder replay for sequential target reuse..."
shared_builder_isolated=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:sharedBuilderB\" ]; })")
shared_builder_pair=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:sharedBuilderA\" \"//areas/app/web:sharedBuilderB\" ]; })")
if ! diff -u \
    <(jq -S '."//areas/app/web:sharedBuilderB"' <<< "$shared_builder_isolated") \
    <(jq -S '."//areas/app/web:sharedBuilderB"' <<< "$shared_builder_pair"); then
    fail "sharedBuilderB deps should match isolated deps after sharedBuilderA preforces the imported shared builder"
fi

# The same replay must hold when the shared sourceful function file is imported
# independently by each target and the second import hits the tracked file eval cache.
echo "Testing tracked file cache replay for sequential imported builder reuse..."
indirect_builder_isolated=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:indirectBuilderB\" ]; })")
indirect_builder_pair=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:indirectBuilderA\" \"//areas/app/web:indirectBuilderB\" ]; })")
if ! diff -u \
    <(jq -S '."//areas/app/web:indirectBuilderB"' <<< "$indirect_builder_isolated") \
    <(jq -S '."//areas/app/web:indirectBuilderB"' <<< "$indirect_builder_pair"); then
    fail "indirectBuilderB deps should match isolated deps after indirectBuilderA prewarms the shared imported builder file"
fi

# The rust module shape uses callPackage (import ./builder.nix) in a shared let.
echo "Testing callPackage imported builder replay for sequential target reuse..."
callpackage_builder_isolated=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:callPackageBuilderB\" ]; })")
callpackage_builder_pair=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:callPackageBuilderA\" \"//areas/app/web:callPackageBuilderB\" ]; })")
if ! diff -u \
    <(jq -S '."//areas/app/web:callPackageBuilderB"' <<< "$callpackage_builder_isolated") \
    <(jq -S '."//areas/app/web:callPackageBuilderB"' <<< "$callpackage_builder_pair"); then
    fail "callPackageBuilderB deps should match isolated deps after callPackageBuilderA prewarms the shared callPackage/import builder"
fi

# This is a Nix-level shape for the ValueStorage::operator= provenance bug.
# copyOnlyPrewarm forces a shared resolver-level string. copyOnlyConsumer then
# uses builtins.break, whose implementation returns by copying its already-forced
# argument without forcing it again. Without copying Value* provenance, the
# consumer misses copy-only-marker.txt.
echo "Testing copy-only finished value provenance replay from Nix code..."
copy_only_pair=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//system/tectonix-copy-value:copyOnlyPrewarm\" \"//system/tectonix-copy-value:copyOnlyConsumer\" ]; })")
assert_jq "$copy_only_pair" '."//system/tectonix-copy-value:copyOnlyConsumer" | has("system/tectonix/copy-only-marker.txt")' \
    "copyOnlyConsumer deps should include the copied sourceful value"

# The target builtin can return normal target values and dependency records in
# one public API shape.
echo "Testing tecnixTargets with dependency records..."
targets_with_deps=$(tecnix_eval_json_parallel_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; includeDependencies = true; })")
assert_jq "$targets_with_deps" 'length == 2 and .[0].target == "//areas/app/web:alpha" and .[1].target == "//areas/app/web:beta"' \
    "tecnixTargets includeDependencies should preserve input order"
assert_jq "$targets_with_deps" '.[0].value.drvPath == "/nix/store/00000000000000000000000000000000-alpha-clean-web-common.drv" and .[1].value.drvPath == "/nix/store/00000000000000000000000000000000-beta-clean-web-common.drv"' \
    "tecnixTargets includeDependencies should include normal target values"
assert_jq "$targets_with_deps" '((.[0].dependencies | has("areas/app/web/targets/alpha.nix") and (has("areas/app/web/targets/beta.nix") | not))) and ((.[1].dependencies | has("areas/app/web/targets/beta.nix") and (has("areas/app/web/targets/alpha.nix") | not)))' \
    "tecnixTargets dependencies should stay isolated per target"
assert_jq "$targets_with_deps" '.[0].dependencies."areas/app/web/targets/alpha.nix" | startswith("git:")' \
    "tecnixTargets dependencies should include source fingerprints"

# A target that does src = ./. (represented here by forcing ../src-dir) should
# depend on the directory tree, not on every child file. This keeps dependency
# discovery cheap for large source directories.
echo "Testing directory source tracking..."
srcdir_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:srcdir\" ]; })")
assert_jq "$srcdir_deps" '(."//areas/app/web:srcdir" | has("areas/app/web/src-dir"))' \
    "srcdir deps should include the source directory"
assert_jq "$srcdir_deps" '(."//areas/app/web:srcdir" | has("areas/app/web/src-dir/file-001.txt") | not) and (."//areas/app/web:srcdir" | has("areas/app/web/src-dir/file-002.txt") | not)' \
    "srcdir deps should not include child files under the source directory"

# Import resolution through symlinks should track both the symlink and the file
# it resolves to, otherwise a symlink retarget could leave stale dependencies.
echo "Testing symlink import tracking..."
symlink_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:symlinked\" ]; })")
assert_jq "$symlink_deps" '(."//areas/app/web:symlinked" | has("areas/app/web/targets/current.nix") and has("areas/app/web/targets/alpha.nix"))' \
    "symlinked deps should include both the symlink and resolved file"

# Prewarming normal target evaluation in the same EvalState populates import
# resolution caches without tracking. Dependency discovery must still use a
# tracking-safe cache domain for Tecnix paths so symlink resolution is recorded.
echo "Testing dependency tracking after untracked import-resolution cache warmup..."
prewarmed_symlink_deps=$(tecnix_eval_json_no_cache "let args = (($base_args) // { targets = [ \"//areas/app/web:symlinked\" ]; }); prewarm = builtins.tecnixTargets args; deps = builtins.deepSeq prewarm (tecnixTargetDependencyPathSet args); in deps")
assert_jq "$prewarmed_symlink_deps" '(."//areas/app/web:symlinked" | has("areas/app/web/targets/current.nix") and has("areas/app/web/targets/alpha.nix"))' \
    "dependency tracking should not reuse untracked import-resolution cache entries for Tecnix paths"

# Negative path existence checks are dependencies too. If a target branches on an
# absent file, adding that file later must invalidate the dependency cache.
echo "Testing absent path dependency tracking..."
missing_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:optional\" ]; })")
assert_jq "$missing_deps" '."//areas/app/web:optional"."areas/app/web/targets/optional-marker.nix" == "absent"' \
    "optional deps should include the absent optional-marker path with an absent fingerprint"

# Positive existence checks are also dependencies, even when the target does not
# read or import the file after checking for it.
echo "Testing existing path dependency tracking..."
existing_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:existsCheck\" ]; })")
assert_jq "$existing_deps" '(."//areas/app/web:existsCheck"."areas/app/web/targets/exists-marker" | startswith("git:"))' \
    "existsCheck deps should include the existing marker path fingerprint"

# File type checks are dependencies too, even when the target does not read the
# file contents.
echo "Testing readFileType dependency tracking..."
file_type_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:fileTypeCheck\" ]; })")
assert_jq "$file_type_deps" '(."//areas/app/web:fileTypeCheck"."areas/app/web/targets/exists-marker" | startswith("git:"))' \
    "fileTypeCheck deps should include the typed marker path fingerprint"

# Directory enumeration is a dependency on the directory tree. Changes under the
# directory should invalidate via the directory fingerprint, without tracking
# every child as a separate dependency.
echo "Testing readDir dependency tracking..."
read_dir_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:readDirCheck\" ]; })")
assert_jq "$read_dir_deps" '(."//areas/app/web:readDirCheck"."areas/app/web/src-dir" | startswith("git:"))' \
    "readDirCheck deps should include the enumerated directory fingerprint"
assert_jq "$read_dir_deps" '(."//areas/app/web:readDirCheck" | has("areas/app/web/src-dir/file-001.txt") | not) and (."//areas/app/web:readDirCheck" | has("areas/app/web/src-dir/file-002.txt") | not)' \
    "readDirCheck deps should not include every enumerated child"

# readFile dependencies are not imports, so they specifically exercise the
# SourceAccessor read path and replay of sourceful non-import thunks.
echo "Testing readFile dependency tracking..."
read_file_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:readFileCheck\" ]; })")
assert_jq "$read_file_deps" '(."//areas/app/web:readFileCheck"."areas/app/web/targets/read-file-marker.txt" | startswith("git:"))' \
    "readFileCheck deps should include the read file fingerprint"

# Two targets share one lazy builtins.readFile thunk from targets.nix. If the
# first target forces it, the second target must still get the read file through
# Value* provenance replay rather than physical I/O.
echo "Testing shared readFile replay across targets..."
shared_read_file_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:readFileSharedA\" \"//areas/app/web:readFileSharedB\" ]; })")
assert_target_dependency_paths "$shared_read_file_deps" "//areas/app/web:readFileSharedA" "shared readFile A deps should be exact" <<'EOF'
areas/app/web/shared-read-file.txt
areas/app/web/targets.nix
areas/app/web/targets/read-file-shared.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$shared_read_file_deps" "//areas/app/web:readFileSharedB" "shared readFile B deps should be exact" <<'EOF'
areas/app/web/shared-read-file.txt
areas/app/web/targets.nix
areas/app/web/targets/read-file-shared.nix
system/tectonix/resolve.nix
EOF

# Shared sourceful thunks can return any type. These catch regressions that only
# replay string-valued source thunks by also reusing a bool and a list.
echo "Testing shared pathExists/readDir replay across targets..."
shared_sourceful_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:sharedExistsA\" \"//areas/app/web:sharedExistsB\" \"//areas/app/web:sharedReadDirA\" \"//areas/app/web:sharedReadDirB\" ]; })")
assert_target_dependency_paths "$shared_sourceful_deps" "//areas/app/web:sharedExistsA" "sharedExistsA deps should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/exists-marker
areas/app/web/targets/shared-exists.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$shared_sourceful_deps" "//areas/app/web:sharedExistsB" "sharedExistsB deps should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/exists-marker
areas/app/web/targets/shared-exists.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$shared_sourceful_deps" "//areas/app/web:sharedReadDirA" "sharedReadDirA deps should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/shared-read-dir.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$shared_sourceful_deps" "//areas/app/web:sharedReadDirB" "sharedReadDirB deps should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/shared-read-dir.nix
system/tectonix/resolve.nix
EOF

# Multi-hop replay must flatten the whole AccessSet closure, not just the direct
# reused value. Both targets share closureOuter -> closureMiddle -> closureLeaf.
echo "Testing chained source closure replay across targets..."
closure_chain_deps=$(tecnix_eval_json_access_set_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:closureMiddleUser\" \"//areas/app/web:closureChainA\" \"//areas/app/web:closureChainB\" ]; })")
assert_target_dependency_paths "$closure_chain_deps" "//areas/app/web:closureMiddleUser" "closureMiddleUser deps should include the leaf closure" <<'EOF'
areas/app/web/closure-leaf.txt
areas/app/web/targets.nix
areas/app/web/targets/closure-middle.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$closure_chain_deps" "//areas/app/web:closureChainA" "closureChainA deps should include the whole replayed closure" <<'EOF'
areas/app/web/closure-leaf.txt
areas/app/web/targets.nix
areas/app/web/targets/closure-chain.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$closure_chain_deps" "//areas/app/web:closureChainB" "closureChainB deps should include the whole already-published closure" <<'EOF'
areas/app/web/closure-leaf.txt
areas/app/web/targets.nix
areas/app/web/targets/closure-chain.nix
system/tectonix/resolve.nix
EOF

# A resolver-side lazy module import should stay attached only to the target
# branch that forces it. This mirrors the real-world oracle failure where a
# resolver module file leaked into an unrelated target.
echo "Testing resolver-side lazy module dependency isolation..."
resolver_module_deps=$(tecnix_eval_json_access_set_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:resolverModuleUser\" \"//areas/lib/shared:gamma\" ]; })")
assert_target_dependency_paths "$resolver_module_deps" "//areas/app/web:resolverModuleUser" "resolverModuleUser deps should be exact" <<'EOF'
areas/app/web/targets.nix
areas/app/web/targets/resolver-module-user.nix
system/tectonix/resolve.nix
system/tectonix/resolver-module.nix
EOF
assert_target_dependency_paths "$resolver_module_deps" "//areas/lib/shared:gamma" "gamma deps should stay isolated from resolver module" <<'EOF'
areas/lib/shared/common.nix
areas/lib/shared/targets.nix
areas/lib/shared/targets/gamma.nix
system/tectonix/resolve.nix
EOF

# Access-set output is now the canonical dependency engine. The default builtin
# path and the explicit compatibility env knob must stay byte-for-byte equivalent
# for every edge-case target in this fixture, including imports, readFile,
# pathExists, readFileType, readDir, symlinked imports, directory materialization,
# and cross-zone targets.
echo "Testing default dependency output matches explicit access-set output..."
all_target_deps_expr="let args = $base_args; targets = builtins.tecnixTargetNames args; in tecnixTargetDependencyPathSet (args // { inherit targets; })"
default_all_deps=$(tecnix_eval_json_no_cache "$all_target_deps_expr")
access_set_all_deps=$(tecnix_eval_json_access_set_no_cache "$all_target_deps_expr")
if ! diff -u <(jq -S . <<< "$default_all_deps") <(jq -S . <<< "$access_set_all_deps"); then
    fail "default dependency output should match explicit access-set output for all fixture targets"
fi
assert_clean_dependency_paths "$default_all_deps"
assert_clean_dependency_paths "$access_set_all_deps"

# Correctness oracle: multi-target dependency output must equal isolated
# single-target output for every target. Run isolated targets in separate evals so
# this mirrors the production oracle instead of comparing against a second
# dependency pass in an already-warmed EvalState.
echo "Testing access-set multi-target vs isolated-target oracle..."
isolated_access_set_deps='{}'
while IFS= read -r target; do
    single_target_deps=$(tecnix_eval_json_access_set_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"$target\" ]; })")
    isolated_access_set_deps=$(jq -S -s '.[0] * .[1]' <(printf '%s' "$isolated_access_set_deps") <(printf '%s' "$single_target_deps"))
done <<< "$(jq -r '.[]' <<< "$target_names")"
if ! diff -u <(jq -S . <<< "$access_set_all_deps") <(jq -S . <<< "$isolated_access_set_deps"); then
    fail "access-set multi-target deps should match isolated single-target deps for all fixture targets"
fi

# The same oracle must hold when target dependency evaluation is parallelized.
# This protects against shared evaluator/cache provenance leaking dependencies
# between concurrently evaluated targets.
echo "Testing parallel access-set multi-target vs isolated-target oracle..."
parallel_all_deps=$(tecnix_eval_json_parallel_no_cache "$all_target_deps_expr")
if ! diff -u <(jq -S . <<< "$parallel_all_deps") <(jq -S . <<< "$isolated_access_set_deps"); then
    fail "parallel access-set multi-target deps should match isolated single-target deps for all fixture targets"
fi

# The dependencies returned alongside target values should exactly match the
# dependency wrapper used by the rest of this test.
echo "Testing access-set target dependency-record equivalence..."
access_set_record_deps=$(tecnix_eval_json_access_set_no_cache "let args = $base_args; targets = builtins.tecnixTargetNames args; combined = builtins.tecnixTargets (args // { inherit targets; includeDependencies = true; }); combinedDeps = builtins.listToAttrs (map (record: { name = record.target; value = record.dependencies; }) combined); deps = tecnixTargetDependencyPathSet (args // { inherit targets; }); in builtins.deepSeq combined (builtins.deepSeq deps { equal = combinedDeps == deps; })")
assert_jq "$access_set_record_deps" '.equal == true' \
    "access-set target dependency records should match the dependency wrapper"

# Detached parallel prefetch must not leak into tracked closures, and tracked
# closures must be identical with and without the parallel executor.
# builtins.parallel's prefetch list and toJSON's deep-force fanout are skipped
# under tracking: tracking contexts are thread-confined, so tracked evaluation
# must not spawn parallel work.
echo "Testing detached prefetch under tracked evaluation..."
prefetch_targets='[ "//areas/app/web:parallelPrefetch" "//areas/app/web:jsonCheck" ]'
prefetch_seq_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = $prefetch_targets; })")
prefetch_par_deps=$(tecnix_eval_json_parallel_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = $prefetch_targets; })")
if ! diff -u <(jq -S . <<< "$prefetch_seq_deps") <(jq -S . <<< "$prefetch_par_deps"); then
    fail "prefetch-using targets should have identical closures with and without the parallel executor"
fi
assert_target_dependency_paths "$prefetch_par_deps" "//areas/app/web:parallelPrefetch" "parallelPrefetch deps should exclude the prefetch-only path" <<'EOF'
areas/app/web/common.nix
areas/app/web/targets.nix
areas/app/web/targets/parallel-prefetch.nix
system/tectonix/resolve.nix
EOF
assert_target_dependency_paths "$prefetch_par_deps" "//areas/app/web:jsonCheck" "jsonCheck deps should be exact" <<'EOF'
areas/app/web/common.nix
areas/app/web/shared-read-file.txt
areas/app/web/targets.nix
areas/app/web/targets/json-check.nix
system/tectonix/resolve.nix
EOF
# Path-filter functions run arbitrary Nix code from inside NAR serialization,
# where dump-internal tracking is suppressed. The filter's own source reads
# (here: an ignore list readFile'd for the first time inside the filter) are
# real dependencies and must appear in the closure, as must the filtered
# source tree itself.
echo "Testing path filter dependency tracking..."
filtered_src_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:filteredSrc\" ]; })")
assert_target_dependency_paths "$filtered_src_deps" "//areas/app/web:filteredSrc" "filteredSrc deps should include the ignore list read inside the filter" <<'EOF'
areas/app/web/common.nix
areas/app/web/filter-ignore.txt
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/filtered-src.nix
system/tectonix/resolve.nix
EOF

# A single-target query evaluates on the coordinator thread with the executor
# enabled, which is the configuration where toJSON's prefetch used to spawn.
prefetch_json_single=$(tecnix_eval_json_parallel_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:jsonCheck\" ]; })")
assert_target_dependency_paths "$prefetch_json_single" "//areas/app/web:jsonCheck" "single-target jsonCheck deps should be exact on the coordinator thread" <<'EOF'
areas/app/web/common.nix
areas/app/web/shared-read-file.txt
areas/app/web/targets.nix
areas/app/web/targets/json-check.nix
system/tectonix/resolve.nix
EOF

# Repeated path materialization in one EvalState must not lose the source path
# after the srcToStore cache has been populated.
echo "Testing repeated path materialization dependencies..."
repeated_srcdir_deps=$(tecnix_eval_json_access_set_no_cache "let args = (($base_args) // { targets = [ \"//areas/app/web:srcdir\" ]; }); first = tecnixTargetDependencyPathSet args; second = builtins.deepSeq first (tecnixTargetDependencyPathSet args); in { inherit first second; }")
for pass in first second; do
    assert_target_dependency_paths "$(jq -c ".$pass" <<< "$repeated_srcdir_deps")" "//areas/app/web:srcdir" "srcdir $pass dependency paths should be exact" <<'EOF'
areas/app/web/src-dir
areas/app/web/targets.nix
areas/app/web/targets/srcdir.nix
system/tectonix/resolve.nix
EOF
done

# The directory-level dependency still needs to see dirty child files through
# the checkout overlay.
echo "Testing dirty child affects directory source target..."
clean_srcdir_drv=$(tecnix_eval_json_no_cache "(builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:srcdir\" ]; })).\"//areas/app/web:srcdir\".drvPath")
cat > "$TEST_WORLD/areas/app/web/src-dir/file-001.txt" << 'DIRTY_SRC_EOF'
dirty source file 001
DIRTY_SRC_EOF
dirty_srcdir_drv=$(tecnix_eval_json_no_cache "(builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:srcdir\" ]; })).\"//areas/app/web:srcdir\".drvPath")
if [[ "$clean_srcdir_drv" == "$dirty_srcdir_drv" ]]; then
    echo "clean drvPath: $clean_srcdir_drv" >&2
    echo "dirty drvPath: $dirty_srcdir_drv" >&2
    fail "dirty child under source directory should change the source derivation"
fi

# Repeated dependency calls in one EvalState should not lose imports to the
# evaluator-local file cache.
echo "Testing repeated tecnixTargets includeDependencies calls..."
repeated_deps=$(tecnix_eval_json_no_cache "let args = (($base_args) // { targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; }); first = tecnixTargetDependencyPathSet args; second = tecnixTargetDependencyPathSet args; in { inherit first second; }")
assert_jq "$repeated_deps" '(.first."//areas/app/web:alpha" | has("system/tectonix/resolve.nix")) and (.second."//areas/app/web:alpha" | has("system/tectonix/resolve.nix"))' \
    "repeated dependency calls should keep resolver deps"
assert_jq "$repeated_deps" '(.first."//areas/app/web:beta" | has("areas/app/web/common.nix")) and (.second."//areas/app/web:beta" | has("areas/app/web/common.nix"))' \
    "repeated dependency calls should keep shared zone deps"

# Sequential dependency calls for different targets in one EvalState should not
# let evaluator-local caches hide imports forced by the first target. The fixture's
# targets.nix has a shared lazy `common = import ./common.nix ...`; alpha forces it
# first, and beta must still record common.nix when evaluated afterwards.
echo "Testing shared lazy import tracking across dependency calls..."
sequential_deps=$(tecnix_eval_json_no_cache "let alphaArgs = (($base_args) // { targets = [ \"//areas/app/web:alpha\" ]; }); betaArgs = (($base_args) // { targets = [ \"//areas/app/web:beta\" ]; }); alpha = tecnixTargetDependencyPathSet alphaArgs; beta = builtins.seq alpha (tecnixTargetDependencyPathSet betaArgs); in { inherit alpha beta; }")
assert_jq "$sequential_deps" '(.alpha."//areas/app/web:alpha" | has("areas/app/web/common.nix")) and (.beta."//areas/app/web:beta" | has("areas/app/web/common.nix"))' \
    "shared lazy imports forced by one target should still be tracked for later targets"
assert_jq "$sequential_deps" '(.alpha."//areas/app/web:alpha" | has("areas/app/web/targets/alpha.nix")) and (.beta."//areas/app/web:beta" | has("areas/app/web/targets/beta.nix"))' \
    "sequential dependency calls should keep target-specific deps"
assert_jq "$sequential_deps" '(.alpha."//areas/app/web:alpha" | has("areas/app/web/targets/beta.nix") | not) and (.beta."//areas/app/web:beta" | has("areas/app/web/targets/alpha.nix") | not)' \
    "sequential dependency calls should not leak target-specific deps"

# Dependency cache behavior: first invocation records dependency fingerprints;
# the second invocation can reuse them without re-running target resolution.
echo "Testing dependency cache hit behavior through tecnixTargets includeDependencies..."
cache_test_system="cache-test-system-$$"
cache_expr=$(rewrite_tecnix_test_expr "tecnixTargetDependencyPathSet (($base_args) // { args = { system = \"$cache_test_system\"; }; targets = [ \"//areas/app/web:alpha\" ]; })")
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache miss, evaluating '//areas/app/web:alpha'"
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:alpha'"

# A dependency-cache hit is not a target-value cache hit: with includeTargets
# (the default), a warm invocation must still resolve fresh target values while
# reusing the cached dependency closures, sequentially and in parallel.
echo "Testing target values alongside a dependency cache hit..."
warm_values_system="cache-values-system-$$"
warm_values_expr=$(rewrite_tecnix_test_expr "builtins.tecnixTargets (($base_args) // { args = { system = \"$warm_values_system\"; }; targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; includeDependencies = true; })")
nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$warm_values_expr" \
    > "$TEST_ROOT/cache-values-cold.json" 2> "$TEST_ROOT/cache-values-cold.err"
grepQuiet "tecnixTargets dependencies: dependency cache miss, evaluating '//areas/app/web:alpha'" "$TEST_ROOT/cache-values-cold.err"
nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$warm_values_expr" \
    > "$TEST_ROOT/cache-values-warm.json" 2> "$TEST_ROOT/cache-values-warm.err"
grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:alpha'" "$TEST_ROOT/cache-values-warm.err"
grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:beta'" "$TEST_ROOT/cache-values-warm.err"
if ! diff -u <(jq -S . "$TEST_ROOT/cache-values-cold.json") <(jq -S . "$TEST_ROOT/cache-values-warm.json"); then
    fail "warm tecnixTargets output (values + dependencies) should match the cold output"
fi
assert_jq "$(cat "$TEST_ROOT/cache-values-warm.json")" '(.[0].value.drvPath | endswith("-alpha-clean-web-common.drv")) and (.[1].value.drvPath | endswith("-beta-clean-web-common.drv"))' \
    "warm tecnixTargets should resolve target values after a dependency cache hit"
nix eval --json --verbose \
    --extra-experimental-features 'nix-command parallel-eval' \
    --eval-cores 2 \
    --option lazy-trees true \
    --expr "$warm_values_expr" \
    > "$TEST_ROOT/cache-values-warm-parallel.json" 2> "$TEST_ROOT/cache-values-warm-parallel.err"
grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:alpha'" "$TEST_ROOT/cache-values-warm-parallel.err"
if ! diff -u <(jq -S . "$TEST_ROOT/cache-values-cold.json") <(jq -S . "$TEST_ROOT/cache-values-warm-parallel.json"); then
    fail "parallel warm tecnixTargets output should match the cold output"
fi

# Missing files have stable fingerprints in the dependency cache. The missing
# state can be cached, but creating the file must invalidate that cache row and
# re-evaluate the target.
echo "Testing dependency cache invalidation for newly-created files..."
optional_cache_expr=$(rewrite_tecnix_test_expr "tecnixTargetDependencyPathSet (($base_args) // { args = { system = \"$cache_test_system\"; }; targets = [ \"//areas/app/web:optional\" ]; })")
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$optional_cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache miss, evaluating '//areas/app/web:optional'"
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$optional_cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:optional'"
cat > "$TEST_WORLD/areas/app/web/targets/optional-marker.nix" << 'OPTIONAL_MARKER_EOF'
"present"
OPTIONAL_MARKER_EOF
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$optional_cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache miss, evaluating '//areas/app/web:optional'"
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$optional_cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:optional'"
rm "$TEST_WORLD/areas/app/web/targets/optional-marker.nix"
# The bounded history keeps both the absent-file and present-file closures, so
# switching back to the absent state can hit immediately instead of thrashing.
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$optional_cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:optional'"
expectStderr 0 nix eval --json --verbose \
    --extra-experimental-features 'nix-command' \
    --option lazy-trees true \
    --expr "$optional_cache_expr" \
    | grepQuiet "tecnixTargets dependencies: dependency cache hit for '//areas/app/web:optional'"
cat > "$TEST_WORLD/areas/app/web/targets/optional-marker.nix" << 'OPTIONAL_MARKER_EOF'
"present"
OPTIONAL_MARKER_EOF
dirty_optional_targets=$(tecnix_eval_json_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:optional\" ]; })")
assert_jq "$dirty_optional_targets" '."//areas/app/web:optional".marker == "optional:present:clean-web-common:test-system"' \
    "tecnixTargets should see newly-created optional files from checkoutPath"

# Git may report untracked directories coarsely unless asked for all untracked
# files. The dirty overlay must still see newly-created nested files.
echo "Testing untracked nested file overlay..."
clean_nested_targets=$(tecnix_eval_json_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:nestedOptional\" ]; })")
assert_jq "$clean_nested_targets" '."//areas/app/web:nestedOptional".marker == "nested:missing:clean-web-common:test-system"' \
    "nestedOptional should start with the nested marker missing"
mkdir -p "$TEST_WORLD/areas/app/web/untracked-dir"
cat > "$TEST_WORLD/areas/app/web/untracked-dir/nested-marker.nix" << 'NESTED_MARKER_EOF'
"present"
NESTED_MARKER_EOF
dirty_nested_targets=$(tecnix_eval_json_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:nestedOptional\" ]; })")
assert_jq "$dirty_nested_targets" '."//areas/app/web:nestedOptional".marker == "nested:present:clean-web-common:test-system"' \
    "tecnixTargets should see newly-created nested files from checkoutPath"
dirty_nested_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:nestedOptional\" ]; })")
assert_jq "$dirty_nested_deps" '(."//areas/app/web:nestedOptional"."areas/app/web/untracked-dir/nested-marker.nix" | contains("dirty="))' \
    "nestedOptional deps should include the newly-created nested marker path fingerprint"

# Dirty zone worktree: the committed rev stays the same, but checkoutPath should
# overlay dirty zone files.
echo "Testing dirty zone worktree overlay..."
cat > "$TEST_WORLD/areas/app/web/common.nix" << 'COMMON_EOF'
{ system }:
{
  marker = "dirty-web-common";
  inherit system;
}
COMMON_EOF

dirty_zone_targets=$(tecnix_eval_json_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; })")
assert_jq "$dirty_zone_targets" '."//areas/app/web:alpha".marker == "alpha:dirty-web-common:test-system" and ."//areas/app/web:beta".marker == "beta:dirty-web-common:test-system"' \
    "tecnixTargets should see dirty zone source files from checkoutPath"

dirty_zone_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:alpha\" \"//areas/app/web:beta\" ]; })")
assert_jq "$dirty_zone_deps" '(."//areas/app/web:alpha"."areas/app/web/common.nix" | contains("dirty=")) and (."//areas/app/web:beta"."areas/app/web/common.nix" | contains("dirty="))' \
    "tecnixTargets includeDependencies should track dirty zone files with repo-relative fingerprints"

# Dirty resolver worktree: the resolver is outside a zone, so this exercises the
# repo-wide dirty overlay used for the resolver path.
echo "Testing dirty resolver overlay..."
sed -i "$TEST_WORLD/system/tectonix/resolve.nix" -e 's/resolvedBy = "clean-resolver";/resolvedBy = "dirty-resolver";/'

dirty_resolver_targets=$(tecnix_eval_json_no_cache "builtins.tecnixTargets (($base_args) // { targets = [ \"//areas/app/web:alpha\" ]; })")
assert_jq "$dirty_resolver_targets" '."//areas/app/web:alpha".resolvedBy == "dirty-resolver" and ."//areas/app/web:alpha".marker == "alpha:dirty-web-common:test-system"' \
    "tecnixTargets should see dirty resolver files from checkoutPath"

dirty_resolver_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($base_args) // { targets = [ \"//areas/app/web:alpha\" ]; })")
assert_jq "$dirty_resolver_deps" '(."//areas/app/web:alpha"."system/tectonix/resolve.nix" | contains("dirty="))' \
    "tecnixTargets includeDependencies should track the resolver path fingerprint when the resolver is dirty"

dirty_target_name_deps=$(tecnix_eval_json_no_cache "tecnixTargetNameDependencyPathSet ($base_args)")
assert_jq "$dirty_target_name_deps" '(."system/tectonix/resolve.nix" | contains("dirty="))' \
    "tecnixTargetNames includeDependencies should track the resolver path fingerprint when the resolver is dirty"

# ============================================================
# Eval cache: enabled end-to-end roundtrip
# ============================================================
# The persistent cache engages only under pure evaluation; these tests use an
# isolated XDG_CACHE_HOME and a committed-only world (no checkoutPath).

echo "Testing eval cache roundtrip..."

EVAL_CACHE_HOME="$TEST_ROOT/tecnix-eval-cache-home"

tecnix_eval_json_cache() {
    local expr
    expr=$(rewrite_tecnix_test_expr "$1")
    XDG_CACHE_HOME="$EVAL_CACHE_HOME" nix eval --json -v \
        --extra-experimental-features 'nix-command' \
        --option lazy-trees true \
        --option tecnix-eval-cache true \
        --pure-eval \
        --expr "$expr"
}

CACHE_WORLD="$TEST_ROOT/tecnix-cache-world"
createGitRepo "$CACHE_WORLD"
(
    cd "$CACHE_WORLD"
    mkdir deps
    echo "alpha dep" > deps/alpha.txt
    echo "beta dep" > deps/beta.txt
    cat > resolve.nix << 'RESOLVE_EOF'
args: {
  allTargetNames = [ "alpha" "beta" ];
  resolve = id: {
    drvPath = "/nix/store/00000000000000000000000000000000-${builtins.hashString "sha256" (builtins.readFile (./deps + "/${id}.txt"))}-${id}.drv";
  };
}
RESOLVE_EOF
    git add -A
    git commit -m "cache world"
)
CACHE_HEAD=$(get_head_sha "$CACHE_WORLD")

cache_args="{ gitDir = \"$CACHE_WORLD/.git\"; resolver = \"resolve.nix\"; args = { system = \"test-system\"; }; rev = \"$CACHE_HEAD\"; }"

cold_deps=$(tecnix_eval_json_cache "tecnixTargetDependencyPathSet (($cache_args) // { targets = [ \"alpha\" \"beta\" ]; })" 2> "$TEST_ROOT/cache-cold.err")
warm_deps=$(tecnix_eval_json_cache "tecnixTargetDependencyPathSet (($cache_args) // { targets = [ \"alpha\" \"beta\" ]; })" 2> "$TEST_ROOT/cache-warm.err")
assert_json_equal "$warm_deps" "$cold_deps" "warm dependency query should equal cold"
assert_jq "$warm_deps" '.alpha | has("deps/alpha.txt")' "cached dependency sets should contain the per-target dep"
grepQuiet "dependency cache hit" "$TEST_ROOT/cache-warm.err"
test -f "$EVAL_CACHE_HOME/nix/tecnix-eval-cache-v1.sqlite"

cold_names=$(tecnix_eval_json_cache "builtins.tecnixTargetNames ($cache_args)" 2> /dev/null)
warm_names=$(tecnix_eval_json_cache "builtins.tecnixTargetNames ($cache_args)" 2> "$TEST_ROOT/cache-warm-names.err")
assert_json_equal "$warm_names" "$cold_names" "warm discovery should equal cold"
grepQuiet "discovery cache hit" "$TEST_ROOT/cache-warm-names.err"

# ============================================================
# Raw-tree contract: git attributes do not filter the Tecnix view
# ============================================================
# The clean backend serves the raw committed tree. An export-ignore rule must
# not hide a file from evaluation: fingerprints describe raw tree objects, so
# a view filtered by .gitattributes would make attribute state an input to
# evaluation that no closure entry certifies — an attribute change could hide
# a file whose blob oid still matches and validate a stale cached result.

EXPORT_IGNORE_WORLD="$TEST_ROOT/tecnix-export-ignore-world"
createGitRepo "$EXPORT_IGNORE_WORLD"
(
    cd "$EXPORT_IGNORE_WORLD"
    echo "secret contents" > secret.txt
    echo "secret.txt export-ignore" > .gitattributes
    cat > resolve.nix << 'RESOLVE_EOF'
args: {
  allTargetNames = [ "reader" ];
  resolve = id: {
    drvPath = "/nix/store/00000000000000000000000000000000-${builtins.hashString "sha256" (builtins.readFile ./secret.txt)}-${id}.drv";
  };
}
RESOLVE_EOF
    git add -A
    git commit -m "export-ignore world"
)
EXPORT_IGNORE_HEAD=$(get_head_sha "$EXPORT_IGNORE_WORLD")

export_ignore_args="{ gitDir = \"$EXPORT_IGNORE_WORLD/.git\"; resolver = \"resolve.nix\"; args = { }; rev = \"$EXPORT_IGNORE_HEAD\"; }"

export_ignore_deps=$(tecnix_eval_json_no_cache "tecnixTargetDependencyPathSet (($export_ignore_args) // { targets = [ \"reader\" ]; })")
assert_jq "$export_ignore_deps" '.reader | has("secret.txt")' \
    "a file with an export-ignore attribute must stay visible to Tecnix evaluation and appear in the closure"
assert_jq "$export_ignore_deps" '.reader."secret.txt" | startswith("git:")' \
    "an export-ignored file should carry an ordinary raw-tree git fingerprint"
assert_jq "$export_ignore_deps" '.reader | has(".gitattributes") | not' \
    "git attributes are inert in the raw-tree view and should not enter the closure"

echo "Tecnix builtin tests passed!"
