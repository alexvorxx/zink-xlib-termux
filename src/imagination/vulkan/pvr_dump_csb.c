/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

#include "pvr_bo.h"
#include "pvr_csb.h"
#include "pvr_csb_enum_helpers.h"
#include "pvr_device_info.h"
#include "pvr_dump.h"
#include "pvr_dump_bo.h"
#include "pvr_private.h"
#include "pvr_util.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vulkan/util/vk_enum_to_str.h"

/*****************************************************************************
   Utilities
 ******************************************************************************/

#define PVR_DUMP_CSB_WORD_SIZE ((unsigned)sizeof(uint32_t))

enum buffer_type {
   BUFFER_TYPE_NONE = 0,
   BUFFER_TYPE_CDMCTRL,
   BUFFER_TYPE_VDMCTRL,
   BUFFER_TYPE_PPP,
   BUFFER_TYPE_INVALID, /* Must be last. */
};

struct pvr_dump_csb_ctx {
   struct pvr_dump_buffer_ctx base;

   /* User-modifiable values */
   uint32_t next_block_idx;
};

static inline bool
pvr_dump_csb_ctx_push(struct pvr_dump_csb_ctx *const ctx,
                      struct pvr_dump_buffer_ctx *const parent_ctx)
{
   if (!pvr_dump_buffer_ctx_push(&ctx->base,
                                 &parent_ctx->base,
                                 parent_ctx->ptr,
                                 parent_ctx->remaining_size)) {
      return false;
   }

   ctx->next_block_idx = 0;

   return true;
}

static inline struct pvr_dump_buffer_ctx *
pvr_dump_csb_ctx_pop(struct pvr_dump_csb_ctx *const ctx, bool advance_parent)
{
   struct pvr_dump_buffer_ctx *parent;
   struct pvr_dump_ctx *parent_base;
   const uint64_t unused_words =
      ctx->base.remaining_size / PVR_DUMP_CSB_WORD_SIZE;

   if (unused_words) {
      pvr_dump_buffer_print_header_line(&ctx->base,
                                        "<%" PRIu64 " unused word%s (%" PRIu64
                                        " bytes)>",
                                        unused_words,
                                        unused_words == 1 ? "" : "s",
                                        unused_words * PVR_DUMP_CSB_WORD_SIZE);

      pvr_dump_buffer_advance(&ctx->base,
                              unused_words * PVR_DUMP_CSB_WORD_SIZE);
   }

   pvr_dump_buffer_print_header_line(&ctx->base, "<end of buffer>");

   parent_base = pvr_dump_buffer_ctx_pop(&ctx->base);
   if (!parent_base)
      return NULL;

   parent = container_of(parent_base, struct pvr_dump_buffer_ctx, base);

   if (advance_parent)
      pvr_dump_buffer_advance(parent, ctx->base.capacity);

   return parent;
}

struct pvr_dump_csb_block_ctx {
   struct pvr_dump_buffer_ctx base;
};

