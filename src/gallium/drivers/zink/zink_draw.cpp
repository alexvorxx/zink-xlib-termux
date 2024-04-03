#include "zink_batch.h"
#include "zink_compiler.h"
#include "zink_context.h"
#include "zink_descriptors.h"
#include "zink_program.h"
#include "zink_program_state.hpp"
#include "zink_query.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_state.h"
#include "zink_surface.h"
#include "zink_inlines.h"

#include "tgsi/tgsi_from_mesa.h"
#include "util/hash_table.h"
#include "util/u_debug.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_prim.h"
#include "util/u_prim_restart.h"

static void
zink_emit_xfb_counter_barrier(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ctx->num_so_targets; i++) {
      struct zink_so_target *t = zink_so_target(ctx->so_targets[i]);
      if (!t)
         continue;
      struct zink_resource *res = zink_resource(t->counter_buffer);
      VkAccessFlags access = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
      VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
      if (t->counter_buffer_valid) {
         /* Between the pause and resume there needs to be a memory barrier for the counter buffers
          * with a source access of VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT
          * at pipeline stage VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT
          * to a destination access of VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT
          * at pipeline stage VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT.
          *
          * - from VK_EXT_transform_feedback spec
          */
         access |= VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;
         stage |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
      }
      zink_resource_buffer_barrier(ctx, res, access, stage);
      res->obj->unordered_read = false;
   }
}

static void
zink_emit_stream_output_targets(struct pipe_context *pctx)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_batch *batch = &ctx->batch;
   VkBuffer buffers[PIPE_MAX_SO_OUTPUTS] = {0};
   VkDeviceSize buffer_offsets[PIPE_MAX_SO_OUTPUTS] = {0};
   VkDeviceSize buffer_sizes[PIPE_MAX_SO_OUTPUTS] = {0};

   for (unsigned i = 0; i < ctx->num_so_targets; i++) {
      struct zink_so_target *t = (struct zink_so_target *)ctx->so_targets[i];
      if (!t) {
         /* no need to reference this or anything */
         buffers[i] = zink_resource(ctx->dummy_xfb_buffer)->obj->buffer;
         buffer_offsets[i] = 0;
         buffer_sizes[i] = sizeof(uint8_t);
         continue;
      }
      struct zink_resource *res = zink_resource(t->base.buffer);
      if (!res->so_valid)
         /* resource has been rebound */
         t->counter_buffer_valid = false;
      buffers[i] = res->obj->buffer;
      zink_batch_reference_resource_rw(batch, res, true);
      buffer_offsets[i] = t->base.buffer_offset;
      buffer_sizes[i] = t->base.buffer_size;
      res->so_valid = true;
      util_range_add(t->base.buffer, &res->valid_buffer_range, t->base.buffer_offset,
                     t->base.buffer_offset + t->base.buffer_size);
   }

   VKCTX(CmdBindTransformFeedbackBuffersEXT)(batch->state->cmdbuf, 0, ctx->num_so_targets,
                                                 buffers, buffer_offsets,
                                                 buffer_sizes);
   ctx->dirty_so_targets = false;
}

ALWAYS_INLINE static void
check_buffer_barrier(struct zink_context *ctx, struct pipe_resource *pres, VkAccessFlags flags, VkPipelineStageFlags pipeline)
{
   struct zink_resource *res = zink_resource(pres);
   zink_resource_buffer_barrier(ctx, res, flags, pipeline);
   res->obj->unordered_read = false;
}

