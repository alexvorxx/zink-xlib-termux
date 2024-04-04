/*
 * Copyright © 2022 Valve Corporation
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
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#ifndef ZINK_TYPES_H
#define ZINK_TYPES_H

#include <vulkan/vulkan.h>

#include "compiler/nir/nir.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"

#include "pipebuffer/pb_cache.h"
#include "pipebuffer/pb_slab.h"

#include "util/disk_cache.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/log.h"
#include "util/set.h"
#include "util/simple_mtx.h"
#include "util/slab.h"
#include "util/u_dynarray.h"
#include "util/u_idalloc.h"
#include "util/u_live_shader_cache.h"
#include "util/u_queue.h"
#include "util/u_range.h"
#include "util/u_threaded_context.h"
#include "util/u_transfer.h"
#include "util/u_vertex_state_cache.h"

#include "vulkan/util/vk_util.h"

#include "zink_device_info.h"
#include "zink_instance.h"
#include "zink_shader_keys.h"
#include "vk_dispatch_table.h"


/* the descriptor binding id for fbfetch/input attachment */
#define ZINK_FBFETCH_BINDING 5
#define ZINK_GFX_SHADER_COUNT 5

/* number of descriptors to allocate in a pool */
#define MAX_LAZY_DESCRIPTORS 500
/* explicit clamping because descriptor caching used to exist */
#define ZINK_MAX_SHADER_IMAGES 32
/* total number of bindless ids that can be allocated */
#define ZINK_MAX_BINDLESS_HANDLES 1024

/* enum zink_descriptor_type */
#define ZINK_MAX_DESCRIPTOR_SETS 6
#define ZINK_MAX_DESCRIPTORS_PER_TYPE (32 * ZINK_GFX_SHADER_COUNT)

/* suballocator defines */
#define NUM_SLAB_ALLOCATORS 3
#define MIN_SLAB_ORDER 8


/* this is the spec minimum */
#define ZINK_SPARSE_BUFFER_PAGE_SIZE (64 * 1024)

/* flag to create screen->copy_context */
#define ZINK_CONTEXT_COPY_ONLY (1<<30)

/* convenience macros for accessing dispatch table functions */
#define VKCTX(fn) zink_screen(ctx->base.screen)->vk.fn
#define VKSCR(fn) screen->vk.fn

/** enums */

/* features for draw/program templates */
typedef enum {
   ZINK_NO_MULTIDRAW,
   ZINK_MULTIDRAW,
} zink_multidraw;

typedef enum {
   ZINK_NO_DYNAMIC_STATE,
   ZINK_DYNAMIC_STATE,
   ZINK_DYNAMIC_STATE2,
   ZINK_DYNAMIC_VERTEX_INPUT2,
   ZINK_DYNAMIC_STATE3,
   ZINK_DYNAMIC_VERTEX_INPUT,
} zink_dynamic_state;

typedef enum {
   ZINK_PIPELINE_NO_DYNAMIC_STATE,
   ZINK_PIPELINE_DYNAMIC_STATE,
   ZINK_PIPELINE_DYNAMIC_STATE2,
   ZINK_PIPELINE_DYNAMIC_STATE2_PCP,
   ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT2,
   ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT2_PCP,
   ZINK_PIPELINE_DYNAMIC_STATE3,
   ZINK_PIPELINE_DYNAMIC_STATE3_PCP,
   ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT,
   ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT_PCP,
} zink_pipeline_dynamic_state;

enum zink_blit_flags {
   ZINK_BLIT_NORMAL = 1 << 0,
   ZINK_BLIT_SAVE_FS = 1 << 1,
   ZINK_BLIT_SAVE_FB = 1 << 2,
   ZINK_BLIT_SAVE_TEXTURES = 1 << 3,
   ZINK_BLIT_NO_COND_RENDER = 1 << 4,
};

/* descriptor types; also the ordering of the sets
 * ...except that ZINK_DESCRIPTOR_BASE_TYPES is actually ZINK_DESCRIPTOR_TYPE_UNIFORMS,
 * and all base type values are thus +1 to get the set id (using screen->desc_set_id[idx])
 */
enum zink_descriptor_type {
   ZINK_DESCRIPTOR_TYPE_UBO,
   ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW,
   ZINK_DESCRIPTOR_TYPE_SSBO,
   ZINK_DESCRIPTOR_TYPE_IMAGE,
   ZINK_DESCRIPTOR_BASE_TYPES, /**< the count/iterator for basic descriptor types */
   ZINK_DESCRIPTOR_BINDLESS,
   ZINK_DESCRIPTOR_ALL_TYPES,
   ZINK_DESCRIPTOR_TYPE_UNIFORMS = ZINK_DESCRIPTOR_BASE_TYPES, /**< this is aliased for convenience */
   ZINK_DESCRIPTOR_NON_BINDLESS_TYPES = ZINK_DESCRIPTOR_BASE_TYPES + 1, /**< for struct sizing */
};

/* indexing for descriptor template management */
enum zink_descriptor_size_index {
   ZDS_INDEX_UBO,
   ZDS_INDEX_COMBINED_SAMPLER,
   ZDS_INDEX_UNIFORM_TEXELS,
   ZDS_INDEX_SAMPLER,
   ZDS_INDEX_STORAGE_BUFFER,
   ZDS_INDEX_STORAGE_IMAGE,
   ZDS_INDEX_STORAGE_TEXELS,
   ZDS_INDEX_MAX,
};

/* indexing for descriptor template management in COMPACT mode */
enum zink_descriptor_size_index_compact {
   ZDS_INDEX_COMP_UBO,
   ZDS_INDEX_COMP_STORAGE_BUFFER,
   ZDS_INDEX_COMP_COMBINED_SAMPLER,
   ZDS_INDEX_COMP_UNIFORM_TEXELS,
   ZDS_INDEX_COMP_SAMPLER,
   ZDS_INDEX_COMP_STORAGE_IMAGE,
   ZDS_INDEX_COMP_STORAGE_TEXELS,
};

enum zink_resource_access {
   ZINK_RESOURCE_ACCESS_READ = 1,
   ZINK_RESOURCE_ACCESS_WRITE = 32,
   ZINK_RESOURCE_ACCESS_RW = ZINK_RESOURCE_ACCESS_READ | ZINK_RESOURCE_ACCESS_WRITE,
};


enum zink_heap {
   ZINK_HEAP_DEVICE_LOCAL,
   ZINK_HEAP_DEVICE_LOCAL_SPARSE,
   ZINK_HEAP_DEVICE_LOCAL_LAZY,
   ZINK_HEAP_DEVICE_LOCAL_VISIBLE,
   ZINK_HEAP_HOST_VISIBLE_COHERENT,
   ZINK_HEAP_HOST_VISIBLE_CACHED,
   ZINK_HEAP_MAX,
};

enum zink_alloc_flag {
   ZINK_ALLOC_SPARSE = 1<<0,
   ZINK_ALLOC_NO_SUBALLOC = 1<<1,
};

