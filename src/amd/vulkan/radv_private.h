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

#ifndef RADV_PRIVATE_H
#define RADV_PRIVATE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_VALGRIND
#include <memcheck.h>
#include <valgrind.h>
#define VG(x) x
#else
#define VG(x) ((void)0)
#endif

#include "c11/threads.h"
#ifndef _WIN32
#include <amdgpu.h>
#include <xf86drm.h>
#endif
#include "compiler/shader_enums.h"
#include "util/bitscan.h"
#include "util/detect_os.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/rwlock.h"
#include "util/xmlconfig.h"
#include "vk_alloc.h"
#include "vk_command_pool.h"
#include "vk_debug_report.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_shader_module.h"
#include "vk_util.h"
#include "vk_ycbcr_conversion.h"

#include "ac_binary.h"
#include "ac_gpu_info.h"
#include "ac_shader_util.h"
#include "ac_spm.h"
#include "ac_sqtt.h"
#include "ac_surface.h"
#include "ac_vcn.h"
#include "radv_cmd_buffer.h"
#include "radv_constants.h"
#include "radv_descriptor_set.h"
#include "radv_device.h"
#include "radv_physical_device.h"
#include "radv_pipeline.h"
#include "radv_pipeline_compute.h"
#include "radv_pipeline_graphics.h"
#include "radv_queue.h"
#include "radv_radeon_winsys.h"
#include "radv_rra.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
#include "sid.h"

#include "radix_sort/radix_sort_vk_devaddr.h"

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "radv_entrypoints.h"

#include "wsi_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* RADV_PRIVATE_H */
