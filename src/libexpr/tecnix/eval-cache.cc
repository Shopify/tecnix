/**
 * The persistent Tecnix evaluation cache (see eval-cache.hh for the API and
 * plans/tecnix-target-eval-caching/ for the design): DependencyShards rows in
 * SQLite holding TXDC blobs — bounded per-key source-closure candidate
 * histories, validated against current fingerprints on every use.
 */

#include "nix/expr/tecnix/eval-cache.hh"
#include "nix/expr/tecnix/eval-cache-row.hh"

#include "nix/expr/eval-inline.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/store/globals.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/signals.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"
#include "nix/util/sync.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nix {

/** A stored row this version cannot read; the row is rebuilt from the current evaluation. */
MakeError(MalformedTecnixCacheRow, Error);

static std::atomic<uint64_t> nextDependencyFingerprintCacheGeneration{1};

DependencyFingerprintCache::DependencyFingerprintCache()
    : generation(nextDependencyFingerprintCacheGeneration.fetch_add(1, std::memory_order_relaxed))
{
}

struct DependencyFingerprintThreadLocalCache
{
    uint64_t generation = 0;
    boost::unordered_flat_map<std::string, std::optional<std::string>, StringViewHash, std::equal_to<>> fingerprints;

    DependencyFingerprintThreadLocalCache()
    {
        fingerprints.reserve(8192);
    }
};

static DependencyFingerprintThreadLocalCache & getDependencyFingerprintThreadCache(DependencyFingerprintCache & cache);

// Unshipped development cache: the schema may change incompatibly at any
// time, with no migrations. Foreign or stale rows are rejected by content
// validation (a miss), and deleting the database is always safe.
static const char * tecnixEvalCacheSchema = R"sql(
create table if not exists DependencyShards (
    gitDir       text not null,
    resolver     text not null,
    argsKey text not null,
    shard        integer not null,
    dependencies blob not null,
    timestamp    integer not null,
    primary key (gitDir, resolver, argsKey, shard)
);

)sql";

// Returns a pointer into the thread-local fingerprint memo. Consume it immediately;
// it must not be retained across another memo insertion, which may rehash the map.
static const std::optional<std::string> *
dependencyFingerprintCached(ref<SourceAccessor> accessor, std::string_view path, DependencyFingerprintCache & cache);

struct TecnixEvalCache
{
    struct State
    {
        SQLite db;
        SQLiteStmt upsertShard, lookupShard;
        /** The largest blob this connection can store: the only ceiling on a row's size. */
        size_t maxRowBytes = 0;
    };

    Sync<State> _state;

    TecnixEvalCache()
    {
        auto state(_state.lock());

        // v2: target candidates carry the evaluated drvPath as their payload.
        auto dbPath = getCacheDir() / "tecnix-eval-cache-v2.sqlite";
        createDirs(dbPath.parent_path());

        state->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
        state->db.isCache();
        state->db.exec(tecnixEvalCacheSchema);
        state->maxRowBytes = state->db.maxLength();

        state->upsertShard.create(
            state->db,
            "insert or replace into DependencyShards(gitDir, resolver, argsKey, shard, dependencies, timestamp) "
            "values (?, ?, ?, ?, ?, ?)");
        state->lookupShard.create(
            state->db,
            "select dependencies from DependencyShards where gitDir = ? and resolver = ? and argsKey = ? and shard = ?");
    }

    static constexpr std::string_view dependencyBlobMagic = "TXDC";

    /**
     * Bump when a reader could misread rows written before the change (layout,
     * record sizes, field or payload meaning, fingerprint format, an ordering
     * the reader relies on); not for rewrites that emit the same bytes, or for
     * limits and retention policy. If older evaluators may still write to the
     * cache directory, bump the database file name instead: an old writer
     * overwrites a row it cannot open. The formatIsPinned unit test fails when
     * the bytes change.
     *
     * v2: target payloads now carry {drvPath, outputName}; old v1 closures
     * omitted the selected-output input and can silently serve the wrong output.
     */
    static constexpr uint32_t dependencyBlobVersion = 2;
    static constexpr uint32_t dependencyBlobFlags = 0;
    static constexpr size_t dependencyBlobFieldCount = 16;
    static constexpr size_t dependencyBlobHeaderSize = 4 + dependencyBlobFieldCount * sizeof(uint32_t);
    static constexpr size_t dependencyShardCount = 256;
    /**
     * Targets per shard row. This and the history setting (candidates per
     * key) are the only bounds on a row's contents: rows have no size cap of
     * their own, and the only hard ceiling is what the SQLite connection can
     * store in one blob (State::maxRowBytes; 1e9 in a stock build), past
     * which a row is discarded with a warning. For scale, a dense shard on
     * the world repo costs ~0.3 MB per unit of history (~115 targets x ~290
     * pair records x 8 bytes; measured Sep 2026).
     */
    static constexpr size_t maxDependencyBlobTargets = 1024;

    enum DependencyBlobField : size_t {
        blobVersionField = 0,
        blobFlagsField,
        blobTargetCountField,
        blobPathCountField,
        blobFingerprintCountField,
        blobPayloadCountField,
        blobCandidateCountField,
        blobPairCountField,
        blobTargetOffsetsOffsetField,
        blobTargetRecordsOffsetField,
        blobPathOffsetsOffsetField,
        blobFingerprintOffsetsOffsetField,
        blobPayloadOffsetsOffsetField,
        blobCandidateRecordsOffsetField,
        blobPairsOffsetField,
        blobEndOffsetField,
    };

