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

#ifndef RADV_PERFCOUNTER_H
#define RADV_PERFCOUNTER_H

#include "radv_radeon_winsys.h"

#include "radv_query.h"

struct radv_physical_device;
struct radv_device;

struct radv_pc_query_pool {
   struct radv_query_pool b;

   uint32_t *pc_regs;
   unsigned num_pc_regs;

   unsigned num_passes;

   unsigned num_counters;
   struct radv_perfcounter_impl *counters;
};

void radv_perfcounter_emit_shaders(struct radv_device *device, struct radeon_cmdbuf *cs, unsigned shaders);

void radv_perfcounter_emit_spm_reset(struct radeon_cmdbuf *cs);

void radv_perfcounter_emit_spm_start(struct radv_device *device, struct radeon_cmdbuf *cs, int family);

void radv_perfcounter_emit_spm_stop(struct radv_device *device, struct radeon_cmdbuf *cs, int family);

void radv_pc_deinit_query_pool(struct radv_pc_query_pool *pool);

VkResult radv_pc_init_query_pool(struct radv_physical_device *pdev, const VkQueryPoolCreateInfo *pCreateInfo,
                                 struct radv_pc_query_pool *pool);

void radv_pc_begin_query(struct radv_cmd_buffer *cmd_buffer, struct radv_pc_query_pool *pool, uint64_t va);

void radv_pc_end_query(struct radv_cmd_buffer *cmd_buffer, struct radv_pc_query_pool *pool, uint64_t va);

void radv_pc_get_results(const struct radv_pc_query_pool *pc_pool, const uint64_t *data, void *out);

#endif /* RADV_PERFCOUNTER_H */
