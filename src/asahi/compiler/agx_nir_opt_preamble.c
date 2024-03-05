/*
 * Copyright 2022 Alyssa Rosenzweig
 * Copyright 2021 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir_builder.h"
#include "agx_compiler.h"
#include "nir.h"
#include "nir_opcodes.h"

static void
def_size(nir_def *def, unsigned *size, unsigned *align)
{
   unsigned bit_size = MAX2(def->bit_size, 16);

   *size = (bit_size * def->num_components) / 16;
   *align = bit_size / 16;
}

static bool
all_uses_float(nir_def *def)
{
   nir_foreach_use_including_if(use, def) {
      if (nir_src_is_if(use))
         return false;

      nir_instr *use_instr = nir_src_parent_instr(use);
      if (use_instr->type != nir_instr_type_alu)
         return false;

      nir_alu_instr *use_alu = nir_instr_as_alu(use_instr);
      unsigned src_index = ~0;
      for (unsigned i = 0; i < nir_op_infos[use_alu->op].num_inputs; i++) {
         if (&use_alu->src[i].src == use) {
            src_index = i;
            break;
         }
      }

      assert(src_index != ~0);
      nir_alu_type src_type = nir_alu_type_get_base_type(
         nir_op_infos[use_alu->op].input_types[src_index]);

      if (src_type != nir_type_float)
         return false;

      /* No float modifiers on G13 */
      if (use_alu->op == nir_op_fmax || use_alu->op == nir_op_fmin)
         return false;
   }

   return true;
}

static float
instr_cost(nir_instr *instr, const void *data)
{
   switch (instr->type) {
   case nir_instr_type_intrinsic:
      switch (nir_instr_as_intrinsic(instr)->intrinsic) {
      case nir_intrinsic_load_global:
      case nir_intrinsic_load_agx:
      case nir_intrinsic_load_global_constant:
      case nir_intrinsic_load_constant_agx:
      case nir_intrinsic_load_ubo:
         return 10.0;
      default:
         /* Assume it's a sysval or something */
         return 0.0;
      }

   case nir_instr_type_tex:
      /* Texturing involes lots of memory bandwidth */
      return 20.0;

   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      switch (alu->op) {
      case nir_op_fneg:
      case nir_op_fabs:
      case nir_op_f2f32:
         /* Float source modifiers will be propagated */
         return all_uses_float(&alu->def) ? 0.0 : 2.0;

      default:
         /* We optimistically assume that moves get coalesced */
         return nir_op_is_vec_or_mov(alu->op) ? 0.0 : 2.0;
      }
   }

   default:
      return 1.0;
   }
}

static float
rewrite_cost(nir_def *def, const void *data)
{
   bool mov_needed = false;
   nir_foreach_use(use, def) {
      nir_instr *parent_instr = nir_src_parent_instr(use);
      if (parent_instr->type != nir_instr_type_alu) {
         mov_needed = true;
         break;
      } else {
         nir_alu_instr *alu = nir_instr_as_alu(parent_instr);
         if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 ||
             alu->op == nir_op_vec4 || alu->op == nir_op_mov) {
            mov_needed = true;
            break;
         } else {
            /* Assume for non-moves that the const is folded into the src */
         }
      }
   }

   return mov_needed ? def->num_components : 0;
}

static bool
avoid_instr(const nir_instr *instr, const void *data)
{
   const nir_def *def = nir_instr_def((nir_instr *)instr);

   /* Do not move bindless handles, since we need those to retain their constant
    * base index.
    */
   if (def) {
      nir_foreach_use(use, def) {
         if (nir_src_parent_instr(use)->type == nir_instr_type_tex) {
            /* Check if used as a bindless texture handle */
            nir_tex_instr *tex = nir_instr_as_tex(nir_src_parent_instr(use));
            int handle_idx =
               nir_tex_instr_src_index(tex, nir_tex_src_texture_handle);

            if (handle_idx >= 0 && tex->src[handle_idx].src.ssa == def)
               return true;
         } else if (nir_src_parent_instr(use)->type ==
                    nir_instr_type_intrinsic) {
            /* Check if used as a bindless image handle */
            nir_intrinsic_instr *intr =
               nir_instr_as_intrinsic(nir_src_parent_instr(use));

            switch (intr->intrinsic) {
            case nir_intrinsic_bindless_image_load:
            case nir_intrinsic_bindless_image_store:
               if (intr->src[0].ssa == def)
                  return true;
               break;
            default:
               break;
            }
         }
      }
   }

   return false;
}

static const nir_opt_preamble_options preamble_options = {
   .drawid_uniform = true,
   .subgroup_size_uniform = true,
   /* not supported in hardware */
   .load_workgroup_size_allowed = false,
   .def_size = def_size,
   .instr_cost_cb = instr_cost,
   .rewrite_cost_cb = rewrite_cost,
   .avoid_instr_cb = avoid_instr,

   /* hardware size is 512, but it's polite to leave some wiggle room to push
    * hot constants so we don't end up rematerializing all over the place.
    * 480 seems to be a sweetspot, based on a few minutes of shader-db.
    */
   .preamble_storage_size = 480,
};

bool
agx_nir_opt_preamble(nir_shader *nir, unsigned *preamble_size)
{
   return nir_opt_preamble(nir, &preamble_options, preamble_size);
}