enum zink_debug {
   ZINK_DEBUG_NIR = (1<<0),
   ZINK_DEBUG_SPIRV = (1<<1),
   ZINK_DEBUG_TGSI = (1<<2),
   ZINK_DEBUG_VALIDATION = (1<<3),
   ZINK_DEBUG_SYNC = (1<<4),
   ZINK_DEBUG_COMPACT = (1<<5),
   ZINK_DEBUG_NOREORDER = (1<<6),
   ZINK_DEBUG_GPL = (1<<7),
   ZINK_DEBUG_SHADERDB = (1<<8),
   ZINK_DEBUG_RP = (1<<9),
};


/** fence types */
struct tc_unflushed_batch_token;

/* an async fence created for tc */
struct zink_tc_fence {
   struct pipe_reference reference;
   /* enables distinction between tc fence submission and vk queue submission */
   uint32_t submit_count;
   /* when the tc fence is signaled for use */
   struct util_queue_fence ready;
   struct tc_unflushed_batch_token *tc_token;
   /* for deferred flushes */
   struct pipe_context *deferred_ctx;
   /* multiple tc fences may point to a real fence */
   struct zink_fence *fence;
   /* for use with semaphore/imported fences */
   VkSemaphore sem;
};

/* a fence is actually a zink_batch_state, but these are split out for logical consistency */
struct zink_fence {
   VkFence fence;
   uint64_t batch_id;
   bool submitted;
   bool completed;
};


/** state types */

struct zink_vertex_elements_hw_state {
   uint32_t hash;
   uint32_t num_bindings, num_attribs;
   /* VK_EXT_vertex_input_dynamic_state uses different types */
   union {
      VkVertexInputAttributeDescription attribs[PIPE_MAX_ATTRIBS];
      VkVertexInputAttributeDescription2EXT dynattribs[PIPE_MAX_ATTRIBS];
   };
   union {
      struct {
         VkVertexInputBindingDivisorDescriptionEXT divisors[PIPE_MAX_ATTRIBS];
         VkVertexInputBindingDescription bindings[PIPE_MAX_ATTRIBS]; // combination of element_state and stride
         uint8_t divisors_present;
      } b;
      VkVertexInputBindingDescription2EXT dynbindings[PIPE_MAX_ATTRIBS];
   };
   uint8_t binding_map[PIPE_MAX_ATTRIBS];
};

struct zink_vertex_elements_state {
   /* decomposed attributes read only a single component for format compatibility */
   bool has_decomposed_attrs;
   struct {
      uint32_t binding;
      VkVertexInputRate inputRate;
   } bindings[PIPE_MAX_ATTRIBS];
   uint32_t divisor[PIPE_MAX_ATTRIBS];
   uint32_t min_stride[PIPE_MAX_ATTRIBS]; //for dynamic_state1
   uint32_t decomposed_attrs;
   unsigned decomposed_attrs_size;
   uint32_t decomposed_attrs_without_w;
   unsigned decomposed_attrs_without_w_size;
   struct zink_vertex_elements_hw_state hw_state;
};

/* for vertex state draws */
struct zink_vertex_state {
   struct pipe_vertex_state b;
   struct zink_vertex_elements_state velems;
   struct set masks;
};

struct zink_rasterizer_hw_state {
   unsigned polygon_mode : 2; //VkPolygonMode
   unsigned line_mode : 2; //VkLineRasterizationModeEXT
   unsigned depth_clip:1;
   unsigned depth_clamp:1;
   unsigned pv_last:1;
   unsigned line_stipple_enable:1;
   unsigned clip_halfz:1;
};

struct zink_rasterizer_state {
   struct pipe_rasterizer_state base;
   bool offset_fill;
   float offset_units, offset_clamp, offset_scale;
   float line_width;
   VkFrontFace front_face;
   VkCullModeFlags cull_mode;
   struct zink_rasterizer_hw_state hw_state;
};

struct zink_blend_state {
   uint32_t hash;
   VkPipelineColorBlendAttachmentState attachments[PIPE_MAX_COLOR_BUFS];

   struct {
      VkBool32 enables[PIPE_MAX_COLOR_BUFS];
      VkColorBlendEquationEXT eq[PIPE_MAX_COLOR_BUFS];
      VkColorComponentFlags wrmask[PIPE_MAX_COLOR_BUFS];
   } ds3;

   VkBool32 logicop_enable;
   VkLogicOp logicop_func;

   VkBool32 alpha_to_coverage;
   VkBool32 alpha_to_one;

   bool need_blend_constants;
   bool dual_src_blend;
};

struct zink_depth_stencil_alpha_hw_state {
   VkBool32 depth_test;
   VkCompareOp depth_compare_op;

   VkBool32 depth_bounds_test;
   float min_depth_bounds, max_depth_bounds;

   VkBool32 stencil_test;
   VkStencilOpState stencil_front;
   VkStencilOpState stencil_back;

   VkBool32 depth_write;
};

struct zink_depth_stencil_alpha_state {
   struct pipe_depth_stencil_alpha_state base;
   struct zink_depth_stencil_alpha_hw_state hw_state;
};


/** descriptor types */

/* zink_descriptor_layout objects are cached: this is the key for one */
struct zink_descriptor_layout_key {
   unsigned num_bindings;
   VkDescriptorSetLayoutBinding *bindings;
};

struct zink_descriptor_layout {
   VkDescriptorSetLayout layout;
};

/* descriptor pools are cached: zink_descriptor_pool_key::id is the id for a type of pool */
struct zink_descriptor_pool_key {
   unsigned use_count;
   unsigned num_type_sizes;
   unsigned id;
   VkDescriptorPoolSize sizes[4];
   struct zink_descriptor_layout_key *layout;
};

/* ctx->dd; created at context creation */
struct zink_descriptor_data {
   bool bindless_bound;
   bool has_fbfetch;
   bool push_state_changed[2]; //gfx, compute
   uint8_t state_changed[2]; //gfx, compute
   struct zink_descriptor_layout_key *push_layout_keys[2]; //gfx, compute
   struct zink_descriptor_layout *push_dsl[2]; //gfx, compute
   VkDescriptorUpdateTemplate push_template[2]; //gfx, compute

   struct zink_descriptor_layout *dummy_dsl;

   VkDescriptorSetLayout bindless_layout;
   VkDescriptorPool bindless_pool;
   VkDescriptorSet bindless_set;

   struct zink_program *pg[2]; //gfx, compute

   VkDescriptorUpdateTemplateEntry push_entries[MESA_SHADER_STAGES]; //gfx+fbfetch
   VkDescriptorUpdateTemplateEntry compute_push_entry;
};

/* pg->dd; created at program creation */
struct zink_program_descriptor_data {
   bool bindless;
   bool fbfetch;
   /* bitmask of ubo0 usage for stages */
   uint8_t push_usage;
   /* bitmask of which sets are used by the program */
   uint8_t binding_usage;
   /* all the pool keys for the program */
   struct zink_descriptor_pool_key *pool_key[ZINK_DESCRIPTOR_BASE_TYPES]; //push set doesn't need one
   /* all the layouts for the program */
   struct zink_descriptor_layout *layouts[ZINK_DESCRIPTOR_NON_BINDLESS_TYPES];
   /* all the templates for the program */
   VkDescriptorUpdateTemplate templates[ZINK_DESCRIPTOR_NON_BINDLESS_TYPES];
};

