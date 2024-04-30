/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "perf/i915/intel_perf.h"

#include "common/intel_gem.h"
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

int
i915_perf_stream_open(struct intel_perf_config *perf_config, int drm_fd,
                      uint32_t ctx_id, uint64_t metrics_set_id,
                      uint64_t report_format, uint64_t period_exponent,
                      bool hold_preemption, bool enable)
{
   uint64_t properties[DRM_I915_PERF_PROP_MAX * 2];
   uint32_t p = 0;

   /* Single context sampling if valid context id. */
   if (ctx_id != INTEL_PERF_INVALID_CTX_ID) {
      properties[p++] = DRM_I915_PERF_PROP_CTX_HANDLE;
      properties[p++] = ctx_id;
   }

   /* Include OA reports in samples */
   properties[p++] = DRM_I915_PERF_PROP_SAMPLE_OA;
   properties[p++] = true;

   /* OA unit configuration */
   properties[p++] = DRM_I915_PERF_PROP_OA_METRICS_SET;
   properties[p++] = metrics_set_id;

   properties[p++] = DRM_I915_PERF_PROP_OA_FORMAT;
   properties[p++] = report_format;

   properties[p++] = DRM_I915_PERF_PROP_OA_EXPONENT;
   properties[p++] = period_exponent;

   if (hold_preemption) {
      properties[p++] = DRM_I915_PERF_PROP_HOLD_PREEMPTION;
      properties[p++] = true;
   }

   /* If global SSEU is available, pin it to the default. This will ensure on
    * Gfx11 for instance we use the full EU array. Initially when perf was
    * enabled we would use only half on Gfx11 because of functional
    * requirements.
    *
    * Not supported on Gfx12.5+.
    */
   if (intel_perf_has_global_sseu(perf_config) &&
       perf_config->devinfo->verx10 < 125) {
      properties[p++] = DRM_I915_PERF_PROP_GLOBAL_SSEU;
      properties[p++] = (uintptr_t) &perf_config->sseu;
   }

   assert(p <= ARRAY_SIZE(properties));

   struct drm_i915_perf_open_param param = {
      .flags = I915_PERF_FLAG_FD_CLOEXEC |
               I915_PERF_FLAG_FD_NONBLOCK |
               (enable ? 0 : I915_PERF_FLAG_DISABLED),
      .num_properties = p / 2,
      .properties_ptr = (uintptr_t) properties,
   };
   int fd = intel_ioctl(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param);
   return fd > -1 ? fd : 0;
}
