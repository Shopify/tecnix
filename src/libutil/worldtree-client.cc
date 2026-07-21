///@file
/// Implementation of the self-contained worldtree control-socket client. See
/// `worldtree-client.hh` for why this depends only on the standard library + POSIX.
///
/// # The wire, in full
/// Every unit on a connection is a `Frame` protobuf message, written **length-delimited**
/// (a LEB128 varint byte-count prefix, then the encoded message — the daemon's
/// `wt-controlplane/src/rpc/codec.rs`). A `Frame` carries `request_id` (1), `kind` (2:
/// REQUEST=1/RESPONSE=2/ERROR=3/CANCEL=4/CREDIT=5), `method` (3), `payload` (4),
/// `credit` (5), `error_code` (6), `error_msg` (7). The opaque `payload` is itself a
/// protobuf message — one of the Tecnix verb request/response types.
///
/// # Flow control
/// On connect the daemon sends exactly one CREDIT frame (its in-flight window) and never
/// another; the client self-accounts thereafter. Issuing one request at a time and
/// draining to its reply keeps us trivially within that window, so we never send CREDIT
/// and simply skip the daemon's opening CREDIT frame while waiting for our response.
///
/// # protobuf subset
/// We encode/decode only what these messages use: varint scalars (wire type 0) and
/// length-delimited strings/bytes/embedded-messages (wire type 2). Encoding follows
/// proto3 canonical form — singular scalar/byte fields equal to their default are
/// omitted, repeated fields emit one entry per element — so our bytes are identical to
/// the daemon's prost output (the golden-bytes test pins this). Decoding tolerates any
/// field order and skips unknown fields (forward compatibility), so a newer daemon that
/// adds fields stays readable.

#include "nix/util/worldtree-client.hh"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

