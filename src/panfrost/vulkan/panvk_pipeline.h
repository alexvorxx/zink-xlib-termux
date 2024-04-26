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

#include "panvk_shader.h"

#define MAX_RTS 8

struct panvk_pipeline_shader {
   mali_ptr code;
   mali_ptr rsd;

   struct {
      mali_ptr attribs;
      unsigned buf_strides[PANVK_VARY_BUF_MAX];
   } varyings;

   struct pan_shader_info info;
   bool has_img_access;
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
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

struct panvk_graphics_pipeline {
   struct panvk_pipeline base;

   struct panvk_pipeline_shader vs;
   struct panvk_pipeline_shader fs;

   struct {
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

   struct panvk_pipeline_shader cs;

   struct pan_compute_dim local_size;
};

static struct panvk_compute_pipeline *
panvk_pipeline_to_compute_pipeline(struct panvk_pipeline *pipeline)
{
   if (pipeline->type != PANVK_PIPELINE_COMPUTE)
      return NULL;

   return container_of(pipeline, struct panvk_compute_pipeline, base);
}

#endif
