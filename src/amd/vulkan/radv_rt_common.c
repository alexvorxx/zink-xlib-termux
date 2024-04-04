/*
 * Copyright © 2021 Google
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

#include "radv_debug.h"
#include "radv_rt_common.h"
#include "radv_acceleration_structure.h"

static nir_ssa_def *build_node_to_addr(struct radv_device *device, nir_builder *b,
                                       nir_ssa_def *node, bool skip_type_and);

bool
radv_enable_rt(const struct radv_physical_device *pdevice, bool rt_pipelines)
{
   if ((pdevice->rad_info.gfx_level < GFX10_3 && !radv_emulate_rt(pdevice)) || pdevice->use_llvm)
      return false;

   if (rt_pipelines)
      return pdevice->instance->perftest_flags & RADV_PERFTEST_RT;

   return true;
}

bool
radv_emulate_rt(const struct radv_physical_device *pdevice)
{
   return pdevice->instance->perftest_flags & RADV_PERFTEST_EMULATE_RT;
}

void
nir_sort_hit_pair(nir_builder *b, nir_variable *var_distances, nir_variable *var_indices,
                  uint32_t chan_1, uint32_t chan_2)
{
   nir_ssa_def *ssa_distances = nir_load_var(b, var_distances);
   nir_ssa_def *ssa_indices = nir_load_var(b, var_indices);
   /* if (distances[chan_2] < distances[chan_1]) { */
   nir_push_if(
      b, nir_flt(b, nir_channel(b, ssa_distances, chan_2), nir_channel(b, ssa_distances, chan_1)));
   {
      /* swap(distances[chan_2], distances[chan_1]); */
      nir_ssa_def *new_distances[4] = {nir_ssa_undef(b, 1, 32), nir_ssa_undef(b, 1, 32),
                                       nir_ssa_undef(b, 1, 32), nir_ssa_undef(b, 1, 32)};
      nir_ssa_def *new_indices[4] = {nir_ssa_undef(b, 1, 32), nir_ssa_undef(b, 1, 32),
                                     nir_ssa_undef(b, 1, 32), nir_ssa_undef(b, 1, 32)};
      new_distances[chan_2] = nir_channel(b, ssa_distances, chan_1);
      new_distances[chan_1] = nir_channel(b, ssa_distances, chan_2);
      new_indices[chan_2] = nir_channel(b, ssa_indices, chan_1);
      new_indices[chan_1] = nir_channel(b, ssa_indices, chan_2);
      nir_store_var(b, var_distances, nir_vec(b, new_distances, 4),
                    (1u << chan_1) | (1u << chan_2));
      nir_store_var(b, var_indices, nir_vec(b, new_indices, 4), (1u << chan_1) | (1u << chan_2));
   }
   /* } */
   nir_pop_if(b, NULL);
}

