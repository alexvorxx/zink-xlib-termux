#ifndef NOUVEAU_BO
#define NOUVEAU_BO 1

#include "nouveau_private.h"

#include "nouveau_device.h"

#ifdef __cplusplus
#include <atomic>
using std::atomic_uint_fast32_t;
#else
#include <stdatomic.h>
#endif

#include <sys/mman.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nouveau_ws_bo_flags {
   /* vram or gart depending on GPU */
   NOUVEAU_WS_BO_VRAM  = 1 << 0,
   NOUVEAU_WS_BO_GART  = 1 << 1,
   NOUVEAU_WS_BO_MAP   = 1 << 2,
   NOUVEAU_WS_BO_NO_SHARE = 1 << 3,
};

enum nouveau_ws_bo_map_flags {
   NOUVEAU_WS_BO_RD   = 1 << 0,
   NOUVEAU_WS_BO_WR   = 1 << 1,
   NOUVEAU_WS_BO_RDWR = NOUVEAU_WS_BO_RD | NOUVEAU_WS_BO_WR,
};

struct nouveau_ws_bo {
   struct nouveau_ws_device *dev;
   uint64_t size;
   uint64_t map_handle;
   uint32_t handle;
   enum nouveau_ws_bo_flags flags;
   atomic_uint_fast32_t refcnt;
};

void nouveau_ws_bo_bind_vma(struct nouveau_ws_device *dev,
                            struct nouveau_ws_bo *bo,
                            uint64_t addr,
                            uint64_t range,
                            uint64_t bo_offset,
                            uint32_t pte_kind);
void nouveau_ws_bo_unbind_vma(struct nouveau_ws_device *dev,
                              uint64_t offset, uint64_t range);

struct nouveau_ws_bo *nouveau_ws_bo_new(struct nouveau_ws_device *,
                                        uint64_t size, uint64_t align,
                                        enum nouveau_ws_bo_flags);
struct nouveau_ws_bo *nouveau_ws_bo_new_mapped(struct nouveau_ws_device *,
                                               uint64_t size, uint64_t align,
                                               enum nouveau_ws_bo_flags,
                                               enum nouveau_ws_bo_map_flags map_flags,
                                               void **map_out);
struct nouveau_ws_bo *nouveau_ws_bo_new_tiled(struct nouveau_ws_device *,
                                              uint64_t size, uint64_t align,
                                              uint8_t pte_kind,
                                              uint16_t tile_mode,
                                              enum nouveau_ws_bo_flags);
struct nouveau_ws_bo *nouveau_ws_bo_from_dma_buf(struct nouveau_ws_device *,
                                                 int fd);
void nouveau_ws_bo_destroy(struct nouveau_ws_bo *);
void *nouveau_ws_bo_map(struct nouveau_ws_bo *,
                        enum nouveau_ws_bo_map_flags);
void nouveau_ws_bo_unmap(struct nouveau_ws_bo *bo, void *ptr);
bool nouveau_ws_bo_wait(struct nouveau_ws_bo *, enum nouveau_ws_bo_map_flags flags);
int nouveau_ws_bo_dma_buf(struct nouveau_ws_bo *, int *fd);

static inline void
nouveau_ws_bo_ref(struct nouveau_ws_bo *bo)
{
   bo->refcnt++;
}

#ifdef __cplusplus
}
#endif

#endif
