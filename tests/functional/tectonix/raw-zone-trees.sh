#!/usr/bin/env bash
# `tectonix-raw-zone-trees`: zone accessors read the committed Git tree exactly,
# ignoring `.gitattributes` (`export-ignore` and LFS smudging), so a zone's
# content hashes to its tree oid.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TEST_WORLD="$TEST_ROOT/world"
create_test_world "$TEST_WORLD"

# One zone hides a file with `export-ignore`.
echo 'secret.txt export-ignore' > "$TEST_WORLD/areas/tools/dev/.gitattributes"
echo 'ignored by git archive' > "$TEST_WORLD/areas/tools/dev/secret.txt"

# Another zone holds an LFS pointer. There is no LFS server here, so a raw read
# succeeds only if it never tries to smudge the pointer.
LFS_OID=$(printf 'hello' | nix hash file --type sha256 --base16 /dev/stdin)
echo '*.bin filter=lfs diff=lfs merge=lfs -text' > "$TEST_WORLD/areas/tools/tec/.gitattributes"
printf 'version https://git-lfs.github.com/spec/v1\noid sha256:%s\nsize 5\n' "$LFS_OID" \
    > "$TEST_WORLD/areas/tools/tec/data.bin"

git -C "$TEST_WORLD" add -A
git -C "$TEST_WORLD" commit -q -m "export-ignore a zone file; add an LFS pointer"
HEAD_SHA=$(get_head_sha "$TEST_WORLD")

zoneSrc() {
    local zone="$1"
    shift
    tectonix_eval "$TEST_WORLD/.git" "$HEAD_SHA" \
        "builtins.unsafeTectonixInternalZoneSrc \"$zone\"" "$@"
}

treeHashMatches() {
    local src="$1" zone_path="$2"
    local expected actual
    expected=$(git -C "$TEST_WORLD" rev-parse "HEAD:$zone_path")
    actual=$(nix hash path --extra-experimental-features 'nix-command git-hashing' \
        --mode git --algo sha1 --base16 "$src")
    [[ "$actual" == "$expected" ]] || fail "$zone_path content hashes to $actual, expected tree oid $expected"
}

# Default: attributes apply, so the export-ignored file is absent.
DEFAULT_SRC=$(zoneSrc //areas/tools/dev)
[[ ! -e "$DEFAULT_SRC/secret.txt" ]] || fail "export-ignore should hide secret.txt by default"
echo "PASS: default zone accessor honors export-ignore"

# Raw trees: the committed blobs exactly, attributes included.
RAW_SRC=$(zoneSrc //areas/tools/dev --option tectonix-raw-zone-trees true)
[[ -f "$RAW_SRC/secret.txt" ]] || fail "raw zone tree should contain secret.txt"
[[ -f "$RAW_SRC/.gitattributes" ]] || fail "raw zone tree should contain .gitattributes"
treeHashMatches "$RAW_SRC" areas/tools/dev
echo "PASS: raw zone content hashes to the zone's tree oid"

# The LFS pointer stays a pointer: no smudge is attempted, and the zone still
# hashes to its tree oid.
RAW_LFS_SRC=$(zoneSrc //areas/tools/tec --option tectonix-raw-zone-trees true)
grep -q "^oid sha256:$LFS_OID\$" "$RAW_LFS_SRC/data.bin" || fail "raw zone tree should keep the LFS pointer"
treeHashMatches "$RAW_LFS_SRC" areas/tools/tec
echo "PASS: raw zone tree keeps LFS pointers unsmudged"

echo "All raw zone tree tests passed!"
