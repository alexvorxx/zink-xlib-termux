/*
 * Copyright © 2013 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file brw_vec4_gs_visitor.cpp
 *
 * Geometry-shader-specific code derived from the vec4_visitor class.
 */

#include "brw_vec4_gs_visitor.h"
#include "brw_cfg.h"
#include "brw_fs.h"

namespace brw {

vec4_gs_visitor::vec4_gs_visitor(const struct brw_compiler *compiler,
                                 const struct brw_compile_params *params,
                                 struct brw_gs_compile *c,
                                 struct brw_gs_prog_data *prog_data,
                                 const nir_shader *shader,
                                 bool no_spills,
                                 bool debug_enabled)
   : vec4_visitor(compiler, params, &c->key.base.tex,
                  &prog_data->base, shader,
                  no_spills, debug_enabled),
     c(c),
     gs_prog_data(prog_data)
{
}


static inline struct brw_reg
attribute_to_hw_reg(int attr, brw_reg_type type, bool interleaved)
{
   struct brw_reg reg;

   unsigned width = REG_SIZE / 2 / MAX2(4, type_sz(type));
   if (interleaved) {
      reg = stride(brw_vecn_grf(width, attr / 2, (attr % 2) * 4), 0, width, 1);
   } else {
      reg = brw_vecn_grf(width, attr, 0);
   }

   reg.type = type;
   return reg;
}

/**
 * Replace each register of type ATTR in this->instructions with a reference
 * to a fixed HW register.
 *
 * If interleaved is true, then each attribute takes up half a register, with
 * register N containing attribute 2*N in its first half and attribute 2*N+1
 * in its second half (this corresponds to the payload setup used by geometry
 * shaders in "single" or "dual instanced" dispatch mode).  If interleaved is
 * false, then each attribute takes up a whole register, with register N
 * containing attribute N (this corresponds to the payload setup used by
 * vertex shaders, and by geometry shaders in "dual object" dispatch mode).
 */
int
vec4_gs_visitor::setup_varying_inputs(int payload_reg,
                                      int attributes_per_reg)
{
   /* For geometry shaders there are N copies of the input attributes, where N
    * is the number of input vertices.  attribute_map[BRW_VARYING_SLOT_COUNT *
    * i + j] represents attribute j for vertex i.
    *
    * Note that GS inputs are read from the VUE 256 bits (2 vec4's) at a time,
    * so the total number of input slots that will be delivered to the GS (and
    * thus the stride of the input arrays) is urb_read_length * 2.
    */
   const unsigned num_input_vertices = nir->info.gs.vertices_in;
   assert(num_input_vertices <= MAX_GS_INPUT_VERTICES);
   unsigned input_array_stride = prog_data->urb_read_length * 2;

   foreach_block_and_inst(block, vec4_instruction, inst, cfg) {
      for (int i = 0; i < 3; i++) {
         if (inst->src[i].file != ATTR)
            continue;

         assert(inst->src[i].offset % REG_SIZE == 0);
         int grf = payload_reg * attributes_per_reg +
                   inst->src[i].nr + inst->src[i].offset / REG_SIZE;

         struct brw_reg reg =
            attribute_to_hw_reg(grf, inst->src[i].type, attributes_per_reg > 1);
         reg.swizzle = inst->src[i].swizzle;
         if (inst->src[i].abs)
            reg = brw_abs(reg);
         if (inst->src[i].negate)
            reg = negate(reg);

         inst->src[i] = reg;
      }
   }

   int regs_used = ALIGN(input_array_stride * num_input_vertices,
                         attributes_per_reg) / attributes_per_reg;
   return payload_reg + regs_used;
}

void
vec4_gs_visitor::setup_payload()
{
   /* If we are in dual instanced or single mode, then attributes are going
    * to be interleaved, so one register contains two attribute slots.
    */
   int attributes_per_reg =
      prog_data->dispatch_mode == INTEL_DISPATCH_MODE_4X2_DUAL_OBJECT ? 1 : 2;

   int reg = 0;

   /* The payload always contains important data in r0, which contains
    * the URB handles that are passed on to the URB write at the end
    * of the thread.
    */
   reg++;

   /* If the shader uses gl_PrimitiveIDIn, that goes in r1. */
   if (gs_prog_data->include_primitive_id)
      reg++;

   reg = setup_uniforms(reg);

   reg = setup_varying_inputs(reg, attributes_per_reg);

   this->first_non_payload_grf = reg;
}


void
vec4_gs_visitor::emit_prolog()
{
   /* In vertex shaders, r0.2 is guaranteed to be initialized to zero.  In
    * geometry shaders, it isn't (it contains a bunch of information we don't
    * need, like the input primitive type).  We need r0.2 to be zero in order
    * to build scratch read/write messages correctly (otherwise this value
    * will be interpreted as a global offset, causing us to do our scratch
    * reads/writes to garbage memory).  So just set it to zero at the top of
    * the shader.
    */
   this->current_annotation = "clear r0.2";
   dst_reg r0(retype(brw_vec4_grf(0, 0), BRW_REGISTER_TYPE_UD));
   vec4_instruction *inst = emit(GS_OPCODE_SET_DWORD_2, r0, brw_imm_ud(0u));
   inst->force_writemask_all = true;

   /* Create a virtual register to hold the vertex count */
   this->vertex_count = src_reg(this, glsl_uint_type());

   /* Initialize the vertex_count register to 0 */
   this->current_annotation = "initialize vertex_count";
   inst = emit(MOV(dst_reg(this->vertex_count), brw_imm_ud(0u)));
   inst->force_writemask_all = true;

   if (c->control_data_header_size_bits > 0) {
      /* Create a virtual register to hold the current set of control data
       * bits.
       */
      this->control_data_bits = src_reg(this, glsl_uint_type());

      /* If we're outputting more than 32 control data bits, then EmitVertex()
       * will set control_data_bits to 0 after emitting the first vertex.
       * Otherwise, we need to initialize it to 0 here.
       */
      if (c->control_data_header_size_bits <= 32) {
         this->current_annotation = "initialize control data bits";
         inst = emit(MOV(dst_reg(this->control_data_bits), brw_imm_ud(0u)));
         inst->force_writemask_all = true;
      }
   }

   this->current_annotation = NULL;
}

void
vec4_gs_visitor::emit_thread_end()
{
   if (c->control_data_header_size_bits > 0) {
      /* During shader execution, we only ever call emit_control_data_bits()
       * just prior to outputting a vertex.  Therefore, the control data bits
       * corresponding to the most recently output vertex still need to be
       * emitted.
       */
      current_annotation = "thread end: emit control data bits";
      emit_control_data_bits();
   }

   /* MRF 0 is reserved for the debugger, so start with message header
    * in MRF 1.
    */
   int base_mrf = 1;

   current_annotation = "thread end";
   dst_reg mrf_reg(MRF, base_mrf);
   src_reg r0(retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UD));
   vec4_instruction *inst = emit(MOV(mrf_reg, r0));
   inst->force_writemask_all = true;
   emit(GS_OPCODE_SET_VERTEX_COUNT, mrf_reg, this->vertex_count);
   inst = emit(GS_OPCODE_THREAD_END);
   inst->base_mrf = base_mrf;
   inst->mlen = 1;
}


