/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 */

#include <fcntl.h>

#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#include <sys/sysmacros.h>
#endif

#include <sys/mman.h>

#include "vk_debug_utils.h"

#include "util/libdrm.h"

#include "tu_device.h"
#include "tu_knl.h"
#include "tu_rmv.h"


VkResult
tu_bo_init_new_explicit_iova(struct tu_device *dev,
                             struct vk_object_base *base,
                             struct tu_bo **out_bo,
                             uint64_t size,
                             uint64_t client_iova,
                             VkMemoryPropertyFlags mem_property,
                             enum tu_bo_alloc_flags flags, const char *name)
{
   struct tu_instance *instance = dev->physical_device->instance;

   VkResult result =
      dev->instance->knl->bo_init(dev, base, out_bo, size, client_iova,
                                  mem_property, flags, name);
   if (result != VK_SUCCESS)
      return result;

   vk_address_binding_report(&instance->vk, base ? base : &dev->vk.base,
                             (*out_bo)->iova, (*out_bo)->size,
                             VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);

   return VK_SUCCESS;
}

VkResult
tu_bo_init_dmabuf(struct tu_device *dev,
                  struct tu_bo **bo,
                  uint64_t size,
                  int fd)
{
   return dev->instance->knl->bo_init_dmabuf(dev, bo, size, fd);
}

int
tu_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   return dev->instance->knl->bo_export_dmabuf(dev, bo);
}

void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   struct tu_instance *instance = dev->physical_device->instance;

   vk_address_binding_report(&instance->vk, bo->base ? bo->base : &dev->vk.base,
                             bo->iova, bo->size,
                             VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);

   dev->instance->knl->bo_finish(dev, bo);
}

VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   if (bo->map && (placed_addr == NULL || placed_addr == bo->map))
      return VK_SUCCESS;
   else if (bo->map)
      /* The BO is already mapped, but with a different address. */
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED, "Cannot remap BO to a different address");

   return dev->instance->knl->bo_map(dev, bo, placed_addr);
}

VkResult
tu_bo_unmap(struct tu_device *dev, struct tu_bo *bo, bool reserve)
{
   if (!bo->map || bo->never_unmap)
      return VK_SUCCESS;

   TU_RMV(bo_unmap, dev, bo);

   if (reserve) {
      void *map = mmap(bo->map, bo->size, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      if (map == MAP_FAILED)
         return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                          "Failed to replace mapping with reserved memory");
   } else {
      munmap(bo->map, bo->size);
   }

   bo->map = NULL;

   return VK_SUCCESS;
}

static inline void
tu_sync_cacheline_to_gpu(void const *p __attribute__((unused)))
{
#if DETECT_ARCH_AARCH64
   /* Clean data cache. */
   __asm volatile("dc cvac, %0" : : "r" (p) : "memory");
#elif (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
   __builtin_ia32_clflush(p);
#elif DETECT_ARCH_ARM
   /* DCCMVAC - same as DC CVAC on aarch64.
    * Seems to be illegal to call from userspace.
    */
   //__asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r" (p) : "memory");
   unreachable("Cache line clean is unsupported on ARMv7");
#endif
}

static inline void
tu_sync_cacheline_from_gpu(void const *p __attribute__((unused)))
{
#if DETECT_ARCH_AARCH64
   /* Clean and Invalidate data cache, there is no separate Invalidate. */
   __asm volatile("dc civac, %0" : : "r" (p) : "memory");
#elif (DETECT_ARCH_X86 || DETECT_ARCH_X86_64)
   __builtin_ia32_clflush(p);
#elif DETECT_ARCH_ARM
   /* DCCIMVAC - same as DC CIVAC on aarch64.
    * Seems to be illegal to call from userspace.
    */
   //__asm volatile("mcr p15, 0, %0, c7, c14, 1" : : "r" (p) : "memory");
   unreachable("Cache line invalidate is unsupported on ARMv7");
#endif
}

