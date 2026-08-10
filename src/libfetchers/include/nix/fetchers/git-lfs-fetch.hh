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

    /**
     * Whether `content` is shaped like a git-lfs pointer file.
     *
     * This is a cheap *necessary* condition for smudging, used to avoid the
     * expensive attribute lookup in `shouldFetch()` for the overwhelming
     * majority of blobs. It deliberately accepts malformed pointers, so that
     * `fetch()` can still warn about them.
     */
    static bool isPointerCandidate(std::string_view content);

    /**
     * Whether the `filter` attribute of `path` is `lfs`.
     *
     * Expensive: libgit2 redoes its attribute setup and rematches every
     * `.gitattributes` rule of every parent directory on each call, so this
     * costs O(number of rules) per path. Prefer `shouldFetch()`, which only
     * gets here for blobs that could actually be a pointer.
     */
    bool hasLfsFilterAttribute(const CanonPath & path) const;

    /**
     * Whether the blob at `path` holding `content` must be smudged.
     */
    bool shouldFetch(const CanonPath & path, std::string_view content) const;
    void fetch(
        const std::string & content,
        const CanonPath & pointerFilePath,
        StringSink & sink,
        std::function<void(uint64_t)> sizeCallback) const;
    std::vector<nlohmann::json> fetchUrls(const std::vector<Pointer> & pointers) const;
};

} // namespace nix::lfs
