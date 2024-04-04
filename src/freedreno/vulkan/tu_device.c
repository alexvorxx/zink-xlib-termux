/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 */

#include "tu_device.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/sysinfo.h>

#include "git_sha1.h"
#include "util/u_debug.h"
#include "util/disk_cache.h"
#include "util/driconf.h"
#include "util/os_misc.h"
#include "vk_shader_module.h"
#include "vk_sampler.h"
#include "vk_util.h"

/* for fd_get_driver/device_uuid() */
#include "freedreno/common/freedreno_uuid.h"

#include "tu_clear_blit.h"
#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_descriptor_set.h"
#include "tu_dynamic_rendering.h"
#include "tu_image.h"
#include "tu_pass.h"
#include "tu_query.h"
#include "tu_tracepoints.h"
#include "tu_wsi.h"

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
     defined(VK_USE_PLATFORM_XCB_KHR) || \
     defined(VK_USE_PLATFORM_XLIB_KHR) || \
     defined(VK_USE_PLATFORM_DISPLAY_KHR)
#define TU_HAS_SURFACE 1
#else
#define TU_HAS_SURFACE 0
#endif


static int
tu_device_get_cache_uuid(struct tu_physical_device *device, void *uuid)
{
   struct mesa_sha1 ctx;
   unsigned char sha1[20];
   /* Note: IR3_SHADER_DEBUG also affects compilation, but it's not
    * initialized until after compiler creation so we have to add it to the
    * shader hash instead, since the compiler is only created with the logical
    * device.
    */
   uint64_t driver_flags = device->instance->debug_flags & TU_DEBUG_NOMULTIPOS;
   uint16_t family = fd_dev_gpu_id(&device->dev_id);

   memset(uuid, 0, VK_UUID_SIZE);
   _mesa_sha1_init(&ctx);

   if (!disk_cache_get_function_identifier(tu_device_get_cache_uuid, &ctx))
      return -1;

   _mesa_sha1_update(&ctx, &family, sizeof(family));
   _mesa_sha1_update(&ctx, &driver_flags, sizeof(driver_flags));
   _mesa_sha1_final(&ctx, sha1);

   memcpy(uuid, sha1, VK_UUID_SIZE);
   return 0;
}

#define TU_API_VERSION VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)

VKAPI_ATTR VkResult VKAPI_CALL
tu_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
    *pApiVersion = TU_API_VERSION;
    return VK_SUCCESS;
}

static const struct vk_instance_extension_table tu_instance_extensions_supported = {
   .KHR_device_group_creation           = true,
   .KHR_external_fence_capabilities     = true,
   .KHR_external_memory_capabilities    = true,
   .KHR_external_semaphore_capabilities = true,
   .KHR_get_physical_device_properties2 = true,
   .KHR_surface                         = TU_HAS_SURFACE,
   .KHR_get_surface_capabilities2       = TU_HAS_SURFACE,
   .EXT_swapchain_colorspace            = TU_HAS_SURFACE,
   .EXT_debug_report                    = true,
   .EXT_debug_utils                     = true,
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface                 = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface                     = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface                    = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
   .EXT_acquire_xlib_display            = true,
#endif
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .KHR_display                         = true,
   .KHR_get_display_properties2         = true,
   .EXT_direct_mode_display             = true,
   .EXT_display_surface_counter         = true,
   .EXT_acquire_drm_display             = true,
#endif
};

static void
get_device_extensions(const struct tu_physical_device *device,
                      struct vk_device_extension_table *ext)
{
   *ext = (struct vk_device_extension_table) {
      .KHR_16bit_storage = device->info->a6xx.storage_16bit,
      .KHR_bind_memory2 = true,
      .KHR_copy_commands2 = true,
      .KHR_create_renderpass2 = true,
      .KHR_dedicated_allocation = true,
      .KHR_depth_stencil_resolve = true,
      .KHR_descriptor_update_template = true,
      .KHR_device_group = true,
      .KHR_draw_indirect_count = true,
      .KHR_external_fence = true,
      .KHR_external_fence_fd = true,
      .KHR_external_memory = true,
      .KHR_external_memory_fd = true,
      .KHR_external_semaphore = true,
      .KHR_external_semaphore_fd = true,
      .KHR_format_feature_flags2 = true,
      .KHR_get_memory_requirements2 = true,
      .KHR_global_priority = true,
      .KHR_imageless_framebuffer = true,
      .KHR_incremental_present = TU_HAS_SURFACE,
      .KHR_image_format_list = true,
      .KHR_maintenance1 = true,
      .KHR_maintenance2 = true,
      .KHR_maintenance3 = true,
      .KHR_maintenance4 = true,
      .KHR_multiview = true,
      .KHR_performance_query = device->instance->debug_flags & TU_DEBUG_PERFC,
      .KHR_pipeline_executable_properties = true,
      .KHR_push_descriptor = true,
      .KHR_relaxed_block_layout = true,
      .KHR_sampler_mirror_clamp_to_edge = true,
      .KHR_sampler_ycbcr_conversion = true,
      .KHR_shader_draw_parameters = true,
      .KHR_shader_float_controls = true,
      .KHR_shader_float16_int8 = true,
      .KHR_shader_subgroup_extended_types = true,
      .KHR_shader_terminate_invocation = true,
      .KHR_spirv_1_4 = true,
      .KHR_storage_buffer_storage_class = true,
      .KHR_swapchain = TU_HAS_SURFACE,
      .KHR_swapchain_mutable_format = TU_HAS_SURFACE,
      .KHR_uniform_buffer_standard_layout = true,
      .KHR_variable_pointers = true,
      .KHR_vulkan_memory_model = true,
      .KHR_driver_properties = true,
      .KHR_separate_depth_stencil_layouts = true,
      .KHR_buffer_device_address = true,
      .KHR_shader_integer_dot_product = true,
      .KHR_zero_initialize_workgroup_memory = true,
      .KHR_shader_non_semantic_info = true,
      .KHR_synchronization2 = true,
      .KHR_dynamic_rendering = true,
      /* Hide these behind dri configs for now since we cannot implement it reliably on
       * all surfaces yet. There is no surface capability query for present wait/id,
       * but the feature is useful enough to hide behind an opt-in mechanism for now.
       * If the instance only enables surface extensions that unconditionally support present wait,
       * we can also expose the extension that way. */
      .KHR_present_id =
         driQueryOptionb(&device->instance->dri_options, "vk_khr_present_wait") ||
         wsi_common_vk_instance_supports_present_wait(&device->instance->vk),
      .KHR_present_wait =
         driQueryOptionb(&device->instance->dri_options, "vk_khr_present_wait") ||
         wsi_common_vk_instance_supports_present_wait(&device->instance->vk),
#ifndef TU_USE_KGSL
      .KHR_timeline_semaphore = true,
#endif
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
      .EXT_display_control = true,
#endif
      .EXT_external_memory_dma_buf = true,
      .EXT_image_drm_format_modifier = true,
      .EXT_sample_locations = device->info->a6xx.has_sample_locations,
      .EXT_sampler_filter_minmax = true,
      .EXT_transform_feedback = true,
      .EXT_4444_formats = true,
      .EXT_border_color_swizzle = true,
      .EXT_conditional_rendering = true,
      .EXT_custom_border_color = true,
      .EXT_depth_clip_control = true,
      .EXT_depth_clip_enable = true,
      .EXT_descriptor_indexing = true,
      .EXT_extended_dynamic_state = true,
      .EXT_extended_dynamic_state2 = true,
      .EXT_extended_dynamic_state3 = true,
      .EXT_filter_cubic = device->info->a6xx.has_tex_filter_cubic,
      .EXT_global_priority = true,
      .EXT_global_priority_query = true,
      .EXT_host_query_reset = true,
      .EXT_index_type_uint8 = true,
      .EXT_memory_budget = true,
      .EXT_primitive_topology_list_restart = true,
      .EXT_private_data = true,
      .EXT_queue_family_foreign = true,
      .EXT_robustness2 = true,
      .EXT_scalar_block_layout = true,
      .EXT_separate_stencil_usage = true,
      .EXT_shader_demote_to_helper_invocation = true,
      .EXT_shader_stencil_export = true,
      .EXT_shader_viewport_index_layer = true,
      .EXT_shader_module_identifier = true,
      .EXT_texel_buffer_alignment = true,
      .EXT_vertex_attribute_divisor = true,
      .EXT_provoking_vertex = true,
      .EXT_line_rasterization = true,
      .EXT_subgroup_size_control = true,
      .EXT_image_robustness = true,
      .EXT_primitives_generated_query = true,
      .EXT_image_view_min_lod = true,
      .EXT_pipeline_creation_feedback = true,
      .EXT_pipeline_creation_cache_control = true,
      .EXT_vertex_input_dynamic_state = true,
      .EXT_attachment_feedback_loop_layout = true,
      .EXT_rasterization_order_attachment_access = true,
      .EXT_multi_draw = true,
#ifndef TU_USE_KGSL
      .EXT_physical_device_drm = true,
#endif
      /* For Graphics Flight Recorder (GFR) */
      .AMD_buffer_marker = true,
      .ARM_rasterization_order_attachment_access = true,
#ifdef ANDROID
      .ANDROID_native_buffer = true,
#endif
      .IMG_filter_cubic = device->info->a6xx.has_tex_filter_cubic,
      .VALVE_mutable_descriptor_type = true,
      .EXT_image_2d_view_of_3d = true,
      .EXT_color_write_enable = true,
      .EXT_load_store_op_none = true,
      .EXT_non_seamless_cube_map = true,
      .EXT_tooling_info = true,
      .EXT_inline_uniform_block = true,
      .EXT_mutable_descriptor_type = true,
      .KHR_pipeline_library = true,
      .EXT_graphics_pipeline_library = true,
      .EXT_post_depth_coverage = true,
   };
}

static const struct vk_pipeline_cache_object_ops *const cache_import_ops[] = {
   &tu_shaders_ops,
   &tu_nir_shaders_ops,
   NULL,
};

VkResult
tu_physical_device_init(struct tu_physical_device *device,
                        struct tu_instance *instance)
{
   VkResult result = VK_SUCCESS;

   const char *fd_name = fd_dev_name(&device->dev_id);
   if (strncmp(fd_name, "FD", 2) == 0) {
      device->name = vk_asprintf(&instance->vk.alloc,
                                 VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE,
                                 "Turnip Adreno (TM) %s", &fd_name[2]);
   } else {
      device->name = vk_strdup(&instance->vk.alloc, fd_name,
                               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

   }
   if (!device->name) {
      return vk_startup_errorf(instance, VK_ERROR_OUT_OF_HOST_MEMORY,
                               "device name alloc fail");
   }

   const struct fd_dev_info *info = fd_dev_info(&device->dev_id);
   if (!info) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "device %s is unsupported", device->name);
      goto fail_free_name;
   }
   switch (fd_dev_gen(&device->dev_id)) {
   case 6:
      device->info = info;
      device->ccu_offset_bypass = device->info->num_ccu * A6XX_CCU_DEPTH_SIZE;
      device->ccu_offset_gmem = (device->gmem_size -
         device->info->num_ccu * A6XX_CCU_GMEM_COLOR_SIZE);
      break;
   default:
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "device %s is unsupported", device->name);
      goto fail_free_name;
   }
   if (tu_device_get_cache_uuid(device, device->cache_uuid)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "cannot generate UUID");
      goto fail_free_name;
   }

   if (device->has_set_iova) {
      mtx_init(&device->vma_mutex, mtx_plain);
      util_vma_heap_init(&device->vma, device->va_start,
                         ROUND_DOWN_TO(device->va_size, 4096));
   }

   fd_get_driver_uuid(device->driver_uuid);
   fd_get_device_uuid(device->device_uuid, &device->dev_id);

   struct vk_device_extension_table supported_extensions;
   get_device_extensions(device, &supported_extensions);

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &tu_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);

   result = vk_physical_device_init(&device->vk, &instance->vk,
                                    &supported_extensions,
                                    &dispatch_table);
   if (result != VK_SUCCESS)
      goto fail_free_vma;

   device->vk.supported_sync_types = device->sync_types;

#if TU_HAS_SURFACE
   result = tu_wsi_init(device);
   if (result != VK_SUCCESS) {
      vk_startup_errorf(instance, result, "WSI init failure");
      vk_physical_device_finish(&device->vk);
      goto fail_free_vma;
   }
#endif

   /* The gpu id is already embedded in the uuid so we just pass "tu"
    * when creating the cache.
    */
   char buf[VK_UUID_SIZE * 2 + 1];
   disk_cache_format_hex_id(buf, device->cache_uuid, VK_UUID_SIZE * 2);
   device->vk.disk_cache = disk_cache_create(device->name, buf, 0);

   device->vk.pipeline_cache_import_ops = cache_import_ops;

   return VK_SUCCESS;

fail_free_vma:
   if (device->has_set_iova)
      util_vma_heap_finish(&device->vma);
fail_free_name:
   vk_free(&instance->vk.alloc, (void *)device->name);
   return result;
}

static void
tu_physical_device_finish(struct tu_physical_device *device)
{
#if TU_HAS_SURFACE
   tu_wsi_finish(device);
#endif

   close(device->local_fd);
   if (device->master_fd != -1)
      close(device->master_fd);

   if (device->has_set_iova)
      util_vma_heap_finish(&device->vma);

   vk_free(&device->instance->vk.alloc, (void *)device->name);

   vk_physical_device_finish(&device->vk);
}

