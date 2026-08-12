#include "nix/expr/eval.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "tecnix/eval-data.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/store-api.hh"
#include "nix/util/current-process.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/hash.hh"
#include "nix/util/processes.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings-inline.hh"
#include "nix/util/util.hh"
#include "nix/util/worldtree-client.hh"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <map>
#include <ranges>
#include <set>
#include <sstream>

#include <sys/xattr.h>

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json.hpp>

namespace nix {

static EvalState::TecnixEvalData * accessorData(EvalState & state)
{
    return &state.tecnixEvalData();
}

static const EvalState::TecnixEvalData * accessorData(const EvalState & state)
{
    return &state.tecnixEvalData();
}

ref<GitRepo> getWorldRepo(const EvalState & state)
{
    std::call_once(accessorData(state)->worldRepoFlag, [&state]() {
        auto gitDir = state.settings.tectonixGitDir.get();
        if (gitDir.empty())
            throw Error("--tectonix-git-dir must be specified to use tectonix builtins");

        // Expand ~ to home directory
        if (hasPrefix(gitDir, "~/"))
            gitDir = getHome() + gitDir.substr(1);

        accessorData(state)->worldRepo = GitRepo::openRepo(std::filesystem::path(gitDir), {.bare = true});
        debug("opened world repo at %s", gitDir);
    });
    return *accessorData(state)->worldRepo;
}

const std::string & requireTectonixGitSha(const EvalState & state)
{
    auto & sha = state.settings.tectonixGitSha.get();
    if (sha.empty())
        throw Error("--tectonix-git-sha must be specified to use tectonix builtins");
    return sha;
}

ref<SourceAccessor> getWorldGitAccessor(const EvalState & state)
{
    std::call_once(accessorData(state)->worldGitAccessorFlag, [&state]() {
        auto & sha = requireTectonixGitSha(state);

        auto repo = getWorldRepo(state);
        auto hash = Hash::parseNonSRIUnprefixed(sha, HashAlgorithm::SHA1);

        if (!repo->hasObject(hash))
            throw Error("tectonix-git-sha '%s' not found in repository", sha);

        // Validate that the SHA is a commit by trying to get its tree.
        // This gives a clear error if someone accidentally passes a tree or blob SHA.
        try {
            repo->getCommitTree(hash);
        } catch (Error & e) {
            throw Error("tectonix-git-sha '%s' does not appear to be a valid commit: %s", sha, e.what());
        }

        // exportIgnore=false: The world accessor is used for path validation and tree SHA
        // computation, where we need to see all files. Repo/zone accessors used for
        // actual content use exportIgnore=true to honor .gitattributes.
        GitAccessorOptions opts{.exportIgnore = false, .smudgeLfs = false};
        accessorData(state)->worldGitAccessor = repo->getAccessor(hash, opts, "world");
        debug("created world accessor at commit %s", sha);
    });
    return *accessorData(state)->worldGitAccessor;
}

bool isTectonixSourceAvailable(const EvalState & state)
{
    return !state.settings.tectonixCheckoutPath.get().empty();
}

// Helper to normalize paths: strip leading // prefix
// Paths in manifest have // prefix (e.g., //areas/tools/dev)
// Filesystem operations need paths without // (e.g., areas/tools/dev)
static std::string normalizePath(std::string_view path)
{
    std::string result(path);
    if (hasPrefix(result, "//"))
        result = result.substr(2);
    return result;
}

static std::string normalizeZonePath(std::string_view zonePath)
{
    return normalizePath(zonePath);
}

static GitAccessorOptions
makeZoneAccessorOptions(ref<GitRepo> repo, const Hash & commitHash, const std::string & zonePath)
{
    std::string attrFp;
    for (auto & h : repo->getGitAttributesAlongPath(commitHash, zonePath))
        attrFp += h.gitRev();
    return {
        .exportIgnore = true,
        .smudgeLfs = true,
        .attrCommitRev = commitHash,
        .attrPathPrefix = zonePath,
        .attrFingerprint = std::move(attrFp),
    };
}

// Helper to sanitize zone path for use in store path names.
// Store paths only allow: a-zA-Z0-9 and +-._?=
// Replaces / with - and any other invalid chars with _
static std::string sanitizeZoneNameForStore(std::string_view zonePath)
{
    auto zone = normalizeZonePath(zonePath);
    std::string result;
    result.reserve(zone.size());
    for (char c : zone) {
        if (c == '/') {
            result += '-';
        } else if (
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' || c == '-'
            || c == '.' || c == '_' || c == '?' || c == '=') {
            result += c;
        } else {
            result += '_';
        }
    }
    return result;
}

// ============================================================================
// Worldtree integration (legacy tectonix builtins)
//
// When `tectonix-worldtree-socket` is set, mutable-checkout metadata moves off the
// libgit2 repo+checkout walk and onto O(changes) RPCs against the bound workspace:
//   * getTectonixDirtyZones() -> `dirty_zones`     — the tracked-dirty set;
//   * getWorldTreeSha()       -> `zone_tree_shas`  — the working-tree subtree oid
//       (the committed oid when clean, the synthesized frontier oid when dirty;
//       the zone's build-cache key);
//   * getLegacyTectonixZoneStorePath() reads source bytes from FUSE in both regimes:
//     the materialized checkout for the mutable workspace and
//     `<mount>/tecnix/<sha>/<zone-id>` for immutable history.
// Fail-loud contract: when the socket is SET, the daemon is the sole source of truth — a
// worldtree sandbox has no git repo to fall back to. A daemon that is unreachable, or that
// refuses/errors a request, is a hard failure (the error propagates), never a silent
// downgrade. libgit2 / the checkout walk are reached ONLY when the socket is UNSET (plain
// local, non-worldtree eval). Historical reads create no daemon connection.
// ============================================================================

/** Reinterpret a 20-byte worldtree object id as a Nix SHA-1 Hash. */
static Hash oidToHash(const worldtree::Oid & oid)
{
    Hash h(HashAlgorithm::SHA1);
    assert(h.hashSize == oid.size());
    std::memcpy(h.hash, oid.data(), oid.size());
    return h;
}

/**
 * The scoped socket is only the mutable root-checkout control plane. Historical
 * committed source is ordinary filesystem input beneath
 * `<tectonix-worldtree-mount>/tecnix/<sha>/<zone-id>`.
 */
struct WorldtreeConn
{
    uint64_t ws;
    std::mutex mutex;
    worldtree::Client client;

