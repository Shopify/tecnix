{
  config,
  ...
}:

# The gs:// binary cache store against fake-gcs-server, a local GCS emulator,
# with no Google credentials anywhere. Two things the unit tests can't cover:
# the real HTTP stack against an independent implementation of the API, and
# the daemon running the credential helper as the user it serves.

let
  pkgs = config.nodes.machine.nixpkgs.pkgs;

  pkgA = pkgs.writeText "test-package-a" "test package a";
  pkgB = pkgs.writeText "test-package-b" "test package b";

  port = 4443;
  endpoint = "http://localhost:${toString port}";

  # A signing key pair for this test only.
  secretKey = "gcs-test-1:Jj7z0dkxRxYrqVjHIxQYL7stSzydvBTWrRU2cFI84NIwskz4Y+C8blE89697wZ3q0+5Q8OaZMZUFXv/5jJDQng==";
  publicKey = "gcs-test-1:MLJM+GPgvG5RPPeve8Gd6tPuUPDmmTGVBV7/+YyQ0J4=";
in
{
  name = "gcs-binary-cache-store";

  nodes.machine =
    { pkgs, ... }:
    {
      virtualisation.writableStore = true;
      virtualisation.additionalPaths = [
        pkgA
        pkgB
      ];
      environment.systemPackages = [ pkgs.curl ];
      environment.etc."nix/gcs-test.sk".text = secretKey;
      users.users.alice.isNormalUser = true;

      systemd.services.fake-gcs-server = {
        wantedBy = [ "multi-user.target" ];
        after = [ "network.target" ];
        serviceConfig.ExecStart = builtins.concatStringsSep " " [
          "${pkgs.fake-gcs-server}/bin/fake-gcs-server"
          "-scheme http"
          "-host 127.0.0.1"
          "-port ${toString port}"
          "-backend memory"
          "-public-host localhost:${toString port}"
        ];
      };

      # The daemon's configuration: the cache, its key, and a credential
      # helper. The helper records which user it ran as, which is how the
      # test checks that the daemon ran it as its client rather than as root.
      nix.extraOptions = ''
        substituters = gs://cache?endpoint=${endpoint}
        trusted-public-keys = ${publicKey}
        gcs-credential-helper = /etc/nix/gcs-credential-helper
      '';
      environment.etc."nix/gcs-credential-helper" = {
        mode = "0755";
        text = ''
          #!/bin/sh
          # A Git credential helper: read the request, answer with a bearer token.
          [ "$1" = get ] || exit 2
          cat > /dev/null
          user=$(${pkgs.coreutils}/bin/id -un)
          echo "$user" >> "$HOME/gcs-credential-helper.calls"
          echo "authtype=Bearer"
          echo "credential=fake-token-for-$user"
          echo "password_expiry_utc=$(( $(${pkgs.coreutils}/bin/date +%s) + 3600 ))"
          echo
        '';
      };
    };

  testScript =
    { nodes }:
    # python
    ''
      import json

      ENDPOINT = "${endpoint}"
      PKG_A = "${pkgA}"
      PKG_B = "${pkgB}"

      def drop_from_store(pkg):
          machine.succeed(f"nix store delete --ignore-liveness {pkg}")
          machine.succeed(f"[ ! -e {pkg} ]")

      machine.wait_for_unit("fake-gcs-server.service")
      machine.wait_for_open_port(${toString port})
      machine.wait_for_unit("nix-daemon.socket")
      machine.succeed(
          f"curl -sf -X POST -H 'Content-Type: application/json' "
          f"-d '{{\"name\": \"cache\"}}' '{ENDPOINT}/storage/v1/b?project=test'"
      )

      with subtest("upload with a compressed narinfo, then read back with the signature checked"):
          machine.succeed(
              f"nix copy --to 'gs://cache?endpoint={ENDPOINT}&narinfo-compression=br&secret-key=/etc/nix/gcs-test.sk' "
              f"{PKG_A} {PKG_B}"
          )
          narinfo = PKG_A.split("/")[-1].split("-")[0] + ".narinfo"
          meta = json.loads(machine.succeed(f"curl -sf '{ENDPOINT}/storage/v1/b/cache/o/{narinfo}'"))
          assert meta.get("contentEncoding") == "br", meta

          drop_from_store(PKG_A)
          machine.succeed(f"nix copy --from 'gs://cache?endpoint={ENDPOINT}' {PKG_A}")
          machine.succeed(f"grep -qx 'test package a' {PKG_A}")
          machine.succeed(f"nix store verify --sigs-needed 1 {PKG_A}")

      with subtest("a missing bucket is an error, not an empty cache"):
          out = machine.fail(f"nix path-info --store 'gs://no-such-bucket?endpoint={ENDPOINT}' {PKG_B} 2>&1")
          assert "no-such-bucket" in out, out

      with subtest("the daemon substitutes for an unprivileged user, running the helper as that user"):
          drop_from_store(PKG_B)
          machine.succeed("rm -f /home/alice/gcs-credential-helper.calls")
          machine.succeed(f"su --login alice -c 'nix-store --realise {PKG_B}'")
          machine.succeed(f"grep -qx 'test package b' {PKG_B}")
          calls = machine.succeed("cat /home/alice/gcs-credential-helper.calls").split()
          assert calls and set(calls) == {"alice"}, calls
    '';
}
