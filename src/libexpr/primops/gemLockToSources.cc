#include "nix/store/derivations.hh"
#include "nix/store/derived-path.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/fetchers/registry.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/hash.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

// Forward declaration from fetchTree.cc
void emitTreeAttrs(
    EvalState & state,
    const StorePath & storePath,
    const fetchers::Input & input,
    Value & v,
    bool emptyRevFallback,
    bool forceDirty);

enum class GemSourceType { Gem, Git, Path };

struct GemSpec {
    std::string name;
    std::string version;
    std::string remote;
    GemSourceType sourceType;
    std::string revision;                    // GIT only
    std::optional<std::string> ref;          // GIT only
    std::optional<std::string> branch;       // GIT only
    std::optional<Hash> sha256;              // from CHECKSUMS (GEM only)
};

static std::vector<std::string> splitLines(std::string_view content)
{
    std::vector<std::string> lines;
    std::istringstream stream{std::string(content)};
    std::string line;
    while (std::getline(stream, line)) {
        // Remove trailing \r if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

static bool startsWith(const std::string & s, const std::string & prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static std::vector<std::string> nixSystemToRubyPlatforms(const std::string & nixSystem)
{
    if (nixSystem == "x86_64-linux")
        return {"x86_64-linux", "x86_64-linux-gnu"};
    if (nixSystem == "aarch64-linux")
        return {"aarch64-linux", "aarch64-linux-gnu"};
    if (nixSystem == "x86_64-darwin")
        return {"x86_64-darwin", "universal-darwin"};
    if (nixSystem == "aarch64-darwin")
        return {"arm64-darwin", "aarch64-darwin", "universal-darwin"};
    return {};
}

struct GemLockParser {
    std::map<std::string, GemSpec> entries;
    std::map<std::string, std::vector<GemSpec>> gemCandidates;
    std::map<std::string, Hash> checksums;

    void parse(std::string_view content, const std::string & nixSystem)
    {
        auto lines = splitLines(content);
        size_t i = 0;
        while (i < lines.size()) {
            auto & line = lines[i];
            if (line == "GIT") {
                i = parseGitBlock(lines, i + 1);
            } else if (line == "PATH") {
                i = parsePathBlock(lines, i + 1);
            } else if (line == "GEM") {
                i = parseGemBlock(lines, i + 1);
            } else if (line == "CHECKSUMS") {
                i = parseChecksumsBlock(lines, i + 1);
            } else {
                i++;
            }
        }
        resolveGemCandidates(nixSystem);
        mergeChecksums();
    }

private:
    // Parse a GIT block:
    //   GIT
    //     remote: <url>
    //     revision: <sha>
    //     ref: <ref>          (optional)
    //     branch: <branch>    (optional)
    //     specs:
    //       <name> (<version>)
    //         <dep> ...       (ignored)
    size_t parseGitBlock(const std::vector<std::string> & lines, size_t i)
    {
        std::string remote, revision;
        std::optional<std::string> ref, branch;

        // Parse metadata (2-space indent)
        while (i < lines.size() && startsWith(lines[i], "  ") && !startsWith(lines[i], "    ")) {
            auto line = lines[i].substr(2);
            if (startsWith(line, "remote: "))
                remote = line.substr(8);
            else if (startsWith(line, "revision: "))
                revision = line.substr(10);
            else if (startsWith(line, "ref: "))
                ref = line.substr(5);
            else if (startsWith(line, "branch: "))
                branch = line.substr(8);
            i++;
        }

        // Parse specs (4-space indent)
        if (i < lines.size() && lines[i] == "  specs:")
            i++;

        while (i < lines.size() && startsWith(lines[i], "    ")) {
            auto line = lines[i];
            // Spec entries are at exactly 4 spaces, deps at 6
            if (startsWith(line, "      ")) {
                // dependency line, skip
                i++;
                continue;
            }
            // Parse "    <name> (<version>)"
            auto content = line.substr(4);
            auto parenPos = content.find(" (");
            if (parenPos != std::string::npos) {
                auto name = content.substr(0, parenPos);
                auto version = content.substr(parenPos + 2);
                if (!version.empty() && version.back() == ')')
                    version.pop_back();

                // GIT has highest priority — always insert/overwrite
                GemSpec spec;
                spec.name = name;
                spec.version = version;
                spec.remote = remote;
                spec.sourceType = GemSourceType::Git;
                spec.revision = revision;
                spec.ref = ref;
                spec.branch = branch;
                entries.insert_or_assign(name, std::move(spec));
            }
            i++;
        }
        return i;
    }

    // Parse a PATH block:
    //   PATH
    //     remote: <relative-path>
    //     specs:
    //       <name> (<version>)
    size_t parsePathBlock(const std::vector<std::string> & lines, size_t i)
    {
        std::string remote;

        while (i < lines.size() && startsWith(lines[i], "  ") && !startsWith(lines[i], "    ")) {
            auto line = lines[i].substr(2);
            if (startsWith(line, "remote: "))
                remote = line.substr(8);
            i++;
        }

        if (i < lines.size() && lines[i] == "  specs:")
            i++;

        while (i < lines.size() && startsWith(lines[i], "    ")) {
            auto line = lines[i];
            if (startsWith(line, "      ")) {
                i++;
                continue;
            }
            auto content = line.substr(4);
            auto parenPos = content.find(" (");
            if (parenPos != std::string::npos) {
                auto name = content.substr(0, parenPos);
                auto version = content.substr(parenPos + 2);
                if (!version.empty() && version.back() == ')')
                    version.pop_back();

                // PATH has second priority — skip if already from GIT
                if (entries.find(name) == entries.end() ||
                    entries.at(name).sourceType == GemSourceType::Gem) {
                    GemSpec spec;
                    spec.name = name;
                    spec.version = version;
                    spec.remote = remote;
                    spec.sourceType = GemSourceType::Path;
                    entries.insert_or_assign(name, std::move(spec));
                }
            }
            i++;
        }
        return i;
    }

    // Parse a GEM block:
    //   GEM
    //     remote: <url>
    //     specs:
    //       <name> (<version>)
    size_t parseGemBlock(const std::vector<std::string> & lines, size_t i)
    {
        std::string remote;

        while (i < lines.size() && startsWith(lines[i], "  ") && !startsWith(lines[i], "    ")) {
            auto line = lines[i].substr(2);
            if (startsWith(line, "remote: "))
                remote = line.substr(8);
            i++;
        }

        if (i < lines.size() && lines[i] == "  specs:")
            i++;

        while (i < lines.size() && startsWith(lines[i], "    ")) {
            auto line = lines[i];
            if (startsWith(line, "      ")) {
                i++;
                continue;
            }
            auto content = line.substr(4);
            auto parenPos = content.find(" (");
            if (parenPos != std::string::npos) {
                auto name = content.substr(0, parenPos);
                auto version = content.substr(parenPos + 2);
                if (!version.empty() && version.back() == ')')
                    version.pop_back();

                GemSpec spec;
                spec.name = name;
                spec.version = version;
                spec.remote = remote;
                spec.sourceType = GemSourceType::Gem;
                gemCandidates[name].push_back(std::move(spec));
            }
            i++;
        }
        return i;
    }

    // Parse CHECKSUMS block:
    //   CHECKSUMS
    //     <name> (<version>[-platform]) sha256=<hex>
    size_t parseChecksumsBlock(const std::vector<std::string> & lines, size_t i)
    {
        while (i < lines.size() && startsWith(lines[i], "  ")) {
            auto line = lines[i].substr(2);
            // Format: "<name> (<version>[-platform]) sha256=<hex>"
            auto parenOpen = line.find(" (");
            if (parenOpen == std::string::npos) {
                i++;
                continue;
            }
            auto parenClose = line.find(')', parenOpen);
            if (parenClose == std::string::npos) {
                i++;
                continue;
            }

            auto name = line.substr(0, parenOpen);
            auto versionPlatform = line.substr(parenOpen + 2, parenClose - parenOpen - 2);

            auto sha256Pos = line.find("sha256=", parenClose);
            if (sha256Pos == std::string::npos) {
                i++;
                continue;
            }
            auto hexStr = line.substr(sha256Pos + 7);

            auto key = name + "-" + versionPlatform;
            try {
                checksums.insert_or_assign(key, Hash::parseNonSRIUnprefixed(hexStr, HashAlgorithm::SHA256));
            } catch (...) {
                // Skip malformed checksums
            }

            i++;
        }
        return i;
    }

    void resolveGemCandidates(const std::string & nixSystem)
    {
        auto rubyPlatforms = nixSystemToRubyPlatforms(nixSystem);

        for (auto & [name, candidates] : gemCandidates) {
            // GIT/PATH have higher priority — skip if already in entries
            if (entries.find(name) != entries.end())
                continue;

            if (candidates.size() == 1) {
                entries.insert_or_assign(name, std::move(candidates[0]));
                continue;
            }

            // Multiple candidates — platform-specific gem
            // Try each Ruby platform in preference order
            bool found = false;
            for (auto & platform : rubyPlatforms) {
                auto suffix = "-" + platform;
                for (auto & candidate : candidates) {
                    if (candidate.version.size() >= suffix.size() &&
                        candidate.version.compare(
                            candidate.version.size() - suffix.size(),
                            suffix.size(), suffix) == 0) {
                        entries.insert_or_assign(name, std::move(candidate));
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }

            if (!found) {
                // Fallback: try a platform-agnostic candidate (version has no '-')
                for (auto & candidate : candidates) {
                    if (candidate.version.find('-') == std::string::npos) {
                        entries.insert_or_assign(name, std::move(candidate));
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                warn("gem '%s' has no variant matching system '%s', skipping", name, nixSystem);
            }
        }
    }

    void mergeChecksums()
    {
        for (auto & [name, spec] : entries) {
            if (spec.sourceType != GemSourceType::Gem)
                continue;
            auto key = spec.name + "-" + spec.version;
            auto it = checksums.find(key);
            if (it != checksums.end()) {
                spec.sha256 = it->second;
            }
        }
    }
};

/**
 * Create a fixed-output derivation that fetches a .gem file via builtin:fetchurl.
 */
static void createGemFOD(EvalState & state, const GemSpec & spec, Value & v)
{
    auto url = spec.remote + (spec.remote.back() == '/' ? "" : "/")
             + "gems/" + spec.name + "-" + spec.version + ".gem";
    auto drvName = spec.name + "-" + spec.version + ".gem";

    Derivation drv;
    drv.name = drvName;
    drv.builder = "builtin:fetchurl";
    drv.platform = "builtin";
    drv.env["builder"] = "builtin:fetchurl";
    drv.env["system"] = "builtin";
    drv.env["url"] = url;
    drv.env["urls"] = url;
    drv.env["name"] = drvName;
    drv.env["preferLocalBuild"] = "1";
    drv.env["outputHashMode"] = "flat";
    drv.env["outputHashAlgo"] = "sha256";
    drv.env["outputHash"] = spec.sha256->to_string(HashFormat::Base16, false);

    // For pkgs.shopify.io, add impureEnvVars for registry auth
    if (spec.remote.find("pkgs.shopify.io") != std::string::npos) {
        drv.env["impureEnvVars"] = "NIX_GEM_REGISTRY_LOGIN NIX_GEM_REGISTRY_PASSWORD";
    }

    DerivationOutput::CAFixed dof{
        .ca = ContentAddress{
            .method = ContentAddressMethod::Raw::Flat,
            .hash = *spec.sha256,
        },
    };

    drv.env["out"] = state.store->printStorePath(dof.path(*state.store, drvName, "out"));
    drv.outputs.insert_or_assign("out", std::move(dof));

    auto drvPath = writeDerivation(*state.store, *state.asyncPathWriter, drv, state.repair);

    // Cache the derivation hash for read-only mode support
    {
        auto h = hashDerivationModulo(*state.store, drv, false);
        drvHashes.insert_or_assign(drvPath, std::move(h));
    }

    auto result = state.buildBindings(1 + drv.outputs.size());
    result.alloc(state.s.drvPath)
        .mkString(
            state.store->printStorePath(drvPath),
            {NixStringContextElem::DrvDeep{.drvPath = drvPath}},
            state.mem);

    for (auto & i : drv.outputs) {
        state.mkOutputString(
            result.alloc(i.first),
            SingleDerivedPath::Built{
                .drvPath = makeConstantStorePathRef(drvPath),
                .output = i.first,
            },
            i.second.path(*state.store, Derivation::nameFromPath(drvPath), i.first));
    }

    v.mkAttrs(result);
}

/**
 * Fetch a git source using the fetchers infrastructure.
 */
static void createGitFetch(EvalState & state, const PosIdx pos, const GemSpec & spec, Value & v)
{
    fetchers::Attrs attrs;
    attrs.emplace("type", "git");
    attrs.emplace("url", spec.remote);
    attrs.emplace("rev", spec.revision);
    if (spec.ref)
        attrs.emplace("ref", *spec.ref);
    attrs.emplace("exportIgnore", Explicit<bool>{true});

    auto input = fetchers::Input::fromAttrs(state.fetchSettings, std::move(attrs));
    state.checkURI(input.toURLString());

    auto cachedInput = state.inputCache->getAccessor(
        state.fetchSettings, *state.store, input, fetchers::UseRegistries::No);
    auto storePath = state.mountInput(
        cachedInput.lockedInput, input, cachedInput.accessor, true);
    emitTreeAttrs(state, storePath, cachedInput.lockedInput, v, true, false);
}

/**
 * Copy a local path to the store.
 */
static void createPathFetch(EvalState & state, const GemSpec & spec,
                            const SourcePath & lockFileDir, Value & v)
{
    auto pathToAdd = lockFileDir / CanonPath(spec.remote);
    auto storePath = fetchToStore(
        state.fetchSettings, *state.store,
        pathToAdd.resolveSymlinks(), FetchMode::Copy, spec.name);
    state.allowAndSetStorePathString(storePath, v);
}

static void prim_gemLockToSources(EvalState & state, const PosIdx pos,
                                   Value ** args, Value & v)
{
    NixStringContext context;
    auto lockFilePath = state.coerceToPath(pos, *args[0], context,
        "while evaluating the argument passed to builtins.gemLockToSources");
    auto lockFileDir = lockFilePath.parent();
    auto content = lockFilePath.readFile();

    GemLockParser parser;
    auto nixSystem = state.settings.getCurrentSystem();
    parser.parse(content, nixSystem);

    auto attrs = state.buildBindings(parser.entries.size());
    for (auto & [name, spec] : parser.entries) {
        switch (spec.sourceType) {
        case GemSourceType::Gem:
            if (!spec.sha256) {
                warn("gem '%s' has no checksum, skipping", name);
                continue;
            }
            createGemFOD(state, spec, attrs.alloc(name));
            break;
        case GemSourceType::Git:
            createGitFetch(state, pos, spec, attrs.alloc(name));
            break;
        case GemSourceType::Path:
            createPathFetch(state, spec, lockFileDir, attrs.alloc(name));
            break;
        }
    }
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_gemLockToSources({
    .name = "__gemLockToSources",
    .args = {"lockFile"},
    .doc = R"(
      Parse a Gemfile.lock and return an attrset of gem sources.
      GEM entries become fetchurl FODs, GIT entries use fetchGit,
      PATH entries are copied to the store.
    )",
    .fun = prim_gemLockToSources,
});

} // namespace nix
