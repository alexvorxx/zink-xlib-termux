/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_pipe.h"
#include "util/format/u_format.h"
#include "util/format_srgb.h"
#include "util/u_helpers.h"
#include "util/hash_table.h"
#include "util/u_pack_color.h"

/* Determine the cache policy. */
static enum si_cache_policy get_cache_policy(struct si_context *sctx, enum si_coherency coher,
                                             uint64_t size)
{
   if ((sctx->gfx_level >= GFX9 && (coher == SI_COHERENCY_CB_META ||
                                     coher == SI_COHERENCY_DB_META ||
                                     coher == SI_COHERENCY_CP)) ||
       (sctx->gfx_level >= GFX7 && coher == SI_COHERENCY_SHADER))
      return L2_LRU; /* it's faster if L2 doesn't evict anything  */

   return L2_BYPASS;
}

unsigned si_get_flush_flags(struct si_context *sctx, enum si_coherency coher,
                            enum si_cache_policy cache_policy)
{
   switch (coher) {
   default:
   case SI_COHERENCY_NONE:
   case SI_COHERENCY_CP:
      return 0;
   case SI_COHERENCY_SHADER:
      return SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE |
             (cache_policy == L2_BYPASS ? SI_CONTEXT_INV_L2 : 0);
   case SI_COHERENCY_CB_META:
      return SI_CONTEXT_FLUSH_AND_INV_CB;
   case SI_COHERENCY_DB_META:
      return SI_CONTEXT_FLUSH_AND_INV_DB;
   }
}

static bool si_is_buffer_idle(struct si_context *sctx, struct si_resource *buf,
                              unsigned usage)
{
   return !si_cs_is_buffer_referenced(sctx, buf->buf, usage) &&
          sctx->ws->buffer_wait(sctx->ws, buf->buf, 0, usage);
}

static void si_improve_sync_flags(struct si_context *sctx, struct pipe_resource *dst,
                                  struct pipe_resource *src, unsigned *flags)
{
   if (dst->target != PIPE_BUFFER || (src && src->target != PIPE_BUFFER))
      return;

   if (si_is_buffer_idle(sctx, si_resource(dst), RADEON_USAGE_READWRITE) &&
       (!src || si_is_buffer_idle(sctx, si_resource(src), RADEON_USAGE_WRITE))) {
      /* Idle buffers don't have to sync. */
      *flags &= ~(SI_OP_SYNC_GE_BEFORE | SI_OP_SYNC_PS_BEFORE | SI_OP_SYNC_CS_BEFORE |
                  SI_OP_SYNC_CPDMA_BEFORE);
      return;
   }

   const unsigned cs_mask = SI_BIND_CONSTANT_BUFFER(PIPE_SHADER_COMPUTE) |
                            SI_BIND_SHADER_BUFFER(PIPE_SHADER_COMPUTE) |
                            SI_BIND_IMAGE_BUFFER(PIPE_SHADER_COMPUTE) |
                            SI_BIND_SAMPLER_BUFFER(PIPE_SHADER_COMPUTE);

   const unsigned ps_mask = SI_BIND_CONSTANT_BUFFER(PIPE_SHADER_FRAGMENT) |
                            SI_BIND_SHADER_BUFFER(PIPE_SHADER_FRAGMENT) |
                            SI_BIND_IMAGE_BUFFER(PIPE_SHADER_FRAGMENT) |
                            SI_BIND_SAMPLER_BUFFER(PIPE_SHADER_FRAGMENT);

   unsigned bind_history = si_resource(dst)->bind_history |
                           (src ? si_resource(src)->bind_history : 0);

   /* Clear SI_OP_SYNC_CS_BEFORE if the buffer has never been used with a CS. */
   if (*flags & SI_OP_SYNC_CS_BEFORE && !(bind_history & cs_mask))
      *flags &= ~SI_OP_SYNC_CS_BEFORE;

   /* Clear SI_OP_SYNC_PS_BEFORE if the buffer has never been used with a PS. */
   if (*flags & SI_OP_SYNC_PS_BEFORE && !(bind_history & ps_mask)) {
      *flags &= ~SI_OP_SYNC_PS_BEFORE;
      *flags |= SI_OP_SYNC_GE_BEFORE;
   }
}

static void si_launch_grid_internal(struct si_context *sctx, const struct pipe_grid_info *info,
                                    void *shader, unsigned flags)
{
   /* Wait for previous shaders to finish. */
   if (flags & SI_OP_SYNC_GE_BEFORE)
      sctx->flags |= SI_CONTEXT_VS_PARTIAL_FLUSH;

   if (flags & SI_OP_SYNC_PS_BEFORE)
      sctx->flags |= SI_CONTEXT_PS_PARTIAL_FLUSH;

   if (flags & SI_OP_SYNC_CS_BEFORE)
      sctx->flags |= SI_CONTEXT_CS_PARTIAL_FLUSH;

   /* Invalidate L0-L1 caches. */
   /* sL0 is never invalidated, because src resources don't use it. */
   if (!(flags & SI_OP_SKIP_CACHE_INV_BEFORE))
      sctx->flags |= SI_CONTEXT_INV_VCACHE;

   /* Set settings for driver-internal compute dispatches. */
   sctx->flags &= ~SI_CONTEXT_START_PIPELINE_STATS;
   if (sctx->num_hw_pipestat_streamout_queries)
      sctx->flags |= SI_CONTEXT_STOP_PIPELINE_STATS;

   if (sctx->flags)
      si_mark_atom_dirty(sctx, &sctx->atoms.s.cache_flush);

   if (!(flags & SI_OP_CS_RENDER_COND_ENABLE))
      sctx->render_cond_enabled = false;

   /* Force-disable fbfetch because there are unsolvable recursion problems. */
   si_force_disable_ps_colorbuf0_slot(sctx);

   /* Skip decompression to prevent infinite recursion. */
   sctx->blitter_running = true;

   /* Dispatch compute. */
   void *saved_cs = sctx->cs_shader_state.program;
   sctx->b.bind_compute_state(&sctx->b, shader);
   sctx->b.launch_grid(&sctx->b, info);
   sctx->b.bind_compute_state(&sctx->b, saved_cs);

   /* Restore default settings. */
   sctx->flags &= ~SI_CONTEXT_STOP_PIPELINE_STATS;
   if (sctx->num_hw_pipestat_streamout_queries)
      sctx->flags |= SI_CONTEXT_START_PIPELINE_STATS;

   sctx->render_cond_enabled = sctx->render_cond;
   sctx->blitter_running = false;

   /* We force-disabled fbfetch, so recompute the state. */
   si_update_ps_colorbuf0_slot(sctx);

   if (flags & SI_OP_SYNC_AFTER) {
      sctx->flags |= SI_CONTEXT_CS_PARTIAL_FLUSH;

      if (flags & SI_OP_CS_IMAGE) {
         /* Make sure image stores are visible to CB, which doesn't use L2 on GFX6-8. */
         sctx->flags |= sctx->gfx_level <= GFX8 ? SI_CONTEXT_WB_L2 : 0;
         /* Make sure image stores are visible to all CUs. */
         sctx->flags |= SI_CONTEXT_INV_VCACHE;
      } else {
         /* Make sure buffer stores are visible to all CUs. */
         sctx->flags |= SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE | SI_CONTEXT_PFP_SYNC_ME;
      }
   }

   if (sctx->flags)
      si_mark_atom_dirty(sctx, &sctx->atoms.s.cache_flush);
}

