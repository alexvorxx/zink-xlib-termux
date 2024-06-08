/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_physical_device.h"

#include "pan_desc.h"
#include "pan_encoder.h"
#include "pan_jc.h"
#include "pan_pool.h"
#include "pan_props.h"

#include <vulkan/vulkan_core.h>

struct panvk_dispatch_info {
   struct pan_compute_dim wg_count;
   mali_ptr attributes;
   mali_ptr attribute_bufs;
   mali_ptr tsd;
   mali_ptr ubos;
   mali_ptr push_uniforms;
   mali_ptr textures;
   mali_ptr samplers;
};

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatch)(VkCommandBuffer commandBuffer, uint32_t x,
                            uint32_t y, uint32_t z)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct panvk_dispatch_info dispatch = {
      .wg_count = {x, y, z},
   };

   panvk_per_arch(cmd_close_batch)(cmdbuf);
   struct panvk_batch *batch = panvk_per_arch(cmd_open_batch)(cmdbuf);

   struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.compute.desc_state;
   const struct panvk_compute_pipeline *pipeline =
      cmdbuf->state.compute.pipeline;
   struct pan_pool *desc_pool_base = &cmdbuf->desc_pool.base;
   struct panfrost_ptr job =
      pan_pool_alloc_desc(&cmdbuf->desc_pool.base, COMPUTE_JOB);
   util_dynarray_append(&batch->jobs, void *, job.cpu);

   struct panvk_compute_sysvals *sysvals = &cmdbuf->state.compute.sysvals;
   sysvals->num_work_groups.x = x;
   sysvals->num_work_groups.y = y;
   sysvals->num_work_groups.z = z;
   sysvals->local_group_size.x = pipeline->local_size.x;
   sysvals->local_group_size.y = pipeline->local_size.y;
   sysvals->local_group_size.z = pipeline->local_size.z;

   panvk_per_arch(cmd_alloc_tls_desc)(cmdbuf, false);
   dispatch.tsd = batch->tls.gpu;

   panvk_per_arch(cmd_prepare_push_sets)(desc_pool_base, desc_state,
                                         &pipeline->base);

   if (pipeline->cs.has_img_access)
      panvk_per_arch(prepare_img_attribs)(desc_pool_base, desc_state,
                                          &pipeline->base);

   dispatch.attributes = desc_state->img.attribs;
   dispatch.attribute_bufs = desc_state->img.attrib_bufs;

   panvk_per_arch(cmd_prepare_ubos)(desc_pool_base, desc_state,
                                    &pipeline->base);
   dispatch.ubos = desc_state->ubos;

   if (!cmdbuf->state.compute.push_uniforms) {
      cmdbuf->state.compute.push_uniforms = panvk_cmd_prepare_push_uniforms(
         desc_pool_base, &cmdbuf->state.push_constants,
         &cmdbuf->state.compute.sysvals, sizeof(cmdbuf->state.compute.sysvals));
   }
   dispatch.push_uniforms = cmdbuf->state.compute.push_uniforms;

   panvk_per_arch(cmd_prepare_textures)(desc_pool_base, desc_state,
                                        &pipeline->base);
   dispatch.textures = desc_state->textures;

   panvk_per_arch(cmd_prepare_samplers)(desc_pool_base, desc_state,
                                        &pipeline->base);
   dispatch.samplers = desc_state->samplers;

   panfrost_pack_work_groups_compute(
      pan_section_ptr(job.cpu, COMPUTE_JOB, INVOCATION), dispatch.wg_count.x,
      dispatch.wg_count.y, dispatch.wg_count.z, pipeline->local_size.x,
      pipeline->local_size.y, pipeline->local_size.z, false, false);

   pan_section_pack(job.cpu, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = util_logbase2_ceil(pipeline->local_size.x + 1) +
                           util_logbase2_ceil(pipeline->local_size.y + 1) +
                           util_logbase2_ceil(pipeline->local_size.z + 1);
   }

   pan_section_pack(job.cpu, COMPUTE_JOB, DRAW, cfg) {
      cfg.state = pipeline->cs.rsd;
      cfg.attributes = dispatch.attributes;
      cfg.attribute_buffers = dispatch.attribute_bufs;
      cfg.thread_storage = dispatch.tsd;
      cfg.uniform_buffers = dispatch.ubos;
      cfg.push_uniforms = dispatch.push_uniforms;
      cfg.textures = dispatch.textures;
      cfg.samplers = dispatch.samplers;
   }

   pan_jc_add_job(&batch->jc, MALI_JOB_TYPE_COMPUTE, false, false, 0, 0, &job,
                  false);

   batch->tlsinfo.tls.size = pipeline->cs.info.tls_size;
   batch->tlsinfo.wls.size = pipeline->cs.info.wls_size;
   if (batch->tlsinfo.wls.size) {
      unsigned core_id_range;

      panfrost_query_core_count(&phys_dev->kmod.props, &core_id_range);
      batch->tlsinfo.wls.instances = pan_wls_instances(&dispatch.wg_count);
      batch->wls_total_size = pan_wls_adjust_size(batch->tlsinfo.wls.size) *
                              batch->tlsinfo.wls.instances * core_id_range;
   }

   panvk_per_arch(cmd_close_batch)(cmdbuf);
   panvk_per_arch(cmd_unprepare_push_sets)(desc_state);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatchBase)(VkCommandBuffer commandBuffer, uint32_t base_x,
                                uint32_t base_y, uint32_t base_z, uint32_t x,
                                uint32_t y, uint32_t z)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatchIndirect)(VkCommandBuffer commandBuffer,
                                    VkBuffer _buffer, VkDeviceSize offset)
{
   panvk_stub();
}
