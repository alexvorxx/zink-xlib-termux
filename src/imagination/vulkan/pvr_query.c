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
 *
 * 'pvr_write_query_to_buffer()' based on anv:
 * Copyright © 2015 Intel Corporation
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "pvr_bo.h"
#include "pvr_csb.h"
#include "pvr_device_info.h"
#include "pvr_private.h"
#include "util/macros.h"
#include "vk_log.h"
#include "vk_object.h"

VkResult pvr_CreateQueryPool(VkDevice _device,
                             const VkQueryPoolCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkQueryPool *pQueryPool)
{
   PVR_FROM_HANDLE(pvr_device, device, _device);
   const uint32_t core_count = device->pdevice->dev_runtime_info.core_count;
   const uint32_t query_size = pCreateInfo->queryCount * sizeof(uint32_t);
   struct pvr_query_pool *pool;
   uint64_t alloc_size;
   VkResult result;

   /* Vulkan 1.0 supports only occlusion, timestamp, and pipeline statistics
    * query.
    * We don't currently support timestamp queries.
    * VkQueueFamilyProperties->timestampValidBits = 0.
    * We don't currently support pipeline statistics queries.
    * VkPhysicalDeviceFeatures->pipelineStatisticsQuery = false.
    */
   assert(!device->features.pipelineStatisticsQuery);
   assert(pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION);

   pool = vk_object_alloc(&device->vk,
                          pAllocator,
                          sizeof(*pool),
                          VK_OBJECT_TYPE_QUERY_POOL);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->result_stride =
      ALIGN_POT(query_size, PVRX(CR_ISP_OCLQRY_BASE_ADDR_ALIGNMENT));

   pool->query_count = pCreateInfo->queryCount;

   /* Each Phantom writes to a separate offset within the vis test heap so
    * allocate space for the total number of Phantoms.
    */
   alloc_size = (uint64_t)pool->result_stride * core_count;

   result = pvr_bo_alloc(device,
                         device->heaps.vis_test_heap,
                         alloc_size,
                         PVRX(CR_ISP_OCLQRY_BASE_ADDR_ALIGNMENT),
                         PVR_BO_ALLOC_FLAG_CPU_MAPPED,
                         &pool->result_buffer);
   if (result != VK_SUCCESS)
      goto err_free_pool;

   result = pvr_bo_alloc(device,
                         device->heaps.general_heap,
                         query_size,
                         sizeof(uint32_t),
                         PVR_BO_ALLOC_FLAG_CPU_MAPPED,
                         &pool->availability_buffer);
   if (result != VK_SUCCESS)
      goto err_free_result_buffer;

   *pQueryPool = pvr_query_pool_to_handle(pool);

   return VK_SUCCESS;

err_free_result_buffer:
   pvr_bo_free(device, pool->result_buffer);

err_free_pool:
   vk_object_free(&device->vk, pAllocator, pool);

   return result;
}

void pvr_DestroyQueryPool(VkDevice _device,
                          VkQueryPool queryPool,
                          const VkAllocationCallbacks *pAllocator)
{
   PVR_FROM_HANDLE(pvr_query_pool, pool, queryPool);
   PVR_FROM_HANDLE(pvr_device, device, _device);

   pvr_bo_free(device, pool->availability_buffer);
   pvr_bo_free(device, pool->result_buffer);

   vk_object_free(&device->vk, pAllocator, pool);
}

static inline void pvr_write_query_to_buffer(uint8_t *buffer,
                                             VkQueryResultFlags flags,
                                             uint32_t idx,
                                             uint64_t value)
{
   if (flags & VK_QUERY_RESULT_64_BIT) {
      uint64_t *query_data = (uint64_t *)buffer;
      query_data[idx] = value;
   } else {
      uint32_t *query_data = (uint32_t *)buffer;
      query_data[idx] = value;
   }
}

