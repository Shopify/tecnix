#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/store/globals.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ============================================================================
// Worldtree connection-pool tests (the accessor side)
//
// worldtreed's own suite covers the daemon-side session invariants; these cover the
// *client* pool in `EvalState::acquireWorldtreeZoneSession` -- the logic most likely to
// silently regress to one-connection-per-zone: lazy growth bounded by
// `tectonix-worldtree-max-connections`, round-robin reuse past the cap, and release of
// each per-zone session when its accessor handle drops (a `scoped.close_ro` on the
// hosting connection). We drive the *real* Client + pool + session RAII against a minimal
// in-process fake worldtreed speaking the real control-socket wire, so nothing about the
// production path is stubbed.
// ============================================================================

namespace nix {

namespace {

// ---- worldtree wire helpers (mirror libutil/worldtree-client.cc, server direction) ----

constexpr uint32_t WT_F_REQUEST_ID = 1;
constexpr uint32_t WT_F_KIND = 2;
constexpr uint32_t WT_F_METHOD = 3;
constexpr uint32_t WT_F_PAYLOAD = 4;
constexpr uint64_t WT_KIND_RESPONSE = 2;

void wtPutVarint(std::string & out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

/// Singular varint field, omitted when zero (proto3 default) -- matches the client encoder.
void wtPutVarintField(std::string & out, uint32_t field, uint64_t v)
{
    if (v == 0)
        return;
    wtPutVarint(out, (static_cast<uint64_t>(field) << 3) | 0);
    wtPutVarint(out, v);
}

/// Singular length-delimited field, omitted when empty (proto3 default).
void wtPutLenField(std::string & out, uint32_t field, std::string_view bytes)
{
    if (bytes.empty())
        return;
    wtPutVarint(out, (static_cast<uint64_t>(field) << 3) | 2);
    wtPutVarint(out, bytes.size());
    out.append(bytes);
}

uint64_t wtReadVarintBuf(const std::string & buf, size_t & p)
{
    uint64_t v = 0;
    int shift = 0;
    while (p < buf.size()) {
        uint8_t b = static_cast<uint8_t>(buf[p++]);
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
    }
    return v;
}

bool wtReadExact(int fd, char * buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, buf + off, n - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false; // peer closed
        off += static_cast<size_t>(r);
    }
    return true;
}

void wtWriteAll(int fd, const std::string & bytes)
{
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += static_cast<size_t>(w);
    }
}

/// A minimal in-process fake worldtree daemon. It answers only the two session verbs the
/// pool exercises -- `scoped.open_ro` (with a fresh, monotonic session id) and
/// `scoped.close_ro` (recording the released id) -- and records how many connections were
/// accepted and which sessions opened on each, so a test can assert the accessor pool's
/// bounded, round-robin, close-on-drop behaviour. Not a general daemon; every other verb
/// gets a benign empty reply (never reached by these tests).
class FakeWorldtreeDaemon
{
public:
    explicit FakeWorldtreeDaemon(std::string path)
        : path_(std::move(path))
    {
        listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        assert(listenFd_ >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        assert(path_.size() < sizeof(addr.sun_path));
        std::memcpy(addr.sun_path, path_.c_str(), path_.size());
        ::unlink(path_.c_str());
        assert(::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
        assert(::listen(listenFd_, 128) == 0);
        acceptThread_ = std::thread([this] { acceptLoop(); });
    }

    ~FakeWorldtreeDaemon()
    {
        stop_ = true;
        // Wake a blocked accept() with a throwaway self-connect, then join.
        int w = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (w >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, path_.c_str(), path_.size());
            (void) ::connect(w, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            ::close(w);
        }
        if (acceptThread_.joinable())
            acceptThread_.join();
        // Unblock any worker still reading (if a client fd has not closed yet), join, close.
        for (int fd : connFds_)
            ::shutdown(fd, SHUT_RDWR);
        for (auto & t : workers_)
            if (t.joinable())
                t.join();
        for (int fd : connFds_)
            ::close(fd);
        if (listenFd_ >= 0)
            ::close(listenFd_);
        ::unlink(path_.c_str());
    }

    const std::string & path() const
    {
        return path_;
    }

    size_t connectionCount()
    {
        std::lock_guard<std::mutex> l(mtx_);
        return sessionsPerConn_.size();
    }

    /// Per-accepted-connection, the session ids opened on it, in acceptance order.
    std::vector<std::vector<uint64_t>> sessionsPerConn()
    {
        std::lock_guard<std::mutex> l(mtx_);
        return sessionsPerConn_;
    }

    std::set<uint64_t> closedSessions()
    {
        std::lock_guard<std::mutex> l(mtx_);
        return closed_;
    }

private:
    void acceptLoop()
    {
        for (;;) {
            int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0)
                return;
            if (stop_) {
                ::close(fd);
                return;
            }
            size_t idx;
            {
                std::lock_guard<std::mutex> l(mtx_);
                idx = sessionsPerConn_.size();
                sessionsPerConn_.emplace_back();
                connFds_.push_back(fd);
            }
            workers_.emplace_back([this, fd, idx] { serve(fd, idx); });
        }
    }

    void serve(int fd, size_t idx)
    {
        for (;;) {
            uint64_t reqId = 0;
            std::string method, payload;
            if (!readFrame(fd, reqId, method, payload))
                return; // client closed the connection
            if (method == "scoped.open_ro") {
                uint64_t session;
                {
                    std::lock_guard<std::mutex> l(mtx_);
                    session = nextSession_++;
                    sessionsPerConn_[idx].push_back(session);
                }
                std::string pl;
                wtPutVarintField(pl, 1, session); // OpenRoResp { ws = 1 }
                sendResponse(fd, reqId, pl);
            } else if (method == "scoped.close_ro") {
                uint64_t ws = firstVarintField(payload); // CloseRoReq { ws = 1 }
                {
                    std::lock_guard<std::mutex> l(mtx_);
                    closed_.insert(ws);
                }
                sendResponse(fd, reqId, {}); // CloseRoResp is empty
            } else {
                sendResponse(fd, reqId, {});
            }
        }
    }

    /// Value of field 1 (varint) in a message, or 0 if absent.
    static uint64_t firstVarintField(const std::string & body)
    {
        size_t p = 0;
        while (p < body.size()) {
            uint64_t tag = wtReadVarintBuf(body, p);
            if ((tag >> 3) == 1 && (tag & 0x7) == 0)
                return wtReadVarintBuf(body, p);
            break;
        }
        return 0;
    }

    bool readFrame(int fd, uint64_t & reqId, std::string & method, std::string & payload)
    {
        // Length-delimited: a LEB128 varint byte count, then the frame body.
        uint64_t len = 0;
        int shift = 0;
        for (;;) {
            char c;
            if (!wtReadExact(fd, &c, 1))
                return false;
            uint8_t b = static_cast<uint8_t>(c);
            len |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                break;
            shift += 7;
        }
        std::string body(len, '\0');
        if (len && !wtReadExact(fd, body.data(), len))
            return false;
        reqId = 0;
        method.clear();
        payload.clear();
        size_t p = 0;
        while (p < body.size()) {
            uint64_t tag = wtReadVarintBuf(body, p);
            uint32_t field = static_cast<uint32_t>(tag >> 3);
            uint32_t wire = static_cast<uint32_t>(tag & 0x7);
            if (wire == 0) {
                uint64_t v = wtReadVarintBuf(body, p);
                if (field == WT_F_REQUEST_ID)
                    reqId = v;
            } else if (wire == 2) {
                uint64_t n = wtReadVarintBuf(body, p);
                std::string s = body.substr(p, n);
                p += n;
                if (field == WT_F_METHOD)
                    method = std::move(s);
                else if (field == WT_F_PAYLOAD)
                    payload = std::move(s);
            } else {
                break; // no other wire types on the request path
            }
        }
        return true;
    }

    void sendResponse(int fd, uint64_t reqId, std::string_view payload)
    {
        std::string frame;
        wtPutVarintField(frame, WT_F_REQUEST_ID, reqId);
        wtPutVarintField(frame, WT_F_KIND, WT_KIND_RESPONSE);
        wtPutLenField(frame, WT_F_PAYLOAD, payload);
        std::string wire;
        wtPutVarint(wire, frame.size());
        wire += frame;
        wtWriteAll(fd, wire);
    }

    std::string path_;
    int listenFd_ = -1;
    std::thread acceptThread_;
    std::atomic<bool> stop_{false};
    std::vector<std::thread> workers_;

    std::mutex mtx_;
    std::vector<int> connFds_;
    std::vector<std::vector<uint64_t>> sessionsPerConn_;
    std::set<uint64_t> closed_;
    uint64_t nextSession_ = 1;
};

} // namespace

class WorldtreePoolTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initLibStore(false); // idempotent (guarded internally)
        initGC();            // idempotent (guarded internally)
    }

    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);
    }

    void TearDown() override
    {
        delTmpDir.reset();
    }

    /// Keeps an EvalState and the settings/store it borrows alive together.
    struct Ctx
    {
        bool readOnly = true;
        fetchers::Settings fetchSettings{};
        EvalSettings evalSettings{readOnly};
        ref<Store> store = openStore("dummy://");
        std::unique_ptr<EvalState> state;

        EvalState & wire(const std::string & socketPath, uint64_t cap)
        {
            evalSettings.nixPath = {};
            evalSettings.tectonixWorldtreeSocket = socketPath;
            evalSettings.tectonixWorldtreeWorkspace = 42;
            evalSettings.tectonixWorldtreeMaxConnections = cap;
            state = std::make_unique<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
            return *state;
        }
    };

    /// Friend-access shim: only `WorldtreePoolTest` may reach the private pool seam, so the
    /// TEST_F bodies (which run in a derived class) call this rather than the private method.
    std::shared_ptr<WorldtreeZoneSession> acquire(EvalState & s, const Hash & sha, const std::string & zone)
    {
        return s.acquireWorldtreeZoneSession(sha, zone);
    }

    std::filesystem::path tmpDir;
    std::unique_ptr<AutoDelete> delTmpDir;
};

