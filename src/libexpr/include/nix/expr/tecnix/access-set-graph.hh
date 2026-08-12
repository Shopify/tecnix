#pragma once
///@file
///
/// The interning graph behind Tecnix source-deps labels: paths become 32-bit
/// access ids, sets of paths become canonical 32-bit set ids (equal sets
/// share one id). Append-only for the EvalState's lifetime — labels held by
/// surviving values stay resolvable forever, so the graph is never cleared.
///
/// Split from source-deps.hh so the tracking machinery that eval.hh inlines
/// (frames, contexts, forceValueTracked) does not pull the interning
/// containers into every evaluator translation unit.

#include "nix/expr/tecnix/source-deps.hh"
#include "nix/util/strings.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

struct EvalSourceAccessSetNode
{
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t nextWithSameHash = 0;
    uint64_t hash = 0;
};

class EvalSourceAccessSetGraph
{
    std::atomic<bool> enabled{false};
    mutable std::mutex mutex;
    boost::concurrent_flat_map<std::string, EvalSourceAccessId, StringViewHash, std::equal_to<>> accessIds;
    std::vector<std::string> accesses;
    std::vector<EvalSourceAccessSetNode> accessSets;
    std::vector<EvalSourceAccessId> accessSetItems;
    std::vector<EvalSourceAccessSetId> singletonAccessSets;
    boost::unordered_flat_map<uint64_t, EvalSourceAccessSetId> accessSetIdsByHash;
    boost::unordered_flat_map<uint64_t, EvalSourceAccessSetId> pairUnionAccessSets;
    mutable std::vector<uint32_t> seenAccessGenerations;
    mutable uint32_t nextFlattenGeneration = 1;

    bool accessSetEquals(EvalSourceAccessSetId id, const std::vector<EvalSourceAccessId> & items) const;

public:
    EvalSourceAccessSetGraph();

    bool isEnabled() const
    {
        return enabled.load(std::memory_order_acquire);
    }

    void enable();

    EvalSourceAccessId internAccess(std::string_view path);
    EvalSourceAccessSetId internAccessSet(
        std::span<const EvalSourceAccessId> directAccesses, std::span<const EvalSourceAccessSetId> children);
    EvalSourceAccessSetId internAccessSet(
        const std::vector<EvalSourceAccessId> & directAccesses, const std::vector<EvalSourceAccessSetId> & children);
    std::string access(EvalSourceAccessId id) const;

    /**
     * Resolve direct accesses plus the transitive members of `accessSetEdges`
     * to unique repo-relative paths, in first-seen order.
     */
    std::vector<std::string> flatten(
        const std::vector<EvalSourceAccessId> & directAccesses,
        const std::vector<EvalSourceAccessSetId> & accessSetEdges) const;
    EvalSourceAccessSetStats stats() const;
};

} // namespace nix
