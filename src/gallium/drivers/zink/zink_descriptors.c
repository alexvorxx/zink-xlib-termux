/*
 * Copyright © 2020 Mike Blumenkrantz
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

#include "tgsi/tgsi_from_mesa.h"



#include "zink_context.h"
#include "zink_descriptors.h"
#include "zink_program.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"


struct zink_descriptor_pool {
   struct pipe_reference reference;
   enum zink_descriptor_type type;
   struct hash_table *desc_sets;
   struct hash_table *free_desc_sets;
   struct util_dynarray alloc_desc_sets;
   const struct zink_descriptor_pool_key *key;
   VkDescriptorPool descpool;
   unsigned num_resources;
   unsigned num_sets_allocated;
   simple_mtx_t mtx;
};

struct zink_descriptor_set {
   struct zink_descriptor_pool *pool;
   struct pipe_reference reference; //incremented for batch usage
   VkDescriptorSet desc_set;
   uint32_t hash;
   bool invalid;
   bool punted;
   bool recycled;
   uint8_t compacted; //bitmask of zink_descriptor_type
   struct zink_descriptor_state_key key;
   struct zink_batch_usage *batch_uses;
#ifndef NDEBUG
   /* for extra debug asserts */
   unsigned num_resources;
#endif
   union {
      struct zink_resource_object **res_objs;
      struct {
         struct zink_descriptor_surface *surfaces;
         struct zink_sampler_state **sampler_states;
      };
   };
};

union zink_program_descriptor_refs {
   struct zink_resource **res;
   struct zink_descriptor_surface *dsurf;
   struct {
      struct zink_descriptor_surface *dsurf;
      struct zink_sampler_state **sampler_state;
   } sampler;
};

struct zink_program_descriptor_data_cached {
   struct zink_program_descriptor_data base;
   struct zink_descriptor_pool *pool[ZINK_DESCRIPTOR_TYPES];
   struct zink_descriptor_set *last_set[ZINK_DESCRIPTOR_TYPES];
   unsigned num_refs[ZINK_DESCRIPTOR_TYPES];
   union zink_program_descriptor_refs *refs[ZINK_DESCRIPTOR_TYPES];
   unsigned cache_misses[ZINK_DESCRIPTOR_TYPES];
};


static inline struct zink_program_descriptor_data_cached *
pdd_cached(struct zink_program *pg)
{
   return (struct zink_program_descriptor_data_cached*)pg->dd;
}

static bool
batch_add_desc_set(struct zink_batch *batch, struct zink_descriptor_set *zds)
{
   if (zink_batch_usage_matches(zds->batch_uses, batch->state) ||
       !batch_ptr_add_usage(batch, batch->state->dd->desc_sets, zds))
      return false;
   pipe_reference(NULL, &zds->reference);
   pipe_reference(NULL, &zds->pool->reference);
   zink_batch_usage_set(&zds->batch_uses, batch->state);
   return true;
}

static void
debug_describe_zink_descriptor_pool(char *buf, const struct zink_descriptor_pool *ptr)
{
   sprintf(buf, "zink_descriptor_pool");
}

static inline uint32_t
get_sampler_view_hash(const struct zink_sampler_view *sampler_view)
{
   if (!sampler_view)
      return 0;
   return sampler_view->base.target == PIPE_BUFFER ?
          sampler_view->buffer_view->hash : sampler_view->image_view->hash;
}

static inline uint32_t
get_image_view_hash(const struct zink_image_view *image_view)
{
   if (!image_view || !image_view->base.resource)
      return 0;
   return image_view->base.resource->target == PIPE_BUFFER ?
          image_view->buffer_view->hash : image_view->surface->hash;
}

uint32_t
zink_get_sampler_view_hash(struct zink_context *ctx, struct zink_sampler_view *sampler_view, bool is_buffer)
{
   return get_sampler_view_hash(sampler_view) ? get_sampler_view_hash(sampler_view) :
          (is_buffer ? zink_screen(ctx->base.screen)->null_descriptor_hashes.buffer_view :
                       zink_screen(ctx->base.screen)->null_descriptor_hashes.image_view);
}

uint32_t
zink_get_image_view_hash(struct zink_context *ctx, struct zink_image_view *image_view, bool is_buffer)
{
   return get_image_view_hash(image_view) ? get_image_view_hash(image_view) :
          (is_buffer ? zink_screen(ctx->base.screen)->null_descriptor_hashes.buffer_view :
                       zink_screen(ctx->base.screen)->null_descriptor_hashes.image_view);
}

#ifndef NDEBUG
static uint32_t
get_descriptor_surface_hash(struct zink_context *ctx, struct zink_descriptor_surface *dsurf)
{
   return dsurf->is_buffer ? (dsurf->bufferview ? dsurf->bufferview->hash : zink_screen(ctx->base.screen)->null_descriptor_hashes.buffer_view) :
                             (dsurf->surface ? dsurf->surface->hash : zink_screen(ctx->base.screen)->null_descriptor_hashes.image_view);
}
#endif

static bool
desc_state_equal(const void *a, const void *b)
{
   const struct zink_descriptor_state_key *a_k = (void*)a;
   const struct zink_descriptor_state_key *b_k = (void*)b;

   for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
      if (a_k->exists[i] != b_k->exists[i])
         return false;
      if (a_k->exists[i] && b_k->exists[i] &&
          a_k->state[i] != b_k->state[i])
         return false;
   }
   return true;
}

static uint32_t
desc_state_hash(const void *key)
{
   const struct zink_descriptor_state_key *d_key = (void*)key;
   uint32_t hash = 0;
   bool first = true;
   for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
      if (d_key->exists[i]) {
         if (!first)
            hash ^= d_key->state[i];
         else
            hash = d_key->state[i];
         first = false;
      }
   }
   return hash;
}

static void
pop_desc_set_ref(struct zink_descriptor_set *zds, struct util_dynarray *refs)
{
   size_t size = sizeof(struct zink_descriptor_reference);
   unsigned num_elements = refs->size / size;
   for (unsigned i = 0; i < num_elements; i++) {
      struct zink_descriptor_reference *ref = util_dynarray_element(refs, struct zink_descriptor_reference, i);
      if (&zds->invalid == ref->invalid) {
         memcpy(util_dynarray_element(refs, struct zink_descriptor_reference, i),
                util_dynarray_pop_ptr(refs, struct zink_descriptor_reference), size);
         break;
      }
   }
}

static void
descriptor_set_invalidate(struct zink_descriptor_set *zds)
{
   zds->invalid = true;
   unsigned idx = 0;
   for (unsigned i = 0; i < zds->pool->key->layout->num_bindings; i++) {
      for (unsigned j = 0; j < zds->pool->key->layout->bindings[i].descriptorCount; j++) {
         switch (zds->pool->type) {
         case ZINK_DESCRIPTOR_TYPE_UBO:
         case ZINK_DESCRIPTOR_TYPE_SSBO:
            if (zds->res_objs[idx])
               pop_desc_set_ref(zds, &zds->res_objs[idx]->desc_set_refs.refs);
            zds->res_objs[idx] = NULL;
            break;
         case ZINK_DESCRIPTOR_TYPE_IMAGE:
            if (zds->surfaces[idx].is_buffer) {
               if (zds->surfaces[idx].bufferview)
                  pop_desc_set_ref(zds, &zds->surfaces[idx].bufferview->desc_set_refs.refs);
               zds->surfaces[idx].bufferview = NULL;
            } else {
               if (zds->surfaces[idx].surface)
                  pop_desc_set_ref(zds, &zds->surfaces[idx].surface->desc_set_refs.refs);
               zds->surfaces[idx].surface = NULL;
            }
            break;
         case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
            if (zds->surfaces[idx].is_buffer) {
               if (zds->surfaces[idx].bufferview)
                  pop_desc_set_ref(zds, &zds->surfaces[idx].bufferview->desc_set_refs.refs);
               zds->surfaces[idx].bufferview = NULL;
            } else {
               if (zds->surfaces[idx].surface)
                  pop_desc_set_ref(zds, &zds->surfaces[idx].surface->desc_set_refs.refs);
               zds->surfaces[idx].surface = NULL;
            }
            if (zds->sampler_states[idx])
               pop_desc_set_ref(zds, &zds->sampler_states[idx]->desc_set_refs.refs);
            zds->sampler_states[idx] = NULL;
            break;
         default:
            break;
         }
         idx++;
      }
   }
}

static void
descriptor_pool_clear(struct hash_table *ht)
{
   hash_table_foreach(ht, entry) {
      struct zink_descriptor_set *zds = entry->data;
      descriptor_set_invalidate(zds);
   }
}

static void
descriptor_pool_free(struct zink_screen *screen, struct zink_descriptor_pool *pool)
{
   if (!pool)
      return;
   if (pool->descpool)
      VKSCR(DestroyDescriptorPool)(screen->dev, pool->descpool, NULL);

   simple_mtx_lock(&pool->mtx);
   if (pool->desc_sets)
      descriptor_pool_clear(pool->desc_sets);
   if (pool->free_desc_sets)
      descriptor_pool_clear(pool->free_desc_sets);
   if (pool->desc_sets)
      _mesa_hash_table_destroy(pool->desc_sets, NULL);
   if (pool->free_desc_sets)
      _mesa_hash_table_destroy(pool->free_desc_sets, NULL);

   simple_mtx_unlock(&pool->mtx);
   util_dynarray_fini(&pool->alloc_desc_sets);
   simple_mtx_destroy(&pool->mtx);
   ralloc_free(pool);
}

static void
descriptor_pool_delete(struct zink_context *ctx, struct zink_descriptor_pool *pool)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (!pool)
      return;
   _mesa_hash_table_remove_key(ctx->dd->descriptor_pools[pool->type], pool->key);
   descriptor_pool_free(screen, pool);
}

