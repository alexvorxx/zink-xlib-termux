/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_DRM_H
#define NVKMD_DRM_H 1

#include "nvkmd/nvkmd.h"
#include "vk_drm_syncobj.h"

#include <sys/types.h>

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

#endif /* NVKMD_DRM_H */
