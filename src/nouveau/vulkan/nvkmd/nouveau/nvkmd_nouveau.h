/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_DRM_H
#define NVKMD_DRM_H 1

#include "nvkmd/nvkmd.h"
#include "vk_drm_syncobj.h"

#include <sys/types.h>

struct nouveau_ws_bo;
struct nouveau_ws_device;

struct nvkmd_nouveau_pdev {
   struct nvkmd_pdev base;

   /* Used for get_vram_used() */
   struct nouveau_ws_device *ws_dev;

   int primary_fd;

   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, nouveau);

VkResult nvkmd_nouveau_try_create_pdev(struct _drmDevice *drm_device,
                                       struct vk_object_base *log_obj,
                                       enum nvk_debug debug_flags,
                                       struct nvkmd_pdev **pdev_out);

struct nvkmd_nouveau_dev {
   struct nvkmd_dev base;

   struct nouveau_ws_device *ws_dev;
};

NVKMD_DECL_SUBCLASS(dev, nouveau);

VkResult nvkmd_nouveau_create_dev(struct nvkmd_pdev *pdev,
                                  struct vk_object_base *log_obj,
                                  struct nvkmd_dev **dev_out);

struct nvkmd_nouveau_mem {
   struct nvkmd_mem base;

   struct nouveau_ws_bo *bo;
};

NVKMD_DECL_SUBCLASS(mem, nouveau);

VkResult nvkmd_nouveau_alloc_mem(struct nvkmd_dev *dev,
                                 struct vk_object_base *log_obj,
                                 uint64_t size_B, uint64_t align_B,
                                 enum nvkmd_mem_flags flags,
                                 struct nvkmd_mem **mem_out);

VkResult nvkmd_nouveau_alloc_tiled_mem(struct nvkmd_dev *dev,
                                       struct vk_object_base *log_obj,
                                       uint64_t size_B, uint64_t align_B,
                                       uint8_t pte_kind, uint16_t tile_mode,
                                       enum nvkmd_mem_flags flags,
                                       struct nvkmd_mem **mem_out);

VkResult nvkmd_nouveau_import_dma_buf(struct nvkmd_dev *dev,
                                      struct vk_object_base *log_obj,
                                      int fd, struct nvkmd_mem **mem_out);

struct nvkmd_nouveau_va {
   struct nvkmd_va base;

   struct nouveau_ws_device *dev;
};

NVKMD_DECL_SUBCLASS(va, nouveau);

/* Internal helper to create a VA object for already allocated VA */
VkResult nvkmd_nouveau_va_create(struct nvkmd_nouveau_dev *dev,
                                 struct vk_object_base *log_obj,
                                 enum nvkmd_va_flags flags, uint8_t pte_kind,
                                 uint64_t addr, uint64_t size_B,
                                 struct nvkmd_va **va_out);

VkResult nvkmd_nouveau_alloc_va(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                enum nvkmd_va_flags flags, uint8_t pte_kind,
                                uint64_t size_B, uint64_t align_B,
                                uint64_t fixed_addr, struct nvkmd_va **va_out);

#endif /* NVKMD_DRM_H */
