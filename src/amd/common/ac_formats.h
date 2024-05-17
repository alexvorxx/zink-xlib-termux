/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_FORMATS_H
#define AC_FORMATS_H

#include "amd_family.h"

#include "util/format/u_format.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t
ac_translate_buffer_numformat(const struct util_format_description *desc,
                              int first_non_void);

uint32_t
ac_translate_buffer_dataformat(const struct util_format_description *desc,
                              int first_non_void);

uint32_t
ac_translate_tex_numformat(const struct util_format_description *desc,
                           int first_non_void);

unsigned
ac_translate_colorswap(enum amd_gfx_level gfx_level,
                       enum pipe_format format,
                       bool do_endian_swap);

#ifdef __cplusplus
}
#endif

#endif
