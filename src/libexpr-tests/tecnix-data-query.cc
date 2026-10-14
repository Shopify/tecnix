#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "nix/expr/tecnix/data-query.hh"
#include "nix/expr/tecnix/eval-cache.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

namespace {

constexpr auto manifestPath = ".meta/manifest.json";

constexpr auto twoZones = R"({ "//zones/a": { "id": "W-000001" }, "//zones/b": { "id": "W-000002" } })";
constexpr auto threeZones =
    R"({ "//zones/a": { "id": "W-000001" }, "//zones/b": { "id": "W-000002" }, "//zones/c": { "id": "W-000003" } })";
constexpr auto bRenumbered = R"({ "//zones/a": { "id": "W-000001" }, "//zones/b": { "id": "W-000009" } })";
constexpr auto reordered = R"({ "//zones/b": { "id": "W-000002" }, "//zones/a": { "id": "W-000001" } })";
constexpr auto twoZonesToml = R"(
# the same tree as twoZones
["//zones/a"]
id = "W-000001"

["//zones/b"]
id = "W-000002"
)";

/** A source tree with one file. */
ref<SourceAccessor> treeWith(std::string_view path, std::string content)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->addFile(CanonPath(path), std::move(content));
    return accessor;
}

std::vector<std::string_view> at(std::initializer_list<std::string_view> segments)
{
    return segments;
}

DataQuery get(std::initializer_list<std::string_view> segments, DataFormat format = DataFormat::Json)
{
    return {.format = format, .op = DataQueryOp::Get, .at = at(segments)};
}

DataQuery has(std::initializer_list<std::string_view> segments)
{
    return {.op = DataQueryOp::Has, .at = at(segments)};
}

DataQuery keys(std::initializer_list<std::string_view> segments)
{
    return {.op = DataQueryOp::Keys, .at = at(segments)};
}

DataQuery find(std::string_view where, std::string_view value, std::initializer_list<std::string_view> segments = {})
{
    return {.op = DataQueryOp::Find, .at = at(segments), .where = where, .value = value};
}

ParsedDataFile parsed(std::string_view content, DataFormat format = DataFormat::Json)
{
    return parseDataFile(format, content);
}

std::string answer(const ParsedDataFile & file, const DataQuery & query)
{
    std::string out;
    appendDataQueryAnswer(out, evaluateDataQuery(file, query));
    return out;
}

std::string line(const ParsedDataFile & file, const DataQuery & query)
{
    std::string out;
    appendDataObservation(out, query, answer(file, query));
    return out;
}

/** The fingerprint a target that asked exactly `queries` of `content` records. */
std::string fingerprintOf(std::string_view content, const std::vector<DataQuery> & queries)
{
    auto file = parsed(content);
    std::vector<std::string> lines;
    for (auto & query : queries)
        lines.push_back(line(file, query));
    return dataQueryFingerprint({lines.begin(), lines.end()});
}

