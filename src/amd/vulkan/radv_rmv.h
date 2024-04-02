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

#ifndef RADV_RMV_H
#define RADV_RMV_H

#include "rmv/vk_rmv_common.h"
#include "rmv/vk_rmv_tokens.h"

#include "radv_radeon_winsys.h"

struct radv_device;
struct radv_physical_device;
struct radv_pipeline;
struct radv_ray_tracing_pipeline;

void radv_memory_trace_init(struct radv_device *device);

void radv_rmv_fill_device_info(const struct radv_physical_device *pdev, struct vk_rmv_device_info *info);

void radv_rmv_collect_trace_events(struct radv_device *device);

void radv_memory_trace_finish(struct radv_device *device);

void radv_rmv_log_heap_create(struct radv_device *device, VkDeviceMemory heap, bool is_internal,
                              VkMemoryAllocateFlags alloc_flags);

void radv_rmv_log_bo_allocate(struct radv_device *device, struct radeon_winsys_bo *bo, bool is_internal);

void radv_rmv_log_bo_destroy(struct radv_device *device, struct radeon_winsys_bo *bo);

void radv_rmv_log_buffer_bind(struct radv_device *device, VkBuffer _buffer);

void radv_rmv_log_image_create(struct radv_device *device, const VkImageCreateInfo *create_info, bool is_internal,
                               VkImage _image);

void radv_rmv_log_image_bind(struct radv_device *device, VkImage _image);

void radv_rmv_log_query_pool_create(struct radv_device *device, VkQueryPool pool);

void radv_rmv_log_command_buffer_bo_create(struct radv_device *device, struct radeon_winsys_bo *bo,
                                           uint32_t executable_size, uint32_t data_size, uint32_t scratch_size);

void radv_rmv_log_command_buffer_bo_destroy(struct radv_device *device, struct radeon_winsys_bo *bo);

void radv_rmv_log_border_color_palette_create(struct radv_device *device, struct radeon_winsys_bo *bo);

void radv_rmv_log_border_color_palette_destroy(struct radv_device *device, struct radeon_winsys_bo *bo);

void radv_rmv_log_sparse_add_residency(struct radv_device *device, struct radeon_winsys_bo *src_bo, uint64_t offset);

void radv_rmv_log_sparse_remove_residency(struct radv_device *device, struct radeon_winsys_bo *src_bo, uint64_t offset);

void radv_rmv_log_descriptor_pool_create(struct radv_device *device, const VkDescriptorPoolCreateInfo *create_info,
                                         VkDescriptorPool pool);

void radv_rmv_log_graphics_pipeline_create(struct radv_device *device, struct radv_pipeline *pipeline,
                                           bool is_internal);

void radv_rmv_log_compute_pipeline_create(struct radv_device *device, struct radv_pipeline *pipeline, bool is_internal);

void radv_rmv_log_rt_pipeline_create(struct radv_device *device, struct radv_ray_tracing_pipeline *pipeline);

void radv_rmv_log_event_create(struct radv_device *device, VkEvent event, VkEventCreateFlags flags, bool is_internal);

void radv_rmv_log_submit(struct radv_device *device, enum amd_ip_type type);

void radv_rmv_log_resource_destroy(struct radv_device *device, uint64_t handle);

#endif /* RADV_RMV_H */