void si_launch_grid_internal_ssbos(struct si_context *sctx, struct pipe_grid_info *info,
                                   void *shader, unsigned flags, enum si_coherency coher,
                                   unsigned num_buffers, const struct pipe_shader_buffer *buffers,
                                   unsigned writeable_bitmask)
{
   if (!(flags & SI_OP_SKIP_CACHE_INV_BEFORE)) {
      sctx->flags |= si_get_flush_flags(sctx, coher, L2_LRU);
      si_mark_atom_dirty(sctx, &sctx->atoms.s.cache_flush);
   }

   /* Save states. */
   struct pipe_shader_buffer saved_sb[3] = {};
   assert(num_buffers <= ARRAY_SIZE(saved_sb));
   si_get_shader_buffers(sctx, PIPE_SHADER_COMPUTE, 0, num_buffers, saved_sb);

   unsigned saved_writable_mask = 0;
   for (unsigned i = 0; i < num_buffers; i++) {
      if (sctx->const_and_shader_buffers[PIPE_SHADER_COMPUTE].writable_mask &
          (1u << si_get_shaderbuf_slot(i)))
         saved_writable_mask |= 1 << i;
   }

   /* Bind buffers and launch compute. */
   si_set_shader_buffers(&sctx->b, PIPE_SHADER_COMPUTE, 0, num_buffers, buffers,
                         writeable_bitmask,
                         true /* don't update bind_history to prevent unnecessary syncs later */);
   si_launch_grid_internal(sctx, info, shader, flags);

   /* Do cache flushing at the end. */
   if (get_cache_policy(sctx, coher, 0) == L2_BYPASS) {
      if (flags & SI_OP_SYNC_AFTER) {
         sctx->flags |= SI_CONTEXT_WB_L2;
         si_mark_atom_dirty(sctx, &sctx->atoms.s.cache_flush);
      }
   } else {
      while (writeable_bitmask)
         si_resource(buffers[u_bit_scan(&writeable_bitmask)].buffer)->TC_L2_dirty = true;
   }

   /* Restore states. */
   sctx->b.set_shader_buffers(&sctx->b, PIPE_SHADER_COMPUTE, 0, num_buffers, saved_sb,
                              saved_writable_mask);
   for (int i = 0; i < num_buffers; i++)
      pipe_resource_reference(&saved_sb[i].buffer, NULL);
}

static unsigned
set_work_size(struct pipe_grid_info *info, unsigned block_x, unsigned block_y, unsigned block_z,
              unsigned work_x, unsigned work_y, unsigned work_z)
{
   info->block[0] = block_x;
   info->block[1] = block_y;
   info->block[2] = block_z;

   unsigned work[3] = {work_x, work_y, work_z};
   for (int i = 0; i < 3; ++i) {
      info->last_block[i] = work[i] % info->block[i];
      info->grid[i] = DIV_ROUND_UP(work[i], info->block[i]);
   }

   return work_z > 1 ? 3 : (work_y > 1 ? 2 : 1);
}

/**
 * Clear a buffer using read-modify-write with a 32-bit write bitmask.
 * The clear value has 32 bits.
 */
void si_compute_clear_buffer_rmw(struct si_context *sctx, struct pipe_resource *dst,
                                 unsigned dst_offset, unsigned size,
                                 uint32_t clear_value, uint32_t writebitmask,
                                 unsigned flags, enum si_coherency coher)
{
   assert(dst_offset % 4 == 0);
   assert(size % 4 == 0);

   assert(dst->target != PIPE_BUFFER || dst_offset + size <= dst->width0);

   /* Use buffer_load_dwordx4 and buffer_store_dwordx4 per thread. */
   unsigned dwords_per_thread = 4;
   unsigned num_threads = DIV_ROUND_UP(size, dwords_per_thread * 4);

   struct pipe_grid_info info = {};
   set_work_size(&info, 64, 1, 1, num_threads, 1, 1);

   struct pipe_shader_buffer sb = {};
   sb.buffer = dst;
   sb.buffer_offset = dst_offset;
   sb.buffer_size = size;

   sctx->cs_user_data[0] = clear_value & writebitmask;
   sctx->cs_user_data[1] = ~writebitmask;

   if (!sctx->cs_clear_buffer_rmw)
      sctx->cs_clear_buffer_rmw = si_create_clear_buffer_rmw_cs(sctx);

   si_launch_grid_internal_ssbos(sctx, &info, sctx->cs_clear_buffer_rmw, flags, coher,
                                 1, &sb, 0x1);
}

static void si_compute_do_clear_or_copy(struct si_context *sctx, struct pipe_resource *dst,
                                        unsigned dst_offset, struct pipe_resource *src,
                                        unsigned src_offset, unsigned size,
                                        const uint32_t *clear_value, unsigned clear_value_size,
                                        unsigned flags, enum si_coherency coher)
{
   assert(src_offset % 4 == 0);
   assert(dst_offset % 4 == 0);
   assert(size % 4 == 0);

   assert(dst->target != PIPE_BUFFER || dst_offset + size <= dst->width0);
   assert(!src || src_offset + size <= src->width0);

   bool is_copy = src != NULL;
   unsigned dwords_per_thread = clear_value_size == 12 ? 3 : 4;
   unsigned num_threads = DIV_ROUND_UP(size, dwords_per_thread * 4);

   struct pipe_grid_info info = {};
   set_work_size(&info, 64, 1, 1, num_threads, 1, 1);

   struct pipe_shader_buffer sb[2] = {};
   sb[is_copy].buffer = dst;
   sb[is_copy].buffer_offset = dst_offset;
   sb[is_copy].buffer_size = size;

   if (is_copy) {
      sb[0].buffer = src;
      sb[0].buffer_offset = src_offset;
      sb[0].buffer_size = size;
   } else {
      assert(clear_value_size >= 4 && clear_value_size <= 16 &&
             (clear_value_size == 12 || util_is_power_of_two_or_zero(clear_value_size)));

      for (unsigned i = 0; i < 4; i++)
         sctx->cs_user_data[i] = clear_value[i % (clear_value_size / 4)];
   }

   void **shader = is_copy ? &sctx->cs_copy_buffer :
                   clear_value_size == 12 ? &sctx->cs_clear_12bytes_buffer : &sctx->cs_clear_buffer;
   if (!*shader)
      *shader = si_create_dma_compute_shader(sctx, dwords_per_thread, !is_copy);

   si_launch_grid_internal_ssbos(sctx, &info, *shader, flags, coher, is_copy ? 2 : 1, sb,
                                 is_copy ? 0x2 : 0x1);
}

void si_clear_buffer(struct si_context *sctx, struct pipe_resource *dst,
                     uint64_t offset, uint64_t size, uint32_t *clear_value,
                     uint32_t clear_value_size, unsigned flags,
                     enum si_coherency coher, enum si_clear_method method)
{
   if (!size)
      return;

   si_improve_sync_flags(sctx, dst, NULL, &flags);

   ASSERTED unsigned clear_alignment = MIN2(clear_value_size, 4);

   assert(clear_value_size != 3 && clear_value_size != 6); /* 12 is allowed. */
   assert(offset % clear_alignment == 0);
   assert(size % clear_alignment == 0);
   assert(size < (UINT_MAX & ~0xf)); /* TODO: test 64-bit sizes in all codepaths */

