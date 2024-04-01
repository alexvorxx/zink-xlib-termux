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

#include "zink_program.h"

#include "zink_compiler.h"
#include "zink_context.h"
#include "zink_descriptors.h"
#include "zink_helpers.h"
#include "zink_pipeline.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_state.h"
#include "zink_inlines.h"

#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

/* for pipeline cache */
#define XXH_INLINE_ALL
#include "util/xxhash.h"

struct gfx_pipeline_cache_entry {
   struct zink_gfx_pipeline_state state;
   VkPipeline pipeline;
};

struct compute_pipeline_cache_entry {
   struct zink_compute_pipeline_state state;
   VkPipeline pipeline;
};

void
debug_describe_zink_gfx_program(char *buf, const struct zink_gfx_program *ptr)
{
   sprintf(buf, "zink_gfx_program");
}

void
debug_describe_zink_compute_program(char *buf, const struct zink_compute_program *ptr)
{
   sprintf(buf, "zink_compute_program");
}

static bool
shader_key_matches(const struct zink_shader_module *zm, bool ignore_size,
                   const struct zink_shader_key *key, unsigned num_uniforms)
{
   bool key_size_differs = ignore_size ? false : zm->key_size != key->size;
   if (key_size_differs || zm->num_uniforms != num_uniforms || zm->has_nonseamless != !!key->base.nonseamless_cube_mask)
      return false;
   const uint32_t nonseamless_size = zm->has_nonseamless ? sizeof(uint32_t) : 0;
   return !memcmp(zm->key, key, zm->key_size) &&
          (!nonseamless_size || !memcmp(zm->key + zm->key_size, &key->base.nonseamless_cube_mask, nonseamless_size)) &&
          (!num_uniforms || !memcmp(zm->key + zm->key_size + nonseamless_size,
                                    key->base.inlined_uniform_values, zm->num_uniforms * sizeof(uint32_t)));
}

static uint32_t
shader_module_hash(const struct zink_shader_module *zm)
{
   const uint32_t nonseamless_size = zm->has_nonseamless ? sizeof(uint32_t) : 0;
   unsigned key_size = zm->key_size + nonseamless_size + zm->num_uniforms * sizeof(uint32_t);
   return _mesa_hash_data(zm->key, key_size);
}

static struct zink_shader_module *
get_shader_module_for_stage(struct zink_context *ctx, struct zink_screen *screen,
                            struct zink_shader *zs, struct zink_gfx_program *prog,
                            struct zink_gfx_pipeline_state *state)
{
   gl_shader_stage stage = zs->nir->info.stage;
   VkShaderModule mod;
   struct zink_shader_module *zm = NULL;
   unsigned inline_size = 0, nonseamless_size = 0;
   struct zink_shader_key *key = &state->shader_keys.key[stage];
   bool ignore_key_size = false;
   if (stage == MESA_SHADER_TESS_CTRL && !zs->is_generated) {
      /* non-generated tcs won't use the shader key */
      ignore_key_size = true;
   }
   if (ctx && zs->nir->info.num_inlinable_uniforms &&
       ctx->inlinable_uniforms_valid_mask & BITFIELD64_BIT(stage)) {
      if (zs->can_inline && (screen->is_cpu || prog->inlined_variant_count[stage] < ZINK_MAX_INLINED_VARIANTS))
         inline_size = zs->nir->info.num_inlinable_uniforms;
      else
         key->inline_uniforms = false;
   }
   if (key->base.nonseamless_cube_mask)
      nonseamless_size = sizeof(uint32_t);

   struct zink_shader_module *iter, *next;
   LIST_FOR_EACH_ENTRY_SAFE(iter, next, &prog->shader_cache[stage][!!nonseamless_size][!!inline_size], list) {
      if (!shader_key_matches(iter, ignore_key_size, key, inline_size))
         continue;
      list_delinit(&iter->list);
      zm = iter;
      break;
   }

   if (!zm) {
      zm = malloc(sizeof(struct zink_shader_module) + key->size + nonseamless_size + inline_size * sizeof(uint32_t));
      if (!zm) {
         return NULL;
      }
      unsigned patch_vertices = state->shader_keys.key[MESA_SHADER_TESS_CTRL ].key.tcs.patch_vertices;
      if (stage == MESA_SHADER_TESS_CTRL && zs->is_generated && zs->spirv) {
         assert(ctx); //TODO async
         mod = zink_shader_tcs_compile(screen, zs, patch_vertices);
      } else {
         mod = zink_shader_compile(screen, zs, prog->nir[stage], key);
      }
      if (!mod) {
         FREE(zm);
         return NULL;
      }
      zm->shader = mod;
      list_inithead(&zm->list);
      zm->num_uniforms = inline_size;
      if (!ignore_key_size) {
         zm->key_size = key->size;
         memcpy(zm->key, key, key->size);
      } else {
         zm->key_size = 0;
         memset(zm->key, 0, key->size);
      }
      if (nonseamless_size) {
         /* nonseamless mask gets added to base key if it exists */
         memcpy(zm->key + key->size, &key->base.nonseamless_cube_mask, nonseamless_size);
      }
      zm->has_nonseamless = !!nonseamless_size;
      if (inline_size)
         memcpy(zm->key + key->size + nonseamless_size, key->base.inlined_uniform_values, inline_size * sizeof(uint32_t));
      if (stage == MESA_SHADER_TESS_CTRL && zs->is_generated)
         zm->hash = patch_vertices;
      else
         zm->hash = shader_module_hash(zm);
      zm->default_variant = !inline_size && list_is_empty(&prog->shader_cache[stage][0][0]);
      if (inline_size)
         prog->inlined_variant_count[stage]++;
   }
   list_add(&zm->list, &prog->shader_cache[stage][!!nonseamless_size][!!inline_size]);
   return zm;
}

static void
zink_destroy_shader_module(struct zink_screen *screen, struct zink_shader_module *zm)
{
   VKSCR(DestroyShaderModule)(screen->dev, zm->shader, NULL);
   free(zm);
}

static void
destroy_shader_cache(struct zink_screen *screen, struct list_head *sc)
{
   struct zink_shader_module *zm, *next;
   LIST_FOR_EACH_ENTRY_SAFE(zm, next, sc, list) {
      list_delinit(&zm->list);
      zink_destroy_shader_module(screen, zm);
   }
}

static void
update_gfx_shader_modules(struct zink_context *ctx,
                      struct zink_screen *screen,
                      struct zink_gfx_program *prog, uint32_t mask,
                      struct zink_gfx_pipeline_state *state)
{
   bool hash_changed = false;
   bool default_variants = true;
   bool first = !prog->modules[MESA_SHADER_VERTEX];
   uint32_t variant_hash = prog->last_variant_hash;
   u_foreach_bit(pstage, mask) {
      assert(prog->shaders[pstage]);
      struct zink_shader_module *zm = get_shader_module_for_stage(ctx, screen, prog->shaders[pstage], prog, state);
      state->modules[pstage] = zm->shader;
      if (prog->modules[pstage] == zm)
         continue;
      if (prog->modules[pstage])
         variant_hash ^= prog->modules[pstage]->hash;
      hash_changed = true;
      default_variants &= zm->default_variant;
      prog->modules[pstage] = zm;
      variant_hash ^= prog->modules[pstage]->hash;
   }

