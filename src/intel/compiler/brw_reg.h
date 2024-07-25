/*
 Copyright (C) Intel Corp.  2006.  All Rights Reserved.
 Intel funded Tungsten Graphics to
 develop this 3D driver.

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the
 "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish,
 distribute, sublicense, and/or sell copies of the Software, and to
 permit persons to whom the Software is furnished to do so, subject to
 the following conditions:

 The above copyright notice and this permission notice (including the
 next paragraph) shall be included in all copies or substantial
 portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **********************************************************************/
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  */

/** @file
 *
 * This file defines struct brw_reg, which is our representation for EU
 * registers.  They're not a hardware specific format, just an abstraction
 * that intends to capture the full flexibility of the hardware registers.
 *
 * The brw_eu_emit.c layer's brw_set_dest/brw_set_src[01] functions encode
 * the abstract brw_reg type into the actual hardware instruction encoding.
 */

#ifndef BRW_REG_H
#define BRW_REG_H

#include <stdbool.h>
#include "util/compiler.h"
#include "util/glheader.h"
#include "util/macros.h"
#include "util/rounding.h"
#include "util/u_math.h"
#include "brw_eu_defines.h"
#include "brw_reg_type.h"

#ifdef __cplusplus
extern "C" {
#endif

struct intel_device_info;

/** Size of general purpose register space in REG_SIZE units */
#define BRW_MAX_GRF 128
#define XE2_MAX_GRF 256

/**
 * BRW hardware swizzles.
 * Only defines XYZW to ensure it can be contained in 2 bits
 */
#define BRW_SWIZZLE_X 0
#define BRW_SWIZZLE_Y 1
#define BRW_SWIZZLE_Z 2
#define BRW_SWIZZLE_W 3

#define BRW_SWIZZLE4(a,b,c,d) (((a)<<0) | ((b)<<2) | ((c)<<4) | ((d)<<6))
#define BRW_GET_SWZ(swz, idx) (((swz) >> ((idx)*2)) & 0x3)

#define BRW_SWIZZLE_NOOP      BRW_SWIZZLE4(0,1,2,3)
#define BRW_SWIZZLE_XYZW      BRW_SWIZZLE4(0,1,2,3)
#define BRW_SWIZZLE_XXXX      BRW_SWIZZLE4(0,0,0,0)
#define BRW_SWIZZLE_YYYY      BRW_SWIZZLE4(1,1,1,1)
#define BRW_SWIZZLE_ZZZZ      BRW_SWIZZLE4(2,2,2,2)
#define BRW_SWIZZLE_WWWW      BRW_SWIZZLE4(3,3,3,3)
#define BRW_SWIZZLE_XYXY      BRW_SWIZZLE4(0,1,0,1)
#define BRW_SWIZZLE_YXYX      BRW_SWIZZLE4(1,0,1,0)
#define BRW_SWIZZLE_XZXZ      BRW_SWIZZLE4(0,2,0,2)
#define BRW_SWIZZLE_YZXW      BRW_SWIZZLE4(1,2,0,3)
#define BRW_SWIZZLE_YWYW      BRW_SWIZZLE4(1,3,1,3)
#define BRW_SWIZZLE_ZXYW      BRW_SWIZZLE4(2,0,1,3)
#define BRW_SWIZZLE_ZWZW      BRW_SWIZZLE4(2,3,2,3)
#define BRW_SWIZZLE_WZWZ      BRW_SWIZZLE4(3,2,3,2)
#define BRW_SWIZZLE_WZYX      BRW_SWIZZLE4(3,2,1,0)
#define BRW_SWIZZLE_XXZZ      BRW_SWIZZLE4(0,0,2,2)
#define BRW_SWIZZLE_YYWW      BRW_SWIZZLE4(1,1,3,3)
#define BRW_SWIZZLE_YXWZ      BRW_SWIZZLE4(1,0,3,2)

#define BRW_SWZ_COMP_INPUT(comp) (BRW_SWIZZLE_XYZW >> ((comp)*2))
#define BRW_SWZ_COMP_OUTPUT(comp) (BRW_SWIZZLE_XYZW << ((comp)*2))

static inline bool
brw_is_single_value_swizzle(unsigned swiz)
{
   return (swiz == BRW_SWIZZLE_XXXX ||
           swiz == BRW_SWIZZLE_YYYY ||
           swiz == BRW_SWIZZLE_ZZZZ ||
           swiz == BRW_SWIZZLE_WWWW);
}

/**
 * Compute the swizzle obtained from the application of \p swz0 on the result
 * of \p swz1.  The argument ordering is expected to match function
 * composition.
 */
static inline unsigned
brw_compose_swizzle(unsigned swz0, unsigned swz1)
{
   return BRW_SWIZZLE4(
      BRW_GET_SWZ(swz1, BRW_GET_SWZ(swz0, 0)),
      BRW_GET_SWZ(swz1, BRW_GET_SWZ(swz0, 1)),
      BRW_GET_SWZ(swz1, BRW_GET_SWZ(swz0, 2)),
      BRW_GET_SWZ(swz1, BRW_GET_SWZ(swz0, 3)));
}

/**
 * Construct an identity swizzle for the set of enabled channels given by \p
 * mask.  The result will only reference channels enabled in the provided \p
 * mask, assuming that \p mask is non-zero.  The constructed swizzle will
 * satisfy the property that for any instruction OP and any mask:
 *
 *    brw_OP(p, brw_writemask(dst, mask),
 *           brw_swizzle(src, brw_swizzle_for_mask(mask)));
 *
 * will be equivalent to the same instruction without swizzle:
 *
 *    brw_OP(p, brw_writemask(dst, mask), src);
 */
static inline unsigned
brw_swizzle_for_mask(unsigned mask)
{
   unsigned last = (mask ? ffs(mask) - 1 : 0);
   unsigned swz[4];

   for (unsigned i = 0; i < 4; i++)
      last = swz[i] = (mask & (1 << i) ? i : last);

   return BRW_SWIZZLE4(swz[0], swz[1], swz[2], swz[3]);
}

uint32_t brw_swizzle_immediate(enum brw_reg_type type, uint32_t x, unsigned swz);

#define REG_SIZE (8*4)

/* These aren't hardware structs, just something useful for us to pass around:
 *
 * Align1 operation has a lot of control over input ranges.  Used in
 * WM programs to implement shaders decomposed into "channel serial"
 * or "structure of array" form:
 */
typedef struct brw_reg {
   union {
      struct {
         enum brw_reg_type type:5;
         enum brw_reg_file file:3;      /* :2 hardware format */
         unsigned negate:1;             /* source only */
         unsigned abs:1;                /* source only */
         unsigned address_mode:1;       /* relative addressing, hopefully! */
         unsigned pad0:16;
         unsigned subnr:5;              /* :1 in align16 */
      };
      uint32_t bits;
   };

   union {
      struct {
         unsigned nr;
         unsigned swizzle:8;      /* src only, align16 only */
         unsigned writemask:4;    /* dest only, align16 only */
         int  indirect_offset:10; /* relative addressing offset */
         unsigned vstride:4;      /* source only */
         unsigned width:3;        /* src only, align1 only */
         unsigned hstride:2;      /* align1 only */
         unsigned pad1:1;
      };

      double df;
      uint64_t u64;
      int64_t d64;
      float f;
      int   d;
      unsigned ud;
   };

   /** Offset from the start of the virtual register in bytes. */
   uint16_t offset;

   /** Register region horizontal stride of virtual registers */
   uint8_t stride;

#ifdef __cplusplus
   /* TODO: Remove this constructor to make this type a POD.  To achieve this
    * we need to make sure the zero value (currently ARF_NULL) is a good
    * replacement for BAD_FILE or make the zero value BAD_FILE.
    */
   brw_reg() {
      memset((void*)this, 0, sizeof(*this));
      this->type = BRW_TYPE_UD;
      this->stride = 1;
      this->file = BAD_FILE;
   }

   bool equals(const brw_reg &r) const;
   bool negative_equals(const brw_reg &r) const;
   bool is_contiguous() const;

   bool is_zero() const;
   bool is_one() const;
   bool is_negative_one() const;
   bool is_null() const;
   bool is_accumulator() const;

   /**
    * Return the size in bytes of a single logical component of the
    * register assuming the given execution width.
    */
   unsigned component_size(unsigned width) const;
#endif /* __cplusplus */
} brw_reg;

static inline unsigned
phys_nr(const struct intel_device_info *devinfo, const struct brw_reg reg)
{
   if (devinfo->ver >= 20) {
      if (reg.file == BRW_GENERAL_REGISTER_FILE)
         return reg.nr / 2;
      else if (reg.file == BRW_ARCHITECTURE_REGISTER_FILE &&
               reg.nr >= BRW_ARF_ACCUMULATOR &&
               reg.nr < BRW_ARF_FLAG)
         return BRW_ARF_ACCUMULATOR + (reg.nr - BRW_ARF_ACCUMULATOR) / 2;
      else
         return reg.nr;
   } else {
      return reg.nr;
   }
}

static inline unsigned
phys_subnr(const struct intel_device_info *devinfo, const struct brw_reg reg)
{
   if (devinfo->ver >= 20) {
      if (reg.file == BRW_GENERAL_REGISTER_FILE ||
          (reg.file == BRW_ARCHITECTURE_REGISTER_FILE &&
           reg.nr >= BRW_ARF_ACCUMULATOR &&
           reg.nr < BRW_ARF_FLAG))
         return (reg.nr & 1) * REG_SIZE + reg.subnr;
      else
         return reg.subnr;
   } else {
      return reg.subnr;
   }
}

static inline bool
brw_regs_equal(const struct brw_reg *a, const struct brw_reg *b)
{
   return a->bits   == b->bits &&
          a->u64    == b->u64 &&
          a->offset == b->offset &&
          a->stride == b->stride;
}

static inline bool
brw_regs_negative_equal(const struct brw_reg *a, const struct brw_reg *b)
{
   if (a->file == IMM) {
      if (a->bits != b->bits)
         return false;

      switch ((enum brw_reg_type) a->type) {
      case BRW_TYPE_UQ:
      case BRW_TYPE_Q:
         return a->d64 == -b->d64;
      case BRW_TYPE_DF:
         return a->df == -b->df;
      case BRW_TYPE_UD:
      case BRW_TYPE_D:
         return a->d == -b->d;
      case BRW_TYPE_F:
         return a->f == -b->f;
      case BRW_TYPE_VF:
         /* It is tempting to treat 0 as a negation of 0 (and -0 as a negation
          * of -0).  There are occasions where 0 or -0 is used and the exact
          * bit pattern is desired.  At the very least, changing this to allow
          * 0 as a negation of 0 causes some fp64 tests to fail on IVB.
          */
         return a->ud == (b->ud ^ 0x80808080);
      case BRW_TYPE_UW:
      case BRW_TYPE_W:
      case BRW_TYPE_UV:
      case BRW_TYPE_V:
      case BRW_TYPE_HF:
         /* FINISHME: Implement support for these types once there is
          * something in the compiler that can generate them.  Until then,
          * they cannot be tested.
          */
         return false;
      case BRW_TYPE_UB:
      case BRW_TYPE_B:
      default:
         unreachable("not reached");
      }
   } else {
      struct brw_reg tmp = *a;

      tmp.negate = !tmp.negate;

      return brw_regs_equal(&tmp, b);
   }
}

static inline enum brw_reg_type
get_exec_type(const enum brw_reg_type type)
{
   switch (type) {
   case BRW_TYPE_B:
   case BRW_TYPE_V:
      return BRW_TYPE_W;
   case BRW_TYPE_UB:
   case BRW_TYPE_UV:
      return BRW_TYPE_UW;
   case BRW_TYPE_VF:
      return BRW_TYPE_F;
   default:
      return type;
   }
}

/**
 * Return an integer type of the requested size and signedness.
 */
static inline enum brw_reg_type
brw_int_type(unsigned sz, bool is_signed)
{
   switch (sz) {
   case 1:
      return (is_signed ? BRW_TYPE_B : BRW_TYPE_UB);
   case 2:
      return (is_signed ? BRW_TYPE_W : BRW_TYPE_UW);
   case 4:
      return (is_signed ? BRW_TYPE_D : BRW_TYPE_UD);
   case 8:
      return (is_signed ? BRW_TYPE_Q : BRW_TYPE_UQ);
   default:
      unreachable("Not reached.");
   }
}

/**
 * Construct a brw_reg.
 * \param file      one of the BRW_x_REGISTER_FILE values
 * \param nr        register number/index
 * \param subnr     register sub number
 * \param negate    register negate modifier
 * \param abs       register abs modifier
 * \param type      one of BRW_TYPE_x
 * \param vstride   one of BRW_VERTICAL_STRIDE_x
 * \param width     one of BRW_WIDTH_x
 * \param hstride   one of BRW_HORIZONTAL_STRIDE_x
 * \param swizzle   one of BRW_SWIZZLE_x
 * \param writemask WRITEMASK_X/Y/Z/W bitfield
 */
static inline struct brw_reg
brw_make_reg(enum brw_reg_file file,
             unsigned nr,
             unsigned subnr,
             unsigned negate,
             unsigned abs,
             enum brw_reg_type type,
             unsigned vstride,
             unsigned width,
             unsigned hstride,
             unsigned swizzle,
             unsigned writemask)
{
   struct brw_reg reg;
   if (file == BRW_GENERAL_REGISTER_FILE)
      assert(nr < XE2_MAX_GRF);
   else if (file == BRW_ARCHITECTURE_REGISTER_FILE)
      assert(nr <= BRW_ARF_TIMESTAMP);

   reg.type = type;
   reg.file = file;
   reg.negate = negate;
   reg.abs = abs;
   reg.address_mode = BRW_ADDRESS_DIRECT;
   reg.pad0 = 0;
   reg.subnr = subnr * brw_type_size_bytes(type);
   reg.nr = nr;

   /* Could do better: If the reg is r5.3<0;1,0>, we probably want to
    * set swizzle and writemask to W, as the lower bits of subnr will
    * be lost when converted to align16.  This is probably too much to
    * keep track of as you'd want it adjusted by suboffset(), etc.
    * Perhaps fix up when converting to align16?
    */
   reg.swizzle = swizzle;
   reg.writemask = writemask;
   reg.indirect_offset = 0;
   reg.vstride = vstride;
   reg.width = width;
   reg.hstride = hstride;
   reg.pad1 = 0;

   reg.offset = 0;
   reg.stride = 1;
   if (file == IMM &&
       type != BRW_TYPE_V &&
       type != BRW_TYPE_UV &&
       type != BRW_TYPE_VF) {
      reg.stride = 0;
   }

   return reg;
}

/** Construct float[16] register */
static inline struct brw_reg
brw_vec16_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return brw_make_reg(file,
                  nr,
                  subnr,
                  0,
                  0,
                  BRW_TYPE_F,
                  BRW_VERTICAL_STRIDE_16,
                  BRW_WIDTH_16,
                  BRW_HORIZONTAL_STRIDE_1,
                  BRW_SWIZZLE_XYZW,
                  WRITEMASK_XYZW);
}

