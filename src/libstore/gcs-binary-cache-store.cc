#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/gcs-creds.hh"
#include "nix/store/gcs-url.hh"
#include "nix/store/http-binary-cache-store.hh"
#include "nix/store/store-registration.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"

namespace nix {

MakeError(UploadToGcs, Error);

class GcsBinaryCacheStore : public virtual HttpBinaryCacheStore
{
public:
    GcsBinaryCacheStore(ref<GcsBinaryCacheStoreConfig> config)
        : Store{*config}
        , BinaryCacheStore{*config}
        , HttpBinaryCacheStore{config}
        , gcsConfig{config}
    {
    }

    void upsertFile(
        const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint) override;

private:
    ref<GcsBinaryCacheStoreConfig> gcsConfig;

    /**
     * Uploads a file to GCS using the JSON API simple upload.
     * Supports files up to 5 GiB, which is sufficient for binary cache objects.
     *
     * @see https://cloud.google.com/storage/docs/uploading-objects#upload-object-json
     */
    void upload(
        std::string_view path,
        RestartableSource & source,
        uint64_t sizeHint,
        std::string_view mimeType,
        std::optional<Headers> headers);
};

void GcsBinaryCacheStore::upsertFile(
    const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint)
{
    try {
        if (auto compressionMethod = getCompressionMethod(path)) {
            CompressedSource compressed(source, *compressionMethod);
            Headers headers = {{"Content-Encoding", *compressionMethod}};
            upload(path, compressed, compressed.size(), mimeType, std::move(headers));
        } else {
            upload(path, source, sizeHint, mimeType, std::nullopt);
        }
    } catch (FileTransferError & e) {
        UploadToGcs err(e.message());
        err.addTrace({}, "while uploading to GCS binary cache at '%s'", config->cacheUri.to_string());
        throw err;
    }
}

void GcsBinaryCacheStore::upload(
    std::string_view path,
    RestartableSource & source,
    uint64_t sizeHint,
    std::string_view mimeType,
    std::optional<Headers> headers)
{
    debug("uploading to GCS '%s' (%d bytes)", path, sizeHint);

    auto parsedGcs = ParsedGcsURL::parse(config->cacheUri);

    // Build the upload URL using the GCS JSON API:
    // POST https://storage.googleapis.com/upload/storage/v1/b/{bucket}/o?uploadType=media&name={object}
    ParsedURL uploadUrl;
    uploadUrl.scheme = "https";
    uploadUrl.authority = ParsedURL::Authority{.host = "storage.googleapis.com"};
    uploadUrl.path = {"", "upload", "storage", "v1", "b", parsedGcs.bucket, "o"};
    uploadUrl.query["uploadType"] = "media";
    uploadUrl.query["name"] = std::string{path};

    FileTransferRequest req(uploadUrl);
    req.method = HttpMethod::Post;

    // Authenticate with write scope via OAuth2
    auto token = getGcsCredentialsProvider()->getAccessToken(/* writable = */ true);
    req.bearerToken = std::move(token);

    if (headers) {
        req.headers.reserve(req.headers.size() + headers->size());
        std::move(headers->begin(), headers->end(), std::back_inserter(req.headers));
    }

    if (auto storageClass = gcsConfig->storageClass.get()) {
        req.headers.emplace_back("x-goog-storage-class", *storageClass);
    }

    req.data = {sizeHint, source};
    req.mimeType = mimeType;

    getFileTransfer()->upload(req);
}

StringSet GcsBinaryCacheStoreConfig::uriSchemes()
{
    return {"gs"};
}

GcsBinaryCacheStoreConfig::GcsBinaryCacheStoreConfig(
    std::string_view scheme, std::string_view _cacheUri, const Params & params)
    : StoreConfig(params)
    , HttpBinaryCacheStoreConfig(scheme, _cacheUri, params)
{
    assert(cacheUri.scheme == "gs");
}

std::string GcsBinaryCacheStoreConfig::getHumanReadableURI() const
{
    return getReference().render();
}

std::string GcsBinaryCacheStoreConfig::doc()
{
    return
#include "gcs-binary-cache-store.md"
        ;
}

ref<Store> GcsBinaryCacheStoreConfig::openStore() const
{
    auto sharedThis = std::const_pointer_cast<GcsBinaryCacheStoreConfig>(
        std::static_pointer_cast<const GcsBinaryCacheStoreConfig>(shared_from_this()));
    return make_ref<GcsBinaryCacheStore>(ref{sharedThis});
}

static RegisterStoreImplementation<GcsBinaryCacheStoreConfig> registerGcsBinaryCacheStore;

} // namespace nix
