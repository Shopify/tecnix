#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/store-registration.hh"
#include "nix/util/compression.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"

#include <cassert>
#include <ranges>

namespace nix {

MakeError(UploadToGcs, Error);

void UploadToGcs::anchor() {}

GcsBinaryCacheStore::GcsBinaryCacheStore(
    ref<Config> config, ref<FileTransfer> fileTransfer, ref<GcsCredentialProvider> credentials)
    : Store{*config}
    , BinaryCacheStore{*config}
    , HttpBinaryCacheStore{config, fileTransfer}
    , gcsConfig{config}
    , credentials{credentials}
{
}

void GcsBinaryCacheStore::initDeferringCredentialErrors(fun<void()> step)
{
    auto defer = [&](Error & e) {
        debug("deferring a credential error for '%s' to its first use: %s", config->cacheUri.to_string(), e.what());
        /* Nothing is known about this cache, so there is nothing to look up
           or record in the narinfo disk cache; its first use fails anyway. */
        diskCache = nullptr;
    };
    try {
        step();
    } catch (GcsAuthError & e) {
        defer(e);
    } catch (FileTransferError & e) {
        if (e.error != FileTransfer::Forbidden && e.error != FileTransfer::Unauthorized)
            throw;
        defer(e);
    }
}

FileTransferRequest GcsBinaryCacheStore::makeRequest(std::string_view path)
{
    auto request = HttpBinaryCacheStore::makeRequest(path);
    request.uri = ParsedGcsURL::parse(request.uri.parsed()).toHttpsUrl();

    /* Reads may be anonymous, e.g. from a public bucket. */
    if (auto token = credentials->maybeGetAccessToken(/* writable = */ false))
        request.bearerToken = std::move(*token);

    return request;
}

void GcsBinaryCacheStore::upsertFile(
    const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint)
{
    try {
        if (auto compressionMethod = getCompressionMethod(path)) {
            StringSource compressed(compress(*compressionMethod, source));
            upload(path, compressed, compressed.s.size(), mimeType, showCompressionAlgo(*compressionMethod));
        } else {
            upload(path, source, sizeHint, mimeType, std::nullopt);
        }
    } catch (FileTransferError & e) {
        UploadToGcs err(std::move(e.info()));
        err.addTrace({}, "while uploading to GCS binary cache at '%s'", config->cacheUri.to_string());
        throw err;
    }
}

void GcsBinaryCacheStore::upload(
    std::string_view path,
    RestartableSource & source,
    uint64_t size,
    std::string_view mimeType,
    std::optional<std::string> contentEncoding)
{
    debug("uploading '%s' to GCS (%s)", path, renderSize(size));

    /* The object this file is: the same URL a download of it uses, so a
       cache under a prefix (gs://bucket/prefix) uploads under that prefix
       too. */
    auto object = ParsedGcsURL::parse(HttpBinaryCacheStore::makeRequest(path).uri.parsed());

    /* POST <endpoint>/upload/storage/v1/b/<bucket>/o?uploadType=media&name=<key>

       The content encoding is object metadata, hence a query parameter: as a
       `Content-Encoding` header it would describe the request body instead,
       and the object would be served back still compressed. */
    auto url = object.endpointUrl();
    url.path.insert(url.path.end(), {"upload", "storage", "v1", "b", object.bucket, "o"});
    url.query["uploadType"] = "media";
    url.query["name"] = concatStringsSep("/", object.key);
    if (contentEncoding)
        url.query["contentEncoding"] = *contentEncoding;

    FileTransferRequest request(std::move(url));
    request.method = HttpMethod::Post;
    request.data = {size, source};
    request.mimeType = mimeType;

    /* Google's endpoint requires credentials for writes; another endpoint
       (an emulator, say) gets them only if there are any. */
    if (std::holds_alternative<std::monostate>(object.endpoint))
        request.bearerToken = credentials->getAccessToken(/* writable = */ true);
    else if (auto token = credentials->maybeGetAccessToken(/* writable = */ true))
        request.bearerToken = std::move(*token);

    fileTransfer->upload(request);
}

StringSet GcsBinaryCacheStoreConfig::uriSchemes()
{
    return {"gs"};
}

GcsBinaryCacheStoreConfig::GcsBinaryCacheStoreConfig(ParsedURL cacheUri_, const Params & params)
    : StoreConfig(params, FilePathType::Unix)
    , HttpBinaryCacheStoreConfig(std::move(cacheUri_), params)
{
    assert(cacheUri.query.empty());
    assert(cacheUri.scheme == "gs");

    /* Settings that affect the request URLs are kept in the cache URI, so
       that `makeRequest` can propagate them to every request. */
    for (const auto & [key, value] : params) {
        auto gcsParams =
            std::views::transform(gcsUriSettings, [](const AbstractSetting * setting) { return setting->name; });
        if (std::ranges::contains(gcsParams, key))
            cacheUri.query[key] = value;
    }
}

GcsBinaryCacheStoreConfig::GcsBinaryCacheStoreConfig(std::string_view bucketName, const Params & params)
    : GcsBinaryCacheStoreConfig(
          ParsedURL{.scheme = "gs", .authority = ParsedURL::Authority{.host = std::string(bucketName)}}, params)
{
}

std::string GcsBinaryCacheStoreConfig::getHumanReadableURI() const
{
    auto reference = getReference();
    reference.params = [&]() {
        Params relevantParams;
        for (auto & setting : gcsUriSettings)
            if (setting->overridden)
                relevantParams.insert({setting->name, reference.params.at(setting->name)});
        return relevantParams;
    }();
    return reference.render();
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