static void
tu_destroy_physical_device(struct vk_physical_device *device)
{
   tu_physical_device_finish((struct tu_physical_device *) device);
   vk_free(&device->instance->alloc, device);
}

static const struct debug_control tu_debug_options[] = {
   { "startup", TU_DEBUG_STARTUP },
   { "nir", TU_DEBUG_NIR },
   { "nobin", TU_DEBUG_NOBIN },
   { "sysmem", TU_DEBUG_SYSMEM },
   { "gmem", TU_DEBUG_GMEM },
   { "forcebin", TU_DEBUG_FORCEBIN },
   { "layout", TU_DEBUG_LAYOUT },
   { "noubwc", TU_DEBUG_NOUBWC },
   { "nomultipos", TU_DEBUG_NOMULTIPOS },
   { "nolrz", TU_DEBUG_NOLRZ },
   { "nolrzfc", TU_DEBUG_NOLRZFC },
   { "perf", TU_DEBUG_PERF },
   { "perfc", TU_DEBUG_PERFC },
   { "flushall", TU_DEBUG_FLUSHALL },
   { "syncdraw", TU_DEBUG_SYNCDRAW },
   { "dontcare_as_load", TU_DEBUG_DONT_CARE_AS_LOAD },
   { "rast_order", TU_DEBUG_RAST_ORDER },
   { "unaligned_store", TU_DEBUG_UNALIGNED_STORE },
   { "log_skip_gmem_ops", TU_DEBUG_LOG_SKIP_GMEM_OPS },
   { "dynamic", TU_DEBUG_DYNAMIC },
   { "bos", TU_DEBUG_BOS },
   { NULL, 0 }
};

const char *
tu_get_debug_option_name(int id)
{
   assert(id < ARRAY_SIZE(tu_debug_options) - 1);
   return tu_debug_options[id].string;
}

static const driOptionDescription tu_dri_options[] = {
   DRI_CONF_SECTION_PERFORMANCE
      DRI_CONF_VK_X11_OVERRIDE_MIN_IMAGE_COUNT(0)
      DRI_CONF_VK_KHR_PRESENT_WAIT(false)
      DRI_CONF_VK_X11_STRICT_IMAGE_COUNT(false)
      DRI_CONF_VK_X11_ENSURE_MIN_IMAGE_COUNT(false)
      DRI_CONF_VK_XWAYLAND_WAIT_READY(true)
   DRI_CONF_SECTION_END

   DRI_CONF_SECTION_DEBUG
      DRI_CONF_VK_WSI_FORCE_BGRA8_UNORM_FIRST(false)
      DRI_CONF_VK_DONT_CARE_AS_LOAD(false)
   DRI_CONF_SECTION_END
};