    static uint32_t dependencyShardForTarget(std::string_view target)
    {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : target) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return static_cast<uint32_t>(hash % dependencyShardCount);
    }

    static void appendU32(std::string & out, uint32_t value)
    {
        char bytes[4] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
            static_cast<char>((value >> 16) & 0xff),
            static_cast<char>((value >> 24) & 0xff),
        };
        out.append(bytes, sizeof(bytes));
    }

    static void writeU32(std::string & out, size_t offset, uint32_t value)
    {
        assert(offset + sizeof(uint32_t) <= out.size());
        out[offset + 0] = static_cast<char>(value & 0xff);
        out[offset + 1] = static_cast<char>((value >> 8) & 0xff);
        out[offset + 2] = static_cast<char>((value >> 16) & 0xff);
        out[offset + 3] = static_cast<char>((value >> 24) & 0xff);
    }

    static std::optional<uint32_t> readU32(std::string_view blob, size_t offset)
    {
        if (offset + sizeof(uint32_t) > blob.size())
            return std::nullopt;
        auto * data = reinterpret_cast<const unsigned char *>(blob.data() + offset);
        return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
               | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    }

    static size_t align4(size_t value)
    {
        return (value + 3) & ~size_t{3};
    }

    static void padTo4(std::string & out)
    {
        while (out.size() != align4(out.size()))
            out.push_back('\0');
    }

    static bool u32Fits(size_t value)
    {
        return value <= std::numeric_limits<uint32_t>::max();
    }

    static uint32_t checkedU32(size_t value, std::string_view what)
    {
        if (!u32Fits(value))
            throw Error("Tecnix dependency cache %s is too large", what);
        return static_cast<uint32_t>(value);
    }

    static void setBlobField(std::string & out, DependencyBlobField field, uint32_t value)
    {
        writeU32(out, 4 + static_cast<size_t>(field) * sizeof(uint32_t), value);
    }

    /**
     * A write transaction that takes the database write lock up front. Shard
     * rows are updated read-merge-write, and evaluators on one machine share
     * the database: holding the lock across the read means the merge always
     * starts from the latest committed row, so concurrent writers serialize
     * and accumulate instead of overwriting each other. (A deferred `begin`
     * would read first and then fail to upgrade once another writer had
     * committed in between.)
     */
    struct SQLiteImmediateTxn
    {
        SQLite & db;
        bool active = false;

        explicit SQLiteImmediateTxn(SQLite & db)
            : db(db)
        {
            db.exec("begin immediate;");
            active = true;
        }

        void commit()
        {
            db.exec("commit;");
            active = false;
        }

        ~SQLiteImmediateTxn()
        {
            try {
                if (active)
                    db.exec("rollback;");
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        }
    };

    struct DependencyBlobView
    {
        std::string_view blob;
        uint32_t targetCount = 0;
        uint32_t pathCount = 0;
        uint32_t fingerprintCount = 0;
        uint32_t payloadCount = 0;
        uint32_t candidateCount = 0;
        uint32_t pairCount = 0;
        uint32_t targetOffsetsOffset = 0;
        uint32_t targetRecordsOffset = 0;
        uint32_t pathOffsetsOffset = 0;
        uint32_t fingerprintOffsetsOffset = 0;
        uint32_t payloadOffsetsOffset = 0;
        uint32_t candidateRecordsOffset = 0;
        uint32_t pairsOffset = 0;
        uint32_t endOffset = 0;

        static constexpr uint32_t targetRecordU32s = 2;
        static constexpr uint32_t candidateRecordU32s = 3;
        static constexpr uint32_t pairRecordU32s = 2;

        /** The format version a blob declares, if it is a TXDC blob at all. */
        static std::optional<uint32_t> declaredVersion(std::string_view blob)
        {
            if (blob.size() < dependencyBlobHeaderSize
                || blob.substr(0, dependencyBlobMagic.size()) != dependencyBlobMagic)
                return std::nullopt;
            return readU32(blob, 4 + static_cast<size_t>(blobVersionField) * sizeof(uint32_t));
        }

        static std::optional<DependencyBlobView> open(std::string_view blob)
        {
            if (blob.size() < dependencyBlobHeaderSize)
                return std::nullopt;
            if (blob.substr(0, dependencyBlobMagic.size()) != dependencyBlobMagic)
                return std::nullopt;

            auto field = [&](DependencyBlobField f) -> std::optional<uint32_t> {
                return readU32(blob, 4 + static_cast<size_t>(f) * sizeof(uint32_t));
            };

            auto version = field(blobVersionField);
            auto flags = field(blobFlagsField);
            if (!version || *version != dependencyBlobVersion || !flags || *flags != dependencyBlobFlags)
                return std::nullopt;

            auto targetCount = field(blobTargetCountField);
            auto pathCount = field(blobPathCountField);
            auto fingerprintCount = field(blobFingerprintCountField);
            auto payloadCount = field(blobPayloadCountField);
            auto candidateCount = field(blobCandidateCountField);
            auto pairCount = field(blobPairCountField);
            auto targetOffsetsOffset = field(blobTargetOffsetsOffsetField);
            auto targetRecordsOffset = field(blobTargetRecordsOffsetField);
            auto pathOffsetsOffset = field(blobPathOffsetsOffsetField);
            auto fingerprintOffsetsOffset = field(blobFingerprintOffsetsOffsetField);
            auto payloadOffsetsOffset = field(blobPayloadOffsetsOffsetField);
            auto candidateRecordsOffset = field(blobCandidateRecordsOffsetField);
            auto pairsOffset = field(blobPairsOffsetField);
            auto endOffset = field(blobEndOffsetField);
            if (!targetCount || !pathCount || !fingerprintCount || !payloadCount || !candidateCount || !pairCount
                || !targetOffsetsOffset || !targetRecordsOffset || !pathOffsetsOffset || !fingerprintOffsetsOffset
                || !payloadOffsetsOffset || !candidateRecordsOffset || !pairsOffset || !endOffset)
                return std::nullopt;

            DependencyBlobView view;
            view.blob = blob;
            view.targetCount = *targetCount;
            view.pathCount = *pathCount;
            view.fingerprintCount = *fingerprintCount;
            view.payloadCount = *payloadCount;
            view.candidateCount = *candidateCount;
            view.pairCount = *pairCount;
            view.targetOffsetsOffset = *targetOffsetsOffset;
            view.targetRecordsOffset = *targetRecordsOffset;
            view.pathOffsetsOffset = *pathOffsetsOffset;
            view.fingerprintOffsetsOffset = *fingerprintOffsetsOffset;
            view.payloadOffsetsOffset = *payloadOffsetsOffset;
            view.candidateRecordsOffset = *candidateRecordsOffset;
            view.pairsOffset = *pairsOffset;
            view.endOffset = *endOffset;

            // Counts need no separate caps: the section checks below pin each
            // one to a region of the blob.
            if (view.targetCount == 0 || view.targetCount > maxDependencyBlobTargets || view.payloadCount == 0
                || view.candidateCount == 0 || view.endOffset != blob.size())
                return std::nullopt;

            auto sectionOk = [&](uint32_t begin, uint32_t end, size_t minBytes = 0) {
                return begin >= dependencyBlobHeaderSize && begin <= end && end <= view.endOffset
                       && static_cast<size_t>(end - begin) >= minBytes;
            };
            auto stringTableOk = [&](uint32_t begin, uint32_t end, uint32_t count) {
                if (!sectionOk(begin, end, (static_cast<size_t>(count) + 1) * sizeof(uint32_t)))
                    return false;
                auto bytesOffset = static_cast<size_t>(begin) + (static_cast<size_t>(count) + 1) * sizeof(uint32_t);
                if (bytesOffset > end)
                    return false;
                uint32_t previous = 0;
                auto byteCount = static_cast<uint32_t>(end - bytesOffset);
                for (uint32_t i = 0; i <= count; i++) {
                    auto current = view.arrayValue(begin, i);
                    if (!current || *current < previous || *current > byteCount)
                        return false;
                    previous = *current;
                }
                return true;
            };
            if (!stringTableOk(view.targetOffsetsOffset, view.targetRecordsOffset, view.targetCount)
                || !sectionOk(
                    view.targetRecordsOffset,
                    view.pathOffsetsOffset,
                    static_cast<size_t>(view.targetCount) * targetRecordU32s * 4)
                || !stringTableOk(view.pathOffsetsOffset, view.fingerprintOffsetsOffset, view.pathCount)
                || !stringTableOk(view.fingerprintOffsetsOffset, view.payloadOffsetsOffset, view.fingerprintCount)
                || !stringTableOk(view.payloadOffsetsOffset, view.candidateRecordsOffset, view.payloadCount)
                || !sectionOk(
                    view.candidateRecordsOffset,
                    view.pairsOffset,
                    static_cast<size_t>(view.candidateCount) * candidateRecordU32s * 4)
                || !sectionOk(
                    view.pairsOffset, view.endOffset, static_cast<size_t>(view.pairCount) * pairRecordU32s * 4))
                return std::nullopt;

            auto exactRecordSection = [](uint32_t begin, uint32_t end, uint32_t count, uint32_t recordU32s) {
                return static_cast<size_t>(end - begin) == static_cast<size_t>(count) * recordU32s * 4;
            };
            if (!exactRecordSection(
                    view.targetRecordsOffset, view.pathOffsetsOffset, view.targetCount, targetRecordU32s)
                || !exactRecordSection(
                    view.candidateRecordsOffset, view.pairsOffset, view.candidateCount, candidateRecordU32s)
                || !exactRecordSection(view.pairsOffset, view.endOffset, view.pairCount, pairRecordU32s))
                return std::nullopt;

            return view;
        }

        std::optional<uint32_t> arrayValue(uint32_t offset, uint32_t index) const
        {
            return readU32(blob, static_cast<size_t>(offset) + static_cast<size_t>(index) * sizeof(uint32_t));
        }

        std::optional<std::string_view>
        stringFromTable(uint32_t offsetsOffset, uint32_t count, uint32_t tableEnd, uint32_t id) const
        {
            if (id >= count)
                return std::nullopt;
            auto bytesOffset = static_cast<size_t>(offsetsOffset) + (static_cast<size_t>(count) + 1) * sizeof(uint32_t);
            if (bytesOffset > tableEnd)
                return std::nullopt;
            auto begin = arrayValue(offsetsOffset, id);
            auto end = arrayValue(offsetsOffset, id + 1);
            if (!begin || !end || *begin > *end
                || static_cast<size_t>(*end) > static_cast<size_t>(tableEnd - bytesOffset))
                return std::nullopt;
            return std::string_view(blob.data() + bytesOffset + *begin, *end - *begin);
        }

        std::optional<std::string_view> target(uint32_t id) const
        {
            return stringFromTable(targetOffsetsOffset, targetCount, targetRecordsOffset, id);
        }

        std::optional<uint32_t> targetField(uint32_t targetId, uint32_t field) const
        {
            if (targetId >= targetCount || field >= targetRecordU32s)
                return std::nullopt;
            return arrayValue(targetRecordsOffset, targetId * targetRecordU32s + field);
        }

        std::optional<uint32_t> targetCandidateStart(uint32_t targetId) const
        {
            return targetField(targetId, 0);
        }

        std::optional<uint32_t> targetCandidateCount(uint32_t targetId) const
        {
            return targetField(targetId, 1);
        }

        std::optional<uint32_t> findTarget(std::string_view name) const
        {
            uint32_t low = 0;
            uint32_t high = targetCount;
            while (low < high) {
                uint32_t mid = low + (high - low) / 2;
                auto current = target(mid);
                if (!current)
                    return std::nullopt;
                if (*current < name)
                    low = mid + 1;
                else
                    high = mid;
            }
            if (low >= targetCount)
                return std::nullopt;
            auto current = target(low);
            if (!current || *current != name)
                return std::nullopt;
            return low;
        }

        std::optional<std::string_view> path(uint32_t id) const
        {
            return stringFromTable(pathOffsetsOffset, pathCount, fingerprintOffsetsOffset, id);
        }

        std::optional<std::string_view> fingerprint(uint32_t id) const
        {
            return stringFromTable(fingerprintOffsetsOffset, fingerprintCount, payloadOffsetsOffset, id);
        }

        std::optional<std::string_view> payload(uint32_t id) const
        {
            return stringFromTable(payloadOffsetsOffset, payloadCount, candidateRecordsOffset, id);
        }

        std::optional<uint32_t> candidateField(uint32_t candidateIndex, uint32_t field) const
        {
            if (candidateIndex >= candidateCount || field >= candidateRecordU32s)
                return std::nullopt;
            return arrayValue(candidateRecordsOffset, candidateIndex * candidateRecordU32s + field);
        }

        std::optional<uint32_t> candidatePairStart(uint32_t candidateIndex) const
        {
            return candidateField(candidateIndex, 0);
        }

        std::optional<uint32_t> candidatePairCount(uint32_t candidateIndex) const
        {
            return candidateField(candidateIndex, 1);
        }

        std::optional<uint32_t> candidatePayload(uint32_t candidateIndex) const
        {
            return candidateField(candidateIndex, 2);
        }

        std::optional<uint32_t> pairField(uint32_t pairIndex, uint32_t field) const
        {
            if (pairIndex >= pairCount || field >= pairRecordU32s)
                return std::nullopt;
            return arrayValue(pairsOffset, pairIndex * pairRecordU32s + field);
        }

        std::optional<uint32_t> pairPath(uint32_t pairIndex) const
        {
            return pairField(pairIndex, 0);
        }

        std::optional<uint32_t> pairFingerprint(uint32_t pairIndex) const
        {
            return pairField(pairIndex, 1);
        }

        struct PairIds
        {
            uint32_t path, fingerprint;
        };

        /** Both ids of a pair, checked against the string tables, for loops over whole candidates. */
        std::optional<PairIds> pairIds(uint32_t pairIndex) const
        {
            auto path = pairPath(pairIndex);
            auto fingerprint = pairFingerprint(pairIndex);
            if (!path || !fingerprint || *path >= pathCount || *fingerprint >= fingerprintCount)
                return std::nullopt;
            return PairIds{*path, *fingerprint};
        }

        struct CurrentFingerprints
        {
            ref<SourceAccessor> accessor;
            DependencyFingerprintCache * fingerprintCache;
        };

        CurrentFingerprints
        currentFingerprints(ref<SourceAccessor> accessor, DependencyFingerprintCache & fingerprintCache) const
        {
            return CurrentFingerprints{.accessor = accessor, .fingerprintCache = &fingerprintCache};
        }

        std::optional<std::string_view> currentFingerprint(uint32_t pathId, CurrentFingerprints & current) const
        {
            auto p = path(pathId);
            if (!p)
                return std::nullopt;
            auto cached = dependencyFingerprintCached(current.accessor, *p, *current.fingerprintCache);
            if (!cached || !*cached)
                return std::nullopt;
            return std::string_view(**cached);
        }

        bool candidateMatches(uint32_t candidateIndex, CurrentFingerprints & currentFingerprints) const
        {
            auto pairStart = candidatePairStart(candidateIndex);
            auto count = candidatePairCount(candidateIndex);
            auto payloadId = candidatePayload(candidateIndex);
            if (!pairStart || !count || !payloadId)
                return false;
            if (*pairStart > pairCount || *count > pairCount - *pairStart || *payloadId >= payloadCount)
                return false;
            for (uint32_t i = 0; i < *count; i++) {
                auto pairIndex = *pairStart + i;
                auto pathId = pairPath(pairIndex);
                auto fingerprintId = pairFingerprint(pairIndex);
                if (!pathId || !fingerprintId || *pathId >= pathCount || *fingerprintId >= fingerprintCount)
                    return false;
                auto currentFp = currentFingerprint(*pathId, currentFingerprints);
                auto expectedFp = fingerprint(*fingerprintId);
                if (!currentFp || !expectedFp || *currentFp != *expectedFp)
                    return false;
            }
            return true;
        }

        std::optional<uint32_t>
        findMatchingCandidate(std::string_view targetName, CurrentFingerprints & currentFingerprints) const
        {
            auto targetId = findTarget(targetName);
            if (!targetId)
                return std::nullopt;
            auto candidateStart = targetCandidateStart(*targetId);
            auto count = targetCandidateCount(*targetId);
            if (!candidateStart || !count || *candidateStart > candidateCount
                || *count > candidateCount - *candidateStart)
                return std::nullopt;
            for (uint32_t i = 0; i < *count; i++) {
                auto candidateIndex = *candidateStart + i;
                if (candidateMatches(candidateIndex, currentFingerprints))
                    return candidateIndex;
            }
            return std::nullopt;
        }

        std::optional<std::string_view> candidatePayloadValue(uint32_t candidateIndex) const
        {
            auto payloadId = candidatePayload(candidateIndex);
            if (!payloadId)
                return std::nullopt;
            return payload(*payloadId);
        }
    };

    static void appendStringTable(std::string & out, std::span<const std::string_view> values)
    {
        std::vector<uint32_t> offsets;
        offsets.reserve(values.size() + 1);
        size_t bytes = 0;
        offsets.push_back(0);
        for (auto & value : values) {
            bytes += value.size();
            if (!u32Fits(bytes))
                throw Error("Tecnix dependency cache row string table is too large");
            offsets.push_back(static_cast<uint32_t>(bytes));
        }
        for (auto offset : offsets)
            appendU32(out, offset);
        for (auto & value : values)
            out.append(value.data(), value.size());
        padTo4(out);
    }

    // ---- Writing rows -------------------------------------------------------
    //
    // A row is rewritten in its own packed form. The stored row's string
    // tables are sorted, so the merged tables are a linear merge of the
    // surviving stored strings with the freshly learned ones; stored
    // candidates are copied through as id pairs under the resulting remap,
    // and only fresh candidates are resolved from strings. Nothing is
    // materialized per pair, so a write costs the old row, the new row, and a
    // few bytes per string, whatever the history bound.

    /**
     * A freshly learned closure, normalized to unique paths in sorted order.
     * Views into the caller's upsert entries.
     */
    struct FreshCandidate
    {
        std::string_view target;
        std::vector<std::pair<std::string_view, std::string_view>> pairs;
        std::string_view payload;
    };

    static FreshCandidate freshCandidate(const TecnixDependencyUpsert & update)
    {
        FreshCandidate fresh{.target = update.target, .payload = update.payload};
        fresh.pairs.reserve(update.dependencies->size());
        for (auto & dependency : *update.dependencies)
            fresh.pairs.emplace_back(dependency.path, dependency.fingerprint);
        std::sort(fresh.pairs.begin(), fresh.pairs.end());

        auto kept = fresh.pairs.begin();
        for (auto it = fresh.pairs.begin(); it != fresh.pairs.end(); ++it) {
            if (kept != fresh.pairs.begin() && (kept - 1)->first == it->first) {
                if ((kept - 1)->second != it->second)
                    throw Error(
                        "Tecnix dependency cache closure contains conflicting fingerprints for path '%s'", it->first);
                continue;
            }
            *kept++ = *it;
        }
        fresh.pairs.erase(kept, fresh.pairs.end());
        return fresh;
    }

    /** Where a candidate of the row being written comes from. */
    struct StoredCandidate
    {
        uint32_t index;
    };

    struct FreshCandidateRef
    {
        size_t index;
    };

    using CandidateSource = std::variant<StoredCandidate, FreshCandidateRef>;

    struct PlannedTarget
    {
        std::string_view name;
        /** Newest first, at most the history bound. */
        std::vector<CandidateSource> candidates;
    };

    struct StoredCandidatePairs
    {
        uint32_t start, count;
    };

    static StoredCandidatePairs storedCandidatePairs(const DependencyBlobView & view, uint32_t candidate)
    {
        auto start = view.candidatePairStart(candidate);
        auto count = view.candidatePairCount(candidate);
        if (!start || !count || *start > view.pairCount || *count > view.pairCount - *start)
            throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
        return {*start, *count};
    }

    /** Whether `source` holds the same closure and payload as `fresh`. */
    static bool sameCandidate(
        const DependencyBlobView * view,
        CandidateSource source,
        std::span<const FreshCandidate> allFresh,
        const FreshCandidate & fresh)
    {
        if (auto * ref = std::get_if<FreshCandidateRef>(&source)) {
            auto & other = allFresh[ref->index];
            return other.payload == fresh.payload && other.pairs == fresh.pairs;
        }
        auto stored = std::get<StoredCandidate>(source).index;
        auto payload = view->candidatePayloadValue(stored);
        if (!payload)
            throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
        auto pairs = storedCandidatePairs(*view, stored);
        if (*payload != fresh.payload || pairs.count != fresh.pairs.size())
            return false;
        // Stored pairs are sorted by path id, and ids follow string order, so
        // both sides are in the same order.
        for (uint32_t i = 0; i < pairs.count; i++) {
            auto ids = view->pairIds(pairs.start + i);
            auto path = ids ? view->path(ids->path) : std::nullopt;
            auto fingerprint = ids ? view->fingerprint(ids->fingerprint) : std::nullopt;
            if (!path || !fingerprint)
                throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
            if (*path != fresh.pairs[i].first || *fingerprint != fresh.pairs[i].second)
                return false;
        }
        return true;
    }

    /**
     * The targets a merged row will hold and where each candidate comes from:
     * every stored history trimmed to `keep`, and each fresh closure first in
     * its target's history, displacing a stored candidate equal to it.
     */
    static std::vector<PlannedTarget>
    planShardRow(const DependencyBlobView * view, std::span<const FreshCandidate> fresh, size_t keep)
    {
        std::vector<PlannedTarget> targets;
        if (view) {
            targets.reserve(view->targetCount);
            for (uint32_t t = 0; t < view->targetCount; t++) {
                auto name = view->target(t);
                auto start = view->targetCandidateStart(t);
                auto count = view->targetCandidateCount(t);
                if (!name || !start || !count || *start > view->candidateCount
                    || *count > view->candidateCount - *start)
                    throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
                // Stored targets are sorted (the reader binary-searches them), like the string tables.
                if (!targets.empty() && *name <= targets.back().name)
                    throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
                PlannedTarget planned{.name = *name};
                for (uint32_t i = 0; i < std::min<size_t>(*count, keep); i++)
                    planned.candidates.emplace_back(StoredCandidate{*start + i});
                targets.push_back(std::move(planned));
            }
        }

        for (size_t f = 0; f < fresh.size(); f++) {
            auto it = std::lower_bound(
                targets.begin(),
                targets.end(),
                fresh[f].target,
                [](const PlannedTarget & entry, std::string_view name) { return entry.name < name; });
            if (it == targets.end() || it->name != fresh[f].target)
                it = targets.insert(it, PlannedTarget{.name = fresh[f].target});

            std::vector<CandidateSource> history;
            history.reserve(std::min(keep, it->candidates.size() + 1));
            history.emplace_back(FreshCandidateRef{f});
            for (auto & candidate : it->candidates) {
                if (history.size() >= keep)
                    break;
                if (!sameCandidate(view, candidate, fresh, fresh[f]))
                    history.push_back(candidate);
            }
            it->candidates = std::move(history);
        }
        return targets;
    }

    /**
     * A string table of the row being written: the stored table's surviving
     * strings merged in order with the fresh strings, and the remap from
     * stored ids to new ones.
     */
    struct MergedStrings
    {
        static constexpr uint32_t unmapped = std::numeric_limits<uint32_t>::max();

        std::vector<std::string_view> strings;
        /** Stored id to new id, or `unmapped` for a stored string that did not survive. */
        std::vector<uint32_t> storedIdMap;
        size_t bytes = 0;

        // Both lookups can only fail through a bug in planning or marking, and
        // a wrong id would corrupt the row rather than be caught downstream,
        // so they fail the write (a warning) instead.
        uint32_t idOf(std::string_view s) const
        {
            auto it = std::lower_bound(strings.begin(), strings.end(), s);
            if (it == strings.end() || *it != s)
                throw Error("Tecnix dependency cache: string '%s' missing from a merged table (internal error)", s);
            return static_cast<uint32_t>(it - strings.begin());
        }

        uint32_t mappedId(uint32_t storedId) const
        {
            auto id = storedIdMap.at(storedId);
            if (id == unmapped)
                throw Error(
                    "Tecnix dependency cache: stored string %d was not carried into the merged table (internal error)",
                    storedId);
            return id;
        }
    };

    template<typename StoredAt>
    static MergedStrings mergeStrings(
        uint32_t storedCount, StoredAt storedAt, const std::vector<bool> & used, std::vector<std::string_view> fresh)
    {
        std::sort(fresh.begin(), fresh.end());
        fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());

        MergedStrings merged;
        merged.strings.reserve(std::count(used.begin(), used.end(), true) + fresh.size());
        merged.storedIdMap.assign(storedCount, MergedStrings::unmapped);
        size_t f = 0;
        std::optional<std::string_view> previous;
        for (uint32_t i = 0; i < storedCount; i++) {
            auto stored = storedAt(i);
            if (!stored || (previous && *stored <= *previous))
                throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
            previous = stored;
            while (f < fresh.size() && fresh[f] < *stored)
                merged.strings.push_back(fresh[f++]);
            bool equal = f < fresh.size() && fresh[f] == *stored;
            if (!used[i] && !equal)
                continue;
            merged.storedIdMap[i] = checkedU32(merged.strings.size(), "string count");
            merged.strings.push_back(*stored);
            if (equal)
                f++;
        }
        while (f < fresh.size())
            merged.strings.push_back(fresh[f++]);
        for (auto & s : merged.strings)
            merged.bytes += s.size();
        return merged;
    }

    /**
     * Serialize a planned row into `out`, throwing if it exceeds `maxRowBytes`.
     * Callers writing many rows pass the same buffer each time, so it grows to
     * the largest row once and its pages are reused.
     */
    static void writeShardRow(
        const DependencyBlobView * view,
        const std::vector<PlannedTarget> & targets,
        std::span<const FreshCandidate> fresh,
        size_t maxRowBytes,
        std::string & out)
    {
        if (targets.empty())
            throw Error("Tecnix dependency cache: shard row has no targets (internal error)");
        if (targets.size() > maxDependencyBlobTargets)
            throw Error("Tecnix dependency cache shard target count is too large");

        // Which stored strings survive into the new row, and what the fresh
        // candidates bring.
        std::vector<bool> usedPaths(view ? view->pathCount : 0), usedFingerprints(view ? view->fingerprintCount : 0),
            usedPayloads(view ? view->payloadCount : 0);
        std::vector<std::string_view> freshPaths, freshFingerprints, freshPayloads;
        size_t candidateCount = 0, pairCount = 0;
        for (auto & target : targets) {
            for (auto & candidate : target.candidates) {
                candidateCount++;
                if (auto * ref = std::get_if<FreshCandidateRef>(&candidate)) {
                    auto & f = fresh[ref->index];
                    for (auto & [path, fingerprint] : f.pairs) {
                        freshPaths.push_back(path);
                        freshFingerprints.push_back(fingerprint);
                    }
                    freshPayloads.push_back(f.payload);
                    pairCount += f.pairs.size();
                    continue;
                }
                auto stored = std::get<StoredCandidate>(candidate).index;
                auto payloadId = view->candidatePayload(stored);
                if (!payloadId || *payloadId >= view->payloadCount)
                    throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
                usedPayloads[*payloadId] = true;
                auto pairs = storedCandidatePairs(*view, stored);
                for (uint32_t i = 0; i < pairs.count; i++) {
                    auto ids = view->pairIds(pairs.start + i);
                    if (!ids)
                        throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");
                    usedPaths[ids->path] = true;
                    usedFingerprints[ids->fingerprint] = true;
                }
                pairCount += pairs.count;
            }
        }

        auto paths = mergeStrings(
            view ? view->pathCount : 0, [&](uint32_t i) { return view->path(i); }, usedPaths, std::move(freshPaths));
        auto fingerprints = mergeStrings(
            view ? view->fingerprintCount : 0,
            [&](uint32_t i) { return view->fingerprint(i); },
            usedFingerprints,
            std::move(freshFingerprints));
        auto payloads = mergeStrings(
            view ? view->payloadCount : 0,
            [&](uint32_t i) { return view->payload(i); },
            usedPayloads,
            std::move(freshPayloads));

        std::vector<std::string_view> names;
        names.reserve(targets.size());
        size_t nameBytes = 0;
        for (auto & target : targets) {
            names.push_back(target.name);
            nameBytes += target.name.size();
        }

        out.clear();
        out.reserve(
            dependencyBlobHeaderSize + 4 * (names.size() + 1) + nameBytes + 8 * targets.size()
            + 4 * (paths.strings.size() + fingerprints.strings.size() + payloads.strings.size() + 3) + paths.bytes
            + fingerprints.bytes + payloads.bytes + 12 * candidateCount + 8 * pairCount + 16);
        out.append(dependencyBlobMagic.data(), dependencyBlobMagic.size());
        for (size_t i = 0; i < dependencyBlobFieldCount; i++)
            appendU32(out, 0);

        setBlobField(out, blobVersionField, dependencyBlobVersion);
        setBlobField(out, blobFlagsField, dependencyBlobFlags);
        setBlobField(out, blobTargetCountField, checkedU32(targets.size(), "target count"));
        setBlobField(out, blobPathCountField, checkedU32(paths.strings.size(), "path count"));
        setBlobField(out, blobFingerprintCountField, checkedU32(fingerprints.strings.size(), "fingerprint count"));
        setBlobField(out, blobPayloadCountField, checkedU32(payloads.strings.size(), "payload count"));
        setBlobField(out, blobCandidateCountField, checkedU32(candidateCount, "candidate count"));
        setBlobField(out, blobPairCountField, checkedU32(pairCount, "pair count"));

        setBlobField(out, blobTargetOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, names);

        setBlobField(out, blobTargetRecordsOffsetField, checkedU32(out.size(), "row"));
        uint32_t nextCandidate = 0;
        for (auto & target : targets) {
            appendU32(out, nextCandidate);
            appendU32(out, checkedU32(target.candidates.size(), "target candidate count"));
            nextCandidate += target.candidates.size();
        }

        setBlobField(out, blobPathOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, paths.strings);
        setBlobField(out, blobFingerprintOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, fingerprints.strings);
        setBlobField(out, blobPayloadOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, payloads.strings);

        // Candidate records, then every candidate's pairs in the same order.
        // Stored pairs keep their order under the remap, since the merged
        // tables preserve the stored tables' order.
        setBlobField(out, blobCandidateRecordsOffsetField, checkedU32(out.size(), "row"));
        uint32_t nextPair = 0;
        for (auto & target : targets) {
            for (auto & candidate : target.candidates) {
                uint32_t count, payloadId;
                if (auto * ref = std::get_if<FreshCandidateRef>(&candidate)) {
                    count = checkedU32(fresh[ref->index].pairs.size(), "candidate pair count");
                    payloadId = payloads.idOf(fresh[ref->index].payload);
                } else {
                    auto stored = std::get<StoredCandidate>(candidate).index;
                    count = storedCandidatePairs(*view, stored).count;
                    payloadId = payloads.mappedId(view->candidatePayload(stored).value());
                }
                appendU32(out, nextPair);
                appendU32(out, count);
                appendU32(out, payloadId);
                nextPair += count;
            }
        }

        setBlobField(out, blobPairsOffsetField, checkedU32(out.size(), "row"));
        for (auto & target : targets) {
            for (auto & candidate : target.candidates) {
                if (auto * ref = std::get_if<FreshCandidateRef>(&candidate)) {
                    // Fresh pairs are sorted by path with unique paths, and ids
                    // follow string order, so this emits them in (path, fingerprint) id order.
                    for (auto & [path, fingerprint] : fresh[ref->index].pairs) {
                        appendU32(out, paths.idOf(path));
                        appendU32(out, fingerprints.idOf(fingerprint));
                    }
                    continue;
                }
                auto pairs = storedCandidatePairs(*view, std::get<StoredCandidate>(candidate).index);
                for (uint32_t i = 0; i < pairs.count; i++) {
                    // Validated while marking; a slip there throws here rather than reading garbage.
                    auto ids = view->pairIds(pairs.start + i).value();
                    appendU32(out, paths.mappedId(ids.path));
                    appendU32(out, fingerprints.mappedId(ids.fingerprint));
                }
            }
        }

        if (out.size() > maxRowBytes)
            throw Error(
                "Tecnix dependency cache row is too large to store (%d bytes; SQLite allows %d)",
                out.size(),
                maxRowBytes);
        setBlobField(out, blobEndOffsetField, checkedU32(out.size(), "row"));
    }

    /**
     * The row for a shard after learning `updates`, merged into `existingBlob`
     * when there is one this version can read. A row it cannot read, or one
     * that has outgrown the target bound (the row has no per-target age, so
     * everything from previous evaluations goes and re-enters on next use), is
     * replaced by what this evaluation learned, as it would have been a miss
     * on the read side. Pure: the caller reports the outcome.
     */
    static TecnixRowMergeOutcome mergedShardRow(
        std::optional<std::string_view> existingBlob,
        const std::vector<const TecnixDependencyUpsert *> & updates,
        size_t historyLimit,
        size_t maxRowBytes,
        std::string & out)
    {
        // The freshly learned closure is always kept, so a limit of 0 acts as 1.
        auto keep = std::max<size_t>(historyLimit, 1);
        std::vector<FreshCandidate> fresh;
        fresh.reserve(updates.size());
        for (auto * update : updates)
            fresh.push_back(freshCandidate(*update));

        auto outcome = TecnixRowMergeOutcome::Fresh;
        if (existingBlob) {
            auto view = DependencyBlobView::open(*existingBlob);
            if (view) {
                try {
                    auto targets = planShardRow(&*view, fresh, keep);
                    if (targets.size() <= maxDependencyBlobTargets) {
                        writeShardRow(&*view, targets, fresh, maxRowBytes, out);
                        return TecnixRowMergeOutcome::Merged;
                    }
                    outcome = TecnixRowMergeOutcome::ReplacedFull;
                } catch (MalformedTecnixCacheRow &) {
                    outcome = TecnixRowMergeOutcome::ReplacedUnreadable;
                }
            } else if (auto version = DependencyBlobView::declaredVersion(*existingBlob);
                       version && *version != dependencyBlobVersion) {
                outcome = TecnixRowMergeOutcome::ReplacedOtherVersion;
            } else {
                outcome = TecnixRowMergeOutcome::ReplacedUnreadable;
            }
        }
        writeShardRow(nullptr, planShardRow(nullptr, fresh, keep), fresh, maxRowBytes, out);
        return outcome;
    }

    static void reportRowMergeOutcome(uint32_t shard, TecnixRowMergeOutcome outcome)
    {
        switch (outcome) {
        case TecnixRowMergeOutcome::Fresh:
        case TecnixRowMergeOutcome::Merged:
            break;
        case TecnixRowMergeOutcome::ReplacedOtherVersion:
            // Routine on the first write after a format bump: every row of the scope is replaced in turn.
            debug("tecnix: dependency cache shard %d was written in another format version; replaced", shard);
            break;
        case TecnixRowMergeOutcome::ReplacedUnreadable:
            warn("tecnix: dependency cache shard %d is unreadable; replaced with this evaluation's results", shard);
            break;
        case TecnixRowMergeOutcome::ReplacedFull:
            warn("tecnix: dependency cache shard %d is full; evicting entries from previous evaluations", shard);
            break;
        }
    }

    /** The keys' shards in ascending order, each with the indices of the keys hashed into it. */
    static std::vector<std::pair<uint32_t, std::vector<size_t>>> keysByShard(std::span<const std::string> keys)
    {
        boost::unordered_flat_map<uint32_t, std::vector<size_t>> indicesByShard;
        indicesByShard.reserve(std::min(keys.size(), dependencyShardCount));
        for (size_t i = 0; i < keys.size(); i++)
            indicesByShard[dependencyShardForTarget(keys[i])].push_back(i);

        std::vector<std::pair<uint32_t, std::vector<size_t>>> shards(indicesByShard.begin(), indicesByShard.end());
        std::sort(shards.begin(), shards.end(), [](const auto & a, const auto & b) { return a.first < b.first; });
        return shards;
    }

    /**
     * Copy one shard row out of the database into `row`, while the connection
     * lock is held. Callers visiting many shards pass the same buffer each
     * time: it grows to the largest row once and its pages are reused, instead
     * of a fresh allocation being faulted in per row. False if the row does
     * not exist.
     */
    static bool readShardRow(State & state, const TecnixCacheScope & scope, uint32_t shard, std::string & row)
    {
        auto stmt(state.lookupShard.use().apply(scope.gitDir).apply(scope.resolver).apply(scope.argsKey).apply(shard));
        if (!stmt.next())
            return false;
        auto blob = stmt.getBlob(0);
        row.assign(blob.data(), blob.size());
        return true;
    }

    bool lookupShardRow(const TecnixCacheScope & scope, uint32_t shard, std::string & row)
    {
        return retrySQLite<bool>([&]() {
            auto state(_state.lock());
            return readShardRow(*state, scope, shard, row);
        });
    }

    /**
     * Merge `entries` into their shard rows, one transaction per shard: the
     * write lock is held for one row's merge at a time, so evaluators sharing
     * the cache interleave, and a row that cannot be written (too large, or
     * an internal error) costs only its own shard's update, with a warning.
     * Each transaction reads, merges, and writes its row under `begin
     * immediate` (see SQLiteImmediateTxn), retried as a whole if busy.
     */
    void
    upsertMany(const TecnixCacheScope & scope, const std::vector<TecnixDependencyUpsert> & entries, size_t historyLimit)
    {
        boost::unordered_flat_map<uint32_t, std::vector<const TecnixDependencyUpsert *>> updatesByShard;
        updatesByShard.reserve(std::min(entries.size(), dependencyShardCount));
        for (auto & entry : entries)
            updatesByShard[dependencyShardForTarget(entry.target)].push_back(&entry);

        // One buffer for the row read and one for the row written, reused across shards.
        std::string existing, blob;
        for (auto & [shard, updates] : updatesByShard) {
            try {
                auto outcome = retrySQLite<TecnixRowMergeOutcome>([&]() {
                    auto state(_state.lock());
                    SQLiteImmediateTxn txn(state->db);
                    std::optional<std::string_view> existingBlob;
                    if (readShardRow(*state, scope, shard, existing))
                        existingBlob = existing;
                    auto outcome = mergedShardRow(existingBlob, updates, historyLimit, state->maxRowBytes, blob);
                    state->upsertShard.use()
                        .apply(scope.gitDir)
                        .apply(scope.resolver)
                        .apply(scope.argsKey)
                        .apply(shard)
                        .apply(reinterpret_cast<const unsigned char *>(blob.data()), blob.size())
                        .apply(time(nullptr))
                        .exec();
                    txn.commit();
                    return outcome;
                });
                reportRowMergeOutcome(shard, outcome);
            } catch (Interrupted &) {
                throw;
            } catch (std::exception & e) {
                warn(
                    "tecnix: failed to write dependency cache shard %d; continuing without caching its results: %s",
                    shard,
                    e.what());
            }
        }
    }
};