// M > cap zones must fan out over at most `cap` physical connections (lazy growth, then
// round-robin reuse), never one connection per zone.
TEST_F(WorldtreePoolTest, bounds_connections_and_round_robins)
{
    const uint64_t cap = 4;
    const int zones = 20;

    // Order matters: `state` (in ctx) is torn down before `fake`, so client fds close and
    // the fake's workers see EOF before the daemon joins them.
    FakeWorldtreeDaemon fake((tmpDir / "w.sock").string());
    Ctx ctx;
    EvalState & state = ctx.wire(fake.path(), cap);

    Hash sha(HashAlgorithm::SHA1); // zero commit oid; the fake ignores it

    std::vector<std::shared_ptr<WorldtreeZoneSession>> handles;

    // Lazy growth: the pool must not pre-allocate. After k < cap acquisitions there are
    // exactly k connections.
    handles.push_back(acquire(state, sha, "//areas/z0"));
    EXPECT_EQ(fake.connectionCount(), 1u);
    handles.push_back(acquire(state, sha, "//areas/z1"));
    EXPECT_EQ(fake.connectionCount(), 2u);

    for (int i = 2; i < zones; ++i)
        handles.push_back(acquire(state, sha, "//areas/z" + std::to_string(i)));

    // Bounded: exactly `cap` connections for M > cap zones (the core regression guard).
    EXPECT_EQ(fake.connectionCount(), cap);

    auto perConn = fake.sessionsPerConn();
    ASSERT_EQ(perConn.size(), cap);

    // Every zone got its own session, and the ids cover 1..M with none lost or duplicated.
    std::set<uint64_t> all;
    size_t total = 0;
    for (const auto & sessions : perConn) {
        total += sessions.size();
        all.insert(sessions.begin(), sessions.end());
    }
    EXPECT_EQ(total, static_cast<size_t>(zones));
    EXPECT_EQ(all.size(), static_cast<size_t>(zones));

    // Round-robin: sessions are handed to connections cyclically, so connection i hosts
    // exactly the sessions whose (id-1) % cap == i, and each connection is used evenly.
    for (size_t i = 0; i < perConn.size(); ++i) {
        for (uint64_t session : perConn[i])
            EXPECT_EQ((session - 1) % cap, i) << "session " << session << " on connection " << i;
        EXPECT_GE(perConn[i].size(), static_cast<size_t>(zones) / cap);
    }
}

// Dropping the accessor handles must release each per-zone session (a `scoped.close_ro` on
// the hosting connection), not leak them on the pooled connections.
TEST_F(WorldtreePoolTest, closes_sessions_on_accessor_drop)
{
    const uint64_t cap = 4;
    const int zones = 20;

    FakeWorldtreeDaemon fake((tmpDir / "w.sock").string());
    Ctx ctx;
    EvalState & state = ctx.wire(fake.path(), cap);

    Hash sha(HashAlgorithm::SHA1);

    std::vector<std::shared_ptr<WorldtreeZoneSession>> handles;
    for (int i = 0; i < zones; ++i)
        handles.push_back(acquire(state, sha, "//areas/z" + std::to_string(i)));

    // Nothing is released while the handles are still held.
    EXPECT_TRUE(fake.closedSessions().empty());

    handles.clear(); // drop every accessor session

    // Every opened session was closed exactly once (ids 1..M).
    std::set<uint64_t> expected;
    for (int i = 1; i <= zones; ++i)
        expected.insert(static_cast<uint64_t>(i));
    EXPECT_EQ(fake.closedSessions(), expected);
}

} // namespace nix
