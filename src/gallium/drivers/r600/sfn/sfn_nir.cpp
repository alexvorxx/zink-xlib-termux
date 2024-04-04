/* -*- mesa-c++  -*-
 *
 * Copyright (c) 2019 Collabora LTD
 *
 * Author: Gert Wollny <gert.wollny@collabora.com>
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

#include "sfn_nir.h"

#include "../r600_pipe.h"
#include "../r600_shader.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir_intrinsics.h"
#include "sfn_assembler.h"
#include "sfn_debug.h"
#include "sfn_instr_tex.h"
#include "sfn_liverangeevaluator.h"
#include "sfn_nir_lower_alu.h"
#include "sfn_nir_lower_fs_out_to_vector.h"
#include "sfn_nir_lower_tex.h"
#include "sfn_optimizer.h"
#include "sfn_ra.h"
#include "sfn_scheduler.h"
#include "sfn_shader.h"
#include "util/u_prim.h"

#include <vector>

namespace r600 {

using std::vector;

NirLowerInstruction::NirLowerInstruction():
    b(nullptr)
{
}

bool
NirLowerInstruction::filter_instr(const nir_instr *instr, const void *data)
{
   auto me = reinterpret_cast<const NirLowerInstruction *>(data);
   return me->filter(instr);
}

nir_ssa_def *
NirLowerInstruction::lower_instr(nir_builder *b, nir_instr *instr, void *data)
{
   auto me = reinterpret_cast<NirLowerInstruction *>(data);
   me->set_builder(b);
   return me->lower(instr);
}

bool
NirLowerInstruction::run(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader, filter_instr, lower_instr, (void *)this);
}

AssemblyFromShader::~AssemblyFromShader() {}

bool
AssemblyFromShader::lower(const Shader& ir)
{
   return do_lower(ir);
}

static void
r600_nir_lower_scratch_address_impl(nir_builder *b, nir_intrinsic_instr *instr)
{
   b->cursor = nir_before_instr(&instr->instr);

   int address_index = 0;
   int align;

   if (instr->intrinsic == nir_intrinsic_store_scratch) {
      align = instr->src[0].ssa->num_components;
      address_index = 1;
   } else {
      align = instr->dest.ssa.num_components;
   }

   nir_ssa_def *address = instr->src[address_index].ssa;
   nir_ssa_def *new_address = nir_ishr(b, address, nir_imm_int(b, 4 * align));

   nir_instr_rewrite_src(&instr->instr,
                         &instr->src[address_index],
                         nir_src_for_ssa(new_address));
}

bool
r600_lower_scratch_addresses(nir_shader *shader)
{
   bool progress = false;
   nir_foreach_function(function, shader)
   {
      nir_builder build;
      nir_builder_init(&build, function->impl);

      nir_foreach_block(block, function->impl)
      {
         nir_foreach_instr(instr, block)
         {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *op = nir_instr_as_intrinsic(instr);
            if (op->intrinsic != nir_intrinsic_load_scratch &&
                op->intrinsic != nir_intrinsic_store_scratch)
               continue;
            r600_nir_lower_scratch_address_impl(&build, op);
            progress = true;
         }
      }
   }
   return progress;
}

static void
insert_uniform_sorted(struct exec_list *var_list, nir_variable *new_var)
{
   nir_foreach_variable_in_list(var, var_list)
   {
      if (var->data.binding > new_var->data.binding ||
          (var->data.binding == new_var->data.binding &&
           var->data.offset > new_var->data.offset)) {
         exec_node_insert_node_before(&var->node, &new_var->node);
         return;
      }
   }
   exec_list_push_tail(var_list, &new_var->node);
}

void
sort_uniforms(nir_shader *shader)
{
   struct exec_list new_list;
   exec_list_make_empty(&new_list);

   nir_foreach_uniform_variable_safe(var, shader)
   {
      exec_node_remove(&var->node);
      insert_uniform_sorted(&new_list, var);
   }
   exec_list_append(&shader->variables, &new_list);
}

static void
insert_fsoutput_sorted(struct exec_list *var_list, nir_variable *new_var)
{

   nir_foreach_variable_in_list(var, var_list)
   {
      if ((var->data.location >= FRAG_RESULT_DATA0 ||
          var->data.location == FRAG_RESULT_COLOR) &&
          (new_var->data.location < FRAG_RESULT_COLOR ||
           new_var->data.location == FRAG_RESULT_SAMPLE_MASK)) {
         exec_node_insert_after(&var->node, &new_var->node);
         return;
      } else if ((new_var->data.location >= FRAG_RESULT_DATA0 ||
                  new_var->data.location == FRAG_RESULT_COLOR) &&
                 (var->data.location < FRAG_RESULT_COLOR ||
                  var->data.location == FRAG_RESULT_SAMPLE_MASK)) {
         exec_node_insert_node_before(&var->node, &new_var->node);
         return;
      } else if (var->data.location > new_var->data.location ||
          (var->data.location == new_var->data.location &&
           var->data.index > new_var->data.index)) {
         exec_node_insert_node_before(&var->node, &new_var->node);
         return;
      }
   }

   exec_list_push_tail(var_list, &new_var->node);
}

void
sort_fsoutput(nir_shader *shader)
{
   struct exec_list new_list;
   exec_list_make_empty(&new_list);

   nir_foreach_shader_out_variable_safe(var, shader)
   {
      exec_node_remove(&var->node);
      insert_fsoutput_sorted(&new_list, var);
   }

   unsigned driver_location = 0;
   nir_foreach_variable_in_list(var, &new_list) var->data.driver_location =
      driver_location++;

   exec_list_append(&shader->variables, &new_list);
}

class LowerClipvertexWrite : public NirLowerInstruction {

public:
   LowerClipvertexWrite(int noutputs, pipe_stream_output_info& so_info):
       m_clipplane1(noutputs),
       m_clipvtx(noutputs + 1),
       m_so_info(so_info)
   {
   }

private:
   bool filter(const nir_instr *instr) const override
   {
      if (instr->type != nir_instr_type_intrinsic)
         return false;

      auto intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_store_output)
         return false;

      return nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_CLIP_VERTEX;
   }

   nir_ssa_def *lower(nir_instr *instr) override
   {

      auto intr = nir_instr_as_intrinsic(instr);
      nir_ssa_def *output[8] = {nullptr};

      auto buf_id = nir_imm_int(b, R600_BUFFER_INFO_CONST_BUFFER);

      assert(intr->src[0].is_ssa);
      auto clip_vtx = intr->src[0].ssa;

      for (int i = 0; i < 8; ++i) {
         auto sel = nir_imm_int(b, i);
         auto mrow = nir_load_ubo_vec4(b, 4, 32, buf_id, sel);
         output[i] = nir_fdot4(b, clip_vtx, mrow);
      }

      unsigned clip_vertex_index = nir_intrinsic_base(intr);

      for (int i = 0; i < 2; ++i) {
         auto clip_i = nir_vec(b, &output[4 * i], 4);
         auto store = nir_store_output(b, clip_i, intr->src[1].ssa);
         nir_intrinsic_set_write_mask(store, 0xf);
         nir_intrinsic_set_base(store, clip_vertex_index);
         nir_io_semantics semantic = nir_intrinsic_io_semantics(intr);
         semantic.location = VARYING_SLOT_CLIP_DIST0 + i;
         semantic.no_varying = 1;

         if (i > 0)
            nir_intrinsic_set_base(store, m_clipplane1);
         nir_intrinsic_set_write_mask(store, 0xf);
         nir_intrinsic_set_io_semantics(store, semantic);
      }
      nir_intrinsic_set_base(intr, m_clipvtx);

      nir_ssa_def *result = NIR_LOWER_INSTR_PROGRESS_REPLACE;
      for (unsigned i = 0; i < m_so_info.num_outputs; ++i) {
         if (m_so_info.output[i].register_index == clip_vertex_index) {
            m_so_info.output[i].register_index = m_clipvtx;
            result = NIR_LOWER_INSTR_PROGRESS;
         }
      }
      return result;
   }
   int m_clipplane1;
   int m_clipvtx;
   pipe_stream_output_info& m_so_info;
};

/* lower_uniforms_to_ubo adds a 1 to the UBO buffer ID.
 * If the buffer ID is a non-constant value we end up
 * with "iadd bufid, 1", bot on r600 we can put that constant
 * "1" as constant cache ID into the CF instruction and don't need
 * to execute that extra ADD op, so eliminate the addition here
 * again and move the buffer base ID into the base value of
 * the intrinsic that is not used otherwise */
