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

#include "zink_resource.h"

#include "zink_batch.h"
#include "zink_context.h"
#include "zink_fence.h"
#include "zink_program.h"
#include "zink_screen.h"
#include "zink_kopper.h"

#ifdef VK_USE_PLATFORM_METAL_EXT
#include "QuartzCore/CAMetalLayer.h"
#endif
#include "vulkan/wsi/wsi_common.h"

#include "vk_format.h"
#include "util/slab.h"
#include "util/u_blitter.h"
#include "util/u_debug.h"
#include "util/format/u_format.h"
#include "util/u_transfer_helper.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/os_file.h"
#include "frontend/winsys_handle.h"

#include "frontend/sw_winsys.h"

#if !defined(__APPLE__)
#define ZINK_USE_DMABUF
#endif

#if defined(ZINK_USE_DMABUF) && !defined(_WIN32)
#include "drm-uapi/drm_fourcc.h"
#else
/* these won't actually be used */
#define DRM_FORMAT_MOD_INVALID 0
#define DRM_FORMAT_MOD_LINEAR 0
#endif

#if defined(__APPLE__)
// Source of MVK_VERSION
#include "MoltenVK/vk_mvk_moltenvk.h"
#endif

#define ZINK_EXTERNAL_MEMORY_HANDLE 999

static bool
equals_ivci(const void *a, const void *b)
{
   const uint8_t *pa = a;
   const uint8_t *pb = b;
   size_t offset = offsetof(VkImageViewCreateInfo, flags);
   return memcmp(pa + offset, pb + offset, sizeof(VkImageViewCreateInfo) - offset) == 0;
}

static bool
equals_bvci(const void *a, const void *b)
{
   const uint8_t *pa = a;
   const uint8_t *pb = b;
   size_t offset = offsetof(VkBufferViewCreateInfo, flags);
   return memcmp(pa + offset, pb + offset, sizeof(VkBufferViewCreateInfo) - offset) == 0;
}

static void
zink_transfer_flush_region(struct pipe_context *pctx,
                           struct pipe_transfer *ptrans,
                           const struct pipe_box *box);

void
debug_describe_zink_resource_object(char *buf, const struct zink_resource_object *ptr)
{
   sprintf(buf, "zink_resource_object");
}

void
zink_destroy_resource_object(struct zink_screen *screen, struct zink_resource_object *obj)
{
   if (obj->is_buffer) {
      VKSCR(DestroyBuffer)(screen->dev, obj->buffer, NULL);
      VKSCR(DestroyBuffer)(screen->dev, obj->storage_buffer, NULL);
   } else if (obj->dt) {
      zink_kopper_displaytarget_destroy(screen, obj->dt);
   } else if (!obj->is_aux) {
      VKSCR(DestroyImage)(screen->dev, obj->image, NULL);
   } else {
#if defined(ZINK_USE_DMABUF) && !defined(_WIN32)
      close(obj->handle);
#endif
   }

   zink_descriptor_set_refs_clear(&obj->desc_set_refs, obj);
   if (obj->dt) {
      FREE(obj->bo); //this is a dummy struct
   } else
      zink_bo_unref(screen, obj->bo);
   FREE(obj);
}

static void
zink_resource_destroy(struct pipe_screen *pscreen,
                      struct pipe_resource *pres)
{
   struct zink_screen *screen = zink_screen(pscreen);
   struct zink_resource *res = zink_resource(pres);
   if (pres->target == PIPE_BUFFER) {
      util_range_destroy(&res->valid_buffer_range);
      util_idalloc_mt_free(&screen->buffer_ids, res->base.buffer_id_unique);
      assert(!_mesa_hash_table_num_entries(&res->bufferview_cache));
      simple_mtx_destroy(&res->bufferview_mtx);
      ralloc_free(res->bufferview_cache.table);
   } else {
      assert(!_mesa_hash_table_num_entries(&res->surface_cache));
      simple_mtx_destroy(&res->surface_mtx);
      ralloc_free(res->surface_cache.table);
   }
   /* no need to do anything for the caches, these objects own the resource lifetimes */

   zink_resource_object_reference(screen, &res->obj, NULL);
   threaded_resource_deinit(pres);
   FREE_CL(res);
}

static VkImageAspectFlags
aspect_from_format(enum pipe_format fmt)
{
   if (util_format_is_depth_or_stencil(fmt)) {
      VkImageAspectFlags aspect = 0;
      const struct util_format_description *desc = util_format_description(fmt);
      if (util_format_has_depth(desc))
         aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (util_format_has_stencil(desc))
         aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
      return aspect;
   } else
     return VK_IMAGE_ASPECT_COLOR_BIT;
}

static VkBufferCreateInfo
create_bci(struct zink_screen *screen, const struct pipe_resource *templ, unsigned bind)
{
   VkBufferCreateInfo bci;
   bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bci.pNext = NULL;
   bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   bci.queueFamilyIndexCount = 0;
   bci.pQueueFamilyIndices = NULL;
   bci.size = templ->width0;
   bci.flags = 0;
   assert(bci.size > 0);

   bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

   bci.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;

   if (bind & PIPE_BIND_SHADER_IMAGE)
      bci.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

   if (bind & PIPE_BIND_QUERY_BUFFER)
      bci.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;

   if (templ->flags & PIPE_RESOURCE_FLAG_SPARSE)
      bci.flags |= VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
   return bci;
}

static bool
check_ici(struct zink_screen *screen, VkImageCreateInfo *ici, uint64_t modifier)
{
   VkImageFormatProperties image_props;
   VkResult ret;
   assert(modifier == DRM_FORMAT_MOD_INVALID ||
          (VKSCR(GetPhysicalDeviceImageFormatProperties2) && screen->info.have_EXT_image_drm_format_modifier));
   if (VKSCR(GetPhysicalDeviceImageFormatProperties2)) {
      VkImageFormatProperties2 props2;
      props2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
      props2.pNext = NULL;
      VkSamplerYcbcrConversionImageFormatProperties ycbcr_props;
      ycbcr_props.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;
      ycbcr_props.pNext = NULL;
      if (screen->info.have_KHR_sampler_ycbcr_conversion)
         props2.pNext = &ycbcr_props;
      VkPhysicalDeviceImageFormatInfo2 info;
      info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
      info.format = ici->format;
      info.type = ici->imageType;
      info.tiling = ici->tiling;
      info.usage = ici->usage;
      info.flags = ici->flags;

      VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info;
      if (modifier != DRM_FORMAT_MOD_INVALID) {
         mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
         mod_info.pNext = NULL;
         mod_info.drmFormatModifier = modifier;
         mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
         mod_info.queueFamilyIndexCount = 0;
         info.pNext = &mod_info;
      } else
         info.pNext = NULL;

      ret = VKSCR(GetPhysicalDeviceImageFormatProperties2)(screen->pdev, &info, &props2);
      /* this is using VK_IMAGE_CREATE_EXTENDED_USAGE_BIT and can't be validated */
      if (vk_format_aspects(ici->format) & VK_IMAGE_ASPECT_PLANE_1_BIT)
         ret = VK_SUCCESS;
      image_props = props2.imageFormatProperties;
   } else
      ret = VKSCR(GetPhysicalDeviceImageFormatProperties)(screen->pdev, ici->format, ici->imageType,
                                                   ici->tiling, ici->usage, ici->flags, &image_props);
   if (ret != VK_SUCCESS)
      return false;
   if (ici->extent.depth > image_props.maxExtent.depth ||
       ici->extent.height > image_props.maxExtent.height ||
       ici->extent.width > image_props.maxExtent.width)
      return false;
   if (ici->mipLevels > image_props.maxMipLevels)
      return false;
   if (ici->arrayLayers > image_props.maxArrayLayers)
      return false;
   return true;
}

