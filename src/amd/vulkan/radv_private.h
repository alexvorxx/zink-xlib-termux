/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#ifndef RADV_PRIVATE_H
#define RADV_PRIVATE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_VALGRIND
#include <memcheck.h>
#include <valgrind.h>
#define VG(x) x
#else
#define VG(x) ((void)0)
#endif

#include "c11/threads.h"
#ifndef _WIN32
#include <amdgpu.h>
#include <xf86drm.h>
#endif
#include "compiler/shader_enums.h"
#include "util/bitscan.h"
#include "util/detect_os.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/rwlock.h"
#include "util/xmlconfig.h"
#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_debug_report.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_shader_module.h"
#include "vk_util.h"
#include "vk_ycbcr_conversion.h"

#include "rmv/vk_rmv_common.h"
#include "rmv/vk_rmv_tokens.h"

#include "ac_binary.h"
#include "ac_gpu_info.h"
#include "ac_shader_util.h"
#include "ac_spm.h"
#include "ac_sqtt.h"
#include "ac_surface.h"
#include "ac_vcn.h"
#include "radv_constants.h"
#include "radv_descriptor_set.h"
#include "radv_device.h"
#include "radv_physical_device.h"
#include "radv_pipeline.h"
#include "radv_pipeline_compute.h"
#include "radv_queue.h"
#include "radv_radeon_winsys.h"
#include "radv_rra.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
#include "sid.h"

#include "radix_sort/radix_sort_vk_devaddr.h"

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include "radv_entrypoints.h"

#include "wsi_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Helper to determine if we should compile
 * any of the Android AHB support.
 *
 * To actually enable the ext we also need
 * the necessary kernel support.
 */
#if DETECT_OS_ANDROID && ANDROID_API_LEVEL >= 26
#define RADV_SUPPORT_ANDROID_HARDWARE_BUFFER 1
#include <vndk/hardware_buffer.h>
#else
#define RADV_SUPPORT_ANDROID_HARDWARE_BUFFER 0
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR) ||   \
   defined(VK_USE_PLATFORM_DISPLAY_KHR)
#define RADV_USE_WSI_PLATFORM
#endif

#ifdef ANDROID_STRICT
#define RADV_API_VERSION VK_MAKE_VERSION(1, 1, VK_HEADER_VERSION)
#else
#define RADV_API_VERSION VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)
#endif

#ifdef _WIN32
#define RADV_SUPPORT_CALIBRATED_TIMESTAMPS 0
#else
#define RADV_SUPPORT_CALIBRATED_TIMESTAMPS 1
#endif

#ifdef _WIN32
#define radv_printflike(a, b)
#else
#define radv_printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#endif

/* The "RAW" clocks on Linux are called "FAST" on FreeBSD */
#if !defined(CLOCK_MONOTONIC_RAW) && defined(CLOCK_MONOTONIC_FAST)
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC_FAST
#endif

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static inline uint32_t
align_u32_npot(uint32_t v, uint32_t a)
{
   return (v + a - 1) / a * a;
}

