/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nouveau.h"

#include "nouveau_bo.h"
#include "vk_log.h"

VkResult
nvkmd_nouveau_va_create(struct nvkmd_nouveau_dev *dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_va_flags flags, uint8_t pte_kind,
                        uint64_t addr, uint64_t size_B,
                        struct nvkmd_va **va_out)
{
   struct nvkmd_nouveau_va *va = CALLOC_STRUCT(nvkmd_nouveau_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   va->base.ops = &nvkmd_nouveau_va_ops;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.addr = addr;
   va->base.size_B = size_B;
   va->dev = dev->ws_dev;

   *va_out = &va->base;

   return VK_SUCCESS;
}

VkResult
nvkmd_nouveau_alloc_va(struct nvkmd_dev *_dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_va_flags flags, uint8_t pte_kind,
                       uint64_t size_B, uint64_t align_B,
                       uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_nouveau_dev *dev = nvkmd_nouveau_dev(_dev);

   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   struct nvkmd_nouveau_va *va = CALLOC_STRUCT(nvkmd_nouveau_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   va->base.addr = nouveau_ws_alloc_vma(dev->ws_dev, fixed_addr,
                                        size_B, align_B,
                                        flags & NVKMD_VA_REPLAY,
                                        flags & NVKMD_VA_SPARSE);
   if (va->base.addr == 0) {
      FREE(va);
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate virtual address range: %m");
   }

   va->base.ops = &nvkmd_nouveau_va_ops;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;
   va->dev = dev->ws_dev;

   *va_out = &va->base;

   return VK_SUCCESS;
}

static void
nvkmd_nouveau_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_nouveau_va *va = nvkmd_nouveau_va(_va);

   nouveau_ws_bo_unbind_vma(va->dev, va->base.addr, va->base.size_B);
   nouveau_ws_free_vma(va->dev, va->base.addr, va->base.size_B,
                       va->base.flags & NVKMD_VA_REPLAY,
                       va->base.flags & NVKMD_VA_SPARSE);
   FREE(va);
}

static VkResult
nvkmd_nouveau_va_bind_mem(struct nvkmd_va *_va,
                          struct vk_object_base *log_obj,
                          uint64_t va_offset_B,
                          struct nvkmd_mem *_mem,
                          uint64_t mem_offset_B,
                          uint64_t range_B)
{
   struct nvkmd_nouveau_va *va = nvkmd_nouveau_va(_va);
   struct nvkmd_nouveau_mem *mem = nvkmd_nouveau_mem(_mem);

   assert(va->dev == mem->bo->dev);
   nouveau_ws_bo_bind_vma(va->dev, mem->bo,
                          va->base.addr + va_offset_B, range_B,
                          mem_offset_B, va->base.pte_kind);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nouveau_va_unbind(struct nvkmd_va *_va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        uint64_t range_B)
{
   struct nvkmd_nouveau_va *va = nvkmd_nouveau_va(_va);

   nouveau_ws_bo_unbind_vma(va->dev, va->base.addr + va_offset_B, range_B);

   return VK_SUCCESS;
}

const struct nvkmd_va_ops nvkmd_nouveau_va_ops = {
   .free = nvkmd_nouveau_va_free,
   .bind_mem = nvkmd_nouveau_va_bind_mem,
   .unbind = nvkmd_nouveau_va_unbind,
};
