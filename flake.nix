# REPO: oss
# SSoA LEVEL 3: MANIFEST SHARD
# FILE: flake.nix
# MISSION: Reproducible open-source build for Skr8tr.
#          Produces all OSS binaries, a dev shell with pinned toolchain,
#          and a NixOS module for one-line production deployment.
#
# Usage:
#   nix build                        # build all OSS binaries + CLI
#   nix develop                      # enter dev shell
#   nix build .#skr8tr-daemons       # build C23 daemons only
#   nix build .#skr8tr-cli           # build Rust CLI only
#
# NixOS deployment (in configuration.nix):
#   imports = [ skr8tr.nixosModules.default ];
#   services.skr8tr = {
#     enable    = true;
#     conductor = true;
#     node      = true;
#     tower     = true;
#   };

{
  description = "Skr8tr — Sovereign Container-Free Workload Orchestrator (OSS)";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:

    # ── per-system outputs (packages + devShell) ───────────────────────────
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # ── shared C build inputs ─────────────────────────────────────────
        cNative = with pkgs; [ gcc gnumake pkg-config ];
        cLibs   = with pkgs; [ liboqs openssl ];

        # ── OSS C daemons ─────────────────────────────────────────────────
        skr8tr-daemons = pkgs.stdenv.mkDerivation {
          pname   = "skr8tr-daemons";
          version = "0.1.0";
          src     = ./.;

          nativeBuildInputs = cNative;
          buildInputs       = cLibs;

          buildPhase = ''
            make \
              OQS_INC=${pkgs.liboqs.dev}/include \
              OQS_LIB=${pkgs.liboqs}/lib
          '';

          installPhase = ''
            mkdir -p $out/bin
            for bin in skr8tr_node skr8tr_sched skr8tr_reg \
                       skr8tr_serve skr8tr_ingress skrtrkey skr8tr; do
              cp bin/$bin $out/bin/
            done
          '';

          meta.description = "Skr8tr OSS daemons: node, sched, reg, serve, ingress, skrtrkey, CLI";
        };

        # ── Rust CLI (cli/) ───────────────────────────────────────────────
        skr8tr-cli = pkgs.rustPlatform.buildRustPackage {
          pname   = "skr8tr-cli";
          version = "0.1.0";
          src     = ./cli;

          cargoLock.lockFile = ./cli/Cargo.lock;

          nativeBuildInputs = with pkgs; [ pkg-config ];
          buildInputs       = with pkgs; [ liboqs openssl ];

          meta.description = "Skr8tr Rust CLI — up/down/status/nodes/rollout/logs";
        };

      in {
        # ── installable packages ──────────────────────────────────────────
        packages = {
          skr8tr-daemons = skr8tr-daemons;
          skr8tr-cli     = skr8tr-cli;

          # `nix build` with no target → daemons + CLI
          default = pkgs.symlinkJoin {
            name  = "skr8tr-oss";
            paths = [ skr8tr-daemons skr8tr-cli ];
          };
        };

        # ── dev shell ─────────────────────────────────────────────────────
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            # C23 toolchain
            gcc
            gnumake
            pkg-config
            liboqs
            openssl
            gdb
            valgrind

            # Rust toolchain
            cargo
            rustc
            rust-analyzer
            clippy
            rustfmt
          ];

          shellHook = ''
            echo "Skr8tr OSS dev shell — gcc $(gcc --version | head -1)"
            echo "cargo $(cargo --version)"
          '';
        };
      }
    )

    # ── system-independent outputs (NixOS module) ─────────────────────────
    // {
      nixosModules.default = { config, lib, pkgs, ... }:
        let
          cfg = config.services.skr8tr;
          pkg = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        in
        with lib; {

          options.services.skr8tr = {

            enable = mkEnableOption "Skr8tr sovereign workload orchestrator";

            conductor = mkOption {
              type    = types.bool;
              default = false;
              description = "Start the Conductor scheduler (skr8tr_sched).";
            };

            node = mkOption {
              type    = types.bool;
              default = false;
              description = "Start the Fleet node daemon (skr8tr_node).";
            };

            gpu = mkOption {
              type    = types.bool;
              default = false;
              description = "Enable GPU admin node mode (requires NVIDIA/CUDA).";
            };

            tower = mkOption {
              type    = types.bool;
              default = false;
              description = "Start the Tower service registry (skr8tr_reg).";
            };

            serve = mkOption {
              type    = types.bool;
              default = false;
              description = "Start the static file server (skr8tr_serve).";
            };

            serveDir = mkOption {
              type    = types.str;
              default = "/var/lib/skr8tr/www";
            };

            servePort = mkOption {
              type    = types.port;
              default = 7773;
            };

            ingress = mkOption {
              type    = types.bool;
              default = false;
              description = "Start the HTTP ingress reverse proxy (skr8tr_ingress).";
            };

            ingressPort = mkOption {
              type    = types.port;
              default = 80;
            };

            ingressRoutes = mkOption {
              type    = types.listOf types.str;
              default = [];
              example = [ "/api:api-service" "/:frontend" ];
              description = "Ingress route rules passed as --route flags.";
            };

            tlsCert = mkOption {
              type    = types.nullOr types.path;
              default = null;
              description = "Path to TLS certificate PEM (enables HTTPS).";
            };

            tlsKey = mkOption {
              type    = types.nullOr types.path;
              default = null;
              description = "Path to TLS private key PEM.";
            };

            conductorPubkey = mkOption {
              type    = types.nullOr types.path;
              default = null;
              description = "ML-DSA-65 public key for conductor auth. Null = dev mode.";
            };
          };

          config = mkIf cfg.enable {

            systemd.services.skr8tr-conductor = mkIf cfg.conductor {
              description   = "Skr8tr Conductor Scheduler";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart  = "${pkg}/bin/skr8tr_sched${
                  optionalString (cfg.conductorPubkey != null)
                    " --pubkey ${cfg.conductorPubkey}"
                }";
                Restart    = "on-failure";
                RestartSec = "2s";
                DynamicUser = true;
                AmbientCapabilities = "CAP_NET_BIND_SERVICE";
              };
            };

            systemd.services.skr8tr-node = mkIf cfg.node {
              description   = "Skr8tr Fleet Node Daemon";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart  = "${pkg}/bin/skr8tr_node${
                  optionalString cfg.gpu " --gpu"
                }";
                Restart    = "on-failure";
                RestartSec = "2s";
              };
            };

            systemd.services.skr8tr-tower = mkIf cfg.tower {
              description   = "Skr8tr Tower Service Registry";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart  = "${pkg}/bin/skr8tr_reg";
                Restart    = "on-failure";
                RestartSec = "2s";
                DynamicUser = true;
              };
            };

            systemd.services.skr8tr-serve = mkIf cfg.serve {
              description   = "Skr8tr Static File Server";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart  = "${pkg}/bin/skr8tr_serve --dir ${cfg.serveDir} --port ${toString cfg.servePort}";
                Restart    = "on-failure";
                RestartSec = "2s";
                DynamicUser = true;
              };
            };

            systemd.services.skr8tr-ingress = mkIf cfg.ingress {
              description   = "Skr8tr HTTP Ingress Proxy";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" "skr8tr-tower.service" ];
              serviceConfig = {
                ExecStart = concatStringsSep " " (
                  [ "${pkg}/bin/skr8tr_ingress" "--port" (toString cfg.ingressPort) ]
                  ++ map (r: "--route ${r}") cfg.ingressRoutes
                  ++ optionals (cfg.tlsCert != null) [ "--tls-cert" cfg.tlsCert "--tls-key" cfg.tlsKey ]
                );
                Restart    = "on-failure";
                RestartSec = "2s";
                DynamicUser = true;
                AmbientCapabilities = "CAP_NET_BIND_SERVICE";
              };
            };

            networking.firewall.allowedUDPPorts = mkIf cfg.enable [ 7770 7771 7772 ];
            networking.firewall.allowedTCPPorts = mkIf cfg.enable (
              [ 7773 7775 7780 9100 ]
              ++ optionals cfg.ingress [ cfg.ingressPort ]
            );
          };
        };
    };
}