static VkImageUsageFlags
get_image_usage_for_feats(struct zink_screen *screen, VkFormatFeatureFlags feats, const struct pipe_resource *templ, unsigned bind, bool *need_extended)
{
   VkImageUsageFlags usage = 0;
   bool is_planar = util_format_get_num_planes(templ->format) > 1;
   *need_extended = false;

   if (bind & ZINK_BIND_TRANSIENT)
      usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
   else {
      /* sadly, gallium doesn't let us know if it'll ever need this, so we have to assume */
      if (is_planar || (feats & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
         usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      if (is_planar || (feats & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
         usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      if (feats & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
         usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

      if ((is_planar || (feats & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) && (bind & PIPE_BIND_SHADER_IMAGE)) {
         assert(templ->nr_samples <= 1 || screen->info.feats.features.shaderStorageImageMultisample);
         usage |= VK_IMAGE_USAGE_STORAGE_BIT;
      }
   }

   if (bind & PIPE_BIND_RENDER_TARGET) {
      if (feats & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
         usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
         if ((bind & (PIPE_BIND_LINEAR | PIPE_BIND_SHARED)) != (PIPE_BIND_LINEAR | PIPE_BIND_SHARED))
            usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
      } else {
         /* trust that gallium isn't going to give us anything wild */
         *need_extended = true;
         return 0;
      }
   } else if ((bind & PIPE_BIND_SAMPLER_VIEW) && !util_format_is_depth_or_stencil(templ->format)) {
      if (!(feats & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
         /* ensure we can u_blitter this later */
         *need_extended = true;
         return 0;
      }
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   }

   if (bind & PIPE_BIND_DEPTH_STENCIL) {
      if (feats & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
         usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      else
         return 0;
   /* this is unlikely to occur and has been included for completeness */
   } else if (bind & PIPE_BIND_SAMPLER_VIEW && !(usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      if (feats & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
         usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      else
         return 0;
   }

   if (bind & PIPE_BIND_STREAM_OUTPUT)
      usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

   return usage;
}

static VkFormatFeatureFlags
find_modifier_feats(const struct zink_modifier_prop *prop, uint64_t modifier, uint64_t *mod)
{
   for (unsigned j = 0; j < prop->drmFormatModifierCount; j++) {
      if (prop->pDrmFormatModifierProperties[j].drmFormatModifier == modifier) {
         *mod = modifier;
         return prop->pDrmFormatModifierProperties[j].drmFormatModifierTilingFeatures;
      }
   }
   return 0;
}

static VkImageUsageFlags
get_image_usage(struct zink_screen *screen, VkImageCreateInfo *ici, const struct pipe_resource *templ, unsigned bind, unsigned modifiers_count, const uint64_t *modifiers, uint64_t *mod)
{
   VkImageTiling tiling = ici->tiling;
   bool need_extended = false;
   *mod = DRM_FORMAT_MOD_INVALID;
   if (modifiers_count) {
      bool have_linear = false;
      const struct zink_modifier_prop *prop = &screen->modifier_props[templ->format];
      assert(tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
      for (unsigned i = 0; i < modifiers_count; i++) {
         if (modifiers[i] == DRM_FORMAT_MOD_LINEAR) {
            have_linear = true;
            if (!screen->info.have_EXT_image_drm_format_modifier)
               break;
            continue;
         }
         VkFormatFeatureFlags feats = find_modifier_feats(prop, modifiers[i], mod);
         if (feats) {
            VkImageUsageFlags usage = get_image_usage_for_feats(screen, feats, templ, bind, &need_extended);
            assert(!need_extended);
            if (usage) {
               ici->usage = usage;
               if (check_ici(screen, ici, *mod))
                  return usage;
            }
         }
      }
      /* only try linear if no other options available */
      if (have_linear) {
         VkFormatFeatureFlags feats = find_modifier_feats(prop, DRM_FORMAT_MOD_LINEAR, mod);
         if (feats) {
            VkImageUsageFlags usage = get_image_usage_for_feats(screen, feats, templ, bind, &need_extended);
            assert(!need_extended);
            if (usage) {
               ici->usage = usage;
               if (check_ici(screen, ici, *mod))
                  return usage;
            }
         }
      }
   } else
   {
      VkFormatProperties props = screen->format_props[templ->format];
      VkFormatFeatureFlags feats = tiling == VK_IMAGE_TILING_LINEAR ? props.linearTilingFeatures : props.optimalTilingFeatures;
      if (ici->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
         feats = UINT32_MAX;
      VkImageUsageFlags usage = get_image_usage_for_feats(screen, feats, templ, bind, &need_extended);
      if (need_extended) {
         ici->flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
         feats = UINT32_MAX;
         usage = get_image_usage_for_feats(screen, feats, templ, bind, &need_extended);
      }
      if (usage) {
         ici->usage = usage;
         if (check_ici(screen, ici, *mod))
            return usage;
      }
   }
   *mod = DRM_FORMAT_MOD_INVALID;
   return 0;
}

static uint64_t
create_ici(struct zink_screen *screen, VkImageCreateInfo *ici, const struct pipe_resource *templ, bool dmabuf, unsigned bind, unsigned modifiers_count, const uint64_t *modifiers, bool *success)
{
   ici->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   ici->pNext = NULL;
   if (util_format_get_num_planes(templ->format) > 1)
      ici->flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
   else
      ici->flags = modifiers_count || dmabuf || bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DEPTH_STENCIL) ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   ici->usage = 0;
   ici->queueFamilyIndexCount = 0;

   if (templ->flags & PIPE_RESOURCE_FLAG_SPARSE)
      ici->flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;

   bool need_2D = false;
   switch (templ->target) {
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
      if (templ->flags & PIPE_RESOURCE_FLAG_SPARSE)
         need_2D |= screen->need_2D_sparse;
      if (util_format_is_depth_or_stencil(templ->format))
         need_2D |= screen->need_2D_zs;
      ici->imageType = need_2D ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_1D;
      break;

   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
      ici->imageType = VK_IMAGE_TYPE_2D;
      break;

   case PIPE_TEXTURE_3D:
      ici->imageType = VK_IMAGE_TYPE_3D;
      ici->flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
      if (screen->info.have_EXT_image_2d_view_of_3d)
         ici->flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
      break;

   case PIPE_BUFFER:
      unreachable("PIPE_BUFFER should already be handled");

   default:
      unreachable("Unknown target");
   }

   if (screen->info.have_EXT_sample_locations &&
       bind & PIPE_BIND_DEPTH_STENCIL &&
       util_format_has_depth(util_format_description(templ->format)))
      ici->flags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;

   ici->format = zink_get_format(screen, templ->format);
   ici->extent.width = templ->width0;
   ici->extent.height = templ->height0;
   ici->extent.depth = templ->depth0;
   ici->mipLevels = templ->last_level + 1;
   ici->arrayLayers = MAX2(templ->array_size, 1);
   ici->samples = templ->nr_samples ? templ->nr_samples : VK_SAMPLE_COUNT_1_BIT;
   ici->tiling = screen->info.have_EXT_image_drm_format_modifier && modifiers_count ?
                 VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT :
                 bind & PIPE_BIND_LINEAR ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
   ici->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   ici->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

   /* sampleCounts will be set to VK_SAMPLE_COUNT_1_BIT if at least one of the following conditions is true:
    * - flags contains VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
    *
    * 44.1.1. Supported Sample Counts
    */
   bool want_cube = ici->samples == 1 &&
                    (templ->target == PIPE_TEXTURE_CUBE ||
                    templ->target == PIPE_TEXTURE_CUBE_ARRAY ||
                    (templ->target == PIPE_TEXTURE_2D_ARRAY && ici->extent.width == ici->extent.height && ici->arrayLayers >= 6));

   if (templ->target == PIPE_TEXTURE_CUBE)
      ici->arrayLayers *= 6;

   if (templ->usage == PIPE_USAGE_STAGING &&
       templ->format != PIPE_FORMAT_B4G4R4A4_UNORM &&
       templ->format != PIPE_FORMAT_B4G4R4A4_UINT)
      ici->tiling = VK_IMAGE_TILING_LINEAR;
   if (ici->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
      modifiers_count = 0;

   bool first = true;
   bool tried[2] = {0};
   uint64_t mod = DRM_FORMAT_MOD_INVALID;
retry:
   while (!ici->usage) {
      if (!first) {
         switch (ici->tiling) {
         case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT:
            ici->tiling = VK_IMAGE_TILING_OPTIMAL;
            modifiers_count = 0;
            break;
         case VK_IMAGE_TILING_OPTIMAL:
            ici->tiling = VK_IMAGE_TILING_LINEAR;
            break;
         case VK_IMAGE_TILING_LINEAR:
            if (bind & PIPE_BIND_LINEAR) {
               *success = false;
               return DRM_FORMAT_MOD_INVALID;
            }
            ici->tiling = VK_IMAGE_TILING_OPTIMAL;
            break;
         default:
            unreachable("unhandled tiling mode");
         }
         if (tried[ici->tiling]) {
            if (ici->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) {
               *success = false;
               return DRM_FORMAT_MOD_INVALID;
            }
            ici->flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            tried[0] = false;
            tried[1] = false;
            first = true;
            goto retry;
         }
      }
      ici->usage = get_image_usage(screen, ici, templ, bind, modifiers_count, modifiers, &mod);
      first = false;
      if (ici->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
         tried[ici->tiling] = true;
   }
   if (want_cube) {
      ici->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
      if (get_image_usage(screen, ici, templ, bind, modifiers_count, modifiers, &mod) != ici->usage)
         ici->flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
   }

   *success = true;
   return mod;
}

static struct zink_resource_object *
resource_object_create(struct zink_screen *screen, const struct pipe_resource *templ, struct winsys_handle *whandle, bool *optimal_tiling,
                       const uint64_t *modifiers, int modifiers_count, const void *loader_private)
{
   struct zink_resource_object *obj = CALLOC_STRUCT(zink_resource_object);
   if (!obj)
      return NULL;
   obj->last_dt_idx = obj->dt_idx = UINT32_MAX; //TODO: unionize

   VkMemoryRequirements reqs = {0};
   VkMemoryPropertyFlags flags;

   /* figure out aux plane count */
   if (whandle && whandle->plane >= util_format_get_num_planes(whandle->format))
      obj->is_aux = true;
   struct pipe_resource *pnext = templ->next;
   for (obj->plane_count = 1; pnext; obj->plane_count++, pnext = pnext->next) {
      struct zink_resource *next = zink_resource(pnext);
      if (!next->obj->is_aux)
         break;
   }

   bool need_dedicated = false;
   bool shared = templ->bind & PIPE_BIND_SHARED;
#if !defined(_WIN32)
   VkExternalMemoryHandleTypeFlags export_types = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#else
   VkExternalMemoryHandleTypeFlags export_types = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#endif
   unsigned num_planes = util_format_get_num_planes(templ->format);
   VkImageAspectFlags plane_aspects[] = {
      VK_IMAGE_ASPECT_PLANE_0_BIT,
      VK_IMAGE_ASPECT_PLANE_1_BIT,
      VK_IMAGE_ASPECT_PLANE_2_BIT,
   };
   VkExternalMemoryHandleTypeFlags external = 0;
   bool needs_export = (templ->bind & (ZINK_BIND_VIDEO | ZINK_BIND_DMABUF)) != 0;
   if (whandle) {
      if (whandle->type == WINSYS_HANDLE_TYPE_FD || whandle->type == ZINK_EXTERNAL_MEMORY_HANDLE)
         needs_export |= true;
      else
         unreachable("unknown handle type");
   }
   if (needs_export) {
      if (whandle && whandle->type == ZINK_EXTERNAL_MEMORY_HANDLE) {
#if !defined(_WIN32)
         external = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#else
         external = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#endif
      } else {
         external = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
         export_types |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      }
   }

   /* we may export WINSYS_HANDLE_TYPE_FD handle which is dma-buf */
   if (shared && screen->info.have_EXT_external_memory_dma_buf)
      export_types |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   pipe_reference_init(&obj->reference, 1);
   util_dynarray_init(&obj->desc_set_refs.refs, NULL);
   if (loader_private) {
      obj->bo = CALLOC_STRUCT(zink_bo);
      obj->transfer_dst = true;
      return obj;
   } else if (templ->target == PIPE_BUFFER) {
      VkBufferCreateInfo bci = create_bci(screen, templ, templ->bind);

      if (VKSCR(CreateBuffer)(screen->dev, &bci, NULL, &obj->buffer) != VK_SUCCESS) {
         mesa_loge("ZINK: vkCreateBuffer failed");
         goto fail1;
      }

      if (!(templ->bind & PIPE_BIND_SHADER_IMAGE)) {
         bci.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
         if (VKSCR(CreateBuffer)(screen->dev, &bci, NULL, &obj->storage_buffer) != VK_SUCCESS) {
            mesa_loge("ZINK: vkCreateBuffer failed");
            goto fail2;
         }
      }

      VKSCR(GetBufferMemoryRequirements)(screen->dev, obj->buffer, &reqs);
      if (templ->usage == PIPE_USAGE_STAGING)
         flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      else if (templ->usage == PIPE_USAGE_STREAM)
         flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      else if (templ->usage == PIPE_USAGE_IMMUTABLE)
         flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      else
         flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      obj->is_buffer = true;
      obj->transfer_dst = true;
   } else {
      bool winsys_modifier = (export_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) && whandle && whandle->modifier != DRM_FORMAT_MOD_INVALID;
      uint64_t mods[10];
      bool try_modifiers = false;
      if ((export_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) &&
          whandle && whandle->modifier == DRM_FORMAT_MOD_INVALID && whandle->stride) {
         modifiers = mods;
         modifiers_count = screen->modifier_props[templ->format].drmFormatModifierCount;
         for (unsigned j = 0; j < modifiers_count; j++)
            mods[j] = screen->modifier_props[templ->format].pDrmFormatModifierProperties[j].drmFormatModifier;
         if (modifiers_count > 1)
            try_modifiers = true;
      }
      const uint64_t *ici_modifiers = winsys_modifier ? &whandle->modifier : modifiers;
      unsigned ici_modifier_count = winsys_modifier ? 1 : modifiers_count;
      bool success = false;
      VkImageCreateInfo ici;
      uint64_t mod = create_ici(screen, &ici, templ, external == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                templ->bind, ici_modifier_count, ici_modifiers, &success);
      VkExternalMemoryImageCreateInfo emici;
      VkImageDrmFormatModifierExplicitCreateInfoEXT idfmeci;
      VkImageDrmFormatModifierListCreateInfoEXT idfmlci;
      VkSubresourceLayout plane_layouts[4];
      VkSubresourceLayout plane_layout = {
         .offset = whandle ? whandle->offset : 0,
         .size = 0,
         .rowPitch = whandle ? whandle->stride : 0,
         .arrayPitch = 0,
         .depthPitch = 0,
      };
      if (!success)
         goto fail1;

      obj->render_target = (ici.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;

      if (shared || external) {
         emici.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
         emici.pNext = NULL;
         emici.handleTypes = export_types;
         ici.pNext = &emici;

         assert(ici.tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT || mod != DRM_FORMAT_MOD_INVALID);
         if (whandle && ici.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            assert(mod == whandle->modifier || !winsys_modifier);
            idfmeci.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
            idfmeci.pNext = ici.pNext;
            idfmeci.drmFormatModifier = mod;

            idfmeci.drmFormatModifierPlaneCount = obj->plane_count;
            plane_layouts[0] = plane_layout;
            pnext = templ->next;
            for (unsigned i = 1; i < obj->plane_count; i++, pnext = pnext->next) {
               struct zink_resource *next = zink_resource(pnext);
               obj->plane_offsets[i] = plane_layouts[i].offset = next->obj->plane_offsets[i];
               obj->plane_strides[i] = plane_layouts[i].rowPitch = next->obj->plane_strides[i];
               plane_layouts[i].size = 0;
               plane_layouts[i].arrayPitch = 0;
               plane_layouts[i].depthPitch = 0;
            }
            idfmeci.pPlaneLayouts = plane_layouts;

            ici.pNext = &idfmeci;
         } else if (ici.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            idfmlci.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
            idfmlci.pNext = ici.pNext;
            idfmlci.drmFormatModifierCount = modifiers_count;
            idfmlci.pDrmFormatModifiers = modifiers;
            ici.pNext = &idfmlci;
         } else if (ici.tiling == VK_IMAGE_TILING_OPTIMAL) {
            if (!external)
               ici.pNext = NULL;
            shared = false;
         }
      }

      if (optimal_tiling)
         *optimal_tiling = ici.tiling == VK_IMAGE_TILING_OPTIMAL;

      if (ici.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
         obj->transfer_dst = true;

#if defined(ZINK_USE_DMABUF) && !defined(_WIN32)
      if (obj->is_aux) {
         obj->modifier = mod;
         obj->modifier_aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << whandle->plane;
         obj->plane_offsets[whandle->plane] = whandle->offset;
         obj->plane_strides[whandle->plane] = whandle->stride;
         obj->handle = os_dupfd_cloexec(whandle->handle);
         if (obj->handle < 0) {
            mesa_loge("ZINK: failed to dup dmabuf fd: %s\n", strerror(errno));
            goto fail1;
         }
         return obj;
      }
#endif
      if (util_format_is_yuv(templ->format)) {
         VkFormatFeatureFlags feats = VK_FORMAT_FEATURE_FLAG_BITS_MAX_ENUM;
         switch (ici.tiling) {
         case VK_IMAGE_TILING_LINEAR:
            feats = screen->format_props[templ->format].linearTilingFeatures;
            break;
         case VK_IMAGE_TILING_OPTIMAL:
            feats = screen->format_props[templ->format].optimalTilingFeatures;
            break;
         case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT:
            /*
               If is tiling then VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, the value of
               imageCreateFormatFeatures is found by calling vkGetPhysicalDeviceFormatProperties2
               with VkImageFormatProperties::format equal to VkImageCreateInfo::format and with
               VkDrmFormatModifierPropertiesListEXT chained into VkImageFormatProperties2; by
               collecting all members of the returned array
               VkDrmFormatModifierPropertiesListEXT::pDrmFormatModifierProperties
               whose drmFormatModifier belongs to imageCreateDrmFormatModifiers; and by taking the bitwise
               intersection, over the collected array members, of drmFormatModifierTilingFeatures.
               (The resultant imageCreateFormatFeatures may be empty).
               * -Chapter 12. Resource Creation
             */
            for (unsigned i = 0; i < screen->modifier_props[templ->format].drmFormatModifierCount; i++)
               feats &= screen->modifier_props[templ->format].pDrmFormatModifierProperties[i].drmFormatModifierTilingFeatures;
            break;
         default:
            unreachable("unknown tiling");
         }
         if (feats & VK_FORMAT_FEATURE_DISJOINT_BIT)
            ici.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
         VkSamplerYcbcrConversionCreateInfo sycci = {0};
         sycci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
         sycci.pNext = NULL;
         sycci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
         sycci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
         sycci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
         sycci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
         sycci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
         sycci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
         sycci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
         if (!feats || (feats & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)) {
            sycci.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
            sycci.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
         } else {
            assert(feats & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT);
            sycci.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
            sycci.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
         }
         sycci.chromaFilter = VK_FILTER_LINEAR;
         sycci.forceExplicitReconstruction = VK_FALSE;
         VkResult res = VKSCR(CreateSamplerYcbcrConversion)(screen->dev, &sycci, NULL, &obj->sampler_conversion);
         if (res != VK_SUCCESS) {
            mesa_loge("ZINK: vkCreateSamplerYcbcrConversion failed");
            goto fail1;
         }
      } else if (whandle) {
         obj->plane_strides[whandle->plane] = whandle->stride;
      }

      VkResult result = VKSCR(CreateImage)(screen->dev, &ici, NULL, &obj->image);
      if (result != VK_SUCCESS) {
         if (try_modifiers) {
            for (unsigned i = 0; i < modifiers_count; i++) {
               if (modifiers[i] == mod)
                  continue;
               idfmeci.drmFormatModifier = modifiers[i];
               result = VKSCR(CreateImage)(screen->dev, &ici, NULL, &obj->image);
               if (result == VK_SUCCESS)
                  break;
            }
         }
      }
      if (result != VK_SUCCESS) {
         mesa_loge("ZINK: vkCreateImage failed");
         goto fail1;
      }

      if (ici.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
         VkImageDrmFormatModifierPropertiesEXT modprops = {0};
         modprops.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
         result = VKSCR(GetImageDrmFormatModifierPropertiesEXT)(screen->dev, obj->image, &modprops);
         if (result != VK_SUCCESS) {
            mesa_loge("ZINK: vkGetImageDrmFormatModifierPropertiesEXT failed");
            goto fail1;
         }
         obj->modifier = modprops.drmFormatModifier;
         unsigned num_dmabuf_planes = screen->base.get_dmabuf_modifier_planes(&screen->base, obj->modifier, templ->format);
         obj->modifier_aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
         if (num_dmabuf_planes > 1)
            obj->modifier_aspect |= VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
         if (num_dmabuf_planes > 2)
            obj->modifier_aspect |= VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
         if (num_dmabuf_planes > 3)
            obj->modifier_aspect |= VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
         assert(num_dmabuf_planes <= 4);
      }

      if (VKSCR(GetImageMemoryRequirements2)) {
         VkMemoryRequirements2 req2;
         req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
         VkImageMemoryRequirementsInfo2 info2;
         info2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
         info2.pNext = NULL;
         info2.image = obj->image;
         VkMemoryDedicatedRequirements ded;
         ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
         ded.pNext = NULL;
         req2.pNext = &ded;
         VkImagePlaneMemoryRequirementsInfo plane;
         plane.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO;
         plane.pNext = NULL;
         if (num_planes > 1)
            info2.pNext = &plane;
         unsigned offset = 0;
         for (unsigned i = 0; i < num_planes; i++) {
            assert(i < ARRAY_SIZE(plane_aspects));
            plane.planeAspect = plane_aspects[i];
            VKSCR(GetImageMemoryRequirements2)(screen->dev, &info2, &req2);
            if (!i)
               reqs.alignment = req2.memoryRequirements.alignment;
            obj->plane_offsets[i] = offset;
            offset += req2.memoryRequirements.size;
            reqs.size += req2.memoryRequirements.size;
            reqs.memoryTypeBits |= req2.memoryRequirements.memoryTypeBits;
            need_dedicated |= ded.prefersDedicatedAllocation || ded.requiresDedicatedAllocation;
         }
      } else {
         VKSCR(GetImageMemoryRequirements)(screen->dev, obj->image, &reqs);
      }
      if (templ->usage == PIPE_USAGE_STAGING && ici.tiling == VK_IMAGE_TILING_LINEAR)
        flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      else
        flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

      obj->vkflags = ici.flags;
      obj->vkusage = ici.usage;
   }
   obj->alignment = reqs.alignment;

   if (templ->flags & PIPE_RESOURCE_FLAG_MAP_COHERENT || templ->usage == PIPE_USAGE_DYNAMIC)
      flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   else if (!(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            templ->usage == PIPE_USAGE_STAGING)
      flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

   if (templ->bind & ZINK_BIND_TRANSIENT)
      flags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

   VkMemoryAllocateInfo mai;
   enum zink_alloc_flag aflags = templ->flags & PIPE_RESOURCE_FLAG_SPARSE ? ZINK_ALLOC_SPARSE : 0;
   mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.pNext = NULL;
   mai.allocationSize = reqs.size;
   enum zink_heap heap = zink_heap_from_domain_flags(flags, aflags);
   mai.memoryTypeIndex = screen->heap_map[heap];
   if (unlikely(!(reqs.memoryTypeBits & BITFIELD_BIT(mai.memoryTypeIndex)))) {
      /* not valid based on reqs; demote to more compatible type */
      switch (heap) {
      case ZINK_HEAP_DEVICE_LOCAL_VISIBLE:
         heap = ZINK_HEAP_DEVICE_LOCAL;
         break;
      case ZINK_HEAP_HOST_VISIBLE_CACHED:
         heap = ZINK_HEAP_HOST_VISIBLE_COHERENT;
         break;
      default:
         break;
      }
      mai.memoryTypeIndex = screen->heap_map[heap];
      assert(reqs.memoryTypeBits & BITFIELD_BIT(mai.memoryTypeIndex));
   }

   VkMemoryDedicatedAllocateInfo ded_alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = mai.pNext,
      .image = obj->image,
      .buffer = VK_NULL_HANDLE,
   };

   if (screen->info.have_KHR_dedicated_allocation && need_dedicated) {
      ded_alloc_info.pNext = mai.pNext;
      mai.pNext = &ded_alloc_info;
   }

   VkExportMemoryAllocateInfo emai;
   if ((templ->bind & ZINK_BIND_VIDEO) || ((templ->bind & PIPE_BIND_SHARED) && shared) || (templ->bind & ZINK_BIND_DMABUF)) {
      emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
      emai.handleTypes = export_types;

      emai.pNext = mai.pNext;
      mai.pNext = &emai;
      obj->exportable = true;
   }

#ifdef ZINK_USE_DMABUF

#if !defined(_WIN32)
   VkImportMemoryFdInfoKHR imfi = {
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      NULL,
   };

   if (whandle) {
      imfi.pNext = NULL;
      imfi.handleType = external;
      imfi.fd = os_dupfd_cloexec(whandle->handle);
      if (imfi.fd < 0) {
         mesa_loge("ZINK: failed to dup dmabuf fd: %s\n", strerror(errno));
         goto fail1;
      }

      imfi.pNext = mai.pNext;
      mai.pNext = &imfi;
   }
#else
   VkImportMemoryWin32HandleInfoKHR imfi = {
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      NULL,
   };

   if (whandle) {
      HANDLE source_target = GetCurrentProcess();
      HANDLE out_handle;

      bool result = DuplicateHandle(source_target, whandle->handle, source_target, &out_handle, 0, false, DUPLICATE_SAME_ACCESS);

      if (!result || !out_handle) {
         mesa_loge("ZINK: failed to DuplicateHandle with winerr: %08x\n", (int)GetLastError());
         goto fail1;
      }

      imfi.pNext = NULL;
      imfi.handleType = external;
      imfi.handle = out_handle;

      imfi.pNext = mai.pNext;
      mai.pNext = &imfi;
   }
#endif

#endif

   unsigned alignment = MAX2(reqs.alignment, 256);
   if (templ->usage == PIPE_USAGE_STAGING && obj->is_buffer)
      alignment = MAX2(alignment, screen->info.props.limits.minMemoryMapAlignment);
   obj->alignment = alignment;
   obj->bo = zink_bo(zink_bo_create(screen, reqs.size, alignment, heap, mai.pNext ? ZINK_ALLOC_NO_SUBALLOC : 0, mai.pNext));
   if (!obj->bo)
     goto fail2;
   if (aflags == ZINK_ALLOC_SPARSE) {
      obj->size = templ->width0;
   } else {
      obj->offset = zink_bo_get_offset(obj->bo);
      obj->size = zink_bo_get_size(obj->bo);
   }

   obj->coherent = obj->bo->base.placement & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   if (!(templ->flags & PIPE_RESOURCE_FLAG_SPARSE)) {
      obj->host_visible = obj->bo->base.placement & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   }

   if (templ->target == PIPE_BUFFER) {
      if (!(templ->flags & PIPE_RESOURCE_FLAG_SPARSE)) {
         if (VKSCR(BindBufferMemory)(screen->dev, obj->buffer, zink_bo_get_mem(obj->bo), obj->offset) != VK_SUCCESS) {
            mesa_loge("ZINK: vkBindBufferMemory failed");
            goto fail3;
         }
         if (obj->storage_buffer && VKSCR(BindBufferMemory)(screen->dev, obj->storage_buffer, zink_bo_get_mem(obj->bo), obj->offset) != VK_SUCCESS) {
            mesa_loge("ZINK: vkBindBufferMemory failed");
            goto fail3;
         }
      }
   } else {
      if (num_planes > 1) {
         VkBindImageMemoryInfo infos[3];
         VkBindImagePlaneMemoryInfo planes[3];
         for (unsigned i = 0; i < num_planes; i++) {
            infos[i].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
            infos[i].image = obj->image;
            infos[i].memory = zink_bo_get_mem(obj->bo);
            infos[i].memoryOffset = obj->plane_offsets[i];
            if (templ->bind & ZINK_BIND_VIDEO) {
               infos[i].pNext = &planes[i];
               planes[i].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
               planes[i].pNext = NULL;
               planes[i].planeAspect = plane_aspects[i];
            }
         }
         if (VKSCR(BindImageMemory2)(screen->dev, num_planes, infos) != VK_SUCCESS) {
            mesa_loge("ZINK: vkBindImageMemory2 failed");
            goto fail3;
         }
      } else {
         if (!(templ->flags & PIPE_RESOURCE_FLAG_SPARSE))
            if (VKSCR(BindImageMemory)(screen->dev, obj->image, zink_bo_get_mem(obj->bo), obj->offset) != VK_SUCCESS) {
               mesa_loge("ZINK: vkBindImageMemory failed");
               goto fail3;
            }
      }
   }
   return obj;

fail3:
   zink_bo_unref(screen, obj->bo);

fail2:
   if (templ->target == PIPE_BUFFER) {
      VKSCR(DestroyBuffer)(screen->dev, obj->buffer, NULL);
      VKSCR(DestroyBuffer)(screen->dev, obj->storage_buffer, NULL);
   } else
      VKSCR(DestroyImage)(screen->dev, obj->image, NULL);
fail1:
   FREE(obj);
   return NULL;
}

static struct pipe_resource *
resource_create(struct pipe_screen *pscreen,
                const struct pipe_resource *templ,
                struct winsys_handle *whandle,
                unsigned external_usage,
                const uint64_t *modifiers, int modifiers_count,
                const void *loader_private)
{
   struct zink_screen *screen = zink_screen(pscreen);
   struct zink_resource *res = CALLOC_STRUCT_CL(zink_resource);

   if (modifiers_count > 0 && screen->info.have_EXT_image_drm_format_modifier) {
      /* for rebinds */
      res->modifiers_count = modifiers_count;
      res->modifiers = mem_dup(modifiers, modifiers_count * sizeof(uint64_t));
      if (!res->modifiers) {
         FREE_CL(res);
         return NULL;
      }
   }

   res->base.b = *templ;

   threaded_resource_init(&res->base.b, false);
   pipe_reference_init(&res->base.b.reference, 1);
   res->base.b.screen = pscreen;

   bool optimal_tiling = false;
   struct pipe_resource templ2 = *templ;
   if (templ2.flags & PIPE_RESOURCE_FLAG_SPARSE)
      templ2.bind |= PIPE_BIND_SHADER_IMAGE;
   if (screen->faked_e5sparse && templ->format == PIPE_FORMAT_R9G9B9E5_FLOAT) {
      templ2.flags &= ~PIPE_RESOURCE_FLAG_SPARSE;
      res->base.b.flags &= ~PIPE_RESOURCE_FLAG_SPARSE;
   }
   res->obj = resource_object_create(screen, &templ2, whandle, &optimal_tiling, modifiers, modifiers_count, loader_private);
   if (!res->obj) {
      free(res->modifiers);
      FREE_CL(res);
      return NULL;
   }

   res->internal_format = templ->format;
   if (templ->target == PIPE_BUFFER) {
      util_range_init(&res->valid_buffer_range);
      res->base.b.bind |= PIPE_BIND_SHADER_IMAGE;
      if (!screen->resizable_bar && templ->width0 >= 8196) {
         /* We don't want to evict buffers from VRAM by mapping them for CPU access,
          * because they might never be moved back again. If a buffer is large enough,
          * upload data by copying from a temporary GTT buffer. 8K might not seem much,
          * but there can be 100000 buffers.
          *
          * This tweak improves performance for viewperf.
          */
         res->base.b.flags |= PIPE_RESOURCE_FLAG_DONT_MAP_DIRECTLY;
      }
   } else {
      if (templ->flags & PIPE_RESOURCE_FLAG_SPARSE)
         res->base.b.bind |= PIPE_BIND_SHADER_IMAGE;
      if (templ->flags & PIPE_RESOURCE_FLAG_SPARSE) {
         uint32_t count = 1;
         VKSCR(GetImageSparseMemoryRequirements)(screen->dev, res->obj->image, &count, &res->sparse);
         res->base.b.nr_sparse_levels = res->sparse.imageMipTailFirstLod;
      }
      res->format = zink_get_format(screen, templ->format);
      if (templ->target == PIPE_TEXTURE_1D || templ->target == PIPE_TEXTURE_1D_ARRAY) {
         res->need_2D = (screen->need_2D_zs && util_format_is_depth_or_stencil(templ->format)) ||
                        (screen->need_2D_sparse && (templ->flags & PIPE_RESOURCE_FLAG_SPARSE));
      }
      res->dmabuf_acquire = whandle && whandle->type == WINSYS_HANDLE_TYPE_FD;
      res->layout = res->dmabuf_acquire ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;
      res->optimal_tiling = optimal_tiling;
      res->aspect = aspect_from_format(templ->format);
   }

   if (screen->winsys && (templ->bind & PIPE_BIND_DISPLAY_TARGET)) {
      struct sw_winsys *winsys = screen->winsys;
      res->dt = winsys->displaytarget_create(screen->winsys,
                                             res->base.b.bind,
                                             res->base.b.format,
                                             templ->width0,
                                             templ->height0,
                                             64, NULL,
                                             &res->dt_stride);
   }

   if (loader_private) {
      if (templ->bind & PIPE_BIND_DISPLAY_TARGET) {
         /* backbuffer */
         res->obj->dt = zink_kopper_displaytarget_create(screen,
                                                         res->base.b.bind,
                                                         res->base.b.format,
                                                         templ->width0,
                                                         templ->height0,
                                                         64, loader_private,
                                                         &res->dt_stride);
         assert(res->obj->dt);
      } else {
         /* frontbuffer */
         struct zink_resource *back = (void*)loader_private;
         struct kopper_displaytarget *cdt = back->obj->dt;
         cdt->refcount++;
         assert(back->obj->dt);
         res->obj->dt = back->obj->dt;
      }
      struct kopper_displaytarget *cdt = res->obj->dt;
      if (zink_kopper_has_srgb(cdt))
         res->obj->vkflags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      if (cdt->swapchain->scci.flags == VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
         res->obj->vkflags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT_KHR;
      res->obj->vkusage = cdt->swapchain->scci.imageUsage;
      res->base.b.bind |= PIPE_BIND_DISPLAY_TARGET;
      res->optimal_tiling = true;
      res->swapchain = true;
   }
   if (res->obj->is_buffer) {
      res->base.buffer_id_unique = util_idalloc_mt_alloc(&screen->buffer_ids);
      _mesa_hash_table_init(&res->bufferview_cache, NULL, NULL, equals_bvci);
      simple_mtx_init(&res->bufferview_mtx, mtx_plain);
   } else {
      _mesa_hash_table_init(&res->surface_cache, NULL, NULL, equals_ivci);
      simple_mtx_init(&res->surface_mtx, mtx_plain);
   }
   if (res->obj->exportable)
      res->base.b.bind |= ZINK_BIND_DMABUF;
   return &res->base.b;
}

static struct pipe_resource *
zink_resource_create(struct pipe_screen *pscreen,
                     const struct pipe_resource *templ)
{
   return resource_create(pscreen, templ, NULL, 0, NULL, 0, NULL);
}

static struct pipe_resource *
zink_resource_create_with_modifiers(struct pipe_screen *pscreen, const struct pipe_resource *templ,
                                    const uint64_t *modifiers, int modifiers_count)
{
   return resource_create(pscreen, templ, NULL, 0, modifiers, modifiers_count, NULL);
}

static struct pipe_resource *
zink_resource_create_drawable(struct pipe_screen *pscreen,
                              const struct pipe_resource *templ,
                              const void *loader_private)
{
   return resource_create(pscreen, templ, NULL, 0, NULL, 0, loader_private);
}

static bool
add_resource_bind(struct zink_context *ctx, struct zink_resource *res, unsigned bind)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   assert((res->base.b.bind & bind) == 0);
   zink_resource_image_barrier(ctx, res, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0);
   res->base.b.bind |= bind;
   struct zink_resource_object *old_obj = res->obj;
   if (bind & ZINK_BIND_DMABUF && !res->modifiers_count) {
      res->modifiers_count = 1;
      res->modifiers = malloc(res->modifiers_count * sizeof(uint64_t));
      res->modifiers[0] = DRM_FORMAT_MOD_LINEAR;
   }
   struct zink_resource_object *new_obj = resource_object_create(screen, &res->base.b, NULL, &res->optimal_tiling, res->modifiers, res->modifiers_count, NULL);
   if (!new_obj) {
      debug_printf("new backing resource alloc failed!");
      res->base.b.bind &= ~bind;
      return false;
   }
   struct zink_resource staging = *res;
   staging.obj = old_obj;
   staging.all_binds = 0;
   res->layout = VK_IMAGE_LAYOUT_UNDEFINED;
   res->obj->access = 0;
   res->obj->access_stage = 0;
   bool needs_unref = true;
   if (zink_resource_has_usage(res)) {
      zink_batch_reference_resource_move(&ctx->batch, res);
      needs_unref = false;
   }
   res->obj = new_obj;
   zink_descriptor_set_refs_clear(&old_obj->desc_set_refs, old_obj);
   for (unsigned i = 0; i <= res->base.b.last_level; i++) {
      struct pipe_box box = {0, 0, 0,
                             u_minify(res->base.b.width0, i),
                             u_minify(res->base.b.height0, i), res->base.b.array_size};
      box.depth = util_num_layers(&res->base.b, i);
      ctx->base.resource_copy_region(&ctx->base, &res->base.b, i, 0, 0, 0, &staging.base.b, i, &box);
   }
   if (needs_unref)
      zink_resource_object_reference(screen, &old_obj, NULL);
   return true;
}

static bool
zink_resource_get_param(struct pipe_screen *pscreen, struct pipe_context *pctx,
                        struct pipe_resource *pres,
                        unsigned plane,
                        unsigned layer,
                        unsigned level,
                        enum pipe_resource_param param,
                        unsigned handle_usage,
                        uint64_t *value)
{
   struct zink_screen *screen = zink_screen(pscreen);
   struct zink_resource *res = zink_resource(pres);
   struct zink_resource_object *obj = res->obj;
   struct winsys_handle whandle;
   VkImageAspectFlags aspect;
   if (res->modifiers) {
      switch (plane) {
      case 0:
         aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
         break;
      case 1:
         aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
         break;
      case 2:
         aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
         break;
      case 3:
         aspect = VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
         break;
      default:
         unreachable("how many planes you got in this thing?");
      }
   } else if (res->obj->sampler_conversion) {
      aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
   } else {
      aspect = res->aspect;
   }
   switch (param) {
   case PIPE_RESOURCE_PARAM_NPLANES:
      if (screen->info.have_EXT_image_drm_format_modifier)
         *value = util_format_get_num_planes(res->drm_format);
      else
         *value = 1;
      break;

   case PIPE_RESOURCE_PARAM_STRIDE: {
      VkImageSubresource sub_res = {0};
      VkSubresourceLayout sub_res_layout = {0};

      sub_res.aspectMask = aspect;

      VKSCR(GetImageSubresourceLayout)(screen->dev, obj->image, &sub_res, &sub_res_layout);

      *value = sub_res_layout.rowPitch;
      break;
   }

   case PIPE_RESOURCE_PARAM_OFFSET: {
         VkImageSubresource isr = {
            aspect,
            level,
            layer
         };
         VkSubresourceLayout srl;
         VKSCR(GetImageSubresourceLayout)(screen->dev, obj->image, &isr, &srl);
         *value = srl.offset;
         break;
   }

   case PIPE_RESOURCE_PARAM_MODIFIER: {
      *value = obj->modifier;
      break;
   }

   case PIPE_RESOURCE_PARAM_LAYER_STRIDE: {
         VkImageSubresource isr = {
            aspect,
            level,
            layer
         };
         VkSubresourceLayout srl;
         VKSCR(GetImageSubresourceLayout)(screen->dev, obj->image, &isr, &srl);
         if (res->base.b.target == PIPE_TEXTURE_3D)
            *value = srl.depthPitch;
         else
            *value = srl.arrayPitch;
         break;
   }

      return false;
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS:
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_SHARED:
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD: {
#ifdef ZINK_USE_DMABUF
      memset(&whandle, 0, sizeof(whandle));
      if (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_SHARED)
         whandle.type = WINSYS_HANDLE_TYPE_SHARED;
      if (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS)
         whandle.type = WINSYS_HANDLE_TYPE_KMS;
      else if (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD)
         whandle.type = WINSYS_HANDLE_TYPE_FD;

      if (!pscreen->resource_get_handle(pscreen, pctx, pres, &whandle, handle_usage))
         return false;

#ifdef _WIN32
      *value = (uintptr_t)whandle.handle;
#else
      *value = whandle.handle;
#endif
      break;
#else
      (void)whandle;
      return false;
#endif
   }
   }
   return true;
}

static bool
zink_resource_get_handle(struct pipe_screen *pscreen,
                         struct pipe_context *context,
                         struct pipe_resource *tex,
                         struct winsys_handle *whandle,
                         unsigned usage)
{
   if (whandle->type == WINSYS_HANDLE_TYPE_FD || whandle->type == WINSYS_HANDLE_TYPE_KMS) {
#ifdef ZINK_USE_DMABUF
      struct zink_resource *res = zink_resource(tex);
      struct zink_screen *screen = zink_screen(pscreen);
      struct zink_resource_object *obj = res->obj;

#if !defined(_WIN32)
      if (whandle->type == WINSYS_HANDLE_TYPE_KMS && screen->drm_fd == -1) {
         whandle->handle = -1;
      } else {
         if (!res->obj->exportable) {
            assert(!res->all_binds); //TODO handle if problematic
            assert(!zink_resource_usage_is_unflushed(res));
            if (!add_resource_bind(screen->copy_context, res, ZINK_BIND_DMABUF | PIPE_BIND_SHARED))
               return false;
            p_atomic_inc(&screen->image_rebind_counter);
            screen->copy_context->base.flush(&screen->copy_context->base, NULL, 0);
            obj = res->obj;
         }

         VkMemoryGetFdInfoKHR fd_info = {0};
         int fd;
         fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
         fd_info.memory = zink_bo_get_mem(obj->bo);
         if (whandle->type == WINSYS_HANDLE_TYPE_FD)
            fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
         else
            fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         VkResult result = VKSCR(GetMemoryFdKHR)(screen->dev, &fd_info, &fd);
         if (result != VK_SUCCESS) {
            mesa_loge("ZINK: vkGetMemoryFdKHR failed");
            return false;
         }
         if (whandle->type == WINSYS_HANDLE_TYPE_KMS) {
            uint32_t h;
            bool ret = zink_bo_get_kms_handle(screen, obj->bo, fd, &h);
            close(fd);
            if (!ret)
               return false;
            fd = h;
         }

         whandle->handle = fd;
      }
#else
      VkMemoryGetWin32HandleInfoKHR handle_info = {0};
      HANDLE handle;
      handle_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
      //TODO: remove for wsi
      handle_info.memory = zink_bo_get_mem(obj->bo);
      handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      VkResult result = VKSCR(GetMemoryWin32HandleKHR)(screen->dev, &handle_info, &handle);
      if (result != VK_SUCCESS)
         return false;
      whandle->handle = handle;
#endif
      uint64_t value;
      zink_resource_get_param(pscreen, context, tex, 0, 0, 0, PIPE_RESOURCE_PARAM_MODIFIER, 0, &value);
      whandle->modifier = value;
      zink_resource_get_param(pscreen, context, tex, 0, 0, 0, PIPE_RESOURCE_PARAM_OFFSET, 0, &value);
      whandle->offset = value;
      zink_resource_get_param(pscreen, context, tex, 0, 0, 0, PIPE_RESOURCE_PARAM_STRIDE, 0, &value);
      whandle->stride = value;
#else
      return false;
#endif
   }
   return true;
}

static struct pipe_resource *
zink_resource_from_handle(struct pipe_screen *pscreen,
                 const struct pipe_resource *templ,
                 struct winsys_handle *whandle,
                 unsigned usage)
{
#ifdef ZINK_USE_DMABUF
   if (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
       !zink_screen(pscreen)->info.have_EXT_image_drm_format_modifier)
      return NULL;

   struct pipe_resource templ2 = *templ;
   if (templ->format == PIPE_FORMAT_NONE)
      templ2.format = whandle->format;

   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   int modifier_count = 0;
   if (whandle->modifier != DRM_FORMAT_MOD_INVALID) {
      modifier = whandle->modifier;
      modifier_count = 1;
   }
   struct pipe_resource *pres = resource_create(pscreen, &templ2, whandle, usage, &modifier, modifier_count, NULL);
   if (pres) {
      struct zink_resource *res = zink_resource(pres);
      res->drm_format = whandle->format;
      if (pres->target != PIPE_BUFFER)
         res->valid = true;
   }
   return pres;
#else
   return NULL;
#endif
}

struct zink_memory_object {
   struct pipe_memory_object b;
   struct winsys_handle whandle;
};

static struct pipe_memory_object *
zink_memobj_create_from_handle(struct pipe_screen *pscreen, struct winsys_handle *whandle, bool dedicated)
{
   struct zink_memory_object *memobj = CALLOC_STRUCT(zink_memory_object);
   if (!memobj)
      return NULL;
   memcpy(&memobj->whandle, whandle, sizeof(struct winsys_handle));
   memobj->whandle.type = ZINK_EXTERNAL_MEMORY_HANDLE;

#ifdef ZINK_USE_DMABUF

#if !defined(_WIN32)
   memobj->whandle.handle = os_dupfd_cloexec(whandle->handle);
#else
   HANDLE source_target = GetCurrentProcess();
   HANDLE out_handle;

   DuplicateHandle(source_target, whandle->handle, source_target, &out_handle, 0, false, DUPLICATE_SAME_ACCESS);
   memobj->whandle.handle = out_handle;

#endif /* _WIN32 */
#endif /* ZINK_USE_DMABUF */

   return (struct pipe_memory_object *)memobj;
}

static void
zink_memobj_destroy(struct pipe_screen *pscreen, struct pipe_memory_object *pmemobj)
{
#ifdef ZINK_USE_DMABUF
   struct zink_memory_object *memobj = (struct zink_memory_object *)pmemobj;

#if !defined(_WIN32)
   close(memobj->whandle.handle);
#else
   CloseHandle(memobj->whandle.handle);
#endif /* _WIN32 */
#endif /* ZINK_USE_DMABUF */
   
   FREE(pmemobj);
}

static struct pipe_resource *
zink_resource_from_memobj(struct pipe_screen *pscreen,
                          const struct pipe_resource *templ,
                          struct pipe_memory_object *pmemobj,
                          uint64_t offset)
{
   struct zink_memory_object *memobj = (struct zink_memory_object *)pmemobj;

   struct pipe_resource *pres = resource_create(pscreen, templ, &memobj->whandle, 0, NULL, 0, NULL);
   if (pres && pres->target != PIPE_BUFFER)
      zink_resource(pres)->valid = true;
   return pres;
}

static bool
invalidate_buffer(struct zink_context *ctx, struct zink_resource *res)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);

   assert(res->base.b.target == PIPE_BUFFER);

   if (res->base.b.flags & PIPE_RESOURCE_FLAG_SPARSE)
      return false;

   if (res->valid_buffer_range.start > res->valid_buffer_range.end)
      return false;

   if (res->so_valid)
      ctx->dirty_so_targets = true;
   /* force counter buffer reset */
   res->so_valid = false;

   util_range_set_empty(&res->valid_buffer_range);
   if (!zink_resource_has_usage(res))
      return false;

   struct zink_resource_object *old_obj = res->obj;
   struct zink_resource_object *new_obj = resource_object_create(screen, &res->base.b, NULL, NULL, NULL, 0, NULL);
   if (!new_obj) {
      debug_printf("new backing resource alloc failed!");
      return false;
   }
   /* this ref must be transferred before rebind or else BOOM */
   zink_batch_reference_resource_move(&ctx->batch, res);
   res->obj = new_obj;
   zink_resource_rebind(ctx, res);
   zink_descriptor_set_refs_clear(&old_obj->desc_set_refs, old_obj);
   return true;
}


static void
zink_resource_invalidate(struct pipe_context *pctx, struct pipe_resource *pres)
{
   if (pres->target == PIPE_BUFFER)
      invalidate_buffer(zink_context(pctx), zink_resource(pres));
   else {
      struct zink_resource *res = zink_resource(pres);
      if (res->valid && res->fb_binds)
         zink_context(pctx)->rp_changed = true;
      res->valid = false;
   }
}

static void
zink_transfer_copy_bufimage(struct zink_context *ctx,
                            struct zink_resource *dst,
                            struct zink_resource *src,
                            struct zink_transfer *trans)
{
   assert((trans->base.b.usage & (PIPE_MAP_DEPTH_ONLY | PIPE_MAP_STENCIL_ONLY)) !=
          (PIPE_MAP_DEPTH_ONLY | PIPE_MAP_STENCIL_ONLY));

   bool buf2img = src->base.b.target == PIPE_BUFFER;

   struct pipe_box box = trans->base.b.box;
   int x = box.x;
   if (buf2img)
      box.x = trans->offset;

   if (dst->obj->transfer_dst)
      zink_copy_image_buffer(ctx, dst, src, trans->base.b.level, buf2img ? x : 0,
                              box.y, box.z, trans->base.b.level, &box, trans->base.b.usage);
   else
      util_blitter_copy_texture(ctx->blitter, &dst->base.b, trans->base.b.level,
                                x, box.y, box.z, &src->base.b,
                                0, &box);
}

ALWAYS_INLINE static void
align_offset_size(const VkDeviceSize alignment, VkDeviceSize *offset, VkDeviceSize *size, VkDeviceSize obj_size)
{
   VkDeviceSize align = *offset % alignment;
   if (alignment - 1 > *offset)
      *offset = 0;
   else
      *offset -= align, *size += align;
   align = alignment - (*size % alignment);
   if (*offset + *size + align > obj_size)
      *size = obj_size - *offset;
   else
      *size += align;
}

VkMappedMemoryRange
zink_resource_init_mem_range(struct zink_screen *screen, struct zink_resource_object *obj, VkDeviceSize offset, VkDeviceSize size)
{
   assert(obj->size);
   align_offset_size(screen->info.props.limits.nonCoherentAtomSize, &offset, &size, obj->size);
   VkMappedMemoryRange range = {
      VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      NULL,
      zink_bo_get_mem(obj->bo),
      offset,
      size
   };
   assert(range.size);
   return range;
}

static void *
map_resource(struct zink_screen *screen, struct zink_resource *res)
{
   assert(res->obj->host_visible);
   return zink_bo_map(screen, res->obj->bo);
}

static void
unmap_resource(struct zink_screen *screen, struct zink_resource *res)
{
   zink_bo_unmap(screen, res->obj->bo);
}

static struct zink_transfer *
create_transfer(struct zink_context *ctx, struct pipe_resource *pres, unsigned usage, const struct pipe_box *box)
{
   struct zink_transfer *trans;

   if (usage & PIPE_MAP_THREAD_SAFE)
      trans = calloc(1, sizeof(*trans));
   else if (usage & TC_TRANSFER_MAP_THREADED_UNSYNC)
      trans = slab_zalloc(&ctx->transfer_pool_unsync);
   else
      trans = slab_zalloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;

   pipe_resource_reference(&trans->base.b.resource, pres);

   trans->base.b.usage = usage;
   trans->base.b.box = *box;
   return trans;
}

static void
destroy_transfer(struct zink_context *ctx, struct zink_transfer *trans)
{
   if (trans->base.b.usage & PIPE_MAP_THREAD_SAFE) {
      free(trans);
   } else {
      /* Don't use pool_transfers_unsync. We are always in the driver
       * thread. Freeing an object into a different pool is allowed.
       */
      slab_free(&ctx->transfer_pool, trans);
   }
}

static void *
zink_buffer_map(struct pipe_context *pctx,
                    struct pipe_resource *pres,
                    unsigned level,
                    unsigned usage,
                    const struct pipe_box *box,
                    struct pipe_transfer **transfer)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_resource *res = zink_resource(pres);
   struct zink_transfer *trans = create_transfer(ctx, pres, usage, box);
   if (!trans)
      return NULL;

   void *ptr = NULL;

   if (res->base.is_user_ptr)
      usage |= PIPE_MAP_PERSISTENT;

   /* See if the buffer range being mapped has never been initialized,
    * in which case it can be mapped unsynchronized. */
   if (!(usage & (PIPE_MAP_UNSYNCHRONIZED | TC_TRANSFER_MAP_NO_INFER_UNSYNCHRONIZED)) &&
       usage & PIPE_MAP_WRITE && !res->base.is_shared &&
       !util_ranges_intersect(&res->valid_buffer_range, box->x, box->x + box->width)) {
      usage |= PIPE_MAP_UNSYNCHRONIZED;
   }

   /* If discarding the entire range, discard the whole resource instead. */
   if (usage & PIPE_MAP_DISCARD_RANGE && box->x == 0 && box->width == res->base.b.width0) {
      usage |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
   }

   /* If a buffer in VRAM is too large and the range is discarded, don't
    * map it directly. This makes sure that the buffer stays in VRAM.
    */
   bool force_discard_range = false;
   if (usage & (PIPE_MAP_DISCARD_WHOLE_RESOURCE | PIPE_MAP_DISCARD_RANGE) &&
       !(usage & PIPE_MAP_PERSISTENT) &&
       res->base.b.flags & PIPE_RESOURCE_FLAG_DONT_MAP_DIRECTLY) {
      usage &= ~(PIPE_MAP_DISCARD_WHOLE_RESOURCE | PIPE_MAP_UNSYNCHRONIZED);
      usage |= PIPE_MAP_DISCARD_RANGE;
      force_discard_range = true;
   }

   if (usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE &&
       !(usage & (PIPE_MAP_UNSYNCHRONIZED | TC_TRANSFER_MAP_NO_INVALIDATE))) {
      assert(usage & PIPE_MAP_WRITE);

      if (invalidate_buffer(ctx, res)) {
         /* At this point, the buffer is always idle. */
         usage |= PIPE_MAP_UNSYNCHRONIZED;
      } else {
         /* Fall back to a temporary buffer. */
         usage |= PIPE_MAP_DISCARD_RANGE;
      }
   }

   if (usage & PIPE_MAP_DISCARD_RANGE &&
        (!res->obj->host_visible ||
        !(usage & (PIPE_MAP_UNSYNCHRONIZED | PIPE_MAP_PERSISTENT)))) {

      /* Check if mapping this buffer would cause waiting for the GPU.
       */

      if (!res->obj->host_visible || force_discard_range ||
          !zink_resource_usage_check_completion(screen, res, ZINK_RESOURCE_ACCESS_RW)) {
         /* Do a wait-free write-only transfer using a temporary buffer. */
         unsigned offset;

         /* If we are not called from the driver thread, we have
          * to use the uploader from u_threaded_context, which is
          * local to the calling thread.
          */
         struct u_upload_mgr *mgr;
         if (usage & TC_TRANSFER_MAP_THREADED_UNSYNC)
            mgr = ctx->tc->base.stream_uploader;
         else
            mgr = ctx->base.stream_uploader;
         u_upload_alloc(mgr, 0, box->width,
                     screen->info.props.limits.minMemoryMapAlignment, &offset,
                     (struct pipe_resource **)&trans->staging_res, (void **)&ptr);
         res = zink_resource(trans->staging_res);
         trans->offset = offset;
         usage |= PIPE_MAP_UNSYNCHRONIZED;
         ptr = ((uint8_t *)ptr);
      } else {
         /* At this point, the buffer is always idle (we checked it above). */
         usage |= PIPE_MAP_UNSYNCHRONIZED;
      }
   } else if (usage & PIPE_MAP_DONTBLOCK) {
      /* sparse/device-local will always need to wait since it has to copy */
      if (!res->obj->host_visible)
         goto success;
      if (!zink_resource_usage_check_completion(screen, res, ZINK_RESOURCE_ACCESS_WRITE))
         goto success;
      usage |= PIPE_MAP_UNSYNCHRONIZED;
   } else if (!(usage & PIPE_MAP_UNSYNCHRONIZED) &&
              (((usage & PIPE_MAP_READ) && !(usage & PIPE_MAP_PERSISTENT) && res->base.b.usage != PIPE_USAGE_STAGING) || !res->obj->host_visible)) {
      assert(!(usage & (TC_TRANSFER_MAP_THREADED_UNSYNC | PIPE_MAP_THREAD_SAFE)));
      if (!res->obj->host_visible || !(usage & PIPE_MAP_ONCE)) {
         trans->offset = box->x % screen->info.props.limits.minMemoryMapAlignment;
         trans->staging_res = pipe_buffer_create(&screen->base, PIPE_BIND_LINEAR, PIPE_USAGE_STAGING, box->width + trans->offset);
         if (!trans->staging_res)
            goto fail;
         struct zink_resource *staging_res = zink_resource(trans->staging_res);
         zink_copy_buffer(ctx, staging_res, res, trans->offset, box->x, box->width);
         res = staging_res;
         usage &= ~PIPE_MAP_UNSYNCHRONIZED;
         ptr = map_resource(screen, res);
         ptr = ((uint8_t *)ptr) + trans->offset;
      }
   } else if ((usage & PIPE_MAP_UNSYNCHRONIZED) && !res->obj->host_visible) {
      trans->offset = box->x % screen->info.props.limits.minMemoryMapAlignment;
      trans->staging_res = pipe_buffer_create(&screen->base, PIPE_BIND_LINEAR, PIPE_USAGE_STAGING, box->width + trans->offset);
      if (!trans->staging_res)
         goto fail;
      struct zink_resource *staging_res = zink_resource(trans->staging_res);
      res = staging_res;
      ptr = map_resource(screen, res);
      ptr = ((uint8_t *)ptr) + trans->offset;
   }

   if (!(usage & PIPE_MAP_UNSYNCHRONIZED)) {
      if (usage & PIPE_MAP_WRITE)
         zink_resource_usage_wait(ctx, res, ZINK_RESOURCE_ACCESS_RW);
      else
         zink_resource_usage_wait(ctx, res, ZINK_RESOURCE_ACCESS_WRITE);
      res->obj->access = 0;
      res->obj->access_stage = 0;
   }

   if (!ptr) {
      /* if writing to a streamout buffer, ensure synchronization next time it's used */
      if (usage & PIPE_MAP_WRITE && res->so_valid) {
         ctx->dirty_so_targets = true;
         /* force counter buffer reset */
         res->so_valid = false;
      }
      ptr = map_resource(screen, res);
      if (!ptr)
         goto fail;
      ptr = ((uint8_t *)ptr) + box->x;
   }

   if (!res->obj->coherent
#if defined(MVK_VERSION)
      // Work around for MoltenVk limitation specifically on coherent memory
      // MoltenVk returns blank memory ranges when there should be data present
      // This is a known limitation of MoltenVK.
      // See https://github.com/KhronosGroup/MoltenVK/blob/master/Docs/MoltenVK_Runtime_UserGuide.md#known-moltenvk-limitations

       || screen->instance_info.have_MVK_moltenvk
#endif
      ) {
      VkDeviceSize size = box->width;
      VkDeviceSize offset = res->obj->offset + trans->offset;
      VkMappedMemoryRange range = zink_resource_init_mem_range(screen, res->obj, offset, size);
      if (VKSCR(InvalidateMappedMemoryRanges)(screen->dev, 1, &range) != VK_SUCCESS) {
         mesa_loge("ZINK: vkInvalidateMappedMemoryRanges failed");
         zink_bo_unmap(screen, res->obj->bo);
         goto fail;
      }
   }
   trans->base.b.usage = usage;
   if (usage & PIPE_MAP_WRITE)
      util_range_add(&res->base.b, &res->valid_buffer_range, box->x, box->x + box->width);
   if ((usage & PIPE_MAP_PERSISTENT) && !(usage & PIPE_MAP_COHERENT))
      res->obj->persistent_maps++;

success:
   *transfer = &trans->base.b;
   return ptr;

fail:
   destroy_transfer(ctx, trans);
   return NULL;
}

static void *
zink_image_map(struct pipe_context *pctx,
                  struct pipe_resource *pres,
                  unsigned level,
                  unsigned usage,
                  const struct pipe_box *box,
                  struct pipe_transfer **transfer)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_resource *res = zink_resource(pres);
   struct zink_transfer *trans = create_transfer(ctx, pres, usage, box);
   if (!trans)
      return NULL;

   trans->base.b.level = level;

   void *ptr;
   if (usage & PIPE_MAP_WRITE && !(usage & PIPE_MAP_READ))
      /* this is like a blit, so we can potentially dump some clears or maybe we have to  */
      zink_fb_clears_apply_or_discard(ctx, pres, zink_rect_from_box(box), false);
   else if (usage & PIPE_MAP_READ)
      /* if the map region intersects with any clears then we have to apply them */
      zink_fb_clears_apply_region(ctx, pres, zink_rect_from_box(box));
   if (res->optimal_tiling || !res->obj->host_visible) {
      enum pipe_format format = pres->format;
      if (usage & PIPE_MAP_DEPTH_ONLY)
         format = util_format_get_depth_only(pres->format);
      else if (usage & PIPE_MAP_STENCIL_ONLY)
         format = PIPE_FORMAT_S8_UINT;
      trans->base.b.stride = util_format_get_stride(format, box->width);
      trans->base.b.layer_stride = util_format_get_2d_size(format,
                                                         trans->base.b.stride,
                                                         box->height);

      struct pipe_resource templ = *pres;
      templ.next = NULL;
      templ.format = format;
      templ.usage = usage & PIPE_MAP_READ ? PIPE_USAGE_STAGING : PIPE_USAGE_STREAM;
      templ.target = PIPE_BUFFER;
      templ.bind = PIPE_BIND_LINEAR;
      templ.width0 = trans->base.b.layer_stride * box->depth;
      templ.height0 = templ.depth0 = 0;
      templ.last_level = 0;
      templ.array_size = 1;
      templ.flags = 0;

      trans->staging_res = zink_resource_create(pctx->screen, &templ);
      if (!trans->staging_res)
         goto fail;

      struct zink_resource *staging_res = zink_resource(trans->staging_res);

      if (usage & PIPE_MAP_READ) {
         /* force multi-context sync */
         if (zink_resource_usage_is_unflushed_write(res))
            zink_resource_usage_wait(ctx, res, ZINK_RESOURCE_ACCESS_WRITE);
         zink_transfer_copy_bufimage(ctx, staging_res, res, trans);
         /* need to wait for rendering to finish */
         zink_fence_wait(pctx);
      }

      ptr = map_resource(screen, staging_res);
   } else {
      assert(!res->optimal_tiling);
      ptr = map_resource(screen, res);
      if (!ptr)
         goto fail;
      if (zink_resource_has_usage(res)) {
         if (usage & PIPE_MAP_WRITE)
            zink_fence_wait(pctx);
         else
            zink_resource_usage_wait(ctx, res, ZINK_RESOURCE_ACCESS_WRITE);
      }
      VkImageSubresource isr = {
         res->modifiers ? res->obj->modifier_aspect : res->aspect,
         level,
         0
      };
      VkSubresourceLayout srl;
      VKSCR(GetImageSubresourceLayout)(screen->dev, res->obj->image, &isr, &srl);
      trans->base.b.stride = srl.rowPitch;
      if (res->base.b.target == PIPE_TEXTURE_3D)
         trans->base.b.layer_stride = srl.depthPitch;
      else
         trans->base.b.layer_stride = srl.arrayPitch;
      trans->offset = srl.offset;
      trans->depthPitch = srl.depthPitch;
      const struct util_format_description *desc = util_format_description(res->base.b.format);
      unsigned offset = srl.offset +
                        box->z * srl.depthPitch +
                        (box->y / desc->block.height) * srl.rowPitch +
                        (box->x / desc->block.width) * (desc->block.bits / 8);
      if (!res->obj->coherent) {
         VkDeviceSize size = (VkDeviceSize)box->width * box->height * desc->block.bits / 8;
         VkMappedMemoryRange range = zink_resource_init_mem_range(screen, res->obj, res->obj->offset + offset, size);
         if (VKSCR(FlushMappedMemoryRanges)(screen->dev, 1, &range) != VK_SUCCESS) {
            mesa_loge("ZINK: vkFlushMappedMemoryRanges failed");
         }
      }
      ptr = ((uint8_t *)ptr) + offset;
   }
   if (!ptr)
      goto fail;
   if (usage & PIPE_MAP_WRITE) {
      if (!res->valid && res->fb_binds)
         ctx->rp_changed = true;
      res->valid = true;
   }

   if (sizeof(void*) == 4)
      trans->base.b.usage |= ZINK_MAP_TEMPORARY;
   if ((usage & PIPE_MAP_PERSISTENT) && !(usage & PIPE_MAP_COHERENT))
      res->obj->persistent_maps++;

   *transfer = &trans->base.b;
   return ptr;

fail:
   destroy_transfer(ctx, trans);
   return NULL;
}

static void
zink_transfer_flush_region(struct pipe_context *pctx,
                           struct pipe_transfer *ptrans,
                           const struct pipe_box *box)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_resource *res = zink_resource(ptrans->resource);
   struct zink_transfer *trans = (struct zink_transfer *)ptrans;

   if (trans->base.b.usage & PIPE_MAP_WRITE) {
      struct zink_screen *screen = zink_screen(pctx->screen);
      struct zink_resource *m = trans->staging_res ? zink_resource(trans->staging_res) :
                                                     res;
      ASSERTED VkDeviceSize size, offset;
      if (m->obj->is_buffer) {
         size = box->width;
         offset = trans->offset;
      } else {
         size = (VkDeviceSize)box->width * box->height * util_format_get_blocksize(m->base.b.format);
         offset = trans->offset +
                  box->z * trans->depthPitch +
                  util_format_get_2d_size(m->base.b.format, trans->base.b.stride, box->y) +
                  util_format_get_stride(m->base.b.format, box->x);
         assert(offset + size <= res->obj->size);
      }
      if (!m->obj->coherent) {
         VkMappedMemoryRange range = zink_resource_init_mem_range(screen, m->obj, m->obj->offset, m->obj->size);
         if (VKSCR(FlushMappedMemoryRanges)(screen->dev, 1, &range) != VK_SUCCESS) {
            mesa_loge("ZINK: vkFlushMappedMemoryRanges failed");
         }
      }
      if (trans->staging_res) {
         struct zink_resource *staging_res = zink_resource(trans->staging_res);

         if (ptrans->resource->target == PIPE_BUFFER)
            zink_copy_buffer(ctx, res, staging_res, box->x, offset, box->width);
         else
            zink_transfer_copy_bufimage(ctx, res, staging_res, trans);
      }
   }
}

static void
transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct zink_context *ctx = zink_context(pctx);
   struct zink_resource *res = zink_resource(ptrans->resource);
   struct zink_transfer *trans = (struct zink_transfer *)ptrans;

   if (!(trans->base.b.usage & (PIPE_MAP_FLUSH_EXPLICIT | PIPE_MAP_COHERENT))) {
      zink_transfer_flush_region(pctx, ptrans, &ptrans->box);
   }

   if ((trans->base.b.usage & PIPE_MAP_PERSISTENT) && !(trans->base.b.usage & PIPE_MAP_COHERENT))
      res->obj->persistent_maps--;

   if (trans->staging_res)
      pipe_resource_reference(&trans->staging_res, NULL);
   pipe_resource_reference(&trans->base.b.resource, NULL);

   destroy_transfer(ctx, trans);
}

static void
do_transfer_unmap(struct zink_screen *screen, struct zink_transfer *trans)
{
   struct zink_resource *res = zink_resource(trans->staging_res);
   if (!res)
      res = zink_resource(trans->base.b.resource);
   unmap_resource(screen, res);
}

static void
zink_buffer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_transfer *trans = (struct zink_transfer *)ptrans;
   if (trans->base.b.usage & PIPE_MAP_ONCE && !trans->staging_res)
      do_transfer_unmap(screen, trans);
   transfer_unmap(pctx, ptrans);
}

static void
zink_image_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_transfer *trans = (struct zink_transfer *)ptrans;
   if (sizeof(void*) == 4)
      do_transfer_unmap(screen, trans);
   transfer_unmap(pctx, ptrans);
}

static void
zink_buffer_subdata(struct pipe_context *ctx, struct pipe_resource *buffer,
                    unsigned usage, unsigned offset, unsigned size, const void *data)
{
   struct pipe_transfer *transfer = NULL;
   struct pipe_box box;
   uint8_t *map = NULL;

   usage |= PIPE_MAP_WRITE;

   if (!(usage & PIPE_MAP_DIRECTLY))
      usage |= PIPE_MAP_DISCARD_RANGE;

   u_box_1d(offset, size, &box);
   map = zink_buffer_map(ctx, buffer, 0, usage, &box, &transfer);
   if (!map)
      return;

   memcpy(map, data, size);
   zink_buffer_unmap(ctx, transfer);
}

static struct pipe_resource *
zink_resource_get_separate_stencil(struct pipe_resource *pres)
{
   /* For packed depth-stencil, we treat depth as the primary resource
    * and store S8 as the "second plane" resource.
    */
   if (pres->next && pres->next->format == PIPE_FORMAT_S8_UINT)
      return pres->next;

   return NULL;

}

bool
zink_resource_object_init_storage(struct zink_context *ctx, struct zink_resource *res)
{
   /* base resource already has the cap */
   if (res->base.b.bind & PIPE_BIND_SHADER_IMAGE)
      return true;
   if (res->obj->is_buffer) {
      unreachable("zink: all buffers should have this bit");
      return true;
   }
   assert(!res->obj->dt);
   zink_fb_clears_apply_region(ctx, &res->base.b, (struct u_rect){0, res->base.b.width0, 0, res->base.b.height0});
   bool ret = add_resource_bind(ctx, res, PIPE_BIND_SHADER_IMAGE);
   if (ret)
      zink_resource_rebind(ctx, res);

   return ret;
}

void
zink_resource_setup_transfer_layouts(struct zink_context *ctx, struct zink_resource *src, struct zink_resource *dst)
{
   if (src == dst) {
      /* The Vulkan 1.1 specification says the following about valid usage
       * of vkCmdBlitImage:
       *
       * "srcImageLayout must be VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR,
       *  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL"
       *
       * and:
       *
       * "dstImageLayout must be VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR,
       *  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL"
       *
       * Since we cant have the same image in two states at the same time,
       * we're effectively left with VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR or
       * VK_IMAGE_LAYOUT_GENERAL. And since this isn't a present-related
       * operation, VK_IMAGE_LAYOUT_GENERAL seems most appropriate.
       */
      zink_resource_image_barrier(ctx, src,
                                  VK_IMAGE_LAYOUT_GENERAL,
                                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);
   } else {
      zink_resource_image_barrier(ctx, src,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);

      zink_resource_image_barrier(ctx, dst,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);
   }
}

void
zink_get_depth_stencil_resources(struct pipe_resource *res,
                                 struct zink_resource **out_z,
                                 struct zink_resource **out_s)
{
   if (!res) {
      if (out_z) *out_z = NULL;
      if (out_s) *out_s = NULL;
      return;
   }

   if (res->format != PIPE_FORMAT_S8_UINT) {
      if (out_z) *out_z = zink_resource(res);
      if (out_s) *out_s = zink_resource(zink_resource_get_separate_stencil(res));
   } else {
      if (out_z) *out_z = NULL;
      if (out_s) *out_s = zink_resource(res);
   }
}

static void
zink_resource_set_separate_stencil(struct pipe_resource *pres,
                                   struct pipe_resource *stencil)
{
   assert(util_format_has_depth(util_format_description(pres->format)));
   pipe_resource_reference(&pres->next, stencil);
}

static enum pipe_format
zink_resource_get_internal_format(struct pipe_resource *pres)
{
   struct zink_resource *res = zink_resource(pres);
   return res->internal_format;
}

static const struct u_transfer_vtbl transfer_vtbl = {
   .resource_create       = zink_resource_create,
   .resource_destroy      = zink_resource_destroy,
   .transfer_map          = zink_image_map,
   .transfer_unmap        = zink_image_unmap,
   .transfer_flush_region = zink_transfer_flush_region,
   .get_internal_format   = zink_resource_get_internal_format,
   .set_stencil           = zink_resource_set_separate_stencil,
   .get_stencil           = zink_resource_get_separate_stencil,
};

bool
zink_screen_resource_init(struct pipe_screen *pscreen)
{
   struct zink_screen *screen = zink_screen(pscreen);
   pscreen->resource_create = zink_resource_create;
   pscreen->resource_create_with_modifiers = zink_resource_create_with_modifiers;
   pscreen->resource_create_drawable = zink_resource_create_drawable;
   pscreen->resource_destroy = zink_resource_destroy;
   pscreen->transfer_helper = u_transfer_helper_create(&transfer_vtbl, true, true, false, false, !screen->have_D24_UNORM_S8_UINT);

   if (screen->info.have_KHR_external_memory_fd || screen->info.have_KHR_external_memory_win32) {
      pscreen->resource_get_handle = zink_resource_get_handle;
      pscreen->resource_from_handle = zink_resource_from_handle;
   }
   if (screen->instance_info.have_KHR_external_memory_capabilities) {
      pscreen->memobj_create_from_handle = zink_memobj_create_from_handle;
      pscreen->memobj_destroy = zink_memobj_destroy;
      pscreen->resource_from_memobj = zink_resource_from_memobj;
   }
   pscreen->resource_get_param = zink_resource_get_param;
   return true;
}

void
zink_context_resource_init(struct pipe_context *pctx)
{
   pctx->buffer_map = zink_buffer_map;
   pctx->buffer_unmap = zink_buffer_unmap;
   pctx->texture_map = u_transfer_helper_deinterleave_transfer_map;
   pctx->texture_unmap = u_transfer_helper_deinterleave_transfer_unmap;

   pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
   pctx->buffer_subdata = zink_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
   pctx->invalidate_resource = zink_resource_invalidate;
}
