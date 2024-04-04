/*
 * Copyright © 2018 Google, Inc.
 * Copyright © 2015 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "tu_drm.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>

#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#include <sys/sysmacros.h>
#endif

#include "vk_util.h"

#include "drm-uapi/msm_drm.h"
#include "util/u_debug.h"
#include "util/hash_table.h"
#include "util/timespec.h"
#include "util/os_time.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_dynamic_rendering.h"

struct tu_queue_submit
{
   struct vk_queue_submit *vk_submit;
   struct tu_u_trace_submission_data *u_trace_submission_data;

   struct tu_cmd_buffer **cmd_buffers;
   struct drm_msm_gem_submit_cmd *cmds;
   struct drm_msm_gem_submit_syncobj *in_syncobjs;
   struct drm_msm_gem_submit_syncobj *out_syncobjs;

   uint32_t nr_cmd_buffers;
   uint32_t nr_in_syncobjs;
   uint32_t nr_out_syncobjs;
   uint32_t entry_count;
   uint32_t perf_pass_index;

   bool     autotune_fence;
};

struct tu_u_trace_syncobj
{
   uint32_t msm_queue_id;
   uint32_t fence;
};

static int
tu_drm_get_param(const struct tu_physical_device *dev,
                 uint32_t param,
                 uint64_t *value)
{
   /* Technically this requires a pipe, but the kernel only supports one pipe
    * anyway at the time of writing and most of these are clearly pipe
    * independent. */
   struct drm_msm_param req = {
      .pipe = MSM_PIPE_3D0,
      .param = param,
   };

   int ret = drmCommandWriteRead(dev->local_fd, DRM_MSM_GET_PARAM, &req,
                                 sizeof(req));
   if (ret)
      return ret;

   *value = req.value;

   return 0;
}

static int
tu_drm_get_gpu_id(const struct tu_physical_device *dev, uint32_t *id)
{
   uint64_t value;
   int ret = tu_drm_get_param(dev, MSM_PARAM_GPU_ID, &value);
   if (ret)
      return ret;

   *id = value;
   return 0;
}

static int
tu_drm_get_gmem_size(const struct tu_physical_device *dev, uint32_t *size)
{
   uint64_t value;
   int ret = tu_drm_get_param(dev, MSM_PARAM_GMEM_SIZE, &value);
   if (ret)
      return ret;

   *size = value;
   return 0;
}

static int
tu_drm_get_gmem_base(const struct tu_physical_device *dev, uint64_t *base)
{
   return tu_drm_get_param(dev, MSM_PARAM_GMEM_BASE, base);
}

static int
tu_drm_get_va_prop(const struct tu_physical_device *dev,
                   uint64_t *va_start, uint64_t *va_size)
{
   uint64_t value;
   int ret = tu_drm_get_param(dev, MSM_PARAM_VA_START, &value);
   if (ret)
      return ret;

   *va_start = value;

   ret = tu_drm_get_param(dev, MSM_PARAM_VA_SIZE, &value);
   if (ret)
      return ret;

   *va_size = value;

   return 0;
}

static uint32_t
tu_drm_get_priorities(const struct tu_physical_device *dev)
{
   uint64_t val = 1;
   tu_drm_get_param(dev, MSM_PARAM_PRIORITIES, &val);
   assert(val >= 1);

   return val;
}

int
tu_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)
{
   return tu_drm_get_param(dev->physical_device, MSM_PARAM_TIMESTAMP, ts);
}

int
tu_device_get_suspend_count(struct tu_device *dev, uint64_t *suspend_count)
{
   int ret = tu_drm_get_param(dev->physical_device, MSM_PARAM_SUSPENDS, suspend_count);
   return ret;
}

VkResult
tu_device_check_status(struct vk_device *vk_device)
{
   struct tu_device *device = container_of(vk_device, struct tu_device, vk);
   struct tu_physical_device *physical_device = device->physical_device;

   uint64_t last_fault_count = physical_device->fault_count;
   int ret = tu_drm_get_param(physical_device, MSM_PARAM_FAULTS, &physical_device->fault_count);
   if (ret != 0)
      return vk_device_set_lost(&device->vk, "error getting GPU fault count: %d", ret);

   if (last_fault_count != physical_device->fault_count)
      return vk_device_set_lost(&device->vk, "GPU faulted or hung");

   return VK_SUCCESS;
}

int
tu_drm_submitqueue_new(const struct tu_device *dev,
                       int priority,
                       uint32_t *queue_id)
{
   assert(priority >= 0 &&
          priority < dev->physical_device->submitqueue_priority_count);
   struct drm_msm_submitqueue req = {
      .flags = 0,
      .prio = priority,
   };

   int ret = drmCommandWriteRead(dev->fd,
                                 DRM_MSM_SUBMITQUEUE_NEW, &req, sizeof(req));
   if (ret)
      return ret;

   *queue_id = req.id;
   return 0;
}

void
tu_drm_submitqueue_close(const struct tu_device *dev, uint32_t queue_id)
{
   drmCommandWrite(dev->fd, DRM_MSM_SUBMITQUEUE_CLOSE,
                   &queue_id, sizeof(uint32_t));
}

static void
tu_gem_close(const struct tu_device *dev, uint32_t gem_handle)
{
   struct drm_gem_close req = {
      .handle = gem_handle,
   };

   drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
}

