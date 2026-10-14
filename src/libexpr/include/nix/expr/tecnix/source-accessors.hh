#pragma once
///@file

#include "nix/expr/eval.hh"

namespace nix {

void configureTectonixContext(EvalState & state, std::string gitDir, std::string rev, std::string checkoutPath);
ref<GitRepo> getWorldRepo(const EvalState & state);
const std::string & requireTectonixGitSha(const EvalState & state);
ref<SourceAccessor> getWorldGitAccessor(const EvalState & state);
Hash getWorldTreeSha(const EvalState & state, std::string_view worldPath);
bool isTectonixSourceAvailable(const EvalState & state);
const std::set<std::string> & getTectonixSparseCheckoutRoots(const EvalState & state);
const std::map<std::string, EvalState::ZoneDirtyInfo> & getTectonixDirtyZones(const EvalState & state);
const std::string & getManifestContent(const EvalState & state);
const nlohmann::json & getManifestJson(const EvalState & state);
StorePath getLegacyTectonixZoneStorePath(EvalState & state, std::string_view zonePath);
ref<SourceAccessor> getTecnixRepoAccessor(EvalState & state);

/**
 * The regular file at `path`, read without recording a Tecnix dependency on
 * it, or nullopt if there is no regular file there. For a caller that records
 * its own, finer-grained observation of the file instead (`tecnixQueryFile`,
 * and the cache validator checking such an observation). Through a Tecnix
 * repo accessor the read is genuinely unrecorded; any other accessor records
 * whatever its reads record.
 */
std::optional<std::string> readSourceFileUnrecorded(SourceAccessor & accessor, const CanonPath & path);

/**
 * Where `path` lies in the Tecnix repo, if it is under the repo accessor's
 * mount (`mountTecnixRepoAccessor`): the repo-relative path, which is what
 * tracked reads of it record and what fingerprints are keyed by. Nullopt
 * for any other path, and when no repo accessor has been mounted.
 */
std::optional<CanonPath> tecnixRepoRelativePath(EvalState & state, const SourcePath & path);

/** Resolve a checkout's HEAD to a commit SHA (throws if it has none). */
std::string resolveCheckoutHeadRev(const std::string & checkoutPath);
StorePath mountTecnixRepoAccessor(EvalState & state);
std::string getTecnixRepoPath(EvalState & state, std::string_view repoRelPath);

} // namespace nix
