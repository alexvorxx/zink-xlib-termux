/*
 * Copyright © 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "intel_gem.h"
#include "drm-uapi/i915_drm.h"

#define RCS_TIMESTAMP 0x2358

bool
intel_gem_supports_syncobj_wait(int fd)
{
   int ret;

   struct drm_syncobj_create create = {
      .flags = 0,
   };
   ret = intel_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
   if (ret)
      return false;

   uint32_t syncobj = create.handle;

   struct drm_syncobj_wait wait = {
      .handles = (uint64_t)(uintptr_t)&create,
      .count_handles = 1,
      .timeout_nsec = 0,
      .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
   };
   ret = intel_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);

   struct drm_syncobj_destroy destroy = {
      .handle = syncobj,
   };
   intel_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

   /* If it timed out, then we have the ioctl and it supports the
    * DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT flag.
    */
   return ret == -1 && errno == ETIME;
}

bool
intel_gem_create_context(int fd, uint32_t *context_id)
{
   struct drm_i915_gem_context_create create = {};
   if (intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create))
      return false;
   *context_id = create.ctx_id;
   return true;
}

bool
intel_gem_destroy_context(int fd, uint32_t context_id)
{
   struct drm_i915_gem_context_destroy destroy = {
      .ctx_id = context_id,
   };
   return intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy) == 0;
}

bool
intel_gem_create_context_engines(int fd,
                                 const struct intel_query_engine_info *info,
                                 int num_engines, enum intel_engine_class *engine_classes,
                                 uint32_t *context_id)
{
   assert(info != NULL);
   assert(num_engines <= 64);
   I915_DEFINE_CONTEXT_PARAM_ENGINES(engines_param, 64);
   engines_param.extensions = 0;

   /* For each type of intel_engine_class of interest, we keep track of
    * the previous engine instance used.
    */
   int last_engine_idx[] = {
      [INTEL_ENGINE_CLASS_RENDER] = -1,
      [INTEL_ENGINE_CLASS_COPY] = -1,
      [INTEL_ENGINE_CLASS_COMPUTE] = -1,
   };

   int engine_counts[] = {
      [INTEL_ENGINE_CLASS_RENDER] =
         intel_engines_count(info, INTEL_ENGINE_CLASS_RENDER),
      [INTEL_ENGINE_CLASS_COPY] =
         intel_engines_count(info, INTEL_ENGINE_CLASS_COPY),
      [INTEL_ENGINE_CLASS_COMPUTE] =
         intel_engines_count(info, INTEL_ENGINE_CLASS_COMPUTE),
   };

   /* For each queue, we look for the next instance that matches the class we
    * need.
    */
   for (int i = 0; i < num_engines; i++) {
      enum intel_engine_class engine_class = engine_classes[i];
      assert(engine_class == INTEL_ENGINE_CLASS_RENDER ||
             engine_class == INTEL_ENGINE_CLASS_COPY ||
             engine_class == INTEL_ENGINE_CLASS_COMPUTE);
      if (engine_counts[engine_class] <= 0)
         return false;

      /* Run through the engines reported by the kernel looking for the next
       * matching instance. We loop in case we want to create multiple
       * contexts on an engine instance.
       */
      int engine_instance = -1;
      for (int i = 0; i < info->num_engines; i++) {
         int *idx = &last_engine_idx[engine_class];
         if (++(*idx) >= info->num_engines)
            *idx = 0;
         if (info->engines[*idx].engine_class == engine_class) {
            engine_instance = info->engines[*idx].engine_instance;
            break;
         }
      }
      if (engine_instance < 0)
         return false;

      engines_param.engines[i].engine_class = intel_engine_class_to_i915(engine_class);
      engines_param.engines[i].engine_instance = engine_instance;
   }

   uint32_t size = sizeof(engines_param.extensions);
   size += sizeof(engines_param.engines[0]) * num_engines;
   struct drm_i915_gem_context_create_ext_setparam set_engines = {
      .base = {
         .name = I915_CONTEXT_CREATE_EXT_SETPARAM,
      },
      .param = {
         .param = I915_CONTEXT_PARAM_ENGINES,
         .value = (uintptr_t)&engines_param,
         .size = size,
      }
   };
   struct drm_i915_gem_context_create_ext create = {
      .flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
      .extensions = (uintptr_t)&set_engines,
   };
   if (intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &create) == -1)
      return false;

   *context_id = create.ctx_id;
   return true;
}

bool
intel_gem_set_context_param(int fd, uint32_t context, uint32_t param,
                            uint64_t value)
{
   struct drm_i915_gem_context_param p = {
      .ctx_id = context,
      .param = param,
      .value = value,
   };
   return intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, &p) == 0;
}

bool
intel_gem_get_context_param(int fd, uint32_t context, uint32_t param,
                            uint64_t *value)
{
   struct drm_i915_gem_context_param gp = {
      .ctx_id = context,
      .param = param,
   };
   if (intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &gp))
      return false;
   *value = gp.value;
   return true;
}

bool intel_gem_read_render_timestamp(int fd, uint64_t *value)
{
   struct drm_i915_reg_read reg_read = {
      .offset = RCS_TIMESTAMP | I915_REG_READ_8B_WA,
   };

   int ret = intel_ioctl(fd, DRM_IOCTL_I915_REG_READ, &reg_read);
   if (ret == 0)
      *value = reg_read.val;
   return ret == 0;
}

bool
intel_gem_create_context_ext(int fd, enum intel_gem_create_context_flags flags,
                             uint32_t *ctx_id)
{
   struct drm_i915_gem_context_create_ext_setparam recoverable_param = {
      .param = {
         .param = I915_CONTEXT_PARAM_RECOVERABLE,
         .value = flags & INTEL_GEM_CREATE_CONTEXT_EXT_RECOVERABLE_FLAG,
      },
   };
   struct drm_i915_gem_context_create_ext_setparam protected_param = {
      .param = {
         .param = I915_CONTEXT_PARAM_PROTECTED_CONTENT,
         .value = flags & INTEL_GEM_CREATE_CONTEXT_EXT_PROTECTED_FLAG,
      },
   };
   struct drm_i915_gem_context_create_ext create = {
      .flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
   };

   intel_gem_add_ext(&create.extensions,
                     I915_CONTEXT_CREATE_EXT_SETPARAM,
                     &recoverable_param.base);
   intel_gem_add_ext(&create.extensions,
                     I915_CONTEXT_CREATE_EXT_SETPARAM,
                     &protected_param.base);

   if (intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &create))
      return false;

   *ctx_id = create.ctx_id;
   return true;
}

bool
intel_gem_supports_protected_context(int fd)
{
   uint32_t ctx_id;
   bool ret = intel_gem_create_context_ext(fd,
                                           INTEL_GEM_CREATE_CONTEXT_EXT_PROTECTED_FLAG,
                                           &ctx_id);
   if (!ret)
      return ret;

   struct drm_i915_gem_context_destroy destroy = {
      .ctx_id = ctx_id,
   };
   intel_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy);

   return ret;
}

bool
intel_gem_get_param(int fd, uint32_t param, int *value)
{
   drm_i915_getparam_t gp = {
      .param = param,
      .value = value,
   };
   return intel_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) == 0;
}

bool intel_gem_can_render_on_fd(int fd)
{
   int val;
   return intel_gem_get_param(fd, I915_PARAM_CHIPSET_ID, &val) && val > 0;
}
