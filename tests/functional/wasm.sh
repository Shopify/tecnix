#!/usr/bin/env bash

source common.sh

if [[ $(nix eval --extra-experimental-features wasm-builtin --expr 'builtins ? wasm') = false ]]; then
    skipTest "builtins.wasm not available"
fi

# Test running a WebAssembly module in text format (WAT).
[[ $(nix eval --json --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { wat = builtins.readFile ./fib.wat; function = \"fib\"; } 40") = 165580141 ]]

# Test running a WebAssembly module in binary format (.wasm).
[[ $(nix eval --json --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { path = ./fib.wasm; function = \"fib\"; } 40") = 165580141 ]]

# A host function called with an out-of-range pointer must fail with an
# error rather than access memory outside the Wasm memory.
expectStderr 1 nix eval --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { wat = builtins.readFile ./oob.wat; function = \"oob\"; } 0" \
    | grepQuiet "Wasm memory access out of bounds"

# Test copying an attrset into Wasm memory with `get_attrset`. The
# attributes must be returned in lexicographically sorted order, both when
# the guest-supplied buffer is used and when the host has to allocate one
# via `nix_wasm_alloc`. The right-hand side of `//` is a layered attrset.
for function in attrs_inline attrs_alloc; do
    [[ $(nix eval --json --impure \
        --extra-experimental-features wasm-builtin \
        --expr "builtins.wasm { wat = builtins.readFile ./attrset.wat; function = \"$function\"; } ({ b = 1; a = 2; } // { \"a b\" = 3; })") = '["a",2,"a b",3,"b",1]' ]]

    [[ $(nix eval --json --impure \
        --extra-experimental-features wasm-builtin \
        --expr "builtins.wasm { wat = builtins.readFile ./attrset.wat; function = \"$function\"; } { }") = '[]' ]]
done

# A `nix_wasm_alloc` export that doesn't have type `(i32, i32) -> i32` must
# be rejected.
expectStderr 1 nix eval --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { wat = builtins.readFile ./bad-alloc.wat; function = \"id\"; } 0" \
    | grepQuiet "does not have type '(size: i32, align: i32) -> i32'"

# A Wasm call on an evaluator fiber must not block the worker thread while
# it waits for a value, since a suspended fiber of the same worker may be
# evaluating it (see `wasm-fiber.nix`). This used to hang.
[[ $(nix eval --json \
    --extra-experimental-features 'wasm-builtin parallel-eval' \
    --option eval-cores 2 --option eval-max-fibers 8 \
    --file ./wasm-fiber.nix wait) = 178 ]]

# Infinite recursion through a Wasm call on a fiber must still be detected,
# rather than hang.
expectStderr 1 nix eval \
    --extra-experimental-features 'wasm-builtin parallel-eval' \
    --option eval-cores 2 \
    --file ./wasm-fiber.nix infiniteRecursion \
    | grepQuiet "infinite recursion encountered"
