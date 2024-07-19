/*
 * Copyright 2024 Red Hat, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Compatibility stub for Xorg. This responds to just enough of the legacy DRI
 * interface to allow the X server to initialize GLX and enable direct
 * rendering clients. It implements the screen creation hook and provides a
 * (static, unambitious) list of framebuffer configs. It will not create an
 * indirect context; Indirect contexts have been disabled by default since
 * 2014 and would be limited to GL 1.4 in any case, so this is no great loss.
 *
 * If you do want indirect contexts to work, you have options. This stub is
 * new with Mesa 24.1, so one option is to use an older Mesa release stream.
 * Another option is to use an X server that does not need this interface. For
 * Xwayland and Xephyr that's XX.X or newer, and for Xorg drivers using glamor
 * for acceleration that's YY.Y or newer.
 */

#include "main/glconfig.h"
#include "main/mtypes.h"
#include <GL/internal/dri_interface.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "gbm/main/gbm.h"

#define EGL_PLATFORM_GBM_MESA             0x31D7

/* avoid needing X11 headers */
#define GLX_NONE 0x8000
#define GLX_DONT_CARE 0xFFFFFFFF

#define CONFIG_DB(color, zs, doublebuffer) \
   { \
      .color_format = color, \
      .zs_format = zs, \
      .doubleBufferMode = doublebuffer, \
   }
#define CONFIG(color, zs) \
   CONFIG_DB(color, zs, GL_TRUE), \
   CONFIG_DB(color, zs, GL_FALSE)

static const struct gl_config drilConfigs[] = {
   CONFIG(PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_Z24_UNORM_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R8G8B8X8_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8X8_UNORM, PIPE_FORMAT_Z24_UNORM_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8X8_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R10G10B10A2_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R10G10B10A2_UNORM, PIPE_FORMAT_Z24_UNORM_S8_UINT),
   CONFIG(PIPE_FORMAT_R10G10B10A2_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R10G10B10X2_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R10G10B10X2_UNORM, PIPE_FORMAT_Z24_UNORM_S8_UINT),
   CONFIG(PIPE_FORMAT_R10G10B10X2_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_Z16_UNORM),
   CONFIG(PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_NONE),
   // etc...
};

#define RGB UTIL_FORMAT_COLORSPACE_RGB
#define RED 0
#define GREEN 1
#define BLUE 2
#define ALPHA 3
#define ZS UTIL_FORMAT_COLORSPACE_ZS
#define DEPTH 0
#define STENCIL 1

#define CASE(ATTRIB, VALUE) \
   case __DRI_ATTRIB_ ## ATTRIB : \
      *value = VALUE; \
      break;

#define SIZE(f, cs, chan)  (f ? util_format_get_component_bits(f, cs, chan) : 0)
#define SHIFT(f, cs, chan) (f ? util_format_get_component_shift(f, cs, chan) : 0)
#define MASK(f, cs, chan) \
   (((1 << SIZE(f, cs, chan)) - 1) << SHIFT(f, cs, chan))

