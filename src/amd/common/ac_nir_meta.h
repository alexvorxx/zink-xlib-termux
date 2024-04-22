/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_NIR_META_H
#define AC_NIR_META_H

#include "ac_gpu_info.h"
#include "nir.h"

union ac_ps_resolve_key {
   struct {
      bool use_aco:1;
      bool src_is_array:1;
      uint8_t log_samples:2;
      uint8_t last_src_channel:2; /* this shouldn't be greater than last_dst_channel */
      uint8_t last_dst_channel:2;
      bool x_clamp_to_edge:1;
      bool y_clamp_to_edge:1;
      bool a16:1;
      bool d16:1;
   };
   uint64_t key; /* use with hash_table_u64 */
};

/* Only immutable settings. */
struct ac_ps_resolve_options {
   const nir_shader_compiler_options *nir_options;
   const struct radeon_info *info;
   bool use_aco;     /* global driver setting */
   bool no_fmask;    /* FMASK disabled by a debug option, ignored on GFX11+ */
   bool print_key;   /* print ac_ps_resolve_key into stderr */
};

nir_shader *
ac_create_resolve_ps(const struct ac_ps_resolve_options *options,
                      const union ac_ps_resolve_key *key);

/* Universal optimized compute shader for image blits and clears. */
#define SI_MAX_COMPUTE_BLIT_LANE_SIZE  16
#define SI_MAX_COMPUTE_BLIT_SAMPLES    8

/* This describes all possible variants of the compute blit shader. */
union ac_cs_blit_key {
   struct {
      bool use_aco:1;
      /* Workgroup settings. */
      uint8_t wg_dim:2; /* 1, 2, or 3 */
      bool has_start_xyz:1;
      /* The size of a block of pixels that a single thread will process. */
      uint8_t log_lane_width:3;
      uint8_t log_lane_height:2;
      uint8_t log_lane_depth:2;
      /* Declaration modifiers. */
      bool is_clear:1;
      bool src_is_1d:1;
      bool dst_is_1d:1;
      bool src_is_msaa:1;
      bool dst_is_msaa:1;
      bool src_has_z:1;
      bool dst_has_z:1;
      bool a16:1;
      bool d16:1;
      uint8_t log_samples:2;
      bool sample0_only:1; /* src is MSAA, dst is not MSAA, log2_samples is ignored */
      /* Source coordinate modifiers. */
      bool x_clamp_to_edge:1;
      bool y_clamp_to_edge:1;
      bool flip_x:1;
      bool flip_y:1;
      /* Output modifiers. */
      bool sint_to_uint:1;
      bool uint_to_sint:1;
      bool dst_is_srgb:1;
      bool use_integer_one:1;
      uint8_t last_src_channel:2; /* this shouldn't be greater than last_dst_channel */
      uint8_t last_dst_channel:2;
   };
   uint64_t key;
};

struct ac_cs_blit_options {
   const nir_shader_compiler_options *nir_options;
   const struct radeon_info *info;
   bool use_aco;     /* global driver setting */
   bool no_fmask;    /* FMASK disabled by a debug option, ignored on GFX11+ */
   bool print_key;   /* print ac_ps_resolve_key into stderr */
};

nir_shader *
ac_create_blit_cs(const struct ac_cs_blit_options *options, const union ac_cs_blit_key *key);

#endif
