#include <gtest/gtest.h>

#include "nix/util/worldtree-client.hh"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// The worldtree daemon closes a control connection that has had no request for 300 s.
// These tests run the client against a fake daemon on an AF_UNIX socket that closes a
// connection after a set number of replies, and check that the client connects again
// and sends the request once more, and only once.

namespace nix::worldtree {

namespace {

constexpr uint64_t KIND_RESPONSE = 2;
constexpr uint64_t KIND_ERROR = 3;
constexpr uint64_t KIND_CREDIT = 5;

void putVarint(std::string & out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

void putVarintField(std::string & out, uint32_t field, uint64_t v)
{
    putVarint(out, (static_cast<uint64_t>(field) << 3) | 0);
    putVarint(out, v);
}

void putLenField(std::string & out, uint32_t field, std::string_view bytes)
{
    putVarint(out, (static_cast<uint64_t>(field) << 3) | 2);
    putVarint(out, bytes.size());
    out.append(bytes);
}

bool readByte(int fd, uint8_t & b)
{
    for (;;) {
        ssize_t n = ::read(fd, &b, 1);
        if (n == 1)
            return true;
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
}

/// Read one length-delimited REQUEST frame and return its request id, or 0 when the
/// peer has gone.
uint64_t readRequestId(int fd)
{
    uint64_t len = 0;
    for (int shift = 0;; shift += 7) {
        uint8_t b;
        if (!readByte(fd, b))
            return 0;
        len |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
    }
    std::string body(len, '\0');
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::read(fd, body.data() + off, len - off);
        if (n <= 0)
            return 0;
        off += static_cast<size_t>(n);
    }
    // request_id is field 1, a varint; the client writes it first.
    if (body.empty() || static_cast<uint8_t>(body[0]) != ((1 << 3) | 0))
        return 0;
    uint64_t id = 0;
    for (size_t p = 1, shift = 0; p < body.size(); ++p, shift += 7) {
        uint8_t b = static_cast<uint8_t>(body[p]);
        id |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
    }
    return id;
}

/// Make a write to a closed peer fail with EPIPE instead of raising SIGPIPE.
void noSigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void) fd;
#endif
}

void writeFrame(int fd, const std::string & frame)
{
    std::string wire;
    putVarint(wire, frame.size());
    wire += frame;
    size_t off = 0;
    while (off < wire.size()) {
#ifdef MSG_NOSIGNAL
        ssize_t n = ::send(fd, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
#else
        ssize_t n = ::send(fd, wire.data() + off, wire.size() - off, 0);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        off += static_cast<size_t>(n);
    }
}

/// A fake daemon. On each connection it sends the opening CREDIT frame, answers up to
/// `repliesPerConnection` requests, then closes the connection, as the real daemon does
/// when a connection is idle for too long. A reply is a `DirtyZonesResp` naming
/// `//zone`, or an ERROR frame when `replyWithError` is set.
class FakeDaemon
{
public:
    FakeDaemon(int repliesPerConnection, bool replyWithError = false)
        : repliesPerConnection(repliesPerConnection)
        , replyWithError(replyWithError)
    {
        char tmpl[] = "/tmp/wt-client-test-XXXXXX";
        if (!::mkdtemp(tmpl))
            throw std::runtime_error("mkdtemp failed");
        dir = tmpl;
        path = dir + "/ws.sock";

        listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        if (::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listenFd, 16) != 0)
            throw std::runtime_error("cannot listen on the fake daemon socket");
        thread = std::thread([this] { serve(); });
    }

    ~FakeDaemon()
    {
        stop = true;
        ::shutdown(listenFd, SHUT_RDWR);
        // Wake a blocked accept() on platforms where shutdown() does not.
        int w = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        (void) ::connect(w, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        ::close(w);
        thread.join();
        ::close(listenFd);
        std::filesystem::remove_all(dir);
    }

    /// Block until the daemon has closed `n` connections.
    void waitClosed(int n)
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return closedConnections >= n; });
    }

    int connections()
    {
        std::lock_guard lock(mutex);
        return acceptedConnections;
    }

    std::string path;

private:
    void serve()
    {
        while (!stop) {
            int fd = ::accept(listenFd, nullptr, nullptr);
            if (fd < 0 || stop) {
                if (fd >= 0)
                    ::close(fd);
                return;
            }
            noSigpipe(fd);
            {
                std::lock_guard lock(mutex);
                ++acceptedConnections;
            }
            std::string credit;
            putVarintField(credit, 2, KIND_CREDIT);
            putVarintField(credit, 5, 64);
            writeFrame(fd, credit);
            for (int i = 0; i < repliesPerConnection; ++i) {
                uint64_t id = readRequestId(fd);
                if (id == 0)
                    break;
                std::string frame;
                putVarintField(frame, 1, id);
                if (replyWithError) {
                    putVarintField(frame, 2, KIND_ERROR);
                    putVarintField(frame, 6, 2);
                    putLenField(frame, 7, "refused");
                } else {
                    std::string payload;
                    putLenField(payload, 1, "//zone");
                    putVarintField(frame, 2, KIND_RESPONSE);
                    putLenField(frame, 4, payload);
                }
                writeFrame(fd, frame);
            }
            ::close(fd);
            {
                std::lock_guard lock(mutex);
                ++closedConnections;
            }
            cv.notify_all();
        }
    }

    int repliesPerConnection;
    bool replyWithError;
    std::string dir;
    int listenFd = -1;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    int acceptedConnections = 0;
    int closedConnections = 0;
};

} // namespace

TEST(WorldtreeClient, reconnects_when_the_daemon_closed_an_idle_connection)
{
    FakeDaemon daemon(1);
    auto client = Client::connect(daemon.path);

    EXPECT_EQ(client.dirtyZones(1), std::vector<std::string>{"//zone"});
    // The daemon has closed the first connection, so the next write fails with EPIPE.
    daemon.waitClosed(1);
    EXPECT_EQ(client.dirtyZones(1), std::vector<std::string>{"//zone"});
    EXPECT_EQ(daemon.connections(), 2);

    // The new connection stays in use.
    daemon.waitClosed(2);
    EXPECT_EQ(client.dirtyZones(1), std::vector<std::string>{"//zone"});
    EXPECT_EQ(daemon.connections(), 3);
}

TEST(WorldtreeClient, sends_a_request_again_only_once)
{
    FakeDaemon daemon(0);
    auto client = Client::connect(daemon.path);

    EXPECT_THROW(client.dirtyZones(1), ConnectionLost);
    EXPECT_EQ(daemon.connections(), 2);
}

TEST(WorldtreeClient, does_not_send_a_refused_request_again)
{
    FakeDaemon daemon(1, true);
    auto client = Client::connect(daemon.path);

    EXPECT_THROW(client.dirtyZones(1), RpcError);
    EXPECT_EQ(daemon.connections(), 1);
}

} // namespace nix::worldtree
