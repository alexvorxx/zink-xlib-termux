/*
 * Copyright © 2021 Bas Nieuwenhuizen
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
#include "radv_acceleration_structure.h"
#include "radv_private.h"

#include "nir_builder.h"
#include "radv_cs.h"
#include "radv_meta.h"

#include "radix_sort/radv_radix_sort.h"

#include "bvh/build_interface.h"

static const uint32_t leaf_spv[] = {
#include "bvh/leaf.comp.spv.h"
};

static const uint32_t morton_spv[] = {
#include "bvh/morton.comp.spv.h"
};

static const uint32_t lbvh_internal_spv[] = {
#include "bvh/lbvh_internal.comp.spv.h"
};

static const uint32_t copy_spv[] = {
#include "bvh/copy.comp.spv.h"
};

static const uint32_t convert_leaf_spv[] = {
#include "bvh/converter_leaf.comp.spv.h"
};

static const uint32_t convert_internal_spv[] = {
#include "bvh/converter_internal.comp.spv.h"
};

/* Min and max bounds of the bvh used to compute morton codes */
#define SCRATCH_TOTAL_BOUNDS_SIZE (6 * sizeof(float))

#define KEY_ID_PAIR_SIZE 8

struct acceleration_structure_layout {
   uint32_t size;
};

struct scratch_layout {
   uint32_t size;

   uint32_t bounds_offset;

   uint32_t sort_buffer_offset[2];
   uint32_t sort_internal_offset;

   uint32_t ir_offset;
};

static void
get_build_layout(struct radv_device *device, uint32_t leaf_count,
                 const VkAccelerationStructureBuildGeometryInfoKHR *build_info,
                 struct acceleration_structure_layout *accel_struct, struct scratch_layout *scratch)
{
   /* Initialize to 1 to have enought space for the root node. */
   uint32_t child_count = leaf_count;
   uint32_t internal_count = 1;
   while (child_count > 1) {
      child_count = DIV_ROUND_UP(child_count, 2);
      internal_count += child_count;
   }

   VkGeometryTypeKHR geometry_type = VK_GEOMETRY_TYPE_TRIANGLES_KHR;

   if (build_info->geometryCount) {
      if (build_info->pGeometries)
         geometry_type = build_info->pGeometries[0].geometryType;
      else
         geometry_type = build_info->ppGeometries[0]->geometryType;
   }

   uint32_t bvh_leaf_size;
   uint32_t ir_leaf_size;
   switch (geometry_type) {
   case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
      ir_leaf_size = sizeof(struct radv_ir_triangle_node);
      bvh_leaf_size = sizeof(struct radv_bvh_triangle_node);
      break;
   case VK_GEOMETRY_TYPE_AABBS_KHR:
      ir_leaf_size = sizeof(struct radv_ir_aabb_node);
      bvh_leaf_size = sizeof(struct radv_bvh_aabb_node);
      break;
   case VK_GEOMETRY_TYPE_INSTANCES_KHR:
      ir_leaf_size = sizeof(struct radv_ir_instance_node);
      bvh_leaf_size = sizeof(struct radv_bvh_instance_node);
      break;
   default:
      unreachable("Unknown VkGeometryTypeKHR");
   }

   if (accel_struct) {
      uint32_t offset = 0;
      offset += ALIGN(sizeof(struct radv_accel_struct_header), 64);
      offset += bvh_leaf_size * leaf_count;
      offset += sizeof(struct radv_bvh_box32_node) * internal_count;
      offset += sizeof(struct radv_accel_struct_geometry_info) * build_info->geometryCount;

      accel_struct->size = offset;
   }