static inline uint64_t
align_u64(uint64_t v, uint64_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

/** Alignment must be a power of 2. */
static inline bool
radv_is_aligned(uintmax_t n, uintmax_t a)
{
   assert(a == (a & -a));
   return (n & (a - 1)) == 0;
}

static inline uint32_t
radv_minify(uint32_t n, uint32_t levels)
{
   if (unlikely(n == 0))
      return 0;
   else
      return MAX2(n >> levels, 1);
}

static inline int
radv_float_to_sfixed(float value, unsigned frac_bits)
{
   return value * (1 << frac_bits);
}

static inline unsigned int
radv_float_to_ufixed(float value, unsigned frac_bits)
{
   return value * (1 << frac_bits);
}

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

struct radv_image_view;
struct rvcn_decode_buffer_s;

/* queue types */

VkResult radv_init_wsi(struct radv_physical_device *pdev);
void radv_finish_wsi(struct radv_physical_device *pdev);

struct radv_shader_binary_part;


struct radv_ray_tracing_pipeline;


enum amd_ip_type radv_queue_family_to_ring(const struct radv_physical_device *dev, enum radv_queue_family f);

struct radv_printf_format {
   char *string;
   uint32_t divergence_mask;
   uint8_t element_sizes[32];
};

VkResult radv_printf_data_init(struct radv_device *device);

void radv_printf_data_finish(struct radv_device *device);

struct radv_printf_buffer_header {
   uint32_t offset;
   uint32_t size;
};

typedef struct nir_builder nir_builder;
typedef struct nir_def nir_def;

void radv_build_printf(nir_builder *b, nir_def *cond, const char *format, ...);

void radv_dump_printf_data(struct radv_device *device, FILE *out);

void radv_device_associate_nir(struct radv_device *device, nir_shader *nir);

enum radv_dynamic_state_bits {
   RADV_DYNAMIC_VIEWPORT = 1ull << 0,
   RADV_DYNAMIC_SCISSOR = 1ull << 1,
   RADV_DYNAMIC_LINE_WIDTH = 1ull << 2,
   RADV_DYNAMIC_DEPTH_BIAS = 1ull << 3,
   RADV_DYNAMIC_BLEND_CONSTANTS = 1ull << 4,
   RADV_DYNAMIC_DEPTH_BOUNDS = 1ull << 5,
   RADV_DYNAMIC_STENCIL_COMPARE_MASK = 1ull << 6,
   RADV_DYNAMIC_STENCIL_WRITE_MASK = 1ull << 7,
   RADV_DYNAMIC_STENCIL_REFERENCE = 1ull << 8,
   RADV_DYNAMIC_DISCARD_RECTANGLE = 1ull << 9,
   RADV_DYNAMIC_SAMPLE_LOCATIONS = 1ull << 10,
   RADV_DYNAMIC_LINE_STIPPLE = 1ull << 11,
   RADV_DYNAMIC_CULL_MODE = 1ull << 12,
   RADV_DYNAMIC_FRONT_FACE = 1ull << 13,
   RADV_DYNAMIC_PRIMITIVE_TOPOLOGY = 1ull << 14,
   RADV_DYNAMIC_DEPTH_TEST_ENABLE = 1ull << 15,
   RADV_DYNAMIC_DEPTH_WRITE_ENABLE = 1ull << 16,
   RADV_DYNAMIC_DEPTH_COMPARE_OP = 1ull << 17,
   RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE = 1ull << 18,
   RADV_DYNAMIC_STENCIL_TEST_ENABLE = 1ull << 19,
   RADV_DYNAMIC_STENCIL_OP = 1ull << 20,
   RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE = 1ull << 21,
   RADV_DYNAMIC_FRAGMENT_SHADING_RATE = 1ull << 22,
   RADV_DYNAMIC_PATCH_CONTROL_POINTS = 1ull << 23,
   RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE = 1ull << 24,
   RADV_DYNAMIC_DEPTH_BIAS_ENABLE = 1ull << 25,
   RADV_DYNAMIC_LOGIC_OP = 1ull << 26,
   RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE = 1ull << 27,
   RADV_DYNAMIC_COLOR_WRITE_ENABLE = 1ull << 28,
   RADV_DYNAMIC_VERTEX_INPUT = 1ull << 29,
   RADV_DYNAMIC_POLYGON_MODE = 1ull << 30,
   RADV_DYNAMIC_TESS_DOMAIN_ORIGIN = 1ull << 31,
   RADV_DYNAMIC_LOGIC_OP_ENABLE = 1ull << 32,
   RADV_DYNAMIC_LINE_STIPPLE_ENABLE = 1ull << 33,
   RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE = 1ull << 34,
   RADV_DYNAMIC_SAMPLE_MASK = 1ull << 35,
   RADV_DYNAMIC_DEPTH_CLIP_ENABLE = 1ull << 36,
   RADV_DYNAMIC_CONSERVATIVE_RAST_MODE = 1ull << 37,
   RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE = 1ull << 38,
   RADV_DYNAMIC_PROVOKING_VERTEX_MODE = 1ull << 39,
   RADV_DYNAMIC_DEPTH_CLAMP_ENABLE = 1ull << 40,
   RADV_DYNAMIC_COLOR_WRITE_MASK = 1ull << 41,
   RADV_DYNAMIC_COLOR_BLEND_ENABLE = 1ull << 42,
   RADV_DYNAMIC_RASTERIZATION_SAMPLES = 1ull << 43,
   RADV_DYNAMIC_LINE_RASTERIZATION_MODE = 1ull << 44,
   RADV_DYNAMIC_COLOR_BLEND_EQUATION = 1ull << 45,
   RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE = 1ull << 46,
   RADV_DYNAMIC_DISCARD_RECTANGLE_MODE = 1ull << 47,
   RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE = 1ull << 48,
   RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE = 1ull << 49,
   RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE = 1ull << 50,
   RADV_DYNAMIC_ALL = (1ull << 51) - 1,
};

enum radv_cmd_dirty_bits {
   /* Keep the dynamic state dirty bits in sync with
    * enum radv_dynamic_state_bits */
   RADV_CMD_DIRTY_DYNAMIC_VIEWPORT = 1ull << 0,
   RADV_CMD_DIRTY_DYNAMIC_SCISSOR = 1ull << 1,
   RADV_CMD_DIRTY_DYNAMIC_LINE_WIDTH = 1ull << 2,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS = 1ull << 3,
   RADV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS = 1ull << 4,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS = 1ull << 5,
   RADV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK = 1ull << 6,
   RADV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK = 1ull << 7,
   RADV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE = 1ull << 8,
   RADV_CMD_DIRTY_DYNAMIC_DISCARD_RECTANGLE = 1ull << 9,
   RADV_CMD_DIRTY_DYNAMIC_SAMPLE_LOCATIONS = 1ull << 10,
   RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE = 1ull << 11,
   RADV_CMD_DIRTY_DYNAMIC_CULL_MODE = 1ull << 12,
   RADV_CMD_DIRTY_DYNAMIC_FRONT_FACE = 1ull << 13,
   RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_TOPOLOGY = 1ull << 14,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_TEST_ENABLE = 1ull << 15,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_WRITE_ENABLE = 1ull << 16,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_COMPARE_OP = 1ull << 17,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE = 1ull << 18,
   RADV_CMD_DIRTY_DYNAMIC_STENCIL_TEST_ENABLE = 1ull << 19,
   RADV_CMD_DIRTY_DYNAMIC_STENCIL_OP = 1ull << 20,
   RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE = 1ull << 21,
   RADV_CMD_DIRTY_DYNAMIC_FRAGMENT_SHADING_RATE = 1ull << 22,
   RADV_CMD_DIRTY_DYNAMIC_PATCH_CONTROL_POINTS = 1ull << 23,
   RADV_CMD_DIRTY_DYNAMIC_RASTERIZER_DISCARD_ENABLE = 1ull << 24,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS_ENABLE = 1ull << 25,
   RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP = 1ull << 26,
   RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_RESTART_ENABLE = 1ull << 27,
   RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_ENABLE = 1ull << 28,
   RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT = 1ull << 29,
   RADV_CMD_DIRTY_DYNAMIC_POLYGON_MODE = 1ull << 30,
   RADV_CMD_DIRTY_DYNAMIC_TESS_DOMAIN_ORIGIN = 1ull << 31,
   RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP_ENABLE = 1ull << 32,
   RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE_ENABLE = 1ull << 33,
   RADV_CMD_DIRTY_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE = 1ull << 34,
   RADV_CMD_DIRTY_DYNAMIC_SAMPLE_MASK = 1ull << 35,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_ENABLE = 1ull << 36,
   RADV_CMD_DIRTY_DYNAMIC_CONSERVATIVE_RAST_MODE = 1ull << 37,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE = 1ull << 38,
   RADV_CMD_DIRTY_DYNAMIC_PROVOKING_VERTEX_MODE = 1ull << 39,
   RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLAMP_ENABLE = 1ull << 40,
   RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK = 1ull << 41,
   RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_ENABLE = 1ull << 42,
   RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES = 1ull << 43,
   RADV_CMD_DIRTY_DYNAMIC_LINE_RASTERIZATION_MODE = 1ull << 44,
   RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_EQUATION = 1ull << 45,
   RADV_CMD_DIRTY_DYNAMIC_DISCARD_RECTANGLE_ENABLE = 1ull << 46,
   RADV_CMD_DIRTY_DYNAMIC_DISCARD_RECTANGLE_MODE = 1ull << 47,
   RADV_CMD_DIRTY_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE = 1ull << 48,
   RADV_CMD_DIRTY_DYNAMIC_SAMPLE_LOCATIONS_ENABLE = 1ull << 49,
   RADV_CMD_DIRTY_DYNAMIC_ALPHA_TO_ONE_ENABLE = 1ull << 50,
   RADV_CMD_DIRTY_DYNAMIC_ALL = (1ull << 51) - 1,
   RADV_CMD_DIRTY_PIPELINE = 1ull << 51,
   RADV_CMD_DIRTY_INDEX_BUFFER = 1ull << 52,
   RADV_CMD_DIRTY_FRAMEBUFFER = 1ull << 53,
   RADV_CMD_DIRTY_VERTEX_BUFFER = 1ull << 54,
   RADV_CMD_DIRTY_STREAMOUT_BUFFER = 1ull << 55,
   RADV_CMD_DIRTY_GUARDBAND = 1ull << 56,
   RADV_CMD_DIRTY_RBPLUS = 1ull << 57,
   RADV_CMD_DIRTY_SHADER_QUERY = 1ull << 58,
   RADV_CMD_DIRTY_OCCLUSION_QUERY = 1ull << 59,
   RADV_CMD_DIRTY_DB_SHADER_CONTROL = 1ull << 60,
   RADV_CMD_DIRTY_STREAMOUT_ENABLE = 1ull << 61,
   RADV_CMD_DIRTY_GRAPHICS_SHADERS = 1ull << 62,
};

enum radv_cmd_flush_bits {
   /* Instruction cache. */
   RADV_CMD_FLAG_INV_ICACHE = 1 << 0,
   /* Scalar L1 cache. */
   RADV_CMD_FLAG_INV_SCACHE = 1 << 1,
   /* Vector L1 cache. */
   RADV_CMD_FLAG_INV_VCACHE = 1 << 2,
   /* L2 cache + L2 metadata cache writeback & invalidate.
    * GFX6-8: Used by shaders only. GFX9-10: Used by everything. */
   RADV_CMD_FLAG_INV_L2 = 1 << 3,
   /* L2 writeback (write dirty L2 lines to memory for non-L2 clients).
    * Only used for coherency with non-L2 clients like CB, DB, CP on GFX6-8.
    * GFX6-7 will do complete invalidation, because the writeback is unsupported. */
   RADV_CMD_FLAG_WB_L2 = 1 << 4,
   /* Invalidate the metadata cache. To be used when the DCC/HTILE metadata
    * changed and we want to read an image from shaders. */
   RADV_CMD_FLAG_INV_L2_METADATA = 1 << 5,
   /* Framebuffer caches */
   RADV_CMD_FLAG_FLUSH_AND_INV_CB_META = 1 << 6,
   RADV_CMD_FLAG_FLUSH_AND_INV_DB_META = 1 << 7,
   RADV_CMD_FLAG_FLUSH_AND_INV_DB = 1 << 8,
   RADV_CMD_FLAG_FLUSH_AND_INV_CB = 1 << 9,
   /* Engine synchronization. */
   RADV_CMD_FLAG_VS_PARTIAL_FLUSH = 1 << 10,
   RADV_CMD_FLAG_PS_PARTIAL_FLUSH = 1 << 11,
   RADV_CMD_FLAG_CS_PARTIAL_FLUSH = 1 << 12,
   RADV_CMD_FLAG_VGT_FLUSH = 1 << 13,
   /* Pipeline query controls. */
   RADV_CMD_FLAG_START_PIPELINE_STATS = 1 << 14,
   RADV_CMD_FLAG_STOP_PIPELINE_STATS = 1 << 15,
   RADV_CMD_FLAG_VGT_STREAMOUT_SYNC = 1 << 16,

   RADV_CMD_FLUSH_AND_INV_FRAMEBUFFER = (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META |
                                         RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META),

   RADV_CMD_FLUSH_ALL_COMPUTE = (RADV_CMD_FLAG_INV_ICACHE | RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_INV_VCACHE |
                                 RADV_CMD_FLAG_INV_L2 | RADV_CMD_FLAG_WB_L2 | RADV_CMD_FLAG_CS_PARTIAL_FLUSH),
};

struct radv_vertex_binding {
   VkDeviceSize offset;
   VkDeviceSize size;
   VkDeviceSize stride;
};

struct radv_streamout_binding {
   struct radv_buffer *buffer;
   VkDeviceSize offset;
   VkDeviceSize size;
};

struct radv_streamout_state {
   /* Mask of bound streamout buffers. */
   uint8_t enabled_mask;

   /* State of VGT_STRMOUT_BUFFER_(CONFIG|END) */
   uint32_t hw_enabled_mask;

   /* State of VGT_STRMOUT_(CONFIG|EN) */
   bool streamout_enabled;
};

struct radv_sample_locations_state {
   VkSampleCountFlagBits per_pixel;
   VkExtent2D grid_size;
   uint32_t count;
   VkSampleLocationEXT locations[MAX_SAMPLE_LOCATIONS];
};

struct radv_dynamic_state {
   struct vk_dynamic_graphics_state vk;

   /**
    * Bitmask of (1ull << VK_DYNAMIC_STATE_*).
    * Defines the set of saved dynamic state.
    */
   uint64_t mask;

   struct {
      struct {
         float scale[3];
         float translate[3];
      } xform[MAX_VIEWPORTS];
   } hw_vp;

   struct radv_sample_locations_state sample_location;

   VkImageAspectFlags feedback_loop_aspects;
};

/**
 * Attachment state when recording a renderpass instance.
 *
 * The clear value is valid only if there exists a pending clear.
 */
struct radv_attachment {
   VkFormat format;
   struct radv_image_view *iview;
   VkImageLayout layout;
   VkImageLayout stencil_layout;

   union {
      struct radv_color_buffer_info cb;
      struct radv_ds_buffer_info ds;
   };

   struct radv_image_view *resolve_iview;
   VkResolveModeFlagBits resolve_mode;
   VkResolveModeFlagBits stencil_resolve_mode;
   VkImageLayout resolve_layout;
   VkImageLayout stencil_resolve_layout;
};

struct radv_rendering_state {
   bool active;
   bool has_image_views;
   VkRect2D area;
   uint32_t layer_count;
   uint32_t view_mask;
   uint32_t color_samples;
   uint32_t ds_samples;
   uint32_t max_samples;
   struct radv_sample_locations_state sample_locations;
   uint32_t color_att_count;
   struct radv_attachment color_att[MAX_RTS];
   struct radv_attachment ds_att;
   VkImageAspectFlags ds_att_aspects;
   struct radv_attachment vrs_att;
   VkExtent2D vrs_texel_size;
};

struct radv_descriptor_state {
   struct radv_descriptor_set *sets[MAX_SETS];
   uint32_t dirty;
   uint32_t valid;
   struct radv_push_descriptor_set push_set;
   uint32_t dynamic_buffers[4 * MAX_DYNAMIC_BUFFERS];
   uint64_t descriptor_buffers[MAX_SETS];
   bool need_indirect_descriptor_sets;
};

struct radv_push_constant_state {
   uint32_t size;
   uint32_t dynamic_offset_count;
};

enum rgp_flush_bits {
   RGP_FLUSH_WAIT_ON_EOP_TS = 0x1,
   RGP_FLUSH_VS_PARTIAL_FLUSH = 0x2,
   RGP_FLUSH_PS_PARTIAL_FLUSH = 0x4,
   RGP_FLUSH_CS_PARTIAL_FLUSH = 0x8,
   RGP_FLUSH_PFP_SYNC_ME = 0x10,
   RGP_FLUSH_SYNC_CP_DMA = 0x20,
   RGP_FLUSH_INVAL_VMEM_L0 = 0x40,
   RGP_FLUSH_INVAL_ICACHE = 0x80,
   RGP_FLUSH_INVAL_SMEM_L0 = 0x100,
   RGP_FLUSH_FLUSH_L2 = 0x200,
   RGP_FLUSH_INVAL_L2 = 0x400,
   RGP_FLUSH_FLUSH_CB = 0x800,
   RGP_FLUSH_INVAL_CB = 0x1000,
   RGP_FLUSH_FLUSH_DB = 0x2000,
   RGP_FLUSH_INVAL_DB = 0x4000,
   RGP_FLUSH_INVAL_L1 = 0x8000,
};

struct radv_multisample_state {
   bool sample_shading_enable;
   float min_sample_shading;
};

struct radv_ia_multi_vgt_param_helpers {
   uint32_t base;
   bool partial_es_wave;
   bool ia_switch_on_eoi;
   bool partial_vs_wave;
};

struct radv_cmd_state {
   /* Vertex descriptors */
   uint64_t vb_va;
   unsigned vb_size;

   bool predicating;
   uint64_t dirty;

   VkShaderStageFlags active_stages;
   struct radv_shader *shaders[MESA_VULKAN_SHADER_STAGES];
   struct radv_shader *gs_copy_shader;
   struct radv_shader *last_vgt_shader;
   struct radv_shader *rt_prolog;

   struct radv_shader_object *shader_objs[MESA_VULKAN_SHADER_STAGES];

   uint32_t prefetch_L2_mask;

   struct radv_graphics_pipeline *graphics_pipeline;
   struct radv_graphics_pipeline *emitted_graphics_pipeline;
   struct radv_compute_pipeline *compute_pipeline;
   struct radv_compute_pipeline *emitted_compute_pipeline;
   struct radv_ray_tracing_pipeline *rt_pipeline; /* emitted = emitted_compute_pipeline */
   struct radv_dynamic_state dynamic;
   struct radv_vs_input_state dynamic_vs_input;
   struct radv_streamout_state streamout;

   struct radv_rendering_state render;

   /* Index buffer */
   uint32_t index_type;
   uint32_t max_index_count;
   uint64_t index_va;
   int32_t last_index_type;

   uint32_t last_primitive_reset_index; /* only relevant on GFX6-7 */
   enum radv_cmd_flush_bits flush_bits;
   unsigned active_occlusion_queries;
   bool perfect_occlusion_queries_enabled;
   unsigned active_pipeline_queries;
   unsigned active_pipeline_gds_queries;
   unsigned active_pipeline_ace_queries; /* Task shader invocations query */
   unsigned active_prims_gen_queries;
   unsigned active_prims_xfb_queries;
   unsigned active_prims_gen_gds_queries;
   unsigned active_prims_xfb_gds_queries;
   uint32_t trace_id;
   uint32_t last_ia_multi_vgt_param;
   uint32_t last_ge_cntl;

   uint32_t last_num_instances;
   uint32_t last_first_instance;
   bool last_vertex_offset_valid;
   uint32_t last_vertex_offset;
   uint32_t last_drawid;
   uint32_t last_subpass_color_count;

   uint32_t last_sx_ps_downconvert;
   uint32_t last_sx_blend_opt_epsilon;
   uint32_t last_sx_blend_opt_control;

   uint32_t last_db_count_control;

   uint32_t last_db_shader_control;

   /* Whether CP DMA is busy/idle. */
   bool dma_is_busy;

   /* Whether any images that are not L2 coherent are dirty from the CB. */
   bool rb_noncoherent_dirty;

   /* Conditional rendering info. */
   uint8_t predication_op; /* 32-bit or 64-bit predicate value */
   int predication_type;   /* -1: disabled, 0: normal, 1: inverted */
   uint64_t predication_va;
   uint64_t mec_inv_pred_va;  /* For inverted predication when using MEC. */
   bool mec_inv_pred_emitted; /* To ensure we don't have to repeat inverting the VA. */

   /* Inheritance info. */
   VkQueryPipelineStatisticFlags inherited_pipeline_statistics;
   bool inherited_occlusion_queries;
   VkQueryControlFlags inherited_query_control_flags;

   bool context_roll_without_scissor_emitted;

   /* SQTT related state. */
   uint32_t current_event_type;
   uint32_t num_events;
   uint32_t num_layout_transitions;
   bool in_barrier;
   bool pending_sqtt_barrier_end;
   enum rgp_flush_bits sqtt_flush_bits;

   /* NGG culling state. */
   bool has_nggc;

   /* Mesh shading state. */
   bool mesh_shading;

   uint8_t cb_mip[MAX_RTS];
   uint8_t ds_mip;

   /* Whether DRAW_{INDEX}_INDIRECT_{MULTI} is emitted. */
   bool uses_draw_indirect;

   uint32_t rt_stack_size;

   struct radv_shader_part *emitted_vs_prolog;
   uint32_t vbo_misaligned_mask;
   uint32_t vbo_misaligned_mask_invalid;
   uint32_t vbo_bound_mask;

   struct radv_shader_part *emitted_ps_epilog;

   /* Per-vertex VRS state. */
   uint32_t last_vrs_rates;
   int8_t last_vrs_rates_sgpr_idx;

   /* Whether to suspend streamout for internal driver operations. */
   bool suspend_streamout;

   /* Whether this commandbuffer uses performance counters. */
   bool uses_perf_counters;

   struct radv_ia_multi_vgt_param_helpers ia_multi_vgt_param;

   /* Tessellation info when patch control points is dynamic. */
   unsigned tess_num_patches;
   unsigned tess_lds_size;

   unsigned col_format_non_compacted;

   /* Binning state */
   unsigned last_pa_sc_binner_cntl_0;

   struct radv_multisample_state ms;

   /* Custom blend mode for internal operations. */
   unsigned custom_blend_mode;
   unsigned db_render_control;

   unsigned rast_prim;

   uint32_t vtx_base_sgpr;
   uint8_t vtx_emit_num;
   bool uses_drawid;
   bool uses_baseinstance;

   bool uses_out_of_order_rast;
   bool uses_vrs_attachment;
   bool uses_dynamic_patch_control_points;
   bool uses_dynamic_vertex_binding_stride;
};

struct radv_cmd_buffer_upload {
   uint8_t *map;
   unsigned offset;
   uint64_t size;
   struct radeon_winsys_bo *upload_bo;
   struct list_head list;
};

struct radv_cmd_buffer {
   struct vk_command_buffer vk;

   VkCommandBufferUsageFlags usage_flags;
   struct radeon_cmdbuf *cs;
   struct radv_cmd_state state;
   struct radv_buffer *vertex_binding_buffers[MAX_VBS];
   struct radv_vertex_binding vertex_bindings[MAX_VBS];
   uint32_t used_vertex_bindings;
   struct radv_streamout_binding streamout_bindings[MAX_SO_BUFFERS];
   enum radv_queue_family qf;

   uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
   VkShaderStageFlags push_constant_stages;
   struct radv_descriptor_set_header meta_push_descriptors;

   struct radv_descriptor_state descriptors[MAX_BIND_POINTS];

   struct radv_push_constant_state push_constant_state[MAX_BIND_POINTS];

   uint64_t descriptor_buffers[MAX_SETS];

   struct radv_cmd_buffer_upload upload;

   uint32_t scratch_size_per_wave_needed;
   uint32_t scratch_waves_wanted;
   uint32_t compute_scratch_size_per_wave_needed;
   uint32_t compute_scratch_waves_wanted;
   uint32_t esgs_ring_size_needed;
   uint32_t gsvs_ring_size_needed;
   bool tess_rings_needed;
   bool task_rings_needed;
   bool mesh_scratch_ring_needed;
   bool gds_needed;    /* for GFX10 streamout and NGG GS queries */
   bool gds_oa_needed; /* for GFX10 streamout */
   bool sample_positions_needed;
   bool has_indirect_pipeline_binds;

   uint64_t gfx9_fence_va;
   uint32_t gfx9_fence_idx;
   uint64_t gfx9_eop_bug_va;

   struct set vs_prologs;
   struct set ps_epilogs;

   /**
    * Gang state.
    * Used when the command buffer needs work done on a different queue
    * (eg. when a graphics command buffer needs compute work).
    * Currently only one follower is possible per command buffer.
    */
   struct {
      /** Follower command stream. */
      struct radeon_cmdbuf *cs;

      /** Flush bits for the follower cmdbuf. */
      enum radv_cmd_flush_bits flush_bits;

      /**
       * For synchronization between the follower and leader.
       * The value of these semaphores are incremented whenever we
       * encounter a barrier that affects the follower.
       *
       * DWORD 0: Leader to follower semaphore.
       *          The leader writes the value and the follower waits.
       * DWORD 1: Follower to leader semaphore.
       *          The follower writes the value, and the leader waits.
       */
      struct {
         uint64_t va;                     /* Virtual address of the semaphore. */
         uint32_t leader_value;           /* Current value of the leader. */
         uint32_t emitted_leader_value;   /* Last value emitted by the leader. */
         uint32_t follower_value;         /* Current value of the follower. */
         uint32_t emitted_follower_value; /* Last value emitted by the follower. */
      } sem;
   } gang;

   /**
    * Whether a query pool has been reset and we have to flush caches.
    */
   bool pending_reset_query;

   /**
    * Bitmask of pending active query flushes.
    */
   enum radv_cmd_flush_bits active_query_flush_bits;

   struct {
      struct radv_video_session *vid;
      struct radv_video_session_params *params;
      struct rvcn_sq_var sq;
      struct rvcn_decode_buffer_s *decode_buffer;
   } video;

   struct {
      /* Temporary space for some transfer queue copy command workarounds. */
      struct radeon_winsys_bo *copy_temp;
   } transfer;

   uint64_t shader_upload_seq;

   uint32_t sqtt_cb_id;

   struct util_dynarray ray_history;
};

static inline struct radv_device *
radv_cmd_buffer_device(const struct radv_cmd_buffer *cmd_buffer)
{
   return (struct radv_device *)cmd_buffer->vk.base.device;
}

static inline bool
radv_cmdbuf_has_stage(const struct radv_cmd_buffer *cmd_buffer, gl_shader_stage stage)
{
   return !!(cmd_buffer->state.active_stages & mesa_to_vk_shader_stage(stage));
}

static inline uint32_t
radv_get_num_pipeline_stat_queries(struct radv_cmd_buffer *cmd_buffer)
{
   /* SAMPLE_STREAMOUTSTATS also requires PIPELINESTAT_START to be enabled. */
   return cmd_buffer->state.active_pipeline_queries + cmd_buffer->state.active_prims_gen_queries +
          cmd_buffer->state.active_prims_xfb_queries;
}

extern const struct vk_command_buffer_ops radv_cmd_buffer_ops;

struct radv_dispatch_info {
   /**
    * Determine the layout of the grid (in block units) to be used.
    */
   uint32_t blocks[3];

   /**
    * A starting offset for the grid. If unaligned is set, the offset
    * must still be aligned.
    */
   uint32_t offsets[3];

   /**
    * Whether it's an unaligned compute dispatch.
    */
   bool unaligned;

   /**
    * Whether waves must be launched in order.
    */
   bool ordered;

   /**
    * Indirect compute parameters resource.
    */
   struct radeon_winsys_bo *indirect;
   uint64_t va;
};

void radv_compute_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info);