static struct zink_descriptor_pool *
descriptor_pool_create(struct zink_screen *screen, enum zink_descriptor_type type,
                       const struct zink_descriptor_pool_key *pool_key)
{
   struct zink_descriptor_pool *pool = rzalloc(NULL, struct zink_descriptor_pool);
   if (!pool)
      return NULL;
   pipe_reference_init(&pool->reference, 1);
   pool->type = type;
   pool->key = pool_key;
   simple_mtx_init(&pool->mtx, mtx_plain);
   for (unsigned i = 0; i < pool_key->layout->num_bindings; i++) {
       pool->num_resources += pool_key->layout->bindings[i].descriptorCount;
   }
   pool->desc_sets = _mesa_hash_table_create(NULL, desc_state_hash, desc_state_equal);
   if (!pool->desc_sets)
      goto fail;

   pool->free_desc_sets = _mesa_hash_table_create(NULL, desc_state_hash, desc_state_equal);
   if (!pool->free_desc_sets)
      goto fail;

   util_dynarray_init(&pool->alloc_desc_sets, NULL);

   VkDescriptorPoolCreateInfo dpci = {0};
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = pool_key->sizes;
   dpci.poolSizeCount = pool_key->num_type_sizes;
   dpci.flags = 0;
   dpci.maxSets = ZINK_DEFAULT_MAX_DESCS;
   assert(pool_key->num_type_sizes);
   VkResult result = VKSCR(CreateDescriptorPool)(screen->dev, &dpci, 0, &pool->descpool);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateDescriptorPool failed (%s)", vk_Result_to_str(result));
      goto fail;
   }

   return pool;
fail:
   descriptor_pool_free(screen, pool);
   return NULL;
}

static VkDescriptorSetLayout
descriptor_layout_create(struct zink_screen *screen, enum zink_descriptor_type t, VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings)
{
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutCreateInfo dcslci = {0};
   dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   dcslci.pNext = NULL;
   VkDescriptorSetLayoutBindingFlagsCreateInfo fci = {0};
   VkDescriptorBindingFlags flags[ZINK_MAX_DESCRIPTORS_PER_TYPE];
   if (zink_descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY) {
      dcslci.pNext = &fci;
      if (t == ZINK_DESCRIPTOR_TYPES)
         dcslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      fci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      fci.bindingCount = num_bindings;
      fci.pBindingFlags = flags;
      for (unsigned i = 0; i < num_bindings; i++) {
         flags[i] = 0;
      }
   }
   dcslci.bindingCount = num_bindings;
   dcslci.pBindings = bindings;
   VkDescriptorSetLayoutSupport supp;
   supp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
   supp.pNext = NULL;
   supp.supported = VK_FALSE;
   if (VKSCR(GetDescriptorSetLayoutSupport)) {
      VKSCR(GetDescriptorSetLayoutSupport)(screen->dev, &dcslci, &supp);
      if (supp.supported == VK_FALSE) {
         debug_printf("vkGetDescriptorSetLayoutSupport claims layout is unsupported\n");
         return VK_NULL_HANDLE;
      }
   }
   VkResult result = VKSCR(CreateDescriptorSetLayout)(screen->dev, &dcslci, 0, &dsl);
   if (result != VK_SUCCESS)
      mesa_loge("ZINK: vkCreateDescriptorSetLayout failed (%s)", vk_Result_to_str(result));
   return dsl;
}

static uint32_t
hash_descriptor_layout(const void *key)
{
   uint32_t hash = 0;
   const struct zink_descriptor_layout_key *k = key;
   hash = XXH32(&k->num_bindings, sizeof(unsigned), hash);
   /* only hash first 3 members: no holes and the rest are always constant */
   for (unsigned i = 0; i < k->num_bindings; i++)
      hash = XXH32(&k->bindings[i], offsetof(VkDescriptorSetLayoutBinding, stageFlags), hash);

   return hash;
}

static bool
equals_descriptor_layout(const void *a, const void *b)
{
   const struct zink_descriptor_layout_key *a_k = a;
   const struct zink_descriptor_layout_key *b_k = b;
   return a_k->num_bindings == b_k->num_bindings &&
          !memcmp(a_k->bindings, b_k->bindings, a_k->num_bindings * sizeof(VkDescriptorSetLayoutBinding));
}

static struct zink_descriptor_layout *
create_layout(struct zink_context *ctx, enum zink_descriptor_type type,
              VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings,
              struct zink_descriptor_layout_key **layout_key)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayout dsl = descriptor_layout_create(screen, type, bindings, num_bindings);
   if (!dsl)
      return NULL;

   struct zink_descriptor_layout_key *k = ralloc(ctx, struct zink_descriptor_layout_key);
   k->num_bindings = num_bindings;
   if (num_bindings) {
      size_t bindings_size = num_bindings * sizeof(VkDescriptorSetLayoutBinding);
      k->bindings = ralloc_size(k, bindings_size);
      if (!k->bindings) {
         ralloc_free(k);
         VKSCR(DestroyDescriptorSetLayout)(screen->dev, dsl, NULL);
         return NULL;
      }
      memcpy(k->bindings, bindings, bindings_size);
   }

   struct zink_descriptor_layout *layout = rzalloc(ctx, struct zink_descriptor_layout);
   layout->layout = dsl;
   *layout_key = k;
   return layout;
}

struct zink_descriptor_layout *
zink_descriptor_util_layout_get(struct zink_context *ctx, enum zink_descriptor_type type,
                      VkDescriptorSetLayoutBinding *bindings, unsigned num_bindings,
                      struct zink_descriptor_layout_key **layout_key)
{
   uint32_t hash = 0;
   struct zink_descriptor_layout_key key = {
      .num_bindings = num_bindings,
      .bindings = bindings,
   };

   if (type != ZINK_DESCRIPTOR_TYPES) {
      hash = hash_descriptor_layout(&key);
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(&ctx->desc_set_layouts[type], hash, &key);
      if (he) {
         *layout_key = (void*)he->key;
         return he->data;
      }
   }

   struct zink_descriptor_layout *layout = create_layout(ctx, type, bindings, num_bindings, layout_key);
   if (layout && type != ZINK_DESCRIPTOR_TYPES) {
      _mesa_hash_table_insert_pre_hashed(&ctx->desc_set_layouts[type], hash, *layout_key, layout);
   }
   return layout;
}


static uint32_t
hash_descriptor_pool_key(const void *key)
{
   uint32_t hash = 0;
   const struct zink_descriptor_pool_key *k = key;
   hash = XXH32(&k->layout, sizeof(void*), hash);
   for (unsigned i = 0; i < k->num_type_sizes; i++)
      hash = XXH32(&k->sizes[i], sizeof(VkDescriptorPoolSize), hash);

   return hash;
}

static bool
equals_descriptor_pool_key(const void *a, const void *b)
{
   const struct zink_descriptor_pool_key *a_k = a;
   const struct zink_descriptor_pool_key *b_k = b;
   const unsigned a_num_type_sizes = a_k->num_type_sizes;
   const unsigned b_num_type_sizes = b_k->num_type_sizes;
   return a_k->layout == b_k->layout &&
          a_num_type_sizes == b_num_type_sizes &&
          !memcmp(a_k->sizes, b_k->sizes, b_num_type_sizes * sizeof(VkDescriptorPoolSize));
}

struct zink_descriptor_pool_key *
zink_descriptor_util_pool_key_get(struct zink_context *ctx, enum zink_descriptor_type type,
                                  struct zink_descriptor_layout_key *layout_key,
                                  VkDescriptorPoolSize *sizes, unsigned num_type_sizes)
{
   uint32_t hash = 0;
   struct zink_descriptor_pool_key key;
   key.num_type_sizes = num_type_sizes;
   if (type != ZINK_DESCRIPTOR_TYPES) {
      key.layout = layout_key;
      memcpy(key.sizes, sizes, num_type_sizes * sizeof(VkDescriptorPoolSize));
      hash = hash_descriptor_pool_key(&key);
      struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->desc_pool_keys[type], hash, &key);
      if (he)
         return (void*)he->key;
   }

   struct zink_descriptor_pool_key *pool_key = rzalloc(ctx, struct zink_descriptor_pool_key);
   pool_key->layout = layout_key;
   pool_key->num_type_sizes = num_type_sizes;
   assert(pool_key->num_type_sizes);
   memcpy(pool_key->sizes, sizes, num_type_sizes * sizeof(VkDescriptorPoolSize));
   if (type != ZINK_DESCRIPTOR_TYPES)
      _mesa_set_add_pre_hashed(&ctx->desc_pool_keys[type], hash, pool_key);
   return pool_key;
}

static void
init_push_binding(VkDescriptorSetLayoutBinding *binding, unsigned i, VkDescriptorType type)
{
   binding->binding = tgsi_processor_to_shader_stage(i);
   binding->descriptorType = type;
   binding->descriptorCount = 1;
   binding->stageFlags = zink_shader_stage(i);
   binding->pImmutableSamplers = NULL;
}

static VkDescriptorType
get_push_types(struct zink_screen *screen, enum zink_descriptor_type *dsl_type)
{
   *dsl_type = zink_descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY &&
               screen->info.have_KHR_push_descriptor ? ZINK_DESCRIPTOR_TYPES : ZINK_DESCRIPTOR_TYPE_UBO;
   return zink_descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY ?
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

static struct zink_descriptor_layout *
create_gfx_layout(struct zink_context *ctx, struct zink_descriptor_layout_key **layout_key, bool fbfetch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[PIPE_SHADER_TYPES];
   enum zink_descriptor_type dsl_type;
   VkDescriptorType vktype = get_push_types(screen, &dsl_type);
   for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++)
      init_push_binding(&bindings[i], i, vktype);
   if (fbfetch) {
      bindings[ZINK_SHADER_COUNT].binding = ZINK_FBFETCH_BINDING;
      bindings[ZINK_SHADER_COUNT].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      bindings[ZINK_SHADER_COUNT].descriptorCount = 1;
      bindings[ZINK_SHADER_COUNT].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      bindings[ZINK_SHADER_COUNT].pImmutableSamplers = NULL;
   }
   return create_layout(ctx, dsl_type, bindings, fbfetch ? ARRAY_SIZE(bindings) : ARRAY_SIZE(bindings) - 1, layout_key);
}

bool
zink_descriptor_util_push_layouts_get(struct zink_context *ctx, struct zink_descriptor_layout **dsls, struct zink_descriptor_layout_key **layout_keys)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding compute_binding;
   enum zink_descriptor_type dsl_type;
   VkDescriptorType vktype = get_push_types(screen, &dsl_type);
   init_push_binding(&compute_binding, PIPE_SHADER_COMPUTE, vktype);
   dsls[0] = create_gfx_layout(ctx, &layout_keys[0], false);
   dsls[1] = create_layout(ctx, dsl_type, &compute_binding, 1, &layout_keys[1]);
   return dsls[0] && dsls[1];
}

