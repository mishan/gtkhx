use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    let include_dir = out_dir
        .ancestors()
        .find(|p| p.ends_with("release") || p.ends_with("debug"))
        .map(|p| p.join("include"))
        .unwrap_or_else(|| out_dir.join("include"));

    std::fs::create_dir_all(&include_dir).unwrap();

    let config = cbindgen::Config::from_file(
        PathBuf::from(&crate_dir).join("cbindgen.toml"),
    )
    .expect("failed to read cbindgen.toml");

    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("failed to generate bindings")
        .write_to_file(include_dir.join("hxcrypto_hash.h"));
}