static TecnixEvalCache & getTecnixEvalCache()
{
    static TecnixEvalCache cache;
    return cache;
}

static void warnTecnixEvalCacheWriteFailure(const std::exception & e)
{
    warn("tecnix: failed to write eval cache entry; continuing without caching this result: %s", e.what());
}

static void warnTecnixEvalCacheWriteFailure()
{
    warn("tecnix: failed to write eval cache entry; continuing without caching this result");
}

/**
 * Store freshly evaluated closures. Cache writes are an optimization: failures
 * warn and continue, they never fail the evaluation that produced the result.
 */
void upsertDependencyClosures(
    const TecnixCacheScope & scope, const std::vector<TecnixDependencyUpsert> & entries, size_t historyLimit)
{
    if (entries.empty())
        return;
    try {
        getTecnixEvalCache().upsertMany(scope, entries, historyLimit);
    } catch (Interrupted &) {
        throw;
    } catch (const std::exception & e) {
        warnTecnixEvalCacheWriteFailure(e);
    } catch (...) {
        warnTecnixEvalCacheWriteFailure();
    }
}

/** A proven candidate within the shard row currently being validated. */
struct DependencyCacheHit::Impl
{
    const TecnixEvalCache::DependencyBlobView & view;
    uint32_t candidateIndex;
};