ALWAYS_INLINE static void
barrier_draw_buffers(struct zink_context *ctx, const struct pipe_draw_info *dinfo,
                     const struct pipe_draw_indirect_info *dindirect, struct pipe_resource *index_buffer)
{
   if (index_buffer)
      check_buffer_barrier(ctx, index_buffer, VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
   if (dindirect && dindirect->buffer) {
      check_buffer_barrier(ctx, dindirect->buffer,
                           VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
      if (dindirect->indirect_draw_count)
         check_buffer_barrier(ctx, dindirect->indirect_draw_count,
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
   }
}

template <zink_dynamic_state DYNAMIC_STATE>
static void
zink_bind_vertex_buffers(struct zink_batch *batch, struct zink_context *ctx)
{
   VkBuffer buffers[PIPE_MAX_ATTRIBS];
   VkDeviceSize buffer_offsets[PIPE_MAX_ATTRIBS];
   VkDeviceSize buffer_strides[PIPE_MAX_ATTRIBS];
   struct zink_vertex_elements_state *elems = ctx->element_state;
   struct zink_screen *screen = zink_screen(ctx->base.screen);

   for (unsigned i = 0; i < elems->hw_state.num_bindings; i++) {
      struct pipe_vertex_buffer *vb = ctx->vertex_buffers + ctx->element_state->binding_map[i];
      assert(vb);
      if (vb->buffer.resource) {
         struct zink_resource *res = zink_resource(vb->buffer.resource);
         assert(res->obj->buffer);
         buffers[i] = res->obj->buffer;
         buffer_offsets[i] = vb->buffer_offset;
         buffer_strides[i] = vb->stride;
         if (DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT)
            elems->hw_state.dynbindings[i].stride = vb->stride;
      } else {
         buffers[i] = zink_resource(ctx->dummy_vertex_buffer)->obj->buffer;
         buffer_offsets[i] = 0;
         buffer_strides[i] = 0;
         if (DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT)
            elems->hw_state.dynbindings[i].stride = 0;
      }
   }

   if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE && DYNAMIC_STATE != ZINK_DYNAMIC_VERTEX_INPUT)
      VKCTX(CmdBindVertexBuffers2EXT)(batch->state->cmdbuf, 0,
                                          elems->hw_state.num_bindings,
                                          buffers, buffer_offsets, NULL, buffer_strides);
   else if (elems->hw_state.num_bindings)
      VKSCR(CmdBindVertexBuffers)(batch->state->cmdbuf, 0,
                             elems->hw_state.num_bindings,
                             buffers, buffer_offsets);

   if (DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT)
      VKCTX(CmdSetVertexInputEXT)(batch->state->cmdbuf,
                                      elems->hw_state.num_bindings, elems->hw_state.dynbindings,
                                      elems->hw_state.num_attribs, elems->hw_state.dynattribs);

   ctx->vertex_buffers_dirty = false;
}

static void
zink_bind_vertex_state(struct zink_batch *batch, struct zink_context *ctx,
                       struct pipe_vertex_state *vstate, uint32_t partial_velem_mask)
{
   if (!vstate->input.vbuffer.buffer.resource)
      return;

   const struct zink_vertex_elements_hw_state *hw_state = zink_vertex_state_mask(vstate, partial_velem_mask, true);
   assert(hw_state);

   struct zink_resource *res = zink_resource(vstate->input.vbuffer.buffer.resource);
   zink_batch_resource_usage_set(&ctx->batch, res, false, true);
   VkDeviceSize offset = vstate->input.vbuffer.buffer_offset;
   VKCTX(CmdBindVertexBuffers)(batch->state->cmdbuf, 0,
                               hw_state->num_bindings,
                               &res->obj->buffer, &offset);

   VKCTX(CmdSetVertexInputEXT)(batch->state->cmdbuf,
                               hw_state->num_bindings, hw_state->dynbindings,
                               hw_state->num_attribs, hw_state->dynattribs);
}

ALWAYS_INLINE static void
update_drawid(struct zink_context *ctx, unsigned draw_id)
{
   VKCTX(CmdPushConstants)(ctx->batch.state->cmdbuf, ctx->curr_program->base.layout, VK_SHADER_STAGE_VERTEX_BIT,
                      offsetof(struct zink_gfx_push_constant, draw_id), sizeof(unsigned),
                      &draw_id);
}

ALWAYS_INLINE static void
draw_indexed_need_index_buffer_unref(struct zink_context *ctx,
             const struct pipe_draw_info *dinfo,
             const struct pipe_draw_start_count_bias *draws,
             unsigned num_draws,
             unsigned draw_id,
             bool needs_drawid)
{
   VkCommandBuffer cmdbuf = ctx->batch.state->cmdbuf;
   if (dinfo->increment_draw_id && needs_drawid) {
      for (unsigned i = 0; i < num_draws; i++) {
         update_drawid(ctx, draw_id);
         VKCTX(CmdDrawIndexed)(cmdbuf,
            draws[i].count, dinfo->instance_count,
            0, draws[i].index_bias, dinfo->start_instance);
         draw_id++;
      }
   } else {
      if (needs_drawid)
         update_drawid(ctx, draw_id);
      for (unsigned i = 0; i < num_draws; i++)
         VKCTX(CmdDrawIndexed)(cmdbuf,
            draws[i].count, dinfo->instance_count,
            0, draws[i].index_bias, dinfo->start_instance);

   }
}

template <zink_multidraw HAS_MULTIDRAW>
ALWAYS_INLINE static void
draw_indexed(struct zink_context *ctx,
             const struct pipe_draw_info *dinfo,
             const struct pipe_draw_start_count_bias *draws,
             unsigned num_draws,
             unsigned draw_id,
             bool needs_drawid)
{
   VkCommandBuffer cmdbuf = ctx->batch.state->cmdbuf;
   if (dinfo->increment_draw_id && needs_drawid) {
      for (unsigned i = 0; i < num_draws; i++) {
         update_drawid(ctx, draw_id);
         VKCTX(CmdDrawIndexed)(cmdbuf,
            draws[i].count, dinfo->instance_count,
            draws[i].start, draws[i].index_bias, dinfo->start_instance);
         draw_id++;
      }
   } else {
      if (needs_drawid)
         update_drawid(ctx, draw_id);
      if (HAS_MULTIDRAW) {
         VKCTX(CmdDrawMultiIndexedEXT)(cmdbuf, num_draws, (const VkMultiDrawIndexedInfoEXT*)draws,
                                       dinfo->instance_count,
                                       dinfo->start_instance, sizeof(struct pipe_draw_start_count_bias),
                                       dinfo->index_bias_varies ? NULL : &draws[0].index_bias);
      } else {
         for (unsigned i = 0; i < num_draws; i++)
            VKCTX(CmdDrawIndexed)(cmdbuf,
               draws[i].count, dinfo->instance_count,
               draws[i].start, draws[i].index_bias, dinfo->start_instance);
      }
   }
}

template <zink_multidraw HAS_MULTIDRAW>
ALWAYS_INLINE static void
draw(struct zink_context *ctx,
     const struct pipe_draw_info *dinfo,
     const struct pipe_draw_start_count_bias *draws,
     unsigned num_draws,
     unsigned draw_id,
     bool needs_drawid)
{
   VkCommandBuffer cmdbuf = ctx->batch.state->cmdbuf;
   if (dinfo->increment_draw_id && needs_drawid) {
      for (unsigned i = 0; i < num_draws; i++) {
         update_drawid(ctx, draw_id);
         VKCTX(CmdDraw)(cmdbuf, draws[i].count, dinfo->instance_count, draws[i].start, dinfo->start_instance);
         draw_id++;
      }
   } else {
      if (needs_drawid)
         update_drawid(ctx, draw_id);
      if (HAS_MULTIDRAW)
         VKCTX(CmdDrawMultiEXT)(cmdbuf, num_draws, (const VkMultiDrawInfoEXT*)draws,
                                dinfo->instance_count, dinfo->start_instance,
                                sizeof(struct pipe_draw_start_count_bias));
      else {
         for (unsigned i = 0; i < num_draws; i++)
            VKCTX(CmdDraw)(cmdbuf, draws[i].count, dinfo->instance_count, draws[i].start, dinfo->start_instance);

      }
   }
}

static void
update_barriers(struct zink_context *ctx, bool is_compute,
                struct pipe_resource *index, struct pipe_resource *indirect, struct pipe_resource *indirect_draw_count)
{
   if (!ctx->need_barriers[is_compute]->entries)
      return;
   struct set *need_barriers = ctx->need_barriers[is_compute];
   ctx->barrier_set_idx[is_compute] = !ctx->barrier_set_idx[is_compute];
   ctx->need_barriers[is_compute] = &ctx->update_barriers[is_compute][ctx->barrier_set_idx[is_compute]];
   set_foreach(need_barriers, he) {
      struct zink_resource *res = (struct zink_resource *)he->key;
      if (res->bind_count[is_compute]) {
         VkPipelineStageFlagBits pipeline = is_compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : res->gfx_barrier;
         if (res->base.b.target == PIPE_BUFFER)
            zink_resource_buffer_barrier(ctx, res, res->barrier_access[is_compute], pipeline);
         else {
            VkImageLayout layout = zink_descriptor_util_image_layout_eval(ctx, res, is_compute);
            if (layout != res->layout)
               zink_resource_image_barrier(ctx, res, layout, res->barrier_access[is_compute], pipeline);
         }
         if (zink_resource_access_is_write(res->barrier_access[is_compute]))
            res->obj->unordered_read = res->obj->unordered_write = false;
         else
            res->obj->unordered_read = false;
         /* always barrier on draw if this resource has either multiple image write binds or
          * image write binds and image read binds
          */
         if (res->write_bind_count[is_compute] && res->bind_count[is_compute] > 1)
            _mesa_set_add_pre_hashed(ctx->need_barriers[is_compute], he->hash, res);
      }
      _mesa_set_remove(need_barriers, he);
      if (!need_barriers->entries)
         break;
   }
}

template <zink_dynamic_state DYNAMIC_STATE, bool BATCH_CHANGED>
static bool
update_gfx_pipeline(struct zink_context *ctx, struct zink_batch_state *bs, enum pipe_prim_type mode)
{
   VkPipeline prev_pipeline = ctx->gfx_pipeline_state.pipeline;
   zink_gfx_program_update(ctx);
   VkPipeline pipeline;
   if (zink_screen(ctx->base.screen)->info.have_EXT_graphics_pipeline_library)
      pipeline = zink_get_gfx_pipeline<DYNAMIC_STATE, true>(ctx, ctx->curr_program, &ctx->gfx_pipeline_state, mode);
   else
      pipeline = zink_get_gfx_pipeline<DYNAMIC_STATE, false>(ctx, ctx->curr_program, &ctx->gfx_pipeline_state, mode);
   bool pipeline_changed = prev_pipeline != pipeline;
   if (BATCH_CHANGED || pipeline_changed)
      VKCTX(CmdBindPipeline)(bs->cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   return pipeline_changed;
}

template <zink_multidraw HAS_MULTIDRAW, zink_dynamic_state DYNAMIC_STATE, bool BATCH_CHANGED, bool DRAW_STATE>
void
zink_draw(struct pipe_context *pctx,
          const struct pipe_draw_info *dinfo,
          unsigned drawid_offset,
          const struct pipe_draw_indirect_info *dindirect,
          const struct pipe_draw_start_count_bias *draws,
          unsigned num_draws,
          struct pipe_vertex_state *vstate,
          uint32_t partial_velem_mask)
{
   if (!dindirect && (!draws[0].count || !dinfo->instance_count))
      return;

   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_rasterizer_state *rast_state = ctx->rast_state;
   struct zink_depth_stencil_alpha_state *dsa_state = ctx->dsa_state;
   struct zink_batch *batch = &ctx->batch;
   struct zink_so_target *so_target =
      dindirect && dindirect->count_from_stream_output ?
         zink_so_target(dindirect->count_from_stream_output) : NULL;
   VkBuffer counter_buffers[PIPE_MAX_SO_OUTPUTS];
   VkDeviceSize counter_buffer_offsets[PIPE_MAX_SO_OUTPUTS];
   bool need_index_buffer_unref = false;
   bool mode_changed = ctx->gfx_pipeline_state.gfx_prim_mode != dinfo->mode;
   bool reads_drawid = ctx->shader_reads_drawid;
   bool reads_basevertex = ctx->shader_reads_basevertex;
   unsigned work_count = ctx->batch.work_count;
   enum pipe_prim_type mode = (enum pipe_prim_type)dinfo->mode;

   if (ctx->memory_barrier)
      zink_flush_memory_barrier(ctx, false);

   if (unlikely(ctx->buffer_rebind_counter < screen->buffer_rebind_counter)) {
      ctx->buffer_rebind_counter = screen->buffer_rebind_counter;
      zink_rebind_all_buffers(ctx);
   }

   if (unlikely(ctx->image_rebind_counter < screen->image_rebind_counter)) {
      ctx->image_rebind_counter = screen->image_rebind_counter;
      zink_rebind_all_images(ctx);
   }

   unsigned index_offset = 0;
   unsigned index_size = dinfo->index_size;
   struct pipe_resource *index_buffer = NULL;
   if (index_size > 0) {
      if (dinfo->has_user_indices) {
         if (!util_upload_index_buffer(pctx, dinfo, &draws[0], &index_buffer, &index_offset, 4)) {
            debug_printf("util_upload_index_buffer() failed\n");
            return;
         }
         /* this will have extra refs from tc */
         if (screen->threaded)
            zink_batch_reference_resource_move(batch, zink_resource(index_buffer));
         else
            zink_batch_reference_resource(batch, zink_resource(index_buffer));
      } else {
         index_buffer = dinfo->index.resource;
         zink_batch_reference_resource_rw(batch, zink_resource(index_buffer), false);
      }
      assert(index_size <= 4 && index_size != 3);
      assert(index_size != 1 || screen->info.have_EXT_index_type_uint8);
   }

   bool have_streamout = !!ctx->num_so_targets;
   if (have_streamout) {
      zink_emit_xfb_counter_barrier(ctx);
      if (ctx->dirty_so_targets) {
         /* have to loop here and below because barriers must be emitted out of renderpass,
          * but xfb buffers can't be bound before the renderpass is active to avoid
          * breaking from recursion
          */
         for (unsigned i = 0; i < ctx->num_so_targets; i++) {
            struct zink_so_target *t = (struct zink_so_target *)ctx->so_targets[i];
            if (t) {
               struct zink_resource *res = zink_resource(t->base.buffer);
               zink_resource_buffer_barrier(ctx, res,
                                            VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT);
               res->obj->unordered_read = res->obj->unordered_write = false;
            }
         }
      }
   }

   barrier_draw_buffers(ctx, dinfo, dindirect, index_buffer);
   /* this may re-emit draw buffer barriers, but such synchronization is harmless */
   update_barriers(ctx, false, index_buffer, dindirect ? dindirect->buffer : NULL, dindirect ? dindirect->indirect_draw_count : NULL);

   /* ensure synchronization between doing streamout with counter buffer
    * and using counter buffer for indirect draw
    */
   if (so_target && so_target->counter_buffer_valid) {
      struct zink_resource *res = zink_resource(so_target->counter_buffer);
      zink_resource_buffer_barrier(ctx, res,
                                   VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT,
                                   VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
      res->obj->unordered_read = false;
   }

   zink_query_update_gs_states(ctx, dinfo->was_line_loop);

   if (unlikely(zink_debug & ZINK_DEBUG_SYNC)) {
      zink_batch_no_rp(ctx);
      VkMemoryBarrier mb;
      mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      mb.pNext = NULL;
      mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      VKSCR(CmdPipelineBarrier)(ctx->batch.state->cmdbuf,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                0, 1, &mb, 0, NULL, 0, NULL);
   }

   zink_batch_rp(ctx);
   /* check dead swapchain */
   if (unlikely(!ctx->batch.in_rp))
      return;

   if (BATCH_CHANGED)
      zink_update_descriptor_refs(ctx, false);

   /* these must be after renderpass start to avoid issues with recursion */
   bool drawid_broken = false;
   if (reads_drawid && (!dindirect || !dindirect->buffer))
      drawid_broken = (drawid_offset != 0 ||
                      (!HAS_MULTIDRAW && num_draws > 1) ||
                      (HAS_MULTIDRAW && num_draws > 1 && !dinfo->increment_draw_id));
   if (drawid_broken != zink_get_last_vertex_key(ctx)->push_drawid)
      zink_set_last_vertex_key(ctx)->push_drawid = drawid_broken;
   if (mode_changed) {
      bool points_changed = false;
      if (mode == PIPE_PRIM_POINTS) {
         ctx->gfx_pipeline_state.has_points++;
         points_changed = true;
      } else if (ctx->gfx_pipeline_state.gfx_prim_mode == PIPE_PRIM_POINTS) {
         ctx->gfx_pipeline_state.has_points--;
         points_changed = true;
      }
      if (points_changed && ctx->rast_state->base.point_quad_rasterization)
         zink_set_fs_point_coord_key(ctx);
   }
   ctx->gfx_pipeline_state.gfx_prim_mode = mode;

   if (index_size) {
      const VkIndexType index_type[3] = {
         VK_INDEX_TYPE_UINT8_EXT,
         VK_INDEX_TYPE_UINT16,
         VK_INDEX_TYPE_UINT32,
      };
      struct zink_resource *res = zink_resource(index_buffer);
      VKCTX(CmdBindIndexBuffer)(batch->state->cmdbuf, res->obj->buffer, index_offset, index_type[index_size >> 1]);
   }
   if (DYNAMIC_STATE < ZINK_DYNAMIC_STATE2) {
      if (ctx->gfx_pipeline_state.dyn_state2.primitive_restart != dinfo->primitive_restart)
         ctx->gfx_pipeline_state.dirty = true;
      ctx->gfx_pipeline_state.dyn_state2.primitive_restart = dinfo->primitive_restart;
   }

   if (have_streamout && ctx->dirty_so_targets)
      zink_emit_stream_output_targets(pctx);

   bool pipeline_changed = update_gfx_pipeline<DYNAMIC_STATE, BATCH_CHANGED>(ctx, batch->state, mode);

   if (BATCH_CHANGED || ctx->vp_state_changed || (DYNAMIC_STATE == ZINK_NO_DYNAMIC_STATE && pipeline_changed)) {
      VkViewport viewports[PIPE_MAX_VIEWPORTS];
      for (unsigned i = 0; i < ctx->vp_state.num_viewports; i++) {
         VkViewport viewport = {
            ctx->vp_state.viewport_states[i].translate[0] - ctx->vp_state.viewport_states[i].scale[0],
            ctx->vp_state.viewport_states[i].translate[1] - ctx->vp_state.viewport_states[i].scale[1],
            MAX2(ctx->vp_state.viewport_states[i].scale[0] * 2, 1),
            ctx->vp_state.viewport_states[i].scale[1] * 2,
            CLAMP(ctx->rast_state->base.clip_halfz ?
                  ctx->vp_state.viewport_states[i].translate[2] :
                  ctx->vp_state.viewport_states[i].translate[2] - ctx->vp_state.viewport_states[i].scale[2],
                  0, 1),
            CLAMP(ctx->vp_state.viewport_states[i].translate[2] + ctx->vp_state.viewport_states[i].scale[2],
                  0, 1)
         };
         if (!ctx->rast_state->base.half_pixel_center) {
             /* magic constant value from dxvk */
             float cf = 0.5f - (1.0f / 128.0f);
             viewport.x += cf;
             if (viewport.height < 0)
                viewport.y += cf;
             else
                viewport.y -= cf;
         }
         viewports[i] = viewport;
      }
      if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE)
         VKCTX(CmdSetViewportWithCountEXT)(batch->state->cmdbuf, ctx->vp_state.num_viewports, viewports);
      else
         VKCTX(CmdSetViewport)(batch->state->cmdbuf, 0, ctx->vp_state.num_viewports, viewports);
   }
   if (BATCH_CHANGED || ctx->scissor_changed || ctx->vp_state_changed || (DYNAMIC_STATE == ZINK_NO_DYNAMIC_STATE && pipeline_changed)) {
      VkRect2D scissors[PIPE_MAX_VIEWPORTS];
      if (ctx->rast_state->base.scissor) {
         for (unsigned i = 0; i < ctx->vp_state.num_viewports; i++) {
            scissors[i].offset.x = ctx->vp_state.scissor_states[i].minx;
            scissors[i].offset.y = ctx->vp_state.scissor_states[i].miny;
            scissors[i].extent.width = ctx->vp_state.scissor_states[i].maxx - ctx->vp_state.scissor_states[i].minx;
            scissors[i].extent.height = ctx->vp_state.scissor_states[i].maxy - ctx->vp_state.scissor_states[i].miny;
         }
      } else {
         for (unsigned i = 0; i < ctx->vp_state.num_viewports; i++) {
            scissors[i].offset.x = 0;
            scissors[i].offset.y = 0;
            scissors[i].extent.width = ctx->fb_state.width;
            scissors[i].extent.height = ctx->fb_state.height;
         }
      }
      if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE)
         VKCTX(CmdSetScissorWithCountEXT)(batch->state->cmdbuf, ctx->vp_state.num_viewports, scissors);
      else
         VKCTX(CmdSetScissor)(batch->state->cmdbuf, 0, ctx->vp_state.num_viewports, scissors);
   }
   ctx->vp_state_changed = false;
   ctx->scissor_changed = false;

   if (BATCH_CHANGED || ctx->stencil_ref_changed) {
      VKCTX(CmdSetStencilReference)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_BIT,
                               ctx->stencil_ref.ref_value[0]);
      VKCTX(CmdSetStencilReference)(batch->state->cmdbuf, VK_STENCIL_FACE_BACK_BIT,
                               ctx->stencil_ref.ref_value[1]);
      ctx->stencil_ref_changed = false;
   }

   if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE && (BATCH_CHANGED || ctx->dsa_state_changed)) {
      VKCTX(CmdSetDepthBoundsTestEnableEXT)(batch->state->cmdbuf, dsa_state->hw_state.depth_bounds_test);
      if (dsa_state->hw_state.depth_bounds_test)
         VKCTX(CmdSetDepthBounds)(batch->state->cmdbuf,
                             dsa_state->hw_state.min_depth_bounds,
                             dsa_state->hw_state.max_depth_bounds);
      VKCTX(CmdSetDepthTestEnableEXT)(batch->state->cmdbuf, dsa_state->hw_state.depth_test);
      if (dsa_state->hw_state.depth_test)
         VKCTX(CmdSetDepthCompareOpEXT)(batch->state->cmdbuf, dsa_state->hw_state.depth_compare_op);
      VKCTX(CmdSetDepthWriteEnableEXT)(batch->state->cmdbuf, dsa_state->hw_state.depth_write);
      VKCTX(CmdSetStencilTestEnableEXT)(batch->state->cmdbuf, dsa_state->hw_state.stencil_test);
      if (dsa_state->hw_state.stencil_test) {
         VKCTX(CmdSetStencilOpEXT)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_BIT,
                                       dsa_state->hw_state.stencil_front.failOp,
                                       dsa_state->hw_state.stencil_front.passOp,
                                       dsa_state->hw_state.stencil_front.depthFailOp,
                                       dsa_state->hw_state.stencil_front.compareOp);
         VKCTX(CmdSetStencilOpEXT)(batch->state->cmdbuf, VK_STENCIL_FACE_BACK_BIT,
                                       dsa_state->hw_state.stencil_back.failOp,
                                       dsa_state->hw_state.stencil_back.passOp,
                                       dsa_state->hw_state.stencil_back.depthFailOp,
                                       dsa_state->hw_state.stencil_back.compareOp);
         if (dsa_state->base.stencil[1].enabled) {
            VKCTX(CmdSetStencilWriteMask)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_BIT, dsa_state->hw_state.stencil_front.writeMask);
            VKCTX(CmdSetStencilWriteMask)(batch->state->cmdbuf, VK_STENCIL_FACE_BACK_BIT, dsa_state->hw_state.stencil_back.writeMask);
            VKCTX(CmdSetStencilCompareMask)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_BIT, dsa_state->hw_state.stencil_front.compareMask);
            VKCTX(CmdSetStencilCompareMask)(batch->state->cmdbuf, VK_STENCIL_FACE_BACK_BIT, dsa_state->hw_state.stencil_back.compareMask);
         } else {
            VKCTX(CmdSetStencilWriteMask)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_AND_BACK, dsa_state->hw_state.stencil_front.writeMask);
            VKCTX(CmdSetStencilCompareMask)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_AND_BACK, dsa_state->hw_state.stencil_front.compareMask);
         }
      } else {
         VKCTX(CmdSetStencilWriteMask)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_AND_BACK, dsa_state->hw_state.stencil_front.writeMask);
         VKCTX(CmdSetStencilCompareMask)(batch->state->cmdbuf, VK_STENCIL_FACE_FRONT_AND_BACK, dsa_state->hw_state.stencil_front.compareMask);
      }
   }
   ctx->dsa_state_changed = false;

   bool rast_state_changed = ctx->rast_state_changed;
   if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE && (BATCH_CHANGED || rast_state_changed)) {
      VKCTX(CmdSetFrontFaceEXT)(batch->state->cmdbuf, (VkFrontFace)ctx->gfx_pipeline_state.dyn_state1.front_face);
      VKCTX(CmdSetCullModeEXT)(batch->state->cmdbuf, ctx->gfx_pipeline_state.dyn_state1.cull_mode);
   }
   if ((BATCH_CHANGED || rast_state_changed) &&
       screen->info.have_EXT_line_rasterization && rast_state->base.line_stipple_enable)
      VKCTX(CmdSetLineStippleEXT)(batch->state->cmdbuf, rast_state->base.line_stipple_factor, rast_state->base.line_stipple_pattern);

   if (BATCH_CHANGED || ctx->rast_state_changed) {
      enum pipe_prim_type reduced_prim = ctx->last_vertex_stage->reduced_prim;
      if (reduced_prim == PIPE_PRIM_MAX)
         reduced_prim = u_reduced_prim(mode);

      bool depth_bias = false;
      switch (reduced_prim) {
      case PIPE_PRIM_POINTS:
         depth_bias = rast_state->offset_point;
         break;

      case PIPE_PRIM_LINES:
         depth_bias = rast_state->offset_line;
         break;

      case PIPE_PRIM_TRIANGLES:
         depth_bias = rast_state->offset_tri;
         break;

      default:
         unreachable("unexpected reduced prim");
      }

      VKCTX(CmdSetLineWidth)(batch->state->cmdbuf, rast_state->line_width);
      if (depth_bias) {
         if (rast_state->base.offset_units_unscaled) {
            VKCTX(CmdSetDepthBias)(batch->state->cmdbuf, rast_state->offset_units * ctx->depth_bias_scale_factor, rast_state->offset_clamp, rast_state->offset_scale);
         } else {
            VKCTX(CmdSetDepthBias)(batch->state->cmdbuf, rast_state->offset_units, rast_state->offset_clamp, rast_state->offset_scale);
         }
      } else {
         VKCTX(CmdSetDepthBias)(batch->state->cmdbuf, 0.0f, 0.0f, 0.0f);
      }
   }
   ctx->rast_state_changed = false;

   if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE) {
      if (ctx->sample_locations_changed) {
         VkSampleLocationsInfoEXT loc;
         zink_init_vk_sample_locations(ctx, &loc);
         VKCTX(CmdSetSampleLocationsEXT)(batch->state->cmdbuf, &loc);
      }
      ctx->sample_locations_changed = false;
   }

   if ((BATCH_CHANGED || ctx->blend_state_changed) &&
       ctx->gfx_pipeline_state.blend_state->need_blend_constants) {
      VKCTX(CmdSetBlendConstants)(batch->state->cmdbuf, ctx->blend_constants);
   }
   ctx->blend_state_changed = false;

   if (DRAW_STATE)
      zink_bind_vertex_state(batch, ctx, vstate, partial_velem_mask);
   else if (BATCH_CHANGED || ctx->vertex_buffers_dirty) {
      if (DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT || ctx->gfx_pipeline_state.uses_dynamic_stride)
         zink_bind_vertex_buffers<DYNAMIC_STATE>(batch, ctx);
      else
         zink_bind_vertex_buffers<ZINK_NO_DYNAMIC_STATE>(batch, ctx);
   }

   if (BATCH_CHANGED) {
      ctx->pipeline_changed[0] = false;
      zink_select_draw_vbo(ctx);
   }

   if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE && (BATCH_CHANGED || mode_changed)) {
      VKCTX(CmdSetPrimitiveTopologyEXT)(batch->state->cmdbuf, zink_primitive_topology(mode));
   }

   if (DYNAMIC_STATE >= ZINK_DYNAMIC_STATE2 && (BATCH_CHANGED || ctx->primitive_restart != dinfo->primitive_restart)) {
      VKCTX(CmdSetPrimitiveRestartEnableEXT)(batch->state->cmdbuf, dinfo->primitive_restart);
      ctx->primitive_restart = dinfo->primitive_restart;
   }

   if (DYNAMIC_STATE >= ZINK_DYNAMIC_STATE2 && (BATCH_CHANGED || ctx->rasterizer_discard_changed)) {
      VKCTX(CmdSetRasterizerDiscardEnableEXT)(batch->state->cmdbuf, ctx->gfx_pipeline_state.dyn_state2.rasterizer_discard);
      ctx->rasterizer_discard_changed = false;
   }

   if (zink_program_has_descriptors(&ctx->curr_program->base))
      zink_descriptors_update(ctx, false);

   if (ctx->di.any_bindless_dirty &&
       /* some apps (d3dretrace) call MakeTextureHandleResidentARB randomly */
       zink_program_has_descriptors(&ctx->curr_program->base) &&
       ctx->curr_program->base.dd.bindless)
      zink_descriptors_update_bindless(ctx);

   if (reads_basevertex) {
      unsigned draw_mode_is_indexed = index_size > 0;
      VKCTX(CmdPushConstants)(batch->state->cmdbuf, ctx->curr_program->base.layout, VK_SHADER_STAGE_VERTEX_BIT,
                         offsetof(struct zink_gfx_push_constant, draw_mode_is_indexed), sizeof(unsigned),
                         &draw_mode_is_indexed);
   }
   if (ctx->curr_program->shaders[MESA_SHADER_TESS_CTRL] && ctx->curr_program->shaders[MESA_SHADER_TESS_CTRL]->is_generated) {
      VKCTX(CmdPushConstants)(batch->state->cmdbuf, ctx->curr_program->base.layout, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                         offsetof(struct zink_gfx_push_constant, default_inner_level), sizeof(float) * 6,
                         &ctx->tess_levels[0]);
   }

   if (have_streamout) {
      for (unsigned i = 0; i < ctx->num_so_targets; i++) {
         struct zink_so_target *t = zink_so_target(ctx->so_targets[i]);
         counter_buffers[i] = VK_NULL_HANDLE;
         if (t) {
            struct zink_resource *res = zink_resource(t->counter_buffer);
            t->stride = ctx->last_vertex_stage->sinfo.so_info.stride[i] * sizeof(uint32_t);
            zink_batch_reference_resource_rw(batch, res, true);
            res->obj->unordered_read = res->obj->unordered_write = false;
            if (t->counter_buffer_valid) {
               counter_buffers[i] = res->obj->buffer;
               counter_buffer_offsets[i] = t->counter_buffer_offset;
            }
         }
      }
      VKCTX(CmdBeginTransformFeedbackEXT)(batch->state->cmdbuf, 0, ctx->num_so_targets, counter_buffers, counter_buffer_offsets);
   }

   bool needs_drawid = reads_drawid && zink_get_last_vertex_key(ctx)->push_drawid;
   work_count += num_draws;
   if (index_size > 0) {
      if (dindirect && dindirect->buffer) {
         assert(num_draws == 1);
         if (needs_drawid)
            update_drawid(ctx, drawid_offset);
         struct zink_resource *indirect = zink_resource(dindirect->buffer);
         zink_batch_reference_resource_rw(batch, indirect, false);
         if (dindirect->indirect_draw_count) {
             struct zink_resource *indirect_draw_count = zink_resource(dindirect->indirect_draw_count);
             zink_batch_reference_resource_rw(batch, indirect_draw_count, false);
             VKCTX(CmdDrawIndexedIndirectCount)(batch->state->cmdbuf, indirect->obj->buffer, dindirect->offset,
                                                indirect_draw_count->obj->buffer, dindirect->indirect_draw_count_offset,
                                                dindirect->draw_count, dindirect->stride);
         } else
            VKCTX(CmdDrawIndexedIndirect)(batch->state->cmdbuf, indirect->obj->buffer, dindirect->offset, dindirect->draw_count, dindirect->stride);
      } else {
         if (need_index_buffer_unref)
            draw_indexed_need_index_buffer_unref(ctx, dinfo, draws, num_draws, drawid_offset, needs_drawid);
         else
            draw_indexed<HAS_MULTIDRAW>(ctx, dinfo, draws, num_draws, drawid_offset, needs_drawid);
      }
   } else {
      if (so_target && screen->info.tf_props.transformFeedbackDraw) {
         /* GTF-GL46.gtf40.GL3Tests.transform_feedback2.transform_feedback2_api attempts a bogus xfb
          * draw using a streamout target that has no data
          * to avoid hanging the gpu, reject any such draws
          */
         if (so_target->counter_buffer_valid) {
            if (needs_drawid)
               update_drawid(ctx, drawid_offset);
            zink_batch_reference_resource_rw(batch, zink_resource(so_target->base.buffer), false);
            zink_batch_reference_resource_rw(batch, zink_resource(so_target->counter_buffer), true);
            VKCTX(CmdDrawIndirectByteCountEXT)(batch->state->cmdbuf, dinfo->instance_count, dinfo->start_instance,
                                          zink_resource(so_target->counter_buffer)->obj->buffer, so_target->counter_buffer_offset, 0,
                                          MIN2(so_target->stride, screen->info.tf_props.maxTransformFeedbackBufferDataStride));
         }
      } else if (dindirect && dindirect->buffer) {
         assert(num_draws == 1);
         if (needs_drawid)
            update_drawid(ctx, drawid_offset);
         struct zink_resource *indirect = zink_resource(dindirect->buffer);
         zink_batch_reference_resource_rw(batch, indirect, false);
         if (dindirect->indirect_draw_count) {
             struct zink_resource *indirect_draw_count = zink_resource(dindirect->indirect_draw_count);
             zink_batch_reference_resource_rw(batch, indirect_draw_count, false);
             VKCTX(CmdDrawIndirectCount)(batch->state->cmdbuf, indirect->obj->buffer, dindirect->offset,
                                           indirect_draw_count->obj->buffer, dindirect->indirect_draw_count_offset,
                                           dindirect->draw_count, dindirect->stride);
         } else
            VKCTX(CmdDrawIndirect)(batch->state->cmdbuf, indirect->obj->buffer, dindirect->offset, dindirect->draw_count, dindirect->stride);
      } else {
         draw<HAS_MULTIDRAW>(ctx, dinfo, draws, num_draws, drawid_offset, needs_drawid);
      }
   }

   if (have_streamout) {
      for (unsigned i = 0; i < ctx->num_so_targets; i++) {
         struct zink_so_target *t = zink_so_target(ctx->so_targets[i]);
         if (t) {
            counter_buffers[i] = zink_resource(t->counter_buffer)->obj->buffer;
            counter_buffer_offsets[i] = t->counter_buffer_offset;
            t->counter_buffer_valid = true;
         }
      }
      VKCTX(CmdEndTransformFeedbackEXT)(batch->state->cmdbuf, 0, ctx->num_so_targets, counter_buffers, counter_buffer_offsets);
   }
   batch->has_work = true;
   batch->last_was_compute = false;
   ctx->batch.work_count = work_count;
   /* flush if there's >100k draws */
   if (unlikely(work_count >= 30000) || ctx->oom_flush)
      pctx->flush(pctx, NULL, 0);
}