struct zink_descriptor_pool {
   /* the current index of 'sets' */
   unsigned set_idx;
   /* number of sets allocated */
   unsigned sets_alloc;
   VkDescriptorPool pool;
   /* sets are lazily allocated */
   VkDescriptorSet sets[MAX_LAZY_DESCRIPTORS];
};

/* a zink_descriptor_pool_key matches up to this struct */
struct zink_descriptor_pool_multi {
   /* for flagging when overflowed pools must be destroyed instead of reused */
   bool reinit_overflow;
   /* this flips to split usable overflow from in-use overflow */
   unsigned overflow_idx;
   /* zink_descriptor_pool objects that have exceeded MAX_LAZY_DESCRIPTORS sets */
   struct util_dynarray overflowed_pools[2];
   /* the current pool; may be null */
   struct zink_descriptor_pool *pool;
   /* pool key for convenience */
   const struct zink_descriptor_pool_key *pool_key;
};

/* bs->dd; created on batch state creation */
struct zink_batch_descriptor_data {
   /* pools have fbfetch initialized */
   bool has_fbfetch;
   /* real size of 'pools' */
   unsigned pool_size[ZINK_DESCRIPTOR_BASE_TYPES];
   /* this array is sized based on the max zink_descriptor_pool_key::id used by the batch; members may be NULL */
   struct util_dynarray pools[ZINK_DESCRIPTOR_BASE_TYPES];
   struct zink_descriptor_pool_multi push_pool[2]; //gfx, compute
   /* the current program (for descriptor updating) */
   struct zink_program *pg[2]; //gfx, compute
   /* the current pipeline compatibility id (for pipeline compatibility rules) */
   uint32_t compat_id[2]; //gfx, compute
   /* the current set layout */
   VkDescriptorSetLayout dsl[2][ZINK_DESCRIPTOR_BASE_TYPES]; //gfx, compute
   /* the current set for a given type; used for rebinding if pipeline compat id changes and current set must be rebound */
   VkDescriptorSet sets[2][ZINK_DESCRIPTOR_NON_BINDLESS_TYPES]; //gfx, compute
   /* mask of push descriptor usage */
   unsigned push_usage[2]; //gfx, compute
};

/** batch types */
/* zink_batch_usage concepts:
 * - batch "usage" is an indicator of when and how a BO was accessed
 * - batch "tracking" is the batch state(s) containing an extra ref for a BO
 *
 * - usage prevents a BO from being mapped while it has pending+conflicting access
 * - usage affects pipeline barrier generation for synchronizing reads and writes
 * - usage MUST be removed before context destruction to avoid crashing during BO
 *   reclaiming in suballocator
 *
 * - tracking prevents a BO from being destroyed early
 * - tracking enables usage to be pruned
 *
 *
 * tracking is added:
 * - any time a BO is used in a "one-off" operation (e.g., blit, index buffer, indirect buffer)
 * - any time a descriptor is unbound
 * - when a buffer is replaced (IFF: resource is bound as a descriptor or usage previously existed)
 *
 * tracking is removed:
 * - in zink_reset_batch_state()
 *
 * usage is added:
 * - any time a BO is used in a "one-off" operation (e.g., blit, index buffer, indirect buffer)
 * - any time a descriptor is bound
 * - any time a descriptor is unbound (IFF: usage previously existed)
 * - for all bound descriptors on the first draw/dispatch after a flush (zink_update_descriptor_refs)
 *
 * usage is removed:
 * - when tracking is removed (IFF: BO usage == tracking, i.e., this is the last batch that a BO was active on)
 */
struct zink_batch_usage {
   uint32_t usage;
   cnd_t flush;
   mtx_t mtx;
   bool unflushed;
};

struct zink_batch_obj_list {
   unsigned max_buffers;
   unsigned num_buffers;
   struct zink_resource_object **objs;
};

struct zink_batch_state {
   struct zink_fence fence;
   struct zink_batch_state *next;

   struct zink_batch_usage usage;
   struct zink_context *ctx;
   VkCommandPool cmdpool;
   VkCommandBuffer cmdbuf;
   VkCommandBuffer barrier_cmdbuf;
   VkSemaphore signal_semaphore; //external signal semaphore
   struct util_dynarray wait_semaphores; //external wait semaphores
   struct util_dynarray wait_semaphore_stages; //external wait semaphores

   VkSemaphore present;
   struct zink_resource *swapchain;
   struct util_dynarray acquires;
   struct util_dynarray acquire_flags;
   struct util_dynarray unref_semaphores;

   struct util_queue_fence flush_completed;

   struct set programs;

#define BUFFER_HASHLIST_SIZE 32768
   /* buffer_indices_hashlist[hash(bo)] returns -1 if the bo
    * isn't part of any buffer lists or the index where the bo could be found.
    * Since 1) hash collisions of 2 different bo can happen and 2) we use a
    * single hashlist for the 3 buffer list, this is only a hint.
    * batch_find_resource uses this hint to speed up buffers look up.
    */
   int16_t buffer_indices_hashlist[BUFFER_HASHLIST_SIZE];
   struct zink_batch_obj_list real_objs;
   struct zink_batch_obj_list slab_objs;
   struct zink_batch_obj_list sparse_objs;
   struct zink_resource_object *last_added_obj;
   struct util_dynarray swapchain_obj; //this doesn't have a zink_bo and must be handled differently

   struct util_dynarray unref_resources;
   struct util_dynarray bindless_releases[2];

   struct util_dynarray persistent_resources;
   struct util_dynarray zombie_samplers;
   struct util_dynarray dead_framebuffers;

   struct set active_queries; /* zink_query objects which were active at some point in this batch */

   struct zink_batch_descriptor_data dd;

   VkDeviceSize resource_size;

    /* this is a monotonic int used to disambiguate internal fences from their tc fence references */
   unsigned submit_count;

   bool is_device_lost;

   bool have_timelines;

   bool has_barriers;
};

static inline struct zink_batch_state *
zink_batch_state(struct zink_fence *fence)
{
   return (struct zink_batch_state *)fence;
}

struct zink_batch {
   struct zink_batch_state *state;

   struct zink_batch_usage *last_batch_usage;
   struct zink_resource *swapchain;

   unsigned work_count;

   bool has_work;
   bool last_was_compute;
   bool in_rp; //renderpass is currently active
};


/** bo types */
struct bo_export {
   /** File descriptor associated with a handle export. */
   int drm_fd;

   /** GEM handle in drm_fd */
   uint32_t gem_handle;

   struct list_head link;
};

struct zink_bo {
   struct pb_buffer base;

   union {
      struct {
         void *cpu_ptr; /* for user_ptr and permanent maps */
         int map_count;
         struct list_head exports;
         simple_mtx_t export_lock;

         bool is_user_ptr;
         bool use_reusable_pool;

