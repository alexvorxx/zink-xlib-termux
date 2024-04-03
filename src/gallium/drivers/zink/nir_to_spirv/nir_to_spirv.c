/*
 * Copyright 2018 Collabora Ltd.
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

#include "nir_to_spirv.h"
#include "spirv_builder.h"

#include "nir.h"
#include "pipe/p_state.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/hash_table.h"

#define SLOT_UNSET ((unsigned char) -1)

struct ntv_context {
   void *mem_ctx;

   /* SPIR-V 1.4 and later requires entrypoints to list all global
    * variables in the interface.
    */
   bool spirv_1_4_interfaces;

   bool explicit_lod; //whether to set lod=0 for texture()

   struct spirv_builder builder;
   nir_shader *nir;

   struct hash_table *glsl_types;
   struct hash_table *bo_struct_types;
   struct hash_table *bo_array_types;

   SpvId GLSL_std_450;

   gl_shader_stage stage;
   const struct zink_shader_info *sinfo;

   SpvId ubos[2][5]; //8, 16, 32, unused, 64
   nir_variable *ubo_vars[2];

   SpvId ssbos[5]; //8, 16, 32, unused, 64
   nir_variable *ssbo_vars;

   SpvId image_types[PIPE_MAX_SHADER_IMAGES];
   SpvId images[PIPE_MAX_SHADER_IMAGES];
   SpvId sampler_types[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   SpvId samplers[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   SpvId cl_samplers[PIPE_MAX_SAMPLERS];
   nir_variable *sampler_var[PIPE_MAX_SHADER_SAMPLER_VIEWS]; /* driver_location -> variable */
   unsigned last_sampler;
   nir_variable *image_var[PIPE_MAX_SHADER_IMAGES]; /* driver_location -> variable */

   SpvId entry_ifaces[PIPE_MAX_SHADER_INPUTS * 4 + PIPE_MAX_SHADER_OUTPUTS * 4];
   size_t num_entry_ifaces;

   SpvId *defs;
   SpvId *resident_defs;
   size_t num_defs;

   SpvId *regs;
   size_t num_regs;

   struct hash_table *vars; /* nir_variable -> SpvId */
   struct hash_table *so_outputs; /* pipe_stream_output -> SpvId */
   unsigned outputs[VARYING_SLOT_MAX * 4];
   const struct glsl_type *so_output_gl_types[VARYING_SLOT_MAX * 4];
   SpvId so_output_types[VARYING_SLOT_MAX * 4];

   const SpvId *block_ids;
   size_t num_blocks;
   bool block_started;
   SpvId loop_break, loop_cont;

   SpvId shared_block_var[5]; //8, 16, 32, unused, 64
   SpvId scratch_block_var[5]; //8, 16, 32, unused, 64

   SpvId front_face_var, instance_id_var, vertex_id_var,
         primitive_id_var, invocation_id_var, // geometry
         sample_mask_type, sample_id_var, sample_pos_var, sample_mask_in_var,
         tess_patch_vertices_in, tess_coord_var, // tess
         push_const_var,
         workgroup_id_var, num_workgroups_var,
         local_invocation_id_var, global_invocation_id_var,
         local_invocation_index_var, helper_invocation_var,
         local_group_size_var,
         base_vertex_var, base_instance_var, draw_id_var;

   SpvId subgroup_eq_mask_var,
         subgroup_ge_mask_var,
         subgroup_gt_mask_var,
         subgroup_id_var,
         subgroup_invocation_var,
         subgroup_le_mask_var,
         subgroup_lt_mask_var,
         subgroup_size_var;

   SpvId discard_func;
};

static SpvId
get_fvec_constant(struct ntv_context *ctx, unsigned bit_size,
                  unsigned num_components, double value);

static SpvId
get_ivec_constant(struct ntv_context *ctx, unsigned bit_size,
                  unsigned num_components, int64_t value);

static SpvId
emit_unop(struct ntv_context *ctx, SpvOp op, SpvId type, SpvId src);

static SpvId
emit_binop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1);

static SpvId
emit_triop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1, SpvId src2);

static SpvId
get_bvec_type(struct ntv_context *ctx, int num_components)
{
   SpvId bool_type = spirv_builder_type_bool(&ctx->builder);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, bool_type,
                                       num_components);

   assert(num_components == 1);
   return bool_type;
}

static SpvScope
get_scope(nir_scope scope)
{
   SpvScope conv[] = {
      [NIR_SCOPE_NONE] = 0,
      [NIR_SCOPE_INVOCATION] = SpvScopeInvocation,
      [NIR_SCOPE_SUBGROUP] = SpvScopeSubgroup,
      [NIR_SCOPE_SHADER_CALL] = SpvScopeShaderCallKHR,
      [NIR_SCOPE_WORKGROUP] = SpvScopeWorkgroup,
      [NIR_SCOPE_QUEUE_FAMILY] = SpvScopeQueueFamily,
      [NIR_SCOPE_DEVICE] = SpvScopeDevice,
   };
   return conv[scope];
}

static SpvId
block_label(struct ntv_context *ctx, nir_block *block)
{
   assert(block->index < ctx->num_blocks);
   return ctx->block_ids[block->index];
}

static void
emit_access_decorations(struct ntv_context *ctx, nir_variable *var, SpvId var_id)
{
    u_foreach_bit(bit, var->data.access) {
       switch (1 << bit) {
       case ACCESS_COHERENT:
          /* SpvDecorationCoherent can't be used with vulkan memory model */
          break;
       case ACCESS_RESTRICT:
          spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationRestrict);
          break;
       case ACCESS_VOLATILE:
          /* SpvDecorationVolatile can't be used with vulkan memory model */
          break;
       case ACCESS_NON_READABLE:
          spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationNonReadable);
          break;
       case ACCESS_NON_WRITEABLE:
          spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationNonWritable);
          break;
       case ACCESS_NON_UNIFORM:
          spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationNonUniform);
          break;
       case ACCESS_CAN_REORDER:
       case ACCESS_STREAM_CACHE_POLICY:
          /* no equivalent */
          break;
       default:
          unreachable("unknown access bit");
       }
    }
    /* The Simple, GLSL, and Vulkan memory models can assume that aliasing is generally
     * not present between the memory object declarations. Specifically, the consumer
     * is free to assume aliasing is not present between memory object declarations,
     * unless the memory object declarations explicitly indicate they alias.
     * ...
     * Applying Restrict is allowed, but has no effect.
     * ...
     * Only those memory object declarations decorated with Aliased or AliasedPointer may alias each other.
     *
     * - SPIRV 2.18.2 Aliasing
     *
     * thus if the variable isn't marked restrict, assume it may alias
     */
    if (!(var->data.access & ACCESS_RESTRICT))
       spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationAliased);
}

static SpvOp
get_atomic_op(struct ntv_context *ctx, unsigned bit_size, nir_intrinsic_op op)
{
   switch (op) {
#define CASE_ATOMIC_OP(type) \
   case nir_intrinsic_deref_atomic_##type: \
   case nir_intrinsic_image_deref_atomic_##type: \
   case nir_intrinsic_shared_atomic_##type

#define ATOMIC_FCAP(NAME) \
   do {\
      if (bit_size == 16) \
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityAtomicFloat16##NAME##EXT); \
      if (bit_size == 32) \
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityAtomicFloat32##NAME##EXT); \
      if (bit_size == 64) \
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityAtomicFloat64##NAME##EXT); \
   } while (0)

   CASE_ATOMIC_OP(fadd):
      ATOMIC_FCAP(Add);
      if (bit_size == 16)
         spirv_builder_emit_extension(&ctx->builder, "SPV_EXT_shader_atomic_float16_add");
      else
         spirv_builder_emit_extension(&ctx->builder, "SPV_EXT_shader_atomic_float_add");
      return SpvOpAtomicFAddEXT;
   CASE_ATOMIC_OP(fmax):
      ATOMIC_FCAP(MinMax);
      spirv_builder_emit_extension(&ctx->builder, "SPV_EXT_shader_atomic_float_min_max");
      return SpvOpAtomicFMaxEXT;
   CASE_ATOMIC_OP(fmin):
      ATOMIC_FCAP(MinMax);
      spirv_builder_emit_extension(&ctx->builder, "SPV_EXT_shader_atomic_float_min_max");
      return SpvOpAtomicFMinEXT;

   CASE_ATOMIC_OP(add):
      return SpvOpAtomicIAdd;
   CASE_ATOMIC_OP(umin):
      return SpvOpAtomicUMin;
   CASE_ATOMIC_OP(imin):
      return SpvOpAtomicSMin;
   CASE_ATOMIC_OP(umax):
      return SpvOpAtomicUMax;
   CASE_ATOMIC_OP(imax):
      return SpvOpAtomicSMax;
   CASE_ATOMIC_OP(and):
      return SpvOpAtomicAnd;
   CASE_ATOMIC_OP(or):
      return SpvOpAtomicOr;
   CASE_ATOMIC_OP(xor):
      return SpvOpAtomicXor;
   CASE_ATOMIC_OP(exchange):
      return SpvOpAtomicExchange;
   CASE_ATOMIC_OP(comp_swap):
      return SpvOpAtomicCompareExchange;
   default:
      debug_printf("%s - ", nir_intrinsic_infos[op].name);
      unreachable("unhandled atomic op");
   }
   return 0;
}

static bool
atomic_op_is_float(nir_intrinsic_op op)
{
   switch (op) {
   CASE_ATOMIC_OP(fadd):
   CASE_ATOMIC_OP(fmax):
   CASE_ATOMIC_OP(fmin):
      return true;
   default:
      break;
   }
   return false;
}
#undef CASE_ATOMIC_OP

static SpvId
emit_float_const(struct ntv_context *ctx, int bit_size, double value)
{
   assert(bit_size == 16 || bit_size == 32 || bit_size == 64);
   return spirv_builder_const_float(&ctx->builder, bit_size, value);
}

static SpvId
emit_uint_const(struct ntv_context *ctx, int bit_size, uint64_t value)
{
   assert(bit_size == 8 || bit_size == 16 || bit_size == 32 || bit_size == 64);
   return spirv_builder_const_uint(&ctx->builder, bit_size, value);
}

static SpvId
emit_int_const(struct ntv_context *ctx, int bit_size, int64_t value)
{
   assert(bit_size == 8 || bit_size == 16 || bit_size == 32 || bit_size == 64);
   return spirv_builder_const_int(&ctx->builder, bit_size, value);
}

static SpvId
get_fvec_type(struct ntv_context *ctx, unsigned bit_size, unsigned num_components)
{
   assert(bit_size == 16 || bit_size == 32 || bit_size == 64);

   SpvId float_type = spirv_builder_type_float(&ctx->builder, bit_size);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, float_type,
                                       num_components);

   assert(num_components == 1);
   return float_type;
}

static SpvId
get_ivec_type(struct ntv_context *ctx, unsigned bit_size, unsigned num_components)
{
   assert(bit_size == 8 || bit_size == 16 || bit_size == 32 || bit_size == 64);

   SpvId int_type = spirv_builder_type_int(&ctx->builder, bit_size);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, int_type,
                                       num_components);

   assert(num_components == 1);
   return int_type;
}

static SpvId
get_uvec_type(struct ntv_context *ctx, unsigned bit_size, unsigned num_components)
{
   assert(bit_size == 8 || bit_size == 16 || bit_size == 32 || bit_size == 64);

   SpvId uint_type = spirv_builder_type_uint(&ctx->builder, bit_size);
   if (num_components > 1)
      return spirv_builder_type_vector(&ctx->builder, uint_type,
                                       num_components);

   assert(num_components == 1);
   return uint_type;
}

static SpvStorageClass
get_storage_class(struct nir_variable *var)
{
   switch (var->data.mode) {
   case nir_var_function_temp:
      return SpvStorageClassFunction;
   case nir_var_mem_push_const:
      return SpvStorageClassPushConstant;
   case nir_var_shader_in:
      return SpvStorageClassInput;
   case nir_var_shader_out:
      return SpvStorageClassOutput;
   case nir_var_uniform:
   case nir_var_image:
      return SpvStorageClassUniformConstant;
   case nir_var_mem_ubo:
      return SpvStorageClassUniform;
   case nir_var_mem_ssbo:
      return SpvStorageClassStorageBuffer;
   default:
      unreachable("Unsupported nir_variable_mode");
   }
   return 0;
}

static SpvId
get_dest_uvec_type(struct ntv_context *ctx, nir_dest *dest)
{
   unsigned bit_size = nir_dest_bit_size(*dest);
   return get_uvec_type(ctx, bit_size, nir_dest_num_components(*dest));
}

static SpvId
get_glsl_basetype(struct ntv_context *ctx, enum glsl_base_type type)
{
   switch (type) {
   case GLSL_TYPE_BOOL:
      return spirv_builder_type_bool(&ctx->builder);

   case GLSL_TYPE_FLOAT16:
      return spirv_builder_type_float(&ctx->builder, 16);

   case GLSL_TYPE_FLOAT:
      return spirv_builder_type_float(&ctx->builder, 32);

   case GLSL_TYPE_INT:
      return spirv_builder_type_int(&ctx->builder, 32);

   case GLSL_TYPE_UINT:
      return spirv_builder_type_uint(&ctx->builder, 32);

   case GLSL_TYPE_DOUBLE:
      return spirv_builder_type_float(&ctx->builder, 64);

   case GLSL_TYPE_INT64:
      return spirv_builder_type_int(&ctx->builder, 64);

   case GLSL_TYPE_UINT64:
      return spirv_builder_type_uint(&ctx->builder, 64);

   case GLSL_TYPE_UINT16:
      return spirv_builder_type_uint(&ctx->builder, 16);
   case GLSL_TYPE_INT16:
      return spirv_builder_type_int(&ctx->builder, 16);
   case GLSL_TYPE_INT8:
      return spirv_builder_type_int(&ctx->builder, 8);
   case GLSL_TYPE_UINT8:
      return spirv_builder_type_uint(&ctx->builder, 8);

   default:
      unreachable("unknown GLSL type");
   }
}

static SpvId
get_glsl_type(struct ntv_context *ctx, const struct glsl_type *type)
{
   assert(type);
   if (glsl_type_is_scalar(type))
      return get_glsl_basetype(ctx, glsl_get_base_type(type));

   if (glsl_type_is_vector(type))
      return spirv_builder_type_vector(&ctx->builder,
         get_glsl_basetype(ctx, glsl_get_base_type(type)),
         glsl_get_vector_elements(type));

   if (glsl_type_is_matrix(type))
      return spirv_builder_type_matrix(&ctx->builder,
                                       spirv_builder_type_vector(&ctx->builder,
                                                                 get_glsl_basetype(ctx, glsl_get_base_type(type)),
                                                                 glsl_get_vector_elements(type)),
                                       glsl_get_matrix_columns(type));

   /* Aggregate types aren't cached in spirv_builder, so let's cache
    * them here instead.
    */

   struct hash_entry *entry =
      _mesa_hash_table_search(ctx->glsl_types, type);
   if (entry)
      return (SpvId)(uintptr_t)entry->data;

   SpvId ret;
   if (glsl_type_is_array(type)) {
      SpvId element_type = get_glsl_type(ctx, glsl_get_array_element(type));
      if (glsl_type_is_unsized_array(type))
         ret = spirv_builder_type_runtime_array(&ctx->builder, element_type);
      else
         ret = spirv_builder_type_array(&ctx->builder,
                                        element_type,
                                        emit_uint_const(ctx, 32, glsl_get_length(type)));
      uint32_t stride = glsl_get_explicit_stride(type);
      if (!stride && glsl_type_is_scalar(glsl_get_array_element(type))) {
         stride = MAX2(glsl_get_bit_size(glsl_get_array_element(type)) / 8, 1);
      }
      if (stride)
         spirv_builder_emit_array_stride(&ctx->builder, ret, stride);
   } else if (glsl_type_is_struct_or_ifc(type)) {
      const unsigned length = glsl_get_length(type);

      /* allocate some SpvId on the stack, falling back to the heap if the array is too long */
      SpvId *types, types_stack[16];

      if (length <= ARRAY_SIZE(types_stack)) {
         types = types_stack;
      } else {
         types = ralloc_array_size(ctx->mem_ctx, sizeof(SpvId), length);
         assert(types != NULL);
      }

      for (unsigned i = 0; i < glsl_get_length(type); i++)
         types[i] = get_glsl_type(ctx, glsl_get_struct_field(type, i));
      ret = spirv_builder_type_struct(&ctx->builder, types,
                                      glsl_get_length(type));
      for (unsigned i = 0; i < glsl_get_length(type); i++) {
         int32_t offset = glsl_get_struct_field_offset(type, i);
         if (offset >= 0)
            spirv_builder_emit_member_offset(&ctx->builder, ret, i, offset);
      }
   } else
      unreachable("Unhandled GLSL type");

   _mesa_hash_table_insert(ctx->glsl_types, type, (void *)(uintptr_t)ret);
   return ret;
}

static void
create_scratch_block(struct ntv_context *ctx, unsigned scratch_size, unsigned bit_size)
{
   unsigned idx = bit_size >> 4;
   SpvId type = spirv_builder_type_uint(&ctx->builder, bit_size);
   unsigned block_size = scratch_size / (bit_size / 8);
   assert(block_size);
   SpvId array = spirv_builder_type_array(&ctx->builder, type, emit_uint_const(ctx, 32, block_size));
   spirv_builder_emit_array_stride(&ctx->builder, array, bit_size / 8);
   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               SpvStorageClassPrivate,
                                               array);
   ctx->scratch_block_var[idx] = spirv_builder_emit_var(&ctx->builder, ptr_type, SpvStorageClassPrivate);
   if (ctx->spirv_1_4_interfaces) {
      assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
      ctx->entry_ifaces[ctx->num_entry_ifaces++] = ctx->scratch_block_var[idx];
   }
}

static SpvId
get_scratch_block(struct ntv_context *ctx, unsigned bit_size)
{
   unsigned idx = bit_size >> 4;
   if (!ctx->scratch_block_var[idx])
      create_scratch_block(ctx, ctx->nir->scratch_size, bit_size);
   return ctx->scratch_block_var[idx];
}

static void
create_shared_block(struct ntv_context *ctx, unsigned shared_size, unsigned bit_size)
{
   unsigned idx = bit_size >> 4;
   SpvId type = spirv_builder_type_uint(&ctx->builder, bit_size);
   unsigned block_size = shared_size / (bit_size / 8);
   assert(block_size);
   SpvId array = spirv_builder_type_array(&ctx->builder, type, emit_uint_const(ctx, 32, block_size));
   spirv_builder_emit_array_stride(&ctx->builder, array, bit_size / 8);
   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               SpvStorageClassWorkgroup,
                                               array);
   ctx->shared_block_var[idx] = spirv_builder_emit_var(&ctx->builder, ptr_type, SpvStorageClassWorkgroup);
   if (ctx->spirv_1_4_interfaces) {
      assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
      ctx->entry_ifaces[ctx->num_entry_ifaces++] = ctx->shared_block_var[idx];
   }
}

static SpvId
get_shared_block(struct ntv_context *ctx, unsigned bit_size)
{
   unsigned idx = bit_size >> 4;
   if (!ctx->shared_block_var[idx])
      create_shared_block(ctx, ctx->nir->info.shared_size, bit_size);
   return ctx->shared_block_var[idx];
}

#define HANDLE_EMIT_BUILTIN(SLOT, BUILTIN) \
      case VARYING_SLOT_##SLOT: \
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltIn##BUILTIN); \
         break


static SpvId
input_var_init(struct ntv_context *ctx, struct nir_variable *var)
{
   SpvId var_type = get_glsl_type(ctx, var->type);
   SpvStorageClass sc = get_storage_class(var);
   if (sc == SpvStorageClassPushConstant)
      spirv_builder_emit_decoration(&ctx->builder, var_type, SpvDecorationBlock);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   sc, var_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type, sc);

   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   if (var->data.mode == nir_var_mem_push_const) {
      ctx->push_const_var = var_id;

      if (ctx->spirv_1_4_interfaces) {
         assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
         ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
      }
   }
   return var_id;
}

static void
emit_interpolation(struct ntv_context *ctx, SpvId var_id,
                   enum glsl_interp_mode mode)
{
   switch (mode) {
   case INTERP_MODE_NONE:
   case INTERP_MODE_SMOOTH:
      /* XXX spirv doesn't seem to have anything for this */
      break;
   case INTERP_MODE_FLAT:
      spirv_builder_emit_decoration(&ctx->builder, var_id,
                                    SpvDecorationFlat);
      break;
   case INTERP_MODE_EXPLICIT:
      spirv_builder_emit_decoration(&ctx->builder, var_id,
                                    SpvDecorationExplicitInterpAMD);
      break;
   case INTERP_MODE_NOPERSPECTIVE:
      spirv_builder_emit_decoration(&ctx->builder, var_id,
                                    SpvDecorationNoPerspective);
      break;
   default:
      unreachable("unknown interpolation value");
   }
}

static void
emit_input(struct ntv_context *ctx, struct nir_variable *var)
{
   SpvId var_id = input_var_init(ctx, var);
   if (ctx->stage == MESA_SHADER_VERTEX)
      spirv_builder_emit_location(&ctx->builder, var_id,
                                  var->data.driver_location);
   else if (ctx->stage == MESA_SHADER_FRAGMENT) {
      switch (var->data.location) {
      HANDLE_EMIT_BUILTIN(POS, FragCoord);
      HANDLE_EMIT_BUILTIN(PNTC, PointCoord);
      HANDLE_EMIT_BUILTIN(LAYER, Layer);
      HANDLE_EMIT_BUILTIN(PRIMITIVE_ID, PrimitiveId);
      HANDLE_EMIT_BUILTIN(CLIP_DIST0, ClipDistance);
      HANDLE_EMIT_BUILTIN(CULL_DIST0, CullDistance);
      HANDLE_EMIT_BUILTIN(VIEWPORT, ViewportIndex);
      HANDLE_EMIT_BUILTIN(FACE, FrontFacing);

      default:
         spirv_builder_emit_location(&ctx->builder, var_id,
                                     var->data.driver_location);
      }
      if (var->data.centroid)
         spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationCentroid);
      else if (var->data.sample)
         spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationSample);
      emit_interpolation(ctx, var_id, var->data.interpolation);
   } else if (ctx->stage < MESA_SHADER_FRAGMENT) {
      switch (var->data.location) {
      HANDLE_EMIT_BUILTIN(POS, Position);
      HANDLE_EMIT_BUILTIN(PSIZ, PointSize);
      HANDLE_EMIT_BUILTIN(LAYER, Layer);
      HANDLE_EMIT_BUILTIN(PRIMITIVE_ID, PrimitiveId);
      HANDLE_EMIT_BUILTIN(CULL_DIST0, CullDistance);
      HANDLE_EMIT_BUILTIN(VIEWPORT, ViewportIndex);
      HANDLE_EMIT_BUILTIN(TESS_LEVEL_OUTER, TessLevelOuter);
      HANDLE_EMIT_BUILTIN(TESS_LEVEL_INNER, TessLevelInner);

      case VARYING_SLOT_CLIP_DIST0:
         assert(glsl_type_is_array(var->type));
         spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInClipDistance);
         break;

      default:
         spirv_builder_emit_location(&ctx->builder, var_id,
                                     var->data.driver_location);
      }
   }

   if (var->data.location_frac)
      spirv_builder_emit_component(&ctx->builder, var_id,
                                   var->data.location_frac);

   if (var->data.patch)
      spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationPatch);

   _mesa_hash_table_insert(ctx->vars, var, (void *)(intptr_t)var_id);

   assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
   ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
}