class OptIndirectUBOLoads : public NirLowerInstruction {
private:
   bool filter(const nir_instr *instr) const override
   {
      if (instr->type != nir_instr_type_intrinsic)
         return false;

      auto intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4)
         return false;

      if (nir_src_as_const_value(intr->src[0]) != nullptr)
         return false;

      return nir_intrinsic_base(intr) == 0;
   }

   nir_ssa_def *lower(nir_instr *instr) override
   {
      auto intr = nir_instr_as_intrinsic(instr);
      assert(intr->intrinsic == nir_intrinsic_load_ubo_vec4);
      assert(intr->src[0].is_ssa);

      auto parent = intr->src[0].ssa->parent_instr;

      if (parent->type != nir_instr_type_alu)
         return nullptr;

      auto alu = nir_instr_as_alu(parent);

      if (alu->op != nir_op_iadd)
         return nullptr;

      int new_base = 0;
      nir_src *new_bufid = nullptr;
      auto src0 = nir_src_as_const_value(alu->src[0].src);
      if (src0) {
         new_bufid = &alu->src[1].src;
         new_base = src0->i32;
      } else if (auto src1 = nir_src_as_const_value(alu->src[1].src)) {
         new_bufid = &alu->src[0].src;
         new_base = src1->i32;
      } else {
         return nullptr;
      }

      assert(new_bufid->is_ssa);

      nir_intrinsic_set_base(intr, new_base);
      nir_instr_rewrite_src(instr, &intr->src[0], nir_src_for_ssa(new_bufid->ssa));
      return &intr->dest.ssa;
   }
};

} // namespace r600

