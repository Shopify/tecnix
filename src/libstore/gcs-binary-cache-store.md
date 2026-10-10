R"(

**Store URL format**: `gs://`*bucket-name*[`/`*prefix*]

This store allows reading and writing a binary cache stored in a
[Google Cloud Storage](https://cloud.google.com/storage) (GCS) bucket,
optionally under a prefix within the bucket.
This store shares many idioms with the [HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md).

For a bucket named `example-nix-cache`, the binary cache URL is <gs://example-nix-cache>.
Objects are read through the [GCS XML API](https://cloud.google.com/storage/docs/xml-api/overview)
at `https://storage.googleapis.com/example-nix-cache/...` and written with the
JSON API's [simple upload](https://cloud.google.com/storage/docs/json_api/v1/how-tos/simple-upload).
Uploaded objects get the bucket's default storage class.

### Authentication

Nix uses a Google credentials JSON file (a service account key, or the
`application_default_credentials.json` written by `gcloud auth
application-default login`) or the GCE metadata server, looked up in this
order:

1. The file named by the `GOOGLE_APPLICATION_CREDENTIALS` environment variable.
2. `~/.config/gcloud/application_default_credentials.json`.
3. The GCE metadata server (automatic on Compute Engine, GKE, Cloud Run, etc.).

Substitutions are performed by the Nix daemon when one is in use, so the
daemon's own environment and home directory are what matter, and every
client it serves acts as that one identity. For read operations, Nix
requests the `devstorage.read_only` OAuth2 scope and for writes
`devstorage.read_write`; the scopes only matter for service account keys,
since user credentials and the metadata server grant the scopes they were
configured with.

### Anonymous reads

If no credentials are configured at all, reads are attempted anonymously,
which works for publicly readable buckets. Writes to Google's endpoint always
require credentials; writes to another `endpoint` (an emulator, say) use them
only when there are any. Credentials that are configured but cannot be used
(for example a revoked or expired `gcloud` login) are an error, rather than a
silent fall back to anonymous access. Likewise, access that GCS denies (HTTP
403) is an error rather than a cache miss: GCS answers 404 for an object that
is missing, so a 403 means the credentials are wrong, not that the path has to
be built. Pass `--fallback` to build locally regardless.

Alternatively, a public bucket can be used with the
[HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md)
at `https://storage.googleapis.com/example-nix-cache`.

### Examples

- To use a GCS bucket as a substituter:

  ```console
  $ nix build --substituters 'gs://example-nix-cache' --trusted-public-keys '...' ...
  ```

- To upload to a GCS binary cache:

  ```console
  $ nix copy nixpkgs#hello --to 'gs://example-nix-cache'
  ```

- To upload to a prefix within a bucket:

  ```console
  $ nix copy nixpkgs#hello --to 'gs://example-nix-cache/nix'
  ```

- To use a GCS emulator such as [fake-gcs-server](https://github.com/fsouza/fake-gcs-server):

  ```console
  $ nix copy nixpkgs#hello --to 'gs://example-nix-cache?endpoint=http://localhost:4443'
  ```

)"
