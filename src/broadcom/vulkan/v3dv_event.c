/*
 * Copyright © 2022 Raspberry Pi Ltd
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

#include "v3dv_private.h"
#include "compiler/nir/nir_builder.h"

static nir_shader *
get_set_event_cs()
{
   const nir_shader_compiler_options *options = v3dv_pipeline_get_nir_options();
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "set event cs");

   b.shader->info.workgroup_size[0] = 1;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_ssa_def *buf =
      nir_vulkan_resource_index(&b, 2, 32, nir_imm_int(&b, 0),
                                .desc_set = 0,
                                .binding = 0,
                                .desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

   nir_ssa_def *offset =
      nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 0, .range = 4);

   nir_ssa_def *value =
      nir_load_push_constant(&b, 1, 8, nir_imm_int(&b, 0), .base = 4, .range = 4);

   nir_store_ssbo(&b, value, buf, offset,
                  .access = 0, .write_mask = 0x1, .align_mul = 4);

   return b.shader;
}

static nir_shader *
get_wait_event_cs()
{
   const nir_shader_compiler_options *options = v3dv_pipeline_get_nir_options();
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "wait event cs");

   b.shader->info.workgroup_size[0] = 1;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_ssa_def *buf =
      nir_vulkan_resource_index(&b, 2, 32, nir_imm_int(&b, 0),
                                .desc_set = 0,
                                .binding = 0,
                                .desc_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

   nir_ssa_def *offset =
      nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 0, .range = 4);

   nir_loop *loop = nir_push_loop(&b);
      nir_ssa_def *load =
         nir_load_ssbo(&b, 1, 8, buf, offset, .access = 0, .align_mul = 4);
      nir_ssa_def *value = nir_i2i32(&b, load);

      nir_if *if_stmt = nir_push_if(&b, nir_ieq_imm(&b, value, 1));
      nir_jump(&b, nir_jump_break);
      nir_pop_if(&b, if_stmt);
   nir_pop_loop(&b, loop);

   return b.shader;
}

static VkResult
create_compute_pipeline_from_nir(struct v3dv_device *device,
                                 nir_shader *nir,
                                 VkPipelineLayout pipeline_layout,
                                 VkPipeline *pipeline)
{
   struct vk_shader_module cs_m = vk_shader_module_from_nir(nir);

   VkPipelineShaderStageCreateInfo set_event_cs_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = vk_shader_module_to_handle(&cs_m),
      .pName = "main",
   };

   VkComputePipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = set_event_cs_stage,
      .layout = pipeline_layout,
   };

   VkResult result =
      v3dv_CreateComputePipelines(v3dv_device_to_handle(device), VK_NULL_HANDLE,
                                  1, &info, &device->vk.alloc, pipeline);

   return result;
}

static bool
create_event_pipelines(struct v3dv_device *device)
{
   VkResult result;

   if (!device->events.descriptor_set_layout) {
      /* Pipeline layout:
       *  - 1 storage buffer for the BO with the events state.
       *  - 2 push constants:
       *    0B: offset of the event in the buffer (4 bytes).
       *    4B: value for the event (1 byte), only used with the set_event_pipeline.
       */
      VkDescriptorSetLayoutBinding descriptor_set_layout_binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };

      VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &descriptor_set_layout_binding,
      };

      result =
         v3dv_CreateDescriptorSetLayout(v3dv_device_to_handle(device),
                                        &descriptor_set_layout_info,
                                        &device->vk.alloc,
                                        &device->events.descriptor_set_layout);

      if (result != VK_SUCCESS)
         return false;
   }

   if (!device->events.pipeline_layout) {
      VkPipelineLayoutCreateInfo pipeline_layout_info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &device->events.descriptor_set_layout,
         .pushConstantRangeCount = 1,
         .pPushConstantRanges =
             &(VkPushConstantRange) { VK_SHADER_STAGE_COMPUTE_BIT, 0, 5 },
      };

      result =
         v3dv_CreatePipelineLayout(v3dv_device_to_handle(device),
                                   &pipeline_layout_info,
                                   &device->vk.alloc,
                                   &device->events.pipeline_layout);

      if (result != VK_SUCCESS)
         return false;
   }

   VkPipeline pipeline;

   if (!device->events.set_event_pipeline) {
      nir_shader *set_event_cs_nir = get_set_event_cs();
      result = create_compute_pipeline_from_nir(device,
                                                set_event_cs_nir,
                                                device->events.pipeline_layout,
                                                &pipeline);
      ralloc_free(set_event_cs_nir);
      if (result != VK_SUCCESS)
         return false;

      device->events.set_event_pipeline = pipeline;
   }

   if (!device->events.wait_event_pipeline) {
      nir_shader *wait_event_cs_nir = get_wait_event_cs();
      result = create_compute_pipeline_from_nir(device,
                                                wait_event_cs_nir,
                                                device->events.pipeline_layout,
                                                &pipeline);
      ralloc_free(wait_event_cs_nir);
      if (result != VK_SUCCESS)
         return false;

      device->events.wait_event_pipeline = pipeline;
   }

   return true;
}

