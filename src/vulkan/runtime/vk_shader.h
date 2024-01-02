/*
 * Copyright © 2024 Collabora, Ltd.
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

#ifndef VK_SHADER_H
#define VK_SHADER_H

#include "compiler/spirv/nir_spirv.h"
#include "vk_limits.h"
#include "vk_pipeline_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

struct blob;
struct nir_shader;
struct vk_command_buffer;
struct vk_device;
struct vk_descriptor_set_layout;
struct vk_dynamic_graphics_state;
struct vk_graphics_pipeline_state;
struct vk_physical_device;
struct vk_pipeline;
struct vk_pipeline_robustness_state;

int vk_shader_cmp_graphics_stages(gl_shader_stage a, gl_shader_stage b);

struct vk_shader_compile_info {
   gl_shader_stage stage;
   VkShaderCreateFlagsEXT flags;
   VkShaderStageFlags next_stage_mask;
   struct nir_shader *nir;

   const struct vk_pipeline_robustness_state *robustness;

   uint32_t set_layout_count;
   struct vk_descriptor_set_layout * const *set_layouts;

   uint32_t push_constant_range_count;
   const VkPushConstantRange *push_constant_ranges;
};

struct vk_shader_ops;

struct vk_shader {
   struct vk_object_base base;

   const struct vk_shader_ops *ops;

   gl_shader_stage stage;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(vk_shader, base, VkShaderEXT,
                               VK_OBJECT_TYPE_SHADER_EXT);

struct vk_shader_ops {
   /** Destroy a vk_shader_object */
   void (*destroy)(struct vk_device *device,
                   struct vk_shader *shader,
                   const VkAllocationCallbacks* pAllocator);

   /** Serialize a vk_shader_object to a blob
    *
    * This function shouldn't need to do any validation of the blob data
    * beyond basic sanity checking.  The common implementation of
    * vkGetShaderBinaryEXT verifies the blobUUID and version of input data as
    * well as a size and checksum to ensure integrity.  This callback is only
    * invoked after validation of the input binary data.
    */
   bool (*serialize)(struct vk_device *device,
                     const struct vk_shader *shader,
                     struct blob *blob);
};

void *vk_shader_zalloc(struct vk_device *device,
                       const struct vk_shader_ops *ops,
                       gl_shader_stage stage,
                       const VkAllocationCallbacks *alloc,
                       size_t size);
void vk_shader_free(struct vk_device *device,
                    const VkAllocationCallbacks *alloc,
                    struct vk_shader *shader);

static inline void
vk_shader_destroy(struct vk_device *device,
                  struct vk_shader *shader,
                  const VkAllocationCallbacks *alloc)
{
   shader->ops->destroy(device, shader, alloc);
}

struct vk_device_shader_ops {
   /** Retrieves a NIR compiler options struct
    *
    * NIR compiler options are only allowed to vary based on physical device,
    * stage, and robustness state.
    */
   const struct nir_shader_compiler_options *(*get_nir_options)(
      struct vk_physical_device *device,
      gl_shader_stage stage,
      const struct vk_pipeline_robustness_state *rs);

   /** Retrieves a SPIR-V options struct
    *
    * SPIR-V options are only allowed to vary based on physical device, stage,
    * and robustness state.
    */
   struct spirv_to_nir_options (*get_spirv_options)(
      struct vk_physical_device *device,
      gl_shader_stage stage,
      const struct vk_pipeline_robustness_state *rs);

   /** Preprocesses a NIR shader
    *
    * This callback is optional.
    *
    * If non-NULL, this callback is invoked after the SPIR-V is parsed into
    * NIR and before it is handed to compile().  The driver should do as much
    * generic optimization and lowering as it can here.  Importantly, the
    * preprocess step only knows about the NIR input and the physical device,
    * not any enabled device features or pipeline state.  This allows us to
    * potentially cache this shader and re-use it across pipelines.
    */
   void (*preprocess_nir)(struct vk_physical_device *device, nir_shader *nir);

   /** Compile (and potentially link) a set of shaders
    *
    * Unlike vkCreateShadersEXT, this callback will only ever be called with
    * multiple shaders if VK_SHADER_CREATE_LINK_STAGE_BIT_EXT is set on all of
    * them.  We also guarantee that the shaders occur in the call in Vulkan
    * pipeline stage order as dictated by vk_shader_cmp_graphics_stages().
    *
    * This callback consumes all input NIR shaders, regardless of whether or
    * not it was successful.
    */
   VkResult (*compile)(struct vk_device *device,
                       uint32_t shader_count,
                       struct vk_shader_compile_info *infos,
                       const struct vk_graphics_pipeline_state *state,
                       const VkAllocationCallbacks* pAllocator,
                       struct vk_shader **shaders_out);

   /** Create a vk_shader from a binary blob */
   VkResult (*deserialize)(struct vk_device *device,
                           struct blob_reader *blob,
                           uint32_t binary_version,
                           const VkAllocationCallbacks* pAllocator,
                           struct vk_shader **shader_out);

   /** Bind a set of shaders
    *
    * This is roughly equivalent to vkCmdBindShadersEXT()
    */
   void (*cmd_bind_shaders)(struct vk_command_buffer *cmd_buffer,
                            uint32_t stage_count,
                            const gl_shader_stage *stages,
                            struct vk_shader ** const shaders);
};

#ifdef __cplusplus
}
#endif

#endif /* VK_SHADER_H */
