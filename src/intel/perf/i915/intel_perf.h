/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct intel_perf_config;

uint64_t i915_perf_get_oa_format(struct intel_perf_config *perf);

int i915_perf_stream_open(struct intel_perf_config *perf_config, int drm_fd,
                          uint32_t ctx_id, uint64_t metrics_set_id,
                          uint64_t report_format, uint64_t period_exponent,
                          bool hold_preemption, bool enable);