VkImageLayout
zink_descriptor_util_image_layout_eval(const struct zink_context *ctx, const struct zink_resource *res, bool is_compute)
{
   if (res->bindless[0] || res->bindless[1]) {
      /* bindless needs most permissive layout */
      if (res->image_bind_count[0] || res->image_bind_count[1])
         return VK_IMAGE_LAYOUT_GENERAL;
      return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   }
   if (res->image_bind_count[is_compute])
      return VK_IMAGE_LAYOUT_GENERAL;
   if (res->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      if (!is_compute && res->fb_binds &&
          ctx->gfx_pipeline_state.render_pass && ctx->gfx_pipeline_state.render_pass->state.rts[ctx->fb_state.nr_cbufs].mixed_zs)
         return VK_IMAGE_LAYOUT_GENERAL;
      if (res->obj->vkusage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
         return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   }
   return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static struct zink_descriptor_pool *
descriptor_pool_get(struct zink_context *ctx, enum zink_descriptor_type type,
                    const struct zink_descriptor_pool_key *pool_key)
{
   uint32_t hash = 0;
   if (type != ZINK_DESCRIPTOR_TYPES) {
      hash = hash_descriptor_pool_key(pool_key);
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(ctx->dd->descriptor_pools[type], hash, pool_key);
      if (he) {
         struct zink_descriptor_pool *pool = he->data;
         pipe_reference(NULL, &pool->reference);
         return pool;
      }
   }
   struct zink_descriptor_pool *pool = descriptor_pool_create(zink_screen(ctx->base.screen), type, pool_key);
   if (type != ZINK_DESCRIPTOR_TYPES)
      _mesa_hash_table_insert_pre_hashed(ctx->dd->descriptor_pools[type], hash, pool_key, pool);
   return pool;
}

static bool
get_invalidated_desc_set(struct zink_descriptor_set *zds)
{
   if (!zds->invalid)
      return false;
   return p_atomic_read(&zds->reference.count) == 1;
}

bool
zink_descriptor_util_alloc_sets(struct zink_screen *screen, VkDescriptorSetLayout dsl, VkDescriptorPool pool, VkDescriptorSet *sets, unsigned num_sets)
{
   VkDescriptorSetAllocateInfo dsai;
   VkDescriptorSetLayout *layouts = alloca(sizeof(*layouts) * num_sets);
   memset((void *)&dsai, 0, sizeof(dsai));
   dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   dsai.pNext = NULL;
   dsai.descriptorPool = pool;
   dsai.descriptorSetCount = num_sets;
   for (unsigned i = 0; i < num_sets; i ++)
      layouts[i] = dsl;
   dsai.pSetLayouts = layouts;

   VkResult result = VKSCR(AllocateDescriptorSets)(screen->dev, &dsai, sets);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: %" PRIu64 " failed to allocate descriptor set :/ (%s)", (uint64_t)dsl, vk_Result_to_str(result));
      return false;
   }
   return true;
}

static struct zink_descriptor_set *
allocate_desc_set(struct zink_context *ctx, struct zink_program *pg, enum zink_descriptor_type type, unsigned descs_used, bool is_compute)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   bool push_set = type == ZINK_DESCRIPTOR_TYPES;
   struct zink_descriptor_pool *pool = push_set ? ctx->dd->push_pool[is_compute] : pdd_cached(pg)->pool[type];
#define DESC_BUCKET_FACTOR 10
   unsigned bucket_size = pool->key->layout->num_bindings ? DESC_BUCKET_FACTOR : 1;
   if (pool->key->layout->num_bindings) {
      for (unsigned desc_factor = DESC_BUCKET_FACTOR; desc_factor < descs_used; desc_factor *= DESC_BUCKET_FACTOR)
         bucket_size = desc_factor;
   }
   /* never grow more than this many at a time */
   bucket_size = MIN2(bucket_size, ZINK_DEFAULT_MAX_DESCS);
   VkDescriptorSet *desc_set = alloca(sizeof(*desc_set) * bucket_size);
   if (!zink_descriptor_util_alloc_sets(screen, push_set ? ctx->dd->push_dsl[is_compute]->layout : pg->dsl[type + 1], pool->descpool, desc_set, bucket_size))
      return VK_NULL_HANDLE;

   struct zink_descriptor_set *alloc = ralloc_array(pool, struct zink_descriptor_set, bucket_size);
   assert(alloc);
   unsigned num_resources = pool->num_resources;
   struct zink_resource_object **res_objs = NULL;
   void **samplers = NULL;
   struct zink_descriptor_surface *surfaces = NULL;
   switch (type) {
   case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
      samplers = rzalloc_array(pool, void*, num_resources * bucket_size);
      assert(samplers);
      FALLTHROUGH;
   case ZINK_DESCRIPTOR_TYPE_IMAGE:
      surfaces = rzalloc_array(pool, struct zink_descriptor_surface, num_resources * bucket_size);
      assert(surfaces);
      break;
   default:
      res_objs = rzalloc_array(pool, struct zink_resource_object*, num_resources * bucket_size);
      assert(res_objs);
      break;
   }
   for (unsigned i = 0; i < bucket_size; i ++) {
      struct zink_descriptor_set *zds = &alloc[i];
      pipe_reference_init(&zds->reference, 1);
      zds->pool = pool;
      zds->hash = 0;
      zds->batch_uses = NULL;
      zds->invalid = true;
      zds->punted = zds->recycled = false;
#ifndef NDEBUG
      zds->num_resources = num_resources;
#endif
      switch (type) {
      case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
         zds->sampler_states = (struct zink_sampler_state**)&samplers[i * num_resources];
         FALLTHROUGH;
      case ZINK_DESCRIPTOR_TYPE_IMAGE:
         zds->surfaces = &surfaces[i * num_resources];
         break;
      default:
         zds->res_objs = (struct zink_resource_object**)&res_objs[i * num_resources];
         break;
      }
      zds->desc_set = desc_set[i];
      if (i > 0)
         util_dynarray_append(&pool->alloc_desc_sets, struct zink_descriptor_set *, zds);
   }
   pool->num_sets_allocated += bucket_size;
   return alloc;
}

static void
populate_zds_key(struct zink_context *ctx, enum zink_descriptor_type type, bool is_compute,
                 struct zink_descriptor_state_key *key, uint32_t push_usage)
{
   if (is_compute) {
      for (unsigned i = 1; i < ZINK_SHADER_COUNT; i++)
         key->exists[i] = false;
      key->exists[0] = true;
      if (type == ZINK_DESCRIPTOR_TYPES)
         key->state[0] = ctx->dd->push_state[is_compute];
      else {
         assert(ctx->dd->descriptor_states[is_compute].valid[type]);
         key->state[0] = ctx->dd->descriptor_states[is_compute].state[type];
      }
   } else if (type == ZINK_DESCRIPTOR_TYPES) {
      /* gfx only */
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         if (push_usage & BITFIELD_BIT(i)) {
            key->exists[i] = true;
            key->state[i] = ctx->dd->gfx_push_state[i];
         } else
            key->exists[i] = false;
      }
   } else {
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         key->exists[i] = ctx->dd->gfx_descriptor_states[i].valid[type];
         key->state[i] = ctx->dd->gfx_descriptor_states[i].state[type];
      }
   }
}

static void
populate_zds_key_compact(struct zink_context *ctx, enum zink_descriptor_type type, bool is_compute,
                         struct zink_descriptor_state_key *key, uint32_t push_usage)
{
   if (is_compute) {
      for (unsigned i = 1; i < ZINK_SHADER_COUNT; i++)
         key->exists[i] = false;
      key->exists[0] = true;
      if (type == ZINK_DESCRIPTOR_TYPES)
         key->state[0] = ctx->dd->push_state[is_compute];
      else {
         assert(ctx->dd->compact_descriptor_states[is_compute].valid[type]);
         key->state[0] = ctx->dd->compact_descriptor_states[is_compute].state[type];
      }
   } else if (type == ZINK_DESCRIPTOR_TYPES) {
      /* gfx only */
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         if (push_usage & BITFIELD_BIT(i)) {
            key->exists[i] = true;
            key->state[i] = ctx->dd->gfx_push_state[i];
         } else
            key->exists[i] = false;
      }
   } else {
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         key->exists[i] = ctx->dd->compact_gfx_descriptor_states[i].valid[type];
         key->state[i] = ctx->dd->compact_gfx_descriptor_states[i].state[type];
      }
   }
}

static void
punt_invalid_set(struct zink_descriptor_set *zds, struct hash_entry *he)
{
   /* this is no longer usable, so we punt it for now until it gets recycled */
   assert(!zds->recycled);
   if (!he)
      he = _mesa_hash_table_search_pre_hashed(zds->pool->desc_sets, zds->hash, &zds->key);
   _mesa_hash_table_remove(zds->pool->desc_sets, he);
   zds->punted = true;
}

