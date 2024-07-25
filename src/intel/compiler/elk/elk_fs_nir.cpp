/*
 * Copyright © 2010 Intel Corporation
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

#include "elk_fs.h"
#include "elk_fs_builder.h"
#include "elk_nir.h"
#include "elk_nir_private.h"
#include "elk_eu.h"
#include "nir.h"
#include "nir_intrinsics.h"
#include "nir_search_helpers.h"
#include "util/u_math.h"
#include "util/bitscan.h"

#include <vector>

using namespace elk;

struct elk_fs_bind_info {
   bool valid;
   bool bindless;
   unsigned block;
   unsigned set;
   unsigned binding;
};

struct nir_to_elk_state {
   elk_fs_visitor &s;
   const nir_shader *nir;
   const intel_device_info *devinfo;
   void *mem_ctx;

   /* Points to the end of the program.  Annotated with the current NIR
    * instruction when applicable.
    */
   fs_builder bld;

   elk_fs_reg *ssa_values;
   elk_fs_inst **resource_insts;
   struct elk_fs_bind_info *ssa_bind_infos;
   elk_fs_reg *resource_values;
   elk_fs_reg *system_values;
};

static elk_fs_reg get_nir_src(nir_to_elk_state &ntb, const nir_src &src);
static elk_fs_reg get_nir_def(nir_to_elk_state &ntb, const nir_def &def);
static nir_component_mask_t get_nir_write_mask(const nir_def &def);

static void fs_nir_emit_intrinsic(nir_to_elk_state &ntb, const fs_builder &bld, nir_intrinsic_instr *instr);
static elk_fs_reg emit_samplepos_setup(nir_to_elk_state &ntb);
static elk_fs_reg emit_sampleid_setup(nir_to_elk_state &ntb);
static elk_fs_reg emit_samplemaskin_setup(nir_to_elk_state &ntb);

static void fs_nir_emit_impl(nir_to_elk_state &ntb, nir_function_impl *impl);
static void fs_nir_emit_cf_list(nir_to_elk_state &ntb, exec_list *list);
static void fs_nir_emit_if(nir_to_elk_state &ntb, nir_if *if_stmt);
static void fs_nir_emit_loop(nir_to_elk_state &ntb, nir_loop *loop);
static void fs_nir_emit_block(nir_to_elk_state &ntb, nir_block *block);
static void fs_nir_emit_instr(nir_to_elk_state &ntb, nir_instr *instr);

static void fs_nir_emit_surface_atomic(nir_to_elk_state &ntb,
                                       const fs_builder &bld,
                                       nir_intrinsic_instr *instr,
                                       elk_fs_reg surface,
                                       bool bindless);
static void fs_nir_emit_global_atomic(nir_to_elk_state &ntb,
                                      const fs_builder &bld,
                                      nir_intrinsic_instr *instr);

static void
fs_nir_setup_outputs(nir_to_elk_state &ntb)
{
   elk_fs_visitor &s = ntb.s;

   if (s.stage == MESA_SHADER_TESS_CTRL ||
       s.stage == MESA_SHADER_FRAGMENT)
      return;

   unsigned vec4s[VARYING_SLOT_TESS_MAX] = { 0, };

   /* Calculate the size of output registers in a separate pass, before
    * allocating them.  With ARB_enhanced_layouts, multiple output variables
    * may occupy the same slot, but have different type sizes.
    */
   nir_foreach_shader_out_variable(var, s.nir) {
      const int loc = var->data.driver_location;
      const unsigned var_vec4s = nir_variable_count_slots(var, var->type);
      vec4s[loc] = MAX2(vec4s[loc], var_vec4s);
   }

   for (unsigned loc = 0; loc < ARRAY_SIZE(vec4s);) {
      if (vec4s[loc] == 0) {
         loc++;
         continue;
      }

      unsigned reg_size = vec4s[loc];

      /* Check if there are any ranges that start within this range and extend
       * past it. If so, include them in this allocation.
       */
      for (unsigned i = 1; i < reg_size; i++) {
         assert(i + loc < ARRAY_SIZE(vec4s));
         reg_size = MAX2(vec4s[i + loc] + i, reg_size);
      }

      elk_fs_reg reg = ntb.bld.vgrf(ELK_REGISTER_TYPE_F, 4 * reg_size);
      for (unsigned i = 0; i < reg_size; i++) {
         assert(loc + i < ARRAY_SIZE(s.outputs));
         s.outputs[loc + i] = offset(reg, ntb.bld, 4 * i);
      }

      loc += reg_size;
   }
}

static void
fs_nir_setup_uniforms(elk_fs_visitor &s)
{
   /* Only the first compile gets to set up uniforms. */
   if (s.push_constant_loc)
      return;

   s.uniforms = s.nir->num_uniforms / 4;

   if (gl_shader_stage_is_compute(s.stage)) {
      /* Add uniforms for builtins after regular NIR uniforms. */
      assert(s.uniforms == s.prog_data->nr_params);

      /* Subgroup ID must be the last uniform on the list.  This will make
       * easier later to split between cross thread and per thread
       * uniforms.
       */
      uint32_t *param = elk_stage_prog_data_add_params(s.prog_data, 1);
      *param = ELK_PARAM_BUILTIN_SUBGROUP_ID;
      s.uniforms++;
   }
}

static elk_fs_reg
emit_work_group_id_setup(nir_to_elk_state &ntb)
{
   elk_fs_visitor &s = ntb.s;
   const fs_builder &bld = ntb.bld;

   assert(gl_shader_stage_is_compute(s.stage));

   elk_fs_reg id = bld.vgrf(ELK_REGISTER_TYPE_UD, 3);

   struct elk_reg r0_1(retype(elk_vec1_grf(0, 1), ELK_REGISTER_TYPE_UD));
   bld.MOV(id, r0_1);

   struct elk_reg r0_6(retype(elk_vec1_grf(0, 6), ELK_REGISTER_TYPE_UD));
   struct elk_reg r0_7(retype(elk_vec1_grf(0, 7), ELK_REGISTER_TYPE_UD));
   bld.MOV(offset(id, bld, 1), r0_6);
   bld.MOV(offset(id, bld, 2), r0_7);

   return id;
}

static bool
emit_system_values_block(nir_to_elk_state &ntb, nir_block *block)
{
   elk_fs_visitor &s = ntb.s;
   elk_fs_reg *reg;

   nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_vertex_id:
      case nir_intrinsic_load_base_vertex:
         unreachable("should be lowered by nir_lower_system_values().");

      case nir_intrinsic_load_vertex_id_zero_base:
      case nir_intrinsic_load_is_indexed_draw:
      case nir_intrinsic_load_first_vertex:
      case nir_intrinsic_load_instance_id:
      case nir_intrinsic_load_base_instance:
         unreachable("should be lowered by elk_nir_lower_vs_inputs().");
         break;

      case nir_intrinsic_load_draw_id:
         unreachable("should be lowered by elk_nir_lower_vs_inputs().");
         break;

      case nir_intrinsic_load_invocation_id:
         if (s.stage == MESA_SHADER_TESS_CTRL)
            break;
         assert(s.stage == MESA_SHADER_GEOMETRY);
         reg = &ntb.system_values[SYSTEM_VALUE_INVOCATION_ID];
         if (reg->file == BAD_FILE) {
            *reg = s.gs_payload().instance_id;
         }
         break;

      case nir_intrinsic_load_sample_pos:
      case nir_intrinsic_load_sample_pos_or_center:
         assert(s.stage == MESA_SHADER_FRAGMENT);
         reg = &ntb.system_values[SYSTEM_VALUE_SAMPLE_POS];
         if (reg->file == BAD_FILE)
            *reg = emit_samplepos_setup(ntb);
         break;

      case nir_intrinsic_load_sample_id:
         assert(s.stage == MESA_SHADER_FRAGMENT);
         reg = &ntb.system_values[SYSTEM_VALUE_SAMPLE_ID];
         if (reg->file == BAD_FILE)
            *reg = emit_sampleid_setup(ntb);
         break;

      case nir_intrinsic_load_sample_mask_in:
         assert(s.stage == MESA_SHADER_FRAGMENT);
         assert(s.devinfo->ver >= 7);
         reg = &ntb.system_values[SYSTEM_VALUE_SAMPLE_MASK_IN];
         if (reg->file == BAD_FILE)
            *reg = emit_samplemaskin_setup(ntb);
         break;

      case nir_intrinsic_load_workgroup_id:
         assert(gl_shader_stage_is_compute(s.stage));
         reg = &ntb.system_values[SYSTEM_VALUE_WORKGROUP_ID];
         if (reg->file == BAD_FILE)
            *reg = emit_work_group_id_setup(ntb);
         break;

      case nir_intrinsic_load_helper_invocation:
         assert(s.stage == MESA_SHADER_FRAGMENT);
         reg = &ntb.system_values[SYSTEM_VALUE_HELPER_INVOCATION];
         if (reg->file == BAD_FILE) {
            const fs_builder abld =
               ntb.bld.annotate("gl_HelperInvocation", NULL);

            /* On Gfx6+ (gl_HelperInvocation is only exposed on Gfx7+) the
             * pixel mask is in g1.7 of the thread payload.
             *
             * We move the per-channel pixel enable bit to the low bit of each
             * channel by shifting the byte containing the pixel mask by the
             * vector immediate 0x76543210UV.
             *
             * The region of <1,8,0> reads only 1 byte (the pixel masks for
             * subspans 0 and 1) in SIMD8 and an additional byte (the pixel
             * masks for 2 and 3) in SIMD16.
             */
            elk_fs_reg shifted = abld.vgrf(ELK_REGISTER_TYPE_UW, 1);

            for (unsigned i = 0; i < DIV_ROUND_UP(s.dispatch_width, 16); i++) {
               const fs_builder hbld = abld.group(MIN2(16, s.dispatch_width), i);
               /* According to the "PS Thread Payload for Normal
                * Dispatch" pages on the BSpec, the dispatch mask is
                * stored in R0.15/R1.15 on gfx20+ and in R1.7/R2.7 on
                * gfx6+.
                */
               const struct elk_reg reg = elk_vec1_grf(i + 1, 7);
               hbld.SHR(offset(shifted, hbld, i),
                        stride(retype(reg, ELK_REGISTER_TYPE_UB), 1, 8, 0),
                        elk_imm_v(0x76543210));
            }

            /* A set bit in the pixel mask means the channel is enabled, but
             * that is the opposite of gl_HelperInvocation so we need to invert
             * the mask.
             *
             * The negate source-modifier bit of logical instructions on Gfx8+
             * performs 1's complement negation, so we can use that instead of
             * a NOT instruction.
             */
            elk_fs_reg inverted = negate(shifted);
            if (s.devinfo->ver < 8) {
               inverted = abld.vgrf(ELK_REGISTER_TYPE_UW);
               abld.NOT(inverted, shifted);
            }

            /* We then resolve the 0/1 result to 0/~0 boolean values by ANDing
             * with 1 and negating.
             */
            elk_fs_reg anded = abld.vgrf(ELK_REGISTER_TYPE_UD, 1);
            abld.AND(anded, inverted, elk_imm_uw(1));

            elk_fs_reg dst = abld.vgrf(ELK_REGISTER_TYPE_D, 1);
            abld.MOV(dst, negate(retype(anded, ELK_REGISTER_TYPE_D)));
            *reg = dst;
         }
         break;

      default:
         break;
      }
   }

   return true;
}

static void
fs_nir_emit_system_values(nir_to_elk_state &ntb)
{
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   ntb.system_values = ralloc_array(ntb.mem_ctx, elk_fs_reg, SYSTEM_VALUE_MAX);
   for (unsigned i = 0; i < SYSTEM_VALUE_MAX; i++) {
      ntb.system_values[i] = elk_fs_reg();
   }

   /* Always emit SUBGROUP_INVOCATION.  Dead code will clean it up if we
    * never end up using it.
    */
   {
      const fs_builder abld = bld.annotate("gl_SubgroupInvocation", NULL);
      elk_fs_reg &reg = ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION];
      reg = abld.vgrf(ELK_REGISTER_TYPE_UW);
      abld.UNDEF(reg);

      const fs_builder allbld8 = abld.group(8, 0).exec_all();
      allbld8.MOV(reg, elk_imm_v(0x76543210));
      if (s.dispatch_width > 8)
         allbld8.ADD(byte_offset(reg, 16), reg, elk_imm_uw(8u));
      if (s.dispatch_width > 16) {
         const fs_builder allbld16 = abld.group(16, 0).exec_all();
         allbld16.ADD(byte_offset(reg, 32), reg, elk_imm_uw(16u));
      }
   }

   nir_function_impl *impl = nir_shader_get_entrypoint((nir_shader *)s.nir);
   nir_foreach_block(block, impl)
      emit_system_values_block(ntb, block);
}

static void
fs_nir_emit_impl(nir_to_elk_state &ntb, nir_function_impl *impl)
{
   ntb.ssa_values = rzalloc_array(ntb.mem_ctx, elk_fs_reg, impl->ssa_alloc);
   ntb.resource_insts = rzalloc_array(ntb.mem_ctx, elk_fs_inst *, impl->ssa_alloc);
   ntb.ssa_bind_infos = rzalloc_array(ntb.mem_ctx, struct elk_fs_bind_info, impl->ssa_alloc);
   ntb.resource_values = rzalloc_array(ntb.mem_ctx, elk_fs_reg, impl->ssa_alloc);

   fs_nir_emit_cf_list(ntb, &impl->body);
}

static void
fs_nir_emit_cf_list(nir_to_elk_state &ntb, exec_list *list)
{
   exec_list_validate(list);
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_if:
         fs_nir_emit_if(ntb, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         fs_nir_emit_loop(ntb, nir_cf_node_as_loop(node));
         break;

      case nir_cf_node_block:
         fs_nir_emit_block(ntb, nir_cf_node_as_block(node));
         break;

      default:
         unreachable("Invalid CFG node block");
      }
   }
}

static void
fs_nir_emit_if(nir_to_elk_state &ntb, nir_if *if_stmt)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;

   bool invert;
   elk_fs_reg cond_reg;

   /* If the condition has the form !other_condition, use other_condition as
    * the source, but invert the predicate on the if instruction.
    */
   nir_alu_instr *cond = nir_src_as_alu_instr(if_stmt->condition);
   if (cond != NULL && cond->op == nir_op_inot) {
      invert = true;
      cond_reg = get_nir_src(ntb, cond->src[0].src);
      cond_reg = offset(cond_reg, bld, cond->src[0].swizzle[0]);

      if (devinfo->ver <= 5 &&
	  (cond->instr.pass_flags & ELK_NIR_BOOLEAN_MASK) == ELK_NIR_BOOLEAN_NEEDS_RESOLVE) {
         /* redo boolean resolve on gen5 */
         elk_fs_reg masked = ntb.s.vgrf(glsl_int_type());
         bld.AND(masked, cond_reg, elk_imm_d(1));
         masked.negate = true;
         elk_fs_reg tmp = bld.vgrf(cond_reg.type);
         bld.MOV(retype(tmp, ELK_REGISTER_TYPE_D), masked);
         cond_reg = tmp;
      }
   } else {
      invert = false;
      cond_reg = get_nir_src(ntb, if_stmt->condition);
   }

   /* first, put the condition into f0 */
   elk_fs_inst *inst = bld.MOV(bld.null_reg_d(),
                           retype(cond_reg, ELK_REGISTER_TYPE_D));
   inst->conditional_mod = ELK_CONDITIONAL_NZ;

   bld.IF(ELK_PREDICATE_NORMAL)->predicate_inverse = invert;

   fs_nir_emit_cf_list(ntb, &if_stmt->then_list);

   if (!nir_cf_list_is_empty_block(&if_stmt->else_list)) {
      bld.emit(ELK_OPCODE_ELSE);
      fs_nir_emit_cf_list(ntb, &if_stmt->else_list);
   }

   bld.emit(ELK_OPCODE_ENDIF);

   if (devinfo->ver < 7)
      ntb.s.limit_dispatch_width(16, "Non-uniform control flow unsupported "
                                   "in SIMD32 mode.");
}

static void
fs_nir_emit_loop(nir_to_elk_state &ntb, nir_loop *loop)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;

   assert(!nir_loop_has_continue_construct(loop));
   bld.emit(ELK_OPCODE_DO);

   fs_nir_emit_cf_list(ntb, &loop->body);

   bld.emit(ELK_OPCODE_WHILE);

   if (devinfo->ver < 7)
      ntb.s.limit_dispatch_width(16, "Non-uniform control flow unsupported "
                                   "in SIMD32 mode.");
}

static void
fs_nir_emit_block(nir_to_elk_state &ntb, nir_block *block)
{
   fs_builder bld = ntb.bld;

   nir_foreach_instr(instr, block) {
      fs_nir_emit_instr(ntb, instr);
   }

   ntb.bld = bld;
}

/**
 * Recognizes a parent instruction of nir_op_extract_* and changes the type to
 * match instr.
 */
static bool
optimize_extract_to_float(nir_to_elk_state &ntb, nir_alu_instr *instr,
                          const elk_fs_reg &result)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;

   /* No fast path for f16 or f64. */
   assert(instr->op == nir_op_i2f32 || instr->op == nir_op_u2f32);

   if (!instr->src[0].src.ssa->parent_instr)
      return false;

   if (instr->src[0].src.ssa->parent_instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *src0 =
      nir_instr_as_alu(instr->src[0].src.ssa->parent_instr);

   unsigned bytes;
   bool is_signed;

   switch (src0->op) {
   case nir_op_extract_u8:
   case nir_op_extract_u16:
      bytes = src0->op == nir_op_extract_u8 ? 1 : 2;

      /* i2f(extract_u8(a, b)) and u2f(extract_u8(a, b)) produce the same
       * result. Ditto for extract_u16.
       */
      is_signed = false;
      break;

   case nir_op_extract_i8:
   case nir_op_extract_i16:
      bytes = src0->op == nir_op_extract_i8 ? 1 : 2;

      /* The fast path can't handle u2f(extract_i8(a, b)) because the implicit
       * sign extension of the extract_i8 is lost. For example,
       * u2f(extract_i8(0x0000ff00, 1)) should produce 4294967295.0, but a
       * fast path could either give 255.0 (by implementing the fast path as
       * u2f(extract_u8(x))) or -1.0 (by implementing the fast path as
       * i2f(extract_i8(x))). At one point in time, we incorrectly implemented
       * the former.
       */
      if (instr->op != nir_op_i2f32)
         return false;

      is_signed = true;
      break;

   default:
      return false;
   }

   unsigned element = nir_src_as_uint(src0->src[1].src);

   /* Element type to extract.*/
   const elk_reg_type type = elk_int_type(bytes, is_signed);

   elk_fs_reg op0 = get_nir_src(ntb, src0->src[0].src);
   op0.type = elk_type_for_nir_type(devinfo,
      (nir_alu_type)(nir_op_infos[src0->op].input_types[0] |
                     nir_src_bit_size(src0->src[0].src)));
   op0 = offset(op0, bld, src0->src[0].swizzle[0]);

   bld.MOV(result, subscript(op0, type, element));
   return true;
}

static bool
optimize_frontfacing_ternary(nir_to_elk_state &ntb,
                             nir_alu_instr *instr,
                             const elk_fs_reg &result)
{
   const intel_device_info *devinfo = ntb.devinfo;
   elk_fs_visitor &s = ntb.s;

   nir_intrinsic_instr *src0 = nir_src_as_intrinsic(instr->src[0].src);
   if (src0 == NULL || src0->intrinsic != nir_intrinsic_load_front_face)
      return false;

   if (!nir_src_is_const(instr->src[1].src) ||
       !nir_src_is_const(instr->src[2].src))
      return false;

   const float value1 = nir_src_as_float(instr->src[1].src);
   const float value2 = nir_src_as_float(instr->src[2].src);
   if (fabsf(value1) != 1.0f || fabsf(value2) != 1.0f)
      return false;

   /* nir_opt_algebraic should have gotten rid of bcsel(b, a, a) */
   assert(value1 == -value2);

   elk_fs_reg tmp = s.vgrf(glsl_int_type());

   if (devinfo->ver >= 6) {
      /* Bit 15 of g0.0 is 0 if the polygon is front facing. */
      elk_fs_reg g0 = elk_fs_reg(retype(elk_vec1_grf(0, 0), ELK_REGISTER_TYPE_W));

      /* For (gl_FrontFacing ? 1.0 : -1.0), emit:
       *
       *    or(8)  tmp.1<2>W  g0.0<0,1,0>W  0x00003f80W
       *    and(8) dst<1>D    tmp<8,8,1>D   0xbf800000D
       *
       * and negate g0.0<0,1,0>W for (gl_FrontFacing ? -1.0 : 1.0).
       *
       * This negation looks like it's safe in practice, because bits 0:4 will
       * surely be TRIANGLES
       */

      if (value1 == -1.0f) {
         g0.negate = true;
      }

      ntb.bld.OR(subscript(tmp, ELK_REGISTER_TYPE_W, 1),
                  g0, elk_imm_uw(0x3f80));
   } else {
      /* Bit 31 of g1.6 is 0 if the polygon is front facing. */
      elk_fs_reg g1_6 = elk_fs_reg(retype(elk_vec1_grf(1, 6), ELK_REGISTER_TYPE_D));

      /* For (gl_FrontFacing ? 1.0 : -1.0), emit:
       *
       *    or(8)  tmp<1>D  g1.6<0,1,0>D  0x3f800000D
       *    and(8) dst<1>D  tmp<8,8,1>D   0xbf800000D
       *
       * and negate g1.6<0,1,0>D for (gl_FrontFacing ? -1.0 : 1.0).
       *
       * This negation looks like it's safe in practice, because bits 0:4 will
       * surely be TRIANGLES
       */

      if (value1 == -1.0f) {
         g1_6.negate = true;
      }

      ntb.bld.OR(tmp, g1_6, elk_imm_d(0x3f800000));
   }
   ntb.bld.AND(retype(result, ELK_REGISTER_TYPE_D), tmp, elk_imm_d(0xbf800000));

   return true;
}

static elk_rnd_mode
elk_rnd_mode_from_nir_op (const nir_op op) {
   switch (op) {
   case nir_op_f2f16_rtz:
      return ELK_RND_MODE_RTZ;
   case nir_op_f2f16_rtne:
      return ELK_RND_MODE_RTNE;
   default:
      unreachable("Operation doesn't support rounding mode");
   }
}

static elk_rnd_mode
elk_rnd_mode_from_execution_mode(unsigned execution_mode)
{
   if (nir_has_any_rounding_mode_rtne(execution_mode))
      return ELK_RND_MODE_RTNE;
   if (nir_has_any_rounding_mode_rtz(execution_mode))
      return ELK_RND_MODE_RTZ;
   return ELK_RND_MODE_UNSPECIFIED;
}

static elk_fs_reg
prepare_alu_destination_and_sources(nir_to_elk_state &ntb,
                                    const fs_builder &bld,
                                    nir_alu_instr *instr,
                                    elk_fs_reg *op,
                                    bool need_dest)
{
   const intel_device_info *devinfo = ntb.devinfo;

   elk_fs_reg result =
      need_dest ? get_nir_def(ntb, instr->def) : bld.null_reg_ud();

   result.type = elk_type_for_nir_type(devinfo,
      (nir_alu_type)(nir_op_infos[instr->op].output_type |
                     instr->def.bit_size));

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      op[i] = get_nir_src(ntb, instr->src[i].src);
      op[i].type = elk_type_for_nir_type(devinfo,
         (nir_alu_type)(nir_op_infos[instr->op].input_types[i] |
                        nir_src_bit_size(instr->src[i].src)));
   }

   /* Move and vecN instrutions may still be vectored.  Return the raw,
    * vectored source and destination so that elk_fs_visitor::nir_emit_alu can
    * handle it.  Other callers should not have to handle these kinds of
    * instructions.
    */
   switch (instr->op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_vec8:
   case nir_op_vec16:
      return result;
   default:
      break;
   }

   /* At this point, we have dealt with any instruction that operates on
    * more than a single channel.  Therefore, we can just adjust the source
    * and destination registers for that channel and emit the instruction.
    */
   unsigned channel = 0;
   if (nir_op_infos[instr->op].output_size == 0) {
      /* Since NIR is doing the scalarizing for us, we should only ever see
       * vectorized operations with a single channel.
       */
      nir_component_mask_t write_mask = get_nir_write_mask(instr->def);
      assert(util_bitcount(write_mask) == 1);
      channel = ffs(write_mask) - 1;

      result = offset(result, bld, channel);
   }

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      assert(nir_op_infos[instr->op].input_sizes[i] < 2);
      op[i] = offset(op[i], bld, instr->src[i].swizzle[channel]);
   }

   return result;
}

static elk_fs_reg
resolve_source_modifiers(const fs_builder &bld, const elk_fs_reg &src)
{
   if (!src.abs && !src.negate)
      return src;

   elk_fs_reg temp = bld.vgrf(src.type);
   bld.MOV(temp, src);

   return temp;
}

static void
resolve_inot_sources(nir_to_elk_state &ntb, const fs_builder &bld, nir_alu_instr *instr,
                     elk_fs_reg *op)
{
   for (unsigned i = 0; i < 2; i++) {
      nir_alu_instr *inot_instr = nir_src_as_alu_instr(instr->src[i].src);

      if (inot_instr != NULL && inot_instr->op == nir_op_inot) {
         /* The source of the inot is now the source of instr. */
         prepare_alu_destination_and_sources(ntb, bld, inot_instr, &op[i], false);

         assert(!op[i].negate);
         op[i].negate = true;
      } else {
         op[i] = resolve_source_modifiers(bld, op[i]);
      }
   }
}

static bool
try_emit_b2fi_of_inot(nir_to_elk_state &ntb, const fs_builder &bld,
                      elk_fs_reg result,
                      nir_alu_instr *instr)
{
   const intel_device_info *devinfo = bld.shader->devinfo;

   if (devinfo->ver < 6)
      return false;

   nir_alu_instr *inot_instr = nir_src_as_alu_instr(instr->src[0].src);

   if (inot_instr == NULL || inot_instr->op != nir_op_inot)
      return false;

   /* HF is also possible as a destination on BDW+.  For nir_op_b2i, the set
    * of valid size-changing combinations is a bit more complex.
    *
    * The source restriction is just because I was lazy about generating the
    * constant below.
    */
   if (instr->def.bit_size != 32 ||
       nir_src_bit_size(inot_instr->src[0].src) != 32)
      return false;

   /* b2[fi](inot(a)) maps a=0 => 1, a=-1 => 0.  Since a can only be 0 or -1,
    * this is float(1 + a).
    */
   elk_fs_reg op;

   prepare_alu_destination_and_sources(ntb, bld, inot_instr, &op, false);

   /* Ignore the saturate modifier, if there is one.  The result of the
    * arithmetic can only be 0 or 1, so the clamping will do nothing anyway.
    */
   bld.ADD(result, op, elk_imm_d(1));

   return true;
}

/**
 * Emit code for nir_op_fsign possibly fused with a nir_op_fmul
 *
 * If \c instr is not the \c nir_op_fsign, then \c fsign_src is the index of
 * the source of \c instr that is a \c nir_op_fsign.
 */
static void
emit_fsign(nir_to_elk_state &ntb, const fs_builder &bld, const nir_alu_instr *instr,
           elk_fs_reg result, elk_fs_reg *op, unsigned fsign_src)
{
   const intel_device_info *devinfo = ntb.devinfo;

   elk_fs_inst *inst;

   assert(instr->op == nir_op_fsign || instr->op == nir_op_fmul);
   assert(fsign_src < nir_op_infos[instr->op].num_inputs);

   if (instr->op != nir_op_fsign) {
      const nir_alu_instr *const fsign_instr =
         nir_src_as_alu_instr(instr->src[fsign_src].src);

      /* op[fsign_src] has the nominal result of the fsign, and op[1 -
       * fsign_src] has the other multiply source.  This must be rearranged so
       * that op[0] is the source of the fsign op[1] is the other multiply
       * source.
       */
      if (fsign_src != 0)
         op[1] = op[0];

      op[0] = get_nir_src(ntb, fsign_instr->src[0].src);

      const nir_alu_type t =
         (nir_alu_type)(nir_op_infos[instr->op].input_types[0] |
                        nir_src_bit_size(fsign_instr->src[0].src));

      op[0].type = elk_type_for_nir_type(devinfo, t);

      unsigned channel = 0;
      if (nir_op_infos[instr->op].output_size == 0) {
         /* Since NIR is doing the scalarizing for us, we should only ever see
          * vectorized operations with a single channel.
          */
         nir_component_mask_t write_mask = get_nir_write_mask(instr->def);
         assert(util_bitcount(write_mask) == 1);
         channel = ffs(write_mask) - 1;
      }

      op[0] = offset(op[0], bld, fsign_instr->src[0].swizzle[channel]);
   }

   if (type_sz(op[0].type) == 2) {
      /* AND(val, 0x8000) gives the sign bit.
       *
       * Predicated OR ORs 1.0 (0x3c00) with the sign bit if val is not zero.
       */
      elk_fs_reg zero = retype(elk_imm_uw(0), ELK_REGISTER_TYPE_HF);
      bld.CMP(bld.null_reg_f(), op[0], zero, ELK_CONDITIONAL_NZ);

      op[0].type = ELK_REGISTER_TYPE_UW;
      result.type = ELK_REGISTER_TYPE_UW;
      bld.AND(result, op[0], elk_imm_uw(0x8000u));

      if (instr->op == nir_op_fsign)
         inst = bld.OR(result, result, elk_imm_uw(0x3c00u));
      else {
         /* Use XOR here to get the result sign correct. */
         inst = bld.XOR(result, result, retype(op[1], ELK_REGISTER_TYPE_UW));
      }

      inst->predicate = ELK_PREDICATE_NORMAL;
   } else if (type_sz(op[0].type) == 4) {
      /* AND(val, 0x80000000) gives the sign bit.
       *
       * Predicated OR ORs 1.0 (0x3f800000) with the sign bit if val is not
       * zero.
       */
      bld.CMP(bld.null_reg_f(), op[0], elk_imm_f(0.0f), ELK_CONDITIONAL_NZ);

      op[0].type = ELK_REGISTER_TYPE_UD;
      result.type = ELK_REGISTER_TYPE_UD;
      bld.AND(result, op[0], elk_imm_ud(0x80000000u));

      if (instr->op == nir_op_fsign)
         inst = bld.OR(result, result, elk_imm_ud(0x3f800000u));
      else {
         /* Use XOR here to get the result sign correct. */
         inst = bld.XOR(result, result, retype(op[1], ELK_REGISTER_TYPE_UD));
      }

      inst->predicate = ELK_PREDICATE_NORMAL;
   } else {
      unreachable("Should have been lowered by nir_opt_algebraic.");
   }
}

/**
 * Determine whether sources of a nir_op_fmul can be fused with a nir_op_fsign
 *
 * Checks the operands of a \c nir_op_fmul to determine whether or not
 * \c emit_fsign could fuse the multiplication with the \c sign() calculation.
 *
 * \param instr  The multiplication instruction
 *
 * \param fsign_src The source of \c instr that may or may not be a
 *                  \c nir_op_fsign
 */
static bool
can_fuse_fmul_fsign(nir_alu_instr *instr, unsigned fsign_src)
{
   assert(instr->op == nir_op_fmul);

   nir_alu_instr *const fsign_instr =
      nir_src_as_alu_instr(instr->src[fsign_src].src);

   /* Rules:
    *
    * 1. instr->src[fsign_src] must be a nir_op_fsign.
    * 2. The nir_op_fsign can only be used by this multiplication.
    * 3. The source that is the nir_op_fsign does not have source modifiers.
    *    \c emit_fsign only examines the source modifiers of the source of the
    *    \c nir_op_fsign.
    *
    * The nir_op_fsign must also not have the saturate modifier, but steps
    * have already been taken (in nir_opt_algebraic) to ensure that.
    */
   return fsign_instr != NULL && fsign_instr->op == nir_op_fsign &&
          is_used_once(fsign_instr);
}

static bool
is_const_zero(const nir_src &src)
{
   return nir_src_is_const(src) && nir_src_as_int(src) == 0;
}

static void
fs_nir_emit_alu(nir_to_elk_state &ntb, nir_alu_instr *instr,
                bool need_dest)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   elk_fs_inst *inst;
   unsigned execution_mode =
      bld.shader->nir->info.float_controls_execution_mode;

   elk_fs_reg op[NIR_MAX_VEC_COMPONENTS];
   elk_fs_reg result = prepare_alu_destination_and_sources(ntb, bld, instr, op, need_dest);

#ifndef NDEBUG
   /* Everything except raw moves, some type conversions, iabs, and ineg
    * should have 8-bit sources lowered by nir_lower_bit_size in
    * elk_preprocess_nir or by elk_nir_lower_conversions in
    * elk_postprocess_nir.
    */
   switch (instr->op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_vec8:
   case nir_op_vec16:
   case nir_op_i2f16:
   case nir_op_i2f32:
   case nir_op_i2i16:
   case nir_op_i2i32:
   case nir_op_u2f16:
   case nir_op_u2f32:
   case nir_op_u2u16:
   case nir_op_u2u32:
   case nir_op_iabs:
   case nir_op_ineg:
   case nir_op_pack_32_4x8_split:
      break;

   default:
      for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
         assert(type_sz(op[i].type) > 1);
      }
   }