   if (scratch) {
      radix_sort_vk_memory_requirements_t requirements;
      radix_sort_vk_get_memory_requirements(device->meta_state.accel_struct_build.radix_sort,
                                            leaf_count, &requirements);

      uint32_t offset = 0;

      scratch->bounds_offset = offset;
      offset += SCRATCH_TOTAL_BOUNDS_SIZE;

      scratch->sort_buffer_offset[0] = offset;
      offset += requirements.keyvals_size;

      scratch->sort_buffer_offset[1] = offset;
      offset += requirements.keyvals_size;

      scratch->sort_internal_offset = offset;
      offset += requirements.internal_size;

      scratch->ir_offset = offset;
      offset += ir_leaf_size * leaf_count;
      offset += sizeof(struct radv_ir_box_node) * internal_count;

      scratch->size = offset;
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_GetAccelerationStructureBuildSizesKHR(
   VkDevice _device, VkAccelerationStructureBuildTypeKHR buildType,
   const VkAccelerationStructureBuildGeometryInfoKHR *pBuildInfo,
   const uint32_t *pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR *pSizeInfo)
{
   RADV_FROM_HANDLE(radv_device, device, _device);

   STATIC_ASSERT(sizeof(struct radv_bvh_triangle_node) == 64);
   STATIC_ASSERT(sizeof(struct radv_bvh_aabb_node) == 64);
   STATIC_ASSERT(sizeof(struct radv_bvh_instance_node) == 128);
   STATIC_ASSERT(sizeof(struct radv_bvh_box16_node) == 64);
   STATIC_ASSERT(sizeof(struct radv_bvh_box32_node) == 128);

   uint32_t leaf_count = 0;
   for (uint32_t i = 0; i < pBuildInfo->geometryCount; i++)
      leaf_count += pMaxPrimitiveCounts[i];

   struct acceleration_structure_layout accel_struct;
   struct scratch_layout scratch;
   get_build_layout(device, leaf_count, pBuildInfo, &accel_struct, &scratch);

   pSizeInfo->accelerationStructureSize = accel_struct.size;
   pSizeInfo->updateScratchSize = scratch.size;
   pSizeInfo->buildScratchSize = scratch.size;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CreateAccelerationStructureKHR(VkDevice _device,
                                    const VkAccelerationStructureCreateInfoKHR *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkAccelerationStructureKHR *pAccelerationStructure)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_buffer, buffer, pCreateInfo->buffer);
   struct radv_acceleration_structure *accel;

   accel = vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*accel), 8,
                     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (accel == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &accel->base, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR);

   accel->mem_offset = buffer->offset + pCreateInfo->offset;
   accel->size = pCreateInfo->size;
   accel->bo = buffer->bo;
   accel->va = radv_buffer_get_va(accel->bo) + accel->mem_offset;

   *pAccelerationStructure = radv_acceleration_structure_to_handle(accel);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
radv_DestroyAccelerationStructureKHR(VkDevice _device,
                                     VkAccelerationStructureKHR accelerationStructure,
                                     const VkAllocationCallbacks *pAllocator)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_acceleration_structure, accel, accelerationStructure);

   if (!accel)
      return;

   vk_object_base_finish(&accel->base);
   vk_free2(&device->vk.alloc, pAllocator, accel);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
radv_GetAccelerationStructureDeviceAddressKHR(
   VkDevice _device, const VkAccelerationStructureDeviceAddressInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_acceleration_structure, accel, pInfo->accelerationStructure);
   return accel->va;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_WriteAccelerationStructuresPropertiesKHR(
   VkDevice _device, uint32_t accelerationStructureCount,
   const VkAccelerationStructureKHR *pAccelerationStructures, VkQueryType queryType,
   size_t dataSize, void *pData, size_t stride)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_BuildAccelerationStructuresKHR(
   VkDevice _device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyAccelerationStructureKHR(VkDevice _device, VkDeferredOperationKHR deferredOperation,
                                  const VkCopyAccelerationStructureInfoKHR *pInfo)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static nir_builder
create_accel_build_shader(struct radv_device *device, const char *name)
{
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_COMPUTE, "%s", name);
   b.shader->info.workgroup_size[0] = 64;

   assert(b.shader->info.workgroup_size[1] == 1);
   assert(b.shader->info.workgroup_size[2] == 1);
   assert(!b.shader->info.workgroup_size_variable);

   return b;
}

void
radv_device_finish_accel_struct_build_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.copy_pipeline,
                        &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device),
                        state->accel_struct_build.lbvh_internal_pipeline, &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.leaf_pipeline,
                        &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device),
                        state->accel_struct_build.convert_leaf_pipeline, &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device),
                        state->accel_struct_build.convert_internal_pipeline, &state->alloc);
   radv_DestroyPipeline(radv_device_to_handle(device), state->accel_struct_build.morton_pipeline,
                        &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.copy_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.lbvh_internal_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.leaf_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.convert_leaf_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.convert_internal_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device),
                              state->accel_struct_build.morton_p_layout, &state->alloc);

   if (state->accel_struct_build.radix_sort)
      radix_sort_vk_destroy(state->accel_struct_build.radix_sort, radv_device_to_handle(device),
                            &state->alloc);
}