static void
destroy_event_pipelines(struct v3dv_device *device)
{
   VkDevice _device = v3dv_device_to_handle(device);

   v3dv_DestroyPipeline(_device, device->events.set_event_pipeline,
                         &device->vk.alloc);
   device->events.set_event_pipeline = VK_NULL_HANDLE;

   v3dv_DestroyPipeline(_device, device->events.wait_event_pipeline,
                         &device->vk.alloc);
   device->events.wait_event_pipeline = VK_NULL_HANDLE;

   v3dv_DestroyPipelineLayout(_device, device->events.pipeline_layout,
                              &device->vk.alloc);
   device->events.pipeline_layout = VK_NULL_HANDLE;

   v3dv_DestroyDescriptorSetLayout(_device,
                                   device->events.descriptor_set_layout,
                                   &device->vk.alloc);
   device->events.descriptor_set_layout = VK_NULL_HANDLE;
}

bool
v3dv_event_allocate_resources(struct v3dv_device *device)
{
   VkResult result = VK_SUCCESS;
   VkDevice _device = v3dv_device_to_handle(device);

   /* BO with event states. Make sure we always align to a page size (4096)
    * to ensure we use all the memory the kernel will allocate for the BO.
    */
   const uint32_t bo_size = 4096;
   struct v3dv_bo *bo = v3dv_bo_alloc(device, bo_size, "events", true);
   if (!bo) {
      result = vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto fail;
   }

   device->events.bo = bo;

   if (!v3dv_bo_map(device, bo, bo_size)) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail;
   }

   /* List of free event state slots in the BO, 1 byte per slot */
   device->events.desc_count = bo_size;
   device->events.desc =
      vk_alloc2(&device->vk.alloc, NULL,
                device->events.desc_count * sizeof(struct v3dv_event_desc), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device->events.desc) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail;
   }

   list_inithead(&device->events.free_list);
   for (int i = 0; i < device->events.desc_count; i++) {
      device->events.desc[i].index = i;
      list_addtail(&device->events.desc[i].link, &device->events.free_list);
   }

   /* Vulkan buffer for the event state BO */
   VkBufferCreateInfo buf_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bo->size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
   };
   result = v3dv_CreateBuffer(_device, &buf_info, NULL,
                              &device->events.buffer);
   if (result != VK_SUCCESS)
      goto fail;

   struct v3dv_device_memory *mem =
      vk_object_zalloc(&device->vk, NULL, sizeof(*mem),
                       VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (!mem) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   mem->bo = bo;
   mem->type = &device->pdevice->memory.memoryTypes[0];

   device->events.mem = v3dv_device_memory_to_handle(mem);
   VkBindBufferMemoryInfo bind_info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = device->events.buffer,
      .memory = device->events.mem,
      .memoryOffset = 0,
   };
   v3dv_BindBufferMemory2(_device, 1, &bind_info);

   /* Pipelines */
   if (!create_event_pipelines(device)) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   /* Descriptor pool & set to access the buffer */
   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };
   VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   result =
      v3dv_CreateDescriptorPool(_device, &pool_info, NULL,
                                &device->events.descriptor_pool);

   if (result != VK_SUCCESS)
      goto fail;

   VkDescriptorSetAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = device->events.descriptor_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &device->events.descriptor_set_layout,
   };
   result = v3dv_AllocateDescriptorSets(_device, &alloc_info,
                                         &device->events.descriptor_set);
   if (result != VK_SUCCESS)
      goto  fail;

   VkDescriptorBufferInfo desc_buf_info = {
      .buffer = device->events.buffer,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };

   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = device->events.descriptor_set,
      .dstBinding = 0,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &desc_buf_info,
   };
   v3dv_UpdateDescriptorSets(_device, 1, &write, 0, NULL);

   return VK_SUCCESS;