static struct zink_descriptor_set *
zink_descriptor_set_get(struct zink_context *ctx,
                               enum zink_descriptor_type type,
                               bool is_compute,
                               bool *cache_hit)
{
   *cache_hit = false;
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_descriptor_set *zds;
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;
   struct zink_batch *batch = &ctx->batch;
   bool push_set = type == ZINK_DESCRIPTOR_TYPES;
   struct zink_descriptor_pool *pool = push_set ? ctx->dd->push_pool[is_compute] : pdd_cached(pg)->pool[type];
   unsigned descs_used = 1;
   assert(type <= ZINK_DESCRIPTOR_TYPES);

   assert(pool->key->layout->num_bindings);
   assert(!screen->compact_descriptors || (type != ZINK_DESCRIPTOR_TYPE_SSBO && type != ZINK_DESCRIPTOR_TYPE_IMAGE));
   uint32_t hash = push_set ? ctx->dd->push_state[is_compute] :
                              screen->compact_descriptors ?
                              ctx->dd->compact_descriptor_states[is_compute].state[type] :
                              ctx->dd->descriptor_states[is_compute].state[type];

   struct zink_descriptor_set *last_set = push_set ? ctx->dd->last_set[is_compute] : pdd_cached(pg)->last_set[type];
   /* if the current state hasn't changed since the last time it was used,
    * it's impossible for this set to not be valid, which means that an
    * early return here can be done safely and with no locking
    */
   if (last_set && ((push_set && !ctx->dd->changed[is_compute][ZINK_DESCRIPTOR_TYPES]) ||
                    (!push_set && (screen->compact_descriptors ?
                                   !ctx->dd->changed[is_compute][type] && !ctx->dd->changed[is_compute][type+ZINK_DESCRIPTOR_COMPACT] :
                                   !ctx->dd->changed[is_compute][type])))) {
      *cache_hit = true;
      return last_set;
   }

   struct zink_descriptor_state_key key;
   if (screen->compact_descriptors)
      populate_zds_key_compact(ctx, type, is_compute, &key, pg->dd->push_usage);
   else
      populate_zds_key(ctx, type, is_compute, &key, pg->dd->push_usage);

   simple_mtx_lock(&pool->mtx);
   if (last_set && last_set->hash == hash && desc_state_equal(&last_set->key, &key)) {
      bool was_recycled = false;
      zds = last_set;
      *cache_hit = !zds->invalid;
      if (zds->recycled) {
         struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pool->free_desc_sets, hash, &key);
         if (he) {
            was_recycled = true;
            _mesa_hash_table_remove(pool->free_desc_sets, he);
         }
         zds->recycled = false;
      }
      if (zds->invalid) {
          if (zink_batch_usage_exists(zds->batch_uses))
             punt_invalid_set(zds, NULL);
          else {
             if (was_recycled) {
                descriptor_set_invalidate(zds);
                goto out;
             }
             /* this set is guaranteed to be in pool->alloc_desc_sets */
             goto skip_hash_tables;
          }
          zds = NULL;
      }
      if (zds)
         goto out;
   }

   struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pool->desc_sets, hash, &key);
   bool recycled = false, punted = false;
   if (he) {
       zds = (void*)he->data;
       if (zds->invalid && zink_batch_usage_exists(zds->batch_uses)) {
          punt_invalid_set(zds, he);
          zds = NULL;
          punted = true;
       }
   }
   if (!he) {
      he = _mesa_hash_table_search_pre_hashed(pool->free_desc_sets, hash, &key);
      recycled = true;
   }
   if (he && !punted) {
      zds = (void*)he->data;
      *cache_hit = !zds->invalid;
      if (recycled) {
         if (zds->invalid)
            descriptor_set_invalidate(zds);
         /* need to migrate this entry back to the in-use hash */
         _mesa_hash_table_remove(pool->free_desc_sets, he);
         goto out;
      }
      goto quick_out;
   }
skip_hash_tables:
   if (util_dynarray_num_elements(&pool->alloc_desc_sets, struct zink_descriptor_set *)) {
      /* grab one off the allocated array */
      zds = util_dynarray_pop(&pool->alloc_desc_sets, struct zink_descriptor_set *);
      goto out;
   }

   if (_mesa_hash_table_num_entries(pool->free_desc_sets)) {
      /* try for an invalidated set first */
      unsigned count = 0;
      hash_table_foreach(pool->free_desc_sets, he) {
         struct zink_descriptor_set *tmp = he->data;
         if ((count++ >= 100 && tmp->reference.count == 1) || get_invalidated_desc_set(he->data)) {
            zds = tmp;
            assert(p_atomic_read(&zds->reference.count) == 1);
            descriptor_set_invalidate(zds);
            _mesa_hash_table_remove(pool->free_desc_sets, he);
            goto out;
         }
      }
   }

   assert(pool->num_sets_allocated < ZINK_DEFAULT_MAX_DESCS);

   zds = allocate_desc_set(ctx, pg, type, descs_used, is_compute);
out:
   if (unlikely(pool->num_sets_allocated >= ZINK_DEFAULT_DESC_CLAMP &&
                _mesa_hash_table_num_entries(pool->free_desc_sets) < ZINK_DEFAULT_MAX_DESCS - ZINK_DEFAULT_DESC_CLAMP))
      ctx->oom_flush = ctx->oom_stall = true;
   zds->hash = hash;
   if (screen->compact_descriptors)
      populate_zds_key_compact(ctx, type, is_compute, &zds->key, pg->dd->push_usage);
   else
      populate_zds_key(ctx, type, is_compute, &zds->key, pg->dd->push_usage);
   zds->recycled = false;
   _mesa_hash_table_insert_pre_hashed(pool->desc_sets, hash, &zds->key, zds);
quick_out:
   if (!push_set) {
      if (screen->compact_descriptors) {
         if (zink_desc_type_from_vktype(pool->key->sizes[0].type) == type)
            zds->compacted |= BITFIELD_BIT(type);
         for (unsigned i = 0; i < pool->key->num_type_sizes; i++) {
            if (zink_desc_type_from_vktype(pool->key->sizes[0].type) == type + ZINK_DESCRIPTOR_COMPACT) {
               zds->compacted |= BITFIELD_BIT(type + ZINK_DESCRIPTOR_COMPACT);
               break;
            }
         }
      } else
         zds->compacted |= BITFIELD_BIT(type);
   }
   zds->punted = zds->invalid = false;
   batch_add_desc_set(batch, zds);
   if (push_set)
      ctx->dd->last_set[is_compute] = zds;
   else
      pdd_cached(pg)->last_set[type] = zds;
   simple_mtx_unlock(&pool->mtx);

   return zds;
}

void
zink_descriptor_set_recycle(struct zink_descriptor_set *zds)
{
   struct zink_descriptor_pool *pool = zds->pool;
   /* if desc set is still in use by a batch, don't recache */
   uint32_t refcount = p_atomic_read(&zds->reference.count);
   if (refcount != 1)
      return;
   /* this is a null set */
   if (!pool->key->layout->num_bindings)
      return;
   simple_mtx_lock(&pool->mtx);
   if (zds->punted)
      zds->invalid = true;
   else {
      /* if we've previously punted this set, then it won't have a hash or be in either of the tables */
      struct hash_entry *he = _mesa_hash_table_search_pre_hashed(pool->desc_sets, zds->hash, &zds->key);
      if (!he) {
         /* desc sets can be used multiple times in the same batch */
         simple_mtx_unlock(&pool->mtx);
         return;
      }
      _mesa_hash_table_remove(pool->desc_sets, he);
   }

   if (zds->invalid) {
      descriptor_set_invalidate(zds);
      util_dynarray_append(&pool->alloc_desc_sets, struct zink_descriptor_set *, zds);
   } else {
      zds->recycled = true;
      _mesa_hash_table_insert_pre_hashed(pool->free_desc_sets, zds->hash, &zds->key, zds);
   }
   simple_mtx_unlock(&pool->mtx);
}


static void
desc_set_ref_add(struct zink_descriptor_set *zds, struct zink_descriptor_refs *refs, void **ref_ptr, void *ptr)
{
   struct zink_descriptor_reference ref = {ref_ptr, &zds->invalid};
   *ref_ptr = ptr;
   if (ptr)
      util_dynarray_append(&refs->refs, struct zink_descriptor_reference, ref);
}

static void
zink_descriptor_surface_desc_set_add(struct zink_descriptor_surface *dsurf, struct zink_descriptor_set *zds, unsigned idx)
{
   assert(idx < zds->num_resources);
   zds->surfaces[idx].is_buffer = dsurf->is_buffer;
   if (dsurf->is_buffer)
      desc_set_ref_add(zds, &dsurf->bufferview->desc_set_refs, (void**)&zds->surfaces[idx].bufferview, dsurf->bufferview);
   else
      desc_set_ref_add(zds, &dsurf->surface->desc_set_refs, (void**)&zds->surfaces[idx].surface, dsurf->surface);
}

static void
zink_image_view_desc_set_add(struct zink_image_view *image_view, struct zink_descriptor_set *zds, unsigned idx, bool is_buffer)
{
   assert(idx < zds->num_resources);
   if (is_buffer)
      desc_set_ref_add(zds, &image_view->buffer_view->desc_set_refs, (void**)&zds->surfaces[idx].bufferview, image_view->buffer_view);
   else
      desc_set_ref_add(zds, &image_view->surface->desc_set_refs, (void**)&zds->surfaces[idx].surface, image_view->surface);
}

static void
zink_sampler_state_desc_set_add(struct zink_sampler_state *sampler_state, struct zink_descriptor_set *zds, unsigned idx)
{
   assert(idx < zds->num_resources);
   if (sampler_state)
      desc_set_ref_add(zds, &sampler_state->desc_set_refs, (void**)&zds->sampler_states[idx], sampler_state);
   else
      zds->sampler_states[idx] = NULL;
}

static void
zink_resource_desc_set_add(struct zink_resource *res, struct zink_descriptor_set *zds, unsigned idx)
{
   assert(idx < zds->num_resources);
   desc_set_ref_add(zds, res ? &res->obj->desc_set_refs : NULL, (void**)&zds->res_objs[idx], res ? res->obj : NULL);
}

void
zink_descriptor_set_refs_clear(struct zink_descriptor_refs *refs, void *ptr)
{
   util_dynarray_foreach(&refs->refs, struct zink_descriptor_reference, ref) {
      if (*ref->ref == ptr) {
         *ref->invalid = true;
         *ref->ref = NULL;
      }
   }
   util_dynarray_fini(&refs->refs);
}

static inline void
zink_descriptor_pool_reference(struct zink_context *ctx,
                               struct zink_descriptor_pool **dst,
                               struct zink_descriptor_pool *src)
{
   struct zink_descriptor_pool *old_dst = dst ? *dst : NULL;

   if (pipe_reference_described(old_dst ? &old_dst->reference : NULL, &src->reference,
                                (debug_reference_descriptor)debug_describe_zink_descriptor_pool))
      descriptor_pool_delete(ctx, old_dst);
   if (dst) *dst = src;
}

