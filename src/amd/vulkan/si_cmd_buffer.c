/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based on si_state.c
 * Copyright © 2015 Advanced Micro Devices, Inc.
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

/* command buffer handling for AMD GCN */

#include "radv_buffer.h"
#include "radv_cs.h"
#include "radv_debug.h"
#include "radv_private.h"
#include "radv_shader.h"
#include "radv_sqtt.h"
#include "sid.h"

static void
radv_write_harvested_raster_configs(struct radv_physical_device *pdev, struct radeon_cmdbuf *cs, unsigned raster_config,
                                    unsigned raster_config_1)
{
   unsigned num_se = MAX2(pdev->info.max_se, 1);
   unsigned raster_config_se[4];
   unsigned se;

   ac_get_harvested_configs(&pdev->info, raster_config, &raster_config_1, raster_config_se);

   for (se = 0; se < num_se; se++) {
      /* GRBM_GFX_INDEX has a different offset on GFX6 and GFX7+ */
      if (pdev->info.gfx_level < GFX7)
         radeon_set_config_reg(
            cs, R_00802C_GRBM_GFX_INDEX,
            S_00802C_SE_INDEX(se) | S_00802C_SH_BROADCAST_WRITES(1) | S_00802C_INSTANCE_BROADCAST_WRITES(1));
      else
         radeon_set_uconfig_reg(
            cs, R_030800_GRBM_GFX_INDEX,
            S_030800_SE_INDEX(se) | S_030800_SH_BROADCAST_WRITES(1) | S_030800_INSTANCE_BROADCAST_WRITES(1));
      radeon_set_context_reg(cs, R_028350_PA_SC_RASTER_CONFIG, raster_config_se[se]);
   }

   /* GRBM_GFX_INDEX has a different offset on GFX6 and GFX7+ */
   if (pdev->info.gfx_level < GFX7)
      radeon_set_config_reg(
         cs, R_00802C_GRBM_GFX_INDEX,
         S_00802C_SE_BROADCAST_WRITES(1) | S_00802C_SH_BROADCAST_WRITES(1) | S_00802C_INSTANCE_BROADCAST_WRITES(1));
   else
      radeon_set_uconfig_reg(
         cs, R_030800_GRBM_GFX_INDEX,
         S_030800_SE_BROADCAST_WRITES(1) | S_030800_SH_BROADCAST_WRITES(1) | S_030800_INSTANCE_BROADCAST_WRITES(1));

   if (pdev->info.gfx_level >= GFX7)
      radeon_set_context_reg(cs, R_028354_PA_SC_RASTER_CONFIG_1, raster_config_1);
}

void
radv_emit_compute(struct radv_device *device, struct radeon_cmdbuf *cs)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;

   radeon_set_sh_reg_seq(cs, R_00B810_COMPUTE_START_X, 3);
   radeon_emit(cs, 0);
   radeon_emit(cs, 0);
   radeon_emit(cs, 0);

   radeon_set_sh_reg(cs, R_00B834_COMPUTE_PGM_HI, S_00B834_DATA(pdev->info.address32_hi >> 8));

   radeon_set_sh_reg_seq(cs, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0, 2);
   /* R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0 / SE1,
    * renamed COMPUTE_DESTINATION_EN_SEn on gfx10. */
   for (unsigned i = 0; i < 2; ++i) {
      unsigned cu_mask = i < gpu_info->num_se ? gpu_info->spi_cu_en : 0x0;
      radeon_emit(cs, S_00B8AC_SA0_CU_EN(cu_mask) | S_00B8AC_SA1_CU_EN(cu_mask));
   }

   if (pdev->info.gfx_level >= GFX7) {
      /* Also set R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE2 / SE3 */
      radeon_set_sh_reg_seq(cs, R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2, 2);
      for (unsigned i = 2; i < 4; ++i) {
         unsigned cu_mask = i < gpu_info->num_se ? gpu_info->spi_cu_en : 0x0;
         radeon_emit(cs, S_00B8AC_SA0_CU_EN(cu_mask) | S_00B8AC_SA1_CU_EN(cu_mask));
      }

      if (device->border_color_data.bo) {
         uint64_t bc_va = radv_buffer_get_va(device->border_color_data.bo);

         radeon_set_uconfig_reg_seq(cs, R_030E00_TA_CS_BC_BASE_ADDR, 2);
         radeon_emit(cs, bc_va >> 8);
         radeon_emit(cs, S_030E04_ADDRESS(bc_va >> 40));
      }
   }

   if (pdev->info.gfx_level >= GFX9 && pdev->info.gfx_level < GFX11) {
      radeon_set_uconfig_reg(cs, R_0301EC_CP_COHER_START_DELAY, pdev->info.gfx_level >= GFX10 ? 0x20 : 0);
   }

   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_sh_reg_seq(cs, R_00B890_COMPUTE_USER_ACCUM_0, 4);
      radeon_emit(cs, 0); /* R_00B890_COMPUTE_USER_ACCUM_0 */
      radeon_emit(cs, 0); /* R_00B894_COMPUTE_USER_ACCUM_1 */
      radeon_emit(cs, 0); /* R_00B898_COMPUTE_USER_ACCUM_2 */
      radeon_emit(cs, 0); /* R_00B89C_COMPUTE_USER_ACCUM_3 */

      radeon_set_sh_reg(cs, R_00B9F4_COMPUTE_DISPATCH_TUNNEL, 0);
   }

   if (pdev->info.gfx_level == GFX6) {
      if (device->border_color_data.bo) {
         uint64_t bc_va = radv_buffer_get_va(device->border_color_data.bo);
         radeon_set_config_reg(cs, R_00950C_TA_CS_BC_BASE_ADDR, bc_va >> 8);
      }
   }

   if (device->tma_bo) {
      uint64_t tba_va, tma_va;

      assert(pdev->info.gfx_level == GFX8);

      tba_va = radv_shader_get_va(device->trap_handler_shader);
      tma_va = radv_buffer_get_va(device->tma_bo);

      radeon_set_sh_reg_seq(cs, R_00B838_COMPUTE_TBA_LO, 4);
      radeon_emit(cs, tba_va >> 8);
      radeon_emit(cs, tba_va >> 40);
      radeon_emit(cs, tma_va >> 8);
      radeon_emit(cs, tma_va >> 40);
   }

   if (pdev->info.gfx_level >= GFX11) {
      radeon_set_sh_reg_seq(cs, R_00B8AC_COMPUTE_STATIC_THREAD_MGMT_SE4, 4);
      /* SE4-SE7 */
      for (unsigned i = 4; i < 8; ++i) {
         unsigned cu_mask = i < gpu_info->num_se ? gpu_info->spi_cu_en : 0x0;
         radeon_emit(cs, S_00B8AC_SA0_CU_EN(cu_mask) | S_00B8AC_SA1_CU_EN(cu_mask));
      }

      radeon_set_sh_reg(cs, R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE, 64);
   }
}

/* 12.4 fixed-point */
static unsigned
radv_pack_float_12p4(float x)
{
   return x <= 0 ? 0 : x >= 4096 ? 0xffff : x * 16;
}

static void
radv_set_raster_config(struct radv_physical_device *pdev, struct radeon_cmdbuf *cs)
{
   unsigned num_rb = MIN2(pdev->info.max_render_backends, 16);
   uint64_t rb_mask = pdev->info.enabled_rb_mask;
   unsigned raster_config, raster_config_1;

   ac_get_raster_config(&pdev->info, &raster_config, &raster_config_1, NULL);

   /* Always use the default config when all backends are enabled
    * (or when we failed to determine the enabled backends).
    */
   if (!rb_mask || util_bitcount64(rb_mask) >= num_rb) {
      radeon_set_context_reg(cs, R_028350_PA_SC_RASTER_CONFIG, raster_config);
      if (pdev->info.gfx_level >= GFX7)
         radeon_set_context_reg(cs, R_028354_PA_SC_RASTER_CONFIG_1, raster_config_1);
   } else {
      radv_write_harvested_raster_configs(pdev, cs, raster_config, raster_config_1);
   }
}