void
vec4_gs_visitor::emit_urb_write_header(int mrf)
{
   /* The SEND instruction that writes the vertex data to the VUE will use
    * per_slot_offset=true, which means that DWORDs 3 and 4 of the message
    * header specify an offset (in multiples of 256 bits) into the URB entry
    * at which the write should take place.
    *
    * So we have to prepare a message header with the appropriate offset
    * values.
    */
   dst_reg mrf_reg(MRF, mrf);
   src_reg r0(retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UD));
   this->current_annotation = "URB write header";
   vec4_instruction *inst = emit(MOV(mrf_reg, r0));
   inst->force_writemask_all = true;
   emit(GS_OPCODE_SET_WRITE_OFFSET, mrf_reg, this->vertex_count,
        brw_imm_ud(gs_prog_data->output_vertex_size_hwords));
}


vec4_instruction *
vec4_gs_visitor::emit_urb_write_opcode(bool complete)
{
   /* We don't care whether the vertex is complete, because in general
    * geometry shaders output multiple vertices, and we don't terminate the
    * thread until all vertices are complete.
    */
   (void) complete;

   vec4_instruction *inst = emit(VEC4_GS_OPCODE_URB_WRITE);
   inst->offset = gs_prog_data->control_data_header_size_hwords;

   inst->urb_write_flags = BRW_URB_WRITE_PER_SLOT_OFFSET;
   return inst;
}


