/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "panvk_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_entrypoints.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"

#include "pan_pool.h"

#include "util/rounding.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"

void
panvk_per_arch(cmd_prepare_push_sets)(struct pan_pool *desc_pool_base,
                                      struct panvk_descriptor_state *desc_state,
                                      const struct panvk_pipeline *pipeline)
{
   const struct panvk_pipeline_layout *playout = pipeline->layout;

   for (unsigned i = 0; i < playout->vk.set_count; i++) {
      const struct panvk_descriptor_set_layout *slayout =
         vk_to_panvk_descriptor_set_layout(playout->vk.set_layouts[i]);
      bool is_push_set =
         slayout->flags &
         VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;

      if (desc_state->sets[i] || !is_push_set || !desc_state->push_sets[i])
         continue;

      struct panvk_descriptor_set *set = &desc_state->push_sets[i]->set;

      panvk_per_arch(push_descriptor_set_assign_layout)(
         desc_state->push_sets[i], slayout);
      if (slayout->desc_ubo_size) {
         struct panfrost_ptr desc_ubo =
            pan_pool_alloc_aligned(desc_pool_base, slayout->desc_ubo_size, 16);
         struct mali_uniform_buffer_packed *ubos = set->ubos;

         memcpy(desc_ubo.cpu, set->desc_ubo.addr.host, slayout->desc_ubo_size);
         set->desc_ubo.addr.dev = desc_ubo.gpu;
         set->desc_ubo.addr.host = desc_ubo.cpu;

         pan_pack(&ubos[slayout->desc_ubo_index], UNIFORM_BUFFER, cfg) {
            cfg.pointer = set->desc_ubo.addr.dev;
            cfg.entries = DIV_ROUND_UP(slayout->desc_ubo_size, 16);
         }
      }

      desc_state->sets[i] = &desc_state->push_sets[i]->set;
   }
}

void
panvk_per_arch(cmd_unprepare_push_sets)(
   struct panvk_descriptor_state *desc_state)
{
   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->sets); i++) {
      if (desc_state->push_sets[i] &&
          &desc_state->push_sets[i]->set == desc_state->sets[i])
         desc_state->sets[i] = NULL;
   }
}

static void
panvk_cmd_prepare_dyn_ssbos(struct pan_pool *desc_pool_base,
                            struct panvk_descriptor_state *desc_state,
                            const struct panvk_pipeline *pipeline)
{
   if (!pipeline->layout->num_dyn_ssbos || desc_state->dyn_desc_ubo)
      return;

   struct panfrost_ptr ssbo_descs =
      pan_pool_alloc_aligned(desc_pool_base, sizeof(desc_state->dyn.ssbos), 16);

   memcpy(ssbo_descs.cpu, desc_state->dyn.ssbos, sizeof(desc_state->dyn.ssbos));

   desc_state->dyn_desc_ubo = ssbo_descs.gpu;
}

void
panvk_per_arch(cmd_prepare_ubos)(struct pan_pool *desc_pool_base,
                                 struct panvk_descriptor_state *desc_state,
                                 const struct panvk_pipeline *pipeline)
{
   unsigned ubo_count =
      panvk_per_arch(pipeline_layout_total_ubo_count)(pipeline->layout);

   if (!ubo_count || desc_state->ubos)
      return;

   panvk_cmd_prepare_dyn_ssbos(desc_pool_base, desc_state, pipeline);

   struct panfrost_ptr ubos =
      pan_pool_alloc_desc_array(desc_pool_base, ubo_count, UNIFORM_BUFFER);
   struct mali_uniform_buffer_packed *ubo_descs = ubos.cpu;

   for (unsigned s = 0; s < pipeline->layout->vk.set_count; s++) {
      const struct panvk_descriptor_set_layout *set_layout =
         vk_to_panvk_descriptor_set_layout(pipeline->layout->vk.set_layouts[s]);
      const struct panvk_descriptor_set *set = desc_state->sets[s];

      unsigned ubo_start =
         panvk_per_arch(pipeline_layout_ubo_start)(pipeline->layout, s, false);

      if (!set) {
         memset(&ubo_descs[ubo_start], 0,
                set_layout->num_ubos * sizeof(*ubo_descs));
      } else {
         memcpy(&ubo_descs[ubo_start], set->ubos,
                set_layout->num_ubos * sizeof(*ubo_descs));
      }
   }

   unsigned dyn_ubos_offset =
      panvk_per_arch(pipeline_layout_dyn_ubos_offset)(pipeline->layout);

   memcpy(&ubo_descs[dyn_ubos_offset], desc_state->dyn.ubos,
          pipeline->layout->num_dyn_ubos * sizeof(*ubo_descs));

   if (pipeline->layout->num_dyn_ssbos) {
      unsigned dyn_desc_ubo =
         panvk_per_arch(pipeline_layout_dyn_desc_ubo_index)(pipeline->layout);

      pan_pack(&ubo_descs[dyn_desc_ubo], UNIFORM_BUFFER, cfg) {
         cfg.pointer = desc_state->dyn_desc_ubo;
         cfg.entries =
            pipeline->layout->num_dyn_ssbos * sizeof(struct panvk_ssbo_addr);
      }
   }

   desc_state->ubos = ubos.gpu;
}