    WorldtreeConn(worldtree::Client && client, uint64_t ws)
        : ws(ws)
        , client(std::move(client))
    {
    }

    /** The dirty set with each zone's changed files (for full ZoneDirtyInfo). */
    std::vector<worldtree::ZoneDirty> dirtyZoneEntries()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return client.dirtyZoneEntries(ws);
    }

    /** One zone's working-tree subtree oid, or nullopt when absent or out of scope. */
    std::optional<Hash> zoneTreeSha(std::string_view worldPath)
    {
        std::string wp = hasPrefix(worldPath, "//") ? std::string(worldPath) : "//" + std::string(worldPath);
        std::lock_guard<std::mutex> lock(mutex);
        auto resp = client.zoneTreeShas(ws, {wp});
        if (resp.empty() || !resp.front().treeSha)
            return std::nullopt;
        return oidToHash(*resp.front().treeSha);
    }
};

static constexpr std::string_view WORLDTREE_TREE_OID_XATTR = "user.worldtree.tree-oid";

static std::filesystem::path worldtreeRevisionRoot(const EvalSettings & settings)
{
    auto revision = Hash::parseNonSRIUnprefixed(settings.tectonixGitSha.get(), HashAlgorithm::SHA1);
    return std::filesystem::path(settings.tectonixWorldtreeMount.get()) / "tecnix" / revision.gitRev();
}

static std::string requireWorldtreeZoneId(const std::string & id)
{
    auto isLowerHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    if (id.size() != 8 || !id.starts_with("W-") || !std::ranges::all_of(std::string_view(id).substr(2), isLowerHex))
        throw Error("worldtree: invalid zone id '%s' in manifest", id);
    return id;
}

/** Find the manifest zone containing worldPath and return (zone id, path within zone). */
static std::pair<std::string, std::string>
worldtreeZoneLocation(const nlohmann::json & manifest, std::string_view worldPath)
{
    auto clean = normalizeZonePath(worldPath);
    while (!clean.empty() && clean.front() == '/')
        clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == '/')
        clean.pop_back();
    for (auto & component : tokenizeString<std::vector<std::string>>(clean, "/"))
        if (component.empty() || component == "." || component == "..")
            throw Error("invalid world path '%s'", worldPath);

    const nlohmann::json * best = nullptr;
    std::string bestPath;
    for (auto & [candidateWorldPath, value] : manifest.items()) {
        auto candidate = normalizeZonePath(candidateWorldPath);
        bool contains =
            clean == candidate
            || (clean.size() > candidate.size() && hasPrefix(clean, candidate) && clean[candidate.size()] == '/');
        if (contains && candidate.size() > bestPath.size()) {
            best = &value;
            bestPath = std::move(candidate);
        }
    }
    if (!best || !best->is_object() || !best->contains("id") || !best->at("id").is_string())
        throw Error("worldtree: path '%s' is not contained by a visible World zone", worldPath);

    auto id = requireWorldtreeZoneId(best->at("id").get<std::string>());
    auto relative = clean.size() == bestPath.size() ? std::string() : clean.substr(bestPath.size() + 1);
    return {std::move(id), std::move(relative)};
}

static Hash readWorldtreeTreeOid(const std::filesystem::path & path)
{
    std::array<char, 40> value{};
#ifdef __APPLE__
    auto size = ::getxattr(path.c_str(), WORLDTREE_TREE_OID_XATTR.data(), value.data(), value.size(), 0, 0);
#else
    auto size = ::getxattr(path.c_str(), WORLDTREE_TREE_OID_XATTR.data(), value.data(), value.size());
#endif
    if (size < 0)
        throw Error("worldtree: cannot read tree identity for '%s': %s", path.string(), std::strerror(errno));
    if (size != static_cast<ssize_t>(value.size()))
        throw Error("worldtree: invalid tree identity on '%s'", path.string());
    return Hash::parseNonSRIUnprefixed(std::string(value.data(), value.size()), HashAlgorithm::SHA1);
}

