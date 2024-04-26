/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_BUFFER_H
#define PANVK_CMD_BUFFER_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include <stdint.h>

#include "vulkan/runtime/vk_command_buffer.h"

#include "panvk_descriptor_set.h"
#include "panvk_descriptor_set_layout.h"
#include "panvk_device.h"
#include "panvk_macros.h"
#include "panvk_mempool.h"
#include "panvk_pipeline.h"
#include "panvk_shader.h"

#include "pan_jc.h"

#include "util/list.h"

#include "genxml/gen_macros.h"

#define MAX_BIND_POINTS         2 /* compute + graphics */
#define MAX_VBS                 16
#define MAX_PUSH_CONSTANTS_SIZE 128

struct panvk_batch {
   struct list_head node;
   struct util_dynarray jobs;
   struct util_dynarray event_ops;
   struct pan_jc jc;
   struct {
      struct panfrost_ptr desc;
      uint32_t bo_count;

      /* One slot per color, two more slots for the depth/stencil buffers. */
      struct pan_kmod_bo *bos[MAX_RTS + 2];
   } fb;
   struct {
      struct pan_kmod_bo *src, *dst;
   } blit;
   struct panfrost_ptr tls;
   mali_ptr fragment_job;
   struct {
      struct pan_tiler_context ctx;
      struct panfrost_ptr heap_desc;
      struct panfrost_ptr ctx_desc;
      struct mali_tiler_heap_packed heap_templ;
      struct mali_tiler_context_packed ctx_templ;
   } tiler;
   struct pan_tls_info tlsinfo;
   unsigned wls_total_size;
   bool issued;
};

enum panvk_cmd_event_op_type {
   PANVK_EVENT_OP_SET,
   PANVK_EVENT_OP_RESET,
   PANVK_EVENT_OP_WAIT,
};

struct panvk_cmd_event_op {
   enum panvk_cmd_event_op_type type;
   struct panvk_event *event;
};

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
   mali_ptr push_uniforms;

   struct {
      mali_ptr attribs;
      mali_ptr attrib_bufs;
   } img;
};

struct panvk_attrib_buf {
   mali_ptr address;
   unsigned size;
};

struct panvk_cmd_graphics_state {
   struct panvk_descriptor_state desc_state;
   const struct panvk_graphics_pipeline *pipeline;

   struct {
      struct vk_vertex_input_state vi;
      struct vk_sample_locations_state sl;
   } dynamic;

   uint32_t dirty;

   struct panvk_graphics_sysvals sysvals;

   struct {
      mali_ptr rsd;
   } fs;

   struct {
      mali_ptr attribs;
      mali_ptr attrib_bufs;
   } vs;

   struct {
      struct panvk_attrib_buf bufs[MAX_VBS];
      unsigned count;
   } vb;

   /* Index buffer */
   struct {
      struct panvk_buffer *buffer;
      uint64_t offset;
      uint8_t index_size;
      uint32_t first_vertex, base_vertex, base_instance;
   } ib;

   struct {
      enum vk_rp_attachment_flags bound_attachments;
      struct {
         VkFormat fmts[MAX_RTS];
         uint8_t samples[MAX_RTS];
      } color_attachments;
      struct pan_fb_info info;
      bool crc_valid[MAX_RTS];
      uint32_t bo_count;
      struct pan_kmod_bo *bos[MAX_RTS + 2];
   } fb;

   mali_ptr vpd;
};

struct panvk_cmd_compute_state {
   struct panvk_descriptor_state desc_state;
   const struct panvk_compute_pipeline *pipeline;
   struct panvk_compute_sysvals sysvals;
};

struct panvk_cmd_buffer {
   struct vk_command_buffer vk;

   struct panvk_pool desc_pool;
   struct panvk_pool varying_pool;
   struct panvk_pool tls_pool;
   struct list_head batches;
   struct panvk_batch *cur_batch;

   struct {
      struct panvk_cmd_graphics_state gfx;
      struct panvk_cmd_compute_state compute;
   } state;

   uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
};

VK_DEFINE_HANDLE_CASTS(panvk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

static inline const struct panvk_pipeline *
panvk_cmd_get_pipeline(const struct panvk_cmd_buffer *cmdbuf,
                       VkPipelineBindPoint bindpoint)
{
   switch (bindpoint) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      return &cmdbuf->state.gfx.pipeline->base;

   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmdbuf->state.compute.pipeline->base;

   default:
      assert(!"Unsupported bind point");
      return NULL;
   }
}

static inline struct panvk_descriptor_state *
panvk_cmd_get_desc_state(struct panvk_cmd_buffer *cmdbuf,
                         VkPipelineBindPoint bindpoint)
{
   switch (bindpoint) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      return &cmdbuf->state.gfx.desc_state;

   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmdbuf->state.compute.desc_state;

   default:
      assert(!"Unsupported bind point");
      return NULL;
   }
}

extern const struct vk_command_buffer_ops panvk_per_arch(cmd_buffer_ops);

struct panvk_batch *
panvk_per_arch(cmd_open_batch)(struct panvk_cmd_buffer *cmdbuf);

void panvk_per_arch(cmd_close_batch)(struct panvk_cmd_buffer *cmdbuf);

void panvk_per_arch(cmd_get_tiler_context)(struct panvk_cmd_buffer *cmdbuf,
                                           unsigned width, unsigned height);

void panvk_per_arch(cmd_alloc_fb_desc)(struct panvk_cmd_buffer *cmdbuf);

void panvk_per_arch(cmd_alloc_tls_desc)(struct panvk_cmd_buffer *cmdbuf,
                                        bool gfx);

void panvk_per_arch(cmd_prepare_tiler_context)(struct panvk_cmd_buffer *cmdbuf);

void panvk_per_arch(emit_viewport)(const VkViewport *viewport,
                                   const VkRect2D *scissor, void *vpd);

#endif
