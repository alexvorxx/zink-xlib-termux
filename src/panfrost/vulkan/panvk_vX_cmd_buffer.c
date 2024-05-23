/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "genxml/gen_macros.h"

#include "panvk_buffer.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_pool.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_event.h"
#include "panvk_image.h"
#include "panvk_image_view.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"
#include "panvk_priv_bo.h"

#include "pan_blitter.h"
#include "pan_desc.h"
#include "pan_earlyzs.h"
#include "pan_encoder.h"
#include "pan_props.h"
#include "pan_samples.h"
#include "pan_shader.h"

#include "util/rounding.h"
#include "util/u_pack_color.h"

#include "vk_descriptor_update_template.h"
#include "vk_format.h"

struct panvk_draw_info {
   unsigned first_index;
   unsigned index_count;
   unsigned index_size;
   unsigned first_vertex;
   unsigned vertex_count;
   unsigned vertex_range;
   unsigned padded_vertex_count;
   unsigned first_instance;
   unsigned instance_count;
   int vertex_offset;
   unsigned offset_start;
   struct mali_invocation_packed invocation;
   struct {
      mali_ptr varyings;
      mali_ptr attributes;
      mali_ptr attribute_bufs;
   } vs;
   struct {
      mali_ptr rsd;
      mali_ptr varyings;
      mali_ptr attributes;
      mali_ptr attribute_bufs;
   } fs;
   mali_ptr push_uniforms;
   mali_ptr varying_bufs;
   mali_ptr textures;
   mali_ptr samplers;
   mali_ptr ubos;
   mali_ptr position;
   mali_ptr indices;
   union {
      mali_ptr psiz;
      float line_width;
   };
   mali_ptr tls;
   mali_ptr fb;
   const struct pan_tiler_context *tiler_ctx;
   mali_ptr viewport;
   struct {
      struct panfrost_ptr vertex;
      struct panfrost_ptr tiler;
   } jobs;
};

struct panvk_dispatch_info {
   struct pan_compute_dim wg_count;
   mali_ptr attributes;
   mali_ptr attribute_bufs;
   mali_ptr tsd;
   mali_ptr ubos;
   mali_ptr push_uniforms;
   mali_ptr textures;
   mali_ptr samplers;
};

static uint32_t
panvk_debug_adjust_bo_flags(const struct panvk_device *device,
                            uint32_t bo_flags)
{
   struct panvk_instance *instance =
      to_panvk_instance(device->vk.physical->instance);

   if (instance->debug_flags & PANVK_DEBUG_DUMP)
      bo_flags &= ~PAN_KMOD_BO_FLAG_NO_MMAP;

   return bo_flags;
}

static void
panvk_cmd_prepare_fragment_job(struct panvk_cmd_buffer *cmdbuf)
{
   const struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   struct panvk_batch *batch = cmdbuf->cur_batch;
   struct panfrost_ptr job_ptr =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, FRAGMENT_JOB);

   GENX(pan_emit_fragment_job)
   (fbinfo, batch->fb.desc.gpu, job_ptr.cpu), batch->fragment_job = job_ptr.gpu;
   util_dynarray_append(&batch->jobs, void *, job_ptr.cpu);
}

void
panvk_per_arch(cmd_close_batch)(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (!batch)
      return;

   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;

   assert(batch);

   bool clear = fbinfo->zs.clear.z | fbinfo->zs.clear.s;
   for (unsigned i = 0; i < fbinfo->rt_count; i++)
      clear |= fbinfo->rts[i].clear;

   if (!clear && !batch->jc.first_job) {
      if (util_dynarray_num_elements(&batch->event_ops,
                                     struct panvk_cmd_event_op) == 0) {
         /* Content-less batch, let's drop it */
         vk_free(&cmdbuf->vk.pool->alloc, batch);
      } else {
         /* Batch has no jobs but is needed for synchronization, let's add a
          * NULL job so the SUBMIT ioctl doesn't choke on it.
          */
         struct panfrost_ptr ptr =
            pan_pool_alloc_desc(&cmdbuf->desc_pool.base, JOB_HEADER);
         util_dynarray_append(&batch->jobs, void *, ptr.cpu);
         pan_jc_add_job(&cmdbuf->desc_pool.base, &batch->jc, MALI_JOB_TYPE_NULL,
                        false, false, 0, 0, &ptr, false);
         list_addtail(&batch->node, &cmdbuf->batches);
      }
      cmdbuf->cur_batch = NULL;
      return;
   }

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);

   list_addtail(&batch->node, &cmdbuf->batches);

   if (batch->jc.first_tiler) {
      struct panfrost_ptr preload_jobs[2];
      unsigned num_preload_jobs = GENX(pan_preload_fb)(
         &dev->meta.blitter.cache, &cmdbuf->desc_pool.base, &batch->jc,
         &cmdbuf->state.gfx.fb.info, batch->tls.gpu, batch->tiler.ctx_desc.gpu,
         preload_jobs);
      for (unsigned i = 0; i < num_preload_jobs; i++)
         util_dynarray_append(&batch->jobs, void *, preload_jobs[i].cpu);
   }

   if (batch->tlsinfo.tls.size) {
      unsigned thread_tls_alloc =
         panfrost_query_thread_tls_alloc(&phys_dev->kmod.props);
      unsigned core_id_range;

      panfrost_query_core_count(&phys_dev->kmod.props, &core_id_range);

      unsigned size = panfrost_get_total_stack_size(
         batch->tlsinfo.tls.size, thread_tls_alloc, core_id_range);
      batch->tlsinfo.tls.ptr =
         pan_pool_alloc_aligned(&cmdbuf->tls_pool.base, size, 4096).gpu;
   }

   if (batch->tlsinfo.wls.size) {
      assert(batch->wls_total_size);
      batch->tlsinfo.wls.ptr =
         pan_pool_alloc_aligned(&cmdbuf->tls_pool.base, batch->wls_total_size,
                                4096)
            .gpu;
   }

   if (batch->tls.cpu)
      GENX(pan_emit_tls)(&batch->tlsinfo, batch->tls.cpu);

   if (batch->fb.desc.cpu) {
      fbinfo->sample_positions = dev->sample_positions->addr.dev +
                                 panfrost_sample_positions_offset(
                                    pan_sample_pattern(fbinfo->nr_samples));

      batch->fb.desc.gpu |=
         GENX(pan_emit_fbd)(&cmdbuf->state.gfx.fb.info, &batch->tlsinfo,
                            &batch->tiler.ctx, batch->fb.desc.cpu);

      panvk_cmd_prepare_fragment_job(cmdbuf);
   }

   cmdbuf->cur_batch = NULL;
}

void
panvk_per_arch(cmd_alloc_fb_desc)(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (batch->fb.desc.gpu)
      return;

   const struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   bool has_zs_ext = fbinfo->zs.view.zs || fbinfo->zs.view.s;

   batch->fb.bo_count = cmdbuf->state.gfx.fb.bo_count;
   memcpy(batch->fb.bos, cmdbuf->state.gfx.fb.bos,
          batch->fb.bo_count * sizeof(batch->fb.bos[0]));
   batch->fb.desc = pan_pool_alloc_desc_aggregate(
      &cmdbuf->desc_pool.base, PAN_DESC(FRAMEBUFFER),
      PAN_DESC_ARRAY(has_zs_ext ? 1 : 0, ZS_CRC_EXTENSION),
      PAN_DESC_ARRAY(MAX2(fbinfo->rt_count, 1), RENDER_TARGET));

   memset(&cmdbuf->state.gfx.fb.info.bifrost.pre_post.dcds, 0,
          sizeof(cmdbuf->state.gfx.fb.info.bifrost.pre_post.dcds));
}

void
panvk_per_arch(cmd_alloc_tls_desc)(struct panvk_cmd_buffer *cmdbuf, bool gfx)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   assert(batch);
   if (!batch->tls.gpu) {
      batch->tls = pan_pool_alloc_desc(&cmdbuf->desc_pool.base, LOCAL_STORAGE);
   }
}

#define is_dirty(__cmdbuf, __name)                                             \
   BITSET_TEST((__cmdbuf)->vk.dynamic_graphics_state.dirty,                    \
               MESA_VK_DYNAMIC_##__name)

#define set_dirty(__cmdbuf, __nm)                                              \
   BITSET_SET((__cmdbuf)->vk.dynamic_graphics_state.dirty,                     \
              MESA_VK_DYNAMIC_##__name)

static void
panvk_cmd_prepare_draw_sysvals(struct panvk_cmd_buffer *cmdbuf,
                               struct panvk_draw_info *draw)
{
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   struct panvk_graphics_sysvals *sysvals = &cmdbuf->state.gfx.sysvals;
   struct vk_color_blend_state *cb = &cmdbuf->vk.dynamic_graphics_state.cb;

   unsigned base_vertex = draw->index_size ? draw->vertex_offset : 0;
   if (sysvals->vs.first_vertex != draw->offset_start ||
       sysvals->vs.base_vertex != base_vertex ||
       sysvals->vs.base_instance != draw->first_instance) {
      sysvals->vs.first_vertex = draw->offset_start;
      sysvals->vs.base_vertex = base_vertex;
      sysvals->vs.base_instance = draw->first_instance;
      desc_state->push_uniforms = 0;
   }

   if (is_dirty(cmdbuf, CB_BLEND_CONSTANTS)) {
      for (unsigned i = 0; i < ARRAY_SIZE(cb->blend_constants); i++)
         sysvals->blend.constants[i] =
            CLAMP(cb->blend_constants[i], 0.0f, 1.0f);
      desc_state->push_uniforms = 0;
   }

   if (is_dirty(cmdbuf, VP_VIEWPORTS)) {
      VkViewport *viewport = &cmdbuf->vk.dynamic_graphics_state.vp.viewports[0];

      /* Upload the viewport scale. Defined as (px/2, py/2, pz) at the start of
       * section 24.5 ("Controlling the Viewport") of the Vulkan spec. At the
       * end of the section, the spec defines:
       *
       * px = width
       * py = height
       * pz = maxDepth - minDepth
       */
      sysvals->viewport.scale.x = 0.5f * viewport->width;
      sysvals->viewport.scale.y = 0.5f * viewport->height;
      sysvals->viewport.scale.z = (viewport->maxDepth - viewport->minDepth);

      /* Upload the viewport offset. Defined as (ox, oy, oz) at the start of
       * section 24.5 ("Controlling the Viewport") of the Vulkan spec. At the
       * end of the section, the spec defines:
       *
       * ox = x + width/2
       * oy = y + height/2
       * oz = minDepth
       */
      sysvals->viewport.offset.x = (0.5f * viewport->width) + viewport->x;
      sysvals->viewport.offset.y = (0.5f * viewport->height) + viewport->y;
      sysvals->viewport.offset.z = viewport->minDepth;
      desc_state->push_uniforms = 0;
   }
}

static void
panvk_cmd_prepare_push_uniforms(struct panvk_cmd_buffer *cmdbuf,
                                struct panvk_descriptor_state *desc_state,
                                void *sysvals, unsigned sysvals_sz)
{
   if (desc_state->push_uniforms)
      return;

   struct panfrost_ptr push_uniforms =
      pan_pool_alloc_aligned(&cmdbuf->desc_pool.base, 512, 16);

   /* The first half is used for push constants. */
   memcpy(push_uniforms.cpu, cmdbuf->push_constants,
          sizeof(cmdbuf->push_constants));

   /* The second half is used for sysvals. */
   memcpy((uint8_t *)push_uniforms.cpu + 256, sysvals, sysvals_sz);

   desc_state->push_uniforms = push_uniforms.gpu;
}

static void
panvk_cmd_prepare_push_sets(struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_descriptor_state *desc_state,
                            const struct panvk_pipeline *pipeline)
{
   const struct panvk_pipeline_layout *playout = pipeline->layout;

   for (unsigned i = 0; i < playout->vk.set_count; i++) {
      const struct panvk_descriptor_set_layout *slayout =
         vk_to_panvk_descriptor_set_layout(playout->vk.set_layouts[i]);
      bool is_push_set =
         slayout->flags &
         VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;

      if (desc_state->sets[i] || !is_push_set || !desc_state->push_sets[i])
         continue;

      struct panvk_descriptor_set *set = &desc_state->push_sets[i]->set;

      panvk_per_arch(push_descriptor_set_assign_layout)(
         desc_state->push_sets[i], slayout);
      if (slayout->desc_ubo_size) {
         struct panfrost_ptr desc_ubo = pan_pool_alloc_aligned(
            &cmdbuf->desc_pool.base, slayout->desc_ubo_size, 16);
         struct mali_uniform_buffer_packed *ubos = set->ubos;

         memcpy(desc_ubo.cpu, set->desc_ubo.addr.host, slayout->desc_ubo_size);
         set->desc_ubo.addr.dev = desc_ubo.gpu;
         set->desc_ubo.addr.host = desc_ubo.cpu;

         pan_pack(&ubos[slayout->desc_ubo_index], UNIFORM_BUFFER, cfg) {
            cfg.pointer = set->desc_ubo.addr.dev;
            cfg.entries = DIV_ROUND_UP(slayout->desc_ubo_size, 16);
         }
      }

      desc_state->sets[i] = &desc_state->push_sets[i]->set;
   }
}

static void
panvk_cmd_unprepare_push_sets(struct panvk_cmd_buffer *cmdbuf,
                              struct panvk_descriptor_state *desc_state)
{
   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->sets); i++) {
      if (desc_state->push_sets[i] &&
          &desc_state->push_sets[i]->set == desc_state->sets[i])
         desc_state->sets[i] = NULL;
   }
}

