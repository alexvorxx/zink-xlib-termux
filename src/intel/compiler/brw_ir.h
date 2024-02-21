/* -*- c++ -*- */
/*
 * Copyright © 2010-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef BRW_IR_H
#define BRW_IR_H

#include <assert.h>
#include "brw_reg.h"
#include "compiler/glsl/list.h"

#define MAX_SAMPLER_MESSAGE_SIZE 11

/* The sampler can return a vec5 when sampling with sparse residency. In
 * SIMD32, each component takes up 4 GRFs, so we need to allow up to size-20
 * VGRFs to hold the result.
 */
#define MAX_VGRF_SIZE(devinfo) ((devinfo)->ver >= 20 ? 40 : 20)

#ifdef __cplusplus
struct backend_reg : private brw_reg
{
   backend_reg() {}
   backend_reg(const struct brw_reg &reg) : brw_reg(reg), offset(0) {}

   const brw_reg &as_brw_reg() const
   {
      assert(file == ARF || file == FIXED_GRF || file == IMM);
      assert(offset == 0);
      return static_cast<const brw_reg &>(*this);
   }

   brw_reg &as_brw_reg()
   {
      assert(file == ARF || file == FIXED_GRF || file == IMM);
      assert(offset == 0);
      return static_cast<brw_reg &>(*this);
   }

   bool equals(const backend_reg &r) const;
   bool negative_equals(const backend_reg &r) const;

   bool is_zero() const;
   bool is_one() const;
   bool is_negative_one() const;
   bool is_null() const;
   bool is_accumulator() const;

   /** Offset from the start of the (virtual) register in bytes. */
   uint16_t offset;

   using brw_reg::type;
   using brw_reg::file;
   using brw_reg::negate;
   using brw_reg::abs;
   using brw_reg::address_mode;
   using brw_reg::subnr;
   using brw_reg::nr;

   using brw_reg::swizzle;
   using brw_reg::writemask;
   using brw_reg::indirect_offset;
   using brw_reg::vstride;
   using brw_reg::width;
   using brw_reg::hstride;

   using brw_reg::df;
   using brw_reg::f;
   using brw_reg::d;
   using brw_reg::ud;
   using brw_reg::d64;
   using brw_reg::u64;
};

struct bblock_t;

#endif

#endif