   if (hash_changed && state) {
      if (default_variants && !first)
         prog->last_variant_hash = prog->default_variant_hash;
      else {
         prog->last_variant_hash = variant_hash;
         if (first) {
            p_atomic_dec(&prog->base.reference.count);
            prog->default_variant_hash = prog->last_variant_hash;
         }
      }

      state->modules_changed = true;
   }
}

static uint32_t
hash_pipeline_lib(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct zink_rasterizer_hw_state));
}

static bool
equals_pipeline_lib(const void *a, const void *b)
{
   return !memcmp(a, b, offsetof(struct zink_gfx_library_key, pipeline));
}

static uint32_t
hash_gfx_pipeline_state(const void *key)
{
   const struct zink_gfx_pipeline_state *state = key;
   uint32_t hash = _mesa_hash_data(key, offsetof(struct zink_gfx_pipeline_state, hash));
   if (!state->have_EXT_extended_dynamic_state2)
      hash = XXH32(&state->dyn_state2, sizeof(state->dyn_state2), hash);
   if (state->have_EXT_extended_dynamic_state)
      return hash;
   return XXH32(&state->dyn_state1, sizeof(state->dyn_state1), hash);
}

static bool
equals_gfx_pipeline_state(const void *a, const void *b)
{
   const struct zink_gfx_pipeline_state *sa = a;
   const struct zink_gfx_pipeline_state *sb = b;
   if (sa->uses_dynamic_stride != sb->uses_dynamic_stride)
      return false;
   /* dynamic vs rp */
   if (!!sa->render_pass != !!sb->render_pass)
      return false;
   if (!sa->have_EXT_extended_dynamic_state || !sa->uses_dynamic_stride) {
      if (sa->vertex_buffers_enabled_mask != sb->vertex_buffers_enabled_mask)
         return false;
      /* if we don't have dynamic states, we have to hash the enabled vertex buffer bindings */
      uint32_t mask_a = sa->vertex_buffers_enabled_mask;
      uint32_t mask_b = sb->vertex_buffers_enabled_mask;
      while (mask_a || mask_b) {
         unsigned idx_a = u_bit_scan(&mask_a);
         unsigned idx_b = u_bit_scan(&mask_b);
         if (sa->vertex_strides[idx_a] != sb->vertex_strides[idx_b])
            return false;
      }
   }
   if (!sa->have_EXT_extended_dynamic_state) {
      if (memcmp(&sa->dyn_state1, &sb->dyn_state1, offsetof(struct zink_pipeline_dynamic_state1, depth_stencil_alpha_state)))
         return false;
      if (!!sa->dyn_state1.depth_stencil_alpha_state != !!sb->dyn_state1.depth_stencil_alpha_state ||
          (sa->dyn_state1.depth_stencil_alpha_state &&
           memcmp(sa->dyn_state1.depth_stencil_alpha_state, sb->dyn_state1.depth_stencil_alpha_state,
                  sizeof(struct zink_depth_stencil_alpha_hw_state))))
         return false;
   }
   if (!sa->have_EXT_extended_dynamic_state2) {
      if (memcmp(&sa->dyn_state2, &sb->dyn_state2, sizeof(sa->dyn_state2)))
         return false;
   } else if (!sa->extendedDynamicState2PatchControlPoints) {
      if (sa->dyn_state2.vertices_per_patch != sb->dyn_state2.vertices_per_patch)
         return false;
   }
   return !memcmp(sa->modules, sb->modules, sizeof(sa->modules)) &&
          !memcmp(a, b, offsetof(struct zink_gfx_pipeline_state, hash));
}


static uint32_t
hash_gfx_input_dynamic(const void *key)
{
   const struct zink_gfx_input_key *ikey = key;
   return ikey->idx;
}

static bool
equals_gfx_input_dynamic(const void *a, const void *b)
{
   const struct zink_gfx_input_key *ikey_a = a;
   const struct zink_gfx_input_key *ikey_b = b;
   return ikey_a->idx == ikey_b->idx;
}

static uint32_t
hash_gfx_input(const void *key)
{
   const struct zink_gfx_input_key *ikey = key;
   if (ikey->uses_dynamic_stride)
      return ikey->input;
   return _mesa_hash_data(key, offsetof(struct zink_gfx_input_key, pipeline));
}

static bool
equals_gfx_input(const void *a, const void *b)
{
   const struct zink_gfx_input_key *ikey_a = a;
   const struct zink_gfx_input_key *ikey_b = b;
   if (ikey_a->uses_dynamic_stride)
      return ikey_a->element_state == ikey_b->element_state &&
             !memcmp(a, b, offsetof(struct zink_gfx_input_key, vertex_buffers_enabled_mask));
   return !memcmp(a, b, offsetof(struct zink_gfx_input_key, pipeline));
}

static uint32_t
hash_gfx_output(const void *key)
{
   const uint8_t *data = key;
   const struct zink_gfx_output_key *okey = key;
   /* manually add in force_persample_interp */
   return okey->force_persample_interp ^
          _mesa_hash_data(data + sizeof(uint16_t), offsetof(struct zink_gfx_output_key, pipeline) - sizeof(uint16_t));
}

static bool
equals_gfx_output(const void *a, const void *b)
{
   const uint8_t *da = a;
   const uint8_t *db = b;
   return !memcmp(da + sizeof(uint16_t), db + sizeof(uint16_t), offsetof(struct zink_gfx_output_key, pipeline) - sizeof(uint16_t));
}

void
zink_update_gfx_program(struct zink_context *ctx, struct zink_gfx_program *prog)
{
   update_gfx_shader_modules(ctx, zink_screen(ctx->base.screen), prog, ctx->dirty_shader_stages & prog->stages_present, &ctx->gfx_pipeline_state);
}

static void
update_cs_shader_module(struct zink_context *ctx, struct zink_compute_program *comp)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_shader *zs = comp->shader;
   VkShaderModule mod;
   struct zink_shader_module *zm = NULL;
   unsigned inline_size = 0, nonseamless_size = 0;
   struct zink_shader_key *key = &ctx->compute_pipeline_state.key;

   if (ctx && zs->nir->info.num_inlinable_uniforms &&
       ctx->inlinable_uniforms_valid_mask & BITFIELD64_BIT(MESA_SHADER_COMPUTE)) {
      if (screen->is_cpu || comp->inlined_variant_count < ZINK_MAX_INLINED_VARIANTS)
         inline_size = zs->nir->info.num_inlinable_uniforms;
      else
         key->inline_uniforms = false;
   }
   if (key->base.nonseamless_cube_mask)
      nonseamless_size = sizeof(uint32_t);

   if (inline_size || nonseamless_size) {
      struct zink_shader_module *iter, *next;
      LIST_FOR_EACH_ENTRY_SAFE(iter, next, &comp->shader_cache[!!nonseamless_size], list) {
         if (!shader_key_matches(iter, false, key, inline_size))
            continue;
         list_delinit(&iter->list);
         zm = iter;
         break;
      }
   } else {
      zm = comp->module;
   }

   if (!zm) {
      zm = malloc(sizeof(struct zink_shader_module) + nonseamless_size + inline_size * sizeof(uint32_t));
      if (!zm) {
         return;
      }
      mod = zink_shader_compile(screen, zs, comp->shader->nir, key);
      if (!mod) {
         FREE(zm);
         return;
      }
      zm->shader = mod;
      list_inithead(&zm->list);
      zm->num_uniforms = inline_size;
      zm->key_size = 0;
      zm->has_nonseamless = !!nonseamless_size;
      assert(nonseamless_size || inline_size);
      if (nonseamless_size)
         memcpy(zm->key, &key->base.nonseamless_cube_mask, nonseamless_size);
      if (inline_size)
         memcpy(zm->key + nonseamless_size, key->base.inlined_uniform_values, inline_size * sizeof(uint32_t));
      zm->hash = shader_module_hash(zm);
      zm->default_variant = false;
      if (inline_size)
         comp->inlined_variant_count++;
   }
   if (zm->num_uniforms || nonseamless_size)
      list_add(&zm->list, &comp->shader_cache[!!nonseamless_size]);
   if (comp->curr == zm)
      return;
   ctx->compute_pipeline_state.final_hash ^= ctx->compute_pipeline_state.module_hash;
   comp->curr = zm;
   ctx->compute_pipeline_state.module_hash = zm->hash;
   ctx->compute_pipeline_state.final_hash ^= ctx->compute_pipeline_state.module_hash;
   ctx->compute_pipeline_state.module_changed = true;
}

