/*
 * Copyright © 2015 Intel Corporation
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

#include "anv_private.h"

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"
#include "genxml/gen_rt_pack.h"

#include "common/intel_l3_config.h"
#include "common/intel_sample_positions.h"
#include "nir/nir_xfb_info.h"
#include "vk_util.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_render_pass.h"

static uint32_t
vertex_element_comp_control(enum isl_format format, unsigned comp)
{
   uint8_t bits;
   switch (comp) {
   case 0: bits = isl_format_layouts[format].channels.r.bits; break;
   case 1: bits = isl_format_layouts[format].channels.g.bits; break;
   case 2: bits = isl_format_layouts[format].channels.b.bits; break;
   case 3: bits = isl_format_layouts[format].channels.a.bits; break;
   default: unreachable("Invalid component");
   }

   /*
    * Take in account hardware restrictions when dealing with 64-bit floats.
    *
    * From Broadwell spec, command reference structures, page 586:
    *  "When SourceElementFormat is set to one of the *64*_PASSTHRU formats,
    *   64-bit components are stored * in the URB without any conversion. In
    *   this case, vertex elements must be written as 128 or 256 bits, with
    *   VFCOMP_STORE_0 being used to pad the output as required. E.g., if
    *   R64_PASSTHRU is used to copy a 64-bit Red component into the URB,
    *   Component 1 must be specified as VFCOMP_STORE_0 (with Components 2,3
    *   set to VFCOMP_NOSTORE) in order to output a 128-bit vertex element, or
    *   Components 1-3 must be specified as VFCOMP_STORE_0 in order to output
    *   a 256-bit vertex element. Likewise, use of R64G64B64_PASSTHRU requires
    *   Component 3 to be specified as VFCOMP_STORE_0 in order to output a
    *   256-bit vertex element."
    */
   if (bits) {
      return VFCOMP_STORE_SRC;
   } else if (comp >= 2 &&
              !isl_format_layouts[format].channels.b.bits &&
              isl_format_layouts[format].channels.r.type == ISL_RAW) {
      /* When emitting 64-bit attributes, we need to write either 128 or 256
       * bit chunks, using VFCOMP_NOSTORE when not writing the chunk, and
       * VFCOMP_STORE_0 to pad the written chunk */
      return VFCOMP_NOSTORE;
   } else if (comp < 3 ||
              isl_format_layouts[format].channels.r.type == ISL_RAW) {
      /* Note we need to pad with value 0, not 1, due hardware restrictions
       * (see comment above) */
      return VFCOMP_STORE_0;
   } else if (isl_format_layouts[format].channels.r.type == ISL_UINT ||
            isl_format_layouts[format].channels.r.type == ISL_SINT) {
      assert(comp == 3);
      return VFCOMP_STORE_1_INT;
   } else {
      assert(comp == 3);
      return VFCOMP_STORE_1_FP;
   }
}

static void
emit_vertex_input(struct anv_graphics_pipeline *pipeline,
                  const struct vk_vertex_input_state *vi)
{
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   /* Pull inputs_read out of the VS prog data */
   const uint64_t inputs_read = vs_prog_data->inputs_read;
   const uint64_t double_inputs_read =
      vs_prog_data->double_inputs_read & inputs_read;
   assert((inputs_read & ((1 << VERT_ATTRIB_GENERIC0) - 1)) == 0);
   const uint32_t elements = inputs_read >> VERT_ATTRIB_GENERIC0;
   const uint32_t elements_double = double_inputs_read >> VERT_ATTRIB_GENERIC0;
   const bool needs_svgs_elem = vs_prog_data->uses_vertexid ||
                                vs_prog_data->uses_instanceid ||
                                vs_prog_data->uses_firstvertex ||
                                vs_prog_data->uses_baseinstance;

   uint32_t elem_count = __builtin_popcount(elements) -
      __builtin_popcount(elements_double) / 2;

   const uint32_t total_elems =
      MAX2(1, elem_count + needs_svgs_elem + vs_prog_data->uses_drawid);

   uint32_t *p;

   const uint32_t num_dwords = 1 + total_elems * 2;
   p = anv_batch_emitn(&pipeline->base.batch, num_dwords,
                       GENX(3DSTATE_VERTEX_ELEMENTS));
   if (!p)
      return;

   for (uint32_t i = 0; i < total_elems; i++) {
      /* The SKL docs for VERTEX_ELEMENT_STATE say:
       *
       *    "All elements must be valid from Element[0] to the last valid
       *    element. (I.e. if Element[2] is valid then Element[1] and
       *    Element[0] must also be valid)."
       *
       * The SKL docs for 3D_Vertex_Component_Control say:
       *
       *    "Don't store this component. (Not valid for Component 0, but can
       *    be used for Component 1-3)."
       *
       * So we can't just leave a vertex element blank and hope for the best.
       * We have to tell the VF hardware to put something in it; so we just
       * store a bunch of zero.
       *
       * TODO: Compact vertex elements so we never end up with holes.
       */
      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .Valid = true,
         .Component0Control = VFCOMP_STORE_0,
         .Component1Control = VFCOMP_STORE_0,
         .Component2Control = VFCOMP_STORE_0,
         .Component3Control = VFCOMP_STORE_0,
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL, &p[1 + i * 2], &element);
   }

   u_foreach_bit(a, vi->attributes_valid) {
      enum isl_format format = anv_get_isl_format(pipeline->base.device->info,
                                                  vi->attributes[a].format,
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                                  VK_IMAGE_TILING_LINEAR);

      uint32_t binding = vi->attributes[a].binding;
      assert(binding < MAX_VBS);

      if ((elements & (1 << a)) == 0)
         continue; /* Binding unused */

      uint32_t slot =
         __builtin_popcount(elements & ((1 << a) - 1)) -
         DIV_ROUND_UP(__builtin_popcount(elements_double &
                                        ((1 << a) -1)), 2);

      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .VertexBufferIndex = vi->attributes[a].binding,
         .Valid = true,
         .SourceElementFormat = format,
         .EdgeFlagEnable = false,
         .SourceElementOffset = vi->attributes[a].offset,
         .Component0Control = vertex_element_comp_control(format, 0),
         .Component1Control = vertex_element_comp_control(format, 1),
         .Component2Control = vertex_element_comp_control(format, 2),
         .Component3Control = vertex_element_comp_control(format, 3),
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL, &p[1 + slot * 2], &element);

      /* On Broadwell and later, we have a separate VF_INSTANCING packet
       * that controls instancing.  On Haswell and prior, that's part of
       * VERTEX_BUFFER_STATE which we emit later.
       */
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_VF_INSTANCING), vfi) {
         bool per_instance = pipeline->vb[binding].instanced;
         uint32_t divisor = pipeline->vb[binding].instance_divisor *
                            pipeline->instance_multiplier;

         vfi.InstancingEnable = per_instance;
         vfi.VertexElementIndex = slot;
         vfi.InstanceDataStepRate = per_instance ? divisor : 1;
      }
   }

   const uint32_t id_slot = elem_count;
   if (needs_svgs_elem) {
      /* From the Broadwell PRM for the 3D_Vertex_Component_Control enum:
       *    "Within a VERTEX_ELEMENT_STATE structure, if a Component
       *    Control field is set to something other than VFCOMP_STORE_SRC,
       *    no higher-numbered Component Control fields may be set to
       *    VFCOMP_STORE_SRC"
       *
       * This means, that if we have BaseInstance, we need BaseVertex as
       * well.  Just do all or nothing.
       */
      uint32_t base_ctrl = (vs_prog_data->uses_firstvertex ||
                            vs_prog_data->uses_baseinstance) ?
                           VFCOMP_STORE_SRC : VFCOMP_STORE_0;

      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .VertexBufferIndex = ANV_SVGS_VB_INDEX,
         .Valid = true,
         .SourceElementFormat = ISL_FORMAT_R32G32_UINT,
         .Component0Control = base_ctrl,
         .Component1Control = base_ctrl,
         .Component2Control = VFCOMP_STORE_0,
         .Component3Control = VFCOMP_STORE_0,
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL, &p[1 + id_slot * 2], &element);

      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_VF_INSTANCING), vfi) {
         vfi.VertexElementIndex = id_slot;
      }
   }

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_VF_SGVS), sgvs) {
      sgvs.VertexIDEnable              = vs_prog_data->uses_vertexid;
      sgvs.VertexIDComponentNumber     = 2;
      sgvs.VertexIDElementOffset       = id_slot;
      sgvs.InstanceIDEnable            = vs_prog_data->uses_instanceid;
      sgvs.InstanceIDComponentNumber   = 3;
      sgvs.InstanceIDElementOffset     = id_slot;
   }

   const uint32_t drawid_slot = elem_count + needs_svgs_elem;
   if (vs_prog_data->uses_drawid) {
      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .VertexBufferIndex = ANV_DRAWID_VB_INDEX,
         .Valid = true,
         .SourceElementFormat = ISL_FORMAT_R32_UINT,
         .Component0Control = VFCOMP_STORE_SRC,
         .Component1Control = VFCOMP_STORE_0,
         .Component2Control = VFCOMP_STORE_0,
         .Component3Control = VFCOMP_STORE_0,
      };
      GENX(VERTEX_ELEMENT_STATE_pack)(NULL,
                                      &p[1 + drawid_slot * 2],
                                      &element);

      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_VF_INSTANCING), vfi) {
         vfi.VertexElementIndex = drawid_slot;
      }
   }
}

void
genX(emit_urb_setup)(struct anv_device *device, struct anv_batch *batch,
                     const struct intel_l3_config *l3_config,
                     VkShaderStageFlags active_stages,
                     const unsigned entry_size[4],
                     enum intel_urb_deref_block_size *deref_block_size)
{
   const struct intel_device_info *devinfo = device->info;

   unsigned entries[4];
   unsigned start[4];
   bool constrained;
   intel_get_urb_config(devinfo, l3_config,
                        active_stages &
                           VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                        active_stages & VK_SHADER_STAGE_GEOMETRY_BIT,
                        entry_size, entries, start, deref_block_size,
                        &constrained);

   for (int i = 0; i <= MESA_SHADER_GEOMETRY; i++) {
      anv_batch_emit(batch, GENX(3DSTATE_URB_VS), urb) {
         urb._3DCommandSubOpcode      += i;
         urb.VSURBStartingAddress      = start[i];
         urb.VSURBEntryAllocationSize  = entry_size[i] - 1;
         urb.VSNumberofURBEntries      = entries[i];
      }
   }
#if GFX_VERx10 >= 125
   if (device->physical->vk.supported_extensions.NV_mesh_shader ||
       device->physical->vk.supported_extensions.EXT_mesh_shader) {
      anv_batch_emit(batch, GENX(3DSTATE_URB_ALLOC_MESH), zero);
      anv_batch_emit(batch, GENX(3DSTATE_URB_ALLOC_TASK), zero);
   }
#endif
}

