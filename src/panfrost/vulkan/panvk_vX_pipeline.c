/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_pipeline.c which is:
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

#include "panvk_cmd_buffer.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"
#include "panvk_priv_bo.h"
#include "panvk_shader.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/blend.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "vk_blend.h"
#include "vk_format.h"
#include "vk_pipeline_cache.h"
#include "vk_render_pass.h"
#include "vk_util.h"

#include "panfrost/util/pan_lower_framebuffer.h"

#include "pan_shader.h"

static VkResult
init_pipeline_shader(struct panvk_pipeline *pipeline,
                     const VkPipelineShaderStageCreateInfo *stage_info,
                     const VkAllocationCallbacks *alloc,
                     struct panvk_pipeline_shader *pshader)
{
   struct panvk_device *dev = to_panvk_device(pipeline->base.device);
   struct panvk_shader *shader;

   shader =
      panvk_per_arch(shader_create)(dev, stage_info, pipeline->layout, alloc);
   if (!shader)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   void *shader_data = util_dynarray_element(&shader->binary, uint8_t, 0);
   unsigned shader_sz = util_dynarray_num_elements(&shader->binary, uint8_t);

   if (shader_sz) {
      pshader->code = pan_pool_upload_aligned(&pipeline->bin_pool.base,
                                              shader_data, shader_sz, 128);
   } else {
      pshader->code = 0;
   }

   pshader->info = shader->info;
   pshader->has_img_access = shader->has_img_access;

   if (stage_info->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      struct panvk_compute_pipeline *compute_pipeline =
         panvk_pipeline_to_compute_pipeline(pipeline);

      compute_pipeline->local_size = shader->local_size;
   }

   if (stage_info->stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
      struct panfrost_ptr rsd =
         pan_pool_alloc_desc(&pipeline->desc_pool.base, RENDERER_STATE);

      pan_pack(rsd.cpu, RENDERER_STATE, cfg) {
         pan_shader_prepare_rsd(&pshader->info, pshader->code, &cfg);
      }

      pshader->rsd = rsd.gpu;
   }

   panvk_per_arch(shader_destroy)(dev, shader, alloc);
   return VK_SUCCESS;
}

static mali_pixel_format
get_varying_format(gl_shader_stage stage, gl_varying_slot loc,
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
      assert(pfmt != PIPE_FORMAT_NONE);
      return GENX(panfrost_format_from_pipe_format)(pfmt)->hw;
   }
}

struct varyings_info {
   enum pipe_format fmts[VARYING_SLOT_MAX];
   BITSET_DECLARE(active, VARYING_SLOT_MAX);
};

static void
collect_varyings_info(const struct pan_shader_varying *varyings,
                      unsigned varying_count, struct varyings_info *info)
{
   for (unsigned i = 0; i < varying_count; i++) {
      gl_varying_slot loc = varyings[i].location;

      if (varyings[i].format == PIPE_FORMAT_NONE)
         continue;

      info->fmts[loc] = varyings[i].format;
      BITSET_SET(info->active, loc);
   }
}

static inline enum panvk_varying_buf_id
varying_buf_id(gl_varying_slot loc)
{
   switch (loc) {
   case VARYING_SLOT_POS:
      return PANVK_VARY_BUF_POSITION;
   case VARYING_SLOT_PSIZ:
      return PANVK_VARY_BUF_PSIZ;
   default:
      return PANVK_VARY_BUF_GENERAL;
   }
}

static mali_pixel_format
varying_format(gl_varying_slot loc, enum pipe_format pfmt)
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
      return GENX(panfrost_format_from_pipe_format)(pfmt)->hw;
   }
}

static mali_ptr
emit_varying_attrs(struct pan_pool *desc_pool,
                   const struct pan_shader_varying *varyings,
                   unsigned varying_count, const struct varyings_info *info,
                   unsigned *buf_offsets)
{
   unsigned attr_count = BITSET_COUNT(info->active);
   struct panfrost_ptr ptr =
      pan_pool_alloc_desc_array(desc_pool, attr_count, ATTRIBUTE);
   struct mali_attribute_packed *attrs = ptr.cpu;
   unsigned attr_idx = 0;

   for (unsigned i = 0; i < varying_count; i++) {
      pan_pack(&attrs[attr_idx++], ATTRIBUTE, cfg) {
         gl_varying_slot loc = varyings[i].location;
         enum pipe_format pfmt = varyings[i].format != PIPE_FORMAT_NONE
                                    ? info->fmts[loc]
                                    : PIPE_FORMAT_NONE;

         if (pfmt == PIPE_FORMAT_NONE) {
#if PAN_ARCH >= 7
            cfg.format = (MALI_CONSTANT << 12) | MALI_RGB_COMPONENT_ORDER_0000;
#else
            cfg.format = (MALI_CONSTANT << 12) | PAN_V6_SWIZZLE(0, 0, 0, 0);
#endif
         } else {
            cfg.buffer_index = varying_buf_id(loc);
            cfg.offset = buf_offsets[loc];
            cfg.format = varying_format(loc, info->fmts[loc]);
         }
         cfg.offset_enable = false;
      }
   }

   return ptr.gpu;
}

