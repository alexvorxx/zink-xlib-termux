#include "zink_batch.h"
#include "zink_context.h"
#include "zink_descriptors.h"
#include "zink_framebuffer.h"
#include "zink_kopper.h"
#include "zink_program.h"
#include "zink_query.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_surface.h"

#ifdef VK_USE_PLATFORM_METAL_EXT
#include "QuartzCore/CAMetalLayer.h"
#endif
#include "wsi_common.h"

#define MAX_VIEW_COUNT 500

void
debug_describe_zink_batch_state(char *buf, const struct zink_batch_state *ptr)
{
   sprintf(buf, "zink_batch_state");
}

/* this resets the batch usage and tracking for a resource object */
static void
reset_obj(struct zink_screen *screen, struct zink_batch_state *bs, struct zink_resource_object *obj)
{
   /* if no batch usage exists after removing the usage from 'bs', this resource is considered fully idle */
   if (!zink_resource_object_usage_unset(obj, bs)) {
      /* the resource is idle, so reset all access/reordering info */
      obj->unordered_read = false;
      obj->unordered_write = false;
      obj->access = 0;
      obj->access_stage = 0;
      /* also prune dead view objects */
      simple_mtx_lock(&obj->view_lock);
      if (obj->is_buffer) {
         while (util_dynarray_contains(&obj->views, VkBufferView))
            VKSCR(DestroyBufferView)(screen->dev, util_dynarray_pop(&obj->views, VkBufferView), NULL);
      } else {
         while (util_dynarray_contains(&obj->views, VkImageView))
            VKSCR(DestroyImageView)(screen->dev, util_dynarray_pop(&obj->views, VkImageView), NULL);
      }
      obj->view_prune_count = 0;
      obj->view_prune_timeline = 0;
      simple_mtx_unlock(&obj->view_lock);
   } else if (util_dynarray_num_elements(&obj->views, VkBufferView) > MAX_VIEW_COUNT && !zink_bo_has_unflushed_usage(obj->bo)) {
      /* avoid ballooning from too many views on always-used resources: */
      simple_mtx_lock(&obj->view_lock);
      /* ensure no existing view pruning is queued, double check elements in case pruning just finished */
      if (!obj->view_prune_timeline && util_dynarray_num_elements(&obj->views, VkBufferView) > MAX_VIEW_COUNT) {
         /* prune all existing views */
         obj->view_prune_count = util_dynarray_num_elements(&obj->views, VkBufferView);
         /* prune them when the views will definitely not be in use */
         obj->view_prune_timeline = MAX2(obj->bo->reads ? obj->bo->reads->usage : 0,
                                         obj->bo->writes ? obj->bo->writes->usage : 0);
      }
      simple_mtx_unlock(&obj->view_lock);
   }
   /* resource objects are not unrefed here;
    * this is typically the last ref on a resource object, and destruction will
    * usually trigger an ioctl, so defer deletion to the submit thread to avoid blocking
    */
   util_dynarray_append(&bs->unref_resources, struct zink_resource_object*, obj);
}

/* reset all the resource objects in a given batch object list */
static void
reset_obj_list(struct zink_screen *screen, struct zink_batch_state *bs, struct zink_batch_obj_list *list)
{
   for (unsigned i = 0; i < list->num_buffers; i++)
      reset_obj(screen, bs, list->objs[i]);
   list->num_buffers = 0;
}

