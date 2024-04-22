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

static void
release_shaders(struct panvk_pipeline *pipeline, struct panvk_shader **shaders,
                const VkAllocationCallbacks *alloc)
{
   struct panvk_device *dev = to_panvk_device(pipeline->base.device);

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!shaders[i])
         continue;

      panvk_per_arch(shader_destroy)(dev, shaders[i], alloc);
   }
}

static bool
is_static_state(const struct panvk_graphics_pipeline *pipeline, uint32_t id)
{
   if (!pipeline)
      return false;

   return !(pipeline->state.dynamic_mask & (1 << id));
}

static VkResult
compile_shaders(struct panvk_pipeline *pipeline,
                const VkPipelineShaderStageCreateInfo *stages,
                uint32_t stage_count, const VkAllocationCallbacks *alloc,
                struct panvk_shader **shaders)
{
   struct panvk_device *dev = to_panvk_device(pipeline->base.device);
   struct panvk_graphics_pipeline *gfx_pipeline =
      panvk_pipeline_to_graphics_pipeline(pipeline);
   const VkPipelineShaderStageCreateInfo *stage_infos[MESA_SHADER_STAGES] = {
      NULL,
   };

   for (uint32_t i = 0; i < stage_count; i++) {
      gl_shader_stage stage = vk_to_mesa_shader_stage(stages[i].stage);
      stage_infos[stage] = &stages[i];
   }

   /* compile shaders in reverse order */
   for (gl_shader_stage stage = MESA_SHADER_STAGES - 1;
        stage > MESA_SHADER_NONE; stage--) {
      const VkPipelineShaderStageCreateInfo *stage_info = stage_infos[stage];
      if (!stage_info)
         continue;

      struct panvk_shader *shader;

      shader = panvk_per_arch(shader_create)(
         dev, stage, stage_info, pipeline->layout,
         gfx_pipeline ? &gfx_pipeline->state.blend.pstate : NULL,
         is_static_state(gfx_pipeline, VK_DYNAMIC_STATE_BLEND_CONSTANTS),
         alloc);
      if (!shader)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      shaders[stage] = shader;
   }

   return VK_SUCCESS;
}

static mali_ptr
upload_shader(struct panvk_pipeline *pipeline,
              const struct panvk_shader *shader)
{
   void *shader_data = util_dynarray_element(&shader->binary, uint8_t, 0);
   unsigned shader_sz = util_dynarray_num_elements(&shader->binary, uint8_t);

   if (!shader_sz)
      return 0;

   return pan_pool_upload_aligned(&pipeline->bin_pool.base, shader_data,
                                  shader_sz, 128);
}

static void
emit_non_fs_rsd(const struct pan_shader_info *shader_info, mali_ptr shader_ptr,
                void *rsd)
{
   assert(shader_info->stage != MESA_SHADER_FRAGMENT);

   pan_pack(rsd, RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(shader_info, shader_ptr, &cfg);
   }
}

