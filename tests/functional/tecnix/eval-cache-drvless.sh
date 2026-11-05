#!/usr/bin/env bash
# Tests for `evalCacheDrvlessSubstituters`: an eval-cache hit whose drv is not
# in the local store, but in one of the binary caches the caller names, is
# served without its drv.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

requireSqlite3() {
    if ! command -v sqlite3 > /dev/null; then
        skipTest "sqlite3 is needed to inspect the eval cache"
    fi
}
requireSqlite3

assert_jq() {
    if ! jq -e "$2" > /dev/null <<< "$1"; then
        echo "$1" >&2
        fail "$3"
    fi
}

assert_json_equal() {
    if ! diff -u <(jq -S . <<< "$2") <(jq -S . <<< "$1"); then
        fail "$3"
    fi
}

WORLD="$TEST_ROOT/drvless-world"
createGitRepo "$WORLD"
(
    cd "$WORLD"
    echo "echo alpha" > alpha.txt
    echo "echo beta" > beta.txt
    echo "echo gamma" > gamma.txt
    # The trace shows whether the resolver ran: a served hit never runs it.
    cat > resolve.nix << 'RESOLVE_EOF'
args: builtins.trace "drvless-resolver-evaluated" {
  allTargetNames = [ "alpha" "beta" "gamma" ];
  resolve = id:
    let
      drv = derivation {
        name = "drvless-${id}";
        system = "test-system";
        builder = "/bin/sh";
        args = [ "-c" (builtins.readFile (./. + "/${id}.txt")) ];
        outputs = [ "out" "dev" ];
      };
    in if id == "beta" then drv.dev
    # Like a floating content-addressed output: no store path to cache.
    else if id == "gamma" then drv.out // { outPath = "/placeholder-gamma"; }
    else drv.out;
}
RESOLVE_EOF
    git add -A
    git -c user.email=test@example.com -c user.name=Test commit -qm "drvless world"
)
HEAD=$(get_head_sha "$WORLD")

CACHE_HOME="$TEST_ROOT/drvless-eval-cache-home"
BINARY_CACHE="file://$TEST_ROOT/drvless-binary-cache"
EMPTY_CACHE="file://$TEST_ROOT/drvless-empty-cache"
CORRUPT_CACHE="file://$TEST_ROOT/drvless-corrupt-cache"
nix copy --to "$EMPTY_CACHE" "$(nix-store --add "$WORLD/alpha.txt")"

# `$1`: the list of drvless substituters, as Nix; `$2`: what to return for
# each target, given `t`; `$3`: the targets, as Nix. Lookups need `substitute`,
# which Nix turns off when it finds no Internet access, as in a build sandbox;
# `$SUBSTITUTE` overrides it.
eval_cached() {
    XDG_CACHE_HOME="$CACHE_HOME" nix eval --json -v \
        --extra-experimental-features 'nix-command' \
        --option substitute "${SUBSTITUTE:-true}" \
        --option lazy-trees true \
        --option tecnix-eval-cache true \
        --pure-eval \
        --expr "builtins.mapAttrs (_: t: $2) (builtins.tecnixTargets { gitDir = \"$WORLD/.git\"; resolver = \"resolve.nix\"; args = { }; rev = \"$HEAD\"; targets = ${3:-[ \"alpha\" \"beta\" ]}; evalCacheDrvlessSubstituters = $1; })"
}

# What `tec build` and `tec tartarus plan` read, and the string contexts.
# shellcheck disable=SC2016 # Nix, not shell
projection='{ drv = t.drvPath; out = t.outPath; outputName = t.outputName; name = t.name; type = t.type; selected = t.${t.outputName}.outPath; drvContext = builtins.getContext t.drvPath; outputContext = builtins.getContext t.outPath; }'

echo "Testing the payload of a target miss..."
cold=$(eval_cached '[ ]' "$projection" 2> "$TEST_ROOT/drvless-cold.err")
grepQuiet "drvless-resolver-evaluated" "$TEST_ROOT/drvless-cold.err"
assert_jq "$cold" '.alpha.outputName == "out" and .beta.outputName == "dev" and (.beta.out | endswith("-drvless-beta-dev"))' \
    "the fixture should select different outputs"
cache_db=("$CACHE_HOME"/nix/tecnix-eval-cache-*.sqlite)
[[ $(sqlite3 "${cache_db[0]}" "SELECT count(*) FROM DependencyShards WHERE instr(CAST(dependencies AS TEXT), 'TXTV2') > 0") -gt 0 ]] \
    || fail "a miss should store a TXTV2 payload"

echo "Testing a hit whose drv is local..."
warm=$(eval_cached '[ ]' "$projection" 2> "$TEST_ROOT/drvless-warm.err")
assert_json_equal "$warm" "$cold" "a hit should give the evaluated value's paths, name and contexts"
grepQuiet "tecnixTargets: 2 target value(s) served from the cache" "$TEST_ROOT/drvless-warm.err"
grepQuietInverse "without their drv" "$TEST_ROOT/drvless-warm.err"
grepQuietInverse "drvless-resolver-evaluated" "$TEST_ROOT/drvless-warm.err"

