/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef ZINK_PIPELINE_H
#define ZINK_PIPELINE_H

#include <vulkan/vulkan.h>

#include "pipe/p_state.h"
#include "zink_shader_keys.h"
#include "zink_state.h"

struct zink_blend_state;
struct zink_depth_stencil_alpha_state;
struct zink_gfx_program;
struct zink_compute_program;
struct zink_rasterizer_state;
struct zink_render_pass;
struct zink_screen;
struct zink_vertex_elements_state;

struct zink_pipeline_dynamic_state1 {
   uint8_t front_face; //VkFrontFace:1
   uint8_t cull_mode; //VkCullModeFlags:2
   uint16_t num_viewports;
   struct zink_depth_stencil_alpha_hw_state *depth_stencil_alpha_state; //must be last
};

struct zink_pipeline_dynamic_state2 {
   bool primitive_restart;
   bool rasterizer_discard;
   uint16_t vertices_per_patch; //5 bits
};

struct zink_gfx_pipeline_state {
   uint32_t rast_state : ZINK_RAST_HW_STATE_SIZE; //zink_rasterizer_hw_state
   uint32_t _pad1 : 6;
   uint32_t force_persample_interp:1; //duplicated for gpl hashing
   /* order matches zink_gfx_output_key: uint16_t offset */
   uint32_t rast_samples:8; //2 extra bits
   uint32_t void_alpha_attachments:PIPE_MAX_COLOR_BUFS;
   VkSampleMask sample_mask;
   unsigned rp_state;
   uint32_t blend_id;

   /* Pre-hashed value for table lookup, invalid when zero.
    * Members after this point are not included in pipeline state hash key */
   uint32_t hash;
   bool dirty;

   struct zink_pipeline_dynamic_state1 dyn_state1;

   struct zink_pipeline_dynamic_state2 dyn_state2;

   uint32_t _pad;
   uint32_t gkey; //for pipeline library lookups
   VkShaderModule modules[PIPE_SHADER_TYPES - 1];
   bool modules_changed;

   uint32_t vertex_hash;

   uint32_t final_hash;

   uint32_t _pad2;
   /* order matches zink_gfx_input_key */
   union {
      struct {
         unsigned idx:8;
         bool uses_dynamic_stride;
      };
      uint32_t input;
   };
   uint32_t vertex_buffers_enabled_mask;
   uint32_t vertex_strides[PIPE_MAX_ATTRIBS];
   struct zink_vertex_elements_hw_state *element_state;
   bool sample_locations_enabled;
   bool have_EXT_extended_dynamic_state;
   bool have_EXT_extended_dynamic_state2;
   bool extendedDynamicState2PatchControlPoints;
   uint8_t has_points; //either gs outputs points or prim type is points
   struct {
      struct zink_shader_key key[5];
      struct zink_shader_key last_vertex;
   } shader_keys;
   struct zink_blend_state *blend_state;
   struct zink_render_pass *render_pass;
   struct zink_render_pass *next_render_pass; //will be used next time rp is begun
   VkFormat rendering_formats[PIPE_MAX_COLOR_BUFS];
   VkPipelineRenderingCreateInfo rendering_info;
   VkPipeline pipeline;
   enum pipe_prim_type gfx_prim_mode; //pending mode
};

struct zink_compute_pipeline_state {
   /* Pre-hashed value for table lookup, invalid when zero.
    * Members after this point are not included in pipeline state hash key */
   uint32_t hash;
   uint32_t final_hash;
   bool dirty;
   bool use_local_size;
   uint32_t local_size[3];

   uint32_t module_hash;
   VkShaderModule module;
   bool module_changed;

   struct zink_shader_key key;

   VkPipeline pipeline;
};

VkPipeline
zink_create_gfx_pipeline(struct zink_screen *screen,
                         struct zink_gfx_program *prog,
                         struct zink_gfx_pipeline_state *state,
                         const uint8_t *binding_map,
                         VkPrimitiveTopology primitive_topology);

VkPipeline
zink_create_compute_pipeline(struct zink_screen *screen, struct zink_compute_program *comp, struct zink_compute_pipeline_state *state);

VkPipeline
zink_create_gfx_pipeline_input(struct zink_screen *screen,
                               struct zink_gfx_pipeline_state *state,
                               const uint8_t *binding_map,
                               VkPrimitiveTopology primitive_topology);
VkPipeline
zink_create_gfx_pipeline_library(struct zink_screen *screen, struct zink_gfx_program *prog,
                                 struct zink_rasterizer_hw_state *hw_rast_state, bool line);
VkPipeline
zink_create_gfx_pipeline_output(struct zink_screen *screen, struct zink_gfx_pipeline_state *state);
VkPipeline
zink_create_gfx_pipeline_combined(struct zink_screen *screen, struct zink_gfx_program *prog, VkPipeline input, VkPipeline library, VkPipeline output);
#endif