/**
 * Write out a batch of 32 control data bits from the control_data_bits
 * register to the URB.
 *
 * The current value of the vertex_count register determines which DWORD in
 * the URB receives the control data bits.  The control_data_bits register is
 * assumed to contain the correct data for the vertex that was most recently
 * output, and all previous vertices that share the same DWORD.
 *
 * This function takes care of ensuring that if no vertices have been output
 * yet, no control bits are emitted.
 */
void
vec4_gs_visitor::emit_control_data_bits()
{
   assert(c->control_data_bits_per_vertex != 0);

   /* Since the URB_WRITE_OWORD message operates with 128-bit (vec4 sized)
    * granularity, we need to use two tricks to ensure that the batch of 32
    * control data bits is written to the appropriate DWORD in the URB.  To
    * select which vec4 we are writing to, we use the "slot {0,1} offset"
    * fields of the message header.  To select which DWORD in the vec4 we are
    * writing to, we use the channel mask fields of the message header.  To
    * avoid penalizing geometry shaders that emit a small number of vertices
    * with extra bookkeeping, we only do each of these tricks when
    * c->prog_data.control_data_header_size_bits is large enough to make it
    * necessary.
    *
    * Note: this means that if we're outputting just a single DWORD of control
    * data bits, we'll actually replicate it four times since we won't do any
    * channel masking.  But that's not a problem since in this case the
    * hardware only pays attention to the first DWORD.
    */
   enum brw_urb_write_flags urb_write_flags = BRW_URB_WRITE_OWORD;
   if (c->control_data_header_size_bits > 32)
      urb_write_flags = urb_write_flags | BRW_URB_WRITE_USE_CHANNEL_MASKS;
   if (c->control_data_header_size_bits > 128)
      urb_write_flags = urb_write_flags | BRW_URB_WRITE_PER_SLOT_OFFSET;

   /* If we are using either channel masks or a per-slot offset, then we
    * need to figure out which DWORD we are trying to write to, using the
    * formula:
    *
    *     dword_index = (vertex_count - 1) * bits_per_vertex / 32
    *
    * Since bits_per_vertex is a power of two, and is known at compile
    * time, this can be optimized to:
    *
    *     dword_index = (vertex_count - 1) >> (6 - log2(bits_per_vertex))
    */
   src_reg dword_index(this, glsl_uint_type());
   if (urb_write_flags) {
      src_reg prev_count(this, glsl_uint_type());
      emit(ADD(dst_reg(prev_count), this->vertex_count,
               brw_imm_ud(0xffffffffu)));
      unsigned log2_bits_per_vertex =
         util_last_bit(c->control_data_bits_per_vertex);
      emit(SHR(dst_reg(dword_index), prev_count,
               brw_imm_ud(6 - log2_bits_per_vertex)));
   }

   /* Start building the URB write message.  The first MRF gets a copy of
    * R0.
    */
   int base_mrf = 1;
   dst_reg mrf_reg(MRF, base_mrf);
   src_reg r0(retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UD));
   vec4_instruction *inst = emit(MOV(mrf_reg, r0));
   inst->force_writemask_all = true;

   if (urb_write_flags & BRW_URB_WRITE_PER_SLOT_OFFSET) {
      /* Set the per-slot offset to dword_index / 4, to that we'll write to
       * the appropriate OWORD within the control data header.
       */
      src_reg per_slot_offset(this, glsl_uint_type());
      emit(SHR(dst_reg(per_slot_offset), dword_index, brw_imm_ud(2u)));
      emit(GS_OPCODE_SET_WRITE_OFFSET, mrf_reg, per_slot_offset,
           brw_imm_ud(1u));
   }

   if (urb_write_flags & BRW_URB_WRITE_USE_CHANNEL_MASKS) {
      /* Set the channel masks to 1 << (dword_index % 4), so that we'll
       * write to the appropriate DWORD within the OWORD.  We need to do
       * this computation with force_writemask_all, otherwise garbage data
       * from invocation 0 might clobber the mask for invocation 1 when
       * GS_OPCODE_PREPARE_CHANNEL_MASKS tries to OR the two masks
       * together.
       */
      src_reg channel(this, glsl_uint_type());
      inst = emit(AND(dst_reg(channel), dword_index, brw_imm_ud(3u)));
      inst->force_writemask_all = true;
      src_reg one(this, glsl_uint_type());
      inst = emit(MOV(dst_reg(one), brw_imm_ud(1u)));
      inst->force_writemask_all = true;
      src_reg channel_mask(this, glsl_uint_type());
      inst = emit(SHL(dst_reg(channel_mask), one, channel));
      inst->force_writemask_all = true;
      emit(GS_OPCODE_PREPARE_CHANNEL_MASKS, dst_reg(channel_mask),
                                            channel_mask);
      emit(GS_OPCODE_SET_CHANNEL_MASKS, mrf_reg, channel_mask);
   }

   /* Store the control data bits in the message payload and send it. */
   dst_reg mrf_reg2(MRF, base_mrf + 1);
   inst = emit(MOV(mrf_reg2, this->control_data_bits));
   inst->force_writemask_all = true;
   inst = emit(VEC4_GS_OPCODE_URB_WRITE);
   inst->urb_write_flags = urb_write_flags;
   inst->base_mrf = base_mrf;
   inst->mlen = 2;
}