   uint32_t clamped;
   if (util_lower_clearsize_to_dword(clear_value, (int*)&clear_value_size, &clamped))
      clear_value = &clamped;

   uint64_t aligned_size = size & ~3ull;
   if (aligned_size >= 4) {
      uint64_t compute_min_size;

      if (sctx->gfx_level <= GFX8) {
         /* CP DMA clears are terribly slow with GTT on GFX6-8, which can always
          * happen due to BO evictions.
          */
         compute_min_size = 0;
      } else {
         /* Use a small enough size because CP DMA is slower than compute with bigger sizes. */
         compute_min_size = 4 * 1024;
      }

      /* TODO: use compute for 8-bit and 16-bit clear values */
      if (method == SI_AUTO_SELECT_CLEAR_METHOD &&
          /* CP DMA doesn't support the render condition. */
          (flags & SI_OP_CS_RENDER_COND_ENABLE ||
           /* CP DMA doesn't support large clear value sizes. */
           clear_value_size > 4 ||
           /* Use compute if the size is large enough. Always prefer compute on GFX12. */
           (clear_value_size == 4 && offset % 4 == 0 &&
            (size > compute_min_size || sctx->screen->info.cp_sdma_ge_use_system_memory_scope))))
         method = SI_COMPUTE_CLEAR_METHOD;

      if (method == SI_COMPUTE_CLEAR_METHOD) {
         si_compute_do_clear_or_copy(sctx, dst, offset, NULL, 0, aligned_size, clear_value,
                                     clear_value_size, flags, coher);
      } else {
         assert(clear_value_size == 4);
         assert(!(flags & SI_OP_CS_RENDER_COND_ENABLE));
         si_cp_dma_clear_buffer(sctx, &sctx->gfx_cs, dst, offset, aligned_size, *clear_value,
                                flags, coher, get_cache_policy(sctx, coher, size));
      }

      offset += aligned_size;
      size -= aligned_size;
   }

   /* Handle non-dword alignment. */
   if (size) {
      assert(!(flags & SI_OP_CS_RENDER_COND_ENABLE));
      assert(dst);
      assert(dst->target == PIPE_BUFFER);
      assert(size < 4);

      sctx->b.buffer_subdata(&sctx->b, dst,
                             PIPE_MAP_WRITE |
                             /* TC forbids drivers to invalidate buffers and infer unsynchronized mappings,
                              * so suppress those optimizations. */
                             (sctx->tc ? TC_TRANSFER_MAP_NO_INFER_UNSYNCHRONIZED |
                                         TC_TRANSFER_MAP_NO_INVALIDATE : 0),
                             offset, size, clear_value);
   }
}

static void si_pipe_clear_buffer(struct pipe_context *ctx, struct pipe_resource *dst,
                                 unsigned offset, unsigned size, const void *clear_value,
                                 int clear_value_size)
{
   si_clear_buffer((struct si_context *)ctx, dst, offset, size, (uint32_t *)clear_value,
                   clear_value_size, SI_OP_SYNC_BEFORE_AFTER, SI_COHERENCY_SHADER,
                   SI_AUTO_SELECT_CLEAR_METHOD);
}

void si_copy_buffer(struct si_context *sctx, struct pipe_resource *dst, struct pipe_resource *src,
                    uint64_t dst_offset, uint64_t src_offset, unsigned size, unsigned flags)
{
   if (!size)
      return;

   enum si_coherency coher = SI_COHERENCY_SHADER;
   enum si_cache_policy cache_policy = get_cache_policy(sctx, coher, size);
   uint64_t compute_min_size = 8 * 1024;

   si_improve_sync_flags(sctx, dst, src, &flags);

   /* Only use compute for VRAM copies on dGPUs. */
   /* TODO: use compute for unaligned big sizes */
   if (sctx->screen->info.has_dedicated_vram && si_resource(dst)->domains & RADEON_DOMAIN_VRAM &&
       si_resource(src)->domains & RADEON_DOMAIN_VRAM &&
       dst_offset % 4 == 0 && src_offset % 4 == 0 && size % 4 == 0 &&
       (size > compute_min_size || sctx->screen->info.cp_sdma_ge_use_system_memory_scope)) {
      si_compute_do_clear_or_copy(sctx, dst, dst_offset, src, src_offset, size, NULL, 0,
                                  flags, coher);
   } else {
      si_cp_dma_copy_buffer(sctx, dst, src, dst_offset, src_offset, size,
                            flags, coher, cache_policy);
   }
}

void si_compute_shorten_ubyte_buffer(struct si_context *sctx, struct pipe_resource *dst, struct pipe_resource *src,
                                     uint64_t dst_offset, uint64_t src_offset, unsigned size, unsigned flags)
{
   if (!size)
      return;

   if (!sctx->cs_ubyte_to_ushort)
      sctx->cs_ubyte_to_ushort = si_create_ubyte_to_ushort_compute_shader(sctx);

   /* Use COHERENCY_NONE to get SI_CONTEXT_WB_L2 automatically used in
    * si_launch_grid_internal_ssbos.
    */
   enum si_coherency coher = SI_COHERENCY_NONE;

   si_improve_sync_flags(sctx, dst, src, &flags);

   struct pipe_grid_info info = {};
   set_work_size(&info, 64, 1, 1, size, 1, 1);

   struct pipe_shader_buffer sb[2] = {};
   sb[0].buffer = dst;
   sb[0].buffer_offset = dst_offset;
   sb[0].buffer_size = dst->width0;

   sb[1].buffer = src;
   sb[1].buffer_offset = src_offset;
   sb[1].buffer_size = src->width0;

   si_launch_grid_internal_ssbos(sctx, &info, sctx->cs_ubyte_to_ushort, flags, coher,
                                 2, sb, 0x1);
}

