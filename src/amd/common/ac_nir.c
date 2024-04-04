/*
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "ac_nir.h"
#include "nir_builder.h"
#include "nir_xfb_info.h"

nir_ssa_def *
ac_nir_load_arg(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg)
{
   unsigned num_components = ac_args->args[arg.arg_index].size;

   if (ac_args->args[arg.arg_index].file == AC_ARG_SGPR)
      return nir_load_scalar_arg_amd(b, num_components, .base = arg.arg_index);
   else
      return nir_load_vector_arg_amd(b, num_components, .base = arg.arg_index);
}

nir_ssa_def *
ac_nir_unpack_arg(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg,
                  unsigned rshift, unsigned bitwidth)
{
   nir_ssa_def *value = ac_nir_load_arg(b, ac_args, arg);
   return nir_ubfe_imm(b, value, rshift, bitwidth);
}

/**
 * This function takes an I/O intrinsic like load/store_input,
 * and emits a sequence that calculates the full offset of that instruction,
 * including a stride to the base and component offsets.
 */
nir_ssa_def *
ac_nir_calc_io_offset(nir_builder *b,
                      nir_intrinsic_instr *intrin,
                      nir_ssa_def *base_stride,
                      unsigned component_stride,
                      ac_nir_map_io_driver_location map_io)
{
   unsigned base = nir_intrinsic_base(intrin);
   unsigned semantic = nir_intrinsic_io_semantics(intrin).location;
   unsigned mapped_driver_location = map_io ? map_io(semantic) : base;

   /* base is the driver_location, which is in slots (1 slot = 4x4 bytes) */
   nir_ssa_def *base_op = nir_imul_imm(b, base_stride, mapped_driver_location);

   /* offset should be interpreted in relation to the base,
    * so the instruction effectively reads/writes another input/output
    * when it has an offset
    */
   nir_ssa_def *offset_op = nir_imul(b, base_stride, nir_ssa_for_src(b, *nir_get_io_offset_src(intrin), 1));

   /* component is in bytes */
   unsigned const_op = nir_intrinsic_component(intrin) * component_stride;

   return nir_iadd_imm_nuw(b, nir_iadd_nuw(b, base_op, offset_op), const_op);
}

bool
ac_nir_lower_indirect_derefs(nir_shader *shader,
                             enum amd_gfx_level gfx_level)
{
   bool progress = false;

   /* Lower large variables to scratch first so that we won't bloat the
    * shader by generating large if ladders for them. We later lower
    * scratch to alloca's, assuming LLVM won't generate VGPR indexing.
    */
   NIR_PASS(progress, shader, nir_lower_vars_to_scratch, nir_var_function_temp, 256,
            glsl_get_natural_size_align_bytes);

   /* LLVM doesn't support VGPR indexing on GFX9. */
   bool llvm_has_working_vgpr_indexing = gfx_level != GFX9;

   /* TODO: Indirect indexing of GS inputs is unimplemented.
    *
    * TCS and TES load inputs directly from LDS or offchip memory, so
    * indirect indexing is trivial.
    */
   nir_variable_mode indirect_mask = 0;
   if (shader->info.stage == MESA_SHADER_GEOMETRY ||
       (shader->info.stage != MESA_SHADER_TESS_CTRL && shader->info.stage != MESA_SHADER_TESS_EVAL &&
        !llvm_has_working_vgpr_indexing)) {
      indirect_mask |= nir_var_shader_in;
   }
   if (!llvm_has_working_vgpr_indexing && shader->info.stage != MESA_SHADER_TESS_CTRL)
      indirect_mask |= nir_var_shader_out;

   /* TODO: We shouldn't need to do this, however LLVM isn't currently
    * smart enough to handle indirects without causing excess spilling
    * causing the gpu to hang.
    *
    * See the following thread for more details of the problem:
    * https://lists.freedesktop.org/archives/mesa-dev/2017-July/162106.html
    */
   indirect_mask |= nir_var_function_temp;

   NIR_PASS(progress, shader, nir_lower_indirect_derefs, indirect_mask, UINT32_MAX);
   return progress;
}

