/*
 * Copyright 2024 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "agx_bo.h"
#include "agx_compile.h"
#include "agx_pack.h"

struct agx_linked_shader {
   /* Mapped executable memory */
   struct agx_bo *bo;

   /* Set if the linked SW vertex shader reads base vertex/instance. The VS
    * prolog can read base instance even when the API VS does not, which is why
    * this needs to be aggregated in the linker.
    */
   bool uses_base_param;

   /* Coefficient register bindings */
   struct agx_varyings_fs cf;

   /* Data structures packed for the linked program */
   struct agx_usc_shader_packed shader;
   struct agx_usc_registers_packed regs;
   struct agx_usc_fragment_properties_packed fragment_props;
   struct agx_output_select_packed osel;
   struct agx_fragment_control_packed fragment_control;
};

struct agx_linked_shader *
agx_fast_link(void *memctx, struct agx_device *dev, bool fragment,
              struct agx_shader_part *main, struct agx_shader_part *prolog,
              struct agx_shader_part *epilog, unsigned nr_samples_shaded);
