#include "nix/store/gcs-creds.hh"
#include "nix/store/daemon.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/base-n.hh"
#include "nix/util/config-global.hh"
#include "nix/util/current-process.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/logging.hh"
#include "nix/util/processes.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#  include <grp.h>
#  include <pwd.h>
#  include <unistd.h>
#endif

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace nix {

GcsSettings gcsSettings;

void GcsSettings::anchor() {}

static GlobalConfig::Register rGcsSettings(&gcsSettings);

namespace {

// RAII wrappers for OpenSSL resources
struct BioDeleter
{
    void operator()(BIO * bio) const
    {
        if (bio)
            BIO_free(bio);
    }
};

using UniqueBio = std::unique_ptr<BIO, BioDeleter>;

struct EvpPkeyDeleter
{
    void operator()(EVP_PKEY * pkey) const
    {
        if (pkey)
            EVP_PKEY_free(pkey);
    }
};

using UniqueEvpPkey = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct EvpMdCtxDeleter
{
    void operator()(EVP_MD_CTX * ctx) const
    {
        if (ctx)
            EVP_MD_CTX_free(ctx);
    }
};

using UniqueEvpMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

// Google's OAuth2 token endpoint
constexpr std::string_view TOKEN_URI = "https://oauth2.googleapis.com/token";

// GCE metadata server for instances running on Google Cloud
constexpr std::string_view GCE_METADATA_TOKEN_URL =
    "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token";

// JWT grant type for service accounts
constexpr std::string_view JWT_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:jwt-bearer";

// GCS scopes
constexpr std::string_view GCS_SCOPE_READ_ONLY = "https://www.googleapis.com/auth/devstorage.read_only";
constexpr std::string_view GCS_SCOPE_READ_WRITE = "https://www.googleapis.com/auth/devstorage.read_write";

/**
 * URL-encode a string for use in application/x-www-form-urlencoded bodies.
 */
std::string urlEncode(std::string_view s)
{
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.' || c == '~') {
            result += c;
        } else {
            result += '%';
            result += "0123456789ABCDEF"[(c >> 4) & 0xF];
            result += "0123456789ABCDEF"[c & 0xF];
        }
    }
    return result;
}

/**
 * Base64url encode (URL-safe alphabet, no padding).
 * Used for JWT encoding.
 */
std::string base64urlEncode(std::string_view data)
{
    auto encoded = base64::encode(std::as_bytes(std::span<const char>{data.data(), data.size()}));

    // Convert to URL-safe alphabet and remove padding
    for (char & c : encoded) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }

    // Remove trailing '=' padding
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }

    return encoded;
}

/**
 * Check if running on GCE by probing the metadata server.
 * Uses a single attempt with no retries to fail quickly on non-GCE.
 */
bool isRunningOnGce()
{
    static std::once_flag flag;
    static bool result = false;

    std::call_once(flag, []() {
        try {
            FileTransferRequest req(VerbatimURL{std::string(GCE_METADATA_TOKEN_URL)});
            req.headers.emplace_back("Metadata-Flavor", "Google");

            FileTransferSettings gceProbeFileTransferSettings(fileTransferSettings);
            gceProbeFileTransferSettings.tries = 1; // Single attempt, no retries - fail fast on non-GCE
            ref<FileTransfer> gceProbeFileTransfer = makeFileTransfer(gceProbeFileTransferSettings);

            gceProbeFileTransfer->download(req);
            result = true;
            debug("GCE metadata server detected");
        } catch (...) {
            // Not on GCE, or metadata server not accessible
        }
    });

    return result;
}

/**
 * Find the ADC credential file path.
 * Order:
 *   1. GOOGLE_APPLICATION_CREDENTIALS env var
 *   2. ~/.config/gcloud/application_default_credentials.json
 */
std::optional<std::filesystem::path> findCredentialFile()
{
    // 1. Check GOOGLE_APPLICATION_CREDENTIALS
    if (auto envPath = getEnv("GOOGLE_APPLICATION_CREDENTIALS")) {
        auto path = std::filesystem::path(*envPath);
        if (!std::filesystem::exists(path))
            throw GcsAuthError("GOOGLE_APPLICATION_CREDENTIALS names a file that does not exist: %s", *envPath);
        debug("Using GCS credentials from GOOGLE_APPLICATION_CREDENTIALS: %s", path.string());
        return path;
    }

    // 2. Check well-known ADC location
    auto adcPath = getHome() / ".config" / "gcloud" / "application_default_credentials.json";
    if (std::filesystem::exists(adcPath)) {
        debug("Using GCS credentials from default location: %s", adcPath.string());
        return adcPath;
    }

    return std::nullopt;
}

