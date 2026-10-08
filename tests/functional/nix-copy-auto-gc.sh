#!/usr/bin/env bash

source common.sh

# A `nix copy` into the daemon must not deadlock with an auto-GC that the
# daemon starts while it is still receiving the paths. The GC logs every path
# it deletes to the client over the same socket the client is sending on, so
# once both socket buffers were full, each side used to wait on the other
# forever.

TODO_NixOS

clearStore

# Far more garbage than fits in a socket buffer once the daemon has logged
# "deleting '<path>'" for each one.
longName=$(printf 'g%.0s' {1..150})
nix eval --expr "
  builtins.deepSeq (builtins.genList (i: builtins.toFile \"$longName-\${toString i}\" (toString i)) 2000) true
" > /dev/null
[[ $(find "$NIX_STORE_DIR" -maxdepth 1 -name "*-$longName-*" | wc -l) -eq 2000 ]]

# A path far bigger than a socket buffer, in a binary cache but not in the
# store, so that it is still being streamed into the daemon when the GC starts.
head -c 20000000 /dev/zero > "$TEST_ROOT/big"
bigPath=$(nix store add-path --name big "$TEST_ROOT/big")
nix copy --to "file://$TEST_ROOT/cache" "$bigPath"
nix-store --delete "$bigPath"

# Make the daemon think it is nearly out of space.
export _NIX_TEST_FREE_SPACE_FILE=$TEST_ROOT/fake-free
echo 100 > "$_NIX_TEST_FREE_SPACE_FILE"
cat >> "$test_nix_conf" <<EOF
min-free = 1M
max-free = 1G
min-free-check-interval = 1
EOF

startDaemon

# A client stuck in write() ignores SIGTERM, so follow up with SIGKILL.
timeout -k 5 120 nix copy --no-check-sigs --from "file://$TEST_ROOT/cache" "$bigPath"

nix path-info "$bigPath"
[[ $(find "$NIX_STORE_DIR" -maxdepth 1 -name "*-$longName-*" | wc -l) -eq 0 ]]
