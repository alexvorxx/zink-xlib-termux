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

#endif