/**
 * Load and parse a credential JSON file.
 */
nlohmann::json loadCredentialFile(const std::filesystem::path & path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        throw GcsAuthError("Cannot open credential file: %s", path.string());
    }

    try {
        return nlohmann::json::parse(file);
    } catch (nlohmann::json::parse_error & e) {
        throw GcsAuthError("Invalid JSON in credential file %s: %s", path.string(), e.what());
    }
}

/**
 * Sign data with RSA-SHA256 using the given PEM private key.
 */
std::string rsaSha256Sign(std::string_view data, const std::string & privateKeyPem)
{
    // Load the private key
    UniqueBio bio(BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size())));
    if (!bio) {
        throw GcsAuthError("Failed to create BIO for private key");
    }

    UniqueEvpPkey pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!pkey) {
        unsigned long err = ERR_get_error();
        char errBuf[256];
        ERR_error_string_n(err, errBuf, sizeof(errBuf));
        throw GcsAuthError("Failed to parse service account private key: %s", errBuf);
    }

    // Create signing context
    UniqueEvpMdCtx ctx(EVP_MD_CTX_new());
    if (!ctx) {
        throw GcsAuthError("Failed to create EVP_MD_CTX");
    }

    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1) {
        throw GcsAuthError("EVP_DigestSignInit failed");
    }

    if (EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
        throw GcsAuthError("EVP_DigestSignUpdate failed");
    }

    // Determine signature length
    size_t sigLen = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sigLen) != 1) {
        throw GcsAuthError("EVP_DigestSignFinal (length query) failed");
    }

    // Allocate and get signature
    std::vector<unsigned char> sigBuf(sigLen);
    if (EVP_DigestSignFinal(ctx.get(), sigBuf.data(), &sigLen) != 1) {
        throw GcsAuthError("EVP_DigestSignFinal failed");
    }

    return std::string(reinterpret_cast<char *>(sigBuf.data()), sigLen);
}

/**
 * Create a signed JWT for service account authentication.
 */
std::string
createServiceAccountJwt(const std::string & clientEmail, const std::string & privateKey, std::string_view scope)
{
    auto now = std::chrono::system_clock::now();
    auto iat = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    auto exp = iat + 3600; // 1 hour

    // JWT Header
    nlohmann::json header = {{"alg", "RS256"}, {"typ", "JWT"}};

    // JWT Claims
    nlohmann::json claims = {
        {"iss", clientEmail},
        {"sub", clientEmail},
        {"aud", TOKEN_URI},
        {"iat", iat},
        {"exp", exp},
        {"scope", scope},
    };

    auto headerB64 = base64urlEncode(header.dump());
    auto claimsB64 = base64urlEncode(claims.dump());
    auto signatureInput = headerB64 + "." + claimsB64;

    // Sign with RSA-SHA256
    auto signature = rsaSha256Sign(signatureInput, privateKey);
    auto signatureB64 = base64urlEncode(signature);

    return signatureInput + "." + signatureB64;
}

} // anonymous namespace

class GcsCredentialProviderImpl : public GcsCredentialProvider
{
public:
    ~GcsCredentialProviderImpl() override;

    std::string getAccessToken(bool writable) override
    {
        // First, check cache under lock
        {
            std::lock_guard<std::mutex> lock(mutex);

            auto & cachedToken = writable ? cachedTokenReadWrite : cachedTokenReadOnly;
            if (cachedToken && !cachedToken->isExpired()) {
                return cachedToken->token;
            }

            // Load credentials if not yet loaded (this is fast, file I/O only)
            if (!credentialsLoaded) {
                loadCredentials();
            }
        }

        // Refresh token without holding the lock (HTTP call can be slow)
        auto scope = writable ? GCS_SCOPE_READ_WRITE : GCS_SCOPE_READ_ONLY;
        auto newToken = refreshToken(scope);

        // Store the new token under lock
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto & cachedToken = writable ? cachedTokenReadWrite : cachedTokenReadOnly;

            // Another thread may have refreshed while we were waiting.
            // Use the newer token (longer expiry).
            if (!cachedToken || cachedToken->expiresAt < newToken.expiresAt) {
                cachedToken = newToken;
            }
            return cachedToken->token;
        }
    }