static void si_launch_grid_internal_images(struct si_context *sctx,
                                           struct pipe_image_view *images,
                                           unsigned num_images,
                                           const struct pipe_grid_info *info,
                                           void *shader, unsigned flags)
{
   struct pipe_image_view saved_image[2] = {};
   assert(num_images <= ARRAY_SIZE(saved_image));

   for (unsigned i = 0; i < num_images; i++) {
      assert(sctx->b.screen->is_format_supported(sctx->b.screen, images[i].format,
                                                 images[i].resource->target,
                                                 images[i].resource->nr_samples,
                                                 images[i].resource->nr_storage_samples,
                                                 PIPE_BIND_SHADER_IMAGE));

      /* Always allow DCC stores on gfx10+. */
      if (sctx->gfx_level >= GFX10 &&
          images[i].access & PIPE_IMAGE_ACCESS_WRITE &&
          !(images[i].access & SI_IMAGE_ACCESS_DCC_OFF))
         images[i].access |= SI_IMAGE_ACCESS_ALLOW_DCC_STORE;

      /* Simplify the format according to what image stores support. */
      if (images[i].access & PIPE_IMAGE_ACCESS_WRITE) {
         images[i].format = util_format_linear(images[i].format); /* SRGB not supported */
         /* Keep L8A8 formats as-is because GFX7 is unable to store into R8A8 for some reason. */
         images[i].format = util_format_intensity_to_red(images[i].format);
         images[i].format = util_format_rgbx_to_rgba(images[i].format); /* prevent partial writes */
      }

      /* Save the image. */
      util_copy_image_view(&saved_image[i], &sctx->images[PIPE_SHADER_COMPUTE].views[i]);
   }

   /* This might invoke DCC decompression, so do it first. */
   sctx->b.set_shader_images(&sctx->b, PIPE_SHADER_COMPUTE, 0, num_images, 0, images);

   /* This should be done after set_shader_images. */
   for (unsigned i = 0; i < num_images; i++) {
      /* The driver doesn't decompress resources automatically here, so do it manually. */
      si_decompress_subresource(&sctx->b, images[i].resource, PIPE_MASK_RGBAZS,
                                images[i].u.tex.level, images[i].u.tex.first_layer,
                                images[i].u.tex.last_layer,
                                images[i].access & PIPE_IMAGE_ACCESS_WRITE);
   }

   /* This must be done before the compute shader. */
   if (flags & SI_OP_SYNC_PS_BEFORE) {
      for (unsigned i = 0; i < num_images; i++) {
         si_make_CB_shader_coherent(sctx, images[i].resource->nr_samples, true,
               ((struct si_texture*)images[i].resource)->surface.u.gfx9.color.dcc.pipe_aligned);
      }
   }

   si_launch_grid_internal(sctx, info, shader, flags | SI_OP_CS_IMAGE);

   /* Make sure RBs see our DCC stores if RBs and TCCs (L2 instances) are non-coherent. */
   if (flags & SI_OP_SYNC_AFTER && sctx->gfx_level >= GFX10 &&
       sctx->screen->info.tcc_rb_non_coherent) {
      for (unsigned i = 0; i < num_images; i++) {
         if (vi_dcc_enabled((struct si_texture*)images[i].resource, images[i].u.tex.level) &&
             images[i].access & PIPE_IMAGE_ACCESS_WRITE &&
             (sctx->screen->always_allow_dcc_stores ||
              images[i].access & SI_IMAGE_ACCESS_ALLOW_DCC_STORE)) {
            sctx->flags |= SI_CONTEXT_INV_L2;
            si_mark_atom_dirty(sctx, &sctx->atoms.s.cache_flush);
            break;
         }
      }
   }

   /* Restore images. */
   sctx->b.set_shader_images(&sctx->b, PIPE_SHADER_COMPUTE, 0, num_images, 0, saved_image);
   for (unsigned i = 0; i < num_images; i++)
      pipe_resource_reference(&saved_image[i].resource, NULL);
}

void si_retile_dcc(struct si_context *sctx, struct si_texture *tex)
{
   assert(sctx->gfx_level < GFX12);

   /* Set the DCC buffer. */
   assert(tex->surface.meta_offset && tex->surface.meta_offset <= UINT_MAX);
   assert(tex->surface.display_dcc_offset && tex->surface.display_dcc_offset <= UINT_MAX);
   assert(tex->surface.display_dcc_offset < tex->surface.meta_offset);
   assert(tex->buffer.bo_size <= UINT_MAX);

   struct pipe_shader_buffer sb = {};
   sb.buffer = &tex->buffer.b.b;
   sb.buffer_offset = tex->surface.display_dcc_offset;
   sb.buffer_size = tex->buffer.bo_size - sb.buffer_offset;

   sctx->cs_user_data[0] = tex->surface.meta_offset - tex->surface.display_dcc_offset;
   sctx->cs_user_data[1] = (tex->surface.u.gfx9.color.dcc_pitch_max + 1) |
                           (tex->surface.u.gfx9.color.dcc_height << 16);
   sctx->cs_user_data[2] = (tex->surface.u.gfx9.color.display_dcc_pitch_max + 1) |
                           (tex->surface.u.gfx9.color.display_dcc_height << 16);

   /* We have only 1 variant per bpp for now, so expect 32 bpp. */
   assert(tex->surface.bpe == 4);

   void **shader = &sctx->cs_dcc_retile[tex->surface.u.gfx9.swizzle_mode];
   if (!*shader)
      *shader = si_create_dcc_retile_cs(sctx, &tex->surface);

   /* Dispatch compute. */
   unsigned width = DIV_ROUND_UP(tex->buffer.b.b.width0, tex->surface.u.gfx9.color.dcc_block_width);
   unsigned height = DIV_ROUND_UP(tex->buffer.b.b.height0, tex->surface.u.gfx9.color.dcc_block_height);

   struct pipe_grid_info info = {};
   set_work_size(&info, 8, 8, 1, width, height, 1);

   si_launch_grid_internal_ssbos(sctx, &info, *shader, SI_OP_SYNC_BEFORE,
                                 SI_COHERENCY_CB_META, 1, &sb, 0x1);

   /* Don't flush caches. L2 will be flushed by the kernel fence. */
}

void gfx9_clear_dcc_msaa(struct si_context *sctx, struct pipe_resource *res, uint32_t clear_value,
                         unsigned flags, enum si_coherency coher)
{
   struct si_texture *tex = (struct si_texture*)res;

   assert(sctx->gfx_level < GFX11);

   /* Set the DCC buffer. */
   assert(tex->surface.meta_offset && tex->surface.meta_offset <= UINT_MAX);
   assert(tex->buffer.bo_size <= UINT_MAX);

   struct pipe_shader_buffer sb = {};
   sb.buffer = &tex->buffer.b.b;
   sb.buffer_offset = tex->surface.meta_offset;
   sb.buffer_size = tex->buffer.bo_size - sb.buffer_offset;

   sctx->cs_user_data[0] = (tex->surface.u.gfx9.color.dcc_pitch_max + 1) |
                           (tex->surface.u.gfx9.color.dcc_height << 16);
   sctx->cs_user_data[1] = (clear_value & 0xffff) |
                           ((uint32_t)tex->surface.tile_swizzle << 16);

   /* These variables identify the shader variant. */
   unsigned swizzle_mode = tex->surface.u.gfx9.swizzle_mode;
   unsigned bpe_log2 = util_logbase2(tex->surface.bpe);
   unsigned log2_samples = util_logbase2(tex->buffer.b.b.nr_samples);
   bool fragments8 = tex->buffer.b.b.nr_storage_samples == 8;
   bool is_array = tex->buffer.b.b.array_size > 1;
   void **shader = &sctx->cs_clear_dcc_msaa[swizzle_mode][bpe_log2][fragments8][log2_samples - 2][is_array];

   if (!*shader)
      *shader = gfx9_create_clear_dcc_msaa_cs(sctx, tex);

   /* Dispatch compute. */
   unsigned width = DIV_ROUND_UP(tex->buffer.b.b.width0, tex->surface.u.gfx9.color.dcc_block_width);
   unsigned height = DIV_ROUND_UP(tex->buffer.b.b.height0, tex->surface.u.gfx9.color.dcc_block_height);
   unsigned depth = DIV_ROUND_UP(tex->buffer.b.b.array_size, tex->surface.u.gfx9.color.dcc_block_depth);

   struct pipe_grid_info info = {};
   set_work_size(&info, 8, 8, 1, width, height, depth);

   si_launch_grid_internal_ssbos(sctx, &info, *shader, flags, coher, 1, &sb, 0x1);
}

