# Wasm calls made on evaluator fibers (see `wasm.sh`).
let
  wasmFib = builtins.wasm {
    path = ./fib.wasm;
    function = "fib";
  };
  sum = n: builtins.foldl' (a: b: a + b) 0 (builtins.genList (i: i) n);
in
{
  # A Wasm call that has to wait for values that suspended fibers of its own
  # worker are evaluating. Run with `eval-cores = 2` and four fibers per
  # worker.
  #
  # The main thread keeps `s` pending while the workers start the six `ts`,
  # which all suspend on it. A worker can't hold more than four fibers, so
  # each worker owns at least two of the `ts` before it starts one of the
  # `ws`, whose Wasm call forces all of the `ts`. If the call blocked its
  # worker thread while waiting, it could never resume the fibers that
  # evaluate them.
  wait =
    let
      ts = builtins.genList (i: s + i) 6;
      ws = builtins.genList (_: wasmFib (builtins.foldl' (a: b: a + b) 0 ts * 0 + 10)) 2;
      s = builtins.parallel (ts ++ ws) (sum 3000000 * 0);
    in
    builtins.seq s (builtins.foldl' (a: b: a + b) 0 ws);

  # A Wasm call on a fiber that forces its own result.
  infiniteRecursion =
    let
      x = wasmFib x;
    in
    builtins.parallel [ x ] (builtins.seq (sum 2000000) x);
}