static void
emit_base_fs_rsd(const struct panvk_graphics_pipeline *pipeline, void *rsd)
{
   const struct pan_shader_info *info = &pipeline->state.fs.info;

   pan_pack(rsd, RENDERER_STATE, cfg) {
      if (pipeline->state.fs.required) {
         pan_shader_prepare_rsd(info, pipeline->state.fs.address, &cfg);

         uint8_t rt_written =
            pipeline->state.fs.info.outputs_written >> FRAG_RESULT_DATA0;
         uint8_t rt_mask = pipeline->state.fs.rt_mask;
         cfg.properties.allow_forward_pixel_to_kill =
            pipeline->state.fs.info.fs.can_fpk && !(rt_mask & ~rt_written) &&
            !pipeline->state.ms.alpha_to_coverage &&
            !pipeline->state.blend.reads_dest;

         bool writes_zs =
            pipeline->state.zs.z_write || pipeline->state.zs.s_test;
         bool zs_always_passes =
            !pipeline->state.zs.z_test && !pipeline->state.zs.s_test;
         bool oq = false; /* TODO: Occlusion queries */

         struct pan_earlyzs_state earlyzs = pan_earlyzs_get(
            pan_earlyzs_analyze(info), writes_zs || oq,
            pipeline->state.ms.alpha_to_coverage, zs_always_passes);

         cfg.properties.pixel_kill_operation = earlyzs.kill;
         cfg.properties.zs_update_operation = earlyzs.update;
      } else {
         cfg.properties.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
         cfg.properties.allow_forward_pixel_to_kill = true;
         cfg.properties.allow_forward_pixel_to_be_killed = true;
         cfg.properties.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
      }

      bool msaa = pipeline->state.ms.rast_samples > 1;
      cfg.multisample_misc.multisample_enable = msaa;
      cfg.multisample_misc.sample_mask =
         msaa ? pipeline->state.ms.sample_mask : UINT16_MAX;

      cfg.multisample_misc.depth_function =
         pipeline->state.zs.z_test ? pipeline->state.zs.z_compare_func
                                   : MALI_FUNC_ALWAYS;

      cfg.multisample_misc.depth_write_mask = pipeline->state.zs.z_write;
      cfg.multisample_misc.fixed_function_near_discard =
         !pipeline->state.rast.clamp_depth;
      cfg.multisample_misc.fixed_function_far_discard =
         !pipeline->state.rast.clamp_depth;
      cfg.multisample_misc.shader_depth_range_fixed = true;

      cfg.stencil_mask_misc.stencil_enable = pipeline->state.zs.s_test;
      cfg.stencil_mask_misc.alpha_to_coverage =
         pipeline->state.ms.alpha_to_coverage;
      cfg.stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_mask_misc.front_facing_depth_bias =
         pipeline->state.rast.depth_bias.enable;
      cfg.stencil_mask_misc.back_facing_depth_bias =
         pipeline->state.rast.depth_bias.enable;
      cfg.stencil_mask_misc.single_sampled_lines =
         pipeline->state.ms.rast_samples <= 1;

      if (is_static_state(pipeline, VK_DYNAMIC_STATE_DEPTH_BIAS)) {
         cfg.depth_units =
            pipeline->state.rast.depth_bias.constant_factor * 2.0f;
         cfg.depth_factor = pipeline->state.rast.depth_bias.slope_factor;
         cfg.depth_bias_clamp = pipeline->state.rast.depth_bias.clamp;
      }

      if (is_static_state(pipeline, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
         cfg.stencil_front.mask = pipeline->state.zs.s_front.compare_mask;
         cfg.stencil_back.mask = pipeline->state.zs.s_back.compare_mask;
      }

      if (is_static_state(pipeline, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
         cfg.stencil_mask_misc.stencil_mask_front =
            pipeline->state.zs.s_front.write_mask;
         cfg.stencil_mask_misc.stencil_mask_back =
            pipeline->state.zs.s_back.write_mask;
      }

      if (is_static_state(pipeline, VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
         cfg.stencil_front.reference_value = pipeline->state.zs.s_front.ref;
         cfg.stencil_back.reference_value = pipeline->state.zs.s_back.ref;
      }

      cfg.stencil_front.compare_function =
         pipeline->state.zs.s_front.compare_func;
      cfg.stencil_front.stencil_fail = pipeline->state.zs.s_front.fail_op;
      cfg.stencil_front.depth_fail = pipeline->state.zs.s_front.z_fail_op;
      cfg.stencil_front.depth_pass = pipeline->state.zs.s_front.pass_op;
      cfg.stencil_back.compare_function =
         pipeline->state.zs.s_back.compare_func;
      cfg.stencil_back.stencil_fail = pipeline->state.zs.s_back.fail_op;
      cfg.stencil_back.depth_fail = pipeline->state.zs.s_back.z_fail_op;
      cfg.stencil_back.depth_pass = pipeline->state.zs.s_back.pass_op;
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
         blend_type_from_nir(pipeline->state.fs.info.bifrost.blend[rt].type);
      cfg.internal.fixed_function.rt = rt;
   }
}

static void
init_shaders(struct panvk_pipeline *pipeline,
             const VkGraphicsPipelineCreateInfo *gfx_create_info,
             struct panvk_shader **shaders)
{
   struct panvk_graphics_pipeline *gfx_pipeline =
      panvk_pipeline_to_graphics_pipeline(pipeline);
   struct panvk_compute_pipeline *compute_pipeline =
      panvk_pipeline_to_compute_pipeline(pipeline);

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct panvk_shader *shader = shaders[i];
      if (!shader)
         continue;

      pipeline->tls_size = MAX2(pipeline->tls_size, shader->info.tls_size);

      if (shader->has_img_access)
         pipeline->img_access_mask |= BITFIELD_BIT(i);

      if (i == MESA_SHADER_VERTEX && shader->info.vs.writes_point_size) {
         VkPrimitiveTopology topology =
            gfx_create_info->pInputAssemblyState->topology;
         bool points = (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST);

         /* Even if the vertex shader writes point size, we only consider the
          * pipeline to write point size when we're actually drawing points.
          * Otherwise the point size write would conflict with wide lines.
          */
         gfx_pipeline->state.ia.writes_point_size = points;
      }

      mali_ptr shader_ptr = i == MESA_SHADER_FRAGMENT
                               ? gfx_pipeline->state.fs.address
                               : upload_shader(pipeline, shader);

      if (i != MESA_SHADER_FRAGMENT) {
         struct panfrost_ptr rsd =
            pan_pool_alloc_desc(&pipeline->desc_pool.base, RENDERER_STATE);

         emit_non_fs_rsd(&shader->info, shader_ptr, rsd.cpu);
         pipeline->rsds[i] = rsd.gpu;
      }

      if (i == MESA_SHADER_COMPUTE) {
         compute_pipeline->local_size = shader->local_size;
         compute_pipeline->wls_size = shader->info.wls_size;
      }
   }

   if (gfx_create_info && !gfx_pipeline->state.fs.dynamic_rsd) {
      unsigned bd_count = MAX2(gfx_pipeline->state.blend.pstate.rt_count, 1);
      struct panfrost_ptr rsd = pan_pool_alloc_desc_aggregate(
         &pipeline->desc_pool.base, PAN_DESC(RENDERER_STATE),
         PAN_DESC_ARRAY(bd_count, BLEND));
      void *bd = rsd.cpu + pan_size(RENDERER_STATE);

      emit_base_fs_rsd(gfx_pipeline, rsd.cpu);
      for (unsigned rt = 0; rt < gfx_pipeline->state.blend.pstate.rt_count;
           rt++) {
         emit_blend(gfx_pipeline, rt, bd);
         bd += pan_size(BLEND);
      }

      pipeline->rsds[MESA_SHADER_FRAGMENT] = rsd.gpu;
   } else if (gfx_create_info) {
      emit_base_fs_rsd(gfx_pipeline,
                       gfx_pipeline->state.fs.rsd_template.opaque);
      for (unsigned rt = 0;
           rt < MAX2(gfx_pipeline->state.blend.pstate.rt_count, 1); rt++) {
         emit_blend(gfx_pipeline, rt,
                    &gfx_pipeline->state.blend.bd_template[rt].opaque);
      }
   }
}

static void
parse_viewport(struct panvk_graphics_pipeline *pipeline,
               const VkGraphicsPipelineCreateInfo *create_info)
{
   /* The spec says:
    *
    *    pViewportState is a pointer to an instance of the
    *    VkPipelineViewportStateCreateInfo structure, and is ignored if the
    *    pipeline has rasterization disabled.
    */
   if (!create_info->pRasterizationState->rasterizerDiscardEnable &&
       is_static_state(pipeline, VK_DYNAMIC_STATE_VIEWPORT) &&
       is_static_state(pipeline, VK_DYNAMIC_STATE_SCISSOR)) {
      struct panfrost_ptr vpd =
         pan_pool_alloc_desc(&pipeline->base.desc_pool.base, VIEWPORT);

      panvk_per_arch(emit_viewport)(create_info->pViewportState->pViewports,
                                    create_info->pViewportState->pScissors,
                                    vpd.cpu);
      pipeline->state.vp.vpd = vpd.gpu;
   }

   if (is_static_state(pipeline, VK_DYNAMIC_STATE_VIEWPORT) &&
       create_info->pViewportState)
      pipeline->state.vp.viewport = create_info->pViewportState->pViewports[0];

   if (is_static_state(pipeline, VK_DYNAMIC_STATE_SCISSOR) &&
       create_info->pViewportState)
      pipeline->state.vp.scissor = create_info->pViewportState->pScissors[0];
}

static void
parse_dynamic_state(struct panvk_graphics_pipeline *pipeline,
                    const VkGraphicsPipelineCreateInfo *create_info)
{
   const VkPipelineDynamicStateCreateInfo *dynamic_info =
      create_info->pDynamicState;

   if (!dynamic_info)
      return;

   for (uint32_t i = 0; i < dynamic_info->dynamicStateCount; i++) {
      VkDynamicState state = dynamic_info->pDynamicStates[i];
      switch (state) {
      case VK_DYNAMIC_STATE_VIEWPORT ... VK_DYNAMIC_STATE_STENCIL_REFERENCE:
         pipeline->state.dynamic_mask |= 1 << state;
         break;
      default:
         unreachable("unsupported dynamic state");
      }
   }
}

static enum mali_draw_mode
translate_prim_topology(VkPrimitiveTopology in)
{
   switch (in) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return MALI_DRAW_MODE_POINTS;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return MALI_DRAW_MODE_LINES;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return MALI_DRAW_MODE_LINE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return MALI_DRAW_MODE_TRIANGLES;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return MALI_DRAW_MODE_TRIANGLE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return MALI_DRAW_MODE_TRIANGLE_FAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
   default:
      unreachable("Invalid primitive type");
   }
}

static void
parse_input_assembly(struct panvk_graphics_pipeline *pipeline,
                     const VkGraphicsPipelineCreateInfo *create_info)
{
   pipeline->state.ia.primitive_restart =
      create_info->pInputAssemblyState->primitiveRestartEnable;
   pipeline->state.ia.topology =
      translate_prim_topology(create_info->pInputAssemblyState->topology);
}

static uint32_t
get_active_color_attachments(const VkGraphicsPipelineCreateInfo *create_info,
                             enum pipe_format *attachment_formats)
{
   if (create_info->pRasterizationState->rasterizerDiscardEnable)
      return 0;

   VK_FROM_HANDLE(vk_render_pass, pass, create_info->renderPass);
   const struct vk_subpass *subpass = &pass->subpasses[create_info->subpass];
   uint32_t active_color_attachments = 0;

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t idx = subpass->color_attachments[i].attachment;
      if (idx == VK_ATTACHMENT_UNUSED)
         continue;

      active_color_attachments |= 1 << i;

      if (attachment_formats)
         attachment_formats[i] =
            vk_format_to_pipe_format(pass->attachments[idx].format);
   }

   return active_color_attachments;
}

