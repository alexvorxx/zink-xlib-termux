/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir_meta.h"
#include "ac_nir_helpers.h"
#include "nir_format_convert.h"
#include "compiler/aco_interface.h"

static nir_def *
deref_ssa(nir_builder *b, nir_variable *var)
{
   return &nir_build_deref_var(b, var)->def;
}

/* unpack_2x16_signed(src, x, y): x = (int32_t)((uint16_t)src); y = src >> 16; */
static void
unpack_2x16_signed(nir_builder *b, unsigned bit_size, nir_def *src, nir_def **x, nir_def **y)
{
   assert(bit_size == 32 || bit_size == 16);
   *x = nir_unpack_32_2x16_split_x(b, src);
   *y = nir_unpack_32_2x16_split_y(b, src);

   if (bit_size == 32) {
      *x = nir_i2i32(b, *x);
      *y = nir_i2i32(b, *y);
   }
}

static nir_def *
convert_linear_to_srgb(nir_builder *b, nir_def *input)
{
   /* There are small precision differences compared to CB, so the gfx blit will return slightly
    * different results.
    */
   for (unsigned i = 0; i < MIN2(3, input->num_components); i++) {
      input = nir_vector_insert_imm(b, input,
                                    nir_format_linear_to_srgb(b, nir_channel(b, input, i)), i);
   }

   return input;
}

static nir_def *
apply_blit_output_modifiers(nir_builder *b, nir_def *color,
                            const union ac_cs_blit_key *key)
{
   unsigned bit_size = color->bit_size;
   nir_def *zero = nir_imm_intN_t(b, 0, bit_size);

   if (key->sint_to_uint)
      color = nir_imax(b, color, zero);

   if (key->uint_to_sint) {
      color = nir_umin(b, color,
                       nir_imm_intN_t(b, bit_size == 16 ? INT16_MAX : INT32_MAX,
                                      bit_size));
   }

   if (key->dst_is_srgb)
      color = convert_linear_to_srgb(b, color);

   nir_def *one = key->use_integer_one ? nir_imm_intN_t(b, 1, bit_size) :
                                             nir_imm_floatN_t(b, 1, bit_size);

   if (key->is_clear) {
      if (key->last_dst_channel < 3)
         color = nir_trim_vector(b, color, key->last_dst_channel + 1);
   } else {
      assert(key->last_src_channel <= key->last_dst_channel);
      assert(color->num_components == key->last_src_channel + 1);

      /* Set channels not present in src to 0 or 1. */
      if (key->last_src_channel < key->last_dst_channel) {
         color = nir_pad_vector(b, color, key->last_dst_channel + 1);

         for (unsigned chan = key->last_src_channel + 1; chan <= key->last_dst_channel; chan++)
            color = nir_vector_insert_imm(b, color, chan == 3 ? one : zero, chan);
      }

      /* Discard channels not present in dst. The hardware fills unstored channels with 0. */
      if (key->last_dst_channel < key->last_src_channel)
         color = nir_trim_vector(b, color, key->last_dst_channel + 1);
   }

   /* Discard channels not present in dst. The hardware fills unstored channels with 0. */
   if (key->last_dst_channel < 3)
      color = nir_trim_vector(b, color, key->last_dst_channel + 1);

   return color;
}

/* The compute blit shader.
 *
 * Implementation details:
 * - Out-of-bounds dst coordinates are not clamped at all. The hw drops
 *   out-of-bounds stores for us.
 * - Out-of-bounds src coordinates are clamped by emulating CLAMP_TO_EDGE using
 *   the image_size NIR intrinsic.
 * - X/Y flipping just does this in the shader: -threadIDs - 1, assuming the starting coordinates
 *   are 1 pixel after the bottom-right corner, e.g. x + width, matching the gallium behavior.
 * - This list doesn't do it justice.
 */
