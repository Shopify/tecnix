/**
 * `builtins.tecnixQueryFile`: query a JSON or TOML file and depend on the
 * answer, not on the file. The query engine and the observation format are in
 * tecnix/data-query.cc; the cache validator re-asks the recorded questions in
 * tecnix/eval-cache.cc.
 */

#include "tecnix/eval-data.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/json-to-value.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/tecnix/data-query.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/expr/tecnix/source-deps.hh"
#include "nix/expr/tecnix/thread-state.hh"

namespace nix {

/**
 * Where a queried file lives: in the Tecnix repo (read unrecorded, each query
 * records its own observation under `repoPath`), or anywhere else (read like
 * `readFile`, recording whatever that records).
 */
struct QueriedFile
{
    SourcePath path;
    std::optional<CanonPath> repoPath;
};

static std::optional<std::string> readQueriedFile(EvalState & state, const QueriedFile & file)
{
    if (file.repoPath)
        return readSourceFileUnrecorded(*getTecnixRepoAccessor(state), *file.repoPath);

    // Outside the repo accessor this is an ordinary existence check and read.
    // Existence is recorded at the primop layer, like `pathExists`.
    if (currentTecnixThreadState.trackingContext && file.path.accessor->tracksEvalAccesses(file.path.path))
        file.path.accessor->recordEvalAccess(file.path.path);
    // `maybeLstat` answers "absent" for a path pure evaluation forbids; only
    // `lstat` tells a missing file (FileNotFound, so: absent) from a forbidden
    // one (RestrictedPathError, which propagates, as it does from `readFile`).
    SourceAccessor::Stat st;
    try {
        st = file.path.lstat();
    } catch (FileNotFound &) {
        return std::nullopt;
    }
    if (st.type != SourceAccessor::tRegular)
        return std::nullopt;
    return file.path.readFile();
}

/** The parsed file, once per evaluation and shared by every query on it. */
static std::shared_ptr<const ParsedDataFile>
queriedDataFile(EvalState & state, const PosIdx pos, const QueriedFile & file, DataFormat format)
{
    auto & cache = *state.tecnixEvalData().dataFileCache;

    std::string key = file.path.to_string();
    key += '\0';
    key += dataFormatName(format);

    std::shared_ptr<const ParsedDataFile> parsed;
    if (cache.cvisit(key, [&](const auto & kv) { parsed = kv.second; }))
        return parsed;

    try {
        auto content = readQueriedFile(state, file);
        parsed = std::make_shared<const ParsedDataFile>(
            content ? ParsedDataFile(parseDataFile(format, *content)) : ParsedDataFile());
    } catch (DataFileParseError & e) {
        state.error<EvalError>("while parsing '%s' as %s: %s", file.path, dataFormatName(format), e.what())
            .atPos(pos)
            .debugThrow();
    }
    // Two threads parsing the same content produce interchangeable documents.
    cache.try_emplace(std::move(key), parsed);
    return parsed;
}

static std::string_view forceQueryField(EvalState & state, const PosIdx pos, Value & v, std::string_view argument)
{
    auto s =
        state.forceStringNoCtx(v, pos, fmt("while evaluating the '%s' argument to builtins.tecnixQueryFile", argument));
    if (!isValidDataQueryField(s))
        state
            .error<EvalError>(
                "the '%s' argument to builtins.tecnixQueryFile may not contain a tab, newline or NUL", argument)
            .atPos(pos)
            .debugThrow();
    return s;
}

static void prim_tecnixQueryFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.tecnixQueryFile");
    auto & attrs = *args[0]->attrs();

    const Attr *pathAttr = nullptr, *formatAttr = nullptr, *opAttr = nullptr, *atAttr = nullptr, *whereAttr = nullptr,
               *valueAttr = nullptr;
    for (auto & attr : attrs) {
        std::string_view name = state.symbols[attr.name];
        if (name == "path")
            pathAttr = &attr;
        else if (name == "format")
            formatAttr = &attr;
        else if (name == "op")
            opAttr = &attr;
        else if (name == "at")
            atAttr = &attr;
        else if (name == "where")
            whereAttr = &attr;
        else if (name == "value")
            valueAttr = &attr;
        else
            state.error<EvalError>("unexpected attribute '%s' passed to builtins.tecnixQueryFile", name)
                .atPos(attr.pos)
                .debugThrow();
    }
    for (auto [attr, name] : {std::pair{pathAttr, "path"}, std::pair{opAttr, "op"}, std::pair{atAttr, "at"}})
        if (!attr)
            state.error<EvalError>("builtins.tecnixQueryFile: missing attribute '%s'", name).atPos(pos).debugThrow();

    DataQuery query;

    auto opName = state.forceStringNoCtx(
        *opAttr->value, opAttr->pos, "while evaluating the 'op' argument to builtins.tecnixQueryFile");
    if (auto op = parseDataQueryOp(opName))
        query.op = *op;
    else
        state
            .error<EvalError>(
                "unknown op '%s' passed to builtins.tecnixQueryFile (expected get, has, keys or find)", opName)
            .atPos(opAttr->pos)
            .debugThrow();