static void
create_descriptor_ref_template(struct zink_context *ctx, struct zink_program *pg)
{
   struct zink_shader **stages;
   if (pg->is_compute)
      stages = &((struct zink_compute_program*)pg)->shader;
   else
      stages = ((struct zink_gfx_program*)pg)->shaders;
   unsigned num_shaders = pg->is_compute ? 1 : ZINK_SHADER_COUNT;

   for (unsigned type = 0; type < ZINK_DESCRIPTOR_TYPES; type++) {
      for (int i = 0; i < num_shaders; i++) {
         struct zink_shader *shader = stages[i];
         if (!shader)
            continue;

         for (int j = 0; j < shader->num_bindings[type]; j++) {
             int index = shader->bindings[type][j].index;
             if (type == ZINK_DESCRIPTOR_TYPE_UBO && !index)
                continue;
             pdd_cached(pg)->num_refs[type] += shader->bindings[type][j].size;
         }
      }

      if (!pdd_cached(pg)->num_refs[type])
         continue;

      pdd_cached(pg)->refs[type] = ralloc_array(pg->dd, union zink_program_descriptor_refs, pdd_cached(pg)->num_refs[type]);
      if (!pdd_cached(pg)->refs[type])
         return;

      unsigned ref_idx = 0;
      for (int i = 0; i < num_shaders; i++) {
         struct zink_shader *shader = stages[i];
         if (!shader)
            continue;

         enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
         for (int j = 0; j < shader->num_bindings[type]; j++) {
            int index = shader->bindings[type][j].index;
            for (unsigned k = 0; k < shader->bindings[type][j].size; k++) {
               switch (type) {
               case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
                  pdd_cached(pg)->refs[type][ref_idx].sampler.sampler_state = (struct zink_sampler_state**)&ctx->sampler_states[stage][index + k];
                  pdd_cached(pg)->refs[type][ref_idx].sampler.dsurf = &ctx->di.sampler_surfaces[stage][index + k];
                  break;
               case ZINK_DESCRIPTOR_TYPE_IMAGE:
                  pdd_cached(pg)->refs[type][ref_idx].dsurf = &ctx->di.image_surfaces[stage][index + k];
                  break;
               case ZINK_DESCRIPTOR_TYPE_UBO:
                  if (!index)
                     continue;
                  FALLTHROUGH;
               default:
                  pdd_cached(pg)->refs[type][ref_idx].res = &ctx->di.descriptor_res[type][stage][index + k];
                  break;
               }
               assert(ref_idx < pdd_cached(pg)->num_refs[type]);
               ref_idx++;
            }
         }
      }
   }
}

bool
zink_descriptor_program_init(struct zink_context *ctx, struct zink_program *pg)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);

   pg->dd = (void*)rzalloc(pg, struct zink_program_descriptor_data_cached);
   if (!pg->dd)
      return false;

   if (!zink_descriptor_program_init_lazy(ctx, pg))
      return false;

   /* no descriptors */
   if (!pg->dd)
      return true;

   bool has_pools = false;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (!pg->dd->pool_key[i])
         continue;

      const struct zink_descriptor_pool_key *pool_key = pg->dd->pool_key[i];
      struct zink_descriptor_pool *pool = descriptor_pool_get(ctx, i, pool_key);
      if (!pool)
         return false;
      pdd_cached(pg)->pool[i] = pool;
      has_pools = true;
   }
   if (has_pools && screen->info.have_KHR_descriptor_update_template &&
       zink_descriptor_mode != ZINK_DESCRIPTOR_MODE_NOTEMPLATES)
      create_descriptor_ref_template(ctx, pg);

   return true;
}

void
zink_descriptor_program_deinit(struct zink_context *ctx, struct zink_program *pg)
{
   if (!pg->dd)
      return;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++)
      zink_descriptor_pool_reference(ctx, &pdd_cached(pg)->pool[i], NULL);

   zink_descriptor_program_deinit_lazy(ctx, pg);
}

static void
zink_descriptor_pool_deinit(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      /* do not free: programs own these pools */
      _mesa_hash_table_destroy(ctx->dd->descriptor_pools[i], NULL);
   }
   descriptor_pool_free(zink_screen(ctx->base.screen), ctx->dd->push_pool[0]);
   descriptor_pool_free(zink_screen(ctx->base.screen), ctx->dd->push_pool[1]);
}

static bool
zink_descriptor_pool_init(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      ctx->dd->descriptor_pools[i] = _mesa_hash_table_create(ctx, hash_descriptor_pool_key, equals_descriptor_pool_key);
      if (!ctx->dd->descriptor_pools[i])
         return false;
   }
   VkDescriptorPoolSize sizes[2];
   sizes[0].type = zink_descriptor_mode == ZINK_DESCRIPTOR_MODE_LAZY ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
   sizes[0].descriptorCount = ZINK_SHADER_COUNT * ZINK_DEFAULT_MAX_DESCS;
   sizes[1].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
   sizes[1].descriptorCount = ZINK_DEFAULT_MAX_DESCS;
   /* these are freed by ralloc */
   struct zink_descriptor_pool_key *pool_key;
   pool_key = zink_descriptor_util_pool_key_get(ctx, ZINK_DESCRIPTOR_TYPES, ctx->dd->push_layout_keys[0], sizes, ctx->dd->has_fbfetch ? 2 : 1);
   ctx->dd->push_pool[0] = descriptor_pool_get(ctx, 0, pool_key);
   sizes[0].descriptorCount = ZINK_DEFAULT_MAX_DESCS;
   pool_key = zink_descriptor_util_pool_key_get(ctx, ZINK_DESCRIPTOR_TYPES, ctx->dd->push_layout_keys[1], sizes, 1);
   ctx->dd->push_pool[1] = descriptor_pool_get(ctx, 0, pool_key);
   return ctx->dd->push_pool[0] && ctx->dd->push_pool[1];
}


static void
desc_set_res_add(struct zink_descriptor_set *zds, struct zink_resource *res, unsigned int i, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
   assert(!cache_hit || zds->res_objs[i] == (res ? res->obj : NULL));
   if (!cache_hit)
      zink_resource_desc_set_add(res, zds, i);
}

static void
desc_set_sampler_add(struct zink_context *ctx, struct zink_descriptor_set *zds, struct zink_descriptor_surface *dsurf,
                     struct zink_sampler_state *state, unsigned int i, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
#ifndef NDEBUG
   uint32_t cur_hash = get_descriptor_surface_hash(ctx, &zds->surfaces[i]);
   uint32_t new_hash = get_descriptor_surface_hash(ctx, dsurf);
#endif
   assert(!cache_hit || cur_hash == new_hash);
   assert(!cache_hit || zds->sampler_states[i] == state);
   if (!cache_hit) {
      zink_descriptor_surface_desc_set_add(dsurf, zds, i);
      zink_sampler_state_desc_set_add(state, zds, i);
   }
}

static void
desc_set_image_add(struct zink_context *ctx, struct zink_descriptor_set *zds, struct zink_descriptor_surface *dsurf,
                   unsigned int i, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
#ifndef NDEBUG
   uint32_t cur_hash = get_descriptor_surface_hash(ctx, &zds->surfaces[i]);
   uint32_t new_hash = get_descriptor_surface_hash(ctx, dsurf);
#endif
   assert(!cache_hit || cur_hash == new_hash);
   if (!cache_hit)
      zink_descriptor_surface_desc_set_add(dsurf, zds, i);
}

static void
desc_set_descriptor_surface_add(struct zink_context *ctx, struct zink_descriptor_set *zds, struct zink_descriptor_surface *dsurf,
                   unsigned int i, bool cache_hit)
{
   /* if we got a cache hit, we have to verify that the cached set is still valid;
    * we store the vk resource to the set here to avoid a more complex and costly mechanism of maintaining a
    * hash table on every resource with the associated descriptor sets that then needs to be iterated through
    * whenever a resource is destroyed
    */
#ifndef NDEBUG
   uint32_t cur_hash = get_descriptor_surface_hash(ctx, &zds->surfaces[i]);
   uint32_t new_hash = get_descriptor_surface_hash(ctx, dsurf);
#endif
   assert(!cache_hit || cur_hash == new_hash);
   if (!cache_hit)
      zink_descriptor_surface_desc_set_add(dsurf, zds, i);
}

static unsigned
init_write_descriptor(struct zink_shader *shader, VkDescriptorSet desc_set, enum zink_descriptor_type type, int idx, VkWriteDescriptorSet *wd, unsigned num_wds)
{
    wd->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd->pNext = NULL;
    wd->dstBinding = shader ? shader->bindings[type][idx].binding : idx;
    wd->dstArrayElement = 0;
    wd->descriptorCount = shader ? shader->bindings[type][idx].size : 1;
    wd->descriptorType = shader ? shader->bindings[type][idx].type :
                                  idx == ZINK_FBFETCH_BINDING ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wd->dstSet = desc_set;
    return num_wds + 1;
}

static unsigned
update_push_ubo_descriptors(struct zink_context *ctx, struct zink_descriptor_set *zds,
                            VkDescriptorSet desc_set,
                            bool is_compute, bool cache_hit, uint32_t *dynamic_offsets)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkWriteDescriptorSet wds[ZINK_SHADER_COUNT + 1];
   VkDescriptorBufferInfo buffer_infos[ZINK_SHADER_COUNT];
   struct zink_shader **stages;
   bool fbfetch = false;

   unsigned num_stages = is_compute ? 1 : ZINK_SHADER_COUNT;
   struct zink_program *pg = is_compute ? &ctx->curr_compute->base : &ctx->curr_program->base;
   if (is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   for (int i = 0; i < num_stages; i++) {
      struct zink_shader *shader = stages[i];
      enum pipe_shader_type pstage = shader ? pipe_shader_type_from_mesa(shader->nir->info.stage) : i;
      VkDescriptorBufferInfo *info = &ctx->di.ubos[pstage][0];
      unsigned dynamic_idx = is_compute ? 0 : tgsi_processor_to_shader_stage(pstage);
 
      /* Values are taken from pDynamicOffsets in an order such that all entries for set N come before set N+1;
       * within a set, entries are ordered by the binding numbers in the descriptor set layouts
       * - vkCmdBindDescriptorSets spec
       *
       * because of this, we have to populate the dynamic offsets by their shader stage to ensure they
       * match what the driver expects
       */
      const bool used = (pg->dd->push_usage & BITFIELD_BIT(pstage)) == BITFIELD_BIT(pstage);
      dynamic_offsets[dynamic_idx] = used ? info->offset : 0;
      if (!cache_hit) {
         init_write_descriptor(NULL, desc_set, ZINK_DESCRIPTOR_TYPE_UBO, tgsi_processor_to_shader_stage(pstage), &wds[i], 0);
         if (used) {
            if (zds)
               desc_set_res_add(zds, ctx->di.descriptor_res[ZINK_DESCRIPTOR_TYPE_UBO][pstage][0], i, cache_hit);
            buffer_infos[i].buffer = info->buffer;
            buffer_infos[i].range = info->range;
         } else {
            if (zds)
               desc_set_res_add(zds, NULL, i, cache_hit);
            if (unlikely(!screen->info.rb2_feats.nullDescriptor))
               buffer_infos[i].buffer = zink_resource(ctx->dummy_vertex_buffer)->obj->buffer;
            else
               buffer_infos[i].buffer = VK_NULL_HANDLE;
            buffer_infos[i].range = VK_WHOLE_SIZE;
         }
         /* these are dynamic UBO descriptors, so we have to always set 0 as the descriptor offset */
         buffer_infos[i].offset = 0;
         wds[i].pBufferInfo = &buffer_infos[i];
      }
   }
   if (unlikely(!cache_hit && !is_compute && ctx->dd->has_fbfetch)) {
      init_write_descriptor(NULL, desc_set, 0, MESA_SHADER_STAGES, &wds[ZINK_SHADER_COUNT], 0);
      wds[ZINK_SHADER_COUNT].pImageInfo = &ctx->di.fbfetch;
      fbfetch = true;
   }

   if (!cache_hit)
      VKSCR(UpdateDescriptorSets)(screen->dev, num_stages + !!fbfetch, wds, 0, NULL);
   return num_stages;
}

