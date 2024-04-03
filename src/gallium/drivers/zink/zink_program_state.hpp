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

#include "zink_types.h"
#include "zink_pipeline.h"
#include "zink_program.h"
#include "zink_screen.h"


template <zink_dynamic_state DYNAMIC_STATE>
static uint32_t
hash_gfx_pipeline_state(const void *key)
{
   const struct zink_gfx_pipeline_state *state = (const struct zink_gfx_pipeline_state *)key;
   uint32_t hash = _mesa_hash_data(key, offsetof(struct zink_gfx_pipeline_state, hash));
   if (DYNAMIC_STATE < ZINK_DYNAMIC_STATE2)
      hash = XXH32(&state->dyn_state3, sizeof(state->dyn_state3), hash);
   if (DYNAMIC_STATE < ZINK_DYNAMIC_STATE3)
      hash = XXH32(&state->dyn_state2, sizeof(state->dyn_state2), hash);
   if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE)
      return hash;
   return XXH32(&state->dyn_state1, sizeof(state->dyn_state1), hash);
}

template <bool HAS_DYNAMIC>
static unsigned
get_pipeline_idx(enum pipe_prim_type mode, VkPrimitiveTopology vkmode)
{
   /* VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY specifies that the topology state in
    * VkPipelineInputAssemblyStateCreateInfo only specifies the topology class,
    * and the specific topology order and adjacency must be set dynamically
    * with vkCmdSetPrimitiveTopology before any drawing commands.
    */
   if (HAS_DYNAMIC) {
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
      ikey->pipeline = zink_create_gfx_pipeline_input(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state, NULL, vkmode);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_inputs, hash, ikey);
   }
   return (struct zink_gfx_input_key *)he->key;
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
      ikey->pipeline = zink_create_gfx_pipeline_input(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state, ikey->element_state->binding_map, vkmode);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_inputs, hash, ikey);
   }
   return (struct zink_gfx_input_key*)he->key;
}

static struct zink_gfx_output_key *
find_or_create_output_ds3(struct zink_context *ctx)
{
   uint32_t hash = hash_gfx_output_ds3(&ctx->gfx_pipeline_state);
   struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->gfx_outputs, hash, &ctx->gfx_pipeline_state);
   if (!he) {
      struct zink_gfx_output_key *okey = rzalloc(ctx, struct zink_gfx_output_key);
      memcpy(okey, &ctx->gfx_pipeline_state, sizeof(uint32_t));
      okey->pipeline = zink_create_gfx_pipeline_output(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_outputs, hash, okey);
   }
   return (struct zink_gfx_output_key*)he->key;
}

static struct zink_gfx_output_key *
find_or_create_output(struct zink_context *ctx)
{
   uint32_t hash = hash_gfx_output(&ctx->gfx_pipeline_state);
   struct set_entry *he = _mesa_set_search_pre_hashed(&ctx->gfx_outputs, hash, &ctx->gfx_pipeline_state);
   if (!he) {
      struct zink_gfx_output_key *okey = rzalloc(ctx, struct zink_gfx_output_key);
      memcpy(okey, &ctx->gfx_pipeline_state, offsetof(struct zink_gfx_output_key, pipeline));
      okey->pipeline = zink_create_gfx_pipeline_output(zink_screen(ctx->base.screen), &ctx->gfx_pipeline_state);
      he = _mesa_set_add_pre_hashed(&ctx->gfx_outputs, hash, okey);
   }
   return (struct zink_gfx_output_key*)he->key;
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
      const struct pipe_vertex_buffer *vb = ctx->vertex_buffers + ves->hw_state.binding_map[i];
      unsigned stride = vb->buffer.resource ? vb->stride : 0;
      if (stride && stride < ves->min_stride[i])
         return false;
   }
   return true;
}

