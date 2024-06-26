/*
 * Copyright © 2018 Valve Corporation
 * Copyright © 2018 Google
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_ir.h"

#include "util/u_math.h"

#include <set>
#include <vector>

namespace aco {
RegisterDemand
get_live_changes(aco_ptr<Instruction>& instr)
{
   RegisterDemand changes;
   for (const Definition& def : instr->definitions) {
      if (!def.isTemp() || def.isKill())
         continue;
      changes += def.getTemp();
   }

   for (const Operand& op : instr->operands) {
      if (!op.isTemp() || !op.isFirstKill())
         continue;
      changes -= op.getTemp();
   }

   return changes;
}

RegisterDemand
get_additional_operand_demand(Instruction* instr)
{
   RegisterDemand additional_demand;
   int op_idx = get_op_fixed_to_def(instr);
   if (op_idx != -1 && !instr->operands[op_idx].isKill())
      additional_demand += instr->definitions[0].getTemp();

   return additional_demand;
}

RegisterDemand
get_temp_registers(aco_ptr<Instruction>& instr)
{
   RegisterDemand demand_before;
   RegisterDemand demand_after;

   for (Definition def : instr->definitions) {
      if (def.isKill())
         demand_after += def.getTemp();
      else if (def.isTemp())
         demand_before -= def.getTemp();
   }

   for (Operand op : instr->operands) {
      if (op.isFirstKill()) {
         demand_before += op.getTemp();
         if (op.isLateKill())
            demand_after += op.getTemp();
      }
   }

   demand_before += get_additional_operand_demand(instr.get());
   demand_after.update(demand_before);
   return demand_after;
}

namespace {

struct live_ctx {
   Program* program;
   int worklist;
};

bool
instr_needs_vcc(Instruction* instr)
{
   if (instr->isVOPC())
      return true;
   if (instr->isVOP2() && !instr->isVOP3()) {
      if (instr->operands.size() == 3 && instr->operands[2].isTemp() &&
          instr->operands[2].regClass().type() == RegType::sgpr)
         return true;
      if (instr->definitions.size() == 2)
         return true;
   }
   return false;
}

void
process_live_temps_per_block(live_ctx& ctx, Block* block)
{
   RegisterDemand new_demand;
   block->register_demand = RegisterDemand();
   IDSet live = ctx.program->live.live_out[block->index];

   /* initialize register demand */
   for (unsigned t : live)
      new_demand += Temp(t, ctx.program->temp_rc[t]);

   /* traverse the instructions backwards */
   int idx;
   for (idx = block->instructions.size() - 1; idx >= 0; idx--) {
      Instruction* insn = block->instructions[idx].get();
      if (is_phi(insn))
         break;

      ctx.program->needs_vcc |= instr_needs_vcc(insn);
      insn->register_demand = RegisterDemand(new_demand.vgpr, new_demand.sgpr);

      /* KILL */
      for (Definition& definition : insn->definitions) {
         if (!definition.isTemp()) {
            continue;
         }
         if (definition.isFixed() && definition.physReg() == vcc)
            ctx.program->needs_vcc = true;

         const Temp temp = definition.getTemp();
         const size_t n = live.erase(temp.id());

         if (n) {
            new_demand -= temp;
            definition.setKill(false);
         } else {
            insn->register_demand += temp;
            definition.setKill(true);
         }
      }

      /* we need to do this in a separate loop because the next one can
       * setKill() for several operands at once and we don't want to
       * overwrite that in a later iteration */
      for (Operand& op : insn->operands)
         op.setKill(false);

      /* GEN */
      for (unsigned i = 0; i < insn->operands.size(); ++i) {
         Operand& operand = insn->operands[i];
         if (!operand.isTemp())
            continue;
         if (operand.isFixed() && operand.physReg() == vcc)
            ctx.program->needs_vcc = true;
         const Temp temp = operand.getTemp();
         const bool inserted = live.insert(temp.id()).second;
         if (inserted) {
            operand.setFirstKill(true);
            for (unsigned j = i + 1; j < insn->operands.size(); ++j) {
               if (insn->operands[j].isTemp() && insn->operands[j].tempId() == operand.tempId()) {
                  insn->operands[j].setFirstKill(false);
                  insn->operands[j].setKill(true);
               }
            }
            if (operand.isLateKill())
               insn->register_demand += temp;
            new_demand += temp;
         }
      }

      RegisterDemand before_instr = new_demand + get_additional_operand_demand(insn);
      insn->register_demand.update(before_instr);
      block->register_demand.update(insn->register_demand);
   }

   /* handle phi definitions */
   for (int phi_idx = 0; phi_idx <= idx; phi_idx++) {
      Instruction* insn = block->instructions[phi_idx].get();
      insn->register_demand = new_demand;

      assert(is_phi(insn) && insn->definitions.size() == 1);
      if (!insn->definitions[0].isTemp()) {
         assert(insn->definitions[0].isFixed() && insn->definitions[0].physReg() == exec);
         continue;
      }
      Definition& definition = insn->definitions[0];
      if (definition.isFixed() && definition.physReg() == vcc)
         ctx.program->needs_vcc = true;
      const Temp temp = definition.getTemp();
      const size_t n = live.erase(temp.id());

      if (n) {
         definition.setKill(false);
      } else {
         definition.setKill(true);
      }
   }

   /* now, we need to merge the live-ins into the live-out sets */
   bool fast_merge =
      block->logical_preds.size() == 0 || block->logical_preds == block->linear_preds;

