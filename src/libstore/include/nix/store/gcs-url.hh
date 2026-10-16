#pragma once
///@file

#include "nix/util/url.hh"

#include <string>
#include <variant>
#include <vector>

namespace nix {

/**
 * Parsed gs:// URL for Google Cloud Storage
 */
struct ParsedGcsURL
{
    std::string bucket;
    /**
     * @see ParsedURL::path. This is a vector for the same reason.
     * Unlike ParsedURL::path this doesn't include the leading empty segment,
     * since the bucket name is necessary.
     */
    std::vector<std::string> key;
    /**
     * Whether write access is requested (via ?write=true query param).
     * Defaults to false (read-only).
     */
    bool writable = false;
    /**
     * The endpoint (`?endpoint=` query parameter) can be either missing,
     * an absolute URI (with a scheme like `http:`) or an authority (so an
     * IP address or a registered name). When missing, the public
     * `https://storage.googleapis.com` endpoint is used.
     *
     * This is mostly useful for testing against GCS emulators.
     */
    std::variant<std::monostate, ParsedURL, ParsedURL::Authority> endpoint;

    /**
     * Parse a gs:// URL.
     *
     * @param parsed The parsed URL to convert
     * @return ParsedGcsURL with bucket and key extracted
     * @throws BadURL if the URL is not a valid gs:// URL
     */
    static ParsedGcsURL parse(const ParsedURL & parsed);

    /**
     * The endpoint's URL (scheme, authority and any path prefix), with no
     * bucket or key: `https://storage.googleapis.com` unless `endpoint` is
     * set. The JSON and XML API URLs are built on top of it.
     */
    ParsedURL endpointUrl() const;

    /**
     * Convert to an HTTPS URL for the GCS XML API, i.e.
     * `https://storage.googleapis.com/<bucket>/<key>` (path-style
     * addressing), or the equivalent on the custom endpoint.
     */
    ParsedURL toHttpsUrl() const;

    auto operator<=>(const ParsedGcsURL & other) const = default;
};

} // namespace nix
