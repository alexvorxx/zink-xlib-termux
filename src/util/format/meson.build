# Copyright © 2019 Google LLC
# SPDX-License-Identifier: MIT

files_mesa_format = files(
  'u_format.c',
  'u_format_bptc.c',
  'u_format_etc.c',
  'u_format_fxt1.c',
  'u_format_latc.c',
  'u_format_other.c',
  'u_format_rgtc.c',
  'u_format_s3tc.c',
  'u_format_tests.c',
  'u_format_unpack_neon.c',
  'u_format_yuv.c',
  'u_format_zs.c',
)

u_format_pack_h = custom_target(
  'u_format_pack.h',
  input : ['u_format_table.py', 'u_format.csv'],
  output : 'u_format_pack.h',
  command : [prog_python, '@INPUT@', '--header'],
  depend_files : files('u_format_pack.py', 'u_format_parse.py'),
  capture : true,
)

u_format_table_c = custom_target(
  'u_format_table.c',
  input : ['u_format_table.py', 'u_format.csv'],
  output : 'u_format_table.c',
  command : [prog_python, '@INPUT@'],
  depend_files : files('u_format_pack.py', 'u_format_parse.py'),
  capture : true,
)

files_mesa_format += [u_format_pack_h, u_format_table_c]
