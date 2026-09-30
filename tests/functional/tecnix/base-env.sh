#!/usr/bin/env bash
# Every builtin must fit the evaluator's fixed-size base environment. The manual
# build runs `nix __dump-language`, which registers all of them, including the
# ones gated behind experimental features. Too many builtins used to write past
# the end of the environment (a heap buffer overflow only a sanitizer noticed).

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

nix __dump-language > "$TEST_ROOT/language.json"
jq -e 'length > 100' "$TEST_ROOT/language.json" > /dev/null \
    || fail "__dump-language should describe every global builtin"

echo "base environment holds every builtin"
