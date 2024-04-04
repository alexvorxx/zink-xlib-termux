/*
 * Copyright © 2015 Intel Corporation
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

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#include <sys/sysmacros.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/drm.h"
#include <xf86drm.h>

#include "anv_private.h"
#include "anv_measure.h"
#include "util/u_debug.h"
#include "util/build_id.h"
#include "util/disk_cache.h"
#include "util/mesa-sha1.h"
#include "util/os_file.h"
#include "util/os_misc.h"
#include "util/u_atomic.h"
#include "util/u_string.h"
#include "util/driconf.h"
#include "git_sha1.h"
#include "vk_util.h"
#include "vk_deferred_operation.h"
#include "vk_drm_syncobj.h"
#include "common/intel_aux_map.h"
#include "common/intel_defines.h"
#include "common/intel_uuid.h"
#include "perf/intel_perf.h"

#include "genxml/gen7_pack.h"
#include "genxml/genX_bits.h"

static const driOptionDescription anv_dri_options[] = {
   DRI_CONF_SECTION_PERFORMANCE
      DRI_CONF_ADAPTIVE_SYNC(true)
      DRI_CONF_VK_X11_OVERRIDE_MIN_IMAGE_COUNT(0)
      DRI_CONF_VK_X11_STRICT_IMAGE_COUNT(false)
      DRI_CONF_VK_KHR_PRESENT_WAIT(false)
      DRI_CONF_VK_XWAYLAND_WAIT_READY(true)
      DRI_CONF_ANV_ASSUME_FULL_SUBGROUPS(false)
      DRI_CONF_ANV_SAMPLE_MASK_OUT_OPENGL_BEHAVIOUR(false)
      DRI_CONF_ANV_FP64_WORKAROUND_ENABLED(false)
   DRI_CONF_SECTION_END

   DRI_CONF_SECTION_DEBUG
      DRI_CONF_ALWAYS_FLUSH_CACHE(false)
      DRI_CONF_VK_WSI_FORCE_BGRA8_UNORM_FIRST(false)
      DRI_CONF_LIMIT_TRIG_INPUT_RANGE(false)
   DRI_CONF_SECTION_END

   DRI_CONF_SECTION_QUALITY
      DRI_CONF_PP_LOWER_DEPTH_RANGE_RATE()
   DRI_CONF_SECTION_END
};

/* This is probably far to big but it reflects the max size used for messages
 * in OpenGLs KHR_debug.
 */
#define MAX_DEBUG_MESSAGE_LENGTH    4096

/* The "RAW" clocks on Linux are called "FAST" on FreeBSD */
#if !defined(CLOCK_MONOTONIC_RAW) && defined(CLOCK_MONOTONIC_FAST)
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC_FAST
#endif

static void
compiler_debug_log(void *data, UNUSED unsigned *id, const char *fmt, ...)
{
   char str[MAX_DEBUG_MESSAGE_LENGTH];
   struct anv_device *device = (struct anv_device *)data;
   UNUSED struct anv_instance *instance = device->physical->instance;

   va_list args;
   va_start(args, fmt);
   (void) vsnprintf(str, MAX_DEBUG_MESSAGE_LENGTH, fmt, args);
   va_end(args);

   //vk_logd(VK_LOG_NO_OBJS(&instance->vk), "%s", str);
}

static void
compiler_perf_log(UNUSED void *data, UNUSED unsigned *id, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);

   if (INTEL_DEBUG(DEBUG_PERF))
      mesa_logd_v(fmt, args);

   va_end(args);
}

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
    defined(VK_USE_PLATFORM_XCB_KHR) || \
    defined(VK_USE_PLATFORM_XLIB_KHR) || \
    defined(VK_USE_PLATFORM_DISPLAY_KHR)
#define ANV_USE_WSI_PLATFORM
#endif

#ifdef ANDROID
#define ANV_API_VERSION VK_MAKE_VERSION(1, 1, VK_HEADER_VERSION)
#else
#define ANV_API_VERSION VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)
#endif

VkResult anv_EnumerateInstanceVersion(
    uint32_t*                                   pApiVersion)
{
    *pApiVersion = ANV_API_VERSION;
    return VK_SUCCESS;
}

static const struct vk_instance_extension_table instance_extensions = {
   .KHR_device_group_creation                = true,
   .KHR_external_fence_capabilities          = true,
   .KHR_external_memory_capabilities         = true,
   .KHR_external_semaphore_capabilities      = true,
   .KHR_get_physical_device_properties2      = true,
   .EXT_debug_report                         = true,
   .EXT_debug_utils                          = true,

#ifdef ANV_USE_WSI_PLATFORM
   .KHR_get_surface_capabilities2            = true,
   .KHR_surface                              = true,
   .KHR_surface_protected_capabilities       = true,
   .EXT_swapchain_colorspace                 = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface                      = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface                          = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface                         = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
   .EXT_acquire_xlib_display                 = true,
#endif
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .KHR_display                              = true,
   .KHR_get_display_properties2              = true,
   .EXT_direct_mode_display                  = true,
   .EXT_display_surface_counter              = true,
   .EXT_acquire_drm_display                  = true,
#endif
};

static void
get_device_extensions(const struct anv_physical_device *device,
                      struct vk_device_extension_table *ext)
{
   const bool has_syncobj_wait =
      (device->sync_syncobj_type.features & VK_SYNC_FEATURE_CPU_WAIT) != 0;

   const bool nv_mesh_shading_enabled =
      debug_get_bool_option("ANV_EXPERIMENTAL_NV_MESH_SHADER", false);

   *ext = (struct vk_device_extension_table) {
      .KHR_8bit_storage                      = true,
      .KHR_16bit_storage                     = true,
      .KHR_acceleration_structure            = device->info.has_ray_tracing,
      .KHR_acceleration_structure            = ANV_SUPPORT_RT &&
                                               device->info.has_ray_tracing,
      .KHR_bind_memory2                      = true,
      .KHR_buffer_device_address             = true,
      .KHR_copy_commands2                    = true,
      .KHR_create_renderpass2                = true,
      .KHR_dedicated_allocation              = true,
      .KHR_deferred_host_operations          = true,
      .KHR_depth_stencil_resolve             = true,
      .KHR_descriptor_update_template        = true,
      .KHR_device_group                      = true,
      .KHR_draw_indirect_count               = true,
      .KHR_driver_properties                 = true,
      .KHR_dynamic_rendering                 = true,
      .KHR_external_fence                    = has_syncobj_wait,
      .KHR_external_fence_fd                 = has_syncobj_wait,
      .KHR_external_memory                   = true,
      .KHR_external_memory_fd                = true,
      .KHR_external_semaphore                = true,
      .KHR_external_semaphore_fd             = true,
      .KHR_format_feature_flags2             = true,
      .KHR_fragment_shading_rate             = device->info.ver >= 11,
      .KHR_get_memory_requirements2          = true,
      .KHR_image_format_list                 = true,
      .KHR_imageless_framebuffer             = true,
#ifdef ANV_USE_WSI_PLATFORM
      .KHR_incremental_present               = true,
#endif
      .KHR_maintenance1                      = true,
      .KHR_maintenance2                      = true,
      .KHR_maintenance3                      = true,
      .KHR_maintenance4                      = true,
      .KHR_multiview                         = true,
      .KHR_performance_query =
         device->perf &&
         (device->perf->i915_perf_version >= 3 ||
          INTEL_DEBUG(DEBUG_NO_OACONFIG)) &&
         device->use_call_secondary,
      .KHR_pipeline_executable_properties    = true,
      .KHR_pipeline_library                  = true,
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
      .KHR_push_descriptor                   = true,
      .KHR_ray_query                         =
         ANV_SUPPORT_RT && device->info.has_ray_tracing,
      .KHR_ray_tracing_pipeline              =
         ANV_SUPPORT_RT && device->info.has_ray_tracing,
      .KHR_relaxed_block_layout              = true,
      .KHR_sampler_mirror_clamp_to_edge      = true,
      .KHR_sampler_ycbcr_conversion          = true,
      .KHR_separate_depth_stencil_layouts    = true,
      .KHR_shader_atomic_int64               = true,
      .KHR_shader_clock                      = true,
      .KHR_shader_draw_parameters            = true,
      .KHR_shader_float16_int8               = true,
      .KHR_shader_float_controls             = true,
      .KHR_shader_integer_dot_product        = true,
      .KHR_shader_non_semantic_info          = true,
      .KHR_shader_subgroup_extended_types    = true,
      .KHR_shader_subgroup_uniform_control_flow = true,
      .KHR_shader_terminate_invocation       = true,
      .KHR_spirv_1_4                         = true,
      .KHR_storage_buffer_storage_class      = true,
#ifdef ANV_USE_WSI_PLATFORM
      .KHR_swapchain                         = true,
      .KHR_swapchain_mutable_format          = true,
#endif
      .KHR_synchronization2                  = true,
      .KHR_timeline_semaphore                = true,
      .KHR_uniform_buffer_standard_layout    = true,
      .KHR_variable_pointers                 = true,
      .KHR_vulkan_memory_model               = true,
      .KHR_workgroup_memory_explicit_layout  = true,
      .KHR_zero_initialize_workgroup_memory  = true,
      .EXT_4444_formats                      = true,
      .EXT_border_color_swizzle              = true,
      .EXT_buffer_device_address             = true,
      .EXT_calibrated_timestamps             = device->has_reg_timestamp,
      .EXT_color_write_enable                = true,
      .EXT_conditional_rendering             = true,
      .EXT_conservative_rasterization        = true,
      .EXT_custom_border_color               = true,
      .EXT_depth_clamp_zero_one              = true,
      .EXT_depth_clip_control                = true,
      .EXT_depth_clip_enable                 = true,
      .EXT_descriptor_indexing               = true,
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
      .EXT_display_control                   = true,
#endif
      .EXT_extended_dynamic_state            = true,
      .EXT_extended_dynamic_state2           = true,
      .EXT_extended_dynamic_state3           = true,
      .EXT_external_memory_dma_buf           = true,
      .EXT_external_memory_host              = true,
      .EXT_fragment_shader_interlock         = true,
      .EXT_global_priority                   = device->max_context_priority >=
                                               VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
      .EXT_global_priority_query             = device->max_context_priority >=
                                               VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
      .EXT_host_query_reset                  = true,
      .EXT_image_2d_view_of_3d               = true,
      .EXT_image_robustness                  = true,
      .EXT_image_drm_format_modifier         = true,
      .EXT_image_view_min_lod                = true,
      .EXT_index_type_uint8                  = true,
      .EXT_inline_uniform_block              = true,
      .EXT_line_rasterization                = true,
      /* Enable the extension only if we have support on both the local &
       * system memory
       */
      .EXT_memory_budget                     = (!device->info.has_local_mem ||
                                                device->vram_mappable.available > 0) &&
                                               device->sys.available,
      .EXT_mesh_shader                       = device->info.has_mesh_shading,
      .EXT_mutable_descriptor_type           = true,
      .EXT_non_seamless_cube_map             = true,
      .EXT_pci_bus_info                      = true,
      .EXT_physical_device_drm               = true,
      .EXT_pipeline_creation_cache_control   = true,
      .EXT_pipeline_creation_feedback        = true,
      .EXT_post_depth_coverage               = true,
      .EXT_primitives_generated_query        = true,
      .EXT_primitive_topology_list_restart   = true,
      .EXT_private_data                      = true,
      .EXT_provoking_vertex                  = true,
      .EXT_queue_family_foreign              = true,
      .EXT_robustness2                       = true,
      .EXT_sample_locations                  = true,
      .EXT_sampler_filter_minmax             = true,
      .EXT_scalar_block_layout               = true,
      .EXT_separate_stencil_usage            = true,
      .EXT_shader_atomic_float               = true,
      .EXT_shader_atomic_float2              = true,
      .EXT_shader_demote_to_helper_invocation = true,
      .EXT_shader_module_identifier          = true,
      .EXT_shader_stencil_export             = true,
      .EXT_shader_subgroup_ballot            = true,
      .EXT_shader_subgroup_vote              = true,
      .EXT_shader_viewport_index_layer       = true,
      .EXT_subgroup_size_control             = true,
      .EXT_texel_buffer_alignment            = true,
      .EXT_tooling_info                      = true,
      .EXT_transform_feedback                = true,
      .EXT_vertex_attribute_divisor          = true,
      .EXT_ycbcr_image_arrays                = true,
#ifdef ANDROID
      .ANDROID_external_memory_android_hardware_buffer = true,
      .ANDROID_native_buffer                 = true,
#endif
      .GOOGLE_decorate_string                = true,
      .GOOGLE_hlsl_functionality1            = true,
      .GOOGLE_user_type                      = true,
      .INTEL_performance_query               = device->perf &&
                                               device->perf->i915_perf_version >= 3,
      .INTEL_shader_integer_functions2       = true,
      .EXT_multi_draw                        = true,
      .NV_compute_shader_derivatives         = true,
      .NV_mesh_shader                        = device->info.has_mesh_shading &&
                                               nv_mesh_shading_enabled,
      .VALVE_mutable_descriptor_type         = true,
   };
}

static uint64_t
anv_compute_sys_heap_size(struct anv_physical_device *device,
                          uint64_t total_ram)
{
   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024ull * 1024ull * 1024ull)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   /* We also want to leave some padding for things we allocate in the driver,
    * so don't go over 3/4 of the GTT either.
    */
   available_ram = MIN2(available_ram, device->gtt_size * 3 / 4);

   if (available_ram > (2ull << 30) && !device->supports_48bit_addresses) {
      /* When running with an overridden PCI ID, we may get a GTT size from
       * the kernel that is greater than 2 GiB but the execbuf check for 48bit
       * address support can still fail.  Just clamp the address space size to
       * 2 GiB if we don't have 48-bit support.
       */
      mesa_logw("%s:%d: The kernel reported a GTT size larger than 2 GiB but "
                "not support for 48-bit addresses",
                __FILE__, __LINE__);
      available_ram = 2ull << 30;
   }

   return available_ram;
}

static VkResult MUST_CHECK
anv_init_meminfo(struct anv_physical_device *device, int fd)
{
   const struct intel_device_info *devinfo = &device->info;

   device->sys.region.memory_class = devinfo->mem.sram.mem_class;
   device->sys.region.memory_instance = devinfo->mem.sram.mem_instance;
   device->sys.size =
      anv_compute_sys_heap_size(device, devinfo->mem.sram.mappable.size);
   device->sys.available = devinfo->mem.sram.mappable.free;

   device->vram_mappable.region.memory_class = devinfo->mem.vram.mem_class;
   device->vram_mappable.region.memory_instance =
      devinfo->mem.vram.mem_instance;
   device->vram_mappable.size = devinfo->mem.vram.mappable.size;
   device->vram_mappable.available = devinfo->mem.vram.mappable.free;

   device->vram_non_mappable.region.memory_class =
      devinfo->mem.vram.mem_class;
   device->vram_non_mappable.region.memory_instance =
      devinfo->mem.vram.mem_instance;
   device->vram_non_mappable.size = devinfo->mem.vram.unmappable.size;
   device->vram_non_mappable.available = devinfo->mem.vram.unmappable.free;

   return VK_SUCCESS;
}

static void
anv_update_meminfo(struct anv_physical_device *device, int fd)
{
   if (!intel_device_info_update_memory_info(&device->info, fd))
      return;

   const struct intel_device_info *devinfo = &device->info;
   device->sys.available = devinfo->mem.sram.mappable.free;
   device->vram_mappable.available = devinfo->mem.vram.mappable.free;
   device->vram_non_mappable.available = devinfo->mem.vram.unmappable.free;
}


static VkResult
anv_physical_device_init_heaps(struct anv_physical_device *device, int fd)
{
   VkResult result = anv_init_meminfo(device, fd);
   if (result != VK_SUCCESS)
      return result;

   assert(device->sys.size != 0);

   if (anv_physical_device_has_vram(device)) {
      /* We can create 2 or 3 different heaps when we have local memory
       * support, first heap with local memory size and second with system
       * memory size and the third is added only if part of the vram is
       * mappable to the host.
       */
      device->memory.heap_count = 2;
      device->memory.heaps[0] = (struct anv_memory_heap) {
         /* If there is a vram_non_mappable, use that for the device only
          * heap. Otherwise use the vram_mappable.
          */
         .size = device->vram_non_mappable.size != 0 ?
                 device->vram_non_mappable.size : device->vram_mappable.size,
         .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
         .is_local_mem = true,
      };
      device->memory.heaps[1] = (struct anv_memory_heap) {
         .size = device->sys.size,
         .flags = 0,
         .is_local_mem = false,
      };
      /* Add an additional smaller vram mappable heap if we can't map all the
       * vram to the host.
       */
      if (device->vram_non_mappable.size > 0) {
         device->memory.heap_count++;
         device->memory.heaps[2] = (struct anv_memory_heap) {
            .size = device->vram_mappable.size,
            .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
            .is_local_mem = true,
         };
      }

      device->memory.type_count = 3;
      device->memory.types[0] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
         .heapIndex = 0,
      };
      device->memory.types[1] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 1,
      };
      device->memory.types[2] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         /* This memory type either comes from heaps[0] if there is only
          * mappable vram region, or from heaps[2] if there is both mappable &
          * non-mappable vram regions.
          */
         .heapIndex = device->vram_non_mappable.size > 0 ? 2 : 0,
      };
   } else if (device->info.has_llc) {
      device->memory.heap_count = 1;
      device->memory.heaps[0] = (struct anv_memory_heap) {
         .size = device->sys.size,
         .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
         .is_local_mem = false,
      };

      /* Big core GPUs share LLC with the CPU and thus one memory type can be
       * both cached and coherent at the same time.
       */
      device->memory.type_count = 1;
      device->memory.types[0] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 0,
      };
   } else {
      device->memory.heap_count = 1;
      device->memory.heaps[0] = (struct anv_memory_heap) {
         .size = device->sys.size,
         .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
         .is_local_mem = false,
      };

      /* The spec requires that we expose a host-visible, coherent memory
       * type, but Atom GPUs don't share LLC. Thus we offer two memory types
       * to give the application a choice between cached, but not coherent and
       * coherent but uncached (WC though).
       */
      device->memory.type_count = 2;
      device->memory.types[0] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         .heapIndex = 0,
      };
      device->memory.types[1] = (struct anv_memory_type) {
         .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
         .heapIndex = 0,
      };
   }

   for (unsigned i = 0; i < device->memory.type_count; i++) {
      VkMemoryPropertyFlags props = device->memory.types[i].propertyFlags;
      if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
          !(props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
#ifdef SUPPORT_INTEL_INTEGRATED_GPUS
         device->memory.need_clflush = true;
#else
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "Memory configuration requires flushing, but it's not implemented for this architecture");
#endif
   }

   return VK_SUCCESS;
}

static VkResult
anv_physical_device_init_uuids(struct anv_physical_device *device)
{
   const struct build_id_note *note =
      build_id_find_nhdr_for_addr(anv_physical_device_init_uuids);
   if (!note) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to find build-id");
   }

   unsigned build_id_len = build_id_length(note);
   if (build_id_len < 20) {
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "build-id too short.  It needs to be a SHA");
   }

   memcpy(device->driver_build_sha1, build_id_data(note), 20);

   struct mesa_sha1 sha1_ctx;
   uint8_t sha1[20];
   STATIC_ASSERT(VK_UUID_SIZE <= sizeof(sha1));

   /* The pipeline cache UUID is used for determining when a pipeline cache is
    * invalid.  It needs both a driver build and the PCI ID of the device.
    */
   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, build_id_data(note), build_id_len);
   _mesa_sha1_update(&sha1_ctx, &device->info.pci_device_id,
                     sizeof(device->info.pci_device_id));
   _mesa_sha1_update(&sha1_ctx, &device->always_use_bindless,
                     sizeof(device->always_use_bindless));
   _mesa_sha1_final(&sha1_ctx, sha1);
   memcpy(device->pipeline_cache_uuid, sha1, VK_UUID_SIZE);

   intel_uuid_compute_driver_id(device->driver_uuid, &device->info, VK_UUID_SIZE);
   intel_uuid_compute_device_id(device->device_uuid, &device->info, VK_UUID_SIZE);

   return VK_SUCCESS;
}

static void
anv_physical_device_init_disk_cache(struct anv_physical_device *device)
{
#ifdef ENABLE_SHADER_CACHE
   char renderer[10];
   ASSERTED int len = snprintf(renderer, sizeof(renderer), "anv_%04x",
                               device->info.pci_device_id);
   assert(len == sizeof(renderer) - 2);

   char timestamp[41];
   _mesa_sha1_format(timestamp, device->driver_build_sha1);

   const uint64_t driver_flags =
      brw_get_compiler_config_value(device->compiler);
   device->vk.disk_cache = disk_cache_create(renderer, timestamp, driver_flags);
#endif
}

