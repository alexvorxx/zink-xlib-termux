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

nir_ssa_def *
ac_nir_load_arg(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg)
{
   unsigned num_components = ac_args->args[arg.arg_index].size;

   if (ac_args->args[arg.arg_index].file == AC_ARG_SGPR)
      return nir_load_scalar_arg_amd(b, num_components, .base = arg.arg_index);
   else
      return nir_load_vector_arg_amd(b, num_components, .base = arg.arg_index);
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
emit_streamout(nir_builder *b, const struct pipe_stream_output_info *info, unsigned stream,
               nir_ssa_def *const outputs[64][4])
{
   nir_ssa_def *so_vtx_count = nir_ubfe_imm(b, nir_load_streamout_config_amd(b), 16, 7);
   nir_ssa_def *tid = nir_load_subgroup_invocation(b);

   nir_push_if(b, nir_ilt(b, tid, so_vtx_count));
   nir_ssa_def *so_write_index = nir_load_streamout_write_index_amd(b);

   nir_ssa_def *so_buffers[PIPE_MAX_SO_BUFFERS];
   nir_ssa_def *so_write_offset[PIPE_MAX_SO_BUFFERS];
   for (unsigned i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
      uint16_t stride = info->stride[i];
      if (!stride)
         continue;

      so_buffers[i] = nir_load_streamout_buffer_amd(b, i);

      nir_ssa_def *offset = nir_load_streamout_offset_amd(b, i);
      offset = nir_iadd(b, nir_imul_imm(b, nir_iadd(b, so_write_index, tid), stride * 4),
                        nir_imul_imm(b, offset, 4));
      so_write_offset[i] = offset;
   }

   nir_ssa_def *undef = nir_ssa_undef(b, 1, 32);
   for (unsigned i = 0; i < info->num_outputs; i++) {
      const struct pipe_stream_output *output = &info->output[i];
      if (stream != output->stream)
         continue;

      nir_ssa_def *vec[4] = {undef, undef, undef, undef};
      uint8_t mask = 0;
      for (unsigned j = 0; j < output->num_components; j++) {
         if (outputs[output->register_index][output->start_component + j]) {
            vec[j] = outputs[output->register_index][output->start_component + j];
            mask |= 1 << j;
         }
      }

      if (!mask)
         continue;

      unsigned buffer = output->output_buffer;
      nir_ssa_def *data = nir_vec(b, vec, output->num_components);
      nir_ssa_def *zero = nir_imm_int(b, 0);
      nir_store_buffer_amd(b, data, so_buffers[buffer], so_write_offset[buffer], zero, zero,
                           .base = output->dst_offset * 4, .slc_amd = true, .write_mask = mask,
                           .access = ACCESS_COHERENT);
   }

   nir_pop_if(b, NULL);
}

nir_shader *
ac_nir_create_gs_copy_shader(const nir_shader *gs_nir,
                             const struct pipe_stream_output_info *so_info, size_t num_outputs,
                             const uint8_t *output_usage_mask, const uint8_t *output_streams,
                             const uint8_t *output_semantics,
                             const uint8_t num_stream_output_components[4])
{
   assert(num_outputs <= 64);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, gs_nir->options, "gs_copy");

   nir_foreach_shader_out_variable(var, gs_nir)
      nir_shader_add_variable(b.shader, nir_variable_clone(var, b.shader));

   nir_ssa_def *gsvs_ring = nir_load_ring_gsvs_amd(&b);

   nir_ssa_def *stream_id = NULL;
   if (so_info->num_outputs)
      stream_id = nir_ubfe_imm(&b, nir_load_streamout_config_amd(&b), 24, 2);

   nir_ssa_def *vtx_offset = nir_imul_imm(&b, nir_load_vertex_id_zero_base(&b), 4);
   nir_ssa_def *undef = nir_ssa_undef(&b, 1, 32);
   nir_ssa_def *zero = nir_imm_zero(&b, 1, 32);

   for (unsigned stream = 0; stream < 4; stream++) {
      if (stream > 0 && (!stream_id || !num_stream_output_components[stream]))
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

         u_foreach_bit (j, mask) {
            if (((output_streams[i] >> (j * 2)) & 0x3) != stream)
               continue;

            outputs[i][j] = nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                                .base = offset, .is_swizzled = false,
                                                .slc_amd = true, .access = ACCESS_COHERENT);

            offset += gs_nir->info.gs.vertices_out * 16 * 4;
         }

         output_mask |= 1ull << i;
      }

      if (stream_id)
         emit_streamout(&b, so_info, stream, outputs);

      if (stream == 0) {
         u_foreach_bit64 (i, output_mask) {
            uint8_t mask = 0;
            nir_ssa_def *vec[4];
            for (unsigned j = 0; j < 4; j++) {
               vec[j] = outputs[i][j] ? outputs[i][j] : undef;
               mask |= (outputs[i][j] ? 1 : 0) << j;
            }

            gl_varying_slot location = output_semantics ? output_semantics[i] : i;
            nir_store_output(&b, nir_vec(&b, vec, 4), zero, .base = i, .write_mask = mask,
                             .src_type = nir_type_uint32,
                             .io_semantics = {.location = location, .num_slots = 1});
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

         unsigned slot = nir_intrinsic_base(intrin);
         u_foreach_bit (i, nir_intrinsic_write_mask(intrin)) {
            unsigned comp = nir_intrinsic_component(intrin) + i;
            outputs[slot][comp] = nir_channel(b, intrin->src[0].ssa, i);
         }
      }
   }
}

void
ac_nir_lower_legacy_vs(nir_shader *nir, int primitive_id_location,
                       const struct pipe_stream_output_info *so_info)
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

   if (so_info && so_info->num_outputs) {
      /* 26.1. Transform Feedback of Vulkan 1.3.229 spec:
       * > The size of each component of an output variable must be at least 32-bits.
       * We lower 64-bit outputs.
       */
      nir_ssa_def *outputs[64][4] = {{0}};
      gather_outputs(&b, impl, outputs);

      emit_streamout(&b, so_info, 0, outputs);
      preserved = nir_metadata_none;
   }

   nir_export_vertex_amd(&b);
   nir_metadata_preserve(impl, preserved);
}
