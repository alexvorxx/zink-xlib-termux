/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

/**
 * \file
 * Optimize subgroup operations with uniform sources.
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"

static bool
opt_uniform_subgroup_filter(const nir_instr *instr, const void *_state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   switch (intrin->intrinsic) {
   case nir_intrinsic_shuffle:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_quad_broadcast:
   case nir_intrinsic_quad_swap_horizontal:
   case nir_intrinsic_quad_swap_vertical:
   case nir_intrinsic_quad_swap_diagonal:
   case nir_intrinsic_quad_swizzle_amd:
   case nir_intrinsic_masked_swizzle_amd:
      return !nir_src_is_divergent(intrin->src[0]);

   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan: {
      if (nir_src_is_divergent(intrin->src[0]))
         return false;

      const nir_op reduction_op = (nir_op) nir_intrinsic_reduction_op(intrin);

      switch (reduction_op) {
      case nir_op_imin:
      case nir_op_umin:
      case nir_op_fmin:
      case nir_op_imax:
      case nir_op_umax:
      case nir_op_fmax:
      case nir_op_iand:
      case nir_op_ior:
         return true;

      /* FINISHME: iadd, ixor, and fadd are also possible. */
      default:
         return false;
      }
   }

   default:
      return false;
   }
}

static nir_def *
opt_uniform_subgroup_instr(nir_builder *b, nir_instr *instr, void *_state)
{
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   return intrin->src[0].ssa;
}

bool
nir_opt_uniform_subgroup(nir_shader *shader)
{
   bool progress = nir_shader_lower_instructions(shader,
                                                 opt_uniform_subgroup_filter,
                                                 opt_uniform_subgroup_instr,
                                                 NULL);

   return progress;
}