[[noreturn]] static void throwHistoricalWorldManifestError(const EvalSettings & settings, std::string_view detail)
{
    throw Error(
        "worldtree: historical World manifest '.meta/manifest.json' for commit '%s' is missing or malformed: %s",
        settings.tectonixGitSha.get(),
        detail);
}

static std::shared_ptr<WorldtreeConn> connectWorldtree(const EvalState & state)
{
    auto socketPath = state.settings.tectonixWorldtreeSocket.get();
    // The socket being unset is the *only* non-worldtree signal — plain local eval, where
    // libgit2 is the source. Returning nullptr here routes callers to that path.
    if (socketPath.empty())
        return nullptr;
    // Fail-loud (the load-bearing invariant): with the socket SET there is no git repo to
    // fall back to, so an unreachable daemon is a hard error — let `Client::connect`'s
    // `ProtocolError` propagate rather than silently degrading to libgit2 (which would read
    // the wrong content, or none).
    auto ws = state.settings.tectonixWorldtreeWorkspace.get();
    return std::make_shared<WorldtreeConn>(worldtree::Client::connect(socketPath), ws);
}

static std::shared_ptr<WorldtreeConn> worldtreeControlConn(const EvalState & state)
{
    // Historical --ref evaluations are filesystem-only. Returning null here is not a
    // libgit2 fallback: their callers branch on worldtree mode before consulting this
    // control seam.
    if (!isTectonixSourceAvailable(state))
        return nullptr;
    std::call_once(accessorData(state)->worldtreeControlConnFlag, [&state]() {
        accessorData(state)->worldtreeControlConn_ = connectWorldtree(state);
    });
    return accessorData(state)->worldtreeControlConn_;
}

Hash getWorldTreeSha(const EvalState & state, std::string_view worldPath)
{
    // The mutable root checkout needs its synthesized working-tree oid from the control
    // plane. A historical evaluation is already pinned by its FUSE path and reads the
    // exact committed tree oid from that directory's synthetic xattr instead.
    if (isTectonixSourceAvailable(state)) {
        if (auto control = worldtreeControlConn(state)) {
            if (auto sha = control->zoneTreeSha(worldPath))
                return *sha;
            throw Error("worldtree: world path '%s' is absent or outside this workspace's visibility scope", worldPath);
        }
    } else if (!state.settings.tectonixWorldtreeSocket.get().empty()) {
        auto revision = worldtreeRevisionRoot(state.settings);
        if (normalizeZonePath(worldPath).empty())
            return readWorldtreeTreeOid(revision);
        auto [zoneId, relative] = worldtreeZoneLocation(getManifestJson(state), worldPath);
        auto path = revision / zoneId;
        if (!relative.empty())
            path /= relative;
        return readWorldtreeTreeOid(path);
    }

    auto path = normalizePath(worldPath);

    // Check cache first
    if (auto cached = getConcurrent(*accessorData(state)->worldTreeShaCache, path)) {
        debug("getWorldTreeSha cache hit for '%s'", path);
        return *cached;
    }

    // Compute by walking from root
    auto repo = getWorldRepo(state);
    auto & sha = requireTectonixGitSha(state);
    auto commitSha = Hash::parseNonSRIUnprefixed(sha, HashAlgorithm::SHA1);

    // Get the root tree SHA from the commit
    auto rootTreeSha = repo->getCommitTree(commitSha);

    // Walk path components, caching intermediate results
    Hash currentSha = rootTreeSha;
    std::string currentPath;

    // Reuse cached accessor for path validation
    auto accessor = getWorldGitAccessor(state);

    for (auto & component : tokenizeString<std::vector<std::string>>(path, "/")) {
        if (component.empty())
            continue;
        if (component == ".." || component == ".")
            throw Error("invalid path component '%s' in world path '%s'", component, worldPath);

        std::string nextPath = currentPath.empty() ? component : currentPath + "/" + component;

        // Check if this level is cached
        if (auto cached = getConcurrent(*accessorData(state)->worldTreeShaCache, nextPath)) {
            currentSha = *cached;
            currentPath = nextPath;
            continue;
        }

        // Need to compute: get tree entry for this component
        auto fullPath = CanonPath("/" + nextPath);
        auto stat = accessor->maybeLstat(fullPath);

        if (!stat || stat->type != SourceAccessor::Type::tDirectory)
            throw Error("path '%s' does not exist or is not a directory in world", nextPath);

        // Get the tree SHA for this subtree
        currentSha = repo->getSubtreeSha(currentSha, component);

        // Cache this level. Note: concurrent threads may compute and insert the same
        // path simultaneously. This is benign because they will compute the same SHA
        // (deterministic from git tree), so either insertion succeeds or finds an
        // equivalent value. We use try_emplace which is atomic for concurrent_flat_map.
        accessorData(state)->worldTreeShaCache->try_emplace(nextPath, currentSha);
        currentPath = nextPath;
    }

    debug("getWorldTreeSha computed '%s' -> %s", path, currentSha.gitRev());
    return currentSha;
}

