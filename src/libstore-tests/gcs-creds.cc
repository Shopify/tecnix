#include "nix/store/gcs-creds.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/tests/gmock-matchers.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nix {

/** Credentials that are configured but unusable are an error, not anonymous access. */
TEST_F(LibStoreTest, aMissingGoogleApplicationCredentialsFileIsAnError)
{
    setEnv("GOOGLE_APPLICATION_CREDENTIALS", "/nonexistent/credentials.json");
    auto provider = makeGcsCredentialsProvider();
    EXPECT_THAT(
        [&] { provider->getAccessToken(false); },
        ::testing::ThrowsMessage<GcsAuthError>(testing::HasSubstrIgnoreANSIMatcher("/nonexistent/credentials.json")));
    EXPECT_THROW(provider->maybeGetAccessToken(false), GcsAuthError);
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
}

} // namespace nix