VkResult pvr_GetQueryPoolResults(VkDevice _device,
                                 VkQueryPool queryPool,
                                 uint32_t firstQuery,
                                 uint32_t queryCount,
                                 size_t dataSize,
                                 void *pData,
                                 VkDeviceSize stride,
                                 VkQueryResultFlags flags)
{
   PVR_FROM_HANDLE(pvr_query_pool, pool, queryPool);
   PVR_FROM_HANDLE(pvr_device, device, _device);
   const uint32_t core_count = device->pdevice->dev_runtime_info.core_count;
   volatile uint32_t *available = pool->availability_buffer->bo->map;
   volatile uint32_t *query_results = pool->result_buffer->bo->map;
   uint8_t *data = (uint8_t *)pData;
   VkResult result = VK_SUCCESS;

   VG(VALGRIND_MAKE_MEM_DEFINED(&available[firstQuery],
                                queryCount * sizeof(uint32_t)));

   for (uint32_t i = 0; i < core_count; i++) {
      VG(VALGRIND_MAKE_MEM_DEFINED(
         &query_results[firstQuery + i * pool->result_stride],
         queryCount * sizeof(uint32_t)));
   }

   for (uint32_t i = 0; i < queryCount; i++) {
      const bool is_available = !!available[firstQuery + i];
      uint64_t count = 0;
      uint32_t idx = 0;

      /* From the Vulkan 1.0 spec:
       *
       *    Commands that wait indefinitely for device execution (namely
       *    vkDeviceWaitIdle, vkQueueWaitIdle, vkWaitForFences or
       *    vkAcquireNextImageKHR with a maximum timeout, and
       *    vkGetQueryPoolResults with the VK_QUERY_RESULT_WAIT_BIT bit set in
       *    flags) must return in finite time even in the case of a lost device,
       *    and return either VK_SUCCESS or VK_ERROR_DEVICE_LOST.
       */
      if (flags & VK_QUERY_RESULT_WAIT_BIT) {
         /* Add support to wait for query results to be available. Also handle
          * device loss scenario.
          */
         pvr_finishme("Unimplemented path.");
      }

      for (uint32_t j = 0; j < core_count; j++)
         count += query_results[pool->result_stride * j + firstQuery + i];

      if (is_available || (flags & VK_QUERY_RESULT_PARTIAL_BIT))
         pvr_write_query_to_buffer(data, flags, idx++, count);
      else
         result = VK_NOT_READY;

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
         pvr_write_query_to_buffer(data, flags, idx++, count);

      data += stride;
   }

   VG(VALGRIND_MAKE_MEM_UNDEFINED(&available[firstQuery],
                                  queryCount * sizeof(uint32_t)));

   for (uint32_t i = 0; i < core_count; i++) {
      VG(VALGRIND_MAKE_MEM_UNDEFINED(
         &query_results[firstQuery + i * pool->result_stride],
         queryCount * sizeof(uint32_t)));
   }

   return result;
}

void pvr_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                           VkQueryPool queryPool,
                           uint32_t firstQuery,
                           uint32_t queryCount)
{
   PVR_FROM_HANDLE(pvr_cmd_buffer, cmd_buffer, commandBuffer);
   struct pvr_query_info query_info;

   PVR_CHECK_COMMAND_BUFFER_BUILDING_STATE(cmd_buffer);

   query_info.type = PVR_QUERY_TYPE_RESET_QUERY_POOL;

   query_info.reset_query_pool.query_pool = queryPool;
   query_info.reset_query_pool.first_query = firstQuery;
   query_info.reset_query_pool.query_count = queryCount;

   pvr_add_query_program(cmd_buffer, &query_info);
}