bool radv_cmd_buffer_uses_mec(struct radv_cmd_buffer *cmd_buffer);

void radv_emit_graphics(struct radv_device *device, struct radeon_cmdbuf *cs);
void radv_emit_compute(struct radv_device *device, struct radeon_cmdbuf *cs);

void radv_create_gfx_config(struct radv_device *device);

void radv_write_scissors(struct radeon_cmdbuf *cs, int count, const VkRect2D *scissors, const VkViewport *viewports);

void radv_write_guardband(struct radeon_cmdbuf *cs, int count, const VkViewport *viewports, unsigned rast_prim,
                          unsigned polygon_mode, float line_width);

VkResult radv_create_shadow_regs_preamble(struct radv_device *device, struct radv_queue_state *queue_state);
void radv_destroy_shadow_regs_preamble(struct radv_device *device, struct radv_queue_state *queue_state,
                                       struct radeon_winsys *ws);
void radv_emit_shadow_regs_preamble(struct radeon_cmdbuf *cs, const struct radv_device *device,
                                    struct radv_queue_state *queue_state);
VkResult radv_init_shadowed_regs_buffer_state(const struct radv_device *device, struct radv_queue *queue);

uint32_t radv_get_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer, bool instanced_draw, bool indirect_draw,
                                     bool count_from_stream_output, uint32_t draw_vertex_count, unsigned topology,
                                     bool prim_restart_enable, unsigned patch_control_points,
                                     unsigned num_tess_patches);
