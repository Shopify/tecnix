# `gs://` binary caches: intent

This document records *why* the GCS binary cache store and its credential
handling are shaped the way they are, so that the design can be reviewed and
revisited without reconstructing the reasoning. The user-facing description is
in `src/libstore/gcs-binary-cache-store.md` and the `gcs-credential-helper`
setting's documentation; this is the layer underneath.

## Headlines

- **`gs://` is a binary cache store in its own right**, not an `s3://`
  store pointed at Google: reads through the XML API, uploads through the
  JSON API's simple upload, signed like any other cache.
- **Credentials come from a credential helper or from Application Default
  Credentials, and nothing else.** The helper is for interactive machines
  with a per-user token broker; ADC (a credentials file, or the GCE metadata
  server with no configuration at all) is for unattended ones. No
  fall-through between them.
- **The helper is a Git credential helper**, called the way Go and Cargo
  call one, and **run as the user the daemon is serving**, not as root.
- **Nix stores no credential on disk.** On a laptop there is no long-lived
  secret anywhere: the broker hands out short-lived tokens, Nix keeps them in
  memory until they expire.
- **The helper's expiry is the only lifetime policy.** No fixed reuse window;
  a helper that reports no expiry is asked for every request.
- **Authentication failures fail the build instead of falling back to
  building from source.** Broken credentials never degrade to anonymous
  requests; a 403 from GCS is an error, not a cache miss; a store whose
  credentials are broken still opens and fails at first use rather than being
  quietly dropped. `--fallback` is the explicit opt-in to build locally
  anyway.
- **The error carries the fix.** The helper's standard error (the broker's
  own instructions) is the body of Nix's error; a 403 shows GCS's message
  naming the principal and the missing permission.
- **Nothing is added to the hot path.** A token costs one helper run per
  store per token lifetime (about hourly, ~10 ms); every other request pays a
  mutex and a clock read. Transfers use the same curl pool as `s3://` and
  `https://`.
- **No multipart uploads.** The limit that motivates them is S3's; GCS takes
  5 TiB objects by any method, and the upload isn't the bottleneck on a
  laptop.
- **No credentials-file setting, no `GCE_METADATA_HOST`, no
  `storage-class`/`project-id`**: each would be a third thing to own for no
  added capability.

## Goal

Substitute from, and copy to, a Google Cloud Storage bucket with the same
guarantees as any other binary cache, under two constraints that the existing
`s3://`-compatible route to GCS does not meet:

1. **No long-lived secret on interactive machines.** A developer's laptop
   should authenticate to the cache the way it authenticates to everything
   else: through whatever per-user credential store or token broker the
   machine already has, with short-lived tokens that Nix never stores.
2. **Authentication problems are errors, not cache misses.** A wrong, expired
   or missing credential must stop the build with a message that says what to
   do. It must never make the cache look empty, because "looks empty" means
   building everything from source, silently.

## Two modes of credentials, and no third

Credentials for `gs://` come from exactly one of two sources, chosen by
configuration, with no fall-through from one to the other:

| Mode | For | Source |
|---|---|---|
| **Credential helper** | interactive machines, where a per-user broker exists and the Nix daemon runs as root | `gcs-credential-helper`: a command run *as the connecting user* |
| **Application Default Credentials** | unattended machines | upstream's provider: `GOOGLE_APPLICATION_CREDENTIALS`, the `gcloud` ADC file, or the GCE metadata server (which needs no configuration at all on GCE/GKE) |

The helper, when configured, is the only source. Configured-but-broken
credentials in either mode are an error. Only the complete absence of any
configured credential leads to anonymous requests, which is what a public
bucket needs.

Things deliberately *not* added, because they would be a third thing to own
for no additional capability:

- a `gcs-credentials-file` setting (the `GOOGLE_APPLICATION_CREDENTIALS`
  environment variable is how every Google client finds a baked credential,
  and the daemon's environment is already how the fleet feeds it its S3
  credentials);
- `GCE_METADATA_HOST` (only ever used by hand against a fake);
- multipart uploads (the 5 GiB limit that motivates them is S3's; GCS takes
  5 TiB objects by any method, and on a laptop the import pipeline, not the
  upload, is the bottleneck);
- `storage-class` and `project-id` parameters (they did nothing).

## The credential helper

### Why a subprocess at all

Per-user credential stores and token brokers answer only processes of the
user they serve; the Nix daemon runs as root and does the substituting. The
daemon already knows which user each connection belongs to (that is how
`trusted-users` is decided), so the worker records the client's identity and
runs the helper **as that user**, with that user's home directory, a minimal
`PATH`, and `XDG_RUNTIME_DIR` where `/run/user/<uid>` exists. The broker then
sees an ordinary same-user caller; the daemon holds no credential of its own,
long-lived or otherwise.

