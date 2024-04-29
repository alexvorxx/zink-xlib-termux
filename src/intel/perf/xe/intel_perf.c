/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "perf/xe/intel_perf.h"

#include <sys/stat.h>

#include "perf/intel_perf.h"
#include "intel_perf_common.h"
#include "intel/common/i915/intel_gem.h"

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

bool
xe_oa_metrics_available(struct intel_perf_config *perf, int fd, bool use_register_snapshots)
{
   bool perf_oa_available = false;
   struct stat sb;

   /* The existence of this file implies that this Xe KMD version supports
    * perf interface.
    */
   if (stat("/proc/sys/dev/xe/perf_stream_paranoid", &sb) == 0) {
      uint64_t paranoid = 1;

      /* Now we need to check if application has privileges to access perf
       * interface.
       *
       * TODO: this approach does not takes into account applications running
       * with CAP_PERFMON privileges.
       */
      read_file_uint64("/proc/sys/dev/xe/perf_stream_paranoid", &paranoid);
      if (paranoid == 0 || geteuid() == 0)
         perf_oa_available = true;
   }

   return perf_oa_available;
}

uint64_t
xe_add_config(struct intel_perf_config *perf, int fd,
              const struct intel_perf_registers *config,
              const char *guid)
{
   struct drm_xe_oa_config xe_config = {};
   struct drm_xe_perf_param perf_param = {
      .perf_type = DRM_XE_PERF_TYPE_OA,
      .perf_op = DRM_XE_PERF_OP_ADD_CONFIG,
      .param = (uintptr_t)&xe_config,
   };
   uint32_t *regs;
   int ret;

   memcpy(xe_config.uuid, guid, sizeof(xe_config.uuid));

   xe_config.n_regs = config->n_mux_regs + config->n_b_counter_regs + config->n_flex_regs;
   assert(xe_config.n_regs > 0);

   regs = malloc(sizeof(uint64_t) * xe_config.n_regs);
   xe_config.regs_ptr = (uintptr_t)regs;

   memcpy(regs, config->mux_regs, config->n_mux_regs * sizeof(uint64_t));
   regs += 2 * config->n_mux_regs;
   memcpy(regs, config->b_counter_regs, config->n_b_counter_regs * sizeof(uint64_t));
   regs += 2 * config->n_b_counter_regs;
   memcpy(regs, config->flex_regs, config->n_flex_regs * sizeof(uint64_t));

   ret = intel_ioctl(fd, DRM_IOCTL_XE_PERF, &perf_param);
   free(regs);
   return ret > 0 ? ret : 0;
}

void
xe_remove_config(struct intel_perf_config *perf, int fd, uint64_t config_id)
{
   struct drm_xe_perf_param perf_param = {
      .perf_type = DRM_XE_PERF_TYPE_OA,
      .perf_op = DRM_XE_PERF_OP_REMOVE_CONFIG,
      .param = (uintptr_t)&config_id,
   };

   intel_ioctl(fd, DRM_IOCTL_XE_PERF, &perf_param);
}