private:
    std::mutex mutex;
    std::optional<GcsAccessToken> cachedTokenReadOnly;
    std::optional<GcsAccessToken> cachedTokenReadWrite;
    bool credentialsLoaded = false;

    // Credential type: "authorized_user", "service_account", or "gce_metadata"
    std::string credentialType;

    // The file the credentials were loaded from, if any (for error messages)
    std::optional<std::filesystem::path> credentialFile;

    // For authorized_user
    std::string clientId;
    std::string clientSecret;
    std::string refreshTokenValue;

    // For service_account
    std::string clientEmail;
    std::string privateKey;

    void loadCredentials()
    {
        // First, check for credential file (highest priority)
        auto credPath = findCredentialFile();
        if (credPath) {
            auto json = loadCredentialFile(*credPath);
            auto & obj = getObject(json);

            credentialFile = *credPath;
            credentialType = getString(valueAt(obj, "type"));

            if (credentialType == "authorized_user") {
                clientId = getString(valueAt(obj, "client_id"));
                clientSecret = getString(valueAt(obj, "client_secret"));
                refreshTokenValue = getString(valueAt(obj, "refresh_token"));
                debug("Loaded authorized_user credentials");
            } else if (credentialType == "service_account") {
                clientEmail = getString(valueAt(obj, "client_email"));
                privateKey = getString(valueAt(obj, "private_key"));
                debug("Loaded service_account credentials for %s", clientEmail);
            } else {
                throw GcsAuthError("Unsupported GCS credential type: %s", credentialType);
            }

            credentialsLoaded = true;
            return;
        }

        // Fall back to GCE metadata server if running on Google Cloud
        if (isRunningOnGce()) {
            credentialType = "gce_metadata";
            debug("Using GCE metadata server for credentials");
            credentialsLoaded = true;
            return;
        }

        throw GcsNoCredentials(
            "No GCS credentials found. Run 'gcloud auth application-default login', "
            "set GOOGLE_APPLICATION_CREDENTIALS, or run on GCE/Cloud Run/GKE");
    }

    GcsAccessToken refreshToken(std::string_view scope)
    {
        if (credentialType == "authorized_user") {
            return refreshAuthorizedUserToken();
        } else if (credentialType == "service_account") {
            return refreshServiceAccountToken(scope);
        } else if (credentialType == "gce_metadata") {
            return refreshGceMetadataToken();
        }
        throw GcsAuthError("Unknown credential type: %s", credentialType);
    }

    GcsAccessToken refreshAuthorizedUserToken()
    {
        debug("Refreshing GCS access token using authorized_user refresh token");

        // Note: authorized_user tokens use the scopes granted at `gcloud auth` time,
        // not per-request scopes. The scope parameter is not used here.

        // POST to token endpoint with refresh_token grant
        std::string body = "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(clientSecret)
                           + "&refresh_token=" + urlEncode(refreshTokenValue) + "&grant_type=refresh_token";

        FileTransferRequest req(VerbatimURL{std::string(TOKEN_URI)});
        req.method = HttpMethod::Post;
        req.mimeType = "application/x-www-form-urlencoded";

        StringSource bodySource(body);
        req.data = FileTransferRequest::UploadData(bodySource);

        try {
            auto result = getFileTransfer()->upload(req);
            return parseTokenResponse(result.data);
        } catch (Error & e) {
            /* Typically an HTTP 400 `invalid_grant`: the login was revoked, the
               password changed, or the session expired. */
            e.addTrace(
                {},
                "while refreshing the gcloud application default credentials in '%s'; "
                "running 'gcloud auth application-default login' again may help",
                credentialFile ? credentialFile->string() : "<unknown>");
            throw;
        }
    }

    GcsAccessToken refreshServiceAccountToken(std::string_view scope)
    {
        debug("Refreshing GCS access token using service_account JWT (scope: %s)", scope);

        // Create and sign JWT assertion with the requested scope
        auto jwt = createServiceAccountJwt(clientEmail, privateKey, scope);

        std::string body = "grant_type=" + urlEncode(std::string(JWT_GRANT_TYPE)) + "&assertion=" + urlEncode(jwt);

        FileTransferRequest req(VerbatimURL{std::string(TOKEN_URI)});
        req.method = HttpMethod::Post;
        req.mimeType = "application/x-www-form-urlencoded";

        StringSource bodySource(body);
        req.data = FileTransferRequest::UploadData(bodySource);

        auto result = getFileTransfer()->upload(req);
        return parseTokenResponse(result.data);
    }

    GcsAccessToken refreshGceMetadataToken()
    {
        debug("Refreshing GCS access token from GCE metadata server");

        // GCE metadata server provides tokens for the instance's service account.
        // Scopes are determined by the instance configuration, not per-request.
        FileTransferRequest req(VerbatimURL{std::string(GCE_METADATA_TOKEN_URL)});
        req.headers.emplace_back("Metadata-Flavor", "Google");

        auto result = getFileTransfer()->download(req);
        return parseTokenResponse(result.data);
    }

    GcsAccessToken parseTokenResponse(const std::string & response)
    {
        try {
            auto json = nlohmann::json::parse(response);
            auto & obj = getObject(json);

            // Check for error response
            if (auto * errPtr = optionalValueAt(obj, "error")) {
                auto error = getString(*errPtr);
                auto description = optionalValueAt(obj, "error_description");
                throw GcsAuthError(
                    "OAuth2 token request failed: %s%s", error, description ? (" - " + getString(*description)) : "");
            }

            auto accessToken = getString(valueAt(obj, "access_token"));
            auto expiresIn = getInteger<int64_t>(valueAt(obj, "expires_in"));

            debug("Obtained GCS access token, expires in %d seconds", expiresIn);

            return GcsAccessToken{
                .token = accessToken, .expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn)};
        } catch (nlohmann::json::exception & e) {
            throw GcsAuthError("Failed to parse OAuth2 token response: %s\nResponse: %s", e.what(), response);
        }
    }
};