nir_ssa_def *
intersect_ray_amd_software_box(struct radv_device *device, nir_builder *b, nir_ssa_def *bvh_node,
                               nir_ssa_def *ray_tmax, nir_ssa_def *origin, nir_ssa_def *dir,
                               nir_ssa_def *inv_dir)
{
   const struct glsl_type *vec4_type = glsl_vector_type(GLSL_TYPE_FLOAT, 4);
   const struct glsl_type *uvec4_type = glsl_vector_type(GLSL_TYPE_UINT, 4);

   nir_ssa_def *node_addr = build_node_to_addr(device, b, bvh_node, false);

   /* vec4 distances = vec4(INF, INF, INF, INF); */
   nir_variable *distances =
      nir_variable_create(b->shader, nir_var_shader_temp, vec4_type, "distances");
   nir_store_var(b, distances, nir_imm_vec4(b, INFINITY, INFINITY, INFINITY, INFINITY), 0xf);

   /* uvec4 child_indices = uvec4(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff); */
   nir_variable *child_indices =
      nir_variable_create(b->shader, nir_var_shader_temp, uvec4_type, "child_indices");
   nir_store_var(b, child_indices,
                 nir_imm_ivec4(b, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu), 0xf);

   /* Need to remove infinities here because otherwise we get nasty NaN propogation
    * if the direction has 0s in it. */
   /* inv_dir = clamp(inv_dir, -FLT_MAX, FLT_MAX); */
   inv_dir = nir_fclamp(b, inv_dir, nir_imm_float(b, -FLT_MAX), nir_imm_float(b, FLT_MAX));

   for (int i = 0; i < 4; i++) {
      const uint32_t child_offset = offsetof(struct radv_bvh_box32_node, children[i]);
      const uint32_t coord_offsets[2] = {
         offsetof(struct radv_bvh_box32_node, coords[i].min.x),
         offsetof(struct radv_bvh_box32_node, coords[i].max.x),
      };

      /* node->children[i] -> uint */
      nir_ssa_def *child_index =
         nir_build_load_global(b, 1, 32, nir_iadd_imm(b, node_addr, child_offset), .align_mul = 64,
                               .align_offset = child_offset % 64);
      /* node->coords[i][0], node->coords[i][1] -> vec3 */
      nir_ssa_def *node_coords[2] = {
         nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, coord_offsets[0]),
                               .align_mul = 64, .align_offset = coord_offsets[0] % 64),
         nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, coord_offsets[1]),
                               .align_mul = 64, .align_offset = coord_offsets[1] % 64),
      };

      /* If x of the aabb min is NaN, then this is an inactive aabb.
       * We don't need to care about any other components being NaN as that is UB.
       * https://www.khronos.org/registry/vulkan/specs/1.2-extensions/html/chap36.html#VkAabbPositionsKHR
       */
      nir_ssa_def *min_x = nir_channel(b, node_coords[0], 0);
      nir_ssa_def *min_x_is_not_nan =
         nir_inot(b, nir_fneu(b, min_x, min_x)); /* NaN != NaN -> true */

      /* vec3 bound0 = (node->coords[i][0] - origin) * inv_dir; */
      nir_ssa_def *bound0 = nir_fmul(b, nir_fsub(b, node_coords[0], origin), inv_dir);
      /* vec3 bound1 = (node->coords[i][1] - origin) * inv_dir; */
      nir_ssa_def *bound1 = nir_fmul(b, nir_fsub(b, node_coords[1], origin), inv_dir);

      /* float tmin = max(max(min(bound0.x, bound1.x), min(bound0.y, bound1.y)), min(bound0.z,
       * bound1.z)); */
      nir_ssa_def *tmin =
         nir_fmax(b,
                  nir_fmax(b, nir_fmin(b, nir_channel(b, bound0, 0), nir_channel(b, bound1, 0)),
                           nir_fmin(b, nir_channel(b, bound0, 1), nir_channel(b, bound1, 1))),
                  nir_fmin(b, nir_channel(b, bound0, 2), nir_channel(b, bound1, 2)));

      /* float tmax = min(min(max(bound0.x, bound1.x), max(bound0.y, bound1.y)), max(bound0.z,
       * bound1.z)); */
      nir_ssa_def *tmax =
         nir_fmin(b,
                  nir_fmin(b, nir_fmax(b, nir_channel(b, bound0, 0), nir_channel(b, bound1, 0)),
                           nir_fmax(b, nir_channel(b, bound0, 1), nir_channel(b, bound1, 1))),
                  nir_fmax(b, nir_channel(b, bound0, 2), nir_channel(b, bound1, 2)));

      /* if (!isnan(node->coords[i][0].x) && tmax >= max(0.0f, tmin) && tmin < ray_tmax) { */
      nir_push_if(b,
                  nir_iand(b, min_x_is_not_nan,
                           nir_iand(b, nir_fge(b, tmax, nir_fmax(b, nir_imm_float(b, 0.0f), tmin)),
                                    nir_flt(b, tmin, ray_tmax))));
      {
         /* child_indices[i] = node->children[i]; */
         nir_ssa_def *new_child_indices[4] = {child_index, child_index, child_index, child_index};
         nir_store_var(b, child_indices, nir_vec(b, new_child_indices, 4), 1u << i);

         /* distances[i] = tmin; */
         nir_ssa_def *new_distances[4] = {tmin, tmin, tmin, tmin};
         nir_store_var(b, distances, nir_vec(b, new_distances, 4), 1u << i);
      }
      /* } */
      nir_pop_if(b, NULL);
   }

   /* Sort our distances with a sorting network. */
   nir_sort_hit_pair(b, distances, child_indices, 0, 1);
   nir_sort_hit_pair(b, distances, child_indices, 2, 3);
   nir_sort_hit_pair(b, distances, child_indices, 0, 2);
   nir_sort_hit_pair(b, distances, child_indices, 1, 3);
   nir_sort_hit_pair(b, distances, child_indices, 1, 2);

   return nir_load_var(b, child_indices);
}

