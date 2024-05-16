/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_descriptors.h"
#include "ac_surface.h"

#include "sid.h"

#include "util/u_math.h"

void
ac_build_sampler_descriptor(const enum amd_gfx_level gfx_level, const struct ac_sampler_state *state, uint32_t desc[4])
{
   const unsigned perf_mip = state->max_aniso_ratio ? state->max_aniso_ratio + 6 : 0;
   const bool compat_mode = gfx_level == GFX8 || gfx_level == GFX9;

   desc[0] = S_008F30_CLAMP_X(state->address_mode_u) |
             S_008F30_CLAMP_Y(state->address_mode_v) |
             S_008F30_CLAMP_Z(state->address_mode_w) |
             S_008F30_MAX_ANISO_RATIO(state->max_aniso_ratio) |
             S_008F30_DEPTH_COMPARE_FUNC(state->depth_compare_func) |
             S_008F30_FORCE_UNNORMALIZED(state->unnormalized_coords) |
             S_008F30_ANISO_THRESHOLD(state->max_aniso_ratio >> 1) |
             S_008F30_ANISO_BIAS(state->max_aniso_ratio) |
             S_008F30_DISABLE_CUBE_WRAP(!state->cube_wrap) |
             S_008F30_COMPAT_MODE(compat_mode) |
             S_008F30_TRUNC_COORD(state->trunc_coord) |
             S_008F30_FILTER_MODE(state->filter_mode);
   desc[1] = 0;
   desc[2] = S_008F38_XY_MAG_FILTER(state->mag_filter) |
             S_008F38_XY_MIN_FILTER(state->min_filter) |
             S_008F38_MIP_FILTER(state->mip_filter);
   desc[3] = S_008F3C_BORDER_COLOR_TYPE(state->border_color_type);

   if (gfx_level >= GFX12) {
      desc[1] |= S_008F34_MIN_LOD_GFX12(util_unsigned_fixed(CLAMP(state->min_lod, 0, 17), 8)) |
                 S_008F34_MAX_LOD_GFX12(util_unsigned_fixed(CLAMP(state->max_lod, 0, 17), 8));
      desc[2] |= S_008F38_PERF_MIP_LO(perf_mip);
      desc[3] |= S_008F3C_PERF_MIP_HI(perf_mip >> 2);
   } else {
      desc[1] |= S_008F34_MIN_LOD_GFX6(util_unsigned_fixed(CLAMP(state->min_lod, 0, 15), 8)) |
                 S_008F34_MAX_LOD_GFX6(util_unsigned_fixed(CLAMP(state->max_lod, 0, 15), 8)) |
                 S_008F34_PERF_MIP(perf_mip);
   }

   if (gfx_level >= GFX10) {
      desc[2] |= S_008F38_LOD_BIAS(util_signed_fixed(CLAMP(state->lod_bias, -32, 31), 8)) |
                 S_008F38_ANISO_OVERRIDE_GFX10(!state->aniso_single_level);
   } else {
      desc[2] |= S_008F38_LOD_BIAS(util_signed_fixed(CLAMP(state->lod_bias, -16, 16), 8)) |
                 S_008F38_DISABLE_LSB_CEIL(gfx_level <= GFX8) |
                 S_008F38_FILTER_PREC_FIX(1) |
                 S_008F38_ANISO_OVERRIDE_GFX8(gfx_level >= GFX8 && !state->aniso_single_level);
   }

   if (gfx_level >= GFX11) {
      desc[3] |= S_008F3C_BORDER_COLOR_PTR_GFX11(state->border_color_ptr);
   } else {
      desc[3] |= S_008F3C_BORDER_COLOR_PTR_GFX6(state->border_color_ptr);
   }
}

