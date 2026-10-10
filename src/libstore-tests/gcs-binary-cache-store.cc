#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/gcs-creds.hh"
#include "nix/store/http-binary-cache-store.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/archive.hh"
#include "nix/util/compression.hh"
#include "nix/util/strings.hh"
#include "nix/util/tests/gmock-matchers.hh"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <map>
#include <ranges>

namespace nix {

using namespace std::string_literals;

TEST(GcsBinaryCacheStoreConfig, constructConfig)
{
    GcsBinaryCacheStoreConfig config{"foobar", {}};

    EXPECT_EQ(
        config.cacheUri,
        (ParsedURL{
            .scheme = "gs",
            .authority = ParsedURL::Authority{.host = "foobar"},
        }));
    EXPECT_EQ(config.getHumanReadableURI(), "gs://foobar");
    EXPECT_EQ(config.endpoint.get(), "");
}

TEST(GcsBinaryCacheStoreConfig, constructConfigWithPrefix)
{
    GcsBinaryCacheStoreConfig config{parseURL("gs://my-bucket/some/prefix/"), {}};

    EXPECT_EQ(config.cacheUri.to_string(), "gs://my-bucket/some/prefix");
}

/**
 * Only the settings that affect request URLs end up in the cache URI,
 * so that `makeRequest` can propagate them to every request.
 */
TEST(GcsBinaryCacheStoreConfig, parameterFiltering)
{
    StringMap params;
    params["endpoint"] = "http://localhost:4443";
    params["priority"] = "10";

    GcsBinaryCacheStoreConfig config("test-bucket", params);

    EXPECT_EQ(
        config.cacheUri,
        (ParsedURL{
            .scheme = "gs",
            .authority = ParsedURL::Authority{.host = "test-bucket"},
            .query = (StringMap) {{"endpoint", "http://localhost:4443"}},
        }));
    EXPECT_EQ(config.priority.get(), 10);

    /* All params are part of the reference, but only the URI settings are shown to humans. */
    EXPECT_EQ(config.getReference().params["priority"], "10");
    EXPECT_EQ(parseURL(config.getHumanReadableURI()).query, (StringMap{{"endpoint", "http://localhost:4443"}}));
}

TEST(GcsBinaryCacheStoreConfig, schemeRegistration)
{
    EXPECT_TRUE(GcsBinaryCacheStoreConfig::uriSchemes().contains("gs"));
    EXPECT_FALSE(HttpBinaryCacheStoreConfig::uriSchemes().contains("gs"));
}

namespace {

/**
 * The parts of the GCS XML and JSON APIs that `GcsBinaryCacheStore` uses,
 * as an in-memory `FileTransfer`: objects are read at
 * `/<bucket>/<key>` and written with the JSON API's simple upload.
 */
struct FakeGcs : FileTransfer
{
    struct Object
    {
        std::string data;
        std::string contentType;
        std::optional<std::string> contentEncoding;
    };

    struct Request
    {
        HttpMethod method;
        ParsedURL url;
        Headers headers;
        std::optional<std::string> bearerToken;

        /** The object the request is about: from the XML path, or the JSON upload's `name`. */
        std::string key() const
        {
            if (auto name = url.query.find("name"); name != url.query.end())
                return name->second;
            return concatStringsSep("/", std::views::drop(url.path, 2) | std::ranges::to<std::vector<std::string>>());
        }
    };

    static constexpr std::string_view readToken = "read-only-token";
    static constexpr std::string_view writeToken = "read-write-token";

    std::string bucket;
    /** Where the fake is served; the default is the real service's address. */
    ParsedURL base{.scheme = "https", .authority = ParsedURL::Authority{.host = "storage.googleapis.com"}};
    /** Whether writes without a token are accepted, as an emulator does. */
    bool anonymousWrites = false;
    /** Like a private bucket: reads without a token are denied. */
    bool privateReads = false;
    std::map<std::string, Object> objects;
    std::vector<Request> requests;

    FakeGcs(std::string bucket)
        : bucket(std::move(bucket))
    {
    }

    std::vector<Request> requestsFor(std::string_view key) const
    {
        std::vector<Request> res;
        for (auto & r : requests)
            if (r.key() == key)
                res.push_back(r);
        return res;
    }

