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
#include <memory>
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
    /** Stored as the candidate's payload (e.g. discovery's target-name JSON); empty for none. */
    std::string payload;
};

/**
 * A proven cache hit: one stored candidate whose complete closure matched
 * current fingerprints. A move-only handle over the stored row bytes; output
 * is built directly from the row (no decoded object graph).
 */
class ValidatedDependencyBlob
{
public:
    struct Impl;

    explicit ValidatedDependencyBlob(std::unique_ptr<Impl> impl);
    ValidatedDependencyBlob(ValidatedDependencyBlob &&) noexcept;
    ValidatedDependencyBlob & operator=(ValidatedDependencyBlob &&) noexcept;
    ~ValidatedDependencyBlob();

    /** Dependency output (`path = fingerprint` attrs) built from the matched candidate's pair stream. */
    Value * toValue(EvalState & state) const;

    /** The matched candidate's payload (e.g. discovery's target-name JSON), if any. */
    std::optional<std::string_view> payload() const;

private:
    std::unique_ptr<Impl> impl;
};

/**
 * Look up cached dependency rows for `keys` (target IDs or the discovery key)
 * and validate their candidates against current fingerprints. An entry is set
 * on a proven hit and nullopt on a miss.
 */
std::vector<std::optional<ValidatedDependencyBlob>> lookupValidatedDependencyBlobs(
    EvalState & state,
    const TecnixCacheScope & scope,
    std::span<const std::string> keys,
    DependencyFingerprintCache & fingerprintCache);

/**
 * Persist freshly learned closures. Cache writes are an optimization:
 * failures warn and continue, and never fail the evaluation.
 */
void upsertDependencyClosures(const TecnixCacheScope & scope, const std::vector<TecnixDependencyUpsert> & entries);

} // namespace nix
