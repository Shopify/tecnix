#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/util/configuration.hh"
#include "nix/util/finally.hh"

namespace nix {

namespace {

/**
 * `__forceOffFiber x`: force `x` through `Executor::runOffFiber()`, like
 * `builtins.wasm` does with the values its host functions force.
 */
void prim_forceOffFiber(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.executor->runOffFiber([&]() { state.forceValue(*args[0], pos); });
    v = *args[0];
}

/**
 * `__blockUntilFinished x`: wait for `x`, which must already be being
 * evaluated by someone else, without ever suspending, i.e. like code that
 * can't be suspended (such as a Wasm call), and return it. This must not
 * block the worker thread of the calling fiber.
 */
void prim_blockUntilFinished(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.executor->runOffFiber([&]() {
        while (!args[0]->isFinished())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    state.forceValue(*args[0], pos);
    v = *args[0];
}

/**
 * `__offFiberProbe x`: what a function run by `Executor::runOffFiber()` sees,
 * compared to its caller.
 */
void prim_offFiberProbe(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto evalThreadIdOutside = myEvalThreadId;
    auto callDepthOutside = CallDepth::callDepth;
    auto osThreadOutside = std::this_thread::get_id();
    auto onWorker = Executor::amWorkerThread;

    uint32_t evalThreadIdInside = 0;
    size_t callDepthInside = 0;
    std::thread::id osThreadInside;
    state.executor->runOffFiber([&]() {
        evalThreadIdInside = myEvalThreadId;
        callDepthInside = CallDepth::callDepth;
        osThreadInside = std::this_thread::get_id();
    });

    auto attrs = state.buildBindings(4);
    attrs.alloc("onWorker").mkBool(onWorker);
    attrs.alloc("otherOsThread").mkBool(osThreadInside != osThreadOutside);
    attrs.alloc("sameEvalThreadId").mkBool(evalThreadIdInside == evalThreadIdOutside);
    attrs.alloc("sameCallDepth").mkBool(callDepthInside == callDepthOutside);
    v.mkAttrs(attrs);
}

/**
 * Run `f`, aborting if it doesn't return within `timeout`: a deadlock would
 * otherwise hang the test run forever.
 */
template<typename F>
auto withDeadline(std::chrono::seconds timeout, std::string_view what, F && f)
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread watchdog([&]() {
        std::unique_lock lk(mutex);
        if (!cv.wait_for(lk, timeout, [&]() { return done; })) {
            std::cerr << "deadlock: " << what << " did not finish within " << timeout.count() << " s\n";
            std::abort();
        }
    });
    Finally stopWatchdog([&]() {
        {
            std::lock_guard lk(mutex);
            done = true;
        }
        cv.notify_one();
        watchdog.join();
    });
    return f();
}

/**
 * fib(n) in Wasm, as in `tests/functional/fib.wat`.
 */
constexpr std::string_view fibWat = R"(
(module
  (import "env" "get_int" (func $get_int (param i32) (result i64)))
  (import "env" "make_int" (func $make_int (param i64) (result i32)))
  (memory (export "memory") 1)
  (func (export "nix_wasm_init_v1"))
  (func $fib (param $n i64) (result i64)
    (if (i64.le_s (local.get $n) (i64.const 1)) (then (return (i64.const 1))))
    (i64.add (call $fib (i64.sub (local.get $n) (i64.const 1)))
             (call $fib (i64.sub (local.get $n) (i64.const 2)))))
  (func (export "fib") (param $arg i32) (result i32)
    (call $make_int (call $fib (call $get_int (local.get $arg))))))
)";

/**
 * The deadlock pattern: the main thread keeps `s` pending while the workers
 * start the six `ts`, which all suspend on it. A worker can't hold more than
 * four fibers, so each worker owns at least two of the `ts` before it starts
 * one of the two `ws`, which waits for each of the `ts` in turn with
 * `waitFor` (a function that waits without suspending, and returns its
 * argument's value).
 */
std::string deadlockPattern(std::string_view waitFor)
{
    return fmt(
        R"(
          let
            waitFor = %s;
            sum = n: builtins.foldl' (a: b: a + b) 0 (builtins.genList (i: i) n);
            ts = builtins.genList (i: s + i) 6;
            ws = builtins.genList (_: builtins.foldl' (a: t: a + waitFor t) 0 ts) 2;
            s = builtins.parallel (ts ++ ws) (sum 3000000 * 0);
          in
          builtins.seq s (builtins.foldl' (a: b: a + b) 0 ws)
        )",
        waitFor);
}

/**
 * Evaluate `x` on a worker fiber: spawn it, keep the main thread busy for a
 * while, then force it.
 */
std::string onFiber(std::string_view x)
{
    return fmt(
        "let x = %s; in builtins.parallel [ x ] (builtins.seq (builtins.foldl' (a: b: a + b) 0 (builtins.genList (i: i) 2000000)) x)",
        x);
}

} // namespace

/**
 * Parallel evaluation with two worker threads and four fibers per worker, and
 * the test primops above.
 */
class ParallelEvalTest : public LibExprTest
{
public:
    ParallelEvalTest()
        : LibExprTest(openStore("dummy://"), [](bool & readOnlyMode) {
            /* Experimental primops are only registered if their feature is
               enabled when the `EvalState` is created. */
            savedFeatures = experimentalFeatureSettings.experimentalFeatures.get();
            experimentalFeatureSettings.set("extra-experimental-features", "parallel-eval wasm-builtin");
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            settings.evalCores = 2;
            settings.evalMaxFibers = 8;
            settings.extraPrimOps.push_back({.name = "__forceOffFiber", .args = {"x"}, .impl = prim_forceOffFiber});
            settings.extraPrimOps.push_back(
                {.name = "__blockUntilFinished", .args = {"x"}, .impl = prim_blockUntilFinished});
            settings.extraPrimOps.push_back({.name = "__offFiberProbe", .args = {"x"}, .impl = prim_offFiberProbe});
            return settings;
        })
    {
    }