void radv_cs_emit_write_event_eop(struct radeon_cmdbuf *cs, enum amd_gfx_level gfx_level, enum radv_queue_family qf,
                                  unsigned event, unsigned event_flags, unsigned dst_sel, unsigned data_sel,
                                  uint64_t va, uint32_t new_fence, uint64_t gfx9_eop_bug_va);

struct radv_vgt_shader_key {
   uint8_t tess : 1;
   uint8_t gs : 1;
   uint8_t mesh_scratch_ring : 1;
   uint8_t mesh : 1;
   uint8_t ngg_passthrough : 1;
   uint8_t ngg : 1; /* gfx10+ */
   uint8_t ngg_streamout : 1;
   uint8_t hs_wave32 : 1;
   uint8_t gs_wave32 : 1;
   uint8_t vs_wave32 : 1;
};

void radv_cs_emit_cache_flush(struct radeon_winsys *ws, struct radeon_cmdbuf *cs, enum amd_gfx_level gfx_level,
                              uint32_t *flush_cnt, uint64_t flush_va, enum radv_queue_family qf,
                              enum radv_cmd_flush_bits flush_bits, enum rgp_flush_bits *sqtt_flush_bits,
                              uint64_t gfx9_eop_bug_va);
void radv_emit_cache_flush(struct radv_cmd_buffer *cmd_buffer);
void radv_emit_set_predication_state(struct radv_cmd_buffer *cmd_buffer, bool draw_visible, unsigned pred_op,
                                     uint64_t va);
void radv_emit_cond_exec(const struct radv_device *device, struct radeon_cmdbuf *cs, uint64_t va, uint32_t count);

void radv_cp_dma_buffer_copy(struct radv_cmd_buffer *cmd_buffer, uint64_t src_va, uint64_t dest_va, uint64_t size);
void radv_cs_cp_dma_prefetch(const struct radv_device *device, struct radeon_cmdbuf *cs, uint64_t va, unsigned size,
                             bool predicating);
void radv_cp_dma_prefetch(struct radv_cmd_buffer *cmd_buffer, uint64_t va, unsigned size);
void radv_cp_dma_clear_buffer(struct radv_cmd_buffer *cmd_buffer, uint64_t va, uint64_t size, unsigned value);
void radv_cp_dma_wait_for_idle(struct radv_cmd_buffer *cmd_buffer);

uint32_t radv_get_vgt_index_size(uint32_t type);

void radv_emit_vgt_shader_config(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs,
                                 const struct radv_vgt_shader_key *key);

void radv_emit_blend_state(struct radeon_cmdbuf *ctx_cs, const struct radv_shader *ps, uint32_t spi_shader_col_format,
                           uint32_t cb_shader_mask);

unsigned radv_instance_rate_prolog_index(unsigned num_attributes, uint32_t instance_rate_inputs);

struct radv_ps_epilog_state {
   uint8_t color_attachment_count;
   VkFormat color_attachment_formats[MAX_RTS];

   uint32_t color_write_mask;
   uint32_t color_blend_enable;

   uint32_t colors_written;
   bool mrt0_is_dual_src;
   bool export_depth;
   bool export_stencil;
   bool export_sample_mask;
   bool alpha_to_coverage_via_mrtz;
   bool alpha_to_one;
   uint8_t need_src_alpha;
};

struct radv_ps_epilog_key radv_generate_ps_epilog_key(const struct radv_device *device,
                                                      const struct radv_ps_epilog_state *state);

bool radv_needs_null_export_workaround(const struct radv_device *device, const struct radv_shader *ps,
                                       unsigned custom_blend_mode);

void radv_cmd_buffer_reset_rendering(struct radv_cmd_buffer *cmd_buffer);
bool radv_cmd_buffer_upload_alloc_aligned(struct radv_cmd_buffer *cmd_buffer, unsigned size, unsigned alignment,
                                          unsigned *out_offset, void **ptr);
bool radv_cmd_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer, unsigned size, unsigned *out_offset, void **ptr);
bool radv_cmd_buffer_upload_data(struct radv_cmd_buffer *cmd_buffer, unsigned size, const void *data,
                                 unsigned *out_offset);
void radv_write_vertex_descriptors(const struct radv_cmd_buffer *cmd_buffer,
                                   const struct radv_graphics_pipeline *pipeline, bool full_null_descriptors,
                                   void *vb_ptr);

void radv_emit_default_sample_locations(struct radeon_cmdbuf *cs, int nr_samples);
unsigned radv_get_default_max_sample_dist(int log_samples);
void radv_device_init_msaa(struct radv_device *device);

