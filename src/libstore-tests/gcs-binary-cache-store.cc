#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/http-binary-cache-store.hh"

#include <gtest/gtest.h>

namespace nix {

TEST(GcsBinaryCacheStore, constructConfig)
{
    GcsBinaryCacheStoreConfig config{"my-bucket", {}};

    EXPECT_EQ(
        config.cacheUri,
        (ParsedURL{
            .scheme = "gs",
            .authority = ParsedURL::Authority{.host = "my-bucket"},
        }));
    EXPECT_EQ(config.endpoint.get(), "");
    EXPECT_EQ(config.storageClass.get(), std::nullopt);
}

/**
 * Requests are built from cacheUri and `gs://` requests read the endpoint from
 * its query: without it there, reads would go to Google instead.
 */
TEST(GcsBinaryCacheStore, endpointTravelsInCacheUri)
{
    StringMap params;
    params["endpoint"] = "http://localhost:4443";
    params["priority"] = "10";
    params["storage-class"] = "NEARLINE";

    GcsBinaryCacheStoreConfig config("my-bucket", params);

    EXPECT_EQ(
        config.cacheUri,
        (ParsedURL{
            .scheme = "gs",
            .authority = ParsedURL::Authority{.host = "my-bucket"},
            .query = (StringMap) {{"endpoint", "http://localhost:4443"}},
        }));
    EXPECT_EQ(config.priority.get(), 10);
    EXPECT_EQ(config.storageClass.get(), std::optional<std::string>("NEARLINE"));

    auto ref = config.getReference();
    EXPECT_EQ(ref.params["endpoint"], "http://localhost:4443");
    EXPECT_EQ(ref.params["priority"], "10");
}

TEST(GcsBinaryCacheStore, gsSchemeRegistration)
{
    EXPECT_TRUE(GcsBinaryCacheStoreConfig::uriSchemes().contains("gs"));
    EXPECT_FALSE(HttpBinaryCacheStoreConfig::uriSchemes().contains("gs"));
}

} // namespace nix
