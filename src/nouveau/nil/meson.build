# Copyright © 2022 Collabora Ltd
# SPDX-License-Identifier: MIT
if meson.version().version_compare('< 1.3.0')
  error('NVK requires meson 1.3.0 or newer')
endif

add_languages('rust', required: true)
rust = import('rust')

rc = meson.get_compiler('rust')
if rc.version().version_compare('< 1.73.0')
  error('nil_rs requires Rust 1.73.0')
endif

prog_cbindgen = find_program('cbindgen', required : false, native : true)

libnil_files = files(
  'nil.h',
  'nil_format.c',
  'nil_image.c',
  'nil_image_tic.c',
)

nil_format_table = custom_target(
  'nil_format_table',
  input : files('nil_format_table_gen.py', 'nil_formats.csv'),
  output : ['nil_format_table.h', 'nil_format_table.c'],
  command : [
    prog_python, '@INPUT0@', '--csv', '@INPUT1@',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@',
  ],
)

libnil_deps = [
  idep_mesautil,
  idep_nouveau_ws,
  idep_nvidia_headers,
]

_libnil_rs_files = files(
  'lib.rs',
)

_libnil_rs_deps = [
]

_libnil_rs_rust_args = [
  '-Aclippy::identity_op',
  '-Aclippy::len_zero',
  '-Aclippy::manual_range_contains',
  # normally this is a good one, but we use it where the "better" code is worse
  '-Aclippy::needless_range_loop',
  '-Aclippy::redundant_field_names',
  '-Aclippy::upper_case_acronyms',
  '-Aclippy::vec_box',
  '-Aclippy::write_with_newline',
  '-Anon_snake_case',
]

_nil_bindings_rs = rust.bindgen(
  input: ['nil_bindings.h'],
  output: 'nil_bindings.rs',
  c_args: [
    pre_args,
  ],
  include_directories : [inc_include, inc_src],
  args: [
    '--raw-line', '#![allow(non_camel_case_types)]',
    '--raw-line', '#![allow(non_snake_case)]',
    '--raw-line', '#![allow(non_upper_case_globals)]',
    '--allowlist-type', 'nv_device_info',
    '--allowlist-type', 'nv_device_type',
    '--allowlist-type', 'pipe_format',
    '--allowlist-type', 'pipe_swizzle',
    '--no-prepend-enum-name',
  ],
  dependencies: _libnil_rs_deps,
)

_libnil_rs_bindings = static_library(
  'nil_rs_bindings',
  _nil_bindings_rs,
  gnu_symbol_visibility: 'hidden',
  rust_abi: 'rust',
)

_libnil_rs_c_abi = static_library(
  'libnil_rs_c_abi',
  files('lib.rs'),
  gnu_symbol_visibility: 'hidden',
  rust_abi: 'c',
  rust_args: _libnil_rs_rust_args,
  link_with: [_libnil_rs_bindings],
  dependencies: _libnil_rs_deps,
)

_nil_rs_h = custom_target(
  'nil_rs_h',
  input : [files('cbindgen.toml'), _libnil_rs_files],
  output : ['nil_rs.h'],
  command : [
    prog_cbindgen, '-q', '--config', '@INPUT0@', '--lang', 'c',
    '--output', '@OUTPUT0@', '--', '@INPUT1@'
  ],
)

_libnil = static_library(
  'nil',
  [libnil_files, nil_format_table, _nil_rs_h],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium],
  dependencies : libnil_deps,
  c_args : [no_override_init_args],
  link_with : [_libnil_rs_c_abi],
  gnu_symbol_visibility : 'hidden',
)

idep_nil = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libnil,
)
