/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvk_upload_queue.h"

#include "nvk_device.h"
#include "nvkmd/nvkmd.h"
#include "vk_alloc.h"

#include <xf86drm.h>
#include "nouveau_context.h"
#include "nouveau_device.h"
#include "drm-uapi/nouveau_drm.h"

#include "nv_push.h"
#include "nv_push_cl90b5.h"

#define NVK_UPLOAD_MEM_SIZE 64*1024

struct nvk_upload_mem {
   struct nvkmd_mem *mem;

   /** Link in nvk_upload_queue::recycle */
   struct list_head link;

   /** Time point at which point this BO will be idle */
   uint64_t idle_time_point;
};

static VkResult
nvk_upload_mem_create(struct nvk_device *dev,
                     struct nvk_upload_mem **mem_out)
{
   struct nvk_upload_mem *mem;
   VkResult result;

   mem = vk_zalloc(&dev->vk.alloc, sizeof(*mem), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (mem == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t flags = NVKMD_MEM_GART | NVKMD_MEM_CAN_MAP | NVKMD_MEM_NO_SHARE;
   result = nvkmd_dev_alloc_mapped_mem(dev->nvkmd, &dev->vk.base,
                                       NVK_UPLOAD_MEM_SIZE, 0,
                                       flags, NVKMD_MEM_MAP_WR, &mem->mem);
   if (result != VK_SUCCESS) {
      vk_free(&dev->vk.alloc, mem);
      return result;
   }

   *mem_out = mem;

   return VK_SUCCESS;
}

static void
nvk_upload_mem_destroy(struct nvk_device *dev,
                      struct nvk_upload_mem *mem)
{
   nvkmd_mem_unref(mem->mem);
   vk_free(&dev->vk.alloc, mem);
}

VkResult
nvk_upload_queue_init(struct nvk_device *dev,
                      struct nvk_upload_queue *queue)
{
   VkResult result;

   memset(queue, 0, sizeof(*queue));

   simple_mtx_init(&queue->mutex, mtx_plain);

   int err = nouveau_ws_context_create(dev->ws_dev, NOUVEAU_WS_ENGINE_COPY,
                                       &queue->drm.ws_ctx);
   if (err != 0) {
      if (err == -ENOSPC)
         result = vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
      else
         result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_mutex;
   }

   err = drmSyncobjCreate(dev->ws_dev->fd, 0, &queue->drm.syncobj);
   if (err < 0) {
      result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_context;
   }

   list_inithead(&queue->recycle);

   return VK_SUCCESS;

fail_mutex:
   simple_mtx_destroy(&queue->mutex);
fail_context:
   nouveau_ws_context_destroy(queue->drm.ws_ctx);

   return result;
}

void
nvk_upload_queue_finish(struct nvk_device *dev,
                        struct nvk_upload_queue *queue)
{
   list_for_each_entry_safe(struct nvk_upload_mem, mem, &queue->recycle, link)
      nvk_upload_mem_destroy(dev, mem);

   if (queue->mem != NULL)
      nvk_upload_mem_destroy(dev, queue->mem);

   drmSyncobjDestroy(dev->ws_dev->fd, queue->drm.syncobj);
   nouveau_ws_context_destroy(queue->drm.ws_ctx);
   simple_mtx_destroy(&queue->mutex);
}

static VkResult
nvk_upload_queue_flush_locked(struct nvk_device *dev,
                              struct nvk_upload_queue *queue,
                              uint64_t *time_point_out)
{
   if (queue->mem == NULL || queue->mem_push_start == queue->mem_push_end) {
      if (time_point_out != NULL)
         *time_point_out = queue->last_time_point;
      return VK_SUCCESS;
   }

   uint64_t time_point = queue->last_time_point + 1;
   if (time_point == UINT64_MAX)
      abort();

   struct drm_nouveau_exec_push push = {
      .va = queue->mem->mem->va->addr + queue->mem_push_start,
      .va_len = queue->mem_push_end - queue->mem_push_start,
   };

   struct drm_nouveau_sync sig = {
      .flags = DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ,
      .handle = queue->drm.syncobj,
      .timeline_value = time_point,
   };

   struct drm_nouveau_exec req = {
      .channel = queue->drm.ws_ctx->channel,
      .push_count = 1,
      .sig_count = 1,
      .push_ptr = (uintptr_t)&push,
      .sig_ptr = (uintptr_t)&sig,
   };

   int err = drmCommandWriteRead(dev->ws_dev->fd, DRM_NOUVEAU_EXEC,
                                 &req, sizeof(req));
   if (err != 0)
      return vk_device_set_lost(&dev->vk, "DRM_NOUVEAU_EXEC failed: %m");

   /* Wait until now to update last_time_point so that, if we do fail and lose
    * the device, nvk_upload_queue_sync won't wait forever on a time point
    * that will never signal.
    */
   queue->last_time_point = time_point;

   queue->mem->idle_time_point = time_point;
   queue->mem_push_start = queue->mem_push_end;

   if (time_point_out != NULL)
      *time_point_out = time_point;

   return VK_SUCCESS;
}

VkResult
nvk_upload_queue_flush(struct nvk_device *dev,
                       struct nvk_upload_queue *queue,
                       uint64_t *time_point_out)
{
   VkResult result;

   simple_mtx_lock(&queue->mutex);
   result = nvk_upload_queue_flush_locked(dev, queue, time_point_out);
   simple_mtx_unlock(&queue->mutex);

   return result;
}

static VkResult
nvk_upload_queue_sync_locked(struct nvk_device *dev,
                             struct nvk_upload_queue *queue)
{
   VkResult result;

   result = nvk_upload_queue_flush_locked(dev, queue, NULL);
   if (result != VK_SUCCESS)
      return result;

   if (queue->last_time_point == 0)
      return VK_SUCCESS;

   int err = drmSyncobjTimelineWait(dev->ws_dev->fd, &queue->drm.syncobj,
                                    &queue->last_time_point, 1, INT64_MAX,
                                    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                                    NULL);
   if (err != 0)
      return vk_device_set_lost(&dev->vk, "DRM_IOCTL_SYNCOBJ_WAIT failed: %m");

   return VK_SUCCESS;
}

VkResult
nvk_upload_queue_sync(struct nvk_device *dev,
                      struct nvk_upload_queue *queue)
{
   VkResult result;

   simple_mtx_lock(&queue->mutex);
   result = nvk_upload_queue_sync_locked(dev, queue);
   simple_mtx_unlock(&queue->mutex);

   return result;
}

static VkResult
nvk_upload_queue_reserve(struct nvk_device *dev,
                         struct nvk_upload_queue *queue,
                         uint32_t min_mem_size)
{
   VkResult result;

   assert(min_mem_size <= NVK_UPLOAD_MEM_SIZE);
   assert(queue->mem_push_end <= queue->mem_data_start);

   if (queue->mem != NULL) {
      if (queue->mem_data_start - queue->mem_push_end >= min_mem_size)
         return VK_SUCCESS;

      /* Not enough room in the BO.  Flush and add it to the recycle list */
      result = nvk_upload_queue_flush_locked(dev, queue, NULL);
      if (result != VK_SUCCESS)
         return result;

      assert(queue->mem_push_start == queue->mem_push_end);
      list_addtail(&queue->mem->link, &queue->recycle);
      queue->mem = NULL;
   }

   assert(queue->mem == NULL);
   queue->mem_push_start = queue->mem_push_end = 0;
   queue->mem_data_start = NVK_UPLOAD_MEM_SIZE;

   /* Try to pop an idle BO off the recycle list */
   if (!list_is_empty(&queue->recycle)) {
      uint64_t time_point_passed = 0;
      int err = drmSyncobjQuery(dev->ws_dev->fd, &queue->drm.syncobj,
                                &time_point_passed, 1);
      if (err) {
         return vk_device_set_lost(&dev->vk,
                                   "DRM_IOCTL_SYNCOBJ_QUERY failed: %m");
      }

      struct nvk_upload_mem *mem =
         list_first_entry(&queue->recycle, struct nvk_upload_mem, link);
      if (time_point_passed >= mem->idle_time_point) {
         list_del(&mem->link);
         queue->mem = mem;
         return VK_SUCCESS;
      }
   }

   return nvk_upload_mem_create(dev, &queue->mem);
}

static VkResult
nvk_upload_queue_upload_locked(struct nvk_device *dev,
                               struct nvk_upload_queue *queue,
                               uint64_t dst_addr,
                               const void *src, size_t size)
{
   VkResult result;

   assert(dst_addr % 4 == 0);
   assert(size % 4 == 0);

   while (size > 0) {
      const uint32_t cmd_size_dw = 12;
      const uint32_t cmd_size = cmd_size_dw * 4;

      /* Don't split the upload for stmall stuff.  If it's under 1KB and we
       * can't fit it in the current buffer, just get another.
       */
      const uint32_t min_size = cmd_size + MIN2(size, 1024);
      result = nvk_upload_queue_reserve(dev, queue, min_size);
      if (result != VK_SUCCESS)
         return result;

      assert(queue->mem != NULL);
      assert(queue->mem_data_start > queue->mem_push_end);
      const uint32_t avail = queue->mem_data_start - queue->mem_push_end;
      assert(avail >= min_size);

      const uint32_t data_size = MIN2(size, avail - cmd_size);

      const uint32_t data_mem_offset = queue->mem_data_start - data_size;
      assert(queue->mem_push_end + cmd_size <= data_mem_offset);
      const uint64_t data_addr = queue->mem->mem->va->addr + data_mem_offset;
      memcpy(queue->mem->mem->map + data_mem_offset, src, data_size);
      queue->mem_data_start = data_mem_offset;

      struct nv_push p;
      nv_push_init(&p, queue->mem->mem->map + queue->mem_push_end, cmd_size_dw);

      assert(data_size <= (1 << 17));

      P_MTHD(&p, NV90B5, OFFSET_IN_UPPER);
      P_NV90B5_OFFSET_IN_UPPER(&p, data_addr >> 32);
      P_NV90B5_OFFSET_IN_LOWER(&p, data_addr & 0xffffffff);
      P_NV90B5_OFFSET_OUT_UPPER(&p, dst_addr >> 32);
      P_NV90B5_OFFSET_OUT_LOWER(&p, dst_addr & 0xffffffff);
      P_NV90B5_PITCH_IN(&p, data_size);
      P_NV90B5_PITCH_OUT(&p, data_size);
      P_NV90B5_LINE_LENGTH_IN(&p, data_size);
      P_NV90B5_LINE_COUNT(&p, 1);

      P_IMMD(&p, NV90B5, LAUNCH_DMA, {
         .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
         .multi_line_enable = MULTI_LINE_ENABLE_FALSE,
         .flush_enable = FLUSH_ENABLE_TRUE,
         .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
         .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
      });

      assert(nv_push_dw_count(&p) <= cmd_size_dw);
      queue->mem_push_end += nv_push_dw_count(&p) * 4;

      dst_addr += data_size;
      src += data_size;
      size -= data_size;
   }

   return VK_SUCCESS;
}

VkResult
nvk_upload_queue_upload(struct nvk_device *dev,
                        struct nvk_upload_queue *queue,
                        uint64_t dst_addr,
                        const void *src, size_t size)
{
   VkResult result;

   simple_mtx_lock(&queue->mutex);
   result = nvk_upload_queue_upload_locked(dev, queue, dst_addr, src, size);
   simple_mtx_unlock(&queue->mutex);

   return result;
}

static VkResult
nvk_upload_queue_fill_locked(struct nvk_device *dev,
                             struct nvk_upload_queue *queue,
                             uint64_t dst_addr, uint32_t data, size_t size)
{
   VkResult result;

   assert(dst_addr % 4 == 0);
   assert(size % 4 == 0);

   while (size > 0) {
      const uint32_t cmd_size_dw = 14;
      const uint32_t cmd_size = cmd_size_dw * 4;

      result = nvk_upload_queue_reserve(dev, queue, cmd_size);
      if (result != VK_SUCCESS)
         return result;

      const uint32_t max_dim = 1 << 17;
      uint32_t width_B, height;
      if (size > max_dim) {
         width_B = max_dim;
         height = MIN2(max_dim, size / width_B);
      } else {
         width_B = size;
         height = 1;
      }
      assert(width_B * height <= size);

      struct nv_push p;
      nv_push_init(&p, queue->mem->mem->map + queue->mem_push_end, cmd_size_dw);

      P_MTHD(&p, NV90B5, OFFSET_OUT_UPPER);
      P_NV90B5_OFFSET_OUT_UPPER(&p, dst_addr >> 32);
      P_NV90B5_OFFSET_OUT_LOWER(&p, dst_addr & 0xffffffff);
      P_NV90B5_PITCH_IN(&p, width_B);
      P_NV90B5_PITCH_OUT(&p, width_B);
      P_NV90B5_LINE_LENGTH_IN(&p, width_B / 4);
      P_NV90B5_LINE_COUNT(&p, height);

      P_IMMD(&p, NV90B5, SET_REMAP_CONST_A, data);
      P_IMMD(&p, NV90B5, SET_REMAP_COMPONENTS, {
         .dst_x = DST_X_CONST_A,
         .dst_y = DST_Y_CONST_A,
         .dst_z = DST_Z_CONST_A,
         .dst_w = DST_W_CONST_A,
         .component_size = COMPONENT_SIZE_FOUR,
         .num_src_components = NUM_SRC_COMPONENTS_ONE,
         .num_dst_components = NUM_DST_COMPONENTS_ONE,
      });

      P_IMMD(&p, NV90B5, LAUNCH_DMA, {
         .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
         .multi_line_enable = height > 1,
         .flush_enable = FLUSH_ENABLE_TRUE,
         .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
         .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
         .remap_enable = REMAP_ENABLE_TRUE,
      });

      assert(nv_push_dw_count(&p) <= cmd_size_dw);
      queue->mem_push_end += nv_push_dw_count(&p) * 4;

      dst_addr += width_B * height;
      size -= width_B * height;
   }

   return VK_SUCCESS;
}

VkResult
nvk_upload_queue_fill(struct nvk_device *dev,
                      struct nvk_upload_queue *queue,
                      uint64_t dst_addr, uint32_t data, size_t size)
{
   VkResult result;

   simple_mtx_lock(&queue->mutex);
   result = nvk_upload_queue_fill_locked(dev, queue, dst_addr, data, size);
   simple_mtx_unlock(&queue->mutex);

   return result;
}