void pvr_CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                                 VkQueryPool queryPool,
                                 uint32_t firstQuery,
                                 uint32_t queryCount,
                                 VkBuffer dstBuffer,
                                 VkDeviceSize dstOffset,
                                 VkDeviceSize stride,
                                 VkQueryResultFlags flags)
{
   PVR_FROM_HANDLE(pvr_cmd_buffer, cmd_buffer, commandBuffer);
   struct pvr_query_info query_info;
   struct pvr_sub_cmd_event *sub_cmd;
   VkResult result;

   PVR_CHECK_COMMAND_BUFFER_BUILDING_STATE(cmd_buffer);

   query_info.type = PVR_QUERY_TYPE_COPY_QUERY_RESULTS;

   query_info.copy_query_results.query_pool = queryPool;
   query_info.copy_query_results.first_query = firstQuery;
   query_info.copy_query_results.query_count = queryCount;
   query_info.copy_query_results.dst_buffer = dstBuffer;
   query_info.copy_query_results.dst_offset = dstOffset;
   query_info.copy_query_results.stride = stride;
   query_info.copy_query_results.flags = flags;

   result = pvr_cmd_buffer_start_sub_cmd(cmd_buffer, PVR_SUB_CMD_TYPE_EVENT);
   if (result != VK_SUCCESS)
      return;

   /* The Vulkan 1.3.231 spec says:
    *
    *    "vkCmdCopyQueryPoolResults is considered to be a transfer operation,
    *    and its writes to buffer memory must be synchronized using
    *    VK_PIPELINE_STAGE_TRANSFER_BIT and VK_ACCESS_TRANSFER_WRITE_BIT before
    *    using the results."
    *
    */
   /* We record barrier event sub commands to sync the compute job used for the
    * copy query results program with transfer jobs to prevent an overlapping
    * transfer job with the compute job.
    */

   sub_cmd = &cmd_buffer->state.current_sub_cmd->event;
   *sub_cmd = (struct pvr_sub_cmd_event) {
      .type = PVR_EVENT_TYPE_BARRIER,
      .barrier = {
         .wait_for_stage_mask = PVR_PIPELINE_STAGE_TRANSFER_BIT,
         .wait_at_stage_mask = PVR_PIPELINE_STAGE_OCCLUSION_QUERY_BIT,
      },
   };

   result = pvr_cmd_buffer_end_sub_cmd(cmd_buffer);
   if (result != VK_SUCCESS)
      return;

   pvr_add_query_program(cmd_buffer, &query_info);

   result = pvr_cmd_buffer_start_sub_cmd(cmd_buffer, PVR_SUB_CMD_TYPE_EVENT);
   if (result != VK_SUCCESS)
      return;

   sub_cmd = &cmd_buffer->state.current_sub_cmd->event;
   *sub_cmd = (struct pvr_sub_cmd_event) {
      .type = PVR_EVENT_TYPE_BARRIER,
      .barrier = {
         .wait_for_stage_mask = PVR_PIPELINE_STAGE_OCCLUSION_QUERY_BIT,
         .wait_at_stage_mask = PVR_PIPELINE_STAGE_TRANSFER_BIT,
      },
   };
}

void pvr_CmdBeginQuery(VkCommandBuffer commandBuffer,
                       VkQueryPool queryPool,
                       uint32_t query,
                       VkQueryControlFlags flags)
{
   PVR_FROM_HANDLE(pvr_cmd_buffer, cmd_buffer, commandBuffer);
   struct pvr_cmd_buffer_state *state = &cmd_buffer->state;
   PVR_FROM_HANDLE(pvr_query_pool, pool, queryPool);

   PVR_CHECK_COMMAND_BUFFER_BUILDING_STATE(cmd_buffer);

   /* Occlusion queries can't be nested. */
   assert(!state->vis_test_enabled);

   if (state->current_sub_cmd) {
      assert(state->current_sub_cmd->type == PVR_SUB_CMD_TYPE_GRAPHICS);

      if (!state->current_sub_cmd->gfx.query_pool) {
         state->current_sub_cmd->gfx.query_pool = pool;
      } else if (state->current_sub_cmd->gfx.query_pool != pool) {
         VkResult result;

         /* Kick render. */
         state->current_sub_cmd->gfx.barrier_store = true;

         result = pvr_cmd_buffer_end_sub_cmd(cmd_buffer);
         if (result != VK_SUCCESS)
            return;

         result =
            pvr_cmd_buffer_start_sub_cmd(cmd_buffer, PVR_SUB_CMD_TYPE_GRAPHICS);
         if (result != VK_SUCCESS)
            return;

         /* Use existing render setup, but load color attachments from HW
          * BGOBJ.
          */
         state->current_sub_cmd->gfx.barrier_load = true;
         state->current_sub_cmd->gfx.barrier_store = false;
         state->current_sub_cmd->gfx.query_pool = pool;
      }
   }

   state->query_pool = pool;
   state->vis_test_enabled = true;
   state->vis_reg = query;
   state->dirty.vis_test = true;

   /* Add the index to the list for this render. */
   util_dynarray_append(&state->query_indices, __typeof__(query), query);
}

void pvr_CmdEndQuery(VkCommandBuffer commandBuffer,
                     VkQueryPool queryPool,
                     uint32_t query)
{
   PVR_FROM_HANDLE(pvr_cmd_buffer, cmd_buffer, commandBuffer);
   struct pvr_cmd_buffer_state *state = &cmd_buffer->state;

   PVR_CHECK_COMMAND_BUFFER_BUILDING_STATE(cmd_buffer);

   state->vis_test_enabled = false;
   state->dirty.vis_test = true;
}
