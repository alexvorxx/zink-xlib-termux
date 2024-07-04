/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_queue.h"

#include "nvk_cmd_buffer.h"
#include "nvk_cmd_pool.h"
#include "nvk_device.h"
#include "nvk_buffer.h"
#include "nvk_image.h"
#include "nvk_device_memory.h"
#include "nvk_physical_device.h"
#include "nvkmd/nvkmd.h"
#include "nvkmd/nouveau/nvkmd_nouveau.h"

#include "nouveau_context.h"

#include "drm-uapi/nouveau_drm.h"

#include "vk_drm_syncobj.h"

#include <xf86drm.h>

#define NVK_PUSH_MAX_SYNCS 256
#define NVK_PUSH_MAX_BINDS 4096
#define NVK_PUSH_MAX_PUSH 1024

struct push_builder {
   uint32_t max_push;
   struct drm_nouveau_sync req_wait[NVK_PUSH_MAX_SYNCS];
   struct drm_nouveau_sync req_sig[NVK_PUSH_MAX_SYNCS];
   struct drm_nouveau_exec_push req_push[NVK_PUSH_MAX_PUSH];
   struct drm_nouveau_exec req;
};

static void
push_builder_init(struct nvk_queue *queue,
                  struct push_builder *pb)
{
   struct nvk_device *dev = nvk_queue_device(queue);

   pb->max_push = MIN2(NVK_PUSH_MAX_PUSH, dev->ws_dev->max_push);
   pb->req = (struct drm_nouveau_exec) {
      .channel = queue->drm.ws_ctx->channel,
      .push_count = 0,
      .wait_count = 0,
      .sig_count = 0,
      .push_ptr = (uintptr_t)&pb->req_push,
      .wait_ptr = (uintptr_t)&pb->req_wait,
      .sig_ptr = (uintptr_t)&pb->req_sig,
   };
}

static void
push_add_syncobj_wait(struct push_builder *pb,
                      uint32_t syncobj,
                      uint64_t wait_value)
{
   assert(pb->req.wait_count < NVK_PUSH_MAX_SYNCS);
   pb->req_wait[pb->req.wait_count++] = (struct drm_nouveau_sync) {
      .flags = wait_value ? DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ :
                            DRM_NOUVEAU_SYNC_SYNCOBJ,
      .handle = syncobj,
      .timeline_value = wait_value,
   };
}

static void
push_add_sync_wait(struct push_builder *pb,
                   struct vk_sync_wait *wait)
{
   struct vk_drm_syncobj *sync = vk_sync_as_drm_syncobj(wait->sync);
   assert(sync != NULL);
   push_add_syncobj_wait(pb, sync->syncobj, wait->wait_value);
}

static void
push_add_sync_signal(struct push_builder *pb,
                     struct vk_sync_signal *sig)
{
   struct vk_drm_syncobj *sync  = vk_sync_as_drm_syncobj(sig->sync);
   assert(sync);
   assert(pb->req.sig_count < NVK_PUSH_MAX_SYNCS);
   pb->req_sig[pb->req.sig_count++] = (struct drm_nouveau_sync) {
      .flags = sig->signal_value ? DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ :
                                   DRM_NOUVEAU_SYNC_SYNCOBJ,
      .handle = sync->syncobj,
      .timeline_value = sig->signal_value,
   };
}

static void
push_add_push(struct push_builder *pb, uint64_t addr, uint32_t range,
              bool no_prefetch)
{
   /* This is the hardware limit on all current GPUs */
   assert((addr % 4) == 0 && (range % 4) == 0);
   assert(range < (1u << 23));

   uint32_t flags = 0;
   if (no_prefetch)
      flags |= DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH;

   assert(pb->req.push_count < pb->max_push);
   pb->req_push[pb->req.push_count++] = (struct drm_nouveau_exec_push) {
      .va = addr,
      .va_len = range,
      .flags = flags,
   };
}

static VkResult
push_submit(struct nvk_queue *queue, struct push_builder *pb, bool sync)
{
   struct nvk_device *dev = nvk_queue_device(queue);

   int err;
   if (sync) {
      assert(pb->req.sig_count < NVK_PUSH_MAX_SYNCS);
      pb->req_sig[pb->req.sig_count++] = (struct drm_nouveau_sync) {
         .flags = DRM_NOUVEAU_SYNC_SYNCOBJ,
         .handle = queue->drm.syncobj,
         .timeline_value = 0,
      };
   }
   err = drmCommandWriteRead(dev->ws_dev->fd,
                             DRM_NOUVEAU_EXEC,
                             &pb->req, sizeof(pb->req));
   if (err) {
      VkResult result = VK_ERROR_UNKNOWN;
      if (err == -ENODEV)
         result = VK_ERROR_DEVICE_LOST;
      return vk_errorf(queue, result,
                       "DRM_NOUVEAU_EXEC failed: %m");
   }
   if (sync) {
      err = drmSyncobjWait(dev->ws_dev->fd,
                           &queue->drm.syncobj, 1, INT64_MAX,
                           DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                           NULL);
      if (err) {
         return vk_errorf(queue, VK_ERROR_UNKNOWN,
                          "DRM_SYNCOBJ_WAIT failed: %m");
      }

      /* Push an empty again, just to check for errors */
      struct drm_nouveau_exec empty = {
         .channel = pb->req.channel,
      };
      err = drmCommandWriteRead(dev->ws_dev->fd,
                                DRM_NOUVEAU_EXEC,
                                &empty, sizeof(empty));
      if (err) {
         return vk_errorf(queue, VK_ERROR_DEVICE_LOST,
                          "DRM_NOUVEAU_EXEC failed: %m");
      }
   }
   return VK_SUCCESS;
}