static VkResult
create_build_pipeline_spv(struct radv_device *device, const uint32_t *spv, uint32_t spv_size,
                          unsigned push_constant_size, VkPipeline *pipeline,
                          VkPipelineLayout *layout)
{
   const VkPipelineLayoutCreateInfo pl_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 0,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges =
         &(VkPushConstantRange){VK_SHADER_STAGE_COMPUTE_BIT, 0, push_constant_size},
   };

   VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .codeSize = spv_size,
      .pCode = spv,
   };

   VkShaderModule module;
   VkResult result = device->vk.dispatch_table.CreateShaderModule(
      radv_device_to_handle(device), &module_info, &device->meta_state.alloc, &module);
   if (result != VK_SUCCESS)
      return result;

   result = radv_CreatePipelineLayout(radv_device_to_handle(device), &pl_create_info,
                                      &device->meta_state.alloc, layout);
   if (result != VK_SUCCESS)
      goto cleanup;

   VkPipelineShaderStageCreateInfo shader_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = module,
      .pName = "main",
      .pSpecializationInfo = NULL,
   };

   VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = shader_stage,
      .flags = 0,
      .layout = *layout,
   };

   result = radv_CreateComputePipelines(radv_device_to_handle(device),
                                        radv_pipeline_cache_to_handle(&device->meta_state.cache), 1,
                                        &pipeline_info, &device->meta_state.alloc, pipeline);

cleanup:
   device->vk.dispatch_table.DestroyShaderModule(radv_device_to_handle(device), module,
                                                 &device->meta_state.alloc);
   return result;
}

static void
radix_sort_fill_buffer(VkCommandBuffer commandBuffer,
                       radix_sort_vk_buffer_info_t const *buffer_info, VkDeviceSize offset,
                       VkDeviceSize size, uint32_t data)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(size != VK_WHOLE_SIZE);

   radv_fill_buffer(cmd_buffer, NULL, NULL, buffer_info->devaddr + buffer_info->offset + offset,
                    size, data);
}

VkResult
radv_device_init_accel_struct_build_state(struct radv_device *device)
{
   VkResult result;

   result = create_build_pipeline_spv(device, leaf_spv, sizeof(leaf_spv), sizeof(struct leaf_args),
                                      &device->meta_state.accel_struct_build.leaf_pipeline,
                                      &device->meta_state.accel_struct_build.leaf_p_layout);
   if (result != VK_SUCCESS)
      return result;

   result = create_build_pipeline_spv(
      device, lbvh_internal_spv, sizeof(lbvh_internal_spv), sizeof(struct lbvh_internal_args),
      &device->meta_state.accel_struct_build.lbvh_internal_pipeline,
      &device->meta_state.accel_struct_build.lbvh_internal_p_layout);
   if (result != VK_SUCCESS)
      return result;

   result = create_build_pipeline_spv(device, convert_leaf_spv, sizeof(convert_leaf_spv),
                                      sizeof(struct convert_leaf_args),
                                      &device->meta_state.accel_struct_build.convert_leaf_pipeline,
                                      &device->meta_state.accel_struct_build.convert_leaf_p_layout);
   if (result != VK_SUCCESS)
      return result;

   result =
      create_build_pipeline_spv(device, convert_internal_spv, sizeof(convert_internal_spv),
                                sizeof(struct convert_internal_args),
                                &device->meta_state.accel_struct_build.convert_internal_pipeline,
                                &device->meta_state.accel_struct_build.convert_internal_p_layout);
   if (result != VK_SUCCESS)
      return result;

   result = create_build_pipeline_spv(device, copy_spv, sizeof(copy_spv), sizeof(struct copy_args),
                                      &device->meta_state.accel_struct_build.copy_pipeline,
                                      &device->meta_state.accel_struct_build.copy_p_layout);

   if (result != VK_SUCCESS)
      return result;

   result =
      create_build_pipeline_spv(device, morton_spv, sizeof(morton_spv), sizeof(struct morton_args),
                                &device->meta_state.accel_struct_build.morton_pipeline,
                                &device->meta_state.accel_struct_build.morton_p_layout);
   if (result != VK_SUCCESS)
      return result;

   device->meta_state.accel_struct_build.radix_sort =
      radv_create_radix_sort_u64(radv_device_to_handle(device), &device->meta_state.alloc,
                                 radv_pipeline_cache_to_handle(&device->meta_state.cache));

   struct radix_sort_vk_sort_devaddr_info *radix_sort_info =
      &device->meta_state.accel_struct_build.radix_sort_info;
   radix_sort_info->ext = NULL;
   radix_sort_info->key_bits = 24;
   radix_sort_info->fill_buffer = radix_sort_fill_buffer;

   return result;
}