#define pvr_dump_csb_block_ctx_push(ctx,                               \
                                    parent_ctx,                        \
                                    header_format,                     \
                                    header_args...)                    \
   ({                                                                  \
      struct pvr_dump_csb_ctx *const _csb_ctx = (parent_ctx);          \
      pvr_dump_buffer_print_header_line(&_csb_ctx->base,               \
                                        "%" PRIu32 ": " header_format, \
                                        _csb_ctx->next_block_idx,      \
                                        ##header_args);                \
      __pvr_dump_csb_block_ctx_push(ctx, _csb_ctx);                    \
   })

static inline bool
__pvr_dump_csb_block_ctx_push(struct pvr_dump_csb_block_ctx *const ctx,
                              struct pvr_dump_csb_ctx *const parent_ctx)
{
   pvr_dump_indent(&parent_ctx->base.base);

   if (!pvr_dump_buffer_ctx_push(&ctx->base,
                                 &parent_ctx->base.base,
                                 parent_ctx->base.ptr,
                                 parent_ctx->base.remaining_size)) {
      return false;
   }

   parent_ctx->next_block_idx++;

   return true;
}

static inline struct pvr_dump_csb_ctx *
pvr_dump_csb_block_ctx_pop(struct pvr_dump_csb_block_ctx *const ctx)
{
   const uint64_t used_size = ctx->base.capacity - ctx->base.remaining_size;
   struct pvr_dump_csb_ctx *parent_ctx;
   struct pvr_dump_ctx *parent_base;

   parent_base = pvr_dump_buffer_ctx_pop(&ctx->base);
   if (!parent_base)
      return NULL;

   parent_ctx = container_of(parent_base, struct pvr_dump_csb_ctx, base.base);

   /* No need to check this since it can never fail. */
   pvr_dump_buffer_advance(&parent_ctx->base, used_size);

   pvr_dump_dedent(parent_base);

   return parent_ctx;
}

static inline const uint32_t *
pvr_dump_csb_block_take(struct pvr_dump_csb_block_ctx *const restrict ctx,
                        const uint32_t nr_words)
{
   return pvr_dump_buffer_take(&ctx->base, nr_words * PVR_DUMP_CSB_WORD_SIZE);
}

#define pvr_dump_csb_block_take_packed(ctx, cmd, dest)             \
   ({                                                              \
      struct pvr_dump_csb_block_ctx *const _block_ctx = (ctx);     \
      struct PVRX(cmd) *const _dest = (dest);                      \
      const void *const _ptr =                                     \
         pvr_dump_csb_block_take(_block_ctx, pvr_cmd_length(cmd)); \
      if (_ptr) {                                                  \
         pvr_cmd_unpack(cmd)(_ptr, _dest);                         \
      } else {                                                     \
         pvr_dump_field_error(&_block_ctx->base.base,              \
                              "failed to unpack word(s)");         \
      }                                                            \
      !!_ptr;                                                      \
   })

/*****************************************************************************
   Feature dumping
 ******************************************************************************/

static inline void
__pvr_dump_field_needs_feature(struct pvr_dump_ctx *const ctx,
                               const char *const name,
                               const char *const feature)
{
   pvr_dump_field(ctx, name, "<feature %s not present>", feature);
}

#define pvr_dump_field_needs_feature(ctx, name, feature)              \
   do {                                                               \
      (void)PVR_HAS_FEATURE((struct pvr_device_info *)NULL, feature); \
      __pvr_dump_field_needs_feature(ctx, name, #feature);            \
   } while (0)

#define pvr_dump_field_member_needs_feature(ctx, compound, member, feature) \
   do {                                                                     \
      (void)&(compound)->member;                                            \
      pvr_dump_field_needs_feature(ctx, #member, feature);                  \
   } while (0)

/******************************************************************************
   Sub buffer printer declaration
 ******************************************************************************/

static bool print_sub_buffer(struct pvr_dump_ctx *ctx,
                             struct pvr_device *device,
                             enum buffer_type type,
                             pvr_dev_addr_t addr,
                             uint64_t expected_size,
                             char const *size_src);

/******************************************************************************
   Block printers
 ******************************************************************************/

static bool print_block_cdmctrl_kernel(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(CDMCTRL_KERNEL0) kernel0;
   struct PVRX(CDMCTRL_KERNEL1) kernel1;
   struct PVRX(CDMCTRL_KERNEL2) kernel2;
   struct PVRX(CDMCTRL_KERNEL3) kernel3;
   struct PVRX(CDMCTRL_KERNEL4) kernel4;
   struct PVRX(CDMCTRL_KERNEL5) kernel5;
   struct PVRX(CDMCTRL_KERNEL6) kernel6;
   struct PVRX(CDMCTRL_KERNEL7) kernel7;
   struct PVRX(CDMCTRL_KERNEL8) kernel8;
   struct PVRX(CDMCTRL_KERNEL9) kernel9;
   struct PVRX(CDMCTRL_KERNEL10) kernel10;
   struct PVRX(CDMCTRL_KERNEL11) kernel11;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "KERNEL"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL0, &kernel0) ||
       !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL1, &kernel1) ||
       !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL2, &kernel2)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_member_bool(base_ctx, &kernel0, indirect_present);
   pvr_dump_field_member_bool(base_ctx, &kernel0, global_offsets_present);
   pvr_dump_field_member_bool(base_ctx, &kernel0, event_object_present);
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &kernel0,
      usc_common_size,
      PVRX(CDMCTRL_KERNEL0_USC_COMMON_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &kernel0,
      usc_unified_size,
      PVRX(CDMCTRL_KERNEL0_USC_UNIFIED_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &kernel0,
      pds_temp_size,
      PVRX(CDMCTRL_KERNEL0_PDS_TEMP_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &kernel0,
      pds_data_size,
      PVRX(CDMCTRL_KERNEL0_PDS_DATA_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_enum(base_ctx,
                              &kernel0,
                              usc_target,
                              pvr_cmd_enum_to_str(CDMCTRL_USC_TARGET));
   pvr_dump_field_member_bool(base_ctx, &kernel0, fence);

   pvr_dump_field_member_addr(base_ctx, &kernel1, data_addr);
   pvr_dump_field_member_enum(base_ctx,
                              &kernel1,
                              sd_type,
                              pvr_cmd_enum_to_str(CDMCTRL_SD_TYPE));
   pvr_dump_field_member_bool(base_ctx, &kernel1, usc_common_shared);

   pvr_dump_field_member_addr(base_ctx, &kernel2, code_addr);
   pvr_dump_field_member_bool(base_ctx, &kernel2, one_wg_per_task);

   if (!kernel0.indirect_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL3, &kernel3) ||
          !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL4, &kernel4) ||
          !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL5, &kernel5)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32_offset(base_ctx, &kernel3, workgroup_x, 1);
      pvr_dump_field_member_u32_offset(base_ctx, &kernel4, workgroup_y, 1);
      pvr_dump_field_member_u32_offset(base_ctx, &kernel5, workgroup_z, 1);

      pvr_dump_field_not_present(base_ctx, "indirect_addr");
   } else {
      if (!pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL6, &kernel6) ||
          !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL7, &kernel7)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_not_present(base_ctx, &kernel3, workgroup_x);
      pvr_dump_field_member_not_present(base_ctx, &kernel4, workgroup_y);
      pvr_dump_field_member_not_present(base_ctx, &kernel5, workgroup_z);

      pvr_dump_field_addr_split(base_ctx,
                                "indirect_addr",
                                kernel6.indirect_addrmsb,
                                kernel7.indirect_addrlsb);
   }

   if (!pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL8, &kernel8))
      goto end_pop_ctx;

   pvr_dump_field_member_u32_zero(base_ctx, &kernel8, max_instances, 32);
   pvr_dump_field_member_u32_offset(base_ctx, &kernel8, workgroup_size_x, 1);
   pvr_dump_field_member_u32_offset(base_ctx, &kernel8, workgroup_size_y, 1);
   pvr_dump_field_member_u32_offset(base_ctx, &kernel8, workgroup_size_z, 1);

   if (kernel0.event_object_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL9, &kernel9) ||
          !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL10, &kernel10) ||
          !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_KERNEL11, &kernel11)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32(base_ctx, &kernel9, global_offset_x);
      pvr_dump_field_member_u32(base_ctx, &kernel10, global_offset_y);
      pvr_dump_field_member_u32(base_ctx, &kernel11, global_offset_z);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &kernel9, global_offset_x);
      pvr_dump_field_member_not_present(base_ctx, &kernel10, global_offset_y);
      pvr_dump_field_member_not_present(base_ctx, &kernel11, global_offset_z);
   }

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_cdmctrl_stream_link(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(CDMCTRL_STREAM_LINK0) link0;
   struct PVRX(CDMCTRL_STREAM_LINK1) link1;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STREAM_LINK"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_STREAM_LINK0, &link0) ||
       !pvr_dump_csb_block_take_packed(&ctx, CDMCTRL_STREAM_LINK1, &link1)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_addr_split(base_ctx,
                             "link_addr",
                             link0.link_addrmsb,
                             link1.link_addrlsb);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_cdmctrl_stream_terminate(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(CDMCTRL_STREAM_TERMINATE) terminate;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "TERMINATE"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx,
                                       CDMCTRL_STREAM_TERMINATE,
                                       &terminate)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_no_fields(base_ctx);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_ppp_state_update(struct pvr_dump_csb_ctx *const csb_ctx,
                                     struct pvr_device *const device)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   pvr_dev_addr_t ppp_addr;
   uint32_t ppp_size;

   struct PVRX(VDMCTRL_PPP_STATE0) state0;
   struct PVRX(VDMCTRL_PPP_STATE1) state1;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "PPP_STATE_UPDATE"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_PPP_STATE0, &state0) ||
       !pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_PPP_STATE1, &state1)) {
      goto end_pop_ctx;
   }

   ppp_addr = PVR_DEV_ADDR(state0.addrmsb.addr | state1.addrlsb.addr);
   ppp_size = state0.word_count ? state0.word_count : 256;

   pvr_dump_field_member_u32_zero(base_ctx, &state0, word_count, 256);
   pvr_dump_field_addr_split(base_ctx, "addr", state0.addrmsb, state1.addrlsb);
   ret = print_sub_buffer(base_ctx,
                          device,
                          BUFFER_TYPE_PPP,
                          ppp_addr,
                          ppp_size,
                          "word_count");

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_pds_state_update(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(VDMCTRL_PDS_STATE0) state0;
   struct PVRX(VDMCTRL_PDS_STATE1) state1;
   struct PVRX(VDMCTRL_PDS_STATE2) state2;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "PDS_STATE_UPDATE"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_PDS_STATE0, &state0) ||
       !pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_PDS_STATE1, &state1) ||
       !pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_PDS_STATE2, &state2)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_member_enum(base_ctx,
                              &state0,
                              dm_target,
                              pvr_cmd_enum_to_str(VDMCTRL_DM_TARGET));
   pvr_dump_field_member_enum(base_ctx,
                              &state0,
                              usc_target,
                              pvr_cmd_enum_to_str(VDMCTRL_USC_TARGET));
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &state0,
      usc_common_size,
      PVRX(VDMCTRL_PDS_STATE0_USC_COMMON_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &state0,
      usc_unified_size,
      PVRX(VDMCTRL_PDS_STATE0_USC_UNIFIED_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &state0,
      pds_temp_size,
      PVRX(VDMCTRL_PDS_STATE0_PDS_TEMP_SIZE_UNIT_SIZE),
      "bytes");
   pvr_dump_field_member_u32_scaled_units(
      base_ctx,
      &state0,
      pds_data_size,
      PVRX(VDMCTRL_PDS_STATE0_PDS_DATA_SIZE_UNIT_SIZE),
      "bytes");

   pvr_dump_field_member_addr(base_ctx, &state1, pds_data_addr);
   pvr_dump_field_member_enum(base_ctx,
                              &state1,
                              sd_type,
                              pvr_cmd_enum_to_str(VDMCTRL_SD_TYPE));
   pvr_dump_field_member_enum(base_ctx,
                              &state1,
                              sd_next_type,
                              pvr_cmd_enum_to_str(VDMCTRL_SD_TYPE));

   pvr_dump_field_member_addr(base_ctx, &state2, pds_code_addr);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_vdm_state_update(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(VDMCTRL_VDM_STATE0) state0;
   struct PVRX(VDMCTRL_VDM_STATE1) state1;
   struct PVRX(VDMCTRL_VDM_STATE2) state2;
   struct PVRX(VDMCTRL_VDM_STATE3) state3;
   struct PVRX(VDMCTRL_VDM_STATE4) state4;
   struct PVRX(VDMCTRL_VDM_STATE5) state5;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "VDM_STATE_UPDATE"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_VDM_STATE0, &state0))
      goto end_pop_ctx;

   if (state0.cut_index_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_VDM_STATE1, &state1))
         goto end_pop_ctx;

      pvr_dump_field_member_x32(base_ctx, &state1, cut_index, 8);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &state1, cut_index);
   }

   if (state0.vs_data_addr_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_VDM_STATE2, &state2))
         goto end_pop_ctx;

      pvr_dump_field_member_addr(base_ctx, &state2, vs_pds_data_base_addr);
   } else {
      pvr_dump_field_member_not_present(base_ctx,
                                        &state2,
                                        vs_pds_data_base_addr);
   }

   if (state0.vs_other_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_VDM_STATE3, &state3) ||
          !pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_VDM_STATE4, &state4) ||
          !pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_VDM_STATE5, &state5)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_addr(base_ctx, &state3, vs_pds_code_base_addr);

      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &state4,
         vs_output_size,
         PVRX(VDMCTRL_VDM_STATE4_VS_OUTPUT_SIZE_UNIT_SIZE),
         "bytes");

      pvr_dump_field_member_u32_zero(base_ctx, &state5, vs_max_instances, 32);
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &state5,
         vs_usc_common_size,
         PVRX(VDMCTRL_VDM_STATE5_VS_USC_COMMON_SIZE_UNIT_SIZE),
         "bytes");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &state5,
         vs_usc_unified_size,
         PVRX(VDMCTRL_VDM_STATE5_VS_USC_UNIFIED_SIZE_UNIT_SIZE),
         "bytes");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &state5,
         vs_pds_temp_size,
         PVRX(VDMCTRL_VDM_STATE5_VS_PDS_TEMP_SIZE_UNIT_SIZE),
         "bytes");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &state5,
         vs_pds_data_size,
         PVRX(VDMCTRL_VDM_STATE5_VS_PDS_DATA_SIZE_UNIT_SIZE),
         "bytes");
   } else {
      pvr_dump_field_member_not_present(base_ctx,
                                        &state3,
                                        vs_pds_code_base_addr);
      pvr_dump_field_member_not_present(base_ctx, &state4, vs_output_size);
      pvr_dump_field_member_not_present(base_ctx, &state5, vs_max_instances);
      pvr_dump_field_member_not_present(base_ctx, &state5, vs_usc_common_size);
      pvr_dump_field_member_not_present(base_ctx, &state5, vs_usc_unified_size);
      pvr_dump_field_member_not_present(base_ctx, &state5, vs_pds_temp_size);
      pvr_dump_field_member_not_present(base_ctx, &state5, vs_pds_data_size);
   }

   pvr_dump_field_member_bool(base_ctx, &state0, ds_present);
   pvr_dump_field_member_bool(base_ctx, &state0, gs_present);
   pvr_dump_field_member_bool(base_ctx, &state0, hs_present);
   pvr_dump_field_member_u32_offset(base_ctx, &state0, cam_size, 1);
   pvr_dump_field_member_enum(
      base_ctx,
      &state0,
      uvs_scratch_size_select,
      pvr_cmd_enum_to_str(VDMCTRL_UVS_SCRATCH_SIZE_SELECT));
   pvr_dump_field_member_bool(base_ctx, &state0, cut_index_enable);
   pvr_dump_field_member_bool(base_ctx, &state0, tess_enable);
   pvr_dump_field_member_bool(base_ctx, &state0, gs_enable);
   pvr_dump_field_member_enum(base_ctx,
                              &state0,
                              flatshade_control,
                              pvr_cmd_enum_to_str(VDMCTRL_FLATSHADE_CONTROL));
   pvr_dump_field_member_bool(base_ctx, &state0, generate_primitive_id);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_index_list(struct pvr_dump_csb_ctx *const csb_ctx,
                               const struct pvr_device_info *const dev_info)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(VDMCTRL_INDEX_LIST0) index_list0;
   struct PVRX(VDMCTRL_INDEX_LIST1) index_list1;
   struct PVRX(VDMCTRL_INDEX_LIST2) index_list2;
   struct PVRX(VDMCTRL_INDEX_LIST3) index_list3;
   struct PVRX(VDMCTRL_INDEX_LIST4) index_list4;
   struct PVRX(VDMCTRL_INDEX_LIST5) index_list5;
   struct PVRX(VDMCTRL_INDEX_LIST6) index_list6;
   struct PVRX(VDMCTRL_INDEX_LIST7) index_list7;
   struct PVRX(VDMCTRL_INDEX_LIST8) index_list8;
   struct PVRX(VDMCTRL_INDEX_LIST9) index_list9;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "INDEX_LIST"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_INDEX_LIST0, &index_list0))
      goto end_pop_ctx;

   if (PVR_HAS_FEATURE(dev_info, vdm_degenerate_culling)) {
      pvr_dump_field_member_bool(base_ctx, &index_list0, degen_cull_enable);
   } else {
      pvr_dump_field_member_needs_feature(base_ctx,
                                          &index_list0,
                                          degen_cull_enable,
                                          vdm_degenerate_culling);
   }

   pvr_dump_field_member_enum(base_ctx,
                              &index_list0,
                              index_size,
                              pvr_cmd_enum_to_str(VDMCTRL_INDEX_SIZE));
   pvr_dump_field_member_u32_offset(base_ctx, &index_list0, patch_count, 1);
   pvr_dump_field_member_enum(base_ctx,
                              &index_list0,
                              primitive_topology,
                              pvr_cmd_enum_to_str(VDMCTRL_PRIMITIVE_TOPOLOGY));

   if (index_list0.index_addr_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST1,
                                          &index_list1)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_addr_split(base_ctx,
                                "index_base_addr",
                                index_list0.index_base_addrmsb,
                                index_list1.index_base_addrlsb);
   } else {
      pvr_dump_field_not_present(base_ctx, "index_base_addr");
   }

   if (index_list0.index_count_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST2,
                                          &index_list2)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32(base_ctx, &index_list2, index_count);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &index_list2, index_count);
   }

   if (index_list0.index_instance_count_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST3,
                                          &index_list3)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32_offset(base_ctx,
                                       &index_list3,
                                       instance_count,
                                       1);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &index_list3, instance_count);
   }

   if (index_list0.index_offset_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST4,
                                          &index_list4)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32(base_ctx, &index_list4, index_offset);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &index_list4, index_offset);
   }

   if (index_list0.start_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST5,
                                          &index_list5) ||
          !pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST6,
                                          &index_list6)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32(base_ctx, &index_list5, start_index);
      pvr_dump_field_member_u32(base_ctx, &index_list6, start_instance);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &index_list5, start_index);
      pvr_dump_field_member_not_present(base_ctx, &index_list6, start_instance);
   }

   if (index_list0.indirect_addr_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST7,
                                          &index_list7) ||
          !pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST8,
                                          &index_list8)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_addr_split(base_ctx,
                                "indirect_base_addr",
                                index_list7.indirect_base_addrmsb,
                                index_list8.indirect_base_addrlsb);
   } else {
      pvr_dump_field_not_present(base_ctx, "indirect_base_addr");
   }

   if (index_list0.split_count_present) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          VDMCTRL_INDEX_LIST9,
                                          &index_list9)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32(base_ctx, &index_list9, split_count);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &index_list9, split_count);
   }

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_stream_link(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(VDMCTRL_STREAM_LINK0) link0;
   struct PVRX(VDMCTRL_STREAM_LINK1) link1;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STREAM_LINK"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_STREAM_LINK0, &link0) ||
       !pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_STREAM_LINK1, &link1)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_member_bool(base_ctx, &link0, with_return);

   if (link0.compare_present) {
      pvr_dump_field_member_u32(base_ctx, &link0, compare_mode);
      pvr_dump_field_member_u32(base_ctx, &link0, compare_data);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &link0, compare_mode);
      pvr_dump_field_member_not_present(base_ctx, &link0, compare_data);
   }

   pvr_dump_field_addr_split(base_ctx,
                             "link_addr",
                             link0.link_addrmsb,
                             link1.link_addrlsb);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_stream_return(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(VDMCTRL_STREAM_RETURN) return_;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STREAM_RETURN"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, VDMCTRL_STREAM_RETURN, &return_))
      goto end_pop_ctx;

   pvr_dump_field_no_fields(base_ctx);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_vdmctrl_stream_terminate(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(VDMCTRL_STREAM_TERMINATE) terminate;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "TERMINATE"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx,
                                       VDMCTRL_STREAM_TERMINATE,
                                       &terminate)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_member_bool(base_ctx, &terminate, context);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_ppp_state_header(struct pvr_dump_csb_ctx *const csb_ctx,
                             struct PVRX(TA_STATE_HEADER) *const header_out)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_HEADER) header;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_HEADER"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_HEADER, &header))
      goto end_pop_ctx;

   pvr_dump_field_member_bool(base_ctx, &header, pres_ispctl);
   pvr_dump_field_member_bool(base_ctx, &header, pres_ispctl_fa);
   pvr_dump_field_member_bool(base_ctx, &header, pres_ispctl_fb);
   pvr_dump_field_member_bool(base_ctx, &header, pres_ispctl_ba);
   pvr_dump_field_member_bool(base_ctx, &header, pres_ispctl_bb);
   pvr_dump_field_member_bool(base_ctx, &header, pres_ispctl_dbsc);
   pvr_dump_field_member_bool(base_ctx, &header, pres_pds_state_ptr0);
   pvr_dump_field_member_bool(base_ctx, &header, pres_pds_state_ptr1);
   pvr_dump_field_member_bool(base_ctx, &header, pres_pds_state_ptr2);
   pvr_dump_field_member_bool(base_ctx, &header, pres_pds_state_ptr3);
   pvr_dump_field_member_bool(base_ctx, &header, pres_region_clip);
   pvr_dump_field_member_bool(base_ctx, &header, pres_viewport);
   pvr_dump_field_member_u32_offset(base_ctx, &header, view_port_count, 1);
   pvr_dump_field_member_bool(base_ctx, &header, pres_wclamp);
   pvr_dump_field_member_bool(base_ctx, &header, pres_outselects);
   pvr_dump_field_member_bool(base_ctx, &header, pres_varying_word0);
   pvr_dump_field_member_bool(base_ctx, &header, pres_varying_word1);
   pvr_dump_field_member_bool(base_ctx, &header, pres_varying_word2);
   pvr_dump_field_member_bool(base_ctx, &header, pres_ppp_ctrl);
   pvr_dump_field_member_bool(base_ctx, &header, pres_stream_out_size);
   pvr_dump_field_member_bool(base_ctx, &header, pres_stream_out_program);
   pvr_dump_field_member_bool(base_ctx, &header, context_switch);
   pvr_dump_field_member_bool(base_ctx, &header, pres_terminate);
   pvr_dump_field_member_bool(base_ctx, &header, not_final_term);

   if (header_out)
      *header_out = header;

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_ppp_state_isp_one_side(struct pvr_dump_csb_block_ctx *const ctx,
                                   const bool bpres)
{
   struct pvr_dump_ctx *const base_ctx = &ctx->base.base;

   struct PVRX(TA_STATE_ISPA) isp_a;
   struct PVRX(TA_STATE_ISPB) isp_b;

   if (!pvr_dump_csb_block_take_packed(ctx, TA_STATE_ISPA, &isp_a))
      return false;

   pvr_dump_field_member_enum(base_ctx,
                              &isp_a,
                              objtype,
                              pvr_cmd_enum_to_str(TA_OBJTYPE));
   pvr_dump_field_member_enum(base_ctx,
                              &isp_a,
                              passtype,
                              pvr_cmd_enum_to_str(TA_PASSTYPE));
   pvr_dump_field_member_bool(base_ctx, &isp_a, ovgvispassmaskop);
   pvr_dump_field_member_bool(base_ctx, &isp_a, maskval);
   pvr_dump_field_member_bool(base_ctx, &isp_a, dwritedisable);
   pvr_dump_field_member_bool(base_ctx, &isp_a, dfbztestenable);
   pvr_dump_field_member_enum(base_ctx,
                              &isp_a,
                              dcmpmode,
                              pvr_cmd_enum_to_str(TA_CMPMODE));
   pvr_dump_field_member_bool(base_ctx, &isp_a, linefilllastpixel);
   pvr_dump_field_member_uq4_4_offset(base_ctx, &isp_a, pointlinewidth, 0x01);
   pvr_dump_field_member_u32(base_ctx, &isp_a, sref);

   if (bpres) {
      if (!pvr_dump_csb_block_take_packed(ctx, TA_STATE_ISPB, &isp_b))
         return false;

      pvr_dump_field_member_enum(base_ctx,
                                 &isp_b,
                                 scmpmode,
                                 pvr_cmd_enum_to_str(TA_CMPMODE));
      pvr_dump_field_member_enum(base_ctx,
                                 &isp_b,
                                 sop1,
                                 pvr_cmd_enum_to_str(TA_ISPB_STENCILOP));
      pvr_dump_field_member_enum(base_ctx,
                                 &isp_b,
                                 sop2,
                                 pvr_cmd_enum_to_str(TA_ISPB_STENCILOP));
      pvr_dump_field_member_enum(base_ctx,
                                 &isp_b,
                                 sop3,
                                 pvr_cmd_enum_to_str(TA_ISPB_STENCILOP));
      pvr_dump_field_member_x32(base_ctx, &isp_b, scmpmask, 2);
      pvr_dump_field_member_x32(base_ctx, &isp_b, swmask, 2);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &isp_b, scmpmode);
      pvr_dump_field_member_not_present(base_ctx, &isp_b, sop1);
      pvr_dump_field_member_not_present(base_ctx, &isp_b, sop2);
      pvr_dump_field_member_not_present(base_ctx, &isp_b, sop3);
      pvr_dump_field_member_not_present(base_ctx, &isp_b, scmpmask);
      pvr_dump_field_member_not_present(base_ctx, &isp_b, swmask);
   }

   return true;
}

