/**
 * Payloads for `builtins.tecnixPersistentMemo` rows (see memo-value.hh).
 *
 * Layout after the magic, all integers as LEB128 varints:
 *
 *   nodeCount
 *   node*          in dependency order: a node only refers to earlier nodes
 *
 * and the last node is the root. A node is a tag byte followed by
 *
 *   null, false, true   nothing
 *   int                 zigzag-encoded value
 *   float               8 bytes, the IEEE-754 bits, little-endian
 *   string              len, bytes, contextCount, (len, bytes)* in the
 *                       context's own encoding (NixStringContextElem)
 *   list                count, node id*
 *   attrs               count, (name len, name bytes, node id)*, names sorted
 */

#include "nix/expr/tecnix/memo-value.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-api.hh"

#include <bit>
#include <cstring>
#include <unordered_map>

namespace nix {

namespace {

enum class Tag : uint8_t {
    Null = 0,
    False = 1,
    True = 2,
    Int = 3,
    Float = 4,
    String = 5,
    List = 6,
    Attrs = 7,
};

void putVarint(std::string & out, uint64_t n)
{
    while (n >= 0x80) {
        out.push_back(char((n & 0x7f) | 0x80));
        n >>= 7;
    }
    out.push_back(char(n));
}

void putBytes(std::string & out, std::string_view s)
{
    putVarint(out, s.size());
    out.append(s);
}

struct Writer
{
    EvalState & state;
    std::string nodes;
    uint64_t nodeCount = 0;

    /* Containers already written, by identity: attribute sets by their
       bindings, lists by the value holding them. Values copied from one
       another share bindings, so a record referenced from many places is
       written once. */
    std::unordered_map<const void *, uint64_t> written;

    /* Containers being written, to fail closed on a cycle instead of
       recursing forever. */
    std::unordered_map<const void *, bool> inProgress;

    uint64_t emit(Tag tag)
    {
        nodes.push_back(char(tag));
        return nodeCount++;
    }

    uint64_t write(Value & v)
    {
        checkInterrupt();
        switch (v.type()) {
        case nNull:
            return emit(Tag::Null);
        case nBool:
            return emit(v.boolean() ? Tag::True : Tag::False);
        case nInt: {
            auto n = v.integer().value;
            auto id = emit(Tag::Int);
            putVarint(nodes, (uint64_t(n) << 1) ^ uint64_t(n >> 63));
            return id;
        }
        case nFloat: {
            auto bits = std::bit_cast<uint64_t>(v.fpoint());
            auto id = emit(Tag::Float);
            for (int i = 0; i < 8; i++)
                nodes.push_back(char((bits >> (8 * i)) & 0xff));
            return id;
        }
        case nString: {
            auto id = emit(Tag::String);
            putBytes(nodes, v.string_view());
            auto * context = v.context();
            putVarint(nodes, context ? context->size() : 0);
            if (context)
                for (auto * elem : *context)
                    putBytes(nodes, elem->view());
            return id;
        }
        case nList:
            return container(&v, [&]() {
                std::vector<uint64_t> elems;
                elems.reserve(v.listSize());
                for (auto * elem : v.listView())
                    elems.push_back(write(*elem));
                auto id = emit(Tag::List);
                putVarint(nodes, elems.size());
                for (auto e : elems)
                    putVarint(nodes, e);
                return id;
            });
        case nAttrs:
            return container(v.attrs(), [&]() {
                std::vector<std::pair<std::string_view, uint64_t>> fields;
                fields.reserve(v.attrs()->size());
                for (auto * attr : v.attrs()->lexicographicOrder(state.symbols))
                    fields.emplace_back(state.symbols[attr->name], write(*attr->value));
                auto id = emit(Tag::Attrs);
                putVarint(nodes, fields.size());
                for (auto & [name, child] : fields) {
                    putBytes(nodes, name);
                    putVarint(nodes, child);
                }
                return id;
            });
        case nPath:
            throw TecnixMemoUnserializable("a memoized value cannot contain a path");
        case nFunction:
            throw TecnixMemoUnserializable("a memoized value cannot contain a function");
        case nExternal:
            throw TecnixMemoUnserializable("a memoized value cannot contain an external value");
        case nThunk:
        case nFailed:
            throw TecnixMemoUnserializable("a memoized value must be fully evaluated");
        }
        unreachable();
    }

    uint64_t container(const void * identity, auto && body)
    {
        if (auto i = written.find(identity); i != written.end())
            return i->second;
        if (!inProgress.emplace(identity, true).second)
            throw TecnixMemoUnserializable("a memoized value cannot be cyclic");
        auto id = body();
        inProgress.erase(identity);
        written.emplace(identity, id);
        return id;
    }
};

struct Reader
{
    std::string_view in;

    std::optional<uint64_t> varint()
    {
        uint64_t n = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (in.empty())
                return std::nullopt;
            auto byte = uint8_t(in.front());
            in.remove_prefix(1);
            n |= uint64_t(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return n;
        }
        return std::nullopt;
    }

