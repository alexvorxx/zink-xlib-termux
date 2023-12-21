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

#include "panvk_cmd_buffer.h"
#include "panvk_buffer.h"
#include "panvk_cmd_pool.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_physical_device.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"

#include "pan_encoder.h"
#include "pan_props.h"

#include "util/rounding.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_framebuffer.h"

VKAPI_ATTR void VKAPI_CALL
panvk_CmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                           uint32_t bindingCount, const VkBuffer *pBuffers,
                           const VkDeviceSize *pOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_descriptor_state *desc_state =
      panvk_cmd_get_desc_state(cmdbuf, GRAPHICS);

   assert(firstBinding + bindingCount <= MAX_VBS);

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(panvk_buffer, buffer, pBuffers[i]);

      cmdbuf->state.vb.bufs[firstBinding + i].address =
         panvk_buffer_gpu_ptr(buffer, pOffsets[i]);
      cmdbuf->state.vb.bufs[firstBinding + i].size =
         panvk_buffer_range(buffer, pOffsets[i], VK_WHOLE_SIZE);
   }

   cmdbuf->state.vb.count =
      MAX2(cmdbuf->state.vb.count, firstBinding + bindingCount);
   desc_state->vs_attrib_bufs = desc_state->vs_attribs = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                         VkDeviceSize offset, VkIndexType indexType)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_buffer, buf, buffer);

   cmdbuf->state.ib.buffer = buf;
   cmdbuf->state.ib.offset = offset;
   switch (indexType) {
   case VK_INDEX_TYPE_UINT16:
      cmdbuf->state.ib.index_size = 16;
      break;
   case VK_INDEX_TYPE_UINT32:
      cmdbuf->state.ib.index_size = 32;
      break;
   case VK_INDEX_TYPE_NONE_KHR:
      cmdbuf->state.ib.index_size = 0;
      break;
   case VK_INDEX_TYPE_UINT8_EXT:
      cmdbuf->state.ib.index_size = 8;
      break;
   default:
      unreachable("Invalid index type\n");
   }
}

static void
panvk_set_dyn_ssbo_pointers(struct panvk_descriptor_state *desc_state,
                            unsigned dyn_ssbo_offset,
                            struct panvk_descriptor_set *set)
{
   struct panvk_sysvals *sysvals = &desc_state->sysvals;

   for (unsigned i = 0; i < set->layout->num_dyn_ssbos; i++) {
      const struct panvk_buffer_desc *ssbo =
         &desc_state->dyn.ssbos[dyn_ssbo_offset + i];

      sysvals->dyn_ssbos[dyn_ssbo_offset + i] = (struct panvk_ssbo_addr){
         .base_addr = panvk_buffer_gpu_ptr(ssbo->buffer, ssbo->offset),
         .size = panvk_buffer_range(ssbo->buffer, ssbo->offset, ssbo->size),
      };
   }

