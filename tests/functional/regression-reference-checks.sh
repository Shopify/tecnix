#!/usr/bin/env bash

source common.sh

TODO_NixOS

clearStore

outpath="$(nix-build regression-reference-checks.nix -A out --no-out-link)"
nix-build regression-reference-checks.nix -A man --no-out-link

nix-store --delete "$outpath"
nix-build regression-reference-checks.nix -A out --no-out-link