static void
emit_output(struct ntv_context *ctx, struct nir_variable *var)
{
   SpvId var_type = get_glsl_type(ctx, var->type);

   /* SampleMask is always an array in spirv */
   if (ctx->stage == MESA_SHADER_FRAGMENT && var->data.location == FRAG_RESULT_SAMPLE_MASK)
      ctx->sample_mask_type = var_type = spirv_builder_type_array(&ctx->builder, var_type, emit_uint_const(ctx, 32, 1));
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassOutput,
                                                   var_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassOutput);
   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   if (var->data.precision == GLSL_PRECISION_MEDIUM || var->data.precision == GLSL_PRECISION_LOW) {
      spirv_builder_emit_decoration(&ctx->builder, var_id,
                                    SpvDecorationRelaxedPrecision);
   }

   if (ctx->stage != MESA_SHADER_FRAGMENT) {
      switch (var->data.location) {
      HANDLE_EMIT_BUILTIN(POS, Position);
      HANDLE_EMIT_BUILTIN(PSIZ, PointSize);
      HANDLE_EMIT_BUILTIN(LAYER, Layer);
      HANDLE_EMIT_BUILTIN(PRIMITIVE_ID, PrimitiveId);
      HANDLE_EMIT_BUILTIN(CLIP_DIST0, ClipDistance);
      HANDLE_EMIT_BUILTIN(CULL_DIST0, CullDistance);
      HANDLE_EMIT_BUILTIN(VIEWPORT, ViewportIndex);
      HANDLE_EMIT_BUILTIN(TESS_LEVEL_OUTER, TessLevelOuter);
      HANDLE_EMIT_BUILTIN(TESS_LEVEL_INNER, TessLevelInner);

      default:
         /* non-xfb psiz output will have location -1 */
         if (var->data.location >= 0)
            spirv_builder_emit_location(&ctx->builder, var_id,
                                        var->data.driver_location);
      }
      /* tcs can't do xfb */
      if (ctx->stage != MESA_SHADER_TESS_CTRL && var->data.location >= 0) {
         unsigned idx = var->data.location << 2 | var->data.location_frac;
         ctx->outputs[idx] = var_id;
         ctx->so_output_gl_types[idx] = var->type;
         ctx->so_output_types[idx] = var_type;
      }
      emit_interpolation(ctx, var_id, var->data.interpolation);
   } else {
      if (var->data.location >= FRAG_RESULT_DATA0) {
         spirv_builder_emit_location(&ctx->builder, var_id,
                                     var->data.location - FRAG_RESULT_DATA0);
         spirv_builder_emit_index(&ctx->builder, var_id, var->data.index);
      } else {
         switch (var->data.location) {
         case FRAG_RESULT_COLOR:
            unreachable("gl_FragColor should be lowered by now");

         case FRAG_RESULT_DEPTH:
            spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInFragDepth);
            break;

         case FRAG_RESULT_SAMPLE_MASK:
            spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInSampleMask);
            break;

         case FRAG_RESULT_STENCIL:
            spirv_builder_emit_builtin(&ctx->builder, var_id, SpvBuiltInFragStencilRefEXT);
            break;

         default:
            spirv_builder_emit_location(&ctx->builder, var_id,
                                        var->data.location);
            spirv_builder_emit_index(&ctx->builder, var_id, var->data.index);
         }
      }
      if (var->data.sample)
         spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationSample);
   }

   if (var->data.location_frac)
      spirv_builder_emit_component(&ctx->builder, var_id,
                                   var->data.location_frac);

   if (var->data.patch)
      spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationPatch);

   if (var->data.explicit_xfb_buffer &&
       (!glsl_type_is_array(var->type) || glsl_array_size(var->type) == 1  || !glsl_type_is_interface(glsl_without_array(var->type)))) {
      spirv_builder_emit_offset(&ctx->builder, var_id, var->data.offset);
      spirv_builder_emit_xfb_buffer(&ctx->builder, var_id, var->data.xfb.buffer);
      spirv_builder_emit_xfb_stride(&ctx->builder, var_id, var->data.xfb.stride);
      if (var->data.stream)
         spirv_builder_emit_stream(&ctx->builder, var_id, var->data.stream);
   }

   _mesa_hash_table_insert(ctx->vars, var, (void *)(intptr_t)var_id);

   assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
   ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
}

static void
emit_temp(struct ntv_context *ctx, struct nir_variable *var)
{
   SpvId var_type = get_glsl_type(ctx, var->type);

   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassFunction,
                                                   var_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassFunction);
   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   _mesa_hash_table_insert(ctx->vars, var, (void *)(intptr_t)var_id);
}

static SpvDim
type_to_dim(enum glsl_sampler_dim gdim, bool *is_ms)
{
   *is_ms = false;
   switch (gdim) {
   case GLSL_SAMPLER_DIM_1D:
      return SpvDim1D;
   case GLSL_SAMPLER_DIM_2D:
      return SpvDim2D;
   case GLSL_SAMPLER_DIM_3D:
      return SpvDim3D;
   case GLSL_SAMPLER_DIM_CUBE:
      return SpvDimCube;
   case GLSL_SAMPLER_DIM_RECT:
      return SpvDim2D;
   case GLSL_SAMPLER_DIM_BUF:
      return SpvDimBuffer;
   case GLSL_SAMPLER_DIM_EXTERNAL:
      return SpvDim2D; /* seems dodgy... */
   case GLSL_SAMPLER_DIM_MS:
      *is_ms = true;
      return SpvDim2D;
   case GLSL_SAMPLER_DIM_SUBPASS_MS:
      *is_ms = true;
      return SpvDimSubpassData;
   case GLSL_SAMPLER_DIM_SUBPASS:
      return SpvDimSubpassData;
   default:
      fprintf(stderr, "unknown sampler type %d\n", gdim);
      break;
   }
   return SpvDim2D;
}

static inline SpvImageFormat
get_shader_image_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      return SpvImageFormatRgba32f;
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      return SpvImageFormatRgba16f;
   case PIPE_FORMAT_R32_FLOAT:
      return SpvImageFormatR32f;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      return SpvImageFormatRgba8;
   case PIPE_FORMAT_R8G8B8A8_SNORM:
      return SpvImageFormatRgba8Snorm;
   case PIPE_FORMAT_R32G32B32A32_SINT:
      return SpvImageFormatRgba32i;
   case PIPE_FORMAT_R16G16B16A16_SINT:
      return SpvImageFormatRgba16i;
   case PIPE_FORMAT_R8G8B8A8_SINT:
      return SpvImageFormatRgba8i;
   case PIPE_FORMAT_R32_SINT:
      return SpvImageFormatR32i;
   case PIPE_FORMAT_R32G32B32A32_UINT:
      return SpvImageFormatRgba32ui;
   case PIPE_FORMAT_R16G16B16A16_UINT:
      return SpvImageFormatRgba16ui;
   case PIPE_FORMAT_R8G8B8A8_UINT:
      return SpvImageFormatRgba8ui;
   case PIPE_FORMAT_R32_UINT:
      return SpvImageFormatR32ui;
   default:
      return SpvImageFormatUnknown;
   }
}

static inline SpvImageFormat
get_extended_image_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32G32_FLOAT:
      return SpvImageFormatRg32f;
   case PIPE_FORMAT_R16G16_FLOAT:
      return SpvImageFormatRg16f;
   case PIPE_FORMAT_R11G11B10_FLOAT:
      return SpvImageFormatR11fG11fB10f;
   case PIPE_FORMAT_R16_FLOAT:
      return SpvImageFormatR16f;
   case PIPE_FORMAT_R16G16B16A16_UNORM:
      return SpvImageFormatRgba16;
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      return SpvImageFormatRgb10A2;
   case PIPE_FORMAT_R16G16_UNORM:
      return SpvImageFormatRg16;
   case PIPE_FORMAT_R8G8_UNORM:
      return SpvImageFormatRg8;
   case PIPE_FORMAT_R16_UNORM:
      return SpvImageFormatR16;
   case PIPE_FORMAT_R8_UNORM:
      return SpvImageFormatR8;
   case PIPE_FORMAT_R16G16B16A16_SNORM:
      return SpvImageFormatRgba16Snorm;
   case PIPE_FORMAT_R16G16_SNORM:
      return SpvImageFormatRg16Snorm;
   case PIPE_FORMAT_R8G8_SNORM:
      return SpvImageFormatRg8Snorm;
   case PIPE_FORMAT_R16_SNORM:
      return SpvImageFormatR16Snorm;
   case PIPE_FORMAT_R8_SNORM:
      return SpvImageFormatR8Snorm;
   case PIPE_FORMAT_R32G32_SINT:
      return SpvImageFormatRg32i;
   case PIPE_FORMAT_R16G16_SINT:
      return SpvImageFormatRg16i;
   case PIPE_FORMAT_R8G8_SINT:
      return SpvImageFormatRg8i;
   case PIPE_FORMAT_R16_SINT:
      return SpvImageFormatR16i;
   case PIPE_FORMAT_R8_SINT:
      return SpvImageFormatR8i;
   case PIPE_FORMAT_R10G10B10A2_UINT:
      return SpvImageFormatRgb10a2ui;
   case PIPE_FORMAT_R32G32_UINT:
      return SpvImageFormatRg32ui;
   case PIPE_FORMAT_R16G16_UINT:
      return SpvImageFormatRg16ui;
   case PIPE_FORMAT_R8G8_UINT:
      return SpvImageFormatRg8ui;
   case PIPE_FORMAT_R16_UINT:
      return SpvImageFormatR16ui;
   case PIPE_FORMAT_R8_UINT:
      return SpvImageFormatR8ui;

   default:
      return SpvImageFormatUnknown;
   }
}

static inline SpvImageFormat
get_image_format(struct ntv_context *ctx, enum pipe_format format)
{
   /* always supported */
   if (format == PIPE_FORMAT_NONE)
      return SpvImageFormatUnknown;

   SpvImageFormat ret = get_shader_image_format(format);
   if (ret != SpvImageFormatUnknown) {
      /* requires the shader-cap, but we already emit that */
      return ret;
   }

   ret = get_extended_image_format(format);
   assert(ret != SpvImageFormatUnknown);
   spirv_builder_emit_cap(&ctx->builder,
                          SpvCapabilityStorageImageExtendedFormats);
   return ret;
}

static SpvId
get_bare_image_type(struct ntv_context *ctx, struct nir_variable *var, bool is_sampler)
{
   const struct glsl_type *type = glsl_without_array(var->type);

   bool is_ms;

   if (var->data.fb_fetch_output) {
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityInputAttachment);
   } else if (!is_sampler && !var->data.image.format) {
      if (!(var->data.access & ACCESS_NON_WRITEABLE))
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityStorageImageWriteWithoutFormat);
      if (!(var->data.access & ACCESS_NON_READABLE))
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityStorageImageReadWithoutFormat);
   }

   SpvDim dimension = type_to_dim(glsl_get_sampler_dim(type), &is_ms);
   if (dimension == SpvDim1D) {
      if (is_sampler)
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilitySampled1D);
      else
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImage1D);
   }
   if (dimension == SpvDimBuffer) {
      if (is_sampler)
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageBuffer);
      else
         spirv_builder_emit_cap(&ctx->builder, SpvCapabilitySampledBuffer);
   }

   bool arrayed = glsl_sampler_type_is_array(type);
   if (dimension == SpvDimCube && arrayed)
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageCubeArray);

   SpvId result_type = get_glsl_basetype(ctx, glsl_get_sampler_result_type(type));
   return spirv_builder_type_image(&ctx->builder, result_type,
                                               dimension, false,
                                               arrayed,
                                               is_ms, is_sampler ? 1 : 2,
                                               get_image_format(ctx, var->data.image.format));
}

static SpvId
get_image_type(struct ntv_context *ctx, struct nir_variable *var, bool is_sampler)
{
   SpvId image_type = get_bare_image_type(ctx, var, is_sampler);
   return is_sampler && ctx->stage != MESA_SHADER_KERNEL ? spirv_builder_type_sampled_image(&ctx->builder, image_type) : image_type;
}

static SpvId
emit_image(struct ntv_context *ctx, struct nir_variable *var, SpvId image_type, bool bindless)
{
   if (var->data.bindless)
      return 0;
   const struct glsl_type *type = glsl_without_array(var->type);

   bool is_sampler = glsl_type_is_sampler(type);
   SpvId var_type = is_sampler && ctx->stage != MESA_SHADER_KERNEL ? spirv_builder_type_sampled_image(&ctx->builder, image_type) : image_type;
   bool mediump = (var->data.precision == GLSL_PRECISION_MEDIUM || var->data.precision == GLSL_PRECISION_LOW);

   int index = var->data.driver_location;
   assert(!is_sampler || !ctx->sampler_types[index]);
   assert(is_sampler || !ctx->image_types[index]);

   if (!bindless && glsl_type_is_array(var->type)) {
      var_type = spirv_builder_type_array(&ctx->builder, var_type,
                                              emit_uint_const(ctx, 32, glsl_get_aoa_size(var->type)));
      spirv_builder_emit_array_stride(&ctx->builder, var_type, sizeof(void*));
   }
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassUniformConstant,
                                                   var_type);

   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassUniformConstant);

   if (mediump) {
      spirv_builder_emit_decoration(&ctx->builder, var_id,
                                    SpvDecorationRelaxedPrecision);
   }

   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   if (var->data.fb_fetch_output)
      spirv_builder_emit_input_attachment_index(&ctx->builder, var_id, var->data.index);

   if (bindless)
      return var_id;

   _mesa_hash_table_insert(ctx->vars, var, (void *)(intptr_t)var_id);
   if (is_sampler) {
      ctx->sampler_types[index] = image_type;
      ctx->samplers[index] = var_id;
   } else {
      ctx->image_types[index] = image_type;
      ctx->images[index] = var_id;
      emit_access_decorations(ctx, var, var_id);
   }
   if (ctx->spirv_1_4_interfaces) {
      assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
      ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
   }

   spirv_builder_emit_descriptor_set(&ctx->builder, var_id, var->data.descriptor_set);
   spirv_builder_emit_binding(&ctx->builder, var_id, var->data.binding);
   return var_id;
}

static void
emit_sampler(struct ntv_context *ctx, unsigned sampler_index, unsigned desc_set)
{
   SpvId type = spirv_builder_type_sampler(&ctx->builder);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassUniformConstant,
                                                   type);

   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         SpvStorageClassUniformConstant);
   char buf[128];
   snprintf(buf, sizeof(buf), "sampler_%u", sampler_index);
   spirv_builder_emit_name(&ctx->builder, var_id, buf);
   spirv_builder_emit_descriptor_set(&ctx->builder, var_id, desc_set);
   spirv_builder_emit_binding(&ctx->builder, var_id, sampler_index);
   ctx->cl_samplers[sampler_index] = var_id;
   if (ctx->spirv_1_4_interfaces) {
      assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
      ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
   }

}

static SpvId
get_sized_uint_array_type(struct ntv_context *ctx, unsigned array_size, unsigned bitsize)
{
   SpvId array_length = emit_uint_const(ctx, 32, array_size);
   SpvId array_type = spirv_builder_type_array(&ctx->builder, get_uvec_type(ctx, bitsize, 1),
                                            array_length);
   spirv_builder_emit_array_stride(&ctx->builder, array_type, bitsize / 8);
   return array_type;
}

/* get array<struct(array_type <--this one)> */
static SpvId
get_bo_array_type(struct ntv_context *ctx, struct nir_variable *var)
{
   struct hash_entry *he = _mesa_hash_table_search(ctx->bo_array_types, var);
   if (he)
      return (SpvId)(uintptr_t)he->data;
   unsigned bitsize = glsl_get_bit_size(glsl_get_array_element(glsl_get_struct_field(glsl_without_array(var->type), 0)));
   assert(bitsize);
   SpvId array_type;
   const struct glsl_type *type = glsl_without_array(var->type);
   const struct glsl_type *first_type = glsl_get_struct_field(type, 0);
   if (!glsl_type_is_unsized_array(first_type)) {
      uint32_t array_size = glsl_get_length(first_type);
      assert(array_size);
      return get_sized_uint_array_type(ctx, array_size, bitsize);
   }
   SpvId uint_type = spirv_builder_type_uint(&ctx->builder, bitsize);
   array_type = spirv_builder_type_runtime_array(&ctx->builder, uint_type);
   spirv_builder_emit_array_stride(&ctx->builder, array_type, bitsize / 8);
   return array_type;
}

/* get array<struct(array_type) <--this one> */
static SpvId
get_bo_struct_type(struct ntv_context *ctx, struct nir_variable *var)
{
   struct hash_entry *he = _mesa_hash_table_search(ctx->bo_struct_types, var);
   if (he)
      return (SpvId)(uintptr_t)he->data;
   const struct glsl_type *bare_type = glsl_without_array(var->type);
   unsigned bitsize = glsl_get_bit_size(glsl_get_array_element(glsl_get_struct_field(bare_type, 0)));
   SpvId array_type = get_bo_array_type(ctx, var);
   _mesa_hash_table_insert(ctx->bo_array_types, var, (void *)(uintptr_t)array_type);
   bool ssbo = var->data.mode == nir_var_mem_ssbo;

   // wrap UBO-array in a struct
   SpvId runtime_array = 0;
   if (ssbo && glsl_get_length(bare_type) > 1) {
       const struct glsl_type *last_member = glsl_get_struct_field(bare_type, glsl_get_length(bare_type) - 1);
       if (glsl_type_is_unsized_array(last_member)) {
          runtime_array = spirv_builder_type_runtime_array(&ctx->builder, get_uvec_type(ctx, bitsize, 1));
          spirv_builder_emit_array_stride(&ctx->builder, runtime_array, glsl_get_explicit_stride(last_member));
       }
   }
   SpvId types[] = {array_type, runtime_array};
   SpvId struct_type = spirv_builder_type_struct(&ctx->builder, types, 1 + !!runtime_array);
   if (var->name) {
      char struct_name[100];
      snprintf(struct_name, sizeof(struct_name), "struct_%s", var->name);
      spirv_builder_emit_name(&ctx->builder, struct_type, struct_name);
   }

   spirv_builder_emit_decoration(&ctx->builder, struct_type,
                                 SpvDecorationBlock);
   spirv_builder_emit_member_offset(&ctx->builder, struct_type, 0, 0);
   if (runtime_array)
      spirv_builder_emit_member_offset(&ctx->builder, struct_type, 1, 0);

   return struct_type;
}

static void
emit_bo(struct ntv_context *ctx, struct nir_variable *var, bool aliased)
{
   unsigned bitsize = glsl_get_bit_size(glsl_get_array_element(glsl_get_struct_field(glsl_without_array(var->type), 0)));
   bool ssbo = var->data.mode == nir_var_mem_ssbo;
   SpvId struct_type = get_bo_struct_type(ctx, var);
   _mesa_hash_table_insert(ctx->bo_struct_types, var, (void *)(uintptr_t)struct_type);
   SpvId array_length = emit_uint_const(ctx, 32, glsl_get_length(var->type));
   SpvId array_type = spirv_builder_type_array(&ctx->builder, struct_type, array_length);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   ssbo ? SpvStorageClassStorageBuffer : SpvStorageClassUniform,
                                                   array_type);
   SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                         ssbo ? SpvStorageClassStorageBuffer : SpvStorageClassUniform);
   if (var->name)
      spirv_builder_emit_name(&ctx->builder, var_id, var->name);

   if (aliased)
      spirv_builder_emit_decoration(&ctx->builder, var_id, SpvDecorationAliased);

   unsigned idx = bitsize >> 4;
   assert(idx < ARRAY_SIZE(ctx->ssbos));
   if (ssbo) {
      assert(!ctx->ssbos[idx]);
      ctx->ssbos[idx] = var_id;
      if (bitsize == 32)
         ctx->ssbo_vars = var;
   } else {
      assert(!ctx->ubos[var->data.driver_location][idx]);
      ctx->ubos[var->data.driver_location][idx] = var_id;
      ctx->ubo_vars[var->data.driver_location] = var;
   }
   if (ctx->spirv_1_4_interfaces) {
      assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
      ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
   }
   _mesa_hash_table_insert(ctx->vars, var, (void *)(intptr_t)var_id);

   spirv_builder_emit_descriptor_set(&ctx->builder, var_id, var->data.descriptor_set);
   spirv_builder_emit_binding(&ctx->builder, var_id, var->data.binding);
}

static SpvId
get_vec_from_bit_size(struct ntv_context *ctx, uint32_t bit_size, uint32_t num_components)
{
   if (bit_size == 1)
      return get_bvec_type(ctx, num_components);
   return get_uvec_type(ctx, bit_size, num_components);
}

static SpvId
get_src_ssa(struct ntv_context *ctx, const nir_ssa_def *ssa)
{
   assert(ssa->index < ctx->num_defs);
   assert(ctx->defs[ssa->index] != 0);
   return ctx->defs[ssa->index];
}

static SpvId
get_var_from_reg(struct ntv_context *ctx, nir_register *reg)
{
   assert(reg->index < ctx->num_regs);
   assert(ctx->regs[reg->index] != 0);
   return ctx->regs[reg->index];
}

static SpvId
get_src_reg(struct ntv_context *ctx, const nir_reg_src *reg)
{
   assert(reg->reg);
   assert(!reg->indirect);
   assert(!reg->base_offset);

   SpvId var = get_var_from_reg(ctx, reg->reg);
   SpvId type = get_vec_from_bit_size(ctx, reg->reg->bit_size, reg->reg->num_components);
   return spirv_builder_emit_load(&ctx->builder, type, var);
}

static SpvId
get_src(struct ntv_context *ctx, nir_src *src)
{
   if (src->is_ssa)
      return get_src_ssa(ctx, src->ssa);
   else
      return get_src_reg(ctx, &src->reg);
}

static SpvId
get_alu_src_raw(struct ntv_context *ctx, nir_alu_instr *alu, unsigned src)
{
   assert(!alu->src[src].negate);
   assert(!alu->src[src].abs);

   SpvId def = get_src(ctx, &alu->src[src].src);

   unsigned used_channels = 0;
   bool need_swizzle = false;
   for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++) {
      if (!nir_alu_instr_channel_used(alu, src, i))
         continue;

      used_channels++;

      if (alu->src[src].swizzle[i] != i)
         need_swizzle = true;
   }
   assert(used_channels != 0);

   unsigned live_channels = nir_src_num_components(alu->src[src].src);
   if (used_channels != live_channels)
      need_swizzle = true;

   if (!need_swizzle)
      return def;

   int bit_size = nir_src_bit_size(alu->src[src].src);
   SpvId raw_type = bit_size == 1 ? spirv_builder_type_bool(&ctx->builder) :
                                    spirv_builder_type_uint(&ctx->builder, bit_size);

   if (used_channels == 1) {
      uint32_t indices[] =  { alu->src[src].swizzle[0] };
      return spirv_builder_emit_composite_extract(&ctx->builder, raw_type,
                                                  def, indices,
                                                  ARRAY_SIZE(indices));
   } else if (live_channels == 1) {
      SpvId raw_vec_type = spirv_builder_type_vector(&ctx->builder,
                                                     raw_type,
                                                     used_channels);

      SpvId constituents[NIR_MAX_VEC_COMPONENTS] = {0};
      for (unsigned i = 0; i < used_channels; ++i)
        constituents[i] = def;

      return spirv_builder_emit_composite_construct(&ctx->builder,
                                                    raw_vec_type,
                                                    constituents,
                                                    used_channels);
   } else {
      SpvId raw_vec_type = spirv_builder_type_vector(&ctx->builder,
                                                     raw_type,
                                                     used_channels);

      uint32_t components[NIR_MAX_VEC_COMPONENTS] = {0};
      size_t num_components = 0;
      for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; i++) {
         if (!nir_alu_instr_channel_used(alu, src, i))
            continue;

         components[num_components++] = alu->src[src].swizzle[i];
      }

      return spirv_builder_emit_vector_shuffle(&ctx->builder, raw_vec_type,
                                               def, def, components,
                                               num_components);
   }
}