/** Helper for DRM_MSM_GEM_INFO, returns 0 on error. */
static uint64_t
tu_gem_info(const struct tu_device *dev, uint32_t gem_handle, uint32_t info)
{
   struct drm_msm_gem_info req = {
      .handle = gem_handle,
      .info = info,
   };

   int ret = drmCommandWriteRead(dev->fd,
                                 DRM_MSM_GEM_INFO, &req, sizeof(req));
   if (ret < 0)
      return 0;

   return req.value;
}


static VkResult
tu_allocate_userspace_iova(struct tu_device *dev,
                           uint32_t gem_handle,
                           uint64_t size,
                           uint64_t client_iova,
                           enum tu_bo_alloc_flags flags,
                           uint64_t *iova)
{
   mtx_lock(&dev->physical_device->vma_mutex);

   *iova = 0;

   if (flags & TU_BO_ALLOC_REPLAYABLE) {
      if (client_iova) {
         if (util_vma_heap_alloc_addr(&dev->physical_device->vma, client_iova,
                                      size)) {
            *iova = client_iova;
         } else {
            return VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS;
         }
      } else {
         /* We have to separate replayable IOVAs from ordinary one in order to
          * for them not to clash. The easiest way to do this is to allocate
          * them from the other end of the address space.
          */
         dev->physical_device->vma.alloc_high = true;
         *iova =
            util_vma_heap_alloc(&dev->physical_device->vma, size, 0x1000);
      }
   } else {
      dev->physical_device->vma.alloc_high = false;
      *iova = util_vma_heap_alloc(&dev->physical_device->vma, size, 0x1000);
   }

   mtx_unlock(&dev->physical_device->vma_mutex);

   if (!*iova)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   struct drm_msm_gem_info req = {
      .handle = gem_handle,
      .info = MSM_INFO_SET_IOVA,
      .value = *iova,
   };

   int ret =
      drmCommandWriteRead(dev->fd, DRM_MSM_GEM_INFO, &req, sizeof(req));
   if (ret < 0)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   return VK_SUCCESS;
}

static VkResult
tu_allocate_kernel_iova(struct tu_device *dev,
                        uint32_t gem_handle,
                        uint64_t *iova)
{
   *iova = tu_gem_info(dev, gem_handle, MSM_INFO_GET_IOVA);
   if (!*iova)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   return VK_SUCCESS;
}

static VkResult
tu_bo_init(struct tu_device *dev,
           struct tu_bo *bo,
           uint32_t gem_handle,
           uint64_t size,
           uint64_t client_iova,
           enum tu_bo_alloc_flags flags,
           const char *name)
{
   VkResult result = VK_SUCCESS;
   uint64_t iova = 0;

   assert(!client_iova || dev->physical_device->has_set_iova);

   if (dev->physical_device->has_set_iova) {
      result = tu_allocate_userspace_iova(dev, gem_handle, size, client_iova,
                                          flags, &iova);
   } else {
      result = tu_allocate_kernel_iova(dev, gem_handle, &iova);
   }

   if (result != VK_SUCCESS)
      goto fail_bo_list;

   name = tu_debug_bos_add(dev, size, name);

   mtx_lock(&dev->bo_mutex);
   uint32_t idx = dev->bo_count++;

   /* grow the bo list if needed */
   if (idx >= dev->bo_list_size) {
      uint32_t new_len = idx + 64;
      struct drm_msm_gem_submit_bo *new_ptr =
         vk_realloc(&dev->vk.alloc, dev->bo_list, new_len * sizeof(*dev->bo_list),
                    8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!new_ptr) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail_bo_list;
      }

      dev->bo_list = new_ptr;
      dev->bo_list_size = new_len;
   }

   bool dump = flags & TU_BO_ALLOC_ALLOW_DUMP;
   dev->bo_list[idx] = (struct drm_msm_gem_submit_bo) {
      .flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE |
               COND(dump, MSM_SUBMIT_BO_DUMP),
      .handle = gem_handle,
      .presumed = iova,
   };

   *bo = (struct tu_bo) {
      .gem_handle = gem_handle,
      .size = size,
      .iova = iova,
      .refcnt = 1,
      .bo_list_idx = idx,
      .name = name,
   };

   mtx_unlock(&dev->bo_mutex);

   return VK_SUCCESS;

fail_bo_list:
   tu_gem_close(dev, gem_handle);
   return result;
}

/**
 * Sets the name in the kernel so that the contents of /debug/dri/0/gem are more
 * useful.
 *
 * We skip this on release builds (when we're also not doing BO debugging) to
 * reduce overhead.
 */
static void
tu_bo_set_kernel_name(struct tu_device *dev, struct tu_bo *bo, const char *name)
{
   bool kernel_bo_names = dev->bo_sizes != NULL;
#ifdef DEBUG
   kernel_bo_names = true;
#endif
   if (!kernel_bo_names)
      return;

   struct drm_msm_gem_info req = {
      .handle = bo->gem_handle,
      .info = MSM_INFO_SET_NAME,
      .value = (uintptr_t)(void *)name,
      .len = strlen(name),
   };

   int ret = drmCommandWrite(dev->fd, DRM_MSM_GEM_INFO, &req, sizeof(req));
   if (ret) {
      mesa_logw_once("Failed to set BO name with DRM_MSM_GEM_INFO: %d",
                     ret);
   }
}