static bool print_block_ppp_state_isp(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_ISPCTL) isp_ctl;
   struct PVRX(TA_STATE_ISPDBSC) isp_dbsc;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_ISP"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_ISPCTL, &isp_ctl))
      goto end_pop_ctx;

   pvr_dump_field_member_u32(base_ctx, &isp_ctl, visreg);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, visbool);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, vistest);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, scenable);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, dbenable);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, bpres);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, two_sided);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, ovgmtestdisable);
   pvr_dump_field_member_bool(base_ctx, &isp_ctl, tagwritedisable);
   pvr_dump_field_member_u32(base_ctx, &isp_ctl, upass);
   pvr_dump_field_member_u32(base_ctx, &isp_ctl, validid);

   pvr_dump_println(base_ctx, "front");
   pvr_dump_indent(base_ctx);
   ret = print_block_ppp_state_isp_one_side(&ctx, isp_ctl.bpres);
   pvr_dump_dedent(base_ctx);
   if (!ret)
      goto end_pop_ctx;

   if (isp_ctl.two_sided) {
      pvr_dump_println(base_ctx, "back");
      pvr_dump_indent(base_ctx);
      ret = print_block_ppp_state_isp_one_side(&ctx, isp_ctl.bpres);
      pvr_dump_dedent(base_ctx);
      if (!ret)
         goto end_pop_ctx;
   } else {
      pvr_dump_field_not_present(base_ctx, "back");
   }

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_ISPDBSC, &isp_dbsc))
      goto end_pop_ctx;

   pvr_dump_field_member_u32(base_ctx, &isp_dbsc, dbindex);
   pvr_dump_field_member_u32(base_ctx, &isp_dbsc, scindex);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool print_block_ppp_state_pds(struct pvr_dump_csb_ctx *const csb_ctx,
                                      const bool has_initial_words,
                                      const bool has_varying,
                                      const bool has_texturedata,
                                      const bool has_uniformdata)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_PDS_SHADERBASE) shader_base;
   struct PVRX(TA_STATE_PDS_TEXUNICODEBASE) tex_unicode_base;
   struct PVRX(TA_STATE_PDS_SIZEINFO1) size_info1;
   struct PVRX(TA_STATE_PDS_SIZEINFO2) size_info2;
   struct PVRX(TA_STATE_PDS_VARYINGBASE) varying_base;
   struct PVRX(TA_STATE_PDS_TEXTUREDATABASE) texture_data_base;
   struct PVRX(TA_STATE_PDS_UNIFORMDATABASE) uniform_data_base;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_PDS"))
      goto end_out;

   if (has_initial_words) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_SHADERBASE,
                                          &shader_base) ||
          !pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_TEXUNICODEBASE,
                                          &tex_unicode_base) ||
          !pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_SIZEINFO1,
                                          &size_info1) ||
          !pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_SIZEINFO2,
                                          &size_info2)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_addr(base_ctx, "shaderbase", shader_base.addr);
      pvr_dump_field_addr(base_ctx, "texunicodebase", tex_unicode_base.addr);

      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &size_info1,
         pds_uniformsize,
         PVRX(TA_STATE_PDS_SIZEINFO1_PDS_UNIFORMSIZE_UNIT_SIZE),
         "words");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &size_info1,
         pds_texturestatesize,
         PVRX(TA_STATE_PDS_SIZEINFO1_PDS_TEXTURESTATESIZE_UNIT_SIZE),
         "words");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &size_info1,
         pds_varyingsize,
         PVRX(TA_STATE_PDS_SIZEINFO1_PDS_VARYINGSIZE_UNIT_SIZE),
         "words");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &size_info1,
         usc_varyingsize,
         PVRX(TA_STATE_PDS_SIZEINFO1_USC_VARYINGSIZE_UNIT_SIZE),
         "words");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &size_info1,
         pds_tempsize,
         PVRX(TA_STATE_PDS_SIZEINFO1_PDS_TEMPSIZE_UNIT_SIZE),
         "words");

      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &size_info2,
         usc_sharedsize,
         PVRX(TA_STATE_PDS_SIZEINFO2_USC_SHAREDSIZE_UNIT_SIZE),
         "words");
      pvr_dump_field_member_bool(base_ctx, &size_info2, pds_tri_merge_disable);
      pvr_dump_field_member_u32(base_ctx, &size_info2, pds_batchnum);
   } else {
      pvr_dump_field_not_present(base_ctx, "shaderbase");
      pvr_dump_field_not_present(base_ctx, "texunicodebase");
      pvr_dump_field_member_not_present(base_ctx, &size_info1, pds_uniformsize);
      pvr_dump_field_member_not_present(base_ctx,
                                        &size_info1,
                                        pds_texturestatesize);
      pvr_dump_field_member_not_present(base_ctx, &size_info1, pds_varyingsize);
      pvr_dump_field_member_not_present(base_ctx, &size_info1, usc_varyingsize);
      pvr_dump_field_member_not_present(base_ctx, &size_info1, pds_tempsize);
      pvr_dump_field_member_not_present(base_ctx, &size_info2, usc_sharedsize);
      pvr_dump_field_member_not_present(base_ctx,
                                        &size_info2,
                                        pds_tri_merge_disable);
      pvr_dump_field_member_not_present(base_ctx, &size_info2, pds_batchnum);
   }

   if (has_varying) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_VARYINGBASE,
                                          &varying_base)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_addr(base_ctx, "varyingbase", varying_base.addr);
   } else {
      pvr_dump_field_not_present(base_ctx, "varyingbase");
   }

   if (has_texturedata) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_TEXTUREDATABASE,
                                          &texture_data_base)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_addr(base_ctx, "texturedatabase", texture_data_base.addr);
   } else {
      pvr_dump_field_not_present(base_ctx, "texturedatabase");
   }

   if (has_uniformdata) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_PDS_UNIFORMDATABASE,
                                          &uniform_data_base)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_addr(base_ctx, "uniformdatabase", uniform_data_base.addr);
   } else {
      pvr_dump_field_not_present(base_ctx, "uniformdatabase");
   }

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool print_block_ppp_region_clip(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_REGION_CLIP0) clip0;
   struct PVRX(TA_REGION_CLIP1) clip1;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "REGION_CLIP"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_REGION_CLIP0, &clip0) ||
       !pvr_dump_csb_block_take_packed(&ctx, TA_REGION_CLIP1, &clip1)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_member_enum(base_ctx,
                              &clip0,
                              mode,
                              pvr_cmd_enum_to_str(TA_REGION_CLIP_MODE));
   pvr_dump_field_member_u32_scaled_units(base_ctx, &clip0, left, 32, "pixels");
   pvr_dump_field_member_u32_scaled_units(base_ctx, &clip0, right, 32, "pixels");

   pvr_dump_field_member_u32_scaled_units(base_ctx, &clip1, top, 32, "pixels");
   pvr_dump_field_member_u32_scaled_units(base_ctx,
                                          &clip1,
                                          bottom,
                                          32,
                                          "pixels");

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool print_block_ppp_viewport(struct pvr_dump_csb_ctx *const csb_ctx,
                                     const uint32_t idx)
{
   static char const *const field_names[] = {
      "a0", "m0", "a1", "m1", "a2", "m2"
   };

   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   STATIC_ASSERT(sizeof(float) == 4);

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "VIEWPORT %" PRIu32, idx))
      goto end_out;

   for (uint32_t i = 0; i < ARRAY_SIZE(field_names); i++) {
      const uint32_t *const value = pvr_dump_csb_block_take(&ctx, 1);
      if (!value)
         goto end_pop_ctx;

      pvr_dump_field_f32(base_ctx, field_names[i], uif(*value));
   }

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool print_block_ppp_wclamp(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   STATIC_ASSERT(sizeof(float) == 4);

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "WCLAMP"))
      goto end_out;

   const uint32_t *const value = pvr_dump_csb_block_take(&ctx, 1);
   if (!value)
      goto end_pop_ctx;

   pvr_dump_field_f32(base_ctx, "value", uif(*value));

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool print_block_ppp_output_sel(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_OUTPUT_SEL) output_sel;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "OUTPUT_SEL"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_OUTPUT_SEL, &output_sel))
      goto end_pop_ctx;

   pvr_dump_field_member_bool(base_ctx, &output_sel, plane0);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane1);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane2);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane3);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane4);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane5);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane6);
   pvr_dump_field_member_bool(base_ctx, &output_sel, plane7);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane0);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane1);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane2);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane3);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane4);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane5);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane6);
   pvr_dump_field_member_bool(base_ctx, &output_sel, cullplane7);
   pvr_dump_field_member_bool(base_ctx, &output_sel, rhw_pres);
   pvr_dump_field_member_bool(base_ctx,
                              &output_sel,
                              isp_position_depth_clamp_z);
   pvr_dump_field_member_bool(base_ctx, &output_sel, psprite_size_pres);
   pvr_dump_field_member_bool(base_ctx, &output_sel, vpt_tgt_pres);
   pvr_dump_field_member_bool(base_ctx, &output_sel, render_tgt_pres);
   pvr_dump_field_member_bool(base_ctx, &output_sel, tsp_unclamped_z_pres);
   pvr_dump_field_member_u32(base_ctx, &output_sel, vtxsize);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_ppp_state_varying(struct pvr_dump_csb_ctx *const csb_ctx,
                              const bool has_word0,
                              const bool has_word1,
                              const bool has_word2)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_VARYING0) varying0;
   struct PVRX(TA_STATE_VARYING1) varying1;
   struct PVRX(TA_STATE_VARYING2) varying2;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_VARYING"))
      goto end_out;

   if (has_word0) {
      if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_VARYING0, &varying0))
         goto end_pop_ctx;

      pvr_dump_field_member_u32(base_ctx, &varying0, f32_linear);
      pvr_dump_field_member_u32(base_ctx, &varying0, f32_flat);
      pvr_dump_field_member_u32(base_ctx, &varying0, f32_npc);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &varying0, f32_linear);
      pvr_dump_field_member_not_present(base_ctx, &varying0, f32_flat);
      pvr_dump_field_member_not_present(base_ctx, &varying0, f32_npc);
   }

   if (has_word1) {
      if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_VARYING1, &varying1))
         goto end_pop_ctx;

      pvr_dump_field_member_u32(base_ctx, &varying1, f16_linear);
      pvr_dump_field_member_u32(base_ctx, &varying1, f16_flat);
      pvr_dump_field_member_u32(base_ctx, &varying1, f16_npc);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &varying1, f16_linear);
      pvr_dump_field_member_not_present(base_ctx, &varying1, f16_flat);
      pvr_dump_field_member_not_present(base_ctx, &varying1, f16_npc);
   }

   if (has_word2) {
      if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_VARYING2, &varying2))
         goto end_pop_ctx;

      pvr_dump_field_member_u32(base_ctx, &varying2, output_clip_planes);
   } else {
      pvr_dump_field_member_not_present(base_ctx,
                                        &varying2,
                                        output_clip_planes);
   }

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_ppp_state_ppp_ctrl(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_PPP_CTRL) ppp_ctrl;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_PPP_CTRL"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_PPP_CTRL, &ppp_ctrl))
      goto end_pop_ctx;

   pvr_dump_field_member_enum(base_ctx,
                              &ppp_ctrl,
                              cullmode,
                              pvr_cmd_enum_to_str(TA_CULLMODE));
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, updatebbox);
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, resetbbox);
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, wbuffen);
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, wclampen);
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, pretransform);
   pvr_dump_field_member_enum(base_ctx,
                              &ppp_ctrl,
                              flatshade_vtx,
                              pvr_cmd_enum_to_str(TA_FLATSHADE));
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, drawclippededges);
   pvr_dump_field_member_enum(base_ctx,
                              &ppp_ctrl,
                              clip_mode,
                              pvr_cmd_enum_to_str(TA_CLIP_MODE));
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, pres_prim_id);
   pvr_dump_field_member_enum(base_ctx,
                              &ppp_ctrl,
                              gs_output_topology,
                              pvr_cmd_enum_to_str(TA_GS_OUTPUT_TOPOLOGY));
   pvr_dump_field_member_bool(base_ctx, &ppp_ctrl, prim_msaa);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_ppp_state_stream_out(struct pvr_dump_csb_ctx *const csb_ctx,
                                 const bool has_word0,
                                 const bool has_words12)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_STREAM_OUT0) stream_out0;
   struct PVRX(TA_STATE_STREAM_OUT1) stream_out1;
   struct PVRX(TA_STATE_STREAM_OUT2) stream_out2;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_STREAM_OUT"))
      goto end_out;

   if (has_word0) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_STREAM_OUT0,
                                          &stream_out0)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_bool(base_ctx, &stream_out0, stream0_ta_output);
      pvr_dump_field_member_bool(base_ctx, &stream_out0, stream0_mem_output);
      pvr_dump_field_member_u32_units(base_ctx,
                                      &stream_out0,
                                      stream1_size,
                                      "words");
      pvr_dump_field_member_u32_units(base_ctx,
                                      &stream_out0,
                                      stream2_size,
                                      "words");
      pvr_dump_field_member_u32_units(base_ctx,
                                      &stream_out0,
                                      stream3_size,
                                      "words");
   } else {
      pvr_dump_field_member_not_present(base_ctx,
                                        &stream_out0,
                                        stream0_ta_output);
      pvr_dump_field_member_not_present(base_ctx,
                                        &stream_out0,
                                        stream0_mem_output);
      pvr_dump_field_member_not_present(base_ctx, &stream_out0, stream1_size);
      pvr_dump_field_member_not_present(base_ctx, &stream_out0, stream2_size);
      pvr_dump_field_member_not_present(base_ctx, &stream_out0, stream3_size);
   }

   if (has_words12) {
      if (!pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_STREAM_OUT1,
                                          &stream_out1) ||
          !pvr_dump_csb_block_take_packed(&ctx,
                                          TA_STATE_STREAM_OUT2,
                                          &stream_out2)) {
         goto end_pop_ctx;
      }

      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &stream_out1,
         pds_temp_size,
         PVRX(TA_STATE_STREAM_OUT1_PDS_TEMP_SIZE_UNIT_SIZE),
         "bytes");
      pvr_dump_field_member_u32_scaled_units(
         base_ctx,
         &stream_out1,
         pds_data_size,
         PVRX(TA_STATE_STREAM_OUT1_PDS_DATA_SIZE_UNIT_SIZE),
         "bytes");
      pvr_dump_field_member_bool(base_ctx, &stream_out1, sync);
      pvr_dump_field_member_addr(base_ctx, &stream_out2, pds_data_addr);
   } else {
      pvr_dump_field_member_not_present(base_ctx, &stream_out1, pds_temp_size);
      pvr_dump_field_member_not_present(base_ctx, &stream_out1, pds_data_size);
      pvr_dump_field_member_not_present(base_ctx, &stream_out1, sync);
      pvr_dump_field_member_not_present(base_ctx, &stream_out2, pds_data_addr);
   }

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