void radv_cs_write_data_imm(struct radeon_cmdbuf *cs, unsigned engine_sel, uint64_t va, uint32_t imm);

void radv_update_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                   VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects);

void radv_update_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                      int cb_idx, uint32_t color_values[2]);

void radv_update_fce_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                              const VkImageSubresourceRange *range, bool value);

void radv_update_dcc_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                              const VkImageSubresourceRange *range, bool value);
enum radv_cmd_flush_bits radv_src_access_flush(struct radv_cmd_buffer *cmd_buffer, VkAccessFlags2 src_flags,
                                               const struct radv_image *image);
enum radv_cmd_flush_bits radv_dst_access_flush(struct radv_cmd_buffer *cmd_buffer, VkAccessFlags2 dst_flags,
                                               const struct radv_image *image);

void radv_cmd_buffer_trace_emit(struct radv_cmd_buffer *cmd_buffer);

void radv_cmd_buffer_annotate(struct radv_cmd_buffer *cmd_buffer, const char *annotation);

static inline void
radv_emit_shader_pointer_head(struct radeon_cmdbuf *cs, unsigned sh_offset, unsigned pointer_count,
                              bool use_32bit_pointers)
{
   radeon_emit(cs, PKT3(PKT3_SET_SH_REG, pointer_count * (use_32bit_pointers ? 1 : 2), 0));
   radeon_emit(cs, (sh_offset - SI_SH_REG_OFFSET) >> 2);
}

static inline void
radv_emit_shader_pointer_body(const struct radv_device *device, struct radeon_cmdbuf *cs, uint64_t va,
                              bool use_32bit_pointers)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   radeon_emit(cs, va);

   if (use_32bit_pointers) {
      assert(va == 0 || (va >> 32) == pdev->info.address32_hi);
   } else {
      radeon_emit(cs, va >> 32);
   }
}

static inline void
radv_emit_shader_pointer(const struct radv_device *device, struct radeon_cmdbuf *cs, uint32_t sh_offset, uint64_t va,
                         bool global)
{
   bool use_32bit_pointers = !global;

   radv_emit_shader_pointer_head(cs, sh_offset, 1, use_32bit_pointers);
   radv_emit_shader_pointer_body(device, cs, va, use_32bit_pointers);
}

static inline unsigned
vk_to_bind_point(VkPipelineBindPoint bind_point)
{
   return bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ? 2 : bind_point;
}

static inline struct radv_descriptor_state *
radv_get_descriptors_state(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   return &cmd_buffer->descriptors[vk_to_bind_point(bind_point)];
}

static inline const struct radv_push_constant_state *
radv_get_push_constants_state(const struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   return &cmd_buffer->push_constant_state[vk_to_bind_point(bind_point)];
}

void radv_get_viewport_xform(const VkViewport *viewport, float scale[3], float translate[3]);

/*
 * Takes x,y,z as exact numbers of invocations, instead of blocks.
 *
 * Limitations: Can't call normal dispatch functions without binding or rebinding
 *              the compute pipeline.
 */
void radv_unaligned_dispatch(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z);

void radv_indirect_dispatch(struct radv_cmd_buffer *cmd_buffer, struct radeon_winsys_bo *bo, uint64_t va);

struct radv_ray_tracing_group;

struct radv_ray_tracing_stage;

struct radv_sqtt_shaders_reloc {
   struct radeon_winsys_bo *bo;
   union radv_shader_arena_block *alloc;
   uint64_t va[MESA_VULKAN_SHADER_STAGES];
};

struct radv_graphics_pipeline {
   struct radv_pipeline base;

   bool uses_drawid;
   bool uses_baseinstance;

   /* Whether the pipeline forces per-vertex VRS (GFX10.3+). */
   bool force_vrs_per_vertex;

   /* Whether the pipeline uses NGG (GFX10+). */
   bool is_ngg;
   bool has_ngg_culling;

   uint8_t vtx_emit_num;

   uint32_t vtx_base_sgpr;
   uint64_t dynamic_states;
   uint64_t needed_dynamic_state;

   VkShaderStageFlags active_stages;

   /* Used for rbplus */
   uint32_t col_format_non_compacted;

   struct radv_dynamic_state dynamic_state;

   struct radv_vs_input_state vs_input_state;

   struct radv_multisample_state ms;
   struct radv_ia_multi_vgt_param_helpers ia_multi_vgt_param;
   uint32_t binding_stride[MAX_VBS];
   uint8_t attrib_bindings[MAX_VERTEX_ATTRIBS];
   uint32_t attrib_ends[MAX_VERTEX_ATTRIBS];
   uint32_t attrib_index_offset[MAX_VERTEX_ATTRIBS];
   uint32_t db_render_control;

   /* Last pre-PS API stage */
   gl_shader_stage last_vgt_api_stage;

   unsigned rast_prim;

   /* For vk_graphics_pipeline_state */
   void *state_data;

   /* Custom blend mode for internal operations. */
   unsigned custom_blend_mode;

   /* Whether the pipeline uses out-of-order rasterization. */
   bool uses_out_of_order_rast;

   /* Whether the pipeline uses a VRS attachment. */
   bool uses_vrs_attachment;

   /* For graphics pipeline library */
   bool retain_shaders;

   /* For relocation of shaders with RGP. */
   struct radv_sqtt_shaders_reloc *sqtt_shaders_reloc;
};

struct radv_retained_shaders {
   struct {
      void *serialized_nir;
      size_t serialized_nir_size;
      unsigned char shader_sha1[SHA1_DIGEST_LENGTH];
      struct radv_shader_stage_key key;
   } stages[MESA_VULKAN_SHADER_STAGES];
};

struct radv_graphics_lib_pipeline {
   struct radv_graphics_pipeline base;

   struct radv_pipeline_layout layout;

   struct vk_graphics_pipeline_state graphics_state;

   VkGraphicsPipelineLibraryFlagsEXT lib_flags;

   struct radv_retained_shaders retained_shaders;

   void *mem_ctx;

   unsigned stage_count;
   VkPipelineShaderStageCreateInfo *stages;
   struct radv_shader_stage_key stage_keys[MESA_VULKAN_SHADER_STAGES];
};

RADV_DECL_PIPELINE_DOWNCAST(graphics, RADV_PIPELINE_GRAPHICS)
RADV_DECL_PIPELINE_DOWNCAST(graphics_lib, RADV_PIPELINE_GRAPHICS_LIB)

static inline bool
radv_pipeline_has_stage(const struct radv_graphics_pipeline *pipeline, gl_shader_stage stage)
{
   return pipeline->base.shaders[stage];
}

bool radv_pipeline_has_gs_copy_shader(const struct radv_pipeline *pipeline);

const struct radv_userdata_info *radv_get_user_sgpr(const struct radv_shader *shader, int idx);

struct radv_shader *radv_get_shader(struct radv_shader *const *shaders, gl_shader_stage stage);


void radv_emit_vertex_shader(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs, struct radeon_cmdbuf *cs,
                             const struct radv_shader *vs, const struct radv_shader *next_stage);

void radv_emit_tess_ctrl_shader(const struct radv_device *device, struct radeon_cmdbuf *cs,
                                const struct radv_shader *tcs);

void radv_emit_tess_eval_shader(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs,
                                struct radeon_cmdbuf *cs, const struct radv_shader *tes, const struct radv_shader *gs);

void radv_emit_fragment_shader(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs, struct radeon_cmdbuf *cs,
                               const struct radv_shader *ps);

void radv_emit_ps_inputs(const struct radv_device *device, struct radeon_cmdbuf *cs,
                         const struct radv_shader *last_vgt_shader, const struct radv_shader *ps);

struct radv_ia_multi_vgt_param_helpers radv_compute_ia_multi_vgt_param(const struct radv_device *device,
                                                                       struct radv_shader *const *shaders);

void radv_emit_vgt_reuse(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs, const struct radv_shader *tes,
                         const struct radv_vgt_shader_key *key);

void radv_emit_vgt_gs_out(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs,
                          uint32_t vgt_gs_out_prim_type);

void radv_emit_vgt_gs_mode(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs,
                           const struct radv_shader *last_vgt_api_shader);

void gfx103_emit_vgt_draw_payload_cntl(struct radeon_cmdbuf *ctx_cs, const struct radv_shader *mesh_shader,
                                       bool enable_vrs);

void gfx103_emit_vrs_state(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs, const struct radv_shader *ps,
                           bool enable_vrs, bool enable_vrs_coarse_shading, bool force_vrs_per_vertex);

void radv_emit_geometry_shader(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs, struct radeon_cmdbuf *cs,
                               const struct radv_shader *gs, const struct radv_shader *es,
                               const struct radv_shader *gs_copy_shader);

void radv_emit_mesh_shader(const struct radv_device *device, struct radeon_cmdbuf *ctx_cs, struct radeon_cmdbuf *cs,
                           const struct radv_shader *ms);

void radv_graphics_shaders_compile(struct radv_device *device, struct vk_pipeline_cache *cache,
                                   struct radv_shader_stage *stages, const struct radv_graphics_state_key *gfx_state,
                                   bool keep_executable_info, bool keep_statistic_info, bool is_internal,
                                   struct radv_retained_shaders *retained_shaders, bool noop_fs,
                                   struct radv_shader **shaders, struct radv_shader_binary **binaries,
                                   struct radv_shader **gs_copy_shader, struct radv_shader_binary **gs_copy_binary);