/** Construct float[8] register */
static inline struct brw_reg
brw_vec8_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return brw_make_reg(file,
                  nr,
                  subnr,
                  0,
                  0,
                  BRW_TYPE_F,
                  BRW_VERTICAL_STRIDE_8,
                  BRW_WIDTH_8,
                  BRW_HORIZONTAL_STRIDE_1,
                  BRW_SWIZZLE_XYZW,
                  WRITEMASK_XYZW);
}

/** Construct float[4] register */
static inline struct brw_reg
brw_vec4_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return brw_make_reg(file,
                  nr,
                  subnr,
                  0,
                  0,
                  BRW_TYPE_F,
                  BRW_VERTICAL_STRIDE_4,
                  BRW_WIDTH_4,
                  BRW_HORIZONTAL_STRIDE_1,
                  BRW_SWIZZLE_XYZW,
                  WRITEMASK_XYZW);
}

/** Construct float[2] register */
static inline struct brw_reg
brw_vec2_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return brw_make_reg(file,
                  nr,
                  subnr,
                  0,
                  0,
                  BRW_TYPE_F,
                  BRW_VERTICAL_STRIDE_2,
                  BRW_WIDTH_2,
                  BRW_HORIZONTAL_STRIDE_1,
                  BRW_SWIZZLE_XYXY,
                  WRITEMASK_XY);
}

