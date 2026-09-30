#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "nix/expr/tecnix/eval-cache.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

namespace {

/** A source tree whose only file is `.meta/manifest.json` with the given content. */
ref<SourceAccessor> treeWithManifest(std::string manifest)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->addFile(CanonPath(".meta/manifest.json"), std::move(manifest));
    return accessor;
}

/** Fingerprint one synthetic manifest path, with a fresh cache like a fresh evaluation. */
std::optional<std::string> fingerprint(ref<SourceAccessor> accessor, std::string_view syntheticPath)
{
    DependencyFingerprintCache cache;
    return dependencyFingerprint(accessor, syntheticPath, cache);
}

constexpr auto twoZones = R"({ "//zones/a": { "id": "W-000001" }, "//zones/b": { "id": "W-000002" } })";
constexpr auto threeZones =
    R"({ "//zones/a": { "id": "W-000001" }, "//zones/b": { "id": "W-000002" }, "//zones/c": { "id": "W-000003" } })";
constexpr auto bRenumbered = R"({ "//zones/a": { "id": "W-000001" }, "//zones/b": { "id": "W-000009" } })";

} // namespace

TEST(TecnixManifestFingerprint, EntryFingerprintIgnoresOtherEntries)
{
    auto entryA = ".meta/manifest.json#//zones/a";
    auto before = fingerprint(treeWithManifest(twoZones), entryA);
    ASSERT_TRUE(before);
    EXPECT_EQ(fingerprint(treeWithManifest(threeZones), entryA), before);
    EXPECT_EQ(fingerprint(treeWithManifest(bRenumbered), entryA), before);
}

TEST(TecnixManifestFingerprint, EntryFingerprintFollowsItsOwnEntry)
{
    auto entryB = ".meta/manifest.json#//zones/b";
    EXPECT_NE(fingerprint(treeWithManifest(twoZones), entryB), fingerprint(treeWithManifest(bRenumbered), entryB));
    EXPECT_NE(
        fingerprint(treeWithManifest(twoZones), ".meta/manifest.json#//zones/a"),
        fingerprint(treeWithManifest(twoZones), entryB));
}

TEST(TecnixManifestFingerprint, AbsentEntryHasItsOwnFingerprint)
{
    auto entryC = ".meta/manifest.json#//zones/c";
    auto absent = fingerprint(treeWithManifest(twoZones), entryC);
    ASSERT_TRUE(absent);
    EXPECT_NE(fingerprint(treeWithManifest(threeZones), entryC), absent);
}

TEST(TecnixManifestFingerprint, KeyFingerprintTracksTheKeySetOnly)
{
    auto keys = ".meta/manifest.json#keys";
    auto before = fingerprint(treeWithManifest(twoZones), keys);
    ASSERT_TRUE(before);
    EXPECT_NE(fingerprint(treeWithManifest(threeZones), keys), before);
    // Renumbering a zone changes no key.
    EXPECT_EQ(fingerprint(treeWithManifest(bRenumbered), keys), before);
}

TEST(TecnixManifestFingerprint, KeyFingerprintIgnoresKeyOrder)
{
    auto reordered = R"({ "//zones/b": { "id": "W-000002" }, "//zones/a": { "id": "W-000001" } })";
    EXPECT_EQ(
        fingerprint(treeWithManifest(twoZones), ".meta/manifest.json#keys"),
        fingerprint(treeWithManifest(reordered), ".meta/manifest.json#keys"));
}

TEST(TecnixManifestFingerprint, IdFingerprintTracksWhereTheIdPoints)
{
    auto idOfB = ".meta/manifest.json#id/W-000002";
    auto before = fingerprint(treeWithManifest(twoZones), idOfB);
    ASSERT_TRUE(before);
    // Adding an unrelated zone leaves the lookup alone; renumbering removes the id.
    EXPECT_EQ(fingerprint(treeWithManifest(threeZones), idOfB), before);
    EXPECT_NE(fingerprint(treeWithManifest(bRenumbered), idOfB), before);
}

TEST(TecnixManifestFingerprint, FollowsTheAccessorAcrossCaches)
{
    // The parsed manifest must not outlive the cache it was read for: another
    // evaluation on the same thread fingerprints against its own commit.
    auto entryB = ".meta/manifest.json#//zones/b";
    auto first = fingerprint(treeWithManifest(twoZones), entryB);
    auto second = fingerprint(treeWithManifest(bRenumbered), entryB);
    EXPECT_NE(first, second);
    EXPECT_NE(
        fingerprint(treeWithManifest(twoZones), ".meta/manifest.json#keys"),
        fingerprint(treeWithManifest(threeZones), ".meta/manifest.json#keys"));
}

TEST(TecnixManifestFingerprint, NoManifestMeansNoFingerprint)
{
    EXPECT_EQ(fingerprint(make_ref<MemorySourceAccessor>(), ".meta/manifest.json#keys"), std::nullopt);
    // ...and that must not leak into the next tree either.
    EXPECT_TRUE(fingerprint(treeWithManifest(twoZones), ".meta/manifest.json#keys"));
}

} // namespace nix
