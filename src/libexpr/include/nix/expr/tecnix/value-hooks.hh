#pragma once
///@file

#include "nix/expr/tecnix/thread-state.hh"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nix {

/**
 * Tecnix source-access labels live outside Value, in a two-level sparse
 * table: a constant-initialized directory indexed by the top address bits,
 * pointing at lazily-mapped shadow chunks holding one 32-bit slot per
 * 16-byte-aligned value cell. Chunks are allocated only by the first
 * nonzero label store in their 4 GiB region, so a process that never runs
 * tracked evaluation allocates nothing; loads and clears on absent chunks
 * are no-ops (absent means all-zero). The directory lives in zeroed BSS,
 * so no dynamic initializer is involved and values finished during static
 * initialization are handled correctly by construction.
 */
constexpr size_t tecnixValueLabelDirSize = size_t{1} << 16; // addr >> 32; covers [0, 2^48)

extern std::atomic<uint32_t *> tecnixValueLabelDir[tecnixValueLabelDirSize];

uint32_t * tecnixInstallValueLabelChunk(size_t dirIndex);
[[noreturn]] void tecnixValueLabelOutOfRange(const void * value);

[[gnu::always_inline]] inline uint32_t tecnixValueLabelLoad(const void * value, std::memory_order order) noexcept
{
    auto addr = reinterpret_cast<uintptr_t>(value);
    auto dirIndex = addr >> 32;
    if (dirIndex >= tecnixValueLabelDirSize) [[unlikely]]
        return 0; // nonzero stores to uncovered addresses abort, so no label can exist here
    // Relaxed is sound: chunks are kernel-zeroed before their pointer is
    // CAS-released into the directory, and all slot access is through an
    // address dependency on the loaded pointer (the rcu_dereference pattern).
    auto * chunk = tecnixValueLabelDir[dirIndex].load(std::memory_order_relaxed);
    if (!chunk)
        return 0;
    return std::atomic_ref<uint32_t>(chunk[(addr & 0xffffffff) >> 4]).load(order);
}

[[gnu::always_inline]] inline void
tecnixValueLabelStore(const void * value, uint32_t accessSet, std::memory_order order) noexcept
{
    auto addr = reinterpret_cast<uintptr_t>(value);
    auto dirIndex = addr >> 32;
    if (dirIndex >= tecnixValueLabelDirSize) [[unlikely]] {
        if (accessSet != 0)
            tecnixValueLabelOutOfRange(value);
        return;
    }
    auto * chunk = tecnixValueLabelDir[dirIndex].load(std::memory_order_relaxed);
    if (!chunk) {
        if (accessSet == 0)
            return; // clearing an absent chunk: already zero
        chunk = tecnixInstallValueLabelChunk(dirIndex);
    }
    std::atomic_ref<uint32_t>(chunk[(addr & 0xffffffff) >> 4]).store(accessSet, order);
}

/**
 * Clear-if-set: reads of untouched demand-zero pages map the shared zero
 * page and commit nothing, so eliding the 0-over-0 store keeps the table's
 * physical footprint proportional to labels actually published rather than
 * to every value ever finished inside a chunk's region.
 */
[[gnu::always_inline]] inline void tecnixValueLabelClear(const void * value) noexcept
{
    auto addr = reinterpret_cast<uintptr_t>(value);
    auto dirIndex = addr >> 32;
    if (dirIndex >= tecnixValueLabelDirSize) [[unlikely]]
        return;
    auto * chunk = tecnixValueLabelDir[dirIndex].load(std::memory_order_relaxed);
    if (!chunk)
        return;
    auto slot = std::atomic_ref<uint32_t>(chunk[(addr & 0xffffffff) >> 4]);
    if (slot.load(std::memory_order_relaxed) != 0)
        slot.store(0, std::memory_order_relaxed);
}

/**
 * The tracer's side table: the same directory structure as the label table
 * with an 8-byte slot per 16-byte cell, holding `(record id << 24) | root`
 * for the record that produced the cell's current value (see tecnix/trace.hh).
 * Written exactly once per finished value, at publish; cleared by the finish
 * hook like the label. Chunks are 2 GiB sparse mappings covering 4 GiB of
 * address space each, installed on the first store in their region.
 */
