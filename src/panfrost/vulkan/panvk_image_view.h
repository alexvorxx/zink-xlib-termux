/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_IMAGE_VIEW_H
#define PANVK_IMAGE_VIEW_H

#include <stdint.h>

#include "vk_image.h"

#include "pan_texture.h"

struct panvk_priv_bo;

#define TEXTURE_DESC_WORDS    8
#define ATTRIB_BUF_DESC_WORDS 4

struct panvk_image_view {
   struct vk_image_view vk;

   struct pan_image_view pview;

   struct panvk_priv_bo *bo;
   struct {
      uint32_t tex[TEXTURE_DESC_WORDS];
      uint32_t img_attrib_buf[ATTRIB_BUF_DESC_WORDS * 2];
   } descs;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_image_view, vk.base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW);

#endif