static bool
print_block_ppp_state_terminate(struct pvr_dump_csb_ctx *const csb_ctx)
{
   struct pvr_dump_csb_block_ctx ctx;
   struct pvr_dump_ctx *const base_ctx = &ctx.base.base;
   bool ret = false;

   struct PVRX(TA_STATE_TERMINATE0) terminate0;
   struct PVRX(TA_STATE_TERMINATE1) terminate1;

   if (!pvr_dump_csb_block_ctx_push(&ctx, csb_ctx, "STATE_TERMINATE"))
      goto end_out;

   if (!pvr_dump_csb_block_take_packed(&ctx, TA_STATE_TERMINATE0, &terminate0) ||
       !pvr_dump_csb_block_take_packed(&ctx, TA_STATE_TERMINATE1, &terminate1)) {
      goto end_pop_ctx;
   }

   pvr_dump_field_member_u32_scaled_units(base_ctx,
                                          &terminate0,
                                          clip_right,
                                          32,
                                          "pixels");
   pvr_dump_field_member_u32_scaled_units(base_ctx,
                                          &terminate0,
                                          clip_top,
                                          32,
                                          "pixels");
   pvr_dump_field_member_u32_scaled_units(base_ctx,
                                          &terminate0,
                                          clip_bottom,
                                          32,
                                          "pixels");
   pvr_dump_field_member_u32_scaled_units(base_ctx,
                                          &terminate1,
                                          clip_left,
                                          32,
                                          "pixels");
   pvr_dump_field_member_u32(base_ctx, &terminate1, render_target);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_block_ctx_pop(&ctx);

end_out:
   return ret;
}

