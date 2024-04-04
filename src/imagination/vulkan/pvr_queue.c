/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * based in part on radv driver which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

/**
 * This file implements VkQueue, VkFence, and VkSemaphore
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "pvr_job_compute.h"
#include "pvr_job_context.h"
#include "pvr_job_render.h"
#include "pvr_job_transfer.h"
#include "pvr_limits.h"
#include "pvr_private.h"
#include "util/macros.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"
#include "vk_fence.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_queue.h"
#include "vk_semaphore.h"
#include "vk_sync.h"
#include "vk_sync_dummy.h"
#include "vk_util.h"

static VkResult pvr_queue_init(struct pvr_device *device,
                               struct pvr_queue *queue,
                               const VkDeviceQueueCreateInfo *pCreateInfo,
                               uint32_t index_in_family)
{
   struct pvr_transfer_ctx *transfer_ctx;
   struct pvr_compute_ctx *compute_ctx;
   struct pvr_compute_ctx *query_ctx;
   struct pvr_render_ctx *gfx_ctx;
   VkResult result;

   *queue = (struct pvr_queue){ 0 };

   result =
      vk_queue_init(&queue->vk, &device->vk, pCreateInfo, index_in_family);
   if (result != VK_SUCCESS)
      return result;

   result = pvr_transfer_ctx_create(device,
                                    PVR_WINSYS_CTX_PRIORITY_MEDIUM,
                                    &transfer_ctx);
   if (result != VK_SUCCESS)
      goto err_vk_queue_finish;

   result = pvr_compute_ctx_create(device,
                                   PVR_WINSYS_CTX_PRIORITY_MEDIUM,
                                   &compute_ctx);
   if (result != VK_SUCCESS)
      goto err_transfer_ctx_destroy;

   result = pvr_compute_ctx_create(device,
                                   PVR_WINSYS_CTX_PRIORITY_MEDIUM,
                                   &query_ctx);
   if (result != VK_SUCCESS)
      goto err_compute_ctx_destroy;

   result =
      pvr_render_ctx_create(device, PVR_WINSYS_CTX_PRIORITY_MEDIUM, &gfx_ctx);
   if (result != VK_SUCCESS)
      goto err_query_ctx_destroy;

   queue->device = device;
   queue->gfx_ctx = gfx_ctx;
   queue->compute_ctx = compute_ctx;
   queue->query_ctx = query_ctx;
   queue->transfer_ctx = transfer_ctx;

   return VK_SUCCESS;

err_query_ctx_destroy:
   pvr_compute_ctx_destroy(query_ctx);

err_compute_ctx_destroy:
   pvr_compute_ctx_destroy(compute_ctx);

err_transfer_ctx_destroy:
   pvr_transfer_ctx_destroy(transfer_ctx);

err_vk_queue_finish:
   vk_queue_finish(&queue->vk);

   return result;
}

VkResult pvr_queues_create(struct pvr_device *device,
                           const VkDeviceCreateInfo *pCreateInfo)
{
   VkResult result;

   /* Check requested queue families and queues */
   assert(pCreateInfo->queueCreateInfoCount == 1);
   assert(pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex == 0);
   assert(pCreateInfo->pQueueCreateInfos[0].queueCount <= PVR_MAX_QUEUES);

   const VkDeviceQueueCreateInfo *queue_create =
      &pCreateInfo->pQueueCreateInfos[0];

   device->queues = vk_alloc(&device->vk.alloc,
                             queue_create->queueCount * sizeof(*device->queues),
                             8,
                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device->queues)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->queue_count = 0;

   for (uint32_t i = 0; i < queue_create->queueCount; i++) {
      result = pvr_queue_init(device, &device->queues[i], queue_create, i);
      if (result != VK_SUCCESS)
         goto err_queues_finish;

      device->queue_count++;
   }

   return VK_SUCCESS;

err_queues_finish:
   pvr_queues_destroy(device);
   return result;
}