void
radv_emit_graphics(struct radv_device *device, struct radeon_cmdbuf *cs)
{
   struct radv_physical_device *pdev = radv_device_physical(device);

   bool has_clear_state = pdev->info.has_clear_state;
   int i;

   if (!device->uses_shadow_regs) {
      radeon_emit(cs, PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
      radeon_emit(cs, CC0_UPDATE_LOAD_ENABLES(1));
      radeon_emit(cs, CC1_UPDATE_SHADOW_ENABLES(1));

      if (has_clear_state) {
         radeon_emit(cs, PKT3(PKT3_CLEAR_STATE, 0, 0));
         radeon_emit(cs, 0);
      }
   }

   if (pdev->info.gfx_level <= GFX8)
      radv_set_raster_config(pdev, cs);

   /* Emulated in shader code on GFX9+. */
   if (pdev->info.gfx_level >= GFX9)
      radeon_set_context_reg(cs, R_028AAC_VGT_ESGS_RING_ITEMSIZE, 1);

   radeon_set_context_reg(cs, R_028A18_VGT_HOS_MAX_TESS_LEVEL, fui(64));
   if (!has_clear_state)
      radeon_set_context_reg(cs, R_028A1C_VGT_HOS_MIN_TESS_LEVEL, fui(0));

   /* FIXME calculate these values somehow ??? */
   if (pdev->info.gfx_level <= GFX8) {
      radeon_set_context_reg(cs, R_028A54_VGT_GS_PER_ES, SI_GS_PER_ES);
      radeon_set_context_reg(cs, R_028A58_VGT_ES_PER_GS, 0x40);
   }

   if (!has_clear_state) {
      if (pdev->info.gfx_level < GFX11) {
         radeon_set_context_reg(cs, R_028A5C_VGT_GS_PER_VS, 0x2);
         radeon_set_context_reg(cs, R_028B98_VGT_STRMOUT_BUFFER_CONFIG, 0x0);
      }
      radeon_set_context_reg(cs, R_028A8C_VGT_PRIMITIVEID_RESET, 0x0);
   }

   if (pdev->info.gfx_level <= GFX9)
      radeon_set_context_reg(cs, R_028AA0_VGT_INSTANCE_STEP_RATE_0, 1);
   if (!has_clear_state && pdev->info.gfx_level < GFX11)
      radeon_set_context_reg(cs, R_028AB8_VGT_VTX_CNT_EN, 0x0);
   if (pdev->info.gfx_level < GFX7)
      radeon_set_config_reg(cs, R_008A14_PA_CL_ENHANCE, S_008A14_NUM_CLIP_SEQ(3) | S_008A14_CLIP_VTX_REORDER_ENA(1));

   if (!has_clear_state)
      radeon_set_context_reg(cs, R_02882C_PA_SU_PRIM_FILTER_CNTL, 0);

   /* CLEAR_STATE doesn't clear these correctly on certain generations.
    * I don't know why. Deduced by trial and error.
    */
   if (pdev->info.gfx_level <= GFX7 || !has_clear_state) {
      radeon_set_context_reg(cs, R_028B28_VGT_STRMOUT_DRAW_OPAQUE_OFFSET, 0);
      radeon_set_context_reg(cs, R_028204_PA_SC_WINDOW_SCISSOR_TL, S_028204_WINDOW_OFFSET_DISABLE(1));
      radeon_set_context_reg(cs, R_028240_PA_SC_GENERIC_SCISSOR_TL, S_028240_WINDOW_OFFSET_DISABLE(1));
      radeon_set_context_reg(cs, R_028244_PA_SC_GENERIC_SCISSOR_BR,
                             S_028244_BR_X(MAX_FRAMEBUFFER_WIDTH) | S_028244_BR_Y(MAX_FRAMEBUFFER_HEIGHT));
      radeon_set_context_reg(cs, R_028030_PA_SC_SCREEN_SCISSOR_TL, 0);
   }

   if (!has_clear_state) {
      for (i = 0; i < 16; i++) {
         radeon_set_context_reg(cs, R_0282D0_PA_SC_VPORT_ZMIN_0 + i * 8, 0);
         radeon_set_context_reg(cs, R_0282D4_PA_SC_VPORT_ZMAX_0 + i * 8, fui(1.0));
      }
   }

   if (!has_clear_state) {
      radeon_set_context_reg(cs, R_02820C_PA_SC_CLIPRECT_RULE, 0xFFFF);
      radeon_set_context_reg(cs, R_028230_PA_SC_EDGERULE, 0xAAAAAAAA);
      /* PA_SU_HARDWARE_SCREEN_OFFSET must be 0 due to hw bug on GFX6 */
      radeon_set_context_reg(cs, R_028234_PA_SU_HARDWARE_SCREEN_OFFSET, 0);
      radeon_set_context_reg(cs, R_028820_PA_CL_NANINF_CNTL, 0);
      radeon_set_context_reg(cs, R_028AC0_DB_SRESULTS_COMPARE_STATE0, 0x0);
      radeon_set_context_reg(cs, R_028AC4_DB_SRESULTS_COMPARE_STATE1, 0x0);
      radeon_set_context_reg(cs, R_028AC8_DB_PRELOAD_CONTROL, 0x0);
   }

   radeon_set_context_reg(
      cs, R_02800C_DB_RENDER_OVERRIDE,
      S_02800C_FORCE_HIS_ENABLE0(V_02800C_FORCE_DISABLE) | S_02800C_FORCE_HIS_ENABLE1(V_02800C_FORCE_DISABLE));

   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_context_reg(cs, R_028A98_VGT_DRAW_PAYLOAD_CNTL, 0);
      radeon_set_uconfig_reg(cs, R_030964_GE_MAX_VTX_INDX, ~0);
      radeon_set_uconfig_reg(cs, R_030924_GE_MIN_VTX_INDX, 0);
      radeon_set_uconfig_reg(cs, R_030928_GE_INDX_OFFSET, 0);
      radeon_set_uconfig_reg(cs, R_03097C_GE_STEREO_CNTL, 0);
      radeon_set_uconfig_reg(cs, R_030988_GE_USER_VGPR_EN, 0);

      if (pdev->info.gfx_level < GFX11) {
         radeon_set_context_reg(cs, R_028038_DB_DFSM_CONTROL, S_028038_PUNCHOUT_MODE(V_028038_FORCE_OFF));
      }
   } else if (pdev->info.gfx_level == GFX9) {
      radeon_set_uconfig_reg(cs, R_030920_VGT_MAX_VTX_INDX, ~0);
      radeon_set_uconfig_reg(cs, R_030924_VGT_MIN_VTX_INDX, 0);
      radeon_set_uconfig_reg(cs, R_030928_VGT_INDX_OFFSET, 0);

      radeon_set_context_reg(cs, R_028060_DB_DFSM_CONTROL, S_028060_PUNCHOUT_MODE(V_028060_FORCE_OFF));
   } else {
      /* These registers, when written, also overwrite the
       * CLEAR_STATE context, so we can't rely on CLEAR_STATE setting
       * them.  It would be an issue if there was another UMD
       * changing them.
       */
      radeon_set_context_reg(cs, R_028400_VGT_MAX_VTX_INDX, ~0);
      radeon_set_context_reg(cs, R_028404_VGT_MIN_VTX_INDX, 0);
      radeon_set_context_reg(cs, R_028408_VGT_INDX_OFFSET, 0);
   }

   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_sh_reg(cs, R_00B524_SPI_SHADER_PGM_HI_LS, S_00B524_MEM_BASE(pdev->info.address32_hi >> 8));
      radeon_set_sh_reg(cs, R_00B324_SPI_SHADER_PGM_HI_ES, S_00B324_MEM_BASE(pdev->info.address32_hi >> 8));
   } else if (pdev->info.gfx_level == GFX9) {
      radeon_set_sh_reg(cs, R_00B414_SPI_SHADER_PGM_HI_LS, S_00B414_MEM_BASE(pdev->info.address32_hi >> 8));
      radeon_set_sh_reg(cs, R_00B214_SPI_SHADER_PGM_HI_ES, S_00B214_MEM_BASE(pdev->info.address32_hi >> 8));
   } else {
      radeon_set_sh_reg(cs, R_00B524_SPI_SHADER_PGM_HI_LS, S_00B524_MEM_BASE(pdev->info.address32_hi >> 8));
      radeon_set_sh_reg(cs, R_00B324_SPI_SHADER_PGM_HI_ES, S_00B324_MEM_BASE(pdev->info.address32_hi >> 8));
   }

   if (pdev->info.gfx_level < GFX11)
      radeon_set_sh_reg(cs, R_00B124_SPI_SHADER_PGM_HI_VS, S_00B124_MEM_BASE(pdev->info.address32_hi >> 8));

   unsigned cu_mask_ps = 0xffffffff;

   /* It's wasteful to enable all CUs for PS if shader arrays have a
    * different number of CUs. The reason is that the hardware sends the
    * same number of PS waves to each shader array, so the slowest shader
    * array limits the performance.  Disable the extra CUs for PS in
    * other shader arrays to save power and thus increase clocks for busy
    * CUs. In the future, we might disable or enable this tweak only for
    * certain apps.
    */
   if (pdev->info.gfx_level >= GFX10_3)
      cu_mask_ps = u_bit_consecutive(0, pdev->info.min_good_cu_per_sa);

   if (pdev->info.gfx_level >= GFX7) {
      if (pdev->info.gfx_level >= GFX10 && pdev->info.gfx_level < GFX11) {
         /* Logical CUs 16 - 31 */
         radeon_set_sh_reg_idx(pdev, cs, R_00B104_SPI_SHADER_PGM_RSRC4_VS, 3,
                               ac_apply_cu_en(S_00B104_CU_EN(0xffff), C_00B104_CU_EN, 16, &pdev->info));
      }

      if (pdev->info.gfx_level >= GFX10) {
         radeon_set_sh_reg_idx(pdev, cs, R_00B404_SPI_SHADER_PGM_RSRC4_HS, 3,
                               ac_apply_cu_en(S_00B404_CU_EN(0xffff), C_00B404_CU_EN, 16, &pdev->info));
         radeon_set_sh_reg_idx(pdev, cs, R_00B004_SPI_SHADER_PGM_RSRC4_PS, 3,
                               ac_apply_cu_en(S_00B004_CU_EN(cu_mask_ps >> 16), C_00B004_CU_EN, 16, &pdev->info));
      }

      if (pdev->info.gfx_level >= GFX9) {
         radeon_set_sh_reg_idx(
            pdev, cs, R_00B41C_SPI_SHADER_PGM_RSRC3_HS, 3,
            ac_apply_cu_en(S_00B41C_CU_EN(0xffff) | S_00B41C_WAVE_LIMIT(0x3F), C_00B41C_CU_EN, 0, &pdev->info));
      } else {
         radeon_set_sh_reg(
            cs, R_00B51C_SPI_SHADER_PGM_RSRC3_LS,
            ac_apply_cu_en(S_00B51C_CU_EN(0xffff) | S_00B51C_WAVE_LIMIT(0x3F), C_00B51C_CU_EN, 0, &pdev->info));
         radeon_set_sh_reg(cs, R_00B41C_SPI_SHADER_PGM_RSRC3_HS, S_00B41C_WAVE_LIMIT(0x3F));
         radeon_set_sh_reg(
            cs, R_00B31C_SPI_SHADER_PGM_RSRC3_ES,
            ac_apply_cu_en(S_00B31C_CU_EN(0xffff) | S_00B31C_WAVE_LIMIT(0x3F), C_00B31C_CU_EN, 0, &pdev->info));
         /* If this is 0, Bonaire can hang even if GS isn't being used.
          * Other chips are unaffected. These are suboptimal values,
          * but we don't use on-chip GS.
          */
         radeon_set_context_reg(cs, R_028A44_VGT_GS_ONCHIP_CNTL,
                                S_028A44_ES_VERTS_PER_SUBGRP(64) | S_028A44_GS_PRIMS_PER_SUBGRP(4));
      }

      radeon_set_sh_reg_idx(pdev, cs, R_00B01C_SPI_SHADER_PGM_RSRC3_PS, 3,
                            ac_apply_cu_en(S_00B01C_CU_EN(cu_mask_ps) | S_00B01C_WAVE_LIMIT(0x3F) |
                                              S_00B01C_LDS_GROUP_SIZE(pdev->info.gfx_level >= GFX11),
                                           C_00B01C_CU_EN, 0, &pdev->info));
   }

   if (pdev->info.gfx_level >= GFX10) {
      /* Break up a pixel wave if it contains deallocs for more than
       * half the parameter cache.
       *
       * To avoid a deadlock where pixel waves aren't launched
       * because they're waiting for more pixels while the frontend
       * is stuck waiting for PC space, the maximum allowed value is
       * the size of the PC minus the largest possible allocation for
       * a single primitive shader subgroup.
       */
      uint32_t max_deallocs_in_wave = pdev->info.gfx_level >= GFX11 ? 16 : 512;
      radeon_set_context_reg(cs, R_028C50_PA_SC_NGG_MODE_CNTL, S_028C50_MAX_DEALLOCS_IN_WAVE(max_deallocs_in_wave));

      if (pdev->info.gfx_level < GFX11)
         radeon_set_context_reg(cs, R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL, 14);

      /* Vulkan doesn't support user edge flags and it also doesn't
       * need to prevent drawing lines on internal edges of
       * decomposed primitives (such as quads) with polygon mode = lines.
       */
      unsigned vertex_reuse_depth = pdev->info.gfx_level >= GFX10_3 ? 30 : 0;
      radeon_set_context_reg(cs, R_028838_PA_CL_NGG_CNTL,
                             S_028838_INDEX_BUF_EDGE_FLAG_ENA(0) | S_028838_VERTEX_REUSE_DEPTH(vertex_reuse_depth));

      /* Enable CMASK/FMASK/HTILE/DCC caching in L2 for small chips. */
      unsigned meta_write_policy, meta_read_policy;
      unsigned no_alloc = pdev->info.gfx_level >= GFX11 ? V_02807C_CACHE_NOA_GFX11 : V_02807C_CACHE_NOA_GFX10;

      /* TODO: investigate whether LRU improves performance on other chips too */
      if (pdev->info.max_render_backends <= 4) {
         meta_write_policy = V_02807C_CACHE_LRU_WR; /* cache writes */
         meta_read_policy = V_02807C_CACHE_LRU_RD;  /* cache reads */
      } else {
         meta_write_policy = V_02807C_CACHE_STREAM; /* write combine */
         meta_read_policy = no_alloc;               /* don't cache reads */
      }

      radeon_set_context_reg(cs, R_02807C_DB_RMI_L2_CACHE_CONTROL,
                             S_02807C_Z_WR_POLICY(V_02807C_CACHE_STREAM) | S_02807C_S_WR_POLICY(V_02807C_CACHE_STREAM) |
                                S_02807C_HTILE_WR_POLICY(meta_write_policy) |
                                S_02807C_ZPCPSD_WR_POLICY(V_02807C_CACHE_STREAM) | S_02807C_Z_RD_POLICY(no_alloc) |
                                S_02807C_S_RD_POLICY(no_alloc) | S_02807C_HTILE_RD_POLICY(meta_read_policy));

      uint32_t gl2_cc;
      if (pdev->info.gfx_level >= GFX11) {
         gl2_cc = S_028410_DCC_WR_POLICY_GFX11(meta_write_policy) |
                  S_028410_COLOR_WR_POLICY_GFX11(V_028410_CACHE_STREAM) |
                  S_028410_COLOR_RD_POLICY(V_028410_CACHE_NOA_GFX11);
      } else {
         gl2_cc = S_028410_CMASK_WR_POLICY(meta_write_policy) | S_028410_FMASK_WR_POLICY(V_028410_CACHE_STREAM) |
                  S_028410_DCC_WR_POLICY_GFX10(meta_write_policy) |
                  S_028410_COLOR_WR_POLICY_GFX10(V_028410_CACHE_STREAM) | S_028410_CMASK_RD_POLICY(meta_read_policy) |
                  S_028410_FMASK_RD_POLICY(V_028410_CACHE_NOA_GFX10) |
                  S_028410_COLOR_RD_POLICY(V_028410_CACHE_NOA_GFX10);
      }

      radeon_set_context_reg(cs, R_028410_CB_RMI_GL2_CACHE_CONTROL, gl2_cc | S_028410_DCC_RD_POLICY(meta_read_policy));
      radeon_set_context_reg(cs, R_028428_CB_COVERAGE_OUT_CONTROL, 0);

      radeon_set_sh_reg_seq(cs, R_00B0C8_SPI_SHADER_USER_ACCUM_PS_0, 4);
      radeon_emit(cs, 0); /* R_00B0C8_SPI_SHADER_USER_ACCUM_PS_0 */
      radeon_emit(cs, 0); /* R_00B0CC_SPI_SHADER_USER_ACCUM_PS_1 */
      radeon_emit(cs, 0); /* R_00B0D0_SPI_SHADER_USER_ACCUM_PS_2 */
      radeon_emit(cs, 0); /* R_00B0D4_SPI_SHADER_USER_ACCUM_PS_3 */

      if (pdev->info.gfx_level < GFX11) {
         radeon_set_sh_reg_seq(cs, R_00B1C8_SPI_SHADER_USER_ACCUM_VS_0, 4);
         radeon_emit(cs, 0); /* R_00B1C8_SPI_SHADER_USER_ACCUM_VS_0 */
         radeon_emit(cs, 0); /* R_00B1CC_SPI_SHADER_USER_ACCUM_VS_1 */
         radeon_emit(cs, 0); /* R_00B1D0_SPI_SHADER_USER_ACCUM_VS_2 */
         radeon_emit(cs, 0); /* R_00B1D4_SPI_SHADER_USER_ACCUM_VS_3 */
      }

      radeon_set_sh_reg_seq(cs, R_00B2C8_SPI_SHADER_USER_ACCUM_ESGS_0, 4);
      radeon_emit(cs, 0); /* R_00B2C8_SPI_SHADER_USER_ACCUM_ESGS_0 */
      radeon_emit(cs, 0); /* R_00B2CC_SPI_SHADER_USER_ACCUM_ESGS_1 */
      radeon_emit(cs, 0); /* R_00B2D0_SPI_SHADER_USER_ACCUM_ESGS_2 */
      radeon_emit(cs, 0); /* R_00B2D4_SPI_SHADER_USER_ACCUM_ESGS_3 */
      radeon_set_sh_reg_seq(cs, R_00B4C8_SPI_SHADER_USER_ACCUM_LSHS_0, 4);
      radeon_emit(cs, 0); /* R_00B4C8_SPI_SHADER_USER_ACCUM_LSHS_0 */
      radeon_emit(cs, 0); /* R_00B4CC_SPI_SHADER_USER_ACCUM_LSHS_1 */
      radeon_emit(cs, 0); /* R_00B4D0_SPI_SHADER_USER_ACCUM_LSHS_2 */
      radeon_emit(cs, 0); /* R_00B4D4_SPI_SHADER_USER_ACCUM_LSHS_3 */

      radeon_set_sh_reg(cs, R_00B0C0_SPI_SHADER_REQ_CTRL_PS,
                        S_00B0C0_SOFT_GROUPING_EN(1) | S_00B0C0_NUMBER_OF_REQUESTS_PER_CU(4 - 1));

      if (pdev->info.gfx_level < GFX11)
         radeon_set_sh_reg(cs, R_00B1C0_SPI_SHADER_REQ_CTRL_VS, 0);

      if (pdev->info.gfx_level >= GFX10_3) {
         radeon_set_context_reg(cs, R_028750_SX_PS_DOWNCONVERT_CONTROL, 0xff);
         /* This allows sample shading. */
         radeon_set_context_reg(cs, R_028848_PA_CL_VRS_CNTL,
                                S_028848_SAMPLE_ITER_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE));
      }
   }

   if (pdev->info.gfx_level >= GFX11) {
      /* ACCUM fields changed their meaning. */
      radeon_set_context_reg(cs, R_028B50_VGT_TESS_DISTRIBUTION,
                             S_028B50_ACCUM_ISOLINE(128) | S_028B50_ACCUM_TRI(128) | S_028B50_ACCUM_QUAD(128) |
                                S_028B50_DONUT_SPLIT_GFX9(24) | S_028B50_TRAP_SPLIT(6));
   } else if (pdev->info.gfx_level >= GFX9) {
      radeon_set_context_reg(cs, R_028B50_VGT_TESS_DISTRIBUTION,
                             S_028B50_ACCUM_ISOLINE(40) | S_028B50_ACCUM_TRI(30) | S_028B50_ACCUM_QUAD(24) |
                                S_028B50_DONUT_SPLIT_GFX9(24) | S_028B50_TRAP_SPLIT(6));
   } else if (pdev->info.gfx_level >= GFX8) {
      uint32_t vgt_tess_distribution;

      vgt_tess_distribution =
         S_028B50_ACCUM_ISOLINE(32) | S_028B50_ACCUM_TRI(11) | S_028B50_ACCUM_QUAD(11) | S_028B50_DONUT_SPLIT_GFX81(16);

      if (pdev->info.family == CHIP_FIJI || pdev->info.family >= CHIP_POLARIS10)
         vgt_tess_distribution |= S_028B50_TRAP_SPLIT(3);

      radeon_set_context_reg(cs, R_028B50_VGT_TESS_DISTRIBUTION, vgt_tess_distribution);
   } else if (!has_clear_state) {
      radeon_set_context_reg(cs, R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL, 14);
      radeon_set_context_reg(cs, R_028C5C_VGT_OUT_DEALLOC_CNTL, 16);
   }

   if (device->border_color_data.bo) {
      uint64_t border_color_va = radv_buffer_get_va(device->border_color_data.bo);

      radeon_set_context_reg(cs, R_028080_TA_BC_BASE_ADDR, border_color_va >> 8);
      if (pdev->info.gfx_level >= GFX7) {
         radeon_set_context_reg(cs, R_028084_TA_BC_BASE_ADDR_HI, S_028084_ADDRESS(border_color_va >> 40));
      }
   }

   if (pdev->info.gfx_level >= GFX8) {
      /* GFX8+ only compares the bits according to the index type by default,
       * so we can always leave the programmed value at the maximum.
       */
      radeon_set_context_reg(cs, R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX, 0xffffffff);
   }

   if (pdev->info.gfx_level >= GFX9) {
      unsigned max_alloc_count = pdev->info.pbb_max_alloc_count;

      /* GFX11+ shouldn't subtract 1 from pbb_max_alloc_count.  */
      if (pdev->info.gfx_level < GFX11)
         max_alloc_count -= 1;

      radeon_set_context_reg(cs, R_028C48_PA_SC_BINNER_CNTL_1,
                             S_028C48_MAX_ALLOC_COUNT(max_alloc_count) | S_028C48_MAX_PRIM_PER_BATCH(1023));
      radeon_set_context_reg(cs, R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1));
      radeon_set_uconfig_reg(cs, R_030968_VGT_INSTANCE_BASE_ID, 0);
   }

   unsigned tmp = (unsigned)(1.0 * 8.0);
   radeon_set_context_reg(cs, R_028A00_PA_SU_POINT_SIZE, S_028A00_HEIGHT(tmp) | S_028A00_WIDTH(tmp));
   radeon_set_context_reg(
      cs, R_028A04_PA_SU_POINT_MINMAX,
      S_028A04_MIN_SIZE(radv_pack_float_12p4(0)) | S_028A04_MAX_SIZE(radv_pack_float_12p4(8191.875 / 2)));

   if (!has_clear_state) {
      radeon_set_context_reg(cs, R_028004_DB_COUNT_CONTROL, S_028004_ZPASS_INCREMENT_DISABLE(1));
   }

   /* Enable the Polaris small primitive filter control.
    * XXX: There is possibly an issue when MSAA is off (see RadeonSI
    * has_msaa_sample_loc_bug). But this doesn't seem to regress anything,
    * and AMDVLK doesn't have a workaround as well.
    */
   if (pdev->info.family >= CHIP_POLARIS10) {
      unsigned small_prim_filter_cntl = S_028830_SMALL_PRIM_FILTER_ENABLE(1) |
                                        /* Workaround for a hw line bug. */
                                        S_028830_LINE_FILTER_DISABLE(pdev->info.family <= CHIP_POLARIS12);

      radeon_set_context_reg(cs, R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL, small_prim_filter_cntl);
   }

   radeon_set_context_reg(cs, R_0286D4_SPI_INTERP_CONTROL_0,
                          S_0286D4_FLAT_SHADE_ENA(1) | S_0286D4_PNT_SPRITE_ENA(1) |
                             S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
                             S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
                             S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
                             S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
                             S_0286D4_PNT_SPRITE_TOP_1(0)); /* vulkan is top to bottom - 1.0 at bottom */

   radeon_set_context_reg(cs, R_028BE4_PA_SU_VTX_CNTL,
                          S_028BE4_PIX_CENTER(1) | S_028BE4_ROUND_MODE(V_028BE4_X_ROUND_TO_EVEN) |
                             S_028BE4_QUANT_MODE(V_028BE4_X_16_8_FIXED_POINT_1_256TH));

   radeon_set_context_reg(cs, R_028818_PA_CL_VTE_CNTL,
                          S_028818_VTX_W0_FMT(1) | S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                             S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) |
                             S_028818_VPORT_Z_SCALE_ENA(1) | S_028818_VPORT_Z_OFFSET_ENA(1));

   if (device->tma_bo) {
      uint64_t tba_va, tma_va;

      assert(pdev->info.gfx_level == GFX8);

      tba_va = radv_shader_get_va(device->trap_handler_shader);
      tma_va = radv_buffer_get_va(device->tma_bo);

      uint32_t regs[] = {R_00B000_SPI_SHADER_TBA_LO_PS, R_00B100_SPI_SHADER_TBA_LO_VS, R_00B200_SPI_SHADER_TBA_LO_GS,
                         R_00B300_SPI_SHADER_TBA_LO_ES, R_00B400_SPI_SHADER_TBA_LO_HS, R_00B500_SPI_SHADER_TBA_LO_LS};

      for (i = 0; i < ARRAY_SIZE(regs); ++i) {
         radeon_set_sh_reg_seq(cs, regs[i], 4);
         radeon_emit(cs, tba_va >> 8);
         radeon_emit(cs, tba_va >> 40);
         radeon_emit(cs, tma_va >> 8);
         radeon_emit(cs, tma_va >> 40);
      }
   }

   if (pdev->info.gfx_level >= GFX11) {
      radeon_set_context_reg(cs, R_028C54_PA_SC_BINNER_CNTL_2,
                             S_028C54_ENABLE_PING_PONG_BIN_ORDER(pdev->info.gfx_level >= GFX11_5));

      uint64_t rb_mask = BITFIELD64_MASK(pdev->info.max_render_backends);

      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PIXEL_PIPE_STAT_CONTROL) | EVENT_INDEX(1));
      radeon_emit(cs, PIXEL_PIPE_STATE_CNTL_COUNTER_ID(0) | PIXEL_PIPE_STATE_CNTL_STRIDE(2) |
                         PIXEL_PIPE_STATE_CNTL_INSTANCE_EN_LO(rb_mask));
      radeon_emit(cs, PIXEL_PIPE_STATE_CNTL_INSTANCE_EN_HI(rb_mask));

      radeon_set_uconfig_reg(cs, R_031110_SPI_GS_THROTTLE_CNTL1, 0x12355123);
      radeon_set_uconfig_reg(cs, R_031114_SPI_GS_THROTTLE_CNTL2, 0x1544D);
   }

   /* The exclusion bits can be set to improve rasterization efficiency if no sample lies on the
    * pixel boundary (-8 sample offset). It's currently always TRUE because the driver doesn't
    * support 16 samples.
    */
   bool exclusion = pdev->info.gfx_level >= GFX7;
   radeon_set_context_reg(cs, R_02882C_PA_SU_PRIM_FILTER_CNTL,
                          S_02882C_XMAX_RIGHT_EXCLUSION(exclusion) | S_02882C_YMAX_BOTTOM_EXCLUSION(exclusion));

   radeon_set_context_reg(cs, R_028828_PA_SU_LINE_STIPPLE_SCALE, 0x3f800000);
   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_uconfig_reg(cs, R_030A00_PA_SU_LINE_STIPPLE_VALUE, 0);
      radeon_set_uconfig_reg(cs, R_030A04_PA_SC_LINE_STIPPLE_STATE, 0);
   } else {
      radeon_set_config_reg(cs, R_008A60_PA_SU_LINE_STIPPLE_VALUE, 0);
      radeon_set_config_reg(cs, R_008B10_PA_SC_LINE_STIPPLE_STATE, 0);
   }

   if (pdev->info.gfx_level >= GFX11) {
      /* Disable primitive restart for all non-indexed draws. */
      radeon_set_uconfig_reg(cs, R_03092C_GE_MULTI_PRIM_IB_RESET_EN, S_03092C_DISABLE_FOR_AUTO_INDEX(1));
   }

   radv_emit_compute(device, cs);
}