DependencyCacheHit::DependencyCacheHit(const Impl & impl)
    : impl(impl)
{
}

std::optional<std::string_view> DependencyCacheHit::payload() const
{
    return impl.view.candidatePayloadValue(impl.candidateIndex);
}

void lookupCachedDependencies(
    EvalState & state,
    const TecnixCacheScope & scope,
    std::span<const std::string> keys,
    DependencyFingerprintCache & fingerprintCache,
    const std::function<void(size_t index, const DependencyCacheHit & hit)> & onHit)
{
    if (keys.empty())
        return;
    auto repoAccessor = getTecnixRepoAccessor(state);

    // The cache is an optimization: a database that cannot be opened or read
    // means misses, with a warning, never a failed evaluation. Only the
    // database access is guarded; errors from `onHit` propagate.
    TecnixEvalCache * cache;
    try {
        cache = &getTecnixEvalCache();
    } catch (Error & e) {
        warn("tecnix: failed to open the dependency cache; continuing without it: %s", e.what());
        return;
    }

    std::string row;
    for (auto & [shard, indices] : TecnixEvalCache::keysByShard(keys)) {
        try {
            if (!cache->lookupShardRow(scope, shard, row))
                continue;
        } catch (Error & e) {
            warn(
                "tecnix: failed to read dependency cache shard %d; treating its targets as misses: %s",
                shard,
                e.what());
            continue;
        }
        auto view = TecnixEvalCache::DependencyBlobView::open(row);
        if (!view)
            continue;
        auto currentFingerprints = view->currentFingerprints(repoAccessor, fingerprintCache);
        for (auto i : indices) {
            if (auto candidate = view->findMatchingCandidate(keys[i], currentFingerprints)) {
                // The hit is a view into `row`, and `impl` lives for this iteration
                // of the loop; `onHit` must copy out whatever it keeps.
                DependencyCacheHit::Impl impl{*view, *candidate};
                onHit(i, DependencyCacheHit(impl));
            }
        }
    }
}