static void
store_ssa_def(struct ntv_context *ctx, nir_ssa_def *ssa, SpvId result)
{
   assert(result != 0);
   assert(ssa->index < ctx->num_defs);
   ctx->defs[ssa->index] = result;
}

static SpvId
emit_select(struct ntv_context *ctx, SpvId type, SpvId cond,
            SpvId if_true, SpvId if_false)
{
   return emit_triop(ctx, SpvOpSelect, type, cond, if_true, if_false);
}

static SpvId
emit_bitcast(struct ntv_context *ctx, SpvId type, SpvId value)
{
   return emit_unop(ctx, SpvOpBitcast, type, value);
}

static SpvId
bitcast_to_uvec(struct ntv_context *ctx, SpvId value, unsigned bit_size,
                unsigned num_components)
{
   SpvId type = get_uvec_type(ctx, bit_size, num_components);
   return emit_bitcast(ctx, type, value);
}

static SpvId
bitcast_to_ivec(struct ntv_context *ctx, SpvId value, unsigned bit_size,
                unsigned num_components)
{
   SpvId type = get_ivec_type(ctx, bit_size, num_components);
   return emit_bitcast(ctx, type, value);
}

static SpvId
bitcast_to_fvec(struct ntv_context *ctx, SpvId value, unsigned bit_size,
               unsigned num_components)
{
   SpvId type = get_fvec_type(ctx, bit_size, num_components);
   return emit_bitcast(ctx, type, value);
}

static void
store_reg_def(struct ntv_context *ctx, nir_reg_dest *reg, SpvId result)
{
   SpvId var = get_var_from_reg(ctx, reg->reg);
   assert(var);
   spirv_builder_emit_store(&ctx->builder, var, result);
}

static void
store_dest_raw(struct ntv_context *ctx, nir_dest *dest, SpvId result)
{
   if (dest->is_ssa)
      store_ssa_def(ctx, &dest->ssa, result);
   else
      store_reg_def(ctx, &dest->reg, result);
}

static SpvId
store_dest(struct ntv_context *ctx, nir_dest *dest, SpvId result, nir_alu_type type)
{
   unsigned num_components = nir_dest_num_components(*dest);
   unsigned bit_size = nir_dest_bit_size(*dest);

   if (bit_size != 1) {
      switch (nir_alu_type_get_base_type(type)) {
      case nir_type_bool:
         assert("bool should have bit-size 1");
         break;

      case nir_type_uint:
      case nir_type_uint8:
      case nir_type_uint16:
      case nir_type_uint64:
         break; /* nothing to do! */

      case nir_type_int:
      case nir_type_int8:
      case nir_type_int16:
      case nir_type_int64:
      case nir_type_float:
      case nir_type_float16:
      case nir_type_float64:
         result = bitcast_to_uvec(ctx, result, bit_size, num_components);
         break;

      default:
         unreachable("unsupported nir_alu_type");
      }
   }

   store_dest_raw(ctx, dest, result);
   return result;
}

static SpvId
emit_unop(struct ntv_context *ctx, SpvOp op, SpvId type, SpvId src)
{
   return spirv_builder_emit_unop(&ctx->builder, op, type, src);
}

/* return the intended xfb output vec type based on base type and vector size */
static SpvId
get_output_type(struct ntv_context *ctx, unsigned register_index, unsigned num_components)
{
   const struct glsl_type *out_type = NULL;
   /* index is based on component, so we might have to go back a few slots to get to the base */
   while (!out_type)
      out_type = ctx->so_output_gl_types[register_index--];
   const struct glsl_type *bare_type = glsl_without_array(out_type);
   enum glsl_base_type base_type;
   if (glsl_type_is_struct_or_ifc(bare_type))
      base_type = GLSL_TYPE_UINT;
   else
      base_type = glsl_get_base_type(bare_type);


   switch (base_type) {
   case GLSL_TYPE_BOOL:
      return get_bvec_type(ctx, num_components);

   case GLSL_TYPE_DOUBLE: //this case is misleading, as so outputs are always 32bit floats
   case GLSL_TYPE_FLOAT:
      return get_fvec_type(ctx, 32, num_components);

   case GLSL_TYPE_INT:
      return get_ivec_type(ctx, 32, num_components);

   case GLSL_TYPE_UINT:
      return get_uvec_type(ctx, 32, num_components);

   default:
      unreachable("unknown type");
      break;
   }
   return 0;
}

static nir_variable *
find_propagate_var(nir_shader *nir, unsigned slot)
{
   nir_foreach_shader_out_variable(var, nir) {
      if (var->data.location == slot && glsl_type_is_array(var->type))
         return var;
   }
   return NULL;
}

/* for streamout create new outputs, as streamout can be done on individual components,
   from complete outputs, so we just can't use the created packed outputs */
static void
emit_so_info(struct ntv_context *ctx, const struct zink_shader_info *so_info,
             unsigned first_so)
{
   unsigned output = 0;
   for (unsigned i = 0; i < so_info->so_info.num_outputs; i++) {
      struct pipe_stream_output so_output = so_info->so_info.output[i];
      unsigned slot = so_info->so_info_slots[i] << 2 | so_output.start_component;
      SpvId out_type = get_output_type(ctx, slot, so_output.num_components);
      SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                      SpvStorageClassOutput,
                                                      out_type);
      SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                            SpvStorageClassOutput);
      char name[10];

      snprintf(name, 10, "xfb%d", output);
      spirv_builder_emit_name(&ctx->builder, var_id, name);
      spirv_builder_emit_offset(&ctx->builder, var_id, (so_output.dst_offset * 4));
      spirv_builder_emit_xfb_buffer(&ctx->builder, var_id, so_output.output_buffer);
      spirv_builder_emit_xfb_stride(&ctx->builder, var_id, so_info->so_info.stride[so_output.output_buffer] * 4);
      if (so_output.stream)
         spirv_builder_emit_stream(&ctx->builder, var_id, so_output.stream);

      /* output location is incremented by VARYING_SLOT_VAR0 for non-builtins in vtn,
       * so we need to ensure that the new xfb location slot doesn't conflict with any previously-emitted
       * outputs.
       */
      uint32_t location = first_so + i;
      assert(location < VARYING_SLOT_VAR0);
      spirv_builder_emit_location(&ctx->builder, var_id, location);

      /* note: gl_ClipDistance[4] can the 0-indexed member of VARYING_SLOT_CLIP_DIST1 here,
       * so this is still the 0 component
       */
      if (so_output.start_component)
         spirv_builder_emit_component(&ctx->builder, var_id, so_output.start_component);

      uint32_t *key = ralloc_size(ctx->mem_ctx, sizeof(uint32_t));
      *key = (uint32_t)so_output.register_index << 2 | so_output.start_component;
      _mesa_hash_table_insert(ctx->so_outputs, key, (void *)(intptr_t)var_id);

      assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
      ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
      output += align(so_output.num_components, 4) / 4;
   }

   /* these are interface block arrays which need to be split
    * across N buffers due to GL spec requirements
    */
   u_foreach_bit(bit, so_info->so_propagate) {
      unsigned slot = bit + VARYING_SLOT_VAR0;
      nir_variable *var = find_propagate_var(ctx->nir, slot);
      assert(var);
      const struct glsl_type *bare_type = glsl_without_array(var->type);
      SpvId base_type = get_glsl_type(ctx, bare_type);
      for (unsigned i = 0; i < glsl_array_size(var->type); i++) {
         SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                         SpvStorageClassOutput,
                                                         base_type);
         SpvId var_id = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                               SpvStorageClassOutput);
         char name[1024];
         if (var->name)
            snprintf(name, sizeof(name), "xfb_%s[%d]", var->name, i);
         else
            snprintf(name, sizeof(name), "xfb_slot%u[%d]", slot, i);
         spirv_builder_emit_name(&ctx->builder, var_id, name);
         spirv_builder_emit_offset(&ctx->builder, var_id, var->data.offset);
         spirv_builder_emit_xfb_buffer(&ctx->builder, var_id, var->data.xfb.buffer + i);
         spirv_builder_emit_xfb_stride(&ctx->builder, var_id, var->data.xfb.stride);
         if (var->data.stream)
            spirv_builder_emit_stream(&ctx->builder, var_id, var->data.stream);

         uint32_t location = first_so + so_info->so_info.num_outputs + i;
         assert(location < VARYING_SLOT_VAR0);
         spirv_builder_emit_location(&ctx->builder, var_id, location);

         uint32_t *key = ralloc_size(ctx->mem_ctx, sizeof(uint32_t));
         *key = (uint32_t)(slot + i) << 2;
         _mesa_hash_table_insert(ctx->so_outputs, key, (void *)(intptr_t)var_id);

         assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
         ctx->entry_ifaces[ctx->num_entry_ifaces++] = var_id;
      }
   }
}

static const struct glsl_type *
unroll_struct_type(struct ntv_context *ctx, const struct glsl_type *slot_type, unsigned *slot_idx, SpvId *deref, const struct glsl_type **arraytype)
{
   const struct glsl_type *type = slot_type;
   unsigned slot_count = 0;
   unsigned cur_slot = 0;
   unsigned idx = 0;
   /* iterate over all the members in the struct, stopping once the slot idx is reached */
   for (unsigned i = 0; i < glsl_get_length(slot_type) && cur_slot <= *slot_idx; i++, cur_slot += slot_count) {
      /* use array type for slot counting but return array member type for unroll */
      *arraytype = glsl_get_struct_field(slot_type, i);
      type = glsl_without_array(*arraytype);
      slot_count = glsl_count_vec4_slots(*arraytype, false, false);
      idx = i;
   }
   *deref = spirv_builder_emit_composite_extract(&ctx->builder, get_glsl_type(ctx, glsl_get_struct_field(slot_type, idx)), *deref, &idx, 1);
   *slot_idx -= (cur_slot - slot_count);
   return type;
}

static void
emit_so_outputs(struct ntv_context *ctx,
                const struct zink_shader_info *so_info)
{
   for (unsigned i = 0; i < so_info->so_info.num_outputs; i++) {
      uint32_t components[NIR_MAX_VEC_COMPONENTS];
      unsigned slot = so_info->so_info_slots[i];
      struct pipe_stream_output so_output = so_info->so_info.output[i];
      uint32_t so_key = (uint32_t) so_output.register_index << 2 | so_output.start_component;
      uint32_t output_location = (uint32_t) slot << 2 | so_output.start_component;
      uint32_t location = output_location;
      struct hash_entry *he = _mesa_hash_table_search(ctx->so_outputs, &so_key);
      assert(he);
      SpvId so_output_var_id = (SpvId)(intptr_t)he->data;

      SpvId type = get_output_type(ctx, location, so_output.num_components);
      SpvId output = 0;
      /* index is based on component, so we might have to go back a few slots to get to the base */
      UNUSED uint32_t orig_location = location;
      while (!output)
         output = ctx->outputs[location--];
      location++;
      SpvId output_type = ctx->so_output_types[location];
      const struct glsl_type *out_type = ctx->so_output_gl_types[location];

      SpvId src = spirv_builder_emit_load(&ctx->builder, output_type, output);

      SpvId result;

      /* this is the type being indexed into */
      const struct glsl_type *bare_type = glsl_without_array(out_type);
      /* this is the array index into matrix types */
      unsigned matrix_offset = glsl_type_is_matrix(bare_type) ? 0 : so_output.register_index;
      do {
         uint32_t base_slot = (location & ~so_output.start_component) / 4;
         /* this is the slot index into the "current" value */
         unsigned slot_idx = slot - base_slot;
         unsigned struct_slots = glsl_count_vec4_slots(bare_type, false, false);
         unsigned array_idx = slot_idx / struct_slots;
         if (glsl_type_is_struct_or_ifc(bare_type)) {
            bool first = true;
            slot_idx %= glsl_count_vec4_slots(bare_type, false, false);
            if (glsl_type_is_array(out_type))
               src = spirv_builder_emit_composite_extract(&ctx->builder, get_glsl_type(ctx, bare_type), src, &array_idx, 1);
            /* need to find the vec4 that's being exported by this slot */
            while (glsl_type_is_struct_or_ifc(bare_type)) {
               /* a struct may have nested arrays of structs: handle them inline here */
               if (!first && glsl_type_is_array(out_type)) {
                  struct_slots = glsl_count_vec4_slots(bare_type, false, false);
                  array_idx = slot_idx / struct_slots;
                  src = spirv_builder_emit_composite_extract(&ctx->builder, get_glsl_type(ctx, bare_type), src, &array_idx, 1);
                  slot_idx -= array_idx * struct_slots;
               }
               /* unroll this level of struct:
                * - slot_idx is incremented to reflect the current value
                * - unwrap src
                * - out_type is the array type if src is an array
                */
               bare_type = unroll_struct_type(ctx, bare_type, &slot_idx, &src, &out_type);
               first = false;
            }
         }
         /* update to the matrix row index */
         matrix_offset = slot_idx;
         output_type = get_glsl_type(ctx, out_type);
         if (glsl_type_is_vector_or_scalar(out_type)) {
            /* this is a simple case: handle below */
            if (glsl_get_vector_elements(out_type) * glsl_get_bit_size(out_type) == so_output.num_components * 32) {
               src = emit_bitcast(ctx, type, src);
               out_type = glsl_vector_type(GLSL_TYPE_UINT, so_output.num_components);
               output_type = get_glsl_type(ctx, out_type);
            }
         } else if (glsl_type_is_array(out_type)) {
             /* this should be impossible */
             if (glsl_type_is_struct(bare_type))
                unreachable("zink: gross nested struct array struct arrays in xfb!");
             if (glsl_type_is_matrix(bare_type)) {
                /* nested matrix type: unwrap, update matrix offset, select a vec, handle below */
                unsigned mat_slots = glsl_count_attribute_slots(bare_type, false);
                array_idx = matrix_offset / mat_slots;
                output_type = get_glsl_type(ctx, bare_type);
                out_type = bare_type;
                src = spirv_builder_emit_composite_extract(&ctx->builder, output_type, src, &array_idx, 1);
                matrix_offset %= mat_slots;
                unsigned real_offset = glsl_type_is_64bit(bare_type) ? matrix_offset / 2 : matrix_offset;
                /* store for later */
                if (glsl_type_is_64bit(bare_type))
                   matrix_offset %= 2;
                assert(real_offset < glsl_get_matrix_columns(bare_type));
                out_type = glsl_without_array_or_matrix(out_type);
                output_type = get_glsl_type(ctx, out_type);
                src = spirv_builder_emit_composite_extract(&ctx->builder, output_type, src, &real_offset, 1);
                break;
             } else if (glsl_type_is_vector(bare_type)) {
                /* just extract the right vec and let it be handled below */
                unsigned vec_slots = glsl_count_attribute_slots(bare_type, false);
                unsigned idx = matrix_offset / vec_slots;
                matrix_offset %= vec_slots;
                output_type = get_glsl_type(ctx, bare_type);
                out_type = bare_type;
                src = spirv_builder_emit_composite_extract(&ctx->builder, output_type, src, &idx, 1);
                break;
             } else {
                assert(glsl_type_is_scalar(bare_type));
                break;
             }
             if (so_output.num_components > 1)
                src = spirv_builder_emit_composite_construct(&ctx->builder, type, components, so_output.num_components);
             else
                src = components[0];
             out_type = glsl_vector_type(GLSL_TYPE_UINT, so_output.num_components);
             output_type = type;
         }
      } while (0);
      assert(!glsl_type_is_struct_or_ifc(out_type));

      if (!glsl_type_is_64bit(out_type) &&
          (glsl_type_is_scalar(out_type) ||
           (type == output_type &&
            (glsl_type_is_vector(out_type) && glsl_get_vector_elements(out_type) == so_output.num_components))))
         /* if we're emitting a scalar or the type we're emitting matches the output's original type and we're
          * emitting the same number of components, then we can skip any sort of conversion here
          */
         result = src;
      else {
         /* OpCompositeExtract can only extract scalars for our use here,
          * but not from arrays since they have different packing rules
          */
         if (so_output.num_components == 1 && !glsl_type_is_array(out_type)) {
            unsigned component = so_output.start_component;
            result = spirv_builder_emit_composite_extract(&ctx->builder, type, src, &component, so_output.num_components);
         } else if (glsl_type_is_vector(out_type)) {
            if (glsl_type_is_64bit(out_type)) {
               /* 64bit components count as 2 so outputs: bitcast to vec2 and extract */
               unsigned idx = 0;
               for (unsigned c = 0; idx < so_output.num_components; c++) {
                  uint32_t member = so_output.start_component + (matrix_offset * 2) + c;
                  SpvId base_type = get_glsl_basetype(ctx, glsl_get_base_type(out_type));
                  SpvId conv = spirv_builder_emit_composite_extract(&ctx->builder, base_type, src, &member, 1);
                  SpvId val = emit_bitcast(ctx, get_uvec_type(ctx, 32, 2), conv);
                  unsigned v = 0;
                  components[idx++] = spirv_builder_emit_composite_extract(&ctx->builder, get_uvec_type(ctx, 32, 1), val, &v, 1);
                  v = 1;
                  components[idx++] = spirv_builder_emit_composite_extract(&ctx->builder, get_uvec_type(ctx, 32, 1), val, &v, 1);
               }
               result = spirv_builder_emit_composite_construct(&ctx->builder, get_uvec_type(ctx, 32, so_output.num_components), components, so_output.num_components);
            } else {
               for (unsigned c = 0; c < so_output.num_components; c++) {
                  components[c] = so_output.start_component + c;
                  /* this is the second half of a 2 * vec4 array */
                  if (slot == VARYING_SLOT_CLIP_DIST1 || slot == VARYING_SLOT_CULL_DIST1)
                     components[c] += 4;
               }
               /* OpVectorShuffle can select vector members into a differently-sized vector */
               result = spirv_builder_emit_vector_shuffle(&ctx->builder, type,
                                                                src, src,
                                                                components, so_output.num_components);
            }
         } else {
             assert(glsl_type_is_array_or_matrix(out_type));
             const struct glsl_type *bare_type = glsl_without_array(out_type);
             assert(!glsl_type_is_struct_or_ifc(bare_type));
             if (glsl_type_is_matrix(out_type)) {
                /* for matrices, the xfb output will never be more than one vec4 from a single row */
                unsigned vec_size = glsl_get_vector_elements(out_type);
                SpvId vec_type = get_fvec_type(ctx, glsl_get_bit_size(out_type), vec_size);
                if (glsl_type_is_64bit(out_type) && vec_size > 2) {
                   /* dvec3/dvec4 uses 2 slots per row: normalize matrix offset */
                   matrix_offset /= 2;
                }
                src = spirv_builder_emit_composite_extract(&ctx->builder, vec_type, src, &matrix_offset, 1);
                out_type = glsl_vector_type(glsl_get_base_type(out_type), glsl_get_vector_elements(out_type));
             }
             /* for arrays (or matrix rows), we need to manually extract each desired member
              * and re-pack them into the desired output type
              */
             unsigned idx = 0;
             for (unsigned c = 0; idx < so_output.num_components; c++) {
                uint32_t member = so_output.start_component + c;
                SpvId base_type = get_glsl_basetype(ctx, glsl_get_base_type(bare_type));

                if (slot == VARYING_SLOT_CLIP_DIST1 || slot == VARYING_SLOT_CULL_DIST1)
                   member += 4;
                components[idx] = spirv_builder_emit_composite_extract(&ctx->builder, base_type, src, &member, 1);
                if (glsl_type_is_64bit(bare_type)) {
                   /* 64bit components count as 2 so outputs: bitcast to vec2 and extract */
                   SpvId val = emit_bitcast(ctx, get_uvec_type(ctx, 32, 2), components[idx]);
                   unsigned v = 0;
                   components[idx++] = spirv_builder_emit_composite_extract(&ctx->builder, get_uvec_type(ctx, 32, 1), val, &v, 1);
                   v = 1;
                   components[idx++] = spirv_builder_emit_composite_extract(&ctx->builder, get_uvec_type(ctx, 32, 1), val, &v, 1);
                } else {
                   components[idx] = emit_bitcast(ctx, spirv_builder_type_uint(&ctx->builder, 32), components[idx]);
                   idx++;
                }
             }
             if (so_output.num_components > 1)
                result = spirv_builder_emit_composite_construct(&ctx->builder, get_uvec_type(ctx, 32, so_output.num_components), components, so_output.num_components);
             else
                result = components[0];
         }
      }

      result = emit_bitcast(ctx, type, result);
      spirv_builder_emit_store(&ctx->builder, so_output_var_id, result);
   }

   u_foreach_bit(bit, so_info->so_propagate) {
      unsigned slot = bit + VARYING_SLOT_VAR0;
      nir_variable *var = find_propagate_var(ctx->nir, slot);
      assert(var);

      const struct glsl_type *bare_type = glsl_without_array(var->type);
      SpvId base_type = get_glsl_type(ctx, bare_type);
      SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                      SpvStorageClassOutput,
                                                      base_type);
      SpvId output = ctx->outputs[slot << 2];
      assert(output);
      for (unsigned i = 0; i < glsl_array_size(var->type); i++) {
         uint32_t so_key = (uint32_t) (slot + i) << 2;
         struct hash_entry *he = _mesa_hash_table_search(ctx->so_outputs, &so_key);
         assert(he);
         SpvId so_output_var_id = (SpvId)(intptr_t)he->data;

         SpvId idx = emit_uint_const(ctx, 32, i);
         SpvId deref = spirv_builder_emit_access_chain(&ctx->builder, pointer_type, output, &idx, 1);
         SpvId load = spirv_builder_emit_load(&ctx->builder, base_type, deref);
         spirv_builder_emit_store(&ctx->builder, so_output_var_id, load);
      }
   }
}

static SpvId
emit_atomic(struct ntv_context *ctx, SpvId op, SpvId type, SpvId src0, SpvId src1, SpvId src2)
{
   if (op == SpvOpAtomicLoad)
      return spirv_builder_emit_triop(&ctx->builder, op, type, src0, emit_uint_const(ctx, 32, SpvScopeDevice),
                                       emit_uint_const(ctx, 32, 0));
   if (op == SpvOpAtomicCompareExchange)
      return spirv_builder_emit_hexop(&ctx->builder, op, type, src0, emit_uint_const(ctx, 32, SpvScopeDevice),
                                       emit_uint_const(ctx, 32, 0),
                                       emit_uint_const(ctx, 32, 0),
                                       /* these params are intentionally swapped */
                                       src2, src1);

   return spirv_builder_emit_quadop(&ctx->builder, op, type, src0, emit_uint_const(ctx, 32, SpvScopeDevice),
                                    emit_uint_const(ctx, 32, 0), src1);
}

static SpvId
emit_binop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1)
{
   return spirv_builder_emit_binop(&ctx->builder, op, type, src0, src1);
}

static SpvId
emit_triop(struct ntv_context *ctx, SpvOp op, SpvId type,
           SpvId src0, SpvId src1, SpvId src2)
{
   return spirv_builder_emit_triop(&ctx->builder, op, type, src0, src1, src2);
}

static SpvId
emit_builtin_unop(struct ntv_context *ctx, enum GLSLstd450 op, SpvId type,
                  SpvId src)
{
   SpvId args[] = { src };
   return spirv_builder_emit_ext_inst(&ctx->builder, type, ctx->GLSL_std_450,
                                      op, args, ARRAY_SIZE(args));
}

