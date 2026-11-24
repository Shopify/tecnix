#!/usr/bin/env bash

# A pinned fetchTarball (or fetchurl) whose store path is already valid
# returns that path without copying it, and evaluation goes on reading
# files inside it. It must take a temp root, or a GC that runs meanwhile
# deletes the path and the next read fails with "path '...' does not
# exist".
source common.sh

TODO_NixOS

needLocalStore "the GC test needs a synchronisation point"

clearStore

pin=$TEST_ROOT/pin
mkdir -p "$pin/lib"
# Says it holds the pin, blocks opening the FIFO, then reads a second file
# from the pin.
echo '{ fifo }: builtins.trace "pin-ready" (builtins.seq (builtins.readFile fifo) (import ./lib/second.nix))' > "$pin/default.nix"
echo '"second file read"' > "$pin/lib/second.nix"
tarball=$TEST_ROOT/pin.tar.gz
tar -C "$TEST_ROOT" -czf "$tarball" pin
hash=$(nix hash path --type sha256 --base32 "$pin")

fifo=$TEST_ROOT/pin.fifo
mkfifo "$fifo"

for lazy in true false; do
    # Something else, another evaluation or a build, put the pin in the store
    # and nothing roots it.
    pinPath=$(nix store add --name pin "$pin")

    rm -f "$TEST_ROOT/pin.err"
    nix eval --impure --raw --option lazy-trees "$lazy" --expr "
      import \"\${fetchTarball { name = \"pin\"; url = \"file://$tarball\"; sha256 = \"$hash\"; }}/default.nix\" { fifo = $fifo; }" \
      > "$TEST_ROOT/pin.out" 2> "$TEST_ROOT/pin.err" &
    pid=$!

    # The trace comes after fetchTarball returned the pin and before the
    # evaluator blocks opening the FIFO, so GC runs while it holds the pin.
    for i in $(seq 1 600); do
        grepQuiet pin-ready "$TEST_ROOT/pin.err" && break
        if ((i == 600)); then
            cat "$TEST_ROOT/pin.err" >&2
            fail "the evaluator never reported holding the pin"
        fi
        sleep 0.1
    done

    nix-store --gc
    test -e "$pinPath/lib/second.nix"

    # Open and close only: the evaluator reads nothing, so writing data could
    # hit a closed reader and SIGPIPE.
    : > "$fifo"
    wait "$pid"
    [[ $(cat "$TEST_ROOT/pin.out") = "second file read" ]]
done
