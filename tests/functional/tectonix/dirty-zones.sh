#!/usr/bin/env bash
# Test dirty zone detection

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Create test world
TEST_WORLD="$TEST_ROOT/world"
create_test_world "$TEST_WORLD"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

echo "Testing dirty zone detection..."

# First, verify zone is clean
zone_is_dirty=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneIsDirty "//areas/tools/dev"' \
    --option tectonix-checkout-path "$TEST_WORLD")
echo "Clean zone status: $zone_is_dirty"

# Extract dirty status (should be false)
if [[ "$zone_is_dirty" == 'true' ]]; then
    fail "Zone should be clean before modification"
fi

# Modify a file in the zone
echo "Modified content" >> "$TEST_WORLD/areas/tools/dev/zone.nix"

# Now check dirty status
zone_is_dirty_after=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneIsDirty "//areas/tools/dev"' \
    --option tectonix-checkout-path "$TEST_WORLD")
echo "Dirty zone dirty status: $zone_is_dirty_after"

# dirty should now be true
if [[ "$zone_is_dirty_after" != "true" ]]; then
    fail "Zone should be dirty after modification"
fi

# Check dirtyZones builtin
dirty_zones=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalDirtyZones' \
    --option tectonix-checkout-path "$TEST_WORLD")
echo "Dirty zones: $dirty_zones"

# Verify the modified zone appears as dirty
if ! echo "$dirty_zones" | grepQuiet "//areas/tools/dev"; then
    fail "Modified zone should appear in dirtyZones"
fi

# Verify an unmodified zone is not dirty
clean_zone_dirty=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" \
    'builtins.unsafeTectonixInternalZoneIsDirty "//areas/tools/tec"' \
    --option tectonix-checkout-path "$TEST_WORLD")
echo "Unmodified zone dirty status: $clean_zone_dirty"

if [[ "$clean_zone_dirty" == "true" ]]; then
    fail "Unmodified zone should not be dirty"
fi

# A dirty zone's source is the commit's tree with the checkout's changes over
# it: modified files, untracked files, and files in untracked directories (git
# status collapses those to one `dir/` entry unless asked to list them), but
# not ignored files.
mkdir -p "$TEST_WORLD/areas/tools/dev/newdir/deeper" "$TEST_WORLD/areas/tools/dev/build-out"
echo "in an untracked dir" > "$TEST_WORLD/areas/tools/dev/newdir/deeper/x.txt"
echo "untracked file" > "$TEST_WORLD/areas/tools/dev/new.txt"
echo "ignored" > "$TEST_WORLD/areas/tools/dev/build-out/junk"
echo "/areas/tools/dev/build-out/" >> "$TEST_WORLD/.git/info/exclude"

zone_src_expr='let src = builtins.unsafeTectonixInternalZoneSrc "//areas/tools/dev"; in {
  modified = builtins.readFile "${src}/zone.nix";
  untrackedFile = builtins.readFile "${src}/new.txt";
  untrackedDir = builtins.readFile "${src}/newdir/deeper/x.txt";
  ignored = builtins.pathExists "${src}/build-out/junk";
}'
zone_src=$(tectonix_eval_json "$TEST_WORLD/.git" "$HEAD_SHA" "$zone_src_expr" \
    --option tectonix-checkout-path "$TEST_WORLD")
echo "Dirty zone source: $zone_src"

echo "$zone_src" | grepQuiet '"modified":"[^"]*Modified content' || fail "zone source should carry the modified file"
echo "$zone_src" | grepQuiet '"untrackedFile":"untracked file' || fail "zone source should carry an untracked file"
echo "$zone_src" | grepQuiet '"untrackedDir":"in an untracked dir' || fail "zone source should carry files in an untracked directory"
echo "$zone_src" | grepQuiet '"ignored":false' || fail "zone source should not carry ignored files"

echo "Dirty zone tests passed!"
