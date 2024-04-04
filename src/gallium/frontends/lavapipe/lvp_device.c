/*
 * Copyright © 2019 Red Hat.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "lvp_private.h"
#include "lvp_conv.h"

#include "pipe-loader/pipe_loader.h"
#include "git_sha1.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_sampler.h"
#include "vk_util.h"
#include "util/detect.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "frontend/drisw_api.h"

#include "util/u_inlines.h"
#include "util/os_memory.h"
#include "util/os_time.h"
#include "util/u_thread.h"
#include "util/u_atomic.h"
#include "util/timespec.h"
#include "util/ptralloc.h"

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
    defined(VK_USE_PLATFORM_WIN32_KHR) || \
    defined(VK_USE_PLATFORM_XCB_KHR) || \
    defined(VK_USE_PLATFORM_XLIB_KHR)
#define LVP_USE_WSI_PLATFORM
#endif
#define LVP_API_VERSION VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateInstanceVersion(uint32_t* pApiVersion)
{
   *pApiVersion = LVP_API_VERSION;
   return VK_SUCCESS;
}

static const struct vk_instance_extension_table lvp_instance_extensions_supported = {
   .KHR_device_group_creation                = true,
   .KHR_external_fence_capabilities          = true,
   .KHR_external_memory_capabilities         = true,
   .KHR_external_semaphore_capabilities      = true,
   .KHR_get_physical_device_properties2      = true,
   .EXT_debug_report                         = true,
   .EXT_debug_utils                          = true,
#ifdef LVP_USE_WSI_PLATFORM
   .KHR_get_surface_capabilities2            = true,
   .KHR_surface                              = true,
   .KHR_surface_protected_capabilities       = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface                      = true,
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
   .KHR_win32_surface                        = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface                          = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface                         = true,
#endif
};

static const struct vk_device_extension_table lvp_device_extensions_supported = {
   .KHR_8bit_storage                      = true,
   .KHR_16bit_storage                     = true,
   .KHR_bind_memory2                      = true,
   .KHR_buffer_device_address             = true,
   .KHR_create_renderpass2                = true,
   .KHR_copy_commands2                    = true,
   .KHR_dedicated_allocation              = true,
   .KHR_depth_stencil_resolve             = true,
   .KHR_descriptor_update_template        = true,
   .KHR_device_group                      = true,
   .KHR_draw_indirect_count               = true,
   .KHR_driver_properties                 = true,
   .KHR_dynamic_rendering                 = true,
   .KHR_format_feature_flags2             = true,
   .KHR_external_fence                    = true,
   .KHR_external_memory                   = true,
#ifdef PIPE_MEMORY_FD
   .KHR_external_memory_fd                = true,
#endif
   .KHR_external_semaphore                = true,
   .KHR_shader_float_controls             = true,
   .KHR_get_memory_requirements2          = true,
#ifdef LVP_USE_WSI_PLATFORM
   .KHR_incremental_present               = true,
#endif
   .KHR_image_format_list                 = true,
   .KHR_imageless_framebuffer             = true,
   .KHR_maintenance1                      = true,
   .KHR_maintenance2                      = true,
   .KHR_maintenance3                      = true,
   .KHR_maintenance4                      = true,
   .KHR_multiview                         = true,
   .KHR_push_descriptor                   = true,
   .KHR_pipeline_library                  = true,
   .KHR_relaxed_block_layout              = true,
   .KHR_sampler_mirror_clamp_to_edge      = true,
   .KHR_separate_depth_stencil_layouts    = true,
   .KHR_shader_atomic_int64               = true,
   .KHR_shader_clock                      = true,
   .KHR_shader_draw_parameters            = true,
   .KHR_shader_float16_int8               = true,
   .KHR_shader_integer_dot_product        = true,
   .KHR_shader_subgroup_extended_types    = true,
   .KHR_shader_terminate_invocation       = true,
   .KHR_spirv_1_4                         = true,
   .KHR_storage_buffer_storage_class      = true,
#ifdef LVP_USE_WSI_PLATFORM
   .KHR_swapchain                         = true,
   .KHR_swapchain_mutable_format          = true,
#endif
   .KHR_synchronization2                  = true,
   .KHR_timeline_semaphore                = true,
   .KHR_uniform_buffer_standard_layout    = true,
   .KHR_variable_pointers                 = true,
   .KHR_vulkan_memory_model               = true,
   .KHR_zero_initialize_workgroup_memory  = true,
   .ARM_rasterization_order_attachment_access = true,
   .EXT_4444_formats                      = true,
   .EXT_attachment_feedback_loop_layout   = true,
   .EXT_border_color_swizzle              = true,
   .EXT_calibrated_timestamps             = true,
   .EXT_color_write_enable                = true,
   .EXT_conditional_rendering             = true,
   .EXT_depth_clip_enable                 = true,
   .EXT_depth_clip_control                = true,
   .EXT_depth_range_unrestricted          = true,
   .EXT_extended_dynamic_state            = true,
   .EXT_extended_dynamic_state2           = true,
   .EXT_extended_dynamic_state3           = true,
   .EXT_external_memory_host              = true,
   .EXT_graphics_pipeline_library         = true,
   .EXT_host_query_reset                  = true,
   .EXT_image_2d_view_of_3d               = true,
   .EXT_image_robustness                  = true,
   .EXT_index_type_uint8                  = true,
   .EXT_inline_uniform_block              = true,
   .EXT_multisampled_render_to_single_sampled = true,
   .EXT_multi_draw                        = true,
   .EXT_non_seamless_cube_map             = true,
   .EXT_pipeline_creation_feedback        = true,
   .EXT_pipeline_creation_cache_control   = true,
   .EXT_post_depth_coverage               = true,
   .EXT_private_data                      = true,
   .EXT_primitives_generated_query        = true,
   .EXT_primitive_topology_list_restart   = true,
   .EXT_rasterization_order_attachment_access = true,
   .EXT_sampler_filter_minmax             = true,
   .EXT_scalar_block_layout               = true,
   .EXT_separate_stencil_usage            = true,
   .EXT_shader_atomic_float               = true,
   .EXT_shader_atomic_float2              = true,
   .EXT_shader_demote_to_helper_invocation= true,
   .EXT_shader_stencil_export             = true,
   .EXT_shader_subgroup_ballot            = true,
   .EXT_shader_subgroup_vote              = true,
   .EXT_shader_viewport_index_layer       = true,
   .EXT_subgroup_size_control             = true,
   .EXT_texel_buffer_alignment            = true,
   .EXT_transform_feedback                = true,
   .EXT_vertex_attribute_divisor          = true,
   .EXT_vertex_input_dynamic_state        = true,
   .EXT_custom_border_color               = true,
   .EXT_provoking_vertex                  = true,
   .EXT_line_rasterization                = true,
   .EXT_robustness2                       = true,
   .GOOGLE_decorate_string                = true,
   .GOOGLE_hlsl_functionality1            = true,
};

static int
min_vertex_pipeline_param(struct pipe_screen *pscreen, enum pipe_shader_cap param)
{
   int val = INT_MAX;
   for (int i = 0; i < PIPE_SHADER_COMPUTE; ++i) {
      if (i == PIPE_SHADER_FRAGMENT ||
          !pscreen->get_shader_param(pscreen, i,
                                     PIPE_SHADER_CAP_MAX_INSTRUCTIONS))
         continue;

      val = MAX2(val, pscreen->get_shader_param(pscreen, i, param));
   }
   return val;
}

static int
min_shader_param(struct pipe_screen *pscreen, enum pipe_shader_cap param)
{
   return MIN3(min_vertex_pipeline_param(pscreen, param),
               pscreen->get_shader_param(pscreen, PIPE_SHADER_FRAGMENT, param),
               pscreen->get_shader_param(pscreen, PIPE_SHADER_COMPUTE, param));
}

static VkResult VKAPI_CALL
lvp_physical_device_init(struct lvp_physical_device *device,
                         struct lvp_instance *instance,
                         struct pipe_loader_device *pld)
{
   VkResult result;

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &lvp_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);
   result = vk_physical_device_init(&device->vk, &instance->vk,
                                    NULL, &dispatch_table);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }
   device->pld = pld;

   device->pscreen = pipe_loader_create_screen_vk(device->pld, true);
   if (!device->pscreen)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   for (unsigned i = 0; i < ARRAY_SIZE(device->drv_options); i++)
      device->drv_options[i] = device->pscreen->get_compiler_options(device->pscreen, PIPE_SHADER_IR_NIR, i);

   device->sync_timeline_type = vk_sync_timeline_get_type(&lvp_pipe_sync_type);
   device->sync_types[0] = &lvp_pipe_sync_type;
   device->sync_types[1] = &device->sync_timeline_type.sync;
   device->sync_types[2] = NULL;
   device->vk.supported_sync_types = device->sync_types;

   device->max_images = device->pscreen->get_shader_param(device->pscreen, PIPE_SHADER_FRAGMENT, PIPE_SHADER_CAP_MAX_SHADER_IMAGES);
   device->vk.supported_extensions = lvp_device_extensions_supported;

   VkSampleCountFlags sample_counts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;

   uint64_t grid_size[3], block_size[3];
   uint64_t max_threads_per_block, max_local_size;

   device->pscreen->get_compute_param(device->pscreen, PIPE_SHADER_IR_NIR,
                                       PIPE_COMPUTE_CAP_MAX_GRID_SIZE, grid_size);
   device->pscreen->get_compute_param(device->pscreen, PIPE_SHADER_IR_NIR,
                                       PIPE_COMPUTE_CAP_MAX_BLOCK_SIZE, block_size);
   device->pscreen->get_compute_param(device->pscreen, PIPE_SHADER_IR_NIR,
                                       PIPE_COMPUTE_CAP_MAX_THREADS_PER_BLOCK,
                                       &max_threads_per_block);
   device->pscreen->get_compute_param(device->pscreen, PIPE_SHADER_IR_NIR,
                                       PIPE_COMPUTE_CAP_MAX_LOCAL_SIZE,
                                       &max_local_size);

   const uint64_t max_render_targets = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_RENDER_TARGETS);
   device->device_limits = (VkPhysicalDeviceLimits) {
      .maxImageDimension1D                      = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_2D_SIZE),
      .maxImageDimension2D                      = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_2D_SIZE),
      .maxImageDimension3D                      = (1 << device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_3D_LEVELS)),
      .maxImageDimensionCube                    = (1 << device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS)),
      .maxImageArrayLayers                      = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS),
      .maxTexelBufferElements                   = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXEL_BUFFER_ELEMENTS_UINT),
      .maxUniformBufferRange                    = min_shader_param(device->pscreen, PIPE_SHADER_CAP_MAX_CONST_BUFFER0_SIZE),
      .maxStorageBufferRange                    = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_SHADER_BUFFER_SIZE_UINT),
      .maxPushConstantsSize                     = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount                 = UINT32_MAX,
      .maxSamplerAllocationCount                = 32 * 1024,
      .bufferImageGranularity                   = 64, /* A cache line */
      .sparseAddressSpaceSize                   = 0,
      .maxBoundDescriptorSets                   = MAX_SETS,
      .maxPerStageDescriptorSamplers            = min_shader_param(device->pscreen, PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS),
      .maxPerStageDescriptorUniformBuffers      = min_shader_param(device->pscreen, PIPE_SHADER_CAP_MAX_CONST_BUFFERS) - 1,
      .maxPerStageDescriptorStorageBuffers      = min_shader_param(device->pscreen, PIPE_SHADER_CAP_MAX_SHADER_BUFFERS),
      .maxPerStageDescriptorSampledImages       = min_shader_param(device->pscreen, PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS),
      .maxPerStageDescriptorStorageImages       = min_shader_param(device->pscreen, PIPE_SHADER_CAP_MAX_SHADER_IMAGES),
      .maxPerStageDescriptorInputAttachments    = 8,
      .maxPerStageResources                     = 128,
      .maxDescriptorSetSamplers                 = 32 * 1024,
      .maxDescriptorSetUniformBuffers           = 256,
      .maxDescriptorSetUniformBuffersDynamic    = 256,
      .maxDescriptorSetStorageBuffers           = 256,
      .maxDescriptorSetStorageBuffersDynamic    = 256,
      .maxDescriptorSetSampledImages            = 256,
      .maxDescriptorSetStorageImages            = 256,
      .maxDescriptorSetInputAttachments         = 256,
      .maxVertexInputAttributes                 = 32,
      .maxVertexInputBindings                   = 32,
      .maxVertexInputAttributeOffset            = 2047,
      .maxVertexInputBindingStride              = 2048,
      .maxVertexOutputComponents                = 128,
      .maxTessellationGenerationLevel           = 64,
      .maxTessellationPatchSize                 = 32,
      .maxTessellationControlPerVertexInputComponents = 128,
      .maxTessellationControlPerVertexOutputComponents = 128,
      .maxTessellationControlPerPatchOutputComponents = 128,
      .maxTessellationControlTotalOutputComponents = 4096,
      .maxTessellationEvaluationInputComponents = 128,
      .maxTessellationEvaluationOutputComponents = 128,
      .maxGeometryShaderInvocations             = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_GS_INVOCATIONS),
      .maxGeometryInputComponents               = 64,
      .maxGeometryOutputComponents              = 128,
      .maxGeometryOutputVertices                = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES),
      .maxGeometryTotalOutputComponents         = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS),
      .maxFragmentInputComponents               = 128,
      .maxFragmentOutputAttachments             = 8,
      .maxFragmentDualSrcAttachments            = 2,
      .maxFragmentCombinedOutputResources       = max_render_targets +
                                                  device->pscreen->get_shader_param(device->pscreen, PIPE_SHADER_FRAGMENT,
                                                     PIPE_SHADER_CAP_MAX_SHADER_BUFFERS) +
                                                  device->pscreen->get_shader_param(device->pscreen, PIPE_SHADER_FRAGMENT,
                                                     PIPE_SHADER_CAP_MAX_SHADER_IMAGES),
      .maxComputeSharedMemorySize               = max_local_size,
      .maxComputeWorkGroupCount                 = { grid_size[0], grid_size[1], grid_size[2] },
      .maxComputeWorkGroupInvocations           = max_threads_per_block,
      .maxComputeWorkGroupSize                  = { block_size[0], block_size[1], block_size[2] },
      .subPixelPrecisionBits                    = device->pscreen->get_param(device->pscreen, PIPE_CAP_RASTERIZER_SUBPIXEL_BITS),
      .subTexelPrecisionBits                    = 8,
      .mipmapPrecisionBits                      = 4,
      .maxDrawIndexedIndexValue                 = UINT32_MAX,
      .maxDrawIndirectCount                     = UINT32_MAX,
      .maxSamplerLodBias                        = 16,
      .maxSamplerAnisotropy                     = 16,
      .maxViewports                             = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_VIEWPORTS),
      .maxViewportDimensions                    = { (1 << 14), (1 << 14) },
      .viewportBoundsRange                      = { -32768.0, 32768.0 },
      .viewportSubPixelBits                     = device->pscreen->get_param(device->pscreen, PIPE_CAP_VIEWPORT_SUBPIXEL_BITS),
      .minMemoryMapAlignment                    = device->pscreen->get_param(device->pscreen, PIPE_CAP_MIN_MAP_BUFFER_ALIGNMENT),
      .minTexelBufferOffsetAlignment            = device->pscreen->get_param(device->pscreen, PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT),
      .minUniformBufferOffsetAlignment          = device->pscreen->get_param(device->pscreen, PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT),
      .minStorageBufferOffsetAlignment          = device->pscreen->get_param(device->pscreen, PIPE_CAP_SHADER_BUFFER_OFFSET_ALIGNMENT),
      .minTexelOffset                           = device->pscreen->get_param(device->pscreen, PIPE_CAP_MIN_TEXEL_OFFSET),
      .maxTexelOffset                           = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXEL_OFFSET),
      .minTexelGatherOffset                     = device->pscreen->get_param(device->pscreen, PIPE_CAP_MIN_TEXTURE_GATHER_OFFSET),
      .maxTexelGatherOffset                     = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_GATHER_OFFSET),
      .minInterpolationOffset                   = -2, /* FIXME */
      .maxInterpolationOffset                   = 2, /* FIXME */
      .subPixelInterpolationOffsetBits          = 8, /* FIXME */
      .maxFramebufferWidth                      = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_2D_SIZE),
      .maxFramebufferHeight                     = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_2D_SIZE),
      .maxFramebufferLayers                     = device->pscreen->get_param(device->pscreen, PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS),
      .framebufferColorSampleCounts             = sample_counts,
      .framebufferDepthSampleCounts             = sample_counts,
      .framebufferStencilSampleCounts           = sample_counts,
      .framebufferNoAttachmentsSampleCounts     = sample_counts,
      .maxColorAttachments                      = max_render_targets,
      .sampledImageColorSampleCounts            = sample_counts,
      .sampledImageIntegerSampleCounts          = sample_counts,
      .sampledImageDepthSampleCounts            = sample_counts,
      .sampledImageStencilSampleCounts          = sample_counts,
      .storageImageSampleCounts                 = sample_counts,
      .maxSampleMaskWords                       = 1,
      .timestampComputeAndGraphics              = true,
      .timestampPeriod                          = 1,
      .maxClipDistances                         = 8,
      .maxCullDistances                         = 8,
      .maxCombinedClipAndCullDistances          = 8,
      .discreteQueuePriorities                  = 2,
      .pointSizeRange                           = { 0.0, device->pscreen->get_paramf(device->pscreen, PIPE_CAPF_MAX_POINT_SIZE) },
      .lineWidthRange                           = { 1.0, device->pscreen->get_paramf(device->pscreen, PIPE_CAPF_MAX_LINE_WIDTH) },
      .pointSizeGranularity                     = (1.0 / 8.0),
      .lineWidthGranularity                     = 1.0 / 128.0,
      .strictLines                              = true,
      .standardSampleLocations                  = true,
      .optimalBufferCopyOffsetAlignment         = 128,
      .optimalBufferCopyRowPitchAlignment       = 128,
      .nonCoherentAtomSize                      = 64,
   };
   result = lvp_init_wsi(device);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&device->vk);
      vk_error(instance, result);
      goto fail;
   }

   return VK_SUCCESS;
 fail:
   return result;
}