/* reset a given batch state */
void
zink_reset_batch_state(struct zink_context *ctx, struct zink_batch_state *bs)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);

   VkResult result = VKSCR(ResetCommandPool)(screen->dev, bs->cmdpool, 0);
   if (result != VK_SUCCESS)
      mesa_loge("ZINK: vkResetCommandPool failed (%s)", vk_Result_to_str(result));

   /* unref/reset all used resources */
   reset_obj_list(screen, bs, &bs->real_objs);
   reset_obj_list(screen, bs, &bs->slab_objs);
   reset_obj_list(screen, bs, &bs->sparse_objs);
   while (util_dynarray_contains(&bs->swapchain_obj, struct zink_resource_object*)) {
      struct zink_resource_object *obj = util_dynarray_pop(&bs->swapchain_obj, struct zink_resource_object*);
      reset_obj(screen, bs, obj);
   }

   /* this is where bindless texture/buffer ids get recycled */
   for (unsigned i = 0; i < 2; i++) {
      while (util_dynarray_contains(&bs->bindless_releases[i], uint32_t)) {
         uint32_t handle = util_dynarray_pop(&bs->bindless_releases[i], uint32_t);
         bool is_buffer = ZINK_BINDLESS_IS_BUFFER(handle);
         struct util_idalloc *ids = i ? &ctx->di.bindless[is_buffer].img_slots : &ctx->di.bindless[is_buffer].tex_slots;
         util_idalloc_free(ids, is_buffer ? handle - ZINK_MAX_BINDLESS_HANDLES : handle);
      }
   }

   /* queries must only be destroyed once they are inactive */
   set_foreach_remove(&bs->active_queries, entry) {
      struct zink_query *query = (void*)entry->key;
      zink_prune_query(screen, bs, query);
   }

   /* framebuffers are appended to the batch state in which they are destroyed
    * to ensure deferred deletion without destroying in-use objects
    */
   util_dynarray_foreach(&bs->dead_framebuffers, struct zink_framebuffer*, fb) {
      zink_framebuffer_reference(screen, fb, NULL);
   }
   util_dynarray_clear(&bs->dead_framebuffers);
   /* samplers are appended to the batch state in which they are destroyed
    * to ensure deferred deletion without destroying in-use objects
    */
   util_dynarray_foreach(&bs->zombie_samplers, VkSampler, samp) {
      VKSCR(DestroySampler)(screen->dev, *samp, NULL);
   }
   util_dynarray_clear(&bs->zombie_samplers);
   util_dynarray_clear(&bs->persistent_resources);

   zink_batch_descriptor_reset(screen, bs);

   /* programs are refcounted and batch-tracked */
   set_foreach_remove(&bs->programs, entry) {
      struct zink_program *pg = (struct zink_program*)entry->key;
      zink_batch_usage_unset(&pg->batch_uses, bs);
      zink_program_reference(screen, &pg, NULL);
   }

   bs->resource_size = 0;
   bs->signal_semaphore = VK_NULL_HANDLE;
   util_dynarray_clear(&bs->wait_semaphore_stages);

   bs->present = VK_NULL_HANDLE;
   /* semaphores are not destroyed here;
    * destroying semaphores triggers ioctls, so defer deletion to the submit thread to avoid blocking
    */
   memcpy(&bs->unref_semaphores, &bs->acquires, sizeof(struct util_dynarray));
   util_dynarray_init(&bs->acquires, NULL);
   while (util_dynarray_contains(&bs->wait_semaphores, VkSemaphore))
      util_dynarray_append(&bs->unref_semaphores, VkSemaphore, util_dynarray_pop(&bs->wait_semaphores, VkSemaphore));
   util_dynarray_init(&bs->wait_semaphores, NULL);
   bs->swapchain = NULL;

   /* only reset submitted here so that tc fence desync can pick up the 'completed' flag
    * before the state is reused
    */
   bs->fence.submitted = false;
   bs->has_barriers = false;
   if (bs->fence.batch_id)
      zink_screen_update_last_finished(screen, bs->fence.batch_id);
   bs->submit_count++;
   bs->fence.batch_id = 0;
   bs->usage.usage = 0;
   bs->next = NULL;
   bs->last_added_obj = NULL;
}

/* this is where deferred resource unrefs occur */
static void
unref_resources(struct zink_screen *screen, struct zink_batch_state *bs)
{
   while (util_dynarray_contains(&bs->unref_resources, struct zink_resource_object*)) {
      struct zink_resource_object *obj = util_dynarray_pop(&bs->unref_resources, struct zink_resource_object*);
      /* view pruning may be deferred to avoid ballooning */
      if (obj->view_prune_timeline && zink_screen_check_last_finished(screen, obj->view_prune_timeline)) {
         simple_mtx_lock(&obj->view_lock);
         /* check again under lock in case multi-context use is in the same place */
         if (obj->view_prune_timeline && zink_screen_check_last_finished(screen, obj->view_prune_timeline)) {
            /* prune `view_prune_count` views */
            if (obj->is_buffer) {
               VkBufferView *views = obj->views.data;
               for (unsigned i = 0; i < obj->view_prune_count; i++)
                  VKSCR(DestroyBufferView)(screen->dev, views[i], NULL);
            } else {
               VkImageView *views = obj->views.data;
               for (unsigned i = 0; i < obj->view_prune_count; i++)
                  VKSCR(DestroyImageView)(screen->dev, views[i], NULL);
            }
            size_t offset = obj->view_prune_count * sizeof(VkBufferView);
            uint8_t *data = obj->views.data;
            /* shift the view array to the start */
            memcpy(data, data + offset, obj->views.size - offset);
            /* adjust the array size */
            obj->views.size -= offset;
            obj->view_prune_count = 0;
            obj->view_prune_timeline = 0;
         }
         simple_mtx_unlock(&obj->view_lock);
      }
      /* this is typically where resource objects get destroyed */
      zink_resource_object_reference(screen, &obj, NULL);
   }
   while (util_dynarray_contains(&bs->unref_semaphores, VkSemaphore))
      VKSCR(DestroySemaphore)(screen->dev, util_dynarray_pop(&bs->unref_semaphores, VkSemaphore), NULL);
}