static void
anv_physical_device_free_disk_cache(struct anv_physical_device *device)
{
#ifdef ENABLE_SHADER_CACHE
   if (device->vk.disk_cache) {
      disk_cache_destroy(device->vk.disk_cache);
      device->vk.disk_cache = NULL;
   }
#else
   assert(device->vk.disk_cache == NULL);
#endif
}

/* The ANV_QUEUE_OVERRIDE environment variable is a comma separated list of
 * queue overrides.
 *
 * To override the number queues:
 *  * "gc" is for graphics queues with compute support
 *  * "g" is for graphics queues with no compute support
 *  * "c" is for compute queues with no graphics support
 *
 * For example, ANV_QUEUE_OVERRIDE=gc=2,c=1 would override the number of
 * advertised queues to be 2 queues with graphics+compute support, and 1 queue
 * with compute-only support.
 *
 * ANV_QUEUE_OVERRIDE=c=1 would override the number of advertised queues to
 * include 1 queue with compute-only support, but it will not change the
 * number of graphics+compute queues.
 *
 * ANV_QUEUE_OVERRIDE=gc=0,c=1 would override the number of advertised queues
 * to include 1 queue with compute-only support, and it would override the
 * number of graphics+compute queues to be 0.
 */
static void
anv_override_engine_counts(int *gc_count, int *g_count, int *c_count)
{
   int gc_override = -1;
   int g_override = -1;
   int c_override = -1;
   char *env = getenv("ANV_QUEUE_OVERRIDE");

   if (env == NULL)
      return;

   env = strdup(env);
   char *save = NULL;
   char *next = strtok_r(env, ",", &save);
   while (next != NULL) {
      if (strncmp(next, "gc=", 3) == 0) {
         gc_override = strtol(next + 3, NULL, 0);
      } else if (strncmp(next, "g=", 2) == 0) {
         g_override = strtol(next + 2, NULL, 0);
      } else if (strncmp(next, "c=", 2) == 0) {
         c_override = strtol(next + 2, NULL, 0);
      } else {
         mesa_logw("Ignoring unsupported ANV_QUEUE_OVERRIDE token: %s", next);
      }
      next = strtok_r(NULL, ",", &save);
   }
   free(env);
   if (gc_override >= 0)
      *gc_count = gc_override;
   if (g_override >= 0)
      *g_count = g_override;
   if (*g_count > 0 && *gc_count <= 0 && (gc_override >= 0 || g_override >= 0))
      mesa_logw("ANV_QUEUE_OVERRIDE: gc=0 with g > 0 violates the "
                "Vulkan specification");
   if (c_override >= 0)
      *c_count = c_override;
}

static void
anv_physical_device_init_queue_families(struct anv_physical_device *pdevice)
{
   uint32_t family_count = 0;

   if (pdevice->engine_info) {
      int gc_count =
         intel_engines_count(pdevice->engine_info,
                             INTEL_ENGINE_CLASS_RENDER);
      int g_count = 0;
      int c_count = 0;
      if (debug_get_bool_option("INTEL_COMPUTE_CLASS", false))
         c_count = intel_engines_count(pdevice->engine_info,
                                       INTEL_ENGINE_CLASS_COMPUTE);
      enum intel_engine_class compute_class =
         c_count < 1 ? INTEL_ENGINE_CLASS_RENDER : INTEL_ENGINE_CLASS_COMPUTE;

      anv_override_engine_counts(&gc_count, &g_count, &c_count);

      if (gc_count > 0) {
         pdevice->queue.families[family_count++] = (struct anv_queue_family) {
            .queueFlags = VK_QUEUE_GRAPHICS_BIT |
                          VK_QUEUE_COMPUTE_BIT |
                          VK_QUEUE_TRANSFER_BIT,
            .queueCount = gc_count,
            .engine_class = INTEL_ENGINE_CLASS_RENDER,
         };
      }
      if (g_count > 0) {
         pdevice->queue.families[family_count++] = (struct anv_queue_family) {
            .queueFlags = VK_QUEUE_GRAPHICS_BIT |
                          VK_QUEUE_TRANSFER_BIT,
            .queueCount = g_count,
            .engine_class = INTEL_ENGINE_CLASS_RENDER,
         };
      }
      if (c_count > 0) {
         pdevice->queue.families[family_count++] = (struct anv_queue_family) {
            .queueFlags = VK_QUEUE_COMPUTE_BIT |
                          VK_QUEUE_TRANSFER_BIT,
            .queueCount = c_count,
            .engine_class = compute_class,
         };
      }
      /* Increase count below when other families are added as a reminder to
       * increase the ANV_MAX_QUEUE_FAMILIES value.
       */
      STATIC_ASSERT(ANV_MAX_QUEUE_FAMILIES >= 3);
   } else {
      /* Default to a single render queue */
      pdevice->queue.families[family_count++] = (struct anv_queue_family) {
         .queueFlags = VK_QUEUE_GRAPHICS_BIT |
                       VK_QUEUE_COMPUTE_BIT |
                       VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .engine_class = INTEL_ENGINE_CLASS_RENDER,
      };
      family_count = 1;
   }
   assert(family_count <= ANV_MAX_QUEUE_FAMILIES);
   pdevice->queue.family_count = family_count;
}

static VkResult
anv_i915_physical_device_get_parameters(struct anv_physical_device *device)
{
   VkResult result = VK_SUCCESS;
   int val, fd = device->local_fd;

   if (!intel_gem_get_param(fd, I915_PARAM_HAS_WAIT_TIMEOUT, &val) || !val) {
       result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "kernel missing gem wait");
       return result;
   }

   if (!intel_gem_get_param(fd, I915_PARAM_HAS_EXECBUF2, &val) || !val) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "kernel missing execbuf2");
      return result;
   }

   if (!device->info.has_llc &&
       (!intel_gem_get_param(fd, I915_PARAM_MMAP_VERSION, &val) || val < 1)) {
       result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "kernel missing wc mmap");
       return result;
   }

   if (!intel_gem_get_param(fd, I915_PARAM_HAS_EXEC_SOFTPIN, &val) || !val) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "kernel missing softpin");
      return result;
   }

   if (!intel_gem_get_param(fd, I915_PARAM_HAS_EXEC_FENCE_ARRAY, &val) || !val) {
      result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                         "kernel missing syncobj support");
      return result;
   }

   if (intel_gem_get_param(fd, I915_PARAM_HAS_EXEC_ASYNC, &val))
      device->has_exec_async = val;
   if (intel_gem_get_param(fd, I915_PARAM_HAS_EXEC_CAPTURE, &val))
      device->has_exec_capture = val;

   /* Start with medium; sorted low to high */
   const VkQueueGlobalPriorityKHR priorities[] = {
         VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR,
         VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
         VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
         VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR,
   };
   device->max_context_priority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
   for (unsigned i = 0; i < ARRAY_SIZE(priorities); i++) {
      if (!anv_gem_has_context_priority(fd, priorities[i]))
         break;
      device->max_context_priority = priorities[i];
   }

   if (intel_gem_get_param(fd, I915_PARAM_HAS_EXEC_TIMELINE_FENCES, &val))
      device->has_exec_timeline = val;

   return result;
}

static VkResult
anv_physical_device_get_parameters(struct anv_physical_device *device)
{
   return anv_i915_physical_device_get_parameters(device);
}

static VkResult
anv_physical_device_try_create(struct vk_instance *vk_instance,
                               struct _drmDevice *drm_device,
                               struct vk_physical_device **out)
{
   struct anv_instance *instance =
      container_of(vk_instance, struct anv_instance, vk);

   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PCI ||
       drm_device->deviceinfo.pci->vendor_id != 0x8086)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   const char *primary_path = drm_device->nodes[DRM_NODE_PRIMARY];
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result;
   int fd;
   int master_fd = -1;

   brw_process_intel_debug_variable();

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      if (errno == ENOMEM) {
         return vk_errorf(instance, VK_ERROR_OUT_OF_HOST_MEMORY,
                          "Unable to open device %s: out of memory", path);
      }
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Unable to open device %s: %m", path);
   }

   struct intel_device_info devinfo;
   if (!intel_get_device_info_from_fd(fd, &devinfo)) {
      result = vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);
      goto fail_fd;
   }

   if (devinfo.ver > 12) {
      result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "Vulkan not yet supported on %s", devinfo.name);
      goto fail_fd;
   } else if (devinfo.ver < 9) {
      /* Silently fail here, hasvk should pick up this device. */
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail_fd;
   }

   struct anv_physical_device *device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_fd;
   }

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &anv_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);

   result = vk_physical_device_init(&device->vk, &instance->vk,
                                    NULL, /* We set up extensions later */
                                    &dispatch_table);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail_alloc;
   }
   device->instance = instance;

   assert(strlen(path) < ARRAY_SIZE(device->path));
   snprintf(device->path, ARRAY_SIZE(device->path), "%s", path);

   device->info = devinfo;

   device->local_fd = fd;
   result = anv_physical_device_get_parameters(device);
   if (result != VK_SUCCESS)
      goto fail_base;

   device->gtt_size = device->info.gtt_size ? device->info.gtt_size :
                                              device->info.aperture_bytes;

   /* We only allow 48-bit addresses with softpin because knowing the actual
    * address is required for the vertex cache flush workaround.
    */
   device->supports_48bit_addresses =
      device->gtt_size > (4ULL << 30 /* GiB */);

   /* We currently only have the right bits for instructions in Gen12+. If the
    * kernel ever starts supporting that feature on previous generations,
    * we'll need to edit genxml prior to enabling here.
    */
   device->has_protected_contexts = device->info.ver >= 12 &&
      intel_gem_supports_protected_context(fd);

   result = anv_physical_device_init_heaps(device, fd);
   if (result != VK_SUCCESS)
      goto fail_base;

   if (debug_get_bool_option("ANV_QUEUE_THREAD_DISABLE", false))
      device->has_exec_timeline = false;

   unsigned st_idx = 0;

   device->sync_syncobj_type = vk_drm_syncobj_get_type(fd);
   if (!device->has_exec_timeline)
      device->sync_syncobj_type.features &= ~VK_SYNC_FEATURE_TIMELINE;
   device->sync_types[st_idx++] = &device->sync_syncobj_type;

   if (!(device->sync_syncobj_type.features & VK_SYNC_FEATURE_CPU_WAIT))
      device->sync_types[st_idx++] = &anv_bo_sync_type;

   if (!(device->sync_syncobj_type.features & VK_SYNC_FEATURE_TIMELINE)) {
      device->sync_timeline_type = vk_sync_timeline_get_type(&anv_bo_sync_type);
      device->sync_types[st_idx++] = &device->sync_timeline_type.sync;
   }

   device->sync_types[st_idx++] = NULL;
   assert(st_idx <= ARRAY_SIZE(device->sync_types));
   device->vk.supported_sync_types = device->sync_types;

   device->vk.pipeline_cache_import_ops = anv_cache_import_ops;

   device->always_use_bindless =
      debug_get_bool_option("ANV_ALWAYS_BINDLESS", false);

   device->use_call_secondary =
      !debug_get_bool_option("ANV_DISABLE_SECONDARY_CMD_BUFFER_CALLS", false);

   device->has_implicit_ccs = device->info.has_aux_map ||
                              device->info.verx10 >= 125;

   /* Check if we can read the GPU timestamp register from the CPU */
   uint64_t u64_ignore;
   device->has_reg_timestamp = intel_gem_read_render_timestamp(fd, &u64_ignore);

   device->always_flush_cache = INTEL_DEBUG(DEBUG_STALL) ||
      driQueryOptionb(&instance->dri_options, "always_flush_cache");

   device->compiler = brw_compiler_create(NULL, &device->info);
   if (device->compiler == NULL) {
      result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_base;
   }
   device->compiler->shader_debug_log = compiler_debug_log;
   device->compiler->shader_perf_log = compiler_perf_log;
   device->compiler->constant_buffer_0_is_relative =
      !device->info.has_context_isolation;
   device->compiler->supports_shader_constants = true;
   device->compiler->indirect_ubos_use_sampler = device->info.ver < 12;

   isl_device_init(&device->isl_dev, &device->info);

   result = anv_physical_device_init_uuids(device);
   if (result != VK_SUCCESS)
      goto fail_compiler;

   anv_physical_device_init_disk_cache(device);

   if (instance->vk.enabled_extensions.KHR_display) {
      master_fd = open(primary_path, O_RDWR | O_CLOEXEC);
      if (master_fd >= 0) {
         /* fail if we don't have permission to even render on this device */
         if (!intel_gem_can_render_on_fd(master_fd)) {
            close(master_fd);
            master_fd = -1;
         }
      }
   }
   device->master_fd = master_fd;

   device->engine_info = intel_engine_get_info(fd);
   anv_physical_device_init_queue_families(device);

   anv_physical_device_init_perf(device, fd);

   get_device_extensions(device, &device->vk.supported_extensions);

   /* Gather major/minor before WSI. */
   struct stat st;

   if (stat(primary_path, &st) == 0) {
      device->has_master = true;
      device->master_major = major(st.st_rdev);
      device->master_minor = minor(st.st_rdev);
   } else {
      device->has_master = false;
      device->master_major = 0;
      device->master_minor = 0;
   }

   if (stat(path, &st) == 0) {
      device->has_local = true;
      device->local_major = major(st.st_rdev);
      device->local_minor = minor(st.st_rdev);
   } else {
      device->has_local = false;
      device->local_major = 0;
      device->local_minor = 0;
   }

   result = anv_init_wsi(device);
   if (result != VK_SUCCESS)
      goto fail_perf;

   anv_measure_device_init(device);

   anv_genX(&device->info, init_physical_device_state)(device);

   *out = &device->vk;

   return VK_SUCCESS;

fail_perf:
   ralloc_free(device->perf);
   free(device->engine_info);
   anv_physical_device_free_disk_cache(device);
fail_compiler:
   ralloc_free(device->compiler);
fail_base:
   vk_physical_device_finish(&device->vk);
fail_alloc:
   vk_free(&instance->vk.alloc, device);
fail_fd:
   close(fd);
   if (master_fd != -1)
      close(master_fd);
   return result;
}

static void
anv_physical_device_destroy(struct vk_physical_device *vk_device)
{
   struct anv_physical_device *device =
      container_of(vk_device, struct anv_physical_device, vk);

   anv_finish_wsi(device);
   anv_measure_device_destroy(device);
   free(device->engine_info);
   anv_physical_device_free_disk_cache(device);
   ralloc_free(device->compiler);
   ralloc_free(device->perf);
   close(device->local_fd);
   if (device->master_fd >= 0)
      close(device->master_fd);
   vk_physical_device_finish(&device->vk);
   vk_free(&device->instance->vk.alloc, device);
}

VkResult anv_EnumerateInstanceExtensionProperties(
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &instance_extensions, pPropertyCount, pProperties);
}

static void
anv_init_dri_options(struct anv_instance *instance)
{
   driParseOptionInfo(&instance->available_dri_options, anv_dri_options,
                      ARRAY_SIZE(anv_dri_options));
   driParseConfigFiles(&instance->dri_options,
                       &instance->available_dri_options, 0, "anv", NULL, NULL,
                       instance->vk.app_info.app_name,
                       instance->vk.app_info.app_version,
                       instance->vk.app_info.engine_name,
                       instance->vk.app_info.engine_version);

    instance->assume_full_subgroups =
            driQueryOptionb(&instance->dri_options, "anv_assume_full_subgroups");
    instance->limit_trig_input_range =
            driQueryOptionb(&instance->dri_options, "limit_trig_input_range");
    instance->sample_mask_out_opengl_behaviour =
            driQueryOptionb(&instance->dri_options, "anv_sample_mask_out_opengl_behaviour");
    instance->lower_depth_range_rate =
            driQueryOptionf(&instance->dri_options, "lower_depth_range_rate");
    instance->fp64_workaround_enabled =
            driQueryOptionb(&instance->dri_options, "fp64_workaround_enabled");
}

VkResult anv_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
   struct anv_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   instance = vk_alloc(pAllocator, sizeof(*instance), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &anv_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk, &instance_extensions,
                             &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->vk.physical_devices.try_create_for_drm = anv_physical_device_try_create;
   instance->vk.physical_devices.destroy = anv_physical_device_destroy;

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   anv_init_dri_options(instance);

   intel_driver_ds_init();

   *pInstance = anv_instance_to_handle(instance);

   return VK_SUCCESS;
}

void anv_DestroyInstance(
    VkInstance                                  _instance,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_instance, instance, _instance);

   if (!instance)
      return;

   VG(VALGRIND_DESTROY_MEMPOOL(instance));

   driDestroyOptionCache(&instance->dri_options);
   driDestroyOptionInfo(&instance->available_dri_options);

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

void anv_GetPhysicalDeviceFeatures(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceFeatures*                   pFeatures)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);

   /* Just pick one; they're all the same */
   const bool has_astc_ldr =
      isl_format_supports_sampling(&pdevice->info,
                                   ISL_FORMAT_ASTC_LDR_2D_4X4_FLT16);

   *pFeatures = (VkPhysicalDeviceFeatures) {
      .robustBufferAccess                       = true,
      .fullDrawIndexUint32                      = true,
      .imageCubeArray                           = true,
      .independentBlend                         = true,
      .geometryShader                           = true,
      .tessellationShader                       = true,
      .sampleRateShading                        = true,
      .dualSrcBlend                             = true,
      .logicOp                                  = true,
      .multiDrawIndirect                        = true,
      .drawIndirectFirstInstance                = true,
      .depthClamp                               = true,
      .depthBiasClamp                           = true,
      .fillModeNonSolid                         = true,
      .depthBounds                              = pdevice->info.ver >= 12,
      .wideLines                                = true,
      .largePoints                              = true,
      .alphaToOne                               = true,
      .multiViewport                            = true,
      .samplerAnisotropy                        = true,
      .textureCompressionETC2                   = true,
      .textureCompressionASTC_LDR               = has_astc_ldr,
      .textureCompressionBC                     = true,
      .occlusionQueryPrecise                    = true,
      .pipelineStatisticsQuery                  = true,
      .fragmentStoresAndAtomics                 = true,
      .shaderTessellationAndGeometryPointSize   = true,
      .shaderImageGatherExtended                = true,
      .shaderStorageImageExtendedFormats        = true,
      .shaderStorageImageMultisample            = false,
      .shaderStorageImageReadWithoutFormat      = false,
      .shaderStorageImageWriteWithoutFormat     = true,
      .shaderUniformBufferArrayDynamicIndexing  = true,
      .shaderSampledImageArrayDynamicIndexing   = true,
      .shaderStorageBufferArrayDynamicIndexing  = true,
      .shaderStorageImageArrayDynamicIndexing   = true,
      .shaderClipDistance                       = true,
      .shaderCullDistance                       = true,
      .shaderFloat64                            = pdevice->info.has_64bit_float,
      .shaderInt64                              = true,
      .shaderInt16                              = true,
      .shaderResourceMinLod                     = true,
      .variableMultisampleRate                  = true,
      .inheritedQueries                         = true,
   };

   /* We can't do image stores in vec4 shaders */
   pFeatures->vertexPipelineStoresAndAtomics =
      pdevice->compiler->scalar_stage[MESA_SHADER_VERTEX] &&
      pdevice->compiler->scalar_stage[MESA_SHADER_GEOMETRY];

   struct vk_app_info *app_info = &pdevice->instance->vk.app_info;

   /* The new DOOM and Wolfenstein games require depthBounds without
    * checking for it.  They seem to run fine without it so just claim it's
    * there and accept the consequences.
    */
   if (app_info->engine_name && strcmp(app_info->engine_name, "idTech") == 0)
      pFeatures->depthBounds = true;
}

static void
anv_get_physical_device_features_1_1(struct anv_physical_device *pdevice,
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
   f->samplerYcbcrConversion              = true;
   f->shaderDrawParameters                = true;
}

