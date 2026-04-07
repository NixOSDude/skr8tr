// build.rs — Skr8tr CLI
// Links against system liboqs for ML-DSA-65 command signing.
// Resolution order:
//   1. OQS_LIBDIR environment variable (explicit override)
//   2. pkg-config liboqs
//   3. Nix store auto-detection (find /nix/store -name liboqs.so)

fn main() {
    println!("cargo:rustc-link-lib=oqs");

    // 1. Explicit env var
    if let Ok(dir) = std::env::var("OQS_LIBDIR") {
        println!("cargo:rustc-link-search=native={}", dir.trim());
        return;
    }

    // 2. pkg-config
    if let Ok(out) = std::process::Command::new("pkg-config")
        .args(["--variable=libdir", "liboqs"])
        .output()
    {
        if out.status.success() {
            let dir = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if !dir.is_empty() {
                println!("cargo:rustc-link-search=native={}", dir);
                return;
            }
        }
    }

    // 3. Nix store
    if let Ok(out) = std::process::Command::new("sh")
        .args([
            "-c",
            "find /nix/store -name 'liboqs.so' 2>/dev/null | head -1 | xargs dirname 2>/dev/null",
        ])
        .output()
    {
        if out.status.success() {
            let dir = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if !dir.is_empty() {
                println!("cargo:rustc-link-search=native={}", dir);
            }
        }
    }
}