/* utility for resetting a batch state; called on context destruction */
void
zink_clear_batch_state(struct zink_context *ctx, struct zink_batch_state *bs)
{
   bs->fence.completed = true;
   zink_reset_batch_state(ctx, bs);
   unref_resources(zink_screen(ctx->base.screen), bs);
}

/* utility for managing the singly-linked batch state list */
static void
pop_batch_state(struct zink_context *ctx)
{
   const struct zink_batch_state *bs = ctx->batch_states;
   ctx->batch_states = bs->next;
   ctx->batch_states_count--;
   if (ctx->last_fence == &bs->fence)
      ctx->last_fence = NULL;
}

/* reset all batch states and append to the free state list
 * only usable after a full stall
 */
void
zink_batch_reset_all(struct zink_context *ctx)
{
   simple_mtx_lock(&ctx->batch_mtx);
   while (ctx->batch_states) {
      struct zink_batch_state *bs = ctx->batch_states;
      bs->fence.completed = true;
      pop_batch_state(ctx);
      zink_reset_batch_state(ctx, bs);
      if (ctx->last_free_batch_state)
         ctx->last_free_batch_state->next = bs;
      else
         ctx->free_batch_states = bs;
      ctx->last_free_batch_state = bs;
   }
   simple_mtx_unlock(&ctx->batch_mtx);
}

/* called only on context destruction */
void
zink_batch_state_destroy(struct zink_screen *screen, struct zink_batch_state *bs)
{
   if (!bs)
      return;

   util_queue_fence_destroy(&bs->flush_completed);

   cnd_destroy(&bs->usage.flush);
   mtx_destroy(&bs->usage.mtx);

   if (bs->fence.fence)
      VKSCR(DestroyFence)(screen->dev, bs->fence.fence, NULL);

   if (bs->cmdbuf)
      VKSCR(FreeCommandBuffers)(screen->dev, bs->cmdpool, 1, &bs->cmdbuf);
   if (bs->barrier_cmdbuf)
      VKSCR(FreeCommandBuffers)(screen->dev, bs->cmdpool, 1, &bs->barrier_cmdbuf);
   if (bs->cmdpool)
      VKSCR(DestroyCommandPool)(screen->dev, bs->cmdpool, NULL);
   free(bs->real_objs.objs);
   free(bs->slab_objs.objs);
   free(bs->sparse_objs.objs);
   util_dynarray_fini(&bs->swapchain_obj);
   util_dynarray_fini(&bs->zombie_samplers);
   util_dynarray_fini(&bs->dead_framebuffers);
   util_dynarray_fini(&bs->unref_resources);
   util_dynarray_fini(&bs->bindless_releases[0]);
   util_dynarray_fini(&bs->bindless_releases[1]);
   util_dynarray_fini(&bs->acquires);
   util_dynarray_fini(&bs->unref_semaphores);
   util_dynarray_fini(&bs->acquire_flags);
   zink_batch_descriptor_deinit(screen, bs);
   ralloc_free(bs);
}

/* batch states are created:
 * - on context creation
 * - dynamically up to a threshold if no free ones are available
 */
static struct zink_batch_state *
create_batch_state(struct zink_context *ctx)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch_state *bs = rzalloc(NULL, struct zink_batch_state);

   bs->have_timelines = ctx->have_timelines;

   VkCommandPoolCreateInfo cpci = {0};
   cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   cpci.queueFamilyIndex = screen->gfx_queue;
   VkResult result = VKSCR(CreateCommandPool)(screen->dev, &cpci, NULL, &bs->cmdpool);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateCommandPool failed (%s)", vk_Result_to_str(result));
      goto fail;
   }

   VkCommandBufferAllocateInfo cbai = {0};
   cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cbai.commandPool = bs->cmdpool;
   cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cbai.commandBufferCount = 1;

   result = VKSCR(AllocateCommandBuffers)(screen->dev, &cbai, &bs->cmdbuf);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkAllocateCommandBuffers failed (%s)", vk_Result_to_str(result));
      goto fail;
   }

   result = VKSCR(AllocateCommandBuffers)(screen->dev, &cbai, &bs->barrier_cmdbuf);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkAllocateCommandBuffers failed (%s)", vk_Result_to_str(result));
      goto fail;
   }

