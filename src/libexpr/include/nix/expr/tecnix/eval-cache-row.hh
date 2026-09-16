#pragma once
///@file
///
/// The Tecnix evaluation cache's packed shard row format (TXDC), as pure
/// functions over bytes: merge freshly learned closures into a row, and decode
/// a row into plain data. The cache's storage layer (tecnix/eval-cache.cc) is
/// the only production user of the merge; the decoder exists for tests and
/// debugging, where materializing a row is fine.

#include "nix/expr/tecnix/eval-cache.hh"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

/** What became of the stored row when a shard was written. */
enum class TecnixRowMergeOutcome {
    /** There was no stored row. */
    Fresh,
    /** The stored row was merged into. */
    Merged,
    /** The stored row declared another format version and was replaced. */
    ReplacedOtherVersion,
    /** The stored row could not be read and was replaced. */
    ReplacedUnreadable,
    /** The stored row had more targets than a row may hold and was replaced. */
    ReplacedFull,
};

/**
 * The row for a shard after learning `updates`, written into `out`: each
 * update's closure becomes the newest candidate of its key, an equal stored
 * candidate is dropped, and every history in the row is trimmed to its
 * `historyLimit` most recent candidates (the fresh closure is always kept,
 * so a limit of 0 acts as 1). Throws if the row would exceed `maxRowBytes`,
 * or on an internal error; never writes a row it could not vouch for.
 */
TecnixRowMergeOutcome mergeTecnixDependencyRow(
    std::optional<std::string_view> existingBlob,
    const std::vector<const TecnixDependencyUpsert *> & updates,
    size_t historyLimit,
    size_t maxRowBytes,
    std::string & out);

struct TecnixDependencyRow
{
    struct Candidate
    {
        DependencyClosure dependencies;
        std::string payload;
    };

    struct Target
    {
        std::string target;
        /** Newest first. */
        std::vector<Candidate> candidates;
    };

    /** In stored (sorted) order. */
    std::vector<Target> targets;
};

/** A row decoded into plain data, or nullopt if it cannot be read. */
std::optional<TecnixDependencyRow> decodeTecnixDependencyRow(std::string_view blob);

} // namespace nix