static void
ac_build_gfx6_fmask_descriptor(const enum amd_gfx_level gfx_level, const struct ac_fmask_state *state, uint32_t desc[8])
{
   const struct radeon_surf *surf = state->surf;
   const uint64_t va = state->va + surf->fmask_offset;
   uint32_t data_format, num_format;

#define FMASK(s, f) (((unsigned)(MAX2(1, s)) * 16) + (MAX2(1, f)))
   if (gfx_level == GFX9) {
      data_format = V_008F14_IMG_DATA_FORMAT_FMASK;
      switch (FMASK(state->num_samples, state->num_storage_samples)) {
      case FMASK(2, 1):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_8_2_1;
         break;
      case FMASK(2, 2):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_8_2_2;
         break;
      case FMASK(4, 1):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_8_4_1;
         break;
      case FMASK(4, 2):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_8_4_2;
         break;
      case FMASK(4, 4):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_8_4_4;
         break;
      case FMASK(8, 1):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_8_8_1;
         break;
      case FMASK(8, 2):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_16_8_2;
         break;
      case FMASK(8, 4):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_32_8_4;
         break;
      case FMASK(8, 8):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_32_8_8;
         break;
      case FMASK(16, 1):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_16_16_1;
         break;
      case FMASK(16, 2):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_32_16_2;
         break;
      case FMASK(16, 4):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_64_16_4;
         break;
      case FMASK(16, 8):
         num_format = V_008F14_IMG_NUM_FORMAT_FMASK_64_16_8;
         break;
      default:
         unreachable("invalid nr_samples");
      }
   } else {
      switch (FMASK(state->num_samples, state->num_storage_samples)) {
      case FMASK(2, 1):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK8_S2_F1;
         break;
      case FMASK(2, 2):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK8_S2_F2;
         break;
      case FMASK(4, 1):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK8_S4_F1;
         break;
      case FMASK(4, 2):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK8_S4_F2;
         break;
      case FMASK(4, 4):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK8_S4_F4;
         break;
      case FMASK(8, 1):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK8_S8_F1;
         break;
      case FMASK(8, 2):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK16_S8_F2;
         break;
      case FMASK(8, 4):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK32_S8_F4;
         break;
      case FMASK(8, 8):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK32_S8_F8;
         break;
      case FMASK(16, 1):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK16_S16_F1;
         break;
      case FMASK(16, 2):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK32_S16_F2;
         break;
      case FMASK(16, 4):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK64_S16_F4;
         break;
      case FMASK(16, 8):
         data_format = V_008F14_IMG_DATA_FORMAT_FMASK64_S16_F8;
         break;
      default:
         unreachable("invalid nr_samples");
      }
      num_format = V_008F14_IMG_NUM_FORMAT_UINT;
   }
#undef FMASK

   desc[0] = (va >> 8) | surf->fmask_tile_swizzle;
   desc[1] = S_008F14_BASE_ADDRESS_HI(va >> 40) |
             S_008F14_DATA_FORMAT(data_format) |
             S_008F14_NUM_FORMAT(num_format);
   desc[2] = S_008F18_WIDTH(state->width - 1) |
             S_008F18_HEIGHT(state->height - 1);
   desc[3] = S_008F1C_DST_SEL_X(V_008F1C_SQ_SEL_X) |
             S_008F1C_DST_SEL_Y(V_008F1C_SQ_SEL_X) |
             S_008F1C_DST_SEL_Z(V_008F1C_SQ_SEL_X) |
             S_008F1C_DST_SEL_W(V_008F1C_SQ_SEL_X) |
             S_008F1C_TYPE(state->type);
   desc[4] = 0;
   desc[5] = S_008F24_BASE_ARRAY(state->first_layer);
   desc[6] = 0;
   desc[7] = 0;

   if (gfx_level == GFX9) {
      desc[3] |= S_008F1C_SW_MODE(surf->u.gfx9.color.fmask_swizzle_mode);
      desc[4] |= S_008F20_DEPTH(state->last_layer) |
                 S_008F20_PITCH(surf->u.gfx9.color.fmask_epitch);
      desc[5] |= S_008F24_META_PIPE_ALIGNED(1) |
                 S_008F24_META_RB_ALIGNED(1);

      if (state->tc_compat_cmask) {
         const uint64_t cmask_va = state->va + surf->cmask_offset;

         desc[5] |= S_008F24_META_DATA_ADDRESS(cmask_va >> 40);
         desc[6] |= S_008F28_COMPRESSION_EN(1);
         desc[7] |= cmask_va >> 8;
      }
   } else {
      desc[3] |= S_008F1C_TILING_INDEX(surf->u.legacy.color.fmask.tiling_index);
      desc[4] |= S_008F20_DEPTH(state->depth - 1) |
                 S_008F20_PITCH(surf->u.legacy.color.fmask.pitch_in_pixels - 1);
      desc[5] |= S_008F24_LAST_ARRAY(state->last_layer);

      if (state->tc_compat_cmask) {
         const uint64_t cmask_va = state->va + surf->cmask_offset;

         desc[6] |= S_008F28_COMPRESSION_EN(1);
         desc[7] |= cmask_va >> 8;
      }
   }
}

