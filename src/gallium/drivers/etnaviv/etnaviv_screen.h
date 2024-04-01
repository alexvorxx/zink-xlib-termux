/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#ifndef H_ETNAVIV_SCREEN
#define H_ETNAVIV_SCREEN

#include "etna_core_info.h"
#include "etnaviv_internal.h"
#include "etnaviv_perfmon.h"

#include "util/u_thread.h"
#include "pipe/p_screen.h"
#include "renderonly/renderonly.h"
#include "util/set.h"
#include "util/slab.h"
#include "util/u_dynarray.h"
#include "util/u_helpers.h"
#include "util/u_queue.h"
#include "compiler/nir/nir.h"
#include "hw/common.xml.h"

struct etna_bo;

/* Enum with indices for each of the feature words */
enum viv_features_word {
   viv_chipFeatures = 0,
   viv_chipMinorFeatures0 = 1,
   viv_chipMinorFeatures1 = 2,
   viv_chipMinorFeatures2 = 3,
   viv_chipMinorFeatures3 = 4,
   viv_chipMinorFeatures4 = 5,
   viv_chipMinorFeatures5 = 6,
   viv_chipMinorFeatures6 = 7,
   viv_chipMinorFeatures7 = 8,
   viv_chipMinorFeatures8 = 9,
   viv_chipMinorFeatures9 = 10,
   viv_chipMinorFeatures10 = 11,
   viv_chipMinorFeatures11 = 12,
   viv_chipMinorFeatures12 = 13,
   VIV_FEATURES_WORD_COUNT /* Must be last */
};

/** Convenience macro to probe features from state.xml.h:
 * _VIV_FEATURE(chipFeatures, FAST_CLEAR)
 * _VIV_FEATURE(chipMinorFeatures1, AUTO_DISABLE)
 */