static nir_intrinsic_op
r600_map_atomic(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_atomic_counter_read_deref:
      return nir_intrinsic_atomic_counter_read;
   case nir_intrinsic_atomic_counter_inc_deref:
      return nir_intrinsic_atomic_counter_inc;
   case nir_intrinsic_atomic_counter_pre_dec_deref:
      return nir_intrinsic_atomic_counter_pre_dec;
   case nir_intrinsic_atomic_counter_post_dec_deref:
      return nir_intrinsic_atomic_counter_post_dec;
   case nir_intrinsic_atomic_counter_add_deref:
      return nir_intrinsic_atomic_counter_add;
   case nir_intrinsic_atomic_counter_min_deref:
      return nir_intrinsic_atomic_counter_min;
   case nir_intrinsic_atomic_counter_max_deref:
      return nir_intrinsic_atomic_counter_max;
   case nir_intrinsic_atomic_counter_and_deref:
      return nir_intrinsic_atomic_counter_and;
   case nir_intrinsic_atomic_counter_or_deref:
      return nir_intrinsic_atomic_counter_or;
   case nir_intrinsic_atomic_counter_xor_deref:
      return nir_intrinsic_atomic_counter_xor;
   case nir_intrinsic_atomic_counter_exchange_deref:
      return nir_intrinsic_atomic_counter_exchange;
   case nir_intrinsic_atomic_counter_comp_swap_deref:
      return nir_intrinsic_atomic_counter_comp_swap;
   default:
      return nir_num_intrinsics;
   }
}