void
zink_update_compute_program(struct zink_context *ctx)
{
   update_cs_shader_module(ctx, ctx->curr_compute);
}

VkPipelineLayout
zink_pipeline_layout_create(struct zink_screen *screen, struct zink_program *pg, uint32_t *compat)
{
   VkPipelineLayoutCreateInfo plci = {0};
   plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

   plci.pSetLayouts = pg->dsl;
   plci.setLayoutCount = pg->num_dsl;

   VkPushConstantRange pcr[2] = {0};
   if (pg->is_compute) {
      if (((struct zink_compute_program*)pg)->shader->nir->info.stage == MESA_SHADER_KERNEL) {
         pcr[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
         pcr[0].offset = 0;
         pcr[0].size = sizeof(struct zink_cs_push_constant);
         plci.pushConstantRangeCount = 1;
      }
   } else {
      pcr[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
      pcr[0].offset = offsetof(struct zink_gfx_push_constant, draw_mode_is_indexed);
      pcr[0].size = 2 * sizeof(unsigned);
      pcr[1].stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
      pcr[1].offset = offsetof(struct zink_gfx_push_constant, default_inner_level);
      pcr[1].size = sizeof(float) * 6;
      plci.pushConstantRangeCount = 2;
   }
   plci.pPushConstantRanges = &pcr[0];

   VkPipelineLayout layout;
   VkResult result = VKSCR(CreatePipelineLayout)(screen->dev, &plci, NULL, &layout);
   if (result != VK_SUCCESS) {
      mesa_loge("vkCreatePipelineLayout failed (%s)", vk_Result_to_str(result));
      return VK_NULL_HANDLE;
   }

   *compat = _mesa_hash_data(pg->dsl, pg->num_dsl * sizeof(pg->dsl[0]));

   return layout;
}

static void
assign_io(struct zink_gfx_program *prog, struct zink_shader *stages[ZINK_GFX_SHADER_COUNT])
{
   struct zink_shader *shaders[MESA_SHADER_STAGES];

   /* build array in pipeline order */
   for (unsigned i = 0; i < ZINK_GFX_SHADER_COUNT; i++)
      shaders[i] = stages[i];

   for (unsigned i = 0; i < MESA_SHADER_FRAGMENT;) {
      nir_shader *producer = shaders[i]->nir;
      for (unsigned j = i + 1; j < ZINK_GFX_SHADER_COUNT; i++, j++) {
         struct zink_shader *consumer = shaders[j];
         if (!consumer)
            continue;
         if (!prog->nir[producer->info.stage])
            prog->nir[producer->info.stage] = nir_shader_clone(prog, producer);
         if (!prog->nir[j])
            prog->nir[j] = nir_shader_clone(prog, consumer->nir);
         zink_compiler_assign_io(prog->nir[producer->info.stage], prog->nir[j]);
         i = j;
         break;
      }
   }
}

struct zink_gfx_program *
zink_create_gfx_program(struct zink_context *ctx,
                        struct zink_shader **stages,
                        unsigned vertices_per_patch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_gfx_program *prog = rzalloc(NULL, struct zink_gfx_program);
   if (!prog)
      goto fail;

   pipe_reference_init(&prog->base.reference, 1);
   util_queue_fence_init(&prog->base.cache_fence);

   for (int i = 0; i < ZINK_GFX_SHADER_COUNT; ++i) {
      list_inithead(&prog->shader_cache[i][0][0]);
      list_inithead(&prog->shader_cache[i][0][1]);
      list_inithead(&prog->shader_cache[i][1][0]);
      list_inithead(&prog->shader_cache[i][1][1]);
      if (stages[i]) {
         prog->shaders[i] = stages[i];
         prog->stages_present |= BITFIELD_BIT(i);
      }
   }
   if (stages[MESA_SHADER_TESS_EVAL] && !stages[MESA_SHADER_TESS_CTRL]) {
      prog->shaders[MESA_SHADER_TESS_EVAL]->generated =
      prog->shaders[MESA_SHADER_TESS_CTRL] =
        zink_shader_tcs_create(screen, stages[MESA_SHADER_VERTEX], vertices_per_patch);
      prog->stages_present |= BITFIELD_BIT(MESA_SHADER_TESS_CTRL);
   }

   assign_io(prog, prog->shaders);

   if (stages[MESA_SHADER_GEOMETRY])
      prog->last_vertex_stage = stages[MESA_SHADER_GEOMETRY];
   else if (stages[MESA_SHADER_TESS_EVAL])
      prog->last_vertex_stage = stages[MESA_SHADER_TESS_EVAL];
   else
      prog->last_vertex_stage = stages[MESA_SHADER_VERTEX];

   for (int i = 0; i < ARRAY_SIZE(prog->pipelines); ++i) {
      _mesa_hash_table_init(&prog->pipelines[i], prog, NULL, equals_gfx_pipeline_state);
      /* only need first 3/4 for point/line/tri/patch */
      if (screen->info.have_EXT_extended_dynamic_state &&
          i == (prog->last_vertex_stage->nir->info.stage == MESA_SHADER_TESS_EVAL ? 4 : 3))
         break;
   }

   for (unsigned i = 0; i < ARRAY_SIZE(prog->libs); i++)
      _mesa_set_init(&prog->libs[i], prog, hash_pipeline_lib, equals_pipeline_lib);

   struct mesa_sha1 sctx;
   _mesa_sha1_init(&sctx);
   for (int i = 0; i < ZINK_GFX_SHADER_COUNT; ++i) {
      if (prog->shaders[i]) {
         simple_mtx_lock(&prog->shaders[i]->lock);
         _mesa_set_add(prog->shaders[i]->programs, prog);
         simple_mtx_unlock(&prog->shaders[i]->lock);
         zink_gfx_program_reference(ctx, NULL, prog);
         _mesa_sha1_update(&sctx, prog->shaders[i]->base.sha1, sizeof(prog->shaders[i]->base.sha1));
      }
   }
   _mesa_sha1_final(&sctx, prog->base.sha1);

   if (!zink_descriptor_program_init(ctx, &prog->base))
      goto fail;

   zink_screen_get_pipeline_cache(screen, &prog->base);
   return prog;

fail:
   if (prog)
      zink_destroy_gfx_program(ctx, prog);
   return NULL;
}

static uint32_t
hash_compute_pipeline_state(const void *key)
{
   const struct zink_compute_pipeline_state *state = key;
   uint32_t hash = _mesa_hash_data(state, offsetof(struct zink_compute_pipeline_state, hash));
   if (state->use_local_size)
      hash = XXH32(&state->local_size[0], sizeof(state->local_size), hash);
   return hash;
}

void
zink_program_update_compute_pipeline_state(struct zink_context *ctx, struct zink_compute_program *comp, const uint block[3])
{
   struct zink_shader *zs = comp->shader;
   bool use_local_size = !(zs->nir->info.workgroup_size[0] ||
                           zs->nir->info.workgroup_size[1] ||
                           zs->nir->info.workgroup_size[2]);
   if (ctx->compute_pipeline_state.use_local_size != use_local_size)
      ctx->compute_pipeline_state.dirty = true;
   ctx->compute_pipeline_state.use_local_size = use_local_size;

   if (ctx->compute_pipeline_state.use_local_size) {
      for (int i = 0; i < ARRAY_SIZE(ctx->compute_pipeline_state.local_size); i++) {
         if (ctx->compute_pipeline_state.local_size[i] != block[i])
            ctx->compute_pipeline_state.dirty = true;
         ctx->compute_pipeline_state.local_size[i] = block[i];
      }
   } else
      ctx->compute_pipeline_state.local_size[0] =
      ctx->compute_pipeline_state.local_size[1] =
      ctx->compute_pipeline_state.local_size[2] = 0;
}

static bool
equals_compute_pipeline_state(const void *a, const void *b)
{
   const struct zink_compute_pipeline_state *sa = a;
   const struct zink_compute_pipeline_state *sb = b;
   return !memcmp(a, b, offsetof(struct zink_compute_pipeline_state, hash)) &&
          sa->module == sb->module;
}

struct zink_compute_program *
zink_create_compute_program(struct zink_context *ctx, struct zink_shader *shader)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_compute_program *comp = rzalloc(NULL, struct zink_compute_program);
   if (!comp)
      goto fail;

   pipe_reference_init(&comp->base.reference, 1);
   util_queue_fence_init(&comp->base.cache_fence);
   comp->base.is_compute = true;

   comp->curr = comp->module = CALLOC_STRUCT(zink_shader_module);
   assert(comp->module);
   comp->module->shader = zink_shader_compile(screen, shader, shader->nir, NULL);
   assert(comp->module->shader);
   list_inithead(&comp->shader_cache[0]);
   list_inithead(&comp->shader_cache[1]);

   comp->pipelines = _mesa_hash_table_create(NULL, NULL,
                                             equals_compute_pipeline_state);

   _mesa_set_add(shader->programs, comp);
   comp->shader = shader;
   memcpy(comp->base.sha1, shader->base.sha1, sizeof(shader->base.sha1));

   if (!zink_descriptor_program_init(ctx, &comp->base))
      goto fail;

   zink_screen_get_pipeline_cache(screen, &comp->base);
   return comp;

fail:
   if (comp)
      zink_destroy_compute_program(ctx, comp);
   return NULL;
}

uint32_t
zink_program_get_descriptor_usage(struct zink_context *ctx, gl_shader_stage stage, enum zink_descriptor_type type)
{
   struct zink_shader *zs = NULL;
   switch (stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
   case MESA_SHADER_FRAGMENT:
      zs = ctx->gfx_stages[stage];
      break;
   case MESA_SHADER_COMPUTE: {
      zs = ctx->compute_stage;
      break;
   }
   default:
      unreachable("unknown shader type");
   }
   if (!zs)
      return 0;
   switch (type) {
   case ZINK_DESCRIPTOR_TYPE_UBO:
      return zs->ubos_used;
   case ZINK_DESCRIPTOR_TYPE_SSBO:
      return zs->ssbos_used;
   case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
      return BITSET_TEST_RANGE(zs->nir->info.textures_used, 0, PIPE_MAX_SAMPLERS - 1);
   case ZINK_DESCRIPTOR_TYPE_IMAGE:
      return BITSET_TEST_RANGE(zs->nir->info.images_used, 0, PIPE_MAX_SAMPLERS - 1);
   default:
      unreachable("unknown descriptor type!");
   }
   return 0;
}

bool
zink_program_descriptor_is_buffer(struct zink_context *ctx, gl_shader_stage stage, enum zink_descriptor_type type, unsigned i)
{
   struct zink_shader *zs = NULL;
   switch (stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
   case MESA_SHADER_GEOMETRY:
   case MESA_SHADER_FRAGMENT:
      zs = ctx->gfx_stages[stage];
      break;
   case MESA_SHADER_COMPUTE: {
      zs = ctx->compute_stage;
      break;
   }
   default:
      unreachable("unknown shader type");
   }
   if (!zs)
      return false;
   return zink_shader_descriptor_is_buffer(zs, type, i);
}

static unsigned
get_num_bindings(struct zink_shader *zs, enum zink_descriptor_type type)
{
   switch (type) {
   case ZINK_DESCRIPTOR_TYPE_UBO:
   case ZINK_DESCRIPTOR_TYPE_SSBO:
      return zs->num_bindings[type];
   default:
      break;
   }
   unsigned num_bindings = 0;
   for (int i = 0; i < zs->num_bindings[type]; i++)
      num_bindings += zs->bindings[type][i].size;
   return num_bindings;
}

unsigned
zink_program_num_bindings_typed(const struct zink_program *pg, enum zink_descriptor_type type, bool is_compute)
{
   unsigned num_bindings = 0;
   if (is_compute) {
      struct zink_compute_program *comp = (void*)pg;
      return get_num_bindings(comp->shader, type);
   }
   struct zink_gfx_program *prog = (void*)pg;
   for (unsigned i = 0; i < ZINK_GFX_SHADER_COUNT; i++) {
      if (prog->shaders[i])
         num_bindings += get_num_bindings(prog->shaders[i], type);
   }
   return num_bindings;
}

unsigned
zink_program_num_bindings(const struct zink_program *pg, bool is_compute)
{
   unsigned num_bindings = 0;
   for (unsigned i = 0; i < ZINK_DESCRIPTOR_TYPES; i++)
      num_bindings += zink_program_num_bindings_typed(pg, i, is_compute);
   return num_bindings;
}

void
zink_destroy_gfx_program(struct zink_context *ctx,
                         struct zink_gfx_program *prog)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   util_queue_fence_wait(&prog->base.cache_fence);
   if (prog->base.layout)
      VKSCR(DestroyPipelineLayout)(screen->dev, prog->base.layout, NULL);

   for (int i = 0; i < ZINK_GFX_SHADER_COUNT; ++i) {
      if (prog->shaders[i]) {
         _mesa_set_remove_key(prog->shaders[i]->programs, prog);
         prog->shaders[i] = NULL;
      }
      destroy_shader_cache(screen, &prog->shader_cache[i][0][0]);
      destroy_shader_cache(screen, &prog->shader_cache[i][0][1]);
      destroy_shader_cache(screen, &prog->shader_cache[i][1][0]);
      destroy_shader_cache(screen, &prog->shader_cache[i][1][1]);
      ralloc_free(prog->nir[i]);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(prog->libs); i++) {
      set_foreach_remove(&prog->libs[i], he) {
         struct zink_gfx_library_key *gkey = (void*)he->key;
         VKSCR(DestroyPipeline)(screen->dev, gkey->pipeline, NULL);
      }
   }

   unsigned max_idx = ARRAY_SIZE(prog->pipelines);
   if (screen->info.have_EXT_extended_dynamic_state) {
      /* only need first 3/4 for point/line/tri/patch */
      if ((prog->stages_present &
          (BITFIELD_BIT(MESA_SHADER_TESS_EVAL) | BITFIELD_BIT(MESA_SHADER_GEOMETRY))) ==
          BITFIELD_BIT(MESA_SHADER_TESS_EVAL))
         max_idx = 4;
      else
         max_idx = 3;
      max_idx++;
   }

   for (int i = 0; i < max_idx; ++i) {
      hash_table_foreach(&prog->pipelines[i], entry) {
         struct gfx_pipeline_cache_entry *pc_entry = entry->data;

         VKSCR(DestroyPipeline)(screen->dev, pc_entry->pipeline, NULL);
         free(pc_entry);
      }
   }
   if (prog->base.pipeline_cache)
      VKSCR(DestroyPipelineCache)(screen->dev, prog->base.pipeline_cache, NULL);
   zink_descriptor_program_deinit(ctx, &prog->base);

   ralloc_free(prog);
}