void
vec4_gs_visitor::set_stream_control_data_bits(unsigned stream_id)
{
   /* control_data_bits |= stream_id << ((2 * (vertex_count - 1)) % 32) */

   /* Note: we are calling this *before* increasing vertex_count, so
    * this->vertex_count == vertex_count - 1 in the formula above.
    */

   /* Stream mode uses 2 bits per vertex */
   assert(c->control_data_bits_per_vertex == 2);

   /* Must be a valid stream */
   assert(stream_id < 4); /* MAX_VERTEX_STREAMS */

   /* Control data bits are initialized to 0 so we don't have to set any
    * bits when sending vertices to stream 0.
    */
   if (stream_id == 0)
      return;

   /* reg::sid = stream_id */
   src_reg sid(this, glsl_uint_type());
   emit(MOV(dst_reg(sid), brw_imm_ud(stream_id)));

   /* reg:shift_count = 2 * (vertex_count - 1) */
   src_reg shift_count(this, glsl_uint_type());
   emit(SHL(dst_reg(shift_count), this->vertex_count, brw_imm_ud(1u)));

   /* Note: we're relying on the fact that the GEN SHL instruction only pays
    * attention to the lower 5 bits of its second source argument, so on this
    * architecture, stream_id << 2 * (vertex_count - 1) is equivalent to
    * stream_id << ((2 * (vertex_count - 1)) % 32).
    */
   src_reg mask(this, glsl_uint_type());
   emit(SHL(dst_reg(mask), sid, shift_count));
   emit(OR(dst_reg(this->control_data_bits), this->control_data_bits, mask));
}