static void
emit_streamout(nir_builder *b, unsigned stream, nir_xfb_info *info,
               nir_ssa_def *const outputs[64][4])
{
   nir_ssa_def *so_vtx_count = nir_ubfe_imm(b, nir_load_streamout_config_amd(b), 16, 7);
   nir_ssa_def *tid = nir_load_subgroup_invocation(b);

   nir_push_if(b, nir_ilt(b, tid, so_vtx_count));
   nir_ssa_def *so_write_index = nir_load_streamout_write_index_amd(b);

   nir_ssa_def *so_buffers[NIR_MAX_XFB_BUFFERS];
   nir_ssa_def *so_write_offset[NIR_MAX_XFB_BUFFERS];
   u_foreach_bit(i, info->buffers_written) {
      so_buffers[i] = nir_load_streamout_buffer_amd(b, i);

      unsigned stride = info->buffers[i].stride;
      nir_ssa_def *offset = nir_load_streamout_offset_amd(b, i);
      offset = nir_iadd(b, nir_imul_imm(b, nir_iadd(b, so_write_index, tid), stride),
                        nir_imul_imm(b, offset, 4));
      so_write_offset[i] = offset;
   }

   nir_ssa_def *undef = nir_ssa_undef(b, 1, 32);
   for (unsigned i = 0; i < info->output_count; i++) {
      const nir_xfb_output_info *output = info->outputs + i;
      if (stream != info->buffer_to_stream[output->buffer])
         continue;

      nir_ssa_def *vec[4] = {undef, undef, undef, undef};
      uint8_t mask = 0;
      u_foreach_bit(j, output->component_mask) {
         nir_ssa_def *src = outputs[output->location][j];
         if (src) {
            unsigned comp = j - output->component_offset;
            vec[comp] = src;
            mask |= 1 << comp;
         }
      }

      if (!mask)
         continue;

      unsigned buffer = output->buffer;
      nir_ssa_def *data = nir_vec(b, vec, util_last_bit(mask));
      nir_ssa_def *zero = nir_imm_int(b, 0);
      nir_store_buffer_amd(b, data, so_buffers[buffer], so_write_offset[buffer], zero, zero,
                           .base = output->offset, .write_mask = mask,
                           .access = ACCESS_COHERENT | ACCESS_STREAM_CACHE_POLICY);
   }

   nir_pop_if(b, NULL);
}

nir_shader *
ac_nir_create_gs_copy_shader(const nir_shader *gs_nir,
                             bool disable_streamout,
                             size_t num_outputs,
                             const uint8_t *output_usage_mask,
                             const uint8_t *output_streams,
                             const uint8_t *output_semantics)
{
   assert(num_outputs <= 64);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, gs_nir->options, "gs_copy");

   nir_foreach_shader_out_variable(var, gs_nir)
      nir_shader_add_variable(b.shader, nir_variable_clone(var, b.shader));

   nir_ssa_def *gsvs_ring = nir_load_ring_gsvs_amd(&b);

   nir_xfb_info *info = gs_nir->xfb_info;
   nir_ssa_def *stream_id = NULL;
   if (!disable_streamout && info)
      stream_id = nir_ubfe_imm(&b, nir_load_streamout_config_amd(&b), 24, 2);

   nir_ssa_def *vtx_offset = nir_imul_imm(&b, nir_load_vertex_id_zero_base(&b), 4);
   nir_ssa_def *zero = nir_imm_zero(&b, 1, 32);

   for (unsigned stream = 0; stream < 4; stream++) {
      if (stream > 0 && (!stream_id || !(info->streams_written & BITFIELD_BIT(stream))))
         continue;

      if (stream_id)
         nir_push_if(&b, nir_ieq_imm(&b, stream_id, stream));

      uint32_t offset = 0;
      uint64_t output_mask = 0;
      nir_ssa_def *outputs[64][4] = {{0}};
      for (unsigned i = 0; i < num_outputs; i++) {
         unsigned mask = output_usage_mask[i];
         if (!mask)
            continue;

         gl_varying_slot location = output_semantics ? output_semantics[i] : i;

         u_foreach_bit (j, mask) {
            if (((output_streams[i] >> (j * 2)) & 0x3) != stream)
               continue;

            outputs[location][j] =
               nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                   .base = offset,
                                   .access = ACCESS_COHERENT | ACCESS_STREAM_CACHE_POLICY);

            offset += gs_nir->info.gs.vertices_out * 16 * 4;
         }

         output_mask |= 1ull << i;
      }

      if (stream_id)
         emit_streamout(&b, stream, info, outputs);

      if (stream == 0) {
         u_foreach_bit64 (i, output_mask) {
            gl_varying_slot location = output_semantics ? output_semantics[i] : i;

            for (unsigned j = 0; j < 4; j++) {
               if (outputs[location][j]) {
                  nir_store_output(&b, outputs[location][j], zero,
                                   .base = i,
                                   .component = j,
                                   .write_mask = 1,
                                   .src_type = nir_type_uint32,
                                   .io_semantics = {.location = location, .num_slots = 1});
               }
            }
         }

         nir_export_vertex_amd(&b);
      }

      if (stream_id)
         nir_push_else(&b, NULL);
   }

   b.shader->info.clip_distance_array_size = gs_nir->info.clip_distance_array_size;
   b.shader->info.cull_distance_array_size = gs_nir->info.cull_distance_array_size;

   return b.shader;
}

