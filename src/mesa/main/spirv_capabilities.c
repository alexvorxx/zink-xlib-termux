/*
 * Copyright 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * \file
 * \brief SPIRV-V capability handling.
 */

#include "spirv_capabilities.h"
#include "compiler/shader_info.h"

void
_mesa_fill_supported_spirv_capabilities(struct spirv_supported_capabilities *caps,
                                        struct gl_constants *consts,
                                        const struct gl_extensions *gl_exts)
{
   const struct spirv_supported_extensions *spirv_exts = consts->SpirVExtensions;

   *caps = (struct spirv_supported_capabilities) {
      .atomic_storage             = gl_exts->ARB_shader_atomic_counters,
      .demote_to_helper_invocation = gl_exts->EXT_demote_to_helper_invocation,
      .draw_parameters            =
         gl_exts->ARB_shader_draw_parameters &&
         spirv_exts->supported[SPV_KHR_shader_draw_parameters],
      .derivative_group           = gl_exts->NV_compute_shader_derivatives,
      .float64                    = gl_exts->ARB_gpu_shader_fp64,
      .geometry_streams           = gl_exts->ARB_gpu_shader5,
      .image_ms_array             = gl_exts->ARB_shader_image_load_store &&
                                               consts->MaxImageSamples > 1,
      .image_read_without_format  = gl_exts->EXT_shader_image_load_formatted,
      .image_write_without_format = gl_exts->ARB_shader_image_load_store,
      .int64                      = gl_exts->ARB_gpu_shader_int64,
      .int64_atomics              = gl_exts->NV_shader_atomic_int64,
      .post_depth_coverage        = gl_exts->ARB_post_depth_coverage,
      .shader_clock               = gl_exts->ARB_shader_clock,
      .shader_viewport_index_layer = gl_exts->ARB_shader_viewport_layer_array,
      .stencil_export             = gl_exts->ARB_shader_stencil_export,
      .storage_image_ms           = gl_exts->ARB_shader_image_load_store &&
                                               consts->MaxImageSamples > 1,
      .subgroup_ballot            =
         gl_exts->ARB_shader_ballot && spirv_exts->supported[SPV_KHR_shader_ballot],
      .subgroup_vote              =
         gl_exts->ARB_shader_group_vote && spirv_exts->supported[SPV_KHR_subgroup_vote],
      .tessellation               = gl_exts->ARB_tessellation_shader,
      .transform_feedback         = gl_exts->ARB_transform_feedback3,
      .variable_pointers          = spirv_exts->supported[SPV_KHR_variable_pointers],
      .integer_functions2         = gl_exts->INTEL_shader_integer_functions2,
   };
}