/** Whether `fingerprint`, recorded for `manifestPath`, holds against a tree with `content` there. */
bool holdsAgainst(std::string_view fingerprint, std::optional<std::string> content)
{
    DependencyFingerprintCache cache;
    auto tree = content ? treeWith(manifestPath, *content) : make_ref<MemorySourceAccessor>();
    return dependencyFingerprintHolds(tree, manifestPath, fingerprint, cache);
}

} // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TEST(TecnixDataQuery, GetReturnsTheValueOrAbsent)
{
    auto file = parsed(twoZones);
    EXPECT_EQ(answer(file, get({"//zones/a"})), R"({"id":"W-000001"})");
    EXPECT_EQ(answer(file, get({"//zones/a", "id"})), R"("W-000001")");
    EXPECT_EQ(answer(file, get({"//zones/c"})), "absent");
    // Descending through a string, not an object
    EXPECT_EQ(answer(file, get({"//zones/a", "id", "x"})), "absent");
    // The whole document
    EXPECT_EQ(answer(file, get({})), R"({"//zones/a":{"id":"W-000001"},"//zones/b":{"id":"W-000002"}})");
}

TEST(TecnixDataQuery, HasIsPresenceNotTruthiness)
{
    auto file = parsed(R"({ "a": null, "b": false })");
    EXPECT_EQ(answer(file, has({"a"})), "true");
    EXPECT_EQ(answer(file, has({"b"})), "true");
    EXPECT_EQ(answer(file, has({"c"})), "false");
    EXPECT_EQ(answer(file, has({})), "true");
}

TEST(TecnixDataQuery, KeysAreSortedAndOnlyForObjects)
{
    auto file = parsed(reordered);
    EXPECT_EQ(answer(file, keys({})), R"(["//zones/a","//zones/b"])");
    EXPECT_EQ(answer(file, keys({"//zones/a", "id"})), "absent");
    EXPECT_EQ(answer(file, keys({"nope"})), "absent");
}

TEST(TecnixDataQuery, FindReturnsEveryMatchingKeySorted)
{
    auto file = parsed(
        R"({ "zones": { "//b": { "id": "X" }, "//a": { "id": "X" }, "//c": { "id": "Y" }, "//d": 3, "//e": { "id": 7 } } })");
    EXPECT_EQ(answer(file, find("id", "X", {"zones"})), R"(["//a","//b"])");
    EXPECT_EQ(answer(file, find("id", "Y", {"zones"})), R"(["//c"])");
    EXPECT_EQ(answer(file, find("id", "Z", {"zones"})), "[]");
    // Only string fields compare
    EXPECT_EQ(answer(file, find("id", "7", {"zones"})), "[]");
    // No object there: empty, not absent
    EXPECT_EQ(answer(file, find("id", "X", {"elsewhere"})), "[]");
}

TEST(TecnixDataQuery, FindOnALargeDocumentMatchesAScan)
{
    nlohmann::json doc = nlohmann::json::object();
    for (int i = 0; i < 2000; i++)
        doc["//zone/" + std::to_string(i)] = {{"id", "W-" + std::to_string(i % 700)}};
    auto file = parsed(doc.dump());
    for (int i = 0; i < 700; i += 97) {
        auto id = "W-" + std::to_string(i);
        std::vector<std::string> expected;
        for (auto & [key, value] : doc.items())
            if (value["id"] == id)
                expected.push_back(key);
        EXPECT_EQ(answer(file, find("id", id)), nlohmann::json(expected).dump()) << id;
    }
}

TEST(TecnixDataQuery, AbsentFileAnswersEveryQuery)
{
    ParsedDataFile file;
    EXPECT_EQ(answer(file, get({"x"})), "absent");
    EXPECT_EQ(answer(file, has({"x"})), "false");
    EXPECT_EQ(answer(file, keys({})), "absent");
    EXPECT_EQ(answer(file, find("id", "x")), "[]");
}

TEST(TecnixDataQuery, LongAnswersAreHashed)
{
    std::string big = R"({"k":")" + std::string(100, 'x') + R"("})";
    auto file = parsed(big);
    auto hashed = answer(file, get({}));
    EXPECT_TRUE(hashed.starts_with("sha256:")) << hashed;
    EXPECT_EQ(hashed.size(), 7 + 64);
    // ...and are still a function of the content.
    EXPECT_NE(hashed, answer(parsed(R"({"k":")" + std::string(100, 'y') + R"("})"), get({})));
}

TEST(TecnixDataQuery, TomlParsesToTheSameTree)
{
    auto json = parsed(twoZones);
    auto toml = parsed(twoZonesToml, DataFormat::Toml);
    EXPECT_EQ(answer(json, get({"//zones/a"})), answer(toml, get({"//zones/a"})));
    EXPECT_EQ(answer(json, keys({})), answer(toml, keys({})));
    EXPECT_EQ(answer(json, find("id", "W-000002")), answer(toml, find("id", "W-000002")));

    auto typed =
        parsed("i = 1\nf = 1.5\nb = true\ns = \"s\"\na = [1, 2]\nd = 1979-05-27T07:32:00Z\n", DataFormat::Toml);
    EXPECT_EQ(answer(typed, get({"i"})), "1");
    EXPECT_EQ(answer(typed, get({"f"})), "1.5");
    EXPECT_EQ(answer(typed, get({"b"})), "true");
    EXPECT_EQ(answer(typed, get({"s"})), R"("s")");
    EXPECT_EQ(answer(typed, get({"a"})), "[1,2]");
    EXPECT_EQ(answer(typed, get({"d", "_type"})), R"("timestamp")");
}

TEST(TecnixDataQuery, UnparseableFilesThrow)
{
    EXPECT_THROW(parsed("{ nope"), DataFileParseError);
    EXPECT_THROW(parsed("= nope", DataFormat::Toml), DataFileParseError);
}

TEST(TecnixDataQuery, FormatFromExtension)
{
    EXPECT_EQ(dataFormatFromExtension(".meta/manifest.json"), DataFormat::Json);
    EXPECT_EQ(dataFormatFromExtension("Cargo.toml"), DataFormat::Toml);
    EXPECT_EQ(dataFormatFromExtension("dir.json/file"), std::nullopt);
    EXPECT_EQ(dataFormatFromExtension("Makefile"), std::nullopt);
    EXPECT_EQ(dataFormatFromExtension("x.yaml"), std::nullopt);
}

// ---------------------------------------------------------------------------
// Observation lines
// ---------------------------------------------------------------------------

TEST(TecnixDataQuery, LinesRoundTrip)
{
    std::vector<std::pair<DataQuery, std::string>> cases = {
        {get({"//zones/a"}), R"({"id":"W-000001"})"},
        {get({}), "absent"},
        {get({"a", "b", "c"}, DataFormat::Toml), "1"},
        {has({"x"}), "true"},
        {keys({}), "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
        {find("id", "W-000001"), R"(["//zones/a"])"},
        {find("id", "W-000001", {"zones", "nested"}), "[]"},
        // Empty segments are keys too.
        {get({""}), "absent"},
        {get({"", ""}), "absent"},
    };
    for (auto & [query, answer] : cases) {
        std::string encoded;
        appendDataObservation(encoded, query, answer);
        auto parsed = parseDataObservation(encoded);
        ASSERT_TRUE(parsed) << encoded;
        EXPECT_EQ(parsed->query, query) << encoded;
        EXPECT_EQ(parsed->answer, answer) << encoded;
        std::string reencoded;
        appendDataObservation(reencoded, parsed->query, parsed->answer);
        EXPECT_EQ(reencoded, encoded);
    }
}

TEST(TecnixDataQuery, LinesAreTabSeparatedWithTheAnswerLast)
{
    std::string encoded;
    appendDataObservation(encoded, find("id", "W-1", {"zones"}), "[]");
    EXPECT_EQ(encoded, "json\tfind\tid\tW-1\tzones\t[]");
    encoded.clear();
    appendDataObservation(encoded, keys({}), "absent");
    EXPECT_EQ(encoded, "json\tkeys\tabsent");
}

TEST(TecnixDataQuery, MalformedLinesDoNotParse)
{
    EXPECT_FALSE(parseDataObservation(""));
    EXPECT_FALSE(parseDataObservation("json\tget"));
    EXPECT_FALSE(parseDataObservation("yaml\tget\tabsent"));
    EXPECT_FALSE(parseDataObservation("json\tselect\tx\tabsent"));
    EXPECT_FALSE(parseDataObservation("json\tfind\tid\tabsent"));
    EXPECT_TRUE(parseDataObservation("json\tfind\tid\tW-1\t[]"));
}

TEST(TecnixDataQuery, FieldsMayNotContainSeparators)
{
    EXPECT_TRUE(isValidDataQueryField("//zones/a"));
    EXPECT_TRUE(isValidDataQueryField(""));
    EXPECT_TRUE(isValidDataQueryField("with space and \"quotes\" and ;semicolons"));
    EXPECT_FALSE(isValidDataQueryField("a\tb"));
    EXPECT_FALSE(isValidDataQueryField("a\nb"));
    EXPECT_FALSE(isValidDataQueryField(std::string_view("a\0b", 3)));
}

// ---------------------------------------------------------------------------
// Fingerprints
// ---------------------------------------------------------------------------

TEST(TecnixDataQuery, FingerprintLinesAreSortedAndDeduplicated)
{
    auto fp = dataQueryFingerprint({"json\tkeys\tabsent", "json\tget\ta\t1", "json\tkeys\tabsent"});
    EXPECT_EQ(fp, "query:json\tget\ta\t1\njson\tkeys\tabsent");
    auto lines = dataQueryFingerprintLines(fp);
    ASSERT_TRUE(lines);
    EXPECT_EQ(*lines, (std::vector<std::string_view>{"json\tget\ta\t1", "json\tkeys\tabsent"}));
}

TEST(TecnixDataQuery, ObjectFingerprintsAreNotQueries)
{
    EXPECT_FALSE(dataQueryFingerprintLines("git:0123;mode=100644"));
    EXPECT_FALSE(dataQueryFingerprintLines("absent"));
    EXPECT_FALSE(dataQueryFingerprintLines("query:"));
}

TEST(TecnixDataQuery, ObservationsSplitAtTheFirstNul)
{
    std::string observation;
    appendDataAccess(observation, "x/y.json", "json\tget\ta\t1");
    auto split = splitDataAccess(observation);
    ASSERT_TRUE(split);
    EXPECT_EQ(split->file, "x/y.json");
    EXPECT_EQ(split->line, "json\tget\ta\t1");
    EXPECT_FALSE(splitDataAccess("lib/common.nix"));
}

// ---------------------------------------------------------------------------
// Validation: does a recorded fingerprint still hold against a tree?
// ---------------------------------------------------------------------------

TEST(TecnixDataQueryHolds, EntryQueryIgnoresOtherEntries)
{
    auto fp = fingerprintOf(twoZones, {get({"//zones/a"})});
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
    EXPECT_TRUE(holdsAgainst(fp, threeZones));
    EXPECT_TRUE(holdsAgainst(fp, bRenumbered));
    EXPECT_TRUE(holdsAgainst(fp, reordered));
    EXPECT_TRUE(holdsAgainst(fp, "{\n  \"//zones/a\": {\n    \"id\": \"W-000001\"\n  }\n}\n"));
}

TEST(TecnixDataQueryHolds, EntryQueryFollowsItsOwnEntry)
{
    auto fp = fingerprintOf(twoZones, {get({"//zones/b"})});
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
    EXPECT_FALSE(holdsAgainst(fp, bRenumbered));
    EXPECT_FALSE(holdsAgainst(fp, R"({ "//zones/a": { "id": "W-000001" } })"));
    EXPECT_FALSE(holdsAgainst(fp, std::nullopt));
}

TEST(TecnixDataQueryHolds, AbsentEntryHoldsUntilItAppears)
{
    auto fp = fingerprintOf(twoZones, {get({"//zones/c"}), has({"//zones/c"})});
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
    EXPECT_TRUE(holdsAgainst(fp, bRenumbered));
    // A missing file gives the same answers.
    EXPECT_TRUE(holdsAgainst(fp, std::nullopt));
    EXPECT_FALSE(holdsAgainst(fp, threeZones));
}

TEST(TecnixDataQueryHolds, KeysFollowTheKeySetOnly)
{
    auto fp = fingerprintOf(twoZones, {keys({})});
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
    EXPECT_TRUE(holdsAgainst(fp, bRenumbered));
    EXPECT_TRUE(holdsAgainst(fp, reordered));
    EXPECT_FALSE(holdsAgainst(fp, threeZones));
}

TEST(TecnixDataQueryHolds, FindFollowsTheMatchSet)
{
    auto fp = fingerprintOf(twoZones, {find("id", "W-000002")});
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
    EXPECT_TRUE(holdsAgainst(fp, threeZones));
    EXPECT_FALSE(holdsAgainst(fp, bRenumbered));
    EXPECT_FALSE(holdsAgainst(fp, R"({ "//zones/b": { "id": "W-000002" }, "//zones/z": { "id": "W-000002" } })"));
}

TEST(TecnixDataQueryHolds, EveryLineMustHold)
{
    auto fp = fingerprintOf(twoZones, {get({"//zones/a"}), get({"//zones/b"})});
    EXPECT_TRUE(holdsAgainst(fp, threeZones));
    EXPECT_FALSE(holdsAgainst(fp, bRenumbered));
}

TEST(TecnixDataQueryHolds, UnparseableAndUnknownNeverHold)
{
    auto fp = fingerprintOf(twoZones, {get({"//zones/c"})});
    EXPECT_FALSE(holdsAgainst(fp, "{ nope"));
    EXPECT_FALSE(holdsAgainst("query:yaml\tget\tx\tabsent", twoZones));
    EXPECT_FALSE(holdsAgainst("query:json\tselect\tx\tabsent", twoZones));
    EXPECT_FALSE(holdsAgainst("query:", twoZones));
    EXPECT_FALSE(holdsAgainst("query:\n", twoZones));
}

TEST(TecnixDataQueryHolds, ObjectFingerprintsCompareAsBefore)
{
    // A MemorySourceAccessor fingerprints every path with its own fingerprint.
    DependencyFingerprintCache cache;
    auto tree = treeWith(manifestPath, twoZones);
    tree->fingerprint = "git:0123;mode=100644";
    auto current = dependencyFingerprint(tree, manifestPath, cache);
    ASSERT_TRUE(current);
    EXPECT_EQ(*current, "git:0123;mode=100644");
    EXPECT_TRUE(dependencyFingerprintHolds(tree, manifestPath, *current, cache));
    EXPECT_FALSE(dependencyFingerprintHolds(tree, manifestPath, "absent", cache));
    EXPECT_FALSE(dependencyFingerprintHolds(tree, manifestPath, "git:4567;mode=100644", cache));
}

TEST(TecnixDataQueryHolds, FollowsTheAccessorAcrossCaches)
{
    // The parsed document must not outlive the run it was read for: another
    // evaluation on the same thread validates against its own tree.
    auto fp = fingerprintOf(twoZones, {get({"//zones/b"})});
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
    EXPECT_FALSE(holdsAgainst(fp, bRenumbered));
    EXPECT_TRUE(holdsAgainst(fp, twoZones));
}

// ---------------------------------------------------------------------------
// Flattening: observations to closure entries
// ---------------------------------------------------------------------------

TEST(TecnixDataQueryClosure, QueriesGroupByFileAndTheObjectSubsumesThem)
{
    DependencyFingerprintCache cache;
    auto tree = make_ref<MemorySourceAccessor>();
    tree->addFile(CanonPath(manifestPath), twoZones);
    tree->addFile(CanonPath("config.toml"), "a = 1\n");
    tree->addFile(CanonPath("lib.nix"), "{}");
    tree->fingerprint = "git:0123;mode=100644";

    auto manifest = parsed(twoZones);
    auto config = parsed("a = 1\n", DataFormat::Toml);
    std::string obsA, obsB, obsConfig, obsLib;
    appendDataAccess(obsA, manifestPath, line(manifest, get({"//zones/a"})));
    appendDataAccess(obsB, manifestPath, line(manifest, get({"//zones/b"})));
    appendDataAccess(obsConfig, "config.toml", line(config, get({"a"}, DataFormat::Toml)));

    auto closure = dependencyFingerprints(tree, {obsB, "lib.nix", obsA, obsConfig, obsA}, cache);
    ASSERT_EQ(closure.size(), 3u);
    EXPECT_EQ(closure[0].path, manifestPath);
    EXPECT_EQ(closure[0].fingerprint, fingerprintOf(twoZones, {get({"//zones/a"}), get({"//zones/b"})}));
    EXPECT_EQ(closure[1].path, "config.toml");
    EXPECT_EQ(closure[1].fingerprint, "query:toml\tget\ta\t1");
    EXPECT_EQ(closure[2].path, "lib.nix");
    EXPECT_FALSE(closure[2].fingerprint.starts_with("query:"));

    // Reading the manifest whole as well: its object fingerprint subsumes the queries.
    auto whole = dependencyFingerprints(tree, {obsA, std::string(manifestPath)}, cache);
    ASSERT_EQ(whole.size(), 1u);
    EXPECT_EQ(whole[0].path, manifestPath);
    EXPECT_EQ(whole[0].fingerprint, dependencyFingerprint(tree, manifestPath, cache));
}

} // namespace nix