static void
ac_build_gfx10_fmask_descriptor(const enum amd_gfx_level gfx_level, const struct ac_fmask_state *state, uint32_t desc[8])
{
   const struct radeon_surf *surf = state->surf;
   const uint64_t va = state->va + surf->fmask_offset;
   uint32_t format;

#define FMASK(s, f) (((unsigned)(MAX2(1, s)) * 16) + (MAX2(1, f)))
   switch (FMASK(state->num_samples, state->num_storage_samples)) {
   case FMASK(2, 1):
      format = V_008F0C_GFX10_FORMAT_FMASK8_S2_F1;
      break;
   case FMASK(2, 2):
      format = V_008F0C_GFX10_FORMAT_FMASK8_S2_F2;
      break;
   case FMASK(4, 1):
      format = V_008F0C_GFX10_FORMAT_FMASK8_S4_F1;
      break;
   case FMASK(4, 2):
      format = V_008F0C_GFX10_FORMAT_FMASK8_S4_F2;
      break;
   case FMASK(4, 4):
      format = V_008F0C_GFX10_FORMAT_FMASK8_S4_F4;
      break;
   case FMASK(8, 1):
      format = V_008F0C_GFX10_FORMAT_FMASK8_S8_F1;
      break;
   case FMASK(8, 2):
      format = V_008F0C_GFX10_FORMAT_FMASK16_S8_F2;
      break;
   case FMASK(8, 4):
      format = V_008F0C_GFX10_FORMAT_FMASK32_S8_F4;
      break;
   case FMASK(8, 8):
      format = V_008F0C_GFX10_FORMAT_FMASK32_S8_F8;
      break;
   case FMASK(16, 1):
      format = V_008F0C_GFX10_FORMAT_FMASK16_S16_F1;
      break;
   case FMASK(16, 2):
      format = V_008F0C_GFX10_FORMAT_FMASK32_S16_F2;
      break;
   case FMASK(16, 4):
      format = V_008F0C_GFX10_FORMAT_FMASK64_S16_F4;
      break;
   case FMASK(16, 8):
      format = V_008F0C_GFX10_FORMAT_FMASK64_S16_F8;
      break;
   default:
      unreachable("invalid nr_samples");
   }
#undef FMASK

   desc[0] = (va >> 8) | surf->fmask_tile_swizzle;
   desc[1] = S_00A004_BASE_ADDRESS_HI(va >> 40) |
             S_00A004_FORMAT_GFX10(format) |
             S_00A004_WIDTH_LO(state->width - 1);
   desc[2] = S_00A008_WIDTH_HI((state->width - 1) >> 2) |
             S_00A008_HEIGHT(state->height - 1) |
             S_00A008_RESOURCE_LEVEL(1);
   desc[3] = S_00A00C_DST_SEL_X(V_008F1C_SQ_SEL_X) |
             S_00A00C_DST_SEL_Y(V_008F1C_SQ_SEL_X) |
             S_00A00C_DST_SEL_Z(V_008F1C_SQ_SEL_X) |
             S_00A00C_DST_SEL_W(V_008F1C_SQ_SEL_X) |
             S_00A00C_SW_MODE(surf->u.gfx9.color.fmask_swizzle_mode) |
             S_00A00C_TYPE(state->type);
   desc[4] = S_00A010_DEPTH_GFX10(state->last_layer) | S_00A010_BASE_ARRAY(state->first_layer);
   desc[5] = 0;
   desc[6] = S_00A018_META_PIPE_ALIGNED(1);
   desc[7] = 0;

   if (state->tc_compat_cmask) {
      uint64_t cmask_va = state->va + surf->cmask_offset;

      desc[6] |= S_00A018_COMPRESSION_EN(1);
      desc[6] |= S_00A018_META_DATA_ADDRESS_LO(cmask_va >> 8);
      desc[7] |= cmask_va >> 16;
   }
}

void
ac_build_fmask_descriptor(const enum amd_gfx_level gfx_level, const struct ac_fmask_state *state, uint32_t desc[8])
{
   assert(gfx_level < GFX11);

   if (gfx_level >= GFX10) {
      ac_build_gfx10_fmask_descriptor(gfx_level, state, desc);
   } else {
      ac_build_gfx6_fmask_descriptor(gfx_level, state, desc);
   }
}