void
radv_cs_emit_write_event_eop(struct radeon_cmdbuf *cs, enum amd_gfx_level gfx_level, enum radv_queue_family qf,
                             unsigned event, unsigned event_flags, unsigned dst_sel, unsigned data_sel, uint64_t va,
                             uint32_t new_fence, uint64_t gfx9_eop_bug_va)
{
   if (qf == RADV_QUEUE_TRANSFER) {
      radeon_emit(cs, SDMA_PACKET(SDMA_OPCODE_FENCE, 0, SDMA_FENCE_MTYPE_UC));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, new_fence);
      return;
   }

   const bool is_mec = qf == RADV_QUEUE_COMPUTE && gfx_level >= GFX7;
   unsigned op =
      EVENT_TYPE(event) | EVENT_INDEX(event == V_028A90_CS_DONE || event == V_028A90_PS_DONE ? 6 : 5) | event_flags;
   unsigned is_gfx8_mec = is_mec && gfx_level < GFX9;
   unsigned sel = EOP_DST_SEL(dst_sel) | EOP_DATA_SEL(data_sel);

   /* Wait for write confirmation before writing data, but don't send
    * an interrupt. */
   if (data_sel != EOP_DATA_SEL_DISCARD)
      sel |= EOP_INT_SEL(EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM);

   if (gfx_level >= GFX9 || is_gfx8_mec) {
      /* A ZPASS_DONE or PIXEL_STAT_DUMP_EVENT (of the DB occlusion
       * counters) must immediately precede every timestamp event to
       * prevent a GPU hang on GFX9.
       */
      if (gfx_level == GFX9 && !is_mec) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 2, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_ZPASS_DONE) | EVENT_INDEX(1));
         radeon_emit(cs, gfx9_eop_bug_va);
         radeon_emit(cs, gfx9_eop_bug_va >> 32);
      }

      radeon_emit(cs, PKT3(PKT3_RELEASE_MEM, is_gfx8_mec ? 5 : 6, false));
      radeon_emit(cs, op);
      radeon_emit(cs, sel);
      radeon_emit(cs, va);        /* address lo */
      radeon_emit(cs, va >> 32);  /* address hi */
      radeon_emit(cs, new_fence); /* immediate data lo */
      radeon_emit(cs, 0);         /* immediate data hi */
      if (!is_gfx8_mec)
         radeon_emit(cs, 0); /* unused */
   } else {
      /* On GFX6, EOS events are always emitted with EVENT_WRITE_EOS.
       * On GFX7+, EOS events are emitted with EVENT_WRITE_EOS on
       * the graphics queue, and with RELEASE_MEM on the compute
       * queue.
       */
      if (event == V_028B9C_CS_DONE || event == V_028B9C_PS_DONE) {
         assert(event_flags == 0 && dst_sel == EOP_DST_SEL_MEM && data_sel == EOP_DATA_SEL_VALUE_32BIT);

         if (is_mec) {
            radeon_emit(cs, PKT3(PKT3_RELEASE_MEM, 5, false));
            radeon_emit(cs, op);
            radeon_emit(cs, sel);
            radeon_emit(cs, va);        /* address lo */
            radeon_emit(cs, va >> 32);  /* address hi */
            radeon_emit(cs, new_fence); /* immediate data lo */
            radeon_emit(cs, 0);         /* immediate data hi */
         } else {
            radeon_emit(cs, PKT3(PKT3_EVENT_WRITE_EOS, 3, false));
            radeon_emit(cs, op);
            radeon_emit(cs, va);
            radeon_emit(cs, ((va >> 32) & 0xffff) | EOS_DATA_SEL(EOS_DATA_SEL_VALUE_32BIT));
            radeon_emit(cs, new_fence);
         }
      } else {
         if (gfx_level == GFX7 || gfx_level == GFX8) {
            /* Two EOP events are required to make all
             * engines go idle (and optional cache flushes
             * executed) before the timestamp is written.
             */
            radeon_emit(cs, PKT3(PKT3_EVENT_WRITE_EOP, 4, false));
            radeon_emit(cs, op);
            radeon_emit(cs, va);
            radeon_emit(cs, ((va >> 32) & 0xffff) | sel);
            radeon_emit(cs, 0); /* immediate data */
            radeon_emit(cs, 0); /* unused */
         }

         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE_EOP, 4, false));
         radeon_emit(cs, op);
         radeon_emit(cs, va);
         radeon_emit(cs, ((va >> 32) & 0xffff) | sel);
         radeon_emit(cs, new_fence); /* immediate data */
         radeon_emit(cs, 0);         /* unused */
      }
   }
}