static void
panvk_cmd_prepare_dyn_ssbos(struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_descriptor_state *desc_state,
                            const struct panvk_pipeline *pipeline)
{
   if (!pipeline->layout->num_dyn_ssbos || desc_state->dyn_desc_ubo)
      return;

   struct panfrost_ptr ssbo_descs = pan_pool_alloc_aligned(
      &cmdbuf->desc_pool.base, sizeof(desc_state->dyn.ssbos), 16);

   memcpy(ssbo_descs.cpu, desc_state->dyn.ssbos, sizeof(desc_state->dyn.ssbos));

   desc_state->dyn_desc_ubo = ssbo_descs.gpu;
}

static void
panvk_cmd_prepare_ubos(struct panvk_cmd_buffer *cmdbuf,
                       struct panvk_descriptor_state *desc_state,
                       const struct panvk_pipeline *pipeline)
{
   unsigned ubo_count =
      panvk_per_arch(pipeline_layout_total_ubo_count)(pipeline->layout);

   if (!ubo_count || desc_state->ubos)
      return;

   panvk_cmd_prepare_dyn_ssbos(cmdbuf, desc_state, pipeline);

   struct panfrost_ptr ubos = pan_pool_alloc_desc_array(
      &cmdbuf->desc_pool.base, ubo_count, UNIFORM_BUFFER);
   struct mali_uniform_buffer_packed *ubo_descs = ubos.cpu;

   for (unsigned s = 0; s < pipeline->layout->vk.set_count; s++) {
      const struct panvk_descriptor_set_layout *set_layout =
         vk_to_panvk_descriptor_set_layout(pipeline->layout->vk.set_layouts[s]);
      const struct panvk_descriptor_set *set = desc_state->sets[s];

      unsigned ubo_start =
         panvk_per_arch(pipeline_layout_ubo_start)(pipeline->layout, s, false);

      if (!set) {
         memset(&ubo_descs[ubo_start], 0,
                set_layout->num_ubos * sizeof(*ubo_descs));
      } else {
         memcpy(&ubo_descs[ubo_start], set->ubos,
                set_layout->num_ubos * sizeof(*ubo_descs));
      }
   }

   unsigned dyn_ubos_offset =
      panvk_per_arch(pipeline_layout_dyn_ubos_offset)(pipeline->layout);

   memcpy(&ubo_descs[dyn_ubos_offset], desc_state->dyn.ubos,
          pipeline->layout->num_dyn_ubos * sizeof(*ubo_descs));

   if (pipeline->layout->num_dyn_ssbos) {
      unsigned dyn_desc_ubo =
         panvk_per_arch(pipeline_layout_dyn_desc_ubo_index)(pipeline->layout);

      pan_pack(&ubo_descs[dyn_desc_ubo], UNIFORM_BUFFER, cfg) {
         cfg.pointer = desc_state->dyn_desc_ubo;
         cfg.entries =
            pipeline->layout->num_dyn_ssbos * sizeof(struct panvk_ssbo_addr);
      }
   }

   desc_state->ubos = ubos.gpu;
}

static void
panvk_cmd_prepare_textures(struct panvk_cmd_buffer *cmdbuf,
                           struct panvk_descriptor_state *desc_state,
                           const struct panvk_pipeline *pipeline)
{
   unsigned num_textures = pipeline->layout->num_textures;

   if (!num_textures || desc_state->textures)
      return;

   struct panfrost_ptr textures = pan_pool_alloc_aligned(
      &cmdbuf->desc_pool.base, num_textures * pan_size(TEXTURE),
      pan_size(TEXTURE));

   void *texture = textures.cpu;

   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->sets); i++) {
      if (!desc_state->sets[i])
         continue;

      memcpy(texture, desc_state->sets[i]->textures,
             desc_state->sets[i]->layout->num_textures * pan_size(TEXTURE));

      texture += desc_state->sets[i]->layout->num_textures * pan_size(TEXTURE);
   }

   desc_state->textures = textures.gpu;
}

static void
panvk_cmd_prepare_samplers(struct panvk_cmd_buffer *cmdbuf,
                           struct panvk_descriptor_state *desc_state,
                           const struct panvk_pipeline *pipeline)
{
   unsigned num_samplers = pipeline->layout->num_samplers;

   if (!num_samplers || desc_state->samplers)
      return;

   struct panfrost_ptr samplers =
      pan_pool_alloc_desc_array(&cmdbuf->desc_pool.base, num_samplers, SAMPLER);

   void *sampler = samplers.cpu;

   /* Prepare the dummy sampler */
   pan_pack(sampler, SAMPLER, cfg) {
      cfg.seamless_cube_map = false;
      cfg.magnify_nearest = true;
      cfg.minify_nearest = true;
      cfg.normalized_coordinates = false;
   }

   sampler += pan_size(SAMPLER);

   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->sets); i++) {
      if (!desc_state->sets[i])
         continue;

      memcpy(sampler, desc_state->sets[i]->samplers,
             desc_state->sets[i]->layout->num_samplers * pan_size(SAMPLER));

      sampler += desc_state->sets[i]->layout->num_samplers * pan_size(SAMPLER);
   }

   desc_state->samplers = samplers.gpu;
}

static bool
has_depth_att(struct panvk_cmd_buffer *cmdbuf)
{
   return (cmdbuf->state.gfx.fb.bound_attachments &
           MESA_VK_RP_ATTACHMENT_DEPTH_BIT) != 0;
}

static bool
has_stencil_att(struct panvk_cmd_buffer *cmdbuf)
{
   return (cmdbuf->state.gfx.fb.bound_attachments &
           MESA_VK_RP_ATTACHMENT_STENCIL_BIT) != 0;
}

static bool
writes_depth(struct panvk_cmd_buffer *cmdbuf)
{
   const struct vk_depth_stencil_state *ds =
      &cmdbuf->vk.dynamic_graphics_state.ds;

   return has_depth_att(cmdbuf) && ds->depth.test_enable &&
          ds->depth.write_enable && ds->depth.compare_op != VK_COMPARE_OP_NEVER;
}

static bool
writes_stencil(struct panvk_cmd_buffer *cmdbuf)
{
   const struct vk_depth_stencil_state *ds =
      &cmdbuf->vk.dynamic_graphics_state.ds;

   return has_stencil_att(cmdbuf) && ds->stencil.test_enable &&
          ((ds->stencil.front.write_mask &&
            (ds->stencil.front.op.fail != VK_STENCIL_OP_KEEP ||
             ds->stencil.front.op.pass != VK_STENCIL_OP_KEEP ||
             ds->stencil.front.op.depth_fail != VK_STENCIL_OP_KEEP)) ||
           (ds->stencil.back.write_mask &&
            (ds->stencil.back.op.fail != VK_STENCIL_OP_KEEP ||
             ds->stencil.back.op.pass != VK_STENCIL_OP_KEEP ||
             ds->stencil.back.op.depth_fail != VK_STENCIL_OP_KEEP)));
}

static bool
ds_test_always_passes(struct panvk_cmd_buffer *cmdbuf)
{
   const struct vk_depth_stencil_state *ds =
      &cmdbuf->vk.dynamic_graphics_state.ds;

   if (!has_depth_att(cmdbuf))
      return true;

   if (ds->depth.test_enable && ds->depth.compare_op != VK_COMPARE_OP_ALWAYS)
      return false;

   if (ds->stencil.test_enable &&
       (ds->stencil.front.op.compare != VK_COMPARE_OP_ALWAYS ||
        ds->stencil.back.op.compare != VK_COMPARE_OP_ALWAYS))
      return false;

   return true;
}

static inline enum mali_func
translate_compare_func(VkCompareOp comp)
{
   STATIC_ASSERT(VK_COMPARE_OP_NEVER == (VkCompareOp)MALI_FUNC_NEVER);
   STATIC_ASSERT(VK_COMPARE_OP_LESS == (VkCompareOp)MALI_FUNC_LESS);
   STATIC_ASSERT(VK_COMPARE_OP_EQUAL == (VkCompareOp)MALI_FUNC_EQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_LESS_OR_EQUAL == (VkCompareOp)MALI_FUNC_LEQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_GREATER == (VkCompareOp)MALI_FUNC_GREATER);
   STATIC_ASSERT(VK_COMPARE_OP_NOT_EQUAL == (VkCompareOp)MALI_FUNC_NOT_EQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_GREATER_OR_EQUAL ==
                 (VkCompareOp)MALI_FUNC_GEQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_ALWAYS == (VkCompareOp)MALI_FUNC_ALWAYS);

   return (enum mali_func)comp;
}

static enum mali_stencil_op
translate_stencil_op(VkStencilOp in)
{
   switch (in) {
   case VK_STENCIL_OP_KEEP:
      return MALI_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO:
      return MALI_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return MALI_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return MALI_STENCIL_OP_INCR_SAT;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return MALI_STENCIL_OP_DECR_SAT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return MALI_STENCIL_OP_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return MALI_STENCIL_OP_DECR_WRAP;
   case VK_STENCIL_OP_INVERT:
      return MALI_STENCIL_OP_INVERT;
   default:
      unreachable("Invalid stencil op");
   }
}

static bool
fs_required(const struct vk_color_blend_state *cb,
            const struct pan_shader_info *fs_info)
{
   /* If we generally have side effects */
   if (fs_info->fs.sidefx)
      return true;

   /* If colour is written we need to execute */
   for (unsigned i = 0; i < cb->attachment_count; ++i) {
      if ((cb->color_write_enables & BITFIELD_BIT(i)) &&
          cb->attachments[i].write_mask)
         return true;
   }

   /* If depth is written and not implied we need to execute.
    * TODO: Predicate on Z/S writes being enabled */
   return (fs_info->fs.writes_depth || fs_info->fs.writes_stencil);
}

static void
panvk_draw_prepare_fs_rsd(struct panvk_cmd_buffer *cmdbuf,
                          struct panvk_draw_info *draw)
{
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;

