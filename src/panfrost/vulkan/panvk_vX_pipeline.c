/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_pipeline.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_cmd_buffer.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_pipeline.h"
#include "panvk_pipeline_layout.h"
#include "panvk_priv_bo.h"
#include "panvk_shader.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/blend.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "vk_blend.h"
#include "vk_format.h"
#include "vk_pipeline_cache.h"
#include "vk_render_pass.h"
#include "vk_util.h"

#include "panfrost/util/pan_lower_framebuffer.h"

#include "pan_earlyzs.h"
#include "pan_shader.h"

static bool
dyn_state_is_set(const struct panvk_graphics_pipeline *pipeline, uint32_t id)
{
   if (!pipeline)
      return false;

   return BITSET_TEST(pipeline->state.dynamic.set, id);
}

static bool
writes_depth(const struct vk_depth_stencil_state *ds)
{

   return ds && ds->depth.test_enable && ds->depth.write_enable &&
          ds->depth.compare_op != VK_COMPARE_OP_NEVER;
}

static bool
writes_stencil(const struct vk_depth_stencil_state *ds)
{
   return ds && ds->stencil.test_enable &&
          ((ds->stencil.front.write_mask &&
            (ds->stencil.front.op.fail != VK_STENCIL_OP_KEEP ||
             ds->stencil.front.op.pass != VK_STENCIL_OP_KEEP ||
             ds->stencil.front.op.depth_fail != VK_STENCIL_OP_KEEP)) ||
           (ds->stencil.back.write_mask &&
            (ds->stencil.back.op.fail != VK_STENCIL_OP_KEEP ||
             ds->stencil.back.op.pass != VK_STENCIL_OP_KEEP ||
             ds->stencil.back.op.depth_fail != VK_STENCIL_OP_KEEP)));
}

static bool
ds_test_always_passes(const struct vk_depth_stencil_state *ds)
{
   if (!ds)
      return true;

   if (ds->depth.test_enable && ds->depth.compare_op != VK_COMPARE_OP_ALWAYS)
      return false;

   if (ds->stencil.test_enable &&
       (ds->stencil.front.op.compare != VK_COMPARE_OP_ALWAYS ||
        ds->stencil.back.op.compare != VK_COMPARE_OP_ALWAYS))
      return false;

   return true;
}

static inline enum mali_func
translate_compare_func(VkCompareOp comp)
{
   STATIC_ASSERT(VK_COMPARE_OP_NEVER == (VkCompareOp)MALI_FUNC_NEVER);
   STATIC_ASSERT(VK_COMPARE_OP_LESS == (VkCompareOp)MALI_FUNC_LESS);
   STATIC_ASSERT(VK_COMPARE_OP_EQUAL == (VkCompareOp)MALI_FUNC_EQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_LESS_OR_EQUAL == (VkCompareOp)MALI_FUNC_LEQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_GREATER == (VkCompareOp)MALI_FUNC_GREATER);
   STATIC_ASSERT(VK_COMPARE_OP_NOT_EQUAL == (VkCompareOp)MALI_FUNC_NOT_EQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_GREATER_OR_EQUAL ==
                 (VkCompareOp)MALI_FUNC_GEQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_ALWAYS == (VkCompareOp)MALI_FUNC_ALWAYS);

   return (enum mali_func)comp;
}

static enum mali_stencil_op
translate_stencil_op(VkStencilOp in)
{
   switch (in) {
   case VK_STENCIL_OP_KEEP:
      return MALI_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO:
      return MALI_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return MALI_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return MALI_STENCIL_OP_INCR_SAT;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return MALI_STENCIL_OP_DECR_SAT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return MALI_STENCIL_OP_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return MALI_STENCIL_OP_DECR_WRAP;
   case VK_STENCIL_OP_INVERT:
      return MALI_STENCIL_OP_INVERT;
   default:
      unreachable("Invalid stencil op");
   }
}