#endif

   switch (instr->op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_vec8:
   case nir_op_vec16: {
      elk_fs_reg temp = result;
      bool need_extra_copy = false;

      nir_intrinsic_instr *store_reg =
         nir_store_reg_for_def(&instr->def);
      if (store_reg != NULL) {
         nir_def *dest_reg = store_reg->src[1].ssa;
         for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
            nir_intrinsic_instr *load_reg =
               nir_load_reg_for_def(instr->src[i].src.ssa);
            if (load_reg == NULL)
               continue;

            if (load_reg->src[0].ssa == dest_reg) {
               need_extra_copy = true;
               temp = bld.vgrf(result.type, 4);
               break;
            }
         }
      }

      nir_component_mask_t write_mask = get_nir_write_mask(instr->def);
      unsigned last_bit = util_last_bit(write_mask);

      for (unsigned i = 0; i < last_bit; i++) {
         if (!(write_mask & (1 << i)))
            continue;

         if (instr->op == nir_op_mov) {
            bld.MOV(offset(temp, bld, i),
                           offset(op[0], bld, instr->src[0].swizzle[i]));
         } else {
            bld.MOV(offset(temp, bld, i),
                           offset(op[i], bld, instr->src[i].swizzle[0]));
         }
      }

      /* In this case the source and destination registers were the same,
       * so we need to insert an extra set of moves in order to deal with
       * any swizzling.
       */
      if (need_extra_copy) {
         for (unsigned i = 0; i < last_bit; i++) {
            if (!(write_mask & (1 << i)))
               continue;

            bld.MOV(offset(result, bld, i), offset(temp, bld, i));
         }
      }
      return;
   }

   case nir_op_i2f32:
   case nir_op_u2f32:
      if (optimize_extract_to_float(ntb, instr, result))
         return;
      inst = bld.MOV(result, op[0]);
      break;

   case nir_op_f2f16_rtne:
   case nir_op_f2f16_rtz:
   case nir_op_f2f16: {
      elk_rnd_mode rnd = ELK_RND_MODE_UNSPECIFIED;

      if (nir_op_f2f16 == instr->op)
         rnd = elk_rnd_mode_from_execution_mode(execution_mode);
      else
         rnd = elk_rnd_mode_from_nir_op(instr->op);

      if (ELK_RND_MODE_UNSPECIFIED != rnd)
         bld.exec_all().emit(ELK_SHADER_OPCODE_RND_MODE, bld.null_reg_ud(), elk_imm_d(rnd));

      assert(type_sz(op[0].type) < 8); /* elk_nir_lower_conversions */
      inst = bld.F32TO16(result, op[0]);
      break;
   }

   case nir_op_b2i8:
   case nir_op_b2i16:
   case nir_op_b2i32:
   case nir_op_b2i64:
   case nir_op_b2f16:
   case nir_op_b2f32:
   case nir_op_b2f64:
      if (try_emit_b2fi_of_inot(ntb, bld, result, instr))
         break;
      op[0].type = ELK_REGISTER_TYPE_D;
      op[0].negate = !op[0].negate;
      FALLTHROUGH;
   case nir_op_i2f64:
   case nir_op_i2i64:
   case nir_op_u2f64:
   case nir_op_u2u64:
   case nir_op_f2f64:
   case nir_op_f2i64:
   case nir_op_f2u64:
   case nir_op_i2i32:
   case nir_op_u2u32:
   case nir_op_f2i32:
   case nir_op_f2u32:
   case nir_op_i2f16:
   case nir_op_u2f16:
   case nir_op_f2i16:
   case nir_op_f2u16:
   case nir_op_f2i8:
   case nir_op_f2u8:
      if (result.type == ELK_REGISTER_TYPE_B ||
          result.type == ELK_REGISTER_TYPE_UB ||
          result.type == ELK_REGISTER_TYPE_HF)
         assert(type_sz(op[0].type) < 8); /* elk_nir_lower_conversions */

      if (op[0].type == ELK_REGISTER_TYPE_B ||
          op[0].type == ELK_REGISTER_TYPE_UB ||
          op[0].type == ELK_REGISTER_TYPE_HF)
         assert(type_sz(result.type) < 8); /* elk_nir_lower_conversions */

      inst = bld.MOV(result, op[0]);
      break;

   case nir_op_i2i8:
   case nir_op_u2u8:
      assert(type_sz(op[0].type) < 8); /* elk_nir_lower_conversions */
      FALLTHROUGH;
   case nir_op_i2i16:
   case nir_op_u2u16: {
      /* Emit better code for u2u8(extract_u8(a, b)) and similar patterns.
       * Emitting the instructions one by one results in two MOV instructions
       * that won't be propagated.  By handling both instructions here, a
       * single MOV is emitted.
       */
      nir_alu_instr *extract_instr = nir_src_as_alu_instr(instr->src[0].src);
      if (extract_instr != NULL) {
         if (extract_instr->op == nir_op_extract_u8 ||
             extract_instr->op == nir_op_extract_i8) {
            prepare_alu_destination_and_sources(ntb, bld, extract_instr, op, false);

            const unsigned byte = nir_src_as_uint(extract_instr->src[1].src);
            const elk_reg_type type =
               elk_int_type(1, extract_instr->op == nir_op_extract_i8);

            op[0] = subscript(op[0], type, byte);
         } else if (extract_instr->op == nir_op_extract_u16 ||
                    extract_instr->op == nir_op_extract_i16) {
            prepare_alu_destination_and_sources(ntb, bld, extract_instr, op, false);

            const unsigned word = nir_src_as_uint(extract_instr->src[1].src);
            const elk_reg_type type =
               elk_int_type(2, extract_instr->op == nir_op_extract_i16);

            op[0] = subscript(op[0], type, word);
         }
      }

      inst = bld.MOV(result, op[0]);
      break;
   }

   case nir_op_fsat:
      inst = bld.MOV(result, op[0]);
      inst->saturate = true;
      break;

   case nir_op_fneg:
   case nir_op_ineg:
      op[0].negate = true;
      inst = bld.MOV(result, op[0]);
      break;

   case nir_op_fabs:
   case nir_op_iabs:
      op[0].negate = false;
      op[0].abs = true;
      inst = bld.MOV(result, op[0]);
      break;

   case nir_op_f2f32:
      if (nir_has_any_rounding_mode_enabled(execution_mode)) {
         elk_rnd_mode rnd =
            elk_rnd_mode_from_execution_mode(execution_mode);
         bld.exec_all().emit(ELK_SHADER_OPCODE_RND_MODE, bld.null_reg_ud(),
                             elk_imm_d(rnd));
      }

      if (op[0].type == ELK_REGISTER_TYPE_HF)
         assert(type_sz(result.type) < 8); /* elk_nir_lower_conversions */

      inst = bld.MOV(result, op[0]);
      break;

   case nir_op_fsign:
      emit_fsign(ntb, bld, instr, result, op, 0);
      break;

   case nir_op_frcp:
      inst = bld.emit(ELK_SHADER_OPCODE_RCP, result, op[0]);
      break;

   case nir_op_fexp2:
      inst = bld.emit(ELK_SHADER_OPCODE_EXP2, result, op[0]);
      break;

   case nir_op_flog2:
      inst = bld.emit(ELK_SHADER_OPCODE_LOG2, result, op[0]);
      break;

   case nir_op_fsin:
      inst = bld.emit(ELK_SHADER_OPCODE_SIN, result, op[0]);
      break;

   case nir_op_fcos:
      inst = bld.emit(ELK_SHADER_OPCODE_COS, result, op[0]);
      break;

   case nir_op_fddx_fine:
      inst = bld.emit(ELK_FS_OPCODE_DDX_FINE, result, op[0]);
      break;
   case nir_op_fddx:
   case nir_op_fddx_coarse:
      inst = bld.emit(ELK_FS_OPCODE_DDX_COARSE, result, op[0]);
      break;
   case nir_op_fddy_fine:
      inst = bld.emit(ELK_FS_OPCODE_DDY_FINE, result, op[0]);
      break;
   case nir_op_fddy:
   case nir_op_fddy_coarse:
      inst = bld.emit(ELK_FS_OPCODE_DDY_COARSE, result, op[0]);
      break;

   case nir_op_fadd:
      if (nir_has_any_rounding_mode_enabled(execution_mode)) {
         elk_rnd_mode rnd =
            elk_rnd_mode_from_execution_mode(execution_mode);
         bld.exec_all().emit(ELK_SHADER_OPCODE_RND_MODE, bld.null_reg_ud(),
                             elk_imm_d(rnd));
      }
      FALLTHROUGH;
   case nir_op_iadd:
      inst = bld.ADD(result, op[0], op[1]);
      break;

   case nir_op_iadd_sat:
   case nir_op_uadd_sat:
      inst = bld.ADD(result, op[0], op[1]);
      inst->saturate = true;
      break;

   case nir_op_isub_sat:
      bld.emit(ELK_SHADER_OPCODE_ISUB_SAT, result, op[0], op[1]);
      break;

   case nir_op_usub_sat:
      bld.emit(ELK_SHADER_OPCODE_USUB_SAT, result, op[0], op[1]);
      break;

   case nir_op_irhadd:
   case nir_op_urhadd:
      assert(instr->def.bit_size < 64);
      inst = bld.AVG(result, op[0], op[1]);
      break;

   case nir_op_ihadd:
   case nir_op_uhadd: {
      assert(instr->def.bit_size < 64);
      elk_fs_reg tmp = bld.vgrf(result.type);

      if (devinfo->ver >= 8) {
         op[0] = resolve_source_modifiers(bld, op[0]);
         op[1] = resolve_source_modifiers(bld, op[1]);
      }

      /* AVG(x, y) - ((x ^ y) & 1) */
      bld.XOR(tmp, op[0], op[1]);
      bld.AND(tmp, tmp, retype(elk_imm_ud(1), result.type));
      bld.AVG(result, op[0], op[1]);
      inst = bld.ADD(result, result, tmp);
      inst->src[1].negate = true;
      break;
   }

   case nir_op_fmul:
      for (unsigned i = 0; i < 2; i++) {
         if (can_fuse_fmul_fsign(instr, i)) {
            emit_fsign(ntb, bld, instr, result, op, i);
            return;
         }
      }

      /* We emit the rounding mode after the previous fsign optimization since
       * it won't result in a MUL, but will try to negate the value by other
       * means.
       */
      if (nir_has_any_rounding_mode_enabled(execution_mode)) {
         elk_rnd_mode rnd =
            elk_rnd_mode_from_execution_mode(execution_mode);
         bld.exec_all().emit(ELK_SHADER_OPCODE_RND_MODE, bld.null_reg_ud(),
                             elk_imm_d(rnd));
      }

      inst = bld.MUL(result, op[0], op[1]);
      break;

   case nir_op_imul_2x32_64:
   case nir_op_umul_2x32_64:
      bld.MUL(result, op[0], op[1]);
      break;

   case nir_op_imul_32x16:
   case nir_op_umul_32x16: {
      const bool ud = instr->op == nir_op_umul_32x16;
      const enum elk_reg_type word_type =
         ud ? ELK_REGISTER_TYPE_UW : ELK_REGISTER_TYPE_W;
      const enum elk_reg_type dword_type =
         ud ? ELK_REGISTER_TYPE_UD : ELK_REGISTER_TYPE_D;

      assert(instr->def.bit_size == 32);

      /* Before copy propagation there are no immediate values. */
      assert(op[0].file != IMM && op[1].file != IMM);

      op[1] = subscript(op[1], word_type, 0);

      if (devinfo->ver >= 7)
         bld.MUL(result, retype(op[0], dword_type), op[1]);
      else
         bld.MUL(result, op[1], retype(op[0], dword_type));

      break;
   }

   case nir_op_imul:
      assert(instr->def.bit_size < 64);
      bld.MUL(result, op[0], op[1]);
      break;

   case nir_op_imul_high:
   case nir_op_umul_high:
      assert(instr->def.bit_size < 64);
      if (instr->def.bit_size == 32) {
         bld.emit(ELK_SHADER_OPCODE_MULH, result, op[0], op[1]);
      } else {
         elk_fs_reg tmp = bld.vgrf(elk_reg_type_from_bit_size(32, op[0].type));
         bld.MUL(tmp, op[0], op[1]);
         bld.MOV(result, subscript(tmp, result.type, 1));
      }
      break;

   case nir_op_idiv:
   case nir_op_udiv:
      assert(instr->def.bit_size < 64);
      bld.emit(ELK_SHADER_OPCODE_INT_QUOTIENT, result, op[0], op[1]);
      break;

   case nir_op_uadd_carry:
      unreachable("Should have been lowered by carry_to_arith().");

   case nir_op_usub_borrow:
      unreachable("Should have been lowered by borrow_to_arith().");

   case nir_op_umod:
   case nir_op_irem:
      /* According to the sign table for INT DIV in the Ivy Bridge PRM, it
       * appears that our hardware just does the right thing for signed
       * remainder.
       */
      assert(instr->def.bit_size < 64);
      bld.emit(ELK_SHADER_OPCODE_INT_REMAINDER, result, op[0], op[1]);
      break;

   case nir_op_imod: {
      /* Get a regular C-style remainder.  If a % b == 0, set the predicate. */
      bld.emit(ELK_SHADER_OPCODE_INT_REMAINDER, result, op[0], op[1]);

      /* Math instructions don't support conditional mod */
      inst = bld.MOV(bld.null_reg_d(), result);
      inst->conditional_mod = ELK_CONDITIONAL_NZ;

      /* Now, we need to determine if signs of the sources are different.
       * When we XOR the sources, the top bit is 0 if they are the same and 1
       * if they are different.  We can then use a conditional modifier to
       * turn that into a predicate.  This leads us to an XOR.l instruction.
       *
       * Technically, according to the PRM, you're not allowed to use .l on a
       * XOR instruction.  However, empirical experiments and Curro's reading
       * of the simulator source both indicate that it's safe.
       */
      elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_D);
      inst = bld.XOR(tmp, op[0], op[1]);
      inst->predicate = ELK_PREDICATE_NORMAL;
      inst->conditional_mod = ELK_CONDITIONAL_L;

      /* If the result of the initial remainder operation is non-zero and the
       * two sources have different signs, add in a copy of op[1] to get the
       * final integer modulus value.
       */
      inst = bld.ADD(result, result, op[1]);
      inst->predicate = ELK_PREDICATE_NORMAL;
      break;
   }

   case nir_op_flt32:
   case nir_op_fge32:
   case nir_op_feq32:
   case nir_op_fneu32: {
      elk_fs_reg dest = result;

      const uint32_t bit_size =  nir_src_bit_size(instr->src[0].src);
      if (bit_size != 32) {
         dest = bld.vgrf(op[0].type, 1);
         bld.UNDEF(dest);
      }

      bld.CMP(dest, op[0], op[1], elk_cmod_for_nir_comparison(instr->op));

      if (bit_size > 32) {
         bld.MOV(result, subscript(dest, ELK_REGISTER_TYPE_UD, 0));
      } else if(bit_size < 32) {
         /* When we convert the result to 32-bit we need to be careful and do
          * it as a signed conversion to get sign extension (for 32-bit true)
          */
         const elk_reg_type src_type =
            elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_D);

         bld.MOV(retype(result, ELK_REGISTER_TYPE_D), retype(dest, src_type));
      }
      break;
   }

   case nir_op_ilt32:
   case nir_op_ult32:
   case nir_op_ige32:
   case nir_op_uge32:
   case nir_op_ieq32:
   case nir_op_ine32: {
      elk_fs_reg dest = result;

      const uint32_t bit_size = type_sz(op[0].type) * 8;
      if (bit_size != 32) {
         dest = bld.vgrf(op[0].type, 1);
         bld.UNDEF(dest);
      }

      bld.CMP(dest, op[0], op[1],
              elk_cmod_for_nir_comparison(instr->op));

      if (bit_size > 32) {
         bld.MOV(result, subscript(dest, ELK_REGISTER_TYPE_UD, 0));
      } else if (bit_size < 32) {
         /* When we convert the result to 32-bit we need to be careful and do
          * it as a signed conversion to get sign extension (for 32-bit true)
          */
         const elk_reg_type src_type =
            elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_D);

         bld.MOV(retype(result, ELK_REGISTER_TYPE_D), retype(dest, src_type));
      }
      break;
   }

   case nir_op_inot:
      if (devinfo->ver >= 8) {
         nir_alu_instr *inot_src_instr = nir_src_as_alu_instr(instr->src[0].src);

         if (inot_src_instr != NULL &&
             (inot_src_instr->op == nir_op_ior ||
              inot_src_instr->op == nir_op_ixor ||
              inot_src_instr->op == nir_op_iand)) {
            /* The sources of the source logical instruction are now the
             * sources of the instruction that will be generated.
             */
            prepare_alu_destination_and_sources(ntb, bld, inot_src_instr, op, false);
            resolve_inot_sources(ntb, bld, inot_src_instr, op);

            /* Smash all of the sources and destination to be signed.  This
             * doesn't matter for the operation of the instruction, but cmod
             * propagation fails on unsigned sources with negation (due to
             * elk_fs_inst::can_do_cmod returning false).
             */
            result.type =
               elk_type_for_nir_type(devinfo,
                                     (nir_alu_type)(nir_type_int |
                                                    instr->def.bit_size));
            op[0].type =
               elk_type_for_nir_type(devinfo,
                                     (nir_alu_type)(nir_type_int |
                                                    nir_src_bit_size(inot_src_instr->src[0].src)));
            op[1].type =
               elk_type_for_nir_type(devinfo,
                                     (nir_alu_type)(nir_type_int |
                                                    nir_src_bit_size(inot_src_instr->src[1].src)));

            /* For XOR, only invert one of the sources.  Arbitrarily choose
             * the first source.
             */
            op[0].negate = !op[0].negate;
            if (inot_src_instr->op != nir_op_ixor)
               op[1].negate = !op[1].negate;

            switch (inot_src_instr->op) {
            case nir_op_ior:
               bld.AND(result, op[0], op[1]);
               return;

            case nir_op_iand:
               bld.OR(result, op[0], op[1]);
               return;

            case nir_op_ixor:
               bld.XOR(result, op[0], op[1]);
               return;

            default:
               unreachable("impossible opcode");
            }
         }
         op[0] = resolve_source_modifiers(bld, op[0]);
      }
      bld.NOT(result, op[0]);
      break;
   case nir_op_ixor:
      if (devinfo->ver >= 8) {
         resolve_inot_sources(ntb, bld, instr, op);
      }
      bld.XOR(result, op[0], op[1]);
      break;
   case nir_op_ior:
      if (devinfo->ver >= 8) {
         resolve_inot_sources(ntb, bld, instr, op);
      }
      bld.OR(result, op[0], op[1]);
      break;
   case nir_op_iand:
      if (devinfo->ver >= 8) {
         resolve_inot_sources(ntb, bld, instr, op);
      }
      bld.AND(result, op[0], op[1]);
      break;

   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_b32all_fequal2:
   case nir_op_b32all_iequal2:
   case nir_op_b32all_fequal3:
   case nir_op_b32all_iequal3:
   case nir_op_b32all_fequal4:
   case nir_op_b32all_iequal4:
   case nir_op_b32any_fnequal2:
   case nir_op_b32any_inequal2:
   case nir_op_b32any_fnequal3:
   case nir_op_b32any_inequal3:
   case nir_op_b32any_fnequal4:
   case nir_op_b32any_inequal4:
      unreachable("Lowered by nir_lower_alu_reductions");

   case nir_op_ldexp:
      unreachable("not reached: should be handled by ldexp_to_arith()");

   case nir_op_fsqrt:
      inst = bld.emit(ELK_SHADER_OPCODE_SQRT, result, op[0]);
      break;

   case nir_op_frsq:
      inst = bld.emit(ELK_SHADER_OPCODE_RSQ, result, op[0]);
      break;

   case nir_op_ftrunc:
      inst = bld.RNDZ(result, op[0]);
      if (devinfo->ver < 6) {
         set_condmod(ELK_CONDITIONAL_R, inst);
         set_predicate(ELK_PREDICATE_NORMAL,
                       bld.ADD(result, result, elk_imm_f(1.0f)));
         inst = bld.MOV(result, result); /* for potential saturation */
      }
      break;

   case nir_op_fceil: {
      op[0].negate = !op[0].negate;
      elk_fs_reg temp = s.vgrf(glsl_float_type());
      bld.RNDD(temp, op[0]);
      temp.negate = true;
      inst = bld.MOV(result, temp);
      break;
   }
   case nir_op_ffloor:
      inst = bld.RNDD(result, op[0]);
      break;
   case nir_op_ffract:
      inst = bld.FRC(result, op[0]);
      break;
   case nir_op_fround_even:
      inst = bld.RNDE(result, op[0]);
      if (devinfo->ver < 6) {
         set_condmod(ELK_CONDITIONAL_R, inst);
         set_predicate(ELK_PREDICATE_NORMAL,
                       bld.ADD(result, result, elk_imm_f(1.0f)));
         inst = bld.MOV(result, result); /* for potential saturation */
      }
      break;

   case nir_op_fquantize2f16: {
      elk_fs_reg tmp16 = bld.vgrf(ELK_REGISTER_TYPE_D);
      elk_fs_reg tmp32 = bld.vgrf(ELK_REGISTER_TYPE_F);
      elk_fs_reg zero = bld.vgrf(ELK_REGISTER_TYPE_F);

      /* The destination stride must be at least as big as the source stride. */
      tmp16 = subscript(tmp16, ELK_REGISTER_TYPE_HF, 0);

      /* Check for denormal */
      elk_fs_reg abs_src0 = op[0];
      abs_src0.abs = true;
      bld.CMP(bld.null_reg_f(), abs_src0, elk_imm_f(ldexpf(1.0, -14)),
              ELK_CONDITIONAL_L);
      /* Get the appropriately signed zero */
      bld.AND(retype(zero, ELK_REGISTER_TYPE_UD),
              retype(op[0], ELK_REGISTER_TYPE_UD),
              elk_imm_ud(0x80000000));
      /* Do the actual F32 -> F16 -> F32 conversion */
      bld.F32TO16(tmp16, op[0]);
      bld.F16TO32(tmp32, tmp16);
      /* Select that or zero based on normal status */
      inst = bld.SEL(result, zero, tmp32);
      inst->predicate = ELK_PREDICATE_NORMAL;
      break;
   }

   case nir_op_imin:
   case nir_op_umin:
   case nir_op_fmin:
      inst = bld.emit_minmax(result, op[0], op[1], ELK_CONDITIONAL_L);
      break;

   case nir_op_imax:
   case nir_op_umax:
   case nir_op_fmax:
      inst = bld.emit_minmax(result, op[0], op[1], ELK_CONDITIONAL_GE);
      break;

   case nir_op_pack_snorm_2x16:
   case nir_op_pack_snorm_4x8:
   case nir_op_pack_unorm_2x16:
   case nir_op_pack_unorm_4x8:
   case nir_op_unpack_snorm_2x16:
   case nir_op_unpack_snorm_4x8:
   case nir_op_unpack_unorm_2x16:
   case nir_op_unpack_unorm_4x8:
   case nir_op_unpack_half_2x16:
   case nir_op_pack_half_2x16:
      unreachable("not reached: should be handled by lower_packing_builtins");

   case nir_op_unpack_half_2x16_split_x:
      inst = bld.F16TO32(result, subscript(op[0], ELK_REGISTER_TYPE_HF, 0));
      break;

   case nir_op_unpack_half_2x16_split_y:
      inst = bld.F16TO32(result, subscript(op[0], ELK_REGISTER_TYPE_HF, 1));
      break;

   case nir_op_pack_64_2x32_split:
   case nir_op_pack_32_2x16_split:
      bld.emit(ELK_FS_OPCODE_PACK, result, op[0], op[1]);
      break;

   case nir_op_pack_32_4x8_split:
      bld.emit(ELK_FS_OPCODE_PACK, result, op, 4);
      break;

   case nir_op_unpack_64_2x32_split_x:
   case nir_op_unpack_64_2x32_split_y: {
      if (instr->op == nir_op_unpack_64_2x32_split_x)
         bld.MOV(result, subscript(op[0], ELK_REGISTER_TYPE_UD, 0));
      else
         bld.MOV(result, subscript(op[0], ELK_REGISTER_TYPE_UD, 1));
      break;
   }

   case nir_op_unpack_32_2x16_split_x:
   case nir_op_unpack_32_2x16_split_y: {
      if (instr->op == nir_op_unpack_32_2x16_split_x)
         bld.MOV(result, subscript(op[0], ELK_REGISTER_TYPE_UW, 0));
      else
         bld.MOV(result, subscript(op[0], ELK_REGISTER_TYPE_UW, 1));
      break;
   }

   case nir_op_fpow:
      inst = bld.emit(ELK_SHADER_OPCODE_POW, result, op[0], op[1]);
      break;

   case nir_op_bitfield_reverse:
      assert(instr->def.bit_size == 32);
      assert(nir_src_bit_size(instr->src[0].src) == 32);
      bld.BFREV(result, op[0]);
      break;

   case nir_op_bit_count:
      assert(instr->def.bit_size == 32);
      assert(nir_src_bit_size(instr->src[0].src) < 64);
      bld.CBIT(result, op[0]);
      break;

   case nir_op_uclz:
      assert(instr->def.bit_size == 32);
      assert(nir_src_bit_size(instr->src[0].src) == 32);
      bld.LZD(retype(result, ELK_REGISTER_TYPE_UD), op[0]);
      break;

   case nir_op_ifind_msb: {
      assert(instr->def.bit_size == 32);
      assert(nir_src_bit_size(instr->src[0].src) == 32);
      assert(devinfo->ver >= 7);

      bld.FBH(retype(result, ELK_REGISTER_TYPE_UD), op[0]);

      /* FBH counts from the MSB side, while GLSL's findMSB() wants the count
       * from the LSB side. If FBH didn't return an error (0xFFFFFFFF), then
       * subtract the result from 31 to convert the MSB count into an LSB
       * count.
       */
      bld.CMP(bld.null_reg_d(), result, elk_imm_d(-1), ELK_CONDITIONAL_NZ);

      inst = bld.ADD(result, result, elk_imm_d(31));
      inst->predicate = ELK_PREDICATE_NORMAL;
      inst->src[0].negate = true;
      break;
   }

   case nir_op_find_lsb:
      assert(instr->def.bit_size == 32);
      assert(nir_src_bit_size(instr->src[0].src) == 32);
      assert(devinfo->ver >= 7);
      bld.FBL(result, op[0]);
      break;

   case nir_op_ubitfield_extract:
   case nir_op_ibitfield_extract:
      unreachable("should have been lowered");
   case nir_op_ubfe:
   case nir_op_ibfe:
      assert(instr->def.bit_size < 64);
      bld.BFE(result, op[2], op[1], op[0]);
      break;
   case nir_op_bfm:
      assert(instr->def.bit_size < 64);
      bld.BFI1(result, op[0], op[1]);
      break;
   case nir_op_bfi:
      assert(instr->def.bit_size < 64);

      /* bfi is ((...) | (~src0 & src2)). The second part is zero when src2 is
       * either 0 or src0. Replacing the 0 with another value can eliminate a
       * temporary register.
       */
      if (is_const_zero(instr->src[2].src))
         bld.BFI2(result, op[0], op[1], op[0]);
      else
         bld.BFI2(result, op[0], op[1], op[2]);

      break;

   case nir_op_bitfield_insert:
      unreachable("not reached: should have been lowered");

   /* With regards to implicit masking of the shift counts for 8- and 16-bit
    * types, the PRMs are **incorrect**. They falsely state that on Gen9+ only
    * the low bits of src1 matching the size of src0 (e.g., 4-bits for W or UW
    * src0) are used. The Bspec (backed by data from experimentation) state
    * that 0x3f is used for Q and UQ types, and 0x1f is used for **all** other
    * types.
    *
    * The match the behavior expected for the NIR opcodes, explicit masks for
    * 8- and 16-bit types must be added.
    */
   case nir_op_ishl:
      if (instr->def.bit_size < 32) {
         bld.AND(result, op[1], elk_imm_ud(instr->def.bit_size - 1));
         bld.SHL(result, op[0], result);
      } else {
         bld.SHL(result, op[0], op[1]);
      }

      break;
   case nir_op_ishr:
      if (instr->def.bit_size < 32) {
         bld.AND(result, op[1], elk_imm_ud(instr->def.bit_size - 1));
         bld.ASR(result, op[0], result);
      } else {
         bld.ASR(result, op[0], op[1]);
      }

      break;
   case nir_op_ushr:
      if (instr->def.bit_size < 32) {
         bld.AND(result, op[1], elk_imm_ud(instr->def.bit_size - 1));
         bld.SHR(result, op[0], result);
      } else {
         bld.SHR(result, op[0], op[1]);
      }

      break;

   case nir_op_pack_half_2x16_split:
      bld.emit(ELK_FS_OPCODE_PACK_HALF_2x16_SPLIT, result, op[0], op[1]);
      break;

   case nir_op_ffma:
      if (nir_has_any_rounding_mode_enabled(execution_mode)) {
         elk_rnd_mode rnd =
            elk_rnd_mode_from_execution_mode(execution_mode);
         bld.exec_all().emit(ELK_SHADER_OPCODE_RND_MODE, bld.null_reg_ud(),
                             elk_imm_d(rnd));
      }

      inst = bld.MAD(result, op[2], op[1], op[0]);
      break;

   case nir_op_flrp:
      if (nir_has_any_rounding_mode_enabled(execution_mode)) {
         elk_rnd_mode rnd =
            elk_rnd_mode_from_execution_mode(execution_mode);
         bld.exec_all().emit(ELK_SHADER_OPCODE_RND_MODE, bld.null_reg_ud(),
                             elk_imm_d(rnd));
      }

      inst = bld.LRP(result, op[0], op[1], op[2]);
      break;

   case nir_op_b32csel:
      if (optimize_frontfacing_ternary(ntb, instr, result))
         return;

      bld.CMP(bld.null_reg_d(), op[0], elk_imm_d(0), ELK_CONDITIONAL_NZ);
      inst = bld.SEL(result, op[1], op[2]);
      inst->predicate = ELK_PREDICATE_NORMAL;
      break;

   case nir_op_extract_u8:
   case nir_op_extract_i8: {
      unsigned byte = nir_src_as_uint(instr->src[1].src);

      /* The PRMs say:
       *
       *    BDW+
       *    There is no direct conversion from B/UB to Q/UQ or Q/UQ to B/UB.
       *    Use two instructions and a word or DWord intermediate integer type.
       */
      if (instr->def.bit_size == 64) {
         const elk_reg_type type = elk_int_type(1, instr->op == nir_op_extract_i8);

         if (instr->op == nir_op_extract_i8) {
            /* If we need to sign extend, extract to a word first */
            elk_fs_reg w_temp = bld.vgrf(ELK_REGISTER_TYPE_W);
            bld.MOV(w_temp, subscript(op[0], type, byte));
            bld.MOV(result, w_temp);
         } else if (byte & 1) {
            /* Extract the high byte from the word containing the desired byte
             * offset.
             */
            bld.SHR(result,
                    subscript(op[0], ELK_REGISTER_TYPE_UW, byte / 2),
                    elk_imm_uw(8));
         } else {
            /* Otherwise use an AND with 0xff and a word type */
            bld.AND(result,
                    subscript(op[0], ELK_REGISTER_TYPE_UW, byte / 2),
                    elk_imm_uw(0xff));
         }
      } else {
         const elk_reg_type type = elk_int_type(1, instr->op == nir_op_extract_i8);
         bld.MOV(result, subscript(op[0], type, byte));
      }
      break;
   }

   case nir_op_extract_u16:
   case nir_op_extract_i16: {
      const elk_reg_type type = elk_int_type(2, instr->op == nir_op_extract_i16);
      unsigned word = nir_src_as_uint(instr->src[1].src);
      bld.MOV(result, subscript(op[0], type, word));
      break;
   }

   default:
      unreachable("unhandled instruction");
   }

   /* If we need to do a boolean resolve, replace the result with -(x & 1)
    * to sign extend the low bit to 0/~0
    */
   if (devinfo->ver <= 5 &&
       !result.is_null() &&
       (instr->instr.pass_flags & ELK_NIR_BOOLEAN_MASK) == ELK_NIR_BOOLEAN_NEEDS_RESOLVE) {
      elk_fs_reg masked = s.vgrf(glsl_int_type());
      bld.AND(masked, result, elk_imm_d(1));
      masked.negate = true;
      bld.MOV(retype(result, ELK_REGISTER_TYPE_D), masked);
   }
}

static void
fs_nir_emit_load_const(nir_to_elk_state &ntb,
                       nir_load_const_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;

   const elk_reg_type reg_type =
      elk_reg_type_from_bit_size(instr->def.bit_size, ELK_REGISTER_TYPE_D);
   elk_fs_reg reg = bld.vgrf(reg_type, instr->def.num_components);

   switch (instr->def.bit_size) {
   case 8:
      for (unsigned i = 0; i < instr->def.num_components; i++)
         bld.MOV(offset(reg, bld, i), elk_setup_imm_b(bld, instr->value[i].i8));
      break;

   case 16:
      for (unsigned i = 0; i < instr->def.num_components; i++)
         bld.MOV(offset(reg, bld, i), elk_imm_w(instr->value[i].i16));
      break;

   case 32:
      for (unsigned i = 0; i < instr->def.num_components; i++)
         bld.MOV(offset(reg, bld, i), elk_imm_d(instr->value[i].i32));
      break;

   case 64:
      assert(devinfo->ver >= 7);
      if (!devinfo->has_64bit_int) {
         for (unsigned i = 0; i < instr->def.num_components; i++) {
            bld.MOV(retype(offset(reg, bld, i), ELK_REGISTER_TYPE_DF),
                    elk_setup_imm_df(bld, instr->value[i].f64));
         }
      } else {
         for (unsigned i = 0; i < instr->def.num_components; i++)
            bld.MOV(offset(reg, bld, i), elk_imm_q(instr->value[i].i64));
      }
      break;

   default:
      unreachable("Invalid bit size");
   }

   ntb.ssa_values[instr->def.index] = reg;
}

static bool
get_nir_src_bindless(nir_to_elk_state &ntb, const nir_src &src)
{
   return ntb.ssa_bind_infos[src.ssa->index].bindless;
}

static bool
is_resource_src(nir_src src)
{
   return src.ssa->parent_instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(src.ssa->parent_instr)->intrinsic == nir_intrinsic_resource_intel;
}

static elk_fs_reg
get_resource_nir_src(nir_to_elk_state &ntb, const nir_src &src)
{
   if (!is_resource_src(src))
      return elk_fs_reg();
   return ntb.resource_values[src.ssa->index];
}

static elk_fs_reg
get_nir_src(nir_to_elk_state &ntb, const nir_src &src)
{
   const intel_device_info *devinfo = ntb.devinfo;

   nir_intrinsic_instr *load_reg = nir_load_reg_for_def(src.ssa);

   elk_fs_reg reg;
   if (!load_reg) {
      if (nir_src_is_undef(src)) {
         const elk_reg_type reg_type =
            elk_reg_type_from_bit_size(src.ssa->bit_size,
                                       ELK_REGISTER_TYPE_D);
         reg = ntb.bld.vgrf(reg_type, src.ssa->num_components);
      } else {
         reg = ntb.ssa_values[src.ssa->index];
      }
   } else {
      nir_intrinsic_instr *decl_reg = nir_reg_get_decl(load_reg->src[0].ssa);
      /* We don't handle indirects on locals */
      assert(nir_intrinsic_base(load_reg) == 0);
      assert(load_reg->intrinsic != nir_intrinsic_load_reg_indirect);
      reg = ntb.ssa_values[decl_reg->def.index];
   }

   if (nir_src_bit_size(src) == 64 && devinfo->ver == 7) {
      /* The only 64-bit type available on gfx7 is DF, so use that. */
      reg.type = ELK_REGISTER_TYPE_DF;
   } else {
      /* To avoid floating-point denorm flushing problems, set the type by
       * default to an integer type - instructions that need floating point
       * semantics will set this to F if they need to
       */
      reg.type = elk_reg_type_from_bit_size(nir_src_bit_size(src),
                                            ELK_REGISTER_TYPE_D);
   }

   return reg;
}

