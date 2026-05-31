# rust/meson.build — Invoke Cargo to build the Rust workspace and produce
# static libraries that the C binary links against.
#
# Strategy: a custom_target runs `cargo build --release` with the target
# directory inside the Meson build dir. The output is the static library
# archive(s) that src/meson.build links into the gtkhx executable.
#
# The cbindgen-generated headers land at
#   <rust_target_dir>/release/include/*.h
# which src/meson.build picks up via compile_args.

cargo = find_program('cargo')

# Collect all .rs sources so Meson knows to re-run when they change.
rust_sources = files(
  'crates/hxrand/src/lib.rs',
  'crates/hxrand/build.rs',
  'crates/hxrand/cbindgen.toml',
  'crates/hxutil/src/lib.rs',
  'crates/hxcrypto-hash/src/lib.rs',
  'crates/hxcrypto-hash/build.rs',
  'crates/hxcrypto-hash/cbindgen.toml',
  'crates/hxcrypto-stream/src/lib.rs',
  'crates/hxcrypto-stream/src/blowfish_ofb.rs',
  'crates/hxcrypto-stream/build.rs',
  'crates/hxcrypto-stream/cbindgen.toml',
  'crates/hxcrypto-aead/src/lib.rs',
  'crates/hxcrypto-aead/build.rs',
  'crates/hxcrypto-aead/cbindgen.toml',
  'crates/hxcompress/src/lib.rs',
  'crates/hxcompress/build.rs',
  'crates/hxcompress/cbindgen.toml',
  'Cargo.toml',
  'crates/hxrand/Cargo.toml',
  'crates/hxutil/Cargo.toml',
  'crates/hxcrypto-hash/Cargo.toml',
  'crates/hxcrypto-stream/Cargo.toml',
  'crates/hxcrypto-aead/Cargo.toml',
  'crates/hxcompress/Cargo.toml',
)

rust_target_dir = meson.project_build_root() / 'rust-target'

# Build the Rust workspace. The custom_target output is a stamp file that
# Meson uses for dependency tracking; the actual artifacts are the .a
# archives and generated headers under rust_target_dir/release/.
rust_build = custom_target('rust-workspace',
  command : [
    'sh', '-c',
    '@0@ build --release --manifest-path @1@ --target-dir @2@ && touch @3@'.format(
      cargo.full_path(),
      meson.current_source_dir() / 'Cargo.toml',
      rust_target_dir,
      '@OUTPUT@',
    ),
  ],
  depend_files : rust_sources,
  output : 'rust-workspace.stamp',
  build_by_default : true,
  console : true,
)

# Common include path for all Rust-generated headers.
rust_include_args = ['-I' + rust_target_dir / 'release' / 'include']

# Create a dependency object for the Rust hxrand library.
rust_hxrand_dep = declare_dependency(
  sources : rust_build,
  compile_args : rust_include_args,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxrand',
  ],
)

# Crypto hash (replaces hmac.c)
rust_hxcrypto_hash_dep = declare_dependency(
  sources : rust_build,
  compile_args : rust_include_args,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcrypto_hash',
  ],
)

# Stream cipher (replaces cipher.c stream operations)
rust_hxcrypto_stream_dep = declare_dependency(
  sources : rust_build,
  compile_args : rust_include_args,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcrypto_stream',
  ],
)

# AEAD (replaces cipher_aead.c)
rust_hxcrypto_aead_dep = declare_dependency(
  sources : rust_build,
  compile_args : rust_include_args,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcrypto_aead',
  ],
)

# Compression (replaces compress.c)
rust_hxcompress_dep = declare_dependency(
  sources : rust_build,
  compile_args : rust_include_args,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcompress',
  ],
)
