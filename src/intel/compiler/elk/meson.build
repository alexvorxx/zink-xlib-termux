# Copyright © 2017 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

libintel_compiler_elk_files = files(
  'brw_cfg.cpp',
  'brw_cfg.h',
  'brw_clip.h',
  'brw_clip_line.c',
  'brw_clip_point.c',
  'brw_clip_tri.c',
  'brw_clip_unfilled.c',
  'brw_clip_util.c',
  'brw_compile_clip.c',
  'brw_compile_ff_gs.c',
  'brw_compile_sf.c',
  'brw_compiler.c',
  'brw_compiler.h',
  'brw_dead_control_flow.cpp',
  'brw_dead_control_flow.h',
  'brw_debug_recompile.c',
  'brw_disasm.c',
  'brw_disasm.h',
  'brw_disasm_info.c',
  'brw_disasm_info.h',
  'brw_eu.c',
  'brw_eu_compact.c',
  'brw_eu_defines.h',
  'brw_eu_emit.c',
  'brw_eu.h',
  'brw_eu_util.c',
  'brw_eu_validate.c',
  'brw_fs_bank_conflicts.cpp',
  'brw_fs_builder.h',
  'brw_fs_cmod_propagation.cpp',
  'brw_fs_combine_constants.cpp',
  'brw_fs_copy_propagation.cpp',
  'brw_fs.cpp',
  'brw_fs_cse.cpp',
  'brw_fs_dead_code_eliminate.cpp',
  'brw_fs_generator.cpp',
  'brw_fs.h',
  'brw_fs_live_variables.cpp',
  'brw_fs_live_variables.h',
  'brw_fs_lower_dpas.cpp',
  'brw_fs_lower_pack.cpp',
  'brw_fs_lower_regioning.cpp',
  'brw_fs_nir.cpp',
  'brw_fs_reg_allocate.cpp',
  'brw_fs_register_coalesce.cpp',
  'brw_fs_saturate_propagation.cpp',
  'brw_fs_scoreboard.cpp',
  'brw_fs_sel_peephole.cpp',
  'brw_fs_thread_payload.cpp',
  'brw_fs_validate.cpp',
  'brw_fs_visitor.cpp',
  'brw_inst.h',
  'brw_interpolation_map.c',
  'brw_ir.h',
  'brw_ir_allocator.h',
  'brw_ir_analysis.h',
  'brw_ir_fs.h',
  'brw_ir_performance.h',
  'brw_ir_performance.cpp',
  'brw_ir_vec4.h',
  'brw_isa_info.h',
  'brw_lower_logical_sends.cpp',
  'brw_mesh.cpp',
  'brw_nir.h',
  'brw_nir.c',
  'brw_nir_analyze_boolean_resolves.c',
  'brw_nir_analyze_ubo_ranges.c',
  'brw_nir_attribute_workarounds.c',
  'brw_nir_lower_cooperative_matrix.c',
  'brw_nir_lower_cs_intrinsics.c',
  'brw_nir_lower_alpha_to_coverage.c',
  'brw_nir_lower_intersection_shader.c',
  'brw_nir_lower_ray_queries.c',
  'brw_nir_lower_rt_intrinsics.c',
  'brw_nir_lower_shader_calls.c',
  'brw_nir_lower_storage_image.c',
  'brw_nir_rt.h',
  'brw_nir_rt.c',
  'brw_nir_rt_builder.h',
  'brw_packed_float.c',
  'brw_predicated_break.cpp',
  'brw_prim.h',
  'brw_private.h',
  'brw_reg.h',
  'brw_reg_type.c',
  'brw_reg_type.h',
  'brw_rt.h',
  'brw_schedule_instructions.cpp',
  'brw_shader.cpp',
  'brw_shader.h',
  'brw_simd_selection.cpp',
  'brw_vec4_builder.h',
  'brw_vec4_cmod_propagation.cpp',
  'brw_vec4_copy_propagation.cpp',
  'brw_vec4.cpp',
  'brw_vec4_cse.cpp',
  'brw_vec4_dead_code_eliminate.cpp',
  'brw_vec4_generator.cpp',
  'brw_vec4_gs_visitor.cpp',
  'brw_vec4_gs_visitor.h',
  'brw_vec4.h',
  'brw_vec4_live_variables.cpp',
  'brw_vec4_live_variables.h',
  'brw_vec4_nir.cpp',
  'brw_vec4_gs_nir.cpp',
  'brw_vec4_reg_allocate.cpp',
  'brw_vec4_surface_builder.cpp',
  'brw_vec4_surface_builder.h',
  'brw_vec4_tcs.cpp',
  'brw_vec4_tcs.h',
  'brw_vec4_tes.cpp',
  'brw_vec4_tes.h',
  'brw_vec4_visitor.cpp',
  'brw_vec4_vs_visitor.cpp',
  'brw_vec4_vs.h',
  'brw_vue_map.c',
  'gfx6_gs_visitor.cpp',
  'gfx6_gs_visitor.h',
)

