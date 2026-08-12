#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/tecnix/source-accessors.hh"
#include "nix/store/store-open.hh"
#include "nix/store/globals.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/util.hh"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <sys/xattr.h>

namespace nix {

// ============================================================================
// The worldtree FUSE projection as the tracked Tecnix clean backend.
//
// These tests fake the immutable projection with a plain directory tree plus
// real `user.worldtree.tree-oid` xattrs, exactly the surface the daemon
// exposes. No socket, no daemon, no git repository.
// ============================================================================

static bool setWorldtreeTestTreeOidXattr(const std::filesystem::path & path, std::string_view value)
{
#ifdef __APPLE__
    return ::setxattr(path.c_str(), "user.worldtree.tree-oid", value.data(), value.size(), 0, 0) == 0;
#else
    return ::setxattr(path.c_str(), "user.worldtree.tree-oid", value.data(), value.size(), 0) == 0;
#endif
}

static bool setWorldtreeTestBlobOidXattr(const std::filesystem::path & path, std::string_view value)
{
#ifdef __APPLE__
    return ::setxattr(path.c_str(), "user.worldtree.blob-oid", value.data(), value.size(), 0, 0) == 0;
#else
    return ::setxattr(path.c_str(), "user.worldtree.blob-oid", value.data(), value.size(), 0) == 0;
#endif
}

/**
 * Independent fingerprint oracle: the git blob oid of `content`, computed from
 * the git object spec framing ("blob <size>\0<bytes>") rather than through the
 * accessor's own hashing helpers.
 */
static std::string gitBlobOidOf(std::string_view content)
{
    std::string object = "blob " + std::to_string(content.size());
    object.push_back('\0');
    object.append(content);
    return hashString(HashAlgorithm::SHA1, object).gitRev();
}

class TecnixWorldtreeTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initLibStore(false);
        initGC();
    }

    std::unique_ptr<AutoDelete> delTmpDir;
    std::filesystem::path tmpDir;
    // The rev only names the projection directory; no repository exists.
    std::string commitSha = std::string(40, 'f');

    void SetUp() override
    {
        auto tmp = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmp, true);
        tmpDir = tmp;
    }

    void TearDown() override
    {
        delTmpDir.reset();
    }

    std::filesystem::path mountDir() const
    {
        return tmpDir / "worldtree";
    }

    std::filesystem::path revisionRoot() const
    {
        return mountDir() / "tecnix" / commitSha;
    }

    void writeManifest(std::string_view manifest)
    {
        auto dir = revisionRoot() / "W-000000";
        std::filesystem::create_directories(dir);
        writeFile((dir / "manifest.json").string(), std::string(manifest));
    }

    struct WorldtreeEvalContext
    {
        bool readOnlyMode = true;
        fetchers::Settings fetchSettings{};
        EvalSettings evalSettings{readOnlyMode};
        ref<Store> store;
        std::unique_ptr<EvalState> state;

        WorldtreeEvalContext(const std::filesystem::path & mount, const std::string & rev)
            : store(openStore("dummy://"))
        {
            evalSettings.nixPath = {};
            evalSettings.tectonixGitSha = rev;
            // Deliberately nonexistent: the FUSE-backed accessor never connects.
            evalSettings.tectonixWorldtreeSocket = (mount / "unused-worldtree.sock").string();
            evalSettings.tectonixWorldtreeMount = mount.string();
            state = std::make_unique<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
        }
    };

    std::unique_ptr<WorldtreeEvalContext> createContext()
    {
        return std::make_unique<WorldtreeEvalContext>(mountDir(), commitSha);
    }
};

TEST_F(TecnixWorldtreeTest, repo_accessor_serves_zone_content_with_git_fingerprints)
{
    writeManifest(R"({
        "//app/zone": { "id": "W-aaaa01" }
    })");
    auto zoneDir = revisionRoot() / "W-aaaa01";
    std::filesystem::create_directories(zoneDir / "sub");
    writeFile((zoneDir / "hello.txt").string(), "hello\n");
    writeFile((zoneDir / "sub" / "nested.txt").string(), "nested\n");
    writeFile((zoneDir / "tool.sh").string(), "hi\n");
    std::filesystem::permissions(
        zoneDir / "tool.sh", std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    std::filesystem::create_symlink("hello.txt", zoneDir / "link");

    const std::string zoneTreeOid(40, 'a');
    const std::string subTreeOid(40, 'b');
    if (!setWorldtreeTestTreeOidXattr(zoneDir, zoneTreeOid)
        || !setWorldtreeTestTreeOidXattr(zoneDir / "sub", subTreeOid))
        GTEST_SKIP() << "filesystem does not support user xattrs";

    auto ctx = createContext();
    auto accessor = getTecnixRepoAccessor(*ctx->state);

    // Repo-relative paths are served through the manifest's zone mapping.
    ASSERT_EQ(accessor->readFile(CanonPath("/app/zone/hello.txt")), "hello\n");
    ASSERT_EQ(accessor->readFile(CanonPath("/app/zone/sub/nested.txt")), "nested\n");
    ASSERT_EQ(accessor->readLink(CanonPath("/app/zone/link")), "hello.txt");
    auto zoneEntries = accessor->readDirectory(CanonPath("/app/zone"));
    ASSERT_TRUE(zoneEntries.count("hello.txt"));
    ASSERT_TRUE(zoneEntries.count("sub"));

    // Zone-ancestor directories are synthesized so traversal can reach zones.
    auto ancestorStat = accessor->maybeLstat(CanonPath("/app"));
    ASSERT_TRUE(ancestorStat && ancestorStat->type == SourceAccessor::Type::tDirectory);
    auto ancestorEntries = accessor->readDirectory(CanonPath("/app"));
    ASSERT_TRUE(ancestorEntries.count("zone"));

    // Directory fingerprints echo the projection's tree-oid xattr byte for byte.
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/app/zone")).second, "git:" + zoneTreeOid + ";mode=040000");
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/app/zone/sub")).second, "git:" + subTreeOid + ";mode=040000");

    // File and symlink fingerprints are exact git blob oids, so TXDC closures
    // cross-validate with the libgit2 backend. Pin one absolute value to anchor
    // the oracle itself ("hello\n" is the classic known git blob).
    auto helloOid = gitBlobOidOf("hello\n");
    ASSERT_EQ(helloOid, "ce013625030ba8dba906f756967f9e9ca394464a");
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/app/zone/hello.txt")).second, "git:" + helloOid + ";mode=100644");
    ASSERT_EQ(
        *accessor->getFingerprint(CanonPath("/app/zone/tool.sh")).second,
        "git:" + gitBlobOidOf("hi\n") + ";mode=100755");
    ASSERT_EQ(
        *accessor->getFingerprint(CanonPath("/app/zone/link")).second,
        "git:" + gitBlobOidOf("hello.txt") + ";mode=120000");

    // Negative lookups, and paths outside every visible zone, observe absence.
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/app/zone/missing.nix")).second, "absent");
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/README.md")).second, "absent");

    // Synthesized directories (the root and zone ancestors) have no single git
    // object; their composite fingerprint stays outside the git vocabulary.
    auto ancestorFp = accessor->getFingerprint(CanonPath("/app")).second;
    ASSERT_TRUE(ancestorFp && hasPrefix(*ancestorFp, "worldtree-union:"));
    auto rootFp = accessor->getFingerprint(CanonPath::root).second;
    ASSERT_TRUE(rootFp && hasPrefix(*rootFp, "worldtree-union:"));
}