struct radv_graphics_pipeline_create_info {
   bool use_rectlist;
   bool db_depth_clear;
   bool db_stencil_clear;
   bool depth_compress_disable;
   bool stencil_compress_disable;
   bool resummarize_enable;
   uint32_t custom_blend_mode;
};


VkResult radv_graphics_pipeline_create(VkDevice device, VkPipelineCache cache,
                                       const VkGraphicsPipelineCreateInfo *pCreateInfo,
                                       const struct radv_graphics_pipeline_create_info *extra,
                                       const VkAllocationCallbacks *alloc, VkPipeline *pPipeline);


bool radv_pipeline_has_ngg(const struct radv_graphics_pipeline *pipeline);


struct vk_format_description;

VkResult radv_image_from_gralloc(VkDevice device_h, const VkImageCreateInfo *base_info,
                                 const VkNativeBufferANDROID *gralloc_info, const VkAllocationCallbacks *alloc,
                                 VkImage *out_image_h);
VkResult radv_import_ahb_memory(struct radv_device *device, struct radv_device_memory *mem, unsigned priority,
                                const VkImportAndroidHardwareBufferInfoANDROID *info);
VkResult radv_create_ahb_memory(struct radv_device *device, struct radv_device_memory *mem, unsigned priority,
                                const VkMemoryAllocateInfo *pAllocateInfo);

unsigned radv_ahb_format_for_vk_format(VkFormat vk_format);

VkFormat radv_select_android_external_format(const void *next, VkFormat default_format);

bool radv_android_gralloc_supports_format(VkFormat format, VkImageUsageFlagBits usage);

struct radv_resolve_barrier {
   VkPipelineStageFlags2 src_stage_mask;
   VkPipelineStageFlags2 dst_stage_mask;
   VkAccessFlags2 src_access_mask;
   VkAccessFlags2 dst_access_mask;
};

void radv_emit_resolve_barrier(struct radv_cmd_buffer *cmd_buffer, const struct radv_resolve_barrier *barrier);

void radv_set_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                             struct radv_descriptor_set *set, unsigned idx);

void radv_meta_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint pipelineBindPoint,
                                   VkPipelineLayout _layout, uint32_t set, uint32_t descriptorWriteCount,
                                   const VkWriteDescriptorSet *pDescriptorWrites);

uint32_t radv_init_dcc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                       const VkImageSubresourceRange *range, uint32_t value);

uint32_t radv_init_fmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range);

/* radv_nir_to_llvm.c */
struct radv_shader_args;
struct radv_nir_compiler_options;
struct radv_shader_info;

void llvm_compile_shader(const struct radv_nir_compiler_options *options, const struct radv_shader_info *info,
                         unsigned shader_count, struct nir_shader *const *shaders, struct radv_shader_binary **binary,
                         const struct radv_shader_args *args);

bool radv_sqtt_init(struct radv_device *device);
void radv_sqtt_finish(struct radv_device *device);
bool radv_begin_sqtt(struct radv_queue *queue);
bool radv_end_sqtt(struct radv_queue *queue);
bool radv_get_sqtt_trace(struct radv_queue *queue, struct ac_sqtt_trace *sqtt_trace);
void radv_reset_sqtt_trace(struct radv_device *device);
void radv_emit_sqtt_userdata(const struct radv_cmd_buffer *cmd_buffer, const void *data, uint32_t num_dwords);
bool radv_is_instruction_timing_enabled(void);
bool radv_sqtt_queue_events_enabled(void);
bool radv_sqtt_sample_clocks(struct radv_device *device);

void radv_emit_inhibit_clockgating(const struct radv_device *device, struct radeon_cmdbuf *cs, bool inhibit);
void radv_emit_spi_config_cntl(const struct radv_device *device, struct radeon_cmdbuf *cs, bool enable);

VkResult radv_sqtt_get_timed_cmdbuf(struct radv_queue *queue, struct radeon_winsys_bo *timestamp_bo,
                                    uint32_t timestamp_offset, VkPipelineStageFlags2 timestamp_stage,
                                    VkCommandBuffer *pcmdbuf);

VkResult radv_sqtt_acquire_gpu_timestamp(struct radv_device *device, struct radeon_winsys_bo **gpu_timestamp_bo,
                                         uint32_t *gpu_timestamp_offset, void **gpu_timestamp_ptr);

void radv_memory_trace_init(struct radv_device *device);
void radv_rmv_log_bo_allocate(struct radv_device *device, struct radeon_winsys_bo *bo, bool is_internal);
void radv_rmv_log_bo_destroy(struct radv_device *device, struct radeon_winsys_bo *bo);
void radv_rmv_log_heap_create(struct radv_device *device, VkDeviceMemory heap, bool is_internal,
                              VkMemoryAllocateFlags alloc_flags);
void radv_rmv_log_buffer_bind(struct radv_device *device, VkBuffer _buffer);
void radv_rmv_log_image_create(struct radv_device *device, const VkImageCreateInfo *create_info, bool is_internal,
                               VkImage _image);
void radv_rmv_log_image_bind(struct radv_device *device, VkImage _image);
void radv_rmv_log_query_pool_create(struct radv_device *device, VkQueryPool pool);
void radv_rmv_log_command_buffer_bo_create(struct radv_device *device, struct radeon_winsys_bo *bo,
                                           uint32_t executable_size, uint32_t data_size, uint32_t scratch_size);
void radv_rmv_log_command_buffer_bo_destroy(struct radv_device *device, struct radeon_winsys_bo *bo);
void radv_rmv_log_border_color_palette_create(struct radv_device *device, struct radeon_winsys_bo *bo);
void radv_rmv_log_border_color_palette_destroy(struct radv_device *device, struct radeon_winsys_bo *bo);
void radv_rmv_log_sparse_add_residency(struct radv_device *device, struct radeon_winsys_bo *src_bo, uint64_t offset);
void radv_rmv_log_sparse_remove_residency(struct radv_device *device, struct radeon_winsys_bo *src_bo, uint64_t offset);
void radv_rmv_log_descriptor_pool_create(struct radv_device *device, const VkDescriptorPoolCreateInfo *create_info,
                                         VkDescriptorPool pool);
void radv_rmv_log_graphics_pipeline_create(struct radv_device *device, struct radv_pipeline *pipeline,
                                           bool is_internal);
void radv_rmv_log_compute_pipeline_create(struct radv_device *device, struct radv_pipeline *pipeline, bool is_internal);
void radv_rmv_log_rt_pipeline_create(struct radv_device *device, struct radv_ray_tracing_pipeline *pipeline);
void radv_rmv_log_event_create(struct radv_device *device, VkEvent event, VkEventCreateFlags flags, bool is_internal);
void radv_rmv_log_resource_destroy(struct radv_device *device, uint64_t handle);
void radv_rmv_log_submit(struct radv_device *device, enum amd_ip_type type);
void radv_rmv_fill_device_info(const struct radv_physical_device *pdev, struct vk_rmv_device_info *info);
void radv_rmv_collect_trace_events(struct radv_device *device);
void radv_memory_trace_finish(struct radv_device *device);

/* radv_sqtt_layer_.c */
struct radv_barrier_data {
   union {
      struct {
         uint16_t depth_stencil_expand : 1;
         uint16_t htile_hiz_range_expand : 1;
         uint16_t depth_stencil_resummarize : 1;
         uint16_t dcc_decompress : 1;
         uint16_t fmask_decompress : 1;
         uint16_t fast_clear_eliminate : 1;
         uint16_t fmask_color_expand : 1;
         uint16_t init_mask_ram : 1;
         uint16_t reserved : 8;
      };
      uint16_t all;
   } layout_transitions;
};

/**
 * Value for the reason field of an RGP barrier start marker originating from
 * the Vulkan client (does not include PAL-defined values). (Table 15)
 */
enum rgp_barrier_reason {
   RGP_BARRIER_UNKNOWN_REASON = 0xFFFFFFFF,

   /* External app-generated barrier reasons, i.e. API synchronization
    * commands Range of valid values: [0x00000001 ... 0x7FFFFFFF].
    */
   RGP_BARRIER_EXTERNAL_CMD_PIPELINE_BARRIER = 0x00000001,
   RGP_BARRIER_EXTERNAL_RENDER_PASS_SYNC = 0x00000002,
   RGP_BARRIER_EXTERNAL_CMD_WAIT_EVENTS = 0x00000003,

   /* Internal barrier reasons, i.e. implicit synchronization inserted by
    * the Vulkan driver Range of valid values: [0xC0000000 ... 0xFFFFFFFE].
    */
   RGP_BARRIER_INTERNAL_BASE = 0xC0000000,
   RGP_BARRIER_INTERNAL_PRE_RESET_QUERY_POOL_SYNC = RGP_BARRIER_INTERNAL_BASE + 0,
   RGP_BARRIER_INTERNAL_POST_RESET_QUERY_POOL_SYNC = RGP_BARRIER_INTERNAL_BASE + 1,
   RGP_BARRIER_INTERNAL_GPU_EVENT_RECYCLE_STALL = RGP_BARRIER_INTERNAL_BASE + 2,
   RGP_BARRIER_INTERNAL_PRE_COPY_QUERY_POOL_RESULTS_SYNC = RGP_BARRIER_INTERNAL_BASE + 3
};