static void pvr_queue_finish(struct pvr_queue *queue)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(queue->job_dependancy); i++) {
      if (queue->job_dependancy[i])
         vk_sync_destroy(&queue->device->vk, queue->job_dependancy[i]);
   }

   for (uint32_t i = 0; i < ARRAY_SIZE(queue->completion); i++) {
      if (queue->completion[i])
         vk_sync_destroy(&queue->device->vk, queue->completion[i]);
   }

   pvr_render_ctx_destroy(queue->gfx_ctx);
   pvr_compute_ctx_destroy(queue->query_ctx);
   pvr_compute_ctx_destroy(queue->compute_ctx);
   pvr_transfer_ctx_destroy(queue->transfer_ctx);

   vk_queue_finish(&queue->vk);
}

void pvr_queues_destroy(struct pvr_device *device)
{
   for (uint32_t q_idx = 0; q_idx < device->queue_count; q_idx++)
      pvr_queue_finish(&device->queues[q_idx]);

   vk_free(&device->vk.alloc, device->queues);
}

VkResult pvr_QueueWaitIdle(VkQueue _queue)
{
   PVR_FROM_HANDLE(pvr_queue, queue, _queue);

   for (int i = 0U; i < ARRAY_SIZE(queue->completion); i++) {
      VkResult result;

      if (!queue->completion[i])
         continue;

      result = vk_sync_wait(&queue->device->vk,
                            queue->completion[i],
                            0U,
                            VK_SYNC_WAIT_COMPLETE,
                            UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
pvr_process_graphics_cmd(struct pvr_device *device,
                         struct pvr_queue *queue,
                         struct pvr_cmd_buffer *cmd_buffer,
                         struct pvr_sub_cmd_gfx *sub_cmd,
                         struct vk_sync *barrier_geom,
                         struct vk_sync *barrier_frag,
                         struct vk_sync **waits,
                         uint32_t wait_count,
                         uint32_t *stage_flags,
                         struct vk_sync *completions[static PVR_JOB_TYPE_MAX])
{
   const struct pvr_framebuffer *framebuffer = sub_cmd->framebuffer;
   struct vk_sync *sync_geom;
   struct vk_sync *sync_frag;
   VkResult result;

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync_geom);
   if (result != VK_SUCCESS)
      return result;

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync_frag);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&device->vk, sync_geom);
      return result;
   }

   /* FIXME: DoShadowLoadOrStore() */

   /* FIXME: If the framebuffer being rendered to has multiple layers then we
    * need to split submissions that run a fragment job into two.
    */
   if (sub_cmd->job.run_frag && framebuffer->layers > 1)
      pvr_finishme("Split job submission for framebuffers with > 1 layers");

   result = pvr_render_job_submit(queue->gfx_ctx,
                                  &sub_cmd->job,
                                  barrier_geom,
                                  barrier_frag,
                                  waits,
                                  wait_count,
                                  stage_flags,
                                  sync_geom,
                                  sync_frag);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&device->vk, sync_geom);
      vk_sync_destroy(&device->vk, sync_frag);
      return result;
   }

   /* Replace the completion fences. */
   if (completions[PVR_JOB_TYPE_GEOM])
      vk_sync_destroy(&device->vk, completions[PVR_JOB_TYPE_GEOM]);

   completions[PVR_JOB_TYPE_GEOM] = sync_geom;

   if (completions[PVR_JOB_TYPE_FRAG])
      vk_sync_destroy(&device->vk, completions[PVR_JOB_TYPE_FRAG]);

   completions[PVR_JOB_TYPE_FRAG] = sync_frag;

   /* FIXME: DoShadowLoadOrStore() */

   return result;
}

