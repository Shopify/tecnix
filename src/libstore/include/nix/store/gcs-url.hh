#pragma once
///@file

#include "nix/util/url.hh"

#include <optional>
#include <string>
#include <vector>

namespace nix {

/**
 * Parsed gs:// URL for Google Cloud Storage
 */
struct ParsedGcsURL
{
    std::string bucket;
    std::vector<std::string> key;
    /**
     * Whether write access is requested (via ?write=true query param).
     * Defaults to false (read-only).
     */
    bool writable = false;

    /**
     * Where the GCS APIs are served (via ?endpoint=URL), e.g. an emulator at
     * `http://127.0.0.1:4443`. Defaults to `https://storage.googleapis.com`.
     * Requests to an endpoint never carry Google credentials (see
     * `sendsCredentials`).
     */
    std::optional<std::string> endpoint;

    /**
     * Whether requests for this URL carry the Application Default Credentials
     * token: only when they go to Google's own endpoint. Anything can put a
     * gs:// URL in front of Nix (a substituter, `nix copy --from`,
     * `builtins.fetchurl`, a fixed-output derivation the daemon fetches with
     * its own credentials), so an `endpoint` must never receive the token.
     */
    bool sendsCredentials() const
    {
        return !endpoint;
    }

    /**
     * Parse a gs:// URL.
     *
     * @param parsed The parsed URL to convert
     * @return ParsedGcsURL with bucket and key extracted
     * @throws BadURL if the URL is not a valid gs:// URL
     */
    static ParsedGcsURL parse(const ParsedURL & parsed);

    /**
     * The endpoint's scheme and authority: `https://storage.googleapis.com`
     * unless `endpoint` is set.
     */
    ParsedURL apiBase() const;

    /**
     * The object's URL on the endpoint (the XML API's path-style form),
     * `https://storage.googleapis.com/<bucket>/<key>` by default.
     */
    ParsedURL toHttpsUrl() const;

    auto operator<=>(const ParsedGcsURL & other) const = default;
};

} // namespace nix