/**
 * Return an IMM for constants; otherwise call get_nir_src() as normal.
 *
 * This function should not be called on any value which may be 64 bits.
 * We could theoretically support 64-bit on gfx8+ but we choose not to
 * because it wouldn't work in general (no gfx7 support) and there are
 * enough restrictions in 64-bit immediates that you can't take the return
 * value and treat it the same as the result of get_nir_src().
 */
static elk_fs_reg
get_nir_src_imm(nir_to_elk_state &ntb, const nir_src &src)
{
   assert(nir_src_bit_size(src) == 32);
   return nir_src_is_const(src) ?
          elk_fs_reg(elk_imm_d(nir_src_as_int(src))) : get_nir_src(ntb, src);
}

static elk_fs_reg
get_nir_def(nir_to_elk_state &ntb, const nir_def &def)
{
   const fs_builder &bld = ntb.bld;

   nir_intrinsic_instr *store_reg = nir_store_reg_for_def(&def);
   if (!store_reg) {
      const elk_reg_type reg_type =
         elk_reg_type_from_bit_size(def.bit_size,
                                    def.bit_size == 8 ?
                                    ELK_REGISTER_TYPE_D :
                                    ELK_REGISTER_TYPE_F);
      ntb.ssa_values[def.index] =
         bld.vgrf(reg_type, def.num_components);
      bld.UNDEF(ntb.ssa_values[def.index]);
      return ntb.ssa_values[def.index];
   } else {
      nir_intrinsic_instr *decl_reg =
         nir_reg_get_decl(store_reg->src[1].ssa);
      /* We don't handle indirects on locals */
      assert(nir_intrinsic_base(store_reg) == 0);
      assert(store_reg->intrinsic != nir_intrinsic_store_reg_indirect);
      return ntb.ssa_values[decl_reg->def.index];
   }
}

static nir_component_mask_t
get_nir_write_mask(const nir_def &def)
{
   nir_intrinsic_instr *store_reg = nir_store_reg_for_def(&def);
   if (!store_reg) {
      return nir_component_mask(def.num_components);
   } else {
      return nir_intrinsic_write_mask(store_reg);
   }
}

static elk_fs_inst *
emit_pixel_interpolater_send(const fs_builder &bld,
                             enum elk_opcode opcode,
                             const elk_fs_reg &dst,
                             const elk_fs_reg &src,
                             const elk_fs_reg &desc,
                             const elk_fs_reg &flag_reg,
                             glsl_interp_mode interpolation)
{
   struct elk_wm_prog_data *wm_prog_data =
      elk_wm_prog_data(bld.shader->stage_prog_data);

   elk_fs_reg srcs[INTERP_NUM_SRCS];
   srcs[INTERP_SRC_OFFSET]       = src;
   srcs[INTERP_SRC_MSG_DESC]     = desc;
   srcs[INTERP_SRC_DYNAMIC_MODE] = flag_reg;

   elk_fs_inst *inst = bld.emit(opcode, dst, srcs, INTERP_NUM_SRCS);
   /* 2 floats per slot returned */
   inst->size_written = 2 * dst.component_size(inst->exec_size);
   if (interpolation == INTERP_MODE_NOPERSPECTIVE) {
      inst->pi_noperspective = true;
      /* TGL BSpec says:
       *     This field cannot be set to "Linear Interpolation"
       *     unless Non-Perspective Barycentric Enable in 3DSTATE_CLIP is enabled"
       */
      wm_prog_data->uses_nonperspective_interp_modes = true;
   }

   wm_prog_data->pulls_bary = true;

   return inst;
}

/**
 * Computes 1 << x, given a D/UD register containing some value x.
 */
static elk_fs_reg
intexp2(const fs_builder &bld, const elk_fs_reg &x)
{
   assert(x.type == ELK_REGISTER_TYPE_UD || x.type == ELK_REGISTER_TYPE_D);

   elk_fs_reg result = bld.vgrf(x.type, 1);
   elk_fs_reg one = bld.vgrf(x.type, 1);

   bld.MOV(one, retype(elk_imm_d(1), one.type));
   bld.SHL(result, one, x);
   return result;
}

static void
emit_gs_end_primitive(nir_to_elk_state &ntb, const nir_src &vertex_count_nir_src)
{
   elk_fs_visitor &s = ntb.s;
   assert(s.stage == MESA_SHADER_GEOMETRY);

   struct elk_gs_prog_data *gs_prog_data = elk_gs_prog_data(s.prog_data);

   if (s.gs_compile->control_data_header_size_bits == 0)
      return;

   /* We can only do EndPrimitive() functionality when the control data
    * consists of cut bits.  Fortunately, the only time it isn't is when the
    * output type is points, in which case EndPrimitive() is a no-op.
    */
   if (gs_prog_data->control_data_format !=
       GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_CUT) {
      return;
   }

   /* Cut bits use one bit per vertex. */
   assert(s.gs_compile->control_data_bits_per_vertex == 1);

   elk_fs_reg vertex_count = get_nir_src(ntb, vertex_count_nir_src);
   vertex_count.type = ELK_REGISTER_TYPE_UD;

   /* Cut bit n should be set to 1 if EndPrimitive() was called after emitting
    * vertex n, 0 otherwise.  So all we need to do here is mark bit
    * (vertex_count - 1) % 32 in the cut_bits register to indicate that
    * EndPrimitive() was called after emitting vertex (vertex_count - 1);
    * vec4_gs_visitor::emit_control_data_bits() will take care of the rest.
    *
    * Note that if EndPrimitive() is called before emitting any vertices, this
    * will cause us to set bit 31 of the control_data_bits register to 1.
    * That's fine because:
    *
    * - If max_vertices < 32, then vertex number 31 (zero-based) will never be
    *   output, so the hardware will ignore cut bit 31.
    *
    * - If max_vertices == 32, then vertex number 31 is guaranteed to be the
    *   last vertex, so setting cut bit 31 has no effect (since the primitive
    *   is automatically ended when the GS terminates).
    *
    * - If max_vertices > 32, then the ir_emit_vertex visitor will reset the
    *   control_data_bits register to 0 when the first vertex is emitted.
    */

   const fs_builder abld = ntb.bld.annotate("end primitive");

   /* control_data_bits |= 1 << ((vertex_count - 1) % 32) */
   elk_fs_reg prev_count = ntb.bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   abld.ADD(prev_count, vertex_count, elk_imm_ud(0xffffffffu));
   elk_fs_reg mask = intexp2(abld, prev_count);
   /* Note: we're relying on the fact that the GEN SHL instruction only pays
    * attention to the lower 5 bits of its second source argument, so on this
    * architecture, 1 << (vertex_count - 1) is equivalent to 1 <<
    * ((vertex_count - 1) % 32).
    */
   abld.OR(s.control_data_bits, s.control_data_bits, mask);
}

void
elk_fs_visitor::emit_gs_control_data_bits(const elk_fs_reg &vertex_count)
{
   assert(stage == MESA_SHADER_GEOMETRY);
   assert(gs_compile->control_data_bits_per_vertex != 0);

   struct elk_gs_prog_data *gs_prog_data = elk_gs_prog_data(prog_data);

   const fs_builder bld = fs_builder(this).at_end();
   const fs_builder abld = bld.annotate("emit control data bits");
   const fs_builder fwa_bld = bld.exec_all();

   /* We use a single UD register to accumulate control data bits (32 bits
    * for each of the SIMD8 channels).  So we need to write a DWord (32 bits)
    * at a time.
    *
    * Unfortunately, the URB_WRITE_SIMD8 message uses 128-bit (OWord) offsets.
    * We have select a 128-bit group via the Global and Per-Slot Offsets, then
    * use the Channel Mask phase to enable/disable which DWord within that
    * group to write.  (Remember, different SIMD8 channels may have emitted
    * different numbers of vertices, so we may need per-slot offsets.)
    *
    * Channel masking presents an annoying problem: we may have to replicate
    * the data up to 4 times:
    *
    * Msg = Handles, Per-Slot Offsets, Channel Masks, Data, Data, Data, Data.
    *
    * To avoid penalizing shaders that emit a small number of vertices, we
    * can avoid these sometimes: if the size of the control data header is
    * <= 128 bits, then there is only 1 OWord.  All SIMD8 channels will land
    * land in the same 128-bit group, so we can skip per-slot offsets.
    *
    * Similarly, if the control data header is <= 32 bits, there is only one
    * DWord, so we can skip channel masks.
    */
   elk_fs_reg channel_mask, per_slot_offset;

   if (gs_compile->control_data_header_size_bits > 32)
      channel_mask = vgrf(glsl_uint_type());

   if (gs_compile->control_data_header_size_bits > 128)
      per_slot_offset = vgrf(glsl_uint_type());

   /* Figure out which DWord we're trying to write to using the formula:
    *
    *    dword_index = (vertex_count - 1) * bits_per_vertex / 32
    *
    * Since bits_per_vertex is a power of two, and is known at compile
    * time, this can be optimized to:
    *
    *    dword_index = (vertex_count - 1) >> (6 - log2(bits_per_vertex))
    */
   if (channel_mask.file != BAD_FILE || per_slot_offset.file != BAD_FILE) {
      elk_fs_reg dword_index = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
      elk_fs_reg prev_count = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
      abld.ADD(prev_count, vertex_count, elk_imm_ud(0xffffffffu));
      unsigned log2_bits_per_vertex =
         util_last_bit(gs_compile->control_data_bits_per_vertex);
      abld.SHR(dword_index, prev_count, elk_imm_ud(6u - log2_bits_per_vertex));

      if (per_slot_offset.file != BAD_FILE) {
         /* Set the per-slot offset to dword_index / 4, so that we'll write to
          * the appropriate OWord within the control data header.
          */
         abld.SHR(per_slot_offset, dword_index, elk_imm_ud(2u));
      }

      /* Set the channel masks to 1 << (dword_index % 4), so that we'll
       * write to the appropriate DWORD within the OWORD.
       */
      elk_fs_reg channel = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
      fwa_bld.AND(channel, dword_index, elk_imm_ud(3u));
      channel_mask = intexp2(fwa_bld, channel);
      /* Then the channel masks need to be in bits 23:16. */
      fwa_bld.SHL(channel_mask, channel_mask, elk_imm_ud(16u));
   }

   /* If there are channel masks, add 3 extra copies of the data. */
   const unsigned length = 1 + 3 * unsigned(channel_mask.file != BAD_FILE);
   elk_fs_reg sources[4];

   for (unsigned i = 0; i < ARRAY_SIZE(sources); i++)
      sources[i] = this->control_data_bits;

   elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
   srcs[URB_LOGICAL_SRC_HANDLE] = gs_payload().urb_handles;
   srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = per_slot_offset;
   srcs[URB_LOGICAL_SRC_CHANNEL_MASK] = channel_mask;
   srcs[URB_LOGICAL_SRC_DATA] = bld.vgrf(ELK_REGISTER_TYPE_F, length);
   srcs[URB_LOGICAL_SRC_COMPONENTS] = elk_imm_ud(length);
   abld.LOAD_PAYLOAD(srcs[URB_LOGICAL_SRC_DATA], sources, length, 0);

   elk_fs_inst *inst = abld.emit(ELK_SHADER_OPCODE_URB_WRITE_LOGICAL, reg_undef,
                             srcs, ARRAY_SIZE(srcs));

   /* We need to increment Global Offset by 256-bits to make room for
    * Broadwell's extra "Vertex Count" payload at the beginning of the
    * URB entry.  Since this is an OWord message, Global Offset is counted
    * in 128-bit units, so we must set it to 2.
    */
   if (gs_prog_data->static_vertex_count == -1)
      inst->offset = 2;
}

static void
set_gs_stream_control_data_bits(nir_to_elk_state &ntb, const elk_fs_reg &vertex_count,
                                unsigned stream_id)
{
   elk_fs_visitor &s = ntb.s;

   /* control_data_bits |= stream_id << ((2 * (vertex_count - 1)) % 32) */

   /* Note: we are calling this *before* increasing vertex_count, so
    * this->vertex_count == vertex_count - 1 in the formula above.
    */

   /* Stream mode uses 2 bits per vertex */
   assert(s.gs_compile->control_data_bits_per_vertex == 2);

   /* Must be a valid stream */
   assert(stream_id < 4); /* MAX_VERTEX_STREAMS */

   /* Control data bits are initialized to 0 so we don't have to set any
    * bits when sending vertices to stream 0.
    */
   if (stream_id == 0)
      return;

   const fs_builder abld = ntb.bld.annotate("set stream control data bits", NULL);

   /* reg::sid = stream_id */
   elk_fs_reg sid = ntb.bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   abld.MOV(sid, elk_imm_ud(stream_id));

   /* reg:shift_count = 2 * (vertex_count - 1) */
   elk_fs_reg shift_count = ntb.bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   abld.SHL(shift_count, vertex_count, elk_imm_ud(1u));

   /* Note: we're relying on the fact that the GEN SHL instruction only pays
    * attention to the lower 5 bits of its second source argument, so on this
    * architecture, stream_id << 2 * (vertex_count - 1) is equivalent to
    * stream_id << ((2 * (vertex_count - 1)) % 32).
    */
   elk_fs_reg mask = ntb.bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   abld.SHL(mask, sid, shift_count);
   abld.OR(s.control_data_bits, s.control_data_bits, mask);
}

static void
emit_gs_vertex(nir_to_elk_state &ntb, const nir_src &vertex_count_nir_src,
               unsigned stream_id)
{
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_GEOMETRY);

   struct elk_gs_prog_data *gs_prog_data = elk_gs_prog_data(s.prog_data);

   elk_fs_reg vertex_count = get_nir_src(ntb, vertex_count_nir_src);
   vertex_count.type = ELK_REGISTER_TYPE_UD;

   /* Haswell and later hardware ignores the "Render Stream Select" bits
    * from the 3DSTATE_STREAMOUT packet when the SOL stage is disabled,
    * and instead sends all primitives down the pipeline for rasterization.
    * If the SOL stage is enabled, "Render Stream Select" is honored and
    * primitives bound to non-zero streams are discarded after stream output.
    *
    * Since the only purpose of primives sent to non-zero streams is to
    * be recorded by transform feedback, we can simply discard all geometry
    * bound to these streams when transform feedback is disabled.
    */
   if (stream_id > 0 && !s.nir->info.has_transform_feedback_varyings)
      return;

   /* If we're outputting 32 control data bits or less, then we can wait
    * until the shader is over to output them all.  Otherwise we need to
    * output them as we go.  Now is the time to do it, since we're about to
    * output the vertex_count'th vertex, so it's guaranteed that the
    * control data bits associated with the (vertex_count - 1)th vertex are
    * correct.
    */
   if (s.gs_compile->control_data_header_size_bits > 32) {
      const fs_builder abld =
         ntb.bld.annotate("emit vertex: emit control data bits");

      /* Only emit control data bits if we've finished accumulating a batch
       * of 32 bits.  This is the case when:
       *
       *     (vertex_count * bits_per_vertex) % 32 == 0
       *
       * (in other words, when the last 5 bits of vertex_count *
       * bits_per_vertex are 0).  Assuming bits_per_vertex == 2^n for some
       * integer n (which is always the case, since bits_per_vertex is
       * always 1 or 2), this is equivalent to requiring that the last 5-n
       * bits of vertex_count are 0:
       *
       *     vertex_count & (2^(5-n) - 1) == 0
       *
       * 2^(5-n) == 2^5 / 2^n == 32 / bits_per_vertex, so this is
       * equivalent to:
       *
       *     vertex_count & (32 / bits_per_vertex - 1) == 0
       *
       * TODO: If vertex_count is an immediate, we could do some of this math
       *       at compile time...
       */
      elk_fs_inst *inst =
         abld.AND(ntb.bld.null_reg_d(), vertex_count,
                  elk_imm_ud(32u / s.gs_compile->control_data_bits_per_vertex - 1u));
      inst->conditional_mod = ELK_CONDITIONAL_Z;

      abld.IF(ELK_PREDICATE_NORMAL);
      /* If vertex_count is 0, then no control data bits have been
       * accumulated yet, so we can skip emitting them.
       */
      abld.CMP(ntb.bld.null_reg_d(), vertex_count, elk_imm_ud(0u),
               ELK_CONDITIONAL_NEQ);
      abld.IF(ELK_PREDICATE_NORMAL);
      s.emit_gs_control_data_bits(vertex_count);
      abld.emit(ELK_OPCODE_ENDIF);

      /* Reset control_data_bits to 0 so we can start accumulating a new
       * batch.
       *
       * Note: in the case where vertex_count == 0, this neutralizes the
       * effect of any call to EndPrimitive() that the shader may have
       * made before outputting its first vertex.
       */
      inst = abld.MOV(s.control_data_bits, elk_imm_ud(0u));
      inst->force_writemask_all = true;
      abld.emit(ELK_OPCODE_ENDIF);
   }

   s.emit_urb_writes(vertex_count);

   /* In stream mode we have to set control data bits for all vertices
    * unless we have disabled control data bits completely (which we do
    * do for MESA_PRIM_POINTS outputs that don't use streams).
    */
   if (s.gs_compile->control_data_header_size_bits > 0 &&
       gs_prog_data->control_data_format ==
          GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_SID) {
      set_gs_stream_control_data_bits(ntb, vertex_count, stream_id);
   }
}

static void
emit_gs_input_load(nir_to_elk_state &ntb, const elk_fs_reg &dst,
                   const nir_src &vertex_src,
                   unsigned base_offset,
                   const nir_src &offset_src,
                   unsigned num_components,
                   unsigned first_component)
{
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(type_sz(dst.type) == 4);
   struct elk_gs_prog_data *gs_prog_data = elk_gs_prog_data(s.prog_data);
   const unsigned push_reg_count = gs_prog_data->base.urb_read_length * 8;

   /* TODO: figure out push input layout for invocations == 1 */
   if (gs_prog_data->invocations == 1 &&
       nir_src_is_const(offset_src) && nir_src_is_const(vertex_src) &&
       4 * (base_offset + nir_src_as_uint(offset_src)) < push_reg_count) {
      int imm_offset = (base_offset + nir_src_as_uint(offset_src)) * 4 +
                       nir_src_as_uint(vertex_src) * push_reg_count;
      const elk_fs_reg attr = elk_fs_reg(ATTR, 0, dst.type);
      for (unsigned i = 0; i < num_components; i++) {
         ntb.bld.MOV(offset(dst, bld, i),
                     offset(attr, bld, imm_offset + i + first_component));
      }
      return;
   }

   /* Resort to the pull model.  Ensure the VUE handles are provided. */
   assert(gs_prog_data->base.include_vue_handles);

   elk_fs_reg start = s.gs_payload().icp_handle_start;
   elk_fs_reg icp_handle = ntb.bld.vgrf(ELK_REGISTER_TYPE_UD, 1);

   if (gs_prog_data->invocations == 1) {
      if (nir_src_is_const(vertex_src)) {
         /* The vertex index is constant; just select the proper URB handle. */
         icp_handle = offset(start, ntb.bld, nir_src_as_uint(vertex_src));
      } else {
         /* The vertex index is non-constant.  We need to use indirect
          * addressing to fetch the proper URB handle.
          *
          * First, we start with the sequence <7, 6, 5, 4, 3, 2, 1, 0>
          * indicating that channel <n> should read the handle from
          * DWord <n>.  We convert that to bytes by multiplying by 4.
          *
          * Next, we convert the vertex index to bytes by multiplying
          * by 32 (shifting by 5), and add the two together.  This is
          * the final indirect byte offset.
          */
         elk_fs_reg sequence =
            ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION];
         elk_fs_reg channel_offsets = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
         elk_fs_reg vertex_offset_bytes = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
         elk_fs_reg icp_offset_bytes = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);

         /* channel_offsets = 4 * sequence = <28, 24, 20, 16, 12, 8, 4, 0> */
         bld.SHL(channel_offsets, sequence, elk_imm_ud(2u));
         /* Convert vertex_index to bytes (multiply by 32) */
         bld.SHL(vertex_offset_bytes,
                 retype(get_nir_src(ntb, vertex_src), ELK_REGISTER_TYPE_UD),
                 elk_imm_ud(5u));
         bld.ADD(icp_offset_bytes, vertex_offset_bytes, channel_offsets);

         /* Use first_icp_handle as the base offset.  There is one register
          * of URB handles per vertex, so inform the register allocator that
          * we might read up to nir->info.gs.vertices_in registers.
          */
         bld.emit(ELK_SHADER_OPCODE_MOV_INDIRECT, icp_handle, start,
                  elk_fs_reg(icp_offset_bytes),
                  elk_imm_ud(s.nir->info.gs.vertices_in * REG_SIZE));
      }
   } else {
      assert(gs_prog_data->invocations > 1);

      if (nir_src_is_const(vertex_src)) {
         unsigned vertex = nir_src_as_uint(vertex_src);
         assert(vertex <= 5);
         bld.MOV(icp_handle, component(start, vertex));
      } else {
         /* The vertex index is non-constant.  We need to use indirect
          * addressing to fetch the proper URB handle.
          *
          */
         elk_fs_reg icp_offset_bytes = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);

         /* Convert vertex_index to bytes (multiply by 4) */
         bld.SHL(icp_offset_bytes,
                 retype(get_nir_src(ntb, vertex_src), ELK_REGISTER_TYPE_UD),
                 elk_imm_ud(2u));

         /* Use first_icp_handle as the base offset.  There is one DWord
          * of URB handles per vertex, so inform the register allocator that
          * we might read up to ceil(nir->info.gs.vertices_in / 8) registers.
          */
         bld.emit(ELK_SHADER_OPCODE_MOV_INDIRECT, icp_handle, start,
                  elk_fs_reg(icp_offset_bytes),
                  elk_imm_ud(DIV_ROUND_UP(s.nir->info.gs.vertices_in, 8) *
                             REG_SIZE));
      }
   }

   elk_fs_inst *inst;
   elk_fs_reg indirect_offset = get_nir_src(ntb, offset_src);

   if (nir_src_is_const(offset_src)) {
      elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
      srcs[URB_LOGICAL_SRC_HANDLE] = icp_handle;

      /* Constant indexing - use global offset. */
      if (first_component != 0) {
         unsigned read_components = num_components + first_component;
         elk_fs_reg tmp = bld.vgrf(dst.type, read_components);
         inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp, srcs,
                         ARRAY_SIZE(srcs));
         inst->size_written = read_components *
                              tmp.component_size(inst->exec_size);
         for (unsigned i = 0; i < num_components; i++) {
            bld.MOV(offset(dst, bld, i),
                    offset(tmp, bld, i + first_component));
         }
      } else {
         inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dst, srcs,
                         ARRAY_SIZE(srcs));
         inst->size_written = num_components *
                              dst.component_size(inst->exec_size);
      }
      inst->offset = base_offset + nir_src_as_uint(offset_src);
   } else {
      /* Indirect indexing - use per-slot offsets as well. */
      unsigned read_components = num_components + first_component;
      elk_fs_reg tmp = bld.vgrf(dst.type, read_components);

      elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
      srcs[URB_LOGICAL_SRC_HANDLE] = icp_handle;
      srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = indirect_offset;

      if (first_component != 0) {
         inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp,
                         srcs, ARRAY_SIZE(srcs));
         inst->size_written = read_components *
                              tmp.component_size(inst->exec_size);
         for (unsigned i = 0; i < num_components; i++) {
            bld.MOV(offset(dst, bld, i),
                    offset(tmp, bld, i + first_component));
         }
      } else {
         inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dst,
                         srcs, ARRAY_SIZE(srcs));
         inst->size_written = num_components *
                              dst.component_size(inst->exec_size);
      }
      inst->offset = base_offset;
   }
}

static elk_fs_reg
get_indirect_offset(nir_to_elk_state &ntb, nir_intrinsic_instr *instr)
{
   nir_src *offset_src = nir_get_io_offset_src(instr);

   if (nir_src_is_const(*offset_src)) {
      /* The only constant offset we should find is 0.  elk_nir.c's
       * add_const_offset_to_base() will fold other constant offsets
       * into the "base" index.
       */
      assert(nir_src_as_uint(*offset_src) == 0);
      return elk_fs_reg();
   }

   return get_nir_src(ntb, *offset_src);
}

static void
fs_nir_emit_vs_intrinsic(nir_to_elk_state &ntb,
                         nir_intrinsic_instr *instr)
{
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;
   assert(s.stage == MESA_SHADER_VERTEX);

   elk_fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_load_vertex_id:
   case nir_intrinsic_load_base_vertex:
      unreachable("should be lowered by nir_lower_system_values()");

   case nir_intrinsic_load_input: {
      assert(instr->def.bit_size == 32);
      const elk_fs_reg src = offset(elk_fs_reg(ATTR, 0, dest.type), bld,
                                nir_intrinsic_base(instr) * 4 +
                                nir_intrinsic_component(instr) +
                                nir_src_as_uint(instr->src[0]));

      for (unsigned i = 0; i < instr->num_components; i++)
         bld.MOV(offset(dest, bld, i), offset(src, bld, i));
      break;
   }

   case nir_intrinsic_load_vertex_id_zero_base:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_base_instance:
   case nir_intrinsic_load_draw_id:
   case nir_intrinsic_load_first_vertex:
   case nir_intrinsic_load_is_indexed_draw:
      unreachable("lowered by elk_nir_lower_vs_inputs");

   default:
      fs_nir_emit_intrinsic(ntb, bld, instr);
      break;
   }
}

static elk_fs_reg
get_tcs_single_patch_icp_handle(nir_to_elk_state &ntb, const fs_builder &bld,
                                nir_intrinsic_instr *instr)
{
   elk_fs_visitor &s = ntb.s;

   struct elk_tcs_prog_data *tcs_prog_data = elk_tcs_prog_data(s.prog_data);
   const nir_src &vertex_src = instr->src[0];
   nir_intrinsic_instr *vertex_intrin = nir_src_as_intrinsic(vertex_src);

   const elk_fs_reg start = s.tcs_payload().icp_handle_start;

   elk_fs_reg icp_handle;

   if (nir_src_is_const(vertex_src)) {
      /* Emit a MOV to resolve <0,1,0> regioning. */
      icp_handle = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
      unsigned vertex = nir_src_as_uint(vertex_src);
      bld.MOV(icp_handle, component(start, vertex));
   } else if (tcs_prog_data->instances == 1 && vertex_intrin &&
              vertex_intrin->intrinsic == nir_intrinsic_load_invocation_id) {
      /* For the common case of only 1 instance, an array index of
       * gl_InvocationID means reading the handles from the start.  Skip all
       * the indirect work.
       */
      icp_handle = start;
   } else {
      /* The vertex index is non-constant.  We need to use indirect
       * addressing to fetch the proper URB handle.
       */
      icp_handle = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);

      /* Each ICP handle is a single DWord (4 bytes) */
      elk_fs_reg vertex_offset_bytes = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
      bld.SHL(vertex_offset_bytes,
              retype(get_nir_src(ntb, vertex_src), ELK_REGISTER_TYPE_UD),
              elk_imm_ud(2u));

      /* We might read up to 4 registers. */
      bld.emit(ELK_SHADER_OPCODE_MOV_INDIRECT, icp_handle,
               start, vertex_offset_bytes,
               elk_imm_ud(4 * REG_SIZE));
   }

   return icp_handle;
}

static elk_fs_reg
get_tcs_multi_patch_icp_handle(nir_to_elk_state &ntb, const fs_builder &bld,
                               nir_intrinsic_instr *instr)
{
   elk_fs_visitor &s = ntb.s;
   const intel_device_info *devinfo = s.devinfo;

   struct elk_tcs_prog_key *tcs_key = (struct elk_tcs_prog_key *) s.key;
   const nir_src &vertex_src = instr->src[0];
   const unsigned grf_size_bytes = REG_SIZE * reg_unit(devinfo);

   const elk_fs_reg start = s.tcs_payload().icp_handle_start;

   if (nir_src_is_const(vertex_src))
      return byte_offset(start, nir_src_as_uint(vertex_src) * grf_size_bytes);

   /* The vertex index is non-constant.  We need to use indirect
    * addressing to fetch the proper URB handle.
    *
    * First, we start with the sequence indicating that channel <n>
    * should read the handle from DWord <n>.  We convert that to bytes
    * by multiplying by 4.
    *
    * Next, we convert the vertex index to bytes by multiplying
    * by the GRF size (by shifting), and add the two together.  This is
    * the final indirect byte offset.
    */
   elk_fs_reg icp_handle = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   elk_fs_reg sequence = ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION];
   elk_fs_reg channel_offsets = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   elk_fs_reg vertex_offset_bytes = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   elk_fs_reg icp_offset_bytes = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);

   /* Offsets will be 0, 4, 8, ... */
   bld.SHL(channel_offsets, sequence, elk_imm_ud(2u));
   /* Convert vertex_index to bytes (multiply by 32) */
   assert(util_is_power_of_two_nonzero(grf_size_bytes)); /* for ffs() */
   bld.SHL(vertex_offset_bytes,
           retype(get_nir_src(ntb, vertex_src), ELK_REGISTER_TYPE_UD),
           elk_imm_ud(ffs(grf_size_bytes) - 1));
   bld.ADD(icp_offset_bytes, vertex_offset_bytes, channel_offsets);

   /* Use start of ICP handles as the base offset.  There is one register
    * of URB handles per vertex, so inform the register allocator that
    * we might read up to nir->info.gs.vertices_in registers.
    */
   bld.emit(ELK_SHADER_OPCODE_MOV_INDIRECT, icp_handle, start,
            icp_offset_bytes,
            elk_imm_ud(elk_tcs_prog_key_input_vertices(tcs_key) *
                       grf_size_bytes));

   return icp_handle;
}

static void
emit_barrier(nir_to_elk_state &ntb)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   /* We are getting the barrier ID from the compute shader header */
   assert(gl_shader_stage_uses_workgroup(s.stage));

   elk_fs_reg payload = elk_fs_reg(VGRF, s.alloc.allocate(1), ELK_REGISTER_TYPE_UD);

   /* Clear the message payload */
   bld.exec_all().group(8, 0).MOV(payload, elk_imm_ud(0u));

   assert(gl_shader_stage_is_compute(s.stage));

   uint32_t barrier_id_mask;
   switch (devinfo->ver) {
   case 7:
   case 8:
      barrier_id_mask = 0x0f000000u; break;
   default:
      unreachable("barrier is only available on gen >= 7");
   }

   /* Copy the barrier id from r0.2 to the message payload reg.2 */
   elk_fs_reg r0_2 = elk_fs_reg(retype(elk_vec1_grf(0, 2), ELK_REGISTER_TYPE_UD));
   bld.exec_all().group(1, 0).AND(component(payload, 2), r0_2,
                                  elk_imm_ud(barrier_id_mask));

   /* Emit a gateway "barrier" message using the payload we set up, followed
    * by a wait instruction.
    */
   bld.exec_all().emit(ELK_SHADER_OPCODE_BARRIER, reg_undef, payload);
}

static void
emit_tcs_barrier(nir_to_elk_state &ntb)
{
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_TESS_CTRL);
   struct elk_tcs_prog_data *tcs_prog_data = elk_tcs_prog_data(s.prog_data);

   elk_fs_reg m0 = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
   elk_fs_reg m0_2 = component(m0, 2);

   const fs_builder chanbld = bld.exec_all().group(1, 0);

   /* Zero the message header */
   bld.exec_all().MOV(m0, elk_imm_ud(0u));

   /* Copy "Barrier ID" from r0.2, bits 16:13 */
   chanbld.AND(m0_2, retype(elk_vec1_grf(0, 2), ELK_REGISTER_TYPE_UD),
               elk_imm_ud(INTEL_MASK(16, 13)));

   /* Shift it up to bits 27:24. */
   chanbld.SHL(m0_2, m0_2, elk_imm_ud(11));

   /* Set the Barrier Count and the enable bit */
   chanbld.OR(m0_2, m0_2,
              elk_imm_ud(tcs_prog_data->instances << 9 | (1 << 15)));

   bld.emit(ELK_SHADER_OPCODE_BARRIER, bld.null_reg_ud(), m0);
}