/******************************************************************************
   Buffer printers
 ******************************************************************************/

static bool print_cdmctrl_buffer(struct pvr_dump_buffer_ctx *const parent_ctx)
{
   struct pvr_dump_csb_ctx ctx;
   bool ret = true;

   /* All blocks contain a block_type member in the first word at the same
    * position. We could unpack any block to pick out this discriminant field,
    * but this one has been chosen because it's only one word long.
    */
   STATIC_ASSERT(pvr_cmd_length(CDMCTRL_STREAM_TERMINATE) == 1);

   if (!pvr_dump_csb_ctx_push(&ctx, parent_ctx))
      return false;

   do {
      enum PVRX(CDMCTRL_BLOCK_TYPE) block_type;
      const uint32_t *next_word;

      next_word = pvr_dump_buffer_peek(&ctx.base, sizeof(*next_word));
      if (!next_word) {
         ret = false;
         goto end_pop_ctx;
      }

      block_type =
         pvr_csb_unpack(next_word, CDMCTRL_STREAM_TERMINATE).block_type;
      switch (block_type) {
      case PVRX(CDMCTRL_BLOCK_TYPE_COMPUTE_KERNEL):
         ret = print_block_cdmctrl_kernel(&ctx);
         break;

      case PVRX(CDMCTRL_BLOCK_TYPE_STREAM_LINK):
         ret = print_block_cdmctrl_stream_link(&ctx);
         break;

      case PVRX(CDMCTRL_BLOCK_TYPE_STREAM_TERMINATE):
         ret = print_block_cdmctrl_stream_terminate(&ctx);
         break;

      default:
         pvr_dump_buffer_print_header_line(
            &ctx.base,
            "<could not decode CDMCTRL block (%u)>",
            block_type);
         ret = false;
         break;
      }

      if (block_type == PVRX(CDMCTRL_BLOCK_TYPE_STREAM_TERMINATE))
         break;
   } while (ret);

end_pop_ctx:
   pvr_dump_csb_ctx_pop(&ctx, true);

   return ret;
}