    std::optional<std::string_view> bytes()
    {
        auto len = varint();
        if (!len || *len > in.size())
            return std::nullopt;
        auto s = in.substr(0, *len);
        in.remove_prefix(*len);
        return s;
    }
};

/** The store path a context element needs valid, or nullopt if it names none we can check. */
std::optional<StorePath> contextStorePath(const NixStringContextElem & elem)
{
    return std::visit(
        overloaded{
            [](const NixStringContextElem::Opaque & o) -> std::optional<StorePath> { return o.path; },
            [](const NixStringContextElem::DrvDeep & d) -> std::optional<StorePath> { return d.drvPath; },
            [](const NixStringContextElem::Built & b) -> std::optional<StorePath> {
                if (auto * opaque = std::get_if<SingleDerivedPath::Opaque>(&b.drvPath->raw()))
                    return opaque->path;
                return std::nullopt;
            },
            [](const NixStringContextElem::Path & p) -> std::optional<StorePath> { return p.storePath; },
        },
        elem.raw);
}

} // namespace

std::string serializeTecnixMemoValue(EvalState & state, Value & v)
{
    Writer writer{.state = state};
    writer.write(v);

    std::string out;
    out.reserve(tecnixMemoPayloadMagic.size() + 10 + writer.nodes.size());
    out.append(tecnixMemoPayloadMagic);
    putVarint(out, writer.nodeCount);
    out.append(writer.nodes);
    return out;
}

std::optional<DecodedTecnixMemoValue> deserializeTecnixMemoValue(EvalState & state, std::string_view payload)
{
    if (!payload.starts_with(tecnixMemoPayloadMagic))
        return std::nullopt;
    Reader reader{payload.substr(tecnixMemoPayloadMagic.size())};

    auto count = reader.varint();
    // Every node takes at least one byte.
    if (!count || *count == 0 || *count > reader.in.size())
        return std::nullopt;

    DecodedTecnixMemoValue result{.value = nullptr};
    std::vector<Value *> nodes;
    nodes.reserve(*count);

    auto child = [&]() -> Value * {
        auto id = reader.varint();
        if (!id || *id >= nodes.size())
            return nullptr;
        return nodes[*id];
    };

    try {
        for (uint64_t i = 0; i < *count; i++) {
            if (reader.in.empty())
                return std::nullopt;
            auto tag = Tag(uint8_t(reader.in.front()));
            reader.in.remove_prefix(1);
            auto * v = state.allocValue();
            switch (tag) {
            case Tag::Null:
                v->mkNull();
                break;
            case Tag::False:
            case Tag::True:
                v->mkBool(tag == Tag::True);
                break;
            case Tag::Int: {
                auto z = reader.varint();
                if (!z)
                    return std::nullopt;
                v->mkInt(NixInt::Inner((*z >> 1) ^ (~(*z & 1) + 1)));
                break;
            }
            case Tag::Float: {
                if (reader.in.size() < 8)
                    return std::nullopt;
                uint64_t bits = 0;
                for (int b = 0; b < 8; b++)
                    bits |= uint64_t(uint8_t(reader.in[b])) << (8 * b);
                reader.in.remove_prefix(8);
                v->mkFloat(std::bit_cast<NixFloat>(bits));
                break;
            }
            case Tag::String: {
                auto s = reader.bytes();
                auto contextCount = reader.varint();
                if (!s || !contextCount || *contextCount > reader.in.size())
                    return std::nullopt;
                NixStringContext context;
                for (uint64_t c = 0; c < *contextCount; c++) {
                    auto encoded = reader.bytes();
                    if (!encoded)
                        return std::nullopt;
                    auto elem = NixStringContextElem::parse(*encoded);
                    auto storePath = contextStorePath(elem);
                    if (!storePath)
                        return std::nullopt;
                    result.storePaths.insert(std::move(*storePath));
                    context.insert(std::move(elem));
                }
                if (context.empty())
                    v->mkString(*s, state.mem);
                else
                    v->mkString(*s, context, state.mem);
                break;
            }
            case Tag::List: {
                auto n = reader.varint();
                if (!n || *n > reader.in.size())
                    return std::nullopt;
                auto list = state.buildList(*n);
                for (uint64_t e = 0; e < *n; e++) {
                    auto * elem = child();
                    if (!elem)
                        return std::nullopt;
                    list[e] = elem;
                }
                v->mkList(list);
                break;
            }
            case Tag::Attrs: {
                auto n = reader.varint();
                if (!n || *n > reader.in.size())
                    return std::nullopt;
                auto bindings = state.buildBindings(*n);
                std::optional<std::string_view> previous;
                for (uint64_t a = 0; a < *n; a++) {
                    auto name = reader.bytes();
                    // Names are written sorted; anything else (a duplicate
                    // included) is not one of ours.
                    if (!name || (previous && *name <= *previous))
                        return std::nullopt;
                    previous = name;
                    auto * value = child();
                    if (!value)
                        return std::nullopt;
                    bindings.insert(state.symbols.create(*name), value);
                }
                v->mkAttrs(bindings);
                break;
            }
            default:
                return std::nullopt;
            }
            nodes.push_back(v);
        }
    } catch (Error &) {
        // An unparsable string context element or store path.
        return std::nullopt;
    }

    if (!reader.in.empty())
        return std::nullopt;
    result.value = nodes.back();
    return result;
}

} // namespace nix