static int
drilIndexConfigAttrib(const __DRIconfig *_config, int index,
                      unsigned int *attrib, unsigned int *value)
{
   struct gl_config *config = (void *)_config;
   enum pipe_format color_format = config->color_format;
   enum pipe_format zs_format = config->zs_format;
   enum pipe_format accum_format = config->accum_format;

   if (index >= __DRI_ATTRIB_MAX)
      return 0;

   switch (index) {
      case __DRI_ATTRIB_SAMPLE_BUFFERS:
         *value = !!config->samples;
         break;

      case __DRI_ATTRIB_BUFFER_SIZE: {
         unsigned int red = 0, green = 0, blue = 0, alpha = 0;
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_RED_SIZE, attrib, &red);
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_GREEN_SIZE, attrib, &green);
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_BLUE_SIZE, attrib, &blue);
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_ALPHA_SIZE, attrib, &alpha);
         *value = red + green + blue + alpha;
         break;
      }

      CASE(RED_SIZE,          SIZE(color_format, RGB, 0));
      CASE(GREEN_SIZE,        SIZE(color_format, RGB, 1));
      CASE(BLUE_SIZE,         SIZE(color_format, RGB, 2));
      CASE(ALPHA_SIZE,        SIZE(color_format, RGB, 3));
      CASE(DEPTH_SIZE,        SIZE(zs_format,    ZS,  0));
      CASE(STENCIL_SIZE,      SIZE(zs_format,    ZS,  1));
      CASE(ACCUM_RED_SIZE,    SIZE(accum_format, RGB, 0));
      CASE(ACCUM_GREEN_SIZE,  SIZE(accum_format, RGB, 1));
      CASE(ACCUM_BLUE_SIZE,   SIZE(accum_format, RGB, 2));
      CASE(ACCUM_ALPHA_SIZE,  SIZE(accum_format, RGB, 3));

      CASE(RENDER_TYPE, __DRI_ATTRIB_RGBA_BIT);
      CASE(CONFORMANT, GL_TRUE);
      CASE(DOUBLE_BUFFER, config->doubleBufferMode);
      CASE(SAMPLES, config->samples);

      CASE(TRANSPARENT_TYPE,        GLX_NONE);
      CASE(TRANSPARENT_INDEX_VALUE, GLX_NONE);
      CASE(TRANSPARENT_RED_VALUE,   GLX_DONT_CARE);
      CASE(TRANSPARENT_GREEN_VALUE, GLX_DONT_CARE);
      CASE(TRANSPARENT_BLUE_VALUE,  GLX_DONT_CARE);
      CASE(TRANSPARENT_ALPHA_VALUE, GLX_DONT_CARE);

      CASE(RED_MASK,   MASK(color_format, RGB, 0));
      CASE(GREEN_MASK, MASK(color_format, RGB, 1));
      CASE(BLUE_MASK,  MASK(color_format, RGB, 2));
      CASE(ALPHA_MASK, MASK(color_format, RGB, 3));

      CASE(SWAP_METHOD, __DRI_ATTRIB_SWAP_UNDEFINED);
      CASE(MAX_SWAP_INTERVAL, INT_MAX);
      CASE(BIND_TO_TEXTURE_RGB, GL_TRUE);
      CASE(BIND_TO_TEXTURE_RGBA, GL_TRUE);
      CASE(BIND_TO_TEXTURE_TARGETS,
           __DRI_ATTRIB_TEXTURE_1D_BIT |
           __DRI_ATTRIB_TEXTURE_2D_BIT |
           __DRI_ATTRIB_TEXTURE_RECTANGLE_BIT);
      CASE(YINVERTED, GL_TRUE);

      CASE(RED_SHIFT,   SHIFT(color_format, RGB, 0));
      CASE(GREEN_SHIFT, SHIFT(color_format, RGB, 1));
      CASE(BLUE_SHIFT,  SHIFT(color_format, RGB, 2));
      CASE(ALPHA_SHIFT, SHIFT(color_format, RGB, 3));

      default:
         *value = 0;
         break;
   }

   *attrib = index;
   return 1;
}

static void
drilDestroyScreen(__DRIscreen *screen)
{
   /* At the moment this is just the bounce table for the configs */
   free(screen);
}

static const __DRI2flushControlExtension dri2FlushControlExtension = {
   .base = { __DRI2_FLUSH_CONTROL, 1 }
};

static void
dril_set_tex_buffer2(__DRIcontext *pDRICtx, GLint target,
                    GLint format, __DRIdrawable *dPriv)
{
}

static void
dril_set_tex_buffer(__DRIcontext *pDRICtx, GLint target,
                   __DRIdrawable *dPriv)
{
}

const __DRItexBufferExtension driTexBufferExtension = {
   .base = { __DRI_TEX_BUFFER, 2 },

   .setTexBuffer       = dril_set_tex_buffer,
   .setTexBuffer2      = dril_set_tex_buffer2,
   .releaseTexBuffer   = NULL,
};

static const __DRIrobustnessExtension dri2Robustness = {
   .base = { __DRI2_ROBUSTNESS, 1 }
};

static const __DRIextension *dril_extensions[] = {
   &dri2FlushControlExtension.base,
   &driTexBufferExtension.base,
   &dri2Robustness.base,
   NULL
};

/* This has to return a pointer to NULL, not just NULL */
static const __DRIextension **
drilGetExtensions(__DRIscreen *screen)
{
   return (void*)&dril_extensions;
}