template <zink_dynamic_state DYNAMIC_STATE, bool HAVE_LIB>
VkPipeline
zink_get_gfx_pipeline(struct zink_context *ctx,
                      struct zink_gfx_program *prog,
                      struct zink_gfx_pipeline_state *state,
                      enum pipe_prim_type mode)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   bool uses_dynamic_stride = state->uses_dynamic_stride;

   VkPrimitiveTopology vkmode = zink_primitive_topology(mode);
   const unsigned idx = screen->info.dynamic_state3_props.dynamicPrimitiveTopologyUnrestricted ?
                        0 :
                        get_pipeline_idx<DYNAMIC_STATE >= ZINK_DYNAMIC_STATE>(mode, vkmode);
   assert(idx <= ARRAY_SIZE(prog->pipelines[0]));
   if (!state->dirty && !state->modules_changed &&
       ((DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT || DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT2) && !ctx->vertex_state_changed) &&
       idx == state->idx)
      return state->pipeline;

   struct hash_entry *entry = NULL;

   if (state->dirty) {
      if (state->pipeline) //avoid on first hash
         state->final_hash ^= state->hash;
      state->hash = hash_gfx_pipeline_state<DYNAMIC_STATE>(state);
      state->final_hash ^= state->hash;
      state->dirty = false;
   }
   if (screen->optimal_keys) {
      ASSERTED const union zink_shader_key_optimal *opt = (union zink_shader_key_optimal*)&prog->last_variant_hash;
      assert(opt->val == state->shader_keys_optimal.key.val);
      assert(state->optimal_key == state->shader_keys_optimal.key.val);
   }
   if (DYNAMIC_STATE != ZINK_DYNAMIC_VERTEX_INPUT2 && DYNAMIC_STATE != ZINK_DYNAMIC_VERTEX_INPUT && ctx->vertex_state_changed) {
      if (state->pipeline)
         state->final_hash ^= state->vertex_hash;
      if (DYNAMIC_STATE != ZINK_NO_DYNAMIC_STATE)
         uses_dynamic_stride = check_vertex_strides(ctx);
      if (!uses_dynamic_stride) {
         uint32_t hash = 0;
         /* if we don't have dynamic states, we have to hash the enabled vertex buffer bindings */
         uint32_t vertex_buffers_enabled_mask = state->vertex_buffers_enabled_mask;
         hash = XXH32(&vertex_buffers_enabled_mask, sizeof(uint32_t), hash);

         for (unsigned i = 0; i < state->element_state->num_bindings; i++) {
            const unsigned buffer_id = ctx->element_state->hw_state.binding_map[i];
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

   const int rp_idx = state->render_pass ? 1 : 0;
   if (DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT || DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT2) {
      if (prog->last_finalized_hash[rp_idx][idx] == state->final_hash && !prog->inline_variants && likely(prog->last_pipeline[rp_idx][idx])) {
         state->pipeline = prog->last_pipeline[rp_idx][idx];
         return state->pipeline;
      }
   }
   entry = _mesa_hash_table_search_pre_hashed(&prog->pipelines[rp_idx][idx], state->final_hash, state);

   if (!entry) {
      util_queue_fence_wait(&prog->base.cache_fence);
      VkPipeline pipeline = VK_NULL_HANDLE;
      struct gfx_pipeline_cache_entry *pc_entry = CALLOC_STRUCT(gfx_pipeline_cache_entry);
      if (!pc_entry)
         return VK_NULL_HANDLE;
      memcpy(&pc_entry->state, state, sizeof(*state));
      entry = _mesa_hash_table_insert_pre_hashed(&prog->pipelines[rp_idx][idx], state->final_hash, pc_entry, pc_entry);
      if (HAVE_LIB &&
          /* TODO: if there's ever a dynamic render extension with input attachments */
          !ctx->gfx_pipeline_state.render_pass &&
          /* TODO: is sample shading even possible to handle with GPL? */
          !ctx->gfx_stages[MESA_SHADER_FRAGMENT]->nir->info.fs.uses_sample_shading &&
          !zink_get_fs_key(ctx)->fbfetch_ms &&
          !ctx->gfx_pipeline_state.force_persample_interp &&
          !ctx->gfx_pipeline_state.min_samples) {
         struct set_entry *he = _mesa_set_search(&prog->libs, &ctx->gfx_pipeline_state.optimal_key);
         struct zink_gfx_library_key *gkey;
         if (he)
            gkey = (struct zink_gfx_library_key *)he->key;
         else
            gkey = zink_create_pipeline_lib(screen, prog, &ctx->gfx_pipeline_state);
         struct zink_gfx_input_key *ikey = DYNAMIC_STATE == ZINK_DYNAMIC_VERTEX_INPUT ?
                                             find_or_create_input_dynamic(ctx, vkmode) :
                                             find_or_create_input(ctx, vkmode);
         struct zink_gfx_output_key *okey = DYNAMIC_STATE >= ZINK_DYNAMIC_STATE3 && screen->have_full_ds3 ?
                                             find_or_create_output_ds3(ctx) :
                                             find_or_create_output(ctx);
         pc_entry->ikey = ikey;
         pc_entry->gkey = gkey;
         pc_entry->okey = okey;
         pipeline = zink_create_gfx_pipeline_combined(screen, prog, ikey->pipeline, gkey->pipeline, okey->pipeline, true);
      } else {
         pipeline = zink_create_gfx_pipeline(screen, prog, state, state->element_state->binding_map, vkmode);
      }
      if (pipeline == VK_NULL_HANDLE)
         return VK_NULL_HANDLE;

      zink_screen_update_pipeline_cache(screen, &prog->base, false);
      pc_entry->pipeline = pipeline;
   }

   struct gfx_pipeline_cache_entry *cache_entry = (struct gfx_pipeline_cache_entry *)entry->data;
   state->pipeline = cache_entry->pipeline;
   if (DYNAMIC_STATE >= ZINK_DYNAMIC_VERTEX_INPUT) {
      prog->last_finalized_hash[rp_idx][idx] = state->final_hash;
      prog->last_pipeline[rp_idx][idx] = state->pipeline;
   }
   return state->pipeline;
}

template <zink_pipeline_dynamic_state DYNAMIC_STATE, unsigned STAGE_MASK>
static bool
equals_gfx_pipeline_state(const void *a, const void *b)
{
   const struct zink_gfx_pipeline_state *sa = (const struct zink_gfx_pipeline_state *)a;
   const struct zink_gfx_pipeline_state *sb = (const struct zink_gfx_pipeline_state *)b;
   if (DYNAMIC_STATE < ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT) {
      if (sa->uses_dynamic_stride != sb->uses_dynamic_stride)
         return false;
   }
   if (DYNAMIC_STATE == ZINK_PIPELINE_NO_DYNAMIC_STATE ||
       (DYNAMIC_STATE < ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT && !sa->uses_dynamic_stride)) {
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
   if (DYNAMIC_STATE == ZINK_PIPELINE_NO_DYNAMIC_STATE) {
      if (memcmp(&sa->dyn_state1, &sb->dyn_state1, offsetof(struct zink_pipeline_dynamic_state1, depth_stencil_alpha_state)))
         return false;
      if (!!sa->dyn_state1.depth_stencil_alpha_state != !!sb->dyn_state1.depth_stencil_alpha_state ||
          (sa->dyn_state1.depth_stencil_alpha_state &&
           memcmp(sa->dyn_state1.depth_stencil_alpha_state, sb->dyn_state1.depth_stencil_alpha_state,
                  sizeof(struct zink_depth_stencil_alpha_hw_state))))
         return false;
   }
   if (DYNAMIC_STATE < ZINK_PIPELINE_DYNAMIC_STATE3) {
      if (DYNAMIC_STATE < ZINK_PIPELINE_DYNAMIC_STATE2) {
         if (memcmp(&sa->dyn_state2, &sb->dyn_state2, sizeof(sa->dyn_state2)))
            return false;
      }
      if (memcmp(&sa->dyn_state3, &sb->dyn_state3, sizeof(sa->dyn_state3)))
         return false;
   } else if (DYNAMIC_STATE != ZINK_PIPELINE_DYNAMIC_STATE2_PCP &&
              DYNAMIC_STATE != ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT2_PCP &&
              DYNAMIC_STATE != ZINK_PIPELINE_DYNAMIC_STATE3_PCP &&
              DYNAMIC_STATE != ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT_PCP &&
              (STAGE_MASK & BITFIELD_BIT(MESA_SHADER_TESS_EVAL)) &&
              !(STAGE_MASK & BITFIELD_BIT(MESA_SHADER_TESS_CTRL))) {
      if (sa->dyn_state2.vertices_per_patch != sb->dyn_state2.vertices_per_patch)
         return false;
   }
   if (STAGE_MASK & STAGE_MASK_OPTIMAL) {
      if (sa->optimal_key != sb->optimal_key)
         return false;
   } else {
      if (STAGE_MASK & BITFIELD_BIT(MESA_SHADER_TESS_CTRL)) {
         if (sa->modules[MESA_SHADER_TESS_CTRL] != sb->modules[MESA_SHADER_TESS_CTRL])
            return false;
      }
      if (STAGE_MASK & BITFIELD_BIT(MESA_SHADER_TESS_EVAL)) {
         if (sa->modules[MESA_SHADER_TESS_EVAL] != sb->modules[MESA_SHADER_TESS_EVAL])
            return false;
      }
      if (STAGE_MASK & BITFIELD_BIT(MESA_SHADER_GEOMETRY)) {
         if (sa->modules[MESA_SHADER_GEOMETRY] != sb->modules[MESA_SHADER_GEOMETRY])
            return false;
      }
      if (sa->modules[MESA_SHADER_VERTEX] != sb->modules[MESA_SHADER_VERTEX])
         return false;
      if (sa->modules[MESA_SHADER_FRAGMENT] != sb->modules[MESA_SHADER_FRAGMENT])
         return false;
   }
   return !memcmp(a, b, offsetof(struct zink_gfx_pipeline_state, hash));
}

template <zink_pipeline_dynamic_state DYNAMIC_STATE, unsigned STAGE_MASK>
static equals_gfx_pipeline_state_func
get_optimal_gfx_pipeline_stage_eq_func(bool optimal_keys)
{
   if (optimal_keys)
      return equals_gfx_pipeline_state<DYNAMIC_STATE, STAGE_MASK | STAGE_MASK_OPTIMAL>;
   return equals_gfx_pipeline_state<DYNAMIC_STATE, STAGE_MASK>;
}

template <zink_pipeline_dynamic_state DYNAMIC_STATE>
static equals_gfx_pipeline_state_func
get_gfx_pipeline_stage_eq_func(struct zink_gfx_program *prog, bool optimal_keys)
{
   unsigned vertex_stages = prog->stages_present & BITFIELD_MASK(MESA_SHADER_FRAGMENT);
   if (vertex_stages & BITFIELD_BIT(MESA_SHADER_TESS_CTRL)) {
      if (prog->shaders[MESA_SHADER_TESS_CTRL]->is_generated)
         vertex_stages &= ~BITFIELD_BIT(MESA_SHADER_TESS_CTRL);
   }
   if (vertex_stages & BITFIELD_BIT(MESA_SHADER_TESS_CTRL)) {
      if (vertex_stages == BITFIELD_MASK(MESA_SHADER_FRAGMENT))
         /* all stages */
         return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                       BITFIELD_MASK(MESA_SHADER_COMPUTE)>(optimal_keys);
      if (vertex_stages == BITFIELD_MASK(MESA_SHADER_GEOMETRY))
         /* tess only: includes generated tcs too */
         return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                       BITFIELD_MASK(MESA_SHADER_COMPUTE) & ~BITFIELD_BIT(MESA_SHADER_GEOMETRY)>(optimal_keys);
      if (vertex_stages == (BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_GEOMETRY)))
         /* geom only */
         return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                       BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_FRAGMENT) | BITFIELD_BIT(MESA_SHADER_GEOMETRY)>(optimal_keys);
   }
   if (vertex_stages == (BITFIELD_MASK(MESA_SHADER_FRAGMENT) & ~BITFIELD_BIT(MESA_SHADER_TESS_CTRL)))
      /* all stages but tcs */
      return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                    BITFIELD_MASK(MESA_SHADER_COMPUTE) & ~BITFIELD_BIT(MESA_SHADER_TESS_CTRL)>(optimal_keys);
   if (vertex_stages == (BITFIELD_MASK(MESA_SHADER_GEOMETRY) & ~BITFIELD_BIT(MESA_SHADER_TESS_CTRL)))
      /* tess only: generated tcs */
      return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                    BITFIELD_MASK(MESA_SHADER_COMPUTE) & ~(BITFIELD_BIT(MESA_SHADER_GEOMETRY) | BITFIELD_BIT(MESA_SHADER_TESS_CTRL))>(optimal_keys);
   if (vertex_stages == (BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_GEOMETRY)))
      /* geom only */
      return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                    BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_FRAGMENT) | BITFIELD_BIT(MESA_SHADER_GEOMETRY)>(optimal_keys);
   return get_optimal_gfx_pipeline_stage_eq_func<DYNAMIC_STATE,
                                                  BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_FRAGMENT)>(optimal_keys);
}

