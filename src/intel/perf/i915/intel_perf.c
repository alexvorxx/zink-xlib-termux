/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "perf/i915/intel_perf.h"

#include "perf/intel_perf.h"

#include "drm-uapi/i915_drm.h"

uint64_t i915_perf_get_oa_format(struct intel_perf_config *perf)
{
   if (perf->devinfo->verx10 <= 75)
      return I915_OA_FORMAT_A45_B8_C8;
   else if (perf->devinfo->verx10 <= 120)
      return I915_OA_FORMAT_A32u40_A4u32_B8_C8;
   else
      return I915_OA_FORMAT_A24u40_A14u32_B8_C8;
}
