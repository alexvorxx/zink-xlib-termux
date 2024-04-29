/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct intel_perf_config;
struct intel_perf_registers;

uint64_t xe_perf_get_oa_format(struct intel_perf_config *perf);

bool xe_oa_metrics_available(struct intel_perf_config *perf, int fd, bool use_register_snapshots);

uint64_t xe_add_config(struct intel_perf_config *perf, int fd, const struct intel_perf_registers *config, const char *guid);
void xe_remove_config(struct intel_perf_config *perf, int fd, uint64_t config_id);
