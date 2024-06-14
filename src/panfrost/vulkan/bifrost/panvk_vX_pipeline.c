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
#include "vk_pipeline_layout.h"
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

   pshader->base = shader;
   pshader->info = shader->info;

   if (stage_info->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      struct panvk_compute_pipeline *compute_pipeline =
         panvk_pipeline_to_compute_pipeline(pipeline);

      compute_pipeline->local_size = shader->local_size;
   }

   return VK_SUCCESS;
}

static void
cleanup_pipeline_shader(struct panvk_pipeline *pipeline,
                        struct panvk_pipeline_shader *pshader,
                        const VkAllocationCallbacks *alloc)
{
   struct panvk_device *dev = to_panvk_device(pipeline->base.device);

   if (pshader->base != NULL)
      panvk_per_arch(shader_destroy)(dev, pshader->base, alloc);
}

static VkResult
panvk_graphics_pipeline_create(struct panvk_device *dev,
                               struct vk_pipeline_cache *cache,
                               const VkGraphicsPipelineCreateInfo *create_info,
                               const VkAllocationCallbacks *alloc,
                               struct panvk_pipeline **out)
{
   VK_FROM_HANDLE(vk_pipeline_layout, layout, create_info->layout);
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

   panvk_per_arch(link_shaders)(&dev->mempools.rw, gfx_pipeline->vs.base,
                                gfx_pipeline->fs.base, &gfx_pipeline->link);

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
   VK_FROM_HANDLE(vk_pipeline_layout, layout, create_info->layout);
   struct panvk_compute_pipeline *compute_pipeline = vk_object_zalloc(
      &dev->vk, alloc, sizeof(*compute_pipeline), VK_OBJECT_TYPE_PIPELINE);

   if (!compute_pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out = &compute_pipeline->base;
   compute_pipeline->base.layout = layout;
   compute_pipeline->base.type = PANVK_PIPELINE_COMPUTE;

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

   if (pipeline->type == PANVK_PIPELINE_GRAPHICS) {
      struct panvk_graphics_pipeline *gfx_pipeline =
         panvk_pipeline_to_graphics_pipeline(pipeline);

      panvk_shader_link_cleanup(&device->mempools.rw, &gfx_pipeline->link);
      cleanup_pipeline_shader(pipeline, &gfx_pipeline->vs, pAllocator);
      cleanup_pipeline_shader(pipeline, &gfx_pipeline->fs, pAllocator);
   } else {
      struct panvk_compute_pipeline *compute_pipeline =
         panvk_pipeline_to_compute_pipeline(pipeline);

      cleanup_pipeline_shader(pipeline, &compute_pipeline->cs, pAllocator);
   }

   vk_object_free(&device->vk, pAllocator, pipeline);
}
