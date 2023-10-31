/*
 * Copyright (C) 2023 Collabora, Ltd.
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
 *
 * Authors:
 *   Louis-Francis Ratté-Boulianne <lfrb@collabora.com>
 */

#include "pan_texture.h"

/* Arm Fixed-Rate Compression (AFRC) is a lossy compression scheme natively
 * implemented in Mali GPUs. AFRC images can only be rendered or textured
 * from. It is currently not possible to do image reads or writes to such
 * resources.
 *
 * AFRC divides the image into an array of fixed-size coding units which are
 * grouped into paging tiles. The size of the coding units (clump size)
 * depends on the image format and the pixel layout (whether it is optimized
 * for 2D locality and rotation, or for scan line order access). The last
 * parameter is the size of the compressed block that can be either 16, 24,
 * or 32 bytes.
 *
 * The compression rate can be calculated by dividing the compressed block
 * size by the uncompressed block size (clump size multiplied by the component
 * size and the number of components).
 */

struct pan_afrc_format_info
panfrost_afrc_get_format_info(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   struct pan_afrc_format_info info = {0};

   /* No AFRC(ZS). */
   if (desc->colorspace == UTIL_FORMAT_COLORSPACE_ZS)
      return info;

   unsigned bpc = 0;
   for (unsigned c = 0; c < desc->nr_channels; c++) {
      if (bpc && bpc != desc->channel[c].size)
         return info;

      bpc = desc->channel[0].size;
   }

   info.bpc = bpc;

   if (desc->colorspace == UTIL_FORMAT_COLORSPACE_YUV) {
      if (desc->layout != UTIL_FORMAT_LAYOUT_SUBSAMPLED)
         info.ichange_fmt = PAN_AFRC_ICHANGE_FORMAT_YUV444;
      else if (util_format_is_subsampled_422(format))
         info.ichange_fmt = PAN_AFRC_ICHANGE_FORMAT_YUV422;
      else
         info.ichange_fmt = PAN_AFRC_ICHANGE_FORMAT_YUV420;
   } else {
      assert(desc->colorspace == UTIL_FORMAT_COLORSPACE_RGB ||
             desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);
      info.ichange_fmt = PAN_AFRC_ICHANGE_FORMAT_RAW;
   }

   info.num_planes = util_format_get_num_planes(format);
   info.num_comps = util_format_get_nr_components(format);
   return info;
}