#if GFX_VERx10 >= 125
static void
emit_urb_setup_mesh(struct anv_graphics_pipeline *pipeline,
                    enum intel_urb_deref_block_size *deref_block_size)
{
   const struct intel_device_info *devinfo = pipeline->base.device->info;

   const struct brw_task_prog_data *task_prog_data =
      anv_pipeline_has_stage(pipeline, MESA_SHADER_TASK) ?
      get_task_prog_data(pipeline) : NULL;
   const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);

   const struct intel_mesh_urb_allocation alloc =
      intel_get_mesh_urb_config(devinfo, pipeline->base.l3_config,
                                task_prog_data ? task_prog_data->map.size_dw : 0,
                                mesh_prog_data->map.size_dw);

   /* Zero out the primitive pipeline URB allocations. */
   for (int i = 0; i <= MESA_SHADER_GEOMETRY; i++) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_URB_VS), urb) {
         urb._3DCommandSubOpcode += i;
      }
   }

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_URB_ALLOC_TASK), urb) {
      if (task_prog_data) {
         urb.TASKURBEntryAllocationSize   = alloc.task_entry_size_64b - 1;
         urb.TASKNumberofURBEntriesSlice0 = alloc.task_entries;
         urb.TASKNumberofURBEntriesSliceN = alloc.task_entries;
         urb.TASKURBStartingAddressSlice0 = alloc.task_starting_address_8kb;
         urb.TASKURBStartingAddressSliceN = alloc.task_starting_address_8kb;
      }
   }

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_URB_ALLOC_MESH), urb) {
      urb.MESHURBEntryAllocationSize   = alloc.mesh_entry_size_64b - 1;
      urb.MESHNumberofURBEntriesSlice0 = alloc.mesh_entries;
      urb.MESHNumberofURBEntriesSliceN = alloc.mesh_entries;
      urb.MESHURBStartingAddressSlice0 = alloc.mesh_starting_address_8kb;
      urb.MESHURBStartingAddressSliceN = alloc.mesh_starting_address_8kb;
   }

   *deref_block_size = alloc.deref_block_size;
}
#endif

static void
emit_urb_setup(struct anv_graphics_pipeline *pipeline,
               enum intel_urb_deref_block_size *deref_block_size)
{
#if GFX_VERx10 >= 125
   if (anv_pipeline_is_mesh(pipeline)) {
      emit_urb_setup_mesh(pipeline, deref_block_size);
      return;
   }
#endif

   unsigned entry_size[4];
   for (int i = MESA_SHADER_VERTEX; i <= MESA_SHADER_GEOMETRY; i++) {
      const struct brw_vue_prog_data *prog_data =
         !anv_pipeline_has_stage(pipeline, i) ? NULL :
         (const struct brw_vue_prog_data *) pipeline->shaders[i]->prog_data;

      entry_size[i] = prog_data ? prog_data->urb_entry_size : 1;
   }

   genX(emit_urb_setup)(pipeline->base.device, &pipeline->base.batch,
                        pipeline->base.l3_config,
                        pipeline->active_stages, entry_size,
                        deref_block_size);
}

static void
emit_3dstate_sbe(struct anv_graphics_pipeline *pipeline)
{
   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_SBE), sbe);
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_SBE_SWIZ), sbe);
#if GFX_VERx10 >= 125
      if (anv_pipeline_is_mesh(pipeline))
         anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_SBE_MESH), sbe_mesh);
#endif
      return;
   }

   struct GENX(3DSTATE_SBE) sbe = {
      GENX(3DSTATE_SBE_header),
      /* TODO(mesh): Figure out cases where we need attribute swizzling.  See also
       * calculate_urb_setup() and related functions.
       */
      .AttributeSwizzleEnable = anv_pipeline_is_primitive(pipeline),
      .PointSpriteTextureCoordinateOrigin = UPPERLEFT,
      .NumberofSFOutputAttributes = wm_prog_data->num_varying_inputs,
      .ConstantInterpolationEnable = wm_prog_data->flat_inputs,
   };

   for (unsigned i = 0; i < 32; i++)
      sbe.AttributeActiveComponentFormat[i] = ACF_XYZW;

   /* On Broadwell, they broke 3DSTATE_SBE into two packets */
   struct GENX(3DSTATE_SBE_SWIZ) swiz = {
      GENX(3DSTATE_SBE_SWIZ_header),
   };

   if (anv_pipeline_is_primitive(pipeline)) {
      const struct brw_vue_map *fs_input_map =
         &anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map;

      int first_slot = brw_compute_first_urb_slot_required(wm_prog_data->inputs,
                                                           fs_input_map);
      assert(first_slot % 2 == 0);
      unsigned urb_entry_read_offset = first_slot / 2;
      int max_source_attr = 0;
      for (uint8_t idx = 0; idx < wm_prog_data->urb_setup_attribs_count; idx++) {
         uint8_t attr = wm_prog_data->urb_setup_attribs[idx];
         int input_index = wm_prog_data->urb_setup[attr];

         assert(0 <= input_index);

         /* gl_Viewport, gl_Layer and FragmentShadingRateKHR are stored in the
          * VUE header
          */
         if (attr == VARYING_SLOT_VIEWPORT ||
             attr == VARYING_SLOT_LAYER ||
             attr == VARYING_SLOT_PRIMITIVE_SHADING_RATE) {
            continue;
         }

         if (attr == VARYING_SLOT_PNTC) {
            sbe.PointSpriteTextureCoordinateEnable = 1 << input_index;
            continue;
         }

         const int slot = fs_input_map->varying_to_slot[attr];

         if (slot == -1) {
            /* This attribute does not exist in the VUE--that means that the
             * vertex shader did not write to it.  It could be that it's a
             * regular varying read by the fragment shader but not written by
             * the vertex shader or it's gl_PrimitiveID. In the first case the
             * value is undefined, in the second it needs to be
             * gl_PrimitiveID.
             */
            swiz.Attribute[input_index].ConstantSource = PRIM_ID;
            swiz.Attribute[input_index].ComponentOverrideX = true;
            swiz.Attribute[input_index].ComponentOverrideY = true;
            swiz.Attribute[input_index].ComponentOverrideZ = true;
            swiz.Attribute[input_index].ComponentOverrideW = true;
            continue;
         }

         /* We have to subtract two slots to account for the URB entry output
          * read offset in the VS and GS stages.
          */
         const int source_attr = slot - 2 * urb_entry_read_offset;
         assert(source_attr >= 0 && source_attr < 32);
         max_source_attr = MAX2(max_source_attr, source_attr);
         /* The hardware can only do overrides on 16 overrides at a time, and the
          * other up to 16 have to be lined up so that the input index = the
          * output index. We'll need to do some tweaking to make sure that's the
          * case.
          */
         if (input_index < 16)
            swiz.Attribute[input_index].SourceAttribute = source_attr;
         else
            assert(source_attr == input_index);
      }

      sbe.VertexURBEntryReadOffset = urb_entry_read_offset;
      sbe.VertexURBEntryReadLength = DIV_ROUND_UP(max_source_attr + 1, 2);
      sbe.ForceVertexURBEntryReadOffset = true;
      sbe.ForceVertexURBEntryReadLength = true;
   } else {
      assert(anv_pipeline_is_mesh(pipeline));
#if GFX_VERx10 >= 125
      const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_SBE_MESH), sbe_mesh) {
         const struct brw_mue_map *mue = &mesh_prog_data->map;

         assert(mue->per_vertex_header_size_dw % 8 == 0);
         sbe_mesh.PerVertexURBEntryOutputReadOffset = mue->per_vertex_header_size_dw / 8;
         sbe_mesh.PerVertexURBEntryOutputReadLength = DIV_ROUND_UP(mue->per_vertex_data_size_dw, 8);

         /* Clip distance array is passed in the per-vertex header so that
          * it can be consumed by the HW. If user wants to read it in the FS,
          * adjust the offset and length to cover it. Conveniently it is at
          * the end of the per-vertex header, right before per-vertex
          * attributes.
          *
          * Note that FS attribute reading must be aware that the clip
          * distances have fixed position.
          */
         if (mue->per_vertex_header_size_dw > 8 &&
               (wm_prog_data->urb_setup[VARYING_SLOT_CLIP_DIST0] >= 0 ||
                wm_prog_data->urb_setup[VARYING_SLOT_CLIP_DIST1] >= 0)) {
            sbe_mesh.PerVertexURBEntryOutputReadOffset -= 1;
            sbe_mesh.PerVertexURBEntryOutputReadLength += 1;
         }

         assert(mue->per_primitive_header_size_dw % 8 == 0);
         sbe_mesh.PerPrimitiveURBEntryOutputReadOffset = mue->per_primitive_header_size_dw / 8;
         sbe_mesh.PerPrimitiveURBEntryOutputReadLength = DIV_ROUND_UP(mue->per_primitive_data_size_dw, 8);

         /* Just like with clip distances, if Primitive Shading Rate,
          * Viewport Index or Layer is read back in the FS, adjust
          * the offset and length to cover the Primitive Header, where
          * PSR, Viewport Index & Layer are stored.
          */
         if (wm_prog_data->urb_setup[VARYING_SLOT_VIEWPORT] >= 0 ||
             wm_prog_data->urb_setup[VARYING_SLOT_PRIMITIVE_SHADING_RATE] >= 0 ||
             wm_prog_data->urb_setup[VARYING_SLOT_LAYER] >= 0) {
            assert(sbe_mesh.PerPrimitiveURBEntryOutputReadOffset > 0);
            sbe_mesh.PerPrimitiveURBEntryOutputReadOffset -= 1;
            sbe_mesh.PerPrimitiveURBEntryOutputReadLength += 1;
         }
      }
#endif
   }

   uint32_t *dw = anv_batch_emit_dwords(&pipeline->base.batch,
                                        GENX(3DSTATE_SBE_length));
   if (!dw)
      return;
   GENX(3DSTATE_SBE_pack)(&pipeline->base.batch, dw, &sbe);

   dw = anv_batch_emit_dwords(&pipeline->base.batch, GENX(3DSTATE_SBE_SWIZ_length));
   if (!dw)
      return;
   GENX(3DSTATE_SBE_SWIZ_pack)(&pipeline->base.batch, dw, &swiz);
}