static void
fs_nir_emit_tcs_intrinsic(nir_to_elk_state &ntb,
                          nir_intrinsic_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_TESS_CTRL);
   struct elk_tcs_prog_data *tcs_prog_data = elk_tcs_prog_data(s.prog_data);
   struct elk_vue_prog_data *vue_prog_data = &tcs_prog_data->base;

   elk_fs_reg dst;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dst = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_load_primitive_id:
      bld.MOV(dst, s.tcs_payload().primitive_id);
      break;
   case nir_intrinsic_load_invocation_id:
      bld.MOV(retype(dst, s.invocation_id.type), s.invocation_id);
      break;

   case nir_intrinsic_barrier:
      if (nir_intrinsic_memory_scope(instr) != SCOPE_NONE)
         fs_nir_emit_intrinsic(ntb, bld, instr);
      if (nir_intrinsic_execution_scope(instr) == SCOPE_WORKGROUP) {
         if (tcs_prog_data->instances != 1)
            emit_tcs_barrier(ntb);
      }
      break;

   case nir_intrinsic_load_input:
      unreachable("nir_lower_io should never give us these.");
      break;

   case nir_intrinsic_load_per_vertex_input: {
      assert(instr->def.bit_size == 32);
      elk_fs_reg indirect_offset = get_indirect_offset(ntb, instr);
      unsigned imm_offset = nir_intrinsic_base(instr);
      elk_fs_inst *inst;

      const bool multi_patch =
         vue_prog_data->dispatch_mode == INTEL_DISPATCH_MODE_TCS_MULTI_PATCH;

      elk_fs_reg icp_handle = multi_patch ?
         get_tcs_multi_patch_icp_handle(ntb, bld, instr) :
         get_tcs_single_patch_icp_handle(ntb, bld, instr);

      /* We can only read two double components with each URB read, so
       * we send two read messages in that case, each one loading up to
       * two double components.
       */
      unsigned num_components = instr->num_components;
      unsigned first_component = nir_intrinsic_component(instr);

      elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
      srcs[URB_LOGICAL_SRC_HANDLE] = icp_handle;

      if (indirect_offset.file == BAD_FILE) {
         /* Constant indexing - use global offset. */
         if (first_component != 0) {
            unsigned read_components = num_components + first_component;
            elk_fs_reg tmp = bld.vgrf(dst.type, read_components);
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp, srcs,
                            ARRAY_SIZE(srcs));
            for (unsigned i = 0; i < num_components; i++) {
               bld.MOV(offset(dst, bld, i),
                       offset(tmp, bld, i + first_component));
            }
         } else {
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dst, srcs,
                            ARRAY_SIZE(srcs));
         }
         inst->offset = imm_offset;
      } else {
         /* Indirect indexing - use per-slot offsets as well. */
         srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = indirect_offset;

         if (first_component != 0) {
            unsigned read_components = num_components + first_component;
            elk_fs_reg tmp = bld.vgrf(dst.type, read_components);
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp,
                            srcs, ARRAY_SIZE(srcs));
            for (unsigned i = 0; i < num_components; i++) {
               bld.MOV(offset(dst, bld, i),
                       offset(tmp, bld, i + first_component));
            }
         } else {
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dst,
                            srcs, ARRAY_SIZE(srcs));
         }
         inst->offset = imm_offset;
      }
      inst->size_written = (num_components + first_component) *
                           inst->dst.component_size(inst->exec_size);

      /* Copy the temporary to the destination to deal with writemasking.
       *
       * Also attempt to deal with gl_PointSize being in the .w component.
       */
      if (inst->offset == 0 && indirect_offset.file == BAD_FILE) {
         assert(type_sz(dst.type) == 4);
         inst->dst = bld.vgrf(dst.type, 4);
         inst->size_written = 4 * REG_SIZE * reg_unit(devinfo);
         bld.MOV(dst, offset(inst->dst, bld, 3));
      }
      break;
   }

   case nir_intrinsic_load_output:
   case nir_intrinsic_load_per_vertex_output: {
      assert(instr->def.bit_size == 32);
      elk_fs_reg indirect_offset = get_indirect_offset(ntb, instr);
      unsigned imm_offset = nir_intrinsic_base(instr);
      unsigned first_component = nir_intrinsic_component(instr);

      elk_fs_inst *inst;
      if (indirect_offset.file == BAD_FILE) {
         /* This MOV replicates the output handle to all enabled channels
          * is SINGLE_PATCH mode.
          */
         elk_fs_reg patch_handle = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
         bld.MOV(patch_handle, s.tcs_payload().patch_urb_output);

         {
            elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
            srcs[URB_LOGICAL_SRC_HANDLE] = patch_handle;

            if (first_component != 0) {
               unsigned read_components =
                  instr->num_components + first_component;
               elk_fs_reg tmp = bld.vgrf(dst.type, read_components);
               inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp,
                               srcs, ARRAY_SIZE(srcs));
               inst->size_written = read_components * REG_SIZE * reg_unit(devinfo);
               for (unsigned i = 0; i < instr->num_components; i++) {
                  bld.MOV(offset(dst, bld, i),
                          offset(tmp, bld, i + first_component));
               }
            } else {
               inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dst,
                               srcs, ARRAY_SIZE(srcs));
               inst->size_written = instr->num_components * REG_SIZE * reg_unit(devinfo);
            }
            inst->offset = imm_offset;
         }
      } else {
         /* Indirect indexing - use per-slot offsets as well. */
         elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
         srcs[URB_LOGICAL_SRC_HANDLE] = s.tcs_payload().patch_urb_output;
         srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = indirect_offset;

         if (first_component != 0) {
            unsigned read_components =
               instr->num_components + first_component;
            elk_fs_reg tmp = bld.vgrf(dst.type, read_components);
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp,
                            srcs, ARRAY_SIZE(srcs));
            inst->size_written = read_components * REG_SIZE * reg_unit(devinfo);
            for (unsigned i = 0; i < instr->num_components; i++) {
               bld.MOV(offset(dst, bld, i),
                       offset(tmp, bld, i + first_component));
            }
         } else {
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dst,
                            srcs, ARRAY_SIZE(srcs));
            inst->size_written = instr->num_components * REG_SIZE * reg_unit(devinfo);
         }
         inst->offset = imm_offset;
      }
      break;
   }

   case nir_intrinsic_store_output:
   case nir_intrinsic_store_per_vertex_output: {
      assert(nir_src_bit_size(instr->src[0]) == 32);
      elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      elk_fs_reg indirect_offset = get_indirect_offset(ntb, instr);
      unsigned imm_offset = nir_intrinsic_base(instr);
      unsigned mask = nir_intrinsic_write_mask(instr);

      if (mask == 0)
         break;

      unsigned num_components = util_last_bit(mask);
      unsigned first_component = nir_intrinsic_component(instr);
      assert((first_component + num_components) <= 4);

      mask = mask << first_component;

      elk_fs_reg mask_reg;
      if (mask != WRITEMASK_XYZW)
         mask_reg = elk_imm_ud(mask << 16);

      elk_fs_reg sources[4];

      unsigned m = first_component;
      for (unsigned i = 0; i < num_components; i++) {
         int c = i + first_component;
         if (mask & (1 << c)) {
            sources[m++] = offset(value, bld, i);
         } else {
            m++;
         }
      }

      assert(m == (first_component + num_components));

      elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
      srcs[URB_LOGICAL_SRC_HANDLE] = s.tcs_payload().patch_urb_output;
      srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = indirect_offset;
      srcs[URB_LOGICAL_SRC_CHANNEL_MASK] = mask_reg;
      srcs[URB_LOGICAL_SRC_DATA] = bld.vgrf(ELK_REGISTER_TYPE_F, m);
      srcs[URB_LOGICAL_SRC_COMPONENTS] = elk_imm_ud(m);
      bld.LOAD_PAYLOAD(srcs[URB_LOGICAL_SRC_DATA], sources, m, 0);

      elk_fs_inst *inst = bld.emit(ELK_SHADER_OPCODE_URB_WRITE_LOGICAL, reg_undef,
                               srcs, ARRAY_SIZE(srcs));
      inst->offset = imm_offset;
      break;
   }

   default:
      fs_nir_emit_intrinsic(ntb, bld, instr);
      break;
   }
}

static void
fs_nir_emit_tes_intrinsic(nir_to_elk_state &ntb,
                          nir_intrinsic_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_TESS_EVAL);
   struct elk_tes_prog_data *tes_prog_data = elk_tes_prog_data(s.prog_data);

   elk_fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_load_primitive_id:
      bld.MOV(dest, s.tes_payload().primitive_id);
      break;

   case nir_intrinsic_load_tess_coord:
      for (unsigned i = 0; i < 3; i++)
         bld.MOV(offset(dest, bld, i), s.tes_payload().coords[i]);
      break;

   case nir_intrinsic_load_input:
   case nir_intrinsic_load_per_vertex_input: {
      assert(instr->def.bit_size == 32);
      elk_fs_reg indirect_offset = get_indirect_offset(ntb, instr);
      unsigned imm_offset = nir_intrinsic_base(instr);
      unsigned first_component = nir_intrinsic_component(instr);

      elk_fs_inst *inst;
      if (indirect_offset.file == BAD_FILE) {
         /* Arbitrarily only push up to 32 vec4 slots worth of data,
          * which is 16 registers (since each holds 2 vec4 slots).
          */
         const unsigned max_push_slots = 32;
         if (imm_offset < max_push_slots) {
            const elk_fs_reg src = horiz_offset(elk_fs_reg(ATTR, 0, dest.type),
                                            4 * imm_offset + first_component);
            for (int i = 0; i < instr->num_components; i++)
               bld.MOV(offset(dest, bld, i), component(src, i));

            tes_prog_data->base.urb_read_length =
               MAX2(tes_prog_data->base.urb_read_length,
                    (imm_offset / 2) + 1);
         } else {
            /* Replicate the patch handle to all enabled channels */
            elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
            srcs[URB_LOGICAL_SRC_HANDLE] = s.tes_payload().patch_urb_input;

            if (first_component != 0) {
               unsigned read_components =
                  instr->num_components + first_component;
               elk_fs_reg tmp = bld.vgrf(dest.type, read_components);
               inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp,
                               srcs, ARRAY_SIZE(srcs));
               inst->size_written = read_components * REG_SIZE * reg_unit(devinfo);
               for (unsigned i = 0; i < instr->num_components; i++) {
                  bld.MOV(offset(dest, bld, i),
                          offset(tmp, bld, i + first_component));
               }
            } else {
               inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dest,
                               srcs, ARRAY_SIZE(srcs));
               inst->size_written = instr->num_components * REG_SIZE * reg_unit(devinfo);
            }
            inst->offset = imm_offset;
         }
      } else {
         /* Indirect indexing - use per-slot offsets as well. */

         /* We can only read two double components with each URB read, so
          * we send two read messages in that case, each one loading up to
          * two double components.
          */
         unsigned num_components = instr->num_components;

         elk_fs_reg srcs[URB_LOGICAL_NUM_SRCS];
         srcs[URB_LOGICAL_SRC_HANDLE] = s.tes_payload().patch_urb_input;
         srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = indirect_offset;

         if (first_component != 0) {
            unsigned read_components =
                num_components + first_component;
            elk_fs_reg tmp = bld.vgrf(dest.type, read_components);
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, tmp,
                            srcs, ARRAY_SIZE(srcs));
            for (unsigned i = 0; i < num_components; i++) {
               bld.MOV(offset(dest, bld, i),
                       offset(tmp, bld, i + first_component));
            }
         } else {
            inst = bld.emit(ELK_SHADER_OPCODE_URB_READ_LOGICAL, dest,
                            srcs, ARRAY_SIZE(srcs));
         }
         inst->offset = imm_offset;
         inst->size_written = (num_components + first_component) *
                              inst->dst.component_size(inst->exec_size);
      }
      break;
   }
   default:
      fs_nir_emit_intrinsic(ntb, bld, instr);
      break;
   }
}

static void
fs_nir_emit_gs_intrinsic(nir_to_elk_state &ntb,
                         nir_intrinsic_instr *instr)
{
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_GEOMETRY);
   elk_fs_reg indirect_offset;

   elk_fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_load_primitive_id:
      assert(s.stage == MESA_SHADER_GEOMETRY);
      assert(elk_gs_prog_data(s.prog_data)->include_primitive_id);
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_UD), s.gs_payload().primitive_id);
      break;

   case nir_intrinsic_load_input:
      unreachable("load_input intrinsics are invalid for the GS stage");

   case nir_intrinsic_load_per_vertex_input:
      emit_gs_input_load(ntb, dest, instr->src[0], nir_intrinsic_base(instr),
                         instr->src[1], instr->num_components,
                         nir_intrinsic_component(instr));
      break;

   case nir_intrinsic_emit_vertex_with_counter:
      emit_gs_vertex(ntb, instr->src[0], nir_intrinsic_stream_id(instr));
      break;

   case nir_intrinsic_end_primitive_with_counter:
      emit_gs_end_primitive(ntb, instr->src[0]);
      break;

   case nir_intrinsic_set_vertex_and_primitive_count:
      bld.MOV(s.final_gs_vertex_count, get_nir_src(ntb, instr->src[0]));
      break;

   case nir_intrinsic_load_invocation_id: {
      elk_fs_reg val = ntb.system_values[SYSTEM_VALUE_INVOCATION_ID];
      assert(val.file != BAD_FILE);
      dest.type = val.type;
      bld.MOV(dest, val);
      break;
   }

   default:
      fs_nir_emit_intrinsic(ntb, bld, instr);
      break;
   }
}

/**
 * Fetch the current render target layer index.
 */
static elk_fs_reg
fetch_render_target_array_index(const fs_builder &bld)
{
   if (bld.shader->devinfo->ver >= 6) {
      /* The render target array index is provided in the thread payload as
       * bits 26:16 of r0.0.
       */
      const elk_fs_reg idx = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.AND(idx, elk_uw1_reg(ELK_GENERAL_REGISTER_FILE, 0, 1),
              elk_imm_uw(0x7ff));
      return idx;
   } else {
      /* Pre-SNB we only ever render into the first layer of the framebuffer
       * since layered rendering is not implemented.
       */
      return elk_imm_ud(0);
   }
}

/* Sample from the MCS surface attached to this multisample texture. */
static elk_fs_reg
emit_mcs_fetch(nir_to_elk_state &ntb, const elk_fs_reg &coordinate, unsigned components,
               const elk_fs_reg &texture,
               const elk_fs_reg &texture_handle)
{
   const fs_builder &bld = ntb.bld;

   const elk_fs_reg dest = ntb.s.vgrf(glsl_uvec4_type());

   elk_fs_reg srcs[TEX_LOGICAL_NUM_SRCS];
   srcs[TEX_LOGICAL_SRC_COORDINATE] = coordinate;
   srcs[TEX_LOGICAL_SRC_SURFACE] = texture;
   srcs[TEX_LOGICAL_SRC_SAMPLER] = elk_imm_ud(0);
   srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE] = texture_handle;
   srcs[TEX_LOGICAL_SRC_COORD_COMPONENTS] = elk_imm_d(components);
   srcs[TEX_LOGICAL_SRC_GRAD_COMPONENTS] = elk_imm_d(0);
   srcs[TEX_LOGICAL_SRC_RESIDENCY] = elk_imm_d(0);

   elk_fs_inst *inst = bld.emit(ELK_SHADER_OPCODE_TXF_MCS_LOGICAL, dest, srcs,
                            ARRAY_SIZE(srcs));

   /* We only care about one or two regs of response, but the sampler always
    * writes 4/8.
    */
   inst->size_written = 4 * dest.component_size(inst->exec_size);

   return dest;
}

/**
 * Fake non-coherent framebuffer read implemented using TXF to fetch from the
 * framebuffer at the current fragment coordinates and sample index.
 */
static elk_fs_inst *
emit_non_coherent_fb_read(nir_to_elk_state &ntb, const fs_builder &bld, const elk_fs_reg &dst,
                          unsigned target)
{
   elk_fs_visitor &s = ntb.s;

   assert(bld.shader->stage == MESA_SHADER_FRAGMENT);
   const elk_wm_prog_key *wm_key =
      reinterpret_cast<const elk_wm_prog_key *>(s.key);
   assert(!wm_key->coherent_fb_fetch);

   /* Calculate the fragment coordinates. */
   const elk_fs_reg coords = bld.vgrf(ELK_REGISTER_TYPE_UD, 3);
   bld.MOV(offset(coords, bld, 0), s.pixel_x);
   bld.MOV(offset(coords, bld, 1), s.pixel_y);
   bld.MOV(offset(coords, bld, 2), fetch_render_target_array_index(bld));

   /* Calculate the sample index and MCS payload when multisampling.  Luckily
    * the MCS fetch message behaves deterministically for UMS surfaces, so it
    * shouldn't be necessary to recompile based on whether the framebuffer is
    * CMS or UMS.
    */
   assert(wm_key->multisample_fbo == ELK_ALWAYS ||
          wm_key->multisample_fbo == ELK_NEVER);
   if (wm_key->multisample_fbo &&
       ntb.system_values[SYSTEM_VALUE_SAMPLE_ID].file == BAD_FILE)
      ntb.system_values[SYSTEM_VALUE_SAMPLE_ID] = emit_sampleid_setup(ntb);

   const elk_fs_reg sample = ntb.system_values[SYSTEM_VALUE_SAMPLE_ID];
   const elk_fs_reg mcs = wm_key->multisample_fbo ?
      emit_mcs_fetch(ntb, coords, 3, elk_imm_ud(target), elk_fs_reg()) : elk_fs_reg();

   /* Use either a normal or a CMS texel fetch message depending on whether
    * the framebuffer is single or multisample.  On SKL+ use the wide CMS
    * message just in case the framebuffer uses 16x multisampling, it should
    * be equivalent to the normal CMS fetch for lower multisampling modes.
    */
   elk_opcode op;
   if (wm_key->multisample_fbo) {
      op = ELK_SHADER_OPCODE_TXF_CMS_LOGICAL;
   } else {
      op = ELK_SHADER_OPCODE_TXF_LOGICAL;
   }

   /* Emit the instruction. */
   elk_fs_reg srcs[TEX_LOGICAL_NUM_SRCS];
   srcs[TEX_LOGICAL_SRC_COORDINATE]       = coords;
   srcs[TEX_LOGICAL_SRC_LOD]              = elk_imm_ud(0);
   srcs[TEX_LOGICAL_SRC_SAMPLE_INDEX]     = sample;
   srcs[TEX_LOGICAL_SRC_MCS]              = mcs;
   srcs[TEX_LOGICAL_SRC_SURFACE]          = elk_imm_ud(target);
   srcs[TEX_LOGICAL_SRC_SAMPLER]          = elk_imm_ud(0);
   srcs[TEX_LOGICAL_SRC_COORD_COMPONENTS] = elk_imm_ud(3);
   srcs[TEX_LOGICAL_SRC_GRAD_COMPONENTS]  = elk_imm_ud(0);
   srcs[TEX_LOGICAL_SRC_RESIDENCY]        = elk_imm_ud(0);

   elk_fs_inst *inst = bld.emit(op, dst, srcs, ARRAY_SIZE(srcs));
   inst->size_written = 4 * inst->dst.component_size(inst->exec_size);

   return inst;
}

static elk_fs_reg
alloc_temporary(const fs_builder &bld, unsigned size, elk_fs_reg *regs, unsigned n)
{
   if (n && regs[0].file != BAD_FILE) {
      return regs[0];

   } else {
      const elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_F, size);

      for (unsigned i = 0; i < n; i++)
         regs[i] = tmp;

      return tmp;
   }
}

static elk_fs_reg
alloc_frag_output(nir_to_elk_state &ntb, unsigned location)
{
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_FRAGMENT);
   const elk_wm_prog_key *const key =
      reinterpret_cast<const elk_wm_prog_key *>(s.key);
   const unsigned l = GET_FIELD(location, ELK_NIR_FRAG_OUTPUT_LOCATION);
   const unsigned i = GET_FIELD(location, ELK_NIR_FRAG_OUTPUT_INDEX);

   if (i > 0 || (key->force_dual_color_blend && l == FRAG_RESULT_DATA1))
      return alloc_temporary(ntb.bld, 4, &s.dual_src_output, 1);

   else if (l == FRAG_RESULT_COLOR)
      return alloc_temporary(ntb.bld, 4, s.outputs,
                             MAX2(key->nr_color_regions, 1));

   else if (l == FRAG_RESULT_DEPTH)
      return alloc_temporary(ntb.bld, 1, &s.frag_depth, 1);

   else if (l == FRAG_RESULT_STENCIL)
      return alloc_temporary(ntb.bld, 1, &s.frag_stencil, 1);

   else if (l == FRAG_RESULT_SAMPLE_MASK)
      return alloc_temporary(ntb.bld, 1, &s.sample_mask, 1);

   else if (l >= FRAG_RESULT_DATA0 &&
            l < FRAG_RESULT_DATA0 + ELK_MAX_DRAW_BUFFERS)
      return alloc_temporary(ntb.bld, 4,
                             &s.outputs[l - FRAG_RESULT_DATA0], 1);

   else
      unreachable("Invalid location");
}

static void
emit_is_helper_invocation(nir_to_elk_state &ntb, elk_fs_reg result)
{
   const fs_builder &bld = ntb.bld;

   /* Unlike the regular gl_HelperInvocation, that is defined at dispatch,
    * the helperInvocationEXT() (aka SpvOpIsHelperInvocationEXT) takes into
    * consideration demoted invocations.
    */
   result.type = ELK_REGISTER_TYPE_UD;

   bld.MOV(result, elk_imm_ud(0));

   /* See elk_sample_mask_reg() for why we split SIMD32 into SIMD16 here. */
   unsigned width = bld.dispatch_width();
   for (unsigned i = 0; i < DIV_ROUND_UP(width, 16); i++) {
      const fs_builder b = bld.group(MIN2(width, 16), i);

      elk_fs_inst *mov = b.MOV(offset(result, b, i), elk_imm_ud(~0));

      /* The at() ensures that any code emitted to get the predicate happens
       * before the mov right above.  This is not an issue elsewhere because
       * lowering code already set up the builder this way.
       */
      elk_emit_predicate_on_sample_mask(b.at(NULL, mov), mov);
      mov->predicate_inverse = true;
   }
}

static void
emit_fragcoord_interpolation(nir_to_elk_state &ntb, elk_fs_reg wpos)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_FRAGMENT);

   /* gl_FragCoord.x */
   bld.MOV(wpos, s.pixel_x);
   wpos = offset(wpos, bld, 1);

   /* gl_FragCoord.y */
   bld.MOV(wpos, s.pixel_y);
   wpos = offset(wpos, bld, 1);

   /* gl_FragCoord.z */
   if (devinfo->ver >= 6) {
      bld.MOV(wpos, s.pixel_z);
   } else {
      bld.emit(ELK_FS_OPCODE_LINTERP, wpos,
               s.delta_xy[ELK_BARYCENTRIC_PERSPECTIVE_PIXEL],
               s.interp_reg(bld, VARYING_SLOT_POS, 2, 0));
   }
   wpos = offset(wpos, bld, 1);

   /* gl_FragCoord.w: Already set up in emit_interpolation */
   bld.MOV(wpos, s.wpos_w);
}

static elk_fs_reg
emit_frontfacing_interpolation(nir_to_elk_state &ntb)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;

   elk_fs_reg ff = bld.vgrf(ELK_REGISTER_TYPE_D);

   if (devinfo->ver >= 6) {
      /* Bit 15 of g0.0 is 0 if the polygon is front facing. We want to create
       * a boolean result from this (~0/true or 0/false).
       *
       * We can use the fact that bit 15 is the MSB of g0.0:W to accomplish
       * this task in only one instruction:
       *    - a negation source modifier will flip the bit; and
       *    - a W -> D type conversion will sign extend the bit into the high
       *      word of the destination.
       *
       * An ASR 15 fills the low word of the destination.
       */
      elk_fs_reg g0 = elk_fs_reg(retype(elk_vec1_grf(0, 0), ELK_REGISTER_TYPE_W));
      g0.negate = true;

      bld.ASR(ff, g0, elk_imm_d(15));
   } else {
      /* Bit 31 of g1.6 is 0 if the polygon is front facing. We want to create
       * a boolean result from this (1/true or 0/false).
       *
       * Like in the above case, since the bit is the MSB of g1.6:UD we can use
       * the negation source modifier to flip it. Unfortunately the SHR
       * instruction only operates on UD (or D with an abs source modifier)
       * sources without negation.
       *
       * Instead, use ASR (which will give ~0/true or 0/false).
       */
      elk_fs_reg g1_6 = elk_fs_reg(retype(elk_vec1_grf(1, 6), ELK_REGISTER_TYPE_D));
      g1_6.negate = true;

      bld.ASR(ff, g1_6, elk_imm_d(31));
   }

   return ff;
}

static elk_fs_reg
emit_samplepos_setup(nir_to_elk_state &ntb)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_FRAGMENT);
   struct elk_wm_prog_data *wm_prog_data = elk_wm_prog_data(s.prog_data);
   assert(devinfo->ver >= 6);

   const fs_builder abld = bld.annotate("compute sample position");
   elk_fs_reg pos = abld.vgrf(ELK_REGISTER_TYPE_F, 2);

   if (wm_prog_data->persample_dispatch == ELK_NEVER) {
      /* From ARB_sample_shading specification:
       * "When rendering to a non-multisample buffer, or if multisample
       *  rasterization is disabled, gl_SamplePosition will always be
       *  (0.5, 0.5).
       */
      bld.MOV(offset(pos, bld, 0), elk_imm_f(0.5f));
      bld.MOV(offset(pos, bld, 1), elk_imm_f(0.5f));
      return pos;
   }

   /* WM will be run in MSDISPMODE_PERSAMPLE. So, only one of SIMD8 or SIMD16
    * mode will be enabled.
    *
    * From the Ivy Bridge PRM, volume 2 part 1, page 344:
    * R31.1:0         Position Offset X/Y for Slot[3:0]
    * R31.3:2         Position Offset X/Y for Slot[7:4]
    * .....
    *
    * The X, Y sample positions come in as bytes in  thread payload. So, read
    * the positions using vstride=16, width=8, hstride=2.
    */
   const elk_fs_reg sample_pos_reg =
      fetch_payload_reg(abld, s.fs_payload().sample_pos_reg, ELK_REGISTER_TYPE_W);

   for (unsigned i = 0; i < 2; i++) {
      elk_fs_reg tmp_d = bld.vgrf(ELK_REGISTER_TYPE_D);
      abld.MOV(tmp_d, subscript(sample_pos_reg, ELK_REGISTER_TYPE_B, i));
      /* Convert int_sample_pos to floating point */
      elk_fs_reg tmp_f = bld.vgrf(ELK_REGISTER_TYPE_F);
      abld.MOV(tmp_f, tmp_d);
      /* Scale to the range [0, 1] */
      abld.MUL(offset(pos, abld, i), tmp_f, elk_imm_f(1 / 16.0f));
   }

   if (wm_prog_data->persample_dispatch == ELK_SOMETIMES) {
      check_dynamic_msaa_flag(abld, wm_prog_data,
                              INTEL_MSAA_FLAG_PERSAMPLE_DISPATCH);
      for (unsigned i = 0; i < 2; i++) {
         set_predicate(ELK_PREDICATE_NORMAL,
                       bld.SEL(offset(pos, abld, i), offset(pos, abld, i),
                               elk_imm_f(0.5f)));
      }
   }

   return pos;
}

static elk_fs_reg
emit_sampleid_setup(nir_to_elk_state &ntb)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_FRAGMENT);
   ASSERTED elk_wm_prog_key *key = (elk_wm_prog_key*) s.key;
   struct elk_wm_prog_data *wm_prog_data = elk_wm_prog_data(s.prog_data);
   assert(devinfo->ver >= 6);

   const fs_builder abld = bld.annotate("compute sample id");
   elk_fs_reg sample_id = abld.vgrf(ELK_REGISTER_TYPE_UD);

   assert(key->multisample_fbo != ELK_NEVER);

   if (devinfo->ver >= 8) {
      /* Sample ID comes in as 4-bit numbers in g1.0:
       *
       *    15:12 Slot 3 SampleID (only used in SIMD16)
       *     11:8 Slot 2 SampleID (only used in SIMD16)
       *      7:4 Slot 1 SampleID
       *      3:0 Slot 0 SampleID
       *
       * Each slot corresponds to four channels, so we want to replicate each
       * half-byte value to 4 channels in a row:
       *
       *    dst+0:    .7    .6    .5    .4    .3    .2    .1    .0
       *             7:4   7:4   7:4   7:4   3:0   3:0   3:0   3:0
       *
       *    dst+1:    .7    .6    .5    .4    .3    .2    .1    .0  (if SIMD16)
       *           15:12 15:12 15:12 15:12  11:8  11:8  11:8  11:8
       *
       * First, we read g1.0 with a <1,8,0>UB region, causing the first 8
       * channels to read the first byte (7:0), and the second group of 8
       * channels to read the second byte (15:8).  Then, we shift right by
       * a vector immediate of <4, 4, 4, 4, 0, 0, 0, 0>, moving the slot 1 / 3
       * values into place.  Finally, we AND with 0xf to keep the low nibble.
       *
       *    shr(16) tmp<1>W g1.0<1,8,0>B 0x44440000:V
       *    and(16) dst<1>D tmp<8,8,1>W  0xf:W
       *
       * TODO: These payload bits exist on Gfx7 too, but they appear to always
       *       be zero, so this code fails to work.  We should find out why.
       */
      const elk_fs_reg tmp = abld.vgrf(ELK_REGISTER_TYPE_UW);

      for (unsigned i = 0; i < DIV_ROUND_UP(s.dispatch_width, 16); i++) {
         const fs_builder hbld = abld.group(MIN2(16, s.dispatch_width), i);
         /* According to the "PS Thread Payload for Normal Dispatch"
          * pages on the BSpec, the sample ids are stored in R1.0/R2.0 on gfx8+.
          */
         const struct elk_reg id_reg = elk_vec1_grf(i + 1, 0);
         hbld.SHR(offset(tmp, hbld, i),
                  stride(retype(id_reg, ELK_REGISTER_TYPE_UB), 1, 8, 0),
                  elk_imm_v(0x44440000));
      }

      abld.AND(sample_id, tmp, elk_imm_w(0xf));
   } else {
      const elk_fs_reg t1 = component(abld.vgrf(ELK_REGISTER_TYPE_UD), 0);
      const elk_fs_reg t2 = abld.vgrf(ELK_REGISTER_TYPE_UW);

      /* The PS will be run in MSDISPMODE_PERSAMPLE. For example with
       * 8x multisampling, subspan 0 will represent sample N (where N
       * is 0, 2, 4 or 6), subspan 1 will represent sample 1, 3, 5 or
       * 7. We can find the value of N by looking at R0.0 bits 7:6
       * ("Starting Sample Pair Index (SSPI)") and multiplying by two
       * (since samples are always delivered in pairs). That is, we
       * compute 2*((R0.0 & 0xc0) >> 6) == (R0.0 & 0xc0) >> 5. Then
       * we need to add N to the sequence (0, 0, 0, 0, 1, 1, 1, 1) in
       * case of SIMD8 and sequence (0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
       * 2, 3, 3, 3, 3) in case of SIMD16. We compute this sequence by
       * populating a temporary variable with the sequence (0, 1, 2, 3),
       * and then reading from it using vstride=1, width=4, hstride=0.
       * These computations hold good for 4x multisampling as well.
       *
       * For 2x MSAA and SIMD16, we want to use the sequence (0, 1, 0, 1):
       * the first four slots are sample 0 of subspan 0; the next four
       * are sample 1 of subspan 0; the third group is sample 0 of
       * subspan 1, and finally sample 1 of subspan 1.
       */

      /* SKL+ has an extra bit for the Starting Sample Pair Index to
       * accommodate 16x MSAA.
       */
      abld.exec_all().group(1, 0)
          .AND(t1, elk_fs_reg(retype(elk_vec1_grf(0, 0), ELK_REGISTER_TYPE_UD)),
               elk_imm_ud(0xc0));
      abld.exec_all().group(1, 0).SHR(t1, t1, elk_imm_d(5));

      /* This works for SIMD8-SIMD16.  It also works for SIMD32 but only if we
       * can assume 4x MSAA.  Disallow it on IVB+
       *
       * FINISHME: One day, we could come up with a way to do this that
       * actually works on gfx7.
       */
      if (devinfo->ver >= 7)
         s.limit_dispatch_width(16, "gl_SampleId is unsupported in SIMD32 on gfx7");
      abld.exec_all().group(8, 0).MOV(t2, elk_imm_v(0x32103210));

      /* This special instruction takes care of setting vstride=1,
       * width=4, hstride=0 of t2 during an ADD instruction.
       */
      abld.emit(ELK_FS_OPCODE_SET_SAMPLE_ID, sample_id, t1, t2);
   }

   if (key->multisample_fbo == ELK_SOMETIMES) {
      check_dynamic_msaa_flag(abld, wm_prog_data,
                              INTEL_MSAA_FLAG_MULTISAMPLE_FBO);
      set_predicate(ELK_PREDICATE_NORMAL,
                    abld.SEL(sample_id, sample_id, elk_imm_ud(0)));
   }

   return sample_id;
}

static elk_fs_reg
emit_samplemaskin_setup(nir_to_elk_state &ntb)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_FRAGMENT);
   struct elk_wm_prog_data *wm_prog_data = elk_wm_prog_data(s.prog_data);
   assert(devinfo->ver >= 6);

   elk_fs_reg coverage_mask =
      fetch_payload_reg(bld, s.fs_payload().sample_mask_in_reg, ELK_REGISTER_TYPE_D);

   if (wm_prog_data->persample_dispatch == ELK_NEVER)
      return coverage_mask;

   /* gl_SampleMaskIn[] comes from two sources: the input coverage mask,
    * and a mask representing which sample is being processed by the
    * current shader invocation.
    *
    * From the OES_sample_variables specification:
    * "When per-sample shading is active due to the use of a fragment input
    *  qualified by "sample" or due to the use of the gl_SampleID or
    *  gl_SamplePosition variables, only the bit for the current sample is
    *  set in gl_SampleMaskIn."
    */
   const fs_builder abld = bld.annotate("compute gl_SampleMaskIn");

   if (ntb.system_values[SYSTEM_VALUE_SAMPLE_ID].file == BAD_FILE)
      ntb.system_values[SYSTEM_VALUE_SAMPLE_ID] = emit_sampleid_setup(ntb);

   elk_fs_reg one = s.vgrf(glsl_int_type());
   elk_fs_reg enabled_mask = s.vgrf(glsl_int_type());
   abld.MOV(one, elk_imm_d(1));
   abld.SHL(enabled_mask, one, ntb.system_values[SYSTEM_VALUE_SAMPLE_ID]);
   elk_fs_reg mask = bld.vgrf(ELK_REGISTER_TYPE_D);
   abld.AND(mask, enabled_mask, coverage_mask);

   if (wm_prog_data->persample_dispatch == ELK_ALWAYS)
      return mask;

   check_dynamic_msaa_flag(abld, wm_prog_data,
                           INTEL_MSAA_FLAG_PERSAMPLE_DISPATCH);
   set_predicate(ELK_PREDICATE_NORMAL, abld.SEL(mask, mask, coverage_mask));

   return mask;
}

