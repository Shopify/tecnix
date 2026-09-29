#include <gtest/gtest.h>

#include "nix/expr/tecnix/source-deps.hh"

#include <string_view>

namespace nix {

using namespace std::string_view_literals;

TEST(ParseGitPorcelainZDirtyPaths, emptyOutput)
{
    auto result = parseGitPorcelainZDirtyPaths(""sv);
    EXPECT_TRUE(result.paths.empty());
    EXPECT_TRUE(result.collapsedUntrackedDirs.empty());
}

TEST(ParseGitPorcelainZDirtyPaths, trackedAndUntrackedFiles)
{
    auto result = parseGitPorcelainZDirtyPaths(" M areas/a.txt\0?? areas/b.txt\0D  areas/c.txt\0"sv);
    EXPECT_EQ(result.paths, (std::vector<std::string>{"areas/a.txt", "areas/b.txt", "areas/c.txt"}));
    EXPECT_TRUE(result.collapsedUntrackedDirs.empty());
}

TEST(ParseGitPorcelainZDirtyPaths, renameRecordsBothPaths)
{
    auto result = parseGitPorcelainZDirtyPaths("R  new.txt\0old.txt\0M  other.txt\0"sv);
    EXPECT_EQ(result.paths, (std::vector<std::string>{"new.txt", "old.txt", "other.txt"}));
    EXPECT_TRUE(result.collapsedUntrackedDirs.empty());
}

TEST(ParseGitPorcelainZDirtyPaths, collapsedUntrackedDirsAreSplitOut)
{
    auto result = parseGitPorcelainZDirtyPaths("?? newdir/\0?? a/deep/dir/\0?? file.txt\0"sv);
    EXPECT_EQ(result.paths, (std::vector<std::string>{"file.txt"}));
    EXPECT_EQ(result.collapsedUntrackedDirs, (std::vector<std::string>{"newdir", "a/deep/dir"}));
}

TEST(ParseGitPorcelainZDirtyPaths, onlyUntrackedEntriesCollapse)
{
    // A trailing slash on a non-`??` entry does not occur in porcelain v1
    // output; if it ever did, treat it as a plain dirty path.
    auto result = parseGitPorcelainZDirtyPaths(" M odd/\0"sv);
    EXPECT_EQ(result.paths, (std::vector<std::string>{"odd/"}));
    EXPECT_TRUE(result.collapsedUntrackedDirs.empty());
}

TEST(ParseGitPorcelainZDirtyPaths, malformedEntriesAreSkipped)
{
    auto result = parseGitPorcelainZDirtyPaths("bogus\0?? ok.txt\0X\0no-nul-terminator"sv);
    EXPECT_EQ(result.paths, (std::vector<std::string>{"ok.txt"}));
    EXPECT_TRUE(result.collapsedUntrackedDirs.empty());
}

} // namespace nix