/** Returns the final polygon mode for rasterization
 *
 * This function takes into account polygon mode, primitive topology and the
 * different shader stages which might generate their own type of primitives.
 */
VkPolygonMode
genX(raster_polygon_mode)(struct anv_graphics_pipeline *pipeline,
                          VkPolygonMode polygon_mode,
                          VkPrimitiveTopology primitive_topology)
{
   if (anv_pipeline_is_mesh(pipeline)) {
      switch (get_mesh_prog_data(pipeline)->primitive_type) {
      case SHADER_PRIM_POINTS:
         return VK_POLYGON_MODE_POINT;
      case SHADER_PRIM_LINES:
         return VK_POLYGON_MODE_LINE;
      case SHADER_PRIM_TRIANGLES:
         return polygon_mode;
      default:
         unreachable("invalid primitive type for mesh");
      }
   } else if (anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY)) {
      switch (get_gs_prog_data(pipeline)->output_topology) {
      case _3DPRIM_POINTLIST:
         return VK_POLYGON_MODE_POINT;

      case _3DPRIM_LINELIST:
      case _3DPRIM_LINESTRIP:
      case _3DPRIM_LINELOOP:
         return VK_POLYGON_MODE_LINE;

      case _3DPRIM_TRILIST:
      case _3DPRIM_TRIFAN:
      case _3DPRIM_TRISTRIP:
      case _3DPRIM_RECTLIST:
      case _3DPRIM_QUADLIST:
      case _3DPRIM_QUADSTRIP:
      case _3DPRIM_POLYGON:
         return polygon_mode;
      }
      unreachable("Unsupported GS output topology");
   } else if (anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
      switch (get_tes_prog_data(pipeline)->output_topology) {
      case BRW_TESS_OUTPUT_TOPOLOGY_POINT:
         return VK_POLYGON_MODE_POINT;

      case BRW_TESS_OUTPUT_TOPOLOGY_LINE:
         return VK_POLYGON_MODE_LINE;

      case BRW_TESS_OUTPUT_TOPOLOGY_TRI_CW:
      case BRW_TESS_OUTPUT_TOPOLOGY_TRI_CCW:
         return polygon_mode;
      }
      unreachable("Unsupported TCS output topology");
   } else {
      switch (primitive_topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
         return VK_POLYGON_MODE_POINT;

      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
         return VK_POLYGON_MODE_LINE;

      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
         return polygon_mode;

      default:
         unreachable("Unsupported primitive topology");
      }
   }
}

const uint32_t genX(vk_to_intel_cullmode)[] = {
   [VK_CULL_MODE_NONE]                       = CULLMODE_NONE,
   [VK_CULL_MODE_FRONT_BIT]                  = CULLMODE_FRONT,
   [VK_CULL_MODE_BACK_BIT]                   = CULLMODE_BACK,
   [VK_CULL_MODE_FRONT_AND_BACK]             = CULLMODE_BOTH
};

const uint32_t genX(vk_to_intel_fillmode)[] = {
   [VK_POLYGON_MODE_FILL]                    = FILL_MODE_SOLID,
   [VK_POLYGON_MODE_LINE]                    = FILL_MODE_WIREFRAME,
   [VK_POLYGON_MODE_POINT]                   = FILL_MODE_POINT,
};

const uint32_t genX(vk_to_intel_front_face)[] = {
   [VK_FRONT_FACE_COUNTER_CLOCKWISE]         = 1,
   [VK_FRONT_FACE_CLOCKWISE]                 = 0
};

void
genX(rasterization_mode)(VkPolygonMode raster_mode,
                         VkLineRasterizationModeEXT line_mode,
                         float line_width,
                         uint32_t *api_mode,
                         bool *msaa_rasterization_enable)
{
   if (raster_mode == VK_POLYGON_MODE_LINE) {
      /* Unfortunately, configuring our line rasterization hardware on gfx8
       * and later is rather painful.  Instead of giving us bits to tell the
       * hardware what line mode to use like we had on gfx7, we now have an
       * arcane combination of API Mode and MSAA enable bits which do things
       * in a table which are expected to magically put the hardware into the
       * right mode for your API.  Sadly, Vulkan isn't any of the APIs the
       * hardware people thought of so nothing works the way you want it to.
       *
       * Look at the table titled "Multisample Rasterization Modes" in Vol 7
       * of the Skylake PRM for more details.
       */
      switch (line_mode) {
      case VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT:
         *api_mode = DX101;
#if GFX_VER <= 9
         /* Prior to ICL, the algorithm the HW uses to draw wide lines
          * doesn't quite match what the CTS expects, at least for rectangular
          * lines, so we set this to false here, making it draw parallelograms
          * instead, which work well enough.
          */
         *msaa_rasterization_enable = line_width < 1.0078125;
#else
         *msaa_rasterization_enable = true;
#endif
         break;

      case VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT:
      case VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT:
         *api_mode = DX9OGL;
         *msaa_rasterization_enable = false;
         break;

      default:
         unreachable("Unsupported line rasterization mode");
      }
   } else {
      *api_mode = DX101;
      *msaa_rasterization_enable = true;
   }
}

static void
emit_rs_state(struct anv_graphics_pipeline *pipeline,
              const struct vk_input_assembly_state *ia,
              const struct vk_rasterization_state *rs,
              const struct vk_multisample_state *ms,
              const struct vk_render_pass_state *rp,
              enum intel_urb_deref_block_size urb_deref_block_size)
{
   struct GENX(3DSTATE_SF) sf = {
      GENX(3DSTATE_SF_header),
   };

   sf.ViewportTransformEnable = true;
   sf.StatisticsEnable = true;
   sf.VertexSubPixelPrecisionSelect = _8Bit;
   sf.AALineDistanceMode = true;

#if GFX_VER >= 12
   sf.DerefBlockSize = urb_deref_block_size;
#endif

   bool point_from_shader;
   if (anv_pipeline_is_primitive(pipeline)) {
      const struct brw_vue_prog_data *last_vue_prog_data =
         anv_pipeline_get_last_vue_prog_data(pipeline);
      point_from_shader = last_vue_prog_data->vue_map.slots_valid & VARYING_BIT_PSIZ;
   } else {
      assert(anv_pipeline_is_mesh(pipeline));
      const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);
      point_from_shader = mesh_prog_data->map.start_dw[VARYING_SLOT_PSIZ] >= 0;
   }

   if (point_from_shader) {
      sf.PointWidthSource = Vertex;
   } else {
      sf.PointWidthSource = State;
      sf.PointWidth = 1.0;
   }

   struct GENX(3DSTATE_RASTER) raster = {
      GENX(3DSTATE_RASTER_header),
   };

   /* For details on 3DSTATE_RASTER multisample state, see the BSpec table
    * "Multisample Modes State".
    */
   /* NOTE: 3DSTATE_RASTER::ForcedSampleCount affects the BDW and SKL PMA fix
    * computations.  If we ever set this bit to a different value, they will
    * need to be updated accordingly.
    */
   raster.ForcedSampleCount = FSC_NUMRASTSAMPLES_0;
   raster.ForceMultisampling = false;

   raster.ScissorRectangleEnable = true;

   raster.ConservativeRasterizationEnable =
      rs->conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
   raster.APIMode = DX101;

   GENX(3DSTATE_SF_pack)(NULL, pipeline->gfx8.sf, &sf);
   GENX(3DSTATE_RASTER_pack)(NULL, pipeline->gfx8.raster, &raster);
}

static void
emit_ms_state(struct anv_graphics_pipeline *pipeline,
              const struct vk_multisample_state *ms)
{
   /* On Gfx8+ 3DSTATE_MULTISAMPLE only holds the number of samples. */
   genX(emit_multisample)(&pipeline->base.batch,
                          pipeline->rasterization_samples);
}

const uint32_t genX(vk_to_intel_logic_op)[] = {
   [VK_LOGIC_OP_COPY]                        = LOGICOP_COPY,
   [VK_LOGIC_OP_CLEAR]                       = LOGICOP_CLEAR,
   [VK_LOGIC_OP_AND]                         = LOGICOP_AND,
   [VK_LOGIC_OP_AND_REVERSE]                 = LOGICOP_AND_REVERSE,
   [VK_LOGIC_OP_AND_INVERTED]                = LOGICOP_AND_INVERTED,
   [VK_LOGIC_OP_NO_OP]                       = LOGICOP_NOOP,
   [VK_LOGIC_OP_XOR]                         = LOGICOP_XOR,
   [VK_LOGIC_OP_OR]                          = LOGICOP_OR,
   [VK_LOGIC_OP_NOR]                         = LOGICOP_NOR,
   [VK_LOGIC_OP_EQUIVALENT]                  = LOGICOP_EQUIV,
   [VK_LOGIC_OP_INVERT]                      = LOGICOP_INVERT,
   [VK_LOGIC_OP_OR_REVERSE]                  = LOGICOP_OR_REVERSE,
   [VK_LOGIC_OP_COPY_INVERTED]               = LOGICOP_COPY_INVERTED,
   [VK_LOGIC_OP_OR_INVERTED]                 = LOGICOP_OR_INVERTED,
   [VK_LOGIC_OP_NAND]                        = LOGICOP_NAND,
   [VK_LOGIC_OP_SET]                         = LOGICOP_SET,
};

const uint32_t genX(vk_to_intel_compare_op)[] = {
   [VK_COMPARE_OP_NEVER]                        = PREFILTEROP_NEVER,
   [VK_COMPARE_OP_LESS]                         = PREFILTEROP_LESS,
   [VK_COMPARE_OP_EQUAL]                        = PREFILTEROP_EQUAL,
   [VK_COMPARE_OP_LESS_OR_EQUAL]                = PREFILTEROP_LEQUAL,
   [VK_COMPARE_OP_GREATER]                      = PREFILTEROP_GREATER,
   [VK_COMPARE_OP_NOT_EQUAL]                    = PREFILTEROP_NOTEQUAL,
   [VK_COMPARE_OP_GREATER_OR_EQUAL]             = PREFILTEROP_GEQUAL,
   [VK_COMPARE_OP_ALWAYS]                       = PREFILTEROP_ALWAYS,
};

