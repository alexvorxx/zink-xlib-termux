/*
 * Copyright © 2024 Valve Corporation
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

#include "nir_test.h"

class nir_opt_loop_test : public nir_test {
protected:
   nir_opt_loop_test();

   nir_def *in_def;
   nir_variable *out_var;
   nir_variable *ubo_var;
};

nir_opt_loop_test::nir_opt_loop_test()
   : nir_test::nir_test("nir_opt_loop_test")
{
   nir_variable *var = nir_variable_create(b->shader, nir_var_shader_in, glsl_int_type(), "in");
   in_def = nir_load_var(b, var);

   ubo_var = nir_variable_create(b->shader, nir_var_mem_ubo, glsl_int_type(), "ubo1");

   out_var = nir_variable_create(b->shader, nir_var_shader_out, glsl_int_type(), "out");
}

TEST_F(nir_opt_loop_test, opt_loop_merge_terminators_deref_after_first_if)
{
   /* Tests that opt_loop_merge_terminators creates valid nir after it merges
    * terminators that have a deref statement between them:
    */
   nir_loop *loop = nir_push_loop(b);

   /* Add first terminator */
   nir_def *one = nir_imm_int(b, 1);
   nir_def *cmp_result = nir_ieq(b, in_def, one);
   nir_if *nif = nir_push_if(b, cmp_result);
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, nif);

   nir_deref_instr *deref = nir_build_deref_var(b, ubo_var);
   nir_def *ubo_def = nir_load_deref(b, deref);

   /* Add second terminator */
   nir_def *two = nir_imm_int(b, 2);
   nir_def *cmp_result2 = nir_ieq(b, ubo_def, two);
   nir_if *nif2 = nir_push_if(b, cmp_result2);
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, nif2);

   /* Load from deref that will be moved inside the continue branch of the
    * first if-statements continue block. If not handled correctly during
    * the merge this will fail nir validation.
    */
   ubo_def = nir_load_deref(b, deref);
   nir_store_var(b, out_var, ubo_def, 1);

   nir_pop_loop(b, loop);

   ASSERT_TRUE(nir_opt_loop(b->shader));

   nir_validate_shader(b->shader, NULL);
}
