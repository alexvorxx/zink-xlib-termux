template = """/*
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _AGX_BUILDER_
#define _AGX_BUILDER_

#include "agx_compiler.h"

static inline agx_instr *
agx_alloc_instr(agx_builder *b, enum agx_opcode op, uint8_t nr_dests, uint8_t nr_srcs)
{
   size_t size = sizeof(agx_instr);
   size += sizeof(agx_index) * nr_dests;
   size += sizeof(agx_index) * nr_srcs;

   agx_instr *I = (agx_instr *) rzalloc_size(b->shader, size);
   I->dest = (agx_index *) (I + 1);
   I->src = I->dest + nr_dests;

   I->op = op;
   I->nr_dests = nr_dests;
   I->nr_srcs = nr_srcs;
   return I;
}

% for opcode in opcodes:
<%
   op = opcodes[opcode]
   dests = op.dests
   srcs = op.srcs
   imms = op.imms
   suffix = "_to" if dests > 0 else ""
   nr_dests = "nr_dests" if op.variable_dests else str(dests)
   nr_srcs = "nr_srcs" if op.variable_srcs else str(srcs)
%>

static inline agx_instr *
agx_${opcode}${suffix}(agx_builder *b

% if op.variable_dests:
   , unsigned nr_dests
% endif

% for dest in range(dests):
   , agx_index dst${dest}
% endfor

% if op.variable_srcs:
   , unsigned nr_srcs
% endif

% for src in range(srcs):
   , agx_index src${src}
% endfor

% for imm in imms:
   , ${imm.ctype} ${imm.name}
% endfor

) {
   agx_instr *I = agx_alloc_instr(b, AGX_OPCODE_${opcode.upper()}, ${nr_dests}, ${nr_srcs});

% for dest in range(dests):
   I->dest[${dest}] = dst${dest};
% endfor

% for src in range(srcs):
   I->src[${src}] = src${src};
% endfor

% for imm in imms:
   I->${imm.name} = ${imm.name};
% endfor

   agx_builder_insert(&b->cursor, I);
   return I;
}

% if dests == 1 and not op.variable_srcs and not op.variable_dests:
static inline agx_index
agx_${opcode}(agx_builder *b

% if srcs == 0:
   , unsigned size
% endif

% for src in range(srcs):
   , agx_index src${src}
% endfor

% for imm in imms:
   , ${imm.ctype} ${imm.name}
% endfor

) {
<%
   args = ["tmp"]
   args += ["src" + str(i) for i in range(srcs)]
   args += [imm.name for imm in imms]
%>
% if srcs == 0:
   agx_index tmp = agx_temp(b->shader, agx_size_for_bits(size));
% else:
   agx_index tmp = agx_temp(b->shader, src0.size);
% endif
   agx_${opcode}_to(b, ${", ".join(args)});
   return tmp;
}
% endif

% endfor

/* Convenience methods */

enum agx_bitop_table {
   AGX_BITOP_NOT = 0x5,
   AGX_BITOP_XOR = 0x6,
   AGX_BITOP_AND = 0x8,
   AGX_BITOP_MOV = 0xA,
   AGX_BITOP_OR  = 0xE
};

static inline agx_instr *
agx_fmov_to(agx_builder *b, agx_index dst0, agx_index src0)
{
   return agx_fadd_to(b, dst0, src0, agx_negzero());
}

static inline agx_instr *
agx_push_exec(agx_builder *b, unsigned n)
{
   return agx_if_fcmp(b, agx_zero(), agx_zero(), n, AGX_FCOND_EQ, false);
}

static inline agx_instr *
agx_ushr_to(agx_builder *b, agx_index dst, agx_index s0, agx_index s1)
{
    return agx_bfeil_to(b, dst, agx_zero(), s0, s1, 0);
}

static inline agx_index
agx_ushr(agx_builder *b, agx_index s0, agx_index s1)
{
    agx_index tmp = agx_temp(b->shader, s0.size);
    agx_ushr_to(b, tmp, s0, s1);
    return tmp;
}

#endif
"""

from mako.template import Template
from agx_opcodes import opcodes

print(Template(template).render(opcodes=opcodes))