static void
gather_outputs(nir_builder *b, nir_function_impl *impl, nir_ssa_def *outputs[64][4])
{
   /* Assume:
    * - the shader used nir_lower_io_to_temporaries
    * - 64-bit outputs are lowered
    * - no indirect indexing is present
    */
   nir_foreach_block(block, impl) {
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_store_output)
            continue;

         assert(nir_src_is_const(intrin->src[1]) && !nir_src_as_uint(intrin->src[1]));

         unsigned slot = nir_intrinsic_io_semantics(intrin).location;
         u_foreach_bit (i, nir_intrinsic_write_mask(intrin)) {
            unsigned comp = nir_intrinsic_component(intrin) + i;
            outputs[slot][comp] = nir_channel(b, intrin->src[0].ssa, i);
         }
      }
   }
}

void
ac_nir_lower_legacy_vs(nir_shader *nir, int primitive_id_location, bool disable_streamout)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_metadata preserved = nir_metadata_block_index | nir_metadata_dominance;

   nir_builder b;
   nir_builder_init(&b, impl);
   b.cursor = nir_after_cf_list(&impl->body);

   if (primitive_id_location >= 0) {
      /* When the primitive ID is read by FS, we must ensure that it's exported by the previous
       * vertex stage because it's implicit for VS or TES (but required by the Vulkan spec for GS
       * or MS).
       */
      nir_variable *var = nir_variable_create(nir, nir_var_shader_out, glsl_int_type(), NULL);
      var->data.location = VARYING_SLOT_PRIMITIVE_ID;
      var->data.interpolation = INTERP_MODE_NONE;
      var->data.driver_location = primitive_id_location;

      nir_store_output(
         &b, nir_load_primitive_id(&b), nir_imm_int(&b, 0), .base = primitive_id_location,
         .src_type = nir_type_int32,
         .io_semantics = (nir_io_semantics){.location = var->data.location, .num_slots = 1});

      /* Update outputs_written to reflect that the pass added a new output. */
      nir->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_ID);
   }

   if (!disable_streamout && nir->xfb_info) {
      /* 26.1. Transform Feedback of Vulkan 1.3.229 spec:
       * > The size of each component of an output variable must be at least 32-bits.
       * We lower 64-bit outputs.
       */
      nir_ssa_def *outputs[64][4] = {{0}};
      gather_outputs(&b, impl, outputs);

      emit_streamout(&b, 0, nir->xfb_info, outputs);
      preserved = nir_metadata_none;
   }

   nir_export_vertex_amd(&b);
   nir_metadata_preserve(impl, preserved);
}

