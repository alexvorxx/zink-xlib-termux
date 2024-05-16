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