/* Expand FMASK to make it identity, so that image stores can ignore it. */
void si_compute_expand_fmask(struct pipe_context *ctx, struct pipe_resource *tex)
{
   struct si_context *sctx = (struct si_context *)ctx;
   bool is_array = tex->target == PIPE_TEXTURE_2D_ARRAY;
   unsigned log_fragments = util_logbase2(tex->nr_storage_samples);
   unsigned log_samples = util_logbase2(tex->nr_samples);
   assert(tex->nr_samples >= 2);

   assert(sctx->gfx_level < GFX11);

   /* EQAA FMASK expansion is unimplemented. */
   if (tex->nr_samples != tex->nr_storage_samples)
      return;

   si_make_CB_shader_coherent(sctx, tex->nr_samples, true,
                              ((struct si_texture*)tex)->surface.u.gfx9.color.dcc.pipe_aligned);

   /* Save states. */
   struct pipe_image_view saved_image = {0};
   util_copy_image_view(&saved_image, &sctx->images[PIPE_SHADER_COMPUTE].views[0]);

   /* Bind the image. */
   struct pipe_image_view image = {0};
   image.resource = tex;
   /* Don't set WRITE so as not to trigger FMASK expansion, causing
    * an infinite loop. */
   image.shader_access = image.access = PIPE_IMAGE_ACCESS_READ;
   image.format = util_format_linear(tex->format);
   if (is_array)
      image.u.tex.last_layer = tex->array_size - 1;

   ctx->set_shader_images(ctx, PIPE_SHADER_COMPUTE, 0, 1, 0, &image);

   /* Bind the shader. */
   void **shader = &sctx->cs_fmask_expand[log_samples - 1][is_array];
   if (!*shader)
      *shader = si_create_fmask_expand_cs(sctx, tex->nr_samples, is_array);

   /* Dispatch compute. */
   struct pipe_grid_info info = {0};
   set_work_size(&info, 8, 8, 1, tex->width0, tex->height0, is_array ? tex->array_size : 1);

   si_launch_grid_internal(sctx, &info, *shader, SI_OP_SYNC_BEFORE_AFTER);

   /* Restore previous states. */
   ctx->set_shader_images(ctx, PIPE_SHADER_COMPUTE, 0, 1, 0, &saved_image);
   pipe_resource_reference(&saved_image.resource, NULL);

   /* Array of fully expanded FMASK values, arranged by [log2(fragments)][log2(samples)-1]. */
#define INVALID 0 /* never used */
   static const uint64_t fmask_expand_values[][4] = {
      /* samples */
      /* 2 (8 bpp) 4 (8 bpp)   8 (8-32bpp) 16 (16-64bpp)      fragments */
      {0x02020202, 0x0E0E0E0E, 0xFEFEFEFE, 0xFFFEFFFE},      /* 1 */
      {0x02020202, 0xA4A4A4A4, 0xAAA4AAA4, 0xAAAAAAA4},      /* 2 */
      {INVALID, 0xE4E4E4E4, 0x44443210, 0x4444444444443210}, /* 4 */
      {INVALID, INVALID, 0x76543210, 0x8888888876543210},    /* 8 */
   };

   /* Clear FMASK to identity. */
   struct si_texture *stex = (struct si_texture *)tex;
   si_clear_buffer(sctx, tex, stex->surface.fmask_offset, stex->surface.fmask_size,
                   (uint32_t *)&fmask_expand_values[log_fragments][log_samples - 1],
                   log_fragments >= 2 && log_samples == 4 ? 8 : 4, SI_OP_SYNC_AFTER,
                   SI_COHERENCY_SHADER, SI_AUTO_SELECT_CLEAR_METHOD);
}

void si_compute_clear_image_dcc_single(struct si_context *sctx, struct si_texture *tex,
                                       unsigned level, enum pipe_format format,
                                       const union pipe_color_union *color, unsigned flags)
{
   assert(sctx->gfx_level >= GFX11); /* not believed to be useful on gfx10 */
   unsigned dcc_block_width = tex->surface.u.gfx9.color.dcc_block_width;
   unsigned dcc_block_height = tex->surface.u.gfx9.color.dcc_block_height;
   unsigned width = DIV_ROUND_UP(u_minify(tex->buffer.b.b.width0, level), dcc_block_width);
   unsigned height = DIV_ROUND_UP(u_minify(tex->buffer.b.b.height0, level), dcc_block_height);
   unsigned depth = util_num_layers(&tex->buffer.b.b, level);
   bool is_msaa = tex->buffer.b.b.nr_samples >= 2;

   struct pipe_image_view image = {0};
   image.resource = &tex->buffer.b.b;
   image.shader_access = image.access = PIPE_IMAGE_ACCESS_WRITE | SI_IMAGE_ACCESS_DCC_OFF;
   image.format = format;
   image.u.tex.level = level;
   image.u.tex.last_layer = depth - 1;

   if (util_format_is_srgb(format)) {
      union pipe_color_union color_srgb;
      for (int i = 0; i < 3; i++)
         color_srgb.f[i] = util_format_linear_to_srgb_float(color->f[i]);
      color_srgb.f[3] = color->f[3];
      memcpy(sctx->cs_user_data, color_srgb.ui, sizeof(color->ui));
   } else {
      memcpy(sctx->cs_user_data, color->ui, sizeof(color->ui));
   }

   sctx->cs_user_data[4] = dcc_block_width | (dcc_block_height << 16);

   struct pipe_grid_info info = {0};
   unsigned wg_dim = set_work_size(&info, 8, 8, 1, width, height, depth);

   void **shader = &sctx->cs_clear_image_dcc_single[is_msaa][wg_dim];
   if (!*shader)
      *shader = si_clear_image_dcc_single_shader(sctx, is_msaa, wg_dim);

   si_launch_grid_internal_images(sctx, &image, 1, &info, *shader, flags);
}

void si_init_compute_blit_functions(struct si_context *sctx)
{
   sctx->b.clear_buffer = si_pipe_clear_buffer;
}

static bool si_should_blit_clamp_to_edge(const struct pipe_blit_info *info, unsigned coord_mask)
{
   return util_is_box_out_of_bounds(&info->src.box, coord_mask, info->src.resource->width0,
                                    info->src.resource->height0, info->src.level);
}

bool si_compute_clear_image(struct si_context *sctx, struct pipe_resource *tex,
                            enum pipe_format format, unsigned level, const struct pipe_box *box,
                            const union pipe_color_union *color, bool render_condition_enable,
                            bool fail_if_slow)
{
   struct pipe_blit_info info;
   memset(&info, 0, sizeof(info));
   info.dst.resource = tex;
   info.dst.level = level;
   info.dst.box = *box;
   info.dst.format = format;
   info.mask = util_format_is_depth_or_stencil(format) ? PIPE_MASK_ZS : PIPE_MASK_RGBA;
   info.render_condition_enable = render_condition_enable;

   return si_compute_blit(sctx, &info, color, 0, 0,
                          SI_OP_SYNC_BEFORE_AFTER | (fail_if_slow ? SI_OP_FAIL_IF_SLOW : 0));
}