static void
radv_emit_acquire_mem(struct radeon_cmdbuf *cs, bool is_mec, bool is_gfx9, unsigned cp_coher_cntl)
{
   if (is_mec || is_gfx9) {
      uint32_t hi_val = is_gfx9 ? 0xffffff : 0xff;
      radeon_emit(cs, PKT3(PKT3_ACQUIRE_MEM, 5, false) | PKT3_SHADER_TYPE_S(is_mec));
      radeon_emit(cs, cp_coher_cntl); /* CP_COHER_CNTL */
      radeon_emit(cs, 0xffffffff);    /* CP_COHER_SIZE */
      radeon_emit(cs, hi_val);        /* CP_COHER_SIZE_HI */
      radeon_emit(cs, 0);             /* CP_COHER_BASE */
      radeon_emit(cs, 0);             /* CP_COHER_BASE_HI */
      radeon_emit(cs, 0x0000000A);    /* POLL_INTERVAL */
   } else {
      /* ACQUIRE_MEM is only required on a compute ring. */
      radeon_emit(cs, PKT3(PKT3_SURFACE_SYNC, 3, false));
      radeon_emit(cs, cp_coher_cntl); /* CP_COHER_CNTL */
      radeon_emit(cs, 0xffffffff);    /* CP_COHER_SIZE */
      radeon_emit(cs, 0);             /* CP_COHER_BASE */
      radeon_emit(cs, 0x0000000A);    /* POLL_INTERVAL */
   }
}

