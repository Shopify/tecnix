/**
 * Tecnix source observation (the tracked evaluation path): the repo-wide
 * accessor composing a clean backend (libgit2 tree or worldtree FUSE
 * projection) with the dirty-checkout overlay, per-path fingerprints, and
 * repo-relative access recording. Serves only the new `builtins.tecnix*`
 * API; the legacy accessor implementations live in tecnix/source-accessors.cc
 * and stay isolated from this code.
 */

#include "eval-data.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/store-api.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/processes.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include <sys/xattr.h>

namespace nix {

static EvalState::TecnixEvalData * tecnixData(EvalState & state)
{
    return &state.tecnixEvalData();
}

/** Strip a leading `//` world prefix: `//areas/x` -> `areas/x`. */
static std::string tecnixNormalizeRepoPath(std::string_view path)
{
    std::string result(path);
    if (hasPrefix(result, "//"))
        result = result.substr(2);
    return result;
}

// Worldtree FUSE-projection helpers, deliberately duplicated from the legacy
// accessors so that file stays byte-identical to upstream wt-single-mount.

static constexpr std::string_view TECNIX_WORLDTREE_TREE_OID_XATTR = "user.worldtree.tree-oid";
static constexpr std::string_view TECNIX_WORLDTREE_BLOB_OID_XATTR = "user.worldtree.blob-oid";

static std::filesystem::path tecnixWorldtreeRevisionRoot(const EvalSettings & settings)
{
    auto revision = Hash::parseNonSRIUnprefixed(settings.tectonixGitSha.get(), HashAlgorithm::SHA1);
    return std::filesystem::path(settings.tectonixWorldtreeMount.get()) / "tecnix" / revision.gitRev();
}

static std::string tecnixRequireWorldtreeZoneId(const std::string & id)
{
    auto isLowerHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    if (id.size() != 8 || !id.starts_with("W-") || !std::ranges::all_of(std::string_view(id).substr(2), isLowerHex))
        throw Error("worldtree: invalid zone id '%s' in manifest", id);
    return id;
}

/** A git oid from one of the daemon's synthetic xattrs, or nullopt. */
static std::optional<Hash> tecnixReadWorldtreeOidXattr(const std::filesystem::path & path, std::string_view xattrName)
{
    std::array<char, 40> value{};
#ifdef __APPLE__
    auto size = ::getxattr(path.c_str(), xattrName.data(), value.data(), value.size(), 0, 0);
#else
    auto size = ::getxattr(path.c_str(), xattrName.data(), value.data(), value.size());
#endif
    if (size != static_cast<ssize_t>(value.size()))
        return std::nullopt;
    try {
        return Hash::parseNonSRIUnprefixed(std::string(value.data(), value.size()), HashAlgorithm::SHA1);
    } catch (BadHash &) {
        return std::nullopt;
    }
}

/** Directory identity. Unlike blob oids, the daemon guarantees this xattr on
 * every directory it serves — zone mounting already relies on it — so absence
 * is a contract violation, not a fallback case. */
static Hash tecnixReadWorldtreeTreeOid(const std::filesystem::path & path)
{
    if (auto oid = tecnixReadWorldtreeOidXattr(path, TECNIX_WORLDTREE_TREE_OID_XATTR))
        return *oid;
    throw Error("worldtree: cannot read tree identity for '%s'", path.string());
}

[[noreturn]] static void tecnixThrowHistoricalWorldManifestError(const EvalSettings & settings, std::string_view detail)
{
    throw Error(
        "worldtree: historical World manifest '.meta/manifest.json' for commit '%s' is missing or malformed: %s",
        settings.tectonixGitSha.get(),
        detail);
}

void configureTectonixContext(EvalState & state, std::string gitDir, std::string rev, std::string checkoutPath)
{
    std::lock_guard lock(tecnixData(state)->tectonixContextMutex);

    EvalState::TecnixEvalData::TectonixContext next{
        .gitDir = std::move(gitDir),
        .rev = std::move(rev),
        .checkoutPath = std::move(checkoutPath),
    };

    if (tecnixData(state)->tectonixContext) {
        auto & current = *tecnixData(state)->tectonixContext;
        if (current.gitDir != next.gitDir || current.rev != next.rev || current.checkoutPath != next.checkoutPath)
            throw Error(
                "Tecnix EvalState is already configured for gitDir '%s', rev '%s', checkoutPath '%s'; "
                "cannot reconfigure it for gitDir '%s', rev '%s', checkoutPath '%s'",
                current.gitDir,
                current.rev,
                current.checkoutPath,
                next.gitDir,
                next.rev,
                next.checkoutPath);
        return;
    }

    tecnixData(state)->tectonixContext = next;

    auto & mutableSettings = const_cast<EvalSettings &>(state.settings);
    mutableSettings.tectonixGitDir.assign(next.gitDir);
    mutableSettings.tectonixGitSha.assign(next.rev);
    if (!next.checkoutPath.empty())
        mutableSettings.tectonixCheckoutPath.assign(next.checkoutPath);
}

static std::string gitFingerprintWithMode(const Hash & oid, uint32_t mode)
{
    std::ostringstream out;
    out << "git:" << oid.gitRev() << ";mode=" << std::oct << std::setw(6) << std::setfill('0') << mode;
    return out.str();
}

/**
 * The dirty (modified, added, deleted, renamed, untracked) repo-relative
 * paths of `checkoutPath`, per `git status --porcelain -z`.
 *
 * Scrubs the git discovery environment (GIT_DIR, GIT_WORK_TREE,
 * GIT_COMMON_DIR, GIT_INDEX_FILE) so an ambient git context — a hook, a
 * rebase exec step, tooling that exports these — cannot redirect status to a
 * different repository or index. Throws if git status fails: the dirty set
 * is load-bearing for Tecnix source-closure validity, so callers that can
 * tolerate an unknown dirty state must catch explicitly.
 */
static std::vector<std::string> gitStatusDirtyPaths(const std::string & checkoutPath)
{
    StringMap gitEnvironment = getEnv();
    gitEnvironment.erase("GIT_DIR");
    gitEnvironment.erase("GIT_WORK_TREE");
    gitEnvironment.erase("GIT_COMMON_DIR");
    gitEnvironment.erase("GIT_INDEX_FILE");

    auto [status, output] = runProgram(
        {.program = "git",
         .args = {"-C", checkoutPath, "--no-optional-locks", "status", "--porcelain", "-z", "--untracked-files=all"},
         .environment = gitEnvironment});
    if (!statusOk(status))
        throw Error("failed to get git status in '%s': program 'git' %s", checkoutPath, statusToString(status));

    return parseGitPorcelainZDirtyPaths(output);
}

/**
 * Read delegation wrapper that preserves the path-fingerprint semantics Tecnix
 * historically used with libgit2: fingerprints are the git object id at the
 * requested path, not merely the root accessor fingerprint.
 */
/**
 * The Tecnix source accessor: a clean source tree pinned at a rev, an
 * optional dirty-checkout overlay, per-path fingerprints, and repo-relative
 * source-access tracking, in one place.
 *
 * Clean content is served by `clean`: a libgit2 tree accessor or, in a
 * worldtree sandbox, the immutable FUSE projection at the pinned rev
 * (`WorldtreeFuseSourceAccessor`). Clean-path fingerprints come from libgit2
 * (`git`) when set; otherwise `clean` fingerprints its own paths via
 * `getFingerprint` (the worldtree FUSE backend does this natively).
 *
 * `repoPrefix` maps accessor-local paths to the repo-relative paths recorded
 * as source dependencies: empty for the repo-wide accessor, or a zone path
 * for legacy zone accessors. Repo-root access has no source-path
 * representation yet, so with an empty prefix it fails closed under active
 * tracking instead of silently under-tracking.
 */
struct TecnixSourceAccessor : SourceAccessor
{
    struct GitCleanFingerprints
    {
        ref<GitRepo> repo;
        Hash treeSha;
    };