bool si_compute_copy_image(struct si_context *sctx, struct pipe_resource *dst, unsigned dst_level,
                           struct pipe_resource *src, unsigned src_level, unsigned dstx,
                           unsigned dsty, unsigned dstz, const struct pipe_box *src_box,
                           bool fail_if_slow)
{
   struct si_texture *ssrc = (struct si_texture*)src;
   struct si_texture *sdst = (struct si_texture*)dst;
   enum pipe_format src_format = util_format_linear(src->format);
   enum pipe_format dst_format = util_format_linear(dst->format);

   assert(util_format_is_subsampled_422(src_format) == util_format_is_subsampled_422(dst_format));

   /* Interpret as integer values to avoid NaN issues */
   if (!vi_dcc_enabled(ssrc, src_level) &&
       !vi_dcc_enabled(sdst, dst_level) &&
       src_format == dst_format &&
       util_format_is_float(src_format) &&
       !util_format_is_compressed(src_format)) {
      switch(util_format_get_blocksizebits(src_format)) {
        case 16:
          src_format = dst_format = PIPE_FORMAT_R16_UINT;
          break;
        case 32:
          src_format = dst_format = PIPE_FORMAT_R32_UINT;
          break;
        case 64:
          src_format = dst_format = PIPE_FORMAT_R32G32_UINT;
          break;
        case 128:
          src_format = dst_format = PIPE_FORMAT_R32G32B32A32_UINT;
          break;
        default:
          assert(false);
      }
   }

   /* Interpret compressed formats as UINT. */
   struct pipe_box new_box;
   unsigned src_access = 0, dst_access = 0;

   /* Note that staging copies do compressed<->UINT, so one of the formats is already UINT. */
   if (util_format_is_compressed(src_format) || util_format_is_compressed(dst_format)) {
      if (util_format_is_compressed(src_format))
         src_access |= SI_IMAGE_ACCESS_BLOCK_FORMAT_AS_UINT;
      if (util_format_is_compressed(dst_format))
         dst_access |= SI_IMAGE_ACCESS_BLOCK_FORMAT_AS_UINT;

      dstx = util_format_get_nblocksx(dst_format, dstx);
      dsty = util_format_get_nblocksy(dst_format, dsty);

      new_box.x = util_format_get_nblocksx(src_format, src_box->x);
      new_box.y = util_format_get_nblocksy(src_format, src_box->y);
      new_box.z = src_box->z;
      new_box.width = util_format_get_nblocksx(src_format, src_box->width);
      new_box.height = util_format_get_nblocksy(src_format, src_box->height);
      new_box.depth = src_box->depth;
      src_box = &new_box;

      if (ssrc->surface.bpe == 8)
         src_format = dst_format = PIPE_FORMAT_R16G16B16A16_UINT; /* 64-bit block */
      else
         src_format = dst_format = PIPE_FORMAT_R32G32B32A32_UINT; /* 128-bit block */
   }

   if (util_format_is_subsampled_422(src_format)) {
      assert(src_format == dst_format);

      src_access |= SI_IMAGE_ACCESS_BLOCK_FORMAT_AS_UINT;
      dst_access |= SI_IMAGE_ACCESS_BLOCK_FORMAT_AS_UINT;

      dstx = util_format_get_nblocksx(src_format, dstx);

      src_format = dst_format = PIPE_FORMAT_R32_UINT;

      /* Interpreting 422 subsampled format (16 bpp) as 32 bpp
       * should force us to divide src_box->x, dstx and width by 2.
       * But given that ac_surface allocates this format as 32 bpp
       * and that surf_size is then modified to pack the values
       * we must keep the original values to get the correct results.
       */
   }

   /* SNORM blitting has precision issues. Use the SINT equivalent instead, which doesn't
    * force DCC decompression.
    */
   if (util_format_is_snorm(dst_format))
      src_format = dst_format = util_format_snorm_to_sint(dst_format);

   struct pipe_blit_info info;
   memset(&info, 0, sizeof(info));
   info.dst.resource = dst;
   info.dst.level = dst_level;
   info.dst.box.x = dstx;
   info.dst.box.y = dsty;
   info.dst.box.z = dstz;
   info.dst.box.width = src_box->width;
   info.dst.box.height = src_box->height;
   info.dst.box.depth = src_box->depth;
   info.dst.format = dst_format;
   info.src.resource = src;
   info.src.level = src_level;
   info.src.box = *src_box;
   info.src.format = src_format;
   info.mask = util_format_is_depth_or_stencil(dst_format) ? PIPE_MASK_ZS : PIPE_MASK_RGBA;

   /* Only the compute blit can copy compressed and subsampled images. */
   fail_if_slow &= !dst_access && !src_access;

   bool success = si_compute_blit(sctx, &info, NULL, dst_access, src_access,
                                  SI_OP_SYNC_BEFORE_AFTER | (fail_if_slow ? SI_OP_FAIL_IF_SLOW : 0));
   assert((!dst_access && !src_access) || success);
   return success;
}

typedef struct {
   unsigned x, y, z;
} uvec3;

