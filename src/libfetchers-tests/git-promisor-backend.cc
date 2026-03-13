#include "nix/fetchers/git-promisor-backend.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"

#include <gtest/gtest.h>
#include <git2/blob.h>
#include <git2/commit.h>
#include <git2/errors.h>
#include <git2/global.h>
#include <git2/odb.h>
#include <git2/odb_backend.h>
#include <git2/repository.h>
#include <git2/signature.h>
#include <git2/sys/odb_backend.h>
#include <git2/tree.h>
#include <git2/types.h>

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace nix {

// Convert git_oid to nix::Hash (mirrors the static toHash in git-utils.cc)
static Hash gitOidToHash(const git_oid & oid)
{
    Hash hash(HashAlgorithm::SHA1);
    memcpy(hash.hash, oid.id, hash.hashSize);
    return hash;
}

/**
 * Test fixture that creates a full git repo and a blobless partial clone.
 *
 * The full repo contains:
 *   hello.txt      -> "hello world\n"
 *   dir/nested.txt -> "nested content\n"
 *
 * The partial clone is created with --filter=blob:none, so tree and commit
 * objects are local but blob objects are missing.
 */
class PromisorBackendTest : public ::testing::Test
{
protected:
    std::unique_ptr<AutoDelete> delTmpDir;
    std::filesystem::path tmpDir;
    std::filesystem::path fullRepoPath;
    std::filesystem::path partialClonePath;

    // OIDs of objects in the full repo
    git_oid helloBlobOid{};
    git_oid nestedBlobOid{};
    git_oid rootTreeOid{};
    git_oid commitOid{};

    void SetUp() override
    {
        git_libgit2_init();

        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);

        fullRepoPath = tmpDir / "full-repo";
        partialClonePath = tmpDir / "partial-clone";

        createFullRepo();
        createPartialClone();
    }

    void TearDown() override
    {
        delTmpDir.reset();
    }

    /** Get the .git directory of the partial clone */
    std::filesystem::path partialGitDir()
    {
        return partialClonePath / ".git";
    }

private:
    /**
     * Create a bare repo with a commit containing hello.txt and dir/nested.txt.
     * Uses git CLI to ensure it's a proper repo that can serve as a promisor remote.
     */
    void createFullRepo()
    {
        auto workDir = tmpDir / "full-work";
        runGit(tmpDir, {"init", workDir.string()});
        runGit(workDir, {"config", "user.email", "test@test.com"});
        runGit(workDir, {"config", "user.name", "Test"});

        // Create files
        std::filesystem::create_directories(workDir / "dir");
        writeTestFile(workDir / "hello.txt", "hello world\n");
        writeTestFile(workDir / "dir" / "nested.txt", "nested content\n");

        // Commit
        runGit(workDir, {"add", "."});
        runGit(workDir, {"commit", "-m", "initial commit"});

        // Record OIDs
        helloBlobOid = parseOid(runGitOutput(workDir, {"rev-parse", "HEAD:hello.txt"}));
        nestedBlobOid = parseOid(runGitOutput(workDir, {"rev-parse", "HEAD:dir/nested.txt"}));
        rootTreeOid = parseOid(runGitOutput(workDir, {"rev-parse", "HEAD^{tree}"}));
        commitOid = parseOid(runGitOutput(workDir, {"rev-parse", "HEAD"}));

        // Clone to a bare repo that will serve as the "remote"
        runGit(tmpDir, {"clone", "--bare", workDir.string(), fullRepoPath.string()});
    }

    /**
     * Create a blobless partial clone from the full repo.
     */
    void createPartialClone()
    {
        // Use file:// protocol to enable partial clone from local repo
        runGit(
            tmpDir,
            {"clone", "--filter=blob:none", "--no-checkout",
             "file://" + fullRepoPath.string(), partialClonePath.string()});
    }

    static void writeTestFile(const std::filesystem::path & path, const std::string & content)
    {
        std::ofstream f(path);
        f << content;
    }

    void runGit(const std::filesystem::path & cwd, std::initializer_list<std::string> args)
    {
        Strings argVec(args);
        auto [status, output] = runProgram(RunOptions{
            .program = "git",
            .args = argVec,
            .chdir = cwd.string(),
            .mergeStderrToStdout = true,
        });
        if (status != 0)
            throw Error("git command failed in %s (status %d): %s", cwd.string(), status, output);
    }

    std::string runGitOutput(const std::filesystem::path & cwd, std::initializer_list<std::string> args)
    {
        Strings argVec(args);
        auto [status, output] = runProgram(RunOptions{
            .program = "git",
            .args = argVec,
            .chdir = cwd.string(),
            .mergeStderrToStdout = true,
        });
        if (status != 0)
            throw Error("git command failed in %s (status %d): %s", cwd.string(), status, output);
        // Trim trailing newline
        while (!output.empty() && output.back() == '\n')
            output.pop_back();
        return output;
    }

    static git_oid parseOid(const std::string & hex)
    {
        git_oid oid;
        if (git_oid_fromstr(&oid, hex.c_str()) != 0)
            throw Error("invalid oid: %s", hex);
        return oid;
    }
};

// ===========================================================================
// isPromisorRepo tests
// ===========================================================================