    /** Transparent hashing so hot-path lookups take `path.rel()` views
     * without materializing a std::string per read/stat. */
    using DirtyPathSet = boost::unordered_flat_set<std::string, StringViewHash, std::equal_to<>>;

    ref<SourceAccessor> clean, disk;
    std::optional<GitCleanFingerprints> git;
    std::string repoPrefix;
    DirtyPathSet dirtyFiles, dirtyDirs;

    TecnixSourceAccessor(
        ref<SourceAccessor> clean,
        ref<SourceAccessor> disk,
        std::optional<GitCleanFingerprints> git,
        std::string repoPrefix,
        DirtyPathSet && dirtyFiles)
        : clean(std::move(clean))
        , disk(std::move(disk))
        , git(std::move(git))
        , repoPrefix(std::move(repoPrefix))
        , dirtyFiles(std::move(dirtyFiles))
    {
        for (auto & f : this->dirtyFiles) {
            debug("TecnixSourceAccessor: dirty file: '%s'", f);
            for (auto p = CanonPath(f); !p.isRoot();) {
                p.pop();
                if (!dirtyDirs.emplace(p.rel()).second)
                    break;
            }
        }
    }

    std::string_view trackedRepoPathForAccess(const CanonPath & path, std::string & scratch) const
    {
        if (path.isRoot()) {
            if (repoPrefix.empty())
                throw Error(
                    "Tecnix dependency tracking cannot represent repo-root source access yet; use a repo-relative child path instead");
            return repoPrefix;
        }

        auto rel = path.rel();
        if (repoPrefix.empty())
            return rel;

        scratch.reserve(repoPrefix.size() + 1 + rel.size());
        scratch.append(repoPrefix);
        scratch.push_back('/');
        scratch.append(rel);
        return scratch;
    }