namespace nix::worldtree {

namespace {

// ---- protobuf wire constants ------------------------------------------------

constexpr uint32_t WIRE_VARINT = 0;
constexpr uint32_t WIRE_I64 = 1;
constexpr uint32_t WIRE_LEN = 2;
constexpr uint32_t WIRE_I32 = 5;

// Frame field numbers (must match wt_proto::Frame in frame.rs).
constexpr uint32_t F_REQUEST_ID = 1;
constexpr uint32_t F_KIND = 2;
constexpr uint32_t F_METHOD = 3;
constexpr uint32_t F_PAYLOAD = 4;
constexpr uint32_t F_ERROR_CODE = 6;
constexpr uint32_t F_ERROR_MSG = 7;

// Frame::kind values (must match wt_proto::FrameKind in frame.rs).
constexpr uint64_t KIND_REQUEST = 1;
constexpr uint64_t KIND_RESPONSE = 2;
constexpr uint64_t KIND_ERROR = 3;
// CANCEL/CREDIT complete the wire enum (must match frame.rs) but the client neither sends
// CANCEL nor tracks CREDIT explicitly — it issues one request at a time and skips the
// daemon's opening CREDIT by request-id mismatch. Marked maybe_unused so a -Werror build
// keeps the documentation without complaint.
[[maybe_unused]] constexpr uint64_t KIND_CANCEL = 4;
[[maybe_unused]] constexpr uint64_t KIND_CREDIT = 5;
constexpr uint64_t KIND_STREAM_END = 6;

// The transport's hard per-frame ceiling (the daemon's `MAX_FRAME`,
// `wt-controlplane/src/rpc/codec.rs`): 256 MiB. A length prefix larger than this is a
// framing violation (or a hostile/corrupt peer) — reject it *before* allocating, so a
// bogus varint can never drive an unbounded `readN`.
constexpr uint64_t MAX_FRAME = 256ull * 1024 * 1024;

// A liveness backstop on blocking reads: no single `read()` may stall longer than this
// (a wedged daemon must surface as an error, not an indefinite hang). It bounds one
// syscall, not a whole streamed response, so a large multi-chunk read still completes as
// long as bytes keep arriving.
constexpr int RECV_TIMEOUT_SECS = 120;

// ---- protobuf encode (append to a buffer) -----------------------------------

void putVarint(std::string & out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

void putTag(std::string & out, uint32_t field, uint32_t wire)
{
    putVarint(out, (static_cast<uint64_t>(field) << 3) | wire);
}

/// A singular varint field — omitted when zero (proto3 default).
void putVarintField(std::string & out, uint32_t field, uint64_t v)
{
    if (v == 0)
        return;
    putTag(out, field, WIRE_VARINT);
    putVarint(out, v);
}

/// A length-delimited field carrying `bytes` — always emitted (used for repeated
/// elements and for embedded messages, where a present-but-empty value is meaningful).
void putLenFieldAlways(std::string & out, uint32_t field, std::string_view bytes)
{
    putTag(out, field, WIRE_LEN);
    putVarint(out, bytes.size());
    out.append(bytes);
}

/// A singular length-delimited field — omitted when empty (proto3 default).
void putLenField(std::string & out, uint32_t field, std::string_view bytes)
{
    if (bytes.empty())
        return;
    putLenFieldAlways(out, field, bytes);
}

// ---- protobuf decode --------------------------------------------------------

/// A cursor over an encoded message. Bounds-checked: every read past the end throws
/// [`ProtocolError`] rather than reading out of bounds.
struct Reader
{
    const char * p;
    const char * end;

    explicit Reader(std::string_view s)
        : p(s.data())
        , end(s.data() + s.size())
    {
    }

    bool atEnd() const
    {
        return p >= end;
    }

    uint64_t varint()
    {
        uint64_t v = 0;
        int shift = 0;
        for (;;) {
            if (p >= end)
                throw ProtocolError("worldtree: truncated varint in reply");
            uint8_t b = static_cast<uint8_t>(*p++);
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
            shift += 7;
            if (shift >= 64)
                throw ProtocolError("worldtree: varint overflow in reply");
        }
    }

    std::string_view bytes()
    {
        uint64_t len = varint();
        if (static_cast<uint64_t>(end - p) < len)
            throw ProtocolError("worldtree: truncated length-delimited field in reply");
        std::string_view s(p, len);
        p += len;
        return s;
    }

    /// Skip a field of the given wire type whose tag we have already read (used for
    /// unknown fields, so a newer daemon stays decodable).
    void skip(uint32_t wire)
    {
        switch (wire) {
        case WIRE_VARINT:
            varint();
            break;
        case WIRE_LEN:
            bytes();
            break;
        case WIRE_I64:
            if (end - p < 8)
                throw ProtocolError("worldtree: truncated 64-bit field in reply");
            p += 8;
            break;
        case WIRE_I32:
            if (end - p < 4)
                throw ProtocolError("worldtree: truncated 32-bit field in reply");
            p += 4;
            break;
        default:
            throw ProtocolError("worldtree: unknown protobuf wire type in reply");
        }
    }
};

/// Convert wire bytes to an [`Oid`]: exactly 20 bytes, or `nullopt` for the empty
/// "absent" sentinel the daemon uses. Any other length is a protocol violation.
std::optional<Oid> toOid(std::string_view b)
{
    if (b.empty())
        return std::nullopt;
    if (b.size() != 20)
        throw ProtocolError("worldtree: object id is not 20 bytes");
    Oid oid;
    std::memcpy(oid.data(), b.data(), 20);
    return oid;
}

} // namespace

RpcError::RpcError(ErrorCode code, const std::string & msg)
    : std::runtime_error(msg)
    , code(code)
{
}

// ---- connection lifecycle ---------------------------------------------------

Client::Client(int fd)
    : fd_(fd)
{
}

Client::Client(Client && other) noexcept
    : fd_(other.fd_)
    , nextId_(other.nextId_)
    , rbuf_(std::move(other.rbuf_))
    , rpos_(other.rpos_)
{
    other.fd_ = -1;
}

Client & Client::operator=(Client && other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        nextId_ = other.nextId_;
        rbuf_ = std::move(other.rbuf_);
        rpos_ = other.rpos_;
        other.fd_ = -1;
    }
    return *this;
}

Client::~Client()
{
    if (fd_ >= 0)
        ::close(fd_);
}

Client Client::connect(const std::string & socketPath)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path))
        throw ProtocolError("worldtree: socket path too long: " + socketPath);
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size());

    // Close-on-exec atomically where the platform supports it (Linux `SOCK_CLOEXEC`), so a
    // daemon connection can never leak into a Tectonix build child through a racing fork.
    // macOS lacks `SOCK_CLOEXEC`; the `fcntl` below is the portable fallback there.
    int sockType = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
    sockType |= SOCK_CLOEXEC;
