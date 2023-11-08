/*
 * Copyright 2023 Asahi Lina
 * SPDX-License-Identifier: MIT
 */
#ifndef AGX_SCRATCH_H
#define AGX_SCRATCH_H

#include "agx_device.h"

struct agx_scratch {
   struct agx_device *dev;
   struct agx_bo *buf;
};

struct agx_bo *agx_build_helper(struct agx_device *dev);

void agx_scratch_init(struct agx_device *dev, struct agx_scratch *scratch);
void agx_scratch_fini(struct agx_scratch *scratch);

#endif