VkResult
tu_bo_init_new_explicit_iova(struct tu_device *dev,
                             struct tu_bo **out_bo,
                             uint64_t size,
                             uint64_t client_iova,
                             enum tu_bo_alloc_flags flags,
                             const char *name)
{
   /* TODO: Choose better flags. As of 2018-11-12, freedreno/drm/msm_bo.c
    * always sets `flags = MSM_BO_WC`, and we copy that behavior here.
    */
   struct drm_msm_gem_new req = {
      .size = size,
      .flags = MSM_BO_WC
   };

   if (flags & TU_BO_ALLOC_GPU_READ_ONLY)
      req.flags |= MSM_BO_GPU_READONLY;

   int ret = drmCommandWriteRead(dev->fd,
                                 DRM_MSM_GEM_NEW, &req, sizeof(req));
   if (ret)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct tu_bo* bo = tu_device_lookup_bo(dev, req.handle);
   assert(bo && bo->gem_handle == 0);

   VkResult result =
      tu_bo_init(dev, bo, req.handle, size, client_iova, flags, name);

   if (result != VK_SUCCESS)
      memset(bo, 0, sizeof(*bo));
   else
      *out_bo = bo;

   /* We don't use bo->name here because for the !TU_DEBUG=bo case bo->name is NULL. */
   tu_bo_set_kernel_name(dev, bo, name);

   return result;
}

VkResult
tu_bo_init_dmabuf(struct tu_device *dev,
                  struct tu_bo **out_bo,
                  uint64_t size,
                  int prime_fd)
{
   /* lseek() to get the real size */
   off_t real_size = lseek(prime_fd, 0, SEEK_END);
   lseek(prime_fd, 0, SEEK_SET);
   if (real_size < 0 || (uint64_t) real_size < size)
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   /* Importing the same dmabuf several times would yield the same
    * gem_handle. Thus there could be a race when destroying
    * BO and importing the same dmabuf from different threads.
    * We must not permit the creation of dmabuf BO and its release
    * to happen in parallel.
    */
   u_rwlock_wrlock(&dev->dma_bo_lock);

   uint32_t gem_handle;
   int ret = drmPrimeFDToHandle(dev->fd, prime_fd,
                                &gem_handle);
   if (ret) {
      u_rwlock_wrunlock(&dev->dma_bo_lock);
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   struct tu_bo* bo = tu_device_lookup_bo(dev, gem_handle);

   if (bo->refcnt != 0) {
      p_atomic_inc(&bo->refcnt);
      u_rwlock_wrunlock(&dev->dma_bo_lock);

      *out_bo = bo;
      return VK_SUCCESS;
   }

   VkResult result =
      tu_bo_init(dev, bo, gem_handle, size, 0, TU_BO_ALLOC_NO_FLAGS, "dmabuf");

   if (result != VK_SUCCESS)
      memset(bo, 0, sizeof(*bo));
   else
      *out_bo = bo;

   u_rwlock_wrunlock(&dev->dma_bo_lock);

   return result;
}

int
tu_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   int prime_fd;
   int ret = drmPrimeHandleToFD(dev->fd, bo->gem_handle,
                                DRM_CLOEXEC | DRM_RDWR, &prime_fd);

   return ret == 0 ? prime_fd : -1;
}

VkResult
tu_bo_map(struct tu_device *dev, struct tu_bo *bo)
{
   if (bo->map)
      return VK_SUCCESS;

   uint64_t offset = tu_gem_info(dev, bo->gem_handle, MSM_INFO_GET_OFFSET);
   if (!offset)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   /* TODO: Should we use the wrapper os_mmap() like Freedreno does? */
   void *map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    dev->fd, offset);
   if (map == MAP_FAILED)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   bo->map = map;
   return VK_SUCCESS;
}

void
tu_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   u_rwlock_rdlock(&dev->dma_bo_lock);

   if (!p_atomic_dec_zero(&bo->refcnt)) {
      u_rwlock_rdunlock(&dev->dma_bo_lock);
      return;
   }

   if (bo->map)
      munmap(bo->map, bo->size);

   tu_debug_bos_del(dev, bo);

   mtx_lock(&dev->bo_mutex);
   dev->bo_count--;
   dev->bo_list[bo->bo_list_idx] = dev->bo_list[dev->bo_count];

   struct tu_bo* exchanging_bo = tu_device_lookup_bo(dev, dev->bo_list[bo->bo_list_idx].handle);
   exchanging_bo->bo_list_idx = bo->bo_list_idx;

   if (bo->implicit_sync)
      dev->implicit_sync_bo_count--;

   mtx_unlock(&dev->bo_mutex);

   if (dev->physical_device->has_set_iova) {
      mtx_lock(&dev->physical_device->vma_mutex);
      util_vma_heap_free(&dev->physical_device->vma, bo->iova, bo->size);
      mtx_unlock(&dev->physical_device->vma_mutex);
   }

   /* Our BO structs are stored in a sparse array in the physical device,
    * so we don't want to free the BO pointer, instead we want to reset it
    * to 0, to signal that array entry as being free.
    */
   uint32_t gem_handle = bo->gem_handle;
   memset(bo, 0, sizeof(*bo));

   tu_gem_close(dev, gem_handle);

   u_rwlock_rdunlock(&dev->dma_bo_lock);
}