nir_shader *
ac_create_blit_cs(const struct ac_cs_blit_options *options, const union ac_cs_blit_key *key)
{
   if (options->print_key) {
      fprintf(stderr, "Internal shader: compute_blit\n");
      fprintf(stderr, "   key.use_aco = %u\n", key->use_aco);
      fprintf(stderr, "   key.wg_dim = %u\n", key->wg_dim);
      fprintf(stderr, "   key.has_start_xyz = %u\n", key->has_start_xyz);
      fprintf(stderr, "   key.log_lane_width = %u\n", key->log_lane_width);
      fprintf(stderr, "   key.log_lane_height = %u\n", key->log_lane_height);
      fprintf(stderr, "   key.log_lane_depth = %u\n", key->log_lane_depth);
      fprintf(stderr, "   key.is_clear = %u\n", key->is_clear);
      fprintf(stderr, "   key.src_is_1d = %u\n", key->src_is_1d);
      fprintf(stderr, "   key.dst_is_1d = %u\n", key->dst_is_1d);
      fprintf(stderr, "   key.src_is_msaa = %u\n", key->src_is_msaa);
      fprintf(stderr, "   key.dst_is_msaa = %u\n", key->dst_is_msaa);
      fprintf(stderr, "   key.src_has_z = %u\n", key->src_has_z);
      fprintf(stderr, "   key.dst_has_z = %u\n", key->dst_has_z);
      fprintf(stderr, "   key.a16 = %u\n", key->a16);
      fprintf(stderr, "   key.d16 = %u\n", key->d16);
      fprintf(stderr, "   key.log_samples = %u\n", key->log_samples);
      fprintf(stderr, "   key.sample0_only = %u\n", key->sample0_only);
      fprintf(stderr, "   key.x_clamp_to_edge = %u\n", key->x_clamp_to_edge);
      fprintf(stderr, "   key.y_clamp_to_edge = %u\n", key->y_clamp_to_edge);
      fprintf(stderr, "   key.flip_x = %u\n", key->flip_x);
      fprintf(stderr, "   key.flip_y = %u\n", key->flip_y);
      fprintf(stderr, "   key.sint_to_uint = %u\n", key->sint_to_uint);
      fprintf(stderr, "   key.uint_to_sint = %u\n", key->uint_to_sint);
      fprintf(stderr, "   key.dst_is_srgb = %u\n", key->dst_is_srgb);
      fprintf(stderr, "   key.use_integer_one = %u\n", key->use_integer_one);
      fprintf(stderr, "   key.last_src_channel = %u\n", key->last_src_channel);
      fprintf(stderr, "   key.last_dst_channel = %u\n", key->last_dst_channel);
      fprintf(stderr, "\n");
   }

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options->nir_options,
                                                  "blit_non_scaled_cs");
   b.shader->info.use_aco_amd = options->use_aco ||
                                (key->use_aco && aco_is_gpu_supported(options->info));
   b.shader->info.num_images = key->is_clear ? 1 : 2;
   unsigned image_dst_index = b.shader->info.num_images - 1;
   if (!key->is_clear && key->src_is_msaa)
      BITSET_SET(b.shader->info.msaa_images, 0);
   if (key->dst_is_msaa)
      BITSET_SET(b.shader->info.msaa_images, image_dst_index);
   /* The workgroup size varies depending on the tiling layout and blit dimensions. */
   b.shader->info.workgroup_size_variable = true;
   b.shader->info.cs.user_data_components_amd =
      key->is_clear ? (key->d16 ? 6 : 8) : key->has_start_xyz ? 4 : 3;

   const struct glsl_type *img_type[2] = {
      glsl_image_type(key->src_is_1d ? GLSL_SAMPLER_DIM_1D :
                      key->src_is_msaa ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D,
                      key->src_has_z, GLSL_TYPE_FLOAT),
      glsl_image_type(key->dst_is_1d ? GLSL_SAMPLER_DIM_1D :
                      key->dst_is_msaa ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D,
                      key->dst_has_z, GLSL_TYPE_FLOAT),
   };

   nir_variable *img_src = NULL;
   if (!key->is_clear) {
      img_src = nir_variable_create(b.shader, nir_var_uniform, img_type[0], "img0");
      img_src->data.binding = 0;
   }

   nir_variable *img_dst = nir_variable_create(b.shader, nir_var_uniform, img_type[1], "img1");
   img_dst->data.binding = image_dst_index;

   unsigned lane_width = 1 << key->log_lane_width;
   unsigned lane_height = 1 << key->log_lane_height;
   unsigned lane_depth = 1 << key->log_lane_depth;
   unsigned lane_size = lane_width * lane_height * lane_depth;
   assert(lane_size <= SI_MAX_COMPUTE_BLIT_LANE_SIZE);

   nir_def *zero_lod = nir_imm_intN_t(&b, 0, key->a16 ? 16 : 32);

   /* Instructions. */
   /* Let's work with 0-based src and dst coordinates (thread IDs) first. */
   unsigned coord_bit_size = key->a16 ? 16 : 32;
   nir_def *dst_xyz = ac_get_global_ids(&b, key->wg_dim, coord_bit_size);
   dst_xyz = nir_pad_vector_imm_int(&b, dst_xyz, 0, 3);

   /* If the blit area is unaligned, we launched extra threads to make it aligned.
    * Skip those threads here.
    */
   nir_if *if_positive = NULL;
   if (key->has_start_xyz) {
      nir_def *start_xyz = nir_channel(&b, nir_load_user_data_amd(&b), 3);
      start_xyz = nir_u2uN(&b, nir_unpack_32_4x8(&b, start_xyz), coord_bit_size);
      start_xyz = nir_trim_vector(&b, start_xyz, 3);

      dst_xyz = nir_isub(&b, dst_xyz, start_xyz);
      nir_def *is_positive_xyz = nir_ige_imm(&b, dst_xyz, 0);
      nir_def *is_positive = nir_iand(&b, nir_channel(&b, is_positive_xyz, 0),
                                      nir_iand(&b, nir_channel(&b, is_positive_xyz, 1),
                                               nir_channel(&b, is_positive_xyz, 2)));
      if_positive = nir_push_if(&b, is_positive);
   }

   dst_xyz = nir_imul(&b, dst_xyz, nir_imm_ivec3_intN(&b, lane_width, lane_height, lane_depth,
                                                      coord_bit_size));
   nir_def *src_xyz = dst_xyz;

   /* Flip src coordinates. */
   for (unsigned i = 0; i < 2; i++) {
      if (i ? key->flip_y : key->flip_x) {
         /* A normal blit loads from (box.x + tid.x) where tid.x = 0..(width - 1).
          *
          * A flipped blit sets box.x = width, so we should make tid.x negative to load from
          * (width - 1)..0.
          *
          * Therefore do: x = -x - 1, which becomes (width - 1) to 0 after we add box.x = width.
          */
         nir_def *comp = nir_channel(&b, src_xyz, i);
         comp = nir_iadd_imm(&b, nir_ineg(&b, comp), -(int)(i ? lane_height : lane_width));
         src_xyz = nir_vector_insert_imm(&b, src_xyz, comp, i);
      }
   }

   /* Add box.xyz. */
   nir_def *base_coord_src = NULL, *base_coord_dst = NULL;
   unpack_2x16_signed(&b, coord_bit_size, nir_trim_vector(&b, nir_load_user_data_amd(&b), 3),
                      &base_coord_src, &base_coord_dst);
   base_coord_dst = nir_iadd(&b, base_coord_dst, dst_xyz);
   base_coord_src = nir_iadd(&b, base_coord_src, src_xyz);

   /* Coordinates must have 4 channels in NIR. */
   base_coord_src = nir_pad_vector(&b, base_coord_src, 4);
   base_coord_dst = nir_pad_vector(&b, base_coord_dst, 4);