static void
gfx10_cs_emit_cache_flush(struct radeon_cmdbuf *cs, enum amd_gfx_level gfx_level, uint32_t *flush_cnt,
                          uint64_t flush_va, enum radv_queue_family qf, enum radv_cmd_flush_bits flush_bits,
                          enum rgp_flush_bits *sqtt_flush_bits, uint64_t gfx9_eop_bug_va)
{
   const bool is_mec = qf == RADV_QUEUE_COMPUTE;
   uint32_t gcr_cntl = 0;
   unsigned cb_db_event = 0;

   /* We don't need these. */
   assert(!(flush_bits & (RADV_CMD_FLAG_VGT_STREAMOUT_SYNC)));

   if (flush_bits & RADV_CMD_FLAG_INV_ICACHE) {
      gcr_cntl |= S_586_GLI_INV(V_586_GLI_ALL);

      *sqtt_flush_bits |= RGP_FLUSH_INVAL_ICACHE;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_SCACHE) {
      /* TODO: When writing to the SMEM L1 cache, we need to set SEQ
       * to FORWARD when both L1 and L2 are written out (WB or INV).
       */
      gcr_cntl |= S_586_GL1_INV(1) | S_586_GLK_INV(1);

      *sqtt_flush_bits |= RGP_FLUSH_INVAL_SMEM_L0;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_VCACHE) {
      gcr_cntl |= S_586_GL1_INV(1) | S_586_GLV_INV(1);

      *sqtt_flush_bits |= RGP_FLUSH_INVAL_VMEM_L0 | RGP_FLUSH_INVAL_L1;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_L2) {
      /* Writeback and invalidate everything in L2. */
      gcr_cntl |= S_586_GL2_INV(1) | S_586_GL2_WB(1) | S_586_GLM_INV(1) | S_586_GLM_WB(1);

      *sqtt_flush_bits |= RGP_FLUSH_INVAL_L2;
   } else if (flush_bits & RADV_CMD_FLAG_WB_L2) {
      /* Writeback but do not invalidate.
       * GLM doesn't support WB alone. If WB is set, INV must be set too.
       */
      gcr_cntl |= S_586_GL2_WB(1) | S_586_GLM_WB(1) | S_586_GLM_INV(1);

      *sqtt_flush_bits |= RGP_FLUSH_FLUSH_L2;
   } else if (flush_bits & RADV_CMD_FLAG_INV_L2_METADATA) {
      gcr_cntl |= S_586_GLM_INV(1) | S_586_GLM_WB(1);
   }