extern const struct vk_sync_type tu_timeline_sync_type;

static inline bool
vk_sync_is_tu_timeline_sync(const struct vk_sync *sync)
{
   return sync->type == &tu_timeline_sync_type;
}

static struct tu_timeline_sync *
to_tu_timeline_sync(struct vk_sync *sync)
{
   assert(sync->type == &tu_timeline_sync_type);
   return container_of(sync, struct tu_timeline_sync, base);
}

static uint32_t
tu_syncobj_from_vk_sync(struct vk_sync *sync)
{
   uint32_t syncobj = -1;
   if (vk_sync_is_tu_timeline_sync(sync)) {
      syncobj = to_tu_timeline_sync(sync)->syncobj;
   } else if (vk_sync_type_is_drm_syncobj(sync->type)) {
      syncobj = vk_sync_as_drm_syncobj(sync)->syncobj;
   }

   assert(syncobj != -1);

   return syncobj;
}

static VkResult
tu_timeline_sync_init(struct vk_device *vk_device,
                      struct vk_sync *vk_sync,
                      uint64_t initial_value)
{
   struct tu_device *device = container_of(vk_device, struct tu_device, vk);
   struct tu_timeline_sync *sync = to_tu_timeline_sync(vk_sync);
   uint32_t flags = 0;

   assert(device->fd >= 0);

   int err = drmSyncobjCreate(device->fd, flags, &sync->syncobj);

   if (err < 0) {
        return vk_error(device, VK_ERROR_DEVICE_LOST);
   }

   sync->state = initial_value ? TU_TIMELINE_SYNC_STATE_SIGNALED :
                                    TU_TIMELINE_SYNC_STATE_RESET;

   return VK_SUCCESS;
}

static void
tu_timeline_sync_finish(struct vk_device *vk_device,
                   struct vk_sync *vk_sync)
{
   struct tu_device *dev = container_of(vk_device, struct tu_device, vk);
   struct tu_timeline_sync *sync = to_tu_timeline_sync(vk_sync);

   assert(dev->fd >= 0);
   ASSERTED int err = drmSyncobjDestroy(dev->fd, sync->syncobj);
   assert(err == 0);
}

static VkResult
tu_timeline_sync_reset(struct vk_device *vk_device,
                  struct vk_sync *vk_sync)
{
   struct tu_device *dev = container_of(vk_device, struct tu_device, vk);
   struct tu_timeline_sync *sync = to_tu_timeline_sync(vk_sync);

   int err = drmSyncobjReset(dev->fd, &sync->syncobj, 1);
   if (err) {
      return vk_errorf(dev, VK_ERROR_UNKNOWN,
                       "DRM_IOCTL_SYNCOBJ_RESET failed: %m");
   } else {
       sync->state = TU_TIMELINE_SYNC_STATE_RESET;
   }

   return VK_SUCCESS;
}

static VkResult
drm_syncobj_wait(struct tu_device *device,
                 uint32_t *handles, uint32_t count_handles,
                 uint64_t timeout_nsec, bool wait_all)
{
   uint32_t syncobj_wait_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   if (wait_all) syncobj_wait_flags |= DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

   /* syncobj absolute timeouts are signed.  clamp OS_TIMEOUT_INFINITE down. */
   timeout_nsec = MIN2(timeout_nsec, (uint64_t)INT64_MAX);

   int err = drmSyncobjWait(device->fd, handles,
                            count_handles, timeout_nsec,
                            syncobj_wait_flags,
                            NULL /* first_signaled */);
   if (err && errno == ETIME) {
      return VK_TIMEOUT;
   } else if (err) {
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "DRM_IOCTL_SYNCOBJ_WAIT failed: %m");
   }

   return VK_SUCCESS;
}