         /* Whether buffer_get_handle or buffer_from_handle has been called,
          * it can only transition from false to true. Protected by lock.
          */
         bool is_shared;
      } real;
      struct {
         struct pb_slab_entry entry;
         struct zink_bo *real;
      } slab;
      struct {
         uint32_t num_va_pages;
         uint32_t num_backing_pages;

         struct list_head backing;

         /* Commitment information for each page of the virtual memory area. */
         struct zink_sparse_commitment *commitments;
      } sparse;
   } u;

   VkDeviceMemory mem;
   uint64_t offset;

   uint32_t unique_id;

   simple_mtx_t lock;

   struct zink_batch_usage *reads;
   struct zink_batch_usage *writes;

   struct pb_cache_entry cache_entry[];
};

static inline struct zink_bo *
zink_bo(struct pb_buffer *pbuf)
{
   return (struct zink_bo*)pbuf;
}

/** clear types */
struct zink_framebuffer_clear_data {
   union {
      union pipe_color_union color;
      struct {
         float depth;
         unsigned stencil;
         uint8_t bits : 2; // PIPE_CLEAR_DEPTH, PIPE_CLEAR_STENCIL
      } zs;
   };
   struct pipe_scissor_state scissor;
   bool has_scissor;
   bool conditional;
};

struct zink_framebuffer_clear {
   struct util_dynarray clears;
};


/** compiler types */
struct zink_shader_info {
   struct pipe_stream_output_info so_info;
   unsigned so_info_slots[PIPE_MAX_SO_OUTPUTS];
   uint32_t so_propagate; //left shifted by 32
   uint32_t sampler_mask;
   bool last_vertex;
   bool have_xfb;
   bool have_sparse;
   bool have_vulkan_memory_model;
};

struct zink_shader {
   struct util_live_shader base;
   uint32_t hash;
   struct nir_shader *nir;

   struct zink_shader_info sinfo;

   struct {
      int index;
      int binding;
      VkDescriptorType type;
      unsigned char size;
   } bindings[ZINK_DESCRIPTOR_BASE_TYPES][ZINK_MAX_DESCRIPTORS_PER_TYPE];
   size_t num_bindings[ZINK_DESCRIPTOR_BASE_TYPES];
   unsigned num_texel_buffers;
   uint32_t ubos_used; // bitfield of which ubo indices are used
   uint32_t ssbos_used; // bitfield of which ssbo indices are used
   bool bindless;
   bool can_inline;
   bool has_uniforms;
   struct spirv_shader *spirv;

   simple_mtx_t lock;
   struct set *programs;

   union {
      struct {
         struct zink_shader *generated_tcs; // a generated shader that this shader "owns"; only valid in the tes stage
         struct zink_shader *generated_gs; // a generated shader that this shader "owns"
         bool is_generated; // if this is a driver-created shader (e.g., tcs)
      } non_fs;

      struct {
         nir_variable *fbfetch; //for fs output
      } fs;
   };
};


/** pipeline types */
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

#define zink_pipeline_dynamic_state3 zink_rasterizer_hw_state

struct zink_gfx_pipeline_state {
   /* order matches zink_gfx_output_key */
   unsigned force_persample_interp:1;
   uint32_t rast_samples:6;
   uint32_t min_samples:6;
   uint32_t feedback_loop : 1;
   uint32_t feedback_loop_zs : 1;
   uint32_t rast_attachment_order : 1;
   uint32_t rp_state : 16;
   VkSampleMask sample_mask;
   uint32_t blend_id;

   /* Pre-hashed value for table lookup, invalid when zero.
    * Members after this point are not included in pipeline state hash key */
   uint32_t hash;
   bool dirty;

   struct zink_pipeline_dynamic_state1 dyn_state1;

   struct zink_pipeline_dynamic_state2 dyn_state2;
   struct zink_pipeline_dynamic_state3 dyn_state3;

   union {
      VkShaderModule modules[MESA_SHADER_STAGES - 1];
      uint32_t optimal_key;
   };
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
   enum pipe_prim_type shader_rast_prim, rast_prim; /* reduced type or max for unknown */
   union {
      struct {
         struct zink_shader_key key[5];
         struct zink_shader_key last_vertex;
      } shader_keys;
      struct {
         union zink_shader_key_optimal key;
      } shader_keys_optimal;
   };
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
   uint32_t local_size[3];

   uint32_t module_hash;
   VkShaderModule module;
   bool module_changed;

   struct zink_shader_key key;

   VkPipeline pipeline;
};


/** program types */

/* create_gfx_pushconst must be kept in sync with this struct */
struct zink_gfx_push_constant {
   unsigned draw_mode_is_indexed;
   unsigned draw_id;
   unsigned framebuffer_is_layered;
   float default_inner_level[2];
   float default_outer_level[4];
   uint32_t line_stipple_pattern;
   float viewport_scale[2];
};

/* The order of the enums MUST match the order of the zink_gfx_push_constant
 * members.
 */
enum zink_gfx_push_constant_member {
   ZINK_GFX_PUSHCONST_DRAW_MODE_IS_INDEXED,
   ZINK_GFX_PUSHCONST_DRAW_ID,
   ZINK_GFX_PUSHCONST_FRAMEBUFFER_IS_LAYERED,
   ZINK_GFX_PUSHCONST_DEFAULT_INNER_LEVEL,
   ZINK_GFX_PUSHCONST_DEFAULT_OUTER_LEVEL,
   ZINK_GFX_PUSHCONST_LINE_STIPPLE_PATTERN,
   ZINK_GFX_PUSHCONST_VIEWPORT_SCALE,
   ZINK_GFX_PUSHCONST_MAX
};

/* a shader module is used for directly reusing a shader module between programs,
 * e.g., in the case where we're swapping out only one shader,
 * allowing us to skip going through shader keys
 */
struct zink_shader_module {
   VkShaderModule shader;
   uint32_t hash;
   bool default_variant;
   bool has_nonseamless;
   uint8_t num_uniforms;
   uint8_t key_size;
   uint8_t key[0]; /* | key | uniforms | */
};

struct zink_program {
   struct pipe_reference reference;
   struct zink_context *ctx;
   unsigned char sha1[20];
   struct util_queue_fence cache_fence;
   VkPipelineCache pipeline_cache;
   size_t pipeline_cache_size;
   struct zink_batch_usage *batch_uses;
   bool is_compute;
   bool can_precompile;

   struct zink_program_descriptor_data dd;

   uint32_t compat_id;
   VkPipelineLayout layout;
   VkDescriptorSetLayout dsl[ZINK_DESCRIPTOR_ALL_TYPES]; // one for each type + push + bindless
   unsigned num_dsl;

   bool removed;
};

#define STAGE_MASK_OPTIMAL (1<<16)
typedef bool (*equals_gfx_pipeline_state_func)(const void *a, const void *b);

struct zink_gfx_library_key {
   uint32_t optimal_key; //equals_pipeline_lib_optimal
   VkShaderModule modules[ZINK_GFX_SHADER_COUNT];
   VkPipeline pipeline;
};

struct zink_gfx_input_key {
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
   VkPipeline pipeline;
};