The alternative — the client resolving credentials and forwarding them to the
daemon, as Docker does — would need a channel for the daemon to ask the
client for a refresh mid-build, which the daemon protocol doesn't have.

### Why Git's credential protocol

The helper is a [Git credential helper](https://git-scm.com/docs/gitcredentials):
Nix runs `<command> get` with `protocol`, `host`, `path` and the `authtype`
capability on its standard input and reads attributes back; the token is
`credential` with `authtype=Bearer` (or `password`, from a helper without the
capability), and `password_expiry_utc` says when it expires.

Alternatives considered, in the order they were tried:

- **A format of our own** ("print a token"). Nobody else implements it, and
  it cannot carry an expiry without inventing more format.
- **Git's output format without its request** (no `get`, no stdin). Smaller,
  but then no actual Git credential helper works with it, so it is nobody's
  protocol.
- **The Bazel credential-helper specification** (EngFlow). The right shape —
  a URI in, arbitrary headers and an RFC 3339 `expires` out — and purpose-built
  for a build tool talking HTTP, but its ecosystem is Bazel's.
- **Git's protocol in full** (chosen). It carries the same information (host
  and path in, bearer credential and expiry out); it is already the de facto
  "credential for this host" call on a developer's machine, used by non-Git
  consumers such as Go (`GOAUTH=git`), Cargo, git-lfs and jj; the credential
  stores and brokers that exist already speak it; and it is the simplest to
  parse (key=value lines, Unix-time expiry). A broker that wants to serve many
  tools needs to implement one protocol, and this is the one with the most
  reach.

The one cultural difference worth knowing: Git helpers may prompt. Under the
daemon there is no terminal, so a prompting helper fails rather than hangs.

### What is asked, and how often

A store asks once, for its root URL (`protocol=https`,
`host=storage.googleapis.com`, `path=<bucket>/<prefix>/`), and uses the
answer for every request under it. `builtins.fetchurl` of a `gs://` URL asks
for that object. The scope of the token is the helper's business: reads need
`devstorage.read_only`, uploads `devstorage.read_write`.

### Lifetime: the helper's expiry is the only policy

- With `password_expiry_utc`, the token is reused until **60 seconds before**
  it. The margin covers the gap between Nix attaching the token and the
  request leaving (queueing behind `http-connections`), not the transfer's
  duration: HTTP authenticates a request when its headers arrive and never
  again, so a long download or upload that starts with a valid token
  completes. 60 s is upstream's number for its own ADC provider, kept for
  consistency; anything from 10 s up is defensible.
- Without an expiry, the helper is run **for every request**, with a one-time
  warning naming the fix. A fixed reuse window would be a second lifetime
  policy, correct only while it happens to agree with the helper's own, and
  nothing would tell anyone when it stopped agreeing. This is the conservative
  reading of "the tool may choose a duration": zero.
- An already expired or unparsable expiry is an error.

Consequence for brokers: a broker that serves cached tokens with a freshness
floor of *F* seconds and a tool margin of 60 s will see per-request calls for
at most `60 − F` seconds once per token lifetime. That is benign and keeps the
two margins independent, which is the point.

### Failure

A failed run is retried once after a second (a broker can fail transiently,
and the loud-failure policy below turns a hiccup into a failed build without
this counterweight). After that, a non-zero exit, a missing credential, an
`authtype` other than `Bearer`, or an expired credential is an error.

**The helper's standard error is the body of that error.** In a daemon worker
the helper's stderr would otherwise reach only the daemon's log, and it is
precisely the part that tells the user what to do ("check: …", "run …"). Nix
adds only the framing: which helper, which exit code.

## Errors reach the user

The principle: an authentication problem is a configuration problem for the
user to fix. Nix's job is to stop and say so, at the point of use, with the
most specific message available. `--fallback` remains the explicit opt-in to
build locally regardless; it is off by default.

Nix's own machinery already does most of this — a substituter that throws
fails the build unless `--fallback` is set — but three places turned
authentication failures into something quieter, and each is closed:

1. **Credential acquisition.** Upstream's GCS provider fell back to anonymous
   requests on any failure to obtain a token. Now only "nothing configured"
   is anonymous; a configured credential that cannot be used (expired login,
   failing helper, `GOOGLE_APPLICATION_CREDENTIALS` naming a missing file) is
   an error with a hint.
2. **403 as "not found".** `HttpBinaryCacheStore` treats 403 as a missing
   file because S3 answers 403 for a missing object in an unlistable bucket.
   GCS does not: with access, a missing object is a 404; a 403 means denied,
   whether the object exists or not (verified against a real bucket). The
   base class now asks a virtual `isMissing()`, which the GCS store narrows
   to 404. Besides stopping the build, this keeps the denial out of the
   negative narinfo cache, where it would otherwise be remembered for an hour
   after the credential is fixed.
