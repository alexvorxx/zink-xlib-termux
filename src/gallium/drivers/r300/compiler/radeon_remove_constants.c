/*
 * Copyright 2010 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdbool.h>
#include "radeon_remove_constants.h"
#include "radeon_dataflow.h"
#include "util/bitscan.h"

struct const_remap_state {
	/* Used when emiting shaders constants. */
	struct const_remap *remap_table;
	/* Used when rewritign registers */
	struct const_remap *inv_remap_table;
	/* Old costant layout. */
	struct rc_constant *constants;
	/* New constant layout. */
	struct rc_constant_list new_constants;
	bool has_rel_addr;
	bool are_externals_remapped;
	bool is_identity;
};

static void remap_regs(struct rc_instruction *inst,
			struct const_remap *inv_remap_table)
{
	const struct rc_opcode_info * opcode = rc_get_opcode_info(inst->U.I.Opcode);
	for(unsigned src = 0; src < opcode->NumSrcRegs; ++src) {
		if (inst->U.I.SrcReg[src].File != RC_FILE_CONSTANT)
			continue;
		unsigned old_index = inst->U.I.SrcReg[src].Index;
		for (unsigned chan = 0; chan < 4; chan++) {
			unsigned old_swz = GET_SWZ(inst->U.I.SrcReg[src].Swizzle, chan);
			if (old_swz <= RC_SWIZZLE_W) {
				inst->U.I.SrcReg[src].Index = inv_remap_table[old_index].index[old_swz];
				SET_SWZ(inst->U.I.SrcReg[src].Swizzle, chan,
					inv_remap_table[old_index].swizzle[old_swz]);
			}
		}
	}
}

static void mark_used(void * userdata, struct rc_instruction * inst,
						struct rc_src_register * src)
{
	struct const_remap_state* d = userdata;

	if (src->File == RC_FILE_CONSTANT) {
		if (src->RelAddr) {
			d->has_rel_addr = true;
		} else {
			for (unsigned chan = 0; chan < 4; chan++) {
				char swz = GET_SWZ(src->Swizzle, chan);
				if (swz > RC_SWIZZLE_W)
					continue;
				d->constants[src->Index].UseMask |= 1 << swz;
			}
		}
	}
}

static void place_constant_in_free_slot(struct const_remap_state *s, unsigned i)
{
	unsigned count = s->new_constants.Count;
	for (unsigned chan = 0; chan < 4; chan++) {
		s->inv_remap_table[i].index[chan] = count;
		s->inv_remap_table[i].swizzle[chan] = chan;
		if (s->constants[i].UseMask & (1 << chan)) {
			s->remap_table[count].index[chan] = i;
			s->remap_table[count].swizzle[chan] = chan;
		}
	}
	s->new_constants.Constants[count] = s->constants[i];

	if (count != i) {
		if (s->constants[i].Type == RC_CONSTANT_EXTERNAL)
			s->are_externals_remapped = true;
		s->is_identity = false;
	}
	s->new_constants.Count++;
}

static void try_merge_constants_external(struct const_remap_state *s, unsigned i)
{
	assert(util_bitcount(s->constants[i].UseMask) == 1);
	for (unsigned j = 0; j < s->new_constants.Count; j++) {
		for (unsigned chan = 0; chan < 4; chan++) {
			if (s->remap_table[j].swizzle[chan] == RC_SWIZZLE_UNUSED) {
				/* Writemask to swizzle */
				unsigned swizzle = 0;
				for (; swizzle < 4; swizzle++)
					if (s->constants[i].UseMask >> swizzle == 1)
						break;
				/* Update the remap tables. */
				s->remap_table[j].index[chan] = i;
				s->remap_table[j].swizzle[chan] = swizzle;
				s->inv_remap_table[i].index[swizzle] = j;
				s->inv_remap_table[i].swizzle[swizzle] = chan;
				s->are_externals_remapped = true;
				s->is_identity = false;
				return;
			}
		}
	}
	place_constant_in_free_slot(s, i);
}