static void
fs_nir_emit_fs_intrinsic(nir_to_elk_state &ntb,
                         nir_intrinsic_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(s.stage == MESA_SHADER_FRAGMENT);

   elk_fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_load_front_face:
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_D),
              emit_frontfacing_interpolation(ntb));
      break;

   case nir_intrinsic_load_sample_pos:
   case nir_intrinsic_load_sample_pos_or_center: {
      elk_fs_reg sample_pos = ntb.system_values[SYSTEM_VALUE_SAMPLE_POS];
      assert(sample_pos.file != BAD_FILE);
      dest.type = sample_pos.type;
      bld.MOV(dest, sample_pos);
      bld.MOV(offset(dest, bld, 1), offset(sample_pos, bld, 1));
      break;
   }

   case nir_intrinsic_load_layer_id:
      dest.type = ELK_REGISTER_TYPE_UD;
      bld.MOV(dest, fetch_render_target_array_index(bld));
      break;

   case nir_intrinsic_is_helper_invocation:
      emit_is_helper_invocation(ntb, dest);
      break;

   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_load_sample_mask_in:
   case nir_intrinsic_load_sample_id:
   case nir_intrinsic_load_frag_shading_rate: {
      gl_system_value sv = nir_system_value_from_intrinsic(instr->intrinsic);
      elk_fs_reg val = ntb.system_values[sv];
      assert(val.file != BAD_FILE);
      dest.type = val.type;
      bld.MOV(dest, val);
      break;
   }

   case nir_intrinsic_store_output: {
      const elk_fs_reg src = get_nir_src(ntb, instr->src[0]);
      const unsigned store_offset = nir_src_as_uint(instr->src[1]);
      const unsigned location = nir_intrinsic_base(instr) +
         SET_FIELD(store_offset, ELK_NIR_FRAG_OUTPUT_LOCATION);
      const elk_fs_reg new_dest = retype(alloc_frag_output(ntb, location),
                                     src.type);

      for (unsigned j = 0; j < instr->num_components; j++)
         bld.MOV(offset(new_dest, bld, nir_intrinsic_component(instr) + j),
                 offset(src, bld, j));

      break;
   }

   case nir_intrinsic_load_output: {
      const unsigned l = GET_FIELD(nir_intrinsic_base(instr),
                                   ELK_NIR_FRAG_OUTPUT_LOCATION);
      assert(l >= FRAG_RESULT_DATA0);
      const unsigned load_offset = nir_src_as_uint(instr->src[0]);
      const unsigned target = l - FRAG_RESULT_DATA0 + load_offset;
      const elk_fs_reg tmp = bld.vgrf(dest.type, 4);

      assert(!reinterpret_cast<const elk_wm_prog_key *>(s.key)->coherent_fb_fetch);
      emit_non_coherent_fb_read(ntb, bld, tmp, target);

      for (unsigned j = 0; j < instr->num_components; j++) {
         bld.MOV(offset(dest, bld, j),
                 offset(tmp, bld, nir_intrinsic_component(instr) + j));
      }

      break;
   }

   case nir_intrinsic_demote:
   case nir_intrinsic_terminate:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate_if: {
      /* We track our discarded pixels in f0.1/f1.0.  By predicating on it, we
       * can update just the flag bits that aren't yet discarded.  If there's
       * no condition, we emit a CMP of g0 != g0, so all currently executing
       * channels will get turned off.
       */
      elk_fs_inst *cmp = NULL;
      if (instr->intrinsic == nir_intrinsic_demote_if ||
          instr->intrinsic == nir_intrinsic_terminate_if) {
         nir_alu_instr *alu = nir_src_as_alu_instr(instr->src[0]);

         if (alu != NULL &&
             alu->op != nir_op_bcsel &&
             (devinfo->ver > 5 ||
              (alu->instr.pass_flags & ELK_NIR_BOOLEAN_MASK) != ELK_NIR_BOOLEAN_NEEDS_RESOLVE ||
              alu->op == nir_op_fneu32 || alu->op == nir_op_feq32 ||
              alu->op == nir_op_flt32 || alu->op == nir_op_fge32 ||
              alu->op == nir_op_ine32 || alu->op == nir_op_ieq32 ||
              alu->op == nir_op_ilt32 || alu->op == nir_op_ige32 ||
              alu->op == nir_op_ult32 || alu->op == nir_op_uge32)) {
            /* Re-emit the instruction that generated the Boolean value, but
             * do not store it.  Since this instruction will be conditional,
             * other instructions that want to use the real Boolean value may
             * get garbage.  This was a problem for piglit's fs-discard-exit-2
             * test.
             *
             * Ideally we'd detect that the instruction cannot have a
             * conditional modifier before emitting the instructions.  Alas,
             * that is nigh impossible.  Instead, we're going to assume the
             * instruction (or last instruction) generated can have a
             * conditional modifier.  If it cannot, fallback to the old-style
             * compare, and hope dead code elimination will clean up the
             * extra instructions generated.
             */
            fs_nir_emit_alu(ntb, alu, false);

            cmp = (elk_fs_inst *) s.instructions.get_tail();
            if (cmp->conditional_mod == ELK_CONDITIONAL_NONE) {
               if (cmp->can_do_cmod())
                  cmp->conditional_mod = ELK_CONDITIONAL_Z;
               else
                  cmp = NULL;
            } else {
               /* The old sequence that would have been generated is,
                * basically, bool_result == false.  This is equivalent to
                * !bool_result, so negate the old modifier.
                */
               cmp->conditional_mod = elk_negate_cmod(cmp->conditional_mod);
            }
         }

         if (cmp == NULL) {
            cmp = bld.CMP(bld.null_reg_f(), get_nir_src(ntb, instr->src[0]),
                          elk_imm_d(0), ELK_CONDITIONAL_Z);
         }
      } else {
         elk_fs_reg some_reg = elk_fs_reg(retype(elk_vec8_grf(0, 0),
                                       ELK_REGISTER_TYPE_UW));
         cmp = bld.CMP(bld.null_reg_f(), some_reg, some_reg, ELK_CONDITIONAL_NZ);
      }

      cmp->predicate = ELK_PREDICATE_NORMAL;
      cmp->flag_subreg = sample_mask_flag_subreg(s);

      elk_fs_inst *jump = bld.emit(ELK_OPCODE_HALT);
      jump->flag_subreg = sample_mask_flag_subreg(s);
      jump->predicate_inverse = true;

      if (instr->intrinsic == nir_intrinsic_terminate ||
          instr->intrinsic == nir_intrinsic_terminate_if) {
         jump->predicate = ELK_PREDICATE_NORMAL;
      } else {
         /* Only jump when the whole quad is demoted.  For historical
          * reasons this is also used for discard.
          */
         jump->predicate = ELK_PREDICATE_ALIGN1_ANY4H;
      }

      if (devinfo->ver < 7)
         s.limit_dispatch_width(
            16, "Fragment discard/demote not implemented in SIMD32 mode.\n");
      break;
   }

   case nir_intrinsic_load_input:
   case nir_intrinsic_load_per_primitive_input: {
      /* In Fragment Shaders load_input is used either for flat inputs or
       * per-primitive inputs.
       */
      assert(instr->def.bit_size == 32);
      unsigned base = nir_intrinsic_base(instr);
      unsigned comp = nir_intrinsic_component(instr);
      unsigned num_components = instr->num_components;

      /* Special case fields in the VUE header */
      if (base == VARYING_SLOT_LAYER)
         comp = 1;
      else if (base == VARYING_SLOT_VIEWPORT)
         comp = 2;

      if (BITFIELD64_BIT(base) & s.nir->info.per_primitive_inputs) {
         assert(base != VARYING_SLOT_PRIMITIVE_INDICES);
         for (unsigned int i = 0; i < num_components; i++) {
            bld.MOV(offset(dest, bld, i),
                    retype(s.per_primitive_reg(bld, base, comp + i), dest.type));
         }
      } else {
         const unsigned k = 3;
         for (unsigned int i = 0; i < num_components; i++) {
            bld.MOV(offset(dest, bld, i),
                    retype(s.interp_reg(bld, base, comp + i, k), dest.type));
         }
      }
      break;
   }

   case nir_intrinsic_load_fs_input_interp_deltas: {
      assert(s.stage == MESA_SHADER_FRAGMENT);
      assert(nir_src_as_uint(instr->src[0]) == 0);
      const unsigned base = nir_intrinsic_base(instr);
      const unsigned comp = nir_intrinsic_component(instr);
      dest.type = ELK_REGISTER_TYPE_F;

      bld.MOV(offset(dest, bld, 0), s.interp_reg(bld, base, comp, 3));
      bld.MOV(offset(dest, bld, 1), s.interp_reg(bld, base, comp, 1));
      bld.MOV(offset(dest, bld, 2), s.interp_reg(bld, base, comp, 0));

      break;
   }

   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample: {
      /* Use the delta_xy values computed from the payload */
      enum elk_barycentric_mode bary = elk_barycentric_mode(instr);
      const elk_fs_reg srcs[] = { offset(s.delta_xy[bary], bld, 0),
                              offset(s.delta_xy[bary], bld, 1) };
      bld.LOAD_PAYLOAD(dest, srcs, ARRAY_SIZE(srcs), 0);
      break;
   }

   case nir_intrinsic_load_barycentric_at_sample: {
      const glsl_interp_mode interpolation =
         (enum glsl_interp_mode) nir_intrinsic_interp_mode(instr);

      elk_fs_reg msg_data;
      if (nir_src_is_const(instr->src[0])) {
         msg_data = elk_imm_ud(nir_src_as_uint(instr->src[0]) << 4);
      } else {
         const elk_fs_reg sample_src = retype(get_nir_src(ntb, instr->src[0]),
                                          ELK_REGISTER_TYPE_UD);
         const elk_fs_reg sample_id = bld.emit_uniformize(sample_src);
         msg_data = component(bld.group(8, 0).vgrf(ELK_REGISTER_TYPE_UD), 0);
         bld.exec_all().group(1, 0).SHL(msg_data, sample_id, elk_imm_ud(4u));
      }

      elk_fs_reg flag_reg;
      struct elk_wm_prog_key *wm_prog_key = (struct elk_wm_prog_key *) s.key;
      if (wm_prog_key->multisample_fbo == ELK_SOMETIMES) {
         struct elk_wm_prog_data *wm_prog_data = elk_wm_prog_data(s.prog_data);

         check_dynamic_msaa_flag(bld.exec_all().group(8, 0),
                                 wm_prog_data,
                                 INTEL_MSAA_FLAG_MULTISAMPLE_FBO);
         flag_reg = elk_flag_reg(0, 0);
      }

      emit_pixel_interpolater_send(bld,
                                   ELK_FS_OPCODE_INTERPOLATE_AT_SAMPLE,
                                   dest,
                                   elk_fs_reg(), /* src */
                                   msg_data,
                                   flag_reg,
                                   interpolation);
      break;
   }

   case nir_intrinsic_load_barycentric_at_offset: {
      const glsl_interp_mode interpolation =
         (enum glsl_interp_mode) nir_intrinsic_interp_mode(instr);

      nir_const_value *const_offset = nir_src_as_const_value(instr->src[0]);

      if (const_offset) {
         assert(nir_src_bit_size(instr->src[0]) == 32);
         unsigned off_x = const_offset[0].u32 & 0xf;
         unsigned off_y = const_offset[1].u32 & 0xf;

         emit_pixel_interpolater_send(bld,
                                      ELK_FS_OPCODE_INTERPOLATE_AT_SHARED_OFFSET,
                                      dest,
                                      elk_fs_reg(), /* src */
                                      elk_imm_ud(off_x | (off_y << 4)),
                                      elk_fs_reg(), /* flag_reg */
                                      interpolation);
      } else {
         elk_fs_reg src = retype(get_nir_src(ntb, instr->src[0]), ELK_REGISTER_TYPE_D);
         const enum elk_opcode opcode = ELK_FS_OPCODE_INTERPOLATE_AT_PER_SLOT_OFFSET;
         emit_pixel_interpolater_send(bld,
                                      opcode,
                                      dest,
                                      src,
                                      elk_imm_ud(0u),
                                      elk_fs_reg(), /* flag_reg */
                                      interpolation);
      }
      break;
   }

   case nir_intrinsic_load_frag_coord:
      emit_fragcoord_interpolation(ntb, dest);
      break;

   case nir_intrinsic_load_interpolated_input: {
      assert(instr->src[0].ssa &&
             instr->src[0].ssa->parent_instr->type == nir_instr_type_intrinsic);
      nir_intrinsic_instr *bary_intrinsic =
         nir_instr_as_intrinsic(instr->src[0].ssa->parent_instr);
      nir_intrinsic_op bary_intrin = bary_intrinsic->intrinsic;
      enum glsl_interp_mode interp_mode =
         (enum glsl_interp_mode) nir_intrinsic_interp_mode(bary_intrinsic);
      elk_fs_reg dst_xy;

      if (bary_intrin == nir_intrinsic_load_barycentric_at_offset ||
          bary_intrin == nir_intrinsic_load_barycentric_at_sample) {
         /* Use the result of the PI message. */
         dst_xy = retype(get_nir_src(ntb, instr->src[0]), ELK_REGISTER_TYPE_F);
      } else {
         /* Use the delta_xy values computed from the payload */
         enum elk_barycentric_mode bary = elk_barycentric_mode(bary_intrinsic);
         dst_xy = s.delta_xy[bary];
      }

      for (unsigned int i = 0; i < instr->num_components; i++) {
         elk_fs_reg interp =
            s.interp_reg(bld, nir_intrinsic_base(instr),
                         nir_intrinsic_component(instr) + i, 0);
         interp.type = ELK_REGISTER_TYPE_F;
         dest.type = ELK_REGISTER_TYPE_F;

         if (devinfo->ver < 6 && interp_mode == INTERP_MODE_SMOOTH) {
            elk_fs_reg tmp = s.vgrf(glsl_float_type());
            bld.emit(ELK_FS_OPCODE_LINTERP, tmp, dst_xy, interp);
            bld.MUL(offset(dest, bld, i), tmp, s.pixel_w);
         } else {
            bld.emit(ELK_FS_OPCODE_LINTERP, offset(dest, bld, i), dst_xy, interp);
         }
      }
      break;
   }

   default:
      fs_nir_emit_intrinsic(ntb, bld, instr);
      break;
   }
}

static void
fs_nir_emit_cs_intrinsic(nir_to_elk_state &ntb,
                         nir_intrinsic_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   assert(gl_shader_stage_uses_workgroup(s.stage));
   struct elk_cs_prog_data *cs_prog_data = elk_cs_prog_data(s.prog_data);

   elk_fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_barrier:
      if (nir_intrinsic_memory_scope(instr) != SCOPE_NONE)
         fs_nir_emit_intrinsic(ntb, bld, instr);
      if (nir_intrinsic_execution_scope(instr) == SCOPE_WORKGROUP) {
         /* The whole workgroup fits in a single HW thread, so all the
          * invocations are already executed lock-step.  Instead of an actual
          * barrier just emit a scheduling fence, that will generate no code.
          */
         if (!s.nir->info.workgroup_size_variable &&
             s.workgroup_size() <= s.dispatch_width) {
            bld.exec_all().group(1, 0).emit(ELK_FS_OPCODE_SCHEDULING_FENCE);
            break;
         }

         emit_barrier(ntb);
         cs_prog_data->uses_barrier = true;
      }
      break;

   case nir_intrinsic_load_subgroup_id:
      s.cs_payload().load_subgroup_id(bld, dest);
      break;

   case nir_intrinsic_load_workgroup_id: {
      elk_fs_reg val = ntb.system_values[SYSTEM_VALUE_WORKGROUP_ID];
      assert(val.file != BAD_FILE);
      dest.type = val.type;
      for (unsigned i = 0; i < 3; i++)
         bld.MOV(offset(dest, bld, i), offset(val, bld, i));
      break;
   }

   case nir_intrinsic_load_num_workgroups: {
      assert(instr->def.bit_size == 32);

      cs_prog_data->uses_num_work_groups = true;

      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] = elk_imm_ud(0);
      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(3); /* num components */
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = elk_imm_ud(0);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);
      elk_fs_inst *inst =
         bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_READ_LOGICAL,
                  dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
      inst->size_written = 3 * s.dispatch_width * 4;
      break;
   }

   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      fs_nir_emit_surface_atomic(ntb, bld, instr, elk_imm_ud(GFX7_BTI_SLM),
                                 false /* bindless */);
      break;

   case nir_intrinsic_load_shared: {
      assert(devinfo->ver >= 7);

      const unsigned bit_size = instr->def.bit_size;
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] = elk_imm_ud(GFX7_BTI_SLM);

      elk_fs_reg addr = get_nir_src(ntb, instr->src[0]);
      int base = nir_intrinsic_base(instr);
      if (base) {
         elk_fs_reg addr_off = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
         bld.ADD(addr_off, addr, elk_imm_d(base));
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] = addr_off;
      } else {
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] = addr;
      }

      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);

      /* Make dest unsigned because that's what the temporary will be */
      dest.type = elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);

      /* Read the vector */
      assert(bit_size <= 32);
      assert(nir_intrinsic_align(instr) > 0);
      if (bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         assert(instr->def.num_components <= 4);
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
         elk_fs_inst *inst =
            bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_READ_LOGICAL,
                     dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
         inst->size_written = instr->num_components * s.dispatch_width * 4;
      } else {
         assert(instr->def.num_components == 1);
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(bit_size);

         elk_fs_reg read_result = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.emit(ELK_SHADER_OPCODE_BYTE_SCATTERED_READ_LOGICAL,
                  read_result, srcs, SURFACE_LOGICAL_NUM_SRCS);
         bld.MOV(dest, subscript(read_result, dest.type, 0));
      }
      break;
   }

   case nir_intrinsic_store_shared: {
      assert(devinfo->ver >= 7);

      const unsigned bit_size = nir_src_bit_size(instr->src[0]);
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] = elk_imm_ud(GFX7_BTI_SLM);

      elk_fs_reg addr = get_nir_src(ntb, instr->src[1]);
      int base = nir_intrinsic_base(instr);
      if (base) {
         elk_fs_reg addr_off = bld.vgrf(ELK_REGISTER_TYPE_UD, 1);
         bld.ADD(addr_off, addr, elk_imm_d(base));
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] = addr_off;
      } else {
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] = addr;
      }

      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      /* No point in masking with sample mask, here we're handling compute
       * intrinsics.
       */
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);

      elk_fs_reg data = get_nir_src(ntb, instr->src[0]);
      data.type = elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);

      assert(bit_size <= 32);
      assert(nir_intrinsic_write_mask(instr) ==
             (1u << instr->num_components) - 1);
      assert(nir_intrinsic_align(instr) > 0);
      if (bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         assert(nir_src_num_components(instr->src[0]) <= 4);
         srcs[SURFACE_LOGICAL_SRC_DATA] = data;
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
         bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      } else {
         assert(nir_src_num_components(instr->src[0]) == 1);
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(bit_size);

         srcs[SURFACE_LOGICAL_SRC_DATA] = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.MOV(srcs[SURFACE_LOGICAL_SRC_DATA], data);

         bld.emit(ELK_SHADER_OPCODE_BYTE_SCATTERED_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      }
      break;
   }

   case nir_intrinsic_load_workgroup_size: {
      /* Should have been lowered by elk_nir_lower_cs_intrinsics() or
       * crocus/iris_setup_uniforms() for the variable group size case.
       */
      unreachable("Should have been lowered");
      break;
   }

   default:
      fs_nir_emit_intrinsic(ntb, bld, instr);
      break;
   }
}

static elk_fs_reg
elk_nir_reduction_op_identity(const fs_builder &bld,
                              nir_op op, elk_reg_type type)
{
   nir_const_value value = nir_alu_binop_identity(op, type_sz(type) * 8);
   switch (type_sz(type)) {
   case 1:
      if (type == ELK_REGISTER_TYPE_UB) {
         return elk_imm_uw(value.u8);
      } else {
         assert(type == ELK_REGISTER_TYPE_B);
         return elk_imm_w(value.i8);
      }
   case 2:
      return retype(elk_imm_uw(value.u16), type);
   case 4:
      return retype(elk_imm_ud(value.u32), type);
   case 8:
      if (type == ELK_REGISTER_TYPE_DF)
         return elk_setup_imm_df(bld, value.f64);
      else
         return retype(elk_imm_u64(value.u64), type);
   default:
      unreachable("Invalid type size");
   }
}

static elk_opcode
elk_op_for_nir_reduction_op(nir_op op)
{
   switch (op) {
   case nir_op_iadd: return ELK_OPCODE_ADD;
   case nir_op_fadd: return ELK_OPCODE_ADD;
   case nir_op_imul: return ELK_OPCODE_MUL;
   case nir_op_fmul: return ELK_OPCODE_MUL;
   case nir_op_imin: return ELK_OPCODE_SEL;
   case nir_op_umin: return ELK_OPCODE_SEL;
   case nir_op_fmin: return ELK_OPCODE_SEL;
   case nir_op_imax: return ELK_OPCODE_SEL;
   case nir_op_umax: return ELK_OPCODE_SEL;
   case nir_op_fmax: return ELK_OPCODE_SEL;
   case nir_op_iand: return ELK_OPCODE_AND;
   case nir_op_ior:  return ELK_OPCODE_OR;
   case nir_op_ixor: return ELK_OPCODE_XOR;
   default:
      unreachable("Invalid reduction operation");
   }
}

static elk_conditional_mod
elk_cond_mod_for_nir_reduction_op(nir_op op)
{
   switch (op) {
   case nir_op_iadd: return ELK_CONDITIONAL_NONE;
   case nir_op_fadd: return ELK_CONDITIONAL_NONE;
   case nir_op_imul: return ELK_CONDITIONAL_NONE;
   case nir_op_fmul: return ELK_CONDITIONAL_NONE;
   case nir_op_imin: return ELK_CONDITIONAL_L;
   case nir_op_umin: return ELK_CONDITIONAL_L;
   case nir_op_fmin: return ELK_CONDITIONAL_L;
   case nir_op_imax: return ELK_CONDITIONAL_GE;
   case nir_op_umax: return ELK_CONDITIONAL_GE;
   case nir_op_fmax: return ELK_CONDITIONAL_GE;
   case nir_op_iand: return ELK_CONDITIONAL_NONE;
   case nir_op_ior:  return ELK_CONDITIONAL_NONE;
   case nir_op_ixor: return ELK_CONDITIONAL_NONE;
   default:
      unreachable("Invalid reduction operation");
   }
}

struct rebuild_resource {
   unsigned idx;
   std::vector<nir_def *> array;
};

static bool
add_rebuild_src(nir_src *src, void *state)
{
   struct rebuild_resource *res = (struct rebuild_resource *) state;

   for (nir_def *def : res->array) {
      if (def == src->ssa)
         return true;
   }

   nir_foreach_src(src->ssa->parent_instr, add_rebuild_src, state);
   res->array.push_back(src->ssa);
   return true;
}

static elk_fs_reg
try_rebuild_resource(nir_to_elk_state &ntb, const elk::fs_builder &bld, nir_def *resource_def)
{
   /* Create a build at the location of the resource_intel intrinsic */
   fs_builder ubld8 = bld.exec_all().group(8, 0);

   struct rebuild_resource resources = {};
   resources.idx = 0;

   if (!nir_foreach_src(resource_def->parent_instr,
                        add_rebuild_src, &resources))
      return elk_fs_reg();
   resources.array.push_back(resource_def);

   if (resources.array.size() == 1) {
      nir_def *def = resources.array[0];

      if (def->parent_instr->type == nir_instr_type_load_const) {
         nir_load_const_instr *load_const =
            nir_instr_as_load_const(def->parent_instr);
         return elk_imm_ud(load_const->value[0].i32);
      } else {
         assert(def->parent_instr->type == nir_instr_type_intrinsic &&
                (nir_instr_as_intrinsic(def->parent_instr)->intrinsic ==
                 nir_intrinsic_load_uniform));
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(def->parent_instr);
         unsigned base_offset = nir_intrinsic_base(intrin);
         unsigned load_offset = nir_src_as_uint(intrin->src[0]);
         elk_fs_reg src(UNIFORM, base_offset / 4, ELK_REGISTER_TYPE_UD);
         src.offset = load_offset + base_offset % 4;
         return src;
      }
   }

   for (unsigned i = 0; i < resources.array.size(); i++) {
      nir_def *def = resources.array[i];

      nir_instr *instr = def->parent_instr;
      switch (instr->type) {
      case nir_instr_type_load_const: {
         nir_load_const_instr *load_const =
            nir_instr_as_load_const(instr);
         elk_fs_reg dst = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
         ntb.resource_insts[def->index] =
            ubld8.MOV(dst, elk_imm_ud(load_const->value[0].i32));
         break;
      }

      case nir_instr_type_alu: {
         nir_alu_instr *alu = nir_instr_as_alu(instr);

         if (nir_op_infos[alu->op].num_inputs == 2) {
            if (alu->src[0].swizzle[0] != 0 ||
                alu->src[1].swizzle[0] != 0)
               break;
         } else if (nir_op_infos[alu->op].num_inputs == 3) {
            if (alu->src[0].swizzle[0] != 0 ||
                alu->src[1].swizzle[0] != 0 ||
                alu->src[2].swizzle[0] != 0)
               break;
         } else {
            /* Not supported ALU input count */
            break;
         }

         switch (alu->op) {
         case nir_op_iadd: {
            elk_fs_reg dst = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
            elk_fs_reg src0 = ntb.resource_insts[alu->src[0].src.ssa->index]->dst;
            elk_fs_reg src1 = ntb.resource_insts[alu->src[1].src.ssa->index]->dst;
            assert(src0.file != BAD_FILE && src1.file != BAD_FILE);
            assert(src0.type == ELK_REGISTER_TYPE_UD);
            ntb.resource_insts[def->index] =
               ubld8.ADD(dst,
                         src0.file != IMM ? src0 : src1,
                         src0.file != IMM ? src1 : src0);
            break;
         }
         case nir_op_ushr: {
            elk_fs_reg dst = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
            elk_fs_reg src0 = ntb.resource_insts[alu->src[0].src.ssa->index]->dst;
            elk_fs_reg src1 = ntb.resource_insts[alu->src[1].src.ssa->index]->dst;
            assert(src0.file != BAD_FILE && src1.file != BAD_FILE);
            assert(src0.type == ELK_REGISTER_TYPE_UD);
            ntb.resource_insts[def->index] = ubld8.SHR(dst, src0, src1);
            break;
         }
         case nir_op_ishl: {
            elk_fs_reg dst = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
            elk_fs_reg src0 = ntb.resource_insts[alu->src[0].src.ssa->index]->dst;
            elk_fs_reg src1 = ntb.resource_insts[alu->src[1].src.ssa->index]->dst;
            assert(src0.file != BAD_FILE && src1.file != BAD_FILE);
            assert(src0.type == ELK_REGISTER_TYPE_UD);
            ntb.resource_insts[def->index] = ubld8.SHL(dst, src0, src1);
            break;
         }
         case nir_op_mov: {
            break;
         }
         default:
            break;
         }
         break;
      }

      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_resource_intel:
            ntb.resource_insts[def->index] =
               ntb.resource_insts[intrin->src[1].ssa->index];
            break;

         case nir_intrinsic_load_uniform: {
            if (!nir_src_is_const(intrin->src[0]))
               break;

            unsigned base_offset = nir_intrinsic_base(intrin);
            unsigned load_offset = nir_src_as_uint(intrin->src[0]);
            elk_fs_reg dst = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
            elk_fs_reg src(UNIFORM, base_offset / 4, ELK_REGISTER_TYPE_UD);
            src.offset = load_offset + base_offset % 4;
            ntb.resource_insts[def->index] = ubld8.MOV(dst, src);
            break;
         }

         default:
            break;
         }
         break;
      }

      default:
         break;
      }

      if (ntb.resource_insts[def->index] == NULL)
         return elk_fs_reg();
   }

   assert(ntb.resource_insts[resource_def->index] != NULL);
   return component(ntb.resource_insts[resource_def->index]->dst, 0);
}

static elk_fs_reg
get_nir_image_intrinsic_image(nir_to_elk_state &ntb, const elk::fs_builder &bld,
                              nir_intrinsic_instr *instr)
{
   if (is_resource_src(instr->src[0])) {
      elk_fs_reg surf_index = get_resource_nir_src(ntb, instr->src[0]);
      if (surf_index.file != BAD_FILE)
         return surf_index;
   }

   elk_fs_reg image = retype(get_nir_src_imm(ntb, instr->src[0]), ELK_REGISTER_TYPE_UD);
   elk_fs_reg surf_index = image;

   return bld.emit_uniformize(surf_index);
}

static elk_fs_reg
get_nir_buffer_intrinsic_index(nir_to_elk_state &ntb, const elk::fs_builder &bld,
                               nir_intrinsic_instr *instr)
{
   /* SSBO stores are weird in that their index is in src[1] */
   const bool is_store =
      instr->intrinsic == nir_intrinsic_store_ssbo ||
      instr->intrinsic == nir_intrinsic_store_ssbo_block_intel;
   nir_src src = is_store ? instr->src[1] : instr->src[0];

   if (nir_src_is_const(src)) {
      return elk_imm_ud(nir_src_as_uint(src));
   } else if (is_resource_src(src)) {
      elk_fs_reg surf_index = get_resource_nir_src(ntb, src);
      if (surf_index.file != BAD_FILE)
         return surf_index;
   }
   return bld.emit_uniformize(get_nir_src(ntb, src));
}

/**
 * The offsets we get from NIR act as if each SIMD channel has it's own blob
 * of contiguous space.  However, if we actually place each SIMD channel in
 * it's own space, we end up with terrible cache performance because each SIMD
 * channel accesses a different cache line even when they're all accessing the
 * same byte offset.  To deal with this problem, we swizzle the address using
 * a simple algorithm which ensures that any time a SIMD message reads or
 * writes the same address, it's all in the same cache line.  We have to keep
 * the bottom two bits fixed so that we can read/write up to a dword at a time
 * and the individual element is contiguous.  We do this by splitting the
 * address as follows:
 *
 *    31                             4-6           2          0
 *    +-------------------------------+------------+----------+
 *    |        Hi address bits        | chan index | addr low |
 *    +-------------------------------+------------+----------+
 *
 * In other words, the bottom two address bits stay, and the top 30 get
 * shifted up so that we can stick the SIMD channel index in the middle.  This
 * way, we can access 8, 16, or 32-bit elements and, when accessing a 32-bit
 * at the same logical offset, the scratch read/write instruction acts on
 * continuous elements and we get good cache locality.
 */
static elk_fs_reg
swizzle_nir_scratch_addr(nir_to_elk_state &ntb,
                         const elk::fs_builder &bld,
                         const elk_fs_reg &nir_addr,
                         bool in_dwords)
{
   elk_fs_visitor &s = ntb.s;

   const elk_fs_reg &chan_index =
      ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION];
   const unsigned chan_index_bits = ffs(s.dispatch_width) - 1;

   elk_fs_reg addr = bld.vgrf(ELK_REGISTER_TYPE_UD);
   if (in_dwords) {
      /* In this case, we know the address is aligned to a DWORD and we want
       * the final address in DWORDs.
       */
      bld.SHL(addr, nir_addr, elk_imm_ud(chan_index_bits - 2));
      bld.OR(addr, addr, chan_index);
   } else {
      /* This case substantially more annoying because we have to pay
       * attention to those pesky two bottom bits.
       */
      elk_fs_reg addr_hi = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.AND(addr_hi, nir_addr, elk_imm_ud(~0x3u));
      bld.SHL(addr_hi, addr_hi, elk_imm_ud(chan_index_bits));
      elk_fs_reg chan_addr = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.SHL(chan_addr, chan_index, elk_imm_ud(2));
      bld.AND(addr, nir_addr, elk_imm_ud(0x3u));
      bld.OR(addr, addr, addr_hi);
      bld.OR(addr, addr, chan_addr);
   }
   return addr;
}

static unsigned
choose_oword_block_size_dwords(const struct intel_device_info *devinfo,
                               unsigned dwords)
{
   unsigned block;
   if (devinfo->has_lsc && dwords >= 64) {
      block = 64;
   } else if (dwords >= 32) {
      block = 32;
   } else if (dwords >= 16) {
      block = 16;
   } else {
      block = 8;
   }
   assert(block <= dwords);
   return block;
}

static void
increment_a64_address(const fs_builder &bld, elk_fs_reg address, uint32_t v)
{
   if (bld.shader->devinfo->has_64bit_int) {
      bld.ADD(address, address, elk_imm_ud(v));
   } else {
      elk_fs_reg low = retype(address, ELK_REGISTER_TYPE_UD);
      elk_fs_reg high = offset(low, bld, 1);

      /* Add low and if that overflows, add carry to high. */
      bld.ADD(low, low, elk_imm_ud(v))->conditional_mod = ELK_CONDITIONAL_O;
      bld.ADD(high, high, elk_imm_ud(0x1))->predicate = ELK_PREDICATE_NORMAL;
   }
}

static elk_fs_reg
emit_fence(const fs_builder &bld, enum elk_opcode opcode,
           uint8_t sfid, uint32_t desc,
           bool commit_enable, uint8_t bti)
{
   assert(opcode == ELK_SHADER_OPCODE_INTERLOCK ||
          opcode == ELK_SHADER_OPCODE_MEMORY_FENCE);

   elk_fs_reg dst = bld.vgrf(ELK_REGISTER_TYPE_UD);
   elk_fs_inst *fence = bld.emit(opcode, dst, elk_vec8_grf(0, 0),
                             elk_imm_ud(commit_enable),
                             elk_imm_ud(bti));
   fence->sfid = sfid;
   fence->desc = desc;

   return dst;
}

/**
 * Create a MOV to read the timestamp register.
 */
static elk_fs_reg
get_timestamp(const fs_builder &bld)
{
   elk_fs_visitor &s = *bld.shader;
   const intel_device_info *devinfo = s.devinfo;

   assert(devinfo->ver >= 7);

   elk_fs_reg ts = elk_fs_reg(retype(elk_vec4_reg(ELK_ARCHITECTURE_REGISTER_FILE,
                                          ELK_ARF_TIMESTAMP,
                                          0),
                             ELK_REGISTER_TYPE_UD));

   elk_fs_reg dst = elk_fs_reg(VGRF, s.alloc.allocate(1), ELK_REGISTER_TYPE_UD);

   /* We want to read the 3 fields we care about even if it's not enabled in
    * the dispatch.
    */
   bld.group(4, 0).exec_all().MOV(dst, ts);

   return dst;
}

