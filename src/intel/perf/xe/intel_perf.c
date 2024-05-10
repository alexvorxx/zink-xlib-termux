/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "perf/xe/intel_perf.h"

#include <sys/stat.h>

#include "perf/intel_perf.h"

#include "drm-uapi/xe_drm.h"

#define FIELD_PREP_ULL(_mask, _val) (((_val) << (ffsll(_mask) - 1)) & (_mask))

uint64_t xe_perf_get_oa_format(struct intel_perf_config *perf)
{
   uint64_t fmt = FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_FMT_TYPE, DRM_XE_OA_FMT_TYPE_OAG);

   /* same as I915_OA_FORMAT_A24u40_A14u32_B8_C8 and
    * I915_OA_FORMAT_A32u40_A4u32_B8_C8 returned for gfx 125+ and gfx 120
    * respectively.
    */
   fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SEL, 5);
   fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE, 0);
   fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_BC_REPORT, 0);

   return fmt;
}
