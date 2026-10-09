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
/**
 * The checkout's dirty repo-relative paths (modified, added, deleted, both
 * sides of a rename, and every untracked file), from one `git status` per
 * evaluation that the repo accessor's overlay and dirty-zone detection share.
 * Throws if git status fails.
 */
const std::vector<std::string> & getTecnixCheckoutDirtyPaths(const EvalState & state);
const std::string & getManifestContent(const EvalState & state);
const nlohmann::json & getManifestJson(const EvalState & state);
StorePath getLegacyTectonixZoneStorePath(EvalState & state, std::string_view zonePath);
ref<SourceAccessor> getTecnixRepoAccessor(EvalState & state);
/**
 * `.meta/manifest.json` as the Tecnix repo accessor serves it (the view that
 * also fingerprints Tecnix dependencies), parsed once per evaluation. The
 * read records nothing: callers record the part of the manifest they observe.
 */
const nlohmann::json & getTecnixManifestJson(EvalState & state);

/** Resolve a checkout's HEAD to a commit SHA (throws if it has none). */
std::string resolveCheckoutHeadRev(const std::string & checkoutPath);
StorePath mountTecnixRepoAccessor(EvalState & state);
std::string getTecnixRepoPath(EvalState & state, std::string_view repoRelPath);

} // namespace nix
