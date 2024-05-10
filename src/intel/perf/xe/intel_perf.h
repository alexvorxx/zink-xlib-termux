/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

struct intel_perf_config;

uint64_t xe_perf_get_oa_format(struct intel_perf_config *perf);
