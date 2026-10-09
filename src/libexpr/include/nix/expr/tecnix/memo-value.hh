#pragma once
///@file
/// The payload of a `builtins.tecnixPersistentMemo` cache row: a finished
/// value as data, with its string contexts, and back.

#include "nix/expr/value.hh"
#include "nix/store/path.hh"
#include "nix/util/error.hh"

#include <optional>
#include <string>
#include <string_view>

namespace nix {

class EvalState;

/**
 * The value can't be stored as data: it holds a function, a path, an
 * external value or a cycle. A memo whose result fails this way is evaluated
 * as usual and simply not persisted.
 */
MakeError(TecnixMemoUnserializable, Error);

/** Leading bytes of every memo payload; a payload without them is a miss. */
constexpr std::string_view tecnixMemoPayloadMagic{"TXMV1\0", 6};

/**
 * Serialize a deeply forced `v`: null, booleans, integers, floats, strings
 * with their contexts, lists and attribute sets. Attribute sets and lists
 * reached more than once are written once and shared again on decode, so a
 * graph of crate records that reference each other stays linear in size.
 *
 * @throws TecnixMemoUnserializable for anything else, or for a cycle.
 */
std::string serializeTecnixMemoValue(EvalState & state, Value & v);

struct DecodedTecnixMemoValue
{
    Value * value;

    /**
     * Every store path the value's string contexts name (opaque paths and
     * derivations). A hit is only usable if all of them are valid.
     */
    StorePathSet storePaths;
};

/**
 * Rebuild a value from `serializeTecnixMemoValue`'s output. A malformed or
 * foreign payload is `std::nullopt`, an ordinary miss; it never throws for
 * bad input.
 */
std::optional<DecodedTecnixMemoValue> deserializeTecnixMemoValue(EvalState & state, std::string_view payload);

} // namespace nix