#define _VIV_FEATURE(screen, word, feature) \
   ((screen->features[viv_ ## word] & (word ## _ ## feature)) != 0)

struct etna_screen {
   struct pipe_screen base;

   struct etna_device *dev;
   struct etna_gpu *gpu;
   struct etna_pipe *pipe;
   struct etna_perfmon *perfmon;
   struct renderonly *ro;

   struct util_dynarray supported_pm_queries;
   struct slab_parent_pool transfer_pool;

   struct etna_core_info *info;
   uint32_t features[VIV_FEATURES_WORD_COUNT];

   struct etna_specs specs;

   uint32_t drm_version;

   struct etna_compiler *compiler;
   struct util_queue shader_compiler_queue;

   /* dummy render target for GPUs that can't fully disable the color pipe */
   struct etna_reloc dummy_rt_reloc;

   /* dummy texture descriptor */
   struct etna_reloc dummy_desc_reloc;
};

static inline bool
VIV_FEATURE(const struct etna_screen *screen, enum etna_feature feature)
{
   switch (feature) {
   case ETNA_FEATURE_FAST_CLEAR:
      return _VIV_FEATURE(screen, chipFeatures, FAST_CLEAR);
   case ETNA_FEATURE_32_BIT_INDICES:
      return _VIV_FEATURE(screen, chipFeatures, 32_BIT_INDICES);
   case ETNA_FEATURE_MSAA:
      return _VIV_FEATURE(screen, chipFeatures, MSAA);
   case ETNA_FEATURE_DXT_TEXTURE_COMPRESSION:
      return _VIV_FEATURE(screen, chipFeatures, DXT_TEXTURE_COMPRESSION);
   case ETNA_FEATURE_ETC1_TEXTURE_COMPRESSION:
      return _VIV_FEATURE(screen, chipFeatures, ETC1_TEXTURE_COMPRESSION);
   case ETNA_FEATURE_NO_EARLY_Z:
      return _VIV_FEATURE(screen, chipFeatures, NO_EARLY_Z);

   case ETNA_FEATURE_MC20:
      return _VIV_FEATURE(screen, chipMinorFeatures0, MC20);
   case ETNA_FEATURE_RENDERTARGET_8K:
      return _VIV_FEATURE(screen, chipMinorFeatures0, RENDERTARGET_8K);
   case ETNA_FEATURE_TEXTURE_8K:
      return _VIV_FEATURE(screen, chipMinorFeatures0, TEXTURE_8K);
   case ETNA_FEATURE_HAS_SIGN_FLOOR_CEIL:
      return _VIV_FEATURE(screen, chipMinorFeatures0, HAS_SIGN_FLOOR_CEIL);
   case ETNA_FEATURE_HAS_SQRT_TRIG:
      return _VIV_FEATURE(screen, chipMinorFeatures0, HAS_SQRT_TRIG);
   case ETNA_FEATURE_2BITPERTILE:
      return _VIV_FEATURE(screen, chipMinorFeatures0, 2BITPERTILE);
   case ETNA_FEATURE_SUPER_TILED:
      return _VIV_FEATURE(screen, chipMinorFeatures0, SUPER_TILED);

   case ETNA_FEATURE_AUTO_DISABLE:
      return _VIV_FEATURE(screen, chipMinorFeatures1, AUTO_DISABLE);
   case ETNA_FEATURE_TEXTURE_HALIGN:
      return _VIV_FEATURE(screen, chipMinorFeatures1, TEXTURE_HALIGN);
   case ETNA_FEATURE_MMU_VERSION:
      return _VIV_FEATURE(screen, chipMinorFeatures1, MMU_VERSION);
   case ETNA_FEATURE_HALF_FLOAT:
      return _VIV_FEATURE(screen, chipMinorFeatures1, HALF_FLOAT);
   case ETNA_FEATURE_WIDE_LINE:
      return _VIV_FEATURE(screen, chipMinorFeatures1, WIDE_LINE);
   case ETNA_FEATURE_HALTI0:
      return _VIV_FEATURE(screen, chipMinorFeatures1, HALTI0);
   case ETNA_FEATURE_NON_POWER_OF_TWO:
      return _VIV_FEATURE(screen, chipMinorFeatures1, NON_POWER_OF_TWO);
   case ETNA_FEATURE_LINEAR_TEXTURE_SUPPORT:
      return _VIV_FEATURE(screen, chipMinorFeatures1, LINEAR_TEXTURE_SUPPORT);

   case ETNA_FEATURE_LINEAR_PE:
      return _VIV_FEATURE(screen, chipMinorFeatures2, LINEAR_PE);
   case ETNA_FEATURE_SUPERTILED_TEXTURE:
      return _VIV_FEATURE(screen, chipMinorFeatures2, SUPERTILED_TEXTURE);
   case ETNA_FEATURE_LOGIC_OP:
      return _VIV_FEATURE(screen, chipMinorFeatures2, LOGIC_OP);
   case ETNA_FEATURE_HALTI1:
      return _VIV_FEATURE(screen, chipMinorFeatures2, HALTI1);
   case ETNA_FEATURE_SEAMLESS_CUBE_MAP:
      return _VIV_FEATURE(screen, chipMinorFeatures2, SEAMLESS_CUBE_MAP);
   case ETNA_FEATURE_LINE_LOOP:
      return _VIV_FEATURE(screen, chipMinorFeatures2, LINE_LOOP);
   case ETNA_FEATURE_TEXTURE_TILED_READ:
      return _VIV_FEATURE(screen, chipMinorFeatures2, TEXTURE_TILED_READ);
   case ETNA_FEATURE_BUG_FIXES8:
      return _VIV_FEATURE(screen, chipMinorFeatures2, BUG_FIXES8);

   case ETNA_FEATURE_PE_DITHER_FIX:
      return _VIV_FEATURE(screen, chipMinorFeatures3, PE_DITHER_FIX);
   case ETNA_FEATURE_INSTRUCTION_CACHE:
      return _VIV_FEATURE(screen, chipMinorFeatures3, INSTRUCTION_CACHE);
   case ETNA_FEATURE_HAS_FAST_TRANSCENDENTALS:
      return _VIV_FEATURE(screen, chipMinorFeatures3, HAS_FAST_TRANSCENDENTALS);

   case ETNA_FEATURE_SMALL_MSAA:
      return _VIV_FEATURE(screen, chipMinorFeatures4, SMALL_MSAA);
   case ETNA_FEATURE_BUG_FIXES18:
      return _VIV_FEATURE(screen, chipMinorFeatures4, BUG_FIXES18);
   case ETNA_FEATURE_TEXTURE_ASTC:
      return _VIV_FEATURE(screen, chipMinorFeatures4, TEXTURE_ASTC);
   case ETNA_FEATURE_SINGLE_BUFFER:
      return _VIV_FEATURE(screen, chipMinorFeatures4, SINGLE_BUFFER);
   case ETNA_FEATURE_HALTI2:
      return _VIV_FEATURE(screen, chipMinorFeatures4, HALTI2);

   case ETNA_FEATURE_BLT_ENGINE:
      return _VIV_FEATURE(screen, chipMinorFeatures5, BLT_ENGINE);
   case ETNA_FEATURE_HALTI3:
      return _VIV_FEATURE(screen, chipMinorFeatures5, HALTI3);
   case ETNA_FEATURE_HALTI4:
      return _VIV_FEATURE(screen, chipMinorFeatures5, HALTI4);
   case ETNA_FEATURE_HALTI5:
      return _VIV_FEATURE(screen, chipMinorFeatures5, HALTI5);
   case ETNA_FEATURE_RA_WRITE_DEPTH:
      return _VIV_FEATURE(screen, chipMinorFeatures5, RA_WRITE_DEPTH);

   case ETNA_FEATURE_CACHE128B256BPERLINE:
      return _VIV_FEATURE(screen, chipMinorFeatures6, CACHE128B256BPERLINE);
   case ETNA_FEATURE_NEW_GPIPE:
      return _VIV_FEATURE(screen, chipMinorFeatures6, NEW_GPIPE);
   case ETNA_FEATURE_NO_ASTC:
      return _VIV_FEATURE(screen, chipMinorFeatures6, NO_ASTC);
   case ETNA_FEATURE_V4_COMPRESSION:
      return _VIV_FEATURE(screen, chipMinorFeatures6, V4_COMPRESSION);

   case ETNA_FEATURE_RS_NEW_BASEADDR:
      return _VIV_FEATURE(screen, chipMinorFeatures7, RS_NEW_BASEADDR);
   case ETNA_FEATURE_PE_NO_ALPHA_TEST:
      return _VIV_FEATURE(screen, chipMinorFeatures7, PE_NO_ALPHA_TEST);

   case ETNA_FEATURE_SH_NO_ONECONST_LIMIT:
      return _VIV_FEATURE(screen, chipMinorFeatures8, SH_NO_ONECONST_LIMIT);

   case ETNA_FEATURE_DEC400:
      return _VIV_FEATURE(screen, chipMinorFeatures10, DEC400);

   default:
      break;
   }

   unreachable("invalid feature enum value");
}

static inline struct etna_screen *
etna_screen(struct pipe_screen *pscreen)
{
   return (struct etna_screen *)pscreen;
}

struct etna_bo *
etna_screen_bo_from_handle(struct pipe_screen *pscreen,
                           struct winsys_handle *whandle);

struct pipe_screen *
etna_screen_create(struct etna_device *dev, struct etna_gpu *gpu,
                   struct renderonly *ro);

static inline size_t
etna_screen_get_tile_size(struct etna_screen *screen, uint8_t ts_mode,
                          bool is_msaa)
{
   if (!VIV_FEATURE(screen, ETNA_FEATURE_CACHE128B256BPERLINE)) {
      if (VIV_FEATURE(screen, ETNA_FEATURE_SMALL_MSAA) && is_msaa)
         return 256;
      return 64;
   }

   if (ts_mode == TS_MODE_256B)
      return 256;
   else
      return 128;
}

#endif
