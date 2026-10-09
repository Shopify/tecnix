#include "nix/store/gcs-url.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

#include <ranges>

namespace nix {

/**
 * Parse an `endpoint` parameter: either an absolute `http(s)://` URL or
 * an authority (`host[:port]`). The two are told apart by `://`, since
 * `host:port` would also parse as a URL with scheme `host`.
 */
static decltype(ParsedGcsURL::endpoint) parseGcsEndpoint(const std::string & endpoint)
{
    if (endpoint.find("://") != std::string::npos) {
        auto url = parseURL(endpoint);
        if ((url.scheme != "http" && url.scheme != "https") || !url.authority || url.authority->host.empty())
            throw BadURL("endpoint '%s' is not an http(s) URL with a host", endpoint);
        return url;
    }
    return ParsedURL::Authority::parse(endpoint);
}

ParsedGcsURL ParsedGcsURL::parse(const ParsedURL & parsed)
try {
    if (parsed.scheme != "gs")
        throw BadURL("URI scheme '%s' is not 'gs'", parsed.scheme);

    /* GCS bucket names are DNS-compatible names, so a registered name
       authority is the right thing. */
    if (!parsed.authority || parsed.authority->host.empty()
        || parsed.authority->hostType != ParsedURL::Authority::HostType::Name)
        throw BadURL("URI has a missing or invalid bucket name");

    if (parsed.path.size() <= 1 || !parsed.path.front().empty())
        throw BadURL("URI has a missing or invalid key");

    auto getOptionalParam = [&](std::string_view key) -> std::optional<std::string> {
        auto it = parsed.query.find(key);
        if (it == parsed.query.end())
            return std::nullopt;
        return it->second;
    };

    return ParsedGcsURL{
        .bucket = parsed.authority->host,
        /* Skip the first empty path segment (from the leading `/`). */
        .key = std::views::drop(parsed.path, 1) | std::ranges::to<std::vector<std::string>>(),
        .writable = getOptionalParam("write") == "true",
        .endpoint = [&]() -> decltype(ParsedGcsURL::endpoint) {
            if (auto endpoint = getOptionalParam("endpoint"))
                return parseGcsEndpoint(*endpoint);
            return std::monostate();
        }(),
    };
} catch (BadURL & e) {
    e.addTrace({}, "while parsing GCS URI: '%s'", parsed.to_string());
    throw;
}

ParsedURL ParsedGcsURL::endpointUrl() const
{
    auto url = std::visit(
        overloaded{
            [&](const std::monostate &) {
                return ParsedURL{
                    .scheme = "https",
                    .authority = ParsedURL::Authority{.host = "storage.googleapis.com"},
                };
            },
            [&](const ParsedURL::Authority & authority) {
                return ParsedURL{
                    .scheme = "https",
                    .authority = authority,
                };
            },
            [&](const ParsedURL & endpointUrl) {
                return ParsedURL{
                    .scheme = endpointUrl.scheme,
                    .authority = endpointUrl.authority,
                    .path = endpointUrl.path,
                };
            },
        },
        endpoint);

    /* Normalise the path to an absolute path without a trailing slash, so
       that further segments can be appended. */
    while (!url.path.empty() && url.path.back().empty())
        url.path.pop_back();
    if (url.path.empty())
        url.path.push_back("");

    return url;
}

ParsedURL ParsedGcsURL::toHttpsUrl() const
{
    auto url = endpointUrl();
    url.path.push_back(bucket);
    url.path.insert(url.path.end(), key.begin(), key.end());
    return url;
}

} // namespace nix