   bool dirty =
      is_dirty(cmdbuf, RS_RASTERIZER_DISCARD_ENABLE) ||
      is_dirty(cmdbuf, RS_DEPTH_CLAMP_ENABLE) ||
      is_dirty(cmdbuf, RS_DEPTH_BIAS_ENABLE) ||
      is_dirty(cmdbuf, RS_DEPTH_BIAS_FACTORS) ||
      is_dirty(cmdbuf, CB_LOGIC_OP_ENABLE) || is_dirty(cmdbuf, CB_LOGIC_OP) ||
      is_dirty(cmdbuf, CB_ATTACHMENT_COUNT) ||
      is_dirty(cmdbuf, CB_COLOR_WRITE_ENABLES) ||
      is_dirty(cmdbuf, CB_BLEND_ENABLES) ||
      is_dirty(cmdbuf, CB_BLEND_EQUATIONS) ||
      is_dirty(cmdbuf, CB_WRITE_MASKS) ||
      is_dirty(cmdbuf, CB_BLEND_CONSTANTS) ||
      is_dirty(cmdbuf, DS_DEPTH_TEST_ENABLE) ||
      is_dirty(cmdbuf, DS_DEPTH_WRITE_ENABLE) ||
      is_dirty(cmdbuf, DS_DEPTH_COMPARE_OP) ||
      is_dirty(cmdbuf, DS_DEPTH_COMPARE_OP) ||
      is_dirty(cmdbuf, DS_STENCIL_TEST_ENABLE) ||
      is_dirty(cmdbuf, DS_STENCIL_OP) ||
      is_dirty(cmdbuf, DS_STENCIL_COMPARE_MASK) ||
      is_dirty(cmdbuf, DS_STENCIL_WRITE_MASK) ||
      is_dirty(cmdbuf, DS_STENCIL_REFERENCE) ||
      is_dirty(cmdbuf, MS_RASTERIZATION_SAMPLES) ||
      is_dirty(cmdbuf, MS_SAMPLE_MASK) ||
      is_dirty(cmdbuf, MS_ALPHA_TO_COVERAGE_ENABLE) ||
      is_dirty(cmdbuf, MS_ALPHA_TO_ONE_ENABLE) || !cmdbuf->state.gfx.fs.rsd;

   if (!dirty) {
      draw->fs.rsd = cmdbuf->state.gfx.fs.rsd;
      return;
   }

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   const struct vk_dynamic_graphics_state *dyns =
      &cmdbuf->vk.dynamic_graphics_state;
   const struct vk_rasterization_state *rs = &dyns->rs;
   const struct vk_color_blend_state *cb = &dyns->cb;
   const struct vk_depth_stencil_state *ds = &dyns->ds;
   const struct pan_shader_info *fs_info = &pipeline->fs.info;
   unsigned bd_count = MAX2(cb->attachment_count, 1);
   bool test_s = has_stencil_att(cmdbuf) && ds->stencil.test_enable;
   bool test_z = has_depth_att(cmdbuf) && ds->depth.test_enable;
   bool writes_z = writes_depth(cmdbuf);
   bool writes_s = writes_stencil(cmdbuf);
   bool needs_fs = fs_required(cb, fs_info);
   bool blend_shader_loads_blend_const = false;
   bool blend_reads_dest = false;

   struct panfrost_ptr ptr = pan_pool_alloc_desc_aggregate(
      &cmdbuf->desc_pool.base, PAN_DESC(RENDERER_STATE),
      PAN_DESC_ARRAY(bd_count, BLEND));
   struct mali_renderer_state_packed *rsd = ptr.cpu;
   struct mali_blend_packed *bds = ptr.cpu + pan_size(RENDERER_STATE);

   panvk_per_arch(blend_emit_descs)(
      dev, cb, cmdbuf->state.gfx.fb.color_attachments.fmts,
      cmdbuf->state.gfx.fb.color_attachments.samples, fs_info,
      pipeline->fs.code, bds, &blend_reads_dest,
      &blend_shader_loads_blend_const);

   pan_pack(rsd, RENDERER_STATE, cfg) {
      bool alpha_to_coverage = dyns->ms.alpha_to_coverage_enable;

      if (needs_fs) {
         pan_shader_prepare_rsd(fs_info, pipeline->fs.code, &cfg);

         if (blend_shader_loads_blend_const) {
            /* Preload the blend constant if the blend shader depends on it. */
            cfg.preload.uniform_count = MAX2(
               cfg.preload.uniform_count,
               DIV_ROUND_UP(256 + sizeof(struct panvk_graphics_sysvals), 8));
         }

         uint8_t rt_written = fs_info->outputs_written >> FRAG_RESULT_DATA0;
         uint8_t rt_mask = cmdbuf->state.gfx.fb.bound_attachments &
                           MESA_VK_RP_ATTACHMENT_ANY_COLOR_BITS;
         cfg.properties.allow_forward_pixel_to_kill =
            fs_info->fs.can_fpk && !(rt_mask & ~rt_written) &&
            !alpha_to_coverage && !blend_reads_dest;

         bool writes_zs = writes_z || writes_s;
         bool zs_always_passes = ds_test_always_passes(cmdbuf);
         bool oq = false; /* TODO: Occlusion queries */

         struct pan_earlyzs_state earlyzs =
            pan_earlyzs_get(pan_earlyzs_analyze(fs_info), writes_zs || oq,
                            alpha_to_coverage, zs_always_passes);

         cfg.properties.pixel_kill_operation = earlyzs.kill;
         cfg.properties.zs_update_operation = earlyzs.update;
      } else {
         cfg.properties.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
         cfg.properties.allow_forward_pixel_to_kill = true;
         cfg.properties.allow_forward_pixel_to_be_killed = true;
         cfg.properties.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
      }

      bool msaa = dyns->ms.rasterization_samples > 1;
      cfg.multisample_misc.multisample_enable = msaa;
      cfg.multisample_misc.sample_mask =
         msaa ? dyns->ms.sample_mask : UINT16_MAX;

      cfg.multisample_misc.depth_function =
         test_z ? translate_compare_func(ds->depth.compare_op)
                : MALI_FUNC_ALWAYS;

      cfg.multisample_misc.depth_write_mask = writes_z;
      cfg.multisample_misc.fixed_function_near_discard =
         !rs->depth_clamp_enable;
      cfg.multisample_misc.fixed_function_far_discard = !rs->depth_clamp_enable;
      cfg.multisample_misc.shader_depth_range_fixed = true;

      cfg.stencil_mask_misc.stencil_enable = test_s;
      cfg.stencil_mask_misc.alpha_to_coverage = alpha_to_coverage;
      cfg.stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.front_facing_depth_bias = rs->depth_bias.enable;
      cfg.stencil_mask_misc.back_facing_depth_bias = rs->depth_bias.enable;
      cfg.stencil_mask_misc.single_sampled_lines =
         dyns->ms.rasterization_samples <= 1;

      cfg.depth_units = rs->depth_bias.constant * 2.0f;
      cfg.depth_factor = rs->depth_bias.slope;
      cfg.depth_bias_clamp = rs->depth_bias.clamp;

      cfg.stencil_front.mask = ds->stencil.front.compare_mask;
      cfg.stencil_back.mask = ds->stencil.back.compare_mask;

      cfg.stencil_mask_misc.stencil_mask_front = ds->stencil.front.write_mask;
      cfg.stencil_mask_misc.stencil_mask_back = ds->stencil.back.write_mask;

      cfg.stencil_front.reference_value = ds->stencil.front.reference;
      cfg.stencil_back.reference_value = ds->stencil.back.reference;

      if (test_s) {
         cfg.stencil_front.compare_function =
            translate_compare_func(ds->stencil.front.op.compare);
         cfg.stencil_front.stencil_fail =
            translate_stencil_op(ds->stencil.front.op.fail);
         cfg.stencil_front.depth_fail =
            translate_stencil_op(ds->stencil.front.op.depth_fail);
         cfg.stencil_front.depth_pass =
            translate_stencil_op(ds->stencil.front.op.pass);
         cfg.stencil_back.compare_function =
            translate_compare_func(ds->stencil.back.op.compare);
         cfg.stencil_back.stencil_fail =
            translate_stencil_op(ds->stencil.back.op.fail);
         cfg.stencil_back.depth_fail =
            translate_stencil_op(ds->stencil.back.op.depth_fail);
         cfg.stencil_back.depth_pass =
            translate_stencil_op(ds->stencil.back.op.pass);
      }
   }

   cmdbuf->state.gfx.fs.rsd = ptr.gpu;
   draw->fs.rsd = cmdbuf->state.gfx.fs.rsd;
}

void
panvk_per_arch(cmd_get_tiler_context)(struct panvk_cmd_buffer *cmdbuf,
                                      unsigned width, unsigned height)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   struct panvk_batch *batch = cmdbuf->cur_batch;

   if (batch->tiler.ctx_desc.cpu)
      return;

   batch->tiler.heap_desc =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, TILER_HEAP);
   batch->tiler.ctx_desc =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, TILER_CONTEXT);

   pan_pack(&batch->tiler.heap_templ, TILER_HEAP, cfg) {
      cfg.size = pan_kmod_bo_size(dev->tiler_heap->bo);
      cfg.base = dev->tiler_heap->addr.dev;
      cfg.bottom = dev->tiler_heap->addr.dev;
      cfg.top = cfg.base + cfg.size;
   }

   pan_pack(&batch->tiler.ctx_templ, TILER_CONTEXT, cfg) {
      cfg.hierarchy_mask = 0x28;
      cfg.fb_width = width;
      cfg.fb_height = height;
      cfg.heap = batch->tiler.heap_desc.gpu;
      cfg.sample_pattern = pan_sample_pattern(fbinfo->nr_samples);
   }

   memcpy(batch->tiler.heap_desc.cpu, &batch->tiler.heap_templ,
          sizeof(batch->tiler.heap_templ));
   memcpy(batch->tiler.ctx_desc.cpu, &batch->tiler.ctx_templ,
          sizeof(batch->tiler.ctx_templ));
   batch->tiler.ctx.bifrost = batch->tiler.ctx_desc.gpu;
}

void
panvk_per_arch(cmd_prepare_tiler_context)(struct panvk_cmd_buffer *cmdbuf)
{
   const struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;

   panvk_per_arch(cmd_get_tiler_context)(cmdbuf, fbinfo->width, fbinfo->height);
}

static void
panvk_draw_prepare_tiler_context(struct panvk_cmd_buffer *cmdbuf,
                                 struct panvk_draw_info *draw)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;

   panvk_per_arch(cmd_prepare_tiler_context)(cmdbuf);
   draw->tiler_ctx = &batch->tiler.ctx;
}

static mali_pixel_format
panvk_varying_hw_format(gl_shader_stage stage, gl_varying_slot loc,
                        enum pipe_format pfmt)
{
   switch (loc) {
   case VARYING_SLOT_PNTC:
   case VARYING_SLOT_PSIZ:
#if PAN_ARCH <= 6
      return (MALI_R16F << 12) | panfrost_get_default_swizzle(1);
#else
      return (MALI_R16F << 12) | MALI_RGB_COMPONENT_ORDER_R000;
#endif
   case VARYING_SLOT_POS:
#if PAN_ARCH <= 6
      return (MALI_SNAP_4 << 12) | panfrost_get_default_swizzle(4);
#else
      return (MALI_SNAP_4 << 12) | MALI_RGB_COMPONENT_ORDER_RGBA;
#endif
   default:
      if (pfmt != PIPE_FORMAT_NONE)
         return GENX(panfrost_format_from_pipe_format)(pfmt)->hw;

#if PAN_ARCH >= 7
      return (MALI_CONSTANT << 12) | MALI_RGB_COMPONENT_ORDER_0000;
#else
      return (MALI_CONSTANT << 12) | PAN_V6_SWIZZLE(0, 0, 0, 0);
#endif
   }
}

