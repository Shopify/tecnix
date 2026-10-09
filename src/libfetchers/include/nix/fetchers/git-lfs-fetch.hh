#pragma once
///@file

#include "nix/util/canon-path.hh"
#include "nix/util/serialise.hh"
#include "nix/util/url.hh"

#include <git2/repository.h>

#include <nlohmann/json_fwd.hpp>

namespace nix::lfs {

/**
 * git-lfs pointer
 * @see https://github.com/git-lfs/git-lfs/blob/2ef4108/docs/spec.md
 */
struct Pointer
{
    std::string oid; // git-lfs managed object id. you give this to the lfs server
                     // for downloads
    size_t size;     // in bytes
};

struct Fetch
{
    const git_repository * repo;
    git_oid rev;
    nix::ParsedURL url;
    std::string attrPathPrefix;

    Fetch(git_repository * repo, git_oid rev, std::string attrPathPrefix = "");
    bool shouldFetch(const CanonPath & path) const;
    void fetch(
        const std::string & content,
        const CanonPath & pointerFilePath,
        Sink & sink,
        std::function<void(uint64_t)> sizeCallback) const;
    std::vector<nlohmann::json> fetchUrls(const std::vector<Pointer> & pointers) const;
};

struct LfsApiInfo
{
    std::string endpoint;
    std::optional<std::string> authHeader;
};

/**
 * Derive the LFS API endpoint of a plain Git remote URL the way git-lfs
 * does: `<remote>.git/info/lfs`.
 * @see https://github.com/git-lfs/git-lfs/blob/main/docs/api/server-discovery.md
 */
ParsedURL lfsEndpointForRemote(ParsedURL url);

/**
 * Resolve the authentication for an LFS API endpoint (as produced by
 * `lfsEndpointForRemote()` or configured via `lfs.url`): via
 * `git-lfs-authenticate` for SSH remotes, and `git credential fill`
 * otherwise. The endpoint is used as-is.
 */
LfsApiInfo getLfsApi(ParsedURL url);

} // namespace nix::lfs
