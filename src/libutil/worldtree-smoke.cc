///@file
/// A standalone exerciser for the worldtree C++ client (`worldtree-client.hh`), used by
/// the cross-language wire-compatibility smoke. It connects to a running worldtreed,
/// drives all four Tecnix read verbs against a workspace the harness has already set up,
/// and prints each result as a parseable line. The harness (a worldtreed Rust example)
/// owns the fixture and asserts on this output, so this program stays a dumb, faithful
/// exerciser — its only job is to prove the C++ codec and the daemon's prost encoder
/// agree on the wire in *both* directions, against real prost (not a golden fixture).
///
/// It is compiled directly with `clang++` against `worldtree-client.cc` (no Nix build,
/// no protobuf-c) — the Tecnix Nix build is, at the time of writing, blocked on an
/// unrelated missing-libgit2 dependency, and this seam must be testable regardless.
///
/// Usage: worldtree-smoke --socket PATH --ws N

#include "nix/util/worldtree-client.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace nix::worldtree;

namespace {

std::string toHex(const Oid & oid)
{
    static const char * digits = "0123456789abcdef";
    std::string s;
    s.reserve(40);
    for (uint8_t b : oid) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 0xf]);
    }
    return s;
}

/// Render bytes printably: as text if all bytes are printable ASCII/whitespace, else as
/// hex. The harness sets blob content to plain text, so the text form is what it greps.
std::string render(const std::string & bytes)
{
    bool printable = true;
    for (unsigned char c : bytes)
        if (!(c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f))) {
            printable = false;
            break;
        }
    if (printable) {
        // One-line: escape newlines so a multi-line blob stays a single output line.
        std::string out;
        for (char c : bytes)
            out += (c == '\n') ? std::string("\\n") : std::string(1, c);
        return "text:" + out;
    }
    std::string out = "hex:";
    static const char * digits = "0123456789abcdef";
    for (unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xf]);
    }
    return out;
}

[[noreturn]] void usage()
{
    std::fprintf(stderr, "usage: worldtree-smoke --socket PATH --ws N\n");
    std::exit(2);
}

} // namespace

int main(int argc, char ** argv)
{
    std::string socketPath;
    uint64_t ws = 0;
    bool haveWs = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                usage();
            return argv[++i];
        };
        if (arg == "--socket")
            socketPath = next();
        else if (arg == "--ws") {
            ws = std::strtoull(next().c_str(), nullptr, 10);
            haveWs = true;
        } else
            usage();
    }
    if (socketPath.empty() || !haveWs)
        usage();

    try {
        Client client = Client::connect(socketPath);

        // 1. dirty_zones — which zones have tracked working-copy changes.
        auto dirty = client.dirtyZones(ws);
        for (const auto & z : dirty)
            std::printf("DIRTY %s\n", z.c_str());

        // 2a. zone_tree_shas with an empty request — expands to the dirty set, the common
        //     Tecnix call. Each dirty zone yields its synthesized working-tree oid.
        auto zts = client.zoneTreeShas(ws, {});
        for (const auto & e : zts)
            std::printf("ZTS %s %s\n", e.zone.c_str(),
                e.treeSha ? toHex(*e.treeSha).c_str() : "absent");

        // 2b. zone_tree_shas with an explicit list mixing a real (dirty) zone and one
        //     that does not exist. This proves the response stays one-to-one with the
        //     request and that the daemon's empty-bytes "absent" marker decodes to a
        //     `nullopt` treeSha on the C++ side (not an empty-but-present oid).
        if (!dirty.empty()) {
            std::vector<std::string> probe = {dirty.front(), "//does/not/exist"};
            auto explicitZts = client.zoneTreeShas(ws, probe);
            for (const auto & e : explicitZts)
                std::printf("ZTS2 %s %s\n", e.zone.c_str(),
                    e.treeSha ? toHex(*e.treeSha).c_str() : "absent");
        }

        // 3. read_tree at the root, deep — the whole committed skeleton (overlay-free).
        //    Collect every blob oid we see for the read_blobs round-trip below.
        auto tree = client.readTree(ws, "", 64);
        std::vector<Oid> blobOids;
        for (const auto & e : tree) {
            std::printf("TREE %s mode=%o oid=%s size=%llu\n", e.path.c_str(), e.mode,
                toHex(e.oid).c_str(), static_cast<unsigned long long>(e.size));
            // A regular/executable file (not a tree, not a symlink): mode 0o100xxx.
            if ((e.mode & 0170000) == 0100000)
                blobOids.push_back(e.oid);
        }

        // 4. read_blobs — fetch each committed blob by oid and render its bytes. This
        //    closes the loop: the harness staged known content, the daemon hashed and
        //    stored it, and we read it back by the oid read_tree reported.
        if (!blobOids.empty()) {
            auto blobs = client.readBlobs(ws, blobOids);
            for (const auto & b : blobs)
                std::printf("BLOB %s %s\n", toHex(b.oid).c_str(),
                    b.content ? render(*b.content).c_str() : "absent");
        }

        std::printf("SMOKE-DONE\n");
        return 0;
    } catch (const RpcError & e) {
        std::fprintf(stderr, "worldtree-smoke: daemon error (code %d): %s\n",
            static_cast<int>(e.code), e.what());
        return 1;
    } catch (const std::exception & e) {
        std::fprintf(stderr, "worldtree-smoke: %s\n", e.what());
        return 1;
    }
}