static void
panvk_draw_prepare_varyings(struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_draw_info *draw)
{
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;
   struct panfrost_ptr bufs = pan_pool_alloc_desc_array(
      &cmdbuf->desc_pool.base, PANVK_VARY_BUF_MAX + 1, ATTRIBUTE_BUFFER);
   struct mali_attribute_buffer_packed *buf_descs = bufs.cpu;
   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;
   bool writes_point_size =
      pipeline->vs.info.vs.writes_point_size &&
      ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   unsigned vertex_count = draw->padded_vertex_count * draw->instance_count;
   mali_ptr psiz_buf = 0;

   for (unsigned i = 0; i < PANVK_VARY_BUF_MAX; i++) {
      unsigned buf_size = vertex_count * pipeline->vs.varyings.buf_strides[i];
      mali_ptr buf_addr =
         buf_size
            ? pan_pool_alloc_aligned(&cmdbuf->varying_pool.base, buf_size, 64)
                 .gpu
            : 0;

      pan_pack(&buf_descs[i], ATTRIBUTE_BUFFER, cfg) {
         cfg.stride = pipeline->vs.varyings.buf_strides[i];
         cfg.size = buf_size;
         cfg.pointer = buf_addr;
      }

      if (i == PANVK_VARY_BUF_POSITION)
         draw->position = buf_addr;

      if (i == PANVK_VARY_BUF_PSIZ)
         psiz_buf = buf_addr;
   }

   /* We need an empty entry to stop prefetching on Bifrost */
   memset(bufs.cpu + (pan_size(ATTRIBUTE_BUFFER) * PANVK_VARY_BUF_MAX), 0,
          pan_size(ATTRIBUTE_BUFFER));

   if (writes_point_size)
      draw->psiz = psiz_buf;
   else if (ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
            ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
      draw->line_width = cmdbuf->vk.dynamic_graphics_state.rs.line.width;
   else
      draw->line_width = 1.0f;

   draw->varying_bufs = bufs.gpu;
   draw->vs.varyings = pipeline->vs.varyings.attribs;
   draw->fs.varyings = pipeline->fs.varyings.attribs;
}

static void
panvk_fill_img_attribs(struct panvk_cmd_buffer *cmdbuf,
                       struct panvk_descriptor_state *desc_state,
                       const struct panvk_pipeline *pipeline, void *attrib_bufs,
                       void *attribs, unsigned first_buf)
{
   for (unsigned s = 0; s < pipeline->layout->vk.set_count; s++) {
      const struct panvk_descriptor_set *set = desc_state->sets[s];

      if (!set)
         continue;

      const struct panvk_descriptor_set_layout *layout = set->layout;
      unsigned img_idx = pipeline->layout->sets[s].img_offset;
      unsigned offset = img_idx * pan_size(ATTRIBUTE_BUFFER) * 2;
      unsigned size = layout->num_imgs * pan_size(ATTRIBUTE_BUFFER) * 2;

      memcpy(attrib_bufs + offset, desc_state->sets[s]->img_attrib_bufs, size);

      offset = img_idx * pan_size(ATTRIBUTE);
      for (unsigned i = 0; i < layout->num_imgs; i++) {
         pan_pack(attribs + offset, ATTRIBUTE, cfg) {
            cfg.buffer_index = first_buf + (img_idx + i) * 2;
            cfg.format = desc_state->sets[s]->img_fmts[i];
            cfg.offset_enable = false;
         }
         offset += pan_size(ATTRIBUTE);
      }
   }
}

static void
panvk_prepare_img_attribs(struct panvk_cmd_buffer *cmdbuf,
                          struct panvk_descriptor_state *desc_state,
                          const struct panvk_pipeline *pipeline)
{
   if (desc_state->img.attribs)
      return;

   unsigned attrib_count = pipeline->layout->num_imgs;
   unsigned attrib_buf_count = (pipeline->layout->num_imgs * 2);
   struct panfrost_ptr bufs = pan_pool_alloc_desc_array(
      &cmdbuf->desc_pool.base, attrib_buf_count + 1, ATTRIBUTE_BUFFER);
   struct panfrost_ptr attribs = pan_pool_alloc_desc_array(
      &cmdbuf->desc_pool.base, attrib_count, ATTRIBUTE);

   panvk_fill_img_attribs(cmdbuf, desc_state, pipeline, bufs.cpu, attribs.cpu,
                          0);

   desc_state->img.attrib_bufs = bufs.gpu;
   desc_state->img.attribs = attribs.gpu;
}

static void
panvk_draw_emit_attrib_buf(const struct panvk_draw_info *draw,
                           const struct vk_vertex_binding_state *buf_info,
                           const struct panvk_attrib_buf *buf, void *desc)
{
   mali_ptr addr = buf->address & ~63ULL;
   unsigned size = buf->size + (buf->address & 63);
   unsigned divisor = draw->padded_vertex_count * buf_info->divisor;
   bool per_instance = buf_info->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
   void *buf_ext = desc + pan_size(ATTRIBUTE_BUFFER);

   /* TODO: support instanced arrays */
   if (draw->instance_count <= 1) {
      pan_pack(desc, ATTRIBUTE_BUFFER, cfg) {
         cfg.type = MALI_ATTRIBUTE_TYPE_1D;
         cfg.stride = per_instance ? 0 : buf_info->stride;
         cfg.pointer = addr;
         cfg.size = size;
      }
   } else if (!per_instance) {
      pan_pack(desc, ATTRIBUTE_BUFFER, cfg) {
         cfg.type = MALI_ATTRIBUTE_TYPE_1D_MODULUS;
         cfg.divisor = draw->padded_vertex_count;
         cfg.stride = buf_info->stride;
         cfg.pointer = addr;
         cfg.size = size;
      }
   } else if (!divisor) {
      /* instance_divisor == 0 means all instances share the same value.
       * Make it a 1D array with a zero stride.
       */
      pan_pack(desc, ATTRIBUTE_BUFFER, cfg) {
         cfg.type = MALI_ATTRIBUTE_TYPE_1D;
         cfg.stride = 0;
         cfg.pointer = addr;
         cfg.size = size;
      }
   } else if (util_is_power_of_two_or_zero(divisor)) {
      pan_pack(desc, ATTRIBUTE_BUFFER, cfg) {
         cfg.type = MALI_ATTRIBUTE_TYPE_1D_POT_DIVISOR;
         cfg.stride = buf_info->stride;
         cfg.pointer = addr;
         cfg.size = size;
         cfg.divisor_r = __builtin_ctz(divisor);
      }
   } else {
      unsigned divisor_r = 0, divisor_e = 0;
      unsigned divisor_num =
         panfrost_compute_magic_divisor(divisor, &divisor_r, &divisor_e);
      pan_pack(desc, ATTRIBUTE_BUFFER, cfg) {
         cfg.type = MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR;
         cfg.stride = buf_info->stride;
         cfg.pointer = addr;
         cfg.size = size;
         cfg.divisor_r = divisor_r;
         cfg.divisor_e = divisor_e;
      }

      pan_pack(buf_ext, ATTRIBUTE_BUFFER_CONTINUATION_NPOT, cfg) {
         cfg.divisor_numerator = divisor_num;
         cfg.divisor = buf_info->divisor;
      }

      buf_ext = NULL;
   }

   /* If the buffer extension wasn't used, memset(0) */
   if (buf_ext)
      memset(buf_ext, 0, pan_size(ATTRIBUTE_BUFFER));
}

static void
panvk_draw_emit_attrib(const struct panvk_draw_info *draw,
                       const struct vk_vertex_attribute_state *attrib_info,
                       const struct vk_vertex_binding_state *buf_info,
                       const struct panvk_attrib_buf *buf, void *desc)
{
   bool per_instance = buf_info->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
   enum pipe_format f = vk_format_to_pipe_format(attrib_info->format);
   unsigned buf_idx = attrib_info->binding;

   pan_pack(desc, ATTRIBUTE, cfg) {
      cfg.buffer_index = buf_idx * 2;
      cfg.offset = attrib_info->offset + (buf->address & 63);
      cfg.offset_enable = true;

      if (per_instance)
         cfg.offset += draw->first_instance * buf_info->stride;

      cfg.format = GENX(panfrost_format_from_pipe_format)(f)->hw;
   }
}

static void
panvk_draw_prepare_vs_attribs(struct panvk_cmd_buffer *cmdbuf,
                              struct panvk_draw_info *draw)
{
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;
   const struct vk_vertex_input_state *vi =
      cmdbuf->vk.dynamic_graphics_state.vi;
   unsigned num_imgs =
      pipeline->vs.has_img_access ? pipeline->base.layout->num_imgs : 0;
   unsigned num_vs_attribs = util_last_bit(vi->attributes_valid);
   unsigned num_vbs = util_last_bit(vi->bindings_valid);
   unsigned attrib_count =
      num_imgs ? MAX_VS_ATTRIBS + num_imgs : num_vs_attribs;
   bool dirty =
      is_dirty(cmdbuf, VI) || is_dirty(cmdbuf, VI_BINDINGS_VALID) ||
      is_dirty(cmdbuf, VI_BINDING_STRIDES) ||
      (num_imgs && !desc_state->img.attribs) ||
      (cmdbuf->state.gfx.vb.count && !cmdbuf->state.gfx.vs.attrib_bufs) ||
      (attrib_count && !cmdbuf->state.gfx.vs.attribs);

   if (!dirty)
      return;

   unsigned attrib_buf_count = (num_vbs + num_imgs) * 2;
   struct panfrost_ptr bufs = pan_pool_alloc_desc_array(
      &cmdbuf->desc_pool.base, attrib_buf_count + 1, ATTRIBUTE_BUFFER);
   struct mali_attribute_buffer_packed *attrib_buf_descs = bufs.cpu;
   struct panfrost_ptr attribs = pan_pool_alloc_desc_array(
      &cmdbuf->desc_pool.base, attrib_count, ATTRIBUTE);
   struct mali_attribute_packed *attrib_descs = attribs.cpu;

   for (unsigned i = 0; i < num_vbs; i++) {
      if (vi->bindings_valid & BITFIELD_BIT(i)) {
         panvk_draw_emit_attrib_buf(draw, &vi->bindings[i],
                                    &cmdbuf->state.gfx.vb.bufs[i],
                                    &attrib_buf_descs[i * 2]);
      } else {
         memset(&attrib_buf_descs[i * 2], 0, sizeof(*attrib_buf_descs) * 2);
      }
   }

   for (unsigned i = 0; i < num_vs_attribs; i++) {
      if (vi->attributes_valid & BITFIELD_BIT(i)) {
         unsigned buf_idx = vi->attributes[i].binding;
         panvk_draw_emit_attrib(
            draw, &vi->attributes[i], &vi->bindings[buf_idx],
            &cmdbuf->state.gfx.vb.bufs[buf_idx], &attrib_descs[i]);
      } else {
         memset(&attrib_descs[i], 0, sizeof(attrib_descs[0]));
      }
   }

   if (num_imgs) {
      /* Image load/store are passed a fixed offset, so we can make vertex input
       * dynamic. Images are always placed after all potential vertex
       * attributes. Buffers are tightly packed since they don't interfere with
       * the vertex shader.
       */
      unsigned attribs_offset = MAX_VS_ATTRIBS * pan_size(ATTRIBUTE);
      unsigned bufs_offset = num_vbs * pan_size(ATTRIBUTE_BUFFER) * 2;

      memset(attribs.cpu + num_vs_attribs * pan_size(ATTRIBUTE), 0,
             (MAX_VS_ATTRIBS - num_vs_attribs) * pan_size(ATTRIBUTE));
      panvk_fill_img_attribs(cmdbuf, desc_state, &pipeline->base,
                             bufs.cpu + bufs_offset,
                             attribs.cpu + attribs_offset, num_vbs * 2);
      desc_state->img.attrib_bufs = bufs.gpu + bufs_offset;
      desc_state->img.attribs = attribs.gpu + attribs_offset;
   }

   /* A NULL entry is needed to stop prefecting on Bifrost */
   memset(bufs.cpu + (pan_size(ATTRIBUTE_BUFFER) * attrib_buf_count), 0,
          pan_size(ATTRIBUTE_BUFFER));

   cmdbuf->state.gfx.vs.attrib_bufs = bufs.gpu;
   cmdbuf->state.gfx.vs.attribs = attribs.gpu;
}

static void
panvk_draw_prepare_attributes(struct panvk_cmd_buffer *cmdbuf,
                              struct panvk_draw_info *draw)
{
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;

   panvk_draw_prepare_vs_attribs(cmdbuf, draw);
   draw->vs.attributes = cmdbuf->state.gfx.vs.attribs;
   draw->vs.attribute_bufs = cmdbuf->state.gfx.vs.attrib_bufs;

   if (pipeline->fs.has_img_access) {
      panvk_prepare_img_attribs(cmdbuf, desc_state, &pipeline->base);
      draw->fs.attributes = desc_state->img.attribs;
      draw->fs.attribute_bufs = desc_state->img.attrib_bufs;
   }
}

void
panvk_per_arch(emit_viewport)(const VkViewport *viewport,
                              const VkRect2D *scissor, void *vpd)
{
   /* The spec says "width must be greater than 0.0" */
   assert(viewport->x >= 0);
   int minx = (int)viewport->x;
   int maxx = (int)(viewport->x + viewport->width);

   /* Viewport height can be negative */
   int miny = MIN2((int)viewport->y, (int)(viewport->y + viewport->height));
   int maxy = MAX2((int)viewport->y, (int)(viewport->y + viewport->height));

   assert(scissor->offset.x >= 0 && scissor->offset.y >= 0);
   miny = MAX2(scissor->offset.x, minx);
   miny = MAX2(scissor->offset.y, miny);
   maxx = MIN2(scissor->offset.x + scissor->extent.width, maxx);
   maxy = MIN2(scissor->offset.y + scissor->extent.height, maxy);

   /* Make sure we don't end up with a max < min when width/height is 0 */
   maxx = maxx > minx ? maxx - 1 : maxx;
   maxy = maxy > miny ? maxy - 1 : maxy;

   assert(viewport->minDepth >= 0.0f && viewport->minDepth <= 1.0f);
   assert(viewport->maxDepth >= 0.0f && viewport->maxDepth <= 1.0f);

   pan_pack(vpd, VIEWPORT, cfg) {
      cfg.scissor_minimum_x = minx;
      cfg.scissor_minimum_y = miny;
      cfg.scissor_maximum_x = maxx;
      cfg.scissor_maximum_y = maxy;
      cfg.minimum_z = MIN2(viewport->minDepth, viewport->maxDepth);
      cfg.maximum_z = MAX2(viewport->minDepth, viewport->maxDepth);
   }
}

static void
panvk_draw_prepare_viewport(struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_draw_info *draw)
{
   if (is_dirty(cmdbuf, VP_VIEWPORTS) || is_dirty(cmdbuf, VP_SCISSORS)) {
      const VkViewport *viewport =
         &cmdbuf->vk.dynamic_graphics_state.vp.viewports[0];
      const VkRect2D *scissor =
         &cmdbuf->vk.dynamic_graphics_state.vp.scissors[0];
      struct panfrost_ptr vp =
         pan_pool_alloc_desc(&cmdbuf->desc_pool.base, VIEWPORT);

      panvk_per_arch(emit_viewport)(viewport, scissor, vp.cpu);
      cmdbuf->state.gfx.vpd = vp.gpu;
   }

   draw->viewport = cmdbuf->state.gfx.vpd;
}

static void
panvk_draw_prepare_vertex_job(struct panvk_cmd_buffer *cmdbuf,
                              struct panvk_draw_info *draw)
{
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;
   struct panvk_batch *batch = cmdbuf->cur_batch;
   struct panfrost_ptr ptr =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, COMPUTE_JOB);

   util_dynarray_append(&batch->jobs, void *, ptr.cpu);
   draw->jobs.vertex = ptr;

   memcpy(pan_section_ptr(ptr.cpu, COMPUTE_JOB, INVOCATION), &draw->invocation,
          pan_size(INVOCATION));

   pan_section_pack(ptr.cpu, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = 5;
   }

   pan_section_pack(ptr.cpu, COMPUTE_JOB, DRAW, cfg) {
      cfg.state = pipeline->vs.rsd;
      cfg.attributes = draw->vs.attributes;
      cfg.attribute_buffers = draw->vs.attribute_bufs;
      cfg.varyings = draw->vs.varyings;
      cfg.varying_buffers = draw->varying_bufs;
      cfg.thread_storage = draw->tls;
      cfg.offset_start = draw->offset_start;
      cfg.instance_size =
         draw->instance_count > 1 ? draw->padded_vertex_count : 1;
      cfg.uniform_buffers = draw->ubos;
      cfg.push_uniforms = draw->push_uniforms;
      cfg.textures = draw->textures;
      cfg.samplers = draw->samplers;
   }
}