const std::set<std::string> & getTectonixSparseCheckoutRoots(const EvalState & state)
{
    std::call_once(accessorData(state)->tectonixSparseCheckoutRootsFlag, [&state]() {
        if (isTectonixSourceAvailable(state)) {
            auto checkoutPath = state.settings.tectonixCheckoutPath.get();

            // Read .git to find the actual git directory. It can be either
            // a directory or a file containing "gitdir: <path>".
            auto dotGitPath = std::filesystem::path(checkoutPath) / ".git";
            std::filesystem::path gitDir;

            if (std::filesystem::is_directory(dotGitPath)) {
                gitDir = dotGitPath;
            } else if (std::filesystem::is_regular_file(dotGitPath)) {
                auto gitdirContent = readFile(dotGitPath.string());
                if (hasPrefix(gitdirContent, "gitdir: ")) {
                    auto path = trim(gitdirContent.substr(8));
                    gitDir = std::filesystem::path(path);
                    if (gitDir.is_relative())
                        gitDir = std::filesystem::path(checkoutPath) / gitDir;
                }
            }

            if (!gitDir.empty()) {
                auto sparseRootsPath = gitDir / "info" / "sparse-checkout-roots";
                if (std::filesystem::exists(sparseRootsPath)) {
                    auto content = readFile(sparseRootsPath.string());
                    for (auto & line : tokenizeString<std::vector<std::string>>(content, "\n")) {
                        auto trimmed = trim(line);
                        if (!trimmed.empty())
                            accessorData(state)->tectonixSparseCheckoutRoots.insert(std::string(trimmed));
                    }
                }
            }
        }
    });
    return accessorData(state)->tectonixSparseCheckoutRoots;
}