static SpvId
emit_builtin_binop(struct ntv_context *ctx, enum GLSLstd450 op, SpvId type,
                   SpvId src0, SpvId src1)
{
   SpvId args[] = { src0, src1 };
   return spirv_builder_emit_ext_inst(&ctx->builder, type, ctx->GLSL_std_450,
                                      op, args, ARRAY_SIZE(args));
}

static SpvId
emit_builtin_triop(struct ntv_context *ctx, enum GLSLstd450 op, SpvId type,
                   SpvId src0, SpvId src1, SpvId src2)
{
   SpvId args[] = { src0, src1, src2 };
   return spirv_builder_emit_ext_inst(&ctx->builder, type, ctx->GLSL_std_450,
                                      op, args, ARRAY_SIZE(args));
}

static SpvId
get_fvec_constant(struct ntv_context *ctx, unsigned bit_size,
                  unsigned num_components, double value)
{
   assert(bit_size == 16 || bit_size == 32 || bit_size == 64);

   SpvId result = emit_float_const(ctx, bit_size, value);
   if (num_components == 1)
      return result;

   assert(num_components > 1);
   SpvId components[NIR_MAX_VEC_COMPONENTS];
   for (int i = 0; i < num_components; i++)
      components[i] = result;

   SpvId type = get_fvec_type(ctx, bit_size, num_components);
   return spirv_builder_const_composite(&ctx->builder, type, components,
                                        num_components);
}

static SpvId
get_ivec_constant(struct ntv_context *ctx, unsigned bit_size,
                  unsigned num_components, int64_t value)
{
   assert(bit_size == 8 || bit_size == 16 || bit_size == 32 || bit_size == 64);

   SpvId result = emit_int_const(ctx, bit_size, value);
   if (num_components == 1)
      return result;

   assert(num_components > 1);
   SpvId components[NIR_MAX_VEC_COMPONENTS];
   for (int i = 0; i < num_components; i++)
      components[i] = result;

   SpvId type = get_ivec_type(ctx, bit_size, num_components);
   return spirv_builder_const_composite(&ctx->builder, type, components,
                                        num_components);
}

static inline unsigned
alu_instr_src_components(const nir_alu_instr *instr, unsigned src)
{
   if (nir_op_infos[instr->op].input_sizes[src] > 0)
      return nir_op_infos[instr->op].input_sizes[src];

   if (instr->dest.dest.is_ssa)
      return instr->dest.dest.ssa.num_components;
   else
      return instr->dest.dest.reg.reg->num_components;
}

static SpvId
get_alu_src(struct ntv_context *ctx, nir_alu_instr *alu, unsigned src, SpvId *raw_value)
{
   *raw_value = get_alu_src_raw(ctx, alu, src);

   unsigned num_components = alu_instr_src_components(alu, src);
   unsigned bit_size = nir_src_bit_size(alu->src[src].src);
   nir_alu_type type = nir_op_infos[alu->op].input_types[src];

   if (bit_size == 1)
      return *raw_value;
   else {
      switch (nir_alu_type_get_base_type(type)) {
      case nir_type_bool:
         unreachable("bool should have bit-size 1");

      case nir_type_int:
         return bitcast_to_ivec(ctx, *raw_value, bit_size, num_components);

      case nir_type_uint:
         return *raw_value;

      case nir_type_float:
         return bitcast_to_fvec(ctx, *raw_value, bit_size, num_components);

      default:
         unreachable("unknown nir_alu_type");
      }
   }
}

static SpvId
store_alu_result(struct ntv_context *ctx, nir_alu_instr *alu, SpvId result, bool force_float)
{
   assert(!alu->dest.saturate);
   return store_dest(ctx, &alu->dest.dest, result,
                     force_float ? nir_type_float : nir_op_infos[alu->op].output_type);
}

static SpvId
get_alu_type(struct ntv_context *ctx, nir_alu_type type, unsigned num_components, unsigned bit_size)
{
   if (bit_size == 1)
      return get_bvec_type(ctx, num_components);

   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_bool:
      unreachable("bool should have bit-size 1");

   case nir_type_int:
   case nir_type_int8:
   case nir_type_int16:
   case nir_type_int64:
      return get_ivec_type(ctx, bit_size, num_components);

   case nir_type_uint:
   case nir_type_uint8:
   case nir_type_uint16:
   case nir_type_uint64:
      return get_uvec_type(ctx, bit_size, num_components);

   case nir_type_float:
   case nir_type_float16:
   case nir_type_float64:
      return get_fvec_type(ctx, bit_size, num_components);

   default:
      unreachable("unsupported nir_alu_type");
   }
}

static SpvId
get_dest_type(struct ntv_context *ctx, nir_dest *dest, nir_alu_type type)
{
   unsigned num_components = nir_dest_num_components(*dest);
   unsigned bit_size = nir_dest_bit_size(*dest);
   return get_alu_type(ctx, type, num_components, bit_size);
}

static bool
needs_derivative_control(nir_alu_instr *alu)
{
   switch (alu->op) {
   case nir_op_fddx_coarse:
   case nir_op_fddx_fine:
   case nir_op_fddy_coarse:
   case nir_op_fddy_fine:
      return true;

   default:
      return false;
   }
}

static void
emit_alu(struct ntv_context *ctx, nir_alu_instr *alu)
{
   SpvId src[NIR_MAX_VEC_COMPONENTS];
   SpvId raw_src[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      src[i] = get_alu_src(ctx, alu, i, &raw_src[i]);

   SpvId dest_type = get_dest_type(ctx, &alu->dest.dest,
                                   nir_op_infos[alu->op].output_type);
   bool force_float = false;
   unsigned bit_size = nir_dest_bit_size(alu->dest.dest);
   unsigned num_components = nir_dest_num_components(alu->dest.dest);

   if (needs_derivative_control(alu))
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityDerivativeControl);

   SpvId result = 0;
   switch (alu->op) {
   case nir_op_mov:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = src[0];
      break;

#define UNOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 1); \
      result = emit_unop(ctx, spirv_op, dest_type, src[0]); \
      break;

   UNOP(nir_op_ineg, SpvOpSNegate)
   UNOP(nir_op_fneg, SpvOpFNegate)
   UNOP(nir_op_fddx, SpvOpDPdx)
   UNOP(nir_op_fddx_coarse, SpvOpDPdxCoarse)
   UNOP(nir_op_fddx_fine, SpvOpDPdxFine)
   UNOP(nir_op_fddy, SpvOpDPdy)
   UNOP(nir_op_fddy_coarse, SpvOpDPdyCoarse)
   UNOP(nir_op_fddy_fine, SpvOpDPdyFine)
   UNOP(nir_op_f2i8, SpvOpConvertFToS)
   UNOP(nir_op_f2u8, SpvOpConvertFToU)
   UNOP(nir_op_f2i16, SpvOpConvertFToS)
   UNOP(nir_op_f2u16, SpvOpConvertFToU)
   UNOP(nir_op_f2i32, SpvOpConvertFToS)
   UNOP(nir_op_f2u32, SpvOpConvertFToU)
   UNOP(nir_op_i2f16, SpvOpConvertSToF)
   UNOP(nir_op_i2f32, SpvOpConvertSToF)
   UNOP(nir_op_u2f16, SpvOpConvertUToF)
   UNOP(nir_op_u2f32, SpvOpConvertUToF)
   UNOP(nir_op_i2i8, SpvOpSConvert)
   UNOP(nir_op_i2i16, SpvOpSConvert)
   UNOP(nir_op_i2i32, SpvOpSConvert)
   UNOP(nir_op_u2u8, SpvOpUConvert)
   UNOP(nir_op_u2u16, SpvOpUConvert)
   UNOP(nir_op_u2u32, SpvOpUConvert)
   UNOP(nir_op_f2f16, SpvOpFConvert)
   UNOP(nir_op_f2f32, SpvOpFConvert)
   UNOP(nir_op_f2i64, SpvOpConvertFToS)
   UNOP(nir_op_f2u64, SpvOpConvertFToU)
   UNOP(nir_op_u2f64, SpvOpConvertUToF)
   UNOP(nir_op_i2f64, SpvOpConvertSToF)
   UNOP(nir_op_i2i64, SpvOpSConvert)
   UNOP(nir_op_u2u64, SpvOpUConvert)
   UNOP(nir_op_f2f64, SpvOpFConvert)
   UNOP(nir_op_bitfield_reverse, SpvOpBitReverse)
   UNOP(nir_op_bit_count, SpvOpBitCount)
#undef UNOP

   case nir_op_f2f16_rtz:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_unop(ctx, SpvOpFConvert, dest_type, src[0]);
      spirv_builder_emit_rounding_mode(&ctx->builder, result, SpvFPRoundingModeRTZ);
      break;

   case nir_op_inot:
      if (bit_size == 1)
         result = emit_unop(ctx, SpvOpLogicalNot, dest_type, src[0]);
      else
         result = emit_unop(ctx, SpvOpNot, dest_type, src[0]);
      break;

   case nir_op_b2i8:
   case nir_op_b2i16:
   case nir_op_b2i32:
   case nir_op_b2i64:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_select(ctx, dest_type, src[0],
                           get_ivec_constant(ctx, bit_size, num_components, 1),
                           get_ivec_constant(ctx, bit_size, num_components, 0));
      break;

   case nir_op_b2f16:
   case nir_op_b2f32:
   case nir_op_b2f64:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_select(ctx, dest_type, src[0],
                           get_fvec_constant(ctx, bit_size, num_components, 1),
                           get_fvec_constant(ctx, bit_size, num_components, 0));
      break;

#define BUILTIN_UNOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 1); \
      result = emit_builtin_unop(ctx, spirv_op, dest_type, src[0]); \
      break;

#define BUILTIN_UNOPF(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 1); \
      result = emit_builtin_unop(ctx, spirv_op, get_dest_type(ctx, &alu->dest.dest, nir_type_float), src[0]); \
      force_float = true; \
      break;

   BUILTIN_UNOP(nir_op_iabs, GLSLstd450SAbs)
   BUILTIN_UNOP(nir_op_fabs, GLSLstd450FAbs)
   BUILTIN_UNOP(nir_op_fsqrt, GLSLstd450Sqrt)
   BUILTIN_UNOP(nir_op_frsq, GLSLstd450InverseSqrt)
   BUILTIN_UNOP(nir_op_flog2, GLSLstd450Log2)
   BUILTIN_UNOP(nir_op_fexp2, GLSLstd450Exp2)
   BUILTIN_UNOP(nir_op_ffract, GLSLstd450Fract)
   BUILTIN_UNOP(nir_op_ffloor, GLSLstd450Floor)
   BUILTIN_UNOP(nir_op_fceil, GLSLstd450Ceil)
   BUILTIN_UNOP(nir_op_ftrunc, GLSLstd450Trunc)
   BUILTIN_UNOP(nir_op_fround_even, GLSLstd450RoundEven)
   BUILTIN_UNOP(nir_op_fsign, GLSLstd450FSign)
   BUILTIN_UNOP(nir_op_isign, GLSLstd450SSign)
   BUILTIN_UNOP(nir_op_fsin, GLSLstd450Sin)
   BUILTIN_UNOP(nir_op_fcos, GLSLstd450Cos)
   BUILTIN_UNOP(nir_op_ufind_msb, GLSLstd450FindUMsb)
   BUILTIN_UNOP(nir_op_find_lsb, GLSLstd450FindILsb)
   BUILTIN_UNOP(nir_op_ifind_msb, GLSLstd450FindSMsb)

   case nir_op_pack_half_2x16:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_builtin_unop(ctx, GLSLstd450PackHalf2x16, get_dest_type(ctx, &alu->dest.dest, nir_type_uint), src[0]);
      break;

   case nir_op_unpack_64_2x32:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_builtin_unop(ctx, GLSLstd450UnpackDouble2x32, get_dest_type(ctx, &alu->dest.dest, nir_type_uint), src[0]);
      break;

   BUILTIN_UNOPF(nir_op_unpack_half_2x16, GLSLstd450UnpackHalf2x16)
   BUILTIN_UNOPF(nir_op_pack_64_2x32, GLSLstd450PackDouble2x32)
#undef BUILTIN_UNOP
#undef BUILTIN_UNOPF

   case nir_op_frcp:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_binop(ctx, SpvOpFDiv, dest_type,
                          get_fvec_constant(ctx, bit_size, num_components, 1),
                          src[0]);
      break;

   case nir_op_f2b1:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_binop(ctx, SpvOpFOrdNotEqual, dest_type, src[0],
                          get_fvec_constant(ctx,
                                            nir_src_bit_size(alu->src[0].src),
                                            num_components, 0));
      break;
   case nir_op_i2b1:
      assert(nir_op_infos[alu->op].num_inputs == 1);
      result = emit_binop(ctx, SpvOpINotEqual, dest_type, src[0],
                          get_ivec_constant(ctx,
                                            nir_src_bit_size(alu->src[0].src),
                                            num_components, 0));
      break;


#define BINOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 2); \
      result = emit_binop(ctx, spirv_op, dest_type, src[0], src[1]); \
      break;

   BINOP(nir_op_iadd, SpvOpIAdd)
   BINOP(nir_op_isub, SpvOpISub)
   BINOP(nir_op_imul, SpvOpIMul)
   BINOP(nir_op_idiv, SpvOpSDiv)
   BINOP(nir_op_udiv, SpvOpUDiv)
   BINOP(nir_op_umod, SpvOpUMod)
   BINOP(nir_op_imod, SpvOpSMod)
   BINOP(nir_op_irem, SpvOpSRem)
   BINOP(nir_op_fadd, SpvOpFAdd)
   BINOP(nir_op_fsub, SpvOpFSub)
   BINOP(nir_op_fmul, SpvOpFMul)
   BINOP(nir_op_fdiv, SpvOpFDiv)
   BINOP(nir_op_fmod, SpvOpFMod)
   BINOP(nir_op_ilt, SpvOpSLessThan)
   BINOP(nir_op_ige, SpvOpSGreaterThanEqual)
   BINOP(nir_op_ult, SpvOpULessThan)
   BINOP(nir_op_uge, SpvOpUGreaterThanEqual)
   BINOP(nir_op_flt, SpvOpFOrdLessThan)
   BINOP(nir_op_fge, SpvOpFOrdGreaterThanEqual)
   BINOP(nir_op_frem, SpvOpFRem)
#undef BINOP

#define BINOP_LOG(nir_op, spv_op, spv_log_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 2); \
      if (nir_src_bit_size(alu->src[0].src) == 1) \
         result = emit_binop(ctx, spv_log_op, dest_type, src[0], src[1]); \
      else \
         result = emit_binop(ctx, spv_op, dest_type, src[0], src[1]); \
      break;

   BINOP_LOG(nir_op_iand, SpvOpBitwiseAnd, SpvOpLogicalAnd)
   BINOP_LOG(nir_op_ior, SpvOpBitwiseOr, SpvOpLogicalOr)
   BINOP_LOG(nir_op_ieq, SpvOpIEqual, SpvOpLogicalEqual)
   BINOP_LOG(nir_op_ine, SpvOpINotEqual, SpvOpLogicalNotEqual)
   BINOP_LOG(nir_op_ixor, SpvOpBitwiseXor, SpvOpLogicalNotEqual)
#undef BINOP_LOG

#define BINOP_SHIFT(nir_op, spirv_op) \
   case nir_op: { \
      assert(nir_op_infos[alu->op].num_inputs == 2); \
      int shift_bit_size = nir_src_bit_size(alu->src[1].src); \
      nir_alu_type shift_nir_type = nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[1]); \
      SpvId shift_type = get_alu_type(ctx, shift_nir_type, num_components, shift_bit_size); \
      SpvId shift_mask = get_ivec_constant(ctx, shift_bit_size, num_components, bit_size - 1); \
      SpvId shift_count = emit_binop(ctx, SpvOpBitwiseAnd, shift_type, src[1], shift_mask); \
      result = emit_binop(ctx, spirv_op, dest_type, src[0], shift_count); \
      break; \
   }

   BINOP_SHIFT(nir_op_ishl, SpvOpShiftLeftLogical)
   BINOP_SHIFT(nir_op_ishr, SpvOpShiftRightArithmetic)
   BINOP_SHIFT(nir_op_ushr, SpvOpShiftRightLogical)
#undef BINOP_SHIFT

#define BUILTIN_BINOP(nir_op, spirv_op) \
   case nir_op: \
      assert(nir_op_infos[alu->op].num_inputs == 2); \
      result = emit_builtin_binop(ctx, spirv_op, dest_type, src[0], src[1]); \
      break;

   BUILTIN_BINOP(nir_op_fmin, GLSLstd450FMin)
   BUILTIN_BINOP(nir_op_fmax, GLSLstd450FMax)
   BUILTIN_BINOP(nir_op_imin, GLSLstd450SMin)
   BUILTIN_BINOP(nir_op_imax, GLSLstd450SMax)
   BUILTIN_BINOP(nir_op_umin, GLSLstd450UMin)
   BUILTIN_BINOP(nir_op_umax, GLSLstd450UMax)
#undef BUILTIN_BINOP

   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
      assert(nir_op_infos[alu->op].num_inputs == 2);
      result = emit_binop(ctx, SpvOpDot, dest_type, src[0], src[1]);
      break;

   case nir_op_fdph:
   case nir_op_seq:
   case nir_op_sne:
   case nir_op_slt:
   case nir_op_sge:
      unreachable("should already be lowered away");

   case nir_op_fneu:
      assert(nir_op_infos[alu->op].num_inputs == 2);
      if (raw_src[0] == raw_src[1])
         result =  emit_unop(ctx, SpvOpIsNan, dest_type, src[0]);
      else
         result = emit_binop(ctx, SpvOpFUnordNotEqual, dest_type, src[0], src[1]);
      break;

   case nir_op_feq:
      assert(nir_op_infos[alu->op].num_inputs == 2);
      if (raw_src[0] == raw_src[1])
         result =  emit_unop(ctx, SpvOpLogicalNot, dest_type,
                             emit_unop(ctx, SpvOpIsNan, dest_type, src[0]));
      else
         result = emit_binop(ctx, SpvOpFOrdEqual, dest_type, src[0], src[1]);
      break;

   case nir_op_flrp:
      assert(nir_op_infos[alu->op].num_inputs == 3);
      result = emit_builtin_triop(ctx, GLSLstd450FMix, dest_type,
                                  src[0], src[1], src[2]);
      break;

   case nir_op_bcsel:
      assert(nir_op_infos[alu->op].num_inputs == 3);
      result = emit_select(ctx, dest_type, src[0], src[1], src[2]);
      break;

   case nir_op_pack_half_2x16_split: {
      SpvId fvec = spirv_builder_emit_composite_construct(&ctx->builder, get_fvec_type(ctx, 32, 2),
                                                          src, 2);
      result = emit_builtin_unop(ctx, GLSLstd450PackHalf2x16, dest_type, fvec);
      break;
   }
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4: {
      int num_inputs = nir_op_infos[alu->op].num_inputs;
      assert(2 <= num_inputs && num_inputs <= 4);
      result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type,
                                                      src, num_inputs);
   }
   break;

   case nir_op_ubitfield_extract:
      assert(nir_op_infos[alu->op].num_inputs == 3);
      result = emit_triop(ctx, SpvOpBitFieldUExtract, dest_type, src[0], src[1], src[2]);
      break;

   case nir_op_ibitfield_extract:
      assert(nir_op_infos[alu->op].num_inputs == 3);
      result = emit_triop(ctx, SpvOpBitFieldSExtract, dest_type, src[0], src[1], src[2]);
      break;

   case nir_op_bitfield_insert:
      assert(nir_op_infos[alu->op].num_inputs == 4);
      result = spirv_builder_emit_quadop(&ctx->builder, SpvOpBitFieldInsert, dest_type, src[0], src[1], src[2], src[3]);
      break;

   case nir_op_pack_32_2x16_split:
   case nir_op_pack_64_2x32_split: {
      nir_alu_type type = nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[0]);
      if (num_components <= 2) {
         SpvId components[] = {src[0], src[1]};
         SpvId vec_type = get_alu_type(ctx, type, num_components * 2, nir_src_bit_size(alu->src[0].src));
         result = spirv_builder_emit_composite_construct(&ctx->builder, vec_type, components, 2);
         result = emit_bitcast(ctx, dest_type, result);
      } else {
         SpvId components[NIR_MAX_VEC_COMPONENTS];
         SpvId conv_type = get_alu_type(ctx, type, 1, nir_src_bit_size(alu->src[0].src));
         SpvId vec_type = get_alu_type(ctx, type, 2, nir_src_bit_size(alu->src[0].src));
         SpvId dest_scalar_type = get_alu_type(ctx, nir_op_infos[alu->op].output_type, 1, bit_size);
         for (unsigned i = 0; i < nir_src_num_components(alu->src[0].src); i++) {
            SpvId conv[2];
            conv[0] = spirv_builder_emit_composite_extract(&ctx->builder, conv_type, src[0], &i, 1);
            conv[1] = spirv_builder_emit_composite_extract(&ctx->builder, conv_type, src[1], &i, 1);
            SpvId vec = spirv_builder_emit_composite_construct(&ctx->builder, vec_type, conv, 2);
            components[i] = emit_bitcast(ctx, dest_scalar_type, vec);
         }
         result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type, components, num_components);
      }
      break;
   }

   case nir_op_unpack_32_2x16_split_x:
   case nir_op_unpack_64_2x32_split_x: {
      nir_alu_type type = nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[0]);
      SpvId vec_type = get_alu_type(ctx, type, 2, bit_size);
      unsigned idx = 0;
      if (num_components == 1) {
         SpvId vec = emit_bitcast(ctx, vec_type, src[0]);
         result = spirv_builder_emit_composite_extract(&ctx->builder, dest_type, vec, &idx, 1);
      } else {
         SpvId components[NIR_MAX_VEC_COMPONENTS];
         for (unsigned i = 0; i < nir_src_num_components(alu->src[0].src); i++) {
            SpvId conv = spirv_builder_emit_composite_extract(&ctx->builder, get_alu_type(ctx, type, 1, nir_src_bit_size(alu->src[0].src)), src[0], &i, 1);
            conv = emit_bitcast(ctx, vec_type, conv);
            SpvId conv_type = get_alu_type(ctx, type, 1, bit_size);
            components[i] = spirv_builder_emit_composite_extract(&ctx->builder, conv_type, conv, &idx, 1);
         }
         result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type, components, num_components);
      }
      break;
   }

   case nir_op_unpack_32_2x16_split_y:
   case nir_op_unpack_64_2x32_split_y: {
      nir_alu_type type = nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[0]);
      SpvId vec_type = get_alu_type(ctx, type, 2, bit_size);
      unsigned idx = 1;
      if (num_components == 1) {
         SpvId vec = emit_bitcast(ctx, vec_type, src[0]);
         result = spirv_builder_emit_composite_extract(&ctx->builder, dest_type, vec, &idx, 1);
      } else {
         SpvId components[NIR_MAX_VEC_COMPONENTS];
         for (unsigned i = 0; i < nir_src_num_components(alu->src[0].src); i++) {
            SpvId conv = spirv_builder_emit_composite_extract(&ctx->builder, get_alu_type(ctx, type, 1, nir_src_bit_size(alu->src[0].src)), src[0], &i, 1);
            conv = emit_bitcast(ctx, vec_type, conv);
            SpvId conv_type = get_alu_type(ctx, type, 1, bit_size);
            components[i] = spirv_builder_emit_composite_extract(&ctx->builder, conv_type, conv, &idx, 1);
         }
         result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type, components, num_components);
      }
      break;
   }

   default:
      fprintf(stderr, "emit_alu: not implemented (%s)\n",
              nir_op_infos[alu->op].name);

      unreachable("unsupported opcode");
      return;
   }
   if (alu->exact)
      spirv_builder_emit_decoration(&ctx->builder, result, SpvDecorationNoContraction);

   store_alu_result(ctx, alu, result, force_float);
}