static void
parse_color_blend(struct panvk_graphics_pipeline *pipeline,
                  const VkGraphicsPipelineCreateInfo *create_info)
{
   if (!create_info->pColorBlendState)
      return;

   enum pipe_format attachment_formats[MAX_RTS] = {0};
   unsigned active_color_attachments =
      get_active_color_attachments(create_info, attachment_formats);
   struct panvk_device *dev = to_panvk_device(pipeline->base.base.device);

   pipeline->state.blend.pstate.logicop_enable =
      create_info->pColorBlendState->logicOpEnable;
   pipeline->state.blend.pstate.logicop_func =
      vk_logic_op_to_pipe(create_info->pColorBlendState->logicOp);
   pipeline->state.blend.pstate.rt_count =
      util_last_bit(active_color_attachments);
   memcpy(pipeline->state.blend.pstate.constants,
          create_info->pColorBlendState->blendConstants,
          sizeof(pipeline->state.blend.pstate.constants));

   for (unsigned i = 0; i < pipeline->state.blend.pstate.rt_count; i++) {
      const VkPipelineColorBlendAttachmentState *in =
         &create_info->pColorBlendState->pAttachments[i];
      struct pan_blend_rt_state *out = &pipeline->state.blend.pstate.rts[i];

      out->format = attachment_formats[i];

      bool dest_has_alpha = util_format_has_alpha(out->format);

      out->nr_samples = create_info->pMultisampleState->rasterizationSamples;
      out->equation.blend_enable = in->blendEnable;
      out->equation.color_mask = in->colorWriteMask;
      out->equation.rgb_func = vk_blend_op_to_pipe(in->colorBlendOp);
      out->equation.rgb_src_factor =
         vk_blend_factor_to_pipe(in->srcColorBlendFactor);
      out->equation.rgb_dst_factor =
         vk_blend_factor_to_pipe(in->dstColorBlendFactor);
      out->equation.alpha_func = vk_blend_op_to_pipe(in->alphaBlendOp);
      out->equation.alpha_src_factor =
         vk_blend_factor_to_pipe(in->srcAlphaBlendFactor);
      out->equation.alpha_dst_factor =
         vk_blend_factor_to_pipe(in->dstAlphaBlendFactor);

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

static void
parse_multisample(struct panvk_graphics_pipeline *pipeline,
                  const VkGraphicsPipelineCreateInfo *create_info)
{
   unsigned nr_samples =
      MAX2(create_info->pMultisampleState->rasterizationSamples, 1);

   pipeline->state.ms.rast_samples =
      create_info->pMultisampleState->rasterizationSamples;
   pipeline->state.ms.sample_mask =
      create_info->pMultisampleState->pSampleMask
         ? create_info->pMultisampleState->pSampleMask[0]
         : UINT16_MAX;
   pipeline->state.ms.min_samples =
      MAX2(create_info->pMultisampleState->minSampleShading * nr_samples, 1);
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

static void
parse_zs(struct panvk_graphics_pipeline *pipeline,
         const VkGraphicsPipelineCreateInfo *create_info)
{
   VK_FROM_HANDLE(vk_render_pass, pass, create_info->renderPass);
   const struct vk_subpass *subpass = &pass->subpasses[create_info->subpass];
   bool use_depth_stencil_attachment =
      subpass->depth_stencil_attachment &&
      subpass->depth_stencil_attachment->attachment != VK_ATTACHMENT_UNUSED;

   if (!use_depth_stencil_attachment)
      return;

   pipeline->state.zs.z_test = create_info->pDepthStencilState->depthTestEnable;

   /* The Vulkan spec says:
    *
    *    depthWriteEnable controls whether depth writes are enabled when
    *    depthTestEnable is VK_TRUE. Depth writes are always disabled when
    *    depthTestEnable is VK_FALSE.
    *
    * The hardware does not make this distinction, though, so we AND in the
    * condition ourselves.
    */
   pipeline->state.zs.z_write =
      pipeline->state.zs.z_test &&
      create_info->pDepthStencilState->depthWriteEnable;

   pipeline->state.zs.z_compare_func =
      translate_compare_func(create_info->pDepthStencilState->depthCompareOp);
   pipeline->state.zs.s_test =
      create_info->pDepthStencilState->stencilTestEnable;
   pipeline->state.zs.s_front.fail_op =
      translate_stencil_op(create_info->pDepthStencilState->front.failOp);
   pipeline->state.zs.s_front.pass_op =
      translate_stencil_op(create_info->pDepthStencilState->front.passOp);
   pipeline->state.zs.s_front.z_fail_op =
      translate_stencil_op(create_info->pDepthStencilState->front.depthFailOp);
   pipeline->state.zs.s_front.compare_func =
      translate_compare_func(create_info->pDepthStencilState->front.compareOp);
   pipeline->state.zs.s_front.compare_mask =
      create_info->pDepthStencilState->front.compareMask;
   pipeline->state.zs.s_front.write_mask =
      create_info->pDepthStencilState->front.writeMask;
   pipeline->state.zs.s_front.ref =
      create_info->pDepthStencilState->front.reference;
   pipeline->state.zs.s_back.fail_op =
      translate_stencil_op(create_info->pDepthStencilState->back.failOp);
   pipeline->state.zs.s_back.pass_op =
      translate_stencil_op(create_info->pDepthStencilState->back.passOp);
   pipeline->state.zs.s_back.z_fail_op =
      translate_stencil_op(create_info->pDepthStencilState->back.depthFailOp);
   pipeline->state.zs.s_back.compare_func =
      translate_compare_func(create_info->pDepthStencilState->back.compareOp);
   pipeline->state.zs.s_back.compare_mask =
      create_info->pDepthStencilState->back.compareMask;
   pipeline->state.zs.s_back.write_mask =
      create_info->pDepthStencilState->back.writeMask;
   pipeline->state.zs.s_back.ref =
      create_info->pDepthStencilState->back.reference;
}

static void
parse_rast(struct panvk_graphics_pipeline *pipeline,
           const VkGraphicsPipelineCreateInfo *create_info)
{
   pipeline->state.rast.clamp_depth =
      create_info->pRasterizationState->depthClampEnable;
   pipeline->state.rast.depth_bias.enable =
      create_info->pRasterizationState->depthBiasEnable;
   pipeline->state.rast.depth_bias.constant_factor =
      create_info->pRasterizationState->depthBiasConstantFactor;
   pipeline->state.rast.depth_bias.clamp =
      create_info->pRasterizationState->depthBiasClamp;
   pipeline->state.rast.depth_bias.slope_factor =
      create_info->pRasterizationState->depthBiasSlopeFactor;
   pipeline->state.rast.front_ccw =
      create_info->pRasterizationState->frontFace ==
      VK_FRONT_FACE_COUNTER_CLOCKWISE;
   pipeline->state.rast.cull_front_face =
      create_info->pRasterizationState->cullMode & VK_CULL_MODE_FRONT_BIT;
   pipeline->state.rast.cull_back_face =
      create_info->pRasterizationState->cullMode & VK_CULL_MODE_BACK_BIT;
   pipeline->state.rast.line_width =
      create_info->pRasterizationState->lineWidth;
   pipeline->state.rast.enable =
      !create_info->pRasterizationState->rasterizerDiscardEnable;
}

static bool
fs_required(struct panvk_graphics_pipeline *pipeline)
{
   const struct pan_shader_info *info = &pipeline->state.fs.info;

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

#define PANVK_DYNAMIC_FS_RSD_MASK                                              \
   ((1 << VK_DYNAMIC_STATE_DEPTH_BIAS) |                                       \
    (1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS) |                                  \
    (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK) |                             \
    (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK) |                               \
    (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE))

static void
init_fs_state(struct panvk_graphics_pipeline *pipeline,
              const VkGraphicsPipelineCreateInfo *create_info,
              struct panvk_shader *shader)
{
   if (!shader)
      return;

   pipeline->state.fs.dynamic_rsd =
      pipeline->state.dynamic_mask & PANVK_DYNAMIC_FS_RSD_MASK;
   pipeline->state.fs.address = upload_shader(&pipeline->base, shader);
   pipeline->state.fs.info = shader->info;
   pipeline->state.fs.rt_mask = get_active_color_attachments(create_info, NULL);
   pipeline->state.fs.required = fs_required(pipeline);
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
collect_varyings(struct panvk_graphics_pipeline *pipeline,
                 struct panvk_shader **shaders)
{
   for (uint32_t s = 0; s < MESA_SHADER_STAGES; s++) {
      if (!shaders[s])
         continue;

      const struct pan_shader_info *info = &shaders[s]->info;

      for (unsigned i = 0; i < info->varyings.input_count; i++) {
         update_varying_slot(&pipeline->varyings, s, &info->varyings.input[i],
                             true);
      }

      for (unsigned i = 0; i < info->varyings.output_count; i++) {
         update_varying_slot(&pipeline->varyings, s, &info->varyings.output[i],
                             false);
      }
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

static void
parse_vertex_input(struct panvk_graphics_pipeline *pipeline,
                   const VkGraphicsPipelineCreateInfo *create_info,
                   struct panvk_shader **shaders)
{
   struct panvk_attribs_info *attribs = &pipeline->state.vs.attribs;
   const VkPipelineVertexInputStateCreateInfo *info =
      create_info->pVertexInputState;

   const VkPipelineVertexInputDivisorStateCreateInfoEXT *div_info =
      vk_find_struct_const(info->pNext,
                           PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);

   for (unsigned i = 0; i < info->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &info->pVertexBindingDescriptions[i];
      attribs->buf_count = MAX2(desc->binding + 1, attribs->buf_count);
      attribs->buf[desc->binding].stride = desc->stride;
      attribs->buf[desc->binding].per_instance =
         desc->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
      attribs->buf[desc->binding].instance_divisor = 1;
   }

   if (div_info) {
      for (unsigned i = 0; i < div_info->vertexBindingDivisorCount; i++) {
         const VkVertexInputBindingDivisorDescriptionEXT *div =
            &div_info->pVertexBindingDivisors[i];
         attribs->buf[div->binding].instance_divisor = div->divisor;
      }
   }

   const struct pan_shader_info *vs = &shaders[MESA_SHADER_VERTEX]->info;

   attribs->attrib_count = 0;
   for (unsigned i = 0; i < info->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &info->pVertexAttributeDescriptions[i];

      unsigned attrib = desc->location + VERT_ATTRIB_GENERIC0;
      unsigned slot =
         util_bitcount64(vs->attributes_read & BITFIELD64_MASK(attrib));

      attribs->attrib[slot].buf = desc->binding;
      attribs->attrib[slot].format = vk_format_to_pipe_format(desc->format);
      attribs->attrib[slot].offset = desc->offset;
      attribs->attrib_count = MAX2(attribs->attrib_count, slot + 1);
   }
}

static VkResult
panvk_graphics_pipeline_create(struct panvk_device *dev,
                               struct vk_pipeline_cache *cache,
                               const VkGraphicsPipelineCreateInfo *create_info,
                               const VkAllocationCallbacks *alloc,
                               struct panvk_pipeline **out)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, layout, create_info->layout);
   struct panvk_graphics_pipeline *gfx_pipeline = vk_object_zalloc(
      &dev->vk, alloc, sizeof(*gfx_pipeline), VK_OBJECT_TYPE_PIPELINE);

   if (!gfx_pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out = &gfx_pipeline->base;
   gfx_pipeline->base.layout = layout;
   gfx_pipeline->base.type = PANVK_PIPELINE_GRAPHICS;

   panvk_pool_init(&gfx_pipeline->base.bin_pool, dev, NULL,
                   PAN_KMOD_BO_FLAG_EXECUTABLE, 4096,
                   "Pipeline shader binaries", false);
   panvk_pool_init(&gfx_pipeline->base.desc_pool, dev, NULL, 0, 4096,
                   "Pipeline static state", false);

   struct panvk_shader *shaders[MESA_SHADER_STAGES] = {NULL};

   parse_dynamic_state(gfx_pipeline, create_info);
   parse_color_blend(gfx_pipeline, create_info);
   compile_shaders(&gfx_pipeline->base, create_info->pStages,
                   create_info->stageCount, alloc, shaders);
   collect_varyings(gfx_pipeline, shaders);
   parse_input_assembly(gfx_pipeline, create_info);
   parse_multisample(gfx_pipeline, create_info);
   parse_zs(gfx_pipeline, create_info);
   parse_rast(gfx_pipeline, create_info);
   parse_vertex_input(gfx_pipeline, create_info, shaders);
   init_fs_state(gfx_pipeline, create_info, shaders[MESA_SHADER_FRAGMENT]);
   init_shaders(&gfx_pipeline->base, create_info, shaders);
   parse_viewport(gfx_pipeline, create_info);

   release_shaders(&gfx_pipeline->base, shaders, alloc);
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

   struct panvk_shader *shaders[MESA_SHADER_STAGES] = {NULL};

   compile_shaders(&compute_pipeline->base, &create_info->stage, 1, alloc,
                   shaders);
   init_shaders(&compute_pipeline->base, NULL, shaders);

   release_shaders(&compute_pipeline->base, shaders, alloc);
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