const std::map<std::string, EvalState::ZoneDirtyInfo> & getTectonixDirtyZones(const EvalState & state)
{
    std::call_once(accessorData(state)->tectonixDirtyZonesFlag, [&state]() {
        auto & dirtyZones = accessorData(state)->tectonixDirtyZones;

        // A historical FUSE view is immutable by construction. Preserve the usual full
        // manifest-shaped result (every visible zone present and clean) without opening a
        // control connection or manufacturing an ephemeral daemon workspace.
        if (!isTectonixSourceAvailable(state) && !state.settings.tectonixWorldtreeSocket.get().empty()) {
            for (auto & [zonePath, value] : getManifestJson(state).items())
                if (value.is_object() && value.contains("id") && value.at("id").is_string())
                    dirtyZones[zonePath] = {};
            return;
        }

        // Worldtree mode: the daemon is authoritative for the tracked-dirty set (design
        // §5.1), derived from the materialization frontier in O(changes) — no
        // O(working-tree) `git status` scan. Reconstruct a *full* ZoneDirtyInfo so every
        // consumer (notably the `__unsafeTectonixInternalDirtyZones` primop) sees every
        // manifest zone with an accurate flag, matching the libgit2 path's shape:
        //   (1) init every manifest zone clean — the immutable FUSE view supplies it in
        //       historical mode and the checkout supplies it in mutable mode (see
        //       getManifestContent); either way it enumerates the full visible zone set;
        //   (2) overlay the daemon's per-zone dirty files for the bound mutable workspace.
        if (auto control = worldtreeControlConn(state)) {
            const nlohmann::json * manifest;
            try {
                manifest = &getManifestJson(state);
            } catch (nlohmann::json::parse_error & e) {
                warn("failed to parse manifest for dirty zone detection: %s", e.what());
                return;
            } catch (Error &) {
                // Manifest unavailable (e.g. daemon refused) — fail loud rather than report
                // a misleading empty/partial dirty set.
                throw;
            }
            for (auto & [zonePath, value] : manifest->items())
                if (value.is_object() && value.contains("id") && value.at("id").is_string())
                    dirtyZones[zonePath] = {};
            for (auto & entry : control->dirtyZoneEntries()) {
                // A dirty zone the daemon names but the manifest omits still surfaces (a
                // zone added in this workspace) — insert-or-update keeps the two sources
                // unioned.
                auto & info = dirtyZones[entry.zone];
                info.dirty = true;
                for (auto & f : entry.files)
                    info.dirtyFiles.insert(f);
            }
            return;
        }

        if (!isTectonixSourceAvailable(state))
            return;

        // Get sparse checkout roots (zone IDs)
        auto & sparseRoots = getTectonixSparseCheckoutRoots(state);
        if (sparseRoots.empty())
            return;

        // Get manifest (uses cached parsed JSON)
        const nlohmann::json * manifest;
        try {
            manifest = &getManifestJson(state);
        } catch (nlohmann::json::parse_error & e) {
            warn("failed to parse manifest for dirty zone detection: %s", e.what());
            return;
        } catch (Error &) {
            // Manifest file not available (e.g., not in world repo)
            return;
        }

        // Build map of zone ID -> zone path for sparse roots only
        std::map<std::string, std::string> zoneIdToPath;
        for (auto & [path, value] : manifest->items()) {
            if (!value.contains("id") || !value.at("id").is_string()) {
                warn("zone '%s' in manifest has missing or non-string 'id' field", path);
                continue;
            }
            auto & id = value.at("id").get_ref<const std::string &>();
            if (sparseRoots.count(id))
                zoneIdToPath[id] = path;
        }

        // Initialize all sparse-checked-out zones as not dirty
        for (auto & [zoneId, zonePath] : zoneIdToPath) {
            dirtyZones[zonePath] = {};
        }

        // Create git command environment with environment variables
        // GIT_DIR/GIT_WORK_TREE/GIT_COMMON_DIR removed since they affect
        // git repository discovery
        StringMap gitEnvironment = getEnv();
        gitEnvironment.erase("GIT_DIR");
        gitEnvironment.erase("GIT_WORK_TREE");
        gitEnvironment.erase("GIT_COMMON_DIR");

        // Get dirty files via git status with -z for NUL-separated output
        // This handles filenames with special characters correctly
        auto checkoutPath = state.settings.tectonixCheckoutPath.get();
        auto [gitStatusCode, gitStatusOutput] = runProgram(
            {.program = "git",
             .args = {"-C", checkoutPath, "status", "--porcelain", "-z"},
             .environment = gitEnvironment});
        if (!statusOk(gitStatusCode)) {
            // If git status fails, treat all zones as clean (fallback)
            // This ensures call_once completes and we don't retry with partial state
            warn(
                "failed to get git status for dirty zone detection in '%s': program 'git' %s; treating all zones as clean",
                checkoutPath,
                statusToString(gitStatusCode));
            return;
        }

        // Parse NUL-separated output
        // Format with -z: XY SP path NUL [orig-path NUL for renames/copies]
        size_t pos = 0;
        while (pos < gitStatusOutput.size()) {
            // Find the next NUL
            auto nulPos = gitStatusOutput.find('\0', pos);
            if (nulPos == std::string::npos)
                break;

            auto entry = gitStatusOutput.substr(pos, nulPos - pos);
            pos = nulPos + 1;

            // Git porcelain format: "XY PATH" where XY is 2-char status, then space, then path
            // Minimum valid entry is "X  P" (4 chars): status + space + 1-char path
            if (entry.size() < 4)
                continue;

            // XY is first 2 chars, then space, then path
            char xy0 = entry[0];
            std::string rawPath = entry.substr(3);

            // Collect paths to check - destination path is always included
            std::vector<std::string> pathsToCheck;
            pathsToCheck.push_back("/" + rawPath);

            // For renames (R) and copies (C), also process the original path
            // Both source and destination zones should be marked dirty
            if (xy0 == 'R' || xy0 == 'C') {
                auto nextNul = gitStatusOutput.find('\0', pos);
                if (nextNul != std::string::npos) {
                    auto origPath = gitStatusOutput.substr(pos, nextNul - pos);
                    pathsToCheck.push_back("/" + origPath);
                    pos = nextNul + 1;
                }
            }

            for (const auto & filePath : pathsToCheck) {
                for (auto & [zonePath, info] : dirtyZones) {
                    auto normalized = "/" + normalizeZonePath(zonePath);
                    if (hasPrefix(filePath, normalized + "/") || filePath == normalized) {
                        info.dirty = true;
                        info.dirtyFiles.insert(filePath.substr(1));
                        break;
                    }
                }
            }
        }

        size_t dirtyCount = 0;
        for (const auto & [_, info] : dirtyZones)
            if (info.dirty)
                dirtyCount++;
        debug("computed dirty zones: %d of %d zones are dirty", dirtyCount, dirtyZones.size());
    });
    return accessorData(state)->tectonixDirtyZones;
}

// Path to the tectonix manifest file within the world repository
static constexpr std::string_view TECTONIX_MANIFEST_PATH = "/.meta/manifest.json";

const std::string & getManifestContent(const EvalState & state)
{
    // Cached for the lifetime of evaluation. This is intentional: evaluation is
    // bound to a specific git SHA (tectonix-git-sha), so the manifest content is
    // immutable for this EvalState instance.
    std::call_once(accessorData(state)->tectonixManifestFlag, [&state]() {
        auto fullPath = CanonPath(TECTONIX_MANIFEST_PATH);

        // Mode A (`tec <cmd>`, materialized checkout): the working tree is the source of
        // truth and may carry uncommitted manifest edits (a zone added/removed in this
        // sandbox), so read the local file — never a stale committed copy.
        if (isTectonixSourceAvailable(state)) {
            auto manifestPath =
                std::filesystem::path(state.settings.tectonixCheckoutPath.get()) / ".meta" / "manifest.json";
            if (std::filesystem::exists(manifestPath)) {
                accessorData(state)->tectonixManifestContent = readFile(manifestPath);
                debug("loaded manifest from checkout: %s", manifestPath.string());
                return;
            }
        }

        // Mode B (`tec --ref`, no checkout): manifest metadata is an ordinary immutable
        // file in the FUSE projection. W-000000 is the reserved manifest pseudo-zone; it
        // follows the same workspace visibility as the root checkout and needs no socket.
        if (!state.settings.tectonixWorldtreeSocket.get().empty()) {
            auto manifestPath = worldtreeRevisionRoot(state.settings) / "W-000000" / "manifest.json";
            std::error_code ec;
            if (!std::filesystem::is_regular_file(manifestPath, ec))
                throwHistoricalWorldManifestError(state.settings, ec ? ec.message() : "file does not exist");
            try {
                accessorData(state)->tectonixManifestContent = readFile(manifestPath);
            } catch (const Error & e) {
                throwHistoricalWorldManifestError(state.settings, e.what());
            }
            debug("loaded manifest from immutable worldtree view: %s", manifestPath.string());
            return;
        }

        // Socket unset (plain local eval): read the committed manifest via libgit2.
        auto accessor = getWorldGitAccessor(state);
        if (!accessor->pathExists(fullPath))
            throw Error("manifest.json does not exist at %s in world", TECTONIX_MANIFEST_PATH);

        accessorData(state)->tectonixManifestContent = accessor->readFile(fullPath);
        debug("loaded manifest from git at %s", fullPath);
    });
    return accessorData(state)->tectonixManifestContent;
}

