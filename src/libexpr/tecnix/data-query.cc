#include "nix/expr/tecnix/data-query.hh"

#include "nix/util/hash.hh"

#include "expr-config-private.hh"

#include <algorithm>
#include <sstream>

#include <toml.hpp>

namespace nix {

using namespace std::literals;

void DataFileParseError::anchor() {}

/** Answers up to this long are stored verbatim; longer ones as a hash. */
static constexpr size_t maxVerbatimAnswerBytes = 64;

static constexpr char fieldSeparator = '\t';
static constexpr char lineSeparator = '\n';
static constexpr std::string_view absentAnswer = "absent";

// ============================================================================
// Names
// ============================================================================

std::string_view dataFormatName(DataFormat format)
{
    switch (format) {
    case DataFormat::Json:
        return "json";
    case DataFormat::Toml:
        return "toml";
    }
    unreachable();
}

std::optional<DataFormat> parseDataFormat(std::string_view name)
{
    if (name == "json")
        return DataFormat::Json;
    if (name == "toml")
        return DataFormat::Toml;
    return std::nullopt;
}

std::optional<DataFormat> dataFormatFromExtension(std::string_view fileName)
{
    auto base = fileName.substr(fileName.rfind('/') + 1);
    auto dot = base.rfind('.');
    if (dot == std::string_view::npos)
        return std::nullopt;
    return parseDataFormat(base.substr(dot + 1));
}

std::string_view dataQueryOpName(DataQueryOp op)
{
    switch (op) {
    case DataQueryOp::Get:
        return "get";
    case DataQueryOp::Has:
        return "has";
    case DataQueryOp::Keys:
        return "keys";
    case DataQueryOp::Find:
        return "find";
    }
    unreachable();
}

std::optional<DataQueryOp> parseDataQueryOp(std::string_view name)
{
    if (name == "get")
        return DataQueryOp::Get;
    if (name == "has")
        return DataQueryOp::Has;
    if (name == "keys")
        return DataQueryOp::Keys;
    if (name == "find")
        return DataQueryOp::Find;
    return std::nullopt;
}

bool isValidDataQueryField(std::string_view s)
{
    return s.find_first_of("\t\n\0"sv) == std::string_view::npos;
}

// ============================================================================
// Parsing data files
// ============================================================================

static nlohmann::json tomlToJson(const toml::value & t)
{
    switch (t.type()) {
    case toml::value_t::table: {
        auto object = nlohmann::json::object();
        for (auto & [key, value] : toml::get<toml::table>(t))
            object[key] = tomlToJson(value);
        return object;
    }
    case toml::value_t::array: {
        auto array = nlohmann::json::array();
        for (auto & value : toml::get<std::vector<toml::value>>(t))
            array.push_back(tomlToJson(value));
        return array;
    }
    case toml::value_t::boolean:
        return toml::get<bool>(t);
    case toml::value_t::integer:
        return toml::get<int64_t>(t);
    case toml::value_t::floating:
        return toml::get<double>(t);
    case toml::value_t::string:
        return std::string(toml::get<std::string_view>(t));
    case toml::value_t::local_datetime:
    case toml::value_t::offset_datetime:
    case toml::value_t::local_date:
    case toml::value_t::local_time: {
        // The shape `builtins.fromTOML` gives timestamps. The text is
        // whatever toml11 prints, which is deterministic for a given input,
        // and that is all a fingerprint needs.
        std::ostringstream s;
        s << t;
        return nlohmann::json{{"_type", "timestamp"}, {"value", s.str()}};
    }
    case toml::value_t::empty:
        return nullptr;
    }
    unreachable();
}

nlohmann::json parseDataFile(DataFormat format, std::string_view content)
{
    switch (format) {
    case DataFormat::Json:
        try {
            return nlohmann::json::parse(content);
        } catch (const nlohmann::json::exception & e) {
            throw DataFileParseError("%s", e.what());
        }
    case DataFormat::Toml:
        try {
            std::istringstream stream{std::string(content)};
            return tomlToJson(
                toml::parse(
                    stream,
                    "tecnixQueryFile"
#if HAVE_TOML11_4
                    ,
                    toml::spec::v(1, 0, 0)
#endif
                        ));
        } catch (const std::exception & e) {
            throw DataFileParseError("%s", e.what());
        }
    }
    unreachable();
}

// ============================================================================
// Evaluating queries
// ============================================================================

/** The node `at` names under `root`, or null if some step is not an object with that key. */
static const nlohmann::json * resolveAt(const nlohmann::json * node, std::span<const std::string_view> at)
{
    for (auto segment : at) {
        if (!node->is_object())
            return nullptr;
        auto it = node->find(segment);
        if (it == node->end())
            return nullptr;
        node = &*it;
    }
    return node;
}

DataQueryAnswer evaluateDataQuery(const ParsedDataFile & file, const DataQuery & query)
{
    auto * node = file ? resolveAt(&*file, query.at) : nullptr;

    switch (query.op) {
    case DataQueryOp::Get:
        if (!node)
            return std::nullopt;
        return *node;

    case DataQueryOp::Has:
        return nlohmann::json(node != nullptr);

    case DataQueryOp::Keys: {
        if (!node || !node->is_object())
            return std::nullopt;
        auto keys = nlohmann::json::array();
        // Object iteration is in key order.
        for (auto & [key, _] : node->items())
            keys.push_back(key);
        return keys;
    }

    case DataQueryOp::Find: {
        auto keys = nlohmann::json::array();
        if (!node || !node->is_object())
            return keys;
        // Object iteration is in key order, so the result is sorted.
        for (auto & [key, child] : node->items()) {
            if (!child.is_object())
                continue;
            auto field = child.find(query.where);
            if (field != child.end() && field->is_string() && field->get_ref<const std::string &>() == query.value)
                keys.push_back(key);
        }
        return keys;
    }
    }
    unreachable();
}

// ============================================================================
// Observation lines
// ============================================================================

void appendDataQueryAnswer(std::string & out, const DataQueryAnswer & answer)
{
    if (!answer) {
        out += absentAnswer;
        return;
    }
    // Compact JSON escapes every control character, so the text never
    // contains a field or line separator. Invalid UTF-8 is replaced rather
    // than thrown on: both sides see the same bytes either way.
    auto text = answer->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (text.size() <= maxVerbatimAnswerBytes) {
        out += text;
        return;
    }
    out += "sha256:";
    out += hashString(HashAlgorithm::SHA256, text).to_string(HashFormat::Base16, false);
}

void appendDataObservation(std::string & out, const DataQuery & query, std::string_view answer)
{
    out += dataFormatName(query.format);
    out += fieldSeparator;
    out += dataQueryOpName(query.op);
    if (query.op == DataQueryOp::Find) {
        out += fieldSeparator;
        out += query.where;
        out += fieldSeparator;
        out += query.value;
    }
    for (auto segment : query.at) {
        out += fieldSeparator;
        out += segment;
    }
    out += fieldSeparator;
    out += answer;
}

static std::vector<std::string_view> split(std::string_view s, char separator)
{
    std::vector<std::string_view> fields;
    while (true) {
        auto end = s.find(separator);
        fields.push_back(s.substr(0, end));
        if (end == std::string_view::npos)
            return fields;
        s.remove_prefix(end + 1);
    }
}

std::optional<DataObservation> parseDataObservation(std::string_view line)
{
    auto fields = split(line, fieldSeparator);
    // format, op, answer at least
    if (fields.size() < 3)
        return std::nullopt;

    DataObservation observation;
    auto format = parseDataFormat(fields[0]);
    auto op = parseDataQueryOp(fields[1]);
    if (!format || !op)
        return std::nullopt;
    observation.query.format = *format;
    observation.query.op = *op;
    observation.answer = fields.back();

    size_t atBegin = 2;
    if (*op == DataQueryOp::Find) {
        // format, op, where, value, answer at least
        if (fields.size() < 5)
            return std::nullopt;
        observation.query.where = fields[2];
        observation.query.value = fields[3];
        atBegin = 4;
    }
    observation.query.at.assign(fields.begin() + atBegin, fields.end() - 1);
    return observation;
}

// ============================================================================
// Fingerprints and observations
// ============================================================================

std::string dataQueryFingerprint(std::vector<std::string_view> lines)
{
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    std::string fingerprint(dataQueryFingerprintPrefix);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0)
            fingerprint += lineSeparator;
        fingerprint += lines[i];
    }
    return fingerprint;
}

std::optional<std::vector<std::string_view>> dataQueryFingerprintLines(std::string_view fingerprint)
{
    if (!fingerprint.starts_with(dataQueryFingerprintPrefix))
        return std::nullopt;
    auto lines = fingerprint.substr(dataQueryFingerprintPrefix.size());
    if (lines.empty())
        return std::nullopt;
    return split(lines, lineSeparator);
}

void appendDataAccess(std::string & out, std::string_view file, std::string_view line)
{
    out += file;
    out += '\0';
    out += line;
}

std::optional<DataAccess> splitDataAccess(std::string_view observation)
{
    auto separator = observation.find('\0');
    if (separator == std::string_view::npos)
        return std::nullopt;
    return DataAccess{
        .file = observation.substr(0, separator),
        .line = observation.substr(separator + 1),
    };
}

} // namespace nix
