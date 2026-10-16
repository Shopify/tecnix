#include "nix/store/gcs-url.hh"
#include "nix/util/error.hh"

namespace nix {

ParsedGcsURL ParsedGcsURL::parse(const ParsedURL & parsed)
try {
    if (parsed.scheme != "gs")
        throw BadURL("URI scheme '%s' is not 'gs'", parsed.scheme);

    if (!parsed.authority || parsed.authority->host.empty()
        || parsed.authority->hostType != ParsedURL::Authority::HostType::Name)
        throw BadURL("URI has a missing or invalid bucket name");

    if (parsed.path.size() <= 1 || !parsed.path.front().empty())
        throw BadURL("URI has a missing or invalid key");

    // Skip the first empty path segment (from leading /)
    std::vector<std::string> key(parsed.path.begin() + 1, parsed.path.end());

    // Check for write=true query parameter
    bool writable = false;
    auto it = parsed.query.find("write");
    if (it != parsed.query.end() && it->second == "true") {
        writable = true;
    }

    std::optional<std::string> endpoint;
    if (auto e = parsed.query.find("endpoint"); e != parsed.query.end() && !e->second.empty()) {
        auto url = parseURL(e->second);
        if ((url.scheme != "https" && url.scheme != "http") || !url.authority || url.authority->host.empty())
            throw BadURL("endpoint '%s' is not an http(s) URL with a host", e->second);
        endpoint = e->second;
    }

    return ParsedGcsURL{
        .bucket = parsed.authority->host,
        .key = std::move(key),
        .writable = writable,
        .endpoint = std::move(endpoint),
    };
} catch (BadURL & e) {
    e.addTrace({}, "while parsing GCS URI: '%s'", parsed.to_string());
    throw;
}

ParsedURL ParsedGcsURL::apiBase() const
{
    if (!endpoint)
        return ParsedURL{
            .scheme = "https",
            .authority = ParsedURL::Authority{.host = "storage.googleapis.com"},
        };
    auto url = parseURL(*endpoint);
    return ParsedURL{.scheme = std::move(url.scheme), .authority = std::move(url.authority)};
}

ParsedURL ParsedGcsURL::toHttpsUrl() const
{
    auto url = apiBase();
    url.path = {"", bucket};
    url.path.insert(url.path.end(), key.begin(), key.end());
    return url;
}

} // namespace nix