   if (flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB)) {
      /* TODO: trigger on RADV_CMD_FLAG_FLUSH_AND_INV_CB_META */
      if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB) {
         /* Flush CMASK/FMASK/DCC. Will wait for idle later. */
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));

         *sqtt_flush_bits |= RGP_FLUSH_FLUSH_CB | RGP_FLUSH_INVAL_CB;
      }

      /* TODO: trigger on RADV_CMD_FLAG_FLUSH_AND_INV_DB_META ? */
      if (gfx_level < GFX11 && (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB)) {
         /* Flush HTILE. Will wait for idle later. */
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));

         *sqtt_flush_bits |= RGP_FLUSH_FLUSH_DB | RGP_FLUSH_INVAL_DB;
      }

      /* First flush CB/DB, then L1/L2. */
      gcr_cntl |= S_586_SEQ(V_586_SEQ_FORWARD);

      if ((flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB)) ==
          (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB)) {
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
      } else if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB) {
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
      } else if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB) {
         if (gfx_level == GFX11) {
            cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
         } else {
            cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
         }
      } else {
         assert(0);
      }
   } else {
      /* Wait for graphics shaders to go idle if requested. */
      if (flush_bits & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));

         *sqtt_flush_bits |= RGP_FLUSH_PS_PARTIAL_FLUSH;
      } else if (flush_bits & RADV_CMD_FLAG_VS_PARTIAL_FLUSH) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));

         *sqtt_flush_bits |= RGP_FLUSH_VS_PARTIAL_FLUSH;
      }
   }

   if (flush_bits & RADV_CMD_FLAG_CS_PARTIAL_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH | EVENT_INDEX(4)));

      *sqtt_flush_bits |= RGP_FLUSH_CS_PARTIAL_FLUSH;
   }

   if (cb_db_event) {
      if (gfx_level >= GFX11) {
         /* Get GCR_CNTL fields, because the encoding is different in RELEASE_MEM. */
         unsigned glm_wb = G_586_GLM_WB(gcr_cntl);
         unsigned glm_inv = G_586_GLM_INV(gcr_cntl);
         unsigned glk_wb = G_586_GLK_WB(gcr_cntl);
         unsigned glk_inv = G_586_GLK_INV(gcr_cntl);
         unsigned glv_inv = G_586_GLV_INV(gcr_cntl);
         unsigned gl1_inv = G_586_GL1_INV(gcr_cntl);
         assert(G_586_GL2_US(gcr_cntl) == 0);
         assert(G_586_GL2_RANGE(gcr_cntl) == 0);
         assert(G_586_GL2_DISCARD(gcr_cntl) == 0);
         unsigned gl2_inv = G_586_GL2_INV(gcr_cntl);
         unsigned gl2_wb = G_586_GL2_WB(gcr_cntl);
         unsigned gcr_seq = G_586_SEQ(gcr_cntl);

         gcr_cntl &= C_586_GLM_WB & C_586_GLM_INV & C_586_GLK_WB & C_586_GLK_INV & C_586_GLV_INV & C_586_GL1_INV &
                     C_586_GL2_INV & C_586_GL2_WB; /* keep SEQ */

         /* Send an event that flushes caches. */
         radeon_emit(cs, PKT3(PKT3_RELEASE_MEM, 6, 0));
         radeon_emit(cs, S_490_EVENT_TYPE(cb_db_event) | S_490_EVENT_INDEX(5) | S_490_GLM_WB(glm_wb) |
                            S_490_GLM_INV(glm_inv) | S_490_GLV_INV(glv_inv) | S_490_GL1_INV(gl1_inv) |
                            S_490_GL2_INV(gl2_inv) | S_490_GL2_WB(gl2_wb) | S_490_SEQ(gcr_seq) | S_490_GLK_WB(glk_wb) |
                            S_490_GLK_INV(glk_inv) | S_490_PWS_ENABLE(1));
         radeon_emit(cs, 0); /* DST_SEL, INT_SEL, DATA_SEL */
         radeon_emit(cs, 0); /* ADDRESS_LO */
         radeon_emit(cs, 0); /* ADDRESS_HI */
         radeon_emit(cs, 0); /* DATA_LO */
         radeon_emit(cs, 0); /* DATA_HI */
         radeon_emit(cs, 0); /* INT_CTXID */

         /* Wait for the event and invalidate remaining caches if needed. */
         radeon_emit(cs, PKT3(PKT3_ACQUIRE_MEM, 6, 0));
         radeon_emit(cs, S_580_PWS_STAGE_SEL(V_580_CP_PFP) | S_580_PWS_COUNTER_SEL(V_580_TS_SELECT) |
                            S_580_PWS_ENA2(1) | S_580_PWS_COUNT(0));
         radeon_emit(cs, 0xffffffff); /* GCR_SIZE */
         radeon_emit(cs, 0x01ffffff); /* GCR_SIZE_HI */
         radeon_emit(cs, 0);          /* GCR_BASE_LO */
         radeon_emit(cs, 0);          /* GCR_BASE_HI */
         radeon_emit(cs, S_585_PWS_ENA(1));
         radeon_emit(cs, gcr_cntl); /* GCR_CNTL */

         gcr_cntl = 0; /* all done */
      } else {
         /* CB/DB flush and invalidate (or possibly just a wait for a
          * meta flush) via RELEASE_MEM.
          *
          * Combine this with other cache flushes when possible; this
          * requires affected shaders to be idle, so do it after the
          * CS_PARTIAL_FLUSH before (VS/PS partial flushes are always
          * implied).
          */
         /* Get GCR_CNTL fields, because the encoding is different in RELEASE_MEM. */
         unsigned glm_wb = G_586_GLM_WB(gcr_cntl);
         unsigned glm_inv = G_586_GLM_INV(gcr_cntl);
         unsigned glv_inv = G_586_GLV_INV(gcr_cntl);
         unsigned gl1_inv = G_586_GL1_INV(gcr_cntl);
         assert(G_586_GL2_US(gcr_cntl) == 0);
         assert(G_586_GL2_RANGE(gcr_cntl) == 0);
         assert(G_586_GL2_DISCARD(gcr_cntl) == 0);
         unsigned gl2_inv = G_586_GL2_INV(gcr_cntl);
         unsigned gl2_wb = G_586_GL2_WB(gcr_cntl);
         unsigned gcr_seq = G_586_SEQ(gcr_cntl);

         gcr_cntl &=
            C_586_GLM_WB & C_586_GLM_INV & C_586_GLV_INV & C_586_GL1_INV & C_586_GL2_INV & C_586_GL2_WB; /* keep SEQ */

         assert(flush_cnt);
         (*flush_cnt)++;

         radv_cs_emit_write_event_eop(cs, gfx_level, qf, cb_db_event,
                                      S_490_GLM_WB(glm_wb) | S_490_GLM_INV(glm_inv) | S_490_GLV_INV(glv_inv) |
                                         S_490_GL1_INV(gl1_inv) | S_490_GL2_INV(gl2_inv) | S_490_GL2_WB(gl2_wb) |
                                         S_490_SEQ(gcr_seq),
                                      EOP_DST_SEL_MEM, EOP_DATA_SEL_VALUE_32BIT, flush_va, *flush_cnt, gfx9_eop_bug_va);

         radv_cp_wait_mem(cs, qf, WAIT_REG_MEM_EQUAL, flush_va, *flush_cnt, 0xffffffff);
      }
   }

   /* VGT state sync */
   if (flush_bits & RADV_CMD_FLAG_VGT_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
   }

   /* Ignore fields that only modify the behavior of other fields. */
   if (gcr_cntl & C_586_GL1_RANGE & C_586_GL2_RANGE & C_586_SEQ) {
      /* Flush caches and wait for the caches to assert idle.
       * The cache flush is executed in the ME, but the PFP waits
       * for completion.
       */
      radeon_emit(cs, PKT3(PKT3_ACQUIRE_MEM, 6, 0));
      radeon_emit(cs, 0);          /* CP_COHER_CNTL */
      radeon_emit(cs, 0xffffffff); /* CP_COHER_SIZE */
      radeon_emit(cs, 0xffffff);   /* CP_COHER_SIZE_HI */
      radeon_emit(cs, 0);          /* CP_COHER_BASE */
      radeon_emit(cs, 0);          /* CP_COHER_BASE_HI */
      radeon_emit(cs, 0x0000000A); /* POLL_INTERVAL */
      radeon_emit(cs, gcr_cntl);   /* GCR_CNTL */
   } else if ((cb_db_event || (flush_bits & (RADV_CMD_FLAG_VS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
                                             RADV_CMD_FLAG_CS_PARTIAL_FLUSH))) &&
              !is_mec) {
      /* We need to ensure that PFP waits as well. */
      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);

      *sqtt_flush_bits |= RGP_FLUSH_PFP_SYNC_ME;
   }

   if (flush_bits & RADV_CMD_FLAG_START_PIPELINE_STATS) {
      if (qf == RADV_QUEUE_GENERAL) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_START) | EVENT_INDEX(0));
      } else if (qf == RADV_QUEUE_COMPUTE) {
         radeon_set_sh_reg(cs, R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(1));
      }
   } else if (flush_bits & RADV_CMD_FLAG_STOP_PIPELINE_STATS) {
      if (qf == RADV_QUEUE_GENERAL) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_STOP) | EVENT_INDEX(0));
      } else if (qf == RADV_QUEUE_COMPUTE) {
         radeon_set_sh_reg(cs, R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(0));
      }
   }
}

