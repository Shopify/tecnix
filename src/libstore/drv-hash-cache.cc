#include "nix/store/drv-hash-cache.hh"
#include "nix/store/globals.hh"
#include "nix/store/sqlite.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/sync.hh"
#include "nix/util/users.hh"

#include <sqlite3.h>

namespace nix {

namespace {

const char * drvHashCacheSchema = R"(
create table if not exists DrvHashes (
    storeDir  text not null,
    drvPath   text not null,
    deferred  integer not null,
    hashes    text not null,
    primary key (storeDir, drvPath)
);
)";

/* A batch this size costs one transaction instead of hundreds. */
constexpr size_t drvHashBatchSize = 512;

/* One line per output: name, a space, the hash in SRI form. Output names
   never contain spaces or newlines. */
std::string encodeHashes(const DrvHash & hash)
{
    std::string out;
    for (auto & [name, h] : hash.hashes) {
        out += name;
        out += ' ';
        out += h.to_string(HashFormat::SRI, true);
        out += '\n';
    }
    return out;
}

std::optional<DrvHash> decodeHashes(std::string_view encoded, bool deferred)
{
    DrvHash result{.kind = deferred ? DrvHash::Kind::Deferred : DrvHash::Kind::Regular};
    while (!encoded.empty()) {
        auto eol = encoded.find('\n');
        if (eol == encoded.npos)
            return std::nullopt;
        auto line = encoded.substr(0, eol);
        encoded.remove_prefix(eol + 1);
        auto space = line.find(' ');
        if (space == line.npos || space == 0)
            return std::nullopt;
        result.hashes.insert_or_assign(std::string(line.substr(0, space)), Hash::parseSRI(line.substr(space + 1)));
    }
    if (result.hashes.empty())
        return std::nullopt;
    return result;
}

struct Pending
{
    std::string storeDir, drvPath, hashes;
    bool deferred;
};

struct DrvHashCache
{
    struct State
    {
        bool opened = false;
        bool broken = false;
        SQLite db;
        SQLiteStmt query, insert;
        std::vector<Pending> pending;
    };

    Sync<State> state_;

    /* Open on first use; any failure makes the cache a no-op for this process. */
    State * open(State & state)
    {
        if (state.broken)
            return nullptr;
        if (state.opened)
            return &state;
        state.opened = true;
        try {
            auto dbPath = getCacheDir() / "tecnix-drv-hashes-v1.sqlite";
            createDirs(dbPath.parent_path());
            state.db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
            state.db.isCache();
            state.db.exec(drvHashCacheSchema);
            state.query.create(state.db, "select deferred, hashes from DrvHashes where storeDir = ? and drvPath = ?");
            state.insert.create(
                state.db, "insert or replace into DrvHashes(storeDir, drvPath, deferred, hashes) values (?, ?, ?, ?)");
            return &state;
        } catch (Error & e) {
            debug("tecnix drv hash cache unavailable: %s", e.what());
            state.broken = true;
            return nullptr;
        }
    }

    void flush(State & state)
    {
        if (state.pending.empty() || !open(state))
            return;
        try {
            SQLiteTxn txn(state.db);
            for (auto & p : state.pending)
                state.insert.use()
                    .apply(p.storeDir)
                    .apply(p.drvPath)
                    .apply(int64_t(p.deferred ? 1 : 0))
                    .apply(p.hashes)
                    .exec();
            txn.commit();
        } catch (Error & e) {
            debug("could not write the tecnix drv hash cache: %s", e.what());
        }
        state.pending.clear();
    }

    ~DrvHashCache()
    {
        try {
            auto state(state_.lock());
            flush(*state);
        } catch (...) {
        }
    }
};

DrvHashCache & drvHashCache()
{
    static DrvHashCache cache;
    return cache;
}

} // namespace

std::optional<DrvHash> lookupPersistentDrvHash(const StoreDirConfig & store, const StorePath & drvPath)
{
    if (!settings.tecnixDrvHashCache)
        return std::nullopt;
    auto state(drvHashCache().state_.lock());
    if (!drvHashCache().open(*state))
        return std::nullopt;
    try {
        auto query(state->query.use().apply(store.storeDir).apply(drvPath.to_string()));
        if (!query.next())
            return std::nullopt;
        return decodeHashes(query.getStr(1), query.getInt(0) != 0);
    } catch (Error & e) {
        debug("could not read the tecnix drv hash cache: %s", e.what());
        return std::nullopt;
    }
}

void recordPersistentDrvHash(const StoreDirConfig & store, const StorePath & drvPath, const DrvHash & hash)
{
    if (!settings.tecnixDrvHashCache)
        return;
    auto state(drvHashCache().state_.lock());
    state->pending.push_back({
        .storeDir = store.storeDir,
        .drvPath = std::string(drvPath.to_string()),
        .hashes = encodeHashes(hash),
        .deferred = hash.kind == DrvHash::Kind::Deferred,
    });
    if (state->pending.size() >= drvHashBatchSize)
        drvHashCache().flush(*state);
}

void flushPersistentDrvHashes()
{
    auto state(drvHashCache().state_.lock());
    drvHashCache().flush(*state);
}

} // namespace nix