struct zink_gfx_output_key {
   /* order matches zink_gfx_output_key */
   union {
      struct {
         unsigned force_persample_interp:1;
         uint32_t rast_samples:6;
         uint32_t min_samples:6;
         uint32_t feedback_loop : 1;
         uint32_t feedback_loop_zs : 1;
         uint32_t rast_attachment_order : 1;
         uint32_t rp_state : 16;
      };
      uint32_t key;
   };

   /* TODO: compress these */
   VkSampleMask sample_mask;
   uint32_t blend_id;
   VkPipeline pipeline;
};

struct zink_gfx_pipeline_cache_entry {
   struct zink_gfx_pipeline_state state;
   VkPipeline pipeline;
   /* GPL only */
   struct util_queue_fence fence;
   struct zink_gfx_input_key *ikey;
   struct zink_gfx_library_key *gkey;
   struct zink_gfx_output_key *okey;
   struct zink_gfx_program *prog;
   VkPipeline unoptimized_pipeline;
};

struct zink_gfx_program {
   struct zink_program base;

   struct zink_context *ctx; //the owner context

   uint32_t stages_present; //mask of stages present in this program
   uint32_t stages_remaining; //mask of zink_shader remaining in this program
   struct nir_shader *nir[ZINK_GFX_SHADER_COUNT];

   VkShaderModule modules[ZINK_GFX_SHADER_COUNT]; // compute stage doesn't belong here
   uint32_t module_hash[ZINK_GFX_SHADER_COUNT];

   struct zink_shader *last_vertex_stage;

   struct util_dynarray shader_cache[ZINK_GFX_SHADER_COUNT][2][2]; //normal, nonseamless cubes, inline uniforms
   unsigned inlined_variant_count[ZINK_GFX_SHADER_COUNT];

   struct zink_shader *shaders[ZINK_GFX_SHADER_COUNT];
   struct hash_table pipelines[2][11]; // [dynamic, renderpass][number of draw modes we support]
   uint32_t default_variant_hash;
   uint32_t last_variant_hash;
   uint8_t inline_variants; //which stages are using inlined uniforms

   uint32_t last_finalized_hash[2][4]; //[dynamic, renderpass][primtype idx]
   struct zink_gfx_pipeline_cache_entry *last_pipeline[2][4]; //[dynamic, renderpass][primtype idx]

   struct set libs; //zink_gfx_library_key -> VkPipeline
};

struct zink_compute_program {
   struct zink_program base;

   bool use_local_size;

   nir_shader *nir;

   struct zink_shader_module *curr;

   struct zink_shader_module *module; //base
   struct util_dynarray shader_cache[2]; //nonseamless cubes, inline uniforms
   unsigned inlined_variant_count;

   struct zink_shader *shader;
   struct hash_table pipelines;

   VkPipeline base_pipeline;
};


/** renderpass types */

struct zink_rt_attrib {
  VkFormat format;
  VkSampleCountFlagBits samples;
  bool clear_color;
  union {
     bool clear_stencil;
     bool fbfetch;
  };

  bool invalid;

  bool swapchain;

  bool needs_write;
  bool resolve;

  bool mixed_zs;

  bool feedback_loop;
};

struct zink_render_pass_state {
   union {
      struct {
         uint8_t num_cbufs : 5; /* PIPE_MAX_COLOR_BUFS = 8 */
         uint8_t have_zsbuf : 1;
         uint8_t samples:1; //for fs samplemask

         uint8_t swapchain_init:1;

         uint32_t num_zsresolves : 1;
         uint32_t num_cresolves : 24; /* PIPE_MAX_COLOR_BUFS, but this is a struct hole */
      };
      uint32_t val; //for comparison
   };
   struct zink_rt_attrib rts[PIPE_MAX_COLOR_BUFS + 1];
   unsigned num_rts;
   uint32_t clears; //for extra verification and update flagging
   uint32_t msaa_expand_mask;
};

struct zink_pipeline_rt {
   VkFormat format;
   VkSampleCountFlagBits samples;
};

struct zink_render_pass_pipeline_state {
   uint32_t num_attachments:22;
   uint32_t fbfetch:1;
   uint32_t color_read:1;
   uint32_t depth_read:1;
   uint32_t depth_write:1;
   uint32_t num_cresolves:4;
   uint32_t num_zsresolves:1;
   bool samples:1; //for fs samplemask
   struct zink_pipeline_rt attachments[PIPE_MAX_COLOR_BUFS + 1];
   unsigned id;
};

struct zink_render_pass {
   VkRenderPass render_pass;
   struct zink_render_pass_state state;
   unsigned pipeline_state;
};


/** resource types */
struct zink_resource_object {
   struct pipe_reference reference;

   VkPipelineStageFlagBits access_stage;
   VkAccessFlags access;
   bool unordered_read;
   bool unordered_write;

   unsigned persistent_maps; //if nonzero, requires vkFlushMappedMemoryRanges during batch use

   VkBuffer storage_buffer;
   simple_mtx_t view_lock;
   uint32_t view_prune_count; //how many views to prune
   uint32_t view_prune_timeline; //when to prune
   struct util_dynarray views;

   union {
      VkBuffer buffer;
      VkImage image;
   };
   VkDeviceAddress bda;

   VkSampleLocationsInfoEXT zs_evaluate;
   bool needs_zs_evaluate;

   bool storage_init; //layout was set for image
   bool transfer_dst;
   bool render_target;
   bool is_buffer;
   bool exportable;

   /* TODO: this should be a union */
   int handle;
   struct zink_bo *bo;
   // struct {
   struct kopper_displaytarget *dt;
   uint32_t dt_idx;
   uint32_t last_dt_idx;
   VkSemaphore present;
   bool new_dt;
   bool indefinite_acquire;
   // }


   VkDeviceSize offset, size, alignment;
   VkImageCreateFlags vkflags;
   VkImageUsageFlags vkusage;
   VkFormatFeatureFlags vkfeats;
   uint64_t modifier;
   VkImageAspectFlags modifier_aspect;
   VkSamplerYcbcrConversion sampler_conversion;
   unsigned plane_offsets[3];
   unsigned plane_strides[3];
   unsigned plane_count;

   bool host_visible;
   bool coherent;
   bool is_aux;
};

struct zink_resource {
   struct threaded_resource base;

   enum pipe_format internal_format:16;

   struct zink_resource_object *obj;
   union {
      struct {
         struct util_range valid_buffer_range;
         uint32_t vbo_bind_mask : PIPE_MAX_ATTRIBS;
         uint8_t ubo_bind_count[2];
         uint8_t ssbo_bind_count[2];
         uint8_t vbo_bind_count;
         uint8_t so_bind_count; //not counted in all_binds
         bool so_valid;
         uint32_t ubo_bind_mask[MESA_SHADER_STAGES];
         uint32_t ssbo_bind_mask[MESA_SHADER_STAGES];
      };
      struct {
         bool linear;
         bool need_2D;
         bool valid;
         uint8_t fb_bind_count; //not counted in all_binds
         uint16_t fb_binds; /* mask of attachment idx; zs is PIPE_MAX_COLOR_BUFS */
         VkSparseImageMemoryRequirements sparse;
         VkFormat format;
         VkImageLayout layout;
         VkImageAspectFlags aspect;
      };
   };
   uint32_t sampler_binds[MESA_SHADER_STAGES];
   uint32_t image_binds[MESA_SHADER_STAGES];
   uint16_t sampler_bind_count[2]; //gfx, compute
   uint16_t image_bind_count[2]; //gfx, compute
   uint16_t write_bind_count[2]; //gfx, compute
   uint16_t bindless[2]; //tex, img
   union {
      uint16_t bind_count[2]; //gfx, compute
      uint32_t all_binds;
   };

