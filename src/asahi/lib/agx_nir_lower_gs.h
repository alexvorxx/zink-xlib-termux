/*
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#ifndef __AGX_NIR_LOWER_GS_H
#define __AGX_NIR_LOWER_GS_H

#include <stdbool.h>
#include <stdint.h>
#include "shader_enums.h"

struct nir_shader;
struct agx_ia_key;
enum mesa_prim;

struct nir_instr;
struct nir_builder;
struct nir_variable;

struct agx_lower_output_to_var_state {
   struct nir_variable *outputs[NUM_TOTAL_VARYING_SLOTS];
   bool arrayed;
};

bool agx_lower_output_to_var(struct nir_builder *b, struct nir_instr *instr,
                             void *data);

struct nir_def *agx_vertex_id_for_topology(struct nir_builder *b,
                                           struct nir_def *vert,
                                           struct agx_ia_key *key);

bool agx_nir_lower_ia(struct nir_shader *s, struct agx_ia_key *ia);

bool agx_nir_lower_vs_before_gs(struct nir_shader *vs,
                                const struct nir_shader *libagx,
                                unsigned index_size_B, uint64_t *outputs);

bool agx_nir_lower_gs(struct nir_shader *gs, const struct nir_shader *libagx,
                      struct agx_ia_key *ia, bool rasterizer_discard,
                      struct nir_shader **gs_count, struct nir_shader **gs_copy,
                      struct nir_shader **pre_gs, enum mesa_prim *out_mode,
                      unsigned *out_count_words);

void agx_nir_prefix_sum_gs(struct nir_builder *b, const void *data);

struct agx_gs_setup_indirect_key {
   enum mesa_prim prim;
};

void agx_nir_gs_setup_indirect(struct nir_builder *b, const void *key);

struct agx_unroll_restart_key {
   enum mesa_prim prim;
   unsigned index_size_B;
};

void agx_nir_unroll_restart(struct nir_builder *b, const void *key);

bool agx_nir_lower_tcs(struct nir_shader *tcs, const struct nir_shader *vs,
                       const struct nir_shader *libagx, uint8_t index_size_B);

bool agx_nir_lower_tes(struct nir_shader *tes, const struct nir_shader *libagx);

uint64_t agx_tcs_per_vertex_outputs(const struct nir_shader *nir);

unsigned agx_tcs_output_stride(const struct nir_shader *nir);

#endif