static bool
r600_lower_deref_instr(nir_builder *b, nir_instr *instr_, UNUSED void *cb_data)
{
   if (instr_->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *instr = nir_instr_as_intrinsic(instr_);

   nir_intrinsic_op op = r600_map_atomic(instr->intrinsic);
   if (nir_num_intrinsics == op)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(instr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   if (var->data.mode != nir_var_uniform && var->data.mode != nir_var_mem_ssbo &&
       var->data.mode != nir_var_mem_shared)
      return false; /* atomics passed as function arguments can't be lowered */

   const unsigned idx = var->data.binding;

   b->cursor = nir_before_instr(&instr->instr);

   nir_ssa_def *offset = nir_imm_int(b, var->data.index);
   for (nir_deref_instr *d = deref; d->deref_type != nir_deref_type_var;
        d = nir_deref_instr_parent(d)) {
      assert(d->deref_type == nir_deref_type_array);
      assert(d->arr.index.is_ssa);

      unsigned array_stride = 1;
      if (glsl_type_is_array(d->type))
         array_stride *= glsl_get_aoa_size(d->type);

      offset =
         nir_iadd(b, offset, nir_imul(b, d->arr.index.ssa, nir_imm_int(b, array_stride)));
   }

   /* Since the first source is a deref and the first source in the lowered
    * instruction is the offset, we can just swap it out and change the
    * opcode.
    */
   instr->intrinsic = op;
   nir_instr_rewrite_src(&instr->instr, &instr->src[0], nir_src_for_ssa(offset));
   nir_intrinsic_set_base(instr, idx);

   nir_deref_instr_remove_if_unused(deref);

   return true;
}

static bool
r600_lower_clipvertex_to_clipdist(nir_shader *sh, pipe_stream_output_info& so_info)
{
   if (!(sh->info.outputs_written & VARYING_BIT_CLIP_VERTEX))
      return false;

   int noutputs = util_bitcount64(sh->info.outputs_written);
   bool result = r600::LowerClipvertexWrite(noutputs, so_info).run(sh);
   return result;
}

static bool
r600_nir_lower_atomics(nir_shader *shader)
{
   /* First re-do the offsets, in Hardware we start at zero for each new
    * binding, and we use an offset of one per counter */
   int current_binding = -1;
   int current_offset = 0;
   nir_foreach_variable_with_modes(var, shader, nir_var_uniform)
   {
      if (!var->type->contains_atomic())
         continue;

      if (current_binding == (int)var->data.binding) {
         var->data.index = current_offset;
         current_offset += var->type->atomic_size() / ATOMIC_COUNTER_SIZE;
      } else {
         current_binding = var->data.binding;
         var->data.index = 0;
         current_offset = var->type->atomic_size() / ATOMIC_COUNTER_SIZE;
      }
   }

   return nir_shader_instructions_pass(shader,
                                       r600_lower_deref_instr,
                                       nir_metadata_block_index | nir_metadata_dominance,
                                       NULL);
}
using r600::r600_lower_fs_out_to_vector;
using r600::r600_lower_scratch_addresses;
using r600::r600_lower_ubo_to_align16;

int
r600_glsl_type_size(const struct glsl_type *type, bool is_bindless)
{
   return glsl_count_vec4_slots(type, false, is_bindless);
}

void
r600_get_natural_size_align_bytes(const struct glsl_type *type,
                                  unsigned *size,
                                  unsigned *align)
{
   if (type->base_type != GLSL_TYPE_ARRAY) {
      *align = 1;
      *size = 1;
   } else {
      unsigned elem_size, elem_align;
      glsl_get_natural_size_align_bytes(type->fields.array, &elem_size, &elem_align);
      *align = 1;
      *size = type->length;
   }
}

static bool
r600_lower_shared_io_impl(nir_function *func)
{
   nir_builder b;
   nir_builder_init(&b, func->impl);

   bool progress = false;
   nir_foreach_block(block, func->impl)
   {
      nir_foreach_instr_safe(instr, block)
      {

         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *op = nir_instr_as_intrinsic(instr);
         if (op->intrinsic != nir_intrinsic_load_shared &&
             op->intrinsic != nir_intrinsic_store_shared)
            continue;

         b.cursor = nir_before_instr(instr);

         if (op->intrinsic == nir_intrinsic_load_shared) {
            nir_ssa_def *addr = op->src[0].ssa;

            switch (nir_dest_num_components(op->dest)) {
            case 2: {
               auto addr2 = nir_iadd_imm(&b, addr, 4);
               addr = nir_vec2(&b, addr, addr2);
               break;
            }
            case 3: {
               auto addr2 = nir_iadd(&b, addr, nir_imm_ivec2(&b, 4, 8));
               addr =
                  nir_vec3(&b, addr, nir_channel(&b, addr2, 0), nir_channel(&b, addr2, 1));
               break;
            }
            case 4: {
               addr = nir_iadd(&b, addr, nir_imm_ivec4(&b, 0, 4, 8, 12));
               break;
            }
            }

            auto load =
               nir_intrinsic_instr_create(b.shader, nir_intrinsic_load_local_shared_r600);
            load->num_components = nir_dest_num_components(op->dest);
            load->src[0] = nir_src_for_ssa(addr);
            nir_ssa_dest_init(&load->instr, &load->dest, load->num_components, 32, NULL);
            nir_ssa_def_rewrite_uses(&op->dest.ssa, &load->dest.ssa);
            nir_builder_instr_insert(&b, &load->instr);
         } else {
            nir_ssa_def *addr = op->src[1].ssa;
            for (int i = 0; i < 2; ++i) {
               unsigned test_mask = (0x3 << 2 * i);
               if (!(nir_intrinsic_write_mask(op) & test_mask))
                  continue;

               auto store =
                  nir_intrinsic_instr_create(b.shader,
                                             nir_intrinsic_store_local_shared_r600);
               unsigned writemask = nir_intrinsic_write_mask(op) & test_mask;
               nir_intrinsic_set_write_mask(store, writemask);
               store->src[0] = nir_src_for_ssa(op->src[0].ssa);
               store->num_components = store->src[0].ssa->num_components;
               bool start_even = (writemask & (1u << (2 * i)));

               auto addr2 =
                  nir_iadd(&b, addr, nir_imm_int(&b, 8 * i + (start_even ? 0 : 4)));
               store->src[1] = nir_src_for_ssa(addr2);

               nir_builder_instr_insert(&b, &store->instr);
            }
         }
         nir_instr_remove(instr);
         progress = true;
      }
   }
   return progress;
}

static bool
r600_lower_shared_io(nir_shader *nir)
{
   bool progress = false;
   nir_foreach_function(function, nir)
   {
      if (function->impl && r600_lower_shared_io_impl(function))
         progress = true;
   }
   return progress;
}

static nir_ssa_def *
r600_lower_fs_pos_input_impl(nir_builder *b, nir_instr *instr, void *_options)
{
   (void)_options;
   auto old_ir = nir_instr_as_intrinsic(instr);
   auto load = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_input);
   nir_ssa_dest_init(&load->instr,
                     &load->dest,
                     old_ir->dest.ssa.num_components,
                     old_ir->dest.ssa.bit_size,
                     NULL);
   nir_intrinsic_set_io_semantics(load, nir_intrinsic_io_semantics(old_ir));

   nir_intrinsic_set_base(load, nir_intrinsic_base(old_ir));
   nir_intrinsic_set_component(load, nir_intrinsic_component(old_ir));
   nir_intrinsic_set_dest_type(load, nir_type_float32);
   load->num_components = old_ir->num_components;
   load->src[0] = old_ir->src[1];
   nir_builder_instr_insert(b, &load->instr);
   return &load->dest.ssa;
}

bool
r600_lower_fs_pos_input_filter(const nir_instr *instr, const void *_options)
{
   (void)_options;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   auto ir = nir_instr_as_intrinsic(instr);
   if (ir->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;

   return nir_intrinsic_io_semantics(ir).location == VARYING_SLOT_POS;
}

/* Strip the interpolator specification, it is not needed and irritates */
bool
r600_lower_fs_pos_input(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader,
                                        r600_lower_fs_pos_input_filter,
                                        r600_lower_fs_pos_input_impl,
                                        nullptr);
};

bool
r600_opt_indirect_fbo_loads(nir_shader *shader)
{
   return r600::OptIndirectUBOLoads().run(shader);
}

static bool
optimize_once(nir_shader *shader)
{
   bool progress = false;
   NIR_PASS(progress, shader, nir_lower_vars_to_ssa);
   NIR_PASS(progress, shader, nir_copy_prop);
   NIR_PASS(progress, shader, nir_opt_dce);
   NIR_PASS(progress, shader, nir_opt_algebraic);
   NIR_PASS(progress, shader, nir_opt_constant_folding);
   NIR_PASS(progress, shader, nir_opt_copy_prop_vars);
   NIR_PASS(progress, shader, nir_opt_remove_phis);

   if (nir_opt_trivial_continues(shader)) {
      progress = true;
      NIR_PASS(progress, shader, nir_copy_prop);
      NIR_PASS(progress, shader, nir_opt_dce);
   }

   NIR_PASS(progress, shader, nir_opt_if, nir_opt_if_optimize_phi_true_false);
   NIR_PASS(progress, shader, nir_opt_dead_cf);
   NIR_PASS(progress, shader, nir_opt_cse);
   NIR_PASS(progress, shader, nir_opt_peephole_select, 200, true, true);

   NIR_PASS(progress, shader, nir_opt_conditional_discard);
   NIR_PASS(progress, shader, nir_opt_dce);
   NIR_PASS(progress, shader, nir_opt_undef);
   NIR_PASS(progress, shader, nir_opt_loop_unroll);
   return progress;
}

bool
has_saturate(const nir_function *func)
{
   nir_foreach_block(block, func->impl)
   {
      nir_foreach_instr(instr, block)
      {
         if (instr->type == nir_instr_type_alu) {
            auto alu = nir_instr_as_alu(instr);
            if (alu->dest.saturate)
               return true;
         }
      }
   }
   return false;
}

static bool
r600_is_last_vertex_stage(nir_shader *nir, const r600_shader_key& key)
{
   if (nir->info.stage == MESA_SHADER_GEOMETRY)
      return true;

   if (nir->info.stage == MESA_SHADER_TESS_EVAL && !key.tes.as_es)
      return true;

   if (nir->info.stage == MESA_SHADER_VERTEX && !key.vs.as_es && !key.vs.as_ls)
      return true;

   return false;
}

extern "C" bool
r600_lower_to_scalar_instr_filter(const nir_instr *instr, const void *)
{
   if (instr->type != nir_instr_type_alu)
      return true;

   auto alu = nir_instr_as_alu(instr);
   switch (alu->op) {
   case nir_op_bany_fnequal3:
   case nir_op_bany_fnequal4:
   case nir_op_ball_fequal3:
   case nir_op_ball_fequal4:
   case nir_op_bany_inequal3:
   case nir_op_bany_inequal4:
   case nir_op_ball_iequal3:
   case nir_op_ball_iequal4:
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_fddx:
   case nir_op_fddx_coarse:
   case nir_op_fddx_fine:
   case nir_op_fddy:
   case nir_op_fddy_coarse:
   case nir_op_fddy_fine:
      return nir_src_bit_size(alu->src[0].src) == 64;
   case nir_op_cube_r600:
      return false;
   default:
      return true;
   }
}

class MallocPoolRelease {
public:
   MallocPoolRelease() { r600::init_pool(); }
   ~MallocPoolRelease() { r600::release_pool(); }
};

extern "C" char *
r600_finalize_nir(pipe_screen *screen, void *shader)
{
   r600_screen *rs = (r600_screen *)screen;

   nir_shader *nir = (nir_shader *)shader;

   NIR_PASS_V(nir, nir_lower_regs_to_ssa);
   const int nir_lower_flrp_mask = 16 | 32 | 64;

   NIR_PASS_V(nir, nir_lower_flrp, nir_lower_flrp_mask, false);

   nir_lower_idiv_options idiv_options = {0};
   NIR_PASS_V(nir, nir_lower_idiv, &idiv_options);

   NIR_PASS_V(nir, r600_nir_lower_trigen, rs->b.gfx_level);
   NIR_PASS_V(nir, nir_lower_phis_to_scalar, false);
   NIR_PASS_V(nir, nir_lower_undef_to_zero);

   struct nir_lower_tex_options lower_tex_options = {0};
   lower_tex_options.lower_txp = ~0u;
   lower_tex_options.lower_txf_offset = true;
   lower_tex_options.lower_invalid_implicit_lod = true;
   lower_tex_options.lower_tg4_offsets = true;

   NIR_PASS_V(nir, nir_lower_tex, &lower_tex_options);
   NIR_PASS_V(nir, r600_nir_lower_txl_txf_array_or_cube);
   NIR_PASS_V(nir, r600_nir_lower_cube_to_2darray);

   NIR_PASS_V(nir, r600_nir_lower_pack_unpack_2x16);

   NIR_PASS_V(nir, r600_lower_shared_io);
   NIR_PASS_V(nir, r600_nir_lower_atomics);

   while (optimize_once(nir))
      ;

   return NULL;
}

int
r600_shader_from_nir(struct r600_context *rctx,
                     struct r600_pipe_shader *pipeshader,
                     r600_shader_key *key)
{

   MallocPoolRelease pool_release;

   struct r600_pipe_shader_selector *sel = pipeshader->selector;

   bool lower_64bit =
      (rctx->b.gfx_level < CAYMAN &&
       (sel->nir->options->lower_int64_options ||
        sel->nir->options->lower_doubles_options) &&
       (sel->nir->info.bit_sizes_float | sel->nir->info.bit_sizes_int) & 64);

   if (rctx->screen->b.debug_flags & DBG_PREOPT_IR) {
      fprintf(stderr, "PRE-OPT-NIR-----------.------------------------------\n");
      nir_print_shader(sel->nir, stderr);
      fprintf(stderr, "END PRE-OPT-NIR--------------------------------------\n\n");
   }

   auto sh = nir_shader_clone(sel->nir, sel->nir);
   r600::sort_uniforms(sel->nir);


   while (optimize_once(sh))
      ;


   if (sh->info.stage == MESA_SHADER_VERTEX)
      NIR_PASS_V(sh, r600_vectorize_vs_inputs);

   if (sh->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(sh, nir_lower_fragcoord_wtrans);
      NIR_PASS_V(sh, r600_lower_fs_out_to_vector);
      NIR_PASS_V(sh, nir_opt_dce);
      NIR_PASS_V(sh, nir_remove_dead_variables, nir_var_shader_out, 0);
      r600::sort_fsoutput(sh);
   }
   nir_variable_mode io_modes = nir_var_uniform | nir_var_shader_in | nir_var_shader_out;

   NIR_PASS_V(sh, nir_opt_combine_stores, nir_var_shader_out);
   NIR_PASS_V(sh,
              nir_lower_io,
              io_modes,
              r600_glsl_type_size,
              nir_lower_io_lower_64bit_to_32);

   if (sh->info.stage == MESA_SHADER_FRAGMENT)
      NIR_PASS_V(sh, r600_lower_fs_pos_input);

   /**/
   if (lower_64bit)
      NIR_PASS_V(sh, nir_lower_indirect_derefs, nir_var_function_temp, 10);

   NIR_PASS_V(sh, nir_opt_constant_folding);
   NIR_PASS_V(sh, nir_io_add_const_offset_to_base, io_modes);

   NIR_PASS_V(sh, nir_lower_alu_to_scalar, r600_lower_to_scalar_instr_filter, NULL);
   NIR_PASS_V(sh, nir_lower_phis_to_scalar, false);
   if (lower_64bit)
      NIR_PASS_V(sh, r600::r600_nir_split_64bit_io);
   NIR_PASS_V(sh, nir_lower_alu_to_scalar, r600_lower_to_scalar_instr_filter, NULL);
   NIR_PASS_V(sh, nir_lower_phis_to_scalar, false);
   NIR_PASS_V(sh, nir_lower_alu_to_scalar, r600_lower_to_scalar_instr_filter, NULL);
   NIR_PASS_V(sh, nir_copy_prop);
   NIR_PASS_V(sh, nir_opt_dce);



   if (r600_is_last_vertex_stage(sh, *key))
      r600_lower_clipvertex_to_clipdist(sh, sel->so);

   if (sh->info.stage == MESA_SHADER_TESS_CTRL ||
       sh->info.stage == MESA_SHADER_TESS_EVAL ||
       (sh->info.stage == MESA_SHADER_VERTEX && key->vs.as_ls)) {
      auto prim_type = sh->info.stage == MESA_SHADER_TESS_EVAL
                          ? u_tess_prim_from_shader(sh->info.tess._primitive_mode)
                          : key->tcs.prim_mode;
      NIR_PASS_V(sh, r600_lower_tess_io, static_cast<pipe_prim_type>(prim_type));
   }

   if (sh->info.stage == MESA_SHADER_TESS_CTRL)
      NIR_PASS_V(sh, r600_append_tcs_TF_emission, (pipe_prim_type)key->tcs.prim_mode);

   if (sh->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS_V(sh,
                 r600_lower_tess_coord,
                 u_tess_prim_from_shader(sh->info.tess._primitive_mode));
   }

   NIR_PASS_V(sh, nir_lower_alu_to_scalar, r600_lower_to_scalar_instr_filter, NULL);
   NIR_PASS_V(sh, nir_lower_phis_to_scalar, false);
   NIR_PASS_V(sh, nir_lower_alu_to_scalar, r600_lower_to_scalar_instr_filter, NULL);
   NIR_PASS_V(sh, r600_nir_lower_int_tg4);
   NIR_PASS_V(sh, r600::r600_nir_lower_tex_to_backend, rctx->b.gfx_level);

   if ((sh->info.bit_sizes_float | sh->info.bit_sizes_int) & 64) {
      NIR_PASS_V(sh, r600::r600_nir_split_64bit_io);
      NIR_PASS_V(sh, r600::r600_split_64bit_alu_and_phi);
      NIR_PASS_V(sh, nir_split_64bit_vec3_and_vec4);
      NIR_PASS_V(sh, nir_lower_int64);
   }

   NIR_PASS_V(sh, nir_lower_ubo_vec4);
   NIR_PASS_V(sh, r600_opt_indirect_fbo_loads);

   if (lower_64bit)
      NIR_PASS_V(sh, r600::r600_nir_64_to_vec2);

   if ((sh->info.bit_sizes_float | sh->info.bit_sizes_int) & 64)
      NIR_PASS_V(sh, r600::r600_split_64bit_uniforms_and_ubo);

   /* Lower to scalar to let some optimization work out better */
   while (optimize_once(sh))
      ;

   if (lower_64bit)
      NIR_PASS_V(sh, r600::r600_merge_vec2_stores);

   NIR_PASS_V(sh, nir_remove_dead_variables, nir_var_shader_in, NULL);
   NIR_PASS_V(sh, nir_remove_dead_variables, nir_var_shader_out, NULL);

   NIR_PASS_V(sh,
              nir_lower_vars_to_scratch,
              nir_var_function_temp,
              40,
              r600_get_natural_size_align_bytes);

   while (optimize_once(sh))
      ;

   bool late_algebraic_progress;
   do {
      late_algebraic_progress = false;
      NIR_PASS(late_algebraic_progress, sh, nir_opt_algebraic_late);
      NIR_PASS(late_algebraic_progress, sh, nir_opt_constant_folding);
      NIR_PASS(late_algebraic_progress, sh, nir_copy_prop);
      NIR_PASS(late_algebraic_progress, sh, nir_opt_dce);
      NIR_PASS(late_algebraic_progress, sh, nir_opt_cse);
   } while (late_algebraic_progress);

   NIR_PASS_V(sh, nir_lower_bool_to_int32);

   NIR_PASS_V(sh, nir_lower_locals_to_regs);

   NIR_PASS_V(sh,
              nir_lower_to_source_mods,
              (nir_lower_to_source_mods_flags)(nir_lower_float_source_mods |
                                               nir_lower_64bit_source_mods));
   NIR_PASS_V(sh, nir_convert_from_ssa, true);
   NIR_PASS_V(sh, nir_opt_dce);

   if (rctx->screen->b.debug_flags & DBG_ALL_SHADERS) {
      fprintf(stderr,
              "-- NIR --------------------------------------------------------\n");
      struct nir_function *func =
         (struct nir_function *)exec_list_get_head(&sh->functions);
      nir_index_ssa_defs(func->impl);
      nir_print_shader(sh, stderr);
      fprintf(stderr,
              "-- END --------------------------------------------------------\n");
   }

   memset(&pipeshader->shader, 0, sizeof(r600_shader));
   pipeshader->scratch_space_needed = sh->scratch_size;

   if (sh->info.stage == MESA_SHADER_TESS_EVAL || sh->info.stage == MESA_SHADER_VERTEX ||
       sh->info.stage == MESA_SHADER_GEOMETRY) {
      pipeshader->shader.clip_dist_write |=
         ((1 << sh->info.clip_distance_array_size) - 1);
      pipeshader->shader.cull_dist_write = ((1 << sh->info.cull_distance_array_size) - 1)
                                           << sh->info.clip_distance_array_size;
      pipeshader->shader.cc_dist_mask =
         (1 << (sh->info.cull_distance_array_size + sh->info.clip_distance_array_size)) -
         1;
   }
   struct r600_shader *gs_shader = nullptr;
   if (rctx->gs_shader)
      gs_shader = &rctx->gs_shader->current->shader;
   r600_screen *rscreen = rctx->screen;

   r600::Shader *shader =
      r600::Shader::translate_from_nir(sh, &sel->so, gs_shader, *key, rctx->isa->hw_class);

   assert(shader);
   if (!shader)
      return -2;

   pipeshader->enabled_stream_buffers_mask = shader->enabled_stream_buffers_mask();
   pipeshader->selector->info.file_count[TGSI_FILE_HW_ATOMIC] +=
      shader->atomic_file_count();
   pipeshader->selector->info.writes_memory =
      shader->has_flag(r600::Shader::sh_writes_memory);

   if (r600::sfn_log.has_debug_flag(r600::SfnLog::steps)) {
      std::cerr << "Shader after conversion from nir\n";
      shader->print(std::cerr);
   }

   if (!r600::sfn_log.has_debug_flag(r600::SfnLog::noopt)) {
      optimize(*shader);

      if (r600::sfn_log.has_debug_flag(r600::SfnLog::steps)) {
         std::cerr << "Shader after optimization\n";
         shader->print(std::cerr);
      }
   }

   auto scheduled_shader = r600::schedule(shader);
   if (r600::sfn_log.has_debug_flag(r600::SfnLog::steps)) {
      std::cerr << "Shader after scheduling\n";
      shader->print(std::cerr);
   }

   if (!r600::sfn_log.has_debug_flag(r600::SfnLog::nomerge)) {

      if (r600::sfn_log.has_debug_flag(r600::SfnLog::merge)) {
         r600::sfn_log << r600::SfnLog::merge << "Shader before RA\n";
         scheduled_shader->print(std::cerr);
      }

      r600::sfn_log << r600::SfnLog::trans << "Merge registers\n";
      auto lrm = r600::LiveRangeEvaluator().run(*scheduled_shader);

      if (!r600::register_allocation(lrm)) {
         R600_ERR("%s: Register allocation failed\n", __func__);
         /* For now crash if the shader could not be benerated */
         assert(0);
         return -1;
      } else if (r600::sfn_log.has_debug_flag(r600::SfnLog::merge) ||
                 r600::sfn_log.has_debug_flag(r600::SfnLog::steps)) {
         r600::sfn_log << "Shader after RA\n";
         scheduled_shader->print(std::cerr);
      }
   }

   scheduled_shader->get_shader_info(&pipeshader->shader);
   pipeshader->shader.uses_doubles = sh->info.bit_sizes_float & 64 ? 1 : 0;

   r600_bytecode_init(&pipeshader->shader.bc,
                      rscreen->b.gfx_level,
                      rscreen->b.family,
                      rscreen->has_compressed_msaa_texturing);

   r600::sfn_log << r600::SfnLog::shader_info << "pipeshader->shader.processor_type = "
                 << pipeshader->shader.processor_type << "\n";

   pipeshader->shader.bc.type = pipeshader->shader.processor_type;
   pipeshader->shader.bc.isa = rctx->isa;

   r600::Assembler afs(&pipeshader->shader, *key);
   if (!afs.lower(scheduled_shader)) {
      R600_ERR("%s: Lowering to assembly failed\n", __func__);

      scheduled_shader->print(std::cerr);
      /* For now crash if the shader could not be generated */
      assert(0);
      return -1;
   }

   if (sh->info.stage == MESA_SHADER_GEOMETRY) {
      r600::sfn_log << r600::SfnLog::shader_info
                    << "Geometry shader, create copy shader\n";
      generate_gs_copy_shader(rctx, pipeshader, &sel->so);
      assert(pipeshader->gs_copy_shader);
   } else {
      r600::sfn_log << r600::SfnLog::shader_info << "This is not a Geometry shader\n";
   }
   ralloc_free(sh);

   return 0;
}
