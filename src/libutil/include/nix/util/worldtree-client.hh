#pragma once
///@file
/// A self-contained client for the worldtree daemon's control socket (worldtree design
/// §5.1/§5.1a). It speaks the daemon's length-delimited protobuf `Frame` envelope over
/// `AF_UNIX` and exposes the four **Tecnix read verbs** — `dirty_zones`,
/// `zone_tree_shas`, `read_tree`, `read_blobs` — that replace Tectonix's git-binary /
/// libgit2 coupling with O(changes) daemon RPCs.
///
/// It deliberately depends only on the C++ standard library and POSIX sockets — **no
/// Nix or libgit2 headers** — for two reasons. First, it must keep working when the
/// rest of the build can't: the daemon read path is the load-bearing seam and is
/// unit-tested by compiling this translation unit on its own. Second, the daemon's wire
/// is a tiny, stable, fully-specified protobuf subset, so a hand-rolled codec is both
/// smaller and freer of dependencies than pulling in protobuf-c. The `EvalState`
/// bridge (which maps these results onto `nix::Hash` / `nix::Error`) lives in libexpr,
/// not here.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nix::worldtree {

/// A raw git object id: 20 SHA-1 bytes, exactly as the daemon puts them on the wire.
using Oid = std::array<uint8_t, 20>;

/// The daemon's typed failure code — a 1:1 image of `wt_proto::ErrorCode` (the daemon's
/// `frame.rs`), so a caller can branch on the *kind* of failure (e.g. `NotFound` for an
/// absent workspace) rather than parsing a string. Values are wire-stable.
enum class ErrorCode : int {
    Unspecified = 0,
    NotFound = 1,
    Corrupt = 2,
    InvalidArg = 3,
    AlreadyExists = 4,
    NotADirectory = 5,
    IsADirectory = 6,
    NotEmpty = 7,
    Stale = 8,
    Refused = 9,
    Conflict = 10,
    Denied = 11,
    Unsupported = 12,
    Io = 13,
    Internal = 14,
    /// The requested base commit is not covered by the daemon's current generation (not
    /// yet replicated on this host). **Retryable**: the caller should surface an
    /// actionable "retry once the SHA replicates" message, not a hard failure.
    BaseCommitUnreachable = 15,
    /// A single response chunk would exceed the transport's frame ceiling
    /// (`MAX_FRAME`). **Terminal** (not retryable): the object is too large to serve in
    /// this protocol version. Distinct from a transport-level framing overflow, which is
    /// a [`ProtocolError`].
    ResponseTooLarge = 16,
};

/// A typed `ERROR` reply from the daemon: it processed the request and refused it. The
/// `code` is the daemon's verdict; `what()` is its human detail.
struct RpcError : std::runtime_error
{
    ErrorCode code;
    RpcError(ErrorCode code, const std::string & msg);
};

/// A transport- or framing-level failure: the connection dropped, a frame was
/// truncated, or a reply was malformed. Distinct from [`RpcError`], which is a
/// well-formed refusal.
struct ProtocolError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// One `zone_tree_shas` result: a zone's working-tree subtree oid. `treeSha` is
/// `nullopt` when the zone is absent or hidden by scope (the daemon sends empty bytes),
/// keeping the response one-to-one with the request.
struct ZoneSha
{
    std::string zone;
    std::optional<Oid> treeSha;
};

/// One `read_tree` node of a prefetched subtree skeleton: a path relative to the
/// requested root, its git `mode` (the object type is mode-derived), its `oid`, and —
/// for a blob or symlink — its `size` (0 for a directory), all from object headers
/// with no content fetch.
struct TreeEntry
{
    std::string path;
    uint32_t mode;
    Oid oid;
    uint64_t size;
};

/// One `read_blobs` result. `content` is `nullopt` when the object was absent (or
/// present but not a blob — the daemon reports `present=false` rather than failing the
/// batch).
struct Blob
{
    Oid oid;
    std::optional<std::string> content;
};

/// One `dirty_zones` detail entry: a dirty zone's World path plus its changed working-copy
/// files (staged ∪ unstaged), each **relative to the workspace root** (the daemon's
/// `StatusEntry.path` form). The daemon computes these while mapping files to zones, so they
/// ride along for free — letting Tectonix reconstruct a full per-file `ZoneDirtyInfo` without
/// its own `git status` walk.
struct ZoneDirty
{
    std::string zone;
    std::vector<std::string> files;
};

/// The result of `scoped.open_ro`: an **ephemeral read-only session** pinned at a base
/// commit (worldtree design §5.1a/§6.1). `ws` *is* the session id — every subsequent read
/// verb ([`Client::readTree`], [`Client::readBlobs`], [`Client::zoneTreeShas`],
/// [`Client::dirtyZones`]) addresses it, so no verb grows a `session_id`. `manifest` is the
/// raw `.meta/manifest.json` bytes at the pinned SHA, delivered in-band (no libgit2, no path
/// traversal). `basePin` is the generation the session refcounts (observability only).
struct RoSession
{
    uint64_t ws;
    std::string manifest;
    uint64_t basePin;
};

