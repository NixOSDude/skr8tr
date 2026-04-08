# REPO: enterprise
# SSoA LEVEL 3: MANIFEST SHARD
# FILE: enterprise-flake.nix
# MISSION: Reproducible enterprise build for Skr8tr.
#          Builds all OSS binaries PLUS the 4 enterprise modules:
#          skr8tr_rbac, skr8tr_sso, skr8tr_conductor_mt, skr8tr_autoscale
#          (compiled into skr8tr_sched via ENTERPRISE=1).
#
# IMPORTANT: This file must ONLY be pushed to gitea/skr8tr-enterprise.
#            Never push to github or gitea/skr8tr.
#
# Usage:
#   nix build .?subflakes=enterprise-flake.nix    # or symlink as flake.nix in enterprise repo
#   nix develop                                    # enterprise dev shell
#
# NixOS deployment (enterprise):
#   imports = [ skr8tr-enterprise.nixosModules.default ];
#   services.skr8tr-enterprise = {
#     enable    = true;
#     conductor = true;
#     node      = true;
#     tower     = true;
#     rbac      = true;
#     sso       = true;
#   };

{
  description = "Skr8tr — Sovereign Workload Orchestrator (Enterprise Edition)";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:

    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # ── shared C build inputs ─────────────────────────────────────────
        cNative = with pkgs; [ gcc gnumake pkg-config ];
        cLibs   = with pkgs; [ liboqs openssl nghttp2 ];

        # ── OSS daemons (identical to flake.nix) ─────────────────────────
        skr8tr-daemons-oss = pkgs.stdenv.mkDerivation {
          pname   = "skr8tr-daemons-oss";
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
        };

        # ── Enterprise daemons (ENTERPRISE=1) ────────────────────────────
        # Builds skr8tr_sched with audit+syslog+conductor-mt+autoscale baked in,
        # plus skr8tr_rbac and skr8tr_sso as standalone binaries.
        skr8tr-daemons-enterprise = pkgs.stdenv.mkDerivation {
          pname   = "skr8tr-daemons-enterprise";
          version = "0.1.0";
          src     = ./.;

          nativeBuildInputs = cNative;
          buildInputs       = cLibs;

          buildPhase = ''
            make ENTERPRISE=1 \
              OQS_INC=${pkgs.liboqs.dev}/include \
              OQS_LIB=${pkgs.liboqs}/lib
          '';

          installPhase = ''
            mkdir -p $out/bin
            # All OSS binaries
            for bin in skr8tr_node skr8tr_sched skr8tr_reg \
                       skr8tr_serve skr8tr_ingress skrtrkey skr8tr; do
              cp bin/$bin $out/bin/
            done
            # Enterprise-only binaries
            for bin in skr8tr_rbac skr8tr_sso; do
              cp bin/$bin $out/bin/
            done
          '';

          meta.description = "Skr8tr enterprise daemons: all OSS + rbac, sso, conductor-mt, autoscale";
        };

        # ── Rust CLI (same as OSS) ────────────────────────────────────────
        skr8tr-cli = pkgs.rustPlatform.buildRustPackage {
          pname   = "skr8tr-cli";
          version = "0.1.0";
          src     = ./cli;

          cargoLock.lockFile = ./cli/Cargo.lock;

          nativeBuildInputs = with pkgs; [ pkg-config ];
          buildInputs       = with pkgs; [ liboqs openssl ];
        };

        # ── Rust agent (same as OSS) ──────────────────────────────────────
        skr8tr-agent = pkgs.rustPlatform.buildRustPackage {
          pname   = "skr8tr-agent";
          version = "0.1.0";
          src     = ./agent;

          cargoLock.lockFile = ./agent/Cargo.lock;

          nativeBuildInputs = with pkgs; [ pkg-config cmake gcc ];
          buildInputs       = with pkgs; [ openssl onnxruntime ];

          ORT_STRATEGY   = "system";
          ORT_DYLIB_PATH = "${pkgs.onnxruntime}/lib/libonnxruntime.so";
        };

      in {
        packages = {
          skr8tr-daemons-oss        = skr8tr-daemons-oss;
          skr8tr-daemons-enterprise = skr8tr-daemons-enterprise;
          skr8tr-cli                = skr8tr-cli;
          skr8tr-agent              = skr8tr-agent;

          # `nix build` → full enterprise stack
          default = pkgs.symlinkJoin {
            name  = "skr8tr-enterprise";
            paths = [ skr8tr-daemons-enterprise skr8tr-cli skr8tr-agent ];
          };

          # OSS-only output from this flake (for parity testing)
          oss = pkgs.symlinkJoin {
            name  = "skr8tr-oss";
            paths = [ skr8tr-daemons-oss skr8tr-cli skr8tr-agent ];
          };
        };

        # ── enterprise dev shell ─────────────────────────────────────────
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            gcc
            gnumake
            pkg-config
            liboqs
            openssl
            gdb
            valgrind
            cargo
            rustc
            rust-analyzer
            clippy
            rustfmt
            onnxruntime
            cmake
            nghttp2
          ];

          ORT_STRATEGY   = "system";
          ORT_DYLIB_PATH = "${pkgs.onnxruntime}/lib/libonnxruntime.so";

          # ENTERPRISE=1 already set so `make` builds the full stack in devshell
          ENTERPRISE = "1";

          shellHook = ''
            echo "Skr8tr ENTERPRISE dev shell — gcc $(gcc --version | head -1)"
            echo "cargo $(cargo --version)"
            echo "ENTERPRISE=1 — make will build all enterprise modules"
            echo "ORT_DYLIB_PATH=$ORT_DYLIB_PATH"
          '';
        };
      }
    )

    // {
      nixosModules.default = { config, lib, pkgs, ... }:
        let
          cfg = config.services.skr8tr-enterprise;
          pkg = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        in
        with lib; {

          options.services.skr8tr-enterprise = {

            enable = mkEnableOption "Skr8tr Enterprise sovereign workload orchestrator";

            # ── OSS services (inherited) ────────────────────────────────
            conductor = mkOption { type = types.bool; default = false;
              description = "Start the enterprise Conductor (with multi-tenant + autoscale)."; };

            node = mkOption { type = types.bool; default = false;
              description = "Start the Fleet node daemon."; };

            gpu = mkOption { type = types.bool; default = false;
              description = "Enable GPU admin node mode (requires NVIDIA/CUDA)."; };

            tower = mkOption { type = types.bool; default = false;
              description = "Start the Tower service registry."; };

            serve = mkOption { type = types.bool; default = false;
              description = "Start the static file server."; };

            serveDir  = mkOption { type = types.str;  default = "/var/lib/skr8tr/www"; };
            servePort = mkOption { type = types.port; default = 7773; };

            ingress = mkOption { type = types.bool; default = false;
              description = "Start the HTTP ingress reverse proxy."; };

            ingressPort   = mkOption { type = types.port;           default = 80; };
            ingressRoutes = mkOption { type = types.listOf types.str; default = []; };
            tlsCert       = mkOption { type = types.nullOr types.path; default = null; };
            tlsKey        = mkOption { type = types.nullOr types.path; default = null; };
            conductorPubkey = mkOption { type = types.nullOr types.path; default = null; };

            # ── enterprise-only services ────────────────────────────────
            rbac = mkOption { type = types.bool; default = false;
              description = "Start the RBAC gateway (skr8tr_rbac) on UDP 7773."; };

            rbacPubkey = mkOption { type = types.nullOr types.path; default = null;
              description = "ML-DSA-65 admin public key for RBAC. Null = dev mode."; };

            sso = mkOption { type = types.bool; default = false;
              description = "Start the SSO/OIDC bridge (skr8tr_sso) on TCP 7780."; };

            ssoIssuer   = mkOption { type = types.str; default = "";
              description = "Expected OIDC issuer URL (e.g. https://accounts.google.com)."; };

            ssoAudience = mkOption { type = types.str; default = "";
              description = "Expected OIDC audience (client_id)."; };

            ssoJwksUrl  = mkOption { type = types.str; default = "";
              description = "JWKS endpoint URL for RS256 token verification."; };
          };

          config = mkIf cfg.enable {

            # Conductor (enterprise — multi-tenant + autoscale compiled in)
            systemd.services.skr8tr-conductor = mkIf cfg.conductor {
              description   = "Skr8tr Enterprise Conductor";
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

            # Fleet node
            systemd.services.skr8tr-node = mkIf cfg.node {
              description   = "Skr8tr Fleet Node Daemon";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart  = "${pkg}/bin/skr8tr_node${optionalString cfg.gpu " --gpu"}";
                Restart    = "on-failure";
                RestartSec = "2s";
              };
            };

            # Tower registry
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

            # Static file server
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

            # HTTP ingress
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

            # RBAC gateway (enterprise only)
            systemd.services.skr8tr-rbac = mkIf cfg.rbac {
              description   = "Skr8tr RBAC Gateway";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart  = "${pkg}/bin/skr8tr_rbac${
                  optionalString (cfg.rbacPubkey != null) " --pubkey ${cfg.rbacPubkey}"
                }";
                Restart    = "on-failure";
                RestartSec = "2s";
                DynamicUser = true;
              };
            };

            # SSO/OIDC bridge (enterprise only)
            systemd.services.skr8tr-sso = mkIf cfg.sso {
              description   = "Skr8tr SSO/OIDC Bridge";
              wantedBy      = [ "multi-user.target" ];
              after         = [ "network.target" ];
              serviceConfig = {
                ExecStart = concatStringsSep " " [
                  "${pkg}/bin/skr8tr_sso"
                  "--issuer"   cfg.ssoIssuer
                  "--audience" cfg.ssoAudience
                  "--jwks"     cfg.ssoJwksUrl
                ];
                Restart    = "on-failure";
                RestartSec = "2s";
                DynamicUser = true;
              };
            };

            # Open firewall ports
            networking.firewall.allowedUDPPorts = [ 7770 7771 7772 7773 ];
            networking.firewall.allowedTCPPorts = (
              [ 7773 7775 7780 9100 ]
              ++ optionals cfg.ingress [ cfg.ingressPort ]
              ++ optionals cfg.sso     [ 7780 ]
            );
          };
        };
    };
}
