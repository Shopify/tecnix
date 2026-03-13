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
#include <vector>

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
 *
 * ## Batching strategy
 *
 * Instead of fetching each missing blob individually (N round-trips),
 * the backend accumulates OIDs of missing objects. When the caller
 * detects a lookup failure, it calls git_promisor_backend_flush() which
 * fetches ALL accumulated OIDs in a single `git fetch` invocation,
 * then the caller retries.
 *
 * With parallel evaluation (builtins.parallel), multiple threads hit
 * missing blobs concurrently. Their OIDs accumulate in pendingOids
 * before any thread triggers a flush. The first thread to flush
 * collects everyone's pending OIDs in one batch.
 */
struct PromisorBackend
{
    git_odb_backend parent; // Must be first member

    std::filesystem::path repoPath;
    std::string gitBin;

    std::mutex mutex;

    /**
     * OIDs that have been requested via read() but not yet fetched.
     * Accumulated until flushPendingFetches() is called.
     */
    std::set<git_oid, GitOidLess> pendingOids;

    /**
     * OIDs that have already been fetched (or attempted).
     * Prevents re-accumulating OIDs on retry after flush.
     */
    std::set<git_oid, GitOidLess> fetchedOids;

    /**
     * Fetch a batch of objects from the promisor remote in a single
     * `git fetch` invocation.
     *
     * @return GIT_OK on success, GIT_ENOTFOUND if fetch fails.
     */
    int fetchObjects(const std::vector<git_oid> & oids)
    {
        if (oids.empty())
            return GIT_OK;

        Strings args = {
            "-C",
            repoPath.string(),
            "--git-dir",
            ".",
            "-c",
            "fetch.negotiationAlgorithm=noop",
            "fetch",
            "origin",
        };

        // Append all OIDs as refspecs
        for (const auto & oid : oids) {
            char oidStr[65];
            git_oid_tostr(oidStr, sizeof(oidStr), &oid);
            args.push_back(oidStr);
        }

        debug("promisor backend: batch-fetching %d objects from %s", oids.size(), repoPath.string());

        try {
            auto [status, output] = runProgram(RunOptions{
                .program = gitBin,
                .args = args,
                .mergeStderrToStdout = true,
            });
            if (status != 0) {
                debug("promisor backend: git fetch exited with status %d: %s", status, output);
                return GIT_ENOTFOUND;
            }
            return GIT_OK;
        } catch (const std::exception & e) {
            printError("promisor backend: failed to fetch batch: %s", e.what());
            return GIT_ENOTFOUND;
        }
    }

    /**
     * Flush all pending OIDs: fetch them in one batch, then refresh
     * the ODB so the pack backend picks up the new packfile.
     *
     * @return true if objects were fetched, false if nothing was pending.
     */
    bool flushPendingFetches()
    {
        std::vector<git_oid> toFetch;

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pendingOids.empty())
                return false;
            toFetch.assign(pendingOids.begin(), pendingOids.end());
            // Move all pending → fetched (even before the actual fetch,
            // to prevent re-accumulation if read() is called concurrently)
            for (const auto & oid : toFetch)
                fetchedOids.insert(oid);
            pendingOids.clear();
        }

        fetchObjects(toFetch);

        // Refresh the ODB so the pack backend picks up new packfiles.
        git_odb * odb = parent.odb;
        if (odb)
            git_odb_refresh(odb);

        return true;
    }
};

/**
 * libgit2 ODB backend callback: read an object.
 *
 * In batch mode: accumulates the OID in pendingOids and returns
 * GIT_ENOTFOUND. The caller is expected to call
 * git_promisor_backend_flush() then retry.
 */
static int promisor_backend_read(
    void ** data_p, size_t * len_p, git_object_t * type_p, git_odb_backend * _backend, const git_oid * oid)
{
    (void) data_p;
    (void) len_p;
    (void) type_p;

    auto * backend = reinterpret_cast<PromisorBackend *>(_backend);

    std::lock_guard<std::mutex> lock(backend->mutex);

    // If we already tried to fetch this OID, don't re-accumulate.
    // The pack backend should have it if the fetch succeeded.
    // If it doesn't, the object genuinely doesn't exist.
    if (backend->fetchedOids.count(*oid))
        return GIT_ENOTFOUND;

    // Accumulate for batch fetch
    backend->pendingOids.insert(*oid);
    return GIT_ENOTFOUND;
}

/**
 * libgit2 ODB backend callback: check if an object exists.
 *
 * Returns 0 ("no") — we don't proactively fetch for existence checks.
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
    auto packDir = repoPath / "objects" / "pack";
    if (std::filesystem::exists(packDir)) {
        std::error_code ec;
        for (const auto & entry : std::filesystem::directory_iterator(packDir, ec)) {
            if (entry.path().extension() == ".promisor")
                return true;
        }
    }

    // Strategy 2: Check git config for remote.<name>.promisor = true.
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

bool git_promisor_backend_flush(git_odb_backend * backend)
{
    if (!backend)
        return false;
    return reinterpret_cast<PromisorBackend *>(backend)->flushPendingFetches();
}

int git_odb_backend_promisor(
    git_odb_backend ** out, const std::filesystem::path & repoPath, const std::string & gitBin)
{
    *out = nullptr;

    if (!isPromisorRepo(repoPath)) {
        return GIT_OK;
    }

    auto * backend = new PromisorBackend();
    std::memset(&backend->parent, 0, sizeof(git_odb_backend));

    backend->parent.version = GIT_ODB_BACKEND_VERSION;
    backend->parent.read = promisor_backend_read;
    backend->parent.exists = promisor_backend_exists;
    backend->parent.free = promisor_backend_free;

    backend->repoPath = repoPath;
    backend->gitBin = gitBin;

    debug("promisor backend: installed for repository %s", repoPath.string());

    *out = &backend->parent;
    return GIT_OK;
}

} // namespace nix
