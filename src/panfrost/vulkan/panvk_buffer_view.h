/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_BUFFER_VIEW_H
#define PANVK_BUFFER_VIEW_H

#include <stdint.h>

#include "vk_buffer_view.h"

struct panvk_priv_bo;

#define TEXTURE_DESC_WORDS    8
#define ATTRIB_BUF_DESC_WORDS 4

struct panvk_buffer_view {
   struct vk_buffer_view vk;
   struct panvk_priv_bo *bo;
   struct {
      uint32_t tex[TEXTURE_DESC_WORDS];
      uint32_t img_attrib_buf[ATTRIB_BUF_DESC_WORDS * 2];
   } descs;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_buffer_view, vk.base, VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)

#endif
