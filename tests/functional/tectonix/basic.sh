#!/usr/bin/env bash
# Basic tectonix functionality tests

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Create test world
TEST_WORLD="$TEST_ROOT/world"
create_test_world "$TEST_WORLD"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

echo "Testing basic zone access..."

# Test: Manifest access
manifest=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalManifest')
echo "Manifest: $manifest"

# Verify manifest contains expected zones
echo "$manifest" | grepQuiet "//areas/tools/dev"
echo "$manifest" | grepQuiet "W-000001"

# Test: Inverted manifest
inverted=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalManifestInverted')
echo "Inverted manifest: $inverted"

echo "$inverted" | grepQuiet "W-000001"
echo "$inverted" | grepQuiet "//areas/tools/dev"

# Test: Tree SHA access
tree_sha=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalTreeSha "//areas/tools/dev"')
echo "Tree SHA for //areas/tools/dev: $tree_sha"

# Verify SHA is 40 hex characters
if [[ ! "$tree_sha" =~ ^[0-9a-f]{40}$ ]]; then
    fail "Tree SHA should be 40 hex characters, got: $tree_sha"
fi

# Test: Zone source access
zone_src=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrc "//areas/tools/dev"')
echo "Zone source path: $zone_src"

# Verify it's a store path
if [[ ! "$zone_src" =~ ^${NIX_STORE_DIR:-/nix/store}/ ]]; then
    fail "Zone source should be a store path, got: $zone_src"
fi

# Test: Zone root access
zone_root=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneRoot "//areas/tools/dev"' \
    --option tectonix-checkout-path "$TEST_WORLD" \
    --no-pure-eval)
echo "Zone root: $zone_root"
[[ -n "$zone_root" ]] || fail "Zone root should not be empty"

# Verify zone root points into the test world
if [[ ! "$zone_root" =~ "$TEST_WORLD" ]]; then
    fail "Zone root should reference test world, got: $zone_root"
fi

# Test: Zone is not dirty in clean repo
zone_is_dirty=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneIsDirty "//areas/tools/dev"' \
    --option tectonix-checkout-path "$TEST_WORLD")
echo "Zone dirty: $zone_is_dirty"

if [[ "$zone_is_dirty" == "true" ]]; then
    fail "Zone should not be dirty in clean repo"
fi

# ==================================================================
# Zone source subpath tests
# ==================================================================
echo "Testing zone source subpath access..."

# Test: Subpath for a single file
subpath_src=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrcSubpath "//areas/tools/dev" "zone.nix"')
echo "Subpath source (file): $subpath_src"

# Verify it's a store path
if [[ ! "$subpath_src" =~ ^${NIX_STORE_DIR:-/nix/store}/ ]]; then
    fail "Subpath source should be a store path, got: $subpath_src"
fi

# Verify subpath result differs from full zone source
if [[ "$subpath_src" == "$zone_src" ]]; then
    fail "Subpath source should differ from full zone source"
fi

# Test: Subpath for a subdirectory
subpath_dir_src=$(tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrcSubpath "//areas/tools/dev" "src"')
echo "Subpath source (dir): $subpath_dir_src"

if [[ ! "$subpath_dir_src" =~ ^${NIX_STORE_DIR:-/nix/store}/ ]]; then
    fail "Subpath dir source should be a store path, got: $subpath_dir_src"
fi

# Test: Invalid subpath - empty string
echo "Testing invalid subpath: empty string..."
expect_failure tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrcSubpath "//areas/tools/dev" ""'

# Test: Invalid subpath - absolute path
echo "Testing invalid subpath: absolute path..."
expect_failure tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrcSubpath "//areas/tools/dev" "/zone.nix"'

# Test: Invalid subpath - parent traversal
echo "Testing invalid subpath: parent traversal..."
expect_failure tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrcSubpath "//areas/tools/dev" "../tools/tec/zone.nix"'

# Test: Invalid subpath - nonexistent path
echo "Testing invalid subpath: nonexistent..."
expect_failure tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneSrcSubpath "//areas/tools/dev" "nonexistent.nix"'

echo "Basic tests passed!"
