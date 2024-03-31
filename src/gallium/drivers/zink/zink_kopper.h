/*
 * Copyright © 2021 Valve Corporation
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
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#ifndef ZINK_KOPPER_H
#define ZINK_KOPPER_H

#include "kopper_interface.h"

struct kopper_swapchain {
   struct kopper_swapchain *next;
   VkSwapchainKHR swapchain;
   VkImage *images;
   bool *inits;
   unsigned last_present;
   unsigned num_images;
   VkSemaphore *acquires;
   uint32_t last_present_prune;
   struct hash_table *presents;
   VkSwapchainCreateInfoKHR scci;
   unsigned num_acquires;
   unsigned max_acquires;
   unsigned async_presents;
};

enum kopper_type {
   KOPPER_X11,
   KOPPER_WAYLAND,
   KOPPER_WIN32
};

struct kopper_displaytarget
{
   unsigned refcount;
   VkFormat formats[2];
   unsigned width;
   unsigned height;
   unsigned stride;
   void *loader_private;

   VkSurfaceKHR surface;
   uint32_t present_modes; //VkPresentModeKHR bitmask
   struct kopper_swapchain *swapchain;
   struct kopper_swapchain *old_swapchain;

   struct kopper_loader_info info;
   struct util_queue_fence present_fence;

   VkSurfaceCapabilitiesKHR caps;
   VkImageFormatListCreateInfoKHR format_list;
   enum kopper_type type;
   bool is_kill;
   VkPresentModeKHR present_mode;
};

struct zink_context;
struct zink_screen;
struct zink_resource;

static inline bool
zink_kopper_has_srgb(const struct kopper_displaytarget *cdt)
{
   return cdt->formats[1] != VK_FORMAT_UNDEFINED;
}

static inline bool
zink_kopper_last_present_eq(const struct kopper_displaytarget *cdt, uint32_t idx)
{
   return cdt->swapchain->last_present == idx;
}

struct kopper_displaytarget *
zink_kopper_displaytarget_create(struct zink_screen *screen, unsigned tex_usage,
                                 enum pipe_format format, unsigned width,
                                 unsigned height, unsigned alignment,
                                 const void *loader_private, unsigned *stride);
void
zink_kopper_displaytarget_destroy(struct zink_screen *screen, struct kopper_displaytarget *cdt);


bool
zink_kopper_acquire(struct zink_context *ctx, struct zink_resource *res, uint64_t timeout);
VkSemaphore
zink_kopper_acquire_submit(struct zink_screen *screen, struct zink_resource *res);
VkSemaphore
zink_kopper_present(struct zink_screen *screen, struct zink_resource *res); 
void
zink_kopper_present_queue(struct zink_screen *screen, struct zink_resource *res);
bool
zink_kopper_acquire_readback(struct zink_context *ctx, struct zink_resource *res);
bool
zink_kopper_present_readback(struct zink_context *ctx, struct zink_resource *res);
void
zink_kopper_deinit_displaytarget(struct zink_screen *screen, struct kopper_displaytarget *cdt);
bool
zink_kopper_update(struct pipe_screen *pscreen, struct pipe_resource *pres, int *w, int *h);
bool
zink_kopper_is_cpu(const struct pipe_screen *pscreen);
void
zink_kopper_fixup_depth_buffer(struct zink_context *ctx);
bool
zink_kopper_check(struct pipe_resource *pres);
void
zink_kopper_set_swap_interval(struct pipe_screen *pscreen, struct pipe_resource *pres, int interval);
#endif
