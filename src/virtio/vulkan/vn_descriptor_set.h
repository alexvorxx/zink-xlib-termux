/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_DESCRIPTOR_SET_H
#define VN_DESCRIPTOR_SET_H

#include "vn_common.h"

enum vn_descriptor_type {
   VN_DESCRIPTOR_TYPE_SAMPLER,
   VN_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
   VN_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
   VN_DESCRIPTOR_TYPE_STORAGE_IMAGE,
   VN_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
   VN_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
   VN_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
   VN_DESCRIPTOR_TYPE_STORAGE_BUFFER,
   VN_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
   VN_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
   VN_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
   VN_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,

   /* add new enum types before this line */
   VN_NUM_DESCRIPTOR_TYPES,
};

/* TODO refactor struct to track enum vn_descriptor_type type.
 * On VkDescriptorSetLayout creation. When we check against
 * VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, it will be against
 * VN_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK instead
 */
struct vn_descriptor_set_layout_binding {
   VkDescriptorType type;
   uint32_t count;
   bool has_immutable_samplers;
};

struct vn_descriptor_set_layout {
   struct vn_object_base base;

   struct vn_refcount refcount;

   uint32_t last_binding;
   bool has_variable_descriptor_count;

   /* bindings must be the last field in the layout */
   struct vn_descriptor_set_layout_binding bindings[];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_set_layout,
                               base.base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

struct vn_descriptor_pool_state {
   uint32_t set_count;
   uint32_t iub_binding_count;
   uint32_t descriptor_counts[VN_NUM_DESCRIPTOR_TYPES];
};

struct vn_descriptor_pool {
   struct vn_object_base base;

   VkAllocationCallbacks allocator;
   bool async_set_allocation;
   struct vn_descriptor_pool_state max;
   struct vn_descriptor_pool_state used;

   struct list_head descriptor_sets;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_pool,
                               base.base,
                               VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

struct vn_update_descriptor_sets {
   uint32_t write_count;
   VkWriteDescriptorSet *writes;
   VkDescriptorImageInfo *images;
   VkDescriptorBufferInfo *buffers;
   VkBufferView *views;
   VkWriteDescriptorSetInlineUniformBlock *iubs;
};

struct vn_descriptor_set {
   struct vn_object_base base;

   struct vn_descriptor_set_layout *layout;
   uint32_t last_binding_descriptor_count;

   struct list_head head;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_set,
                               base.base,
                               VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

struct vn_descriptor_update_template_entry {
   size_t offset;
   size_t stride;
};

struct vn_descriptor_update_template {
   struct vn_object_base base;

   mtx_t mutex;
   struct vn_update_descriptor_sets *update;

   struct vn_descriptor_update_template_entry entries[];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_descriptor_update_template,
                               base.base,
                               VkDescriptorUpdateTemplate,
                               VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE)

#endif /* VN_DESCRIPTOR_SET_H */