void
radv_cs_emit_cache_flush(struct radeon_winsys *ws, struct radeon_cmdbuf *cs, enum amd_gfx_level gfx_level,
                         uint32_t *flush_cnt, uint64_t flush_va, enum radv_queue_family qf,
                         enum radv_cmd_flush_bits flush_bits, enum rgp_flush_bits *sqtt_flush_bits,
                         uint64_t gfx9_eop_bug_va)
{
   unsigned cp_coher_cntl = 0;
   uint32_t flush_cb_db = flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB);

   radeon_check_space(ws, cs, 128);

   if (gfx_level >= GFX10) {
      /* GFX10 cache flush handling is quite different. */
      gfx10_cs_emit_cache_flush(cs, gfx_level, flush_cnt, flush_va, qf, flush_bits, sqtt_flush_bits, gfx9_eop_bug_va);
      return;
   }

   const bool is_mec = qf == RADV_QUEUE_COMPUTE && gfx_level >= GFX7;

   if (flush_bits & RADV_CMD_FLAG_INV_ICACHE) {
      cp_coher_cntl |= S_0085F0_SH_ICACHE_ACTION_ENA(1);
      *sqtt_flush_bits |= RGP_FLUSH_INVAL_ICACHE;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_SCACHE) {
      cp_coher_cntl |= S_0085F0_SH_KCACHE_ACTION_ENA(1);
      *sqtt_flush_bits |= RGP_FLUSH_INVAL_SMEM_L0;
   }

   if (gfx_level <= GFX8) {
      if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB) {
         cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1) | S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
                          S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
                          S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
                          S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1);

         /* Necessary for DCC */
         if (gfx_level >= GFX8) {
            radv_cs_emit_write_event_eop(cs, gfx_level, is_mec, V_028A90_FLUSH_AND_INV_CB_DATA_TS, 0, EOP_DST_SEL_MEM,
                                         EOP_DATA_SEL_DISCARD, 0, 0, gfx9_eop_bug_va);
         }

         *sqtt_flush_bits |= RGP_FLUSH_FLUSH_CB | RGP_FLUSH_INVAL_CB;
      }
      if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB) {
         cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1) | S_0085F0_DB_DEST_BASE_ENA(1);

         *sqtt_flush_bits |= RGP_FLUSH_FLUSH_DB | RGP_FLUSH_INVAL_DB;
      }
   }

   if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB_META) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0));

      *sqtt_flush_bits |= RGP_FLUSH_FLUSH_CB | RGP_FLUSH_INVAL_CB;
   }

   if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB_META) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_FLUSH_AND_INV_DB_META) | EVENT_INDEX(0));

      *sqtt_flush_bits |= RGP_FLUSH_FLUSH_DB | RGP_FLUSH_INVAL_DB;
   }

   if (flush_bits & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));

      *sqtt_flush_bits |= RGP_FLUSH_PS_PARTIAL_FLUSH;
   } else if (flush_bits & RADV_CMD_FLAG_VS_PARTIAL_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));

      *sqtt_flush_bits |= RGP_FLUSH_VS_PARTIAL_FLUSH;
   }

   if (flush_bits & RADV_CMD_FLAG_CS_PARTIAL_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH) | EVENT_INDEX(4));

      *sqtt_flush_bits |= RGP_FLUSH_CS_PARTIAL_FLUSH;
   }

   if (gfx_level == GFX9 && flush_cb_db) {
      unsigned cb_db_event, tc_flags;

      /* Set the CB/DB flush event. */
      cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;

      /* These are the only allowed combinations. If you need to
       * do multiple operations at once, do them separately.
       * All operations that invalidate L2 also seem to invalidate
       * metadata. Volatile (VOL) and WC flushes are not listed here.
       *
       * TC    | TC_WB         = writeback & invalidate L2 & L1
       * TC    | TC_WB | TC_NC = writeback & invalidate L2 for MTYPE == NC
       *         TC_WB | TC_NC = writeback L2 for MTYPE == NC
       * TC            | TC_NC = invalidate L2 for MTYPE == NC
       * TC    | TC_MD         = writeback & invalidate L2 metadata (DCC, etc.)
       * TCL1                  = invalidate L1
       */
      tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_MD_ACTION_ENA;

      *sqtt_flush_bits |= RGP_FLUSH_FLUSH_CB | RGP_FLUSH_INVAL_CB | RGP_FLUSH_FLUSH_DB | RGP_FLUSH_INVAL_DB;

      /* Ideally flush TC together with CB/DB. */
      if (flush_bits & RADV_CMD_FLAG_INV_L2) {
         /* Writeback and invalidate everything in L2 & L1. */
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_WB_ACTION_ENA;

         /* Clear the flags. */
         flush_bits &= ~(RADV_CMD_FLAG_INV_L2 | RADV_CMD_FLAG_WB_L2 | RADV_CMD_FLAG_INV_VCACHE);

         *sqtt_flush_bits |= RGP_FLUSH_INVAL_L2;
      }

      assert(flush_cnt);
      (*flush_cnt)++;

      radv_cs_emit_write_event_eop(cs, gfx_level, false, cb_db_event, tc_flags, EOP_DST_SEL_MEM,
                                   EOP_DATA_SEL_VALUE_32BIT, flush_va, *flush_cnt, gfx9_eop_bug_va);
      radv_cp_wait_mem(cs, qf, WAIT_REG_MEM_EQUAL, flush_va, *flush_cnt, 0xffffffff);
   }

   /* VGT state sync */
   if (flush_bits & RADV_CMD_FLAG_VGT_FLUSH) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
   }

   /* VGT streamout state sync */
   if (flush_bits & RADV_CMD_FLAG_VGT_STREAMOUT_SYNC) {
      radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_STREAMOUT_SYNC) | EVENT_INDEX(0));
   }

   /* Make sure ME is idle (it executes most packets) before continuing.
    * This prevents read-after-write hazards between PFP and ME.
    */
   if ((cp_coher_cntl || (flush_bits & (RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE |
                                        RADV_CMD_FLAG_INV_L2 | RADV_CMD_FLAG_WB_L2))) &&
       !is_mec) {
      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);

      *sqtt_flush_bits |= RGP_FLUSH_PFP_SYNC_ME;
   }

   if ((flush_bits & RADV_CMD_FLAG_INV_L2) || (gfx_level <= GFX7 && (flush_bits & RADV_CMD_FLAG_WB_L2))) {
      radv_emit_acquire_mem(cs, is_mec, gfx_level == GFX9,
                            cp_coher_cntl | S_0085F0_TC_ACTION_ENA(1) | S_0085F0_TCL1_ACTION_ENA(1) |
                               S_0301F0_TC_WB_ACTION_ENA(gfx_level >= GFX8));
      cp_coher_cntl = 0;

      *sqtt_flush_bits |= RGP_FLUSH_INVAL_L2 | RGP_FLUSH_INVAL_VMEM_L0;
   } else {
      if (flush_bits & RADV_CMD_FLAG_WB_L2) {
         /* WB = write-back
          * NC = apply to non-coherent MTYPEs
          *      (i.e. MTYPE <= 1, which is what we use everywhere)
          *
          * WB doesn't work without NC.
          */
         radv_emit_acquire_mem(cs, is_mec, gfx_level == GFX9,
                               cp_coher_cntl | S_0301F0_TC_WB_ACTION_ENA(1) | S_0301F0_TC_NC_ACTION_ENA(1));
         cp_coher_cntl = 0;

         *sqtt_flush_bits |= RGP_FLUSH_FLUSH_L2 | RGP_FLUSH_INVAL_VMEM_L0;
      }
      if (flush_bits & RADV_CMD_FLAG_INV_VCACHE) {
         radv_emit_acquire_mem(cs, is_mec, gfx_level == GFX9, cp_coher_cntl | S_0085F0_TCL1_ACTION_ENA(1));
         cp_coher_cntl = 0;

         *sqtt_flush_bits |= RGP_FLUSH_INVAL_VMEM_L0;
      }
   }

   /* When one of the DEST_BASE flags is set, SURFACE_SYNC waits for idle.
    * Therefore, it should be last. Done in PFP.
    */
   if (cp_coher_cntl)
      radv_emit_acquire_mem(cs, is_mec, gfx_level == GFX9, cp_coher_cntl);

   if (flush_bits & RADV_CMD_FLAG_START_PIPELINE_STATS) {
      if (qf == RADV_QUEUE_GENERAL) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_START) | EVENT_INDEX(0));
      } else if (qf == RADV_QUEUE_COMPUTE) {
         radeon_set_sh_reg(cs, R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(1));
      }
   } else if (flush_bits & RADV_CMD_FLAG_STOP_PIPELINE_STATS) {
      if (qf == RADV_QUEUE_GENERAL) {
         radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cs, EVENT_TYPE(V_028A90_PIPELINESTAT_STOP) | EVENT_INDEX(0));
      } else if (qf == RADV_QUEUE_COMPUTE) {
         radeon_set_sh_reg(cs, R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(0));
      }
   }
}