    ItemHandle
    enqueueFileTransfer(const FileTransferRequest & request, Callback<FileTransferResult> callback) noexcept override
    {
        try {
            callback(handle(request));
        } catch (...) {
            callback.rethrow();
        }
        return ItemHandle(std::weak_ptr<Item>());
    }

    void unpauseTransfer(ItemHandle) override {}

    FileTransferResult handle(const FileTransferRequest & request)
    {
        auto url = request.uri.parsed();
        requests.push_back({request.method, url, request.headers, request.bearerToken});

        if (url.scheme != base.scheme || url.authority != base.authority)
            throw nix::Error("fake GCS: unexpected URL '%s'", url);

        FileTransferResult result;

        /* POST /upload/storage/v1/b/<bucket>/o?uploadType=media&name=<key> */
        if (url.path == std::vector<std::string>{"", "upload", "storage", "v1", "b", bucket, "o"}) {
            if (request.method != HttpMethod::Post || url.query.at("uploadType") != "media" || !request.data)
                throw nix::Error("fake GCS: unsupported upload '%s'", url);
            if (request.bearerToken != writeToken && !(anonymousWrites && !request.bearerToken))
                throw FileTransferError(
                    FileTransfer::Forbidden, std::nullopt, "fake GCS: write to '%s' without a read-write token", url);
            request.data->source->restart();
            StringSink body;
            request.data->source->drainInto(body);
            auto encoding = url.query.find("contentEncoding");
            objects[url.query.at("name")] = Object{
                .data = std::move(body.s),
                .contentType = request.mimeType,
                .contentEncoding =
                    encoding == url.query.end() ? std::nullopt : std::optional<std::string>(encoding->second),
            };
            return result;
        }

        /* GET or HEAD /<bucket>/<key> */
        if (url.path.size() < 3 || !url.path[0].empty() || url.path[1] != bucket
            || (request.method != HttpMethod::Get && request.method != HttpMethod::Head))
            throw nix::Error("fake GCS: unexpected %s of '%s'", request.verb(), url);

        if (privateReads && !request.bearerToken)
            throw FileTransferError(
                FileTransfer::Forbidden, std::nullopt, "fake GCS: anonymous caller may not read '%s'", url);

        auto obj = objects.find(requests.back().key());
        if (obj == objects.end())
            throw FileTransferError(FileTransfer::NotFound, std::nullopt, "fake GCS: '%s' does not exist", url);

        if (request.method == HttpMethod::Get) {
            if (request.dataCallback)
                request.dataCallback(obj->second.data);
            else
                result.data = obj->second.data;
        }
        return result;
    }
};

struct FakeGcsCredentials : GcsCredentialProvider
{
    std::string getAccessToken(bool writable) override
    {
        return std::string(writable ? FakeGcs::writeToken : FakeGcs::readToken);
    }
};

/** No credentials are configured at all. */
struct NoGcsCredentials : GcsCredentialProvider
{
    std::string getAccessToken(bool writable) override
    {
        throw GcsNoCredentials("no credentials");
    }
};

/** Credentials are configured, but unusable (e.g. a revoked gcloud login). */
struct BrokenGcsCredentials : GcsCredentialProvider
{
    std::string getAccessToken(bool writable) override
    {
        throw GcsAuthError("OAuth2 token request failed: invalid_grant");
    }
};

/**
 * Test shim: don't use the on-disk narinfo cache in unit tests, and expose
 * the file-level operations.
 */
class TestGcsBinaryCacheStore : public GcsBinaryCacheStore
{
public:
    TestGcsBinaryCacheStore(
        ref<GcsBinaryCacheStoreConfig> config, ref<FileTransfer> fileTransfer, ref<GcsCredentialProvider> credentials)
        : Store{*config}
        , BinaryCacheStore{*config}
        , HttpBinaryCacheStore{config, fileTransfer}
        , GcsBinaryCacheStore{config, fileTransfer, credentials}
    {
        diskCache = nullptr;
    }

    /* Without the narinfo disk cache that `HttpBinaryCacheStore::init()`
       consults, but with the GCS store's own treatment of credentials. */
    void init() override
    {
        initDeferringCredentialErrors([&] { BinaryCacheStore::init(); });
    }

    using BinaryCacheStore::fileExists;
    using BinaryCacheStore::getFile;
    using BinaryCacheStore::upsertFile;
};

class GcsBinaryCacheStoreTest : public LibStoreTest
{
protected:
    ref<FakeGcs> gcs = make_ref<FakeGcs>("test-bucket");