static void
tu_init_dri_options(struct tu_instance *instance)
{
   driParseOptionInfo(&instance->available_dri_options, tu_dri_options,
                      ARRAY_SIZE(tu_dri_options));
   driParseConfigFiles(&instance->dri_options, &instance->available_dri_options, 0, "turnip", NULL, NULL,
                       instance->vk.app_info.app_name, instance->vk.app_info.app_version,
                       instance->vk.app_info.engine_name, instance->vk.app_info.engine_version);

   if (driQueryOptionb(&instance->dri_options, "vk_dont_care_as_load"))
      instance->debug_flags |= TU_DEBUG_DONT_CARE_AS_LOAD;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkInstance *pInstance)
{
   struct tu_instance *instance;
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
      &dispatch_table, &tu_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk,
                             &tu_instance_extensions_supported,
                             &dispatch_table,
                             pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

#ifndef TU_USE_KGSL
   instance->vk.physical_devices.try_create_for_drm =
      tu_physical_device_try_create;
#else
   instance->vk.physical_devices.enumerate = tu_enumerate_devices;
#endif
   instance->vk.physical_devices.destroy = tu_destroy_physical_device;

   instance->debug_flags =
      parse_debug_string(os_get_option("TU_DEBUG"), tu_debug_options);

#ifdef DEBUG
   /* Enable startup debugging by default on debug drivers.  You almost always
    * want to see your startup failures in that case, and it's hard to set
    * this env var on android.
    */
   instance->debug_flags |= TU_DEBUG_STARTUP;
#endif

   if (instance->debug_flags & TU_DEBUG_STARTUP)
      mesa_logi("Created an instance");

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   tu_init_dri_options(instance);

   *pInstance = tu_instance_to_handle(instance);

#ifdef HAVE_PERFETTO
   tu_perfetto_init();
#endif

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyInstance(VkInstance _instance,
                   const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_instance, instance, _instance);

   if (!instance)
      return;

   VG(VALGRIND_DESTROY_MEMPOOL(instance));

   driDestroyOptionCache(&instance->dri_options);
   driDestroyOptionInfo(&instance->available_dri_options);

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

static void
tu_get_physical_device_features_1_1(struct tu_physical_device *pdevice,
                                    VkPhysicalDeviceVulkan11Features *features)
{
   features->storageBuffer16BitAccess            = pdevice->info->a6xx.storage_16bit;
   features->uniformAndStorageBuffer16BitAccess  = false;
   features->storagePushConstant16               = false;
   features->storageInputOutput16                = false;
   features->multiview                           = true;
   features->multiviewGeometryShader             = false;
   features->multiviewTessellationShader         = false;
   features->variablePointersStorageBuffer       = true;
   features->variablePointers                    = true;
   features->protectedMemory                     = false;
   features->samplerYcbcrConversion              = true;
   features->shaderDrawParameters                = true;
}

static void
tu_get_physical_device_features_1_2(struct tu_physical_device *pdevice,
                                    VkPhysicalDeviceVulkan12Features *features)
{
   features->samplerMirrorClampToEdge            = true;
   features->drawIndirectCount                   = true;
   features->storageBuffer8BitAccess             = false;
   features->uniformAndStorageBuffer8BitAccess   = false;
   features->storagePushConstant8                = false;
   features->shaderBufferInt64Atomics            = false;
   features->shaderSharedInt64Atomics            = false;
   features->shaderFloat16                       = true;
   features->shaderInt8                          = false;

   features->descriptorIndexing                                 = true;
   features->shaderInputAttachmentArrayDynamicIndexing          = false;
   features->shaderUniformTexelBufferArrayDynamicIndexing       = true;
   features->shaderStorageTexelBufferArrayDynamicIndexing       = true;
   features->shaderUniformBufferArrayNonUniformIndexing         = true;
   features->shaderSampledImageArrayNonUniformIndexing          = true;
   features->shaderStorageBufferArrayNonUniformIndexing         = true;
   features->shaderStorageImageArrayNonUniformIndexing          = true;
   features->shaderInputAttachmentArrayNonUniformIndexing       = false;
   features->shaderUniformTexelBufferArrayNonUniformIndexing    = true;
   features->shaderStorageTexelBufferArrayNonUniformIndexing    = true;
   features->descriptorBindingUniformBufferUpdateAfterBind      = true;
   features->descriptorBindingSampledImageUpdateAfterBind       = true;
   features->descriptorBindingStorageImageUpdateAfterBind       = true;
   features->descriptorBindingStorageBufferUpdateAfterBind      = true;
   features->descriptorBindingUniformTexelBufferUpdateAfterBind = true;
   features->descriptorBindingStorageTexelBufferUpdateAfterBind = true;
   features->descriptorBindingUpdateUnusedWhilePending          = true;
   features->descriptorBindingPartiallyBound                    = true;
   features->descriptorBindingVariableDescriptorCount           = true;
   features->runtimeDescriptorArray                             = true;

   features->samplerFilterMinmax                 = true;
   features->scalarBlockLayout                   = true;
   features->imagelessFramebuffer                = true;
   features->uniformBufferStandardLayout         = true;
   features->shaderSubgroupExtendedTypes         = true;
   features->separateDepthStencilLayouts         = true;
   features->hostQueryReset                      = true;
   features->timelineSemaphore                   = true;
   features->bufferDeviceAddress                 = true;
   features->bufferDeviceAddressCaptureReplay    = pdevice->has_set_iova;
   features->bufferDeviceAddressMultiDevice      = false;
   features->vulkanMemoryModel                   = true;
   features->vulkanMemoryModelDeviceScope        = true;
   features->vulkanMemoryModelAvailabilityVisibilityChains = true;
   features->shaderOutputViewportIndex           = true;
   features->shaderOutputLayer                   = true;
   features->subgroupBroadcastDynamicId          = true;
}

static void
tu_get_physical_device_features_1_3(struct tu_physical_device *pdevice,
                                    VkPhysicalDeviceVulkan13Features *features)
{
   features->robustImageAccess                   = true;
   features->inlineUniformBlock                  = true;
   features->descriptorBindingInlineUniformBlockUpdateAfterBind = true;
   features->pipelineCreationCacheControl        = true;
   features->privateData                         = true;
   features->shaderDemoteToHelperInvocation      = true;
   features->shaderTerminateInvocation           = true;
   features->subgroupSizeControl                 = true;
   features->computeFullSubgroups                = true;
   features->synchronization2                    = true;
   features->textureCompressionASTC_HDR          = false;
   features->shaderZeroInitializeWorkgroupMemory = true;
   features->dynamicRendering                    = true;
   features->shaderIntegerDotProduct             = true;
   features->maintenance4                        = true;
}

void
tu_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceFeatures2 *pFeatures)
{
   TU_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);

   pFeatures->features = (VkPhysicalDeviceFeatures) {
      .robustBufferAccess = true,
      .fullDrawIndexUint32 = true,
      .imageCubeArray = true,
      .independentBlend = true,
      .geometryShader = true,
      .tessellationShader = true,
      .sampleRateShading = true,
      .dualSrcBlend = true,
      .logicOp = true,
      .multiDrawIndirect = true,
      .drawIndirectFirstInstance = true,
      .depthClamp = true,
      .depthBiasClamp = true,
      .fillModeNonSolid = true,
      .depthBounds = true,
      .wideLines = false,
      .largePoints = true,
      .alphaToOne = true,
      .multiViewport = true,
      .samplerAnisotropy = true,
      .textureCompressionETC2 = true,
      .textureCompressionASTC_LDR = true,
      .textureCompressionBC = true,
      .occlusionQueryPrecise = true,
      .pipelineStatisticsQuery = true,
      .vertexPipelineStoresAndAtomics = true,
      .fragmentStoresAndAtomics = true,
      .shaderTessellationAndGeometryPointSize = true,
      .shaderImageGatherExtended = true,
      .shaderStorageImageExtendedFormats = true,
      .shaderStorageImageMultisample = false,
      .shaderUniformBufferArrayDynamicIndexing = true,
      .shaderSampledImageArrayDynamicIndexing = true,
      .shaderStorageBufferArrayDynamicIndexing = true,
      .shaderStorageImageArrayDynamicIndexing = true,
      .shaderStorageImageReadWithoutFormat = true,
      .shaderStorageImageWriteWithoutFormat = true,
      .shaderClipDistance = true,
      .shaderCullDistance = true,
      .shaderFloat64 = false,
      .shaderInt64 = false,
      .shaderInt16 = true,
      .sparseBinding = false,
      .variableMultisampleRate = true,
      .inheritedQueries = true,
   };

   VkPhysicalDeviceVulkan11Features core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };
   tu_get_physical_device_features_1_1(pdevice, &core_1_1);

   VkPhysicalDeviceVulkan12Features core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
   };
   tu_get_physical_device_features_1_2(pdevice, &core_1_2);

   VkPhysicalDeviceVulkan13Features core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   tu_get_physical_device_features_1_3(pdevice, &core_1_3);

   vk_foreach_struct(ext, pFeatures->pNext)
   {
      if (vk_get_physical_device_core_1_1_feature_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_feature_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_feature_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT: {
         VkPhysicalDeviceConditionalRenderingFeaturesEXT *features =
            (VkPhysicalDeviceConditionalRenderingFeaturesEXT *) ext;
         features->conditionalRendering = true;
         features->inheritedConditionalRendering = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
         VkPhysicalDeviceTransformFeedbackFeaturesEXT *features =
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT *) ext;
         features->transformFeedback = true;
         features->geometryStreams = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
         VkPhysicalDeviceIndexTypeUint8FeaturesEXT *features =
            (VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)ext;
         features->indexTypeUint8 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *features =
            (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)ext;
         features->vertexAttributeInstanceRateDivisor = true;
         features->vertexAttributeInstanceRateZeroDivisor = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipEnableFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)ext;
         features->depthClipEnable = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
         VkPhysicalDevice4444FormatsFeaturesEXT *features = (void *)ext;
         features->formatA4R4G4B4 = true;
         features->formatA4B4G4R4 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT: {
         VkPhysicalDeviceBorderColorSwizzleFeaturesEXT *features = (void *)ext;
         features->borderColorSwizzle = true;
         features->borderColorSwizzleFromImage = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
         VkPhysicalDeviceCustomBorderColorFeaturesEXT *features = (void *) ext;
         features->customBorderColors = true;
         features->customBorderColorWithoutFormat = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *features = (void *)ext;
         features->extendedDynamicState = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *features =
            (VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *)ext;
         features->extendedDynamicState2 = true;
         features->extendedDynamicState2LogicOp = true;
         features->extendedDynamicState2PatchControlPoints = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *features =
            (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *)ext;
         features->extendedDynamicState3PolygonMode = true;
         features->extendedDynamicState3TessellationDomainOrigin = true;
         features->extendedDynamicState3DepthClampEnable = true;
         features->extendedDynamicState3DepthClipEnable = true;
         features->extendedDynamicState3LogicOpEnable = true;
         features->extendedDynamicState3SampleMask = true;
         features->extendedDynamicState3RasterizationSamples = true;
         features->extendedDynamicState3AlphaToCoverageEnable = true;
         features->extendedDynamicState3AlphaToOneEnable = true;
         features->extendedDynamicState3DepthClipNegativeOneToOne = true;
         features->extendedDynamicState3RasterizationStream = true;
         features->extendedDynamicState3ConservativeRasterizationMode = false;
         features->extendedDynamicState3ExtraPrimitiveOverestimationSize = false;
         features->extendedDynamicState3LineRasterizationMode = true;
         features->extendedDynamicState3LineStippleEnable = false;
         features->extendedDynamicState3ProvokingVertexMode = true;
         features->extendedDynamicState3SampleLocationsEnable = true;
         features->extendedDynamicState3ColorBlendEnable = true;
         features->extendedDynamicState3ColorBlendEquation = true;
         features->extendedDynamicState3ColorWriteMask = true;
         features->extendedDynamicState3ViewportWScalingEnable = false;
         features->extendedDynamicState3ViewportSwizzle = false;
         features->extendedDynamicState3ShadingRateImageEnable = false;
         features->extendedDynamicState3CoverageToColorEnable = false;
         features->extendedDynamicState3CoverageToColorLocation = false;
         features->extendedDynamicState3CoverageModulationMode = false;
         features->extendedDynamicState3CoverageModulationTableEnable = false;
         features->extendedDynamicState3CoverageModulationTable = false;
         features->extendedDynamicState3CoverageReductionMode = false;
         features->extendedDynamicState3RepresentativeFragmentTestEnable = false;
         features->extendedDynamicState3ColorBlendAdvanced = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR: {
         VkPhysicalDevicePerformanceQueryFeaturesKHR *feature =
            (VkPhysicalDevicePerformanceQueryFeaturesKHR *)ext;
         feature->performanceCounterQueryPools = true;
         feature->performanceCounterMultipleQueryPools = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR: {
         VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR *features =
            (VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR *)ext;
         features->pipelineExecutableInfo = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
         VkPhysicalDeviceShaderFloat16Int8Features *features =
            (VkPhysicalDeviceShaderFloat16Int8Features *) ext;
         features->shaderFloat16 = true;
         features->shaderInt8 = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES: {
         VkPhysicalDeviceScalarBlockLayoutFeatures *features = (void *)ext;
         features->scalarBlockLayout = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
         VkPhysicalDeviceRobustness2FeaturesEXT *features = (void *)ext;
         features->robustBufferAccess2 = true;
         features->robustImageAccess2 = true;
         features->nullDescriptor = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
         VkPhysicalDeviceTimelineSemaphoreFeatures *features =
            (VkPhysicalDeviceTimelineSemaphoreFeatures *) ext;
         features->timelineSemaphore = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
         VkPhysicalDeviceProvokingVertexFeaturesEXT *features =
            (VkPhysicalDeviceProvokingVertexFeaturesEXT *)ext;
         features->provokingVertexLast = true;
         features->transformFeedbackPreservesProvokingVertex = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT: {
         VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT *features =
            (VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT *)ext;
         features->mutableDescriptorType = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT: {
         VkPhysicalDeviceLineRasterizationFeaturesEXT *features =
            (VkPhysicalDeviceLineRasterizationFeaturesEXT *)ext;
         features->rectangularLines = true;
         features->bresenhamLines = true;
         features->smoothLines = false;
         features->stippledRectangularLines = false;
         features->stippledBresenhamLines = false;
         features->stippledSmoothLines = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT: {
         VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *features =
            (VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *)ext;
         features->primitiveTopologyListRestart = true;
         features->primitiveTopologyPatchListRestart = false;
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
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipControlFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipControlFeaturesEXT *)ext;
         features->depthClipControl = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT: {
         VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *features =
            (VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT *)ext;
         features->texelBufferAlignment = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT: {
         VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *features =
            (VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *)ext;
         features->primitivesGeneratedQuery = true;
         features->primitivesGeneratedQueryWithRasterizerDiscard = false;
         features->primitivesGeneratedQueryWithNonZeroStreams = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT: {
         VkPhysicalDeviceImageViewMinLodFeaturesEXT *features =
            (VkPhysicalDeviceImageViewMinLodFeaturesEXT *)ext;
         features->minLod = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT: {
         VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *features =
            (VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *)ext;
         features->image2DViewOf3D = true;
         features->sampler2DViewOf3D = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceColorWriteEnableFeaturesEXT *features =
            (VkPhysicalDeviceColorWriteEnableFeaturesEXT *)ext;
         features->colorWriteEnable = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT: {
         VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *features =
            (VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *)ext;
         features->shaderModuleIdentifier = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT: {
         VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *features =
            (VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)ext;
         features->vertexInputDynamicState = true;
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
            (VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT*)ext;
         features->attachmentFeedbackLoopLayout = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR: {
         VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR *features =
            (VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR*)ext;
         features->globalPriorityQuery = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT: {
         VkPhysicalDeviceMultiDrawFeaturesEXT *features =
            (VkPhysicalDeviceMultiDrawFeaturesEXT *)ext;
         features->multiDraw = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT: {
         VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *features =
            (VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)ext;
         features->graphicsPipelineLibrary = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR: {
         VkPhysicalDevicePresentIdFeaturesKHR *features =
            (VkPhysicalDevicePresentIdFeaturesKHR *) ext;
         features->presentId = pdevice->vk.supported_extensions.KHR_present_id;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR: {
         VkPhysicalDevicePresentWaitFeaturesKHR *features =
            (VkPhysicalDevicePresentWaitFeaturesKHR *) ext;
         features->presentWait = pdevice->vk.supported_extensions.KHR_present_wait;
         break;
      }

      default:
         break;
      }
   }
}


static void
tu_get_physical_device_properties_1_1(struct tu_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan11Properties *p)
{
   assert(p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);

   memcpy(p->deviceUUID, pdevice->device_uuid, VK_UUID_SIZE);
   memcpy(p->driverUUID, pdevice->driver_uuid, VK_UUID_SIZE);
   memset(p->deviceLUID, 0, VK_LUID_SIZE);
   p->deviceNodeMask = 0;
   p->deviceLUIDValid = false;

   p->subgroupSize = 128;
   p->subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
   p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                    VK_SUBGROUP_FEATURE_VOTE_BIT |
                                    VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                    VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                    VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                    VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
   if (pdevice->info->a6xx.has_getfiberid) {
      p->subgroupSupportedStages |= VK_SHADER_STAGE_ALL_GRAPHICS;
      p->subgroupSupportedOperations |= VK_SUBGROUP_FEATURE_QUAD_BIT;
   }

   p->subgroupQuadOperationsInAllStages = false;

   p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
   p->maxMultiviewViewCount = MAX_VIEWS;
   p->maxMultiviewInstanceIndex = INT_MAX;
   p->protectedNoFault = false;
   /* Our largest descriptors are 2 texture descriptors, or a texture and
    * sampler descriptor.
    */
   p->maxPerSetDescriptors = MAX_SET_SIZE / (2 * A6XX_TEX_CONST_DWORDS * 4);
   /* Our buffer size fields allow only this much */
   p->maxMemoryAllocationSize = 0xFFFFFFFFull;

}


static const size_t max_descriptor_set_size = MAX_SET_SIZE / (4 * A6XX_TEX_CONST_DWORDS);
static const VkSampleCountFlags sample_counts =
   VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;

static void
tu_get_physical_device_properties_1_2(struct tu_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan12Properties *p)
{
   assert(p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);

   p->driverID = VK_DRIVER_ID_MESA_TURNIP;
   memset(p->driverName, 0, sizeof(p->driverName));
   snprintf(p->driverName, VK_MAX_DRIVER_NAME_SIZE,
            "turnip Mesa driver");
   memset(p->driverInfo, 0, sizeof(p->driverInfo));
   snprintf(p->driverInfo, VK_MAX_DRIVER_INFO_SIZE,
            "Mesa " PACKAGE_VERSION MESA_GIT_SHA1);
   p->conformanceVersion = (VkConformanceVersion) {
      .major = 1,
      .minor = 2,
      .subminor = 7,
      .patch = 1,
   };

   p->denormBehaviorIndependence =
      VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
   p->roundingModeIndependence =
      VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;

   p->shaderDenormFlushToZeroFloat16         = true;
   p->shaderDenormPreserveFloat16            = false;
   p->shaderRoundingModeRTEFloat16           = true;
   p->shaderRoundingModeRTZFloat16           = false;
   p->shaderSignedZeroInfNanPreserveFloat16  = true;

   p->shaderDenormFlushToZeroFloat32         = true;
   p->shaderDenormPreserveFloat32            = false;
   p->shaderRoundingModeRTEFloat32           = true;
   p->shaderRoundingModeRTZFloat32           = false;
   p->shaderSignedZeroInfNanPreserveFloat32  = true;

   p->shaderDenormFlushToZeroFloat64         = false;
   p->shaderDenormPreserveFloat64            = false;
   p->shaderRoundingModeRTEFloat64           = false;
   p->shaderRoundingModeRTZFloat64           = false;
   p->shaderSignedZeroInfNanPreserveFloat64  = false;

   p->shaderUniformBufferArrayNonUniformIndexingNative   = true;
   p->shaderSampledImageArrayNonUniformIndexingNative    = true;
   p->shaderStorageBufferArrayNonUniformIndexingNative   = true;
   p->shaderStorageImageArrayNonUniformIndexingNative    = true;
   p->shaderInputAttachmentArrayNonUniformIndexingNative = false;
   p->robustBufferAccessUpdateAfterBind                  = false;
   p->quadDivergentImplicitLod                           = false;

   p->maxUpdateAfterBindDescriptorsInAllPools            = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindSamplers       = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindSampledImages  = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindStorageImages  = max_descriptor_set_size;
   p->maxPerStageDescriptorUpdateAfterBindInputAttachments = MAX_RTS;
   p->maxPerStageUpdateAfterBindResources                = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindSamplers            = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindUniformBuffers      = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = MAX_DYNAMIC_UNIFORM_BUFFERS;
   p->maxDescriptorSetUpdateAfterBindStorageBuffers      = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = MAX_DYNAMIC_STORAGE_BUFFERS;
   p->maxDescriptorSetUpdateAfterBindSampledImages       = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindStorageImages       = max_descriptor_set_size;
   p->maxDescriptorSetUpdateAfterBindInputAttachments    = MAX_RTS;

   p->supportedDepthResolveModes    = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
   p->supportedStencilResolveModes  = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
   p->independentResolveNone  = false;
   p->independentResolve      = false;

   p->filterMinmaxSingleComponentFormats  = true;
   p->filterMinmaxImageComponentMapping   = true;

   p->maxTimelineSemaphoreValueDifference = UINT64_MAX;

   p->framebufferIntegerColorSampleCounts = sample_counts;
}

static void
tu_get_physical_device_properties_1_3(struct tu_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan13Properties *p)
{
   /* TODO move threadsize_base and max_waves to fd_dev_info and use them here */
   p->minSubgroupSize = 64; /* threadsize_base */
   p->maxSubgroupSize = 128; /* threadsize_base * 2 */
   p->maxComputeWorkgroupSubgroups = 16; /* max_waves */
   p->requiredSubgroupSizeStages = VK_SHADER_STAGE_ALL;

   p->maxInlineUniformBlockSize = MAX_INLINE_UBO_RANGE;
   p->maxPerStageDescriptorInlineUniformBlocks = MAX_INLINE_UBOS;
   p->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = MAX_INLINE_UBOS;
   p->maxDescriptorSetInlineUniformBlocks = MAX_INLINE_UBOS;
   p->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = MAX_INLINE_UBOS;
   p->maxInlineUniformTotalSize = MAX_INLINE_UBOS * MAX_INLINE_UBO_RANGE;

   p->integerDotProduct8BitUnsignedAccelerated = false;
   p->integerDotProduct8BitSignedAccelerated = false;
   p->integerDotProduct8BitMixedSignednessAccelerated = false;
   p->integerDotProduct4x8BitPackedUnsignedAccelerated =
      pdevice->info->a6xx.has_dp2acc;
   /* TODO: we should be able to emulate 4x8BitPackedSigned fast enough */
   p->integerDotProduct4x8BitPackedSignedAccelerated = false;
   p->integerDotProduct4x8BitPackedMixedSignednessAccelerated =
      pdevice->info->a6xx.has_dp2acc;
   p->integerDotProduct16BitUnsignedAccelerated = false;
   p->integerDotProduct16BitSignedAccelerated = false;
   p->integerDotProduct16BitMixedSignednessAccelerated = false;
   p->integerDotProduct32BitUnsignedAccelerated = false;
   p->integerDotProduct32BitSignedAccelerated = false;
   p->integerDotProduct32BitMixedSignednessAccelerated = false;
   p->integerDotProduct64BitUnsignedAccelerated = false;
   p->integerDotProduct64BitSignedAccelerated = false;
   p->integerDotProduct64BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating8BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating8BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated =
      pdevice->info->a6xx.has_dp2acc;
   /* TODO: we should be able to emulate Saturating4x8BitPackedSigned fast enough */
   p->integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated =
      pdevice->info->a6xx.has_dp2acc;
   p->integerDotProductAccumulatingSaturating16BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated = false;

   p->storageTexelBufferOffsetAlignmentBytes = 64;
   p->storageTexelBufferOffsetSingleTexelAlignment = false;
   p->uniformTexelBufferOffsetAlignmentBytes = 64;
   p->uniformTexelBufferOffsetSingleTexelAlignment = false;

   /* The address space is 4GB for current kernels, so there's no point
    * allowing a larger buffer. Our buffer sizes are 64-bit though, so
    * GetBufferDeviceRequirements won't fall over if someone actually creates
    * a 4GB buffer.
    */
   p->maxBufferSize = 1ull << 32;
}

VKAPI_ATTR void VKAPI_CALL
tu_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties2 *pProperties)
{
   TU_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);

   VkPhysicalDeviceLimits limits = {
      .maxImageDimension1D = (1 << 14),
      .maxImageDimension2D = (1 << 14),
      .maxImageDimension3D = (1 << 11),
      .maxImageDimensionCube = (1 << 14),
      .maxImageArrayLayers = (1 << 11),
      .maxTexelBufferElements = 128 * 1024 * 1024,
      .maxUniformBufferRange = MAX_UNIFORM_BUFFER_RANGE,
      .maxStorageBufferRange = MAX_STORAGE_BUFFER_RANGE,
      .maxPushConstantsSize = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount = UINT32_MAX,
      .maxSamplerAllocationCount = 64 * 1024,
      .bufferImageGranularity = 64,          /* A cache line */
      .sparseAddressSpaceSize = 0,
      .maxBoundDescriptorSets = MAX_SETS,
      .maxPerStageDescriptorSamplers = max_descriptor_set_size,
      .maxPerStageDescriptorUniformBuffers = max_descriptor_set_size,
      .maxPerStageDescriptorStorageBuffers = max_descriptor_set_size,
      .maxPerStageDescriptorSampledImages = max_descriptor_set_size,
      .maxPerStageDescriptorStorageImages = max_descriptor_set_size,
      .maxPerStageDescriptorInputAttachments = MAX_RTS,
      .maxPerStageResources = max_descriptor_set_size,
      .maxDescriptorSetSamplers = max_descriptor_set_size,
      .maxDescriptorSetUniformBuffers = max_descriptor_set_size,
      .maxDescriptorSetUniformBuffersDynamic = MAX_DYNAMIC_UNIFORM_BUFFERS,
      .maxDescriptorSetStorageBuffers = max_descriptor_set_size,
      .maxDescriptorSetStorageBuffersDynamic = MAX_DYNAMIC_STORAGE_BUFFERS,
      .maxDescriptorSetSampledImages = max_descriptor_set_size,
      .maxDescriptorSetStorageImages = max_descriptor_set_size,
      .maxDescriptorSetInputAttachments = MAX_RTS,
      .maxVertexInputAttributes = 32,
      .maxVertexInputBindings = 32,
      .maxVertexInputAttributeOffset = 4095,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 128,
      .maxTessellationGenerationLevel = 64,
      .maxTessellationPatchSize = 32,
      .maxTessellationControlPerVertexInputComponents = 128,
      .maxTessellationControlPerVertexOutputComponents = 128,
      .maxTessellationControlPerPatchOutputComponents = 120,
      .maxTessellationControlTotalOutputComponents = 4096,
      .maxTessellationEvaluationInputComponents = 128,
      .maxTessellationEvaluationOutputComponents = 128,
      .maxGeometryShaderInvocations = 32,
      .maxGeometryInputComponents = 64,
      .maxGeometryOutputComponents = 128,
      .maxGeometryOutputVertices = 256,
      .maxGeometryTotalOutputComponents = 1024,
      .maxFragmentInputComponents = 124,
      .maxFragmentOutputAttachments = 8,
      .maxFragmentDualSrcAttachments = 1,
      .maxFragmentCombinedOutputResources = MAX_RTS + max_descriptor_set_size * 2,
      .maxComputeSharedMemorySize = 32768,
      .maxComputeWorkGroupCount = { 65535, 65535, 65535 },
      .maxComputeWorkGroupInvocations = 2048,
      .maxComputeWorkGroupSize = { 1024, 1024, 1024 },
      .subPixelPrecisionBits = 8,
      .subTexelPrecisionBits = 8,
      .mipmapPrecisionBits = 8,
      .maxDrawIndexedIndexValue = UINT32_MAX,
      .maxDrawIndirectCount = UINT32_MAX,
      .maxSamplerLodBias = 4095.0 / 256.0, /* [-16, 15.99609375] */
      .maxSamplerAnisotropy = 16,
      .maxViewports = MAX_VIEWPORTS,
      .maxViewportDimensions = { MAX_VIEWPORT_SIZE, MAX_VIEWPORT_SIZE },
      .viewportBoundsRange = { INT16_MIN, INT16_MAX },
      .viewportSubPixelBits = 8,
      .minMemoryMapAlignment = 4096, /* A page */
      .minTexelBufferOffsetAlignment = 64,
      .minUniformBufferOffsetAlignment = 64,
      .minStorageBufferOffsetAlignment = 64,
      .minTexelOffset = -16,
      .maxTexelOffset = 15,
      .minTexelGatherOffset = -32,
      .maxTexelGatherOffset = 31,
      .minInterpolationOffset = -0.5,
      .maxInterpolationOffset = 0.4375,
      .subPixelInterpolationOffsetBits = 4,
      .maxFramebufferWidth = (1 << 14),
      .maxFramebufferHeight = (1 << 14),
      .maxFramebufferLayers = (1 << 10),
      .framebufferColorSampleCounts = sample_counts,
      .framebufferDepthSampleCounts = sample_counts,
      .framebufferStencilSampleCounts = sample_counts,
      .framebufferNoAttachmentsSampleCounts = sample_counts,
      .maxColorAttachments = MAX_RTS,
      .sampledImageColorSampleCounts = sample_counts,
      .sampledImageIntegerSampleCounts = sample_counts,
      .sampledImageDepthSampleCounts = sample_counts,
      .sampledImageStencilSampleCounts = sample_counts,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .timestampComputeAndGraphics = true,
      .timestampPeriod = 1000000000.0 / 19200000.0, /* CP_ALWAYS_ON_COUNTER is fixed 19.2MHz */
      .maxClipDistances = 8,
      .maxCullDistances = 8,
      .maxCombinedClipAndCullDistances = 8,
      .discreteQueuePriorities = 2,
      .pointSizeRange = { 1, 4092 },
      .lineWidthRange = { 1.0, 1.0 },
      .pointSizeGranularity = 	0.0625,
      .lineWidthGranularity = 0.0,
      .strictLines = true,
      .standardSampleLocations = true,
      .optimalBufferCopyOffsetAlignment = 128,
      .optimalBufferCopyRowPitchAlignment = 128,
      .nonCoherentAtomSize = 64,
   };

   pProperties->properties = (VkPhysicalDeviceProperties) {
      .apiVersion = TU_API_VERSION,
      .driverVersion = vk_get_driver_version(),
      .vendorID = 0x5143,
      .deviceID = pdevice->dev_id.chip_id,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = { 0 },
   };

   strcpy(pProperties->properties.deviceName, pdevice->name);
   memcpy(pProperties->properties.pipelineCacheUUID, pdevice->cache_uuid, VK_UUID_SIZE);

   VkPhysicalDeviceVulkan11Properties core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
   };
   tu_get_physical_device_properties_1_1(pdevice, &core_1_1);

   VkPhysicalDeviceVulkan12Properties core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
   };
   tu_get_physical_device_properties_1_2(pdevice, &core_1_2);

   VkPhysicalDeviceVulkan13Properties core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
   };
   tu_get_physical_device_properties_1_3(pdevice, &core_1_3);

   vk_foreach_struct(ext, pProperties->pNext)
   {
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
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT: {
         VkPhysicalDeviceTransformFeedbackPropertiesEXT *properties =
            (VkPhysicalDeviceTransformFeedbackPropertiesEXT *)ext;

         properties->maxTransformFeedbackStreams = IR3_MAX_SO_STREAMS;
         properties->maxTransformFeedbackBuffers = IR3_MAX_SO_BUFFERS;
         properties->maxTransformFeedbackBufferSize = UINT32_MAX;
         properties->maxTransformFeedbackStreamDataSize = 512;
         properties->maxTransformFeedbackBufferDataSize = 512;
         properties->maxTransformFeedbackBufferDataStride = 512;
         properties->transformFeedbackQueries = true;
         properties->transformFeedbackStreamsLinesTriangles = true;
         properties->transformFeedbackRasterizationStreamSelect = true;
         properties->transformFeedbackDraw = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT: {
         VkPhysicalDeviceSampleLocationsPropertiesEXT *properties =
            (VkPhysicalDeviceSampleLocationsPropertiesEXT *)ext;
         properties->sampleLocationSampleCounts = 0;
         if (pdevice->vk.supported_extensions.EXT_sample_locations) {
            properties->sampleLocationSampleCounts =
               VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
         }
         properties->maxSampleLocationGridSize = (VkExtent2D) { 1 , 1 };
         properties->sampleLocationCoordinateRange[0] = SAMPLE_LOCATION_MIN;
         properties->sampleLocationCoordinateRange[1] = SAMPLE_LOCATION_MAX;
         properties->sampleLocationSubPixelBits = 4;
         properties->variableSampleLocations = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *props =
            (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)ext;
         props->maxVertexAttribDivisor = UINT32_MAX;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT: {
         VkPhysicalDeviceCustomBorderColorPropertiesEXT *props = (void *)ext;
         props->maxCustomBorderColorSamplers = TU_BORDER_COLOR_COUNT;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_PROPERTIES_KHR: {
         VkPhysicalDevicePerformanceQueryPropertiesKHR *properties =
            (VkPhysicalDevicePerformanceQueryPropertiesKHR *)ext;
         properties->allowCommandBufferQueryCopies = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT: {
         VkPhysicalDeviceRobustness2PropertiesEXT *props = (void *)ext;
         /* see write_buffer_descriptor() */
         props->robustStorageBufferAccessSizeAlignment = 4;
         /* see write_ubo_descriptor() */
         props->robustUniformBufferAccessSizeAlignment = 16;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT: {
         VkPhysicalDeviceProvokingVertexPropertiesEXT *properties =
            (VkPhysicalDeviceProvokingVertexPropertiesEXT *)ext;
         properties->provokingVertexModePerPipeline = true;
         properties->transformFeedbackPreservesTriangleFanProvokingVertex = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT: {
         VkPhysicalDeviceLineRasterizationPropertiesEXT *props =
            (VkPhysicalDeviceLineRasterizationPropertiesEXT *)ext;
         props->lineSubPixelPrecisionBits = 8;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT: {
         VkPhysicalDeviceDrmPropertiesEXT *props =
            (VkPhysicalDeviceDrmPropertiesEXT *)ext;
         props->hasPrimary = pdevice->has_master;
         props->primaryMajor = pdevice->master_major;
         props->primaryMinor = pdevice->master_minor;

         props->hasRender = pdevice->has_local;
         props->renderMajor = pdevice->local_major;
         props->renderMinor = pdevice->local_minor;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT: {
         VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT *props =
            (VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT *)ext;
         STATIC_ASSERT(sizeof(vk_shaderModuleIdentifierAlgorithmUUID) ==
                       sizeof(props->shaderModuleIdentifierAlgorithmUUID));
         memcpy(props->shaderModuleIdentifierAlgorithmUUID,
                vk_shaderModuleIdentifierAlgorithmUUID,
                sizeof(props->shaderModuleIdentifierAlgorithmUUID));
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT: {
         VkPhysicalDeviceMultiDrawPropertiesEXT *properties =
            (VkPhysicalDeviceMultiDrawPropertiesEXT *)ext;
         properties->maxMultiDrawCount = 2048;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT: {
         VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *props =
            (VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *)ext;
         props->graphicsPipelineLibraryFastLinking = true;
         props->graphicsPipelineLibraryIndependentInterpolationDecoration = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *properties =
            (VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *)ext;
         properties->dynamicPrimitiveTopologyUnrestricted = true;
         break;
      }
      default:
         break;
      }
   }
}

static const VkQueueFamilyProperties tu_queue_family_properties = {
   .queueFlags =
      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 48,
   .minImageTransferGranularity = { 1, 1, 1 },
};

static void
tu_physical_device_get_global_priority_properties(const struct tu_physical_device *pdevice,
                                                  VkQueueFamilyGlobalPriorityPropertiesKHR *props)
{
   props->priorityCount = MIN2(pdevice->submitqueue_priority_count, 3);
   switch (props->priorityCount) {
   case 1:
      props->priorities[0] = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
      break;
   case 2:
      props->priorities[0] = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
      props->priorities[1] = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
      break;
   case 3:
      props->priorities[0] = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
      props->priorities[1] = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
      props->priorities[2] = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
      break;
   default:
      unreachable("unexpected priority count");
      break;
   }
}

static int
tu_physical_device_get_submitqueue_priority(const struct tu_physical_device *pdevice,
                                            VkQueueGlobalPriorityKHR global_priority,
                                            bool global_priority_query)
{
   if (global_priority_query) {
      VkQueueFamilyGlobalPriorityPropertiesKHR props;
      tu_physical_device_get_global_priority_properties(pdevice, &props);

      bool valid = false;
      for (uint32_t i = 0; i < props.priorityCount; i++) {
         if (props.priorities[i] == global_priority) {
            valid = true;
            break;
         }
      }

      if (!valid)
         return -1;
   }

   /* Valid values are from 0 to (pdevice->submitqueue_priority_count - 1),
    * with 0 being the highest priority.  This matches what freedreno does.
    */
   int priority;
   if (global_priority == VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR)
      priority = pdevice->submitqueue_priority_count / 2;
   else if (global_priority < VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR)
      priority = pdevice->submitqueue_priority_count - 1;
   else
      priority = 0;

   return priority;
}

VKAPI_ATTR void VKAPI_CALL
tu_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   TU_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);

   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out,
                          pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p)
   {
      p->queueFamilyProperties = tu_queue_family_properties;

      vk_foreach_struct(ext, p->pNext) {
         switch (ext->sType) {
         case VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR: {
            VkQueueFamilyGlobalPriorityPropertiesKHR *props = (void *)ext;
            tu_physical_device_get_global_priority_properties(pdevice, props);
            break;
         }
         default:
            break;
         }
      }
   }
}

uint64_t
tu_get_system_heap_size()
{
   struct sysinfo info;
   sysinfo(&info);

   uint64_t total_ram = (uint64_t) info.totalram * (uint64_t) info.mem_unit;

   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024ull * 1024ull * 1024ull)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   return available_ram;
}

static VkDeviceSize
tu_get_budget_memory(struct tu_physical_device *physical_device)
{
   uint64_t heap_size = physical_device->heap.size;
   uint64_t heap_used = physical_device->heap.used;
   uint64_t sys_available;
   ASSERTED bool has_available_memory =
      os_get_available_system_memory(&sys_available);
   assert(has_available_memory);

   /*
    * Let's not incite the app to starve the system: report at most 90% of
    * available system memory.
    */
   uint64_t heap_available = sys_available * 9 / 10;
   return MIN2(heap_size, heap_used + heap_available);
}

VKAPI_ATTR void VKAPI_CALL
tu_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pdev,
                                      VkPhysicalDeviceMemoryProperties2 *props2)
{
   TU_FROM_HANDLE(tu_physical_device, physical_device, pdev);

   VkPhysicalDeviceMemoryProperties *props = &props2->memoryProperties;
   props->memoryHeapCount = 1;
   props->memoryHeaps[0].size = physical_device->heap.size;
   props->memoryHeaps[0].flags = physical_device->heap.flags;

   props->memoryTypeCount = 1;
   props->memoryTypes[0].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   props->memoryTypes[0].heapIndex = 0;

   vk_foreach_struct(ext, props2->pNext)
   {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT: {
         VkPhysicalDeviceMemoryBudgetPropertiesEXT *memory_budget_props =
            (VkPhysicalDeviceMemoryBudgetPropertiesEXT *) ext;
         memory_budget_props->heapUsage[0] = physical_device->heap.used;
         memory_budget_props->heapBudget[0] = tu_get_budget_memory(physical_device);

         /* The heapBudget and heapUsage values must be zero for array elements
          * greater than or equal to VkPhysicalDeviceMemoryProperties::memoryHeapCount
          */
         for (unsigned i = 1; i < VK_MAX_MEMORY_HEAPS; i++) {
            memory_budget_props->heapBudget[i] = 0u;
            memory_budget_props->heapUsage[i] = 0u;
         }
         break;
      }
      default:
         break;
      }
   }
}

static VkResult
tu_queue_init(struct tu_device *device,
              struct tu_queue *queue,
              int idx,
              const VkDeviceQueueCreateInfo *create_info,
              bool global_priority_query)
{
   const VkDeviceQueueGlobalPriorityCreateInfoKHR *priority_info =
      vk_find_struct_const(create_info->pNext,
            DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR);
   const enum VkQueueGlobalPriorityKHR global_priority = priority_info ?
      priority_info->globalPriority : VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;

   const int priority = tu_physical_device_get_submitqueue_priority(
         device->physical_device, global_priority, global_priority_query);
   if (priority < 0) {
      return vk_startup_errorf(device->instance, VK_ERROR_INITIALIZATION_FAILED,
                               "invalid global priority");
   }

   VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info, idx);
   if (result != VK_SUCCESS)
      return result;

   queue->device = device;
#ifndef TU_USE_KGSL
   queue->vk.driver_submit = tu_queue_submit;
#endif

   int ret = tu_drm_submitqueue_new(device, priority, &queue->msm_queue_id);
   if (ret)
      return vk_startup_errorf(device->instance, VK_ERROR_INITIALIZATION_FAILED,
                               "submitqueue create failed");

   queue->fence = -1;

   return VK_SUCCESS;
}

static void
tu_queue_finish(struct tu_queue *queue)
{
   vk_queue_finish(&queue->vk);
   if (queue->fence >= 0)
      close(queue->fence);
   tu_drm_submitqueue_close(queue->device, queue->msm_queue_id);
}

uint64_t
tu_device_ticks_to_ns(struct tu_device *dev, uint64_t ts)
{
   /* This is based on the 19.2MHz always-on rbbm timer.
    *
    * TODO we should probably query this value from kernel..
    */
   return ts * (1000000000 / 19200000);
}

struct u_trace_context *
tu_device_get_u_trace(struct tu_device *device)
{
   return &device->trace_context;
}

static void*
tu_trace_create_ts_buffer(struct u_trace_context *utctx, uint32_t size)
{
   struct tu_device *device =
      container_of(utctx, struct tu_device, trace_context);

   struct tu_bo *bo;
   tu_bo_init_new(device, &bo, size, false, "trace");

   return bo;
}

static void
tu_trace_destroy_ts_buffer(struct u_trace_context *utctx, void *timestamps)
{
   struct tu_device *device =
      container_of(utctx, struct tu_device, trace_context);
   struct tu_bo *bo = timestamps;

   tu_bo_finish(device, bo);
}

static void
tu_trace_record_ts(struct u_trace *ut, void *cs, void *timestamps,
                   unsigned idx, bool end_of_pipe)
{
   struct tu_bo *bo = timestamps;
   struct tu_cs *ts_cs = cs;

   unsigned ts_offset = idx * sizeof(uint64_t);
   tu_cs_emit_pkt7(ts_cs, CP_EVENT_WRITE, 4);
   tu_cs_emit(ts_cs, CP_EVENT_WRITE_0_EVENT(RB_DONE_TS) | CP_EVENT_WRITE_0_TIMESTAMP);
   tu_cs_emit_qw(ts_cs, bo->iova + ts_offset);
   tu_cs_emit(ts_cs, 0x00000000);
}

static uint64_t
tu_trace_read_ts(struct u_trace_context *utctx,
                 void *timestamps, unsigned idx, void *flush_data)
{
   struct tu_device *device =
      container_of(utctx, struct tu_device, trace_context);
   struct tu_bo *bo = timestamps;
   struct tu_u_trace_submission_data *submission_data = flush_data;

   /* Only need to stall on results for the first entry: */
   if (idx == 0) {
      tu_device_wait_u_trace(device, submission_data->syncobj);
   }

   if (tu_bo_map(device, bo) != VK_SUCCESS) {
      return U_TRACE_NO_TIMESTAMP;
   }

   uint64_t *ts = bo->map;

   /* Don't translate the no-timestamp marker: */
   if (ts[idx] == U_TRACE_NO_TIMESTAMP)
      return U_TRACE_NO_TIMESTAMP;

   return tu_device_ticks_to_ns(device, ts[idx]);
}

static void
tu_trace_delete_flush_data(struct u_trace_context *utctx, void *flush_data)
{
   struct tu_device *device =
      container_of(utctx, struct tu_device, trace_context);
   struct tu_u_trace_submission_data *submission_data = flush_data;

   tu_u_trace_submission_data_finish(device, submission_data);
}

void
tu_copy_timestamp_buffer(struct u_trace_context *utctx, void *cmdstream,
                         void *ts_from, uint32_t from_offset,
                         void *ts_to, uint32_t to_offset,
                         uint32_t count)
{
   struct tu_cs *cs = cmdstream;
   struct tu_bo *bo_from = ts_from;
   struct tu_bo *bo_to = ts_to;

   tu_cs_emit_pkt7(cs, CP_MEMCPY, 5);
   tu_cs_emit(cs, count * sizeof(uint64_t) / sizeof(uint32_t));
   tu_cs_emit_qw(cs, bo_from->iova + from_offset * sizeof(uint64_t));
   tu_cs_emit_qw(cs, bo_to->iova + to_offset * sizeof(uint64_t));
}

/* Special helpers instead of u_trace_begin_iterator()/u_trace_end_iterator()
 * that ignore tracepoints at the beginning/end that are part of a
 * suspend/resume chain.
 */
static struct u_trace_iterator
tu_cmd_begin_iterator(struct tu_cmd_buffer *cmdbuf)
{
   switch (cmdbuf->state.suspend_resume) {
   case SR_IN_PRE_CHAIN:
      return cmdbuf->trace_renderpass_end;
   case SR_AFTER_PRE_CHAIN:
   case SR_IN_CHAIN_AFTER_PRE_CHAIN:
      return cmdbuf->pre_chain.trace_renderpass_end;
   default:
      return u_trace_begin_iterator(&cmdbuf->trace);
   }
}

static struct u_trace_iterator
tu_cmd_end_iterator(struct tu_cmd_buffer *cmdbuf)
{
   switch (cmdbuf->state.suspend_resume) {
   case SR_IN_PRE_CHAIN:
      return cmdbuf->trace_renderpass_end;
   case SR_IN_CHAIN:
   case SR_IN_CHAIN_AFTER_PRE_CHAIN:
      return cmdbuf->trace_renderpass_start;
   default:
      return u_trace_end_iterator(&cmdbuf->trace);
   }
}
VkResult
tu_create_copy_timestamp_cs(struct tu_cmd_buffer *cmdbuf, struct tu_cs** cs,
                            struct u_trace **trace_copy)
{
   *cs = vk_zalloc(&cmdbuf->device->vk.alloc, sizeof(struct tu_cs), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (*cs == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   tu_cs_init(*cs, cmdbuf->device, TU_CS_MODE_GROW,
              list_length(&cmdbuf->trace.trace_chunks) * 6 + 3, "trace copy timestamp cs");

   tu_cs_begin(*cs);

   tu_cs_emit_wfi(*cs);
   tu_cs_emit_pkt7(*cs, CP_WAIT_FOR_ME, 0);

   *trace_copy = vk_zalloc(&cmdbuf->device->vk.alloc, sizeof(struct u_trace), 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (*trace_copy == NULL) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   u_trace_init(*trace_copy, cmdbuf->trace.utctx);
   u_trace_clone_append(tu_cmd_begin_iterator(cmdbuf),
                        tu_cmd_end_iterator(cmdbuf),
                        *trace_copy, *cs,
                        tu_copy_timestamp_buffer);

   tu_cs_emit_wfi(*cs);

   tu_cs_end(*cs);

   return VK_SUCCESS;
}

VkResult
tu_u_trace_submission_data_create(
   struct tu_device *device,
   struct tu_cmd_buffer **cmd_buffers,
   uint32_t cmd_buffer_count,
   struct tu_u_trace_submission_data **submission_data)
{
   *submission_data =
      vk_zalloc(&device->vk.alloc,
                sizeof(struct tu_u_trace_submission_data), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (!(*submission_data)) {
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct tu_u_trace_submission_data *data = *submission_data;

   data->cmd_trace_data =
      vk_zalloc(&device->vk.alloc,
                cmd_buffer_count * sizeof(struct tu_u_trace_cmd_data), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (!data->cmd_trace_data) {
      goto fail;
   }

   data->cmd_buffer_count = cmd_buffer_count;
   data->last_buffer_with_tracepoints = -1;

   for (uint32_t i = 0; i < cmd_buffer_count; ++i) {
      struct tu_cmd_buffer *cmdbuf = cmd_buffers[i];

      if (!u_trace_has_points(&cmdbuf->trace))
         continue;

      data->last_buffer_with_tracepoints = i;

      if (!(cmdbuf->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) {
         /* A single command buffer could be submitted several times, but we
          * already baked timestamp iova addresses and trace points are
          * single-use. Therefor we have to copy trace points and create
          * a new timestamp buffer on every submit of reusable command buffer.
          */
         if (tu_create_copy_timestamp_cs(cmdbuf,
               &data->cmd_trace_data[i].timestamp_copy_cs,
               &data->cmd_trace_data[i].trace) != VK_SUCCESS) {
            goto fail;
         }

         assert(data->cmd_trace_data[i].timestamp_copy_cs->entry_count == 1);
      } else {
         data->cmd_trace_data[i].trace = &cmdbuf->trace;
      }
   }

   assert(data->last_buffer_with_tracepoints != -1);

   return VK_SUCCESS;

fail:
   tu_u_trace_submission_data_finish(device, data);
   *submission_data = NULL;

   return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
}

void
tu_u_trace_submission_data_finish(
   struct tu_device *device,
   struct tu_u_trace_submission_data *submission_data)
{
   for (uint32_t i = 0; i < submission_data->cmd_buffer_count; ++i) {
      /* Only if we had to create a copy of trace we should free it */
      struct tu_u_trace_cmd_data *cmd_data = &submission_data->cmd_trace_data[i];
      if (cmd_data->timestamp_copy_cs) {
         tu_cs_finish(cmd_data->timestamp_copy_cs);
         vk_free(&device->vk.alloc, cmd_data->timestamp_copy_cs);

         u_trace_fini(cmd_data->trace);
         vk_free(&device->vk.alloc, cmd_data->trace);
      }
   }

   vk_free(&device->vk.alloc, submission_data->cmd_trace_data);
   vk_free(&device->vk.alloc, submission_data->syncobj);
   vk_free(&device->vk.alloc, submission_data);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateDevice(VkPhysicalDevice physicalDevice,
                const VkDeviceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkDevice *pDevice)
{
   TU_FROM_HANDLE(tu_physical_device, physical_device, physicalDevice);
   VkResult result;
   struct tu_device *device;
   bool custom_border_colors = false;
   bool perf_query_pools = false;
   bool robust_buffer_access2 = false;
   bool border_color_without_format = false;
   bool global_priority_query = false;

   vk_foreach_struct_const(ext, pCreateInfo->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
         const VkPhysicalDeviceCustomBorderColorFeaturesEXT *border_color_features = (const void *)ext;
         custom_border_colors = border_color_features->customBorderColors;
         border_color_without_format =
            border_color_features->customBorderColorWithoutFormat;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR: {
         const VkPhysicalDevicePerformanceQueryFeaturesKHR *feature =
            (VkPhysicalDevicePerformanceQueryFeaturesKHR *)ext;
         perf_query_pools = feature->performanceCounterQueryPools;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
         VkPhysicalDeviceRobustness2FeaturesEXT *features = (void *)ext;
         robust_buffer_access2 = features->robustBufferAccess2;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR: {
         VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR *features = (void *)ext;
         global_priority_query = features->globalPriorityQuery;
         break;
      }
      default:
         break;
      }
   }

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_startup_errorf(physical_device->instance, VK_ERROR_OUT_OF_HOST_MEMORY, "OOM");

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &tu_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_device_entrypoints, false);

   result = vk_device_init(&device->vk, &physical_device->vk,
                           &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return vk_startup_errorf(physical_device->instance, result,
                               "vk_device_init failed");
   }

   device->instance = physical_device->instance;
   device->physical_device = physical_device;
   device->fd = physical_device->local_fd;
   device->vk.command_buffer_ops = &tu_cmd_buffer_ops;
   device->vk.check_status = tu_device_check_status;

   mtx_init(&device->bo_mutex, mtx_plain);
   mtx_init(&device->pipeline_mutex, mtx_plain);
   mtx_init(&device->autotune_mutex, mtx_plain);
   u_rwlock_init(&device->dma_bo_lock);
   pthread_mutex_init(&device->submit_mutex, NULL);

   if (device->instance->debug_flags & TU_DEBUG_BOS)
      device->bo_sizes = _mesa_hash_table_create(NULL, _mesa_hash_string, _mesa_key_string_equal);

#ifndef TU_USE_KGSL
   vk_device_set_drm_fd(&device->vk, device->fd);
#endif

   for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_create =
         &pCreateInfo->pQueueCreateInfos[i];
      uint32_t qfi = queue_create->queueFamilyIndex;
      device->queues[qfi] = vk_alloc(
         &device->vk.alloc, queue_create->queueCount * sizeof(struct tu_queue),
         8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!device->queues[qfi]) {
         result = vk_startup_errorf(physical_device->instance,
                                    VK_ERROR_OUT_OF_HOST_MEMORY,
                                    "OOM");
         goto fail_queues;
      }

      memset(device->queues[qfi], 0,
             queue_create->queueCount * sizeof(struct tu_queue));

      device->queue_count[qfi] = queue_create->queueCount;

      for (unsigned q = 0; q < queue_create->queueCount; q++) {
         result = tu_queue_init(device, &device->queues[qfi][q], q,
                                queue_create, global_priority_query);
         if (result != VK_SUCCESS) {
            device->queue_count[qfi] = q;
            goto fail_queues;
         }
      }
   }

   device->compiler =
      ir3_compiler_create(NULL, &physical_device->dev_id,
                          &(struct ir3_compiler_options) {
                              .robust_buffer_access2 = robust_buffer_access2,
                              .push_ubo_with_preamble = true,
                              .disable_cache = true,
                           });
   if (!device->compiler) {
      result = vk_startup_errorf(physical_device->instance,
                                 VK_ERROR_INITIALIZATION_FAILED,
                                 "failed to initialize ir3 compiler");
      goto fail_queues;
   }

   /* Initialize sparse array for refcounting imported BOs */
   util_sparse_array_init(&device->bo_map, sizeof(struct tu_bo), 512);

   /* initial sizes, these will increase if there is overflow */
   device->vsc_draw_strm_pitch = 0x1000 + VSC_PAD;
   device->vsc_prim_strm_pitch = 0x4000 + VSC_PAD;

   uint32_t global_size = sizeof(struct tu6_global);
   if (custom_border_colors)
      global_size += TU_BORDER_COLOR_COUNT * sizeof(struct bcolor_entry);

   tu_bo_suballocator_init(&device->pipeline_suballoc, device,
                           128 * 1024, TU_BO_ALLOC_GPU_READ_ONLY | TU_BO_ALLOC_ALLOW_DUMP);
   tu_bo_suballocator_init(&device->autotune_suballoc, device,
                           128 * 1024, 0);

   result = tu_bo_init_new(device, &device->global_bo, global_size,
                           TU_BO_ALLOC_ALLOW_DUMP, "global");
   if (result != VK_SUCCESS) {
      vk_startup_errorf(device->instance, result, "BO init");
      goto fail_global_bo;
   }

   result = tu_bo_map(device, device->global_bo);
   if (result != VK_SUCCESS) {
      vk_startup_errorf(device->instance, result, "BO map");
      goto fail_global_bo_map;
   }

   struct tu6_global *global = device->global_bo->map;
   tu_init_clear_blit_shaders(device);
   global->predicate = 0;
   global->vtx_stats_query_not_running = 1;
   global->dbg_one = (uint32_t)-1;
   global->dbg_gmem_total_loads = 0;
   global->dbg_gmem_taken_loads = 0;
   global->dbg_gmem_total_stores = 0;
   global->dbg_gmem_taken_stores = 0;
   for (int i = 0; i < TU_BORDER_COLOR_BUILTIN; i++) {
      VkClearColorValue border_color = vk_border_color_value(i);
      tu6_pack_border_color(&global->bcolor_builtin[i], &border_color,
                            vk_border_color_is_int(i));
   }

   /* initialize to ones so ffs can be used to find unused slots */
   BITSET_ONES(device->custom_border_color);

   result = tu_init_dynamic_rendering(device);
   if (result != VK_SUCCESS) {
      vk_startup_errorf(device->instance, result, "dynamic rendering");
      goto fail_dynamic_rendering;
   }

   struct vk_pipeline_cache_create_info pcc_info = { };
   device->mem_cache = vk_pipeline_cache_create(&device->vk, &pcc_info,
                                                false);
   if (!device->mem_cache) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      vk_startup_errorf(device->instance, result, "create pipeline cache failed");
      goto fail_pipeline_cache;
   }

   if (perf_query_pools) {
      /* Prepare command streams setting pass index to the PERF_CNTRS_REG
       * from 0 to 31. One of these will be picked up at cmd submit time
       * when the perf query is executed.
       */
      struct tu_cs *cs;

      if (!(device->perfcntrs_pass_cs = calloc(1, sizeof(struct tu_cs)))) {
         result = vk_startup_errorf(device->instance,
               VK_ERROR_OUT_OF_HOST_MEMORY, "OOM");
         goto fail_perfcntrs_pass_alloc;
      }

      device->perfcntrs_pass_cs_entries = calloc(32, sizeof(struct tu_cs_entry));
      if (!device->perfcntrs_pass_cs_entries) {
         result = vk_startup_errorf(device->instance,
               VK_ERROR_OUT_OF_HOST_MEMORY, "OOM");
         goto fail_perfcntrs_pass_entries_alloc;
      }

      cs = device->perfcntrs_pass_cs;
      tu_cs_init(cs, device, TU_CS_MODE_SUB_STREAM, 96, "perfcntrs cs");

      for (unsigned i = 0; i < 32; i++) {
         struct tu_cs sub_cs;

         result = tu_cs_begin_sub_stream(cs, 3, &sub_cs);
         if (result != VK_SUCCESS) {
            vk_startup_errorf(device->instance, result,
                  "failed to allocate commands streams");
            goto fail_prepare_perfcntrs_pass_cs;
         }

         tu_cs_emit_regs(&sub_cs, A6XX_CP_SCRATCH_REG(PERF_CNTRS_REG, 1 << i));
         tu_cs_emit_pkt7(&sub_cs, CP_WAIT_FOR_ME, 0);

         device->perfcntrs_pass_cs_entries[i] = tu_cs_end_sub_stream(cs, &sub_cs);
      }
   }

   /* Initialize a condition variable for timeline semaphore */
   pthread_condattr_t condattr;
   if (pthread_condattr_init(&condattr) != 0) {
      result = vk_startup_errorf(physical_device->instance,
                                 VK_ERROR_INITIALIZATION_FAILED,
                                 "pthread condattr init");
      goto fail_timeline_cond;
   }
   if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_startup_errorf(physical_device->instance,
                                 VK_ERROR_INITIALIZATION_FAILED,
                                 "pthread condattr clock setup");
      goto fail_timeline_cond;
   }
   if (pthread_cond_init(&device->timeline_cond, &condattr) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_startup_errorf(physical_device->instance,
                                 VK_ERROR_INITIALIZATION_FAILED,
                                 "pthread cond init");
      goto fail_timeline_cond;
   }
   pthread_condattr_destroy(&condattr);

   result = tu_autotune_init(&device->autotune, device);
   if (result != VK_SUCCESS) {
      goto fail_timeline_cond;
   }

   for (unsigned i = 0; i < ARRAY_SIZE(device->scratch_bos); i++)
      mtx_init(&device->scratch_bos[i].construct_mtx, mtx_plain);

   mtx_init(&device->fiber_pvtmem_bo.mtx, mtx_plain);
   mtx_init(&device->wave_pvtmem_bo.mtx, mtx_plain);

   mtx_init(&device->mutex, mtx_plain);

   device->use_z24uint_s8uint =
      physical_device->info->a6xx.has_z24uint_s8uint &&
      !border_color_without_format;

   tu_gpu_tracepoint_config_variable();

   device->submit_count = 0;
   u_trace_context_init(&device->trace_context, device,
                     tu_trace_create_ts_buffer,
                     tu_trace_destroy_ts_buffer,
                     tu_trace_record_ts,
                     tu_trace_read_ts,
                     tu_trace_delete_flush_data);

   tu_breadcrumbs_init(device);

   *pDevice = tu_device_to_handle(device);
   return VK_SUCCESS;

fail_timeline_cond:
fail_prepare_perfcntrs_pass_cs:
   free(device->perfcntrs_pass_cs_entries);
   tu_cs_finish(device->perfcntrs_pass_cs);
fail_perfcntrs_pass_entries_alloc:
   free(device->perfcntrs_pass_cs);
fail_perfcntrs_pass_alloc:
   vk_pipeline_cache_destroy(device->mem_cache, &device->vk.alloc);
fail_pipeline_cache:
   tu_destroy_dynamic_rendering(device);
fail_dynamic_rendering:
   tu_destroy_clear_blit_shaders(device);
fail_global_bo_map:
   tu_bo_finish(device, device->global_bo);
   vk_free(&device->vk.alloc, device->bo_list);
fail_global_bo:
   ir3_compiler_destroy(device->compiler);
   util_sparse_array_finish(&device->bo_map);

fail_queues:
   for (unsigned i = 0; i < TU_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         tu_queue_finish(&device->queues[i][q]);
      if (device->queues[i])
         vk_free(&device->vk.alloc, device->queues[i]);
   }

   u_rwlock_destroy(&device->dma_bo_lock);
   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);

   if (!device)
      return;

   tu_breadcrumbs_finish(device);

   u_trace_context_fini(&device->trace_context);

   for (unsigned i = 0; i < TU_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         tu_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_free(&device->vk.alloc, device->queues[i]);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(device->scratch_bos); i++) {
      if (device->scratch_bos[i].initialized)
         tu_bo_finish(device, device->scratch_bos[i].bo);
   }

   if (device->fiber_pvtmem_bo.bo)
      tu_bo_finish(device, device->fiber_pvtmem_bo.bo);
   
   if (device->wave_pvtmem_bo.bo)
      tu_bo_finish(device, device->wave_pvtmem_bo.bo);

   tu_destroy_clear_blit_shaders(device);

   tu_destroy_dynamic_rendering(device);

   ir3_compiler_destroy(device->compiler);

   vk_pipeline_cache_destroy(device->mem_cache, &device->vk.alloc);

   if (device->perfcntrs_pass_cs) {
      free(device->perfcntrs_pass_cs_entries);
      tu_cs_finish(device->perfcntrs_pass_cs);
      free(device->perfcntrs_pass_cs);
   }

   tu_autotune_fini(&device->autotune, device);

   tu_bo_suballocator_finish(&device->pipeline_suballoc);
   tu_bo_suballocator_finish(&device->autotune_suballoc);

   util_sparse_array_finish(&device->bo_map);
   u_rwlock_destroy(&device->dma_bo_lock);

   pthread_cond_destroy(&device->timeline_cond);
   _mesa_hash_table_destroy(device->bo_sizes, NULL);
   vk_free(&device->vk.alloc, device->bo_list);
   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
}

VkResult
tu_get_scratch_bo(struct tu_device *dev, uint64_t size, struct tu_bo **bo)
{
   unsigned size_log2 = MAX2(util_logbase2_ceil64(size), MIN_SCRATCH_BO_SIZE_LOG2);
   unsigned index = size_log2 - MIN_SCRATCH_BO_SIZE_LOG2;
   assert(index < ARRAY_SIZE(dev->scratch_bos));

   for (unsigned i = index; i < ARRAY_SIZE(dev->scratch_bos); i++) {
      if (p_atomic_read(&dev->scratch_bos[i].initialized)) {
         /* Fast path: just return the already-allocated BO. */
         *bo = dev->scratch_bos[i].bo;
         return VK_SUCCESS;
      }
   }

   /* Slow path: actually allocate the BO. We take a lock because the process
    * of allocating it is slow, and we don't want to block the CPU while it
    * finishes.
   */
   mtx_lock(&dev->scratch_bos[index].construct_mtx);

   /* Another thread may have allocated it already while we were waiting on
    * the lock. We need to check this in order to avoid double-allocating.
    */
   if (dev->scratch_bos[index].initialized) {
      mtx_unlock(&dev->scratch_bos[index].construct_mtx);
      *bo = dev->scratch_bos[index].bo;
      return VK_SUCCESS;
   }

   unsigned bo_size = 1ull << size_log2;
   VkResult result = tu_bo_init_new(dev, &dev->scratch_bos[index].bo, bo_size,
                                    TU_BO_ALLOC_NO_FLAGS, "scratch");
   if (result != VK_SUCCESS) {
      mtx_unlock(&dev->scratch_bos[index].construct_mtx);
      return result;
   }

   p_atomic_set(&dev->scratch_bos[index].initialized, true);

   mtx_unlock(&dev->scratch_bos[index].construct_mtx);

   *bo = dev->scratch_bos[index].bo;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                    VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

/* Only used for kgsl since drm started using common implementation */
#ifdef TU_USE_KGSL
VKAPI_ATTR VkResult VKAPI_CALL
tu_QueueWaitIdle(VkQueue _queue)
{
   TU_FROM_HANDLE(tu_queue, queue, _queue);

   if (vk_device_is_lost(&queue->device->vk))
      return VK_ERROR_DEVICE_LOST;

   if (queue->fence < 0)
      return VK_SUCCESS;

   struct pollfd fds = { .fd = queue->fence, .events = POLLIN };
   int ret;
   do {
      ret = poll(&fds, 1, -1);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   /* TODO: otherwise set device lost ? */
   assert(ret == 1 && !(fds.revents & (POLLERR | POLLNVAL)));

   close(queue->fence);
   queue->fence = -1;
   return VK_SUCCESS;
}
#endif

VKAPI_ATTR VkResult VKAPI_CALL
tu_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                        uint32_t *pPropertyCount,
                                        VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &tu_instance_extensions_supported, pPropertyCount, pProperties);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
tu_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   TU_FROM_HANDLE(tu_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk,
                                    &tu_instance_entrypoints,
                                    pName);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return tu_GetInstanceProcAddr(instance, pName);
}

/* With version 4+ of the loader interface the ICD should expose
 * vk_icdGetPhysicalDeviceProcAddr()
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance  _instance,
                                const char* pName);

PFN_vkVoidFunction
vk_icdGetPhysicalDeviceProcAddr(VkInstance  _instance,
                                const char* pName)
{
   TU_FROM_HANDLE(tu_instance, instance, _instance);

   return vk_instance_get_physical_device_proc_addr(&instance->vk, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_AllocateMemory(VkDevice _device,
                  const VkMemoryAllocateInfo *pAllocateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkDeviceMemory *pMem)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_device_memory *mem;
   VkResult result;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   if (pAllocateInfo->allocationSize == 0) {
      /* Apparently, this is allowed */
      *pMem = VK_NULL_HANDLE;
      return VK_SUCCESS;
   }

   struct tu_memory_heap *mem_heap = &device->physical_device->heap;
   uint64_t mem_heap_used = p_atomic_read(&mem_heap->used);
   if (mem_heap_used > mem_heap->size)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   mem = vk_object_alloc(&device->vk, pAllocator, sizeof(*mem),
                         VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (mem == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (fd_info) {
      assert(fd_info->handleType ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
             fd_info->handleType ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      /*
       * TODO Importing the same fd twice gives us the same handle without
       * reference counting.  We need to maintain a per-instance handle-to-bo
       * table and add reference count to tu_bo.
       */
      result = tu_bo_init_dmabuf(device, &mem->bo,
                                 pAllocateInfo->allocationSize, fd_info->fd);
      if (result == VK_SUCCESS) {
         /* take ownership and close the fd */
         close(fd_info->fd);
      }
   } else {
      uint64_t client_address = 0;
      enum tu_bo_alloc_flags alloc_flags = TU_BO_ALLOC_NO_FLAGS;

      const VkMemoryOpaqueCaptureAddressAllocateInfo *replay_info =
         vk_find_struct_const(pAllocateInfo->pNext,
                              MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO);
      if (replay_info && replay_info->opaqueCaptureAddress) {
         client_address = replay_info->opaqueCaptureAddress;
         alloc_flags |= TU_BO_ALLOC_REPLAYABLE;
      }

      const VkMemoryAllocateFlagsInfo *flags_info = vk_find_struct_const(
         pAllocateInfo->pNext, MEMORY_ALLOCATE_FLAGS_INFO);
      if (flags_info &&
          (flags_info->flags &
           VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT)) {
         alloc_flags |= TU_BO_ALLOC_REPLAYABLE;
      }

      char name[64] = "vkAllocateMemory()";
      if (device->bo_sizes)
         snprintf(name, ARRAY_SIZE(name), "vkAllocateMemory(%ldkb)",
                  (long)DIV_ROUND_UP(pAllocateInfo->allocationSize, 1024));
      result = tu_bo_init_new_explicit_iova(
         device, &mem->bo, pAllocateInfo->allocationSize, client_address,
         alloc_flags, name);
   }

   if (result == VK_SUCCESS) {
      mem_heap_used = p_atomic_add_return(&mem_heap->used, mem->bo->size);
      if (mem_heap_used > mem_heap->size) {
         p_atomic_add(&mem_heap->used, -mem->bo->size);
         tu_bo_finish(device, mem->bo);
         result = vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "Out of heap memory");
      }
   }

   if (result != VK_SUCCESS) {
      vk_object_free(&device->vk, pAllocator, mem);
      return result;
   }

   /* Track in the device whether our BO list contains any implicit-sync BOs, so
    * we can suppress implicit sync on non-WSI usage.
    */
   const struct wsi_memory_allocate_info *wsi_info =
      vk_find_struct_const(pAllocateInfo->pNext, WSI_MEMORY_ALLOCATE_INFO_MESA);
   if (wsi_info && wsi_info->implicit_sync) {
      mtx_lock(&device->bo_mutex);
      if (!mem->bo->implicit_sync) {
         mem->bo->implicit_sync = true;
         device->implicit_sync_bo_count++;
      }
      mtx_unlock(&device->bo_mutex);
   }

   *pMem = tu_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_FreeMemory(VkDevice _device,
              VkDeviceMemory _mem,
              const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   p_atomic_add(&device->physical_device->heap.used, -mem->bo->size);
   tu_bo_finish(device, mem->bo);
   vk_object_free(&device->vk, pAllocator, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_MapMemory(VkDevice _device,
             VkDeviceMemory _memory,
             VkDeviceSize offset,
             VkDeviceSize size,
             VkMemoryMapFlags flags,
             void **ppData)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_device_memory, mem, _memory);
   VkResult result;

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (!mem->bo->map) {
      result = tu_bo_map(device, mem->bo);
      if (result != VK_SUCCESS)
         return result;
   }

   *ppData = mem->bo->map + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_UnmapMemory(VkDevice _device, VkDeviceMemory _memory)
{
   /* TODO: unmap here instead of waiting for FreeMemory */
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_FlushMappedMemoryRanges(VkDevice _device,
                           uint32_t memoryRangeCount,
                           const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_InvalidateMappedMemoryRanges(VkDevice _device,
                                uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

static void
tu_get_buffer_memory_requirements(uint64_t size,
                                  VkMemoryRequirements2 *pMemoryRequirements)
{
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements) {
      .memoryTypeBits = 1,
      .alignment = 64,
      .size = MAX2(align64(size, 64), size),
   };

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

VKAPI_ATTR void VKAPI_CALL
tu_GetBufferMemoryRequirements2(
   VkDevice device,
   const VkBufferMemoryRequirementsInfo2 *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements)
{
   TU_FROM_HANDLE(tu_buffer, buffer, pInfo->buffer);

   tu_get_buffer_memory_requirements(buffer->vk.size, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
tu_GetDeviceBufferMemoryRequirements(
   VkDevice device,
   const VkDeviceBufferMemoryRequirements *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements)
{
   tu_get_buffer_memory_requirements(pInfo->pCreateInfo->size, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
tu_GetDeviceMemoryCommitment(VkDevice device,
                             VkDeviceMemory memory,
                             VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_BindBufferMemory2(VkDevice device,
                     uint32_t bindInfoCount,
                     const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      TU_FROM_HANDLE(tu_device_memory, mem, pBindInfos[i].memory);
      TU_FROM_HANDLE(tu_buffer, buffer, pBindInfos[i].buffer);

      if (mem) {
         buffer->bo = mem->bo;
         buffer->iova = mem->bo->iova + pBindInfos[i].memoryOffset;
      } else {
         buffer->bo = NULL;
      }
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_BindImageMemory2(VkDevice device,
                    uint32_t bindInfoCount,
                    const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      TU_FROM_HANDLE(tu_image, image, pBindInfos[i].image);
      TU_FROM_HANDLE(tu_device_memory, mem, pBindInfos[i].memory);

      if (mem) {
         image->bo = mem->bo;
         image->iova = mem->bo->iova + pBindInfos[i].memoryOffset;
      } else {
         image->bo = NULL;
         image->iova = 0;
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_QueueBindSparse(VkQueue _queue,
                   uint32_t bindInfoCount,
                   const VkBindSparseInfo *pBindInfo,
                   VkFence _fence)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateEvent(VkDevice _device,
               const VkEventCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkEvent *pEvent)
{
   TU_FROM_HANDLE(tu_device, device, _device);

   struct tu_event *event =
         vk_object_alloc(&device->vk, pAllocator, sizeof(*event),
                         VK_OBJECT_TYPE_EVENT);
   if (!event)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = tu_bo_init_new(device, &event->bo, 0x1000,
                                    TU_BO_ALLOC_NO_FLAGS, "event");
   if (result != VK_SUCCESS)
      goto fail_alloc;

   result = tu_bo_map(device, event->bo);
   if (result != VK_SUCCESS)
      goto fail_map;

   *pEvent = tu_event_to_handle(event);

   return VK_SUCCESS;

fail_map:
   tu_bo_finish(device, event->bo);
fail_alloc:
   vk_object_free(&device->vk, pAllocator, event);
   return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyEvent(VkDevice _device,
                VkEvent _event,
                const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_event, event, _event);

   if (!event)
      return;

   tu_bo_finish(device, event->bo);
   vk_object_free(&device->vk, pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetEventStatus(VkDevice _device, VkEvent _event)
{
   TU_FROM_HANDLE(tu_event, event, _event);

   if (*(uint64_t*) event->bo->map == 1)
      return VK_EVENT_SET;
   return VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_SetEvent(VkDevice _device, VkEvent _event)
{
   TU_FROM_HANDLE(tu_event, event, _event);
   *(uint64_t*) event->bo->map = 1;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_ResetEvent(VkDevice _device, VkEvent _event)
{
   TU_FROM_HANDLE(tu_event, event, _event);
   *(uint64_t*) event->bo->map = 0;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateBuffer(VkDevice _device,
                const VkBufferCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkBuffer *pBuffer)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_buffer *buffer;

   buffer = vk_buffer_create(&device->vk, pCreateInfo, pAllocator,
         sizeof(*buffer));
   if (buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pBuffer = tu_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyBuffer(VkDevice _device,
                 VkBuffer _buffer,
                 const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateFramebuffer(VkDevice _device,
                     const VkFramebufferCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkFramebuffer *pFramebuffer)
{
   TU_FROM_HANDLE(tu_device, device, _device);

   if (unlikely(device->instance->debug_flags & TU_DEBUG_DYNAMIC))
      return vk_common_CreateFramebuffer(_device, pCreateInfo, pAllocator,
                                         pFramebuffer);

   TU_FROM_HANDLE(tu_render_pass, pass, pCreateInfo->renderPass);
   struct tu_framebuffer *framebuffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

   bool imageless = pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;

   size_t size = sizeof(*framebuffer);
   if (!imageless)
      size += sizeof(struct tu_attachment_info) * pCreateInfo->attachmentCount;
   framebuffer = vk_object_alloc(&device->vk, pAllocator, size,
                                 VK_OBJECT_TYPE_FRAMEBUFFER);
   if (framebuffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   framebuffer->attachment_count = pCreateInfo->attachmentCount;
   framebuffer->width = pCreateInfo->width;
   framebuffer->height = pCreateInfo->height;
   framebuffer->layers = pCreateInfo->layers;

   if (!imageless) {
      for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
         VkImageView _iview = pCreateInfo->pAttachments[i];
         struct tu_image_view *iview = tu_image_view_from_handle(_iview);
         framebuffer->attachments[i].attachment = iview;
      }
   }

   tu_framebuffer_tiling_config(framebuffer, device, pass);

   *pFramebuffer = tu_framebuffer_to_handle(framebuffer);
   return VK_SUCCESS;
}

void
tu_setup_dynamic_framebuffer(struct tu_cmd_buffer *cmd_buffer,
                             const VkRenderingInfo *pRenderingInfo)
{
   struct tu_render_pass *pass = &cmd_buffer->dynamic_pass;
   struct tu_framebuffer *framebuffer = &cmd_buffer->dynamic_framebuffer;

   framebuffer->attachment_count = pass->attachment_count;
   framebuffer->width = pRenderingInfo->renderArea.offset.x +
      pRenderingInfo->renderArea.extent.width;
   framebuffer->height = pRenderingInfo->renderArea.offset.y +
      pRenderingInfo->renderArea.extent.height;
   framebuffer->layers = pRenderingInfo->layerCount;

   tu_framebuffer_tiling_config(framebuffer, cmd_buffer->device, pass);
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroyFramebuffer(VkDevice _device,
                      VkFramebuffer _fb,
                      const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);

   if (unlikely(device->instance->debug_flags & TU_DEBUG_DYNAMIC)) {
      vk_common_DestroyFramebuffer(_device, _fb, pAllocator);
      return;
   }

   TU_FROM_HANDLE(tu_framebuffer, fb, _fb);

   if (!fb)
      return;

   vk_object_free(&device->vk, pAllocator, fb);
}

static void
tu_init_sampler(struct tu_device *device,
                struct tu_sampler *sampler,
                const VkSamplerCreateInfo *pCreateInfo)
{
   const struct VkSamplerReductionModeCreateInfo *reduction =
      vk_find_struct_const(pCreateInfo->pNext, SAMPLER_REDUCTION_MODE_CREATE_INFO);
   const struct VkSamplerYcbcrConversionInfo *ycbcr_conversion =
      vk_find_struct_const(pCreateInfo->pNext,  SAMPLER_YCBCR_CONVERSION_INFO);
   const VkSamplerCustomBorderColorCreateInfoEXT *custom_border_color =
      vk_find_struct_const(pCreateInfo->pNext, SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT);
   /* for non-custom border colors, the VK enum is translated directly to an offset in
    * the border color buffer. custom border colors are located immediately after the
    * builtin colors, and thus an offset of TU_BORDER_COLOR_BUILTIN is added.
    */
   uint32_t border_color = (unsigned) pCreateInfo->borderColor;
   if (pCreateInfo->borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ||
       pCreateInfo->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT) {
      mtx_lock(&device->mutex);
      border_color = BITSET_FFS(device->custom_border_color) - 1;
      assert(border_color < TU_BORDER_COLOR_COUNT);
      BITSET_CLEAR(device->custom_border_color, border_color);
      mtx_unlock(&device->mutex);

      VkClearColorValue color = custom_border_color->customBorderColor;
      if (custom_border_color->format == VK_FORMAT_D24_UNORM_S8_UINT &&
          pCreateInfo->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT &&
          device->use_z24uint_s8uint) {
         /* When sampling stencil using the special Z24UINT_S8UINT format, the
          * border color is in the second component. Note: if
          * customBorderColorWithoutFormat is enabled, we may miss doing this
          * here if the format isn't specified, which is why we don't use that
          * format.
          */
         color.uint32[1] = color.uint32[0];
      }

      tu6_pack_border_color(device->global_bo->map + gb_offset(bcolor[border_color]),
                            &color,
                            pCreateInfo->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT);
      border_color += TU_BORDER_COLOR_BUILTIN;
   }

   unsigned aniso = pCreateInfo->anisotropyEnable ?
      util_last_bit(MIN2((uint32_t)pCreateInfo->maxAnisotropy >> 1, 8)) : 0;
   bool miplinear = (pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
   float min_lod = CLAMP(pCreateInfo->minLod, 0.0f, 4095.0f / 256.0f);
   float max_lod = CLAMP(pCreateInfo->maxLod, 0.0f, 4095.0f / 256.0f);

   sampler->descriptor[0] =
      COND(miplinear, A6XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR) |
      A6XX_TEX_SAMP_0_XY_MAG(tu6_tex_filter(pCreateInfo->magFilter, aniso)) |
      A6XX_TEX_SAMP_0_XY_MIN(tu6_tex_filter(pCreateInfo->minFilter, aniso)) |
      A6XX_TEX_SAMP_0_ANISO(aniso) |
      A6XX_TEX_SAMP_0_WRAP_S(tu6_tex_wrap(pCreateInfo->addressModeU)) |
      A6XX_TEX_SAMP_0_WRAP_T(tu6_tex_wrap(pCreateInfo->addressModeV)) |
      A6XX_TEX_SAMP_0_WRAP_R(tu6_tex_wrap(pCreateInfo->addressModeW)) |
      A6XX_TEX_SAMP_0_LOD_BIAS(pCreateInfo->mipLodBias);
   sampler->descriptor[1] =
      COND(pCreateInfo->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT,
           A6XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF) |
      COND(pCreateInfo->unnormalizedCoordinates, A6XX_TEX_SAMP_1_UNNORM_COORDS) |
      A6XX_TEX_SAMP_1_MIN_LOD(min_lod) |
      A6XX_TEX_SAMP_1_MAX_LOD(max_lod) |
      COND(pCreateInfo->compareEnable,
           A6XX_TEX_SAMP_1_COMPARE_FUNC(tu6_compare_func(pCreateInfo->compareOp)));
   sampler->descriptor[2] = A6XX_TEX_SAMP_2_BCOLOR(border_color);
   sampler->descriptor[3] = 0;

   if (reduction) {
      sampler->descriptor[2] |= A6XX_TEX_SAMP_2_REDUCTION_MODE(
         tu6_reduction_mode(reduction->reductionMode));
   }

   sampler->ycbcr_sampler = ycbcr_conversion ?
      tu_sampler_ycbcr_conversion_from_handle(ycbcr_conversion->conversion) : NULL;

   if (sampler->ycbcr_sampler &&
       sampler->ycbcr_sampler->chroma_filter == VK_FILTER_LINEAR) {
      sampler->descriptor[2] |= A6XX_TEX_SAMP_2_CHROMA_LINEAR;
   }

   /* TODO:
    * A6XX_TEX_SAMP_1_MIPFILTER_LINEAR_FAR disables mipmapping, but vk has no NONE mipfilter?
    */
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_CreateSampler(VkDevice _device,
                 const VkSamplerCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkSampler *pSampler)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_sampler *sampler;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

   sampler = vk_object_alloc(&device->vk, pAllocator, sizeof(*sampler),
                             VK_OBJECT_TYPE_SAMPLER);
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   tu_init_sampler(device, sampler, pCreateInfo);
   *pSampler = tu_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroySampler(VkDevice _device,
                  VkSampler _sampler,
                  const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_sampler, sampler, _sampler);
   uint32_t border_color;

   if (!sampler)
      return;

   border_color = (sampler->descriptor[2] & A6XX_TEX_SAMP_2_BCOLOR__MASK) >> A6XX_TEX_SAMP_2_BCOLOR__SHIFT;
   if (border_color >= TU_BORDER_COLOR_BUILTIN) {
      border_color -= TU_BORDER_COLOR_BUILTIN;
      /* if the sampler had a custom border color, free it. TODO: no lock */
      mtx_lock(&device->mutex);
      assert(!BITSET_TEST(device->custom_border_color, border_color));
      BITSET_SET(device->custom_border_color, border_color);
      mtx_unlock(&device->mutex);
   }

   vk_object_free(&device->vk, pAllocator, sampler);
}

/* vk_icd.h does not declare this function, so we declare it here to
 * suppress Wmissing-prototypes.
 */
PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion);

PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
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
    *       - The ICD must statically expose no other Vulkan symbol unless it
    * is linked with -Bsymbolic.
    *       - Each dispatchable Vulkan handle created by the ICD must be
    *         a pointer to a struct whose first member is VK_LOADER_DATA. The
    *         ICD must initialize VK_LOADER_DATA.loadMagic to
    * ICD_LOADER_MAGIC.
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

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetMemoryFdKHR(VkDevice _device,
                  const VkMemoryGetFdInfoKHR *pGetFdInfo,
                  int *pFd)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   /* At the moment, we support only the below handle types. */
   assert(pGetFdInfo->handleType ==
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
          pGetFdInfo->handleType ==
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int prime_fd = tu_bo_export_dmabuf(device, memory->bo);
   if (prime_fd < 0)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetMemoryFdPropertiesKHR(VkDevice _device,
                            VkExternalMemoryHandleTypeFlagBits handleType,
                            int fd,
                            VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   assert(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   pMemoryFdProperties->memoryTypeBits = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
tu_GetDeviceGroupPeerMemoryFeatures(
   VkDevice device,
   uint32_t heapIndex,
   uint32_t localDeviceIndex,
   uint32_t remoteDeviceIndex,
   VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   assert(localDeviceIndex == remoteDeviceIndex);

   *pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT |
                          VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
                          VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT |
                          VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}

VKAPI_ATTR void VKAPI_CALL
tu_GetPhysicalDeviceMultisamplePropertiesEXT(
   VkPhysicalDevice                            physicalDevice,
   VkSampleCountFlagBits                       samples,
   VkMultisamplePropertiesEXT*                 pMultisampleProperties)
{
   TU_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);

   if (samples <= VK_SAMPLE_COUNT_4_BIT && pdevice->vk.supported_extensions.EXT_sample_locations)
      pMultisampleProperties->maxSampleLocationGridSize = (VkExtent2D){ 1, 1 };
   else
      pMultisampleProperties->maxSampleLocationGridSize = (VkExtent2D){ 0, 0 };
}

VkDeviceAddress
tu_GetBufferDeviceAddress(VkDevice _device,
                          const VkBufferDeviceAddressInfo* pInfo)
{
   TU_FROM_HANDLE(tu_buffer, buffer, pInfo->buffer);

   return buffer->iova;
}

uint64_t tu_GetBufferOpaqueCaptureAddress(
    VkDevice                                    device,
    const VkBufferDeviceAddressInfo*            pInfo)
{
   /* We care only about memory allocation opaque addresses */
   return 0;
}

uint64_t tu_GetDeviceMemoryOpaqueCaptureAddress(
    VkDevice                                    device,
    const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo)
{
   TU_FROM_HANDLE(tu_device_memory, mem, pInfo->memory);
   return mem->bo->iova;
}

struct tu_debug_bos_entry {
   uint32_t count;
   uint64_t size;
   const char *name;
};

const char *
tu_debug_bos_add(struct tu_device *dev, uint64_t size, const char *name)
{
   assert(name);

   if (likely(!dev->bo_sizes))
      return NULL;

   mtx_lock(&dev->bo_mutex);
   struct hash_entry *entry = _mesa_hash_table_search(dev->bo_sizes, name);
   struct tu_debug_bos_entry *debug_bos;

   if (!entry) {
      debug_bos = calloc(1, sizeof(struct tu_debug_bos_entry));
      debug_bos->name = strdup(name);
      _mesa_hash_table_insert(dev->bo_sizes, debug_bos->name, debug_bos);
   } else {
      debug_bos = entry->data;
   }

   debug_bos->count++;
   debug_bos->size += align(size, 4096);
   mtx_unlock(&dev->bo_mutex);

   return debug_bos->name;
}

void
tu_debug_bos_del(struct tu_device *dev, struct tu_bo *bo)
{
   if (likely(!dev->bo_sizes) || !bo->name)
      return;

   mtx_lock(&dev->bo_mutex);
   struct hash_entry *entry =
      _mesa_hash_table_search(dev->bo_sizes, bo->name);
   /* If we're finishing the BO, it should have been added already */
   assert(entry);

   struct tu_debug_bos_entry *debug_bos = entry->data;
   debug_bos->count--;
   debug_bos->size -= align(bo->size, 4096);
   if (!debug_bos->count) {
      _mesa_hash_table_remove(dev->bo_sizes, entry);
      free((void *) debug_bos->name);
      free(debug_bos);
   }
   mtx_unlock(&dev->bo_mutex);
}

static int debug_bos_count_compare(const void *in_a, const void *in_b)
{
   struct tu_debug_bos_entry *a = *(struct tu_debug_bos_entry **)in_a;
   struct tu_debug_bos_entry *b = *(struct tu_debug_bos_entry **)in_b;
   return a->count - b->count;
}

void
tu_debug_bos_print_stats(struct tu_device *dev)
{
   if (likely(!dev->bo_sizes))
      return;

   mtx_lock(&dev->bo_mutex);

   /* Put the HT's sizes data in an array so we can sort by number of allocations. */
   struct util_dynarray dyn;
   util_dynarray_init(&dyn, NULL);

   uint32_t size = 0;
   uint32_t count = 0;
   hash_table_foreach(dev->bo_sizes, entry)
   {
      struct tu_debug_bos_entry *debug_bos = (void *) entry->data;
      util_dynarray_append(&dyn, struct tu_debug_bos_entry *, debug_bos);
      size += debug_bos->size / 1024;
      count += debug_bos->count;
   }

   qsort(dyn.data,
         util_dynarray_num_elements(&dyn, struct tu_debug_bos_entry *),
         sizeof(struct tu_debug_bos_entryos_entry *), debug_bos_count_compare);

   util_dynarray_foreach(&dyn, struct tu_debug_bos_entry *, entryp)
   {
      struct tu_debug_bos_entry *debug_bos = *entryp;
      mesa_logi("%30s: %4d bos, %lld kb\n", debug_bos->name, debug_bos->count,
                (long long) (debug_bos->size / 1024));
   }

   mesa_logi("submitted %d bos (%d MB)\n", count, DIV_ROUND_UP(size, 1024));

   util_dynarray_fini(&dyn);

   mtx_unlock(&dev->bo_mutex);
}
