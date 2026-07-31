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

/** Resolve a checkout's HEAD to a commit SHA (throws if it has none). */
std::string resolveCheckoutHeadRev(const std::string & checkoutPath);
StorePath mountTecnixRepoAccessor(EvalState & state);
std::string getTecnixRepoPath(EvalState & state, std::string_view repoRelPath);

} // namespace nix