TEST_F(PromisorBackendTest, isPromisorRepo_true_for_partial_clone)
{
    EXPECT_TRUE(isPromisorRepo(partialGitDir()));
}

TEST_F(PromisorBackendTest, isPromisorRepo_false_for_full_clone)
{
    EXPECT_FALSE(isPromisorRepo(fullRepoPath));
}

TEST_F(PromisorBackendTest, isPromisorRepo_false_for_nonexistent_path)
{
    EXPECT_FALSE(isPromisorRepo(tmpDir / "nonexistent"));
}

// ===========================================================================
// Backend creation tests
// ===========================================================================

TEST_F(PromisorBackendTest, backend_created_for_partial_clone)
{
    git_odb_backend * backend = nullptr;
    ASSERT_EQ(git_odb_backend_promisor(&backend, partialGitDir()), GIT_OK);
    EXPECT_NE(backend, nullptr);

    // Clean up — normally libgit2 would call free when the ODB is destroyed
    if (backend && backend->free)
        backend->free(backend);
}

TEST_F(PromisorBackendTest, backend_not_created_for_full_clone)
{
    git_odb_backend * backend = nullptr;
    ASSERT_EQ(git_odb_backend_promisor(&backend, fullRepoPath), GIT_OK);
    EXPECT_EQ(backend, nullptr);
}

// ===========================================================================
// Object read tests via GitRepo (full integration with promisor backend)
// ===========================================================================

TEST_F(PromisorBackendTest, tree_objects_available_locally)
{
    // Tree objects should be present in a blobless clone (only blobs are filtered)
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);
    EXPECT_TRUE(repo->hasObject(gitOidToHash(rootTreeOid)));
}

TEST_F(PromisorBackendTest, read_missing_blob_triggers_fetch)
{
    // The promisor backend should transparently fetch the missing blob
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);
    EXPECT_TRUE(repo->hasObject(gitOidToHash(helloBlobOid)));
}

TEST_F(PromisorBackendTest, read_nonexistent_oid_fails_cleanly)
{
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);

    // Fabricated OID that doesn't exist anywhere — should fail without crashing
    git_oid fakeOid;
    git_oid_fromstr(&fakeOid, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    EXPECT_FALSE(repo->hasObject(gitOidToHash(fakeOid)));
}

TEST_F(PromisorBackendTest, accessor_reads_file_from_partial_clone)
{
    // Integration test: use GitSourceAccessor (same path as Tecnix zone eval)
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);
    auto accessor = repo->getAccessor(gitOidToHash(commitOid), {}, "test");

    auto content = accessor->readFile(CanonPath("hello.txt"));
    EXPECT_EQ(content, "hello world\n");

    auto nested = accessor->readFile(CanonPath("dir/nested.txt"));
    EXPECT_EQ(nested, "nested content\n");
}

TEST_F(PromisorBackendTest, double_read_uses_cached_blob)
{
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);
    auto accessor = repo->getAccessor(gitOidToHash(commitOid), {}, "test");

    // First read triggers fetch
    auto content1 = accessor->readFile(CanonPath("hello.txt"));
    EXPECT_EQ(content1, "hello world\n");

    // Second read should use the now-local packfile
    auto content2 = accessor->readFile(CanonPath("hello.txt"));
    EXPECT_EQ(content2, "hello world\n");
}

TEST_F(PromisorBackendTest, directory_listing_works_without_blob_fetch)
{
    // Directory listing only needs tree objects, which are local
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);
    auto accessor = repo->getAccessor(gitOidToHash(commitOid), {}, "test");

    auto entries = accessor->readDirectory(CanonPath::root);
    EXPECT_EQ(entries.size(), 2u); // hello.txt, dir/
}

TEST_F(PromisorBackendTest, concurrent_blob_reads)
{
    // Verify thread safety with concurrent fetches
    auto repo = GitRepo::openRepo(partialGitDir(), false, true);

    std::vector<std::thread> threads;
    std::string result0, result1;
    std::exception_ptr exc0, exc1;

    threads.emplace_back([&] {
        try {
            auto acc = repo->getAccessor(gitOidToHash(commitOid), {}, "t0");
            result0 = acc->readFile(CanonPath("hello.txt"));
        } catch (...) {
            exc0 = std::current_exception();
        }
    });

    threads.emplace_back([&] {
        try {
            auto acc = repo->getAccessor(gitOidToHash(commitOid), {}, "t1");
            result1 = acc->readFile(CanonPath("dir/nested.txt"));
        } catch (...) {
            exc1 = std::current_exception();
        }
    });

    for (auto & t : threads)
        t.join();

    if (exc0)
        std::rethrow_exception(exc0);
    if (exc1)
        std::rethrow_exception(exc1);

    EXPECT_EQ(result0, "hello world\n");
    EXPECT_EQ(result1, "nested content\n");
}

// ===========================================================================
// Full clone (non-partial) should work identically (no promisor backend needed)
// ===========================================================================

TEST_F(PromisorBackendTest, full_clone_works_without_promisor)
{
    // Opening a full clone should work normally — promisor backend is not installed
    auto repo = GitRepo::openRepo(fullRepoPath, false, true);
    EXPECT_TRUE(repo->hasObject(gitOidToHash(helloBlobOid)));
    EXPECT_TRUE(repo->hasObject(gitOidToHash(rootTreeOid)));
}

} // namespace nix
