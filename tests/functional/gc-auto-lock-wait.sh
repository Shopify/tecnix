#!/usr/bin/env bash

# Test that an auto-GC which had to wait for the GC lock checks free
# space again before deleting anything. Other collections may have
# freed enough while it waited.
source common.sh

# shellcheck disable=SC1111
needLocalStore "“min-free” and “max-free” are daemon options, and the GC test needs a synchronisation point"

TODO_NixOS

clearStore

garbage=$(nix store add-path --name garbage ./nar-access.sh)

# Hold the GC lock with a collection that deletes nothing. This FIFO
# is read just after the roots have been read, so the root server is
# up and other processes can still register temporary roots.
fifo=$TEST_ROOT/gc-sync.fifo
mkfifo "$fifo"
_NIX_TEST_GC_SYNC_2=$fifo nix-store --gc --print-dead > /dev/null &
pid1=$!

# Opening the FIFO for writing blocks until the collector opens it,
# that is, until it holds the lock.
exec 3> "$fifo"

# Pretend that we are short of space, and add a path, which runs an
# auto-GC and waits for it.
fake_free=$TEST_ROOT/fake-free
echo 100 > "$fake_free"

# The collector only resumes once every writer has closed the FIFO, so
# don't let this one inherit it.
echo "trigger $RANDOM" > "$TEST_ROOT/trigger"
log=$TEST_ROOT/trigger.log
_NIX_TEST_FREE_SPACE_FILE=$fake_free nix store add-path --name trigger "$TEST_ROOT/trigger" \
    --min-free 1K --max-free 2K 2> "$log" 3>&- &
pid2=$!

for _ in {1..60}; do
    if grepQuiet "waiting for the big garbage collector lock" "$log"; then
        break
    fi
    sleep 1
done
cat "$log"
grepQuiet "waiting for the big garbage collector lock" "$log"

# While the auto-GC waits, enough space becomes free.
echo 10000 > "$fake_free.tmp"
mv "$fake_free.tmp" "$fake_free"

# Release the lock.
exec 3>&-
wait "$pid1"
wait "$pid2"

cat "$log"
grepQuiet "skipping auto-GC: 10000 bytes free after waiting for the GC lock" "$log"
test -e "$garbage"