bool
ac_nir_gs_shader_query(nir_builder *b,
                       bool has_gen_prim_query,
                       bool has_pipeline_stats_query,
                       unsigned num_vertices_per_primitive,
                       unsigned wave_size,
                       nir_ssa_def *vertex_count[4],
                       nir_ssa_def *primitive_count[4])
{
   nir_ssa_def *pipeline_query_enabled = NULL;
   nir_ssa_def *prim_gen_query_enabled = NULL;
   nir_ssa_def *shader_query_enabled = NULL;
   if (has_gen_prim_query) {
      prim_gen_query_enabled = nir_load_prim_gen_query_enabled_amd(b);
      if (has_pipeline_stats_query) {
         pipeline_query_enabled = nir_load_pipeline_stat_query_enabled_amd(b);
         shader_query_enabled = nir_ior(b, pipeline_query_enabled, prim_gen_query_enabled);
      } else {
         shader_query_enabled = prim_gen_query_enabled;
      }
   } else if (has_pipeline_stats_query) {
      pipeline_query_enabled = nir_load_pipeline_stat_query_enabled_amd(b);
      shader_query_enabled = pipeline_query_enabled;
   } else {
      /* has no query */
      return false;
   }

   nir_if *if_shader_query = nir_push_if(b, shader_query_enabled);

   nir_ssa_def *active_threads_mask = nir_ballot(b, 1, wave_size, nir_imm_bool(b, true));
   nir_ssa_def *num_active_threads = nir_bit_count(b, active_threads_mask);

   /* Calculate the "real" number of emitted primitives from the emitted GS vertices and primitives.
    * GS emits points, line strips or triangle strips.
    * Real primitives are points, lines or triangles.
    */
   nir_ssa_def *num_prims_in_wave[4] = {0};
   u_foreach_bit (i, b->shader->info.gs.active_stream_mask) {
      assert(vertex_count[i] && primitive_count[i]);

      nir_ssa_scalar vtx_cnt = nir_get_ssa_scalar(vertex_count[i], 0);
      nir_ssa_scalar prm_cnt = nir_get_ssa_scalar(primitive_count[i], 0);

      if (nir_ssa_scalar_is_const(vtx_cnt) && nir_ssa_scalar_is_const(prm_cnt)) {
         unsigned gs_vtx_cnt = nir_ssa_scalar_as_uint(vtx_cnt);
         unsigned gs_prm_cnt = nir_ssa_scalar_as_uint(prm_cnt);
         unsigned total_prm_cnt = gs_vtx_cnt - gs_prm_cnt * (num_vertices_per_primitive - 1u);
         if (total_prm_cnt == 0)
            continue;

         num_prims_in_wave[i] = nir_imul_imm(b, num_active_threads, total_prm_cnt);
      } else {
         nir_ssa_def *gs_vtx_cnt = vtx_cnt.def;
         nir_ssa_def *gs_prm_cnt = prm_cnt.def;
         if (num_vertices_per_primitive > 1)
            gs_prm_cnt = nir_iadd(b, nir_imul_imm(b, gs_prm_cnt, -1u * (num_vertices_per_primitive - 1)), gs_vtx_cnt);
         num_prims_in_wave[i] = nir_reduce(b, gs_prm_cnt, .reduction_op = nir_op_iadd);
      }
   }

   /* Store the query result to query result using an atomic add. */
   nir_if *if_first_lane = nir_push_if(b, nir_elect(b, 1));
   {
      if (has_pipeline_stats_query) {
         nir_if *if_pipeline_query = nir_push_if(b, pipeline_query_enabled);
         {
            nir_ssa_def *count = NULL;

            /* Add all streams' number to the same counter. */
            for (int i = 0; i < 4; i++) {
               if (num_prims_in_wave[i]) {
                  if (count)
                     count = nir_iadd(b, count, num_prims_in_wave[i]);
                  else
                     count = num_prims_in_wave[i];
               }
            }

            if (count)
               nir_atomic_add_gs_emit_prim_count_amd(b, count);

            nir_atomic_add_gs_invocation_count_amd(b, num_active_threads);
         }
         nir_pop_if(b, if_pipeline_query);
      }

      if (has_gen_prim_query) {
         nir_if *if_prim_gen_query = nir_push_if(b, prim_gen_query_enabled);
         {
            /* Add to the counter for this stream. */
            for (int i = 0; i < 4; i++) {
               if (num_prims_in_wave[i])
                  nir_atomic_add_gen_prim_count_amd(b, num_prims_in_wave[i], .stream_id = i);
            }
         }
         nir_pop_if(b, if_prim_gen_query);
      }
   }
   nir_pop_if(b, if_first_lane);

   nir_pop_if(b, if_shader_query);
   return true;
}

