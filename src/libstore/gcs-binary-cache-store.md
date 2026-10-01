R"(

**Store URL format**: `gs://`*bucket-name*

This store allows reading and writing a binary cache stored in a Google Cloud Storage (GCS) bucket.
This store shares many idioms with the [HTTP Binary Cache Store](@docroot@/store/types/http-binary-cache-store.md).

For a bucket named `example-nix-cache`, the binary cache URL is <gs://example-nix-cache>.

### Authentication

Nix uses [Application Default Credentials (ADC)](https://cloud.google.com/docs/authentication/application-default-credentials)
for authenticating requests to Google Cloud Storage:

1. The `GOOGLE_APPLICATION_CREDENTIALS` environment variable pointing to a service account key file
2. User credentials from `gcloud auth application-default login` (at `~/.config/gcloud/application_default_credentials.json`)
3. The GCE metadata server (automatic on Compute Engine, GKE, Cloud Run, etc.)

For read operations, Nix requests the `devstorage.read_only` OAuth2 scope.
For write operations (uploads), Nix requests the `devstorage.read_write` scope.

### Anonymous reads

If your bucket is publicly accessible and does not require authentication,
you can use the [HTTP Binary Cache Store] with
`https://storage.googleapis.com/example-nix-cache` instead of `gs://example-nix-cache`.

### Examples

- To use a GCS bucket as a substituter:

  ```console
  $ nix build --substituters 'gs://example-nix-cache' --no-require-sigs ...
  ```

- To upload to a GCS binary cache:

  ```console
  $ nix copy nixpkgs.hello --to 'gs://example-nix-cache'
  ```

- To specify a storage class for uploads:

  ```console
  $ nix copy nixpkgs.hello --to 'gs://example-nix-cache?storage-class=NEARLINE'
  ```

)"