static void VKAPI_CALL
lvp_physical_device_finish(struct lvp_physical_device *device)
{
   lvp_finish_wsi(device);
   device->pscreen->destroy(device->pscreen);
   vk_physical_device_finish(&device->vk);
}

static void
lvp_destroy_physical_device(struct vk_physical_device *device)
{
   lvp_physical_device_finish((struct lvp_physical_device *)device);
   vk_free(&device->instance->alloc, device);
}

static VkResult
lvp_enumerate_physical_devices(struct vk_instance *vk_instance);

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateInstance(
   const VkInstanceCreateInfo*                 pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkInstance*                                 pInstance)
{
   struct lvp_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   instance = vk_zalloc(pAllocator, sizeof(*instance), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &lvp_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk,
                             &lvp_instance_extensions_supported,
                             &dispatch_table,
                             pCreateInfo,
                             pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(instance, result);
   }

   instance->apiVersion = LVP_API_VERSION;

   instance->vk.physical_devices.enumerate = lvp_enumerate_physical_devices;
   instance->vk.physical_devices.destroy = lvp_destroy_physical_device;

   //   _mesa_locale_init();
   //   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = lvp_instance_to_handle(instance);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyInstance(
   VkInstance                                  _instance,
   const VkAllocationCallbacks*                pAllocator)
{
   LVP_FROM_HANDLE(lvp_instance, instance, _instance);

   if (!instance)
      return;
   //   _mesa_locale_fini();

   pipe_loader_release(&instance->devs, instance->num_devices);

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

#if defined(HAVE_DRI)
static void lvp_get_image(struct dri_drawable *dri_drawable,
                          int x, int y, unsigned width, unsigned height, unsigned stride,
                          void *data)
{

}

static void lvp_put_image(struct dri_drawable *dri_drawable,
                          void *data, unsigned width, unsigned height)
{
   fprintf(stderr, "put image %dx%d\n", width, height);
}

static void lvp_put_image2(struct dri_drawable *dri_drawable,
                           void *data, int x, int y, unsigned width, unsigned height,
                           unsigned stride)
{
   fprintf(stderr, "put image 2 %d,%d %dx%d\n", x, y, width, height);
}

static struct drisw_loader_funcs lvp_sw_lf = {
   .get_image = lvp_get_image,
   .put_image = lvp_put_image,
   .put_image2 = lvp_put_image2,
};
#endif

static VkResult
lvp_enumerate_physical_devices(struct vk_instance *vk_instance)
{
   struct lvp_instance *instance =
      container_of(vk_instance, struct lvp_instance, vk);

   /* sw only for now */
   instance->num_devices = pipe_loader_sw_probe(NULL, 0);

   assert(instance->num_devices == 1);

#if defined(HAVE_DRI)
   pipe_loader_sw_probe_dri(&instance->devs, &lvp_sw_lf);
#else
   pipe_loader_sw_probe_null(&instance->devs);
#endif

   struct lvp_physical_device *device =
      vk_zalloc2(&instance->vk.alloc, NULL, sizeof(*device), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = lvp_physical_device_init(device, instance, &instance->devs[0]);
   if (result == VK_SUCCESS)
      list_addtail(&device->vk.link, &instance->vk.physical_devices.list);
   else
      vk_free(&vk_instance->alloc, device);

   return result;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceFeatures(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceFeatures*                   pFeatures)
{
   LVP_FROM_HANDLE(lvp_physical_device, pdevice, physicalDevice);
   bool indirect = false;//pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_GLSL_FEATURE_LEVEL) >= 400;
   memset(pFeatures, 0, sizeof(*pFeatures));
   *pFeatures = (VkPhysicalDeviceFeatures) {
      .robustBufferAccess                       = true,
      .fullDrawIndexUint32                      = true,
      .imageCubeArray                           = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_CUBE_MAP_ARRAY) != 0),
      .independentBlend                         = true,
      .geometryShader                           = (pdevice->pscreen->get_shader_param(pdevice->pscreen, PIPE_SHADER_GEOMETRY, PIPE_SHADER_CAP_MAX_INSTRUCTIONS) != 0),
      .tessellationShader                       = (pdevice->pscreen->get_shader_param(pdevice->pscreen, PIPE_SHADER_TESS_EVAL, PIPE_SHADER_CAP_MAX_INSTRUCTIONS) != 0),
      .sampleRateShading                        = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_SAMPLE_SHADING) != 0),
      .dualSrcBlend                             = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS) != 0),
      .logicOp                                  = true,
      .multiDrawIndirect                        = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_MULTI_DRAW_INDIRECT) != 0),
      .drawIndirectFirstInstance                = true,
      .depthClamp                               = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_DEPTH_CLIP_DISABLE) != 0),
      .depthBiasClamp                           = true,
      .fillModeNonSolid                         = true,
      .depthBounds                              = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_DEPTH_BOUNDS_TEST) != 0),
      .wideLines                                = true,
      .largePoints                              = true,
      .alphaToOne                               = true,
      .multiViewport                            = true,
      .samplerAnisotropy                        = true,
      .textureCompressionETC2                   = false,
      .textureCompressionASTC_LDR               = false,
      .textureCompressionBC                     = true,
      .occlusionQueryPrecise                    = true,
      .pipelineStatisticsQuery                  = true,
      .vertexPipelineStoresAndAtomics           = (min_vertex_pipeline_param(pdevice->pscreen, PIPE_SHADER_CAP_MAX_SHADER_BUFFERS) != 0),
      .fragmentStoresAndAtomics                 = (pdevice->pscreen->get_shader_param(pdevice->pscreen, PIPE_SHADER_FRAGMENT, PIPE_SHADER_CAP_MAX_SHADER_BUFFERS) != 0),
      .shaderTessellationAndGeometryPointSize   = true,
      .shaderImageGatherExtended                = true,
      .shaderStorageImageExtendedFormats        = (min_shader_param(pdevice->pscreen, PIPE_SHADER_CAP_MAX_SHADER_IMAGES) != 0),
      .shaderStorageImageMultisample            = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_TEXTURE_MULTISAMPLE) != 0),
      .shaderUniformBufferArrayDynamicIndexing  = true,
      .shaderSampledImageArrayDynamicIndexing   = indirect,
      .shaderStorageBufferArrayDynamicIndexing  = true,
      .shaderStorageImageArrayDynamicIndexing   = indirect,
      .shaderStorageImageReadWithoutFormat      = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_IMAGE_LOAD_FORMATTED) != 0),
      .shaderStorageImageWriteWithoutFormat     = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_IMAGE_STORE_FORMATTED) != 0),
      .shaderClipDistance                       = true,
      .shaderCullDistance                       = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_CULL_DISTANCE) == 1),
      .shaderFloat64                            = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_DOUBLES) == 1),
      .shaderInt64                              = (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_INT64) == 1),
      .shaderInt16                              = (min_shader_param(pdevice->pscreen, PIPE_SHADER_CAP_INT16) == 1),
      .variableMultisampleRate                  = false,
      .inheritedQueries                         = false,
   };
}