extern std::atomic<uint64_t *> tecnixValueTraceDir[tecnixValueLabelDirSize];

/**
 * Set when the first tracing session is created in this process and never
 * cleared: slots may outlive the call that wrote them, so the finish hook
 * must keep clearing them. Off means the hook pays one predicted branch.
 */
extern std::atomic<bool> tecnixTraceEverEnabled;

uint64_t * tecnixInstallValueTraceChunk(size_t dirIndex);

[[gnu::always_inline]] inline uint64_t tecnixValueTraceLoad(const void * value, std::memory_order order) noexcept
{
    auto addr = reinterpret_cast<uintptr_t>(value);
    auto dirIndex = addr >> 32;
    if (dirIndex >= tecnixValueLabelDirSize) [[unlikely]]
        return 0;
    auto * chunk = tecnixValueTraceDir[dirIndex].load(std::memory_order_relaxed);
    if (!chunk)
        return 0;
    return std::atomic_ref<uint64_t>(chunk[(addr & 0xffffffff) >> 4]).load(order);
}

/** Store a nonzero slot (clearing is `tecnixValueTraceClear`). */
[[gnu::always_inline]] inline void
tecnixValueTraceStore(const void * value, uint64_t slot, std::memory_order order) noexcept
{
    auto addr = reinterpret_cast<uintptr_t>(value);
    auto dirIndex = addr >> 32;
    if (dirIndex >= tecnixValueLabelDirSize) [[unlikely]] {
        tecnixValueLabelOutOfRange(value);
        return;
    }
    auto * chunk = tecnixValueTraceDir[dirIndex].load(std::memory_order_relaxed);
    if (!chunk)
        chunk = tecnixInstallValueTraceChunk(dirIndex);
    std::atomic_ref<uint64_t>(chunk[(addr & 0xffffffff) >> 4]).store(slot, order);
}

/** Clear-if-set, for the same reason as `tecnixValueLabelClear`. */
[[gnu::always_inline]] inline void tecnixValueTraceClear(const void * value) noexcept
{
    auto addr = reinterpret_cast<uintptr_t>(value);
    auto dirIndex = addr >> 32;
    if (dirIndex >= tecnixValueLabelDirSize) [[unlikely]]
        return;
    auto * chunk = tecnixValueTraceDir[dirIndex].load(std::memory_order_relaxed);
    if (!chunk)
        return;
    auto slot = std::atomic_ref<uint64_t>(chunk[(addr & 0xffffffff) >> 4]);
    if (slot.load(std::memory_order_relaxed) != 0)
        slot.store(0, std::memory_order_relaxed);
}

void publishTrackedValueDependencies(const void * value);
void copyTrackedValueDependencies(void * dst, const void * src);
void publishCopiedValueDependencies(void * dst, const void * src);

/**
 * ValueStorage::finish is the single chokepoint through which every cell
 * becomes a finished value, so clearing the label slot here is what
 * guarantees a label is never stale: whatever the slot held for a previous
 * occupant of this address (a reused GC cell, a reused stack slot), the
 * finished value starts empty and receives its label from the publish that
 * follows, ordered before the cell is observable as finished. The tracer's
 * slot follows the same rule.
 */
inline void tecnixValueFinishHook(const void * value)
{
    tecnixValueLabelClear(value);
    if (tecnixTraceEverEnabled.load(std::memory_order_relaxed)) [[unlikely]]
        tecnixValueTraceClear(value);
    if (currentTecnixThreadState.valueDependencyPublishValue == value)
        publishTrackedValueDependencies(value);
}

inline void tecnixValueCopyBeforeFinish(void * dst, const void * src)
{
    copyTrackedValueDependencies(dst, src);
}

inline void tecnixValueCopyAfterFinish(void * dst, const void * src)
{
    publishCopiedValueDependencies(dst, src);
}

} // namespace nix
