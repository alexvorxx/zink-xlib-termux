/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nouveau.h"

#include "nouveau_bo.h"
#include "vk_log.h"

VkResult
nvkmd_nouveau_alloc_mem(struct nvkmd_dev *dev,
                        struct vk_object_base *log_obj,
                        uint64_t size_B, uint64_t align_B,
                        enum nvkmd_mem_flags flags,
                        struct nvkmd_mem **mem_out)
{
   return nvkmd_nouveau_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                        0 /* pte_kind */, 0 /* tile_mode */,
                                        flags, mem_out);
}

static VkResult
create_mem_or_close_bo(struct nvkmd_nouveau_dev *dev,
                       struct vk_object_base *log_obj,
                       uint64_t va_align_B, uint8_t pte_kind,
                       enum nvkmd_mem_flags flags,
                       struct nouveau_ws_bo *bo,
                       struct nvkmd_mem **mem_out)
{
   const uint64_t size_B = bo->size;
   VkResult result;

   struct nvkmd_nouveau_mem *mem = CALLOC_STRUCT(nvkmd_nouveau_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_bo;
   }

   mem->base.ops = &nvkmd_nouveau_mem_ops;
   mem->base.refcnt = 1;
   mem->base.flags = flags;
   mem->base.size_B = size_B;
   mem->bo = bo;

   result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                               0 /* flags */, pte_kind,
                               size_B, va_align_B,
                               0 /* fixed_addr */,
                               &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0 /* va_offset_B */,
                              &mem->base, 0 /* mem_offset_B */, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;

   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   FREE(mem);
fail_bo:
   nouveau_ws_bo_destroy(bo);

   return result;
}

VkResult
nvkmd_nouveau_alloc_tiled_mem(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              uint64_t size_B, uint64_t align_B,
                              uint8_t pte_kind, uint16_t tile_mode,
                              enum nvkmd_mem_flags flags,
                              struct nvkmd_mem **mem_out)
{
   struct nvkmd_nouveau_dev *dev = nvkmd_nouveau_dev(_dev);

   STATIC_ASSERT(NVKMD_MEM_LOCAL    == (int)NOUVEAU_WS_BO_LOCAL);
   STATIC_ASSERT(NVKMD_MEM_GART     == (int)NOUVEAU_WS_BO_GART);
   STATIC_ASSERT(NVKMD_MEM_CAN_MAP  == (int)NOUVEAU_WS_BO_MAP);
   STATIC_ASSERT(NVKMD_MEM_NO_SHARE == (int)NOUVEAU_WS_BO_NO_SHARE);

   struct nouveau_ws_bo *bo = nouveau_ws_bo_new_tiled(dev->ws_dev,
                                                      size_B, align_B,
                                                      pte_kind, tile_mode,
                                                      (int)flags);
   if (bo == NULL)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");

   return create_mem_or_close_bo(dev, log_obj, align_B, pte_kind,
                                 flags, bo, mem_out);
}

VkResult
nvkmd_nouveau_import_dma_buf(struct nvkmd_dev *_dev,
                             struct vk_object_base *log_obj,
                             int fd, struct nvkmd_mem **mem_out)
{
   struct nvkmd_nouveau_dev *dev = nvkmd_nouveau_dev(_dev);

   struct nouveau_ws_bo *bo = nouveau_ws_bo_from_dma_buf(dev->ws_dev, fd);
   if (bo == NULL)
      return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE, "%m");

   return create_mem_or_close_bo(dev, log_obj,
                                 0 /* align_B */,
                                 0 /* pte_kind */,
                                 (int)bo->flags,
                                 bo, mem_out);
}

static void
nvkmd_nouveau_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_nouveau_mem *mem = nvkmd_nouveau_mem(_mem);

   nvkmd_va_free(mem->base.va);
   nouveau_ws_bo_destroy(mem->bo);
   FREE(mem);
}

static VkResult
nvkmd_nouveau_mem_map(struct nvkmd_mem *_mem,
                      struct vk_object_base *log_obj,
                      enum nvkmd_mem_map_flags map_flags,
                      void *fixed_addr)
{
   struct nvkmd_nouveau_mem *mem = nvkmd_nouveau_mem(_mem);

   if (!(map_flags & NVKMD_MEM_MAP_FIXED))
      fixed_addr = NULL;

   STATIC_ASSERT(NVKMD_MEM_MAP_RD == (int)NOUVEAU_WS_BO_RD);
   STATIC_ASSERT(NVKMD_MEM_MAP_WR == (int)NOUVEAU_WS_BO_WR);

   enum nouveau_ws_bo_map_flags ws_map_flags =
      map_flags & NOUVEAU_WS_BO_RDWR;

   mem->base.map = nouveau_ws_bo_map(mem->bo, ws_map_flags, fixed_addr);
   if (mem->base.map == NULL)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   return VK_SUCCESS;
}

static void
nvkmd_nouveau_mem_unmap(struct nvkmd_mem *_mem)
{
   struct nvkmd_nouveau_mem *mem = nvkmd_nouveau_mem(_mem);

   nouveau_ws_bo_unmap(mem->bo, mem->base.map);
   mem->base.map = NULL;
}

static VkResult
nvkmd_nouveau_mem_overmap(struct nvkmd_mem *_mem,
                          struct vk_object_base *log_obj)
{
   struct nvkmd_nouveau_mem *mem = nvkmd_nouveau_mem(_mem);

   if (nouveau_ws_bo_overmap(mem->bo, mem->base.map)) {
      return vk_errorf(log_obj, VK_ERROR_MEMORY_MAP_FAILED,
                       "Failed to map over original mapping");
   }

   mem->base.map = NULL;

   return VK_SUCCESS;
}

static VkResult
nvkmd_nouveau_mem_export_dma_buf(struct nvkmd_mem *_mem,
                                 struct vk_object_base *log_obj,
                                 int *fd_out)
{
   struct nvkmd_nouveau_mem *mem = nvkmd_nouveau_mem(_mem);

   int err = nouveau_ws_bo_dma_buf(mem->bo, fd_out);
   if (err)
      return vk_errorf(log_obj, VK_ERROR_TOO_MANY_OBJECTS,
                       "Failed to export dma-buf: %m");

   return VK_SUCCESS;
}

const struct nvkmd_mem_ops nvkmd_nouveau_mem_ops = {
   .free = nvkmd_nouveau_mem_free,
   .map = nvkmd_nouveau_mem_map,
   .unmap = nvkmd_nouveau_mem_unmap,
   .overmap = nvkmd_nouveau_mem_overmap,
   .export_dma_buf = nvkmd_nouveau_mem_export_dma_buf,
};