const uint32_t genX(vk_to_intel_stencil_op)[] = {
   [VK_STENCIL_OP_KEEP]                         = STENCILOP_KEEP,
   [VK_STENCIL_OP_ZERO]                         = STENCILOP_ZERO,
   [VK_STENCIL_OP_REPLACE]                      = STENCILOP_REPLACE,
   [VK_STENCIL_OP_INCREMENT_AND_CLAMP]          = STENCILOP_INCRSAT,
   [VK_STENCIL_OP_DECREMENT_AND_CLAMP]          = STENCILOP_DECRSAT,
   [VK_STENCIL_OP_INVERT]                       = STENCILOP_INVERT,
   [VK_STENCIL_OP_INCREMENT_AND_WRAP]           = STENCILOP_INCR,
   [VK_STENCIL_OP_DECREMENT_AND_WRAP]           = STENCILOP_DECR,
};

const uint32_t genX(vk_to_intel_primitive_type)[] = {
   [VK_PRIMITIVE_TOPOLOGY_POINT_LIST]                    = _3DPRIM_POINTLIST,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST]                     = _3DPRIM_LINELIST,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP]                    = _3DPRIM_LINESTRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]                 = _3DPRIM_TRILIST,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP]                = _3DPRIM_TRISTRIP,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN]                  = _3DPRIM_TRIFAN,
   [VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY]      = _3DPRIM_LINELIST_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY]     = _3DPRIM_LINESTRIP_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY]  = _3DPRIM_TRILIST_ADJ,
   [VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY] = _3DPRIM_TRISTRIP_ADJ,
};

static inline uint32_t *
write_disabled_blend(uint32_t *state)
{
   struct GENX(BLEND_STATE_ENTRY) entry = {
      .WriteDisableAlpha = true,
      .WriteDisableRed = true,
      .WriteDisableGreen = true,
      .WriteDisableBlue = true,
   };
   GENX(BLEND_STATE_ENTRY_pack)(NULL, state, &entry);
   return state + GENX(BLEND_STATE_ENTRY_length);
}

static void
emit_cb_state(struct anv_graphics_pipeline *pipeline,
              const struct vk_color_blend_state *cb,
              const struct vk_multisample_state *ms)
{
   uint32_t surface_count = 0;
   struct anv_pipeline_bind_map *map;
   if (anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      map = &pipeline->shaders[MESA_SHADER_FRAGMENT]->bind_map;
      surface_count = map->surface_count;
   }

   uint32_t *state_pos = pipeline->gfx8.blend_state;

   state_pos += GENX(BLEND_STATE_length);
   for (unsigned i = 0; i < surface_count; i++) {
      struct anv_pipeline_binding *binding = &map->surface_to_descriptor[i];

      /* All color attachments are at the beginning of the binding table */
      if (binding->set != ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS)
         break;

      /* We can have at most 8 attachments */
      assert(i < MAX_RTS);

      if (cb == NULL || binding->index >= cb->attachment_count) {
         state_pos = write_disabled_blend(state_pos);
         continue;
      }

      struct GENX(BLEND_STATE_ENTRY) entry = {
         /* Vulkan specification 1.2.168, VkLogicOp:
          *
          *   "Logical operations are controlled by the logicOpEnable and
          *    logicOp members of VkPipelineColorBlendStateCreateInfo. If
          *    logicOpEnable is VK_TRUE, then a logical operation selected by
          *    logicOp is applied between each color attachment and the
          *    fragment’s corresponding output value, and blending of all
          *    attachments is treated as if it were disabled."
          *
          * From the Broadwell PRM Volume 2d: Command Reference: Structures:
          * BLEND_STATE_ENTRY:
          *
          *   "Enabling LogicOp and Color Buffer Blending at the same time is
          *    UNDEFINED"
          *
          * Above is handled during emit since these states are dynamic.
          */
         .ColorClampRange = COLORCLAMP_RTFORMAT,
         .PreBlendColorClampEnable = true,
         .PostBlendColorClampEnable = true,
      };

      GENX(BLEND_STATE_ENTRY_pack)(NULL, state_pos, &entry);
      state_pos += GENX(BLEND_STATE_ENTRY_length);
   }
}

static void
emit_3dstate_clip(struct anv_graphics_pipeline *pipeline,
                  const struct vk_input_assembly_state *ia,
                  const struct vk_viewport_state *vp,
                  const struct vk_rasterization_state *rs)
{
   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);
   (void) wm_prog_data;

   struct GENX(3DSTATE_CLIP) clip = {
      GENX(3DSTATE_CLIP_header),
   };

   clip.ClipEnable               = true;
   clip.StatisticsEnable         = true;
   clip.EarlyCullEnable          = true;
   clip.GuardbandClipTestEnable  = true;

   clip.VertexSubPixelPrecisionSelect = _8Bit;
   clip.ClipMode = CLIPMODE_NORMAL;

   clip.MinimumPointWidth = 0.125;
   clip.MaximumPointWidth = 255.875;

   /* TODO(mesh): Multiview. */
   if (anv_pipeline_is_primitive(pipeline)) {
      const struct brw_vue_prog_data *last =
         anv_pipeline_get_last_vue_prog_data(pipeline);

      /* From the Vulkan 1.0.45 spec:
       *
       *    "If the last active vertex processing stage shader entry point's
       *    interface does not include a variable decorated with
       *    ViewportIndex, then the first viewport is used."
       */
      if (vp && (last->vue_map.slots_valid & VARYING_BIT_VIEWPORT)) {
         clip.MaximumVPIndex = vp->viewport_count > 0 ?
                               vp->viewport_count - 1 : 0;
      } else {
         clip.MaximumVPIndex = 0;
      }

      /* From the Vulkan 1.0.45 spec:
       *
       *    "If the last active vertex processing stage shader entry point's
       *    interface does not include a variable decorated with Layer, then
       *    the first layer is used."
       */
      clip.ForceZeroRTAIndexEnable =
         !(last->vue_map.slots_valid & VARYING_BIT_LAYER);

   } else if (anv_pipeline_is_mesh(pipeline)) {
      const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);
      if (vp && vp->viewport_count > 0 &&
          mesh_prog_data->map.start_dw[VARYING_SLOT_VIEWPORT] >= 0) {
         clip.MaximumVPIndex = vp->viewport_count - 1;
      }
   }

   clip.NonPerspectiveBarycentricEnable = wm_prog_data ?
      wm_prog_data->uses_nonperspective_interp_modes : 0;

   GENX(3DSTATE_CLIP_pack)(NULL, pipeline->gfx8.clip, &clip);

#if GFX_VERx10 >= 125
   if (anv_pipeline_is_mesh(pipeline)) {
      const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_CLIP_MESH), clip_mesh) {
         clip_mesh.PrimitiveHeaderEnable = mesh_prog_data->map.per_primitive_header_size_dw > 0;
         clip_mesh.UserClipDistanceClipTestEnableBitmask = mesh_prog_data->clip_distance_mask;
         clip_mesh.UserClipDistanceCullTestEnableBitmask = mesh_prog_data->cull_distance_mask;
      }
   }
#endif
}

static void
emit_3dstate_streamout(struct anv_graphics_pipeline *pipeline,
                       const struct vk_rasterization_state *rs)
{
   const struct brw_vue_prog_data *prog_data =
      anv_pipeline_get_last_vue_prog_data(pipeline);
   const struct brw_vue_map *vue_map = &prog_data->vue_map;

   nir_xfb_info *xfb_info;
   if (anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY))
      xfb_info = pipeline->shaders[MESA_SHADER_GEOMETRY]->xfb_info;
   else if (anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL))
      xfb_info = pipeline->shaders[MESA_SHADER_TESS_EVAL]->xfb_info;
   else
      xfb_info = pipeline->shaders[MESA_SHADER_VERTEX]->xfb_info;

   if (xfb_info) {
      struct GENX(SO_DECL) so_decl[MAX_XFB_STREAMS][128];
      int next_offset[MAX_XFB_BUFFERS] = {0, 0, 0, 0};
      int decls[MAX_XFB_STREAMS] = {0, 0, 0, 0};

      memset(so_decl, 0, sizeof(so_decl));

      for (unsigned i = 0; i < xfb_info->output_count; i++) {
         const nir_xfb_output_info *output = &xfb_info->outputs[i];
         unsigned buffer = output->buffer;
         unsigned stream = xfb_info->buffer_to_stream[buffer];

         /* Our hardware is unusual in that it requires us to program SO_DECLs
          * for fake "hole" components, rather than simply taking the offset
          * for each real varying.  Each hole can have size 1, 2, 3, or 4; we
          * program as many size = 4 holes as we can, then a final hole to
          * accommodate the final 1, 2, or 3 remaining.
          */
         int hole_dwords = (output->offset - next_offset[buffer]) / 4;
         while (hole_dwords > 0) {
            so_decl[stream][decls[stream]++] = (struct GENX(SO_DECL)) {
               .HoleFlag = 1,
               .OutputBufferSlot = buffer,
               .ComponentMask = (1 << MIN2(hole_dwords, 4)) - 1,
            };
            hole_dwords -= 4;
         }

         int varying = output->location;
         uint8_t component_mask = output->component_mask;
         /* VARYING_SLOT_PSIZ contains four scalar fields packed together:
          * - VARYING_SLOT_PRIMITIVE_SHADING_RATE in VARYING_SLOT_PSIZ.x
          * - VARYING_SLOT_LAYER                  in VARYING_SLOT_PSIZ.y
          * - VARYING_SLOT_VIEWPORT               in VARYING_SLOT_PSIZ.z
          * - VARYING_SLOT_PSIZ                   in VARYING_SLOT_PSIZ.w
          */
         if (varying == VARYING_SLOT_PRIMITIVE_SHADING_RATE) {
            varying = VARYING_SLOT_PSIZ;
            component_mask = 1 << 0; // SO_DECL_COMPMASK_X
         } else if (varying == VARYING_SLOT_LAYER) {
            varying = VARYING_SLOT_PSIZ;
            component_mask = 1 << 1; // SO_DECL_COMPMASK_Y
         } else if (varying == VARYING_SLOT_VIEWPORT) {
            varying = VARYING_SLOT_PSIZ;
            component_mask = 1 << 2; // SO_DECL_COMPMASK_Z
         } else if (varying == VARYING_SLOT_PSIZ) {
            component_mask = 1 << 3; // SO_DECL_COMPMASK_W
         }

         next_offset[buffer] = output->offset +
                               __builtin_popcount(component_mask) * 4;

         const int slot = vue_map->varying_to_slot[varying];
         if (slot < 0) {
            /* This can happen if the shader never writes to the varying.
             * Insert a hole instead of actual varying data.
             */
            so_decl[stream][decls[stream]++] = (struct GENX(SO_DECL)) {
               .HoleFlag = true,
               .OutputBufferSlot = buffer,
               .ComponentMask = component_mask,
            };
         } else {
            so_decl[stream][decls[stream]++] = (struct GENX(SO_DECL)) {
               .OutputBufferSlot = buffer,
               .RegisterIndex = slot,
               .ComponentMask = component_mask,
            };
         }
      }

      int max_decls = 0;
      for (unsigned s = 0; s < MAX_XFB_STREAMS; s++)
         max_decls = MAX2(max_decls, decls[s]);

      uint8_t sbs[MAX_XFB_STREAMS] = { };
      for (unsigned b = 0; b < MAX_XFB_BUFFERS; b++) {
         if (xfb_info->buffers_written & (1 << b))
            sbs[xfb_info->buffer_to_stream[b]] |= 1 << b;
      }

      /* Wa_16011773973:
       * If SOL is enabled and SO_DECL state has to be programmed,
       *    1. Send 3D State SOL state with SOL disabled
       *    2. Send SO_DECL NP state
       *    3. Send 3D State SOL with SOL Enabled
       */
      if (intel_device_info_is_dg2(pipeline->base.device->info))
         anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_STREAMOUT), so);

      uint32_t *dw = anv_batch_emitn(&pipeline->base.batch, 3 + 2 * max_decls,
                                     GENX(3DSTATE_SO_DECL_LIST),
                                     .StreamtoBufferSelects0 = sbs[0],
                                     .StreamtoBufferSelects1 = sbs[1],
                                     .StreamtoBufferSelects2 = sbs[2],
                                     .StreamtoBufferSelects3 = sbs[3],
                                     .NumEntries0 = decls[0],
                                     .NumEntries1 = decls[1],
                                     .NumEntries2 = decls[2],
                                     .NumEntries3 = decls[3]);

      for (int i = 0; i < max_decls; i++) {
         GENX(SO_DECL_ENTRY_pack)(NULL, dw + 3 + i * 2,
            &(struct GENX(SO_DECL_ENTRY)) {
               .Stream0Decl = so_decl[0][i],
               .Stream1Decl = so_decl[1][i],
               .Stream2Decl = so_decl[2][i],
               .Stream3Decl = so_decl[3][i],
            });
      }

