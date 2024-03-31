/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "zink_context.h"
#include "zink_framebuffer.h"
#include "zink_kopper.h"
#include "zink_query.h"
#include "zink_render_pass.h"
#include "zink_resource.h"
#include "zink_screen.h"

#include "util/u_memory.h"
#include "util/u_string.h"
#include "util/u_blitter.h"

static VkRenderPass
create_render_pass(struct zink_screen *screen, struct zink_render_pass_state *state, struct zink_render_pass_pipeline_state *pstate)
{

   VkAttachmentReference color_refs[PIPE_MAX_COLOR_BUFS], zs_ref;
   VkAttachmentReference input_attachments[PIPE_MAX_COLOR_BUFS];
   VkAttachmentDescription attachments[PIPE_MAX_COLOR_BUFS + 1];
   VkPipelineStageFlags dep_pipeline = 0;
   VkAccessFlags dep_access = 0;
   unsigned input_count = 0;

   pstate->num_attachments = state->num_cbufs;
   for (int i = 0; i < state->num_cbufs; i++) {
      struct zink_rt_attrib *rt = state->rts + i;
      attachments[i].flags = 0;
      pstate->attachments[i].format = attachments[i].format = rt->format;
      pstate->attachments[i].samples = attachments[i].samples = rt->samples;
      attachments[i].loadOp = rt->clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                state->swapchain_init && rt->swapchain ?
                                                VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      /* if layout changes are ever handled here, need VkAttachmentSampleLocationsEXT */
      VkImageLayout layout = rt->fbfetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachments[i].initialLayout = layout;
      attachments[i].finalLayout = layout;
      color_refs[i].attachment = i;
      color_refs[i].layout = layout;
      dep_pipeline |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      if (rt->fbfetch) {
         memcpy(&input_attachments[input_count++], &color_refs[i], sizeof(VkAttachmentReference));
         dep_pipeline |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
         dep_access |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
      }
      dep_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      if (attachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
         dep_access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
   }

   int num_attachments = state->num_cbufs;
   if (state->have_zsbuf)  {
      struct zink_rt_attrib *rt = state->rts + state->num_cbufs;
      bool has_clear = rt->clear_color || rt->clear_stencil;
      VkImageLayout layout;
      if (rt->mixed_zs)
         layout = VK_IMAGE_LAYOUT_GENERAL;
      else
         layout = rt->needs_write || has_clear ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      attachments[num_attachments].flags = 0;
      pstate->attachments[num_attachments].format = attachments[num_attachments].format = rt->format;
      pstate->attachments[num_attachments].samples = attachments[num_attachments].samples = rt->samples;
      attachments[num_attachments].loadOp = rt->clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      if (rt->mixed_zs)
         attachments[num_attachments].storeOp = VK_ATTACHMENT_STORE_OP_NONE;
      else
         attachments[num_attachments].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[num_attachments].stencilLoadOp = rt->clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[num_attachments].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
      /* if layout changes are ever handled here, need VkAttachmentSampleLocationsEXT */
      attachments[num_attachments].initialLayout = layout;
      attachments[num_attachments].finalLayout = layout;

      dep_pipeline |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
         dep_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      if (attachments[num_attachments].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
          attachments[num_attachments].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
         dep_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

      zs_ref.attachment = num_attachments++;
      zs_ref.layout = layout;
      pstate->num_attachments++;
   }

   if (!screen->info.have_KHR_synchronization2)
      dep_pipeline = MAX2(dep_pipeline, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
   VkSubpassDependency deps[] = {
      {VK_SUBPASS_EXTERNAL, 0, dep_pipeline, dep_pipeline, 0, dep_access, VK_DEPENDENCY_BY_REGION_BIT},
      {0, VK_SUBPASS_EXTERNAL, dep_pipeline, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dep_access, 0, VK_DEPENDENCY_BY_REGION_BIT}
   };
   VkPipelineStageFlags input_dep = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
   //if (zs_fbfetch) input_dep |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
   VkAccessFlags input_access = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
   //if (zs_fbfetch) input_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
   VkSubpassDependency fbfetch_deps[] = {
      {VK_SUBPASS_EXTERNAL, 0, dep_pipeline, dep_pipeline, 0, dep_access, VK_DEPENDENCY_BY_REGION_BIT},
      {0, 0, dep_pipeline, input_dep, dep_access, input_access, VK_DEPENDENCY_BY_REGION_BIT},
      {0, VK_SUBPASS_EXTERNAL, dep_pipeline, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dep_access, 0, VK_DEPENDENCY_BY_REGION_BIT}
   };

   VkSubpassDescription subpass = {0};
   subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = state->num_cbufs;
   subpass.pColorAttachments = color_refs;
   subpass.pDepthStencilAttachment = state->have_zsbuf ? &zs_ref : NULL;
   subpass.inputAttachmentCount = input_count;
   subpass.pInputAttachments = input_attachments;

   VkRenderPassCreateInfo rpci = {0};
   rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   rpci.attachmentCount = num_attachments;
   rpci.pAttachments = attachments;
   rpci.subpassCount = 1;
   rpci.pSubpasses = &subpass;
   rpci.dependencyCount = input_count ? 3 : 2;
   rpci.pDependencies = input_count ? fbfetch_deps : deps;

   VkRenderPass render_pass;
   if (VKSCR(CreateRenderPass)(screen->dev, &rpci, NULL, &render_pass) != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateRenderPass failed");
      return VK_NULL_HANDLE;
   }

   return render_pass;
}

static VkRenderPass
create_render_pass2(struct zink_screen *screen, struct zink_render_pass_state *state, struct zink_render_pass_pipeline_state *pstate)
{

   VkAttachmentReference2 color_refs[PIPE_MAX_COLOR_BUFS], color_resolves[PIPE_MAX_COLOR_BUFS], zs_ref, zs_resolve;
   VkAttachmentReference2 input_attachments[PIPE_MAX_COLOR_BUFS];
   VkAttachmentDescription2 attachments[2 * (PIPE_MAX_COLOR_BUFS + 1)];
   VkPipelineStageFlags dep_pipeline = 0;
   VkAccessFlags dep_access = 0;
   unsigned input_count = 0;
   const unsigned cresolve_offset = state->num_cbufs + state->have_zsbuf;
   const unsigned zsresolve_offset = cresolve_offset + state->num_cresolves;

   pstate->num_attachments = state->num_cbufs;
   pstate->num_cresolves = state->num_cresolves;
   pstate->num_zsresolves = state->num_zsresolves;
   for (int i = 0; i < state->num_cbufs; i++) {
      struct zink_rt_attrib *rt = state->rts + i;
      attachments[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
      attachments[i].pNext = NULL;
      attachments[i].flags = 0;
      pstate->attachments[i].format = attachments[i].format = rt->format;
      pstate->attachments[i].samples = attachments[i].samples = rt->samples;
      attachments[i].loadOp = rt->clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                /* TODO: need replicate EXT */
                                                //rt->resolve || rt->invalid ?
                                                rt->invalid ?
                                                VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                VK_ATTACHMENT_LOAD_OP_LOAD;

      /* TODO: need replicate EXT */
      //attachments[i].storeOp = rt->resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      /* if layout changes are ever handled here, need VkAttachmentSampleLocationsEXT */
      VkImageLayout layout = rt->fbfetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachments[i].initialLayout = layout;
      attachments[i].finalLayout = layout;
      color_refs[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
      color_refs[i].pNext = NULL;
      color_refs[i].attachment = i;
      color_refs[i].layout = layout;
      color_refs[i].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      dep_pipeline |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      if (rt->fbfetch) {
         memcpy(&input_attachments[input_count++], &color_refs[i], sizeof(VkAttachmentReference2));
         dep_pipeline |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
         dep_access |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
         pstate->fbfetch = 1;
      }
      dep_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      if (attachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
         dep_access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

      if (rt->resolve) {
         memcpy(&attachments[cresolve_offset + i], &attachments[i], sizeof(VkAttachmentDescription2));
         attachments[cresolve_offset + i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
         attachments[cresolve_offset + i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
         attachments[cresolve_offset + i].samples = 1;
         memcpy(&color_resolves[i], &color_refs[i], sizeof(VkAttachmentReference2));
         color_resolves[i].attachment = cresolve_offset + i;
         if (attachments[cresolve_offset + i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
            dep_access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
      }
   }

   int num_attachments = state->num_cbufs;
   if (state->have_zsbuf)  {
      struct zink_rt_attrib *rt = state->rts + state->num_cbufs;
      bool has_clear = rt->clear_color || rt->clear_stencil;
      VkImageLayout layout;
      if (rt->mixed_zs)
         layout = VK_IMAGE_LAYOUT_GENERAL;
      else
         layout = rt->needs_write || has_clear ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      attachments[num_attachments].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
      attachments[num_attachments].pNext = NULL;
      attachments[num_attachments].flags = 0;
      pstate->attachments[num_attachments].format = attachments[num_attachments].format = rt->format;
      pstate->attachments[num_attachments].samples = attachments[num_attachments].samples = rt->samples;
      attachments[num_attachments].loadOp = rt->clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      attachments[num_attachments].stencilLoadOp = rt->clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      /* TODO: need replicate EXT */
      //attachments[num_attachments].storeOp = rt->resolve ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
      //attachments[num_attachments].stencilStoreOp = rt->resolve ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
      if (rt->mixed_zs)
         attachments[num_attachments].storeOp = VK_ATTACHMENT_STORE_OP_NONE;
      else
         attachments[num_attachments].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[num_attachments].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
      /* if layout changes are ever handled here, need VkAttachmentSampleLocationsEXT */
      attachments[num_attachments].initialLayout = layout;
      attachments[num_attachments].finalLayout = layout;

      dep_pipeline |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      if (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
         dep_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      if (attachments[num_attachments].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
          attachments[num_attachments].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
         dep_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

      zs_ref.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
      zs_ref.pNext = NULL;
      zs_ref.attachment = num_attachments++;
      zs_ref.layout = layout;
      if (rt->resolve) {
         memcpy(&attachments[zsresolve_offset], &attachments[num_attachments], sizeof(VkAttachmentDescription2));
         attachments[zsresolve_offset].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
         attachments[zsresolve_offset].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
         attachments[zsresolve_offset].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
         attachments[zsresolve_offset].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
         attachments[zsresolve_offset].samples = 1;
         memcpy(&zs_resolve, &zs_ref, sizeof(VkAttachmentReference2));
         zs_ref.attachment = zsresolve_offset;
         if (attachments[zsresolve_offset].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
             attachments[zsresolve_offset].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
            dep_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
      }
      pstate->num_attachments++;
   }
   if (dep_access & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
      pstate->color_read = true;
   if (dep_access & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
      pstate->depth_read = true;
   if (dep_access & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
      pstate->depth_write = true;

   if (!screen->info.have_KHR_synchronization2)
      dep_pipeline = MAX2(dep_pipeline, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

   //VkDependencyFlags flag = screen->info.have_KHR_synchronization2 ? VK_DEPENDENCY_BY_REGION_BIT : 0;
   VkSubpassDependency2 deps[] = {
      /*{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, VK_SUBPASS_EXTERNAL, 0, dep_pipeline, dep_pipeline, 0, dep_access, flag, 0},
      {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, VK_SUBPASS_EXTERNAL, dep_pipeline, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dep_access, 0, flag, 0}*/
	  {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, VK_SUBPASS_EXTERNAL, 0, dep_pipeline, dep_pipeline, 0, dep_access, VK_DEPENDENCY_BY_REGION_BIT, 0},
      {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, VK_SUBPASS_EXTERNAL, dep_pipeline, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dep_access, 0, VK_DEPENDENCY_BY_REGION_BIT, 0}
   };
   VkPipelineStageFlags input_dep = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
   //if (zs_fbfetch) input_dep |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
   VkAccessFlags input_access = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
   //if (zs_fbfetch) input_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
   VkSubpassDependency2 fbfetch_deps[] = {
      /*{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, VK_SUBPASS_EXTERNAL, 0, dep_pipeline, dep_pipeline, 0, dep_access, flag, 0},
      {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, 0, dep_pipeline, input_dep, dep_access, input_access, flag, 0},
      {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, VK_SUBPASS_EXTERNAL, dep_pipeline, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dep_access, 0, flag, 0}*/
	  {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, VK_SUBPASS_EXTERNAL, 0, dep_pipeline, dep_pipeline, 0, dep_access, VK_DEPENDENCY_BY_REGION_BIT, 0},
      {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, 0, dep_pipeline, input_dep, dep_access, input_access, VK_DEPENDENCY_BY_REGION_BIT, 0},
      {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, VK_SUBPASS_EXTERNAL, dep_pipeline, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dep_access, 0, VK_DEPENDENCY_BY_REGION_BIT, 0}
   };

   VkSubpassDescription2 subpass = {0};
   VkSubpassDescriptionDepthStencilResolve zsresolve;
   subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
   subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = state->num_cbufs;
   subpass.pColorAttachments = color_refs;
   subpass.pDepthStencilAttachment = state->have_zsbuf ? &zs_ref : NULL;
   subpass.inputAttachmentCount = input_count;
   subpass.pInputAttachments = input_attachments;
   if (state->num_cresolves)
      subpass.pResolveAttachments = color_resolves;
   if (state->num_zsresolves) {
      subpass.pNext = &zsresolve;
      zsresolve.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
      zsresolve.pNext = NULL;
      zsresolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      zsresolve.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      zsresolve.pDepthStencilResolveAttachment = &zs_resolve;
   } else
      subpass.pNext = NULL;

   VkRenderPassCreateInfo2 rpci = {0};
   rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
   rpci.attachmentCount = num_attachments + state->num_cresolves + state->num_zsresolves;
   rpci.pAttachments = attachments;
   rpci.subpassCount = 1;
   rpci.pSubpasses = &subpass;
   rpci.dependencyCount = input_count ? 3 : 2;
   rpci.pDependencies = input_count ? fbfetch_deps : deps;

   VkRenderPass render_pass;
   if (VKSCR(CreateRenderPass2)(screen->dev, &rpci, NULL, &render_pass) != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateRenderPass2 failed");
      return VK_NULL_HANDLE;
   }

   return render_pass;
}

struct zink_render_pass *
zink_create_render_pass(struct zink_screen *screen,
                        struct zink_render_pass_state *state,
                        struct zink_render_pass_pipeline_state *pstate)
{
   struct zink_render_pass *rp = CALLOC_STRUCT(zink_render_pass);
   if (!rp)
      goto fail;

   //rp->render_pass = create_render_pass2(screen, state, pstate);
   rp->render_pass = screen->vk_version >= VK_MAKE_VERSION(1,2,0) ?
                     create_render_pass2(screen, state, pstate) : create_render_pass(screen, state, pstate);
   if (!rp->render_pass)
      goto fail;
   memcpy(&rp->state, state, sizeof(struct zink_render_pass_state));
   return rp;

fail:
   if (rp)
      zink_destroy_render_pass(screen, rp);
   return NULL;
}

void
zink_destroy_render_pass(struct zink_screen *screen,
                         struct zink_render_pass *rp)
{
   VKSCR(DestroyRenderPass)(screen->dev, rp->render_pass, NULL);
   FREE(rp);
}

VkImageLayout
zink_render_pass_attachment_get_barrier_info(const struct zink_rt_attrib *rt, bool color,
                                             VkPipelineStageFlags *pipeline, VkAccessFlags *access)
{
   *access = 0;
   if (color) {
      *pipeline = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      *access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      if (!rt->clear_color && !rt->invalid)
         *access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
      return rt->fbfetch ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   }

   *pipeline = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
   if (rt->mixed_zs) {
      *access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      return VK_IMAGE_LAYOUT_GENERAL;
   }
   if (!rt->clear_color && !rt->clear_stencil)
      *access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
   if (!rt->clear_color && !rt->clear_stencil && !rt->needs_write)
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   *access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}


static size_t
rp_state_size(const struct zink_render_pass_pipeline_state *pstate)
{
   return offsetof(struct zink_render_pass_pipeline_state, attachments) +
                   sizeof(pstate->attachments[0]) * pstate->num_attachments;
}

static uint32_t
hash_rp_state(const void *key)
{
   const struct zink_render_pass_pipeline_state *s = key;
   return _mesa_hash_data(key, rp_state_size(s));
}

static bool
equals_rp_state(const void *a, const void *b)
{
   return !memcmp(a, b, rp_state_size(a));
}

static uint32_t
hash_render_pass_state(const void *key)
{
   struct zink_render_pass_state* s = (struct zink_render_pass_state*)key;
   return _mesa_hash_data(key, offsetof(struct zink_render_pass_state, rts) + sizeof(s->rts[0]) * s->num_rts);
}

static bool
equals_render_pass_state(const void *a, const void *b)
{
   const struct zink_render_pass_state *s_a = a, *s_b = b;
   if (s_a->num_rts != s_b->num_rts)
      return false;
   return memcmp(a, b, offsetof(struct zink_render_pass_state, rts) + sizeof(s_a->rts[0]) * s_a->num_rts) == 0;
}

void
zink_init_zs_attachment(struct zink_context *ctx, struct zink_rt_attrib *rt)
{
   const struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_resource *zsbuf = zink_resource(fb->zsbuf->texture);
   struct zink_framebuffer_clear *fb_clear = &ctx->fb_clears[PIPE_MAX_COLOR_BUFS];
   struct zink_surface *transient = zink_transient_surface(fb->zsbuf);
   rt->format = zsbuf->format;
   rt->samples = MAX3(transient ? transient->base.nr_samples : 0, fb->zsbuf->texture->nr_samples, 1);
   rt->clear_color = zink_fb_clear_enabled(ctx, PIPE_MAX_COLOR_BUFS) &&
                                         !zink_fb_clear_first_needs_explicit(fb_clear) &&
                                         (zink_fb_clear_element(fb_clear, 0)->zs.bits & PIPE_CLEAR_DEPTH);
   rt->clear_stencil = zink_fb_clear_enabled(ctx, PIPE_MAX_COLOR_BUFS) &&
                                           !zink_fb_clear_first_needs_explicit(fb_clear) &&
                                           (zink_fb_clear_element(fb_clear, 0)->zs.bits & PIPE_CLEAR_STENCIL);
   const uint64_t outputs_written = ctx->gfx_stages[PIPE_SHADER_FRAGMENT] ?
                                    ctx->gfx_stages[PIPE_SHADER_FRAGMENT]->nir->info.outputs_written : 0;
   bool needs_write_z = (ctx->dsa_state && ctx->dsa_state->hw_state.depth_write) ||
                       outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH);
   needs_write_z |= transient || rt->clear_color;

   bool needs_write_s = rt->clear_stencil || outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL);
   if (!needs_write_z && (!ctx->dsa_state || !ctx->dsa_state->base.depth_enabled))
      /* depth sample, stencil write */
      rt->mixed_zs = needs_write_s && zsbuf->bind_count[0];
   else
      /* depth write + sample */
      rt->mixed_zs = needs_write_z && zsbuf->bind_count[0];
   rt->needs_write = needs_write_z | needs_write_s;
}

void
zink_init_color_attachment(struct zink_context *ctx, unsigned i, struct zink_rt_attrib *rt)
{
   const struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct pipe_surface *psurf = fb->cbufs[i];
   if (psurf && !zink_use_dummy_attachments(ctx)) {
      struct zink_surface *surf = zink_csurface(psurf);
      struct zink_surface *transient = zink_transient_surface(psurf);
      rt->format = surf->info.format[0];
      rt->samples = MAX3(transient ? transient->base.nr_samples : 0, psurf->texture->nr_samples, 1);
      rt->clear_color = zink_fb_clear_enabled(ctx, i) && !zink_fb_clear_first_needs_explicit(&ctx->fb_clears[i]);
      rt->invalid = !zink_resource(psurf->texture)->valid || (ctx->new_swapchain && (psurf->texture->bind & PIPE_BIND_DISPLAY_TARGET));
      rt->fbfetch = (ctx->fbfetch_outputs & BITFIELD_BIT(i)) > 0;
   } else {
      memset(rt, 0, sizeof(struct zink_rt_attrib));
      rt->format = VK_FORMAT_R8G8B8A8_UNORM;
      rt->samples = fb->samples;
   }
}

static struct zink_render_pass *
get_render_pass(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   const struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_render_pass_state state = {0};
   uint32_t clears = 0;
   state.samples = fb->samples > 0;

   for (int i = 0; i < fb->nr_cbufs; i++) {
      zink_init_color_attachment(ctx, i, &state.rts[i]);
      struct pipe_surface *surf = fb->cbufs[i];
      if (surf && !zink_use_dummy_attachments(ctx)) {
         clears |= !!state.rts[i].clear_color ? PIPE_CLEAR_COLOR0 << i : 0;
         struct zink_surface *transient = zink_transient_surface(surf);
         if (transient) {
            state.num_cresolves++;
            state.rts[i].resolve = true;
            if (!state.rts[i].clear_color)
               state.msaa_expand_mask |= BITFIELD_BIT(i);
         } else {
            state.rts[i].resolve = false;
         }
      }
      state.num_rts++;
   }
   state.num_cbufs = fb->nr_cbufs;
   assert(!state.num_cresolves || state.num_cbufs == state.num_cresolves);

   if (fb->zsbuf) {
      zink_init_zs_attachment(ctx, &state.rts[fb->nr_cbufs]);
      struct zink_surface *transient = zink_transient_surface(fb->zsbuf);
      if (transient) {
         state.num_zsresolves = 1;
         state.rts[fb->nr_cbufs].resolve = true;
      }
      if (state.rts[fb->nr_cbufs].clear_color)
         clears |= PIPE_CLEAR_DEPTH;
      if (state.rts[fb->nr_cbufs].clear_stencil)
         clears |= PIPE_CLEAR_STENCIL;
      state.num_rts++;
   }
   state.have_zsbuf = fb->zsbuf != NULL;
   if (zink_use_dummy_attachments(ctx))
      assert(clears == (ctx->rp_clears_enabled & PIPE_CLEAR_DEPTHSTENCIL));
   else
      assert(clears == ctx->rp_clears_enabled);
   state.clears = clears;
   uint32_t hash = hash_render_pass_state(&state);
   struct hash_entry *entry = _mesa_hash_table_search_pre_hashed(ctx->render_pass_cache, hash,
                                                                 &state);
   struct zink_render_pass *rp;
   if (entry) {
      rp = entry->data;
      assert(rp->state.clears == clears);
   } else {
      struct zink_render_pass_pipeline_state pstate;
      pstate.samples = state.samples;
      rp = zink_create_render_pass(screen, &state, &pstate);
      if (!_mesa_hash_table_insert_pre_hashed(ctx->render_pass_cache, hash, &rp->state, rp))
         return NULL;
      bool found = false;
      struct set_entry *entry = _mesa_set_search_or_add(&ctx->render_pass_state_cache, &pstate, &found);
      struct zink_render_pass_pipeline_state *ppstate;
      if (!found) {
         entry->key = ralloc(ctx, struct zink_render_pass_pipeline_state);
         ppstate = (void*)entry->key;
         memcpy(ppstate, &pstate, rp_state_size(&pstate));
         ppstate->id = ctx->render_pass_state_cache.entries;
      }
      ppstate = (void*)entry->key;
      rp->pipeline_state = ppstate->id;
   }
   return rp;
}

static void
setup_framebuffer(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_render_pass *rp = ctx->gfx_pipeline_state.render_pass;

   zink_update_vk_sample_locations(ctx);

   if (rp)
      ctx->rp_changed |= ctx->rp_clears_enabled != rp->state.clears;
   if (ctx->rp_changed)
      rp = get_render_pass(ctx);

   ctx->fb_changed |= rp != ctx->gfx_pipeline_state.render_pass;
   if (rp->pipeline_state != ctx->gfx_pipeline_state.rp_state) {
      ctx->gfx_pipeline_state.rp_state = rp->pipeline_state;
      ctx->gfx_pipeline_state.dirty = true;
   }

   ctx->rp_changed = false;
   zink_render_update_swapchain(ctx);

   if (!ctx->fb_changed)
      return;

   zink_update_framebuffer_state(ctx);
   //zink_init_framebuffer(screen, ctx->framebuffer, rp);
   
   ctx->init_framebuffer(screen, ctx->framebuffer, rp);

   ctx->fb_changed = false;
   ctx->gfx_pipeline_state.render_pass = rp;
}

static bool
prep_fb_attachments(struct zink_context *ctx, VkImageView *att)
{
   const unsigned cresolve_offset = ctx->fb_state.nr_cbufs + !!ctx->fb_state.zsbuf;
   unsigned num_resolves = 0;
   for (int i = 0; i < ctx->fb_state.nr_cbufs; i++) {
      struct zink_surface *surf = zink_csurface(ctx->fb_state.cbufs[i]);
      struct zink_surface *transient = zink_transient_surface(ctx->fb_state.cbufs[i]);
      if (transient) {
         att[i] = zink_prep_fb_attachment(ctx, transient, i);
         att[i + cresolve_offset] = zink_prep_fb_attachment(ctx, surf, i);
         num_resolves++;
      } else {
         att[i] = zink_prep_fb_attachment(ctx, surf, i);
         if (!att[i])
            /* dead swapchain */
            return false;
      }
   }
   if (ctx->fb_state.zsbuf) {
      struct zink_surface *surf = zink_csurface(ctx->fb_state.zsbuf);
      struct zink_surface *transient = zink_transient_surface(ctx->fb_state.zsbuf);
      if (transient) {
         att[ctx->fb_state.nr_cbufs] = zink_prep_fb_attachment(ctx, transient, ctx->fb_state.nr_cbufs);
         att[cresolve_offset + num_resolves] = zink_prep_fb_attachment(ctx, surf, ctx->fb_state.nr_cbufs);
      } else {
         att[ctx->fb_state.nr_cbufs] = zink_prep_fb_attachment(ctx, surf, ctx->fb_state.nr_cbufs);
      }
   }
   return true;
}

static unsigned
begin_render_pass(struct zink_context *ctx)
{
   struct zink_batch *batch = &ctx->batch;
   struct pipe_framebuffer_state *fb_state = &ctx->fb_state;

   VkRenderPassBeginInfo rpbi = {0};
   rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   rpbi.renderPass = ctx->gfx_pipeline_state.render_pass->render_pass;
   rpbi.renderArea.offset.x = 0;
   rpbi.renderArea.offset.y = 0;
   rpbi.renderArea.extent.width = fb_state->width;
   rpbi.renderArea.extent.height = fb_state->height;

   VkClearValue clears[PIPE_MAX_COLOR_BUFS + 1] = {0};
   unsigned clear_buffers = 0;
   uint32_t clear_validate = 0;
   for (int i = 0; i < fb_state->nr_cbufs; i++) {
      /* these are no-ops */
      if (!fb_state->cbufs[i] || !zink_fb_clear_enabled(ctx, i) || zink_use_dummy_attachments(ctx))
         continue;
      /* these need actual clear calls inside the rp */
      struct zink_framebuffer_clear_data *clear = zink_fb_clear_element(&ctx->fb_clears[i], 0);
      if (zink_fb_clear_needs_explicit(&ctx->fb_clears[i])) {
         clear_buffers |= (PIPE_CLEAR_COLOR0 << i);
         if (zink_fb_clear_count(&ctx->fb_clears[i]) < 2 ||
             zink_fb_clear_element_needs_explicit(clear))
            continue;
      }
      /* we now know there's one clear that can be done here */
      zink_fb_clear_util_unpack_clear_color(clear, fb_state->cbufs[i]->format, (void*)&clears[i].color);
      rpbi.clearValueCount = i + 1;
      clear_validate |= PIPE_CLEAR_COLOR0 << i;
      assert(ctx->framebuffer->rp->state.clears);
   }
   if (fb_state->zsbuf && zink_fb_clear_enabled(ctx, PIPE_MAX_COLOR_BUFS)) {
      struct zink_framebuffer_clear *fb_clear = &ctx->fb_clears[PIPE_MAX_COLOR_BUFS];
      struct zink_framebuffer_clear_data *clear = zink_fb_clear_element(fb_clear, 0);
      if (!zink_fb_clear_element_needs_explicit(clear)) {
         clears[fb_state->nr_cbufs].depthStencil.depth = clear->zs.depth;
         clears[fb_state->nr_cbufs].depthStencil.stencil = clear->zs.stencil;
         rpbi.clearValueCount = fb_state->nr_cbufs + 1;
         clear_validate |= clear->zs.bits;
         assert(ctx->framebuffer->rp->state.clears);
      }
      if (zink_fb_clear_needs_explicit(fb_clear)) {
         for (int j = !zink_fb_clear_element_needs_explicit(clear);
              (clear_buffers & PIPE_CLEAR_DEPTHSTENCIL) != PIPE_CLEAR_DEPTHSTENCIL && j < zink_fb_clear_count(fb_clear);
              j++)
            clear_buffers |= zink_fb_clear_element(fb_clear, j)->zs.bits;
      }
   }
   assert(clear_validate == ctx->framebuffer->rp->state.clears);
   rpbi.pClearValues = &clears[0];
   rpbi.framebuffer = ctx->framebuffer->fb;

   assert(ctx->gfx_pipeline_state.render_pass && ctx->framebuffer);

   VkRenderPassAttachmentBeginInfo infos;
   VkImageView att[2 * (PIPE_MAX_COLOR_BUFS + 1)];
   infos.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
   infos.pNext = NULL;
   infos.attachmentCount = ctx->framebuffer->state.num_attachments;
   infos.pAttachments = att;
   if (!prep_fb_attachments(ctx, att))
      return 0;

   if (zink_screen(ctx->base.screen)->info.have_KHR_imageless_framebuffer) {  
#ifndef NDEBUG
   const unsigned cresolve_offset = ctx->fb_state.nr_cbufs + !!ctx->fb_state.zsbuf;
   for (int i = 0; i < ctx->fb_state.nr_cbufs; i++) {
      if (ctx->fb_state.cbufs[i]) {
         struct zink_surface *surf = zink_csurface(ctx->fb_state.cbufs[i]);
         if (zink_use_dummy_attachments(ctx)) {
            surf = zink_csurface(ctx->dummy_surface[util_logbase2_ceil(ctx->fb_state.samples)]);
            assert(zink_resource(surf->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[i].usage);
         } else {
            struct zink_surface *transient = zink_transient_surface(&surf->base);
            if (surf->base.format == ctx->fb_state.cbufs[i]->format) {
               if (transient) {
                  assert(zink_resource(transient->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[i].usage);
                  assert(zink_resource(surf->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[cresolve_offset].usage);
               } else {
                  assert(zink_resource(surf->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[i].usage);
               }
            }
         }
      }
   }
   if (ctx->fb_state.zsbuf) {
      struct zink_surface *surf = zink_csurface(ctx->fb_state.zsbuf);
      struct zink_surface *transient = zink_transient_surface(ctx->fb_state.zsbuf);
      if (transient) {
         assert(zink_resource(transient->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[ctx->fb_state.nr_cbufs].usage);
         assert(zink_resource(surf->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[cresolve_offset].usage);
      } else {
         assert(zink_resource(surf->base.texture)->obj->vkusage == ctx->framebuffer->state.infos[ctx->fb_state.nr_cbufs].usage);
      }
   }
#endif
   rpbi.pNext = &infos;
   }

   VKCTX(CmdBeginRenderPass)(batch->state->cmdbuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   batch->in_rp = true;
   ctx->new_swapchain = false;
   return clear_buffers;
}

unsigned
zink_begin_render_pass(struct zink_context *ctx)
{
   setup_framebuffer(ctx);
   /* TODO: need replicate EXT */
   if (ctx->framebuffer->rp->state.msaa_expand_mask) {
      uint32_t rp_state = ctx->gfx_pipeline_state.rp_state;
      struct zink_render_pass *rp = ctx->gfx_pipeline_state.render_pass;

      u_foreach_bit(i, ctx->framebuffer->rp->state.msaa_expand_mask) {
         struct zink_ctx_surface *csurf = (struct zink_ctx_surface*)ctx->fb_state.cbufs[i];
         if (csurf->transient_init)
            continue;
         struct pipe_surface *dst_view = (struct pipe_surface*)csurf->transient;
         assert(dst_view);
         struct pipe_sampler_view src_templ, *src_view;
         struct pipe_resource *src = ctx->fb_state.cbufs[i]->texture;
         struct pipe_box dstbox;

         u_box_3d(0, 0, 0, ctx->fb_state.width, ctx->fb_state.height,
                  1 + dst_view->u.tex.last_layer - dst_view->u.tex.first_layer, &dstbox);

         util_blitter_default_src_texture(ctx->blitter, &src_templ, src, ctx->fb_state.cbufs[i]->u.tex.level);
         src_view = ctx->base.create_sampler_view(&ctx->base, src, &src_templ);

         zink_blit_begin(ctx, ZINK_BLIT_SAVE_FB | ZINK_BLIT_SAVE_FS | ZINK_BLIT_SAVE_TEXTURES);
         util_blitter_blit_generic(ctx->blitter, dst_view, &dstbox,
                                   src_view, &dstbox, ctx->fb_state.width, ctx->fb_state.height,
                                   PIPE_MASK_RGBAZS, PIPE_TEX_FILTER_NEAREST, NULL,
                                   false, false);

         pipe_sampler_view_reference(&src_view, NULL);
         csurf->transient_init = true;
      }
      ctx->fb_changed = ctx->rp_changed = false;
      ctx->gfx_pipeline_state.rp_state = rp_state;
      ctx->gfx_pipeline_state.render_pass = rp;
   }
   assert(ctx->gfx_pipeline_state.render_pass);
   return begin_render_pass(ctx);
}

void
zink_end_render_pass(struct zink_context *ctx)
{
   if (ctx->batch.in_rp) {
      VKCTX(CmdEndRenderPass)(ctx->batch.state->cmdbuf);
      for (unsigned i = 0; i < ctx->fb_state.nr_cbufs; i++) {
         struct zink_ctx_surface *csurf = (struct zink_ctx_surface*)ctx->fb_state.cbufs[i];
         if (csurf)
            csurf->transient_init = true;
      }
   }
   ctx->batch.in_rp = false;
}

bool
zink_init_render_pass(struct zink_context *ctx)
{
   _mesa_set_init(&ctx->render_pass_state_cache, ctx, hash_rp_state, equals_rp_state);
   ctx->render_pass_cache = _mesa_hash_table_create(NULL,
                                                    hash_render_pass_state,
                                                    equals_render_pass_state);
   return !!ctx->render_pass_cache;
}

void
zink_render_update_swapchain(struct zink_context *ctx)
{
   bool has_swapchain = false;
   for (unsigned i = 0; i < ctx->fb_state.nr_cbufs; i++) {
      if (!ctx->fb_state.cbufs[i])
         continue;
      struct zink_resource *res = zink_resource(ctx->fb_state.cbufs[i]->texture);
      if (zink_is_swapchain(res)) {
         has_swapchain = true;
         if (zink_kopper_acquire(ctx, res, UINT64_MAX))
            zink_surface_swapchain_update(ctx, zink_csurface(ctx->fb_state.cbufs[i]));
      }
   }
   if (has_swapchain && (ctx->swapchain_size.width || ctx->swapchain_size.height)) {
      unsigned old_w = ctx->fb_state.width;
      unsigned old_h = ctx->fb_state.height;
      ctx->fb_state.width = ctx->swapchain_size.width;
      ctx->fb_state.height = ctx->swapchain_size.height;
      zink_kopper_fixup_depth_buffer(ctx);
      if (ctx->fb_state.width != old_w || ctx->fb_state.height != old_h)
         ctx->scissor_changed = true;
      zink_update_framebuffer_state(ctx);
      ctx->swapchain_size.width = ctx->swapchain_size.height = 0;
   }
}