struct bvh_state {
   uint32_t node_offset;
   uint32_t node_count;
   uint32_t scratch_offset;

   uint32_t leaf_node_count;
   uint32_t internal_node_count;
   uint32_t leaf_node_size;

   struct acceleration_structure_layout accel_struct;
   struct scratch_layout scratch;
};

static void
build_leaves(VkCommandBuffer commandBuffer, uint32_t infoCount,
             const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
             const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos,
             struct bvh_state *bvh_states, enum radv_cmd_flush_bits flush_bits)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.leaf_pipeline);
   for (uint32_t i = 0; i < infoCount; ++i) {
      struct leaf_args leaf_consts = {
         .bvh = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.ir_offset,
         .bounds = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.bounds_offset,
         .ids = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.sort_buffer_offset[0],
         .dst_offset = 0,
      };

      for (unsigned j = 0; j < pInfos[i].geometryCount; ++j) {
         const VkAccelerationStructureGeometryKHR *geom =
            pInfos[i].pGeometries ? &pInfos[i].pGeometries[j] : pInfos[i].ppGeometries[j];

         const VkAccelerationStructureBuildRangeInfoKHR *buildRangeInfo = &ppBuildRangeInfos[i][j];

         leaf_consts.first_id = bvh_states[i].node_count;

         leaf_consts.geometry_type = geom->geometryType;
         leaf_consts.geometry_id = j | (geom->flags << 28);
         unsigned prim_size;
         switch (geom->geometryType) {
         case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
            assert(pInfos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

            leaf_consts.data = geom->geometry.triangles.vertexData.deviceAddress +
                               buildRangeInfo->firstVertex * geom->geometry.triangles.vertexStride;
            leaf_consts.indices = geom->geometry.triangles.indexData.deviceAddress;

            if (geom->geometry.triangles.indexType == VK_INDEX_TYPE_NONE_KHR)
               leaf_consts.data += buildRangeInfo->primitiveOffset;
            else
               leaf_consts.indices += buildRangeInfo->primitiveOffset;

            leaf_consts.transform = geom->geometry.triangles.transformData.deviceAddress;
            if (leaf_consts.transform)
               leaf_consts.transform += buildRangeInfo->transformOffset;

            leaf_consts.stride = geom->geometry.triangles.vertexStride;
            leaf_consts.vertex_format = geom->geometry.triangles.vertexFormat;
            leaf_consts.index_format = geom->geometry.triangles.indexType;

            prim_size = sizeof(struct radv_ir_triangle_node);
            break;
         case VK_GEOMETRY_TYPE_AABBS_KHR:
            assert(pInfos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

            leaf_consts.data =
               geom->geometry.aabbs.data.deviceAddress + buildRangeInfo->primitiveOffset;
            leaf_consts.stride = geom->geometry.aabbs.stride;

            prim_size = sizeof(struct radv_ir_aabb_node);
            break;
         case VK_GEOMETRY_TYPE_INSTANCES_KHR:
            assert(pInfos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);

            leaf_consts.data =
               geom->geometry.instances.data.deviceAddress + buildRangeInfo->primitiveOffset;

            if (geom->geometry.instances.arrayOfPointers)
               leaf_consts.stride = 8;
            else
               leaf_consts.stride = sizeof(VkAccelerationStructureInstanceKHR);

            prim_size = sizeof(struct radv_ir_instance_node);
            break;
         default:
            unreachable("Unknown geometryType");
         }

         radv_CmdPushConstants(commandBuffer,
                               cmd_buffer->device->meta_state.accel_struct_build.leaf_p_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(leaf_consts), &leaf_consts);
         radv_unaligned_dispatch(cmd_buffer, buildRangeInfo->primitiveCount, 1, 1);

         leaf_consts.dst_offset += prim_size * buildRangeInfo->primitiveCount;

         bvh_states[i].leaf_node_count += buildRangeInfo->primitiveCount;
         bvh_states[i].node_count += buildRangeInfo->primitiveCount;
      }
      bvh_states[i].node_offset = leaf_consts.dst_offset;
   }

   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
morton_generate(VkCommandBuffer commandBuffer, uint32_t infoCount,
                const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                struct bvh_state *bvh_states, enum radv_cmd_flush_bits flush_bits)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.morton_pipeline);

   for (uint32_t i = 0; i < infoCount; ++i) {
      const struct morton_args consts = {
         .bvh = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.ir_offset,
         .bounds = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.bounds_offset,
         .ids = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.sort_buffer_offset[0],
      };

      radv_CmdPushConstants(commandBuffer,
                            cmd_buffer->device->meta_state.accel_struct_build.morton_p_layout,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);
      radv_unaligned_dispatch(cmd_buffer, bvh_states[i].node_count, 1, 1);
   }

   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
morton_sort(VkCommandBuffer commandBuffer, uint32_t infoCount,
            const VkAccelerationStructureBuildGeometryInfoKHR *pInfos, struct bvh_state *bvh_states,
            enum radv_cmd_flush_bits flush_bits)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   for (uint32_t i = 0; i < infoCount; ++i) {
      struct radix_sort_vk_memory_requirements requirements;
      radix_sort_vk_get_memory_requirements(
         cmd_buffer->device->meta_state.accel_struct_build.radix_sort, bvh_states[i].node_count,
         &requirements);

      struct radix_sort_vk_sort_devaddr_info info =
         cmd_buffer->device->meta_state.accel_struct_build.radix_sort_info;
      info.count = bvh_states[i].node_count;

      info.keyvals_even.buffer = VK_NULL_HANDLE;
      info.keyvals_even.offset = 0;
      info.keyvals_even.devaddr =
         pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.sort_buffer_offset[0];

      info.keyvals_odd =
         pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.sort_buffer_offset[1];

      info.internal.buffer = VK_NULL_HANDLE;
      info.internal.offset = 0;
      info.internal.devaddr =
         pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.sort_internal_offset;

      VkDeviceAddress result_addr;
      radix_sort_vk_sort_devaddr(cmd_buffer->device->meta_state.accel_struct_build.radix_sort,
                                 &info, radv_device_to_handle(cmd_buffer->device), commandBuffer,
                                 &result_addr);

      assert(result_addr == info.keyvals_even.devaddr || result_addr == info.keyvals_odd);

      bvh_states[i].scratch_offset = (uint32_t)(result_addr - pInfos[i].scratchData.deviceAddress);
   }

   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
lbvh_build_internal(VkCommandBuffer commandBuffer, uint32_t infoCount,
                    const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                    struct bvh_state *bvh_states, enum radv_cmd_flush_bits flush_bits)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.lbvh_internal_pipeline);
   bool progress = true;
   for (unsigned iter = 0; progress; ++iter) {
      progress = false;
      for (uint32_t i = 0; i < infoCount; ++i) {
         if (iter && bvh_states[i].node_count == 1)
            continue;

         if (!progress)
            cmd_buffer->state.flush_bits |= flush_bits;

         progress = true;

         uint32_t dst_node_count = MAX2(1, DIV_ROUND_UP(bvh_states[i].node_count, 2));
         bool final_iter = dst_node_count == 1;

         uint32_t src_scratch_offset = bvh_states[i].scratch_offset;
         uint32_t dst_scratch_offset =
            (src_scratch_offset == bvh_states[i].scratch.sort_buffer_offset[0])
               ? bvh_states[i].scratch.sort_buffer_offset[1]
               : bvh_states[i].scratch.sort_buffer_offset[0];

         uint32_t dst_node_offset = bvh_states[i].node_offset;

         /* Make sure we build the BVH so the hardcoded root node is valid. */
         STATIC_ASSERT(RADV_BVH_ROOT_NODE ==
                       DIV_ROUND_UP(sizeof(struct radv_accel_struct_header), 64) * 8 +
                          radv_bvh_node_internal);

         const struct lbvh_internal_args consts = {
            .bvh = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.ir_offset,
            .src_ids = pInfos[i].scratchData.deviceAddress + src_scratch_offset,
            .dst_ids = pInfos[i].scratchData.deviceAddress + dst_scratch_offset,
            .dst_offset = dst_node_offset,
            .is_root_and_count = bvh_states[i].node_count | (final_iter ? 0x80000000U : 0),
         };

         radv_CmdPushConstants(
            commandBuffer, cmd_buffer->device->meta_state.accel_struct_build.lbvh_internal_p_layout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);
         radv_unaligned_dispatch(cmd_buffer, dst_node_count, 1, 1);
         bvh_states[i].node_offset += dst_node_count * sizeof(struct radv_ir_box_node);
         bvh_states[i].node_count = dst_node_count;
         bvh_states[i].internal_node_count += dst_node_count;
         bvh_states[i].scratch_offset = dst_scratch_offset;
      }
   }
   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