const nlohmann::json & getManifestJson(const EvalState & state)
{
    std::call_once(accessorData(state)->tectonixManifestJsonFlag, [&state]() {
        try {
            accessorData(state)->tectonixManifestJson =
                std::make_unique<nlohmann::json>(nlohmann::json::parse(getManifestContent(state)));
        } catch (const nlohmann::json::parse_error & e) {
            if (!state.settings.tectonixWorldtreeSocket.get().empty() && !isTectonixSourceAvailable(state))
                throwHistoricalWorldManifestError(state.settings, e.what());
            throw;
        }
    });
    return *accessorData(state)->tectonixManifestJson;
}

static StorePath mountLegacyTectonixZoneByTreeSha(EvalState & state, const Hash & treeSha, std::string_view zonePath);
static StorePath worldtreeMountAccessor(
    EvalState & state, const Hash & treeSha, std::string_view zonePath, ref<SourceAccessor> accessor);
static StorePath getLegacyTectonixZoneFromCheckout(
    EvalState & state, std::string_view zonePath, const boost::unordered_flat_set<std::string> * dirtyFiles = nullptr);

StorePath getLegacyTectonixZoneStorePath(EvalState & state, std::string_view zonePath)
{
    // A worldtree sandbox has two source regimes but only one filesystem accessor:
    // the mutable root checkout path for ordinary evaluation, or the immutable
    // commit/zone path for --ref. Only the former needs control RPCs for its dirty
    // frontier identity.
    if (!state.settings.tectonixWorldtreeSocket.get().empty()) {
        if (isTectonixSourceAvailable(state)) {
            auto control = worldtreeControlConn(state);
            if (!control)
                throw Error("worldtree: mutable checkout has no control connection");
            auto treeSha = control->zoneTreeSha(zonePath);
            if (!treeSha)
                throw Error("worldtree: zone '%s' is absent or outside this workspace's visibility scope", zonePath);
            auto fullPath =
                std::filesystem::path(state.settings.tectonixCheckoutPath.get()) / normalizeZonePath(zonePath);
            if (!std::filesystem::is_directory(fullPath))
                throw Error("worldtree: zone '%s' is not materialized at '%s'", zonePath, fullPath.string());
            return worldtreeMountAccessor(state, *treeSha, zonePath, makeFSSourceAccessor(fullPath));
        }

        auto manifestIt = getManifestJson(state).find(std::string(zonePath));
        if (manifestIt == getManifestJson(state).end() || !manifestIt->is_object() || !manifestIt->contains("id")
            || !manifestIt->at("id").is_string())
            throw Error("worldtree: zone '%s' is absent from the visible manifest", zonePath);
        auto zoneId = requireWorldtreeZoneId(manifestIt->at("id").get<std::string>());
        auto fullPath = worldtreeRevisionRoot(state.settings) / zoneId;
        if (!std::filesystem::is_directory(fullPath))
            throw Error("worldtree: immutable zone '%s' is unavailable at '%s'", zonePath, fullPath.string());
        auto treeSha = readWorldtreeTreeOid(fullPath);
        return worldtreeMountAccessor(state, treeSha, zonePath, makeFSSourceAccessor(fullPath));
    }

    // Check dirty status using original zonePath (with // prefix) since
    // tectonixDirtyZones keys come directly from manifest with // prefix
    const EvalState::ZoneDirtyInfo * dirtyInfo = nullptr;
    if (isTectonixSourceAvailable(state)) {
        auto & dirtyZones = getTectonixDirtyZones(state);
        auto it = dirtyZones.find(std::string(zonePath));
        if (it != dirtyZones.end() && it->second.dirty)
            dirtyInfo = &it->second;
    }

    if (dirtyInfo) {
        debug("getLegacyTectonixZoneStorePath: %s is dirty, using checkout", zonePath);
        return getLegacyTectonixZoneFromCheckout(state, zonePath, &dirtyInfo->dirtyFiles);
    }

    // Clean zone: get tree SHA
    auto treeSha = getWorldTreeSha(state, zonePath);

    if (!state.settings.lazyTrees) {
        debug("getLegacyTectonixZoneStorePath: %s clean, eager copy from git (tree %s)", zonePath, treeSha.gitRev());
        auto repo = getWorldRepo(state);
        auto commitHash = Hash::parseNonSRIUnprefixed(requireTectonixGitSha(state), HashAlgorithm::SHA1);
        auto opts = makeZoneAccessorOptions(repo, commitHash, normalizeZonePath(zonePath));
        auto accessor = repo->getAccessor(treeSha, opts, "zone");

        std::string name = "zone-" + sanitizeZoneNameForStore(zonePath);
        auto storePath = fetchToStore(
            state.fetchSettings, *state.store, SourcePath(accessor, CanonPath::root), FetchMode::Copy, name);

        state.allowPath(storePath);
        return storePath;
    }

    debug("getLegacyTectonixZoneStorePath: %s clean, lazy mount (tree %s)", zonePath, treeSha.gitRev());
    return mountLegacyTectonixZoneByTreeSha(state, treeSha, zonePath);
}