void
radv_emit_cond_exec(const struct radv_device *device, struct radeon_cmdbuf *cs, uint64_t va, uint32_t count)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;

   if (gfx_level >= GFX7) {
      radeon_emit(cs, PKT3(PKT3_COND_EXEC, 3, 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, 0);
      radeon_emit(cs, count);
   } else {
      radeon_emit(cs, PKT3(PKT3_COND_EXEC, 2, 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, count);
   }
}

/* For MSAA sample positions. */
#define FILL_SREG(s0x, s0y, s1x, s1y, s2x, s2y, s3x, s3y)                                                              \
   ((((unsigned)(s0x)&0xf) << 0) | (((unsigned)(s0y)&0xf) << 4) | (((unsigned)(s1x)&0xf) << 8) |                       \
    (((unsigned)(s1y)&0xf) << 12) | (((unsigned)(s2x)&0xf) << 16) | (((unsigned)(s2y)&0xf) << 20) |                    \
    (((unsigned)(s3x)&0xf) << 24) | (((unsigned)(s3y)&0xf) << 28))

/* For obtaining location coordinates from registers */
#define SEXT4(x)               ((int)((x) | ((x)&0x8 ? 0xfffffff0 : 0)))
#define GET_SFIELD(reg, index) SEXT4(((reg) >> ((index)*4)) & 0xf)
#define GET_SX(reg, index)     GET_SFIELD((reg)[(index) / 4], ((index) % 4) * 2)
#define GET_SY(reg, index)     GET_SFIELD((reg)[(index) / 4], ((index) % 4) * 2 + 1)

/* 1x MSAA */
static const uint32_t sample_locs_1x = FILL_SREG(0, 0, 0, 0, 0, 0, 0, 0);
static const unsigned max_dist_1x = 0;
static const uint64_t centroid_priority_1x = 0x0000000000000000ull;

/* 2xMSAA */
static const uint32_t sample_locs_2x = FILL_SREG(4, 4, -4, -4, 0, 0, 0, 0);
static const unsigned max_dist_2x = 4;
static const uint64_t centroid_priority_2x = 0x1010101010101010ull;

/* 4xMSAA */
static const uint32_t sample_locs_4x = FILL_SREG(-2, -6, 6, -2, -6, 2, 2, 6);
static const unsigned max_dist_4x = 6;
static const uint64_t centroid_priority_4x = 0x3210321032103210ull;

/* 8xMSAA */
static const uint32_t sample_locs_8x[] = {
   FILL_SREG(1, -3, -1, 3, 5, 1, -3, -5),
   FILL_SREG(-5, 5, -7, -1, 3, 7, 7, -7),
   /* The following are unused by hardware, but we emit them to IBs
    * instead of multiple SET_CONTEXT_REG packets. */
   0,
   0,
};
static const unsigned max_dist_8x = 7;
static const uint64_t centroid_priority_8x = 0x7654321076543210ull;

unsigned
radv_get_default_max_sample_dist(int log_samples)
{
   unsigned max_dist[] = {
      max_dist_1x,
      max_dist_2x,
      max_dist_4x,
      max_dist_8x,
   };
   return max_dist[log_samples];
}

void
radv_emit_default_sample_locations(struct radeon_cmdbuf *cs, int nr_samples)
{
   switch (nr_samples) {
   default:
   case 1:
      radeon_set_context_reg_seq(cs, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
      radeon_emit(cs, (uint32_t)centroid_priority_1x);
      radeon_emit(cs, centroid_priority_1x >> 32);
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, sample_locs_1x);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, sample_locs_1x);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, sample_locs_1x);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, sample_locs_1x);
      break;
   case 2:
      radeon_set_context_reg_seq(cs, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
      radeon_emit(cs, (uint32_t)centroid_priority_2x);
      radeon_emit(cs, centroid_priority_2x >> 32);
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, sample_locs_2x);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, sample_locs_2x);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, sample_locs_2x);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, sample_locs_2x);
      break;
   case 4:
      radeon_set_context_reg_seq(cs, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
      radeon_emit(cs, (uint32_t)centroid_priority_4x);
      radeon_emit(cs, centroid_priority_4x >> 32);
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, sample_locs_4x);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, sample_locs_4x);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, sample_locs_4x);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, sample_locs_4x);
      break;
   case 8:
      radeon_set_context_reg_seq(cs, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
      radeon_emit(cs, (uint32_t)centroid_priority_8x);
      radeon_emit(cs, centroid_priority_8x >> 32);
      radeon_set_context_reg_seq(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, 14);
      radeon_emit_array(cs, sample_locs_8x, 4);
      radeon_emit_array(cs, sample_locs_8x, 4);
      radeon_emit_array(cs, sample_locs_8x, 4);
      radeon_emit_array(cs, sample_locs_8x, 2);
      break;
   }
}

static void
radv_get_sample_position(struct radv_device *device, unsigned sample_count, unsigned sample_index, float *out_value)
{
   const uint32_t *sample_locs;

   switch (sample_count) {
   case 1:
   default:
      sample_locs = &sample_locs_1x;
      break;
   case 2:
      sample_locs = &sample_locs_2x;
      break;
   case 4:
      sample_locs = &sample_locs_4x;
      break;
   case 8:
      sample_locs = sample_locs_8x;
      break;
   }

   out_value[0] = (GET_SX(sample_locs, sample_index) + 8) / 16.0f;
   out_value[1] = (GET_SY(sample_locs, sample_index) + 8) / 16.0f;
}

void
radv_device_init_msaa(struct radv_device *device)
{
   int i;

   radv_get_sample_position(device, 1, 0, device->sample_locations_1x[0]);

   for (i = 0; i < 2; i++)
      radv_get_sample_position(device, 2, i, device->sample_locations_2x[i]);
   for (i = 0; i < 4; i++)
      radv_get_sample_position(device, 4, i, device->sample_locations_4x[i]);
   for (i = 0; i < 8; i++)
      radv_get_sample_position(device, 8, i, device->sample_locations_8x[i]);
}

void
radv_cs_write_data_imm(struct radeon_cmdbuf *cs, unsigned engine_sel, uint64_t va, uint32_t imm)
{
   radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
   radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(engine_sel));
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);
   radeon_emit(cs, imm);
}