static enum mali_draw_mode
translate_prim_topology(VkPrimitiveTopology in)
{
   switch (in) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return MALI_DRAW_MODE_POINTS;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return MALI_DRAW_MODE_LINES;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return MALI_DRAW_MODE_LINE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return MALI_DRAW_MODE_TRIANGLES;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return MALI_DRAW_MODE_TRIANGLE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return MALI_DRAW_MODE_TRIANGLE_FAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
   default:
      unreachable("Invalid primitive type");
   }
}

static void
panvk_emit_tiler_primitive(struct panvk_cmd_buffer *cmdbuf,
                           const struct panvk_draw_info *draw, void *prim)
{
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;
   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;
   bool writes_point_size =
      pipeline->vs.info.vs.writes_point_size &&
      ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

   pan_pack(prim, PRIMITIVE, cfg) {
      cfg.draw_mode = translate_prim_topology(ia->primitive_topology);
      if (writes_point_size)
         cfg.point_size_array_format = MALI_POINT_SIZE_ARRAY_FORMAT_FP16;

      cfg.first_provoking_vertex = true;
      if (ia->primitive_restart_enable)
         cfg.primitive_restart = MALI_PRIMITIVE_RESTART_IMPLICIT;
      cfg.job_task_split = 6;

      if (draw->index_size) {
         cfg.index_count = draw->index_count;
         cfg.indices = draw->indices;
         cfg.base_vertex_offset = draw->vertex_offset - draw->offset_start;

         switch (draw->index_size) {
         case 32:
            cfg.index_type = MALI_INDEX_TYPE_UINT32;
            break;
         case 16:
            cfg.index_type = MALI_INDEX_TYPE_UINT16;
            break;
         case 8:
            cfg.index_type = MALI_INDEX_TYPE_UINT8;
            break;
         default:
            unreachable("Invalid index size");
         }
      } else {
         cfg.index_count = draw->vertex_count;
         cfg.index_type = MALI_INDEX_TYPE_NONE;
      }
   }
}

static void
panvk_emit_tiler_primitive_size(struct panvk_cmd_buffer *cmdbuf,
                                const struct panvk_draw_info *draw,
                                void *primsz)
{
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;
   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;
   bool writes_point_size =
      pipeline->vs.info.vs.writes_point_size &&
      ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

   pan_pack(primsz, PRIMITIVE_SIZE, cfg) {
      if (writes_point_size) {
         cfg.size_array = draw->psiz;
      } else {
         cfg.constant = draw->line_width;
      }
   }
}

static void
panvk_emit_tiler_dcd(struct panvk_cmd_buffer *cmdbuf,
                     const struct panvk_draw_info *draw, void *dcd)
{
   const struct vk_rasterization_state *rs =
      &cmdbuf->vk.dynamic_graphics_state.rs;
   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;

   pan_pack(dcd, DRAW, cfg) {
      cfg.front_face_ccw = rs->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;
      cfg.cull_front_face = (rs->cull_mode & VK_CULL_MODE_FRONT_BIT) != 0;
      cfg.cull_back_face = (rs->cull_mode & VK_CULL_MODE_BACK_BIT) != 0;
      cfg.position = draw->position;
      cfg.state = draw->fs.rsd;
      cfg.attributes = draw->fs.attributes;
      cfg.attribute_buffers = draw->fs.attribute_bufs;
      cfg.viewport = draw->viewport;
      cfg.varyings = draw->fs.varyings;
      cfg.varying_buffers = cfg.varyings ? draw->varying_bufs : 0;
      cfg.thread_storage = draw->tls;

      /* For all primitives but lines DRAW.flat_shading_vertex must
       * be set to 0 and the provoking vertex is selected with the
       * PRIMITIVE.first_provoking_vertex field.
       */
      if (ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
          ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
         cfg.flat_shading_vertex = true;

      cfg.offset_start = draw->offset_start;
      cfg.instance_size =
         draw->instance_count > 1 ? draw->padded_vertex_count : 1;
      cfg.uniform_buffers = draw->ubos;
      cfg.push_uniforms = draw->push_uniforms;
      cfg.textures = draw->textures;
      cfg.samplers = draw->samplers;

      /* TODO: occlusion queries */
   }
}

static void
panvk_draw_prepare_tiler_job(struct panvk_cmd_buffer *cmdbuf,
                             struct panvk_draw_info *draw)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;
   struct panfrost_ptr ptr =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, TILER_JOB);

   /* If the vertex job doesn't write the position, we don't need a tiler job. */
   if (!draw->position)
      return;

   util_dynarray_append(&batch->jobs, void *, ptr.cpu);
   draw->jobs.tiler = ptr;

   memcpy(pan_section_ptr(ptr.cpu, TILER_JOB, INVOCATION), &draw->invocation,
          pan_size(INVOCATION));

   panvk_emit_tiler_primitive(cmdbuf, draw,
                              pan_section_ptr(ptr.cpu, TILER_JOB, PRIMITIVE));

   panvk_emit_tiler_primitive_size(
      cmdbuf, draw, pan_section_ptr(ptr.cpu, TILER_JOB, PRIMITIVE_SIZE));

   panvk_emit_tiler_dcd(cmdbuf, draw,
                        pan_section_ptr(ptr.cpu, TILER_JOB, DRAW));

   pan_section_pack(ptr.cpu, TILER_JOB, TILER, cfg) {
      cfg.address = draw->tiler_ctx->bifrost;
   }

   pan_section_pack(ptr.cpu, TILER_JOB, PADDING, padding)
      ;
}

static void
panvk_cmd_preload_fb_after_batch_split(struct panvk_cmd_buffer *cmdbuf)
{
   for (unsigned i = 0; i < cmdbuf->state.gfx.fb.info.rt_count; i++) {
      if (cmdbuf->state.gfx.fb.info.rts[i].view) {
         cmdbuf->state.gfx.fb.info.rts[i].clear = false;
         cmdbuf->state.gfx.fb.info.rts[i].preload = true;
      }
   }

   if (cmdbuf->state.gfx.fb.info.zs.view.zs) {
      cmdbuf->state.gfx.fb.info.zs.clear.z = false;
      cmdbuf->state.gfx.fb.info.zs.preload.z = true;
   }

   if (cmdbuf->state.gfx.fb.info.zs.view.s ||
       (cmdbuf->state.gfx.fb.info.zs.view.zs &&
        util_format_is_depth_and_stencil(
           cmdbuf->state.gfx.fb.info.zs.view.zs->format))) {
      cmdbuf->state.gfx.fb.info.zs.clear.s = false;
      cmdbuf->state.gfx.fb.info.zs.preload.s = true;
   }
}