void
zink_destroy_compute_program(struct zink_context *ctx,
                             struct zink_compute_program *comp)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   util_queue_fence_wait(&comp->base.cache_fence);
   if (comp->base.layout)
      VKSCR(DestroyPipelineLayout)(screen->dev, comp->base.layout, NULL);

   if (comp->shader)
      _mesa_set_remove_key(comp->shader->programs, comp);
   destroy_shader_cache(screen, &comp->shader_cache[0]);
   destroy_shader_cache(screen, &comp->shader_cache[1]);

   hash_table_foreach(comp->pipelines, entry) {
      struct compute_pipeline_cache_entry *pc_entry = entry->data;

      VKSCR(DestroyPipeline)(screen->dev, pc_entry->pipeline, NULL);
      free(pc_entry);
   }
   _mesa_hash_table_destroy(comp->pipelines, NULL);
   VKSCR(DestroyShaderModule)(screen->dev, comp->module->shader, NULL);
   free(comp->module);
   if (comp->base.pipeline_cache)
      VKSCR(DestroyPipelineCache)(screen->dev, comp->base.pipeline_cache, NULL);
   zink_descriptor_program_deinit(ctx, &comp->base);

   ralloc_free(comp);
}

static unsigned
get_primtype_idx(enum pipe_prim_type mode)
{
   if (mode == PIPE_PRIM_PATCHES)
      return 3;
   switch (u_reduced_prim(mode)) {
   case PIPE_PRIM_POINTS:
      return 0;
   case PIPE_PRIM_LINES:
      return 1;
   default:
      return 2;
   }
}