#endif
    int fd = ::socket(AF_UNIX, sockType, 0);
    if (fd < 0)
        throw ProtocolError(std::string("worldtree: socket(): ") + std::strerror(errno));

    // Fallback close-on-exec for platforms without SOCK_CLOEXEC (and harmless where the
    // flag already took): a daemon connection must not leak into Tectonix build children.
    int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0)
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

    // A receive timeout so a wedged daemon surfaces as a clean transport error rather than
    // an indefinite hang inside a build. Best-effort: a kernel that rejects the option
    // just leaves reads blocking, the prior behavior.
    struct timeval tv{};
    tv.tv_sec = RECV_TIMEOUT_SECS;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        throw ProtocolError("worldtree: connect(" + socketPath + "): " + std::strerror(e));
    }
    return Client(fd);
}

// ---- socket I/O -------------------------------------------------------------

void Client::writeAll(const std::string & bytes)
{
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = ::write(fd_, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw ProtocolError(std::string("worldtree: write(): ") + std::strerror(errno));
        }
        off += static_cast<size_t>(n);
    }
}

void Client::refill()
{
    // Drop already-consumed prefix so rbuf_ never grows without bound across calls.
    if (rpos_ > 0) {
        rbuf_.erase(0, rpos_);
        rpos_ = 0;
    }
    char tmp[65536];
    for (;;) {
        ssize_t n = ::read(fd_, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            // SO_RCVTIMEO fired: the daemon has gone quiet mid-reply. Surface it as a
            // clear transport error rather than the opaque "Resource temporarily
            // unavailable" of a raw EAGAIN.
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                throw ProtocolError("worldtree: timed out waiting for the daemon to reply");
            throw ProtocolError(std::string("worldtree: read(): ") + std::strerror(errno));
        }
        if (n == 0)
            throw ProtocolError("worldtree: daemon closed the connection");
        rbuf_.append(tmp, static_cast<size_t>(n));
        return;
    }
}

uint8_t Client::nextByte()
{
    while (rpos_ >= rbuf_.size())
        refill();
    return static_cast<uint8_t>(rbuf_[rpos_++]);
}

uint64_t Client::readVarint()
{
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        uint8_t b = nextByte();
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
        shift += 7;
        if (shift >= 64)
            throw ProtocolError("worldtree: frame length varint overflow");
    }
}

std::string Client::readN(uint64_t n)
{
    while (rbuf_.size() - rpos_ < n)
        refill();
    std::string s = rbuf_.substr(rpos_, n);
    rpos_ += static_cast<size_t>(n);
    return s;
}

// ---- the call: write one REQUEST frame, drain to its reply ------------------

uint64_t Client::send(const std::string & method, const std::string & payload)
{
    uint64_t id = nextId_++;

    // Encode the REQUEST frame, then length-delimit it.
    std::string frame;
    putVarintField(frame, F_REQUEST_ID, id);
    putVarintField(frame, F_KIND, KIND_REQUEST);
    putLenField(frame, F_METHOD, method);
    putLenField(frame, F_PAYLOAD, payload);

    std::string wire;
    putVarint(wire, frame.size());
    wire += frame;
    writeAll(wire);
    return id;
}

Client::RecvFrame Client::recvFor(uint64_t id)
{
    // Read frames until we see one for our request id. The daemon's opening CREDIT frame
    // (request_id 0) and any other non-matching frame are skipped.
    for (;;) {
        uint64_t len = readVarint();
        if (len > MAX_FRAME)
            throw ProtocolError("worldtree: reply frame exceeds the maximum frame size");
        std::string body = readN(len);

        Reader r(body);
        RecvFrame fr;
        uint64_t frRequestId = 0;

        while (!r.atEnd()) {
            uint64_t tag = r.varint();
            uint32_t field = static_cast<uint32_t>(tag >> 3);
            uint32_t wire = static_cast<uint32_t>(tag & 0x7);
            switch (field) {
            case F_REQUEST_ID:
                if (wire != WIRE_VARINT)
                    throw ProtocolError("worldtree: bad wire type for request_id");
                frRequestId = r.varint();
                break;
            case F_KIND:
                if (wire != WIRE_VARINT)
                    throw ProtocolError("worldtree: bad wire type for kind");
                fr.kind = r.varint();
                break;
            case F_PAYLOAD:
                if (wire != WIRE_LEN)
                    throw ProtocolError("worldtree: bad wire type for payload");
                fr.payload = std::string(r.bytes());
                fr.havePayload = true;
                break;
            case F_ERROR_CODE:
                if (wire != WIRE_VARINT)
                    throw ProtocolError("worldtree: bad wire type for error_code");
                fr.errorCode = r.varint();
                break;
            case F_ERROR_MSG:
                if (wire != WIRE_LEN)
                    throw ProtocolError("worldtree: bad wire type for error_msg");
                fr.errorMsg = std::string(r.bytes());
                break;
            default:
                r.skip(wire); // method, credit, or a field a newer daemon added
                break;
            }
        }

        if (frRequestId != id)
            continue; // CREDIT (id 0) or a stray frame — not ours
        return fr;
    }
}