void
panvk_per_arch(cmd_prepare_textures)(struct pan_pool *desc_pool_base,
                                     struct panvk_descriptor_state *desc_state,
                                     const struct panvk_pipeline *pipeline)
{
   unsigned num_textures = pipeline->layout->num_textures;

   if (!num_textures || desc_state->textures)
      return;

   struct panfrost_ptr textures = pan_pool_alloc_aligned(
      desc_pool_base, num_textures * pan_size(TEXTURE), pan_size(TEXTURE));

   void *texture = textures.cpu;

   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->sets); i++) {
      if (!desc_state->sets[i])
         continue;

      memcpy(texture, desc_state->sets[i]->textures,
             desc_state->sets[i]->layout->num_textures * pan_size(TEXTURE));

      texture += desc_state->sets[i]->layout->num_textures * pan_size(TEXTURE);
   }

   desc_state->textures = textures.gpu;
}

void
panvk_per_arch(cmd_prepare_samplers)(struct pan_pool *desc_pool_base,
                                     struct panvk_descriptor_state *desc_state,
                                     const struct panvk_pipeline *pipeline)
{
   unsigned num_samplers = pipeline->layout->num_samplers;

   if (!num_samplers || desc_state->samplers)
      return;

   struct panfrost_ptr samplers =
      pan_pool_alloc_desc_array(desc_pool_base, num_samplers, SAMPLER);

   void *sampler = samplers.cpu;

   /* Prepare the dummy sampler */
   pan_pack(sampler, SAMPLER, cfg) {
      cfg.seamless_cube_map = false;
      cfg.magnify_nearest = true;
      cfg.minify_nearest = true;
      cfg.normalized_coordinates = false;
   }

   sampler += pan_size(SAMPLER);

   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->sets); i++) {
      if (!desc_state->sets[i])
         continue;

      memcpy(sampler, desc_state->sets[i]->samplers,
             desc_state->sets[i]->layout->num_samplers * pan_size(SAMPLER));

      sampler += desc_state->sets[i]->layout->num_samplers * pan_size(SAMPLER);
   }

   desc_state->samplers = samplers.gpu;
}

void
panvk_per_arch(fill_img_attribs)(struct panvk_descriptor_state *desc_state,
                                 const struct panvk_pipeline *pipeline,
                                 void *attrib_bufs, void *attribs,
                                 unsigned first_buf)
{
   for (unsigned s = 0; s < pipeline->layout->vk.set_count; s++) {
      const struct panvk_descriptor_set *set = desc_state->sets[s];

      if (!set)
         continue;

      const struct panvk_descriptor_set_layout *layout = set->layout;
      unsigned img_idx = pipeline->layout->sets[s].img_offset;
      unsigned offset = img_idx * pan_size(ATTRIBUTE_BUFFER) * 2;
      unsigned size = layout->num_imgs * pan_size(ATTRIBUTE_BUFFER) * 2;

      memcpy(attrib_bufs + offset, desc_state->sets[s]->img_attrib_bufs, size);

      offset = img_idx * pan_size(ATTRIBUTE);
      for (unsigned i = 0; i < layout->num_imgs; i++) {
         pan_pack(attribs + offset, ATTRIBUTE, cfg) {
            cfg.buffer_index = first_buf + (img_idx + i) * 2;
            cfg.format = desc_state->sets[s]->img_fmts[i];
            cfg.offset_enable = false;
         }
         offset += pan_size(ATTRIBUTE);
      }
   }
}

