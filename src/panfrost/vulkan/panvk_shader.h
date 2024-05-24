/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_SHADER_H
#define PANVK_SHADER_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include "util/u_dynarray.h"

#include "util/pan_ir.h"

#include "pan_desc.h"

#include "panvk_descriptor_set.h"
#include "panvk_macros.h"

#include "vk_pipeline_layout.h"

#define MAX_VS_ATTRIBS 16

struct nir_shader;
struct pan_blend_state;
struct panvk_device;

enum panvk_varying_buf_id {
   PANVK_VARY_BUF_GENERAL,
   PANVK_VARY_BUF_POSITION,
   PANVK_VARY_BUF_PSIZ,

   /* Keep last */
   PANVK_VARY_BUF_MAX,
};

struct panvk_graphics_sysvals {
   struct {
      struct {
         float x, y, z;
      } scale, offset;
   } viewport;

   struct {
      float constants[4];
   } blend;

   struct {
      uint32_t first_vertex;
      uint32_t base_vertex;
      uint32_t base_instance;
   } vs;

#if PAN_ARCH <= 7
   struct {
      uint64_t sets[MAX_SETS];
      uint64_t vs_dyn_ssbos;
      uint64_t fs_dyn_ssbos;
   } desc;
#endif
};

struct panvk_compute_sysvals {
   struct {
      uint32_t x, y, z;
   } num_work_groups;
   struct {
      uint32_t x, y, z;
   } local_group_size;

#if PAN_ARCH <= 7
   struct {
      uint64_t sets[MAX_SETS];
      uint64_t dyn_ssbos;
   } desc;
#endif
};

enum panvk_bifrost_desc_table_type {
   PANVK_BIFROST_DESC_TABLE_INVALID = -1,

   /* UBO is encoded on 8 bytes */
   PANVK_BIFROST_DESC_TABLE_UBO = 0,

   /* Images are using a <3DAttributeBuffer,Attribute> pair, each
    * of them being stored in a separate table. */
   PANVK_BIFROST_DESC_TABLE_IMG,

   /* Texture and sampler are encoded on 32 bytes */
   PANVK_BIFROST_DESC_TABLE_TEXTURE,
   PANVK_BIFROST_DESC_TABLE_SAMPLER,

   PANVK_BIFROST_DESC_TABLE_COUNT,
};

#define COPY_DESC_HANDLE(table, idx)           ((table << 28) | (idx))
#define COPY_DESC_HANDLE_EXTRACT_INDEX(handle) ((handle)&BITFIELD_MASK(28))
#define COPY_DESC_HANDLE_EXTRACT_TABLE(handle) ((handle) >> 28)

struct panvk_shader_desc_map {
   /* The index of the map serves as the table offset, the value of the
    * entry is a COPY_DESC_HANDLE() encoding the source set, and the
    * index of the descriptor in the set. */
   uint32_t *map;

   /* Number of entries in the map array. */
   uint32_t count;
};

struct panvk_shader_desc_info {
   uint32_t used_set_mask;
   struct panvk_shader_desc_map dyn_ubos;
   struct panvk_shader_desc_map dyn_ssbos;
   struct panvk_shader_desc_map others[PANVK_BIFROST_DESC_TABLE_COUNT];
};

struct panvk_shader {
   struct pan_shader_info info;
   struct util_dynarray binary;
   struct pan_compute_dim local_size;
   struct panvk_shader_desc_info desc_info;
};

struct panvk_shader *panvk_per_arch(shader_create)(
   struct panvk_device *dev, const VkPipelineShaderStageCreateInfo *stage_info,
   const struct vk_pipeline_layout *layout, const VkAllocationCallbacks *alloc);

void panvk_per_arch(shader_destroy)(struct panvk_device *dev,
                                    struct panvk_shader *shader,
                                    const VkAllocationCallbacks *alloc);

bool panvk_per_arch(nir_lower_descriptors)(
   nir_shader *nir, struct panvk_device *dev,
   const struct vk_pipeline_layout *layout,
   struct panvk_shader_desc_info *shader_desc_info);

#endif
