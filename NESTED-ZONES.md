# Nested Zones Design and Implementation Plan

This document describes the design for **nested (internal) zones** - zones that exist within other zones, providing encapsulation and modularity.

## Overview

Internal zones are:
- **Hidden** from their host zone's lazy-tree source (as if `_internal` doesn't exist)
- **Addressable** as first-class zones via extended paths like `//a/b/c/_internal/d/e`
- **Recursively nestable** - internal zones can have their own `_internal` with more zones

### Constraints

The `_internal` directory must contain precisely:
1. A `manifest.json`
2. Zone directories
3. No other files

Internal zones are only readable from:
- The enclosing zone
- Co-internal cousins within that enclosing zone

---

## Zone Path Algebra

### Grammar

```
zone_path      ::= top_level | internal
top_level      ::= "//" segments
internal       ::= zone_path "/_internal/" segments
segments       ::= name ("/" name)*
```

This grammar reveals the key insight: **an internal zone path is recursive** - the host of an internal zone can itself be an internal zone.

### Examples

| Path | Host | Internal Path |
|------|------|---------------|
| `//areas/tools/tec` | (root manifest) | — |
| `//areas/tools/tec/_internal/helpers` | `//areas/tools/tec` | `helpers` |
| `//areas/tools/tec/_internal/a/b/_internal/c` | `//areas/tools/tec/_internal/a/b` | `c` |

### The "Peel" Operation

Every zone path can be **peeled** into at most one layer:

```cpp
struct PeeledZonePath {
    std::optional<std::string> hostPath;  // nullopt for top-level
    std::string localPath;                 // The path to look up in manifest
};

PeeledZonePath peel(std::string_view path) {
    auto pos = path.rfind("/_internal/");
    if (pos == std::string_view::npos) {
        return {.hostPath = std::nullopt, .localPath = std::string(path)};
    }
    return {
        .hostPath = std::string(path.substr(0, pos)),
        .localPath = std::string(path.substr(pos + 11))  // skip "/_internal/"
    };
}
```

This is elegant because:
- `peel("//a/b/c")` → `{nullopt, "//a/b/c"}` — top-level
- `peel("//a/b/_internal/c")` → `{"//a/b", "c"}` — one level of nesting
- `peel("//a/_internal/b/_internal/c")` → `{"//a/_internal/b", "c"}` — recursive host

---

## Resolution Algorithm

```
resolveZone(path):
    peeled = peel(path)

    if peeled.hostPath is null:
        # Top-level zone: use root manifest
        manifest = readRootManifest()
        assert peeled.localPath in manifest
        treeSha = computeTreeShaFromWorldRoot(peeled.localPath)
        return Zone(path, treeSha, manifest[peeled.localPath].id)

    # Internal zone: resolve host first (recursive!)
    hostZone = resolveZone(peeled.hostPath)

    # Read host's internal manifest
    internalManifest = readFile(hostZone.tree, "_internal/manifest.json")
    assert peeled.localPath in internalManifest

    # Compute tree SHA relative to host
    treeSha = getSubtreeSha(hostZone.treeSha, "_internal/" + peeled.localPath)

    return Zone(path, treeSha, internalManifest[peeled.localPath].id)
```

The beauty: **one algorithm handles arbitrary nesting depth** through recursion.

---

## Source Filtering: The Disappearing `_internal`

Every zone's source accessor must filter out `_internal` directories **at every level**:

```cpp
class ZoneFilteringAccessor : public FilteringSourceAccessor {
    bool isAllowed(const CanonPath & path) override {
        // Check each path component
        for (auto it = path.begin(); it != path.end(); ++it) {
            if (*it == "_internal")
                return false;
        }
        return true;
    }
};
```

This means:
- `//a/b/c` sees everything EXCEPT any `_internal` subdirectories
- `//a/b/c/_internal/d` sees everything EXCEPT any `_internal` subdirectories within it
- Each zone is hermetically sealed from its internal zones

---

## Manifest Structure