void
panvk_per_arch(prepare_img_attribs)(struct pan_pool *desc_pool_base,
                                    struct panvk_descriptor_state *desc_state,
                                    const struct panvk_pipeline *pipeline)
{
   if (desc_state->img.attribs)
      return;

   unsigned attrib_count = pipeline->layout->num_imgs;
   unsigned attrib_buf_count = (pipeline->layout->num_imgs * 2);
   struct panfrost_ptr bufs = pan_pool_alloc_desc_array(
      desc_pool_base, attrib_buf_count + 1, ATTRIBUTE_BUFFER);
   struct panfrost_ptr attribs =
      pan_pool_alloc_desc_array(desc_pool_base, attrib_count, ATTRIBUTE);

   panvk_per_arch(fill_img_attribs)(desc_state, pipeline, bufs.cpu, attribs.cpu,
                                    0);

   desc_state->img.attrib_bufs = bufs.gpu;
   desc_state->img.attribs = attribs.gpu;
}

void
panvk_per_arch(cmd_desc_state_reset)(
   struct panvk_descriptor_state *gfx_desc_state,
   struct panvk_descriptor_state *compute_desc_state)
{
   memset(&gfx_desc_state->sets, 0, sizeof(gfx_desc_state->sets));
   memset(&compute_desc_state->sets, 0, sizeof(compute_desc_state->sets));
}

void
panvk_per_arch(cmd_desc_state_cleanup)(
   struct vk_command_buffer *cmdbuf,
   struct panvk_descriptor_state *gfx_desc_state,
   struct panvk_descriptor_state *compute_desc_state)
{
   for (unsigned i = 0; i < MAX_SETS; i++) {
      if (gfx_desc_state->push_sets[i])
         vk_free(&cmdbuf->pool->alloc, gfx_desc_state->push_sets[i]);
      if (compute_desc_state->push_sets[i])
         vk_free(&cmdbuf->pool->alloc, compute_desc_state->push_sets[i]);
   }
}

