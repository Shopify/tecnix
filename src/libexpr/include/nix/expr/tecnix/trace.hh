#pragma once
///@file
///
/// Tecnix evaluation tracing: a recorder riding on the source-dependency
/// tracker's frames. For every thunk forced under a tracing `TrackingContext`
/// it appends one record -- who needed the value first, which root computed
/// it, how long it took, how many GC-heap bytes it allocated -- and it
/// records which roots later reused values other roots computed. Everything
/// is held in memory and written to SQLite when the outermost tracked builtin
/// call returns. Design: plans/tecnix-tracing/README.md.
///
/// This header is the hot-path surface used by `forceValueTracked` and the
/// value hooks. The session, buffers and dump live in tecnix/trace-session.hh
/// and tecnix/trace.cc.

#include "nix/expr/tecnix/source-deps.hh"
#include "nix/expr/tecnix/value-hooks.hh"
#include "nix/util/pos-idx.hh"

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nix {

class EvalState;
struct Expr;
struct SourcePath;
struct TraceSession;
struct TraceThreadBuffer;
struct Value;

/**
 * One finished record. Plain data, 72 bytes; appended to a per-thread chunk
 * at frame close and never touched again until the dump.
 */
struct TraceRecord
{
    /**
     * `Value`: a thunk this thread computed. `Root`: a target (or the
     * resolver, or discovery). `Wait`: this thread waited for another thread
     * to finish the value. `Fetch`: a fetcher call (`fetchGit`, `fetchTree`,
     * `fetchurl`, `fetchTarball`, ...): I/O, and cacheable, so kept apart
     * from evaluation. `Store`: a store write on the evaluator's behalf
     * (`derivationStrict`'s `.drv`, a source copy). Fetch and store records
     * are children of the frame that triggered them, so that frame's own
     * time is evaluation only.
     */
    enum Kind : uint8_t { Value = 0, Root = 1, Wait = 2, Fetch = 3, Store = 4 };

    enum KeyKind : uint8_t { NoKey = 0, Pos = 1, Import = 2 };

    /** `(thread index << 32) | per-thread counter`; 40 bits used. */
    uint64_t id;
    /** The record that was open when this one started; 0 for none. */
    uint64_t parent;
    /** Root id; 24 bits used. */
    uint32_t root;
    /** `PosIdx` raw value or import-key index, per `keyKind`. */
    uint32_t key;
    Kind kind;
    KeyKind keyKind;
    /** Nanoseconds (steady clock; made run-relative at dump) and bytes. */
    int64_t start, dur, gcAlloc, selfDur, selfGcAlloc;
};

static_assert(std::is_trivially_copyable_v<TraceRecord>);
static_assert(sizeof(TraceRecord) == 72);

struct TraceReuse
{
    uint32_t root;
    uint64_t producer;
};

/**
 * A value's tracer slot: `(record id << 24) | root`, held in the address-keyed
 * side table beside the tracker's label. Written exactly once, at publish.
 */
constexpr uint64_t tecnixTracePackSlot(uint64_t record, uint32_t root)
{
    return (record << 24) | root;
}

constexpr uint32_t tecnixTraceSlotRoot(uint64_t slot)
{
    return static_cast<uint32_t>(slot & 0xffffff);
}

constexpr uint64_t tecnixTraceSlotRecord(uint64_t slot)
{
    return slot >> 24;
}

/**
 * The tracing state of one value-force frame. Left uninitialised until
 * `traceFrameOpen`, which runs only when the context is tracing.
 */
struct TraceFrameData
{
    /** The enclosing tracing frame in this root, or null when the root's own
        record is the parent. */
    TraceFrameData * parent;
    /**
     * The thunk's expression, or for a delayed application the function
     * being applied, read at open without dereferencing: another thread may
     * finish the thunk (overwriting the words) at any time. Dereferenced at
     * close only if this frame published, i.e. this thread owned the thunk
     * from before the read until it finished. (The frame lives on the stack,
     * so the collector sees `appFun` and keeps it alive until then.)
     */
    Expr * expr;
    const Value * appFun;
    uint64_t id;
    /** Slot of the value this frame's value was copied from, or 0. */
    uint64_t aliasOf;
    uint32_t importKey;
    /** The force site, used when the thunk has no position of its own. */
    PosIdx pos;
    int64_t t0, gc0, childDur, childAlloc;
    uint64_t a0;
};

/**
 * The frame `forceValueTracked` pushes. Every value frame under a tracing
 * context is one of these (the tracer downcasts `nearestValueForceFrame`
 * on publish); the trailing data costs nothing until tracing initialises it.
 */
struct TracedSourceDepsFrame final : TrackedSourceDepsFrame
{
    using TrackedSourceDepsFrame::TrackedSourceDepsFrame;

    TraceFrameData trace;
};

/**
 * Per-root tracing state, owned by a `TrackingContext` for exactly its
 * lifetime. Constructing it opens the root's record; destroying it closes
 * the record and feeds the enclosing frame's accumulators like any child.
 * Contexts are thread-confined, so the thread buffer is looked up once here.
 */
struct TraceRoot
{
    TraceSession & session;
    TraceThreadBuffer & buffer;
    uint32_t root;
    /** The root's own record id. */
    uint64_t record;
    /** The record that was open when the root started (a nested call), or 0. */
    uint64_t parentRecord;
    /** Where to feed this root's cost at close: the enclosing context's open
        frame, else the enclosing root, else nowhere. */
    TraceFrameData * parentFrame;
    TraceRoot * parentRoot;
    /** The innermost open tracing frame in this root. */
    TraceFrameData * current = nullptr;
    int64_t t0, gc0, childDur = 0, childAlloc = 0;
    uint64_t a0;
    /** Producers already recorded as reused by this root: one row per (root, producer). */
    boost::unordered_flat_set<uint64_t> seen;

    TraceRoot(TraceSession & session, std::string_view name);
    TraceRoot(const TraceRoot &) = delete;
    TraceRoot & operator=(const TraceRoot &) = delete;
    ~TraceRoot();
};

/** The state's tracing session, or null when none has been created. */
TraceSession * tecnixTraceSession(EvalState & state);

/** Create the state's session if `tecnix-trace` is set (idempotent); null otherwise. */
TraceSession * ensureTecnixTraceSession(EvalState & state);

/** Everything recorded since the last dump; for tests and debugging. */
struct TraceSnapshot
{
    std::vector<TraceRecord> records;
    std::vector<TraceReuse> reuses;
    /** (root id, name) */
    std::vector<std::pair<uint32_t, std::string>> roots;
    /** Index 0 unused; the rest as recorded by `evalFile`. */
    std::vector<std::string> importKeys;
};

TraceSnapshot tecnixTraceSnapshot(EvalState & state);

/** Write the trace file now, as the outermost builtin call does on return. */
void tecnixTraceDump(EvalState & state);

/* Out of line on purpose: `forceValueTracked` is inlined into every
   `forceValue` site, and these run only under tracing. */

/** Thunk/app branch, before `v.force`. */
void traceFrameOpen(TraceRoot & root, TracedSourceDepsFrame & frame, const Value & v, PosIdx pos) noexcept;
/** Thunk/app branch, at close (inside the `Finally`, after the tracker's merge). */
void traceFrameClose(TraceRoot & root, TracedSourceDepsFrame & frame) noexcept;
/** Finished branch: a use of an already-finished value. */
void traceFinishedValue(TraceRoot & root, const Value & v) noexcept;
/** Pending/awaited branch: forces `v`, timing the wait as a child of the open frame. */
void traceWaitedForce(EvalState & state, Value & v, PosIdx pos, TraceRoot & root);
/** Publish hook: the frame's value becomes visible; write its slot. */
void tracePublish(TraceRoot & root, TracedSourceDepsFrame & frame) noexcept;
/** Before-finish copy hook: the frame's value is a copy of `src`. */
void traceCopyIntoFrame(TraceRoot & root, TracedSourceDepsFrame & frame, const Value & src) noexcept;
/** After-finish copy hook for any other destination: the copy keeps `src`'s identity. */
void traceCopy(const Value & dst, const Value & src) noexcept;
/** `evalFile`: key the next frame opened on this thread on `path`. */
void tecnixTraceNoteImport(TrackingContext & trackingCtx, const SourcePath & path);

void traceIoOpen(TraceRoot & root, TraceFrameData & data, PosIdx pos) noexcept;
void traceIoClose(TraceRoot & root, TraceFrameData & data, TraceRecord::Kind kind) noexcept;

/**
 * Brackets I/O the evaluator does on a thunk's behalf -- a fetcher call or a
 * store write -- as a `Fetch`/`Store` child record of the open frame, keyed
 * on the call site. Thunks forced inside the scope (a fetcher's arguments)
 * nest under it as ordinary records, so the record's own time is the I/O.
 * Costs one thread-local load when the thread is not tracing.
 */
struct TecnixTraceIoScope
{
    TraceRoot * root = nullptr;
    TraceFrameData data;
    TraceRecord::Kind kind;

    TecnixTraceIoScope(PosIdx pos, TraceRecord::Kind kind) noexcept
        : kind(kind)
    {
        if (auto * ctx = currentTecnixThreadState.trackingContext) [[unlikely]]
            if ((root = ctx->trace.get()))
                traceIoOpen(*root, data, pos);
    }

    TecnixTraceIoScope(const TecnixTraceIoScope &) = delete;
    TecnixTraceIoScope & operator=(const TecnixTraceIoScope &) = delete;

    ~TecnixTraceIoScope()
    {
        if (root)
            traceIoClose(*root, data, kind);
    }
};

} // namespace nix