/* Based on anv_bo_sync_wait */
static VkResult
tu_timeline_sync_wait(struct vk_device *vk_device,
                 uint32_t wait_count,
                 const struct vk_sync_wait *waits,
                 enum vk_sync_wait_flags wait_flags,
                 uint64_t abs_timeout_ns)
{
   struct tu_device *dev = container_of(vk_device, struct tu_device, vk);
   bool wait_all = !(wait_flags & VK_SYNC_WAIT_ANY);

   uint32_t handles[wait_count];
   uint32_t submit_count;
   VkResult ret = VK_SUCCESS;
   uint32_t pending = wait_count;
   struct tu_timeline_sync *submitted_syncs[wait_count];

   while (pending) {
      pending = 0;
      submit_count = 0;

      for (unsigned i = 0; i < wait_count; ++i) {
         struct tu_timeline_sync *sync = to_tu_timeline_sync(waits[i].sync);

         if (sync->state == TU_TIMELINE_SYNC_STATE_RESET) {
            assert(!(wait_flags & VK_SYNC_WAIT_PENDING));
            pending++;
         } else if (sync->state == TU_TIMELINE_SYNC_STATE_SIGNALED) {
            if (wait_flags & VK_SYNC_WAIT_ANY)
               return VK_SUCCESS;
         } else if (sync->state == TU_TIMELINE_SYNC_STATE_SUBMITTED) {
            if (!(wait_flags & VK_SYNC_WAIT_PENDING)) {
               handles[submit_count] = sync->syncobj;
               submitted_syncs[submit_count++] = sync;
            }
         }
      }

      if (submit_count > 0) {
         do {
            ret = drm_syncobj_wait(dev, handles, submit_count, abs_timeout_ns, wait_all);
         } while (ret == VK_TIMEOUT && os_time_get_nano() < abs_timeout_ns);

         if (ret == VK_SUCCESS) {
            for (unsigned i = 0; i < submit_count; ++i) {
               struct tu_timeline_sync *sync = submitted_syncs[i];
               sync->state = TU_TIMELINE_SYNC_STATE_SIGNALED;
            }
         } else {
            /* return error covering timeout */
            return ret;
         }
      } else if (pending > 0) {
         /* If we've hit this then someone decided to vkWaitForFences before
          * they've actually submitted any of them to a queue.  This is a
          * fairly pessimal case, so it's ok to lock here and use a standard
          * pthreads condition variable.
          */
         pthread_mutex_lock(&dev->submit_mutex);

         /* It's possible that some of the fences have changed state since the
          * last time we checked.  Now that we have the lock, check for
          * pending fences again and don't wait if it's changed.
          */
         uint32_t now_pending = 0;
         for (uint32_t i = 0; i < wait_count; i++) {
            struct tu_timeline_sync *sync = to_tu_timeline_sync(waits[i].sync);
            if (sync->state == TU_TIMELINE_SYNC_STATE_RESET)
               now_pending++;
         }
         assert(now_pending <= pending);

         if (now_pending == pending) {
            struct timespec abstime = {
               .tv_sec = abs_timeout_ns / NSEC_PER_SEC,
               .tv_nsec = abs_timeout_ns % NSEC_PER_SEC,
            };

            ASSERTED int ret;
            ret = pthread_cond_timedwait(&dev->timeline_cond,
                                         &dev->submit_mutex, &abstime);
            assert(ret != EINVAL);
            if (os_time_get_nano() >= abs_timeout_ns) {
               pthread_mutex_unlock(&dev->submit_mutex);
               return VK_TIMEOUT;
            }
         }

         pthread_mutex_unlock(&dev->submit_mutex);
      }
   }

   return ret;
}

const struct vk_sync_type tu_timeline_sync_type = {
   .size = sizeof(struct tu_timeline_sync),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = tu_timeline_sync_init,
   .finish = tu_timeline_sync_finish,
   .reset = tu_timeline_sync_reset,
   .wait_many = tu_timeline_sync_wait,
};

VkResult
tu_physical_device_try_create(struct vk_instance *vk_instance,
                              struct _drmDevice *drm_device,
                              struct vk_physical_device **out)
{
   struct tu_instance *instance =
      container_of(vk_instance, struct tu_instance, vk);

   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PLATFORM)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   const char *primary_path = drm_device->nodes[DRM_NODE_PRIMARY];
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result = VK_SUCCESS;
   drmVersionPtr version;
   int fd;
   int master_fd = -1;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "failed to open device %s", path);
   }

   /* Version 1.6 added SYNCOBJ support. */
   const int min_version_major = 1;
   const int min_version_minor = 6;

   version = drmGetVersion(fd);
   if (!version) {
      close(fd);
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "failed to query kernel driver version for device %s",
                               path);
   }

   if (strcmp(version->name, "msm")) {
      drmFreeVersion(version);
      close(fd);
      return vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                               "device %s does not use the msm kernel driver",
                               path);
   }

   if (version->version_major != min_version_major ||
       version->version_minor < min_version_minor) {
      result = vk_startup_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                                 "kernel driver for device %s has version %d.%d, "
                                 "but Vulkan requires version >= %d.%d",
                                 path,
                                 version->version_major, version->version_minor,
                                 min_version_major, min_version_minor);
      drmFreeVersion(version);
      close(fd);
      return result;
   }

   struct tu_physical_device *device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device) {
      result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      drmFreeVersion(version);
      goto fail;
   }

   device->msm_major_version = version->version_major;
   device->msm_minor_version = version->version_minor;

   drmFreeVersion(version);

   if (instance->debug_flags & TU_DEBUG_STARTUP)
      mesa_logi("Found compatible device '%s'.", path);

   device->instance = instance;

   if (instance->vk.enabled_extensions.KHR_display) {
      master_fd = open(primary_path, O_RDWR | O_CLOEXEC);
      if (master_fd >= 0) {
         /* TODO: free master_fd is accel is not working? */
      }
   }

   device->master_fd = master_fd;
   device->local_fd = fd;

   if (tu_drm_get_gpu_id(device, &device->dev_id.gpu_id)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "could not get GPU ID");
      goto fail;
   }

   if (tu_drm_get_param(device, MSM_PARAM_CHIP_ID, &device->dev_id.chip_id)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "could not get CHIP ID");
      goto fail;
   }

   if (tu_drm_get_gmem_size(device, &device->gmem_size)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                "could not get GMEM size");
      goto fail;
   }
   device->gmem_size = debug_get_num_option("TU_GMEM", device->gmem_size);

   if (tu_drm_get_gmem_base(device, &device->gmem_base)) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "could not get GMEM size");
      goto fail;
   }

   /*
    * device->has_set_iova = !tu_drm_get_va_prop(device, &device->va_start,
    *                                            &device->va_size);
    *
    * If BO is freed while kernel considers it busy, our VMA state gets
    * desynchronized from kernel's VMA state, because kernel waits
    * until BO stops being busy. And whether BO is busy decided at
    * submission granularity.
    *
    * Disable this capability until solution is found.
    */
   device->has_set_iova = false;

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
      goto fail;
   }

   int ret = tu_drm_get_param(device, MSM_PARAM_FAULTS, &device->fault_count);
   if (ret != 0) {
      result = vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                                 "Failed to get initial fault count: %d", ret);
      goto fail;
   }

   device->submitqueue_priority_count = tu_drm_get_priorities(device);

   device->syncobj_type = vk_drm_syncobj_get_type(fd);
   /* we don't support DRM_CAP_SYNCOBJ_TIMELINE, but drm-shim does */
   if (!(device->syncobj_type.features & VK_SYNC_FEATURE_TIMELINE))
      device->timeline_type = vk_sync_timeline_get_type(&tu_timeline_sync_type);

   device->sync_types[0] = &device->syncobj_type;
   device->sync_types[1] = &device->timeline_type.sync;
   device->sync_types[2] = NULL;

   device->heap.size = tu_get_system_heap_size();
   device->heap.used = 0u;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   result = tu_physical_device_init(device, instance);

   if (result == VK_SUCCESS) {
      *out = &device->vk;
      return result;
   }