static void
fs_nir_emit_intrinsic(nir_to_elk_state &ntb,
                      const fs_builder &bld, nir_intrinsic_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   elk_fs_visitor &s = ntb.s;

   /* We handle this as a special case */
   if (instr->intrinsic == nir_intrinsic_decl_reg) {
      assert(nir_intrinsic_num_array_elems(instr) == 0);
      unsigned bit_size = nir_intrinsic_bit_size(instr);
      unsigned num_components = nir_intrinsic_num_components(instr);
      const elk_reg_type reg_type =
         elk_reg_type_from_bit_size(bit_size, bit_size == 8 ?
                                              ELK_REGISTER_TYPE_D :
                                              ELK_REGISTER_TYPE_F);

      /* Re-use the destination's slot in the table for the register */
      ntb.ssa_values[instr->def.index] =
         bld.vgrf(reg_type, num_components);
      return;
   }

   elk_fs_reg dest;
   if (nir_intrinsic_infos[instr->intrinsic].has_dest)
      dest = get_nir_def(ntb, instr->def);

   switch (instr->intrinsic) {
   case nir_intrinsic_resource_intel:
      ntb.ssa_bind_infos[instr->def.index].valid = true;
      ntb.ssa_bind_infos[instr->def.index].bindless =
         (nir_intrinsic_resource_access_intel(instr) &
          nir_resource_intel_bindless) != 0;
      ntb.ssa_bind_infos[instr->def.index].block =
         nir_intrinsic_resource_block_intel(instr);
      ntb.ssa_bind_infos[instr->def.index].set =
         nir_intrinsic_desc_set(instr);
      ntb.ssa_bind_infos[instr->def.index].binding =
         nir_intrinsic_binding(instr);

      if (nir_intrinsic_resource_access_intel(instr) &
           nir_resource_intel_non_uniform) {
         ntb.resource_values[instr->def.index] = elk_fs_reg();
      } else {
         ntb.resource_values[instr->def.index] =
            try_rebuild_resource(ntb, bld, instr->src[1].ssa);
      }
      ntb.ssa_values[instr->def.index] =
         ntb.ssa_values[instr->src[1].ssa->index];
      break;

   case nir_intrinsic_load_reg:
   case nir_intrinsic_store_reg:
      /* Nothing to do with these. */
      break;

   case nir_intrinsic_image_load:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap: {
      /* Get some metadata from the image intrinsic. */
      const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];

      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];

      switch (instr->intrinsic) {
      case nir_intrinsic_image_load:
      case nir_intrinsic_image_store:
      case nir_intrinsic_image_atomic:
      case nir_intrinsic_image_atomic_swap:
         srcs[SURFACE_LOGICAL_SRC_SURFACE] =
            get_nir_image_intrinsic_image(ntb, bld, instr);
         break;

      default:
         /* Bindless */
         srcs[SURFACE_LOGICAL_SRC_SURFACE_HANDLE] =
            get_nir_image_intrinsic_image(ntb, bld, instr);
         break;
      }

      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = get_nir_src(ntb, instr->src[1]);
      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] =
         elk_imm_ud(nir_image_intrinsic_coord_components(instr));

      /* Emit an image load, store or atomic op. */
      if (instr->intrinsic == nir_intrinsic_image_load ||
          instr->intrinsic == nir_intrinsic_bindless_image_load) {
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
         srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);
         elk_fs_inst *inst =
            bld.emit(ELK_SHADER_OPCODE_TYPED_SURFACE_READ_LOGICAL,
                     dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
         inst->size_written = instr->num_components * s.dispatch_width * 4;
      } else if (instr->intrinsic == nir_intrinsic_image_store ||
                 instr->intrinsic == nir_intrinsic_bindless_image_store) {
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
         srcs[SURFACE_LOGICAL_SRC_DATA] = get_nir_src(ntb, instr->src[3]);
         srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(1);
         bld.emit(ELK_SHADER_OPCODE_TYPED_SURFACE_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      } else {
         unsigned num_srcs = info->num_srcs;
         enum elk_lsc_opcode op = elk_lsc_aop_for_nir_intrinsic(instr);
         if (op == LSC_OP_ATOMIC_INC || op == LSC_OP_ATOMIC_DEC) {
            assert(num_srcs == 4);
            num_srcs = 3;
         }

         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(op);

         elk_fs_reg data;
         if (num_srcs >= 4)
            data = get_nir_src(ntb, instr->src[3]);
         if (num_srcs >= 5) {
            elk_fs_reg tmp = bld.vgrf(data.type, 2);
            elk_fs_reg sources[2] = { data, get_nir_src(ntb, instr->src[4]) };
            bld.LOAD_PAYLOAD(tmp, sources, 2, 0);
            data = tmp;
         }
         srcs[SURFACE_LOGICAL_SRC_DATA] = data;
         srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(1);

         bld.emit(ELK_SHADER_OPCODE_TYPED_ATOMIC_LOGICAL,
                  dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
      }
      break;
   }

   case nir_intrinsic_image_size:
   case nir_intrinsic_bindless_image_size: {
      /* Cube image sizes should have previously been lowered to a 2D array */
      assert(nir_intrinsic_image_dim(instr) != GLSL_SAMPLER_DIM_CUBE);

      /* Unlike the [un]typed load and store opcodes, the TXS that this turns
       * into will handle the binding table index for us in the geneerator.
       * Incidentally, this means that we can handle bindless with exactly the
       * same code.
       */
      elk_fs_reg image = retype(get_nir_src_imm(ntb, instr->src[0]),
                            ELK_REGISTER_TYPE_UD);
      image = bld.emit_uniformize(image);

      assert(nir_src_as_uint(instr->src[1]) == 0);

      elk_fs_reg srcs[TEX_LOGICAL_NUM_SRCS];
      if (instr->intrinsic == nir_intrinsic_image_size)
         srcs[TEX_LOGICAL_SRC_SURFACE] = image;
      else
         srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE] = image;
      srcs[TEX_LOGICAL_SRC_SAMPLER] = elk_imm_d(0);
      srcs[TEX_LOGICAL_SRC_COORD_COMPONENTS] = elk_imm_d(0);
      srcs[TEX_LOGICAL_SRC_GRAD_COMPONENTS] = elk_imm_d(0);
      srcs[TEX_LOGICAL_SRC_RESIDENCY] = elk_imm_d(0);

      /* Since the image size is always uniform, we can just emit a SIMD8
       * query instruction and splat the result out.
       */
      const fs_builder ubld = bld.exec_all().group(8 * reg_unit(devinfo), 0);

      elk_fs_reg tmp = ubld.vgrf(ELK_REGISTER_TYPE_UD, 4);
      elk_fs_inst *inst = ubld.emit(ELK_SHADER_OPCODE_IMAGE_SIZE_LOGICAL,
                                tmp, srcs, ARRAY_SIZE(srcs));
      inst->size_written = 4 * REG_SIZE * reg_unit(devinfo);

      for (unsigned c = 0; c < instr->def.num_components; ++c) {
         bld.MOV(offset(retype(dest, tmp.type), bld, c),
                 component(offset(tmp, ubld, c), 0));
      }
      break;
   }

   case nir_intrinsic_image_load_raw_intel: {
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] =
         get_nir_image_intrinsic_image(ntb, bld, instr);
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = get_nir_src(ntb, instr->src[1]);
      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);

      elk_fs_inst *inst =
         bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_READ_LOGICAL,
                  dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
      inst->size_written = instr->num_components * s.dispatch_width * 4;
      break;
   }

   case nir_intrinsic_image_store_raw_intel: {
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] =
         get_nir_image_intrinsic_image(ntb, bld, instr);
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = get_nir_src(ntb, instr->src[1]);
      srcs[SURFACE_LOGICAL_SRC_DATA] = get_nir_src(ntb, instr->src[2]);
      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(1);

      bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_WRITE_LOGICAL,
               elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      break;
   }

   case nir_intrinsic_barrier:
   case nir_intrinsic_begin_invocation_interlock:
   case nir_intrinsic_end_invocation_interlock: {
      bool ugm_fence, slm_fence, tgm_fence, urb_fence;
      enum elk_opcode opcode = ELK_OPCODE_NOP;

      /* Handling interlock intrinsics here will allow the logic for IVB
       * render cache (see below) to be reused.
       */

      switch (instr->intrinsic) {
      case nir_intrinsic_barrier: {
         /* Note we only care about the memory part of the
          * barrier.  The execution part will be taken care
          * of by the stage specific intrinsic handler functions.
          */
         nir_variable_mode modes = nir_intrinsic_memory_modes(instr);
         ugm_fence = modes & (nir_var_mem_ssbo | nir_var_mem_global);
         slm_fence = modes & nir_var_mem_shared;
         tgm_fence = modes & nir_var_image;
         urb_fence = modes & (nir_var_shader_out | nir_var_mem_task_payload);
         if (nir_intrinsic_memory_scope(instr) != SCOPE_NONE)
            opcode = ELK_SHADER_OPCODE_MEMORY_FENCE;
         break;
      }

      case nir_intrinsic_begin_invocation_interlock:
         /* For beginInvocationInterlockARB(), we will generate a memory fence
          * but with a different opcode so that generator can pick SENDC
          * instead of SEND.
          */
         assert(s.stage == MESA_SHADER_FRAGMENT);
         ugm_fence = tgm_fence = true;
         slm_fence = urb_fence = false;
         opcode = ELK_SHADER_OPCODE_INTERLOCK;
         break;

      case nir_intrinsic_end_invocation_interlock:
         /* For endInvocationInterlockARB(), we need to insert a memory fence which
          * stalls in the shader until the memory transactions prior to that
          * fence are complete.  This ensures that the shader does not end before
          * any writes from its critical section have landed.  Otherwise, you can
          * end up with a case where the next invocation on that pixel properly
          * stalls for previous FS invocation on its pixel to complete but
          * doesn't actually wait for the dataport memory transactions from that
          * thread to land before submitting its own.
          */
         assert(s.stage == MESA_SHADER_FRAGMENT);
         ugm_fence = tgm_fence = true;
         slm_fence = urb_fence = false;
         opcode = ELK_SHADER_OPCODE_MEMORY_FENCE;
         break;

      default:
         unreachable("invalid intrinsic");
      }

      if (opcode == ELK_OPCODE_NOP)
         break;

      if (s.nir->info.shared_size > 0) {
         assert(gl_shader_stage_uses_workgroup(s.stage));
      } else {
         slm_fence = false;
      }

      /* If the workgroup fits in a single HW thread, the messages for SLM are
       * processed in-order and the shader itself is already synchronized so
       * the memory fence is not necessary.
       *
       * TODO: Check if applies for many HW threads sharing same Data Port.
       */
      if (!s.nir->info.workgroup_size_variable &&
          slm_fence && s.workgroup_size() <= s.dispatch_width)
         slm_fence = false;

      switch (s.stage) {
         case MESA_SHADER_TESS_CTRL:
            break;
         default:
            urb_fence = false;
            break;
      }

      unsigned fence_regs_count = 0;
      elk_fs_reg fence_regs[4] = {};

      const fs_builder ubld = bld.group(8, 0);

      /* Prior to Icelake, they're all lumped into a single cache except on
       * Ivy Bridge and Bay Trail where typed messages actually go through
       * the render cache.  There, we need both fences because we may
       * access storage images as either typed or untyped.
       */
      const bool render_fence = tgm_fence && devinfo->verx10 == 70;

      const bool commit_enable = render_fence ||
         instr->intrinsic == nir_intrinsic_end_invocation_interlock;

      if (tgm_fence || ugm_fence || slm_fence || urb_fence) {
         fence_regs[fence_regs_count++] =
            emit_fence(ubld, opcode, GFX7_SFID_DATAPORT_DATA_CACHE, 0,
                       commit_enable, 0 /* BTI */);
      }

      if (render_fence) {
         fence_regs[fence_regs_count++] =
            emit_fence(ubld, opcode, GFX6_SFID_DATAPORT_RENDER_CACHE, 0,
                       commit_enable, /* bti */ 0);
      }

      assert(fence_regs_count <= ARRAY_SIZE(fence_regs));

      /* There are four cases where we want to insert a stall:
       *
       *  1. If we're a nir_intrinsic_end_invocation_interlock.  This is
       *     required to ensure that the shader EOT doesn't happen until
       *     after the fence returns.  Otherwise, we might end up with the
       *     next shader invocation for that pixel not respecting our fence
       *     because it may happen on a different HW thread.
       *
       *  2. If we have multiple fences.  This is required to ensure that
       *     they all complete and nothing gets weirdly out-of-order.
       *
       *  3. If we have no fences.  In this case, we need at least a
       *     scheduling barrier to keep the compiler from moving things
       *     around in an invalid way.
       */
      if (instr->intrinsic == nir_intrinsic_end_invocation_interlock ||
          fence_regs_count != 1) {
         ubld.exec_all().group(1, 0).emit(
            ELK_FS_OPCODE_SCHEDULING_FENCE, ubld.null_reg_ud(),
            fence_regs, fence_regs_count);
      }

      break;
   }

   case nir_intrinsic_shader_clock: {
      /* We cannot do anything if there is an event, so ignore it for now */
      const elk_fs_reg shader_clock = get_timestamp(bld);
      const elk_fs_reg srcs[] = { component(shader_clock, 0),
                              component(shader_clock, 1) };
      bld.LOAD_PAYLOAD(dest, srcs, ARRAY_SIZE(srcs), 0);
      break;
   }

   case nir_intrinsic_load_reloc_const_intel: {
      uint32_t id = nir_intrinsic_param_idx(instr);

      /* Emit the reloc in the smallest SIMD size to limit register usage. */
      const fs_builder ubld = bld.exec_all().group(1, 0);
      elk_fs_reg small_dest = ubld.vgrf(dest.type);
      ubld.UNDEF(small_dest);
      ubld.exec_all().group(1, 0).emit(ELK_SHADER_OPCODE_MOV_RELOC_IMM,
                                       small_dest, elk_imm_ud(id));

      /* Copy propagation will get rid of this MOV. */
      bld.MOV(dest, component(small_dest, 0));
      break;
   }

   case nir_intrinsic_load_uniform: {
      /* Offsets are in bytes but they should always aligned to
       * the type size
       */
      unsigned base_offset = nir_intrinsic_base(instr);
      assert(base_offset % 4 == 0 || base_offset % type_sz(dest.type) == 0);

      elk_fs_reg src(UNIFORM, base_offset / 4, dest.type);

      if (nir_src_is_const(instr->src[0])) {
         unsigned load_offset = nir_src_as_uint(instr->src[0]);
         assert(load_offset % type_sz(dest.type) == 0);
         /* The base offset can only handle 32-bit units, so for 16-bit
          * data take the modulo of the offset with 4 bytes and add it to
          * the offset to read from within the source register.
          */
         src.offset = load_offset + base_offset % 4;

         for (unsigned j = 0; j < instr->num_components; j++) {
            bld.MOV(offset(dest, bld, j), offset(src, bld, j));
         }
      } else {
         elk_fs_reg indirect = retype(get_nir_src(ntb, instr->src[0]),
                                  ELK_REGISTER_TYPE_UD);

         /* We need to pass a size to the MOV_INDIRECT but we don't want it to
          * go past the end of the uniform.  In order to keep the n'th
          * component from running past, we subtract off the size of all but
          * one component of the vector.
          */
         assert(nir_intrinsic_range(instr) >=
                instr->num_components * type_sz(dest.type));
         unsigned read_size = nir_intrinsic_range(instr) -
            (instr->num_components - 1) * type_sz(dest.type);

         bool supports_64bit_indirects = devinfo->platform != INTEL_PLATFORM_CHV;

         if (type_sz(dest.type) != 8 || supports_64bit_indirects) {
            for (unsigned j = 0; j < instr->num_components; j++) {
               bld.emit(ELK_SHADER_OPCODE_MOV_INDIRECT,
                        offset(dest, bld, j), offset(src, bld, j),
                        indirect, elk_imm_ud(read_size));
            }
         } else {
            const unsigned num_mov_indirects =
               type_sz(dest.type) / type_sz(ELK_REGISTER_TYPE_UD);
            /* We read a little bit less per MOV INDIRECT, as they are now
             * 32-bits ones instead of 64-bit. Fix read_size then.
             */
            const unsigned read_size_32bit = read_size -
                (num_mov_indirects - 1) * type_sz(ELK_REGISTER_TYPE_UD);
            for (unsigned j = 0; j < instr->num_components; j++) {
               for (unsigned i = 0; i < num_mov_indirects; i++) {
                  bld.emit(ELK_SHADER_OPCODE_MOV_INDIRECT,
                           subscript(offset(dest, bld, j), ELK_REGISTER_TYPE_UD, i),
                           subscript(offset(src, bld, j), ELK_REGISTER_TYPE_UD, i),
                           indirect, elk_imm_ud(read_size_32bit));
               }
            }
         }
      }
      break;
   }

   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ubo_uniform_block_intel: {
      elk_fs_reg surface, surface_handle;

      if (get_nir_src_bindless(ntb, instr->src[0]))
         surface_handle = get_nir_buffer_intrinsic_index(ntb, bld, instr);
      else
         surface = get_nir_buffer_intrinsic_index(ntb, bld, instr);

      if (!nir_src_is_const(instr->src[1])) {
         if (instr->intrinsic == nir_intrinsic_load_ubo) {
            /* load_ubo with non-uniform offset */
            elk_fs_reg base_offset = retype(get_nir_src(ntb, instr->src[1]),
                                        ELK_REGISTER_TYPE_UD);

            const unsigned comps_per_load = type_sz(dest.type) == 8 ? 2 : 4;

            for (int i = 0; i < instr->num_components; i += comps_per_load) {
               const unsigned remaining = instr->num_components - i;
               s.VARYING_PULL_CONSTANT_LOAD(bld, offset(dest, bld, i),
                                            surface, surface_handle,
                                            base_offset,
                                            i * type_sz(dest.type),
                                            instr->def.bit_size / 8,
                                            MIN2(remaining, comps_per_load));
            }

            s.prog_data->has_ubo_pull = true;
         } else {
            /* load_ubo with uniform offset */
            const fs_builder ubld1 = bld.exec_all().group(1, 0);
            const fs_builder ubld8 = bld.exec_all().group(8, 0);
            const fs_builder ubld16 = bld.exec_all().group(16, 0);

            elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];

            srcs[SURFACE_LOGICAL_SRC_SURFACE]        = surface;
            srcs[SURFACE_LOGICAL_SRC_SURFACE_HANDLE] = surface_handle;

            const nir_src load_offset = instr->src[1];
            if (nir_src_is_const(load_offset)) {
               elk_fs_reg addr = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
               ubld8.MOV(addr, elk_imm_ud(nir_src_as_uint(load_offset)));
               srcs[SURFACE_LOGICAL_SRC_ADDRESS] = component(addr, 0);
            } else {
               srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
                  bld.emit_uniformize(get_nir_src(ntb, load_offset));
            }

            const unsigned total_dwords =
               ALIGN(instr->num_components, REG_SIZE * reg_unit(devinfo) / 4);
            unsigned loaded_dwords = 0;

            const elk_fs_reg packed_consts =
               ubld1.vgrf(ELK_REGISTER_TYPE_UD, total_dwords);

            while (loaded_dwords < total_dwords) {
               const unsigned block =
                  choose_oword_block_size_dwords(devinfo,
                                                 total_dwords - loaded_dwords);
               const unsigned block_bytes = block * 4;

               srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(block);

               const fs_builder &ubld = block <= 8 ? ubld8 : ubld16;
               ubld.emit(ELK_SHADER_OPCODE_UNALIGNED_OWORD_BLOCK_READ_LOGICAL,
                         retype(byte_offset(packed_consts, loaded_dwords * 4), ELK_REGISTER_TYPE_UD),
                         srcs, SURFACE_LOGICAL_NUM_SRCS)->size_written =
                  align(block_bytes, REG_SIZE * reg_unit(devinfo));

               loaded_dwords += block;

               ubld1.ADD(srcs[SURFACE_LOGICAL_SRC_ADDRESS],
                         srcs[SURFACE_LOGICAL_SRC_ADDRESS],
                         elk_imm_ud(block_bytes));
            }

            for (unsigned c = 0; c < instr->num_components; c++) {
               bld.MOV(retype(offset(dest, bld, c), ELK_REGISTER_TYPE_UD),
                       component(packed_consts, c));
            }

            s.prog_data->has_ubo_pull = true;
         }
      } else {
         /* Even if we are loading doubles, a pull constant load will load
          * a 32-bit vec4, so should only reserve vgrf space for that. If we
          * need to load a full dvec4 we will have to emit 2 loads. This is
          * similar to demote_pull_constants(), except that in that case we
          * see individual accesses to each component of the vector and then
          * we let CSE deal with duplicate loads. Here we see a vector access
          * and we have to split it if necessary.
          */
         const unsigned type_size = type_sz(dest.type);
         const unsigned load_offset = nir_src_as_uint(instr->src[1]);
         const unsigned ubo_block =
            elk_nir_ubo_surface_index_get_push_block(instr->src[0]);
         const unsigned offset_256b = load_offset / 32;
         const unsigned end_256b =
            DIV_ROUND_UP(load_offset + type_size * instr->num_components, 32);

         /* See if we've selected this as a push constant candidate */
         elk_fs_reg push_reg;
         for (int i = 0; i < 4; i++) {
            const struct elk_ubo_range *range = &s.prog_data->ubo_ranges[i];
            if (range->block == ubo_block &&
                offset_256b >= range->start &&
                end_256b <= range->start + range->length) {

               push_reg = elk_fs_reg(UNIFORM, UBO_START + i, dest.type);
               push_reg.offset = load_offset - 32 * range->start;
               break;
            }
         }

         if (push_reg.file != BAD_FILE) {
            for (unsigned i = 0; i < instr->num_components; i++) {
               bld.MOV(offset(dest, bld, i),
                       byte_offset(push_reg, i * type_size));
            }
            break;
         }

         s.prog_data->has_ubo_pull = true;

         const unsigned block_sz = 64; /* Fetch one cacheline at a time. */
         const fs_builder ubld = bld.exec_all().group(block_sz / 4, 0);

         for (unsigned c = 0; c < instr->num_components;) {
            const unsigned base = load_offset + c * type_size;
            /* Number of usable components in the next block-aligned load. */
            const unsigned count = MIN2(instr->num_components - c,
                                        (block_sz - base % block_sz) / type_size);

            const elk_fs_reg packed_consts = ubld.vgrf(ELK_REGISTER_TYPE_UD);
            elk_fs_reg srcs[PULL_UNIFORM_CONSTANT_SRCS];
            srcs[PULL_UNIFORM_CONSTANT_SRC_SURFACE]        = surface;
            srcs[PULL_UNIFORM_CONSTANT_SRC_SURFACE_HANDLE] = surface_handle;
            srcs[PULL_UNIFORM_CONSTANT_SRC_OFFSET]         = elk_imm_ud(base & ~(block_sz - 1));
            srcs[PULL_UNIFORM_CONSTANT_SRC_SIZE]           = elk_imm_ud(block_sz);

            ubld.emit(ELK_FS_OPCODE_UNIFORM_PULL_CONSTANT_LOAD, packed_consts,
                      srcs, PULL_UNIFORM_CONSTANT_SRCS);

            const elk_fs_reg consts =
               retype(byte_offset(packed_consts, base & (block_sz - 1)),
                      dest.type);

            for (unsigned d = 0; d < count; d++)
               bld.MOV(offset(dest, bld, c + d), component(consts, d));

            c += count;
         }
      }
      break;
   }

   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant: {
      assert(devinfo->ver >= 8);

      assert(instr->def.bit_size <= 32);
      assert(nir_intrinsic_align(instr) > 0);
      elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
      srcs[A64_LOGICAL_ADDRESS] = get_nir_src(ntb, instr->src[0]);
      srcs[A64_LOGICAL_SRC] = elk_fs_reg(); /* No source data */
      srcs[A64_LOGICAL_ENABLE_HELPERS] =
         elk_imm_ud(nir_intrinsic_access(instr) & ACCESS_INCLUDE_HELPERS);

      if (instr->def.bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         assert(instr->def.num_components <= 4);

         srcs[A64_LOGICAL_ARG] = elk_imm_ud(instr->num_components);

         elk_fs_inst *inst =
            bld.emit(ELK_SHADER_OPCODE_A64_UNTYPED_READ_LOGICAL, dest,
                     srcs, A64_LOGICAL_NUM_SRCS);
         inst->size_written = instr->num_components *
                              inst->dst.component_size(inst->exec_size);
      } else {
         const unsigned bit_size = instr->def.bit_size;
         assert(instr->def.num_components == 1);
         elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_UD);

         srcs[A64_LOGICAL_ARG] = elk_imm_ud(bit_size);

         bld.emit(ELK_SHADER_OPCODE_A64_BYTE_SCATTERED_READ_LOGICAL, tmp,
                  srcs, A64_LOGICAL_NUM_SRCS);
         bld.MOV(dest, subscript(tmp, dest.type, 0));
      }
      break;
   }

   case nir_intrinsic_store_global: {
      assert(devinfo->ver >= 8);

      assert(nir_src_bit_size(instr->src[0]) <= 32);
      assert(nir_intrinsic_write_mask(instr) ==
             (1u << instr->num_components) - 1);
      assert(nir_intrinsic_align(instr) > 0);

      elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
      srcs[A64_LOGICAL_ADDRESS] = get_nir_src(ntb, instr->src[1]);
      srcs[A64_LOGICAL_ENABLE_HELPERS] =
         elk_imm_ud(nir_intrinsic_access(instr) & ACCESS_INCLUDE_HELPERS);

      if (nir_src_bit_size(instr->src[0]) == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         assert(nir_src_num_components(instr->src[0]) <= 4);

         srcs[A64_LOGICAL_SRC] = get_nir_src(ntb, instr->src[0]); /* Data */
         srcs[A64_LOGICAL_ARG] = elk_imm_ud(instr->num_components);

         bld.emit(ELK_SHADER_OPCODE_A64_UNTYPED_WRITE_LOGICAL, elk_fs_reg(),
                  srcs, A64_LOGICAL_NUM_SRCS);
      } else {
         assert(nir_src_num_components(instr->src[0]) == 1);
         const unsigned bit_size = nir_src_bit_size(instr->src[0]);
         elk_reg_type data_type =
            elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);
         elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.MOV(tmp, retype(get_nir_src(ntb, instr->src[0]), data_type));

         srcs[A64_LOGICAL_SRC] = tmp;
         srcs[A64_LOGICAL_ARG] = elk_imm_ud(bit_size);

         bld.emit(ELK_SHADER_OPCODE_A64_BYTE_SCATTERED_WRITE_LOGICAL, elk_fs_reg(),
                  srcs, A64_LOGICAL_NUM_SRCS);
      }
      break;
   }

   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
      fs_nir_emit_global_atomic(ntb, bld, instr);
      break;

   case nir_intrinsic_load_global_const_block_intel: {
      assert(instr->def.bit_size == 32);
      assert(instr->num_components == 8 || instr->num_components == 16);

      const fs_builder ubld = bld.exec_all().group(instr->num_components, 0);
      elk_fs_reg load_val;

      bool is_pred_const = nir_src_is_const(instr->src[1]);
      if (is_pred_const && nir_src_as_uint(instr->src[1]) == 0) {
         /* In this case, we don't want the UBO load at all.  We really
          * shouldn't get here but it's possible.
          */
         load_val = elk_imm_ud(0);
      } else {
         /* The uniform process may stomp the flag so do this first */
         elk_fs_reg addr = bld.emit_uniformize(get_nir_src(ntb, instr->src[0]));

         load_val = ubld.vgrf(ELK_REGISTER_TYPE_UD);

         /* If the predicate is constant and we got here, then it's non-zero
          * and we don't need the predicate at all.
          */
         if (!is_pred_const) {
            /* Load the predicate */
            elk_fs_reg pred = bld.emit_uniformize(get_nir_src(ntb, instr->src[1]));
            elk_fs_inst *mov = ubld.MOV(bld.null_reg_d(), pred);
            mov->conditional_mod = ELK_CONDITIONAL_NZ;

            /* Stomp the destination with 0 if we're OOB */
            mov = ubld.MOV(load_val, elk_imm_ud(0));
            mov->predicate = ELK_PREDICATE_NORMAL;
            mov->predicate_inverse = true;
         }

         elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
         srcs[A64_LOGICAL_ADDRESS] = addr;
         srcs[A64_LOGICAL_SRC] = elk_fs_reg(); /* No source data */
         srcs[A64_LOGICAL_ARG] = elk_imm_ud(instr->num_components);
         /* This intrinsic loads memory from a uniform address, sometimes
          * shared across lanes. We never need to mask it.
          */
         srcs[A64_LOGICAL_ENABLE_HELPERS] = elk_imm_ud(0);

         elk_fs_inst *load = ubld.emit(ELK_SHADER_OPCODE_A64_OWORD_BLOCK_READ_LOGICAL,
                                   load_val, srcs, A64_LOGICAL_NUM_SRCS);
         if (!is_pred_const)
            load->predicate = ELK_PREDICATE_NORMAL;
      }

      /* From the HW perspective, we just did a single SIMD16 instruction
       * which loaded a dword in each SIMD channel.  From NIR's perspective,
       * this instruction returns a vec16.  Any users of this data in the
       * back-end will expect a vec16 per SIMD channel so we have to emit a
       * pile of MOVs to resolve this discrepancy.  Fortunately, copy-prop
       * will generally clean them up for us.
       */
      for (unsigned i = 0; i < instr->num_components; i++) {
         bld.MOV(retype(offset(dest, bld, i), ELK_REGISTER_TYPE_UD),
                 component(load_val, i));
      }
      break;
   }

   case nir_intrinsic_load_global_constant_uniform_block_intel: {
      const unsigned total_dwords = ALIGN(instr->num_components,
                                          REG_SIZE * reg_unit(devinfo) / 4);
      unsigned loaded_dwords = 0;

      const fs_builder ubld1 = bld.exec_all().group(1, 0);
      const fs_builder ubld8 = bld.exec_all().group(8, 0);
      const fs_builder ubld16 = bld.exec_all().group(16, 0);

      const elk_fs_reg packed_consts =
         ubld1.vgrf(ELK_REGISTER_TYPE_UD, total_dwords);
      elk_fs_reg address = bld.emit_uniformize(get_nir_src(ntb, instr->src[0]));

      while (loaded_dwords < total_dwords) {
         const unsigned block =
            choose_oword_block_size_dwords(devinfo,
                                           total_dwords - loaded_dwords);
         const unsigned block_bytes = block * 4;

         const fs_builder &ubld = block <= 8 ? ubld8 : ubld16;

         elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
         srcs[A64_LOGICAL_ADDRESS] = address;
         srcs[A64_LOGICAL_SRC] = elk_fs_reg(); /* No source data */
         srcs[A64_LOGICAL_ARG] = elk_imm_ud(block);
         srcs[A64_LOGICAL_ENABLE_HELPERS] = elk_imm_ud(0);
         ubld.emit(ELK_SHADER_OPCODE_A64_UNALIGNED_OWORD_BLOCK_READ_LOGICAL,
                   retype(byte_offset(packed_consts, loaded_dwords * 4), ELK_REGISTER_TYPE_UD),
                   srcs, A64_LOGICAL_NUM_SRCS)->size_written =
            align(block_bytes, REG_SIZE * reg_unit(devinfo));

         increment_a64_address(ubld1, address, block_bytes);
         loaded_dwords += block;
      }

      for (unsigned c = 0; c < instr->num_components; c++)
         bld.MOV(retype(offset(dest, bld, c), ELK_REGISTER_TYPE_UD),
                 component(packed_consts, c));

      break;
   }

   case nir_intrinsic_load_ssbo: {
      assert(devinfo->ver >= 7);

      const unsigned bit_size = instr->def.bit_size;
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[get_nir_src_bindless(ntb, instr->src[0]) ?
           SURFACE_LOGICAL_SRC_SURFACE_HANDLE :
           SURFACE_LOGICAL_SRC_SURFACE] =
         get_nir_buffer_intrinsic_index(ntb, bld, instr);
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = get_nir_src(ntb, instr->src[1]);
      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);

      /* Make dest unsigned because that's what the temporary will be */
      dest.type = elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);

      /* Read the vector */
      assert(bit_size <= 32);
      assert(nir_intrinsic_align(instr) > 0);
      if (bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         assert(instr->def.num_components <= 4);
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
         elk_fs_inst *inst =
            bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_READ_LOGICAL,
                     dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
         inst->size_written = instr->num_components * s.dispatch_width * 4;
      } else {
         assert(instr->def.num_components == 1);
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(bit_size);

         elk_fs_reg read_result = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.emit(ELK_SHADER_OPCODE_BYTE_SCATTERED_READ_LOGICAL,
                  read_result, srcs, SURFACE_LOGICAL_NUM_SRCS);
         bld.MOV(dest, subscript(read_result, dest.type, 0));
      }
      break;
   }

   case nir_intrinsic_store_ssbo: {
      assert(devinfo->ver >= 7);

      const unsigned bit_size = nir_src_bit_size(instr->src[0]);
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[get_nir_src_bindless(ntb, instr->src[1]) ?
           SURFACE_LOGICAL_SRC_SURFACE_HANDLE :
           SURFACE_LOGICAL_SRC_SURFACE] =
         get_nir_buffer_intrinsic_index(ntb, bld, instr);
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = get_nir_src(ntb, instr->src[2]);
      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(1);

      elk_fs_reg data = get_nir_src(ntb, instr->src[0]);
      data.type = elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);

      assert(bit_size <= 32);
      assert(nir_intrinsic_write_mask(instr) ==
             (1u << instr->num_components) - 1);
      assert(nir_intrinsic_align(instr) > 0);
      if (bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         assert(nir_src_num_components(instr->src[0]) <= 4);
         srcs[SURFACE_LOGICAL_SRC_DATA] = data;
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(instr->num_components);
         bld.emit(ELK_SHADER_OPCODE_UNTYPED_SURFACE_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      } else {
         assert(nir_src_num_components(instr->src[0]) == 1);
         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(bit_size);

         srcs[SURFACE_LOGICAL_SRC_DATA] = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.MOV(srcs[SURFACE_LOGICAL_SRC_DATA], data);

         bld.emit(ELK_SHADER_OPCODE_BYTE_SCATTERED_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      }
      break;
   }

   case nir_intrinsic_load_ssbo_uniform_block_intel:
   case nir_intrinsic_load_shared_uniform_block_intel: {
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];

      const bool is_ssbo =
         instr->intrinsic == nir_intrinsic_load_ssbo_uniform_block_intel;
      if (is_ssbo) {
         srcs[get_nir_src_bindless(ntb, instr->src[0]) ?
              SURFACE_LOGICAL_SRC_SURFACE_HANDLE :
              SURFACE_LOGICAL_SRC_SURFACE] =
            get_nir_buffer_intrinsic_index(ntb, bld, instr);
      } else {
         srcs[SURFACE_LOGICAL_SRC_SURFACE] = elk_fs_reg(elk_imm_ud(GFX7_BTI_SLM));
      }

      const unsigned total_dwords = ALIGN(instr->num_components,
                                          REG_SIZE * reg_unit(devinfo) / 4);
      unsigned loaded_dwords = 0;

      const fs_builder ubld1 = bld.exec_all().group(1, 0);
      const fs_builder ubld8 = bld.exec_all().group(8, 0);
      const fs_builder ubld16 = bld.exec_all().group(16, 0);

      const elk_fs_reg packed_consts =
         ubld1.vgrf(ELK_REGISTER_TYPE_UD, total_dwords);

      const nir_src load_offset = is_ssbo ? instr->src[1] : instr->src[0];
      if (nir_src_is_const(load_offset)) {
         elk_fs_reg addr = ubld8.vgrf(ELK_REGISTER_TYPE_UD);
         ubld8.MOV(addr, elk_imm_ud(nir_src_as_uint(load_offset)));
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] = component(addr, 0);
      } else {
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
            bld.emit_uniformize(get_nir_src(ntb, load_offset));
      }

      while (loaded_dwords < total_dwords) {
         const unsigned block =
            choose_oword_block_size_dwords(devinfo,
                                           total_dwords - loaded_dwords);
         const unsigned block_bytes = block * 4;

         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(block);

         const fs_builder &ubld = block <= 8 ? ubld8 : ubld16;
         ubld.emit(ELK_SHADER_OPCODE_UNALIGNED_OWORD_BLOCK_READ_LOGICAL,
                   retype(byte_offset(packed_consts, loaded_dwords * 4), ELK_REGISTER_TYPE_UD),
                   srcs, SURFACE_LOGICAL_NUM_SRCS)->size_written =
            align(block_bytes, REG_SIZE * reg_unit(devinfo));

         loaded_dwords += block;

         ubld1.ADD(srcs[SURFACE_LOGICAL_SRC_ADDRESS],
                   srcs[SURFACE_LOGICAL_SRC_ADDRESS],
                   elk_imm_ud(block_bytes));
      }

      for (unsigned c = 0; c < instr->num_components; c++)
         bld.MOV(retype(offset(dest, bld, c), ELK_REGISTER_TYPE_UD),
                 component(packed_consts, c));

      break;
   }

   case nir_intrinsic_store_output: {
      assert(nir_src_bit_size(instr->src[0]) == 32);
      elk_fs_reg src = get_nir_src(ntb, instr->src[0]);

      unsigned store_offset = nir_src_as_uint(instr->src[1]);
      unsigned num_components = instr->num_components;
      unsigned first_component = nir_intrinsic_component(instr);

      elk_fs_reg new_dest = retype(offset(s.outputs[instr->const_index[0]], bld,
                                      4 * store_offset), src.type);
      for (unsigned j = 0; j < num_components; j++) {
         bld.MOV(offset(new_dest, bld, j + first_component),
                 offset(src, bld, j));
      }
      break;
   }

   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
      fs_nir_emit_surface_atomic(ntb, bld, instr,
                                 get_nir_buffer_intrinsic_index(ntb, bld, instr),
                                 get_nir_src_bindless(ntb, instr->src[0]));
      break;

   case nir_intrinsic_get_ssbo_size: {
      assert(nir_src_num_components(instr->src[0]) == 1);

      /* A resinfo's sampler message is used to get the buffer size.  The
       * SIMD8's writeback message consists of four registers and SIMD16's
       * writeback message consists of 8 destination registers (two per each
       * component).  Because we are only interested on the first channel of
       * the first returned component, where resinfo returns the buffer size
       * for SURFTYPE_BUFFER, we can just use the SIMD8 variant regardless of
       * the dispatch width.
       */
      const fs_builder ubld = bld.exec_all().group(8 * reg_unit(devinfo), 0);
      elk_fs_reg src_payload = ubld.vgrf(ELK_REGISTER_TYPE_UD);
      elk_fs_reg ret_payload = ubld.vgrf(ELK_REGISTER_TYPE_UD, 4);

      /* Set LOD = 0 */
      ubld.MOV(src_payload, elk_imm_d(0));

      elk_fs_reg srcs[GET_BUFFER_SIZE_SRCS];
      srcs[get_nir_src_bindless(ntb, instr->src[0]) ?
           GET_BUFFER_SIZE_SRC_SURFACE_HANDLE :
           GET_BUFFER_SIZE_SRC_SURFACE] =
         get_nir_buffer_intrinsic_index(ntb, bld, instr);
      srcs[GET_BUFFER_SIZE_SRC_LOD] = src_payload;
      elk_fs_inst *inst = ubld.emit(ELK_SHADER_OPCODE_GET_BUFFER_SIZE, ret_payload,
                                srcs, GET_BUFFER_SIZE_SRCS);
      inst->header_size = 0;
      inst->mlen = reg_unit(devinfo);
      inst->size_written = 4 * REG_SIZE * reg_unit(devinfo);

      /* SKL PRM, vol07, 3D Media GPGPU Engine, Bounds Checking and Faulting:
       *
       * "Out-of-bounds checking is always performed at a DWord granularity. If
       * any part of the DWord is out-of-bounds then the whole DWord is
       * considered out-of-bounds."
       *
       * This implies that types with size smaller than 4-bytes need to be
       * padded if they don't complete the last dword of the buffer. But as we
       * need to maintain the original size we need to reverse the padding
       * calculation to return the correct size to know the number of elements
       * of an unsized array. As we stored in the last two bits of the surface
       * size the needed padding for the buffer, we calculate here the
       * original buffer_size reversing the surface_size calculation:
       *
       * surface_size = isl_align(buffer_size, 4) +
       *                (isl_align(buffer_size) - buffer_size)
       *
       * buffer_size = surface_size & ~3 - surface_size & 3
       */

      elk_fs_reg size_aligned4 = ubld.vgrf(ELK_REGISTER_TYPE_UD);
      elk_fs_reg size_padding = ubld.vgrf(ELK_REGISTER_TYPE_UD);
      elk_fs_reg buffer_size = ubld.vgrf(ELK_REGISTER_TYPE_UD);

      ubld.AND(size_padding, ret_payload, elk_imm_ud(3));
      ubld.AND(size_aligned4, ret_payload, elk_imm_ud(~3));
      ubld.ADD(buffer_size, size_aligned4, negate(size_padding));

      bld.MOV(retype(dest, ret_payload.type), component(buffer_size, 0));
      break;
   }

   case nir_intrinsic_load_scratch: {
      assert(devinfo->ver >= 7);

      assert(instr->def.num_components == 1);
      const unsigned bit_size = instr->def.bit_size;
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];

      if (devinfo->ver >= 8) {
         srcs[SURFACE_LOGICAL_SRC_SURFACE] =
            elk_imm_ud(GFX8_BTI_STATELESS_NON_COHERENT);
      } else {
         srcs[SURFACE_LOGICAL_SRC_SURFACE] = elk_imm_ud(ELK_BTI_STATELESS);
      }

      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(bit_size);
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);
      const elk_fs_reg nir_addr = get_nir_src(ntb, instr->src[0]);

      /* Make dest unsigned because that's what the temporary will be */
      dest.type = elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);

      /* Read the vector */
      assert(instr->def.num_components == 1);
      assert(bit_size <= 32);
      assert(nir_intrinsic_align(instr) > 0);
      if (bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         /* The offset for a DWORD scattered message is in dwords. */
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
            swizzle_nir_scratch_addr(ntb, bld, nir_addr, true);

         bld.emit(ELK_SHADER_OPCODE_DWORD_SCATTERED_READ_LOGICAL,
                  dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
      } else {
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
            swizzle_nir_scratch_addr(ntb, bld, nir_addr, false);

         elk_fs_reg read_result = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.emit(ELK_SHADER_OPCODE_BYTE_SCATTERED_READ_LOGICAL,
                  read_result, srcs, SURFACE_LOGICAL_NUM_SRCS);
         bld.MOV(dest, read_result);
      }

      s.shader_stats.fill_count += DIV_ROUND_UP(s.dispatch_width, 16);
      break;
   }

   case nir_intrinsic_store_scratch: {
      assert(devinfo->ver >= 7);

      assert(nir_src_num_components(instr->src[0]) == 1);
      const unsigned bit_size = nir_src_bit_size(instr->src[0]);
      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];

      if (devinfo->ver >= 8) {
         srcs[SURFACE_LOGICAL_SRC_SURFACE] =
            elk_imm_ud(GFX8_BTI_STATELESS_NON_COHERENT);
      } else {
         srcs[SURFACE_LOGICAL_SRC_SURFACE] = elk_imm_ud(ELK_BTI_STATELESS);
      }

      srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
      srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(bit_size);
      /**
       * While this instruction has side-effects, it should not be predicated
       * on sample mask, because otherwise fs helper invocations would
       * load undefined values from scratch memory. And scratch memory
       * load-stores are produced from operations without side-effects, thus
       * they should not have different behaviour in the helper invocations.
       */
      srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(0);
      const elk_fs_reg nir_addr = get_nir_src(ntb, instr->src[1]);

      elk_fs_reg data = get_nir_src(ntb, instr->src[0]);
      data.type = elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_UD);

      assert(nir_src_num_components(instr->src[0]) == 1);
      assert(bit_size <= 32);
      assert(nir_intrinsic_write_mask(instr) == 1);
      assert(nir_intrinsic_align(instr) > 0);
      if (bit_size == 32 &&
          nir_intrinsic_align(instr) >= 4) {
         srcs[SURFACE_LOGICAL_SRC_DATA] = data;

         /* The offset for a DWORD scattered message is in dwords. */
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
            swizzle_nir_scratch_addr(ntb, bld, nir_addr, true);

         bld.emit(ELK_SHADER_OPCODE_DWORD_SCATTERED_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      } else {
         srcs[SURFACE_LOGICAL_SRC_DATA] = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.MOV(srcs[SURFACE_LOGICAL_SRC_DATA], data);

         srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
            swizzle_nir_scratch_addr(ntb, bld, nir_addr, false);

         bld.emit(ELK_SHADER_OPCODE_BYTE_SCATTERED_WRITE_LOGICAL,
                  elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);
      }
      s.shader_stats.spill_count += DIV_ROUND_UP(s.dispatch_width, 16);
      break;
   }

   case nir_intrinsic_load_subgroup_size:
      /* This should only happen for fragment shaders because every other case
       * is lowered in NIR so we can optimize on it.
       */
      assert(s.stage == MESA_SHADER_FRAGMENT);
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_D), elk_imm_d(s.dispatch_width));
      break;

   case nir_intrinsic_load_subgroup_invocation:
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_D),
              ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION]);
      break;

   case nir_intrinsic_load_subgroup_eq_mask:
   case nir_intrinsic_load_subgroup_ge_mask:
   case nir_intrinsic_load_subgroup_gt_mask:
   case nir_intrinsic_load_subgroup_le_mask:
   case nir_intrinsic_load_subgroup_lt_mask:
      unreachable("not reached");

   case nir_intrinsic_vote_any: {
      const fs_builder ubld1 = bld.exec_all().group(1, 0);

      /* The any/all predicates do not consider channel enables. To prevent
       * dead channels from affecting the result, we initialize the flag with
       * with the identity value for the logical operation.
       */
      if (s.dispatch_width == 32) {
         /* For SIMD32, we use a UD type so we fill both f0.0 and f0.1. */
         ubld1.MOV(retype(elk_flag_reg(0, 0), ELK_REGISTER_TYPE_UD),
                   elk_imm_ud(0));
      } else {
         ubld1.MOV(elk_flag_reg(0, 0), elk_imm_uw(0));
      }
      bld.CMP(bld.null_reg_d(), get_nir_src(ntb, instr->src[0]), elk_imm_d(0), ELK_CONDITIONAL_NZ);

      /* For some reason, the any/all predicates don't work properly with
       * SIMD32.  In particular, it appears that a SEL with a QtrCtrl of 2H
       * doesn't read the correct subset of the flag register and you end up
       * getting garbage in the second half.  Work around this by using a pair
       * of 1-wide MOVs and scattering the result.
       */
      const fs_builder ubld = ubld1;
      elk_fs_reg res1 = ubld.vgrf(ELK_REGISTER_TYPE_D);
      ubld.MOV(res1, elk_imm_d(0));
      set_predicate(s.dispatch_width == 8  ? ELK_PREDICATE_ALIGN1_ANY8H :
                    s.dispatch_width == 16 ? ELK_PREDICATE_ALIGN1_ANY16H :
                                             ELK_PREDICATE_ALIGN1_ANY32H,
                    ubld.MOV(res1, elk_imm_d(-1)));

      bld.MOV(retype(dest, ELK_REGISTER_TYPE_D), component(res1, 0));
      break;
   }
   case nir_intrinsic_vote_all: {
      const fs_builder ubld1 = bld.exec_all().group(1, 0);

      /* The any/all predicates do not consider channel enables. To prevent
       * dead channels from affecting the result, we initialize the flag with
       * with the identity value for the logical operation.
       */
      if (s.dispatch_width == 32) {
         /* For SIMD32, we use a UD type so we fill both f0.0 and f0.1. */
         ubld1.MOV(retype(elk_flag_reg(0, 0), ELK_REGISTER_TYPE_UD),
                   elk_imm_ud(0xffffffff));
      } else {
         ubld1.MOV(elk_flag_reg(0, 0), elk_imm_uw(0xffff));
      }
      bld.CMP(bld.null_reg_d(), get_nir_src(ntb, instr->src[0]), elk_imm_d(0), ELK_CONDITIONAL_NZ);

      /* For some reason, the any/all predicates don't work properly with
       * SIMD32.  In particular, it appears that a SEL with a QtrCtrl of 2H
       * doesn't read the correct subset of the flag register and you end up
       * getting garbage in the second half.  Work around this by using a pair
       * of 1-wide MOVs and scattering the result.
       */
      const fs_builder ubld = ubld1;
      elk_fs_reg res1 = ubld.vgrf(ELK_REGISTER_TYPE_D);
      ubld.MOV(res1, elk_imm_d(0));
      set_predicate(s.dispatch_width == 8  ? ELK_PREDICATE_ALIGN1_ALL8H :
                    s.dispatch_width == 16 ? ELK_PREDICATE_ALIGN1_ALL16H :
                                             ELK_PREDICATE_ALIGN1_ALL32H,
                    ubld.MOV(res1, elk_imm_d(-1)));

      bld.MOV(retype(dest, ELK_REGISTER_TYPE_D), component(res1, 0));
      break;
   }
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq: {
      elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      if (instr->intrinsic == nir_intrinsic_vote_feq) {
         const unsigned bit_size = nir_src_bit_size(instr->src[0]);
         value.type = bit_size == 8 ? ELK_REGISTER_TYPE_B :
            elk_reg_type_from_bit_size(bit_size, ELK_REGISTER_TYPE_F);
      }

      elk_fs_reg uniformized = bld.emit_uniformize(value);
      const fs_builder ubld1 = bld.exec_all().group(1, 0);

      /* The any/all predicates do not consider channel enables. To prevent
       * dead channels from affecting the result, we initialize the flag with
       * with the identity value for the logical operation.
       */
      if (s.dispatch_width == 32) {
         /* For SIMD32, we use a UD type so we fill both f0.0 and f0.1. */
         ubld1.MOV(retype(elk_flag_reg(0, 0), ELK_REGISTER_TYPE_UD),
                         elk_imm_ud(0xffffffff));
      } else {
         ubld1.MOV(elk_flag_reg(0, 0), elk_imm_uw(0xffff));
      }
      bld.CMP(bld.null_reg_d(), value, uniformized, ELK_CONDITIONAL_Z);

      /* For some reason, the any/all predicates don't work properly with
       * SIMD32.  In particular, it appears that a SEL with a QtrCtrl of 2H
       * doesn't read the correct subset of the flag register and you end up
       * getting garbage in the second half.  Work around this by using a pair
       * of 1-wide MOVs and scattering the result.
       */
      const fs_builder ubld = ubld1;
      elk_fs_reg res1 = ubld.vgrf(ELK_REGISTER_TYPE_D);
      ubld.MOV(res1, elk_imm_d(0));
      set_predicate(s.dispatch_width == 8  ? ELK_PREDICATE_ALIGN1_ALL8H :
                    s.dispatch_width == 16 ? ELK_PREDICATE_ALIGN1_ALL16H :
                                             ELK_PREDICATE_ALIGN1_ALL32H,
                    ubld.MOV(res1, elk_imm_d(-1)));

      bld.MOV(retype(dest, ELK_REGISTER_TYPE_D), component(res1, 0));
      break;
   }

   case nir_intrinsic_ballot: {
      const elk_fs_reg value = retype(get_nir_src(ntb, instr->src[0]),
                                  ELK_REGISTER_TYPE_UD);
      struct elk_reg flag = elk_flag_reg(0, 0);
      /* FIXME: For SIMD32 programs, this causes us to stomp on f0.1 as well
       * as f0.0.  This is a problem for fragment programs as we currently use
       * f0.1 for discards.  Fortunately, we don't support SIMD32 fragment
       * programs yet so this isn't a problem.  When we do, something will
       * have to change.
       */
      if (s.dispatch_width == 32)
         flag.type = ELK_REGISTER_TYPE_UD;

      bld.exec_all().group(1, 0).MOV(flag, elk_imm_ud(0u));
      bld.CMP(bld.null_reg_ud(), value, elk_imm_ud(0u), ELK_CONDITIONAL_NZ);

      if (instr->def.bit_size > 32) {
         dest.type = ELK_REGISTER_TYPE_UQ;
      } else {
         dest.type = ELK_REGISTER_TYPE_UD;
      }
      bld.MOV(dest, flag);
      break;
   }

   case nir_intrinsic_read_invocation: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      const elk_fs_reg invocation = get_nir_src(ntb, instr->src[1]);

      elk_fs_reg tmp = bld.vgrf(value.type);

      /* When for some reason the subgroup_size picked by NIR is larger than
       * the dispatch size picked by the backend (this could happen in RT,
       * FS), bound the invocation to the dispatch size.
       */
      elk_fs_reg bound_invocation;
      if (s.api_subgroup_size == 0 ||
          bld.dispatch_width() < s.api_subgroup_size) {
         bound_invocation = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.AND(bound_invocation, invocation, elk_imm_ud(s.dispatch_width - 1));
      } else {
         bound_invocation = invocation;
      }
      bld.exec_all().emit(ELK_SHADER_OPCODE_BROADCAST, tmp, value,
                          bld.emit_uniformize(bound_invocation));

      bld.MOV(retype(dest, value.type), elk_fs_reg(component(tmp, 0)));
      break;
   }

   case nir_intrinsic_read_first_invocation: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      bld.MOV(retype(dest, value.type), bld.emit_uniformize(value));
      break;
   }

   case nir_intrinsic_shuffle: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      const elk_fs_reg index = get_nir_src(ntb, instr->src[1]);

      bld.emit(ELK_SHADER_OPCODE_SHUFFLE, retype(dest, value.type), value, index);
      break;
   }

   case nir_intrinsic_first_invocation: {
      elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.exec_all().emit(ELK_SHADER_OPCODE_FIND_LIVE_CHANNEL, tmp);
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_UD),
              elk_fs_reg(component(tmp, 0)));
      break;
   }

   case nir_intrinsic_last_invocation: {
      elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.exec_all().emit(ELK_SHADER_OPCODE_FIND_LAST_LIVE_CHANNEL, tmp);
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_UD),
              elk_fs_reg(component(tmp, 0)));
      break;
   }

   case nir_intrinsic_quad_broadcast: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      const unsigned index = nir_src_as_uint(instr->src[1]);

      bld.emit(ELK_SHADER_OPCODE_CLUSTER_BROADCAST, retype(dest, value.type),
               value, elk_imm_ud(index), elk_imm_ud(4));
      break;
   }

   case nir_intrinsic_quad_swap_horizontal: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      const elk_fs_reg tmp = bld.vgrf(value.type);
      if (devinfo->ver <= 7) {
         /* The hardware doesn't seem to support these crazy regions with
          * compressed instructions on gfx7 and earlier so we fall back to
          * using quad swizzles.  Fortunately, we don't support 64-bit
          * anything in Vulkan on gfx7.
          */
         assert(nir_src_bit_size(instr->src[0]) == 32);
         const fs_builder ubld = bld.exec_all();
         ubld.emit(ELK_SHADER_OPCODE_QUAD_SWIZZLE, tmp, value,
                   elk_imm_ud(ELK_SWIZZLE4(1,0,3,2)));
         bld.MOV(retype(dest, value.type), tmp);
      } else {
         const fs_builder ubld = bld.exec_all().group(s.dispatch_width / 2, 0);

         const elk_fs_reg src_left = horiz_stride(value, 2);
         const elk_fs_reg src_right = horiz_stride(horiz_offset(value, 1), 2);
         const elk_fs_reg tmp_left = horiz_stride(tmp, 2);
         const elk_fs_reg tmp_right = horiz_stride(horiz_offset(tmp, 1), 2);

         ubld.MOV(tmp_left, src_right);
         ubld.MOV(tmp_right, src_left);

      }
      bld.MOV(retype(dest, value.type), tmp);
      break;
   }

   case nir_intrinsic_quad_swap_vertical: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      if (nir_src_bit_size(instr->src[0]) == 32) {
         /* For 32-bit, we can use a SIMD4x2 instruction to do this easily */
         const elk_fs_reg tmp = bld.vgrf(value.type);
         const fs_builder ubld = bld.exec_all();
         ubld.emit(ELK_SHADER_OPCODE_QUAD_SWIZZLE, tmp, value,
                   elk_imm_ud(ELK_SWIZZLE4(2,3,0,1)));
         bld.MOV(retype(dest, value.type), tmp);
      } else {
         /* For larger data types, we have to either emit dispatch_width many
          * MOVs or else fall back to doing indirects.
          */
         elk_fs_reg idx = bld.vgrf(ELK_REGISTER_TYPE_W);
         bld.XOR(idx, ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION],
                      elk_imm_w(0x2));
         bld.emit(ELK_SHADER_OPCODE_SHUFFLE, retype(dest, value.type), value, idx);
      }
      break;
   }

   case nir_intrinsic_quad_swap_diagonal: {
      const elk_fs_reg value = get_nir_src(ntb, instr->src[0]);
      if (nir_src_bit_size(instr->src[0]) == 32) {
         /* For 32-bit, we can use a SIMD4x2 instruction to do this easily */
         const elk_fs_reg tmp = bld.vgrf(value.type);
         const fs_builder ubld = bld.exec_all();
         ubld.emit(ELK_SHADER_OPCODE_QUAD_SWIZZLE, tmp, value,
                   elk_imm_ud(ELK_SWIZZLE4(3,2,1,0)));
         bld.MOV(retype(dest, value.type), tmp);
      } else {
         /* For larger data types, we have to either emit dispatch_width many
          * MOVs or else fall back to doing indirects.
          */
         elk_fs_reg idx = bld.vgrf(ELK_REGISTER_TYPE_W);
         bld.XOR(idx, ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION],
                      elk_imm_w(0x3));
         bld.emit(ELK_SHADER_OPCODE_SHUFFLE, retype(dest, value.type), value, idx);
      }
      break;
   }

   case nir_intrinsic_reduce: {
      elk_fs_reg src = get_nir_src(ntb, instr->src[0]);
      nir_op redop = (nir_op)nir_intrinsic_reduction_op(instr);
      unsigned cluster_size = nir_intrinsic_cluster_size(instr);
      if (cluster_size == 0 || cluster_size > s.dispatch_width)
         cluster_size = s.dispatch_width;

      /* Figure out the source type */
      src.type = elk_type_for_nir_type(devinfo,
         (nir_alu_type)(nir_op_infos[redop].input_types[0] |
                        nir_src_bit_size(instr->src[0])));

      elk_fs_reg identity = elk_nir_reduction_op_identity(bld, redop, src.type);
      elk_opcode elk_op = elk_op_for_nir_reduction_op(redop);
      elk_conditional_mod cond_mod = elk_cond_mod_for_nir_reduction_op(redop);

      /* Set up a register for all of our scratching around and initialize it
       * to reduction operation's identity value.
       */
      elk_fs_reg scan = bld.vgrf(src.type);
      bld.exec_all().emit(ELK_SHADER_OPCODE_SEL_EXEC, scan, src, identity);

      bld.emit_scan(elk_op, scan, cluster_size, cond_mod);

      dest.type = src.type;
      if (cluster_size * type_sz(src.type) >= REG_SIZE * 2) {
         /* In this case, CLUSTER_BROADCAST instruction isn't needed because
          * the distance between clusters is at least 2 GRFs.  In this case,
          * we don't need the weird striding of the CLUSTER_BROADCAST
          * instruction and can just do regular MOVs.
          */
         assert((cluster_size * type_sz(src.type)) % (REG_SIZE * 2) == 0);
         const unsigned groups =
            (s.dispatch_width * type_sz(src.type)) / (REG_SIZE * 2);
         const unsigned group_size = s.dispatch_width / groups;
         for (unsigned i = 0; i < groups; i++) {
            const unsigned cluster = (i * group_size) / cluster_size;
            const unsigned comp = cluster * cluster_size + (cluster_size - 1);
            bld.group(group_size, i).MOV(horiz_offset(dest, i * group_size),
                                         component(scan, comp));
         }
      } else {
         bld.emit(ELK_SHADER_OPCODE_CLUSTER_BROADCAST, dest, scan,
                  elk_imm_ud(cluster_size - 1), elk_imm_ud(cluster_size));
      }
      break;
   }

   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan: {
      elk_fs_reg src = get_nir_src(ntb, instr->src[0]);
      nir_op redop = (nir_op)nir_intrinsic_reduction_op(instr);

      /* Figure out the source type */
      src.type = elk_type_for_nir_type(devinfo,
         (nir_alu_type)(nir_op_infos[redop].input_types[0] |
                        nir_src_bit_size(instr->src[0])));

      elk_fs_reg identity = elk_nir_reduction_op_identity(bld, redop, src.type);
      elk_opcode elk_op = elk_op_for_nir_reduction_op(redop);
      elk_conditional_mod cond_mod = elk_cond_mod_for_nir_reduction_op(redop);

      /* Set up a register for all of our scratching around and initialize it
       * to reduction operation's identity value.
       */
      elk_fs_reg scan = bld.vgrf(src.type);
      const fs_builder allbld = bld.exec_all();
      allbld.emit(ELK_SHADER_OPCODE_SEL_EXEC, scan, src, identity);

      if (instr->intrinsic == nir_intrinsic_exclusive_scan) {
         /* Exclusive scan is a bit harder because we have to do an annoying
          * shift of the contents before we can begin.  To make things worse,
          * we can't do this with a normal stride; we have to use indirects.
          */
         elk_fs_reg shifted = bld.vgrf(src.type);
         elk_fs_reg idx = bld.vgrf(ELK_REGISTER_TYPE_W);
         allbld.ADD(idx, ntb.system_values[SYSTEM_VALUE_SUBGROUP_INVOCATION],
                         elk_imm_w(-1));
         allbld.emit(ELK_SHADER_OPCODE_SHUFFLE, shifted, scan, idx);
         allbld.group(1, 0).MOV(component(shifted, 0), identity);
         scan = shifted;
      }

      bld.emit_scan(elk_op, scan, s.dispatch_width, cond_mod);

      bld.MOV(retype(dest, src.type), scan);
      break;
   }

   case nir_intrinsic_load_global_block_intel: {
      assert(instr->def.bit_size == 32);

      elk_fs_reg address = bld.emit_uniformize(get_nir_src(ntb, instr->src[0]));

      const fs_builder ubld1 = bld.exec_all().group(1, 0);
      const fs_builder ubld8 = bld.exec_all().group(8, 0);
      const fs_builder ubld16 = bld.exec_all().group(16, 0);

      const unsigned total = instr->num_components * s.dispatch_width;
      unsigned loaded = 0;

      while (loaded < total) {
         const unsigned block =
            choose_oword_block_size_dwords(devinfo, total - loaded);
         const unsigned block_bytes = block * 4;

         const fs_builder &ubld = block == 8 ? ubld8 : ubld16;

         elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
         srcs[A64_LOGICAL_ADDRESS] = address;
         srcs[A64_LOGICAL_SRC] = elk_fs_reg(); /* No source data */
         srcs[A64_LOGICAL_ARG] = elk_imm_ud(block);
         srcs[A64_LOGICAL_ENABLE_HELPERS] = elk_imm_ud(1);
         ubld.emit(ELK_SHADER_OPCODE_A64_UNALIGNED_OWORD_BLOCK_READ_LOGICAL,
                   retype(byte_offset(dest, loaded * 4), ELK_REGISTER_TYPE_UD),
                   srcs, A64_LOGICAL_NUM_SRCS)->size_written = block_bytes;

         increment_a64_address(ubld1, address, block_bytes);
         loaded += block;
      }

      assert(loaded == total);
      break;
   }

   case nir_intrinsic_store_global_block_intel: {
      assert(nir_src_bit_size(instr->src[0]) == 32);

      elk_fs_reg address = bld.emit_uniformize(get_nir_src(ntb, instr->src[1]));
      elk_fs_reg src = get_nir_src(ntb, instr->src[0]);

      const fs_builder ubld1 = bld.exec_all().group(1, 0);
      const fs_builder ubld8 = bld.exec_all().group(8, 0);
      const fs_builder ubld16 = bld.exec_all().group(16, 0);

      const unsigned total = instr->num_components * s.dispatch_width;
      unsigned written = 0;

      while (written < total) {
         const unsigned block =
            choose_oword_block_size_dwords(devinfo, total - written);

         elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
         srcs[A64_LOGICAL_ADDRESS] = address;
         srcs[A64_LOGICAL_SRC] = retype(byte_offset(src, written * 4),
                                        ELK_REGISTER_TYPE_UD);
         srcs[A64_LOGICAL_ARG] = elk_imm_ud(block);
         srcs[A64_LOGICAL_ENABLE_HELPERS] = elk_imm_ud(0);

         const fs_builder &ubld = block == 8 ? ubld8 : ubld16;
         ubld.emit(ELK_SHADER_OPCODE_A64_OWORD_BLOCK_WRITE_LOGICAL, elk_fs_reg(),
                   srcs, A64_LOGICAL_NUM_SRCS);

         const unsigned block_bytes = block * 4;
         increment_a64_address(ubld1, address, block_bytes);
         written += block;
      }

      assert(written == total);
      break;
   }

   case nir_intrinsic_load_shared_block_intel:
   case nir_intrinsic_load_ssbo_block_intel: {
      assert(instr->def.bit_size == 32);

      const bool is_ssbo =
         instr->intrinsic == nir_intrinsic_load_ssbo_block_intel;
      elk_fs_reg address = bld.emit_uniformize(get_nir_src(ntb, instr->src[is_ssbo ? 1 : 0]));

      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] = is_ssbo ?
         get_nir_buffer_intrinsic_index(ntb, bld, instr) :
         elk_fs_reg(elk_imm_ud(GFX7_BTI_SLM));
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = address;

      const fs_builder ubld1 = bld.exec_all().group(1, 0);
      const fs_builder ubld8 = bld.exec_all().group(8, 0);
      const fs_builder ubld16 = bld.exec_all().group(16, 0);

      const unsigned total = instr->num_components * s.dispatch_width;
      unsigned loaded = 0;

      while (loaded < total) {
         const unsigned block =
            choose_oword_block_size_dwords(devinfo, total - loaded);
         const unsigned block_bytes = block * 4;

         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(block);

         const fs_builder &ubld = block == 8 ? ubld8 : ubld16;
         ubld.emit(ELK_SHADER_OPCODE_UNALIGNED_OWORD_BLOCK_READ_LOGICAL,
                   retype(byte_offset(dest, loaded * 4), ELK_REGISTER_TYPE_UD),
                   srcs, SURFACE_LOGICAL_NUM_SRCS)->size_written = block_bytes;

         ubld1.ADD(address, address, elk_imm_ud(block_bytes));
         loaded += block;
      }

      assert(loaded == total);
      break;
   }

   case nir_intrinsic_store_shared_block_intel:
   case nir_intrinsic_store_ssbo_block_intel: {
      assert(nir_src_bit_size(instr->src[0]) == 32);

      const bool is_ssbo =
         instr->intrinsic == nir_intrinsic_store_ssbo_block_intel;

      elk_fs_reg address = bld.emit_uniformize(get_nir_src(ntb, instr->src[is_ssbo ? 2 : 1]));
      elk_fs_reg src = get_nir_src(ntb, instr->src[0]);

      elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
      srcs[SURFACE_LOGICAL_SRC_SURFACE] = is_ssbo ?
         get_nir_buffer_intrinsic_index(ntb, bld, instr) :
         elk_fs_reg(elk_imm_ud(GFX7_BTI_SLM));
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = address;

      const fs_builder ubld1 = bld.exec_all().group(1, 0);
      const fs_builder ubld8 = bld.exec_all().group(8, 0);
      const fs_builder ubld16 = bld.exec_all().group(16, 0);

      const unsigned total = instr->num_components * s.dispatch_width;
      unsigned written = 0;

      while (written < total) {
         const unsigned block =
            choose_oword_block_size_dwords(devinfo, total - written);

         srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(block);
         srcs[SURFACE_LOGICAL_SRC_DATA] =
            retype(byte_offset(src, written * 4), ELK_REGISTER_TYPE_UD);

         const fs_builder &ubld = block == 8 ? ubld8 : ubld16;
         ubld.emit(ELK_SHADER_OPCODE_OWORD_BLOCK_WRITE_LOGICAL,
                   elk_fs_reg(), srcs, SURFACE_LOGICAL_NUM_SRCS);

         const unsigned block_bytes = block * 4;
         ubld1.ADD(address, address, elk_imm_ud(block_bytes));
         written += block;
      }

      assert(written == total);
      break;
   }

   default:
