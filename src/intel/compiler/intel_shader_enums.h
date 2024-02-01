/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEL_SHADER_ENUMS_H
#define INTEL_SHADER_ENUMS_H

#include "util/enum_operators.h"

#ifdef __cplusplus
extern "C" {
#endif

enum intel_msaa_flags {
   /** Must be set whenever any dynamic MSAA is used
    *
    * This flag mostly exists to let us assert that the driver understands
    * dynamic MSAA so we don't run into trouble with drivers that don't.
    */
   INTEL_MSAA_FLAG_ENABLE_DYNAMIC = (1 << 0),

   /** True if the framebuffer is multisampled */
   INTEL_MSAA_FLAG_MULTISAMPLE_FBO = (1 << 1),

   /** True if this shader has been dispatched per-sample */
   INTEL_MSAA_FLAG_PERSAMPLE_DISPATCH = (1 << 2),

   /** True if inputs should be interpolated per-sample by default */
   INTEL_MSAA_FLAG_PERSAMPLE_INTERP = (1 << 3),

   /** True if this shader has been dispatched with alpha-to-coverage */
   INTEL_MSAA_FLAG_ALPHA_TO_COVERAGE = (1 << 4),

   /** True if this shader has been dispatched coarse
    *
    * This is intentionally chose to be bit 15 to correspond to the coarse bit
    * in the pixel interpolator messages.
    */
   INTEL_MSAA_FLAG_COARSE_PI_MSG = (1 << 15),

   /** True if this shader has been dispatched coarse
    *
    * This is intentionally chose to be bit 18 to correspond to the coarse bit
    * in the render target messages.
    */
   INTEL_MSAA_FLAG_COARSE_RT_WRITES = (1 << 18),
};
MESA_DEFINE_CPP_ENUM_BITFIELD_OPERATORS(intel_msaa_flags)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* INTEL_SHADER_ENUMS_H */
