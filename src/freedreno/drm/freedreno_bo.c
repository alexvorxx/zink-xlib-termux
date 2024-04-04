/*
 * Copyright (C) 2012-2018 Rob Clark <robclark@freedesktop.org>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "util/os_mman.h"

#include "freedreno_drmif.h"
#include "freedreno_priv.h"

simple_mtx_t table_lock = SIMPLE_MTX_INITIALIZER;
void bo_del(struct fd_bo *bo);
void bo_del_flush(struct fd_device *dev);

/* set buffer name, and add to table, call w/ table_lock held: */
static void
set_name(struct fd_bo *bo, uint32_t name)
{
   bo->name = name;
   /* add ourself into the handle table: */
   _mesa_hash_table_insert(bo->dev->name_table, &bo->name, bo);
}

/* lookup a buffer, call w/ table_lock held: */
static struct fd_bo *
lookup_bo(struct hash_table *tbl, uint32_t key)
{
   struct fd_bo *bo = NULL;
   struct hash_entry *entry = _mesa_hash_table_search(tbl, &key);
   if (entry) {
      /* found, incr refcnt and return: */
      bo = fd_bo_ref(entry->data);

      /* don't break the bucket if this bo was found in one */
      list_delinit(&bo->list);
   }
   return bo;
}

void
fd_bo_init_common(struct fd_bo *bo, struct fd_device *dev)
{
   /* Backend should have initialized these: */
   assert(bo->size);
   assert(bo->handle);

   bo->dev = dev;
   bo->iova = bo->funcs->iova(bo);
   bo->reloc_flags = FD_RELOC_FLAGS_INIT;

   p_atomic_set(&bo->refcnt, 1);
   list_inithead(&bo->list);
}

/* allocate a new buffer object, call w/ table_lock held */
static struct fd_bo *
bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle)
{
   struct fd_bo *bo;

   simple_mtx_assert_locked(&table_lock);

   bo = dev->funcs->bo_from_handle(dev, size, handle);
   if (!bo) {
      struct drm_gem_close req = {
         .handle = handle,
      };
      drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
      return NULL;
   }

   /* add ourself into the handle table: */
   _mesa_hash_table_insert(dev->handle_table, &bo->handle, bo);

   return bo;
}

static struct fd_bo *
bo_new(struct fd_device *dev, uint32_t size, uint32_t flags,
       struct fd_bo_cache *cache)
{
   struct fd_bo *bo = NULL;

   /* demote cached-coherent to WC if not supported: */
   if ((flags & FD_BO_CACHED_COHERENT) && !dev->has_cached_coherent)
      flags &= ~FD_BO_CACHED_COHERENT;

   bo = fd_bo_cache_alloc(cache, &size, flags);
   if (bo)
      return bo;

   bo = dev->funcs->bo_new(dev, size, flags);
   if (!bo)
      return NULL;

   simple_mtx_lock(&table_lock);
   /* add ourself into the handle table: */
   _mesa_hash_table_insert(dev->handle_table, &bo->handle, bo);
   simple_mtx_unlock(&table_lock);

   bo->alloc_flags = flags;
   bo->max_fences = 1;
   bo->fences = &bo->_inline_fence;

   VG_BO_ALLOC(bo);

   return bo;
}

struct fd_bo *
_fd_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags)
{
   struct fd_bo *bo = bo_new(dev, size, flags, &dev->bo_cache);
   if (bo)
      bo->bo_reuse = BO_CACHE;
   return bo;
}

void
_fd_bo_set_name(struct fd_bo *bo, const char *fmt, va_list ap)
{
   bo->funcs->set_name(bo, fmt, ap);
}

/* internal function to allocate bo's that use the ringbuffer cache
 * instead of the normal bo_cache.  The purpose is, because cmdstream
 * bo's get vmap'd on the kernel side, and that is expensive, we want
 * to re-use cmdstream bo's for cmdstream and not unrelated purposes.
 */
struct fd_bo *
fd_bo_new_ring(struct fd_device *dev, uint32_t size)
{
   struct fd_bo *bo = bo_new(dev, size, RING_FLAGS, &dev->ring_cache);
   if (bo) {
      bo->bo_reuse = RING_CACHE;
      bo->reloc_flags |= FD_RELOC_DUMP;
      fd_bo_set_name(bo, "cmdstream");
   }
   return bo;
}