#ifndef NDEBUG
      assert(instr->intrinsic < nir_num_intrinsics);
      fprintf(stderr, "intrinsic: %s\n", nir_intrinsic_infos[instr->intrinsic].name);
#endif
      unreachable("unknown intrinsic");
   }
}

static elk_fs_reg
expand_to_32bit(const fs_builder &bld, const elk_fs_reg &src)
{
   if (type_sz(src.type) == 2) {
      elk_fs_reg src32 = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.MOV(src32, retype(src, ELK_REGISTER_TYPE_UW));
      return src32;
   } else {
      return src;
   }
}

static void
fs_nir_emit_surface_atomic(nir_to_elk_state &ntb, const fs_builder &bld,
                           nir_intrinsic_instr *instr,
                           elk_fs_reg surface,
                           bool bindless)
{
   const intel_device_info *devinfo = ntb.devinfo;
   elk_fs_visitor &s = ntb.s;

   enum elk_lsc_opcode op = elk_lsc_aop_for_nir_intrinsic(instr);
   int num_data = lsc_op_num_data_values(op);

   bool shared = surface.file == IMM && surface.ud == GFX7_BTI_SLM;

   /* The BTI untyped atomic messages only support 32-bit atomics.  If you
    * just look at the big table of messages in the Vol 7 of the SKL PRM, they
    * appear to exist.  However, if you look at Vol 2a, there are no message
    * descriptors provided for Qword atomic ops except for A64 messages.
    *
    * 16-bit float atomics are supported, however.
    */
   assert(instr->def.bit_size == 32 ||
          (instr->def.bit_size == 64 && devinfo->has_lsc) ||
          (instr->def.bit_size == 16 &&
           (devinfo->has_lsc || elk_lsc_opcode_is_atomic_float(op))));

   elk_fs_reg dest = get_nir_def(ntb, instr->def);

   elk_fs_reg srcs[SURFACE_LOGICAL_NUM_SRCS];
   srcs[bindless ?
        SURFACE_LOGICAL_SRC_SURFACE_HANDLE :
        SURFACE_LOGICAL_SRC_SURFACE] = surface;
   srcs[SURFACE_LOGICAL_SRC_IMM_DIMS] = elk_imm_ud(1);
   srcs[SURFACE_LOGICAL_SRC_IMM_ARG] = elk_imm_ud(op);
   srcs[SURFACE_LOGICAL_SRC_ALLOW_SAMPLE_MASK] = elk_imm_ud(1);

   if (shared) {
      /* SLM - Get the offset */
      if (nir_src_is_const(instr->src[0])) {
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] =
            elk_imm_ud(nir_intrinsic_base(instr) +
                       nir_src_as_uint(instr->src[0]));
      } else {
         srcs[SURFACE_LOGICAL_SRC_ADDRESS] = s.vgrf(glsl_uint_type());
         bld.ADD(srcs[SURFACE_LOGICAL_SRC_ADDRESS],
                 retype(get_nir_src(ntb, instr->src[0]), ELK_REGISTER_TYPE_UD),
                 elk_imm_ud(nir_intrinsic_base(instr)));
      }
   } else {
      /* SSBOs */
      srcs[SURFACE_LOGICAL_SRC_ADDRESS] = get_nir_src(ntb, instr->src[1]);
   }

   elk_fs_reg data;
   if (num_data >= 1)
      data = expand_to_32bit(bld, get_nir_src(ntb, instr->src[shared ? 1 : 2]));

   if (num_data >= 2) {
      elk_fs_reg tmp = bld.vgrf(data.type, 2);
      elk_fs_reg sources[2] = {
         data,
         expand_to_32bit(bld, get_nir_src(ntb, instr->src[shared ? 2 : 3]))
      };
      bld.LOAD_PAYLOAD(tmp, sources, 2, 0);
      data = tmp;
   }
   srcs[SURFACE_LOGICAL_SRC_DATA] = data;

   /* Emit the actual atomic operation */

   switch (instr->def.bit_size) {
      case 16: {
         elk_fs_reg dest32 = bld.vgrf(ELK_REGISTER_TYPE_UD);
         bld.emit(ELK_SHADER_OPCODE_UNTYPED_ATOMIC_LOGICAL,
                  retype(dest32, dest.type),
                  srcs, SURFACE_LOGICAL_NUM_SRCS);
         bld.MOV(retype(dest, ELK_REGISTER_TYPE_UW),
                 retype(dest32, ELK_REGISTER_TYPE_UD));
         break;
      }

      case 32:
      case 64:
         bld.emit(ELK_SHADER_OPCODE_UNTYPED_ATOMIC_LOGICAL,
                  dest, srcs, SURFACE_LOGICAL_NUM_SRCS);
         break;
      default:
         unreachable("Unsupported bit size");
   }
}

