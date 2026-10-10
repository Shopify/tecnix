#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/ref.hh"

#include <chrono>
#include <optional>
#include <string>

namespace nix {

/**
 * GCS access token with expiration tracking
 */
struct GcsAccessToken
{
    std::string token;
    std::chrono::steady_clock::time_point expiresAt;

    bool isExpired() const
    {
        // Refresh 60 seconds before actual expiry for safety margin
        return std::chrono::steady_clock::now() >= (expiresAt - std::chrono::seconds(60));
    }
};

/**
 * Credentials were found but could not be used, e.g. the gcloud login has
 * been revoked or the service account key is malformed.
 */
MakeError(GcsAuthError, Error);

/**
 * No credentials are configured at all. Unlike other `GcsAuthError`s,
 * this is expected on machines without Google Cloud credentials, and
 * reads then proceed anonymously.
 */
MakeError(GcsNoCredentials, GcsAuthError);

/**
 * Provider for Google Cloud Storage credentials.
 * Implements Application Default Credentials (ADC) discovery:
 *   1. GOOGLE_APPLICATION_CREDENTIALS environment variable
 *   2. ~/.config/gcloud/application_default_credentials.json
 *   3. GCE metadata server (when running on Google Cloud)
 *
 * Supports credential types:
 *   - authorized_user: Uses refresh token (from `gcloud auth application-default login`)
 *   - service_account: Uses JWT signed with private key
 *   - gce_metadata: Fetches tokens from GCE metadata server (automatic on GCE/GKE/Cloud Run)
 */
class GcsCredentialProvider
{
public:
    /**
     * Get an access token for GCS requests.
     * Automatically refreshes expired tokens.
     *
     * @param writable If true, request read/write scope; otherwise read-only
     * @return Access token string
     * @throws GcsAuthError if credentials cannot be resolved
     */
    virtual std::string getAccessToken(bool writable = false) = 0;

    /**
     * Get an access token, or nullopt if no credentials are configured
     * (so that the request can be attempted anonymously). Credentials
     * that are configured but unusable are an error, like in
     * `getAccessToken()`, so that e.g. an expired login doesn't silently
     * turn into anonymous requests and 403s.
     *
     * @param writable If true, request read/write scope; otherwise read-only
     * @throws GcsAuthError if credentials exist but no token can be obtained
     */
    std::optional<std::string> maybeGetAccessToken(bool writable = false);

    virtual ~GcsCredentialProvider();
};

/**
 * Create a new GCS credential provider.
 */
ref<GcsCredentialProvider> makeGcsCredentialsProvider();

/**
 * Get a reference to the global GCS credential provider.
 */
ref<GcsCredentialProvider> getGcsCredentialsProvider();

} // namespace nix
