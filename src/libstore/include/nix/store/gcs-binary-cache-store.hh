#pragma once
///@file

#include "nix/store/config.hh"
#include "nix/store/gcs-creds.hh"
#include "nix/store/gcs-url.hh"
#include "nix/store/http-binary-cache-store.hh"

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
