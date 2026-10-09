#!/usr/bin/env bash
# tecnix-drv-hash-cache: a derivation's hash modulo, once computed, is reused
# by later processes instead of reading its closure again.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

export XDG_CACHE_HOME="$TEST_ROOT/drv-hash-cache-home"

clearStore

dep='derivation { name = "dep"; system = "x"; builder = "/bin/sh"; args = [ "-c" "dep" ]; }'

# First process: instantiates dep and a derivation on top of it, so dep's hash
# is computed and stored.
depDrv=$(nix-instantiate --expr "$dep")
depOut=$(nix eval --raw --expr "($dep).outPath")
nix-instantiate --expr "derivation { name = \"top-one\"; system = \"x\"; builder = \"/bin/sh\"; args = [ \"-c\" \"\${$dep}\" ]; }" > /dev/null

# A later process names dep only by its paths (appendContext reads no
# derivation), so hashing a new derivation on top of dep needs dep's hash
# modulo: from the cache, or by reading dep.drv.
top() {
    nix-instantiate "${@:2}" --expr "derivation {
      name = \"top-$1\"; system = \"x\"; builder = \"/bin/sh\";
      args = [ \"-c\" (builtins.appendContext \"$depOut\" { \"$depDrv\" = { outputs = [ \"out\" ]; }; }) ];
    }"
}

# Make dep.drv unreadable to prove the cache answers.
chmod u+w "$(dirname "$depDrv")"
chmod 000 "$depDrv"
top2=$(top two) || { chmod 444 "$depDrv"; fail "instantiation should not need to read dep.drv"; }

# With the cache off, it has to read the derivation, and can't.
expectStderr 1 top three --option tecnix-drv-hash-cache false | grepQuiet "dep.drv"

# The reused hash gives exactly the derivation a fresh computation gives.
chmod 444 "$depDrv"
fresh=$(top two --option tecnix-drv-hash-cache false)
[[ "$fresh" == "$top2" ]] || fail "a reused hash should give the same derivation ($fresh vs $top2)"
