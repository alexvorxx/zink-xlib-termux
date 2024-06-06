/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

/* We reserve :
 *    - GPR 14 for perf queries
 *    - GPR 15 for conditional rendering
 */
#define MI_BUILDER_NUM_ALLOC_GPRS 14
#define MI_BUILDER_CAN_WRITE_BATCH true
#define __gen_get_batch_dwords anv_batch_emit_dwords
#define __gen_address_offset anv_address_add
#define __gen_get_batch_address(b, a) anv_batch_address(b, a)
#include "common/mi_builder.h"
