/*
 * Copyright © 2022 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#include "nil_format.h"
#include "nil_format_table.h"

#include "nouveau_device.h"

#include "cla297.h"
#include "clb097.h"

bool
nil_format_supports_texturing(struct nv_device_info *dev,
                              enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   if (!(fmt->support & NIL_FORMAT_SUPPORTS_TEXTURE_BIT))
      return false;

   const struct util_format_description *desc = util_format_description(format);
   if (desc->layout == UTIL_FORMAT_LAYOUT_ETC ||
       desc->layout == UTIL_FORMAT_LAYOUT_ASTC) {
      return dev->type == NV_DEVICE_TYPE_SOC && dev->cls_eng3d >= KEPLER_C;
   }

   return true;
}

bool
nil_format_supports_filtering(struct nv_device_info *dev,
                              enum pipe_format format)
{
   return nil_format_supports_texturing(dev, format) &&
          !util_format_is_pure_integer(format);
}

bool
nil_format_supports_buffer(struct nv_device_info *dev,
                           enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   return fmt->support & NIL_FORMAT_SUPPORTS_BUFFER_BIT;
}

bool
nil_format_supports_storage(struct nv_device_info *dev,
                            enum pipe_format format)
{
   if ((format == PIPE_FORMAT_R64_UINT || format == PIPE_FORMAT_R64_SINT) &&
       dev->cls_eng3d < MAXWELL_A)
      return false;

   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   return fmt->support & NIL_FORMAT_SUPPORTS_STORAGE_BIT;
}

bool
nil_format_supports_color_targets(struct nv_device_info *dev,
                                  enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   return fmt->support & NIL_FORMAT_SUPPORTS_RENDER_BIT;
}

bool
nil_format_supports_blending(struct nv_device_info *dev,
                             enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   return fmt->support & NIL_FORMAT_SUPPORTS_ALPHA_BLEND_BIT;
}

bool
nil_format_supports_depth_stencil(struct nv_device_info *dev,
                                  enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   return fmt->support & NIL_FORMAT_SUPPORTS_DEPTH_STENCIL_BIT;
}

uint8_t
nil_format_to_color_target(enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   assert(fmt->support & NIL_FORMAT_SUPPORTS_RENDER_BIT);
   return fmt->czt;
}

uint8_t
nil_format_to_depth_stencil(enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   assert(fmt->support & NIL_FORMAT_SUPPORTS_DEPTH_STENCIL_BIT);
   return fmt->czt;
}

const struct nil_tic_format *
nil_tic_format_for_pipe(enum pipe_format format)
{
   assert(format < PIPE_FORMAT_COUNT);
   const struct nil_format_info *fmt = &nil_format_table[format];
   return fmt->tic.comp_sizes == 0 ? NULL : &fmt->tic;
}