void
vec4_gs_visitor::gs_emit_vertex(int stream_id)
{
   this->current_annotation = "emit vertex: safety check";

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
   if (stream_id > 0 && !nir->info.has_transform_feedback_varyings)
      return;

   /* If we're outputting 32 control data bits or less, then we can wait
    * until the shader is over to output them all.  Otherwise we need to
    * output them as we go.  Now is the time to do it, since we're about to
    * output the vertex_count'th vertex, so it's guaranteed that the
    * control data bits associated with the (vertex_count - 1)th vertex are
    * correct.
    */
   if (c->control_data_header_size_bits > 32) {
      this->current_annotation = "emit vertex: emit control data bits";
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
       */
      vec4_instruction *inst =
         emit(AND(dst_null_ud(), this->vertex_count,
                  brw_imm_ud(32 / c->control_data_bits_per_vertex - 1)));
      inst->conditional_mod = BRW_CONDITIONAL_Z;

      emit(IF(BRW_PREDICATE_NORMAL));
      {
         /* If vertex_count is 0, then no control data bits have been
          * accumulated yet, so we skip emitting them.
          */
         emit(CMP(dst_null_ud(), this->vertex_count, brw_imm_ud(0u),
                  BRW_CONDITIONAL_NEQ));
         emit(IF(BRW_PREDICATE_NORMAL));
         emit_control_data_bits();
         emit(BRW_OPCODE_ENDIF);

         /* Reset control_data_bits to 0 so we can start accumulating a new
          * batch.
          *
          * Note: in the case where vertex_count == 0, this neutralizes the
          * effect of any call to EndPrimitive() that the shader may have
          * made before outputting its first vertex.
          */
         inst = emit(MOV(dst_reg(this->control_data_bits), brw_imm_ud(0u)));
         inst->force_writemask_all = true;
      }
      emit(BRW_OPCODE_ENDIF);
   }

   this->current_annotation = "emit vertex: vertex data";
   emit_vertex();

   /* In stream mode we have to set control data bits for all vertices
    * unless we have disabled control data bits completely (which we do
    * do for MESA_PRIM_POINTS outputs that don't use streams).
    */
   if (c->control_data_header_size_bits > 0 &&
       gs_prog_data->control_data_format ==
          GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_SID) {
       this->current_annotation = "emit vertex: Stream control data bits";
       set_stream_control_data_bits(stream_id);
   }

   this->current_annotation = NULL;
}

void
vec4_gs_visitor::gs_end_primitive()
{
   /* We can only do EndPrimitive() functionality when the control data
    * consists of cut bits.  Fortunately, the only time it isn't is when the
    * output type is points, in which case EndPrimitive() is a no-op.
    */
   if (gs_prog_data->control_data_format !=
       GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_CUT) {
      return;
   }

   if (c->control_data_header_size_bits == 0)
      return;

   /* Cut bits use one bit per vertex. */
   assert(c->control_data_bits_per_vertex == 1);

   /* Cut bit n should be set to 1 if EndPrimitive() was called after emitting
    * vertex n, 0 otherwise.  So all we need to do here is mark bit
    * (vertex_count - 1) % 32 in the cut_bits register to indicate that
    * EndPrimitive() was called after emitting vertex (vertex_count - 1);
    * vec4_gs_visitor::emit_control_data_bits() will take care of the rest.
    *
    * Note that if EndPrimitve() is called before emitting any vertices, this
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

   /* control_data_bits |= 1 << ((vertex_count - 1) % 32) */
   src_reg one(this, glsl_uint_type());
   emit(MOV(dst_reg(one), brw_imm_ud(1u)));
   src_reg prev_count(this, glsl_uint_type());
   emit(ADD(dst_reg(prev_count), this->vertex_count, brw_imm_ud(0xffffffffu)));
   src_reg mask(this, glsl_uint_type());
   /* Note: we're relying on the fact that the GEN SHL instruction only pays
    * attention to the lower 5 bits of its second source argument, so on this
    * architecture, 1 << (vertex_count - 1) is equivalent to 1 <<
    * ((vertex_count - 1) % 32).
    */
   emit(SHL(dst_reg(mask), one, prev_count));
   emit(OR(dst_reg(this->control_data_bits), this->control_data_bits, mask));
}

} /* namespace brw */