static void
emit_load_const(struct ntv_context *ctx, nir_load_const_instr *load_const)
{
   unsigned bit_size = load_const->def.bit_size;
   unsigned num_components = load_const->def.num_components;

   SpvId components[NIR_MAX_VEC_COMPONENTS];
   if (bit_size == 1) {
      for (int i = 0; i < num_components; i++)
         components[i] = spirv_builder_const_bool(&ctx->builder,
                                                  load_const->value[i].b);
   } else {
      for (int i = 0; i < num_components; i++) {
         uint64_t tmp = nir_const_value_as_uint(load_const->value[i],
                                                bit_size);
         components[i] = emit_uint_const(ctx, bit_size, tmp);
      }
   }

   if (num_components > 1) {
      SpvId type = get_vec_from_bit_size(ctx, bit_size,
                                         num_components);
      SpvId value = spirv_builder_const_composite(&ctx->builder,
                                                  type, components,
                                                  num_components);
      store_ssa_def(ctx, &load_const->def, value);
   } else {
      assert(num_components == 1);
      store_ssa_def(ctx, &load_const->def, components[0]);
   }
}

static void
emit_discard(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   assert(ctx->discard_func);
   SpvId type_void = spirv_builder_type_void(&ctx->builder);
   spirv_builder_function_call(&ctx->builder, type_void,
                               ctx->discard_func, NULL, 0);
}

static void
emit_load_deref(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId ptr = get_src(ctx, intr->src);

   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   SpvId type;
   if (glsl_type_is_image(deref->type)) {
      nir_variable *var = nir_deref_instr_get_variable(deref);
      type = get_image_type(ctx, var, glsl_type_is_sampler(glsl_without_array(var->type)));
   } else {
      type = get_glsl_type(ctx, deref->type);
   }
   SpvId result;

   if (nir_intrinsic_access(intr) & ACCESS_COHERENT)
      result = emit_atomic(ctx, SpvOpAtomicLoad, type, ptr, 0, 0);
   else
      result = spirv_builder_emit_load(&ctx->builder, type, ptr);
   unsigned num_components = nir_dest_num_components(intr->dest);
   unsigned bit_size = nir_dest_bit_size(intr->dest);
   result = bitcast_to_uvec(ctx, result, bit_size, num_components);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_store_deref(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId ptr = get_src(ctx, &intr->src[0]);
   SpvId src = get_src(ctx, &intr->src[1]);

   const struct glsl_type *gtype = nir_src_as_deref(intr->src[0])->type;
   SpvId type = get_glsl_type(ctx, gtype);
   nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
   unsigned wrmask = nir_intrinsic_write_mask(intr);
   if (!glsl_type_is_scalar(gtype) &&
       wrmask != BITFIELD_MASK(glsl_type_is_array(gtype) ? glsl_get_aoa_size(gtype) : glsl_get_vector_elements(gtype))) {
      /* no idea what we do if this fails */
      assert(glsl_type_is_array(gtype) || glsl_type_is_vector(gtype));

      /* this is a partial write, so we have to loop and do a per-component write */
      SpvId result_type;
      SpvId member_type;
      if (glsl_type_is_vector(gtype)) {
         result_type = get_glsl_basetype(ctx, glsl_get_base_type(gtype));
         member_type = get_uvec_type(ctx, glsl_get_bit_size(gtype), 1);
      } else
         member_type = result_type = get_glsl_type(ctx, glsl_get_array_element(gtype));
      SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                                  get_storage_class(var),
                                                  result_type);
      for (unsigned i = 0; i < 4; i++)
         if (wrmask & BITFIELD_BIT(i)) {
            SpvId idx = emit_uint_const(ctx, 32, i);
            SpvId val = spirv_builder_emit_composite_extract(&ctx->builder, member_type, src, &i, 1);
            val = emit_bitcast(ctx, result_type, val);
            SpvId member = spirv_builder_emit_access_chain(&ctx->builder, ptr_type,
                                                           ptr, &idx, 1);
            spirv_builder_emit_store(&ctx->builder, member, val);
         }
      return;

   }
   SpvId result;
   if (ctx->stage == MESA_SHADER_FRAGMENT &&
       var->data.mode == nir_var_shader_out &&
       var->data.location == FRAG_RESULT_SAMPLE_MASK) {
      src = emit_bitcast(ctx, type, src);
      /* SampleMask is always an array in spirv, so we need to construct it into one */
      result = spirv_builder_emit_composite_construct(&ctx->builder, ctx->sample_mask_type, &src, 1);
   } else
      result = emit_bitcast(ctx, type, src);
   if (nir_intrinsic_access(intr) & ACCESS_COHERENT)
      spirv_builder_emit_atomic_store(&ctx->builder, ptr, SpvScopeDevice, 0, result);
   else
      spirv_builder_emit_store(&ctx->builder, ptr, result);
}

static void
emit_load_shared(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId dest_type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   unsigned num_components = nir_dest_num_components(intr->dest);
   unsigned bit_size = nir_dest_bit_size(intr->dest);
   SpvId uint_type = get_uvec_type(ctx, bit_size, 1);
   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               SpvStorageClassWorkgroup,
                                               uint_type);
   SpvId offset = get_src(ctx, &intr->src[0]);
   SpvId constituents[NIR_MAX_VEC_COMPONENTS];
   SpvId shared_block = get_shared_block(ctx, bit_size);
   /* need to convert array -> vec */
   for (unsigned i = 0; i < num_components; i++) {
      SpvId member = spirv_builder_emit_access_chain(&ctx->builder, ptr_type,
                                                     shared_block, &offset, 1);
      constituents[i] = spirv_builder_emit_load(&ctx->builder, uint_type, member);
      offset = emit_binop(ctx, SpvOpIAdd, spirv_builder_type_uint(&ctx->builder, 32), offset, emit_uint_const(ctx, 32, 1));
   }
   SpvId result;
   if (num_components > 1)
      result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type, constituents, num_components);
   else
      result = bitcast_to_uvec(ctx, constituents[0], bit_size, num_components);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_store_shared(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId src = get_src(ctx, &intr->src[0]);

   unsigned wrmask = nir_intrinsic_write_mask(intr);
   unsigned bit_size = nir_src_bit_size(intr->src[0]);
   SpvId uint_type = get_uvec_type(ctx, bit_size, 1);
   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               SpvStorageClassWorkgroup,
                                               uint_type);
   SpvId offset = get_src(ctx, &intr->src[1]);
   SpvId shared_block = get_shared_block(ctx, bit_size);
   /* this is a partial write, so we have to loop and do a per-component write */
   u_foreach_bit(i, wrmask) {
      SpvId shared_offset = emit_binop(ctx, SpvOpIAdd, spirv_builder_type_uint(&ctx->builder, 32), offset, emit_uint_const(ctx, 32, i));
      SpvId val = src;
      if (nir_src_num_components(intr->src[0]) != 1)
         val = spirv_builder_emit_composite_extract(&ctx->builder, uint_type, src, &i, 1);
      SpvId member = spirv_builder_emit_access_chain(&ctx->builder, ptr_type,
                                                     shared_block, &shared_offset, 1);
      spirv_builder_emit_store(&ctx->builder, member, val);
   }
}

static void
emit_load_scratch(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId dest_type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   unsigned num_components = nir_dest_num_components(intr->dest);
   unsigned bit_size = nir_dest_bit_size(intr->dest);
   SpvId uint_type = get_uvec_type(ctx, bit_size, 1);
   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               SpvStorageClassPrivate,
                                               uint_type);
   SpvId offset = get_src(ctx, &intr->src[0]);
   SpvId constituents[NIR_MAX_VEC_COMPONENTS];
   SpvId scratch_block = get_scratch_block(ctx, bit_size);
   /* need to convert array -> vec */
   for (unsigned i = 0; i < num_components; i++) {
      SpvId member = spirv_builder_emit_access_chain(&ctx->builder, ptr_type,
                                                     scratch_block, &offset, 1);
      constituents[i] = spirv_builder_emit_load(&ctx->builder, uint_type, member);
      offset = emit_binop(ctx, SpvOpIAdd, spirv_builder_type_uint(&ctx->builder, 32), offset, emit_uint_const(ctx, 32, 1));
   }
   SpvId result;
   if (num_components > 1)
      result = spirv_builder_emit_composite_construct(&ctx->builder, dest_type, constituents, num_components);
   else
      result = bitcast_to_uvec(ctx, constituents[0], bit_size, num_components);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_store_scratch(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId src = get_src(ctx, &intr->src[0]);

   unsigned wrmask = nir_intrinsic_write_mask(intr);
   unsigned bit_size = nir_src_bit_size(intr->src[0]);
   SpvId uint_type = get_uvec_type(ctx, bit_size, 1);
   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               SpvStorageClassPrivate,
                                               uint_type);
   SpvId offset = get_src(ctx, &intr->src[1]);
   SpvId scratch_block = get_scratch_block(ctx, bit_size);
   /* this is a partial write, so we have to loop and do a per-component write */
   u_foreach_bit(i, wrmask) {
      SpvId scratch_offset = emit_binop(ctx, SpvOpIAdd, spirv_builder_type_uint(&ctx->builder, 32), offset, emit_uint_const(ctx, 32, i));
      SpvId val = src;
      if (nir_src_num_components(intr->src[0]) != 1)
         val = spirv_builder_emit_composite_extract(&ctx->builder, uint_type, src, &i, 1);
      SpvId member = spirv_builder_emit_access_chain(&ctx->builder, ptr_type,
                                                     scratch_block, &scratch_offset, 1);
      spirv_builder_emit_store(&ctx->builder, member, val);
   }
}

static void
emit_load_push_const(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId uint_type = get_uvec_type(ctx, 32, 1);
   SpvId load_type = get_uvec_type(ctx, 32, 1);

   /* number of components being loaded */
   unsigned num_components = nir_dest_num_components(intr->dest);
   SpvId constituents[NIR_MAX_VEC_COMPONENTS * 2];
   SpvId result;

   /* destination type for the load */
   SpvId type = get_dest_uvec_type(ctx, &intr->dest);
   SpvId one = emit_uint_const(ctx, 32, 1);

   /* we grab a single array member at a time, so it's a pointer to a uint */
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassPushConstant,
                                                   load_type);

   SpvId member = get_src(ctx, &intr->src[0]);
   /* reuse the offset from ZINK_PUSH_CONST_OFFSET */
   SpvId offset = emit_uint_const(ctx, 32, 0);
   /* OpAccessChain takes an array of indices that drill into a hierarchy based on the type:
    * index 0 is accessing 'base'
    * index 1 is accessing 'base[index 1]'
    *
    */
   for (unsigned i = 0; i < num_components; i++) {
      SpvId indices[2] = { member, offset };
      SpvId ptr = spirv_builder_emit_access_chain(&ctx->builder, pointer_type,
                                                  ctx->push_const_var, indices,
                                                  ARRAY_SIZE(indices));
      /* load a single value into the constituents array */
      constituents[i] = spirv_builder_emit_load(&ctx->builder, load_type, ptr);
      /* increment to the next vec4 member index for the next load */
      offset = emit_binop(ctx, SpvOpIAdd, uint_type, offset, one);
   }

   /* if loading more than 1 value, reassemble the results into the desired type,
    * otherwise just use the loaded result
    */
   if (num_components > 1) {
      result = spirv_builder_emit_composite_construct(&ctx->builder,
                                                      type,
                                                      constituents,
                                                      num_components);
   } else
      result = constituents[0];

   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_load_global(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityPhysicalStorageBufferAddresses);
   SpvId dest_type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassPhysicalStorageBuffer,
                                                   dest_type);
   SpvId ptr = emit_bitcast(ctx, pointer_type, get_src(ctx, &intr->src[0]));
   SpvId result = spirv_builder_emit_load(&ctx->builder, dest_type, ptr);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_store_global(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityPhysicalStorageBufferAddresses);
   unsigned bit_size = nir_src_bit_size(intr->src[0]);
   SpvId dest_type = get_uvec_type(ctx, bit_size, 1);
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassPhysicalStorageBuffer,
                                                   dest_type);
   SpvId param = get_src(ctx, &intr->src[0]);
   SpvId ptr = emit_bitcast(ctx, pointer_type, get_src(ctx, &intr->src[1]));
   spirv_builder_emit_store(&ctx->builder, ptr, param);
}

static SpvId
create_builtin_var(struct ntv_context *ctx, SpvId var_type,
                   SpvStorageClass storage_class,
                   const char *name, SpvBuiltIn builtin)
{
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   storage_class,
                                                   var_type);
   SpvId var = spirv_builder_emit_var(&ctx->builder, pointer_type,
                                      storage_class);
   spirv_builder_emit_name(&ctx->builder, var, name);
   spirv_builder_emit_builtin(&ctx->builder, var, builtin);

   assert(ctx->num_entry_ifaces < ARRAY_SIZE(ctx->entry_ifaces));
   ctx->entry_ifaces[ctx->num_entry_ifaces++] = var;
   return var;
}

static void
emit_load_front_face(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId var_type = spirv_builder_type_bool(&ctx->builder);
   if (!ctx->front_face_var)
      ctx->front_face_var = create_builtin_var(ctx, var_type,
                                               SpvStorageClassInput,
                                               "gl_FrontFacing",
                                               SpvBuiltInFrontFacing);

   SpvId result = spirv_builder_emit_load(&ctx->builder, var_type,
                                          ctx->front_face_var);
   assert(1 == nir_dest_num_components(intr->dest));
   store_dest(ctx, &intr->dest, result, nir_type_bool);
}

static void
emit_load_uint_input(struct ntv_context *ctx, nir_intrinsic_instr *intr, SpvId *var_id, const char *var_name, SpvBuiltIn builtin)
{
   SpvId var_type = spirv_builder_type_uint(&ctx->builder, 32);
   if (!*var_id) {
      if (builtin == SpvBuiltInSampleMask) {
         /* gl_SampleMaskIn is an array[1] in spirv... */
         var_type = spirv_builder_type_array(&ctx->builder, var_type, emit_uint_const(ctx, 32, 1));
         spirv_builder_emit_array_stride(&ctx->builder, var_type, sizeof(uint32_t));
      }
      *var_id = create_builtin_var(ctx, var_type,
                                   SpvStorageClassInput,
                                   var_name,
                                   builtin);
      if (builtin == SpvBuiltInSampleMask) {
         SpvId zero = emit_uint_const(ctx, 32, 0);
         var_type = spirv_builder_type_uint(&ctx->builder, 32);
         SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                         SpvStorageClassInput,
                                                         var_type);
         *var_id = spirv_builder_emit_access_chain(&ctx->builder, pointer_type, *var_id, &zero, 1);
      }
   }

   SpvId result = spirv_builder_emit_load(&ctx->builder, var_type, *var_id);
   assert(1 == nir_dest_num_components(intr->dest));
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_load_vec_input(struct ntv_context *ctx, nir_intrinsic_instr *intr, SpvId *var_id, const char *var_name, SpvBuiltIn builtin, nir_alu_type type)
{
   SpvId var_type;

   switch (type) {
   case nir_type_bool:
      var_type = get_bvec_type(ctx, nir_dest_num_components(intr->dest));
      break;
   case nir_type_int:
      var_type = get_ivec_type(ctx, nir_dest_bit_size(intr->dest), nir_dest_num_components(intr->dest));
      break;
   case nir_type_uint:
      var_type = get_uvec_type(ctx, nir_dest_bit_size(intr->dest), nir_dest_num_components(intr->dest));
      break;
   case nir_type_float:
      var_type = get_fvec_type(ctx, nir_dest_bit_size(intr->dest), nir_dest_num_components(intr->dest));
      break;
   default:
      unreachable("unknown type passed");
   }
   if (!*var_id)
      *var_id = create_builtin_var(ctx, var_type,
                                   SpvStorageClassInput,
                                   var_name,
                                   builtin);

   SpvId result = spirv_builder_emit_load(&ctx->builder, var_type, *var_id);
   store_dest(ctx, &intr->dest, result, type);
}

static void
emit_interpolate(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId op;
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityInterpolationFunction);
   SpvId src1 = 0;
   switch (intr->intrinsic) {
   case nir_intrinsic_interp_deref_at_centroid:
      op = GLSLstd450InterpolateAtCentroid;
      break;
   case nir_intrinsic_interp_deref_at_sample:
      op = GLSLstd450InterpolateAtSample;
      src1 = get_src(ctx, &intr->src[1]);
      break;
   case nir_intrinsic_interp_deref_at_offset:
      op = GLSLstd450InterpolateAtOffset;
      src1 = get_src(ctx, &intr->src[1]);
      /*
         The offset operand must be a vector of 2 components of 32-bit floating-point type.
         - InterpolateAtOffset spec
       */
      src1 = emit_bitcast(ctx, get_fvec_type(ctx, 32, 2), src1);
      break;
   default:
      unreachable("unknown interp op");
   }
   SpvId ptr = get_src(ctx, &intr->src[0]);
   SpvId result;
   if (intr->intrinsic == nir_intrinsic_interp_deref_at_centroid)
      result = emit_builtin_unop(ctx, op, get_glsl_type(ctx, nir_src_as_deref(intr->src[0])->type), ptr);
   else
      result = emit_builtin_binop(ctx, op, get_glsl_type(ctx, nir_src_as_deref(intr->src[0])->type),
                                  ptr, src1);
   unsigned num_components = nir_dest_num_components(intr->dest);
   unsigned bit_size = nir_dest_bit_size(intr->dest);
   result = bitcast_to_uvec(ctx, result, bit_size, num_components);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
handle_atomic_op(struct ntv_context *ctx, nir_intrinsic_instr *intr, SpvId ptr, SpvId param, SpvId param2, nir_alu_type type)
{
   SpvId dest_type = get_dest_type(ctx, &intr->dest, type);
   SpvId result = emit_atomic(ctx, get_atomic_op(ctx, nir_dest_bit_size(intr->dest), intr->intrinsic), dest_type, ptr, param, param2);
   assert(result);
   store_dest(ctx, &intr->dest, result, type);
}

static void
emit_deref_atomic_intrinsic(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId ptr = get_src(ctx, &intr->src[0]);
   SpvId param = get_src(ctx, &intr->src[1]);

   SpvId param2 = 0;

   if (nir_src_bit_size(intr->src[1]) == 64)
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityInt64Atomics);

   if (intr->intrinsic == nir_intrinsic_deref_atomic_comp_swap)
      param2 = get_src(ctx, &intr->src[2]);

   handle_atomic_op(ctx, intr, ptr, param, param2, atomic_op_is_float(intr->intrinsic) ? nir_type_float : nir_type_uint32);
}

static void
emit_shared_atomic_intrinsic(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   unsigned bit_size = nir_src_bit_size(intr->src[1]);
   SpvId dest_type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   SpvId param = get_src(ctx, &intr->src[1]);

   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassWorkgroup,
                                                   dest_type);
   SpvId offset = emit_binop(ctx, SpvOpUDiv, get_uvec_type(ctx, 32, 1), get_src(ctx, &intr->src[0]), emit_uint_const(ctx, 32, bit_size / 8));
   SpvId shared_block = get_shared_block(ctx, bit_size);
   SpvId ptr = spirv_builder_emit_access_chain(&ctx->builder, pointer_type,
                                               shared_block, &offset, 1);
   if (nir_src_bit_size(intr->src[1]) == 64)
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityInt64Atomics);
   SpvId param2 = 0;

   if (intr->intrinsic == nir_intrinsic_shared_atomic_comp_swap)
      param2 = get_src(ctx, &intr->src[2]);

   handle_atomic_op(ctx, intr, ptr, param, param2, atomic_op_is_float(intr->intrinsic) ? nir_type_float : nir_type_uint32);
}