   VkPipelineStageFlagBits gfx_barrier;
   VkAccessFlagBits barrier_access[2]; //gfx, compute

   union {
      struct {
         struct hash_table bufferview_cache;
         simple_mtx_t bufferview_mtx;
      };
      struct {
         struct hash_table surface_cache;
         simple_mtx_t surface_mtx;
      };
   };

   bool swapchain;
   bool dmabuf_acquire;
   bool dmabuf;

   struct sw_displaytarget *dt;

   unsigned dt_stride;

   uint8_t modifiers_count;
   uint64_t *modifiers;
   enum pipe_format drm_format;
};

static inline struct zink_resource *
zink_resource(struct pipe_resource *r)
{
   return (struct zink_resource *)r;
}


struct zink_transfer {
   struct threaded_transfer base;
   struct pipe_resource *staging_res;
   unsigned offset;
   unsigned depthPitch;
};


/** screen types */
struct zink_modifier_prop {
    uint32_t                             drmFormatModifierCount;
    VkDrmFormatModifierPropertiesEXT*    pDrmFormatModifierProperties;
};

struct zink_screen {
   struct pipe_screen base;

   struct util_dl_library *loader_lib;
   PFN_vkGetInstanceProcAddr vk_GetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr vk_GetDeviceProcAddr;

   bool threaded;
   bool is_cpu;
   bool abort_on_hang;
   uint64_t curr_batch; //the current batch id
   uint32_t last_finished;
   VkSemaphore sem;
   VkSemaphore prev_sem;
   VkFence fence;
   struct util_queue flush_queue;
   struct zink_context *copy_context;

   unsigned buffer_rebind_counter;
   unsigned image_rebind_counter;
   unsigned robust_ctx_count;

   struct hash_table dts;
   simple_mtx_t dt_lock;

   bool device_lost;

   struct sw_winsys *winsys;

   int drm_fd;

   struct hash_table framebuffer_cache;

   simple_mtx_t framebuffer_mtx;

   struct slab_parent_pool transfer_pool;
   struct disk_cache *disk_cache;
   struct util_queue cache_put_thread;
   struct util_queue cache_get_thread;

   simple_mtx_t desc_set_layouts_lock;
   struct hash_table desc_set_layouts[ZINK_DESCRIPTOR_BASE_TYPES];
   simple_mtx_t desc_pool_keys_lock;
   struct set desc_pool_keys[ZINK_DESCRIPTOR_BASE_TYPES];
   struct util_live_shader_cache shaders;

   struct {
      struct pb_cache bo_cache;
      struct pb_slabs bo_slabs[NUM_SLAB_ALLOCATORS];
      unsigned min_alloc_size;
      uint32_t next_bo_unique_id;
   } pb;
   uint8_t heap_map[ZINK_HEAP_MAX][VK_MAX_MEMORY_TYPES];
   uint8_t heap_count[ZINK_HEAP_MAX];
   bool resizable_bar;

   uint64_t total_video_mem;
   uint64_t clamp_video_mem;
   uint64_t total_mem;

   VkInstance instance;
   struct zink_instance_info instance_info;

   VkPhysicalDevice pdev;
   uint32_t vk_version, spirv_version;
   struct util_idalloc_mt buffer_ids;
   struct util_vertex_state_cache vertex_state_cache;

   struct zink_device_info info;
   struct nir_shader_compiler_options nir_options;

   bool optimal_keys;
   bool have_full_ds3;
   bool have_X8_D24_UNORM_PACK32;
   bool have_D24_UNORM_S8_UINT;
   bool have_D32_SFLOAT_S8_UINT;
   bool have_triangle_fans;
   bool need_decompose_attrs;
   bool need_2D_zs;
   bool need_2D_sparse;
   bool faked_e5sparse; //drivers may not expose R9G9B9E5 but cts requires it

   uint32_t gfx_queue;
   uint32_t sparse_queue;
   uint32_t max_queues;
   uint32_t timestamp_valid_bits;

   unsigned max_fences;

   VkDevice dev;
   VkQueue queue; //gfx+compute
   VkQueue queue_sparse;
   simple_mtx_t queue_lock;
   VkDebugUtilsMessengerEXT debugUtilsCallbackHandle;

   uint32_t cur_custom_border_color_samplers;

   struct vk_dispatch_table vk;

   void (*buffer_barrier)(struct zink_context *ctx, struct zink_resource *res, VkAccessFlags flags, VkPipelineStageFlags pipeline);
   void (*image_barrier)(struct zink_context *ctx, struct zink_resource *res, VkImageLayout new_layout, VkAccessFlags flags, VkPipelineStageFlags pipeline);

   bool compact_descriptors; /**< toggled if descriptor set ids are compacted */
   uint8_t desc_set_id[ZINK_MAX_DESCRIPTOR_SETS]; /**< converts enum zink_descriptor_type -> the actual set id */

   struct {
      bool dual_color_blend_by_location;
      bool glsl_correct_derivatives_after_discard;
      bool inline_uniforms;
   } driconf;

   VkFormatProperties format_props[PIPE_FORMAT_COUNT];
   struct zink_modifier_prop modifier_props[PIPE_FORMAT_COUNT];

   VkExtent2D maxSampleLocationGridSize[5];
   VkPipelineLayout gfx_push_constant_layout;

   struct {
      bool broken_l4a4;
      bool depth_clip_control_missing;
      bool implicit_sync;
      bool always_feedback_loop;
      bool always_feedback_loop_zs;
      bool needs_sanitised_layer;
      bool track_renderpasses;
      bool no_linestipple;
      unsigned z16_unscaled_bias;
      unsigned z24_unscaled_bias;
   } driver_workarounds;
};

static inline struct zink_screen *
zink_screen(struct pipe_screen *pipe)
{
   return (struct zink_screen *)pipe;
}

/** surface types */

/* info for validating/creating imageless framebuffers */
struct zink_surface_info {
   VkImageCreateFlags flags;
   VkImageUsageFlags usage;
   uint32_t width;
   uint32_t height;
   uint32_t layerCount;
   VkFormat format[2]; //base format, srgb format (for srgb framebuffer)
};

/* an imageview for a zink_resource:
   - may be a fb attachment, samplerview, or shader image
   - cached on the parent zink_resource_object
   - also handles swapchains
 */
struct zink_surface {
   struct pipe_surface base;
   /* all the info for creating a new imageview */
   VkImageViewCreateInfo ivci;
   VkImageViewUsageCreateInfo usage_info;
   /* for framebuffer use */
   struct zink_surface_info info;
   bool is_swapchain;
   /* the current imageview */
   VkImageView image_view;
   /* array of imageviews for swapchains, one for each image */
   VkImageView *swapchain;
   unsigned swapchain_size;