static void
anv_get_physical_device_features_1_2(struct anv_physical_device *pdevice,
                                     VkPhysicalDeviceVulkan12Features *f)
{
   assert(f->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);

   f->samplerMirrorClampToEdge            = true;
   f->drawIndirectCount                   = true;
   f->storageBuffer8BitAccess             = true;
   f->uniformAndStorageBuffer8BitAccess   = true;
   f->storagePushConstant8                = true;
   f->shaderBufferInt64Atomics            = true;
   f->shaderSharedInt64Atomics            = false;
   f->shaderFloat16                       = true;
   f->shaderInt8                          = true;

   f->descriptorIndexing                                 = true;
   f->shaderInputAttachmentArrayDynamicIndexing          = false;
   f->shaderUniformTexelBufferArrayDynamicIndexing       = true;
   f->shaderStorageTexelBufferArrayDynamicIndexing       = true;
   f->shaderUniformBufferArrayNonUniformIndexing         = false;
   f->shaderSampledImageArrayNonUniformIndexing          = true;
   f->shaderStorageBufferArrayNonUniformIndexing         = true;
   f->shaderStorageImageArrayNonUniformIndexing          = true;
   f->shaderInputAttachmentArrayNonUniformIndexing       = false;
   f->shaderUniformTexelBufferArrayNonUniformIndexing    = true;
   f->shaderStorageTexelBufferArrayNonUniformIndexing    = true;
   f->descriptorBindingUniformBufferUpdateAfterBind      = true;
   f->descriptorBindingSampledImageUpdateAfterBind       = true;
   f->descriptorBindingStorageImageUpdateAfterBind       = true;
   f->descriptorBindingStorageBufferUpdateAfterBind      = true;
   f->descriptorBindingUniformTexelBufferUpdateAfterBind = true;
   f->descriptorBindingStorageTexelBufferUpdateAfterBind = true;
   f->descriptorBindingUpdateUnusedWhilePending          = true;
   f->descriptorBindingPartiallyBound                    = true;
   f->descriptorBindingVariableDescriptorCount           = true;
   f->runtimeDescriptorArray                             = true;

   f->samplerFilterMinmax                 = true;
   f->scalarBlockLayout                   = true;
   f->imagelessFramebuffer                = true;
   f->uniformBufferStandardLayout         = true;
   f->shaderSubgroupExtendedTypes         = true;
   f->separateDepthStencilLayouts         = true;
   f->hostQueryReset                      = true;
   f->timelineSemaphore                   = true;
   f->bufferDeviceAddress                 = true;
   f->bufferDeviceAddressCaptureReplay    = true;
   f->bufferDeviceAddressMultiDevice      = false;
   f->vulkanMemoryModel                   = true;
   f->vulkanMemoryModelDeviceScope        = true;
   f->vulkanMemoryModelAvailabilityVisibilityChains = true;
   f->shaderOutputViewportIndex           = true;
   f->shaderOutputLayer                   = true;
   f->subgroupBroadcastDynamicId          = true;
}

static void
anv_get_physical_device_features_1_3(struct anv_physical_device *pdevice,
                                     VkPhysicalDeviceVulkan13Features *f)
{
   assert(f->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);

   f->robustImageAccess = true;
   f->inlineUniformBlock = true;
   f->descriptorBindingInlineUniformBlockUpdateAfterBind = true;
   f->pipelineCreationCacheControl = true;
   f->privateData = true;
   f->shaderDemoteToHelperInvocation = true;
   f->shaderTerminateInvocation = true;
   f->subgroupSizeControl = true;
   f->computeFullSubgroups = true;
   f->synchronization2 = true;
   f->textureCompressionASTC_HDR = false;
   f->shaderZeroInitializeWorkgroupMemory = true;
   f->dynamicRendering = true;
   f->shaderIntegerDotProduct = true;
   f->maintenance4 = true;
}