static void
emit_get_ssbo_size(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId uint_type = get_uvec_type(ctx, 32, 1);
   nir_variable *var = ctx->ssbo_vars;
   const struct glsl_type *bare_type = glsl_without_array(var->type);
   unsigned last_member_idx = glsl_get_length(bare_type) - 1;
   SpvId pointer_type = spirv_builder_type_pointer(&ctx->builder,
                                                   SpvStorageClassStorageBuffer,
                                                   get_bo_struct_type(ctx, var));
   SpvId bo = get_src(ctx, &intr->src[0]);
   SpvId indices[] = { bo };
   SpvId ptr = spirv_builder_emit_access_chain(&ctx->builder, pointer_type,
                                               ctx->ssbos[2], indices,
                                               ARRAY_SIZE(indices));
   SpvId result = spirv_builder_emit_binop(&ctx->builder, SpvOpArrayLength, uint_type,
                                           ptr, last_member_idx);
   /* this is going to be converted by nir to:

      length = (buffer_size - offset) / stride

      * so we need to un-convert it to avoid having the calculation performed twice
      */
   const struct glsl_type *last_member = glsl_get_struct_field(bare_type, last_member_idx);
   /* multiply by stride */
   result = emit_binop(ctx, SpvOpIMul, uint_type, result, emit_uint_const(ctx, 32, glsl_get_explicit_stride(last_member)));
   /* get total ssbo size by adding offset */
   result = emit_binop(ctx, SpvOpIAdd, uint_type, result,
                        emit_uint_const(ctx, 32,
                                       glsl_get_struct_field_offset(bare_type, last_member_idx)));
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static SpvId
get_image_coords(struct ntv_context *ctx, const struct glsl_type *type, nir_src *src)
{
   uint32_t num_coords = glsl_get_sampler_coordinate_components(type);
   uint32_t src_components = nir_src_num_components(*src);

   SpvId spv = get_src(ctx, src);
   if (num_coords == src_components)
      return spv;

   /* need to extract the coord dimensions that the image can use */
   SpvId vec_type = get_uvec_type(ctx, 32, num_coords);
   if (num_coords == 1)
      return spirv_builder_emit_vector_extract(&ctx->builder, vec_type, spv, 0);
   uint32_t constituents[4];
   SpvId zero = emit_uint_const(ctx, nir_src_bit_size(*src), 0);
   assert(num_coords < ARRAY_SIZE(constituents));
   for (unsigned i = 0; i < num_coords; i++)
      constituents[i] = i < src_components ? i : zero;
   return spirv_builder_emit_vector_shuffle(&ctx->builder, vec_type, spv, spv, constituents, num_coords);
}

static void
emit_image_deref_store(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId img_var = get_src(ctx, &intr->src[0]);
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   SpvId img_type = var->data.bindless ? get_bare_image_type(ctx, var, false) : ctx->image_types[var->data.driver_location];
   const struct glsl_type *type = glsl_without_array(var->type);
   SpvId base_type = get_glsl_basetype(ctx, glsl_get_sampler_result_type(type));
   SpvId img = spirv_builder_emit_load(&ctx->builder, img_type, img_var);
   SpvId coord = get_image_coords(ctx, type, &intr->src[1]);
   SpvId texel = get_src(ctx, &intr->src[3]);
   bool use_sample = glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_MS ||
                     glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_SUBPASS_MS;
   SpvId sample = use_sample ? get_src(ctx, &intr->src[2]) : 0;
   assert(nir_src_bit_size(intr->src[3]) == glsl_base_type_bit_size(glsl_get_sampler_result_type(type)));
   /* texel type must match image type */
   texel = emit_bitcast(ctx,
                        spirv_builder_type_vector(&ctx->builder, base_type, 4),
                        texel);
   spirv_builder_emit_image_write(&ctx->builder, img, coord, texel, 0, sample, 0);
}

static SpvId
extract_sparse_load(struct ntv_context *ctx, SpvId result, SpvId dest_type, nir_ssa_def *dest_ssa)
{
   /* Result Type must be an OpTypeStruct with two members.
    * The first member’s type must be an integer type scalar.
    * It holds a Residency Code that can be passed to OpImageSparseTexelsResident
    * - OpImageSparseRead spec
    */
   uint32_t idx = 0;
   SpvId resident = spirv_builder_emit_composite_extract(&ctx->builder, spirv_builder_type_uint(&ctx->builder, 32), result, &idx, 1);
   idx = 1;
   /* normal vec4 return */
   if (dest_ssa->num_components == 4)
      result = spirv_builder_emit_composite_extract(&ctx->builder, dest_type, result, &idx, 1);
   else {
      /* shadow */
      assert(dest_ssa->num_components == 1);
      SpvId type = spirv_builder_type_float(&ctx->builder, dest_ssa->bit_size);
      SpvId val[2];
      /* pad to 2 components: the upcoming is_sparse_texels_resident instr will always use the
       * separate residency value, but the shader still expects this return to be a vec2,
       * so give it a vec2
       */
      val[0] = spirv_builder_emit_composite_extract(&ctx->builder, type, result, &idx, 1);
      val[1] = emit_float_const(ctx, dest_ssa->bit_size, 0);
      result = spirv_builder_emit_composite_construct(&ctx->builder, get_fvec_type(ctx, dest_ssa->bit_size, 2), val, 2);
   }
   assert(resident != 0);
   assert(dest_ssa->index < ctx->num_defs);
   ctx->resident_defs[dest_ssa->index] = resident;
   return result;
}

static void
emit_image_deref_load(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   bool sparse = intr->intrinsic == nir_intrinsic_image_deref_sparse_load;
   SpvId img_var = get_src(ctx, &intr->src[0]);
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   bool mediump = (var->data.precision == GLSL_PRECISION_MEDIUM || var->data.precision == GLSL_PRECISION_LOW);
   SpvId img_type = var->data.bindless ? get_bare_image_type(ctx, var, false) : ctx->image_types[var->data.driver_location];
   const struct glsl_type *type = glsl_without_array(var->type);
   SpvId base_type = get_glsl_basetype(ctx, glsl_get_sampler_result_type(type));
   SpvId img = spirv_builder_emit_load(&ctx->builder, img_type, img_var);
   SpvId coord = get_image_coords(ctx, type, &intr->src[1]);
   bool use_sample = glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_MS ||
                     glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_SUBPASS_MS;
   SpvId sample = use_sample ? get_src(ctx, &intr->src[2]) : 0;
   SpvId dest_type = spirv_builder_type_vector(&ctx->builder, base_type, nir_dest_num_components(intr->dest));
   SpvId result = spirv_builder_emit_image_read(&ctx->builder,
                                 dest_type,
                                 img, coord, 0, sample, 0, sparse);
   if (sparse)
      result = extract_sparse_load(ctx, result, dest_type, &intr->dest.ssa);

   if (!sparse && mediump) {
      spirv_builder_emit_decoration(&ctx->builder, result,
                                    SpvDecorationRelaxedPrecision);
   }

   store_dest(ctx, &intr->dest, result, nir_type_float);
}

static void
emit_image_deref_size(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId img_var = get_src(ctx, &intr->src[0]);
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   SpvId img_type = var->data.bindless ? get_bare_image_type(ctx, var, false) : ctx->image_types[var->data.driver_location];
   const struct glsl_type *type = glsl_without_array(var->type);
   SpvId img = spirv_builder_emit_load(&ctx->builder, img_type, img_var);
   unsigned num_components = glsl_get_sampler_coordinate_components(type);
   /* SPIRV requires 2 components for non-array cube size */
   if (glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_CUBE && !glsl_sampler_type_is_array(type))
      num_components = 2;

   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageQuery);
   SpvId result = spirv_builder_emit_image_query_size(&ctx->builder, get_uvec_type(ctx, 32, num_components), img, 0);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_image_deref_samples(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId img_var = get_src(ctx, &intr->src[0]);
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   SpvId img_type = var->data.bindless ? get_bare_image_type(ctx, var, false) : ctx->image_types[var->data.driver_location];
   SpvId img = spirv_builder_emit_load(&ctx->builder, img_type, img_var);

   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageQuery);
   SpvId result = spirv_builder_emit_unop(&ctx->builder, SpvOpImageQuerySamples, get_dest_type(ctx, &intr->dest, nir_type_uint), img);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_image_intrinsic(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvId param = get_src(ctx, &intr->src[3]);
   SpvId img_var = get_src(ctx, &intr->src[0]);
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   const struct glsl_type *type = glsl_without_array(var->type);
   bool is_ms;
   type_to_dim(glsl_get_sampler_dim(type), &is_ms);
   SpvId sample = is_ms ? get_src(ctx, &intr->src[2]) : emit_uint_const(ctx, 32, 0);
   SpvId coord = get_image_coords(ctx, type, &intr->src[1]);
   enum glsl_base_type glsl_type = glsl_get_sampler_result_type(type);
   SpvId base_type = get_glsl_basetype(ctx, glsl_type);
   SpvId texel = spirv_builder_emit_image_texel_pointer(&ctx->builder, base_type, img_var, coord, sample);
   SpvId param2 = 0;

   /* The type of Value must be the same as Result Type.
    * The type of the value pointed to by Pointer must be the same as Result Type.
    */
   nir_alu_type ntype = nir_get_nir_type_for_glsl_base_type(glsl_type);
   SpvId cast_type = get_dest_type(ctx, &intr->dest, ntype);
   param = emit_bitcast(ctx, cast_type, param);

   if (intr->intrinsic == nir_intrinsic_image_deref_atomic_comp_swap) {
      param2 = get_src(ctx, &intr->src[4]);
      param2 = emit_bitcast(ctx, cast_type, param2);
   }

   handle_atomic_op(ctx, intr, texel, param, param2, ntype);
}

static void
emit_ballot(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilitySubgroupBallotKHR);
   spirv_builder_emit_extension(&ctx->builder, "SPV_KHR_shader_ballot");
   SpvId type = get_dest_uvec_type(ctx, &intr->dest);
   SpvId result = emit_unop(ctx, SpvOpSubgroupBallotKHR, type, get_src(ctx, &intr->src[0]));
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_read_first_invocation(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilitySubgroupBallotKHR);
   spirv_builder_emit_extension(&ctx->builder, "SPV_KHR_shader_ballot");
   SpvId type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   SpvId result = emit_unop(ctx, SpvOpSubgroupFirstInvocationKHR, type, get_src(ctx, &intr->src[0]));
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_read_invocation(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilitySubgroupBallotKHR);
   spirv_builder_emit_extension(&ctx->builder, "SPV_KHR_shader_ballot");
   SpvId type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   SpvId result = emit_binop(ctx, SpvOpSubgroupReadInvocationKHR, type,
                              get_src(ctx, &intr->src[0]),
                              get_src(ctx, &intr->src[1]));
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_shader_clock(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityShaderClockKHR);
   spirv_builder_emit_extension(&ctx->builder, "SPV_KHR_shader_clock");

   SpvScope scope = get_scope(nir_intrinsic_memory_scope(intr));
   SpvId type = get_dest_type(ctx, &intr->dest, nir_type_uint);
   SpvId result = spirv_builder_emit_unop_const(&ctx->builder, SpvOpReadClockKHR, type, scope);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_is_sparse_texels_resident(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilitySparseResidency);

   SpvId type = get_dest_type(ctx, &intr->dest, nir_type_uint);

   /* this will always be stored with the ssa index of the parent instr */
   assert(intr->src[0].is_ssa);
   nir_ssa_def *ssa = intr->src[0].ssa;
   assert(ssa->parent_instr->type == nir_instr_type_alu);
   nir_alu_instr *alu = nir_instr_as_alu(ssa->parent_instr);
   assert(alu->src[0].src.is_ssa);
   unsigned index = alu->src[0].src.ssa->index;
   assert(index < ctx->num_defs);
   assert(ctx->resident_defs[index] != 0);
   SpvId resident = ctx->resident_defs[index];

   SpvId result = spirv_builder_emit_unop(&ctx->builder, SpvOpImageSparseTexelsResident, type, resident);
   store_dest(ctx, &intr->dest, result, nir_type_uint);
}

static void
emit_vote(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   SpvOp op;

   switch (intr->intrinsic) {
   case nir_intrinsic_vote_all:
      op = SpvOpGroupNonUniformAll;
      break;
   case nir_intrinsic_vote_any:
      op = SpvOpGroupNonUniformAny;
      break;
   case nir_intrinsic_vote_ieq:
   case nir_intrinsic_vote_feq:
      op = SpvOpGroupNonUniformAllEqual;
      break;
   default:
      unreachable("unknown vote intrinsic");
   }
   spirv_builder_emit_cap(&ctx->builder, SpvCapabilityGroupNonUniformVote);
   SpvId result = spirv_builder_emit_vote(&ctx->builder, op, get_src(ctx, &intr->src[0]));
   store_dest_raw(ctx, &intr->dest, result);
}

static void
emit_is_helper_invocation(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   spirv_builder_emit_extension(&ctx->builder,
                                "SPV_EXT_demote_to_helper_invocation");
   SpvId result = spirv_is_helper_invocation(&ctx->builder);
   store_dest(ctx, &intr->dest, result, nir_type_bool);
}

static void
emit_intrinsic(struct ntv_context *ctx, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_discard:
      emit_discard(ctx, intr);
      break;

   case nir_intrinsic_demote:
      spirv_builder_emit_demote(&ctx->builder);
      break;

   case nir_intrinsic_load_deref:
      emit_load_deref(ctx, intr);
      break;

   case nir_intrinsic_store_deref:
      emit_store_deref(ctx, intr);
      break;

   case nir_intrinsic_load_push_constant:
      emit_load_push_const(ctx, intr);
      break;

   case nir_intrinsic_load_global:
      emit_load_global(ctx, intr);
      break;

   case nir_intrinsic_store_global:
      emit_store_global(ctx, intr);
      break;

   case nir_intrinsic_load_front_face:
      emit_load_front_face(ctx, intr);
      break;

   case nir_intrinsic_load_base_instance:
      emit_load_uint_input(ctx, intr, &ctx->base_instance_var, "gl_BaseInstance", SpvBuiltInBaseInstance);
      break;

   case nir_intrinsic_load_instance_id:
      emit_load_uint_input(ctx, intr, &ctx->instance_id_var, "gl_InstanceId", SpvBuiltInInstanceIndex);
      break;

   case nir_intrinsic_load_base_vertex:
      emit_load_uint_input(ctx, intr, &ctx->base_vertex_var, "gl_BaseVertex", SpvBuiltInBaseVertex);
      break;

   case nir_intrinsic_load_draw_id:
      emit_load_uint_input(ctx, intr, &ctx->draw_id_var, "gl_DrawID", SpvBuiltInDrawIndex);
      break;

   case nir_intrinsic_load_vertex_id:
      emit_load_uint_input(ctx, intr, &ctx->vertex_id_var, "gl_VertexId", SpvBuiltInVertexIndex);
      break;

   case nir_intrinsic_load_primitive_id:
      emit_load_uint_input(ctx, intr, &ctx->primitive_id_var, "gl_PrimitiveIdIn", SpvBuiltInPrimitiveId);
      break;

   case nir_intrinsic_load_invocation_id:
      emit_load_uint_input(ctx, intr, &ctx->invocation_id_var, "gl_InvocationId", SpvBuiltInInvocationId);
      break;

   case nir_intrinsic_load_sample_id:
      emit_load_uint_input(ctx, intr, &ctx->sample_id_var, "gl_SampleId", SpvBuiltInSampleId);
      break;

   case nir_intrinsic_load_sample_pos:
      emit_load_vec_input(ctx, intr, &ctx->sample_pos_var, "gl_SamplePosition", SpvBuiltInSamplePosition, nir_type_float);
      break;

   case nir_intrinsic_load_sample_mask_in:
      emit_load_uint_input(ctx, intr, &ctx->sample_mask_in_var, "gl_SampleMaskIn", SpvBuiltInSampleMask);
      break;

   case nir_intrinsic_emit_vertex_with_counter:
      /* geometry shader emits copied xfb outputs just prior to EmitVertex(),
       * since that's the end of the shader
       */
      if (ctx->sinfo)
         emit_so_outputs(ctx, ctx->sinfo);
      if (ctx->nir->info.gs.vertices_out) //skip vertex emission if !vertices_out
         spirv_builder_emit_vertex(&ctx->builder, nir_intrinsic_stream_id(intr),
                                   ctx->nir->info.stage == MESA_SHADER_GEOMETRY && util_bitcount(ctx->nir->info.gs.active_stream_mask) > 1);
      break;

   case nir_intrinsic_set_vertex_and_primitive_count:
      /* do nothing */
      break;

   case nir_intrinsic_end_primitive_with_counter:
      spirv_builder_end_primitive(&ctx->builder, nir_intrinsic_stream_id(intr),
                                  ctx->nir->info.stage == MESA_SHADER_GEOMETRY && util_bitcount(ctx->nir->info.gs.active_stream_mask) > 1);
      break;

   case nir_intrinsic_load_helper_invocation:
      emit_load_vec_input(ctx, intr, &ctx->helper_invocation_var, "gl_HelperInvocation", SpvBuiltInHelperInvocation, nir_type_bool);
      break;

   case nir_intrinsic_load_patch_vertices_in:
      emit_load_vec_input(ctx, intr, &ctx->tess_patch_vertices_in, "gl_PatchVerticesIn",
                          SpvBuiltInPatchVertices, nir_type_int);
      break;

   case nir_intrinsic_load_tess_coord:
      emit_load_vec_input(ctx, intr, &ctx->tess_coord_var, "gl_TessCoord",
                          SpvBuiltInTessCoord, nir_type_float);
      break;

   case nir_intrinsic_memory_barrier_tcs_patch:
      /* handled by subsequent nir_intrinsic_control_barrier */
      break;

   case nir_intrinsic_memory_barrier:
      spirv_builder_emit_memory_barrier(&ctx->builder,
                                        ctx->sinfo->have_vulkan_memory_model ? SpvScopeQueueFamily : SpvScopeDevice,
                                        SpvMemorySemanticsImageMemoryMask |
                                        SpvMemorySemanticsUniformMemoryMask |
                                        SpvMemorySemanticsWorkgroupMemoryMask |
                                        SpvMemorySemanticsAtomicCounterMemoryMask |
                                        SpvMemorySemanticsAcquireReleaseMask);
      break;

   case nir_intrinsic_memory_barrier_image:
      spirv_builder_emit_memory_barrier(&ctx->builder, SpvScopeDevice,
                                        SpvMemorySemanticsImageMemoryMask |
                                        SpvMemorySemanticsAcquireReleaseMask);
      break;

   case nir_intrinsic_group_memory_barrier:
      spirv_builder_emit_memory_barrier(&ctx->builder, SpvScopeWorkgroup,
                                        SpvMemorySemanticsImageMemoryMask |
                                        SpvMemorySemanticsUniformMemoryMask |
                                        SpvMemorySemanticsWorkgroupMemoryMask |
                                        SpvMemorySemanticsAtomicCounterMemoryMask |
                                        SpvMemorySemanticsAcquireReleaseMask);
      break;

   case nir_intrinsic_memory_barrier_shared:
      spirv_builder_emit_memory_barrier(&ctx->builder, SpvScopeWorkgroup,
                                        SpvMemorySemanticsWorkgroupMemoryMask |
                                        SpvMemorySemanticsAcquireReleaseMask);
      break;

   case nir_intrinsic_control_barrier:
      if (gl_shader_stage_is_compute(ctx->stage))
         spirv_builder_emit_control_barrier(&ctx->builder, SpvScopeWorkgroup,
                                            SpvScopeWorkgroup,
                                            SpvMemorySemanticsWorkgroupMemoryMask | SpvMemorySemanticsAcquireReleaseMask);
      else if (ctx->sinfo->have_vulkan_memory_model)
         spirv_builder_emit_control_barrier(&ctx->builder, SpvScopeWorkgroup,
                                            SpvScopeWorkgroup,
                                            SpvMemorySemanticsOutputMemoryMask | SpvMemorySemanticsAcquireReleaseMask);
      else
         spirv_builder_emit_control_barrier(&ctx->builder, SpvScopeWorkgroup, SpvScopeInvocation, 0);
      break;

   case nir_intrinsic_interp_deref_at_centroid:
   case nir_intrinsic_interp_deref_at_sample:
   case nir_intrinsic_interp_deref_at_offset:
      emit_interpolate(ctx, intr);
      break;

   case nir_intrinsic_memory_barrier_buffer:
      spirv_builder_emit_memory_barrier(&ctx->builder, SpvScopeDevice,
                                        SpvMemorySemanticsUniformMemoryMask |
                                        SpvMemorySemanticsAcquireReleaseMask);
      break;

   case nir_intrinsic_deref_atomic_fadd:
   case nir_intrinsic_deref_atomic_fmin:
   case nir_intrinsic_deref_atomic_fmax:
   case nir_intrinsic_deref_atomic_fcomp_swap:
   case nir_intrinsic_deref_atomic_add:
   case nir_intrinsic_deref_atomic_umin:
   case nir_intrinsic_deref_atomic_imin:
   case nir_intrinsic_deref_atomic_umax:
   case nir_intrinsic_deref_atomic_imax:
   case nir_intrinsic_deref_atomic_and:
   case nir_intrinsic_deref_atomic_or:
   case nir_intrinsic_deref_atomic_xor:
   case nir_intrinsic_deref_atomic_exchange:
   case nir_intrinsic_deref_atomic_comp_swap:
      emit_deref_atomic_intrinsic(ctx, intr);
      break;

   case nir_intrinsic_shared_atomic_fadd:
   case nir_intrinsic_shared_atomic_fmin:
   case nir_intrinsic_shared_atomic_fmax:
   case nir_intrinsic_shared_atomic_add:
   case nir_intrinsic_shared_atomic_umin:
   case nir_intrinsic_shared_atomic_imin:
   case nir_intrinsic_shared_atomic_umax:
   case nir_intrinsic_shared_atomic_imax:
   case nir_intrinsic_shared_atomic_and:
   case nir_intrinsic_shared_atomic_or:
   case nir_intrinsic_shared_atomic_xor:
   case nir_intrinsic_shared_atomic_exchange:
   case nir_intrinsic_shared_atomic_comp_swap:
      emit_shared_atomic_intrinsic(ctx, intr);
      break;

   case nir_intrinsic_begin_invocation_interlock:
   case nir_intrinsic_end_invocation_interlock:
      spirv_builder_emit_interlock(&ctx->builder, intr->intrinsic == nir_intrinsic_end_invocation_interlock);
      break;

   case nir_intrinsic_get_ssbo_size:
      emit_get_ssbo_size(ctx, intr);
      break;

   case nir_intrinsic_image_deref_store:
      emit_image_deref_store(ctx, intr);
      break;

   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_load:
      emit_image_deref_load(ctx, intr);
      break;

   case nir_intrinsic_image_deref_size:
      emit_image_deref_size(ctx, intr);
      break;

   case nir_intrinsic_image_deref_samples:
      emit_image_deref_samples(ctx, intr);
      break;

   case nir_intrinsic_image_deref_atomic_add:
   case nir_intrinsic_image_deref_atomic_umin:
   case nir_intrinsic_image_deref_atomic_imin:
   case nir_intrinsic_image_deref_atomic_umax:
   case nir_intrinsic_image_deref_atomic_imax:
   case nir_intrinsic_image_deref_atomic_and:
   case nir_intrinsic_image_deref_atomic_or:
   case nir_intrinsic_image_deref_atomic_xor:
   case nir_intrinsic_image_deref_atomic_exchange:
   case nir_intrinsic_image_deref_atomic_comp_swap:
      emit_image_intrinsic(ctx, intr);
      break;

   case nir_intrinsic_load_workgroup_id:
      emit_load_vec_input(ctx, intr, &ctx->workgroup_id_var, "gl_WorkGroupID", SpvBuiltInWorkgroupId, nir_type_uint);
      break;

   case nir_intrinsic_load_num_workgroups:
      emit_load_vec_input(ctx, intr, &ctx->num_workgroups_var, "gl_NumWorkGroups", SpvBuiltInNumWorkgroups, nir_type_uint);
      break;

   case nir_intrinsic_load_local_invocation_id:
      emit_load_vec_input(ctx, intr, &ctx->local_invocation_id_var, "gl_LocalInvocationID", SpvBuiltInLocalInvocationId, nir_type_uint);
      break;

   case nir_intrinsic_load_global_invocation_id:
      emit_load_vec_input(ctx, intr, &ctx->global_invocation_id_var, "gl_GlobalInvocationID", SpvBuiltInGlobalInvocationId, nir_type_uint);
      break;

   case nir_intrinsic_load_local_invocation_index:
      emit_load_uint_input(ctx, intr, &ctx->local_invocation_index_var, "gl_LocalInvocationIndex", SpvBuiltInLocalInvocationIndex);
      break;

#define LOAD_SHADER_BALLOT(lowercase, camelcase) \
   case nir_intrinsic_load_##lowercase: \
      emit_load_uint_input(ctx, intr, &ctx->lowercase##_var, "gl_"#camelcase, SpvBuiltIn##camelcase); \
      break

   LOAD_SHADER_BALLOT(subgroup_id, SubgroupId);
   LOAD_SHADER_BALLOT(subgroup_eq_mask, SubgroupEqMask);
   LOAD_SHADER_BALLOT(subgroup_ge_mask, SubgroupGeMask);
   LOAD_SHADER_BALLOT(subgroup_invocation, SubgroupLocalInvocationId);
   LOAD_SHADER_BALLOT(subgroup_le_mask, SubgroupLeMask);
   LOAD_SHADER_BALLOT(subgroup_lt_mask, SubgroupLtMask);
   LOAD_SHADER_BALLOT(subgroup_size, SubgroupSize);

   case nir_intrinsic_ballot:
      emit_ballot(ctx, intr);
      break;

   case nir_intrinsic_read_first_invocation:
      emit_read_first_invocation(ctx, intr);
      break;

   case nir_intrinsic_read_invocation:
      emit_read_invocation(ctx, intr);
      break;

   case nir_intrinsic_load_workgroup_size:
      assert(ctx->local_group_size_var);
      store_dest(ctx, &intr->dest, ctx->local_group_size_var, nir_type_uint);
      break;

   case nir_intrinsic_load_shared:
      emit_load_shared(ctx, intr);
      break;

   case nir_intrinsic_store_shared:
      emit_store_shared(ctx, intr);
      break;

   case nir_intrinsic_load_scratch:
      emit_load_scratch(ctx, intr);
      break;

   case nir_intrinsic_store_scratch:
      emit_store_scratch(ctx, intr);
      break;

   case nir_intrinsic_shader_clock:
      emit_shader_clock(ctx, intr);
      break;

   case nir_intrinsic_vote_all:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_ieq:
   case nir_intrinsic_vote_feq:
      emit_vote(ctx, intr);
      break;

   case nir_intrinsic_is_sparse_texels_resident:
      emit_is_sparse_texels_resident(ctx, intr);
      break;

   case nir_intrinsic_is_helper_invocation:
      emit_is_helper_invocation(ctx, intr);
      break;

   default:
      fprintf(stderr, "emit_intrinsic: not implemented (%s)\n",
              nir_intrinsic_infos[intr->intrinsic].name);
      unreachable("unsupported intrinsic");
   }
}