static VkResult
pvr_process_compute_cmd(struct pvr_device *device,
                        struct pvr_queue *queue,
                        struct pvr_sub_cmd_compute *sub_cmd,
                        struct vk_sync *barrier,
                        struct vk_sync **waits,
                        uint32_t wait_count,
                        uint32_t *stage_flags,
                        struct vk_sync *completions[static PVR_JOB_TYPE_MAX])
{
   struct vk_sync *sync;
   VkResult result;

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync);
   if (result != VK_SUCCESS)
      return result;

   result = pvr_compute_job_submit(queue->compute_ctx,
                                   sub_cmd,
                                   barrier,
                                   waits,
                                   wait_count,
                                   stage_flags,
                                   sync);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&device->vk, sync);
      return result;
   }

   /* Replace the completion fences. */
   if (completions[PVR_JOB_TYPE_COMPUTE])
      vk_sync_destroy(&device->vk, completions[PVR_JOB_TYPE_COMPUTE]);

   completions[PVR_JOB_TYPE_COMPUTE] = sync;

   return result;
}

static VkResult
pvr_process_transfer_cmds(struct pvr_device *device,
                          struct pvr_queue *queue,
                          struct pvr_sub_cmd_transfer *sub_cmd,
                          struct vk_sync *barrier,
                          struct vk_sync **waits,
                          uint32_t wait_count,
                          uint32_t *stage_flags,
                          struct vk_sync *completions[static PVR_JOB_TYPE_MAX])
{
   struct vk_sync *sync;
   VkResult result;

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync);
   if (result != VK_SUCCESS)
      return result;

   result = pvr_transfer_job_submit(device,
                                    queue->transfer_ctx,
                                    sub_cmd,
                                    barrier,
                                    waits,
                                    wait_count,
                                    stage_flags,
                                    sync);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&device->vk, sync);
      return result;
   }

   /* Replace the completion fences. */
   if (completions[PVR_JOB_TYPE_TRANSFER])
      vk_sync_destroy(&device->vk, completions[PVR_JOB_TYPE_TRANSFER]);

   completions[PVR_JOB_TYPE_TRANSFER] = sync;

   return result;
}

static VkResult pvr_process_occlusion_query_cmd(
   struct pvr_device *device,
   struct pvr_queue *queue,
   struct pvr_sub_cmd_compute *sub_cmd,
   struct vk_sync *barrier,
   struct vk_sync **waits,
   uint32_t wait_count,
   uint32_t *stage_flags,
   struct vk_sync *completions[static PVR_JOB_TYPE_MAX])
{
   struct vk_sync *sync;
   VkResult result;

   /* TODO: Currently we add barrier event sub commands to handle the sync
    * necessary for the different occlusion query types. Would we get any speed
    * up in processing the queue by doing that sync here without using event sub
    * commands?
    */

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync);
   if (result != VK_SUCCESS)
      return result;

   result = pvr_compute_job_submit(queue->query_ctx,
                                   sub_cmd,
                                   barrier,
                                   waits,
                                   wait_count,
                                   stage_flags,
                                   sync);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&device->vk, sync);
      return result;
   }

   if (completions[PVR_JOB_TYPE_OCCLUSION_QUERY])
      vk_sync_destroy(&device->vk, completions[PVR_JOB_TYPE_OCCLUSION_QUERY]);

   completions[PVR_JOB_TYPE_OCCLUSION_QUERY] = sync;

   return result;
}

static VkResult pvr_process_event_cmd_barrier(
   struct pvr_device *device,
   struct pvr_sub_cmd_event *sub_cmd,
   struct vk_sync *barriers[static PVR_JOB_TYPE_MAX],
   struct vk_sync *per_cmd_buffer_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *per_submit_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *queue_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *previous_queue_syncobjs[static PVR_JOB_TYPE_MAX])
{
   const uint32_t src_mask = sub_cmd->barrier.wait_for_stage_mask;
   const uint32_t dst_mask = sub_cmd->barrier.wait_at_stage_mask;
   const bool in_render_pass = sub_cmd->barrier.in_render_pass;
   struct vk_sync *new_barriers[PVR_JOB_TYPE_MAX] = { 0 };
   struct vk_sync *completions[PVR_JOB_TYPE_MAX] = { 0 };
   struct vk_sync *src_syncobjs[PVR_JOB_TYPE_MAX];
   uint32_t src_syncobj_count = 0;
   VkResult result;