static void
create_pipeline_lib(struct zink_screen *screen, struct zink_gfx_program *prog, struct zink_gfx_pipeline_state *state, enum pipe_prim_type mode)
{
   struct zink_gfx_library_key *gkey = rzalloc(prog, struct zink_gfx_library_key);
   gkey->hw_rast_state = state->rast_state;
   memcpy(gkey->modules, state->modules, sizeof(gkey->modules));
   bool line = u_reduced_prim(mode) == PIPE_PRIM_LINES;
   gkey->pipeline = zink_create_gfx_pipeline_library(screen, prog, (struct zink_rasterizer_hw_state*)state, line);
   _mesa_set_add(&prog->libs[get_primtype_idx(mode)], gkey);
}

static unsigned
get_pipeline_idx(bool have_EXT_extended_dynamic_state, enum pipe_prim_type mode, VkPrimitiveTopology vkmode)
{
   /* VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY specifies that the topology state in
    * VkPipelineInputAssemblyStateCreateInfo only specifies the topology class,
    * and the specific topology order and adjacency must be set dynamically
    * with vkCmdSetPrimitiveTopology before any drawing commands.
    */
   if (have_EXT_extended_dynamic_state) {
      return get_primtype_idx(mode);
   }
   return vkmode;
}

static struct zink_gfx_input_key *
find_or_create_input_dynamic(struct zink_context *ctx, VkPrimitiveTopology vkmode)
{
   uint32_t hash = hash_gfx_input_dynamic(&ctx->gfx_pipeline_state.input);
   struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->gfx_inputs, hash, &ctx->gfx_pipeline_state.input);
   if (!he) {
      struct zink_gfx_input_key *ikey = rzalloc(ctx, struct zink_gfx_input_key);
      ikey->idx = ctx->gfx_pipeline_state.idx;
      ikey->pipeline = zink_create_gfx_pipeline_input(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state, ctx->element_state->binding_map, vkmode);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_inputs, hash, ikey);
   }
   return (void*)he->key;
}

static struct zink_gfx_input_key *
find_or_create_input(struct zink_context *ctx, VkPrimitiveTopology vkmode)
{
   uint32_t hash = hash_gfx_input(&ctx->gfx_pipeline_state.input);
   struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->gfx_inputs, hash, &ctx->gfx_pipeline_state.input);
   if (!he) {
      struct zink_gfx_input_key *ikey = rzalloc(ctx, struct zink_gfx_input_key);
      if (ctx->gfx_pipeline_state.uses_dynamic_stride) {
         memcpy(ikey, &ctx->gfx_pipeline_state.input, offsetof(struct zink_gfx_input_key, vertex_buffers_enabled_mask));
         ikey->element_state = ctx->gfx_pipeline_state.element_state;
      } else {
         memcpy(ikey, &ctx->gfx_pipeline_state.input, offsetof(struct zink_gfx_input_key, pipeline));
      }
      ikey->pipeline = zink_create_gfx_pipeline_input(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state, ctx->element_state->binding_map, vkmode);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_inputs, hash, ikey);
   }
   return (void*)he->key;
}

static struct zink_gfx_output_key *
find_or_create_output(struct zink_context *ctx)
{
   uint32_t hash = hash_gfx_output(&ctx->gfx_pipeline_state);
   struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->gfx_outputs, hash, &ctx->gfx_pipeline_state);
   if (!he) {
      struct zink_gfx_output_key *okey = rzalloc(ctx, struct zink_gfx_output_key);
      memcpy(okey, &ctx->gfx_pipeline_state, offsetof(struct zink_gfx_output_key, pipeline));
      okey->_pad = 0;
      okey->pipeline = zink_create_gfx_pipeline_output(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_outputs, hash, okey);
   }
   return (void*)he->key;
}

/*
   VUID-vkCmdBindVertexBuffers2-pStrides-06209
   If pStrides is not NULL each element of pStrides must be either 0 or greater than or equal
   to the maximum extent of all vertex input attributes fetched from the corresponding
   binding, where the extent is calculated as the VkVertexInputAttributeDescription::offset
   plus VkVertexInputAttributeDescription::format size

   * thus, if the stride doesn't meet the minimum requirement for a binding,
   * disable the dynamic state here and use a fully-baked pipeline
 */
