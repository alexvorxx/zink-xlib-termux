/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_BUFFER_VIEW_H
#define RADV_BUFFER_VIEW_H

#include "vk_buffer_view.h"

struct radv_device;

struct radv_buffer_view {
   struct vk_buffer_view vk;
   struct radeon_winsys_bo *bo;
   uint32_t state[4];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(radv_buffer_view, vk.base, VkBufferView, VK_OBJECT_TYPE_BUFFER_VIEW)

void radv_buffer_view_init(struct radv_buffer_view *view, struct radv_device *device,
                           const VkBufferViewCreateInfo *pCreateInfo);
void radv_buffer_view_finish(struct radv_buffer_view *view);

void radv_make_texel_buffer_descriptor(struct radv_device *device, uint64_t va, VkFormat vk_format, unsigned offset,
                                       unsigned range, uint32_t *state);

#endif /* RADV_BUFFER_VIEW_H */