VkResult
nvk_queue_init_drm_nouveau(struct nvk_device *dev,
                           struct nvk_queue *queue,
                           VkQueueFlags queue_flags)
{
   VkResult result;
   int err;

   enum nouveau_ws_engines engines = 0;
   if (queue_flags & VK_QUEUE_GRAPHICS_BIT)
      engines |= NOUVEAU_WS_ENGINE_3D;
   if (queue_flags & VK_QUEUE_COMPUTE_BIT)
      engines |= NOUVEAU_WS_ENGINE_COMPUTE;
   if (queue_flags & VK_QUEUE_TRANSFER_BIT)
      engines |= NOUVEAU_WS_ENGINE_COPY;

   err = nouveau_ws_context_create(dev->ws_dev, engines, &queue->drm.ws_ctx);
   if (err != 0) {
      if (err == -ENOSPC)
         return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
      else
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   err = drmSyncobjCreate(dev->ws_dev->fd, 0, &queue->drm.syncobj);
   if (err < 0) {
      result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_context;
   }

   return VK_SUCCESS;

fail_context:
   nouveau_ws_context_destroy(queue->drm.ws_ctx);

   return result;
}

void
nvk_queue_finish_drm_nouveau(struct nvk_device *dev,
                             struct nvk_queue *queue)
{
   ASSERTED int err = drmSyncobjDestroy(dev->ws_dev->fd, queue->drm.syncobj);
   assert(err == 0);
   nouveau_ws_context_destroy(queue->drm.ws_ctx);
}

VkResult
nvk_queue_submit_simple_drm_nouveau(struct nvk_queue *queue,
                                    uint32_t push_dw_count,
                                    struct nouveau_ws_bo *push_bo)
{
   struct push_builder pb;
   push_builder_init(queue, &pb);

   push_add_push(&pb, push_bo->offset, push_dw_count * 4, false);

   return push_submit(queue, &pb, true);
}

static void
push_add_queue_state(struct push_builder *pb, struct nvk_queue_state *qs)
{
   if (qs->push.mem)
      push_add_push(pb, qs->push.mem->va->addr, qs->push.dw_count * 4, false);
}

VkResult
nvk_queue_submit_drm_nouveau(struct nvk_queue *queue,
                             struct vk_queue_submit *submit,
                             bool sync)
{
   struct nvk_device *dev = nvk_queue_device(queue);
   struct push_builder pb;
   VkResult result;

   uint64_t upload_time_point;
   result = nvk_upload_queue_flush(dev, &dev->upload, &upload_time_point);
   if (result != VK_SUCCESS)
      return result;

   push_builder_init(queue, &pb);

   if (upload_time_point > 0) {
      push_add_sync_wait(&pb, &(struct vk_sync_wait) {
         .sync = dev->upload.sync,
         .stage_mask = ~0,
         .wait_value = upload_time_point,
      });
   }

   for (uint32_t i = 0; i < submit->wait_count; i++)
      push_add_sync_wait(&pb, &submit->waits[i]);

   push_add_queue_state(&pb, &queue->state);

   assert(submit->buffer_bind_count == 0);
   assert(submit->image_bind_count == 0);
   assert(submit->image_opaque_bind_count == 0);

   for (unsigned i = 0; i < submit->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd =
         container_of(submit->command_buffers[i], struct nvk_cmd_buffer, vk);

      util_dynarray_foreach(&cmd->pushes, struct nvk_cmd_push, push) {
         if (push->range == 0)
            continue;

         if (pb.req.push_count >= pb.max_push) {
            result = push_submit(queue, &pb, sync);
            if (result != VK_SUCCESS)
               return result;

            push_builder_init(queue, &pb);
         }

         push_add_push(&pb, push->addr, push->range, push->no_prefetch);
      }
   }

   for (uint32_t i = 0; i < submit->signal_count; i++)
      push_add_sync_signal(&pb, &submit->signals[i]);

   return push_submit(queue, &pb, sync);
}