void GcsAuthError::anchor() {}

void GcsNoCredentials::anchor() {}

GcsCredentialProvider::~GcsCredentialProvider() {}

GcsCredentialProviderImpl::~GcsCredentialProviderImpl() {}

std::optional<std::string> GcsCredentialProvider::maybeGetAccessToken(bool writable)
{
    try {
        return getAccessToken(writable);
    } catch (GcsNoCredentials & e) {
        debug("no GCS credentials, proceeding without authentication: %s", e.message());
        return std::nullopt;
    }
}

/**
 * A credential provider that asks the `gcs-credential-helper` command, a
 * Git credential helper (gitcredentials(7)), the way Go's `GOAUTH=git` or
 * Cargo do: Nix runs `<command> get` with the request on its standard
 * input (`protocol`, `host`, `path`) and reads attributes back. GCS
 * authenticates with a bearer token, so the answer must be
 * `authtype=Bearer` with `credential=<token>` (or, from a helper without
 * the authtype capability, `password=<token>`), and `password_expiry_utc`
 * says when it expires.
 *
 * The helper's expiry is the only lifetime policy here. A token is reused
 * until shortly before it; a helper that reports no expiry is run for every
 * request, since guessing how long its tokens last would be a second
 * policy that works only while it happens to agree with the helper's own.
 *
 * @see https://git-scm.com/docs/git-credential#IOFMT
 */
class GcsCredentialHelperProvider : public GcsCredentialProvider
{
    /* Stop using a token this long before the helper says it expires. */
    static constexpr auto expiryMargin = std::chrono::seconds(60);

    const Strings command;
    const ParsedURL uri;

    std::mutex mutex;
    std::optional<std::string> token;
    std::chrono::steady_clock::time_point tokenValidUntil;
    bool warnedNoExpiry = false;

public:
    GcsCredentialHelperProvider(Strings command, ParsedURL uri)
        : command(std::move(command))
        , uri(std::move(uri))
    {
        assert(!this->command.empty());
    }

    ~GcsCredentialHelperProvider() override;

