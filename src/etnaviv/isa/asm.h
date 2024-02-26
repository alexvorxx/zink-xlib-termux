/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#pragma once

#include <stdint.h>

/* Number of source operands per instruction */
#define ETNA_NUM_SRC (3)

/*** operands ***/

/* destination operand */
struct etna_inst_dst {
   unsigned use        : 1; /* 0: not in use, 1: in use */
   unsigned amode      : 3; /* INST_AMODE_* */
   unsigned reg        : 7; /* register number 0..127 */
   unsigned write_mask : 4; /* INST_COMPS_* */
};

/* texture operand */
struct etna_inst_tex {
   unsigned id    : 5; /* sampler id */
   unsigned amode : 3; /* INST_AMODE_* */
   unsigned swiz  : 8; /* INST_SWIZ */
};

/* source operand */
struct etna_inst_src {
   unsigned use    : 1; /* 0: not in use, 1: in use */
   unsigned rgroup : 3; /* INST_RGROUP_* */
   union {
      struct __attribute__((__packed__)) {
         unsigned reg   : 9; /* register or uniform number 0..511 */
         unsigned swiz  : 8; /* INST_SWIZ */
         unsigned neg   : 1; /* negate (flip sign) if set */
         unsigned abs   : 1; /* absolute (remove sign) if set */
         unsigned amode : 3; /* INST_AMODE_* */
      };
      struct __attribute__((__packed__)) {
         unsigned imm_val  : 20;
         unsigned imm_type : 2;
      };
   };
};

/*** instruction ***/
struct etna_inst {
   uint8_t opcode;                         /* INST_OPCODE_* */
   uint8_t type;                           /* INST_TYPE_* */
   unsigned cond              : 5;         /* INST_CONDITION_* */
   unsigned sat               : 1;         /* saturate result between 0..1 */
   unsigned sel_bit0          : 1;         /* select low half mediump */
   unsigned sel_bit1          : 1;         /* select high half mediump */
   unsigned dst_full          : 1;         /* write to highp register */
   struct etna_inst_dst dst;               /* destination operand */
   struct etna_inst_tex tex;               /* texture operand */
   struct etna_inst_src src[ETNA_NUM_SRC]; /* source operand */
   unsigned imm;                           /* takes place of src[2] for BRANCH/CALL */
};