TecnixRowMergeOutcome mergeTecnixDependencyRow(
    std::optional<std::string_view> existingBlob,
    const std::vector<const TecnixDependencyUpsert *> & updates,
    size_t historyLimit,
    size_t maxRowBytes,
    std::string & out)
{
    return TecnixEvalCache::mergedShardRow(existingBlob, updates, historyLimit, maxRowBytes, out);
}

std::optional<TecnixDependencyRow> decodeTecnixDependencyRow(std::string_view blob)
{
    auto view = TecnixEvalCache::DependencyBlobView::open(blob);
    if (!view)
        return std::nullopt;
    try {
        TecnixDependencyRow row;
        for (uint32_t t = 0; t < view->targetCount; t++) {
            auto name = view->target(t);
            auto start = view->targetCandidateStart(t);
            auto count = view->targetCandidateCount(t);
            if (!name || !start || !count || *start > view->candidateCount || *count > view->candidateCount - *start)
                return std::nullopt;
            TecnixDependencyRow::Target target{.target = std::string(*name)};
            for (uint32_t c = *start; c < *start + *count; c++) {
                auto payload = view->candidatePayloadValue(c);
                if (!payload)
                    return std::nullopt;
                TecnixDependencyRow::Candidate candidate{.payload = std::string(*payload)};
                auto pairs = TecnixEvalCache::storedCandidatePairs(*view, c);
                for (uint32_t i = 0; i < pairs.count; i++) {
                    auto ids = view->pairIds(pairs.start + i);
                    auto path = ids ? view->path(ids->path) : std::nullopt;
                    auto fingerprint = ids ? view->fingerprint(ids->fingerprint) : std::nullopt;
                    if (!path || !fingerprint)
                        return std::nullopt;
                    candidate.dependencies.push_back({std::string(*path), std::string(*fingerprint)});
                }
                target.candidates.push_back(std::move(candidate));
            }
            row.targets.push_back(std::move(target));
        }
        return row;
    } catch (MalformedTecnixCacheRow &) {
        return std::nullopt;
    }
}

