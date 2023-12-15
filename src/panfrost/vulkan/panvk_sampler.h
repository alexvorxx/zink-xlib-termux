/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_SAMPLER_H
#define PANVK_SAMPLER_H

#include <stdint.h>

#include "vk_sampler.h"

#define SAMPLER_DESC_WORDS 8

struct panvk_sampler {
   struct vk_sampler vk;
   uint32_t desc[SAMPLER_DESC_WORDS];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

#endif