   assert(sub_cmd->type == PVR_EVENT_TYPE_BARRIER);

   assert(!(src_mask & ~PVR_PIPELINE_STAGE_ALL_BITS));
   assert(!(dst_mask & ~PVR_PIPELINE_STAGE_ALL_BITS));

   /* TODO: We're likely over synchronizing here, but the kernel doesn't
    * guarantee that jobs submitted on a context will execute and complete in
    * order, even though in practice they will, so we play it safe and don't
    * make any assumptions. If the kernel starts to offer this guarantee then
    * remove the extra dependencies being added here.
    */

   u_foreach_bit (stage, src_mask) {
      struct vk_sync *syncobj;

      syncobj = per_cmd_buffer_syncobjs[stage];

      if (!in_render_pass & !syncobj) {
         if (per_submit_syncobjs[stage])
            syncobj = per_submit_syncobjs[stage];
         else if (queue_syncobjs[stage])
            syncobj = queue_syncobjs[stage];
         else if (previous_queue_syncobjs[stage])
            syncobj = previous_queue_syncobjs[stage];
      }

      if (!syncobj)
         continue;

      src_syncobjs[src_syncobj_count++] = syncobj;
   }

   /* No previous src jobs that need finishing so no need for a barrier. */
   if (src_syncobj_count == 0)
      return VK_SUCCESS;

   u_foreach_bit (stage, dst_mask) {
      struct vk_sync *completion;

      result = vk_sync_create(&device->vk,
                              &device->pdevice->ws->syncobj_type,
                              0U,
                              0UL,
                              &completion);
      if (result != VK_SUCCESS)
         goto err_destroy_completions;

      result = device->ws->ops->null_job_submit(device->ws,
                                                src_syncobjs,
                                                src_syncobj_count,
                                                completion);
      if (result != VK_SUCCESS) {
         vk_sync_destroy(&device->vk, completion);

         goto err_destroy_completions;
      }

      completions[stage] = completion;
   }

   u_foreach_bit (stage, dst_mask) {
      struct vk_sync *barrier_src_syncobjs[2];
      uint32_t barrier_src_syncobj_count = 0;
      struct vk_sync *barrier;
      VkResult result;

      assert(completions[stage]);
      barrier_src_syncobjs[barrier_src_syncobj_count++] = completions[stage];

      /* If there is a previous barrier we want to merge it with the new one.
       *
       * E.g.
       *    A <compute>, B <compute>,
       *       X <barrier src=compute, dst=graphics>,
       *    C <transfer>
       *       Y <barrier src=transfer, dst=graphics>,
       *    D <graphics>
       *
       * X barriers A and B at D. Y barriers C at D. So we want to merge both
       * X and Y graphics vk_sync barriers to pass to D.
       *
       * Note that this is the same as:
       *    A <compute>, B <compute>, C <transfer>
       *       X <barrier src=compute, dst=graphics>,
       *       Y <barrier src=transfer, dst=graphics>,
       *    D <graphics>
       *
       */
      if (barriers[stage])
         barrier_src_syncobjs[barrier_src_syncobj_count++] = barriers[stage];

      result = vk_sync_create(&device->vk,
                              &device->pdevice->ws->syncobj_type,
                              0U,
                              0UL,
                              &barrier);
      if (result != VK_SUCCESS)
         goto err_destroy_new_barriers;

      result = device->ws->ops->null_job_submit(device->ws,
                                                barrier_src_syncobjs,
                                                barrier_src_syncobj_count,
                                                barrier);
      if (result != VK_SUCCESS) {
         vk_sync_destroy(&device->vk, barrier);

         goto err_destroy_new_barriers;
      }

      new_barriers[stage] = barrier;
   }