static void
lvp_get_physical_device_features_1_1(struct lvp_physical_device *pdevice,
                                     VkPhysicalDeviceVulkan11Features *f)
{
   assert(f->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);

   f->storageBuffer16BitAccess            = true;
   f->uniformAndStorageBuffer16BitAccess  = true;
   f->storagePushConstant16               = true;
   f->storageInputOutput16                = false;
   f->multiview                           = true;
   f->multiviewGeometryShader             = true;
   f->multiviewTessellationShader         = true;
   f->variablePointersStorageBuffer       = true;
   f->variablePointers                    = true;
   f->protectedMemory                     = false;
   f->samplerYcbcrConversion              = false;
   f->shaderDrawParameters                = true;
}

static void
lvp_get_physical_device_features_1_2(struct lvp_physical_device *pdevice,
                                     VkPhysicalDeviceVulkan12Features *f)
{
   assert(f->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);

   f->samplerMirrorClampToEdge = true;
   f->drawIndirectCount = true;
   f->storageBuffer8BitAccess = true;
   f->uniformAndStorageBuffer8BitAccess = true;
   f->storagePushConstant8 = true;
   f->shaderBufferInt64Atomics = true;
   f->shaderSharedInt64Atomics = true;
   f->shaderFloat16 = pdevice->pscreen->get_shader_param(pdevice->pscreen, PIPE_SHADER_FRAGMENT, PIPE_SHADER_CAP_FP16) != 0;
   f->shaderInt8 = true;

   f->descriptorIndexing = false;
   f->shaderInputAttachmentArrayDynamicIndexing = false;
   f->shaderUniformTexelBufferArrayDynamicIndexing = false;
   f->shaderStorageTexelBufferArrayDynamicIndexing = false;
   f->shaderUniformBufferArrayNonUniformIndexing = false;
   f->shaderSampledImageArrayNonUniformIndexing = false;
   f->shaderStorageBufferArrayNonUniformIndexing = false;
   f->shaderStorageImageArrayNonUniformIndexing = false;
   f->shaderInputAttachmentArrayNonUniformIndexing = false;
   f->shaderUniformTexelBufferArrayNonUniformIndexing = false;
   f->shaderStorageTexelBufferArrayNonUniformIndexing = false;
   f->descriptorBindingUniformBufferUpdateAfterBind = false;
   f->descriptorBindingSampledImageUpdateAfterBind = false;
   f->descriptorBindingStorageImageUpdateAfterBind = false;
   f->descriptorBindingStorageBufferUpdateAfterBind = false;
   f->descriptorBindingUniformTexelBufferUpdateAfterBind = false;
   f->descriptorBindingStorageTexelBufferUpdateAfterBind = false;
   f->descriptorBindingUpdateUnusedWhilePending = false;
   f->descriptorBindingPartiallyBound = false;
   f->descriptorBindingVariableDescriptorCount = false;
   f->runtimeDescriptorArray = false;

   f->samplerFilterMinmax = true;
   f->scalarBlockLayout = true;
   f->imagelessFramebuffer = true;
   f->uniformBufferStandardLayout = true;
   f->shaderSubgroupExtendedTypes = true;
   f->separateDepthStencilLayouts = true;
   f->hostQueryReset = true;
   f->timelineSemaphore = true;
   f->bufferDeviceAddress = true;
   f->bufferDeviceAddressCaptureReplay = false;
   f->bufferDeviceAddressMultiDevice = false;
   f->vulkanMemoryModel = true;
   f->vulkanMemoryModelDeviceScope = true;
   f->vulkanMemoryModelAvailabilityVisibilityChains = true;
   f->shaderOutputViewportIndex = true;
   f->shaderOutputLayer = true;
   f->subgroupBroadcastDynamicId = true;
}