#if GFX_VERx10 == 125
      /* Wa_14015946265: Send PC with CS stall after SO_DECL. */
      anv_batch_emit(&pipeline->base.batch, GENX(PIPE_CONTROL), pc) {
         pc.CommandStreamerStallEnable = true;
      }
#endif
   }

   struct GENX(3DSTATE_STREAMOUT) so = {
      GENX(3DSTATE_STREAMOUT_header),
   };

   if (xfb_info) {
      so.SOFunctionEnable = true;
      so.SOStatisticsEnable = true;

      so.Buffer0SurfacePitch = xfb_info->buffers[0].stride;
      so.Buffer1SurfacePitch = xfb_info->buffers[1].stride;
      so.Buffer2SurfacePitch = xfb_info->buffers[2].stride;
      so.Buffer3SurfacePitch = xfb_info->buffers[3].stride;

      int urb_entry_read_offset = 0;
      int urb_entry_read_length =
         (prog_data->vue_map.num_slots + 1) / 2 - urb_entry_read_offset;

      /* We always read the whole vertex.  This could be reduced at some
       * point by reading less and offsetting the register index in the
       * SO_DECLs.
       */
      so.Stream0VertexReadOffset = urb_entry_read_offset;
      so.Stream0VertexReadLength = urb_entry_read_length - 1;
      so.Stream1VertexReadOffset = urb_entry_read_offset;
      so.Stream1VertexReadLength = urb_entry_read_length - 1;
      so.Stream2VertexReadOffset = urb_entry_read_offset;
      so.Stream2VertexReadLength = urb_entry_read_length - 1;
      so.Stream3VertexReadOffset = urb_entry_read_offset;
      so.Stream3VertexReadLength = urb_entry_read_length - 1;
   }

   GENX(3DSTATE_STREAMOUT_pack)(NULL, pipeline->gfx8.streamout_state, &so);
}

static uint32_t
get_sampler_count(const struct anv_shader_bin *bin)
{
   uint32_t count_by_4 = DIV_ROUND_UP(bin->bind_map.sampler_count, 4);

   /* We can potentially have way more than 32 samplers and that's ok.
    * However, the 3DSTATE_XS packets only have 3 bits to specify how
    * many to pre-fetch and all values above 4 are marked reserved.
    */
   return MIN2(count_by_4, 4);
}

static UNUSED struct anv_address
get_scratch_address(struct anv_pipeline *pipeline,
                    gl_shader_stage stage,
                    const struct anv_shader_bin *bin)
{
   return (struct anv_address) {
      .bo = anv_scratch_pool_alloc(pipeline->device,
                                   &pipeline->device->scratch_pool,
                                   stage, bin->prog_data->total_scratch),
      .offset = 0,
   };
}

static UNUSED uint32_t
get_scratch_space(const struct anv_shader_bin *bin)
{
   return ffs(bin->prog_data->total_scratch / 2048);
}

static UNUSED uint32_t
get_scratch_surf(struct anv_pipeline *pipeline,
                 gl_shader_stage stage,
                 const struct anv_shader_bin *bin)
{
   if (bin->prog_data->total_scratch == 0)
      return 0;

   struct anv_bo *bo =
      anv_scratch_pool_alloc(pipeline->device,
                             &pipeline->device->scratch_pool,
                             stage, bin->prog_data->total_scratch);
   anv_reloc_list_add_bo(pipeline->batch.relocs,
                         pipeline->batch.alloc, bo);
   return anv_scratch_pool_get_surf(pipeline->device,
                                    &pipeline->device->scratch_pool,
                                    bin->prog_data->total_scratch) >> 4;
}

static void
emit_3dstate_vs(struct anv_graphics_pipeline *pipeline)
{
   const struct intel_device_info *devinfo = pipeline->base.device->info;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);
   const struct anv_shader_bin *vs_bin =
      pipeline->shaders[MESA_SHADER_VERTEX];

   assert(anv_pipeline_has_stage(pipeline, MESA_SHADER_VERTEX));

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_VS), vs) {
      vs.Enable               = true;
      vs.StatisticsEnable     = true;
      vs.KernelStartPointer   = vs_bin->kernel.offset;
      vs.SIMD8DispatchEnable  =
         vs_prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8;

      assert(!vs_prog_data->base.base.use_alt_mode);
#if GFX_VER < 11
      vs.SingleVertexDispatch       = false;
#endif
      vs.VectorMaskEnable           = false;
      /* Wa_1606682166:
       * Incorrect TDL's SSP address shift in SARB for 16:6 & 18:8 modes.
       * Disable the Sampler state prefetch functionality in the SARB by
       * programming 0xB000[30] to '1'.
       */
      vs.SamplerCount               = GFX_VER == 11 ? 0 : get_sampler_count(vs_bin);
      vs.BindingTableEntryCount     = vs_bin->bind_map.surface_count;
      vs.FloatingPointMode          = IEEE754;
      vs.IllegalOpcodeExceptionEnable = false;
      vs.SoftwareExceptionEnable    = false;
      vs.MaximumNumberofThreads     = devinfo->max_vs_threads - 1;

      if (GFX_VER == 9 && devinfo->gt == 4 &&
          anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
         /* On Sky Lake GT4, we have experienced some hangs related to the VS
          * cache and tessellation.  It is unknown exactly what is happening
          * but the Haswell docs for the "VS Reference Count Full Force Miss
          * Enable" field of the "Thread Mode" register refer to a HSW bug in
          * which the VUE handle reference count would overflow resulting in
          * internal reference counting bugs.  My (Jason's) best guess is that
          * this bug cropped back up on SKL GT4 when we suddenly had more
          * threads in play than any previous gfx9 hardware.
          *
          * What we do know for sure is that setting this bit when
          * tessellation shaders are in use fixes a GPU hang in Batman: Arkham
          * City when playing with DXVK (https://bugs.freedesktop.org/107280).
          * Disabling the vertex cache with tessellation shaders should only
          * have a minor performance impact as the tessellation shaders are
          * likely generating and processing far more geometry than the vertex
          * stage.
          */
         vs.VertexCacheDisable = true;
      }

      vs.VertexURBEntryReadLength      = vs_prog_data->base.urb_read_length;
      vs.VertexURBEntryReadOffset      = 0;
      vs.DispatchGRFStartRegisterForURBData =
         vs_prog_data->base.base.dispatch_grf_start_reg;

      vs.UserClipDistanceClipTestEnableBitmask =
         vs_prog_data->base.clip_distance_mask;
      vs.UserClipDistanceCullTestEnableBitmask =
         vs_prog_data->base.cull_distance_mask;

#if GFX_VERx10 >= 125
      vs.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_VERTEX, vs_bin);
#else
      vs.PerThreadScratchSpace   = get_scratch_space(vs_bin);
      vs.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base, MESA_SHADER_VERTEX, vs_bin);
#endif
   }
}

static void
emit_3dstate_hs_ds(struct anv_graphics_pipeline *pipeline,
                   const struct vk_tessellation_state *ts)
{
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_EVAL)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_HS), hs);
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_DS), ds);
      return;
   }

   const struct intel_device_info *devinfo = pipeline->base.device->info;
   const struct anv_shader_bin *tcs_bin =
      pipeline->shaders[MESA_SHADER_TESS_CTRL];
   const struct anv_shader_bin *tes_bin =
      pipeline->shaders[MESA_SHADER_TESS_EVAL];

   const struct brw_tcs_prog_data *tcs_prog_data = get_tcs_prog_data(pipeline);
   const struct brw_tes_prog_data *tes_prog_data = get_tes_prog_data(pipeline);

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_HS), hs) {
      hs.Enable = true;
      hs.StatisticsEnable = true;
      hs.KernelStartPointer = tcs_bin->kernel.offset;
      /* Wa_1606682166 */
      hs.SamplerCount = GFX_VER == 11 ? 0 : get_sampler_count(tcs_bin);
      hs.BindingTableEntryCount = tcs_bin->bind_map.surface_count;

#if GFX_VER >= 12
      /* Wa_1604578095:
       *
       *    Hang occurs when the number of max threads is less than 2 times
       *    the number of instance count. The number of max threads must be
       *    more than 2 times the number of instance count.
       */
      assert((devinfo->max_tcs_threads / 2) > tcs_prog_data->instances);
#endif

      hs.MaximumNumberofThreads = devinfo->max_tcs_threads - 1;
      hs.IncludeVertexHandles = true;
      hs.InstanceCount = tcs_prog_data->instances - 1;

      hs.VertexURBEntryReadLength = 0;
      hs.VertexURBEntryReadOffset = 0;
      hs.DispatchGRFStartRegisterForURBData =
         tcs_prog_data->base.base.dispatch_grf_start_reg & 0x1f;
#if GFX_VER >= 12
      hs.DispatchGRFStartRegisterForURBData5 =
         tcs_prog_data->base.base.dispatch_grf_start_reg >> 5;
#endif

#if GFX_VERx10 >= 125
      hs.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_TESS_CTRL, tcs_bin);