static void init_constant_remap_state(struct radeon_compiler *c, struct const_remap_state *s)
{
	s->is_identity = true;
	s->new_constants.Constants =
		malloc(sizeof(struct rc_constant) * c->Program.Constants.Count);
	s->new_constants._Reserved = c->Program.Constants.Count;
	s->constants = c->Program.Constants.Constants;

	s->remap_table = malloc(c->Program.Constants.Count * sizeof(struct const_remap));
	s->inv_remap_table =
	malloc(c->Program.Constants.Count * sizeof(struct const_remap));
	for (unsigned i = 0; i < c->Program.Constants.Count; i++) {
		/* Clear the UseMask, we will update it later. */
		s->constants[i].UseMask = 0;
		for (unsigned swz = 0; swz < 4; swz++) {
			s->remap_table[i].index[swz] = -1;
			s->remap_table[i].swizzle[swz] = RC_SWIZZLE_UNUSED;
		}
	}
}

void rc_remove_unused_constants(struct radeon_compiler *c, void *user)
{
	struct const_remap **out_remap_table = (struct const_remap **)user;
	struct rc_constant *constants = c->Program.Constants.Constants;
	struct const_remap_state remap_state = {};
	struct const_remap_state *s = &remap_state;

	if (!c->Program.Constants.Count) {
		*out_remap_table = NULL;
		return;
	}

	init_constant_remap_state(c, s);

	/* Pass 1: Mark used constants. */
	for (struct rc_instruction *inst = c->Program.Instructions.Next;
	     inst != &c->Program.Instructions; inst = inst->Next) {
		rc_for_all_reads_src(inst, mark_used, s);
	}

	/* Pass 2: If there is relative addressing or dead constant elimination
	 * is disabled, mark all externals as used. */
	if (s->has_rel_addr || !c->remove_unused_constants) {
		for (unsigned i = 0; i < c->Program.Constants.Count; i++)
			if (constants[i].Type == RC_CONSTANT_EXTERNAL)
				s->constants[i].UseMask = RC_MASK_XYZW;
	}


	/* Pass 3: Make the remapping table and remap constants.
	 * First iterate over used vec2, vec3 and vec4 externals and place them in a free
	 * slots. While we could in theory merge 2 vec2 together, its not worth it
	 * as we would have to a) check that the swizzle is valid, b) transforming
	 * xy to zw would mean we need rgb and alpha source slot, thus it would hurt
	 * us potentially during pair scheduling. */
	for (unsigned i = 0; i < c->Program.Constants.Count; i++) {
		if (constants[i].Type != RC_CONSTANT_EXTERNAL)
			continue;
		if (util_bitcount(s->constants[i].UseMask) > 1) {
			place_constant_in_free_slot(s, i);
		}
	}

	/* Now iterate over scalarar externals and put them into empty slots. */
	for (unsigned i = 0; i < c->Program.Constants.Count; i++) {
		if (constants[i].Type != RC_CONSTANT_EXTERNAL)
			continue;
		if (util_bitcount(s->constants[i].UseMask) == 1)
			try_merge_constants_external(s, i);
	}

	/* Now put the immediates and state constants. */
	for (unsigned i = 0; i < c->Program.Constants.Count; i++) {
		if (constants[i].Type == RC_CONSTANT_EXTERNAL)
			continue;
		if (util_bitcount(s->constants[i].UseMask) > 0) {
			place_constant_in_free_slot(s,  i);
		}
	}

	/*  is_identity ==> new_count == old_count
	 * !is_identity ==> new_count <  old_count */
	assert(!((s->has_rel_addr || !c->remove_unused_constants) && s->are_externals_remapped));

	/* Pass 4: Redirect reads of all constants to their new locations. */
	if (!s->is_identity) {
		for (struct rc_instruction *inst = c->Program.Instructions.Next;
		     inst != &c->Program.Instructions; inst = inst->Next) {
			remap_regs(inst, s->inv_remap_table);
		}
	}

	/* Set the new constant count. Note that new_count may be less than
	 * Count even though the remapping function is identity. In that case,
	 * the constants have been removed at the end of the array. */
	rc_constants_destroy(&c->Program.Constants);
	c->Program.Constants = s->new_constants;

	if (s->are_externals_remapped) {
		*out_remap_table = s->remap_table;
	} else {
		*out_remap_table = NULL;
		free(s->remap_table);
	}

	free(s->inv_remap_table);

	if (c->Debug & RC_DBG_LOG)
		rc_constants_print(&c->Program.Constants, s->remap_table);
}