static void
panvk_emit_dyn_ubo(struct panvk_descriptor_state *desc_state,
                   const struct panvk_descriptor_set *desc_set,
                   unsigned binding, unsigned array_idx, uint32_t dyn_offset,
                   unsigned dyn_ubo_slot)
{
   struct mali_uniform_buffer_packed *ubo = &desc_state->dyn.ubos[dyn_ubo_slot];
   const struct panvk_descriptor_set_layout *slayout = desc_set->layout;
   VkDescriptorType type = slayout->bindings[binding].type;

   assert(type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
   assert(dyn_ubo_slot < ARRAY_SIZE(desc_state->dyn.ubos));

   const unsigned dyn_ubo_idx = slayout->bindings[binding].dyn_ubo_idx;
   const struct panvk_buffer_desc *bdesc =
      &desc_set->dyn_ubos[dyn_ubo_idx + array_idx];
   mali_ptr address =
      panvk_buffer_gpu_ptr(bdesc->buffer, bdesc->offset + dyn_offset);
   size_t size = panvk_buffer_range(bdesc->buffer, bdesc->offset + dyn_offset,
                                    bdesc->size);

   if (size) {
      pan_pack(ubo, UNIFORM_BUFFER, cfg) {
         cfg.pointer = address;
         cfg.entries = DIV_ROUND_UP(size, 16);
      }
   } else {
      memset(ubo, 0, sizeof(*ubo));
   }
}

static void
panvk_emit_dyn_ssbo(struct panvk_descriptor_state *desc_state,
                    const struct panvk_descriptor_set *desc_set,
                    unsigned binding, unsigned array_idx, uint32_t dyn_offset,
                    unsigned dyn_ssbo_slot)
{
   struct panvk_ssbo_addr *ssbo = &desc_state->dyn.ssbos[dyn_ssbo_slot];
   const struct panvk_descriptor_set_layout *slayout = desc_set->layout;
   VkDescriptorType type = slayout->bindings[binding].type;

   assert(type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
   assert(dyn_ssbo_slot < ARRAY_SIZE(desc_state->dyn.ssbos));

   const unsigned dyn_ssbo_idx = slayout->bindings[binding].dyn_ssbo_idx;
   const struct panvk_buffer_desc *bdesc =
      &desc_set->dyn_ssbos[dyn_ssbo_idx + array_idx];

   *ssbo = (struct panvk_ssbo_addr){
      .base_addr =
         panvk_buffer_gpu_ptr(bdesc->buffer, bdesc->offset + dyn_offset),
      .size = panvk_buffer_range(bdesc->buffer, bdesc->offset + dyn_offset,
                                 bdesc->size),
   };
}

void
panvk_per_arch(cmd_desc_state_bind_sets)(
   struct panvk_descriptor_state *desc_state, VkPipelineLayout layout,
   uint32_t first_set, uint32_t desc_set_count,
   const VkDescriptorSet *desc_sets, uint32_t dyn_offset_count,
   const uint32_t *dyn_offsets)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, playout, layout);

   unsigned dynoffset_idx = 0;
   for (unsigned i = 0; i < desc_set_count; ++i) {
      unsigned idx = i + first_set;
      VK_FROM_HANDLE(panvk_descriptor_set, set, desc_sets[i]);

      desc_state->sets[idx] = set;

      if (set->layout->num_dyn_ssbos || set->layout->num_dyn_ubos) {
         unsigned dyn_ubo_slot = playout->sets[idx].dyn_ubo_offset;
         unsigned dyn_ssbo_slot = playout->sets[idx].dyn_ssbo_offset;

         for (unsigned b = 0; b < set->layout->binding_count; b++) {
            for (unsigned e = 0; e < set->layout->bindings[b].array_size; e++) {
               VkDescriptorType type = set->layout->bindings[b].type;

               if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                  panvk_emit_dyn_ubo(desc_state, set, b, e,
                                     dyn_offsets[dynoffset_idx++],
                                     dyn_ubo_slot++);
               } else if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                  panvk_emit_dyn_ssbo(desc_state, set, b, e,
                                      dyn_offsets[dynoffset_idx++],
                                      dyn_ssbo_slot++);
               }
            }
         }
      }
   }

   /* Unconditionally reset all previously emitted descriptors tables.
    * TODO: we could be smarter by checking which part of the pipeline layout
    * are compatible with the previouly bound descriptor sets.
    */
   desc_state->ubos = 0;
   desc_state->textures = 0;
   desc_state->samplers = 0;
   desc_state->dyn_desc_ubo = 0;
   desc_state->img.attrib_bufs = 0;
   desc_state->img.attribs = 0;

   assert(dynoffset_idx == dyn_offset_count);
}

struct panvk_push_descriptor_set *
panvk_per_arch(cmd_push_descriptors)(struct vk_command_buffer *cmdbuf,
                                     struct panvk_descriptor_state *desc_state,
                                     uint32_t set)
{
   assert(set < MAX_SETS);
   if (unlikely(desc_state->push_sets[set] == NULL)) {
      desc_state->push_sets[set] =
         vk_zalloc(&cmdbuf->pool->alloc, sizeof(*desc_state->push_sets[0]), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(desc_state->push_sets[set] == NULL)) {
         vk_command_buffer_set_error(cmdbuf, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
   }

   /* Pushing descriptors replaces whatever sets are bound */
   desc_state->sets[set] = NULL;

   /* Reset all descs to force emission of new tables on the next draw/dispatch.
    * TODO: Be smarter and only reset those when required.
    */
   desc_state->ubos = 0;
   desc_state->textures = 0;
   desc_state->samplers = 0;
   desc_state->img.attrib_bufs = 0;
   desc_state->img.attribs = 0;
   return desc_state->push_sets[set];
}