static void
set_descriptor_set_refs(struct zink_context *ctx, struct zink_descriptor_set *zds, struct zink_program *pg, bool cache_hit)
{
   const bool compact_descriptors = zink_screen(ctx->base.screen)->compact_descriptors;
   STATIC_ASSERT(ZINK_DESCRIPTOR_TYPE_UBO + ZINK_DESCRIPTOR_COMPACT == ZINK_DESCRIPTOR_TYPE_SSBO);
   STATIC_ASSERT(ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW + ZINK_DESCRIPTOR_COMPACT == ZINK_DESCRIPTOR_TYPE_IMAGE);
   const enum zink_descriptor_type types[] = {zds->pool->type, zds->pool->type + ZINK_DESCRIPTOR_COMPACT};
   unsigned num_types = compact_descriptors ? 2 : 1;
   for (unsigned n = 0; n < num_types; n++) {
      const enum zink_descriptor_type type = types[n];
      for (unsigned i = 0; i < pdd_cached(pg)->num_refs[type]; i++) {
         switch (type) {
         case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
            desc_set_sampler_add(ctx, zds, pdd_cached(pg)->refs[type][i].sampler.dsurf,
                                           *pdd_cached(pg)->refs[type][i].sampler.sampler_state, i, cache_hit);
            break;
         case ZINK_DESCRIPTOR_TYPE_IMAGE:
            desc_set_descriptor_surface_add(ctx, zds, pdd_cached(pg)->refs[type][i].dsurf, i, cache_hit);
            break;
         default:
            desc_set_res_add(zds, *pdd_cached(pg)->refs[type][i].res, i, cache_hit);
            break;
         }
      }
   }
}

static void
update_descriptors_internal(struct zink_context *ctx, enum zink_descriptor_type type, struct zink_descriptor_set *zds, struct zink_program *pg, bool cache_hit)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_shader **stages;

   unsigned num_stages = pg->is_compute ? 1 : ZINK_SHADER_COUNT;
   if (pg->is_compute)
      stages = &ctx->curr_compute->shader;
   else
      stages = &ctx->gfx_stages[0];

   if (cache_hit || !zds)
      return;

   if (screen->info.have_KHR_descriptor_update_template &&
       zink_descriptor_mode != ZINK_DESCRIPTOR_MODE_NOTEMPLATES) {
      set_descriptor_set_refs(ctx, zds, pg, cache_hit);
      zink_descriptor_set_update_lazy(ctx, pg, type, zds->desc_set);
      return;
   }

   unsigned num_resources = 0;
   ASSERTED unsigned num_bindings = zds->pool->num_resources;
   VkWriteDescriptorSet wds[ZINK_MAX_DESCRIPTORS_PER_TYPE];
   unsigned num_wds = 0;

   const enum zink_descriptor_type types[2] = {type, type + ZINK_DESCRIPTOR_COMPACT};
   for (unsigned n = 0; n < ARRAY_SIZE(types); n++) {
      if (!(zds->compacted & BITFIELD_BIT(types[n])))
         continue;
      type = types[n];
      for (int i = 0; i < num_stages; i++) {
         struct zink_shader *shader = stages[i];
         if (!shader)
            continue;
         enum pipe_shader_type stage = pipe_shader_type_from_mesa(shader->nir->info.stage);
         for (int j = 0; j < shader->num_bindings[type]; j++) {
            int index = shader->bindings[type][j].index;
            switch (type) {
            case ZINK_DESCRIPTOR_TYPE_UBO:
               if (!index)
                  continue;
            FALLTHROUGH;
            case ZINK_DESCRIPTOR_TYPE_SSBO: {
               VkDescriptorBufferInfo *info;
               struct zink_resource *res = ctx->di.descriptor_res[type][stage][index];
               if (type == ZINK_DESCRIPTOR_TYPE_UBO)
                  info = &ctx->di.ubos[stage][index];
               else
                  info = &ctx->di.ssbos[stage][index];
               assert(num_resources < num_bindings);
               desc_set_res_add(zds, res, num_resources++, cache_hit);
               wds[num_wds].pBufferInfo = info;
            }
            break;
            case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
            case ZINK_DESCRIPTOR_TYPE_IMAGE: {
               VkDescriptorImageInfo *image_info;
               VkBufferView *buffer_info;
               if (type == ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW) {
                  image_info = &ctx->di.textures[stage][index];
                  buffer_info = &ctx->di.tbos[stage][index];
               } else {
                  image_info = &ctx->di.images[stage][index];
                  buffer_info = &ctx->di.texel_images[stage][index];
               }
               bool is_buffer = zink_shader_descriptor_is_buffer(shader, type, j);
               for (unsigned k = 0; k < shader->bindings[type][j].size; k++) {
                  assert(num_resources < num_bindings);
                  if (type == ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW) {
                     struct zink_sampler_state *sampler = NULL;
                     if (!is_buffer && image_info->imageView)
                        sampler = ctx->sampler_states[stage][index + k];;

                     desc_set_sampler_add(ctx, zds, &ctx->di.sampler_surfaces[stage][index + k], sampler, num_resources++, cache_hit);
                  } else {
                     desc_set_image_add(ctx, zds, &ctx->di.image_surfaces[stage][index + k], num_resources++, cache_hit);
                  }
               }
               if (is_buffer)
                  wds[num_wds].pTexelBufferView = buffer_info;
               else
                  wds[num_wds].pImageInfo = image_info;
            }
            break;
            default:
               unreachable("unknown descriptor type");
            }
            num_wds = init_write_descriptor(shader, zds->desc_set, type, j, &wds[num_wds], num_wds);
         }
      }
   }
   if (num_wds)
      VKSCR(UpdateDescriptorSets)(screen->dev, num_wds, wds, 0, NULL);
}

static void
zink_context_update_descriptor_states(struct zink_context *ctx, struct zink_program *pg);

#define MAX_CACHE_MISSES 50

void
zink_descriptors_update(struct zink_context *ctx, bool is_compute)
{
   struct zink_program *pg = is_compute ? (struct zink_program *)ctx->curr_compute : (struct zink_program *)ctx->curr_program;

   if (ctx->dd->pg[is_compute] != pg) {
      for (int h = 0; h < ZINK_DESCRIPTOR_TYPES; h++) {
         if (pg->dd->real_binding_usage & BITFIELD_BIT(h))
            ctx->dd->changed[is_compute][h] = true;
         ctx->dd->descriptor_states[is_compute].valid[h] = false;
         if (!is_compute) {
            for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++)
               ctx->dd->gfx_descriptor_states[i].valid[h] = false;
         }
      }
   }
   zink_context_update_descriptor_states(ctx, pg);
   bool cache_hit;
   VkDescriptorSet desc_set = VK_NULL_HANDLE;
   struct zink_descriptor_set *zds = NULL;

   struct zink_batch *batch = &ctx->batch;
   VkPipelineBindPoint bp = is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

   {
      uint32_t dynamic_offsets[PIPE_MAX_CONSTANT_BUFFERS];
      unsigned dynamic_offset_idx = 0;

      /* push set is indexed in vulkan as 0 but isn't in the general pool array */
      ctx->dd->changed[is_compute][ZINK_DESCRIPTOR_TYPES] |= ctx->dd->pg[is_compute] != pg;
      if (pg->dd->push_usage) {
         if (pg->dd->fbfetch) {
            /* fbfetch is not cacheable: grab a lazy set because it's faster */
            cache_hit = false;
            desc_set = zink_descriptors_alloc_lazy_push(ctx);
         } else {
            zds = zink_descriptor_set_get(ctx, ZINK_DESCRIPTOR_TYPES, is_compute, &cache_hit);
            desc_set = zds ? zds->desc_set : VK_NULL_HANDLE;
         }
      } else {
         cache_hit = false;
      }
      ctx->dd->changed[is_compute][ZINK_DESCRIPTOR_TYPES] = false;
      if (desc_set) {
         if (pg->dd->push_usage) // push set
            dynamic_offset_idx = update_push_ubo_descriptors(ctx, zds, desc_set,
                                                             is_compute, cache_hit, dynamic_offsets);
         VKCTX(CmdBindDescriptorSets)(batch->state->cmdbuf, bp,
                                 pg->layout, 0, 1, &desc_set,
                                 dynamic_offset_idx, dynamic_offsets);
      }
   }

   {
      for (int h = 0; h < ZINK_DESCRIPTOR_TYPES; h++) {
         if (pdd_cached(pg)->cache_misses[h] < MAX_CACHE_MISSES) {
            if (pg->dsl[h + 1]) {
               /* null set has null pool */
               if (pdd_cached(pg)->pool[h]) {
                  zds = zink_descriptor_set_get(ctx, h, is_compute, &cache_hit);
                  if (cache_hit) {
                     pdd_cached(pg)->cache_misses[h] = 0;
                  } else {
                     if (++pdd_cached(pg)->cache_misses[h] == MAX_CACHE_MISSES) {
#ifdef PRINT_DEBUG
                        const char *set_names[] = {
                           "UBO",
                           "TEXTURES",
                           "SSBO",
                           "IMAGES",
                        };
                        debug_printf("zink: descriptor cache exploded for prog %p set %s: getting lazy (not a bug, just lettin you know)\n", pg, set_names[h]);
#endif
                     }
                  }
               } else
                  zds = NULL;
               if (zds) {
                  desc_set = zds->desc_set;
                  update_descriptors_internal(ctx, h, zds, pg, cache_hit);

                  VKCTX(CmdBindDescriptorSets)(batch->state->cmdbuf, bp,
                                               pg->layout, h + 1, 1, &desc_set,
                                               0, NULL);
                  if (pdd_cached(pg)->cache_misses[h] == MAX_CACHE_MISSES)
                     zink_descriptor_pool_reference(ctx, &pdd_cached(pg)->pool[h], NULL);
               }
            }
         } else {
            zink_descriptors_update_lazy_masked(ctx, is_compute, BITFIELD_BIT(h), 0);
         }
         ctx->dd->changed[is_compute][h] = false;
      }
   }
   ctx->dd->pg[is_compute] = pg;

   if (pg->dd->bindless && unlikely(!ctx->dd->bindless_bound)) {
      VKCTX(CmdBindDescriptorSets)(batch->state->cmdbuf, bp,
                                   pg->layout, ZINK_DESCRIPTOR_BINDLESS, 1, &ctx->dd->bindless_set,
                                   0, NULL);
      ctx->dd->bindless_bound = true;
   }
}

