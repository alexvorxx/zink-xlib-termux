/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_PIPELINE_H
#define PANVK_PIPELINE_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdbool.h>
#include <stdint.h>

#include "vk_graphics_state.h"
#include "vk_object.h"

#include "util/pan_ir.h"

#include "pan_blend.h"
#include "pan_desc.h"

#include "panvk_varyings.h"

#define MAX_RTS 8

struct panvk_attrib_info {
   unsigned buf;
   unsigned offset;
   enum pipe_format format;
};

struct panvk_attrib_buf_info {
   unsigned stride;
   bool per_instance;
   uint32_t instance_divisor;
};

struct panvk_attribs_info {
   struct panvk_attrib_info attrib[PAN_MAX_ATTRIBUTE];
   unsigned attrib_count;
   struct panvk_attrib_buf_info buf[PAN_MAX_ATTRIBUTE];
   unsigned buf_count;
};

enum panvk_pipeline_type {
   PANVK_PIPELINE_GRAPHICS,
   PANVK_PIPELINE_COMPUTE,
};

struct panvk_pipeline {
   struct vk_object_base base;
   enum panvk_pipeline_type type;

   const struct panvk_pipeline_layout *layout;

   struct panvk_pool bin_pool;
   struct panvk_pool desc_pool;

   unsigned active_stages;

   uint64_t rsds[MESA_SHADER_STAGES];

   /* shader stage bit is set of the stage accesses storage images */
   uint32_t img_access_mask;

   unsigned tls_size;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

struct panvk_graphics_pipeline {
   struct panvk_pipeline base;

   struct panvk_varyings_info varyings;

   struct {
      struct {
         struct panvk_attribs_info attribs;
         bool writes_point_size;
      } vs;

      struct {
         uint64_t address;
         struct pan_shader_info info;
         bool required;
         bool dynamic_rsd;
         uint8_t rt_mask;
         struct mali_renderer_state_packed rsd_template;
      } fs;

      struct {
         struct pan_blend_state pstate;
         struct {
            uint8_t index;
            uint16_t bifrost_factor;
         } constant[8];
         struct mali_blend_packed bd_template[8];
         bool reads_dest;
      } blend;

      struct vk_dynamic_graphics_state dynamic;
      struct vk_vertex_input_state vi;
      struct vk_sample_locations_state sl;
      struct vk_render_pass_state rp;
   } state;
};

static struct panvk_graphics_pipeline *
panvk_pipeline_to_graphics_pipeline(struct panvk_pipeline *pipeline)
{
   if (pipeline->type != PANVK_PIPELINE_GRAPHICS)
      return NULL;

   return container_of(pipeline, struct panvk_graphics_pipeline, base);
}

struct panvk_compute_pipeline {
   struct panvk_pipeline base;

   struct pan_compute_dim local_size;
   unsigned wls_size;
};

static struct panvk_compute_pipeline *
panvk_pipeline_to_compute_pipeline(struct panvk_pipeline *pipeline)
{
   if (pipeline->type != PANVK_PIPELINE_COMPUTE)
      return NULL;

   return container_of(pipeline, struct panvk_compute_pipeline, base);
}

#endif
