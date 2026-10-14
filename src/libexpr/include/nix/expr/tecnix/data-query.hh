#pragma once
/**
 * @file
 *
 * Queries on data files (`builtins.tecnixQueryFile`) and the observations
 * they leave in a target's source closure.
 *
 * A fingerprint records what evaluation observed about a path, and a closure
 * holds while the tree still yields the same observations. A target that only
 * queried a data file observed the answers it got, not the whole object, so
 * its fingerprint for the file is
 *
 *     query:<line>\n<line>…
 *
 * one sorted line per distinct query, each
 *
 *     <format> \t <op> \t <fixed args…> \t <at segment…> \t <answer>
 *
 * with the answer last, so a variable number of segments is unambiguous.
 * Fields never contain tab, newline or NUL (the builtin rejects arguments
 * that do; answers are a bare word, a hash, or compact JSON, which escapes
 * control characters), so parsing is splitting, with no unescaping.
 *
 * Everything here is pure, so the evaluator (which records an answer while
 * a target runs) and the cache validator (which recomputes it against the
 * current tree, with no evaluator at hand) share one implementation.
 */

#include "nix/util/error.hh"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

enum class DataFormat { Json, Toml };

std::string_view dataFormatName(DataFormat format);
std::optional<DataFormat> parseDataFormat(std::string_view name);
/** The format a file name's extension implies (`.json`, `.toml`), if any. */
std::optional<DataFormat> dataFormatFromExtension(std::string_view fileName);

enum class DataQueryOp {
    /** The value at `at`, or absent. */
    Get,
    /** Whether `at` resolves (a present `null` counts). */
    Has,
    /** The sorted keys of the object at `at`, or absent if there is no object there. */
    Keys,
    /** The sorted keys of the children of the object at `at` whose string
        field `where` equals `value`; empty when there is no object there. */
    Find,
};

std::string_view dataQueryOpName(DataQueryOp op);
std::optional<DataQueryOp> parseDataQueryOp(std::string_view name);

/**
 * One question asked of a data file. The views borrow from whatever the
 * query was built or parsed from (the builtin's forced arguments, or an
 * observation line), so a `DataQuery` does not outlive that.
 */
struct DataQuery
{
    DataFormat format = DataFormat::Json;
    DataQueryOp op = DataQueryOp::Get;
    /** Object keys, root down. */
    std::vector<std::string_view> at;
    /** `Find` only: the field of each child to compare. */
    std::string_view where;
    /** `Find` only: the string the field must equal. */
    std::string_view value;

    bool operator==(const DataQuery &) const = default;
};

/** Whether a string may be a field of an observation line (no tab, newline or NUL). */
bool isValidDataQueryField(std::string_view s);

/** The file exists but is not valid `<format>`. */
MakeError(DataFileParseError, Error);

/** Parse `content` as `format`; throws `DataFileParseError`. */
nlohmann::json parseDataFile(DataFormat format, std::string_view content);

/**
 * A data file's parsed document, or nullopt for a file that does not exist,
 * which answers every query as an empty document would.
 */
using ParsedDataFile = std::optional<nlohmann::json>;

/** A query's answer: the resulting JSON value, or nullopt for absent. */
using DataQueryAnswer = std::optional<nlohmann::json>;

DataQueryAnswer evaluateDataQuery(const ParsedDataFile & file, const DataQuery & query);

/**
 * Append the encoding of an answer: `absent`; the compact JSON when it is
 * short; otherwise `sha256:<hex>` of the compact JSON.
 */
void appendDataQueryAnswer(std::string & out, const DataQueryAnswer & answer);

/** Append the observation line for `query` with an already encoded `answer`. */
void appendDataObservation(std::string & out, const DataQuery & query, std::string_view answer);

struct DataObservation
{
    DataQuery query;
    std::string_view answer;
};

/** Parse one observation line; the views borrow from `line`. Nullopt if malformed. */
std::optional<DataObservation> parseDataObservation(std::string_view line);

/** Every fingerprint built from observation lines begins with this. */
constexpr std::string_view dataQueryFingerprintPrefix = "query:";

/** The fingerprint recording the observation `lines` (deduplicated and sorted). */
std::string dataQueryFingerprint(std::vector<std::string_view> lines);

/**
 * The observation lines of a `query:` fingerprint, or nullopt if `fingerprint`
 * is not one (an object fingerprint, or a `query:` with no lines, which
 * nothing ever writes).
 */
std::optional<std::vector<std::string_view>> dataQueryFingerprintLines(std::string_view fingerprint);

/**
 * What the builtin records into a tracking frame: the repo path of the file
 * and one observation line, as `<file>\0<line>`. Repo paths contain no NUL, so
 * the first NUL is the separator and a plain path observation never has one.
 * Flattening a closure groups these by file into one `query:` fingerprint.
 */
void appendDataAccess(std::string & out, std::string_view file, std::string_view line);

struct DataAccess
{
    std::string_view file;
    std::string_view line;
};

/** Split an observation; nullopt for a plain path. */
std::optional<DataAccess> splitDataAccess(std::string_view observation);

} // namespace nix