   VkImageView *old_swapchain;
   unsigned old_swapchain_size;
   VkImageView simage_view;//old iview after storage replacement/rebind
   void *obj; //backing resource object; used to determine rebinds
   void *dt; //current swapchain object; used to determine swapchain rebinds
   uint32_t hash; //for surface caching

   struct zink_batch_usage *batch_uses;

   struct util_dynarray framebuffer_refs;
   struct zink_descriptor_refs desc_set_refs;
};

/* wrapper object that preserves the gallium expectation of having
 * pipe_surface::context match the context used to create the surface
 */
struct zink_ctx_surface {
   struct pipe_surface base;
   struct zink_surface *surf; //the actual surface
   /* TODO: use VK_EXT_multisampled_render_to_single_sampled */
   struct zink_ctx_surface *transient; //for use with EXT_multisample_render_to_texture
   bool transient_init; //whether the transient surface has data
};

/* use this cast for framebuffer surfaces */
static inline struct zink_surface *
zink_csurface(struct pipe_surface *psurface)
{
   return psurface ? ((struct zink_ctx_surface *)psurface)->surf : NULL;
}

/* use this cast for checking transient framebuffer surfaces */
static inline struct zink_surface *
zink_transient_surface(struct pipe_surface *psurface)
{
   return psurface ? ((struct zink_ctx_surface *)psurface)->transient ? ((struct zink_ctx_surface *)psurface)->transient->surf : NULL : NULL;
}

/* use this cast for internal surfaces */
static inline struct zink_surface *
zink_surface(struct pipe_surface *psurface)
{
   return (struct zink_surface *)psurface;
}


/** framebuffer types */
struct zink_framebuffer_state {
   uint32_t width;
   uint16_t height;
   uint32_t layers:6;
   uint32_t samples:6;
   uint32_t num_attachments:4;
   //struct zink_surface_info infos[PIPE_MAX_COLOR_BUFS + 1];

   union {
      VkImageView attachments[PIPE_MAX_COLOR_BUFS + 1];
      struct zink_surface_info infos[PIPE_MAX_COLOR_BUFS + 1];
   };
};

struct zink_framebuffer {
   struct pipe_reference reference;

   /* current objects */
   VkFramebuffer fb;
   struct zink_render_pass *rp;

   struct zink_framebuffer_state state;
   //VkFramebufferAttachmentImageInfo infos[PIPE_MAX_COLOR_BUFS + 1];

   union {
      struct pipe_surface *surfaces[PIPE_MAX_COLOR_BUFS + 1];
      VkFramebufferAttachmentImageInfo infos[PIPE_MAX_COLOR_BUFS + 1];
   };

   struct hash_table objects;
};


/** context types */
struct zink_sampler_state {
   VkSampler sampler;
   VkSampler sampler_clamped;
   bool custom_border_color;
   bool emulate_nonseamless;
};

struct zink_buffer_view {
   struct pipe_reference reference;
   struct pipe_resource *pres;
   VkBufferViewCreateInfo bvci;
   VkBufferView buffer_view;
   uint32_t hash;
};

struct zink_sampler_view {
   struct pipe_sampler_view base;
   union {
      struct zink_surface *image_view;
      struct zink_buffer_view *buffer_view;
   };
   struct zink_surface *cube_array;
};

struct zink_image_view {
   struct pipe_image_view base;
   union {
      struct zink_surface *surface;
      struct zink_buffer_view *buffer_view;
   };
};

static inline struct zink_sampler_view *
zink_sampler_view(struct pipe_sampler_view *pview)
{
   return (struct zink_sampler_view *)pview;
}

struct zink_so_target {
   struct pipe_stream_output_target base;
   struct pipe_resource *counter_buffer;
   VkDeviceSize counter_buffer_offset;
   uint32_t stride;
   bool counter_buffer_valid;
};

static inline struct zink_so_target *
zink_so_target(struct pipe_stream_output_target *so_target)
{
   return (struct zink_so_target *)so_target;
}

struct zink_viewport_state {
   struct pipe_viewport_state viewport_states[PIPE_MAX_VIEWPORTS];
   struct pipe_scissor_state scissor_states[PIPE_MAX_VIEWPORTS];
   uint8_t num_viewports;
};


struct zink_descriptor_surface {
   union {
      struct zink_surface *surface;
      struct zink_buffer_view *bufferview;
   };
   bool is_buffer;
};

struct zink_bindless_descriptor {
   struct zink_descriptor_surface ds;
   struct zink_sampler_state *sampler;
   uint32_t handle;
   uint32_t access; //PIPE_ACCESS_...
};

struct zink_rendering_info {
   VkPipelineRenderingCreateInfo info;
   unsigned id;
};


typedef void (*pipe_draw_vbo_func)(struct pipe_context *pipe,
                                   const struct pipe_draw_info *info,
                                   unsigned drawid_offset,
                                   const struct pipe_draw_indirect_info *indirect,
                                   const struct pipe_draw_start_count_bias *draws,
                                   unsigned num_draws);
typedef void (*pipe_draw_vertex_state_func)(struct pipe_context *ctx,
                                            struct pipe_vertex_state *vstate,
                                            uint32_t partial_velem_mask,
                                            struct pipe_draw_vertex_state_info info,
                                            const struct pipe_draw_start_count_bias *draws,
                                            unsigned num_draws);
typedef void (*pipe_launch_grid_func)(struct pipe_context *pipe, const struct pipe_grid_info *info);

struct zink_context {
   struct pipe_context base;
   struct threaded_context *tc;
   struct slab_child_pool transfer_pool;
   struct slab_child_pool transfer_pool_unsync;
   struct blitter_context *blitter;

   unsigned flags;

   pipe_draw_vbo_func draw_vbo[2]; //batch changed
   pipe_draw_vertex_state_func draw_state[2]; //batch changed
   pipe_launch_grid_func launch_grid[2]; //batch changed

   struct pipe_device_reset_callback reset;

   simple_mtx_t batch_mtx;

   struct zink_fence *deferred_fence;
   struct zink_fence *last_fence; //the last command buffer submitted
   struct zink_batch_state *batch_states; //list of submitted batch states: ordered by increasing timeline id
   unsigned batch_states_count; //number of states in `batch_states`
   struct zink_batch_state *free_batch_states; //unused batch states
   struct zink_batch_state *last_free_batch_state; //for appending
   bool oom_flush;
   bool oom_stall;
   struct zink_batch batch;

   unsigned shader_has_inlinable_uniforms_mask;
   unsigned inlinable_uniforms_valid_mask;

