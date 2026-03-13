#include "nix/fetchers/git-promisor-backend.hh"
#include "nix/util/processes.hh"
#include "nix/util/logging.hh"

#include <git2/config.h>
#include <git2/errors.h>
#include <git2/odb.h>
#include <git2/odb_backend.h>
#include <git2/sys/odb_backend.h>
#include <git2/types.h>

#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>

namespace nix {

// Comparator for git_oid to use in std::set
struct GitOidLess
{
    bool operator()(const git_oid & a, const git_oid & b) const
    {
        return git_oid_cmp(&a, &b) < 0;
    }
};

/**
 * Custom ODB backend that fetches missing objects from a promisor remote.
 *
 * This struct uses C-style inheritance: `parent` must be the first member
 * so that a pointer to PromisorBackend can be cast to git_odb_backend*.
 */
struct PromisorBackend
{
    git_odb_backend parent; // Must be first member

    std::filesystem::path repoPath;
    std::string gitBin;

    /**
     * Track OIDs we've already attempted to fetch in this backend instance.
     * Prevents infinite recursion when git_odb_read() re-invokes our read()
     * callback after we fetch and refresh.
     */
    std::mutex fetchedMutex;
    std::set<git_oid, GitOidLess> alreadyFetched;
};

/**
 * Fetch a single object from the promisor remote using the git CLI.
 *
 * Uses `git -C <repo> fetch origin <oid>` which triggers the promisor
 * remote's lazy fetch mechanism for the specific object.
 */
static int fetchObject(PromisorBackend * backend, const git_oid * oid)
{
    // GIT_OID_SHA1_HEXSIZE is 40; use 64+1 to also cover SHA-256 if enabled.
    char oidStr[65];
    git_oid_tostr(oidStr, sizeof(oidStr), oid);

    debug("promisor backend: fetching object %s from %s", oidStr, backend->repoPath.string());

    try {
        // Use `git fetch origin <oid>` which handles promisor remote
        // negotiation, authentication, and protocol details.
        //
        // We set fetch.negotiationAlgorithm=noop to skip negotiation
        // overhead since we know exactly which object we want and the
        // promisor remote is guaranteed to have it.
        auto [status, output] = runProgram(RunOptions{
            .program = backend->gitBin,
            .args =
                {
                    "-C",
                    backend->repoPath.string(),
                    "--git-dir",
                    ".",
                    "-c",
                    "fetch.negotiationAlgorithm=noop",
                    "fetch",
                    "origin",
                    oidStr,
                },
            .mergeStderrToStdout = true,
        });
        if (status != 0) {
            debug("promisor backend: git fetch for %s exited with status %d: %s", oidStr, status, output);
            return GIT_ENOTFOUND;
        }
        return GIT_OK;
    } catch (const std::exception & e) {
        printError("promisor backend: failed to fetch %s: %s", oidStr, e.what());
        return GIT_ENOTFOUND;
    }
}

/**
 * libgit2 ODB backend callback: read an object.
 *
 * Called when higher-priority backends (pack, mempack) fail to find an object.
 * Fetches the missing object from the promisor remote, refreshes the ODB,
 * then re-reads from the ODB (which will find it in the newly-fetched packfile).
 */
static int promisor_backend_read(
    void ** data_p, size_t * len_p, git_object_t * type_p, git_odb_backend * _backend, const git_oid * oid)
{
    auto * backend = reinterpret_cast<PromisorBackend *>(_backend);

    // Prevent infinite recursion: if we've already tried to fetch this oid,
    // return ENOTFOUND immediately. This happens when git_odb_read() below
    // re-invokes the entire backend stack including this backend.
    {
        std::lock_guard<std::mutex> lock(backend->fetchedMutex);
        if (backend->alreadyFetched.count(*oid))
            return GIT_ENOTFOUND;
        backend->alreadyFetched.insert(*oid);
    }

    // Fetch the object from the promisor remote
    if (fetchObject(backend, oid) != GIT_OK)
        return GIT_ENOTFOUND;

    // The parent.odb pointer is set by libgit2 when the backend is added
    // to an ODB via git_odb_add_backend(). Refresh the ODB so the pack
    // backend picks up the newly-fetched packfile.
    git_odb * odb = backend->parent.odb;
    if (!odb)
        return GIT_ERROR;

    git_odb_refresh(odb);

    // Re-read from the ODB. This will invoke all backends (including us),
    // but our alreadyFetched check above prevents recursion. The pack
    // backend should now find the object in the new packfile.
    git_odb_object * obj = nullptr;
    if (git_odb_read(&obj, odb, oid) != GIT_OK)
        return GIT_ENOTFOUND;

    // Copy data out — the caller (libgit2) owns the returned buffer.
    // Use git_odb_backend_data_alloc so libgit2 can properly free it.
    *len_p = git_odb_object_size(obj);
    *type_p = git_odb_object_type(obj);
    *data_p = git_odb_backend_data_alloc(_backend, *len_p);
    if (!*data_p) {
        git_odb_object_free(obj);
        return GIT_ERROR;
    }
    std::memcpy(*data_p, git_odb_object_data(obj), *len_p);
    git_odb_object_free(obj);
    return GIT_OK;
}

/**
 * libgit2 ODB backend callback: check if an object exists.
 *
 * We return 0 ("no") to avoid proactively fetching objects just for
 * existence checks. Objects are only fetched on actual read attempts.
 */
static int promisor_backend_exists(git_odb_backend * /* _backend */, const git_oid * /* oid */)
{
    return 0;
}

/**
 * libgit2 ODB backend callback: free the backend.
 */
static void promisor_backend_free(git_odb_backend * _backend)
{
    auto * backend = reinterpret_cast<PromisorBackend *>(_backend);
    delete backend;
}

bool isPromisorRepo(const std::filesystem::path & repoPath)
{
    // Strategy 1: Check for .promisor sentinel files.
    // When git fetches from a promisor remote, it creates a .promisor
    // file alongside each packfile received from that remote.
    auto packDir = repoPath / "objects" / "pack";
    if (std::filesystem::exists(packDir)) {
        std::error_code ec;
        for (const auto & entry : std::filesystem::directory_iterator(packDir, ec)) {
            if (entry.path().extension() == ".promisor")
                return true;
        }
    }

    // Strategy 2: Check git config for remote.<name>.promisor = true.
    // This handles cases where the promisor files might not exist yet
    // (e.g., freshly configured partial clone before first fetch).
    auto configPath = repoPath / "config";
    if (std::filesystem::exists(configPath)) {
        git_config * cfg = nullptr;
        if (git_config_open_ondisk(&cfg, configPath.string().c_str()) == GIT_OK) {
            git_config_iterator * it = nullptr;
            if (git_config_iterator_glob_new(&it, cfg, "^remote\\..*\\.promisor$") == GIT_OK) {
                git_config_entry * entry = nullptr;
                while (git_config_next(&entry, it) == GIT_OK) {
                    if (entry->value && std::string(entry->value) == "true") {
                        git_config_iterator_free(it);
                        git_config_free(cfg);
                        return true;
                    }
                }
                git_config_iterator_free(it);
            }
            git_config_free(cfg);
        }
    }

    return false;
}

int git_odb_backend_promisor(
    git_odb_backend ** out, const std::filesystem::path & repoPath, const std::string & gitBin)
{
    *out = nullptr;

    if (!isPromisorRepo(repoPath)) {
        // Not a partial clone — don't install the backend.
        // Returning GIT_OK with *out = nullptr signals "not needed".
        return GIT_OK;
    }

    auto * backend = new PromisorBackend();
    std::memset(&backend->parent, 0, sizeof(git_odb_backend));

    backend->parent.version = GIT_ODB_BACKEND_VERSION;
    backend->parent.read = promisor_backend_read;
    backend->parent.exists = promisor_backend_exists;
    backend->parent.free = promisor_backend_free;
    // read_prefix, read_header, write, etc. are left null.
    // libgit2 will fall back to read() for read_prefix/read_header.

    backend->repoPath = repoPath;
    backend->gitBin = gitBin;

    debug("promisor backend: installed for repository %s", repoPath.string());

    *out = &backend->parent;
    return GIT_OK;
}

} // namespace nix
