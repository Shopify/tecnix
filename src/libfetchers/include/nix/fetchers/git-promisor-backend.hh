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
 * object in the local ODB, this backend shells out to git to
 * fetch the object from the promisor remote, then the caller can
 * re-read it from the (now-populated) local pack files.
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