/** Construct float[1] register */
static inline struct brw_reg
brw_vec1_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return brw_make_reg(file,
                  nr,
                  subnr,
                  0,
                  0,
                  BRW_TYPE_F,
                  BRW_VERTICAL_STRIDE_0,
                  BRW_WIDTH_1,
                  BRW_HORIZONTAL_STRIDE_0,
                  BRW_SWIZZLE_XXXX,
                  WRITEMASK_X);
}

static inline struct brw_reg
brw_vecn_reg(unsigned width, enum brw_reg_file file,
             unsigned nr, unsigned subnr)
{
   switch (width) {
   case 1:
      return brw_vec1_reg(file, nr, subnr);
   case 2:
      return brw_vec2_reg(file, nr, subnr);
   case 4:
      return brw_vec4_reg(file, nr, subnr);
   case 8:
      return brw_vec8_reg(file, nr, subnr);
   case 16:
      return brw_vec16_reg(file, nr, subnr);
   default:
      unreachable("Invalid register width");
   }
}

static inline struct brw_reg
retype(struct brw_reg reg, enum brw_reg_type type)
{
   reg.type = type;
   return reg;
}

static inline struct brw_reg
firsthalf(struct brw_reg reg)
{
   return reg;
}

static inline struct brw_reg
sechalf(struct brw_reg reg)
{
   if (reg.vstride)
      reg.nr++;
   return reg;
}