void radv_describe_begin_cmd_buffer(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_end_cmd_buffer(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_draw(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info);
void radv_describe_begin_render_pass_clear(struct radv_cmd_buffer *cmd_buffer, VkImageAspectFlagBits aspects);
void radv_describe_end_render_pass_clear(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_begin_render_pass_resolve(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_end_render_pass_resolve(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_barrier_start(struct radv_cmd_buffer *cmd_buffer, enum rgp_barrier_reason reason);
void radv_describe_barrier_end(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_barrier_end_delayed(struct radv_cmd_buffer *cmd_buffer);
void radv_describe_layout_transition(struct radv_cmd_buffer *cmd_buffer, const struct radv_barrier_data *barrier);
void radv_describe_begin_accel_struct_build(struct radv_cmd_buffer *cmd_buffer, uint32_t count);
void radv_describe_end_accel_struct_build(struct radv_cmd_buffer *cmd_buffer);

void radv_sqtt_emit_relocated_shaders(struct radv_cmd_buffer *cmd_buffer, struct radv_graphics_pipeline *pipeline);

void radv_write_user_event_marker(struct radv_cmd_buffer *cmd_buffer, enum rgp_sqtt_marker_user_event_type type,
                                  const char *str);

static inline uint32_t
radv_conv_prim_to_gs_out(uint32_t topology, bool is_ngg)
{
   switch (topology) {
   case V_008958_DI_PT_POINTLIST:
   case V_008958_DI_PT_PATCH:
      return V_028A6C_POINTLIST;
   case V_008958_DI_PT_LINELIST:
   case V_008958_DI_PT_LINESTRIP:
   case V_008958_DI_PT_LINELIST_ADJ:
   case V_008958_DI_PT_LINESTRIP_ADJ:
      return V_028A6C_LINESTRIP;
   case V_008958_DI_PT_TRILIST:
   case V_008958_DI_PT_TRISTRIP:
   case V_008958_DI_PT_TRIFAN:
   case V_008958_DI_PT_TRILIST_ADJ:
   case V_008958_DI_PT_TRISTRIP_ADJ:
      return V_028A6C_TRISTRIP;
   case V_008958_DI_PT_RECTLIST:
      return is_ngg ? V_028A6C_RECTLIST : V_028A6C_TRISTRIP;
   default:
      assert(0);
      return 0;
   }
}

static inline uint32_t
radv_translate_prim(unsigned topology)
{
   switch (topology) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return V_008958_DI_PT_POINTLIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return V_008958_DI_PT_LINELIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return V_008958_DI_PT_LINESTRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return V_008958_DI_PT_TRILIST;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return V_008958_DI_PT_TRISTRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return V_008958_DI_PT_TRIFAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      return V_008958_DI_PT_LINELIST_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      return V_008958_DI_PT_LINESTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      return V_008958_DI_PT_TRILIST_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
      return V_008958_DI_PT_TRISTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      return V_008958_DI_PT_PATCH;
   default:
      unreachable("unhandled primitive type");
   }
}

static inline bool
radv_prim_is_points_or_lines(unsigned topology)
{
   switch (topology) {
   case V_008958_DI_PT_POINTLIST:
   case V_008958_DI_PT_LINELIST:
   case V_008958_DI_PT_LINESTRIP:
   case V_008958_DI_PT_LINELIST_ADJ:
   case V_008958_DI_PT_LINESTRIP_ADJ:
      return true;
   default:
      return false;
   }
}

static inline bool
radv_rast_prim_is_point(unsigned rast_prim)
{
   return rast_prim == V_028A6C_POINTLIST;
}

static inline bool
radv_rast_prim_is_line(unsigned rast_prim)
{
   return rast_prim == V_028A6C_LINESTRIP;
}

static inline bool
radv_rast_prim_is_points_or_lines(unsigned rast_prim)
{
   return radv_rast_prim_is_point(rast_prim) || radv_rast_prim_is_line(rast_prim);
}

static inline bool
radv_polygon_mode_is_point(unsigned polygon_mode)
{
   return polygon_mode == V_028814_X_DRAW_POINTS;
}

static inline bool
radv_polygon_mode_is_line(unsigned polygon_mode)
{
   return polygon_mode == V_028814_X_DRAW_LINES;
}

static inline bool
radv_polygon_mode_is_points_or_lines(unsigned polygon_mode)
{
   return radv_polygon_mode_is_point(polygon_mode) || radv_polygon_mode_is_line(polygon_mode);
}

static inline bool
radv_primitive_topology_is_line_list(unsigned primitive_topology)
{
   return primitive_topology == V_008958_DI_PT_LINELIST || primitive_topology == V_008958_DI_PT_LINELIST_ADJ;
}

static inline unsigned
radv_get_num_vertices_per_prim(const struct radv_graphics_state_key *gfx_state)
{
   if (gfx_state->ia.topology == V_008958_DI_PT_NONE) {
      /* When the topology is unknown (with graphics pipeline library), return the maximum number of
       * vertices per primitives for VS. This is used to lower NGG (the HW will ignore the extra
       * bits for points/lines) and also to enable NGG culling unconditionally (it will be disabled
       * dynamically for points/lines).
       */
      return 3;
   } else {
      /* Need to add 1, because: V_028A6C_POINTLIST=0, V_028A6C_LINESTRIP=1, V_028A6C_TRISTRIP=2, etc. */
      return radv_conv_prim_to_gs_out(gfx_state->ia.topology, false) + 1;
   }
}

uint32_t radv_get_vgt_gs_out(struct radv_shader **shaders, uint32_t primitive_topology);

struct radv_vgt_shader_key radv_get_vgt_shader_key(const struct radv_device *device, struct radv_shader **shaders,
                                                   const struct radv_shader *gs_copy_shader);

static inline uint32_t
radv_translate_fill(VkPolygonMode func)
{
   switch (func) {
   case VK_POLYGON_MODE_FILL:
      return V_028814_X_DRAW_TRIANGLES;
   case VK_POLYGON_MODE_LINE:
      return V_028814_X_DRAW_LINES;
   case VK_POLYGON_MODE_POINT:
      return V_028814_X_DRAW_POINTS;
   default:
      assert(0);
      return V_028814_X_DRAW_POINTS;
   }
}

static inline uint32_t
radv_translate_stencil_op(enum VkStencilOp op)
{
   switch (op) {
   case VK_STENCIL_OP_KEEP:
      return V_02842C_STENCIL_KEEP;
   case VK_STENCIL_OP_ZERO:
      return V_02842C_STENCIL_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return V_02842C_STENCIL_REPLACE_TEST;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return V_02842C_STENCIL_ADD_CLAMP;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return V_02842C_STENCIL_SUB_CLAMP;
   case VK_STENCIL_OP_INVERT:
      return V_02842C_STENCIL_INVERT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return V_02842C_STENCIL_ADD_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return V_02842C_STENCIL_SUB_WRAP;
   default:
      return 0;
   }
}

static inline uint32_t
radv_translate_blend_logic_op(VkLogicOp op)
{
   switch (op) {
   case VK_LOGIC_OP_CLEAR:
      return V_028808_ROP3_CLEAR;
   case VK_LOGIC_OP_AND:
      return V_028808_ROP3_AND;
   case VK_LOGIC_OP_AND_REVERSE:
      return V_028808_ROP3_AND_REVERSE;
   case VK_LOGIC_OP_COPY:
      return V_028808_ROP3_COPY;
   case VK_LOGIC_OP_AND_INVERTED:
      return V_028808_ROP3_AND_INVERTED;
   case VK_LOGIC_OP_NO_OP:
      return V_028808_ROP3_NO_OP;
   case VK_LOGIC_OP_XOR:
      return V_028808_ROP3_XOR;
   case VK_LOGIC_OP_OR:
      return V_028808_ROP3_OR;
   case VK_LOGIC_OP_NOR:
      return V_028808_ROP3_NOR;
   case VK_LOGIC_OP_EQUIVALENT:
      return V_028808_ROP3_EQUIVALENT;
   case VK_LOGIC_OP_INVERT:
      return V_028808_ROP3_INVERT;
   case VK_LOGIC_OP_OR_REVERSE:
      return V_028808_ROP3_OR_REVERSE;
   case VK_LOGIC_OP_COPY_INVERTED:
      return V_028808_ROP3_COPY_INVERTED;
   case VK_LOGIC_OP_OR_INVERTED:
      return V_028808_ROP3_OR_INVERTED;
   case VK_LOGIC_OP_NAND:
      return V_028808_ROP3_NAND;
   case VK_LOGIC_OP_SET:
      return V_028808_ROP3_SET;
   default:
      unreachable("Unhandled logic op");
   }
}

static inline uint32_t
radv_translate_blend_function(VkBlendOp op)
{
   switch (op) {
   case VK_BLEND_OP_ADD:
      return V_028780_COMB_DST_PLUS_SRC;
   case VK_BLEND_OP_SUBTRACT:
      return V_028780_COMB_SRC_MINUS_DST;
   case VK_BLEND_OP_REVERSE_SUBTRACT:
      return V_028780_COMB_DST_MINUS_SRC;
   case VK_BLEND_OP_MIN:
      return V_028780_COMB_MIN_DST_SRC;
   case VK_BLEND_OP_MAX:
      return V_028780_COMB_MAX_DST_SRC;
   default:
      return 0;
   }
}

static inline uint32_t
radv_translate_blend_factor(enum amd_gfx_level gfx_level, VkBlendFactor factor)
{
   switch (factor) {
   case VK_BLEND_FACTOR_ZERO:
      return V_028780_BLEND_ZERO;
   case VK_BLEND_FACTOR_ONE:
      return V_028780_BLEND_ONE;
   case VK_BLEND_FACTOR_SRC_COLOR:
      return V_028780_BLEND_SRC_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      return V_028780_BLEND_ONE_MINUS_SRC_COLOR;
   case VK_BLEND_FACTOR_DST_COLOR:
      return V_028780_BLEND_DST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
      return V_028780_BLEND_ONE_MINUS_DST_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA:
      return V_028780_BLEND_SRC_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
      return V_028780_BLEND_ONE_MINUS_SRC_ALPHA;
   case VK_BLEND_FACTOR_DST_ALPHA:
      return V_028780_BLEND_DST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
      return V_028780_BLEND_ONE_MINUS_DST_ALPHA;
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
      return gfx_level >= GFX11 ? V_028780_BLEND_CONSTANT_COLOR_GFX11 : V_028780_BLEND_CONSTANT_COLOR_GFX6;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
      return gfx_level >= GFX11 ? V_028780_BLEND_ONE_MINUS_CONSTANT_COLOR_GFX11
                                : V_028780_BLEND_ONE_MINUS_CONSTANT_COLOR_GFX6;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
      return gfx_level >= GFX11 ? V_028780_BLEND_CONSTANT_ALPHA_GFX11 : V_028780_BLEND_CONSTANT_ALPHA_GFX6;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      return gfx_level >= GFX11 ? V_028780_BLEND_ONE_MINUS_CONSTANT_ALPHA_GFX11
                                : V_028780_BLEND_ONE_MINUS_CONSTANT_ALPHA_GFX6;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
      return V_028780_BLEND_SRC_ALPHA_SATURATE;
   case VK_BLEND_FACTOR_SRC1_COLOR:
      return gfx_level >= GFX11 ? V_028780_BLEND_SRC1_COLOR_GFX11 : V_028780_BLEND_SRC1_COLOR_GFX6;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
      return gfx_level >= GFX11 ? V_028780_BLEND_INV_SRC1_COLOR_GFX11 : V_028780_BLEND_INV_SRC1_COLOR_GFX6;
   case VK_BLEND_FACTOR_SRC1_ALPHA:
      return gfx_level >= GFX11 ? V_028780_BLEND_SRC1_ALPHA_GFX11 : V_028780_BLEND_SRC1_ALPHA_GFX6;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return gfx_level >= GFX11 ? V_028780_BLEND_INV_SRC1_ALPHA_GFX11 : V_028780_BLEND_INV_SRC1_ALPHA_GFX6;
   default:
      return 0;
   }
}

static inline uint32_t
radv_translate_blend_opt_factor(VkBlendFactor factor, bool is_alpha)
{
   switch (factor) {
   case VK_BLEND_FACTOR_ZERO:
      return V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_ALL;
   case VK_BLEND_FACTOR_ONE:
      return V_028760_BLEND_OPT_PRESERVE_ALL_IGNORE_NONE;
   case VK_BLEND_FACTOR_SRC_COLOR:
      return is_alpha ? V_028760_BLEND_OPT_PRESERVE_A1_IGNORE_A0 : V_028760_BLEND_OPT_PRESERVE_C1_IGNORE_C0;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      return is_alpha ? V_028760_BLEND_OPT_PRESERVE_A0_IGNORE_A1 : V_028760_BLEND_OPT_PRESERVE_C0_IGNORE_C1;
   case VK_BLEND_FACTOR_SRC_ALPHA:
      return V_028760_BLEND_OPT_PRESERVE_A1_IGNORE_A0;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
      return V_028760_BLEND_OPT_PRESERVE_A0_IGNORE_A1;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
      return is_alpha ? V_028760_BLEND_OPT_PRESERVE_ALL_IGNORE_NONE : V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_A0;
   default:
      return V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_NONE;
   }
}

static inline uint32_t
radv_translate_blend_opt_function(VkBlendOp op)
{
   switch (op) {
   case VK_BLEND_OP_ADD:
      return V_028760_OPT_COMB_ADD;
   case VK_BLEND_OP_SUBTRACT:
      return V_028760_OPT_COMB_SUBTRACT;
   case VK_BLEND_OP_REVERSE_SUBTRACT:
      return V_028760_OPT_COMB_REVSUBTRACT;
   case VK_BLEND_OP_MIN:
      return V_028760_OPT_COMB_MIN;
   case VK_BLEND_OP_MAX:
      return V_028760_OPT_COMB_MAX;
   default:
      return V_028760_OPT_COMB_BLEND_DISABLED;
   }
}

static inline bool
radv_blend_factor_uses_dst(VkBlendFactor factor)
{
   return factor == VK_BLEND_FACTOR_DST_COLOR || factor == VK_BLEND_FACTOR_DST_ALPHA ||
          factor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE || factor == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ||
          factor == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
}

static inline bool
radv_is_dual_src(VkBlendFactor factor)
{
   switch (factor) {
   case VK_BLEND_FACTOR_SRC1_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
   case VK_BLEND_FACTOR_SRC1_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return true;
   default:
      return false;
   }
}

static ALWAYS_INLINE bool
radv_can_enable_dual_src(const struct vk_color_blend_attachment_state *att)
{
   VkBlendOp eqRGB = att->color_blend_op;
   VkBlendFactor srcRGB = att->src_color_blend_factor;
   VkBlendFactor dstRGB = att->dst_color_blend_factor;
   VkBlendOp eqA = att->alpha_blend_op;
   VkBlendFactor srcA = att->src_alpha_blend_factor;
   VkBlendFactor dstA = att->dst_alpha_blend_factor;
   bool eqRGB_minmax = eqRGB == VK_BLEND_OP_MIN || eqRGB == VK_BLEND_OP_MAX;
   bool eqA_minmax = eqA == VK_BLEND_OP_MIN || eqA == VK_BLEND_OP_MAX;

   if (!eqRGB_minmax && (radv_is_dual_src(srcRGB) || radv_is_dual_src(dstRGB)))
      return true;
   if (!eqA_minmax && (radv_is_dual_src(srcA) || radv_is_dual_src(dstA)))
      return true;
   return false;
}

static inline void
radv_normalize_blend_factor(VkBlendOp op, VkBlendFactor *src_factor, VkBlendFactor *dst_factor)
{
   if (op == VK_BLEND_OP_MIN || op == VK_BLEND_OP_MAX) {
      *src_factor = VK_BLEND_FACTOR_ONE;
      *dst_factor = VK_BLEND_FACTOR_ONE;
   }
}

void radv_blend_remove_dst(VkBlendOp *func, VkBlendFactor *src_factor, VkBlendFactor *dst_factor,
                           VkBlendFactor expected_dst, VkBlendFactor replacement_src);

ALWAYS_INLINE static bool
radv_is_streamout_enabled(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;

   /* Streamout must be enabled for the PRIMITIVES_GENERATED query to work. */
   return (so->streamout_enabled || cmd_buffer->state.active_prims_gen_queries) && !cmd_buffer->state.suspend_streamout;
}

/*
 * Queue helper to get ring.
 * placed here as it needs queue + device structs.
 */
static inline enum amd_ip_type
radv_queue_ring(const struct radv_queue *queue)
{
   struct radv_device *device = radv_queue_device(queue);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return radv_queue_family_to_ring(pdev, queue->state.qf);
}

unsigned radv_compact_spi_shader_col_format(const struct radv_shader *ps, uint32_t spi_shader_col_format);

/* radv_spm.c */
bool radv_spm_init(struct radv_device *device);
void radv_spm_finish(struct radv_device *device);
void radv_emit_spm_setup(struct radv_device *device, struct radeon_cmdbuf *cs, enum radv_queue_family qf);

void radv_destroy_graphics_pipeline(struct radv_device *device, struct radv_graphics_pipeline *pipeline);
void radv_destroy_graphics_lib_pipeline(struct radv_device *device, struct radv_graphics_lib_pipeline *pipeline);

void radv_begin_conditional_rendering(struct radv_cmd_buffer *cmd_buffer, uint64_t va, bool draw_visible);
void radv_end_conditional_rendering(struct radv_cmd_buffer *cmd_buffer);

bool radv_gang_init(struct radv_cmd_buffer *cmd_buffer);
void radv_gang_cache_flush(struct radv_cmd_buffer *cmd_buffer);

#define RADV_FROM_HANDLE(__radv_type, __name, __handle) VK_FROM_HANDLE(__radv_type, __name, __handle)

VK_DEFINE_HANDLE_CASTS(radv_cmd_buffer, vk.base, VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(radv_shader_object, base, VkShaderEXT, VK_OBJECT_TYPE_SHADER_EXT);

static inline uint64_t
radv_get_tdr_timeout_for_ip(enum amd_ip_type ip_type)
{
   const uint64_t compute_tdr_duration_ns = 60000000000ull; /* 1 minute (default in kernel) */
   const uint64_t other_tdr_duration_ns = 10000000000ull;   /* 10 seconds (default in kernel) */

   return ip_type == AMD_IP_COMPUTE ? compute_tdr_duration_ns : other_tdr_duration_ns;
}

#ifdef __cplusplus
}
#endif

#endif /* RADV_PRIVATE_H */
