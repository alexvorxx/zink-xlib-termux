/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#ifndef __AGX_TILEBUFFER_H
#define __AGX_TILEBUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "util/format/u_formats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to keep the header lean */
struct nir_shader;
struct agx_usc_builder;

struct agx_tile_size {
   uint8_t width;
   uint8_t height;
};

struct agx_tilebuffer_layout {
   /* Logical format of each render target. Use agx_tilebuffer_physical_format
    * to get the physical format.
    */
   enum pipe_format logical_format[8];

   /* Offset into the sample of each render target */
   uint8_t offset_B[8];

   /* Total bytes per sample, rounded up as needed */
   uint8_t sample_size_B;

   /* Number of samples per pixel */
   uint8_t nr_samples;

   /* Selected tile size */
   struct agx_tile_size tile_size;
};

struct agx_tilebuffer_layout
agx_build_tilebuffer_layout(enum pipe_format *formats, uint8_t nr_cbufs, uint8_t nr_samples);

bool
agx_nir_lower_tilebuffer(struct nir_shader *shader, struct agx_tilebuffer_layout *tib);

void
agx_usc_tilebuffer(struct agx_usc_builder *b, struct agx_tilebuffer_layout *tib);

uint32_t
agx_tilebuffer_total_size(struct agx_tilebuffer_layout *tib);

enum pipe_format
agx_tilebuffer_physical_format(struct agx_tilebuffer_layout *tib, unsigned rt);

#ifdef __cplusplus
} /* extern C */
#endif

#endif
