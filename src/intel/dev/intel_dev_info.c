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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <xf86drm.h>

#include "intel_device_info.h"
#include "intel_hwconfig.h"

static int
error(char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);

   return EXIT_FAILURE;
}

static void
print_regions_info(const struct intel_device_info *devinfo)
{
   if (devinfo->mem.sram.mappable.size > 0 ||
       devinfo->mem.sram.unmappable.size > 0) {
      fprintf(stdout, "   sram:\n");
      if (devinfo->mem.use_class_instance) {
         fprintf(stdout, "      class: %d; instance: %d\n",
                 devinfo->mem.sram.mem_class, devinfo->mem.sram.mem_instance);
      }
      fprintf(stdout, "      mappable: %" PRId64 "; ",
              devinfo->mem.sram.mappable.size);
      fprintf(stdout, "free: %" PRId64 "\n",
              devinfo->mem.sram.mappable.free);
      if (devinfo->mem.sram.unmappable.size > 0) {
         fprintf(stdout, "      unmappable: %" PRId64 "; ",
                 devinfo->mem.sram.unmappable.size);
         fprintf(stdout, "free: %" PRId64 "\n",
                 devinfo->mem.sram.unmappable.free);
      }
   }

   if (devinfo->mem.vram.mappable.size > 0 ||
       devinfo->mem.vram.unmappable.size > 0) {
      fprintf(stdout, "   vram:\n");
      if (devinfo->mem.use_class_instance) {
         fprintf(stdout, "      class: %d; instance: %d\n",
                 devinfo->mem.vram.mem_class, devinfo->mem.vram.mem_instance);
      }
      fprintf(stdout, "      mappable: %" PRId64 "; ",
              devinfo->mem.vram.mappable.size);
      fprintf(stdout, "free: %" PRId64 "\n",
              devinfo->mem.vram.mappable.free);
      if (devinfo->mem.vram.unmappable.size > 0) {
         fprintf(stdout, "      unmappable: %" PRId64 "; ",
                 devinfo->mem.vram.unmappable.size);
         fprintf(stdout, "free: %" PRId64 "\n",
                 devinfo->mem.vram.unmappable.free);
      }
   }
}

int
main(int argc, char *argv[])
{
   drmDevicePtr devices[8];
   int max_devices;
   bool print_hwconfig = argc > 1 && strcmp(argv[1], "--hwconfig") == 0;

   max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));
   if (max_devices < 1)
      return error("Not device found");

   for (int i = 0; i < max_devices; i++) {
      struct intel_device_info devinfo;
      const char *path = devices[i]->nodes[DRM_NODE_RENDER];
      int fd = open(path, O_RDWR | O_CLOEXEC);

      if (fd < 0)
         continue;

      if (print_hwconfig) {
         intel_get_and_print_hwconfig_table(fd);
      }

      bool success = intel_get_device_info_from_fd(fd, &devinfo);
      close(fd);

      if (!success)
         continue;

      fprintf(stdout, "devinfo struct size = %zu\n", sizeof(devinfo));

      fprintf(stdout, "%s:\n", path);

      fprintf(stdout, "   name: %s\n", devinfo.name);
      fprintf(stdout, "   gen: %u\n", devinfo.ver);
      fprintf(stdout, "   PCI device id: 0x%x\n", devinfo.pci_device_id);
      fprintf(stdout, "   PCI domain: 0x%x\n", devinfo.pci_domain);
      fprintf(stdout, "   PCI bus: 0x%x\n", devinfo.pci_bus);
      fprintf(stdout, "   PCI dev: 0x%x\n", devinfo.pci_dev);
      fprintf(stdout, "   PCI function: 0x%x\n", devinfo.pci_func);
      fprintf(stdout, "   PCI revision id: 0x%x\n", devinfo.pci_revision_id);
      fprintf(stdout, "   revision: %u\n", devinfo.revision);

      const char *subslice_name = devinfo.ver >= 12 ? "dualsubslice" : "subslice";
      uint32_t n_s = 0, n_ss = 0, n_eus = 0;
      for (unsigned s = 0; s < devinfo.max_slices; s++) {
         n_s += (devinfo.slice_masks & (1u << s)) ? 1 : 0;
         for (unsigned ss = 0; ss < devinfo.max_subslices_per_slice; ss++) {
            fprintf(stdout, "   slice%u.%s%u: ", s, subslice_name, ss);
            if (intel_device_info_subslice_available(&devinfo, s, ss)) {
               n_ss++;
               for (unsigned eu = 0; eu < devinfo.max_eus_per_subslice; eu++) {
                  n_eus += intel_device_info_eu_available(&devinfo, s, ss, eu) ? 1 : 0;
                  fprintf(stdout, "%s", intel_device_info_eu_available(&devinfo, s, ss, eu) ? "1" : "0");
               }
            } else {
               fprintf(stdout, "fused");
            }
            fprintf(stdout, "\n");
         }
      }
      for (uint32_t pp = 0; pp < ARRAY_SIZE(devinfo.ppipe_subslices); pp++) {
         fprintf(stdout, "   pixel pipe %02u: %u\n",
                 pp, devinfo.ppipe_subslices[pp]);
      }

      fprintf(stdout, "   slices: %u\n", n_s);
      fprintf(stdout, "   %s: %u\n", subslice_name, n_ss);
      fprintf(stdout, "   EUs: %u\n", n_eus);
      fprintf(stdout, "   EU threads: %u\n", n_eus * devinfo.num_thread_per_eu);

      fprintf(stdout, "   LLC: %u\n", devinfo.has_llc);
      fprintf(stdout, "   threads per EU: %u\n", devinfo.num_thread_per_eu);
      fprintf(stdout, "   URB size: %u\n", devinfo.urb.size);
      fprintf(stdout, "   L3 banks: %u\n", devinfo.l3_banks);
      fprintf(stdout, "   max VS  threads: %u\n", devinfo.max_vs_threads);
      fprintf(stdout, "   max TCS threads: %u\n", devinfo.max_tcs_threads);
      fprintf(stdout, "   max TES threads: %u\n", devinfo.max_tes_threads);
      fprintf(stdout, "   max GS  threads: %u\n", devinfo.max_gs_threads);
      fprintf(stdout, "   max WM  threads: %u\n", devinfo.max_wm_threads);
      fprintf(stdout, "   max CS  threads: %u\n", devinfo.max_cs_threads);
      fprintf(stdout, "   timestamp frequency: %" PRIu64 " / %.4f ns\n",
              devinfo.timestamp_frequency, 1000000000.0 / devinfo.timestamp_frequency);

      print_regions_info(&devinfo);
   }

   return EXIT_SUCCESS;
}
