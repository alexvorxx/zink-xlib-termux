/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd.h"

VkResult
nvkmd_try_create_pdev_for_drm(struct _drmDevice *drm_device,
                              struct vk_object_base *log_obj,
                              enum nvk_debug debug_flags,
                              struct nvkmd_pdev **pdev_out)
{
   return VK_ERROR_INCOMPATIBLE_DRIVER;
}
