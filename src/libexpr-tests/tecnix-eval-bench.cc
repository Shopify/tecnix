#include <benchmark/benchmark.h>

#include <fstream>
#include <map>

#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"
#include "nix/util/processes.hh"
#include "nix/util/strings.hh"

using namespace nix;

namespace {

/**
 * A synthetic Tecnix world: `targetCount` targets whose source closures each
 * contain ~20 paths (own target file, a per-target leaf, 15 shared libs, the
 * resolver, and the target index). Shared libs exercise label inheritance and
 * the pair-union cache; per-target files exercise first-sight interning and
 * per-path fingerprinting at scale.
 *
 * Targets are spread over a deep, narrow directory hierarchy (~25 entries per
 * directory), matching real monorepo geometry (measured median directory
 * width 2, p99 44). Directory shape matters: git tree objects over ~4 KiB are
 * outside libgit2's default object cache, so fingerprinting many paths under
 * one wide directory degrades sharply; that shape deserves its own benchmark
 * if it ever becomes representative.
 */
struct SyntheticWorld
{
    std::filesystem::path dir;
    std::string rev;
    std::string exprString;

    explicit SyntheticWorld(size_t targetCount)
    {
        dir = createTempDir() + "/world";
        std::filesystem::create_directories(dir / "lib");

        constexpr size_t libCount = 15;
        constexpr size_t targetsPerDir = 13; // 13 target files + 13 leaves per directory
        constexpr size_t dirsPerGroup = 25;
        for (size_t i = 0; i < libCount; ++i)
            std::ofstream(dir / "lib" / fmt("common-%d.nix", i)) << fmt("{ v = %d; }\n", i);

        std::string libImports, libSum;
        for (size_t i = 0; i < libCount; ++i) {
            libImports += fmt("  l%1% = import ../../../lib/common-%1%.nix;\n", i);
            libSum += fmt("%sl%d.v", i == 0 ? "" : " + ", i);
        }

        std::string index = "{\n";
        for (size_t i = 0; i < targetCount; ++i) {
            auto zone = i / (targetsPerDir * dirsPerGroup);
            auto group = (i / targetsPerDir) % dirsPerGroup;
            auto rel = fmt("zones/z-%d/g-%d", zone, group);
            std::filesystem::create_directories(dir / rel);
            std::ofstream(dir / rel / fmt("leaf-%d.txt", i)) << fmt("leaf %d\n", i);
            std::ofstream(dir / rel / fmt("target-%d.nix", i)) << fmt(
                "{ args }:\n"
                "let\n"
                "%s"
                "  leaf = builtins.readFile ./leaf-%d.txt;\n"
                "in {\n"
                "  drvPath = \"/nix/store/00000000000000000000000000000000-t${toString (%s)}-${builtins.hashString "
                "\"sha256\" leaf}-%d.drv\";\n"
                "}\n",
                libImports,
                i,
                libSum,
                i);
            index += fmt("  \"target-%1%\" = import ./%2%/target-%1%.nix;\n", i, rel);
        }
        index += "}\n";
        std::ofstream(dir / "targets-index.nix") << index;

        std::ofstream(dir / "resolve.nix") << "args:\n"
                                              "let targets = import ./targets-index.nix;\n"
                                              "in {\n"
                                              "  allTargetNames = builtins.attrNames targets;\n"
                                              "  resolve = id: targets.${id} { inherit args; };\n"
                                              "}\n";

        auto git = [&](Strings args) {
            args.insert(args.begin(), {"-C", dir.string()});
            return runProgram("git", true, args);
        };
        git({"init", "-q"});
        git({"config", "user.email", "bench@example.com"});
        git({"config", "user.name", "bench"});
        git({"add", "-A"});
        git({"commit", "-q", "-m", "synthetic world"});
        rev = chomp(git({"rev-parse", "HEAD"}));

        exprString = fmt(
            "let\n"
            "  base = { gitDir = \"%s/.git\"; resolver = \"resolve.nix\"; rev = \"%s\"; args = { system = \"bench\"; "
            "}; };\n"
            "  names = builtins.tecnixTargetNames base;\n"
            "  result = builtins.tecnixTargets (base // { targets = names; includeDependencies = true; includeTargets "
            "= false; });\n"
            "in builtins.deepSeq result (builtins.length result)",
            dir.string(),
            rev);
    }
};

SyntheticWorld & worldForTargetCount(size_t targetCount)
{
    // The per-process Tecnix SQLite cache must live in a fresh directory so
    // benchmark runs are isolated; set before the first eval creates it.
    static bool cacheIsolated = [] {
        setenv("XDG_CACHE_HOME", createTempDir("", "tecnix-bench-cache").c_str(), 1);
        return true;
    }();
    (void) cacheIsolated;

    static std::map<size_t, std::unique_ptr<SyntheticWorld>> worlds;
    auto & world = worlds[targetCount];
    if (!world)
        world = std::make_unique<SyntheticWorld>(targetCount);
    return *world;
}

struct BenchEnv
{
    ref<Store> store = openStore("dummy://");
    fetchers::Settings fetchSettings{};
    bool readOnlyMode = true;
    EvalSettings evalSettings{readOnlyMode};

    explicit BenchEnv(bool tecnixEvalCache)
    {
        evalSettings.nixPath = {};
        evalSettings.pureEval = true;
        evalSettings.lazyTrees = true;
        evalSettings.tecnixEvalCache = tecnixEvalCache;
    }

    /** One full tracked evaluation in a fresh EvalState; returns the target count. */
    NixInt::Inner evalOnce(const SyntheticWorld & world)
    {
        EvalState state(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
        auto * expr = state.parseExprFromString(world.exprString, state.rootPath(CanonPath::root));
        Value v;
        state.eval(expr, v);
        state.forceValue(v, noPos);
        return v.integer().value;
    }
};

void BM_TecnixTrackedEvalUncached(benchmark::State & state)
{
    auto & world = worldForTargetCount(state.range(0));
    BenchEnv env(false);
    for (auto _ : state) {
        auto n = env.evalOnce(world);
        benchmark::DoNotOptimize(n);
        if (n != state.range(0))
            state.SkipWithError("unexpected target count");
    }
}

void BM_TecnixWarmCacheHit(benchmark::State & state)
{
    auto & world = worldForTargetCount(state.range(0));
    BenchEnv env(true);
    // Populate the persistent cache outside the timed loop.
    env.evalOnce(world);
    for (auto _ : state) {
        auto n = env.evalOnce(world);
        benchmark::DoNotOptimize(n);
        if (n != state.range(0))
            state.SkipWithError("unexpected target count");
    }
}

} // namespace

BENCHMARK(BM_TecnixTrackedEvalUncached)->Unit(benchmark::kMillisecond)->Arg(1000)->Arg(10000);
BENCHMARK(BM_TecnixWarmCacheHit)->Unit(benchmark::kMillisecond)->Arg(1000)->Arg(10000);