struct fd_bo *
fd_bo_from_handle(struct fd_device *dev, uint32_t handle, uint32_t size)
{
   struct fd_bo *bo = NULL;

   simple_mtx_lock(&table_lock);

   bo = lookup_bo(dev->handle_table, handle);
   if (bo)
      goto out_unlock;

   bo = bo_from_handle(dev, size, handle);

   VG_BO_ALLOC(bo);

out_unlock:
   simple_mtx_unlock(&table_lock);

   return bo;
}

struct fd_bo *
fd_bo_from_dmabuf(struct fd_device *dev, int fd)
{
   int ret, size;
   uint32_t handle;
   struct fd_bo *bo;

   simple_mtx_lock(&table_lock);
   ret = drmPrimeFDToHandle(dev->fd, fd, &handle);
   if (ret) {
      simple_mtx_unlock(&table_lock);
      return NULL;
   }

   bo = lookup_bo(dev->handle_table, handle);
   if (bo)
      goto out_unlock;

   /* lseek() to get bo size */
   size = lseek(fd, 0, SEEK_END);
   lseek(fd, 0, SEEK_CUR);

   bo = bo_from_handle(dev, size, handle);

   VG_BO_ALLOC(bo);

out_unlock:
   simple_mtx_unlock(&table_lock);

   return bo;
}

struct fd_bo *
fd_bo_from_name(struct fd_device *dev, uint32_t name)
{
   struct drm_gem_open req = {
      .name = name,
   };
   struct fd_bo *bo;

   simple_mtx_lock(&table_lock);

   /* check name table first, to see if bo is already open: */
   bo = lookup_bo(dev->name_table, name);
   if (bo)
      goto out_unlock;

   if (drmIoctl(dev->fd, DRM_IOCTL_GEM_OPEN, &req)) {
      ERROR_MSG("gem-open failed: %s", strerror(errno));
      goto out_unlock;
   }

   bo = lookup_bo(dev->handle_table, req.handle);
   if (bo)
      goto out_unlock;

   bo = bo_from_handle(dev, req.size, req.handle);
   if (bo) {
      set_name(bo, name);
      VG_BO_ALLOC(bo);
   }

out_unlock:
   simple_mtx_unlock(&table_lock);

   return bo;
}

void
fd_bo_mark_for_dump(struct fd_bo *bo)
{
   bo->reloc_flags |= FD_RELOC_DUMP;
}

struct fd_bo *
fd_bo_ref(struct fd_bo *bo)
{
   p_atomic_inc(&bo->refcnt);
   return bo;
}

static void
bo_del_or_recycle(struct fd_bo *bo)
{
   struct fd_device *dev = bo->dev;

   simple_mtx_assert_locked(&table_lock);

   if ((bo->bo_reuse == BO_CACHE) &&
       (fd_bo_cache_free(&dev->bo_cache, bo) == 0))
      return;

   if ((bo->bo_reuse == RING_CACHE) &&
       (fd_bo_cache_free(&dev->ring_cache, bo) == 0))
      return;

   bo_del(bo);
}

void
fd_bo_del_locked(struct fd_bo *bo)
{
   simple_mtx_assert_locked(&table_lock);

   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   struct fd_device *dev = bo->dev;

   bo_del_or_recycle(bo);
   bo_del_flush(dev);
}

void
fd_bo_del(struct fd_bo *bo)
{
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   struct fd_device *dev = bo->dev;

   simple_mtx_lock(&table_lock);
   bo_del_or_recycle(bo);
   bo_del_flush(dev);
   simple_mtx_unlock(&table_lock);
}

void
fd_bo_del_array(struct fd_bo **bos, unsigned count)
{
   if (!count)
      return;

   struct fd_device *dev = bos[0]->dev;

   simple_mtx_lock(&table_lock);
   for (unsigned i = 0; i < count; i++) {
      if (!p_atomic_dec_zero(&bos[i]->refcnt))
         continue;
      bo_del_or_recycle(bos[i]);
   }
   bo_del_flush(dev);
   simple_mtx_unlock(&table_lock);
}