   u_foreach_bit (stage, dst_mask) {
      if (per_cmd_buffer_syncobjs[stage])
         vk_sync_destroy(&device->vk, per_cmd_buffer_syncobjs[stage]);

      per_cmd_buffer_syncobjs[stage] = completions[stage];

      if (barriers[stage])
         vk_sync_destroy(&device->vk, barriers[stage]);

      barriers[stage] = new_barriers[stage];
   }

   return VK_SUCCESS;

err_destroy_new_barriers:
   u_foreach_bit (stage, dst_mask) {
      if (new_barriers[stage])
         vk_sync_destroy(&device->vk, new_barriers[stage]);
   }

err_destroy_completions:
   u_foreach_bit (stage, dst_mask) {
      if (completions[stage])
         vk_sync_destroy(&device->vk, completions[stage]);
   }

   return result;
}

static VkResult pvr_process_event_cmd(
   struct pvr_device *device,
   struct pvr_sub_cmd_event *sub_cmd,
   struct vk_sync *barriers[static PVR_JOB_TYPE_MAX],
   struct vk_sync *per_cmd_buffer_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *per_submit_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *queue_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *previous_queue_syncobjs[static PVR_JOB_TYPE_MAX])
{
   switch (sub_cmd->type) {
   case PVR_EVENT_TYPE_SET:
   case PVR_EVENT_TYPE_RESET:
   case PVR_EVENT_TYPE_WAIT:
      pvr_finishme("Add support for event sub command type: %d", sub_cmd->type);
      return VK_SUCCESS;

   case PVR_EVENT_TYPE_BARRIER:
      return pvr_process_event_cmd_barrier(device,
                                           sub_cmd,
                                           barriers,
                                           per_cmd_buffer_syncobjs,
                                           per_submit_syncobjs,
                                           queue_syncobjs,
                                           previous_queue_syncobjs);

   default:
      unreachable("Invalid event sub-command type.");
   };
}

static VkResult
pvr_set_semaphore_payloads(struct pvr_device *device,
                           struct vk_sync *completions[static PVR_JOB_TYPE_MAX],
                           const VkSemaphore *signals,
                           uint32_t signal_count)
{
   struct vk_sync *sync;
   VkResult result;
   int fd = -1;

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync);
   if (result != VK_SUCCESS)
      return result;

   result = device->ws->ops->null_job_submit(device->ws,
                                             completions,
                                             PVR_JOB_TYPE_MAX,
                                             sync);
   if (result != VK_SUCCESS)
      goto end_set_semaphore_payloads;

   /* If we have a single signal semaphore, we can simply move merged sync's
    * payload to the signal semahpore's payload.
    */
   if (signal_count == 1U) {
      VK_FROM_HANDLE(vk_semaphore, sem, signals[0]);
      struct vk_sync *sem_sync = vk_semaphore_get_active_sync(sem);

      result = vk_sync_move(&device->vk, sem_sync, sync);
      goto end_set_semaphore_payloads;
   }

   result = vk_sync_export_sync_file(&device->vk, sync, &fd);
   if (result != VK_SUCCESS)
      goto end_set_semaphore_payloads;

   for (uint32_t i = 0U; i < signal_count; i++) {
      VK_FROM_HANDLE(vk_semaphore, sem, signals[i]);
      struct vk_sync *sem_sync = vk_semaphore_get_active_sync(sem);

      result = vk_sync_import_sync_file(&device->vk, sem_sync, fd);
      if (result != VK_SUCCESS)
         goto end_set_semaphore_payloads;
   }

end_set_semaphore_payloads:
   if (fd != -1)
      close(fd);

   vk_sync_destroy(&device->vk, sync);

   return result;
}