fail:
   v3dv_event_free_resources(device);
   return result;
}

void
v3dv_event_free_resources(struct v3dv_device *device)
{
   if (device->events.bo) {
      v3dv_bo_free(device, device->events.bo);
      device->events.bo = NULL;
   }

   if (device->events.desc) {
      vk_free2(&device->vk.alloc, NULL, device->events.desc);
      device->events.desc = NULL;
   }

   vk_object_free(&device->vk, NULL,
                  v3dv_device_memory_from_handle(device->events.mem));
   device->events.mem = VK_NULL_HANDLE;

   v3dv_DestroyBuffer(v3dv_device_to_handle(device),
                      device->events.buffer, NULL);
   device->events.buffer = VK_NULL_HANDLE;

   v3dv_FreeDescriptorSets(v3dv_device_to_handle(device),
                           device->events.descriptor_pool,
                           1, &device->events.descriptor_set);
   device->events.descriptor_set = VK_NULL_HANDLE;

   v3dv_DestroyDescriptorPool(v3dv_device_to_handle(device),
                              device->events.descriptor_pool,
                              NULL);
   device->events.descriptor_pool = VK_NULL_HANDLE;

   destroy_event_pipelines(device);
}

static struct v3dv_event_desc *
allocate_event_descriptor(struct v3dv_device *device)
{
   mtx_lock(&device->events.lock);
   if (list_is_empty(&device->events.free_list)) {
      mtx_unlock(&device->events.lock);
      return NULL;
   }

   struct v3dv_event_desc *desc =
      list_first_entry(&device->events.free_list, struct v3dv_event_desc, link);
   list_del(&desc->link);
   mtx_unlock(&device->events.lock);

   return desc;
}

static void
free_event_descriptor(struct v3dv_device *device, uint32_t index)
{
   mtx_lock(&device->events.lock);
   assert(index < device->events.desc_count);
   list_addtail(&device->events.desc[index].link, &device->events.free_list);
   mtx_unlock(&device->events.lock);
}

static void
event_set_value(struct v3dv_device *device,
                       struct v3dv_event *event,
                       uint8_t value)
{
   assert(value == 0 || value == 1);
   uint8_t *data = (uint8_t *) device->events.bo->map;
   data[event->index] = value;
}

static uint8_t
event_get_value(struct v3dv_device *device, struct v3dv_event *event)
{
   uint8_t *data = (uint8_t *) device->events.bo->map;
   return data[event->index];
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_CreateEvent(VkDevice _device,
                 const VkEventCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkEvent *pEvent)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   VkResult result = VK_SUCCESS;

   struct v3dv_event *event =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*event),
                       VK_OBJECT_TYPE_EVENT);
   if (!event) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail;
   }

   struct v3dv_event_desc *desc = allocate_event_descriptor(device);
   if (!desc) {
      result = vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto fail;
   }

   event->index = desc->index;
   event_set_value(device, event, 0);
   *pEvent = v3dv_event_to_handle(event);
   return VK_SUCCESS;

fail:
   return result;
}

VKAPI_ATTR void VKAPI_CALL
v3dv_DestroyEvent(VkDevice _device,
                  VkEvent _event,
                  const VkAllocationCallbacks *pAllocator)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_event, event, _event);

   if (!event)
      return;

   free_event_descriptor(device, event->index);
   vk_object_free(&device->vk, pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_GetEventStatus(VkDevice _device, VkEvent _event)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_event, event, _event);
   return event_get_value(device, event) ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_SetEvent(VkDevice _device, VkEvent _event)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_event, event, _event);
   event_set_value(device, event, 1);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
v3dv_ResetEvent(VkDevice _device, VkEvent _event)
{
   V3DV_FROM_HANDLE(v3dv_device, device, _device);
   V3DV_FROM_HANDLE(v3dv_event, event, _event);
   event_set_value(device, event, 0);
   return VK_SUCCESS;
}