   desc_state->sysvals_ptr = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                            VkPipelineBindPoint pipelineBindPoint,
                            VkPipelineLayout layout, uint32_t firstSet,
                            uint32_t descriptorSetCount,
                            const VkDescriptorSet *pDescriptorSets,
                            uint32_t dynamicOffsetCount,
                            const uint32_t *pDynamicOffsets)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);

   struct panvk_descriptor_state *descriptors_state =
      &cmdbuf->bind_points[pipelineBindPoint].desc_state;

   unsigned dynoffset_idx = 0;
   for (unsigned i = 0; i < descriptorSetCount; ++i) {
      unsigned idx = i + firstSet;
      VK_FROM_HANDLE(panvk_descriptor_set, set, pDescriptorSets[i]);

      descriptors_state->sets[idx] = set;

      if (set->layout->num_dyn_ssbos || set->layout->num_dyn_ubos) {
         unsigned dyn_ubo_offset = playout->sets[idx].dyn_ubo_offset;
         unsigned dyn_ssbo_offset = playout->sets[idx].dyn_ssbo_offset;

         for (unsigned b = 0; b < set->layout->binding_count; b++) {
            for (unsigned e = 0; e < set->layout->bindings[b].array_size; e++) {
               struct panvk_buffer_desc *bdesc = NULL;

               if (set->layout->bindings[b].type ==
                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                  bdesc = &descriptors_state->dyn.ubos[dyn_ubo_offset++];
                  *bdesc =
                     set->dyn_ubos[set->layout->bindings[b].dyn_ubo_idx + e];
               } else if (set->layout->bindings[b].type ==
                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                  bdesc = &descriptors_state->dyn.ssbos[dyn_ssbo_offset++];
                  *bdesc =
                     set->dyn_ssbos[set->layout->bindings[b].dyn_ssbo_idx + e];
               }

               if (bdesc) {
                  bdesc->offset += pDynamicOffsets[dynoffset_idx++];
               }
            }
         }
      }

      if (set->layout->num_dyn_ssbos) {
         panvk_set_dyn_ssbo_pointers(descriptors_state,
                                     playout->sets[idx].dyn_ssbo_offset, set);
      }

      if (set->layout->num_dyn_ssbos)
         descriptors_state->dirty |= PANVK_DYNAMIC_SSBO;

      if (set->layout->num_ubos || set->layout->num_dyn_ubos ||
          set->layout->num_dyn_ssbos || set->layout->desc_ubo_size)
         descriptors_state->ubos = 0;

      if (set->layout->num_textures)
         descriptors_state->textures = 0;

      if (set->layout->num_samplers)
         descriptors_state->samplers = 0;

      if (set->layout->num_imgs) {
         descriptors_state->vs_attrib_bufs =
            descriptors_state->non_vs_attrib_bufs = 0;
         descriptors_state->vs_attribs = descriptors_state->non_vs_attribs = 0;
      }
   }

   assert(dynoffset_idx == dynamicOffsetCount);
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                       VkShaderStageFlags stageFlags, uint32_t offset,
                       uint32_t size, const void *pValues)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   memcpy(cmdbuf->push_constants + offset, pValues, size);

   if (stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS) {
      struct panvk_descriptor_state *desc_state =
         panvk_cmd_get_desc_state(cmdbuf, GRAPHICS);

      desc_state->ubos = 0;
      desc_state->push_constants = 0;
   }

   if (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct panvk_descriptor_state *desc_state =
         panvk_cmd_get_desc_state(cmdbuf, COMPUTE);

      desc_state->ubos = 0;
      desc_state->push_constants = 0;
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdBindPipeline(VkCommandBuffer commandBuffer,
                      VkPipelineBindPoint pipelineBindPoint,
                      VkPipeline _pipeline)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_pipeline, pipeline, _pipeline);

   cmdbuf->bind_points[pipelineBindPoint].pipeline = pipeline;
   cmdbuf->state.fs_rsd = 0;

   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      cmdbuf->state.varyings = pipeline->varyings;

      if (!(pipeline->dynamic_state_mask &
            BITFIELD_BIT(VK_DYNAMIC_STATE_VIEWPORT))) {
         cmdbuf->state.viewport = pipeline->viewport;
         cmdbuf->state.dirty |= PANVK_DYNAMIC_VIEWPORT;
      }
      if (!(pipeline->dynamic_state_mask &
            BITFIELD_BIT(VK_DYNAMIC_STATE_SCISSOR))) {
         cmdbuf->state.scissor = pipeline->scissor;
         cmdbuf->state.dirty |= PANVK_DYNAMIC_SCISSOR;
      }
   }

   /* Sysvals are passed through UBOs, we need dirty the UBO array if the
    * pipeline contain shaders using sysvals.
    */
   cmdbuf->bind_points[pipelineBindPoint].desc_state.ubos = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport,
                     uint32_t viewportCount, const VkViewport *pViewports)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   assert(viewportCount == 1);
   assert(!firstViewport);

   cmdbuf->state.viewport = pViewports[0];
   cmdbuf->state.vpd = 0;
   cmdbuf->state.dirty |= PANVK_DYNAMIC_VIEWPORT;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor,
                    uint32_t scissorCount, const VkRect2D *pScissors)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   assert(scissorCount == 1);
   assert(!firstScissor);

   cmdbuf->state.scissor = pScissors[0];
   cmdbuf->state.vpd = 0;
   cmdbuf->state.dirty |= PANVK_DYNAMIC_SCISSOR;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   cmdbuf->state.rast.line_width = lineWidth;
   cmdbuf->state.dirty |= PANVK_DYNAMIC_LINE_WIDTH;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetDepthBias(VkCommandBuffer commandBuffer,
                      float depthBiasConstantFactor, float depthBiasClamp,
                      float depthBiasSlopeFactor)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   cmdbuf->state.rast.depth_bias.constant_factor = depthBiasConstantFactor;
   cmdbuf->state.rast.depth_bias.clamp = depthBiasClamp;
   cmdbuf->state.rast.depth_bias.slope_factor = depthBiasSlopeFactor;
   cmdbuf->state.dirty |= PANVK_DYNAMIC_DEPTH_BIAS;
   cmdbuf->state.fs_rsd = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                           const float blendConstants[4])
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   for (unsigned i = 0; i < 4; i++)
      cmdbuf->state.blend.constants[i] = CLAMP(blendConstants[i], 0.0f, 1.0f);

   cmdbuf->state.dirty |= PANVK_DYNAMIC_BLEND_CONSTANTS;
   cmdbuf->state.fs_rsd = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds,
                        float maxDepthBounds)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                               VkStencilFaceFlags faceMask,
                               uint32_t compareMask)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmdbuf->state.zs.s_front.compare_mask = compareMask;

   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmdbuf->state.zs.s_back.compare_mask = compareMask;

   cmdbuf->state.dirty |= PANVK_DYNAMIC_STENCIL_COMPARE_MASK;
   cmdbuf->state.fs_rsd = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                             VkStencilFaceFlags faceMask, uint32_t writeMask)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmdbuf->state.zs.s_front.write_mask = writeMask;

   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmdbuf->state.zs.s_back.write_mask = writeMask;

   cmdbuf->state.dirty |= PANVK_DYNAMIC_STENCIL_WRITE_MASK;
   cmdbuf->state.fs_rsd = 0;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                             VkStencilFaceFlags faceMask, uint32_t reference)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      cmdbuf->state.zs.s_front.ref = reference;

   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      cmdbuf->state.zs.s_back.ref = reference;

   cmdbuf->state.dirty |= PANVK_DYNAMIC_STENCIL_REFERENCE;
   cmdbuf->state.fs_rsd = 0;
}

