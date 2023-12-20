# Copyright © 2023 Intel Corporation
# SPDX-License-Identifier: MIT

intel_float64_spv_h = custom_target(
  'float64_spv.h',
  input : [glsl2spirv, float64_glsl_file],
  output : 'float64_spv.h',
  command : [
    prog_python, '@INPUT@', '@OUTPUT@',
    prog_glslang,
    '--create-entry', 'main',
    '--vn', 'float64_spv_source',
    '--glsl-version', '450',
    '-Olib',
  ]
)

intel_shader_files = files(
  'libintel_shaders.h',
  'generate.cl',
  'generate_draws.cl',
  'generate_draws_iris.cl',
  'memcpy.cl',
  'query_copy.cl',
)

prepended_input_args = []
foreach input_arg : intel_shader_files
  prepended_input_args += ['--in', input_arg]
endforeach

intel_shaders_gens = [ [ 80,   8],
                       [ 90,   9],
                       [110,  11],
                       [120,  12],
                       [125, 125],
                       [200,  20] ]
intel_shaders = []
foreach gen : intel_shaders_gens
  intel_shaders += custom_target(
    'intel_gfx@0@_shaders_code.h'.format(gen[1]),
    input : intel_shader_files,
    output : 'intel_gfx@0@_shaders_code.h'.format(gen[1]),
    command : [
      prog_intel_clc, '--nir',
      '--prefix', 'gfx@0@_intel_shaders'.format(gen[1]),
      prepended_input_args, '-o', '@OUTPUT@', '--',
      '-cl-std=cl2.0', '-D__OPENCL_VERSION__=200',
      '-DGFX_VERx10=@0@'.format(gen[0]),
      '-I' + join_paths(meson.current_source_dir(), '.'),
      '-I' + join_paths(meson.source_root(), 'src'),
      '-I' + join_paths(meson.source_root(), 'src/intel'),
      '-I' + join_paths(meson.build_root(), 'src/intel'),
      '-I' + join_paths(meson.source_root(), 'src/intel/genxml'),
      '-include', 'opencl-c.h',
    ],
    env: ['MESA_SHADER_CACHE_DISABLE=true'],
    depends : [dep_prog_intel_clc, gen_cl_xml_pack],
  )
endforeach

idep_intel_shaders = declare_dependency(
  sources : intel_shaders,
  include_directories : include_directories('.'),
)