    /* The scope is up to the helper, so `writable` plays no part here. */
    std::string getAccessToken(bool writable) override
    {
        std::lock_guard<std::mutex> lock(mutex);

        auto now = std::chrono::steady_clock::now();
        if (token && now < tokenValidUntil)
            return *token;

        auto credential = parseResponse(runHelper());
        token = credential.token;
        if (credential.expiresIn)
            tokenValidUntil = now + std::max(*credential.expiresIn - expiryMargin, std::chrono::seconds(0));
        else {
            warnOnce(
                warnedNoExpiry,
                "%s does not report when its credentials expire, so it is run for every request; "
                "have it return 'password_expiry_utc' to avoid that",
                describe());
            tokenValidUntil = now;
        }
        return *token;
    }

private:
    struct Credential
    {
        std::string token;
        /** How much longer the token is valid, if the helper said. */
        std::optional<std::chrono::seconds> expiresIn;
    };

    std::string describe() const
    {
        return fmt("GCS credential helper '%s'", concatStringsSep(" ", command));
    }

    /**
     * The request for `uri`, as Git would write it with
     * `credential.useHttpPath` on. The `authtype` capability asks for a
     * `credential` rather than a password.
     */
    std::string request() const
    {
        std::string host = uri.authority ? uri.authority->host : "";
        if (uri.authority && uri.authority->port)
            host += fmt(":%d", *uri.authority->port);
        auto path = concatStringsSep("/", std::views::drop(uri.path, 1) | std::ranges::to<Strings>());
        return fmt("protocol=%s\nhost=%s\npath=%s\ncapability[]=authtype\n\n", uri.scheme, host, path);
    }

    /**
     * Interpret the helper's answer.
     */
    Credential parseResponse(const std::string & output) const
    {
        Credential credential;
        std::optional<std::string> password, authtype;

        for (auto & line : tokenizeString<Strings>(output, "\n")) {
            auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto key = line.substr(0, eq);
            auto value = line.substr(eq + 1);
            if (key == "credential")
                credential.token = value;
            else if (key == "password")
                password = value;
            else if (key == "authtype")
                authtype = value;
            else if (key == "password_expiry_utc") {
                auto expiry = string2Int<int64_t>(value);
                if (!expiry)
                    throw GcsAuthError(
                        "%s returned a 'password_expiry_utc' that is not a Unix time: '%s'", describe(), value);
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
                credential.expiresIn = std::chrono::seconds(*expiry - now);
            }
        }

        if (authtype && toLower(*authtype) != "bearer")
            throw GcsAuthError(
                "%s returned a '%s' credential, but GCS authenticates with a bearer token", describe(), *authtype);
        if (credential.token.empty() && password)
            credential.token = *password;
        if (credential.token.empty())
            throw GcsAuthError("%s returned no credential", describe());
        if (credential.expiresIn && *credential.expiresIn <= std::chrono::seconds(0))
            throw GcsAuthError(
                "%s returned a credential that expired %d s ago", describe(), -credential.expiresIn->count());

        debug(
            "obtained a GCS access token from the credential helper%s",
            credential.expiresIn ? fmt(", valid for %d s", credential.expiresIn->count()) : "");
        return credential;
    }