typedef struct {
   nir_ssa_def *outputs[64][4];
   nir_ssa_def *outputs_16bit_lo[16][4];
   nir_ssa_def *outputs_16bit_hi[16][4];

   ac_nir_gs_output_info *info;

   nir_ssa_def *vertex_count[4];
   nir_ssa_def *primitive_count[4];
} lower_legacy_gs_state;

static bool
lower_legacy_gs_store_output(nir_builder *b, nir_intrinsic_instr *intrin,
                             lower_legacy_gs_state *s)
{
   /* Assume:
    * - the shader used nir_lower_io_to_temporaries
    * - 64-bit outputs are lowered
    * - no indirect indexing is present
    */
   assert(nir_src_is_const(intrin->src[1]) && !nir_src_as_uint(intrin->src[1]));

   b->cursor = nir_before_instr(&intrin->instr);

   unsigned component = nir_intrinsic_component(intrin);
   unsigned write_mask = nir_intrinsic_write_mask(intrin);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);

   nir_ssa_def **outputs;
   if (sem.location < VARYING_SLOT_VAR0_16BIT) {
      outputs = s->outputs[sem.location];
   } else {
      unsigned index = sem.location - VARYING_SLOT_VAR0_16BIT;
      if (sem.high_16bits)
         outputs = s->outputs_16bit_hi[index];
      else
         outputs = s->outputs_16bit_lo[index];
   }

   nir_ssa_def *store_val = intrin->src[0].ssa;
   /* 64bit output has been lowered to 32bit */
   assert(store_val->bit_size <= 32);

   u_foreach_bit (i, write_mask) {
      unsigned comp = component + i;
      outputs[comp] = nir_channel(b, store_val, i);
   }

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_legacy_gs_emit_vertex_with_counter(nir_builder *b, nir_intrinsic_instr *intrin,
                                         lower_legacy_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned stream = nir_intrinsic_stream_id(intrin);
   nir_ssa_def *vtxidx = intrin->src[0].ssa;

   nir_ssa_def *gsvs_ring = nir_load_ring_gsvs_amd(b, .stream_id = stream);
   nir_ssa_def *soffset = nir_load_ring_gs2vs_offset_amd(b);

   unsigned offset = 0;
   u_foreach_bit64 (i, b->shader->info.outputs_written) {
      for (unsigned j = 0; j < 4; j++) {
         nir_ssa_def *output = s->outputs[i][j];
         /* Next vertex emit need a new value, reset all outputs. */
         s->outputs[i][j] = NULL;

         if (!(s->info->usage_mask[i] & (1 << j)) ||
             ((s->info->streams[i] >> (j * 2)) & 0x3) != stream)
            continue;

         unsigned base = offset * b->shader->info.gs.vertices_out;
         offset++;

         /* no one set this output, skip the buffer store */
         if (!output)
            continue;

         nir_ssa_def *voffset = nir_iadd_imm(b, vtxidx, base);
         voffset = nir_ishl_imm(b, voffset, 2);

         /* extend 8/16 bit to 32 bit, 64 bit has been lowered */
         nir_ssa_def *data = nir_u2uN(b, output, 32);

         nir_store_buffer_amd(b, data, gsvs_ring, voffset, soffset, nir_imm_int(b, 0),
                              .access = ACCESS_COHERENT | ACCESS_STREAM_CACHE_POLICY |
                                        ACCESS_IS_SWIZZLED_AMD,
                              /* For ACO to not reorder this store around EmitVertex/EndPrimitve */
                              .memory_modes = nir_var_shader_out);
      }
   }

   u_foreach_bit (i, b->shader->info.outputs_written_16bit) {
      for (unsigned j = 0; j < 4; j++) {
         nir_ssa_def *output_lo = s->outputs_16bit_lo[i][j];
         nir_ssa_def *output_hi = s->outputs_16bit_hi[i][j];
         /* Next vertex emit need a new value, reset all outputs. */
         s->outputs_16bit_lo[i][j] = NULL;
         s->outputs_16bit_hi[i][j] = NULL;

         bool has_lo_16bit = (s->info->usage_mask_16bit_lo[i] & (1 << j)) &&
            ((s->info->streams_16bit_lo[i] >> (j * 2)) & 0x3) == stream;
         bool has_hi_16bit = (s->info->usage_mask_16bit_hi[i] & (1 << j)) &&
            ((s->info->streams_16bit_hi[i] >> (j * 2)) & 0x3) == stream;
         if (!has_lo_16bit && !has_hi_16bit)
            continue;

         unsigned base = offset * b->shader->info.gs.vertices_out;
         offset++;

         bool has_lo_16bit_out = has_lo_16bit && output_lo;
         bool has_hi_16bit_out = has_hi_16bit && output_hi;

         /* no one set needed output, skip the buffer store */
         if (!has_lo_16bit_out && !has_hi_16bit_out)
            continue;

         if (!has_lo_16bit_out)
            output_lo = nir_ssa_undef(b, 1, 16);

         if (!has_hi_16bit_out)
            output_hi = nir_ssa_undef(b, 1, 16);

         nir_ssa_def *voffset = nir_iadd_imm(b, vtxidx, base);
         voffset = nir_ishl_imm(b, voffset, 2);

         nir_store_buffer_amd(b, nir_pack_32_2x16_split(b, output_lo, output_hi),
                              gsvs_ring, voffset, soffset, nir_imm_int(b, 0),
                              .access = ACCESS_COHERENT | ACCESS_STREAM_CACHE_POLICY |
                                        ACCESS_IS_SWIZZLED_AMD,
                              /* For ACO to not reorder this store around EmitVertex/EndPrimitve */
                              .memory_modes = nir_var_shader_out);
      }
   }

   /* Keep this instruction to signal vertex emission. */

   return true;
}