std::string Client::call(const std::string & method, const std::string & payload)
{
    uint64_t id = send(method, payload);
    RecvFrame fr = recvFor(id);
    if (fr.kind == KIND_RESPONSE)
        return fr.havePayload ? fr.payload : std::string();
    if (fr.kind == KIND_ERROR)
        throw RpcError(
            static_cast<ErrorCode>(fr.errorCode),
            fr.errorMsg.empty() ? "worldtree: daemon returned an error" : fr.errorMsg);
    throw ProtocolError("worldtree: unexpected frame kind in reply to a request");
}

void Client::callStream(
    const std::string & method, const std::string & payload, const std::function<void(std::string_view)> & onChunk)
{
    uint64_t id = send(method, payload);
    // A streamed reply is zero or more RESPONSE chunks (in order) terminated by a single
    // STREAM_END, or an ERROR that ends the stream. Unlike a one-shot RESPONSE, the reader
    // must consume every frame through the terminator so the connection is left clean for
    // the next call.
    for (;;) {
        RecvFrame fr = recvFor(id);
        if (fr.kind == KIND_RESPONSE) {
            if (fr.havePayload)
                onChunk(fr.payload);
            continue;
        }
        if (fr.kind == KIND_STREAM_END)
            return;
        if (fr.kind == KIND_ERROR)
            throw RpcError(
                static_cast<ErrorCode>(fr.errorCode),
                fr.errorMsg.empty() ? "worldtree: daemon returned an error" : fr.errorMsg);
        throw ProtocolError("worldtree: unexpected frame kind in a streamed reply");
    }
}

// ---- the four Tecnix read verbs ---------------------------------------------

std::vector<std::string> Client::dirtyZones(uint64_t ws)
{
    std::string req;
    putVarintField(req, 1, ws); // DirtyZonesReq { ws = 1 }

    std::string resp = call("tecnix.dirty_zones", req);

    std::vector<std::string> zones;
    Reader r(resp);
    while (!r.atEnd()) {
        uint64_t tag = r.varint();
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        if (field == 1 && wire == WIRE_LEN) // DirtyZonesResp { zones = 1 (repeated string) }
            zones.emplace_back(r.bytes());
        else
            r.skip(wire);
    }
    return zones;
}

std::vector<ZoneDirty> Client::dirtyZoneEntries(uint64_t ws)
{
    std::string req;
    putVarintField(req, 1, ws); // DirtyZonesReq { ws = 1 }

    std::string resp = call("tecnix.dirty_zones", req);

    std::vector<ZoneDirty> out;
    Reader r(resp);
    while (!r.atEnd()) {
        uint64_t tag = r.varint();
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        if (field == 2 && wire == WIRE_LEN) { // DirtyZonesResp { entries = 2 (repeated ZoneDirty) }
            Reader e(r.bytes());
            ZoneDirty entry;
            while (!e.atEnd()) {
                uint64_t etag = e.varint();
                uint32_t efield = static_cast<uint32_t>(etag >> 3);
                uint32_t ewire = static_cast<uint32_t>(etag & 0x7);
                if (efield == 1 && ewire == WIRE_LEN) // ZoneDirty { zone = 1 (string) }
                    entry.zone = std::string(e.bytes());
                else if (efield == 2 && ewire == WIRE_LEN) // ZoneDirty { files = 2 (repeated string) }
                    entry.files.emplace_back(e.bytes());
                else
                    e.skip(ewire);
            }
            out.push_back(std::move(entry));
        } else
            r.skip(wire); // field 1 (flat `zones`) is the projection of `entries` — skip it here
    }
    return out;
}

