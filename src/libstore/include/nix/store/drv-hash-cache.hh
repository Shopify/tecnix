#pragma once
///@file
/// A persistent memo of `hashDerivationModulo` by derivation path.

#include "nix/store/derivations.hh"

#include <optional>

namespace nix {

/**
 * The stored hash of `drvPath`, if this machine has computed it before.
 *
 * Sound because a derivation path names the derivation's exact contents,
 * input derivation paths included, and its hash modulo is a function of
 * those contents alone. Rows are keyed by store directory as well, since a
 * store path's printed form depends on it.
 *
 * Gated by the `tecnix-drv-hash-cache` setting. Failures are misses.
 */
std::optional<DrvHash> lookupPersistentDrvHash(const StoreDirConfig & store, const StorePath & drvPath);

/**
 * Remember a freshly computed hash. Writes are batched; a pending batch is
 * written when it fills up and when the process exits.
 */
void recordPersistentDrvHash(const StoreDirConfig & store, const StorePath & drvPath, const DrvHash & hash);

/** Write any pending batch now. */
void flushPersistentDrvHashes();

} // namespace nix