#else
      hs.PerThreadScratchSpace = get_scratch_space(tcs_bin);
      hs.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base, MESA_SHADER_TESS_CTRL, tcs_bin);
#endif

#if GFX_VER == 12
      /*  Patch Count threshold specifies the maximum number of patches that
       *  will be accumulated before a thread dispatch is forced.
       */
      hs.PatchCountThreshold = tcs_prog_data->patch_count_threshold;
#endif

      hs.DispatchMode = tcs_prog_data->base.dispatch_mode;
      hs.IncludePrimitiveID = tcs_prog_data->include_primitive_id;
   }

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_DS), ds) {
      ds.Enable = true;
      ds.StatisticsEnable = true;
      ds.KernelStartPointer = tes_bin->kernel.offset;
      /* Wa_1606682166 */
      ds.SamplerCount = GFX_VER == 11 ? 0 : get_sampler_count(tes_bin);
      ds.BindingTableEntryCount = tes_bin->bind_map.surface_count;
      ds.MaximumNumberofThreads = devinfo->max_tes_threads - 1;

      ds.ComputeWCoordinateEnable =
         tes_prog_data->domain == BRW_TESS_DOMAIN_TRI;

      ds.PatchURBEntryReadLength = tes_prog_data->base.urb_read_length;
      ds.PatchURBEntryReadOffset = 0;
      ds.DispatchGRFStartRegisterForURBData =
         tes_prog_data->base.base.dispatch_grf_start_reg;

#if GFX_VER < 11
      ds.DispatchMode =
         tes_prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8 ?
            DISPATCH_MODE_SIMD8_SINGLE_PATCH :
            DISPATCH_MODE_SIMD4X2;
#else
      assert(tes_prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8);
      ds.DispatchMode = DISPATCH_MODE_SIMD8_SINGLE_PATCH;
#endif

      ds.UserClipDistanceClipTestEnableBitmask =
         tes_prog_data->base.clip_distance_mask;
      ds.UserClipDistanceCullTestEnableBitmask =
         tes_prog_data->base.cull_distance_mask;

#if GFX_VER >= 12
      ds.PrimitiveIDNotRequired = !tes_prog_data->include_primitive_id;
#endif
#if GFX_VERx10 >= 125
      ds.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_TESS_EVAL, tes_bin);
#else
      ds.PerThreadScratchSpace = get_scratch_space(tes_bin);
      ds.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base, MESA_SHADER_TESS_EVAL, tes_bin);
#endif
   }
}

static void
emit_3dstate_gs(struct anv_graphics_pipeline *pipeline)
{
   const struct intel_device_info *devinfo = pipeline->base.device->info;
   const struct anv_shader_bin *gs_bin =
      pipeline->shaders[MESA_SHADER_GEOMETRY];

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_GS), gs);
      return;
   }

   const struct brw_gs_prog_data *gs_prog_data = get_gs_prog_data(pipeline);

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_GS), gs) {
      gs.Enable                  = true;
      gs.StatisticsEnable        = true;
      gs.KernelStartPointer      = gs_bin->kernel.offset;
      gs.DispatchMode            = gs_prog_data->base.dispatch_mode;

      gs.SingleProgramFlow       = false;
      gs.VectorMaskEnable        = false;
      /* Wa_1606682166 */
      gs.SamplerCount            = GFX_VER == 11 ? 0 : get_sampler_count(gs_bin);
      gs.BindingTableEntryCount  = gs_bin->bind_map.surface_count;
      gs.IncludeVertexHandles    = gs_prog_data->base.include_vue_handles;
      gs.IncludePrimitiveID      = gs_prog_data->include_primitive_id;

      gs.MaximumNumberofThreads = devinfo->max_gs_threads - 1;

      gs.OutputVertexSize        = gs_prog_data->output_vertex_size_hwords * 2 - 1;
      gs.OutputTopology          = gs_prog_data->output_topology;
      gs.ControlDataFormat       = gs_prog_data->control_data_format;
      gs.ControlDataHeaderSize   = gs_prog_data->control_data_header_size_hwords;
      gs.InstanceControl         = MAX2(gs_prog_data->invocations, 1) - 1;
      gs.ReorderMode             = TRAILING;

      gs.ExpectedVertexCount     = gs_prog_data->vertices_in;
      gs.StaticOutput            = gs_prog_data->static_vertex_count >= 0;
      gs.StaticOutputVertexCount = gs_prog_data->static_vertex_count >= 0 ?
                                   gs_prog_data->static_vertex_count : 0;

      gs.VertexURBEntryReadOffset = 0;
      gs.VertexURBEntryReadLength = gs_prog_data->base.urb_read_length;
      gs.DispatchGRFStartRegisterForURBData =
         gs_prog_data->base.base.dispatch_grf_start_reg;

      gs.UserClipDistanceClipTestEnableBitmask =
         gs_prog_data->base.clip_distance_mask;
      gs.UserClipDistanceCullTestEnableBitmask =
         gs_prog_data->base.cull_distance_mask;

#if GFX_VERx10 >= 125
      gs.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_GEOMETRY, gs_bin);
#else
      gs.PerThreadScratchSpace   = get_scratch_space(gs_bin);
      gs.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base, MESA_SHADER_GEOMETRY, gs_bin);
#endif
   }
}

static void
emit_3dstate_wm(struct anv_graphics_pipeline *pipeline,
                const struct vk_input_assembly_state *ia,
                const struct vk_rasterization_state *rs,
                const struct vk_multisample_state *ms,
                const struct vk_color_blend_state *cb,
                const struct vk_render_pass_state *rp)
{
   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

   struct GENX(3DSTATE_WM) wm = {
      GENX(3DSTATE_WM_header),
   };
   wm.StatisticsEnable                    = true;
   wm.LineEndCapAntialiasingRegionWidth   = _05pixels;
   wm.LineAntialiasingRegionWidth         = _10pixels;
   wm.PointRasterizationRule              = RASTRULE_UPPER_LEFT;

   if (anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      if (wm_prog_data->early_fragment_tests) {
            wm.EarlyDepthStencilControl         = EDSC_PREPS;
      } else if (wm_prog_data->has_side_effects) {
         wm.EarlyDepthStencilControl         = EDSC_PSEXEC;
      } else {
         wm.EarlyDepthStencilControl         = EDSC_NORMAL;
      }

      /* Gen8 hardware tries to compute ThreadDispatchEnable for us but
       * doesn't take into account KillPixels when no depth or stencil
       * writes are enabled.  In order for occlusion queries to work
       * correctly with no attachments, we need to force-enable PS thread
       * dispatch.
       *
       * The BDW docs are pretty clear that that this bit isn't validated
       * and probably shouldn't be used in production:
       *
       *    "This must always be set to Normal. This field should not be
       *    tested for functional validation."
       *
       * Unfortunately, however, the other mechanism we have for doing this
       * is 3DSTATE_PS_EXTRA::PixelShaderHasUAV which causes hangs on BDW.
       * Given two bad options, we choose the one which works.
       */
      pipeline->force_fragment_thread_dispatch =
         wm_prog_data->has_side_effects ||
         wm_prog_data->uses_kill;

      wm.BarycentricInterpolationMode =
         wm_prog_data->barycentric_interp_modes;
   }

   GENX(3DSTATE_WM_pack)(NULL, pipeline->gfx8.wm, &wm);
}

static void
emit_3dstate_ps(struct anv_graphics_pipeline *pipeline,
                const struct vk_multisample_state *ms,
                const struct vk_color_blend_state *cb)
{
   UNUSED const struct intel_device_info *devinfo =
      pipeline->base.device->info;
   const struct anv_shader_bin *fs_bin =
      pipeline->shaders[MESA_SHADER_FRAGMENT];

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PS), ps) {
      }
      return;
   }

   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PS), ps) {
      ps._8PixelDispatchEnable      = wm_prog_data->dispatch_8;
      ps._16PixelDispatchEnable     = wm_prog_data->dispatch_16;
      ps._32PixelDispatchEnable     = wm_prog_data->dispatch_32;

      /* From the Sky Lake PRM 3DSTATE_PS::32 Pixel Dispatch Enable:
       *
       *    "When NUM_MULTISAMPLES = 16 or FORCE_SAMPLE_COUNT = 16, SIMD32
       *    Dispatch must not be enabled for PER_PIXEL dispatch mode."
       *
       * Since 16x MSAA is first introduced on SKL, we don't need to apply
       * the workaround on any older hardware.
       */
      if (!wm_prog_data->persample_dispatch &&
          ms != NULL && ms->rasterization_samples == 16) {
         assert(ps._8PixelDispatchEnable || ps._16PixelDispatchEnable);
         ps._32PixelDispatchEnable = false;
      }

      ps.KernelStartPointer0 = fs_bin->kernel.offset +
                               brw_wm_prog_data_prog_offset(wm_prog_data, ps, 0);
      ps.KernelStartPointer1 = fs_bin->kernel.offset +
                               brw_wm_prog_data_prog_offset(wm_prog_data, ps, 1);
      ps.KernelStartPointer2 = fs_bin->kernel.offset +
                               brw_wm_prog_data_prog_offset(wm_prog_data, ps, 2);

      ps.SingleProgramFlow          = false;
      ps.VectorMaskEnable           = wm_prog_data->uses_vmask;
      /* Wa_1606682166 */
      ps.SamplerCount               = GFX_VER == 11 ? 0 : get_sampler_count(fs_bin);
      ps.BindingTableEntryCount     = fs_bin->bind_map.surface_count;
      ps.PushConstantEnable         = wm_prog_data->base.nr_params > 0 ||
                                      wm_prog_data->base.ubo_ranges[0].length;
      ps.PositionXYOffsetSelect     = wm_prog_data->uses_pos_offset ?
                                      POSOFFSET_SAMPLE: POSOFFSET_NONE;

      ps.MaximumNumberofThreadsPerPSD = devinfo->max_threads_per_psd - 1;

      ps.DispatchGRFStartRegisterForConstantSetupData0 =
         brw_wm_prog_data_dispatch_grf_start_reg(wm_prog_data, ps, 0);
      ps.DispatchGRFStartRegisterForConstantSetupData1 =
         brw_wm_prog_data_dispatch_grf_start_reg(wm_prog_data, ps, 1);
      ps.DispatchGRFStartRegisterForConstantSetupData2 =
         brw_wm_prog_data_dispatch_grf_start_reg(wm_prog_data, ps, 2);

#if GFX_VERx10 >= 125
      ps.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_FRAGMENT, fs_bin);
