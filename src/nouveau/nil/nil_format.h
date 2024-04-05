/*
 * Copyright © 2022 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */
#ifndef NIL_FORMAT_H
#define NIL_FORMAT_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "util/format/u_format.h"

struct nv_device_info;

/* We don't have our own format enum; we use PIPE_FORMAT for everything */

bool nil_format_supports_texturing(struct nv_device_info *dev,
                                   enum pipe_format format);

bool nil_format_supports_filtering(struct nv_device_info *dev,
                                   enum pipe_format format);

bool nil_format_supports_buffer(struct nv_device_info *dev,
                                enum pipe_format format);

bool nil_format_supports_storage(struct nv_device_info *dev,
                                 enum pipe_format format);

bool nil_format_supports_color_targets(struct nv_device_info *dev,
                                       enum pipe_format format);

bool nil_format_supports_blending(struct nv_device_info *dev,
                                  enum pipe_format format);

bool nil_format_supports_depth_stencil(struct nv_device_info *dev,
                                       enum pipe_format format);

uint8_t nil_format_to_color_target(enum pipe_format format);

uint8_t nil_format_to_depth_stencil(enum pipe_format format);

#endif /* NIL_FORMAT_H */
