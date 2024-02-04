/*
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "asahi/compiler/agx_compile.h"
#include "compiler/nir/nir_builder.h"
#include "shaders/geometry.h"
#include "util/compiler.h"
#include "agx_nir_lower_gs.h"
#include "libagx_shaders.h"
#include "nir.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "shader_enums.h"

/*
 * This file implements input assembly in software for geometry/tessellation
 * shaders. load_vertex_id is lowered based on the topology. Most of the logic
 * lives in CL library routines.
 */

/*
 * Sync with geometry.cl, this is preferred to avoid NIR needing to chew through
 * the massive switch statement (bad for compile time).
 */
nir_def *
agx_vertex_id_for_topology(nir_builder *b, nir_def *vert,
                           struct agx_ia_key *key)
{
   nir_def *prim = nir_load_primitive_id(b);
   nir_def *flatshade_first = nir_ieq_imm(b, nir_load_provoking_last(b), 0);

   switch (key->mode) {
   case MESA_PRIM_POINTS:
      return prim;

   case MESA_PRIM_LINES:
   case MESA_PRIM_TRIANGLES:
   case MESA_PRIM_LINES_ADJACENCY:
   case MESA_PRIM_TRIANGLES_ADJACENCY:
      return nir_iadd(
         b, nir_imul_imm(b, prim, mesa_vertices_per_prim(key->mode)), vert);

   case MESA_PRIM_LINE_LOOP:
      return libagx_vertex_id_for_line_loop(b, prim, vert,
                                            nir_load_num_vertices(b));

   case MESA_PRIM_LINE_STRIP:
   case MESA_PRIM_LINE_STRIP_ADJACENCY:
      return nir_iadd(b, prim, vert);

   case MESA_PRIM_TRIANGLE_STRIP: {
      return nir_iadd(
         b, prim,
         libagx_map_vertex_in_tri_strip(b, prim, vert, flatshade_first));
   }

   case MESA_PRIM_TRIANGLE_FAN:
      return libagx_vertex_id_for_tri_fan(b, prim, vert, flatshade_first);

   case MESA_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return libagx_vertex_id_for_tri_strip_adj(
         b, prim, vert, nir_load_num_vertices(b), flatshade_first);

   case MESA_PRIM_PATCHES:
      return nir_iadd(b, nir_imul(b, prim, nir_load_patch_vertices_in(b)),
                      nir_load_invocation_id(b));

   default:
      unreachable("invalid mode");
   }
}

static nir_def *
load_vertex_id(nir_builder *b, struct agx_ia_key *key)
{
   nir_def *id = agx_vertex_id_for_topology(b, NULL, key);

   /* If drawing with an index buffer, pull the vertex ID. Otherwise, the
    * vertex ID is just the index as-is.
    */
   if (key->index_size) {
      nir_def *ia = nir_load_input_assembly_buffer_agx(b);

      nir_def *address =
         libagx_index_buffer(b, ia, id, nir_imm_int(b, key->index_size));

      nir_def *index = nir_load_global_constant(b, address, key->index_size, 1,
                                                key->index_size * 8);

      id = nir_u2uN(b, index, id->bit_size);
   }

   /* Add the "start", either an index bias or a base vertex. This must happen
    * after indexing for proper index bias behaviour.
    */
   return nir_iadd(b, id, nir_load_first_vertex(b));
}

static bool
lower_vertex_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);
   assert(intr->def.bit_size == 32);
   nir_def_rewrite_uses(&intr->def, load_vertex_id(b, data));
   return true;
}

bool
agx_nir_lower_ia(nir_shader *s, struct agx_ia_key *key)
{
   return nir_shader_intrinsics_pass(
      s, lower_vertex_id, nir_metadata_block_index | nir_metadata_dominance,
      key);
}