static VkResult
pvr_set_fence_payload(struct pvr_device *device,
                      struct vk_sync *completions[static PVR_JOB_TYPE_MAX],
                      VkFence _fence)
{
   VK_FROM_HANDLE(vk_fence, fence, _fence);
   struct vk_sync *fence_sync;
   struct vk_sync *sync;
   VkResult result;

   result = vk_sync_create(&device->vk,
                           &device->pdevice->ws->syncobj_type,
                           0U,
                           0UL,
                           &sync);
   if (result != VK_SUCCESS)
      return result;

   result = device->ws->ops->null_job_submit(device->ws,
                                             completions,
                                             PVR_JOB_TYPE_MAX,
                                             sync);
   if (result != VK_SUCCESS) {
      vk_sync_destroy(&device->vk, sync);
      return result;
   }

   fence_sync = vk_fence_get_active_sync(fence);
   result = vk_sync_move(&device->vk, fence_sync, sync);
   vk_sync_destroy(&device->vk, sync);

   return result;
}

static void pvr_update_syncobjs(struct pvr_device *device,
                                struct vk_sync *src[static PVR_JOB_TYPE_MAX],
                                struct vk_sync *dst[static PVR_JOB_TYPE_MAX])
{
   for (uint32_t i = 0; i < PVR_JOB_TYPE_MAX; i++) {
      if (src[i]) {
         if (dst[i])
            vk_sync_destroy(&device->vk, dst[i]);

         dst[i] = src[i];
      }
   }
}

static VkResult pvr_process_cmd_buffer(
   struct pvr_device *device,
   struct pvr_queue *queue,
   VkCommandBuffer commandBuffer,
   struct vk_sync *barriers[static PVR_JOB_TYPE_MAX],
   struct vk_sync **waits,
   uint32_t wait_count,
   uint32_t *stage_flags,
   struct vk_sync *per_submit_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *queue_syncobjs[static PVR_JOB_TYPE_MAX],
   struct vk_sync *previous_queue_syncobjs[static PVR_JOB_TYPE_MAX])
{
   struct vk_sync *per_cmd_buffer_syncobjs[PVR_JOB_TYPE_MAX] = {};
   PVR_FROM_HANDLE(pvr_cmd_buffer, cmd_buffer, commandBuffer);
   VkResult result;

   assert(cmd_buffer->vk.state == MESA_VK_COMMAND_BUFFER_STATE_EXECUTABLE);