static StorePath
worldtreeMountAccessor(EvalState & state, const Hash & treeSha, std::string_view zonePath, ref<SourceAccessor> accessor)
{
    std::string name = "zone-" + sanitizeZoneNameForStore(zonePath);

    if (!state.settings.lazyTrees) {
        // Eager: copy the zone content into the store now (content-addressed by content).
        auto storePath = fetchToStore(
            state.fetchSettings, *state.store, SourcePath(accessor, CanonPath::root), FetchMode::Copy, name);
        state.allowPath(storePath);
        return storePath;
    }

    // Lazy-trees: mount at a virtual store path, deduplicated by the daemon's working-tree
    // oid so a zone evaluated twice in one EvalState mounts once (same shape as
    // mountLegacyTectonixZoneByTreeSha — the two share tectonixZoneCache_'s tree-oid
    // keyspace).
    {
        auto cache = accessorData(state)->tectonixZoneCache_.readLock();
        if (auto it = cache->find(treeSha); it != cache->end())
            return it->second;
    }

    auto storePath = StorePath::random(name);

    auto cache = accessorData(state)->tectonixZoneCache_.lock();
    if (auto it = cache->find(treeSha); it != cache->end())
        return it->second;

    state.storeFS->mount(CanonPath(state.store->printStorePath(storePath)), accessor);
    state.allowPath(storePath);
    cache->emplace(treeSha, storePath);

    debug(
        "worldtree: mounted zone %s (tree %s) at %s",
        zonePath,
        treeSha.gitRev(),
        state.store->printStorePath(storePath));

    return storePath;
}

static StorePath mountLegacyTectonixZoneByTreeSha(EvalState & state, const Hash & treeSha, std::string_view zonePath)
{
    // Double-checked locking pattern for concurrent zone mounting:
    // 1. Read lock check (fast path - allows concurrent readers)
    {
        auto cache = accessorData(state)->tectonixZoneCache_.readLock();
        auto it = cache->find(treeSha);
        if (it != cache->end()) {
            debug("zone cache hit for tree %s", treeSha.gitRev());
            return it->second;
        }
    } // Read lock released

    // 2. Write lock check (catch races between read unlock and write lock)
    {
        auto cache = accessorData(state)->tectonixZoneCache_.lock();
        auto it = cache->find(treeSha);
        if (it != cache->end()) {
            debug("zone cache hit for tree %s (after lock upgrade)", treeSha.gitRev());
            return it->second;
        }
    } // Write lock released - expensive work happens without holding lock

    // 3. Perform expensive git operations without holding lock.
    // This allows concurrent mounts of different zones. Multiple threads may
    // race to mount the same zone, but we check again before inserting.
    auto repo = getWorldRepo(state);
    auto commitHash = Hash::parseNonSRIUnprefixed(requireTectonixGitSha(state), HashAlgorithm::SHA1);
    auto opts = makeZoneAccessorOptions(repo, commitHash, std::string(zonePath));
    auto accessor = repo->getAccessor(treeSha, opts, "zone");

    // Generate name from zone path (sanitized for store path requirements)
    std::string name = "zone-" + sanitizeZoneNameForStore(zonePath);

    // Create virtual store path
    auto storePath = StorePath::random(name);

    // 4. Re-acquire write lock and check again before mounting
    auto cache = accessorData(state)->tectonixZoneCache_.lock();
    auto it = cache->find(treeSha);
    if (it != cache->end()) {
        // Another thread mounted while we were working - use their result
        debug("zone cache hit for tree %s (after work)", treeSha.gitRev());
        return it->second;
    }

    // Mount accessor at this path first, then allow the path.
    // This order ensures we don't leave allowed paths without mounts on exception.
    state.storeFS->mount(CanonPath(state.store->printStorePath(storePath)), accessor);
    state.allowPath(storePath);

    // Insert into cache (we hold the lock, so this will succeed)
    cache->emplace(treeSha, storePath);

    debug("mounted zone %s (tree %s) at %s", zonePath, treeSha.gitRev(), state.store->printStorePath(storePath));

    return storePath;
}

/**
 * Overlays dirty files from disk on top of a clean git tree accessor.
 * Serves only the legacy tectonix zone builtins; the tracked Tecnix
 * evaluation path uses TecnixSourceAccessor above.
 */
