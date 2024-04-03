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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ir_optimization.h
 *
 * Prototypes for optimization passes to be called by the compiler and drivers.
 */

#ifndef GLSL_IR_OPTIMIZATION_H
#define GLSL_IR_OPTIMIZATION_H

struct gl_linked_shader;
struct gl_shader_program;

/* Operations for lower_instructions() */
#define SUB_TO_ADD_NEG     0x01
#define LDEXP_TO_ARITH     0x80
#define CARRY_TO_ARITH     0x100
#define BORROW_TO_ARITH    0x200
#define DOPS_TO_DFRAC      0x800
#define DFREXP_DLDEXP_TO_ARITH    0x1000
#define BIT_COUNT_TO_MATH         0x02000
#define EXTRACT_TO_SHIFTS         0x04000
#define INSERT_TO_SHIFTS          0x08000
#define REVERSE_TO_SHIFTS         0x10000
#define FIND_LSB_TO_FLOAT_CAST    0x20000
#define FIND_MSB_TO_FLOAT_CAST    0x40000
#define IMUL_HIGH_TO_MUL          0x80000
#define SQRT_TO_ABS_SQRT          0x200000

/* Operations for lower_64bit_integer_instructions() */
#define DIV64                     (1U << 0)
#define MOD64                     (1U << 1)

/**
 * \see class lower_packing_builtins_visitor
 */
enum lower_packing_builtins_op {
   LOWER_PACK_UNPACK_NONE               = 0x0000,

   LOWER_PACK_SNORM_2x16                = 0x0001,
   LOWER_UNPACK_SNORM_2x16              = 0x0002,

   LOWER_PACK_UNORM_2x16                = 0x0004,
   LOWER_UNPACK_UNORM_2x16              = 0x0008,

   LOWER_PACK_HALF_2x16                 = 0x0010,
   LOWER_UNPACK_HALF_2x16               = 0x0020,

   LOWER_PACK_SNORM_4x8                 = 0x0040,
   LOWER_UNPACK_SNORM_4x8               = 0x0080,

   LOWER_PACK_UNORM_4x8                 = 0x0100,
   LOWER_UNPACK_UNORM_4x8               = 0x0200,

   LOWER_PACK_USE_BFI                   = 0x0400,
   LOWER_PACK_USE_BFE                   = 0x0800,
};

bool do_common_optimization(exec_list *ir, bool linked,
                            const struct gl_shader_compiler_options *options,
                            bool native_integers);

bool ir_constant_fold(ir_rvalue **rvalue);

bool do_rebalance_tree(exec_list *instructions);
bool do_algebraic(exec_list *instructions, bool native_integers,
                  const struct gl_shader_compiler_options *options);
bool do_constant_folding(exec_list *instructions);
bool do_constant_variable(exec_list *instructions);
bool do_constant_variable_unlinked(exec_list *instructions);
bool do_copy_propagation_elements(exec_list *instructions);
bool do_constant_propagation(exec_list *instructions);
bool do_dead_code(exec_list *instructions);
bool do_dead_code_local(exec_list *instructions);
bool do_dead_code_unlinked(exec_list *instructions);
bool do_dead_functions(exec_list *instructions);
bool opt_flip_matrices(exec_list *instructions);
bool do_function_inlining(exec_list *instructions);
bool do_lower_jumps(exec_list *instructions, bool pull_out_jumps = true, bool lower_sub_return = true, bool lower_main_return = false, bool lower_continue = false);
bool do_if_simplification(exec_list *instructions);
bool opt_flatten_nested_if_blocks(exec_list *instructions);
bool do_mat_op_to_vec(exec_list *instructions);
bool do_minmax_prune(exec_list *instructions);
bool do_structure_splitting(exec_list *instructions);
bool do_tree_grafting(exec_list *instructions);
bool do_vec_index_to_cond_assign(exec_list *instructions);
bool do_vec_index_to_swizzle(exec_list *instructions);
bool lower_discard(exec_list *instructions);
void lower_discard_flow(exec_list *instructions);
bool lower_instructions(exec_list *instructions, unsigned what_to_lower);
bool lower_clip_cull_distance(struct gl_shader_program *prog,
                              gl_linked_shader *shader);
void lower_output_reads(unsigned stage, exec_list *instructions);
bool lower_packing_builtins(exec_list *instructions, int op_mask);
bool lower_vector_insert(exec_list *instructions, bool lower_nonconstant_index);
bool lower_vector_derefs(gl_linked_shader *shader);
void lower_named_interface_blocks(void *mem_ctx, gl_linked_shader *shader);
bool optimize_split_arrays(exec_list *instructions, bool linked);
void optimize_dead_builtin_variables(exec_list *instructions,
                                     enum ir_variable_mode other);
bool lower_tess_level(gl_linked_shader *shader);

bool lower_blend_equation_advanced(gl_linked_shader *shader, bool coherent);

bool lower_builtins(exec_list *instructions);
bool lower_subroutine(exec_list *instructions, struct _mesa_glsl_parse_state *state);
bool propagate_invariance(exec_list *instructions);

namespace ir_builder { class ir_factory; };

ir_variable *compare_index_block(ir_builder::ir_factory &body,
                                 ir_variable *index,
                                 unsigned base, unsigned components);

bool lower_64bit_integer_instructions(exec_list *instructions,
                                      unsigned what_to_lower);

void lower_precision(const struct gl_shader_compiler_options *options,
                     exec_list *instructions);

#endif /* GLSL_IR_OPTIMIZATION_H */
