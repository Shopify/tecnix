#pragma once
///@file

#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"

#include <functional>
#include <optional>

#ifndef _WIN32
#  include <sys/types.h>
#endif

namespace nix::daemon {

enum RecursiveFlag : bool { NotRecursive = false, Recursive = true };

/**
 * Serve a client on the given file descriptors.
 *
 * @param setupTelemetry Called once after the handshake and logger
 * setup, with the W3C `traceparent` string received from the client
 * (empty if the client sent none or the protocol feature was not
 * negotiated). Allows the caller to set up distributed tracing for
 * the connection, e.g. by adding a tracing logger.
 */
void processConnection(
    ref<Store> store,
    FdSource && from,
    FdSink && to,
    TrustedFlag trusted,
    RecursiveFlag recursive,
    std::function<void(std::string_view traceparent)> setupTelemetry = {});

#ifndef _WIN32
/**
 * The user on whose behalf this process is serving a daemon connection.
 *
 * The daemon forks one worker process per client connection, so a worker
 * has exactly one client. Helpers that act on the user's behalf, such as
 * the `gcs-credential-helper`, run as this user rather than as the
 * (typically root) daemon, so that per-user services can authenticate
 * them.
 */
struct Client
{
    uid_t uid;
    gid_t gid;
};

/**
 * Record the client served by this process. Called by the daemon in the
 * forked worker before `processConnection()`.
 */
void setClient(std::optional<Client> client);

/**
 * The client served by this process, or none if this process isn't a
 * daemon worker (or the client's identity is unknown).
 */
std::optional<Client> getClient();
#endif

} // namespace nix::daemon
