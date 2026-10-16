#include "nix/store/filetransfer.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/tests/gmock-matchers.hh"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace nix {

class FileTransferTest : public LibStoreTest
{};

/**
 * Setting up `s3://` and `gs://` requests (URL rewriting, credentials)
 * happens inside the `noexcept` `enqueueFileTransfer()`. Failures there
 * must be reported through the callback like any other transfer error,
 * not terminate the process.
 */
TEST_F(FileTransferTest, malformedGcsUrlIsAnError)
{
    auto fileTransfer = makeFileTransfer();
    FileTransferRequest request(VerbatimURL{std::string("gs://bucket")});
    EXPECT_THAT(
        [&] { fileTransfer->download(request); },
        ::testing::ThrowsMessage<BadURL>(testing::HasSubstrIgnoreANSIMatcher("URI has a missing or invalid key")));
}

TEST_F(FileTransferTest, malformedS3UrlIsAnError)
{
    auto fileTransfer = makeFileTransfer();
    FileTransferRequest request(VerbatimURL{std::string("s3://bucket")});
    EXPECT_THAT(
        [&] { fileTransfer->download(request); },
        ::testing::ThrowsMessage<BadURL>(testing::HasSubstrIgnoreANSIMatcher("URI has a missing or invalid key")));
}

} // namespace nix