TEST_F(TecnixWorldtreeTest, blob_fingerprints_memoize_in_memory_only)
{
    writeManifest(R"({
        "//app/zone": { "id": "W-aaaa02" }
    })");
    auto zoneDir = revisionRoot() / "W-aaaa02";
    std::filesystem::create_directories(zoneDir);
    writeFile((zoneDir / "marker.txt").string(), "one\n");

    auto fingerprintOf = [&](WorldtreeEvalContext & ctx) {
        return *getTecnixRepoAccessor(*ctx.state)->getFingerprint(CanonPath("/app/zone/marker.txt")).second;
    };

    auto ctx1 = createContext();
    ASSERT_EQ(fingerprintOf(*ctx1), "git:" + gitBlobOidOf("one\n") + ";mode=100644");

    // Rewrite the bytes. A real projection cannot do this — it is immutable —
    // so the stale answer through the same accessor proves fingerprints are
    // memoized rather than re-hashed per query.
    writeFile((zoneDir / "marker.txt").string(), "two\n");
    ASSERT_EQ(fingerprintOf(*ctx1), "git:" + gitBlobOidOf("one\n") + ";mode=100644");

    // A fresh evaluator re-hashes: the memo lives in memory, never on disk.
    auto ctx2 = createContext();
    ASSERT_EQ(fingerprintOf(*ctx2), "git:" + gitBlobOidOf("two\n") + ";mode=100644");
}

TEST_F(TecnixWorldtreeTest, blob_fingerprints_prefer_blob_oid_xattr_and_fall_back_to_hashing)
{
    writeManifest(R"({
        "//app/zone": { "id": "W-aaaa03" }
    })");
    auto zoneDir = revisionRoot() / "W-aaaa03";
    std::filesystem::create_directories(zoneDir);
    writeFile((zoneDir / "served.txt").string(), "one\n");
    writeFile((zoneDir / "tool.sh").string(), "hi\n");
    std::filesystem::permissions(
        zoneDir / "tool.sh", std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
    writeFile((zoneDir / "unserved.txt").string(), "two\n");
    std::filesystem::create_symlink("served.txt", zoneDir / "link");

    const std::string servedOid(40, 'e');
    if (!setWorldtreeTestBlobOidXattr(zoneDir / "served.txt", servedOid)
        || !setWorldtreeTestBlobOidXattr(zoneDir / "tool.sh", servedOid))
        GTEST_SKIP() << "filesystem does not support user xattrs";

    auto ctx = createContext();
    auto accessor = getTecnixRepoAccessor(*ctx->state);

    // A served blob oid wins over content hashing: the sentinel oid is not the
    // content's hash, so any hashing would produce a different answer. The
    // mode still comes from the filesystem stat.
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/app/zone/served.txt")).second, "git:" + servedOid + ";mode=100644");
    ASSERT_EQ(*accessor->getFingerprint(CanonPath("/app/zone/tool.sh")).second, "git:" + servedOid + ";mode=100755");

    // No xattr: fall back to hashing the bytes.
    ASSERT_EQ(
        *accessor->getFingerprint(CanonPath("/app/zone/unserved.txt")).second,
        "git:" + gitBlobOidOf("two\n") + ";mode=100644");

    // A symlink must never attempt the xattr: getxattr() follows links, and
    // its target carries the sentinel blob oid — answering with it would
    // fingerprint the wrong git object. The link hashes its target string.
    ASSERT_EQ(
        *accessor->getFingerprint(CanonPath("/app/zone/link")).second,
        "git:" + gitBlobOidOf("served.txt") + ";mode=120000");
}

} // namespace nix