nir_ssa_def *
intersect_ray_amd_software_tri(struct radv_device *device, nir_builder *b, nir_ssa_def *bvh_node,
                               nir_ssa_def *ray_tmax, nir_ssa_def *origin, nir_ssa_def *dir,
                               nir_ssa_def *inv_dir)
{
   const struct glsl_type *vec4_type = glsl_vector_type(GLSL_TYPE_FLOAT, 4);

   nir_ssa_def *node_addr = build_node_to_addr(device, b, bvh_node, false);

   const uint32_t coord_offsets[3] = {
      offsetof(struct radv_bvh_triangle_node, coords[0]),
      offsetof(struct radv_bvh_triangle_node, coords[1]),
      offsetof(struct radv_bvh_triangle_node, coords[2]),
   };

   /* node->coords[0], node->coords[1], node->coords[2] -> vec3 */
   nir_ssa_def *node_coords[3] = {
      nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, coord_offsets[0]), .align_mul = 64,
                            .align_offset = coord_offsets[0] % 64),
      nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, coord_offsets[1]), .align_mul = 64,
                            .align_offset = coord_offsets[1] % 64),
      nir_build_load_global(b, 3, 32, nir_iadd_imm(b, node_addr, coord_offsets[2]), .align_mul = 64,
                            .align_offset = coord_offsets[2] % 64),
   };

   nir_variable *result = nir_variable_create(b->shader, nir_var_shader_temp, vec4_type, "result");
   nir_store_var(b, result, nir_imm_vec4(b, INFINITY, 1.0f, 0.0f, 0.0f), 0xf);

   /* Based on watertight Ray/Triangle intersection from
    * http://jcgt.org/published/0002/01/05/paper.pdf */

   /* Calculate the dimension where the ray direction is largest */
   nir_ssa_def *abs_dir = nir_fabs(b, dir);

   nir_ssa_def *abs_dirs[3] = {
      nir_channel(b, abs_dir, 0),
      nir_channel(b, abs_dir, 1),
      nir_channel(b, abs_dir, 2),
   };
   /* Find index of greatest value of abs_dir and put that as kz. */
   nir_ssa_def *kz = nir_bcsel(
      b, nir_fge(b, abs_dirs[0], abs_dirs[1]),
      nir_bcsel(b, nir_fge(b, abs_dirs[0], abs_dirs[2]), nir_imm_int(b, 0), nir_imm_int(b, 2)),
      nir_bcsel(b, nir_fge(b, abs_dirs[1], abs_dirs[2]), nir_imm_int(b, 1), nir_imm_int(b, 2)));
   nir_ssa_def *kx = nir_imod(b, nir_iadd_imm(b, kz, 1), nir_imm_int(b, 3));
   nir_ssa_def *ky = nir_imod(b, nir_iadd_imm(b, kx, 1), nir_imm_int(b, 3));
   nir_ssa_def *k_indices[3] = {kx, ky, kz};
   nir_ssa_def *k = nir_vec(b, k_indices, 3);

   /* Swap kx and ky dimensions to preseve winding order */
   unsigned swap_xy_swizzle[4] = {1, 0, 2, 3};
   k = nir_bcsel(b, nir_flt(b, nir_vector_extract(b, dir, kz), nir_imm_float(b, 0.0f)),
                 nir_swizzle(b, k, swap_xy_swizzle, 3), k);

   kx = nir_channel(b, k, 0);
   ky = nir_channel(b, k, 1);
   kz = nir_channel(b, k, 2);

   /* Calculate shear constants */
   nir_ssa_def *sz = nir_frcp(b, nir_vector_extract(b, dir, kz));
   nir_ssa_def *sx = nir_fmul(b, nir_vector_extract(b, dir, kx), sz);
   nir_ssa_def *sy = nir_fmul(b, nir_vector_extract(b, dir, ky), sz);

   /* Calculate vertices relative to ray origin */
   nir_ssa_def *v_a = nir_fsub(b, node_coords[0], origin);
   nir_ssa_def *v_b = nir_fsub(b, node_coords[1], origin);
   nir_ssa_def *v_c = nir_fsub(b, node_coords[2], origin);

   /* Perform shear and scale */
   nir_ssa_def *ax =
      nir_fsub(b, nir_vector_extract(b, v_a, kx), nir_fmul(b, sx, nir_vector_extract(b, v_a, kz)));
   nir_ssa_def *ay =
      nir_fsub(b, nir_vector_extract(b, v_a, ky), nir_fmul(b, sy, nir_vector_extract(b, v_a, kz)));
   nir_ssa_def *bx =
      nir_fsub(b, nir_vector_extract(b, v_b, kx), nir_fmul(b, sx, nir_vector_extract(b, v_b, kz)));
   nir_ssa_def *by =
      nir_fsub(b, nir_vector_extract(b, v_b, ky), nir_fmul(b, sy, nir_vector_extract(b, v_b, kz)));
   nir_ssa_def *cx =
      nir_fsub(b, nir_vector_extract(b, v_c, kx), nir_fmul(b, sx, nir_vector_extract(b, v_c, kz)));
   nir_ssa_def *cy =
      nir_fsub(b, nir_vector_extract(b, v_c, ky), nir_fmul(b, sy, nir_vector_extract(b, v_c, kz)));

   nir_ssa_def *u = nir_fsub(b, nir_fmul(b, cx, by), nir_fmul(b, cy, bx));
   nir_ssa_def *v = nir_fsub(b, nir_fmul(b, ax, cy), nir_fmul(b, ay, cx));
   nir_ssa_def *w = nir_fsub(b, nir_fmul(b, bx, ay), nir_fmul(b, by, ax));

   nir_variable *u_var =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_float_type(), "u");
   nir_variable *v_var =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_float_type(), "v");
   nir_variable *w_var =
      nir_variable_create(b->shader, nir_var_shader_temp, glsl_float_type(), "w");
   nir_store_var(b, u_var, u, 0x1);
   nir_store_var(b, v_var, v, 0x1);
   nir_store_var(b, w_var, w, 0x1);

   /* Fallback to testing edges with double precision...
    *
    * The Vulkan spec states it only needs single precision watertightness
    * but we fail dEQP-VK.ray_tracing_pipeline.watertightness.closedFan2.1024 with
    * failures = 1 without doing this. :( */
   nir_ssa_def *cond_retest = nir_ior(
      b, nir_ior(b, nir_feq(b, u, nir_imm_float(b, 0.0f)), nir_feq(b, v, nir_imm_float(b, 0.0f))),
      nir_feq(b, w, nir_imm_float(b, 0.0f)));

   nir_push_if(b, cond_retest);
   {
      ax = nir_f2f64(b, ax);
      ay = nir_f2f64(b, ay);
      bx = nir_f2f64(b, bx);
      by = nir_f2f64(b, by);
      cx = nir_f2f64(b, cx);
      cy = nir_f2f64(b, cy);

      nir_store_var(b, u_var, nir_f2f32(b, nir_fsub(b, nir_fmul(b, cx, by), nir_fmul(b, cy, bx))),
                    0x1);
      nir_store_var(b, v_var, nir_f2f32(b, nir_fsub(b, nir_fmul(b, ax, cy), nir_fmul(b, ay, cx))),
                    0x1);
      nir_store_var(b, w_var, nir_f2f32(b, nir_fsub(b, nir_fmul(b, bx, ay), nir_fmul(b, by, ax))),
                    0x1);
   }
   nir_pop_if(b, NULL);

   u = nir_load_var(b, u_var);
   v = nir_load_var(b, v_var);
   w = nir_load_var(b, w_var);

   /* Perform edge tests. */
   nir_ssa_def *cond_back = nir_ior(
      b, nir_ior(b, nir_flt(b, u, nir_imm_float(b, 0.0f)), nir_flt(b, v, nir_imm_float(b, 0.0f))),
      nir_flt(b, w, nir_imm_float(b, 0.0f)));

   nir_ssa_def *cond_front = nir_ior(
      b, nir_ior(b, nir_flt(b, nir_imm_float(b, 0.0f), u), nir_flt(b, nir_imm_float(b, 0.0f), v)),
      nir_flt(b, nir_imm_float(b, 0.0f), w));

   nir_ssa_def *cond = nir_inot(b, nir_iand(b, cond_back, cond_front));

   nir_push_if(b, cond);
   {
      nir_ssa_def *det = nir_fadd(b, u, nir_fadd(b, v, w));

      nir_ssa_def *az = nir_fmul(b, sz, nir_vector_extract(b, v_a, kz));
      nir_ssa_def *bz = nir_fmul(b, sz, nir_vector_extract(b, v_b, kz));
      nir_ssa_def *cz = nir_fmul(b, sz, nir_vector_extract(b, v_c, kz));

      nir_ssa_def *t =
         nir_fadd(b, nir_fadd(b, nir_fmul(b, u, az), nir_fmul(b, v, bz)), nir_fmul(b, w, cz));

      nir_ssa_def *t_signed = nir_fmul(b, nir_fsign(b, det), t);

      nir_ssa_def *det_cond_front = nir_inot(b, nir_flt(b, t_signed, nir_imm_float(b, 0.0f)));

      nir_push_if(b, det_cond_front);
      {
         nir_ssa_def *indices[4] = {t, det, v, w};
         nir_store_var(b, result, nir_vec(b, indices, 4), 0xf);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);

   return nir_load_var(b, result);
}