#else
      ps.PerThreadScratchSpace   = get_scratch_space(fs_bin);
      ps.ScratchSpaceBasePointer =
         get_scratch_address(&pipeline->base, MESA_SHADER_FRAGMENT, fs_bin);
#endif
   }
}

static void
emit_3dstate_ps_extra(struct anv_graphics_pipeline *pipeline,
                      const struct vk_rasterization_state *rs,
                      const struct vk_render_pass_state *rp)
{
   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PS_EXTRA), ps);
      return;
   }

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PS_EXTRA), ps) {
      ps.PixelShaderValid              = true;
      ps.AttributeEnable               = wm_prog_data->num_varying_inputs > 0;
      ps.oMaskPresenttoRenderTarget    = wm_prog_data->uses_omask;
      ps.PixelShaderIsPerSample        = wm_prog_data->persample_dispatch;
      ps.PixelShaderComputedDepthMode  = wm_prog_data->computed_depth_mode;
      ps.PixelShaderUsesSourceDepth    = wm_prog_data->uses_src_depth;
      ps.PixelShaderUsesSourceW        = wm_prog_data->uses_src_w;

      /* If the subpass has a depth or stencil self-dependency, then we need
       * to force the hardware to do the depth/stencil write *after* fragment
       * shader execution.  Otherwise, the writes may hit memory before we get
       * around to fetching from the input attachment and we may get the depth
       * or stencil value from the current draw rather than the previous one.
       */
      ps.PixelShaderKillsPixel         = rp->depth_self_dependency ||
                                         rp->stencil_self_dependency ||
                                         wm_prog_data->uses_kill;

      ps.PixelShaderComputesStencil = wm_prog_data->computed_stencil;
      ps.PixelShaderPullsBary    = wm_prog_data->pulls_bary;

      ps.InputCoverageMaskState = ICMS_NONE;
      assert(!wm_prog_data->inner_coverage); /* Not available in SPIR-V */
      if (!wm_prog_data->uses_sample_mask)
         ps.InputCoverageMaskState = ICMS_NONE;
      else if (wm_prog_data->per_coarse_pixel_dispatch)
         ps.InputCoverageMaskState  = ICMS_NORMAL;
      else if (wm_prog_data->post_depth_coverage)
         ps.InputCoverageMaskState = ICMS_DEPTH_COVERAGE;
      else
         ps.InputCoverageMaskState = ICMS_NORMAL;

#if GFX_VER >= 11
      ps.PixelShaderRequiresSourceDepthandorWPlaneCoefficients =
         wm_prog_data->uses_depth_w_coefficients;
      ps.PixelShaderIsPerCoarsePixel = wm_prog_data->per_coarse_pixel_dispatch;
#endif
#if GFX_VERx10 >= 125
      /* TODO: We should only require this when the last geometry shader uses
       *       a fragment shading rate that is not constant.
       */
      ps.EnablePSDependencyOnCPsizeChange = wm_prog_data->per_coarse_pixel_dispatch;
#endif
   }
}

static void
emit_3dstate_vf_statistics(struct anv_graphics_pipeline *pipeline)
{
   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_VF_STATISTICS), vfs) {
      vfs.StatisticsEnable = true;
   }
}

static void
compute_kill_pixel(struct anv_graphics_pipeline *pipeline,
                   const struct vk_multisample_state *ms,
                   const struct vk_render_pass_state *rp)
{
   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT)) {
      pipeline->kill_pixel = false;
      return;
   }

   const struct brw_wm_prog_data *wm_prog_data = get_wm_prog_data(pipeline);

   /* This computes the KillPixel portion of the computation for whether or
    * not we want to enable the PMA fix on gfx8 or gfx9.  It's given by this
    * chunk of the giant formula:
    *
    *    (3DSTATE_PS_EXTRA::PixelShaderKillsPixels ||
    *     3DSTATE_PS_EXTRA::oMask Present to RenderTarget ||
    *     3DSTATE_PS_BLEND::AlphaToCoverageEnable ||
    *     3DSTATE_PS_BLEND::AlphaTestEnable ||
    *     3DSTATE_WM_CHROMAKEY::ChromaKeyKillEnable)
    *
    * 3DSTATE_WM_CHROMAKEY::ChromaKeyKillEnable is always false and so is
    * 3DSTATE_PS_BLEND::AlphaTestEnable since Vulkan doesn't have a concept
    * of an alpha test.
    */
   pipeline->kill_pixel =
      rp->depth_self_dependency ||
      rp->stencil_self_dependency ||
      wm_prog_data->uses_kill ||
      wm_prog_data->uses_omask ||
      (ms && ms->alpha_to_coverage_enable);
}

#if GFX_VER == 12
static void
emit_3dstate_primitive_replication(struct anv_graphics_pipeline *pipeline,
                                   const struct vk_render_pass_state *rp)
{
   if (anv_pipeline_is_mesh(pipeline)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PRIMITIVE_REPLICATION), pr);
      return;
   }

   const int replication_count =
      anv_pipeline_get_last_vue_prog_data(pipeline)->vue_map.num_pos_slots;

   assert(replication_count >= 1);
   if (replication_count == 1) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PRIMITIVE_REPLICATION), pr);
      return;
   }

   assert(replication_count == util_bitcount(rp->view_mask));
   assert(replication_count <= MAX_VIEWS_FOR_PRIMITIVE_REPLICATION);

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_PRIMITIVE_REPLICATION), pr) {
      pr.ReplicaMask = (1 << replication_count) - 1;
      pr.ReplicationCount = replication_count - 1;

      int i = 0;
      u_foreach_bit(view_index, rp->view_mask) {
         pr.RTAIOffset[i] = view_index;
         i++;
      }
   }
}
#endif

#if GFX_VERx10 >= 125
static void
emit_task_state(struct anv_graphics_pipeline *pipeline)
{
   assert(anv_pipeline_is_mesh(pipeline));

   if (!anv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_TASK_CONTROL), zero);
      return;
   }

   const struct anv_shader_bin *task_bin = pipeline->shaders[MESA_SHADER_TASK];

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_TASK_CONTROL), tc) {
      tc.TaskShaderEnable = true;
      tc.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_TASK, task_bin);
      tc.MaximumNumberofThreadGroups = 511;
   }

   const struct intel_device_info *devinfo = pipeline->base.device->info;
   const struct brw_task_prog_data *task_prog_data = get_task_prog_data(pipeline);
   const struct brw_cs_dispatch_info task_dispatch =
      brw_cs_get_dispatch_info(devinfo, &task_prog_data->base, NULL);

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_TASK_SHADER), task) {
      task.KernelStartPointer                = task_bin->kernel.offset;
      task.SIMDSize                          = task_dispatch.simd_size / 16;
      task.MessageSIMD                       = task.SIMDSize;
      task.NumberofThreadsinGPGPUThreadGroup = task_dispatch.threads;
      task.ExecutionMask                     = task_dispatch.right_mask;
      task.LocalXMaximum                     = task_dispatch.group_size - 1;
      task.EmitLocalIDX                      = true;

      task.NumberofBarriers                  = task_prog_data->base.uses_barrier;
      task.SharedLocalMemorySize             =
         encode_slm_size(GFX_VER, task_prog_data->base.base.total_shared);

      /*
       * 3DSTATE_TASK_SHADER_DATA.InlineData[0:1] will be used for an address
       * of a buffer with push constants and descriptor set table and
       * InlineData[2:7] will be used for first few push constants.
       */
      task.EmitInlineParameter = true;

      task.XP0Required = task_prog_data->uses_drawid;
   }

   /* Recommended values from "Task and Mesh Distribution Programming". */
   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_TASK_REDISTRIB), redistrib) {
      redistrib.LocalBOTAccumulatorThreshold = MULTIPLIER_1;
      redistrib.SmallTaskThreshold = 1; /* 2^N */
      redistrib.TargetMeshBatchSize = devinfo->num_slices > 2 ? 3 : 5; /* 2^N */
      redistrib.TaskRedistributionLevel = TASKREDISTRIB_BOM;

      /* TODO: We have an unknown issue with Task Payload when task redistribution
       * is enabled. Disable it for now.
       * See https://gitlab.freedesktop.org/mesa/mesa/-/issues/7141
       */
      redistrib.TaskRedistributionMode = TASKREDISTRIB_OFF;
   }
}