struct panvk_batch *
panvk_per_arch(cmd_open_batch)(struct panvk_cmd_buffer *cmdbuf)
{
   assert(!cmdbuf->cur_batch);
   cmdbuf->cur_batch =
      vk_zalloc(&cmdbuf->vk.pool->alloc, sizeof(*cmdbuf->cur_batch), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   util_dynarray_init(&cmdbuf->cur_batch->jobs, NULL);
   util_dynarray_init(&cmdbuf->cur_batch->event_ops, NULL);
   assert(cmdbuf->cur_batch);
   return cmdbuf->cur_batch;
}

static void
panvk_cmd_draw(struct panvk_cmd_buffer *cmdbuf, struct panvk_draw_info *draw)
{
   struct panvk_batch *batch = cmdbuf->cur_batch;
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   const struct panvk_graphics_pipeline *pipeline = cmdbuf->state.gfx.pipeline;
   const struct vk_rasterization_state *rs =
      &cmdbuf->vk.dynamic_graphics_state.rs;

   /* There are only 16 bits in the descriptor for the job ID, make sure all
    * the 3 (2 in Bifrost) jobs in this draw are in the same batch.
    */
   if (batch->jc.job_index >= (UINT16_MAX - 3)) {
      panvk_per_arch(cmd_close_batch)(cmdbuf);
      panvk_cmd_preload_fb_after_batch_split(cmdbuf);
      batch = panvk_per_arch(cmd_open_batch)(cmdbuf);
   }

   if (!rs->rasterizer_discard_enable)
      panvk_per_arch(cmd_alloc_fb_desc)(cmdbuf);

   panvk_per_arch(cmd_alloc_tls_desc)(cmdbuf, true);

   panvk_cmd_prepare_draw_sysvals(cmdbuf, draw);
   panvk_cmd_prepare_push_sets(cmdbuf, desc_state, &pipeline->base);
   panvk_cmd_prepare_push_uniforms(cmdbuf, desc_state,
                                   &cmdbuf->state.gfx.sysvals,
                                   sizeof(cmdbuf->state.gfx.sysvals));
   panvk_cmd_prepare_ubos(cmdbuf, desc_state, &pipeline->base);
   panvk_cmd_prepare_textures(cmdbuf, desc_state, &pipeline->base);
   panvk_cmd_prepare_samplers(cmdbuf, desc_state, &pipeline->base);

   /* TODO: indexed draws */
   draw->tls = batch->tls.gpu;
   draw->fb = batch->fb.desc.gpu;
   draw->ubos = desc_state->ubos;
   draw->push_uniforms = desc_state->push_uniforms;
   draw->textures = desc_state->textures;
   draw->samplers = desc_state->samplers;

   panfrost_pack_work_groups_compute(&draw->invocation, 1, draw->vertex_range,
                                     draw->instance_count, 1, 1, 1, true,
                                     false);

   panvk_draw_prepare_fs_rsd(cmdbuf, draw);
   panvk_draw_prepare_varyings(cmdbuf, draw);
   panvk_draw_prepare_attributes(cmdbuf, draw);
   panvk_draw_prepare_viewport(cmdbuf, draw);
   panvk_draw_prepare_tiler_context(cmdbuf, draw);
   panvk_draw_prepare_vertex_job(cmdbuf, draw);
   panvk_draw_prepare_tiler_job(cmdbuf, draw);
   batch->tlsinfo.tls.size =
      MAX3(pipeline->vs.info.tls_size, pipeline->fs.info.tls_size,
           batch->tlsinfo.tls.size);

   unsigned vjob_id =
      pan_jc_add_job(&cmdbuf->desc_pool.base, &batch->jc, MALI_JOB_TYPE_VERTEX,
                     false, false, 0, 0, &draw->jobs.vertex, false);

   if (!rs->rasterizer_discard_enable && draw->position) {
      pan_jc_add_job(&cmdbuf->desc_pool.base, &batch->jc, MALI_JOB_TYPE_TILER,
                     false, false, vjob_id, 0, &draw->jobs.tiler, false);
   }

   /* Clear the dirty flags all at once */
   cmdbuf->state.gfx.dirty = 0;
   panvk_cmd_unprepare_push_sets(cmdbuf, desc_state);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                        uint32_t instanceCount, uint32_t firstVertex,
                        uint32_t firstInstance)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (instanceCount == 0 || vertexCount == 0)
      return;

   struct panvk_draw_info draw = {
      .first_vertex = firstVertex,
      .vertex_count = vertexCount,
      .vertex_range = vertexCount,
      .first_instance = firstInstance,
      .instance_count = instanceCount,
      .padded_vertex_count = instanceCount > 1
                                ? panfrost_padded_vertex_count(vertexCount)
                                : vertexCount,
      .offset_start = firstVertex,
   };

   panvk_cmd_draw(cmdbuf, &draw);
}

static void
panvk_index_minmax_search(struct panvk_cmd_buffer *cmdbuf, uint32_t start,
                          uint32_t count, bool restart, uint32_t *min,
                          uint32_t *max)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_instance *instance =
      to_panvk_instance(dev->vk.physical->instance);
   void *ptr =
      cmdbuf->state.gfx.ib.buffer->host_ptr + cmdbuf->state.gfx.ib.offset;

   assert(cmdbuf->state.gfx.ib.buffer);
   assert(cmdbuf->state.gfx.ib.buffer->bo);
   assert(cmdbuf->state.gfx.ib.buffer->host_ptr);

   if (!(instance->debug_flags & PANVK_DEBUG_NO_KNOWN_WARN)) {
      fprintf(
         stderr,
         "WARNING: Crawling index buffers from the CPU isn't valid in Vulkan\n");
   }

   *max = 0;

   /* TODO: Use panfrost_minmax_cache */
   /* TODO: Read full cacheline of data to mitigate the uncached
    * mapping slowness.
    */
   switch (cmdbuf->state.gfx.ib.index_size) {
#define MINMAX_SEARCH_CASE(sz)                                                 \
   case sz: {                                                                  \
      uint##sz##_t *indices = ptr;                                             \
      *min = UINT##sz##_MAX;                                                   \
      for (uint32_t i = 0; i < count; i++) {                                   \
         if (restart && indices[i + start] == UINT##sz##_MAX)                  \
            continue;                                                          \
         *min = MIN2(indices[i + start], *min);                                \
         *max = MAX2(indices[i + start], *max);                                \
      }                                                                        \
      break;                                                                   \
   }
      MINMAX_SEARCH_CASE(32)
      MINMAX_SEARCH_CASE(16)
      MINMAX_SEARCH_CASE(8)
#undef MINMAX_SEARCH_CASE
   default:
      unreachable("Invalid index size");
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDrawIndexed)(VkCommandBuffer commandBuffer,
                               uint32_t indexCount, uint32_t instanceCount,
                               uint32_t firstIndex, int32_t vertexOffset,
                               uint32_t firstInstance)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   uint32_t min_vertex, max_vertex;

   if (instanceCount == 0 || indexCount == 0)
      return;

   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;
   bool primitive_restart = ia->primitive_restart_enable;

   panvk_index_minmax_search(cmdbuf, firstIndex, indexCount, primitive_restart,
                             &min_vertex, &max_vertex);

   unsigned vertex_range = max_vertex - min_vertex + 1;
   struct panvk_draw_info draw = {
      .index_size = cmdbuf->state.gfx.ib.index_size,
      .first_index = firstIndex,
      .index_count = indexCount,
      .vertex_offset = vertexOffset,
      .first_instance = firstInstance,
      .instance_count = instanceCount,
      .vertex_range = vertex_range,
      .vertex_count = indexCount + abs(vertexOffset),
      .padded_vertex_count = instanceCount > 1
                                ? panfrost_padded_vertex_count(vertex_range)
                                : vertex_range,
      .offset_start = min_vertex + vertexOffset,
      .indices = panvk_buffer_gpu_ptr(cmdbuf->state.gfx.ib.buffer,
                                      cmdbuf->state.gfx.ib.offset) +
                 (firstIndex * (cmdbuf->state.gfx.ib.index_size / 8)),
   };

   panvk_cmd_draw(cmdbuf, &draw);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(EndCommandBuffer)(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   panvk_per_arch(cmd_close_batch)(cmdbuf);

   return vk_command_buffer_end(&cmdbuf->vk);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPipelineBarrier2)(VkCommandBuffer commandBuffer,
                                    const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   /* Caches are flushed/invalidated at batch boundaries for now, nothing to do
    * for memory barriers assuming we implement barriers with the creation of a
    * new batch.
    * FIXME: We can probably do better with a CacheFlush job that has the
    * barrier flag set to true.
    */
   if (cmdbuf->cur_batch) {
      panvk_per_arch(cmd_close_batch)(cmdbuf);
      panvk_cmd_preload_fb_after_batch_split(cmdbuf);
      panvk_per_arch(cmd_open_batch)(cmdbuf);
   }
}

static void
panvk_add_set_event_operation(struct panvk_cmd_buffer *cmdbuf,
                              struct panvk_event *event,
                              enum panvk_cmd_event_op_type type)
{
   struct panvk_cmd_event_op op = {
      .type = type,
      .event = event,
   };

   if (cmdbuf->cur_batch == NULL) {
      /* No open batch, let's create a new one so this operation happens in
       * the right order.
       */
      panvk_per_arch(cmd_open_batch)(cmdbuf);
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
      panvk_per_arch(cmd_close_batch)(cmdbuf);
   } else {
      /* Let's close the current batch so the operation executes before any
       * future commands.
       */
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
      panvk_per_arch(cmd_close_batch)(cmdbuf);
      panvk_cmd_preload_fb_after_batch_split(cmdbuf);
      panvk_per_arch(cmd_open_batch)(cmdbuf);
   }
}

static void
panvk_add_wait_event_operation(struct panvk_cmd_buffer *cmdbuf,
                               struct panvk_event *event)
{
   struct panvk_cmd_event_op op = {
      .type = PANVK_EVENT_OP_WAIT,
      .event = event,
   };

   if (cmdbuf->cur_batch == NULL) {
      /* No open batch, let's create a new one and have it wait for this event. */
      panvk_per_arch(cmd_open_batch)(cmdbuf);
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
   } else {
      /* Let's close the current batch so any future commands wait on the
       * event signal operation.
       */
      if (cmdbuf->cur_batch->fragment_job || cmdbuf->cur_batch->jc.first_job) {
         panvk_per_arch(cmd_close_batch)(cmdbuf);
         panvk_cmd_preload_fb_after_batch_split(cmdbuf);
         panvk_per_arch(cmd_open_batch)(cmdbuf);
      }
      util_dynarray_append(&cmdbuf->cur_batch->event_ops,
                           struct panvk_cmd_event_op, op);
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdSetEvent2)(VkCommandBuffer commandBuffer, VkEvent _event,
                             const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_event, event, _event);

   /* vkCmdSetEvent cannot be called inside a render pass */
   assert(cmdbuf->vk.render_pass == NULL);

   panvk_add_set_event_operation(cmdbuf, event, PANVK_EVENT_OP_SET);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdResetEvent2)(VkCommandBuffer commandBuffer, VkEvent _event,
                               VkPipelineStageFlags2 stageMask)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_event, event, _event);

   /* vkCmdResetEvent cannot be called inside a render pass */
   assert(cmdbuf->vk.render_pass == NULL);

   panvk_add_set_event_operation(cmdbuf, event, PANVK_EVENT_OP_RESET);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdWaitEvents2)(VkCommandBuffer commandBuffer,
                               uint32_t eventCount, const VkEvent *pEvents,
                               const VkDependencyInfo *pDependencyInfos)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   assert(eventCount > 0);

   for (uint32_t i = 0; i < eventCount; i++) {
      VK_FROM_HANDLE(panvk_event, event, pEvents[i]);
      panvk_add_wait_event_operation(cmdbuf, event);
   }
}

static void
panvk_reset_cmdbuf(struct vk_command_buffer *vk_cmdbuf,
                   VkCommandBufferResetFlags flags)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);

   vk_command_buffer_reset(&cmdbuf->vk);

   list_for_each_entry_safe(struct panvk_batch, batch, &cmdbuf->batches, node) {
      list_del(&batch->node);
      util_dynarray_fini(&batch->jobs);
      util_dynarray_fini(&batch->event_ops);

      vk_free(&cmdbuf->vk.pool->alloc, batch);
   }

   panvk_pool_reset(&cmdbuf->desc_pool);
   panvk_pool_reset(&cmdbuf->tls_pool);
   panvk_pool_reset(&cmdbuf->varying_pool);

   memset(&cmdbuf->state.gfx.desc_state.sets, 0,
          sizeof(cmdbuf->state.gfx.desc_state.sets));
   memset(&cmdbuf->state.compute.desc_state.sets, 0,
          sizeof(cmdbuf->state.compute.desc_state.sets));
}

static void
panvk_destroy_cmdbuf(struct vk_command_buffer *vk_cmdbuf)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);

   for (unsigned i = 0; i < MAX_SETS; i++) {
      if (cmdbuf->state.gfx.desc_state.push_sets[i])
         vk_free(&cmdbuf->vk.pool->alloc,
                 cmdbuf->state.gfx.desc_state.push_sets[i]);
      if (cmdbuf->state.compute.desc_state.push_sets[i])
         vk_free(&cmdbuf->vk.pool->alloc,
                 cmdbuf->state.compute.desc_state.push_sets[i]);
   }

   list_for_each_entry_safe(struct panvk_batch, batch, &cmdbuf->batches, node) {
      list_del(&batch->node);
      util_dynarray_fini(&batch->jobs);
      util_dynarray_fini(&batch->event_ops);

      vk_free(&cmdbuf->vk.pool->alloc, batch);
   }

   panvk_pool_cleanup(&cmdbuf->desc_pool);
   panvk_pool_cleanup(&cmdbuf->tls_pool);
   panvk_pool_cleanup(&cmdbuf->varying_pool);
   vk_command_buffer_finish(&cmdbuf->vk);
   vk_free(&dev->vk.alloc, cmdbuf);
}

