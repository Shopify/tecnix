#!/usr/bin/env bash
# `tectonix-raw-zone-trees`: zone accessors read the committed Git tree exactly,
# ignoring `.gitattributes` (export-ignore here; LFS smudging is the other
# attribute it disables), so a zone's content hashes to its tree oid.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TEST_WORLD="$TEST_ROOT/world"
create_test_world "$TEST_WORLD"
echo 'secret.txt export-ignore' > "$TEST_WORLD/areas/tools/dev/.gitattributes"
echo 'ignored by git archive' > "$TEST_WORLD/areas/tools/dev/secret.txt"
git -C "$TEST_WORLD" add -A
git -C "$TEST_WORLD" commit -q -m "export-ignore a zone file"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")
TREE_OID=$(git -C "$TEST_WORLD" rev-parse "HEAD:areas/tools/dev")

zoneSrc() {
    tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
        'builtins.unsafeTectonixInternalZoneSrc "//areas/tools/dev"' "$@"
}

# Default: attributes apply, so the export-ignored file is absent.
DEFAULT_SRC=$(zoneSrc)
[[ ! -e "$DEFAULT_SRC/secret.txt" ]] || fail "export-ignore should hide secret.txt by default"
echo "PASS: default zone accessor honors export-ignore"

# Raw trees: the committed blobs exactly, attributes included.
RAW_SRC=$(zoneSrc --option tectonix-raw-zone-trees true)
[[ -f "$RAW_SRC/secret.txt" ]] || fail "raw zone tree should contain secret.txt"
[[ -f "$RAW_SRC/.gitattributes" ]] || fail "raw zone tree should contain .gitattributes"
RAW_HASH=$(nix hash path --extra-experimental-features 'nix-command git-hashing' \
    --mode git --algo sha1 --base16 "$RAW_SRC")
[[ "$RAW_HASH" == "$TREE_OID" ]] || fail "raw zone content hashes to $RAW_HASH, expected tree oid $TREE_OID"
echo "PASS: raw zone content hashes to the zone's tree oid"

echo "All raw zone tree tests passed!"