void
zink_batch_descriptor_deinit(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs->dd)
      return;
   _mesa_set_destroy(bs->dd->desc_sets, NULL);
   zink_batch_descriptor_deinit_lazy(screen, bs);
}

void
zink_batch_descriptor_reset(struct zink_screen *screen, struct zink_batch_state *bs)
{
   set_foreach(bs->dd->desc_sets, entry) {
      struct zink_descriptor_set *zds = (void*)entry->key;
      zink_batch_usage_unset(&zds->batch_uses, bs);
      /* reset descriptor pools when no bs is using this program to avoid
       * having some inactive program hogging a billion descriptors
       */
      pipe_reference(&zds->reference, NULL);
      zink_descriptor_set_recycle(zds);
      if (zds->reference.count == 1) {
         struct zink_descriptor_pool *pool = zds->pool;
         zink_descriptor_pool_reference(bs->ctx, &pool, NULL);
      }
      _mesa_set_remove(bs->dd->desc_sets, entry);
   }
   zink_batch_descriptor_reset_lazy(screen, bs);
}

bool
zink_batch_descriptor_init(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!zink_batch_descriptor_init_lazy(screen, bs))
      return false;
   bs->dd->desc_sets = _mesa_pointer_set_create(bs);
   return !!bs->dd->desc_sets;
}

static uint32_t
calc_descriptor_state_hash_ubo(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash, bool need_offset)
{
   for (unsigned k = 0; k < zs->bindings[ZINK_DESCRIPTOR_TYPE_UBO][i].size; k++) {
      struct zink_resource *res = ctx->di.descriptor_res[ZINK_DESCRIPTOR_TYPE_UBO][shader][idx + k];
      struct zink_resource_object *obj = res ? res->obj : NULL;
      hash = XXH32(&obj, sizeof(void*), hash);
      void *hash_data = &ctx->di.ubos[shader][idx + k].range;
      size_t data_size = sizeof(unsigned);
      hash = XXH32(hash_data, data_size, hash);
      if (need_offset)
         hash = XXH32(&ctx->di.ubos[shader][idx + k].offset, sizeof(unsigned), hash);
   }
   return hash;
}

static uint32_t
calc_descriptor_state_hash_ssbo(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash)
{
   for (unsigned k = 0; k < zs->bindings[ZINK_DESCRIPTOR_TYPE_SSBO][i].size; k++) {
      struct zink_resource *res = ctx->di.descriptor_res[ZINK_DESCRIPTOR_TYPE_SSBO][shader][idx + k];
      struct zink_resource_object *obj = res ? res->obj : NULL;
      hash = XXH32(&obj, sizeof(void*), hash);
      if (obj) {
         struct pipe_shader_buffer *ssbo = &ctx->ssbos[shader][idx + k];
         hash = XXH32(&ssbo->buffer_offset, sizeof(ssbo->buffer_offset), hash);
         hash = XXH32(&ssbo->buffer_size, sizeof(ssbo->buffer_size), hash);
         /* compacted sets need a way to differentiate between a buffer bound as a ubo vs ssbo */
         if (zink_screen(ctx->base.screen)->compact_descriptors) {
            uint32_t writable = ctx->writable_ssbos[shader] & BITFIELD_BIT(idx + k);
            hash = XXH32(&writable, sizeof(writable), hash);
         }
      }
   }
   return hash;
}

static uint32_t
calc_descriptor_state_hash_sampler(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash)
{
   for (unsigned k = 0; k < zs->bindings[ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW][i].size; k++) {
      struct zink_sampler_view *sampler_view = zink_sampler_view(ctx->sampler_views[shader][idx + k]);
      bool is_buffer = zink_shader_descriptor_is_buffer(zs, ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW, i);
      ctx->di.sampler_surfaces[shader][idx + k].is_buffer = is_buffer;
      uint32_t val = zink_get_sampler_view_hash(ctx, sampler_view, is_buffer);
      hash = XXH32(&val, sizeof(uint32_t), hash);
      if (is_buffer)
         continue;

      hash = XXH32(&ctx->di.textures[shader][idx + k].imageLayout, sizeof(VkImageLayout), hash);

      struct zink_sampler_state *sampler_state = ctx->sampler_states[shader][idx + k];

      if (sampler_state)
         hash = XXH32(&sampler_state->hash, sizeof(uint32_t), hash);
   }
   return hash;
}

static uint32_t
calc_descriptor_state_hash_image(struct zink_context *ctx, struct zink_shader *zs, enum pipe_shader_type shader, int i, int idx, uint32_t hash)
{
   for (unsigned k = 0; k < zs->bindings[ZINK_DESCRIPTOR_TYPE_IMAGE][i].size; k++) {
      bool is_buffer = zink_shader_descriptor_is_buffer(zs, ZINK_DESCRIPTOR_TYPE_IMAGE, i);
      uint32_t val = zink_get_image_view_hash(ctx, &ctx->image_views[shader][idx + k], is_buffer);
      ctx->di.image_surfaces[shader][idx + k].is_buffer = is_buffer;
      hash = XXH32(&val, sizeof(uint32_t), hash);
   }
   return hash;
}

static uint32_t
update_descriptor_stage_state(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type)
{
   struct zink_shader *zs = shader == PIPE_SHADER_COMPUTE ? ctx->compute_stage : ctx->gfx_stages[shader];

   uint32_t hash = 0;
   for (int i = 0; i < zs->num_bindings[type]; i++) {
      /* skip push set members */
      if (zs->bindings[type][i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
         continue;

      int idx = zs->bindings[type][i].index;
      switch (type) {
      case ZINK_DESCRIPTOR_TYPE_UBO:
         hash = calc_descriptor_state_hash_ubo(ctx, zs, shader, i, idx, hash, true);
         break;
      case ZINK_DESCRIPTOR_TYPE_SSBO:
         hash = calc_descriptor_state_hash_ssbo(ctx, zs, shader, i, idx, hash);
         break;
      case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
         hash = calc_descriptor_state_hash_sampler(ctx, zs, shader, i, idx, hash);
         break;
      case ZINK_DESCRIPTOR_TYPE_IMAGE:
         hash = calc_descriptor_state_hash_image(ctx, zs, shader, i, idx, hash);
         break;
      default:
         unreachable("unknown descriptor type");
      }
   }
   return hash;
}

static void
update_descriptor_state(struct zink_context *ctx, enum zink_descriptor_type type, bool is_compute)
{
   /* we shouldn't be calling this if we don't have to */
   assert(!ctx->dd->descriptor_states[is_compute].valid[type]);
   bool has_any_usage = false;

   if (is_compute) {
      /* just update compute state */
      bool has_usage = zink_program_get_descriptor_usage(ctx, PIPE_SHADER_COMPUTE, type);
      if (has_usage)
         ctx->dd->descriptor_states[is_compute].state[type] = update_descriptor_stage_state(ctx, PIPE_SHADER_COMPUTE, type);
      else
         ctx->dd->descriptor_states[is_compute].state[type] = 0;
      has_any_usage = has_usage;
   } else {
      /* update all gfx states */
      bool first = true;
      for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
         bool has_usage = false;
         /* this is the incremental update for the shader stage */
         if (!ctx->dd->gfx_descriptor_states[i].valid[type]) {
            ctx->dd->gfx_descriptor_states[i].state[type] = 0;
            if (ctx->gfx_stages[i]) {
               has_usage = zink_program_get_descriptor_usage(ctx, i, type);
               if (has_usage)
                  ctx->dd->gfx_descriptor_states[i].state[type] = update_descriptor_stage_state(ctx, i, type);
               ctx->dd->gfx_descriptor_states[i].valid[type] = has_usage;
            }
         }
         if (ctx->dd->gfx_descriptor_states[i].valid[type]) {
            /* this is the overall state update for the descriptor set hash */
            if (first) {
               /* no need to double hash the first state */
               ctx->dd->descriptor_states[is_compute].state[type] = ctx->dd->gfx_descriptor_states[i].state[type];
               first = false;
            } else {
               ctx->dd->descriptor_states[is_compute].state[type] ^= ctx->dd->gfx_descriptor_states[i].state[type];
            }
         }
         has_any_usage |= has_usage;
      }
   }
   ctx->dd->descriptor_states[is_compute].valid[type] = has_any_usage;
}

static void
zink_context_update_descriptor_states(struct zink_context *ctx, struct zink_program *pg)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (pg->dd->push_usage && (!ctx->dd->push_valid[pg->is_compute] ||
                                           pg->dd->push_usage != ctx->dd->last_push_usage[pg->is_compute])) {
      uint32_t hash = 0;
      if (pg->is_compute) {
          hash = calc_descriptor_state_hash_ubo(ctx, ctx->compute_stage, PIPE_SHADER_COMPUTE, 0, 0, 0, false);
      } else {
         bool first = true;
         u_foreach_bit(stage, pg->dd->push_usage) {
            if (!ctx->dd->gfx_push_valid[stage]) {
               ctx->dd->gfx_push_state[stage] = calc_descriptor_state_hash_ubo(ctx, ctx->gfx_stages[stage], stage, 0, 0, 0, false);
               ctx->dd->gfx_push_valid[stage] = true;
            }
            if (first)
               hash = ctx->dd->gfx_push_state[stage];
            else
               hash ^= ctx->dd->gfx_push_state[stage];
            first = false;
         }
      }
      ctx->dd->changed[pg->is_compute][ZINK_DESCRIPTOR_TYPES] |= ctx->dd->push_state[pg->is_compute] != hash;
      ctx->dd->changed[pg->is_compute][ZINK_DESCRIPTOR_TYPES] |= pg->dd->push_usage != ctx->dd->last_push_usage[pg->is_compute];
      ctx->dd->push_state[pg->is_compute] = hash;
      ctx->dd->push_valid[pg->is_compute] = true;
      ctx->dd->last_push_usage[pg->is_compute] = pg->dd->push_usage;
   }
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (pdd_cached(pg)->pool[screen->desc_set_id[i] - 1] && pdd_cached(pg)->cache_misses[i] < MAX_CACHE_MISSES &&
          ctx->dd->changed[pg->is_compute][i] &&
          !ctx->dd->descriptor_states[pg->is_compute].valid[i])
         update_descriptor_state(ctx, i, pg->is_compute);
   }

   if (!screen->compact_descriptors)
      return;

   for (unsigned n = 0; n < 2; n++) {
      ctx->dd->compact_descriptor_states[pg->is_compute].valid[n] = ctx->dd->descriptor_states[pg->is_compute].valid[n] |
                                                                    ctx->dd->descriptor_states[pg->is_compute].valid[n + ZINK_DESCRIPTOR_COMPACT];
      if (ctx->dd->compact_descriptor_states[pg->is_compute].valid[n]) {
         if (pg->is_compute) {
            ctx->dd->compact_descriptor_states[pg->is_compute].state[n] = ctx->dd->descriptor_states[pg->is_compute].state[n] ^
                                                                          ctx->dd->descriptor_states[pg->is_compute].state[n + ZINK_DESCRIPTOR_COMPACT];
         } else {
            uint32_t hash = 0;
            bool first = true;
            for (unsigned i = 0; i < ZINK_SHADER_COUNT; i++) {
               ctx->dd->compact_gfx_descriptor_states[i].valid[n] = ctx->dd->gfx_descriptor_states[i].valid[n] |
                                                                    ctx->dd->gfx_descriptor_states[i].valid[n + ZINK_DESCRIPTOR_COMPACT];
               if (ctx->dd->compact_gfx_descriptor_states[i].valid[n]) {
                  ctx->dd->compact_gfx_descriptor_states[i].state[n] = ctx->dd->gfx_descriptor_states[i].state[n] ^
                                                                       ctx->dd->gfx_descriptor_states[i].state[n + ZINK_DESCRIPTOR_COMPACT];
                  if (first)
                     hash = ctx->dd->compact_gfx_descriptor_states[i].state[n];
                  else
                     hash ^= ctx->dd->compact_gfx_descriptor_states[i].state[n];
                  first = false;
               } else {
                  ctx->dd->compact_gfx_descriptor_states[i].state[n] = 0;
               }
            }
            ctx->dd->compact_descriptor_states[pg->is_compute].state[n] = hash;
         }
      } else {
         ctx->dd->compact_descriptor_states[pg->is_compute].state[n] = 0;
      }
   }
}