struct DirtyOverlaySourceAccessor : SourceAccessor
{
    ref<SourceAccessor> base, disk;
    boost::unordered_flat_set<std::string> dirtyFiles, dirtyDirs;

    DirtyOverlaySourceAccessor(
        ref<SourceAccessor> base, ref<SourceAccessor> disk, boost::unordered_flat_set<std::string> && dirtyFiles)
        : base(base)
        , disk(disk)
        , dirtyFiles(std::move(dirtyFiles))
    {
        for (auto & f : this->dirtyFiles) {
            for (auto p = CanonPath(f); !p.isRoot();) {
                p.pop();
                if (!dirtyDirs.insert(p.rel().empty() ? "" : std::string(p.rel())).second)
                    break;
            }
        }
    }

    bool isDirty(const CanonPath & path)
    {
        return dirtyFiles.contains(std::string(path.rel()));
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        if (path.isRoot())
            return base->maybeLstat(path);
        if (isDirty(path))
            return disk->maybeLstat(path);
        auto s = base->maybeLstat(path);
        if (s || !dirtyDirs.contains(std::string(path.rel())))
            return s;
        return disk->maybeLstat(path);
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        return (isDirty(path) ? disk : base)->readFile(path, sink, sizeCallback);
    }

    std::string readLink(const CanonPath & path) override
    {
        return (isDirty(path) ? disk : base)->readLink(path);
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return (isDirty(path) ? disk : base)->getPhysicalPath(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto rel = path.isRoot() ? "" : std::string(path.rel());
        if (!path.isRoot() && !dirtyDirs.contains(rel))
            return base->readDirectory(path);

        DirEntries entries;
        try {
            entries = base->readDirectory(path);
        } catch (...) {
        }

        auto prefix = rel.empty() ? "" : rel + "/";
        for (auto & f : dirtyFiles) {
            if (!f.starts_with(prefix))
                continue;
            auto rest = std::string_view(f).substr(prefix.size());
            if (rest.find('/') != std::string_view::npos)
                continue;
            auto stat = disk->maybeLstat(path / rest);
            if (stat)
                entries[std::string(rest)] = stat->type;
            else
                entries.erase(std::string(rest));
        }
        for (auto & d : dirtyDirs) {
            if (!d.starts_with(prefix))
                continue;
            auto rest = std::string_view(d).substr(prefix.size());
            if (rest.find('/') != std::string_view::npos || rest.empty())
                continue;
            if (!entries.count(std::string(rest)))
                entries[std::string(rest)] = Type::tDirectory;
        }
        return entries;
    }
};

static StorePath getLegacyTectonixZoneFromCheckout(
    EvalState & state, std::string_view zonePath, const boost::unordered_flat_set<std::string> * dirtyFiles)
{
    auto zone = normalizeZonePath(zonePath);
    std::string name = "zone-" + sanitizeZoneNameForStore(zonePath);
    auto checkoutPath = state.settings.tectonixCheckoutPath.get();
    auto fullPath = std::filesystem::path(checkoutPath) / zone;

    auto makeDirtyAccessor = [&]() -> ref<SourceAccessor> {
        auto repo = getWorldRepo(state);
        auto commitHash = Hash::parseNonSRIUnprefixed(requireTectonixGitSha(state), HashAlgorithm::SHA1);
        auto zoneOpts = makeZoneAccessorOptions(repo, commitHash, zone);
        auto baseAccessor = repo->getAccessor(getWorldTreeSha(state, zone), zoneOpts, "zone");
        boost::unordered_flat_set<std::string> zoneDirtyFiles;
        if (dirtyFiles) {
            auto zonePrefix = zone + "/";
            for (auto & f : *dirtyFiles)
                if (f.starts_with(zonePrefix))
                    zoneDirtyFiles.insert(f.substr(zonePrefix.size()));
        }
        return make_ref<DirtyOverlaySourceAccessor>(
            baseAccessor, makeFSSourceAccessor(fullPath), std::move(zoneDirtyFiles));
    };

    if (!state.settings.lazyTrees) {
        auto accessor = makeDirtyAccessor();
        auto storePath = fetchToStore(
            state.fetchSettings, *state.store, SourcePath(accessor, CanonPath::root), FetchMode::Copy, name);
        state.allowPath(storePath);
        return storePath;
    }

    {
        auto cache = accessorData(state)->tectonixCheckoutZoneCache_.readLock();
        auto it = cache->find(std::string(zonePath));
        if (it != cache->end())
            return it->second;
    }

    auto cache = accessorData(state)->tectonixCheckoutZoneCache_.lock();
    auto it = cache->find(std::string(zonePath));
    if (it != cache->end())
        return it->second;

    if (!std::filesystem::exists(fullPath))
        throw Error("zone '%s' not found in checkout at '%s'", zonePath, fullPath.string());

    auto storePath = StorePath::random(name);
    state.storeFS->mount(CanonPath(state.store->printStorePath(storePath)), makeDirtyAccessor());
    state.allowPath(storePath);
    cache->emplace(std::string(zonePath), storePath);
    return storePath;
}

} // namespace nix