static void
emit_base_fs_rsd(const struct panvk_graphics_pipeline *pipeline,
                 const struct vk_graphics_pipeline_state *state, void *rsd)
{
   const struct pan_shader_info *info = &pipeline->fs.info;
   const struct vk_rasterization_state *rs = state->rs;
   const struct vk_depth_stencil_state *ds = state->ds;
   const struct vk_multisample_state *ms = state->ms;
   bool test_s = ds && ds->stencil.test_enable;
   bool test_z = ds && ds->depth.test_enable;
   bool writes_z = writes_depth(ds);
   bool writes_s = writes_stencil(ds);

   pan_pack(rsd, RENDERER_STATE, cfg) {
      bool alpha_to_coverage = ms && ms->alpha_to_coverage_enable;

      if (pipeline->state.fs.required) {
         pan_shader_prepare_rsd(info, pipeline->fs.code, &cfg);

         uint8_t rt_written = info->outputs_written >> FRAG_RESULT_DATA0;
         uint8_t rt_mask = pipeline->state.fs.rt_mask;
         cfg.properties.allow_forward_pixel_to_kill =
            pipeline->fs.info.fs.can_fpk && !(rt_mask & ~rt_written) &&
            !alpha_to_coverage && !pipeline->state.blend.reads_dest;

         bool writes_zs = writes_z || writes_s;
         bool zs_always_passes = ds_test_always_passes(ds);
         bool oq = false; /* TODO: Occlusion queries */

         struct pan_earlyzs_state earlyzs =
            pan_earlyzs_get(pan_earlyzs_analyze(info), writes_zs || oq,
                            alpha_to_coverage, zs_always_passes);

         cfg.properties.pixel_kill_operation = earlyzs.kill;
         cfg.properties.zs_update_operation = earlyzs.update;
      } else {
         cfg.properties.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
         cfg.properties.allow_forward_pixel_to_kill = true;
         cfg.properties.allow_forward_pixel_to_be_killed = true;
         cfg.properties.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
      }

      bool msaa = ms && ms->rasterization_samples > 1;
      cfg.multisample_misc.multisample_enable = msaa;
      cfg.multisample_misc.sample_mask = msaa ? ms->sample_mask : UINT16_MAX;

      cfg.multisample_misc.depth_function =
         test_z ? translate_compare_func(ds->depth.compare_op)
                : MALI_FUNC_ALWAYS;

      cfg.multisample_misc.depth_write_mask = writes_z;
      cfg.multisample_misc.fixed_function_near_discard =
         !rs->depth_clamp_enable;
      cfg.multisample_misc.fixed_function_far_discard = !rs->depth_clamp_enable;
      cfg.multisample_misc.shader_depth_range_fixed = true;

      cfg.stencil_mask_misc.stencil_enable = test_s;
      cfg.stencil_mask_misc.alpha_to_coverage = alpha_to_coverage;
      cfg.stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.front_facing_depth_bias = rs->depth_bias.enable;
      cfg.stencil_mask_misc.back_facing_depth_bias = rs->depth_bias.enable;
      cfg.stencil_mask_misc.single_sampled_lines =
         !ms || ms->rasterization_samples <= 1;

      if (dyn_state_is_set(pipeline, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS)) {
         cfg.depth_units = rs->depth_bias.constant * 2.0f;
         cfg.depth_factor = rs->depth_bias.slope;
         cfg.depth_bias_clamp = rs->depth_bias.clamp;
      }

      if (dyn_state_is_set(pipeline, MESA_VK_DYNAMIC_DS_STENCIL_COMPARE_MASK)) {
         cfg.stencil_front.mask = ds->stencil.front.compare_mask;
         cfg.stencil_back.mask = ds->stencil.back.compare_mask;
      }

      if (dyn_state_is_set(pipeline, MESA_VK_DYNAMIC_DS_STENCIL_WRITE_MASK)) {
         cfg.stencil_mask_misc.stencil_mask_front =
            ds->stencil.front.write_mask;
         cfg.stencil_mask_misc.stencil_mask_back = ds->stencil.back.write_mask;
      }

      if (dyn_state_is_set(pipeline, MESA_VK_DYNAMIC_DS_STENCIL_REFERENCE)) {
         cfg.stencil_front.reference_value = ds->stencil.front.reference;
         cfg.stencil_back.reference_value = ds->stencil.back.reference;
      }

      if (test_s) {
         cfg.stencil_front.compare_function =
            translate_compare_func(ds->stencil.front.op.compare);
         cfg.stencil_front.stencil_fail =
            translate_stencil_op(ds->stencil.front.op.fail);
         cfg.stencil_front.depth_fail =
            translate_stencil_op(ds->stencil.front.op.depth_fail);
         cfg.stencil_front.depth_pass =
            translate_stencil_op(ds->stencil.front.op.pass);
         cfg.stencil_back.compare_function =
            translate_compare_func(ds->stencil.back.op.compare);
         cfg.stencil_back.stencil_fail =
            translate_stencil_op(ds->stencil.back.op.fail);
         cfg.stencil_back.depth_fail =
            translate_stencil_op(ds->stencil.back.op.depth_fail);
         cfg.stencil_back.depth_pass =
            translate_stencil_op(ds->stencil.back.op.pass);
      }
   }
}