void
zink_context_invalidate_descriptor_state(struct zink_context *ctx, enum pipe_shader_type shader, enum zink_descriptor_type type, unsigned start, unsigned count)
{
   zink_context_invalidate_descriptor_state_lazy(ctx, shader, type, start, count);
   if (type == ZINK_DESCRIPTOR_TYPE_UBO && !start) {
      /* ubo 0 is the push set */
      ctx->dd->push_state[shader == PIPE_SHADER_COMPUTE] = 0;
      ctx->dd->push_valid[shader == PIPE_SHADER_COMPUTE] = false;
      if (shader != PIPE_SHADER_COMPUTE) {
         ctx->dd->gfx_push_state[shader] = 0;
         ctx->dd->gfx_push_valid[shader] = false;
      }
      ctx->dd->changed[shader == PIPE_SHADER_COMPUTE][ZINK_DESCRIPTOR_TYPES] = true;
      return;
   }
   if (shader != PIPE_SHADER_COMPUTE) {
      ctx->dd->gfx_descriptor_states[shader].valid[type] = false;
      ctx->dd->gfx_descriptor_states[shader].state[type] = 0;
   }
   ctx->dd->descriptor_states[shader == PIPE_SHADER_COMPUTE].valid[type] = false;
   ctx->dd->descriptor_states[shader == PIPE_SHADER_COMPUTE].state[type] = 0;
   ctx->dd->changed[shader == PIPE_SHADER_COMPUTE][type] = true;
}

bool
zink_descriptors_init(struct zink_context *ctx)
{
   zink_descriptors_init_lazy(ctx);
   if (!ctx->dd)
      return false;
   return zink_descriptor_pool_init(ctx);
}

void
zink_descriptors_deinit(struct zink_context *ctx)
{
   zink_descriptor_pool_deinit(ctx);
   zink_descriptors_deinit_lazy(ctx);
}

bool
zink_descriptor_layouts_init(struct zink_context *ctx)
{
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      if (!_mesa_hash_table_init(&ctx->desc_set_layouts[i], ctx, hash_descriptor_layout, equals_descriptor_layout))
         return false;
      if (!_mesa_set_init(&ctx->desc_pool_keys[i], ctx, hash_descriptor_pool_key, equals_descriptor_pool_key))
         return false;
   }
   return true;
}

void
zink_descriptor_layouts_deinit(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++) {
      hash_table_foreach(&ctx->desc_set_layouts[i], he) {
         struct zink_descriptor_layout *layout = he->data;
         VKSCR(DestroyDescriptorSetLayout)(screen->dev, layout->layout, NULL);
         ralloc_free(layout);
         _mesa_hash_table_remove(&ctx->desc_set_layouts[i], he);
      }
   }
}


void
zink_descriptor_util_init_fbfetch(struct zink_context *ctx)
{
   if (ctx->dd->has_fbfetch)
      return;

   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VKSCR(DestroyDescriptorSetLayout)(screen->dev, ctx->dd->push_dsl[0]->layout, NULL);
   //don't free these now, let ralloc free on teardown to avoid invalid access
   //ralloc_free(ctx->dd->push_dsl[0]);
   //ralloc_free(ctx->dd->push_layout_keys[0]);
   ctx->dd->push_dsl[0] = create_gfx_layout(ctx, &ctx->dd->push_layout_keys[0], true);
   ctx->dd->has_fbfetch = true;
   if (zink_descriptor_mode != ZINK_DESCRIPTOR_MODE_LAZY)
      zink_descriptor_pool_init(ctx);
}

ALWAYS_INLINE static VkDescriptorType
type_from_bindless_index(unsigned idx)
{
   switch (idx) {
   case 0: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   case 1: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   case 2: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   case 3: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
   default:
      unreachable("unknown index");
   }
}

void
zink_descriptors_init_bindless(struct zink_context *ctx)
{
   if (ctx->dd->bindless_set)
      return;

   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkDescriptorSetLayoutBinding bindings[4];
   const unsigned num_bindings = 4;
   VkDescriptorSetLayoutCreateInfo dcslci = {0};
   dcslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   dcslci.pNext = NULL;
   VkDescriptorSetLayoutBindingFlagsCreateInfo fci = {0};
   VkDescriptorBindingFlags flags[4];
   dcslci.pNext = &fci;
   dcslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
   fci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
   fci.bindingCount = num_bindings;
   fci.pBindingFlags = flags;
   for (unsigned i = 0; i < num_bindings; i++) {
      flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
   }
   for (unsigned i = 0; i < num_bindings; i++) {
      bindings[i].binding = i;
      bindings[i].descriptorType = type_from_bindless_index(i);
      bindings[i].descriptorCount = ZINK_MAX_BINDLESS_HANDLES;
      bindings[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
      bindings[i].pImmutableSamplers = NULL;
   }
   
   dcslci.bindingCount = num_bindings;
   dcslci.pBindings = bindings;
   VkResult result = VKSCR(CreateDescriptorSetLayout)(screen->dev, &dcslci, 0, &ctx->dd->bindless_layout);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateDescriptorSetLayout failed (%s)", vk_Result_to_str(result));
      return;
   }

   VkDescriptorPoolCreateInfo dpci = {0};
   VkDescriptorPoolSize sizes[4];
   for (unsigned i = 0; i < 4; i++) {
      sizes[i].type = type_from_bindless_index(i);
      sizes[i].descriptorCount = ZINK_MAX_BINDLESS_HANDLES;
   }
   dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   dpci.pPoolSizes = sizes;
   dpci.poolSizeCount = 4;
   dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
   dpci.maxSets = 1;
   result = VKSCR(CreateDescriptorPool)(screen->dev, &dpci, 0, &ctx->dd->bindless_pool);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateDescriptorPool failed (%s)", vk_Result_to_str(result));
      return;
   }

   zink_descriptor_util_alloc_sets(screen, ctx->dd->bindless_layout, ctx->dd->bindless_pool, &ctx->dd->bindless_set, 1);
}

void
zink_descriptors_deinit_bindless(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   if (ctx->dd->bindless_layout)
      VKSCR(DestroyDescriptorSetLayout)(screen->dev, ctx->dd->bindless_layout, NULL);
   if (ctx->dd->bindless_pool)
      VKSCR(DestroyDescriptorPool)(screen->dev, ctx->dd->bindless_pool, NULL);
}

void
zink_descriptors_update_bindless(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   for (unsigned i = 0; i < 2; i++) {
      if (!ctx->di.bindless_dirty[i])
         continue;
      while (util_dynarray_contains(&ctx->di.bindless[i].updates, uint32_t)) {
         uint32_t handle = util_dynarray_pop(&ctx->di.bindless[i].updates, uint32_t);
         bool is_buffer = ZINK_BINDLESS_IS_BUFFER(handle);
         VkWriteDescriptorSet wd;
         wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
         wd.pNext = NULL;
         wd.dstSet = ctx->dd->bindless_set;
         wd.dstBinding = is_buffer ? i * 2 + 1: i * 2;
         wd.dstArrayElement = is_buffer ? handle - ZINK_MAX_BINDLESS_HANDLES : handle;
         wd.descriptorCount = 1;
         wd.descriptorType = type_from_bindless_index(wd.dstBinding);
         if (is_buffer)
            wd.pTexelBufferView = &ctx->di.bindless[i].buffer_infos[wd.dstArrayElement];
         else
            wd.pImageInfo = &ctx->di.bindless[i].img_infos[handle];
         VKSCR(UpdateDescriptorSets)(screen->dev, 1, &wd, 0, NULL);
      }
   }
   ctx->di.any_bindless_dirty = 0;
}