static bool
check_vertex_strides(struct zink_context *ctx)
{
   const struct zink_vertex_elements_state *ves = ctx->element_state;
   for (unsigned i = 0; i < ves->hw_state.num_bindings; i++) {
      const struct pipe_vertex_buffer *vb = ctx->vertex_buffers + ves->binding_map[i];
      unsigned stride = vb->buffer.resource ? vb->stride : 0;
      if (stride && stride < ves->min_stride[i])
         return false;
   }
   return true;
}

VkPipeline
zink_get_gfx_pipeline(struct zink_context *ctx,
                      struct zink_gfx_program *prog,
                      struct zink_gfx_pipeline_state *state,
                      enum pipe_prim_type mode)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   const bool have_EXT_vertex_input_dynamic_state = screen->info.have_EXT_vertex_input_dynamic_state;
   const bool have_EXT_extended_dynamic_state = screen->info.have_EXT_extended_dynamic_state;
   bool uses_dynamic_stride = state->uses_dynamic_stride;

   VkPrimitiveTopology vkmode = zink_primitive_topology(mode);
   const unsigned idx = get_pipeline_idx(screen->info.have_EXT_extended_dynamic_state, mode, vkmode);
   assert(idx <= ARRAY_SIZE(prog->pipelines));
   if (!state->dirty && !state->modules_changed &&
       (have_EXT_vertex_input_dynamic_state || !ctx->vertex_state_changed) &&
       idx == state->idx)
      return state->pipeline;

   struct hash_entry *entry = NULL;

   if (state->dirty) {
      if (state->pipeline) //avoid on first hash
         state->final_hash ^= state->hash;
      state->hash = hash_gfx_pipeline_state(state);
      state->final_hash ^= state->hash;
      state->dirty = false;
   }
   if (!have_EXT_vertex_input_dynamic_state && ctx->vertex_state_changed) {
      if (state->pipeline)
         state->final_hash ^= state->vertex_hash;
      if (have_EXT_extended_dynamic_state)
         uses_dynamic_stride = check_vertex_strides(ctx);
      if (!uses_dynamic_stride) {
         uint32_t hash = 0;
         /* if we don't have dynamic states, we have to hash the enabled vertex buffer bindings */
         uint32_t vertex_buffers_enabled_mask = state->vertex_buffers_enabled_mask;
         hash = XXH32(&vertex_buffers_enabled_mask, sizeof(uint32_t), hash);

         for (unsigned i = 0; i < state->element_state->num_bindings; i++) {
            const unsigned buffer_id = ctx->element_state->binding_map[i];
            struct pipe_vertex_buffer *vb = ctx->vertex_buffers + buffer_id;
            state->vertex_strides[buffer_id] = vb->buffer.resource ? vb->stride : 0;
            hash = XXH32(&state->vertex_strides[buffer_id], sizeof(uint32_t), hash);
         }
         state->vertex_hash = hash ^ state->element_state->hash;
      } else
         state->vertex_hash = state->element_state->hash;
      state->final_hash ^= state->vertex_hash;
   }
   state->modules_changed = false;
   state->uses_dynamic_stride = uses_dynamic_stride;
   state->idx = idx;
   ctx->vertex_state_changed = false;

   entry = _mesa_hash_table_search_pre_hashed(&prog->pipelines[idx], state->final_hash, state);

   if (!entry) {
      util_queue_fence_wait(&prog->base.cache_fence);
      VkPipeline pipeline = VK_NULL_HANDLE;
      if (screen->info.have_EXT_graphics_pipeline_library &&
          /* TODO: if there's ever a dynamic render extension with input attachments */
          !ctx->gfx_pipeline_state.render_pass) {
         ctx->gfx_pipeline_state.gkey = ctx->gfx_pipeline_state.rast_state;
         struct set_entry *he = NULL;
         /* TODO: this will eventually be pre-populated by async shader compile */
         //struct set_entry *he = _mesa_set_search(&prog->libs[idx], &ctx->gfx_pipeline_state.gkey);
         if (!he && screen->driver_workarounds.force_pipeline_library) {
            create_pipeline_lib(screen, prog, &ctx->gfx_pipeline_state, mode);
            he = _mesa_set_search(&prog->libs[idx], &ctx->gfx_pipeline_state.gkey);
            assert(he);
         }
         if (he) {
            struct zink_gfx_library_key *gkey = (void*)he->key;
            struct zink_gfx_input_key *ikey = have_EXT_vertex_input_dynamic_state ?
                                              find_or_create_input_dynamic(ctx, vkmode) :
                                              find_or_create_input(ctx, vkmode);
            struct zink_gfx_output_key *okey = find_or_create_output(ctx);
            pipeline = zink_create_gfx_pipeline_combined(screen, prog, ikey->pipeline, gkey->pipeline, okey->pipeline);
         }
      }
      if (!pipeline) {
         pipeline = zink_create_gfx_pipeline(screen, prog, state,
                                             ctx->element_state->binding_map,
                                             vkmode);
      }
      if (pipeline == VK_NULL_HANDLE)
         return VK_NULL_HANDLE;

      zink_screen_update_pipeline_cache(screen, &prog->base);
      struct gfx_pipeline_cache_entry *pc_entry = CALLOC_STRUCT(gfx_pipeline_cache_entry);
      if (!pc_entry)
         return VK_NULL_HANDLE;

      memcpy(&pc_entry->state, state, sizeof(*state));
      pc_entry->pipeline = pipeline;

      entry = _mesa_hash_table_insert_pre_hashed(&prog->pipelines[idx], state->final_hash, pc_entry, pc_entry);
      assert(entry);
   }

   struct gfx_pipeline_cache_entry *cache_entry = entry->data;
   state->pipeline = cache_entry->pipeline;
   return state->pipeline;
}

VkPipeline
zink_get_compute_pipeline(struct zink_screen *screen,
                      struct zink_compute_program *comp,
                      struct zink_compute_pipeline_state *state)
{
   struct hash_entry *entry = NULL;

   if (!state->dirty && !state->module_changed)
      return state->pipeline;
   if (state->dirty) {
      if (state->pipeline) //avoid on first hash
         state->final_hash ^= state->hash;
      state->hash = hash_compute_pipeline_state(state);
      state->dirty = false;
      state->final_hash ^= state->hash;
   }
   entry = _mesa_hash_table_search_pre_hashed(comp->pipelines, state->final_hash, state);

   if (!entry) {
      util_queue_fence_wait(&comp->base.cache_fence);
      VkPipeline pipeline = zink_create_compute_pipeline(screen, comp, state);

      if (pipeline == VK_NULL_HANDLE)
         return VK_NULL_HANDLE;

      struct compute_pipeline_cache_entry *pc_entry = CALLOC_STRUCT(compute_pipeline_cache_entry);
      if (!pc_entry)
         return VK_NULL_HANDLE;

      memcpy(&pc_entry->state, state, sizeof(*state));
      pc_entry->pipeline = pipeline;

      entry = _mesa_hash_table_insert_pre_hashed(comp->pipelines, state->final_hash, pc_entry, pc_entry);
      assert(entry);
   }

   struct compute_pipeline_cache_entry *cache_entry = entry->data;
   state->pipeline = cache_entry->pipeline;
   return state->pipeline;
}

static inline void
bind_stage(struct zink_context *ctx, gl_shader_stage stage,
           struct zink_shader *shader)
{
   if (shader && shader->nir->info.num_inlinable_uniforms)
      ctx->shader_has_inlinable_uniforms_mask |= 1 << stage;
   else
      ctx->shader_has_inlinable_uniforms_mask &= ~(1 << stage);

