/*
 * Copyright (C) 2022 Alyssa Rosenzweig
 * Copyright (C) 2021 Collabora, Ltd.
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
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTAagxLITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIAagxLITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "agx_compiler.h"

/* Validatation doesn't make sense in release builds */
#ifndef NDEBUG

#define agx_validate_assert(stmt) if (!(stmt)) { return false; }

/*
 * If a block contains phi nodes, they must come at the start of the block. If a
 * block contains control flow, it must come after a p_logical_end marker.
 * Therefore the form of a valid block is:
 *
 *       Phi nodes
 *       General instructions
 *       Logical end
 *       Control flow instructions
 *
 * Validate that this form is satisfied.
 *
 * XXX: This only applies before we delete the logical end instructions, maybe
 * that should be deferred though?
 */
enum agx_block_state {
   AGX_BLOCK_STATE_PHI = 0,
   AGX_BLOCK_STATE_BODY = 1,
   AGX_BLOCK_STATE_CF = 2
};

static bool
agx_validate_block_form(agx_block *block)
{
   enum agx_block_state state = AGX_BLOCK_STATE_PHI;

   agx_foreach_instr_in_block(block, I) {
      switch (I->op) {
      case AGX_OPCODE_PHI:
         agx_validate_assert(state == AGX_BLOCK_STATE_PHI);
         break;

      default:
         agx_validate_assert(state != AGX_BLOCK_STATE_CF);
         state = AGX_BLOCK_STATE_BODY;
         break;

      case AGX_OPCODE_LOGICAL_END:
         agx_validate_assert(state != AGX_BLOCK_STATE_CF);
         state = AGX_BLOCK_STATE_CF;
         break;

      case AGX_OPCODE_JMP_EXEC_ANY:
      case AGX_OPCODE_JMP_EXEC_NONE:
      case AGX_OPCODE_POP_EXEC:
      case AGX_OPCODE_IF_ICMP:
      case AGX_OPCODE_ELSE_ICMP:
      case AGX_OPCODE_WHILE_ICMP:
      case AGX_OPCODE_IF_FCMP:
      case AGX_OPCODE_ELSE_FCMP:
      case AGX_OPCODE_WHILE_FCMP:
         agx_validate_assert(state == AGX_BLOCK_STATE_CF);
         break;
      }
   }

   return true;
}

static bool
agx_validate_sources(agx_instr *I)
{
   agx_foreach_src(I, s) {
      agx_index src = I->src[s];

      if (src.type == AGX_INDEX_IMMEDIATE) {
         agx_validate_assert(!src.kill);
         agx_validate_assert(!src.cache);
         agx_validate_assert(!src.discard);

         bool ldst =
            (I->op == AGX_OPCODE_DEVICE_LOAD) ||
            (I->op == AGX_OPCODE_UNIFORM_STORE);

         /* Immediates are encoded as 8-bit (16-bit for memory load/store). For
          * integers, they extend to 16-bit. For floating point, they are 8-bit
          * minifloats. The 8-bit minifloats are a strict subset of 16-bit
          * standard floats, so we treat them as such in the IR, with an
          * implicit f16->f32 for 32-bit floating point operations.
          */
         agx_validate_assert(src.size == AGX_SIZE_16);
         agx_validate_assert(src.value < (1 << (ldst ? 16 : 8)));
      }
   }

   return true;
}

static bool
agx_validate_defs(agx_instr *I, BITSET_WORD *defs)
{
   agx_foreach_ssa_src(I, s) {
      /* Skip phis, they're special in loop headers */
      if (I->op == AGX_OPCODE_PHI)
         break;

      /* Sources must be defined before their use */
      if (!BITSET_TEST(defs, I->src[s].value))
         return false;
   }

   agx_foreach_ssa_dest(I, d) {
      /* Static single assignment */ 
      if (BITSET_TEST(defs, I->dest[d].value))
         return false;

      BITSET_SET(defs, I->dest[d].value);
   }

   return true;
}

void
agx_validate(agx_context *ctx, const char *after)
{
   bool fail = false;

   if (agx_debug & AGX_DBG_NOVALIDATE)
      return;

   agx_foreach_block(ctx, block) {
      if (!agx_validate_block_form(block)) {
         fprintf(stderr, "Invalid block form after %s\n", after);
         agx_print_block(block, stdout);
         fail = true;
      }
   }

   {
      BITSET_WORD *defs = calloc(sizeof(BITSET_WORD), BITSET_WORDS(ctx->alloc));

      agx_foreach_instr_global(ctx, I) {
         if (!agx_validate_defs(I, defs)) {
               fprintf(stderr, "Invalid defs after %s\n", after);
               agx_print_instr(I, stdout);
               fail = true;
         }
      }

      free(defs);
   }

   agx_foreach_instr_global(ctx, I) {
      if (!agx_validate_sources(I)) {
            fprintf(stderr, "Invalid sources form after %s\n", after);
            agx_print_instr(I, stdout);
            fail = true;
      }
   }

   /* TODO: Validate more invariants */

   if (fail) {
      agx_print_shader(ctx, stderr);
      exit(1);
   }
}

#endif /* NDEBUG */