static void
link_shaders(struct panvk_graphics_pipeline *pipeline,
             struct panvk_pipeline_shader *stage,
             struct panvk_pipeline_shader *next_stage)
{
   BITSET_DECLARE(active_attrs, VARYING_SLOT_MAX) = {0};
   unsigned buf_strides[PANVK_VARY_BUF_MAX] = {0};
   unsigned buf_offsets[VARYING_SLOT_MAX] = {0};
   struct varyings_info out_vars = {0};
   struct varyings_info in_vars = {0};
   unsigned loc;

   collect_varyings_info(stage->info.varyings.output,
                         stage->info.varyings.output_count, &out_vars);
   collect_varyings_info(next_stage->info.varyings.input,
                         next_stage->info.varyings.input_count, &in_vars);

   BITSET_OR(active_attrs, in_vars.active, out_vars.active);

   /* Handle the position and point size buffers explicitly, as they are
    * passed through separate buffer pointers to the tiler job.
    */
   if (next_stage->info.stage == MESA_SHADER_FRAGMENT) {
      if (BITSET_TEST(out_vars.active, VARYING_SLOT_POS)) {
         buf_strides[PANVK_VARY_BUF_POSITION] = sizeof(float) * 4;
         BITSET_CLEAR(active_attrs, VARYING_SLOT_POS);
      }

      if (BITSET_TEST(out_vars.active, VARYING_SLOT_PSIZ)) {
         buf_strides[PANVK_VARY_BUF_PSIZ] = sizeof(uint16_t);
         BITSET_CLEAR(active_attrs, VARYING_SLOT_PSIZ);
      }
   }

   BITSET_FOREACH_SET(loc, active_attrs, VARYING_SLOT_MAX) {
      /* We expect stage to write to all inputs read by next_stage, and
       * next_stage to read all inputs written by stage. If that's not the
       * case, we keep PIPE_FORMAT_NONE to reflect the fact we should use a
       * sink attribute (writes are discarded, reads return zeros).
       */
      if (in_vars.fmts[loc] == PIPE_FORMAT_NONE ||
          out_vars.fmts[loc] == PIPE_FORMAT_NONE) {
         in_vars.fmts[loc] = PIPE_FORMAT_NONE;
         out_vars.fmts[loc] = PIPE_FORMAT_NONE;
         continue;
      }

      unsigned out_size = util_format_get_blocksize(out_vars.fmts[loc]);
      unsigned buf_idx = varying_buf_id(loc);

      /* Always trust the 'next_stage' input format, so we can:
       * - discard components that are never read
       * - use float types for interpolated fragment shader inputs
       * - use fp16 for floats with mediump
       * - make sure components that are not written by 'stage' are set to zero
       */
      out_vars.fmts[loc] = in_vars.fmts[loc];

      /* Special buffers are handled explicitly before this loop, everything
       * else should be laid out in the general varying buffer.
       */
      assert(buf_idx == PANVK_VARY_BUF_GENERAL);

      /* Keep things aligned a 32-bit component. */
      buf_offsets[loc] = buf_strides[buf_idx];
      buf_strides[buf_idx] += ALIGN_POT(out_size, 4);
   }

   stage->varyings.attribs = emit_varying_attrs(
      &pipeline->base.desc_pool.base, stage->info.varyings.output,
      stage->info.varyings.output_count, &out_vars, buf_offsets);
   next_stage->varyings.attribs = emit_varying_attrs(
      &pipeline->base.desc_pool.base, next_stage->info.varyings.input,
      next_stage->info.varyings.input_count, &in_vars, buf_offsets);
   memcpy(stage->varyings.buf_strides, buf_strides,
          sizeof(stage->varyings.buf_strides));
   memcpy(next_stage->varyings.buf_strides, buf_strides,
          sizeof(next_stage->varyings.buf_strides));
}