static void
emit_undef(struct ntv_context *ctx, nir_ssa_undef_instr *undef)
{
   SpvId type = undef->def.bit_size == 1 ? get_bvec_type(ctx, undef->def.num_components) :
                                           get_uvec_type(ctx, undef->def.bit_size,
                                                         undef->def.num_components);

   store_ssa_def(ctx, &undef->def,
                 spirv_builder_emit_undef(&ctx->builder, type));
}

static SpvId
get_src_float(struct ntv_context *ctx, nir_src *src)
{
   SpvId def = get_src(ctx, src);
   unsigned num_components = nir_src_num_components(*src);
   unsigned bit_size = nir_src_bit_size(*src);
   return bitcast_to_fvec(ctx, def, bit_size, num_components);
}

static SpvId
get_src_int(struct ntv_context *ctx, nir_src *src)
{
   SpvId def = get_src(ctx, src);
   unsigned num_components = nir_src_num_components(*src);
   unsigned bit_size = nir_src_bit_size(*src);
   return bitcast_to_ivec(ctx, def, bit_size, num_components);
}

static inline bool
tex_instr_is_lod_allowed(nir_tex_instr *tex)
{
   /* This can only be used with an OpTypeImage that has a Dim operand of 1D, 2D, 3D, or Cube
    * - SPIR-V: 3.14. Image Operands
    */

   return (tex->sampler_dim == GLSL_SAMPLER_DIM_1D ||
           tex->sampler_dim == GLSL_SAMPLER_DIM_2D ||
           tex->sampler_dim == GLSL_SAMPLER_DIM_3D ||
           tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE ||
           /* RECT will always become 2D, so this is fine */
           tex->sampler_dim == GLSL_SAMPLER_DIM_RECT);
}

static void
emit_tex(struct ntv_context *ctx, nir_tex_instr *tex)
{
   assert(tex->op == nir_texop_tex ||
          tex->op == nir_texop_txb ||
          tex->op == nir_texop_txl ||
          tex->op == nir_texop_txd ||
          tex->op == nir_texop_txf ||
          tex->op == nir_texop_txf_ms ||
          tex->op == nir_texop_txs ||
          tex->op == nir_texop_lod ||
          tex->op == nir_texop_tg4 ||
          tex->op == nir_texop_texture_samples ||
          tex->op == nir_texop_query_levels);
   assert(tex->texture_index == tex->sampler_index || ctx->stage == MESA_SHADER_KERNEL);

   SpvId coord = 0, proj = 0, bias = 0, lod = 0, dref = 0, dx = 0, dy = 0,
         const_offset = 0, offset = 0, sample = 0, tex_offset = 0, bindless = 0, min_lod = 0;
   unsigned coord_components = 0;
   nir_variable *bindless_var = NULL;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      nir_const_value *cv;
      switch (tex->src[i].src_type) {
      case nir_tex_src_coord:
         if (tex->op == nir_texop_txf ||
             tex->op == nir_texop_txf_ms)
            coord = get_src_int(ctx, &tex->src[i].src);
         else
            coord = get_src_float(ctx, &tex->src[i].src);
         coord_components = nir_src_num_components(tex->src[i].src);
         break;

      case nir_tex_src_projector:
         assert(nir_src_num_components(tex->src[i].src) == 1);
         proj = get_src_float(ctx, &tex->src[i].src);
         assert(proj != 0);
         break;

      case nir_tex_src_offset:
         cv = nir_src_as_const_value(tex->src[i].src);
         if (cv) {
            unsigned bit_size = nir_src_bit_size(tex->src[i].src);
            unsigned num_components = nir_src_num_components(tex->src[i].src);

            SpvId components[NIR_MAX_VEC_COMPONENTS];
            for (int i = 0; i < num_components; ++i) {
               int64_t tmp = nir_const_value_as_int(cv[i], bit_size);
               components[i] = emit_int_const(ctx, bit_size, tmp);
            }

            if (num_components > 1) {
               SpvId type = get_ivec_type(ctx, bit_size, num_components);
               const_offset = spirv_builder_const_composite(&ctx->builder,
                                                            type,
                                                            components,
                                                            num_components);
            } else
               const_offset = components[0];
         } else
            offset = get_src_int(ctx, &tex->src[i].src);
         break;

      case nir_tex_src_bias:
         assert(tex->op == nir_texop_txb);
         bias = get_src_float(ctx, &tex->src[i].src);
         assert(bias != 0);
         break;

      case nir_tex_src_min_lod:
         assert(nir_src_num_components(tex->src[i].src) == 1);
         min_lod = get_src_float(ctx, &tex->src[i].src);
         assert(min_lod != 0);
         break;

      case nir_tex_src_lod:
         assert(nir_src_num_components(tex->src[i].src) == 1);
         if (tex->op == nir_texop_txf ||
             tex->op == nir_texop_txf_ms ||
             tex->op == nir_texop_txs)
            lod = get_src_int(ctx, &tex->src[i].src);
         else
            lod = get_src_float(ctx, &tex->src[i].src);
         assert(lod != 0);
         break;

      case nir_tex_src_ms_index:
         assert(nir_src_num_components(tex->src[i].src) == 1);
         sample = get_src_int(ctx, &tex->src[i].src);
         break;

      case nir_tex_src_comparator:
         assert(nir_src_num_components(tex->src[i].src) == 1);
         dref = get_src_float(ctx, &tex->src[i].src);
         assert(dref != 0);
         break;

      case nir_tex_src_ddx:
         dx = get_src_float(ctx, &tex->src[i].src);
         assert(dx != 0);
         break;

      case nir_tex_src_ddy:
         dy = get_src_float(ctx, &tex->src[i].src);
         assert(dy != 0);
         break;

      case nir_tex_src_texture_offset:
         tex_offset = get_src_int(ctx, &tex->src[i].src);
         break;

      case nir_tex_src_sampler_offset:
      case nir_tex_src_sampler_handle:
         /* don't care */
         break;

      case nir_tex_src_texture_handle:
         bindless = get_src(ctx, &tex->src[i].src);
         bindless_var = nir_deref_instr_get_variable(nir_src_as_deref(tex->src[i].src));
         break;

      default:
         fprintf(stderr, "texture source: %d\n", tex->src[i].src_type);
         unreachable("unknown texture source");
      }
   }

   unsigned texture_index = tex->texture_index;
   nir_variable *var = bindless_var ? bindless_var : ctx->sampler_var[tex->texture_index];
   if (!bindless_var && (!tex_offset || !var)) {
      if (ctx->sampler_var[texture_index]) {
         if (glsl_type_is_array(ctx->sampler_var[texture_index]->type))
            tex_offset = emit_uint_const(ctx, 32, 0);
         assert(var);
      } else {
         /* convert constant index back to base + offset */
         for (int i = texture_index; i >= 0; i--) {
            if (ctx->sampler_var[i]) {
               assert(glsl_type_is_array(ctx->sampler_var[i]->type));
               if (!tex_offset)
                  tex_offset = emit_uint_const(ctx, 32, texture_index - i);
               var = ctx->sampler_var[i];
               texture_index = i;
               break;
            }
         }
      }
   }
   assert(var);
   SpvId image_type = bindless ? get_bare_image_type(ctx, bindless_var, true) : ctx->sampler_types[texture_index];
   assert(image_type);
   SpvId sampled_type = spirv_builder_type_sampled_image(&ctx->builder,
                                                         image_type);
   assert(sampled_type);
   SpvId sampler_id = bindless ? bindless : ctx->samplers[texture_index];
   if (tex_offset) {
      SpvId ptr = spirv_builder_type_pointer(&ctx->builder, SpvStorageClassUniformConstant, sampled_type);
      sampler_id = spirv_builder_emit_access_chain(&ctx->builder, ptr, sampler_id, &tex_offset, 1);
   }
   SpvId load;
   if (ctx->stage == MESA_SHADER_KERNEL) {
      SpvId image_load = spirv_builder_emit_load(&ctx->builder, image_type, sampler_id);
      SpvId sampler_load = spirv_builder_emit_load(&ctx->builder, spirv_builder_type_sampler(&ctx->builder), ctx->cl_samplers[tex->sampler_index]);
      load = spirv_builder_emit_sampled_image(&ctx->builder, sampled_type, image_load, sampler_load);
   } else {
      load = spirv_builder_emit_load(&ctx->builder, sampled_type, sampler_id);
   }

   if (tex->is_sparse)
      tex->dest.ssa.num_components--;
   SpvId dest_type = get_dest_type(ctx, &tex->dest, tex->dest_type);

   if (nir_tex_instr_is_query(tex))
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageQuery);

   if (!tex_instr_is_lod_allowed(tex))
      lod = 0;
   else if (ctx->stage != MESA_SHADER_FRAGMENT &&
            tex->op == nir_texop_tex && ctx->explicit_lod && !lod)
      lod = emit_float_const(ctx, 32, 0.0);
   if (tex->op == nir_texop_txs) {
      SpvId image = spirv_builder_emit_image(&ctx->builder, image_type, load);
      /* Its Dim operand must be one of 1D, 2D, 3D, or Cube
       * - OpImageQuerySizeLod specification
       *
       * Additionally, if its Dim is 1D, 2D, 3D, or Cube,
       * it must also have either an MS of 1 or a Sampled of 0 or 2.
       * - OpImageQuerySize specification
       *
       * all spirv samplers use these types
       */
      if (!lod && tex_instr_is_lod_allowed(tex))
         lod = emit_uint_const(ctx, 32, 0);
      SpvId result = spirv_builder_emit_image_query_size(&ctx->builder,
                                                         dest_type, image,
                                                         lod);
      store_dest(ctx, &tex->dest, result, tex->dest_type);
      return;
   }
   if (tex->op == nir_texop_query_levels) {
      SpvId image = spirv_builder_emit_image(&ctx->builder, image_type, load);
      SpvId result = spirv_builder_emit_image_query_levels(&ctx->builder,
                                                         dest_type, image);
      store_dest(ctx, &tex->dest, result, tex->dest_type);
      return;
   }
   if (tex->op == nir_texop_texture_samples) {
      SpvId image = spirv_builder_emit_image(&ctx->builder, image_type, load);
      SpvId result = spirv_builder_emit_unop(&ctx->builder, SpvOpImageQuerySamples,
                                             dest_type, image);
      store_dest(ctx, &tex->dest, result, tex->dest_type);
      return;
   }

   if (proj && coord_components > 0) {
      SpvId constituents[NIR_MAX_VEC_COMPONENTS + 1];
      if (coord_components == 1)
         constituents[0] = coord;
      else {
         assert(coord_components > 1);
         SpvId float_type = spirv_builder_type_float(&ctx->builder, 32);
         for (uint32_t i = 0; i < coord_components; ++i)
            constituents[i] = spirv_builder_emit_composite_extract(&ctx->builder,
                                                 float_type,
                                                 coord,
                                                 &i, 1);
      }

      constituents[coord_components++] = proj;

      SpvId vec_type = get_fvec_type(ctx, 32, coord_components);
      coord = spirv_builder_emit_composite_construct(&ctx->builder,
                                                            vec_type,
                                                            constituents,
                                                            coord_components);
   }
   if (tex->op == nir_texop_lod) {
      SpvId result = spirv_builder_emit_image_query_lod(&ctx->builder,
                                                         dest_type, load,
                                                         coord);
      store_dest(ctx, &tex->dest, result, tex->dest_type);
      return;
   }
   SpvId actual_dest_type;
   if (dref && tex->op != nir_texop_tg4)
      actual_dest_type = spirv_builder_type_float(&ctx->builder, 32);
   else {
      unsigned num_components = nir_dest_num_components(tex->dest);
      switch (nir_alu_type_get_base_type(tex->dest_type)) {
      case nir_type_int:
         actual_dest_type = get_ivec_type(ctx, 32, num_components);
         break;

      case nir_type_uint:
         actual_dest_type = get_uvec_type(ctx, 32, num_components);
         break;

      case nir_type_float:
         actual_dest_type = get_fvec_type(ctx, 32, num_components);
         break;

      default:
         unreachable("unexpected nir_alu_type");
      }
   }

   SpvId result;
   if (offset)
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageGatherExtended);
   if (min_lod)
      spirv_builder_emit_cap(&ctx->builder, SpvCapabilityMinLod);
   if (tex->op == nir_texop_txf ||
       tex->op == nir_texop_txf_ms ||
       tex->op == nir_texop_tg4) {
      SpvId image = spirv_builder_emit_image(&ctx->builder, image_type, load);

      if (tex->op == nir_texop_tg4) {
         if (const_offset)
            spirv_builder_emit_cap(&ctx->builder, SpvCapabilityImageGatherExtended);
         result = spirv_builder_emit_image_gather(&ctx->builder, actual_dest_type,
                                                 load, coord, emit_uint_const(ctx, 32, tex->component),
                                                 lod, sample, const_offset, offset, dref, tex->is_sparse);
         actual_dest_type = dest_type;
      } else
         result = spirv_builder_emit_image_fetch(&ctx->builder, actual_dest_type,
                                                 image, coord, lod, sample, const_offset, offset, tex->is_sparse);
   } else {
      if (tex->op == nir_texop_txl)
         min_lod = 0;
      result = spirv_builder_emit_image_sample(&ctx->builder,
                                               actual_dest_type, load,
                                               coord,
                                               proj != 0,
                                               lod, bias, dref, dx, dy,
                                               const_offset, offset, min_lod, tex->is_sparse);
   }

   if (!bindless_var && (var->data.precision == GLSL_PRECISION_MEDIUM || var->data.precision == GLSL_PRECISION_LOW)) {
      spirv_builder_emit_decoration(&ctx->builder, result,
                                    SpvDecorationRelaxedPrecision);
   }

   if (tex->is_sparse)
      result = extract_sparse_load(ctx, result, actual_dest_type, &tex->dest.ssa);
   if (dref && nir_dest_num_components(tex->dest) > 1 && tex->op != nir_texop_tg4) {
      SpvId components[4] = { result, result, result, result };
      result = spirv_builder_emit_composite_construct(&ctx->builder,
                                                      dest_type,
                                                      components,
                                                      4);
   }

   if (nir_dest_bit_size(tex->dest) != 32) {
      /* convert FP32 to FP16 */
      result = emit_unop(ctx, SpvOpFConvert, dest_type, result);
   }

   if (tex->is_sparse && tex->is_shadow)
      tex->dest.ssa.num_components++;
   store_dest(ctx, &tex->dest, result, tex->dest_type);
   if (tex->is_sparse && !tex->is_shadow)
      tex->dest.ssa.num_components++;
}

static void
start_block(struct ntv_context *ctx, SpvId label)
{
   /* terminate previous block if needed */
   if (ctx->block_started)
      spirv_builder_emit_branch(&ctx->builder, label);

   /* start new block */
   spirv_builder_label(&ctx->builder, label);
   ctx->block_started = true;
}

static void
branch(struct ntv_context *ctx, SpvId label)
{
   assert(ctx->block_started);
   spirv_builder_emit_branch(&ctx->builder, label);
   ctx->block_started = false;
}

static void
branch_conditional(struct ntv_context *ctx, SpvId condition, SpvId then_id,
                   SpvId else_id)
{
   assert(ctx->block_started);
   spirv_builder_emit_branch_conditional(&ctx->builder, condition,
                                         then_id, else_id);
   ctx->block_started = false;
}

static void
emit_jump(struct ntv_context *ctx, nir_jump_instr *jump)
{
   switch (jump->type) {
   case nir_jump_break:
      assert(ctx->loop_break);
      branch(ctx, ctx->loop_break);
      break;

   case nir_jump_continue:
      assert(ctx->loop_cont);
      branch(ctx, ctx->loop_cont);
      break;

   default:
      unreachable("Unsupported jump type\n");
   }
}

static void
emit_deref_var(struct ntv_context *ctx, nir_deref_instr *deref)
{
   assert(deref->deref_type == nir_deref_type_var);

   struct hash_entry *he = _mesa_hash_table_search(ctx->vars, deref->var);
   assert(he);
   SpvId result = (SpvId)(intptr_t)he->data;
   store_dest_raw(ctx, &deref->dest, result);
}

static void
emit_deref_array(struct ntv_context *ctx, nir_deref_instr *deref)
{
   assert(deref->deref_type == nir_deref_type_array);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   if (!nir_src_is_always_uniform(deref->arr.index)) {
      if (deref->modes & nir_var_mem_ubo)
         spirv_builder_emit_cap(&ctx->builder,
                                SpvCapabilityUniformBufferArrayDynamicIndexing);

      if (deref->modes & nir_var_mem_ssbo)
         spirv_builder_emit_cap(&ctx->builder,
                                SpvCapabilityStorageBufferArrayDynamicIndexing);

      if (deref->modes & (nir_var_uniform | nir_var_image)) {
         const struct glsl_type *type = glsl_without_array(var->type);
         assert(glsl_type_is_sampler(type) || glsl_type_is_image(type));

         if (glsl_type_is_sampler(type))
            spirv_builder_emit_cap(&ctx->builder,
                                   SpvCapabilitySampledImageArrayDynamicIndexing);
         else
            spirv_builder_emit_cap(&ctx->builder,
                                   SpvCapabilityStorageImageArrayDynamicIndexing);
      }
   }

   SpvStorageClass storage_class = get_storage_class(var);
   SpvId base, type;
   switch (var->data.mode) {

   case nir_var_mem_ubo:
   case nir_var_mem_ssbo:
      base = get_src(ctx, &deref->parent);
      /* this is either the array<buffers> deref or the array<uint> deref */
      if (glsl_type_is_struct_or_ifc(deref->type)) {
         /* array<buffers> */
         type = get_bo_struct_type(ctx, var);
         break;
      }
      /* array<uint> */
      FALLTHROUGH;
   case nir_var_function_temp:
   case nir_var_shader_in:
   case nir_var_shader_out:
      base = get_src(ctx, &deref->parent);
      type = get_glsl_type(ctx, deref->type);
      break;

   case nir_var_uniform:
   case nir_var_image: {
      struct hash_entry *he = _mesa_hash_table_search(ctx->vars, var);
      assert(he);
      base = (SpvId)(intptr_t)he->data;
      type = get_image_type(ctx, var, glsl_type_is_sampler(glsl_without_array(var->type)));
      break;
   }

   default:
      unreachable("Unsupported nir_variable_mode\n");
   }

   SpvId index = get_src(ctx, &deref->arr.index);

   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               storage_class,
                                               type);

   SpvId result = spirv_builder_emit_access_chain(&ctx->builder,
                                                  ptr_type,
                                                  base,
                                                  &index, 1);
   /* uint is a bit of a lie here, it's really just an opaque type */
   store_dest(ctx, &deref->dest, result, nir_type_uint);
}

static void
emit_deref_struct(struct ntv_context *ctx, nir_deref_instr *deref)
{
   assert(deref->deref_type == nir_deref_type_struct);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   SpvStorageClass storage_class = get_storage_class(var);

   SpvId index = emit_uint_const(ctx, 32, deref->strct.index);
   SpvId type = (var->data.mode & (nir_var_mem_ubo | nir_var_mem_ssbo)) ?
                get_bo_array_type(ctx, var) :
                get_glsl_type(ctx, deref->type);

   SpvId ptr_type = spirv_builder_type_pointer(&ctx->builder,
                                               storage_class,
                                               type);

   SpvId result = spirv_builder_emit_access_chain(&ctx->builder,
                                                  ptr_type,
                                                  get_src(ctx, &deref->parent),
                                                  &index, 1);
   /* uint is a bit of a lie here, it's really just an opaque type */
   store_dest(ctx, &deref->dest, result, nir_type_uint);
}

static void
emit_deref(struct ntv_context *ctx, nir_deref_instr *deref)
{
   switch (deref->deref_type) {
   case nir_deref_type_var:
      emit_deref_var(ctx, deref);
      break;

   case nir_deref_type_array:
      emit_deref_array(ctx, deref);
      break;

   case nir_deref_type_struct:
      emit_deref_struct(ctx, deref);
      break;

   default:
      unreachable("unexpected deref_type");
   }
}

static void
emit_block(struct ntv_context *ctx, struct nir_block *block)
{
   start_block(ctx, block_label(ctx, block));
   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         emit_alu(ctx, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_intrinsic:
         emit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_load_const:
         emit_load_const(ctx, nir_instr_as_load_const(instr));
         break;
      case nir_instr_type_ssa_undef:
         emit_undef(ctx, nir_instr_as_ssa_undef(instr));
         break;
      case nir_instr_type_tex:
         emit_tex(ctx, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_phi:
         unreachable("nir_instr_type_phi not supported");
         break;
      case nir_instr_type_jump:
         emit_jump(ctx, nir_instr_as_jump(instr));
         break;
      case nir_instr_type_call:
         unreachable("nir_instr_type_call not supported");
         break;
      case nir_instr_type_parallel_copy:
         unreachable("nir_instr_type_parallel_copy not supported");
         break;
      case nir_instr_type_deref:
         emit_deref(ctx, nir_instr_as_deref(instr));
         break;
      }
   }
}

static void
emit_cf_list(struct ntv_context *ctx, struct exec_list *list);

static SpvId
get_src_bool(struct ntv_context *ctx, nir_src *src)
{
   assert(nir_src_bit_size(*src) == 1);
   return get_src(ctx, src);
}

static void
emit_if(struct ntv_context *ctx, nir_if *if_stmt)
{
   SpvId condition = get_src_bool(ctx, &if_stmt->condition);

   SpvId header_id = spirv_builder_new_id(&ctx->builder);
   SpvId then_id = block_label(ctx, nir_if_first_then_block(if_stmt));
   SpvId endif_id = spirv_builder_new_id(&ctx->builder);
   SpvId else_id = endif_id;

   bool has_else = !exec_list_is_empty(&if_stmt->else_list);
   if (has_else) {
      assert(nir_if_first_else_block(if_stmt)->index < ctx->num_blocks);
      else_id = block_label(ctx, nir_if_first_else_block(if_stmt));
   }

   /* create a header-block */
   start_block(ctx, header_id);
   spirv_builder_emit_selection_merge(&ctx->builder, endif_id,
                                      SpvSelectionControlMaskNone);
   branch_conditional(ctx, condition, then_id, else_id);

   emit_cf_list(ctx, &if_stmt->then_list);

   if (has_else) {
      if (ctx->block_started)
         branch(ctx, endif_id);

      emit_cf_list(ctx, &if_stmt->else_list);
   }

   start_block(ctx, endif_id);
}

static void
emit_loop(struct ntv_context *ctx, nir_loop *loop)
{
   SpvId header_id = spirv_builder_new_id(&ctx->builder);
   SpvId begin_id = block_label(ctx, nir_loop_first_block(loop));
   SpvId break_id = spirv_builder_new_id(&ctx->builder);
   SpvId cont_id = spirv_builder_new_id(&ctx->builder);

   /* create a header-block */
   start_block(ctx, header_id);
   spirv_builder_loop_merge(&ctx->builder, break_id, cont_id, SpvLoopControlMaskNone);
   branch(ctx, begin_id);

   SpvId save_break = ctx->loop_break;
   SpvId save_cont = ctx->loop_cont;
   ctx->loop_break = break_id;
   ctx->loop_cont = cont_id;

   emit_cf_list(ctx, &loop->body);

   ctx->loop_break = save_break;
   ctx->loop_cont = save_cont;

   /* loop->body may have already ended our block */
   if (ctx->block_started)
      branch(ctx, cont_id);
   start_block(ctx, cont_id);
   branch(ctx, header_id);

   start_block(ctx, break_id);
}

static void
emit_cf_list(struct ntv_context *ctx, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         emit_block(ctx, nir_cf_node_as_block(node));
         break;

      case nir_cf_node_if:
         emit_if(ctx, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         emit_loop(ctx, nir_cf_node_as_loop(node));
         break;

      case nir_cf_node_function:
         unreachable("nir_cf_node_function not supported");
         break;
      }
   }
}