   if (stage == MESA_SHADER_COMPUTE) {
      if (ctx->compute_stage) {
         ctx->compute_pipeline_state.final_hash ^= ctx->compute_pipeline_state.module_hash;
         ctx->compute_pipeline_state.module = VK_NULL_HANDLE;
         ctx->compute_pipeline_state.module_hash = 0;
      }
      if (shader && shader != ctx->compute_stage) {
         struct hash_entry *entry = _mesa_hash_table_search(&ctx->compute_program_cache, shader);
         if (entry) {
            ctx->compute_pipeline_state.dirty = true;
            ctx->curr_compute = entry->data;
         } else {
            struct zink_compute_program *comp = zink_create_compute_program(ctx, shader);
            _mesa_hash_table_insert(&ctx->compute_program_cache, comp->shader, comp);
            ctx->compute_pipeline_state.dirty = true;
            ctx->curr_compute = comp;
            zink_batch_reference_program(&ctx->batch, &ctx->curr_compute->base);
         }
         ctx->compute_pipeline_state.module_hash = ctx->curr_compute->curr->hash;
         ctx->compute_pipeline_state.module = ctx->curr_compute->curr->shader;
         ctx->compute_pipeline_state.final_hash ^= ctx->compute_pipeline_state.module_hash;
         if (ctx->compute_pipeline_state.key.base.nonseamless_cube_mask)
            ctx->dirty_shader_stages |= BITFIELD_BIT(MESA_SHADER_COMPUTE);
      } else if (!shader)
         ctx->curr_compute = NULL;
      ctx->compute_stage = shader;
      zink_select_launch_grid(ctx);
   } else {
      if (ctx->gfx_stages[stage])
         ctx->gfx_hash ^= ctx->gfx_stages[stage]->hash;
      ctx->gfx_stages[stage] = shader;
      ctx->gfx_dirty = ctx->gfx_stages[MESA_SHADER_FRAGMENT] && ctx->gfx_stages[MESA_SHADER_VERTEX];
      ctx->gfx_pipeline_state.modules_changed = true;
      if (shader) {
         ctx->shader_stages |= BITFIELD_BIT(stage);
         ctx->gfx_hash ^= ctx->gfx_stages[stage]->hash;
      } else {
         ctx->gfx_pipeline_state.modules[stage] = VK_NULL_HANDLE;
         if (ctx->curr_program)
            ctx->gfx_pipeline_state.final_hash ^= ctx->curr_program->last_variant_hash;
         ctx->curr_program = NULL;
         ctx->shader_stages &= ~BITFIELD_BIT(stage);
      }
   }
}

static void
bind_last_vertex_stage(struct zink_context *ctx)
{
   gl_shader_stage old = ctx->last_vertex_stage ? ctx->last_vertex_stage->nir->info.stage : MESA_SHADER_STAGES;
   if (ctx->gfx_stages[MESA_SHADER_GEOMETRY])
      ctx->last_vertex_stage = ctx->gfx_stages[MESA_SHADER_GEOMETRY];
   else if (ctx->gfx_stages[MESA_SHADER_TESS_EVAL])
      ctx->last_vertex_stage = ctx->gfx_stages[MESA_SHADER_TESS_EVAL];
   else
      ctx->last_vertex_stage = ctx->gfx_stages[MESA_SHADER_VERTEX];
   gl_shader_stage current = ctx->last_vertex_stage ? ctx->last_vertex_stage->nir->info.stage : MESA_SHADER_VERTEX;
   if (old != current) {
      if (old != MESA_SHADER_STAGES) {
         memset(&ctx->gfx_pipeline_state.shader_keys.key[old].key.vs_base, 0, sizeof(struct zink_vs_key_base));
         ctx->dirty_shader_stages |= BITFIELD_BIT(old);
      } else {
         /* always unset vertex shader values when changing to a non-vs last stage */
         memset(&ctx->gfx_pipeline_state.shader_keys.key[MESA_SHADER_VERTEX].key.vs_base, 0, sizeof(struct zink_vs_key_base));
      }

      unsigned num_viewports = ctx->vp_state.num_viewports;
      struct zink_screen *screen = zink_screen(ctx->base.screen);
      /* number of enabled viewports is based on whether last vertex stage writes viewport index */
      if (ctx->last_vertex_stage) {
         if (ctx->last_vertex_stage->nir->info.outputs_written & (VARYING_BIT_VIEWPORT | VARYING_BIT_VIEWPORT_MASK))
            ctx->vp_state.num_viewports = MIN2(screen->info.props.limits.maxViewports, PIPE_MAX_VIEWPORTS);
         else
            ctx->vp_state.num_viewports = 1;
      } else {
         ctx->vp_state.num_viewports = 1;
      }
      ctx->vp_state_changed |= num_viewports != ctx->vp_state.num_viewports;
      if (!screen->info.have_EXT_extended_dynamic_state) {
         if (ctx->gfx_pipeline_state.dyn_state1.num_viewports != ctx->vp_state.num_viewports)
            ctx->gfx_pipeline_state.dirty = true;
         ctx->gfx_pipeline_state.dyn_state1.num_viewports = ctx->vp_state.num_viewports;
      }
      ctx->last_vertex_stage_dirty = true;
   }
}

static void
zink_bind_vs_state(struct pipe_context *pctx,
                   void *cso)
{
   struct zink_context *ctx = zink_context(pctx);
   if (!cso && !ctx->gfx_stages[MESA_SHADER_VERTEX])
      return;
   bind_stage(ctx, MESA_SHADER_VERTEX, cso);
   bind_last_vertex_stage(ctx);
   if (cso) {
      struct zink_shader *zs = cso;
      ctx->shader_reads_drawid = BITSET_TEST(zs->nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID);
      ctx->shader_reads_basevertex = BITSET_TEST(zs->nir->info.system_values_read, SYSTEM_VALUE_BASE_VERTEX);
   } else {
      ctx->shader_reads_drawid = false;
      ctx->shader_reads_basevertex = false;
   }
}

/* if gl_SampleMask[] is written to, we have to ensure that we get a shader with the same sample count:
 * in GL, samples==1 means ignore gl_SampleMask[]
 * in VK, gl_SampleMask[] is never ignored
 */
void
zink_update_fs_key_samples(struct zink_context *ctx)
{
   if (!ctx->gfx_stages[MESA_SHADER_FRAGMENT])
      return;
   nir_shader *nir = ctx->gfx_stages[MESA_SHADER_FRAGMENT]->nir;
   if (nir->info.outputs_written & (1 << FRAG_RESULT_SAMPLE_MASK)) {
      bool samples = zink_get_fs_key(ctx)->samples;
      if (samples != (ctx->fb_state.samples > 1))
         zink_set_fs_key(ctx)->samples = ctx->fb_state.samples > 1;
   }
}

static void
zink_bind_fs_state(struct pipe_context *pctx,
                   void *cso)
{
   struct zink_context *ctx = zink_context(pctx);
   if (!cso && !ctx->gfx_stages[MESA_SHADER_FRAGMENT])
      return;
   bind_stage(ctx, MESA_SHADER_FRAGMENT, cso);
   ctx->fbfetch_outputs = 0;
   if (cso) {
      nir_shader *nir = ctx->gfx_stages[MESA_SHADER_FRAGMENT]->nir;
      if (nir->info.fs.uses_fbfetch_output) {
         nir_foreach_shader_out_variable(var, ctx->gfx_stages[MESA_SHADER_FRAGMENT]->nir) {
            if (var->data.fb_fetch_output)
               ctx->fbfetch_outputs |= BITFIELD_BIT(var->data.location - FRAG_RESULT_DATA0);
         }
      }
      zink_update_fs_key_samples(ctx);
   }
   zink_update_fbfetch(ctx);
}