static bool
lower_legacy_gs_set_vertex_and_primitive_count(nir_builder *b, nir_intrinsic_instr *intrin,
                                               lower_legacy_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned stream = nir_intrinsic_stream_id(intrin);

   s->vertex_count[stream] = intrin->src[0].ssa;
   s->primitive_count[stream] = intrin->src[1].ssa;

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_legacy_gs_intrinsic(nir_builder *b, nir_instr *instr, void *state)
{
   lower_legacy_gs_state *s = (lower_legacy_gs_state *) state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic == nir_intrinsic_store_output)
      return lower_legacy_gs_store_output(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_emit_vertex_with_counter)
      return lower_legacy_gs_emit_vertex_with_counter(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_set_vertex_and_primitive_count)
      return lower_legacy_gs_set_vertex_and_primitive_count(b, intrin, s);

   return false;
}

void
ac_nir_lower_legacy_gs(nir_shader *nir,
                       bool has_gen_prim_query,
                       bool has_pipeline_stats_query,
                       ac_nir_gs_output_info *output_info)
{
   lower_legacy_gs_state s = {
      .info = output_info,
   };

   unsigned num_vertices_per_primitive = 0;
   switch (nir->info.gs.output_primitive) {
   case SHADER_PRIM_POINTS:
      num_vertices_per_primitive = 1;
      break;
   case SHADER_PRIM_LINE_STRIP:
      num_vertices_per_primitive = 2;
      break;
   case SHADER_PRIM_TRIANGLE_STRIP:
      num_vertices_per_primitive = 3;
      break;
   default:
      unreachable("Invalid GS output primitive.");
      break;
   }

   nir_shader_instructions_pass(nir, lower_legacy_gs_intrinsic,
                                nir_metadata_block_index | nir_metadata_dominance, &s);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder builder;
   nir_builder *b = &builder;
   nir_builder_init(b, impl);

   b->cursor = nir_after_cf_list(&impl->body);

   /* Emit shader query for mix use legacy/NGG GS */
   bool progress = ac_nir_gs_shader_query(b,
                                          has_gen_prim_query,
                                          has_pipeline_stats_query,
                                          num_vertices_per_primitive,
                                          64,
                                          s.vertex_count,
                                          s.primitive_count);
   if (progress)
      nir_metadata_preserve(impl, nir_metadata_none);
}
