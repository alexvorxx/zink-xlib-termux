/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "nir_builder.h"
#include "util/u_prim.h"

#include "ac_nir.h"
#include "si_pipe.h"
#include "si_query.h"
#include "si_state.h"
#include "si_shader_internal.h"

struct lower_abi_state {
   struct si_shader *shader;
   struct si_shader_args *args;
};

#define GET_FIELD_NIR(field) \
   ac_nir_unpack_arg(b, &args->ac, args->vs_state_bits, \
                     field##__SHIFT, util_bitcount(field##__MASK))

static nir_ssa_def *load_internal_binding(nir_builder *b, struct si_shader_args *args,
                                          unsigned slot)
{
   nir_ssa_def *addr = ac_nir_load_arg(b, &args->ac, args->internal_bindings);
   return nir_load_smem_amd(b, 4, addr, nir_imm_int(b, slot * 16));
}

static nir_ssa_def *get_num_vert_per_prim(nir_builder *b, struct si_shader *shader,
                                          struct si_shader_args *args)
{
   const struct si_shader_info *info = &shader->selector->info;
   gl_shader_stage stage = shader->selector->stage;

   unsigned num_vertices;
   if (stage == MESA_SHADER_GEOMETRY) {
      num_vertices = u_vertices_per_prim(info->base.gs.output_primitive);
   } else if (stage == MESA_SHADER_VERTEX) {
      if (info->base.vs.blit_sgprs_amd)
         num_vertices = 3;
      else if (shader->key.ge.opt.ngg_culling & SI_NGG_CULL_LINES)
         num_vertices = 2;
      else {
         /* Extract OUTPRIM field. */
         nir_ssa_def *num = GET_FIELD_NIR(GS_STATE_OUTPRIM);
         return nir_iadd_imm(b, num, 1);
      }
   } else {
      assert(stage == MESA_SHADER_TESS_EVAL);

      if (info->base.tess.point_mode)
         num_vertices = 1;
      else if (info->base.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         num_vertices = 2;
      else
         num_vertices = 3;
   }
   return nir_imm_int(b, num_vertices);
}

static nir_ssa_def *build_attr_ring_desc(nir_builder *b, struct si_shader *shader,
                                         struct si_shader_args *args)
{
   struct si_shader_selector *sel = shader->selector;

   nir_ssa_def *attr_address =
      sel->stage == MESA_SHADER_VERTEX && sel->info.base.vs.blit_sgprs_amd ?
      load_internal_binding(b, args, SI_GS_ATTRIBUTE_RING) :
      ac_nir_load_arg(b, &args->ac, args->gs_attr_address);

   unsigned stride = 16 * shader->info.nr_param_exports;
   nir_ssa_def *comp[] = {
      attr_address,
      nir_imm_int(b, S_008F04_BASE_ADDRESS_HI(sel->screen->info.address32_hi) |
                  S_008F04_STRIDE(stride) |
                  S_008F04_SWIZZLE_ENABLE_GFX11(3) /* 16B */),
      nir_imm_int(b, 0xffffffff),
      nir_imm_int(b, S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
                  S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
                  S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
                  S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
                  S_008F0C_FORMAT(V_008F0C_GFX11_FORMAT_32_32_32_32_FLOAT) |
                  S_008F0C_INDEX_STRIDE(2) /* 32 elements */),
   };

   return nir_vec(b, comp, 4);
}

static bool lower_abi_instr(nir_builder *b, nir_instr *instr, struct lower_abi_state *s)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   struct si_shader *shader = s->shader;
   struct si_shader_args *args = s->args;
   struct si_shader_selector *sel = shader->selector;
   union si_shader_key *key = &shader->key;
   gl_shader_stage stage = sel->stage;

   b->cursor = nir_before_instr(instr);

   nir_ssa_def *replacement = NULL;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_first_vertex:
      replacement = ac_nir_load_arg(b, &args->ac, args->ac.base_vertex);
      break;
   case nir_intrinsic_load_base_vertex: {
      nir_ssa_def *indexed = GET_FIELD_NIR(VS_STATE_INDEXED);
      indexed = nir_i2b(b, indexed);

      nir_ssa_def *base_vertex = ac_nir_load_arg(b, &args->ac, args->ac.base_vertex);
      replacement = nir_bcsel(b, indexed, base_vertex, nir_imm_int(b, 0));
      break;
   }
   case nir_intrinsic_load_workgroup_size: {
      assert(sel->info.base.workgroup_size_variable && sel->info.uses_variable_block_size);

      nir_ssa_def *block_size = ac_nir_load_arg(b, &args->ac, args->block_size);
      nir_ssa_def *comp[] = {
         nir_ubfe_imm(b, block_size, 0, 10),
         nir_ubfe_imm(b, block_size, 10, 10),
         nir_ubfe_imm(b, block_size, 20, 10),
      };
      replacement = nir_vec(b, comp, 3);
      break;
   }
   case nir_intrinsic_load_tess_level_outer_default:
   case nir_intrinsic_load_tess_level_inner_default: {
      nir_ssa_def *buf = load_internal_binding(b, args, SI_HS_CONST_DEFAULT_TESS_LEVELS);
      unsigned num_components = intrin->dest.ssa.num_components;
      unsigned offset =
         intrin->intrinsic == nir_intrinsic_load_tess_level_inner_default ? 16 : 0;
      replacement = nir_load_smem_buffer_amd(b, num_components, buf, nir_imm_int(b, offset));
      break;
   }
   case nir_intrinsic_load_patch_vertices_in:
      if (stage == MESA_SHADER_TESS_CTRL)
         replacement = ac_nir_unpack_arg(b, &args->ac, args->tcs_out_lds_layout, 13, 6);
      else if (stage == MESA_SHADER_TESS_EVAL) {
         nir_ssa_def *tmp = ac_nir_unpack_arg(b, &args->ac, args->tcs_offchip_layout, 6, 5);
         replacement = nir_iadd_imm(b, tmp, 1);
      } else
         unreachable("no nir_load_patch_vertices_in");
      break;
   case nir_intrinsic_load_sample_mask_in:
      replacement = ac_nir_load_arg(b, &args->ac, args->ac.sample_coverage);
      break;
   case nir_intrinsic_load_lshs_vertex_stride_amd:
      if (stage == MESA_SHADER_VERTEX)
         replacement = nir_imm_int(b, sel->info.lshs_vertex_stride);
      else if (stage == MESA_SHADER_TESS_CTRL)
         replacement = sel->screen->info.gfx_level >= GFX9 && shader->is_monolithic ?
            nir_imm_int(b, key->ge.part.tcs.ls->info.lshs_vertex_stride) :
            nir_ishl_imm(b, GET_FIELD_NIR(VS_STATE_LS_OUT_VERTEX_SIZE), 2);
      else
         unreachable("no nir_load_lshs_vertex_stride_amd");
      break;
   case nir_intrinsic_load_tcs_num_patches_amd: {
      nir_ssa_def *tmp = ac_nir_unpack_arg(b, &args->ac, args->tcs_offchip_layout, 0, 6);
      replacement = nir_iadd_imm(b, tmp, 1);
      break;
   }
   case nir_intrinsic_load_hs_out_patch_data_offset_amd:
      replacement = ac_nir_unpack_arg(b, &args->ac, args->tcs_offchip_layout, 11, 21);
      break;
   case nir_intrinsic_load_ring_tess_offchip_offset_amd:
      replacement = ac_nir_load_arg(b, &args->ac, args->ac.tess_offchip_offset);
      break;
   case nir_intrinsic_load_ring_es2gs_offset_amd:
      replacement = ac_nir_load_arg(b, &args->ac, args->ac.es2gs_offset);
      break;
   case nir_intrinsic_load_clip_half_line_width_amd: {
      nir_ssa_def *addr = ac_nir_load_arg(b, &args->ac, args->small_prim_cull_info);
      replacement = nir_load_smem_amd(b, 2, addr, nir_imm_int(b, 32));
      break;
   }
   case nir_intrinsic_load_viewport_xy_scale_and_offset: {
      bool prim_is_lines = key->ge.opt.ngg_culling & SI_NGG_CULL_LINES;
      nir_ssa_def *addr = ac_nir_load_arg(b, &args->ac, args->small_prim_cull_info);
      unsigned offset = prim_is_lines ? 16 : 0;
      replacement = nir_load_smem_amd(b, 4, addr, nir_imm_int(b, offset));
      break;
   }
   case nir_intrinsic_load_num_vertices_per_primitive_amd:
      replacement = get_num_vert_per_prim(b, shader, args);
      break;
   case nir_intrinsic_load_cull_ccw_amd:
      /* radeonsi embed cw/ccw info into front/back face enabled */
      replacement = nir_imm_bool(b, false);
      break;
   case nir_intrinsic_load_cull_any_enabled_amd:
      replacement = nir_imm_bool(b, !!key->ge.opt.ngg_culling);
      break;
   case nir_intrinsic_load_cull_back_face_enabled_amd:
      replacement = nir_imm_bool(b, key->ge.opt.ngg_culling & SI_NGG_CULL_BACK_FACE);
      break;
   case nir_intrinsic_load_cull_front_face_enabled_amd:
      replacement = nir_imm_bool(b, key->ge.opt.ngg_culling & SI_NGG_CULL_FRONT_FACE);
      break;
   case nir_intrinsic_load_cull_small_prim_precision_amd: {
      nir_ssa_def *small_prim_precision =
         key->ge.opt.ngg_culling & SI_NGG_CULL_LINES ?
         GET_FIELD_NIR(GS_STATE_SMALL_PRIM_PRECISION_NO_AA) :
         GET_FIELD_NIR(GS_STATE_SMALL_PRIM_PRECISION);

      /* Extract the small prim precision. */
      small_prim_precision = nir_ior_imm(b, small_prim_precision, 0x70);
      replacement = nir_ishl_imm(b, small_prim_precision, 23);
      break;
   }
   case nir_intrinsic_load_cull_small_primitives_enabled_amd: {
      unsigned mask = SI_NGG_CULL_LINES | SI_NGG_CULL_SMALL_LINES_DIAMOND_EXIT;
      replacement = nir_imm_bool(b, (key->ge.opt.ngg_culling & mask) != SI_NGG_CULL_LINES);
      break;
   }
   case nir_intrinsic_load_provoking_vtx_in_prim_amd:
      replacement = GET_FIELD_NIR(GS_STATE_PROVOKING_VTX_INDEX);
      break;
   case nir_intrinsic_load_pipeline_stat_query_enabled_amd:
      replacement = nir_i2b(b, GET_FIELD_NIR(GS_STATE_PIPELINE_STATS_EMU));
      break;
   case nir_intrinsic_load_prim_gen_query_enabled_amd:
   case nir_intrinsic_load_prim_xfb_query_enabled_amd:
      replacement = nir_i2b(b, GET_FIELD_NIR(GS_STATE_STREAMOUT_QUERY_ENABLED));
      break;
   case nir_intrinsic_load_clamp_vertex_color_amd:
      replacement = nir_i2b(b, GET_FIELD_NIR(VS_STATE_CLAMP_VERTEX_COLOR));
      break;
   case nir_intrinsic_load_user_clip_plane: {
      nir_ssa_def *buf = load_internal_binding(b, args, SI_VS_CONST_CLIP_PLANES);
      unsigned offset = nir_intrinsic_ucp_id(intrin) * 16;
      replacement = nir_load_smem_buffer_amd(b, 4, buf, nir_imm_int(b, offset));
      break;
   }
   case nir_intrinsic_load_streamout_buffer_amd: {
      unsigned slot = SI_VS_STREAMOUT_BUF0 + nir_intrinsic_base(intrin);
      replacement = load_internal_binding(b, args, slot);
      break;
   }
   case nir_intrinsic_atomic_add_gs_emit_prim_count_amd:
   case nir_intrinsic_atomic_add_gs_invocation_count_amd: {
      nir_ssa_def *buf = load_internal_binding(b, args, SI_GS_QUERY_EMULATED_COUNTERS_BUF);

      enum pipe_statistics_query_index index =
         intrin->intrinsic == nir_intrinsic_atomic_add_gs_emit_prim_count_amd ?
         PIPE_STAT_QUERY_GS_PRIMITIVES : PIPE_STAT_QUERY_GS_INVOCATIONS;
      unsigned offset = si_query_pipestat_end_dw_offset(sel->screen, index) * 4;

      nir_ssa_def *count = intrin->src[0].ssa;
      nir_buffer_atomic_add_amd(b, 32, buf, count, .base = offset);
      break;
   }
   case nir_intrinsic_atomic_add_gen_prim_count_amd:
   case nir_intrinsic_atomic_add_xfb_prim_count_amd: {
      nir_ssa_def *buf = load_internal_binding(b, args, SI_GS_QUERY_BUF);

      unsigned stream = nir_intrinsic_stream_id(intrin);
      unsigned offset = intrin->intrinsic == nir_intrinsic_atomic_add_gen_prim_count_amd ?
         offsetof(struct gfx10_sh_query_buffer_mem, stream[stream].generated_primitives) :
         offsetof(struct gfx10_sh_query_buffer_mem, stream[stream].emitted_primitives);

      nir_ssa_def *prim_count = intrin->src[0].ssa;
      nir_buffer_atomic_add_amd(b, 32, buf, prim_count, .base = offset);
      break;
   }
   case nir_intrinsic_load_ring_attr_amd:
      replacement = build_attr_ring_desc(b, shader, args);
      break;
   case nir_intrinsic_load_ring_attr_offset_amd: {
      nir_ssa_def *offset = ac_nir_unpack_arg(b, &args->ac, args->ac.gs_attr_offset, 0, 15);
      replacement = nir_ishl_imm(b, offset, 9);
      break;
   }
   default:
      return false;
   }

   if (replacement)
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, replacement);

   nir_instr_remove(instr);
   nir_instr_free(instr);

   return true;
}

bool si_nir_lower_abi(nir_shader *nir, struct si_shader *shader, struct si_shader_args *args)
{
   struct lower_abi_state state = {
      .shader = shader,
      .args = args,
   };

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder b;
   nir_builder_init(&b, impl);

   bool progress = false;
   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         progress |= lower_abi_instr(&b, instr, &state);
      }
   }

   nir_metadata preserved = progress ?
      nir_metadata_dominance | nir_metadata_block_index :
      nir_metadata_all;
   nir_metadata_preserve(impl, preserved);

   return progress;
}
