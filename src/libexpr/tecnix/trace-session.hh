#pragma once
///@file
///
/// The Tecnix tracing session: per-thread record buffers, id allocation,
/// import keys, roots and phases, a background writer that streams filled
/// record chunks into the SQLite file while evaluation runs, and the
/// end-of-call dump of everything else. One per `EvalState`, created by the
/// first tracked builtin call with `tecnix-trace` set and kept for the
/// state's lifetime so that record ids stay unique across calls and slots
/// written by an earlier call remain valid references into the same file.
/// Owned by `EvalState::TecnixEvalData`.

#include "nix/expr/tecnix/trace.hh"
#include "nix/store/sqlite.hh"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nix {

class EvalState;
struct TraceSession;

/**
 * One evaluation thread's records and reuses. Ordinary heap memory (Boehm
 * does not scan it). The current chunk is appended to only by its thread;
 * a full chunk is handed to the session's writer, so at most one chunk per
 * thread is ever held here.
 */
struct TraceThreadBuffer
{
    /** 256 Ki records of 72 bytes: 18 MiB per chunk, one transaction each. */
    static constexpr size_t chunkRecords = size_t{1} << 18;

    TraceSession & session;
    std::vector<TraceRecord> current;
    std::vector<TraceReuse> reuses;
    /** Session-local dense index, the high bits of every id from this thread. */
    const uint32_t threadIndex;
    uint32_t nextCounter = 1;
    /** Records dropped because memory could not be allocated. */
    uint64_t dropped = 0;

    TraceThreadBuffer(TraceSession & session, uint32_t threadIndex)
        : session(session)
        , threadIndex(threadIndex)
    {
    }

    uint64_t nextId() noexcept
    {
        return (uint64_t{threadIndex} << 32) | nextCounter++;
    }

    void append(const TraceRecord & record) noexcept;
    void appendReuse(TraceReuse reuse) noexcept;
};

struct TraceRootInfo
{
    uint32_t id;
    uint64_t record;
    std::string name;
};

struct TracePhaseInfo
{
    uint32_t call;
    std::string name;
    int64_t start, end;
};

/**
 * Streams record chunks into the trace file on its own thread while
 * evaluation runs, so the run holds at most a few chunks in memory and the
 * end-of-call dump has only the small tables left to write. Owns the SQLite
 * connection; the dump uses it too, after `drain()`, when the thread is idle.
 * Never touches the GC heap.
 */
struct TraceWriter
{
    /** Chunks queued but not yet written before an appending thread blocks. */
    static constexpr size_t maxQueuedChunks = 128;

    const std::filesystem::path path;
    /** Subtracted from every record's start: times in the file are run-relative. */
    const int64_t epoch;
    std::optional<SQLite> db;
    std::optional<SQLiteStmt> insertRecord;
    /** Indexes exist and would slow inserts: the next write drops them, the dump rebuilds them. */
    bool indexed = false;
    uint64_t rowsWritten = 0;
    /** Distinct expr ids of written records; resolved at dump time. */
    std::unordered_set<uint64_t> exprKeys;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<TraceRecord>> queue;
    bool writing = false;
    bool stop = false;
    std::exception_ptr error;
    std::thread thread;

    TraceWriter(std::filesystem::path path, int64_t epoch);
    TraceWriter(const TraceWriter &) = delete;
    TraceWriter & operator=(const TraceWriter &) = delete;
    ~TraceWriter();

    /** Hand a chunk to the writer; blocks briefly if the queue is full. */
    void enqueue(std::vector<TraceRecord> chunk);
    /** Wait until every queued chunk is in the file. */
    void drain();
    /** Open the file and create the schema if not yet done (writer idle or writer thread). */
    void ensureOpen();

private:
    void run();
    void writeChunk(const std::vector<TraceRecord> & chunk);
};

struct TraceSession
{
    /** Ids fit 8 bits of thread index above a 32-bit counter. */
    static constexpr size_t maxThreads = 256;

    EvalState & state;
    const std::filesystem::path dir;
    /** Steady-clock ns at creation; every time in the file is relative to it. */
    const int64_t epoch;
    TraceWriter writer;

    /** Guards everything below. Held for the whole dump, so a call starting on
        another thread mid-dump waits at its first buffer lookup. */
    std::mutex mutex;
    /** Keyed by `myEvalThreadId`. */
    std::map<uint32_t, std::unique_ptr<TraceThreadBuffer>> buffers;
    uint32_t nextRoot = 1;
    std::vector<TraceRootInfo> roots;
    std::vector<TracePhaseInfo> phases;
    /** Index 0 is unused (0 means "no key"). */
    std::vector<std::string> importKeys{std::string{}};
    std::unordered_map<std::string, uint32_t> importKeyIds;
    std::unordered_set<uint64_t> dumpedExprs;
    size_t dumpedGcPauses = 0;
    /** Depth of nested tracked builtin calls; the dump runs when it returns to 0. */
    std::atomic<int> callDepth{0};
    uint32_t nextCall = 1;
    uint64_t dumps = 0;
    /** `writer.rowsWritten` when the previous dump finished. */
    uint64_t rowsAtLastDump = 0;
    std::string gitDir, rev, checkoutPath, resolver;

    TraceSession(EvalState & state, std::filesystem::path dir);
    TraceSession(const TraceSession &) = delete;
    TraceSession & operator=(const TraceSession &) = delete;

    TraceThreadBuffer & bufferForThisThread();
    uint32_t registerRoot(uint64_t record, std::string_view name);
    uint32_t internImportKey(std::string_view path);
    void addPhase(uint32_t call, std::string name, int64_t start, int64_t end);

    /** Flush the buffers, write everything else recorded since the previous dump; never throws. */
    void dump() noexcept;

    std::filesystem::path filePath() const;
};

/**
 * Brackets one public Tecnix builtin call. Must be the first statement of
 * the primop so it is destroyed last -- after every `TrackingContext` in the
 * call has closed its root record -- and runs the dump when the outermost
 * call returns, on the exception path too.
 */
struct TecnixTraceCall
{
    TraceSession * session = nullptr;
    uint32_t call = 0;

    explicit TecnixTraceCall(EvalState & state);
    TecnixTraceCall(const TecnixTraceCall &) = delete;
    TecnixTraceCall & operator=(const TecnixTraceCall &) = delete;
    ~TecnixTraceCall();

    void
    noteArgs(std::string_view gitDir, std::string_view rev, std::string_view checkoutPath, std::string_view resolver);
};

/** Times one non-evaluation section of a builtin call into `phases`. */
struct TecnixTracePhase
{
    TraceSession * session;
    uint32_t call;
    const char * name;
    int64_t start;

    TecnixTracePhase(EvalState & state, uint32_t call, const char * name);
    TecnixTracePhase(const TecnixTracePhase &) = delete;
    TecnixTracePhase & operator=(const TecnixTracePhase &) = delete;
    ~TecnixTracePhase();
};

int64_t traceNow() noexcept;

} // namespace nix
