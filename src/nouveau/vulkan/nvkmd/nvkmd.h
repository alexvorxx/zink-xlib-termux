/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_H
#define NVKMD_H 1

#include "nv_device_info.h"
#include "vulkan/vulkan_core.h"
#include "nouveau_device.h"

#include <assert.h>
#include <stdbool.h>
#include <stdbool.h>
#include <sys/types.h>

struct nvkmd_ctx;
struct nvkmd_dev;
struct nvkmd_mem;
struct nvkmd_pdev;
struct nvkmd_va;

struct _drmDevice;
struct vk_object_base;

/*
 * Enums
 */

/*
 * Structs
 */

struct nvkmd_info {
   bool has_get_vram_used;
   bool has_alloc_tiled;
   bool has_map_fixed;
   bool has_overmap;
};

struct nvkmd_pdev_ops {
   void (*destroy)(struct nvkmd_pdev *pdev);

   uint64_t (*get_vram_used)(struct nvkmd_pdev *pdev);

   int (*get_drm_primary_fd)(struct nvkmd_pdev *pdev);

   VkResult (*create_dev)(struct nvkmd_pdev *pdev,
                          struct vk_object_base *log_obj,
                          struct nvkmd_dev **dev_out);
};

struct nvkmd_pdev {
   const struct nvkmd_pdev_ops *ops;

   struct nv_device_info dev_info;
   struct nvkmd_info kmd_info;

   struct {
      dev_t render_dev;
      dev_t primary_dev;
   } drm;

   const struct vk_sync_type *const *sync_types;
};

struct nvkmd_dev_ops {
   void (*destroy)(struct nvkmd_dev *dev);

   uint64_t (*get_gpu_timestamp)(struct nvkmd_dev *dev);

   int (*get_drm_fd)(struct nvkmd_dev *dev);
};

struct nvkmd_dev {
   const struct nvkmd_dev_ops *ops;
};

/*
 * Macros
 *
 * All subclassed structs must be named nvkmd_<subcls>_<strct> where the
 * original struct is named nvkmd_<strct>
 */

#define NVKMD_DECL_SUBCLASS(strct, subcls)                                 \
   extern const struct nvkmd_##strct##_ops nvkmd_##subcls##_##strct##_ops; \
   static inline struct nvkmd_##subcls##_##strct *                         \
   nvkmd_##subcls##_##strct(struct nvkmd_##strct *nvkmd)                   \
   {                                                                       \
      assert(nvkmd->ops == &nvkmd_##subcls##_##strct##_ops);               \
      return container_of(nvkmd, struct nvkmd_##subcls##_##strct, base);   \
   }

/*
 * Methods
 *
 * Even though everything goes through a function pointer table, we always add
 * an inline wrapper in case we want to move something into "core" NVKMD.
 */

VkResult MUST_CHECK
nvkmd_try_create_pdev_for_drm(struct _drmDevice *drm_device,
                              struct vk_object_base *log_obj,
                              enum nvk_debug debug_flags,
                              struct nvkmd_pdev **pdev_out);

static inline void
nvkmd_pdev_destroy(struct nvkmd_pdev *pdev)
{
   pdev->ops->destroy(pdev);
}

static inline uint64_t
nvkmd_pdev_get_vram_used(struct nvkmd_pdev *pdev)
{
   return pdev->ops->get_vram_used(pdev);
}

static inline int
nvkmd_pdev_get_drm_primary_fd(struct nvkmd_pdev *pdev)
{
   if (pdev->ops->get_drm_primary_fd == NULL)
      return -1;

   return pdev->ops->get_drm_primary_fd(pdev);
}

static inline VkResult MUST_CHECK
nvkmd_pdev_create_dev(struct nvkmd_pdev *pdev,
                      struct vk_object_base *log_obj,
                      struct nvkmd_dev **dev_out)
{
   return pdev->ops->create_dev(pdev, log_obj, dev_out);
}

static inline void
nvkmd_dev_destroy(struct nvkmd_dev *dev)
{
   dev->ops->destroy(dev);
}

static inline uint64_t
nvkmd_dev_get_gpu_timestamp(struct nvkmd_dev *dev)
{
   return dev->ops->get_gpu_timestamp(dev);
}

static inline int
nvkmd_dev_get_drm_fd(struct nvkmd_dev *dev)
{
   if (dev->ops->get_drm_fd == NULL)
      return -1;

   return dev->ops->get_drm_fd(dev);
}

#endif /* NVKMD_H */