static bool print_vdmctrl_buffer(struct pvr_dump_buffer_ctx *const parent_ctx,
                                 struct pvr_device *const device)
{
   struct pvr_dump_csb_ctx ctx;
   bool ret = true;

   /* All blocks contain a block_type member in the first word at the same
    * position. We could unpack any block to pick out this discriminant field,
    * but this one has been chosen because it's only one word long.
    */
   STATIC_ASSERT(pvr_cmd_length(VDMCTRL_STREAM_RETURN) == 1);

   if (!pvr_dump_csb_ctx_push(&ctx, parent_ctx))
      return false;

   do {
      enum PVRX(VDMCTRL_BLOCK_TYPE) block_type;
      const uint32_t *next_word;

      next_word = pvr_dump_buffer_peek(&ctx.base, sizeof(*next_word));
      if (!next_word) {
         ret = false;
         goto end_pop_ctx;
      }

      block_type = pvr_csb_unpack(next_word, VDMCTRL_STREAM_RETURN).block_type;
      switch (block_type) {
      case PVRX(VDMCTRL_BLOCK_TYPE_PPP_STATE_UPDATE):
         ret = print_block_vdmctrl_ppp_state_update(&ctx, device);
         break;

      case PVRX(VDMCTRL_BLOCK_TYPE_PDS_STATE_UPDATE):
         ret = print_block_vdmctrl_pds_state_update(&ctx);
         break;

      case PVRX(VDMCTRL_BLOCK_TYPE_VDM_STATE_UPDATE):
         ret = print_block_vdmctrl_vdm_state_update(&ctx);
         break;

      case PVRX(VDMCTRL_BLOCK_TYPE_INDEX_LIST):
         ret = print_block_vdmctrl_index_list(&ctx, &device->pdevice->dev_info);
         break;

      case PVRX(VDMCTRL_BLOCK_TYPE_STREAM_LINK):
         ret = print_block_vdmctrl_stream_link(&ctx);
         break;

      case PVRX(VDMCTRL_BLOCK_TYPE_STREAM_RETURN):
         ret = print_block_vdmctrl_stream_return(&ctx);
         break;

      case PVRX(VDMCTRL_BLOCK_TYPE_STREAM_TERMINATE):
         ret = print_block_vdmctrl_stream_terminate(&ctx);
         break;

      default:
         pvr_dump_buffer_print_header_line(
            &ctx.base,
            "<could not decode VDMCTRL block (%u)>",
            block_type);
         ret = false;
         break;
      }

      if (block_type == PVRX(VDMCTRL_BLOCK_TYPE_STREAM_TERMINATE))
         break;
   } while (ret);

end_pop_ctx:
   pvr_dump_csb_ctx_pop(&ctx, true);