bool si_compute_blit(struct si_context *sctx, const struct pipe_blit_info *info,
                     const union pipe_color_union *clear_color, unsigned dst_access,
                     unsigned src_access, unsigned flags)
{
   struct si_texture *sdst = (struct si_texture *)info->dst.resource;
   struct si_texture *ssrc = (struct si_texture *)info->src.resource;
   bool is_3d_tiling = sdst->surface.thick_tiling;
   bool is_clear = !info->src.resource;
   unsigned dst_samples = MAX2(1, sdst->buffer.b.b.nr_samples);
   unsigned src_samples = is_clear ? 1 : MAX2(1, ssrc->buffer.b.b.nr_samples);
   bool sample0_only = src_samples >= 2 && dst_samples == 1 &&
                       (info->sample0_only || util_format_is_pure_integer(info->dst.format));
   /* Get the channel sizes. */
   unsigned max_dst_chan_size = util_format_get_max_channel_size(info->dst.format);
   unsigned max_src_chan_size = is_clear ? 0 : util_format_get_max_channel_size(info->src.format);

   /* Reject blits with invalid parameters. */
   if (info->dst.box.width < 0 || info->dst.box.height < 0 || info->dst.box.depth < 0 ||
       info->src.box.depth < 0) {
      assert(!"invalid box parameters"); /* this is reachable and prevents hangs */
      return true;
   }

   /* Skip zero-area blits. */
   if (!info->dst.box.width || !info->dst.box.height || !info->dst.box.depth ||
       (!is_clear && (!info->src.box.width || !info->src.box.height || !info->src.box.depth)))
      return true;

   /* MSAA image stores don't work on <= Gfx10.3. It's an issue with FMASK because
    * AMD_DEBUG=nofmask fixes them. EQAA image stores are also unimplemented.
    * MSAA image stores work fine on Gfx11 (it has neither FMASK nor EQAA).
    */
   if (sctx->gfx_level < GFX11 && !(sctx->screen->debug_flags & DBG(NO_FMASK)) && dst_samples > 1)
      return false;

   if (info->dst.format == PIPE_FORMAT_A8R8_UNORM || /* This format fails AMD_TEST=imagecopy. */
       max_dst_chan_size == 5 || /* PIPE_FORMAT_R5G5B5A1_UNORM has precision issues */
       util_format_is_depth_or_stencil(info->dst.resource->format) ||
       dst_samples > SI_MAX_COMPUTE_BLIT_SAMPLES ||
       info->dst_sample != 0 ||
       /* Image stores support DCC since GFX10. Return only for gfx queues. DCC is disabled
        * for compute queues farther below. */
       (sctx->gfx_level < GFX10 && sctx->has_graphics && vi_dcc_enabled(sdst, info->dst.level) &&
        !src_access && !dst_access) ||
       info->alpha_blend ||
       info->num_window_rectangles ||
       info->scissor_enable ||
       (!is_clear &&
        /* Scaling is not implemented by the compute shader. */
        (info->dst.box.width != abs(info->src.box.width) ||
         info->dst.box.height != abs(info->src.box.height) ||
         info->dst.box.depth != abs(info->src.box.depth) ||
         util_format_is_depth_or_stencil(info->src.resource->format) ||
         src_samples > SI_MAX_COMPUTE_BLIT_SAMPLES)))
      return false;

   /* Testing on Navi21 showed that the compute blit is slightly slower than the gfx blit.
    * The compute blit is even slower with DCC stores. VP13 CATIA_plane_pencil is a good test
    * for that because it's mostly just blits.
    *
    * TODO: benchmark the performance on gfx11
    */
   if (sctx->gfx_level < GFX11 && sctx->has_graphics && flags & SI_OP_FAIL_IF_SLOW)
      return false;

   if (sctx->gfx_level < GFX10 && !sctx->has_graphics && vi_dcc_enabled(sdst, info->dst.level))
      si_texture_disable_dcc(sctx, sdst);

   unsigned width = info->dst.box.width;
   unsigned height = info->dst.box.height;
   unsigned depth = info->dst.box.depth;

   /* Determine the alignment of coordinates of the first thread of each wave. The alignment should be
    * to a 256B block or the size of 1 wave, whichever is less, but there are a few exceptions.
    */
   uvec3 align;
   if (is_3d_tiling) {
      /* Thick tiling. */
      /* This is based on GFX11_SW_PATTERN_NIBBLE01, which also matches GFX10. */
      if (sdst->surface.bpe == 1)
         align = (uvec3){8, 4, 8};
      else if (sdst->surface.bpe == 2)
         align = (uvec3){4, 4, 8};
      else if (sdst->surface.bpe == 4)
         align = (uvec3){4, 4, 4};
      else if (sdst->surface.bpe == 8)
         align = (uvec3){4, 2, 4};
      else {
         /* 16bpp linear source image reads perform better with this. */
         if (!is_clear && ssrc->surface.is_linear)
            align = (uvec3){4, 2, 4}; /* align to 512B for linear->tiled */
         else
            align = (uvec3){2, 2, 4};
      }

      /* Clamp the alignment to the size of 1 wave. */
      align.x = MIN2(align.x, 4);
      align.y = MIN2(align.y, 4);
      align.z = MIN2(align.z, 4);
   } else if (sdst->surface.is_linear) {
      /* 1D blits from linear to linear are faster unaligned.
       * 1D image clears don't benefit from any alignment.
       */
      if (height == 1 && depth == 1 && (is_clear || ssrc->surface.is_linear)) {
         align = (uvec3){1, 1, 1};
      } else {
         /* Linear blits should use the cache line size instead of 256B alignment. */
         align.x = MIN2(64, sctx->screen->info.tcc_cache_line_size / sdst->surface.bpe);
         align.y = 1;
         align.z = 1;
      }
   } else {
      /* Thin tiling. */
      if (sctx->gfx_level >= GFX11) {
         /* Samples are next to each other on GFX11+. */
         unsigned pix_size = sdst->surface.bpe * dst_samples;

         /* This is based on GFX11_SW_PATTERN_NIBBLE01. */
         if (pix_size == 1)
            align = (uvec3){16, 16, 1};
         else if (pix_size == 2)
            align = (uvec3){16, 8, 1};
         else if (pix_size == 4)
            align = (uvec3){8, 8, 1};
         else if (pix_size == 8)
            align = (uvec3){8, 4, 1};
         else if (pix_size == 16)
            align = (uvec3){4, 4, 1};
         else if (pix_size == 32)
            align = (uvec3){4, 2, 1};
         else if (pix_size == 64)
            align = (uvec3){2, 2, 1};
         else
            align = (uvec3){2, 1, 1}; /* 16bpp 8xAA */
      } else {
         /* This is for 64KB_R_X. (most likely to occur due to DCC)
          * It's based on GFX10_SW_64K_R_X_*xaa_RBPLUS_PATINFO (GFX10.3).
          * The patterns are GFX10_SW_PATTERN_NIBBLE01[0, 1, 39, 6, 7] for 8bpp-128bpp.
          * GFX6-10.1 and other swizzle modes might be similar.
          */
         if (sdst->surface.bpe == 1)
            align = (uvec3){16, 16, 1};
         else if (sdst->surface.bpe == 2)
            align = (uvec3){16, 8, 1};
         else if (sdst->surface.bpe == 4)
            align = (uvec3){8, 8, 1};
         else if (sdst->surface.bpe == 8)
            align = (uvec3){8, 4, 1};
         else
            align = (uvec3){4, 4, 1};
      }

      /* Clamp the alignment to the size of 1 wave. */
      align.x = MIN2(align.x, 8);
      align.y = MIN2(align.y, 8);
   }

   /* If we don't have much to copy, don't align. The threshold is guessed and isn't covered
    * by benchmarking.
    */
   if (width <= align.x * 4)
      align.x = 1;
   if (height <= align.y * 4)
      align.y = 1;
   if (depth <= align.z * 4)
      align.z = 1;

   unsigned start_x, start_y, start_z;
   unsigned block_x, block_y, block_z;

   /* If the blit destination area is unaligned, launch extra threads before 0,0,0 to make it
    * aligned. This makes sure that a wave doesn't straddle a DCC block boundary or a cache line
    * unnecessarily, so that each cache line is only stored by exactly 1 CU. The shader will skip
    * the extra threads. This makes unaligned compute blits faster.
    */
   start_x = info->dst.box.x % align.x;
   start_y = info->dst.box.y % align.y;
   start_z = info->dst.box.z % align.z;
   width += start_x;
   height += start_y;
   depth += start_z;

   /* Choose the block (i.e. wave) dimensions based on the copy area size and the image layout
    * of dst.
    */
   if (is_3d_tiling) {
      /* Thick tiling. (microtiles are 3D boxes)
       * If the box height and depth is > 2, the block size will be 4x4x4.
       * If not, the threads will spill over to X.
       */
      block_y = util_next_power_of_two(MIN2(height, 4));
      block_z = util_next_power_of_two(MIN2(depth, 4));
      block_x = 64 / (block_y * block_z);
   } else if (sdst->surface.is_linear) {
      /* If the box width is > 128B, the block size will be 64x1 for bpp <= 4, 32x2 for bpp == 8,
       * and 16x4 for bpp == 16.
       * If not, the threads will spill over to Y, then Z if they aren't small.
       *
       * This is derived from the fact that the linear image layout has 256B linear blocks, and
       * longer blocks don't benefit linear write performance, but they hurt tiled read performance.
       * We want to prioritize blocks that are 256Bx2 over 512Bx1 because the source can be tiled.
       *
       * Using the cache line size (128B) instead of hardcoding 256B makes linear blits slower.
       */
      block_x = util_next_power_of_two(MIN3(width, 64, 256 / sdst->surface.bpe));
      block_y = util_next_power_of_two(MIN2(height, 64 / block_x));
      block_z = util_next_power_of_two(MIN2(depth, 64 / (block_x * block_y)));
      block_x = 64 / (block_y * block_z);
   } else {
      /* Thin tiling. (microtiles are 2D rectangles)
       * If the box width and height is > 4, the block size will be 8x8.
       * If Y is <= 4, the threads will spill over to X.
       * If X is <= 4, the threads will spill over to Y, then Z if they aren't small.
       */
      block_y = util_next_power_of_two(MIN2(height, 8));
      block_x = util_next_power_of_two(MIN2(width, 64 / block_y));
      block_y = util_next_power_of_two(MIN2(height, 64 / block_x));
      block_z = util_next_power_of_two(MIN2(depth, 64 / (block_x * block_y)));
      block_x = 64 / (block_y * block_z);
   }

   struct pipe_grid_info grid = {0};
   unsigned wg_dim = set_work_size(&grid, block_x, block_y, block_z, width, height, depth);

   /* Get the shader key. */
   union si_compute_blit_shader_key options;
   options.key = 0;

   options.is_clear = is_clear;
   options.wg_dim = wg_dim;
   options.has_start_xyz = start_x || start_y || start_z;
   options.dst_is_1d = info->dst.resource->target == PIPE_TEXTURE_1D ||
                       info->dst.resource->target == PIPE_TEXTURE_1D_ARRAY;
   options.dst_is_msaa = dst_samples > 1;
   options.dst_has_z = info->dst.resource->target == PIPE_TEXTURE_3D ||
                       info->dst.resource->target == PIPE_TEXTURE_CUBE ||
                       info->dst.resource->target == PIPE_TEXTURE_1D_ARRAY ||
                       info->dst.resource->target == PIPE_TEXTURE_2D_ARRAY ||
                       info->dst.resource->target == PIPE_TEXTURE_CUBE_ARRAY;
   options.last_dst_channel = util_format_get_last_component(info->dst.format);

   /* ACO doesn't support D16 on GFX8 */
   bool has_d16 = sctx->gfx_level >= (sctx->screen->use_aco ? GFX9 : GFX8);

   if (is_clear) {
      assert(dst_samples <= 8);
      options.log_samples = util_logbase2(dst_samples);
      options.d16 = has_d16 &&
                    max_dst_chan_size <= (util_format_is_float(info->dst.format) ||
                                          util_format_is_pure_integer(info->dst.format) ? 16 : 11);
   } else {
      options.src_is_1d = info->src.resource->target == PIPE_TEXTURE_1D ||
                          info->src.resource->target == PIPE_TEXTURE_1D_ARRAY;
      options.src_is_msaa = src_samples > 1;
      options.src_has_z = info->src.resource->target == PIPE_TEXTURE_3D ||
                          info->src.resource->target == PIPE_TEXTURE_CUBE ||
                          info->src.resource->target == PIPE_TEXTURE_1D_ARRAY ||
                          info->src.resource->target == PIPE_TEXTURE_2D_ARRAY ||
                          info->src.resource->target == PIPE_TEXTURE_CUBE_ARRAY;
      /* Resolving integer formats only copies sample 0. log_samples is then unused. */
      options.sample0_only = sample0_only;
      unsigned num_samples = MAX2(src_samples, dst_samples);
      assert(num_samples <= 8);
      options.log_samples = sample0_only ? 0 : util_logbase2(num_samples);
      options.x_clamp_to_edge = si_should_blit_clamp_to_edge(info, BITFIELD_BIT(0));
      options.y_clamp_to_edge = si_should_blit_clamp_to_edge(info, BITFIELD_BIT(1));
      options.flip_x = info->src.box.width < 0;
      options.flip_y = info->src.box.height < 0;
      options.sint_to_uint = util_format_is_pure_sint(info->src.format) &&
                             util_format_is_pure_uint(info->dst.format);
      options.uint_to_sint = util_format_is_pure_uint(info->src.format) &&
                             util_format_is_pure_sint(info->dst.format);
      options.dst_is_srgb = util_format_is_srgb(info->dst.format);
      options.last_src_channel = MIN2(util_format_get_last_component(info->src.format),
                                      options.last_dst_channel);
      options.use_integer_one = util_format_is_pure_integer(info->dst.format) &&
                                options.last_src_channel < options.last_dst_channel &&
                                options.last_dst_channel == 3;
      bool is_resolve = options.src_is_msaa && !options.dst_is_msaa && !options.sample0_only;
      options.d16 = has_d16 &&
                    /* Blitting FP16 using D16 has precision issues. Resolving has precision
                     * issues all the way down to R11G11B10_FLOAT. */
                    MIN2(max_dst_chan_size, max_src_chan_size) <=
                    (util_format_is_pure_integer(info->dst.format) ?
                        (options.sint_to_uint || options.uint_to_sint ? 10 : 16) :
                        (is_resolve ? 10 : 11));
   }

   void *shader = _mesa_hash_table_u64_search(sctx->cs_blit_shaders, options.key);
   if (!shader) {
      shader = si_create_blit_cs(sctx, &options);
      _mesa_hash_table_u64_insert(sctx->cs_blit_shaders, options.key, shader);
   }

   sctx->cs_user_data[0] = (info->src.box.x & 0xffff) | ((info->dst.box.x & 0xffff) << 16);
   sctx->cs_user_data[1] = (info->src.box.y & 0xffff) | ((info->dst.box.y & 0xffff) << 16);
   sctx->cs_user_data[2] = (info->src.box.z & 0xffff) | ((info->dst.box.z & 0xffff) << 16);
   sctx->cs_user_data[3] = (start_x & 0xff) | ((start_y & 0xff) << 8) | ((start_z & 0xff) << 16);

   if (is_clear) {
      union pipe_color_union final_value;
      memcpy(&final_value, clear_color, sizeof(final_value));

      /* Do the conversion to sRGB here instead of the shader. */
      if (util_format_is_srgb(info->dst.format)) {
         for (int i = 0; i < 3; i++)
            final_value.f[i] = util_format_linear_to_srgb_float(final_value.f[i]);
      }

      if (options.d16) {
         enum pipe_format data_format;

         if (util_format_is_pure_uint(info->dst.format))
            data_format = PIPE_FORMAT_R16G16B16A16_UINT;
         else if (util_format_is_pure_sint(info->dst.format))
            data_format = PIPE_FORMAT_R16G16B16A16_SINT;
         else
            data_format = PIPE_FORMAT_R16G16B16A16_FLOAT;

         util_pack_color_union(data_format, (union util_color *)&sctx->cs_user_data[4],
                               &final_value);
      } else {
         memcpy(&sctx->cs_user_data[4], &final_value, sizeof(final_value));
      }
   }

   /* Shader images. */
   struct pipe_image_view image[2];
   unsigned dst_index = is_clear ? 0 : 1;

   if (!is_clear) {
      image[0].resource = info->src.resource;
      image[0].shader_access = image[0].access = PIPE_IMAGE_ACCESS_READ | src_access;
      image[0].format = info->src.format;
      image[0].u.tex.level = info->src.level;
      image[0].u.tex.first_layer = 0;
      image[0].u.tex.last_layer = util_max_layer(info->src.resource, info->src.level);
   }

   image[dst_index].resource = info->dst.resource;
   image[dst_index].shader_access = image[dst_index].access = PIPE_IMAGE_ACCESS_WRITE | dst_access;
   image[dst_index].format = info->dst.format;
   image[dst_index].u.tex.level = info->dst.level;
   image[dst_index].u.tex.first_layer = 0;
   image[dst_index].u.tex.last_layer = util_max_layer(info->dst.resource, info->dst.level);

   si_launch_grid_internal_images(sctx, image, is_clear ? 1 : 2, &grid, shader, flags |
                                  (info->render_condition_enable ? SI_OP_CS_RENDER_COND_ENABLE : 0));
   return true;
}