    bool isDirty(const CanonPath & path)
    {
        return dirtyFiles.contains(path.rel());
    }

    bool tracksEvalAccesses(const CanonPath &) override
    {
        return true;
    }

    void recordEvalAccess(const CanonPath & path) override
    {
        trackAccess(path);
    }

    void trackAccess(const CanonPath & path)
    {
        if (auto ctx = currentTecnixThreadState.trackingContext; ctx) {
            static thread_local std::string trackedRepoPathScratch;
            trackedRepoPathScratch.clear();
            ctx->recordAccess(trackedRepoPathForAccess(path, trackedRepoPathScratch));
        }
    }

    /**
     * Deliberately untracked. Stats are mostly evaluator plumbing (symlink
     * and import resolution, store-copy machinery), and recording every stat
     * would drag ancestor directories — up to the unrepresentable repo root —
     * into closures. The rule is: record at the lowest layer that knows the
     * observation is semantic. Every read is semantic, so reads self-record
     * here; for stats only the call site knows, so call sites where existence
     * or file type is the observed result must record it via
     * `recordEvalAccess` (see `prim_pathExists` and `prim_readFileType`). Any
     * new primop that observes existence or type without a read must do the
     * same, or it silently under-tracks.
     */
    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        std::optional<Stat> s;
        if (path.isRoot())
            s = clean->maybeLstat(path);
        else if (isDirty(path))
            s = disk->maybeLstat(path);
        else {
            s = clean->maybeLstat(path);
            if (!s && dirtyDirs.contains(path.rel()))
                s = disk->maybeLstat(path);
        }
        return s;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        if (dumpPathDepth == 0)
            trackAccess(path);
        return (isDirty(path) ? disk : clean)->readFile(path, sink, sizeCallback);
    }

    std::string readLink(const CanonPath & path) override
    {
        if (dumpPathDepth == 0)
            trackAccess(path);
        return (isDirty(path) ? disk : clean)->readLink(path);
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return (isDirty(path) ? disk : clean)->getPhysicalPath(path);
    }

    /** The fingerprint of `path` in the clean tree, before the dirty overlay. */
    std::pair<CanonPath, std::optional<std::string>> cleanFingerprint(const CanonPath & path)
    {
        if (!git)
            return clean->getFingerprint(path);

        if (path.isRoot())
            return {path, "git:" + git->treeSha.gitRev()};

        auto pathInfo = git->repo->getPathInfo(git->treeSha, std::string(path.rel()));
        if (!pathInfo)
            return {path, "absent"};
        return {path, gitFingerprintWithMode(pathInfo->oid, pathInfo->mode)};
    }

    /**
     * Deliberately memo-free: Tecnix closure validation and fingerprinting
     * already memoize per run (the thread-local dependency-fingerprint cache
     * in this file), and other callers (fetchToStore) are bounded by the
     * srcToStore cache. One caching layer is enough.
     */
    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        trackAccess(path);

