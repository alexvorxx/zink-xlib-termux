/*
 * Copyright © 2014 Broadcom
 * SPDX-License-Identifier: MIT
 */

#ifndef NIR_TO_RC_H
#define NIR_TO_RC_H

#include <stdbool.h>
#include "pipe/p_defines.h"

struct nir_shader;
struct pipe_screen;
struct pipe_shader_state;

struct nir_to_rc_options {
   bool lower_cmp;
   /* Emit MAX(a,-a) instead of abs src modifier) */
   bool lower_fabs;
   bool unoptimized_ra;
   bool lower_ssbo_bindings;
   uint32_t ubo_vec4_max;
};

const void *nir_to_rc(struct nir_shader *s,
                        struct pipe_screen *screen);

const void *nir_to_rc_options(struct nir_shader *s,
                                struct pipe_screen *screen,
                                const struct nir_to_rc_options *ntr_options);

#endif /* NIR_TO_RC_H */