void
tu_bo_sync_cache(struct tu_device *dev,
                 struct tu_bo *bo,
                 VkDeviceSize offset,
                 VkDeviceSize size,
                 enum tu_mem_sync_op op)
{
   uintptr_t level1_dcache_size = dev->physical_device->level1_dcache_size;
   char *start = (char *) bo->map + offset;
   char *end = start + (size == VK_WHOLE_SIZE ? (bo->size - offset) : size);

   start = (char *) ((uintptr_t) start & ~(level1_dcache_size - 1));

   for (; start < end; start += level1_dcache_size) {
      if (op == TU_MEM_SYNC_CACHE_TO_GPU) {
         tu_sync_cacheline_to_gpu(start);
      } else {
         tu_sync_cacheline_from_gpu(start);
      }
   }
}

uint32_t
tu_get_l1_dcache_size()
{
if (!(DETECT_ARCH_AARCH64 || DETECT_ARCH_X86 || DETECT_ARCH_X86_64))
   return 0;

#if DETECT_ARCH_AARCH64 &&                                                   \
   (!defined(_SC_LEVEL1_DCACHE_LINESIZE) || DETECT_OS_ANDROID)
   /* Bionic does not implement _SC_LEVEL1_DCACHE_LINESIZE properly: */
   uint64_t ctr_el0;
   asm("mrs\t%x0, ctr_el0" : "=r"(ctr_el0));
   return 4 << ((ctr_el0 >> 16) & 0xf);
#elif defined(_SC_LEVEL1_DCACHE_LINESIZE)
   return sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
#else
   return 0;
#endif
}

void tu_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
   dev->instance->knl->bo_allow_dump(dev, bo);
}

void
tu_bo_set_metadata(struct tu_device *dev, struct tu_bo *bo,
                   void *metadata, uint32_t metadata_size)
{
   if (!dev->instance->knl->bo_set_metadata)
      return;
   dev->instance->knl->bo_set_metadata(dev, bo, metadata, metadata_size);
}

int
tu_bo_get_metadata(struct tu_device *dev, struct tu_bo *bo,
                   void *metadata, uint32_t metadata_size)
{
   if (!dev->instance->knl->bo_get_metadata)
      return -ENOSYS;
   return dev->instance->knl->bo_get_metadata(dev, bo, metadata, metadata_size);
}

VkResult
tu_drm_device_init(struct tu_device *dev)
{
   return dev->instance->knl->device_init(dev);
}

void
tu_drm_device_finish(struct tu_device *dev)
{
   dev->instance->knl->device_finish(dev);
}

int
tu_device_get_gpu_timestamp(struct tu_device *dev,
                            uint64_t *ts)
{
   return dev->instance->knl->device_get_gpu_timestamp(dev, ts);
}

int
tu_device_get_suspend_count(struct tu_device *dev,
                            uint64_t *suspend_count)
{
   return dev->instance->knl->device_get_suspend_count(dev, suspend_count);
}

VkResult
tu_device_wait_u_trace(struct tu_device *dev, struct tu_u_trace_syncobj *syncobj)
{
   return dev->instance->knl->device_wait_u_trace(dev, syncobj);
}

VkResult
tu_device_check_status(struct vk_device *vk_device)
{
   struct tu_device *dev = container_of(vk_device, struct tu_device, vk);
   return dev->instance->knl->device_check_status(dev);
}

int
tu_drm_submitqueue_new(struct tu_device *dev,
                       int priority,
                       uint32_t *queue_id)
{
   return dev->instance->knl->submitqueue_new(dev, priority, queue_id);
}

void
tu_drm_submitqueue_close(struct tu_device *dev, uint32_t queue_id)
{
   dev->instance->knl->submitqueue_close(dev, queue_id);
}

