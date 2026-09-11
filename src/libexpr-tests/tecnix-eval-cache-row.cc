#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "nix/expr/tecnix/eval-cache-row.hh"
#include "nix/util/error.hh"

namespace nix {

namespace {

constexpr size_t noRowLimit = 1 << 30;

using Candidate = TecnixDependencyRow::Candidate;

/** A closure as the merge stores it: unique paths in sorted order. */
DependencyClosure normalized(DependencyClosure closure)
{
    std::sort(closure.begin(), closure.end(), [](auto & a, auto & b) { return a.path < b.path; });
    closure.erase(
        std::unique(closure.begin(), closure.end(), [](auto & a, auto & b) { return a.path == b.path; }),
        closure.end());
    return closure;
}

bool sameCandidate(const Candidate & a, const Candidate & b)
{
    if (a.payload != b.payload || a.dependencies.size() != b.dependencies.size())
        return false;
    for (size_t i = 0; i < a.dependencies.size(); i++)
        if (a.dependencies[i].path != b.dependencies[i].path
            || a.dependencies[i].fingerprint != b.dependencies[i].fingerprint)
            return false;
    return true;
}

/**
 * What a row should hold: per target, candidates newest first. The rules the
 * merge implements, written the obvious way.
 */
struct Model
{
    std::map<std::string, std::vector<Candidate>> targets;

    void merge(const std::vector<TecnixDependencyUpsert> & updates, size_t historyLimit)
    {
        auto keep = std::max<size_t>(historyLimit, 1);
        for (auto & [_, candidates] : targets)
            if (candidates.size() > keep)
                candidates.resize(keep);
        for (auto & update : updates) {
            Candidate fresh{.dependencies = normalized(*update.dependencies), .payload = update.payload};
            std::vector<Candidate> history{fresh};
            for (auto & candidate : targets[std::string(update.target)]) {
                if (history.size() >= keep)
                    break;
                if (!sameCandidate(candidate, fresh))
                    history.push_back(candidate);
            }
            targets[std::string(update.target)] = std::move(history);
        }
    }
};

void expectRowMatchesModel(const TecnixDependencyRow & row, const Model & model)
{
    ASSERT_EQ(row.targets.size(), model.targets.size());
    size_t t = 0;
    for (auto & [name, candidates] : model.targets) {
        auto & target = row.targets[t++];
        EXPECT_EQ(target.target, name);
        ASSERT_EQ(target.candidates.size(), candidates.size()) << name;
        for (size_t c = 0; c < candidates.size(); c++)
            EXPECT_TRUE(sameCandidate(target.candidates[c], candidates[c])) << name << " candidate " << c;
    }
}

/** Random closures over a small alphabet, so fresh closures often equal or overlap stored ones. */
struct Generator
{
    std::mt19937 rng;

    explicit Generator(unsigned seed)
        : rng(seed)
    {
    }

    size_t upTo(size_t n)
    {
        return std::uniform_int_distribution<size_t>(0, n - 1)(rng);
    }

    std::string target()
    {
        return "target" + std::to_string(upTo(6));
    }

    /** One fingerprint per path (two would be a conflict the merge rejects), sometimes listed twice. */
    DependencyClosure closure()
    {
        std::map<std::string, std::string> byPath;
        auto size = 1 + upTo(8);
        for (size_t i = 0; i < size; i++)
            byPath.emplace(
                "dir/file" + std::to_string(upTo(12)) + ".nix", "git:" + std::to_string(upTo(4)) + ";mode=100644");
        DependencyClosure closure;
        for (auto & [path, fingerprint] : byPath) {
            closure.push_back({path, fingerprint});
            if (upTo(6) == 0)
                closure.push_back({path, fingerprint});
        }
        std::shuffle(closure.begin(), closure.end(), rng);
        return closure;
    }