static __DRIcontext *
drilCreateContextAttribs(__DRIscreen *psp, int api,
                        const __DRIconfig *config,
                        __DRIcontext *shared,
                        unsigned num_attribs,
                        const uint32_t *attribs,
                        unsigned *error,
                        void *data)
{
   return NULL;
}

static __DRIcontext *
drilCreateNewContextForAPI(__DRIscreen *screen, int api,
                          const __DRIconfig *config,
                          __DRIcontext *shared, void *data)
{
   return NULL;
}

static __DRIcontext *
drilCreateNewContext(__DRIscreen *screen, const __DRIconfig *config,
                    __DRIcontext *shared, void *data)
{
   return NULL;
}

static void
drilDestroyDrawable(__DRIdrawable *pdp)
{
}

static const __DRIcoreExtension drilCoreExtension = {
   .base = { __DRI_CORE, 1 },

   .destroyScreen       = drilDestroyScreen,
   .getExtensions       = drilGetExtensions,
   .getConfigAttrib     = NULL, // XXX not actually used!
   .indexConfigAttrib   = drilIndexConfigAttrib,
   .destroyDrawable     = drilDestroyDrawable,
   .createNewContext    = drilCreateNewContext,
};

static int drilBindContext(__DRIcontext *pcp,
                          __DRIdrawable *pdp,
                          __DRIdrawable *prp)
{
   return 0; // Success
}

static int drilUnbindContext(__DRIcontext *pcp)
{
   return 0; // Success
}

static __DRIdrawable *
drilCreateNewDrawable(__DRIscreen *psp,
                     const __DRIconfig *config,
                     void *data)
{
   return NULL;
}

#define NUM_SAMPLE_COUNTS 7

/* DRI2 awfulness */
static bool
init_dri2_configs(int fd, const __DRIconfig **configs)
{
   void *egl = NULL;
   bool ret = false;

   /* dlopen/dlsym to avoid linkage */
   egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
   if (!egl)
      return false;

   void * (*peglGetProcAddress)(const char *) = dlsym(egl, "eglGetProcAddress");
   EGLDisplay (*peglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = peglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay (*peglInitialize)(EGLDisplay, int*, int*) = peglGetProcAddress("eglInitialize");
   void (*peglTerminate)(EGLDisplay) = peglGetProcAddress("eglTerminate");
   EGLBoolean (*peglChooseConfig)(EGLDisplay, EGLint const *, EGLConfig *, EGLint, EGLint*) = peglGetProcAddress("eglChooseConfig");

   /* try opening GBM for hardware driver info */
   struct gbm_device *gbm = gbm_create_device(fd);
   if (!gbm)
      goto out;

   EGLDisplay dpy = peglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, gbm, NULL);
   if (!dpy)
      goto out_gbm;

   int maj, min;
   if (!peglInitialize(dpy, &maj, &min))
      goto out_gbm;

   unsigned c = 0;

   /* iterate over base configs and check for multisample support */
   for (unsigned i = 0; i < ARRAY_SIZE(drilConfigs); i++) {
      unsigned num_samples[] = {
         0, 2, 4, 6, 8, 16, 32
      };
      static_assert(ARRAY_SIZE(num_samples) == NUM_SAMPLE_COUNTS, "sample count define needs updating");
      for (unsigned j = 0; j < ARRAY_SIZE(num_samples); j++) {
         const EGLint config_attribs[] = {
            EGL_RED_SIZE,           SIZE(drilConfigs[i].color_format, RGB, 0),
            EGL_GREEN_SIZE,         SIZE(drilConfigs[i].color_format, RGB, 1),
            EGL_BLUE_SIZE,          SIZE(drilConfigs[i].color_format, RGB, 2),
            EGL_ALPHA_SIZE,         SIZE(drilConfigs[i].color_format, RGB, 3),
            EGL_DEPTH_SIZE,         SIZE(drilConfigs[i].zs_format, ZS, 0),
            EGL_STENCIL_SIZE,       SIZE(drilConfigs[i].zs_format, ZS, 1),
            EGL_SAMPLES,            num_samples[j],
            EGL_NONE
         };
         int num_configs = 0;
         if (peglChooseConfig(dpy, config_attribs, NULL, 0, &num_configs) && num_configs) {
            /* only copy supported configs */
            configs[c] = mem_dup(&drilConfigs[i], sizeof(drilConfigs[i]));

            /* hardcoded configs have samples=0, need to update */
            struct gl_config *cfg = (void*)configs[c];
            cfg->samples = num_samples[j];
            ret = true;
            c++;
         }
      }
   }

   /* don't forget cleanup */
   peglTerminate(dpy);

out_gbm:
   gbm_device_destroy(gbm);
out:
   dlclose(egl);
   return ret;
}