   return ret;
}

static bool print_ppp_buffer(struct pvr_dump_buffer_ctx *const parent_ctx,
                             const struct pvr_device_info *const dev_info)
{
   struct pvr_dump_csb_ctx ctx;
   bool ret = false;

   struct PVRX(TA_STATE_HEADER) header = { 0 };

   if (!pvr_dump_csb_ctx_push(&ctx, parent_ctx))
      goto end_out;

   if (!print_block_ppp_state_header(&ctx, &header))
      goto end_pop_ctx;

   if (header.pres_ispctl_fa || header.pres_ispctl_fb ||
       header.pres_ispctl_ba || header.pres_ispctl_bb ||
       header.pres_ispctl_dbsc) {
      if (!header.pres_ispctl) {
         ret =
            pvr_dump_field_error(&ctx.base.base, "missing ispctl control word");
         goto end_pop_ctx;
      }

      print_block_ppp_state_isp(&ctx);
   }

   if (header.pres_pds_state_ptr0 || header.pres_pds_state_ptr1 ||
       header.pres_pds_state_ptr2 || header.pres_pds_state_ptr3) {
      print_block_ppp_state_pds(&ctx,
                                header.pres_pds_state_ptr0,
                                header.pres_pds_state_ptr1,
                                header.pres_pds_state_ptr2,
                                header.pres_pds_state_ptr3);
   }

   if (header.pres_region_clip)
      print_block_ppp_region_clip(&ctx);

   if (header.pres_viewport) {
      for (uint32_t i = 0; i < header.view_port_count + 1; i++)
         print_block_ppp_viewport(&ctx, i);
   }

   if (header.pres_wclamp)
      print_block_ppp_wclamp(&ctx);

   if (header.pres_outselects)
      print_block_ppp_output_sel(&ctx);

   if (header.pres_varying_word0 || header.pres_varying_word1 ||
       header.pres_varying_word2) {
      print_block_ppp_state_varying(&ctx,
                                    header.pres_varying_word0,
                                    header.pres_varying_word1,
                                    header.pres_varying_word2);
   }

   if (header.pres_ppp_ctrl)
      print_block_ppp_state_ppp_ctrl(&ctx);

   if (header.pres_stream_out_size || header.pres_stream_out_program) {
      print_block_ppp_state_stream_out(&ctx,
                                       header.pres_stream_out_size,
                                       header.pres_stream_out_program);
   }

   if (header.pres_terminate)
      print_block_ppp_state_terminate(&ctx);

   ret = true;

end_pop_ctx:
   pvr_dump_csb_ctx_pop(&ctx, true);

end_out:
   return ret;
}

