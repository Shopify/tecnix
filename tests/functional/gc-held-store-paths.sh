#!/usr/bin/env bash

# An evaluation that is handed an already-valid store path, and goes on
# reading files inside it, must hold a temp root on it. Otherwise a GC
# that runs meanwhile deletes the path and the next read fails with
# "path '...' does not exist". Covered here: a pinned fetchTarball (its
# "already in the store" early exit), builtins.path with a sha256, and
# builtins.storePath.
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

pid=
stopEvaluator() {
    if [[ -n $pid ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}
trap stopEvaluator EXIT

# $1 is an expression for the pin's store path, given the path in $pinPath.
# It is checked with lazy trees on and off.
checkHeld() {
    local pinExpr=$1 lazy
    for lazy in true false; do
        # Something else, another evaluation or a build, put the pin in the
        # store and nothing roots it.
        pinPath=$(nix store add --name pin "$pin")

        rm -f "$TEST_ROOT/pin.err"
        nix eval --impure --raw --option lazy-trees "$lazy" --expr "
          import \"\${${pinExpr//@pinPath@/$pinPath}}/default.nix\" { fifo = $fifo; }" \
          > "$TEST_ROOT/pin.out" 2> "$TEST_ROOT/pin.err" &
        pid=$!

        # The trace comes after the pin's path was returned and before the
        # evaluator blocks opening the FIFO, so GC runs while it holds the pin.
        for i in $(seq 1 600); do
            grepQuiet pin-ready "$TEST_ROOT/pin.err" && break
            if ! kill -0 "$pid" 2>/dev/null || ((i == 600)); then
                cat "$TEST_ROOT/pin.err" >&2
                fail "$pinExpr (lazy-trees $lazy): the evaluator never reported holding the pin"
            fi
            sleep 0.1
        done

        nix-store --gc
        test -e "$pinPath/lib/second.nix" \
            || fail "$pinExpr (lazy-trees $lazy): GC deleted the pin while evaluation held it"

        # Open and close only. builtins.readFile reads a file's st_size
        # bytes, 0 for a FIFO, so the evaluator closes it without reading,
        # and a writer that wrote data could get SIGPIPE.
        : > "$fifo"
        wait "$pid"
        pid=
        [[ $(cat "$TEST_ROOT/pin.out") = "second file read" ]]
    done
}

checkHeld "fetchTarball { name = \"pin\"; url = \"file://$tarball\"; sha256 = \"$hash\"; }"
checkHeld "builtins.path { path = $pin; name = \"pin\"; sha256 = \"$hash\"; }"
checkHeld "builtins.storePath @pinPath@"