convert_leaf_nodes(VkCommandBuffer commandBuffer, uint32_t infoCount,
                   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                   struct bvh_state *bvh_states)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.convert_leaf_pipeline);
   for (uint32_t i = 0; i < infoCount; ++i) {
      if (!pInfos[i].geometryCount)
         continue;

      RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                       pInfos[i].dstAccelerationStructure);

      const struct convert_leaf_args args = {
         .intermediate_bvh = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.ir_offset,
         .output_bvh = accel_struct->va,
         .geometry_type = pInfos->pGeometries ? pInfos->pGeometries[0].geometryType
                                              : pInfos->ppGeometries[0]->geometryType,
      };
      radv_CmdPushConstants(commandBuffer,
                            cmd_buffer->device->meta_state.accel_struct_build.convert_leaf_p_layout,
                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
      radv_unaligned_dispatch(cmd_buffer, bvh_states[i].leaf_node_count, 1, 1);
   }
   /* This is the final access to the leaf nodes, no need to flush */
}

static void
convert_internal_nodes(VkCommandBuffer commandBuffer, uint32_t infoCount,
                       const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
                       struct bvh_state *bvh_states)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_CmdBindPipeline(
      commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
      cmd_buffer->device->meta_state.accel_struct_build.convert_internal_pipeline);
   for (uint32_t i = 0; i < infoCount; ++i) {
      RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                       pInfos[i].dstAccelerationStructure);

      VkGeometryTypeKHR geometry_type = VK_GEOMETRY_TYPE_TRIANGLES_KHR;

      /* If the geometry count is 0, then the size does not matter
       * because it will be multiplied with 0.
       */
      if (pInfos[i].geometryCount)
         geometry_type = pInfos->pGeometries ? pInfos->pGeometries[0].geometryType
                                             : pInfos->ppGeometries[0]->geometryType;

      const struct convert_internal_args args = {
         .intermediate_bvh = pInfos[i].scratchData.deviceAddress + bvh_states[i].scratch.ir_offset,
         .output_bvh = accel_struct->va,
         .leaf_node_count = bvh_states[i].leaf_node_count,
         .internal_node_count = bvh_states[i].internal_node_count,
         .geometry_type = geometry_type,
      };
      radv_CmdPushConstants(
         commandBuffer, cmd_buffer->device->meta_state.accel_struct_build.convert_internal_p_layout,
         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
      radv_unaligned_dispatch(cmd_buffer, bvh_states[i].internal_node_count, 1, 1);
   }
   /* This is the final access to the leaf nodes, no need to flush */
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBuildAccelerationStructuresKHR(
   VkCommandBuffer commandBuffer, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_meta_saved_state saved_state;

   enum radv_cmd_flush_bits flush_bits =
      RADV_CMD_FLAG_CS_PARTIAL_FLUSH |
      radv_src_access_flush(cmd_buffer, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                            NULL) |
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                            NULL);

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);
   struct bvh_state *bvh_states = calloc(infoCount, sizeof(struct bvh_state));

   for (uint32_t i = 0; i < infoCount; ++i) {
      /* Clear the bvh bounds with int max/min. */
      si_cp_dma_clear_buffer(cmd_buffer, pInfos[i].scratchData.deviceAddress, 3 * sizeof(float),
                             0x7fffffff);
      si_cp_dma_clear_buffer(cmd_buffer, pInfos[i].scratchData.deviceAddress + 3 * sizeof(float),
                             3 * sizeof(float), 0x80000000);
   }

   cmd_buffer->state.flush_bits |= flush_bits;

   for (uint32_t i = 0; i < infoCount; ++i) {
      uint32_t leaf_node_count = 0;
      for (uint32_t j = 0; j < pInfos[i].geometryCount; ++j) {
         leaf_node_count += ppBuildRangeInfos[i][j].primitiveCount;
      }

      get_build_layout(cmd_buffer->device, leaf_node_count, pInfos + i, &bvh_states[i].accel_struct,
                       &bvh_states[i].scratch);
   }

   build_leaves(commandBuffer, infoCount, pInfos, ppBuildRangeInfos, bvh_states, flush_bits);

   morton_generate(commandBuffer, infoCount, pInfos, bvh_states, flush_bits);

   morton_sort(commandBuffer, infoCount, pInfos, bvh_states, flush_bits);

   lbvh_build_internal(commandBuffer, infoCount, pInfos, bvh_states, flush_bits);

   convert_leaf_nodes(commandBuffer, infoCount, pInfos, bvh_states);

   convert_internal_nodes(commandBuffer, infoCount, pInfos, bvh_states);

   for (uint32_t i = 0; i < infoCount; ++i) {
      RADV_FROM_HANDLE(radv_acceleration_structure, accel_struct,
                       pInfos[i].dstAccelerationStructure);
      const size_t base = offsetof(struct radv_accel_struct_header, compacted_size);
      struct radv_accel_struct_header header;

      bool is_tlas = pInfos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

      uint64_t geometry_infos_size =
         pInfos[i].geometryCount * sizeof(struct radv_accel_struct_geometry_info);

      header.instance_offset =
         align(sizeof(struct radv_accel_struct_header), 64) + sizeof(struct radv_bvh_box32_node);
      header.instance_count = is_tlas ? bvh_states[i].leaf_node_count : 0;
      header.compacted_size = bvh_states[i].accel_struct.size;

      header.copy_dispatch_size[0] = DIV_ROUND_UP(header.compacted_size, 16 * 64);
      header.copy_dispatch_size[1] = 1;
      header.copy_dispatch_size[2] = 1;

      header.serialization_size =
         header.compacted_size + align(sizeof(struct radv_accel_struct_serialization_header) +
                                          sizeof(uint64_t) * header.instance_count,
                                       128);

      header.size = header.serialization_size -
                    sizeof(struct radv_accel_struct_serialization_header) -
                    sizeof(uint64_t) * header.instance_count;

      header.build_flags = pInfos[i].flags;
      header.geometry_count = pInfos[i].geometryCount;
      header.internal_node_count = bvh_states[i].internal_node_count;

      struct radv_accel_struct_geometry_info *geometry_infos = malloc(geometry_infos_size);
      if (!geometry_infos)
         goto fail;

      for (uint32_t j = 0; j < pInfos[i].geometryCount; ++j) {
         const VkAccelerationStructureGeometryKHR *geometry =
            pInfos[i].pGeometries ? pInfos[i].pGeometries + j : pInfos[i].ppGeometries[j];
         geometry_infos[j].type = geometry->geometryType;
         geometry_infos[j].flags = geometry->flags;
         geometry_infos[j].primitive_count = ppBuildRangeInfos[i][j].primitiveCount;
      }

      radv_update_buffer_cp(cmd_buffer,
                            radv_buffer_get_va(accel_struct->bo) + accel_struct->mem_offset + base,
                            (const char *)&header + base, sizeof(header) - base);

      VkDeviceSize geometry_infos_offset = header.compacted_size - geometry_infos_size;

      struct radv_buffer accel_struct_buffer;
      radv_buffer_init(&accel_struct_buffer, cmd_buffer->device, accel_struct->bo,
                       accel_struct->size, accel_struct->mem_offset);
      radv_CmdUpdateBuffer(commandBuffer, radv_buffer_to_handle(&accel_struct_buffer),
                           geometry_infos_offset, geometry_infos_size, geometry_infos);
      radv_buffer_finish(&accel_struct_buffer);

      free(geometry_infos);
   }

