#include <gtest/gtest.h>

#include "nix/expr/tecnix/memo-value.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value/context.hh"

namespace nix {

class TecnixMemoValueTest : public LibExprTest
{
protected:
    Value deep(std::string expr)
    {
        auto v = eval(expr);
        state.forceValueDeep(v);
        return v;
    }

    DecodedTecnixMemoValue roundTrip(Value & v)
    {
        auto decoded = deserializeTecnixMemoValue(state, serializeTecnixMemoValue(state, v));
        EXPECT_TRUE(decoded);
        return *decoded;
    }

    StorePath path(std::string_view name)
    {
        return StorePath(std::string("g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-") + std::string(name));
    }
};

TEST_F(TecnixMemoValueTest, dataRoundTrips)
{
    auto v = deep(R"({
        a = null; b = true; c = false; d = -5; e = 9223372036854775807; f = -9223372036854775807 - 1;
        g = 1.5; h = ""; i = "s\u0000t"; j = [ 1 "x" { y = 2; } [ ] ]; k = { };
    })");
    auto decoded = roundTrip(v);
    EXPECT_TRUE(state.eqValues(v, *decoded.value, noPos, "while comparing"));
    EXPECT_TRUE(decoded.storePaths.empty());
}

TEST_F(TecnixMemoValueTest, stringContextsRoundTripAndNameTheirStorePaths)
{
    auto drv = path("foo.drv");
    auto opaque = path("source");
    NixStringContext context{
        NixStringContextElem::Opaque{opaque},
        NixStringContextElem::DrvDeep{drv},
        NixStringContextElem::Built{
            .drvPath = makeConstantStorePathRef(drv),
            .output = "out",
        },
    };
    Value v;
    v.mkString("some string", context, state.mem);

    auto decoded = roundTrip(v);
    NixStringContext got;
    copyContext(*decoded.value, got);
    EXPECT_EQ(got, context);
    EXPECT_EQ(decoded.value->string_view(), "some string");
    EXPECT_EQ(decoded.storePaths, (StorePathSet{drv, opaque}));
}

TEST_F(TecnixMemoValueTest, sharedRecordsAreWrittenOnceAndSharedAgain)
{
    // Records that reference a common dependency, as crate records do.
    auto v = deep(R"(let
        dep = { name = "dep"; deps = [ ]; };
        mid = { name = "mid"; deps = [ dep dep ]; };
      in [ mid mid { name = "top"; deps = [ mid dep ]; } ])");
    auto shared = serializeTecnixMemoValue(state, v);
    auto unshared = deep(R"([
        { name = "mid"; deps = [ { name = "dep"; deps = [ ]; } { name = "dep"; deps = [ ]; } ]; }
        { name = "mid"; deps = [ { name = "dep"; deps = [ ]; } { name = "dep"; deps = [ ]; } ]; }
        { name = "top"; deps = [
          { name = "mid"; deps = [ { name = "dep"; deps = [ ]; } { name = "dep"; deps = [ ]; } ]; }
          { name = "dep"; deps = [ ]; } ]; }
      ])");
    EXPECT_LT(shared.size(), serializeTecnixMemoValue(state, unshared).size());

    auto decoded = deserializeTecnixMemoValue(state, shared);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(state.eqValues(v, *decoded->value, noPos, "while comparing"));
    auto list = decoded->value->listView();
    EXPECT_EQ(list[0]->attrs(), list[1]->attrs());
}

TEST_F(TecnixMemoValueTest, nonDataFailsClosed)
{
    for (auto expr : {"{ f = x: x; }", "[ ./. ]", "builtins.add"}) {
        auto v = deep(expr);
        EXPECT_THROW(serializeTecnixMemoValue(state, v), TecnixMemoUnserializable) << expr;
    }
}

TEST_F(TecnixMemoValueTest, malformedPayloadsAreMisses)
{
    auto v = deep(R"({ a = [ 1 "x" ]; b = { c = null; }; })");
    auto good = serializeTecnixMemoValue(state, v);
    ASSERT_TRUE(deserializeTecnixMemoValue(state, good));

    EXPECT_FALSE(deserializeTecnixMemoValue(state, ""));
    EXPECT_FALSE(deserializeTecnixMemoValue(state, "TXTV1"));
    EXPECT_FALSE(deserializeTecnixMemoValue(state, std::string(tecnixMemoPayloadMagic)));
    // Every truncation, and trailing garbage.
    for (size_t n = tecnixMemoPayloadMagic.size(); n < good.size(); n++)
        EXPECT_FALSE(deserializeTecnixMemoValue(state, good.substr(0, n))) << n;
    EXPECT_FALSE(deserializeTecnixMemoValue(state, good + "x"));

    // A node naming a node that comes after it, and a duplicate attribute.
    std::string forward(tecnixMemoPayloadMagic);
    forward += std::string{2, 6, 1, 1, 0};
    EXPECT_FALSE(deserializeTecnixMemoValue(state, forward));
    std::string duplicate(tecnixMemoPayloadMagic);
    duplicate += std::string{2, 0, 7, 2, 1, 'a', 0, 1, 'a', 0};
    EXPECT_FALSE(deserializeTecnixMemoValue(state, duplicate));

    // An unparsable string context element.
    std::string badContext(tecnixMemoPayloadMagic);
    badContext += std::string{1, 5, 1, 'x', 1, 1, '!'};
    EXPECT_FALSE(deserializeTecnixMemoValue(state, badContext));
}

} // namespace nix