nir_ssa_def *
build_addr_to_node(nir_builder *b, nir_ssa_def *addr)
{
   const uint64_t bvh_size = 1ull << 42;
   nir_ssa_def *node = nir_ushr_imm(b, addr, 3);
   return nir_iand_imm(b, node, (bvh_size - 1) << 3);
}

static nir_ssa_def *
build_node_to_addr(struct radv_device *device, nir_builder *b, nir_ssa_def *node,
                   bool skip_type_and)
{
   nir_ssa_def *addr = skip_type_and ? node : nir_iand_imm(b, node, ~7ull);
   addr = nir_ishl_imm(b, addr, 3);
   /* Assumes everything is in the top half of address space, which is true in
    * GFX9+ for now. */
   return device->physical_device->rad_info.gfx_level >= GFX9
             ? nir_ior_imm(b, addr, 0xffffull << 48)
             : addr;
}

nir_ssa_def *
nir_build_vec3_mat_mult(nir_builder *b, nir_ssa_def *vec, nir_ssa_def *matrix[], bool translation)
{
   nir_ssa_def *result_components[3] = {
      nir_channel(b, matrix[0], 3),
      nir_channel(b, matrix[1], 3),
      nir_channel(b, matrix[2], 3),
   };
   for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         nir_ssa_def *v =
            nir_fmul(b, nir_channels(b, vec, 1 << j), nir_channels(b, matrix[i], 1 << j));
         result_components[i] = (translation || j) ? nir_fadd(b, result_components[i], v) : v;
      }
   }
   return nir_vec(b, result_components, 3);
}