static __DRIscreen *
drilCreateNewScreen(int scrn, int fd,
                    const __DRIextension **loader_extensions,
                    const __DRIextension **driver_extensions,
                    const __DRIconfig ***driver_configs, void *data)
{
   /* multiply for possible 1/2/4/8/16/32 MSAA configs */
   // allocate an array of pointers
   const __DRIconfig **configs = calloc(ARRAY_SIZE(drilConfigs) * NUM_SAMPLE_COUNTS + 1, sizeof(void *));
   /* try dri2 if fd is valid */
   if (fd < 0 || !init_dri2_configs(fd, configs)) {
      // otherwise set configs to point to our config list
      for (int i = 0; i < ARRAY_SIZE(drilConfigs); i++) {
         configs[i] = mem_dup(&drilConfigs[i], sizeof(drilConfigs[i]));
      }
   }

   // outpointer it
   *driver_configs = configs;

   // This has to be a separate allocation from the configs.
   // If we had any additional screen state we'd need to do
   // something less hacky.
   return malloc(sizeof(int));
}

const __DRIextension *__driDriverExtensions[];

static __DRIscreen *
dril2CreateNewScreen(int scrn, int fd,
                     const __DRIextension **extensions,
                     const __DRIconfig ***driver_configs, void *data)
{
   return drilCreateNewScreen(scrn, fd,
                              extensions,
                              __driDriverExtensions,
                              driver_configs, data);
}

static __DRIscreen *
drilSWCreateNewScreen(int scrn, const __DRIextension **extensions,
                      const __DRIconfig ***driver_configs,
                      void *data)
{
   return drilCreateNewScreen(scrn, -1,
                              extensions,
                              __driDriverExtensions,
                              driver_configs, data);
}

static __DRIscreen *
drilSWCreateNewScreen2(int scrn, const __DRIextension **extensions,
                       const __DRIextension **driver_extensions,
                       const __DRIconfig ***driver_configs, void *data)
{
   return drilCreateNewScreen(scrn, -1,
                              extensions,
                              __driDriverExtensions,
                              driver_configs, data);
}

static int
drilSWQueryBufferAge(__DRIdrawable *pdp)
{
   return 0;
}


static const __DRIswrastExtension drilSWRastExtension = {
   .base = { __DRI_SWRAST, 5 },

   .createNewScreen = drilSWCreateNewScreen,
   .createNewDrawable = drilCreateNewDrawable,
   .createNewContextForAPI     = drilCreateNewContextForAPI,
   .createContextAttribs       = drilCreateContextAttribs,
   .createNewScreen2           = drilSWCreateNewScreen2,
   .queryBufferAge             = drilSWQueryBufferAge,
};

const __DRIdri2Extension drilDRI2Extension = {
    .base = { __DRI_DRI2, 5 },

    /* these are the methods used by the xserver */
    .createNewScreen            = dril2CreateNewScreen,
    .createNewDrawable          = drilCreateNewDrawable,
    .createNewContext           = drilCreateNewContext,
    .createContextAttribs       = drilCreateContextAttribs,
};

const __DRIextension *__driDriverExtensions[] = {
   &drilCoreExtension.base,
   &drilSWRastExtension.base,
   &drilDRI2Extension.base,
   NULL
};

#include "util/detect_os.h"

#include "target-helpers/drm_helper.h"
#include "target-helpers/sw_helper.h"

#define DEFINE_LOADER_DRM_ENTRYPOINT(drivername)                          \
const __DRIextension **__driDriverGetExtensions_##drivername(void);       \
PUBLIC const __DRIextension **__driDriverGetExtensions_##drivername(void) \
{                                                                         \
   return __driDriverExtensions;                                   \
}