**Root manifest** (`//.meta/manifest.json`):
```json
{
  "//areas/tools/tec": {"id": "W-123456"},
  "//areas/platform/core": {"id": "W-789abc"}
}
```

**Internal manifest** (`//areas/tools/tec/_internal/manifest.json`):
```json
{
  "helpers": {"id": "W-def000"},
  "test-utils": {"id": "W-def001"},
  "deeply/nested/thing": {"id": "W-def002"}
}
```

Note: Internal manifest paths are **relative** (no `//` prefix).

---

## Implementation Plan

### Phase 1: Zone Path Parsing Infrastructure

**File: `src/libexpr/primops/tectonix.cc`**

```cpp
namespace {

struct PeeledZonePath {
    std::optional<std::string> hostPath;
    std::string localPath;

    bool isInternal() const { return hostPath.has_value(); }
};

PeeledZonePath peelZonePath(std::string_view path) {
    auto pos = path.rfind("/_internal/");
    if (pos == std::string_view::npos) {
        return {.hostPath = std::nullopt, .localPath = std::string(path)};
    }
    return {
        .hostPath = std::string(path.substr(0, pos)),
        .localPath = std::string(path.substr(pos + 11))
    };
}

} // anonymous namespace
```

### Phase 2: Internal Manifest Reading

**Add to `src/libexpr/primops/tectonix.cc`:**

```cpp
static std::optional<nlohmann::json> readInternalManifest(
    EvalState & state,
    const Hash & hostTreeSha)
{
    auto repo = state.getWorldRepo();
    GitAccessorOptions opts{.exportIgnore = false, .smudgeLfs = false};
    auto accessor = repo->getAccessor(hostTreeSha, opts, "host");

    auto manifestPath = CanonPath("_internal/manifest.json");
    if (!accessor->pathExists(manifestPath))
        return std::nullopt;

    return nlohmann::json::parse(accessor->readFile(manifestPath));
}
```

### Phase 3: Recursive Tree SHA Computation

**Modify `EvalState::getWorldTreeSha` in `src/libexpr/eval.cc`:**

```cpp
Hash EvalState::getWorldTreeSha(std::string_view zonePath) const
{
    auto peeled = peelZonePath(zonePath);

    if (!peeled.isInternal()) {
        // Existing top-level logic (unchanged)
        return computeTreeShaFromWorldRoot(peeled.localPath);
    }

    // Internal zone: recursive resolution
    auto hostTreeSha = getWorldTreeSha(*peeled.hostPath);
    auto repo = getWorldRepo();

    // Navigate: hostTree -> _internal -> localPath
    auto internalTreeSha = repo->getSubtreeSha(hostTreeSha, "_internal");

    // Walk through localPath segments
    for (auto & segment : tokenizeString<std::vector<std::string>>(peeled.localPath, "/")) {
        internalTreeSha = repo->getSubtreeSha(internalTreeSha, segment);
    }

    return internalTreeSha;
}
```

### Phase 4: Zone Filtering Accessor

**Add to `src/libfetchers/filtering-source-accessor.cc` or inline:**

```cpp
class ZoneFilteringAccessor : public FilteringSourceAccessor {
public:
    ZoneFilteringAccessor(ref<SourceAccessor> next)
        : FilteringSourceAccessor(std::move(next), makeNotAllowedError) {}

private:
    static MakeNotAllowedError makeNotAllowedError(const CanonPath & path) {
        return RestrictedPathError(
            fmt("'%s' is hidden (inside _internal)", path));
    }

    bool isAllowed(const CanonPath & path) override {
        for (auto it = path.begin(); it != path.end(); ++it) {
            if (*it == "_internal")
                return false;
        }
        return true;
    }
};
```

### Phase 5: Updated Zone Resolution

**Modify `prim_unsafeTectonixInternalZone` in `src/libexpr/primops/tectonix.cc`:**