static VkResult
panvk_create_cmdbuf(struct vk_command_pool *vk_pool, VkCommandBufferLevel level,
                    struct vk_command_buffer **cmdbuf_out)
{
   struct panvk_device *device =
      container_of(vk_pool->base.device, struct panvk_device, vk);
   struct panvk_cmd_pool *pool =
      container_of(vk_pool, struct panvk_cmd_pool, vk);
   struct panvk_cmd_buffer *cmdbuf;

   cmdbuf = vk_zalloc(&device->vk.alloc, sizeof(*cmdbuf), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmdbuf)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_buffer_init(
      &pool->vk, &cmdbuf->vk, &panvk_per_arch(cmd_buffer_ops), level);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, cmdbuf);
      return result;
   }

   cmdbuf->vk.dynamic_graphics_state.vi = &cmdbuf->state.gfx.dynamic.vi;
   cmdbuf->vk.dynamic_graphics_state.ms.sample_locations =
      &cmdbuf->state.gfx.dynamic.sl;

   panvk_pool_init(&cmdbuf->desc_pool, device, &pool->desc_bo_pool, 0,
                   64 * 1024, "Command buffer descriptor pool", true);
   panvk_pool_init(
      &cmdbuf->tls_pool, device, &pool->tls_bo_pool,
      panvk_debug_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_NO_MMAP), 64 * 1024,
      "TLS pool", false);
   panvk_pool_init(
      &cmdbuf->varying_pool, device, &pool->varying_bo_pool,
      panvk_debug_adjust_bo_flags(device, PAN_KMOD_BO_FLAG_NO_MMAP), 64 * 1024,
      "Varyings pool", false);
   list_inithead(&cmdbuf->batches);
   *cmdbuf_out = &cmdbuf->vk;
   return VK_SUCCESS;
}

const struct vk_command_buffer_ops panvk_per_arch(cmd_buffer_ops) = {
   .create = panvk_create_cmdbuf,
   .reset = panvk_reset_cmdbuf,
   .destroy = panvk_destroy_cmdbuf,
};

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(BeginCommandBuffer)(VkCommandBuffer commandBuffer,
                                   const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   vk_command_buffer_begin(&cmdbuf->vk, pBeginInfo);

   memset(&cmdbuf->state, 0, sizeof(cmdbuf->state));

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatch)(VkCommandBuffer commandBuffer, uint32_t x,
                            uint32_t y, uint32_t z)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct panvk_dispatch_info dispatch = {
      .wg_count = {x, y, z},
   };

   panvk_per_arch(cmd_close_batch)(cmdbuf);
   struct panvk_batch *batch = panvk_per_arch(cmd_open_batch)(cmdbuf);

   struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.compute.desc_state;
   const struct panvk_compute_pipeline *pipeline =
      cmdbuf->state.compute.pipeline;
   struct panfrost_ptr job =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, COMPUTE_JOB);

   struct panvk_compute_sysvals *sysvals = &cmdbuf->state.compute.sysvals;
   sysvals->num_work_groups.x = x;
   sysvals->num_work_groups.y = y;
   sysvals->num_work_groups.z = z;
   sysvals->local_group_size.x = pipeline->local_size.x;
   sysvals->local_group_size.y = pipeline->local_size.y;
   sysvals->local_group_size.z = pipeline->local_size.z;
   desc_state->push_uniforms = 0;

   panvk_per_arch(cmd_alloc_tls_desc)(cmdbuf, false);
   dispatch.tsd = batch->tls.gpu;

   panvk_cmd_prepare_push_sets(cmdbuf, desc_state, &pipeline->base);

   if (pipeline->cs.has_img_access)
      panvk_prepare_img_attribs(cmdbuf, desc_state, &pipeline->base);

   dispatch.attributes = desc_state->img.attribs;
   dispatch.attribute_bufs = desc_state->img.attrib_bufs;

   panvk_cmd_prepare_ubos(cmdbuf, desc_state, &pipeline->base);
   dispatch.ubos = desc_state->ubos;

   panvk_cmd_prepare_push_uniforms(cmdbuf, desc_state,
                                   &cmdbuf->state.compute.sysvals,
                                   sizeof(cmdbuf->state.compute.sysvals));
   dispatch.push_uniforms = desc_state->push_uniforms;

   panvk_cmd_prepare_textures(cmdbuf, desc_state, &pipeline->base);
   dispatch.textures = desc_state->textures;

   panvk_cmd_prepare_samplers(cmdbuf, desc_state, &pipeline->base);
   dispatch.samplers = desc_state->samplers;

   panfrost_pack_work_groups_compute(
      pan_section_ptr(job.cpu, COMPUTE_JOB, INVOCATION), dispatch.wg_count.x,
      dispatch.wg_count.y, dispatch.wg_count.z, pipeline->local_size.x,
      pipeline->local_size.y, pipeline->local_size.z, false, false);

   pan_section_pack(job.cpu, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = util_logbase2_ceil(pipeline->local_size.x + 1) +
                           util_logbase2_ceil(pipeline->local_size.y + 1) +
                           util_logbase2_ceil(pipeline->local_size.z + 1);
   }

   pan_section_pack(job.cpu, COMPUTE_JOB, DRAW, cfg) {
      cfg.state = pipeline->cs.rsd;
      cfg.attributes = dispatch.attributes;
      cfg.attribute_buffers = dispatch.attribute_bufs;
      cfg.thread_storage = dispatch.tsd;
      cfg.uniform_buffers = dispatch.ubos;
      cfg.push_uniforms = dispatch.push_uniforms;
      cfg.textures = dispatch.textures;
      cfg.samplers = dispatch.samplers;
   }

   pan_jc_add_job(&cmdbuf->desc_pool.base, &batch->jc, MALI_JOB_TYPE_COMPUTE,
                  false, false, 0, 0, &job, false);

   batch->tlsinfo.tls.size = pipeline->cs.info.tls_size;
   batch->tlsinfo.wls.size = pipeline->cs.info.wls_size;
   if (batch->tlsinfo.wls.size) {
      unsigned core_id_range;

      panfrost_query_core_count(&phys_dev->kmod.props, &core_id_range);
      batch->tlsinfo.wls.instances = pan_wls_instances(&dispatch.wg_count);
      batch->wls_total_size = pan_wls_adjust_size(batch->tlsinfo.wls.size) *
                              batch->tlsinfo.wls.instances * core_id_range;
   }

   panvk_per_arch(cmd_close_batch)(cmdbuf);
   panvk_cmd_unprepare_push_sets(cmdbuf, desc_state);
}

static void
panvk_cmd_begin_rendering_init_fbinfo(struct panvk_cmd_buffer *cmdbuf,
                                      const VkRenderingInfo *pRenderingInfo)
{
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.fb.info;
   uint32_t att_width = 0, att_height = 0;

   cmdbuf->state.gfx.fb.bo_count = 0;
   memset(cmdbuf->state.gfx.fb.bos, 0, sizeof(cmdbuf->state.gfx.fb.bos));
   memset(cmdbuf->state.gfx.fb.crc_valid, 0,
          sizeof(cmdbuf->state.gfx.fb.crc_valid));
   memset(&cmdbuf->state.gfx.fb.color_attachments, 0,
          sizeof(cmdbuf->state.gfx.fb.color_attachments));
   cmdbuf->state.gfx.fb.bound_attachments = 0;

   *fbinfo = (struct pan_fb_info){
      .tile_buf_budget = panfrost_query_optimal_tib_size(phys_dev->model),
      .nr_samples = 1,
      .rt_count = pRenderingInfo->colorAttachmentCount,
   };

   assert(pRenderingInfo->colorAttachmentCount <= ARRAY_SIZE(fbinfo->rts));

   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att =
         &pRenderingInfo->pColorAttachments[i];
      VK_FROM_HANDLE(panvk_image_view, iview, att->imageView);

      if (!iview)
         continue;

      struct panvk_image *img =
         container_of(iview->vk.image, struct panvk_image, vk);
      const VkExtent3D iview_size =
         vk_image_mip_level_extent(&img->vk, iview->vk.base_mip_level);

      cmdbuf->state.gfx.fb.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_COLOR_BIT(i);
      cmdbuf->state.gfx.fb.color_attachments.fmts[i] = iview->vk.format;
      cmdbuf->state.gfx.fb.color_attachments.samples[i] = img->vk.samples;
      att_width = MAX2(iview_size.width, att_width);
      att_height = MAX2(iview_size.height, att_height);

      assert(att->resolveMode == VK_RESOLVE_MODE_NONE);

      cmdbuf->state.gfx.fb.bos[cmdbuf->state.gfx.fb.bo_count++] = img->bo;
      fbinfo->rts[i].view = &iview->pview;
      fbinfo->rts[i].crc_valid = &cmdbuf->state.gfx.fb.crc_valid[i];
      fbinfo->nr_samples =
         MAX2(fbinfo->nr_samples, pan_image_view_get_nr_samples(&iview->pview));

      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         enum pipe_format fmt = vk_format_to_pipe_format(iview->vk.format);
         union pipe_color_union *col =
            (union pipe_color_union *)&att->clearValue.color;

         fbinfo->rts[i].clear = true;
         pan_pack_color(phys_dev->formats.blendable, fbinfo->rts[i].clear_value,
                        col, fmt, false);
      } else if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         fbinfo->rts[i].preload = true;
      }
   }

   if (pRenderingInfo->pDepthAttachment &&
       pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE) {
      const VkRenderingAttachmentInfo *att = pRenderingInfo->pDepthAttachment;
      VK_FROM_HANDLE(panvk_image_view, iview, att->imageView);
      struct panvk_image *img =
         container_of(iview->vk.image, struct panvk_image, vk);
      const VkExtent3D iview_size =
         vk_image_mip_level_extent(&img->vk, iview->vk.base_mip_level);

      cmdbuf->state.gfx.fb.bound_attachments |= MESA_VK_RP_ATTACHMENT_DEPTH_BIT;
      att_width = MAX2(iview_size.width, att_width);
      att_height = MAX2(iview_size.height, att_height);

      assert(att->resolveMode == VK_RESOLVE_MODE_NONE);

      cmdbuf->state.gfx.fb.bos[cmdbuf->state.gfx.fb.bo_count++] = img->bo;
      fbinfo->zs.view.zs = &iview->pview;

      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         fbinfo->zs.clear.z = true;
         fbinfo->zs.clear_value.depth = att->clearValue.depthStencil.depth;
      } else if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         fbinfo->zs.preload.z = true;
      }
   }

   if (pRenderingInfo->pStencilAttachment &&
       pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE) {
      const VkRenderingAttachmentInfo *att = pRenderingInfo->pStencilAttachment;
      VK_FROM_HANDLE(panvk_image_view, iview, att->imageView);
      struct panvk_image *img =
         container_of(iview->vk.image, struct panvk_image, vk);
      const VkExtent3D iview_size =
         vk_image_mip_level_extent(&img->vk, iview->vk.base_mip_level);

      cmdbuf->state.gfx.fb.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_STENCIL_BIT;
      att_width = MAX2(iview_size.width, att_width);
      att_height = MAX2(iview_size.height, att_height);

      assert(att->resolveMode == VK_RESOLVE_MODE_NONE);

      cmdbuf->state.gfx.fb.bos[cmdbuf->state.gfx.fb.bo_count++] = img->bo;
      fbinfo->zs.view.s =
         &iview->pview != fbinfo->zs.view.zs ? &iview->pview : NULL;

      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         fbinfo->zs.clear.s = true;
         fbinfo->zs.clear_value.stencil = att->clearValue.depthStencil.stencil;
      } else if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
         fbinfo->zs.preload.s = true;
      }
   }

   fbinfo->width = pRenderingInfo->renderArea.offset.x +
                   pRenderingInfo->renderArea.extent.width;
   fbinfo->height = pRenderingInfo->renderArea.offset.y +
                    pRenderingInfo->renderArea.extent.height;

   if (cmdbuf->state.gfx.fb.bound_attachments) {
      /* We need the rendering area to be aligned on a 32x32 section for tile
       * buffer preloading to work correctly.
       */
      fbinfo->width = MIN2(att_width, ALIGN_POT(fbinfo->width, 32));
      fbinfo->height = MIN2(att_height, ALIGN_POT(fbinfo->height, 32));
   }

   assert(fbinfo->width && fbinfo->height);

   fbinfo->extent.maxx = fbinfo->width - 1;
   fbinfo->extent.maxy = fbinfo->height - 1;

   /* We need to re-emit the FS RSD when the color attachments change. */
   cmdbuf->state.gfx.fs.rsd = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBeginRendering)(VkCommandBuffer commandBuffer,
                                  const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   panvk_cmd_begin_rendering_init_fbinfo(cmdbuf, pRenderingInfo);
   panvk_per_arch(cmd_open_batch)(cmdbuf);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdEndRendering)(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   panvk_per_arch(cmd_close_batch)(cmdbuf);
   cmdbuf->cur_batch = NULL;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindVertexBuffers)(VkCommandBuffer commandBuffer,
                                     uint32_t firstBinding,
                                     uint32_t bindingCount,
                                     const VkBuffer *pBuffers,
                                     const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   assert(firstBinding + bindingCount <= MAX_VBS);

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(panvk_buffer, buffer, pBuffers[i]);

      cmdbuf->state.gfx.vb.bufs[firstBinding + i].address =
         panvk_buffer_gpu_ptr(buffer, pOffsets[i]);
      cmdbuf->state.gfx.vb.bufs[firstBinding + i].size =
         panvk_buffer_range(buffer, pOffsets[i], VK_WHOLE_SIZE);
   }

   cmdbuf->state.gfx.vb.count =
      MAX2(cmdbuf->state.gfx.vb.count, firstBinding + bindingCount);
   cmdbuf->state.gfx.vs.attrib_bufs = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindIndexBuffer)(VkCommandBuffer commandBuffer,
                                   VkBuffer buffer, VkDeviceSize offset,
                                   VkIndexType indexType)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_buffer, buf, buffer);

   cmdbuf->state.gfx.ib.buffer = buf;
   cmdbuf->state.gfx.ib.offset = offset;
   switch (indexType) {
   case VK_INDEX_TYPE_UINT16:
      cmdbuf->state.gfx.ib.index_size = 16;
      break;
   case VK_INDEX_TYPE_UINT32:
      cmdbuf->state.gfx.ib.index_size = 32;
      break;
   case VK_INDEX_TYPE_NONE_KHR:
      cmdbuf->state.gfx.ib.index_size = 0;
      break;
   case VK_INDEX_TYPE_UINT8_EXT:
      cmdbuf->state.gfx.ib.index_size = 8;
      break;
   default:
      unreachable("Invalid index type\n");
   }
}