static void
emit_mesh_state(struct anv_graphics_pipeline *pipeline)
{
   assert(anv_pipeline_is_mesh(pipeline));

   const struct anv_shader_bin *mesh_bin = pipeline->shaders[MESA_SHADER_MESH];

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_MESH_CONTROL), mc) {
      mc.MeshShaderEnable = true;
      mc.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_MESH, mesh_bin);
      mc.MaximumNumberofThreadGroups = 511;
   }

   const struct intel_device_info *devinfo = pipeline->base.device->info;
   const struct brw_mesh_prog_data *mesh_prog_data = get_mesh_prog_data(pipeline);
   const struct brw_cs_dispatch_info mesh_dispatch =
      brw_cs_get_dispatch_info(devinfo, &mesh_prog_data->base, NULL);

   const unsigned output_topology =
      mesh_prog_data->primitive_type == SHADER_PRIM_POINTS ? OUTPUT_POINT :
      mesh_prog_data->primitive_type == SHADER_PRIM_LINES  ? OUTPUT_LINE :
                                                             OUTPUT_TRI;

   uint32_t index_format;
   switch (mesh_prog_data->index_format) {
   case BRW_INDEX_FORMAT_U32:
      index_format = INDEX_U32;
      break;
   default:
      unreachable("invalid index format");
   }

   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_MESH_SHADER), mesh) {
      mesh.KernelStartPointer                = mesh_bin->kernel.offset;
      mesh.SIMDSize                          = mesh_dispatch.simd_size / 16;
      mesh.MessageSIMD                       = mesh.SIMDSize;
      mesh.NumberofThreadsinGPGPUThreadGroup = mesh_dispatch.threads;
      mesh.ExecutionMask                     = mesh_dispatch.right_mask;
      mesh.LocalXMaximum                     = mesh_dispatch.group_size - 1;
      mesh.EmitLocalIDX                      = true;

      mesh.MaximumPrimitiveCount             = mesh_prog_data->map.max_primitives - 1;
      mesh.OutputTopology                    = output_topology;
      mesh.PerVertexDataPitch                = mesh_prog_data->map.per_vertex_pitch_dw / 8;
      mesh.PerPrimitiveDataPresent           = mesh_prog_data->map.per_primitive_pitch_dw > 0;
      mesh.PerPrimitiveDataPitch             = mesh_prog_data->map.per_primitive_pitch_dw / 8;
      mesh.IndexFormat                       = index_format;

      mesh.NumberofBarriers                  = mesh_prog_data->base.uses_barrier;
      mesh.SharedLocalMemorySize             =
         encode_slm_size(GFX_VER, mesh_prog_data->base.base.total_shared);

      /*
       * 3DSTATE_MESH_SHADER_DATA.InlineData[0:1] will be used for an address
       * of a buffer with push constants and descriptor set table and
       * InlineData[2:7] will be used for first few push constants.
       */
      mesh.EmitInlineParameter = true;

      mesh.XP0Required = mesh_prog_data->uses_drawid;
   }

   /* Recommended values from "Task and Mesh Distribution Programming". */
   anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_MESH_DISTRIB), distrib) {
      distrib.DistributionMode = MESH_RR_FREE;
      distrib.TaskDistributionBatchSize = devinfo->num_slices > 2 ? 4 : 9; /* 2^N thread groups */
      distrib.MeshDistributionBatchSize = devinfo->num_slices > 2 ? 3 : 3; /* 2^N thread groups */
   }
}
#endif

void
genX(graphics_pipeline_emit)(struct anv_graphics_pipeline *pipeline,
                             const struct vk_graphics_pipeline_state *state)
{
   enum intel_urb_deref_block_size urb_deref_block_size;
   emit_urb_setup(pipeline, &urb_deref_block_size);

   assert(state->rs != NULL);
   emit_rs_state(pipeline, state->ia, state->rs, state->ms, state->rp,
                           urb_deref_block_size);
   emit_ms_state(pipeline, state->ms);
   emit_cb_state(pipeline, state->cb, state->ms);
   compute_kill_pixel(pipeline, state->ms, state->rp);

   emit_3dstate_clip(pipeline, state->ia, state->vp, state->rs);

#if GFX_VER == 12
   emit_3dstate_primitive_replication(pipeline, state->rp);
#endif

   if (anv_pipeline_is_primitive(pipeline)) {
      emit_vertex_input(pipeline, state->vi);

      emit_3dstate_vs(pipeline);
      emit_3dstate_hs_ds(pipeline, state->ts);
      emit_3dstate_gs(pipeline);

      emit_3dstate_vf_statistics(pipeline);

      emit_3dstate_streamout(pipeline, state->rs);

#if GFX_VERx10 >= 125
      const struct anv_device *device = pipeline->base.device;
      /* Disable Mesh. */
      if (device->physical->vk.supported_extensions.NV_mesh_shader ||
          device->physical->vk.supported_extensions.EXT_mesh_shader) {
         anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_MESH_CONTROL), zero);
         anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_TASK_CONTROL), zero);
      }
#endif
   } else {
      assert(anv_pipeline_is_mesh(pipeline));

      /* BSpec 46303 forbids both 3DSTATE_MESH_CONTROL.MeshShaderEnable
       * and 3DSTATE_STREAMOUT.SOFunctionEnable to be 1.
       */
      anv_batch_emit(&pipeline->base.batch, GENX(3DSTATE_STREAMOUT), so) {}

#if GFX_VERx10 >= 125
      emit_task_state(pipeline);
      emit_mesh_state(pipeline);
#endif
   }

   emit_3dstate_sbe(pipeline);
   emit_3dstate_wm(pipeline, state->ia, state->rs,
                   state->ms, state->cb, state->rp);
   emit_3dstate_ps(pipeline, state->ms, state->cb);
   emit_3dstate_ps_extra(pipeline, state->rs, state->rp);
}

#if GFX_VERx10 >= 125

void
genX(compute_pipeline_emit)(struct anv_compute_pipeline *pipeline)
{
   struct anv_device *device = pipeline->base.device;
   const struct brw_cs_prog_data *cs_prog_data = get_cs_prog_data(pipeline);
   anv_pipeline_setup_l3_config(&pipeline->base, cs_prog_data->base.total_shared > 0);

   const UNUSED struct anv_shader_bin *cs_bin = pipeline->cs;
   const struct intel_device_info *devinfo = device->info;

   anv_batch_emit(&pipeline->base.batch, GENX(CFE_STATE), cfe) {
      cfe.MaximumNumberofThreads =
         devinfo->max_cs_threads * devinfo->subslice_total;
      cfe.ScratchSpaceBuffer =
         get_scratch_surf(&pipeline->base, MESA_SHADER_COMPUTE, cs_bin);
   }
}

#else /* #if GFX_VERx10 >= 125 */

void
genX(compute_pipeline_emit)(struct anv_compute_pipeline *pipeline)
{
   struct anv_device *device = pipeline->base.device;
   const struct intel_device_info *devinfo = device->info;
   const struct brw_cs_prog_data *cs_prog_data = get_cs_prog_data(pipeline);

   anv_pipeline_setup_l3_config(&pipeline->base, cs_prog_data->base.total_shared > 0);

   const struct brw_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, cs_prog_data, NULL);
   const uint32_t vfe_curbe_allocation =
      ALIGN(cs_prog_data->push.per_thread.regs * dispatch.threads +
            cs_prog_data->push.cross_thread.regs, 2);

   const struct anv_shader_bin *cs_bin = pipeline->cs;

   anv_batch_emit(&pipeline->base.batch, GENX(MEDIA_VFE_STATE), vfe) {
      vfe.StackSize              = 0;
      vfe.MaximumNumberofThreads =
         devinfo->max_cs_threads * devinfo->subslice_total - 1;
      vfe.NumberofURBEntries     = 2;
#if GFX_VER < 11
      vfe.ResetGatewayTimer      = true;
#endif
      vfe.URBEntryAllocationSize = 2;
      vfe.CURBEAllocationSize    = vfe_curbe_allocation;

      if (cs_bin->prog_data->total_scratch) {
         /* Broadwell's Per Thread Scratch Space is in the range [0, 11]
          * where 0 = 1k, 1 = 2k, 2 = 4k, ..., 11 = 2M.
          */
         vfe.PerThreadScratchSpace =
            ffs(cs_bin->prog_data->total_scratch) - 11;
         vfe.ScratchSpaceBasePointer =
            get_scratch_address(&pipeline->base, MESA_SHADER_COMPUTE, cs_bin);
      }
   }

   struct GENX(INTERFACE_DESCRIPTOR_DATA) desc = {
      .KernelStartPointer     =
         cs_bin->kernel.offset +
         brw_cs_prog_data_prog_offset(cs_prog_data, dispatch.simd_size),

      /* Wa_1606682166 */
      .SamplerCount           = GFX_VER == 11 ? 0 : get_sampler_count(cs_bin),
      /* We add 1 because the CS indirect parameters buffer isn't accounted
       * for in bind_map.surface_count.
       *
       * Typically set to 0 to avoid prefetching on every thread dispatch.
       */
      .BindingTableEntryCount = devinfo->verx10 == 125 ?
         0 : 1 + MIN2(pipeline->cs->bind_map.surface_count, 30),
      .BarrierEnable          = cs_prog_data->uses_barrier,
      .SharedLocalMemorySize  =
         encode_slm_size(GFX_VER, cs_prog_data->base.total_shared),

      .ConstantURBEntryReadOffset = 0,
      .ConstantURBEntryReadLength = cs_prog_data->push.per_thread.regs,
      .CrossThreadConstantDataReadLength =
         cs_prog_data->push.cross_thread.regs,
#if GFX_VER >= 12
      /* TODO: Check if we are missing workarounds and enable mid-thread
       * preemption.
       *
       * We still have issues with mid-thread preemption (it was already
       * disabled by the kernel on gfx11, due to missing workarounds). It's
       * possible that we are just missing some workarounds, and could enable
       * it later, but for now let's disable it to fix a GPU in compute in Car
       * Chase (and possibly more).
       */
      .ThreadPreemptionDisable = true,
#endif

      .NumberofThreadsinGPGPUThreadGroup = dispatch.threads,
   };
   GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL,
                                        pipeline->interface_descriptor_data,
                                        &desc);
}

#endif /* #if GFX_VERx10 >= 125 */

#if GFX_VERx10 >= 125

void
genX(ray_tracing_pipeline_emit)(struct anv_ray_tracing_pipeline *pipeline)
{
   for (uint32_t i = 0; i < pipeline->group_count; i++) {
      struct anv_rt_shader_group *group = &pipeline->groups[i];

      switch (group->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR: {
         struct GFX_RT_GENERAL_SBT_HANDLE sh = {};
         sh.General = anv_shader_bin_get_bsr(group->general, 32);
         GFX_RT_GENERAL_SBT_HANDLE_pack(NULL, group->handle, &sh);
         break;
      }

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR: {
         struct GFX_RT_TRIANGLES_SBT_HANDLE sh = {};
         if (group->closest_hit)
            sh.ClosestHit = anv_shader_bin_get_bsr(group->closest_hit, 32);
         if (group->any_hit)
            sh.AnyHit = anv_shader_bin_get_bsr(group->any_hit, 24);
         GFX_RT_TRIANGLES_SBT_HANDLE_pack(NULL, group->handle, &sh);
         break;
      }

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR: {
         struct GFX_RT_PROCEDURAL_SBT_HANDLE sh = {};
         if (group->closest_hit)
            sh.ClosestHit = anv_shader_bin_get_bsr(group->closest_hit, 32);
         sh.Intersection = anv_shader_bin_get_bsr(group->intersection, 24);
         GFX_RT_PROCEDURAL_SBT_HANDLE_pack(NULL, group->handle, &sh);
         break;
      }

      default:
         unreachable("Invalid shader group type");
      }
   }
}

#else

void
genX(ray_tracing_pipeline_emit)(struct anv_ray_tracing_pipeline *pipeline)
{
   unreachable("Ray tracing not supported");
}

#endif /* GFX_VERx10 >= 125 */