std::vector<ZoneSha> Client::zoneTreeShas(uint64_t ws, const std::vector<std::string> & zones)
{
    std::string req;
    putVarintField(req, 1, ws); // ZoneTreeShasReq { ws = 1, zones = 2 (repeated string) }
    for (const auto & z : zones)
        putLenFieldAlways(req, 2, z);

    std::string resp = call("tecnix.zone_tree_shas", req);

    std::vector<ZoneSha> out;
    Reader r(resp);
    while (!r.atEnd()) {
        uint64_t tag = r.varint();
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        if (field == 1 && wire == WIRE_LEN) { // ZoneTreeShasResp { entries = 1 (repeated ZoneSha) }
            Reader e(r.bytes());
            ZoneSha entry;
            while (!e.atEnd()) {
                uint64_t etag = e.varint();
                uint32_t efield = static_cast<uint32_t>(etag >> 3);
                uint32_t ewire = static_cast<uint32_t>(etag & 0x7);
                if (efield == 1 && ewire == WIRE_LEN) // ZoneSha { zone = 1 (string) }
                    entry.zone = std::string(e.bytes());
                else if (efield == 2 && ewire == WIRE_LEN) // ZoneSha { tree_sha = 2 (bytes) }
                    entry.treeSha = toOid(e.bytes());
                else
                    e.skip(ewire);
            }
            out.push_back(std::move(entry));
        } else
            r.skip(wire);
    }
    return out;
}

std::vector<TreeEntry> Client::readTree(uint64_t ws, const std::string & path, uint32_t depth)
{
    std::string req;
    putVarintField(req, 1, ws); // ReadTreeReq { ws = 1, path = 2 (bytes), depth = 3 }
    putLenField(req, 2, path);
    putVarintField(req, 3, depth);

    // The daemon streams the skeleton as one or more `ReadTreeResp` chunks (each a subset of
    // the entries) terminated by STREAM_END; a small tree arrives as a single chunk. Every
    // chunk parses identically — accumulate all of their entries into one vector.
    std::vector<TreeEntry> out;
    callStream("tecnix.read_tree", req, [&out](std::string_view chunk) {
        Reader r(chunk);
        while (!r.atEnd()) {
            uint64_t tag = r.varint();
            uint32_t field = static_cast<uint32_t>(tag >> 3);
            uint32_t wire = static_cast<uint32_t>(tag & 0x7);
            if (field == 1 && wire == WIRE_LEN) { // ReadTreeResp { entries = 1 (repeated TreeEntry) }
                Reader e(r.bytes());
                TreeEntry entry{};
                std::optional<Oid> oid;
                while (!e.atEnd()) {
                    uint64_t etag = e.varint();
                    uint32_t efield = static_cast<uint32_t>(etag >> 3);
                    uint32_t ewire = static_cast<uint32_t>(etag & 0x7);
                    switch (efield) {
                    case 1: // path (bytes)
                        if (ewire != WIRE_LEN)
                            throw ProtocolError("worldtree: bad wire type for tree-entry path");
                        entry.path = std::string(e.bytes());
                        break;
                    case 2: // mode (uint32)
                        if (ewire != WIRE_VARINT)
                            throw ProtocolError("worldtree: bad wire type for tree-entry mode");
                        entry.mode = static_cast<uint32_t>(e.varint());
                        break;
                    case 3: // oid (bytes)
                        if (ewire != WIRE_LEN)
                            throw ProtocolError("worldtree: bad wire type for tree-entry oid");
                        oid = toOid(e.bytes());
                        break;
                    case 4: // size (uint64)
                        if (ewire != WIRE_VARINT)
                            throw ProtocolError("worldtree: bad wire type for tree-entry size");
                        entry.size = e.varint();
                        break;
                    default:
                        e.skip(ewire);
                        break;
                    }
                }
                // A tree node always carries a 20-byte oid; an empty/absent one is malformed.
                if (!oid)
                    throw ProtocolError("worldtree: tree entry missing its object id");
                entry.oid = *oid;
                out.push_back(std::move(entry));
            } else
                r.skip(wire);
        }
    });
    return out;
}