void
nir_build_wto_matrix_load(nir_builder *b, nir_ssa_def *instance_addr, nir_ssa_def **out)
{
   unsigned offset = offsetof(struct radv_bvh_instance_node, wto_matrix);
   for (unsigned i = 0; i < 3; ++i) {
      out[i] = nir_build_load_global(b, 4, 32, nir_iadd_imm(b, instance_addr, offset + i * 16),
                                     .align_mul = 64, .align_offset = offset + i * 16);
   }
}

/* When a hit is opaque the any_hit shader is skipped for this hit and the hit
 * is assumed to be an actual hit. */
static nir_ssa_def *
hit_is_opaque(nir_builder *b, nir_ssa_def *sbt_offset_and_flags,
              const struct radv_ray_flags *ray_flags, nir_ssa_def *geometry_id_and_flags)
{
   nir_ssa_def *opaque =
      nir_uge(b, nir_ior(b, geometry_id_and_flags, sbt_offset_and_flags),
              nir_imm_int(b, RADV_INSTANCE_FORCE_OPAQUE | RADV_INSTANCE_NO_FORCE_NOT_OPAQUE));
   opaque = nir_bcsel(b, ray_flags->force_opaque, nir_imm_bool(b, true), opaque);
   opaque = nir_bcsel(b, ray_flags->force_not_opaque, nir_imm_bool(b, false), opaque);
   return opaque;
}

nir_ssa_def *
create_bvh_descriptor(nir_builder *b)
{
   /* We create a BVH descriptor that covers the entire memory range. That way we can always
    * use the same descriptor, which avoids divergence when different rays hit different
    * instances at the cost of having to use 64-bit node ids. */
   const uint64_t bvh_size = 1ull << 42;
   return nir_imm_ivec4(
      b, 0, 1u << 31 /* Enable box sorting */, (bvh_size - 1) & 0xFFFFFFFFu,
      ((bvh_size - 1) >> 32) | (1u << 24 /* Return IJ for triangles */) | (1u << 31));
}