#define SET_CREATE_OR_FAIL(ptr) \
   if (!_mesa_set_init(ptr, bs, _mesa_hash_pointer, _mesa_key_pointer_equal)) \
      goto fail

   bs->ctx = ctx;

   SET_CREATE_OR_FAIL(&bs->programs);
   SET_CREATE_OR_FAIL(&bs->active_queries);
   util_dynarray_init(&bs->wait_semaphores, NULL);
   util_dynarray_init(&bs->wait_semaphore_stages, NULL);
   util_dynarray_init(&bs->zombie_samplers, NULL);
   util_dynarray_init(&bs->dead_framebuffers, NULL);
   util_dynarray_init(&bs->persistent_resources, NULL);
   util_dynarray_init(&bs->unref_resources, NULL);
   util_dynarray_init(&bs->acquires, NULL);
   util_dynarray_init(&bs->unref_semaphores, NULL);
   util_dynarray_init(&bs->acquire_flags, NULL);
   util_dynarray_init(&bs->bindless_releases[0], NULL);
   util_dynarray_init(&bs->bindless_releases[1], NULL);
   util_dynarray_init(&bs->swapchain_obj, NULL);

   cnd_init(&bs->usage.flush);
   mtx_init(&bs->usage.mtx, mtx_plain);
   memset(&bs->buffer_indices_hashlist, -1, sizeof(bs->buffer_indices_hashlist));

   if (!zink_batch_descriptor_init(screen, bs))
      goto fail;

   if (!screen->info.have_KHR_timeline_semaphore) {
      VkFenceCreateInfo fci = {0};
      fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   
      if (VKSCR(CreateFence)(screen->dev, &fci, NULL, &bs->fence.fence) != VK_SUCCESS)
      goto fail;
   }

   util_queue_fence_init(&bs->flush_completed);

   return bs;
fail:
   zink_batch_state_destroy(screen, bs);
   return NULL;
}

/* a batch state is considered "free" if it is both submitted and completed */
static inline bool
find_unused_state(struct zink_batch_state *bs)
{
   struct zink_fence *fence = &bs->fence;
   /* we can't reset these from fence_finish because threads */
   bool completed = p_atomic_read(&fence->completed);
   bool submitted = p_atomic_read(&fence->submitted);
   return submitted && completed;
}

/* find a "free" batch state */
static struct zink_batch_state *
get_batch_state(struct zink_context *ctx, struct zink_batch *batch)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch_state *bs = NULL;

   simple_mtx_lock(&ctx->batch_mtx);

   /* try from the ones that are known to be free first */
   if (ctx->free_batch_states) {
      bs = ctx->free_batch_states;
      ctx->free_batch_states = bs->next;
      if (bs == ctx->last_free_batch_state)
         ctx->last_free_batch_state = NULL;
   }

   if (!bs && ctx->batch_states) {
      /* states are stored sequentially, so if the first one doesn't work, none of them will */
      if (zink_screen_check_last_finished(screen, ctx->batch_states->fence.batch_id) ||
          find_unused_state(ctx->batch_states)) {
         bs = ctx->batch_states;
         pop_batch_state(ctx);
      }
   }

   simple_mtx_unlock(&ctx->batch_mtx);
   
   if (bs) {
	  if (bs->fence.submitted && !bs->fence.completed)
         /* this fence is already done, so we need vulkan to release the cmdbuf */
         zink_vkfence_wait(screen, &bs->fence, PIPE_TIMEOUT_INFINITE);

      zink_reset_batch_state(ctx, bs);
   } else {
      if (!batch->state) {
         /* this is batch init, so create a few more states for later use */
         for (int i = 0; i < 3; i++) {
            struct zink_batch_state *state = create_batch_state(ctx);
            if (ctx->last_free_batch_state)
               ctx->last_free_batch_state->next = state;
            else
               ctx->free_batch_states = state;
            ctx->last_free_batch_state = state;
         }
      }
      /* no batch states were available: make a new one */
      bs = create_batch_state(ctx);
   }
   return bs;
}