static inline struct brw_reg
offset(struct brw_reg reg, unsigned delta)
{
   reg.nr += delta;
   return reg;
}


static inline struct brw_reg
byte_offset(struct brw_reg reg, unsigned bytes)
{
   switch (reg.file) {
   case BAD_FILE:
      break;
   case VGRF:
   case ATTR:
   case UNIFORM:
      reg.offset += bytes;
      break;
   case ARF:
   case FIXED_GRF: {
      const unsigned suboffset = reg.subnr + bytes;
      reg.nr += suboffset / REG_SIZE;
      reg.subnr = suboffset % REG_SIZE;
      break;
   }
   case IMM:
   default:
      assert(bytes == 0);
   }
   return reg;
}

static inline struct brw_reg
suboffset(struct brw_reg reg, unsigned delta)
{
   return byte_offset(reg, delta * brw_type_size_bytes(reg.type));
}

/** Construct unsigned word[16] register */
static inline struct brw_reg
brw_uw16_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return suboffset(retype(brw_vec16_reg(file, nr, 0), BRW_TYPE_UW), subnr);
}

/** Construct unsigned word[8] register */
static inline struct brw_reg
brw_uw8_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return suboffset(retype(brw_vec8_reg(file, nr, 0), BRW_TYPE_UW), subnr);
}

/** Construct unsigned word[1] register */
static inline struct brw_reg
brw_uw1_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return suboffset(retype(brw_vec1_reg(file, nr, 0), BRW_TYPE_UW), subnr);
}

static inline struct brw_reg
brw_ud8_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return retype(brw_vec8_reg(file, nr, subnr), BRW_TYPE_UD);
}

static inline struct brw_reg
brw_ud1_reg(enum brw_reg_file file, unsigned nr, unsigned subnr)
{
   return retype(brw_vec1_reg(file, nr, subnr), BRW_TYPE_UD);
}

static inline struct brw_reg
brw_imm_reg(enum brw_reg_type type)
{
   return brw_make_reg(BRW_IMMEDIATE_VALUE,
                  0,
                  0,
                  0,
                  0,
                  type,
                  BRW_VERTICAL_STRIDE_0,
                  BRW_WIDTH_1,
                  BRW_HORIZONTAL_STRIDE_0,
                  0,
                  0);
}

/** Construct float immediate register */
static inline struct brw_reg
brw_imm_df(double df)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_DF);
   imm.df = df;
   return imm;
}

static inline struct brw_reg
brw_imm_u64(uint64_t u64)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_UQ);
   imm.u64 = u64;
   return imm;
}

static inline struct brw_reg
brw_imm_f(float f)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_F);
   imm.f = f;
   return imm;
}

/** Construct int64_t immediate register */
static inline struct brw_reg
brw_imm_q(int64_t q)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_Q);
   imm.d64 = q;
   return imm;
}

/** Construct int64_t immediate register */
static inline struct brw_reg
brw_imm_uq(uint64_t uq)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_UQ);
   imm.u64 = uq;
   return imm;
}

/** Construct integer immediate register */
static inline struct brw_reg
brw_imm_d(int d)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_D);
   imm.d = d;
   return imm;
}

/** Construct uint immediate register */
static inline struct brw_reg
brw_imm_ud(unsigned ud)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_UD);
   imm.ud = ud;
   return imm;
}

/** Construct ushort immediate register */
static inline struct brw_reg
brw_imm_uw(uint16_t uw)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_UW);
   imm.ud = uw | (uw << 16);
   return imm;
}

/** Construct short immediate register */
static inline struct brw_reg
brw_imm_w(int16_t w)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_W);
   imm.ud = (uint16_t)w | (uint32_t)(uint16_t)w << 16;
   return imm;
}

/* brw_imm_b and brw_imm_ub aren't supported by hardware - the type
 * numbers alias with _V and _VF below:
 */

/** Construct vector of eight signed half-byte values */
static inline struct brw_reg
brw_imm_v(unsigned v)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_V);
   imm.ud = v;
   return imm;
}

/** Construct vector of eight unsigned half-byte values */
static inline struct brw_reg
brw_imm_uv(unsigned uv)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_UV);
   imm.ud = uv;
   return imm;
}

/** Construct vector of four 8-bit float values */
static inline struct brw_reg
brw_imm_vf(unsigned v)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_VF);
   imm.ud = v;
   return imm;
}