static DependencyFingerprintThreadLocalCache & getDependencyFingerprintThreadCache(DependencyFingerprintCache & cache)
{
    static thread_local DependencyFingerprintThreadLocalCache threadCache;
    if (threadCache.generation != cache.generation) {
        threadCache.generation = cache.generation;
        threadCache.fingerprints.clear();
    }
    return threadCache;
}

static const std::optional<std::string> *
dependencyFingerprintCached(ref<SourceAccessor> accessor, std::string_view path, DependencyFingerprintCache & cache)
{
    auto & threadCache = getDependencyFingerprintThreadCache(cache);

    auto it = threadCache.fingerprints.find(path);
    if (it != threadCache.fingerprints.end())
        return &it->second;

    auto [_, fp] = accessor->getFingerprint(CanonPath(path));
    auto inserted = threadCache.fingerprints.emplace(std::string(path), std::move(fp)).first;
    return &inserted->second;
}

std::optional<std::string>
dependencyFingerprint(ref<SourceAccessor> accessor, std::string_view path, DependencyFingerprintCache & cache)
{
    auto fp = dependencyFingerprintCached(accessor, path, cache);
    if (!fp)
        return std::nullopt;
    return *fp;
}

DependencyClosure dependencyFingerprints(
    ref<SourceAccessor> accessor, const std::vector<std::string> & paths, DependencyFingerprintCache & cache)
{
    DependencyClosure result;
    result.reserve(paths.size());
    for (auto & path : paths) {
        auto fp = dependencyFingerprint(accessor, path, cache);
        if (!fp)
            throw Error("failed to fingerprint Tecnix dependency path '%s'", path);
        result.push_back({path, std::move(*fp)});
    }
    return result;
}

Value * DependencyCacheHit::toValue(EvalState & state) const
{
    auto & view = impl.view;
    auto pairs = TecnixEvalCache::storedCandidatePairs(view, impl.candidateIndex);

    auto attrs = state.buildBindings(pairs.count);
    for (uint32_t i = 0; i < pairs.count; i++) {
        auto ids = view.pairIds(pairs.start + i);
        auto path = ids ? view.path(ids->path) : std::nullopt;
        auto fingerprint = ids ? view.fingerprint(ids->fingerprint) : std::nullopt;
        if (!path || !fingerprint)
            throw MalformedTecnixCacheRow("malformed Tecnix dependency cache row");

        auto * fingerprintValue = state.allocValue();
        fingerprintValue->mkString(*fingerprint, state.mem);
        attrs.insert(state.symbols.create(*path), fingerprintValue);
    }
    auto * val = state.allocValue();
    val->mkAttrs(attrs);

    return val;
}

} // namespace nix
