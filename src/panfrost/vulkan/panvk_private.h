/*
 * Copyright © 2021 Collabora Ltd.
 *
 * derived from tu_private.h driver which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PANVK_PRIVATE_H
#define PANVK_PRIVATE_H

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "compiler/shader_enums.h"
#include "util/list.h"
#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_buffer.h"
#include "vk_buffer_view.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_descriptor_set_layout.h"
#include "vk_device.h"
#include "vk_device_memory.h"
#include "vk_image.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_physical_device.h"
#include "vk_pipeline_layout.h"
#include "vk_queue.h"
#include "vk_sampler.h"
#include "vk_sync.h"
#include "wsi_common.h"

#include "drm-uapi/panfrost_drm.h"

#include "pan_blend.h"
#include "pan_blitter.h"
#include "pan_desc.h"
#include "pan_jc.h"
#include "pan_texture.h"
#include "panvk_descriptor_set.h"
#include "panvk_descriptor_set_layout.h"
#include "panvk_instance.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_meta.h"
#include "panvk_physical_device.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"
#include "panvk_shader.h"
#include "panvk_varyings.h"
#include "vk_extensions.h"

#include "kmod/pan_kmod.h"

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "panvk_entrypoints.h"

#define MAX_VERTEX_ATTRIBS     16
#define MAX_VSC_PIPES          32
#define MAX_SCISSORS           16
#define MAX_DISCARD_RECTANGLES 4
#define MAX_SAMPLES_LOG2       4
#define NUM_META_FS_KEYS       13
#define MAX_VIEWS              8

#define NUM_DEPTH_CLEAR_PIPELINES 3

struct panvk_device;
struct panvk_pipeline_layout;
struct panvk_queue;

/* Used for internal object allocation. */
struct panvk_priv_bo {
   struct panvk_device *dev;
   struct pan_kmod_bo *bo;
   struct {
      mali_ptr dev;
      void *host;
   } addr;
};

struct panvk_priv_bo *panvk_priv_bo_create(struct panvk_device *dev,
                                           size_t size, uint32_t flags,
                                           const VkAllocationCallbacks *alloc,
                                           VkSystemAllocationScope scope);

void panvk_priv_bo_destroy(struct panvk_priv_bo *bo,
                           const VkAllocationCallbacks *alloc);

VkResult panvk_wsi_init(struct panvk_physical_device *physical_device);
void panvk_wsi_finish(struct panvk_physical_device *physical_device);

#endif /* PANVK_PRIVATE_H */