    std::string payload()
    {
        return upTo(3) == 0 ? "" : "/nix/store/" + std::to_string(upTo(4)) + "-x.drv";
    }
};

} // namespace

TEST(TecnixDependencyRow, mergesLikeTheModel)
{
    Generator gen(20260909);
    for (unsigned round = 0; round < 200; round++) {
        Model model;
        std::string row;
        auto historyLimit = gen.upTo(5); // 0..4, so trimming and the 0-acts-as-1 rule both happen
        for (unsigned step = 0; step < 6; step++) {
            std::vector<DependencyClosure> closures;
            std::vector<std::string> targets, payloads;
            auto count = 1 + gen.upTo(3);
            for (size_t i = 0; i < count; i++) {
                closures.push_back(gen.closure());
                targets.push_back(gen.target());
                payloads.push_back(gen.payload());
            }
            std::vector<TecnixDependencyUpsert> updates;
            std::vector<const TecnixDependencyUpsert *> updatePtrs;
            for (size_t i = 0; i < count; i++)
                updates.push_back({.target = targets[i], .dependencies = &closures[i], .payload = payloads[i]});
            for (auto & update : updates)
                updatePtrs.push_back(&update);

            std::optional<std::string_view> existing;
            if (!row.empty())
                existing = row;
            std::string merged;
            auto outcome = mergeTecnixDependencyRow(existing, updatePtrs, historyLimit, noRowLimit, merged);
            EXPECT_EQ(outcome, existing ? TecnixRowMergeOutcome::Merged : TecnixRowMergeOutcome::Fresh);
            model.merge(updates, historyLimit);

            auto decoded = decodeTecnixDependencyRow(merged);
            ASSERT_TRUE(decoded) << "round " << round << " step " << step;
            expectRowMatchesModel(*decoded, model);
            row = std::move(merged);
        }
    }
}

TEST(TecnixDependencyRow, conflictingFingerprintsForOnePathAreRejected)
{
    DependencyClosure closure{{"a.nix", "git:1;mode=100644"}, {"a.nix", "git:2;mode=100644"}};
    TecnixDependencyUpsert update{.target = "t", .dependencies = &closure, .payload = ""};
    std::string out;
    EXPECT_THROW(mergeTecnixDependencyRow(std::nullopt, {&update}, 32, noRowLimit, out), Error);
}

TEST(TecnixDependencyRow, rowsPastTheSizeLimitAreNotWritten)
{
    DependencyClosure closure{{"a.nix", "git:1;mode=100644"}};
    TecnixDependencyUpsert update{.target = "t", .dependencies = &closure, .payload = ""};
    std::string out;
    EXPECT_THROW(mergeTecnixDependencyRow(std::nullopt, {&update}, 32, 16, out), Error);
}

TEST(TecnixDependencyRow, damagedRowsAreUnreadableNeverMisread)
{
    Generator gen(1);
    std::vector<DependencyClosure> closures;
    std::vector<std::string> names; // the upserts view these
    std::vector<TecnixDependencyUpsert> updates;
    for (size_t i = 0; i < 4; i++) {
        closures.push_back(gen.closure());
        names.push_back("target" + std::to_string(i));
    }
    for (size_t i = 0; i < 4; i++)
        updates.push_back({.target = names[i], .dependencies = &closures[i], .payload = "p"});
    std::vector<const TecnixDependencyUpsert *> updatePtrs;
    for (auto & update : updates)
        updatePtrs.push_back(&update);
    std::string row;
    mergeTecnixDependencyRow(std::nullopt, updatePtrs, 32, noRowLimit, row);
    ASSERT_TRUE(decodeTecnixDependencyRow(row));

    // Every truncation and a few hundred single-byte changes: each must either
    // still decode (the byte was slack) or be rejected, and merging into it
    // must yield a readable row either way.
    for (size_t length = 0; length < row.size(); length += 7)
        EXPECT_NO_THROW(decodeTecnixDependencyRow(row.substr(0, length)));
    for (unsigned i = 0; i < 300; i++) {
        auto damaged = row;
        damaged[gen.upTo(damaged.size())] ^= static_cast<char>(1 + gen.upTo(255));
        EXPECT_NO_THROW(decodeTecnixDependencyRow(damaged));
        std::string merged;
        auto outcome = mergeTecnixDependencyRow(damaged, updatePtrs, 32, noRowLimit, merged);
        EXPECT_TRUE(
            outcome == TecnixRowMergeOutcome::Merged || outcome == TecnixRowMergeOutcome::ReplacedUnreadable
            || outcome == TecnixRowMergeOutcome::ReplacedOtherVersion);
        // A flipped byte may leave a stored target readable under another
        // name; what must hold is that every fresh target is present with its
        // closure newest.
        auto decoded = decodeTecnixDependencyRow(merged);
        ASSERT_TRUE(decoded);
        for (auto & update : updates) {
            auto target = std::find_if(
                decoded->targets.begin(), decoded->targets.end(), [&](auto & t) { return t.target == update.target; });
            ASSERT_NE(target, decoded->targets.end()) << update.target;
            ASSERT_FALSE(target->candidates.empty());
            EXPECT_TRUE(sameCandidate(
                target->candidates.front(),
                Candidate{.dependencies = normalized(*update.dependencies), .payload = update.payload}));
        }
    }
}

TEST(TecnixDependencyRow, storedRowsFromAnotherVersionAreReplacedQuietly)
{
    DependencyClosure closure{{"a.nix", "git:1;mode=100644"}};
    TecnixDependencyUpsert update{.target = "t", .dependencies = &closure, .payload = ""};
    std::vector<const TecnixDependencyUpsert *> updatePtrs{&update};
    std::string row;
    mergeTecnixDependencyRow(std::nullopt, updatePtrs, 32, noRowLimit, row);
    row[4] = 99; // the little-endian version field follows the 4-byte magic
    std::string merged;
    EXPECT_EQ(
        mergeTecnixDependencyRow(row, updatePtrs, 32, noRowLimit, merged), TecnixRowMergeOutcome::ReplacedOtherVersion);
    EXPECT_TRUE(decodeTecnixDependencyRow(merged));
}

/** A row in the current format, byte for byte. If this fails the format changed: undo it, or bump
    dependencyBlobVersion and regenerate these bytes on purpose. */
TEST(TecnixDependencyRow, formatIsPinned)
{
    static const char golden[] =
        "545844430200000000000000020000000200000003000000030000000300000005000000440000005c0000006c000000"
        "84000000c800000004010000280100005001000000000000050000000a000000616c7068616561727468000000000000"
        "02000000020000000100000000000000050000000a000000612e6e6978622e6e69780000000000001100000022000000"
        "330000006769743a313b6d6f64653d3130303634346769743a323b6d6f64653d3130303634346769743a333b6d6f6465"
        "3d313030363434000000000000000000160000002c0000002f6e69782f73746f72652f782d616c7068612e6472762f6e"
        "69782f73746f72652f792d616c7068612e64727600000000020000000200000002000000020000000100000004000000"
        "010000000000000000000000000000000100000002000000000000000000000001000000010000000000000000000000";

    DependencyClosure alphaV1{{"b.nix", "git:2;mode=100644"}, {"a.nix", "git:1;mode=100644"}};
    DependencyClosure alphaV2{{"a.nix", "git:1;mode=100644"}, {"b.nix", "git:3;mode=100644"}};
    DependencyClosure earth{{"a.nix", "git:1;mode=100644"}};
    std::vector<TecnixDependencyUpsert> first{
        {.target = "alpha", .dependencies = &alphaV1, .payload = "/nix/store/x-alpha.drv"},
        {.target = "earth", .dependencies = &earth, .payload = ""},
    };
    std::string row;
    mergeTecnixDependencyRow(std::nullopt, {&first[0], &first[1]}, 32, noRowLimit, row);
    TecnixDependencyUpsert second{.target = "alpha", .dependencies = &alphaV2, .payload = "/nix/store/y-alpha.drv"};
    std::string merged;
    mergeTecnixDependencyRow(row, {&second}, 32, noRowLimit, merged);

    std::string hex;
    for (unsigned char c : merged) {
        char byte[3];
        snprintf(byte, sizeof(byte), "%02x", c);
        hex += byte;
    }
    EXPECT_EQ(hex, golden);

    // And the pinned bytes still decode to what they say.
    std::string pinned;
    for (size_t i = 0; golden[i]; i += 2)
        pinned.push_back(static_cast<char>(std::stoi(std::string(golden + i, 2), nullptr, 16)));
    auto decoded = decodeTecnixDependencyRow(pinned);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->targets.size(), 2u);
    EXPECT_EQ(decoded->targets[0].target, "alpha");
    ASSERT_EQ(decoded->targets[0].candidates.size(), 2u);
    EXPECT_EQ(decoded->targets[0].candidates[0].payload, "/nix/store/y-alpha.drv");
    EXPECT_EQ(decoded->targets[0].candidates[0].dependencies[1].fingerprint, "git:3;mode=100644");
    EXPECT_EQ(decoded->targets[0].candidates[1].payload, "/nix/store/x-alpha.drv");
    EXPECT_EQ(decoded->targets[1].target, "earth");
    ASSERT_EQ(decoded->targets[1].candidates.size(), 1u);
    EXPECT_EQ(decoded->targets[1].candidates[0].payload, "");
}

} // namespace nix