   list_for_each_entry_safe (struct pvr_sub_cmd,
                             sub_cmd,
                             &cmd_buffer->sub_cmds,
                             link) {
      switch (sub_cmd->type) {
      case PVR_SUB_CMD_TYPE_GRAPHICS: {
         if (sub_cmd->gfx.has_occlusion_query) {
            struct pvr_sub_cmd_event frag_to_transfer_barrier = {
               .type = PVR_EVENT_TYPE_BARRIER,
               .barrier = {
                  .wait_for_stage_mask = PVR_PIPELINE_STAGE_OCCLUSION_QUERY_BIT,
                  .wait_at_stage_mask = PVR_PIPELINE_STAGE_FRAG_BIT,
               },
            };

            /* If the fragment job utilizes occlusion queries, for data
             * integrity it needs to wait for the occlusion query to be
             * processed.
             */

            result = pvr_process_event_cmd_barrier(device,
                                                   &frag_to_transfer_barrier,
                                                   barriers,
                                                   per_cmd_buffer_syncobjs,
                                                   per_submit_syncobjs,
                                                   queue_syncobjs,
                                                   previous_queue_syncobjs);
            if (result != VK_SUCCESS)
               break;
         }

         result = pvr_process_graphics_cmd(device,
                                           queue,
                                           cmd_buffer,
                                           &sub_cmd->gfx,
                                           barriers[PVR_JOB_TYPE_GEOM],
                                           barriers[PVR_JOB_TYPE_FRAG],
                                           waits,
                                           wait_count,
                                           stage_flags,
                                           per_cmd_buffer_syncobjs);
         break;
      }

      case PVR_SUB_CMD_TYPE_COMPUTE:
         result = pvr_process_compute_cmd(device,
                                          queue,
                                          &sub_cmd->compute,
                                          barriers[PVR_JOB_TYPE_COMPUTE],
                                          waits,
                                          wait_count,
                                          stage_flags,
                                          per_cmd_buffer_syncobjs);
         break;

      case PVR_SUB_CMD_TYPE_TRANSFER: {
         const bool serialize_with_frag = sub_cmd->transfer.serialize_with_frag;

         if (serialize_with_frag) {
            struct pvr_sub_cmd_event frag_to_transfer_barrier = {
               .type = PVR_EVENT_TYPE_BARRIER,
               .barrier = {
                  .wait_for_stage_mask = PVR_PIPELINE_STAGE_FRAG_BIT,
                  .wait_at_stage_mask = PVR_PIPELINE_STAGE_TRANSFER_BIT,
               },
            };

            result = pvr_process_event_cmd_barrier(device,
                                                   &frag_to_transfer_barrier,
                                                   barriers,
                                                   per_cmd_buffer_syncobjs,
                                                   per_submit_syncobjs,
                                                   queue_syncobjs,
                                                   previous_queue_syncobjs);
            if (result != VK_SUCCESS)
               break;
         }

         result = pvr_process_transfer_cmds(device,
                                            queue,
                                            &sub_cmd->transfer,
                                            barriers[PVR_JOB_TYPE_TRANSFER],
                                            waits,
                                            wait_count,
                                            stage_flags,
                                            per_cmd_buffer_syncobjs);

         if (serialize_with_frag) {
            struct pvr_sub_cmd_event transfer_to_frag_barrier = {
               .type = PVR_EVENT_TYPE_BARRIER,
               .barrier = {
                  .wait_for_stage_mask = PVR_PIPELINE_STAGE_TRANSFER_BIT,
                  .wait_at_stage_mask = PVR_PIPELINE_STAGE_FRAG_BIT,
               },
            };

            if (result != VK_SUCCESS)
               break;

            result = pvr_process_event_cmd_barrier(device,
                                                   &transfer_to_frag_barrier,
                                                   barriers,
                                                   per_cmd_buffer_syncobjs,
                                                   per_submit_syncobjs,
                                                   queue_syncobjs,
                                                   previous_queue_syncobjs);
         }

         break;
      }

      case PVR_SUB_CMD_TYPE_OCCLUSION_QUERY:
         result = pvr_process_occlusion_query_cmd(
            device,
            queue,
            &sub_cmd->compute,
            barriers[PVR_JOB_TYPE_OCCLUSION_QUERY],
            waits,
            wait_count,
            stage_flags,
            per_cmd_buffer_syncobjs);
         break;

      case PVR_SUB_CMD_TYPE_EVENT:
         result = pvr_process_event_cmd(device,
                                        &sub_cmd->event,
                                        barriers,
                                        per_cmd_buffer_syncobjs,
                                        per_submit_syncobjs,
                                        queue_syncobjs,
                                        previous_queue_syncobjs);
         break;

      default:
         mesa_loge("Unsupported sub-command type %d", sub_cmd->type);
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      if (result != VK_SUCCESS)
         return result;

      p_atomic_inc(&device->global_queue_job_count);
   }

   pvr_update_syncobjs(device, per_cmd_buffer_syncobjs, per_submit_syncobjs);

   return VK_SUCCESS;
}

static VkResult
pvr_submit_null_job(struct pvr_device *device,
                    struct vk_sync **waits,
                    uint32_t wait_count,
                    uint32_t *stage_flags,
                    struct vk_sync *completions[static PVR_JOB_TYPE_MAX])
{
   VkResult result;