   struct pipe_constant_buffer ubos[MESA_SHADER_STAGES][PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_shader_buffer ssbos[MESA_SHADER_STAGES][PIPE_MAX_SHADER_BUFFERS];
   uint32_t writable_ssbos[MESA_SHADER_STAGES];
   struct zink_image_view image_views[MESA_SHADER_STAGES][ZINK_MAX_SHADER_IMAGES];

   uint32_t transient_attachments;
   struct pipe_framebuffer_state fb_state;

   struct zink_framebuffer *(*get_framebuffer)(struct zink_context*);
   void (*init_framebuffer)(struct zink_screen *screen, struct zink_framebuffer *fb, struct zink_render_pass *rp);

   struct hash_table framebuffer_cache;

   struct zink_vertex_elements_state *element_state;
   struct zink_rasterizer_state *rast_state;
   struct zink_depth_stencil_alpha_state *dsa_state;

   bool pipeline_changed[2]; //gfx, compute

   struct zink_shader *gfx_stages[ZINK_GFX_SHADER_COUNT];
   struct zink_shader *last_vertex_stage;
   bool shader_reads_drawid;
   bool shader_reads_basevertex;
   struct zink_gfx_pipeline_state gfx_pipeline_state;
   /* there are 5 gfx stages, but VS and FS are assumed to be always present,
    * thus only 3 stages need to be considered, giving 2^3 = 8 program caches.
    */
   struct hash_table program_cache[8];
   simple_mtx_t program_lock[8];
   uint32_t gfx_hash;
   struct zink_gfx_program *curr_program;
   struct set gfx_inputs;
   struct set gfx_outputs;

   struct zink_descriptor_data dd;

   struct zink_compute_pipeline_state compute_pipeline_state;
   struct zink_compute_program *curr_compute;

   unsigned shader_stages : ZINK_GFX_SHADER_COUNT; /* mask of bound gfx shader stages */
   uint8_t dirty_gfx_stages; /* mask of changed gfx shader stages */
   bool last_vertex_stage_dirty;
   bool compute_dirty;

   struct {
      VkRenderingAttachmentInfo attachments[PIPE_MAX_COLOR_BUFS + 2]; //+depth, +stencil
      VkRenderingInfo info;
      struct tc_renderpass_info tc_info;
   } dynamic_fb;
   uint32_t fb_layer_mismatch; //bitmask
   unsigned depth_bias_scale_factor;
   struct set rendering_state_cache;
   struct set render_pass_state_cache;
   struct hash_table *render_pass_cache;
   VkExtent2D swapchain_size;
   bool fb_changed;
   bool rp_changed; //force renderpass restart
   bool rp_layout_changed; //renderpass changed, maybe restart
   bool rp_loadop_changed; //renderpass changed, don't restart

   struct zink_framebuffer *framebuffer;
   struct zink_framebuffer_clear fb_clears[PIPE_MAX_COLOR_BUFS + 1];
   uint16_t clears_enabled;
   uint16_t rp_clears_enabled;
   uint16_t void_clears;
   uint16_t fbfetch_outputs;
   uint16_t feedback_loops;
   struct zink_resource *needs_present;

   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   bool vertex_buffers_dirty;

   struct zink_sampler_state *sampler_states[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *sampler_views[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];

   struct zink_viewport_state vp_state;
   bool vp_state_changed;
   bool scissor_changed;

   float blend_constants[4];

   bool sample_locations_changed;
   VkSampleLocationEXT vk_sample_locations[PIPE_MAX_SAMPLE_LOCATION_GRID_SIZE * PIPE_MAX_SAMPLE_LOCATION_GRID_SIZE];
   uint8_t sample_locations[2 * 4 * 8 * 16];

   struct pipe_stencil_ref stencil_ref;

   union {
      struct {
         float default_inner_level[2];
         float default_outer_level[4];
      };
      float tess_levels[6];
   };

   struct zink_vk_query *curr_xfb_queries[PIPE_MAX_VERTEX_STREAMS];

   struct list_head query_pools;
   struct list_head suspended_queries;
   struct list_head primitives_generated_queries;
   struct zink_query *vertices_query;
   bool disable_color_writes;
   bool primitives_generated_active;
   bool queries_disabled, render_condition_active;
   struct {
      struct zink_query *query;
      bool inverted;
      bool active; //this is the internal vk state
   } render_condition;

   struct pipe_resource *dummy_vertex_buffer;
   struct pipe_resource *dummy_xfb_buffer;
   struct pipe_surface *dummy_surface[7];
   struct zink_buffer_view *dummy_bufferview;

   unsigned buffer_rebind_counter;
   unsigned image_rebind_counter;

   struct {
      /* descriptor info */
      VkDescriptorBufferInfo ubos[MESA_SHADER_STAGES][PIPE_MAX_CONSTANT_BUFFERS];
      uint32_t push_valid;
      uint8_t num_ubos[MESA_SHADER_STAGES];

      VkDescriptorBufferInfo ssbos[MESA_SHADER_STAGES][PIPE_MAX_SHADER_BUFFERS];
      uint8_t num_ssbos[MESA_SHADER_STAGES];

      VkDescriptorImageInfo textures[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
      VkBufferView tbos[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
      uint32_t emulate_nonseamless[MESA_SHADER_STAGES];
      uint32_t cubes[MESA_SHADER_STAGES];
      uint8_t num_samplers[MESA_SHADER_STAGES];
      uint8_t num_sampler_views[MESA_SHADER_STAGES];

      VkDescriptorImageInfo images[MESA_SHADER_STAGES][ZINK_MAX_SHADER_IMAGES];
      VkBufferView texel_images[MESA_SHADER_STAGES][ZINK_MAX_SHADER_IMAGES];
      uint8_t num_images[MESA_SHADER_STAGES];

      VkDescriptorImageInfo fbfetch;

      struct zink_resource *descriptor_res[ZINK_DESCRIPTOR_BASE_TYPES][MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];

      struct {
         struct util_idalloc tex_slots;
         struct util_idalloc img_slots;
         struct hash_table tex_handles;
         struct hash_table img_handles;
         VkBufferView *buffer_infos; //tex, img
         VkDescriptorImageInfo *img_infos; //tex, img
         struct util_dynarray updates;
         struct util_dynarray resident;
      } bindless[2];  //img, buffer
      union {
         bool bindless_dirty[2]; //tex, img
         uint16_t any_bindless_dirty;
      };
      bool bindless_refs_dirty;
   } di;
   struct set *need_barriers[2]; //gfx, compute
   struct set update_barriers[2][2]; //[gfx, compute][current, next]
   uint8_t barrier_set_idx[2];
   unsigned memory_barrier;

   uint32_t num_so_targets;
   struct pipe_stream_output_target *so_targets[PIPE_MAX_SO_OUTPUTS];
   bool dirty_so_targets;

   bool first_frame_done;
   bool have_timelines;

   bool gfx_dirty;

   bool is_device_lost;
   bool primitive_restart;
   bool blitting : 1;
   bool vertex_state_changed : 1;
   bool blend_state_changed : 1;
   bool sample_mask_changed : 1;
   bool rast_state_changed : 1;
   bool dsa_state_changed : 1;
   bool stencil_ref_changed : 1;
   bool rasterizer_discard_changed : 1;
   bool rp_tc_info_updated : 1;
};

static inline struct zink_context *
zink_context(struct pipe_context *context)
{
   return (struct zink_context *)context;
}

#endif
