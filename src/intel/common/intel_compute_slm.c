/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "intel_compute_slm.h"

#include <assert.h>

#include "util/macros.h"
#include "util/u_math.h"

/* Shared Local Memory Size is specified as powers of two,
 * and also have a Gen-dependent minimum value if not zero.
 */
uint32_t
intel_compute_slm_calculate_size(unsigned gen, uint32_t bytes)
{
   assert(bytes <= 64 * 1024);
   if (bytes > 0)
      return MAX2(util_next_power_of_two(bytes), gen >= 9 ? 1024 : 4096);
   else
      return 0;
}

uint32_t
intel_compute_slm_encode_size(unsigned gen, uint32_t bytes)
{
   uint32_t slm_size = 0;

   /* Shared Local Memory is specified as powers of two, and encoded in
    * INTERFACE_DESCRIPTOR_DATA with the following representations:
    *
    * Size   | 0 kB | 1 kB | 2 kB | 4 kB | 8 kB | 16 kB | 32 kB | 64 kB |
    * -------------------------------------------------------------------
    * Gfx7-8 |    0 | none | none |    1 |    2 |     4 |     8 |    16 |
    * -------------------------------------------------------------------
    * Gfx9+  |    0 |    1 |    2 |    3 |    4 |     5 |     6 |     7 |
    */

   if (bytes > 0) {
      slm_size = intel_compute_slm_calculate_size(gen, bytes);
      assert(util_is_power_of_two_nonzero(slm_size));

      if (gen >= 9) {
         /* Turn an exponent of 10 (1024 kB) into 1. */
         assert(slm_size >= 1024);
         slm_size = ffs(slm_size) - 10;
      } else {
         assert(slm_size >= 4096);
         /* Convert to the pre-Gfx9 representation. */
         slm_size = slm_size / 4096;
      }
   }

   return slm_size;
}