    /**
     * Run `<command> get` with the request on its standard input and return
     * what it printed. The request is a few dozen bytes, so it is written
     * in full before the answer is read; a pipe holds far more.
     */
    std::string runHelper()
    {
#ifdef _WIN32
        throw UnimplementedError("GCS credential helpers are not supported on this platform");
#else
        auto input = request();

        /* In a daemon worker, act as the client rather than as the daemon,
           so that per-user token brokers recognise the caller. */
        std::optional<daemon::Client> runAs;
        std::optional<OsStringMap> environment;
        if (auto client = daemon::getClient(); client && client->uid != geteuid()) {
            debug("running %s as uid %d", describe(), client->uid);
            runAs = client;
            environment = environmentFor(client->uid);
        }

        /* The helper's diagnostics go into the error, since in a daemon
           worker its stderr would otherwise reach only the daemon's log,
           and they are what tells the user what to do. */
        auto run = [&]() -> std::tuple<int, std::string, std::string> {
            checkInterrupt();
            Pipe in, out;
            in.create();
            out.create();
            auto [diagnosticsFd, diagnosticsPath] = createTempFile("nix-credential-helper");
            AutoDelete delDiagnostics(diagnosticsPath);

            Pid pid = startProcess(
                [&] {
                    if (environment)
                        replaceEnv(*environment);
                    if (dup2(in.readSide.get(), STDIN_FILENO) == -1)
                        throw SysError("dupping stdin");
                    if (dup2(out.writeSide.get(), STDOUT_FILENO) == -1)
                        throw SysError("dupping stdout");
                    if (dup2(diagnosticsFd.get(), STDERR_FILENO) == -1)
                        throw SysError("dupping stderr");
                    if (runAs) {
                        if (setgid(runAs->gid) == -1)
                            throw SysError("setgid failed");
                        if (setgroups(0, 0) == -1)
                            throw SysError("setgroups failed");
                        if (setuid(runAs->uid) == -1)
                            throw SysError("setuid failed");
                    }
                    Strings args(command);
                    args.push_back("get");
                    restoreProcessContext();
                    execvp(command.front().c_str(), stringsToCharPtrs(args).data());
                    throw SysError("executing %s", command.front());
                },
                {.allowVfork = false});

            in.readSide.close();
            out.writeSide.close();
            try {
                writeFull(in.writeSide.get(), input);
            } catch (SysError &) {
                /* A helper that doesn't read its input is not thereby
                   wrong; its exit status and output decide. */
            }
            in.writeSide.close();
            auto output = drainFD(out.readSide.get());
            auto status = pid.wait();
            diagnosticsFd.close();
            return {status, std::move(output), trim(readFile(diagnosticsPath))};
        };

        /* A helper typically asks a broker, which can fail transiently; one
           more try is cheap compared to failing the user's request. */
        auto [status, output, diagnostics] = run();
        if (!statusOk(status)) {
            debug("%s %s, retrying once", describe(), statusToString(status));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::tie(status, output, diagnostics) = run();
        }
        if (!statusOk(status))
            throw GcsAuthError(
                "%s %s%s", describe(), statusToString(status), diagnostics.empty() ? "" : ":\n" + diagnostics);
        if (!diagnostics.empty())
            debug("%s said: %s", describe(), diagnostics);

        return output;
#endif
    }

#ifndef _WIN32
    /**
     * A minimal environment for running the helper as `uid`: the user's
     * identity and home directory, so that the helper finds its own
     * per-user state, and nothing of the daemon's environment.
     */
    static OsStringMap environmentFor(uid_t uid)
    {
        struct passwd pwbuf;
        struct passwd * pw = nullptr;
        std::vector<char> buf(16384);
        if (getpwuid_r(uid, &pwbuf, buf.data(), buf.size(), &pw) != 0 || !pw || !pw->pw_dir || !pw->pw_dir[0])
            throw GcsAuthError("cannot look up the home directory of uid %d to run the GCS credential helper", uid);

        OsStringMap env;
        env["HOME"] = pw->pw_dir;
        env["USER"] = pw->pw_name;
        env["LOGNAME"] = pw->pw_name;
        env["PATH"] = "/usr/local/bin:/usr/bin:/bin";

        /* Where systemd puts a logged-in user's runtime directory; per-user
           services bind their sockets under it. */
        auto runtimeDir = fmt("/run/user/%d", uid);
        if (pathExists(runtimeDir))
            env["XDG_RUNTIME_DIR"] = runtimeDir;

        return env;
    }
#endif
};

GcsCredentialHelperProvider::~GcsCredentialHelperProvider() {}

ref<GcsCredentialProvider> makeGcsCredentialsProvider()
{
    return make_ref<GcsCredentialProviderImpl>();
}

ref<GcsCredentialProvider> makeGcsCredentialHelperProvider(Strings command, ParsedURL uri)
{
    if (command.empty())
        throw UsageError("gcs-credential-helper must name a command");
    return make_ref<GcsCredentialHelperProvider>(std::move(command), std::move(uri));
}

ref<GcsCredentialProvider> getGcsCredentialsProvider(const ParsedURL & uri)
{
    if (auto & helper = gcsSettings.credentialHelper.get(); !helper.empty())
        return makeGcsCredentialHelperProvider(helper, uri);

    static auto applicationDefault = makeGcsCredentialsProvider();
    return applicationDefault;
}

} // namespace nix