/* reset the batch object: get a new state and unset 'has_work' to disable flushing */
void
zink_reset_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   batch->state = get_batch_state(ctx, batch);
   assert(batch->state);

   batch->has_work = false;
}

/* called on context creation and after flushing an old batch */
void
zink_start_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   zink_reset_batch(ctx, batch);

   batch->state->usage.unflushed = true;

   VkCommandBufferBeginInfo cbbi = {0};
   cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

   VkResult result = VKCTX(BeginCommandBuffer)(batch->state->cmdbuf, &cbbi);
   if (result != VK_SUCCESS)
      mesa_loge("ZINK: vkBeginCommandBuffer failed (%s)", vk_Result_to_str(result));
   
   result = VKCTX(BeginCommandBuffer)(batch->state->barrier_cmdbuf, &cbbi);
   if (result != VK_SUCCESS)
      mesa_loge("ZINK: vkBeginCommandBuffer failed (%s)", vk_Result_to_str(result));

   batch->state->fence.completed = false;
   if (ctx->last_fence) {
      struct zink_batch_state *last_state = zink_batch_state(ctx->last_fence);
      batch->last_batch_usage = &last_state->usage;
   }

   if (!ctx->queries_disabled)
      zink_resume_queries(ctx, batch);
}

/* common operations to run post submit; split out for clarity */
static void
post_submit(void *data, void *gdata, int thread_index)
{
   struct zink_batch_state *bs = data;
   struct zink_screen *screen = zink_screen(bs->ctx->base.screen);

   if (bs->is_device_lost) {
      if (bs->ctx->reset.reset)
         bs->ctx->reset.reset(bs->ctx->reset.data, PIPE_GUILTY_CONTEXT_RESET);
      else if (screen->abort_on_hang && !screen->robust_ctx_count)
         /* if nothing can save us, abort */
         abort();
      screen->device_lost = true;
   } else if (bs->ctx->batch_states_count > 5000) {
      /* throttle in case something crazy is happening */
      //zink_screen_timeline_wait(screen, bs->fence.batch_id - 2500, PIPE_TIMEOUT_INFINITE);
      zink_screen_batch_id_wait(screen, bs->fence.batch_id - 2500, PIPE_TIMEOUT_INFINITE);
   }
   /* this resets the buffer hashlist for the state's next use */
   memset(&bs->buffer_indices_hashlist, -1, sizeof(bs->buffer_indices_hashlist));
}

