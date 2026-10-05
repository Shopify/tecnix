#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "nix/expr/tecnix/source-deps.hh"

namespace nix {

namespace {

using namespace std::string_literals;

constexpr auto headOid = "d00491fd7e5bb6fa28c517a0bb32b8b506539d4d";
constexpr auto indexOid = "0cfbf08886fca9a91cb753ec8734c84fcbe52c9f";
constexpr auto zeroOid = "0000000000000000000000000000000000000000";

std::string
ordinary(std::string_view xy, std::string_view modes, std::string_view hH, std::string_view hI, std::string_view path)
{
    return "1 "s + std::string(xy) + " N... " + std::string(modes) + " " + std::string(hH) + " " + std::string(hI) + " "
           + std::string(path) + '\0';
}

GitStatusEntry staged(std::string path, uint32_t mode = 0100644)
{
    return {.path = std::move(path), .index = GitIndexBlob{.oid = indexOid, .mode = mode}};
}

GitStatusEntry onDisk(std::string path)
{
    return {.path = std::move(path)};
}

} // namespace

TEST(TecnixGitStatus, StagedOnlyChangeIsServedFromTheIndex)
{
    // What `git status` reports for a cleanly merged, skip-worktree file of an
    // uncommitted merge in a sparse checkout: staged, absent from disk.
    auto output = ordinary("M.", "100644 100644 100644", headOid, indexOid, "system/tectonix/default.nix");
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{staged("system/tectonix/default.nix")});
}

TEST(TecnixGitStatus, WorktreeChangesStayOnDisk)
{
    auto output = ordinary(".M", "100644 100644 100644", headOid, headOid, "a.nix")
                  + ordinary("MM", "100644 100644 100644", headOid, indexOid, "b.nix")
                  + ordinary("AD", "000000 100644 000000", zeroOid, indexOid, "c.nix");
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), (std::vector{onDisk("a.nix"), onDisk("b.nix"), onDisk("c.nix")}));
}

TEST(TecnixGitStatus, StagedAdditionIsServedFromTheIndex)
{
    auto output = ordinary("A.", "000000 100644 100644", zeroOid, indexOid, "new.nix");
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{staged("new.nix")});
}

TEST(TecnixGitStatus, StagedDeletionHasNoIndexBlob)
{
    auto output = ordinary("D.", "100644 000000 000000", headOid, zeroOid, "gone.nix");
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{onDisk("gone.nix")});
}

TEST(TecnixGitStatus, IndexModeIsKept)
{
    auto output = ordinary("M.", "100644 100755 100755", headOid, indexOid, "bin/run")
                  + ordinary("A.", "000000 120000 120000", zeroOid, indexOid, "link");
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), (std::vector{staged("bin/run", 0100755), staged("link", 0120000)}));
}

TEST(TecnixGitStatus, SubmoduleStaysOnDisk)
{
    auto output = "1 M. SC.. 160000 160000 160000 "s + headOid + " " + indexOid + " vendor/sub" + '\0';
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{onDisk("vendor/sub")});
}

TEST(TecnixGitStatus, RenameKeepsBothNames)
{
    auto output = "2 R. N... 100644 100644 100644 "s + headOid + " " + indexOid + " R100 new name.nix" + '\0'
                  + "old name.nix" + '\0';
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), (std::vector{staged("new name.nix"), onDisk("old name.nix")}));
}

TEST(TecnixGitStatus, CopySourceIsNotDirty)
{
    auto output =
        "2 C. N... 100644 100644 100644 "s + headOid + " " + indexOid + " C100 copy.nix" + '\0' + "source.nix" + '\0';
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{staged("copy.nix")});
}

TEST(TecnixGitStatus, WorktreeCopySourceIsNotDirty)
{
    auto output =
        "2 .C N... 100644 100644 100644 "s + headOid + " " + headOid + " C100 copy.nix" + '\0' + "source.nix" + '\0';
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{onDisk("copy.nix")});
}

TEST(TecnixGitStatus, UnmergedAndUntrackedStayOnDisk)
{
    auto output = "u UU N... 100644 100644 100644 100644 "s + headOid + " " + indexOid + " " + headOid
                  + " conflicted.nix" + '\0' + "? untracked dir/file.nix" + '\0';
    EXPECT_EQ(
        parseGitPorcelainV2ZStatus(output), (std::vector{onDisk("conflicted.nix"), onDisk("untracked dir/file.nix")}));
}

TEST(TecnixGitStatus, PathsWithSpacesAreKeptWhole)
{
    auto output = ordinary("M.", "100644 100644 100644", headOid, indexOid, "dir with spaces/a b.nix");
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{staged("dir with spaces/a b.nix")});
}

TEST(TecnixGitStatus, HeadersAndTruncatedRecordsAreIgnored)
{
    auto output = "# branch.oid "s + headOid + '\0' + ordinary("M.", "100644 100644 100644", headOid, indexOid, "a.nix")
                  + "1 M. N... 100644";
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{staged("a.nix")});
}

TEST(TecnixGitStatus, RenameMissingItsOriginalPathEndsTheParse)
{
    auto output = ordinary(".M", "100644 100644 100644", headOid, headOid, "a.nix") + "2 R. N... 100644 100644 100644 "
                  + headOid + " " + indexOid + " R100 new.nix" + '\0';
    EXPECT_EQ(parseGitPorcelainV2ZStatus(output), std::vector{onDisk("a.nix")});
}

} // namespace nix
