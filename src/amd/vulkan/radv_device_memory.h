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

#ifndef RADV_DEVICE_MEMORY_H
#define RADV_DEVICE_MEMORY_H

#include "vk_object.h"

#include "radv_private.h"

struct radv_device;

struct radv_device_memory {
   struct vk_object_base base;
   struct radeon_winsys_bo *bo;
   /* for dedicated allocations */
   struct radv_image *image;
   struct radv_buffer *buffer;
   uint32_t heap_index;
   uint64_t alloc_size;
   void *map;
   void *user_ptr;

#if RADV_SUPPORT_ANDROID_HARDWARE_BUFFER
   struct AHardwareBuffer *android_hardware_buffer;
#endif
};

VK_DEFINE_NONDISP_HANDLE_CASTS(radv_device_memory, base, VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

void radv_device_memory_init(struct radv_device_memory *mem, struct radv_device *device, struct radeon_winsys_bo *bo);

void radv_device_memory_finish(struct radv_device_memory *mem);

void radv_free_memory(struct radv_device *device, const VkAllocationCallbacks *pAllocator,
                      struct radv_device_memory *mem);

VkResult radv_alloc_memory(struct radv_device *device, const VkMemoryAllocateInfo *pAllocateInfo,
                           const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMem, bool is_internal);

#endif /* RADV_DEVICE_MEMORY_H */
