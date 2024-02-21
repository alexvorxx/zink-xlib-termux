/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdio.h>

#include "decoder/intel_decoder.h"

void read_xe_data_file(FILE *file, enum intel_batch_decode_flags batch_flags);
