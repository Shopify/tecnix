#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ============================================================================
// The control-connection keepalive.
//
// worldtreed reaps a control-plane connection that has been idle for its
// `ServeConfig::idle_timeout` (300 s for both the host socket and every scoped
// per-workspace listener). A wide evaluation can easily leave the control connection
// untouched for longer than that -- source bytes come from the FUSE projection, not the
// socket -- and the client only learns at its next request, which then dies with
// `worldtree: daemon closed the connection` / EPIPE mid-eval.
//
// So the client heartbeats: while a connection is alive it must see traffic well inside
// the daemon's idle window. These tests drive the real `EvalState` control seam against a
// minimal in-process fake worldtreed speaking the real wire, so nothing on the production
// path is stubbed: they assert the heartbeat happens on the *same* connection while the
// evaluator is idle, and that it can be turned off.
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

/// Length-delimited field, always emitted (repeated elements / embedded messages).
void wtPutLenFieldAlways(std::string & out, uint32_t field, std::string_view bytes)
{
    wtPutVarint(out, (static_cast<uint64_t>(field) << 3) | 2);
    wtPutVarint(out, bytes.size());
    out.append(bytes);
}

/// Singular length-delimited field, omitted when empty (proto3 default).
void wtPutLenField(std::string & out, uint32_t field, std::string_view bytes)
{
    if (bytes.empty())
        return;
    wtPutLenFieldAlways(out, field, bytes);
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

/// A minimal in-process fake worldtree daemon: it records every method it is asked for
/// (per accepted connection, in arrival order), answers `tecnix.zone_tree_shas` with a
/// fixed 20-byte oid so the accessor's real read path completes, and replies to anything
/// else -- the heartbeat included -- with a benign empty RESPONSE. It deliberately does
/// *not* reap idle connections; these tests assert what the client sends, and the reaping
/// half is worldtreed's own (tested) behaviour.
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
        return methodsPerConn_.size();
    }

    /// How many times `method` was requested across all connections.
    size_t methodCount(const std::string & method)
    {
        std::lock_guard<std::mutex> l(mtx_);
        return counts_.count(method) ? counts_.at(method) : 0;
    }

    /// Per-accepted-connection, the methods requested on it, in arrival order.
    std::vector<std::vector<std::string>> methodsPerConn()
    {
        std::lock_guard<std::mutex> l(mtx_);
        return methodsPerConn_;
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
                idx = methodsPerConn_.size();
                methodsPerConn_.emplace_back();
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
            {
                std::lock_guard<std::mutex> l(mtx_);
                methodsPerConn_[idx].push_back(method);
                counts_[method]++;
            }
            if (method == "tecnix.zone_tree_shas") {
                // ZoneTreeShasResp { entries = 1 (repeated ZoneSha { zone = 1, tree_sha = 2 }) }
                std::string entry;
                wtPutLenFieldAlways(entry, 1, "//areas/z0");
                wtPutLenFieldAlways(entry, 2, std::string(20, '\x11'));
                std::string pl;
                wtPutLenFieldAlways(pl, 1, entry);
                sendResponse(fd, reqId, pl);
            } else {
                sendResponse(fd, reqId, {});
            }
        }
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
    std::vector<std::vector<std::string>> methodsPerConn_;
    std::map<std::string, size_t> counts_;
};

/// Poll `pred` until it holds or `limit` elapses (so a passing run is fast and a failing
/// one still terminates). Returns the final verdict.
bool waitFor(const std::function<bool()> & pred, std::chrono::milliseconds limit)
{
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

class WorldtreeKeepaliveTest : public ::testing::Test
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

        EvalState & wire(const std::string & socketPath, const std::string & checkout, uint64_t keepaliveMs)
        {
            evalSettings.nixPath = {};
            evalSettings.tectonixCheckoutPath = checkout; // the mutable-checkout regime
            evalSettings.tectonixWorldtreeSocket = socketPath;
            evalSettings.tectonixWorldtreeWorkspace = 42;
            evalSettings.tectonixWorldtreeKeepaliveIntervalMs = keepaliveMs;
            state = std::make_unique<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
            return *state;
        }
    };

    std::filesystem::path tmpDir;
    std::unique_ptr<AutoDelete> delTmpDir;
};

// While the evaluator is busy elsewhere (reading source bytes from the FUSE projection, or
// just evaluating), the control connection must keep seeing traffic -- on the *same*
// connection, without opening new ones -- or worldtreed's idle reaper closes it under us.
TEST_F(WorldtreeKeepaliveTest, heartbeats_the_idle_control_connection)
{
    // Order matters: `state` (in ctx) is torn down before `fake`, so the client fd closes
    // and the fake's worker sees EOF before the daemon joins it.
    FakeWorldtreeDaemon fake((tmpDir / "w.sock").string());
    Ctx ctx;
    EvalState & state = ctx.wire(fake.path(), tmpDir.string(), /*keepaliveMs=*/25);

    // One real request establishes the connection; nothing else touches it afterwards.
    EXPECT_NO_THROW(getWorldTreeSha(state, "//areas/z0"));
    ASSERT_EQ(fake.connectionCount(), 1u);

    // Several heartbeats must land while the evaluator is idle.
    EXPECT_TRUE(waitFor([&] { return fake.methodCount("scoped.resolve_ref") >= 3; }, std::chrono::seconds(5)))
        << "no keepalive traffic on an idle control connection";

    // The heartbeat rides the existing connection: it must not dial a new one (which
    // would leave the original just as reapable).
    EXPECT_EQ(fake.connectionCount(), 1u);
    auto perConn = fake.methodsPerConn();
    ASSERT_EQ(perConn.size(), 1u);
    EXPECT_EQ(perConn[0].front(), "tecnix.zone_tree_shas");
}

// A zero interval turns the heartbeat off (the escape hatch for a daemon with the reaper
// disabled, or for debugging): the connection then sees nothing but real requests.
TEST_F(WorldtreeKeepaliveTest, keepalive_can_be_disabled)
{
    FakeWorldtreeDaemon fake((tmpDir / "w.sock").string());
    Ctx ctx;
    EvalState & state = ctx.wire(fake.path(), tmpDir.string(), /*keepaliveMs=*/0);

    EXPECT_NO_THROW(getWorldTreeSha(state, "//areas/z0"));
    ASSERT_EQ(fake.connectionCount(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_EQ(fake.methodCount("scoped.resolve_ref"), 0u);
    EXPECT_EQ(fake.methodCount("tecnix.zone_tree_shas"), 1u);
}

} // namespace nix