void
panvk_cmd_preload_fb_after_batch_split(struct panvk_cmd_buffer *cmdbuf)
{
   for (unsigned i = 0; i < cmdbuf->state.fb.info.rt_count; i++) {
      if (cmdbuf->state.fb.info.rts[i].view) {
         cmdbuf->state.fb.info.rts[i].clear = false;
         cmdbuf->state.fb.info.rts[i].preload = true;
      }
   }

   if (cmdbuf->state.fb.info.zs.view.zs) {
      cmdbuf->state.fb.info.zs.clear.z = false;
      cmdbuf->state.fb.info.zs.preload.z = true;
   }

   if (cmdbuf->state.fb.info.zs.view.s ||
       (cmdbuf->state.fb.info.zs.view.zs &&
        util_format_is_depth_and_stencil(
           cmdbuf->state.fb.info.zs.view.zs->format))) {
      cmdbuf->state.fb.info.zs.clear.s = false;
      cmdbuf->state.fb.info.zs.preload.s = true;
   }
}

struct panvk_batch *
panvk_cmd_open_batch(struct panvk_cmd_buffer *cmdbuf)
{
   assert(!cmdbuf->state.batch);
   cmdbuf->state.batch =
      vk_zalloc(&cmdbuf->vk.pool->alloc, sizeof(*cmdbuf->state.batch), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   util_dynarray_init(&cmdbuf->state.batch->jobs, NULL);
   util_dynarray_init(&cmdbuf->state.batch->event_ops, NULL);
   assert(cmdbuf->state.batch);
   return cmdbuf->state.batch;
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                      VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                             VkDeviceSize offset, uint32_t drawCount,
                             uint32_t stride)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t base_x,
                      uint32_t base_y, uint32_t base_z, uint32_t x, uint32_t y,
                      uint32_t z)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_CmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                          VkDeviceSize offset)
{
   panvk_stub();
}
