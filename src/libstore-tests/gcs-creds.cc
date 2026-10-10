#include "nix/store/gcs-creds.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
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

class GcsCredentialHelperTest : public LibStoreTest
{
protected:
    std::filesystem::path tmpDir = createTempDir();
    AutoDelete delTmpDir{tmpDir};

    /**
     * A helper that runs `script` with `sh` after reading Git's request
     * into `$request`, with `$COUNTER` naming a file it can append to.
     */
    ref<GcsCredentialProvider>
    helper(std::string script, std::string_view uri = "https://storage.googleapis.com/bucket/some/prefix/")
    {
        return makeGcsCredentialHelperProvider(
            {"/bin/sh",
             "-c",
             "[ \"$1\" = get ] || exit 7; request=$(cat); COUNTER=" + (tmpDir / "counter").string() + "; " + script,
             "sh"},
            parseURL(uri));
    }

    size_t invocations()
    {
        auto counter = tmpDir / "counter";
        return pathExists(counter) ? readFile(counter).size() : 0;
    }

    /** A bearer token answer, expiring `seconds` from now if given. */
    static std::string answer(std::optional<int> seconds = std::nullopt)
    {
        std::string s = "printf 'capability[]=authtype\\nauthtype=Bearer\\ncredential=tok\\n";
        if (seconds) {
            auto now =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            s += "password_expiry_utc=" + std::to_string(now + *seconds) + "\\n";
        }
        return s + "\\n'";
    }
};

/** The request is what Git would send with `credential.useHttpPath`. */
TEST_F(GcsCredentialHelperTest, speaksTheGitCredentialProtocol)
{
    auto provider = helper(R"(
        for line in protocol=https host=storage.googleapis.com path=bucket/some/prefix/ 'capability[]=authtype'; do
            printf '%s\n' "$request" | grep -qxF "$line" || exit 8
        done; )" + answer(3600));
    EXPECT_EQ(provider->getAccessToken(false), "tok");
}

TEST_F(GcsCredentialHelperTest, includesThePortInTheHost)
{
    auto provider = helper(
        R"(printf '%s\n' "$request" | grep -qxF host=gcs.local:4443 || exit 8; )" + answer(3600),
        "http://gcs.local:4443/bucket/");
    EXPECT_EQ(provider->getAccessToken(false), "tok");
}

/** A helper without the authtype capability answers with username/password. */
TEST_F(GcsCredentialHelperTest, acceptsAPasswordAsTheToken)
{
    auto provider = helper("printf 'username=oauth2accesstoken\\npassword=tok2\\n\\n'");
    EXPECT_EQ(provider->getAccessToken(false), "tok2");
}

/** With an expiry, one answer serves until shortly before it. */
TEST_F(GcsCredentialHelperTest, reusesTheTokenUntilItExpires)
{
    auto provider = helper("printf x >> $COUNTER; " + answer(3600));
    EXPECT_EQ(provider->getAccessToken(false), "tok");
    EXPECT_EQ(provider->getAccessToken(true), "tok");
    EXPECT_EQ(provider->maybeGetAccessToken(false), "tok");
    EXPECT_EQ(invocations(), 1u);
}

/** Without an expiry there is nothing to base reuse on, so there is none. */
TEST_F(GcsCredentialHelperTest, doesNotReuseATokenWithoutExpiry)
{
    auto provider = helper("printf x >> $COUNTER; " + answer());
    EXPECT_EQ(provider->getAccessToken(false), "tok");
    EXPECT_EQ(provider->getAccessToken(false), "tok");
    EXPECT_EQ(invocations(), 2u);
}

TEST_F(GcsCredentialHelperTest, doesNotReuseATokenAboutToExpire)
{
    auto provider = helper("printf x >> $COUNTER; " + answer(10));
    EXPECT_EQ(provider->getAccessToken(false), "tok");
    EXPECT_EQ(provider->getAccessToken(false), "tok");
    EXPECT_EQ(invocations(), 2u);
}

/** What the helper said on stderr is the actionable part, so it is in the error. */
TEST_F(GcsCredentialHelperTest, failureIsAnError)
{
    auto provider = helper("echo 'not enrolled; run the enrolment command' >&2; exit 3");
    EXPECT_THAT(
        [&] { provider->getAccessToken(false); },
        ::testing::ThrowsMessage<GcsAuthError>(::testing::AllOf(
            testing::HasSubstrIgnoreANSIMatcher("exit code 3"),
            testing::HasSubstrIgnoreANSIMatcher("not enrolled; run the enrolment command"))));
    /* Configured but broken credentials never degrade to anonymous access. */
    EXPECT_THROW(provider->maybeGetAccessToken(false), GcsAuthError);
}

TEST_F(GcsCredentialHelperTest, aTransientFailureIsRetriedOnce)
{
    auto provider = helper("printf x >> $COUNTER; [ $(wc -c < $COUNTER) -ge 2 ] || exit 1; " + answer(3600));
    EXPECT_EQ(provider->getAccessToken(false), "tok");
    EXPECT_EQ(invocations(), 2u);
}

TEST_F(GcsCredentialHelperTest, anExpiredCredentialIsAnError)
{
    auto provider = helper(answer(-300));
    EXPECT_THAT(
        [&] { provider->getAccessToken(false); },
        ::testing::ThrowsMessage<GcsAuthError>(::testing::AnyOf(
            testing::HasSubstrIgnoreANSIMatcher("expired 300 s ago"),
            testing::HasSubstrIgnoreANSIMatcher("expired 301 s ago"))));
}

TEST_F(GcsCredentialHelperTest, aMalformedExpiryIsAnError)
{
    auto provider = helper("printf 'credential=tok\\npassword_expiry_utc=soon\\n\\n'");
    EXPECT_THAT(
        [&] { provider->getAccessToken(false); },
        ::testing::ThrowsMessage<GcsAuthError>(testing::HasSubstrIgnoreANSIMatcher("not a Unix time: 'soon'")));
}

TEST_F(GcsCredentialHelperTest, aNonBearerCredentialIsAnError)
{
    auto provider = helper("printf 'authtype=Basic\\ncredential=dXNlcjpwYXNz\\n\\n'");
    EXPECT_THAT(
        [&] { provider->getAccessToken(false); },
        ::testing::ThrowsMessage<GcsAuthError>(testing::HasSubstrIgnoreANSIMatcher("bearer token")));
}

TEST_F(GcsCredentialHelperTest, noCredentialIsAnError)
{
    auto provider = helper("printf 'username=nobody\\n\\n'");
    EXPECT_THAT(
        [&] { provider->getAccessToken(false); },
        ::testing::ThrowsMessage<GcsAuthError>(testing::HasSubstrIgnoreANSIMatcher("returned no credential")));
}

TEST(GcsCredentialHelper, rejectsAnEmptyCommand)
{
    EXPECT_THROW(makeGcsCredentialHelperProvider({}, parseURL("https://storage.googleapis.com/bucket/")), UsageError);
}

} // namespace nix