/******************************************************************************
   Sub buffer printer definition
 ******************************************************************************/

static bool print_sub_buffer(struct pvr_dump_ctx *const ctx,
                             struct pvr_device *const device,
                             const enum buffer_type type,
                             const pvr_dev_addr_t addr,
                             const uint64_t expected_size,
                             const char *const size_src)
{
   struct pvr_dump_bo_ctx sub_ctx;
   struct pvr_dump_ctx *base_ctx;
   struct pvr_bo *bo;
   uint64_t real_size;
   uint64_t offset;
   bool ret = false;

   pvr_dump_indent(ctx);

   bo = pvr_bo_store_lookup(device, addr);
   if (!bo) {
      pvr_dump_println(ctx, "<buffer does not exist>");
      goto end_out;
   }

   offset = addr.addr - bo->vma->dev_addr.addr;

   if (!pvr_dump_bo_ctx_push(&sub_ctx, ctx, device, bo)) {
      pvr_dump_println(&sub_ctx.base.base, "<unable to read buffer>");
      goto end_out;
   }

   base_ctx = &sub_ctx.base.base;

   if (!pvr_dump_buffer_advance(&sub_ctx.base, offset))
      goto end_pop_ctx;

   real_size = sub_ctx.base.remaining_size;

   if (!expected_size) {
      pvr_dump_field(base_ctx,
                     "<buffer size>",
                     "%" PRIu64 " words (%" PRIu64 " bytes) mapped",
                     real_size,
                     real_size * PVR_DUMP_CSB_WORD_SIZE);
   } else if (expected_size > real_size) {
      pvr_dump_field(base_ctx,
                     "<buffer size>",
                     "%" PRIu64 " (%" PRIu64 " bytes) mapped, expected %" PRIu64
                     " (%" PRIu64 " bytes) from %s",
                     real_size,
                     real_size * PVR_DUMP_CSB_WORD_SIZE,
                     expected_size,
                     expected_size * PVR_DUMP_CSB_WORD_SIZE,
                     size_src);
   } else {
      pvr_dump_field(base_ctx,
                     "<buffer size>",
                     "%" PRIu64 " (%" PRIu64 " bytes; from %s)",
                     expected_size,
                     expected_size * PVR_DUMP_CSB_WORD_SIZE,
                     size_src);
      pvr_dump_buffer_truncate(&sub_ctx.base,
                               expected_size * PVR_DUMP_CSB_WORD_SIZE);
   }

   if (sub_ctx.bo_mapped_in_ctx)
      pvr_dump_field(base_ctx, "<host addr>", "<unmapped>");
   else
      pvr_dump_field(base_ctx, "<host addr>", "%p", sub_ctx.base.ptr);

   switch (type) {
   case BUFFER_TYPE_NONE:
      pvr_dump_field(base_ctx, "<content>", "<not decoded>");
      ret = true;
      break;

   case BUFFER_TYPE_PPP:
      pvr_dump_field(base_ctx, "<content>", "<decoded as PPP>");
      ret = print_ppp_buffer(&sub_ctx.base, &device->pdevice->dev_info);
      break;

   default:
      pvr_dump_field(base_ctx, "<content>", "<unsupported format>");
      goto end_pop_ctx;
   }

end_pop_ctx:
   pvr_dump_bo_ctx_pop(&sub_ctx);

end_out:
   pvr_dump_dedent(ctx);

   return ret;
}

/******************************************************************************
   Top-level dumping
 ******************************************************************************/

static bool dump_first_buffer(struct pvr_dump_buffer_ctx *const ctx,
                              const enum pvr_cmd_stream_type stream_type,
                              struct pvr_device *const device)
{
   bool ret = false;

   pvr_dump_mark_section(&ctx->base, "First buffer content");
   switch (stream_type) {
   case PVR_CMD_STREAM_TYPE_GRAPHICS:
      ret = print_vdmctrl_buffer(ctx, device);
      break;

   case PVR_CMD_STREAM_TYPE_COMPUTE:
      ret = print_cdmctrl_buffer(ctx);
      break;

   default:
      unreachable("Unknown stream type");
   }

   if (!ret)
      pvr_dump_println(&ctx->base,
                       "<error while decoding at 0x%tx>",
                       ctx->ptr - ctx->initial_ptr);

   pvr_dump_buffer_restart(ctx);
   pvr_dump_mark_section(&ctx->base, "First buffer hexdump");
   return pvr_dump_buffer_hex(ctx, 0);
}

/******************************************************************************
   Public functions
 ******************************************************************************/

void pvr_csb_dump(const struct pvr_csb *const csb,
                  const uint32_t frame_num,
                  const uint32_t job_num)
{
   const uint32_t nr_bos = list_length(&csb->pvr_bo_list);
   struct pvr_device *const device = csb->device;

   struct pvr_dump_bo_ctx first_bo_ctx;
   struct pvr_dump_ctx root_ctx;

   pvr_bo_store_dump(device);

   pvr_dump_begin(&root_ctx, stderr, "CONTROL STREAM DUMP", 6);

   pvr_dump_field_u32(&root_ctx, "Frame num", frame_num);
   pvr_dump_field_u32(&root_ctx, "Job num", job_num);
   pvr_dump_field_enum(&root_ctx, "Status", csb->status, vk_Result_to_str);
   pvr_dump_field_enum(&root_ctx,
                       "Stream type",
                       csb->stream_type,
                       pvr_cmd_stream_type_to_str);

   if (nr_bos <= 1) {
      pvr_dump_field_u32(&root_ctx, "Nr of BOs", nr_bos);
   } else {
      /* TODO: Implement multi-buffer dumping. */
      pvr_dump_field_computed(&root_ctx,
                              "Nr of BOs",
                              "%" PRIu32,
                              "only the first buffer will be dumped",
                              nr_bos);
   }

   if (nr_bos == 0)
      goto end_dump;

   pvr_dump_mark_section(&root_ctx, "Buffer objects");
   pvr_bo_list_dump(&root_ctx, &csb->pvr_bo_list, nr_bos);

   if (!pvr_dump_bo_ctx_push(
          &first_bo_ctx,
          &root_ctx,
          device,
          list_first_entry(&csb->pvr_bo_list, struct pvr_bo, link))) {
      pvr_dump_mark_section(&root_ctx, "First buffer");
      pvr_dump_println(&root_ctx, "<unable to read buffer>");
      goto end_dump;
   }

   dump_first_buffer(&first_bo_ctx.base, csb->stream_type, device);

   pvr_dump_bo_ctx_pop(&first_bo_ctx);

end_dump:
   pvr_dump_end(&root_ctx);
}