equals_gfx_pipeline_state_func
zink_get_gfx_pipeline_eq_func(struct zink_screen *screen, struct zink_gfx_program *prog)
{
   if (screen->info.have_EXT_extended_dynamic_state) {
      if (screen->info.have_EXT_extended_dynamic_state2) {
         if (screen->info.have_EXT_extended_dynamic_state3) {
            if (screen->info.have_EXT_vertex_input_dynamic_state) {
               if (screen->info.dynamic_state2_feats.extendedDynamicState2PatchControlPoints)
                  return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT_PCP>(prog, screen->optimal_keys);
               else
                  return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT>(prog, screen->optimal_keys);
            } else {
               if (screen->info.dynamic_state2_feats.extendedDynamicState2PatchControlPoints)
                  return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_STATE3_PCP>(prog, screen->optimal_keys);
               else
                  return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_STATE3>(prog, screen->optimal_keys);
            }
         }
         if (screen->info.have_EXT_vertex_input_dynamic_state) {
            if (screen->info.dynamic_state2_feats.extendedDynamicState2PatchControlPoints)
               return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT2_PCP>(prog, screen->optimal_keys);
            else
               return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_VERTEX_INPUT2>(prog, screen->optimal_keys);
         } else {
            if (screen->info.dynamic_state2_feats.extendedDynamicState2PatchControlPoints)
               return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_STATE2_PCP>(prog, screen->optimal_keys);
            else
               return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_STATE2>(prog, screen->optimal_keys);
         }
      }
      return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_DYNAMIC_STATE>(prog, screen->optimal_keys);
   }
   return get_gfx_pipeline_stage_eq_func<ZINK_PIPELINE_NO_DYNAMIC_STATE>(prog, screen->optimal_keys);
}