elk_nir_trig = custom_target(
  'brw_nir_trig_workarounds.c',
  input : 'brw_nir_trig_workarounds.py',
  output : 'brw_nir_trig_workarounds.c',
  command : [
    prog_python, '@INPUT@', '-p', dir_compiler_nir,
  ],
  depend_files : nir_algebraic_depends,
  capture : true,
)

libintel_compiler_elk = static_library(
  'intel_compiler_elk',
  [libintel_compiler_elk_files, intel_nir_files, elk_nir_trig, ir_expression_operation_h],
  include_directories : [inc_include, inc_src, inc_intel],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  dependencies : [idep_nir_headers, idep_mesautil, idep_intel_dev],
  build_by_default : false,
)

if with_tests
  test(
    'intel_compiler_elk_tests',
    executable(
      'intel_compiler_elk_tests',
      files(
        'test_predicated_break.cpp',
        'test_eu_compact.cpp',
        'test_eu_validate.cpp',
        'test_fs_cmod_propagation.cpp',
        'test_fs_combine_constants.cpp',
        'test_fs_copy_propagation.cpp',
        'test_fs_saturate_propagation.cpp',
        'test_fs_scoreboard.cpp',
        'test_simd_selection.cpp',
        'test_vec4_cmod_propagation.cpp',
        'test_vec4_copy_propagation.cpp',
        'test_vec4_dead_code_eliminate.cpp',
        'test_vec4_register_coalesce.cpp',
        'test_vf_float_conversions.cpp',
      ),
      ir_expression_operation_h,
      include_directories : [inc_include, inc_src, inc_intel],
      link_with : [
        libintel_compiler_elk, libintel_common, libisl
      ],
      dependencies : [idep_gtest, idep_nir, idep_mesautil, idep_intel_dev],
    ),
    suite : ['intel'],
    protocol : 'gtest',
  )
endif

if with_intel_tools

elk_gram_tab = custom_target(
  'brw_gram.tab.[ch]',
  input : 'brw_gram.y',
  output : ['brw_gram.tab.c', 'brw_gram.tab.h'],
  command : bison_command
)

elk_lex_yy_c = custom_target(
  'brw_lex.yy.c',
  input : 'brw_lex.l',
  output : 'brw_lex.yy.c',
  command : [prog_flex, '-o', '@OUTPUT@', '@INPUT@']
)

elk_asm_tool = executable(
  'elk_asm',
  ['brw_asm_tool.c', elk_gram_tab[0], elk_gram_tab[1], elk_lex_yy_c],
  dependencies : [idep_mesautil, dep_thread, idep_intel_dev],
  include_directories : [inc_include, inc_src, inc_intel],
  link_with : [libintel_common, libintel_compiler_elk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  install : true
)

asm_testcases = [
  ['brw', 'gfx4'],
  ['g4x', 'gfx4.5'],
  ['ilk', 'gfx5'],
  ['snb', 'gfx6'],
  ['ivb', 'gfx7'],
  ['hsw', 'gfx7.5'],
  ['bdw', 'gfx8'],
  ['skl', 'gfx9'],
  ['icl', 'gfx11'],
  ['tgl', 'gfx12'],
  ['dg2', 'gfx12.5'],
]

test_runner = find_program('tests/run-test.py')
foreach testcase : asm_testcases
  _gen_name = testcase[0]
  _gen_num = testcase[1]
  _gen_folder = join_paths(meson.current_source_dir(), 'tests',
                           _gen_num.replace('gfx', 'gen'))
  test(
    'elk_asm_' + _gen_num, test_runner,
    args : [
      '--elk_asm', elk_asm_tool,
      '--gen_name', _gen_name,
      '--gen_folder', _gen_folder,
    ],
    suite : 'intel',
  )
endforeach

elk_disasm_tool = executable(
  'elk_disasm',
  files('brw_disasm_tool.c'),
  dependencies : [idep_mesautil, dep_thread, idep_intel_dev],
  include_directories : [inc_include, inc_src, inc_intel],
  link_with : [libintel_common, libintel_compiler_elk],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  install : true
)

endif