#ifndef NDEBUG
   if ((block->linear_preds.empty() && !live.empty()) ||
       (block->logical_preds.empty() && new_demand.vgpr > 0))
      fast_merge = false; /* we might have errors */
#endif

   if (fast_merge) {
      for (unsigned pred_idx : block->linear_preds) {
         if (ctx.program->live.live_out[pred_idx].insert(live))
            ctx.worklist = std::max<int>(ctx.worklist, pred_idx);
      }
   } else {
      for (unsigned t : live) {
         RegClass rc = ctx.program->temp_rc[t];
         Block::edge_vec& preds = rc.is_linear() ? block->linear_preds : block->logical_preds;

#ifndef NDEBUG
         if (preds.empty())
            aco_err(ctx.program, "Temporary never defined or are defined after use: %%%d in BB%d",
                    t, block->index);
#endif

         for (unsigned pred_idx : preds) {
            auto it = ctx.program->live.live_out[pred_idx].insert(t);
            if (it.second)
               ctx.worklist = std::max<int>(ctx.worklist, pred_idx);
         }
      }
   }

   /* handle phi operands */
   for (int phi_idx = 0; phi_idx <= idx; phi_idx++) {
      Instruction* insn = block->instructions[phi_idx].get();
      assert(is_phi(insn));
      /* Ignore dead phis. */
      if (insn->definitions[0].isKill())
         continue;
      /* directly insert into the predecessors live-out set */
      Block::edge_vec& preds =
         insn->opcode == aco_opcode::p_phi ? block->logical_preds : block->linear_preds;
      for (unsigned i = 0; i < preds.size(); ++i) {
         Operand& operand = insn->operands[i];
         if (!operand.isTemp())
            continue;
         if (operand.isFixed() && operand.physReg() == vcc)
            ctx.program->needs_vcc = true;
         /* check if we changed an already processed block */
         const bool inserted = ctx.program->live.live_out[preds[i]].insert(operand.tempId()).second;
         if (inserted)
            ctx.worklist = std::max<int>(ctx.worklist, preds[i]);

         /* set if the operand is killed by this (or another) phi instruction */
         operand.setKill(!live.count(operand.tempId()));
      }
   }

   block->live_in_demand = new_demand;
   block->live_in_demand.sgpr += 2; /* Add 2 SGPRs for potential long-jumps. */
   block->register_demand.update(block->live_in_demand);
   ctx.program->max_reg_demand.update(block->register_demand);

   assert(!block->linear_preds.empty() || (new_demand == RegisterDemand() && live.empty()));
}