```cpp
static void prim_unsafeTectonixInternalZone(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto zonePath = state.forceStringNoCtx(*args[0], pos, "...");
    auto peeled = peelZonePath(zonePath);

    // Validate zone exists in appropriate manifest
    if (!peeled.isInternal()) {
        // Top-level: check root manifest (existing logic)
        auto manifest = readRootManifest(state, pos);
        if (!manifest.contains(std::string(zonePath)))
            state.error<EvalError>("'%s' is not a zone", zonePath).atPos(pos).debugThrow();
    } else {
        // Internal: resolve host, check its internal manifest
        auto hostTreeSha = state.getWorldTreeSha(*peeled.hostPath);
        auto internalManifest = readInternalManifest(state, hostTreeSha);

        if (!internalManifest)
            state.error<EvalError>("zone '%s' has no internal manifest", *peeled.hostPath)
                .atPos(pos).debugThrow();

        if (!internalManifest->contains(peeled.localPath))
            state.error<EvalError>("'%s' is not an internal zone of '%s'",
                peeled.localPath, *peeled.hostPath).atPos(pos).debugThrow();
    }

    // Get tree SHA (handles recursion internally)
    auto treeSha = state.getWorldTreeSha(zonePath);

    // ... rest of existing logic, but wrap accessor with ZoneFilteringAccessor
}
```

### Phase 6: Updated `mountZoneByTreeSha`

**Modify in `src/libexpr/eval.cc`:**

```cpp
StorePath EvalState::mountZoneByTreeSha(const Hash & treeSha, std::string_view zonePath)
{
    // ... existing cache check ...

    auto repo = getWorldRepo();
    GitAccessorOptions opts{.exportIgnore = true, .smudgeLfs = false};
    auto rawAccessor = repo->getAccessor(treeSha, opts, "zone");

    // NEW: Wrap with _internal filter
    auto accessor = make_ref<ZoneFilteringAccessor>(rawAccessor);

    // ... rest of existing logic ...
}
```

### Phase 7: Dirty Zone Detection for Internal Zones

**Modify `getTectonixDirtyZones` in `src/libexpr/eval.cc`:**

This is trickier because we need to:
1. Detect dirty files in the checkout
2. Map them to zones (including internal zones)
3. A file at `a/b/_internal/c/foo.nix` means zone `//a/b/_internal/c` is dirty

```cpp
// When processing dirty files, check if path contains _internal
// and attribute dirtiness to the correct internal zone

for (auto & dirtyFile : dirtyFiles) {
    auto zonePath = findEnclosingZone(dirtyFile, allManifests);
    dirtyZones[zonePath] = true;
}
```

---

## Summary of Changes

| Component | Change |
|-----------|--------|
| Zone path parsing | Add `peelZonePath()` function |
| Tree SHA computation | Recursive resolution for internal zones |
| Manifest lookup | Support internal manifests relative to host zones |
| Source accessor | Filter `_internal` at all levels |
| Zone validation | Check appropriate manifest (root vs internal) |
| Dirty detection | Attribute dirty files to correct zone level |

---

## Design Elegance

The elegance comes from:

1. **One grammar** for all zone paths
2. **One algorithm** (peel + recurse) for all resolution depths
3. **One filter** (`_internal` everywhere) for all source access
4. **Relative paths** in internal manifests (no duplication of host path)

---

## Edge Cases

### Zone path with consecutive `_internal`

`//a/_internal/_internal/b` — This shouldn't happen by design (manifest would declare `_internal/b`, not `_internal`). Should error gracefully.

### Missing internal manifest

Error clearly: "Zone X does not have an internal manifest"

### Zone references itself

Not possible with the manifest structure.

### Circular internal zones

Not possible — each `_internal` is strictly nested deeper.

### Dirty zone detection for internal zones

Need to check if the internal zone's files are dirty. The host zone being dirty doesn't mean the internal zone is dirty.

---

## Future Considerations: Access Control

The design mentions that internal zones are "only readable from the zone that encloses them or their co-internal cousins." This access control could be enforced at:

1. **Nix expression level** — The code that uses these builtins enforces who can call them
2. **Builtin level** — Add a "caller zone" context and validate access

This is deferred to a future phase.