static void
fs_nir_emit_global_atomic(nir_to_elk_state &ntb, const fs_builder &bld,
                          nir_intrinsic_instr *instr)
{
   enum elk_lsc_opcode op = elk_lsc_aop_for_nir_intrinsic(instr);
   int num_data = lsc_op_num_data_values(op);

   elk_fs_reg dest = get_nir_def(ntb, instr->def);

   elk_fs_reg addr = get_nir_src(ntb, instr->src[0]);

   elk_fs_reg data;
   if (num_data >= 1)
      data = expand_to_32bit(bld, get_nir_src(ntb, instr->src[1]));

   if (num_data >= 2) {
      elk_fs_reg tmp = bld.vgrf(data.type, 2);
      elk_fs_reg sources[2] = {
         data,
         expand_to_32bit(bld, get_nir_src(ntb, instr->src[2]))
      };
      bld.LOAD_PAYLOAD(tmp, sources, 2, 0);
      data = tmp;
   }

   elk_fs_reg srcs[A64_LOGICAL_NUM_SRCS];
   srcs[A64_LOGICAL_ADDRESS] = addr;
   srcs[A64_LOGICAL_SRC] = data;
   srcs[A64_LOGICAL_ARG] = elk_imm_ud(op);
   srcs[A64_LOGICAL_ENABLE_HELPERS] = elk_imm_ud(0);

   switch (instr->def.bit_size) {
   case 16: {
      elk_fs_reg dest32 = bld.vgrf(ELK_REGISTER_TYPE_UD);
      bld.emit(ELK_SHADER_OPCODE_A64_UNTYPED_ATOMIC_LOGICAL,
               retype(dest32, dest.type),
               srcs, A64_LOGICAL_NUM_SRCS);
      bld.MOV(retype(dest, ELK_REGISTER_TYPE_UW), dest32);
      break;
   }
   case 32:
   case 64:
      bld.emit(ELK_SHADER_OPCODE_A64_UNTYPED_ATOMIC_LOGICAL, dest,
               srcs, A64_LOGICAL_NUM_SRCS);
      break;
   default:
      unreachable("Unsupported bit size");
   }
}

static void
fs_nir_emit_texture(nir_to_elk_state &ntb,
                    nir_tex_instr *instr)
{
   const intel_device_info *devinfo = ntb.devinfo;
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   elk_fs_reg srcs[TEX_LOGICAL_NUM_SRCS];

   /* SKL PRMs: Volume 7: 3D-Media-GPGPU:
    *
    *    "The Pixel Null Mask field, when enabled via the Pixel Null Mask
    *     Enable will be incorect for sample_c when applied to a surface with
    *     64-bit per texel format such as R16G16BA16_UNORM. Pixel Null mask
    *     Enable may incorrectly report pixels as referencing a Null surface."
    *
    * We'll take care of this in NIR.
    */
   assert(!instr->is_sparse || srcs[TEX_LOGICAL_SRC_SHADOW_C].file == BAD_FILE);

   srcs[TEX_LOGICAL_SRC_RESIDENCY] = elk_imm_ud(instr->is_sparse);

   int lod_components = 0;

   /* The hardware requires a LOD for buffer textures */
   if (instr->sampler_dim == GLSL_SAMPLER_DIM_BUF)
      srcs[TEX_LOGICAL_SRC_LOD] = elk_imm_d(0);

   ASSERTED bool got_lod = false;
   ASSERTED bool got_bias = false;
   uint32_t header_bits = 0;
   for (unsigned i = 0; i < instr->num_srcs; i++) {
      nir_src nir_src = instr->src[i].src;
      elk_fs_reg src = get_nir_src(ntb, nir_src);
      switch (instr->src[i].src_type) {
      case nir_tex_src_bias:
         assert(!got_lod);
         got_bias = true;

         srcs[TEX_LOGICAL_SRC_LOD] =
            retype(get_nir_src_imm(ntb, instr->src[i].src), ELK_REGISTER_TYPE_F);
         break;
      case nir_tex_src_comparator:
         srcs[TEX_LOGICAL_SRC_SHADOW_C] = retype(src, ELK_REGISTER_TYPE_F);
         break;
      case nir_tex_src_coord:
         switch (instr->op) {
         case nir_texop_txf:
         case nir_texop_txf_ms:
         case nir_texop_txf_ms_mcs_intel:
         case nir_texop_samples_identical:
            srcs[TEX_LOGICAL_SRC_COORDINATE] = retype(src, ELK_REGISTER_TYPE_D);
            break;
         default:
            srcs[TEX_LOGICAL_SRC_COORDINATE] = retype(src, ELK_REGISTER_TYPE_F);
            break;
         }
         break;
      case nir_tex_src_ddx:
         srcs[TEX_LOGICAL_SRC_LOD] = retype(src, ELK_REGISTER_TYPE_F);
         lod_components = nir_tex_instr_src_size(instr, i);
         break;
      case nir_tex_src_ddy:
         srcs[TEX_LOGICAL_SRC_LOD2] = retype(src, ELK_REGISTER_TYPE_F);
         break;
      case nir_tex_src_lod:
         assert(!got_bias);
         got_lod = true;

         switch (instr->op) {
         case nir_texop_txs:
            srcs[TEX_LOGICAL_SRC_LOD] =
               retype(get_nir_src_imm(ntb, instr->src[i].src), ELK_REGISTER_TYPE_UD);
            break;
         case nir_texop_txf:
            srcs[TEX_LOGICAL_SRC_LOD] =
               retype(get_nir_src_imm(ntb, instr->src[i].src), ELK_REGISTER_TYPE_D);
            break;
         default:
            srcs[TEX_LOGICAL_SRC_LOD] =
               retype(get_nir_src_imm(ntb, instr->src[i].src), ELK_REGISTER_TYPE_F);
            break;
         }
         break;
      case nir_tex_src_min_lod:
         srcs[TEX_LOGICAL_SRC_MIN_LOD] =
            retype(get_nir_src_imm(ntb, instr->src[i].src), ELK_REGISTER_TYPE_F);
         break;
      case nir_tex_src_ms_index:
         srcs[TEX_LOGICAL_SRC_SAMPLE_INDEX] = retype(src, ELK_REGISTER_TYPE_UD);
         break;

      case nir_tex_src_offset: {
         uint32_t offset_bits = 0;
         if (elk_texture_offset(instr, i, &offset_bits)) {
            header_bits |= offset_bits;
         } else {
            srcs[TEX_LOGICAL_SRC_TG4_OFFSET] =
               retype(src, ELK_REGISTER_TYPE_D);
         }
         break;
      }

      case nir_tex_src_projector:
         unreachable("should be lowered");

      case nir_tex_src_texture_offset: {
         assert(srcs[TEX_LOGICAL_SRC_SURFACE].file == BAD_FILE);
         /* Emit code to evaluate the actual indexing expression */
         if (instr->texture_index == 0 && is_resource_src(nir_src))
            srcs[TEX_LOGICAL_SRC_SURFACE] = get_resource_nir_src(ntb, nir_src);
         if (srcs[TEX_LOGICAL_SRC_SURFACE].file == BAD_FILE) {
            elk_fs_reg tmp = s.vgrf(glsl_uint_type());
            bld.ADD(tmp, src, elk_imm_ud(instr->texture_index));
            srcs[TEX_LOGICAL_SRC_SURFACE] = bld.emit_uniformize(tmp);
         }
         assert(srcs[TEX_LOGICAL_SRC_SURFACE].file != BAD_FILE);
         break;
      }

      case nir_tex_src_sampler_offset: {
         /* Emit code to evaluate the actual indexing expression */
         if (instr->sampler_index == 0 && is_resource_src(nir_src))
            srcs[TEX_LOGICAL_SRC_SAMPLER] = get_resource_nir_src(ntb, nir_src);
         if (srcs[TEX_LOGICAL_SRC_SAMPLER].file == BAD_FILE) {
            elk_fs_reg tmp = s.vgrf(glsl_uint_type());
            bld.ADD(tmp, src, elk_imm_ud(instr->sampler_index));
            srcs[TEX_LOGICAL_SRC_SAMPLER] = bld.emit_uniformize(tmp);
         }
         break;
      }

      case nir_tex_src_texture_handle:
         assert(nir_tex_instr_src_index(instr, nir_tex_src_texture_offset) == -1);
         srcs[TEX_LOGICAL_SRC_SURFACE] = elk_fs_reg();
         if (is_resource_src(nir_src))
            srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE] = get_resource_nir_src(ntb, nir_src);
         if (srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE].file == BAD_FILE)
            srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE] = bld.emit_uniformize(src);
         break;

      case nir_tex_src_sampler_handle:
         assert(nir_tex_instr_src_index(instr, nir_tex_src_sampler_offset) == -1);
         srcs[TEX_LOGICAL_SRC_SAMPLER] = elk_fs_reg();
         if (is_resource_src(nir_src))
            srcs[TEX_LOGICAL_SRC_SAMPLER_HANDLE] = get_resource_nir_src(ntb, nir_src);
         if (srcs[TEX_LOGICAL_SRC_SAMPLER_HANDLE].file == BAD_FILE)
            srcs[TEX_LOGICAL_SRC_SAMPLER_HANDLE] = bld.emit_uniformize(src);
         break;

      case nir_tex_src_ms_mcs_intel:
         assert(instr->op == nir_texop_txf_ms);
         srcs[TEX_LOGICAL_SRC_MCS] = retype(src, ELK_REGISTER_TYPE_D);
         break;

      /* If this parameter is present, we are packing either the explicit LOD
       * or LOD bias and the array index into a single (32-bit) value when
       * 32-bit texture coordinates are used.
       */
      case nir_tex_src_backend1:
         assert(!got_lod && !got_bias);
         got_lod = true;

         assert(instr->op == nir_texop_txl || instr->op == nir_texop_txb);
         srcs[TEX_LOGICAL_SRC_LOD] =
            retype(get_nir_src_imm(ntb, instr->src[i].src), ELK_REGISTER_TYPE_F);
         break;

      default:
         unreachable("unknown texture source");
      }
   }

   /* If the surface or sampler were not specified through sources, use the
    * instruction index.
    */
   if (srcs[TEX_LOGICAL_SRC_SURFACE].file == BAD_FILE &&
       srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE].file == BAD_FILE)
      srcs[TEX_LOGICAL_SRC_SURFACE] = elk_imm_ud(instr->texture_index);
   if (srcs[TEX_LOGICAL_SRC_SAMPLER].file == BAD_FILE &&
       srcs[TEX_LOGICAL_SRC_SAMPLER_HANDLE].file == BAD_FILE)
      srcs[TEX_LOGICAL_SRC_SAMPLER] = elk_imm_ud(instr->sampler_index);

   if (srcs[TEX_LOGICAL_SRC_MCS].file == BAD_FILE &&
       (instr->op == nir_texop_txf_ms ||
        instr->op == nir_texop_samples_identical)) {
      if (devinfo->ver >= 7) {
         srcs[TEX_LOGICAL_SRC_MCS] =
            emit_mcs_fetch(ntb, srcs[TEX_LOGICAL_SRC_COORDINATE],
                           instr->coord_components,
                           srcs[TEX_LOGICAL_SRC_SURFACE],
                           srcs[TEX_LOGICAL_SRC_SURFACE_HANDLE]);
      } else {
         srcs[TEX_LOGICAL_SRC_MCS] = elk_imm_ud(0u);
      }
   }

   srcs[TEX_LOGICAL_SRC_COORD_COMPONENTS] = elk_imm_d(instr->coord_components);
   srcs[TEX_LOGICAL_SRC_GRAD_COMPONENTS] = elk_imm_d(lod_components);

   enum elk_opcode opcode;
   switch (instr->op) {
   case nir_texop_tex:
      opcode = ELK_SHADER_OPCODE_TEX_LOGICAL;
      break;
   case nir_texop_txb:
      opcode = ELK_FS_OPCODE_TXB_LOGICAL;
      break;
   case nir_texop_txl:
      opcode = ELK_SHADER_OPCODE_TXL_LOGICAL;
      break;
   case nir_texop_txd:
      opcode = ELK_SHADER_OPCODE_TXD_LOGICAL;
      break;
   case nir_texop_txf:
      opcode = ELK_SHADER_OPCODE_TXF_LOGICAL;
      break;
   case nir_texop_txf_ms:
      opcode = ELK_SHADER_OPCODE_TXF_CMS_LOGICAL;
      break;
   case nir_texop_txf_ms_mcs_intel:
      opcode = ELK_SHADER_OPCODE_TXF_MCS_LOGICAL;
      break;
   case nir_texop_query_levels:
   case nir_texop_txs:
      opcode = ELK_SHADER_OPCODE_TXS_LOGICAL;
      break;
   case nir_texop_lod:
      opcode = ELK_SHADER_OPCODE_LOD_LOGICAL;
      break;
   case nir_texop_tg4:
      if (srcs[TEX_LOGICAL_SRC_TG4_OFFSET].file != BAD_FILE)
         opcode = ELK_SHADER_OPCODE_TG4_OFFSET_LOGICAL;
      else
         opcode = ELK_SHADER_OPCODE_TG4_LOGICAL;
      break;
   case nir_texop_texture_samples:
      opcode = ELK_SHADER_OPCODE_SAMPLEINFO_LOGICAL;
      break;
   case nir_texop_samples_identical: {
      elk_fs_reg dst = retype(get_nir_def(ntb, instr->def), ELK_REGISTER_TYPE_D);

      /* If mcs is an immediate value, it means there is no MCS.  In that case
       * just return false.
       */
      if (srcs[TEX_LOGICAL_SRC_MCS].file == ELK_IMMEDIATE_VALUE) {
         bld.MOV(dst, elk_imm_ud(0u));
      } else {
         bld.CMP(dst, srcs[TEX_LOGICAL_SRC_MCS], elk_imm_ud(0u),
                 ELK_CONDITIONAL_EQ);
      }
      return;
   }
   default:
      unreachable("unknown texture opcode");
   }

   if (instr->op == nir_texop_tg4) {
      if (instr->component == 1 &&
          s.key_tex->gather_channel_quirk_mask & (1 << instr->texture_index)) {
         /* gather4 sampler is broken for green channel on RG32F --
          * we must ask for blue instead.
          */
         header_bits |= 2 << 16;
      } else {
         header_bits |= instr->component << 16;
      }
   }

   elk_fs_reg dst = bld.vgrf(elk_type_for_nir_type(devinfo, instr->dest_type), 4 + instr->is_sparse);
   elk_fs_inst *inst = bld.emit(opcode, dst, srcs, ARRAY_SIZE(srcs));
   inst->offset = header_bits;

   const unsigned dest_size = nir_tex_instr_dest_size(instr);
   inst->size_written = 4 * inst->dst.component_size(inst->exec_size) +
                        (instr->is_sparse ? (reg_unit(devinfo) * REG_SIZE) : 0);

   if (srcs[TEX_LOGICAL_SRC_SHADOW_C].file != BAD_FILE)
      inst->shadow_compare = true;

   /* Wa_14012688258:
    *
    * Don't trim zeros at the end of payload for sample operations
    * in cube and cube arrays.
    */
   if (instr->sampler_dim == GLSL_SAMPLER_DIM_CUBE &&
       intel_needs_workaround(devinfo, 14012688258)) {

      /* Compiler should send U,V,R parameters even if V,R are 0. */
      if (srcs[TEX_LOGICAL_SRC_COORDINATE].file != BAD_FILE)
         assert(instr->coord_components >= 3u);

      /* See opt_zero_samples(). */
      inst->keep_payload_trailing_zeros = true;
   }

   elk_fs_reg nir_dest[5];
   for (unsigned i = 0; i < dest_size; i++)
      nir_dest[i] = offset(dst, bld, i);

   if (instr->op == nir_texop_query_levels) {
      /* # levels is in .w */
      /**
       * Wa_1940217:
       *
       * When a surface of type SURFTYPE_NULL is accessed by resinfo, the
       * MIPCount returned is undefined instead of 0.
       */
      elk_fs_inst *mov = bld.MOV(bld.null_reg_d(), dst);
      mov->conditional_mod = ELK_CONDITIONAL_NZ;
      nir_dest[0] = bld.vgrf(ELK_REGISTER_TYPE_D);
      elk_fs_inst *sel = bld.SEL(nir_dest[0], offset(dst, bld, 3), elk_imm_d(0));
      sel->predicate = ELK_PREDICATE_NORMAL;
   } else if (instr->op == nir_texop_txs &&
              dest_size >= 3 && devinfo->ver < 7) {
      /* Gfx4-6 return 0 instead of 1 for single layer surfaces. */
      elk_fs_reg depth = offset(dst, bld, 2);
      nir_dest[2] = s.vgrf(glsl_int_type());
      bld.emit_minmax(nir_dest[2], depth, elk_imm_d(1), ELK_CONDITIONAL_GE);
   }

   /* The residency bits are only in the first component. */
   if (instr->is_sparse)
      nir_dest[dest_size - 1] = component(offset(dst, bld, dest_size - 1), 0);

   bld.LOAD_PAYLOAD(get_nir_def(ntb, instr->def), nir_dest, dest_size, 0);
}

static void
fs_nir_emit_jump(nir_to_elk_state &ntb, nir_jump_instr *instr)
{
   switch (instr->type) {
   case nir_jump_break:
      ntb.bld.emit(ELK_OPCODE_BREAK);
      break;
   case nir_jump_continue:
      ntb.bld.emit(ELK_OPCODE_CONTINUE);
      break;
   case nir_jump_halt:
      ntb.bld.emit(ELK_OPCODE_HALT);
      break;
   case nir_jump_return:
   default:
      unreachable("unknown jump");
   }
}

/*
 * This helper takes a source register and un/shuffles it into the destination
 * register.
 *
 * If source type size is smaller than destination type size the operation
 * needed is a component shuffle. The opposite case would be an unshuffle. If
 * source/destination type size is equal a shuffle is done that would be
 * equivalent to a simple MOV.
 *
 * For example, if source is a 16-bit type and destination is 32-bit. A 3
 * components .xyz 16-bit vector on SIMD8 would be.
 *
 *    |x1|x2|x3|x4|x5|x6|x7|x8|y1|y2|y3|y4|y5|y6|y7|y8|
 *    |z1|z2|z3|z4|z5|z6|z7|z8|  |  |  |  |  |  |  |  |
 *
 * This helper will return the following 2 32-bit components with the 16-bit
 * values shuffled:
 *
 *    |x1 y1|x2 y2|x3 y3|x4 y4|x5 y5|x6 y6|x7 y7|x8 y8|
 *    |z1   |z2   |z3   |z4   |z5   |z6   |z7   |z8   |
 *
 * For unshuffle, the example would be the opposite, a 64-bit type source
 * and a 32-bit destination. A 2 component .xy 64-bit vector on SIMD8
 * would be:
 *
 *    | x1l   x1h | x2l   x2h | x3l   x3h | x4l   x4h |
 *    | x5l   x5h | x6l   x6h | x7l   x7h | x8l   x8h |
 *    | y1l   y1h | y2l   y2h | y3l   y3h | y4l   y4h |
 *    | y5l   y5h | y6l   y6h | y7l   y7h | y8l   y8h |
 *
 * The returned result would be the following 4 32-bit components unshuffled:
 *
 *    | x1l | x2l | x3l | x4l | x5l | x6l | x7l | x8l |
 *    | x1h | x2h | x3h | x4h | x5h | x6h | x7h | x8h |
 *    | y1l | y2l | y3l | y4l | y5l | y6l | y7l | y8l |
 *    | y1h | y2h | y3h | y4h | y5h | y6h | y7h | y8h |
 *
 * - Source and destination register must not be overlapped.
 * - components units are measured in terms of the smaller type between
 *   source and destination because we are un/shuffling the smaller
 *   components from/into the bigger ones.
 * - first_component parameter allows skipping source components.
 */
void
elk_shuffle_src_to_dst(const fs_builder &bld,
                   const elk_fs_reg &dst,
                   const elk_fs_reg &src,
                   uint32_t first_component,
                   uint32_t components)
{
   if (type_sz(src.type) == type_sz(dst.type)) {
      assert(!regions_overlap(dst,
         type_sz(dst.type) * bld.dispatch_width() * components,
         offset(src, bld, first_component),
         type_sz(src.type) * bld.dispatch_width() * components));
      for (unsigned i = 0; i < components; i++) {
         bld.MOV(retype(offset(dst, bld, i), src.type),
                 offset(src, bld, i + first_component));
      }
   } else if (type_sz(src.type) < type_sz(dst.type)) {
      /* Source is shuffled into destination */
      unsigned size_ratio = type_sz(dst.type) / type_sz(src.type);
      assert(!regions_overlap(dst,
         type_sz(dst.type) * bld.dispatch_width() *
         DIV_ROUND_UP(components, size_ratio),
         offset(src, bld, first_component),
         type_sz(src.type) * bld.dispatch_width() * components));

      elk_reg_type shuffle_type =
         elk_reg_type_from_bit_size(8 * type_sz(src.type),
                                    ELK_REGISTER_TYPE_D);
      for (unsigned i = 0; i < components; i++) {
         elk_fs_reg shuffle_component_i =
            subscript(offset(dst, bld, i / size_ratio),
                      shuffle_type, i % size_ratio);
         bld.MOV(shuffle_component_i,
                 retype(offset(src, bld, i + first_component), shuffle_type));
      }
   } else {
      /* Source is unshuffled into destination */
      unsigned size_ratio = type_sz(src.type) / type_sz(dst.type);
      assert(!regions_overlap(dst,
         type_sz(dst.type) * bld.dispatch_width() * components,
         offset(src, bld, first_component / size_ratio),
         type_sz(src.type) * bld.dispatch_width() *
         DIV_ROUND_UP(components + (first_component % size_ratio),
                      size_ratio)));

      elk_reg_type shuffle_type =
         elk_reg_type_from_bit_size(8 * type_sz(dst.type),
                                    ELK_REGISTER_TYPE_D);
      for (unsigned i = 0; i < components; i++) {
         elk_fs_reg shuffle_component_i =
            subscript(offset(src, bld, (first_component + i) / size_ratio),
                      shuffle_type, (first_component + i) % size_ratio);
         bld.MOV(retype(offset(dst, bld, i), shuffle_type),
                 shuffle_component_i);
      }
   }
}

void
elk_shuffle_from_32bit_read(const fs_builder &bld,
                        const elk_fs_reg &dst,
                        const elk_fs_reg &src,
                        uint32_t first_component,
                        uint32_t components)
{
   assert(type_sz(src.type) == 4);

   /* This function takes components in units of the destination type while
    * elk_shuffle_src_to_dst takes components in units of the smallest type
    */
   if (type_sz(dst.type) > 4) {
      assert(type_sz(dst.type) == 8);
      first_component *= 2;
      components *= 2;
   }

   elk_shuffle_src_to_dst(bld, dst, src, first_component, components);
}

elk_fs_reg
elk_setup_imm_df(const fs_builder &bld, double v)
{
   const struct intel_device_info *devinfo = bld.shader->devinfo;
   assert(devinfo->ver >= 7);

   if (devinfo->ver >= 8)
      return elk_imm_df(v);

   /* gfx7.5 does not support DF immediates straightforward but the DIM
    * instruction allows to set the 64-bit immediate value.
    */
   if (devinfo->platform == INTEL_PLATFORM_HSW) {
      const fs_builder ubld = bld.exec_all().group(1, 0);
      elk_fs_reg dst = ubld.vgrf(ELK_REGISTER_TYPE_DF, 1);
      ubld.DIM(dst, elk_imm_df(v));
      return component(dst, 0);
   }

   /* gfx7 does not support DF immediates, so we generate a 64-bit constant by
    * writing the low 32-bit of the constant to suboffset 0 of a VGRF and
    * the high 32-bit to suboffset 4 and then applying a stride of 0.
    *
    * Alternatively, we could also produce a normal VGRF (without stride 0)
    * by writing to all the channels in the VGRF, however, that would hit the
    * gfx7 bug where we have to split writes that span more than 1 register
    * into instructions with a width of 4 (otherwise the write to the second
    * register written runs into an execmask hardware bug) which isn't very
    * nice.
    */
   union {
      double d;
      struct {
         uint32_t i1;
         uint32_t i2;
      };
   } di;

   di.d = v;

   const fs_builder ubld = bld.exec_all().group(1, 0);
   const elk_fs_reg tmp = ubld.vgrf(ELK_REGISTER_TYPE_UD, 2);
   ubld.MOV(tmp, elk_imm_ud(di.i1));
   ubld.MOV(horiz_offset(tmp, 1), elk_imm_ud(di.i2));

   return component(retype(tmp, ELK_REGISTER_TYPE_DF), 0);
}

elk_fs_reg
elk_setup_imm_b(const fs_builder &bld, int8_t v)
{
   const elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_B);
   bld.MOV(tmp, elk_imm_w(v));
   return tmp;
}

elk_fs_reg
elk_setup_imm_ub(const fs_builder &bld, uint8_t v)
{
   const elk_fs_reg tmp = bld.vgrf(ELK_REGISTER_TYPE_UB);
   bld.MOV(tmp, elk_imm_uw(v));
   return tmp;
}

static void
fs_nir_emit_instr(nir_to_elk_state &ntb, nir_instr *instr)
{
   ntb.bld = ntb.bld.annotate(NULL, instr);

   switch (instr->type) {
   case nir_instr_type_alu:
      fs_nir_emit_alu(ntb, nir_instr_as_alu(instr), true);
      break;

   case nir_instr_type_deref:
      unreachable("All derefs should've been lowered");
      break;

   case nir_instr_type_intrinsic:
      switch (ntb.s.stage) {
      case MESA_SHADER_VERTEX:
         fs_nir_emit_vs_intrinsic(ntb, nir_instr_as_intrinsic(instr));
         break;
      case MESA_SHADER_TESS_CTRL:
         fs_nir_emit_tcs_intrinsic(ntb, nir_instr_as_intrinsic(instr));
         break;
      case MESA_SHADER_TESS_EVAL:
         fs_nir_emit_tes_intrinsic(ntb, nir_instr_as_intrinsic(instr));
         break;
      case MESA_SHADER_GEOMETRY:
         fs_nir_emit_gs_intrinsic(ntb, nir_instr_as_intrinsic(instr));
         break;
      case MESA_SHADER_FRAGMENT:
         fs_nir_emit_fs_intrinsic(ntb, nir_instr_as_intrinsic(instr));
         break;
      case MESA_SHADER_COMPUTE:
         fs_nir_emit_cs_intrinsic(ntb, nir_instr_as_intrinsic(instr));
         break;
      default:
         unreachable("unsupported shader stage");
      }
      break;

   case nir_instr_type_tex:
      fs_nir_emit_texture(ntb, nir_instr_as_tex(instr));
      break;

   case nir_instr_type_load_const:
      fs_nir_emit_load_const(ntb, nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_undef:
      /* We create a new VGRF for undefs on every use (by handling
       * them in get_nir_src()), rather than for each definition.
       * This helps register coalescing eliminate MOVs from undef.
       */
      break;

   case nir_instr_type_jump:
      fs_nir_emit_jump(ntb, nir_instr_as_jump(instr));
      break;

   default:
      unreachable("unknown instruction type");
   }
}

static unsigned
elk_rnd_mode_from_nir(unsigned mode, unsigned *mask)
{
   unsigned elk_mode = 0;
   *mask = 0;

   if ((FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP32 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP64) &
       mode) {
      elk_mode |= ELK_RND_MODE_RTZ << ELK_CR0_RND_MODE_SHIFT;
      *mask |= ELK_CR0_RND_MODE_MASK;
   }
   if ((FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP32 |
        FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP64) &
       mode) {
      elk_mode |= ELK_RND_MODE_RTNE << ELK_CR0_RND_MODE_SHIFT;
      *mask |= ELK_CR0_RND_MODE_MASK;
   }
   if (mode & FLOAT_CONTROLS_DENORM_PRESERVE_FP16) {
      elk_mode |= ELK_CR0_FP16_DENORM_PRESERVE;
      *mask |= ELK_CR0_FP16_DENORM_PRESERVE;
   }
   if (mode & FLOAT_CONTROLS_DENORM_PRESERVE_FP32) {
      elk_mode |= ELK_CR0_FP32_DENORM_PRESERVE;
      *mask |= ELK_CR0_FP32_DENORM_PRESERVE;
   }
   if (mode & FLOAT_CONTROLS_DENORM_PRESERVE_FP64) {
      elk_mode |= ELK_CR0_FP64_DENORM_PRESERVE;
      *mask |= ELK_CR0_FP64_DENORM_PRESERVE;
   }
   if (mode & FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP16)
      *mask |= ELK_CR0_FP16_DENORM_PRESERVE;
   if (mode & FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP32)
      *mask |= ELK_CR0_FP32_DENORM_PRESERVE;
   if (mode & FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP64)
      *mask |= ELK_CR0_FP64_DENORM_PRESERVE;
   if (mode == FLOAT_CONTROLS_DEFAULT_FLOAT_CONTROL_MODE)
      *mask |= ELK_CR0_FP_MODE_MASK;

   if (*mask != 0)
      assert((*mask & elk_mode) == elk_mode);

   return elk_mode;
}

static void
emit_shader_float_controls_execution_mode(nir_to_elk_state &ntb)
{
   const fs_builder &bld = ntb.bld;
   elk_fs_visitor &s = ntb.s;

   unsigned execution_mode = s.nir->info.float_controls_execution_mode;
   if (execution_mode == FLOAT_CONTROLS_DEFAULT_FLOAT_CONTROL_MODE)
      return;

   fs_builder ubld = bld.exec_all().group(1, 0);
   fs_builder abld = ubld.annotate("shader floats control execution mode");
   unsigned mask, mode = elk_rnd_mode_from_nir(execution_mode, &mask);

   if (mask == 0)
      return;

   abld.emit(ELK_SHADER_OPCODE_FLOAT_CONTROL_MODE, bld.null_reg_ud(),
             elk_imm_d(mode), elk_imm_d(mask));
}

void
nir_to_elk(elk_fs_visitor *s)
{
   nir_to_elk_state ntb = {
      .s       = *s,
      .nir     = s->nir,
      .devinfo = s->devinfo,
      .mem_ctx = ralloc_context(NULL),
      .bld     = fs_builder(s).at_end(),
   };

   emit_shader_float_controls_execution_mode(ntb);

   /* emit the arrays used for inputs and outputs - load/store intrinsics will
    * be converted to reads/writes of these arrays
    */
   fs_nir_setup_outputs(ntb);
   fs_nir_setup_uniforms(ntb.s);
   fs_nir_emit_system_values(ntb);
   ntb.s.last_scratch = ALIGN(ntb.nir->scratch_size, 4) * ntb.s.dispatch_width;

   fs_nir_emit_impl(ntb, nir_shader_get_entrypoint((nir_shader *)ntb.nir));

   ntb.bld.emit(ELK_SHADER_OPCODE_HALT_TARGET);

   ralloc_free(ntb.mem_ctx);
}

