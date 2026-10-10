#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/gcs-creds.hh"
#include "nix/store/gcs-url.hh"
#include "nix/store/http-binary-cache-store.hh"
#include "nix/store/store-registration.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/compression.hh"
#include "nix/util/serialise.hh"

namespace nix {

MakeError(UploadToGcs, Error);

void UploadToGcs::anchor() {}

class GcsBinaryCacheStore : public virtual HttpBinaryCacheStore
{
public:
    GcsBinaryCacheStore(ref<GcsBinaryCacheStoreConfig> config)
        : Store{*config}
        , BinaryCacheStore{*config}
        , HttpBinaryCacheStore{config}
    {
    }

    void upsertFile(
        const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint) override;

private:
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
            StringSource compressed(compress(*compressionMethod, source));
            Headers headers = {{"Content-Encoding", showCompressionAlgo(*compressionMethod)}};
            upload(path, compressed, compressed.s.size(), mimeType, std::move(headers));
        } else {
            upload(path, source, sizeHint, mimeType, std::nullopt);
        }
    } catch (FileTransferError & e) {
        UploadToGcs err(e.message());
        err.addTrace({}, "while uploading to GCS binary cache at '%s'", config->cacheUri.to_string());
        throw std::move(err);
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

    /* The object this file is: the same URL a download of it uses, so a cache
       under a prefix (gs://bucket/prefix) uploads under that prefix too. The
       cache URL itself has no object key and doesn't parse as one. */
    auto parsedGcs = ParsedGcsURL::parse(makeRequest(path).uri.parsed());

    // The GCS JSON API's simple upload:
    // POST <endpoint>/upload/storage/v1/b/{bucket}/o?uploadType=media&name={object}
    auto uploadUrl = parsedGcs.apiBase();
    uploadUrl.path = {"", "upload", "storage", "v1", "b", parsedGcs.bucket, "o"};
    uploadUrl.query["uploadType"] = "media";
    uploadUrl.query["name"] = concatStringsSep("/", parsedGcs.key);

    /* A simple upload takes the object's contentEncoding as a query parameter
       (objects.insert), not from a Content-Encoding header: the header would
       describe the request body, not the stored object. Without it, a
       compressed narinfo or listing would be served back still compressed. */
    Headers otherHeaders;
    for (auto & [name, value] : headers.value_or(Headers{})) {
        if (toLower(name) == "content-encoding")
            uploadUrl.query["contentEncoding"] = value;
        else
            otherHeaders.emplace_back(name, value);
    }

    FileTransferRequest req(VerbatimURL{uploadUrl});
    req.method = HttpMethod::Post;
    req.headers.insert(req.headers.end(), otherHeaders.begin(), otherHeaders.end());

    // Authenticate with write scope via OAuth2. Google's endpoint always needs
    // credentials; another endpoint (an emulator) never gets them.
    if (parsedGcs.sendsCredentials())
        req.bearerToken = getGcsCredentialsProvider()->getAccessToken(/* writable = */ true);

    req.data = {sizeHint, source};
    req.mimeType = mimeType;

    getFileTransfer()->upload(req);
}

StringSet GcsBinaryCacheStoreConfig::uriSchemes()
{
    return {"gs"};
}

GcsBinaryCacheStoreConfig::GcsBinaryCacheStoreConfig(ParsedURL cacheUri_, const Params & params)
    : StoreConfig(params, FilePathType::Unix)
    , HttpBinaryCacheStoreConfig(std::move(cacheUri_), params)
{
    assert(cacheUri.scheme == "gs");

    /* Requests are built from cacheUri, and `gs://` requests read the
       endpoint from its query, so the endpoint travels there. */
    if (!endpoint.get().empty())
        cacheUri.query["endpoint"] = endpoint.get();
}

GcsBinaryCacheStoreConfig::GcsBinaryCacheStoreConfig(std::string_view bucketName, const Params & params)
    : GcsBinaryCacheStoreConfig(
          ParsedURL{.scheme = "gs", .authority = ParsedURL::Authority{.host = std::string(bucketName)}}, params)
{
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