static void
lvp_get_physical_device_features_1_3(struct lvp_physical_device *pdevice,
                                     VkPhysicalDeviceVulkan13Features *f)
{
   assert(f->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);

   f->robustImageAccess = VK_TRUE;
   f->inlineUniformBlock = VK_TRUE;
   f->descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
   f->pipelineCreationCacheControl = VK_TRUE;
   f->privateData = VK_TRUE;
   f->shaderDemoteToHelperInvocation = VK_TRUE;
   f->shaderTerminateInvocation = VK_TRUE;
   f->subgroupSizeControl = VK_TRUE;
   f->computeFullSubgroups = VK_TRUE;
   f->synchronization2 = VK_TRUE;
   f->textureCompressionASTC_HDR = VK_FALSE;
   f->shaderZeroInitializeWorkgroupMemory = VK_TRUE;
   f->dynamicRendering = VK_TRUE;
   f->shaderIntegerDotProduct = VK_TRUE;
   f->maintenance4 = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceFeatures2(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceFeatures2                  *pFeatures)
{
   LVP_FROM_HANDLE(lvp_physical_device, pdevice, physicalDevice);
   lvp_GetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

   VkPhysicalDeviceVulkan11Features core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };
   lvp_get_physical_device_features_1_1(pdevice, &core_1_1);

   VkPhysicalDeviceVulkan12Features core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
   };
   lvp_get_physical_device_features_1_2(pdevice, &core_1_2);

   VkPhysicalDeviceVulkan13Features core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   lvp_get_physical_device_features_1_3(pdevice, &core_1_3);

   vk_foreach_struct(ext, pFeatures->pNext) {

      if (vk_get_physical_device_core_1_1_feature_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_feature_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_feature_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES: {
         VkPhysicalDevicePrivateDataFeatures *features =
            (VkPhysicalDevicePrivateDataFeatures *)ext;
         features->privateData = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
         VkPhysicalDeviceSynchronization2Features *features =
            (VkPhysicalDeviceSynchronization2Features *)ext;
         features->synchronization2 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES: {
         VkPhysicalDevicePipelineCreationCacheControlFeatures *features =
            (VkPhysicalDevicePipelineCreationCacheControlFeatures *)ext;
         features->pipelineCreationCacheControl = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT: {
         VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *features =
            (VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *)ext;
         features->primitivesGeneratedQuery = true;
         features->primitivesGeneratedQueryWithRasterizerDiscard = true;
         features->primitivesGeneratedQueryWithNonZeroStreams = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT: {
         VkPhysicalDeviceBorderColorSwizzleFeaturesEXT *features =
            (VkPhysicalDeviceBorderColorSwizzleFeaturesEXT *)ext;
         features->borderColorSwizzle = true;
         features->borderColorSwizzleFromImage = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT: {
         VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT *features =
            (VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT *)ext;
         features->nonSeamlessCubeMap = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT: {
         VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT *features =
            (VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT *)ext;
         features->attachmentFeedbackLoopLayout = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT: {
         VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT *features =
            (VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT *)ext;
         features->rasterizationOrderColorAttachmentAccess = true;
         features->rasterizationOrderDepthAttachmentAccess = true;
         features->rasterizationOrderStencilAttachmentAccess = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT: {
         VkPhysicalDeviceLineRasterizationFeaturesEXT *features =
            (VkPhysicalDeviceLineRasterizationFeaturesEXT *)ext;
         features->rectangularLines = true;
         features->bresenhamLines = true;
         features->smoothLines = true;
         features->stippledRectangularLines = true;
         features->stippledBresenhamLines = true;
         features->stippledSmoothLines = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *features =
            (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)ext;
         if (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR) != 0) {
            features->vertexAttributeInstanceRateZeroDivisor = true;
            features->vertexAttributeInstanceRateDivisor = true;
         } else {
            features->vertexAttributeInstanceRateDivisor = false;
            features->vertexAttributeInstanceRateZeroDivisor = false;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT: {
         VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT *features =
            (VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT *)ext;
         features->multisampledRenderToSingleSampled = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
         VkPhysicalDeviceIndexTypeUint8FeaturesEXT *features =
            (VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)ext;
         features->indexTypeUint8 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: {
         VkPhysicalDeviceShaderIntegerDotProductFeatures *features =
            (VkPhysicalDeviceShaderIntegerDotProductFeatures *)ext;
         features->shaderIntegerDotProduct = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT: {
         VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *features =
            (VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)ext;
         features->vertexInputDynamicState = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
         VkPhysicalDeviceMaintenance4Features *features =
            (VkPhysicalDeviceMaintenance4Features *)ext;
         features->maintenance4 = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
         VkPhysicalDeviceSubgroupSizeControlFeatures *features =
            (VkPhysicalDeviceSubgroupSizeControlFeatures *)ext;
         features->subgroupSizeControl = true;
         features->computeFullSubgroups = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipControlFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipControlFeaturesEXT *)ext;
         features->depthClipControl = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES: {
         VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures *features =
            (VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures *)ext;
         features->shaderZeroInitializeWorkgroupMemory = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR: {
         VkPhysicalDeviceShaderClockFeaturesKHR *features =
            (VkPhysicalDeviceShaderClockFeaturesKHR *)ext;
         features->shaderSubgroupClock = true;
         features->shaderDeviceClock = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT: {
         VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *features =
            (VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *)ext;
         features->texelBufferAlignment = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
         VkPhysicalDeviceTransformFeedbackFeaturesEXT *features =
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT*)ext;

         features->transformFeedback = true;
         features->geometryStreams = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT: {
         VkPhysicalDeviceConditionalRenderingFeaturesEXT *features =
            (VkPhysicalDeviceConditionalRenderingFeaturesEXT*)ext;
         features->conditionalRendering = true;
         features->inheritedConditionalRendering = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *features =
            (VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)ext;
         features->extendedDynamicState = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES: {
         VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *features =
            (VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *)ext;
         features->shaderDemoteToHelperInvocation = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
         VkPhysicalDevice4444FormatsFeaturesEXT *features =
            (VkPhysicalDevice4444FormatsFeaturesEXT*)ext;
         features->formatA4R4G4B4 = true;
         features->formatA4B4G4R4 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES: {
         VkPhysicalDeviceInlineUniformBlockFeatures *features =
            (VkPhysicalDeviceInlineUniformBlockFeatures*)ext;
         features->inlineUniformBlock = true;
         features->descriptorBindingInlineUniformBlockUpdateAfterBind = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
         VkPhysicalDeviceCustomBorderColorFeaturesEXT *features =
            (VkPhysicalDeviceCustomBorderColorFeaturesEXT *)ext;
         features->customBorderColors = true;
         features->customBorderColorWithoutFormat = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceColorWriteEnableFeaturesEXT *features =
            (VkPhysicalDeviceColorWriteEnableFeaturesEXT *)ext;
         features->colorWriteEnable = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT: {
         VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *features =
            (VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *)ext;
         features->image2DViewOf3D = true;
         features->sampler2DViewOf3D = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
         VkPhysicalDeviceProvokingVertexFeaturesEXT *features =
            (VkPhysicalDeviceProvokingVertexFeaturesEXT*)ext;
         features->provokingVertexLast = true;
         features->transformFeedbackPreservesProvokingVertex = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT: {
         VkPhysicalDeviceMultiDrawFeaturesEXT *features = (VkPhysicalDeviceMultiDrawFeaturesEXT *)ext;
         features->multiDraw = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipEnableFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)ext;
         if (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_DEPTH_CLAMP_ENABLE) != 0)
            features->depthClipEnable = true;
         else
            features->depthClipEnable = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *features = (VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *)ext;
         features->extendedDynamicState2 = true;
         features->extendedDynamicState2LogicOp = true;
         features->extendedDynamicState2PatchControlPoints = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *features = (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *)ext;
         features->extendedDynamicState3PolygonMode = VK_TRUE;
         features->extendedDynamicState3TessellationDomainOrigin = VK_TRUE;
         features->extendedDynamicState3DepthClampEnable = VK_TRUE;
         features->extendedDynamicState3DepthClipEnable = VK_TRUE;
         features->extendedDynamicState3LogicOpEnable = VK_TRUE;
         features->extendedDynamicState3SampleMask = VK_TRUE;
         features->extendedDynamicState3RasterizationSamples = VK_TRUE;
         features->extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
         features->extendedDynamicState3AlphaToOneEnable = VK_TRUE;
         features->extendedDynamicState3DepthClipNegativeOneToOne = VK_TRUE;
         features->extendedDynamicState3RasterizationStream = VK_FALSE;
         features->extendedDynamicState3ConservativeRasterizationMode = VK_FALSE;
         features->extendedDynamicState3ExtraPrimitiveOverestimationSize = VK_FALSE;
         features->extendedDynamicState3LineRasterizationMode = VK_TRUE;
         features->extendedDynamicState3LineStippleEnable = VK_TRUE;
         features->extendedDynamicState3ProvokingVertexMode = VK_TRUE;
         features->extendedDynamicState3SampleLocationsEnable = VK_FALSE;
         features->extendedDynamicState3ColorBlendEnable = VK_TRUE;
         features->extendedDynamicState3ColorBlendEquation = VK_TRUE;
         features->extendedDynamicState3ColorWriteMask = VK_TRUE;
         features->extendedDynamicState3ViewportWScalingEnable = VK_FALSE;
         features->extendedDynamicState3ViewportSwizzle = VK_FALSE;
         features->extendedDynamicState3ShadingRateImageEnable = VK_FALSE;
         features->extendedDynamicState3CoverageToColorEnable = VK_FALSE;
         features->extendedDynamicState3CoverageToColorLocation = VK_FALSE;
         features->extendedDynamicState3CoverageModulationMode = VK_FALSE;
         features->extendedDynamicState3CoverageModulationTableEnable = VK_FALSE;
         features->extendedDynamicState3CoverageModulationTable = VK_FALSE;
         features->extendedDynamicState3CoverageReductionMode = VK_FALSE;
         features->extendedDynamicState3RepresentativeFragmentTestEnable = VK_FALSE;
         features->extendedDynamicState3ColorBlendAdvanced = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES: {
         VkPhysicalDeviceImageRobustnessFeatures *features = (VkPhysicalDeviceImageRobustnessFeatures *)ext;
         features->robustImageAccess = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
         VkPhysicalDeviceRobustness2FeaturesEXT *features = (VkPhysicalDeviceRobustness2FeaturesEXT *)ext;
         features->robustBufferAccess2 = true;
         features->robustImageAccess2 = true;
         features->nullDescriptor = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT: {
         VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *features = (VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *)ext;
         features->primitiveTopologyListRestart = true;
         features->primitiveTopologyPatchListRestart = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES: {
         VkPhysicalDeviceShaderTerminateInvocationFeatures *features = (VkPhysicalDeviceShaderTerminateInvocationFeatures *)ext;
         features->shaderTerminateInvocation = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
         VkPhysicalDeviceDynamicRenderingFeatures *features = (VkPhysicalDeviceDynamicRenderingFeatures *)ext;
         features->dynamicRendering = VK_TRUE;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT: {
         VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *features = (VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)ext;
         features->graphicsPipelineLibrary = VK_TRUE;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT: {
         VkPhysicalDeviceShaderAtomicFloatFeaturesEXT *features = (VkPhysicalDeviceShaderAtomicFloatFeaturesEXT *)ext;
         features->shaderBufferFloat32Atomics =    true;
         features->shaderBufferFloat32AtomicAdd =  true;
         features->shaderBufferFloat64Atomics = false;
         features->shaderBufferFloat64AtomicAdd =  false;
         features->shaderSharedFloat32Atomics =    true;
         features->shaderSharedFloat32AtomicAdd =  true;
         features->shaderSharedFloat64Atomics =    false;
         features->shaderSharedFloat64AtomicAdd =  false;
         features->shaderImageFloat32Atomics =     true;
         features->shaderImageFloat32AtomicAdd =   true;
         features->sparseImageFloat32Atomics =     false;
         features->sparseImageFloat32AtomicAdd =   false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT: {
         VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT *features = (VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT *)ext;
         features->shaderBufferFloat16Atomics      = false;
         features->shaderBufferFloat16AtomicAdd    = false;
         features->shaderBufferFloat16AtomicMinMax = false;
         features->shaderBufferFloat32AtomicMinMax = LLVM_VERSION_MAJOR >= 15;
         features->shaderBufferFloat64AtomicMinMax = false;
         features->shaderSharedFloat16Atomics      = false;
         features->shaderSharedFloat16AtomicAdd    = false;
         features->shaderSharedFloat16AtomicMinMax = false;
         features->shaderSharedFloat32AtomicMinMax = LLVM_VERSION_MAJOR >= 15;
         features->shaderSharedFloat64AtomicMinMax = false;
         features->shaderImageFloat32AtomicMinMax  = LLVM_VERSION_MAJOR >= 15;
         features->sparseImageFloat32AtomicMinMax  = false;
         break;
      }
      default:
         break;
      }
   }
}

void
lvp_device_get_cache_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
   snprintf(uuid, VK_UUID_SIZE, "val-%s", &MESA_GIT_SHA1[4]);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties *pProperties)
{
   LVP_FROM_HANDLE(lvp_physical_device, pdevice, physicalDevice);

   *pProperties = (VkPhysicalDeviceProperties) {
      .apiVersion = LVP_API_VERSION,
      .driverVersion = 1,
      .vendorID = VK_VENDOR_ID_MESA,
      .deviceID = 0,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU,
      .limits = pdevice->device_limits,
      .sparseProperties = {0},
   };

   strcpy(pProperties->deviceName, pdevice->pscreen->get_name(pdevice->pscreen));
   lvp_device_get_cache_uuid(pProperties->pipelineCacheUUID);

}

extern unsigned lp_native_vector_width;
static void
lvp_get_physical_device_properties_1_1(struct lvp_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan11Properties *p)
{
   assert(p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);

   pdevice->pscreen->get_device_uuid(pdevice->pscreen, (char*)(p->deviceUUID));
   pdevice->pscreen->get_driver_uuid(pdevice->pscreen, (char*)(p->driverUUID));
   memset(p->deviceLUID, 0, VK_LUID_SIZE);
   /* The LUID is for Windows. */
   p->deviceLUIDValid = false;
   p->deviceNodeMask = 0;

   p->subgroupSize = lp_native_vector_width / 32;
   p->subgroupSupportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
   p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
   p->subgroupQuadOperationsInAllStages = false;

#if LLVM_VERSION_MAJOR >= 10
   p->subgroupSupportedOperations |= VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
#endif

   p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
   p->maxMultiviewViewCount = 6;
   p->maxMultiviewInstanceIndex = INT_MAX;
   p->protectedNoFault = false;
   p->maxPerSetDescriptors = 1024;
   p->maxMemoryAllocationSize = (1u << 31);
}

static void
lvp_get_physical_device_properties_1_2(struct lvp_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan12Properties *p)
{
   p->driverID = VK_DRIVER_ID_MESA_LLVMPIPE;
   snprintf(p->driverName, VK_MAX_DRIVER_NAME_SIZE, "llvmpipe");
   snprintf(p->driverInfo, VK_MAX_DRIVER_INFO_SIZE, "Mesa " PACKAGE_VERSION MESA_GIT_SHA1
#ifdef MESA_LLVM_VERSION_STRING
                  " (LLVM " MESA_LLVM_VERSION_STRING ")"
#endif
            );

   p->conformanceVersion = (VkConformanceVersion){
      .major = 1,
      .minor = 3,
      .subminor = 1,
      .patch = 1,
   };

   p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
   p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
   p->shaderDenormFlushToZeroFloat16 = false;
   p->shaderDenormPreserveFloat16 = false;
   p->shaderRoundingModeRTEFloat16 = true;
   p->shaderRoundingModeRTZFloat16 = false;
   p->shaderSignedZeroInfNanPreserveFloat16 = true;

   p->shaderDenormFlushToZeroFloat32 = false;
   p->shaderDenormPreserveFloat32 = false;
   p->shaderRoundingModeRTEFloat32 = true;
   p->shaderRoundingModeRTZFloat32 = false;
   p->shaderSignedZeroInfNanPreserveFloat32 = true;

   p->shaderDenormFlushToZeroFloat64 = false;
   p->shaderDenormPreserveFloat64 = false;
   p->shaderRoundingModeRTEFloat64 = true;
   p->shaderRoundingModeRTZFloat64 = false;
   p->shaderSignedZeroInfNanPreserveFloat64 = true;

   p->maxUpdateAfterBindDescriptorsInAllPools = UINT32_MAX / 64;
   p->shaderUniformBufferArrayNonUniformIndexingNative = false;
   p->shaderSampledImageArrayNonUniformIndexingNative = false;
   p->shaderStorageBufferArrayNonUniformIndexingNative = false;
   p->shaderStorageImageArrayNonUniformIndexingNative = false;
   p->shaderInputAttachmentArrayNonUniformIndexingNative = false;
   p->robustBufferAccessUpdateAfterBind = true;
   p->quadDivergentImplicitLod = false;

   size_t max_descriptor_set_size = 65536; //TODO
   p->maxPerStageDescriptorUpdateAfterBindSamplers = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindSampledImages = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindStorageImages = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindInputAttachments = max_descriptor_set_size;
   p->maxPerStageUpdateAfterBindResources = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindSamplers = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindUniformBuffers = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 16;
   p->maxDescriptorSetUpdateAfterBindStorageBuffers = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 16;
   p->maxDescriptorSetUpdateAfterBindSampledImages = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindStorageImages = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindInputAttachments = max_descriptor_set_size;

   p->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT | VK_RESOLVE_MODE_AVERAGE_BIT;
   p->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
   p->independentResolveNone = false;
   p->independentResolve = false;

   p->filterMinmaxImageComponentMapping = true;
   p->filterMinmaxSingleComponentFormats = true;

   p->maxTimelineSemaphoreValueDifference = UINT64_MAX;
   p->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
}

static void
lvp_get_physical_device_properties_1_3(struct lvp_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan13Properties *p)
{
   p->minSubgroupSize = lp_native_vector_width / 32;
   p->maxSubgroupSize = lp_native_vector_width / 32;
   p->maxComputeWorkgroupSubgroups = 32;
   p->requiredSubgroupSizeStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
   p->maxInlineUniformTotalSize = MAX_DESCRIPTOR_UNIFORM_BLOCK_SIZE * MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS * MAX_SETS;
   p->maxInlineUniformBlockSize = MAX_DESCRIPTOR_UNIFORM_BLOCK_SIZE;
   p->maxPerStageDescriptorInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
   p->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
   p->maxDescriptorSetInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
   p->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
   int alignment = pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT);
   p->storageTexelBufferOffsetAlignmentBytes = alignment;
   p->storageTexelBufferOffsetSingleTexelAlignment = true;
   p->uniformTexelBufferOffsetAlignmentBytes = alignment;
   p->uniformTexelBufferOffsetSingleTexelAlignment = true;
   p->maxBufferSize = UINT32_MAX;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceProperties2(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceProperties2                *pProperties)
{
   LVP_FROM_HANDLE(lvp_physical_device, pdevice, physicalDevice);
   lvp_GetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

   VkPhysicalDeviceVulkan11Properties core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
   };
   lvp_get_physical_device_properties_1_1(pdevice, &core_1_1);

   VkPhysicalDeviceVulkan12Properties core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
   };
   lvp_get_physical_device_properties_1_2(pdevice, &core_1_2);

   VkPhysicalDeviceVulkan13Properties core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
   };
   lvp_get_physical_device_properties_1_3(pdevice, &core_1_3);

   vk_foreach_struct(ext, pProperties->pNext) {

      if (vk_get_physical_device_core_1_1_property_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_property_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_property_ext(ext, &core_1_3))
         continue;
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
         VkPhysicalDevicePushDescriptorPropertiesKHR *properties =
            (VkPhysicalDevicePushDescriptorPropertiesKHR *) ext;
         properties->maxPushDescriptors = MAX_PUSH_DESCRIPTORS;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES: {
         VkPhysicalDeviceShaderIntegerDotProductProperties *properties =
            (VkPhysicalDeviceShaderIntegerDotProductProperties *) ext;
         void *pnext = properties->pNext;
         memset(properties, 0, sizeof(VkPhysicalDeviceShaderIntegerDotProductProperties));
         properties->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES;
         properties->pNext = pnext;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES: {
         VkPhysicalDevicePointClippingProperties *properties =
            (VkPhysicalDevicePointClippingProperties*)ext;
         properties->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *props =
            (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)ext;
         if (pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR) != 0)
            props->maxVertexAttribDivisor = UINT32_MAX;
         else
            props->maxVertexAttribDivisor = 1;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT: {
         VkPhysicalDeviceTransformFeedbackPropertiesEXT *properties =
            (VkPhysicalDeviceTransformFeedbackPropertiesEXT*)ext;
         properties->maxTransformFeedbackStreams = pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_MAX_VERTEX_STREAMS);
         properties->maxTransformFeedbackBuffers = pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS);
         properties->maxTransformFeedbackBufferSize = UINT32_MAX;
         properties->maxTransformFeedbackStreamDataSize = 512;
         properties->maxTransformFeedbackBufferDataSize = 512;
         properties->maxTransformFeedbackBufferDataStride = 512;
         properties->transformFeedbackQueries = true;
         properties->transformFeedbackStreamsLinesTriangles = false;
         properties->transformFeedbackRasterizationStreamSelect = false;
         properties->transformFeedbackDraw = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES: {
         VkPhysicalDeviceMaintenance4Properties *properties =
            (VkPhysicalDeviceMaintenance4Properties *)ext;
         properties->maxBufferSize = UINT32_MAX;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *properties =
            (VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *)ext;
         properties->dynamicPrimitiveTopologyUnrestricted = VK_TRUE;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT: {
         VkPhysicalDeviceLineRasterizationPropertiesEXT *properties =
            (VkPhysicalDeviceLineRasterizationPropertiesEXT *)ext;
         properties->lineSubPixelPrecisionBits =
            pdevice->pscreen->get_param(pdevice->pscreen,
                                        PIPE_CAP_RASTERIZER_SUBPIXEL_BITS);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES: {
         VkPhysicalDeviceInlineUniformBlockProperties *properties =
            (VkPhysicalDeviceInlineUniformBlockProperties *)ext;
         properties->maxInlineUniformBlockSize = MAX_DESCRIPTOR_UNIFORM_BLOCK_SIZE;
         properties->maxPerStageDescriptorInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
         properties->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
         properties->maxDescriptorSetInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
         properties->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT: {
         VkPhysicalDeviceExternalMemoryHostPropertiesEXT *properties =
            (VkPhysicalDeviceExternalMemoryHostPropertiesEXT *)ext;
         properties->minImportedHostPointerAlignment = 4096;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT: {
         VkPhysicalDeviceCustomBorderColorPropertiesEXT *properties =
            (VkPhysicalDeviceCustomBorderColorPropertiesEXT *)ext;
         properties->maxCustomBorderColorSamplers = 32 * 1024;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES: {
         VkPhysicalDeviceSubgroupSizeControlProperties *props = (VkPhysicalDeviceSubgroupSizeControlProperties *)ext;
         props->minSubgroupSize = lp_native_vector_width / 32;
         props->maxSubgroupSize = lp_native_vector_width / 32;
         props->maxComputeWorkgroupSubgroups = 32;
         props->requiredSubgroupSizeStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT: {
         VkPhysicalDeviceProvokingVertexPropertiesEXT *properties =
            (VkPhysicalDeviceProvokingVertexPropertiesEXT*)ext;
         properties->provokingVertexModePerPipeline = true;
         properties->transformFeedbackPreservesTriangleFanProvokingVertex = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT: {
         VkPhysicalDeviceMultiDrawPropertiesEXT *props = (VkPhysicalDeviceMultiDrawPropertiesEXT *)ext;
         props->maxMultiDrawCount = 2048;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES: {
         VkPhysicalDeviceTexelBufferAlignmentProperties *properties =
            (VkPhysicalDeviceTexelBufferAlignmentProperties *)ext;
         int alignment = pdevice->pscreen->get_param(pdevice->pscreen, PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT);
         properties->storageTexelBufferOffsetAlignmentBytes = alignment;
         properties->storageTexelBufferOffsetSingleTexelAlignment = true;
         properties->uniformTexelBufferOffsetAlignmentBytes = alignment;
         properties->uniformTexelBufferOffsetSingleTexelAlignment = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT: {
         VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *props = (VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *)ext;
         props->graphicsPipelineLibraryFastLinking = VK_TRUE;
         props->graphicsPipelineLibraryIndependentInterpolationDecoration = VK_TRUE;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT: {
         VkPhysicalDeviceRobustness2PropertiesEXT *props =
            (VkPhysicalDeviceRobustness2PropertiesEXT *)ext;
         props->robustStorageBufferAccessSizeAlignment = 1;
         props->robustUniformBufferAccessSizeAlignment = 1;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice                            physicalDevice,
   uint32_t*                                   pCount,
   VkQueueFamilyProperties2                   *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pQueueFamilyProperties, pCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties) {
         .queueFlags = VK_QUEUE_GRAPHICS_BIT |
         VK_QUEUE_COMPUTE_BIT |
         VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .timestampValidBits = 64,
         .minImageTransferGranularity = (VkExtent3D) { 1, 1, 1 },
      };
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceMemoryProperties*           pMemoryProperties)
{
   pMemoryProperties->memoryTypeCount = 1;
   pMemoryProperties->memoryTypes[0] = (VkMemoryType) {
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      .heapIndex = 0,
   };

   pMemoryProperties->memoryHeapCount = 1;
   pMemoryProperties->memoryHeaps[0] = (VkMemoryHeap) {
      .size = 2ULL*1024*1024*1024,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceMemoryProperties2          *pMemoryProperties)
{
   lvp_GetPhysicalDeviceMemoryProperties(physicalDevice,
                                         &pMemoryProperties->memoryProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
lvp_GetMemoryHostPointerPropertiesEXT(
   VkDevice _device,
   VkExternalMemoryHandleTypeFlagBits handleType,
   const void *pHostPointer,
   VkMemoryHostPointerPropertiesEXT *pMemoryHostPointerProperties)
{
   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT: {
      pMemoryHostPointerProperties->memoryTypeBits = 1;
      return VK_SUCCESS;
   }
   default:
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lvp_GetInstanceProcAddr(
   VkInstance                                  _instance,
   const char*                                 pName)
{
   LVP_FROM_HANDLE(lvp_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk,
                                    &lvp_instance_entrypoints,
                                    pName);
}

/* Windows will use a dll definition file to avoid build errors. */
#ifdef _WIN32
#undef PUBLIC
#define PUBLIC
#endif

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
   VkInstance                                  instance,
   const char*                                 pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
   VkInstance                                  instance,
   const char*                                 pName)
{
   return lvp_GetInstanceProcAddr(instance, pName);
}

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
   VkInstance                                  _instance,
   const char*                                 pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
   VkInstance                                  _instance,
   const char*                                 pName)
{
   LVP_FROM_HANDLE(lvp_instance, instance, _instance);
   return vk_instance_get_physical_device_proc_addr(&instance->vk, pName);
}

static void
destroy_pipelines(struct lvp_queue *queue)
{
   simple_mtx_lock(&queue->pipeline_lock);
   while (util_dynarray_contains(&queue->pipeline_destroys, struct lvp_pipeline*)) {
      lvp_pipeline_destroy(queue->device, util_dynarray_pop(&queue->pipeline_destroys, struct lvp_pipeline*));
   }
   simple_mtx_unlock(&queue->pipeline_lock);
}

static VkResult
lvp_queue_submit(struct vk_queue *vk_queue,
                 struct vk_queue_submit *submit)
{
   struct lvp_queue *queue = container_of(vk_queue, struct lvp_queue, vk);

   VkResult result = vk_sync_wait_many(&queue->device->vk,
                                       submit->wait_count, submit->waits,
                                       VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct lvp_cmd_buffer *cmd_buffer =
         container_of(submit->command_buffers[i], struct lvp_cmd_buffer, vk);

      lvp_execute_cmds(queue->device, queue, cmd_buffer);
   }

   if (submit->command_buffer_count > 0)
      queue->ctx->flush(queue->ctx, &queue->last_fence, 0);

   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct lvp_pipe_sync *sync =
         vk_sync_as_lvp_pipe_sync(submit->signals[i].sync);
      lvp_pipe_sync_signal_with_fence(queue->device, sync, queue->last_fence);
   }
   destroy_pipelines(queue);

   return VK_SUCCESS;
}

static VkResult
lvp_queue_init(struct lvp_device *device, struct lvp_queue *queue,
               const VkDeviceQueueCreateInfo *create_info,
               uint32_t index_in_family)
{
   VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info,
                                   index_in_family);
   if (result != VK_SUCCESS)
      return result;

   result = vk_queue_enable_submit_thread(&queue->vk);
   if (result != VK_SUCCESS) {
      vk_queue_finish(&queue->vk);
      return result;
   }

   queue->device = device;

   queue->ctx = device->pscreen->context_create(device->pscreen, NULL, PIPE_CONTEXT_ROBUST_BUFFER_ACCESS);
   queue->cso = cso_create_context(queue->ctx, CSO_NO_VBUF);
   queue->uploader = u_upload_create(queue->ctx, 1024 * 1024, PIPE_BIND_CONSTANT_BUFFER, PIPE_USAGE_STREAM, 0);

   queue->vk.driver_submit = lvp_queue_submit;

   simple_mtx_init(&queue->pipeline_lock, mtx_plain);
   util_dynarray_init(&queue->pipeline_destroys, NULL);

   return VK_SUCCESS;
}

static void
lvp_queue_finish(struct lvp_queue *queue)
{
   vk_queue_finish(&queue->vk);

   destroy_pipelines(queue);
   simple_mtx_destroy(&queue->pipeline_lock);
   util_dynarray_fini(&queue->pipeline_destroys);

   u_upload_destroy(queue->uploader);
   cso_destroy_context(queue->cso);
   queue->ctx->destroy(queue->ctx);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateDevice(
   VkPhysicalDevice                            physicalDevice,
   const VkDeviceCreateInfo*                   pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkDevice*                                   pDevice)
{
   LVP_FROM_HANDLE(lvp_physical_device, physical_device, physicalDevice);
   struct lvp_device *device;
   struct lvp_instance *instance = (struct lvp_instance *)physical_device->vk.instance;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   size_t state_size = lvp_get_rendering_state_size();
   device = vk_zalloc2(&physical_device->vk.instance->alloc, pAllocator,
                       sizeof(*device) + state_size, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->queue.state = device + 1;
   device->poison_mem = debug_get_bool_option("LVP_POISON_MEMORY", false);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      &lvp_device_entrypoints, true);
   lvp_add_enqueue_cmd_entrypoints(&dispatch_table);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      &wsi_device_entrypoints, false);
   VkResult result = vk_device_init(&device->vk,
                                    &physical_device->vk,
                                    &dispatch_table, pCreateInfo,
                                    pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   vk_device_enable_threaded_submit(&device->vk);
   device->vk.command_buffer_ops = &lvp_cmd_buffer_ops;

   device->instance = (struct lvp_instance *)physical_device->vk.instance;
   device->physical_device = physical_device;

   device->pscreen = physical_device->pscreen;

   assert(pCreateInfo->queueCreateInfoCount == 1);
   assert(pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex == 0);
   assert(pCreateInfo->pQueueCreateInfos[0].queueCount == 1);
   result = lvp_queue_init(device, &device->queue, pCreateInfo->pQueueCreateInfos, 0);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   *pDevice = lvp_device_to_handle(device);

   return VK_SUCCESS;

}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyDevice(
   VkDevice                                    _device,
   const VkAllocationCallbacks*                pAllocator)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);

   if (device->queue.last_fence)
      device->pscreen->fence_reference(device->pscreen, &device->queue.last_fence, NULL);
   lvp_queue_finish(&device->queue);
   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateInstanceExtensionProperties(
   const char*                                 pLayerName,
   uint32_t*                                   pPropertyCount,
   VkExtensionProperties*                      pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &lvp_instance_extensions_supported, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateInstanceLayerProperties(
   uint32_t*                                   pPropertyCount,
   VkLayerProperties*                          pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateDeviceLayerProperties(
   VkPhysicalDevice                            physicalDevice,
   uint32_t*                                   pPropertyCount,
   VkLayerProperties*                          pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_AllocateMemory(
   VkDevice                                    _device,
   const VkMemoryAllocateInfo*                 pAllocateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkDeviceMemory*                             pMem)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   struct lvp_device_memory *mem;
   ASSERTED const VkExportMemoryAllocateInfo *export_info = NULL;
   ASSERTED const VkImportMemoryFdInfoKHR *import_info = NULL;
   const VkImportMemoryHostPointerInfoEXT *host_ptr_info = NULL;
   VkResult error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   if (pAllocateInfo->allocationSize == 0) {
      /* Apparently, this is allowed */
      *pMem = VK_NULL_HANDLE;
      return VK_SUCCESS;
   }

   vk_foreach_struct_const(ext, pAllocateInfo->pNext) {
      switch ((unsigned)ext->sType) {
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
         host_ptr_info = (VkImportMemoryHostPointerInfoEXT*)ext;
         assert(host_ptr_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
         break;
      case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
         export_info = (VkExportMemoryAllocateInfo*)ext;
         assert(!export_info->handleTypes || export_info->handleTypes == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
         break;
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
         import_info = (VkImportMemoryFdInfoKHR*)ext;
         assert(import_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
         break;
      default:
         break;
      }
   }

#ifdef PIPE_MEMORY_FD
   if (import_info != NULL && import_info->fd < 0) {
      return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
#endif

   mem = vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*mem), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &mem->base,
                       VK_OBJECT_TYPE_DEVICE_MEMORY);

   mem->memory_type = LVP_DEVICE_MEMORY_TYPE_DEFAULT;
   mem->backed_fd = -1;

   if (host_ptr_info) {
      mem->pmem = host_ptr_info->pHostPointer;
      mem->memory_type = LVP_DEVICE_MEMORY_TYPE_USER_PTR;
   }
#ifdef PIPE_MEMORY_FD
   else if(import_info) {
      uint64_t size;
      if(!device->pscreen->import_memory_fd(device->pscreen, import_info->fd, &mem->pmem, &size)) {
         close(import_info->fd);
         error = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto fail;
      }
      if(size < pAllocateInfo->allocationSize) {
         device->pscreen->free_memory_fd(device->pscreen, mem->pmem);
         close(import_info->fd);
         goto fail;
      }
      if (export_info && export_info->handleTypes) {
         mem->backed_fd = import_info->fd;
      }
      else {
         close(import_info->fd);
      }
      mem->memory_type = LVP_DEVICE_MEMORY_TYPE_OPAQUE_FD;
   }
   else if (export_info && export_info->handleTypes) {
      mem->pmem = device->pscreen->allocate_memory_fd(device->pscreen, pAllocateInfo->allocationSize, &mem->backed_fd);
      if (!mem->pmem || mem->backed_fd < 0) {
         goto fail;
      }
      mem->memory_type = LVP_DEVICE_MEMORY_TYPE_OPAQUE_FD;
   }
#endif
   else {
      mem->pmem = device->pscreen->allocate_memory(device->pscreen, pAllocateInfo->allocationSize);
      if (!mem->pmem) {
         goto fail;
      }
      if (device->poison_mem)
         /* this is a value that will definitely break things */
         memset(mem->pmem, UINT8_MAX / 2 + 1, pAllocateInfo->allocationSize);
   }

   mem->type_index = pAllocateInfo->memoryTypeIndex;

   *pMem = lvp_device_memory_to_handle(mem);

   return VK_SUCCESS;

fail:
   vk_free2(&device->vk.alloc, pAllocator, mem);
   return vk_error(device, error);
}

VKAPI_ATTR void VKAPI_CALL lvp_FreeMemory(
   VkDevice                                    _device,
   VkDeviceMemory                              _mem,
   const VkAllocationCallbacks*                pAllocator)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   LVP_FROM_HANDLE(lvp_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   switch(mem->memory_type) {
   case LVP_DEVICE_MEMORY_TYPE_DEFAULT:
      device->pscreen->free_memory(device->pscreen, mem->pmem);
      break;
#ifdef PIPE_MEMORY_FD
   case LVP_DEVICE_MEMORY_TYPE_OPAQUE_FD:
      device->pscreen->free_memory_fd(device->pscreen, mem->pmem);
      if(mem->backed_fd >= 0)
         close(mem->backed_fd);
      break;
#endif
   case LVP_DEVICE_MEMORY_TYPE_USER_PTR:
   default:
      break;
   }
   vk_object_base_finish(&mem->base);
   vk_free2(&device->vk.alloc, pAllocator, mem);

}

VKAPI_ATTR VkResult VKAPI_CALL lvp_MapMemory(
   VkDevice                                    _device,
   VkDeviceMemory                              _memory,
   VkDeviceSize                                offset,
   VkDeviceSize                                size,
   VkMemoryMapFlags                            flags,
   void**                                      ppData)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   LVP_FROM_HANDLE(lvp_device_memory, mem, _memory);
   void *map;
   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   map = device->pscreen->map_memory(device->pscreen, mem->pmem);

   *ppData = (char *)map + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_UnmapMemory(
   VkDevice                                    _device,
   VkDeviceMemory                              _memory)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   LVP_FROM_HANDLE(lvp_device_memory, mem, _memory);

   if (mem == NULL)
      return;

   device->pscreen->unmap_memory(device->pscreen, mem->pmem);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_FlushMappedMemoryRanges(
   VkDevice                                    _device,
   uint32_t                                    memoryRangeCount,
   const VkMappedMemoryRange*                  pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_InvalidateMappedMemoryRanges(
   VkDevice                                    _device,
   uint32_t                                    memoryRangeCount,
   const VkMappedMemoryRange*                  pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceBufferMemoryRequirements(
    VkDevice                                    _device,
    const VkDeviceBufferMemoryRequirements*     pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = 64;
   pMemoryRequirements->memoryRequirements.size = 0;

   VkBuffer _buffer;
   if (lvp_CreateBuffer(_device, pInfo->pCreateInfo, NULL, &_buffer) != VK_SUCCESS)
      return;
   LVP_FROM_HANDLE(lvp_buffer, buffer, _buffer);
   pMemoryRequirements->memoryRequirements.size = buffer->total_size;
   lvp_DestroyBuffer(_device, _buffer, NULL);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceImageSparseMemoryRequirements(
    VkDevice                                    device,
    const VkDeviceImageMemoryRequirements*      pInfo,
    uint32_t*                                   pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2*           pSparseMemoryRequirements)
{
   stub();
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceImageMemoryRequirements(
    VkDevice                                    _device,
    const VkDeviceImageMemoryRequirements*     pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = 0;
   pMemoryRequirements->memoryRequirements.size = 0;

   VkImage _image;
   if (lvp_CreateImage(_device, pInfo->pCreateInfo, NULL, &_image) != VK_SUCCESS)
      return;
   LVP_FROM_HANDLE(lvp_image, image, _image);
   pMemoryRequirements->memoryRequirements.size = image->size;
   pMemoryRequirements->memoryRequirements.alignment = image->alignment;
   lvp_DestroyImage(_device, _image, NULL);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetBufferMemoryRequirements(
   VkDevice                                    device,
   VkBuffer                                    _buffer,
   VkMemoryRequirements*                       pMemoryRequirements)
{
   LVP_FROM_HANDLE(lvp_buffer, buffer, _buffer);

   /* The Vulkan spec (git aaed022) says:
    *
    *    memoryTypeBits is a bitfield and contains one bit set for every
    *    supported memory type for the resource. The bit `1<<i` is set if and
    *    only if the memory type `i` in the VkPhysicalDeviceMemoryProperties
    *    structure for the physical device is supported.
    *
    * We support exactly one memory type.
    */
   pMemoryRequirements->memoryTypeBits = 1;

   pMemoryRequirements->size = buffer->total_size;
   pMemoryRequirements->alignment = 64;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetBufferMemoryRequirements2(
   VkDevice                                     device,
   const VkBufferMemoryRequirementsInfo2       *pInfo,
   VkMemoryRequirements2                       *pMemoryRequirements)
{
   lvp_GetBufferMemoryRequirements(device, pInfo->buffer,
                                   &pMemoryRequirements->memoryRequirements);
   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *req =
            (VkMemoryDedicatedRequirements *) ext;
         req->requiresDedicatedAllocation = false;
         req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetImageMemoryRequirements(
   VkDevice                                    device,
   VkImage                                     _image,
   VkMemoryRequirements*                       pMemoryRequirements)
{
   LVP_FROM_HANDLE(lvp_image, image, _image);
   pMemoryRequirements->memoryTypeBits = 1;

   pMemoryRequirements->size = image->size;
   pMemoryRequirements->alignment = image->alignment;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetImageMemoryRequirements2(
   VkDevice                                    device,
   const VkImageMemoryRequirementsInfo2       *pInfo,
   VkMemoryRequirements2                      *pMemoryRequirements)
{
   lvp_GetImageMemoryRequirements(device, pInfo->image,
                                  &pMemoryRequirements->memoryRequirements);

   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *req =
            (VkMemoryDedicatedRequirements *) ext;
         req->requiresDedicatedAllocation = false;
         req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetImageSparseMemoryRequirements(
   VkDevice                                    device,
   VkImage                                     image,
   uint32_t*                                   pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements*            pSparseMemoryRequirements)
{
   stub();
}

VKAPI_ATTR void VKAPI_CALL lvp_GetImageSparseMemoryRequirements2(
   VkDevice                                    device,
   const VkImageSparseMemoryRequirementsInfo2* pInfo,
   uint32_t* pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2* pSparseMemoryRequirements)
{
   stub();
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceMemoryCommitment(
   VkDevice                                    device,
   VkDeviceMemory                              memory,
   VkDeviceSize*                               pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_BindBufferMemory2(VkDevice _device,
                               uint32_t bindInfoCount,
                               const VkBindBufferMemoryInfo *pBindInfos)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      LVP_FROM_HANDLE(lvp_device_memory, mem, pBindInfos[i].memory);
      LVP_FROM_HANDLE(lvp_buffer, buffer, pBindInfos[i].buffer);

      buffer->pmem = mem->pmem;
      buffer->offset = pBindInfos[i].memoryOffset;
      device->pscreen->resource_bind_backing(device->pscreen,
                                             buffer->bo,
                                             mem->pmem,
                                             pBindInfos[i].memoryOffset);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_BindImageMemory2(VkDevice _device,
                              uint32_t bindInfoCount,
                              const VkBindImageMemoryInfo *pBindInfos)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      const VkBindImageMemoryInfo *bind_info = &pBindInfos[i];
      LVP_FROM_HANDLE(lvp_device_memory, mem, bind_info->memory);
      LVP_FROM_HANDLE(lvp_image, image, bind_info->image);
      bool did_bind = false;

      vk_foreach_struct_const(s, bind_info->pNext) {
         switch (s->sType) {
         case VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR: {
            const VkBindImageMemorySwapchainInfoKHR *swapchain_info =
               (const VkBindImageMemorySwapchainInfoKHR *) s;
            struct lvp_image *swapchain_image =
               lvp_swapchain_get_image(swapchain_info->swapchain,
                                       swapchain_info->imageIndex);

            image->pmem = swapchain_image->pmem;
            image->memory_offset = swapchain_image->memory_offset;
            device->pscreen->resource_bind_backing(device->pscreen,
                                                   image->bo,
                                                   image->pmem,
                                                   image->memory_offset);
            did_bind = true;
            break;
         }
         default:
            break;
         }
      }

      if (!did_bind) {
         if (!device->pscreen->resource_bind_backing(device->pscreen,
                                                     image->bo,
                                                     mem->pmem,
                                                     bind_info->memoryOffset)) {
            /* This is probably caused by the texture being too large, so let's
             * report this as the *closest* allowed error-code. It's not ideal,
             * but it's unlikely that anyone will care too much.
             */
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         }
         image->pmem = mem->pmem;
         image->memory_offset = bind_info->memoryOffset;
      }
   }
   return VK_SUCCESS;
}

#ifdef PIPE_MEMORY_FD

VkResult
lvp_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFD)
{
   LVP_FROM_HANDLE(lvp_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);
   assert(pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);

   *pFD = dup(memory->backed_fd);
   assert(*pFD >= 0);
   return VK_SUCCESS;
}

VkResult
lvp_GetMemoryFdPropertiesKHR(VkDevice _device,
                             VkExternalMemoryHandleTypeFlagBits handleType,
                             int fd,
                             VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);

   assert(pMemoryFdProperties->sType == VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR);

   if(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
      // There is only one memoryType so select this one
      pMemoryFdProperties->memoryTypeBits = 1;
   }
   else
      return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   return VK_SUCCESS;
}

#endif

VKAPI_ATTR VkResult VKAPI_CALL lvp_QueueBindSparse(
   VkQueue                                     queue,
   uint32_t                                    bindInfoCount,
   const VkBindSparseInfo*                     pBindInfo,
   VkFence                                     fence)
{
   stub_return(VK_ERROR_INCOMPATIBLE_DRIVER);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateEvent(
   VkDevice                                    _device,
   const VkEventCreateInfo*                    pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkEvent*                                    pEvent)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   struct lvp_event *event = vk_alloc2(&device->vk.alloc, pAllocator,
                                       sizeof(*event), 8,
                                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!event)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &event->base, VK_OBJECT_TYPE_EVENT);
   *pEvent = lvp_event_to_handle(event);
   event->event_storage = 0;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyEvent(
   VkDevice                                    _device,
   VkEvent                                     _event,
   const VkAllocationCallbacks*                pAllocator)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   LVP_FROM_HANDLE(lvp_event, event, _event);

   if (!event)
      return;

   vk_object_base_finish(&event->base);
   vk_free2(&device->vk.alloc, pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_GetEventStatus(
   VkDevice                                    _device,
   VkEvent                                     _event)
{
   LVP_FROM_HANDLE(lvp_event, event, _event);
   if (event->event_storage == 1)
      return VK_EVENT_SET;
   return VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_SetEvent(
   VkDevice                                    _device,
   VkEvent                                     _event)
{
   LVP_FROM_HANDLE(lvp_event, event, _event);
   event->event_storage = 1;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_ResetEvent(
   VkDevice                                    _device,
   VkEvent                                     _event)
{
   LVP_FROM_HANDLE(lvp_event, event, _event);
   event->event_storage = 0;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateSampler(
   VkDevice                                    _device,
   const VkSamplerCreateInfo*                  pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkSampler*                                  pSampler)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   struct lvp_sampler *sampler;
   const VkSamplerReductionModeCreateInfo *reduction_mode_create_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           SAMPLER_REDUCTION_MODE_CREATE_INFO);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*sampler), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &sampler->base,
                       VK_OBJECT_TYPE_SAMPLER);

   VkClearColorValue border_color =
      vk_sampler_border_color_value(pCreateInfo, NULL);
   STATIC_ASSERT(sizeof(sampler->state.border_color) == sizeof(border_color));

   sampler->state.wrap_s = vk_conv_wrap_mode(pCreateInfo->addressModeU);
   sampler->state.wrap_t = vk_conv_wrap_mode(pCreateInfo->addressModeV);
   sampler->state.wrap_r = vk_conv_wrap_mode(pCreateInfo->addressModeW);
   sampler->state.min_img_filter = pCreateInfo->minFilter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   sampler->state.min_mip_filter = pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;
   sampler->state.mag_img_filter = pCreateInfo->magFilter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   sampler->state.min_lod = pCreateInfo->minLod;
   sampler->state.max_lod = pCreateInfo->maxLod;
   sampler->state.lod_bias = pCreateInfo->mipLodBias;
   if (pCreateInfo->anisotropyEnable)
      sampler->state.max_anisotropy = pCreateInfo->maxAnisotropy;
   else
      sampler->state.max_anisotropy = 1;
   sampler->state.unnormalized_coords = pCreateInfo->unnormalizedCoordinates;
   sampler->state.compare_mode = pCreateInfo->compareEnable ? PIPE_TEX_COMPARE_R_TO_TEXTURE : PIPE_TEX_COMPARE_NONE;
   sampler->state.compare_func = pCreateInfo->compareOp;
   sampler->state.seamless_cube_map = !(pCreateInfo->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT);
   STATIC_ASSERT((unsigned)VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE == (unsigned)PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE);
   STATIC_ASSERT((unsigned)VK_SAMPLER_REDUCTION_MODE_MIN == (unsigned)PIPE_TEX_REDUCTION_MIN);
   STATIC_ASSERT((unsigned)VK_SAMPLER_REDUCTION_MODE_MAX == (unsigned)PIPE_TEX_REDUCTION_MAX);
   if (reduction_mode_create_info)
      sampler->state.reduction_mode = (enum pipe_tex_reduction_mode)reduction_mode_create_info->reductionMode;
   else
      sampler->state.reduction_mode = PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE;
   memcpy(&sampler->state.border_color, &border_color, sizeof(border_color));

   *pSampler = lvp_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroySampler(
   VkDevice                                    _device,
   VkSampler                                   _sampler,
   const VkAllocationCallbacks*                pAllocator)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   LVP_FROM_HANDLE(lvp_sampler, sampler, _sampler);

   if (!_sampler)
      return;
   vk_object_base_finish(&sampler->base);
   vk_free2(&device->vk.alloc, pAllocator, sampler);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateSamplerYcbcrConversionKHR(
    VkDevice                                    device,
    const VkSamplerYcbcrConversionCreateInfo*   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSamplerYcbcrConversion*                   pYcbcrConversion)
{
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroySamplerYcbcrConversionKHR(
    VkDevice                                    device,
    VkSamplerYcbcrConversion                    ycbcrConversion,
    const VkAllocationCallbacks*                pAllocator)
{
}

/* vk_icd.h does not declare this function, so we declare it here to
 * suppress Wmissing-prototypes.
 */
PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);

PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion)
{
   /* For the full details on loader interface versioning, see
    * <https://github.com/KhronosGroup/Vulkan-LoaderAndValidationLayers/blob/master/loader/LoaderAndLayerInterface.md>.
    * What follows is a condensed summary, to help you navigate the large and
    * confusing official doc.
    *
    *   - Loader interface v0 is incompatible with later versions. We don't
    *     support it.
    *
    *   - In loader interface v1:
    *       - The first ICD entrypoint called by the loader is
    *         vk_icdGetInstanceProcAddr(). The ICD must statically expose this
    *         entrypoint.
    *       - The ICD must statically expose no other Vulkan symbol unless it is
    *         linked with -Bsymbolic.
    *       - Each dispatchable Vulkan handle created by the ICD must be
    *         a pointer to a struct whose first member is VK_LOADER_DATA. The
    *         ICD must initialize VK_LOADER_DATA.loadMagic to ICD_LOADER_MAGIC.
    *       - The loader implements vkCreate{PLATFORM}SurfaceKHR() and
    *         vkDestroySurfaceKHR(). The ICD must be capable of working with
    *         such loader-managed surfaces.
    *
    *    - Loader interface v2 differs from v1 in:
    *       - The first ICD entrypoint called by the loader is
    *         vk_icdNegotiateLoaderICDInterfaceVersion(). The ICD must
    *         statically expose this entrypoint.
    *
    *    - Loader interface v3 differs from v2 in:
    *        - The ICD must implement vkCreate{PLATFORM}SurfaceKHR(),
    *          vkDestroySurfaceKHR(), and other API which uses VKSurfaceKHR,
    *          because the loader no longer does so.
    *
    *    - Loader interface v4 differs from v3 in:
    *        - The ICD must implement vk_icdGetPhysicalDeviceProcAddr().
    * 
    *    - Loader interface v5 differs from v4 in:
    *        - The ICD must support Vulkan API version 1.1 and must not return 
    *          VK_ERROR_INCOMPATIBLE_DRIVER from vkCreateInstance() unless a
    *          Vulkan Loader with interface v4 or smaller is being used and the
    *          application provides an API version that is greater than 1.0.
    */
   *pSupportedVersion = MIN2(*pSupportedVersion, 5u);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreatePrivateDataSlotEXT(
   VkDevice                                    _device,
   const VkPrivateDataSlotCreateInfo*          pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkPrivateDataSlot*                          pPrivateDataSlot)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   return vk_private_data_slot_create(&device->vk, pCreateInfo, pAllocator,
                                      pPrivateDataSlot);
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyPrivateDataSlotEXT(
   VkDevice                                    _device,
   VkPrivateDataSlot                           privateDataSlot,
   const VkAllocationCallbacks*                pAllocator)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   vk_private_data_slot_destroy(&device->vk, privateDataSlot, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_SetPrivateDataEXT(
   VkDevice                                    _device,
   VkObjectType                                objectType,
   uint64_t                                    objectHandle,
   VkPrivateDataSlot                           privateDataSlot,
   uint64_t                                    data)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   return vk_object_base_set_private_data(&device->vk, objectType,
                                          objectHandle, privateDataSlot,
                                          data);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPrivateDataEXT(
   VkDevice                                    _device,
   VkObjectType                                objectType,
   uint64_t                                    objectHandle,
   VkPrivateDataSlot                           privateDataSlot,
   uint64_t*                                   pData)
{
   LVP_FROM_HANDLE(lvp_device, device, _device);
   vk_object_base_get_private_data(&device->vk, objectType, objectHandle,
                                   privateDataSlot, pData);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceExternalFenceProperties(
   VkPhysicalDevice                           physicalDevice,
   const VkPhysicalDeviceExternalFenceInfo    *pExternalFenceInfo,
   VkExternalFenceProperties                  *pExternalFenceProperties)
{
   pExternalFenceProperties->exportFromImportedHandleTypes = 0;
   pExternalFenceProperties->compatibleHandleTypes = 0;
   pExternalFenceProperties->externalFenceFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice                            physicalDevice,
   const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
   VkExternalSemaphoreProperties               *pExternalSemaphoreProperties)
{
   pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
   pExternalSemaphoreProperties->compatibleHandleTypes = 0;
   pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
}

static const VkTimeDomainEXT lvp_time_domains[] = {
        VK_TIME_DOMAIN_DEVICE_EXT,
        VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
};

VKAPI_ATTR VkResult VKAPI_CALL lvp_GetPhysicalDeviceCalibrateableTimeDomainsEXT(
   VkPhysicalDevice physicalDevice,
   uint32_t *pTimeDomainCount,
   VkTimeDomainEXT *pTimeDomains)
{
   int d;
   VK_OUTARRAY_MAKE_TYPED(VkTimeDomainEXT, out, pTimeDomains,
                          pTimeDomainCount);

   for (d = 0; d < ARRAY_SIZE(lvp_time_domains); d++) {
      vk_outarray_append_typed(VkTimeDomainEXT, &out, i) {
         *i = lvp_time_domains[d];
      }
    }

    return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_GetCalibratedTimestampsEXT(
   VkDevice device,
   uint32_t timestampCount,
   const VkCalibratedTimestampInfoEXT *pTimestampInfos,
   uint64_t *pTimestamps,
   uint64_t *pMaxDeviation)
{
   *pMaxDeviation = 1;

   uint64_t now = os_time_get_nano();
   for (unsigned i = 0; i < timestampCount; i++) {
      pTimestamps[i] = now;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceGroupPeerMemoryFeaturesKHR(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   *pPeerMemoryFeatures = 0;
}