static void
submit_queue(void *data, void *gdata, int thread_index)
{
   struct zink_batch_state *bs = data;
   struct zink_context *ctx = bs->ctx;
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   VkSubmitInfo si[2] = {0};
   int num_si = 2;
   while (!bs->fence.batch_id)
      bs->fence.batch_id = (uint32_t)p_atomic_inc_return(&screen->curr_batch);
   bs->usage.usage = bs->fence.batch_id;
   bs->usage.unflushed = false;

   //if (screen->last_finished > bs->fence.batch_id && bs->fence.batch_id == 1) {
   if (ctx->have_timelines && screen->last_finished > bs->fence.batch_id && bs->fence.batch_id == 1) {   
      if (!zink_screen_init_semaphore(screen)) {
         debug_printf("timeline init failed, things are about to go dramatically wrong.");
		 ctx->have_timelines = false;
      }
   }
   
   if (VKSCR(ResetFences)(screen->dev, 1, &bs->fence.fence) != VK_SUCCESS) {
      mesa_loge("ZINK: vkResetFences failed");
   }

   uint64_t batch_id = bs->fence.batch_id;
   /* first submit is just for acquire waits since they have a separate array */
   si[0].sType = si[1].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si[0].waitSemaphoreCount = util_dynarray_num_elements(&bs->acquires, VkSemaphore);
   si[0].pWaitSemaphores = bs->acquires.data;
   while (util_dynarray_num_elements(&bs->acquire_flags, VkPipelineStageFlags) < si[0].waitSemaphoreCount) {
      VkPipelineStageFlags mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      util_dynarray_append(&bs->acquire_flags, VkPipelineStageFlags, mask);
   }
   assert(util_dynarray_num_elements(&bs->acquires, VkSemaphore) <= util_dynarray_num_elements(&bs->acquire_flags, VkPipelineStageFlags));
   si[0].pWaitDstStageMask = bs->acquire_flags.data;

   if (si[0].waitSemaphoreCount == 0)
     num_si--;

   /* then the real submit */
   si[1].waitSemaphoreCount = util_dynarray_num_elements(&bs->wait_semaphores, VkSemaphore);
   si[1].pWaitSemaphores = bs->wait_semaphores.data;
   si[1].pWaitDstStageMask = bs->wait_semaphore_stages.data;
   si[1].commandBufferCount = bs->has_barriers ? 2 : 1;
   VkCommandBuffer cmdbufs[2] = {
      bs->barrier_cmdbuf,
      bs->cmdbuf,
   };
   si[1].pCommandBuffers = bs->has_barriers ? cmdbufs : &cmdbufs[1];

   VkSemaphore signals[3];
   si[1].signalSemaphoreCount = !!bs->signal_semaphore;
   signals[0] = bs->signal_semaphore;
   si[1].pSignalSemaphores = signals;
   VkTimelineSemaphoreSubmitInfo tsi = {0};
   uint64_t signal_values[2] = {0};

   if (bs->have_timelines) {
      tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
      si[1].pNext = &tsi;
      tsi.pSignalSemaphoreValues = signal_values;
      signal_values[si[1].signalSemaphoreCount] = batch_id;
      signals[si[1].signalSemaphoreCount++] = screen->sem;
      tsi.signalSemaphoreValueCount = si[1].signalSemaphoreCount;
   }

   if (bs->present)
      signals[si[1].signalSemaphoreCount++] = bs->present;
   tsi.signalSemaphoreValueCount = si[1].signalSemaphoreCount;

   VkResult result = VKSCR(EndCommandBuffer)(bs->cmdbuf);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkEndCommandBuffer failed (%s)", vk_Result_to_str(result));
      bs->is_device_lost = true;
      goto end;
   }
   if (bs->has_barriers) {
      result = VKSCR(EndCommandBuffer)(bs->barrier_cmdbuf);
      if (result != VK_SUCCESS) {
         mesa_loge("ZINK: vkEndCommandBuffer failed (%s)", vk_Result_to_str(result));
         bs->is_device_lost = true;
         goto end;
      }
   }

   while (util_dynarray_contains(&bs->persistent_resources, struct zink_resource_object*)) {
      struct zink_resource_object *obj = util_dynarray_pop(&bs->persistent_resources, struct zink_resource_object*);
       VkMappedMemoryRange range = zink_resource_init_mem_range(screen, obj, 0, obj->size);

       result = VKSCR(FlushMappedMemoryRanges)(screen->dev, 1, &range);
       if (result != VK_SUCCESS) {
          mesa_loge("ZINK: vkFlushMappedMemoryRanges failed (%s)", vk_Result_to_str(result));
       }
   }

   simple_mtx_lock(&screen->queue_lock);
   result = VKSCR(QueueSubmit)(screen->queue, num_si, num_si == 2 ? si : &si[1], bs->fence.fence);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkQueueSubmit failed (%s)", vk_Result_to_str(result));
      bs->is_device_lost = true;
   }
   simple_mtx_unlock(&screen->queue_lock);
   bs->submit_count++;
end:
   cnd_broadcast(&bs->usage.flush);

   p_atomic_set(&bs->fence.submitted, true);
   unref_resources(screen, bs);
}

