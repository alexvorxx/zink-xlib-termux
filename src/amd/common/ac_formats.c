/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_formats.h"

#include "sid.h"

uint32_t
ac_translate_buffer_numformat(const struct util_format_description *desc,
                              int first_non_void)
{
   if (desc->format == PIPE_FORMAT_R11G11B10_FLOAT)
      return V_008F0C_BUF_NUM_FORMAT_FLOAT;

   assert(first_non_void >= 0);

   switch (desc->channel[first_non_void].type) {
   case UTIL_FORMAT_TYPE_SIGNED:
   case UTIL_FORMAT_TYPE_FIXED:
      if (desc->channel[first_non_void].size >= 32 || desc->channel[first_non_void].pure_integer)
         return V_008F0C_BUF_NUM_FORMAT_SINT;
      else if (desc->channel[first_non_void].normalized)
         return V_008F0C_BUF_NUM_FORMAT_SNORM;
      else
         return V_008F0C_BUF_NUM_FORMAT_SSCALED;
      break;
   case UTIL_FORMAT_TYPE_UNSIGNED:
      if (desc->channel[first_non_void].size >= 32 || desc->channel[first_non_void].pure_integer)
         return V_008F0C_BUF_NUM_FORMAT_UINT;
      else if (desc->channel[first_non_void].normalized)
         return V_008F0C_BUF_NUM_FORMAT_UNORM;
      else
         return V_008F0C_BUF_NUM_FORMAT_USCALED;
      break;
   case UTIL_FORMAT_TYPE_FLOAT:
   default:
      return V_008F0C_BUF_NUM_FORMAT_FLOAT;
   }
}

uint32_t
ac_translate_buffer_dataformat(const struct util_format_description *desc,
                               int first_non_void)
{
   int i;

   if (desc->format == PIPE_FORMAT_R11G11B10_FLOAT)
      return V_008F0C_BUF_DATA_FORMAT_10_11_11;

   assert(first_non_void >= 0);

   if (desc->nr_channels == 4 && desc->channel[0].size == 10 && desc->channel[1].size == 10 &&
       desc->channel[2].size == 10 && desc->channel[3].size == 2)
      return V_008F0C_BUF_DATA_FORMAT_2_10_10_10;

   /* See whether the components are of the same size. */
   for (i = 0; i < desc->nr_channels; i++) {
      if (desc->channel[first_non_void].size != desc->channel[i].size)
         return V_008F0C_BUF_DATA_FORMAT_INVALID;
   }

   switch (desc->channel[first_non_void].size) {
   case 8:
      switch (desc->nr_channels) {
      case 1:
      case 3: /* 3 loads */
         return V_008F0C_BUF_DATA_FORMAT_8;
      case 2:
         return V_008F0C_BUF_DATA_FORMAT_8_8;
      case 4:
         return V_008F0C_BUF_DATA_FORMAT_8_8_8_8;
      }
      break;
   case 16:
      switch (desc->nr_channels) {
      case 1:
      case 3: /* 3 loads */
         return V_008F0C_BUF_DATA_FORMAT_16;
      case 2:
         return V_008F0C_BUF_DATA_FORMAT_16_16;
      case 4:
         return V_008F0C_BUF_DATA_FORMAT_16_16_16_16;
      }
      break;
   case 32:
      switch (desc->nr_channels) {
      case 1:
         return V_008F0C_BUF_DATA_FORMAT_32;
      case 2:
         return V_008F0C_BUF_DATA_FORMAT_32_32;
      case 3:
         return V_008F0C_BUF_DATA_FORMAT_32_32_32;
      case 4:
         return V_008F0C_BUF_DATA_FORMAT_32_32_32_32;
      }
      break;
   case 64:
      /* Legacy double formats. */
      switch (desc->nr_channels) {
      case 1: /* 1 load */
         return V_008F0C_BUF_DATA_FORMAT_32_32;
      case 2: /* 1 load */
         return V_008F0C_BUF_DATA_FORMAT_32_32_32_32;
      case 3: /* 3 loads */
         return V_008F0C_BUF_DATA_FORMAT_32_32;
      case 4: /* 2 loads */
         return V_008F0C_BUF_DATA_FORMAT_32_32_32_32;
      }
      break;
   }

   return V_008F0C_BUF_DATA_FORMAT_INVALID;
}

uint32_t
ac_translate_tex_numformat(const struct util_format_description *desc,
                           int first_non_void)
{
   uint32_t num_format;

   switch (desc->format) {
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
      num_format = V_008F14_IMG_NUM_FORMAT_UNORM;
      break;
   default:
      if (first_non_void < 0) {
         if (util_format_is_compressed(desc->format)) {
            switch (desc->format) {
            case PIPE_FORMAT_DXT1_SRGB:
            case PIPE_FORMAT_DXT1_SRGBA:
            case PIPE_FORMAT_DXT3_SRGBA:
            case PIPE_FORMAT_DXT5_SRGBA:
            case PIPE_FORMAT_BPTC_SRGBA:
            case PIPE_FORMAT_ETC2_SRGB8:
            case PIPE_FORMAT_ETC2_SRGB8A1:
            case PIPE_FORMAT_ETC2_SRGBA8:
               num_format = V_008F14_IMG_NUM_FORMAT_SRGB;
               break;
            case PIPE_FORMAT_RGTC1_SNORM:
            case PIPE_FORMAT_LATC1_SNORM:
            case PIPE_FORMAT_RGTC2_SNORM:
            case PIPE_FORMAT_LATC2_SNORM:
            case PIPE_FORMAT_ETC2_R11_SNORM:
            case PIPE_FORMAT_ETC2_RG11_SNORM:
            /* implies float, so use SNORM/UNORM to determine
               whether data is signed or not */
            case PIPE_FORMAT_BPTC_RGB_FLOAT:
               num_format = V_008F14_IMG_NUM_FORMAT_SNORM;
               break;
            default:
               num_format = V_008F14_IMG_NUM_FORMAT_UNORM;
               break;
            }
         } else if (desc->layout == UTIL_FORMAT_LAYOUT_SUBSAMPLED) {
            num_format = V_008F14_IMG_NUM_FORMAT_UNORM;
         } else {
            num_format = V_008F14_IMG_NUM_FORMAT_FLOAT;
         }
      } else if (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB) {
         num_format = V_008F14_IMG_NUM_FORMAT_SRGB;
      } else {
         switch (desc->channel[first_non_void].type) {
         case UTIL_FORMAT_TYPE_FLOAT:
            num_format = V_008F14_IMG_NUM_FORMAT_FLOAT;
            break;
         case UTIL_FORMAT_TYPE_SIGNED:
            if (desc->channel[first_non_void].normalized)
               num_format = V_008F14_IMG_NUM_FORMAT_SNORM;
            else if (desc->channel[first_non_void].pure_integer)
               num_format = V_008F14_IMG_NUM_FORMAT_SINT;
            else
               num_format = V_008F14_IMG_NUM_FORMAT_SSCALED;
            break;
         case UTIL_FORMAT_TYPE_UNSIGNED:
            if (desc->channel[first_non_void].normalized)
               num_format = V_008F14_IMG_NUM_FORMAT_UNORM;
            else if (desc->channel[first_non_void].pure_integer)
               num_format = V_008F14_IMG_NUM_FORMAT_UINT;
            else
               num_format = V_008F14_IMG_NUM_FORMAT_USCALED;
            break;
         default:
            num_format = V_008F14_IMG_NUM_FORMAT_UNORM;
            break;
         }
      }
   }

   return num_format;
}