        auto rel = path.isRoot() ? std::string{} : std::string(path.rel());
        auto [_cleanPath, cleanFp] = cleanFingerprint(path);

        // nullopt means the backend declined to certify this path (a missing
        // path reports "absent"); propagate rather than coercing to "absent",
        // which would be a fetch-cache key shared by every revision.
        if (!cleanFp)
            return {path, std::nullopt};

        auto dirtyPrefix = path.isRoot() ? "" : rel + "/";
        std::vector<std::string> dirtyUnderPath;
        for (auto & f : dirtyFiles) {
            if (path.isRoot() || f == rel || f.starts_with(dirtyPrefix))
                dirtyUnderPath.push_back(f);
        }

        if (!path.isRoot() && dirtyFiles.contains(rel) && !disk->maybeLstat(path))
            return {path, "absent"};

        std::string fp = *cleanFp;
        if (!dirtyUnderPath.empty()) {
            std::sort(dirtyUnderPath.begin(), dirtyUnderPath.end());
            HashSink hashSink{HashAlgorithm::SHA256};
            for (auto & f : dirtyUnderPath) {
                hashSink << f;
                auto st = disk->maybeLstat(CanonPath(f));
                if (!st) {
                    hashSink << "D";
                } else if (st->type == Type::tRegular) {
                    hashSink << (st->isExecutable ? "X" : "F");
                    hashSink << disk->readFile(CanonPath(f));
                } else if (st->type == Type::tSymlink) {
                    hashSink << "L";
                    hashSink << disk->readLink(CanonPath(f));
                }
            }
            fp += ";dirty=" + hashSink.finish().hash.to_string(HashFormat::Base16, false);
        }

        return {path, fp};
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        if (dumpPathDepth == 0)
            trackAccess(path);

        auto rel = path.isRoot() ? "" : std::string(path.rel());
        if (!path.isRoot() && !dirtyDirs.contains(rel))
            return clean->readDirectory(path);

        DirEntries entries;
        try {
            entries = clean->readDirectory(path);
        } catch (...) {
        }

        auto dirPrefix = rel.empty() ? "" : rel + "/";
        for (auto & f : dirtyFiles) {
            if (!f.starts_with(dirPrefix))
                continue;
            auto rest = std::string_view(f).substr(dirPrefix.size());
            if (rest.find('/') != std::string_view::npos)
                continue;
            auto stat = disk->maybeLstat(path / rest);
            if (stat)
                entries[std::string(rest)] = stat->type;
            else
                entries.erase(std::string(rest));
        }
        for (auto & d : dirtyDirs) {
            if (!d.starts_with(dirPrefix))
                continue;
            auto rest = std::string_view(d).substr(dirPrefix.size());
            if (rest.find('/') != std::string_view::npos || rest.empty())
                continue;
            if (!entries.count(std::string(rest)))
                entries[std::string(rest)] = Type::tDirectory;
        }
        return entries;
    }
};

// ============================================================================
// Tecnix repo-wide accessor (the tracked evaluation path)
// ============================================================================

/**
 * The Tecnix clean-tree backend for worldtree sandboxes: the committed repo
 * view at a pinned revision, served by the worldtree FUSE projection
 * (`<mount>/tecnix/<rev>`). Repo-relative paths map through the committed
 * manifest to `<zone-id>/<path-under-zone>` reads; ancestors of zone roots
 * are synthesized, and committed paths outside every visible zone do not
 * exist in this view (the daemon's visibility contract).
 *
 * Fingerprints keep the git vocabulary (`git:<oid>;mode=<mode>` / `absent`)
 * so TXDC rows validate across backends: directories read the tree-oid
 * xattr, files the blob-oid xattr when served (hashing the bytes otherwise,
 * which reproduces the same oid), synthesized directories compose their
 * children. See the explainer §7 for the economics.
 */
struct WorldtreeFuseSourceAccessor : SourceAccessor
{
    std::filesystem::path revisionRoot;

    /** Blob fingerprint memo; the projection is immutable, so entries stay
     * valid for the accessor's lifetime. */
    boost::concurrent_flat_map<std::string, std::string> blobFingerprintMemo;

    /** Repo-relative zone path (no leading `//`) → validated zone id. */
    std::map<std::string, std::string> zones;