/* called during flush */
void
zink_end_batch(struct zink_context *ctx, struct zink_batch *batch)
{
   if (!ctx->queries_disabled)
      zink_suspend_queries(ctx, batch);

   tc_driver_internal_flush_notify(ctx->tc);

   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_batch_state *bs;

   simple_mtx_lock(&ctx->batch_mtx);

   /* oom flushing is triggered to handle stupid piglit tests like streaming-texture-leak */
   if (ctx->oom_flush || ctx->batch_states_count > 25) {
      assert(!ctx->batch_states_count || ctx->batch_states);
      while (ctx->batch_states) {
         bs = ctx->batch_states;
         struct zink_fence *fence = &bs->fence;
         /* once an incomplete state is reached, no more will be complete */
         //if (!zink_check_batch_completion(ctx, fence->batch_id))
	 if (!zink_check_batch_completion(ctx, fence->batch_id, true))	 
            break;

         if (bs->fence.submitted && !bs->fence.completed)
            /* this fence is already done, so we need vulkan to release the cmdbuf */
            zink_vkfence_wait(screen, &bs->fence, PIPE_TIMEOUT_INFINITE);

         pop_batch_state(ctx);
         zink_reset_batch_state(ctx, bs);
         if (ctx->last_free_batch_state)
            ctx->last_free_batch_state->next = bs;
         else
            ctx->free_batch_states = bs;
         ctx->last_free_batch_state = bs;
      }
      if (ctx->batch_states_count > 50)
         ctx->oom_flush = true;
   }

   bs = batch->state;
   if (ctx->last_fence)
      zink_batch_state(ctx->last_fence)->next = bs;
   else {
      assert(!ctx->batch_states);
      ctx->batch_states = bs;
   }
   ctx->last_fence = &bs->fence;
   ctx->batch_states_count++;

   simple_mtx_unlock(&ctx->batch_mtx);

   batch->work_count = 0;

   /* this is swapchain presentation semaphore handling */
   if (batch->swapchain) {
      if (zink_kopper_acquired(batch->swapchain->obj->dt, batch->swapchain->obj->dt_idx) && !batch->swapchain->obj->present) {
         batch->state->present = zink_kopper_present(screen, batch->swapchain);
         batch->state->swapchain = batch->swapchain;
      }
      batch->swapchain = NULL;
   }

   if (screen->device_lost)
      return;

   if (screen->threaded) {
      util_queue_add_job(&screen->flush_queue, bs, &bs->flush_completed,
                         submit_queue, post_submit, 0);
   } else {
      submit_queue(bs, NULL, 0);
      post_submit(bs, NULL, 0);
   }
}

static int
batch_find_resource(struct zink_batch_state *bs, struct zink_resource_object *obj, struct zink_batch_obj_list *list)
{
   unsigned hash = obj->bo->unique_id & (BUFFER_HASHLIST_SIZE-1);
   int i = bs->buffer_indices_hashlist[hash];

   /* not found or found */
   if (i < 0 || (i < list->num_buffers && list->objs[i] == obj))
      return i;

   /* Hash collision, look for the BO in the list of list->objs linearly. */
   for (int i = list->num_buffers - 1; i >= 0; i--) {
      if (list->objs[i] == obj) {
         /* Put this buffer in the hash list.
          * This will prevent additional hash collisions if there are
          * several consecutive lookup_buffer calls for the same buffer.
          *
          * Example: Assuming list->objs A,B,C collide in the hash list,
          * the following sequence of list->objs:
          *         AAAAAAAAAAABBBBBBBBBBBBBBCCCCCCCC
          * will collide here: ^ and here:   ^,
          * meaning that we should get very few collisions in the end. */
         bs->buffer_indices_hashlist[hash] = i & (BUFFER_HASHLIST_SIZE-1);
         return i;
      }
   }
   return -1;
}

void
zink_batch_reference_resource_rw(struct zink_batch *batch, struct zink_resource *res, bool write)
{
   /* if the resource already has usage of any sort set for this batch, */
   if (!zink_resource_usage_matches(res, batch->state) ||
       /* or if it's bound somewhere */
       !zink_resource_has_binds(res))
      /* then it already has a batch ref and doesn't need one here */
      zink_batch_reference_resource(batch, res);
   zink_batch_resource_usage_set(batch, res, write, res->obj->is_buffer);
}

void
zink_batch_add_wait_semaphore(struct zink_batch *batch, VkSemaphore sem)
{
   util_dynarray_append(&batch->state->acquires, VkSemaphore, sem);
}

static bool
batch_ptr_add_usage(struct zink_batch *batch, struct set *s, void *ptr)
{
   bool found = false;
   _mesa_set_search_or_add(s, ptr, &found);
   return !found;
}

/* this is a vague, handwave-y estimate */
ALWAYS_INLINE static void
check_oom_flush(struct zink_context *ctx, const struct zink_batch *batch)
{
   const VkDeviceSize resource_size = batch->state->resource_size;
   if (resource_size >= zink_screen(ctx->base.screen)->clamp_video_mem) {
       ctx->oom_flush = true;
       ctx->oom_stall = true;
    }
}

/* this adds a ref (batch tracking) */
void
zink_batch_reference_resource(struct zink_batch *batch, struct zink_resource *res)
{
   if (!zink_batch_reference_resource_move(batch, res))
      zink_resource_object_reference(NULL, NULL, res->obj);
}

