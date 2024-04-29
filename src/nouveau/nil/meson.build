# Copyright © 2022 Collabora Ltd
# SPDX-License-Identifier: MIT

libnil_files = files(
  'nil_format.c',
  'nil_format.h',
  'nil_image.c',
  'nil_image.h',
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

_libnil = static_library(
  'nil',
  [libnil_files, nil_format_table],
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium],
  dependencies : libnil_deps,
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
)

idep_nil = declare_dependency(
  include_directories : include_directories('.'),
  link_with : _libnil,
)
