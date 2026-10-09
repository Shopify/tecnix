#include "nix/util/source-accessor.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

// FIXME: Use ForwardingSourceAccessor.
namespace {

class CachingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> next;

    boost::concurrent_flat_map<CanonPath, Stat> lstatCache;
    boost::concurrent_flat_map<CanonPath, std::string> readLinkCache;

    void anchor() override {};

public:
    CachingSourceAccessor(ref<SourceAccessor> next_)
        : next(std::move(next_))
    {
        displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        next->readFile(path, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        if (auto res = getConcurrent(lstatCache, path))
            return *res;

        auto st = next->maybeLstat(path);
        if (!st)
            return std::nullopt;

        /* Never evict, the evaluator better keep positive lookups cached. */
        lstatCache.emplace(path, *st);
        return st;
    }

    Stat lstat(const CanonPath & path) override
    {
        if (auto res = getConcurrent(lstatCache, path))
            return *res;

        auto st = next->lstat(path);
        /* Never evict, the evaluator better keep positive lookups cached. */
        lstatCache.emplace(path, st);
        return st;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        return next->readDirectory(path);
    }

    void readDirectory(
        const CanonPath & dirPath,
        std::function<void(SourceAccessor & subdirAccessor, const CanonPath & subdirRelPath)> callback) override
    {
        return next->readDirectory(dirPath, std::move(callback));
    }

    std::string readLink(const CanonPath & path) override
    {
        /* Tecnix: an accessor that tracks evaluation accesses records a
           symlink read itself, so answering from the cache would hide the
           read from any later tracking context (a retargeted symlink would
           then not invalidate that context's dependencies). Stats are
           different: they are recorded by the call sites that observe them,
           through `recordEvalAccess()`, which is forwarded below. */
        if (next->tracksEvalAccesses(path))
            return next->readLink(path);

        if (auto res = getConcurrent(readLinkCache, path))
            return *res;

        auto target = next->readLink(path);
        /* Never evict, the evaluator better keep positive lookups cached. */
        readLinkCache.emplace(path, target);
        return target;
    }

    std::string showPath(const CanonPath & path) override
    {
        return next->showPath(path);
    }

    void invalidateCache() override
    {
        lstatCache.clear();
        readLinkCache.clear();
        next->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return next->getPhysicalPath(path);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        return next->getFingerprint(path);
    }

    std::shared_ptr<const Provenance> getProvenance(const CanonPath & path) override
    {
        return next->getProvenance(path);
    }

    bool tracksEvalAccesses(const CanonPath & path) override
    {
        return next->tracksEvalAccesses(path);
    }

    void recordEvalAccess(const CanonPath & path) override
    {
        next->recordEvalAccess(path);
    }
};

} // namespace

ref<SourceAccessor> makeCachingSourceAccessor(ref<SourceAccessor> next)
{
    return make_ref<CachingSourceAccessor>(std::move(next));
}

} // namespace nix
