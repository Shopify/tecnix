#pragma once
///@file

#include "nix/util/configuration.hh"
#include "nix/util/error.hh"
#include "nix/util/ref.hh"
#include "nix/util/url.hh"
#include "nix/util/types.hh"

#include <chrono>
#include <optional>
#include <string>

namespace nix {

/**
 * Settings for access to Google Cloud Storage.
 */
struct GcsSettings : Config
{
    void anchor() override;

    Setting<Strings> credentialHelper{
        this,
        {},
        "gcs-credential-helper",
        R"(
          A [Git credential helper](https://git-scm.com/docs/gitcredentials)
          that provides the OAuth2 access token for `gs://` stores. Nix
          calls it the way Go's `GOAUTH=git` or Cargo do: it runs the
          command with `get` appended, writes the request to its standard
          input,

              protocol=https
              host=storage.googleapis.com
              path=<bucket>/<prefix>/
              capability[]=authtype

          and reads attributes back. GCS authenticates with a bearer token,
          so the answer must carry one,

              authtype=Bearer
              credential=ya29.a0AfH6SMB...
              password_expiry_utc=1760011200

          (a helper without the `authtype` capability may return it as
          `password` instead). When set, this is the only source of
          credentials.

          Nix asks once per store, for the store's URL, and uses the answer
          for all requests under it until a minute before
          `password_expiry_utc`. A helper that reports no expiry is run for
          every request, since Nix does not guess how long its tokens stay
          valid. A non-zero exit, a missing credential, an `authtype` other
          than `Bearer` or an already expired credential is an error, not a
          fall back to anonymous access.

          The token's scope is the helper's business: reads from a binary
          cache need `devstorage.read_only`, uploads `devstorage.read_write`.

          When the Nix daemon serves a client, the helper runs *as that
          client's user* (with that user's home directory, and
          `XDG_RUNTIME_DIR` set to `/run/user/<uid>` when that exists), not
          as the daemon. A per-user token broker therefore sees an ordinary
          same-user caller, and the daemon never holds a long-lived
          credential. Use an absolute path: the helper runs with a minimal
          `PATH`, and without a terminal, so a helper that prompts fails.
        )"};
};

extern GcsSettings gcsSettings;

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
 * Create a new GCS credential provider that resolves credentials with
 * Application Default Credentials.
 */
ref<GcsCredentialProvider> makeGcsCredentialsProvider();

/**
 * A provider that gets its credentials for `uri` from the given Git
 * credential helper, as described for `gcs-credential-helper`.
 */
ref<GcsCredentialProvider> makeGcsCredentialHelperProvider(Strings command, ParsedURL uri);

/**
 * The credential provider for requests to `uri`: the `gcs-credential-helper`
 * if one is configured, otherwise the shared Application Default
 * Credentials provider.
 */
ref<GcsCredentialProvider> getGcsCredentialsProvider(const ParsedURL & uri);

} // namespace nix