fail:
   free(bvh_states);
   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyAccelerationStructureKHR(VkCommandBuffer commandBuffer,
                                     const VkCopyAccelerationStructureInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_acceleration_structure, src, pInfo->src);
   RADV_FROM_HANDLE(radv_acceleration_structure, dst, pInfo->dst);
   struct radv_meta_saved_state saved_state;

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.copy_pipeline);

   struct copy_args consts = {
      .src_addr = src->va,
      .dst_addr = dst->va,
      .mode = RADV_COPY_MODE_COPY,
   };

   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                         cmd_buffer->device->meta_state.accel_struct_build.copy_p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);

   cmd_buffer->state.flush_bits |=
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, NULL);

   radv_indirect_dispatch(cmd_buffer, src->bo,
                          src->va + offsetof(struct radv_accel_struct_header, copy_dispatch_size));
   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_GetDeviceAccelerationStructureCompatibilityKHR(
   VkDevice _device, const VkAccelerationStructureVersionInfoKHR *pVersionInfo,
   VkAccelerationStructureCompatibilityKHR *pCompatibility)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   uint8_t zero[VK_UUID_SIZE] = {
      0,
   };
   bool compat =
      memcmp(pVersionInfo->pVersionData, device->physical_device->driver_uuid, VK_UUID_SIZE) == 0 &&
      memcmp(pVersionInfo->pVersionData + VK_UUID_SIZE, zero, VK_UUID_SIZE) == 0;
   *pCompatibility = compat ? VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR
                            : VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyMemoryToAccelerationStructureKHR(VkDevice _device,
                                          VkDeferredOperationKHR deferredOperation,
                                          const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyAccelerationStructureToMemoryKHR(VkDevice _device,
                                          VkDeferredOperationKHR deferredOperation,
                                          const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
{
   unreachable("Unimplemented");
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyMemoryToAccelerationStructureKHR(
   VkCommandBuffer commandBuffer, const VkCopyMemoryToAccelerationStructureInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_acceleration_structure, dst, pInfo->dst);
   struct radv_meta_saved_state saved_state;

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.copy_pipeline);

   const struct copy_args consts = {
      .src_addr = pInfo->src.deviceAddress,
      .dst_addr = dst->va,
      .mode = RADV_COPY_MODE_DESERIALIZE,
   };

   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                         cmd_buffer->device->meta_state.accel_struct_build.copy_p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);

   radv_CmdDispatch(commandBuffer, 512, 1, 1);
   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyAccelerationStructureToMemoryKHR(
   VkCommandBuffer commandBuffer, const VkCopyAccelerationStructureToMemoryInfoKHR *pInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_acceleration_structure, src, pInfo->src);
   struct radv_meta_saved_state saved_state;

   radv_meta_save(
      &saved_state, cmd_buffer,
      RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_CONSTANTS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                        cmd_buffer->device->meta_state.accel_struct_build.copy_pipeline);

   const struct copy_args consts = {
      .src_addr = src->va,
      .dst_addr = pInfo->dst.deviceAddress,
      .mode = RADV_COPY_MODE_SERIALIZE,
   };

   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                         cmd_buffer->device->meta_state.accel_struct_build.copy_p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(consts), &consts);

   cmd_buffer->state.flush_bits |=
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, NULL);

   radv_indirect_dispatch(cmd_buffer, src->bo,
                          src->va + offsetof(struct radv_accel_struct_header, copy_dispatch_size));
   radv_meta_restore(&saved_state, cmd_buffer);

   /* Set the header of the serialized data. */
   uint8_t header_data[2 * VK_UUID_SIZE] = {0};
   memcpy(header_data, cmd_buffer->device->physical_device->driver_uuid, VK_UUID_SIZE);

   radv_update_buffer_cp(cmd_buffer, pInfo->dst.deviceAddress, header_data, sizeof(header_data));
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBuildAccelerationStructuresIndirectKHR(
   VkCommandBuffer commandBuffer, uint32_t infoCount,
   const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
   const VkDeviceAddress *pIndirectDeviceAddresses, const uint32_t *pIndirectStrides,
   const uint32_t *const *ppMaxPrimitiveCounts)
{
   unreachable("Unimplemented");
}
