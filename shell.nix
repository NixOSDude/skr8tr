{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "skr8tr-dev";

  buildInputs = with pkgs; [
    # C23 toolchain
    gcc
    gnumake
    cmake
    pkg-config

    # PQC — ML-DSA-65 identity (same as LambdaC)
    liboqs

    # Rust toolchain — CLI
    rustup
    cargo
    rustfmt
    clippy

    # Dev tools
    gdb
    valgrind
    clang-tools   # clangd LSP

    # Utilities
    curl
    jq
  ];

  shellHook = ''
    echo ""
    echo "  ╔══════════════════════════════════════════════╗"
    echo "  ║   Skr8tr — Sovereign Orchestrator            ║"
    echo "  ║   The k8s Killer                             ║"
    echo "  ╚══════════════════════════════════════════════╝"
    echo ""
    echo "  make              — build all C23 daemons"
    echo "  cd cli && cargo build --release"
    echo "                    — build skr8tr CLI"
    echo ""

    export PKG_CONFIG_PATH="${pkgs.liboqs.dev}/lib/pkgconfig:$PKG_CONFIG_PATH"
    export C_INCLUDE_PATH="${pkgs.liboqs.dev}/include:$C_INCLUDE_PATH"
    export LIBRARY_PATH="${pkgs.liboqs}/lib:$LIBRARY_PATH"
  '';
}
