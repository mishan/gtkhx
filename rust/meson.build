# rust/meson.build — Invoke Cargo to build the Rust workspace and produce
# static libraries that the C binary links against.
#
# Strategy: a custom_target runs `cargo build --release` with the target
# directory inside the Meson build dir. The output is the static library
# archive(s) that src/meson.build links into the gtkhx executable.
#
# The cbindgen-generated header (hxrand.h) lands at
#   <rust_target_dir>/release/include/hxrand.h
# which src/meson.build picks up via an include_directories() declaration.

cargo = find_program('cargo')

# Collect all .rs sources so Meson knows to re-run when they change.
rust_sources = files(
  'crates/hxrand/src/lib.rs',
  'crates/hxrand/build.rs',
  'crates/hxrand/cbindgen.toml',
  'crates/hxutil/src/lib.rs',
  'Cargo.toml',
  'crates/hxrand/Cargo.toml',
  'crates/hxutil/Cargo.toml',
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

# The cbindgen-generated headers live here after cargo build.
rust_inc = include_directories(rust_target_dir / 'release' / 'include')

# Create a dependency object for the Rust hxrand library.
rust_hxrand_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxrand',
  ],
  include_directories : rust_inc,
)
