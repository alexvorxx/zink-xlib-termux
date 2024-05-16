/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_DESCRIPTORS_H
#define AC_DESCRIPTORS_H

#include "ac_gpu_info.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ac_sampler_state {
   unsigned address_mode_u : 3;
   unsigned address_mode_v : 3;
   unsigned address_mode_w : 3;
   unsigned max_aniso_ratio : 3;
   unsigned depth_compare_func : 3;
   unsigned unnormalized_coords : 1;
   unsigned cube_wrap : 1;
   unsigned trunc_coord : 1;
   unsigned filter_mode : 2;
   unsigned mag_filter : 2;
   unsigned min_filter : 2;
   unsigned mip_filter : 2;
   unsigned aniso_single_level : 1;
   unsigned border_color_type : 2;
   unsigned border_color_ptr : 12;
   float min_lod;
   float max_lod;
   float lod_bias;
};

void
ac_build_sampler_descriptor(const enum amd_gfx_level gfx_level,
                            const struct ac_sampler_state *state,
                            uint32_t desc[4]);

struct ac_fmask_state {
   const struct radeon_surf *surf;
   uint64_t va;
   uint32_t width : 16;
   uint32_t height : 16;
   uint32_t depth : 14;
   uint32_t type : 4;
   uint32_t first_layer : 14;
   uint32_t last_layer : 13;

   uint32_t num_samples : 5;
   uint32_t num_storage_samples : 4;
   uint32_t tc_compat_cmask : 1;
};

void
ac_build_fmask_descriptor(const enum amd_gfx_level gfx_level,
                          const struct ac_fmask_state *state,
                          uint32_t desc[8]);

#ifdef __cplusplus
}
#endif

#endif
