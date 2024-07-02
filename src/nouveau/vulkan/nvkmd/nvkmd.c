/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd.h"
#include "nouveau/nvkmd_nouveau.h"

VkResult
nvkmd_try_create_pdev_for_drm(struct _drmDevice *drm_device,
                              struct vk_object_base *log_obj,
                              enum nvk_debug debug_flags,
                              struct nvkmd_pdev **pdev_out)
{
   return nvkmd_nouveau_try_create_pdev(drm_device, log_obj,
                                        debug_flags, pdev_out);
}

VkResult
nvkmd_dev_alloc_mapped_mem(struct nvkmd_dev *dev,
                           struct vk_object_base *log_obj,
                           uint64_t size_B, uint64_t align_B,
                           enum nvkmd_mem_flags flags,
                           enum nvkmd_mem_map_flags map_flags,
                           struct nvkmd_mem **mem_out)
{
   struct nvkmd_mem *mem;
   VkResult result;

   result = nvkmd_dev_alloc_mem(dev, log_obj, size_B, align_B,
                                flags | NVKMD_MEM_CAN_MAP, &mem);
   if (result != VK_SUCCESS)
      return result;

   assert(!(map_flags & NVKMD_MEM_MAP_FIXED));
   result = mem->ops->map(mem, log_obj, map_flags, NULL);
   if (result != VK_SUCCESS) {
      mem->ops->free(mem);
      return result;
   }

   *mem_out = mem;

   return VK_SUCCESS;
}

void
nvkmd_mem_unref(struct nvkmd_mem *mem)
{
   assert(p_atomic_read(&mem->refcnt) > 0);
   if (!p_atomic_dec_zero(&mem->refcnt))
      return;

   if (mem->map != NULL)
      mem->ops->unmap(mem);

   mem->ops->free(mem);
}