    /** The expected contents of `nix-cache-info`. */
    static std::string cacheInfo()
    {
        return "StoreDir: " + GcsBinaryCacheStoreConfig("test-bucket", {}).storeDir + "\n";
    }

    ref<TestGcsBinaryCacheStore> openStore(
        StoreConfig::Params params = {},
        std::string_view uri = "gs://test-bucket",
        ref<GcsCredentialProvider> credentials = make_ref<FakeGcsCredentials>())
    {
        params.insert({"compression", "none"});
        auto config = make_ref<GcsBinaryCacheStoreConfig>(parseURL(uri), params);
        config->pathInfoCacheSize = 0;
        auto store = make_ref<TestGcsBinaryCacheStore>(config, gcs, credentials);
        store->init();
        return store;
    }
};

} // namespace

/**
 * Opening an empty cache reads `nix-cache-info` through the XML API with
 * the read-only token and creates it with the JSON API's simple upload
 * and the read-write token.
 */
TEST_F(GcsBinaryCacheStoreTest, initCreatesCacheInfo)
{
    openStore();

    auto & obj = gcs->objects.at("nix-cache-info");
    EXPECT_EQ(obj.data, cacheInfo());
    EXPECT_EQ(obj.contentType, "text/x-nix-cache-info");

    auto reqs = gcs->requestsFor("nix-cache-info");
    ASSERT_EQ(reqs.size(), 2u);
    EXPECT_EQ(reqs[0].method, HttpMethod::Get);
    EXPECT_EQ(reqs[0].bearerToken, FakeGcs::readToken);
    EXPECT_EQ(reqs[0].url.to_string(), "https://storage.googleapis.com/test-bucket/nix-cache-info");
    EXPECT_EQ(reqs[1].method, HttpMethod::Post);
    EXPECT_EQ(reqs[1].bearerToken, FakeGcs::writeToken);
    EXPECT_EQ(
        reqs[1].url.to_string(),
        "https://storage.googleapis.com/upload/storage/v1/b/test-bucket/o?name=nix-cache-info&uploadType=media");
}

TEST_F(GcsBinaryCacheStoreTest, prefixIsPartOfTheKey)
{
    auto store = openStore({}, "gs://test-bucket/some/prefix");
    store->upsertFile("foo", "bar"s, "text/plain");

    EXPECT_TRUE(gcs->objects.contains("some/prefix/nix-cache-info"));
    EXPECT_EQ(gcs->objects.at("some/prefix/foo").data, "bar");
    EXPECT_TRUE(store->fileExists("foo"));
}

/**
 * A compressed narinfo's encoding is object metadata, passed to the simple
 * upload as a query parameter rather than as a header describing the body.
 */
TEST_F(GcsBinaryCacheStoreTest, uploadWithCompression)
{
    auto store = openStore({{"narinfo-compression", "gzip"}});
    store->upsertFile("abc.narinfo", "StorePath: /nix/store/abc\n"s, "text/x-nix-narinfo");

    auto & obj = gcs->objects.at("abc.narinfo");
    EXPECT_EQ(decompress(CompressionAlgo::gzip, obj.data), "StorePath: /nix/store/abc\n");
    EXPECT_EQ(obj.contentEncoding, "gzip");
    for (auto & [name, value] : gcs->requestsFor("abc.narinfo").at(0).headers)
        EXPECT_NE(name, "Content-Encoding");
}

TEST_F(GcsBinaryCacheStoreTest, readsMayBeAnonymousButWritesMayNot)
{
    gcs->objects["nix-cache-info"] = {.data = cacheInfo()};
    gcs->objects["foo"] = {.data = "bar"};
    auto store = openStore({}, "gs://test-bucket", make_ref<NoGcsCredentials>());

    EXPECT_EQ(store->getFile("foo"), "bar");
    for (auto & req : gcs->requests)
        EXPECT_EQ(req.bearerToken, std::nullopt);

    EXPECT_THROW(store->upsertFile("foo", "baz"s, "text/plain"), GcsAuthError);
}

/**
 * Google's endpoint always needs credentials for writes, but another
 * endpoint (an emulator) gets them only if there are any.
 */
TEST_F(GcsBinaryCacheStoreTest, writesToAnotherEndpointMayBeAnonymous)
{
    gcs->base = parseURL("http://gcs.local:4443");
    gcs->anonymousWrites = true;
    auto store = openStore({{"endpoint", "http://gcs.local:4443"}}, "gs://test-bucket", make_ref<NoGcsCredentials>());

    store->upsertFile("foo", "bar"s, "text/plain");

    EXPECT_EQ(gcs->objects.at("foo").data, "bar");
    for (auto & req : gcs->requests) {
        EXPECT_EQ(req.bearerToken, std::nullopt);
        EXPECT_EQ(req.url.authority->host, "gcs.local");
    }
}

/**
 * Credentials that exist but don't work are an error, never a fall back to
 * anonymous requests. Opening the store still succeeds, since a substituter
 * that fails to open is dropped with a warning (the quiet fall back again);
 * its first use fails instead, where that stops the build.
 */
TEST_F(GcsBinaryCacheStoreTest, brokenCredentialsAreAnError)
{
    gcs->objects["nix-cache-info"] = {.data = cacheInfo()};
    auto store = openStore({}, "gs://test-bucket", make_ref<BrokenGcsCredentials>());
    EXPECT_TRUE(gcs->requests.empty()) << "no anonymous request should have been made";

    EXPECT_THAT(
        [&] { store->queryPathInfo(StorePath("g25sx2bjrsxs0zj1pgskqhh2cka48wlh-foo")); },
        ::testing::ThrowsMessage<GcsAuthError>(testing::HasSubstrIgnoreANSIMatcher("invalid_grant")));
    EXPECT_TRUE(gcs->requests.empty()) << "no anonymous request should have been made";
}

/**
 * Denied access is reported, not taken for a cache miss: a 403 on a
 * private bucket must not quietly turn into building from source.
 */
TEST_F(GcsBinaryCacheStoreTest, deniedAccessIsAnErrorNotAMiss)
{
    gcs->privateReads = true;
    gcs->objects["nix-cache-info"] = {.data = cacheInfo()};
    auto config = make_ref<GcsBinaryCacheStoreConfig>(parseURL("gs://test-bucket"), StoreConfig::Params{});
    config->pathInfoCacheSize = 0;
    auto store = make_ref<TestGcsBinaryCacheStore>(config, gcs, make_ref<NoGcsCredentials>());

    EXPECT_THAT(
        [&] { store->fileExists("nix-cache-info"); },
        ::testing::ThrowsMessage<FileTransferError>(testing::HasSubstrIgnoreANSIMatcher("anonymous caller")));
    EXPECT_THROW(store->queryPathInfo(StorePath("g25sx2bjrsxs0zj1pgskqhh2cka48wlh-foo")), FileTransferError);
    /* Opening the store, though, succeeds: a substituter that fails to open
       is dropped with a warning, which is the quiet fall back again. */
    EXPECT_NO_THROW(store->init());
    EXPECT_THROW(store->queryPathInfo(StorePath("g25sx2bjrsxs0zj1pgskqhh2cka48wlh-foo")), FileTransferError);
}

TEST_F(GcsBinaryCacheStoreTest, missingFiles)
{
    auto store = openStore();

    EXPECT_FALSE(store->fileExists("nope"));
    EXPECT_EQ(gcs->requestsFor("nope").at(0).method, HttpMethod::Head);

    StringSink sink;
    EXPECT_THROW(store->getFile("nope", sink), NoSuchBinaryCacheFile);
}

/**
 * A store path goes out as a narinfo and a NAR, and comes back.
 */
TEST_F(GcsBinaryCacheStoreTest, roundTripStorePath)
{
    auto store = openStore();
    std::string data = "some file contents";

    StringSource dump{data};
    /* Via `Store` to get the default arguments. */
    auto path = static_cast<Store &>(*store).addToStoreFromDump(
        dump, "file", FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Flat);

    auto narInfo = std::dynamic_pointer_cast<const NarInfo>(store->queryPathInfo(path).get_ptr());
    ASSERT_TRUE(narInfo);
    EXPECT_TRUE(gcs->objects.contains(narInfo->url));

    StringSink nar;
    store->narFromPath(path, nar);
    StringSink expected;
    dumpString(data, expected);
    EXPECT_EQ(nar.s, expected.s);
}

} // namespace nix
