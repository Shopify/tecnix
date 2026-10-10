#pragma once
///@file

#include "nix/store/config.hh"
#include "nix/store/gcs-creds.hh"
#include "nix/store/gcs-url.hh"
#include "nix/store/http-binary-cache-store.hh"
#include "nix/util/fun.hh"

namespace nix {

struct GcsBinaryCacheStoreConfig : HttpBinaryCacheStoreConfig
{
    GcsBinaryCacheStoreConfig(const Params & params)
        : StoreConfig(params, FilePathType::Unix)
        , HttpBinaryCacheStoreConfig(params)
    {
    }

    GcsBinaryCacheStoreConfig(ParsedURL cacheUri, const Params & params);

    GcsBinaryCacheStoreConfig(std::string_view bucketName, const Params & params);

    Setting<std::string> endpoint{
        this,
        "",
        "endpoint",
        R"(
          The GCS endpoint to use. When empty (default), the public
          `https://storage.googleapis.com` endpoint is used. This is mainly
          useful for testing against a GCS emulator, e.g.
          `endpoint=http://localhost:4443`.
        )"};

    /**
     * Set of settings that are part of the GCS URI itself, i.e. that
     * influence how `gs://` URLs are turned into HTTP requests.
     */
    const std::set<const AbstractSetting *> gcsUriSettings = {&endpoint};

    static const std::string name()
    {
        return "GCS Binary Cache Store";
    }

    static StringSet uriSchemes();

    static std::string doc();

    std::string getHumanReadableURI() const override;

    ref<Store> openStore() const override;
};

/**
 * A binary cache store backed by a Google Cloud Storage bucket.
 *
 * Objects are read through the XML API (path-style) and written with
 * the JSON API's simple upload. Requests carry an OAuth2 bearer token
 * from a `GcsCredentialProvider`.
 */
class GcsBinaryCacheStore : public virtual HttpBinaryCacheStore
{
public:
    using Config = GcsBinaryCacheStoreConfig;

    ref<Config> gcsConfig;

    GcsBinaryCacheStore(
        ref<Config> config,
        ref<FileTransfer> fileTransfer = getFileTransfer(),
        ref<GcsCredentialProvider> credentials = getGcsCredentialsProvider());

protected:

    ref<GcsCredentialProvider> credentials;

    /**
     * GCS answers 404 for a missing object and 403 only for denied
     * access, so a 403 is a credential problem to report, not a miss.
     */
    bool isMissing(const FileTransferError & e) const override
    {
        return e.error == FileTransfer::NotFound;
    }

    /**
     * Like `HttpBinaryCacheStore::init()`, except that a credential
     * problem doesn't stop the store from opening: a substituter that
     * fails to open is dropped with a warning, so that an unreachable
     * cache doesn't stop builds, which would quietly turn wrong
     * credentials into building from source. The store stays registered
     * and its first query fails with the same error, where it stops the
     * build.
     */
    void init() override
    {
        initDeferringCredentialErrors([&] { HttpBinaryCacheStore::init(); });
    }

    /**
     * Run the initialisation `step`, letting a credential error wait for
     * the store's first use.
     */
    void initDeferringCredentialErrors(fun<void()> step);

    /**
     * Turn the `gs://` URL for `path` into an authenticated (read-only)
     * HTTPS request against the GCS XML API.
     */
    FileTransferRequest makeRequest(std::string_view path) override;

    void upsertFile(
        const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint) override;

private:

    /**
     * Upload a file with the JSON API's "simple upload".
     *
     * @param contentEncoding The object's `Content-Encoding` metadata, if
     * the file is compressed.
     *
     * @see https://cloud.google.com/storage/docs/json_api/v1/how-tos/simple-upload
     */
    void upload(
        std::string_view path,
        RestartableSource & source,
        uint64_t size,
        std::string_view mimeType,
        std::optional<std::string> contentEncoding);
};

} // namespace nix