unsigned
calc_waves_per_workgroup(Program* program)
{
   /* When workgroup size is not known, just go with wave_size */
   unsigned workgroup_size =
      program->workgroup_size == UINT_MAX ? program->wave_size : program->workgroup_size;

   return align(workgroup_size, program->wave_size) / program->wave_size;
}
} /* end namespace */

bool
uses_scratch(Program* program)
{
   /* RT uses scratch but we don't yet know how much. */
   return program->config->scratch_bytes_per_wave || program->stage == raytracing_cs;
}

uint16_t
get_extra_sgprs(Program* program)
{
   /* We don't use this register on GFX6-8 and it's removed on GFX10+. */
   bool needs_flat_scr = uses_scratch(program) && program->gfx_level == GFX9;

   if (program->gfx_level >= GFX10) {
      assert(!program->dev.xnack_enabled);
      return 0;
   } else if (program->gfx_level >= GFX8) {
      if (needs_flat_scr)
         return 6;
      else if (program->dev.xnack_enabled)
         return 4;
      else if (program->needs_vcc)
         return 2;
      else
         return 0;
   } else {
      assert(!program->dev.xnack_enabled);
      if (needs_flat_scr)
         return 4;
      else if (program->needs_vcc)
         return 2;
      else
         return 0;
   }
}

uint16_t
get_sgpr_alloc(Program* program, uint16_t addressable_sgprs)
{
   uint16_t sgprs = addressable_sgprs + get_extra_sgprs(program);
   uint16_t granule = program->dev.sgpr_alloc_granule;
   return ALIGN_NPOT(std::max(sgprs, granule), granule);
}

uint16_t
get_vgpr_alloc(Program* program, uint16_t addressable_vgprs)
{
   assert(addressable_vgprs <= program->dev.vgpr_limit);
   uint16_t granule = program->dev.vgpr_alloc_granule;
   return ALIGN_NPOT(std::max(addressable_vgprs, granule), granule);
}

unsigned
round_down(unsigned a, unsigned b)
{
   return a - (a % b);
}

uint16_t
get_addr_sgpr_from_waves(Program* program, uint16_t waves)
{
   /* it's not possible to allocate more than 128 SGPRs */
   uint16_t sgprs = std::min(program->dev.physical_sgprs / waves, 128);
   sgprs = round_down(sgprs, program->dev.sgpr_alloc_granule);
   sgprs -= get_extra_sgprs(program);
   return std::min(sgprs, program->dev.sgpr_limit);
}

uint16_t
get_addr_vgpr_from_waves(Program* program, uint16_t waves)
{
   uint16_t vgprs = program->dev.physical_vgprs / waves;
   vgprs = vgprs / program->dev.vgpr_alloc_granule * program->dev.vgpr_alloc_granule;
   vgprs -= program->config->num_shared_vgprs / 2;
   return std::min(vgprs, program->dev.vgpr_limit);
}

void
calc_min_waves(Program* program)
{
   unsigned waves_per_workgroup = calc_waves_per_workgroup(program);
   unsigned simd_per_cu_wgp = program->dev.simd_per_cu * (program->wgp_mode ? 2 : 1);
   program->min_waves = DIV_ROUND_UP(waves_per_workgroup, simd_per_cu_wgp);
}