/**
 * Cleanup fences, dropping pipe references.  If 'expired' is true, only
 * cleanup expired fences.
 *
 * Normally we expect at most a single fence, the exception being bo's
 * shared between contexts
 */
static void
cleanup_fences(struct fd_bo *bo, bool expired)
{
   simple_mtx_assert_locked(&table_lock);

   for (int i = 0; i < bo->nr_fences; i++) {
      struct fd_bo_fence *f = &bo->fences[i];

      if (expired && fd_fence_before(f->pipe->control->fence, f->fence))
         continue;

      struct fd_pipe *pipe = f->pipe;

      bo->nr_fences--;

      if (bo->nr_fences > 0) {
         /* Shuffle up the last entry to replace the current slot: */
         bo->fences[i] = bo->fences[bo->nr_fences];
         i--;
      }

      fd_pipe_del_locked(pipe);
   }
}

/* Called under table_lock, bo_del_flush() *must* be called before
 * table_lock is released (but bo_del() can be called multiple times
 * before bo_del_flush(), as long as table_lock is held the entire
 * time)
 */
void
bo_del(struct fd_bo *bo)
{
   struct fd_device *dev = bo->dev;
   uint32_t handle = bo->handle;

   VG_BO_FREE(bo);

   simple_mtx_assert_locked(&table_lock);

   cleanup_fences(bo, false);
   if (bo->fences != &bo->_inline_fence)
      free(bo->fences);

   if (bo->map)
      os_munmap(bo->map, bo->size);

   if (handle) {
      _mesa_hash_table_remove_key(dev->handle_table, &handle);
      if (bo->name)
         _mesa_hash_table_remove_key(dev->name_table, &bo->name);
   }

   bo->funcs->destroy(bo);

   if (handle) {
      if (dev->num_deferred_handles == ARRAY_SIZE(dev->deferred_handles))
         bo_del_flush(dev);
      dev->deferred_handles[dev->num_deferred_handles++] = handle;
   }
}

/* Called under table_lock */
void
bo_del_flush(struct fd_device *dev)
{
   if (!dev->num_deferred_handles)
      return;

   if (dev->funcs->flush)
      dev->funcs->flush(dev);

   for (unsigned i = 0; i < dev->num_deferred_handles; i++) {
      struct drm_gem_close req = {
         .handle = dev->deferred_handles[i],
      };
      drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
   }

   dev->num_deferred_handles = 0;
}

static void
bo_flush(struct fd_bo *bo)
{
   for (int i = 0; i < bo->nr_fences; i++) {
      struct fd_bo_fence *f = &bo->fences[i];
      fd_pipe_flush(f->pipe, f->fence);
   }
}

int
fd_bo_get_name(struct fd_bo *bo, uint32_t *name)
{
   if (!bo->name) {
      struct drm_gem_flink req = {
         .handle = bo->handle,
      };
      int ret;

      ret = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &req);
      if (ret) {
         return ret;
      }

      simple_mtx_lock(&table_lock);
      set_name(bo, req.name);
      simple_mtx_unlock(&table_lock);
      bo->bo_reuse = NO_CACHE;
      bo->shared = true;
      bo_flush(bo);
   }

   *name = bo->name;

   return 0;
}

uint32_t
fd_bo_handle(struct fd_bo *bo)
{
   bo->bo_reuse = NO_CACHE;
   bo->shared = true;
   bo_flush(bo);
   return bo->handle;
}

int
fd_bo_dmabuf(struct fd_bo *bo)
{
   int ret, prime_fd;

   ret = drmPrimeHandleToFD(bo->dev->fd, bo->handle, DRM_CLOEXEC | DRM_RDWR,
                            &prime_fd);
   if (ret) {
      ERROR_MSG("failed to get dmabuf fd: %d", ret);
      return ret;
   }

   bo->bo_reuse = NO_CACHE;
   bo->shared = true;
   bo_flush(bo);

   return prime_fd;
}

uint32_t
fd_bo_size(struct fd_bo *bo)
{
   return bo->size;
}

bool
fd_bo_is_cached(struct fd_bo *bo)
{
   return !!(bo->alloc_flags & FD_BO_CACHED_COHERENT);
}