/* Iterate over all pixels in the lane. num_samples is the only input.
 * (sample, x, y, z) are generated coordinates, while "i" is the coordinates converted to
 * an absolute index.
 */
#define foreach_pixel_in_lane(num_samples, sample, x, y, z, i) \
   for (unsigned z = 0; z < lane_depth; z++) \
      for (unsigned y = 0; y < lane_height; y++) \
         for (unsigned x = 0; x < lane_width; x++) \
            for (unsigned i = ((z * lane_height + y) * lane_width + x) * (num_samples), sample = 0; \
                 sample < (num_samples); sample++, i++) \

   /* Swizzle coordinates for 1D_ARRAY. */
   static const unsigned swizzle_xz[] = {0, 2, 0, 0};

   /* Execute image loads and stores. */
   unsigned num_src_coords = (key->src_is_1d ? 1 : 2) + key->src_has_z + key->src_is_msaa;
   unsigned num_dst_coords = (key->dst_is_1d ? 1 : 2) + key->dst_has_z + key->dst_is_msaa;
   unsigned bit_size = key->d16 ? 16 : 32;
   unsigned num_samples = 1 << key->log_samples;
   unsigned src_samples = key->src_is_msaa && !key->sample0_only &&
                          !key->is_clear ? num_samples : 1;
   unsigned dst_samples = key->dst_is_msaa ? num_samples : 1;
   nir_def *color[SI_MAX_COMPUTE_BLIT_LANE_SIZE * SI_MAX_COMPUTE_BLIT_SAMPLES] = {0};
   nir_def *coord_dst[SI_MAX_COMPUTE_BLIT_LANE_SIZE * SI_MAX_COMPUTE_BLIT_SAMPLES] = {0};
   nir_def *src_resinfo = NULL;

   if (key->is_clear) {
      /* The clear color starts at component 4 of user data. */
      color[0] = nir_channels(&b, nir_load_user_data_amd(&b),
                              BITFIELD_RANGE(4, key->d16 ? 2 : 4));
      if (key->d16)
         color[0] = nir_unpack_64_4x16(&b, nir_pack_64_2x32(&b, color[0]));

      foreach_pixel_in_lane(1, sample, x, y, z, i) {
         color[i] = color[0];
      }
   } else {
      nir_def *coord_src[SI_MAX_COMPUTE_BLIT_LANE_SIZE * SI_MAX_COMPUTE_BLIT_SAMPLES] = {0};

      /* Initialize src coordinates, one vector per pixel. */
      foreach_pixel_in_lane(src_samples, sample, x, y, z, i) {
         unsigned tmp_x = x;
         unsigned tmp_y = y;

         /* Change the order from 0..N to N..0 for flipped blits. */
         if (key->flip_x)
            tmp_x = lane_width - 1 - x;
         if (key->flip_y)
            tmp_y = lane_height - 1 - y;

         coord_src[i] = nir_iadd(&b, base_coord_src,
                                     nir_imm_ivec4_intN(&b, tmp_x, tmp_y, z, 0, coord_bit_size));
         if (key->src_is_1d)
            coord_src[i] = nir_swizzle(&b, coord_src[i], swizzle_xz, 4);
         if (key->src_is_msaa) {
            coord_src[i] = nir_vector_insert_imm(&b, coord_src[i],
                                                 nir_imm_intN_t(&b, sample, coord_bit_size),
                                                 num_src_coords - 1);
         }

         /* Clamp to edge for src, only X and Y because Z can't be out of bounds. */
         for (unsigned chan = 0; chan < 2; chan++) {
            if (chan ? key->y_clamp_to_edge : key->x_clamp_to_edge) {
               assert(!key->src_is_1d || chan == 0);

               if (!src_resinfo) {
                  /* Always use the 32-bit return type because the image dimensions can be
                   * > INT16_MAX even if the blit box fits within sint16.
                   */
                  src_resinfo = nir_image_deref_size(&b, 4, 32, deref_ssa(&b, img_src),
                                                     zero_lod);
                  if (coord_bit_size == 16) {
                     src_resinfo = nir_umin_imm(&b, src_resinfo, INT16_MAX);
                     src_resinfo = nir_i2i16(&b, src_resinfo);
                  }
               }

               nir_def *tmp = nir_channel(&b, coord_src[i], chan);
               tmp = nir_imax_imm(&b, tmp, 0);
               tmp = nir_imin(&b, tmp, nir_iadd_imm(&b, nir_channel(&b, src_resinfo, chan), -1));
               coord_src[i] = nir_vector_insert_imm(&b, coord_src[i], tmp, chan);
            }
         }
      }

      /* We don't want the computation of src coordinates to be interleaved with loads. */
      if (lane_size > 1 || src_samples > 1) {
         ac_optimization_barrier_vgpr_array(options->info, &b, coord_src,
                                            lane_size * src_samples, num_src_coords);
      }

      /* Use "samples_identical" for MSAA resolving if it's supported. */
      bool is_resolve = src_samples > 1 && dst_samples == 1;
      bool uses_samples_identical = options->info->gfx_level < GFX11 && !options->no_fmask && is_resolve;
      nir_def *samples_identical = NULL, *sample0[SI_MAX_COMPUTE_BLIT_LANE_SIZE] = {0};
      nir_if *if_identical = NULL;

      if (uses_samples_identical) {
         samples_identical = nir_imm_true(&b);

         /* If we are resolving multiple pixels per lane, AND all results of "samples_identical". */
         foreach_pixel_in_lane(1, sample, x, y, z, i) {
            nir_def *iden = nir_image_deref_samples_identical(&b, 1, deref_ssa(&b, img_src),
                                                              coord_src[i * src_samples],
                                                              .image_dim = GLSL_SAMPLER_DIM_MS);
            samples_identical = nir_iand(&b, samples_identical, iden);
         }

         /* If all samples are identical, load only sample 0. */
         if_identical = nir_push_if(&b, samples_identical);
         foreach_pixel_in_lane(1, sample, x, y, z, i) {
            sample0[i] = nir_image_deref_load(&b, key->last_src_channel + 1, bit_size,
                                              deref_ssa(&b, img_src), coord_src[i * src_samples],
                                              nir_channel(&b, coord_src[i * src_samples],
                                                          num_src_coords - 1), zero_lod,
                                              .image_dim = img_src->type->sampler_dimensionality,
                                              .image_array = img_src->type->sampler_array);
         }
         nir_push_else(&b, if_identical);
      }

      /* Load src pixels, one per sample. */
      foreach_pixel_in_lane(src_samples, sample, x, y, z, i) {
         color[i] = nir_image_deref_load(&b, key->last_src_channel + 1, bit_size,
                                         deref_ssa(&b, img_src), coord_src[i],
                                         nir_channel(&b, coord_src[i], num_src_coords - 1), zero_lod,
                                         .image_dim = img_src->type->sampler_dimensionality,
                                         .image_array = img_src->type->sampler_array);
      }

      /* Resolve MSAA if necessary. */
      if (is_resolve) {
         /* We don't want the averaging of samples to be interleaved with image loads. */
         ac_optimization_barrier_vgpr_array(options->info, &b, color, lane_size * src_samples,
                                            key->last_src_channel + 1);

         /* This reduces the "color" array from "src_samples * lane_size" elements to only
          * "lane_size" elements.
          */
         foreach_pixel_in_lane(1, sample, x, y, z, i) {
            color[i] = ac_average_samples(&b, &color[i * src_samples], src_samples);
         }
         src_samples = 1;
      }

      if (uses_samples_identical) {
         nir_pop_if(&b, if_identical);
         foreach_pixel_in_lane(1, sample, x, y, z, i) {
            color[i] = nir_if_phi(&b, sample0[i], color[i]);
         }
      }
   }

   /* We need to load the descriptor here, otherwise the load would be after optimization
    * barriers waiting for image loads, i.e. after s_waitcnt vmcnt(0).
    */
   nir_def *img_dst_desc = nir_image_deref_descriptor_amd(&b, 8, 32, deref_ssa(&b, img_dst));
   if (lane_size > 1 && !b.shader->info.use_aco_amd)
      img_dst_desc = nir_optimization_barrier_sgpr_amd(&b, 32, img_dst_desc);

   /* Apply the blit output modifiers, once per sample.  */
   foreach_pixel_in_lane(src_samples, sample, x, y, z, i) {
      color[i] = apply_blit_output_modifiers(&b, color[i], key);
   }

   /* Initialize dst coordinates, one vector per pixel. */
   foreach_pixel_in_lane(dst_samples, sample, x, y, z, i) {
      coord_dst[i] = nir_iadd(&b, base_coord_dst,
                              nir_imm_ivec4_intN(&b, x, y, z, 0, coord_bit_size));
      if (key->dst_is_1d)
         coord_dst[i] = nir_swizzle(&b, coord_dst[i], swizzle_xz, 4);
      if (key->dst_is_msaa) {
         coord_dst[i] = nir_vector_insert_imm(&b, coord_dst[i],
                                              nir_imm_intN_t(&b, sample, coord_bit_size),
                                              num_dst_coords - 1);
      }
   }

   /* We don't want the computation of dst coordinates to be interleaved with stores. */
   if (lane_size > 1 || dst_samples > 1) {
      ac_optimization_barrier_vgpr_array(options->info, &b, coord_dst, lane_size * dst_samples,
                                         num_dst_coords);
   }

   /* We don't want the application of blit output modifiers to be interleaved with stores. */
   if (!key->is_clear && (lane_size > 1 || MIN2(src_samples, dst_samples) > 1)) {
      ac_optimization_barrier_vgpr_array(options->info, &b, color, lane_size * src_samples,
                                         key->last_dst_channel + 1);
   }

   /* Store the pixels, one per sample. */
   foreach_pixel_in_lane(dst_samples, sample, x, y, z, i) {
      nir_bindless_image_store(&b, img_dst_desc, coord_dst[i],
                               nir_channel(&b, coord_dst[i], num_dst_coords - 1),
                               src_samples > 1 ? color[i] : color[i / dst_samples], zero_lod,
                               .image_dim = glsl_get_sampler_dim(img_type[1]),
                               .image_array = glsl_sampler_type_is_array(img_type[1]));
   }

   if (key->has_start_xyz)
      nir_pop_if(&b, if_positive);

   return b.shader;
}
