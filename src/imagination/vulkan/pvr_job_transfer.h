/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PVR_JOB_TRANSFER_H
#define PVR_JOB_TRANSFER_H

#include <stdint.h>
#include <vulkan/vulkan.h>

struct pvr_device;
struct pvr_sub_cmd_transfer;
struct pvr_transfer_ctx;
struct vk_sync;

VkResult pvr_transfer_job_submit(struct pvr_device *device,
                                 struct pvr_transfer_ctx *ctx,
                                 struct pvr_sub_cmd_transfer *sub_cmd,
                                 struct vk_sync *barrier,
                                 struct vk_sync **waits,
                                 uint32_t wait_count,
                                 uint32_t *stage_flags,
                                 struct vk_sync *signal_sync);

#endif /* PVR_JOB_TRANSFER_H */
