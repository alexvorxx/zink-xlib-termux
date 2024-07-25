/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_cmd_buffer.h"
#include "meta/radv_meta.h"
#include "radv_cp_dma.h"
#include "radv_cs.h"
#include "radv_debug.h"
#include "radv_device_generated_commands.h"
#include "radv_event.h"
#include "radv_pipeline_rt.h"
#include "radv_radeon_winsys.h"
#include "radv_rmv.h"
#include "radv_rra.h"
#include "radv_shader.h"
#include "radv_shader_object.h"
#include "radv_sqtt.h"
#include "sid.h"
#include "vk_command_pool.h"
#include "vk_common_entrypoints.h"
#include "vk_enum_defines.h"
#include "vk_format.h"
#include "vk_framebuffer.h"
#include "vk_render_pass.h"
#include "vk_synchronization.h"
#include "vk_util.h"

#include "ac_debug.h"
#include "ac_descriptors.h"
#include "ac_nir.h"
#include "ac_shader_args.h"

#include "aco_interface.h"

#include "util/fast_idiv_by_const.h"

enum {
   RADV_PREFETCH_VBO_DESCRIPTORS = (1 << 0),
   RADV_PREFETCH_VS = (1 << 1),
   RADV_PREFETCH_TCS = (1 << 2),
   RADV_PREFETCH_TES = (1 << 3),
   RADV_PREFETCH_GS = (1 << 4),
   RADV_PREFETCH_PS = (1 << 5),
   RADV_PREFETCH_MS = (1 << 6),
   RADV_PREFETCH_SHADERS = (RADV_PREFETCH_VS | RADV_PREFETCH_TCS | RADV_PREFETCH_TES | RADV_PREFETCH_GS |
                            RADV_PREFETCH_PS | RADV_PREFETCH_MS)
};

static void radv_handle_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                         VkImageLayout src_layout, VkImageLayout dst_layout, uint32_t src_family_index,
                                         uint32_t dst_family_index, const VkImageSubresourceRange *range,
                                         struct radv_sample_locations_state *sample_locs);

static void
radv_bind_dynamic_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_dynamic_state *src)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint64_t copy_mask = src->mask;
   uint64_t dest_mask = 0;

   dest->vk.dr.rectangle_count = src->vk.dr.rectangle_count;
   dest->sample_location.count = src->sample_location.count;

   if (copy_mask & RADV_DYNAMIC_VIEWPORT) {
      if (dest->vk.vp.viewport_count != src->vk.vp.viewport_count) {
         dest->vk.vp.viewport_count = src->vk.vp.viewport_count;
         dest_mask |= RADV_DYNAMIC_VIEWPORT;
      }

      if (memcmp(&dest->vk.vp.viewports, &src->vk.vp.viewports, src->vk.vp.viewport_count * sizeof(VkViewport))) {
         typed_memcpy(dest->vk.vp.viewports, src->vk.vp.viewports, src->vk.vp.viewport_count);
         typed_memcpy(dest->hw_vp.xform, src->hw_vp.xform, src->vk.vp.viewport_count);
         dest_mask |= RADV_DYNAMIC_VIEWPORT;
      }
   }

   if (copy_mask & RADV_DYNAMIC_SCISSOR) {
      if (dest->vk.vp.scissor_count != src->vk.vp.scissor_count) {
         dest->vk.vp.scissor_count = src->vk.vp.scissor_count;
         dest_mask |= RADV_DYNAMIC_SCISSOR;
      }

      if (memcmp(&dest->vk.vp.scissors, &src->vk.vp.scissors, src->vk.vp.scissor_count * sizeof(VkRect2D))) {
         typed_memcpy(dest->vk.vp.scissors, src->vk.vp.scissors, src->vk.vp.scissor_count);
         dest_mask |= RADV_DYNAMIC_SCISSOR;
      }
   }

   if (copy_mask & RADV_DYNAMIC_BLEND_CONSTANTS) {
      if (memcmp(&dest->vk.cb.blend_constants, &src->vk.cb.blend_constants, sizeof(src->vk.cb.blend_constants))) {
         typed_memcpy(dest->vk.cb.blend_constants, src->vk.cb.blend_constants, 4);
         dest_mask |= RADV_DYNAMIC_BLEND_CONSTANTS;
      }
   }

   if (copy_mask & RADV_DYNAMIC_DISCARD_RECTANGLE) {
      if (memcmp(&dest->vk.dr.rectangles, &src->vk.dr.rectangles, src->vk.dr.rectangle_count * sizeof(VkRect2D))) {
         typed_memcpy(dest->vk.dr.rectangles, src->vk.dr.rectangles, src->vk.dr.rectangle_count);
         dest_mask |= RADV_DYNAMIC_DISCARD_RECTANGLE;
      }
   }

   if (copy_mask & RADV_DYNAMIC_SAMPLE_LOCATIONS) {
      if (dest->sample_location.per_pixel != src->sample_location.per_pixel ||
          dest->sample_location.grid_size.width != src->sample_location.grid_size.width ||
          dest->sample_location.grid_size.height != src->sample_location.grid_size.height ||
          memcmp(&dest->sample_location.locations, &src->sample_location.locations,
                 src->sample_location.count * sizeof(VkSampleLocationEXT))) {
         dest->sample_location.per_pixel = src->sample_location.per_pixel;
         dest->sample_location.grid_size = src->sample_location.grid_size;
         typed_memcpy(dest->sample_location.locations, src->sample_location.locations, src->sample_location.count);
         dest_mask |= RADV_DYNAMIC_SAMPLE_LOCATIONS;
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_WRITE_MASK) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (dest->vk.cb.attachments[i].write_mask != src->vk.cb.attachments[i].write_mask) {
            dest->vk.cb.attachments[i].write_mask = src->vk.cb.attachments[i].write_mask;
            dest_mask |= RADV_DYNAMIC_COLOR_WRITE_MASK;
         }
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_BLEND_ENABLE) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (dest->vk.cb.attachments[i].blend_enable != src->vk.cb.attachments[i].blend_enable) {
            dest->vk.cb.attachments[i].blend_enable = src->vk.cb.attachments[i].blend_enable;
            dest_mask |= RADV_DYNAMIC_COLOR_BLEND_ENABLE;
         }
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_BLEND_EQUATION) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (dest->vk.cb.attachments[i].src_color_blend_factor != src->vk.cb.attachments[i].src_color_blend_factor ||
             dest->vk.cb.attachments[i].dst_color_blend_factor != src->vk.cb.attachments[i].dst_color_blend_factor ||
             dest->vk.cb.attachments[i].color_blend_op != src->vk.cb.attachments[i].color_blend_op ||
             dest->vk.cb.attachments[i].src_alpha_blend_factor != src->vk.cb.attachments[i].src_alpha_blend_factor ||
             dest->vk.cb.attachments[i].dst_alpha_blend_factor != src->vk.cb.attachments[i].dst_alpha_blend_factor ||
             dest->vk.cb.attachments[i].alpha_blend_op != src->vk.cb.attachments[i].alpha_blend_op) {
            dest->vk.cb.attachments[i].src_color_blend_factor = src->vk.cb.attachments[i].src_color_blend_factor;
            dest->vk.cb.attachments[i].dst_color_blend_factor = src->vk.cb.attachments[i].dst_color_blend_factor;
            dest->vk.cb.attachments[i].color_blend_op = src->vk.cb.attachments[i].color_blend_op;
            dest->vk.cb.attachments[i].src_alpha_blend_factor = src->vk.cb.attachments[i].src_alpha_blend_factor;
            dest->vk.cb.attachments[i].dst_alpha_blend_factor = src->vk.cb.attachments[i].dst_alpha_blend_factor;
            dest->vk.cb.attachments[i].alpha_blend_op = src->vk.cb.attachments[i].alpha_blend_op;
            dest_mask |= RADV_DYNAMIC_COLOR_BLEND_EQUATION;
         }
      }
   }

   if (memcmp(&dest->vk.cal.color_map, &src->vk.cal.color_map, sizeof(src->vk.cal.color_map))) {
      typed_memcpy(dest->vk.cal.color_map, src->vk.cal.color_map, MAX_RTS);
      dest_mask |= RADV_DYNAMIC_COLOR_ATTACHMENT_MAP;
   }

#define RADV_CMP_COPY(field, flag)                                                                                     \
   if (copy_mask & flag) {                                                                                             \
      if (dest->field != src->field) {                                                                                 \
         dest->field = src->field;                                                                                     \
         dest_mask |= flag;                                                                                            \
      }                                                                                                                \
   }

   RADV_CMP_COPY(vk.ia.primitive_topology, RADV_DYNAMIC_PRIMITIVE_TOPOLOGY);
   RADV_CMP_COPY(vk.ia.primitive_restart_enable, RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE);

   RADV_CMP_COPY(vk.vp.depth_clip_negative_one_to_one, RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE);

   RADV_CMP_COPY(vk.ts.patch_control_points, RADV_DYNAMIC_PATCH_CONTROL_POINTS);
   RADV_CMP_COPY(vk.ts.domain_origin, RADV_DYNAMIC_TESS_DOMAIN_ORIGIN);

   RADV_CMP_COPY(vk.rs.line.width, RADV_DYNAMIC_LINE_WIDTH);
   RADV_CMP_COPY(vk.rs.depth_bias.constant, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.depth_bias.clamp, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.depth_bias.slope, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.depth_bias.representation, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.line.stipple.factor, RADV_DYNAMIC_LINE_STIPPLE);
   RADV_CMP_COPY(vk.rs.line.stipple.pattern, RADV_DYNAMIC_LINE_STIPPLE);
   RADV_CMP_COPY(vk.rs.cull_mode, RADV_DYNAMIC_CULL_MODE);
   RADV_CMP_COPY(vk.rs.front_face, RADV_DYNAMIC_FRONT_FACE);
   RADV_CMP_COPY(vk.rs.depth_bias.enable, RADV_DYNAMIC_DEPTH_BIAS_ENABLE);
   RADV_CMP_COPY(vk.rs.rasterizer_discard_enable, RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE);
   RADV_CMP_COPY(vk.rs.polygon_mode, RADV_DYNAMIC_POLYGON_MODE);
   RADV_CMP_COPY(vk.rs.line.stipple.enable, RADV_DYNAMIC_LINE_STIPPLE_ENABLE);
   RADV_CMP_COPY(vk.rs.depth_clip_enable, RADV_DYNAMIC_DEPTH_CLIP_ENABLE);
   RADV_CMP_COPY(vk.rs.conservative_mode, RADV_DYNAMIC_CONSERVATIVE_RAST_MODE);
   RADV_CMP_COPY(vk.rs.provoking_vertex, RADV_DYNAMIC_PROVOKING_VERTEX_MODE);
   RADV_CMP_COPY(vk.rs.depth_clamp_enable, RADV_DYNAMIC_DEPTH_CLAMP_ENABLE);
   RADV_CMP_COPY(vk.rs.line.mode, RADV_DYNAMIC_LINE_RASTERIZATION_MODE);

   RADV_CMP_COPY(vk.ms.alpha_to_coverage_enable, RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE);
   RADV_CMP_COPY(vk.ms.alpha_to_one_enable, RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE);
   RADV_CMP_COPY(vk.ms.sample_mask, RADV_DYNAMIC_SAMPLE_MASK);
   RADV_CMP_COPY(vk.ms.rasterization_samples, RADV_DYNAMIC_RASTERIZATION_SAMPLES);
   RADV_CMP_COPY(vk.ms.sample_locations_enable, RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE);

   RADV_CMP_COPY(vk.ds.depth.bounds_test.min, RADV_DYNAMIC_DEPTH_BOUNDS);
   RADV_CMP_COPY(vk.ds.depth.bounds_test.max, RADV_DYNAMIC_DEPTH_BOUNDS);
   RADV_CMP_COPY(vk.ds.stencil.front.compare_mask, RADV_DYNAMIC_STENCIL_COMPARE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.back.compare_mask, RADV_DYNAMIC_STENCIL_COMPARE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.front.write_mask, RADV_DYNAMIC_STENCIL_WRITE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.back.write_mask, RADV_DYNAMIC_STENCIL_WRITE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.front.reference, RADV_DYNAMIC_STENCIL_REFERENCE);
   RADV_CMP_COPY(vk.ds.stencil.back.reference, RADV_DYNAMIC_STENCIL_REFERENCE);
   RADV_CMP_COPY(vk.ds.depth.test_enable, RADV_DYNAMIC_DEPTH_TEST_ENABLE);
   RADV_CMP_COPY(vk.ds.depth.write_enable, RADV_DYNAMIC_DEPTH_WRITE_ENABLE);
   RADV_CMP_COPY(vk.ds.depth.compare_op, RADV_DYNAMIC_DEPTH_COMPARE_OP);
   RADV_CMP_COPY(vk.ds.depth.bounds_test.enable, RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE);
   RADV_CMP_COPY(vk.ds.stencil.test_enable, RADV_DYNAMIC_STENCIL_TEST_ENABLE);
   RADV_CMP_COPY(vk.ds.stencil.front.op.fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.front.op.pass, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.front.op.depth_fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.front.op.compare, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.pass, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.depth_fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.compare, RADV_DYNAMIC_STENCIL_OP);

   RADV_CMP_COPY(vk.cb.logic_op, RADV_DYNAMIC_LOGIC_OP);
   RADV_CMP_COPY(vk.cb.color_write_enables, RADV_DYNAMIC_COLOR_WRITE_ENABLE);
   RADV_CMP_COPY(vk.cb.logic_op_enable, RADV_DYNAMIC_LOGIC_OP_ENABLE);

   RADV_CMP_COPY(vk.fsr.fragment_size.width, RADV_DYNAMIC_FRAGMENT_SHADING_RATE);
   RADV_CMP_COPY(vk.fsr.fragment_size.height, RADV_DYNAMIC_FRAGMENT_SHADING_RATE);
   RADV_CMP_COPY(vk.fsr.combiner_ops[0], RADV_DYNAMIC_FRAGMENT_SHADING_RATE);
   RADV_CMP_COPY(vk.fsr.combiner_ops[1], RADV_DYNAMIC_FRAGMENT_SHADING_RATE);

   RADV_CMP_COPY(vk.dr.enable, RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE);
   RADV_CMP_COPY(vk.dr.mode, RADV_DYNAMIC_DISCARD_RECTANGLE_MODE);

   RADV_CMP_COPY(feedback_loop_aspects, RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE);

#undef RADV_CMP_COPY

   cmd_buffer->state.dirty_dynamic |= dest_mask;

   /* Handle driver specific states that need to be re-emitted when PSO are bound. */
   if (dest_mask & (RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_POLYGON_MODE | RADV_DYNAMIC_LINE_WIDTH |
                    RADV_DYNAMIC_PRIMITIVE_TOPOLOGY)) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND;
   }

   if (pdev->info.rbplus_allowed && (dest_mask & RADV_DYNAMIC_COLOR_WRITE_MASK)) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
   }
}

bool
radv_cmd_buffer_uses_mec(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return cmd_buffer->qf == RADV_QUEUE_COMPUTE && pdev->info.gfx_level >= GFX7;
}

static void
radv_write_data(struct radv_cmd_buffer *cmd_buffer, const unsigned engine_sel, const uint64_t va, const unsigned count,
                const uint32_t *data, const bool predicating)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   radv_cs_write_data(device, cmd_buffer->cs, cmd_buffer->qf, engine_sel, va, count, data, predicating);
}

static void
radv_emit_clear_data(struct radv_cmd_buffer *cmd_buffer, unsigned engine_sel, uint64_t va, unsigned size)
{
   uint32_t *zeroes = alloca(size);
   memset(zeroes, 0, size);
   radv_write_data(cmd_buffer, engine_sel, va, size / 4, zeroes, false);
}

static void
radv_cmd_buffer_finish_shader_part_cache(struct radv_cmd_buffer *cmd_buffer)
{
   ralloc_free(cmd_buffer->vs_prologs.table);
   ralloc_free(cmd_buffer->ps_epilogs.table);
}

static bool
radv_cmd_buffer_init_shader_part_cache(struct radv_device *device, struct radv_cmd_buffer *cmd_buffer)
{
   if (device->vs_prologs.ops) {
      if (!_mesa_set_init(&cmd_buffer->vs_prologs, NULL, device->vs_prologs.ops->hash, device->vs_prologs.ops->equals))
         return false;
   }
   if (device->ps_epilogs.ops) {
      if (!_mesa_set_init(&cmd_buffer->ps_epilogs, NULL, device->ps_epilogs.ops->hash, device->ps_epilogs.ops->equals))
         return false;
   }
   return true;
}

static void
radv_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct radv_cmd_buffer *cmd_buffer = container_of(vk_cmd_buffer, struct radv_cmd_buffer, vk);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (cmd_buffer->qf != RADV_QUEUE_SPARSE) {
      util_dynarray_fini(&cmd_buffer->ray_history);

      radv_rra_accel_struct_buffers_unref(device, cmd_buffer->accel_struct_buffers);
      _mesa_set_destroy(cmd_buffer->accel_struct_buffers, NULL);

      list_for_each_entry_safe (struct radv_cmd_buffer_upload, up, &cmd_buffer->upload.list, list) {
         radv_rmv_log_command_buffer_bo_destroy(device, up->upload_bo);
         radv_bo_destroy(device, &cmd_buffer->vk.base, up->upload_bo);
         list_del(&up->list);
         free(up);
      }

      if (cmd_buffer->upload.upload_bo) {
         radv_rmv_log_command_buffer_bo_destroy(device, cmd_buffer->upload.upload_bo);
         radv_bo_destroy(device, &cmd_buffer->vk.base, cmd_buffer->upload.upload_bo);
      }

      if (cmd_buffer->cs)
         device->ws->cs_destroy(cmd_buffer->cs);
      if (cmd_buffer->gang.cs)
         device->ws->cs_destroy(cmd_buffer->gang.cs);
      if (cmd_buffer->transfer.copy_temp)
         radv_bo_destroy(device, &cmd_buffer->vk.base, cmd_buffer->transfer.copy_temp);

      radv_cmd_buffer_finish_shader_part_cache(cmd_buffer);

      for (unsigned i = 0; i < MAX_BIND_POINTS; i++) {
         struct radv_descriptor_set_header *set = &cmd_buffer->descriptors[i].push_set.set;
         free(set->mapped_ptr);
         if (set->layout)
            vk_descriptor_set_layout_unref(&device->vk, &set->layout->vk);
         vk_object_base_finish(&set->base);
      }

      vk_object_base_finish(&cmd_buffer->meta_push_descriptors.base);
   }

   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
}

static VkResult
radv_create_cmd_buffer(struct vk_command_pool *pool, VkCommandBufferLevel level,
                       struct vk_command_buffer **cmd_buffer_out)
{
   struct radv_device *device = container_of(pool->base.device, struct radv_device, vk);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_buffer *cmd_buffer;
   unsigned ring;
   cmd_buffer = vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_buffer_init(pool, &cmd_buffer->vk, &radv_cmd_buffer_ops, level);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
      return result;
   }

   cmd_buffer->qf = vk_queue_to_radv(pdev, pool->queue_family_index);

   if (cmd_buffer->qf != RADV_QUEUE_SPARSE) {
      list_inithead(&cmd_buffer->upload.list);

      if (!radv_cmd_buffer_init_shader_part_cache(device, cmd_buffer)) {
         radv_destroy_cmd_buffer(&cmd_buffer->vk);
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      ring = radv_queue_family_to_ring(pdev, cmd_buffer->qf);

      cmd_buffer->cs =
         device->ws->cs_create(device->ws, ring, cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);
      if (!cmd_buffer->cs) {
         radv_destroy_cmd_buffer(&cmd_buffer->vk);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }

      vk_object_base_init(&device->vk, &cmd_buffer->meta_push_descriptors.base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

      for (unsigned i = 0; i < MAX_BIND_POINTS; i++)
         vk_object_base_init(&device->vk, &cmd_buffer->descriptors[i].push_set.set.base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

      cmd_buffer->accel_struct_buffers = _mesa_pointer_set_create(NULL);
      util_dynarray_init(&cmd_buffer->ray_history, NULL);
   }

   *cmd_buffer_out = &cmd_buffer->vk;

   return VK_SUCCESS;
}

void
radv_cmd_buffer_reset_rendering(struct radv_cmd_buffer *cmd_buffer)
{
   memset(&cmd_buffer->state.render, 0, sizeof(cmd_buffer->state.render));
}

static void
radv_reset_tracked_regs(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_tracked_regs *tracked_regs = &cmd_buffer->tracked_regs;

   /* Mark all registers as unknown. */
   memset(tracked_regs->reg_value, 0, RADV_NUM_ALL_TRACKED_REGS * sizeof(uint32_t));
   BITSET_ZERO(tracked_regs->reg_saved_mask);

   /* 0xffffffff is an impossible value for SPI_PS_INPUT_CNTL_n registers */
   memset(tracked_regs->spi_ps_input_cntl, 0xff, sizeof(uint32_t) * 32);
}

static void
radv_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer, UNUSED VkCommandBufferResetFlags flags)
{
   struct radv_cmd_buffer *cmd_buffer = container_of(vk_cmd_buffer, struct radv_cmd_buffer, vk);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   vk_command_buffer_reset(&cmd_buffer->vk);

   if (cmd_buffer->qf == RADV_QUEUE_SPARSE)
      return;

   device->ws->cs_reset(cmd_buffer->cs);
   if (cmd_buffer->gang.cs)
      device->ws->cs_reset(cmd_buffer->gang.cs);

   list_for_each_entry_safe (struct radv_cmd_buffer_upload, up, &cmd_buffer->upload.list, list) {
      radv_rmv_log_command_buffer_bo_destroy(device, up->upload_bo);
      radv_bo_destroy(device, &cmd_buffer->vk.base, up->upload_bo);
      list_del(&up->list);
      free(up);
   }

   util_dynarray_clear(&cmd_buffer->ray_history);

   radv_rra_accel_struct_buffers_unref(device, cmd_buffer->accel_struct_buffers);

   cmd_buffer->push_constant_stages = 0;
   cmd_buffer->scratch_size_per_wave_needed = 0;
   cmd_buffer->scratch_waves_wanted = 0;
   cmd_buffer->compute_scratch_size_per_wave_needed = 0;
   cmd_buffer->compute_scratch_waves_wanted = 0;
   cmd_buffer->esgs_ring_size_needed = 0;
   cmd_buffer->gsvs_ring_size_needed = 0;
   cmd_buffer->tess_rings_needed = false;
   cmd_buffer->task_rings_needed = false;
   cmd_buffer->mesh_scratch_ring_needed = false;
   cmd_buffer->gds_needed = false;
   cmd_buffer->gds_oa_needed = false;
   cmd_buffer->sample_positions_needed = false;
   cmd_buffer->gang.sem.leader_value = 0;
   cmd_buffer->gang.sem.emitted_leader_value = 0;
   cmd_buffer->gang.sem.va = 0;
   cmd_buffer->shader_upload_seq = 0;
   cmd_buffer->has_indirect_pipeline_binds = false;

   if (cmd_buffer->upload.upload_bo)
      radv_cs_add_buffer(device->ws, cmd_buffer->cs, cmd_buffer->upload.upload_bo);
   cmd_buffer->upload.offset = 0;

   memset(cmd_buffer->vertex_binding_buffers, 0, sizeof(struct radv_buffer *) * cmd_buffer->used_vertex_bindings);
   cmd_buffer->used_vertex_bindings = 0;

   for (unsigned i = 0; i < MAX_BIND_POINTS; i++) {
      cmd_buffer->descriptors[i].dirty = 0;
      cmd_buffer->descriptors[i].valid = 0;
   }

   radv_cmd_buffer_reset_rendering(cmd_buffer);
}

const struct vk_command_buffer_ops radv_cmd_buffer_ops = {
   .create = radv_create_cmd_buffer,
   .reset = radv_reset_cmd_buffer,
   .destroy = radv_destroy_cmd_buffer,
};

static bool
radv_cmd_buffer_resize_upload_buf(struct radv_cmd_buffer *cmd_buffer, uint64_t min_needed)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint64_t new_size;
   struct radeon_winsys_bo *bo = NULL;
   struct radv_cmd_buffer_upload *upload;

   new_size = MAX2(min_needed, 16 * 1024);
   new_size = MAX2(new_size, 2 * cmd_buffer->upload.size);

   VkResult result = radv_bo_create(
      device, &cmd_buffer->vk.base, new_size, 4096, device->ws->cs_domain(device->ws),
      RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_32BIT | RADEON_FLAG_GTT_WC,
      RADV_BO_PRIORITY_UPLOAD_BUFFER, 0, true, &bo);

   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return false;
   }

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, bo);
   if (cmd_buffer->upload.upload_bo) {
      upload = malloc(sizeof(*upload));

      if (!upload) {
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         radv_bo_destroy(device, &cmd_buffer->vk.base, bo);
         return false;
      }

      memcpy(upload, &cmd_buffer->upload, sizeof(*upload));
      list_add(&upload->list, &cmd_buffer->upload.list);
   }

   cmd_buffer->upload.upload_bo = bo;
   cmd_buffer->upload.size = new_size;
   cmd_buffer->upload.offset = 0;
   cmd_buffer->upload.map = radv_buffer_map(device->ws, cmd_buffer->upload.upload_bo);

   if (!cmd_buffer->upload.map) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }

   radv_rmv_log_command_buffer_bo_create(device, cmd_buffer->upload.upload_bo, 0, cmd_buffer->upload.size, 0);

   return true;
}

bool
radv_cmd_buffer_upload_alloc_aligned(struct radv_cmd_buffer *cmd_buffer, unsigned size, unsigned alignment,
                                     unsigned *out_offset, void **ptr)
{
   assert(size % 4 == 0);

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;

   /* Align to the scalar cache line size if it results in this allocation
    * being placed in less of them.
    */
   unsigned offset = cmd_buffer->upload.offset;
   unsigned line_size = gpu_info->gfx_level >= GFX10 ? 64 : 32;
   unsigned gap = align(offset, line_size) - offset;
   if ((size & (line_size - 1)) > gap)
      offset = align(offset, line_size);

   if (alignment)
      offset = align(offset, alignment);
   if (offset + size > cmd_buffer->upload.size) {
      if (!radv_cmd_buffer_resize_upload_buf(cmd_buffer, size))
         return false;
      offset = 0;
   }

   *out_offset = offset;
   *ptr = cmd_buffer->upload.map + offset;

   cmd_buffer->upload.offset = offset + size;
   return true;
}

bool
radv_cmd_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer, unsigned size, unsigned *out_offset, void **ptr)
{
   return radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, size, 0, out_offset, ptr);
}

bool
radv_cmd_buffer_upload_data(struct radv_cmd_buffer *cmd_buffer, unsigned size, const void *data, unsigned *out_offset)
{
   uint8_t *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, out_offset, (void **)&ptr))
      return false;
   assert(ptr);

   memcpy(ptr, data, size);
   return true;
}

void
radv_cmd_buffer_trace_emit(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint64_t va;

   if (cmd_buffer->qf != RADV_QUEUE_GENERAL && cmd_buffer->qf != RADV_QUEUE_COMPUTE)
      return;

   va = radv_buffer_get_va(device->trace_bo);
   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      va += offsetof(struct radv_trace_data, primary_id);
   else
      va += offsetof(struct radv_trace_data, secondary_id);

   ++cmd_buffer->state.trace_id;
   radv_write_data(cmd_buffer, V_370_ME, va, 1, &cmd_buffer->state.trace_id, false);

   radeon_check_space(device->ws, cs, 2);

   radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
   radeon_emit(cs, AC_ENCODE_TRACE_POINT(cmd_buffer->state.trace_id));
}

void
radv_cmd_buffer_annotate(struct radv_cmd_buffer *cmd_buffer, const char *annotation)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   device->ws->cs_annotate(cmd_buffer->cs, annotation);
}

static void
radv_gang_barrier(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask,
                  VkPipelineStageFlags2 dst_stage_mask)
{
   /* Update flush bits from the main cmdbuf, except the stage flush. */
   cmd_buffer->gang.flush_bits |=
      cmd_buffer->state.flush_bits & RADV_CMD_FLUSH_ALL_COMPUTE & ~RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Add stage flush only when necessary. */
   if (src_stage_mask & (VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT |
                         VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_NV))
      cmd_buffer->gang.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Block task shaders when we have to wait for CP DMA on the GFX cmdbuf. */
   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      dst_stage_mask |= cmd_buffer->state.dma_is_busy ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT : 0;

   /* Increment the GFX/ACE semaphore when task shaders are blocked. */
   if (dst_stage_mask & (VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT))
      cmd_buffer->gang.sem.leader_value++;
}

void
radv_gang_cache_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *ace_cs = cmd_buffer->gang.cs;
   const uint32_t flush_bits = cmd_buffer->gang.flush_bits;
   enum rgp_flush_bits sqtt_flush_bits = 0;

   radv_cs_emit_cache_flush(device->ws, ace_cs, pdev->info.gfx_level, NULL, 0, RADV_QUEUE_COMPUTE, flush_bits,
                            &sqtt_flush_bits, 0);

   cmd_buffer->gang.flush_bits = 0;
}

static bool
radv_gang_sem_init(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->gang.sem.va)
      return true;

   /* DWORD 0: GFX->ACE semaphore (GFX blocks ACE, ie. ACE waits for GFX)
    * DWORD 1: ACE->GFX semaphore
    */
   uint64_t sem_init = 0;
   uint32_t va_off = 0;
   if (!radv_cmd_buffer_upload_data(cmd_buffer, sizeof(uint64_t), &sem_init, &va_off)) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return false;
   }

   cmd_buffer->gang.sem.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + va_off;
   return true;
}

static bool
radv_gang_leader_sem_dirty(const struct radv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->gang.sem.leader_value != cmd_buffer->gang.sem.emitted_leader_value;
}

static bool
radv_gang_follower_sem_dirty(const struct radv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->gang.sem.follower_value != cmd_buffer->gang.sem.emitted_follower_value;
}

ALWAYS_INLINE static bool
radv_flush_gang_semaphore(struct radv_cmd_buffer *cmd_buffer, struct radeon_cmdbuf *cs, const enum radv_queue_family qf,
                          const uint32_t va_off, const uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!radv_gang_sem_init(cmd_buffer))
      return false;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs, 12);

   radv_cs_emit_write_event_eop(cs, pdev->info.gfx_level, qf, V_028A90_BOTTOM_OF_PIPE_TS, 0, EOP_DST_SEL_MEM,
                                EOP_DATA_SEL_VALUE_32BIT, cmd_buffer->gang.sem.va + va_off, value,
                                cmd_buffer->gfx9_eop_bug_va);

   assert(cmd_buffer->cs->cdw <= cdw_max);
   return true;
}

ALWAYS_INLINE static bool
radv_flush_gang_leader_semaphore(struct radv_cmd_buffer *cmd_buffer)
{
   if (!radv_gang_leader_sem_dirty(cmd_buffer))
      return false;

   /* Gang leader writes a value to the semaphore which the follower can wait for. */
   cmd_buffer->gang.sem.emitted_leader_value = cmd_buffer->gang.sem.leader_value;
   return radv_flush_gang_semaphore(cmd_buffer, cmd_buffer->cs, cmd_buffer->qf, 0, cmd_buffer->gang.sem.leader_value);
}

ALWAYS_INLINE static bool
radv_flush_gang_follower_semaphore(struct radv_cmd_buffer *cmd_buffer)
{
   if (!radv_gang_follower_sem_dirty(cmd_buffer))
      return false;

   /* Follower writes a value to the semaphore which the gang leader can wait for. */
   cmd_buffer->gang.sem.emitted_follower_value = cmd_buffer->gang.sem.follower_value;
   return radv_flush_gang_semaphore(cmd_buffer, cmd_buffer->gang.cs, RADV_QUEUE_COMPUTE, 4,
                                    cmd_buffer->gang.sem.follower_value);
}

ALWAYS_INLINE static void
radv_wait_gang_semaphore(struct radv_cmd_buffer *cmd_buffer, struct radeon_cmdbuf *cs, const enum radv_queue_family qf,
                         const uint32_t va_off, const uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(cmd_buffer->gang.sem.va);
   radeon_check_space(device->ws, cs, 7);
   radv_cp_wait_mem(cs, qf, WAIT_REG_MEM_GREATER_OR_EQUAL, cmd_buffer->gang.sem.va + va_off, value, 0xffffffff);
}

ALWAYS_INLINE static void
radv_wait_gang_leader(struct radv_cmd_buffer *cmd_buffer)
{
   /* Follower waits for the semaphore which the gang leader wrote. */
   radv_wait_gang_semaphore(cmd_buffer, cmd_buffer->gang.cs, RADV_QUEUE_COMPUTE, 0, cmd_buffer->gang.sem.leader_value);
}

ALWAYS_INLINE static void
radv_wait_gang_follower(struct radv_cmd_buffer *cmd_buffer)
{
   /* Gang leader waits for the semaphore which the follower wrote. */
   radv_wait_gang_semaphore(cmd_buffer, cmd_buffer->cs, cmd_buffer->qf, 4, cmd_buffer->gang.sem.follower_value);
}

bool
radv_gang_init(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (cmd_buffer->gang.cs)
      return true;

   struct radeon_cmdbuf *ace_cs =
      device->ws->cs_create(device->ws, AMD_IP_COMPUTE, cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);

   if (!ace_cs) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }

   cmd_buffer->gang.cs = ace_cs;
   return true;
}

static VkResult
radv_gang_finalize(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(cmd_buffer->gang.cs);
   struct radeon_cmdbuf *ace_cs = cmd_buffer->gang.cs;

   /* Emit pending cache flush. */
   radv_gang_cache_flush(cmd_buffer);

   /* Clear the leader<->follower semaphores if they exist.
    * This is necessary in case the same cmd buffer is submitted again in the future.
    */
   if (cmd_buffer->gang.sem.va) {
      uint64_t leader2follower_va = cmd_buffer->gang.sem.va;
      uint64_t follower2leader_va = cmd_buffer->gang.sem.va + 4;
      const uint32_t zero = 0;

      /* Follower: write 0 to the leader->follower semaphore. */
      radv_cs_write_data(device, ace_cs, RADV_QUEUE_COMPUTE, V_370_ME, leader2follower_va, 1, &zero, false);

      /* Leader: write 0 to the follower->leader semaphore. */
      radv_write_data(cmd_buffer, V_370_ME, follower2leader_va, 1, &zero, false);
   }

   return device->ws->cs_finalize(ace_cs);
}

static void
radv_cmd_buffer_after_draw(struct radv_cmd_buffer *cmd_buffer, enum radv_cmd_flush_bits flags, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);

   if (unlikely(device->sqtt.bo) && !dgc) {
      radeon_check_space(device->ws, cmd_buffer->cs, 2);

      radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, cmd_buffer->state.predicating));
      radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_THREAD_TRACE_MARKER) | EVENT_INDEX(0));
   }

   if (instance->debug_flags & RADV_DEBUG_SYNC_SHADERS) {
      enum rgp_flush_bits sqtt_flush_bits = 0;
      assert(flags & (RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH));

      /* Force wait for graphics or compute engines to be idle. */
      radv_cs_emit_cache_flush(device->ws, cmd_buffer->cs, pdev->info.gfx_level, &cmd_buffer->gfx9_fence_idx,
                               cmd_buffer->gfx9_fence_va, cmd_buffer->qf, flags, &sqtt_flush_bits,
                               cmd_buffer->gfx9_eop_bug_va);

      if ((flags & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) && radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
         /* Force wait for compute engines to be idle on the internal cmdbuf. */
         radv_cs_emit_cache_flush(device->ws, cmd_buffer->gang.cs, pdev->info.gfx_level, NULL, 0, RADV_QUEUE_COMPUTE,
                                  RADV_CMD_FLAG_CS_PARTIAL_FLUSH, &sqtt_flush_bits, 0);
      }
   }

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);
}

static void
radv_save_pipeline(struct radv_cmd_buffer *cmd_buffer, struct radv_pipeline *pipeline)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   enum amd_ip_type ring;
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo);

   ring = radv_queue_family_to_ring(pdev, cmd_buffer->qf);

   switch (ring) {
   case AMD_IP_GFX:
      va += offsetof(struct radv_trace_data, gfx_ring_pipeline);
      break;
   case AMD_IP_COMPUTE:
      va += offsetof(struct radv_trace_data, comp_ring_pipeline);
      break;
   default:
      assert(!"invalid IP type");
   }

   uint64_t pipeline_address = (uintptr_t)pipeline;
   data[0] = pipeline_address;
   data[1] = pipeline_address >> 32;

   radv_write_data(cmd_buffer, V_370_ME, va, 2, data, false);
}

static void
radv_save_vertex_descriptors(struct radv_cmd_buffer *cmd_buffer, uint64_t vb_ptr)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, vertex_descriptors);

   data[0] = vb_ptr;
   data[1] = vb_ptr >> 32;

   radv_write_data(cmd_buffer, V_370_ME, va, 2, data, false);
}

static void
radv_save_vs_prolog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader_part *prolog)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, vertex_prolog);

   uint64_t prolog_address = (uintptr_t)prolog;
   data[0] = prolog_address;
   data[1] = prolog_address >> 32;

   radv_write_data(cmd_buffer, V_370_ME, va, 2, data, false);
}

void
radv_set_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                        struct radv_descriptor_set *set, unsigned idx)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->sets[idx] = set;

   descriptors_state->valid |= (1u << idx); /* active descriptors */
   descriptors_state->dirty |= (1u << idx);
}

static void
radv_save_descriptors(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t data[MAX_SETS * 2] = {0};
   uint64_t va;
   va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, descriptor_sets);

   u_foreach_bit (i, descriptors_state->valid) {
      struct radv_descriptor_set *set = descriptors_state->sets[i];
      data[i * 2] = (uint64_t)(uintptr_t)set;
      data[i * 2 + 1] = (uint64_t)(uintptr_t)set >> 32;
   }

   radv_write_data(cmd_buffer, V_370_ME, va, MAX_SETS * 2, data, false);
}

static void
radv_emit_userdata_address(struct radv_device *device, struct radeon_cmdbuf *cs, struct radv_shader *shader,
                           uint32_t base_reg, int idx, uint64_t va)
{
   const struct radv_userdata_info *loc = &shader->info.user_sgprs_locs.shader_data[idx];

   if (loc->sgpr_idx == -1)
      return;

   assert(loc->num_sgprs == 1);

   radv_emit_shader_pointer(device, cs, base_reg + loc->sgpr_idx * 4, va, false);
}

uint64_t
radv_descriptor_get_va(const struct radv_descriptor_state *descriptors_state, unsigned set_idx)
{
   struct radv_descriptor_set *set = descriptors_state->sets[set_idx];
   uint64_t va;

   if (set) {
      va = set->header.va;
   } else {
      va = descriptors_state->descriptor_buffers[set_idx];
   }

   return va;
}

static void
radv_emit_descriptor_pointers(struct radv_device *device, struct radeon_cmdbuf *cs, struct radv_shader *shader,
                              uint32_t sh_base, struct radv_descriptor_state *descriptors_state)
{
   struct radv_userdata_locations *locs = &shader->info.user_sgprs_locs;
   unsigned mask = locs->descriptor_sets_enabled;

   mask &= descriptors_state->dirty & descriptors_state->valid;

   while (mask) {
      int start, count;

      u_bit_scan_consecutive_range(&mask, &start, &count);

      struct radv_userdata_info *loc = &locs->descriptor_sets[start];
      unsigned sh_offset = sh_base + loc->sgpr_idx * 4;

      radv_emit_shader_pointer_head(cs, sh_offset, count, true);
      for (int i = 0; i < count; i++) {
         uint64_t va = radv_descriptor_get_va(descriptors_state, start + i);

         radv_emit_shader_pointer_body(device, cs, va, true);
      }
   }
}

static unsigned
radv_get_rasterization_prim(const struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (cmd_buffer->state.active_stages &
       (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_MESH_BIT_EXT)) {
      /* Ignore dynamic primitive topology for TES/GS/MS stages. */
      return cmd_buffer->state.rast_prim;
   }

   return radv_conv_prim_to_gs_out(d->vk.ia.primitive_topology, last_vgt_shader->info.is_ngg);
}

static ALWAYS_INLINE VkLineRasterizationModeEXT
radv_get_line_mode(const struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   const unsigned rast_prim = radv_get_rasterization_prim(cmd_buffer);

   bool draw_lines = radv_rast_prim_is_line(rast_prim) || radv_polygon_mode_is_line(d->vk.rs.polygon_mode);
   draw_lines &= !radv_rast_prim_is_point(rast_prim);
   draw_lines &= !radv_polygon_mode_is_point(d->vk.rs.polygon_mode);
   if (draw_lines)
      return d->vk.rs.line.mode;

   return VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT;
}

static ALWAYS_INLINE unsigned
radv_get_rasterization_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   VkLineRasterizationModeEXT line_mode = radv_get_line_mode(cmd_buffer);

   if (line_mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR) {
      /* From the Vulkan spec 1.3.221:
       *
       * "When Bresenham lines are being rasterized, sample locations may all be treated as being at
       * the pixel center (this may affect attribute and depth interpolation)."
       *
       * "One consequence of this is that Bresenham lines cover the same pixels regardless of the
       * number of rasterization samples, and cover all samples in those pixels (unless masked out
       * or killed)."
       */
      return 1;
   }

   if (line_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR) {
      return RADV_NUM_SMOOTH_AA_SAMPLES;
   }

   return MAX2(1, d->vk.ms.rasterization_samples);
}

static ALWAYS_INLINE unsigned
radv_get_ps_iter_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   unsigned ps_iter_samples = 1;

   if (cmd_buffer->state.ms.sample_shading_enable) {
      unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
      unsigned color_samples = MAX2(render->color_samples, rasterization_samples);

      ps_iter_samples = ceilf(cmd_buffer->state.ms.min_sample_shading * color_samples);
      ps_iter_samples = util_next_power_of_two(ps_iter_samples);
   }

   return ps_iter_samples;
}

/**
 * Convert the user sample locations to hardware sample locations (the values
 * that will be emitted by PA_SC_AA_SAMPLE_LOCS_PIXEL_*).
 */
static void
radv_convert_user_sample_locs(const struct radv_sample_locations_state *state, uint32_t x, uint32_t y,
                              VkOffset2D *sample_locs)
{
   uint32_t x_offset = x % state->grid_size.width;
   uint32_t y_offset = y % state->grid_size.height;
   uint32_t num_samples = (uint32_t)state->per_pixel;
   uint32_t pixel_offset;

   pixel_offset = (x_offset + y_offset * state->grid_size.width) * num_samples;

   assert(pixel_offset <= MAX_SAMPLE_LOCATIONS);
   const VkSampleLocationEXT *user_locs = &state->locations[pixel_offset];

   for (uint32_t i = 0; i < num_samples; i++) {
      float shifted_pos_x = user_locs[i].x - 0.5;
      float shifted_pos_y = user_locs[i].y - 0.5;

      int32_t scaled_pos_x = floorf(shifted_pos_x * 16);
      int32_t scaled_pos_y = floorf(shifted_pos_y * 16);

      sample_locs[i].x = CLAMP(scaled_pos_x, -8, 7);
      sample_locs[i].y = CLAMP(scaled_pos_y, -8, 7);
   }
}

/**
 * Compute the PA_SC_AA_SAMPLE_LOCS_PIXEL_* mask based on hardware sample
 * locations.
 */
static void
radv_compute_sample_locs_pixel(uint32_t num_samples, VkOffset2D *sample_locs, uint32_t *sample_locs_pixel)
{
   for (uint32_t i = 0; i < num_samples; i++) {
      uint32_t sample_reg_idx = i / 4;
      uint32_t sample_loc_idx = i % 4;
      int32_t pos_x = sample_locs[i].x;
      int32_t pos_y = sample_locs[i].y;

      uint32_t shift_x = 8 * sample_loc_idx;
      uint32_t shift_y = shift_x + 4;

      sample_locs_pixel[sample_reg_idx] |= (pos_x & 0xf) << shift_x;
      sample_locs_pixel[sample_reg_idx] |= (pos_y & 0xf) << shift_y;
   }
}

/**
 * Compute the PA_SC_CENTROID_PRIORITY_* mask based on the top left hardware
 * sample locations.
 */
static uint64_t
radv_compute_centroid_priority(struct radv_cmd_buffer *cmd_buffer, VkOffset2D *sample_locs, uint32_t num_samples)
{
   uint32_t *centroid_priorities = alloca(num_samples * sizeof(*centroid_priorities));
   uint32_t sample_mask = num_samples - 1;
   uint32_t *distances = alloca(num_samples * sizeof(*distances));
   uint64_t centroid_priority = 0;

   /* Compute the distances from center for each sample. */
   for (int i = 0; i < num_samples; i++) {
      distances[i] = (sample_locs[i].x * sample_locs[i].x) + (sample_locs[i].y * sample_locs[i].y);
   }

   /* Compute the centroid priorities by looking at the distances array. */
   for (int i = 0; i < num_samples; i++) {
      uint32_t min_idx = 0;

      for (int j = 1; j < num_samples; j++) {
         if (distances[j] < distances[min_idx])
            min_idx = j;
      }

      centroid_priorities[i] = min_idx;
      distances[min_idx] = 0xffffffff;
   }

   /* Compute the final centroid priority. */
   for (int i = 0; i < 8; i++) {
      centroid_priority |= centroid_priorities[i & sample_mask] << (i * 4);
   }

   return centroid_priority << 32 | centroid_priority;
}

/**
 * Emit the sample locations that are specified with VK_EXT_sample_locations.
 */
static void
radv_emit_sample_locations(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t num_samples = (uint32_t)d->sample_location.per_pixel;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t sample_locs_pixel[4][2] = {0};
   VkOffset2D sample_locs[4][8]; /* 8 is the max. sample count supported */
   uint64_t centroid_priority;

   if (!d->sample_location.count || !d->vk.ms.sample_locations_enable)
      return;

   /* Convert the user sample locations to hardware sample locations. */
   radv_convert_user_sample_locs(&d->sample_location, 0, 0, sample_locs[0]);
   radv_convert_user_sample_locs(&d->sample_location, 1, 0, sample_locs[1]);
   radv_convert_user_sample_locs(&d->sample_location, 0, 1, sample_locs[2]);
   radv_convert_user_sample_locs(&d->sample_location, 1, 1, sample_locs[3]);

   /* Compute the PA_SC_AA_SAMPLE_LOCS_PIXEL_* mask. */
   for (uint32_t i = 0; i < 4; i++) {
      radv_compute_sample_locs_pixel(num_samples, sample_locs[i], sample_locs_pixel[i]);
   }

   /* Compute the PA_SC_CENTROID_PRIORITY_* mask. */
   centroid_priority = radv_compute_centroid_priority(cmd_buffer, sample_locs[0], num_samples);

   /* Emit the specified user sample locations. */
   switch (num_samples) {
   case 2:
   case 4:
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, sample_locs_pixel[0][0]);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, sample_locs_pixel[1][0]);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, sample_locs_pixel[2][0]);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, sample_locs_pixel[3][0]);
      break;
   case 8:
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, sample_locs_pixel[0][0]);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, sample_locs_pixel[1][0]);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, sample_locs_pixel[2][0]);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, sample_locs_pixel[3][0]);
      radeon_set_context_reg(cs, R_028BFC_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_1, sample_locs_pixel[0][1]);
      radeon_set_context_reg(cs, R_028C0C_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_1, sample_locs_pixel[1][1]);
      radeon_set_context_reg(cs, R_028C1C_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_1, sample_locs_pixel[2][1]);
      radeon_set_context_reg(cs, R_028C2C_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_1, sample_locs_pixel[3][1]);
      break;
   default:
      unreachable("invalid number of samples");
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(cs, R_028BF0_PA_SC_CENTROID_PRIORITY_0, 2);
   } else {
      radeon_set_context_reg_seq(cs, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
   }
   radeon_emit(cs, centroid_priority);
   radeon_emit(cs, centroid_priority >> 32);
}

static void
radv_emit_inline_push_consts(struct radv_device *device, struct radeon_cmdbuf *cs, const struct radv_shader *shader,
                             uint32_t base_reg, int idx, uint32_t *values)
{
   const struct radv_userdata_info *loc = &shader->info.user_sgprs_locs.shader_data[idx];

   if (loc->sgpr_idx == -1)
      return;

   radeon_check_space(device->ws, cs, 2 + loc->num_sgprs);

   radeon_set_sh_reg_seq(cs, base_reg + loc->sgpr_idx * 4, loc->num_sgprs);
   radeon_emit_array(cs, values, loc->num_sgprs);
}

struct radv_bin_size_entry {
   unsigned bpp;
   VkExtent2D extent;
};

static VkExtent2D
radv_gfx10_compute_bin_size(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   VkExtent2D extent = {512, 512};

   const unsigned db_tag_size = 64;
   const unsigned db_tag_count = 312;
   const unsigned color_tag_size = 1024;
   const unsigned color_tag_count = 31;
   const unsigned fmask_tag_size = 256;
   const unsigned fmask_tag_count = 44;

   const unsigned rb_count = pdev->info.max_render_backends;
   const unsigned pipe_count = MAX2(rb_count, pdev->info.num_tcc_blocks);

   const unsigned db_tag_part = (db_tag_count * rb_count / pipe_count) * db_tag_size * pipe_count;
   const unsigned color_tag_part = (color_tag_count * rb_count / pipe_count) * color_tag_size * pipe_count;
   const unsigned fmask_tag_part = (fmask_tag_count * rb_count / pipe_count) * fmask_tag_size * pipe_count;

   const unsigned total_samples = radv_get_rasterization_samples(cmd_buffer);
   const unsigned samples_log = util_logbase2_ceil(total_samples);

   unsigned color_bytes_per_pixel = 0;
   unsigned fmask_bytes_per_pixel = 0;

   for (unsigned i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      if (!d->vk.cb.attachments[i].write_mask)
         continue;

      color_bytes_per_pixel += vk_format_get_blocksize(render->color_att[i].format);

      if (total_samples > 1) {
         assert(samples_log <= 3);
         const unsigned fmask_array[] = {0, 1, 1, 4};
         fmask_bytes_per_pixel += fmask_array[samples_log];
      }
   }

   color_bytes_per_pixel *= total_samples;
   color_bytes_per_pixel = MAX2(color_bytes_per_pixel, 1);

   const unsigned color_pixel_count_log = util_logbase2(color_tag_part / color_bytes_per_pixel);
   extent.width = 1ull << ((color_pixel_count_log + 1) / 2);
   extent.height = 1ull << (color_pixel_count_log / 2);

   if (fmask_bytes_per_pixel) {
      const unsigned fmask_pixel_count_log = util_logbase2(fmask_tag_part / fmask_bytes_per_pixel);

      const VkExtent2D fmask_extent = (VkExtent2D){.width = 1ull << ((fmask_pixel_count_log + 1) / 2),
                                                   .height = 1ull << (color_pixel_count_log / 2)};

      if (fmask_extent.width * fmask_extent.height < extent.width * extent.height)
         extent = fmask_extent;
   }

   if (render->ds_att.iview) {
      /* Coefficients taken from AMDVLK */
      unsigned depth_coeff = vk_format_has_depth(render->ds_att.format) ? 5 : 0;
      unsigned stencil_coeff = vk_format_has_stencil(render->ds_att.format) ? 1 : 0;
      unsigned db_bytes_per_pixel = (depth_coeff + stencil_coeff) * total_samples;

      const unsigned db_pixel_count_log = util_logbase2(db_tag_part / db_bytes_per_pixel);

      const VkExtent2D db_extent =
         (VkExtent2D){.width = 1ull << ((db_pixel_count_log + 1) / 2), .height = 1ull << (color_pixel_count_log / 2)};

      if (db_extent.width * db_extent.height < extent.width * extent.height)
         extent = db_extent;
   }

   extent.width = MAX2(extent.width, 128);
   extent.height = MAX2(extent.width, pdev->info.gfx_level >= GFX12 ? 128 : 64);

   return extent;
}

static VkExtent2D
radv_gfx9_compute_bin_size(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   static const struct radv_bin_size_entry color_size_table[][3][9] = {
      {
         /* One RB / SE */
         {
            /* One shader engine */
            {0, {128, 128}},
            {1, {64, 128}},
            {2, {32, 128}},
            {3, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {128, 128}},
            {2, {64, 128}},
            {3, {32, 128}},
            {5, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {128, 128}},
            {3, {64, 128}},
            {5, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         /* Two RB / SE */
         {
            /* One shader engine */
            {0, {128, 128}},
            {2, {64, 128}},
            {3, {32, 128}},
            {5, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {128, 128}},
            {3, {64, 128}},
            {5, {32, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {256, 256}},
            {2, {128, 256}},
            {3, {128, 128}},
            {5, {64, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         /* Four RB / SE */
         {
            /* One shader engine */
            {0, {128, 256}},
            {2, {128, 128}},
            {3, {64, 128}},
            {5, {32, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {256, 256}},
            {2, {128, 256}},
            {3, {128, 128}},
            {5, {64, 128}},
            {9, {32, 128}},
            {17, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {256, 512}},
            {2, {256, 256}},
            {3, {128, 256}},
            {5, {128, 128}},
            {9, {64, 128}},
            {17, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
   };
   static const struct radv_bin_size_entry ds_size_table[][3][9] = {
      {
         // One RB / SE
         {
            // One shader engine
            {0, {128, 256}},
            {2, {128, 128}},
            {4, {64, 128}},
            {7, {32, 128}},
            {13, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {256, 256}},
            {2, {128, 256}},
            {4, {128, 128}},
            {7, {64, 128}},
            {13, {32, 128}},
            {25, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         // Two RB / SE
         {
            // One shader engine
            {0, {256, 256}},
            {2, {128, 256}},
            {4, {128, 128}},
            {7, {64, 128}},
            {13, {32, 128}},
            {25, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {32, 128}},
            {49, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {512, 512}},
            {2, {256, 512}},
            {4, {256, 256}},
            {7, {128, 256}},
            {13, {128, 128}},
            {25, {64, 128}},
            {49, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         // Four RB / SE
         {
            // One shader engine
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {32, 128}},
            {49, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {512, 512}},
            {2, {256, 512}},
            {4, {256, 256}},
            {7, {128, 256}},
            {13, {128, 128}},
            {25, {64, 128}},
            {49, {32, 128}},
            {97, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {512, 512}},
            {4, {256, 512}},
            {7, {256, 256}},
            {13, {128, 256}},
            {25, {128, 128}},
            {49, {64, 128}},
            {97, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
      },
   };

   VkExtent2D extent = {512, 512};

   unsigned log_num_rb_per_se = util_logbase2_ceil(pdev->info.max_render_backends / pdev->info.max_se);
   unsigned log_num_se = util_logbase2_ceil(pdev->info.max_se);

   unsigned total_samples = radv_get_rasterization_samples(cmd_buffer);
   unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   unsigned effective_samples = total_samples;
   unsigned color_bytes_per_pixel = 0;

   for (unsigned i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      if (!d->vk.cb.attachments[i].write_mask)
         continue;

      color_bytes_per_pixel += vk_format_get_blocksize(render->color_att[i].format);
   }

   /* MSAA images typically don't use all samples all the time. */
   if (effective_samples >= 2 && ps_iter_samples <= 1)
      effective_samples = 2;
   color_bytes_per_pixel *= effective_samples;

   const struct radv_bin_size_entry *color_entry = color_size_table[log_num_rb_per_se][log_num_se];
   while (color_entry[1].bpp <= color_bytes_per_pixel)
      ++color_entry;

   extent = color_entry->extent;

   if (render->ds_att.iview) {
      /* Coefficients taken from AMDVLK */
      unsigned depth_coeff = vk_format_has_depth(render->ds_att.format) ? 5 : 0;
      unsigned stencil_coeff = vk_format_has_stencil(render->ds_att.format) ? 1 : 0;
      unsigned ds_bytes_per_pixel = 4 * (depth_coeff + stencil_coeff) * total_samples;

      const struct radv_bin_size_entry *ds_entry = ds_size_table[log_num_rb_per_se][log_num_se];
      while (ds_entry[1].bpp <= ds_bytes_per_pixel)
         ++ds_entry;

      if (ds_entry->extent.width * ds_entry->extent.height < extent.width * extent.height)
         extent = ds_entry->extent;
   }

   return extent;
}

static unsigned
radv_get_disabled_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t pa_sc_binner_cntl_0;

   if (pdev->info.gfx_level >= GFX12) {
      const uint32_t bin_size_x = 128, bin_size_y = 128;

      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_BINNING_DISABLED) | S_028C44_BIN_SIZE_X_EXTEND(util_logbase2(bin_size_x) - 5) |
         S_028C44_BIN_SIZE_Y_EXTEND(util_logbase2(bin_size_y) - 5) | S_028C44_DISABLE_START_OF_PRIM(1) |
         S_028C44_FPOVS_PER_BATCH(63) | S_028C44_OPTIMAL_BIN_SELECTION(1) | S_028C44_FLUSH_ON_BINNING_TRANSITION(1);
   } else if (pdev->info.gfx_level >= GFX10) {
      const unsigned binning_disabled =
         pdev->info.gfx_level >= GFX11_5 ? V_028C44_BINNING_DISABLED : V_028C44_DISABLE_BINNING_USE_NEW_SC;
      unsigned min_bytes_per_pixel = 0;

      for (unsigned i = 0; i < render->color_att_count; ++i) {
         struct radv_image_view *iview = render->color_att[i].iview;

         if (!iview)
            continue;

         if (!d->vk.cb.attachments[i].write_mask)
            continue;

         unsigned bytes = vk_format_get_blocksize(render->color_att[i].format);
         if (!min_bytes_per_pixel || bytes < min_bytes_per_pixel)
            min_bytes_per_pixel = bytes;
      }

      pa_sc_binner_cntl_0 = S_028C44_BINNING_MODE(binning_disabled) | S_028C44_BIN_SIZE_X(0) | S_028C44_BIN_SIZE_Y(0) |
                            S_028C44_BIN_SIZE_X_EXTEND(2) |                                /* 128 */
                            S_028C44_BIN_SIZE_Y_EXTEND(min_bytes_per_pixel <= 4 ? 2 : 1) | /* 128 or 64 */
                            S_028C44_DISABLE_START_OF_PRIM(1) | S_028C44_FLUSH_ON_BINNING_TRANSITION(1);
   } else {
      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_DISABLE_BINNING_USE_LEGACY_SC) | S_028C44_DISABLE_START_OF_PRIM(1) |
         S_028C44_FLUSH_ON_BINNING_TRANSITION(pdev->info.family == CHIP_VEGA12 || pdev->info.family == CHIP_VEGA20 ||
                                              pdev->info.family >= CHIP_RAVEN2);
   }

   return pa_sc_binner_cntl_0;
}

static unsigned
radv_get_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned pa_sc_binner_cntl_0;
   VkExtent2D bin_size;

   if (pdev->info.gfx_level >= GFX10) {
      bin_size = radv_gfx10_compute_bin_size(cmd_buffer);
   } else {
      assert(pdev->info.gfx_level == GFX9);
      bin_size = radv_gfx9_compute_bin_size(cmd_buffer);
   }

   if (device->pbb_allowed && bin_size.width && bin_size.height) {
      const struct radv_binning_settings *settings = &pdev->binning_settings;

      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_BINNING_ALLOWED) | S_028C44_BIN_SIZE_X(bin_size.width == 16) |
         S_028C44_BIN_SIZE_Y(bin_size.height == 16) |
         S_028C44_BIN_SIZE_X_EXTEND(util_logbase2(MAX2(bin_size.width, 32)) - 5) |
         S_028C44_BIN_SIZE_Y_EXTEND(util_logbase2(MAX2(bin_size.height, 32)) - 5) |
         S_028C44_CONTEXT_STATES_PER_BIN(settings->context_states_per_bin - 1) |
         S_028C44_PERSISTENT_STATES_PER_BIN(settings->persistent_states_per_bin - 1) |
         S_028C44_DISABLE_START_OF_PRIM(1) | S_028C44_FPOVS_PER_BATCH(settings->fpovs_per_batch) |
         S_028C44_OPTIMAL_BIN_SELECTION(1) |
         S_028C44_FLUSH_ON_BINNING_TRANSITION(pdev->info.family == CHIP_VEGA12 || pdev->info.family == CHIP_VEGA20 ||
                                              pdev->info.family >= CHIP_RAVEN2);
   } else {
      pa_sc_binner_cntl_0 = radv_get_disabled_binning_state(cmd_buffer);
   }

   return pa_sc_binner_cntl_0;
}

static void
radv_emit_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned pa_sc_binner_cntl_0;

   if (pdev->info.gfx_level < GFX9)
      return;

   pa_sc_binner_cntl_0 = radv_get_binning_state(cmd_buffer);

   radeon_opt_set_context_reg(cmd_buffer, R_028C44_PA_SC_BINNER_CNTL_0, RADV_TRACKED_PA_SC_BINNER_CNTL_0,
                              pa_sc_binner_cntl_0);
}

static void
radv_emit_shader_prefetch(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *shader)
{
   uint64_t va;

   if (!shader)
      return;

   va = radv_shader_get_va(shader);

   radv_cp_dma_prefetch(cmd_buffer, va, shader->code_size);
}

ALWAYS_INLINE static void
radv_emit_prefetch_L2(struct radv_cmd_buffer *cmd_buffer, bool first_stage_only)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t mask = state->prefetch_L2_mask;

   /* Fast prefetch path for starting draws as soon as possible. */
   if (first_stage_only)
      mask &= RADV_PREFETCH_VS | RADV_PREFETCH_VBO_DESCRIPTORS | RADV_PREFETCH_MS;

   if (mask & RADV_PREFETCH_VS)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_VERTEX]);

   if (mask & RADV_PREFETCH_MS)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_MESH]);

   if (mask & RADV_PREFETCH_VBO_DESCRIPTORS)
      radv_cp_dma_prefetch(cmd_buffer, state->vb_va, state->vb_size);

   if (mask & RADV_PREFETCH_TCS)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL]);

   if (mask & RADV_PREFETCH_TES)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]);

   if (mask & RADV_PREFETCH_GS) {
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]);
      if (cmd_buffer->state.gs_copy_shader)
         radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.gs_copy_shader);
   }

   if (mask & RADV_PREFETCH_PS) {
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT]);
   }

   state->prefetch_L2_mask &= ~mask;
}

static void
radv_emit_rbplus_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   assert(pdev->info.rbplus_allowed);

   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   unsigned sx_ps_downconvert = 0;
   unsigned sx_blend_opt_epsilon = 0;
   unsigned sx_blend_opt_control = 0;

   for (unsigned i = 0; i < render->color_att_count; i++) {
      unsigned format, swap;
      bool has_alpha, has_rgb;
      if (render->color_att[i].iview == NULL) {
         /* We don't set the DISABLE bits, because the HW can't have holes,
          * so the SPI color format is set to 32-bit 1-component. */
         sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_R << (i * 4);
         continue;
      }

      struct radv_color_buffer_info *cb = &render->color_att[i].cb;

      format = pdev->info.gfx_level >= GFX11 ? G_028C70_FORMAT_GFX11(cb->ac.cb_color_info)
                                             : G_028C70_FORMAT_GFX6(cb->ac.cb_color_info);
      swap = G_028C70_COMP_SWAP(cb->ac.cb_color_info);
      has_alpha = pdev->info.gfx_level >= GFX11 ? !G_028C74_FORCE_DST_ALPHA_1_GFX11(cb->ac.cb_color_attrib)
                                                : !G_028C74_FORCE_DST_ALPHA_1_GFX6(cb->ac.cb_color_attrib);

      uint32_t spi_format = (cmd_buffer->state.spi_shader_col_format >> (i * 4)) & 0xf;
      uint32_t colormask = d->vk.cb.attachments[i].write_mask;

      if (format == V_028C70_COLOR_8 || format == V_028C70_COLOR_16 || format == V_028C70_COLOR_32)
         has_rgb = !has_alpha;
      else
         has_rgb = true;

      /* Check the colormask and export format. */
      if (!(colormask & 0x7))
         has_rgb = false;
      if (!(colormask & 0x8))
         has_alpha = false;

      if (spi_format == V_028714_SPI_SHADER_ZERO) {
         has_rgb = false;
         has_alpha = false;
      }

      /* The HW doesn't quite blend correctly with rgb9e5 if we disable the alpha
       * optimization, even though it has no alpha. */
      if (has_rgb && format == V_028C70_COLOR_5_9_9_9)
         has_alpha = true;

      /* Disable value checking for disabled channels. */
      if (!has_rgb)
         sx_blend_opt_control |= S_02875C_MRT0_COLOR_OPT_DISABLE(1) << (i * 4);
      if (!has_alpha)
         sx_blend_opt_control |= S_02875C_MRT0_ALPHA_OPT_DISABLE(1) << (i * 4);

      /* Enable down-conversion for 32bpp and smaller formats. */
      switch (format) {
      case V_028C70_COLOR_8:
      case V_028C70_COLOR_8_8:
      case V_028C70_COLOR_8_8_8_8:
         /* For 1 and 2-channel formats, use the superset thereof. */
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR || spi_format == V_028714_SPI_SHADER_UINT16_ABGR ||
             spi_format == V_028714_SPI_SHADER_SINT16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_8_8_8_8 << (i * 4);

            if (G_028C70_NUMBER_TYPE(cb->ac.cb_color_info) != V_028C70_NUMBER_SRGB)
               sx_blend_opt_epsilon |= V_028758_8BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_5_6_5:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_5_6_5 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_6BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_1_5_5_5:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_1_5_5_5 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_5BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_4_4_4_4:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_4_4_4_4 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_4BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_32:
         if (swap == V_028C70_SWAP_STD && spi_format == V_028714_SPI_SHADER_32_R)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_R << (i * 4);
         else if (swap == V_028C70_SWAP_ALT_REV && spi_format == V_028714_SPI_SHADER_32_AR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_A << (i * 4);
         break;

      case V_028C70_COLOR_16:
      case V_028C70_COLOR_16_16:
         /* For 1-channel formats, use the superset thereof. */
         if (spi_format == V_028714_SPI_SHADER_UNORM16_ABGR || spi_format == V_028714_SPI_SHADER_SNORM16_ABGR ||
             spi_format == V_028714_SPI_SHADER_UINT16_ABGR || spi_format == V_028714_SPI_SHADER_SINT16_ABGR) {
            if (swap == V_028C70_SWAP_STD || swap == V_028C70_SWAP_STD_REV)
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_16_16_GR << (i * 4);
            else
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_16_16_AR << (i * 4);
         }
         break;

      case V_028C70_COLOR_10_11_11:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_10_11_11 << (i * 4);
         break;

      case V_028C70_COLOR_2_10_10_10:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_2_10_10_10 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_10BIT_FORMAT_0_5 << (i * 4);
         }
         break;
      case V_028C70_COLOR_5_9_9_9:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_9_9_9_E5 << (i * 4);
         break;
      }
   }

   /* Do not set the DISABLE bits for the unused attachments, as that
    * breaks dual source blending in SkQP and does not seem to improve
    * performance. */

   radeon_opt_set_context_reg3(cmd_buffer, R_028754_SX_PS_DOWNCONVERT, RADV_TRACKED_SX_PS_DOWNCONVERT,
                               sx_ps_downconvert, sx_blend_opt_epsilon, sx_blend_opt_control);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_RBPLUS;
}

static void
radv_emit_epilog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader,
                 const struct radv_shader_part *epilog)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   radv_cs_add_buffer(device->ws, cs, epilog->bo);

   assert((epilog->va >> 32) == pdev->info.address32_hi);

   const struct radv_userdata_info *loc = &shader->info.user_sgprs_locs.shader_data[AC_UD_EPILOG_PC];
   const uint32_t base_reg = shader->info.user_data_0;
   assert(loc->sgpr_idx != -1 && loc->num_sgprs == 1);
   radv_emit_shader_pointer(device, cs, base_reg + loc->sgpr_idx * 4, epilog->va, false);

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, epilog->upload_seq);
}

static void
radv_emit_ps_epilog_state(struct radv_cmd_buffer *cmd_buffer, struct radv_shader_part *ps_epilog)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_shader *ps_shader = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   if (cmd_buffer->state.emitted_ps_epilog == ps_epilog)
      return;

   if (ps_epilog->spi_shader_z_format) {
      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg(cmd_buffer->cs, R_028650_SPI_SHADER_Z_FORMAT, ps_epilog->spi_shader_z_format);
      } else {
         radeon_set_context_reg(cmd_buffer->cs, R_028710_SPI_SHADER_Z_FORMAT, ps_epilog->spi_shader_z_format);
      }
   }

   assert(ps_shader->config.num_shared_vgprs == 0);
   if (G_00B848_VGPRS(ps_epilog->rsrc1) > G_00B848_VGPRS(ps_shader->config.rsrc1)) {
      uint32_t rsrc1 = ps_shader->config.rsrc1;
      rsrc1 = (rsrc1 & C_00B848_VGPRS) | (ps_epilog->rsrc1 & ~C_00B848_VGPRS);
      radeon_set_sh_reg(cmd_buffer->cs, R_00B028_SPI_SHADER_PGM_RSRC1_PS, rsrc1);
   }

   radv_emit_epilog(cmd_buffer, ps_shader, ps_epilog);

   cmd_buffer->state.emitted_ps_epilog = ps_epilog;
}

static void
radv_emit_compute_shader(const struct radv_physical_device *pdev, struct radeon_cmdbuf *cs,
                         const struct radv_shader *shader)
{
   uint64_t va = radv_shader_get_va(shader);

   radeon_set_sh_reg(cs, R_00B830_COMPUTE_PGM_LO, va >> 8);

   radeon_set_sh_reg_seq(cs, R_00B848_COMPUTE_PGM_RSRC1, 2);
   radeon_emit(cs, shader->config.rsrc1);
   radeon_emit(cs, shader->config.rsrc2);
   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_sh_reg(cs, R_00B8A0_COMPUTE_PGM_RSRC3, shader->config.rsrc3);
   }

   radeon_set_sh_reg(cs, R_00B854_COMPUTE_RESOURCE_LIMITS, shader->info.regs.cs.compute_resource_limits);
   radeon_set_sh_reg_seq(cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
   radeon_emit(cs, shader->info.regs.cs.compute_num_thread_x);
   radeon_emit(cs, shader->info.regs.cs.compute_num_thread_y);
   radeon_emit(cs, shader->info.regs.cs.compute_num_thread_z);
}

static void
radv_emit_vgt_gs_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader_info *info = &cmd_buffer->state.last_vgt_shader->info;
   unsigned vgt_primitiveid_en = 0;
   uint32_t vgt_gs_mode = 0;

   if (info->is_ngg)
      return;

   if (info->stage == MESA_SHADER_GEOMETRY) {
      vgt_gs_mode = ac_vgt_gs_mode(info->gs.vertices_out, pdev->info.gfx_level);
   } else if (info->outinfo.export_prim_id || info->uses_prim_id) {
      vgt_gs_mode = S_028A40_MODE(V_028A40_GS_SCENARIO_A);
      vgt_primitiveid_en |= S_028A84_PRIMITIVEID_EN(1);
   }

   radeon_opt_set_context_reg(cmd_buffer, R_028A84_VGT_PRIMITIVEID_EN, RADV_TRACKED_VGT_PRIMITIVEID_EN,
                              vgt_primitiveid_en);
   radeon_opt_set_context_reg(cmd_buffer, R_028A40_VGT_GS_MODE, RADV_TRACKED_VGT_GS_MODE, vgt_gs_mode);
}

static void
radv_emit_hw_vs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t va = radv_shader_get_va(shader);

   radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B120_SPI_SHADER_PGM_LO_VS, 4);
   radeon_emit(cmd_buffer->cs, va >> 8);
   radeon_emit(cmd_buffer->cs, S_00B124_MEM_BASE(va >> 40));
   radeon_emit(cmd_buffer->cs, shader->config.rsrc1);
   radeon_emit(cmd_buffer->cs, shader->config.rsrc2);

   radeon_opt_set_context_reg(cmd_buffer, R_0286C4_SPI_VS_OUT_CONFIG, RADV_TRACKED_SPI_VS_OUT_CONFIG,
                              shader->info.regs.spi_vs_out_config);
   radeon_opt_set_context_reg(cmd_buffer, R_02870C_SPI_SHADER_POS_FORMAT, RADV_TRACKED_SPI_SHADER_POS_FORMAT,
                              shader->info.regs.spi_shader_pos_format);
   radeon_opt_set_context_reg(cmd_buffer, R_02881C_PA_CL_VS_OUT_CNTL, RADV_TRACKED_PA_CL_VS_OUT_CNTL,
                              shader->info.regs.pa_cl_vs_out_cntl);

   if (pdev->info.gfx_level <= GFX8)
      radeon_opt_set_context_reg(cmd_buffer, R_028AB4_VGT_REUSE_OFF, RADV_TRACKED_VGT_REUSE_OFF,
                                 shader->info.regs.vs.vgt_reuse_off);

   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_sh_reg_idx(&pdev->info, cmd_buffer->cs, R_00B118_SPI_SHADER_PGM_RSRC3_VS, 3,
                            shader->info.regs.vs.spi_shader_pgm_rsrc3_vs);
      radeon_set_sh_reg(cmd_buffer->cs, R_00B11C_SPI_SHADER_LATE_ALLOC_VS,
                        shader->info.regs.vs.spi_shader_late_alloc_vs);

      if (pdev->info.gfx_level >= GFX10) {
         radeon_set_uconfig_reg(cmd_buffer->cs, R_030980_GE_PC_ALLOC, shader->info.regs.ge_pc_alloc);

         if (shader->info.stage == MESA_SHADER_TESS_EVAL) {
            radeon_opt_set_context_reg(cmd_buffer, R_028A44_VGT_GS_ONCHIP_CNTL, RADV_TRACKED_VGT_GS_ONCHIP_CNTL,
                                       shader->info.regs.vgt_gs_onchip_cntl);
         }
      }
   }
}

static void
radv_emit_hw_es(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const uint64_t va = radv_shader_get_va(shader);

   radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B320_SPI_SHADER_PGM_LO_ES, 4);
   radeon_emit(cmd_buffer->cs, va >> 8);
   radeon_emit(cmd_buffer->cs, S_00B324_MEM_BASE(va >> 40));
   radeon_emit(cmd_buffer->cs, shader->config.rsrc1);
   radeon_emit(cmd_buffer->cs, shader->config.rsrc2);
}

static void
radv_emit_hw_ls(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const uint64_t va = radv_shader_get_va(shader);

   radeon_set_sh_reg(cmd_buffer->cs, R_00B520_SPI_SHADER_PGM_LO_LS, va >> 8);

   radeon_set_sh_reg(cmd_buffer->cs, R_00B528_SPI_SHADER_PGM_RSRC1_LS, shader->config.rsrc1);
}

static void
radv_emit_hw_ngg(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *es, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t va = radv_shader_get_va(shader);
   gl_shader_stage es_type;
   const struct gfx10_ngg_info *ngg_state = &shader->info.ngg_info;

   if (shader->info.stage == MESA_SHADER_GEOMETRY) {
      if (shader->info.merged_shader_compiled_separately) {
         es_type = es->info.stage;
      } else {
         es_type = shader->info.gs.es_type;
      }
   } else {
      es_type = shader->info.stage;
   }

   if (!shader->info.merged_shader_compiled_separately) {
      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_sh_reg(cmd_buffer->cs, R_00B224_SPI_SHADER_PGM_LO_ES, va >> 8);
      } else {
         radeon_set_sh_reg(cmd_buffer->cs, R_00B320_SPI_SHADER_PGM_LO_ES, va >> 8);
      }

      radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B228_SPI_SHADER_PGM_RSRC1_GS, 2);
      radeon_emit(cmd_buffer->cs, shader->config.rsrc1);
      radeon_emit(cmd_buffer->cs, shader->config.rsrc2);
   }

   const struct radv_vs_output_info *outinfo = &shader->info.outinfo;

   const bool es_enable_prim_id = outinfo->export_prim_id || (es && es->info.uses_prim_id);
   bool break_wave_at_eoi = false;

   if (es_type == MESA_SHADER_TESS_EVAL) {
      if (es_enable_prim_id || (shader->info.uses_prim_id))
         break_wave_at_eoi = true;
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(cmd_buffer, R_028818_PA_CL_VS_OUT_CNTL, RADV_TRACKED_PA_CL_VS_OUT_CNTL,
                                 shader->info.regs.pa_cl_vs_out_cntl);

      radeon_opt_set_context_reg(cmd_buffer, R_028B3C_VGT_GS_INSTANCE_CNT, RADV_TRACKED_VGT_GS_INSTANCE_CNT,
                                 shader->info.regs.vgt_gs_instance_cnt);

      radeon_set_uconfig_reg(cmd_buffer->cs, R_030988_VGT_PRIMITIVEID_EN, shader->info.regs.ngg.vgt_primitiveid_en);

      radeon_opt_set_context_reg2(cmd_buffer, R_028648_SPI_SHADER_IDX_FORMAT, RADV_TRACKED_SPI_SHADER_IDX_FORMAT,
                                  shader->info.regs.ngg.spi_shader_idx_format, shader->info.regs.spi_shader_pos_format);
   } else {
      radeon_opt_set_context_reg(cmd_buffer, R_02881C_PA_CL_VS_OUT_CNTL, RADV_TRACKED_PA_CL_VS_OUT_CNTL,
                                 shader->info.regs.pa_cl_vs_out_cntl);

      radeon_opt_set_context_reg(cmd_buffer, R_028B90_VGT_GS_INSTANCE_CNT, RADV_TRACKED_VGT_GS_INSTANCE_CNT,
                                 shader->info.regs.vgt_gs_instance_cnt);

      radeon_opt_set_context_reg(cmd_buffer, R_028A84_VGT_PRIMITIVEID_EN, RADV_TRACKED_VGT_PRIMITIVEID_EN,
                                 shader->info.regs.ngg.vgt_primitiveid_en | S_028A84_PRIMITIVEID_EN(es_enable_prim_id));

      radeon_opt_set_context_reg2(cmd_buffer, R_028708_SPI_SHADER_IDX_FORMAT, RADV_TRACKED_SPI_SHADER_IDX_FORMAT,
                                  shader->info.regs.ngg.spi_shader_idx_format, shader->info.regs.spi_shader_pos_format);

      radeon_opt_set_context_reg(cmd_buffer, R_0286C4_SPI_VS_OUT_CONFIG, RADV_TRACKED_SPI_VS_OUT_CONFIG,
                                 shader->info.regs.spi_vs_out_config);
   }

   radeon_opt_set_context_reg(cmd_buffer, R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP, RADV_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP,
                              shader->info.regs.ngg.ge_max_output_per_subgroup);

   radeon_opt_set_context_reg(cmd_buffer, R_028B4C_GE_NGG_SUBGRP_CNTL, RADV_TRACKED_GE_NGG_SUBGRP_CNTL,
                              shader->info.regs.ngg.ge_ngg_subgrp_cntl);

   uint32_t ge_cntl = shader->info.regs.ngg.ge_cntl;
   if (pdev->info.gfx_level >= GFX11) {
      ge_cntl |= S_03096C_BREAK_PRIMGRP_AT_EOI(break_wave_at_eoi);
   } else {
      ge_cntl |= S_03096C_BREAK_WAVE_AT_EOI(break_wave_at_eoi);

      /* Bug workaround for a possible hang with non-tessellation cases.
       * Tessellation always sets GE_CNTL.VERT_GRP_SIZE = 0
       *
       * Requirement: GE_CNTL.VERT_GRP_SIZE = VGT_GS_ONCHIP_CNTL.ES_VERTS_PER_SUBGRP - 5
       */
      if (pdev->info.gfx_level == GFX10 && es_type != MESA_SHADER_TESS_EVAL && ngg_state->hw_max_esverts != 256) {
         ge_cntl &= C_03096C_VERT_GRP_SIZE;

         if (ngg_state->hw_max_esverts > 5) {
            ge_cntl |= S_03096C_VERT_GRP_SIZE(ngg_state->hw_max_esverts - 5);
         }
      }

      radeon_opt_set_context_reg(cmd_buffer, R_028A44_VGT_GS_ONCHIP_CNTL, RADV_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 shader->info.regs.vgt_gs_onchip_cntl);
   }

   radeon_set_uconfig_reg(cmd_buffer->cs, R_03096C_GE_CNTL, ge_cntl);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_sh_reg(cmd_buffer->cs, R_00B220_SPI_SHADER_PGM_RSRC4_GS, shader->info.regs.spi_shader_pgm_rsrc4_gs);
   } else {
      if (pdev->info.gfx_level >= GFX7) {
         radeon_set_sh_reg_idx(&pdev->info, cmd_buffer->cs, R_00B21C_SPI_SHADER_PGM_RSRC3_GS, 3,
                               shader->info.regs.spi_shader_pgm_rsrc3_gs);
      }

      radeon_set_sh_reg_idx(&pdev->info, cmd_buffer->cs, R_00B204_SPI_SHADER_PGM_RSRC4_GS, 3,
                            shader->info.regs.spi_shader_pgm_rsrc4_gs);

      radeon_set_uconfig_reg(cmd_buffer->cs, R_030980_GE_PC_ALLOC, shader->info.regs.ge_pc_alloc);
   }
}

static void
radv_emit_hw_hs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t va = radv_shader_get_va(shader);

   if (pdev->info.gfx_level >= GFX9) {
      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_sh_reg(cmd_buffer->cs, R_00B424_SPI_SHADER_PGM_LO_LS, va >> 8);
      } else if (pdev->info.gfx_level >= GFX10) {
         radeon_set_sh_reg(cmd_buffer->cs, R_00B520_SPI_SHADER_PGM_LO_LS, va >> 8);
      } else {
         radeon_set_sh_reg(cmd_buffer->cs, R_00B410_SPI_SHADER_PGM_LO_LS, va >> 8);
      }

      radeon_set_sh_reg(cmd_buffer->cs, R_00B428_SPI_SHADER_PGM_RSRC1_HS, shader->config.rsrc1);
   } else {
      radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B420_SPI_SHADER_PGM_LO_HS, 4);
      radeon_emit(cmd_buffer->cs, va >> 8);
      radeon_emit(cmd_buffer->cs, S_00B424_MEM_BASE(va >> 40));
      radeon_emit(cmd_buffer->cs, shader->config.rsrc1);
      radeon_emit(cmd_buffer->cs, shader->config.rsrc2);
   }
}

static void
radv_emit_vertex_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *vs = cmd_buffer->state.shaders[MESA_SHADER_VERTEX];

   if (vs->info.merged_shader_compiled_separately) {
      assert(vs->info.next_stage == MESA_SHADER_TESS_CTRL || vs->info.next_stage == MESA_SHADER_GEOMETRY);

      const struct radv_userdata_info *loc = &vs->info.user_sgprs_locs.shader_data[AC_UD_NEXT_STAGE_PC];
      const struct radv_shader *next_stage = cmd_buffer->state.shaders[vs->info.next_stage];
      const uint32_t base_reg = vs->info.user_data_0;

      assert(loc->sgpr_idx != -1 && loc->num_sgprs == 1);

      if (!vs->info.vs.has_prolog) {
         uint32_t rsrc1, rsrc2;

         if (vs->info.next_stage == MESA_SHADER_TESS_CTRL) {
            radv_shader_combine_cfg_vs_tcs(vs, next_stage, &rsrc1, NULL);

            if (pdev->info.gfx_level >= GFX12) {
               radeon_set_sh_reg(cmd_buffer->cs, R_00B424_SPI_SHADER_PGM_LO_LS, vs->va >> 8);
            } else if (pdev->info.gfx_level >= GFX10) {
               radeon_set_sh_reg(cmd_buffer->cs, R_00B520_SPI_SHADER_PGM_LO_LS, vs->va >> 8);
            } else {
               radeon_set_sh_reg(cmd_buffer->cs, R_00B410_SPI_SHADER_PGM_LO_LS, vs->va >> 8);
            }

            radeon_set_sh_reg(cmd_buffer->cs, R_00B428_SPI_SHADER_PGM_RSRC1_HS, rsrc1);
         } else {
            radv_shader_combine_cfg_vs_gs(vs, next_stage, &rsrc1, &rsrc2);

            if (pdev->info.gfx_level >= GFX12) {
               radeon_set_sh_reg(cmd_buffer->cs, R_00B224_SPI_SHADER_PGM_LO_ES, vs->va >> 8);
            } else if (pdev->info.gfx_level >= GFX10) {
               radeon_set_sh_reg(cmd_buffer->cs, R_00B320_SPI_SHADER_PGM_LO_ES, vs->va >> 8);
            } else {
               radeon_set_sh_reg(cmd_buffer->cs, R_00B210_SPI_SHADER_PGM_LO_ES, vs->va >> 8);
            }

            unsigned lds_size;
            if (next_stage->info.is_ngg) {
               lds_size = DIV_ROUND_UP(next_stage->info.ngg_info.lds_size, pdev->info.lds_encode_granularity);
            } else {
               lds_size = next_stage->info.gs_ring_info.lds_size;
            }

            radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B228_SPI_SHADER_PGM_RSRC1_GS, 2);
            radeon_emit(cmd_buffer->cs, rsrc1);
            radeon_emit(cmd_buffer->cs, rsrc2 | S_00B22C_LDS_SIZE(lds_size));
         }
      }

      radv_emit_shader_pointer(device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, next_stage->va, false);
      return;
   }

   if (vs->info.vs.as_ls)
      radv_emit_hw_ls(cmd_buffer, vs);
   else if (vs->info.vs.as_es)
      radv_emit_hw_es(cmd_buffer, vs);
   else if (vs->info.is_ngg)
      radv_emit_hw_ngg(cmd_buffer, NULL, vs);
   else
      radv_emit_hw_vs(cmd_buffer, vs);
}

static void
radv_emit_tess_ctrl_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];

   if (tcs->info.merged_shader_compiled_separately) {
      /* When VS+TCS are compiled separately on GFX9+, the VS will jump to the TCS and everything is
       * emitted as part of the VS.
       */
      return;
   }

   radv_emit_hw_hs(cmd_buffer, tcs);
}

static void
radv_emit_tess_eval_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tes = cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL];

   if (tes->info.merged_shader_compiled_separately) {
      assert(tes->info.next_stage == MESA_SHADER_GEOMETRY);

      const struct radv_userdata_info *loc = &tes->info.user_sgprs_locs.shader_data[AC_UD_NEXT_STAGE_PC];
      const struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];
      const uint32_t base_reg = tes->info.user_data_0;
      uint32_t rsrc1, rsrc2;

      assert(loc->sgpr_idx != -1 && loc->num_sgprs == 1);

      radv_shader_combine_cfg_tes_gs(tes, gs, &rsrc1, &rsrc2);

      radeon_set_sh_reg(cmd_buffer->cs, R_00B210_SPI_SHADER_PGM_LO_ES, tes->va >> 8);

      unsigned lds_size;
      if (gs->info.is_ngg) {
         lds_size = DIV_ROUND_UP(gs->info.ngg_info.lds_size, pdev->info.lds_encode_granularity);
      } else {
         lds_size = gs->info.gs_ring_info.lds_size;
      }

      radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B228_SPI_SHADER_PGM_RSRC1_GS, 2);
      radeon_emit(cmd_buffer->cs, rsrc1);
      radeon_emit(cmd_buffer->cs, rsrc2 | S_00B22C_LDS_SIZE(lds_size));

      radv_emit_shader_pointer(device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, gs->va, false);
      return;
   }

   if (tes->info.is_ngg) {
      radv_emit_hw_ngg(cmd_buffer, NULL, tes);
   } else if (tes->info.tes.as_es) {
      radv_emit_hw_es(cmd_buffer, tes);
   } else {
      radv_emit_hw_vs(cmd_buffer, tes);
   }
}

static void
radv_emit_hw_gs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *gs)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_legacy_gs_info *gs_state = &gs->info.gs_ring_info;
   const uint64_t va = radv_shader_get_va(gs);

   radeon_opt_set_context_reg3(cmd_buffer, R_028A60_VGT_GSVS_RING_OFFSET_1, RADV_TRACKED_VGT_GSVS_RING_OFFSET_1,
                               gs->info.regs.gs.vgt_gsvs_ring_offset[0], gs->info.regs.gs.vgt_gsvs_ring_offset[1],
                               gs->info.regs.gs.vgt_gsvs_ring_offset[2]);

   radeon_opt_set_context_reg(cmd_buffer, R_028AB0_VGT_GSVS_RING_ITEMSIZE, RADV_TRACKED_VGT_GSVS_RING_ITEMSIZE,
                              gs->info.regs.gs.vgt_gsvs_ring_itemsize);

   radeon_opt_set_context_reg4(cmd_buffer, R_028B5C_VGT_GS_VERT_ITEMSIZE, RADV_TRACKED_VGT_GS_VERT_ITEMSIZE,
                               gs->info.regs.gs.vgt_gs_vert_itemsize[0], gs->info.regs.gs.vgt_gs_vert_itemsize[1],
                               gs->info.regs.gs.vgt_gs_vert_itemsize[2], gs->info.regs.gs.vgt_gs_vert_itemsize[3]);

   radeon_opt_set_context_reg(cmd_buffer, R_028B90_VGT_GS_INSTANCE_CNT, RADV_TRACKED_VGT_GS_INSTANCE_CNT,
                              gs->info.regs.gs.vgt_gs_instance_cnt);

   if (pdev->info.gfx_level >= GFX9) {
      if (!gs->info.merged_shader_compiled_separately) {

         if (pdev->info.gfx_level >= GFX10) {
            radeon_set_sh_reg(cmd_buffer->cs, R_00B320_SPI_SHADER_PGM_LO_ES, va >> 8);
         } else {
            radeon_set_sh_reg(cmd_buffer->cs, R_00B210_SPI_SHADER_PGM_LO_ES, va >> 8);
         }

         radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B228_SPI_SHADER_PGM_RSRC1_GS, 2);
         radeon_emit(cmd_buffer->cs, gs->config.rsrc1);
         radeon_emit(cmd_buffer->cs, gs->config.rsrc2 | S_00B22C_LDS_SIZE(gs_state->lds_size));
      }

      radeon_opt_set_context_reg(cmd_buffer, R_028A44_VGT_GS_ONCHIP_CNTL, RADV_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 gs->info.regs.vgt_gs_onchip_cntl);

      if (pdev->info.gfx_level == GFX9) {
         radeon_opt_set_context_reg(cmd_buffer, R_028A94_VGT_GS_MAX_PRIMS_PER_SUBGROUP,
                                    RADV_TRACKED_VGT_GS_MAX_PRIMS_PER_SUBGROUP,
                                    gs->info.regs.gs.vgt_gs_max_prims_per_subgroup);
      }
   } else {
      radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B220_SPI_SHADER_PGM_LO_GS, 4);
      radeon_emit(cmd_buffer->cs, va >> 8);
      radeon_emit(cmd_buffer->cs, S_00B224_MEM_BASE(va >> 40));
      radeon_emit(cmd_buffer->cs, gs->config.rsrc1);
      radeon_emit(cmd_buffer->cs, gs->config.rsrc2);

      /* GFX6-8: ESGS offchip ring buffer is allocated according to VGT_ESGS_RING_ITEMSIZE.
       * GFX9+: Only used to set the GS input VGPRs, emulated in shaders.
       */
      radeon_opt_set_context_reg(cmd_buffer, R_028AAC_VGT_ESGS_RING_ITEMSIZE, RADV_TRACKED_VGT_ESGS_RING_ITEMSIZE,
                                 gs->info.regs.gs.vgt_esgs_ring_itemsize);
   }

   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_sh_reg_idx(&pdev->info, cmd_buffer->cs, R_00B21C_SPI_SHADER_PGM_RSRC3_GS, 3,
                            gs->info.regs.spi_shader_pgm_rsrc3_gs);
   }

   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_sh_reg_idx(&pdev->info, cmd_buffer->cs, R_00B204_SPI_SHADER_PGM_RSRC4_GS, 3,
                            gs->info.regs.spi_shader_pgm_rsrc4_gs);
   }
}

static void
radv_emit_geometry_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];
   const struct radv_shader *es = cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                     ? cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                     : cmd_buffer->state.shaders[MESA_SHADER_VERTEX];
   if (gs->info.is_ngg) {
      radv_emit_hw_ngg(cmd_buffer, es, gs);
   } else {
      radv_emit_hw_gs(cmd_buffer, gs);
      radv_emit_hw_vs(cmd_buffer, cmd_buffer->state.gs_copy_shader);
   }

   radeon_opt_set_context_reg(cmd_buffer, R_028B38_VGT_GS_MAX_VERT_OUT, RADV_TRACKED_VGT_GS_MAX_VERT_OUT,
                              gs->info.regs.vgt_gs_max_vert_out);

   if (gs->info.merged_shader_compiled_separately) {
      const uint32_t vgt_esgs_ring_itemsize_offset = radv_get_user_sgpr_loc(gs, AC_UD_VGT_ESGS_RING_ITEMSIZE);

      assert(vgt_esgs_ring_itemsize_offset);

      radeon_set_sh_reg(cmd_buffer->cs, vgt_esgs_ring_itemsize_offset, es->info.esgs_itemsize / 4);

      if (gs->info.is_ngg) {
         const uint32_t ngg_lds_layout_offset = radv_get_user_sgpr_loc(gs, AC_UD_NGG_LDS_LAYOUT);

         assert(ngg_lds_layout_offset);
         assert(!(gs->info.ngg_info.esgs_ring_size & 0xffff0000) && !(gs->info.ngg_info.scratch_lds_base & 0xffff0000));

         radeon_set_sh_reg(cmd_buffer->cs, ngg_lds_layout_offset,
                           SET_SGPR_FIELD(NGG_LDS_LAYOUT_GS_OUT_VERTEX_BASE, gs->info.ngg_info.esgs_ring_size) |
                              SET_SGPR_FIELD(NGG_LDS_LAYOUT_SCRATCH_BASE, gs->info.ngg_info.scratch_lds_base));
      }
   }
}

static void
radv_emit_vgt_gs_out(struct radv_cmd_buffer *cmd_buffer, uint32_t vgt_gs_out_prim_type)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->info.gfx_level >= GFX11) {
      radeon_set_uconfig_reg(cmd_buffer->cs, R_030998_VGT_GS_OUT_PRIM_TYPE, vgt_gs_out_prim_type);
   } else {
      radeon_opt_set_context_reg(cmd_buffer, R_028A6C_VGT_GS_OUT_PRIM_TYPE, RADV_TRACKED_VGT_GS_OUT_PRIM_TYPE,
                                 vgt_gs_out_prim_type);
   }
}

static void
radv_emit_mesh_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ms = cmd_buffer->state.shaders[MESA_SHADER_MESH];
   const uint32_t gs_out = radv_conv_gl_prim_to_gs_out(ms->info.ms.output_prim);

   radv_emit_hw_ngg(cmd_buffer, NULL, ms);
   radeon_opt_set_context_reg(cmd_buffer, R_028B38_VGT_GS_MAX_VERT_OUT, RADV_TRACKED_VGT_GS_MAX_VERT_OUT,
                              ms->info.regs.vgt_gs_max_vert_out);
   radeon_set_uconfig_reg_idx(&pdev->info, cmd_buffer->cs, R_030908_VGT_PRIMITIVE_TYPE, 1, V_008958_DI_PT_POINTLIST);

   if (pdev->mesh_fast_launch_2) {
      radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B2B0_SPI_SHADER_GS_MESHLET_DIM, 2);
      radeon_emit(cmd_buffer->cs, ms->info.regs.ms.spi_shader_gs_meshlet_dim);
      radeon_emit(cmd_buffer->cs, ms->info.regs.ms.spi_shader_gs_meshlet_exp_alloc);
   }

   radv_emit_vgt_gs_out(cmd_buffer, gs_out);
}

enum radv_ps_in_type {
   radv_ps_in_interpolated,
   radv_ps_in_flat,
   radv_ps_in_explicit,
   radv_ps_in_explicit_strict,
   radv_ps_in_interpolated_fp16,
   radv_ps_in_interpolated_fp16_hi,
   radv_ps_in_per_prim_gfx103,
   radv_ps_in_per_prim_gfx11,
};

static uint32_t
offset_to_ps_input(const uint32_t offset, const enum radv_ps_in_type type)
{
   assert(offset != AC_EXP_PARAM_UNDEFINED);

   if (offset >= AC_EXP_PARAM_DEFAULT_VAL_0000 && offset <= AC_EXP_PARAM_DEFAULT_VAL_1111) {
      /* The input is a DEFAULT_VAL constant. */
      return S_028644_OFFSET(0x20) | S_028644_DEFAULT_VAL(offset - AC_EXP_PARAM_DEFAULT_VAL_0000);
   }

   assert(offset <= AC_EXP_PARAM_OFFSET_31);
   uint32_t ps_input_cntl = S_028644_OFFSET(offset);

   switch (type) {
   case radv_ps_in_explicit_strict:
      /* Rotate parameter cache contents to strict vertex order. */
      ps_input_cntl |= S_028644_ROTATE_PC_PTR(1);
      FALLTHROUGH;
   case radv_ps_in_explicit:
      /* Force parameter cache to be read in passthrough mode. */
      ps_input_cntl |= S_028644_OFFSET(1 << 5);
      FALLTHROUGH;
   case radv_ps_in_flat:
      ps_input_cntl |= S_028644_FLAT_SHADE(1);
      break;
   case radv_ps_in_interpolated_fp16_hi:
      ps_input_cntl |= S_028644_ATTR1_VALID(1);
      FALLTHROUGH;
   case radv_ps_in_interpolated_fp16:
      /* These must be set even if only the high 16 bits are used. */
      ps_input_cntl |= S_028644_FP16_INTERP_MODE(1) | S_028644_ATTR0_VALID(1);
      break;
   case radv_ps_in_per_prim_gfx11:
      ps_input_cntl |= S_028644_PRIM_ATTR(1);
      break;
   case radv_ps_in_interpolated:
   case radv_ps_in_per_prim_gfx103:
      break;
   }

   return ps_input_cntl;
}

static void
slot_to_ps_input(const struct radv_vs_output_info *outinfo, unsigned slot, uint32_t *ps_input_cntl, unsigned *ps_offset,
                 const bool use_default_0, const enum radv_ps_in_type type)
{
   unsigned vs_offset = outinfo->vs_output_param_offset[slot];

   if (vs_offset == AC_EXP_PARAM_UNDEFINED) {
      if (use_default_0)
         vs_offset = AC_EXP_PARAM_DEFAULT_VAL_0000;
      else
         return;
   }

   ps_input_cntl[*ps_offset] = offset_to_ps_input(vs_offset, type);
   ++(*ps_offset);
}

static void
input_mask_to_ps_inputs(const struct radv_vs_output_info *outinfo, const struct radv_shader *ps, uint32_t input_mask,
                        uint32_t *ps_input_cntl, unsigned *ps_offset, const enum radv_ps_in_type default_type)
{
   u_foreach_bit (i, input_mask) {
      unsigned vs_offset = outinfo->vs_output_param_offset[VARYING_SLOT_VAR0 + i];
      if (vs_offset == AC_EXP_PARAM_UNDEFINED) {
         ps_input_cntl[*ps_offset] = S_028644_OFFSET(0x20);
         ++(*ps_offset);
         continue;
      }

      enum radv_ps_in_type type = default_type;

      if (ps->info.ps.explicit_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_explicit;
      else if (ps->info.ps.explicit_strict_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_explicit_strict;
      else if (ps->info.ps.float16_hi_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_interpolated_fp16_hi;
      else if (ps->info.ps.float16_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_interpolated_fp16;
      else if (ps->info.ps.float32_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_interpolated;

      ps_input_cntl[*ps_offset] = offset_to_ps_input(vs_offset, type);
      ++(*ps_offset);
   }
}

static void
radv_emit_ps_inputs(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const struct radv_vs_output_info *outinfo = &last_vgt_shader->info.outinfo;
   const bool mesh = last_vgt_shader->info.stage == MESA_SHADER_MESH;
   const bool gfx11plus = pdev->info.gfx_level >= GFX11;
   const enum radv_ps_in_type per_prim = gfx11plus ? radv_ps_in_per_prim_gfx11 : radv_ps_in_per_prim_gfx103;

   uint32_t ps_input_cntl[32];
   unsigned ps_offset = 0;

   if (!mesh) {
      if (ps->info.ps.prim_id_input)
         slot_to_ps_input(outinfo, VARYING_SLOT_PRIMITIVE_ID, ps_input_cntl, &ps_offset, false, radv_ps_in_flat);

      if (ps->info.ps.layer_input)
         slot_to_ps_input(outinfo, VARYING_SLOT_LAYER, ps_input_cntl, &ps_offset, true, radv_ps_in_flat);

      if (ps->info.ps.viewport_index_input)
         slot_to_ps_input(outinfo, VARYING_SLOT_VIEWPORT, ps_input_cntl, &ps_offset, true, radv_ps_in_flat);
   }

   if (ps->info.ps.has_pcoord)
      ps_input_cntl[ps_offset++] = S_028644_PT_SPRITE_TEX(1) | S_028644_OFFSET(0x20);

   if (ps->info.ps.input_clips_culls_mask & 0x0f)
      slot_to_ps_input(outinfo, VARYING_SLOT_CLIP_DIST0, ps_input_cntl, &ps_offset, false, radv_ps_in_interpolated);

   if (ps->info.ps.input_clips_culls_mask & 0xf0)
      slot_to_ps_input(outinfo, VARYING_SLOT_CLIP_DIST1, ps_input_cntl, &ps_offset, false, radv_ps_in_interpolated);

   input_mask_to_ps_inputs(outinfo, ps, ps->info.ps.input_mask, ps_input_cntl, &ps_offset, radv_ps_in_flat);

   /* Per-primitive PS inputs: the HW needs these to be last. */
   if (mesh) {
      if (ps->info.ps.prim_id_input)
         slot_to_ps_input(outinfo, VARYING_SLOT_PRIMITIVE_ID, ps_input_cntl, &ps_offset, false, per_prim);

      if (ps->info.ps.layer_input)
         slot_to_ps_input(outinfo, VARYING_SLOT_LAYER, ps_input_cntl, &ps_offset, true, per_prim);

      if (ps->info.ps.viewport_index_input)
         slot_to_ps_input(outinfo, VARYING_SLOT_VIEWPORT, ps_input_cntl, &ps_offset, true, per_prim);
   }

   input_mask_to_ps_inputs(outinfo, ps, ps->info.ps.input_per_primitive_mask, ps_input_cntl, &ps_offset, per_prim);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_sh_reg(cmd_buffer->cs, R_00B0C4_SPI_SHADER_GS_OUT_CONFIG_PS,
                        last_vgt_shader->info.regs.spi_vs_out_config | ps->info.regs.ps.spi_gs_out_config_ps);

      radeon_opt_set_context_regn(cmd_buffer, R_028664_SPI_PS_INPUT_CNTL_0, ps_input_cntl,
                                  cmd_buffer->tracked_regs.spi_ps_input_cntl, ps_offset);
   } else {
      radeon_opt_set_context_regn(cmd_buffer, R_028644_SPI_PS_INPUT_CNTL_0, ps_input_cntl,
                                  cmd_buffer->tracked_regs.spi_ps_input_cntl, ps_offset);
   }
}

static void
radv_emit_fragment_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const uint64_t va = radv_shader_get_va(ps);

   radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B020_SPI_SHADER_PGM_LO_PS, 4);
   radeon_emit(cmd_buffer->cs, va >> 8);
   radeon_emit(cmd_buffer->cs, S_00B024_MEM_BASE(va >> 40));
   radeon_emit(cmd_buffer->cs, ps->config.rsrc1);
   radeon_emit(cmd_buffer->cs, ps->config.rsrc2);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg2(cmd_buffer, R_02865C_SPI_PS_INPUT_ENA, RADV_TRACKED_SPI_PS_INPUT_ENA,
                                  ps->config.spi_ps_input_ena, ps->config.spi_ps_input_addr);

      radeon_opt_set_context_reg(cmd_buffer, R_028640_SPI_PS_IN_CONTROL, RADV_TRACKED_SPI_PS_IN_CONTROL,
                                 ps->info.regs.ps.spi_ps_in_control);

      radeon_set_context_reg(cmd_buffer->cs, R_028650_SPI_SHADER_Z_FORMAT, ps->info.regs.ps.spi_shader_z_format);

      radeon_set_context_reg(cmd_buffer->cs, R_028BBC_PA_SC_HISZ_CONTROL, ps->info.regs.ps.pa_sc_hisz_control);
   } else {
      radeon_opt_set_context_reg2(cmd_buffer, R_0286CC_SPI_PS_INPUT_ENA, RADV_TRACKED_SPI_PS_INPUT_ENA,
                                  ps->config.spi_ps_input_ena, ps->config.spi_ps_input_addr);

      radeon_opt_set_context_reg(cmd_buffer, R_0286D8_SPI_PS_IN_CONTROL, RADV_TRACKED_SPI_PS_IN_CONTROL,
                                 ps->info.regs.ps.spi_ps_in_control);

      radeon_opt_set_context_reg(cmd_buffer, R_028710_SPI_SHADER_Z_FORMAT, RADV_TRACKED_SPI_SHADER_Z_FORMAT,
                                 ps->info.regs.ps.spi_shader_z_format);

      if (pdev->info.gfx_level >= GFX9 && pdev->info.gfx_level < GFX11)
         radeon_opt_set_context_reg(cmd_buffer, R_028C40_PA_SC_SHADER_CONTROL, RADV_TRACKED_PA_SC_SHADER_CONTROL,
                                    ps->info.regs.ps.pa_sc_shader_control);
   }
}

static void
radv_emit_vgt_reuse(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);

   if (pdev->info.gfx_level == GFX10_3) {
      /* Legacy Tess+GS should disable reuse to prevent hangs on GFX10.3. */
      const bool has_legacy_tess_gs = key->tess && key->gs && !key->ngg;

      radeon_opt_set_context_reg(cmd_buffer, R_028AB4_VGT_REUSE_OFF, RADV_TRACKED_VGT_REUSE_OFF,
                                 S_028AB4_REUSE_OFF(has_legacy_tess_gs));
   }

   if (pdev->info.family >= CHIP_POLARIS10 && pdev->info.gfx_level < GFX10) {
      unsigned vtx_reuse_depth = 30;
      if (tes && tes->info.tes.spacing == TESS_SPACING_FRACTIONAL_ODD) {
         vtx_reuse_depth = 14;
      }
      radeon_opt_set_context_reg(cmd_buffer, R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                 RADV_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL, S_028C58_VTX_REUSE_DEPTH(vtx_reuse_depth));
   }
}

static void
radv_emit_vgt_shader_config_gfx12(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const bool ngg_wave_id_en = key->ngg_streamout || (key->mesh && key->mesh_scratch_ring);
   uint32_t stages = 0;

   stages |= S_028A98_GS_EN(key->gs) | S_028A98_GS_FAST_LAUNCH(key->mesh) | S_028A98_GS_W32_EN(key->gs_wave32) |
             S_028A98_NGG_WAVE_ID_EN(ngg_wave_id_en) | S_028A98_PRIMGEN_PASSTHRU_NO_MSG(key->ngg_passthrough);

   if (key->tess)
      stages |= S_028A98_HS_EN(1) | S_028A98_HS_W32_EN(key->hs_wave32);

   radeon_opt_set_context_reg(cmd_buffer, R_028A98_VGT_SHADER_STAGES_EN, RADV_TRACKED_VGT_SHADER_STAGES_EN, stages);
}

static void
radv_emit_vgt_shader_config_gfx6(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t stages = 0;

   if (key->tess) {
      stages |=
         S_028B54_LS_EN(V_028B54_LS_STAGE_ON) | S_028B54_HS_EN(1) | S_028B54_DYNAMIC_HS(pdev->info.gfx_level != GFX9);

      if (key->gs)
         stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_DS) | S_028B54_GS_EN(1);
      else if (key->ngg)
         stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_DS);
      else
         stages |= S_028B54_VS_EN(V_028B54_VS_STAGE_DS);
   } else if (key->gs) {
      stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_REAL) | S_028B54_GS_EN(1);
   } else if (key->mesh) {
      assert(!key->ngg_passthrough);
      unsigned gs_fast_launch = pdev->mesh_fast_launch_2 ? 2 : 1;
      stages |=
         S_028B54_GS_EN(1) | S_028B54_GS_FAST_LAUNCH(gs_fast_launch) | S_028B54_NGG_WAVE_ID_EN(key->mesh_scratch_ring);
   } else if (key->ngg) {
      stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_REAL);
   }

   if (key->ngg) {
      stages |= S_028B54_PRIMGEN_EN(1) | S_028B54_NGG_WAVE_ID_EN(key->ngg_streamout) |
                S_028B54_PRIMGEN_PASSTHRU_EN(key->ngg_passthrough) |
                S_028B54_PRIMGEN_PASSTHRU_NO_MSG(key->ngg_passthrough && pdev->info.family >= CHIP_NAVI23);
   } else if (key->gs) {
      stages |= S_028B54_VS_EN(V_028B54_VS_STAGE_COPY_SHADER);
   }

   if (pdev->info.gfx_level >= GFX9)
      stages |= S_028B54_MAX_PRIMGRP_IN_WAVE(2);

   if (pdev->info.gfx_level >= GFX10) {
      stages |= S_028B54_HS_W32_EN(key->hs_wave32) | S_028B54_GS_W32_EN(key->gs_wave32) |
                S_028B54_VS_W32_EN(pdev->info.gfx_level < GFX11 && key->vs_wave32);
      /* Legacy GS only supports Wave64. Read it as an implication. */
      assert(!(key->gs && !key->ngg) || !key->gs_wave32);
   }

   radeon_opt_set_context_reg(cmd_buffer, R_028B54_VGT_SHADER_STAGES_EN, RADV_TRACKED_VGT_SHADER_STAGES_EN, stages);
}

static void
radv_emit_vgt_shader_config(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->info.gfx_level >= GFX12) {
      radv_emit_vgt_shader_config_gfx12(cmd_buffer, key);
   } else {
      radv_emit_vgt_shader_config_gfx6(cmd_buffer, key);
   }
}

static void
gfx103_emit_vgt_draw_payload_cntl(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *mesh_shader = cmd_buffer->state.shaders[MESA_SHADER_MESH];
   const bool enable_vrs = cmd_buffer->state.uses_vrs;
   bool enable_prim_payload = false;

   /* Enables the second channel of the primitive export instruction.
    * This channel contains: VRS rate x, y, viewport and layer.
    */
   if (mesh_shader) {
      const struct radv_vs_output_info *outinfo = &mesh_shader->info.outinfo;

      enable_prim_payload = (outinfo->writes_viewport_index_per_primitive || outinfo->writes_layer_per_primitive ||
                             outinfo->writes_primitive_shading_rate_per_primitive);
   }

   const uint32_t vgt_draw_payload_cntl =
      S_028A98_EN_VRS_RATE(enable_vrs) | S_028A98_EN_PRIM_PAYLOAD(enable_prim_payload);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(cmd_buffer, R_028AA0_VGT_DRAW_PAYLOAD_CNTL, RADV_TRACKED_VGT_DRAW_PAYLOAD_CNTL,
                                 vgt_draw_payload_cntl);
   } else {
      radeon_opt_set_context_reg(cmd_buffer, R_028A98_VGT_DRAW_PAYLOAD_CNTL, RADV_TRACKED_VGT_DRAW_PAYLOAD_CNTL,
                                 vgt_draw_payload_cntl);
   }
}

static void
gfx103_emit_vrs_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const bool force_vrs_per_vertex = cmd_buffer->state.last_vgt_shader->info.force_vrs_per_vertex;
   const bool enable_vrs_coarse_shading = cmd_buffer->state.uses_vrs_coarse_shading;
   uint32_t mode = V_028064_SC_VRS_COMB_MODE_PASSTHRU;
   uint8_t rate_x = 0, rate_y = 0;

   if (enable_vrs_coarse_shading) {
      /* When per-draw VRS is not enabled at all, try enabling VRS coarse shading 2x2 if the driver
       * determined that it's safe to enable.
       */
      mode = V_028064_SC_VRS_COMB_MODE_OVERRIDE;
      rate_x = rate_y = 1;
   } else if (force_vrs_per_vertex) {
      /* Otherwise, if per-draw VRS is not enabled statically, try forcing per-vertex VRS if
       * requested by the user. Note that vkd3d-proton always has to declare VRS as dynamic because
       * in DX12 it's fully dynamic.
       */
      radeon_opt_set_context_reg(cmd_buffer, R_028848_PA_CL_VRS_CNTL, RADV_TRACKED_PA_CL_VRS_CNTL,
                                 S_028848_SAMPLE_ITER_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE) |
                                    S_028848_VERTEX_RATE_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE));

      /* If the shader is using discard, turn off coarse shading because discard at 2x2 pixel
       * granularity degrades quality too much. MIN allows sample shading but not coarse shading.
       */
      mode = ps->info.ps.can_discard ? V_028064_SC_VRS_COMB_MODE_MIN : V_028064_SC_VRS_COMB_MODE_PASSTHRU;
   }

   if (pdev->info.gfx_level < GFX11) {
      radeon_opt_set_context_reg(cmd_buffer, R_028064_DB_VRS_OVERRIDE_CNTL, RADV_TRACKED_DB_VRS_OVERRIDE_CNTL,
                                 S_028064_VRS_OVERRIDE_RATE_COMBINER_MODE(mode) | S_028064_VRS_OVERRIDE_RATE_X(rate_x) |
                                    S_028064_VRS_OVERRIDE_RATE_Y(rate_y));
   }
}

static void
radv_emit_graphics_shaders(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   radv_foreach_stage(s, cmd_buffer->state.active_stages & RADV_GRAPHICS_STAGE_BITS)
   {
      switch (s) {
      case MESA_SHADER_VERTEX:
         radv_emit_vertex_shader(cmd_buffer);
         break;
      case MESA_SHADER_TESS_CTRL:
         radv_emit_tess_ctrl_shader(cmd_buffer);
         break;
      case MESA_SHADER_TESS_EVAL:
         radv_emit_tess_eval_shader(cmd_buffer);
         break;
      case MESA_SHADER_GEOMETRY:
         radv_emit_geometry_shader(cmd_buffer);
         break;
      case MESA_SHADER_FRAGMENT:
         radv_emit_fragment_shader(cmd_buffer);
         radv_emit_ps_inputs(cmd_buffer);
         break;
      case MESA_SHADER_MESH:
         radv_emit_mesh_shader(cmd_buffer);
         break;
      case MESA_SHADER_TASK:
         radv_emit_compute_shader(pdev, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK]);
         break;
      default:
         unreachable("invalid bind stage");
      }
   }

   const struct radv_vgt_shader_key vgt_shader_cfg_key =
      radv_get_vgt_shader_key(device, cmd_buffer->state.shaders, cmd_buffer->state.gs_copy_shader);

   radv_emit_vgt_gs_mode(cmd_buffer);
   radv_emit_vgt_reuse(cmd_buffer, &vgt_shader_cfg_key);
   radv_emit_vgt_shader_config(cmd_buffer, &vgt_shader_cfg_key);

   if (pdev->info.gfx_level >= GFX10_3) {
      gfx103_emit_vgt_draw_payload_cntl(cmd_buffer);
      gfx103_emit_vrs_state(cmd_buffer);
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GRAPHICS_SHADERS;
}

static void
radv_emit_graphics_pipeline(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (cmd_buffer->state.emitted_graphics_pipeline == pipeline)
      return;

   if (cmd_buffer->state.emitted_graphics_pipeline) {
      if (radv_rast_prim_is_points_or_lines(cmd_buffer->state.emitted_graphics_pipeline->rast_prim) !=
          radv_rast_prim_is_points_or_lines(pipeline->rast_prim))
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND;

      if (cmd_buffer->state.emitted_graphics_pipeline->rast_prim != pipeline->rast_prim)
         cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_RASTERIZATION_SAMPLES;

      if (cmd_buffer->state.emitted_graphics_pipeline->ms.min_sample_shading != pipeline->ms.min_sample_shading ||
          cmd_buffer->state.emitted_graphics_pipeline->uses_out_of_order_rast != pipeline->uses_out_of_order_rast ||
          cmd_buffer->state.emitted_graphics_pipeline->uses_vrs_attachment != pipeline->uses_vrs_attachment)
         cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;

      if (cmd_buffer->state.emitted_graphics_pipeline->ms.sample_shading_enable != pipeline->ms.sample_shading_enable) {
         cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;
         if (pdev->info.gfx_level >= GFX10_3)
            cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
      }

      if (cmd_buffer->state.emitted_graphics_pipeline->db_render_control != pipeline->db_render_control)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
   }

   radv_emit_graphics_shaders(cmd_buffer);

   if (device->pbb_allowed) {
      const struct radv_binning_settings *settings = &pdev->binning_settings;

      if ((!cmd_buffer->state.emitted_graphics_pipeline ||
           cmd_buffer->state.emitted_graphics_pipeline->base.shaders[MESA_SHADER_FRAGMENT] !=
              cmd_buffer->state.graphics_pipeline->base.shaders[MESA_SHADER_FRAGMENT]) &&
          (settings->context_states_per_bin > 1 || settings->persistent_states_per_bin > 1)) {
         /* Break the batch on PS changes. */
         radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_BREAK_BATCH) | EVENT_INDEX(0));
      }
   }

   if (pipeline->sqtt_shaders_reloc) {
      /* Emit shaders relocation because RGP requires them to be contiguous in memory. */
      radv_sqtt_emit_relocated_shaders(cmd_buffer, pipeline);

      struct radv_shader *task_shader = cmd_buffer->state.shaders[MESA_SHADER_TASK];
      if (task_shader) {
         const struct radv_sqtt_shaders_reloc *reloc = pipeline->sqtt_shaders_reloc;
         const uint64_t va = reloc->va[MESA_SHADER_TASK];

         radeon_set_sh_reg(cmd_buffer->gang.cs, R_00B830_COMPUTE_PGM_LO, va >> 8);
      }
   }

   if (radv_device_fault_detection_enabled(device))
      radv_save_pipeline(cmd_buffer, &pipeline->base);

   cmd_buffer->state.emitted_graphics_pipeline = pipeline;

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PIPELINE;
}

static bool
radv_get_depth_clip_enable(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   return d->vk.rs.depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_TRUE ||
          (d->vk.rs.depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP && !d->vk.rs.depth_clamp_enable);
}

enum radv_depth_clamp_mode {
   RADV_DEPTH_CLAMP_MODE_VIEWPORT = 0,    /* Clamp to the viewport min/max depth bounds */
   RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE = 1, /* Clamp between 0.0f and 1.0f */
   RADV_DEPTH_CLAMP_MODE_DISABLED = 2,    /* Disable depth clamping */
};

static enum radv_depth_clamp_mode
radv_get_depth_clamp_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   bool depth_clip_enable = radv_get_depth_clip_enable(cmd_buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   enum radv_depth_clamp_mode mode;

   mode = RADV_DEPTH_CLAMP_MODE_VIEWPORT;
   if (!d->vk.rs.depth_clamp_enable) {
      /* For optimal performance, depth clamping should always be enabled except if the application
       * disables clamping explicitly or uses depth values outside of the [0.0, 1.0] range.
       */
      if (!depth_clip_enable || device->vk.enabled_extensions.EXT_depth_range_unrestricted) {
         mode = RADV_DEPTH_CLAMP_MODE_DISABLED;
      } else {
         mode = RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE;
      }
   }

   return mode;
}

static void
radv_get_viewport_zscale_ztranslate(struct radv_cmd_buffer *cmd_buffer, uint32_t vp_idx, float *zscale,
                                    float *ztranslate)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (d->vk.vp.depth_clip_negative_one_to_one) {
      *zscale = d->hw_vp.xform[vp_idx].scale[2] * 0.5f;
      *ztranslate = (d->hw_vp.xform[vp_idx].translate[2] + d->vk.vp.viewports[vp_idx].maxDepth) * 0.5f;
   } else {
      *zscale = d->hw_vp.xform[vp_idx].scale[2];
      *ztranslate = d->hw_vp.xform[vp_idx].translate[2];
   }
}

static void
radv_get_viewport_zmin_zmax(struct radv_cmd_buffer *cmd_buffer, const VkViewport *viewport, float *zmin, float *zmax)
{
   const enum radv_depth_clamp_mode depth_clamp_mode = radv_get_depth_clamp_mode(cmd_buffer);

   if (depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE) {
      *zmin = 0.0f;
      *zmax = 1.0f;
   } else {
      *zmin = MIN2(viewport->minDepth, viewport->maxDepth);
      *zmax = MAX2(viewport->minDepth, viewport->maxDepth);
   }
}

static void
radv_emit_viewport(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   assert(d->vk.vp.viewport_count);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_02843C_PA_CL_VPORT_XSCALE, d->vk.vp.viewport_count * 8);

      for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
         float zscale, ztranslate, zmin, zmax;

         radv_get_viewport_zscale_ztranslate(cmd_buffer, i, &zscale, &ztranslate);
         radv_get_viewport_zmin_zmax(cmd_buffer, &d->vk.vp.viewports[i], &zmin, &zmax);

         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].scale[0]));
         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].translate[0]));
         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].scale[1]));
         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].translate[1]));
         radeon_emit(cmd_buffer->cs, fui(zscale));
         radeon_emit(cmd_buffer->cs, fui(ztranslate));
         radeon_emit(cmd_buffer->cs, fui(zmin));
         radeon_emit(cmd_buffer->cs, fui(zmax));
      }
   } else {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_02843C_PA_CL_VPORT_XSCALE, d->vk.vp.viewport_count * 6);

      for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
         float zscale, ztranslate;

         radv_get_viewport_zscale_ztranslate(cmd_buffer, i, &zscale, &ztranslate);

         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].scale[0]));
         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].translate[0]));
         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].scale[1]));
         radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].translate[1]));
         radeon_emit(cmd_buffer->cs, fui(zscale));
         radeon_emit(cmd_buffer->cs, fui(ztranslate));
      }

      radeon_set_context_reg_seq(cmd_buffer->cs, R_0282D0_PA_SC_VPORT_ZMIN_0, d->vk.vp.viewport_count * 2);
      for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
         float zmin, zmax;

         radv_get_viewport_zmin_zmax(cmd_buffer, &d->vk.vp.viewports[i], &zmin, &zmax);

         radeon_emit(cmd_buffer->cs, fui(zmin));
         radeon_emit(cmd_buffer->cs, fui(zmax));
      }
   }
}

static VkRect2D
radv_scissor_from_viewport(const VkViewport *viewport)
{
   float scale[3], translate[3];
   VkRect2D rect;

   radv_get_viewport_xform(viewport, scale, translate);

   rect.offset.x = translate[0] - fabsf(scale[0]);
   rect.offset.y = translate[1] - fabsf(scale[1]);
   rect.extent.width = ceilf(translate[0] + fabsf(scale[0])) - rect.offset.x;
   rect.extent.height = ceilf(translate[1] + fabsf(scale[1])) - rect.offset.y;

   return rect;
}

static VkRect2D
radv_intersect_scissor(const VkRect2D *a, const VkRect2D *b)
{
   VkRect2D ret;
   ret.offset.x = MAX2(a->offset.x, b->offset.x);
   ret.offset.y = MAX2(a->offset.y, b->offset.y);
   ret.extent.width = MIN2(a->offset.x + a->extent.width, b->offset.x + b->extent.width) - ret.offset.x;
   ret.extent.height = MIN2(a->offset.y + a->extent.height, b->offset.y + b->extent.height) - ret.offset.y;
   return ret;
}

static void
radv_emit_scissor(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (!d->vk.vp.scissor_count)
      return;

   radeon_set_context_reg_seq(cs, R_028250_PA_SC_VPORT_SCISSOR_0_TL, d->vk.vp.scissor_count * 2);
   for (unsigned i = 0; i < d->vk.vp.scissor_count; i++) {
      VkRect2D viewport_scissor = radv_scissor_from_viewport(d->vk.vp.viewports + i);
      VkRect2D scissor = radv_intersect_scissor(&d->vk.vp.scissors[i], &viewport_scissor);

      uint32_t minx = scissor.offset.x;
      uint32_t miny = scissor.offset.y;
      uint32_t maxx = minx + scissor.extent.width;
      uint32_t maxy = miny + scissor.extent.height;

      if (pdev->info.gfx_level >= GFX12) {
         /* On GFX12, an empty scissor must be done like this because the bottom-right bounds are inclusive. */
         if (maxx == 0 || maxy == 0) {
            minx = miny = maxx = maxy = 1;
         }

         radeon_emit(cs, S_028250_TL_X(minx) | S_028250_TL_Y_GFX12(miny));
         radeon_emit(cs, S_028254_BR_X(maxx - 1) | S_028254_BR_Y(maxy - 1));
      } else {
         radeon_emit(cs, S_028250_TL_X(minx) | S_028250_TL_Y_GFX6(miny) | S_028250_WINDOW_OFFSET_DISABLE(1));
         radeon_emit(cs, S_028254_BR_X(maxx) | S_028254_BR_Y(maxy));
      }
   }
}

static void
radv_emit_discard_rectangle(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t cliprect_rule = 0;

   if (!d->vk.dr.enable) {
      cliprect_rule = 0xffff;
   } else {
      for (unsigned i = 0; i < (1u << MAX_DISCARD_RECTANGLES); ++i) {
         /* Interpret i as a bitmask, and then set the bit in
          * the mask if that combination of rectangles in which
          * the pixel is contained should pass the cliprect
          * test.
          */
         unsigned relevant_subset = i & ((1u << d->vk.dr.rectangle_count) - 1);

         if (d->vk.dr.mode == VK_DISCARD_RECTANGLE_MODE_INCLUSIVE_EXT && !relevant_subset)
            continue;

         if (d->vk.dr.mode == VK_DISCARD_RECTANGLE_MODE_EXCLUSIVE_EXT && relevant_subset)
            continue;

         cliprect_rule |= 1u << i;
      }

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028210_PA_SC_CLIPRECT_0_TL, d->vk.dr.rectangle_count * 2);
      for (unsigned i = 0; i < d->vk.dr.rectangle_count; ++i) {
         VkRect2D rect = d->vk.dr.rectangles[i];
         radeon_emit(cmd_buffer->cs, S_028210_TL_X(rect.offset.x) | S_028210_TL_Y(rect.offset.y));
         radeon_emit(cmd_buffer->cs, S_028214_BR_X(rect.offset.x + rect.extent.width) |
                                        S_028214_BR_Y(rect.offset.y + rect.extent.height));
      }

      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028374_PA_SC_CLIPRECT_0_EXT, d->vk.dr.rectangle_count);
         for (unsigned i = 0; i < d->vk.dr.rectangle_count; ++i) {
            VkRect2D rect = d->vk.dr.rectangles[i];
            radeon_emit(cmd_buffer->cs, S_028374_TL_X_EXT(rect.offset.x >> 15) |
                                           S_028374_TL_Y_EXT(rect.offset.y >> 15) |
                                           S_028374_BR_X_EXT((rect.offset.x + rect.extent.width) >> 15) |
                                           S_028374_BR_Y_EXT((rect.offset.y + rect.extent.height) >> 15));
         }
      }
   }

   radeon_set_context_reg(cmd_buffer->cs, R_02820C_PA_SC_CLIPRECT_RULE, cliprect_rule);
}

static void
radv_emit_line_width(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg(cmd_buffer->cs, R_028A08_PA_SU_LINE_CNTL,
                          S_028A08_WIDTH(CLAMP(d->vk.rs.line.width * 8, 0, 0xFFFF)));
}

static void
radv_emit_blend_constants(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028414_CB_BLEND_RED, 4);
   radeon_emit_array(cmd_buffer->cs, (uint32_t *)d->vk.cb.blend_constants, 4);
}

static void
radv_emit_stencil(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(
         cmd_buffer->cs, R_028088_DB_STENCIL_REF,
         S_028088_TESTVAL(d->vk.ds.stencil.front.reference) | S_028088_TESTVAL_BF(d->vk.ds.stencil.back.reference));

      radeon_set_context_reg(cmd_buffer->cs, R_028090_DB_STENCIL_READ_MASK,
                             S_028090_TESTMASK(d->vk.ds.stencil.front.compare_mask) |
                                S_028090_TESTMASK_BF(d->vk.ds.stencil.back.compare_mask));

      radeon_set_context_reg(cmd_buffer->cs, R_028094_DB_STENCIL_WRITE_MASK,
                             S_028094_WRITEMASK(d->vk.ds.stencil.front.write_mask) |
                                S_028094_WRITEMASK_BF(d->vk.ds.stencil.back.write_mask));
   } else {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028430_DB_STENCILREFMASK, 2);
      radeon_emit(cmd_buffer->cs, S_028430_STENCILTESTVAL(d->vk.ds.stencil.front.reference) |
                                     S_028430_STENCILMASK(d->vk.ds.stencil.front.compare_mask) |
                                     S_028430_STENCILWRITEMASK(d->vk.ds.stencil.front.write_mask) |
                                     S_028430_STENCILOPVAL(1));
      radeon_emit(cmd_buffer->cs, S_028434_STENCILTESTVAL_BF(d->vk.ds.stencil.back.reference) |
                                     S_028434_STENCILMASK_BF(d->vk.ds.stencil.back.compare_mask) |
                                     S_028434_STENCILWRITEMASK_BF(d->vk.ds.stencil.back.write_mask) |
                                     S_028434_STENCILOPVAL_BF(1));
   }
}

static void
radv_emit_depth_bounds(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028050_DB_DEPTH_BOUNDS_MIN, 2);
   } else {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028020_DB_DEPTH_BOUNDS_MIN, 2);
   }

   radeon_emit(cmd_buffer->cs, fui(d->vk.ds.depth.bounds_test.min));
   radeon_emit(cmd_buffer->cs, fui(d->vk.ds.depth.bounds_test.max));
}

static void
radv_emit_depth_bias(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   unsigned slope = fui(d->vk.rs.depth_bias.slope * 16.0f);
   unsigned pa_su_poly_offset_db_fmt_cntl = 0;

   if (vk_format_has_depth(render->ds_att.format) &&
       d->vk.rs.depth_bias.representation != VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT) {
      VkFormat format = vk_format_depth_only(render->ds_att.format);

      if (format == VK_FORMAT_D16_UNORM) {
         pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-16);
      } else {
         assert(format == VK_FORMAT_D32_SFLOAT);
         if (d->vk.rs.depth_bias.representation ==
             VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT) {
            pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-24);
         } else {
            pa_su_poly_offset_db_fmt_cntl =
               S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-23) | S_028B78_POLY_OFFSET_DB_IS_FLOAT_FMT(1);
         }
      }
   }

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028B7C_PA_SU_POLY_OFFSET_CLAMP, 5);
   radeon_emit(cmd_buffer->cs, fui(d->vk.rs.depth_bias.clamp));    /* CLAMP */
   radeon_emit(cmd_buffer->cs, slope);                             /* FRONT SCALE */
   radeon_emit(cmd_buffer->cs, fui(d->vk.rs.depth_bias.constant)); /* FRONT OFFSET */
   radeon_emit(cmd_buffer->cs, slope);                             /* BACK SCALE */
   radeon_emit(cmd_buffer->cs, fui(d->vk.rs.depth_bias.constant)); /* BACK OFFSET */

   radeon_set_context_reg(cmd_buffer->cs, R_028B78_PA_SU_POLY_OFFSET_DB_FMT_CNTL, pa_su_poly_offset_db_fmt_cntl);
}

static void
radv_emit_line_stipple(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   /* GFX9 chips fail linestrip CTS tests unless this is set to 0 = no reset */
   uint32_t auto_reset_cntl = (gfx_level == GFX9) ? 0 : 2;

   if (radv_primitive_topology_is_line_list(d->vk.ia.primitive_topology))
      auto_reset_cntl = 1;

   radeon_set_context_reg(cmd_buffer->cs, R_028A0C_PA_SC_LINE_STIPPLE,
                          S_028A0C_LINE_PATTERN(d->vk.rs.line.stipple.pattern) |
                             S_028A0C_REPEAT_COUNT(d->vk.rs.line.stipple.factor - 1) |
                             S_028A0C_AUTO_RESET_CNTL(pdev->info.gfx_level < GFX12 ? auto_reset_cntl : 0));

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028A44_PA_SC_LINE_STIPPLE_RESET,
                             S_028A44_AUTO_RESET_CNTL(auto_reset_cntl));
   }
}

static void
radv_emit_culling(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned pa_su_sc_mode_cntl;

   pa_su_sc_mode_cntl =
      S_028814_CULL_FRONT(!!(d->vk.rs.cull_mode & VK_CULL_MODE_FRONT_BIT)) |
      S_028814_CULL_BACK(!!(d->vk.rs.cull_mode & VK_CULL_MODE_BACK_BIT)) | S_028814_FACE(d->vk.rs.front_face) |
      S_028814_POLY_OFFSET_FRONT_ENABLE(d->vk.rs.depth_bias.enable) |
      S_028814_POLY_OFFSET_BACK_ENABLE(d->vk.rs.depth_bias.enable) |
      S_028814_POLY_OFFSET_PARA_ENABLE(d->vk.rs.depth_bias.enable) |
      S_028814_POLY_MODE(d->vk.rs.polygon_mode != V_028814_X_DRAW_TRIANGLES) |
      S_028814_POLYMODE_FRONT_PTYPE(d->vk.rs.polygon_mode) | S_028814_POLYMODE_BACK_PTYPE(d->vk.rs.polygon_mode) |
      S_028814_PROVOKING_VTX_LAST(d->vk.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);

   if (gfx_level >= GFX10 && gfx_level < GFX12) {
      /* Ensure that SC processes the primitive group in the same order as PA produced them.  Needed
       * when either POLY_MODE or PERPENDICULAR_ENDCAP_ENA is set.
       */
      pa_su_sc_mode_cntl |=
         S_028814_KEEP_TOGETHER_ENABLE(d->vk.rs.polygon_mode != V_028814_X_DRAW_TRIANGLES ||
                                       radv_get_line_mode(cmd_buffer) == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR);
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_02881C_PA_SU_SC_MODE_CNTL, pa_su_sc_mode_cntl);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028814_PA_SU_SC_MODE_CNTL, pa_su_sc_mode_cntl);
   }
}

static void
radv_emit_provoking_vertex_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const unsigned stage = last_vgt_shader->info.stage;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const uint32_t ngg_provoking_vtx_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGG_PROVOKING_VTX);
   unsigned provoking_vtx = 0;

   if (!ngg_provoking_vtx_offset)
      return;

   if (d->vk.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT) {
      if (stage == MESA_SHADER_VERTEX) {
         provoking_vtx = radv_conv_prim_to_gs_out(d->vk.ia.primitive_topology, last_vgt_shader->info.is_ngg);
      } else {
         assert(stage == MESA_SHADER_GEOMETRY);
         provoking_vtx = last_vgt_shader->info.gs.vertices_in - 1;
      }
   }

   radeon_set_sh_reg(cmd_buffer->cs, ngg_provoking_vtx_offset, provoking_vtx);
}

static void
radv_emit_primitive_topology(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const uint32_t verts_per_prim_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NUM_VERTS_PER_PRIM);
   const uint32_t vgt_gs_out_prim_type = radv_get_rasterization_prim(cmd_buffer);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   assert(!cmd_buffer->state.mesh_shading);

   if (pdev->info.gfx_level >= GFX7) {
      uint32_t vgt_prim = d->vk.ia.primitive_topology;

      if (pdev->info.gfx_level >= GFX12)
         vgt_prim |= S_030908_NUM_INPUT_CP(d->vk.ts.patch_control_points);

      radeon_set_uconfig_reg_idx(&pdev->info, cmd_buffer->cs, R_030908_VGT_PRIMITIVE_TYPE, 1, vgt_prim);
   } else {
      radeon_set_config_reg(cmd_buffer->cs, R_008958_VGT_PRIMITIVE_TYPE, d->vk.ia.primitive_topology);
   }

   radv_emit_vgt_gs_out(cmd_buffer, vgt_gs_out_prim_type);

   if (!verts_per_prim_offset)
      return;

   radeon_set_sh_reg(cmd_buffer->cs, verts_per_prim_offset,
                     radv_conv_prim_to_gs_out(d->vk.ia.primitive_topology, last_vgt_shader->info.is_ngg) + 1);
}

static void
radv_emit_depth_control(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const bool stencil_test_enable =
      d->vk.ds.stencil.test_enable && (render->ds_att_aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
   const uint32_t db_depth_control =
      S_028800_Z_ENABLE(d->vk.ds.depth.test_enable ? 1 : 0) |
      S_028800_Z_WRITE_ENABLE(d->vk.ds.depth.write_enable ? 1 : 0) | S_028800_ZFUNC(d->vk.ds.depth.compare_op) |
      S_028800_DEPTH_BOUNDS_ENABLE(d->vk.ds.depth.bounds_test.enable ? 1 : 0) |
      S_028800_STENCIL_ENABLE(stencil_test_enable) | S_028800_BACKFACE_ENABLE(stencil_test_enable) |
      S_028800_STENCILFUNC(d->vk.ds.stencil.front.op.compare) |
      S_028800_STENCILFUNC_BF(d->vk.ds.stencil.back.op.compare);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028070_DB_DEPTH_CONTROL, db_depth_control);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028800_DB_DEPTH_CONTROL, db_depth_control);
   }
}

static void
radv_emit_stencil_control(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const uint32_t db_stencil_control =
      S_02842C_STENCILFAIL(radv_translate_stencil_op(d->vk.ds.stencil.front.op.fail)) |
      S_02842C_STENCILZPASS(radv_translate_stencil_op(d->vk.ds.stencil.front.op.pass)) |
      S_02842C_STENCILZFAIL(radv_translate_stencil_op(d->vk.ds.stencil.front.op.depth_fail)) |
      S_02842C_STENCILFAIL_BF(radv_translate_stencil_op(d->vk.ds.stencil.back.op.fail)) |
      S_02842C_STENCILZPASS_BF(radv_translate_stencil_op(d->vk.ds.stencil.back.op.pass)) |
      S_02842C_STENCILZFAIL_BF(radv_translate_stencil_op(d->vk.ds.stencil.back.op.depth_fail));

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028074_DB_STENCIL_CONTROL, db_stencil_control);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_02842C_DB_STENCIL_CONTROL, db_stencil_control);
   }
}

static bool
radv_should_force_vrs1x1(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   return pdev->info.gfx_level >= GFX10_3 &&
          (cmd_buffer->state.ms.sample_shading_enable || (ps && ps->info.ps.force_sample_iter_shading_rate));
}

static void
radv_emit_fragment_shading_rate(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   /* When per-vertex VRS is forced and the dynamic fragment shading rate is a no-op, ignore
    * it. This is needed for vkd3d-proton because it always declares per-draw VRS as dynamic.
    */
   if (device->force_vrs != RADV_FORCE_VRS_1x1 && d->vk.fsr.fragment_size.width == 1 &&
       d->vk.fsr.fragment_size.height == 1 &&
       d->vk.fsr.combiner_ops[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR &&
       d->vk.fsr.combiner_ops[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR)
      return;

   uint32_t rate_x = MIN2(2, d->vk.fsr.fragment_size.width) - 1;
   uint32_t rate_y = MIN2(2, d->vk.fsr.fragment_size.height) - 1;
   uint32_t pipeline_comb_mode = d->vk.fsr.combiner_ops[0];
   uint32_t htile_comb_mode = d->vk.fsr.combiner_ops[1];
   uint32_t pa_cl_vrs_cntl = 0;

   assert(pdev->info.gfx_level >= GFX10_3);

   if (!cmd_buffer->state.render.vrs_att.iview) {
      /* When the current subpass has no VRS attachment, the VRS rates are expected to be 1x1, so we
       * can cheat by tweaking the different combiner modes.
       */
      switch (htile_comb_mode) {
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR:
         /* The result of min(A, 1x1) is always 1x1. */
         FALLTHROUGH;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR:
         /* Force the per-draw VRS rate to 1x1. */
         rate_x = rate_y = 0;

         /* As the result of min(A, 1x1) or replace(A, 1x1) are always 1x1, set the vertex rate
          * combiner mode as passthrough.
          */
         pipeline_comb_mode = V_028848_SC_VRS_COMB_MODE_PASSTHRU;
         break;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR:
         /* The result of max(A, 1x1) is always A. */
         FALLTHROUGH;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR:
         /* Nothing to do here because the SAMPLE_ITER combiner mode should already be passthrough. */
         break;
      default:
         break;
      }
   }

   /* Emit per-draw VRS rate which is the first combiner. */
   radeon_set_uconfig_reg(cmd_buffer->cs, R_03098C_GE_VRS_RATE, S_03098C_RATE_X(rate_x) | S_03098C_RATE_Y(rate_y));

   /* Disable VRS and use the rates from PS_ITER_SAMPLES if:
    *
    * 1) sample shading is enabled or per-sample interpolation is used by the fragment shader
    * 2) the fragment shader requires 1x1 shading rate for some other reason
    */
   if (radv_should_force_vrs1x1(cmd_buffer)) {
      pa_cl_vrs_cntl |= S_028848_SAMPLE_ITER_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE);
   }

   /* VERTEX_RATE_COMBINER_MODE controls the combiner mode between the
    * draw rate and the vertex rate.
    */
   if (cmd_buffer->state.mesh_shading) {
      pa_cl_vrs_cntl |= S_028848_VERTEX_RATE_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_PASSTHRU) |
                        S_028848_PRIMITIVE_RATE_COMBINER_MODE(pipeline_comb_mode);
   } else {
      pa_cl_vrs_cntl |= S_028848_VERTEX_RATE_COMBINER_MODE(pipeline_comb_mode) |
                        S_028848_PRIMITIVE_RATE_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_PASSTHRU);
   }

   /* HTILE_RATE_COMBINER_MODE controls the combiner mode between the primitive rate and the HTILE
    * rate.
    */
   pa_cl_vrs_cntl |= S_028848_HTILE_RATE_COMBINER_MODE(htile_comb_mode);

   radeon_set_context_reg(cmd_buffer->cs, R_028848_PA_CL_VRS_CNTL, pa_cl_vrs_cntl);
}

static uint32_t
radv_get_primitive_reset_index(const struct radv_cmd_buffer *cmd_buffer)
{
   const uint32_t index_type = G_028A7C_INDEX_TYPE(cmd_buffer->state.index_type);
   switch (index_type) {
   case V_028A7C_VGT_INDEX_8:
      return 0xffu;
   case V_028A7C_VGT_INDEX_16:
      return 0xffffu;
   case V_028A7C_VGT_INDEX_32:
      return 0xffffffffu;
   default:
      unreachable("invalid index type");
   }
}

static void
radv_emit_primitive_restart_enable(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const struct radv_dynamic_state *const d = &cmd_buffer->state.dynamic;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const bool en = d->vk.ia.primitive_restart_enable;

   if (gfx_level >= GFX11) {
      radeon_set_uconfig_reg(cs, R_03092C_GE_MULTI_PRIM_IB_RESET_EN,
                             S_03092C_RESET_EN(en) |
                                /* This disables primitive restart for non-indexed draws.
                                 * By keeping this set, we don't have to unset RESET_EN
                                 * for non-indexed draws. */
                                S_03092C_DISABLE_FOR_AUTO_INDEX(1));
   } else if (gfx_level >= GFX9) {
      radeon_set_uconfig_reg(cs, R_03092C_VGT_MULTI_PRIM_IB_RESET_EN, en);
   } else {
      radeon_set_context_reg(cs, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN, en);

      /* GFX6-7: All 32 bits are compared.
       * GFX8: Only index type bits are compared.
       * GFX9+: Default is same as GFX8, MATCH_ALL_BITS=1 selects GFX6-7 behavior
       */
      if (en && gfx_level <= GFX7) {
         const uint32_t primitive_reset_index = radv_get_primitive_reset_index(cmd_buffer);

         radeon_opt_set_context_reg(cmd_buffer, R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX,
                                    RADV_TRACKED_VGT_MULTI_PRIM_IB_RESET_INDX, primitive_reset_index);
      }
   }
}

static void
radv_emit_clipping(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   bool depth_clip_enable = radv_get_depth_clip_enable(cmd_buffer);

   radeon_set_context_reg(
      cmd_buffer->cs, R_028810_PA_CL_CLIP_CNTL,
      S_028810_DX_RASTERIZATION_KILL(d->vk.rs.rasterizer_discard_enable) |
         S_028810_ZCLIP_NEAR_DISABLE(!depth_clip_enable) | S_028810_ZCLIP_FAR_DISABLE(!depth_clip_enable) |
         S_028810_DX_CLIP_SPACE_DEF(!d->vk.vp.depth_clip_negative_one_to_one) | S_028810_DX_LINEAR_ATTR_CLIP_ENA(1));
}

static bool
radv_is_mrt0_dual_src(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (!d->vk.cb.attachments[0].write_mask || !d->vk.cb.attachments[0].blend_enable)
      return false;

   return radv_can_enable_dual_src(&d->vk.cb.attachments[0]);
}

static void
radv_emit_logic_op(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned cb_color_control = 0;

   if (d->vk.cb.logic_op_enable) {
      cb_color_control |= S_028808_ROP3(d->vk.cb.logic_op);
   } else {
      cb_color_control |= S_028808_ROP3(V_028808_ROP3_COPY);
   }

   if (pdev->info.has_rbplus) {
      /* RB+ doesn't work with dual source blending, logic op and CB_RESOLVE. */
      bool mrt0_is_dual_src = radv_is_mrt0_dual_src(cmd_buffer);

      cb_color_control |= S_028808_DISABLE_DUAL_QUAD(mrt0_is_dual_src || d->vk.cb.logic_op_enable ||
                                                     cmd_buffer->state.custom_blend_mode == V_028808_CB_RESOLVE);
   }

   if (cmd_buffer->state.custom_blend_mode) {
      cb_color_control |= S_028808_MODE(cmd_buffer->state.custom_blend_mode);
   } else {
      bool color_write_enabled = false;

      for (unsigned i = 0; i < MAX_RTS; i++) {
         if (d->vk.cb.attachments[i].write_mask) {
            color_write_enabled = true;
            break;
         }
      }

      if (color_write_enabled) {
         cb_color_control |= S_028808_MODE(V_028808_CB_NORMAL);
      } else {
         cb_color_control |= S_028808_MODE(V_028808_CB_DISABLE);
      }
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028858_CB_COLOR_CONTROL, cb_color_control);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028808_CB_COLOR_CONTROL, cb_color_control);
   }
}

static void
radv_emit_color_write(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_binning_settings *settings = &pdev->binning_settings;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t color_write_enable = 0, color_write_mask = 0;

   u_foreach_bit (i, d->vk.cb.color_write_enables) {
      color_write_enable |= 0xfu << (i * 4);
   }

   for (unsigned i = 0; i < MAX_RTS; i++) {
      color_write_mask |= d->vk.cb.attachments[i].write_mask << (4 * i);
   }

   if (device->pbb_allowed && settings->context_states_per_bin > 1) {
      /* Flush DFSM on CB_TARGET_MASK changes. */
      radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_BREAK_BATCH) | EVENT_INDEX(0));
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028850_CB_TARGET_MASK, color_write_mask & color_write_enable);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028238_CB_TARGET_MASK, color_write_mask & color_write_enable);
   }
}

static void
radv_emit_patch_control_points(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *vs = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   const struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];
   const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned ls_hs_config;

   /* Compute tessellation info that depends on the number of patch control points when this state
    * is dynamic.
    */
   if (cmd_buffer->state.uses_dynamic_patch_control_points) {
      /* Compute the number of patches. */
      cmd_buffer->state.tess_num_patches = radv_get_tcs_num_patches(
         pdev, d->vk.ts.patch_control_points, tcs->info.tcs.tcs_vertices_out, vs->info.vs.num_linked_outputs,
         tcs->info.tcs.num_lds_per_vertex_outputs, tcs->info.tcs.num_lds_per_patch_outputs,
         tcs->info.tcs.num_linked_outputs, tcs->info.tcs.num_linked_patch_outputs);

      /* Compute the LDS size. */
      cmd_buffer->state.tess_lds_size =
         radv_get_tess_lds_size(pdev, d->vk.ts.patch_control_points, tcs->info.tcs.tcs_vertices_out,
                                vs->info.vs.num_linked_outputs, cmd_buffer->state.tess_num_patches,
                                tcs->info.tcs.num_lds_per_vertex_outputs, tcs->info.tcs.num_lds_per_patch_outputs);
   }

   ls_hs_config = S_028B58_NUM_PATCHES(cmd_buffer->state.tess_num_patches) |
                  /* GFX12 programs patch_vertices in VGT_PRIMITIVE_TYPE.NUM_INPUT_CP. */
                  S_028B58_HS_NUM_INPUT_CP(pdev->info.gfx_level < GFX12 ? d->vk.ts.patch_control_points : 0) |
                  S_028B58_HS_NUM_OUTPUT_CP(tcs->info.tcs.tcs_vertices_out);

   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_context_reg_idx(cmd_buffer->cs, R_028B58_VGT_LS_HS_CONFIG, 2, ls_hs_config);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028B58_VGT_LS_HS_CONFIG, ls_hs_config);
   }

   if (pdev->info.gfx_level >= GFX9) {
      unsigned hs_rsrc2;

      if (tcs->info.merged_shader_compiled_separately) {
         radv_shader_combine_cfg_vs_tcs(cmd_buffer->state.shaders[MESA_SHADER_VERTEX], tcs, NULL, &hs_rsrc2);
      } else {
         hs_rsrc2 = tcs->config.rsrc2;
      }

      if (pdev->info.gfx_level >= GFX10) {
         hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX10(cmd_buffer->state.tess_lds_size);
      } else {
         hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX9(cmd_buffer->state.tess_lds_size);
      }

      radeon_set_sh_reg(cmd_buffer->cs, R_00B42C_SPI_SHADER_PGM_RSRC2_HS, hs_rsrc2);
   } else {
      unsigned ls_rsrc2 = vs->config.rsrc2 | S_00B52C_LDS_SIZE(cmd_buffer->state.tess_lds_size);

      radeon_set_sh_reg(cmd_buffer->cs, R_00B52C_SPI_SHADER_PGM_RSRC2_LS, ls_rsrc2);
   }

   /* Emit user SGPRs for dynamic patch control points. */
   uint32_t tcs_offchip_layout_offset = radv_get_user_sgpr_loc(tcs, AC_UD_TCS_OFFCHIP_LAYOUT);
   if (!tcs_offchip_layout_offset)
      return;

   unsigned tcs_offchip_layout =
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PATCH_CONTROL_POINTS, d->vk.ts.patch_control_points - 1) |
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_OUT_PATCH_CP, tcs->info.tcs.tcs_vertices_out - 1) |
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_PATCHES, cmd_buffer->state.tess_num_patches - 1) |
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_LS_OUTPUTS, vs->info.vs.num_linked_outputs) |
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_HS_OUTPUTS, tcs->info.tcs.num_linked_outputs) |
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_TES_READS_TF, tes->info.tes.reads_tess_factors) |
      SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PRIMITIVE_MODE, tes->info.tes._primitive_mode);

   radeon_set_sh_reg(cmd_buffer->cs, tcs_offchip_layout_offset, tcs_offchip_layout);

   tcs_offchip_layout_offset = radv_get_user_sgpr_loc(tes, AC_UD_TCS_OFFCHIP_LAYOUT);
   assert(tcs_offchip_layout_offset);

   radeon_set_sh_reg(cmd_buffer->cs, tcs_offchip_layout_offset, tcs_offchip_layout);
}

static void
radv_emit_conservative_rast_mode(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (pdev->info.gfx_level >= GFX9) {
      uint32_t pa_sc_conservative_rast;

      if (d->vk.rs.conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT) {
         const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
         const bool uses_inner_coverage = ps && ps->info.ps.reads_fully_covered;

         pa_sc_conservative_rast =
            S_028C4C_PREZ_AA_MASK_ENABLE(1) | S_028C4C_POSTZ_AA_MASK_ENABLE(1) | S_028C4C_CENTROID_SAMPLE_OVERRIDE(1);

         /* Inner coverage requires underestimate conservative rasterization. */
         if (d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT &&
             !uses_inner_coverage) {
            pa_sc_conservative_rast |= S_028C4C_OVER_RAST_ENABLE(1) | S_028C4C_UNDER_RAST_SAMPLE_SELECT(1) |
                                       S_028C4C_PBB_UNCERTAINTY_REGION_ENABLE(1);
         } else {
            pa_sc_conservative_rast |= S_028C4C_OVER_RAST_SAMPLE_SELECT(1) | S_028C4C_UNDER_RAST_ENABLE(1);
         }
      } else {
         pa_sc_conservative_rast = S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1);
      }

      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg(cmd_buffer->cs, R_028C54_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
                                pa_sc_conservative_rast);
      } else {
         radeon_set_context_reg(cmd_buffer->cs, R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
                                pa_sc_conservative_rast);
      }
   }
}

static void
radv_emit_depth_clamp_enable(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   enum radv_depth_clamp_mode mode = radv_get_depth_clamp_mode(cmd_buffer);

   radeon_set_context_reg(
      cmd_buffer->cs, R_02800C_DB_RENDER_OVERRIDE,
      S_02800C_FORCE_HIS_ENABLE0(V_02800C_FORCE_DISABLE) | S_02800C_FORCE_HIS_ENABLE1(V_02800C_FORCE_DISABLE) |
         S_02800C_DISABLE_VIEWPORT_CLAMP(pdev->info.gfx_level < GFX12 && mode == RADV_DEPTH_CLAMP_MODE_DISABLED));

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028064_DB_VIEWPORT_CONTROL,
                             S_028064_DISABLE_VIEWPORT_CLAMP(mode == RADV_DEPTH_CLAMP_MODE_DISABLED));
   }
}

static void
radv_emit_rasterization_samples(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
   unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned spi_baryc_cntl = S_0286E0_FRONT_FACE_ALL_BITS(1);
   unsigned pa_sc_mode_cntl_1;
   bool has_hiz_his = false;

   if (pdev->info.gfx_level >= GFX12) {
      const struct radv_rendering_state *render = &cmd_buffer->state.render;

      if (render->ds_att.iview) {
         const struct radeon_surf *surf = &render->ds_att.iview->image->planes[0].surface;
         has_hiz_his = surf->u.gfx9.zs.hiz.offset || surf->u.gfx9.zs.his.offset;
      }
   }

   pa_sc_mode_cntl_1 =
      S_028A4C_WALK_FENCE_ENABLE(1) | // TODO linear dst fixes
      S_028A4C_WALK_FENCE_SIZE(pdev->info.num_tile_pipes == 2 ? 2 : 3) |
      S_028A4C_OUT_OF_ORDER_PRIMITIVE_ENABLE(cmd_buffer->state.uses_out_of_order_rast) |
      S_028A4C_OUT_OF_ORDER_WATER_MARK(pdev->info.gfx_level >= GFX12 ? 0 : 0x7) |
      /* always 1: */
      S_028A4C_SUPERTILE_WALK_ORDER_ENABLE(1) | S_028A4C_TILE_WALK_ORDER_ENABLE(1) |
      S_028A4C_MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE(1) | S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) |
      S_028A4C_FORCE_EOV_REZ_ENABLE(1) |
      /* This should only be set when VRS surfaces aren't enabled on GFX11, otherwise the GPU might
       * hang.
       */
      S_028A4C_WALK_ALIGN8_PRIM_FITS_ST(pdev->info.gfx_level < GFX11 || !cmd_buffer->state.uses_vrs_attachment ||
                                        (pdev->info.gfx_level >= GFX12 && !has_hiz_his));

   if (!d->sample_location.count)
      radv_emit_default_sample_locations(pdev, cmd_buffer->cs, rasterization_samples);

   if (ps_iter_samples > 1) {
      spi_baryc_cntl |= S_0286E0_POS_FLOAT_LOCATION(2);
      pa_sc_mode_cntl_1 |= S_028A4C_PS_ITER_SAMPLE(1);
   }

   if (radv_should_force_vrs1x1(cmd_buffer)) {
      /* Make sure sample shading is enabled even if only MSAA1x is used because the SAMPLE_ITER
       * combiner is in passthrough mode if PS_ITER_SAMPLE is 0, and it uses the per-draw rate. The
       * default VRS rate when sample shading is enabled is 1x1.
       */
      if (!G_028A4C_PS_ITER_SAMPLE(pa_sc_mode_cntl_1))
         pa_sc_mode_cntl_1 |= S_028A4C_PS_ITER_SAMPLE(1);
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028658_SPI_BARYC_CNTL, spi_baryc_cntl);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_0286E0_SPI_BARYC_CNTL, spi_baryc_cntl);
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028A4C_PA_SC_MODE_CNTL_1, pa_sc_mode_cntl_1);
}

static void
radv_emit_fb_color_state(struct radv_cmd_buffer *cmd_buffer, int index, struct radv_color_buffer_info *cb,
                         struct radv_image_view *iview, VkImageLayout layout)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool is_vi = pdev->info.gfx_level >= GFX8;
   uint32_t cb_fdcc_control = cb->ac.cb_dcc_control;
   uint32_t cb_color_info = cb->ac.cb_color_info;
   struct radv_image *image = iview->image;

   if (!radv_layout_dcc_compressed(device, image, iview->vk.base_mip_level, layout,
                                   radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf))) {
      if (pdev->info.gfx_level >= GFX11) {
         cb_fdcc_control &= C_028C78_FDCC_ENABLE;
      } else {
         cb_color_info &= C_028C70_DCC_ENABLE;
      }
   }

   const enum radv_fmask_compression fmask_comp = radv_layout_fmask_compression(
      device, image, layout, radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf));
   if (fmask_comp == RADV_FMASK_COMPRESSION_NONE) {
      cb_color_info &= C_028C70_COMPRESSION;
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x24, cb->ac.cb_color_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028C64_CB_COLOR0_VIEW + index * 0x24, cb->ac.cb_color_view);
      radeon_set_context_reg(cmd_buffer->cs, R_028C68_CB_COLOR0_VIEW2 + index * 0x24, cb->ac.cb_color_view2);
      radeon_set_context_reg(cmd_buffer->cs, R_028C6C_CB_COLOR0_ATTRIB + index * 0x24, cb->ac.cb_color_attrib);
      radeon_set_context_reg(cmd_buffer->cs, R_028C70_CB_COLOR0_FDCC_CONTROL + index * 0x24, cb_fdcc_control);
      radeon_set_context_reg(cmd_buffer->cs, R_028C78_CB_COLOR0_ATTRIB2 + index * 0x24, cb->ac.cb_color_attrib2);
      radeon_set_context_reg(cmd_buffer->cs, R_028C7C_CB_COLOR0_ATTRIB3 + index * 0x24, cb->ac.cb_color_attrib3);
      radeon_set_context_reg(cmd_buffer->cs, R_028E40_CB_COLOR0_BASE_EXT + index * 4,
                             S_028E40_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_INFO + index * 4, cb->ac.cb_color_info);
   } else if (pdev->info.gfx_level >= GFX11) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C6C_CB_COLOR0_VIEW + index * 0x3c, 4);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_view);    /* CB_COLOR0_VIEW */
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_info);    /* CB_COLOR0_INFO */
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_attrib);  /* CB_COLOR0_ATTRIB */
      radeon_emit(cmd_buffer->cs, cb_fdcc_control);     /* CB_COLOR0_FDCC_CONTROL */

      radeon_set_context_reg(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, cb->ac.cb_color_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028E40_CB_COLOR0_BASE_EXT + index * 4,
                             S_028E40_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->ac.cb_dcc_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028EA0_CB_COLOR0_DCC_BASE_EXT + index * 4,
                             S_028EA0_BASE_256B(cb->ac.cb_dcc_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_ATTRIB2 + index * 4, cb->ac.cb_color_attrib2);
      radeon_set_context_reg(cmd_buffer->cs, R_028EE0_CB_COLOR0_ATTRIB3 + index * 4, cb->ac.cb_color_attrib3);
   } else if (pdev->info.gfx_level >= GFX10) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_base);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_view);
      radeon_emit(cmd_buffer->cs, cb_color_info);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_attrib);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_dcc_control);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_cmask);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_fmask);
      radeon_emit(cmd_buffer->cs, 0);

      radeon_set_context_reg(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->ac.cb_dcc_base);

      radeon_set_context_reg(cmd_buffer->cs, R_028E40_CB_COLOR0_BASE_EXT + index * 4,
                             S_028E40_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028E60_CB_COLOR0_CMASK_BASE_EXT + index * 4,
                             S_028E60_BASE_256B(cb->ac.cb_color_cmask >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028E80_CB_COLOR0_FMASK_BASE_EXT + index * 4,
                             S_028E80_BASE_256B(cb->ac.cb_color_fmask >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028EA0_CB_COLOR0_DCC_BASE_EXT + index * 4,
                             S_028EA0_BASE_256B(cb->ac.cb_dcc_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_ATTRIB2 + index * 4, cb->ac.cb_color_attrib2);
      radeon_set_context_reg(cmd_buffer->cs, R_028EE0_CB_COLOR0_ATTRIB3 + index * 4, cb->ac.cb_color_attrib3);
   } else if (pdev->info.gfx_level == GFX9) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_base);
      radeon_emit(cmd_buffer->cs, S_028C64_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_attrib2);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_view);
      radeon_emit(cmd_buffer->cs, cb_color_info);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_attrib);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_dcc_control);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_cmask);
      radeon_emit(cmd_buffer->cs, S_028C80_BASE_256B(cb->ac.cb_color_cmask >> 32));
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_fmask);
      radeon_emit(cmd_buffer->cs, S_028C88_BASE_256B(cb->ac.cb_color_fmask >> 32));

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, 2);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_dcc_base);
      radeon_emit(cmd_buffer->cs, S_028C98_BASE_256B(cb->ac.cb_dcc_base >> 32));

      radeon_set_context_reg(cmd_buffer->cs, R_0287A0_CB_MRT0_EPITCH + index * 4, cb->ac.cb_mrt_epitch);
   } else {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, 6);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_base);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_pitch);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_slice);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_view);
      radeon_emit(cmd_buffer->cs, cb_color_info);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_attrib);

      if (pdev->info.gfx_level == GFX8)
         radeon_set_context_reg(cmd_buffer->cs, R_028C78_CB_COLOR0_DCC_CONTROL + index * 0x3c, cb->ac.cb_dcc_control);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C7C_CB_COLOR0_CMASK + index * 0x3c, 4);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_cmask);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_cmask_slice);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_fmask);
      radeon_emit(cmd_buffer->cs, cb->ac.cb_color_fmask_slice);

      if (is_vi) { /* DCC BASE */
         radeon_set_context_reg(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->ac.cb_dcc_base);
      }
   }

   if (pdev->info.gfx_level >= GFX11 ? G_028C78_FDCC_ENABLE(cb_fdcc_control) : G_028C70_DCC_ENABLE(cb_color_info)) {
      /* Drawing with DCC enabled also compresses colorbuffers. */
      VkImageSubresourceRange range = {
         .aspectMask = iview->vk.aspects,
         .baseMipLevel = iview->vk.base_mip_level,
         .levelCount = iview->vk.level_count,
         .baseArrayLayer = iview->vk.base_array_layer,
         .layerCount = iview->vk.layer_count,
      };

      radv_update_dcc_metadata(cmd_buffer, image, &range, true);
   }
}

static void
radv_update_zrange_precision(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds,
                             const struct radv_image_view *iview, bool requires_cond_exec)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_image *image = iview->image;
   uint32_t db_z_info = ds->ac.db_z_info;
   uint32_t db_z_info_reg;

   if (!pdev->info.has_tc_compat_zrange_bug || !radv_image_is_tc_compat_htile(image))
      return;

   db_z_info &= C_028040_ZRANGE_PRECISION;

   if (pdev->info.gfx_level == GFX9) {
      db_z_info_reg = R_028038_DB_Z_INFO;
   } else {
      db_z_info_reg = R_028040_DB_Z_INFO;
   }

   /* When we don't know the last fast clear value we need to emit a
    * conditional packet that will eventually skip the following
    * SET_CONTEXT_REG packet.
    */
   if (requires_cond_exec) {
      uint64_t va = radv_get_tc_compat_zrange_va(image, iview->vk.base_mip_level);

      radv_emit_cond_exec(device, cmd_buffer->cs, va, 3 /* SET_CONTEXT_REG size */);
   }

   radeon_set_context_reg(cmd_buffer->cs, db_z_info_reg, db_z_info);
}

static struct radv_image *
radv_cmd_buffer_get_vrs_image(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!device->vrs.image) {
      VkResult result;

      /* The global VRS state is initialized on-demand to avoid wasting VRAM. */
      result = radv_device_init_vrs_state(device);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd_buffer->vk, result);
         return NULL;
      }
   }

   return device->vrs.image;
}

static void
radv_emit_fb_ds_state(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds, struct radv_image_view *iview,
                      bool depth_compressed, bool stencil_compressed)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t db_htile_data_base = ds->ac.u.gfx6.db_htile_data_base;
   uint32_t db_htile_surface = ds->ac.u.gfx6.db_htile_surface;
   uint32_t db_render_control = ds->db_render_control | cmd_buffer->state.db_render_control;
   uint32_t db_z_info = ds->ac.db_z_info;

   if (!depth_compressed)
      db_render_control |= S_028000_DEPTH_COMPRESS_DISABLE(1);
   if (!stencil_compressed)
      db_render_control |= S_028000_STENCIL_COMPRESS_DISABLE(1);

   if (pdev->info.gfx_level == GFX10_3) {
      if (!cmd_buffer->state.render.vrs_att.iview) {
         db_htile_surface &= C_028ABC_VRS_HTILE_ENCODING;
      } else {
         /* On GFX10.3, when a subpass uses VRS attachment but HTILE can't be enabled, we fallback to
          * our internal HTILE buffer.
          */
         if (!radv_htile_enabled(iview->image, iview->vk.base_mip_level) && radv_cmd_buffer_get_vrs_image(cmd_buffer)) {
            struct radv_buffer *htile_buffer = device->vrs.buffer;

            assert(!G_028038_TILE_SURFACE_ENABLE(db_z_info) && !db_htile_data_base && !db_htile_surface);
            db_z_info |= S_028038_TILE_SURFACE_ENABLE(1);
            db_htile_data_base = radv_buffer_get_va(htile_buffer->bo) >> 8;
            db_htile_surface = S_028ABC_FULL_CACHE(1) | S_028ABC_PIPE_ALIGNED(1) |
                               S_028ABC_VRS_HTILE_ENCODING(V_028ABC_VRS_HTILE_4BIT_ENCODING);
         }
      }
   }

   if (pdev->info.gfx_level < GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028000_DB_RENDER_CONTROL, db_render_control);
      radeon_set_context_reg(cmd_buffer->cs, R_028008_DB_DEPTH_VIEW, ds->ac.db_depth_view);
      radeon_set_context_reg(cmd_buffer->cs, R_028ABC_DB_HTILE_SURFACE, db_htile_surface);
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028010_DB_RENDER_OVERRIDE2, ds->db_render_override2);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028004_DB_DEPTH_VIEW, ds->ac.db_depth_view);
      radeon_set_context_reg(cmd_buffer->cs, R_028008_DB_DEPTH_VIEW1, ds->ac.u.gfx12.db_depth_view1);
      radeon_set_context_reg(cmd_buffer->cs, R_028014_DB_DEPTH_SIZE_XY, ds->ac.db_depth_size);
      radeon_set_context_reg(cmd_buffer->cs, R_028018_DB_Z_INFO, ds->ac.db_z_info);
      radeon_set_context_reg(cmd_buffer->cs, R_02801C_DB_STENCIL_INFO, ds->ac.db_stencil_info);
      radeon_set_context_reg(cmd_buffer->cs, R_028020_DB_Z_READ_BASE, ds->ac.db_depth_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028024_DB_Z_READ_BASE_HI, S_028024_BASE_HI(ds->ac.db_depth_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028028_DB_Z_WRITE_BASE, ds->ac.db_depth_base);
      radeon_set_context_reg(cmd_buffer->cs, R_02802C_DB_Z_WRITE_BASE_HI, S_02802C_BASE_HI(ds->ac.db_depth_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028030_DB_STENCIL_READ_BASE, ds->ac.db_stencil_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028034_DB_STENCIL_READ_BASE_HI,
                             S_028034_BASE_HI(ds->ac.db_stencil_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028038_DB_STENCIL_WRITE_BASE, ds->ac.db_stencil_base);
      radeon_set_context_reg(cmd_buffer->cs, R_02803C_DB_STENCIL_WRITE_BASE_HI,
                             S_02803C_BASE_HI(ds->ac.db_stencil_base >> 32));
      radeon_set_context_reg(cmd_buffer->cs, R_028B94_PA_SC_HIZ_INFO, ds->ac.u.gfx12.hiz_info);
      radeon_set_context_reg(cmd_buffer->cs, R_028B98_PA_SC_HIS_INFO, ds->ac.u.gfx12.his_info);

      if (ds->ac.u.gfx12.hiz_info) {
         radeon_set_context_reg(cmd_buffer->cs, R_028B9C_PA_SC_HIZ_BASE, ds->ac.u.gfx12.hiz_base);
         radeon_set_context_reg(cmd_buffer->cs, R_028BA0_PA_SC_HIZ_BASE_EXT,
                                S_028BA0_BASE_256B(ds->ac.u.gfx12.hiz_base >> 32));
         radeon_set_context_reg(cmd_buffer->cs, R_028BA4_PA_SC_HIZ_SIZE_XY, ds->ac.u.gfx12.hiz_size_xy);
      }
      if (ds->ac.u.gfx12.his_info) {
         radeon_set_context_reg(cmd_buffer->cs, R_028BA8_PA_SC_HIS_BASE, ds->ac.u.gfx12.his_base);
         radeon_set_context_reg(cmd_buffer->cs, R_028BAC_PA_SC_HIS_BASE_EXT,
                                S_028BAC_BASE_256B(ds->ac.u.gfx12.his_base >> 32));
         radeon_set_context_reg(cmd_buffer->cs, R_028BB0_PA_SC_HIS_SIZE_XY, ds->ac.u.gfx12.his_size_xy);
      }
   } else if (pdev->info.gfx_level >= GFX10) {
      radeon_set_context_reg(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, db_htile_data_base);
      radeon_set_context_reg(cmd_buffer->cs, R_02801C_DB_DEPTH_SIZE_XY, ds->ac.db_depth_size);

      if (pdev->info.gfx_level >= GFX11) {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028040_DB_Z_INFO, 6);
      } else {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_02803C_DB_DEPTH_INFO, 7);
         radeon_emit(cmd_buffer->cs, S_02803C_RESOURCE_LEVEL(1));
      }
      radeon_emit(cmd_buffer->cs, db_z_info);
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_info);
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_base);
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_base);
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_base);
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_base);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028068_DB_Z_READ_BASE_HI, 5);
      radeon_emit(cmd_buffer->cs, S_028068_BASE_HI(ds->ac.db_depth_base >> 32));
      radeon_emit(cmd_buffer->cs, S_02806C_BASE_HI(ds->ac.db_stencil_base >> 32));
      radeon_emit(cmd_buffer->cs, S_028070_BASE_HI(ds->ac.db_depth_base >> 32));
      radeon_emit(cmd_buffer->cs, S_028074_BASE_HI(ds->ac.db_stencil_base >> 32));
      radeon_emit(cmd_buffer->cs, S_028078_BASE_HI(db_htile_data_base >> 32));
   } else if (pdev->info.gfx_level == GFX9) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, 3);
      radeon_emit(cmd_buffer->cs, db_htile_data_base);
      radeon_emit(cmd_buffer->cs, S_028018_BASE_HI(db_htile_data_base >> 32));
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_size);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028038_DB_Z_INFO, 10);
      radeon_emit(cmd_buffer->cs, db_z_info);                                         /* DB_Z_INFO */
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_info);                            /* DB_STENCIL_INFO */
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_base);                              /* DB_Z_READ_BASE */
      radeon_emit(cmd_buffer->cs, S_028044_BASE_HI(ds->ac.db_depth_base >> 32));      /* DB_Z_READ_BASE_HI */
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_base);                            /* DB_STENCIL_READ_BASE */
      radeon_emit(cmd_buffer->cs, S_02804C_BASE_HI(ds->ac.db_stencil_base >> 32));    /* DB_STENCIL_READ_BASE_HI */
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_base);                              /* DB_Z_WRITE_BASE */
      radeon_emit(cmd_buffer->cs, S_028054_BASE_HI(ds->ac.db_depth_base >> 32));      /* DB_Z_WRITE_BASE_HI */
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_base);                            /* DB_STENCIL_WRITE_BASE */
      radeon_emit(cmd_buffer->cs, S_02805C_BASE_HI(ds->ac.db_stencil_base >> 32));    /* DB_STENCIL_WRITE_BASE_HI */

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028068_DB_Z_INFO2, 2);
      radeon_emit(cmd_buffer->cs, ds->ac.u.gfx6.db_z_info2);
      radeon_emit(cmd_buffer->cs, ds->ac.u.gfx6.db_stencil_info2);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, db_htile_data_base);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_02803C_DB_DEPTH_INFO, 9);
      radeon_emit(cmd_buffer->cs, ds->ac.u.gfx6.db_depth_info);  /* R_02803C_DB_DEPTH_INFO */
      radeon_emit(cmd_buffer->cs, db_z_info);                    /* R_028040_DB_Z_INFO */
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_info);       /* R_028044_DB_STENCIL_INFO */
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_base);         /* R_028048_DB_Z_READ_BASE */
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_base);       /* R_02804C_DB_STENCIL_READ_BASE */
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_base);         /* R_028050_DB_Z_WRITE_BASE */
      radeon_emit(cmd_buffer->cs, ds->ac.db_stencil_base);       /* R_028054_DB_STENCIL_WRITE_BASE */
      radeon_emit(cmd_buffer->cs, ds->ac.db_depth_size);         /* R_028058_DB_DEPTH_SIZE */
      radeon_emit(cmd_buffer->cs, ds->ac.u.gfx6.db_depth_slice); /* R_02805C_DB_DEPTH_SLICE */
   }

   /* Update the ZRANGE_PRECISION value for the TC-compat bug. */
   radv_update_zrange_precision(cmd_buffer, ds, iview, true);
}

static void
radv_emit_null_ds_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028018_DB_Z_INFO, 2);
      radeon_emit(cmd_buffer->cs, S_028018_FORMAT(V_028018_Z_INVALID) | S_028018_NUM_SAMPLES(3));
      radeon_emit(cmd_buffer->cs, S_02801C_FORMAT(V_02801C_STENCIL_INVALID) | S_02801C_TILE_STENCIL_DISABLE(1));

      radeon_set_context_reg(cmd_buffer->cs, R_028B94_PA_SC_HIZ_INFO, S_028B94_SURFACE_ENABLE(0));
      radeon_set_context_reg(cmd_buffer->cs, R_028B98_PA_SC_HIS_INFO, S_028B98_SURFACE_ENABLE(0));
   } else {
      if (gfx_level == GFX9) {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028038_DB_Z_INFO, 2);
      } else {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028040_DB_Z_INFO, 2);
      }

      /* On GFX11+, the hw intentionally looks at DB_Z_INFO.NUM_SAMPLES when there is no bound
       * depth/stencil buffer and it clamps the number of samples like MIN2(DB_Z_INFO.NUM_SAMPLES,
       * PA_SC_AA_CONFIG.MSAA_EXPOSED_SAMPLES). Use 8x for DB_Z_INFO.NUM_SAMPLES to make sure it's not
       * the constraining factor. This affects VRS, occlusion queries and POPS.
       */
      radeon_emit(cmd_buffer->cs,
                  S_028040_FORMAT(V_028040_Z_INVALID) | S_028040_NUM_SAMPLES(pdev->info.gfx_level >= GFX11 ? 3 : 0));
      radeon_emit(cmd_buffer->cs, S_028044_FORMAT(V_028044_STENCIL_INVALID));
      uint32_t db_render_control = 0;

      if (gfx_level == GFX11 || gfx_level == GFX11_5)
         radv_gfx11_set_db_render_control(device, 1, &db_render_control);

      radeon_set_context_reg(cmd_buffer->cs, R_028000_DB_RENDER_CONTROL, db_render_control);
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028010_DB_RENDER_OVERRIDE2,
                          S_028010_CENTROID_COMPUTATION_MODE(gfx_level >= GFX10_3));
}
/**
 * Update the fast clear depth/stencil values if the image is bound as a
 * depth/stencil buffer.
 */
static void
radv_update_bound_fast_clear_ds(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   const struct radv_image *image = iview->image;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (cmd_buffer->state.render.ds_att.iview == NULL || cmd_buffer->state.render.ds_att.iview->image != image)
      return;

   if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      radeon_set_context_reg_seq(cs, R_028028_DB_STENCIL_CLEAR, 2);
      radeon_emit(cs, ds_clear_value.stencil);
      radeon_emit(cs, fui(ds_clear_value.depth));
   } else if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
      radeon_set_context_reg(cs, R_02802C_DB_DEPTH_CLEAR, fui(ds_clear_value.depth));
   } else {
      assert(aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
      radeon_set_context_reg(cs, R_028028_DB_STENCIL_CLEAR, ds_clear_value.stencil);
   }

   /* Update the ZRANGE_PRECISION value for the TC-compat bug. This is
    * only needed when clearing Z to 0.0.
    */
   if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && ds_clear_value.depth == 0.0) {
      radv_update_zrange_precision(cmd_buffer, &cmd_buffer->state.render.ds_att.ds, iview, false);
   }

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

/**
 * Set the clear depth/stencil values to the image's metadata.
 */
static void
radv_set_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                           const VkImageSubresourceRange *range, VkClearDepthStencilValue ds_clear_value,
                           VkImageAspectFlags aspects)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      uint64_t va = radv_get_ds_clear_value_va(image, range->baseMipLevel);

      /* Use the fastest way when both aspects are used. */
      ASSERTED unsigned cdw_end = radv_cs_write_data_head(device, cmd_buffer->cs, cmd_buffer->qf, V_370_PFP, va,
                                                          2 * level_count, cmd_buffer->state.predicating);

      for (uint32_t l = 0; l < level_count; l++) {
         radeon_emit(cs, ds_clear_value.stencil);
         radeon_emit(cs, fui(ds_clear_value.depth));
      }

      assert(cmd_buffer->cs->cdw == cdw_end);
   } else {
      /* Otherwise we need one WRITE_DATA packet per level. */
      for (uint32_t l = 0; l < level_count; l++) {
         uint64_t va = radv_get_ds_clear_value_va(image, range->baseMipLevel + l);
         unsigned value;

         if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
            value = fui(ds_clear_value.depth);
            va += 4;
         } else {
            assert(aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
            value = ds_clear_value.stencil;
         }

         radv_write_data(cmd_buffer, V_370_PFP, va, 1, &value, cmd_buffer->state.predicating);
      }
   }
}

/**
 * Update the TC-compat metadata value for this image.
 */
static void
radv_set_tc_compat_zrange_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   const VkImageSubresourceRange *range, uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (!pdev->info.has_tc_compat_zrange_bug)
      return;

   uint64_t va = radv_get_tc_compat_zrange_va(image, range->baseMipLevel);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   ASSERTED unsigned cdw_end = radv_cs_write_data_head(device, cmd_buffer->cs, cmd_buffer->qf, V_370_PFP, va,
                                                       level_count, cmd_buffer->state.predicating);

   for (uint32_t l = 0; l < level_count; l++)
      radeon_emit(cs, value);

   assert(cmd_buffer->cs->cdw == cdw_end);
}

static void
radv_update_tc_compat_zrange_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                      VkClearDepthStencilValue ds_clear_value)
{
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };
   uint32_t cond_val;

   /* Conditionally set DB_Z_INFO.ZRANGE_PRECISION to 0 when the last
    * depth clear value is 0.0f.
    */
   cond_val = ds_clear_value.depth == 0.0f ? UINT_MAX : 0;

   radv_set_tc_compat_zrange_metadata(cmd_buffer, iview->image, &range, cond_val);
}

/**
 * Update the clear depth/stencil values for this image.
 */
void
radv_update_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                              VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };
   struct radv_image *image = iview->image;

   assert(radv_htile_enabled(image, range.baseMipLevel));

   radv_set_ds_clear_metadata(cmd_buffer, iview->image, &range, ds_clear_value, aspects);

   if (radv_image_is_tc_compat_htile(image) && (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      radv_update_tc_compat_zrange_metadata(cmd_buffer, iview, ds_clear_value);
   }

   radv_update_bound_fast_clear_ds(cmd_buffer, iview, ds_clear_value, aspects);
}

/**
 * Load the clear depth/stencil values from the image's metadata.
 */
static void
radv_load_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const struct radv_image *image = iview->image;
   VkImageAspectFlags aspects = vk_format_aspects(image->vk.format);
   uint64_t va = radv_get_ds_clear_value_va(image, iview->vk.base_mip_level);
   unsigned reg_offset = 0, reg_count = 0;

   assert(radv_image_has_htile(image));

   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      ++reg_count;
   } else {
      ++reg_offset;
      va += 4;
   }
   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      ++reg_count;

   uint32_t reg = R_028028_DB_STENCIL_CLEAR + 4 * reg_offset;

   if (pdev->info.has_load_ctx_reg_pkt) {
      radeon_emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, (reg - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(cs, reg_count);
   } else {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) |
                         (reg_count == 2 ? COPY_DATA_COUNT_SEL : 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, reg >> 2);
      radeon_emit(cs, 0);

      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);
   }
}

/*
 * With DCC some colors don't require CMASK elimination before being
 * used as a texture. This sets a predicate value to determine if the
 * cmask eliminate is required.
 */
void
radv_update_fce_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!image->fce_pred_offset)
      return;

   uint64_t pred_val = value;
   uint64_t va = radv_image_get_fce_pred_va(image, range->baseMipLevel);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   ASSERTED unsigned cdw_end =
      radv_cs_write_data_head(device, cmd_buffer->cs, cmd_buffer->qf, V_370_PFP, va, 2 * level_count, false);

   for (uint32_t l = 0; l < level_count; l++) {
      radeon_emit(cmd_buffer->cs, pred_val);
      radeon_emit(cmd_buffer->cs, pred_val >> 32);
   }

   assert(cmd_buffer->cs->cdw == cdw_end);
}

/**
 * Update the DCC predicate to reflect the compression state.
 */
void
radv_update_dcc_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (image->dcc_pred_offset == 0)
      return;

   uint64_t pred_val = value;
   uint64_t va = radv_image_get_dcc_pred_va(image, range->baseMipLevel);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   assert(radv_dcc_enabled(image, range->baseMipLevel));

   ASSERTED unsigned cdw_end =
      radv_cs_write_data_head(device, cmd_buffer->cs, cmd_buffer->qf, V_370_PFP, va, 2 * level_count, false);

   for (uint32_t l = 0; l < level_count; l++) {
      radeon_emit(cmd_buffer->cs, pred_val);
      radeon_emit(cmd_buffer->cs, pred_val >> 32);
   }

   assert(cmd_buffer->cs->cdw == cdw_end);
}

/**
 * Update the fast clear color values if the image is bound as a color buffer.
 */
static void
radv_update_bound_fast_clear_color(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, int cb_idx,
                                   uint32_t color_values[2])
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (cb_idx >= cmd_buffer->state.render.color_att_count || cmd_buffer->state.render.color_att[cb_idx].iview == NULL ||
       cmd_buffer->state.render.color_att[cb_idx].iview->image != image)
      return;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 4);

   radeon_set_context_reg_seq(cs, R_028C8C_CB_COLOR0_CLEAR_WORD0 + cb_idx * 0x3c, 2);
   radeon_emit(cs, color_values[0]);
   radeon_emit(cs, color_values[1]);

   assert(cmd_buffer->cs->cdw <= cdw_max);

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

/**
 * Set the clear color values to the image's metadata.
 */
static void
radv_set_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                              const VkImageSubresourceRange *range, uint32_t color_values[2])
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   assert(radv_image_has_cmask(image) || radv_dcc_enabled(image, range->baseMipLevel));

   if (radv_image_has_clear_value(image)) {
      uint64_t va = radv_image_get_fast_clear_va(image, range->baseMipLevel);

      ASSERTED unsigned cdw_end = radv_cs_write_data_head(device, cmd_buffer->cs, cmd_buffer->qf, V_370_PFP, va,
                                                          2 * level_count, cmd_buffer->state.predicating);

      for (uint32_t l = 0; l < level_count; l++) {
         radeon_emit(cs, color_values[0]);
         radeon_emit(cs, color_values[1]);
      }

      assert(cmd_buffer->cs->cdw == cdw_end);
   } else {
      /* Some default value we can set in the update. */
      assert(color_values[0] == 0 && color_values[1] == 0);
   }
}

/**
 * Update the clear color values for this image.
 */
void
radv_update_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview, int cb_idx,
                                 uint32_t color_values[2])
{
   struct radv_image *image = iview->image;
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };

   assert(radv_image_has_cmask(image) || radv_dcc_enabled(image, iview->vk.base_mip_level));

   /* Do not need to update the clear value for images that are fast cleared with the comp-to-single
    * mode because the hardware gets the value from the image directly.
    */
   if (iview->image->support_comp_to_single)
      return;

   radv_set_color_clear_metadata(cmd_buffer, image, &range, color_values);

   radv_update_bound_fast_clear_color(cmd_buffer, image, cb_idx, color_values);
}

/**
 * Load the clear color values from the image's metadata.
 */
static void
radv_load_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *iview, int cb_idx)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_image *image = iview->image;

   if (!radv_image_has_cmask(image) && !radv_dcc_enabled(image, iview->vk.base_mip_level))
      return;

   if (iview->image->support_comp_to_single)
      return;

   if (!radv_image_has_clear_value(image)) {
      uint32_t color_values[2] = {0, 0};
      radv_update_bound_fast_clear_color(cmd_buffer, image, cb_idx, color_values);
      return;
   }

   uint64_t va = radv_image_get_fast_clear_va(image, iview->vk.base_mip_level);
   uint32_t reg = R_028C8C_CB_COLOR0_CLEAR_WORD0 + cb_idx * 0x3c;

   if (pdev->info.has_load_ctx_reg_pkt) {
      radeon_emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, cmd_buffer->state.predicating));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, (reg - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(cs, 2);
   } else {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, cmd_buffer->state.predicating));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) | COPY_DATA_COUNT_SEL);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, reg >> 2);
      radeon_emit(cs, 0);

      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, cmd_buffer->state.predicating));
      radeon_emit(cs, 0);
   }
}

/* GFX9+ metadata cache flushing workaround. metadata cache coherency is
 * broken if the CB caches data of multiple mips of the same image at the
 * same time.
 *
 * Insert some flushes to avoid this.
 */
static void
radv_emit_fb_mip_change_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   bool color_mip_changed = false;

   /* Entire workaround is not applicable before GFX9 */
   if (pdev->info.gfx_level < GFX9)
      return;

   for (int i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      if ((radv_image_has_cmask(iview->image) || radv_dcc_enabled(iview->image, iview->vk.base_mip_level) ||
           radv_dcc_enabled(iview->image, cmd_buffer->state.cb_mip[i])) &&
          cmd_buffer->state.cb_mip[i] != iview->vk.base_mip_level)
         color_mip_changed = true;

      cmd_buffer->state.cb_mip[i] = iview->vk.base_mip_level;
   }

   if (color_mip_changed) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   const struct radv_image_view *iview = render->ds_att.iview;
   if (iview) {
      if ((radv_htile_enabled(iview->image, iview->vk.base_mip_level) ||
           radv_htile_enabled(iview->image, cmd_buffer->state.ds_mip)) &&
          cmd_buffer->state.ds_mip != iview->vk.base_mip_level) {
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
      }

      cmd_buffer->state.ds_mip = iview->vk.base_mip_level;
   }
}

/* This function does the flushes for mip changes if the levels are not zero for
 * all render targets. This way we can assume at the start of the next cmd_buffer
 * that rendering to mip 0 doesn't need any flushes. As that is the most common
 * case that saves some flushes. */
static void
radv_emit_mip_change_flush_default(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   /* Entire workaround is not applicable before GFX9 */
   if (pdev->info.gfx_level < GFX9)
      return;

   bool need_color_mip_flush = false;
   for (unsigned i = 0; i < 8; ++i) {
      if (cmd_buffer->state.cb_mip[i]) {
         need_color_mip_flush = true;
         break;
      }
   }

   if (need_color_mip_flush) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   if (cmd_buffer->state.ds_mip) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   memset(cmd_buffer->state.cb_mip, 0, sizeof(cmd_buffer->state.cb_mip));
   cmd_buffer->state.ds_mip = 0;
}

static void
radv_emit_framebuffer_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   int i;
   bool disable_constant_encode_ac01 = false;
   unsigned color_invalid = pdev->info.gfx_level >= GFX12   ? S_028EC0_FORMAT(V_028EC0_COLOR_INVALID)
                            : pdev->info.gfx_level >= GFX11 ? S_028C70_FORMAT_GFX11(V_028C70_COLOR_INVALID)
                                                            : S_028C70_FORMAT_GFX6(V_028C70_COLOR_INVALID);
   VkExtent2D extent = {MAX_FRAMEBUFFER_WIDTH, MAX_FRAMEBUFFER_HEIGHT};

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 51 + MAX_RTS * 70);

   for (i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview) {
         if (pdev->info.gfx_level >= GFX12) {
            radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_INFO + i * 4, color_invalid);
         } else {
            radeon_set_context_reg(cmd_buffer->cs, R_028C70_CB_COLOR0_INFO + i * 0x3C, color_invalid);
         }
         continue;
      }

      VkImageLayout layout = render->color_att[i].layout;

      radv_cs_add_buffer(device->ws, cmd_buffer->cs, iview->image->bindings[0].bo);

      assert(iview->vk.aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
                                  VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT));

      if (iview->image->disjoint && iview->vk.aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
         for (uint32_t plane_id = 0; plane_id < iview->image->plane_count; plane_id++) {
            radv_cs_add_buffer(device->ws, cmd_buffer->cs, iview->image->bindings[plane_id].bo);
         }
      } else {
         uint32_t plane_id = iview->image->disjoint ? iview->plane_id : 0;
         radv_cs_add_buffer(device->ws, cmd_buffer->cs, iview->image->bindings[plane_id].bo);
      }

      radv_emit_fb_color_state(cmd_buffer, i, &render->color_att[i].cb, iview, layout);

      radv_load_color_clear_metadata(cmd_buffer, iview, i);

      if (pdev->info.gfx_level >= GFX9 && iview->image->dcc_sign_reinterpret) {
         /* Disable constant encoding with the clear value of "1" with different DCC signedness
          * because the hardware will fill "1" instead of the clear value.
          */
         disable_constant_encode_ac01 = true;
      }

      extent.width = MIN2(extent.width, iview->vk.extent.width);
      extent.height = MIN2(extent.height, iview->vk.extent.height);
   }
   for (; i < cmd_buffer->state.last_subpass_color_count; i++) {
      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_INFO + i * 4, color_invalid);
      } else {
         radeon_set_context_reg(cmd_buffer->cs, R_028C70_CB_COLOR0_INFO + i * 0x3C, color_invalid);
      }
   }
   cmd_buffer->state.last_subpass_color_count = render->color_att_count;

   if (render->ds_att.iview) {
      struct radv_image_view *iview = render->ds_att.iview;
      const struct radv_image *image = iview->image;
      radv_cs_add_buffer(device->ws, cmd_buffer->cs, image->bindings[0].bo);

      uint32_t qf_mask = radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf);
      bool depth_compressed = radv_layout_is_htile_compressed(device, image, render->ds_att.layout, qf_mask);
      bool stencil_compressed = radv_layout_is_htile_compressed(device, image, render->ds_att.stencil_layout, qf_mask);

      radv_emit_fb_ds_state(cmd_buffer, &render->ds_att.ds, iview, depth_compressed, stencil_compressed);

      if (depth_compressed || stencil_compressed) {
         /* Only load the depth/stencil fast clear values when
          * compressed rendering is enabled.
          */
         radv_load_ds_clear_metadata(cmd_buffer, iview);
      }

      extent.width = MIN2(extent.width, iview->vk.extent.width);
      extent.height = MIN2(extent.height, iview->vk.extent.height);
   } else if (pdev->info.gfx_level == GFX10_3 && render->vrs_att.iview && radv_cmd_buffer_get_vrs_image(cmd_buffer)) {
      /* When a subpass uses a VRS attachment without binding a depth/stencil attachment, we have to
       * bind our internal depth buffer that contains the VRS data as part of HTILE.
       */
      VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      struct radv_buffer *htile_buffer = device->vrs.buffer;
      struct radv_image *image = device->vrs.image;
      struct radv_ds_buffer_info ds;
      struct radv_image_view iview;

      radv_image_view_init(&iview, device,
                           &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = radv_image_to_handle(image),
                              .viewType = radv_meta_get_view_type(image),
                              .format = image->vk.format,
                              .subresourceRange =
                                 {
                                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                    .baseMipLevel = 0,
                                    .levelCount = 1,
                                    .baseArrayLayer = 0,
                                    .layerCount = 1,
                                 },
                           },
                           0, NULL);

      radv_initialise_vrs_surface(image, htile_buffer, &ds);

      radv_cs_add_buffer(device->ws, cmd_buffer->cs, htile_buffer->bo);

      bool depth_compressed = radv_layout_is_htile_compressed(
         device, image, layout, radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf));
      radv_emit_fb_ds_state(cmd_buffer, &ds, &iview, depth_compressed, false);

      radv_image_view_finish(&iview);
   } else {
      radv_emit_null_ds_state(cmd_buffer);
   }

   if (pdev->info.gfx_level >= GFX11) {
      bool vrs_surface_enable = render->vrs_att.iview != NULL;
      unsigned xmax = 0, ymax = 0;
      uint64_t va = 0;

      if (vrs_surface_enable) {
         const struct radv_image_view *vrs_iview = render->vrs_att.iview;
         struct radv_image *vrs_image = vrs_iview->image;

         va = radv_image_get_va(vrs_image, 0);
         va |= vrs_image->planes[0].surface.tile_swizzle << 8;

         xmax = vrs_iview->vk.extent.width - 1;
         ymax = vrs_iview->vk.extent.height - 1;
      }

      radeon_set_context_reg_seq(cmd_buffer->cs, R_0283F0_PA_SC_VRS_RATE_BASE, 3);
      radeon_emit(cmd_buffer->cs, va >> 8);
      radeon_emit(cmd_buffer->cs, S_0283F4_BASE_256B(va >> 40));
      radeon_emit(cmd_buffer->cs, S_0283F8_X_MAX(xmax) | S_0283F8_Y_MAX(ymax));

      radeon_set_context_reg(cmd_buffer->cs, R_0283D0_PA_SC_VRS_OVERRIDE_CNTL,
                             S_0283D0_VRS_SURFACE_ENABLE(vrs_surface_enable));
   }

   if (pdev->info.gfx_level >= GFX8 && pdev->info.gfx_level < GFX12) {
      bool disable_constant_encode = pdev->info.has_dcc_constant_encode;
      enum amd_gfx_level gfx_level = pdev->info.gfx_level;

      if (pdev->info.gfx_level >= GFX11) {
         const bool has_dedicated_vram = pdev->info.has_dedicated_vram;

         radeon_set_context_reg(cmd_buffer->cs, R_028424_CB_FDCC_CONTROL,
                                S_028424_SAMPLE_MASK_TRACKER_WATERMARK(has_dedicated_vram ? 0 : 15));
      } else {
         uint8_t watermark = gfx_level >= GFX10 ? 6 : 4;

         radeon_set_context_reg(cmd_buffer->cs, R_028424_CB_DCC_CONTROL,
                                S_028424_OVERWRITE_COMBINER_MRT_SHARING_DISABLE(gfx_level <= GFX9) |
                                   S_028424_OVERWRITE_COMBINER_WATERMARK(watermark) |
                                   S_028424_DISABLE_CONSTANT_ENCODE_AC01(disable_constant_encode_ac01) |
                                   S_028424_DISABLE_CONSTANT_ENCODE_REG(disable_constant_encode));
      }
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028184_PA_SC_SCREEN_SCISSOR_BR,
                             S_028034_BR_X(extent.width) | S_028034_BR_Y(extent.height));
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028034_PA_SC_SCREEN_SCISSOR_BR,
                             S_028034_BR_X(extent.width) | S_028034_BR_Y(extent.height));
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FRAMEBUFFER;
}

static void
radv_emit_guardband_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned rast_prim = radv_get_rasterization_prim(cmd_buffer);
   const bool draw_points = radv_rast_prim_is_point(rast_prim) || radv_polygon_mode_is_point(d->vk.rs.polygon_mode);
   const bool draw_lines = radv_rast_prim_is_line(rast_prim) || radv_polygon_mode_is_line(d->vk.rs.polygon_mode);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   int i;
   float scale[3], translate[3], guardband_x = INFINITY, guardband_y = INFINITY;
   float discard_x = 1.0f, discard_y = 1.0f;
   const float max_range = 32767.0f;

   if (!d->vk.vp.viewport_count)
      return;

   for (i = 0; i < d->vk.vp.viewport_count; i++) {
      radv_get_viewport_xform(d->vk.vp.viewports + i, scale, translate);
      scale[0] = fabsf(scale[0]);
      scale[1] = fabsf(scale[1]);

      if (scale[0] < 0.5)
         scale[0] = 0.5;
      if (scale[1] < 0.5)
         scale[1] = 0.5;

      guardband_x = MIN2(guardband_x, (max_range - fabsf(translate[0])) / scale[0]);
      guardband_y = MIN2(guardband_y, (max_range - fabsf(translate[1])) / scale[1]);

      if (draw_points || draw_lines) {
         /* When rendering wide points or lines, we need to be more conservative about when to
          * discard them entirely. */
         float pixels;

         if (draw_points) {
            pixels = 8191.875f;
         } else {
            pixels = d->vk.rs.line.width;
         }

         /* Add half the point size / line width. */
         discard_x += pixels / (2.0 * scale[0]);
         discard_y += pixels / (2.0 * scale[1]);

         /* Discard primitives that would lie entirely outside the clip region. */
         discard_x = MIN2(discard_x, guardband_x);
         discard_y = MIN2(discard_y, guardband_y);
      }
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(cs, R_02842C_PA_CL_GB_VERT_CLIP_ADJ, 4);
   } else {
      radeon_set_context_reg_seq(cs, R_028BE8_PA_CL_GB_VERT_CLIP_ADJ, 4);
   }
   radeon_emit(cs, fui(guardband_y));
   radeon_emit(cs, fui(discard_y));
   radeon_emit(cs, fui(guardband_x));
   radeon_emit(cs, fui(discard_x));

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GUARDBAND;
}

/* Bind an internal index buffer for GPUs that hang with 0-sized index buffers to handle robustness2
 * which requires 0 for out-of-bounds access.
 */
static void
radv_handle_zero_index_buffer_bug(struct radv_cmd_buffer *cmd_buffer, uint64_t *index_va, uint32_t *remaining_indexes)
{
   const uint32_t zero = 0;
   uint32_t offset;

   if (!radv_cmd_buffer_upload_data(cmd_buffer, sizeof(uint32_t), &zero, &offset)) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   *index_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;
   *remaining_indexes = 1;
}

static void
radv_emit_index_buffer(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t max_index_count = state->max_index_count;
   uint64_t index_va = state->index_va;

   /* With indirect generated commands the index buffer bind may be part of the
    * indirect command buffer, in which case the app may not have bound any yet. */
   if (state->index_type < 0)
      return;

   /* Handle indirect draw calls with NULL index buffer if the GPU doesn't support them. */
   if (!max_index_count && pdev->info.has_zero_index_buffer_bug) {
      radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &max_index_count);
   }

   radeon_emit(cs, PKT3(PKT3_INDEX_BASE, 1, 0));
   radeon_emit(cs, index_va);
   radeon_emit(cs, index_va >> 32);

   radeon_emit(cs, PKT3(PKT3_INDEX_BUFFER_SIZE, 0, 0));
   radeon_emit(cs, max_index_count);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_INDEX_BUFFER;
}

static void
radv_flush_occlusion_query_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const bool enable_occlusion_queries =
      cmd_buffer->state.active_occlusion_queries || cmd_buffer->state.inherited_occlusion_queries;
   uint32_t db_count_control;

   if (!enable_occlusion_queries) {
      db_count_control = S_028004_ZPASS_INCREMENT_DISABLE(gfx_level < GFX11);
   } else {
      bool gfx10_perfect =
         gfx_level >= GFX10 && (cmd_buffer->state.perfect_occlusion_queries_enabled ||
                                cmd_buffer->state.inherited_query_control_flags & VK_QUERY_CONTROL_PRECISE_BIT);

      if (gfx_level >= GFX7) {
         /* Always enable PERFECT_ZPASS_COUNTS due to issues with partially
          * covered tiles, discards, and early depth testing. For more details,
          * see https://gitlab.freedesktop.org/mesa/mesa/-/issues/3218 */
         db_count_control = S_028004_PERFECT_ZPASS_COUNTS(1) |
                            S_028004_DISABLE_CONSERVATIVE_ZPASS_COUNTS(gfx10_perfect) | S_028004_ZPASS_ENABLE(1) |
                            S_028004_SLICE_EVEN_ENABLE(1) | S_028004_SLICE_ODD_ENABLE(1);
      } else {
         db_count_control = S_028004_PERFECT_ZPASS_COUNTS(1);
      }

      if (gfx_level < GFX12) {
         const uint32_t rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
         const uint32_t sample_rate = util_logbase2(rasterization_samples);

         db_count_control |= S_028004_SAMPLE_RATE(sample_rate);
      }
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(cmd_buffer, R_028060_DB_COUNT_CONTROL, RADV_TRACKED_DB_COUNT_CONTROL,
                                 db_count_control);
   } else {
      radeon_opt_set_context_reg(cmd_buffer, R_028004_DB_COUNT_CONTROL, RADV_TRACKED_DB_COUNT_CONTROL,
                                 db_count_control);
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_OCCLUSION_QUERY;
}

unsigned
radv_instance_rate_prolog_index(unsigned num_attributes, uint32_t instance_rate_inputs)
{
   /* instance_rate_vs_prologs is a flattened array of array of arrays of different sizes, or a
    * single array sorted in ascending order using:
    * - total number of attributes
    * - number of instanced attributes
    * - index of first instanced attribute
    */

   /* From total number of attributes to offset. */
   static const uint16_t total_to_offset[16] = {0, 1, 4, 10, 20, 35, 56, 84, 120, 165, 220, 286, 364, 455, 560, 680};
   unsigned start_index = total_to_offset[num_attributes - 1];

   /* From number of instanced attributes to offset. This would require a different LUT depending on
    * the total number of attributes, but we can exploit a pattern to use just the LUT for 16 total
    * attributes.
    */
   static const uint8_t count_to_offset_total16[16] = {0,   16,  31,  45,  58,  70,  81,  91,
                                                       100, 108, 115, 121, 126, 130, 133, 135};
   unsigned count = util_bitcount(instance_rate_inputs);
   unsigned offset_from_start_index = count_to_offset_total16[count - 1] - ((16 - num_attributes) * (count - 1));

   unsigned first = ffs(instance_rate_inputs) - 1;
   return start_index + offset_from_start_index + first;
}

static struct radv_shader_part *
lookup_vs_prolog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader, uint32_t *nontrivial_divisors)
{
   assert(vs_shader->info.vs.dynamic_inputs);

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_vs_input_state *state = &cmd_buffer->state.dynamic_vs_input;

   unsigned num_attributes = util_last_bit(vs_shader->info.vs.vb_desc_usage_mask);
   uint32_t attribute_mask = BITFIELD_MASK(num_attributes);

   uint32_t instance_rate_inputs = state->instance_rate_inputs & attribute_mask;
   uint32_t zero_divisors = state->zero_divisors & attribute_mask;
   *nontrivial_divisors = state->nontrivial_divisors & attribute_mask;
   uint32_t misaligned_mask = cmd_buffer->state.vbo_misaligned_mask;
   uint32_t unaligned_mask = cmd_buffer->state.vbo_unaligned_mask;
   if (cmd_buffer->state.vbo_misaligned_mask_invalid) {
      bool misalignment_possible = pdev->info.gfx_level == GFX6 || pdev->info.gfx_level >= GFX10;
      u_foreach_bit (index, cmd_buffer->state.vbo_misaligned_mask_invalid & attribute_mask) {
         uint8_t binding = state->bindings[index];
         if (!(cmd_buffer->state.vbo_bound_mask & BITFIELD_BIT(binding)))
            continue;

         uint8_t format_req = state->format_align_req_minus_1[index];
         uint8_t component_req = state->component_align_req_minus_1[index];
         uint64_t vb_offset = cmd_buffer->vertex_bindings[binding].offset;
         uint64_t vb_stride;

         if (cmd_buffer->state.uses_dynamic_vertex_binding_stride) {
            vb_stride = cmd_buffer->vertex_bindings[binding].stride;
         } else {
            vb_stride = cmd_buffer->state.graphics_pipeline->binding_stride[binding];
         }

         VkDeviceSize offset = vb_offset + state->offsets[index];

         if (misalignment_possible && ((offset | vb_stride) & format_req))
            misaligned_mask |= BITFIELD_BIT(index);
         if ((offset | vb_stride) & component_req)
            unaligned_mask |= BITFIELD_BIT(index);
      }
      cmd_buffer->state.vbo_misaligned_mask = misaligned_mask;
      cmd_buffer->state.vbo_unaligned_mask = unaligned_mask;
      cmd_buffer->state.vbo_misaligned_mask_invalid &= ~attribute_mask;
   }
   misaligned_mask |= state->nontrivial_formats | unaligned_mask;
   misaligned_mask &= attribute_mask;
   unaligned_mask &= attribute_mask;

   const bool can_use_simple_input =
      cmd_buffer->state.shaders[MESA_SHADER_VERTEX] &&
      !cmd_buffer->state.shaders[MESA_SHADER_VERTEX]->info.merged_shader_compiled_separately &&
      cmd_buffer->state.shaders[MESA_SHADER_VERTEX]->info.is_ngg == pdev->use_ngg &&
      cmd_buffer->state.shaders[MESA_SHADER_VERTEX]->info.wave_size == pdev->ge_wave_size;

   /* The instance ID input VGPR is placed differently when as_ls=true. as_ls is also needed to
    * workaround the LS VGPR initialization bug.
    */
   bool as_ls = vs_shader->info.vs.as_ls && (instance_rate_inputs || pdev->info.has_ls_vgpr_init_bug);

   /* try to use a pre-compiled prolog first */
   struct radv_shader_part *prolog = NULL;
   if (can_use_simple_input && !as_ls && !misaligned_mask && !state->alpha_adjust_lo && !state->alpha_adjust_hi) {
      if (!instance_rate_inputs) {
         prolog = device->simple_vs_prologs[num_attributes - 1];
      } else if (num_attributes <= 16 && !*nontrivial_divisors && !zero_divisors &&
                 util_bitcount(instance_rate_inputs) ==
                    (util_last_bit(instance_rate_inputs) - ffs(instance_rate_inputs) + 1)) {
         unsigned index = radv_instance_rate_prolog_index(num_attributes, instance_rate_inputs);
         prolog = device->instance_rate_vs_prologs[index];
      }
   }
   if (prolog)
      return prolog;

   struct radv_vs_prolog_key key;
   memset(&key, 0, sizeof(key));
   key.instance_rate_inputs = instance_rate_inputs;
   key.nontrivial_divisors = *nontrivial_divisors;
   key.zero_divisors = zero_divisors;
   /* If the attribute is aligned, post shuffle is implemented using DST_SEL instead. */
   key.post_shuffle = state->post_shuffle & misaligned_mask;
   key.alpha_adjust_hi = state->alpha_adjust_hi & attribute_mask & ~unaligned_mask;
   key.alpha_adjust_lo = state->alpha_adjust_lo & attribute_mask & ~unaligned_mask;
   u_foreach_bit (index, misaligned_mask)
      key.formats[index] = state->formats[index];
   key.num_attributes = num_attributes;
   key.misaligned_mask = misaligned_mask;
   key.unaligned_mask = unaligned_mask;
   key.as_ls = as_ls;
   key.is_ngg = vs_shader->info.is_ngg;
   key.wave32 = vs_shader->info.wave_size == 32;

   if (vs_shader->info.merged_shader_compiled_separately) {
      assert(vs_shader->info.next_stage == MESA_SHADER_TESS_CTRL || vs_shader->info.next_stage == MESA_SHADER_GEOMETRY);
      key.next_stage = vs_shader->info.next_stage;
   } else {
      key.next_stage = vs_shader->info.stage;
   }

   return radv_shader_part_cache_get(device, &device->vs_prologs, &cmd_buffer->vs_prologs, &key);
}

static void
emit_prolog_regs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                 const struct radv_shader_part *prolog)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t rsrc1, rsrc2;

   /* no need to re-emit anything in this case */
   if (cmd_buffer->state.emitted_vs_prolog == prolog)
      return;

   enum amd_gfx_level chip = pdev->info.gfx_level;

   assert(cmd_buffer->state.emitted_graphics_pipeline == cmd_buffer->state.graphics_pipeline);

   if (vs_shader->info.merged_shader_compiled_separately) {
      if (vs_shader->info.next_stage == MESA_SHADER_GEOMETRY) {
         radv_shader_combine_cfg_vs_gs(vs_shader, cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY], &rsrc1, &rsrc2);
      } else {
         assert(vs_shader->info.next_stage == MESA_SHADER_TESS_CTRL);

         radv_shader_combine_cfg_vs_tcs(vs_shader, cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL], &rsrc1, &rsrc2);
      }
   } else {
      rsrc1 = vs_shader->config.rsrc1;
   }

   if (chip < GFX10 && G_00B228_SGPRS(prolog->rsrc1) > G_00B228_SGPRS(rsrc1))
      rsrc1 = (rsrc1 & C_00B228_SGPRS) | (prolog->rsrc1 & ~C_00B228_SGPRS);

   if (G_00B848_VGPRS(prolog->rsrc1) > G_00B848_VGPRS(rsrc1))
      rsrc1 = (rsrc1 & C_00B848_VGPRS) | (prolog->rsrc1 & ~C_00B848_VGPRS);

   unsigned pgm_lo_reg = R_00B120_SPI_SHADER_PGM_LO_VS;
   unsigned rsrc1_reg = R_00B128_SPI_SHADER_PGM_RSRC1_VS;
   if (vs_shader->info.is_ngg || cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY] == vs_shader ||
       (vs_shader->info.merged_shader_compiled_separately && vs_shader->info.next_stage == MESA_SHADER_GEOMETRY)) {
      pgm_lo_reg = chip >= GFX12   ? R_00B224_SPI_SHADER_PGM_LO_ES
                   : chip >= GFX10 ? R_00B320_SPI_SHADER_PGM_LO_ES
                                   : R_00B210_SPI_SHADER_PGM_LO_ES;
      rsrc1_reg = R_00B228_SPI_SHADER_PGM_RSRC1_GS;
   } else if (cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL] == vs_shader ||
              (vs_shader->info.merged_shader_compiled_separately &&
               vs_shader->info.next_stage == MESA_SHADER_TESS_CTRL)) {
      pgm_lo_reg = chip >= GFX12   ? R_00B424_SPI_SHADER_PGM_LO_LS
                   : chip >= GFX10 ? R_00B520_SPI_SHADER_PGM_LO_LS
                                   : R_00B410_SPI_SHADER_PGM_LO_LS;
      rsrc1_reg = R_00B428_SPI_SHADER_PGM_RSRC1_HS;
   } else if (vs_shader->info.vs.as_ls) {
      pgm_lo_reg = R_00B520_SPI_SHADER_PGM_LO_LS;
      rsrc1_reg = R_00B528_SPI_SHADER_PGM_RSRC1_LS;
   } else if (vs_shader->info.vs.as_es) {
      pgm_lo_reg = R_00B320_SPI_SHADER_PGM_LO_ES;
      rsrc1_reg = R_00B328_SPI_SHADER_PGM_RSRC1_ES;
   }

   radeon_set_sh_reg(cmd_buffer->cs, pgm_lo_reg, prolog->va >> 8);

   radeon_set_sh_reg(cmd_buffer->cs, rsrc1_reg, rsrc1);

   if (vs_shader->info.merged_shader_compiled_separately) {
      if (vs_shader->info.next_stage == MESA_SHADER_GEOMETRY) {
         const struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];
         unsigned lds_size;

         if (gs->info.is_ngg) {
            lds_size = DIV_ROUND_UP(gs->info.ngg_info.lds_size, pdev->info.lds_encode_granularity);
         } else {
            lds_size = gs->info.gs_ring_info.lds_size;
         }

         radeon_set_sh_reg(cmd_buffer->cs, rsrc1_reg + 4, rsrc2 | S_00B22C_LDS_SIZE(lds_size));
      } else {
         radeon_set_sh_reg(cmd_buffer->cs, rsrc1_reg + 4, rsrc2);
      }
   }

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, prolog->bo);
}

static void
emit_prolog_inputs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                   uint32_t nontrivial_divisors)
{
   /* no need to re-emit anything in this case */
   if (!nontrivial_divisors && cmd_buffer->state.emitted_vs_prolog &&
       !cmd_buffer->state.emitted_vs_prolog->nontrivial_divisors)
      return;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_vs_input_state *state = &cmd_buffer->state.dynamic_vs_input;
   uint64_t input_va = radv_shader_get_va(vs_shader);

   if (nontrivial_divisors) {
      unsigned inputs_offset;
      uint32_t *inputs;
      unsigned size = 8 + util_bitcount(nontrivial_divisors) * 8;
      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &inputs_offset, (void **)&inputs))
         return;

      *(inputs++) = input_va;
      *(inputs++) = input_va >> 32;

      u_foreach_bit (index, nontrivial_divisors) {
         uint32_t div = state->divisors[index];
         if (div == 0) {
            *(inputs++) = 0;
            *(inputs++) = 1;
         } else if (util_is_power_of_two_or_zero(div)) {
            *(inputs++) = util_logbase2(div) | (1 << 8);
            *(inputs++) = 0xffffffffu;
         } else {
            struct util_fast_udiv_info info = util_compute_fast_udiv_info(div, 32, 32);
            *(inputs++) = info.pre_shift | (info.increment << 8) | (info.post_shift << 16);
            *(inputs++) = info.multiplier;
         }
      }

      input_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + inputs_offset;
   }

   const struct radv_userdata_info *loc = &vs_shader->info.user_sgprs_locs.shader_data[AC_UD_VS_PROLOG_INPUTS];
   uint32_t base_reg = vs_shader->info.user_data_0;
   assert(loc->sgpr_idx != -1);
   assert(loc->num_sgprs == 2);
   radv_emit_shader_pointer(device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, input_va, true);
}

static void
radv_emit_vertex_input(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *vs_shader = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(!cmd_buffer->state.mesh_shading);

   if (!vs_shader->info.vs.has_prolog)
      return;

   uint32_t nontrivial_divisors;
   struct radv_shader_part *prolog = lookup_vs_prolog(cmd_buffer, vs_shader, &nontrivial_divisors);
   if (!prolog) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   emit_prolog_regs(cmd_buffer, vs_shader, prolog);
   emit_prolog_inputs(cmd_buffer, vs_shader, nontrivial_divisors);

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, prolog->upload_seq);

   cmd_buffer->state.emitted_vs_prolog = prolog;

   if (radv_device_fault_detection_enabled(device))
      radv_save_vs_prolog(cmd_buffer, prolog);
}

static void
radv_emit_tess_domain_origin(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned type = 0, partitioning = 0;
   unsigned topology;

   switch (tes->info.tes._primitive_mode) {
   case TESS_PRIMITIVE_TRIANGLES:
      type = V_028B6C_TESS_TRIANGLE;
      break;
   case TESS_PRIMITIVE_QUADS:
      type = V_028B6C_TESS_QUAD;
      break;
   case TESS_PRIMITIVE_ISOLINES:
      type = V_028B6C_TESS_ISOLINE;
      break;
   default:
      unreachable("Invalid tess primitive type");
   }

   switch (tes->info.tes.spacing) {
   case TESS_SPACING_EQUAL:
      partitioning = V_028B6C_PART_INTEGER;
      break;
   case TESS_SPACING_FRACTIONAL_ODD:
      partitioning = V_028B6C_PART_FRAC_ODD;
      break;
   case TESS_SPACING_FRACTIONAL_EVEN:
      partitioning = V_028B6C_PART_FRAC_EVEN;
      break;
   default:
      unreachable("Invalid tess spacing type");
   }

   if (tes->info.tes.point_mode) {
      topology = V_028B6C_OUTPUT_POINT;
   } else if (tes->info.tes._primitive_mode == TESS_PRIMITIVE_ISOLINES) {
      topology = V_028B6C_OUTPUT_LINE;
   } else {
      bool ccw = tes->info.tes.ccw;

      if (d->vk.ts.domain_origin != VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT) {
         ccw = !ccw;
      }

      topology = ccw ? V_028B6C_OUTPUT_TRIANGLE_CCW : V_028B6C_OUTPUT_TRIANGLE_CW;
   }

   uint32_t vgt_tf_param = S_028B6C_TYPE(type) | S_028B6C_PARTITIONING(partitioning) | S_028B6C_TOPOLOGY(topology) |
                           S_028B6C_DISTRIBUTION_MODE(pdev->tess_distribution_mode);

   if (pdev->info.gfx_level >= GFX12) {
      vgt_tf_param |= S_028AA4_TEMPORAL(gfx12_load_last_use_discard);

      radeon_set_context_reg(cmd_buffer->cs, R_028AA4_VGT_TF_PARAM, vgt_tf_param);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028B6C_VGT_TF_PARAM, vgt_tf_param);
   }
}

static void
radv_emit_alpha_to_coverage_enable(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned db_alpha_to_mask = 0;

   if (instance->debug_flags & RADV_DEBUG_NO_ATOC_DITHERING) {
      db_alpha_to_mask = S_028B70_ALPHA_TO_MASK_OFFSET0(2) | S_028B70_ALPHA_TO_MASK_OFFSET1(2) |
                         S_028B70_ALPHA_TO_MASK_OFFSET2(2) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
                         S_028B70_OFFSET_ROUND(0);
   } else {
      db_alpha_to_mask = S_028B70_ALPHA_TO_MASK_OFFSET0(3) | S_028B70_ALPHA_TO_MASK_OFFSET1(1) |
                         S_028B70_ALPHA_TO_MASK_OFFSET2(0) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
                         S_028B70_OFFSET_ROUND(1);
   }

   db_alpha_to_mask |= S_028B70_ALPHA_TO_MASK_ENABLE(d->vk.ms.alpha_to_coverage_enable);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_02807C_DB_ALPHA_TO_MASK, db_alpha_to_mask);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028B70_DB_ALPHA_TO_MASK, db_alpha_to_mask);
   }
}

static void
radv_emit_sample_mask(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0, 2);
   radeon_emit(cmd_buffer->cs, d->vk.ms.sample_mask | ((uint32_t)d->vk.ms.sample_mask << 16));
   radeon_emit(cmd_buffer->cs, d->vk.ms.sample_mask | ((uint32_t)d->vk.ms.sample_mask << 16));
}

static void
radv_emit_color_blend(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned cb_blend_control[MAX_RTS], sx_mrt_blend_opt[MAX_RTS];
   bool mrt0_is_dual_src = radv_is_mrt0_dual_src(cmd_buffer);

   for (unsigned i = 0; i < MAX_RTS; i++) {
      VkBlendOp eqRGB = d->vk.cb.attachments[i].color_blend_op;
      VkBlendFactor srcRGB = d->vk.cb.attachments[i].src_color_blend_factor;
      VkBlendFactor dstRGB = d->vk.cb.attachments[i].dst_color_blend_factor;
      VkBlendOp eqA = d->vk.cb.attachments[i].alpha_blend_op;
      VkBlendFactor srcA = d->vk.cb.attachments[i].src_alpha_blend_factor;
      VkBlendFactor dstA = d->vk.cb.attachments[i].dst_alpha_blend_factor;
      unsigned srcRGB_opt, dstRGB_opt, srcA_opt, dstA_opt;
      unsigned blend_cntl = 0;

      cb_blend_control[i] = sx_mrt_blend_opt[i] = 0;

      /* Ignore other blend targets if dual-source blending is enabled to prevent wrong behaviour.
       */
      if (i > 0 && mrt0_is_dual_src)
         continue;

      if (!d->vk.cb.attachments[i].blend_enable) {
         sx_mrt_blend_opt[i] |= S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) |
                                S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED);
         continue;
      }

      radv_normalize_blend_factor(eqRGB, &srcRGB, &dstRGB);
      radv_normalize_blend_factor(eqA, &srcA, &dstA);

      /* Blending optimizations for RB+.
       * These transformations don't change the behavior.
       *
       * First, get rid of DST in the blend factors:
       *    func(src * DST, dst * 0) ---> func(src * 0, dst * SRC)
       */
      radv_blend_remove_dst(&eqRGB, &srcRGB, &dstRGB, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR);

      radv_blend_remove_dst(&eqA, &srcA, &dstA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR);

      radv_blend_remove_dst(&eqA, &srcA, &dstA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA);

      /* Look up the ideal settings from tables. */
      srcRGB_opt = radv_translate_blend_opt_factor(srcRGB, false);
      dstRGB_opt = radv_translate_blend_opt_factor(dstRGB, false);
      srcA_opt = radv_translate_blend_opt_factor(srcA, true);
      dstA_opt = radv_translate_blend_opt_factor(dstA, true);

      /* Handle interdependencies. */
      if (radv_blend_factor_uses_dst(srcRGB))
         dstRGB_opt = V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_NONE;
      if (radv_blend_factor_uses_dst(srcA))
         dstA_opt = V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_NONE;

      if (srcRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE &&
          (dstRGB == VK_BLEND_FACTOR_ZERO || dstRGB == VK_BLEND_FACTOR_SRC_ALPHA ||
           dstRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE))
         dstRGB_opt = V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_A0;

      /* Set the final value. */
      sx_mrt_blend_opt[i] = S_028760_COLOR_SRC_OPT(srcRGB_opt) | S_028760_COLOR_DST_OPT(dstRGB_opt) |
                            S_028760_COLOR_COMB_FCN(radv_translate_blend_opt_function(eqRGB)) |
                            S_028760_ALPHA_SRC_OPT(srcA_opt) | S_028760_ALPHA_DST_OPT(dstA_opt) |
                            S_028760_ALPHA_COMB_FCN(radv_translate_blend_opt_function(eqA));

      blend_cntl |= S_028780_ENABLE(1);
      blend_cntl |= S_028780_COLOR_COMB_FCN(radv_translate_blend_function(eqRGB));
      blend_cntl |= S_028780_COLOR_SRCBLEND(radv_translate_blend_factor(gfx_level, srcRGB));
      blend_cntl |= S_028780_COLOR_DESTBLEND(radv_translate_blend_factor(gfx_level, dstRGB));
      if (srcA != srcRGB || dstA != dstRGB || eqA != eqRGB) {
         blend_cntl |= S_028780_SEPARATE_ALPHA_BLEND(1);
         blend_cntl |= S_028780_ALPHA_COMB_FCN(radv_translate_blend_function(eqA));
         blend_cntl |= S_028780_ALPHA_SRCBLEND(radv_translate_blend_factor(gfx_level, srcA));
         blend_cntl |= S_028780_ALPHA_DESTBLEND(radv_translate_blend_factor(gfx_level, dstA));
      }
      cb_blend_control[i] = blend_cntl;
   }

   if (pdev->info.has_rbplus) {
      /* Disable RB+ blend optimizations for dual source blending. */
      if (mrt0_is_dual_src) {
         for (unsigned i = 0; i < MAX_RTS; i++) {
            sx_mrt_blend_opt[i] =
               S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_NONE) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_NONE);
         }
      }

      /* Disable RB+ blend optimizations on GFX11 when alpha-to-coverage is enabled. */
      if (gfx_level >= GFX11 && d->vk.ms.alpha_to_coverage_enable) {
         sx_mrt_blend_opt[0] =
            S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_NONE) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_NONE);
      }
   }

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028780_CB_BLEND0_CONTROL, MAX_RTS);
   radeon_emit_array(cmd_buffer->cs, cb_blend_control, MAX_RTS);

   if (pdev->info.has_rbplus) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028760_SX_MRT0_BLEND_OPT, MAX_RTS);
      radeon_emit_array(cmd_buffer->cs, sx_mrt_blend_opt, MAX_RTS);
   }
}

static struct radv_shader_part *
lookup_ps_epilog(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_ps_epilog_state state = {0};
   uint8_t color_remap[MAX_RTS];

   memset(color_remap, MESA_VK_ATTACHMENT_UNUSED, sizeof(color_remap));

   state.color_attachment_count = render->color_att_count;
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      state.color_attachment_formats[i] = render->color_att[i].format;
   }

   for (unsigned i = 0; i < MAX_RTS; i++) {
      VkBlendOp eqRGB = d->vk.cb.attachments[i].color_blend_op;
      VkBlendFactor srcRGB = d->vk.cb.attachments[i].src_color_blend_factor;
      VkBlendFactor dstRGB = d->vk.cb.attachments[i].dst_color_blend_factor;

      state.color_write_mask |= d->vk.cb.attachments[i].write_mask << (4 * i);
      state.color_blend_enable |= d->vk.cb.attachments[i].blend_enable << (4 * i);

      radv_normalize_blend_factor(eqRGB, &srcRGB, &dstRGB);

      if (srcRGB == VK_BLEND_FACTOR_SRC_ALPHA || dstRGB == VK_BLEND_FACTOR_SRC_ALPHA ||
          srcRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE || dstRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
          srcRGB == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA || dstRGB == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)
         state.need_src_alpha |= 1 << i;

      state.color_attachment_mappings[i] = d->vk.cal.color_map[i];
      if (state.color_attachment_mappings[i] != MESA_VK_ATTACHMENT_UNUSED)
         color_remap[state.color_attachment_mappings[i]] = i;
   }

   state.mrt0_is_dual_src = radv_is_mrt0_dual_src(cmd_buffer);

   if (d->vk.ms.alpha_to_coverage_enable) {
      /* Select a color export format with alpha when alpha to coverage is enabled. */
      state.need_src_alpha |= 0x1;
   }

   state.alpha_to_one = d->vk.ms.alpha_to_one_enable;

   if (ps) {
      state.colors_written = ps->info.ps.colors_written;

      if (ps->info.ps.exports_mrtz_via_epilog) {
         assert(pdev->info.gfx_level >= GFX11);
         state.export_depth = ps->info.ps.writes_z;
         state.export_stencil = ps->info.ps.writes_stencil;
         state.export_sample_mask = ps->info.ps.writes_sample_mask;
         state.alpha_to_coverage_via_mrtz = d->vk.ms.alpha_to_coverage_enable;
      }
   }

   struct radv_ps_epilog_key key = radv_generate_ps_epilog_key(device, &state);

   /* Determine the actual colors written if outputs are remapped. */
   uint32_t colors_written = 0;
   for (uint32_t i = 0; i < MAX_RTS; i++) {
      if (!((ps->info.ps.colors_written >> (i * 4)) & 0xf))
         continue;

      if (color_remap[i] == MESA_VK_ATTACHMENT_UNUSED)
         continue;

      colors_written |= 0xfu << (4 * color_remap[i]);
   }

   /* Clear color attachments that aren't exported by the FS to match IO shader arguments. */
   key.spi_shader_col_format &= colors_written;

   return radv_shader_part_cache_get(device, &device->ps_epilogs, &cmd_buffer->ps_epilogs, &key);
}

static void
radv_emit_msaa_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned log_samples = util_logbase2(rasterization_samples);
   unsigned pa_sc_aa_config = 0;
   unsigned max_sample_dist = 0;
   unsigned db_eqaa;

   db_eqaa = S_028804_HIGH_QUALITY_INTERSECTIONS(1) | S_028804_INCOHERENT_EQAA_READS(pdev->info.gfx_level < GFX12) |
             S_028804_STATIC_ANCHOR_ASSOCIATIONS(1);

   if (pdev->info.gfx_level >= GFX9 && d->vk.rs.conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT) {
      /* Adjust MSAA state if conservative rasterization is enabled. */
      db_eqaa |= S_028804_OVERRASTERIZATION_AMOUNT(4);
      pa_sc_aa_config |= S_028BE0_AA_MASK_CENTROID_DTMN(1);
   }

   if (!d->sample_location.count) {
      max_sample_dist = radv_get_default_max_sample_dist(log_samples);
   } else {
      uint32_t num_samples = (uint32_t)d->sample_location.per_pixel;
      VkOffset2D sample_locs[4][8]; /* 8 is the max. sample count supported */

      /* Convert the user sample locations to hardware sample locations. */
      radv_convert_user_sample_locs(&d->sample_location, 0, 0, sample_locs[0]);
      radv_convert_user_sample_locs(&d->sample_location, 1, 0, sample_locs[1]);
      radv_convert_user_sample_locs(&d->sample_location, 0, 1, sample_locs[2]);
      radv_convert_user_sample_locs(&d->sample_location, 1, 1, sample_locs[3]);

      /* Compute the maximum sample distance from the specified locations. */
      for (unsigned i = 0; i < 4; ++i) {
         for (uint32_t j = 0; j < num_samples; j++) {
            VkOffset2D offset = sample_locs[i][j];
            max_sample_dist = MAX2(max_sample_dist, MAX2(abs(offset.x), abs(offset.y)));
         }
      }
   }

   if (rasterization_samples > 1) {
      unsigned z_samples = MAX2(render->ds_samples, rasterization_samples);
      unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
      unsigned log_z_samples = util_logbase2(z_samples);
      unsigned log_ps_iter_samples = util_logbase2(ps_iter_samples);
      bool uses_underestimate = d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT;

      pa_sc_aa_config |=
         S_028BE0_MSAA_NUM_SAMPLES(uses_underestimate ? 0 : log_samples) | S_028BE0_MSAA_EXPOSED_SAMPLES(log_samples);

      if (pdev->info.gfx_level >= GFX12) {
         pa_sc_aa_config |= S_028BE0_PS_ITER_SAMPLES(log_ps_iter_samples);

         db_eqaa |= S_028078_MASK_EXPORT_NUM_SAMPLES(log_samples) | S_028078_ALPHA_TO_MASK_NUM_SAMPLES(log_samples);
      } else {
         pa_sc_aa_config |= S_028BE0_MAX_SAMPLE_DIST(max_sample_dist) |
                            S_028BE0_COVERED_CENTROID_IS_CENTER(pdev->info.gfx_level >= GFX10_3);

         db_eqaa |= S_028804_MAX_ANCHOR_SAMPLES(log_z_samples) | S_028804_PS_ITER_SAMPLES(log_ps_iter_samples) |
                    S_028804_MASK_EXPORT_NUM_SAMPLES(log_samples) | S_028804_ALPHA_TO_MASK_NUM_SAMPLES(log_samples);
      }

      if (radv_get_line_mode(cmd_buffer) == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR)
         db_eqaa |= S_028804_OVERRASTERIZATION_AMOUNT(log_samples);
   }

   /* GFX12 programs it in SPI_PS_INPUT_ENA.COVERAGE_TO_SHADER_SELECT */
   pa_sc_aa_config |=
      S_028BE0_COVERAGE_TO_SHADER_SELECT(pdev->info.gfx_level < GFX12 && ps && ps->info.ps.reads_fully_covered);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028C5C_PA_SC_SAMPLE_PROPERTIES,
                             S_028C5C_MAX_SAMPLE_DIST(max_sample_dist));

      radeon_set_context_reg(cmd_buffer->cs, R_028078_DB_EQAA, db_eqaa);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028804_DB_EQAA, db_eqaa);
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028BE0_PA_SC_AA_CONFIG, pa_sc_aa_config);
   radeon_set_context_reg(
      cmd_buffer->cs, R_028A48_PA_SC_MODE_CNTL_0,
      S_028A48_ALTERNATE_RBS_PER_TILE(pdev->info.gfx_level >= GFX9) | S_028A48_VPORT_SCISSOR_ENABLE(1) |
         S_028A48_LINE_STIPPLE_ENABLE(d->vk.rs.line.stipple.enable) | S_028A48_MSAA_ENABLE(rasterization_samples > 1));
}

static void
radv_emit_line_rasterization_mode(struct radv_cmd_buffer *cmd_buffer)
{
   /* The DX10 diamond test is unnecessary with Vulkan and it decreases line rasterization
    * performance.
    */
   radeon_set_context_reg(
      cmd_buffer->cs, R_028BDC_PA_SC_LINE_CNTL,
      S_028BDC_PERPENDICULAR_ENDCAP_ENA(radv_get_line_mode(cmd_buffer) == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR));
}

static void
radv_cmd_buffer_flush_dynamic_state(struct radv_cmd_buffer *cmd_buffer, const uint64_t states)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (states & (RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_DEPTH_CLIP_ENABLE | RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE |
                 RADV_DYNAMIC_DEPTH_CLAMP_ENABLE))
      radv_emit_viewport(cmd_buffer);

   if (states & (RADV_DYNAMIC_SCISSOR | RADV_DYNAMIC_VIEWPORT) && !pdev->info.has_gfx9_scissor_bug)
      radv_emit_scissor(cmd_buffer);

   if (states & RADV_DYNAMIC_LINE_WIDTH)
      radv_emit_line_width(cmd_buffer);

   if (states & RADV_DYNAMIC_BLEND_CONSTANTS)
      radv_emit_blend_constants(cmd_buffer);

   if (states & (RADV_DYNAMIC_STENCIL_REFERENCE | RADV_DYNAMIC_STENCIL_WRITE_MASK | RADV_DYNAMIC_STENCIL_COMPARE_MASK))
      radv_emit_stencil(cmd_buffer);

   if (states & RADV_DYNAMIC_DEPTH_BOUNDS)
      radv_emit_depth_bounds(cmd_buffer);

   if (states & RADV_DYNAMIC_DEPTH_BIAS)
      radv_emit_depth_bias(cmd_buffer);

   if (states &
       (RADV_DYNAMIC_DISCARD_RECTANGLE | RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE | RADV_DYNAMIC_DISCARD_RECTANGLE_MODE))
      radv_emit_discard_rectangle(cmd_buffer);

   if (states & RADV_DYNAMIC_CONSERVATIVE_RAST_MODE)
      radv_emit_conservative_rast_mode(cmd_buffer);

   if (states & RADV_DYNAMIC_SAMPLE_LOCATIONS)
      radv_emit_sample_locations(cmd_buffer);

   if (states & RADV_DYNAMIC_LINE_STIPPLE)
      radv_emit_line_stipple(cmd_buffer);

   if (states & (RADV_DYNAMIC_CULL_MODE | RADV_DYNAMIC_FRONT_FACE | RADV_DYNAMIC_DEPTH_BIAS_ENABLE |
                 RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE | RADV_DYNAMIC_PROVOKING_VERTEX_MODE |
                 RADV_DYNAMIC_LINE_RASTERIZATION_MODE))
      radv_emit_culling(cmd_buffer);

   if (states & (RADV_DYNAMIC_PROVOKING_VERTEX_MODE | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY))
      radv_emit_provoking_vertex_mode(cmd_buffer);

   if ((states & RADV_DYNAMIC_PRIMITIVE_TOPOLOGY) ||
       (pdev->info.gfx_level >= GFX12 && states & RADV_DYNAMIC_PATCH_CONTROL_POINTS))
      radv_emit_primitive_topology(cmd_buffer);

   if (states & (RADV_DYNAMIC_DEPTH_TEST_ENABLE | RADV_DYNAMIC_DEPTH_WRITE_ENABLE | RADV_DYNAMIC_DEPTH_COMPARE_OP |
                 RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE | RADV_DYNAMIC_STENCIL_TEST_ENABLE | RADV_DYNAMIC_STENCIL_OP))
      radv_emit_depth_control(cmd_buffer);

   if (states & RADV_DYNAMIC_STENCIL_OP)
      radv_emit_stencil_control(cmd_buffer);

   if (states & RADV_DYNAMIC_FRAGMENT_SHADING_RATE)
      radv_emit_fragment_shading_rate(cmd_buffer);

   if (states & RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE)
      radv_emit_primitive_restart_enable(cmd_buffer);

   if (states & (RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE | RADV_DYNAMIC_DEPTH_CLIP_ENABLE |
                 RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE | RADV_DYNAMIC_DEPTH_CLAMP_ENABLE))
      radv_emit_clipping(cmd_buffer);

   if (states & (RADV_DYNAMIC_LOGIC_OP | RADV_DYNAMIC_LOGIC_OP_ENABLE | RADV_DYNAMIC_COLOR_WRITE_MASK |
                 RADV_DYNAMIC_COLOR_BLEND_ENABLE | RADV_DYNAMIC_COLOR_BLEND_EQUATION))
      radv_emit_logic_op(cmd_buffer);

   if (states & (RADV_DYNAMIC_COLOR_WRITE_ENABLE | RADV_DYNAMIC_COLOR_WRITE_MASK))
      radv_emit_color_write(cmd_buffer);

   if (states & RADV_DYNAMIC_VERTEX_INPUT)
      radv_emit_vertex_input(cmd_buffer);

   if (states & RADV_DYNAMIC_PATCH_CONTROL_POINTS)
      radv_emit_patch_control_points(cmd_buffer);

   if (states & RADV_DYNAMIC_TESS_DOMAIN_ORIGIN)
      radv_emit_tess_domain_origin(cmd_buffer);

   if (states & RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE)
      radv_emit_alpha_to_coverage_enable(cmd_buffer);

   if (states & RADV_DYNAMIC_SAMPLE_MASK)
      radv_emit_sample_mask(cmd_buffer);

   if (states & (RADV_DYNAMIC_DEPTH_CLAMP_ENABLE | RADV_DYNAMIC_DEPTH_CLIP_ENABLE))
      radv_emit_depth_clamp_enable(cmd_buffer);

   if (states & (RADV_DYNAMIC_COLOR_BLEND_ENABLE | RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_COLOR_BLEND_EQUATION |
                 RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE))
      radv_emit_color_blend(cmd_buffer);

   if (states & (RADV_DYNAMIC_LINE_RASTERIZATION_MODE | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE))
      radv_emit_line_rasterization_mode(cmd_buffer);

   if (states & (RADV_DYNAMIC_RASTERIZATION_SAMPLES | RADV_DYNAMIC_LINE_RASTERIZATION_MODE |
                 RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE))
      radv_emit_rasterization_samples(cmd_buffer);

   if (states & (RADV_DYNAMIC_LINE_STIPPLE_ENABLE | RADV_DYNAMIC_CONSERVATIVE_RAST_MODE |
                 RADV_DYNAMIC_SAMPLE_LOCATIONS | RADV_DYNAMIC_RASTERIZATION_SAMPLES |
                 RADV_DYNAMIC_LINE_RASTERIZATION_MODE | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE))
      radv_emit_msaa_state(cmd_buffer);

   /* RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE is handled by radv_emit_db_shader_control. */

   cmd_buffer->state.dirty_dynamic &= ~states;
}

static void
radv_flush_push_descriptors(struct radv_cmd_buffer *cmd_buffer, struct radv_descriptor_state *descriptors_state)
{
   struct radv_descriptor_set *set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   unsigned bo_offset;

   if (!radv_cmd_buffer_upload_data(cmd_buffer, set->header.size, set->header.mapped_ptr, &bo_offset))
      return;

   set->header.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   set->header.va += bo_offset;
}

static void
radv_flush_indirect_descriptor_sets(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   uint32_t size = MAX_SETS * 4;
   uint32_t offset;
   void *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &offset, &ptr))
      return;

   for (unsigned i = 0; i < MAX_SETS; i++) {
      uint32_t *uptr = ((uint32_t *)ptr) + i;
      uint64_t set_va = 0;
      if (descriptors_state->valid & (1u << i))
         set_va = radv_descriptor_get_va(descriptors_state, i);

      uptr[0] = set_va & 0xffffffff;
   }

   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint64_t va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   va += offset;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs, MESA_VULKAN_SHADER_STAGES * 3);

   if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      for (unsigned s = MESA_SHADER_VERTEX; s <= MESA_SHADER_FRAGMENT; s++)
         if (radv_cmdbuf_has_stage(cmd_buffer, s))
            radv_emit_userdata_address(device, cs, cmd_buffer->state.shaders[s],
                                       cmd_buffer->state.shaders[s]->info.user_data_0, AC_UD_INDIRECT_DESCRIPTOR_SETS,
                                       va);

      if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_MESH))
         radv_emit_userdata_address(device, cs, cmd_buffer->state.shaders[MESA_SHADER_MESH],
                                    cmd_buffer->state.shaders[MESA_SHADER_MESH]->info.user_data_0,
                                    AC_UD_INDIRECT_DESCRIPTOR_SETS, va);

      if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
         radeon_check_space(device->ws, cmd_buffer->gang.cs, 3);
         radv_emit_userdata_address(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                    cmd_buffer->state.shaders[MESA_SHADER_TASK]->info.user_data_0,
                                    AC_UD_INDIRECT_DESCRIPTOR_SETS, va);
      }
   } else {
      struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                              ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                              : cmd_buffer->state.rt_prolog;

      radv_emit_userdata_address(device, cs, compute_shader, compute_shader->info.user_data_0,
                                 AC_UD_INDIRECT_DESCRIPTOR_SETS, va);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

ALWAYS_INLINE static void
radv_flush_descriptors(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   bool flush_indirect_descriptors;

   if (!descriptors_state->dirty)
      return;

   flush_indirect_descriptors = descriptors_state->need_indirect_descriptor_sets;

   if (flush_indirect_descriptors)
      radv_flush_indirect_descriptor_sets(cmd_buffer, bind_point);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs, MAX_SETS * MESA_VULKAN_SHADER_STAGES * 4);

   if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                              ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                              : cmd_buffer->state.rt_prolog;

      radv_emit_descriptor_pointers(device, cs, compute_shader, compute_shader->info.user_data_0, descriptors_state);
   } else {
      radv_foreach_stage(stage, stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
      {
         if (!cmd_buffer->state.shaders[stage])
            continue;

         radv_emit_descriptor_pointers(device, cs, cmd_buffer->state.shaders[stage],
                                       cmd_buffer->state.shaders[stage]->info.user_data_0, descriptors_state);
      }

      if (stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_descriptor_pointers(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                       cmd_buffer->state.shaders[MESA_SHADER_TASK]->info.user_data_0,
                                       descriptors_state);
      }
   }

   descriptors_state->dirty = 0;

   assert(cmd_buffer->cs->cdw <= cdw_max);

   if (radv_device_fault_detection_enabled(device))
      radv_save_descriptors(cmd_buffer, bind_point);
}

static void
radv_emit_all_inline_push_consts(struct radv_device *device, struct radeon_cmdbuf *cs, struct radv_shader *shader,
                                 uint32_t base_reg, uint32_t *values, bool *need_push_constants)
{
   if (radv_get_user_sgpr_info(shader, AC_UD_PUSH_CONSTANTS)->sgpr_idx != -1)
      *need_push_constants |= true;

   const uint64_t mask = shader->info.inline_push_constant_mask;
   if (!mask)
      return;

   const uint8_t base = ffs(mask) - 1;
   if (mask == u_bit_consecutive64(base, util_last_bit64(mask) - base)) {
      /* consecutive inline push constants */
      radv_emit_inline_push_consts(device, cs, shader, base_reg, AC_UD_INLINE_PUSH_CONSTANTS, values + base);
   } else {
      /* sparse inline push constants */
      uint32_t consts[AC_MAX_INLINE_PUSH_CONSTS];
      unsigned num_consts = 0;
      u_foreach_bit64 (idx, mask)
         consts[num_consts++] = values[idx];
      radv_emit_inline_push_consts(device, cs, shader, base_reg, AC_UD_INLINE_PUSH_CONSTANTS, consts);
   }
}

ALWAYS_INLINE static VkShaderStageFlags
radv_must_flush_constants(const struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages,
                          VkPipelineBindPoint bind_point)
{
   const struct radv_push_constant_state *push_constants = radv_get_push_constants_state(cmd_buffer, bind_point);

   if (push_constants->size || push_constants->dynamic_offset_count)
      return stages & cmd_buffer->push_constant_stages;

   return 0;
}

static void
radv_flush_constants(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages, VkPipelineBindPoint bind_point)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   const struct radv_push_constant_state *push_constants = radv_get_push_constants_state(cmd_buffer, bind_point);
   struct radv_shader *shader, *prev_shader;
   bool need_push_constants = false;
   unsigned offset;
   void *ptr;
   uint64_t va;
   uint32_t internal_stages = stages;
   uint32_t dirty_stages = 0;

   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      break;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      dirty_stages = RADV_RT_STAGE_BITS;
      break;
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
      internal_stages = VK_SHADER_STAGE_COMPUTE_BIT;
      dirty_stages = VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   default:
      unreachable("Unhandled bind point");
   }

   if (internal_stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                              ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                              : cmd_buffer->state.rt_prolog;

      radv_emit_all_inline_push_consts(device, cs, compute_shader, compute_shader->info.user_data_0,
                                       (uint32_t *)cmd_buffer->push_constants, &need_push_constants);
   } else {
      radv_foreach_stage(stage, internal_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
      {
         shader = radv_get_shader(cmd_buffer->state.shaders, stage);

         if (!shader)
            continue;

         radv_emit_all_inline_push_consts(device, cs, shader, shader->info.user_data_0,
                                          (uint32_t *)cmd_buffer->push_constants, &need_push_constants);
      }

      if (internal_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_all_inline_push_consts(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                          cmd_buffer->state.shaders[MESA_SHADER_TASK]->info.user_data_0,
                                          (uint32_t *)cmd_buffer->push_constants, &need_push_constants);
      }
   }

   if (need_push_constants) {
      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, push_constants->size + 16 * push_constants->dynamic_offset_count,
                                        &offset, &ptr))
         return;

      memcpy(ptr, cmd_buffer->push_constants, push_constants->size);
      memcpy((char *)ptr + push_constants->size, descriptors_state->dynamic_buffers,
             16 * push_constants->dynamic_offset_count);

      va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      va += offset;

      ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, MESA_VULKAN_SHADER_STAGES * 4);

      if (internal_stages & VK_SHADER_STAGE_COMPUTE_BIT) {
         struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                                 ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                                 : cmd_buffer->state.rt_prolog;

         radv_emit_userdata_address(device, cs, compute_shader, compute_shader->info.user_data_0, AC_UD_PUSH_CONSTANTS,
                                    va);
      } else {
         prev_shader = NULL;
         radv_foreach_stage(stage, internal_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
         {
            shader = radv_get_shader(cmd_buffer->state.shaders, stage);

            /* Avoid redundantly emitting the address for merged stages. */
            if (shader && shader != prev_shader) {
               radv_emit_userdata_address(device, cs, shader, shader->info.user_data_0, AC_UD_PUSH_CONSTANTS, va);

               prev_shader = shader;
            }
         }

         if (internal_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
            radv_emit_userdata_address(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                       cmd_buffer->state.shaders[MESA_SHADER_TASK]->info.user_data_0,
                                       AC_UD_PUSH_CONSTANTS, va);
         }
      }

      assert(cmd_buffer->cs->cdw <= cdw_max);
   }

   cmd_buffer->push_constant_stages &= ~stages;
   cmd_buffer->push_constant_stages |= dirty_stages;
}

void
radv_write_vertex_descriptors(const struct radv_cmd_buffer *cmd_buffer, const struct radv_graphics_pipeline *pipeline,
                              bool full_null_descriptors, void *vb_ptr)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_shader *vs_shader = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   enum amd_gfx_level chip = pdev->info.gfx_level;
   enum radeon_family family = pdev->info.family;
   unsigned desc_index = 0;
   uint32_t mask = vs_shader->info.vs.vb_desc_usage_mask;
   uint64_t va;
   const struct radv_vs_input_state *vs_state =
      vs_shader->info.vs.dynamic_inputs ? &cmd_buffer->state.dynamic_vs_input : NULL;
   assert(!vs_state || vs_shader->info.vs.use_per_attribute_vb_descs);

   const struct ac_vtx_format_info *vtx_info_table = vs_state ? ac_get_vtx_format_info_table(chip, family) : NULL;

   while (mask) {
      unsigned i = u_bit_scan(&mask);
      uint32_t *desc = &((uint32_t *)vb_ptr)[desc_index++ * 4];
      uint32_t offset, rsrc_word3;

      if (vs_state && !(vs_state->attribute_mask & BITFIELD_BIT(i))) {
         /* No vertex attribute description given: assume that the shader doesn't use this
          * location (vb_desc_usage_mask can be larger than attribute usage) and use a null
          * descriptor to avoid hangs (prologs load all attributes, even if there are holes).
          */
         memset(desc, 0, 4 * 4);
         continue;
      }

      unsigned binding = vs_state ? cmd_buffer->state.dynamic_vs_input.bindings[i]
                                  : (vs_shader->info.vs.use_per_attribute_vb_descs ? pipeline->attrib_bindings[i] : i);
      struct radv_buffer *buffer = cmd_buffer->vertex_binding_buffers[binding];
      unsigned num_records;
      unsigned stride;

      if (vs_state && !(vs_state->nontrivial_formats & BITFIELD_BIT(i))) {
         const struct ac_vtx_format_info *vtx_info = &vtx_info_table[vs_state->formats[i]];
         unsigned hw_format = vtx_info->hw_format[vtx_info->num_channels - 1];

         if (chip >= GFX10) {
            rsrc_word3 = vtx_info->dst_sel | S_008F0C_FORMAT_GFX10(hw_format);
         } else {
            rsrc_word3 =
               vtx_info->dst_sel | S_008F0C_NUM_FORMAT((hw_format >> 4) & 0x7) | S_008F0C_DATA_FORMAT(hw_format & 0xf);
         }
      } else {
         rsrc_word3 = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
                      S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);
         if (chip >= GFX10)
            rsrc_word3 |= S_008F0C_FORMAT_GFX10(V_008F0C_GFX10_FORMAT_32_UINT);
         else
            rsrc_word3 |=
               S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_UINT) | S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
      }

      if (cmd_buffer->state.uses_dynamic_vertex_binding_stride) {
         stride = cmd_buffer->vertex_bindings[binding].stride;
      } else {
         stride = pipeline->binding_stride[binding];
      }

      if (!buffer) {
         if (full_null_descriptors) {
            /* Put all the info in for the DGC generation shader in case the VBO gets overridden. */
            desc[0] = 0;
            desc[1] = S_008F04_STRIDE(stride);
            desc[2] = 0;
            desc[3] = rsrc_word3;
         } else if (vs_state) {
            /* Stride needs to be non-zero on GFX9, or else bounds checking is disabled. We need
             * to include the format/word3 so that the alpha channel is 1 for formats without an
             * alpha channel.
             */
            desc[0] = 0;
            desc[1] = S_008F04_STRIDE(16);
            desc[2] = 0;
            desc[3] = rsrc_word3;
         } else {
            memset(desc, 0, 4 * 4);
         }

         continue;
      }

      va = radv_buffer_get_va(buffer->bo);

      offset = cmd_buffer->vertex_bindings[binding].offset;
      va += offset + buffer->offset;
      if (vs_state)
         va += vs_state->offsets[i];

      if (cmd_buffer->vertex_bindings[binding].size) {
         num_records = cmd_buffer->vertex_bindings[binding].size;
      } else {
         num_records = vk_buffer_range(&buffer->vk, offset, VK_WHOLE_SIZE);
      }

      if (vs_shader->info.vs.use_per_attribute_vb_descs) {
         uint32_t attrib_end = vs_state ? vs_state->offsets[i] + vs_state->format_sizes[i] : pipeline->attrib_ends[i];

         if (num_records < attrib_end) {
            num_records = 0; /* not enough space for one vertex */
         } else if (stride == 0) {
            num_records = 1; /* only one vertex */
         } else {
            num_records = (num_records - attrib_end) / stride + 1;
            /* If attrib_offset>stride, then the compiler will increase the vertex index by
             * attrib_offset/stride and decrease the offset by attrib_offset%stride. This is
             * only allowed with static strides.
             */
            num_records += pipeline ? pipeline->attrib_index_offset[i] : 0;
         }

         /* GFX10 uses OOB_SELECT_RAW if stride==0, so convert num_records from elements into
          * into bytes in that case. GFX8 always uses bytes.
          */
         if (num_records && (chip == GFX8 || (chip != GFX9 && !stride))) {
            num_records = (num_records - 1) * stride + attrib_end;
         } else if (!num_records) {
            /* On GFX9, it seems bounds checking is disabled if both
             * num_records and stride are zero. This doesn't seem necessary on GFX8, GFX10 and
             * GFX10.3 but it doesn't hurt.
             */
            if (full_null_descriptors) {
               /* Put all the info in for the DGC generation shader in case the VBO gets overridden.
                */
               desc[0] = 0;
               desc[1] = S_008F04_STRIDE(stride);
               desc[2] = 0;
               desc[3] = rsrc_word3;
            } else if (vs_state) {
               desc[0] = 0;
               desc[1] = S_008F04_STRIDE(16);
               desc[2] = 0;
               desc[3] = rsrc_word3;
            } else {
               memset(desc, 0, 16);
            }

            continue;
         }
      } else {
         if (chip != GFX8 && stride)
            num_records = DIV_ROUND_UP(num_records, stride);
      }

      if (chip >= GFX10) {
         /* OOB_SELECT chooses the out-of-bounds check:
          * - 1: index >= NUM_RECORDS (Structured)
          * - 3: offset >= NUM_RECORDS (Raw)
          */
         int oob_select = stride ? V_008F0C_OOB_SELECT_STRUCTURED : V_008F0C_OOB_SELECT_RAW;
         rsrc_word3 |= S_008F0C_OOB_SELECT(oob_select) | S_008F0C_RESOURCE_LEVEL(chip < GFX11);
      }

      desc[0] = va;
      desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) | S_008F04_STRIDE(stride);
      desc[2] = num_records;
      desc[3] = rsrc_word3;
   }
}

static void
radv_flush_vertex_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_shader *vs = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!vs->info.vs.vb_desc_usage_mask)
      return;

   /* Mesh shaders don't have vertex descriptors. */
   assert(!cmd_buffer->state.mesh_shading);

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   unsigned vb_desc_alloc_size = util_bitcount(vs->info.vs.vb_desc_usage_mask) * 16;
   unsigned vb_offset;
   void *vb_ptr;
   uint64_t va;

   /* allocate some descriptor state for vertex buffers */
   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, vb_desc_alloc_size, &vb_offset, &vb_ptr))
      return;

   radv_write_vertex_descriptors(cmd_buffer, pipeline, false, vb_ptr);

   va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   va += vb_offset;

   radv_emit_userdata_address(device, cmd_buffer->cs, vs, vs->info.user_data_0, AC_UD_VS_VERTEX_BUFFERS, va);

   cmd_buffer->state.vb_va = va;
   cmd_buffer->state.vb_size = vb_desc_alloc_size;
   cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_VBO_DESCRIPTORS;

   if (radv_device_fault_detection_enabled(device))
      radv_save_vertex_descriptors(cmd_buffer, (uintptr_t)vb_ptr);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_VERTEX_BUFFER;
}

static void
radv_emit_streamout_buffers(struct radv_cmd_buffer *cmd_buffer, uint64_t va)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   uint32_t streamout_buffers_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_STREAMOUT_BUFFERS);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!streamout_buffers_offset)
      return;

   radv_emit_shader_pointer(device, cmd_buffer->cs, streamout_buffers_offset, va, false);

   if (cmd_buffer->state.gs_copy_shader) {
      streamout_buffers_offset = radv_get_user_sgpr_loc(cmd_buffer->state.gs_copy_shader, AC_UD_STREAMOUT_BUFFERS);
      if (streamout_buffers_offset)
         radv_emit_shader_pointer(device, cmd_buffer->cs, streamout_buffers_offset, va, false);
   }
}

static void
radv_emit_streamout_state(struct radv_cmd_buffer *cmd_buffer, uint64_t va)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const uint32_t streamout_state_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_STREAMOUT_STATE);
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!streamout_state_offset)
      return;

   radv_emit_shader_pointer(device, cmd_buffer->cs, streamout_state_offset, va, false);
}

static void
radv_flush_streamout_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_STREAMOUT_BUFFER) {
      struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
      struct radv_streamout_state *so = &cmd_buffer->state.streamout;
      unsigned so_offset;
      uint64_t desc_va;
      void *so_ptr;

      /* Allocate some descriptor state for streamout buffers. */
      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, MAX_SO_BUFFERS * 16, &so_offset, &so_ptr))
         return;

      for (uint32_t i = 0; i < MAX_SO_BUFFERS; i++) {
         struct radv_buffer *buffer = sb[i].buffer;
         uint32_t *desc = &((uint32_t *)so_ptr)[i * 4];
         uint32_t size = 0;
         uint64_t va = 0;

         if (so->enabled_mask & (1 << i)) {
            va = radv_buffer_get_va(buffer->bo) + buffer->offset;

            va += sb[i].offset;

            /* Set the descriptor.
             *
             * On GFX8, the format must be non-INVALID, otherwise
             * the buffer will be considered not bound and store
             * instructions will be no-ops.
             */
            size = 0xffffffff;

            if (pdev->use_ngg_streamout) {
               /* With NGG streamout, the buffer size is used to determine the max emit per buffer
                * and also acts as a disable bit when it's 0.
                */
               size = radv_is_streamout_enabled(cmd_buffer) ? sb[i].size : 0;
            }
         }

         ac_build_raw_buffer_descriptor(pdev->info.gfx_level, va, size, desc);
      }

      desc_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      desc_va += so_offset;

      radv_emit_streamout_buffers(cmd_buffer, desc_va);

      if (pdev->info.gfx_level >= GFX12) {
         const uint8_t first_target = ffs(so->enabled_mask) - 1;
         unsigned state_offset;
         uint64_t state_va;
         void *state_ptr;

         /* The layout is:
          *    struct {
          *       struct {
          *          uint32_t ordered_id; // equal for all buffers
          *          uint32_t dwords_written;
          *       } buffer[4];
          *    };
          *
          * The buffer must be initialized to 0 and the address must be aligned to 64
          * because it's faster when the atomic doesn't straddle a 64B block boundary.
          */
         if (!radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, MAX_SO_BUFFERS * 8, 64, &state_offset, &state_ptr))
            return;

         memset(state_ptr, 0, MAX_SO_BUFFERS * 8);

         state_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
         state_va += state_offset;

         /* The first enabled streamout target will contain the ordered ID/offset buffer for all
          * targets.
          */
         state_va += first_target * 8;

         radv_emit_streamout_state(cmd_buffer, state_va);
      }
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_STREAMOUT_BUFFER;
}

static void
radv_flush_shader_query_state_gfx(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const uint32_t shader_query_state_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_SHADER_QUERY_STATE);
   enum radv_shader_query_state shader_query_state = radv_shader_query_none;

   if (!shader_query_state_offset)
      return;

   assert(last_vgt_shader->info.is_ngg || last_vgt_shader->info.stage == MESA_SHADER_GEOMETRY);

   /* By default shader queries are disabled but they are enabled if the command buffer has active GDS
    * queries or if it's a secondary command buffer that inherits the number of generated
    * primitives.
    */
   if (cmd_buffer->state.active_pipeline_gds_queries ||
       (cmd_buffer->state.inherited_pipeline_statistics &
        (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT)) ||
       (pdev->emulate_mesh_shader_queries && (cmd_buffer->state.inherited_pipeline_statistics &
                                              VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT)))
      shader_query_state |= radv_shader_query_pipeline_stat;

   if (cmd_buffer->state.active_prims_gen_gds_queries)
      shader_query_state |= radv_shader_query_prim_gen;

   if (cmd_buffer->state.active_prims_xfb_gds_queries && radv_is_streamout_enabled(cmd_buffer)) {
      shader_query_state |= radv_shader_query_prim_xfb | radv_shader_query_prim_gen;
   }

   radeon_set_sh_reg(cmd_buffer->cs, shader_query_state_offset, shader_query_state);
}

static void
radv_flush_shader_query_state_ace(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *task_shader)
{
   const uint32_t shader_query_state_offset = radv_get_user_sgpr_loc(task_shader, AC_UD_SHADER_QUERY_STATE);
   enum radv_shader_query_state shader_query_state = radv_shader_query_none;

   if (!shader_query_state_offset)
      return;

   /* By default shader queries are disabled but they are enabled if the command buffer has active ACE
    * queries or if it's a secondary command buffer that inherits the number of task shader
    * invocations query.
    */
   if (cmd_buffer->state.active_pipeline_ace_queries ||
       (cmd_buffer->state.inherited_pipeline_statistics & VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT))
      shader_query_state |= radv_shader_query_pipeline_stat;

   radeon_set_sh_reg(cmd_buffer->gang.cs, shader_query_state_offset, shader_query_state);
}

static void
radv_flush_shader_query_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   radv_flush_shader_query_state_gfx(cmd_buffer);

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK) && pdev->emulate_mesh_shader_queries)
      radv_flush_shader_query_state_ace(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_TASK]);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_SHADER_QUERY;
}

static void
radv_flush_force_vrs_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;

   if (!last_vgt_shader->info.force_vrs_per_vertex) {
      /* Un-set the SGPR index so we know to re-emit it later. */
      cmd_buffer->state.last_vrs_rates_sgpr_idx = -1;
      return;
   }

   const struct radv_userdata_info *loc;
   uint32_t base_reg;

   if (cmd_buffer->state.gs_copy_shader) {
      loc = &cmd_buffer->state.gs_copy_shader->info.user_sgprs_locs.shader_data[AC_UD_FORCE_VRS_RATES];
      base_reg = R_00B130_SPI_SHADER_USER_DATA_VS_0;
   } else {
      loc = radv_get_user_sgpr_info(last_vgt_shader, AC_UD_FORCE_VRS_RATES);
      base_reg = last_vgt_shader->info.user_data_0;
   }

   assert(loc->sgpr_idx != -1);

   enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   uint32_t vrs_rates = 0;

   switch (device->force_vrs) {
   case RADV_FORCE_VRS_2x2:
      vrs_rates = gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_2X2 : (1u << 2) | (1u << 4);
      break;
   case RADV_FORCE_VRS_2x1:
      vrs_rates = gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_2X1 : (1u << 2) | (0u << 4);
      break;
   case RADV_FORCE_VRS_1x2:
      vrs_rates = gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_1X2 : (0u << 2) | (1u << 4);
      break;
   default:
      break;
   }

   if (cmd_buffer->state.last_vrs_rates != vrs_rates || cmd_buffer->state.last_vrs_rates_sgpr_idx != loc->sgpr_idx) {
      radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, vrs_rates);
   }

   cmd_buffer->state.last_vrs_rates = vrs_rates;
   cmd_buffer->state.last_vrs_rates_sgpr_idx = loc->sgpr_idx;
}

static void
radv_upload_graphics_shader_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_VERTEX_BUFFER)
      radv_flush_vertex_descriptors(cmd_buffer);

   radv_flush_streamout_descriptors(cmd_buffer);

   VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS;
   radv_flush_descriptors(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);

   const VkShaderStageFlags pc_stages = radv_must_flush_constants(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, VK_PIPELINE_BIND_POINT_GRAPHICS);

   radv_flush_force_vrs_state(cmd_buffer);
}

struct radv_draw_info {
   /**
    * Number of vertices.
    */
   uint32_t count;

   /**
    * First instance id.
    */
   uint32_t first_instance;

   /**
    * Number of instances.
    */
   uint32_t instance_count;

   /**
    * Whether it's an indexed draw.
    */
   bool indexed;

   /**
    * Indirect draw parameters resource.
    */
   struct radv_buffer *indirect;
   uint64_t indirect_offset;
   uint32_t stride;

   /**
    * Draw count parameters resource.
    */
   struct radv_buffer *count_buffer;
   uint64_t count_buffer_offset;

   /**
    * Stream output parameters resource.
    */
   struct radv_buffer *strmout_buffer;
   uint64_t strmout_buffer_offset;
};

struct radv_prim_vertex_count {
   uint8_t min;
   uint8_t incr;
};

static inline unsigned
radv_prims_for_vertices(struct radv_prim_vertex_count *info, unsigned num)
{
   if (num == 0)
      return 0;

   if (info->incr == 0)
      return 0;

   if (num < info->min)
      return 0;

   return 1 + ((num - info->min) / info->incr);
}

static const struct radv_prim_vertex_count prim_size_table[] = {
   [V_008958_DI_PT_NONE] = {0, 0},          [V_008958_DI_PT_POINTLIST] = {1, 1},
   [V_008958_DI_PT_LINELIST] = {2, 2},      [V_008958_DI_PT_LINESTRIP] = {2, 1},
   [V_008958_DI_PT_TRILIST] = {3, 3},       [V_008958_DI_PT_TRIFAN] = {3, 1},
   [V_008958_DI_PT_TRISTRIP] = {3, 1},      [V_008958_DI_PT_LINELIST_ADJ] = {4, 4},
   [V_008958_DI_PT_LINESTRIP_ADJ] = {4, 1}, [V_008958_DI_PT_TRILIST_ADJ] = {6, 6},
   [V_008958_DI_PT_TRISTRIP_ADJ] = {6, 2},  [V_008958_DI_PT_RECTLIST] = {3, 3},
   [V_008958_DI_PT_LINELOOP] = {2, 1},      [V_008958_DI_PT_POLYGON] = {3, 1},
   [V_008958_DI_PT_2D_TRI_STRIP] = {0, 0},
};

static uint32_t
radv_get_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer, bool instanced_draw, bool indirect_draw,
                            bool count_from_stream_output, uint32_t draw_vertex_count, unsigned topology,
                            bool prim_restart_enable, unsigned patch_control_points, unsigned num_tess_patches)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   const unsigned max_primgroup_in_wave = 2;
   /* SWITCH_ON_EOP(0) is always preferable. */
   bool wd_switch_on_eop = false;
   bool ia_switch_on_eop = false;
   bool ia_switch_on_eoi = false;
   bool partial_vs_wave = false;
   bool partial_es_wave = cmd_buffer->state.ia_multi_vgt_param.partial_es_wave;
   bool multi_instances_smaller_than_primgroup;
   struct radv_prim_vertex_count prim_vertex_count = prim_size_table[topology];
   unsigned primgroup_size;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TESS_CTRL)) {
      primgroup_size = num_tess_patches;
   } else if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY)) {
      primgroup_size = 64;
   } else {
      primgroup_size = 128; /* recommended without a GS */
   }

   /* GS requirement. */
   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY) && gpu_info->gfx_level <= GFX8) {
      unsigned gs_table_depth = pdev->gs_table_depth;
      if (SI_GS_PER_ES / primgroup_size >= gs_table_depth - 3)
         partial_es_wave = true;
   }

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TESS_CTRL)) {
      if (topology == V_008958_DI_PT_PATCH) {
         prim_vertex_count.min = patch_control_points;
         prim_vertex_count.incr = 1;
      }
   }

   multi_instances_smaller_than_primgroup = indirect_draw;
   if (!multi_instances_smaller_than_primgroup && instanced_draw) {
      uint32_t num_prims = radv_prims_for_vertices(&prim_vertex_count, draw_vertex_count);
      if (num_prims < primgroup_size)
         multi_instances_smaller_than_primgroup = true;
   }

   ia_switch_on_eoi = cmd_buffer->state.ia_multi_vgt_param.ia_switch_on_eoi;
   partial_vs_wave = cmd_buffer->state.ia_multi_vgt_param.partial_vs_wave;

   if (gpu_info->gfx_level >= GFX7) {
      /* WD_SWITCH_ON_EOP has no effect on GPUs with less than
       * 4 shader engines. Set 1 to pass the assertion below.
       * The other cases are hardware requirements. */
      if (gpu_info->max_se < 4 || topology == V_008958_DI_PT_POLYGON || topology == V_008958_DI_PT_LINELOOP ||
          topology == V_008958_DI_PT_TRIFAN || topology == V_008958_DI_PT_TRISTRIP_ADJ ||
          (prim_restart_enable && (gpu_info->family < CHIP_POLARIS10 ||
                                   (topology != V_008958_DI_PT_POINTLIST && topology != V_008958_DI_PT_LINESTRIP))))
         wd_switch_on_eop = true;

      /* Hawaii hangs if instancing is enabled and WD_SWITCH_ON_EOP is 0.
       * We don't know that for indirect drawing, so treat it as
       * always problematic. */
      if (gpu_info->family == CHIP_HAWAII && (instanced_draw || indirect_draw))
         wd_switch_on_eop = true;

      /* Performance recommendation for 4 SE Gfx7-8 parts if
       * instances are smaller than a primgroup.
       * Assume indirect draws always use small instances.
       * This is needed for good VS wave utilization.
       */
      if (gpu_info->gfx_level <= GFX8 && gpu_info->max_se == 4 && multi_instances_smaller_than_primgroup)
         wd_switch_on_eop = true;

      /* Hardware requirement when drawing primitives from a stream
       * output buffer.
       */
      if (count_from_stream_output)
         wd_switch_on_eop = true;

      /* Required on GFX7 and later. */
      if (gpu_info->max_se > 2 && !wd_switch_on_eop)
         ia_switch_on_eoi = true;

      /* Required by Hawaii and, for some special cases, by GFX8. */
      if (ia_switch_on_eoi &&
          (gpu_info->family == CHIP_HAWAII ||
           (gpu_info->gfx_level == GFX8 &&
            /* max primgroup in wave is always 2 - leave this for documentation */
            (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY) || max_primgroup_in_wave != 2))))
         partial_vs_wave = true;

      /* Instancing bug on Bonaire. */
      if (gpu_info->family == CHIP_BONAIRE && ia_switch_on_eoi && (instanced_draw || indirect_draw))
         partial_vs_wave = true;

      /* If the WD switch is false, the IA switch must be false too. */
      assert(wd_switch_on_eop || !ia_switch_on_eop);
   }
   /* If SWITCH_ON_EOI is set, PARTIAL_ES_WAVE must be set too. */
   if (gpu_info->gfx_level <= GFX8 && ia_switch_on_eoi)
      partial_es_wave = true;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY)) {
      /* GS hw bug with single-primitive instances and SWITCH_ON_EOI.
       * The hw doc says all multi-SE chips are affected, but amdgpu-pro Vulkan
       * only applies it to Hawaii. Do what amdgpu-pro Vulkan does.
       */
      if (gpu_info->family == CHIP_HAWAII && ia_switch_on_eoi) {
         bool set_vgt_flush = indirect_draw;
         if (!set_vgt_flush && instanced_draw) {
            uint32_t num_prims = radv_prims_for_vertices(&prim_vertex_count, draw_vertex_count);
            if (num_prims <= 1)
               set_vgt_flush = true;
         }
         if (set_vgt_flush)
            cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_FLUSH;
      }
   }

   /* Workaround for a VGT hang when strip primitive types are used with
    * primitive restart.
    */
   if (prim_restart_enable && (topology == V_008958_DI_PT_LINESTRIP || topology == V_008958_DI_PT_TRISTRIP ||
                               topology == V_008958_DI_PT_LINESTRIP_ADJ || topology == V_008958_DI_PT_TRISTRIP_ADJ)) {
      partial_vs_wave = true;
   }

   return cmd_buffer->state.ia_multi_vgt_param.base | S_028AA8_PRIMGROUP_SIZE(primgroup_size - 1) |
          S_028AA8_SWITCH_ON_EOP(ia_switch_on_eop) | S_028AA8_SWITCH_ON_EOI(ia_switch_on_eoi) |
          S_028AA8_PARTIAL_VS_WAVE_ON(partial_vs_wave) | S_028AA8_PARTIAL_ES_WAVE_ON(partial_es_wave) |
          S_028AA8_WD_SWITCH_ON_EOP(gpu_info->gfx_level >= GFX7 ? wd_switch_on_eop : 0);
}

static void
radv_emit_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer, bool instanced_draw, bool indirect_draw,
                             bool count_from_stream_output, uint32_t draw_vertex_count)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   struct radv_cmd_state *state = &cmd_buffer->state;
   const unsigned patch_control_points = state->dynamic.vk.ts.patch_control_points;
   const unsigned topology = state->dynamic.vk.ia.primitive_topology;
   const bool prim_restart_enable = state->dynamic.vk.ia.primitive_restart_enable;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned ia_multi_vgt_param;

   ia_multi_vgt_param = radv_get_ia_multi_vgt_param(cmd_buffer, instanced_draw, indirect_draw, count_from_stream_output,
                                                    draw_vertex_count, topology, prim_restart_enable,
                                                    patch_control_points, state->tess_num_patches);

   if (state->last_ia_multi_vgt_param != ia_multi_vgt_param) {
      if (gpu_info->gfx_level == GFX9) {
         radeon_set_uconfig_reg_idx(&pdev->info, cs, R_030960_IA_MULTI_VGT_PARAM, 4, ia_multi_vgt_param);
      } else if (gpu_info->gfx_level >= GFX7) {
         radeon_set_context_reg_idx(cs, R_028AA8_IA_MULTI_VGT_PARAM, 1, ia_multi_vgt_param);
      } else {
         radeon_set_context_reg(cs, R_028AA8_IA_MULTI_VGT_PARAM, ia_multi_vgt_param);
      }
      state->last_ia_multi_vgt_param = ia_multi_vgt_param;
   }
}

static void
gfx10_emit_ge_cntl(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   struct radv_cmd_state *state = &cmd_buffer->state;
   bool break_wave_at_eoi = false;
   unsigned primgroup_size;
   unsigned ge_cntl;

   if (last_vgt_shader->info.is_ngg)
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TESS_CTRL)) {
      const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);

      primgroup_size = state->tess_num_patches;

      if (cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL]->info.uses_prim_id || tes->info.uses_prim_id ||
          (tes->info.merged_shader_compiled_separately &&
           cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.uses_prim_id)) {
         break_wave_at_eoi = true;
      }
   } else if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY)) {
      const struct radv_legacy_gs_info *gs_state = &cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.gs_ring_info;
      primgroup_size = gs_state->gs_prims_per_subgroup;
   } else {
      primgroup_size = 128; /* recommended without a GS and tess */
   }

   ge_cntl = S_03096C_PRIM_GRP_SIZE_GFX10(primgroup_size) | S_03096C_VERT_GRP_SIZE(256) | /* disable vertex grouping */
             S_03096C_PACKET_TO_ONE_PA(0) /* this should only be set if LINE_STIPPLE_TEX_ENA == 1 */ |
             S_03096C_BREAK_WAVE_AT_EOI(break_wave_at_eoi);

   if (state->last_ge_cntl != ge_cntl) {
      radeon_set_uconfig_reg(cmd_buffer->cs, R_03096C_GE_CNTL, ge_cntl);
      state->last_ge_cntl = ge_cntl;
   }
}

static void
radv_emit_draw_registers(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *draw_info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t topology = state->dynamic.vk.ia.primitive_topology;
   bool disable_instance_packing = false;

   /* Draw state. */
   if (gpu_info->gfx_level >= GFX10) {
      gfx10_emit_ge_cntl(cmd_buffer);
   } else {
      radv_emit_ia_multi_vgt_param(cmd_buffer, draw_info->instance_count > 1, draw_info->indirect,
                                   !!draw_info->strmout_buffer, draw_info->indirect ? 0 : draw_info->count);
   }

   /* RDNA2 is affected by a hardware bug when instance packing is enabled for adjacent primitive
    * topologies and instance_count > 1, pipeline stats generated by GE are incorrect. It needs to
    * be applied for indexed and non-indexed draws.
    */
   if (gpu_info->gfx_level == GFX10_3 && state->active_pipeline_queries > 0 &&
       (draw_info->instance_count > 1 || draw_info->indirect) &&
       (topology == V_008958_DI_PT_LINELIST_ADJ || topology == V_008958_DI_PT_LINESTRIP_ADJ ||
        topology == V_008958_DI_PT_TRILIST_ADJ || topology == V_008958_DI_PT_TRISTRIP_ADJ)) {
      disable_instance_packing = true;
   }

   if ((draw_info->indexed && state->index_type != state->last_index_type) ||
       (gpu_info->gfx_level == GFX10_3 &&
        (state->last_index_type == -1 ||
         disable_instance_packing != G_028A7C_DISABLE_INSTANCE_PACKING(state->last_index_type)))) {
      uint32_t index_type = state->index_type | S_028A7C_DISABLE_INSTANCE_PACKING(disable_instance_packing);

      if (pdev->info.gfx_level >= GFX9) {
         radeon_set_uconfig_reg_idx(&pdev->info, cs, R_03090C_VGT_INDEX_TYPE, 2, index_type);
      } else {
         radeon_emit(cs, PKT3(PKT3_INDEX_TYPE, 0, 0));
         radeon_emit(cs, index_type);
      }

      state->last_index_type = index_type;
   }
}

static void
radv_stage_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask)
{
   /* For simplicity, if the barrier wants to wait for the task shader,
    * just make it wait for the mesh shader too.
    */
   if (src_stage_mask & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
      src_stage_mask |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;

   if (src_stage_mask & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      /* Be conservative for now. */
      src_stage_mask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
   }

   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_NV | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;
   }

   if (src_stage_mask & (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;
   } else if (src_stage_mask &
              (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
               VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
               VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT |
               VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
   }
}

static bool
can_skip_buffer_l2_flushes(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return pdev->info.gfx_level == GFX9 || (pdev->info.gfx_level >= GFX10 && !pdev->info.tcc_rb_non_coherent);
}

/*
 * In vulkan barriers have two kinds of operations:
 *
 * - visibility (implemented with radv_src_access_flush)
 * - availability (implemented with radv_dst_access_flush)
 *
 * for a memory operation to observe the result of a previous memory operation
 * one needs to do a visibility operation from the source memory and then an
 * availability operation to the target memory.
 *
 * The complication is the availability and visibility operations do not need to
 * be in the same barrier.
 *
 * The cleanest way to implement this is to define the visibility operation to
 * bring the caches to a "state of rest", which none of the caches below that
 * level dirty.
 *
 * For GFX8 and earlier this would be VRAM/GTT with none of the caches dirty.
 *
 * For GFX9+ we can define the state at rest to be L2 instead of VRAM for all
 * buffers and for images marked as coherent, and VRAM/GTT for non-coherent
 * images. However, given the existence of memory barriers which do not specify
 * the image/buffer it often devolves to just VRAM/GTT anyway.
 *
 * To help reducing the invalidations for GPUs that have L2 coherency between the
 * RB and the shader caches, we always invalidate L2 on the src side, as we can
 * use our knowledge of past usage to optimize flushes away.
 */

enum radv_cmd_flush_bits
radv_src_access_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_flags,
                      const struct radv_image *image)
{
   src_flags = vk_expand_src_access_flags2(src_stages, src_flags);

   bool has_CB_meta = true, has_DB_meta = true;
   bool image_is_coherent = image ? image->l2_coherent : false;
   enum radv_cmd_flush_bits flush_bits = 0;

   if (image) {
      if (!radv_image_has_CB_metadata(image))
         has_CB_meta = false;
      if (!radv_image_has_htile(image))
         has_DB_meta = false;
   }

   if (src_flags & VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_NV)
      flush_bits |= RADV_CMD_FLAG_INV_L2;

   if (src_flags & (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)) {
      /* since the STORAGE bit isn't set we know that this is a meta operation.
       * on the dst flush side we skip CB/DB flushes without the STORAGE bit, so
       * set it here. */
      if (image && !(image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
         if (vk_format_is_depth_or_stencil(image->vk.format)) {
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
         } else {
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
         }
      }

      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (src_flags &
       (VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT)) {
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_WB_L2;
   }

   if (src_flags & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) {
      flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
      if (has_CB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   if (src_flags & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
      flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
      if (has_DB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   if (src_flags & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
      flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB;

      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
      if (has_CB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
      if (has_DB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   return flush_bits;
}

enum radv_cmd_flush_bits
radv_dst_access_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_flags,
                      const struct radv_image *image)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool has_CB_meta = true, has_DB_meta = true;
   enum radv_cmd_flush_bits flush_bits = 0;
   bool flush_CB = true, flush_DB = true;
   bool image_is_coherent = image ? image->l2_coherent : false;
   bool flush_L2_metadata = false;

   dst_flags = vk_expand_dst_access_flags2(dst_stages, dst_flags);

   if (image) {
      if (!(image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
         flush_CB = false;
         flush_DB = false;
      }

      if (!radv_image_has_CB_metadata(image))
         has_CB_meta = false;
      if (!radv_image_has_htile(image))
         has_DB_meta = false;
   }

   flush_L2_metadata = (has_CB_meta || has_DB_meta) && pdev->info.gfx_level < GFX12;

   /* All the L2 invalidations below are not the CB/DB. So if there are no incoherent images
    * in the L2 cache in CB/DB mode then they are already usable from all the other L2 clients. */
   image_is_coherent |= can_skip_buffer_l2_flushes(device) && !cmd_buffer->state.rb_noncoherent_dirty;

   if (dst_flags & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT) {
      /* SMEM loads are used to read compute dispatch size in shaders */
      if (!device->load_grid_size_from_user_sgpr)
         flush_bits |= RADV_CMD_FLAG_INV_SCACHE;

      /* Ensure the DGC meta shader can read the commands. */
      if (radv_uses_device_generated_commands(device)) {
         flush_bits |= RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_INV_VCACHE;

         if (pdev->info.gfx_level < GFX9)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
      }
   }

   if (dst_flags & VK_ACCESS_2_UNIFORM_READ_BIT)
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_SCACHE;

   if (dst_flags & (VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_TRANSFER_READ_BIT)) {
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;

      if (flush_L2_metadata)
         flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (dst_flags & VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT)
      flush_bits |= RADV_CMD_FLAG_INV_SCACHE;

   if (dst_flags & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)) {
      if (dst_flags & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR |
                       VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR)) {
         /* Unlike LLVM, ACO uses SMEM for SSBOs and we have to
          * invalidate the scalar cache. */
         if (!pdev->use_llvm && !image)
            flush_bits |= RADV_CMD_FLAG_INV_SCACHE;
      }

      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
      if (flush_L2_metadata)
         flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (dst_flags & VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_NV) {
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
      if (pdev->info.gfx_level < GFX9)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (dst_flags & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) {
      if (flush_CB)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
      if (has_CB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   if (dst_flags & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) {
      if (flush_DB)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
      if (has_DB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   return flush_bits;
}

void
radv_emit_resolve_barrier(struct radv_cmd_buffer *cmd_buffer, const struct radv_resolve_barrier *barrier)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      cmd_buffer->state.flush_bits |=
         radv_src_access_flush(cmd_buffer, barrier->src_stage_mask, barrier->src_access_mask, iview->image);
   }
   if (render->ds_att.iview) {
      cmd_buffer->state.flush_bits |= radv_src_access_flush(cmd_buffer, barrier->src_stage_mask,
                                                            barrier->src_access_mask, render->ds_att.iview->image);
   }

   radv_stage_flush(cmd_buffer, barrier->src_stage_mask);

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      cmd_buffer->state.flush_bits |=
         radv_dst_access_flush(cmd_buffer, barrier->dst_stage_mask, barrier->dst_access_mask, iview->image);
   }
   if (render->ds_att.iview) {
      cmd_buffer->state.flush_bits |= radv_dst_access_flush(cmd_buffer, barrier->dst_stage_mask,
                                                            barrier->dst_access_mask, render->ds_att.iview->image);
   }

   radv_gang_barrier(cmd_buffer, barrier->src_stage_mask, barrier->dst_stage_mask);
}

static void
radv_handle_image_transition_separate(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                      VkImageLayout src_layout, VkImageLayout dst_layout,
                                      VkImageLayout src_stencil_layout, VkImageLayout dst_stencil_layout,
                                      uint32_t src_family_index, uint32_t dst_family_index,
                                      const VkImageSubresourceRange *range,
                                      struct radv_sample_locations_state *sample_locs)
{
   /* If we have a stencil layout that's different from depth, we need to
    * perform the stencil transition separately.
    */
   if ((range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       (src_layout != src_stencil_layout || dst_layout != dst_stencil_layout)) {
      VkImageSubresourceRange aspect_range = *range;
      /* Depth-only transitions. */
      if (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         aspect_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
         radv_handle_image_transition(cmd_buffer, image, src_layout, dst_layout, src_family_index, dst_family_index,
                                      &aspect_range, sample_locs);
      }

      /* Stencil-only transitions. */
      aspect_range.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      radv_handle_image_transition(cmd_buffer, image, src_stencil_layout, dst_stencil_layout, src_family_index,
                                   dst_family_index, &aspect_range, sample_locs);
   } else {
      radv_handle_image_transition(cmd_buffer, image, src_layout, dst_layout, src_family_index, dst_family_index, range,
                                   sample_locs);
   }
}

static void
radv_handle_rendering_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *view,
                                       uint32_t layer_count, uint32_t view_mask, VkImageLayout initial_layout,
                                       VkImageLayout initial_stencil_layout, VkImageLayout final_layout,
                                       VkImageLayout final_stencil_layout,
                                       struct radv_sample_locations_state *sample_locs)
{
   VkImageSubresourceRange range;
   range.aspectMask = view->image->vk.aspects;
   range.baseMipLevel = view->vk.base_mip_level;
   range.levelCount = 1;

   if (view_mask) {
      while (view_mask) {
         int start, count;
         u_bit_scan_consecutive_range(&view_mask, &start, &count);

         range.baseArrayLayer = view->vk.base_array_layer + start;
         range.layerCount = count;

         radv_handle_image_transition_separate(cmd_buffer, view->image, initial_layout, final_layout,
                                               initial_stencil_layout, final_stencil_layout, 0, 0, &range, sample_locs);
      }
   } else {
      range.baseArrayLayer = view->vk.base_array_layer;
      range.layerCount = layer_count;
      radv_handle_image_transition_separate(cmd_buffer, view->image, initial_layout, final_layout,
                                            initial_stencil_layout, final_stencil_layout, 0, 0, &range, sample_locs);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkResult result = VK_SUCCESS;

   vk_command_buffer_begin(&cmd_buffer->vk, pBeginInfo);

   if (cmd_buffer->qf == RADV_QUEUE_SPARSE)
      return result;

   memset(&cmd_buffer->state, 0, sizeof(cmd_buffer->state));
   cmd_buffer->state.last_index_type = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_vertex_offset_valid = false;
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_subpass_color_count = MAX_RTS;
   cmd_buffer->state.predication_type = -1;
   cmd_buffer->state.mesh_shading = false;
   cmd_buffer->state.last_vrs_rates = -1;
   cmd_buffer->state.last_vrs_rates_sgpr_idx = -1;

   radv_reset_tracked_regs(cmd_buffer);

   cmd_buffer->usage_flags = pBeginInfo->flags;

   cmd_buffer->state.dirty |=
      RADV_CMD_DIRTY_GUARDBAND | RADV_CMD_DIRTY_OCCLUSION_QUERY | RADV_CMD_DIRTY_DB_SHADER_CONTROL;
   cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_ALL;

   if (cmd_buffer->qf == RADV_QUEUE_GENERAL)
      vk_dynamic_graphics_state_init(&cmd_buffer->state.dynamic.vk);

   if (cmd_buffer->qf == RADV_QUEUE_COMPUTE || device->vk.enabled_features.taskShader) {
      uint32_t pred_value = 0;
      uint32_t pred_offset;
      if (!radv_cmd_buffer_upload_data(cmd_buffer, 4, &pred_value, &pred_offset))
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

      cmd_buffer->state.mec_inv_pred_emitted = false;
      cmd_buffer->state.mec_inv_pred_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + pred_offset;
   }

   if (pdev->info.gfx_level >= GFX9 && cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      unsigned num_db = pdev->info.max_render_backends;
      unsigned fence_offset, eop_bug_offset;
      void *fence_ptr;

      radv_cmd_buffer_upload_alloc(cmd_buffer, 8, &fence_offset, &fence_ptr);
      memset(fence_ptr, 0, 8);

      cmd_buffer->gfx9_fence_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      cmd_buffer->gfx9_fence_va += fence_offset;

      radv_emit_clear_data(cmd_buffer, V_370_PFP, cmd_buffer->gfx9_fence_va, 8);

      if (pdev->info.gfx_level == GFX9) {
         /* Allocate a buffer for the EOP bug on GFX9. */
         radv_cmd_buffer_upload_alloc(cmd_buffer, 16 * num_db, &eop_bug_offset, &fence_ptr);
         memset(fence_ptr, 0, 16 * num_db);
         cmd_buffer->gfx9_eop_bug_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
         cmd_buffer->gfx9_eop_bug_va += eop_bug_offset;

         radv_emit_clear_data(cmd_buffer, V_370_PFP, cmd_buffer->gfx9_eop_bug_va, 16 * num_db);
      }
   }

   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
       (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {

      char gcbiar_data[VK_GCBIARR_DATA_SIZE(MAX_RTS)];
      const VkRenderingInfo *resume_info =
         vk_get_command_buffer_inheritance_as_rendering_resume(cmd_buffer->vk.level, pBeginInfo, gcbiar_data);
      if (resume_info) {
         radv_CmdBeginRendering(commandBuffer, resume_info);
      } else {
         const VkCommandBufferInheritanceRenderingInfo *inheritance_info =
            vk_get_command_buffer_inheritance_rendering_info(cmd_buffer->vk.level, pBeginInfo);

         radv_cmd_buffer_reset_rendering(cmd_buffer);
         struct radv_rendering_state *render = &cmd_buffer->state.render;
         render->active = true;
         render->view_mask = inheritance_info->viewMask;
         render->max_samples = inheritance_info->rasterizationSamples;
         render->color_att_count = inheritance_info->colorAttachmentCount;
         for (uint32_t i = 0; i < render->color_att_count; i++) {
            render->color_att[i] = (struct radv_attachment){
               .format = inheritance_info->pColorAttachmentFormats[i],
            };
         }
         assert(inheritance_info->depthAttachmentFormat == VK_FORMAT_UNDEFINED ||
                inheritance_info->stencilAttachmentFormat == VK_FORMAT_UNDEFINED ||
                inheritance_info->depthAttachmentFormat == inheritance_info->stencilAttachmentFormat);
         render->ds_att = (struct radv_attachment){.iview = NULL};
         if (inheritance_info->depthAttachmentFormat != VK_FORMAT_UNDEFINED)
            render->ds_att.format = inheritance_info->depthAttachmentFormat;
         if (inheritance_info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED)
            render->ds_att.format = inheritance_info->stencilAttachmentFormat;

         if (vk_format_has_depth(render->ds_att.format))
            render->ds_att_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
         if (vk_format_has_stencil(render->ds_att.format))
            render->ds_att_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      }

      cmd_buffer->state.inherited_pipeline_statistics = pBeginInfo->pInheritanceInfo->pipelineStatistics;

      if (cmd_buffer->state.inherited_pipeline_statistics &
          (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
           VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT))
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;

      cmd_buffer->state.inherited_occlusion_queries = pBeginInfo->pInheritanceInfo->occlusionQueryEnable;
      cmd_buffer->state.inherited_query_control_flags = pBeginInfo->pInheritanceInfo->queryFlags;
      if (cmd_buffer->state.inherited_occlusion_queries)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);

   radv_describe_begin_cmd_buffer(cmd_buffer);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                           const VkBuffer *pBuffers, const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes,
                           const VkDeviceSize *pStrides)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_vertex_binding *vb = cmd_buffer->vertex_bindings;
   const struct radv_vs_input_state *state = &cmd_buffer->state.dynamic_vs_input;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline. */

   assert(firstBinding + bindingCount <= MAX_VBS);

   if (firstBinding + bindingCount > cmd_buffer->used_vertex_bindings)
      cmd_buffer->used_vertex_bindings = firstBinding + bindingCount;

   uint32_t misaligned_mask_invalid = 0;

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(radv_buffer, buffer, pBuffers[i]);
      uint32_t idx = firstBinding + i;
      VkDeviceSize size = pSizes ? pSizes[i] : 0;
      /* if pStrides=NULL, it shouldn't overwrite the strides specified by CmdSetVertexInputEXT */
      VkDeviceSize stride = pStrides ? pStrides[i] : vb[idx].stride;

      if (!!cmd_buffer->vertex_binding_buffers[idx] != !!buffer ||
          (buffer && ((vb[idx].offset & 0x3) != (pOffsets[i] & 0x3) || (vb[idx].stride & 0x3) != (stride & 0x3)))) {
         misaligned_mask_invalid |= state->bindings_match_attrib ? BITFIELD_BIT(idx) : 0xffffffff;
      }

      cmd_buffer->vertex_binding_buffers[idx] = buffer;
      vb[idx].offset = pOffsets[i];
      vb[idx].size = buffer ? vk_buffer_range(&buffer->vk, pOffsets[i], size) : size;
      vb[idx].stride = stride;

      uint32_t bit = BITFIELD_BIT(idx);
      if (buffer) {
         radv_cs_add_buffer(device->ws, cmd_buffer->cs, cmd_buffer->vertex_binding_buffers[idx]->bo);
         cmd_buffer->state.vbo_bound_mask |= bit;
      } else {
         cmd_buffer->state.vbo_bound_mask &= ~bit;
      }
   }

   if (misaligned_mask_invalid) {
      cmd_buffer->state.vbo_misaligned_mask_invalid = misaligned_mask_invalid;
      cmd_buffer->state.vbo_misaligned_mask &= ~misaligned_mask_invalid;
      cmd_buffer->state.vbo_unaligned_mask &= ~misaligned_mask_invalid;
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
   cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT;
}

static uint32_t
vk_to_index_type(VkIndexType type)
{
   switch (type) {
   case VK_INDEX_TYPE_UINT8_KHR:
      return V_028A7C_VGT_INDEX_8;
   case VK_INDEX_TYPE_UINT16:
      return V_028A7C_VGT_INDEX_16;
   case VK_INDEX_TYPE_UINT32:
      return V_028A7C_VGT_INDEX_32;
   default:
      unreachable("invalid index type");
   }
}

static uint32_t
radv_get_vgt_index_size(uint32_t type)
{
   uint32_t index_type = G_028A7C_INDEX_TYPE(type);
   switch (index_type) {
   case V_028A7C_VGT_INDEX_8:
      return 1;
   case V_028A7C_VGT_INDEX_16:
      return 2;
   case V_028A7C_VGT_INDEX_32:
      return 4;
   default:
      unreachable("invalid index type");
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindIndexBuffer2KHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                            VkIndexType indexType)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, index_buffer, buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   cmd_buffer->state.index_type = vk_to_index_type(indexType);

   if (index_buffer) {
      cmd_buffer->state.index_va = radv_buffer_get_va(index_buffer->bo);
      cmd_buffer->state.index_va += index_buffer->offset + offset;

      int index_size = radv_get_vgt_index_size(vk_to_index_type(indexType));
      cmd_buffer->state.max_index_count = (vk_buffer_range(&index_buffer->vk, offset, size)) / index_size;
      radv_cs_add_buffer(device->ws, cmd_buffer->cs, index_buffer->bo);
   } else {
      cmd_buffer->state.index_va = 0;
      cmd_buffer->state.max_index_count = 0;

      if (pdev->info.has_null_index_buffer_clamping_bug)
         cmd_buffer->state.index_va = 0x2;
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;

   /* Primitive restart state depends on the index type. */
   if (cmd_buffer->state.dynamic.vk.ia.primitive_restart_enable)
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE;
}

static void
radv_bind_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                         struct radv_descriptor_set *set, unsigned idx)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_winsys *ws = device->ws;

   radv_set_descriptor_set(cmd_buffer, bind_point, set, idx);

   assert(set);
   assert(!(set->header.layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR));

   if (!device->use_global_bo_list) {
      for (unsigned j = 0; j < set->header.buffer_count; ++j)
         if (set->descriptors[j])
            radv_cs_add_buffer(ws, cmd_buffer->cs, set->descriptors[j]);
   }

   if (set->header.bo)
      radv_cs_add_buffer(ws, cmd_buffer->cs, set->header.bo);
}

static void
radv_bind_descriptor_sets(struct radv_cmd_buffer *cmd_buffer,
                          const VkBindDescriptorSetsInfoKHR *pBindDescriptorSetsInfo, VkPipelineBindPoint bind_point)
{
   VK_FROM_HANDLE(radv_pipeline_layout, layout, pBindDescriptorSetsInfo->layout);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   const bool no_dynamic_bounds = instance->debug_flags & RADV_DEBUG_NO_DYNAMIC_BOUNDS;
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   unsigned dyn_idx = 0;

   for (unsigned i = 0; i < pBindDescriptorSetsInfo->descriptorSetCount; ++i) {
      unsigned set_idx = i + pBindDescriptorSetsInfo->firstSet;
      VK_FROM_HANDLE(radv_descriptor_set, set, pBindDescriptorSetsInfo->pDescriptorSets[i]);

      if (!set)
         continue;

      /* If the set is already bound we only need to update the
       * (potentially changed) dynamic offsets. */
      if (descriptors_state->sets[set_idx] != set || !(descriptors_state->valid & (1u << set_idx))) {
         radv_bind_descriptor_set(cmd_buffer, bind_point, set, set_idx);
      }

      for (unsigned j = 0; j < set->header.layout->dynamic_offset_count; ++j, ++dyn_idx) {
         unsigned idx = j + layout->set[i + pBindDescriptorSetsInfo->firstSet].dynamic_offset_start;
         uint32_t *dst = descriptors_state->dynamic_buffers + idx * 4;
         assert(dyn_idx < pBindDescriptorSetsInfo->dynamicOffsetCount);

         struct radv_descriptor_range *range = set->header.dynamic_descriptors + j;

         if (!range->va) {
            memset(dst, 0, 4 * 4);
         } else {
            uint64_t va = range->va + pBindDescriptorSetsInfo->pDynamicOffsets[dyn_idx];
            const uint32_t size = no_dynamic_bounds ? 0xffffffffu : range->size;

            ac_build_raw_buffer_descriptor(pdev->info.gfx_level, va, size, dst);
         }

         cmd_buffer->push_constant_stages |= set->header.layout->dynamic_shader_stages;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorSets2KHR(VkCommandBuffer commandBuffer,
                               const VkBindDescriptorSetsInfoKHR *pBindDescriptorSetsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_bind_descriptor_sets(cmd_buffer, pBindDescriptorSetsInfo, VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pBindDescriptorSetsInfo->stageFlags & RADV_GRAPHICS_STAGE_BITS) {
      radv_bind_descriptor_sets(cmd_buffer, pBindDescriptorSetsInfo, VK_PIPELINE_BIND_POINT_GRAPHICS);
   }

   if (pBindDescriptorSetsInfo->stageFlags & RADV_RT_STAGE_BITS) {
      radv_bind_descriptor_sets(cmd_buffer, pBindDescriptorSetsInfo, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

static bool
radv_init_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, struct radv_descriptor_set *set,
                              struct radv_descriptor_set_layout *layout, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   set->header.size = layout->size;

   if (set->header.layout != layout) {
      if (set->header.layout)
         vk_descriptor_set_layout_unref(&device->vk, &set->header.layout->vk);
      vk_descriptor_set_layout_ref(&layout->vk);
      set->header.layout = layout;
   }

   if (descriptors_state->push_set.capacity < set->header.size) {
      size_t new_size = MAX2(set->header.size, 1024);
      new_size = MAX2(new_size, 2 * descriptors_state->push_set.capacity);
      new_size = MIN2(new_size, 96 * MAX_PUSH_DESCRIPTORS);

      free(set->header.mapped_ptr);
      set->header.mapped_ptr = malloc(new_size);

      if (!set->header.mapped_ptr) {
         descriptors_state->push_set.capacity = 0;
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return false;
      }

      descriptors_state->push_set.capacity = new_size;
   }

   return true;
}

void
radv_meta_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint pipelineBindPoint,
                              VkPipelineLayout _layout, uint32_t set, uint32_t descriptorWriteCount,
                              const VkWriteDescriptorSet *pDescriptorWrites)
{
   VK_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
   struct radv_descriptor_set *push_set = (struct radv_descriptor_set *)&cmd_buffer->meta_push_descriptors;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   unsigned bo_offset;

   assert(set == 0);
   assert(layout->set[set].layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

   push_set->header.size = layout->set[set].layout->size;
   push_set->header.layout = layout->set[set].layout;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, push_set->header.size, &bo_offset,
                                     (void **)&push_set->header.mapped_ptr))
      return;

   push_set->header.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   push_set->header.va += bo_offset;

   radv_cmd_update_descriptor_sets(device, cmd_buffer, radv_descriptor_set_to_handle(push_set), descriptorWriteCount,
                                   pDescriptorWrites, 0, NULL);

   radv_set_descriptor_set(cmd_buffer, pipelineBindPoint, push_set, set);
}

static void
radv_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo,
                         VkPipelineBindPoint bind_point)
{
   VK_FROM_HANDLE(radv_pipeline_layout, layout, pPushDescriptorSetInfo->layout);
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_descriptor_set *push_set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(layout->set[pPushDescriptorSetInfo->set].layout->flags &
          VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

   if (!radv_init_push_descriptor_set(cmd_buffer, push_set, layout->set[pPushDescriptorSetInfo->set].layout,
                                      bind_point))
      return;

   /* Check that there are no inline uniform block updates when calling vkCmdPushDescriptorSetKHR()
    * because it is invalid, according to Vulkan spec.
    */
   for (int i = 0; i < pPushDescriptorSetInfo->descriptorWriteCount; i++) {
      ASSERTED const VkWriteDescriptorSet *writeset = &pPushDescriptorSetInfo->pDescriptorWrites[i];
      assert(writeset->descriptorType != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
   }

   radv_cmd_update_descriptor_sets(device, cmd_buffer, radv_descriptor_set_to_handle(push_set),
                                   pPushDescriptorSetInfo->descriptorWriteCount,
                                   pPushDescriptorSetInfo->pDescriptorWrites, 0, NULL);

   radv_set_descriptor_set(cmd_buffer, bind_point, push_set, pPushDescriptorSetInfo->set);

   radv_flush_push_descriptors(cmd_buffer, descriptors_state);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   if (pPushDescriptorSetInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_push_descriptor_set(cmd_buffer, pPushDescriptorSetInfo, VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pPushDescriptorSetInfo->stageFlags & RADV_GRAPHICS_STAGE_BITS) {
      radv_push_descriptor_set(cmd_buffer, pPushDescriptorSetInfo, VK_PIPELINE_BIND_POINT_GRAPHICS);
   }

   if (pPushDescriptorSetInfo->stageFlags & RADV_RT_STAGE_BITS) {
      radv_push_descriptor_set(cmd_buffer, pPushDescriptorSetInfo, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushDescriptorSetWithTemplate2KHR(
   VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfoKHR *pPushDescriptorSetWithTemplateInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_pipeline_layout, layout, pPushDescriptorSetWithTemplateInfo->layout);
   VK_FROM_HANDLE(radv_descriptor_update_template, templ, pPushDescriptorSetWithTemplateInfo->descriptorUpdateTemplate);
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, templ->bind_point);
   struct radv_descriptor_set *push_set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(layout->set[pPushDescriptorSetWithTemplateInfo->set].layout->flags &
          VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

   if (!radv_init_push_descriptor_set(cmd_buffer, push_set, layout->set[pPushDescriptorSetWithTemplateInfo->set].layout,
                                      templ->bind_point))
      return;

   radv_cmd_update_descriptor_set_with_template(device, cmd_buffer, push_set,
                                                pPushDescriptorSetWithTemplateInfo->descriptorUpdateTemplate,
                                                pPushDescriptorSetWithTemplateInfo->pData);

   radv_set_descriptor_set(cmd_buffer, templ->bind_point, push_set, pPushDescriptorSetWithTemplateInfo->set);

   radv_flush_push_descriptors(cmd_buffer, descriptors_state);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushConstants2KHR(VkCommandBuffer commandBuffer, const VkPushConstantsInfoKHR *pPushConstantsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   memcpy(cmd_buffer->push_constants + pPushConstantsInfo->offset, pPushConstantsInfo->pValues,
          pPushConstantsInfo->size);
   cmd_buffer->push_constant_stages |= pPushConstantsInfo->stageFlags;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (cmd_buffer->qf == RADV_QUEUE_SPARSE)
      return vk_command_buffer_end(&cmd_buffer->vk);

   radv_emit_mip_change_flush_default(cmd_buffer);

   const bool is_gfx_or_ace = cmd_buffer->qf == RADV_QUEUE_GENERAL || cmd_buffer->qf == RADV_QUEUE_COMPUTE;

   if (is_gfx_or_ace) {
      if (pdev->info.gfx_level == GFX6)
         cmd_buffer->state.flush_bits |=
            RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_WB_L2;

      /* Make sure to sync all pending active queries at the end of
       * command buffer.
       */
      cmd_buffer->state.flush_bits |= cmd_buffer->active_query_flush_bits;

      /* Flush noncoherent images on GFX9+ so we can assume they're clean on the start of a
       * command buffer.
       */
      if (cmd_buffer->state.rb_noncoherent_dirty && !can_skip_buffer_l2_flushes(device))
         cmd_buffer->state.flush_bits |= radv_src_access_flush(
            cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, NULL);

      /* Since NGG streamout uses GDS, we need to make GDS idle when
       * we leave the IB, otherwise another process might overwrite
       * it while our shaders are busy.
       */
      if (cmd_buffer->gds_needed)
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;
   }

   /* Finalize the internal compute command stream, if it exists. */
   if (cmd_buffer->gang.cs) {
      VkResult result = radv_gang_finalize(cmd_buffer);
      if (result != VK_SUCCESS)
         return vk_error(cmd_buffer, result);
   }

   if (is_gfx_or_ace) {
      radv_emit_cache_flush(cmd_buffer);

      /* Make sure CP DMA is idle at the end of IBs because the kernel
       * doesn't wait for it.
       */
      radv_cp_dma_wait_for_idle(cmd_buffer);
   }

   radv_describe_end_cmd_buffer(cmd_buffer);

   VkResult result = device->ws->cs_finalize(cmd_buffer->cs);
   if (result != VK_SUCCESS)
      return vk_error(cmd_buffer, result);

   return vk_command_buffer_end(&cmd_buffer->vk);
}

static void
radv_emit_compute_pipeline(struct radv_cmd_buffer *cmd_buffer, struct radv_compute_pipeline *pipeline)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pipeline == cmd_buffer->state.emitted_compute_pipeline)
      return;

   radeon_check_space(device->ws, cmd_buffer->cs, pdev->info.gfx_level >= GFX10 ? 19 : 16);

   if (pipeline->base.type == RADV_PIPELINE_COMPUTE) {
      radv_emit_compute_shader(pdev, cmd_buffer->cs, cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]);
   } else {
      radv_emit_compute_shader(pdev, cmd_buffer->cs, cmd_buffer->state.rt_prolog);
   }

   cmd_buffer->state.emitted_compute_pipeline = pipeline;

   if (radv_device_fault_detection_enabled(device))
      radv_save_pipeline(cmd_buffer, &pipeline->base);
}

static void
radv_mark_descriptor_sets_dirty(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->dirty |= descriptors_state->valid;
}

static void
radv_bind_vs_input_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_graphics_pipeline *pipeline)
{
   const struct radv_shader *vs_shader = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   const struct radv_vs_input_state *src = &pipeline->vs_input_state;

   /* Bind the vertex input state from the pipeline when the VS has a prolog and the state isn't
    * dynamic. This can happen when the pre-rasterization stages and the vertex input state are from
    * two different libraries. Otherwise, if the VS has a prolog, the state is dynamic and there is
    * nothing to bind.
    */
   if (!vs_shader || !vs_shader->info.vs.has_prolog || (pipeline->dynamic_states & RADV_DYNAMIC_VERTEX_INPUT))
      return;

   cmd_buffer->state.dynamic_vs_input = *src;

   cmd_buffer->state.vbo_misaligned_mask = 0;
   cmd_buffer->state.vbo_unaligned_mask = 0;
   cmd_buffer->state.vbo_misaligned_mask_invalid = src->attribute_mask;

   cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT;
}

static void
radv_bind_multisample_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_multisample_state *ms)
{
   if (ms->sample_shading_enable) {
      cmd_buffer->state.ms.sample_shading_enable = true;
      cmd_buffer->state.ms.min_sample_shading = ms->min_sample_shading;
   }
}

static void
radv_bind_custom_blend_mode(struct radv_cmd_buffer *cmd_buffer, unsigned custom_blend_mode)
{
   /* Re-emit CB_COLOR_CONTROL when the custom blending mode changes. */
   if (cmd_buffer->state.custom_blend_mode != custom_blend_mode)
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_LOGIC_OP | RADV_DYNAMIC_LOGIC_OP_ENABLE;

   cmd_buffer->state.custom_blend_mode = custom_blend_mode;
}

static void
radv_bind_pre_rast_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool mesh_shading = shader->info.stage == MESA_SHADER_MESH;
   const struct radv_userdata_info *loc;

   assert(shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_TESS_CTRL ||
          shader->info.stage == MESA_SHADER_TESS_EVAL || shader->info.stage == MESA_SHADER_GEOMETRY ||
          shader->info.stage == MESA_SHADER_MESH);

   if (radv_get_user_sgpr_info(shader, AC_UD_NGG_PROVOKING_VTX)->sgpr_idx != -1) {
      /* Re-emit the provoking vertex mode state because the SGPR idx can be different. */
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_PROVOKING_VERTEX_MODE;
   }

   if (radv_get_user_sgpr_info(shader, AC_UD_STREAMOUT_BUFFERS)->sgpr_idx != -1) {
      /* Re-emit the streamout buffers because the SGPR idx can be different and with NGG streamout
       * they always need to be emitted because a buffer size of 0 is used to disable streamout.
       */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;

      if (pdev->use_ngg_streamout && pdev->info.gfx_level < GFX12) {
         /* GFX11 needs GDS OA for streamout. */
         cmd_buffer->gds_oa_needed = true;
      }
   }

   if (radv_get_user_sgpr_info(shader, AC_UD_NUM_VERTS_PER_PRIM)->sgpr_idx != -1) {
      /* Re-emit the primitive topology because the SGPR idx can be different. */
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_TOPOLOGY;
   }

   if (radv_get_user_sgpr_info(shader, AC_UD_SHADER_QUERY_STATE)->sgpr_idx != -1) {
      /* Re-emit shader query state when SGPR exists but location potentially changed. */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   const bool needs_vtx_sgpr =
      shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH ||
      (shader->info.stage == MESA_SHADER_GEOMETRY && !shader->info.merged_shader_compiled_separately) ||
      (shader->info.stage == MESA_SHADER_TESS_CTRL && !shader->info.merged_shader_compiled_separately);

   loc = radv_get_user_sgpr_info(shader, AC_UD_VS_BASE_VERTEX_START_INSTANCE);
   if (needs_vtx_sgpr && loc->sgpr_idx != -1) {
      cmd_buffer->state.vtx_base_sgpr = shader->info.user_data_0 + loc->sgpr_idx * 4;
      cmd_buffer->state.vtx_emit_num = loc->num_sgprs;
      cmd_buffer->state.uses_drawid = shader->info.vs.needs_draw_id;
      cmd_buffer->state.uses_baseinstance = shader->info.vs.needs_base_instance;

      if (shader->info.merged_shader_compiled_separately) {
         /* Merged shaders compiled separately (eg. VS+TCS) always declare these user SGPRS
          * because the input arguments must match.
          */
         cmd_buffer->state.uses_drawid = true;
         cmd_buffer->state.uses_baseinstance = true;
      }

      /* Re-emit some vertex states because the SGPR idx can be different. */
      cmd_buffer->state.last_first_instance = -1;
      cmd_buffer->state.last_vertex_offset_valid = false;
      cmd_buffer->state.last_drawid = -1;
   }

   if (mesh_shading != cmd_buffer->state.mesh_shading) {
      /* Re-emit VRS state because the combiner is different (vertex vs primitive). Re-emit
       * primitive topology because the mesh shading pipeline clobbered it.
       */
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_FRAGMENT_SHADING_RATE | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY;
   }

   cmd_buffer->state.mesh_shading = mesh_shading;
}

static void
radv_bind_vertex_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs)
{
   radv_bind_pre_rast_shader(cmd_buffer, vs);

   /* Re-emit states that need to be updated when the vertex shader is compiled separately
    * because shader configs are combined.
    */
   if (vs->info.merged_shader_compiled_separately && vs->info.next_stage == MESA_SHADER_TESS_CTRL) {
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_PATCH_CONTROL_POINTS;
   }

   /* Can't put anything else here due to merged shaders */
}

static void
radv_bind_tess_ctrl_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *tcs)
{
   radv_bind_pre_rast_shader(cmd_buffer, tcs);

   cmd_buffer->tess_rings_needed = true;

   /* Always re-emit patch control points/domain origin when a new pipeline with tessellation is
    * bound because a bunch of parameters (user SGPRs, TCS vertices out, ccw, etc) can be different.
    */
   cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_PATCH_CONTROL_POINTS | RADV_DYNAMIC_TESS_DOMAIN_ORIGIN;

   /* Re-emit the VS prolog when the tessellation control shader is compiled separately because
    * shader configs are combined and need to be updated.
    */
   if (tcs->info.merged_shader_compiled_separately)
      cmd_buffer->state.emitted_vs_prolog = NULL;
}

static void
radv_bind_tess_eval_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *tes)
{
   radv_bind_pre_rast_shader(cmd_buffer, tes);

   /* Can't put anything else here due to merged shaders */
}

static void
radv_bind_geometry_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *gs)
{
   radv_bind_pre_rast_shader(cmd_buffer, gs);

   cmd_buffer->esgs_ring_size_needed = MAX2(cmd_buffer->esgs_ring_size_needed, gs->info.gs_ring_info.esgs_ring_size);
   cmd_buffer->gsvs_ring_size_needed = MAX2(cmd_buffer->gsvs_ring_size_needed, gs->info.gs_ring_info.gsvs_ring_size);

   /* Re-emit the VS prolog when the geometry shader is compiled separately because shader configs
    * are combined and need to be updated.
    */
   if (gs->info.merged_shader_compiled_separately)
      cmd_buffer->state.emitted_vs_prolog = NULL;
}

static void
radv_bind_gs_copy_shader(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *gs_copy_shader)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   cmd_buffer->state.gs_copy_shader = gs_copy_shader;

   if (gs_copy_shader) {
      cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, gs_copy_shader->upload_seq);

      radv_cs_add_buffer(device->ws, cmd_buffer->cs, gs_copy_shader->bo);
   }
}

static void
radv_bind_mesh_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ms)
{
   radv_bind_pre_rast_shader(cmd_buffer, ms);

   cmd_buffer->mesh_scratch_ring_needed |= ms->info.ms.needs_ms_scratch_ring;
}

static void
radv_bind_fragment_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ps)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const struct radv_shader *previous_ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const float min_sample_shading = 1.0f;

   if (ps->info.ps.needs_sample_positions) {
      cmd_buffer->sample_positions_needed = true;
   }

   /* Re-emit the FS state because the SGPR idx can be different. */
   if (radv_get_user_sgpr_info(ps, AC_UD_PS_STATE)->sgpr_idx != -1) {
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES | RADV_DYNAMIC_LINE_RASTERIZATION_MODE;
   }

   /* Re-emit the conservative rasterization mode because inner coverage is different. */
   if (!previous_ps || previous_ps->info.ps.reads_fully_covered != ps->info.ps.reads_fully_covered)
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_CONSERVATIVE_RAST_MODE;

   if (gfx_level >= GFX10_3 && (!previous_ps || previous_ps->info.ps.force_sample_iter_shading_rate !=
                                                   ps->info.ps.force_sample_iter_shading_rate))
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES | RADV_DYNAMIC_FRAGMENT_SHADING_RATE;

   if (cmd_buffer->state.ms.sample_shading_enable != ps->info.ps.uses_sample_shading) {
      cmd_buffer->state.ms.sample_shading_enable = ps->info.ps.uses_sample_shading;
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;

      if (gfx_level >= GFX10_3)
         cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
   }

   if (cmd_buffer->state.ms.min_sample_shading != min_sample_shading) {
      cmd_buffer->state.ms.min_sample_shading = min_sample_shading;
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;
   }

   if (!previous_ps || previous_ps->info.regs.ps.db_shader_control != ps->info.regs.ps.db_shader_control ||
       previous_ps->info.ps.pops_is_per_sample != ps->info.ps.pops_is_per_sample)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DB_SHADER_CONTROL;

   /* Re-emit the PS epilog when a new fragment shader is bound. */
   if (ps->info.has_epilog)
      cmd_buffer->state.emitted_ps_epilog = NULL;
}

static void
radv_bind_task_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ts)
{
   if (!radv_gang_init(cmd_buffer))
      return;

   cmd_buffer->task_rings_needed = true;
}

static void
radv_bind_rt_prolog(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *rt_prolog)
{
   cmd_buffer->state.rt_prolog = rt_prolog;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const unsigned max_scratch_waves = radv_get_max_scratch_waves(device, rt_prolog);
   cmd_buffer->compute_scratch_waves_wanted = MAX2(cmd_buffer->compute_scratch_waves_wanted, max_scratch_waves);

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, rt_prolog->upload_seq);

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, rt_prolog->bo);
}

/* This function binds/unbinds a shader to the cmdbuffer state. */
static void
radv_bind_shader(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *shader, gl_shader_stage stage)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!shader) {
      cmd_buffer->state.shaders[stage] = NULL;
      cmd_buffer->state.active_stages &= ~mesa_to_vk_shader_stage(stage);

      /* Reset some dynamic states when a shader stage is unbound. */
      switch (stage) {
      case MESA_SHADER_FRAGMENT:
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DB_SHADER_CONTROL;
         cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_CONSERVATIVE_RAST_MODE | RADV_DYNAMIC_RASTERIZATION_SAMPLES |
                                            RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
         break;
      default:
         break;
      }
      return;
   }

   switch (stage) {
   case MESA_SHADER_VERTEX:
      radv_bind_vertex_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_TESS_CTRL:
      radv_bind_tess_ctrl_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_TESS_EVAL:
      radv_bind_tess_eval_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_GEOMETRY:
      radv_bind_geometry_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_FRAGMENT:
      radv_bind_fragment_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_MESH:
      radv_bind_mesh_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_TASK:
      radv_bind_task_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_COMPUTE: {
      cmd_buffer->compute_scratch_size_per_wave_needed =
         MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, shader->config.scratch_bytes_per_wave);

      const unsigned max_stage_waves = radv_get_max_scratch_waves(device, shader);
      cmd_buffer->compute_scratch_waves_wanted = MAX2(cmd_buffer->compute_scratch_waves_wanted, max_stage_waves);
      break;
   }
   case MESA_SHADER_INTERSECTION:
      /* no-op */
      break;
   default:
      unreachable("invalid shader stage");
   }

   cmd_buffer->state.shaders[stage] = shader;
   cmd_buffer->state.active_stages |= mesa_to_vk_shader_stage(stage);

   if (mesa_to_vk_shader_stage(stage) & RADV_GRAPHICS_STAGE_BITS) {
      cmd_buffer->scratch_size_per_wave_needed =
         MAX2(cmd_buffer->scratch_size_per_wave_needed, shader->config.scratch_bytes_per_wave);

      const unsigned max_stage_waves = radv_get_max_scratch_waves(device, shader);
      cmd_buffer->scratch_waves_wanted = MAX2(cmd_buffer->scratch_waves_wanted, max_stage_waves);
   }

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, shader->upload_seq);

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, shader->bo);
}

static void
radv_reset_shader_object_state(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint pipelineBindPoint)
{
   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      if (cmd_buffer->state.shader_objs[MESA_SHADER_COMPUTE]) {
         radv_bind_shader(cmd_buffer, NULL, MESA_SHADER_COMPUTE);
         cmd_buffer->state.shader_objs[MESA_SHADER_COMPUTE] = NULL;
      }
      break;
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      radv_foreach_stage(s, RADV_GRAPHICS_STAGE_BITS)
      {
         if (cmd_buffer->state.shader_objs[s]) {
            radv_bind_shader(cmd_buffer, NULL, s);
            cmd_buffer->state.shader_objs[s] = NULL;
         }
      }
      break;
   default:
      break;
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GRAPHICS_SHADERS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline _pipeline)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_pipeline, pipeline, _pipeline);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   radv_reset_shader_object_state(cmd_buffer, pipelineBindPoint);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE: {
      struct radv_compute_pipeline *compute_pipeline = radv_pipeline_to_compute(pipeline);

      if (cmd_buffer->state.compute_pipeline == compute_pipeline)
         return;
      radv_mark_descriptor_sets_dirty(cmd_buffer, pipelineBindPoint);

      radv_bind_shader(cmd_buffer, compute_pipeline->base.shaders[MESA_SHADER_COMPUTE], MESA_SHADER_COMPUTE);

      cmd_buffer->state.compute_pipeline = compute_pipeline;
      cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   }
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: {
      struct radv_ray_tracing_pipeline *rt_pipeline = radv_pipeline_to_ray_tracing(pipeline);

      if (cmd_buffer->state.rt_pipeline == rt_pipeline)
         return;
      radv_mark_descriptor_sets_dirty(cmd_buffer, pipelineBindPoint);

      radv_bind_shader(cmd_buffer, rt_pipeline->base.base.shaders[MESA_SHADER_INTERSECTION], MESA_SHADER_INTERSECTION);
      radv_bind_rt_prolog(cmd_buffer, rt_pipeline->prolog);

      for (unsigned i = 0; i < rt_pipeline->stage_count; ++i) {
         struct radv_shader *shader = rt_pipeline->stages[i].shader;
         if (shader)
            radv_cs_add_buffer(device->ws, cmd_buffer->cs, shader->bo);
      }

      cmd_buffer->state.rt_pipeline = rt_pipeline;
      cmd_buffer->push_constant_stages |= RADV_RT_STAGE_BITS;

      /* Bind the stack size when it's not dynamic. */
      if (rt_pipeline->stack_size != -1u)
         cmd_buffer->state.rt_stack_size = rt_pipeline->stack_size;

      break;
   }
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);

      /* Bind the non-dynamic graphics state from the pipeline unconditionally because some PSO
       * might have been overwritten between two binds of the same pipeline.
       */
      radv_bind_dynamic_state(cmd_buffer, &graphics_pipeline->dynamic_state);

      if (cmd_buffer->state.graphics_pipeline == graphics_pipeline)
         return;
      radv_mark_descriptor_sets_dirty(cmd_buffer, pipelineBindPoint);

      radv_foreach_stage(
         stage, (cmd_buffer->state.active_stages | graphics_pipeline->active_stages) & RADV_GRAPHICS_STAGE_BITS)
      {
         radv_bind_shader(cmd_buffer, graphics_pipeline->base.shaders[stage], stage);
      }

      radv_bind_gs_copy_shader(cmd_buffer, graphics_pipeline->base.gs_copy_shader);

      cmd_buffer->state.last_vgt_shader = graphics_pipeline->base.shaders[graphics_pipeline->last_vgt_api_stage];

      cmd_buffer->state.graphics_pipeline = graphics_pipeline;

      cmd_buffer->state.has_nggc = graphics_pipeline->has_ngg_culling;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PIPELINE;
      cmd_buffer->push_constant_stages |= graphics_pipeline->active_stages;

      /* Prefetch all pipeline shaders at first draw time. */
      cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_SHADERS;

      if (pdev->info.has_vgt_flush_ngg_legacy_bug &&
          (!cmd_buffer->state.emitted_graphics_pipeline ||
           (cmd_buffer->state.emitted_graphics_pipeline->is_ngg && !cmd_buffer->state.graphics_pipeline->is_ngg))) {
         /* Transitioning from NGG to legacy GS requires
          * VGT_FLUSH on GFX10 and Navi21. VGT_FLUSH
          * is also emitted at the beginning of IBs when legacy
          * GS ring pointers are set.
          */
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_FLUSH;
      }

      cmd_buffer->state.uses_dynamic_patch_control_points =
         !!(graphics_pipeline->dynamic_states & RADV_DYNAMIC_PATCH_CONTROL_POINTS);

      if (graphics_pipeline->active_stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) {
         if (!cmd_buffer->state.uses_dynamic_patch_control_points) {
            /* Bind the tessellation state from the pipeline when it's not dynamic. */
            struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];

            cmd_buffer->state.tess_num_patches = tcs->info.num_tess_patches;
            cmd_buffer->state.tess_lds_size = tcs->info.tcs.num_lds_blocks;
         }
      }

      const struct radv_shader *vs = radv_get_shader(graphics_pipeline->base.shaders, MESA_SHADER_VERTEX);
      if (vs) {
         /* Re-emit the VS prolog when a new vertex shader is bound. */
         if (vs->info.vs.has_prolog) {
            cmd_buffer->state.emitted_vs_prolog = NULL;
            cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT;
         }

         /* Re-emit the vertex buffer descriptors because they are really tied to the pipeline. */
         if (vs->info.vs.vb_desc_usage_mask) {
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
         }
      }

      if (!cmd_buffer->state.emitted_graphics_pipeline ||
          cmd_buffer->state.spi_shader_col_format != graphics_pipeline->spi_shader_col_format) {
         cmd_buffer->state.spi_shader_col_format = graphics_pipeline->spi_shader_col_format;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_COLOR_OUTPUT;
         if (pdev->info.rbplus_allowed)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
      }

      if (!cmd_buffer->state.emitted_graphics_pipeline ||
          cmd_buffer->state.cb_shader_mask != graphics_pipeline->cb_shader_mask) {
         cmd_buffer->state.cb_shader_mask = graphics_pipeline->cb_shader_mask;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_COLOR_OUTPUT;
      }

      radv_bind_vs_input_state(cmd_buffer, graphics_pipeline);

      radv_bind_multisample_state(cmd_buffer, &graphics_pipeline->ms);

      radv_bind_custom_blend_mode(cmd_buffer, graphics_pipeline->custom_blend_mode);

      cmd_buffer->state.db_render_control = graphics_pipeline->db_render_control;

      cmd_buffer->state.rast_prim = graphics_pipeline->rast_prim;

      cmd_buffer->state.ia_multi_vgt_param = graphics_pipeline->ia_multi_vgt_param;

      cmd_buffer->state.uses_out_of_order_rast = graphics_pipeline->uses_out_of_order_rast;
      cmd_buffer->state.uses_vrs = graphics_pipeline->uses_vrs;
      cmd_buffer->state.uses_vrs_attachment = graphics_pipeline->uses_vrs_attachment;
      cmd_buffer->state.uses_vrs_coarse_shading = graphics_pipeline->uses_vrs_coarse_shading;
      cmd_buffer->state.uses_dynamic_vertex_binding_stride =
         !!(graphics_pipeline->dynamic_states & (RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE | RADV_DYNAMIC_VERTEX_INPUT));
      break;
   }
   default:
      assert(!"invalid bind point");
      break;
   }

   cmd_buffer->push_constant_state[vk_to_bind_point(pipelineBindPoint)].size = pipeline->push_constant_size;
   cmd_buffer->push_constant_state[vk_to_bind_point(pipelineBindPoint)].dynamic_offset_count =
      pipeline->dynamic_offset_count;
   cmd_buffer->descriptors[vk_to_bind_point(pipelineBindPoint)].need_indirect_descriptor_sets =
      pipeline->need_indirect_descriptor_sets;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
                    const VkViewport *pViewports)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   ASSERTED const uint32_t total_count = firstViewport + viewportCount;

   assert(firstViewport < MAX_VIEWPORTS);
   assert(total_count >= 1 && total_count <= MAX_VIEWPORTS);

   if (state->dynamic.vk.vp.viewport_count < total_count)
      state->dynamic.vk.vp.viewport_count = total_count;

   memcpy(state->dynamic.vk.vp.viewports + firstViewport, pViewports, viewportCount * sizeof(*pViewports));
   for (unsigned i = 0; i < viewportCount; i++) {
      radv_get_viewport_xform(&pViewports[i], state->dynamic.hw_vp.xform[i + firstViewport].scale,
                              state->dynamic.hw_vp.xform[i + firstViewport].translate);
   }

   state->dirty_dynamic |= RADV_DYNAMIC_VIEWPORT;
   state->dirty |= RADV_CMD_DIRTY_GUARDBAND;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
                   const VkRect2D *pScissors)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   ASSERTED const uint32_t total_count = firstScissor + scissorCount;

   assert(firstScissor < MAX_SCISSORS);
   assert(total_count >= 1 && total_count <= MAX_SCISSORS);

   if (state->dynamic.vk.vp.scissor_count < total_count)
      state->dynamic.vk.vp.scissor_count = total_count;

   memcpy(state->dynamic.vk.vp.scissors + firstScissor, pScissors, scissorCount * sizeof(*pScissors));

   state->dirty_dynamic |= RADV_DYNAMIC_SCISSOR;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.width = lineWidth;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_WIDTH;
   state->dirty |= RADV_CMD_DIRTY_GUARDBAND;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   memcpy(state->dynamic.vk.cb.blend_constants, blendConstants, sizeof(float) * 4);

   state->dirty_dynamic |= RADV_DYNAMIC_BLEND_CONSTANTS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.bounds_test.min = minDepthBounds;
   state->dynamic.vk.ds.depth.bounds_test.max = maxDepthBounds;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BOUNDS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.compare_mask = compareMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.compare_mask = compareMask;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_COMPARE_MASK;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t writeMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.write_mask = writeMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.write_mask = writeMask;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_WRITE_MASK;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.reference = reference;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.reference = reference;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_REFERENCE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t firstDiscardRectangle,
                               uint32_t discardRectangleCount, const VkRect2D *pDiscardRectangles)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   ASSERTED const uint32_t total_count = firstDiscardRectangle + discardRectangleCount;

   assert(firstDiscardRectangle < MAX_DISCARD_RECTANGLES);
   assert(total_count >= 1 && total_count <= MAX_DISCARD_RECTANGLES);

   typed_memcpy(&state->dynamic.vk.dr.rectangles[firstDiscardRectangle], pDiscardRectangles, discardRectangleCount);

   state->dirty_dynamic |= RADV_DYNAMIC_DISCARD_RECTANGLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer, const VkSampleLocationsInfoEXT *pSampleLocationsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(pSampleLocationsInfo->sampleLocationsCount <= MAX_SAMPLE_LOCATIONS);

   state->dynamic.sample_location.per_pixel = pSampleLocationsInfo->sampleLocationsPerPixel;
   state->dynamic.sample_location.grid_size = pSampleLocationsInfo->sampleLocationGridSize;
   state->dynamic.sample_location.count = pSampleLocationsInfo->sampleLocationsCount;
   typed_memcpy(&state->dynamic.sample_location.locations[0], pSampleLocationsInfo->pSampleLocations,
                pSampleLocationsInfo->sampleLocationsCount);

   state->dirty_dynamic |= RADV_DYNAMIC_SAMPLE_LOCATIONS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineStippleKHR(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.stipple.factor = lineStippleFactor;
   state->dynamic.vk.rs.line.stipple.pattern = lineStipplePattern;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_STIPPLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.cull_mode = cullMode;

   state->dirty_dynamic |= RADV_DYNAMIC_CULL_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.front_face = frontFace;

   state->dirty_dynamic |= RADV_DYNAMIC_FRONT_FACE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned primitive_topology = radv_translate_prim(primitiveTopology);

   if (radv_primitive_topology_is_line_list(state->dynamic.vk.ia.primitive_topology) !=
       radv_primitive_topology_is_line_list(primitive_topology))
      state->dirty_dynamic |= RADV_DYNAMIC_LINE_STIPPLE;

   if (radv_prim_is_points_or_lines(state->dynamic.vk.ia.primitive_topology) !=
       radv_prim_is_points_or_lines(primitive_topology))
      state->dirty |= RADV_CMD_DIRTY_GUARDBAND;

   state->dynamic.vk.ia.primitive_topology = primitive_topology;

   state->dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_TOPOLOGY;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport *pViewports)
{
   radv_CmdSetViewport(commandBuffer, 0, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D *pScissors)
{
   radv_CmdSetScissor(commandBuffer, 0, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)

{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.test_enable = depthTestEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_TEST_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.write_enable = depthWriteEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_WRITE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.compare_op = depthCompareOp;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_COMPARE_OP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.bounds_test.enable = depthBoundsTestEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.stencil.test_enable = stencilTestEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_TEST_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp,
                     VkStencilOp depthFailOp, VkCompareOp compareOp)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
      state->dynamic.vk.ds.stencil.front.op.fail = failOp;
      state->dynamic.vk.ds.stencil.front.op.pass = passOp;
      state->dynamic.vk.ds.stencil.front.op.depth_fail = depthFailOp;
      state->dynamic.vk.ds.stencil.front.op.compare = compareOp;
   }

   if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
      state->dynamic.vk.ds.stencil.back.op.fail = failOp;
      state->dynamic.vk.ds.stencil.back.op.pass = passOp;
      state->dynamic.vk.ds.stencil.back.op.depth_fail = depthFailOp;
      state->dynamic.vk.ds.stencil.back.op.compare = compareOp;
   }

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_OP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetFragmentShadingRateKHR(VkCommandBuffer commandBuffer, const VkExtent2D *pFragmentSize,
                                  const VkFragmentShadingRateCombinerOpKHR combinerOps[2])
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.fsr.fragment_size = *pFragmentSize;
   for (unsigned i = 0; i < 2; i++)
      state->dynamic.vk.fsr.combiner_ops[i] = combinerOps[i];

   state->dirty_dynamic |= RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_bias.enable = depthBiasEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BIAS_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ia.primitive_restart_enable = primitiveRestartEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.rasterizer_discard_enable = rasterizerDiscardEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPatchControlPointsEXT(VkCommandBuffer commandBuffer, uint32_t patchControlPoints)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ts.patch_control_points = patchControlPoints;

   state->dirty_dynamic |= RADV_DYNAMIC_PATCH_CONTROL_POINTS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp logicOp)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned logic_op = radv_translate_blend_logic_op(logicOp);

   state->dynamic.vk.cb.logic_op = logic_op;

   state->dirty_dynamic |= RADV_DYNAMIC_LOGIC_OP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorWriteEnableEXT(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                               const VkBool32 *pColorWriteEnables)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint8_t color_write_enable = 0;

   assert(attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pColorWriteEnables[i]) {
         color_write_enable |= BITFIELD_BIT(i);
      }
   }

   state->dynamic.vk.cb.color_write_enables = color_write_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_WRITE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetVertexInputEXT(VkCommandBuffer commandBuffer, uint32_t vertexBindingDescriptionCount,
                          const VkVertexInputBindingDescription2EXT *pVertexBindingDescriptions,
                          uint32_t vertexAttributeDescriptionCount,
                          const VkVertexInputAttributeDescription2EXT *pVertexAttributeDescriptions)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_vs_input_state *vs_state = &state->dynamic_vs_input;

   const VkVertexInputBindingDescription2EXT *bindings[MAX_VBS];
   for (unsigned i = 0; i < vertexBindingDescriptionCount; i++)
      bindings[pVertexBindingDescriptions[i].binding] = &pVertexBindingDescriptions[i];

   state->vbo_misaligned_mask = 0;
   state->vbo_unaligned_mask = 0;
   state->vbo_misaligned_mask_invalid = 0;

   vs_state->attribute_mask = 0;
   vs_state->instance_rate_inputs = 0;
   vs_state->nontrivial_divisors = 0;
   vs_state->zero_divisors = 0;
   vs_state->post_shuffle = 0;
   vs_state->alpha_adjust_lo = 0;
   vs_state->alpha_adjust_hi = 0;
   vs_state->nontrivial_formats = 0;
   vs_state->bindings_match_attrib = true;

   enum amd_gfx_level chip = pdev->info.gfx_level;
   enum radeon_family family = pdev->info.family;
   const struct ac_vtx_format_info *vtx_info_table = ac_get_vtx_format_info_table(chip, family);

   for (unsigned i = 0; i < vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription2EXT *attrib = &pVertexAttributeDescriptions[i];
      const VkVertexInputBindingDescription2EXT *binding = bindings[attrib->binding];
      unsigned loc = attrib->location;

      vs_state->attribute_mask |= 1u << loc;
      vs_state->bindings[loc] = attrib->binding;
      if (attrib->binding != loc)
         vs_state->bindings_match_attrib = false;
      if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
         vs_state->instance_rate_inputs |= 1u << loc;
         vs_state->divisors[loc] = binding->divisor;
         if (binding->divisor == 0) {
            vs_state->zero_divisors |= 1u << loc;
         } else if (binding->divisor > 1) {
            vs_state->nontrivial_divisors |= 1u << loc;
         }
      }
      cmd_buffer->vertex_bindings[attrib->binding].stride = binding->stride;
      vs_state->offsets[loc] = attrib->offset;

      enum pipe_format format = vk_format_map[attrib->format];
      const struct ac_vtx_format_info *vtx_info = &vtx_info_table[format];

      vs_state->formats[loc] = format;
      uint8_t format_align_req_minus_1 = vtx_info->chan_byte_size >= 4 ? 3 : (vtx_info->element_size - 1);
      vs_state->format_align_req_minus_1[loc] = format_align_req_minus_1;
      uint8_t component_align_req_minus_1 =
         MIN2(vtx_info->chan_byte_size ? vtx_info->chan_byte_size : vtx_info->element_size, 4) - 1;
      vs_state->component_align_req_minus_1[loc] = component_align_req_minus_1;
      vs_state->format_sizes[loc] = vtx_info->element_size;
      vs_state->alpha_adjust_lo |= (vtx_info->alpha_adjust & 0x1) << loc;
      vs_state->alpha_adjust_hi |= (vtx_info->alpha_adjust >> 1) << loc;
      if (G_008F0C_DST_SEL_X(vtx_info->dst_sel) == V_008F0C_SQ_SEL_Z)
         vs_state->post_shuffle |= BITFIELD_BIT(loc);

      if (!(vtx_info->has_hw_format & BITFIELD_BIT(vtx_info->num_channels - 1)))
         vs_state->nontrivial_formats |= BITFIELD_BIT(loc);

      if (state->vbo_bound_mask & BITFIELD_BIT(attrib->binding)) {
         uint32_t stride = binding->stride;
         uint64_t offset = cmd_buffer->vertex_bindings[attrib->binding].offset + vs_state->offsets[loc];
         if ((chip == GFX6 || chip >= GFX10) && ((stride | offset) & format_align_req_minus_1))
            state->vbo_misaligned_mask |= BITFIELD_BIT(loc);
         if ((stride | offset) & component_align_req_minus_1)
            state->vbo_unaligned_mask |= BITFIELD_BIT(loc);
      }
   }

   state->dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT;
   state->dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPolygonModeEXT(VkCommandBuffer commandBuffer, VkPolygonMode polygonMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned polygon_mode = radv_translate_fill(polygonMode);

   if (radv_polygon_mode_is_points_or_lines(state->dynamic.vk.rs.polygon_mode) !=
       radv_polygon_mode_is_points_or_lines(polygon_mode))
      state->dirty |= RADV_CMD_DIRTY_GUARDBAND;

   state->dynamic.vk.rs.polygon_mode = polygon_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_POLYGON_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetTessellationDomainOriginEXT(VkCommandBuffer commandBuffer, VkTessellationDomainOrigin domainOrigin)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ts.domain_origin = domainOrigin;

   state->dirty_dynamic |= RADV_DYNAMIC_TESS_DOMAIN_ORIGIN;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLogicOpEnableEXT(VkCommandBuffer commandBuffer, VkBool32 logicOpEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.cb.logic_op_enable = logicOpEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_LOGIC_OP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineStippleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stippledLineEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.stipple.enable = stippledLineEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_STIPPLE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAlphaToCoverageEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToCoverageEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.alpha_to_coverage_enable = alphaToCoverageEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAlphaToOneEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToOneEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.alpha_to_one_enable = alphaToOneEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleMaskEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits samples, const VkSampleMask *pSampleMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.sample_mask = pSampleMask[0] & 0xffff;

   state->dirty_dynamic |= RADV_DYNAMIC_SAMPLE_MASK;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClipEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClipEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_clip_enable = depthClipEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLIP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetConservativeRasterizationModeEXT(VkCommandBuffer commandBuffer,
                                            VkConservativeRasterizationModeEXT conservativeRasterizationMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.conservative_mode = conservativeRasterizationMode;

   state->dirty_dynamic |= RADV_DYNAMIC_CONSERVATIVE_RAST_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer commandBuffer, VkBool32 negativeOneToOne)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.vp.depth_clip_negative_one_to_one = negativeOneToOne;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetProvokingVertexModeEXT(VkCommandBuffer commandBuffer, VkProvokingVertexModeEXT provokingVertexMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.provoking_vertex = provokingVertexMode;

   state->dirty_dynamic |= RADV_DYNAMIC_PROVOKING_VERTEX_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClampEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClampEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_clamp_enable = depthClampEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLAMP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
                             const VkColorComponentFlags *pColorWriteMasks)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(firstAttachment + attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t idx = firstAttachment + i;

      state->dynamic.vk.cb.attachments[idx].write_mask = pColorWriteMasks[i];
   }

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_WRITE_MASK;

   if (pdev->info.rbplus_allowed)
      state->dirty |= RADV_CMD_DIRTY_RBPLUS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
                               const VkBool32 *pColorBlendEnables)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(firstAttachment + attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t idx = firstAttachment + i;

      state->dynamic.vk.cb.attachments[idx].blend_enable = pColorBlendEnables[i];
   }

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_BLEND_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRasterizationSamplesEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits rasterizationSamples)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.rasterization_samples = rasterizationSamples;

   state->dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineRasterizationModeEXT(VkCommandBuffer commandBuffer, VkLineRasterizationModeKHR lineRasterizationMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.mode = lineRasterizationMode;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_RASTERIZATION_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
                                 const VkColorBlendEquationEXT *pColorBlendEquations)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(firstAttachment + attachmentCount <= MAX_RTS);
   for (uint32_t i = 0; i < attachmentCount; i++) {
      unsigned idx = firstAttachment + i;

      state->dynamic.vk.cb.attachments[idx].src_color_blend_factor = pColorBlendEquations[i].srcColorBlendFactor;
      state->dynamic.vk.cb.attachments[idx].dst_color_blend_factor = pColorBlendEquations[i].dstColorBlendFactor;
      state->dynamic.vk.cb.attachments[idx].color_blend_op = pColorBlendEquations[i].colorBlendOp;
      state->dynamic.vk.cb.attachments[idx].src_alpha_blend_factor = pColorBlendEquations[i].srcAlphaBlendFactor;
      state->dynamic.vk.cb.attachments[idx].dst_alpha_blend_factor = pColorBlendEquations[i].dstAlphaBlendFactor;
      state->dynamic.vk.cb.attachments[idx].alpha_blend_op = pColorBlendEquations[i].alphaBlendOp;
   }

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_BLEND_EQUATION;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleLocationsEnableEXT(VkCommandBuffer commandBuffer, VkBool32 sampleLocationsEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.sample_locations_enable = sampleLocationsEnable;

   state->dirty_dynamic |= RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 discardRectangleEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.dr.enable = discardRectangleEnable;
   state->dynamic.vk.dr.rectangle_count = discardRectangleEnable ? MAX_DISCARD_RECTANGLES : 0;

   state->dirty_dynamic |= RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleModeEXT(VkCommandBuffer commandBuffer, VkDiscardRectangleModeEXT discardRectangleMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.dr.mode = discardRectangleMode;

   state->dirty_dynamic |= RADV_DYNAMIC_DISCARD_RECTANGLE_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer commandBuffer, VkImageAspectFlags aspectMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.feedback_loop_aspects = aspectMask;

   state->dirty_dynamic |= RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBias2EXT(VkCommandBuffer commandBuffer, const VkDepthBiasInfoEXT *pDepthBiasInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   const VkDepthBiasRepresentationInfoEXT *dbr_info =
      vk_find_struct_const(pDepthBiasInfo->pNext, DEPTH_BIAS_REPRESENTATION_INFO_EXT);

   state->dynamic.vk.rs.depth_bias.constant = pDepthBiasInfo->depthBiasConstantFactor;
   state->dynamic.vk.rs.depth_bias.clamp = pDepthBiasInfo->depthBiasClamp;
   state->dynamic.vk.rs.depth_bias.slope = pDepthBiasInfo->depthBiasSlopeFactor;
   state->dynamic.vk.rs.depth_bias.representation =
      dbr_info ? dbr_info->depthBiasRepresentation : VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BIAS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRenderingAttachmentLocationsKHR(VkCommandBuffer commandBuffer,
                                           const VkRenderingAttachmentLocationInfoKHR *pLocationInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assume(pLocationInfo->colorAttachmentCount <= MESA_VK_MAX_COLOR_ATTACHMENTS);
   for (uint32_t i = 0; i < pLocationInfo->colorAttachmentCount; i++) {
      state->dynamic.vk.cal.color_map[i] = pLocationInfo->pColorAttachmentLocations[i] == VK_ATTACHMENT_UNUSED
                                              ? MESA_VK_ATTACHMENT_UNUSED
                                              : pLocationInfo->pColorAttachmentLocations[i];
   }

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_ATTACHMENT_MAP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCmdBuffers)
{
   VK_FROM_HANDLE(radv_cmd_buffer, primary, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(primary);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   assert(commandBufferCount > 0);

   radv_emit_mip_change_flush_default(primary);

   /* Emit pending flushes on primary prior to executing secondary */
   radv_emit_cache_flush(primary);

   /* Make sure CP DMA is idle on primary prior to executing secondary. */
   radv_cp_dma_wait_for_idle(primary);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(radv_cmd_buffer, secondary, pCmdBuffers[i]);

      /* Do not launch an IB2 for secondary command buffers that contain
       * DRAW_{INDEX}_INDIRECT_{MULTI} on GFX6-7 because it's illegal and hangs the GPU.
       */
      const bool allow_ib2 = !secondary->state.uses_draw_indirect || pdev->info.gfx_level >= GFX8;

      primary->scratch_size_per_wave_needed =
         MAX2(primary->scratch_size_per_wave_needed, secondary->scratch_size_per_wave_needed);
      primary->scratch_waves_wanted = MAX2(primary->scratch_waves_wanted, secondary->scratch_waves_wanted);
      primary->compute_scratch_size_per_wave_needed =
         MAX2(primary->compute_scratch_size_per_wave_needed, secondary->compute_scratch_size_per_wave_needed);
      primary->compute_scratch_waves_wanted =
         MAX2(primary->compute_scratch_waves_wanted, secondary->compute_scratch_waves_wanted);

      if (secondary->esgs_ring_size_needed > primary->esgs_ring_size_needed)
         primary->esgs_ring_size_needed = secondary->esgs_ring_size_needed;
      if (secondary->gsvs_ring_size_needed > primary->gsvs_ring_size_needed)
         primary->gsvs_ring_size_needed = secondary->gsvs_ring_size_needed;
      if (secondary->tess_rings_needed)
         primary->tess_rings_needed = true;
      if (secondary->task_rings_needed)
         primary->task_rings_needed = true;
      if (secondary->mesh_scratch_ring_needed)
         primary->mesh_scratch_ring_needed = true;
      if (secondary->sample_positions_needed)
         primary->sample_positions_needed = true;
      if (secondary->gds_needed)
         primary->gds_needed = true;
      if (secondary->gds_oa_needed)
         primary->gds_oa_needed = true;

      primary->shader_upload_seq = MAX2(primary->shader_upload_seq, secondary->shader_upload_seq);

      if (!secondary->state.render.has_image_views && primary->state.render.active &&
          (primary->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)) {
         /* Emit the framebuffer state from primary if secondary
          * has been recorded without a framebuffer, otherwise
          * fast color/depth clears can't work.
          */
         radv_emit_framebuffer_state(primary);
      }

      if (secondary->gang.cs) {
         if (!radv_gang_init(primary))
            return;

         struct radeon_cmdbuf *ace_primary = primary->gang.cs;
         struct radeon_cmdbuf *ace_secondary = secondary->gang.cs;

         /* Emit pending flushes on primary prior to executing secondary. */
         radv_gang_cache_flush(primary);

         /* Wait for gang semaphores, if necessary. */
         if (radv_flush_gang_leader_semaphore(primary))
            radv_wait_gang_leader(primary);
         if (radv_flush_gang_follower_semaphore(primary))
            radv_wait_gang_follower(primary);

         /* Execute the secondary compute cmdbuf.
          * Don't use IB2 packets because they are not supported on compute queues.
          */
         device->ws->cs_execute_secondary(ace_primary, ace_secondary, false);
      }

      /* Update pending ACE internal flush bits from the secondary cmdbuf */
      primary->gang.flush_bits |= secondary->gang.flush_bits;

      /* Increment gang semaphores if secondary was dirty.
       * This happens when the secondary cmdbuf has a barrier which
       * isn't consumed by a draw call.
       */
      if (radv_gang_leader_sem_dirty(secondary))
         primary->gang.sem.leader_value++;
      if (radv_gang_follower_sem_dirty(secondary))
         primary->gang.sem.follower_value++;

      device->ws->cs_execute_secondary(primary->cs, secondary->cs, allow_ib2);

      /* When the secondary command buffer is compute only we don't
       * need to re-emit the current graphics pipeline.
       */
      if (secondary->state.emitted_graphics_pipeline) {
         primary->state.emitted_graphics_pipeline = secondary->state.emitted_graphics_pipeline;
      }

      /* When the secondary command buffer is graphics only we don't
       * need to re-emit the current compute pipeline.
       */
      if (secondary->state.emitted_compute_pipeline) {
         primary->state.emitted_compute_pipeline = secondary->state.emitted_compute_pipeline;
      }

      if (secondary->state.last_ia_multi_vgt_param) {
         primary->state.last_ia_multi_vgt_param = secondary->state.last_ia_multi_vgt_param;
      }

      if (secondary->state.last_ge_cntl) {
         primary->state.last_ge_cntl = secondary->state.last_ge_cntl;
      }

      primary->state.last_num_instances = secondary->state.last_num_instances;
      primary->state.last_subpass_color_count = secondary->state.last_subpass_color_count;

      if (secondary->state.last_index_type != -1) {
         primary->state.last_index_type = secondary->state.last_index_type;
      }

      primary->state.last_vrs_rates = secondary->state.last_vrs_rates;
      primary->state.last_vrs_rates_sgpr_idx = secondary->state.last_vrs_rates_sgpr_idx;

      primary->state.rb_noncoherent_dirty |= secondary->state.rb_noncoherent_dirty;

      primary->state.uses_draw_indirect |= secondary->state.uses_draw_indirect;

      for (uint32_t reg = 0; reg < RADV_NUM_ALL_TRACKED_REGS; reg++) {
         if (!BITSET_TEST(secondary->tracked_regs.reg_saved_mask, reg))
            continue;

         BITSET_SET(primary->tracked_regs.reg_saved_mask, reg);
         primary->tracked_regs.reg_value[reg] = secondary->tracked_regs.reg_value[reg];
      }

      memcpy(primary->tracked_regs.spi_ps_input_cntl, secondary->tracked_regs.spi_ps_input_cntl,
             sizeof(primary->tracked_regs.spi_ps_input_cntl));
   }

   /* After executing commands from secondary buffers we have to dirty
    * some states.
    */
   primary->state.dirty_dynamic |= RADV_DYNAMIC_ALL;
   primary->state.dirty |= RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_INDEX_BUFFER | RADV_CMD_DIRTY_GUARDBAND |
                           RADV_CMD_DIRTY_SHADER_QUERY | RADV_CMD_DIRTY_OCCLUSION_QUERY |
                           RADV_CMD_DIRTY_DB_SHADER_CONTROL | RADV_CMD_DIRTY_COLOR_OUTPUT;
   radv_mark_descriptor_sets_dirty(primary, VK_PIPELINE_BIND_POINT_GRAPHICS);
   radv_mark_descriptor_sets_dirty(primary, VK_PIPELINE_BIND_POINT_COMPUTE);

   primary->state.last_first_instance = -1;
   primary->state.last_drawid = -1;
   primary->state.last_vertex_offset_valid = false;
}

static void
radv_mark_noncoherent_rb(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   /* Have to be conservative in cmdbuffers with inherited attachments. */
   if (!render->has_image_views) {
      cmd_buffer->state.rb_noncoherent_dirty = true;
      return;
   }

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      if (render->color_att[i].iview && !render->color_att[i].iview->image->l2_coherent) {
         cmd_buffer->state.rb_noncoherent_dirty = true;
         return;
      }
   }
   if (render->ds_att.iview && !render->ds_att.iview->image->l2_coherent)
      cmd_buffer->state.rb_noncoherent_dirty = true;
}

static VkImageLayout
attachment_initial_layout(const VkRenderingAttachmentInfo *att)
{
   const VkRenderingAttachmentInitialLayoutInfoMESA *layout_info =
      vk_find_struct_const(att->pNext, RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA);
   if (layout_info != NULL)
      return layout_info->initialLayout;

   return att->imageLayout;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   const struct VkSampleLocationsInfoEXT *sample_locs_info =
      vk_find_struct_const(pRenderingInfo->pNext, SAMPLE_LOCATIONS_INFO_EXT);

   struct radv_sample_locations_state sample_locations = {
      .count = 0,
   };
   if (sample_locs_info) {
      sample_locations = (struct radv_sample_locations_state){
         .per_pixel = sample_locs_info->sampleLocationsPerPixel,
         .grid_size = sample_locs_info->sampleLocationGridSize,
         .count = sample_locs_info->sampleLocationsCount,
      };
      typed_memcpy(sample_locations.locations, sample_locs_info->pSampleLocations,
                   sample_locs_info->sampleLocationsCount);
   }

   /* Dynamic rendering does not have implicit transitions, so limit the marker to
    * when a render pass is used.
    * Additionally, some internal meta operations called inside a barrier may issue
    * render calls (with dynamic rendering), so this makes sure those case don't
    * create a nested barrier scope.
    */
   if (cmd_buffer->vk.render_pass)
      radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_EXTERNAL_RENDER_PASS_SYNC);
   uint32_t color_samples = 0, ds_samples = 0;
   struct radv_attachment color_att[MAX_RTS];
   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att_info = &pRenderingInfo->pColorAttachments[i];

      color_att[i] = (struct radv_attachment){.iview = NULL};
      if (att_info->imageView == VK_NULL_HANDLE)
         continue;

      VK_FROM_HANDLE(radv_image_view, iview, att_info->imageView);
      color_att[i].format = iview->vk.format;
      color_att[i].iview = iview;
      color_att[i].layout = att_info->imageLayout;
      radv_initialise_color_surface(device, &color_att[i].cb, iview);

      if (att_info->resolveMode != VK_RESOLVE_MODE_NONE && att_info->resolveImageView != VK_NULL_HANDLE) {
         color_att[i].resolve_mode = att_info->resolveMode;
         color_att[i].resolve_iview = radv_image_view_from_handle(att_info->resolveImageView);
         color_att[i].resolve_layout = att_info->resolveImageLayout;
      }

      color_samples = MAX2(color_samples, color_att[i].iview->vk.image->samples);

      VkImageLayout initial_layout = attachment_initial_layout(att_info);
      if (initial_layout != color_att[i].layout) {
         assert(!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT));
         radv_handle_rendering_image_transition(cmd_buffer, color_att[i].iview, pRenderingInfo->layerCount,
                                                pRenderingInfo->viewMask, initial_layout, VK_IMAGE_LAYOUT_UNDEFINED,
                                                color_att[i].layout, VK_IMAGE_LAYOUT_UNDEFINED, &sample_locations);
      }
   }

   struct radv_attachment ds_att = {.iview = NULL};
   VkImageAspectFlags ds_att_aspects = 0;
   const VkRenderingAttachmentInfo *d_att_info = pRenderingInfo->pDepthAttachment;
   const VkRenderingAttachmentInfo *s_att_info = pRenderingInfo->pStencilAttachment;
   if ((d_att_info != NULL && d_att_info->imageView != VK_NULL_HANDLE) ||
       (s_att_info != NULL && s_att_info->imageView != VK_NULL_HANDLE)) {
      struct radv_image_view *d_iview = NULL, *s_iview = NULL;
      struct radv_image_view *d_res_iview = NULL, *s_res_iview = NULL;
      VkImageLayout initial_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImageLayout initial_stencil_layout = VK_IMAGE_LAYOUT_UNDEFINED;

      if (d_att_info != NULL && d_att_info->imageView != VK_NULL_HANDLE) {
         d_iview = radv_image_view_from_handle(d_att_info->imageView);
         initial_depth_layout = attachment_initial_layout(d_att_info);
         ds_att.layout = d_att_info->imageLayout;

         if (d_att_info->resolveMode != VK_RESOLVE_MODE_NONE && d_att_info->resolveImageView != VK_NULL_HANDLE) {
            d_res_iview = radv_image_view_from_handle(d_att_info->resolveImageView);
            ds_att.resolve_mode = d_att_info->resolveMode;
            ds_att.resolve_layout = d_att_info->resolveImageLayout;
         }
      }

      if (s_att_info != NULL && s_att_info->imageView != VK_NULL_HANDLE) {
         s_iview = radv_image_view_from_handle(s_att_info->imageView);
         initial_stencil_layout = attachment_initial_layout(s_att_info);
         ds_att.stencil_layout = s_att_info->imageLayout;

         if (s_att_info->resolveMode != VK_RESOLVE_MODE_NONE && s_att_info->resolveImageView != VK_NULL_HANDLE) {
            s_res_iview = radv_image_view_from_handle(s_att_info->resolveImageView);
            ds_att.stencil_resolve_mode = s_att_info->resolveMode;
            ds_att.stencil_resolve_layout = s_att_info->resolveImageLayout;
         }
      }

      assert(d_iview == NULL || s_iview == NULL || d_iview == s_iview);
      ds_att.iview = d_iview ? d_iview : s_iview, ds_att.format = ds_att.iview->vk.format;

      if (d_iview && s_iview) {
         ds_att_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
      } else if (d_iview) {
         ds_att_aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
      } else {
         ds_att_aspects = VK_IMAGE_ASPECT_STENCIL_BIT;
      }

      radv_initialise_ds_surface(device, &ds_att.ds, ds_att.iview, ds_att_aspects);

      assert(d_res_iview == NULL || s_res_iview == NULL || d_res_iview == s_res_iview);
      ds_att.resolve_iview = d_res_iview ? d_res_iview : s_res_iview;

      ds_samples = ds_att.iview->vk.image->samples;

      if (initial_depth_layout != ds_att.layout || initial_stencil_layout != ds_att.stencil_layout) {
         assert(!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT));
         radv_handle_rendering_image_transition(cmd_buffer, ds_att.iview, pRenderingInfo->layerCount,
                                                pRenderingInfo->viewMask, initial_depth_layout, initial_stencil_layout,
                                                ds_att.layout, ds_att.stencil_layout, &sample_locations);
      }
   }
   if (cmd_buffer->vk.render_pass)
      radv_describe_barrier_end(cmd_buffer);

   const VkRenderingFragmentShadingRateAttachmentInfoKHR *fsr_info =
      vk_find_struct_const(pRenderingInfo->pNext, RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
   struct radv_attachment vrs_att = {.iview = NULL};
   VkExtent2D vrs_texel_size = {.width = 0};
   if (fsr_info && fsr_info->imageView) {
      VK_FROM_HANDLE(radv_image_view, iview, fsr_info->imageView);
      vrs_att = (struct radv_attachment){
         .format = iview->vk.format,
         .iview = iview,
         .layout = fsr_info->imageLayout,
      };
      vrs_texel_size = fsr_info->shadingRateAttachmentTexelSize;
   }

   /* Now that we've done any layout transitions which may invoke meta, we can
    * fill out the actual rendering info and set up for the client's render pass.
    */
   radv_cmd_buffer_reset_rendering(cmd_buffer);

   struct radv_rendering_state *render = &cmd_buffer->state.render;
   render->active = true;
   render->has_image_views = true;
   render->area = pRenderingInfo->renderArea;
   render->view_mask = pRenderingInfo->viewMask;
   render->layer_count = pRenderingInfo->layerCount;
   render->color_samples = color_samples;
   render->ds_samples = ds_samples;
   render->max_samples = MAX2(color_samples, ds_samples);
   render->sample_locations = sample_locations;
   render->color_att_count = pRenderingInfo->colorAttachmentCount;
   typed_memcpy(render->color_att, color_att, render->color_att_count);
   render->ds_att = ds_att;
   render->ds_att_aspects = ds_att_aspects;
   render->vrs_att = vrs_att;
   render->vrs_texel_size = vrs_texel_size;
   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;

   if (pdev->info.rbplus_allowed)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;

   cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_DEPTH_BIAS | RADV_DYNAMIC_STENCIL_TEST_ENABLE;
   if (pdev->info.gfx_level >= GFX12)
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;

   if (render->vrs_att.iview && pdev->info.gfx_level == GFX10_3) {
      if (render->ds_att.iview &&
          radv_htile_enabled(render->ds_att.iview->image, render->ds_att.iview->vk.base_mip_level)) {
         /* When we have a VRS attachment and a depth/stencil attachment, we just need to copy the
          * VRS rates to the HTILE buffer of the attachment.
          */
         struct radv_image_view *ds_iview = render->ds_att.iview;
         struct radv_image *ds_image = ds_iview->image;
         uint32_t level = ds_iview->vk.base_mip_level;

         /* HTILE buffer */
         uint64_t htile_offset = ds_image->bindings[0].offset + ds_image->planes[0].surface.meta_offset +
                                 ds_image->planes[0].surface.u.gfx9.meta_levels[level].offset;
         uint64_t htile_size = ds_image->planes[0].surface.u.gfx9.meta_levels[level].size;
         struct radv_buffer htile_buffer;

         radv_buffer_init(&htile_buffer, device, ds_image->bindings[0].bo, htile_size, htile_offset);

         assert(render->area.offset.x + render->area.extent.width <= ds_image->vk.extent.width &&
                render->area.offset.x + render->area.extent.height <= ds_image->vk.extent.height);

         /* Copy the VRS rates to the HTILE buffer. */
         radv_copy_vrs_htile(cmd_buffer, render->vrs_att.iview, &render->area, ds_image, &htile_buffer, true);

         radv_buffer_finish(&htile_buffer);
      } else {
         /* When a subpass uses a VRS attachment without binding a depth/stencil attachment, or when
          * HTILE isn't enabled, we use a fallback that copies the VRS rates to our internal HTILE buffer.
          */
         struct radv_image *ds_image = radv_cmd_buffer_get_vrs_image(cmd_buffer);

         if (ds_image && render->area.offset.x < ds_image->vk.extent.width &&
             render->area.offset.y < ds_image->vk.extent.height) {
            /* HTILE buffer */
            struct radv_buffer *htile_buffer = device->vrs.buffer;

            VkRect2D area = render->area;
            area.extent.width = MIN2(area.extent.width, ds_image->vk.extent.width - area.offset.x);
            area.extent.height = MIN2(area.extent.height, ds_image->vk.extent.height - area.offset.y);

            /* Copy the VRS rates to the HTILE buffer. */
            radv_copy_vrs_htile(cmd_buffer, render->vrs_att.iview, &area, ds_image, htile_buffer, false);
         }
      }
   }

   const uint32_t minx = render->area.offset.x;
   const uint32_t miny = render->area.offset.y;
   const uint32_t maxx = minx + render->area.extent.width;
   const uint32_t maxy = miny + render->area.extent.height;

   radeon_check_space(device->ws, cmd_buffer->cs, 6);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028204_PA_SC_WINDOW_SCISSOR_TL,
                             S_028204_TL_X(minx) | S_028204_TL_Y_GFX12(miny));
      radeon_set_context_reg(cmd_buffer->cs, R_028208_PA_SC_WINDOW_SCISSOR_BR,
                             S_028208_BR_X(maxx - 1) | S_028208_BR_Y(maxy - 1)); /* inclusive */
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028204_PA_SC_WINDOW_SCISSOR_TL,
                             S_028204_TL_X(minx) | S_028204_TL_Y_GFX6(miny));
      radeon_set_context_reg(cmd_buffer->cs, R_028208_PA_SC_WINDOW_SCISSOR_BR,
                             S_028208_BR_X(maxx) | S_028208_BR_Y(maxy));
   }

   radv_emit_fb_mip_change_flush(cmd_buffer);

   if (!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT))
      radv_cmd_buffer_clear_rendering(cmd_buffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndRendering(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_mark_noncoherent_rb(cmd_buffer);
   radv_cmd_buffer_resolve_rendering(cmd_buffer);
   radv_cmd_buffer_reset_rendering(cmd_buffer);
}

static void
radv_emit_view_index_per_stage(struct radeon_cmdbuf *cs, const struct radv_shader *shader, uint32_t base_reg,
                               unsigned index)
{
   const uint32_t view_index_offset = radv_get_user_sgpr_loc(shader, AC_UD_VIEW_INDEX);

   if (!view_index_offset)
      return;

   radeon_set_sh_reg(cs, view_index_offset, index);
}

static void
radv_emit_view_index(const struct radv_cmd_state *cmd_state, struct radeon_cmdbuf *cs, unsigned index)
{
   radv_foreach_stage(stage, cmd_state->active_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
   {
      const struct radv_shader *shader = radv_get_shader(cmd_state->shaders, stage);

      radv_emit_view_index_per_stage(cs, shader, shader->info.user_data_0, index);
   }

   if (cmd_state->gs_copy_shader) {
      radv_emit_view_index_per_stage(cs, cmd_state->gs_copy_shader, R_00B130_SPI_SHADER_USER_DATA_VS_0, index);
   }
}

/**
 * Emulates predication for MEC using COND_EXEC.
 * When the current command buffer is predicating, emit a COND_EXEC packet
 * so that the MEC skips the next few dwords worth of packets.
 *
 * To make it work with inverted conditional rendering, we allocate
 * space in the upload BO and emit some packets to invert the condition.
 */
static void
radv_cs_emit_compute_predication(const struct radv_device *device, struct radv_cmd_state *state,
                                 struct radeon_cmdbuf *cs, uint64_t inv_va, bool *inv_emitted, unsigned dwords)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!state->predicating)
      return;

   uint64_t va = state->predication_va;

   if (!state->predication_type) {
      /* Invert the condition the first time it is needed. */
      if (!*inv_emitted) {
         const enum amd_gfx_level gfx_level = pdev->info.gfx_level;

         *inv_emitted = true;

         /* Write 1 to the inverted predication VA. */
         radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
         radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                            COPY_DATA_WR_CONFIRM | (gfx_level == GFX6 ? COPY_DATA_ENGINE_PFP : 0));
         radeon_emit(cs, 1);
         radeon_emit(cs, 0);
         radeon_emit(cs, inv_va);
         radeon_emit(cs, inv_va >> 32);

         /* If the API predication VA == 0, skip next command. */
         radv_emit_cond_exec(device, cs, va, 6 /* 1x COPY_DATA size */);

         /* Write 0 to the new predication VA (when the API condition != 0) */
         radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
         radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                            COPY_DATA_WR_CONFIRM | (gfx_level == GFX6 ? COPY_DATA_ENGINE_PFP : 0));
         radeon_emit(cs, 0);
         radeon_emit(cs, 0);
         radeon_emit(cs, inv_va);
         radeon_emit(cs, inv_va >> 32);
      }

      va = inv_va;
   }

   radv_emit_cond_exec(device, cs, va, dwords);
}

static void
radv_cs_emit_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t vertex_count, uint32_t use_opaque)
{
   radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_INDEX_AUTO, 1, cmd_buffer->state.predicating));
   radeon_emit(cmd_buffer->cs, vertex_count);
   radeon_emit(cmd_buffer->cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX | use_opaque);
}

/**
 * Emit a PKT3_DRAW_INDEX_2 packet to render "index_count` vertices.
 *
 * The starting address "index_va" may point anywhere within the index buffer. The number of
 * indexes allocated in the index buffer *past that point* is specified by "max_index_count".
 * Hardware uses this information to return 0 for out-of-bounds reads.
 */
static void
radv_cs_emit_draw_indexed_packet(struct radv_cmd_buffer *cmd_buffer, uint64_t index_va, uint32_t max_index_count,
                                 uint32_t index_count, bool not_eop)
{
   radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_INDEX_2, 4, cmd_buffer->state.predicating));
   radeon_emit(cmd_buffer->cs, max_index_count);
   radeon_emit(cmd_buffer->cs, index_va);
   radeon_emit(cmd_buffer->cs, index_va >> 32);
   radeon_emit(cmd_buffer->cs, index_count);
   /* NOT_EOP allows merging multiple draws into 1 wave, but only user VGPRs
    * can be changed between draws and GS fast launch must be disabled.
    * NOT_EOP doesn't work on gfx6-gfx9 and gfx12.
    */
   radeon_emit(cmd_buffer->cs, V_0287F0_DI_SRC_SEL_DMA | S_0287F0_NOT_EOP(not_eop));
}

/* MUST inline this function to avoid massive perf loss in drawoverhead */
ALWAYS_INLINE static void
radv_cs_emit_indirect_draw_packet(struct radv_cmd_buffer *cmd_buffer, bool indexed, uint32_t draw_count,
                                  uint64_t count_va, uint32_t stride)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const unsigned di_src_sel = indexed ? V_0287F0_DI_SRC_SEL_DMA : V_0287F0_DI_SRC_SEL_AUTO_INDEX;
   bool draw_id_enable = cmd_buffer->state.uses_drawid;
   uint32_t base_reg = cmd_buffer->state.vtx_base_sgpr;
   uint32_t vertex_offset_reg, start_instance_reg = 0, draw_id_reg = 0;
   bool predicating = cmd_buffer->state.predicating;
   assert(base_reg);

   /* just reset draw state for vertex data */
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_vertex_offset_valid = false;

   vertex_offset_reg = (base_reg - SI_SH_REG_OFFSET) >> 2;
   if (cmd_buffer->state.uses_baseinstance)
      start_instance_reg = ((base_reg + (draw_id_enable ? 8 : 4)) - SI_SH_REG_OFFSET) >> 2;
   if (draw_id_enable)
      draw_id_reg = ((base_reg + 4) - SI_SH_REG_OFFSET) >> 2;

   if (draw_count == 1 && !count_va && !draw_id_enable) {
      radeon_emit(cs, PKT3(indexed ? PKT3_DRAW_INDEX_INDIRECT : PKT3_DRAW_INDIRECT, 3, predicating));
      radeon_emit(cs, 0);
      radeon_emit(cs, vertex_offset_reg);
      radeon_emit(cs, start_instance_reg);
      radeon_emit(cs, di_src_sel);
   } else {
      radeon_emit(cs, PKT3(indexed ? PKT3_DRAW_INDEX_INDIRECT_MULTI : PKT3_DRAW_INDIRECT_MULTI, 8, predicating));
      radeon_emit(cs, 0);
      radeon_emit(cs, vertex_offset_reg);
      radeon_emit(cs, start_instance_reg);
      radeon_emit(cs, draw_id_reg | S_2C3_DRAW_INDEX_ENABLE(draw_id_enable) | S_2C3_COUNT_INDIRECT_ENABLE(!!count_va));
      radeon_emit(cs, draw_count); /* count */
      radeon_emit(cs, count_va);   /* count_addr */
      radeon_emit(cs, count_va >> 32);
      radeon_emit(cs, stride); /* stride */
      radeon_emit(cs, di_src_sel);
   }

   cmd_buffer->state.uses_draw_indirect = true;
}

ALWAYS_INLINE static void
radv_cs_emit_indirect_mesh_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t draw_count, uint64_t count_va,
                                       uint32_t stride)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *mesh_shader = cmd_buffer->state.shaders[MESA_SHADER_MESH];
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t base_reg = cmd_buffer->state.vtx_base_sgpr;
   bool predicating = cmd_buffer->state.predicating;
   assert(base_reg || (!cmd_buffer->state.uses_drawid && !mesh_shader->info.cs.uses_grid_size));

   /* Reset draw state. */
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_vertex_offset_valid = false;

   uint32_t xyz_dim_enable = mesh_shader->info.cs.uses_grid_size;
   uint32_t xyz_dim_reg = !xyz_dim_enable ? 0 : (base_reg - SI_SH_REG_OFFSET) >> 2;
   uint32_t draw_id_enable = !!cmd_buffer->state.uses_drawid;
   uint32_t draw_id_reg = !draw_id_enable ? 0 : (base_reg + (xyz_dim_enable ? 12 : 0) - SI_SH_REG_OFFSET) >> 2;

   uint32_t mode1_enable = !pdev->mesh_fast_launch_2;

   radeon_emit(cs, PKT3(PKT3_DISPATCH_MESH_INDIRECT_MULTI, 7, predicating) | PKT3_RESET_FILTER_CAM_S(1));
   radeon_emit(cs, 0); /* data_offset */
   radeon_emit(cs, S_4C1_XYZ_DIM_REG(xyz_dim_reg) | S_4C1_DRAW_INDEX_REG(draw_id_reg));
   if (pdev->info.gfx_level >= GFX11)
      radeon_emit(cs, S_4C2_DRAW_INDEX_ENABLE(draw_id_enable) | S_4C2_COUNT_INDIRECT_ENABLE(!!count_va) |
                         S_4C2_XYZ_DIM_ENABLE(xyz_dim_enable) | S_4C2_MODE1_ENABLE(mode1_enable));
   else
      radeon_emit(cs, S_4C2_DRAW_INDEX_ENABLE(draw_id_enable) | S_4C2_COUNT_INDIRECT_ENABLE(!!count_va));
   radeon_emit(cs, draw_count);
   radeon_emit(cs, count_va);
   radeon_emit(cs, count_va >> 32);
   radeon_emit(cs, stride);
   radeon_emit(cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX);
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_direct_ace_packet(const struct radv_device *device,
                                                 const struct radv_cmd_state *cmd_state, struct radeon_cmdbuf *ace_cs,
                                                 const uint32_t x, const uint32_t y, const uint32_t z)
{
   const struct radv_shader *task_shader = cmd_state->shaders[MESA_SHADER_TASK];
   const bool predicating = cmd_state->predicating;
   const uint32_t dispatch_initiator =
      device->dispatch_initiator_task | S_00B800_CS_W32_EN(task_shader->info.wave_size == 32);
   const uint32_t ring_entry_reg = radv_get_user_sgpr(task_shader, AC_UD_TASK_RING_ENTRY);

   radeon_emit(ace_cs, PKT3(PKT3_DISPATCH_TASKMESH_DIRECT_ACE, 4, predicating) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(ace_cs, x);
   radeon_emit(ace_cs, y);
   radeon_emit(ace_cs, z);
   radeon_emit(ace_cs, dispatch_initiator);
   radeon_emit(ace_cs, ring_entry_reg & 0xFFFF);
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(const struct radv_device *device,
                                                         const struct radv_cmd_state *cmd_state,
                                                         struct radeon_cmdbuf *ace_cs, uint64_t data_va,
                                                         uint32_t draw_count, uint64_t count_va, uint32_t stride)
{
   assert((data_va & 0x03) == 0);
   assert((count_va & 0x03) == 0);

   const struct radv_shader *task_shader = cmd_state->shaders[MESA_SHADER_TASK];

   const uint32_t dispatch_initiator =
      device->dispatch_initiator_task | S_00B800_CS_W32_EN(task_shader->info.wave_size == 32);
   const uint32_t ring_entry_reg = radv_get_user_sgpr(task_shader, AC_UD_TASK_RING_ENTRY);
   const uint32_t xyz_dim_reg = radv_get_user_sgpr(task_shader, AC_UD_CS_GRID_SIZE);
   const uint32_t draw_id_reg = radv_get_user_sgpr(task_shader, AC_UD_CS_TASK_DRAW_ID);

   radeon_emit(ace_cs, PKT3(PKT3_DISPATCH_TASKMESH_INDIRECT_MULTI_ACE, 9, 0) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(ace_cs, data_va);
   radeon_emit(ace_cs, data_va >> 32);
   radeon_emit(ace_cs, S_AD2_RING_ENTRY_REG(ring_entry_reg));
   radeon_emit(ace_cs, S_AD3_COUNT_INDIRECT_ENABLE(!!count_va) | S_AD3_DRAW_INDEX_ENABLE(!!draw_id_reg) |
                          S_AD3_XYZ_DIM_ENABLE(!!xyz_dim_reg) | S_AD3_DRAW_INDEX_REG(draw_id_reg));
   radeon_emit(ace_cs, S_AD4_XYZ_DIM_REG(xyz_dim_reg));
   radeon_emit(ace_cs, draw_count);
   radeon_emit(ace_cs, count_va);
   radeon_emit(ace_cs, count_va >> 32);
   radeon_emit(ace_cs, stride);
   radeon_emit(ace_cs, dispatch_initiator);
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_gfx_packet(const struct radv_device *device, const struct radv_cmd_state *cmd_state,
                                          struct radeon_cmdbuf *cs)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *mesh_shader = cmd_state->shaders[MESA_SHADER_MESH];
   const bool predicating = cmd_state->predicating;

   const uint32_t ring_entry_reg = radv_get_user_sgpr(mesh_shader, AC_UD_TASK_RING_ENTRY);

   uint32_t xyz_dim_en = mesh_shader->info.cs.uses_grid_size;
   uint32_t xyz_dim_reg = !xyz_dim_en ? 0 : (cmd_state->vtx_base_sgpr - SI_SH_REG_OFFSET) >> 2;
   uint32_t mode1_en = !pdev->mesh_fast_launch_2;
   uint32_t linear_dispatch_en = cmd_state->shaders[MESA_SHADER_TASK]->info.cs.linear_taskmesh_dispatch;
   const bool sqtt_en = !!device->sqtt.bo;

   radeon_emit(cs, PKT3(PKT3_DISPATCH_TASKMESH_GFX, 2, predicating) | PKT3_RESET_FILTER_CAM_S(1));
   radeon_emit(cs, S_4D0_RING_ENTRY_REG(ring_entry_reg) | S_4D0_XYZ_DIM_REG(xyz_dim_reg));
   if (pdev->info.gfx_level >= GFX11)
      radeon_emit(cs, S_4D1_XYZ_DIM_ENABLE(xyz_dim_en) | S_4D1_MODE1_ENABLE(mode1_en) |
                         S_4D1_LINEAR_DISPATCH_ENABLE(linear_dispatch_en) | S_4D1_THREAD_TRACE_MARKER_ENABLE(sqtt_en));
   else
      radeon_emit(cs, S_4D1_THREAD_TRACE_MARKER_ENABLE(sqtt_en));
   radeon_emit(cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX);
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex_internal(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                                   const uint32_t vertex_offset)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const bool uses_baseinstance = state->uses_baseinstance;
   const bool uses_drawid = state->uses_drawid;

   radeon_set_sh_reg_seq(cs, state->vtx_base_sgpr, state->vtx_emit_num);

   radeon_emit(cs, vertex_offset);
   state->last_vertex_offset_valid = true;
   state->last_vertex_offset = vertex_offset;
   if (uses_drawid) {
      radeon_emit(cs, 0);
      state->last_drawid = 0;
   }
   if (uses_baseinstance) {
      radeon_emit(cs, info->first_instance);
      state->last_first_instance = info->first_instance;
   }
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                          const uint32_t vertex_offset)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   const bool uses_baseinstance = state->uses_baseinstance;
   const bool uses_drawid = state->uses_drawid;

   if (!state->last_vertex_offset_valid || vertex_offset != state->last_vertex_offset ||
       (uses_drawid && 0 != state->last_drawid) ||
       (uses_baseinstance && info->first_instance != state->last_first_instance))
      radv_emit_userdata_vertex_internal(cmd_buffer, info, vertex_offset);
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex_drawid(struct radv_cmd_buffer *cmd_buffer, uint32_t vertex_offset, uint32_t drawid)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   radeon_set_sh_reg_seq(cs, state->vtx_base_sgpr, 1 + !!drawid);
   radeon_emit(cs, vertex_offset);
   state->last_vertex_offset_valid = true;
   state->last_vertex_offset = vertex_offset;
   if (drawid)
      radeon_emit(cs, drawid);
}

ALWAYS_INLINE static void
radv_emit_userdata_mesh(struct radv_cmd_buffer *cmd_buffer, const uint32_t x, const uint32_t y, const uint32_t z)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   const struct radv_shader *mesh_shader = state->shaders[MESA_SHADER_MESH];
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const bool uses_drawid = state->uses_drawid;
   const bool uses_grid_size = mesh_shader->info.cs.uses_grid_size;

   if (!uses_drawid && !uses_grid_size)
      return;

   radeon_set_sh_reg_seq(cs, state->vtx_base_sgpr, state->vtx_emit_num);
   if (uses_grid_size) {
      radeon_emit(cs, x);
      radeon_emit(cs, y);
      radeon_emit(cs, z);
   }
   if (uses_drawid) {
      radeon_emit(cs, 0);
      state->last_drawid = 0;
   }
}

ALWAYS_INLINE static void
radv_emit_userdata_task(const struct radv_cmd_state *cmd_state, struct radeon_cmdbuf *ace_cs, uint32_t x, uint32_t y,
                        uint32_t z)
{
   const struct radv_shader *task_shader = cmd_state->shaders[MESA_SHADER_TASK];

   const uint32_t xyz_offset = radv_get_user_sgpr_loc(task_shader, AC_UD_CS_GRID_SIZE);
   const uint32_t draw_id_offset = radv_get_user_sgpr_loc(task_shader, AC_UD_CS_TASK_DRAW_ID);

   if (xyz_offset) {
      radeon_set_sh_reg_seq(ace_cs, xyz_offset, 3);
      radeon_emit(ace_cs, x);
      radeon_emit(ace_cs, y);
      radeon_emit(ace_cs, z);
   }

   if (draw_id_offset) {
      radeon_set_sh_reg_seq(ace_cs, draw_id_offset, 1);
      radeon_emit(ace_cs, 0);
   }
}

ALWAYS_INLINE static void
radv_emit_draw_packets_indexed(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                               uint32_t drawCount, const VkMultiDrawIndexedInfoEXT *minfo, uint32_t stride,
                               const int32_t *vertexOffset)

{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const int index_size = radv_get_vgt_index_size(state->index_type);
   unsigned i = 0;
   const bool uses_drawid = state->uses_drawid;
   const bool can_eop = !uses_drawid && pdev->info.gfx_level >= GFX10 && pdev->info.gfx_level < GFX12;

   if (uses_drawid) {
      if (vertexOffset) {
         radv_emit_userdata_vertex(cmd_buffer, info, *vertexOffset);
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            if (i > 0)
               radeon_set_sh_reg(cs, state->vtx_base_sgpr + sizeof(uint32_t), i);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      } else {
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            if (i > 0) {
               assert(state->last_vertex_offset_valid);
               if (state->last_vertex_offset != draw->vertexOffset)
                  radv_emit_userdata_vertex_drawid(cmd_buffer, draw->vertexOffset, i);
               else
                  radeon_set_sh_reg(cs, state->vtx_base_sgpr + sizeof(uint32_t), i);
            } else
               radv_emit_userdata_vertex(cmd_buffer, info, draw->vertexOffset);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      }
      if (drawCount > 1) {
         state->last_drawid = drawCount - 1;
      }
   } else {
      if (vertexOffset) {
         if (pdev->info.gfx_level == GFX10) {
            /* GFX10 has a bug that consecutive draw packets with NOT_EOP must not have
             * count == 0 for the last draw that doesn't have NOT_EOP.
             */
            while (drawCount > 1) {
               const VkMultiDrawIndexedInfoEXT *last =
                  (const VkMultiDrawIndexedInfoEXT *)(((const uint8_t *)minfo) + (drawCount - 1) * stride);
               if (last->indexCount)
                  break;
               drawCount--;
            }
         }

         radv_emit_userdata_vertex(cmd_buffer, info, *vertexOffset);
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount,
                                                can_eop && i < drawCount - 1);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      } else {
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            const VkMultiDrawIndexedInfoEXT *next =
               (const VkMultiDrawIndexedInfoEXT *)(i < drawCount - 1 ? ((uint8_t *)draw + stride) : NULL);
            const bool offset_changes = next && next->vertexOffset != draw->vertexOffset;
            radv_emit_userdata_vertex(cmd_buffer, info, draw->vertexOffset);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount,
                                                can_eop && !offset_changes && i < drawCount - 1);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      }
      if (drawCount > 1) {
         state->last_drawid = drawCount - 1;
      }
   }
}

ALWAYS_INLINE static void
radv_emit_direct_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount,
                              const VkMultiDrawInfoEXT *minfo, uint32_t use_opaque, uint32_t stride)
{
   unsigned i = 0;
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   const bool uses_drawid = cmd_buffer->state.uses_drawid;
   uint32_t last_start = 0;

   vk_foreach_multi_draw (draw, i, minfo, drawCount, stride) {
      if (!i)
         radv_emit_userdata_vertex(cmd_buffer, info, draw->firstVertex);
      else
         radv_emit_userdata_vertex_drawid(cmd_buffer, draw->firstVertex, uses_drawid ? i : 0);

      if (!view_mask) {
         radv_cs_emit_draw_packet(cmd_buffer, draw->vertexCount, use_opaque);
      } else {
         u_foreach_bit (view, view_mask) {
            radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);
            radv_cs_emit_draw_packet(cmd_buffer, draw->vertexCount, use_opaque);
         }
      }
      last_start = draw->firstVertex;
   }
   if (drawCount > 1) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      assert(state->last_vertex_offset_valid);
      state->last_vertex_offset = last_start;
      if (uses_drawid)
         state->last_drawid = drawCount - 1;
   }
}

static void
radv_cs_emit_mesh_dispatch_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   radeon_emit(cmd_buffer->cs, PKT3(PKT3_DISPATCH_MESH_DIRECT, 3, cmd_buffer->state.predicating));
   radeon_emit(cmd_buffer->cs, x);
   radeon_emit(cmd_buffer->cs, y);
   radeon_emit(cmd_buffer->cs, z);
   radeon_emit(cmd_buffer->cs, S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX));
}

ALWAYS_INLINE static void
radv_emit_direct_mesh_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;

   radv_emit_userdata_mesh(cmd_buffer, x, y, z);

   if (pdev->mesh_fast_launch_2) {
      if (!view_mask) {
         radv_cs_emit_mesh_dispatch_packet(cmd_buffer, x, y, z);
      } else {
         u_foreach_bit (view, view_mask) {
            radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);
            radv_cs_emit_mesh_dispatch_packet(cmd_buffer, x, y, z);
         }
      }
   } else {
      const uint32_t count = x * y * z;
      if (!view_mask) {
         radv_cs_emit_draw_packet(cmd_buffer, count, 0);
      } else {
         u_foreach_bit (view, view_mask) {
            radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);
            radv_cs_emit_draw_packet(cmd_buffer, count, 0);
         }
      }
   }
}

ALWAYS_INLINE static void
radv_emit_indirect_mesh_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_winsys *ws = device->ws;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const uint64_t va = radv_buffer_get_va(info->indirect->bo) + info->indirect->offset + info->indirect_offset;
   const uint64_t count_va = !info->count_buffer ? 0
                                                 : radv_buffer_get_va(info->count_buffer->bo) +
                                                      info->count_buffer->offset + info->count_buffer_offset;

   radv_cs_add_buffer(ws, cs, info->indirect->bo);

   if (info->count_buffer) {
      radv_cs_add_buffer(ws, cs, info->count_buffer->bo);
   }

   radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0));
   radeon_emit(cs, 1);
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);

   if (state->uses_drawid) {
      const struct radv_shader *mesh_shader = state->shaders[MESA_SHADER_MESH];
      unsigned reg = state->vtx_base_sgpr + (mesh_shader->info.cs.uses_grid_size ? 12 : 0);
      radeon_set_sh_reg_seq(cs, reg, 1);
      radeon_emit(cs, 0);
   }

   if (!state->render.view_mask) {
      radv_cs_emit_indirect_mesh_draw_packet(cmd_buffer, info->count, count_va, info->stride);
   } else {
      u_foreach_bit (i, state->render.view_mask) {
         radv_emit_view_index(&cmd_buffer->state, cs, i);
         radv_cs_emit_indirect_mesh_draw_packet(cmd_buffer, info->count, count_va, info->stride);
      }
   }
}

ALWAYS_INLINE static void
radv_emit_direct_taskmesh_draw_packets(const struct radv_device *device, struct radv_cmd_state *cmd_state,
                                       struct radeon_cmdbuf *cs, struct radeon_cmdbuf *ace_cs, uint32_t x, uint32_t y,
                                       uint32_t z)
{
   const uint32_t view_mask = cmd_state->render.view_mask;
   const unsigned num_views = MAX2(1, util_bitcount(view_mask));
   const unsigned ace_predication_size = num_views * 6; /* DISPATCH_TASKMESH_DIRECT_ACE size */

   radv_emit_userdata_task(cmd_state, ace_cs, x, y, z);
   radv_cs_emit_compute_predication(device, cmd_state, ace_cs, cmd_state->mec_inv_pred_va,
                                    &cmd_state->mec_inv_pred_emitted, ace_predication_size);

   if (!view_mask) {
      radv_cs_emit_dispatch_taskmesh_direct_ace_packet(device, cmd_state, ace_cs, x, y, z);
      radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_state, cs, view);

         radv_cs_emit_dispatch_taskmesh_direct_ace_packet(device, cmd_state, ace_cs, x, y, z);
         radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
      }
   }
}

static void
radv_emit_indirect_taskmesh_draw_packets(const struct radv_device *device, struct radv_cmd_state *cmd_state,
                                         struct radeon_cmdbuf *cs, struct radeon_cmdbuf *ace_cs,
                                         const struct radv_draw_info *info, uint64_t workaround_cond_va)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t view_mask = cmd_state->render.view_mask;
   struct radeon_winsys *ws = device->ws;
   const unsigned num_views = MAX2(1, util_bitcount(view_mask));
   unsigned ace_predication_size = num_views * 11; /* DISPATCH_TASKMESH_INDIRECT_MULTI_ACE size */

   const uint64_t va = radv_buffer_get_va(info->indirect->bo) + info->indirect->offset + info->indirect_offset;
   const uint64_t count_va = !info->count_buffer ? 0
                                                 : radv_buffer_get_va(info->count_buffer->bo) +
                                                      info->count_buffer->offset + info->count_buffer_offset;

   if (count_va)
      radv_cs_add_buffer(ws, ace_cs, info->count_buffer->bo);

   if (pdev->info.has_taskmesh_indirect0_bug && count_va) {
      /* MEC firmware bug workaround.
       * When the count buffer contains zero, DISPATCH_TASKMESH_INDIRECT_MULTI_ACE hangs.
       * - We must ensure that DISPATCH_TASKMESH_INDIRECT_MULTI_ACE
       *   is only executed when the count buffer contains non-zero.
       * - Furthermore, we must also ensure that each DISPATCH_TASKMESH_GFX packet
       *   has a matching ACE packet.
       *
       * As a workaround:
       * - Reserve a dword in the upload buffer and initialize it to 1 for the workaround
       * - When count != 0, write 0 to the workaround BO and execute the indirect dispatch
       * - When workaround BO != 0 (count was 0), execute an empty direct dispatch
       */
      radeon_emit(ace_cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(ace_cs,
                  COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM);
      radeon_emit(ace_cs, 1);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, workaround_cond_va);
      radeon_emit(ace_cs, workaround_cond_va >> 32);

      /* 2x COND_EXEC + 1x COPY_DATA + Nx DISPATCH_TASKMESH_DIRECT_ACE */
      ace_predication_size += 2 * 5 + 6 + 6 * num_views;
   }

   radv_cs_add_buffer(ws, ace_cs, info->indirect->bo);
   radv_cs_emit_compute_predication(device, cmd_state, ace_cs, cmd_state->mec_inv_pred_va,
                                    &cmd_state->mec_inv_pred_emitted, ace_predication_size);

   if (workaround_cond_va) {
      radv_emit_cond_exec(device, ace_cs, count_va,
                          6 + 11 * num_views /* 1x COPY_DATA + Nx DISPATCH_TASKMESH_INDIRECT_MULTI_ACE */);

      radeon_emit(ace_cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(ace_cs,
                  COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, workaround_cond_va);
      radeon_emit(ace_cs, workaround_cond_va >> 32);
   }

   if (!view_mask) {
      radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(device, cmd_state, ace_cs, va, info->count, count_va,
                                                               info->stride);
      radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_state, cs, view);

         radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(device, cmd_state, ace_cs, va, info->count, count_va,
                                                                  info->stride);
         radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
      }
   }

   if (workaround_cond_va) {
      radv_emit_cond_exec(device, ace_cs, workaround_cond_va, 6 * num_views /* Nx DISPATCH_TASKMESH_DIRECT_ACE */);

      for (unsigned v = 0; v < num_views; ++v) {
         radv_cs_emit_dispatch_taskmesh_direct_ace_packet(device, cmd_state, ace_cs, 0, 0, 0);
      }
   }
}

static void
radv_emit_indirect_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_winsys *ws = device->ws;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const uint64_t va = radv_buffer_get_va(info->indirect->bo) + info->indirect->offset + info->indirect_offset;
   const uint64_t count_va = info->count_buffer ? radv_buffer_get_va(info->count_buffer->bo) +
                                                     info->count_buffer->offset + info->count_buffer_offset
                                                : 0;

   radv_cs_add_buffer(ws, cs, info->indirect->bo);

   radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0));
   radeon_emit(cs, 1);
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);

   if (info->count_buffer) {
      radv_cs_add_buffer(ws, cs, info->count_buffer->bo);
   }

   if (!state->render.view_mask) {
      radv_cs_emit_indirect_draw_packet(cmd_buffer, info->indexed, info->count, count_va, info->stride);
   } else {
      u_foreach_bit (i, state->render.view_mask) {
         radv_emit_view_index(&cmd_buffer->state, cs, i);

         radv_cs_emit_indirect_draw_packet(cmd_buffer, info->indexed, info->count, count_va, info->stride);
      }
   }
}

static uint64_t
radv_get_needed_dynamic_states(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t dynamic_states = RADV_DYNAMIC_ALL;

   if (cmd_buffer->state.graphics_pipeline)
      return cmd_buffer->state.graphics_pipeline->needed_dynamic_state;

   /* Clear unnecessary dynamic states for shader objects. */
   if (!cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL])
      dynamic_states &= ~(RADV_DYNAMIC_PATCH_CONTROL_POINTS | RADV_DYNAMIC_TESS_DOMAIN_ORIGIN);

   if (pdev->info.gfx_level >= GFX10_3) {
      if (cmd_buffer->state.shaders[MESA_SHADER_MESH])
         dynamic_states &= ~(RADV_DYNAMIC_VERTEX_INPUT | RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE |
                             RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY);
   } else {
      dynamic_states &= ~RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
   }

   return dynamic_states;
}

/*
 * Vega and raven have a bug which triggers if there are multiple context
 * register contexts active at the same time with different scissor values.
 *
 * There are two possible workarounds:
 * 1) Wait for PS_PARTIAL_FLUSH every time the scissor is changed. That way
 *    there is only ever 1 active set of scissor values at the same time.
 *
 * 2) Whenever the hardware switches contexts we have to set the scissor
 *    registers again even if it is a noop. That way the new context gets
 *    the correct scissor values.
 *
 * This implements option 2. radv_need_late_scissor_emission needs to
 * return true on affected HW if radv_emit_all_graphics_states sets
 * any context registers.
 */
static bool
radv_need_late_scissor_emission(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   if (cmd_buffer->state.context_roll_without_scissor_emitted || info->strmout_buffer)
      return true;

   uint64_t used_dynamic_states = radv_get_needed_dynamic_states(cmd_buffer);

   used_dynamic_states &= ~RADV_DYNAMIC_VERTEX_INPUT;

   if (cmd_buffer->state.dirty_dynamic & used_dynamic_states)
      return true;

   /* Index, vertex and streamout buffers don't change context regs.
    * We assume that any other dirty flag causes context rolls.
    */
   uint64_t used_states = RADV_CMD_DIRTY_ALL;
   used_states &= ~(RADV_CMD_DIRTY_INDEX_BUFFER | RADV_CMD_DIRTY_VERTEX_BUFFER | RADV_CMD_DIRTY_STREAMOUT_BUFFER);

   return cmd_buffer->state.dirty & used_states;
}

ALWAYS_INLINE static uint32_t
radv_get_ngg_culling_settings(struct radv_cmd_buffer *cmd_buffer, bool vp_y_inverted)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   /* Disable shader culling entirely when conservative overestimate is used.
    * The face culling algorithm can delete very tiny triangles (even if unintended).
    */
   if (d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT)
      return radv_nggc_none;

   /* With graphics pipeline library, NGG culling is unconditionally compiled into shaders
    * because we don't know the primitive topology at compile time, so we should
    * disable it dynamically for points or lines.
    */
   const unsigned num_vertices_per_prim = radv_conv_prim_to_gs_out(d->vk.ia.primitive_topology, true) + 1;
   if (num_vertices_per_prim != 3)
      return radv_nggc_none;

   /* Cull every triangle when rasterizer discard is enabled. */
   if (d->vk.rs.rasterizer_discard_enable)
      return radv_nggc_front_face | radv_nggc_back_face;

   uint32_t nggc_settings = radv_nggc_none;

   /* The culling code needs to know whether face is CW or CCW. */
   bool ccw = d->vk.rs.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;

   /* Take inverted viewport into account. */
   ccw ^= vp_y_inverted;

   if (ccw)
      nggc_settings |= radv_nggc_face_is_ccw;

   /* Face culling settings. */
   if (d->vk.rs.cull_mode & VK_CULL_MODE_FRONT_BIT)
      nggc_settings |= radv_nggc_front_face;
   if (d->vk.rs.cull_mode & VK_CULL_MODE_BACK_BIT)
      nggc_settings |= radv_nggc_back_face;

   /* Small primitive culling assumes a sample position at (0.5, 0.5)
    * so don't enable it with user sample locations.
    */
   if (!d->vk.ms.sample_locations_enable) {
      nggc_settings |= radv_nggc_small_primitives;

      /* small_prim_precision = num_samples / 2^subpixel_bits
       * num_samples is also always a power of two, so the small prim precision can only be
       * a power of two between 2^-2 and 2^-6, therefore it's enough to remember the exponent.
       */
      unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
      unsigned subpixel_bits = 256;
      int32_t small_prim_precision_log2 = util_logbase2(rasterization_samples) - util_logbase2(subpixel_bits);
      nggc_settings |= ((uint32_t)small_prim_precision_log2 << 24u);
   }

   return nggc_settings;
}

static void
radv_emit_ngg_culling_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;

   /* Get viewport transform. */
   float vp_scale[2], vp_translate[2];
   memcpy(vp_scale, cmd_buffer->state.dynamic.hw_vp.xform[0].scale, 2 * sizeof(float));
   memcpy(vp_translate, cmd_buffer->state.dynamic.hw_vp.xform[0].translate, 2 * sizeof(float));
   bool vp_y_inverted = (-vp_scale[1] + vp_translate[1]) > (vp_scale[1] + vp_translate[1]);

   /* Get current culling settings. */
   uint32_t nggc_settings = radv_get_ngg_culling_settings(cmd_buffer, vp_y_inverted);

   if ((cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE) ||
       (cmd_buffer->state.dirty_dynamic & (RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_RASTERIZATION_SAMPLES))) {
      /* Correction for inverted Y */
      if (vp_y_inverted) {
         vp_scale[1] = -vp_scale[1];
         vp_translate[1] = -vp_translate[1];
      }

      /* Correction for number of samples per pixel. */
      for (unsigned i = 0; i < 2; ++i) {
         vp_scale[i] *= (float)cmd_buffer->state.dynamic.vk.ms.rasterization_samples;
         vp_translate[i] *= (float)cmd_buffer->state.dynamic.vk.ms.rasterization_samples;
      }

      uint32_t vp_reg_values[4] = {fui(vp_scale[0]), fui(vp_scale[1]), fui(vp_translate[0]), fui(vp_translate[1])};
      const uint32_t ngg_viewport_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGG_VIEWPORT);
      radeon_set_sh_reg_seq(cmd_buffer->cs, ngg_viewport_offset, 4);
      radeon_emit_array(cmd_buffer->cs, vp_reg_values, 4);
   }

   const uint32_t ngg_culling_settings_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGG_CULLING_SETTINGS);

   radeon_set_sh_reg(cmd_buffer->cs, ngg_culling_settings_offset, nggc_settings);
}

static void
radv_emit_fs_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   if (!ps)
      return;

   const uint32_t ps_state_offset = radv_get_user_sgpr_loc(ps, AC_UD_PS_STATE);
   if (!ps_state_offset)
      return;

   const unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
   const unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   const uint16_t ps_iter_mask = ac_get_ps_iter_mask(ps_iter_samples);
   const unsigned rast_prim = radv_get_rasterization_prim(cmd_buffer);
   const unsigned ps_state = SET_SGPR_FIELD(PS_STATE_NUM_SAMPLES, rasterization_samples) |
                             SET_SGPR_FIELD(PS_STATE_PS_ITER_MASK, ps_iter_mask) |
                             SET_SGPR_FIELD(PS_STATE_LINE_RAST_MODE, radv_get_line_mode(cmd_buffer)) |
                             SET_SGPR_FIELD(PS_STATE_RAST_PRIM, rast_prim);

   radeon_set_sh_reg(cmd_buffer->cs, ps_state_offset, ps_state);
}

static void
radv_emit_db_shader_control(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const bool uses_ds_feedback_loop =
      !!(d->feedback_loop_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
   const unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);

   uint32_t db_shader_control;

   if (ps) {
      db_shader_control = ps->info.regs.ps.db_shader_control;
   } else {
      db_shader_control = S_02880C_CONSERVATIVE_Z_EXPORT(V_02880C_EXPORT_ANY_Z) |
                          S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) |
                          S_02880C_DUAL_QUAD_DISABLE(gpu_info->has_rbplus && !gpu_info->rbplus_allowed);
   }

   /* When a depth/stencil attachment is used inside feedback loops, use LATE_Z to make sure shader invocations read the
    * correct value.
    * Also apply the bug workaround for smoothing (overrasterization) on GFX6.
    */
   if (uses_ds_feedback_loop || (gpu_info->gfx_level == GFX6 &&
                                 radv_get_line_mode(cmd_buffer) == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR))
      db_shader_control = (db_shader_control & C_02880C_Z_ORDER) | S_02880C_Z_ORDER(V_02880C_LATE_Z);

   if (ps && ps->info.ps.pops) {
      /* POPS_OVERLAP_NUM_SAMPLES (OVERRIDE_INTRINSIC_RATE on GFX11, must always be enabled for POPS) controls the
       * interlock granularity.
       * PixelInterlock: 1x.
       * SampleInterlock: MSAA_EXPOSED_SAMPLES (much faster at common edges of adjacent primitives with MSAA).
       */
      if (gpu_info->gfx_level >= GFX11) {
         db_shader_control |= S_02880C_OVERRIDE_INTRINSIC_RATE_ENABLE(1);
         if (ps->info.ps.pops_is_per_sample)
            db_shader_control |= S_02880C_OVERRIDE_INTRINSIC_RATE(util_logbase2(rasterization_samples));
      } else {
         if (ps->info.ps.pops_is_per_sample)
            db_shader_control |= S_02880C_POPS_OVERLAP_NUM_SAMPLES(util_logbase2(rasterization_samples));

         if (gpu_info->has_pops_missed_overlap_bug) {
            radeon_set_context_reg(cmd_buffer->cs, R_028060_DB_DFSM_CONTROL,
                                   S_028060_PUNCHOUT_MODE(V_028060_FORCE_OFF) |
                                      S_028060_POPS_DRAIN_PS_ON_OVERLAP(rasterization_samples >= 8));
         }
      }
   } else if (gpu_info->has_export_conflict_bug && rasterization_samples == 1) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (d->vk.cb.attachments[i].write_mask && d->vk.cb.attachments[i].blend_enable) {
            db_shader_control |= S_02880C_OVERRIDE_INTRINSIC_RATE_ENABLE(1) | S_02880C_OVERRIDE_INTRINSIC_RATE(2);
            break;
         }
      }
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(cmd_buffer, R_02806C_DB_SHADER_CONTROL, RADV_TRACKED_DB_SHADER_CONTROL,
                                 db_shader_control);
   } else {
      radeon_opt_set_context_reg(cmd_buffer, R_02880C_DB_SHADER_CONTROL, RADV_TRACKED_DB_SHADER_CONTROL,
                                 db_shader_control);
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_DB_SHADER_CONTROL;
}

static void
radv_emit_streamout_enable_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   const bool streamout_enabled = radv_is_streamout_enabled(cmd_buffer);
   uint32_t enabled_stream_buffers_mask = 0;

   assert(!pdev->use_ngg_streamout);

   if (streamout_enabled && cmd_buffer->state.last_vgt_shader) {
      const struct radv_shader_info *info = &cmd_buffer->state.last_vgt_shader->info;

      enabled_stream_buffers_mask = info->so.enabled_stream_buffers_mask;

      u_foreach_bit (i, so->enabled_mask) {
         radeon_set_context_reg(cmd_buffer->cs, R_028AD4_VGT_STRMOUT_VTX_STRIDE_0 + 16 * i, info->so.strides[i]);
      }
   }

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028B94_VGT_STRMOUT_CONFIG, 2);
   radeon_emit(cmd_buffer->cs, S_028B94_STREAMOUT_0_EN(streamout_enabled) | S_028B94_RAST_STREAM(0) |
                                  S_028B94_STREAMOUT_1_EN(streamout_enabled) |
                                  S_028B94_STREAMOUT_2_EN(streamout_enabled) |
                                  S_028B94_STREAMOUT_3_EN(streamout_enabled));
   radeon_emit(cmd_buffer->cs, so->hw_enabled_mask & enabled_stream_buffers_mask);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_STREAMOUT_ENABLE;
}

static gl_shader_stage
radv_cmdbuf_get_last_vgt_api_stage(const struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.active_stages & VK_SHADER_STAGE_MESH_BIT_EXT)
      return MESA_SHADER_MESH;

   return util_last_bit(cmd_buffer->state.active_stages & BITFIELD_MASK(MESA_SHADER_FRAGMENT)) - 1;
}

static void
radv_emit_color_output_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   uint32_t col_format_compacted = radv_compact_spi_shader_col_format(cmd_buffer->state.spi_shader_col_format);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg(cmd_buffer->cs, R_028854_CB_SHADER_MASK, cmd_buffer->state.cb_shader_mask);
      radeon_set_context_reg(cmd_buffer->cs, R_028654_SPI_SHADER_COL_FORMAT, col_format_compacted);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_02823C_CB_SHADER_MASK, cmd_buffer->state.cb_shader_mask);
      radeon_set_context_reg(cmd_buffer->cs, R_028714_SPI_SHADER_COL_FORMAT, col_format_compacted);
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_COLOR_OUTPUT;
}

static void
radv_emit_all_graphics_states(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_shader_part *ps_epilog = NULL;

   if (cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT] &&
       cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT]->info.has_epilog) {
      if ((cmd_buffer->state.emitted_graphics_pipeline != cmd_buffer->state.graphics_pipeline ||
           ((cmd_buffer->state.dirty & (RADV_CMD_DIRTY_GRAPHICS_SHADERS | RADV_CMD_DIRTY_FRAMEBUFFER)) ||
            (cmd_buffer->state.dirty_dynamic &
             (RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_COLOR_BLEND_ENABLE | RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE |
              RADV_DYNAMIC_COLOR_BLEND_EQUATION | RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE |
              RADV_DYNAMIC_COLOR_ATTACHMENT_MAP))))) {
         ps_epilog = lookup_ps_epilog(cmd_buffer);
         if (!ps_epilog) {
            vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
         }

         uint32_t col_format = ps_epilog->spi_shader_col_format;
         uint32_t cb_shader_mask = ps_epilog->cb_shader_mask;

         assert(cmd_buffer->state.custom_blend_mode == 0);

         if (radv_needs_null_export_workaround(device, cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT], 0) &&
             !col_format)
            col_format = V_028714_SPI_SHADER_32_R;

         if (cmd_buffer->state.spi_shader_col_format != col_format) {
            cmd_buffer->state.spi_shader_col_format = col_format;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_COLOR_OUTPUT;
            if (pdev->info.rbplus_allowed)
               cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
         }

         if (cmd_buffer->state.cb_shader_mask != cb_shader_mask) {
            cmd_buffer->state.cb_shader_mask = cb_shader_mask;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_COLOR_OUTPUT;
         }
      }
   }

   /* Determine whether GFX9 late scissor workaround should be applied based on:
    * 1. radv_need_late_scissor_emission
    * 2. any dirty dynamic flags that may cause context rolls
    */
   const bool late_scissor_emission =
      pdev->info.has_gfx9_scissor_bug ? radv_need_late_scissor_emission(cmd_buffer, info) : false;

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_RBPLUS)
      radv_emit_rbplus_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_SHADER_QUERY)
      radv_flush_shader_query_state(cmd_buffer);

   if ((cmd_buffer->state.dirty & RADV_CMD_DIRTY_OCCLUSION_QUERY) ||
       (cmd_buffer->state.dirty_dynamic & (RADV_DYNAMIC_RASTERIZATION_SAMPLES | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY)))
      radv_flush_occlusion_query_state(cmd_buffer);

   if (((cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE) ||
        (cmd_buffer->state.dirty_dynamic &
         (RADV_DYNAMIC_CULL_MODE | RADV_DYNAMIC_FRONT_FACE | RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE |
          RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_CONSERVATIVE_RAST_MODE | RADV_DYNAMIC_RASTERIZATION_SAMPLES |
          RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE))) &&
       cmd_buffer->state.has_nggc)
      radv_emit_ngg_culling_state(cmd_buffer);

   if ((cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER) ||
       (cmd_buffer->state.dirty_dynamic &
        (RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_RASTERIZATION_SAMPLES | RADV_DYNAMIC_LINE_RASTERIZATION_MODE |
         RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE)))
      radv_emit_binning_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE) {
      radv_emit_graphics_pipeline(cmd_buffer);
   } else if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GRAPHICS_SHADERS) {
      radv_emit_graphics_shaders(cmd_buffer);
   }

   if (ps_epilog)
      radv_emit_ps_epilog_state(cmd_buffer, ps_epilog);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_COLOR_OUTPUT)
      radv_emit_color_output_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)
      radv_emit_framebuffer_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GUARDBAND)
      radv_emit_guardband_state(cmd_buffer);

   if ((cmd_buffer->state.dirty & RADV_CMD_DIRTY_DB_SHADER_CONTROL) ||
       (cmd_buffer->state.dirty_dynamic &
        (RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_COLOR_BLEND_ENABLE | RADV_DYNAMIC_RASTERIZATION_SAMPLES |
         RADV_DYNAMIC_LINE_RASTERIZATION_MODE | RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE |
         RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE)))
      radv_emit_db_shader_control(cmd_buffer);

   if (info->indexed && info->indirect && cmd_buffer->state.dirty & RADV_CMD_DIRTY_INDEX_BUFFER)
      radv_emit_index_buffer(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_STREAMOUT_ENABLE)
      radv_emit_streamout_enable_state(cmd_buffer);

   const uint64_t dynamic_states = cmd_buffer->state.dirty_dynamic & radv_get_needed_dynamic_states(cmd_buffer);

   if (dynamic_states) {
      radv_cmd_buffer_flush_dynamic_state(cmd_buffer, dynamic_states);

      if (dynamic_states & (RADV_DYNAMIC_RASTERIZATION_SAMPLES | RADV_DYNAMIC_LINE_RASTERIZATION_MODE |
                            RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE))
         radv_emit_fs_state(cmd_buffer);
   }

   radv_emit_draw_registers(cmd_buffer, info);

   if (late_scissor_emission) {
      radv_emit_scissor(cmd_buffer);
      cmd_buffer->state.context_roll_without_scissor_emitted = false;
   }
}

static void
radv_bind_graphics_shaders(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t push_constant_size = 0, dynamic_offset_count = 0;
   bool need_indirect_descriptor_sets = false;

   for (unsigned s = 0; s <= MESA_SHADER_MESH; s++) {
      const struct radv_shader_object *shader_obj = cmd_buffer->state.shader_objs[s];
      struct radv_shader *shader = NULL;

      if (s == MESA_SHADER_COMPUTE)
         continue;

      if (!shader_obj) {
         radv_bind_shader(cmd_buffer, NULL, s);
         continue;
      }

      /* Select shader variants. */
      if (s == MESA_SHADER_VERTEX && (cmd_buffer->state.shader_objs[MESA_SHADER_TESS_CTRL] ||
                                      cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY])) {
         if (cmd_buffer->state.shader_objs[MESA_SHADER_TESS_CTRL]) {
            shader = shader_obj->as_ls.shader;
         } else {
            shader = shader_obj->as_es.shader;
         }
      } else if (s == MESA_SHADER_TESS_EVAL && cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY]) {
         shader = shader_obj->as_es.shader;
      } else {
         shader = shader_obj->shader;
      }

      radv_bind_shader(cmd_buffer, shader, s);
      if (!shader)
         continue;

      /* Compute push constants/indirect descriptors state. */
      need_indirect_descriptor_sets |= radv_get_user_sgpr_info(shader, AC_UD_INDIRECT_DESCRIPTOR_SETS)->sgpr_idx != -1;
      push_constant_size += shader_obj->push_constant_size;
      dynamic_offset_count += shader_obj->dynamic_offset_count;
   }

   /* Determine the last VGT shader. */
   const gl_shader_stage last_vgt_api_stage = radv_cmdbuf_get_last_vgt_api_stage(cmd_buffer);

   assume(last_vgt_api_stage != MESA_SHADER_NONE);
   if (pdev->info.has_vgt_flush_ngg_legacy_bug &&
       (!cmd_buffer->state.last_vgt_shader || (cmd_buffer->state.last_vgt_shader->info.is_ngg &&
                                               !cmd_buffer->state.shaders[last_vgt_api_stage]->info.is_ngg))) {
      /* Transitioning from NGG to legacy GS requires VGT_FLUSH on GFX10 and Navi21. VGT_FLUSH is
       * also emitted at the beginning of IBs when legacy GS ring pointers are set.
       */
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_FLUSH;
   }

   cmd_buffer->state.last_vgt_shader = cmd_buffer->state.shaders[last_vgt_api_stage];

   struct radv_shader *gs_copy_shader = cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY]
                                           ? cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY]->gs.copy_shader
                                           : NULL;

   radv_bind_gs_copy_shader(cmd_buffer, gs_copy_shader);

   /* Determine NGG GS info. */
   if (cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY] &&
       cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.is_ngg &&
       cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.merged_shader_compiled_separately) {
      struct radv_shader *es = cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                  ? cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                  : cmd_buffer->state.shaders[MESA_SHADER_VERTEX];
      struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];

      gfx10_get_ngg_info(device, &es->info, &gs->info, &gs->info.ngg_info);
      radv_precompute_registers_hw_ngg(device, &gs->config, &gs->info);
   }

   /* Determine the rasterized primitive. */
   if (cmd_buffer->state.active_stages &
       (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
        VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_MESH_BIT_EXT)) {
      cmd_buffer->state.rast_prim = radv_get_vgt_gs_out(cmd_buffer->state.shaders, 0);
   }

   const struct radv_shader *vs = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   if (vs) {
      /* Re-emit the VS prolog when a new vertex shader is bound. */
      if (vs->info.vs.has_prolog) {
         cmd_buffer->state.emitted_vs_prolog = NULL;
         cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT;
      }

      /* Re-emit the vertex buffer descriptors because they are really tied to the pipeline. */
      if (vs->info.vs.vb_desc_usage_mask) {
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
      }
   }

   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   if (ps && !ps->info.has_epilog) {
      uint32_t col_format = 0, cb_shader_mask = 0;
      if (radv_needs_null_export_workaround(device, ps, 0))
         col_format = V_028714_SPI_SHADER_32_R;

      if (cmd_buffer->state.spi_shader_col_format != col_format) {
         cmd_buffer->state.spi_shader_col_format = col_format;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_COLOR_OUTPUT;
         if (pdev->info.rbplus_allowed)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
      }

      if (cmd_buffer->state.cb_shader_mask != cb_shader_mask) {
         cmd_buffer->state.cb_shader_mask = cb_shader_mask;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_COLOR_OUTPUT;
      }
   }

   /* Update push constants/indirect descriptors state. */
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
   struct radv_push_constant_state *pc_state = &cmd_buffer->push_constant_state[VK_PIPELINE_BIND_POINT_GRAPHICS];

   descriptors_state->need_indirect_descriptor_sets = need_indirect_descriptor_sets;
   pc_state->size = push_constant_size;
   pc_state->dynamic_offset_count = dynamic_offset_count;

   if (pdev->info.gfx_level <= GFX9) {
      cmd_buffer->state.ia_multi_vgt_param = radv_compute_ia_multi_vgt_param(device, cmd_buffer->state.shaders);
   }

   if (cmd_buffer->state.active_stages &
       (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) {
      cmd_buffer->state.uses_dynamic_patch_control_points = true;
   }

   cmd_buffer->state.uses_dynamic_vertex_binding_stride = true;
}

/* MUST inline this function to avoid massive perf loss in drawoverhead */
ALWAYS_INLINE static bool
radv_before_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const bool has_prefetch = pdev->info.gfx_level >= GFX7;

   ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 4096 + 128 * (drawCount - 1));

   if (likely(!info->indirect)) {
      /* GFX6-GFX7 treat instance_count==0 as instance_count==1. There is
       * no workaround for indirect draws, but we can at least skip
       * direct draws.
       */
      if (unlikely(!info->instance_count))
         return false;

      /* Handle count == 0. */
      if (unlikely(!info->count && !info->strmout_buffer))
         return false;
   }

   if (!info->indexed && pdev->info.gfx_level >= GFX7) {
      /* On GFX7 and later, non-indexed draws overwrite VGT_INDEX_TYPE,
       * so the state must be re-emitted before the next indexed
       * draw.
       */
      cmd_buffer->state.last_index_type = -1;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GRAPHICS_SHADERS) {
      radv_bind_graphics_shaders(cmd_buffer);
   }

   /* Use optimal packet order based on whether we need to sync the
    * pipeline.
    */
   if (cmd_buffer->state.flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB |
                                       RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH)) {
      /* If we have to wait for idle, set all states first, so that
       * all SET packets are processed in parallel with previous draw
       * calls. Then upload descriptors, set shader pointers, and
       * draw, and prefetch at the end. This ensures that the time
       * the CUs are idle is very short. (there are only SET_SH
       * packets between the wait and the draw)
       */
      radv_emit_all_graphics_states(cmd_buffer, info);
      radv_emit_cache_flush(cmd_buffer);
      /* <-- CUs are idle here --> */

      radv_upload_graphics_shader_descriptors(cmd_buffer);
   } else {
      const bool need_prefetch = has_prefetch && cmd_buffer->state.prefetch_L2_mask;

      /* If we don't wait for idle, start prefetches first, then set
       * states, and draw at the end.
       */
      radv_emit_cache_flush(cmd_buffer);

      if (need_prefetch) {
         /* Only prefetch the vertex shader and VBO descriptors
          * in order to start the draw as soon as possible.
          */
         radv_emit_prefetch_L2(cmd_buffer, true);
      }

      radv_upload_graphics_shader_descriptors(cmd_buffer);

      radv_emit_all_graphics_states(cmd_buffer, info);
   }

   if (!dgc)
      radv_describe_draw(cmd_buffer);
   if (likely(!info->indirect)) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      struct radeon_cmdbuf *cs = cmd_buffer->cs;
      assert(state->vtx_base_sgpr);
      if (state->last_num_instances != info->instance_count) {
         radeon_emit(cs, PKT3(PKT3_NUM_INSTANCES, 0, false));
         radeon_emit(cs, info->instance_count);
         state->last_num_instances = info->instance_count;
      }
   }
   assert(cmd_buffer->cs->cdw <= cdw_max);

   return true;
}

ALWAYS_INLINE static bool
radv_before_taskmesh_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount,
                          bool dgc)
{
   /* For direct draws, this makes sure we don't draw anything.
    * For indirect draws, this is necessary to prevent a GPU hang (on MEC version < 100).
    */
   if (unlikely(!info->count))
      return false;

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GRAPHICS_SHADERS) {
      radv_bind_graphics_shaders(cmd_buffer);
   }

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *ace_cs = cmd_buffer->gang.cs;
   struct radv_shader *task_shader = cmd_buffer->state.shaders[MESA_SHADER_TASK];

   assert(!task_shader || ace_cs);

   const VkShaderStageFlags stages =
      VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT | (task_shader ? VK_SHADER_STAGE_TASK_BIT_EXT : 0);
   const bool need_task_semaphore = task_shader && radv_flush_gang_leader_semaphore(cmd_buffer);

   ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 4096 + 128 * (drawCount - 1));
   ASSERTED const unsigned ace_cdw_max =
      !ace_cs ? 0 : radeon_check_space(device->ws, ace_cs, 4096 + 128 * (drawCount - 1));

   radv_emit_all_graphics_states(cmd_buffer, info);

   radv_emit_cache_flush(cmd_buffer);

   if (task_shader) {
      radv_gang_cache_flush(cmd_buffer);

      if (need_task_semaphore) {
         radv_wait_gang_leader(cmd_buffer);
      }
   }

   radv_flush_descriptors(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);

   const VkShaderStageFlags pc_stages = radv_must_flush_constants(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, VK_PIPELINE_BIND_POINT_GRAPHICS);

   if (!dgc)
      radv_describe_draw(cmd_buffer);
   if (likely(!info->indirect)) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      if (unlikely(state->last_num_instances != 1)) {
         struct radeon_cmdbuf *cs = cmd_buffer->cs;
         radeon_emit(cs, PKT3(PKT3_NUM_INSTANCES, 0, false));
         radeon_emit(cs, 1);
         state->last_num_instances = 1;
      }
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
   assert(!ace_cs || ace_cs->cdw <= ace_cdw_max);

   cmd_buffer->state.last_index_type = -1;

   return true;
}

ALWAYS_INLINE static void
radv_after_draw(struct radv_cmd_buffer *cmd_buffer, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   bool has_prefetch = pdev->info.gfx_level >= GFX7;
   /* Start prefetches after the draw has been started. Both will
    * run in parallel, but starting the draw first is more
    * important.
    */
   if (has_prefetch && cmd_buffer->state.prefetch_L2_mask) {
      radv_emit_prefetch_L2(cmd_buffer, false);
   }

   /* Workaround for a VGT hang when streamout is enabled.
    * It must be done after drawing.
    */
   if (radv_is_streamout_enabled(cmd_buffer) &&
       (gpu_info->family == CHIP_HAWAII || gpu_info->family == CHIP_TONGA || gpu_info->family == CHIP_FIJI)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_STREAMOUT_SYNC;
   }

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_PS_PARTIAL_FLUSH, dgc);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
             uint32_t firstInstance)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   info.count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;
   info.indexed = false;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   const VkMultiDrawInfoEXT minfo = {firstVertex, vertexCount};
   radv_emit_direct_draw_packets(cmd_buffer, &info, 1, &minfo, 0, 0);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawInfoEXT *pVertexInfo,
                     uint32_t instanceCount, uint32_t firstInstance, uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   if (!drawCount)
      return;

   info.count = pVertexInfo->vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;
   info.indexed = false;

   if (!radv_before_draw(cmd_buffer, &info, drawCount, false))
      return;
   radv_emit_direct_draw_packets(cmd_buffer, &info, drawCount, pVertexInfo, 0, stride);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                    int32_t vertexOffset, uint32_t firstInstance)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = indexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   const VkMultiDrawIndexedInfoEXT minfo = {firstIndex, indexCount, vertexOffset};
   radv_emit_draw_packets_indexed(cmd_buffer, &info, 1, &minfo, 0, NULL);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMultiIndexedEXT(VkCommandBuffer commandBuffer, uint32_t drawCount,
                            const VkMultiDrawIndexedInfoEXT *pIndexInfo, uint32_t instanceCount, uint32_t firstInstance,
                            uint32_t stride, const int32_t *pVertexOffset)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   if (!drawCount)
      return;

   const VkMultiDrawIndexedInfoEXT *minfo = pIndexInfo;
   info.indexed = true;
   info.count = minfo->indexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_draw(cmd_buffer, &info, drawCount, false))
      return;
   radv_emit_draw_packets_indexed(cmd_buffer, &info, drawCount, pIndexInfo, stride, pVertexOffset);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset, uint32_t drawCount,
                     uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_draw_info info;

   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset, uint32_t drawCount,
                            uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count_buffer = NULL;
   info.strmout_buffer = NULL;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset, VkBuffer _countBuffer,
                          VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   VK_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_draw_info info;

   info.count = maxDrawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.stride = stride;
   info.strmout_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                                 VkBuffer _countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                 uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   VK_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = maxDrawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.stride = stride;
   info.strmout_buffer = NULL;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_draw_info info;

   info.count = x * y * z;
   info.instance_count = 1;
   info.first_instance = 0;
   info.stride = 0;
   info.indexed = false;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1, false))
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
      radv_emit_direct_taskmesh_draw_packets(device, &cmd_buffer->state, cmd_buffer->cs, cmd_buffer->gang.cs, x, y, z);
   } else {
      radv_emit_direct_mesh_draw_packet(cmd_buffer, x, y, z);
   }

   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                                 uint32_t drawCount, uint32_t stride)
{
   if (!drawCount)
      return;

   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_draw_info info;

   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count = drawCount;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, drawCount, false))
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
      radv_emit_indirect_taskmesh_draw_packets(device, &cmd_buffer->state, cmd_buffer->cs, cmd_buffer->gang.cs, &info,
                                               0);
   } else {
      radv_emit_indirect_mesh_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                                      VkBuffer _countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                      uint32_t stride)
{

   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   VK_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_draw_info info;

   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count = maxDrawCount;
   info.strmout_buffer = NULL;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, maxDrawCount, false))
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
      uint64_t workaround_cond_va = 0;

      if (pdev->info.has_taskmesh_indirect0_bug && info.count_buffer) {
         /* Allocate a 32-bit value for the MEC firmware bug workaround. */
         uint32_t workaround_cond_init = 0;
         uint32_t workaround_cond_off;

         if (!radv_cmd_buffer_upload_data(cmd_buffer, 4, &workaround_cond_init, &workaround_cond_off))
            vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

         workaround_cond_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + workaround_cond_off;
      }

      radv_emit_indirect_taskmesh_draw_packets(device, &cmd_buffer->state, cmd_buffer->cs, cmd_buffer->gang.cs, &info,
                                               workaround_cond_va);
   } else {
      radv_emit_indirect_mesh_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer, false);
}

/* TODO: Use these functions with the normal dispatch path. */
static void radv_dgc_before_dispatch(struct radv_cmd_buffer *cmd_buffer);
static void radv_dgc_after_dispatch(struct radv_cmd_buffer *cmd_buffer);

VKAPI_ATTR void VKAPI_CALL
radv_CmdPreprocessGeneratedCommandsNV(VkCommandBuffer commandBuffer,
                                      const VkGeneratedCommandsInfoNV *pGeneratedCommandsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_indirect_command_layout, layout, pGeneratedCommandsInfo->indirectCommandsLayout);
   VK_FROM_HANDLE(radv_pipeline, pipeline, pGeneratedCommandsInfo->pipeline);

   if (!radv_dgc_can_preprocess(layout, pipeline))
      return;

   /* VK_EXT_conditional_rendering says that copy commands should not be
    * affected by conditional rendering.
    */
   const bool old_predicating = cmd_buffer->state.predicating;
   cmd_buffer->state.predicating = false;

   radv_prepare_dgc(cmd_buffer, pGeneratedCommandsInfo, false);

   /* Restore conditional rendering. */
   cmd_buffer->state.predicating = old_predicating;
}

static void
radv_dgc_execute_ib(struct radv_cmd_buffer *cmd_buffer, const VkGeneratedCommandsInfoNV *pGeneratedCommandsInfo)
{
   VK_FROM_HANDLE(radv_buffer, prep_buffer, pGeneratedCommandsInfo->preprocessBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const bool has_task_shader = radv_dgc_with_task_shader(pGeneratedCommandsInfo);

   const uint32_t cmdbuf_size = radv_get_indirect_cmdbuf_size(pGeneratedCommandsInfo);
   const uint64_t ib_va =
      radv_buffer_get_va(prep_buffer->bo) + prep_buffer->offset + pGeneratedCommandsInfo->preprocessOffset;

   device->ws->cs_execute_ib(cmd_buffer->cs, NULL, ib_va, cmdbuf_size >> 2, cmd_buffer->state.predicating);

   if (has_task_shader) {
      const uint32_t ace_cmdbuf_size = radv_get_indirect_ace_cmdbuf_size(pGeneratedCommandsInfo);
      const uint64_t ace_ib_va = ib_va + radv_get_indirect_ace_cmdbuf_offset(pGeneratedCommandsInfo);

      assert(cmd_buffer->gang.cs);
      device->ws->cs_execute_ib(cmd_buffer->gang.cs, NULL, ace_ib_va, ace_cmdbuf_size >> 2,
                                cmd_buffer->state.predicating);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdExecuteGeneratedCommandsNV(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed,
                                   const VkGeneratedCommandsInfoNV *pGeneratedCommandsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_indirect_command_layout, layout, pGeneratedCommandsInfo->indirectCommandsLayout);
   VK_FROM_HANDLE(radv_pipeline, pipeline, pGeneratedCommandsInfo->pipeline);
   VK_FROM_HANDLE(radv_buffer, prep_buffer, pGeneratedCommandsInfo->preprocessBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const bool compute = layout->pipeline_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE;
   const bool use_predication = radv_use_dgc_predication(cmd_buffer, pGeneratedCommandsInfo);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   /* Secondary command buffers are needed for the full extension but can't use
    * PKT3_INDIRECT_BUFFER.
    */
   assert(cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   if (use_predication) {
      VK_FROM_HANDLE(radv_buffer, seq_count_buffer, pGeneratedCommandsInfo->sequencesCountBuffer);
      const uint64_t va = radv_buffer_get_va(seq_count_buffer->bo) + seq_count_buffer->offset +
                          pGeneratedCommandsInfo->sequencesCountOffset;

      radv_begin_conditional_rendering(cmd_buffer, va, true);
   }

   if (!radv_dgc_can_preprocess(layout, pipeline)) {
      /* Suspend conditional rendering when the DGC execute is called on the compute queue to
       * generate a cmdbuf which will skips dispatches when necessary. This is because the
       * compute queue is missing IB2 which means it's not possible to skip the cmdbuf entirely.
       * It should also be suspended when task shaders are used because the DGC ACE IB would be
       * uninitialized otherwise.
       */
      const bool suspend_cond_render =
         (cmd_buffer->qf == RADV_QUEUE_COMPUTE || radv_dgc_with_task_shader(pGeneratedCommandsInfo));
      const bool old_predicating = cmd_buffer->state.predicating;

      if (suspend_cond_render && cmd_buffer->state.predicating) {
         cmd_buffer->state.predicating = false;
      }

      radv_prepare_dgc(cmd_buffer, pGeneratedCommandsInfo, old_predicating);

      if (suspend_cond_render) {
         cmd_buffer->state.predicating = old_predicating;
      }

      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_L2;

      if (radv_dgc_with_task_shader(pGeneratedCommandsInfo)) {
         /* Make sure the DGC ACE IB will wait for the DGC prepare shader before the execution
          * starts.
          */
         radv_gang_barrier(cmd_buffer, VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_NV,
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
      }
   }

   if (compute) {
      radv_dgc_before_dispatch(cmd_buffer);

      if (!pGeneratedCommandsInfo->pipeline)
         cmd_buffer->has_indirect_pipeline_binds = true;
   } else {
      struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);
      struct radv_draw_info info;

      info.count = pGeneratedCommandsInfo->sequencesCount;
      info.indirect = prep_buffer; /* We're not really going use it this way, but a good signal
                                   that this is not direct. */
      info.indirect_offset = 0;
      info.stride = 0;
      info.strmout_buffer = NULL;
      info.count_buffer = NULL;
      info.indexed = layout->indexed;
      info.instance_count = 0;

      if (radv_pipeline_has_stage(graphics_pipeline, MESA_SHADER_MESH)) {
         if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1, true))
            return;
      } else {
         if (!radv_before_draw(cmd_buffer, &info, 1, true))
            return;
      }
   }

   const uint32_t view_mask = cmd_buffer->state.render.view_mask;

   if (!radv_cmd_buffer_uses_mec(cmd_buffer)) {
      radeon_emit(cmd_buffer->cs, PKT3(PKT3_PFP_SYNC_ME, 0, cmd_buffer->state.predicating));
      radeon_emit(cmd_buffer->cs, 0);
   }

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, prep_buffer->bo);

   if (compute || !view_mask) {
      radv_dgc_execute_ib(cmd_buffer, pGeneratedCommandsInfo);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(&cmd_buffer->state, cmd_buffer->cs, view);

         radv_dgc_execute_ib(cmd_buffer, pGeneratedCommandsInfo);
      }
   }

   if (compute) {
      cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;

      if (!pGeneratedCommandsInfo->pipeline)
         radv_mark_descriptor_sets_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);

      radv_dgc_after_dispatch(cmd_buffer);
   } else {
      struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);

      if (layout->binds_index_buffer) {
         cmd_buffer->state.last_index_type = -1;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;
      }

      if (layout->bind_vbo_mask)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;

      cmd_buffer->push_constant_stages |= graphics_pipeline->active_stages;

      if (!layout->indexed && pdev->info.gfx_level >= GFX7) {
         /* On GFX7 and later, non-indexed draws overwrite VGT_INDEX_TYPE, so the state must be
          * re-emitted before the next indexed draw.
          */
         cmd_buffer->state.last_index_type = -1;
      }

      cmd_buffer->state.last_num_instances = -1;
      cmd_buffer->state.last_vertex_offset_valid = false;
      cmd_buffer->state.last_first_instance = -1;
      cmd_buffer->state.last_drawid = -1;

      radv_after_draw(cmd_buffer, true);
   }

   if (use_predication) {
      radv_end_conditional_rendering(cmd_buffer);
   }
}

static void
radv_save_dispatch_size(struct radv_cmd_buffer *cmd_buffer, uint64_t indirect_va)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   radeon_check_space(device->ws, cs, 18);

   uint64_t va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, indirect_dispatch);

   for (uint32_t i = 0; i < 3; i++) {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs,
                  COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, indirect_va);
      radeon_emit(cs, indirect_va >> 32);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);

      indirect_va += 4;
      va += 4;
   }
}

static void
radv_emit_dispatch_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *compute_shader,
                           const struct radv_dispatch_info *info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned dispatch_initiator = device->dispatch_initiator;
   struct radeon_winsys *ws = device->ws;
   bool predicating = cmd_buffer->state.predicating;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const uint32_t grid_size_offset = radv_get_user_sgpr_loc(compute_shader, AC_UD_CS_GRID_SIZE);

   radv_describe_dispatch(cmd_buffer, info);

   ASSERTED unsigned cdw_max = radeon_check_space(ws, cs, 30);

   if (compute_shader->info.wave_size == 32) {
      assert(pdev->info.gfx_level >= GFX10);
      dispatch_initiator |= S_00B800_CS_W32_EN(1);
   }

   if (info->ordered)
      dispatch_initiator &= ~S_00B800_ORDER_MODE(1);

   if (info->va) {
      if (radv_device_fault_detection_enabled(device))
         radv_save_dispatch_size(cmd_buffer, info->va);

      if (info->indirect)
         radv_cs_add_buffer(ws, cs, info->indirect);

      if (info->unaligned) {
         radeon_set_sh_reg_seq(cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
         if (pdev->info.gfx_level >= GFX12) {
            radeon_emit(cs, S_00B81C_NUM_THREAD_FULL_GFX12(compute_shader->info.cs.block_size[0]));
            radeon_emit(cs, S_00B820_NUM_THREAD_FULL_GFX12(compute_shader->info.cs.block_size[1]));
         } else {
            radeon_emit(cs, S_00B81C_NUM_THREAD_FULL_GFX6(compute_shader->info.cs.block_size[0]));
            radeon_emit(cs, S_00B820_NUM_THREAD_FULL_GFX6(compute_shader->info.cs.block_size[1]));
         }
         radeon_emit(cs, S_00B824_NUM_THREAD_FULL(compute_shader->info.cs.block_size[2]));

         dispatch_initiator |= S_00B800_USE_THREAD_DIMENSIONS(1);
      }

      if (grid_size_offset) {
         if (device->load_grid_size_from_user_sgpr) {
            assert(pdev->info.gfx_level >= GFX10_3);
            radeon_emit(cs, PKT3(PKT3_LOAD_SH_REG_INDEX, 3, 0));
            radeon_emit(cs, info->va);
            radeon_emit(cs, info->va >> 32);
            radeon_emit(cs, (grid_size_offset - SI_SH_REG_OFFSET) >> 2);
            radeon_emit(cs, 3);
         } else {
            radv_emit_shader_pointer(device, cmd_buffer->cs, grid_size_offset, info->va, true);
         }
      }

      if (radv_cmd_buffer_uses_mec(cmd_buffer)) {
         uint64_t indirect_va = info->va;
         const bool needs_align32_workaround = pdev->info.has_async_compute_align32_bug &&
                                               cmd_buffer->qf == RADV_QUEUE_COMPUTE &&
                                               !util_is_aligned(indirect_va, 32);
         const unsigned ace_predication_size =
            4 /* DISPATCH_INDIRECT */ + (needs_align32_workaround ? 6 * 3 /* 3x COPY_DATA */ : 0);

         radv_cs_emit_compute_predication(device, &cmd_buffer->state, cs, cmd_buffer->state.mec_inv_pred_va,
                                          &cmd_buffer->state.mec_inv_pred_emitted, ace_predication_size);

         if (needs_align32_workaround) {
            const uint64_t unaligned_va = indirect_va;
            UNUSED void *ptr;
            uint32_t offset;

            if (!radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, sizeof(VkDispatchIndirectCommand), 32, &offset, &ptr))
               return;

            indirect_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

            for (uint32_t i = 0; i < 3; i++) {
               const uint64_t src_va = unaligned_va + i * 4;
               const uint64_t dst_va = indirect_va + i * 4;

               radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
               radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                                  COPY_DATA_WR_CONFIRM);
               radeon_emit(cs, src_va);
               radeon_emit(cs, src_va >> 32);
               radeon_emit(cs, dst_va);
               radeon_emit(cs, dst_va >> 32);
            }
         }

         radeon_emit(cs, PKT3(PKT3_DISPATCH_INDIRECT, 2, 0) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(cs, indirect_va);
         radeon_emit(cs, indirect_va >> 32);
         radeon_emit(cs, dispatch_initiator);
      } else {
         radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(cs, 1);
         radeon_emit(cs, info->va);
         radeon_emit(cs, info->va >> 32);

         if (cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
            radv_cs_emit_compute_predication(device, &cmd_buffer->state, cs, cmd_buffer->state.mec_inv_pred_va,
                                             &cmd_buffer->state.mec_inv_pred_emitted, 3 /* PKT3_DISPATCH_INDIRECT */);
            predicating = false;
         }

         radeon_emit(cs, PKT3(PKT3_DISPATCH_INDIRECT, 1, predicating) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(cs, 0);
         radeon_emit(cs, dispatch_initiator);
      }
   } else {
      const unsigned *cs_block_size = compute_shader->info.cs.block_size;
      unsigned blocks[3] = {info->blocks[0], info->blocks[1], info->blocks[2]};
      unsigned offsets[3] = {info->offsets[0], info->offsets[1], info->offsets[2]};

      if (info->unaligned) {
         unsigned remainder[3];

         /* If aligned, these should be an entire block size,
          * not 0.
          */
         remainder[0] = blocks[0] + cs_block_size[0] - ALIGN_NPOT(blocks[0], cs_block_size[0]);
         remainder[1] = blocks[1] + cs_block_size[1] - ALIGN_NPOT(blocks[1], cs_block_size[1]);
         remainder[2] = blocks[2] + cs_block_size[2] - ALIGN_NPOT(blocks[2], cs_block_size[2]);

         blocks[0] = DIV_ROUND_UP(blocks[0], cs_block_size[0]);
         blocks[1] = DIV_ROUND_UP(blocks[1], cs_block_size[1]);
         blocks[2] = DIV_ROUND_UP(blocks[2], cs_block_size[2]);

         for (unsigned i = 0; i < 3; ++i) {
            assert(offsets[i] % cs_block_size[i] == 0);
            offsets[i] /= cs_block_size[i];
         }

         radeon_set_sh_reg_seq(cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
         if (pdev->info.gfx_level >= GFX12) {
            radeon_emit(cs,
                        S_00B81C_NUM_THREAD_FULL_GFX12(cs_block_size[0]) | S_00B81C_NUM_THREAD_PARTIAL(remainder[0]));
            radeon_emit(cs,
                        S_00B820_NUM_THREAD_FULL_GFX12(cs_block_size[1]) | S_00B820_NUM_THREAD_PARTIAL(remainder[1]));
         } else {
            radeon_emit(cs,
                        S_00B81C_NUM_THREAD_FULL_GFX6(cs_block_size[0]) | S_00B81C_NUM_THREAD_PARTIAL(remainder[0]));
            radeon_emit(cs,
                        S_00B820_NUM_THREAD_FULL_GFX6(cs_block_size[1]) | S_00B820_NUM_THREAD_PARTIAL(remainder[1]));
         }
         radeon_emit(cs, S_00B824_NUM_THREAD_FULL(cs_block_size[2]) | S_00B824_NUM_THREAD_PARTIAL(remainder[2]));

         dispatch_initiator |= S_00B800_PARTIAL_TG_EN(1);
      }

      if (grid_size_offset) {
         if (device->load_grid_size_from_user_sgpr) {
            radeon_set_sh_reg_seq(cs, grid_size_offset, 3);
            radeon_emit(cs, blocks[0]);
            radeon_emit(cs, blocks[1]);
            radeon_emit(cs, blocks[2]);
         } else {
            uint32_t offset;
            if (!radv_cmd_buffer_upload_data(cmd_buffer, 12, blocks, &offset))
               return;

            uint64_t va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;
            radv_emit_shader_pointer(device, cmd_buffer->cs, grid_size_offset, va, true);
         }
      }

      if (offsets[0] || offsets[1] || offsets[2]) {
         radeon_set_sh_reg_seq(cs, R_00B810_COMPUTE_START_X, 3);
         radeon_emit(cs, offsets[0]);
         radeon_emit(cs, offsets[1]);
         radeon_emit(cs, offsets[2]);

         /* The blocks in the packet are not counts but end values. */
         for (unsigned i = 0; i < 3; ++i)
            blocks[i] += offsets[i];
      } else {
         dispatch_initiator |= S_00B800_FORCE_START_AT_000(1);
      }

      if (cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
         radv_cs_emit_compute_predication(device, &cmd_buffer->state, cs, cmd_buffer->state.mec_inv_pred_va,
                                          &cmd_buffer->state.mec_inv_pred_emitted, 5 /* DISPATCH_DIRECT size */);
         predicating = false;
      }

      if (pdev->info.has_async_compute_threadgroup_bug && cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
         for (unsigned i = 0; i < 3; i++) {
            if (info->unaligned) {
               /* info->blocks is already in thread dimensions for unaligned dispatches. */
               blocks[i] = info->blocks[i];
            } else {
               /* Force the async compute dispatch to be in "thread" dim mode to workaround a hw bug. */
               blocks[i] *= cs_block_size[i];
            }

            dispatch_initiator |= S_00B800_USE_THREAD_DIMENSIONS(1);
         }
      }

      radeon_emit(cs, PKT3(PKT3_DISPATCH_DIRECT, 3, predicating) | PKT3_SHADER_TYPE_S(1));
      radeon_emit(cs, blocks[0]);
      radeon_emit(cs, blocks[1]);
      radeon_emit(cs, blocks[2]);
      radeon_emit(cs, dispatch_initiator);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

static void
radv_upload_compute_shader_descriptors(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   radv_flush_descriptors(cmd_buffer, VK_SHADER_STAGE_COMPUTE_BIT, bind_point);
   const VkShaderStageFlags stages =
      bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ? RADV_RT_STAGE_BITS : VK_SHADER_STAGE_COMPUTE_BIT;
   const VkShaderStageFlags pc_stages = radv_must_flush_constants(cmd_buffer, stages, bind_point);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, bind_point);
}

static void
radv_emit_rt_stack_size(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   unsigned rsrc2 = cmd_buffer->state.rt_prolog->config.rsrc2;
   if (cmd_buffer->state.rt_stack_size)
      rsrc2 |= S_00B12C_SCRATCH_EN(1);

   radeon_check_space(device->ws, cmd_buffer->cs, 3);
   radeon_set_sh_reg(cmd_buffer->cs, R_00B84C_COMPUTE_PGM_RSRC2, rsrc2);
}

static void
radv_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info,
              struct radv_compute_pipeline *pipeline, struct radv_shader *compute_shader,
              VkPipelineBindPoint bind_point)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool has_prefetch = pdev->info.gfx_level >= GFX7;
   bool pipeline_is_dirty = pipeline != cmd_buffer->state.emitted_compute_pipeline;

   if (compute_shader->info.cs.regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   if (cmd_buffer->state.flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB |
                                       RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH)) {
      /* If we have to wait for idle, set all states first, so that
       * all SET packets are processed in parallel with previous draw
       * calls. Then upload descriptors, set shader pointers, and
       * dispatch, and prefetch at the end. This ensures that the
       * time the CUs are idle is very short. (there are only SET_SH
       * packets between the wait and the draw)
       */
      radv_emit_compute_pipeline(cmd_buffer, pipeline);
      if (bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
         radv_emit_rt_stack_size(cmd_buffer);
      radv_emit_cache_flush(cmd_buffer);
      /* <-- CUs are idle here --> */

      radv_upload_compute_shader_descriptors(cmd_buffer, bind_point);

      radv_emit_dispatch_packets(cmd_buffer, compute_shader, info);
      /* <-- CUs are busy here --> */

      /* Start prefetches after the dispatch has been started. Both
       * will run in parallel, but starting the dispatch first is
       * more important.
       */
      if (has_prefetch && pipeline_is_dirty) {
         radv_emit_shader_prefetch(cmd_buffer, compute_shader);
      }
   } else {
      /* If we don't wait for idle, start prefetches first, then set
       * states, and dispatch at the end.
       */
      radv_emit_cache_flush(cmd_buffer);

      if (has_prefetch && pipeline_is_dirty) {
         radv_emit_shader_prefetch(cmd_buffer, compute_shader);
      }

      radv_upload_compute_shader_descriptors(cmd_buffer, bind_point);

      radv_emit_compute_pipeline(cmd_buffer, pipeline);
      if (bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
         radv_emit_rt_stack_size(cmd_buffer);
      radv_emit_dispatch_packets(cmd_buffer, compute_shader, info);
   }

   if (pipeline_is_dirty) {
      /* Raytracing uses compute shaders but has separate bind points and pipelines.
       * So if we set compute userdata & shader registers we should dirty the raytracing
       * ones and the other way around.
       *
       * We only need to do this when the pipeline is dirty because when we switch between
       * the two we always need to switch pipelines.
       */
      radv_mark_descriptor_sets_dirty(cmd_buffer, bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                                     ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                                                     : VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (compute_shader->info.cs.regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_CS_PARTIAL_FLUSH, false);
}

static void
radv_dgc_before_dispatch(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_compute_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   struct radv_shader *compute_shader = cmd_buffer->state.shaders[MESA_SHADER_COMPUTE];
   bool pipeline_is_dirty = pipeline != cmd_buffer->state.emitted_compute_pipeline;

   /* We will have run the DGC patch shaders before, so we can assume that there is something to
    * flush. Otherwise, we just split radv_dispatch in two. One pre-dispatch and another one
    * post-dispatch. */

   if (compute_shader->info.cs.regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   if (pipeline)
      radv_emit_compute_pipeline(cmd_buffer, pipeline);
   radv_emit_cache_flush(cmd_buffer);

   radv_upload_compute_shader_descriptors(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);

   if (pipeline_is_dirty) {
      const bool has_prefetch = pdev->info.gfx_level >= GFX7;

      if (has_prefetch)
         radv_emit_shader_prefetch(cmd_buffer, compute_shader);

      /* Raytracing uses compute shaders but has separate bind points and pipelines.
       * So if we set compute userdata & shader registers we should dirty the raytracing
       * ones and the other way around.
       *
       * We only need to do this when the pipeline is dirty because when we switch between
       * the two we always need to switch pipelines.
       */
      radv_mark_descriptor_sets_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

static void
radv_dgc_after_dispatch(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_shader *compute_shader = cmd_buffer->state.shaders[MESA_SHADER_COMPUTE];

   if (compute_shader->info.cs.regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_CS_PARTIAL_FLUSH, true);
}

void
radv_compute_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info)
{
   radv_dispatch(cmd_buffer, info, cmd_buffer->state.compute_pipeline, cmd_buffer->state.shaders[MESA_SHADER_COMPUTE],
                 VK_PIPELINE_BIND_POINT_COMPUTE);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t base_x, uint32_t base_y, uint32_t base_z, uint32_t x,
                     uint32_t y, uint32_t z)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_dispatch_info info = {0};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;

   info.offsets[0] = base_x;
   info.offsets[1] = base_y;
   info.offsets[2] = base_z;
   radv_compute_dispatch(cmd_buffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_dispatch_info info = {0};

   info.indirect = buffer->bo;
   info.va = radv_buffer_get_va(buffer->bo) + buffer->offset + offset;

   radv_compute_dispatch(cmd_buffer, &info);
}

void
radv_unaligned_dispatch(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   struct radv_dispatch_info info = {0};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;
   info.unaligned = 1;

   radv_compute_dispatch(cmd_buffer, &info);
}

void
radv_indirect_dispatch(struct radv_cmd_buffer *cmd_buffer, struct radeon_winsys_bo *bo, uint64_t va)
{
   struct radv_dispatch_info info = {0};

   info.indirect = bo;
   info.va = va;

   radv_compute_dispatch(cmd_buffer, &info);
}

static void
radv_trace_trace_rays(struct radv_cmd_buffer *cmd_buffer, const VkTraceRaysIndirectCommand2KHR *cmd,
                      uint64_t indirect_va)
{
   if (!cmd || indirect_va)
      return;

   struct radv_rra_ray_history_data *data = malloc(sizeof(struct radv_rra_ray_history_data));
   if (!data)
      return;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t width = DIV_ROUND_UP(cmd->width, device->rra_trace.ray_history_resolution_scale);
   uint32_t height = DIV_ROUND_UP(cmd->height, device->rra_trace.ray_history_resolution_scale);
   uint32_t depth = DIV_ROUND_UP(cmd->depth, device->rra_trace.ray_history_resolution_scale);

   struct radv_rra_ray_history_counter counter = {
      .dispatch_size = {width, height, depth},
      .hit_shader_count = cmd->hitShaderBindingTableSize / cmd->hitShaderBindingTableStride,
      .miss_shader_count = cmd->missShaderBindingTableSize / cmd->missShaderBindingTableStride,
      .shader_count = cmd_buffer->state.rt_pipeline->stage_count,
      .pipeline_api_hash = cmd_buffer->state.rt_pipeline->base.base.pipeline_hash,
      .mode = 1,
      .stride = sizeof(uint32_t),
      .data_size = 0,
      .ray_id_begin = 0,
      .ray_id_end = 0xFFFFFFFF,
      .pipeline_type = RADV_RRA_PIPELINE_RAY_TRACING,
   };

   struct radv_rra_ray_history_dispatch_size dispatch_size = {
      .size = {width, height, depth},
   };

   struct radv_rra_ray_history_traversal_flags traversal_flags = {0};

   data->metadata = (struct radv_rra_ray_history_metadata){
      .counter_info.type = RADV_RRA_COUNTER_INFO,
      .counter_info.size = sizeof(struct radv_rra_ray_history_counter),
      .counter = counter,

      .dispatch_size_info.type = RADV_RRA_DISPATCH_SIZE,
      .dispatch_size_info.size = sizeof(struct radv_rra_ray_history_dispatch_size),
      .dispatch_size = dispatch_size,

      .traversal_flags_info.type = RADV_RRA_TRAVERSAL_FLAGS,
      .traversal_flags_info.size = sizeof(struct radv_rra_ray_history_traversal_flags),
      .traversal_flags = traversal_flags,
   };

   uint32_t dispatch_index = util_dynarray_num_elements(&cmd_buffer->ray_history, struct radv_rra_ray_history_data *)
                             << 16;

   util_dynarray_append(&cmd_buffer->ray_history, struct radv_rra_ray_history_data *, data);

   cmd_buffer->state.flush_bits |=
      RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_CS_PARTIAL_FLUSH |
      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, NULL) |
      radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, NULL);

   radv_update_buffer_cp(cmd_buffer,
                         device->rra_trace.ray_history_addr + offsetof(struct radv_ray_history_header, dispatch_index),
                         &dispatch_index, sizeof(dispatch_index));
}

enum radv_rt_mode {
   radv_rt_mode_direct,
   radv_rt_mode_indirect,
   radv_rt_mode_indirect2,
};

static void
radv_upload_trace_rays_params(struct radv_cmd_buffer *cmd_buffer, VkTraceRaysIndirectCommand2KHR *tables,
                              enum radv_rt_mode mode, uint64_t *launch_size_va, uint64_t *sbt_va)
{
   uint32_t upload_size = mode == radv_rt_mode_direct ? sizeof(VkTraceRaysIndirectCommand2KHR)
                                                      : offsetof(VkTraceRaysIndirectCommand2KHR, width);

   uint32_t offset;
   if (!radv_cmd_buffer_upload_data(cmd_buffer, upload_size, tables, &offset))
      return;

   uint64_t upload_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

   if (mode == radv_rt_mode_direct)
      *launch_size_va = upload_va + offsetof(VkTraceRaysIndirectCommand2KHR, width);
   if (sbt_va)
      *sbt_va = upload_va;
}

static void
radv_trace_rays(struct radv_cmd_buffer *cmd_buffer, VkTraceRaysIndirectCommand2KHR *tables, uint64_t indirect_va,
                enum radv_rt_mode mode)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);

   if (instance->debug_flags & RADV_DEBUG_NO_RT)
      return;

   if (unlikely(device->rra_trace.ray_history_buffer))
      radv_trace_trace_rays(cmd_buffer, tables, indirect_va);

   struct radv_compute_pipeline *pipeline = &cmd_buffer->state.rt_pipeline->base;
   struct radv_shader *rt_prolog = cmd_buffer->state.rt_prolog;

   /* Reserve scratch for stacks manually since it is not handled by the compute path. */
   uint32_t scratch_bytes_per_wave = rt_prolog->config.scratch_bytes_per_wave;
   uint32_t wave_size = rt_prolog->info.wave_size;

   /* The hardware register is specified as a multiple of 64 or 256 DWORDS. */
   unsigned scratch_alloc_granule = pdev->info.gfx_level >= GFX11 ? 256 : 1024;
   scratch_bytes_per_wave += align(cmd_buffer->state.rt_stack_size * wave_size, scratch_alloc_granule);

   cmd_buffer->compute_scratch_size_per_wave_needed =
      MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, scratch_bytes_per_wave);

   /* Since the workgroup size is 8x4 (or 8x8), 1D dispatches can only fill 8 threads per wave at most. To increase
    * occupancy, it's beneficial to convert to a 2D dispatch in these cases. */
   if (tables && tables->height == 1 && tables->width >= cmd_buffer->state.rt_prolog->info.cs.block_size[0])
      tables->height = ACO_RT_CONVERTED_2D_LAUNCH_SIZE;

   struct radv_dispatch_info info = {0};
   info.unaligned = true;

   uint64_t launch_size_va = 0;
   uint64_t sbt_va = 0;

   if (mode != radv_rt_mode_indirect2) {
      launch_size_va = indirect_va;
      radv_upload_trace_rays_params(cmd_buffer, tables, mode, &launch_size_va, &sbt_va);
   } else {
      launch_size_va = indirect_va + offsetof(VkTraceRaysIndirectCommand2KHR, width);
      sbt_va = indirect_va;
   }

   uint32_t remaining_ray_count = 0;

   if (mode == radv_rt_mode_direct) {
      info.blocks[0] = tables->width;
      info.blocks[1] = tables->height;
      info.blocks[2] = tables->depth;

      if (tables->height == ACO_RT_CONVERTED_2D_LAUNCH_SIZE) {
         /* We need the ray count for the 2D dispatch to be a multiple of the y block size for the division to work, and
          * a multiple of the x block size because the invocation offset must be a multiple of the block size when
          * dispatching the remaining rays. Fortunately, the x block size is itself a multiple of the y block size, so
          * we only need to ensure that the ray count is a multiple of the x block size. */
         remaining_ray_count = tables->width % rt_prolog->info.cs.block_size[0];

         uint32_t ray_count = tables->width - remaining_ray_count;
         info.blocks[0] = ray_count / rt_prolog->info.cs.block_size[1];
         info.blocks[1] = rt_prolog->info.cs.block_size[1];
      }
   } else
      info.va = launch_size_va;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 15);

   const uint32_t sbt_descriptors_offset = radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_SBT_DESCRIPTORS);
   if (sbt_descriptors_offset) {
      radv_emit_shader_pointer(device, cmd_buffer->cs, sbt_descriptors_offset, sbt_va, true);
   }

   const uint32_t ray_launch_size_addr_offset = radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_RAY_LAUNCH_SIZE_ADDR);
   if (ray_launch_size_addr_offset) {
      radv_emit_shader_pointer(device, cmd_buffer->cs, ray_launch_size_addr_offset, launch_size_va, true);
   }

   const uint32_t ray_dynamic_callback_stack_base_offset =
      radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_RAY_DYNAMIC_CALLABLE_STACK_BASE);
   if (ray_dynamic_callback_stack_base_offset) {
      const struct radv_shader_info *cs_info = &rt_prolog->info;
      radeon_set_sh_reg(cmd_buffer->cs, ray_dynamic_callback_stack_base_offset,
                        rt_prolog->config.scratch_bytes_per_wave / cs_info->wave_size);
   }

   const uint32_t traversal_shader_addr_offset = radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_TRAVERSAL_SHADER_ADDR);
   struct radv_shader *traversal_shader = cmd_buffer->state.shaders[MESA_SHADER_INTERSECTION];
   if (traversal_shader_addr_offset && traversal_shader) {
      uint64_t traversal_va = traversal_shader->va | radv_rt_priority_traversal;
      radv_emit_shader_pointer(device, cmd_buffer->cs, traversal_shader_addr_offset, traversal_va, true);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);

   radv_dispatch(cmd_buffer, &info, pipeline, rt_prolog, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

   if (remaining_ray_count) {
      info.blocks[0] = remaining_ray_count;
      info.blocks[1] = 1;
      info.offsets[0] = tables->width - remaining_ray_count;

      /* Reset the ray launch size so the prolog doesn't think this is a converted dispatch */
      tables->height = 1;
      radv_upload_trace_rays_params(cmd_buffer, tables, mode, &launch_size_va, NULL);
      if (ray_launch_size_addr_offset) {
         radv_emit_shader_pointer(device, cmd_buffer->cs, ray_launch_size_addr_offset, launch_size_va, true);
      }

      radv_dispatch(cmd_buffer, &info, pipeline, rt_prolog, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysKHR(VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable, uint32_t width,
                     uint32_t height, uint32_t depth)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   VkTraceRaysIndirectCommand2KHR tables = {
      .raygenShaderRecordAddress = pRaygenShaderBindingTable->deviceAddress,
      .raygenShaderRecordSize = pRaygenShaderBindingTable->size,
      .missShaderBindingTableAddress = pMissShaderBindingTable->deviceAddress,
      .missShaderBindingTableSize = pMissShaderBindingTable->size,
      .missShaderBindingTableStride = pMissShaderBindingTable->stride,
      .hitShaderBindingTableAddress = pHitShaderBindingTable->deviceAddress,
      .hitShaderBindingTableSize = pHitShaderBindingTable->size,
      .hitShaderBindingTableStride = pHitShaderBindingTable->stride,
      .callableShaderBindingTableAddress = pCallableShaderBindingTable->deviceAddress,
      .callableShaderBindingTableSize = pCallableShaderBindingTable->size,
      .callableShaderBindingTableStride = pCallableShaderBindingTable->stride,
      .width = width,
      .height = height,
      .depth = depth,
   };

   radv_trace_rays(cmd_buffer, &tables, 0, radv_rt_mode_direct);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysIndirectKHR(VkCommandBuffer commandBuffer,
                             const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
                             VkDeviceAddress indirectDeviceAddress)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(device->use_global_bo_list);

   VkTraceRaysIndirectCommand2KHR tables = {
      .raygenShaderRecordAddress = pRaygenShaderBindingTable->deviceAddress,
      .raygenShaderRecordSize = pRaygenShaderBindingTable->size,
      .missShaderBindingTableAddress = pMissShaderBindingTable->deviceAddress,
      .missShaderBindingTableSize = pMissShaderBindingTable->size,
      .missShaderBindingTableStride = pMissShaderBindingTable->stride,
      .hitShaderBindingTableAddress = pHitShaderBindingTable->deviceAddress,
      .hitShaderBindingTableSize = pHitShaderBindingTable->size,
      .hitShaderBindingTableStride = pHitShaderBindingTable->stride,
      .callableShaderBindingTableAddress = pCallableShaderBindingTable->deviceAddress,
      .callableShaderBindingTableSize = pCallableShaderBindingTable->size,
      .callableShaderBindingTableStride = pCallableShaderBindingTable->stride,
   };

   radv_trace_rays(cmd_buffer, &tables, indirectDeviceAddress, radv_rt_mode_indirect);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysIndirect2KHR(VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(device->use_global_bo_list);

   radv_trace_rays(cmd_buffer, NULL, indirectDeviceAddress, radv_rt_mode_indirect2);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer commandBuffer, uint32_t size)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   cmd_buffer->state.rt_stack_size = size;
}

/*
 * For HTILE we have the following interesting clear words:
 *   0xfffff30f: Uncompressed, full depth range, for depth+stencil HTILE
 *   0xfffc000f: Uncompressed, full depth range, for depth only HTILE.
 *   0xfffffff0: Clear depth to 1.0
 *   0x00000000: Clear depth to 0.0
 */
static void
radv_initialize_htile(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                      const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t htile_value = radv_get_htile_initial_value(device, image);
   VkClearDepthStencilValue value = {0};
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is consistent
    * in considering previous rendering work for WAW hazards. */
   state->flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, image);

   if (image->planes[0].surface.has_stencil &&
       !(range->aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
      /* Flush caches before performing a separate aspect initialization because it's a
       * read-modify-write operation.
       */
      state->flush_bits |=
         radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, image);
   }

   state->flush_bits |= radv_clear_htile(cmd_buffer, image, range, htile_value);

   radv_set_ds_clear_metadata(cmd_buffer, image, range, value, range->aspectMask);

   if (radv_image_is_tc_compat_htile(image) && (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      /* Initialize the TC-compat metada value to 0 because by
       * default DB_Z_INFO.RANGE_PRECISION is set to 1, and we only
       * need have to conditionally update its value when performing
       * a fast depth clear.
       */
      radv_set_tc_compat_zrange_metadata(cmd_buffer, image, range, 0);
   }
}

static void
radv_handle_depth_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   VkImageLayout src_layout, VkImageLayout dst_layout, unsigned src_queue_mask,
                                   unsigned dst_queue_mask, const VkImageSubresourceRange *range,
                                   struct radv_sample_locations_state *sample_locs)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!radv_htile_enabled(image, range->baseMipLevel))
      return;

   if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      radv_initialize_htile(cmd_buffer, image, range);
   } else if (!radv_layout_is_htile_compressed(device, image, src_layout, src_queue_mask) &&
              radv_layout_is_htile_compressed(device, image, dst_layout, dst_queue_mask)) {
      radv_initialize_htile(cmd_buffer, image, range);
   } else if (radv_layout_is_htile_compressed(device, image, src_layout, src_queue_mask) &&
              !radv_layout_is_htile_compressed(device, image, dst_layout, dst_queue_mask)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;

      radv_expand_depth_stencil(cmd_buffer, image, range, sample_locs);

      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }
}

static uint32_t
radv_init_cmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
                uint32_t value)
{
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   return radv_clear_cmask(cmd_buffer, image, range, value);
}

uint32_t
radv_init_fmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range)
{
   static const uint32_t fmask_clear_values[4] = {0x00000000, 0x02020202, 0xE4E4E4E4, 0x76543210};
   uint32_t log2_samples = util_logbase2(image->vk.samples);
   uint32_t value = fmask_clear_values[log2_samples];
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   return radv_clear_fmask(cmd_buffer, image, range, value);
}

uint32_t
radv_init_dcc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
              uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_barrier_data barrier = {0};
   uint32_t flush_bits = 0;
   unsigned size = 0;

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   flush_bits |= radv_clear_dcc(cmd_buffer, image, range, value);

   if (pdev->info.gfx_level == GFX8) {
      /* When DCC is enabled with mipmaps, some levels might not
       * support fast clears and we have to initialize them as "fully
       * expanded".
       */
      /* Compute the size of all fast clearable DCC levels. */
      for (unsigned i = 0; i < image->planes[0].surface.num_meta_levels; i++) {
         struct legacy_surf_dcc_level *dcc_level = &image->planes[0].surface.u.legacy.color.dcc_level[i];
         unsigned dcc_fast_clear_size = dcc_level->dcc_slice_fast_clear_size * image->vk.array_layers;

         if (!dcc_fast_clear_size)
            break;

         size = dcc_level->dcc_offset + dcc_fast_clear_size;
      }

      /* Initialize the mipmap levels without DCC. */
      if (size != image->planes[0].surface.meta_size) {
         flush_bits |= radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo,
                                        radv_image_get_va(image, 0) + image->planes[0].surface.meta_offset + size,
                                        image->planes[0].surface.meta_size - size, 0xffffffff);
      }
   }

   return flush_bits;
}

/**
 * Initialize DCC/FMASK/CMASK metadata for a color image.
 */
static void
radv_init_color_image_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout src_layout,
                               VkImageLayout dst_layout, unsigned src_queue_mask, unsigned dst_queue_mask,
                               const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t flush_bits = 0;

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is
    * consistent in considering previous rendering work for WAW hazards.
    */
   cmd_buffer->state.flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, image);

   if (radv_image_has_cmask(image)) {
      static const uint32_t cmask_clear_values[4] = {0xffffffff, 0xdddddddd, 0xeeeeeeee, 0xffffffff};
      uint32_t log2_samples = util_logbase2(image->vk.samples);

      flush_bits |= radv_init_cmask(cmd_buffer, image, range, cmask_clear_values[log2_samples]);
   }

   if (radv_image_has_fmask(image)) {
      flush_bits |= radv_init_fmask(cmd_buffer, image, range);
   }

   if (radv_dcc_enabled(image, range->baseMipLevel)) {
      uint32_t value = 0xffffffffu; /* Fully expanded mode. */

      if (radv_layout_dcc_compressed(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         value = 0u;
      }

      flush_bits |= radv_init_dcc(cmd_buffer, image, range, value);
   }

   if (radv_image_has_cmask(image) || radv_dcc_enabled(image, range->baseMipLevel)) {
      radv_update_fce_metadata(cmd_buffer, image, range, false);

      uint32_t color_values[2] = {0};
      radv_set_color_clear_metadata(cmd_buffer, image, range, color_values);
   }

   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
radv_retile_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout src_layout,
                       VkImageLayout dst_layout, unsigned dst_queue_mask)
{
   /* If the image is read-only, we don't have to retile DCC because it can't change. */
   if (!(image->vk.usage & RADV_IMAGE_USAGE_WRITE_BITS))
      return;

   if (src_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
       (dst_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || (dst_queue_mask & (1u << RADV_QUEUE_FOREIGN))))
      radv_retile_dcc(cmd_buffer, image);
}

static bool
radv_image_need_retile(const struct radv_cmd_buffer *cmd_buffer, const struct radv_image *image)
{
   return cmd_buffer->qf != RADV_QUEUE_TRANSFER && image->planes[0].surface.display_dcc_offset &&
          image->planes[0].surface.display_dcc_offset != image->planes[0].surface.meta_offset;
}

/**
 * Handle color image transitions for DCC/FMASK/CMASK.
 */
static void
radv_handle_color_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   VkImageLayout src_layout, VkImageLayout dst_layout, unsigned src_queue_mask,
                                   unsigned dst_queue_mask, const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   bool dcc_decompressed = false, fast_clear_flushed = false;

   if (!radv_image_has_cmask(image) && !radv_image_has_fmask(image) && !radv_dcc_enabled(image, range->baseMipLevel))
      return;

   if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      radv_init_color_image_metadata(cmd_buffer, image, src_layout, dst_layout, src_queue_mask, dst_queue_mask, range);

      if (radv_image_need_retile(cmd_buffer, image))
         radv_retile_transition(cmd_buffer, image, src_layout, dst_layout, dst_queue_mask);
      return;
   }

   if (radv_dcc_enabled(image, range->baseMipLevel)) {
      if (src_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
         cmd_buffer->state.flush_bits |= radv_init_dcc(cmd_buffer, image, range, 0xffffffffu);
      } else if (radv_layout_dcc_compressed(device, image, range->baseMipLevel, src_layout, src_queue_mask) &&
                 !radv_layout_dcc_compressed(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         radv_decompress_dcc(cmd_buffer, image, range);
         dcc_decompressed = true;
      } else if (radv_layout_can_fast_clear(device, image, range->baseMipLevel, src_layout, src_queue_mask) &&
                 !radv_layout_can_fast_clear(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         radv_fast_clear_flush_image_inplace(cmd_buffer, image, range);
         fast_clear_flushed = true;
      }

      if (radv_image_need_retile(cmd_buffer, image))
         radv_retile_transition(cmd_buffer, image, src_layout, dst_layout, dst_queue_mask);
   } else if (radv_image_has_cmask(image) || radv_image_has_fmask(image)) {
      if (radv_layout_can_fast_clear(device, image, range->baseMipLevel, src_layout, src_queue_mask) &&
          !radv_layout_can_fast_clear(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         radv_fast_clear_flush_image_inplace(cmd_buffer, image, range);
         fast_clear_flushed = true;
      }
   }

   /* MSAA color decompress. */
   const enum radv_fmask_compression src_fmask_comp =
      radv_layout_fmask_compression(device, image, src_layout, src_queue_mask);
   const enum radv_fmask_compression dst_fmask_comp =
      radv_layout_fmask_compression(device, image, dst_layout, dst_queue_mask);
   if (src_fmask_comp <= dst_fmask_comp)
      return;

   if (src_fmask_comp == RADV_FMASK_COMPRESSION_FULL) {
      if (radv_dcc_enabled(image, range->baseMipLevel) && !radv_image_use_dcc_image_stores(device, image) &&
          !dcc_decompressed) {
         /* A DCC decompress is required before expanding FMASK
          * when DCC stores aren't supported to avoid being in
          * a state where DCC is compressed and the main
          * surface is uncompressed.
          */
         radv_decompress_dcc(cmd_buffer, image, range);
      } else if (!fast_clear_flushed) {
         /* A FMASK decompress is required before expanding
          * FMASK.
          */
         radv_fast_clear_flush_image_inplace(cmd_buffer, image, range);
      }
   }

   if (dst_fmask_comp == RADV_FMASK_COMPRESSION_NONE) {
      struct radv_barrier_data barrier = {0};
      barrier.layout_transitions.fmask_color_expand = 1;
      radv_describe_layout_transition(cmd_buffer, &barrier);

      radv_expand_fmask_image_inplace(cmd_buffer, image, range);
   }
}

static void
radv_handle_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout src_layout,
                             VkImageLayout dst_layout, uint32_t src_family_index, uint32_t dst_family_index,
                             const VkImageSubresourceRange *range, struct radv_sample_locations_state *sample_locs)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   enum radv_queue_family src_qf = vk_queue_to_radv(pdev, src_family_index);
   enum radv_queue_family dst_qf = vk_queue_to_radv(pdev, dst_family_index);
   if (image->exclusive && src_family_index != dst_family_index) {
      /* This is an acquire or a release operation and there will be
       * a corresponding release/acquire. Do the transition in the
       * most flexible queue. */

      assert(src_qf == cmd_buffer->qf || dst_qf == cmd_buffer->qf);

      if (src_family_index == VK_QUEUE_FAMILY_EXTERNAL || src_family_index == VK_QUEUE_FAMILY_FOREIGN_EXT)
         return;

      if (cmd_buffer->qf == RADV_QUEUE_TRANSFER)
         return;

      if (cmd_buffer->qf == RADV_QUEUE_COMPUTE && (src_qf == RADV_QUEUE_GENERAL || dst_qf == RADV_QUEUE_GENERAL))
         return;
   }

   unsigned src_queue_mask = radv_image_queue_family_mask(image, src_qf, cmd_buffer->qf);
   unsigned dst_queue_mask = radv_image_queue_family_mask(image, dst_qf, cmd_buffer->qf);

   if (src_layout == dst_layout && src_queue_mask == dst_queue_mask)
      return;

   if (image->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      radv_handle_depth_image_transition(cmd_buffer, image, src_layout, dst_layout, src_queue_mask, dst_queue_mask,
                                         range, sample_locs);
   } else {
      radv_handle_color_image_transition(cmd_buffer, image, src_layout, dst_layout, src_queue_mask, dst_queue_mask,
                                         range);
   }
}

static void
radv_cp_dma_wait_for_stages(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 stage_mask)
{
   /* Make sure CP DMA is idle because the driver might have performed a DMA operation for copying a
    * buffer (or a MSAA image using FMASK). Note that updating a buffer is considered a clear
    * operation but it might also use a CP DMA copy in some rare situations. Other operations using
    * a CP DMA clear are implicitly synchronized (see CP_DMA_SYNC).
    */
   if (stage_mask &
       (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      radv_cp_dma_wait_for_idle(cmd_buffer);
}

void
radv_emit_cache_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool is_compute = cmd_buffer->qf == RADV_QUEUE_COMPUTE;

   if (is_compute)
      cmd_buffer->state.flush_bits &=
         ~(RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META | RADV_CMD_FLAG_FLUSH_AND_INV_DB |
           RADV_CMD_FLAG_FLUSH_AND_INV_DB_META | RADV_CMD_FLAG_INV_L2_METADATA | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
           RADV_CMD_FLAG_VS_PARTIAL_FLUSH | RADV_CMD_FLAG_VGT_FLUSH | RADV_CMD_FLAG_START_PIPELINE_STATS |
           RADV_CMD_FLAG_STOP_PIPELINE_STATS);

   if (!cmd_buffer->state.flush_bits) {
      radv_describe_barrier_end_delayed(cmd_buffer);
      return;
   }

   radv_cs_emit_cache_flush(device->ws, cmd_buffer->cs, pdev->info.gfx_level, &cmd_buffer->gfx9_fence_idx,
                            cmd_buffer->gfx9_fence_va, radv_cmd_buffer_uses_mec(cmd_buffer),
                            cmd_buffer->state.flush_bits, &cmd_buffer->state.sqtt_flush_bits,
                            cmd_buffer->gfx9_eop_bug_va);

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);

   if (cmd_buffer->state.flush_bits & RADV_CMD_FLAG_INV_L2)
      cmd_buffer->state.rb_noncoherent_dirty = false;

   /* Clear the caches that have been flushed to avoid syncing too much
    * when there is some pending active queries.
    */
   cmd_buffer->active_query_flush_bits &= ~cmd_buffer->state.flush_bits;

   cmd_buffer->state.flush_bits = 0;

   /* If the driver used a compute shader for resetting a query pool, it
    * should be finished at this point.
    */
   cmd_buffer->pending_reset_query = false;

   radv_describe_barrier_end_delayed(cmd_buffer);
}

static void
radv_barrier(struct radv_cmd_buffer *cmd_buffer, uint32_t dep_count, const VkDependencyInfo *dep_infos,
             enum rgp_barrier_reason reason)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   enum radv_cmd_flush_bits src_flush_bits = 0;
   enum radv_cmd_flush_bits dst_flush_bits = 0;
   VkPipelineStageFlags2 src_stage_mask = 0;
   VkPipelineStageFlags2 dst_stage_mask = 0;

   if (cmd_buffer->state.render.active)
      radv_mark_noncoherent_rb(cmd_buffer);

   radv_describe_barrier_start(cmd_buffer, reason);

   for (uint32_t dep_idx = 0; dep_idx < dep_count; dep_idx++) {
      const VkDependencyInfo *dep_info = &dep_infos[dep_idx];

      for (uint32_t i = 0; i < dep_info->memoryBarrierCount; i++) {
         const VkMemoryBarrier2 *barrier = &dep_info->pMemoryBarriers[i];
         src_stage_mask |= barrier->srcStageMask;
         src_flush_bits |= radv_src_access_flush(cmd_buffer, barrier->srcStageMask, barrier->srcAccessMask, NULL);
         dst_stage_mask |= barrier->dstStageMask;
         dst_flush_bits |= radv_dst_access_flush(cmd_buffer, barrier->dstStageMask, barrier->dstAccessMask, NULL);
      }

      for (uint32_t i = 0; i < dep_info->bufferMemoryBarrierCount; i++) {
         const VkBufferMemoryBarrier2 *barrier = &dep_info->pBufferMemoryBarriers[i];
         src_stage_mask |= barrier->srcStageMask;
         src_flush_bits |= radv_src_access_flush(cmd_buffer, barrier->srcStageMask, barrier->srcAccessMask, NULL);
         dst_stage_mask |= barrier->dstStageMask;
         dst_flush_bits |= radv_dst_access_flush(cmd_buffer, barrier->dstStageMask, barrier->dstAccessMask, NULL);
      }

      for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
         const VkImageMemoryBarrier2 *barrier = &dep_info->pImageMemoryBarriers[i];
         VK_FROM_HANDLE(radv_image, image, barrier->image);

         src_stage_mask |= barrier->srcStageMask;
         src_flush_bits |= radv_src_access_flush(cmd_buffer, barrier->srcStageMask, barrier->srcAccessMask, image);
         dst_stage_mask |= barrier->dstStageMask;
         dst_flush_bits |= radv_dst_access_flush(cmd_buffer, barrier->dstStageMask, barrier->dstAccessMask, image);
      }
   }

   /* The Vulkan spec 1.1.98 says:
    *
    * "An execution dependency with only
    *  VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT in the destination stage mask
    *  will only prevent that stage from executing in subsequently
    *  submitted commands. As this stage does not perform any actual
    *  execution, this is not observable - in effect, it does not delay
    *  processing of subsequent commands. Similarly an execution dependency
    *  with only VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT in the source stage mask
    *  will effectively not wait for any prior commands to complete."
    */
   if (dst_stage_mask != VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT)
      radv_stage_flush(cmd_buffer, src_stage_mask);
   cmd_buffer->state.flush_bits |= src_flush_bits;

   radv_gang_barrier(cmd_buffer, src_stage_mask, 0);

   for (uint32_t dep_idx = 0; dep_idx < dep_count; dep_idx++) {
      const VkDependencyInfo *dep_info = &dep_infos[dep_idx];

      for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
         VK_FROM_HANDLE(radv_image, image, dep_info->pImageMemoryBarriers[i].image);

         const struct VkSampleLocationsInfoEXT *sample_locs_info =
            vk_find_struct_const(dep_info->pImageMemoryBarriers[i].pNext, SAMPLE_LOCATIONS_INFO_EXT);
         struct radv_sample_locations_state sample_locations;

         if (sample_locs_info) {
            assert(image->vk.create_flags & VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT);
            sample_locations.per_pixel = sample_locs_info->sampleLocationsPerPixel;
            sample_locations.grid_size = sample_locs_info->sampleLocationGridSize;
            sample_locations.count = sample_locs_info->sampleLocationsCount;
            typed_memcpy(&sample_locations.locations[0], sample_locs_info->pSampleLocations,
                         sample_locs_info->sampleLocationsCount);
         }

         radv_handle_image_transition(
            cmd_buffer, image, dep_info->pImageMemoryBarriers[i].oldLayout, dep_info->pImageMemoryBarriers[i].newLayout,
            dep_info->pImageMemoryBarriers[i].srcQueueFamilyIndex,
            dep_info->pImageMemoryBarriers[i].dstQueueFamilyIndex, &dep_info->pImageMemoryBarriers[i].subresourceRange,
            sample_locs_info ? &sample_locations : NULL);
      }
   }

   radv_gang_barrier(cmd_buffer, 0, dst_stage_mask);

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      /* SDMA NOP packet waits for all pending SDMA operations to complete.
       * Note that GFX9+ is supposed to have RAW dependency tracking, but it's buggy
       * so we can't rely on it fow now.
       */
      radeon_check_space(device->ws, cmd_buffer->cs, 1);
      radeon_emit(cmd_buffer->cs, SDMA_PACKET(SDMA_OPCODE_NOP, 0, 0));
   } else {
      const bool is_gfx_or_ace = cmd_buffer->qf == RADV_QUEUE_GENERAL || cmd_buffer->qf == RADV_QUEUE_COMPUTE;
      if (is_gfx_or_ace)
         radv_cp_dma_wait_for_stages(cmd_buffer, src_stage_mask);
   }

   cmd_buffer->state.flush_bits |= dst_flush_bits;

   radv_describe_barrier_end(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   enum rgp_barrier_reason barrier_reason;

   if (cmd_buffer->vk.runtime_rp_barrier) {
      barrier_reason = RGP_BARRIER_EXTERNAL_RENDER_PASS_SYNC;
   } else {
      barrier_reason = RGP_BARRIER_EXTERNAL_CMD_PIPELINE_BARRIER;
   }

   radv_barrier(cmd_buffer, 1, pDependencyInfo, barrier_reason);
}

static void
write_event(struct radv_cmd_buffer *cmd_buffer, struct radv_event *event, VkPipelineStageFlags2 stageMask,
            unsigned value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint64_t va = radv_buffer_get_va(event->bo);

   if (cmd_buffer->qf == RADV_QUEUE_VIDEO_DEC || cmd_buffer->qf == RADV_QUEUE_VIDEO_ENC)
      return;

   radv_emit_cache_flush(cmd_buffer);

   radv_cs_add_buffer(device->ws, cs, event->bo);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs, 28);

   if (stageMask & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                    VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      /* Be conservative for now. */
      stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
   }

   /* Flags that only require a top-of-pipe event. */
   VkPipelineStageFlags2 top_of_pipe_flags = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

   /* Flags that only require a post-index-fetch event. */
   VkPipelineStageFlags2 post_index_fetch_flags =
      top_of_pipe_flags | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;

   /* Flags that only require signaling post PS. */
   VkPipelineStageFlags2 post_ps_flags =
      post_index_fetch_flags | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
      VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
      VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

   /* Flags that only require signaling post CS. */
   VkPipelineStageFlags2 post_cs_flags = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

   radv_cp_dma_wait_for_stages(cmd_buffer, stageMask);

   if (!(stageMask & ~top_of_pipe_flags)) {
      /* Just need to sync the PFP engine. */
      radv_write_data(cmd_buffer, V_370_PFP, va, 1, &value, false);
   } else if (!(stageMask & ~post_index_fetch_flags)) {
      /* Sync ME because PFP reads index and indirect buffers. */
      radv_write_data(cmd_buffer, V_370_ME, va, 1, &value, false);
   } else {
      unsigned event_type;

      if (!(stageMask & ~post_ps_flags)) {
         /* Sync previous fragment shaders. */
         event_type = V_028A90_PS_DONE;
      } else if (!(stageMask & ~post_cs_flags)) {
         /* Sync previous compute shaders. */
         event_type = V_028A90_CS_DONE;
      } else {
         /* Otherwise, sync all prior GPU work. */
         event_type = V_028A90_BOTTOM_OF_PIPE_TS;
      }

      radv_cs_emit_write_event_eop(cs, pdev->info.gfx_level, cmd_buffer->qf, event_type, 0, EOP_DST_SEL_MEM,
                                   EOP_DATA_SEL_VALUE_32BIT, va, value, cmd_buffer->gfx9_eop_bug_va);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent _event, const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_event, event, _event);
   VkPipelineStageFlags2 src_stage_mask = 0;

   for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pBufferMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pImageMemoryBarriers[i].srcStageMask;

   write_event(cmd_buffer, event, src_stage_mask, 1);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent _event, VkPipelineStageFlags2 stageMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_event, event, _event);

   write_event(cmd_buffer, event, stageMask, 0);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
                    const VkDependencyInfo *pDependencyInfos)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (cmd_buffer->qf == RADV_QUEUE_VIDEO_DEC || cmd_buffer->qf == RADV_QUEUE_VIDEO_ENC)
      return;

   for (unsigned i = 0; i < eventCount; ++i) {
      VK_FROM_HANDLE(radv_event, event, pEvents[i]);
      uint64_t va = radv_buffer_get_va(event->bo);

      radv_cs_add_buffer(device->ws, cs, event->bo);

      ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs, 7);

      radv_cp_wait_mem(cs, cmd_buffer->qf, WAIT_REG_MEM_EQUAL, va, 1, 0xffffffff);
      assert(cmd_buffer->cs->cdw <= cdw_max);
   }

   radv_barrier(cmd_buffer, eventCount, pDependencyInfos, RGP_BARRIER_EXTERNAL_CMD_WAIT_EVENTS);
}

void
radv_emit_set_predication_state(struct radv_cmd_buffer *cmd_buffer, bool draw_visible, unsigned pred_op, uint64_t va)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t op = 0;

   radeon_check_space(device->ws, cmd_buffer->cs, 4);

   if (va) {
      assert(pred_op == PREDICATION_OP_BOOL32 || pred_op == PREDICATION_OP_BOOL64);

      op = PRED_OP(pred_op);

      /* PREDICATION_DRAW_VISIBLE means that if the 32-bit value is
       * zero, all rendering commands are discarded. Otherwise, they
       * are discarded if the value is non zero.
       */
      op |= draw_visible ? PREDICATION_DRAW_VISIBLE : PREDICATION_DRAW_NOT_VISIBLE;
   }
   if (pdev->info.gfx_level >= GFX9) {
      radeon_emit(cmd_buffer->cs, PKT3(PKT3_SET_PREDICATION, 2, 0));
      radeon_emit(cmd_buffer->cs, op);
      radeon_emit(cmd_buffer->cs, va);
      radeon_emit(cmd_buffer->cs, va >> 32);
   } else {
      radeon_emit(cmd_buffer->cs, PKT3(PKT3_SET_PREDICATION, 1, 0));
      radeon_emit(cmd_buffer->cs, va);
      radeon_emit(cmd_buffer->cs, op | ((va >> 32) & 0xFF));
   }
}

void
radv_begin_conditional_rendering(struct radv_cmd_buffer *cmd_buffer, uint64_t va, bool draw_visible)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned pred_op = PREDICATION_OP_BOOL32;

   radv_emit_cache_flush(cmd_buffer);

   if (cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      if (!pdev->info.has_32bit_predication) {
         uint64_t pred_value = 0, pred_va;
         unsigned pred_offset;

         /* From the Vulkan spec 1.1.107:
          *
          * "If the 32-bit value at offset in buffer memory is zero,
          *  then the rendering commands are discarded, otherwise they
          *  are executed as normal. If the value of the predicate in
          *  buffer memory changes while conditional rendering is
          *  active, the rendering commands may be discarded in an
          *  implementation-dependent way. Some implementations may
          *  latch the value of the predicate upon beginning conditional
          *  rendering while others may read it before every rendering
          *  command."
          *
          * But, the AMD hardware treats the predicate as a 64-bit
          * value which means we need a workaround in the driver.
          * Luckily, it's not required to support if the value changes
          * when predication is active.
          *
          * The workaround is as follows:
          * 1) allocate a 64-value in the upload BO and initialize it
          *    to 0
          * 2) copy the 32-bit predicate value to the upload BO
          * 3) use the new allocated VA address for predication
          *
          * Based on the conditionalrender demo, it's faster to do the
          * COPY_DATA in ME  (+ sync PFP) instead of PFP.
          */
         radv_cmd_buffer_upload_data(cmd_buffer, 8, &pred_value, &pred_offset);

         pred_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + pred_offset;

         radeon_check_space(device->ws, cmd_buffer->cs, 8);

         radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
         radeon_emit(
            cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM);
         radeon_emit(cs, va);
         radeon_emit(cs, va >> 32);
         radeon_emit(cs, pred_va);
         radeon_emit(cs, pred_va >> 32);

         radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
         radeon_emit(cs, 0);

         va = pred_va;
         pred_op = PREDICATION_OP_BOOL64;
      }

      radv_emit_set_predication_state(cmd_buffer, draw_visible, pred_op, va);
   } else {
      /* Compute queue doesn't support predication and it's emulated elsewhere. */
   }

   /* Store conditional rendering user info. */
   cmd_buffer->state.predicating = true;
   cmd_buffer->state.predication_type = draw_visible;
   cmd_buffer->state.predication_op = pred_op;
   cmd_buffer->state.predication_va = va;
   cmd_buffer->state.mec_inv_pred_emitted = false;
}

void
radv_end_conditional_rendering(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      radv_emit_set_predication_state(cmd_buffer, false, 0, 0);
   } else {
      /* Compute queue doesn't support predication, no need to emit anything here. */
   }

   /* Reset conditional rendering user info. */
   cmd_buffer->state.predicating = false;
   cmd_buffer->state.predication_type = -1;
   cmd_buffer->state.predication_op = 0;
   cmd_buffer->state.predication_va = 0;
   cmd_buffer->state.mec_inv_pred_emitted = false;
}

/* VK_EXT_conditional_rendering */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginConditionalRenderingEXT(VkCommandBuffer commandBuffer,
                                     const VkConditionalRenderingBeginInfoEXT *pConditionalRenderingBegin)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, pConditionalRenderingBegin->buffer);
   bool draw_visible = true;
   uint64_t va;

   va = radv_buffer_get_va(buffer->bo) + buffer->offset + pConditionalRenderingBegin->offset;

   /* By default, if the 32-bit value at offset in buffer memory is zero,
    * then the rendering commands are discarded, otherwise they are
    * executed as normal. If the inverted flag is set, all commands are
    * discarded if the value is non zero.
    */
   if (pConditionalRenderingBegin->flags & VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT) {
      draw_visible = false;
   }

   radv_begin_conditional_rendering(cmd_buffer, va, draw_visible);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_end_conditional_rendering(cmd_buffer);
}

/* VK_EXT_transform_feedback */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                                        const VkBuffer *pBuffers, const VkDeviceSize *pOffsets,
                                        const VkDeviceSize *pSizes)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   uint8_t enabled_mask = 0;

   assert(firstBinding + bindingCount <= MAX_SO_BUFFERS);
   for (uint32_t i = 0; i < bindingCount; i++) {
      uint32_t idx = firstBinding + i;

      sb[idx].buffer = radv_buffer_from_handle(pBuffers[i]);
      sb[idx].offset = pOffsets[i];

      if (!pSizes || pSizes[i] == VK_WHOLE_SIZE) {
         sb[idx].size = sb[idx].buffer->vk.size - sb[idx].offset;
      } else {
         sb[idx].size = pSizes[i];
      }

      radv_cs_add_buffer(device->ws, cmd_buffer->cs, sb[idx].buffer->bo);

      enabled_mask |= 1 << idx;
   }

   cmd_buffer->state.streamout.enabled_mask |= enabled_mask;

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;
}

static void
radv_set_streamout_enable(struct radv_cmd_buffer *cmd_buffer, bool enable)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   bool old_streamout_enabled = radv_is_streamout_enabled(cmd_buffer);
   uint32_t old_hw_enabled_mask = so->hw_enabled_mask;

   so->streamout_enabled = enable;

   so->hw_enabled_mask =
      so->enabled_mask | (so->enabled_mask << 4) | (so->enabled_mask << 8) | (so->enabled_mask << 12);

   if (!pdev->use_ngg_streamout && ((old_streamout_enabled != radv_is_streamout_enabled(cmd_buffer)) ||
                                    (old_hw_enabled_mask != so->hw_enabled_mask)))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;

   if (pdev->use_ngg_streamout) {
      /* Re-emit streamout desciptors because with NGG streamout, a buffer size of 0 acts like a
       * disable bit and this is needed when streamout needs to be ignored in shaders.
       */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY | RADV_CMD_DIRTY_STREAMOUT_BUFFER;
   }
}

static void
radv_flush_vgt_streamout(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned reg_strmout_cntl;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 14);

   /* The register is at different places on different ASICs. */
   if (pdev->info.gfx_level >= GFX9) {
      reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM_MAPPED_REGISTER) | S_370_ENGINE_SEL(V_370_ME));
      radeon_emit(cs, R_0300FC_CP_STRMOUT_CNTL >> 2);
      radeon_emit(cs, 0);
      radeon_emit(cs, 0);
   } else if (pdev->info.gfx_level >= GFX7) {
      reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
      radeon_set_uconfig_reg(cs, reg_strmout_cntl, 0);
   } else {
      reg_strmout_cntl = R_0084FC_CP_STRMOUT_CNTL;
      radeon_set_config_reg(cs, reg_strmout_cntl, 0);
   }

   radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
   radeon_emit(cs, EVENT_TYPE(V_028A90_SO_VGTSTREAMOUT_FLUSH) | EVENT_INDEX(0));

   radeon_emit(cs, PKT3(PKT3_WAIT_REG_MEM, 5, 0));
   radeon_emit(cs, WAIT_REG_MEM_EQUAL);    /* wait until the register is equal to the reference value */
   radeon_emit(cs, reg_strmout_cntl >> 2); /* register */
   radeon_emit(cs, 0);
   radeon_emit(cs, S_0084FC_OFFSET_UPDATE_DONE(1)); /* reference value */
   radeon_emit(cs, S_0084FC_OFFSET_UPDATE_DONE(1)); /* mask */
   radeon_emit(cs, 4);                              /* poll interval */

   assert(cs->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
                                  uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
                                  const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   bool first_target = true;

   assert(firstCounterBuffer + counterBufferCount <= MAX_SO_BUFFERS);
   if (!pdev->use_ngg_streamout)
      radv_flush_vgt_streamout(cmd_buffer);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, MAX_SO_BUFFERS * 10);

   u_foreach_bit (i, so->enabled_mask) {
      int32_t counter_buffer_idx = i - firstCounterBuffer;
      if (counter_buffer_idx >= 0 && counter_buffer_idx >= counterBufferCount)
         counter_buffer_idx = -1;

      bool append = counter_buffer_idx >= 0 && pCounterBuffers && pCounterBuffers[counter_buffer_idx];
      uint64_t va = 0;

      if (append) {
         VK_FROM_HANDLE(radv_buffer, buffer, pCounterBuffers[counter_buffer_idx]);
         uint64_t counter_buffer_offset = 0;

         if (pCounterBufferOffsets)
            counter_buffer_offset = pCounterBufferOffsets[counter_buffer_idx];

         va += radv_buffer_get_va(buffer->bo);
         va += buffer->offset + counter_buffer_offset;

         radv_cs_add_buffer(device->ws, cs, buffer->bo);
      }

      if (pdev->info.gfx_level >= GFX12) {
         /* Only the first streamout target holds information. */
         if (first_target) {
            if (append) {
               radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
               radeon_emit(
                  cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) | COPY_DATA_WR_CONFIRM);
               radeon_emit(cs, va);
               radeon_emit(cs, va >> 32);
               radeon_emit(cs, (R_0309B0_GE_GS_ORDERED_ID_BASE >> 2));
               radeon_emit(cs, 0);
            } else {
               radeon_set_uconfig_reg(cs, R_0309B0_GE_GS_ORDERED_ID_BASE, 0);
            }

            first_target = false;
         }
      } else if (pdev->use_ngg_streamout) {
         if (append) {
            radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
            radeon_emit(cs,
                        COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) | COPY_DATA_WR_CONFIRM);
            radeon_emit(cs, va);
            radeon_emit(cs, va >> 32);
            radeon_emit(cs, (R_031088_GDS_STRMOUT_DWORDS_WRITTEN_0 >> 2) + i);
            radeon_emit(cs, 0);
         } else {
            /* The PKT3 CAM bit workaround seems needed for initializing this GDS register to zero. */
            radeon_set_uconfig_perfctr_reg(pdev->info.gfx_level, cmd_buffer->qf, cs,
                                           R_031088_GDS_STRMOUT_DWORDS_WRITTEN_0 + i * 4, 0);
         }
      } else {
         /* AMD GCN binds streamout buffers as shader resources.
          * VGT only counts primitives and tells the shader through
          * SGPRs what to do.
          */
         radeon_set_context_reg(cs, R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16 * i, sb[i].size >> 2);

         cmd_buffer->state.context_roll_without_scissor_emitted = true;

         if (append) {
            radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) |   /* offset in bytes */
                               STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_MEM)); /* control */
            radeon_emit(cs, 0);                                                 /* unused */
            radeon_emit(cs, 0);                                                 /* unused */
            radeon_emit(cs, va);                                                /* src address lo */
            radeon_emit(cs, va >> 32);                                          /* src address hi */
         } else {
            /* Start from the beginning. */
            radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) |      /* offset in bytes */
                               STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_PACKET)); /* control */
            radeon_emit(cs, 0);                                                    /* unused */
            radeon_emit(cs, 0);                                                    /* unused */
            radeon_emit(cs, 0);                                                    /* unused */
            radeon_emit(cs, 0);                                                    /* unused */
         }
      }
   }

   assert(cs->cdw <= cdw_max);

   radv_set_streamout_enable(cmd_buffer, true);

   if (!pdev->use_ngg_streamout)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer, uint32_t counterBufferCount,
                                const VkBuffer *pCounterBuffers, const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   assert(firstCounterBuffer + counterBufferCount <= MAX_SO_BUFFERS);

   if (pdev->info.gfx_level >= GFX12) {
      /* Nothing to do. The streamout state buffer already contains the next ordered ID, which
       * is the only thing we need to restore.
       */
      radv_set_streamout_enable(cmd_buffer, false);
      return;
   }

   if (pdev->use_ngg_streamout) {
      /* Wait for streamout to finish before reading GDS_STRMOUT registers. */
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
      radv_emit_cache_flush(cmd_buffer);
   } else {
      radv_flush_vgt_streamout(cmd_buffer);
   }

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, MAX_SO_BUFFERS * 12);

   u_foreach_bit (i, so->enabled_mask) {
      int32_t counter_buffer_idx = i - firstCounterBuffer;
      if (counter_buffer_idx >= 0 && counter_buffer_idx >= counterBufferCount)
         counter_buffer_idx = -1;

      bool append = counter_buffer_idx >= 0 && pCounterBuffers && pCounterBuffers[counter_buffer_idx];
      uint64_t va = 0;

      if (append) {
         VK_FROM_HANDLE(radv_buffer, buffer, pCounterBuffers[counter_buffer_idx]);
         uint64_t counter_buffer_offset = 0;

         if (pCounterBufferOffsets)
            counter_buffer_offset = pCounterBufferOffsets[counter_buffer_idx];

         va += radv_buffer_get_va(buffer->bo);
         va += buffer->offset + counter_buffer_offset;

         radv_cs_add_buffer(device->ws, cs, buffer->bo);
      }

      if (pdev->use_ngg_streamout) {
         if (append) {
            radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
            radeon_emit(cs,
                        COPY_DATA_SRC_SEL(COPY_DATA_REG) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM);
            radeon_emit(cs, (R_031088_GDS_STRMOUT_DWORDS_WRITTEN_0 >> 2) + i);
            radeon_emit(cs, 0);
            radeon_emit(cs, va);
            radeon_emit(cs, va >> 32);
         }
      } else {
         if (append) {
            radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) | /* offset in bytes */
                               STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_NONE) |
                               STRMOUT_STORE_BUFFER_FILLED_SIZE); /* control */
            radeon_emit(cs, va);                                  /* dst address lo */
            radeon_emit(cs, va >> 32);                            /* dst address hi */
            radeon_emit(cs, 0);                                   /* unused */
            radeon_emit(cs, 0);                                   /* unused */
         }

         /* Deactivate transform feedback by zeroing the buffer size.
          * The counters (primitives generated, primitives emitted) may
          * be enabled even if there is not buffer bound. This ensures
          * that the primitives-emitted query won't increment.
          */
         radeon_set_context_reg(cs, R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16 * i, 0);

         cmd_buffer->state.context_roll_without_scissor_emitted = true;
      }
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);

   radv_set_streamout_enable(cmd_buffer, false);
}

static void
radv_emit_strmout_buffer(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *draw_info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   uint64_t va = radv_buffer_get_va(draw_info->strmout_buffer->bo);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   va += draw_info->strmout_buffer->offset + draw_info->strmout_buffer_offset;

   radeon_set_context_reg(cs, R_028B30_VGT_STRMOUT_DRAW_OPAQUE_VERTEX_STRIDE, draw_info->stride);

   if (gfx_level >= GFX10) {
      /* Emitting a COPY_DATA packet should be enough because RADV doesn't support preemption
       * (shadow memory) but for unknown reasons, it can lead to GPU hangs on GFX10+.
       */
      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);

      radeon_emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, (R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(cs, 1); /* 1 DWORD */
   } else {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) | COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE >> 2);
      radeon_emit(cs, 0); /* unused */
   }

   radv_cs_add_buffer(device->ws, cs, draw_info->strmout_buffer->bo);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount, uint32_t firstInstance,
                                 VkBuffer _counterBuffer, VkDeviceSize counterBufferOffset, uint32_t counterOffset,
                                 uint32_t vertexStride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, counterBuffer, _counterBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_draw_info info;

   info.count = 0;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = counterBuffer;
   info.strmout_buffer_offset = counterBufferOffset;
   info.stride = vertexStride;
   info.indexed = false;
   info.indirect = NULL;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   struct VkMultiDrawInfoEXT minfo = {0, 0};
   radv_emit_strmout_buffer(cmd_buffer, &info);
   radv_emit_direct_draw_packets(cmd_buffer, &info, 1, &minfo, S_0287F0_USE_OPAQUE(1), 0);

   if (pdev->info.gfx_level == GFX12) {
      /* DrawTransformFeedback requires 3 SQ_NON_EVENTs after the packet. */
      for (unsigned i = 0; i < 3; i++) {
         radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_SQ_NON_EVENT) | EVENT_INDEX(0));
      }
   }

   radv_after_draw(cmd_buffer, false);
}

/* VK_AMD_buffer_marker */
VKAPI_ATTR void VKAPI_CALL
radv_CmdWriteBufferMarker2AMD(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkBuffer dstBuffer,
                              VkDeviceSize dstOffset, uint32_t marker)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, dstBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const uint64_t va = radv_buffer_get_va(buffer->bo) + buffer->offset + dstOffset;

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      radeon_check_space(device->ws, cmd_buffer->cs, 4);
      radeon_emit(cmd_buffer->cs, SDMA_PACKET(SDMA_OPCODE_FENCE, 0, SDMA_FENCE_MTYPE_UC));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, marker);
      return;
   }

   radv_emit_cache_flush(cmd_buffer);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 12);

   if (!(stage & ~VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, marker);
      radeon_emit(cs, 0);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
   } else {
      radv_cs_emit_write_event_eop(cs, pdev->info.gfx_level, cmd_buffer->qf, V_028A90_BOTTOM_OF_PIPE_TS, 0,
                                   EOP_DST_SEL_MEM, EOP_DATA_SEL_VALUE_32BIT, va, marker, cmd_buffer->gfx9_eop_bug_va);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindPipelineShaderGroupNV(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                  VkPipeline pipeline, uint32_t groupIndex)
{
   fprintf(stderr, "radv: unimplemented vkCmdBindPipelineShaderGroupNV\n");
   abort();
}

/* VK_NV_device_generated_commands_compute */
VKAPI_ATTR void VKAPI_CALL
radv_CmdUpdatePipelineIndirectBufferNV(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                       VkPipeline _pipeline)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_pipeline, pipeline, _pipeline);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_compute_pipeline *compute_pipeline = radv_pipeline_to_compute(pipeline);
   const uint64_t va = compute_pipeline->indirect.va;
   struct radv_compute_pipeline_metadata metadata;

   radv_get_compute_shader_metadata(device, compute_pipeline->base.shaders[MESA_SHADER_COMPUTE], &metadata);

   assert(sizeof(metadata) <= compute_pipeline->indirect.size);
   radv_write_data(cmd_buffer, V_370_ME, va, sizeof(metadata) / 4, (const uint32_t *)&metadata, false);
}

/* VK_EXT_descriptor_buffer */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer, uint32_t bufferCount,
                                 const VkDescriptorBufferBindingInfoEXT *pBindingInfos)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   for (uint32_t i = 0; i < bufferCount; i++) {
      cmd_buffer->descriptor_buffers[i] = pBindingInfos[i].address;
   }
}

static void
radv_set_descriptor_buffer_offsets(struct radv_cmd_buffer *cmd_buffer,
                                   const VkSetDescriptorBufferOffsetsInfoEXT *pSetDescriptorBufferOffsetsInfo,
                                   VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   for (unsigned i = 0; i < pSetDescriptorBufferOffsetsInfo->setCount; i++) {
      const uint32_t buffer_idx = pSetDescriptorBufferOffsetsInfo->pBufferIndices[i];
      const uint64_t offset = pSetDescriptorBufferOffsetsInfo->pOffsets[i];
      unsigned idx = i + pSetDescriptorBufferOffsetsInfo->firstSet;

      descriptors_state->descriptor_buffers[idx] = cmd_buffer->descriptor_buffers[buffer_idx] + offset;

      radv_set_descriptor_set(cmd_buffer, bind_point, NULL, idx);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDescriptorBufferOffsets2EXT(VkCommandBuffer commandBuffer,
                                       const VkSetDescriptorBufferOffsetsInfoEXT *pSetDescriptorBufferOffsetsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   if (pSetDescriptorBufferOffsetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_set_descriptor_buffer_offsets(cmd_buffer, pSetDescriptorBufferOffsetsInfo, VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pSetDescriptorBufferOffsetsInfo->stageFlags & RADV_GRAPHICS_STAGE_BITS) {
      radv_set_descriptor_buffer_offsets(cmd_buffer, pSetDescriptorBufferOffsetsInfo, VK_PIPELINE_BIND_POINT_GRAPHICS);
   }

   if (pSetDescriptorBufferOffsetsInfo->stageFlags & RADV_RT_STAGE_BITS) {
      radv_set_descriptor_buffer_offsets(cmd_buffer, pSetDescriptorBufferOffsetsInfo,
                                         VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorBufferEmbeddedSamplers2EXT(
   VkCommandBuffer commandBuffer,
   const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *pBindDescriptorBufferEmbeddedSamplersInfo)
{
   /* This is a no-op because embedded samplers are inlined at compile time. */
}

/* VK_EXT_shader_object */
static void
radv_reset_pipeline_state(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint pipelineBindPoint)
{
   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      if (cmd_buffer->state.compute_pipeline) {
         radv_bind_shader(cmd_buffer, NULL, MESA_SHADER_COMPUTE);
         cmd_buffer->state.compute_pipeline = NULL;
      }
      if (cmd_buffer->state.emitted_compute_pipeline) {
         cmd_buffer->state.emitted_compute_pipeline = NULL;
      }
      break;
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      if (cmd_buffer->state.graphics_pipeline) {
         radv_foreach_stage(s, cmd_buffer->state.graphics_pipeline->active_stages)
         {
            radv_bind_shader(cmd_buffer, NULL, s);
         }
         cmd_buffer->state.graphics_pipeline = NULL;

         cmd_buffer->state.gs_copy_shader = NULL;
         cmd_buffer->state.last_vgt_shader = NULL;
         cmd_buffer->state.has_nggc = false;
         cmd_buffer->state.emitted_vs_prolog = NULL;
         cmd_buffer->state.spi_shader_col_format = 0;
         cmd_buffer->state.cb_shader_mask = 0;
         cmd_buffer->state.ms.sample_shading_enable = false;
         cmd_buffer->state.ms.min_sample_shading = 1.0f;
         cmd_buffer->state.rast_prim = 0;
         cmd_buffer->state.uses_out_of_order_rast = false;
         cmd_buffer->state.uses_vrs_attachment = false;
         cmd_buffer->state.uses_dynamic_vertex_binding_stride = false;
      }
      if (cmd_buffer->state.emitted_graphics_pipeline) {
         radv_bind_custom_blend_mode(cmd_buffer, 0);

         if (cmd_buffer->state.db_render_control) {
            cmd_buffer->state.db_render_control = 0;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
         }

         cmd_buffer->state.uses_vrs = false;
         cmd_buffer->state.uses_vrs_coarse_shading = false;

         cmd_buffer->state.emitted_graphics_pipeline = NULL;
      }
      break;
   default:
      break;
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PIPELINE;
}

static void
radv_bind_compute_shader(struct radv_cmd_buffer *cmd_buffer, struct radv_shader_object *shader_obj)
{
   struct radv_shader *shader = shader_obj ? shader_obj->shader : NULL;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   radv_bind_shader(cmd_buffer, shader, MESA_SHADER_COMPUTE);

   if (!shader_obj)
      return;

   ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 128);

   radv_emit_compute_shader(pdev, cs, shader);

   /* Update push constants/indirect descriptors state. */
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);
   struct radv_push_constant_state *pc_state = &cmd_buffer->push_constant_state[VK_PIPELINE_BIND_POINT_COMPUTE];

   descriptors_state->need_indirect_descriptor_sets =
      radv_get_user_sgpr_info(shader, AC_UD_INDIRECT_DESCRIPTOR_SETS)->sgpr_idx != -1;
   pc_state->size = shader_obj->push_constant_size;
   pc_state->dynamic_offset_count = shader_obj->dynamic_offset_count;

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindShadersEXT(VkCommandBuffer commandBuffer, uint32_t stageCount, const VkShaderStageFlagBits *pStages,
                       const VkShaderEXT *pShaders)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VkShaderStageFlagBits bound_stages = 0;

   for (uint32_t i = 0; i < stageCount; i++) {
      const gl_shader_stage stage = vk_to_mesa_shader_stage(pStages[i]);

      if (!pShaders) {
         cmd_buffer->state.shader_objs[stage] = NULL;
         continue;
      }

      VK_FROM_HANDLE(radv_shader_object, shader_obj, pShaders[i]);

      cmd_buffer->state.shader_objs[stage] = shader_obj;

      bound_stages |= pStages[i];
   }

   if (bound_stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_reset_pipeline_state(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);
      radv_mark_descriptor_sets_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);

      radv_bind_compute_shader(cmd_buffer, cmd_buffer->state.shader_objs[MESA_SHADER_COMPUTE]);
   }

   if (bound_stages & RADV_GRAPHICS_STAGE_BITS) {
      radv_reset_pipeline_state(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
      radv_mark_descriptor_sets_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);

      /* Graphics shaders are handled at draw time because of shader variants. */
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GRAPHICS_SHADERS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageModulationModeNV(VkCommandBuffer commandBuffer, VkCoverageModulationModeNV coverageModulationMode)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageModulationTableEnableNV(VkCommandBuffer commandBuffer, VkBool32 coverageModulationTableEnable)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageModulationTableNV(VkCommandBuffer commandBuffer, uint32_t coverageModulationTableCount,
                                     const float *pCoverageModulationTable)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageReductionModeNV(VkCommandBuffer commandBuffer, VkCoverageReductionModeNV coverageReductionMode)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageToColorEnableNV(VkCommandBuffer commandBuffer, VkBool32 coverageToColorEnable)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageToColorLocationNV(VkCommandBuffer commandBuffer, uint32_t coverageToColorLocation)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRepresentativeFragmentTestEnableNV(VkCommandBuffer commandBuffer, VkBool32 representativeFragmentTestEnable)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetShadingRateImageEnableNV(VkCommandBuffer commandBuffer, VkBool32 shadingRateImageEnable)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportSwizzleNV(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
                             const VkViewportSwizzleNV *pViewportSwizzles)
{
   unreachable("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportWScalingEnableNV(VkCommandBuffer commandBuffer, VkBool32 viewportWScalingEnable)
{
   unreachable("Not supported by RADV.");
}
