/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

uint32_t intel_compute_slm_calculate_size(unsigned gen, uint32_t bytes);
uint32_t intel_compute_slm_encode_size(unsigned gen, uint32_t bytes);
uint32_t intel_compute_preferred_slm_encode_size(unsigned gen, uint32_t bytes);
