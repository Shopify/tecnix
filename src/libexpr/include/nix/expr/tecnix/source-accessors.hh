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

/**
 * `tectonix-world-input-paths`: `path` as a World input instead of a store copy, or nothing when
 * `path` is not clean committed World content (then the caller copies it as usual).
 *
 * `path` may be in the repo-wide World mount (a World path value) or inside a World input view
 * whose element is in `inContext`. The result is `<view>/<name>`: the view of a one-entry tree
 * holding the object under `name`, filtered by `filterFun` (as `builtins.path` filters) when it is
 * a directory. The World element is added to `outContext`, and the view is mounted for eval-time
 * reads.
 */
std::optional<std::string> tecnixWorldInputForPath(
    EvalState & state,
    const SourcePath & path,
    std::string_view name,
    Value * filterFun,
    PosIdx pos,
    const NixStringContext & inContext,
    NixStringContext & outContext);

} // namespace nix