static void
zink_bind_gs_state(struct pipe_context *pctx,
                   void *cso)
{
   struct zink_context *ctx = zink_context(pctx);
   if (!cso && !ctx->gfx_stages[MESA_SHADER_GEOMETRY])
      return;
   bool had_points = ctx->gfx_stages[MESA_SHADER_GEOMETRY] ? ctx->gfx_stages[MESA_SHADER_GEOMETRY]->nir->info.gs.output_primitive == SHADER_PRIM_POINTS : false;
   bind_stage(ctx, MESA_SHADER_GEOMETRY, cso);
   bind_last_vertex_stage(ctx);
   if (cso) {
      if (!had_points && ctx->last_vertex_stage->nir->info.gs.output_primitive == SHADER_PRIM_POINTS)
         ctx->gfx_pipeline_state.has_points++;
   } else {
      if (had_points)
         ctx->gfx_pipeline_state.has_points--;
   }
}

static void
zink_bind_tcs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), MESA_SHADER_TESS_CTRL, cso);
}

static void
zink_bind_tes_state(struct pipe_context *pctx,
                   void *cso)
{
   struct zink_context *ctx = zink_context(pctx);
   if (!cso && !ctx->gfx_stages[MESA_SHADER_TESS_EVAL])
      return;
   if (!!ctx->gfx_stages[MESA_SHADER_TESS_EVAL] != !!cso) {
      if (!cso) {
         /* if unsetting a TESS that uses a generated TCS, ensure the TCS is unset */
         if (ctx->gfx_stages[MESA_SHADER_TESS_EVAL]->generated)
            ctx->gfx_stages[MESA_SHADER_TESS_CTRL] = NULL;
      }
   }
   bind_stage(ctx, MESA_SHADER_TESS_EVAL, cso);
   bind_last_vertex_stage(ctx);
}

static void *
zink_create_cs_state(struct pipe_context *pctx,
                     const struct pipe_compute_state *shader)
{
   struct nir_shader *nir;
   if (shader->ir_type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->prog);
   else
      nir = (struct nir_shader *)shader->prog;

   return zink_shader_create(zink_screen(pctx->screen), nir, NULL);
}

static void
zink_bind_cs_state(struct pipe_context *pctx,
                   void *cso)
{
   bind_stage(zink_context(pctx), MESA_SHADER_COMPUTE, cso);
}

void
zink_delete_shader_state(struct pipe_context *pctx, void *cso)
{
   zink_shader_free(zink_context(pctx), cso);
}

void *
zink_create_gfx_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *shader)
{
   nir_shader *nir;
   if (shader->type != PIPE_SHADER_IR_NIR)
      nir = zink_tgsi_to_nir(pctx->screen, shader->tokens);
   else
      nir = (struct nir_shader *)shader->ir.nir;

   return zink_shader_create(zink_screen(pctx->screen), nir, &shader->stream_output);
}

static void
zink_delete_cached_shader_state(struct pipe_context *pctx, void *cso)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   util_shader_reference(pctx, &screen->shaders, &cso, NULL);
}

static void *
zink_create_cached_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *shader)
{
   bool cache_hit;
   struct zink_screen *screen = zink_screen(pctx->screen);
   return util_live_shader_cache_get(pctx, &screen->shaders, shader, &cache_hit);
}

void
zink_program_init(struct zink_context *ctx)
{
   ctx->base.create_vs_state = zink_create_cached_shader_state;
   ctx->base.bind_vs_state = zink_bind_vs_state;
   ctx->base.delete_vs_state = zink_delete_cached_shader_state;

   ctx->base.create_fs_state = zink_create_cached_shader_state;
   ctx->base.bind_fs_state = zink_bind_fs_state;
   ctx->base.delete_fs_state = zink_delete_cached_shader_state;

   ctx->base.create_gs_state = zink_create_cached_shader_state;
   ctx->base.bind_gs_state = zink_bind_gs_state;
   ctx->base.delete_gs_state = zink_delete_cached_shader_state;

   ctx->base.create_tcs_state = zink_create_cached_shader_state;
   ctx->base.bind_tcs_state = zink_bind_tcs_state;
   ctx->base.delete_tcs_state = zink_delete_cached_shader_state;

   ctx->base.create_tes_state = zink_create_cached_shader_state;
   ctx->base.bind_tes_state = zink_bind_tes_state;
   ctx->base.delete_tes_state = zink_delete_cached_shader_state;

   ctx->base.create_compute_state = zink_create_cs_state;
   ctx->base.bind_compute_state = zink_bind_cs_state;
   ctx->base.delete_compute_state = zink_delete_shader_state;

   if (zink_screen(ctx->base.screen)->info.have_EXT_vertex_input_dynamic_state)
      _mesa_set_init(&ctx->gfx_inputs, ctx, hash_gfx_input_dynamic, equals_gfx_input_dynamic);
   else
      _mesa_set_init(&ctx->gfx_inputs, ctx, hash_gfx_input, equals_gfx_input);
   _mesa_set_init(&ctx->gfx_outputs, ctx, hash_gfx_output, equals_gfx_output);
   /* validate struct packing */
   STATIC_ASSERT(offsetof(struct zink_gfx_pipeline_state, vertex_buffers_enabled_mask) - offsetof(struct zink_gfx_pipeline_state, input) ==
                 offsetof(struct zink_gfx_input_key, vertex_buffers_enabled_mask) - offsetof(struct zink_gfx_input_key, input));
   STATIC_ASSERT(offsetof(struct zink_gfx_pipeline_state, vertex_strides) - offsetof(struct zink_gfx_pipeline_state, input) ==
                 offsetof(struct zink_gfx_input_key, vertex_strides) - offsetof(struct zink_gfx_input_key, input));
   STATIC_ASSERT(offsetof(struct zink_gfx_pipeline_state, element_state) - offsetof(struct zink_gfx_pipeline_state, input) ==
                 offsetof(struct zink_gfx_input_key, element_state) - offsetof(struct zink_gfx_input_key, input));

   STATIC_ASSERT(offsetof(struct zink_gfx_pipeline_state, modules) - offsetof(struct zink_gfx_pipeline_state, gkey) ==
                 offsetof(struct zink_gfx_library_key, modules) - offsetof(struct zink_gfx_library_key, hw_rast_state));
}

bool
zink_set_rasterizer_discard(struct zink_context *ctx, bool disable)
{
   bool value = disable ? false : (ctx->rast_state ? ctx->rast_state->base.rasterizer_discard : false);
   bool changed = ctx->gfx_pipeline_state.dyn_state2.rasterizer_discard != value;
   ctx->gfx_pipeline_state.dyn_state2.rasterizer_discard = value;
   if (!changed)
      return false;
   if (!zink_screen(ctx->base.screen)->info.have_EXT_extended_dynamic_state2)
      ctx->gfx_pipeline_state.dirty |= true;
   ctx->rasterizer_discard_changed = true;
   return true;
}