static inline struct brw_reg
brw_imm_vf4(unsigned v0, unsigned v1, unsigned v2, unsigned v3)
{
   struct brw_reg imm = brw_imm_reg(BRW_TYPE_VF);
   imm.vstride = BRW_VERTICAL_STRIDE_0;
   imm.width = BRW_WIDTH_4;
   imm.hstride = BRW_HORIZONTAL_STRIDE_1;
   imm.ud = ((v0 << 0) | (v1 << 8) | (v2 << 16) | (v3 << 24));
   return imm;
}


static inline struct brw_reg
brw_address(struct brw_reg reg)
{
   return brw_imm_uw(reg.nr * REG_SIZE + reg.subnr);
}

/** Construct float[1] general-purpose register */
static inline struct brw_reg
brw_vec1_grf(unsigned nr, unsigned subnr)
{
   return brw_vec1_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
xe2_vec1_grf(unsigned nr, unsigned subnr)
{
   return brw_vec1_reg(BRW_GENERAL_REGISTER_FILE, 2 * nr + subnr / 8, subnr % 8);
}

/** Construct float[2] general-purpose register */
static inline struct brw_reg
brw_vec2_grf(unsigned nr, unsigned subnr)
{
   return brw_vec2_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
xe2_vec2_grf(unsigned nr, unsigned subnr)
{
   return brw_vec2_reg(BRW_GENERAL_REGISTER_FILE, 2 * nr + subnr / 8, subnr % 8);
}

/** Construct float[4] general-purpose register */
static inline struct brw_reg
brw_vec4_grf(unsigned nr, unsigned subnr)
{
   return brw_vec4_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
xe2_vec4_grf(unsigned nr, unsigned subnr)
{
   return brw_vec4_reg(BRW_GENERAL_REGISTER_FILE, 2 * nr + subnr / 8, subnr % 8);
}

/** Construct float[8] general-purpose register */
static inline struct brw_reg
brw_vec8_grf(unsigned nr, unsigned subnr)
{
   return brw_vec8_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
xe2_vec8_grf(unsigned nr, unsigned subnr)
{
   return brw_vec8_reg(BRW_GENERAL_REGISTER_FILE, 2 * nr + subnr / 8, subnr % 8);
}

/** Construct float[16] general-purpose register */
static inline struct brw_reg
brw_vec16_grf(unsigned nr, unsigned subnr)
{
   return brw_vec16_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
xe2_vec16_grf(unsigned nr, unsigned subnr)
{
   return brw_vec16_reg(BRW_GENERAL_REGISTER_FILE, 2 * nr + subnr / 8, subnr % 8);
}

static inline struct brw_reg
brw_vecn_grf(unsigned width, unsigned nr, unsigned subnr)
{
   return brw_vecn_reg(width, BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
xe2_vecn_grf(unsigned width, unsigned nr, unsigned subnr)
{
   return brw_vecn_reg(width, BRW_GENERAL_REGISTER_FILE, nr + subnr / 8, subnr % 8);
}

static inline struct brw_reg
brw_uw1_grf(unsigned nr, unsigned subnr)
{
   return brw_uw1_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
brw_uw8_grf(unsigned nr, unsigned subnr)
{
   return brw_uw8_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
brw_uw16_grf(unsigned nr, unsigned subnr)
{
   return brw_uw16_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
brw_ud8_grf(unsigned nr, unsigned subnr)
{
   return brw_ud8_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}

static inline struct brw_reg
brw_ud1_grf(unsigned nr, unsigned subnr)
{
   return brw_ud1_reg(BRW_GENERAL_REGISTER_FILE, nr, subnr);
}


/** Construct null register (usually used for setting condition codes) */
static inline struct brw_reg
brw_null_reg(void)
{
   return brw_vec8_reg(BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_NULL, 0);
}

static inline struct brw_reg
brw_null_vec(unsigned width)
{
   return brw_vecn_reg(width, BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_NULL, 0);
}

static inline struct brw_reg
brw_address_reg(unsigned subnr)
{
   return brw_uw1_reg(BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_ADDRESS, subnr);
}

static inline struct brw_reg
brw_tdr_reg(void)
{
   return brw_uw1_reg(BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_TDR, 0);
}

/* If/else instructions break in align16 mode if writemask & swizzle
 * aren't xyzw.  This goes against the convention for other scalar
 * regs:
 */
static inline struct brw_reg
brw_ip_reg(void)
{
   return brw_make_reg(BRW_ARCHITECTURE_REGISTER_FILE,
                  BRW_ARF_IP,
                  0,
                  0,
                  0,
                  BRW_TYPE_UD,
                  BRW_VERTICAL_STRIDE_4, /* ? */
                  BRW_WIDTH_1,
                  BRW_HORIZONTAL_STRIDE_0,
                  BRW_SWIZZLE_XYZW, /* NOTE! */
                  WRITEMASK_XYZW); /* NOTE! */
}

static inline struct brw_reg
brw_notification_reg(void)
{
   return brw_make_reg(BRW_ARCHITECTURE_REGISTER_FILE,
                  BRW_ARF_NOTIFICATION_COUNT,
                  0,
                  0,
                  0,
                  BRW_TYPE_UD,
                  BRW_VERTICAL_STRIDE_0,
                  BRW_WIDTH_1,
                  BRW_HORIZONTAL_STRIDE_0,
                  BRW_SWIZZLE_XXXX,
                  WRITEMASK_X);
}

static inline struct brw_reg
brw_cr0_reg(unsigned subnr)
{
   return brw_ud1_reg(BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_CONTROL, subnr);
}

static inline struct brw_reg
brw_sr0_reg(unsigned subnr)
{
   return brw_ud1_reg(BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_STATE, subnr);
}

static inline struct brw_reg
brw_acc_reg(unsigned width)
{
   return brw_vecn_reg(width, BRW_ARCHITECTURE_REGISTER_FILE,
                       BRW_ARF_ACCUMULATOR, 0);
}

static inline struct brw_reg
brw_flag_reg(int reg, int subreg)
{
   return brw_uw1_reg(BRW_ARCHITECTURE_REGISTER_FILE,
                      BRW_ARF_FLAG + reg, subreg);
}

static inline struct brw_reg
brw_flag_subreg(unsigned subreg)
{
   return brw_uw1_reg(BRW_ARCHITECTURE_REGISTER_FILE,
                      BRW_ARF_FLAG + subreg / 2, subreg % 2);
}

/**
 * Return the mask register present in Gfx4-5, or the related register present
 * in Gfx7.5 and later hardware referred to as "channel enable" register in
 * the documentation.
 */
static inline struct brw_reg
brw_mask_reg(unsigned subnr)
{
   return brw_uw1_reg(BRW_ARCHITECTURE_REGISTER_FILE, BRW_ARF_MASK, subnr);
}

static inline struct brw_reg
brw_vmask_reg()
{
   return brw_sr0_reg(3);
}

static inline struct brw_reg
brw_dmask_reg()
{
   return brw_sr0_reg(2);
}

static inline struct brw_reg
brw_mask_stack_reg(unsigned subnr)
{
   return suboffset(retype(brw_vec16_reg(BRW_ARCHITECTURE_REGISTER_FILE,
                                         BRW_ARF_MASK_STACK, 0),
                           BRW_TYPE_UB), subnr);
}

static inline struct brw_reg
brw_mask_stack_depth_reg(unsigned subnr)
{
   return brw_uw1_reg(BRW_ARCHITECTURE_REGISTER_FILE,
                      BRW_ARF_MASK_STACK_DEPTH, subnr);
}

static inline struct brw_reg
brw_vgrf(unsigned nr, enum brw_reg_type type)
{
   struct brw_reg reg = {};
   reg.file = VGRF;
   reg.nr = nr;
   reg.type = type;
   reg.stride = 1;
   return reg;
}

static inline struct brw_reg
brw_attr_reg(unsigned nr, enum brw_reg_type type)
{
   struct brw_reg reg = {};
   reg.file = ATTR;
   reg.nr = nr;
   reg.type = type;
   reg.stride = 1;
   return reg;
}

static inline struct brw_reg
brw_uniform_reg(unsigned nr, enum brw_reg_type type)
{
   struct brw_reg reg = {};
   reg.file = UNIFORM;
   reg.nr = nr;
   reg.type = type;
   reg.stride = 0;
   return reg;
}

/* This is almost always called with a numeric constant argument, so
 * make things easy to evaluate at compile time:
 */
static inline unsigned cvt(unsigned val)
{
   switch (val) {
   case 0: return 0;
   case 1: return 1;
   case 2: return 2;
   case 4: return 3;
   case 8: return 4;
   case 16: return 5;
   case 32: return 6;
   }
   return 0;
}

static inline struct brw_reg
stride(struct brw_reg reg, unsigned vstride, unsigned width, unsigned hstride)
{
   reg.vstride = cvt(vstride);
   reg.width = cvt(width) - 1;
   reg.hstride = cvt(hstride);
   return reg;
}

/**
 * Multiply the vertical and horizontal stride of a register by the given
 * factor \a s.
 */
static inline struct brw_reg
spread(struct brw_reg reg, unsigned s)
{
   if (s) {
      assert(util_is_power_of_two_nonzero(s));

      if (reg.hstride)
         reg.hstride += cvt(s) - 1;

      if (reg.vstride)
         reg.vstride += cvt(s) - 1;

      return reg;
   } else {
      return stride(reg, 0, 1, 0);
   }
}

/**
 * Reinterpret each channel of register \p reg as a vector of values of the
 * given smaller type and take the i-th subcomponent from each.
 */
static inline struct brw_reg
subscript(struct brw_reg reg, enum brw_reg_type type, unsigned i)
{
   assert((i + 1) * brw_type_size_bytes(type) <= brw_type_size_bytes(reg.type));

   if (reg.file == ARF || reg.file == FIXED_GRF) {
      /* The stride is encoded inconsistently for fixed GRF and ARF registers
       * as the log2 of the actual vertical and horizontal strides.
       */
      const int delta = util_logbase2(brw_type_size_bytes(reg.type)) -
                        util_logbase2(brw_type_size_bytes(type));
      reg.hstride += (reg.hstride ? delta : 0);
      reg.vstride += (reg.vstride ? delta : 0);

   } else if (reg.file == IMM) {
      unsigned bit_size = brw_type_size_bits(type);
      reg.u64 >>= i * bit_size;
      reg.u64 &= BITFIELD64_MASK(bit_size);
      if (bit_size <= 16)
         reg.u64 |= reg.u64 << 16;
      return retype(reg, type);
   } else {
      reg.stride *= brw_type_size_bytes(reg.type) / brw_type_size_bytes(type);
   }

   return byte_offset(retype(reg, type), i * brw_type_size_bytes(type));
}

static inline struct brw_reg
vec16(struct brw_reg reg)
{
   return stride(reg, 16,16,1);
}

static inline struct brw_reg
vec8(struct brw_reg reg)
{
   return stride(reg, 8,8,1);
}

static inline struct brw_reg
vec4(struct brw_reg reg)
{
   return stride(reg, 4,4,1);
}

static inline struct brw_reg
vec2(struct brw_reg reg)
{
   return stride(reg, 2,2,1);
}

static inline struct brw_reg
vec1(struct brw_reg reg)
{
   return stride(reg, 0,1,0);
}


static inline struct brw_reg
get_element(struct brw_reg reg, unsigned elt)
{
   return vec1(suboffset(reg, elt));
}

static inline struct brw_reg
get_element_ud(struct brw_reg reg, unsigned elt)
{
   return vec1(suboffset(retype(reg, BRW_TYPE_UD), elt));
}

static inline struct brw_reg
get_element_d(struct brw_reg reg, unsigned elt)
{
   return vec1(suboffset(retype(reg, BRW_TYPE_D), elt));
}

static inline struct brw_reg
brw_swizzle(struct brw_reg reg, unsigned swz)
{
   if (reg.file == BRW_IMMEDIATE_VALUE)
      reg.ud = brw_swizzle_immediate(reg.type, reg.ud, swz);
   else
      reg.swizzle = brw_compose_swizzle(swz, reg.swizzle);

   return reg;
}

static inline struct brw_reg
brw_writemask(struct brw_reg reg, unsigned mask)
{
   assert(reg.file != BRW_IMMEDIATE_VALUE);
   reg.writemask &= mask;
   return reg;
}

static inline struct brw_reg
brw_set_writemask(struct brw_reg reg, unsigned mask)
{
   assert(reg.file != BRW_IMMEDIATE_VALUE);
   reg.writemask = mask;
   return reg;
}

static inline unsigned
brw_writemask_for_size(unsigned n)
{
   return (1 << n) - 1;
}

static inline unsigned
brw_writemask_for_component_packing(unsigned n, unsigned first_component)
{
   assert(first_component + n <= 4);
   return (((1 << n) - 1) << first_component);
}

static inline struct brw_reg
negate(struct brw_reg reg)
{
   reg.negate ^= 1;
   return reg;
}

static inline struct brw_reg
brw_abs(struct brw_reg reg)
{
   reg.abs = 1;
   reg.negate = 0;
   return reg;
}

/************************************************************************/

static inline struct brw_reg
brw_vec1_indirect(unsigned subnr, int offset)
{
   struct brw_reg reg =  brw_vec1_grf(0, 0);
   reg.subnr = subnr;
   reg.address_mode = BRW_ADDRESS_REGISTER_INDIRECT_REGISTER;
   reg.indirect_offset = offset;
   return reg;
}

static inline struct brw_reg
brw_VxH_indirect(unsigned subnr, int offset)
{
   struct brw_reg reg = brw_vec1_grf(0, 0);
   reg.vstride = BRW_VERTICAL_STRIDE_ONE_DIMENSIONAL;
   reg.subnr = subnr;
   reg.address_mode = BRW_ADDRESS_REGISTER_INDIRECT_REGISTER;
   reg.indirect_offset = offset;
   return reg;
}

static inline bool
region_matches(struct brw_reg reg, enum brw_vertical_stride v,
               enum brw_width w, enum brw_horizontal_stride h)
{
   return reg.vstride == v &&
          reg.width == w &&
          reg.hstride == h;
}

#define has_scalar_region(reg) \
   region_matches(reg, BRW_VERTICAL_STRIDE_0, BRW_WIDTH_1, \
                  BRW_HORIZONTAL_STRIDE_0)

/**
 * Return the size in bytes per data element of register \p reg on the
 * corresponding register file.
 */
static inline unsigned
element_sz(struct brw_reg reg)
{
   if (reg.file == BRW_IMMEDIATE_VALUE || has_scalar_region(reg)) {
      return brw_type_size_bytes(reg.type);

   } else if (reg.width == BRW_WIDTH_1 &&
              reg.hstride == BRW_HORIZONTAL_STRIDE_0) {
      assert(reg.vstride != BRW_VERTICAL_STRIDE_0);
      return brw_type_size_bytes(reg.type) << (reg.vstride - 1);

   } else {
      assert(reg.hstride != BRW_HORIZONTAL_STRIDE_0);
      assert(reg.vstride == reg.hstride + reg.width);
      return brw_type_size_bytes(reg.type) << (reg.hstride - 1);
   }
}

/* brw_packed_float.c */
int brw_float_to_vf(float f);
float brw_vf_to_float(unsigned char vf);

bool brw_reg_saturate_immediate(brw_reg *reg);
bool brw_reg_negate_immediate(brw_reg *reg);
bool brw_reg_abs_immediate(brw_reg *reg);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

static inline brw_reg
horiz_offset(const brw_reg &reg, unsigned delta)
{
   switch (reg.file) {
   case BAD_FILE:
   case UNIFORM:
   case IMM:
      /* These only have a single component that is implicitly splatted.  A
       * horizontal offset should be a harmless no-op.
       * XXX - Handle vector immediates correctly.
       */
      return reg;
   case VGRF:
   case ATTR:
      return byte_offset(reg, delta * reg.stride * brw_type_size_bytes(reg.type));
   case ARF:
   case FIXED_GRF:
      if (reg.is_null()) {
         return reg;
      } else {
         const unsigned hstride = reg.hstride ? 1 << (reg.hstride - 1) : 0;
         const unsigned vstride = reg.vstride ? 1 << (reg.vstride - 1) : 0;
         const unsigned width = 1 << reg.width;

         if (delta % width == 0) {
            return byte_offset(reg, delta / width * vstride * brw_type_size_bytes(reg.type));
         } else {
            assert(vstride == hstride * width);
            return byte_offset(reg, delta * hstride * brw_type_size_bytes(reg.type));
         }
      }
   }
   unreachable("Invalid register file");
}

static inline brw_reg
offset(brw_reg reg, unsigned width, unsigned delta)
{
   switch (reg.file) {
   case BAD_FILE:
      break;
   case ARF:
   case FIXED_GRF:
   case VGRF:
   case ATTR:
   case UNIFORM:
      return byte_offset(reg, delta * reg.component_size(width));
   case IMM:
      assert(delta == 0);
   }
   return reg;
}

/**
 * Get the scalar channel of \p reg given by \p idx and replicate it to all
 * channels of the result.
 */
static inline brw_reg
component(brw_reg reg, unsigned idx)
{
   reg = horiz_offset(reg, idx);
   reg.stride = 0;
   if (reg.file == ARF || reg.file == FIXED_GRF) {
      reg.vstride = BRW_VERTICAL_STRIDE_0;
      reg.width = BRW_WIDTH_1;
      reg.hstride = BRW_HORIZONTAL_STRIDE_0;
   }
   return reg;
}

/**
 * Return an integer identifying the discrete address space a register is
 * contained in.  A register is by definition fully contained in the single
 * reg_space it belongs to, so two registers with different reg_space ids are
 * guaranteed not to overlap.  Most register files are a single reg_space of
 * its own, only the VGRF and ATTR files are composed of multiple discrete
 * address spaces, one for each allocation and input attribute respectively.
 */
static inline uint32_t
reg_space(const brw_reg &r)
{
   return r.file << 16 | (r.file == VGRF || r.file == ATTR ? r.nr : 0);
}

/**
 * Return the base offset in bytes of a register relative to the start of its
 * reg_space().
 */
static inline unsigned
reg_offset(const brw_reg &r)
{
   return (r.file == VGRF || r.file == IMM || r.file == ATTR ? 0 : r.nr) *
          (r.file == UNIFORM ? 4 : REG_SIZE) + r.offset +
          (r.file == ARF || r.file == FIXED_GRF ? r.subnr : 0);
}

/**
 * Return the amount of padding in bytes left unused between individual
 * components of register \p r due to a (horizontal) stride value greater than
 * one, or zero if components are tightly packed in the register file.
 */
static inline unsigned
reg_padding(const brw_reg &r)
{
   const unsigned stride = ((r.file != ARF && r.file != FIXED_GRF) ? r.stride :
                            r.hstride == 0 ? 0 :
                            1 << (r.hstride - 1));
   return (MAX2(1, stride) - 1) * brw_type_size_bytes(r.type);
}

/**
 * Return whether the register region starting at \p r and spanning \p dr
 * bytes could potentially overlap the register region starting at \p s and
 * spanning \p ds bytes.
 */
static inline bool
regions_overlap(const brw_reg &r, unsigned dr, const brw_reg &s, unsigned ds)
{
   if (r.file != s.file)
      return false;

   if (r.file == VGRF) {
      return r.nr == s.nr &&
             !(r.offset + dr <= s.offset || s.offset + ds <= r.offset);
   } else {
      return !(reg_offset(r) + dr <= reg_offset(s) ||
               reg_offset(s) + ds <= reg_offset(r));
   }
}

/**
 * Check that the register region given by r [r.offset, r.offset + dr[
 * is fully contained inside the register region given by s
 * [s.offset, s.offset + ds[.
 */
static inline bool
region_contained_in(const brw_reg &r, unsigned dr, const brw_reg &s, unsigned ds)
{
   return reg_space(r) == reg_space(s) &&
          reg_offset(r) >= reg_offset(s) &&
          reg_offset(r) + dr <= reg_offset(s) + ds;
}

/**
 * Return whether the given register region is n-periodic, i.e. whether the
 * original region remains invariant after shifting it by \p n scalar
 * channels.
 */
static inline bool
is_periodic(const brw_reg &reg, unsigned n)
{
   if (reg.file == BAD_FILE || reg.is_null()) {
      return true;

   } else if (reg.file == IMM) {
      const unsigned period = (reg.type == BRW_TYPE_UV ||
                               reg.type == BRW_TYPE_V ? 8 :
                               reg.type == BRW_TYPE_VF ? 4 :
                               1);
      return n % period == 0;

   } else if (reg.file == ARF || reg.file == FIXED_GRF) {
      const unsigned period = (reg.hstride == 0 && reg.vstride == 0 ? 1 :
                               reg.vstride == 0 ? 1 << reg.width :
                               ~0);
      return n % period == 0;

   } else {
      return reg.stride == 0;
   }
}

static inline bool
is_uniform(const brw_reg &reg)
{
   return is_periodic(reg, 1);
}

/**
 * Get the specified 8-component quarter of a register.
 */
static inline brw_reg
quarter(const brw_reg &reg, unsigned idx)
{
   assert(idx < 4);
   return horiz_offset(reg, 8 * idx);
}

static inline brw_reg
horiz_stride(brw_reg reg, unsigned s)
{
   reg.stride *= s;
   return reg;
}

static const brw_reg reg_undef;

/*
 * Return the stride between channels of the specified register in
 * byte units, or ~0u if the region cannot be represented with a
 * single one-dimensional stride.
 */
static inline unsigned
byte_stride(const brw_reg &reg)
{
   switch (reg.file) {
   case BAD_FILE:
   case UNIFORM:
   case IMM:
   case VGRF:
   case ATTR:
      return reg.stride * brw_type_size_bytes(reg.type);
   case ARF:
   case FIXED_GRF:
      if (reg.is_null()) {
         return 0;
      } else {
         const unsigned hstride = reg.hstride ? 1 << (reg.hstride - 1) : 0;
         const unsigned vstride = reg.vstride ? 1 << (reg.vstride - 1) : 0;
         const unsigned width = 1 << reg.width;

         if (width == 1) {
            return vstride * brw_type_size_bytes(reg.type);
         } else if (hstride * width == vstride) {
            return hstride * brw_type_size_bytes(reg.type);
         } else {
            return ~0u;
         }
      }
   default:
      unreachable("Invalid register file");
   }
}

#endif /* __cplusplus */

#endif