/* this adds batch usage */
bool
zink_batch_reference_resource_move(struct zink_batch *batch, struct zink_resource *res)
{
   struct zink_batch_state *bs = batch->state;

   /* swapchains are special */
   if (zink_is_swapchain(res)) {
      struct zink_resource_object **swapchains = bs->swapchain_obj.data;
      unsigned count = util_dynarray_num_elements(&bs->swapchain_obj, struct zink_resource_object*);
      for (unsigned i = 0; i < count; i++) {
         if (swapchains[i] == res->obj)
            return true;
      }
      util_dynarray_append(&bs->swapchain_obj, struct zink_resource_object*, res->obj);
      return false;
   }
   /* Fast exit for no-op calls.
    * This is very effective with suballocators and linear uploaders that
    * are outside of the winsys.
    */
   if (res->obj == bs->last_added_obj)
      return true;

   struct zink_bo *bo = res->obj->bo;
   struct zink_batch_obj_list *list;
   if (!(res->base.b.flags & PIPE_RESOURCE_FLAG_SPARSE)) {
      if (!bo->mem) {
         list = &bs->slab_objs;
      } else {
         list = &bs->real_objs;
      }
   } else {
      list = &bs->sparse_objs;
   }
   int idx = batch_find_resource(bs, res->obj, list);
   if (idx >= 0)
      return true;

   if (list->num_buffers >= list->max_buffers) {
      unsigned new_max = MAX2(list->max_buffers + 16, (unsigned)(list->max_buffers * 1.3));
      struct zink_resource_object **objs = realloc(list->objs, new_max * sizeof(void*));
      if (!objs) {
         /* things are about to go dramatically wrong anyway */
         mesa_loge("zink: buffer list realloc failed due to oom!\n");
         abort();
      }
      list->objs = objs;
      list->max_buffers = new_max;
   }
   idx = list->num_buffers++;
   list->objs[idx] = res->obj;
   unsigned hash = bo->unique_id & (BUFFER_HASHLIST_SIZE-1);
   bs->buffer_indices_hashlist[hash] = idx & 0x7fff;
   bs->last_added_obj = res->obj;
   if (!(res->base.b.flags & PIPE_RESOURCE_FLAG_SPARSE)) {
      bs->resource_size += res->obj->size;
   } else {
      // TODO: check backing pages
   }
   check_oom_flush(batch->state->ctx, batch);
   batch->has_work = true;
   return false;
}

/* this is how programs achieve deferred deletion */
void
zink_batch_reference_program(struct zink_batch *batch,
                             struct zink_program *pg)
{
   if (zink_batch_usage_matches(pg->batch_uses, batch->state) ||
       !batch_ptr_add_usage(batch, &batch->state->programs, pg))
      return;
   pipe_reference(NULL, &pg->reference);
   zink_batch_usage_set(&pg->batch_uses, batch->state);
   batch->has_work = true;
}

/* a fast (hopefully) way to check whether a given batch has completed */
bool
zink_screen_usage_check_completion(struct zink_screen *screen, const struct zink_batch_usage *u)
{
   if (!zink_batch_usage_exists(u))
      return true;
   if (zink_batch_usage_is_unflushed(u))
      return false;

   //return zink_screen_timeline_wait(screen, u->usage, 0);
   return zink_screen_batch_id_wait(screen, u->usage, 0);
}

bool
zink_batch_usage_check_completion(struct zink_context *ctx, const struct zink_batch_usage *u)
{
   if (!zink_batch_usage_exists(u))
      return true;
   if (zink_batch_usage_is_unflushed(u))
      return false;

   //return zink_check_batch_completion(ctx, u->usage);
   return zink_check_batch_completion(ctx, u->usage, false);
}

static void
batch_usage_wait(struct zink_context *ctx, struct zink_batch_usage *u, bool trywait)
{
   if (!zink_batch_usage_exists(u))
      return;
   if (zink_batch_usage_is_unflushed(u)) {
      if (likely(u == &ctx->batch.state->usage))
         ctx->base.flush(&ctx->base, NULL, PIPE_FLUSH_HINT_FINISH);
      else { //multi-context
         mtx_lock(&u->mtx);
         if (trywait) {
            struct timespec ts = {0, 10000};
            cnd_timedwait(&u->flush, &u->mtx, &ts);
         } else
            cnd_wait(&u->flush, &u->mtx);
         mtx_unlock(&u->mtx);
      }
   }
   zink_wait_on_batch(ctx, u->usage);
}

void
zink_batch_usage_wait(struct zink_context *ctx, struct zink_batch_usage *u)
{
   batch_usage_wait(ctx, u, false);
}

void
zink_batch_usage_try_wait(struct zink_context *ctx, struct zink_batch_usage *u)
{
   batch_usage_wait(ctx, u, true);
}
