/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_DESC_STATE_H
#define PANVK_CMD_DESC_STATE_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdint.h>

#include "genxml/gen_macros.h"

#include "panvk_descriptor_set.h"
#include "panvk_macros.h"
#include "panvk_pipeline.h"

#include "vk_command_buffer.h"

#include "pan_pool.h"

struct panvk_descriptor_state {
   const struct panvk_descriptor_set *sets[MAX_SETS];
   struct panvk_push_descriptor_set *push_sets[MAX_SETS];

   struct {
      struct mali_uniform_buffer_packed ubos[MAX_DYNAMIC_UNIFORM_BUFFERS];
      struct panvk_ssbo_addr ssbos[MAX_DYNAMIC_STORAGE_BUFFERS];
   } dyn;
   mali_ptr ubos;
   mali_ptr textures;
   mali_ptr samplers;
   mali_ptr dyn_desc_ubo;

   struct {
      mali_ptr attribs;
      mali_ptr attrib_bufs;
   } img;
};

void panvk_per_arch(cmd_prepare_push_sets)(
   struct pan_pool *desc_pool_base, struct panvk_descriptor_state *desc_state,
   const struct panvk_pipeline *pipeline);

void panvk_per_arch(cmd_unprepare_push_sets)(
   struct panvk_descriptor_state *desc_state);

void panvk_per_arch(cmd_prepare_ubos)(struct pan_pool *desc_pool_base,
                                      struct panvk_descriptor_state *desc_state,
                                      const struct panvk_pipeline *pipeline);

void panvk_per_arch(cmd_prepare_textures)(
   struct pan_pool *desc_pool_base, struct panvk_descriptor_state *desc_state,
   const struct panvk_pipeline *pipeline);

void panvk_per_arch(cmd_prepare_samplers)(
   struct pan_pool *desc_pool_base, struct panvk_descriptor_state *desc_state,
   const struct panvk_pipeline *pipeline);

void panvk_per_arch(fill_img_attribs)(struct panvk_descriptor_state *desc_state,
                                      const struct panvk_pipeline *pipeline,
                                      void *attrib_bufs, void *attribs,
                                      unsigned first_buf);

void panvk_per_arch(prepare_img_attribs)(
   struct pan_pool *desc_pool_base, struct panvk_descriptor_state *desc_state,
   const struct panvk_pipeline *pipeline);

void panvk_per_arch(cmd_desc_state_reset)(
   struct panvk_descriptor_state *gfx_desc_state,
   struct panvk_descriptor_state *compute_desc_state);

void panvk_per_arch(cmd_desc_state_cleanup)(
   struct vk_command_buffer *cmdbuf,
   struct panvk_descriptor_state *gfx_desc_state,
   struct panvk_descriptor_state *compute_desc_state);

void panvk_per_arch(cmd_desc_state_bind_sets)(
   struct panvk_descriptor_state *desc_state, VkPipelineLayout layout,
   uint32_t first_set, uint32_t desc_set_count,
   const VkDescriptorSet *desc_sets, uint32_t dyn_offset_count,
   const uint32_t *dyn_offsets);

struct panvk_push_descriptor_set *panvk_per_arch(cmd_push_descriptors)(
   struct vk_command_buffer *cmdbuf, struct panvk_descriptor_state *desc_state,
   uint32_t set);

#endif