/// A synchronous, one-call-at-a-time client over a single daemon connection. Not
/// thread-safe: a connection multiplexes by request id, but this client issues one
/// request and drains to its reply before the next, which is all Tectonix's
/// memoized read methods need. Move-only (it owns the socket fd).
class Client
{
public:
    /// Connect to the daemon's control socket at `socketPath`. Throws [`ProtocolError`]
    /// if the socket cannot be reached.
    static Client connect(const std::string & socketPath);

    Client(Client && other) noexcept;
    Client & operator=(Client && other) noexcept;
    Client(const Client &) = delete;
    Client & operator=(const Client &) = delete;
    ~Client();

    /// `tecnix.dirty_zones` — the World paths (`//areas/...`) of zones with tracked
    /// working-copy changes (the flat projection; see [`dirtyZoneEntries`] for per-file detail).
    std::vector<std::string> dirtyZones(uint64_t ws);

    /// `tecnix.dirty_zones` — the dirty set with each zone's changed files. Empty from an
    /// older daemon (which sends only the flat `zones`); the caller then falls back to
    /// [`dirtyZones`] (dirty flag only, no per-file detail).
    std::vector<ZoneDirty> dirtyZoneEntries(uint64_t ws);

    /// `tecnix.zone_tree_shas` — the working-tree subtree oid of each requested zone, in
    /// request order. An empty `zones` resolves the dirty set (the common call). A clean
    /// zone yields its committed oid, a dirty zone the synthesized frontier oid.
    std::vector<ZoneSha> zoneTreeShas(uint64_t ws, const std::vector<std::string> & zones);

    /// `tecnix.read_tree` — the committed subtree skeleton at workspace-relative `path`
    /// (empty ⇒ the root tree), descending `depth` directory levels below the immediate
    /// children. Throws [`RpcError`] with `NotFound` for an absent or hidden path.
    std::vector<TreeEntry> readTree(uint64_t ws, const std::string & path, uint32_t depth);

    /// `tecnix.read_blobs` — the decoded bytes of each blob oid, in request order.
    std::vector<Blob> readBlobs(uint64_t ws, const std::vector<Oid> & oids);

    /// `scoped.open_ro` — open an **ephemeral read-only session** pinned at `baseSha`
    /// (20 raw commit-oid bytes), optionally confined to a zone cone. `visibilityMode` is
    /// `""`/`"full"` for full visibility (the zone lists must then be empty) or `"scoped"`
    /// with `visibleZoneIds`/`visibleZonePaths` naming the cone. Returns the session `ws`
    /// + the in-band manifest + the pinned generation. Throws [`RpcError`] with
    /// `BaseCommitUnreachable` (retryable) if the SHA is not covered by the current
    /// generation, or `Denied` if the connection may not open sessions.
    RoSession openRo(
        const Oid & baseSha,
        const std::string & visibilityMode,
        const std::vector<std::string> & visibleZoneIds,
        const std::vector<std::string> & visibleZonePaths);

    /// `scoped.close_ro` — release a session opened on **this** connection. Idempotent and
    /// ownership-gated: closing an id this connection never owned is a clean no-op.
    void closeRo(uint64_t ws);

private:
    explicit Client(int fd);

    /// A single decoded reply frame addressed to our request (transport detail).
    struct RecvFrame
    {
        uint64_t kind = 0;
        uint64_t errorCode = 0;
        std::string payload;
        std::string errorMsg;
        bool havePayload = false;
    };

    /// Encode + write one REQUEST frame, returning its request id.
    uint64_t send(const std::string & method, const std::string & payload);

    /// Read frames until one is addressed to `id` (skipping the daemon's opening CREDIT
    /// frame and any stray frame), returning it. Enforces the [`MAX_FRAME`] length cap.
    RecvFrame recvFor(uint64_t id);

    /// A **one-shot** call: issue `method`, return the single RESPONSE payload (throwing on
    /// an ERROR reply / transport failure). For verbs the daemon does not stream.
    std::string call(const std::string & method, const std::string & payload);

    /// A **streaming** call: issue `method`, then invoke `onChunk` for each RESPONSE chunk
    /// in order, returning once STREAM_END arrives (throwing on an ERROR reply). Used by
    /// the read verbs, which the daemon serves as a chunked stream (design §6.6).
    void callStream(
        const std::string & method, const std::string & payload, const std::function<void(std::string_view)> & onChunk);

    void writeAll(const std::string & bytes);

    // Buffered frame reader (handles the server's initial CREDIT frame and any
    // coalesced reads transparently).
    void refill();
    uint8_t nextByte();
    uint64_t readVarint();
    std::string readN(uint64_t n);

    int fd_ = -1;
    uint64_t nextId_ = 1;
    std::string rbuf_; ///< bytes read from the socket but not yet consumed
    size_t rpos_ = 0;  ///< parse cursor into rbuf_
};

} // namespace nix::worldtree