static void
insert_traversal_triangle_case(struct radv_device *device, nir_builder *b,
                               const struct radv_ray_traversal_args *args,
                               const struct radv_ray_flags *ray_flags, nir_ssa_def *result,
                               nir_ssa_def *bvh_node)
{
   if (!args->triangle_cb)
      return;

   struct radv_triangle_intersection intersection;
   intersection.t = nir_channel(b, result, 0);
   nir_ssa_def *div = nir_channel(b, result, 1);
   intersection.t = nir_fdiv(b, intersection.t, div);

   nir_push_if(b, nir_flt(b, intersection.t, nir_load_deref(b, args->vars.tmax)));
   {
      intersection.frontface = nir_flt(b, nir_imm_float(b, 0), div);
      nir_ssa_def *switch_ccw = nir_test_mask(b, nir_load_deref(b, args->vars.sbt_offset_and_flags),
                                              RADV_INSTANCE_TRIANGLE_FLIP_FACING);
      intersection.frontface = nir_ixor(b, intersection.frontface, switch_ccw);

      nir_ssa_def *not_cull = ray_flags->no_skip_triangles;
      nir_ssa_def *not_facing_cull =
         nir_bcsel(b, intersection.frontface, ray_flags->no_cull_front, ray_flags->no_cull_back);

      not_cull =
         nir_iand(b, not_cull,
                  nir_ior(b, not_facing_cull,
                          nir_test_mask(b, nir_load_deref(b, args->vars.sbt_offset_and_flags),
                                        RADV_INSTANCE_TRIANGLE_FACING_CULL_DISABLE)));

      nir_push_if(b, nir_iand(b,

                              nir_flt(b, args->tmin, intersection.t), not_cull));
      {
         intersection.base.node_addr = build_node_to_addr(device, b, bvh_node, false);
         nir_ssa_def *triangle_info = nir_build_load_global(
            b, 2, 32,
            nir_iadd_imm(b, intersection.base.node_addr,
                         offsetof(struct radv_bvh_triangle_node, triangle_id)));
         intersection.base.primitive_id = nir_channel(b, triangle_info, 0);
         intersection.base.geometry_id_and_flags = nir_channel(b, triangle_info, 1);
         intersection.base.opaque =
            hit_is_opaque(b, nir_load_deref(b, args->vars.sbt_offset_and_flags), ray_flags,
                          intersection.base.geometry_id_and_flags);

         not_cull = nir_bcsel(b, intersection.base.opaque, ray_flags->no_cull_opaque,
                              ray_flags->no_cull_no_opaque);
         nir_push_if(b, not_cull);
         {
            nir_ssa_def *divs[2] = {div, div};
            intersection.barycentrics =
               nir_fdiv(b, nir_channels(b, result, 0xc), nir_vec(b, divs, 2));

            args->triangle_cb(b, &intersection, args, ray_flags);
         }
         nir_pop_if(b, NULL);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
}

static void
insert_traversal_aabb_case(struct radv_device *device, nir_builder *b,
                           const struct radv_ray_traversal_args *args,
                           const struct radv_ray_flags *ray_flags, nir_ssa_def *bvh_node)
{
   if (!args->aabb_cb)
      return;

   struct radv_leaf_intersection intersection;
   intersection.node_addr = build_node_to_addr(device, b, bvh_node, false);
   nir_ssa_def *triangle_info =
      nir_build_load_global(b, 2, 32, nir_iadd_imm(b, intersection.node_addr, 24));
   intersection.primitive_id = nir_channel(b, triangle_info, 0);
   intersection.geometry_id_and_flags = nir_channel(b, triangle_info, 1);
   intersection.opaque = hit_is_opaque(b, nir_load_deref(b, args->vars.sbt_offset_and_flags),
                                       ray_flags, intersection.geometry_id_and_flags);

   nir_ssa_def *not_cull =
      nir_bcsel(b, intersection.opaque, ray_flags->no_cull_opaque, ray_flags->no_cull_no_opaque);
   not_cull = nir_iand(b, not_cull, ray_flags->no_skip_aabbs);
   nir_push_if(b, not_cull);
   {
      args->aabb_cb(b, &intersection, args);
   }
   nir_pop_if(b, NULL);
}

static nir_ssa_def *
fetch_parent_node(nir_builder *b, nir_ssa_def *bvh, nir_ssa_def *node)
{
   nir_ssa_def *offset = nir_iadd_imm(b, nir_imul_imm(b, nir_udiv_imm(b, node, 8), 4), 4);

   return nir_build_load_global(b, 1, 32, nir_isub(b, bvh, nir_u2u64(b, offset)), .align_mul = 4);
}

nir_ssa_def *
radv_build_ray_traversal(struct radv_device *device, nir_builder *b,
                         const struct radv_ray_traversal_args *args)
{
   nir_variable *incomplete = nir_local_variable_create(b->impl, glsl_bool_type(), "incomplete");
   nir_store_var(b, incomplete, nir_ine_imm(b, args->root_bvh_base, 0), 0x1);

   nir_push_if(b, nir_load_var(b, incomplete));
   {
      nir_ssa_def *desc = create_bvh_descriptor(b);
      nir_ssa_def *vec3ones = nir_imm_vec3(b, 1.0, 1.0, 1.0);

      struct radv_ray_flags ray_flags = {
         .force_opaque = nir_test_mask(b, args->flags, SpvRayFlagsOpaqueKHRMask),
         .force_not_opaque = nir_test_mask(b, args->flags, SpvRayFlagsNoOpaqueKHRMask),
         .terminate_on_first_hit =
            nir_test_mask(b, args->flags, SpvRayFlagsTerminateOnFirstHitKHRMask),
         .no_cull_front = nir_ieq_imm(
            b, nir_iand_imm(b, args->flags, SpvRayFlagsCullFrontFacingTrianglesKHRMask), 0),
         .no_cull_back = nir_ieq_imm(
            b, nir_iand_imm(b, args->flags, SpvRayFlagsCullBackFacingTrianglesKHRMask), 0),
         .no_cull_opaque =
            nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsCullOpaqueKHRMask), 0),
         .no_cull_no_opaque =
            nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsCullNoOpaqueKHRMask), 0),
         .no_skip_triangles =
            nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsSkipTrianglesKHRMask), 0),
         .no_skip_aabbs =
            nir_ieq_imm(b, nir_iand_imm(b, args->flags, SpvRayFlagsSkipAABBsKHRMask), 0),
      };
      nir_push_loop(b);
      {
         nir_push_if(
            b, nir_ieq_imm(b, nir_load_deref(b, args->vars.current_node), RADV_BVH_INVALID_NODE));
         {
            /* Early exit if we never overflowed the stack, to avoid having to backtrack to
             * the root for no reason. */
            nir_push_if(b, nir_ilt(b, nir_load_deref(b, args->vars.stack),
                                   nir_imm_int(b, args->stack_stride)));
            {
               nir_store_var(b, incomplete, nir_imm_bool(b, false), 0x1);
               nir_jump(b, nir_jump_break);
            }
            nir_pop_if(b, NULL);

            nir_ssa_def *stack_instance_exit = nir_ige(b, nir_load_deref(b, args->vars.top_stack),
                                                       nir_load_deref(b, args->vars.stack));
            nir_ssa_def *root_instance_exit =
               nir_ieq(b, nir_load_deref(b, args->vars.previous_node),
                       nir_load_deref(b, args->vars.instance_bottom_node));
            nir_if *instance_exit =
               nir_push_if(b, nir_ior(b, stack_instance_exit, root_instance_exit));
            instance_exit->control = nir_selection_control_dont_flatten;
            {
               nir_store_deref(b, args->vars.top_stack, nir_imm_int(b, -1), 1);
               nir_store_deref(b, args->vars.previous_node,
                               nir_load_deref(b, args->vars.instance_top_node), 1);
               nir_store_deref(b, args->vars.instance_bottom_node,
                               nir_imm_int(b, RADV_BVH_NO_INSTANCE_ROOT), 1);

               nir_store_deref(b, args->vars.bvh_base, args->root_bvh_base, 1);
               nir_store_deref(b, args->vars.origin, args->origin, 7);
               nir_store_deref(b, args->vars.dir, args->dir, 7);
               nir_store_deref(b, args->vars.inv_dir, nir_fdiv(b, vec3ones, args->dir), 7);
            }
            nir_pop_if(b, NULL);

            nir_push_if(b, nir_ige(b, nir_load_deref(b, args->vars.stack_base),
                                   nir_load_deref(b, args->vars.stack)));
            {
               nir_ssa_def *prev = nir_load_deref(b, args->vars.previous_node);
               nir_ssa_def *bvh_addr =
                  build_node_to_addr(device, b, nir_load_deref(b, args->vars.bvh_base), true);

               nir_ssa_def *parent = fetch_parent_node(b, bvh_addr, prev);
               nir_push_if(b, nir_ieq(b, parent, nir_imm_int(b, RADV_BVH_INVALID_NODE)));
               {
                  nir_store_var(b, incomplete, nir_imm_bool(b, false), 0x1);
                  nir_jump(b, nir_jump_break);
               }
               nir_pop_if(b, NULL);
               nir_store_deref(b, args->vars.current_node, parent, 0x1);
            }
            nir_push_else(b, NULL);
            {
               nir_store_deref(
                  b, args->vars.stack,
                  nir_iadd_imm(b, nir_load_deref(b, args->vars.stack), -args->stack_stride), 1);

               nir_ssa_def *stack_ptr =
                  nir_umod(b, nir_load_deref(b, args->vars.stack),
                           nir_imm_int(b, args->stack_stride * args->stack_entries));
               nir_ssa_def *bvh_node = args->stack_load_cb(b, stack_ptr, args);
               nir_store_deref(b, args->vars.current_node, bvh_node, 0x1);
               nir_store_deref(b, args->vars.previous_node, nir_imm_int(b, RADV_BVH_INVALID_NODE),
                               0x1);
            }
            nir_pop_if(b, NULL);
         }
         nir_push_else(b, NULL);
         {
            nir_store_deref(b, args->vars.previous_node, nir_imm_int(b, RADV_BVH_INVALID_NODE),
                            0x1);
         }
         nir_pop_if(b, NULL);

         nir_ssa_def *bvh_node = nir_load_deref(b, args->vars.current_node);

         nir_ssa_def *prev_node = nir_load_deref(b, args->vars.previous_node);
         nir_store_deref(b, args->vars.previous_node, bvh_node, 0x1);
         nir_store_deref(b, args->vars.current_node, nir_imm_int(b, RADV_BVH_INVALID_NODE), 0x1);

         nir_ssa_def *global_bvh_node =
            nir_iadd(b, nir_load_deref(b, args->vars.bvh_base), nir_u2u64(b, bvh_node));

         nir_ssa_def *intrinsic_result = NULL;
         if (!radv_emulate_rt(device->physical_device)) {
            intrinsic_result = nir_bvh64_intersect_ray_amd(
               b, 32, desc, nir_unpack_64_2x32(b, global_bvh_node),
               nir_load_deref(b, args->vars.tmax), nir_load_deref(b, args->vars.origin),
               nir_load_deref(b, args->vars.dir), nir_load_deref(b, args->vars.inv_dir));
         }

         nir_ssa_def *node_type = nir_iand_imm(b, bvh_node, 7);
         nir_push_if(b, nir_uge(b, node_type, nir_imm_int(b, radv_bvh_node_box16)));
         {
            nir_push_if(b, nir_uge(b, node_type, nir_imm_int(b, radv_bvh_node_instance)));
            {
               nir_push_if(b, nir_ieq_imm(b, node_type, radv_bvh_node_aabb));
               {
                  insert_traversal_aabb_case(device, b, args, &ray_flags, global_bvh_node);
               }
               nir_push_else(b, NULL);
               {
                  /* instance */
                  nir_ssa_def *instance_node_addr =
                     build_node_to_addr(device, b, global_bvh_node, false);
                  nir_ssa_def *instance_data = nir_build_load_global(
                     b, 4, 32, instance_node_addr, .align_mul = 64, .align_offset = 0);
                  nir_ssa_def *instance_and_mask = nir_channel(b, instance_data, 2);
                  nir_ssa_def *instance_mask = nir_ushr_imm(b, instance_and_mask, 24);

                  nir_push_if(b, nir_ieq_imm(b, nir_iand(b, instance_mask, args->cull_mask), 0));
                  {
                     nir_jump(b, nir_jump_continue);
                  }
                  nir_pop_if(b, NULL);

                  nir_ssa_def *wto_matrix[3];
                  nir_build_wto_matrix_load(b, instance_node_addr, wto_matrix);

                  nir_store_deref(b, args->vars.top_stack, nir_load_deref(b, args->vars.stack), 1);
                  nir_store_deref(b, args->vars.bvh_base,
                                  nir_pack_64_2x32(b, nir_channels(b, instance_data, 0x3)), 1);

                  /* Push the instance root node onto the stack */
                  nir_store_deref(b, args->vars.current_node, nir_imm_int(b, RADV_BVH_ROOT_NODE),
                                  0x1);
                  nir_store_deref(b, args->vars.instance_bottom_node,
                                  nir_imm_int(b, RADV_BVH_ROOT_NODE), 1);
                  nir_store_deref(b, args->vars.instance_top_node, bvh_node, 1);

                  /* Transform the ray into object space */
                  nir_store_deref(b, args->vars.origin,
                                  nir_build_vec3_mat_mult(b, args->origin, wto_matrix, true), 7);
                  nir_store_deref(b, args->vars.dir,
                                  nir_build_vec3_mat_mult(b, args->dir, wto_matrix, false), 7);
                  nir_store_deref(b, args->vars.inv_dir,
                                  nir_fdiv(b, vec3ones, nir_load_deref(b, args->vars.dir)), 7);

                  nir_store_deref(b, args->vars.sbt_offset_and_flags,
                                  nir_channel(b, instance_data, 3), 1);
                  nir_store_deref(b, args->vars.instance_addr, instance_node_addr, 1);
               }
               nir_pop_if(b, NULL);
            }
            nir_push_else(b, NULL);
            {
               nir_ssa_def *result = intrinsic_result;
               if (!result) {
                  /* If we didn't run the intrinsic cause the hardware didn't support it,
                   * emulate ray/box intersection here */
                  result = intersect_ray_amd_software_box(
                     device, b, global_bvh_node, nir_load_deref(b, args->vars.tmax),
                     nir_load_deref(b, args->vars.origin), nir_load_deref(b, args->vars.dir),
                     nir_load_deref(b, args->vars.inv_dir));
               }

               /* box */
               nir_push_if(b, nir_ieq_imm(b, prev_node, RADV_BVH_INVALID_NODE));
               {
                  nir_ssa_def *new_nodes[4];
                  for (unsigned i = 0; i < 4; ++i)
                     new_nodes[i] = nir_channel(b, result, i);

                  for (unsigned i = 1; i < 4; ++i)
                     nir_push_if(b, nir_ine_imm(b, new_nodes[i], RADV_BVH_INVALID_NODE));

                  for (unsigned i = 4; i-- > 1;) {
                     nir_ssa_def *stack = nir_load_deref(b, args->vars.stack);
                     nir_ssa_def *stack_ptr = nir_umod(
                        b, stack, nir_imm_int(b, args->stack_entries * args->stack_stride));
                     args->stack_store_cb(b, stack_ptr, new_nodes[i], args);
                     nir_store_deref(b, args->vars.stack,
                                     nir_iadd_imm(b, stack, args->stack_stride), 1);

                     if (i == 1) {
                        nir_ssa_def *new_base =
                           nir_iadd_imm(b, nir_load_deref(b, args->vars.stack),
                                        -args->stack_entries * args->stack_stride);
                        new_base = nir_imax(b, nir_load_deref(b, args->vars.stack_base), new_base);
                        nir_store_deref(b, args->vars.stack_base, new_base, 0x1);
                     }

                     nir_pop_if(b, NULL);
                  }
                  nir_store_deref(b, args->vars.current_node, new_nodes[0], 0x1);
               }
               nir_push_else(b, NULL);
               {
                  nir_ssa_def *next = nir_imm_int(b, RADV_BVH_INVALID_NODE);
                  for (unsigned i = 0; i < 3; ++i) {
                     next = nir_bcsel(b, nir_ieq(b, prev_node, nir_channel(b, result, i)),
                                      nir_channel(b, result, i + 1), next);
                  }
                  nir_store_deref(b, args->vars.current_node, next, 0x1);
               }
               nir_pop_if(b, NULL);
            }
            nir_pop_if(b, NULL);
         }
         nir_push_else(b, NULL);
         {
            nir_ssa_def *result = intrinsic_result;
            if (!result) {
               /* If we didn't run the intrinsic cause the hardware didn't support it,
                * emulate ray/tri intersection here */
               result = intersect_ray_amd_software_tri(
                  device, b, global_bvh_node, nir_load_deref(b, args->vars.tmax),
                  nir_load_deref(b, args->vars.origin), nir_load_deref(b, args->vars.dir),
                  nir_load_deref(b, args->vars.inv_dir));
            }
            insert_traversal_triangle_case(device, b, args, &ray_flags, result, global_bvh_node);
         }
         nir_pop_if(b, NULL);
      }
      nir_pop_loop(b, NULL);
   }
   nir_pop_if(b, NULL);

   return nir_load_var(b, incomplete);
}