fail:
   if (device)
      vk_free(&instance->vk.alloc, device);
   close(fd);
   if (master_fd != -1)
      close(master_fd);
   return result;
}

static VkResult
tu_queue_submit_create_locked(struct tu_queue *queue,
                              struct vk_queue_submit *vk_submit,
                              const uint32_t nr_in_syncobjs,
                              const uint32_t nr_out_syncobjs,
                              uint32_t perf_pass_index,
                              struct tu_queue_submit *new_submit)
{
   VkResult result;

   bool u_trace_enabled = u_trace_should_process(&queue->device->trace_context);
   bool has_trace_points = false;

   struct vk_command_buffer **vk_cmd_buffers = vk_submit->command_buffers;

   memset(new_submit, 0, sizeof(struct tu_queue_submit));

   new_submit->cmd_buffers = (void *)vk_cmd_buffers;
   new_submit->nr_cmd_buffers = vk_submit->command_buffer_count;
   tu_insert_dynamic_cmdbufs(queue->device, &new_submit->cmd_buffers,
                             &new_submit->nr_cmd_buffers);

   uint32_t entry_count = 0;
   for (uint32_t j = 0; j < new_submit->nr_cmd_buffers; ++j) {
      struct tu_cmd_buffer *cmdbuf = new_submit->cmd_buffers[j];

      if (perf_pass_index != ~0)
         entry_count++;

      entry_count += cmdbuf->cs.entry_count;

      if (u_trace_enabled && u_trace_has_points(&cmdbuf->trace)) {
         if (!(cmdbuf->usage_flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))
            entry_count++;

         has_trace_points = true;
      }
   }

   new_submit->autotune_fence =
      tu_autotune_submit_requires_fence(new_submit->cmd_buffers, new_submit->nr_cmd_buffers);
   if (new_submit->autotune_fence)
      entry_count++;