    ~ParallelEvalTest()
    {
        experimentalFeatureSettings.experimentalFeatures = savedFeatures;
    }

    Value evalWithDeadline(std::string input)
    {
        return withDeadline(std::chrono::seconds(60), "the evaluation", [&]() { return eval(input); });
    }

    static inline std::set<ExperimentalFeature> savedFeatures;
};

TEST_F(ParallelEvalTest, offFiberCodeMayWaitForSuspendedFibersOfItsWorker)
{
    auto v = evalWithDeadline(deadlockPattern("builtins.blockUntilFinished"));
    // Each `ws` item is the sum of the `ts`, i.e. 0 + 1 + ... + 5, since `s = 0`.
    ASSERT_EQ(state.forceInt(v, noPos, "").value, 2 * 15);
    EXPECT_GT(state.executor->nrOffFiberCalls.load(), 0u);
}

TEST_F(ParallelEvalTest, wasmCallMayWaitForSuspendedFibersOfItsWorker)
{
    if (!eval("builtins ? wasm").boolean())
        GTEST_SKIP() << "builtins.wasm is not available";
    auto wasmFib = fmt("x: builtins.wasm { wat = ''%s''; function = \"fib\"; } (x * 0 + 10)", fibWat);
    auto v = evalWithDeadline(deadlockPattern(wasmFib));
    // fib 10 = 89, once per `ts` item and `ws` item.
    ASSERT_EQ(state.forceInt(v, noPos, "").value, 2 * 6 * 89);
}

TEST_F(ParallelEvalTest, offFiberCodeRunsWithTheIdentityOfItsFiber)
{
    auto v = evalWithDeadline(onFiber("builtins.offFiberProbe null"));
    state.forceAttrs(v, noPos, "");
    auto get = [&](const char * name) {
        auto attr = v.attrs()->get(createSymbol(name));
        assert(attr);
        return state.forceBool(*attr->value, noPos, "");
    };
    ASSERT_TRUE(get("onWorker"));
    EXPECT_TRUE(get("otherOsThread"));
    EXPECT_TRUE(get("sameEvalThreadId"));
    EXPECT_TRUE(get("sameCallDepth"));
}

TEST_F(ParallelEvalTest, offFiberCodeRunsInlineOffFiber)
{
    auto v = evalWithDeadline("builtins.offFiberProbe null");
    state.forceAttrs(v, noPos, "");
    EXPECT_FALSE(state.forceBool(*v.attrs()->get(createSymbol("onWorker"))->value, noPos, ""));
    EXPECT_FALSE(state.forceBool(*v.attrs()->get(createSymbol("otherOsThread"))->value, noPos, ""));
}

TEST_F(ParallelEvalTest, infiniteRecursionThroughOffFiberCodeIsReported)
{
    EXPECT_THROW(evalWithDeadline(onFiber("let y = builtins.forceOffFiber y; in y")), InfiniteRecursionError);
}

TEST_F(ParallelEvalTest, offFiberCodeThrowsIntoItsFiber)
{
    EXPECT_THROW(evalWithDeadline(onFiber("builtins.forceOffFiber (throw \"boom\")")), ThrownError);
}

} // namespace nix
