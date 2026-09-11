#pragma once
///@file
///
/// The persistent Tecnix evaluation cache: bounded per-key source-closure
/// histories in SQLite, validated against current fingerprints (never trusted
/// by key; see plans/tecnix-target-eval-caching/). This header is the narrow
/// boundary the builtins use; the TXDC blob format, sharding, and SQLite
/// schema are implementation details of tecnix/eval-cache.cc.

#include "nix/util/ref.hh"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

class EvalState;
struct SourceAccessor;
struct Value;

struct DependencyEntry
{
    std::string path;
    std::string fingerprint;
};

using DependencyClosure = std::vector<DependencyEntry>;

/**
 * Per-run token for the thread-local fingerprint memo: fingerprints are
 * memoized per unique path for the lifetime of one of these, so validation
 * cost scales with unique paths, not total closure entries.
 */
struct DependencyFingerprintCache
{
    uint64_t generation;

    DependencyFingerprintCache();
};

std::optional<std::string>
dependencyFingerprint(ref<SourceAccessor> accessor, std::string_view path, DependencyFingerprintCache & cache);

/** Fingerprint every path, throwing if any path cannot be certified. */
DependencyClosure dependencyFingerprints(
    ref<SourceAccessor> accessor, const std::vector<std::string> & paths, DependencyFingerprintCache & cache);

/**
 * Reserved cache key under which target discovery stores its closure and
 * discovered-name payload. Rejected as a caller-supplied target id.
 */
constexpr std::string_view tecnixTargetNamesCacheKey = "__tecnixTargetNames";

/** The (gitDir, resolver, argsKey) row family a lookup or upsert addresses. */
struct TecnixCacheScope
{
    std::string_view gitDir;
    std::string_view resolver;
    std::string_view argsKey;
};

struct TecnixDependencyUpsert
{
    std::string_view target;
    const DependencyClosure * dependencies;
    /** Stored as the candidate's payload (discovery's target-name JSON, or a
        target's evaluated drvPath/outputName result); empty for none. */
    std::string payload;
};

/**
 * A proven cache hit: one stored candidate whose complete closure matched
 * current fingerprints. A non-owning view into the shard row being validated,
 * valid only for the duration of the callback that receives it; output is
 * built directly from the row bytes (no decoded object graph), and callers
 * copy out whatever they keep.
 */
class DependencyCacheHit
{
public:
    struct Impl;

    explicit DependencyCacheHit(const Impl & impl);
    DependencyCacheHit(const DependencyCacheHit &) = delete;
    DependencyCacheHit & operator=(const DependencyCacheHit &) = delete;

    /** Dependency output (`path = fingerprint` attrs) built from the matched candidate's pair stream. */
    Value * toValue(EvalState & state) const;

    /** The matched candidate's payload (discovery's target-name JSON, or a
        target's evaluated drvPath/outputName result), if any. */
    std::optional<std::string_view> payload() const;

private:
    const Impl & impl;
};

/**
 * Look up cached dependency rows for `keys` (target IDs or the discovery key)
 * and validate their candidates against current fingerprints, one shard row
 * at a time: `onHit(i, hit)` is called for each proven key while its row is
 * loaded, and the row is released before the next shard is read, so memory is
 * bounded by one row rather than the whole scope. Keys never reported are
 * misses.
 */
void lookupCachedDependencies(
    EvalState & state,
    const TecnixCacheScope & scope,
    std::span<const std::string> keys,
    DependencyFingerprintCache & fingerprintCache,
    const std::function<void(size_t index, const DependencyCacheHit & hit)> & onHit);

/**
 * Persist freshly learned closures by merging each into its key's stored
 * history: the fresh closure becomes the newest candidate, and every history
 * in the rewritten rows is trimmed to its `historyLimit` most recent distinct
 * candidates (the fresh closure is always kept). The merge happens under the
 * database write lock, so evaluators sharing a cache accumulate each other's
 * candidates instead of overwriting them. Cache writes are an optimization:
 * failures warn and continue, and never fail the evaluation.
 */
void upsertDependencyClosures(
    const TecnixCacheScope & scope, const std::vector<TecnixDependencyUpsert> & entries, size_t historyLimit);

} // namespace nix