static void
cmd_buffer_emit_set_event(struct v3dv_cmd_buffer *cmd_buffer,
                          struct v3dv_event *event,
                          uint8_t value)
{
   assert(value == 0 || value == 1);

   struct v3dv_device *device = cmd_buffer->device;
   VkCommandBuffer commandBuffer = v3dv_cmd_buffer_to_handle(cmd_buffer);

   v3dv_cmd_buffer_meta_state_push(cmd_buffer, true);

   v3dv_CmdBindPipeline(commandBuffer,
                        VK_PIPELINE_BIND_POINT_COMPUTE,
                        device->events.set_event_pipeline);

   v3dv_CmdBindDescriptorSets(commandBuffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE,
                              device->events.pipeline_layout,
                              0, 1, &device->events.descriptor_set, 0, NULL);

   assert(event->index < device->events.desc_count);
   uint32_t offset = event->index;
   v3dv_CmdPushConstants(commandBuffer,
                         device->events.pipeline_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT,
                         0, 4, &offset);

   v3dv_CmdPushConstants(commandBuffer,
                         device->events.pipeline_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT,
                         4, 1, &value);

   v3dv_CmdDispatch(commandBuffer, 1, 1, 1);

   v3dv_cmd_buffer_meta_state_pop(cmd_buffer, 0, false);
}

static void
cmd_buffer_emit_wait_event(struct v3dv_cmd_buffer *cmd_buffer,
                           struct v3dv_event *event)
{
   struct v3dv_device *device = cmd_buffer->device;
   VkCommandBuffer commandBuffer = v3dv_cmd_buffer_to_handle(cmd_buffer);

   v3dv_cmd_buffer_meta_state_push(cmd_buffer, true);

   v3dv_CmdBindPipeline(commandBuffer,
                        VK_PIPELINE_BIND_POINT_COMPUTE,
                        device->events.wait_event_pipeline);

   v3dv_CmdBindDescriptorSets(commandBuffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE,
                              device->events.pipeline_layout,
                              0, 1, &device->events.descriptor_set, 0, NULL);

   assert(event->index < device->events.desc_count);
   uint32_t offset = event->index;
   v3dv_CmdPushConstants(commandBuffer,
                         device->events.pipeline_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT,
                         0, 4, &offset);

   v3dv_CmdDispatch(commandBuffer, 1, 1, 1);

   v3dv_cmd_buffer_meta_state_pop(cmd_buffer, 0, false);
}

VKAPI_ATTR void VKAPI_CALL
v3dv_CmdSetEvent2(VkCommandBuffer commandBuffer,
                  VkEvent _event,
                  const VkDependencyInfo *pDependencyInfo)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_event, event, _event);

   /* Event (re)sets can only happen outside a render pass instance so we
    * should not be in the middle of job recording.
    */
   assert(cmd_buffer->state.pass == NULL);
   assert(cmd_buffer->state.job == NULL);

   v3dv_CmdPipelineBarrier2(commandBuffer, pDependencyInfo);
   cmd_buffer_emit_set_event(cmd_buffer, event, 1);
}

VKAPI_ATTR void VKAPI_CALL
v3dv_CmdResetEvent2(VkCommandBuffer commandBuffer,
                    VkEvent _event,
                    VkPipelineStageFlags2 stageMask)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   V3DV_FROM_HANDLE(v3dv_event, event, _event);

   /* Event (re)sets can only happen outside a render pass instance so we
    * should not be in the middle of job recording.
    */
   assert(cmd_buffer->state.pass == NULL);
   assert(cmd_buffer->state.job == NULL);

   cmd_buffer_emit_set_event(cmd_buffer, event, 0);
}

VKAPI_ATTR void VKAPI_CALL
v3dv_CmdWaitEvents2(VkCommandBuffer commandBuffer,
                    uint32_t eventCount,
                    const VkEvent *pEvents,
                    const VkDependencyInfo *pDependencyInfos)
{
   V3DV_FROM_HANDLE(v3dv_cmd_buffer, cmd_buffer, commandBuffer);
   for (uint32_t i = 0; i < eventCount; i++) {
      struct v3dv_event *event = v3dv_event_from_handle(pEvents[i]);;
      cmd_buffer_emit_wait_event(cmd_buffer, event);
   }
}