VkResult
tu_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct tu_queue *queue = container_of(vk_queue, struct tu_queue, vk);
   return queue->device->instance->knl->queue_submit(queue, submit);
}

/**
 * Enumeration entrypoint specific to non-drm devices (ie. kgsl)
 */
VkResult
tu_enumerate_devices(struct vk_instance *vk_instance)
{
#ifdef TU_HAS_KGSL
   struct tu_instance *instance =
      container_of(vk_instance, struct tu_instance, vk);

   static const char path[] = "/dev/kgsl-3d0";
   int fd;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      if (errno == ENOENT)
         return VK_ERROR_INCOMPATIBLE_DRIVER;

      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "failed to open device %s", path);
   }

   VkResult result = tu_knl_kgsl_load(instance, fd);
   if (result != VK_SUCCESS) {
      close(fd);
      return result;
   }

   if (TU_DEBUG(STARTUP))
      mesa_logi("Found compatible device '%s'.", path);

   return result;
#else
   return VK_ERROR_INCOMPATIBLE_DRIVER;
#endif
}

/**
 * Enumeration entrypoint for drm devices
 */
VkResult
tu_physical_device_try_create(struct vk_instance *vk_instance,
                              struct _drmDevice *drm_device,
                              struct vk_physical_device **out)
{
   struct tu_instance *instance =
      container_of(vk_instance, struct tu_instance, vk);

   /* Note that "msm" is a platform device, but "virtio_gpu" is a pci
    * device.  In general we shouldn't care about the bus type.
    */
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   const char *primary_path = drm_device->nodes[DRM_NODE_PRIMARY];
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   drmVersionPtr version;
   int fd;
   int master_fd = -1;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "failed to open device %s", path);
   }

   version = drmGetVersion(fd);
   if (!version) {
      close(fd);
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "failed to query kernel driver version for device %s",
                               path);
   }

   struct tu_physical_device *device = NULL;

   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   if (strcmp(version->name, "msm") == 0) {
#ifdef TU_HAS_MSM
      result = tu_knl_drm_msm_load(instance, fd, version, &device);
#endif
   } else if (strcmp(version->name, "virtio_gpu") == 0) {
#ifdef TU_HAS_VIRTIO
      result = tu_knl_drm_virtio_load(instance, fd, version, &device);
#endif
   } else if (TU_DEBUG(STARTUP)) {
      result = vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                                 "device %s (%s) is not compatible with turnip",
                                 path, version->name);
   }

   if (result != VK_SUCCESS)
      goto out;

   assert(device);

   if (instance->vk.enabled_extensions.KHR_display) {
      master_fd = open(primary_path, O_RDWR | O_CLOEXEC);
   }

   device->master_fd = master_fd;
   device->kgsl_dma_fd = -1;

   assert(strlen(path) < ARRAY_SIZE(device->fd_path));
   snprintf(device->fd_path, ARRAY_SIZE(device->fd_path), "%s", path);

   struct stat st;

   if (stat(primary_path, &st) == 0) {
      device->has_master = true;
      device->master_major = major(st.st_rdev);
      device->master_minor = minor(st.st_rdev);
   } else {
      device->has_master = false;
      device->master_major = 0;
      device->master_minor = 0;
   }

   if (stat(path, &st) == 0) {
      device->has_local = true;
      device->local_major = major(st.st_rdev);
      device->local_minor = minor(st.st_rdev);
   } else {
      result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                         "failed to stat DRM render node %s", path);
      goto out;
   }

   result = tu_physical_device_init(device, instance);
   if (result != VK_SUCCESS)
      goto out;

   if (TU_DEBUG(STARTUP))
      mesa_logi("Found compatible device '%s' (%s).", path, version->name);

   *out = &device->vk;

out:
   if (result != VK_SUCCESS) {
      if (master_fd != -1)
         close(master_fd);
      close(fd);
      vk_free(&instance->vk.alloc, device);
   }

   drmFreeVersion(version);

   return result;
}