static void
panvk_emit_dyn_ubo(struct panvk_descriptor_state *desc_state,
                   const struct panvk_descriptor_set *desc_set,
                   unsigned binding, unsigned array_idx, uint32_t dyn_offset,
                   unsigned dyn_ubo_slot)
{
   struct mali_uniform_buffer_packed *ubo = &desc_state->dyn.ubos[dyn_ubo_slot];
   const struct panvk_descriptor_set_layout *slayout = desc_set->layout;
   VkDescriptorType type = slayout->bindings[binding].type;

   assert(type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
   assert(dyn_ubo_slot < ARRAY_SIZE(desc_state->dyn.ubos));

   const unsigned dyn_ubo_idx = slayout->bindings[binding].dyn_ubo_idx;
   const struct panvk_buffer_desc *bdesc =
      &desc_set->dyn_ubos[dyn_ubo_idx + array_idx];
   mali_ptr address =
      panvk_buffer_gpu_ptr(bdesc->buffer, bdesc->offset + dyn_offset);
   size_t size = panvk_buffer_range(bdesc->buffer, bdesc->offset + dyn_offset,
                                    bdesc->size);

   if (size) {
      pan_pack(ubo, UNIFORM_BUFFER, cfg) {
         cfg.pointer = address;
         cfg.entries = DIV_ROUND_UP(size, 16);
      }
   } else {
      memset(ubo, 0, sizeof(*ubo));
   }
}

static void
panvk_emit_dyn_ssbo(struct panvk_descriptor_state *desc_state,
                    const struct panvk_descriptor_set *desc_set,
                    unsigned binding, unsigned array_idx, uint32_t dyn_offset,
                    unsigned dyn_ssbo_slot)
{
   struct panvk_ssbo_addr *ssbo = &desc_state->dyn.ssbos[dyn_ssbo_slot];
   const struct panvk_descriptor_set_layout *slayout = desc_set->layout;
   VkDescriptorType type = slayout->bindings[binding].type;

   assert(type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
   assert(dyn_ssbo_slot < ARRAY_SIZE(desc_state->dyn.ssbos));

   const unsigned dyn_ssbo_idx = slayout->bindings[binding].dyn_ssbo_idx;
   const struct panvk_buffer_desc *bdesc =
      &desc_set->dyn_ssbos[dyn_ssbo_idx + array_idx];

   *ssbo = (struct panvk_ssbo_addr){
      .base_addr =
         panvk_buffer_gpu_ptr(bdesc->buffer, bdesc->offset + dyn_offset),
      .size = panvk_buffer_range(bdesc->buffer, bdesc->offset + dyn_offset,
                                 bdesc->size),
   };
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindDescriptorSets)(
   VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount,
   const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount,
   const uint32_t *pDynamicOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);

   struct panvk_descriptor_state *descriptors_state =
      panvk_cmd_get_desc_state(cmdbuf, pipelineBindPoint);

   unsigned dynoffset_idx = 0;
   for (unsigned i = 0; i < descriptorSetCount; ++i) {
      unsigned idx = i + firstSet;
      VK_FROM_HANDLE(panvk_descriptor_set, set, pDescriptorSets[i]);

      descriptors_state->sets[idx] = set;

      if (set->layout->num_dyn_ssbos || set->layout->num_dyn_ubos) {
         unsigned dyn_ubo_slot = playout->sets[idx].dyn_ubo_offset;
         unsigned dyn_ssbo_slot = playout->sets[idx].dyn_ssbo_offset;

         for (unsigned b = 0; b < set->layout->binding_count; b++) {
            for (unsigned e = 0; e < set->layout->bindings[b].array_size; e++) {
               VkDescriptorType type = set->layout->bindings[b].type;

               if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                  panvk_emit_dyn_ubo(descriptors_state, set, b, e,
                                     pDynamicOffsets[dynoffset_idx++],
                                     dyn_ubo_slot++);
               } else if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                  panvk_emit_dyn_ssbo(descriptors_state, set, b, e,
                                      pDynamicOffsets[dynoffset_idx++],
                                      dyn_ssbo_slot++);
               }
            }
         }
      }
   }

   /* Unconditionally reset all previously emitted descriptors tables.
    * TODO: we could be smarter by checking which part of the pipeline layout
    * are compatible with the previouly bound descriptor sets.
    */
   descriptors_state->ubos = 0;
   descriptors_state->textures = 0;
   descriptors_state->samplers = 0;
   descriptors_state->dyn_desc_ubo = 0;
   descriptors_state->img.attrib_bufs = 0;
   descriptors_state->img.attribs = 0;

   assert(dynoffset_idx == dynamicOffsetCount);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushConstants)(VkCommandBuffer commandBuffer,
                                 VkPipelineLayout layout,
                                 VkShaderStageFlags stageFlags, uint32_t offset,
                                 uint32_t size, const void *pValues)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   memcpy(cmdbuf->push_constants + offset, pValues, size);

   if (stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS) {
      struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;

      desc_state->push_uniforms = 0;
   }

   if (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct panvk_descriptor_state *desc_state =
         &cmdbuf->state.compute.desc_state;

      desc_state->push_uniforms = 0;
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBindPipeline)(VkCommandBuffer commandBuffer,
                                VkPipelineBindPoint pipelineBindPoint,
                                VkPipeline _pipeline)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      struct panvk_graphics_pipeline *gfx_pipeline =
         panvk_pipeline_to_graphics_pipeline(pipeline);

      vk_cmd_set_dynamic_graphics_state(&cmdbuf->vk,
                                        &gfx_pipeline->state.dynamic);

      cmdbuf->state.gfx.fs.rsd = 0;
      cmdbuf->state.gfx.pipeline = gfx_pipeline;
      break;
   }

   case VK_PIPELINE_BIND_POINT_COMPUTE:
      cmdbuf->state.compute.pipeline =
         panvk_pipeline_to_compute_pipeline(pipeline);
      break;

   default:
      assert(!"Unsupported bind point");
      break;
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDrawIndirect)(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                VkDeviceSize offset, uint32_t drawCount,
                                uint32_t stride)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDrawIndexedIndirect)(VkCommandBuffer commandBuffer,
                                       VkBuffer _buffer, VkDeviceSize offset,
                                       uint32_t drawCount, uint32_t stride)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatchBase)(VkCommandBuffer commandBuffer, uint32_t base_x,
                                uint32_t base_y, uint32_t base_z, uint32_t x,
                                uint32_t y, uint32_t z)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatchIndirect)(VkCommandBuffer commandBuffer,
                                    VkBuffer _buffer, VkDeviceSize offset)
{
   panvk_stub();
}

static struct panvk_push_descriptor_set *
panvk_cmd_push_descriptors(struct panvk_cmd_buffer *cmdbuf,
                           VkPipelineBindPoint bind_point, uint32_t set)
{
   struct panvk_descriptor_state *desc_state =
      panvk_cmd_get_desc_state(cmdbuf, bind_point);

   assert(set < MAX_SETS);
   if (unlikely(desc_state->push_sets[set] == NULL)) {
      desc_state->push_sets[set] =
         vk_zalloc(&cmdbuf->vk.pool->alloc, sizeof(*desc_state->push_sets[0]),
                   8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(desc_state->push_sets[set] == NULL)) {
         vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
   }

   /* Pushing descriptors replaces whatever sets are bound */
   desc_state->sets[set] = NULL;

   /* Reset all descs to force emission of new tables on the next draw/dispatch.
    * TODO: Be smarter and only reset those when required.
    */
   desc_state->ubos = 0;
   desc_state->textures = 0;
   desc_state->samplers = 0;
   desc_state->img.attrib_bufs = 0;
   desc_state->img.attribs = 0;
   return desc_state->push_sets[set];
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushDescriptorSetKHR)(
   VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount,
   const VkWriteDescriptorSet *pDescriptorWrites)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(playout->vk.set_layouts[set]);
   struct panvk_push_descriptor_set *push_set =
      panvk_cmd_push_descriptors(cmdbuf, pipelineBindPoint, set);
   if (!push_set)
      return;

   panvk_per_arch(push_descriptor_set)(push_set, set_layout,
                                       descriptorWriteCount, pDescriptorWrites);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushDescriptorSetWithTemplateKHR)(
   VkCommandBuffer commandBuffer,
   VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout,
   uint32_t set, const void *pData)
{
   VK_FROM_HANDLE(vk_descriptor_update_template, template,
                  descriptorUpdateTemplate);
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);
   const struct panvk_descriptor_set_layout *set_layout =
      vk_to_panvk_descriptor_set_layout(playout->vk.set_layouts[set]);
   struct panvk_push_descriptor_set *push_set =
      panvk_cmd_push_descriptors(cmdbuf, template->bind_point, set);
   if (!push_set)
      return;

   panvk_per_arch(push_descriptor_set_with_template)(
      push_set, set_layout, descriptorUpdateTemplate, pData);
}