static SpvExecutionMode
get_input_prim_type_mode(enum shader_prim type)
{
   switch (type) {
   case SHADER_PRIM_POINTS:
      return SpvExecutionModeInputPoints;
   case SHADER_PRIM_LINES:
   case SHADER_PRIM_LINE_LOOP:
   case SHADER_PRIM_LINE_STRIP:
      return SpvExecutionModeInputLines;
   case SHADER_PRIM_TRIANGLE_STRIP:
   case SHADER_PRIM_TRIANGLES:
   case SHADER_PRIM_TRIANGLE_FAN:
      return SpvExecutionModeTriangles;
   case SHADER_PRIM_QUADS:
   case SHADER_PRIM_QUAD_STRIP:
      return SpvExecutionModeQuads;
      break;
   case SHADER_PRIM_POLYGON:
      unreachable("handle polygons in gs");
      break;
   case SHADER_PRIM_LINES_ADJACENCY:
   case SHADER_PRIM_LINE_STRIP_ADJACENCY:
      return SpvExecutionModeInputLinesAdjacency;
   case SHADER_PRIM_TRIANGLES_ADJACENCY:
   case SHADER_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return SpvExecutionModeInputTrianglesAdjacency;
      break;
   default:
      debug_printf("unknown geometry shader input mode %u\n", type);
      unreachable("error!");
      break;
   }

   return 0;
}
static SpvExecutionMode
get_output_prim_type_mode(enum shader_prim type)
{
   switch (type) {
   case SHADER_PRIM_POINTS:
      return SpvExecutionModeOutputPoints;
   case SHADER_PRIM_LINES:
   case SHADER_PRIM_LINE_LOOP:
      unreachable("SHADER_PRIM_LINES/LINE_LOOP passed as gs output");
      break;
   case SHADER_PRIM_LINE_STRIP:
      return SpvExecutionModeOutputLineStrip;
   case SHADER_PRIM_TRIANGLE_STRIP:
      return SpvExecutionModeOutputTriangleStrip;
   case SHADER_PRIM_TRIANGLES:
   case SHADER_PRIM_TRIANGLE_FAN: //FIXME: not sure if right for output
      return SpvExecutionModeTriangles;
   case SHADER_PRIM_QUADS:
   case SHADER_PRIM_QUAD_STRIP:
      return SpvExecutionModeQuads;
   case SHADER_PRIM_POLYGON:
      unreachable("handle polygons in gs");
      break;
   case SHADER_PRIM_LINES_ADJACENCY:
   case SHADER_PRIM_LINE_STRIP_ADJACENCY:
      unreachable("handle line adjacency in gs");
      break;
   case SHADER_PRIM_TRIANGLES_ADJACENCY:
   case SHADER_PRIM_TRIANGLE_STRIP_ADJACENCY:
      unreachable("handle triangle adjacency in gs");
      break;
   default:
      debug_printf("unknown geometry shader output mode %u\n", type);
      unreachable("error!");
      break;
   }

   return 0;
}

static SpvExecutionMode
get_depth_layout_mode(enum gl_frag_depth_layout depth_layout)
{
   switch (depth_layout) {
   case FRAG_DEPTH_LAYOUT_NONE:
   case FRAG_DEPTH_LAYOUT_ANY:
      return SpvExecutionModeDepthReplacing;
   case FRAG_DEPTH_LAYOUT_GREATER:
      return SpvExecutionModeDepthGreater;
   case FRAG_DEPTH_LAYOUT_LESS:
      return SpvExecutionModeDepthLess;
   case FRAG_DEPTH_LAYOUT_UNCHANGED:
      return SpvExecutionModeDepthUnchanged;
   default:
      unreachable("unexpected depth layout");
   }
}

static SpvExecutionMode
get_primitive_mode(enum tess_primitive_mode primitive_mode)
{
   switch (primitive_mode) {
   case TESS_PRIMITIVE_TRIANGLES: return SpvExecutionModeTriangles;
   case TESS_PRIMITIVE_QUADS: return SpvExecutionModeQuads;
   case TESS_PRIMITIVE_ISOLINES: return SpvExecutionModeIsolines;
   default:
      unreachable("unknown tess prim type!");
   }
}

static SpvExecutionMode
get_spacing(enum gl_tess_spacing spacing)
{
   switch (spacing) {
   case TESS_SPACING_EQUAL:
      return SpvExecutionModeSpacingEqual;
   case TESS_SPACING_FRACTIONAL_ODD:
      return SpvExecutionModeSpacingFractionalOdd;
   case TESS_SPACING_FRACTIONAL_EVEN:
      return SpvExecutionModeSpacingFractionalEven;
   default:
      unreachable("unknown tess spacing!");
   }
}

struct spirv_shader *
nir_to_spirv(struct nir_shader *s, const struct zink_shader_info *sinfo, uint32_t spirv_version)
{
   struct spirv_shader *ret = NULL;

   struct ntv_context ctx = {0};
   ctx.mem_ctx = ralloc_context(NULL);
   ctx.nir = s;
   ctx.builder.mem_ctx = ctx.mem_ctx;
   assert(spirv_version >= SPIRV_VERSION(1, 0));
   ctx.spirv_1_4_interfaces = spirv_version >= SPIRV_VERSION(1, 4);

   ctx.glsl_types = _mesa_pointer_hash_table_create(ctx.mem_ctx);
   ctx.bo_array_types = _mesa_pointer_hash_table_create(ctx.mem_ctx);
   ctx.bo_struct_types = _mesa_pointer_hash_table_create(ctx.mem_ctx);
   if (!ctx.glsl_types || !ctx.bo_array_types || !ctx.bo_struct_types)
      goto fail;

   spirv_builder_emit_cap(&ctx.builder, SpvCapabilityShader);

   switch (s->info.stage) {
   case MESA_SHADER_FRAGMENT:
      if (s->info.fs.post_depth_coverage &&
          BITSET_TEST(s->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN))
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilitySampleMaskPostDepthCoverage);
      if (s->info.fs.uses_sample_shading)
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilitySampleRateShading);
      if (s->info.fs.uses_demote)
         spirv_builder_emit_extension(&ctx.builder,
                                      "SPV_EXT_demote_to_helper_invocation");
      break;

   case MESA_SHADER_VERTEX:
      if (BITSET_TEST(s->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID) ||
          BITSET_TEST(s->info.system_values_read, SYSTEM_VALUE_DRAW_ID) ||
          BITSET_TEST(s->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE) ||
          BITSET_TEST(s->info.system_values_read, SYSTEM_VALUE_BASE_VERTEX)) {
         if (spirv_version < SPIRV_VERSION(1, 3))
            spirv_builder_emit_extension(&ctx.builder, "SPV_KHR_shader_draw_parameters");
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityDrawParameters);
      }
      break;

   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityTessellation);
      /* TODO: check features for this */
      if (s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_PSIZ))
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityTessellationPointSize);
      break;

   case MESA_SHADER_GEOMETRY:
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityGeometry);
      if (s->info.gs.active_stream_mask)
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityGeometryStreams);
      if (s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_PSIZ))
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityGeometryPointSize);
      break;

   default: ;
   }

   if (s->info.stage < MESA_SHADER_GEOMETRY) {
      if (s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_LAYER) ||
          s->info.inputs_read & BITFIELD64_BIT(VARYING_SLOT_LAYER)) {
         if (spirv_version >= SPIRV_VERSION(1, 5))
            spirv_builder_emit_cap(&ctx.builder, SpvCapabilityShaderLayer);
         else {
            spirv_builder_emit_extension(&ctx.builder, "SPV_EXT_shader_viewport_index_layer");
            spirv_builder_emit_cap(&ctx.builder, SpvCapabilityShaderViewportIndexLayerEXT);
         }
      }
   } else if (s->info.stage == MESA_SHADER_FRAGMENT) {
      /* incredibly, this is legal and intended.
       * https://github.com/KhronosGroup/SPIRV-Registry/issues/95
       */
      if (s->info.inputs_read & (BITFIELD64_BIT(VARYING_SLOT_LAYER) |
                                 BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_ID)))
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityGeometry);
   }

   if (s->info.num_ssbos && spirv_version < SPIRV_VERSION(1, 1))
      spirv_builder_emit_extension(&ctx.builder, "SPV_KHR_storage_buffer_storage_class");

   if (s->info.stage < MESA_SHADER_FRAGMENT &&
       s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_VIEWPORT)) {
      if (s->info.stage < MESA_SHADER_GEOMETRY)
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityShaderViewportIndex);
      else
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityMultiViewport);
   }

   ctx.stage = s->info.stage;
   ctx.sinfo = sinfo;
   ctx.GLSL_std_450 = spirv_builder_import(&ctx.builder, "GLSL.std.450");
   ctx.explicit_lod = true;
   spirv_builder_emit_source(&ctx.builder, SpvSourceLanguageUnknown, 0);

   if (gl_shader_stage_is_compute(s->info.stage)) {
      SpvAddressingModel model;
      if (s->info.cs.ptr_size == 32)
         model = SpvAddressingModelPhysical32;
      else if (s->info.cs.ptr_size == 64)
         model = SpvAddressingModelPhysicalStorageBuffer64;
      else
         model = SpvAddressingModelLogical;
      spirv_builder_emit_mem_model(&ctx.builder, model,
                                   SpvMemoryModelGLSL450);
   } else {
      if (ctx.sinfo->have_vulkan_memory_model) {
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityVulkanMemoryModel);
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityVulkanMemoryModelDeviceScope);
         spirv_builder_emit_mem_model(&ctx.builder, SpvAddressingModelLogical,
                                      SpvMemoryModelVulkan);
      } else {
         spirv_builder_emit_mem_model(&ctx.builder, SpvAddressingModelLogical,
                                      SpvMemoryModelGLSL450);
      }
   }

   if (s->info.stage == MESA_SHADER_FRAGMENT &&
       s->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL)) {
      spirv_builder_emit_extension(&ctx.builder, "SPV_EXT_shader_stencil_export");
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityStencilExportEXT);
   }

   SpvExecutionModel exec_model;
   switch (s->info.stage) {
   case MESA_SHADER_VERTEX:
      exec_model = SpvExecutionModelVertex;
      break;
   case MESA_SHADER_TESS_CTRL:
      exec_model = SpvExecutionModelTessellationControl;
      break;
   case MESA_SHADER_TESS_EVAL:
      exec_model = SpvExecutionModelTessellationEvaluation;
      break;
   case MESA_SHADER_GEOMETRY:
      exec_model = SpvExecutionModelGeometry;
      break;
   case MESA_SHADER_FRAGMENT:
      exec_model = SpvExecutionModelFragment;
      break;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      exec_model = SpvExecutionModelGLCompute;
      break;
   default:
      unreachable("invalid stage");
   }

   SpvId type_void = spirv_builder_type_void(&ctx.builder);
   SpvId type_void_func = spirv_builder_type_function(&ctx.builder, type_void,
                                                      NULL, 0);
   SpvId entry_point = spirv_builder_new_id(&ctx.builder);
   spirv_builder_emit_name(&ctx.builder, entry_point, "main");

   ctx.vars = _mesa_hash_table_create(ctx.mem_ctx, _mesa_hash_pointer,
                                      _mesa_key_pointer_equal);

   ctx.so_outputs = _mesa_hash_table_create(ctx.mem_ctx, _mesa_hash_u32,
                                            _mesa_key_u32_equal);

   nir_foreach_variable_with_modes(var, s, nir_var_mem_push_const)
      input_var_init(&ctx, var);

   nir_foreach_shader_in_variable(var, s)
      emit_input(&ctx, var);

   int max_output = 0;
   nir_foreach_shader_out_variable(var, s) {
      /* ignore SPIR-V built-ins, tagged with a sentinel value */
      if (var->data.driver_location != UINT_MAX) {
         assert(var->data.driver_location < INT_MAX);
         unsigned extent = glsl_count_attribute_slots(var->type, false);
         max_output = MAX2(max_output, (int)var->data.driver_location + extent);
      }
      emit_output(&ctx, var);
   }

   if (sinfo->last_vertex)
      emit_so_info(&ctx, sinfo, max_output);
   uint32_t tcs_vertices_out_word = 0;

   unsigned ubo_counter[2] = {0};
   nir_foreach_variable_with_modes(var, s, nir_var_mem_ubo)
      ubo_counter[var->data.driver_location != 0]++;
   nir_foreach_variable_with_modes(var, s, nir_var_mem_ubo)
      emit_bo(&ctx, var, ubo_counter[var->data.driver_location != 0] > 1);

   unsigned ssbo_counter = 0;
   nir_foreach_variable_with_modes(var, s, nir_var_mem_ssbo)
      ssbo_counter++;
   nir_foreach_variable_with_modes(var, s, nir_var_mem_ssbo)
      emit_bo(&ctx, var, ssbo_counter > 1);

   nir_foreach_variable_with_modes(var, s, nir_var_image)
      ctx.image_var[var->data.driver_location] = var;
   nir_foreach_variable_with_modes(var, s, nir_var_uniform) {
      if (glsl_type_is_sampler(glsl_without_array(var->type))) {
         ctx.sampler_var[var->data.driver_location] = var;
         ctx.last_sampler = MAX2(ctx.last_sampler, var->data.driver_location);
      }
   }
   if (sinfo->sampler_mask) {
      assert(s->info.stage == MESA_SHADER_KERNEL);
      int desc_set = -1;
      nir_foreach_variable_with_modes(var, s, nir_var_uniform) {
         if (glsl_type_is_sampler(glsl_without_array(var->type))) {
            desc_set = var->data.descriptor_set;
            break;
         }
      }
      assert(desc_set != -1);
      u_foreach_bit(sampler, sinfo->sampler_mask)
         emit_sampler(&ctx, sampler, desc_set);
   }
   nir_foreach_variable_with_modes(var, s, nir_var_image | nir_var_uniform) {
      const struct glsl_type *type = glsl_without_array(var->type);
      if (glsl_type_is_sampler(type))
         emit_image(&ctx, var, get_bare_image_type(&ctx, var, true), false);
      else if (glsl_type_is_image(type))
         emit_image(&ctx, var, get_bare_image_type(&ctx, var, false), false);
   }

   switch (s->info.stage) {
   case MESA_SHADER_FRAGMENT:
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   SpvExecutionModeOriginUpperLeft);
      if (s->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH))
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                      get_depth_layout_mode(s->info.fs.depth_layout));
      if (s->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL))
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                      SpvExecutionModeStencilRefReplacingEXT);
      if (s->info.fs.early_fragment_tests)
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                      SpvExecutionModeEarlyFragmentTests);
      if (s->info.fs.post_depth_coverage) {
         spirv_builder_emit_extension(&ctx.builder, "SPV_KHR_post_depth_coverage");
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                      SpvExecutionModePostDepthCoverage);
      }

      if (s->info.fs.pixel_interlock_ordered || s->info.fs.pixel_interlock_unordered ||
          s->info.fs.sample_interlock_ordered || s->info.fs.sample_interlock_unordered)
         spirv_builder_emit_extension(&ctx.builder, "SPV_EXT_fragment_shader_interlock");
      if (s->info.fs.pixel_interlock_ordered || s->info.fs.pixel_interlock_unordered)
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityFragmentShaderPixelInterlockEXT);
      if (s->info.fs.sample_interlock_ordered || s->info.fs.sample_interlock_unordered)
         spirv_builder_emit_cap(&ctx.builder, SpvCapabilityFragmentShaderSampleInterlockEXT);
      if (s->info.fs.pixel_interlock_ordered)
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point, SpvExecutionModePixelInterlockOrderedEXT);
      if (s->info.fs.pixel_interlock_unordered)
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point, SpvExecutionModePixelInterlockUnorderedEXT);
      if (s->info.fs.sample_interlock_ordered)
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point, SpvExecutionModeSampleInterlockOrderedEXT);
      if (s->info.fs.sample_interlock_unordered)
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point, SpvExecutionModeSampleInterlockUnorderedEXT);
      break;
   case MESA_SHADER_TESS_CTRL:
      tcs_vertices_out_word = spirv_builder_emit_exec_mode_literal(&ctx.builder, entry_point,
                                                                   SpvExecutionModeOutputVertices,
                                                                   s->info.tess.tcs_vertices_out);
      break;
   case MESA_SHADER_TESS_EVAL:
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   get_primitive_mode(s->info.tess._primitive_mode));
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   s->info.tess.ccw ? SpvExecutionModeVertexOrderCcw
                                                    : SpvExecutionModeVertexOrderCw);
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   get_spacing(s->info.tess.spacing));
      if (s->info.tess.point_mode)
         spirv_builder_emit_exec_mode(&ctx.builder, entry_point, SpvExecutionModePointMode);
      break;
   case MESA_SHADER_GEOMETRY:
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   get_input_prim_type_mode(s->info.gs.input_primitive));
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   get_output_prim_type_mode(s->info.gs.output_primitive));
      spirv_builder_emit_exec_mode_literal(&ctx.builder, entry_point,
                                           SpvExecutionModeInvocations,
                                           s->info.gs.invocations);
      spirv_builder_emit_exec_mode_literal(&ctx.builder, entry_point,
                                           SpvExecutionModeOutputVertices,
                                           MAX2(s->info.gs.vertices_out, 1));
      break;
   case MESA_SHADER_KERNEL:
   case MESA_SHADER_COMPUTE:
      if (s->info.workgroup_size[0] || s->info.workgroup_size[1] || s->info.workgroup_size[2])
         spirv_builder_emit_exec_mode_literal3(&ctx.builder, entry_point, SpvExecutionModeLocalSize,
                                               (uint32_t[3]){(uint32_t)s->info.workgroup_size[0], (uint32_t)s->info.workgroup_size[1],
                                               (uint32_t)s->info.workgroup_size[2]});
      else {
         SpvId sizes[3];
         uint32_t ids[] = {ZINK_WORKGROUP_SIZE_X, ZINK_WORKGROUP_SIZE_Y, ZINK_WORKGROUP_SIZE_Z};
         const char *names[] = {"x", "y", "z"};
         for (int i = 0; i < 3; i ++) {
            sizes[i] = spirv_builder_spec_const_uint(&ctx.builder, 32);
            spirv_builder_emit_specid(&ctx.builder, sizes[i], ids[i]);
            spirv_builder_emit_name(&ctx.builder, sizes[i], names[i]);
         }
         SpvId var_type = get_uvec_type(&ctx, 32, 3);
         ctx.local_group_size_var = spirv_builder_spec_const_composite(&ctx.builder, var_type, sizes, 3);
         spirv_builder_emit_name(&ctx.builder, ctx.local_group_size_var, "gl_LocalGroupSize");
         spirv_builder_emit_builtin(&ctx.builder, ctx.local_group_size_var, SpvBuiltInWorkgroupSize);
      }
      break;
   default:
      break;
   }
   if (BITSET_TEST_RANGE(s->info.system_values_read, SYSTEM_VALUE_SUBGROUP_SIZE, SYSTEM_VALUE_SUBGROUP_LT_MASK)) {
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilitySubgroupBallotKHR);
      spirv_builder_emit_extension(&ctx.builder, "SPV_KHR_shader_ballot");
   }
   if (s->info.has_transform_feedback_varyings) {
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilityTransformFeedback);
      spirv_builder_emit_exec_mode(&ctx.builder, entry_point,
                                   SpvExecutionModeXfb);
   }

   if (s->info.stage == MESA_SHADER_FRAGMENT && s->info.fs.uses_discard) {
      ctx.discard_func = spirv_builder_new_id(&ctx.builder);
      spirv_builder_emit_name(&ctx.builder, ctx.discard_func, "discard");
      spirv_builder_function(&ctx.builder, ctx.discard_func, type_void,
                             SpvFunctionControlMaskNone,
                             type_void_func);
      SpvId label = spirv_builder_new_id(&ctx.builder);
      spirv_builder_label(&ctx.builder, label);
      spirv_builder_emit_kill(&ctx.builder);
      spirv_builder_function_end(&ctx.builder);
   }

   spirv_builder_function(&ctx.builder, entry_point, type_void,
                          SpvFunctionControlMaskNone,
                          type_void_func);

   nir_function_impl *entry = nir_shader_get_entrypoint(s);
   nir_metadata_require(entry, nir_metadata_block_index);

   ctx.defs = ralloc_array_size(ctx.mem_ctx,
                                sizeof(SpvId), entry->ssa_alloc);
   if (!ctx.defs)
      goto fail;
   if (sinfo->have_sparse) {
      spirv_builder_emit_cap(&ctx.builder, SpvCapabilitySparseResidency);
      /* this could be huge, so only alloc if needed since it's extremely unlikely to
       * ever be used by anything except cts
       */
      ctx.resident_defs = ralloc_array_size(ctx.mem_ctx,
                                            sizeof(SpvId), entry->ssa_alloc);
      if (!ctx.resident_defs)
         goto fail;
   }
   ctx.num_defs = entry->ssa_alloc;

   nir_index_local_regs(entry);
   ctx.regs = ralloc_array_size(ctx.mem_ctx,
                                sizeof(SpvId), entry->reg_alloc);
   if (!ctx.regs)
      goto fail;
   ctx.num_regs = entry->reg_alloc;

   SpvId *block_ids = ralloc_array_size(ctx.mem_ctx,
                                        sizeof(SpvId), entry->num_blocks);
   if (!block_ids)
      goto fail;

   for (int i = 0; i < entry->num_blocks; ++i)
      block_ids[i] = spirv_builder_new_id(&ctx.builder);

   ctx.block_ids = block_ids;
   ctx.num_blocks = entry->num_blocks;

   /* emit a block only for the variable declarations */
   start_block(&ctx, spirv_builder_new_id(&ctx.builder));
   foreach_list_typed(nir_register, reg, node, &entry->registers) {
      SpvId type = get_vec_from_bit_size(&ctx, reg->bit_size, reg->num_components);
      SpvId pointer_type = spirv_builder_type_pointer(&ctx.builder,
                                                      SpvStorageClassFunction,
                                                      type);
      SpvId var = spirv_builder_emit_var(&ctx.builder, pointer_type,
                                         SpvStorageClassFunction);

      ctx.regs[reg->index] = var;
   }

   nir_foreach_function_temp_variable(var, entry)
      emit_temp(&ctx, var);


   emit_cf_list(&ctx, &entry->body);

   /* vertex/tess shader emits copied xfb outputs at the end of the shader */
   if (sinfo->last_vertex && (ctx.stage == MESA_SHADER_VERTEX || ctx.stage == MESA_SHADER_TESS_EVAL))
      emit_so_outputs(&ctx, sinfo);

   spirv_builder_return(&ctx.builder); // doesn't belong here, but whatevz
   spirv_builder_function_end(&ctx.builder);

   spirv_builder_emit_entry_point(&ctx.builder, exec_model, entry_point,
                                  "main", ctx.entry_ifaces,
                                  ctx.num_entry_ifaces);

   size_t num_words = spirv_builder_get_num_words(&ctx.builder);

   ret = ralloc(NULL, struct spirv_shader);
   if (!ret)
      goto fail;

   ret->words = ralloc_size(ret, sizeof(uint32_t) * num_words);
   if (!ret->words)
      goto fail;

   ret->num_words = spirv_builder_get_words(&ctx.builder, ret->words, num_words, spirv_version, &tcs_vertices_out_word);
   ret->tcs_vertices_out_word = tcs_vertices_out_word;
   assert(ret->num_words == num_words);

   ralloc_free(ctx.mem_ctx);

   return ret;

fail:
   ralloc_free(ctx.mem_ctx);

   if (ret)
      spirv_shader_delete(ret);

   return NULL;
}

void
spirv_shader_delete(struct spirv_shader *s)
{
   ralloc_free(s);
}
