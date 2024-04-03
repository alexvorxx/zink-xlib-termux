/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum xe_topic {
   XE_TOPIC_DEVICE = 0,
   XE_TOPIC_GUC_CT,
   XE_TOPIC_JOB,
   XE_TOPIC_HW_ENGINES,
   XE_TOPIC_VM,
   XE_TOPIC_INVALID,
};

bool error_decode_xe_read_u64_hexacimal_parameter(const char *line, const char *parameter, uint64_t *value);
bool error_decode_xe_read_hexacimal_parameter(const char *line, const char *parameter, int *value);
bool error_decode_xe_read_engine_name(const char *line, char *ring_name);

bool error_decode_xe_decode_topic(const char *line, enum xe_topic *new_topic);