    QueriedFile file{.path = state.realisePath(pathAttr->pos, *pathAttr->value)};
    file.repoPath = tecnixRepoRelativePath(state, file.path);

    if (formatAttr) {
        auto formatName = state.forceStringNoCtx(
            *formatAttr->value, formatAttr->pos, "while evaluating the 'format' argument to builtins.tecnixQueryFile");
        if (auto format = parseDataFormat(formatName))
            query.format = *format;
        else
            state
                .error<EvalError>(
                    "unknown format '%s' passed to builtins.tecnixQueryFile (expected json or toml)", formatName)
                .atPos(formatAttr->pos)
                .debugThrow();
    } else if (auto format = dataFormatFromExtension(file.path.path.abs())) {
        query.format = *format;
    } else {
        state
            .error<EvalError>(
                "cannot infer the format of '%s' from its extension; pass 'format' to builtins.tecnixQueryFile",
                file.path)
            .atPos(pathAttr->pos)
            .debugThrow();
    }

    state.forceList(*atAttr->value, atAttr->pos, "while evaluating the 'at' argument to builtins.tecnixQueryFile");
    query.at.reserve(atAttr->value->listSize());
    for (auto * segment : atAttr->value->listView())
        query.at.push_back(forceQueryField(state, atAttr->pos, *segment, "at"));

    if (query.op == DataQueryOp::Find) {
        if (!whereAttr || !valueAttr)
            state.error<EvalError>("builtins.tecnixQueryFile requires 'where' and 'value' attributes for op 'find'")
                .atPos(pos)
                .debugThrow();
        query.where = forceQueryField(state, whereAttr->pos, *whereAttr->value, "where");
        query.value = forceQueryField(state, valueAttr->pos, *valueAttr->value, "value");
    } else if (whereAttr || valueAttr) {
        state.error<EvalError>("builtins.tecnixQueryFile accepts 'where' and 'value' only for op 'find'")
            .atPos(pos)
            .debugThrow();
    }

    auto answer = evaluateDataQuery(*queriedDataFile(state, pos, file, query.format), query);

    // The observation: this file, this question, this answer. Built in scratch
    // buffers so a repeated query allocates nothing; interning copies the
    // string on first sight only, like a path.
    if (auto ctx = currentTecnixThreadState.trackingContext; ctx && file.repoPath) {
        static thread_local std::string encodedAnswer, line, observation;
        encodedAnswer.clear();
        appendDataQueryAnswer(encodedAnswer, answer);
        line.clear();
        appendDataObservation(line, query, encodedAnswer);
        observation.clear();
        appendDataAccess(observation, file.repoPath->rel(), line);
        ctx->recordAccess(observation);
    }

    if (!answer)
        v.mkNull();
    else
        parseJSON(state, answer->dump(), v);
}

static RegisterPrimOp primop_tecnixQueryFile({
    .name = "__tecnixQueryFile",
    .args = {"query"},
    .doc = R"(
      Query a JSON or TOML file, depending on the answer rather than on the file.

      ```nix
      builtins.tecnixQueryFile {
        path = ../../.meta/manifest.json;   # a file, as a path or string
        format = "json";                    # "json" or "toml"; default: from the extension
        op = "get";                         # "get", "has", "keys" or "find"
        at = [ "//areas/tools/dev" ];       # object keys, root down
        # find only:
        # where = "id";                     # field of each child of `at`
        # value = "W-123456";               # the string it must equal
      }
      ```

      | `op`   | returns                                                                 | when `at` does not resolve |
      |--------|-------------------------------------------------------------------------|----------------------------|
      | `get`  | the value at `at`, as `builtins.fromJSON` would produce it              | `null`                     |
      | `has`  | `true` if `at` resolves (a present `null` counts)                       | `false`                    |
      | `keys` | the sorted attribute names of the object at `at`                        | `null` (also for a non-object) |
      | `find` | the sorted keys of the children of the object at `at` whose string field `where` equals `value` | `[ ]` |

      `at` is a list, not a dotted string, so keys may contain `/` and `.`.
      Segments, `where` and `value` are strings and may not contain a tab,
      newline or NUL. A missing file is not an error: every op sees an absent
      document. A file that does not parse is an error. TOML datetimes become
      `{ _type = "timestamp"; value = "..."; }`.

      Under Tecnix source tracking, a query on a file in the repository records
      what was asked and what the answer was, not the file: the target's
      closure entry for the file becomes a `query:` fingerprint listing one
      observation per distinct query, and the entry stays valid while the file
      keeps giving those answers, whatever else in it changes. A target that
      also reads the file whole depends on the whole file, as before. Outside
      the repository the builtin is `fromJSON (readFile path)` plus the
      selection.
    )",
    .impl = prim_tecnixQueryFile,
});

} // namespace nix
