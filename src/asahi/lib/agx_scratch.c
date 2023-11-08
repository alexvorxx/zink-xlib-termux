/*
 * Copyright 2023 Asahi Lina
 * SPDX-License-Identifier: MIT
 */

#include "agx_scratch.h"
#include "asahi/compiler/agx_compile.h"
#include "agx_bo.h"
#include "libagx_shaders.h"
#include "nir.h"
#include "nir_builder_opcodes.h"

struct agx_bo *
agx_build_helper(struct agx_device *dev)
{
   struct agx_bo *bo;
   struct util_dynarray binary;

   util_dynarray_init(&binary, NULL);

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "Helper shader");

   libagx_helper(&b);

   UNUSED struct agx_uncompiled_shader_info info;
   UNUSED struct agx_shader_info compiled_info;
   struct agx_shader_key key = {.libagx = dev->libagx};

   agx_preprocess_nir(b.shader, dev->libagx, false, &info);
   agx_compile_shader_nir(b.shader, &key, NULL, &binary, &compiled_info);

   bo = agx_bo_create(dev, binary.size,
                      AGX_BO_READONLY | AGX_BO_EXEC | AGX_BO_LOW_VA,
                      "Helper shader");
   assert(bo);
   memcpy(bo->ptr.cpu, binary.data, binary.size);
   util_dynarray_fini(&binary);
   ralloc_free(b.shader);

   return bo;
}

void
agx_scratch_init(struct agx_device *dev, struct agx_scratch *scratch)
{
}

void
agx_scratch_fini(struct agx_scratch *scratch)
{
}
