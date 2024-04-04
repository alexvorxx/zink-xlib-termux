/*
 * Copyright © 2021 Intel Corporation
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

#include "brw_private.h"
#include "compiler/shader_info.h"
#include "intel/dev/intel_debug.h"
#include "intel/dev/intel_device_info.h"
#include "util/ralloc.h"

unsigned
brw_required_dispatch_width(const struct shader_info *info)
{
   if ((int)info->subgroup_size >= (int)SUBGROUP_SIZE_REQUIRE_8) {
      assert(gl_shader_stage_uses_workgroup(info->stage));
      /* These enum values are expressly chosen to be equal to the subgroup
       * size that they require.
       */
      return (unsigned)info->subgroup_size;
   } else {
      return 0;
   }
}

static inline bool
test_bit(unsigned mask, unsigned bit) {
   return mask & (1u << bit);
}

namespace {

struct brw_cs_prog_data *
get_cs_prog_data(brw_simd_selection_state &state)
{
   if (std::holds_alternative<struct brw_cs_prog_data *>(state.prog_data))
      return std::get<struct brw_cs_prog_data *>(state.prog_data);
   else
      return nullptr;
}

}

bool
brw_simd_should_compile(brw_simd_selection_state &state, unsigned simd)
{
   assert(simd < SIMD_COUNT);
   assert(!state.compiled[simd]);

   const auto cs_prog_data = get_cs_prog_data(state);
   const unsigned width = 8u << simd;

   /* For shaders with variable size workgroup, in most cases we can compile
    * all the variants (exceptions are bindless dispatch & ray queries), since
    * the choice will happen only at dispatch time.
    */
   const bool workgroup_size_variable = cs_prog_data && cs_prog_data->local_size[0] == 0;

   if (!workgroup_size_variable) {
      if (state.spilled[simd]) {
         state.error[simd] = ralloc_asprintf(
            state.mem_ctx, "SIMD%u skipped because would spill", width);
         return false;
      }

      if (state.required_width && state.required_width != width) {
         state.error[simd] = ralloc_asprintf(
            state.mem_ctx, "SIMD%u skipped because required dispatch width is %u",
            width, state.required_width);
         return false;
      }

      if (cs_prog_data) {
         const unsigned workgroup_size = cs_prog_data->local_size[0] *
                                         cs_prog_data->local_size[1] *
                                         cs_prog_data->local_size[2];

         unsigned max_threads = state.devinfo->max_cs_workgroup_threads;

         if (simd > 0 && state.compiled[simd - 1] &&
            workgroup_size <= (width / 2)) {
            state.error[simd] = ralloc_asprintf(
               state.mem_ctx, "SIMD%u skipped because workgroup size %u already fits in SIMD%u",
               width, workgroup_size, width / 2);
            return false;
         }

         if (DIV_ROUND_UP(workgroup_size, width) > max_threads) {
            state.error[simd] = ralloc_asprintf(
               state.mem_ctx, "SIMD%u can't fit all %u invocations in %u threads",
               width, workgroup_size, max_threads);
            return false;
         }
      }

      /* The SIMD32 is only enabled for cases it is needed unless forced.
       *
       * TODO: Use performance_analysis and drop this rule.
       */
      if (width == 32) {
         if (!INTEL_DEBUG(DEBUG_DO32) && (state.compiled[0] || state.compiled[1])) {
            state.error[simd] = ralloc_strdup(
               state.mem_ctx, "SIMD32 skipped because not required");
            return false;
         }
      }
   }

   if (width == 32 && cs_prog_data && cs_prog_data->base.ray_queries > 0) {
      state.error[simd] = ralloc_asprintf(
         state.mem_ctx, "SIMD%u skipped because of ray queries",
         width);
      return false;
   }

   if (width == 32 && cs_prog_data && cs_prog_data->uses_btd_stack_ids) {
      state.error[simd] = ralloc_asprintf(
         state.mem_ctx, "SIMD%u skipped because of bindless shader calls",
         width);
      return false;
   }

   static const bool env_skip[] = {
      INTEL_DEBUG(DEBUG_NO8) != 0,
      INTEL_DEBUG(DEBUG_NO16) != 0,
      INTEL_DEBUG(DEBUG_NO32) != 0,
   };

   static_assert(ARRAY_SIZE(env_skip) == SIMD_COUNT);

   if (unlikely(env_skip[simd])) {
      state.error[simd] = ralloc_asprintf(
         state.mem_ctx, "SIMD%u skipped because INTEL_DEBUG=no%u",
         width, width);
      return false;
   }

   return true;
}

void
brw_simd_mark_compiled(brw_simd_selection_state &state, unsigned simd, bool spilled)
{
   assert(simd < SIMD_COUNT);
   assert(!state.compiled[simd]);

   auto cs_prog_data = get_cs_prog_data(state);

   state.compiled[simd] = true;
   if (cs_prog_data)
      cs_prog_data->prog_mask |= 1u << simd;

   /* If a SIMD spilled, all the larger ones would spill too. */
   if (spilled) {
      for (unsigned i = simd; i < SIMD_COUNT; i++) {
         state.spilled[i] = true;
         if (cs_prog_data)
            cs_prog_data->prog_spilled |= 1u << i;
      }
   }
}

int
brw_simd_select(const struct brw_simd_selection_state &state)
{
   for (int i = SIMD_COUNT - 1; i >= 0; i--) {
      if (state.compiled[i] && !state.spilled[i])
         return i;
   }
   for (int i = SIMD_COUNT - 1; i >= 0; i--) {
      if (state.compiled[i])
         return i;
   }
   return -1;
}

int
brw_simd_select_for_workgroup_size(const struct intel_device_info *devinfo,
                                   const struct brw_cs_prog_data *prog_data,
                                   const unsigned *sizes)
{
   if (!sizes || (prog_data->local_size[0] == sizes[0] &&
                  prog_data->local_size[1] == sizes[1] &&
                  prog_data->local_size[2] == sizes[2])) {
      brw_simd_selection_state simd_state{
         .prog_data = const_cast<struct brw_cs_prog_data *>(prog_data),
      };

      /* Propagate the prog_data information back to the simd_state,
       * so we can use select() directly.
       */
      for (int i = 0; i < SIMD_COUNT; i++) {
         simd_state.compiled[i] = test_bit(prog_data->prog_mask, i);
         simd_state.spilled[i] = test_bit(prog_data->prog_spilled, i);
      }

      return brw_simd_select(simd_state);
   }

   struct brw_cs_prog_data cloned = *prog_data;
   for (unsigned i = 0; i < 3; i++)
      cloned.local_size[i] = sizes[i];

   cloned.prog_mask = 0;
   cloned.prog_spilled = 0;

   void *mem_ctx = ralloc_context(NULL);

   brw_simd_selection_state simd_state{
      .mem_ctx = mem_ctx,
      .devinfo = devinfo,
      .prog_data = &cloned,
   };

   for (unsigned simd = 0; simd < SIMD_COUNT; simd++) {
      /* We are not recompiling, so use original results of prog_mask and
       * prog_spilled as they will already contain all possible compilations.
       */
      if (brw_simd_should_compile(simd_state, simd) &&
          test_bit(prog_data->prog_mask, simd)) {
         brw_simd_mark_compiled(simd_state, simd, test_bit(prog_data->prog_spilled, simd));
      }
   }

   ralloc_free(mem_ctx);

   return brw_simd_select(simd_state);
}