3. **Failing to open.** A substituter whose `init()` throws is dropped with a
   warning — upstream's tolerance for unreachable caches, which should stay —
   and `init()` reads `nix-cache-info`. On a cold narinfo disk cache, wrong
   credentials therefore meant a warning and no substituter, i.e. building
   from source; on a warm cache they meant an error. The GCS store now opens
   regardless of a credential error and lets its first query fail instead,
   without the narinfo disk cache (nothing is known about the cache to
   record). Behaviour no longer depends on cache history.

What the user sees, through a daemon, on a cold cache:

| Situation | Result |
|---|---|
| Helper fails (broker down, not enrolled, …) | `error: GCS credential helper '…' failed with exit code N:` followed by the helper's own stderr |
| Helper returns something that isn't a bearer credential | `error: … returned no credential` / `returned a 'Basic' credential, but GCS authenticates with a bearer token` |
| Token lacks access, or no credentials on a private bucket | `error: unable to download '…/<hash>.narinfo': HTTP error 403` with GCS's body, which names the principal and the missing permission |
| Token invalid or expired mid-way | `HTTP error 401`; the next run asks the helper again |
| Working credentials | `copying path '…' from 'gs://…'` |

Rolling this out changes the failure mode of a laptop with a broken broker
credential from "slow" (quietly building from source) to "stuck until the
broker is fixed". That is the intent, and worth saying in the rollout notes.

## Performance

Substitution speed is a first-order requirement, so each choice above was
checked against where the time actually goes:

| Step | Cost | Changed by this work? |
|---|---|---|
| Obtaining a token | one helper run per store per token lifetime (about hourly per daemon worker), ~10 ms median against a local broker; every other request pays a mutex and a clock read | new, and cheaper than the alternatives: ADC's hourly refresh is an HTTPS call to Google; the `s3://` route runs the AWS credential chain, which on a laptop probes the EC2 metadata endpoint and waits for it to time out |
| First use per connection | lazy: the helper runs only when a request needs a token, so a build of already-present paths runs nothing; `nix-cache-info` is fetched once per narinfo-disk-cache TTL, as for any HTTP cache | no |
| narinfo lookups | one HTTPS GET each, `http-connections` (25) in parallel, HTTP/2 on, positive and negative disk cache | no: same curl pool as `s3://` and `https://` |
| NAR transfer | same engine; 1 → 8 parallel streams measured 37 → 58 MiB/s on a laptop, but `nix copy` end to end tops out around 126 MiB/s on NAR import regardless | no; this measurement is why multipart uploads were dropped |
| Signing | one bearer header | cheaper than S3's per-request SigV4; not material |

Nothing in the design adds a round trip or a synchronisation point to the
healthy path. Two things it avoids outright: the GCE metadata probe (only the
ADC path makes it, and only when no credentials file is found — a latency
hazard for an *un*configured laptop, moot with the helper), and the AWS
chain's metadata probe that the `s3://` route pays on misconfigured laptops.

The degraded case to know about: a helper that reports no expiry is run for
every request, serialized on the provider's mutex — roughly +10 ms per
narinfo, so +10 s on a thousand-path closure, with a one-time warning. That is
the price of not inventing a reuse window, and it is why a helper must report
expiry. `builtins.fetchurl` of a `gs://` URL also runs the helper per fetch;
rare enough to leave as is (memoising providers per host would fix it).

## Verification

- Unit tests with an in-memory fake GCS: uploads (JSON simple upload, content
  encoding as object metadata), reads, prefixes, endpoints, anonymous versus
  authenticated writes, denied access as an error, broken credentials as an
  error that does not prevent opening, the exact Git request, expiry reuse
  and refusal, stderr capture, retry.
- A NixOS VM test against a GCS emulator: upload and signed read-back, and
  the daemon substituting for an unprivileged user with the helper running
  *as that user*.
- Against a real private bucket (reads only): the store and `fetchurl`
  through a helper; the three daemon outcomes above, cold cache, no crashes.

## Open follow-ups

- **Re-ask the helper on a 401.** Git calls `erase` when a server rejects a
  credential, so the helper can drop a cached token; Nix currently treats 401
  as fatal and does not. Worth adding if revoked tokens turn out to linger in
  broker caches.
- **Resolve the token when a transfer attempt starts.** A retried transfer
  reuses the token captured when the request was built, so a transfer that
  fails late and retries after expiry fails for good. The ADC path shares
  this.
- **Unverified paths.** A write to real GCS (only read-only credentials were
  available); the root daemon impersonating its client on macOS specifically
  (the VM test is Linux); a real broker-backed helper rather than a stand-in
  printing the same bytes.
- **Generalising the helper to `https://` caches** would be the natural
  upstream story ("credential helpers, like Go's `GOAUTH`"); the setting is
  GCS-scoped here on purpose.
- **Review shape.** The ~70 lines of daemon plumbing (client identity, macOS
  peer-credential fix) could be their own commit, and the store could land
  as one PR with the helper as a second.
