/**************************************************************************
 *
 * Copyright 2018-2019 Alyssa Rosenzweig
 * Copyright 2018-2019 Collabora, Ltd.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef PAN_SCREEN_H
#define PAN_SCREEN_H

#include <xf86drm.h>
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "renderonly/renderonly.h"
#include "util/bitset.h"
#include "util/disk_cache.h"
#include "util/log.h"
#include "util/set.h"
#include "util/u_dynarray.h"

#include "pan_device.h"
#include "pan_mempool.h"

#define PAN_QUERY_DRAW_CALLS (PIPE_QUERY_DRIVER_SPECIFIC + 0)

static const struct pipe_driver_query_info panfrost_driver_query_list[] = {
   {"draw-calls", PAN_QUERY_DRAW_CALLS, {0}},
};

struct panfrost_batch;
struct panfrost_context;
struct panfrost_resource;
struct panfrost_compiled_shader;
struct pan_fb_info;
struct pan_blend_state;

/* Virtual table of per-generation (GenXML) functions */

struct panfrost_vtable {
   /* Prepares the renderer state descriptor or shader program descriptor
    * for a given compiled shader, and if desired uploads it as well */
   void (*prepare_shader)(struct panfrost_compiled_shader *,
                          struct panfrost_pool *, bool);

   /* Emits a thread local storage descriptor */
   void (*emit_tls)(struct panfrost_batch *);

   /* Emits a framebuffer descriptor */
   void (*emit_fbd)(struct panfrost_batch *, const struct pan_fb_info *);

   /* Emits a fragment job */
   mali_ptr (*emit_fragment_job)(struct panfrost_batch *,
                                 const struct pan_fb_info *);

   /* General destructor */
   void (*screen_destroy)(struct pipe_screen *);

   /* Preload framebuffer */
   void (*preload)(struct panfrost_batch *, struct pan_fb_info *);

   /* Initialize a Gallium context */
   void (*context_init)(struct pipe_context *pipe);

   /* Device-dependent initialization of a panfrost_batch */
   void (*init_batch)(struct panfrost_batch *batch);

   /* Get blend shader */
   struct pan_blend_shader_variant *(*get_blend_shader)(
      const struct panfrost_device *, const struct pan_blend_state *,
      nir_alu_type, nir_alu_type, unsigned rt);

   /* Initialize the polygon list */
   void (*init_polygon_list)(struct panfrost_batch *);

   /* Shader compilation methods */
   const nir_shader_compiler_options *(*get_compiler_options)(void);
   void (*compile_shader)(nir_shader *s, struct panfrost_compile_inputs *inputs,
                          struct util_dynarray *binary,
                          struct pan_shader_info *info);
};

struct panfrost_screen {
   struct pipe_screen base;
   struct panfrost_device dev;
   struct {
      struct panfrost_pool bin_pool;
      struct panfrost_pool desc_pool;
   } blitter;

   struct panfrost_vtable vtbl;
   struct disk_cache *disk_cache;
};

static inline struct panfrost_screen *
pan_screen(struct pipe_screen *p)
{
   return (struct panfrost_screen *)p;
}

static inline struct panfrost_device *
pan_device(struct pipe_screen *p)
{
   return &(pan_screen(p)->dev);
}

int panfrost_get_driver_query_info(struct pipe_screen *pscreen, unsigned index,
                                   struct pipe_driver_query_info *info);

void panfrost_cmdstream_screen_init_v4(struct panfrost_screen *screen);
void panfrost_cmdstream_screen_init_v5(struct panfrost_screen *screen);
void panfrost_cmdstream_screen_init_v6(struct panfrost_screen *screen);
void panfrost_cmdstream_screen_init_v7(struct panfrost_screen *screen);
void panfrost_cmdstream_screen_init_v9(struct panfrost_screen *screen);

#define perf_debug(dev, ...)                                                   \
   do {                                                                        \
      if (unlikely((dev)->debug & PAN_DBG_PERF))                               \
         mesa_logw(__VA_ARGS__);                                               \
   } while (0)

#define perf_debug_ctx(ctx, ...)                                               \
   perf_debug(pan_device((ctx)->base.screen), __VA_ARGS__);

#endif /* PAN_SCREEN_H */