   STATIC_ASSERT(PVR_JOB_TYPE_MAX >= PVR_NUM_SYNC_PIPELINE_STAGES);
   for (uint32_t i = 0U; i < PVR_JOB_TYPE_MAX; i++) {
      struct vk_sync *per_job_waits[wait_count];
      uint32_t per_job_waits_count = 0;

      /* Get the waits specific to the job type. */
      for (uint32_t j = 0U; j < wait_count; j++) {
         if (stage_flags[j] & (1U << i)) {
            per_job_waits[per_job_waits_count] = waits[j];
            per_job_waits_count++;
         }
      }

      if (per_job_waits_count == 0U)
         continue;

      result = vk_sync_create(&device->vk,
                              &device->pdevice->ws->syncobj_type,
                              0U,
                              0UL,
                              &completions[i]);
      if (result != VK_SUCCESS)
         goto err_destroy_completion_syncs;

      result = device->ws->ops->null_job_submit(device->ws,
                                                per_job_waits,
                                                per_job_waits_count,
                                                completions[i]);
      if (result != VK_SUCCESS)
         goto err_destroy_completion_syncs;
   }

   return VK_SUCCESS;

err_destroy_completion_syncs:
   for (uint32_t i = 0U; i < PVR_JOB_TYPE_MAX; i++) {
      if (completions[i]) {
         vk_sync_destroy(&device->vk, completions[i]);
         completions[i] = NULL;
      }
   }

   return result;
}

VkResult pvr_QueueSubmit(VkQueue _queue,
                         uint32_t submitCount,
                         const VkSubmitInfo *pSubmits,
                         VkFence fence)
{
   PVR_FROM_HANDLE(pvr_queue, queue, _queue);
   struct vk_sync *completion_syncobjs[PVR_JOB_TYPE_MAX] = {};
   struct pvr_device *device = queue->device;
   VkResult result;

   for (uint32_t i = 0U; i < submitCount; i++) {
      struct vk_sync *per_submit_completion_syncobjs[PVR_JOB_TYPE_MAX] = {};
      const VkSubmitInfo *desc = &pSubmits[i];
      struct vk_sync *waits[desc->waitSemaphoreCount];
      uint32_t stage_flags[desc->waitSemaphoreCount];
      uint32_t wait_count = 0;

      for (uint32_t j = 0U; j < desc->waitSemaphoreCount; j++) {
         VK_FROM_HANDLE(vk_semaphore, semaphore, desc->pWaitSemaphores[j]);
         struct vk_sync *sync = vk_semaphore_get_active_sync(semaphore);

         if (sync->type == &vk_sync_dummy_type)
            continue;

         /* We don't currently support timeline semaphores. */
         assert(!(sync->flags & VK_SYNC_IS_TIMELINE));

         stage_flags[wait_count] =
            pvr_stage_mask_dst(desc->pWaitDstStageMask[j]);
         waits[wait_count] = vk_semaphore_get_active_sync(semaphore);
         wait_count++;
      }

      if (desc->commandBufferCount > 0U) {
         for (uint32_t j = 0U; j < desc->commandBufferCount; j++) {
            result = pvr_process_cmd_buffer(device,
                                            queue,
                                            desc->pCommandBuffers[j],
                                            queue->job_dependancy,
                                            waits,
                                            wait_count,
                                            stage_flags,
                                            per_submit_completion_syncobjs,
                                            completion_syncobjs,
                                            queue->completion);
            if (result != VK_SUCCESS)
               return result;
         }
      } else {
         result = pvr_submit_null_job(device,
                                      waits,
                                      wait_count,
                                      stage_flags,
                                      per_submit_completion_syncobjs);
         if (result != VK_SUCCESS)
            return result;
      }

      if (desc->signalSemaphoreCount) {
         result = pvr_set_semaphore_payloads(device,
                                             per_submit_completion_syncobjs,
                                             desc->pSignalSemaphores,
                                             desc->signalSemaphoreCount);
         if (result != VK_SUCCESS)
            return result;
      }

      pvr_update_syncobjs(device,
                          per_submit_completion_syncobjs,
                          completion_syncobjs);
   }

   if (fence) {
      result = pvr_set_fence_payload(device, completion_syncobjs, fence);
      if (result != VK_SUCCESS)
         return result;
   }

   pvr_update_syncobjs(device, completion_syncobjs, queue->completion);

   return VK_SUCCESS;
}
