/*
 * Copyright (C) 2021 Alyssa Rosenzweig
 * Copyright (C) 2020-2021 Collabora, Ltd.
 * Copyright (C) 2014 Broadcom
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "agx_state.h"
#include "compiler/nir/nir_builder.h"
#include "asahi/compiler/agx_compile.h"
#include "gallium/auxiliary/util/u_blitter.h"
#include "gallium/auxiliary/util/u_dump.h"

void
agx_blitter_save(struct agx_context *ctx, struct blitter_context *blitter,
                 bool render_cond)
{
   util_blitter_save_vertex_buffer_slot(blitter, ctx->vertex_buffers);
   util_blitter_save_vertex_elements(blitter, ctx->attributes);
   util_blitter_save_vertex_shader(blitter, ctx->stage[PIPE_SHADER_VERTEX].shader);
   util_blitter_save_rasterizer(blitter, ctx->rast);
   util_blitter_save_viewport(blitter, &ctx->viewport);
   util_blitter_save_scissor(blitter, &ctx->scissor);
   util_blitter_save_fragment_shader(blitter, ctx->stage[PIPE_SHADER_FRAGMENT].shader);
   util_blitter_save_blend(blitter, ctx->blend);
   util_blitter_save_depth_stencil_alpha(blitter, ctx->zs);
   util_blitter_save_stencil_ref(blitter, &ctx->stencil_ref);
   util_blitter_save_so_targets(blitter, 0, NULL);
   util_blitter_save_sample_mask(blitter, ctx->sample_mask, 0);

   util_blitter_save_framebuffer(blitter, &ctx->framebuffer);
   util_blitter_save_fragment_sampler_states(blitter,
         ctx->stage[PIPE_SHADER_FRAGMENT].sampler_count,
         (void **)(ctx->stage[PIPE_SHADER_FRAGMENT].samplers));
   util_blitter_save_fragment_sampler_views(blitter,
         ctx->stage[PIPE_SHADER_FRAGMENT].texture_count,
         (struct pipe_sampler_view **)ctx->stage[PIPE_SHADER_FRAGMENT].textures);
   util_blitter_save_fragment_constant_buffer_slot(blitter,
         ctx->stage[PIPE_SHADER_FRAGMENT].cb);

   if (!render_cond) {
      util_blitter_save_render_condition(blitter,
            (struct pipe_query *) ctx->cond_query,
            ctx->cond_cond, ctx->cond_mode);
   }
}

void
agx_blit(struct pipe_context *pipe,
              const struct pipe_blit_info *info)
{
   //if (info->render_condition_enable &&
   //    !agx_render_condition_check(pan_context(pipe)))
   //        return;

   struct agx_context *ctx = agx_context(pipe);

   if (!util_blitter_is_blit_supported(ctx->blitter, info)) {
      fprintf(stderr, "\n");
      util_dump_blit_info(stderr, info);
      fprintf(stderr, "\n\n");
      unreachable("Unsupported blit");
   }

   agx_blitter_save(ctx, ctx->blitter, info->render_condition_enable);
   util_blitter_blit(ctx->blitter, info);
}