uint16_t
max_suitable_waves(Program* program, uint16_t waves)
{
   unsigned num_simd = program->dev.simd_per_cu * (program->wgp_mode ? 2 : 1);
   unsigned waves_per_workgroup = calc_waves_per_workgroup(program);
   unsigned num_workgroups = waves * num_simd / waves_per_workgroup;

   /* Adjust #workgroups for LDS */
   unsigned lds_per_workgroup = align(program->config->lds_size * program->dev.lds_encoding_granule,
                                      program->dev.lds_alloc_granule);

   if (program->stage == fragment_fs) {
      /* PS inputs are moved from PC (parameter cache) to LDS before PS waves are launched.
       * Each PS input occupies 3x vec4 of LDS space. See Figure 10.3 in GCN3 ISA manual.
       * These limit occupancy the same way as other stages' LDS usage does.
       */
      unsigned lds_bytes_per_interp = 3 * 16;
      unsigned lds_param_bytes = lds_bytes_per_interp * program->info.ps.num_interp;
      lds_per_workgroup += align(lds_param_bytes, program->dev.lds_alloc_granule);
   }
   unsigned lds_limit = program->wgp_mode ? program->dev.lds_limit * 2 : program->dev.lds_limit;
   if (lds_per_workgroup)
      num_workgroups = std::min(num_workgroups, lds_limit / lds_per_workgroup);

   /* Hardware limitation */
   if (waves_per_workgroup > 1)
      num_workgroups = std::min(num_workgroups, program->wgp_mode ? 32u : 16u);

   /* Adjust #waves for workgroup multiples:
    * In cases like waves_per_workgroup=3 or lds=65536 and
    * waves_per_workgroup=1, we want the maximum possible number of waves per
    * SIMD and not the minimum. so DIV_ROUND_UP is used
    */
   unsigned workgroup_waves = num_workgroups * waves_per_workgroup;
   return DIV_ROUND_UP(workgroup_waves, num_simd);
}

void
update_vgpr_sgpr_demand(Program* program, const RegisterDemand new_demand)
{
   assert(program->min_waves >= 1);
   uint16_t sgpr_limit = get_addr_sgpr_from_waves(program, program->min_waves);
   uint16_t vgpr_limit = get_addr_vgpr_from_waves(program, program->min_waves);

   /* this won't compile, register pressure reduction necessary */
   if (new_demand.vgpr > vgpr_limit || new_demand.sgpr > sgpr_limit) {
      program->num_waves = 0;
      program->max_reg_demand = new_demand;
   } else {
      program->num_waves = program->dev.physical_sgprs / get_sgpr_alloc(program, new_demand.sgpr);
      uint16_t vgpr_demand =
         get_vgpr_alloc(program, new_demand.vgpr) + program->config->num_shared_vgprs / 2;
      program->num_waves =
         std::min<uint16_t>(program->num_waves, program->dev.physical_vgprs / vgpr_demand);
      program->num_waves = std::min(program->num_waves, program->dev.max_waves_per_simd);

      /* Adjust for LDS and workgroup multiples and calculate max_reg_demand */
      program->num_waves = max_suitable_waves(program, program->num_waves);
      program->max_reg_demand.vgpr = get_addr_vgpr_from_waves(program, program->num_waves);
      program->max_reg_demand.sgpr = get_addr_sgpr_from_waves(program, program->num_waves);
   }
}

void
live_var_analysis(Program* program)
{
   program->live.live_out.clear();
   program->live.memory.release();
   program->live.live_out.resize(program->blocks.size(), IDSet(program->live.memory));
   program->max_reg_demand = RegisterDemand();
   program->needs_vcc = program->gfx_level >= GFX10;

   live_ctx ctx;
   ctx.program = program;
   ctx.worklist = program->blocks.size() - 1;

   /* this implementation assumes that the block idx corresponds to the block's position in
    * program->blocks vector */
   while (ctx.worklist >= 0) {
      process_live_temps_per_block(ctx, &program->blocks[ctx.worklist--]);
   }

   /* calculate the program's register demand and number of waves */
   if (program->progress < CompilationProgress::after_ra)
      update_vgpr_sgpr_demand(program, program->max_reg_demand);
}

} // namespace aco