const __DRIextension **__driDriverGetExtensions_swrast(void);

PUBLIC const __DRIextension **__driDriverGetExtensions_swrast(void)
{
   return __driDriverExtensions;
}

const __DRIextension **__driDriverGetExtensions_kms_swrast(void);

PUBLIC const __DRIextension **__driDriverGetExtensions_kms_swrast(void)
{
   return __driDriverExtensions;
}

DEFINE_LOADER_DRM_ENTRYPOINT(i915)
DEFINE_LOADER_DRM_ENTRYPOINT(iris)
DEFINE_LOADER_DRM_ENTRYPOINT(crocus)
DEFINE_LOADER_DRM_ENTRYPOINT(nouveau)
DEFINE_LOADER_DRM_ENTRYPOINT(r300)
DEFINE_LOADER_DRM_ENTRYPOINT(r600)
DEFINE_LOADER_DRM_ENTRYPOINT(radeonsi)
DEFINE_LOADER_DRM_ENTRYPOINT(vmwgfx)
DEFINE_LOADER_DRM_ENTRYPOINT(msm)
DEFINE_LOADER_DRM_ENTRYPOINT(kgsl)
DEFINE_LOADER_DRM_ENTRYPOINT(virtio_gpu)
DEFINE_LOADER_DRM_ENTRYPOINT(v3d)
DEFINE_LOADER_DRM_ENTRYPOINT(vc4)
DEFINE_LOADER_DRM_ENTRYPOINT(panfrost)
DEFINE_LOADER_DRM_ENTRYPOINT(panthor)
DEFINE_LOADER_DRM_ENTRYPOINT(asahi)
DEFINE_LOADER_DRM_ENTRYPOINT(etnaviv)
DEFINE_LOADER_DRM_ENTRYPOINT(tegra)
DEFINE_LOADER_DRM_ENTRYPOINT(armada_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(exynos)
DEFINE_LOADER_DRM_ENTRYPOINT(gm12u320)
DEFINE_LOADER_DRM_ENTRYPOINT(hdlcd)
DEFINE_LOADER_DRM_ENTRYPOINT(hx8357d)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9163)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9225)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9341)
DEFINE_LOADER_DRM_ENTRYPOINT(ili9486)
DEFINE_LOADER_DRM_ENTRYPOINT(imx_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(imx_dcss)
DEFINE_LOADER_DRM_ENTRYPOINT(imx_lcdif)
DEFINE_LOADER_DRM_ENTRYPOINT(ingenic_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(kirin)
DEFINE_LOADER_DRM_ENTRYPOINT(komeda)
DEFINE_LOADER_DRM_ENTRYPOINT(mali_dp)
DEFINE_LOADER_DRM_ENTRYPOINT(mcde)
DEFINE_LOADER_DRM_ENTRYPOINT(mediatek)
DEFINE_LOADER_DRM_ENTRYPOINT(meson)
DEFINE_LOADER_DRM_ENTRYPOINT(mi0283qt)
DEFINE_LOADER_DRM_ENTRYPOINT(mxsfb_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(panel_mipi_dbi)
DEFINE_LOADER_DRM_ENTRYPOINT(pl111)
DEFINE_LOADER_DRM_ENTRYPOINT(rcar_du)
DEFINE_LOADER_DRM_ENTRYPOINT(repaper)
DEFINE_LOADER_DRM_ENTRYPOINT(rockchip)
DEFINE_LOADER_DRM_ENTRYPOINT(rzg2l_du)
DEFINE_LOADER_DRM_ENTRYPOINT(ssd130x)
DEFINE_LOADER_DRM_ENTRYPOINT(st7586)
DEFINE_LOADER_DRM_ENTRYPOINT(st7735r)
DEFINE_LOADER_DRM_ENTRYPOINT(sti)
DEFINE_LOADER_DRM_ENTRYPOINT(stm)
DEFINE_LOADER_DRM_ENTRYPOINT(sun4i_drm)
DEFINE_LOADER_DRM_ENTRYPOINT(udl)
DEFINE_LOADER_DRM_ENTRYPOINT(zynqmp_dpsub)
DEFINE_LOADER_DRM_ENTRYPOINT(lima)
DEFINE_LOADER_DRM_ENTRYPOINT(d3d12)