std::vector<Blob> Client::readBlobs(uint64_t ws, const std::vector<Oid> & oids)
{
    std::string req;
    putVarintField(req, 1, ws); // ReadBlobsReq { ws = 1, oids = 2 (repeated bytes) }
    for (const auto & oid : oids)
        putLenFieldAlways(req, 2, std::string_view(reinterpret_cast<const char *>(oid.data()), oid.size()));

    // Streamed as one or more `ReadBlobsResp` chunks (each a subset of the requested blobs)
    // terminated by STREAM_END. Accumulate every chunk's blobs, preserving request order
    // (the daemon emits them in the order asked).
    std::vector<Blob> out;
    callStream("tecnix.read_blobs", req, [&out](std::string_view chunk) {
        Reader r(chunk);
        while (!r.atEnd()) {
            uint64_t tag = r.varint();
            uint32_t field = static_cast<uint32_t>(tag >> 3);
            uint32_t wire = static_cast<uint32_t>(tag & 0x7);
            if (field == 1 && wire == WIRE_LEN) { // ReadBlobsResp { blobs = 1 (repeated Blob) }
                Reader e(r.bytes());
                std::optional<Oid> oid;
                std::string content;
                bool present = false;
                while (!e.atEnd()) {
                    uint64_t etag = e.varint();
                    uint32_t efield = static_cast<uint32_t>(etag >> 3);
                    uint32_t ewire = static_cast<uint32_t>(etag & 0x7);
                    switch (efield) {
                    case 1: // oid (bytes)
                        if (ewire != WIRE_LEN)
                            throw ProtocolError("worldtree: bad wire type for blob oid");
                        oid = toOid(e.bytes());
                        break;
                    case 2: // content (bytes)
                        if (ewire != WIRE_LEN)
                            throw ProtocolError("worldtree: bad wire type for blob content");
                        content = std::string(e.bytes());
                        break;
                    case 3: // present (bool)
                        if (ewire != WIRE_VARINT)
                            throw ProtocolError("worldtree: bad wire type for blob present");
                        present = e.varint() != 0;
                        break;
                    default:
                        e.skip(ewire);
                        break;
                    }
                }
                Blob blob;
                blob.oid = oid.value_or(Oid{});
                if (present)
                    blob.content = std::move(content);
                out.push_back(std::move(blob));
            } else
                r.skip(wire);
        }
    });
    return out;
}

// ---- session lifecycle: scoped.open_ro / scoped.close_ro --------------------

RoSession Client::openRo(
    const Oid & baseSha,
    const std::string & visibilityMode,
    const std::vector<std::string> & visibleZoneIds,
    const std::vector<std::string> & visibleZonePaths)
{
    std::string req;
    // OpenRoReq { base_sha = 1 (bytes), visibility_mode = 2 (string),
    //             visible_zone_ids = 3 (repeated string), visible_zone_paths = 4 (repeated string) }
    putLenFieldAlways(req, 1, std::string_view(reinterpret_cast<const char *>(baseSha.data()), baseSha.size()));
    putLenField(req, 2, visibilityMode);
    for (const auto & z : visibleZoneIds)
        putLenFieldAlways(req, 3, z);
    for (const auto & z : visibleZonePaths)
        putLenFieldAlways(req, 4, z);

    std::string resp = call("scoped.open_ro", req);

    RoSession out{};
    Reader r(resp);
    while (!r.atEnd()) {
        uint64_t tag = r.varint();
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        switch (field) {
        case 1: // ws (uint64)
            if (wire != WIRE_VARINT)
                throw ProtocolError("worldtree: bad wire type for open_ro ws");
            out.ws = r.varint();
            break;
        case 2: // manifest (bytes)
            if (wire != WIRE_LEN)
                throw ProtocolError("worldtree: bad wire type for open_ro manifest");
            out.manifest = std::string(r.bytes());
            break;
        case 3: // base_pin (uint64)
            if (wire != WIRE_VARINT)
                throw ProtocolError("worldtree: bad wire type for open_ro base_pin");
            out.basePin = r.varint();
            break;
        default:
            r.skip(wire);
            break;
        }
    }
    return out;
}

void Client::closeRo(uint64_t ws)
{
    std::string req;
    putVarintField(req, 1, ws);          // CloseRoReq { ws = 1 }
    (void) call("scoped.close_ro", req); // CloseRoResp is empty; idempotent, ownership-gated.
}

} // namespace nix::worldtree