    /** Per-zone filesystem accessors rooted at `<revisionRoot>/<zone-id>`. */
    std::mutex zoneFSMutex;
    std::map<std::string, ref<SourceAccessor>> zoneFS;

    WorldtreeFuseSourceAccessor(std::filesystem::path revisionRoot, const nlohmann::json & manifest)
        : revisionRoot(std::move(revisionRoot))
    {
        for (auto & [worldPath, value] : manifest.items()) {
            if (!value.is_object() || !value.contains("id") || !value.at("id").is_string())
                continue;
            auto zonePath = tecnixNormalizeRepoPath(worldPath);
            if (zonePath.empty())
                continue;
            zones[zonePath] = tecnixRequireWorldtreeZoneId(value.at("id").get<std::string>());
        }
        if (zones.empty())
            throw Error("worldtree: the manifest at '%s' names no zones", this->revisionRoot.string());
    }

    struct InZone
    {
        std::string zonePath;
        std::string zoneId;
        std::string rel; // path under the zone root; empty for the zone root itself
    };

    static std::string_view key(const CanonPath & path)
    {
        return path.isRoot() ? std::string_view() : path.rel();
    }

    /** Longest manifest zone containing `rel`, if any. */
    std::optional<InZone> resolveZone(std::string_view rel) const
    {
        std::optional<InZone> best;
        for (auto & [zonePath, zoneId] : zones) {
            bool contains =
                rel == zonePath
                || (rel.size() > zonePath.size() && rel.starts_with(zonePath) && rel[zonePath.size()] == '/');
            if (contains && (!best || zonePath.size() > best->zonePath.size()))
                best = InZone{
                    .zonePath = zonePath,
                    .zoneId = zoneId,
                    .rel = rel.size() == zonePath.size() ? std::string() : std::string(rel.substr(zonePath.size() + 1)),
                };
        }
        return best;
    }

    /** Whether `rel` is the repo root or a proper ancestor of a zone root. */
    bool isZoneAncestor(std::string_view rel) const
    {
        if (rel.empty())
            return true;
        for (auto & [zonePath, _] : zones)
            if (zonePath.size() > rel.size() && zonePath.starts_with(rel) && zonePath[rel.size()] == '/')
                return true;
        return false;
    }

    /** Next path components of zone roots strictly below ancestor `rel`. */
    std::set<std::string> zoneChildNames(std::string_view rel) const
    {
        std::set<std::string> names;
        for (auto & [zonePath, _] : zones) {
            std::string_view tail;
            if (rel.empty())
                tail = zonePath;
            else if (zonePath.size() > rel.size() && zonePath.starts_with(rel) && zonePath[rel.size()] == '/')
                tail = std::string_view(zonePath).substr(rel.size() + 1);
            else
                continue;
            names.insert(std::string(tail.substr(0, tail.find('/'))));
        }
        return names;
    }

    ref<SourceAccessor> zoneAccessor(const InZone & z)
    {
        std::lock_guard<std::mutex> lock(zoneFSMutex);
        if (auto it = zoneFS.find(z.zoneId); it != zoneFS.end())
            return it->second;
        auto root = revisionRoot / z.zoneId;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            throw Error("worldtree: immutable zone '%s' is unavailable at '%s'", z.zonePath, root.string());
        auto accessor = makeFSSourceAccessor(root);
        zoneFS.emplace(z.zoneId, accessor);
        return accessor;
    }

    static CanonPath zoneRelPath(const InZone & z)
    {
        return CanonPath(z.rel);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto rel = key(path);
        if (auto z = resolveZone(rel))
            return zoneAccessor(*z)->maybeLstat(zoneRelPath(*z));
        if (isZoneAncestor(rel))
            return Stat{.type = tDirectory};
        return std::nullopt;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto rel = key(path);
        DirEntries entries;
        if (auto z = resolveZone(rel))
            entries = zoneAccessor(*z)->readDirectory(zoneRelPath(*z));
        else if (!isZoneAncestor(rel))
            throw Error("worldtree: '%s' is not a directory in the immutable view", showPath(path));
        // Nested zone roots surface even when the enclosing projection omits them.
        for (auto & name : zoneChildNames(rel))
            entries.emplace(name, tDirectory);
        return entries;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto z = resolveZone(key(path));
        if (!z)
            throw Error("worldtree: '%s' is not a readable file in the immutable view", showPath(path));
        zoneAccessor(*z)->readFile(zoneRelPath(*z), sink, std::move(sizeCallback));
    }

