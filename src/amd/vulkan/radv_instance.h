/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_INSTANCE_H
#define RADV_INSTANCE_H

#include "util/xmlconfig.h"

#include "vk_instance.h"

enum radv_trace_mode {
   /** Radeon GPU Profiler */
   RADV_TRACE_MODE_RGP = 1 << VK_TRACE_MODE_COUNT,

   /** Radeon Raytracing Analyzer */
   RADV_TRACE_MODE_RRA = 1 << (VK_TRACE_MODE_COUNT + 1),

   /** Gather context rolls of submitted command buffers */
   RADV_TRACE_MODE_CTX_ROLLS = 1 << (VK_TRACE_MODE_COUNT + 2),
};

struct radv_instance {
   struct vk_instance vk;

   VkAllocationCallbacks alloc;

   uint64_t debug_flags;
   uint64_t perftest_flags;

   struct {
      struct driOptionCache options;
      struct driOptionCache available_options;

      bool enable_mrt_output_nan_fixup;
      bool disable_tc_compat_htile_in_general;
      bool disable_shrink_image_store;
      bool disable_aniso_single_level;
      bool disable_trunc_coord;
      bool zero_vram;
      bool disable_sinking_load_input_fs;
      bool flush_before_query_copy;
      bool enable_unified_heap_on_apu;
      bool tex_non_uniform;
      bool ssbo_non_uniform;
      bool flush_before_timestamp_write;
      bool force_rt_wave64;
      bool dual_color_blend_by_location;
      bool legacy_sparse_binding;
      bool force_pstate_peak_gfx11_dgpu;
      bool clear_lds;
      bool enable_dgc;
      bool enable_khr_present_wait;
      bool report_llvm9_version_string;
      bool vk_require_etc2;
      bool vk_require_astc;
      char *app_layer;
      uint8_t override_graphics_shader_version;
      uint8_t override_compute_shader_version;
      uint8_t override_ray_tracing_shader_version;
      int override_vram_size;
      int override_uniform_offset_alignment;
   } drirc;
};

VK_DEFINE_HANDLE_CASTS(radv_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)

const char *radv_get_debug_option_name(int id);

const char *radv_get_perftest_option_name(int id);

#endif /* RADV_INSTANCE_H */