static enum mali_register_file_format
blend_type_from_nir(nir_alu_type nir_type)
{
   switch (nir_type) {
   case 0: /* Render target not in use */
      return 0;
   case nir_type_float16:
      return MALI_REGISTER_FILE_FORMAT_F16;
   case nir_type_float32:
      return MALI_REGISTER_FILE_FORMAT_F32;
   case nir_type_int32:
      return MALI_REGISTER_FILE_FORMAT_I32;
   case nir_type_uint32:
      return MALI_REGISTER_FILE_FORMAT_U32;
   case nir_type_int16:
      return MALI_REGISTER_FILE_FORMAT_I16;
   case nir_type_uint16:
      return MALI_REGISTER_FILE_FORMAT_U16;
   default:
      unreachable("Unsupported blend shader type for NIR alu type");
   }
}

static void
emit_blend(const struct panvk_graphics_pipeline *pipeline, unsigned rt,
           void *bd)
{
   const struct pan_blend_state *blend = &pipeline->state.blend.pstate;
   const struct pan_blend_rt_state *rts = &blend->rts[rt];
   bool dithered = false;

   pan_pack(bd, BLEND, cfg) {
      if (!blend->rt_count || !rts->equation.color_mask) {
         cfg.enable = false;
         cfg.internal.mode = MALI_BLEND_MODE_OFF;
         continue;
      }

      cfg.srgb = util_format_is_srgb(rts->format);
      cfg.load_destination = pan_blend_reads_dest(blend->rts[rt].equation);
      cfg.round_to_fb_precision = !dithered;

      const struct util_format_description *format_desc =
         util_format_description(rts->format);
      unsigned chan_size = 0;
      for (unsigned i = 0; i < format_desc->nr_channels; i++)
         chan_size = MAX2(format_desc->channel[i].size, chan_size);

      pan_blend_to_fixed_function_equation(blend->rts[rt].equation,
                                           &cfg.equation);

      /* Fixed point constant */
      float fconst = pan_blend_get_constant(
         pan_blend_constant_mask(blend->rts[rt].equation), blend->constants);
      u16 constant = fconst * ((1 << chan_size) - 1);
      constant <<= 16 - chan_size;
      cfg.constant = constant;

      if (pan_blend_is_opaque(blend->rts[rt].equation)) {
         cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
      } else {
         cfg.internal.mode = MALI_BLEND_MODE_FIXED_FUNCTION;

         cfg.internal.fixed_function.alpha_zero_nop =
            pan_blend_alpha_zero_nop(blend->rts[rt].equation);
         cfg.internal.fixed_function.alpha_one_store =
            pan_blend_alpha_one_store(blend->rts[rt].equation);
      }

      /* If we want the conversion to work properly,
       * num_comps must be set to 4
       */
      cfg.internal.fixed_function.num_comps = 4;
      cfg.internal.fixed_function.conversion.memory_format =
         GENX(panfrost_dithered_format_from_pipe_format)(rts->format, dithered);
      cfg.internal.fixed_function.conversion.register_format =
         blend_type_from_nir(pipeline->fs.info.bifrost.blend[rt].type);
      cfg.internal.fixed_function.rt = rt;
   }
}

