use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    // Walk up from OUT_DIR to the workspace target directory, then place
    // headers in a predictable location that Meson can reference.
    // OUT_DIR is something like: target/release/build/hxrand-<hash>/out
    // We want: target/release/include/hxrand.h
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
        .write_to_file(include_dir.join("hxrand.h"));
}