    std::string readLink(const CanonPath & path) override
    {
        auto z = resolveZone(key(path));
        if (!z)
            throw Error("worldtree: '%s' is not a symlink in the immutable view", showPath(path));
        return zoneAccessor(*z)->readLink(zoneRelPath(*z));
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        if (auto z = resolveZone(key(path)))
            return zoneAccessor(*z)->getPhysicalPath(zoneRelPath(*z));
        return std::nullopt;
    }

    /** Git blob fingerprint hashed from content; a blob oid is a pure
     * function of the bytes, so this reproduces what libgit2 reports. */
    std::string hashBlobFingerprint(const InZone & z, const Stat & st)
    {
        auto fs = zoneAccessor(z);
        auto p = zoneRelPath(z);
        HashSink sink(HashAlgorithm::SHA1);
        // Git object framing per the object spec: "blob <size>\0<bytes>".
        auto writeBlobPrefix = [&](uint64_t size) {
            auto prefix = "blob " + std::to_string(size);
            prefix.push_back('\0');
            sink(prefix);
        };
        uint32_t mode;
        if (st.type == tSymlink) {
            auto target = fs->readLink(p);
            writeBlobPrefix(target.size());
            sink(target);
            mode = 0120000;
        } else {
            fs->readFile(p, sink, [&](uint64_t size) { writeBlobPrefix(size); });
            mode = st.isExecutable ? 0100755 : 0100644;
        }
        return gitFingerprintWithMode(sink.finish().hash, mode);
    }

    std::string blobFingerprint(const InZone & z, const Stat & st)
    {
        auto memoKey = z.zoneId + "/" + z.rel;
        {
            std::string cached;
            if (blobFingerprintMemo.visit(memoKey, [&](const auto & entry) { cached = entry.second; }))
                return cached;
        }

        // Regular files may carry the blob oid as an xattr. Symlinks never do:
        // getxattr() would follow the link and answer for its target.
        std::string fingerprint;
        if (st.type == tRegular) {
            auto physical = revisionRoot / z.zoneId;
            if (!z.rel.empty())
                physical /= z.rel;
            if (auto oid = tecnixReadWorldtreeOidXattr(physical, TECNIX_WORLDTREE_BLOB_OID_XATTR))
                fingerprint = gitFingerprintWithMode(*oid, st.isExecutable ? 0100755 : 0100644);
        }
        if (fingerprint.empty())
            fingerprint = hashBlobFingerprint(z, st);

        blobFingerprintMemo.insert_or_assign(memoKey, fingerprint);
        return fingerprint;
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        auto rel = key(path);
        if (auto z = resolveZone(rel)) {
            auto st = zoneAccessor(*z)->maybeLstat(zoneRelPath(*z));
            if (!st)
                return {path, "absent"};
            if (st->type == tDirectory) {
                auto physical = revisionRoot / z->zoneId;
                if (!z->rel.empty())
                    physical /= z->rel;
                return {path, gitFingerprintWithMode(tecnixReadWorldtreeTreeOid(physical), 0040000)};
            }
            return {path, blobFingerprint(*z, *st)};
        }

        if (!isZoneAncestor(rel))
            return {path, "absent"};

        // Synthesized directories (the root and zone ancestors) correspond to
        // no single git object; compose identity from their children.
        HashSink sink(HashAlgorithm::SHA256);
        for (auto & name : zoneChildNames(rel)) {
            auto [childPath, childFp] = getFingerprint(path / name);
            sink << name << childFp.value_or("");
        }
        return {path, "worldtree-union:" + sink.finish().hash.to_string(HashFormat::Base16, false)};
    }

    std::optional<time_t> getLastModified() override
    {
        return std::nullopt;
    }
};

