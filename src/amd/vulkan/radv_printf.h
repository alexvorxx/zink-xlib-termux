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

#ifndef RADV_PRINTF_H
#define RADV_PRINTF_H

#include "radv_device.h"

typedef struct nir_builder nir_builder;
typedef struct nir_def nir_def;

struct radv_printf_format {
   char *string;
   uint32_t divergence_mask;
   uint8_t element_sizes[32];
};

struct radv_printf_buffer_header {
   uint32_t offset;
   uint32_t size;
};

VkResult radv_printf_data_init(struct radv_device *device);

void radv_printf_data_finish(struct radv_device *device);

void radv_build_printf(nir_builder *b, nir_def *cond, const char *format, ...);

void radv_dump_printf_data(struct radv_device *device, FILE *out);

void radv_device_associate_nir(struct radv_device *device, nir_shader *nir);

#endif /* RADV_PRINTF_H */
