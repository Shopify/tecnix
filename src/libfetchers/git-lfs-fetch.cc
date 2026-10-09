#include "nix/fetchers/git-lfs-fetch.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/base-n.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"
#include "nix/util/url.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"
#include "nix/util/json-utils.hh"
#include "nix/store/ssh.hh"
#include "nix/util/deleter.hh"

#include <git2/attr.h>
#include <git2/config.h>
#include <git2/errors.h>
#include <git2/remote.h>

#include <nlohmann/json.hpp>

namespace nix::lfs {

static void downloadToSink(
    const std::string & url,
    const std::optional<std::string> & authHeader,
    Sink & sink,
    std::string sha256Expected,
    size_t sizeExpected)
{
    FileTransferRequest request(parseURL(url));
    Headers headers;
    if (authHeader.has_value())
        headers.push_back({"Authorization", *authHeader});
    request.headers = headers;

    HashSink hashSink(HashAlgorithm::SHA256);
    TeeSink teeSink(hashSink, sink);

    getFileTransfer()->download(std::move(request), teeSink);

    auto hashResult = hashSink.finish();

    if (sizeExpected != hashResult.numBytesDigested)
        throw Error(
            "size mismatch while fetching %s: expected %d but got %d", url, sizeExpected, hashResult.numBytesDigested);

    auto sha256Actual = hashResult.hash.to_string(HashFormat::Base16, false);
    if (sha256Actual != sha256Expected)
        throw Error(
            "hash mismatch while fetching %s: expected sha256:%s but got sha256:%s", url, sha256Expected, sha256Actual);
}

/**
 * Run `git credential fill` with `description` on its standard input and
 * return its standard output. `runProgram()` doesn't redirect standard
 * input, so spawn the process directly.
 */
static std::string runGitCredentialFill(const std::string & description)
{
#ifdef _WIN32
    throw UnimplementedError("git-credential-fill is not supported on Windows");
#else
    Pipe toChild, fromChild;
    toChild.create();
    fromChild.create();

    Pid pid = startProcess([&]() {
        toChild.writeSide.close();
        fromChild.readSide.close();
        if (dup2(toChild.readSide.get(), STDIN_FILENO) == -1)
            throw SysError("dupping stdin");
        if (dup2(fromChild.writeSide.get(), STDOUT_FILENO) == -1)
            throw SysError("dupping stdout");
        execlp("git", "git", "credential", "fill", nullptr);
        throw SysError("executing 'git credential fill'");
    });

    toChild.readSide.close();
    fromChild.writeSide.close();
    writeFull(toChild.writeSide.get(), description);
    toChild.writeSide.close();

    auto output = drainFD(fromChild.readSide.get());
    /* As with `runProgram()`'s status-returning overload, a failing helper
       is reported through its (empty) output rather than its exit status. */
    pid.wait();
    return output;
#endif
}

ParsedURL lfsEndpointForRemote(ParsedURL url)
{
    /**
     * Try to mimic what git-lfs will do to plain remotes
     * https://github.com/git-lfs/git-lfs/blob/main/docs/api/server-discovery.md
     *
     * Try to be smarter with remotes ending in a /, like
     * `https://github.com/NixOS/nix/`. This should be
     * `https://github.com/NixOS/nix.git/info/lfs`, not
     * `https://github.com/NixOS/nix/.git/info/lfs`
     */
    bool hasDotGit = false;
    for (auto it = url.path.rbegin(); it != url.path.rend(); ++it) {
        if (it->empty())
            continue;
        if (!it->ends_with(".git"))
            *it += ".git";
        hasDotGit = true;
        break;
    }
    if (!hasDotGit) {
        if (url.path.size() > 1) // e.g. {"", ""} (single trailing slash)
            url.path.back() = ".git";
        else if (url.path.size() == 1) // {""}
            url.path.push_back(".git");
        else { // {}
            url.path.push_back("");
            url.path.push_back(".git");
        }
    }
    if (url.path.back().empty())
        url.path.back() = "info";
    else
        url.path.push_back("info");
    url.path.push_back("lfs");

    return url;
}

LfsApiInfo getLfsApi(ParsedURL url)
{
    assert(url.authority.has_value());
    if (url.scheme == "ssh") {
        auto args = getNixSshOpts();

        if (url.authority->port)
            args.push_back(string_to_os_string(fmt("-p%d", *url.authority->port)));

        std::ostringstream hostnameAndUser;
        if (url.authority->user)
            hostnameAndUser << *url.authority->user << "@";
        hostnameAndUser << url.authority->host;
        args.push_back(string_to_os_string(std::move(hostnameAndUser).str()));

        args.push_back(OS_STR("--"));
        args.push_back(OS_STR("git-lfs-authenticate"));
        // FIXME %2F encode slashes? Does this command take/accept percent encoding?
        args.push_back(string_to_os_string(url.renderPath(/*encode=*/false)));
        args.push_back(OS_STR("download"));

        auto [status, output] = runProgram({.program = "ssh", .args = args});

        if (output.empty())
            throw Error(
                "git-lfs-authenticate: no output (cmd: 'ssh %s')",
                concatMapStringsSep(
                    " ", args, [](const OsString & s) { return escapeShellArgAlways(os_string_to_string(s)); }));

        auto queryResp = nlohmann::json::parse(output);
        auto headerIt = queryResp.find("header");
        if (headerIt == queryResp.end())
            throw Error("no header in git-lfs-authenticate response");
        auto authIt = headerIt->find("Authorization");
        if (authIt == headerIt->end())
            throw Error("no Authorization in git-lfs-authenticate response");

        auto href = url.to_string();
        auto hrefIt = queryResp.find("href");
        if (hrefIt != queryResp.end())
            href = hrefIt->get<std::string>();

        return {href, authIt->get<std::string>()};
    } else {
        // git credential attributes take the path decoded and without a leading slash
        auto credentialPath = url.renderPath(false);
        if (credentialPath.starts_with("/"))
            credentialPath.erase(0, 1);

        std::ostringstream inputCredDescr;
        inputCredDescr << "protocol=" << url.scheme << "\n";
        inputCredDescr << "host=" << url.authority->host << "\n";
        inputCredDescr << "path=" << credentialPath << "\n";

        auto output = runGitCredentialFill(std::move(inputCredDescr).str());

        if (output.empty())
            throw Error(
                "git-credential-fill: no output (cmd: 'git credential fill' for protocol=%s, host=%s, path=%s)",
                url.scheme,
                url.authority->host,
                credentialPath);

        std::string username;
        std::string password;
        for (auto & line : tokenizeString<Strings>(output, "\n")) {
            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            if (key == "username")
                username = val;
            else if (key == "password")
                password = val;
        }

        if (username.empty() || password.empty())
            throw Error(
                "git-credential-fill: no credentials returned (cmd: 'git credential fill' for protocol=%s, host=%s, path=%s)",
                url.scheme,
                url.authority->host,
                credentialPath);

        return {
            url.to_string(),
            "Basic " + base64::encode(std::as_bytes(std::span<const char>{username + ":" + password}))};
    }

    return {url.to_string(), std::nullopt};
}

typedef std::unique_ptr<git_config, Deleter<git_config_free>> GitConfig;
typedef std::unique_ptr<git_config_entry, Deleter<git_config_entry_free>> GitConfigEntry;

static std::string getLfsEndpointUrl(git_repository * repo)
{
    GitConfig config;
    if (!git_repository_config(Setter(config), repo)) {
        GitConfigEntry entry;
        if (!git_config_get_entry(Setter(entry), config.get(), "lfs.url")) {
            auto value = std::string(entry->value);
            if (!value.empty()) {
                debug("Found explicit lfs.url value: %s", value);
                return value;
            }
        }

        GitConfigEntry remoteEntry;
        if (!git_config_get_entry(Setter(remoteEntry), config.get(), "remote.origin.lfsurl")) {
            auto value = std::string(remoteEntry->value);
            if (!value.empty()) {
                debug("Found explicit remote.origin.lfsurl value: %s", value);
                return value;
            }
        }
    }

    git_remote * remote = nullptr;
    if (!git_remote_lookup(&remote, repo, "origin")) {
        const char * url_c_str = git_remote_url(remote);
        if (url_c_str)
            return lfsEndpointForRemote(fixGitURL(url_c_str)).to_string();
    }

    return "";
}

static std::filesystem::path getLfsStorageDir(git_repository * repo)
{
    auto gitDir = std::filesystem::path(git_repository_commondir(repo));

    GitConfig config;
    if (!git_repository_config(Setter(config), repo)) {
        GitConfigEntry entry;
        if (!git_config_get_entry(Setter(entry), config.get(), "lfs.storage")) {
            std::filesystem::path storage(entry->value);
            if (!storage.empty()) {
                // git-lfs: an absolute path is used as-is, a relative path is
                // resolved against the git common directory.
                return storage.is_absolute() ? storage : gitDir / storage;
            }
        }
    }

    return gitDir / "lfs"; // git-lfs default
}

static std::optional<Pointer> parseLfsPointer(std::string_view content, std::string_view filename)
{
    // https://github.com/git-lfs/git-lfs/blob/2ef4108/docs/spec.md
    //
    // example git-lfs pointer file:
    // version https://git-lfs.github.com/spec/v1
    // oid sha256:f5e02aa71e67f41d79023a128ca35bad86cf7b6656967bfe0884b3a3c4325eaf
    // size 10000000
    // (ending \n)

    if (!content.starts_with("version ")) {
        // Invalid pointer file
        return std::nullopt;
    }

    if (!content.starts_with("version https://git-lfs.github.com/spec/v1")) {
        // In case there's new spec versions in the future, but for now only v1 exists
        debug("Invalid version found on potential lfs pointer file, skipping");
        return std::nullopt;
    }

    std::string oid;
    std::string size;

    for (auto & line : tokenizeString<Strings>(content, "\n")) {
        if (line.starts_with("version ")) {
            continue;
        }
        if (line.starts_with("oid sha256:")) {
            oid = line.substr(11); // skip "oid sha256:"
            continue;
        }
        if (line.starts_with("size ")) {
            size = line.substr(5); // skip "size "
            continue;
        }

        debug("Custom extension '%s' found, ignoring", line);
    }

    if (oid.length() != 64 || !std::all_of(oid.begin(), oid.end(), ::isxdigit)) {
        debug("Invalid sha256 %s, skipping", oid);
        return std::nullopt;
    }

    if (size.length() == 0 || !std::all_of(size.begin(), size.end(), ::isdigit)) {
        debug("Invalid size %s, skipping", size);
        return std::nullopt;
    }

    return std::make_optional(Pointer{oid, std::stoul(size)});
}

Fetch::Fetch(git_repository * repo, git_oid rev, std::string attrPathPrefix)
{
    this->repo = repo;
    this->rev = rev;
    this->attrPathPrefix = std::move(attrPathPrefix);

    const auto remoteUrl = lfs::getLfsEndpointUrl(repo);

    /* A repository with neither an `origin` remote nor an `lfs.url` has no
       LFS endpoint. Merely enabling smudging must not fail on it (there may
       be no LFS pointers to smudge), so leave `url` empty and report it
       from `fetchUrls()` instead. */
    if (!remoteUrl.empty())
        this->url = nix::fixGitURL(remoteUrl).canonicalise();
}

bool Fetch::shouldFetch(const CanonPath & path) const
{
    const char * attr = nullptr;
    git_attr_options opts = GIT_ATTR_OPTIONS_INIT;
    opts.attr_commit_id = this->rev;
    opts.flags = GIT_ATTR_CHECK_INCLUDE_COMMIT | GIT_ATTR_CHECK_NO_SYSTEM;
    auto fullPath = attrPathPrefix.empty() ? path : CanonPath("/" + attrPathPrefix) / path;
    if (git_attr_get_ext(&attr, (git_repository *) (this->repo), &opts, fullPath.rel_c_str(), "filter"))
        throw Error("cannot get git-lfs attribute: %s", git_error_last()->message);
    debug("Git filter for '%s' is '%s'", fullPath, attr ? attr : "null");
    return attr != nullptr && !std::string(attr).compare("lfs");
}

static nlohmann::json pointerToPayload(const std::vector<Pointer> & items)
{
    nlohmann::json jArray = nlohmann::json::array();
    for (const auto & pointer : items)
        jArray.push_back({{"oid", pointer.oid}, {"size", pointer.size}});
    return jArray;
}

std::vector<nlohmann::json> Fetch::fetchUrls(const std::vector<Pointer> & pointers) const
{
    if (url.scheme.empty())
        throw Error("cannot fetch git-lfs objects: the repository has no 'origin' remote and no 'lfs.url' configured");
    auto api = lfs::getLfsApi(this->url);
    auto url = api.endpoint + "/objects/batch";
    const auto & authHeader = api.authHeader;
    FileTransferRequest request(parseURL(url));
    request.method = HttpMethod::Post;
    Headers headers;
    if (authHeader.has_value())
        headers.push_back({"Authorization", *authHeader});
    headers.push_back({"Content-Type", "application/vnd.git-lfs+json"});
    headers.push_back({"Accept", "application/vnd.git-lfs+json"});
    request.headers = headers;
    nlohmann::json oidList = pointerToPayload(pointers);
    nlohmann::json data = {{"operation", "download"}};
    data["objects"] = oidList;
    auto payload = data.dump();
    StringSource source{payload};
    request.data = {source};

    FileTransferResult result = getFileTransfer()->upload(request);
    auto responseString = result.data;

    std::vector<nlohmann::json> objects;
    // example resp here:
    // {"objects":[{"oid":"f5e02aa71e67f41d79023a128ca35bad86cf7b6656967bfe0884b3a3c4325eaf","size":10000000,"actions":{"download":{"href":"https://gitlab.com/b-camacho/test-lfs.git/gitlab-lfs/objects/f5e02aa71e67f41d79023a128ca35bad86cf7b6656967bfe0884b3a3c4325eaf","header":{"Authorization":"Basic
    // Yi1jYW1hY2hvOmV5SjBlWEFpT2lKS1YxUWlMQ0poYkdjaU9pSklVekkxTmlKOS5leUprWVhSaElqcDdJbUZqZEc5eUlqb2lZaTFqWVcxaFkyaHZJbjBzSW1wMGFTSTZJbUptTURZNFpXVTFMVEprWmpVdE5HWm1ZUzFpWWpRMExUSXpNVEV3WVRReU1qWmtaaUlzSW1saGRDSTZNVGN4TkRZeE16ZzBOU3dpYm1KbUlqb3hOekUwTmpFek9EUXdMQ0psZUhBaU9qRTNNVFEyTWpFd05EVjkuZk9yMDNkYjBWSTFXQzFZaTBKRmJUNnJTTHJPZlBwVW9lYllkT0NQZlJ4QQ=="}}},"authenticated":true}]}

    try {
        auto resp = nlohmann::json::parse(responseString);
        if (resp.contains("objects"))
            objects.insert(objects.end(), resp["objects"].begin(), resp["objects"].end());
        else
            throw Error("response does not contain 'objects'");

        return objects;
    } catch (const nlohmann::json::parse_error & e) {
        printMsg(lvlTalkative, "Full response: '%1%'", responseString);
        throw Error("response did not parse as json: %s", e.what());
    }
}

void Fetch::fetch(
    const std::string & content,
    const CanonPath & pointerFilePath,
    Sink & sink,
    std::function<void(uint64_t)> sizeCallback) const
{
    debug("trying to fetch '%s' using git-lfs", pointerFilePath);

    if (content.length() >= 1024) {
        warn("encountered file '%s' that should have been a git-lfs pointer, but is too large", pointerFilePath);
        sizeCallback(content.length());
        sink(content);
        return;
    }

    const auto pointer = parseLfsPointer(content, pointerFilePath.rel());
    if (pointer == std::nullopt) {
        warn("encountered file '%s' that should have been a git-lfs pointer, but is invalid", pointerFilePath);
        sizeCallback(content.length());
        sink(content);
        return;
    }

    // Check the local git LFS object store before hitting the network
    auto localLfsPath = getLfsStorageDir((git_repository *) repo) / "objects" / pointer->oid.substr(0, 2)
                        / pointer->oid.substr(2, 2) / pointer->oid;
    if (pathExists(localLfsPath)) {
        debug("using local git lfs object %s", PathFmt(localLfsPath));
        auto localContent = readFile(localLfsPath);
        sizeCallback(localContent.length());
        sink(localContent);
        return;
    }

    auto cacheDir = getCacheDir() / "git-lfs";
    std::string key = hashString(HashAlgorithm::SHA256, pointerFilePath.rel()).to_string(HashFormat::Base16, false)
                      + "/" + pointer->oid;
    auto cachePath = cacheDir / key;
    AutoCloseFD cacheFile(openFileReadonly(cachePath, FinalSymlink::DontFollow));
    if (cacheFile) {
        debug("using cache entry %s -> %s", key, PathFmt(cachePath));
        FdSource cacheSource(cacheFile.get());
        auto size = getFileSize(cacheFile.get());
        sizeCallback(size);
        cacheSource.drainInto(sink, size);
        return;
    }
    debug("did not find cache entry for %s", key);

    std::vector<Pointer> pointers;
    pointers.push_back(pointer.value());
    const auto objUrls = fetchUrls(pointers);

    const auto obj = objUrls[0];
    try {
        // Use the committed pointer's oid/size for integrity, not server's claim
        std::string sha256 = pointer->oid;
        std::string ourl = obj.at("actions").at("download").at("href");
        auto authHeader = [&]() -> std::optional<std::string> {
            const auto & download = obj.at("actions").at("download");
            auto headerIt = download.find("header");
            if (headerIt == download.end())
                return std::nullopt;
            auto authIt = headerIt->find("Authorization");
            if (authIt == headerIt->end())
                return std::nullopt;
            return std::string(*authIt);
        }();
        const uint64_t size = pointer->size;

        auto objOid = getString(valueAt(getObject(obj), "oid"));
        auto objSize = getUnsigned(valueAt(getObject(obj), "size"));
        if (objOid != pointer->oid || objSize != pointer->size) {
            throw Error(
                "LFS server returned mismatched oid/size for '%s' (got oid=%s size=%d, expected oid=%s size=%d)",
                pointerFilePath,
                objOid,
                objSize,
                pointer->oid,
                pointer->size);
        }

        debug("creating cache entry %s -> %s", key, PathFmt(cachePath));

        if (!pathExists(cachePath.parent_path()))
            createDirs(cachePath.parent_path());
        auto [tempFile, tempPath] = createTempFile(cachePath.parent_path(), {});
        AutoDelete tempDeleter(tempPath);
        FdSink tempSink(tempFile.get());
        downloadToSink(ourl, authHeader, tempSink, sha256, size);
        tempSink.flush();

        std::filesystem::rename(tempPath, cachePath);
        tempDeleter.cancel();

        FdSource cacheSource(tempFile.get());
        cacheSource.restart();
        sizeCallback(size);
        cacheSource.drainInto(sink, size);

        debug("%s fetched with git-lfs", pointerFilePath);
    } catch (const nlohmann::json::out_of_range & e) {
        throw Error("bad json from /info/lfs/objects/batch: %s %s", obj, e.what());
    }
}

} // namespace nix::lfs
