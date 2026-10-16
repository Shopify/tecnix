{
  config,
  ...
}:

# The gs:// binary cache store against fake-gcs-server, a local GCS emulator:
# uploads through the JSON API, downloads through gs:// requests, both at the
# store's `endpoint`, and no Google credentials anywhere.

let
  pkgs = config.nodes.client.nixpkgs.pkgs;

  pkgA = pkgs.writeText "test-package-a" "test package a";
  pkgB = pkgs.writeText "test-package-b" (
    builtins.concatStringsSep "\n" (builtins.genList toString 4096)
  );

  port = 4443;
in
{
  name = "gcs-binary-cache-store";

  nodes = {
    server =
      { pkgs, ... }:
      {
        virtualisation.writableStore = true;
        virtualisation.additionalPaths = [
          pkgA
          pkgB
        ];
        nix.extraOptions = ''
          substituters =
        '';
        environment.systemPackages = [ pkgs.curl ];
        networking.firewall.allowedTCPPorts = [ port ];
        systemd.services.fake-gcs-server = {
          wantedBy = [ "multi-user.target" ];
          after = [ "network.target" ];
          serviceConfig.ExecStart = builtins.concatStringsSep " " [
            "${pkgs.fake-gcs-server}/bin/fake-gcs-server"
            "-scheme http"
            "-host 0.0.0.0"
            "-port ${toString port}"
            "-backend memory"
            "-public-host server:${toString port}"
          ];
        };
      };

    client =
      { ... }:
      {
        virtualisation.writableStore = true;
        nix.extraOptions = ''
          substituters =
        '';
      };
  };

  testScript =
    { nodes }:
    # python
    ''
      import json

      ENDPOINT = "http://server:${toString port}"
      PKGS = {"A": "${pkgA}", "B": "${pkgB}"}

      def store_url(bucket, **params):
          params.setdefault("endpoint", ENDPOINT)
          return f"gs://{bucket}?" + "&".join(f"{k}={v}" for k, v in params.items())

      def make_bucket(name):
          server.succeed(
              f"curl -sf -X POST -H 'Content-Type: application/json' "
              f"-d '{{\"name\": \"{name}\"}}' '{ENDPOINT}/storage/v1/b?project=test'"
          )

      def object_meta(bucket, name):
          return json.loads(server.succeed(
              f"curl -sf '{ENDPOINT}/storage/v1/b/{bucket}/o/{name.replace('/', '%2F')}'"
          ))

      start_all()
      server.wait_for_unit("fake-gcs-server.service")
      server.wait_for_open_port(${toString port})
      client.wait_for_unit("network-addresses-eth1.service")

      with subtest("upload, with the narinfo and listing compressed"):
          make_bucket("cache")
          server.succeed("nix-store --generate-binary-cache-key server /tmp/sk /tmp/pk")
          url = store_url("cache", compression="xz", **{"narinfo-compression": "br", "ls-compression": "gzip", "write-nar-listing": "true", "secret-key": "/tmp/sk"})
          server.succeed(f"nix copy --to '{url}' {PKGS['A']} {PKGS['B']}")

          h = PKGS["A"].split("/")[-1].split("-")[0]
          meta = object_meta("cache", f"{h}.narinfo")
          assert meta.get("contentEncoding") == "br", meta
          assert object_meta("cache", f"{h}.ls").get("contentEncoding") == "gzip"
          assert object_meta("cache", "nix-cache-info")["name"] == "nix-cache-info"

      with subtest("read back into another machine's store, signatures checked"):
          pk = server.succeed("cat /tmp/pk").strip()
          # The VMs see the host's store; drop the paths so they come from the cache.
          for pkg in PKGS.values():
              client.succeed(f"[ ! -e {pkg} ] || nix store delete --ignore-liveness {pkg}")
              client.succeed(f"[ ! -e {pkg} ]")
          client.succeed(
              f"nix copy --from '{store_url('cache')}' "
              f"--option trusted-public-keys '{pk}' {PKGS['A']} {PKGS['B']}"
          )
          client.succeed(f"grep -qx 'test package a' {PKGS['A']}")
          client.succeed(f"nix store verify --sigs-needed 1 --option trusted-public-keys '{pk}' {PKGS['B']}")

      with subtest("store info and lookups go to the endpoint"):
          info = json.loads(client.succeed(f"nix store info --json --store '{store_url('cache')}'"))
          assert info["url"].startswith("gs://cache"), info
          client.succeed(f"nix path-info --store '{store_url('cache')}' {PKGS['A']}")
          client.fail(f"nix path-info --store '{store_url('cache')}' /nix/store/00000000000000000000000000000000-missing")

      with subtest("a missing bucket is an error, not an empty cache"):
          out = client.fail(f"nix store info --store '{store_url('no-such-bucket')}' 2>&1")
          assert "no-such-bucket" in out, out
    '';
}
