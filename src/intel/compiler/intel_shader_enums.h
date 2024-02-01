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

/**
 * @defgroup Tessellator parameter enumerations.
 *
 * These correspond to the hardware values in 3DSTATE_TE, and are provided
 * as part of the tessellation evaluation shader.
 *
 * @{
 */
enum intel_tess_partitioning {
   INTEL_TESS_PARTITIONING_INTEGER         = 0,
   INTEL_TESS_PARTITIONING_ODD_FRACTIONAL  = 1,
   INTEL_TESS_PARTITIONING_EVEN_FRACTIONAL = 2,
};

enum intel_tess_output_topology {
   INTEL_TESS_OUTPUT_TOPOLOGY_POINT   = 0,
   INTEL_TESS_OUTPUT_TOPOLOGY_LINE    = 1,
   INTEL_TESS_OUTPUT_TOPOLOGY_TRI_CW  = 2,
   INTEL_TESS_OUTPUT_TOPOLOGY_TRI_CCW = 3,
};

enum intel_tess_domain {
   INTEL_TESS_DOMAIN_QUAD    = 0,
   INTEL_TESS_DOMAIN_TRI     = 1,
   INTEL_TESS_DOMAIN_ISOLINE = 2,
};
/** @} */

enum intel_shader_dispatch_mode {
   INTEL_DISPATCH_MODE_4X1_SINGLE = 0,
   INTEL_DISPATCH_MODE_4X2_DUAL_INSTANCE = 1,
   INTEL_DISPATCH_MODE_4X2_DUAL_OBJECT = 2,
   INTEL_DISPATCH_MODE_SIMD8 = 3,

   INTEL_DISPATCH_MODE_TCS_SINGLE_PATCH = 0,
   INTEL_DISPATCH_MODE_TCS_MULTI_PATCH = 2,
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* INTEL_SHADER_ENUMS_H */
