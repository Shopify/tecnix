#include "nix/fetchers/git-lfs-fetch.hh"
#include "nix/util/url.hh"

#include <gtest/gtest.h>

namespace nix::lfs {

struct GitLFSParameterizedTestFixture : public ::testing::TestWithParam<std::pair<std::string, std::string>>
{};

TEST_P(GitLFSParameterizedTestFixture, lfs_endpoint_for_remote)
{
    auto & [input, expected] = GetParam();
    ASSERT_EQ(lfsEndpointForRemote(parseURL(input)).to_string(), expected);
};

INSTANTIATE_TEST_SUITE_P(
    GitLFSTests,
    GitLFSParameterizedTestFixture,
    ::testing::Values(
        std::pair{"https://git-server.com/foo/bar", "https://git-server.com/foo/bar.git/info/lfs"},
        std::pair{"https://git-server.com/foo/bar.git", "https://git-server.com/foo/bar.git/info/lfs"},
        std::pair{"https://git-server.com", "https://git-server.com/.git/info/lfs"},
        std::pair{"https://git-server.com/", "https://git-server.com/.git/info/lfs"},
        std::pair{"https://git-server.com//", "https://git-server.com//.git/info/lfs"},
        std::pair{"https://git-server.com/foo/bar/", "https://git-server.com/foo/bar.git/info/lfs"}));

} // namespace nix::lfs