static void *
bo_map(struct fd_bo *bo)
{
   if (!bo->map) {
      uint64_t offset;
      int ret;

      ret = bo->funcs->offset(bo, &offset);
      if (ret) {
         return NULL;
      }

      bo->map = os_mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        bo->dev->fd, offset);
      if (bo->map == MAP_FAILED) {
         ERROR_MSG("mmap failed: %s", strerror(errno));
         bo->map = NULL;
      }
   }
   return bo->map;
}

void *
fd_bo_map(struct fd_bo *bo)
{
   /* don't allow mmap'ing something allocated with FD_BO_NOMAP
    * for sanity
    */
   if (bo->alloc_flags & FD_BO_NOMAP)
      return NULL;

   return bo_map(bo);
}

void
fd_bo_upload(struct fd_bo *bo, void *src, unsigned off, unsigned len)
{
   if (bo->funcs->upload) {
      bo->funcs->upload(bo, src, off, len);
      return;
   }

   memcpy((uint8_t *)bo_map(bo) + off, src, len);
}

bool
fd_bo_prefer_upload(struct fd_bo *bo, unsigned len)
{
   if (bo->funcs->prefer_upload)
      return bo->funcs->prefer_upload(bo, len);

   return false;
}

/* a bit odd to take the pipe as an arg, but it's a, umm, quirk of kgsl.. */
int
fd_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   simple_mtx_lock(&table_lock);
   enum fd_bo_state state = fd_bo_state(bo);
   simple_mtx_unlock(&table_lock);

   if (state == FD_BO_STATE_IDLE)
      return 0;

   if (op & (FD_BO_PREP_NOSYNC | FD_BO_PREP_FLUSH)) {
      if (op & FD_BO_PREP_FLUSH)
         bo_flush(bo);

      /* If we have *only* been asked to flush, then we aren't really
       * interested about whether shared buffers are busy, so avoid
       * the kernel ioctl.
       */
      if ((state == FD_BO_STATE_BUSY) ||
          (op == FD_BO_PREP_FLUSH))
         return -EBUSY;
   }

   /* In case the bo is referenced by a deferred submit, flush up to the
    * required fence now:
    */
   bo_flush(bo);

   op &= ~FD_BO_PREP_FLUSH;

   if (!op)
      return 0;

   /* FD_BO_PREP_FLUSH is purely a frontend flag, and is not seen/handled
    * by backend or kernel:
    */
   return bo->funcs->cpu_prep(bo, pipe, op);
}

void
fd_bo_cpu_fini(struct fd_bo *bo)
{
// TODO until we have cached buffers, the kernel side ioctl does nothing,
//      so just skip it.  When we have cached buffers, we can make the
//      ioctl conditional
//   bo->funcs->cpu_fini(bo);
}

void
fd_bo_add_fence(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t fence)
{
   simple_mtx_assert_locked(&table_lock);

   if (bo->nosync)
      return;

   /* The common case is bo re-used on the same pipe it had previously
    * been used on:
    */
   for (int i = 0; i < bo->nr_fences; i++) {
      struct fd_bo_fence *f = &bo->fences[i];
      if (f->pipe == pipe) {
         assert(fd_fence_before(f->fence, fence));
         f->fence = fence;
         return;
      }
   }

   cleanup_fences(bo, true);

   /* The first time we grow past a single fence, we need some special
    * handling, as we've been using the embedded _inline_fence to avoid
    * a separate allocation:
    */
   if (unlikely((bo->nr_fences == 1) &&
                (bo->fences == &bo->_inline_fence))) {
      bo->nr_fences = bo->max_fences = 0;
      bo->fences = NULL;
      APPEND(bo, fences, bo->_inline_fence);
   }

   APPEND(bo, fences, (struct fd_bo_fence){
      .pipe = fd_pipe_ref_locked(pipe),
      .fence = fence,
   });
}

enum fd_bo_state
fd_bo_state(struct fd_bo *bo)
{
   simple_mtx_assert_locked(&table_lock);

   cleanup_fences(bo, true);

   if (bo->shared || bo->nosync)
      return FD_BO_STATE_UNKNOWN;

   if (!bo->nr_fences)
      return FD_BO_STATE_IDLE;

   return FD_BO_STATE_BUSY;
}

