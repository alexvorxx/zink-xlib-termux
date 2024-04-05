/*
 * Copyright © 2022 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#ifndef NIL_H
#define NIL_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "util/macros.h"
#include "util/format/u_format.h"

#include "nil_rs.h"

struct nv_device_info;

enum ENUM_PACKED nil_view_type {
   NIL_VIEW_TYPE_1D,
   NIL_VIEW_TYPE_2D,
   NIL_VIEW_TYPE_3D,
   NIL_VIEW_TYPE_3D_SLICED,
   NIL_VIEW_TYPE_CUBE,
   NIL_VIEW_TYPE_1D_ARRAY,
   NIL_VIEW_TYPE_2D_ARRAY,
   NIL_VIEW_TYPE_CUBE_ARRAY,
};

struct nil_view {
   enum nil_view_type type;

   /**
    * The format to use in the view
    *
    * This may differ from the format of the actual isl_surf but must have
    * the same block size.
    */
   enum pipe_format format;

   uint32_t base_level;
   uint32_t num_levels;

   /**
    * Base array layer
    *
    * For cube maps, both base_array_layer and array_len should be
    * specified in terms of 2-D layers and must be a multiple of 6.
    */
   uint32_t base_array_layer;

   /**
    * Array Length
    *
    * Indicates the number of array elements starting at  Base Array Layer.
    */
   uint32_t array_len;

   enum pipe_swizzle swizzle[4];

   /* VK_EXT_image_view_min_lod */
   float min_lod_clamp;
};

void nil_image_fill_tic(struct nv_device_info *dev,
                        const struct nil_image *image,
                        const struct nil_view *view,
                        uint64_t base_address,
                        void *desc_out);

void nil_buffer_fill_tic(struct nv_device_info *dev,
                         uint64_t base_address,
                         enum pipe_format format,
                         uint32_t num_elements,
                         void *desc_out);

#endif /* NIL_H */