template <zink_multidraw HAS_MULTIDRAW, zink_dynamic_state DYNAMIC_STATE, bool BATCH_CHANGED>
static void
zink_draw_vbo(struct pipe_context *pctx,
              const struct pipe_draw_info *info,
              unsigned drawid_offset,
              const struct pipe_draw_indirect_info *indirect,
              const struct pipe_draw_start_count_bias *draws,
              unsigned num_draws)
{
   zink_draw<HAS_MULTIDRAW, DYNAMIC_STATE, BATCH_CHANGED, false>(pctx, info, drawid_offset, indirect, draws, num_draws, NULL, 0);
}

template <zink_multidraw HAS_MULTIDRAW, zink_dynamic_state DYNAMIC_STATE, bool BATCH_CHANGED>
static void
zink_draw_vertex_state(struct pipe_context *pctx,
                       struct pipe_vertex_state *vstate,
                       uint32_t partial_velem_mask,
                       struct pipe_draw_vertex_state_info info,
                       const struct pipe_draw_start_count_bias *draws,
                       unsigned num_draws)
{
   struct pipe_draw_info dinfo = {};

   dinfo.mode = info.mode;
   dinfo.index_size = 4;
   dinfo.instance_count = 1;
   dinfo.index.resource = vstate->input.indexbuf;
   struct zink_context *ctx = zink_context(pctx);
   struct zink_resource *res = zink_resource(vstate->input.vbuffer.buffer.resource);
   zink_resource_buffer_barrier(ctx, res, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
   res->obj->unordered_read = false;
   struct zink_vertex_elements_hw_state *hw_state = ctx->gfx_pipeline_state.element_state;
   ctx->gfx_pipeline_state.element_state = &((struct zink_vertex_state*)vstate)->velems.hw_state;

   zink_draw<HAS_MULTIDRAW, DYNAMIC_STATE, BATCH_CHANGED, true>(pctx, &dinfo, 0, NULL, draws, num_draws, vstate, partial_velem_mask);
   ctx->gfx_pipeline_state.element_state = hw_state;

   if (info.take_vertex_state_ownership)
      pipe_vertex_state_reference(&vstate, NULL);
}

template <bool BATCH_CHANGED>
static void
zink_launch_grid(struct pipe_context *pctx, const struct pipe_grid_info *info)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_batch *batch = &ctx->batch;

   if (ctx->render_condition_active)
      zink_start_conditional_render(ctx);

   if (info->indirect) {
      /*
         VK_ACCESS_INDIRECT_COMMAND_READ_BIT specifies read access to indirect command data read as
         part of an indirect build, trace, drawing or dispatching command. Such access occurs in the
         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT pipeline stage.

         - Chapter 7. Synchronization and Cache Control
       */
      check_buffer_barrier(ctx, info->indirect, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
   }

   update_barriers(ctx, true, NULL, info->indirect, NULL);
   if (ctx->memory_barrier)
      zink_flush_memory_barrier(ctx, true);

   if (unlikely(zink_debug & ZINK_DEBUG_SYNC)) {
      zink_batch_no_rp(ctx);
      VkMemoryBarrier mb;
      mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      mb.pNext = NULL;
      mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      VKSCR(CmdPipelineBarrier)(ctx->batch.state->cmdbuf,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                0, 1, &mb, 0, NULL, 0, NULL);
   }

   zink_program_update_compute_pipeline_state(ctx, ctx->curr_compute, info->block);
   VkPipeline prev_pipeline = ctx->compute_pipeline_state.pipeline;

   if (BATCH_CHANGED) {
      zink_update_descriptor_refs(ctx, true);
      zink_batch_reference_program(&ctx->batch, &ctx->curr_compute->base);
   }
   if (ctx->dirty_shader_stages & BITFIELD_BIT(MESA_SHADER_COMPUTE)) {
      /* update inlinable constants */
      zink_update_compute_program(ctx);
      ctx->dirty_shader_stages &= ~BITFIELD_BIT(MESA_SHADER_COMPUTE);
   }

   VkPipeline pipeline = zink_get_compute_pipeline(screen, ctx->curr_compute,
                                               &ctx->compute_pipeline_state);

   if (prev_pipeline != pipeline || BATCH_CHANGED)
      VKCTX(CmdBindPipeline)(batch->state->cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   if (BATCH_CHANGED) {
      ctx->pipeline_changed[1] = false;
      zink_select_launch_grid(ctx);
   }

   if (zink_program_has_descriptors(&ctx->curr_compute->base))
      zink_descriptors_update(ctx, true);
   if (ctx->di.any_bindless_dirty && ctx->curr_compute->base.dd.bindless)
      zink_descriptors_update_bindless(ctx);

   if (BITSET_TEST(ctx->curr_compute->shader->nir->info.system_values_read, SYSTEM_VALUE_WORK_DIM))
      VKCTX(CmdPushConstants)(batch->state->cmdbuf, ctx->curr_compute->base.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                         offsetof(struct zink_cs_push_constant, work_dim), sizeof(uint32_t),
                         &info->work_dim);

   batch->work_count++;
   zink_batch_no_rp(ctx);
   if (info->indirect) {
      VKCTX(CmdDispatchIndirect)(batch->state->cmdbuf, zink_resource(info->indirect)->obj->buffer, info->indirect_offset);
      zink_batch_reference_resource_rw(batch, zink_resource(info->indirect), false);
   } else
      VKCTX(CmdDispatch)(batch->state->cmdbuf, info->grid[0], info->grid[1], info->grid[2]);
   batch->has_work = true;
   batch->last_was_compute = true;
   /* flush if there's >100k computes */
   if (unlikely(ctx->batch.work_count >= 30000) || ctx->oom_flush)
      pctx->flush(pctx, NULL, 0);
}

template <zink_multidraw HAS_MULTIDRAW, zink_dynamic_state DYNAMIC_STATE, bool BATCH_CHANGED>
static void
init_batch_changed_functions(struct zink_context *ctx, pipe_draw_vbo_func draw_vbo_array[2][4][2], pipe_draw_vertex_state_func draw_state_array[2][4][2])
{
   draw_vbo_array[HAS_MULTIDRAW][DYNAMIC_STATE][BATCH_CHANGED] = zink_draw_vbo<HAS_MULTIDRAW, DYNAMIC_STATE, BATCH_CHANGED>;
   draw_state_array[HAS_MULTIDRAW][DYNAMIC_STATE][BATCH_CHANGED] = zink_draw_vertex_state<HAS_MULTIDRAW, DYNAMIC_STATE, BATCH_CHANGED>;
}

template <zink_multidraw HAS_MULTIDRAW, zink_dynamic_state DYNAMIC_STATE>
static void
init_dynamic_state_functions(struct zink_context *ctx, pipe_draw_vbo_func draw_vbo_array[2][4][2], pipe_draw_vertex_state_func draw_state_array[2][4][2])
{
   init_batch_changed_functions<HAS_MULTIDRAW, DYNAMIC_STATE, false>(ctx, draw_vbo_array, draw_state_array);
   init_batch_changed_functions<HAS_MULTIDRAW, DYNAMIC_STATE, true>(ctx, draw_vbo_array, draw_state_array);
}

template <zink_multidraw HAS_MULTIDRAW>
static void
init_multidraw_functions(struct zink_context *ctx, pipe_draw_vbo_func draw_vbo_array[2][4][2], pipe_draw_vertex_state_func draw_state_array[2][4][2])
{
   init_dynamic_state_functions<HAS_MULTIDRAW, ZINK_NO_DYNAMIC_STATE>(ctx, draw_vbo_array, draw_state_array);
   init_dynamic_state_functions<HAS_MULTIDRAW, ZINK_DYNAMIC_STATE>(ctx, draw_vbo_array, draw_state_array);
   init_dynamic_state_functions<HAS_MULTIDRAW, ZINK_DYNAMIC_STATE2>(ctx, draw_vbo_array, draw_state_array);
   init_dynamic_state_functions<HAS_MULTIDRAW, ZINK_DYNAMIC_VERTEX_INPUT>(ctx, draw_vbo_array, draw_state_array);
}

static void
init_all_draw_functions(struct zink_context *ctx, pipe_draw_vbo_func draw_vbo_array[2][4][2], pipe_draw_vertex_state_func draw_state_array[2][4][2])
{
   init_multidraw_functions<ZINK_NO_MULTIDRAW>(ctx, draw_vbo_array, draw_state_array);
   init_multidraw_functions<ZINK_MULTIDRAW>(ctx, draw_vbo_array, draw_state_array);
}

template <bool BATCH_CHANGED>
static void
init_grid_batch_changed_functions(struct zink_context *ctx)
{
   ctx->launch_grid[BATCH_CHANGED] = zink_launch_grid<BATCH_CHANGED>;
}

static void
init_all_grid_functions(struct zink_context *ctx)
{
   init_grid_batch_changed_functions<false>(ctx);
   init_grid_batch_changed_functions<true>(ctx);
}

static void
zink_invalid_draw_vbo(struct pipe_context *pipe,
                      const struct pipe_draw_info *dinfo,
                      unsigned drawid_offset,
                      const struct pipe_draw_indirect_info *dindirect,
                      const struct pipe_draw_start_count_bias *draws,
                      unsigned num_draws)
{
   unreachable("vertex shader not bound");
}

static void
zink_invalid_draw_vertex_state(struct pipe_context *pipe,
                               struct pipe_vertex_state *vstate,
                               uint32_t partial_velem_mask,
                               struct pipe_draw_vertex_state_info info,
                               const struct pipe_draw_start_count_bias *draws,
                               unsigned num_draws)
{
   unreachable("vertex shader not bound");
}

static void
zink_invalid_launch_grid(struct pipe_context *pctx, const struct pipe_grid_info *info)
{
   unreachable("compute shader not bound");
}

#define STAGE_BASE 0
#define STAGE_BASE_GS (BITFIELD_BIT(MESA_SHADER_GEOMETRY) >> 1)
#define STAGE_BASE_TES (BITFIELD_BIT(MESA_SHADER_TESS_EVAL) >> 1)
#define STAGE_BASE_TES_GS ((BITFIELD_BIT(MESA_SHADER_TESS_EVAL) | BITFIELD_BIT(MESA_SHADER_GEOMETRY)) >> 1)
#define STAGE_BASE_TCS_TES ((BITFIELD_BIT(MESA_SHADER_TESS_CTRL) | BITFIELD_BIT(MESA_SHADER_TESS_EVAL)) >> 1)
#define STAGE_BASE_TCS_TES_GS ((BITFIELD_BIT(MESA_SHADER_TESS_CTRL) | BITFIELD_BIT(MESA_SHADER_TESS_EVAL) | BITFIELD_BIT(MESA_SHADER_GEOMETRY)) >> 1)

template <unsigned STAGE_MASK>
static uint32_t
hash_gfx_program(const void *key)
{
   const struct zink_shader **shaders = (const struct zink_shader**)key;
   uint32_t base_hash = shaders[MESA_SHADER_VERTEX]->hash ^ shaders[MESA_SHADER_FRAGMENT]->hash;
   if (STAGE_MASK == STAGE_BASE) //VS+FS
      return base_hash;
   if (STAGE_MASK == STAGE_BASE_GS) //VS+GS+FS
      return base_hash ^ shaders[MESA_SHADER_GEOMETRY]->hash;
   /*VS+TCS+FS isn't a thing */
   /*VS+TCS+GS+FS isn't a thing */
   if (STAGE_MASK == STAGE_BASE_TES) //VS+TES+FS
      return base_hash ^ shaders[MESA_SHADER_TESS_EVAL]->hash;
   if (STAGE_MASK == STAGE_BASE_TES_GS) //VS+TES+GS+FS
      return base_hash ^ shaders[MESA_SHADER_GEOMETRY]->hash ^ shaders[MESA_SHADER_TESS_EVAL]->hash;
   if (STAGE_MASK == STAGE_BASE_TCS_TES) //VS+TCS+TES+FS
      return base_hash ^ shaders[MESA_SHADER_TESS_CTRL]->hash ^ shaders[MESA_SHADER_TESS_EVAL]->hash;

   /* all stages */
   return base_hash ^ shaders[MESA_SHADER_GEOMETRY]->hash ^ shaders[MESA_SHADER_TESS_CTRL]->hash ^ shaders[MESA_SHADER_TESS_EVAL]->hash;
}

template <unsigned STAGE_MASK>
static bool
equals_gfx_program(const void *a, const void *b)
{
   const void **sa = (const void**)a;
   const void **sb = (const void**)b;
   STATIC_ASSERT(MESA_SHADER_VERTEX == 0);
   STATIC_ASSERT(MESA_SHADER_TESS_CTRL == 1);
   STATIC_ASSERT(MESA_SHADER_TESS_EVAL == 2);
   STATIC_ASSERT(MESA_SHADER_GEOMETRY == 3);
   STATIC_ASSERT(MESA_SHADER_FRAGMENT == 4);
   if (STAGE_MASK == STAGE_BASE) //VS+FS
      return sa[MESA_SHADER_VERTEX] == sb[MESA_SHADER_VERTEX] &&
             sa[MESA_SHADER_FRAGMENT] == sb[MESA_SHADER_FRAGMENT];
   if (STAGE_MASK == STAGE_BASE_GS) //VS+GS+FS
      return sa[MESA_SHADER_VERTEX] == sb[MESA_SHADER_VERTEX] &&
             !memcmp(&sa[MESA_SHADER_GEOMETRY], &sb[MESA_SHADER_GEOMETRY], sizeof(void*) * 2);
   /*VS+TCS+FS isn't a thing */
   /*VS+TCS+GS+FS isn't a thing */
   if (STAGE_MASK == STAGE_BASE_TES) //VS+TES+FS
      return sa[MESA_SHADER_VERTEX] == sb[MESA_SHADER_VERTEX] &&
             sa[MESA_SHADER_TESS_EVAL] == sb[MESA_SHADER_TESS_EVAL] &&
             sa[MESA_SHADER_FRAGMENT] == sb[MESA_SHADER_FRAGMENT];
   if (STAGE_MASK == STAGE_BASE_TES_GS) //VS+TES+GS+FS
      return sa[MESA_SHADER_VERTEX] == sb[MESA_SHADER_VERTEX] &&
             !memcmp(&sa[MESA_SHADER_TESS_EVAL], &sb[MESA_SHADER_TESS_EVAL], sizeof(void*) * 3);
   if (STAGE_MASK == STAGE_BASE_TCS_TES) //VS+TCS+TES+FS
      return !memcmp(sa, sb, sizeof(void*) * 3) &&
             sa[MESA_SHADER_FRAGMENT] == sb[MESA_SHADER_FRAGMENT];

   /* all stages */
   return !memcmp(a, b, sizeof(void*) * ZINK_GFX_SHADER_COUNT);
}

extern "C"
void
zink_init_draw_functions(struct zink_context *ctx, struct zink_screen *screen)
{
   pipe_draw_vbo_func draw_vbo_array[2][4] //multidraw, zink_dynamic_state
                                    [2];   //batch changed
   pipe_draw_vertex_state_func draw_state_array[2][4] //multidraw, zink_dynamic_state
                                               [2];   //batch changed
   zink_dynamic_state dynamic;
   if (screen->info.have_EXT_extended_dynamic_state) {
      if (screen->info.have_EXT_extended_dynamic_state2) {
         if (screen->info.have_EXT_vertex_input_dynamic_state)
            dynamic = ZINK_DYNAMIC_VERTEX_INPUT;
         else
            dynamic = ZINK_DYNAMIC_STATE2;
      } else {
         dynamic = ZINK_DYNAMIC_STATE;
      }
   } else {
      dynamic = ZINK_NO_DYNAMIC_STATE;
   }
   init_all_draw_functions(ctx, draw_vbo_array, draw_state_array);
   memcpy(ctx->draw_vbo, &draw_vbo_array[screen->info.have_EXT_multi_draw]
                                        [dynamic],
                                        sizeof(ctx->draw_vbo));
   memcpy(ctx->draw_state, &draw_state_array[screen->info.have_EXT_multi_draw]
                                          [dynamic],
                                          sizeof(ctx->draw_state));

   /* Bind a fake draw_vbo, so that draw_vbo isn't NULL, which would skip
    * initialization of callbacks in upper layers (such as u_threaded_context).
    */
   ctx->base.draw_vbo = zink_invalid_draw_vbo;
   ctx->base.draw_vertex_state = zink_invalid_draw_vertex_state;

   _mesa_hash_table_init(&ctx->program_cache[0], ctx, hash_gfx_program<0>, equals_gfx_program<0>);
   _mesa_hash_table_init(&ctx->program_cache[1], ctx, hash_gfx_program<1>, equals_gfx_program<1>);
   _mesa_hash_table_init(&ctx->program_cache[2], ctx, hash_gfx_program<2>, equals_gfx_program<2>);
   _mesa_hash_table_init(&ctx->program_cache[3], ctx, hash_gfx_program<3>, equals_gfx_program<3>);
   _mesa_hash_table_init(&ctx->program_cache[4], ctx, hash_gfx_program<4>, equals_gfx_program<4>);
   _mesa_hash_table_init(&ctx->program_cache[5], ctx, hash_gfx_program<5>, equals_gfx_program<5>);
   _mesa_hash_table_init(&ctx->program_cache[6], ctx, hash_gfx_program<6>, equals_gfx_program<6>);
   _mesa_hash_table_init(&ctx->program_cache[7], ctx, hash_gfx_program<7>, equals_gfx_program<7>);
}

void
zink_init_grid_functions(struct zink_context *ctx)
{
   init_all_grid_functions(ctx);
   /* Bind a fake launch_grid, so that draw_vbo isn't NULL, which would skip
    * initialization of callbacks in upper layers (such as u_threaded_context).
    */
   ctx->base.launch_grid = zink_invalid_launch_grid;
}
