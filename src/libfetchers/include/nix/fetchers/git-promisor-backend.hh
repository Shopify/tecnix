#pragma once

#include <git2/odb.h>
#include <filesystem>
#include <string>

namespace nix {

/**
 * Create a custom libgit2 ODB backend that fetches missing objects
 * from a promisor remote using `git fetch` (CLI).
 *
 * This enables Tecnix to work with blobless partial clones
 * (--filter=blob:none). When git_object_lookup fails to find an
 * object in the local ODB, this backend accumulates the missing
 * OIDs. A subsequent call to git_promisor_backend_flush() fetches
 * all accumulated OIDs in a single batch, then the caller retries
 * the lookup.
 *
 * The backend is installed at priority 0 (lowest), so it is only
 * consulted after all other backends (pack, mempack) have failed
 * to find the object.
 *
 * @param out       Pointer to receive the created backend, or nullptr
 *                  if the repository is not a partial clone.
 * @param repoPath  Path to the git repository (.git directory).
 * @param gitBin    Path to the git binary (default: "git").
 * @return 0 on success, GIT_ERROR on failure.
 */
int git_odb_backend_promisor(
    git_odb_backend ** out,
    const std::filesystem::path & repoPath,
    const std::string & gitBin = "git");

/**
 * Flush all pending object fetches accumulated by the promisor backend.
 *
 * This fetches all OIDs that were requested via read() but not yet
 * retrieved from the remote, in a single `git fetch` invocation.
 * After flushing, the ODB is refreshed so the pack backend picks up
 * the newly-fetched packfile.
 *
 * Typical usage:
 *   1. git_object_lookup() fails (GIT_ENOTFOUND) for a missing blob
 *   2. Caller calls git_promisor_backend_flush() to batch-fetch
 *   3. Caller retries the lookup — pack backend now has the object
 *
 * Thread-safe: multiple threads can call read() concurrently to
 * accumulate OIDs, then one thread calls flush to fetch them all.
 *
 * @param backend   The promisor ODB backend (from git_odb_backend_promisor).
 *                  If null, this is a no-op.
 * @return true if objects were fetched, false if nothing was pending.
 */
bool git_promisor_backend_flush(git_odb_backend * backend);

/**
 * Check whether a git repository is a partial clone with a promisor
 * remote configured.
 *
 * Detects promisor remotes by looking for .promisor packfile sentinel
 * files in the objects/pack directory, or by checking the git config
 * for remote.<name>.promisor = true.
 *
 * @param repoPath  Path to the git repository (.git directory).
 * @return true if the repo has a promisor remote configured.
 */
bool isPromisorRepo(const std::filesystem::path & repoPath);

} // namespace nix