#define is_dyn(__state, __name)                                                \
   BITSET_TEST((__state)->dynamic, MESA_VK_DYNAMIC_##__name)

static uint32_t
get_active_color_attachments(const struct vk_graphics_pipeline_state *state)
{
   const struct vk_color_blend_state *cb = state->cb;

   if (state->rs->rasterizer_discard_enable || !cb)
      return 0;

   return cb->color_write_enables & BITFIELD_MASK(cb->attachment_count);
}

static void
parse_color_blend(struct panvk_graphics_pipeline *pipeline,
                  const struct vk_graphics_pipeline_state *state)
{
   const struct vk_color_blend_state *cb = state->cb;
   const struct vk_render_pass_state *rp = state->rp;
   const struct vk_multisample_state *ms = state->ms;
   struct panvk_device *dev = to_panvk_device(pipeline->base.base.device);

   if (!cb)
      return;

   uint32_t active_color_attachments = get_active_color_attachments(state);

   pipeline->state.blend.pstate.logicop_enable = cb->logic_op_enable;
   pipeline->state.blend.pstate.logicop_func =
      vk_logic_op_to_pipe(cb->logic_op);
   pipeline->state.blend.pstate.rt_count =
      util_last_bit(active_color_attachments);
   memcpy(pipeline->state.blend.pstate.constants, cb->blend_constants,
          sizeof(pipeline->state.blend.pstate.constants));

   for (unsigned i = 0; i < pipeline->state.blend.pstate.rt_count; i++) {
      const struct vk_color_blend_attachment_state *in = &cb->attachments[i];
      struct pan_blend_rt_state *out = &pipeline->state.blend.pstate.rts[i];

      out->format = vk_format_to_pipe_format(rp->color_attachment_formats[i]);

      bool dest_has_alpha = util_format_has_alpha(out->format);

      out->nr_samples = ms->rasterization_samples;
      out->equation.blend_enable = in->blend_enable;
      out->equation.color_mask = in->write_mask;
      out->equation.rgb_func = vk_blend_op_to_pipe(in->color_blend_op);
      out->equation.rgb_src_factor =
         vk_blend_factor_to_pipe(in->src_color_blend_factor);
      out->equation.rgb_dst_factor =
         vk_blend_factor_to_pipe(in->dst_color_blend_factor);
      out->equation.alpha_func = vk_blend_op_to_pipe(in->alpha_blend_op);
      out->equation.alpha_src_factor =
         vk_blend_factor_to_pipe(in->src_alpha_blend_factor);
      out->equation.alpha_dst_factor =
         vk_blend_factor_to_pipe(in->dst_alpha_blend_factor);

      if (!dest_has_alpha) {
         out->equation.rgb_src_factor =
            util_blend_dst_alpha_to_one(out->equation.rgb_src_factor);
         out->equation.rgb_dst_factor =
            util_blend_dst_alpha_to_one(out->equation.rgb_dst_factor);

         out->equation.alpha_src_factor =
            util_blend_dst_alpha_to_one(out->equation.alpha_src_factor);
         out->equation.alpha_dst_factor =
            util_blend_dst_alpha_to_one(out->equation.alpha_dst_factor);
      }

      pipeline->state.blend.reads_dest |= pan_blend_reads_dest(out->equation);

      unsigned constant_mask = panvk_per_arch(blend_needs_lowering)(
                                  dev, &pipeline->state.blend.pstate, i)
                                  ? 0
                                  : pan_blend_constant_mask(out->equation);
      pipeline->state.blend.constant[i].index = ffs(constant_mask) - 1;
      if (constant_mask) {
         /* On Bifrost, the blend constant is expressed with a UNORM of the
          * size of the target format. The value is then shifted such that
          * used bits are in the MSB. Here we calculate the factor at pipeline
          * creation time so we only have to do a
          *   hw_constant = float_constant * factor;
          * at descriptor emission time.
          */
         const struct util_format_description *format_desc =
            util_format_description(out->format);
         unsigned chan_size = 0;
         for (unsigned c = 0; c < format_desc->nr_channels; c++)
            chan_size = MAX2(format_desc->channel[c].size, chan_size);
         pipeline->state.blend.constant[i].bifrost_factor =
            ((1 << chan_size) - 1) << (16 - chan_size);
      }
   }
}

static bool
fs_required(struct panvk_graphics_pipeline *pipeline)
{
   const struct pan_shader_info *info = &pipeline->fs.info;

   /* If we generally have side effects */
   if (info->fs.sidefx)
      return true;

   /* If colour is written we need to execute */
   const struct pan_blend_state *blend = &pipeline->state.blend.pstate;
   for (unsigned i = 0; i < blend->rt_count; ++i) {
      if (blend->rts[i].equation.color_mask)
         return true;
   }

   /* If depth is written and not implied we need to execute.
    * TODO: Predicate on Z/S writes being enabled */
   return (info->fs.writes_depth || info->fs.writes_stencil);
}

static void
init_fs_state(struct panvk_graphics_pipeline *pipeline,
              const struct vk_graphics_pipeline_state *state)
{
   pipeline->state.fs.dynamic_rsd = is_dyn(state, RS_DEPTH_BIAS_FACTORS) ||
                                    is_dyn(state, CB_BLEND_CONSTANTS) ||
                                    is_dyn(state, DS_STENCIL_COMPARE_MASK) ||
                                    is_dyn(state, DS_STENCIL_WRITE_MASK) ||
                                    is_dyn(state, DS_STENCIL_REFERENCE);
   pipeline->state.fs.rt_mask = get_active_color_attachments(state);
   pipeline->state.fs.required = fs_required(pipeline);

   unsigned bd_count = MAX2(pipeline->state.blend.pstate.rt_count, 1);
   struct mali_renderer_state_packed *rsd = &pipeline->state.fs.rsd_template;
   struct mali_blend_packed *bds = pipeline->state.blend.bd_template;

   if (!pipeline->state.fs.dynamic_rsd) {
      struct panfrost_ptr ptr = pan_pool_alloc_desc_aggregate(
         &pipeline->base.desc_pool.base, PAN_DESC(RENDERER_STATE),
         PAN_DESC_ARRAY(bd_count, BLEND));

      rsd = ptr.cpu;
      bds = ptr.cpu + pan_size(RENDERER_STATE);
      pipeline->fs.rsd = ptr.gpu;
   }

   emit_base_fs_rsd(pipeline, state, rsd);
   for (unsigned i = 0; i < bd_count; i++)
      emit_blend(pipeline, i, &bds[i]);
}

static void
update_varying_slot(struct panvk_varyings_info *varyings, gl_shader_stage stage,
                    const struct pan_shader_varying *varying, bool input)
{
   gl_varying_slot loc = varying->location;
   enum panvk_varying_buf_id buf_id = panvk_varying_buf_id(loc);

   varyings->stage[stage].loc[varyings->stage[stage].count++] = loc;

   assert(loc < ARRAY_SIZE(varyings->varying));

   enum pipe_format new_fmt = varying->format;
   enum pipe_format old_fmt = varyings->varying[loc].format;

   BITSET_SET(varyings->active, loc);

   /* We expect inputs to either be set by a previous stage or be built
    * in, skip the entry if that's not the case, we'll emit a const
    * varying returning zero for those entries.
    */
   if (input && old_fmt == PIPE_FORMAT_NONE)
      return;

   unsigned new_size = util_format_get_blocksize(new_fmt);
   unsigned old_size = util_format_get_blocksize(old_fmt);

   if (old_size < new_size)
      varyings->varying[loc].format = new_fmt;

   /* Type (float or not) information is only known in the fragment shader, so
    * override for that
    */
   if (input) {
      assert(stage == MESA_SHADER_FRAGMENT && "no geom/tess on Bifrost");
      varyings->varying[loc].format = new_fmt;
   }

   varyings->buf_mask |= 1 << buf_id;
}

static void
collect_varyings(struct panvk_graphics_pipeline *pipeline)
{
   const struct pan_shader_info *vs_info = &pipeline->vs.info;
   const struct pan_shader_info *fs_info = &pipeline->fs.info;

   for (unsigned i = 0; i < vs_info->varyings.output_count; i++) {
      update_varying_slot(&pipeline->varyings, MESA_SHADER_VERTEX,
                          &vs_info->varyings.output[i], false);
   }

   for (unsigned i = 0; i < fs_info->varyings.input_count; i++) {
      update_varying_slot(&pipeline->varyings, MESA_SHADER_FRAGMENT,
                          &fs_info->varyings.input[i], true);
   }

   /* TODO: Xfb */
   gl_varying_slot loc;
   BITSET_FOREACH_SET(loc, pipeline->varyings.active, VARYING_SLOT_MAX) {
      if (pipeline->varyings.varying[loc].format == PIPE_FORMAT_NONE)
         continue;

      enum panvk_varying_buf_id buf_id = panvk_varying_buf_id(loc);
      unsigned buf_idx = panvk_varying_buf_index(&pipeline->varyings, buf_id);
      unsigned varying_sz = panvk_varying_size(&pipeline->varyings, loc);

      pipeline->varyings.varying[loc].buf = buf_idx;
      pipeline->varyings.varying[loc].offset =
         pipeline->varyings.buf[buf_idx].stride;
      pipeline->varyings.buf[buf_idx].stride += varying_sz;
   }
}

static VkResult
init_pipeline_shader(struct panvk_pipeline *pipeline,
                     const VkPipelineShaderStageCreateInfo *stage_info,
                     const VkAllocationCallbacks *alloc,
                     struct panvk_pipeline_shader *pshader)
{
   struct panvk_device *dev = to_panvk_device(pipeline->base.device);
   struct panvk_graphics_pipeline *gfx_pipeline =
      panvk_pipeline_to_graphics_pipeline(pipeline);
   struct panvk_shader *shader;

   shader = panvk_per_arch(shader_create)(
      dev, stage_info, pipeline->layout,
      gfx_pipeline ? &gfx_pipeline->state.blend.pstate : NULL,
      dyn_state_is_set(gfx_pipeline, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS),
      alloc);
   if (!shader)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   void *shader_data = util_dynarray_element(&shader->binary, uint8_t, 0);
   unsigned shader_sz = util_dynarray_num_elements(&shader->binary, uint8_t);

   if (shader_sz) {
      pshader->code = pan_pool_upload_aligned(&pipeline->bin_pool.base,
                                              shader_data, shader_sz, 128);
   } else {
      pshader->code = 0;
   }

   pshader->info = shader->info;
   pshader->has_img_access = shader->has_img_access;

   if (stage_info->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      struct panvk_compute_pipeline *compute_pipeline =
         panvk_pipeline_to_compute_pipeline(pipeline);

      compute_pipeline->local_size = shader->local_size;
   }

   if (stage_info->stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
      struct panfrost_ptr rsd =
         pan_pool_alloc_desc(&pipeline->desc_pool.base, RENDERER_STATE);

      pan_pack(rsd.cpu, RENDERER_STATE, cfg) {
         pan_shader_prepare_rsd(&pshader->info, pshader->code, &cfg);
      }

      pshader->rsd = rsd.gpu;
   }

   panvk_per_arch(shader_destroy)(dev, shader, alloc);
   return VK_SUCCESS;
}

static VkResult
panvk_graphics_pipeline_create(struct panvk_device *dev,
                               struct vk_pipeline_cache *cache,
                               const VkGraphicsPipelineCreateInfo *create_info,
                               const VkAllocationCallbacks *alloc,
                               struct panvk_pipeline **out)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, layout, create_info->layout);
   struct vk_graphics_pipeline_all_state all;
   struct vk_graphics_pipeline_state state = {};
   VkResult result;

   result = vk_graphics_pipeline_state_fill(&dev->vk, &state, create_info, NULL,
                                            0, &all, NULL, 0, NULL);
   if (result)
      return result;

   struct panvk_graphics_pipeline *gfx_pipeline = vk_object_zalloc(
      &dev->vk, alloc, sizeof(*gfx_pipeline), VK_OBJECT_TYPE_PIPELINE);

   if (!gfx_pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out = &gfx_pipeline->base;
   gfx_pipeline->base.layout = layout;
   gfx_pipeline->base.type = PANVK_PIPELINE_GRAPHICS;
   gfx_pipeline->state.dynamic.vi = &gfx_pipeline->state.vi;
   gfx_pipeline->state.dynamic.ms.sample_locations = &gfx_pipeline->state.sl;
   vk_dynamic_graphics_state_fill(&gfx_pipeline->state.dynamic, &state);
   gfx_pipeline->state.rp = *state.rp;

   panvk_pool_init(&gfx_pipeline->base.bin_pool, dev, NULL,
                   PAN_KMOD_BO_FLAG_EXECUTABLE, 4096,
                   "Pipeline shader binaries", false);
   panvk_pool_init(&gfx_pipeline->base.desc_pool, dev, NULL, 0, 4096,
                   "Pipeline static state", false);

   parse_color_blend(gfx_pipeline, &state);

   /* Make sure the stage info is correct even if no stage info is provided for
    * this stage in pStages.
    */
   gfx_pipeline->vs.info.stage = MESA_SHADER_VERTEX;
   gfx_pipeline->fs.info.stage = MESA_SHADER_FRAGMENT;

   for (uint32_t i = 0; i < create_info->stageCount; i++) {
      struct panvk_pipeline_shader *pshader = NULL;
      switch (create_info->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
         pshader = &gfx_pipeline->vs;
         break;

      case VK_SHADER_STAGE_FRAGMENT_BIT:
         pshader = &gfx_pipeline->fs;
         break;

      default:
         assert(!"Unsupported graphics pipeline stage");
      }

      VkResult result = init_pipeline_shader(
         &gfx_pipeline->base, &create_info->pStages[i], alloc, pshader);
      if (result != VK_SUCCESS)
         return result;
   }

   collect_varyings(gfx_pipeline);
   init_fs_state(gfx_pipeline, &state);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(CreateGraphicsPipelines)(
   VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
   const VkGraphicsPipelineCreateInfo *pCreateInfos,
   const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(panvk_device, dev, device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct panvk_pipeline *pipeline;
      VkResult result = panvk_graphics_pipeline_create(
         dev, cache, &pCreateInfos[i], pAllocator, &pipeline);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            panvk_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = panvk_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

static VkResult
panvk_compute_pipeline_create(struct panvk_device *dev,
                              struct vk_pipeline_cache *cache,
                              const VkComputePipelineCreateInfo *create_info,
                              const VkAllocationCallbacks *alloc,
                              struct panvk_pipeline **out)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, layout, create_info->layout);
   struct panvk_compute_pipeline *compute_pipeline = vk_object_zalloc(
      &dev->vk, alloc, sizeof(*compute_pipeline), VK_OBJECT_TYPE_PIPELINE);

   if (!compute_pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out = &compute_pipeline->base;
   compute_pipeline->base.layout = layout;
   compute_pipeline->base.type = PANVK_PIPELINE_COMPUTE;

   panvk_pool_init(&compute_pipeline->base.bin_pool, dev, NULL,
                   PAN_KMOD_BO_FLAG_EXECUTABLE, 4096,
                   "Pipeline shader binaries", false);
   panvk_pool_init(&compute_pipeline->base.desc_pool, dev, NULL, 0, 4096,
                   "Pipeline static state", false);

   VkResult result =
      init_pipeline_shader(&compute_pipeline->base, &create_info->stage, alloc,
                           &compute_pipeline->cs);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(CreateComputePipelines)(
   VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
   const VkComputePipelineCreateInfo *pCreateInfos,
   const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(panvk_device, dev, device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct panvk_pipeline *pipeline;
      VkResult result = panvk_compute_pipeline_create(
         dev, cache, &pCreateInfos[i], pAllocator, &pipeline);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            panvk_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = panvk_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(DestroyPipeline)(VkDevice _device, VkPipeline _pipeline,
                                const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_pipeline, pipeline, _pipeline);

   panvk_pool_cleanup(&pipeline->bin_pool);
   panvk_pool_cleanup(&pipeline->desc_pool);
   vk_object_free(&device->vk, pAllocator, pipeline);
}
