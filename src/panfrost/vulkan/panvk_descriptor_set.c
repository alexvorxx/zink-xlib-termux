/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_descriptor_set.h"
#include "panvk_device.h"
#include "panvk_priv_bo.h"
#include "panvk_private.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "util/mesa-sha1.h"
#include "vk_descriptors.h"
#include "vk_util.h"

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateDescriptorPool(VkDevice _device,
                           const VkDescriptorPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_descriptor_pool *pool;

   pool = vk_object_zalloc(&device->vk, pAllocator,
                           sizeof(struct panvk_descriptor_pool),
                           VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (!pool)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->max.sets = pCreateInfo->maxSets;

   for (unsigned i = 0; i < pCreateInfo->poolSizeCount; ++i) {
      unsigned desc_count = pCreateInfo->pPoolSizes[i].descriptorCount;

      switch (pCreateInfo->pPoolSizes[i].type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         pool->max.samplers += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         pool->max.combined_image_samplers += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         pool->max.sampled_images += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         pool->max.storage_images += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         pool->max.uniform_texel_bufs += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         pool->max.storage_texel_bufs += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         pool->max.input_attachments += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         pool->max.uniform_bufs += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         pool->max.storage_bufs += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         pool->max.uniform_dyn_bufs += desc_count;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         pool->max.storage_dyn_bufs += desc_count;
         break;
      default:
         unreachable("Invalid descriptor type");
      }
   }

   *pDescriptorPool = panvk_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                            const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_descriptor_pool, pool, _pool);

   if (pool)
      vk_object_free(&device->vk, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_ResetDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                          VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(panvk_descriptor_pool, pool, _pool);
   memset(&pool->cur, 0, sizeof(pool->cur));
   return VK_SUCCESS;
}

static void
panvk_descriptor_set_destroy(struct panvk_device *device,
                             struct panvk_descriptor_pool *pool,
                             struct panvk_descriptor_set *set)
{
   if (set->desc_bo)
      panvk_priv_bo_destroy(set->desc_bo, NULL);

   vk_object_free(&device->vk, NULL, set);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_FreeDescriptorSets(VkDevice _device, VkDescriptorPool descriptorPool,
                         uint32_t count, const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_descriptor_pool, pool, descriptorPool);

   for (unsigned i = 0; i < count; i++) {
      VK_FROM_HANDLE(panvk_descriptor_set, set, pDescriptorSets[i]);

      if (set)
         panvk_descriptor_set_destroy(device, pool, set);
   }
   return VK_SUCCESS;
}
