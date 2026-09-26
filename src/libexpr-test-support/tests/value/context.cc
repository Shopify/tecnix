#include <exception> // Needed by rapidcheck on Darwin
#include <rapidcheck.h>

#include "nix/store/tests/path.hh"
#include "nix/expr/tests/value/context.hh"

namespace rc {
using namespace nix;

Gen<NixStringContextElem::DrvDeep> Arbitrary<NixStringContextElem::DrvDeep>::arbitrary()
{
    return gen::map(gen::arbitrary<StorePath>(), [](StorePath drvPath) {
        return NixStringContextElem::DrvDeep{
            .drvPath = drvPath,
        };
    });
}

Gen<NixStringContextElem::Path> Arbitrary<NixStringContextElem::Path>::arbitrary()
{
    return gen::map(gen::arbitrary<StorePath>(), [](StorePath storePath) {
        return NixStringContextElem::Path{
            .storePath = storePath,
        };
    });
}
Gen<NixStringContextElem::World> Arbitrary<NixStringContextElem::World>::arbitrary()
{
    // Generate a random 20-byte SHA1 hash as hex, plus a fixed path.
    return gen::map(
        gen::container<std::string>(40, gen::element<char>('a', 'b', 'c', 'd', 'e', 'f', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9')),
        [](std::string oidHex) {
            return NixStringContextElem::World{
                .oid = Hash::parseNonSRIUnprefixed(oidHex, HashAlgorithm::SHA1),
                .path = "//areas/test/zone",
            };
        });
}

Gen<NixStringContextElem> Arbitrary<NixStringContextElem>::arbitrary()
{
    return gen::mapcat(
        gen::inRange<uint8_t>(0, std::variant_size_v<NixStringContextElem::Raw>),
        [](uint8_t n) -> Gen<NixStringContextElem> {
            switch (n) {
            case 0:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::Opaque>(), [](NixStringContextElem a) { return a; });
            case 1:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::DrvDeep>(), [](NixStringContextElem a) { return a; });
            case 2:
                return gen::map(
                    gen::arbitrary<NixStringContextElem::Built>(), [](NixStringContextElem a) { return a; });
            case 3:
                return gen::map(gen::arbitrary<NixStringContextElem::Path>(), [](NixStringContextElem a) { return a; });
            case 4:
                return gen::map(gen::arbitrary<NixStringContextElem::World>(), [](NixStringContextElem a) { return a; });
            default:
                assert(false);
            }
        });
}

} // namespace rc
