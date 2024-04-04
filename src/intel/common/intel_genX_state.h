/*
 * Copyright (c) 2022 Intel Corporation
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

#ifndef INTEL_GENX_STATE_H
#define INTEL_GENX_STATE_H

#ifndef GFX_VERx10
#error This file should only be included by genX files.
#endif

#include <stdbool.h>

#include "dev/intel_device_info.h"
#include "genxml/gen_macros.h"

#ifdef __cplusplus
extern "C" {
#endif

#if GFX_VER >= 7

static inline void
intel_set_ps_dispatch_state(struct GENX(3DSTATE_PS) *ps,
                            const struct intel_device_info *devinfo,
                            const struct brw_wm_prog_data *prog_data,
                            unsigned rasterization_samples)
{
   assert(rasterization_samples != 0);

   bool enable_8  = prog_data->dispatch_8;
   bool enable_16 = prog_data->dispatch_16;
   bool enable_32 = prog_data->dispatch_32;

   if (prog_data->persample_dispatch) {
      /* TGL PRMs, Volume 2d: Command Reference: Structures:
       *    3DSTATE_PS_BODY::32 Pixel Dispatch Enable:
       *
       *    "Must not be enabled when dispatch rate is sample AND NUM_MULTISAMPLES > 1."
       */
      if (GFX_VER >= 12 && rasterization_samples > 1)
         enable_32 = false;

      /* Starting with SandyBridge (where we first get MSAA), the different
       * pixel dispatch combinations are grouped into classifications A
       * through F (SNB PRM Vol. 2 Part 1 Section 7.7.1).  On most hardware
       * generations, the only configurations supporting persample dispatch
       * are those in which only one dispatch width is enabled.
       *
       * The Gfx12 hardware spec has a similar dispatch grouping table, but
       * the following conflicting restriction applies (from the page on
       * "Structure_3DSTATE_PS_BODY"), so we need to keep the SIMD16 shader:
       *
       *  "SIMD32 may only be enabled if SIMD16 or (dual)SIMD8 is also
       *   enabled."
       */
      if (enable_32 || enable_16)
         enable_8 = false;
      if (GFX_VER < 12 && enable_32)
         enable_16 = false;
   }

   /* The docs for 3DSTATE_PS::32 Pixel Dispatch Enable say:
    *
    *    "When NUM_MULTISAMPLES = 16 or FORCE_SAMPLE_COUNT = 16,
    *     SIMD32 Dispatch must not be enabled for PER_PIXEL dispatch
    *     mode."
    *
    * 16x MSAA only exists on Gfx9+, so we can skip this on Gfx8.
    */
   if (GFX_VER >= 9 && rasterization_samples == 16 &&
       !prog_data->persample_dispatch) {
      assert(enable_8 || enable_16);
      enable_32 = false;
   }

   assert(enable_8 || enable_16 || enable_32);

   ps->_8PixelDispatchEnable = enable_8;
   ps->_16PixelDispatchEnable = enable_16;
   ps->_32PixelDispatchEnable = enable_32;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* INTEL_GENX_STATE_H */