static VkResult
panvk_graphics_pipeline_create(struct panvk_device *dev,
                               struct vk_pipeline_cache *cache,
                               const VkGraphicsPipelineCreateInfo *create_info,
                               const VkAllocationCallbacks *alloc,
                               struct panvk_pipeline **out)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, layout, create_info->layout);
   struct vk_graphics_pipeline_all_state all;
   struct vk_graphics_pipeline_state state = {};
   VkResult result;

   result = vk_graphics_pipeline_state_fill(&dev->vk, &state, create_info, NULL,
                                            0, &all, NULL, 0, NULL);
   if (result)
      return result;

   struct panvk_graphics_pipeline *gfx_pipeline = vk_object_zalloc(
      &dev->vk, alloc, sizeof(*gfx_pipeline), VK_OBJECT_TYPE_PIPELINE);

   if (!gfx_pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out = &gfx_pipeline->base;
   gfx_pipeline->base.layout = layout;
   gfx_pipeline->base.type = PANVK_PIPELINE_GRAPHICS;
   gfx_pipeline->state.dynamic.vi = &gfx_pipeline->state.vi;
   gfx_pipeline->state.dynamic.ms.sample_locations = &gfx_pipeline->state.sl;
   vk_dynamic_graphics_state_fill(&gfx_pipeline->state.dynamic, &state);
   gfx_pipeline->state.rp = *state.rp;

   panvk_pool_init(&gfx_pipeline->base.bin_pool, dev, NULL,
                   PAN_KMOD_BO_FLAG_EXECUTABLE, 4096,
                   "Pipeline shader binaries", false);
   panvk_pool_init(&gfx_pipeline->base.desc_pool, dev, NULL, 0, 4096,
                   "Pipeline static state", false);

   /* Make sure the stage info is correct even if no stage info is provided for
    * this stage in pStages.
    */
   gfx_pipeline->vs.info.stage = MESA_SHADER_VERTEX;
   gfx_pipeline->fs.info.stage = MESA_SHADER_FRAGMENT;

   for (uint32_t i = 0; i < create_info->stageCount; i++) {
      struct panvk_pipeline_shader *pshader = NULL;
      switch (create_info->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
         pshader = &gfx_pipeline->vs;
         break;

      case VK_SHADER_STAGE_FRAGMENT_BIT:
         pshader = &gfx_pipeline->fs;
         break;

      default:
         assert(!"Unsupported graphics pipeline stage");
      }

      VkResult result = init_pipeline_shader(
         &gfx_pipeline->base, &create_info->pStages[i], alloc, pshader);
      if (result != VK_SUCCESS)
         return result;
   }

   link_shaders(gfx_pipeline, &gfx_pipeline->vs, &gfx_pipeline->fs);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(CreateGraphicsPipelines)(
   VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
   const VkGraphicsPipelineCreateInfo *pCreateInfos,
   const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(panvk_device, dev, device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct panvk_pipeline *pipeline;
      VkResult result = panvk_graphics_pipeline_create(
         dev, cache, &pCreateInfos[i], pAllocator, &pipeline);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            panvk_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = panvk_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

static VkResult
panvk_compute_pipeline_create(struct panvk_device *dev,
                              struct vk_pipeline_cache *cache,
                              const VkComputePipelineCreateInfo *create_info,
                              const VkAllocationCallbacks *alloc,
                              struct panvk_pipeline **out)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, layout, create_info->layout);
   struct panvk_compute_pipeline *compute_pipeline = vk_object_zalloc(
      &dev->vk, alloc, sizeof(*compute_pipeline), VK_OBJECT_TYPE_PIPELINE);

   if (!compute_pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out = &compute_pipeline->base;
   compute_pipeline->base.layout = layout;
   compute_pipeline->base.type = PANVK_PIPELINE_COMPUTE;

   panvk_pool_init(&compute_pipeline->base.bin_pool, dev, NULL,
                   PAN_KMOD_BO_FLAG_EXECUTABLE, 4096,
                   "Pipeline shader binaries", false);
   panvk_pool_init(&compute_pipeline->base.desc_pool, dev, NULL, 0, 4096,
                   "Pipeline static state", false);

   VkResult result =
      init_pipeline_shader(&compute_pipeline->base, &create_info->stage, alloc,
                           &compute_pipeline->cs);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(CreateComputePipelines)(
   VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
   const VkComputePipelineCreateInfo *pCreateInfos,
   const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(panvk_device, dev, device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct panvk_pipeline *pipeline;
      VkResult result = panvk_compute_pipeline_create(
         dev, cache, &pCreateInfos[i], pAllocator, &pipeline);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            panvk_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = panvk_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(DestroyPipeline)(VkDevice _device, VkPipeline _pipeline,
                                const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_pipeline, pipeline, _pipeline);

   panvk_pool_cleanup(&pipeline->bin_pool);
   panvk_pool_cleanup(&pipeline->desc_pool);
   vk_object_free(&device->vk, pAllocator, pipeline);
}