ref<SourceAccessor> getTecnixRepoAccessor(EvalState & state)
{
    std::call_once(tecnixData(state)->tecnixRepoAccessorFlag, [&state]() {
        auto & sha = requireTectonixGitSha(state);
        auto commitHash = Hash::parseNonSRIUnprefixed(sha, HashAlgorithm::SHA1);

        auto [cleanAccessor, gitFingerprints] =
            [&]() -> std::pair<ref<SourceAccessor>, std::optional<TecnixSourceAccessor::GitCleanFingerprints>> {
            if (!state.settings.tectonixWorldtreeSocket.get().empty()) {
                // Clean base: the immutable FUSE projection at the pinned rev,
                // mapped through the *committed* manifest (never the checkout copy).
                auto revisionRoot = tecnixWorldtreeRevisionRoot(state.settings);
                auto manifestPath = revisionRoot / "W-000000" / "manifest.json";
                std::error_code ec;
                if (!std::filesystem::is_regular_file(manifestPath, ec))
                    tecnixThrowHistoricalWorldManifestError(state.settings, ec ? ec.message() : "file does not exist");
                nlohmann::json manifest;
                try {
                    manifest = nlohmann::json::parse(readFile(manifestPath));
                } catch (const nlohmann::json::parse_error & e) {
                    tecnixThrowHistoricalWorldManifestError(state.settings, e.what());
                }
                debug("created Tecnix repo-wide worldtree FUSE accessor at commit %s", sha);
                return {make_ref<WorldtreeFuseSourceAccessor>(revisionRoot, manifest), std::nullopt};
            }

            auto repo = getWorldRepo(state);
            auto rootTreeSha = repo->getCommitTree(commitHash);
            GitAccessorOptions opts{.exportIgnore = false, .smudgeLfs = false};
            debug("created Tecnix repo-wide libgit2 accessor at commit %s", sha);
            return {
                repo->getAccessor(rootTreeSha, opts, "repo"),
                TecnixSourceAccessor::GitCleanFingerprints{repo, rootTreeSha}};
        }();

        if (isTectonixSourceAvailable(state)) {
            auto checkoutPath = state.settings.tectonixCheckoutPath.get();

            // Get all dirty files in the repo. This is load-bearing for source
            // closure validity: if we cannot determine the dirty overlay, do
            // not continue with a clean-tree accessor (gitStatusDirtyPaths
            // throws). Until the daemon exposes a full repo dirty-path RPC,
            // materialized checkouts keep using git status at this boundary.
            TecnixSourceAccessor::DirtyPathSet dirtyFiles;
            for (auto & path : gitStatusDirtyPaths(checkoutPath))
                dirtyFiles.insert(std::move(path));

            tecnixData(state)->tecnixRepoAccessor = make_ref<TecnixSourceAccessor>(
                cleanAccessor, makeFSSourceAccessor(checkoutPath), gitFingerprints, "", std::move(dirtyFiles));
        } else {
            tecnixData(state)->tecnixRepoAccessor = make_ref<TecnixSourceAccessor>(
                cleanAccessor, cleanAccessor, gitFingerprints, "", TecnixSourceAccessor::DirtyPathSet{});
        }

        debug("created Tecnix repo-wide accessor");
    });
    return *tecnixData(state)->tecnixRepoAccessor;
}

StorePath mountTecnixRepoAccessor(EvalState & state)
{
    std::call_once(tecnixData(state)->tecnixRepoMountFlag, [&state]() {
        auto accessor = getTecnixRepoAccessor(state);
        auto storePath = StorePath::random("world-repo");
        state.storeFS->mount(CanonPath(state.store->printStorePath(storePath)), accessor);
        state.allowPath(storePath);
        tecnixData(state)->tecnixRepoMountStorePath = storePath;
        debug("mounted Tecnix repo accessor at %s", state.store->printStorePath(storePath));
    });
    return *tecnixData(state)->tecnixRepoMountStorePath;
}

std::string resolveCheckoutHeadRev(const std::string & checkoutPath)
{
    return GitRepo::openRepo(checkoutPath, {})->resolveRef("HEAD").gitRev();
}

std::string getTecnixRepoPath(EvalState & state, std::string_view repoRelPath)
{
    auto rootStorePath = mountTecnixRepoAccessor(state);
    auto path = tecnixNormalizeRepoPath(repoRelPath);
    auto result = state.store->printStorePath(rootStorePath) + "/" + path;
    debug("getTecnixRepoPath: '%s' -> '%s'", repoRelPath, result);
    return result;
}

} // namespace nix
