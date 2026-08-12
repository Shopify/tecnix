/**
 * The persistent Tecnix evaluation cache (see eval-cache.hh for the API and
 * plans/tecnix-target-eval-caching/ for the design): DependencyShards rows in
 * SQLite holding TXDC blobs — bounded per-key source-closure candidate
 * histories, validated against current fingerprints on every use.
 */

#include "nix/expr/tecnix/eval-cache.hh"

#include "nix/expr/eval-inline.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/store/globals.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

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
        SQLiteStmt upsertShard, lookupShard, lookupAllShards;
    };

    Sync<State> _state;

    TecnixEvalCache()
    {
        auto state(_state.lock());

        auto dbPath = getCacheDir() / "tecnix-eval-cache-v1.sqlite";
        createDirs(dbPath.parent_path());

        state->db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
        state->db.isCache();
        state->db.exec(tecnixEvalCacheSchema);

        state->upsertShard.create(
            state->db,
            "insert or replace into DependencyShards(gitDir, resolver, argsKey, shard, dependencies, timestamp) "
            "values (?, ?, ?, ?, ?, ?)");
        state->lookupShard.create(
            state->db,
            "select dependencies from DependencyShards where gitDir = ? and resolver = ? and argsKey = ? and shard = ?");
        state->lookupAllShards.create(
            state->db,
            "select shard, dependencies from DependencyShards where gitDir = ? and resolver = ? and argsKey = ?");
    }

    static constexpr std::string_view dependencyBlobMagic = "TXDC";
    static constexpr uint32_t dependencyBlobVersion = 1;
    static constexpr uint32_t dependencyBlobFlags = 0;
    static constexpr size_t dependencyBlobFieldCount = 16;
    static constexpr size_t dependencyBlobHeaderSize = 4 + dependencyBlobFieldCount * sizeof(uint32_t);
    static constexpr size_t dependencyShardCount = 256;
    static constexpr size_t maxDependencyBlobTargets = 1024;
    static constexpr size_t maxDependencyBlobCandidates = 32;
    static constexpr size_t maxDependencyBlobStrings = 200000;
    static constexpr size_t maxDependencyBlobPairs = 1000000;
    static constexpr size_t maxDependencyBlobBytes = 64 * 1024 * 1024;

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

    struct DependencyCandidate
    {
        DependencyClosure dependencies;
        std::string payload;
    };

    struct DependencyShardTarget
    {
        std::string target;
        std::vector<DependencyCandidate> candidates;
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
        out.push_back(static_cast<char>(value & 0xff));
        out.push_back(static_cast<char>((value >> 8) & 0xff));
        out.push_back(static_cast<char>((value >> 16) & 0xff));
        out.push_back(static_cast<char>((value >> 24) & 0xff));
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

        static std::optional<DependencyBlobView> open(std::string_view blob)
        {
            if (blob.size() < dependencyBlobHeaderSize || blob.size() > maxDependencyBlobBytes)
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

            if (view.targetCount == 0 || view.targetCount > maxDependencyBlobTargets
                || view.pathCount > maxDependencyBlobStrings || view.fingerprintCount > maxDependencyBlobStrings
                || view.payloadCount == 0 || view.payloadCount > maxDependencyBlobTargets * maxDependencyBlobCandidates
                || view.candidateCount == 0
                || view.candidateCount > maxDependencyBlobTargets * maxDependencyBlobCandidates
                || view.pairCount > maxDependencyBlobPairs || view.endOffset != blob.size())
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

        DependencyClosure toDependencyClosure(uint32_t candidateIndex) const
        {
            auto pairStart = candidatePairStart(candidateIndex);
            auto count = candidatePairCount(candidateIndex);
            if (!pairStart || !count)
                throw Error("malformed Tecnix dependency cache row");

            DependencyClosure result;
            result.reserve(*count);
            for (uint32_t i = 0; i < *count; i++) {
                auto pairIndex = *pairStart + i;
                auto pathId = pairPath(pairIndex);
                auto fingerprintId = pairFingerprint(pairIndex);
                if (!pathId || !fingerprintId)
                    throw Error("malformed Tecnix dependency cache row");
                auto p = path(*pathId);
                auto fp = fingerprint(*fingerprintId);
                if (!p || !fp)
                    throw Error("malformed Tecnix dependency cache row");
                result.push_back({std::string(*p), std::string(*fp)});
            }
            return result;
        }

        std::vector<DependencyShardTarget> toShardTargets() const
        {
            std::vector<DependencyShardTarget> result;
            result.reserve(targetCount);
            for (uint32_t targetId = 0; targetId < targetCount; targetId++) {
                auto targetName = target(targetId);
                auto candidateStart = targetCandidateStart(targetId);
                auto count = targetCandidateCount(targetId);
                if (!targetName || !candidateStart || !count || *candidateStart > candidateCount
                    || *count > candidateCount - *candidateStart)
                    throw Error("malformed Tecnix dependency cache row");

                DependencyShardTarget targetResult;
                targetResult.target = std::string(*targetName);
                targetResult.candidates.reserve(*count);
                for (uint32_t i = 0; i < *count; i++) {
                    auto candidateIndex = *candidateStart + i;
                    auto payloadValue = candidatePayloadValue(candidateIndex);
                    if (!payloadValue)
                        throw Error("malformed Tecnix dependency cache row");
                    targetResult.candidates.push_back({
                        .dependencies = toDependencyClosure(candidateIndex),
                        .payload = std::string(*payloadValue),
                    });
                }
                result.push_back(std::move(targetResult));
            }
            return result;
        }
    };

    static void appendStringTable(std::string & out, const std::vector<std::string> & values)
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

    static DependencyClosure normalizedDependencyClosure(const DependencyClosure & dependencies)
    {
        DependencyClosure result = dependencies;
        std::sort(result.begin(), result.end(), [](const auto & a, const auto & b) { return a.path < b.path; });

        DependencyClosure deduped;
        deduped.reserve(result.size());
        for (auto & dependency : result) {
            if (!deduped.empty() && deduped.back().path == dependency.path) {
                if (deduped.back().fingerprint != dependency.fingerprint)
                    throw Error(
                        "Tecnix dependency cache closure contains conflicting fingerprints for path '%s'",
                        dependency.path);
                continue;
            }
            deduped.push_back(std::move(dependency));
        }
        return deduped;
    }

    static bool dependencyClosuresEqual(const DependencyClosure & a, const DependencyClosure & b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].path != b[i].path || a[i].fingerprint != b[i].fingerprint)
                return false;
        }
        return true;
    }

    static std::vector<DependencyCandidate> candidatesWithInsertedClosure(
        std::span<const DependencyCandidate> existingCandidates,
        const DependencyClosure & dependencies,
        std::string payload)
    {
        DependencyCandidate fresh{
            .dependencies = normalizedDependencyClosure(dependencies),
            .payload = std::move(payload),
        };

        std::vector<DependencyCandidate> result;
        result.reserve(maxDependencyBlobCandidates);
        result.push_back(fresh);

        for (auto & candidate : existingCandidates) {
            auto normalized = normalizedDependencyClosure(candidate.dependencies);
            if (candidate.payload == result.front().payload
                && dependencyClosuresEqual(normalized, result.front().dependencies))
                continue;
            if (result.size() >= maxDependencyBlobCandidates)
                break;
            result.push_back({
                .dependencies = std::move(normalized),
                .payload = candidate.payload,
            });
        }

        return result;
    }

    static std::string dependencyBlobFromShardTargets(const std::vector<DependencyShardTarget> & shardTargets)
    {
        if (shardTargets.empty() || shardTargets.size() > maxDependencyBlobTargets)
            throw Error("Tecnix dependency cache shard target count is too large");

        struct PairId
        {
            uint32_t pathId;
            uint32_t fingerprintId;
        };

        struct TargetRecord
        {
            uint32_t candidateStart = 0;
            uint32_t candidateCount = 0;
        };

        struct CandidateRecord
        {
            uint32_t pairStart;
            uint32_t pairCount;
            uint32_t payloadId;
        };

        std::vector<DependencyShardTarget> normalizedTargets;
        normalizedTargets.reserve(shardTargets.size());
        for (auto & shardTarget : shardTargets) {
            DependencyShardTarget normalizedTarget;
            normalizedTarget.target = shardTarget.target;
            normalizedTarget.candidates.reserve(shardTarget.candidates.size());
            for (auto & candidate : shardTarget.candidates) {
                normalizedTarget.candidates.push_back({
                    .dependencies = normalizedDependencyClosure(candidate.dependencies),
                    .payload = candidate.payload,
                });
            }
            if (!normalizedTarget.candidates.empty())
                normalizedTargets.push_back(std::move(normalizedTarget));
        }
        std::sort(normalizedTargets.begin(), normalizedTargets.end(), [](const auto & a, const auto & b) {
            return a.target < b.target;
        });
        normalizedTargets.erase(
            std::unique(
                normalizedTargets.begin(),
                normalizedTargets.end(),
                [](const auto & a, const auto & b) { return a.target == b.target; }),
            normalizedTargets.end());
        if (normalizedTargets.empty())
            throw Error("Tecnix dependency cache shard has no targets");

        size_t totalPairs = 0;
        size_t totalCandidates = 0;
        std::vector<std::string> targets;
        std::vector<std::string> paths;
        std::vector<std::string> fingerprints;
        std::vector<std::string> payloads;
        targets.reserve(normalizedTargets.size());
        for (auto & shardTarget : normalizedTargets) {
            targets.push_back(shardTarget.target);
            totalCandidates += shardTarget.candidates.size();
            if (shardTarget.candidates.size() > maxDependencyBlobCandidates
                || totalCandidates > maxDependencyBlobTargets * maxDependencyBlobCandidates)
                throw Error("Tecnix dependency cache candidate history is too large");
            for (auto & candidate : shardTarget.candidates) {
                totalPairs += candidate.dependencies.size();
                if (totalPairs > maxDependencyBlobPairs)
                    throw Error("Tecnix dependency cache closure history is too large");
                payloads.push_back(candidate.payload);
                for (auto & dependency : candidate.dependencies) {
                    paths.push_back(dependency.path);
                    fingerprints.push_back(dependency.fingerprint);
                }
            }
        }

        auto sortUnique = [](std::vector<std::string> values) {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
            return values;
        };
        paths = sortUnique(std::move(paths));
        fingerprints = sortUnique(std::move(fingerprints));
        payloads = sortUnique(std::move(payloads));

        if (targets.size() > maxDependencyBlobTargets || paths.size() > maxDependencyBlobStrings
            || fingerprints.size() > maxDependencyBlobStrings || payloads.empty()
            || payloads.size() > maxDependencyBlobTargets * maxDependencyBlobCandidates)
            throw Error("Tecnix dependency cache row string tables are too large");

        boost::unordered_flat_map<std::string, uint32_t, StringViewHash, std::equal_to<>> pathIds;
        boost::unordered_flat_map<std::string, uint32_t, StringViewHash, std::equal_to<>> fingerprintIds;
        boost::unordered_flat_map<std::string, uint32_t, StringViewHash, std::equal_to<>> payloadIds;
        pathIds.reserve(paths.size());
        fingerprintIds.reserve(fingerprints.size());
        payloadIds.reserve(payloads.size());
        for (uint32_t i = 0; i < paths.size(); i++)
            pathIds.emplace(paths[i], i);
        for (uint32_t i = 0; i < fingerprints.size(); i++)
            fingerprintIds.emplace(fingerprints[i], i);
        for (uint32_t i = 0; i < payloads.size(); i++)
            payloadIds.emplace(payloads[i], i);

        std::vector<TargetRecord> targetRecords(targets.size());
        std::vector<CandidateRecord> candidateRecords;
        candidateRecords.reserve(totalCandidates);
        std::vector<PairId> pairs;
        pairs.reserve(totalPairs);

        auto getPathId = [&](const std::string & path) -> uint32_t { return pathIds.find(path)->second; };
        auto getFingerprintId = [&](const std::string & fingerprint) -> uint32_t {
            return fingerprintIds.find(fingerprint)->second;
        };
        auto getPayloadId = [&](const std::string & payload) -> uint32_t { return payloadIds.find(payload)->second; };

        for (uint32_t targetId = 0; targetId < normalizedTargets.size(); targetId++) {
            auto & shardTarget = normalizedTargets[targetId];
            auto candidateStart = checkedU32(candidateRecords.size(), "candidate count");
            targetRecords[targetId] = {
                .candidateStart = candidateStart,
                .candidateCount = checkedU32(shardTarget.candidates.size(), "target candidate count"),
            };

            for (auto & candidate : shardTarget.candidates) {
                auto pairStart = checkedU32(pairs.size(), "pair count");
                std::vector<PairId> candidatePairs;
                candidatePairs.reserve(candidate.dependencies.size());
                for (auto & dependency : candidate.dependencies) {
                    candidatePairs.push_back({
                        .pathId = getPathId(dependency.path),
                        .fingerprintId = getFingerprintId(dependency.fingerprint),
                    });
                }
                std::sort(candidatePairs.begin(), candidatePairs.end(), [](const auto & a, const auto & b) {
                    if (a.pathId != b.pathId)
                        return a.pathId < b.pathId;
                    return a.fingerprintId < b.fingerprintId;
                });
                pairs.insert(pairs.end(), candidatePairs.begin(), candidatePairs.end());
                candidateRecords.push_back({
                    .pairStart = pairStart,
                    .pairCount = checkedU32(candidatePairs.size(), "candidate pair count"),
                    .payloadId = getPayloadId(candidate.payload),
                });
            }
        }

        std::string out;
        out.reserve(
            std::min<size_t>(
                maxDependencyBlobBytes,
                totalPairs * 8 + totalCandidates * 12 + paths.size() * 32 + targets.size() * 64 + 4096));
        out.append(dependencyBlobMagic.data(), dependencyBlobMagic.size());
        for (size_t i = 0; i < dependencyBlobFieldCount; i++)
            appendU32(out, 0);

        setBlobField(out, blobVersionField, dependencyBlobVersion);
        setBlobField(out, blobFlagsField, dependencyBlobFlags);
        setBlobField(out, blobTargetCountField, checkedU32(targets.size(), "target count"));
        setBlobField(out, blobPathCountField, checkedU32(paths.size(), "path count"));
        setBlobField(out, blobFingerprintCountField, checkedU32(fingerprints.size(), "fingerprint count"));
        setBlobField(out, blobPayloadCountField, checkedU32(payloads.size(), "payload count"));
        setBlobField(out, blobCandidateCountField, checkedU32(candidateRecords.size(), "candidate count"));
        setBlobField(out, blobPairCountField, checkedU32(pairs.size(), "pair count"));

        padTo4(out);
        setBlobField(out, blobTargetOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, targets);

        setBlobField(out, blobTargetRecordsOffsetField, checkedU32(out.size(), "row"));
        for (auto & targetRecord : targetRecords) {
            appendU32(out, targetRecord.candidateStart);
            appendU32(out, targetRecord.candidateCount);
        }

        setBlobField(out, blobPathOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, paths);

        setBlobField(out, blobFingerprintOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, fingerprints);

        setBlobField(out, blobPayloadOffsetsOffsetField, checkedU32(out.size(), "row"));
        appendStringTable(out, payloads);

        padTo4(out);
        setBlobField(out, blobCandidateRecordsOffsetField, checkedU32(out.size(), "row"));
        for (auto & candidateRecord : candidateRecords) {
            appendU32(out, candidateRecord.pairStart);
            appendU32(out, candidateRecord.pairCount);
            appendU32(out, candidateRecord.payloadId);
        }

        setBlobField(out, blobPairsOffsetField, checkedU32(out.size(), "row"));
        for (auto & pair : pairs) {
            appendU32(out, pair.pathId);
            appendU32(out, pair.fingerprintId);
        }

        padTo4(out);
        if (!u32Fits(out.size()) || out.size() > maxDependencyBlobBytes)
            throw Error("Tecnix dependency cache row is too large");
        setBlobField(out, blobEndOffsetField, static_cast<uint32_t>(out.size()));
        return out;
    }

    using DependencyBlobRef = std::shared_ptr<std::string>;

    std::vector<std::optional<DependencyBlobRef>>
    lookupBlobs(const TecnixCacheScope & scope, std::span<const std::string> targets)
    {
        std::vector<std::optional<DependencyBlobRef>> blobs(targets.size());
        if (targets.empty())
            return blobs;

        boost::unordered_flat_map<uint32_t, std::vector<size_t>> indicesByShard;
        indicesByShard.reserve(std::min(targets.size(), dependencyShardCount));
        for (size_t i = 0; i < targets.size(); i++)
            indicesByShard[dependencyShardForTarget(targets[i])].push_back(i);

        auto state(_state.lock());
        if (indicesByShard.size() == 1) {
            auto shard = indicesByShard.begin()->first;
            auto stmt(
                state->lookupShard.use().apply(scope.gitDir).apply(scope.resolver).apply(scope.argsKey).apply(shard));
            if (!stmt.next())
                return blobs;
            auto blobView = stmt.getBlob(0);
            auto blob = std::make_shared<std::string>(blobView.data(), blobView.size());
            for (auto index : indicesByShard.begin()->second)
                blobs[index] = blob;
            return blobs;
        }

        auto stmt(state->lookupAllShards.use().apply(scope.gitDir).apply(scope.resolver).apply(scope.argsKey));
        while (stmt.next()) {
            auto shard = static_cast<uint32_t>(stmt.getInt(0));
            auto indices = indicesByShard.find(shard);
            if (indices == indicesByShard.end())
                continue;
            auto blobView = stmt.getBlob(1);
            auto blob = std::make_shared<std::string>(blobView.data(), blobView.size());
            for (auto index : indices->second)
                blobs[index] = blob;
        }

        return blobs;
    }

    static std::vector<DependencyShardTarget> shardTargetsWithUpdates(
        std::optional<std::string_view> existingBlob,
        const std::vector<std::tuple<std::string, const DependencyClosure *, std::string>> & updates)
    {
        std::vector<DependencyShardTarget> shardTargets;
        if (existingBlob) {
            if (auto view = DependencyBlobView::open(*existingBlob)) {
                try {
                    shardTargets = view->toShardTargets();
                } catch (const Error &) {
                    shardTargets.clear();
                }
            }
        }

        std::sort(shardTargets.begin(), shardTargets.end(), [](const auto & a, const auto & b) {
            return a.target < b.target;
        });

        for (auto & [target, dependencies, payload] : updates) {
            auto it = std::lower_bound(
                shardTargets.begin(), shardTargets.end(), target, [](const auto & entry, const std::string & target) {
                    return entry.target < target;
                });
            if (it == shardTargets.end() || it->target != target) {
                DependencyShardTarget inserted;
                inserted.target = target;
                it = shardTargets.insert(it, std::move(inserted));
            }
            it->candidates = candidatesWithInsertedClosure(
                std::span<const DependencyCandidate>{it->candidates.data(), it->candidates.size()},
                *dependencies,
                payload);
        }

        if (existingBlob && shardTargets.size() > maxDependencyBlobTargets) {
            // The blob has no per-target age, so evict everything from
            // previous evaluations; dropped targets re-enter on next use.
            warn("tecnix: dependency cache shard is full; evicting entries from previous evaluations");
            return shardTargetsWithUpdates(std::nullopt, updates);
        }

        return shardTargets;
    }

    void upsertMany(const TecnixCacheScope & scope, const std::vector<TecnixDependencyUpsert> & entries)
    {
        if (entries.empty())
            return;

        boost::
            unordered_flat_map<uint32_t, std::vector<std::tuple<std::string, const DependencyClosure *, std::string>>>
                updatesByShard;
        updatesByShard.reserve(std::min(entries.size(), dependencyShardCount));
        for (auto & entry : entries)
            updatesByShard[dependencyShardForTarget(entry.target)].push_back(
                {std::string(entry.target), entry.dependencies, entry.payload});

        auto state(_state.lock());
        SQLiteImmediateTxn txn(state->db);

        boost::unordered_flat_map<uint32_t, std::string> existingBlobs;
        existingBlobs.reserve(updatesByShard.size());
        if (updatesByShard.size() == 1) {
            auto shard = updatesByShard.begin()->first;
            auto stmt(
                state->lookupShard.use().apply(scope.gitDir).apply(scope.resolver).apply(scope.argsKey).apply(shard));
            if (stmt.next()) {
                auto blobView = stmt.getBlob(0);
                existingBlobs.emplace(shard, std::string(blobView.data(), blobView.size()));
            }
        } else {
            auto stmt(state->lookupAllShards.use().apply(scope.gitDir).apply(scope.resolver).apply(scope.argsKey));
            while (stmt.next()) {
                auto shard = static_cast<uint32_t>(stmt.getInt(0));
                if (updatesByShard.find(shard) == updatesByShard.end())
                    continue;
                auto blobView = stmt.getBlob(1);
                existingBlobs.emplace(shard, std::string(blobView.data(), blobView.size()));
            }
        }

        std::vector<std::pair<uint32_t, std::string>> blobs;
        blobs.reserve(updatesByShard.size());
        for (auto & [shard, updates] : updatesByShard) {
            std::optional<std::string_view> existingBlob;
            if (auto existing = existingBlobs.find(shard); existing != existingBlobs.end())
                existingBlob = std::string_view(existing->second);
            auto shardTargets = shardTargetsWithUpdates(existingBlob, updates);
            blobs.emplace_back(shard, dependencyBlobFromShardTargets(shardTargets));
        }

        auto timestamp = time(nullptr);
        for (auto & [shard, blob] : blobs) {
            state->upsertShard.use()
                .apply(scope.gitDir)
                .apply(scope.resolver)
                .apply(scope.argsKey)
                .apply(shard)
                .apply(reinterpret_cast<const unsigned char *>(blob.data()), blob.size())
                .apply(timestamp)
                .exec();
        }
        txn.commit();
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
void upsertDependencyClosures(const TecnixCacheScope & scope, const std::vector<TecnixDependencyUpsert> & entries)
{
    if (entries.empty())
        return;
    try {
        getTecnixEvalCache().upsertMany(scope, entries);
    } catch (const std::exception & e) {
        warnTecnixEvalCacheWriteFailure(e);
    } catch (...) {
        warnTecnixEvalCacheWriteFailure();
    }
}

/** The blob's string storage is owned by the shared_ptr, so the view's
 * borrowed spans stay valid for the Impl's lifetime and moves of the
 * public handle move only the pointer. */
struct ValidatedDependencyBlob::Impl
{
    TecnixEvalCache::DependencyBlobRef blob;
    TecnixEvalCache::DependencyBlobView view;
    uint32_t candidateIndex;
};

ValidatedDependencyBlob::ValidatedDependencyBlob(std::unique_ptr<Impl> impl)
    : impl(std::move(impl))
{
}

ValidatedDependencyBlob::ValidatedDependencyBlob(ValidatedDependencyBlob &&) noexcept = default;
ValidatedDependencyBlob & ValidatedDependencyBlob::operator=(ValidatedDependencyBlob &&) noexcept = default;
ValidatedDependencyBlob::~ValidatedDependencyBlob() = default;

std::optional<std::string_view> ValidatedDependencyBlob::payload() const
{
    return impl->view.candidatePayloadValue(impl->candidateIndex);
}

/**
 * Look up cached dependency rows for `keys` (target IDs or the discovery key)
 * and validate their candidates against current fingerprints. An entry is set
 * on a proven hit and nullopt on a miss.
 */
std::vector<std::optional<ValidatedDependencyBlob>> lookupValidatedDependencyBlobs(
    EvalState & state,
    const TecnixCacheScope & scope,
    std::span<const std::string> keys,
    DependencyFingerprintCache & fingerprintCache)
{
    std::vector<std::optional<ValidatedDependencyBlob>> results(keys.size());
    auto repoAccessor = getTecnixRepoAccessor(state);
    auto cachedBlobs = getTecnixEvalCache().lookupBlobs(scope, keys);

    struct BlobWork
    {
        TecnixEvalCache::DependencyBlobRef blob;
        std::vector<size_t> indices;
    };

    std::vector<BlobWork> blobWork;
    blobWork.reserve(std::min(keys.size(), TecnixEvalCache::dependencyShardCount));
    boost::unordered_flat_map<const std::string *, size_t> blobWorkByBlob;
    for (size_t i = 0; i < cachedBlobs.size(); i++) {
        if (!cachedBlobs[i])
            continue;
        auto key = cachedBlobs[i]->get();
        auto [it, inserted] = blobWorkByBlob.emplace(key, blobWork.size());
        if (inserted)
            blobWork.push_back({.blob = *cachedBlobs[i], .indices = {}});
        blobWork[it->second].indices.push_back(i);
    }

    for (auto & work : blobWork) {
        auto view = TecnixEvalCache::DependencyBlobView::open(*work.blob);
        if (!view)
            continue;
        auto currentFingerprints = view->currentFingerprints(repoAccessor, fingerprintCache);
        for (auto i : work.indices) {
            if (auto candidate = view->findMatchingCandidate(keys[i], currentFingerprints))
                results[i].emplace(
                    std::unique_ptr<ValidatedDependencyBlob::Impl>(
                        new ValidatedDependencyBlob::Impl{work.blob, *view, *candidate}));
        }
    }

    return results;
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

Value * ValidatedDependencyBlob::toValue(EvalState & state) const
{
    auto & view = impl->view;
    auto candidateIndex = impl->candidateIndex;
    auto pairStart = view.candidatePairStart(candidateIndex);
    auto pairCount = view.candidatePairCount(candidateIndex);
    if (!pairStart || !pairCount)
        throw Error("malformed Tecnix dependency cache row");

    auto attrs = state.buildBindings(*pairCount);
    for (uint32_t i = 0; i < *pairCount; i++) {
        auto pairIndex = *pairStart + i;
        auto pathId = view.pairPath(pairIndex);
        auto fingerprintId = view.pairFingerprint(pairIndex);
        if (!pathId || !fingerprintId)
            throw Error("malformed Tecnix dependency cache row");
        auto path = view.path(*pathId);
        auto fingerprint = view.fingerprint(*fingerprintId);
        if (!path || !fingerprint)
            throw Error("malformed Tecnix dependency cache row");

        auto * fingerprintValue = state.allocValue();
        fingerprintValue->mkString(*fingerprint, state.mem);
        attrs.insert(state.symbols.create(*path), fingerprintValue);
    }
    auto * val = state.allocValue();
    val->mkAttrs(attrs);

    return val;
}

} // namespace nix