void anv_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceFeatures2*                  pFeatures)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);
   anv_GetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

   VkPhysicalDeviceVulkan11Features core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };
   anv_get_physical_device_features_1_1(pdevice, &core_1_1);

   VkPhysicalDeviceVulkan12Features core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
   };
   anv_get_physical_device_features_1_2(pdevice, &core_1_2);

   VkPhysicalDeviceVulkan13Features core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   anv_get_physical_device_features_1_3(pdevice, &core_1_3);

   vk_foreach_struct(ext, pFeatures->pNext) {
      if (vk_get_physical_device_core_1_1_feature_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_feature_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_feature_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
         VkPhysicalDevice4444FormatsFeaturesEXT *features =
            (VkPhysicalDevice4444FormatsFeaturesEXT *)ext;
         features->formatA4R4G4B4 = true;
         features->formatA4B4G4R4 = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR: {
         VkPhysicalDeviceAccelerationStructureFeaturesKHR *features = (void *)ext;
         features->accelerationStructure =
            ANV_SUPPORT_RT && pdevice->info.has_ray_tracing;
         features->accelerationStructureCaptureReplay = false; /* TODO */
         features->accelerationStructureIndirectBuild = false; /* TODO */
         features->accelerationStructureHostCommands = false;
         features->descriptorBindingAccelerationStructureUpdateAfterBind =
            ANV_SUPPORT_RT && pdevice->info.has_ray_tracing;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT: {
         VkPhysicalDeviceBufferDeviceAddressFeaturesEXT *features = (void *)ext;
         features->bufferDeviceAddress = true;
         features->bufferDeviceAddressCaptureReplay = false;
         features->bufferDeviceAddressMultiDevice = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT: {
         VkPhysicalDeviceBorderColorSwizzleFeaturesEXT *features =
            (VkPhysicalDeviceBorderColorSwizzleFeaturesEXT *)ext;
         features->borderColorSwizzle = true;
         features->borderColorSwizzleFromImage = true;
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

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV: {
         VkPhysicalDeviceComputeShaderDerivativesFeaturesNV *features =
            (VkPhysicalDeviceComputeShaderDerivativesFeaturesNV *)ext;
         features->computeDerivativeGroupQuads = true;
         features->computeDerivativeGroupLinear = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT: {
         VkPhysicalDeviceConditionalRenderingFeaturesEXT *features =
            (VkPhysicalDeviceConditionalRenderingFeaturesEXT*)ext;
         features->conditionalRendering = true;
         features->inheritedConditionalRendering = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
         VkPhysicalDeviceCustomBorderColorFeaturesEXT *features =
            (VkPhysicalDeviceCustomBorderColorFeaturesEXT *)ext;
         features->customBorderColors = true;
         features->customBorderColorWithoutFormat = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT: {
         VkPhysicalDeviceDepthClampZeroOneFeaturesEXT *features =
            (VkPhysicalDeviceDepthClampZeroOneFeaturesEXT *)ext;
         features->depthClampZeroOne = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipEnableFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)ext;
         features->depthClipEnable = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT: {
         VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT *features =
            (VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT *)ext;
         features->fragmentShaderSampleInterlock = true;
         features->fragmentShaderPixelInterlock = true;
         features->fragmentShaderShadingRateInterlock = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR: {
         VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR *features =
            (VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR *)ext;
         features->globalPriorityQuery = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR: {
         VkPhysicalDeviceFragmentShadingRateFeaturesKHR *features =
            (VkPhysicalDeviceFragmentShadingRateFeaturesKHR *)ext;
         features->attachmentFragmentShadingRate = false;
         features->pipelineFragmentShadingRate = true;
         features->primitiveFragmentShadingRate =
            pdevice->info.has_coarse_pixel_primitive_and_cb;
         features->attachmentFragmentShadingRate =
            pdevice->info.has_coarse_pixel_primitive_and_cb;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT: {
         VkPhysicalDeviceImageViewMinLodFeaturesEXT *features =
            (VkPhysicalDeviceImageViewMinLodFeaturesEXT *)ext;
         features->minLod = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
         VkPhysicalDeviceIndexTypeUint8FeaturesEXT *features =
            (VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)ext;
         features->indexTypeUint8 = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT: {
         VkPhysicalDeviceLineRasterizationFeaturesEXT *features =
            (VkPhysicalDeviceLineRasterizationFeaturesEXT *)ext;
         /* Rectangular lines must use the strict algorithm, which is not
          * supported for wide lines prior to ICL.  See rasterization_mode for
          * details and how the HW states are programmed.
          */
         features->rectangularLines = pdevice->info.ver >= 10;
         features->bresenhamLines = true;
         /* Support for Smooth lines with MSAA was removed on gfx11.  From the
          * BSpec section "Multisample ModesState" table for "AA Line Support
          * Requirements":
          *
          *    GFX10:BUG:######## 	NUM_MULTISAMPLES == 1
          *
          * Fortunately, this isn't a case most people care about.
          */
         features->smoothLines = pdevice->info.ver < 10;
         features->stippledRectangularLines = false;
         features->stippledBresenhamLines = true;
         features->stippledSmoothLines = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV: {
         VkPhysicalDeviceMeshShaderFeaturesNV *features =
            (VkPhysicalDeviceMeshShaderFeaturesNV *)ext;
         features->taskShader = pdevice->vk.supported_extensions.NV_mesh_shader;
         features->meshShader = pdevice->vk.supported_extensions.NV_mesh_shader;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT: {
         VkPhysicalDeviceMeshShaderFeaturesEXT *features =
            (VkPhysicalDeviceMeshShaderFeaturesEXT *)ext;
         features->meshShader = pdevice->vk.supported_extensions.EXT_mesh_shader;
         features->taskShader = pdevice->vk.supported_extensions.EXT_mesh_shader;
         features->multiviewMeshShader = false;
         features->primitiveFragmentShadingRateMeshShader = features->meshShader;
         features->meshShaderQueries = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT: {
         VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT *features =
            (VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT *)ext;
         features->mutableDescriptorType = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR: {
         VkPhysicalDevicePerformanceQueryFeaturesKHR *feature =
            (VkPhysicalDevicePerformanceQueryFeaturesKHR *)ext;
         feature->performanceCounterQueryPools = true;
         /* HW only supports a single configuration at a time. */
         feature->performanceCounterMultipleQueryPools = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR: {
         VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR *features =
            (VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR *)ext;
         features->pipelineExecutableInfo = true;
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

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
         VkPhysicalDeviceProvokingVertexFeaturesEXT *features =
            (VkPhysicalDeviceProvokingVertexFeaturesEXT *)ext;
         features->provokingVertexLast = true;
         features->transformFeedbackPreservesProvokingVertex = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR: {
         VkPhysicalDeviceRayQueryFeaturesKHR *features = (void *)ext;
         features->rayQuery = ANV_SUPPORT_RT && pdevice->info.has_ray_tracing;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR: {
         VkPhysicalDeviceRayTracingPipelineFeaturesKHR *features = (void *)ext;
         features->rayTracingPipeline = pdevice->info.has_ray_tracing;
         features->rayTracingPipelineShaderGroupHandleCaptureReplay = false;
         features->rayTracingPipelineShaderGroupHandleCaptureReplayMixed = false;
         features->rayTracingPipelineTraceRaysIndirect = true;
         features->rayTraversalPrimitiveCulling = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
         VkPhysicalDeviceRobustness2FeaturesEXT *features = (void *)ext;
         features->robustBufferAccess2 = true;
         features->robustImageAccess2 = true;
         features->nullDescriptor = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT: {
         VkPhysicalDeviceShaderAtomicFloatFeaturesEXT *features = (void *)ext;
         features->shaderBufferFloat32Atomics =    true;
         features->shaderBufferFloat32AtomicAdd =  pdevice->info.has_lsc;
         features->shaderBufferFloat64Atomics =
            pdevice->info.has_64bit_float && pdevice->info.has_lsc;
         features->shaderBufferFloat64AtomicAdd =  false;
         features->shaderSharedFloat32Atomics =    true;
         features->shaderSharedFloat32AtomicAdd =  false;
         features->shaderSharedFloat64Atomics =    false;
         features->shaderSharedFloat64AtomicAdd =  false;
         features->shaderImageFloat32Atomics =     true;
         features->shaderImageFloat32AtomicAdd =   false;
         features->sparseImageFloat32Atomics =     false;
         features->sparseImageFloat32AtomicAdd =   false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT: {
         VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT *features = (void *)ext;
         features->shaderBufferFloat16Atomics      = pdevice->info.has_lsc;
         features->shaderBufferFloat16AtomicAdd    = false;
         features->shaderBufferFloat16AtomicMinMax = pdevice->info.has_lsc;
         features->shaderBufferFloat32AtomicMinMax = true;
         features->shaderBufferFloat64AtomicMinMax =
            pdevice->info.has_64bit_float && pdevice->info.has_lsc;
         features->shaderSharedFloat16Atomics      = pdevice->info.has_lsc;
         features->shaderSharedFloat16AtomicAdd    = false;
         features->shaderSharedFloat16AtomicMinMax = pdevice->info.has_lsc;
         features->shaderSharedFloat32AtomicMinMax = true;
         features->shaderSharedFloat64AtomicMinMax = false;
         features->shaderImageFloat32AtomicMinMax  = false;
         features->sparseImageFloat32AtomicMinMax  = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR: {
         VkPhysicalDeviceShaderClockFeaturesKHR *features =
            (VkPhysicalDeviceShaderClockFeaturesKHR *)ext;
         features->shaderSubgroupClock = true;
         features->shaderDeviceClock = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_FUNCTIONS_2_FEATURES_INTEL: {
         VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL *features =
            (VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL *)ext;
         features->shaderIntegerFunctions2 = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT: {
         VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *features =
            (VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *)ext;
         features->shaderModuleIdentifier = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR: {
         VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR *features =
            (VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR *)ext;
         features->shaderSubgroupUniformControlFlow = true;
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
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)ext;
         features->transformFeedback = true;
         features->geometryStreams = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *features =
            (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)ext;
         features->vertexAttributeInstanceRateDivisor = true;
         features->vertexAttributeInstanceRateZeroDivisor = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR: {
         VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR *features =
            (VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR *)ext;
         features->workgroupMemoryExplicitLayout = true;
         features->workgroupMemoryExplicitLayoutScalarBlockLayout = true;
         features->workgroupMemoryExplicitLayout8BitAccess = true;
         features->workgroupMemoryExplicitLayout16BitAccess = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT: {
         VkPhysicalDeviceYcbcrImageArraysFeaturesEXT *features =
            (VkPhysicalDeviceYcbcrImageArraysFeaturesEXT *)ext;
         features->ycbcrImageArrays = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *features =
            (VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)ext;
         features->extendedDynamicState = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *features =
            (VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *)ext;
         features->extendedDynamicState2 = true;
         features->extendedDynamicState2LogicOp = true;
         features->extendedDynamicState2PatchControlPoints = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *features =
            (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *)ext;
         features->extendedDynamicState3PolygonMode = true;
         features->extendedDynamicState3TessellationDomainOrigin = true;
         features->extendedDynamicState3RasterizationStream = true;
         features->extendedDynamicState3LineStippleEnable = true;
         features->extendedDynamicState3LineRasterizationMode = true;
         features->extendedDynamicState3LogicOpEnable = true;
         features->extendedDynamicState3AlphaToOneEnable = true;
         features->extendedDynamicState3DepthClipEnable = true;
         features->extendedDynamicState3DepthClampEnable = true;
         features->extendedDynamicState3DepthClipNegativeOneToOne = true;
         features->extendedDynamicState3ProvokingVertexMode = true;
         features->extendedDynamicState3ColorBlendEnable = true;
         features->extendedDynamicState3ColorWriteMask = true;
         features->extendedDynamicState3ColorBlendEquation = true;
         features->extendedDynamicState3SampleLocationsEnable = true;
         features->extendedDynamicState3SampleMask = true;

         features->extendedDynamicState3RasterizationSamples = false;
         features->extendedDynamicState3AlphaToCoverageEnable = false;
         features->extendedDynamicState3ConservativeRasterizationMode = false;
         features->extendedDynamicState3ExtraPrimitiveOverestimationSize = false;
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

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT: {
         VkPhysicalDeviceMultiDrawFeaturesEXT *features = (VkPhysicalDeviceMultiDrawFeaturesEXT *)ext;
         features->multiDraw = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT : {
         VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT *features =
            (VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT *)ext;
         features->nonSeamlessCubeMap = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT: {
         VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *features =
            (VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT *)ext;
         features->primitiveTopologyListRestart = true;
         features->primitiveTopologyPatchListRestart = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipControlFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipControlFeaturesEXT *)ext;
         features->depthClipControl = true;
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
         anv_debug_ignored_stype(ext->sType);
         break;
      }
   }

}

#define MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BUFFERS   64

#define MAX_PER_STAGE_DESCRIPTOR_INPUT_ATTACHMENTS 64
#define MAX_DESCRIPTOR_SET_INPUT_ATTACHMENTS       256

#define MAX_CUSTOM_BORDER_COLORS                   4096

void anv_GetPhysicalDeviceProperties(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceProperties*                 pProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);
   const struct intel_device_info *devinfo = &pdevice->info;

   const uint32_t max_ssbos = UINT16_MAX;
   const uint32_t max_textures = UINT16_MAX;
   const uint32_t max_samplers = UINT16_MAX;
   const uint32_t max_images = UINT16_MAX;

   /* Claim a high per-stage limit since we have bindless. */
   const uint32_t max_per_stage = UINT32_MAX;

   const uint32_t max_workgroup_size =
      MIN2(1024, 32 * devinfo->max_cs_workgroup_threads);

   VkSampleCountFlags sample_counts =
      isl_device_get_sample_counts(&pdevice->isl_dev);


   VkPhysicalDeviceLimits limits = {
      .maxImageDimension1D                      = (1 << 14),
      .maxImageDimension2D                      = (1 << 14),
      .maxImageDimension3D                      = (1 << 11),
      .maxImageDimensionCube                    = (1 << 14),
      .maxImageArrayLayers                      = (1 << 11),
      .maxTexelBufferElements                   = 128 * 1024 * 1024,
      .maxUniformBufferRange                    = pdevice->compiler->indirect_ubos_use_sampler ? (1u << 27) : (1u << 30),
      .maxStorageBufferRange                    = pdevice->isl_dev.max_buffer_size,
      .maxPushConstantsSize                     = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount                 = UINT32_MAX,
      .maxSamplerAllocationCount                = 64 * 1024,
      .bufferImageGranularity                   = 1,
      .sparseAddressSpaceSize                   = 0,
      .maxBoundDescriptorSets                   = MAX_SETS,
      .maxPerStageDescriptorSamplers            = max_samplers,
      .maxPerStageDescriptorUniformBuffers      = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BUFFERS,
      .maxPerStageDescriptorStorageBuffers      = max_ssbos,
      .maxPerStageDescriptorSampledImages       = max_textures,
      .maxPerStageDescriptorStorageImages       = max_images,
      .maxPerStageDescriptorInputAttachments    = MAX_PER_STAGE_DESCRIPTOR_INPUT_ATTACHMENTS,
      .maxPerStageResources                     = max_per_stage,
      .maxDescriptorSetSamplers                 = 6 * max_samplers, /* number of stages * maxPerStageDescriptorSamplers */
      .maxDescriptorSetUniformBuffers           = 6 * MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BUFFERS,           /* number of stages * maxPerStageDescriptorUniformBuffers */
      .maxDescriptorSetUniformBuffersDynamic    = MAX_DYNAMIC_BUFFERS / 2,
      .maxDescriptorSetStorageBuffers           = 6 * max_ssbos,    /* number of stages * maxPerStageDescriptorStorageBuffers */
      .maxDescriptorSetStorageBuffersDynamic    = MAX_DYNAMIC_BUFFERS / 2,
      .maxDescriptorSetSampledImages            = 6 * max_textures, /* number of stages * maxPerStageDescriptorSampledImages */
      .maxDescriptorSetStorageImages            = 6 * max_images,   /* number of stages * maxPerStageDescriptorStorageImages */
      .maxDescriptorSetInputAttachments         = MAX_DESCRIPTOR_SET_INPUT_ATTACHMENTS,
      .maxVertexInputAttributes                 = MAX_VES,
      .maxVertexInputBindings                   = MAX_VBS,
      /* Broadwell PRMs: Volume 2d: Command Reference: Structures:
       *
       * VERTEX_ELEMENT_STATE::Source Element Offset: [0,2047]
       */
      .maxVertexInputAttributeOffset            = 2047,
      /* Skylake PRMs: Volume 2d: Command Reference: Structures:
       *
       * VERTEX_BUFFER_STATE::Buffer Pitch: [0,4095]
       */
      .maxVertexInputBindingStride              = 4095,
      .maxVertexOutputComponents                = 128,
      .maxTessellationGenerationLevel           = 64,
      .maxTessellationPatchSize                 = 32,
      .maxTessellationControlPerVertexInputComponents = 128,
      .maxTessellationControlPerVertexOutputComponents = 128,
      .maxTessellationControlPerPatchOutputComponents = 128,
      .maxTessellationControlTotalOutputComponents = 2048,
      .maxTessellationEvaluationInputComponents = 128,
      .maxTessellationEvaluationOutputComponents = 128,
      .maxGeometryShaderInvocations             = 32,
      .maxGeometryInputComponents               = 128,
      .maxGeometryOutputComponents              = 128,
      .maxGeometryOutputVertices                = 256,
      .maxGeometryTotalOutputComponents         = 1024,
      .maxFragmentInputComponents               = 116, /* 128 components - (PSIZ, CLIP_DIST0, CLIP_DIST1) */
      .maxFragmentOutputAttachments             = 8,
      .maxFragmentDualSrcAttachments            = 1,
      .maxFragmentCombinedOutputResources       = MAX_RTS + max_ssbos + max_images,
      .maxComputeSharedMemorySize               = 64 * 1024,
      .maxComputeWorkGroupCount                 = { 65535, 65535, 65535 },
      .maxComputeWorkGroupInvocations           = max_workgroup_size,
      .maxComputeWorkGroupSize = {
         max_workgroup_size,
         max_workgroup_size,
         max_workgroup_size,
      },
      .subPixelPrecisionBits                    = 8,
      .subTexelPrecisionBits                    = 8,
      .mipmapPrecisionBits                      = 8,
      .maxDrawIndexedIndexValue                 = UINT32_MAX,
      .maxDrawIndirectCount                     = UINT32_MAX,
      .maxSamplerLodBias                        = 16,
      .maxSamplerAnisotropy                     = 16,
      .maxViewports                             = MAX_VIEWPORTS,
      .maxViewportDimensions                    = { (1 << 14), (1 << 14) },
      .viewportBoundsRange                      = { INT16_MIN, INT16_MAX },
      .viewportSubPixelBits                     = 13, /* We take a float? */
      .minMemoryMapAlignment                    = 4096, /* A page */
      /* The dataport requires texel alignment so we need to assume a worst
       * case of R32G32B32A32 which is 16 bytes.
       */
      .minTexelBufferOffsetAlignment            = 16,
      .minUniformBufferOffsetAlignment          = ANV_UBO_ALIGNMENT,
      .minStorageBufferOffsetAlignment          = ANV_SSBO_ALIGNMENT,
      .minTexelOffset                           = -8,
      .maxTexelOffset                           = 7,
      .minTexelGatherOffset                     = -32,
      .maxTexelGatherOffset                     = 31,
      .minInterpolationOffset                   = -0.5,
      .maxInterpolationOffset                   = 0.4375,
      .subPixelInterpolationOffsetBits          = 4,
      .maxFramebufferWidth                      = (1 << 14),
      .maxFramebufferHeight                     = (1 << 14),
      .maxFramebufferLayers                     = (1 << 11),
      .framebufferColorSampleCounts             = sample_counts,
      .framebufferDepthSampleCounts             = sample_counts,
      .framebufferStencilSampleCounts           = sample_counts,
      .framebufferNoAttachmentsSampleCounts     = sample_counts,
      .maxColorAttachments                      = MAX_RTS,
      .sampledImageColorSampleCounts            = sample_counts,
      .sampledImageIntegerSampleCounts          = sample_counts,
      .sampledImageDepthSampleCounts            = sample_counts,
      .sampledImageStencilSampleCounts          = sample_counts,
      .storageImageSampleCounts                 = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords                       = 1,
      .timestampComputeAndGraphics              = true,
      .timestampPeriod                          = 1000000000.0 / devinfo->timestamp_frequency,
      .maxClipDistances                         = 8,
      .maxCullDistances                         = 8,
      .maxCombinedClipAndCullDistances          = 8,
      .discreteQueuePriorities                  = 2,
      .pointSizeRange                           = { 0.125, 255.875 },
      /* While SKL and up support much wider lines than we are setting here,
       * in practice we run into conformance issues if we go past this limit.
       * Since the Windows driver does the same, it's probably fair to assume
       * that no one needs more than this.
       */
      .lineWidthRange                           = { 0.0, 8.0 },
      .pointSizeGranularity                     = (1.0 / 8.0),
      .lineWidthGranularity                     = (1.0 / 128.0),
      .strictLines                              = false,
      .standardSampleLocations                  = true,
      .optimalBufferCopyOffsetAlignment         = 128,
      .optimalBufferCopyRowPitchAlignment       = 128,
      .nonCoherentAtomSize                      = 64,
   };

   *pProperties = (VkPhysicalDeviceProperties) {
      .apiVersion = ANV_API_VERSION,
      .driverVersion = vk_get_driver_version(),
      .vendorID = 0x8086,
      .deviceID = pdevice->info.pci_device_id,
      .deviceType = pdevice->info.has_local_mem ?
                    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU :
                    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = {0}, /* Broadwell doesn't do sparse. */
   };

   snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
            "%s", pdevice->info.name);
   memcpy(pProperties->pipelineCacheUUID,
          pdevice->pipeline_cache_uuid, VK_UUID_SIZE);
}

static void
anv_get_physical_device_properties_1_1(struct anv_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan11Properties *p)
{
   assert(p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);

   memcpy(p->deviceUUID, pdevice->device_uuid, VK_UUID_SIZE);
   memcpy(p->driverUUID, pdevice->driver_uuid, VK_UUID_SIZE);
   memset(p->deviceLUID, 0, VK_LUID_SIZE);
   p->deviceNodeMask = 0;
   p->deviceLUIDValid = false;

   p->subgroupSize = BRW_SUBGROUP_SIZE;
   VkShaderStageFlags scalar_stages = 0;
   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      if (pdevice->compiler->scalar_stage[stage])
         scalar_stages |= mesa_to_vk_shader_stage(stage);
   }
   if (pdevice->vk.supported_extensions.KHR_ray_tracing_pipeline) {
      scalar_stages |= VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_MISS_BIT_KHR |
                       VK_SHADER_STAGE_INTERSECTION_BIT_KHR |
                       VK_SHADER_STAGE_CALLABLE_BIT_KHR;
   }
   if (pdevice->vk.supported_extensions.NV_mesh_shader ||
       pdevice->vk.supported_extensions.EXT_mesh_shader) {
      scalar_stages |= VK_SHADER_STAGE_TASK_BIT_EXT |
                       VK_SHADER_STAGE_MESH_BIT_EXT;
   }
   p->subgroupSupportedStages = scalar_stages;
   p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                    VK_SUBGROUP_FEATURE_VOTE_BIT |
                                    VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                    VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                    VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                    VK_SUBGROUP_FEATURE_QUAD_BIT |
                                    VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                    VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
   p->subgroupQuadOperationsInAllStages = true;

   p->pointClippingBehavior      = VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY;
   p->maxMultiviewViewCount      = 16;
   p->maxMultiviewInstanceIndex  = UINT32_MAX / 16;
   p->protectedNoFault           = false;
   /* This value doesn't matter for us today as our per-stage descriptors are
    * the real limit.
    */
   p->maxPerSetDescriptors       = 1024;
   p->maxMemoryAllocationSize    = MAX_MEMORY_ALLOCATION_SIZE;
}

static void
anv_get_physical_device_properties_1_2(struct anv_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan12Properties *p)
{
   assert(p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);

   p->driverID = VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA;
   memset(p->driverName, 0, sizeof(p->driverName));
   snprintf(p->driverName, VK_MAX_DRIVER_NAME_SIZE,
            "Intel open-source Mesa driver");
   memset(p->driverInfo, 0, sizeof(p->driverInfo));
   snprintf(p->driverInfo, VK_MAX_DRIVER_INFO_SIZE,
            "Mesa " PACKAGE_VERSION MESA_GIT_SHA1);

   p->conformanceVersion = (VkConformanceVersion) {
      .major = 1,
      .minor = 3,
      .subminor = 0,
      .patch = 0,
   };

   p->denormBehaviorIndependence =
      VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
   p->roundingModeIndependence =
      VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;

   /* Broadwell does not support HF denorms and there are restrictions
    * other gens. According to Kabylake's PRM:
    *
    * "math - Extended Math Function
    * [...]
    * Restriction : Half-float denorms are always retained."
    */
   p->shaderDenormFlushToZeroFloat16         = false;
   p->shaderDenormPreserveFloat16            = pdevice->info.ver > 8;
   p->shaderRoundingModeRTEFloat16           = true;
   p->shaderRoundingModeRTZFloat16           = true;
   p->shaderSignedZeroInfNanPreserveFloat16  = true;

   p->shaderDenormFlushToZeroFloat32         = true;
   p->shaderDenormPreserveFloat32            = true;
   p->shaderRoundingModeRTEFloat32           = true;
   p->shaderRoundingModeRTZFloat32           = true;
   p->shaderSignedZeroInfNanPreserveFloat32  = true;

   p->shaderDenormFlushToZeroFloat64         = true;
   p->shaderDenormPreserveFloat64            = true;
   p->shaderRoundingModeRTEFloat64           = true;
   p->shaderRoundingModeRTZFloat64           = true;
   p->shaderSignedZeroInfNanPreserveFloat64  = true;

   /* It's a bit hard to exactly map our implementation to the limits
    * described by Vulkan.  The bindless surface handle in the extended
    * message descriptors is 20 bits and it's an index into the table of
    * RENDER_SURFACE_STATE structs that starts at bindless surface base
    * address.  This means that we can have at must 1M surface states
    * allocated at any given time.  Since most image views take two
    * descriptors, this means we have a limit of about 500K image views.
    *
    * However, since we allocate surface states at vkCreateImageView time,
    * this means our limit is actually something on the order of 500K image
    * views allocated at any time.  The actual limit describe by Vulkan, on
    * the other hand, is a limit of how many you can have in a descriptor set.
    * Assuming anyone using 1M descriptors will be using the same image view
    * twice a bunch of times (or a bunch of null descriptors), we can safely
    * advertise a larger limit here.
    */
   const unsigned max_bindless_views = 1 << 20;
   p->maxUpdateAfterBindDescriptorsInAllPools            = max_bindless_views;
   p->shaderUniformBufferArrayNonUniformIndexingNative   = false;
   p->shaderSampledImageArrayNonUniformIndexingNative    = false;
   p->shaderStorageBufferArrayNonUniformIndexingNative   = true;
   p->shaderStorageImageArrayNonUniformIndexingNative    = false;
   p->shaderInputAttachmentArrayNonUniformIndexingNative = false;
   p->robustBufferAccessUpdateAfterBind                  = true;
   p->quadDivergentImplicitLod                           = false;
   p->maxPerStageDescriptorUpdateAfterBindSamplers       = max_bindless_views;
   p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BUFFERS;
   p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = UINT32_MAX;
   p->maxPerStageDescriptorUpdateAfterBindSampledImages  = max_bindless_views;
   p->maxPerStageDescriptorUpdateAfterBindStorageImages  = max_bindless_views;
   p->maxPerStageDescriptorUpdateAfterBindInputAttachments = MAX_PER_STAGE_DESCRIPTOR_INPUT_ATTACHMENTS;
   p->maxPerStageUpdateAfterBindResources                = UINT32_MAX;
   p->maxDescriptorSetUpdateAfterBindSamplers            = max_bindless_views;
   p->maxDescriptorSetUpdateAfterBindUniformBuffers      = 6 * MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BUFFERS;
   p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = MAX_DYNAMIC_BUFFERS / 2;
   p->maxDescriptorSetUpdateAfterBindStorageBuffers      = UINT32_MAX;
   p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = MAX_DYNAMIC_BUFFERS / 2;
   p->maxDescriptorSetUpdateAfterBindSampledImages       = max_bindless_views;
   p->maxDescriptorSetUpdateAfterBindStorageImages       = max_bindless_views;
   p->maxDescriptorSetUpdateAfterBindInputAttachments    = MAX_DESCRIPTOR_SET_INPUT_ATTACHMENTS;

   /* We support all of the depth resolve modes */
   p->supportedDepthResolveModes    = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT |
                                      VK_RESOLVE_MODE_AVERAGE_BIT |
                                      VK_RESOLVE_MODE_MIN_BIT |
                                      VK_RESOLVE_MODE_MAX_BIT;
   /* Average doesn't make sense for stencil so we don't support that */
   p->supportedStencilResolveModes  = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT |
                                      VK_RESOLVE_MODE_MIN_BIT |
                                      VK_RESOLVE_MODE_MAX_BIT;
   p->independentResolveNone  = true;
   p->independentResolve      = true;

   p->filterMinmaxSingleComponentFormats  = true;
   p->filterMinmaxImageComponentMapping   = true;

   p->maxTimelineSemaphoreValueDifference = UINT64_MAX;

   p->framebufferIntegerColorSampleCounts =
      isl_device_get_sample_counts(&pdevice->isl_dev);
}

static void
anv_get_physical_device_properties_1_3(struct anv_physical_device *pdevice,
                                       VkPhysicalDeviceVulkan13Properties *p)
{
   assert(p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES);

   p->minSubgroupSize = 8;
   p->maxSubgroupSize = 32;
   p->maxComputeWorkgroupSubgroups = pdevice->info.max_cs_workgroup_threads;
   p->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT |
                                   VK_SHADER_STAGE_TASK_BIT_EXT |
                                   VK_SHADER_STAGE_MESH_BIT_EXT;

   p->maxInlineUniformBlockSize = MAX_INLINE_UNIFORM_BLOCK_SIZE;
   p->maxPerStageDescriptorInlineUniformBlocks =
      MAX_INLINE_UNIFORM_BLOCK_DESCRIPTORS;
   p->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks =
      MAX_INLINE_UNIFORM_BLOCK_DESCRIPTORS;
   p->maxDescriptorSetInlineUniformBlocks =
      MAX_INLINE_UNIFORM_BLOCK_DESCRIPTORS;
   p->maxDescriptorSetUpdateAfterBindInlineUniformBlocks =
      MAX_INLINE_UNIFORM_BLOCK_DESCRIPTORS;
   p->maxInlineUniformTotalSize = UINT16_MAX;

   p->integerDotProduct8BitUnsignedAccelerated = false;
   p->integerDotProduct8BitSignedAccelerated = false;
   p->integerDotProduct8BitMixedSignednessAccelerated = false;
   p->integerDotProduct4x8BitPackedUnsignedAccelerated = pdevice->info.ver >= 12;
   p->integerDotProduct4x8BitPackedSignedAccelerated = pdevice->info.ver >= 12;
   p->integerDotProduct4x8BitPackedMixedSignednessAccelerated = pdevice->info.ver >= 12;
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
   p->integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated = pdevice->info.ver >= 12;
   p->integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = pdevice->info.ver >= 12;
   p->integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated = pdevice->info.ver >= 12;
   p->integerDotProductAccumulatingSaturating16BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated = false;

   /* From the SKL PRM Vol. 2d, docs for RENDER_SURFACE_STATE::Surface
    * Base Address:
    *
    *    "For SURFTYPE_BUFFER non-rendertarget surfaces, this field
    *    specifies the base address of the first element of the surface,
    *    computed in software by adding the surface base address to the
    *    byte offset of the element in the buffer. The base address must
    *    be aligned to element size."
    *
    * The typed dataport messages require that things be texel aligned.
    * Otherwise, we may just load/store the wrong data or, in the worst
    * case, there may be hangs.
    */
   p->storageTexelBufferOffsetAlignmentBytes = 16;
   p->storageTexelBufferOffsetSingleTexelAlignment = true;

   /* The sampler, however, is much more forgiving and it can handle
    * arbitrary byte alignment for linear and buffer surfaces.  It's
    * hard to find a good PRM citation for this but years of empirical
    * experience demonstrate that this is true.
    */
   p->uniformTexelBufferOffsetAlignmentBytes = 1;
   p->uniformTexelBufferOffsetSingleTexelAlignment = false;

   p->maxBufferSize = pdevice->isl_dev.max_buffer_size;
}

void anv_GetPhysicalDeviceProperties2(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceProperties2*                pProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);

   anv_GetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

   VkPhysicalDeviceVulkan11Properties core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
   };
   anv_get_physical_device_properties_1_1(pdevice, &core_1_1);

   VkPhysicalDeviceVulkan12Properties core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
   };
   anv_get_physical_device_properties_1_2(pdevice, &core_1_2);

   VkPhysicalDeviceVulkan13Properties core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
   };
   anv_get_physical_device_properties_1_3(pdevice, &core_1_3);

   vk_foreach_struct(ext, pProperties->pNext) {
      if (vk_get_physical_device_core_1_1_property_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_property_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_property_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR: {
         VkPhysicalDeviceAccelerationStructurePropertiesKHR *props = (void *)ext;
         props->maxGeometryCount = (1u << 24) - 1;
         props->maxInstanceCount = (1u << 24) - 1;
         props->maxPrimitiveCount = (1u << 29) - 1;
         props->maxPerStageDescriptorAccelerationStructures = UINT16_MAX;
         props->maxPerStageDescriptorUpdateAfterBindAccelerationStructures = UINT16_MAX;
         props->maxDescriptorSetAccelerationStructures = UINT16_MAX;
         props->maxDescriptorSetUpdateAfterBindAccelerationStructures = UINT16_MAX;
         props->minAccelerationStructureScratchOffsetAlignment = 64;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT: {
         /* TODO: Real limits */
         VkPhysicalDeviceConservativeRasterizationPropertiesEXT *properties =
            (VkPhysicalDeviceConservativeRasterizationPropertiesEXT *)ext;
         /* There's nothing in the public docs about this value as far as I
          * can tell.  However, this is the value the Windows driver reports
          * and there's a comment on a rejected HW feature in the internal
          * docs that says:
          *
          *    "This is similar to conservative rasterization, except the
          *    primitive area is not extended by 1/512 and..."
          *
          * That's a bit of an obtuse reference but it's the best we've got
          * for now.
          */
         properties->primitiveOverestimationSize = 1.0f / 512.0f;
         properties->maxExtraPrimitiveOverestimationSize = 0.0f;
         properties->extraPrimitiveOverestimationSizeGranularity = 0.0f;
         properties->primitiveUnderestimation = false;
         properties->conservativePointAndLineRasterization = false;
         properties->degenerateTrianglesRasterized = true;
         properties->degenerateLinesRasterized = false;
         properties->fullyCoveredFragmentShaderInputVariable = false;
         properties->conservativeRasterizationPostDepthCoverage = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT: {
         VkPhysicalDeviceCustomBorderColorPropertiesEXT *properties =
            (VkPhysicalDeviceCustomBorderColorPropertiesEXT *)ext;
         properties->maxCustomBorderColorSamplers = MAX_CUSTOM_BORDER_COLORS;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR: {
         VkPhysicalDeviceFragmentShadingRatePropertiesKHR *props =
            (VkPhysicalDeviceFragmentShadingRatePropertiesKHR *)ext;
         props->primitiveFragmentShadingRateWithMultipleViewports =
            pdevice->info.has_coarse_pixel_primitive_and_cb;
         props->layeredShadingRateAttachments = pdevice->info.has_coarse_pixel_primitive_and_cb;
         props->fragmentShadingRateNonTrivialCombinerOps =
            pdevice->info.has_coarse_pixel_primitive_and_cb;
         props->maxFragmentSize = (VkExtent2D) { 4, 4 };
         props->maxFragmentSizeAspectRatio =
            pdevice->info.has_coarse_pixel_primitive_and_cb ?
            2 : 4;
         props->maxFragmentShadingRateCoverageSamples = 4 * 4 *
            (pdevice->info.has_coarse_pixel_primitive_and_cb ? 4 : 16);
         props->maxFragmentShadingRateRasterizationSamples =
            pdevice->info.has_coarse_pixel_primitive_and_cb ?
            VK_SAMPLE_COUNT_4_BIT :  VK_SAMPLE_COUNT_16_BIT;
         props->fragmentShadingRateWithShaderDepthStencilWrites = false;
         props->fragmentShadingRateWithSampleMask = true;
         props->fragmentShadingRateWithShaderSampleMask = false;
         props->fragmentShadingRateWithConservativeRasterization = true;
         props->fragmentShadingRateWithFragmentShaderInterlock = true;
         props->fragmentShadingRateWithCustomSampleLocations = true;

         /* Fix in DG2_G10_C0 and DG2_G11_B0. Consider any other Sku as having
          * the fix.
          */
         props->fragmentShadingRateStrictMultiplyCombiner =
            pdevice->info.platform == INTEL_PLATFORM_DG2_G10 ?
            pdevice->info.revision >= 8 :
            pdevice->info.platform == INTEL_PLATFORM_DG2_G11 ?
            pdevice->info.revision >= 4 : true;

         if (pdevice->info.has_coarse_pixel_primitive_and_cb) {
            props->minFragmentShadingRateAttachmentTexelSize = (VkExtent2D) { 8, 8 };
            props->maxFragmentShadingRateAttachmentTexelSize = (VkExtent2D) { 8, 8 };
            props->maxFragmentShadingRateAttachmentTexelSizeAspectRatio = 1;
         } else {
            /* Those must be 0 if attachmentFragmentShadingRate is not
             * supported.
             */
            props->minFragmentShadingRateAttachmentTexelSize = (VkExtent2D) { 0, 0 };
            props->maxFragmentShadingRateAttachmentTexelSize = (VkExtent2D) { 0, 0 };
            props->maxFragmentShadingRateAttachmentTexelSizeAspectRatio = 0;
         }
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

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT: {
         VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *props =
            (VkPhysicalDeviceExtendedDynamicState3PropertiesEXT *) ext;
         props->dynamicPrimitiveTopologyUnrestricted = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT: {
         VkPhysicalDeviceExternalMemoryHostPropertiesEXT *props =
            (VkPhysicalDeviceExternalMemoryHostPropertiesEXT *) ext;
         /* Userptr needs page aligned memory. */
         props->minImportedHostPointerAlignment = 4096;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT: {
         VkPhysicalDeviceLineRasterizationPropertiesEXT *props =
            (VkPhysicalDeviceLineRasterizationPropertiesEXT *)ext;
         /* In the Skylake PRM Vol. 7, subsection titled "GIQ (Diamond)
          * Sampling Rules - Legacy Mode", it says the following:
          *
          *    "Note that the device divides a pixel into a 16x16 array of
          *    subpixels, referenced by their upper left corners."
          *
          * This is the only known reference in the PRMs to the subpixel
          * precision of line rasterization and a "16x16 array of subpixels"
          * implies 4 subpixel precision bits.  Empirical testing has shown
          * that 4 subpixel precision bits applies to all line rasterization
          * types.
          */
         props->lineSubPixelPrecisionBits = 4;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES: {
         VkPhysicalDeviceMaintenance4Properties *properties =
            (VkPhysicalDeviceMaintenance4Properties *)ext;
         properties->maxBufferSize = pdevice->isl_dev.max_buffer_size;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_NV: {
         VkPhysicalDeviceMeshShaderPropertiesNV *props =
            (VkPhysicalDeviceMeshShaderPropertiesNV *)ext;

         /* Bounded by the maximum representable size in
          * 3DSTATE_MESH_SHADER_BODY::SharedLocalMemorySize.  Same for Task.
          */
         const uint32_t max_slm_size = 64 * 1024;

         /* Bounded by the maximum representable size in
          * 3DSTATE_MESH_SHADER_BODY::LocalXMaximum.  Same for Task.
          */
         const uint32_t max_workgroup_size = 1 << 10;

         /* Bounded by the maximum representable count in
          * 3DSTATE_MESH_SHADER_BODY::MaximumPrimitiveCount.
          */
         const uint32_t max_primitives = 1024;

         /* TODO(mesh): Multiview. */
         const uint32_t max_view_count = 1;

         props->maxDrawMeshTasksCount = UINT32_MAX;

         /* TODO(mesh): Implement workgroup Y and Z sizes larger than one by
          * mapping them to/from the single value that HW provides us
          * (currently used for X).
          */

         props->maxTaskWorkGroupInvocations = max_workgroup_size;
         props->maxTaskWorkGroupSize[0] = max_workgroup_size;
         props->maxTaskWorkGroupSize[1] = 1;
         props->maxTaskWorkGroupSize[2] = 1;
         props->maxTaskTotalMemorySize = max_slm_size;
         props->maxTaskOutputCount = UINT16_MAX;

         props->maxMeshWorkGroupInvocations = max_workgroup_size;
         props->maxMeshWorkGroupSize[0] = max_workgroup_size;
         props->maxMeshWorkGroupSize[1] = 1;
         props->maxMeshWorkGroupSize[2] = 1;
         props->maxMeshTotalMemorySize = max_slm_size / max_view_count;
         props->maxMeshOutputPrimitives = max_primitives / max_view_count;
         props->maxMeshMultiviewViewCount = max_view_count;

         /* Depends on what indices can be represented with IndexFormat.  For
          * now we always use U32, so bound to the maximum unique vertices we
          * need for the maximum primitives.
          *
          * TODO(mesh): Revisit this if we drop "U32" IndexFormat when adding
          * support for others.
          */
         props->maxMeshOutputVertices = 3 * props->maxMeshOutputPrimitives;


         props->meshOutputPerVertexGranularity = 32;
         props->meshOutputPerPrimitiveGranularity = 32;

         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT: {
         VkPhysicalDeviceMeshShaderPropertiesEXT *properties =
            (VkPhysicalDeviceMeshShaderPropertiesEXT *)ext;

         /* Bounded by the maximum representable size in
          * 3DSTATE_MESH_SHADER_BODY::SharedLocalMemorySize.  Same for Task.
          */
         const uint32_t max_slm_size = 64 * 1024;

         /* Bounded by the maximum representable size in
          * 3DSTATE_MESH_SHADER_BODY::LocalXMaximum.  Same for Task.
          */
         const uint32_t max_workgroup_size = 1 << 10;

         /* 3DMESH_3D limitation. */
         const uint32_t max_threadgroup_count = 1 << 22;

         /* 3DMESH_3D limitation. */
         const uint32_t max_threadgroup_xyz = 65535;

         const uint32_t max_urb_size = 64 * 1024;

         properties->maxTaskWorkGroupTotalCount = max_threadgroup_count;
         properties->maxTaskWorkGroupCount[0] = max_threadgroup_xyz;
         properties->maxTaskWorkGroupCount[1] = max_threadgroup_xyz;
         properties->maxTaskWorkGroupCount[2] = max_threadgroup_xyz;

         properties->maxTaskWorkGroupInvocations = max_workgroup_size;
         properties->maxTaskWorkGroupSize[0] = max_workgroup_size;
         properties->maxTaskWorkGroupSize[1] = max_workgroup_size;
         properties->maxTaskWorkGroupSize[2] = max_workgroup_size;

         /* TUE header with padding */
         const uint32_t task_payload_reserved = 32;

         properties->maxTaskPayloadSize = max_urb_size - task_payload_reserved;
         properties->maxTaskSharedMemorySize = max_slm_size;
         properties->maxTaskPayloadAndSharedMemorySize =
               properties->maxTaskPayloadSize +
               properties->maxTaskSharedMemorySize;

         properties->maxMeshWorkGroupTotalCount = max_threadgroup_count;
         properties->maxMeshWorkGroupCount[0] = max_threadgroup_xyz;
         properties->maxMeshWorkGroupCount[1] = max_threadgroup_xyz;
         properties->maxMeshWorkGroupCount[2] = max_threadgroup_xyz;

         properties->maxMeshWorkGroupInvocations = max_workgroup_size;
         properties->maxMeshWorkGroupSize[0] = max_workgroup_size;
         properties->maxMeshWorkGroupSize[1] = max_workgroup_size;
         properties->maxMeshWorkGroupSize[2] = max_workgroup_size;

         properties->maxMeshSharedMemorySize = max_slm_size;
         properties->maxMeshPayloadAndSharedMemorySize =
               properties->maxTaskPayloadSize +
               properties->maxMeshSharedMemorySize;

         /* Unfortunately spec's formula for the max output size doesn't match our hardware
          * (because some per-primitive and per-vertex attributes have alignment restrictions),
          * so we have to advertise the minimum value mandated by the spec to not overflow it.
          */
         properties->maxMeshOutputPrimitives = 256;
         properties->maxMeshOutputVertices = 256;

         /* NumPrim + Primitive Data List */
         const uint32_t max_indices_memory =
               ALIGN(sizeof(uint32_t) +
                     sizeof(uint32_t) * properties->maxMeshOutputVertices, 32);

         properties->maxMeshOutputMemorySize = MIN2(max_urb_size - max_indices_memory, 32768);

         properties->maxMeshPayloadAndOutputMemorySize =
               properties->maxTaskPayloadSize +
               properties->maxMeshOutputMemorySize;

         properties->maxMeshOutputComponents = 128;

         /* RTAIndex is 11-bits wide */
         properties->maxMeshOutputLayers = 1 << 11;

         properties->maxMeshMultiviewViewCount = 1;

         /* Elements in Vertex Data Array must be aligned to 32 bytes (8 dwords). */
         properties->meshOutputPerVertexGranularity = 8;
         /* Elements in Primitive Data Array must be aligned to 32 bytes (8 dwords). */
         properties->meshOutputPerPrimitiveGranularity = 8;

         /* SIMD16 */
         properties->maxPreferredTaskWorkGroupInvocations = 16;
         properties->maxPreferredMeshWorkGroupInvocations = 16;

         properties->prefersLocalInvocationVertexOutput = false;
         properties->prefersLocalInvocationPrimitiveOutput = false;
         properties->prefersCompactVertexOutput = false;
         properties->prefersCompactPrimitiveOutput = false;

         /* Spec minimum values */
         assert(properties->maxTaskWorkGroupTotalCount >= (1U << 22));
         assert(properties->maxTaskWorkGroupCount[0] >= 65535);
         assert(properties->maxTaskWorkGroupCount[1] >= 65535);
         assert(properties->maxTaskWorkGroupCount[2] >= 65535);

         assert(properties->maxTaskWorkGroupInvocations >= 128);
         assert(properties->maxTaskWorkGroupSize[0] >= 128);
         assert(properties->maxTaskWorkGroupSize[1] >= 128);
         assert(properties->maxTaskWorkGroupSize[2] >= 128);

         assert(properties->maxTaskPayloadSize >= 16384);
         assert(properties->maxTaskSharedMemorySize >= 32768);
         assert(properties->maxTaskPayloadAndSharedMemorySize >= 32768);


         assert(properties->maxMeshWorkGroupTotalCount >= (1U << 22));
         assert(properties->maxMeshWorkGroupCount[0] >= 65535);
         assert(properties->maxMeshWorkGroupCount[1] >= 65535);
         assert(properties->maxMeshWorkGroupCount[2] >= 65535);

         assert(properties->maxMeshWorkGroupInvocations >= 128);
         assert(properties->maxMeshWorkGroupSize[0] >= 128);
         assert(properties->maxMeshWorkGroupSize[1] >= 128);
         assert(properties->maxMeshWorkGroupSize[2] >= 128);

         assert(properties->maxMeshSharedMemorySize >= 28672);
         assert(properties->maxMeshPayloadAndSharedMemorySize >= 28672);
         assert(properties->maxMeshOutputMemorySize >= 32768);
         assert(properties->maxMeshPayloadAndOutputMemorySize >= 48128);

         assert(properties->maxMeshOutputComponents >= 128);

         assert(properties->maxMeshOutputVertices >= 256);
         assert(properties->maxMeshOutputPrimitives >= 256);
         assert(properties->maxMeshOutputLayers >= 8);
         assert(properties->maxMeshMultiviewViewCount >= 1);

         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT: {
         VkPhysicalDevicePCIBusInfoPropertiesEXT *properties =
            (VkPhysicalDevicePCIBusInfoPropertiesEXT *)ext;
         properties->pciDomain = pdevice->info.pci_domain;
         properties->pciBus = pdevice->info.pci_bus;
         properties->pciDevice = pdevice->info.pci_dev;
         properties->pciFunction = pdevice->info.pci_func;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_PROPERTIES_KHR: {
         VkPhysicalDevicePerformanceQueryPropertiesKHR *properties =
            (VkPhysicalDevicePerformanceQueryPropertiesKHR *)ext;
         /* We could support this by spawning a shader to do the equation
          * normalization.
          */
         properties->allowCommandBufferQueryCopies = false;
         break;
      }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch"
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENTATION_PROPERTIES_ANDROID: {
         VkPhysicalDevicePresentationPropertiesANDROID *props =
            (VkPhysicalDevicePresentationPropertiesANDROID *)ext;
         props->sharedImage = VK_FALSE;
         break;
      }
#pragma GCC diagnostic pop

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT: {
         VkPhysicalDeviceProvokingVertexPropertiesEXT *properties =
            (VkPhysicalDeviceProvokingVertexPropertiesEXT *)ext;
         properties->provokingVertexModePerPipeline = true;
         properties->transformFeedbackPreservesTriangleFanProvokingVertex = false;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
         VkPhysicalDevicePushDescriptorPropertiesKHR *properties =
            (VkPhysicalDevicePushDescriptorPropertiesKHR *) ext;
         properties->maxPushDescriptors = MAX_PUSH_DESCRIPTORS;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR: {
         VkPhysicalDeviceRayTracingPipelinePropertiesKHR *props = (void *)ext;
         /* TODO */
         props->shaderGroupHandleSize = 32;
         props->maxRayRecursionDepth = 31;
         /* MemRay::hitGroupSRStride is 16 bits */
         props->maxShaderGroupStride = UINT16_MAX;
         /* MemRay::hitGroupSRBasePtr requires 16B alignment */
         props->shaderGroupBaseAlignment = 16;
         props->shaderGroupHandleAlignment = 16;
         props->shaderGroupHandleCaptureReplaySize = 32;
         props->maxRayDispatchInvocationCount = 1U << 30; /* required min limit */
         props->maxRayHitAttributeSize = BRW_RT_SIZEOF_HIT_ATTRIB_DATA;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT: {
         VkPhysicalDeviceRobustness2PropertiesEXT *properties = (void *)ext;
         properties->robustStorageBufferAccessSizeAlignment =
            ANV_SSBO_BOUNDS_CHECK_ALIGNMENT;
         properties->robustUniformBufferAccessSizeAlignment =
            ANV_UBO_ALIGNMENT;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT: {
         VkPhysicalDeviceSampleLocationsPropertiesEXT *props =
            (VkPhysicalDeviceSampleLocationsPropertiesEXT *)ext;

         props->sampleLocationSampleCounts =
            isl_device_get_sample_counts(&pdevice->isl_dev);

         /* See also anv_GetPhysicalDeviceMultisamplePropertiesEXT */
         props->maxSampleLocationGridSize.width = 1;
         props->maxSampleLocationGridSize.height = 1;

         props->sampleLocationCoordinateRange[0] = 0;
         props->sampleLocationCoordinateRange[1] = 0.9375;
         props->sampleLocationSubPixelBits = 4;

         props->variableSampleLocations = true;
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

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT: {
         VkPhysicalDeviceTransformFeedbackPropertiesEXT *props =
            (VkPhysicalDeviceTransformFeedbackPropertiesEXT *)ext;

         props->maxTransformFeedbackStreams = MAX_XFB_STREAMS;
         props->maxTransformFeedbackBuffers = MAX_XFB_BUFFERS;
         props->maxTransformFeedbackBufferSize = (1ull << 32);
         props->maxTransformFeedbackStreamDataSize = 128 * 4;
         props->maxTransformFeedbackBufferDataSize = 128 * 4;
         props->maxTransformFeedbackBufferDataStride = 2048;
         props->transformFeedbackQueries = true;
         props->transformFeedbackStreamsLinesTriangles = false;
         props->transformFeedbackRasterizationStreamSelect = false;
         props->transformFeedbackDraw = true;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *props =
            (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)ext;
         /* We have to restrict this a bit for multiview */
         props->maxVertexAttribDivisor = UINT32_MAX / 16;
         break;
      }

      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT: {
         VkPhysicalDeviceMultiDrawPropertiesEXT *props = (VkPhysicalDeviceMultiDrawPropertiesEXT *)ext;
         props->maxMultiDrawCount = 2048;
         break;
      }

      default:
         anv_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

static const VkQueueFamilyProperties
anv_queue_family_properties_template = {
   .timestampValidBits = 36, /* XXX: Real value here */
   .minImageTransferGranularity = { 1, 1, 1 },
};

void anv_GetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice                            physicalDevice,
    uint32_t*                                   pQueueFamilyPropertyCount,
    VkQueueFamilyProperties2*                   pQueueFamilyProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, pdevice, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out,
                          pQueueFamilyProperties, pQueueFamilyPropertyCount);

   for (uint32_t i = 0; i < pdevice->queue.family_count; i++) {
      struct anv_queue_family *queue_family = &pdevice->queue.families[i];
      vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
         p->queueFamilyProperties = anv_queue_family_properties_template;
         p->queueFamilyProperties.queueFlags = queue_family->queueFlags;
         p->queueFamilyProperties.queueCount = queue_family->queueCount;

         vk_foreach_struct(ext, p->pNext) {
            switch (ext->sType) {
            case VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR: {
               VkQueueFamilyGlobalPriorityPropertiesKHR *properties =
                  (VkQueueFamilyGlobalPriorityPropertiesKHR *)ext;

               /* Deliberately sorted low to high */
               VkQueueGlobalPriorityKHR all_priorities[] = {
                  VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR,
                  VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
                  VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
                  VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR,
               };

               uint32_t count = 0;
               for (unsigned i = 0; i < ARRAY_SIZE(all_priorities); i++) {
                  if (all_priorities[i] > pdevice->max_context_priority)
                     break;

                  properties->priorities[count++] = all_priorities[i];
               }
               properties->priorityCount = count;
               break;
            }

            default:
               anv_debug_ignored_stype(ext->sType);
            }
         }
      }
   }
}

void anv_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceMemoryProperties*           pMemoryProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);

   pMemoryProperties->memoryTypeCount = physical_device->memory.type_count;
   for (uint32_t i = 0; i < physical_device->memory.type_count; i++) {
      pMemoryProperties->memoryTypes[i] = (VkMemoryType) {
         .propertyFlags = physical_device->memory.types[i].propertyFlags,
         .heapIndex     = physical_device->memory.types[i].heapIndex,
      };
   }

   pMemoryProperties->memoryHeapCount = physical_device->memory.heap_count;
   for (uint32_t i = 0; i < physical_device->memory.heap_count; i++) {
      pMemoryProperties->memoryHeaps[i] = (VkMemoryHeap) {
         .size    = physical_device->memory.heaps[i].size,
         .flags   = physical_device->memory.heaps[i].flags,
      };
   }
}

static void
anv_get_memory_budget(VkPhysicalDevice physicalDevice,
                      VkPhysicalDeviceMemoryBudgetPropertiesEXT *memoryBudget)
{
   ANV_FROM_HANDLE(anv_physical_device, device, physicalDevice);

   if (!device->vk.supported_extensions.EXT_memory_budget)
      return;

   anv_update_meminfo(device, device->local_fd);

   VkDeviceSize total_sys_heaps_size = 0, total_vram_heaps_size = 0;
   for (size_t i = 0; i < device->memory.heap_count; i++) {
      if (device->memory.heaps[i].is_local_mem) {
         total_vram_heaps_size += device->memory.heaps[i].size;
      } else {
         total_sys_heaps_size += device->memory.heaps[i].size;
      }
   }

   for (size_t i = 0; i < device->memory.heap_count; i++) {
      VkDeviceSize heap_size = device->memory.heaps[i].size;
      VkDeviceSize heap_used = device->memory.heaps[i].used;
      VkDeviceSize heap_budget, total_heaps_size;
      uint64_t mem_available = 0;

      if (device->memory.heaps[i].is_local_mem) {
         total_heaps_size = total_vram_heaps_size;
         if (device->vram_non_mappable.size > 0 && i == 0) {
            mem_available = device->vram_non_mappable.available;
         } else {
            mem_available = device->vram_mappable.available;
         }
      } else {
         total_heaps_size = total_sys_heaps_size;
         mem_available = device->sys.available;
      }

      double heap_proportion = (double) heap_size / total_heaps_size;
      VkDeviceSize available_prop = mem_available * heap_proportion;

      /*
       * Let's not incite the app to starve the system: report at most 90% of
       * the available heap memory.
       */
      uint64_t heap_available = available_prop * 9 / 10;
      heap_budget = MIN2(heap_size, heap_used + heap_available);

      /*
       * Round down to the nearest MB
       */
      heap_budget &= ~((1ull << 20) - 1);

      /*
       * The heapBudget value must be non-zero for array elements less than
       * VkPhysicalDeviceMemoryProperties::memoryHeapCount. The heapBudget
       * value must be less than or equal to VkMemoryHeap::size for each heap.
       */
      assert(0 < heap_budget && heap_budget <= heap_size);

      memoryBudget->heapUsage[i] = heap_used;
      memoryBudget->heapBudget[i] = heap_budget;
   }

   /* The heapBudget and heapUsage values must be zero for array elements
    * greater than or equal to VkPhysicalDeviceMemoryProperties::memoryHeapCount
    */
   for (uint32_t i = device->memory.heap_count; i < VK_MAX_MEMORY_HEAPS; i++) {
      memoryBudget->heapBudget[i] = 0;
      memoryBudget->heapUsage[i] = 0;
   }
}

void anv_GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceMemoryProperties2*          pMemoryProperties)
{
   anv_GetPhysicalDeviceMemoryProperties(physicalDevice,
                                         &pMemoryProperties->memoryProperties);

   vk_foreach_struct(ext, pMemoryProperties->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT:
         anv_get_memory_budget(physicalDevice, (void*)ext);
         break;
      default:
         anv_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

void
anv_GetDeviceGroupPeerMemoryFeatures(
    VkDevice                                    device,
    uint32_t                                    heapIndex,
    uint32_t                                    localDeviceIndex,
    uint32_t                                    remoteDeviceIndex,
    VkPeerMemoryFeatureFlags*                   pPeerMemoryFeatures)
{
   assert(localDeviceIndex == 0 && remoteDeviceIndex == 0);
   *pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT |
                          VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
                          VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT |
                          VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}

PFN_vkVoidFunction anv_GetInstanceProcAddr(
    VkInstance                                  _instance,
    const char*                                 pName)
{
   ANV_FROM_HANDLE(anv_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk,
                                    &anv_instance_entrypoints,
                                    pName);
}

/* With version 1+ of the loader interface the ICD should expose
 * vk_icdGetInstanceProcAddr to work around certain LD_PRELOAD issues seen in apps.
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
   return anv_GetInstanceProcAddr(instance, pName);
}

/* With version 4+ of the loader interface the ICD should expose
 * vk_icdGetPhysicalDeviceProcAddr()
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
    VkInstance  _instance,
    const char* pName);

PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(
    VkInstance  _instance,
    const char* pName)
{
   ANV_FROM_HANDLE(anv_instance, instance, _instance);
   return vk_instance_get_physical_device_proc_addr(&instance->vk, pName);
}

static struct anv_state
anv_state_pool_emit_data(struct anv_state_pool *pool, size_t size, size_t align, const void *p)
{
   struct anv_state state;

   state = anv_state_pool_alloc(pool, size, align);
   memcpy(state.map, p, size);

   return state;
}

static void
anv_device_init_border_colors(struct anv_device *device)
{
   if (device->info->platform == INTEL_PLATFORM_HSW) {
      static const struct hsw_border_color border_colors[] = {
         [VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK] =  { .float32 = { 0.0, 0.0, 0.0, 0.0 } },
         [VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK] =       { .float32 = { 0.0, 0.0, 0.0, 1.0 } },
         [VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE] =       { .float32 = { 1.0, 1.0, 1.0, 1.0 } },
         [VK_BORDER_COLOR_INT_TRANSPARENT_BLACK] =    { .uint32 = { 0, 0, 0, 0 } },
         [VK_BORDER_COLOR_INT_OPAQUE_BLACK] =         { .uint32 = { 0, 0, 0, 1 } },
         [VK_BORDER_COLOR_INT_OPAQUE_WHITE] =         { .uint32 = { 1, 1, 1, 1 } },
      };

      device->border_colors =
         anv_state_pool_emit_data(&device->dynamic_state_pool,
                                  sizeof(border_colors), 512, border_colors);
   } else {
      static const struct gfx8_border_color border_colors[] = {
         [VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK] =  { .float32 = { 0.0, 0.0, 0.0, 0.0 } },
         [VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK] =       { .float32 = { 0.0, 0.0, 0.0, 1.0 } },
         [VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE] =       { .float32 = { 1.0, 1.0, 1.0, 1.0 } },
         [VK_BORDER_COLOR_INT_TRANSPARENT_BLACK] =    { .uint32 = { 0, 0, 0, 0 } },
         [VK_BORDER_COLOR_INT_OPAQUE_BLACK] =         { .uint32 = { 0, 0, 0, 1 } },
         [VK_BORDER_COLOR_INT_OPAQUE_WHITE] =         { .uint32 = { 1, 1, 1, 1 } },
      };

      device->border_colors =
         anv_state_pool_emit_data(&device->dynamic_state_pool,
                                  sizeof(border_colors), 64, border_colors);
   }
}

static VkResult
anv_device_init_trivial_batch(struct anv_device *device)
{
   VkResult result = anv_device_alloc_bo(device, "trivial-batch", 4096,
                                         ANV_BO_ALLOC_MAPPED,
                                         0 /* explicit_address */,
                                         &device->trivial_batch_bo);
   if (result != VK_SUCCESS)
      return result;

   struct anv_batch batch = {
      .start = device->trivial_batch_bo->map,
      .next = device->trivial_batch_bo->map,
      .end = device->trivial_batch_bo->map + 4096,
   };

   anv_batch_emit(&batch, GFX7_MI_BATCH_BUFFER_END, bbe);
   anv_batch_emit(&batch, GFX7_MI_NOOP, noop);

#ifdef SUPPORT_INTEL_INTEGRATED_GPUS
   if (device->physical->memory.need_clflush)
      intel_clflush_range(batch.start, batch.next - batch.start);
#endif

   return VK_SUCCESS;
}

static bool
get_bo_from_pool(struct intel_batch_decode_bo *ret,
                 struct anv_block_pool *pool,
                 uint64_t address)
{
   anv_block_pool_foreach_bo(bo, pool) {
      uint64_t bo_address = intel_48b_address(bo->offset);
      if (address >= bo_address && address < (bo_address + bo->size)) {
         *ret = (struct intel_batch_decode_bo) {
            .addr = bo_address,
            .size = bo->size,
            .map = bo->map,
         };
         return true;
      }
   }
   return false;
}

/* Finding a buffer for batch decoding */
static struct intel_batch_decode_bo
decode_get_bo(void *v_batch, bool ppgtt, uint64_t address)
{
   struct anv_device *device = v_batch;
   struct intel_batch_decode_bo ret_bo = {};

   assert(ppgtt);

   if (get_bo_from_pool(&ret_bo, &device->dynamic_state_pool.block_pool, address))
      return ret_bo;
   if (get_bo_from_pool(&ret_bo, &device->instruction_state_pool.block_pool, address))
      return ret_bo;
   if (get_bo_from_pool(&ret_bo, &device->binding_table_pool.block_pool, address))
      return ret_bo;
   if (get_bo_from_pool(&ret_bo, &device->scratch_surface_state_pool.block_pool, address))
      return ret_bo;
   if (get_bo_from_pool(&ret_bo, &device->bindless_surface_state_pool.block_pool, address))
      return ret_bo;
   if (get_bo_from_pool(&ret_bo, &device->internal_surface_state_pool.block_pool, address))
      return ret_bo;

   if (!device->cmd_buffer_being_decoded)
      return (struct intel_batch_decode_bo) { };

   struct anv_batch_bo **bo;

   u_vector_foreach(bo, &device->cmd_buffer_being_decoded->seen_bbos) {
      /* The decoder zeroes out the top 16 bits, so we need to as well */
      uint64_t bo_address = (*bo)->bo->offset & (~0ull >> 16);

      if (address >= bo_address && address < bo_address + (*bo)->bo->size) {
         return (struct intel_batch_decode_bo) {
            .addr = bo_address,
            .size = (*bo)->bo->size,
            .map = (*bo)->bo->map,
         };
      }
   }

   return (struct intel_batch_decode_bo) { };
}

struct intel_aux_map_buffer {
   struct intel_buffer base;
   struct anv_state state;
};

static struct intel_buffer *
intel_aux_map_buffer_alloc(void *driver_ctx, uint32_t size)
{
   struct intel_aux_map_buffer *buf = malloc(sizeof(struct intel_aux_map_buffer));
   if (!buf)
      return NULL;

   struct anv_device *device = (struct anv_device*)driver_ctx;
   assert(device->physical->supports_48bit_addresses);

   struct anv_state_pool *pool = &device->dynamic_state_pool;
   buf->state = anv_state_pool_alloc(pool, size, size);

   buf->base.gpu = pool->block_pool.bo->offset + buf->state.offset;
   buf->base.gpu_end = buf->base.gpu + buf->state.alloc_size;
   buf->base.map = buf->state.map;
   buf->base.driver_bo = &buf->state;
   return &buf->base;
}

static void
intel_aux_map_buffer_free(void *driver_ctx, struct intel_buffer *buffer)
{
   struct intel_aux_map_buffer *buf = (struct intel_aux_map_buffer*)buffer;
   struct anv_device *device = (struct anv_device*)driver_ctx;
   struct anv_state_pool *pool = &device->dynamic_state_pool;
   anv_state_pool_free(pool, buf->state);
   free(buf);
}

static struct intel_mapped_pinned_buffer_alloc aux_map_allocator = {
   .alloc = intel_aux_map_buffer_alloc,
   .free = intel_aux_map_buffer_free,
};

static VkResult anv_device_check_status(struct vk_device *vk_device);

static VkResult
anv_device_setup_context(struct anv_device *device,
                         const VkDeviceCreateInfo *pCreateInfo,
                         const uint32_t num_queues)
{
   struct anv_physical_device *physical_device = device->physical;
   VkResult result = VK_SUCCESS;

   if (device->physical->engine_info) {
      /* The kernel API supports at most 64 engines */
      assert(num_queues <= 64);
      enum intel_engine_class engine_classes[64];
      int engine_count = 0;
      for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
         const VkDeviceQueueCreateInfo *queueCreateInfo =
            &pCreateInfo->pQueueCreateInfos[i];

         assert(queueCreateInfo->queueFamilyIndex <
                physical_device->queue.family_count);
         struct anv_queue_family *queue_family =
            &physical_device->queue.families[queueCreateInfo->queueFamilyIndex];

         for (uint32_t j = 0; j < queueCreateInfo->queueCount; j++)
            engine_classes[engine_count++] = queue_family->engine_class;
      }
      if (!intel_gem_create_context_engines(device->fd,
                                            physical_device->engine_info,
                                            engine_count, engine_classes,
                                            (uint32_t *)&device->context_id))
         result = vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                            "kernel context creation failed");
   } else {
      assert(num_queues == 1);
      if (!intel_gem_create_context(device->fd, &device->context_id))
         result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   }

   if (result != VK_SUCCESS)
      return result;

   /* Here we tell the kernel not to attempt to recover our context but
    * immediately (on the next batchbuffer submission) report that the
    * context is lost, and we will do the recovery ourselves.  In the case
    * of Vulkan, recovery means throwing VK_ERROR_DEVICE_LOST and letting
    * the client clean up the pieces.
    */
   anv_gem_set_context_param(device->fd, device->context_id,
                             I915_CONTEXT_PARAM_RECOVERABLE, false);

   /* Check if client specified queue priority. */
   const VkDeviceQueueGlobalPriorityCreateInfoKHR *queue_priority =
      vk_find_struct_const(pCreateInfo->pQueueCreateInfos[0].pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR);

   VkQueueGlobalPriorityKHR priority =
      queue_priority ? queue_priority->globalPriority :
         VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;

   /* As per spec, the driver implementation may deny requests to acquire
    * a priority above the default priority (MEDIUM) if the caller does not
    * have sufficient privileges. In this scenario VK_ERROR_NOT_PERMITTED_KHR
    * is returned.
    */
   if (physical_device->max_context_priority >= VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR) {
      int err = anv_gem_set_context_param(device->fd, device->context_id,
                                          I915_CONTEXT_PARAM_PRIORITY,
                                          priority);
      if (err != 0 && priority > VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR) {
         result = vk_error(device, VK_ERROR_NOT_PERMITTED_KHR);
         goto fail_context;
      }
   }

   return result;

fail_context:
   intel_gem_destroy_context(device->fd, device->context_id);
   return result;
}

VkResult anv_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);
   VkResult result;
   struct anv_device *device;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   /* Check enabled features */
   bool robust_buffer_access = false;
   if (pCreateInfo->pEnabledFeatures) {
      if (pCreateInfo->pEnabledFeatures->robustBufferAccess)
         robust_buffer_access = true;
   }

   vk_foreach_struct_const(ext, pCreateInfo->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
         const VkPhysicalDeviceFeatures2 *features = (const void *)ext;
         if (features->features.robustBufferAccess)
            robust_buffer_access = true;
         break;
      }

      default:
         /* Don't warn */
         break;
      }
   }

   /* Check requested queues and fail if we are requested to create any
    * queues with flags we don't support.
    */
   assert(pCreateInfo->queueCreateInfoCount > 0);
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      if (pCreateInfo->pQueueCreateInfos[i].flags != 0)
         return vk_error(physical_device, VK_ERROR_INITIALIZATION_FAILED);
   }

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;

   bool override_initial_entrypoints = true;
   if (physical_device->instance->vk.app_info.app_name &&
       !strcmp(physical_device->instance->vk.app_info.app_name, "HITMAN3.exe")) {
      vk_device_dispatch_table_from_entrypoints(&dispatch_table, &hitman3_device_entrypoints, true);
      override_initial_entrypoints = false;
   }
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      anv_genX(&physical_device->info, device_entrypoints),
      override_initial_entrypoints);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      &anv_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      &wsi_device_entrypoints, false);

   result = vk_device_init(&device->vk, &physical_device->vk,
                           &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   if (INTEL_DEBUG(DEBUG_BATCH)) {
      const unsigned decode_flags =
         INTEL_BATCH_DECODE_FULL |
         (INTEL_DEBUG(DEBUG_COLOR) ? INTEL_BATCH_DECODE_IN_COLOR : 0) |
         INTEL_BATCH_DECODE_OFFSETS |
         INTEL_BATCH_DECODE_FLOATS;

      intel_batch_decode_ctx_init(&device->decoder_ctx,
                                  &physical_device->compiler->isa,
                                  &physical_device->info,
                                  stderr, decode_flags, NULL,
                                  decode_get_bo, NULL, device);

      device->decoder_ctx.dynamic_base = DYNAMIC_STATE_POOL_MIN_ADDRESS;
      device->decoder_ctx.surface_base = INTERNAL_SURFACE_STATE_POOL_MIN_ADDRESS;
      device->decoder_ctx.instruction_base =
         INSTRUCTION_STATE_POOL_MIN_ADDRESS;
   }

   anv_device_set_physical(device, physical_device);

   /* XXX(chadv): Can we dup() physicalDevice->fd here? */
   device->fd = open(physical_device->path, O_RDWR | O_CLOEXEC);
   if (device->fd == -1) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_device;
   }

   device->vk.command_buffer_ops = &anv_cmd_buffer_ops;
   device->vk.check_status = anv_device_check_status;
   device->vk.create_sync_for_memory = anv_create_sync_for_memory;
   vk_device_set_drm_fd(&device->vk, device->fd);

   uint32_t num_queues = 0;
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
      num_queues += pCreateInfo->pQueueCreateInfos[i].queueCount;

   result = anv_device_setup_context(device, pCreateInfo, num_queues);
   if (result != VK_SUCCESS)
      goto fail_fd;

   device->queues =
      vk_zalloc(&device->vk.alloc, num_queues * sizeof(*device->queues), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (device->queues == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_context_id;
   }

   device->queue_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queueCreateInfo =
         &pCreateInfo->pQueueCreateInfos[i];

      for (uint32_t j = 0; j < queueCreateInfo->queueCount; j++) {
         /* When using legacy contexts, we use I915_EXEC_RENDER but, with
          * engine-based contexts, the bottom 6 bits of exec_flags are used
          * for the engine ID.
          */
         uint32_t exec_flags = device->physical->engine_info ?
                               device->queue_count : I915_EXEC_RENDER;

         result = anv_queue_init(device, &device->queues[device->queue_count],
                                 exec_flags, queueCreateInfo, j);
         if (result != VK_SUCCESS)
            goto fail_queues;

         device->queue_count++;
      }
   }

   if (pthread_mutex_init(&device->vma_mutex, NULL) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_queues;
   }

   /* keep the page with address zero out of the allocator */
   util_vma_heap_init(&device->vma_lo,
                      LOW_HEAP_MIN_ADDRESS, LOW_HEAP_SIZE);

   util_vma_heap_init(&device->vma_cva, CLIENT_VISIBLE_HEAP_MIN_ADDRESS,
                      CLIENT_VISIBLE_HEAP_SIZE);

   /* Leave the last 4GiB out of the high vma range, so that no state
    * base address + size can overflow 48 bits. For more information see
    * the comment about Wa32bitGeneralStateOffset in anv_allocator.c
    */
   util_vma_heap_init(&device->vma_hi, HIGH_HEAP_MIN_ADDRESS,
                      physical_device->gtt_size - (1ull << 32) -
                      HIGH_HEAP_MIN_ADDRESS);

   list_inithead(&device->memory_objects);

   device->robust_buffer_access = robust_buffer_access;

   if (pthread_mutex_init(&device->mutex, NULL) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_vmas;
   }

   pthread_condattr_t condattr;
   if (pthread_condattr_init(&condattr) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   if (pthread_cond_init(&device->queue_submit, &condattr) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   pthread_condattr_destroy(&condattr);

   result = anv_bo_cache_init(&device->bo_cache, device);
   if (result != VK_SUCCESS)
      goto fail_queue_cond;

   anv_bo_pool_init(&device->batch_bo_pool, device, "batch");

   /* Because scratch is also relative to General State Base Address, we leave
    * the base address 0 and start the pool memory at an offset.  This way we
    * get the correct offsets in the anv_states that get allocated from it.
    */
   result = anv_state_pool_init(&device->general_state_pool, device,
                                "general pool",
                                0, GENERAL_STATE_POOL_MIN_ADDRESS, 16384);
   if (result != VK_SUCCESS)
      goto fail_batch_bo_pool;

   result = anv_state_pool_init(&device->dynamic_state_pool, device,
                                "dynamic pool",
                                DYNAMIC_STATE_POOL_MIN_ADDRESS, 0, 16384);
   if (result != VK_SUCCESS)
      goto fail_general_state_pool;

   /* The border color pointer is limited to 24 bits, so we need to make
    * sure that any such color used at any point in the program doesn't
    * exceed that limit.
    * We achieve that by reserving all the custom border colors we support
    * right off the bat, so they are close to the base address.
    */
   anv_state_reserved_pool_init(&device->custom_border_colors,
                                &device->dynamic_state_pool,
                                MAX_CUSTOM_BORDER_COLORS,
                                sizeof(struct gfx8_border_color), 64);

   result = anv_state_pool_init(&device->instruction_state_pool, device,
                                "instruction pool",
                                INSTRUCTION_STATE_POOL_MIN_ADDRESS, 0, 16384);
   if (result != VK_SUCCESS)
      goto fail_dynamic_state_pool;

   if (device->info->verx10 >= 125) {
      /* Put the scratch surface states at the beginning of the internal
       * surface state pool.
       */
      result = anv_state_pool_init(&device->scratch_surface_state_pool, device,
                                   "scratch surface state pool",
                                   SCRATCH_SURFACE_STATE_POOL_MIN_ADDRESS, 0, 4096);
      if (result != VK_SUCCESS)
         goto fail_instruction_state_pool;

      result = anv_state_pool_init(&device->internal_surface_state_pool, device,
                                   "internal surface state pool",
                                   INTERNAL_SURFACE_STATE_POOL_MIN_ADDRESS,
                                   SCRATCH_SURFACE_STATE_POOL_SIZE, 4096);
   } else {
      result = anv_state_pool_init(&device->internal_surface_state_pool, device,
                                   "internal surface state pool",
                                   INTERNAL_SURFACE_STATE_POOL_MIN_ADDRESS, 0, 4096);
   }
   if (result != VK_SUCCESS)
      goto fail_scratch_surface_state_pool;

   result = anv_state_pool_init(&device->bindless_surface_state_pool, device,
                                "bindless surface state pool",
                                BINDLESS_SURFACE_STATE_POOL_MIN_ADDRESS, 0, 4096);
   if (result != VK_SUCCESS)
      goto fail_internal_surface_state_pool;

   if (device->info->verx10 >= 125) {
      /* We're using 3DSTATE_BINDING_TABLE_POOL_ALLOC to give the binding
       * table its own base address separately from surface state base.
       */
      result = anv_state_pool_init(&device->binding_table_pool, device,
                                   "binding table pool",
                                   BINDING_TABLE_POOL_MIN_ADDRESS, 0,
                                   BINDING_TABLE_POOL_BLOCK_SIZE);
   } else {
      int64_t bt_pool_offset = (int64_t)BINDING_TABLE_POOL_MIN_ADDRESS -
                               (int64_t)INTERNAL_SURFACE_STATE_POOL_MIN_ADDRESS;
      assert(INT32_MIN < bt_pool_offset && bt_pool_offset < 0);
      result = anv_state_pool_init(&device->binding_table_pool, device,
                                   "binding table pool",
                                   INTERNAL_SURFACE_STATE_POOL_MIN_ADDRESS,
                                   bt_pool_offset,
                                   BINDING_TABLE_POOL_BLOCK_SIZE);
   }
   if (result != VK_SUCCESS)
      goto fail_bindless_surface_state_pool;

   if (device->info->has_aux_map) {
      device->aux_map_ctx = intel_aux_map_init(device, &aux_map_allocator,
                                               &physical_device->info);
      if (!device->aux_map_ctx)
         goto fail_binding_table_pool;
   }

   result = anv_device_alloc_bo(device, "workaround", 4096,
                                ANV_BO_ALLOC_CAPTURE |
                                ANV_BO_ALLOC_MAPPED |
                                (device->info->has_local_mem ?
                                 ANV_BO_ALLOC_WRITE_COMBINE : 0),
                                0 /* explicit_address */,
                                &device->workaround_bo);
   if (result != VK_SUCCESS)
      goto fail_surface_aux_map_pool;

   device->workaround_address = (struct anv_address) {
      .bo = device->workaround_bo,
      .offset = align_u32(
         intel_debug_write_identifiers(device->workaround_bo->map,
                                       device->workaround_bo->size,
                                       "Anv") + 8, 8),
   };

   device->rt_uuid_addr = anv_address_add(device->workaround_address, 8);
   memcpy(device->rt_uuid_addr.bo->map + device->rt_uuid_addr.offset,
          physical_device->rt_uuid,
          sizeof(physical_device->rt_uuid));

   device->debug_frame_desc =
      intel_debug_get_identifier_block(device->workaround_bo->map,
                                       device->workaround_bo->size,
                                       INTEL_DEBUG_BLOCK_TYPE_FRAME);

   if (device->vk.enabled_extensions.KHR_ray_query) {
      uint32_t ray_queries_size =
         align_u32(brw_rt_ray_queries_hw_stacks_size(device->info), 4096);

      result = anv_device_alloc_bo(device, "ray queries",
                                   ray_queries_size,
                                   0,
                                   0 /* explicit_address */,
                                   &device->ray_query_bo);
      if (result != VK_SUCCESS)
         goto fail_workaround_bo;
   }

   result = anv_device_init_trivial_batch(device);
   if (result != VK_SUCCESS)
      goto fail_ray_query_bo;

   /* Emit the CPS states before running the initialization batch as those
    * structures are referenced.
    */
   if (device->info->ver >= 12) {
      uint32_t n_cps_states = 3 * 3; /* All combinaisons of X by Y CP sizes (1, 2, 4) */

      if (device->info->has_coarse_pixel_primitive_and_cb)
         n_cps_states *= 5 * 5; /* 5 combiners by 2 operators */

      n_cps_states += 1; /* Disable CPS */

       /* Each of the combinaison must be replicated on all viewports */
      n_cps_states *= MAX_VIEWPORTS;

      device->cps_states =
         anv_state_pool_alloc(&device->dynamic_state_pool,
                              n_cps_states * CPS_STATE_length(device->info) * 4,
                              32);
      if (device->cps_states.map == NULL)
         goto fail_trivial_batch;

      anv_genX(device->info, init_cps_device_state)(device);
   }

   /* Allocate a null surface state at surface state offset 0.  This makes
    * NULL descriptor handling trivial because we can just memset structures
    * to zero and they have a valid descriptor.
    */
   device->null_surface_state =
      anv_state_pool_alloc(device->info->verx10 >= 125 ?
                           &device->scratch_surface_state_pool :
                           &device->internal_surface_state_pool,
                           device->isl_dev.ss.size,
                           device->isl_dev.ss.align);
   isl_null_fill_state(&device->isl_dev, device->null_surface_state.map,
                       .size = isl_extent3d(1, 1, 1) /* This shouldn't matter */);
   assert(device->null_surface_state.offset == 0);

   anv_scratch_pool_init(device, &device->scratch_pool);

   /* TODO(RT): Do we want some sort of data structure for this? */
   memset(device->rt_scratch_bos, 0, sizeof(device->rt_scratch_bos));

   if (ANV_SUPPORT_RT && device->info->has_ray_tracing) {
      /* The docs say to always allocate 128KB per DSS */
      const uint32_t btd_fifo_bo_size =
         128 * 1024 * intel_device_info_dual_subslice_id_bound(device->info);
      result = anv_device_alloc_bo(device,
                                   "rt-btd-fifo",
                                   btd_fifo_bo_size,
                                   0 /* alloc_flags */,
                                   0 /* explicit_address */,
                                   &device->btd_fifo_bo);
      if (result != VK_SUCCESS)
         goto fail_trivial_batch_bo_and_scratch_pool;
   }

   result = anv_genX(device->info, init_device_state)(device);
   if (result != VK_SUCCESS)
      goto fail_btd_fifo_bo;

   struct vk_pipeline_cache_create_info pcc_info = { };
   device->default_pipeline_cache =
      vk_pipeline_cache_create(&device->vk, &pcc_info, NULL);
   if (!device->default_pipeline_cache) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_btd_fifo_bo;
   }

   /* Internal shaders need their own pipeline cache because, unlike the rest
    * of ANV, it won't work at all without the cache. It depends on it for
    * shaders to remain resident while it runs. Therefore, we need a special
    * cache just for BLORP/RT that's forced to always be enabled.
    */
   pcc_info.force_enable = true;
   device->internal_cache =
      vk_pipeline_cache_create(&device->vk, &pcc_info, NULL);
   if (device->internal_cache == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_default_pipeline_cache;
   }

   /* The device (currently is ICL/TGL) does not have float64 support. */
   if (!device->info->has_64bit_float &&
      device->physical->instance->fp64_workaround_enabled)
      anv_load_fp64_shader(device);

   result = anv_device_init_rt_shaders(device);
   if (result != VK_SUCCESS) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_internal_cache;
   }

   anv_device_init_blorp(device);

   anv_device_init_border_colors(device);

   anv_device_perf_init(device);

   anv_device_utrace_init(device);

   *pDevice = anv_device_to_handle(device);

   return VK_SUCCESS;

 fail_internal_cache:
   vk_pipeline_cache_destroy(device->internal_cache, NULL);
 fail_default_pipeline_cache:
   vk_pipeline_cache_destroy(device->default_pipeline_cache, NULL);
 fail_btd_fifo_bo:
   if (ANV_SUPPORT_RT && device->info->has_ray_tracing)
      anv_device_release_bo(device, device->btd_fifo_bo);
 fail_trivial_batch_bo_and_scratch_pool:
   anv_scratch_pool_finish(device, &device->scratch_pool);
 fail_trivial_batch:
   anv_device_release_bo(device, device->trivial_batch_bo);
 fail_ray_query_bo:
   if (device->ray_query_bo)
      anv_device_release_bo(device, device->ray_query_bo);
 fail_workaround_bo:
   anv_device_release_bo(device, device->workaround_bo);
 fail_surface_aux_map_pool:
   if (device->info->has_aux_map) {
      intel_aux_map_finish(device->aux_map_ctx);
      device->aux_map_ctx = NULL;
   }
 fail_binding_table_pool:
   anv_state_pool_finish(&device->binding_table_pool);
 fail_bindless_surface_state_pool:
   anv_state_pool_finish(&device->bindless_surface_state_pool);
 fail_internal_surface_state_pool:
   anv_state_pool_finish(&device->internal_surface_state_pool);
 fail_scratch_surface_state_pool:
   if (device->info->verx10 >= 125)
      anv_state_pool_finish(&device->scratch_surface_state_pool);
 fail_instruction_state_pool:
   anv_state_pool_finish(&device->instruction_state_pool);
 fail_dynamic_state_pool:
   anv_state_reserved_pool_finish(&device->custom_border_colors);
   anv_state_pool_finish(&device->dynamic_state_pool);
 fail_general_state_pool:
   anv_state_pool_finish(&device->general_state_pool);
 fail_batch_bo_pool:
   anv_bo_pool_finish(&device->batch_bo_pool);
   anv_bo_cache_finish(&device->bo_cache);
 fail_queue_cond:
   pthread_cond_destroy(&device->queue_submit);
 fail_mutex:
   pthread_mutex_destroy(&device->mutex);
 fail_vmas:
   util_vma_heap_finish(&device->vma_hi);
   util_vma_heap_finish(&device->vma_cva);
   util_vma_heap_finish(&device->vma_lo);
 fail_queues:
   for (uint32_t i = 0; i < device->queue_count; i++)
      anv_queue_finish(&device->queues[i]);
   vk_free(&device->vk.alloc, device->queues);
 fail_context_id:
   intel_gem_destroy_context(device->fd, device->context_id);
 fail_fd:
   close(device->fd);
 fail_device:
   vk_device_finish(&device->vk);
 fail_alloc:
   vk_free(&device->vk.alloc, device);

   return result;
}

void anv_DestroyDevice(
    VkDevice                                    _device,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   if (!device)
      return;

   anv_device_utrace_finish(device);

   anv_device_finish_blorp(device);

   anv_device_finish_rt_shaders(device);

   vk_pipeline_cache_destroy(device->internal_cache, NULL);
   vk_pipeline_cache_destroy(device->default_pipeline_cache, NULL);

   if (ANV_SUPPORT_RT && device->info->has_ray_tracing)
      anv_device_release_bo(device, device->btd_fifo_bo);

#ifdef HAVE_VALGRIND
   /* We only need to free these to prevent valgrind errors.  The backing
    * BO will go away in a couple of lines so we don't actually leak.
    */
   anv_state_reserved_pool_finish(&device->custom_border_colors);
   anv_state_pool_free(&device->dynamic_state_pool, device->border_colors);
   anv_state_pool_free(&device->dynamic_state_pool, device->slice_hash);
   anv_state_pool_free(&device->dynamic_state_pool, device->cps_states);
#endif

   for (unsigned i = 0; i < ARRAY_SIZE(device->rt_scratch_bos); i++) {
      if (device->rt_scratch_bos[i] != NULL)
         anv_device_release_bo(device, device->rt_scratch_bos[i]);
   }

   anv_scratch_pool_finish(device, &device->scratch_pool);

   if (device->vk.enabled_extensions.KHR_ray_query) {
      for (unsigned i = 0; i < ARRAY_SIZE(device->ray_query_shadow_bos); i++) {
         if (device->ray_query_shadow_bos[i] != NULL)
            anv_device_release_bo(device, device->ray_query_shadow_bos[i]);
      }
      anv_device_release_bo(device, device->ray_query_bo);
   }
   anv_device_release_bo(device, device->workaround_bo);
   anv_device_release_bo(device, device->trivial_batch_bo);

   if (device->info->has_aux_map) {
      intel_aux_map_finish(device->aux_map_ctx);
      device->aux_map_ctx = NULL;
   }

   anv_state_pool_finish(&device->binding_table_pool);
   if (device->info->verx10 >= 125)
      anv_state_pool_finish(&device->scratch_surface_state_pool);
   anv_state_pool_finish(&device->internal_surface_state_pool);
   anv_state_pool_finish(&device->bindless_surface_state_pool);
   anv_state_pool_finish(&device->instruction_state_pool);
   anv_state_pool_finish(&device->dynamic_state_pool);
   anv_state_pool_finish(&device->general_state_pool);

   anv_bo_pool_finish(&device->batch_bo_pool);

   anv_bo_cache_finish(&device->bo_cache);

   util_vma_heap_finish(&device->vma_hi);
   util_vma_heap_finish(&device->vma_cva);
   util_vma_heap_finish(&device->vma_lo);

   pthread_cond_destroy(&device->queue_submit);
   pthread_mutex_destroy(&device->mutex);

   for (uint32_t i = 0; i < device->queue_count; i++)
      anv_queue_finish(&device->queues[i]);
   vk_free(&device->vk.alloc, device->queues);

   intel_gem_destroy_context(device->fd, device->context_id);

   if (INTEL_DEBUG(DEBUG_BATCH))
      intel_batch_decode_ctx_finish(&device->decoder_ctx);

   close(device->fd);

   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
}

VkResult anv_EnumerateInstanceLayerProperties(
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

static VkResult
anv_device_check_status(struct vk_device *vk_device)
{
   struct anv_device *device = container_of(vk_device, struct anv_device, vk);

   uint32_t active, pending;
   int ret = anv_gem_context_get_reset_stats(device->fd, device->context_id,
                                             &active, &pending);
   if (ret == -1) {
      /* We don't know the real error. */
      return vk_device_set_lost(&device->vk, "get_reset_stats failed: %m");
   }

   if (active) {
      return vk_device_set_lost(&device->vk, "GPU hung on one of our command buffers");
   } else if (pending) {
      return vk_device_set_lost(&device->vk, "GPU hung with commands in-flight");
   }

   return VK_SUCCESS;
}

VkResult
anv_device_wait(struct anv_device *device, struct anv_bo *bo,
                int64_t timeout)
{
   int ret = anv_gem_wait(device, bo->gem_handle, &timeout);
   if (ret == -1 && errno == ETIME) {
      return VK_TIMEOUT;
   } else if (ret == -1) {
      /* We don't know the real error. */
      return vk_device_set_lost(&device->vk, "gem wait failed: %m");
   } else {
      return VK_SUCCESS;
   }
}

uint64_t
anv_vma_alloc(struct anv_device *device,
              uint64_t size, uint64_t align,
              enum anv_bo_alloc_flags alloc_flags,
              uint64_t client_address)
{
   pthread_mutex_lock(&device->vma_mutex);

   uint64_t addr = 0;

   if (alloc_flags & ANV_BO_ALLOC_CLIENT_VISIBLE_ADDRESS) {
      if (client_address) {
         if (util_vma_heap_alloc_addr(&device->vma_cva,
                                      client_address, size)) {
            addr = client_address;
         }
      } else {
         addr = util_vma_heap_alloc(&device->vma_cva, size, align);
      }
      /* We don't want to fall back to other heaps */
      goto done;
   }

   assert(client_address == 0);

   if (!(alloc_flags & ANV_BO_ALLOC_32BIT_ADDRESS))
      addr = util_vma_heap_alloc(&device->vma_hi, size, align);

   if (addr == 0)
      addr = util_vma_heap_alloc(&device->vma_lo, size, align);

done:
   pthread_mutex_unlock(&device->vma_mutex);

   assert(addr == intel_48b_address(addr));
   return intel_canonical_address(addr);
}

void
anv_vma_free(struct anv_device *device,
             uint64_t address, uint64_t size)
{
   const uint64_t addr_48b = intel_48b_address(address);

   pthread_mutex_lock(&device->vma_mutex);

   if (addr_48b >= LOW_HEAP_MIN_ADDRESS &&
       addr_48b <= LOW_HEAP_MAX_ADDRESS) {
      util_vma_heap_free(&device->vma_lo, addr_48b, size);
   } else if (addr_48b >= CLIENT_VISIBLE_HEAP_MIN_ADDRESS &&
              addr_48b <= CLIENT_VISIBLE_HEAP_MAX_ADDRESS) {
      util_vma_heap_free(&device->vma_cva, addr_48b, size);
   } else {
      assert(addr_48b >= HIGH_HEAP_MIN_ADDRESS);
      util_vma_heap_free(&device->vma_hi, addr_48b, size);
   }

   pthread_mutex_unlock(&device->vma_mutex);
}

VkResult anv_AllocateMemory(
    VkDevice                                    _device,
    const VkMemoryAllocateInfo*                 pAllocateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDeviceMemory*                             pMem)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_physical_device *pdevice = device->physical;
   struct anv_device_memory *mem;
   VkResult result = VK_SUCCESS;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   /* The Vulkan 1.0.33 spec says "allocationSize must be greater than 0". */
   assert(pAllocateInfo->allocationSize > 0);

   VkDeviceSize aligned_alloc_size =
      align_u64(pAllocateInfo->allocationSize, 4096);

   if (aligned_alloc_size > MAX_MEMORY_ALLOCATION_SIZE)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   assert(pAllocateInfo->memoryTypeIndex < pdevice->memory.type_count);
   struct anv_memory_type *mem_type =
      &pdevice->memory.types[pAllocateInfo->memoryTypeIndex];
   assert(mem_type->heapIndex < pdevice->memory.heap_count);
   struct anv_memory_heap *mem_heap =
      &pdevice->memory.heaps[mem_type->heapIndex];

   uint64_t mem_heap_used = p_atomic_read(&mem_heap->used);
   if (mem_heap_used + aligned_alloc_size > mem_heap->size)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   mem = vk_object_alloc(&device->vk, pAllocator, sizeof(*mem),
                         VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (mem == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   mem->type = mem_type;
   mem->map = NULL;
   mem->map_size = 0;
   mem->map_delta = 0;
   mem->ahw = NULL;
   mem->host_ptr = NULL;

   enum anv_bo_alloc_flags alloc_flags = 0;

   const VkExportMemoryAllocateInfo *export_info = NULL;
   const VkImportAndroidHardwareBufferInfoANDROID *ahw_import_info = NULL;
   const VkImportMemoryFdInfoKHR *fd_info = NULL;
   const VkImportMemoryHostPointerInfoEXT *host_ptr_info = NULL;
   const VkMemoryDedicatedAllocateInfo *dedicated_info = NULL;
   VkMemoryAllocateFlags vk_flags = 0;
   uint64_t client_address = 0;

   vk_foreach_struct_const(ext, pAllocateInfo->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
         export_info = (void *)ext;
         break;

      case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
         ahw_import_info = (void *)ext;
         break;

      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
         fd_info = (void *)ext;
         break;

      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
         host_ptr_info = (void *)ext;
         break;

      case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: {
         const VkMemoryAllocateFlagsInfo *flags_info = (void *)ext;
         vk_flags = flags_info->flags;
         break;
      }

      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
         dedicated_info = (void *)ext;
         break;

      case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: {
         const VkMemoryOpaqueCaptureAddressAllocateInfo *addr_info =
            (const VkMemoryOpaqueCaptureAddressAllocateInfo *)ext;
         client_address = addr_info->opaqueCaptureAddress;
         break;
      }

      default:
         if (ext->sType != VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA)
            /* this isn't a real enum value,
             * so use conditional to avoid compiler warn
             */
            anv_debug_ignored_stype(ext->sType);
         break;
      }
   }

   /* By default, we want all VkDeviceMemory objects to support CCS */
   if (device->physical->has_implicit_ccs && device->info->has_aux_map)
      alloc_flags |= ANV_BO_ALLOC_IMPLICIT_CCS;

   /* If i915 reported a mappable/non_mappable vram regions and the
    * application want lmem mappable, then we need to use the
    * I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS flag to create our BO.
    */
   if (pdevice->vram_mappable.size > 0 &&
       pdevice->vram_non_mappable.size > 0 &&
       (mem_type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
       (mem_type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      alloc_flags |= ANV_BO_ALLOC_LOCAL_MEM_CPU_VISIBLE;

   if (!(mem_type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      alloc_flags |= ANV_BO_ALLOC_NO_LOCAL_MEM;

   /* If the allocated buffer might end up in local memory and it's host
    * visible, make CPU writes are combined, it should be faster.
    */
   if (!(alloc_flags & ANV_BO_ALLOC_NO_LOCAL_MEM) &&
       (mem_type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      alloc_flags |= ANV_BO_ALLOC_WRITE_COMBINE;

   if (vk_flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)
      alloc_flags |= ANV_BO_ALLOC_CLIENT_VISIBLE_ADDRESS;

   if ((export_info && export_info->handleTypes) ||
       (fd_info && fd_info->handleType) ||
       (host_ptr_info && host_ptr_info->handleType)) {
      /* Anything imported or exported is EXTERNAL */
      alloc_flags |= ANV_BO_ALLOC_EXTERNAL;
   }

   /* Check if we need to support Android HW buffer export. If so,
    * create AHardwareBuffer and import memory from it.
    */
   bool android_export = false;
   if (export_info && export_info->handleTypes &
       VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)
      android_export = true;

   if (ahw_import_info) {
      result = anv_import_ahw_memory(_device, mem, ahw_import_info);
      if (result != VK_SUCCESS)
         goto fail;

      goto success;
   } else if (android_export) {
      result = anv_create_ahw_memory(_device, mem, pAllocateInfo);
      if (result != VK_SUCCESS)
         goto fail;

      goto success;
   }

   /* The Vulkan spec permits handleType to be 0, in which case the struct is
    * ignored.
    */
   if (fd_info && fd_info->handleType) {
      /* At the moment, we support only the below handle types. */
      assert(fd_info->handleType ==
               VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
             fd_info->handleType ==
               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      result = anv_device_import_bo(device, fd_info->fd, alloc_flags,
                                    client_address, &mem->bo);
      if (result != VK_SUCCESS)
         goto fail;

      /* For security purposes, we reject importing the bo if it's smaller
       * than the requested allocation size.  This prevents a malicious client
       * from passing a buffer to a trusted client, lying about the size, and
       * telling the trusted client to try and texture from an image that goes
       * out-of-bounds.  This sort of thing could lead to GPU hangs or worse
       * in the trusted client.  The trusted client can protect itself against
       * this sort of attack but only if it can trust the buffer size.
       */
      if (mem->bo->size < aligned_alloc_size) {
         result = vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                            "aligned allocationSize too large for "
                            "VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT: "
                            "%"PRIu64"B > %"PRIu64"B",
                            aligned_alloc_size, mem->bo->size);
         anv_device_release_bo(device, mem->bo);
         goto fail;
      }

      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
      goto success;
   }

   if (host_ptr_info && host_ptr_info->handleType) {
      if (host_ptr_info->handleType ==
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT) {
         result = vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail;
      }

      assert(host_ptr_info->handleType ==
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);

      result = anv_device_import_bo_from_host_ptr(device,
                                                  host_ptr_info->pHostPointer,
                                                  pAllocateInfo->allocationSize,
                                                  alloc_flags,
                                                  client_address,
                                                  &mem->bo);
      if (result != VK_SUCCESS)
         goto fail;

      mem->host_ptr = host_ptr_info->pHostPointer;
      goto success;
   }

   /* Regular allocate (not importing memory). */

   result = anv_device_alloc_bo(device, "user", pAllocateInfo->allocationSize,
                                alloc_flags, client_address, &mem->bo);
   if (result != VK_SUCCESS)
      goto fail;

   if (dedicated_info && dedicated_info->image != VK_NULL_HANDLE) {
      ANV_FROM_HANDLE(anv_image, image, dedicated_info->image);

      /* Some legacy (non-modifiers) consumers need the tiling to be set on
       * the BO.  In this case, we have a dedicated allocation.
       */
      if (image->vk.wsi_legacy_scanout) {
         const struct isl_surf *surf = &image->planes[0].primary_surface.isl;
         result = anv_device_set_bo_tiling(device, mem->bo,
                                           surf->row_pitch_B,
                                           surf->tiling);
         if (result != VK_SUCCESS) {
            anv_device_release_bo(device, mem->bo);
            goto fail;
         }
      }
   }

 success:
   mem_heap_used = p_atomic_add_return(&mem_heap->used, mem->bo->size);
   if (mem_heap_used > mem_heap->size) {
      p_atomic_add(&mem_heap->used, -mem->bo->size);
      anv_device_release_bo(device, mem->bo);
      result = vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                         "Out of heap memory");
      goto fail;
   }

   pthread_mutex_lock(&device->mutex);
   list_addtail(&mem->link, &device->memory_objects);
   pthread_mutex_unlock(&device->mutex);

   *pMem = anv_device_memory_to_handle(mem);

   return VK_SUCCESS;

 fail:
   vk_object_free(&device->vk, pAllocator, mem);

   return result;
}

VkResult anv_GetMemoryFdKHR(
    VkDevice                                    device_h,
    const VkMemoryGetFdInfoKHR*                 pGetFdInfo,
    int*                                        pFd)
{
   ANV_FROM_HANDLE(anv_device, dev, device_h);
   ANV_FROM_HANDLE(anv_device_memory, mem, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   assert(pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
          pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   return anv_device_export_bo(dev, mem->bo, pFd);
}

VkResult anv_GetMemoryFdPropertiesKHR(
    VkDevice                                    _device,
    VkExternalMemoryHandleTypeFlagBits          handleType,
    int                                         fd,
    VkMemoryFdPropertiesKHR*                    pMemoryFdProperties)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      /* dma-buf can be imported as any memory type */
      pMemoryFdProperties->memoryTypeBits =
         (1 << device->physical->memory.type_count) - 1;
      return VK_SUCCESS;

   default:
      /* The valid usage section for this function says:
       *
       *    "handleType must not be one of the handle types defined as
       *    opaque."
       *
       * So opaque handle types fall into the default "unsupported" case.
       */
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
}

VkResult anv_GetMemoryHostPointerPropertiesEXT(
   VkDevice                                    _device,
   VkExternalMemoryHandleTypeFlagBits          handleType,
   const void*                                 pHostPointer,
   VkMemoryHostPointerPropertiesEXT*           pMemoryHostPointerProperties)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   assert(pMemoryHostPointerProperties->sType ==
          VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT);

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT:
      /* Host memory can be imported as any memory type. */
      pMemoryHostPointerProperties->memoryTypeBits =
         (1ull << device->physical->memory.type_count) - 1;

      return VK_SUCCESS;

   default:
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
}

void anv_FreeMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _mem,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   pthread_mutex_lock(&device->mutex);
   list_del(&mem->link);
   pthread_mutex_unlock(&device->mutex);

   if (mem->map)
      anv_UnmapMemory(_device, _mem);

   p_atomic_add(&device->physical->memory.heaps[mem->type->heapIndex].used,
                -mem->bo->size);

   anv_device_release_bo(device, mem->bo);

#if defined(ANDROID) && ANDROID_API_LEVEL >= 26
   if (mem->ahw)
      AHardwareBuffer_release(mem->ahw);
#endif

   vk_object_free(&device->vk, pAllocator, mem);
}

VkResult anv_MapMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _memory,
    VkDeviceSize                                offset,
    VkDeviceSize                                size,
    VkMemoryMapFlags                            flags,
    void**                                      ppData)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_device_memory, mem, _memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (mem->host_ptr) {
      *ppData = mem->host_ptr + offset;
      return VK_SUCCESS;
   }

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * memory must have been created with a memory type that reports
    *    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    */
   if (!(mem->type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object not mappable.");
   }

   if (size == VK_WHOLE_SIZE)
      size = mem->bo->size - offset;

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%"PRIx64" does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");
   }

   uint32_t gem_flags = 0;

   if (!device->info->has_llc &&
       (mem->type->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      gem_flags |= I915_MMAP_WC;

   /* GEM will fail to map if the offset isn't 4k-aligned.  Round down. */
   uint64_t map_offset;
   if (!device->physical->info.has_mmap_offset)
      map_offset = offset & ~4095ull;
   else
      map_offset = 0;
   assert(offset >= map_offset);
   uint64_t map_size = (offset + size) - map_offset;

   /* Let's map whole pages */
   map_size = align_u64(map_size, 4096);

   void *map;
   VkResult result = anv_device_map_bo(device, mem->bo, map_offset,
                                       map_size, gem_flags, &map);
   if (result != VK_SUCCESS)
      return result;

   mem->map = map;
   mem->map_size = map_size;
   mem->map_delta = (offset - map_offset);
   *ppData = mem->map + mem->map_delta;

   return VK_SUCCESS;
}

void anv_UnmapMemory(
    VkDevice                                    _device,
    VkDeviceMemory                              _memory)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_device_memory, mem, _memory);

   if (mem == NULL || mem->host_ptr)
      return;

   anv_device_unmap_bo(device, mem->bo, mem->map, mem->map_size);

   mem->map = NULL;
   mem->map_size = 0;
   mem->map_delta = 0;
}

VkResult anv_FlushMappedMemoryRanges(
    VkDevice                                    _device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges)
{
#ifdef SUPPORT_INTEL_INTEGRATED_GPUS
   ANV_FROM_HANDLE(anv_device, device, _device);

   if (!device->physical->memory.need_clflush)
      return VK_SUCCESS;

   /* Make sure the writes we're flushing have landed. */
   __builtin_ia32_mfence();

   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      ANV_FROM_HANDLE(anv_device_memory, mem, pMemoryRanges[i].memory);
      if (mem->type->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
         continue;

      uint64_t map_offset = pMemoryRanges[i].offset + mem->map_delta;
      if (map_offset >= mem->map_size)
         continue;

      intel_clflush_range(mem->map + map_offset,
                          MIN2(pMemoryRanges[i].size,
                               mem->map_size - map_offset));
   }
#endif
   return VK_SUCCESS;
}

VkResult anv_InvalidateMappedMemoryRanges(
    VkDevice                                    _device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges)
{
#ifdef SUPPORT_INTEL_INTEGRATED_GPUS
   ANV_FROM_HANDLE(anv_device, device, _device);

   if (!device->physical->memory.need_clflush)
      return VK_SUCCESS;

   for (uint32_t i = 0; i < memoryRangeCount; i++) {
      ANV_FROM_HANDLE(anv_device_memory, mem, pMemoryRanges[i].memory);
      if (mem->type->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
         continue;

      uint64_t map_offset = pMemoryRanges[i].offset + mem->map_delta;
      if (map_offset >= mem->map_size)
         continue;

      intel_invalidate_range(mem->map + map_offset,
                             MIN2(pMemoryRanges[i].size,
                                  mem->map_size - map_offset));
   }

   /* Make sure no reads get moved up above the invalidate. */
   __builtin_ia32_mfence();
#endif
   return VK_SUCCESS;
}

void anv_GetDeviceMemoryCommitment(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize*                               pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

static void
anv_bind_buffer_memory(const VkBindBufferMemoryInfo *pBindInfo)
{
   ANV_FROM_HANDLE(anv_device_memory, mem, pBindInfo->memory);
   ANV_FROM_HANDLE(anv_buffer, buffer, pBindInfo->buffer);

   assert(pBindInfo->sType == VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO);

   if (mem) {
      assert(pBindInfo->memoryOffset < mem->bo->size);
      assert(mem->bo->size - pBindInfo->memoryOffset >= buffer->vk.size);
      buffer->address = (struct anv_address) {
         .bo = mem->bo,
         .offset = pBindInfo->memoryOffset,
      };
   } else {
      buffer->address = ANV_NULL_ADDRESS;
   }
}

VkResult anv_BindBufferMemory2(
    VkDevice                                    device,
    uint32_t                                    bindInfoCount,
    const VkBindBufferMemoryInfo*               pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; i++)
      anv_bind_buffer_memory(&pBindInfos[i]);

   return VK_SUCCESS;
}

VkResult anv_QueueBindSparse(
    VkQueue                                     _queue,
    uint32_t                                    bindInfoCount,
    const VkBindSparseInfo*                     pBindInfo,
    VkFence                                     fence)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   if (vk_device_is_lost(&queue->device->vk))
      return VK_ERROR_DEVICE_LOST;

   return vk_error(queue, VK_ERROR_FEATURE_NOT_PRESENT);
}

// Event functions

VkResult anv_CreateEvent(
    VkDevice                                    _device,
    const VkEventCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkEvent*                                    pEvent)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_event *event;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);

   event = vk_object_alloc(&device->vk, pAllocator, sizeof(*event),
                           VK_OBJECT_TYPE_EVENT);
   if (event == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   event->state = anv_state_pool_alloc(&device->dynamic_state_pool,
                                       sizeof(uint64_t), 8);
   *(uint64_t *)event->state.map = VK_EVENT_RESET;

   *pEvent = anv_event_to_handle(event);

   return VK_SUCCESS;
}

void anv_DestroyEvent(
    VkDevice                                    _device,
    VkEvent                                     _event,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_event, event, _event);

   if (!event)
      return;

   anv_state_pool_free(&device->dynamic_state_pool, event->state);

   vk_object_free(&device->vk, pAllocator, event);
}

VkResult anv_GetEventStatus(
    VkDevice                                    _device,
    VkEvent                                     _event)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_event, event, _event);

   if (vk_device_is_lost(&device->vk))
      return VK_ERROR_DEVICE_LOST;

   return *(uint64_t *)event->state.map;
}

VkResult anv_SetEvent(
    VkDevice                                    _device,
    VkEvent                                     _event)
{
   ANV_FROM_HANDLE(anv_event, event, _event);

   *(uint64_t *)event->state.map = VK_EVENT_SET;

   return VK_SUCCESS;
}

VkResult anv_ResetEvent(
    VkDevice                                    _device,
    VkEvent                                     _event)
{
   ANV_FROM_HANDLE(anv_event, event, _event);

   *(uint64_t *)event->state.map = VK_EVENT_RESET;

   return VK_SUCCESS;
}

// Buffer functions

static void
anv_get_buffer_memory_requirements(struct anv_device *device,
                                   VkDeviceSize size,
                                   VkBufferUsageFlags usage,
                                   VkMemoryRequirements2* pMemoryRequirements)
{
   /* The Vulkan spec (git aaed022) says:
    *
    *    memoryTypeBits is a bitfield and contains one bit set for every
    *    supported memory type for the resource. The bit `1<<i` is set if and
    *    only if the memory type `i` in the VkPhysicalDeviceMemoryProperties
    *    structure for the physical device is supported.
    */
   uint32_t memory_types = (1ull << device->physical->memory.type_count) - 1;

   /* Base alignment requirement of a cache line */
   uint32_t alignment = 16;

   if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
      alignment = MAX2(alignment, ANV_UBO_ALIGNMENT);

   pMemoryRequirements->memoryRequirements.size = size;
   pMemoryRequirements->memoryRequirements.alignment = alignment;

   /* Storage and Uniform buffers should have their size aligned to
    * 32-bits to avoid boundary checks when last DWord is not complete.
    * This would ensure that not internal padding would be needed for
    * 16-bit types.
    */
   if (device->robust_buffer_access &&
       (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT ||
        usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
      pMemoryRequirements->memoryRequirements.size = align_u64(size, 4);

   pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_types;

   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *requirements = (void *)ext;
         requirements->prefersDedicatedAllocation = false;
         requirements->requiresDedicatedAllocation = false;
         break;
      }

      default:
         anv_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

void anv_GetBufferMemoryRequirements2(
    VkDevice                                    _device,
    const VkBufferMemoryRequirementsInfo2*      pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_buffer, buffer, pInfo->buffer);

   anv_get_buffer_memory_requirements(device,
                                      buffer->vk.size,
                                      buffer->vk.usage,
                                      pMemoryRequirements);
}

void anv_GetDeviceBufferMemoryRequirementsKHR(
    VkDevice                                    _device,
    const VkDeviceBufferMemoryRequirements*     pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   anv_get_buffer_memory_requirements(device,
                                      pInfo->pCreateInfo->size,
                                      pInfo->pCreateInfo->usage,
                                      pMemoryRequirements);
}

VkResult anv_CreateBuffer(
    VkDevice                                    _device,
    const VkBufferCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBuffer*                                   pBuffer)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   struct anv_buffer *buffer;

   /* Don't allow creating buffers bigger than our address space.  The real
    * issue here is that we may align up the buffer size and we don't want
    * doing so to cause roll-over.  However, no one has any business
    * allocating a buffer larger than our GTT size.
    */
   if (pCreateInfo->size > device->physical->gtt_size)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   buffer = vk_buffer_create(&device->vk, pCreateInfo,
                             pAllocator, sizeof(*buffer));
   if (buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->address = ANV_NULL_ADDRESS;

   *pBuffer = anv_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

void anv_DestroyBuffer(
    VkDevice                                    _device,
    VkBuffer                                    _buffer,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);

   if (!buffer)
      return;

   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

VkDeviceAddress anv_GetBufferDeviceAddress(
    VkDevice                                    device,
    const VkBufferDeviceAddressInfo*            pInfo)
{
   ANV_FROM_HANDLE(anv_buffer, buffer, pInfo->buffer);

   assert(!anv_address_is_null(buffer->address));

   return anv_address_physical(buffer->address);
}

uint64_t anv_GetBufferOpaqueCaptureAddress(
    VkDevice                                    device,
    const VkBufferDeviceAddressInfo*            pInfo)
{
   return 0;
}

uint64_t anv_GetDeviceMemoryOpaqueCaptureAddress(
    VkDevice                                    device,
    const VkDeviceMemoryOpaqueCaptureAddressInfo* pInfo)
{
   ANV_FROM_HANDLE(anv_device_memory, memory, pInfo->memory);

   assert(memory->bo->has_client_visible_address);

   return intel_48b_address(memory->bo->offset);
}

void
anv_fill_buffer_surface_state(struct anv_device *device, struct anv_state state,
                              enum isl_format format,
                              struct isl_swizzle swizzle,
                              isl_surf_usage_flags_t usage,
                              struct anv_address address,
                              uint32_t range, uint32_t stride)
{
   isl_buffer_fill_state(&device->isl_dev, state.map,
                         .address = anv_address_physical(address),
                         .mocs = isl_mocs(&device->isl_dev, usage,
                                          address.bo && address.bo->is_external),
                         .size_B = range,
                         .format = format,
                         .swizzle = swizzle,
                         .stride_B = stride);
}

void anv_DestroySampler(
    VkDevice                                    _device,
    VkSampler                                   _sampler,
    const VkAllocationCallbacks*                pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_sampler, sampler, _sampler);

   if (!sampler)
      return;

   if (sampler->bindless_state.map) {
      anv_state_pool_free(&device->dynamic_state_pool,
                          sampler->bindless_state);
   }

   if (sampler->custom_border_color.map) {
      anv_state_reserved_pool_free(&device->custom_border_colors,
                                   sampler->custom_border_color);
   }

   vk_object_free(&device->vk, pAllocator, sampler);
}

static const VkTimeDomainEXT anv_time_domains[] = {
   VK_TIME_DOMAIN_DEVICE_EXT,
   VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
#ifdef CLOCK_MONOTONIC_RAW
   VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
#endif
};

VkResult anv_GetPhysicalDeviceCalibrateableTimeDomainsEXT(
   VkPhysicalDevice                             physicalDevice,
   uint32_t                                     *pTimeDomainCount,
   VkTimeDomainEXT                              *pTimeDomains)
{
   int d;
   VK_OUTARRAY_MAKE_TYPED(VkTimeDomainEXT, out, pTimeDomains, pTimeDomainCount);

   for (d = 0; d < ARRAY_SIZE(anv_time_domains); d++) {
      vk_outarray_append_typed(VkTimeDomainEXT, &out, i) {
         *i = anv_time_domains[d];
      }
   }

   return vk_outarray_status(&out);
}

VkResult anv_GetCalibratedTimestampsEXT(
   VkDevice                                     _device,
   uint32_t                                     timestampCount,
   const VkCalibratedTimestampInfoEXT           *pTimestampInfos,
   uint64_t                                     *pTimestamps,
   uint64_t                                     *pMaxDeviation)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   uint64_t timestamp_frequency = device->info->timestamp_frequency;
   int d;
   uint64_t begin, end;
   uint64_t max_clock_period = 0;

#ifdef CLOCK_MONOTONIC_RAW
   begin = vk_clock_gettime(CLOCK_MONOTONIC_RAW);
#else
   begin = vk_clock_gettime(CLOCK_MONOTONIC);
#endif

   for (d = 0; d < timestampCount; d++) {
      switch (pTimestampInfos[d].timeDomain) {
      case VK_TIME_DOMAIN_DEVICE_EXT:
         if (!intel_gem_read_render_timestamp(device->fd, &pTimestamps[d])) {
            return vk_device_set_lost(&device->vk, "Failed to read the "
                                      "TIMESTAMP register: %m");
         }
         uint64_t device_period = DIV_ROUND_UP(1000000000, timestamp_frequency);
         max_clock_period = MAX2(max_clock_period, device_period);
         break;
      case VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT:
         pTimestamps[d] = vk_clock_gettime(CLOCK_MONOTONIC);
         max_clock_period = MAX2(max_clock_period, 1);
         break;

#ifdef CLOCK_MONOTONIC_RAW
      case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT:
         pTimestamps[d] = begin;
         break;
#endif
      default:
         pTimestamps[d] = 0;
         break;
      }
   }

#ifdef CLOCK_MONOTONIC_RAW
   end = vk_clock_gettime(CLOCK_MONOTONIC_RAW);
#else
   end = vk_clock_gettime(CLOCK_MONOTONIC);
#endif

   *pMaxDeviation = vk_time_max_deviation(begin, end, max_clock_period);

   return VK_SUCCESS;
}

void anv_GetPhysicalDeviceMultisamplePropertiesEXT(
    VkPhysicalDevice                            physicalDevice,
    VkSampleCountFlagBits                       samples,
    VkMultisamplePropertiesEXT*                 pMultisampleProperties)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);

   assert(pMultisampleProperties->sType ==
          VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT);

   VkExtent2D grid_size;
   if (samples & isl_device_get_sample_counts(&physical_device->isl_dev)) {
      grid_size.width = 1;
      grid_size.height = 1;
   } else {
      grid_size.width = 0;
      grid_size.height = 0;
   }
   pMultisampleProperties->maxSampleLocationGridSize = grid_size;

   vk_foreach_struct(ext, pMultisampleProperties->pNext)
      anv_debug_ignored_stype(ext->sType);
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

VkResult anv_GetPhysicalDeviceFragmentShadingRatesKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t*                                   pFragmentShadingRateCount,
    VkPhysicalDeviceFragmentShadingRateKHR*     pFragmentShadingRates)
{
   ANV_FROM_HANDLE(anv_physical_device, physical_device, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkPhysicalDeviceFragmentShadingRateKHR, out,
                          pFragmentShadingRates, pFragmentShadingRateCount);

#define append_rate(_samples, _width, _height)                                      \
   do {                                                                             \
      vk_outarray_append_typed(VkPhysicalDeviceFragmentShadingRateKHR, &out, __r) { \
         __r->sampleCounts = _samples;                                              \
         __r->fragmentSize = (VkExtent2D) {                                         \
            .width = _width,                                                        \
            .height = _height,                                                      \
         };                                                                         \
      }                                                                             \
   } while (0)

   VkSampleCountFlags sample_counts =
      isl_device_get_sample_counts(&physical_device->isl_dev);

   /* BSpec 47003: There are a number of restrictions on the sample count
    * based off the coarse pixel size.
    */
   static const VkSampleCountFlags cp_size_sample_limits[] = {
      [1]  = ISL_SAMPLE_COUNT_16_BIT | ISL_SAMPLE_COUNT_8_BIT |
             ISL_SAMPLE_COUNT_4_BIT | ISL_SAMPLE_COUNT_2_BIT | ISL_SAMPLE_COUNT_1_BIT,
      [2]  = ISL_SAMPLE_COUNT_4_BIT | ISL_SAMPLE_COUNT_2_BIT | ISL_SAMPLE_COUNT_1_BIT,
      [4]  = ISL_SAMPLE_COUNT_4_BIT | ISL_SAMPLE_COUNT_2_BIT | ISL_SAMPLE_COUNT_1_BIT,
      [8]  = ISL_SAMPLE_COUNT_2_BIT | ISL_SAMPLE_COUNT_1_BIT,
      [16] = ISL_SAMPLE_COUNT_1_BIT,
   };

   for (uint32_t x = 4; x >= 1; x /= 2) {
       for (uint32_t y = 4; y >= 1; y /= 2) {
          if (physical_device->info.has_coarse_pixel_primitive_and_cb) {
             /* BSpec 47003:
              *   "CPsize 1x4 and 4x1 are not supported"
              */
             if ((x == 1 && y == 4) || (x == 4 && y == 1))
                continue;

             /* For size {1, 1}, the sample count must be ~0
              *
              * 4x2 is also a specially case.
              */
             if (x == 1 && y == 1)
                append_rate(~0, x, y);
             else if (x == 4 && y == 2)
                append_rate(ISL_SAMPLE_COUNT_1_BIT, x, y);
             else
                append_rate(cp_size_sample_limits[x * y], x, y);
          } else {
             /* For size {1, 1}, the sample count must be ~0 */
             if (x == 1 && y == 1)
                append_rate(~0, x, y);
             else
                append_rate(sample_counts, x, y);
          }
       }
   }

#undef append_rate

   return vk_outarray_status(&out);
}