   new_submit->cmds = vk_zalloc(&queue->device->vk.alloc,
         entry_count * sizeof(*new_submit->cmds), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (new_submit->cmds == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_cmds;
   }

   if (has_trace_points) {
      result =
         tu_u_trace_submission_data_create(
            queue->device, new_submit->cmd_buffers,
            new_submit->nr_cmd_buffers,
            &new_submit->u_trace_submission_data);

      if (result != VK_SUCCESS) {
         goto fail_u_trace_submission_data;
      }
   }

   /* Allocate without wait timeline semaphores */
   new_submit->in_syncobjs = vk_zalloc(&queue->device->vk.alloc,
         nr_in_syncobjs * sizeof(*new_submit->in_syncobjs), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (new_submit->in_syncobjs == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_in_syncobjs;
   }

   /* Allocate with signal timeline semaphores considered */
   new_submit->out_syncobjs = vk_zalloc(&queue->device->vk.alloc,
         nr_out_syncobjs * sizeof(*new_submit->out_syncobjs), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   if (new_submit->out_syncobjs == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_out_syncobjs;
   }

   new_submit->entry_count = entry_count;
   new_submit->nr_in_syncobjs = nr_in_syncobjs;
   new_submit->nr_out_syncobjs = nr_out_syncobjs;
   new_submit->perf_pass_index = perf_pass_index;
   new_submit->vk_submit = vk_submit;

   return VK_SUCCESS;

fail_out_syncobjs:
   vk_free(&queue->device->vk.alloc, new_submit->in_syncobjs);
fail_in_syncobjs:
   if (new_submit->u_trace_submission_data)
      tu_u_trace_submission_data_finish(queue->device,
                                        new_submit->u_trace_submission_data);
fail_u_trace_submission_data:
   vk_free(&queue->device->vk.alloc, new_submit->cmds);
fail_cmds:
   return result;
}

static void
tu_queue_submit_finish(struct tu_queue *queue, struct tu_queue_submit *submit)
{
   vk_free(&queue->device->vk.alloc, submit->cmds);
   vk_free(&queue->device->vk.alloc, submit->in_syncobjs);
   vk_free(&queue->device->vk.alloc, submit->out_syncobjs);
   if (submit->cmd_buffers != (void *) submit->vk_submit->command_buffers)
      vk_free(&queue->device->vk.alloc, submit->cmd_buffers);
}

static void
tu_fill_msm_gem_submit(struct tu_device *dev,
                       struct drm_msm_gem_submit_cmd *cmd,
                       struct tu_cs_entry *cs_entry)
{
   cmd->type = MSM_SUBMIT_CMD_BUF;
   cmd->submit_idx = cs_entry->bo->bo_list_idx;
   cmd->submit_offset = cs_entry->offset;
   cmd->size = cs_entry->size;
   cmd->pad = 0;
   cmd->nr_relocs = 0;
   cmd->relocs = 0;
}

static void
tu_queue_build_msm_gem_submit_cmds(struct tu_queue *queue,
                                   struct tu_queue_submit *submit,
                                   struct tu_cs *autotune_cs)
{
   struct tu_device *dev = queue->device;
   struct drm_msm_gem_submit_cmd *cmds = submit->cmds;

   uint32_t entry_idx = 0;
   for (uint32_t j = 0; j < submit->nr_cmd_buffers; ++j) {
      struct tu_device *dev = queue->device;
      struct tu_cmd_buffer *cmdbuf = submit->cmd_buffers[j];
      struct tu_cs *cs = &cmdbuf->cs;

      if (submit->perf_pass_index != ~0) {
         struct tu_cs_entry *perf_cs_entry =
            &dev->perfcntrs_pass_cs_entries[submit->perf_pass_index];

         tu_fill_msm_gem_submit(dev, &cmds[entry_idx], perf_cs_entry);
         entry_idx++;
      }

      for (unsigned i = 0; i < cs->entry_count; ++i, ++entry_idx) {
         tu_fill_msm_gem_submit(dev, &cmds[entry_idx], &cs->entries[i]);
      }

      if (submit->u_trace_submission_data) {
         struct tu_cs *ts_cs =
            submit->u_trace_submission_data->cmd_trace_data[j].timestamp_copy_cs;
         if (ts_cs) {
            tu_fill_msm_gem_submit(dev, &cmds[entry_idx], &ts_cs->entries[0]);
            entry_idx++;
         }
      }
   }

   if (autotune_cs) {
      assert(autotune_cs->entry_count == 1);
      tu_fill_msm_gem_submit(dev, &cmds[entry_idx], &autotune_cs->entries[0]);
      entry_idx++;
   }
}

static VkResult
tu_queue_submit_locked(struct tu_queue *queue, struct tu_queue_submit *submit)
{
   queue->device->submit_count++;

   struct tu_cs *autotune_cs = NULL;
   if (submit->autotune_fence) {
      autotune_cs = tu_autotune_on_submit(queue->device,
                                          &queue->device->autotune,
                                          submit->cmd_buffers,
                                          submit->nr_cmd_buffers);
   }

   uint32_t flags = MSM_PIPE_3D0;

   if (submit->vk_submit->wait_count)
      flags |= MSM_SUBMIT_SYNCOBJ_IN;

   if (submit->vk_submit->signal_count)
      flags |= MSM_SUBMIT_SYNCOBJ_OUT;

   mtx_lock(&queue->device->bo_mutex);

   if (queue->device->implicit_sync_bo_count == 0)
      flags |= MSM_SUBMIT_NO_IMPLICIT;

   /* drm_msm_gem_submit_cmd requires index of bo which could change at any
    * time when bo_mutex is not locked. So we build submit cmds here the real
    * place to submit.
    */
   tu_queue_build_msm_gem_submit_cmds(queue, submit, autotune_cs);

   struct drm_msm_gem_submit req = {
      .flags = flags,
      .queueid = queue->msm_queue_id,
      .bos = (uint64_t)(uintptr_t) queue->device->bo_list,
      .nr_bos = submit->entry_count ? queue->device->bo_count : 0,
      .cmds = (uint64_t)(uintptr_t)submit->cmds,
      .nr_cmds = submit->entry_count,
      .in_syncobjs = (uint64_t)(uintptr_t)submit->in_syncobjs,
      .out_syncobjs = (uint64_t)(uintptr_t)submit->out_syncobjs,
      .nr_in_syncobjs = submit->nr_in_syncobjs,
      .nr_out_syncobjs = submit->nr_out_syncobjs,
      .syncobj_stride = sizeof(struct drm_msm_gem_submit_syncobj),
   };

   int ret = drmCommandWriteRead(queue->device->fd,
                                 DRM_MSM_GEM_SUBMIT,
                                 &req, sizeof(req));

   mtx_unlock(&queue->device->bo_mutex);

   tu_debug_bos_print_stats(queue->device);

   if (ret)
      return vk_device_set_lost(&queue->device->vk, "submit failed: %m");

#if HAVE_PERFETTO
   tu_perfetto_submit(queue->device, queue->device->submit_count);
#endif

   if (submit->u_trace_submission_data) {
      struct tu_u_trace_submission_data *submission_data =
         submit->u_trace_submission_data;
      submission_data->submission_id = queue->device->submit_count;
      /* We have to allocate it here since it is different between drm/kgsl */
      submission_data->syncobj =
         vk_alloc(&queue->device->vk.alloc, sizeof(struct tu_u_trace_syncobj),
               8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      submission_data->syncobj->fence = req.fence;
      submission_data->syncobj->msm_queue_id = queue->msm_queue_id;

      submit->u_trace_submission_data = NULL;

      for (uint32_t i = 0; i < submission_data->cmd_buffer_count; i++) {
         bool free_data = i == submission_data->last_buffer_with_tracepoints;
         if (submission_data->cmd_trace_data[i].trace)
            u_trace_flush(submission_data->cmd_trace_data[i].trace,
                          submission_data, free_data);

         if (!submission_data->cmd_trace_data[i].timestamp_copy_cs) {
            /* u_trace is owned by cmd_buffer */
            submission_data->cmd_trace_data[i].trace = NULL;
         }
      }
   }

   for (uint32_t i = 0; i < submit->vk_submit->wait_count; i++) {
      if (!vk_sync_is_tu_timeline_sync(submit->vk_submit->waits[i].sync))
         continue;

      struct tu_timeline_sync *sync =
         container_of(submit->vk_submit->waits[i].sync, struct tu_timeline_sync, base);

      assert(sync->state != TU_TIMELINE_SYNC_STATE_RESET);

      /* Set SIGNALED to the state of the wait timeline sync since this means the syncobj
       * is done and ready again so this can be garbage-collectioned later.
       */
      sync->state = TU_TIMELINE_SYNC_STATE_SIGNALED;
   }

   for (uint32_t i = 0; i < submit->vk_submit->signal_count; i++) {
      if (!vk_sync_is_tu_timeline_sync(submit->vk_submit->signals[i].sync))
         continue;

      struct tu_timeline_sync *sync =
         container_of(submit->vk_submit->signals[i].sync, struct tu_timeline_sync, base);

      assert(sync->state == TU_TIMELINE_SYNC_STATE_RESET);
      /* Set SUBMITTED to the state of the signal timeline sync so we could wait for
       * this timeline sync until completed if necessary.
       */
      sync->state = TU_TIMELINE_SYNC_STATE_SUBMITTED;
   }

   pthread_cond_broadcast(&queue->device->timeline_cond);

   return VK_SUCCESS;
}

static inline void
get_abs_timeout(struct drm_msm_timespec *tv, uint64_t ns)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   tv->tv_sec = t.tv_sec + ns / 1000000000;
   tv->tv_nsec = t.tv_nsec + ns % 1000000000;
}

VkResult
tu_device_wait_u_trace(struct tu_device *dev, struct tu_u_trace_syncobj *syncobj)
{
   struct drm_msm_wait_fence req = {
      .fence = syncobj->fence,
      .queueid = syncobj->msm_queue_id,
   };
   int ret;

   get_abs_timeout(&req.timeout, 1000000000);

   ret = drmCommandWrite(dev->fd, DRM_MSM_WAIT_FENCE, &req, sizeof(req));
   if (ret && (ret != -ETIMEDOUT)) {
      fprintf(stderr, "wait-fence failed! %d (%s)", ret, strerror(errno));
      return VK_TIMEOUT;
   }

   return VK_SUCCESS;
}

VkResult
tu_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   MESA_TRACE_FUNC();
   struct tu_queue *queue = container_of(vk_queue, struct tu_queue, vk);
   uint32_t perf_pass_index = queue->device->perfcntrs_pass_cs ?
                              submit->perf_pass_index : ~0;
   struct tu_queue_submit submit_req;

   if (unlikely(queue->device->physical_device->instance->debug_flags &
                 TU_DEBUG_LOG_SKIP_GMEM_OPS)) {
      tu_dbg_log_gmem_load_store_skips(queue->device);
   }

   pthread_mutex_lock(&queue->device->submit_mutex);

   VkResult ret = tu_queue_submit_create_locked(queue, submit,
         submit->wait_count, submit->signal_count,
         perf_pass_index, &submit_req);

   if (ret != VK_SUCCESS) {
      pthread_mutex_unlock(&queue->device->submit_mutex);
      return ret;
   }

   /* note: assuming there won't be any very large semaphore counts */
   struct drm_msm_gem_submit_syncobj *in_syncobjs = submit_req.in_syncobjs;
   struct drm_msm_gem_submit_syncobj *out_syncobjs = submit_req.out_syncobjs;

   uint32_t nr_in_syncobjs = 0, nr_out_syncobjs = 0;

   for (uint32_t i = 0; i < submit->wait_count; i++) {
      struct vk_sync *sync = submit->waits[i].sync;

      in_syncobjs[nr_in_syncobjs++] = (struct drm_msm_gem_submit_syncobj) {
         .handle = tu_syncobj_from_vk_sync(sync),
         .flags = 0,
      };
   }

   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct vk_sync *sync = submit->signals[i].sync;

      out_syncobjs[nr_out_syncobjs++] = (struct drm_msm_gem_submit_syncobj) {
         .handle = tu_syncobj_from_vk_sync(sync),
         .flags = 0,
      };
   }

   ret = tu_queue_submit_locked(queue, &submit_req);

   pthread_mutex_unlock(&queue->device->submit_mutex);
   tu_queue_submit_finish(queue, &submit_req);

   if (ret != VK_SUCCESS)
       return ret;

   u_trace_context_process(&queue->device->trace_context, true);

   return VK_SUCCESS;
}