echo "Testing that a hit without drvless substituters keeps the imported drv's shape..."
# The imported drv has every output and `all`; a payload-built value has neither.
# shellcheck disable=SC2016 # Nix, not shell
siblings='{ all = map (o: o.outPath) t.all; out = t.out.outPath; dev = t.dev.outPath; }'
warm_siblings=$(eval_cached '[ ]' "$siblings" 2> "$TEST_ROOT/drvless-warm-siblings.err")
grepQuiet "tecnixTargets: 2 target value(s) served from the cache" "$TEST_ROOT/drvless-warm-siblings.err"
grepQuietInverse "drvless-resolver-evaluated" "$TEST_ROOT/drvless-warm-siblings.err"
assert_jq "{\"w\": $warm_siblings, \"c\": $cold}" \
    '(.w.alpha.all | sort) == ([.w.alpha.out, .w.alpha.dev] | sort) and .w.alpha.out == .c.alpha.out
     and (.w.beta.all | sort) == ([.w.beta.out, .w.beta.dev] | sort) and .w.beta.dev == .c.beta.out' \
    "a hit without drvless substituters should import its drv"

alpha_drv=$(jq -r .alpha.drv <<< "$cold")
beta_drv=$(jq -r .beta.drv <<< "$cold")
nix copy --to "$BINARY_CACHE" "$alpha_drv" "$beta_drv"
# A cache that answers with an unreadable narinfo: an error, not a not-found.
nix copy --to "$CORRUPT_CACHE" "$alpha_drv" "$beta_drv"
for narinfo in "$TEST_ROOT"/drvless-corrupt-cache/*.narinfo; do
    echo "garbage" > "$narinfo"
done

# Deleting needs a trusted user (`--ignore-liveness`), so outside NixOS it opens
# the store directly, which also works while an untrusting daemon runs.
store_cmd() {
    if isTestOnNixOS; then "$@"; else NIX_REMOTE='' "$@"; fi
}

delete_drvs() {
    store_cmd nix-store --delete "$alpha_drv" "$beta_drv" --ignore-liveness > /dev/null 2>&1
    ! store_cmd nix-store --check-validity "$alpha_drv" 2> /dev/null || fail "alpha's drv should be gone"
    ! store_cmd nix-store --check-validity "$beta_drv" 2> /dev/null || fail "beta's drv should be gone"
}

# The narinfo caches of the evaluator and the daemon, so each lookup asks.
clear_narinfo_caches() {
    clearCacheCache
    rm -f "$CACHE_HOME"/nix/binary-cache*
}

# Whether the store asks the caches a client names: a local store does, and so
# does a daemon that trusts the user. One that does not, as on NixOS for a normal
# user, drops each cache it does not list itself, so every drv is a miss.
store_trusts_us() {
    [[ $(nix store info --json | jq '.trusted != false') == true ]]
}

# A hit whose drv one of the substituters has is served without the drv, which
# stays out of the store.
expect_drvless_hits() {
    local substituters="$1" err="$2"
    clear_narinfo_caches
    delete_drvs
    local value
    value=$(eval_cached "$substituters" "$projection" 2> "$err")
    assert_json_equal "$value" "$cold" "a drvless hit should give the evaluated value's paths, name and contexts"
    grepQuiet "tecnixTargets: 2 target value(s) served from the cache" "$err"
    grepQuiet "tecnixTargets: 2 target value(s) served from the cache without their drv, which a drvless substituter has" "$err"
    grepQuietInverse "drvless-resolver-evaluated" "$err"
    ! store_cmd nix-store --check-validity "$alpha_drv" 2> /dev/null || fail "a drvless hit should not substitute its drv"
}

# A hit whose drv no substituter has is an ordinary miss, evaluated again.
expect_misses() {
    local substituters="$1" err="$2"
    clear_narinfo_caches
    delete_drvs
    local value
    value=$(eval_cached "$substituters" "$projection" 2> "$err")
    assert_json_equal "$value" "$cold" "a miss should evaluate the target again"
    grepQuiet "dependency cache hit for 'alpha' has no valid target value, evaluating" "$err"
    grepQuiet "drvless-resolver-evaluated" "$err"
}

run_lookups() {
    if store_trusts_us; then
        echo "Testing drvless hits ($1)..."
        expect_drvless_hits "[ \"$BINARY_CACHE\" ]" "$TEST_ROOT/drvless-hit-$1.err"
        # Caches are asked in priority order, and a cache without the drv is no error.
        expect_drvless_hits "[ \"$EMPTY_CACHE\" \"$BINARY_CACHE\" ]" "$TEST_ROOT/drvless-hit-second-$1.err"
    else
        echo "Testing that an untrusting daemon drops the drvless substituters ($1)..."
        expect_misses "[ \"$BINARY_CACHE\" ]" "$TEST_ROOT/drvless-untrusted-$1.err"
        grepQuiet "ignoring untrusted substituter" "$TEST_ROOT/drvless-untrusted-$1.err"
    fi

    echo "Testing drvless misses ($1)..."
    expect_misses '[ ]' "$TEST_ROOT/drvless-none-$1.err"
    expect_misses "[ \"$EMPTY_CACHE\" ]" "$TEST_ROOT/drvless-empty-$1.err"
    # A cache that fails fails the lookup, and every drv is a miss.
    expect_misses "[ \"$CORRUPT_CACHE\" ]" "$TEST_ROOT/drvless-broken-$1.err"
    if store_trusts_us; then
        grepQuiet "could not ask the drvless substituters" "$TEST_ROOT/drvless-broken-$1.err"
    fi

    # Without `substitute`, no cache is opened or asked.
    SUBSTITUTE=false expect_misses '[ "http://127.0.0.1:1" ]' "$TEST_ROOT/drvless-no-substitute-$1.err"
    grepQuietInverse "nix-cache-info" "$TEST_ROOT/drvless-no-substitute-$1.err"
    grepQuietInverse "could not ask the drvless substituters" "$TEST_ROOT/drvless-no-substitute-$1.err"
}

run_lookups local

echo "Testing that only binary caches are accepted..."
# Opening an ssh-ng store runs its remote-program.
for uri in "ssh-ng://localhost?remote-program=touch%20$TEST_ROOT/drvless-ran" "ssh://localhost" daemon local "unix://$TEST_ROOT/socket"; do
    expectStderr 1 eval_cached "[ \"$uri\" ]" 't.drvPath' | grepQuiet "takes only binary caches"
done
[[ ! -e "$TEST_ROOT/drvless-ran" ]] || fail "a rejected store must not be opened"

if store_trusts_us; then
    echo "Testing that a value depending on a drvless hit fails loudly..."
    clear_narinfo_caches
    delete_drvs
    dependent="derivation { name = \"dependent\"; system = \"test-system\"; builder = \"/bin/sh\"; args = [ t.outPath ]; }"
    expectStderr 1 eval_cached "[ \"$BINARY_CACHE\" ]" "($dependent).drvPath" | grepQuiet "drvless-alpha.drv"
fi

echo "Testing that a TXTV1 payload is served only with its drv local..."
# A value whose outPath is no store path gets the payload without it.
gamma=$(eval_cached '[ ]' 't.drvPath' '[ "gamma" ]' 2> "$TEST_ROOT/drvless-v1-cold.err")
gamma_drv=$(jq -r .gamma <<< "$gamma")
grepQuiet "drvless-resolver-evaluated" "$TEST_ROOT/drvless-v1-cold.err"
[[ $(sqlite3 "${cache_db[0]}" "SELECT count(*) FROM DependencyShards WHERE instr(CAST(dependencies AS TEXT), 'TXTV1') > 0") -gt 0 ]] \
    || fail "a miss without a store outPath should store a TXTV1 payload"
eval_cached '[ ]' 't.drvPath' '[ "gamma" ]' > /dev/null 2> "$TEST_ROOT/drvless-v1-local.err"
grepQuiet "tecnixTargets: 1 target value(s) served from the cache" "$TEST_ROOT/drvless-v1-local.err"
nix copy --to "$BINARY_CACHE" "$gamma_drv"
store_cmd nix-store --delete "$gamma_drv" --ignore-liveness > /dev/null 2>&1
clear_narinfo_caches
eval_cached "[ \"$BINARY_CACHE\" ]" 't.drvPath' '[ "gamma" ]' > /dev/null 2> "$TEST_ROOT/drvless-v1-remote.err"
grepQuiet "dependency cache hit for 'gamma' has no valid target value, evaluating" "$TEST_ROOT/drvless-v1-remote.err"

if ! isTestOnNixOS; then
    # Through the daemon: the lookup sets the substituters on a connection of
    # its own, so the daemon asks them though its own `substituters` is empty.
    startDaemon
    run_lookups daemon

    echo "Testing that the daemon drops substituters it does not trust..."
    killDaemon
    cp "$NIX_CONF_DIR/nix.conf" "$TEST_ROOT/nix.conf.saved"
    sed -i 's/^trusted-users = .*/trusted-users =/' "$NIX_CONF_DIR/nix.conf"
    echo "trusted-substituters = $BINARY_CACHE" >> "$NIX_CONF_DIR/nix.conf"
    startDaemon
    expect_drvless_hits "[ \"$BINARY_CACHE\" ]" "$TEST_ROOT/drvless-trusted.err"
    expect_misses "[ \"$BINARY_CACHE?priority=10\" ]" "$TEST_ROOT/drvless-untrusted.err"
    grepQuiet "ignoring untrusted substituter" "$TEST_ROOT/drvless-untrusted.err"
    killDaemon
    cp "$TEST_ROOT/nix.conf.saved" "$NIX_CONF_DIR/nix.conf"
fi
