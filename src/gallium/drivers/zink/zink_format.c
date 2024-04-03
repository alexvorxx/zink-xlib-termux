#include "util/format/u_format.h"
#include "zink_format.h"
#include "util/u_math.h"

static const VkFormat formats[PIPE_FORMAT_COUNT] = {
#define MAP_FORMAT_NORM(FMT) \
   [PIPE_FORMAT_ ## FMT ## _UNORM] = VK_FORMAT_ ## FMT ## _UNORM, \
   [PIPE_FORMAT_ ## FMT ## _SNORM] = VK_FORMAT_ ## FMT ## _SNORM,

#define MAP_FORMAT_SCALED(FMT) \
   [PIPE_FORMAT_ ## FMT ## _USCALED] = VK_FORMAT_ ## FMT ## _USCALED, \
   [PIPE_FORMAT_ ## FMT ## _SSCALED] = VK_FORMAT_ ## FMT ## _SSCALED,

#define MAP_FORMAT_INT(FMT) \
   [PIPE_FORMAT_ ## FMT ## _UINT] = VK_FORMAT_ ## FMT ## _UINT, \
   [PIPE_FORMAT_ ## FMT ## _SINT] = VK_FORMAT_ ## FMT ## _SINT,

#define MAP_FORMAT_SRGB(FMT) \
   [PIPE_FORMAT_ ## FMT ## _SRGB] = VK_FORMAT_ ## FMT ## _SRGB,

#define MAP_FORMAT_FLOAT(FMT) \
   [PIPE_FORMAT_ ## FMT ## _FLOAT] = VK_FORMAT_ ## FMT ## _SFLOAT,

   // one component

   // 8-bits
   MAP_FORMAT_NORM(R8)
   MAP_FORMAT_SCALED(R8)
   MAP_FORMAT_INT(R8)
   MAP_FORMAT_SRGB(R8)
   // 16-bits
   MAP_FORMAT_NORM(R16)
   MAP_FORMAT_SCALED(R16)
   MAP_FORMAT_INT(R16)
   MAP_FORMAT_FLOAT(R16)
   // 32-bits
   MAP_FORMAT_INT(R32)
   MAP_FORMAT_FLOAT(R32)

   // two components

   // 8-bits
   MAP_FORMAT_NORM(R8G8)
   MAP_FORMAT_SCALED(R8G8)
   MAP_FORMAT_INT(R8G8)
   MAP_FORMAT_SRGB(R8G8)
   // 16-bits
   MAP_FORMAT_NORM(R16G16)
   MAP_FORMAT_SCALED(R16G16)
   MAP_FORMAT_INT(R16G16)
   MAP_FORMAT_FLOAT(R16G16)
   // 32-bits
   MAP_FORMAT_INT(R32G32)
   MAP_FORMAT_FLOAT(R32G32)

   // three components

   // 8-bits
   MAP_FORMAT_NORM(R8G8B8)
   MAP_FORMAT_SCALED(R8G8B8)
   MAP_FORMAT_INT(R8G8B8)
   MAP_FORMAT_SRGB(R8G8B8)
   MAP_FORMAT_NORM(B8G8R8)
   MAP_FORMAT_SCALED(B8G8R8)
   MAP_FORMAT_INT(B8G8R8)
   MAP_FORMAT_SRGB(B8G8R8)
   // 16-bits
   MAP_FORMAT_NORM(R16G16B16)
   MAP_FORMAT_SCALED(R16G16B16)
   MAP_FORMAT_INT(R16G16B16)
   MAP_FORMAT_FLOAT(R16G16B16)
   // 32-bits
   MAP_FORMAT_INT(R32G32B32)
   MAP_FORMAT_FLOAT(R32G32B32)

   // four components

   // 8-bits
   MAP_FORMAT_NORM(R8G8B8A8)
   MAP_FORMAT_SCALED(R8G8B8A8)
   MAP_FORMAT_INT(R8G8B8A8)
   MAP_FORMAT_NORM(B8G8R8A8)
   MAP_FORMAT_SCALED(B8G8R8A8)
   MAP_FORMAT_INT(B8G8R8A8)
   MAP_FORMAT_SRGB(B8G8R8A8)
   [PIPE_FORMAT_RGBA8888_SRGB] = VK_FORMAT_A8B8G8R8_SRGB_PACK32,
   // 16-bits
   MAP_FORMAT_NORM(R16G16B16A16)
   MAP_FORMAT_SCALED(R16G16B16A16)
   MAP_FORMAT_INT(R16G16B16A16)
   MAP_FORMAT_FLOAT(R16G16B16A16)
   // 32-bits
   MAP_FORMAT_INT(R32G32B32A32)
   MAP_FORMAT_FLOAT(R32G32B32A32)

   // other color formats
   [PIPE_FORMAT_A4B4G4R4_UNORM] = VK_FORMAT_R4G4B4A4_UNORM_PACK16,
   [PIPE_FORMAT_A4R4G4B4_UNORM] = VK_FORMAT_B4G4R4A4_UNORM_PACK16,
   [PIPE_FORMAT_B4G4R4A4_UNORM] = VK_FORMAT_A4R4G4B4_UNORM_PACK16,
   [PIPE_FORMAT_R4G4B4A4_UNORM] = VK_FORMAT_A4B4G4R4_UNORM_PACK16,
   [PIPE_FORMAT_B5G6R5_UNORM] = VK_FORMAT_R5G6B5_UNORM_PACK16,
   [PIPE_FORMAT_R5G6B5_UNORM] = VK_FORMAT_B5G6R5_UNORM_PACK16,

   [PIPE_FORMAT_A1B5G5R5_UNORM] = VK_FORMAT_R5G5B5A1_UNORM_PACK16,
   [PIPE_FORMAT_A1R5G5B5_UNORM] = VK_FORMAT_B5G5R5A1_UNORM_PACK16,
   [PIPE_FORMAT_B5G5R5A1_UNORM] = VK_FORMAT_A1R5G5B5_UNORM_PACK16,

   [PIPE_FORMAT_R11G11B10_FLOAT] = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
   [PIPE_FORMAT_R9G9B9E5_FLOAT] = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
   /* ARB_vertex_type_2_10_10_10 */
   [PIPE_FORMAT_R10G10B10A2_UNORM] = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
   [PIPE_FORMAT_R10G10B10A2_SNORM] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
   [PIPE_FORMAT_B10G10R10A2_UNORM] = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
   [PIPE_FORMAT_B10G10R10A2_SNORM] = VK_FORMAT_A2R10G10B10_SNORM_PACK32,
   [PIPE_FORMAT_R10G10B10A2_USCALED] = VK_FORMAT_A2B10G10R10_USCALED_PACK32,
   [PIPE_FORMAT_R10G10B10A2_SSCALED] = VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
   [PIPE_FORMAT_B10G10R10A2_USCALED] = VK_FORMAT_A2R10G10B10_USCALED_PACK32,
   [PIPE_FORMAT_B10G10R10A2_SSCALED] = VK_FORMAT_A2R10G10B10_SSCALED_PACK32,
   [PIPE_FORMAT_R10G10B10A2_UINT] = VK_FORMAT_A2B10G10R10_UINT_PACK32,
   [PIPE_FORMAT_B10G10R10A2_UINT] = VK_FORMAT_A2R10G10B10_UINT_PACK32,
   [PIPE_FORMAT_B10G10R10A2_SINT] = VK_FORMAT_A2R10G10B10_SINT_PACK32,

   // depth/stencil formats
   [PIPE_FORMAT_Z32_FLOAT] = VK_FORMAT_D32_SFLOAT,
   [PIPE_FORMAT_Z32_FLOAT_S8X24_UINT] = VK_FORMAT_D32_SFLOAT_S8_UINT,
   [PIPE_FORMAT_Z16_UNORM] = VK_FORMAT_D16_UNORM,
   [PIPE_FORMAT_Z16_UNORM_S8_UINT] = VK_FORMAT_D16_UNORM_S8_UINT,
   [PIPE_FORMAT_Z24X8_UNORM] = VK_FORMAT_X8_D24_UNORM_PACK32,
   [PIPE_FORMAT_Z24_UNORM_S8_UINT] = VK_FORMAT_D24_UNORM_S8_UINT,
   [PIPE_FORMAT_S8_UINT] = VK_FORMAT_S8_UINT,

   // compressed formats
   [PIPE_FORMAT_DXT1_RGB] = VK_FORMAT_BC1_RGB_UNORM_BLOCK,
   [PIPE_FORMAT_DXT1_RGBA] = VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
   [PIPE_FORMAT_DXT3_RGBA] = VK_FORMAT_BC2_UNORM_BLOCK,
   [PIPE_FORMAT_DXT5_RGBA] = VK_FORMAT_BC3_UNORM_BLOCK,
   [PIPE_FORMAT_DXT1_SRGB] = VK_FORMAT_BC1_RGB_SRGB_BLOCK,
   [PIPE_FORMAT_DXT1_SRGBA] = VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
   [PIPE_FORMAT_DXT3_SRGBA] = VK_FORMAT_BC2_SRGB_BLOCK,
   [PIPE_FORMAT_DXT5_SRGBA] = VK_FORMAT_BC3_SRGB_BLOCK,

   [PIPE_FORMAT_RGTC1_UNORM] = VK_FORMAT_BC4_UNORM_BLOCK,
   [PIPE_FORMAT_RGTC1_SNORM] = VK_FORMAT_BC4_SNORM_BLOCK,
   [PIPE_FORMAT_RGTC2_UNORM] = VK_FORMAT_BC5_UNORM_BLOCK,
   [PIPE_FORMAT_RGTC2_SNORM] = VK_FORMAT_BC5_SNORM_BLOCK,
   [PIPE_FORMAT_BPTC_RGBA_UNORM] = VK_FORMAT_BC7_UNORM_BLOCK,
   [PIPE_FORMAT_BPTC_SRGBA] = VK_FORMAT_BC7_SRGB_BLOCK,
   [PIPE_FORMAT_BPTC_RGB_FLOAT] = VK_FORMAT_BC6H_SFLOAT_BLOCK,
   [PIPE_FORMAT_BPTC_RGB_UFLOAT] = VK_FORMAT_BC6H_UFLOAT_BLOCK,

   [PIPE_FORMAT_ETC1_RGB8] = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
   [PIPE_FORMAT_ETC2_RGB8] = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
   [PIPE_FORMAT_ETC2_SRGB8] = VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
   [PIPE_FORMAT_ETC2_RGB8A1] = VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
   [PIPE_FORMAT_ETC2_SRGB8A1] = VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
   [PIPE_FORMAT_ETC2_RGBA8] = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
   [PIPE_FORMAT_ETC2_SRGBA8] = VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
   [PIPE_FORMAT_ETC2_R11_UNORM] = VK_FORMAT_EAC_R11_UNORM_BLOCK,
   [PIPE_FORMAT_ETC2_R11_SNORM] = VK_FORMAT_EAC_R11_SNORM_BLOCK,
   [PIPE_FORMAT_ETC2_RG11_UNORM] = VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
   [PIPE_FORMAT_ETC2_RG11_SNORM] = VK_FORMAT_EAC_R11G11_SNORM_BLOCK,

   [PIPE_FORMAT_ASTC_4x4] = VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_4x4_SRGB] = VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_5x4] = VK_FORMAT_ASTC_5x4_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_5x4_SRGB] = VK_FORMAT_ASTC_5x4_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_5x5] = VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_5x5_SRGB] = VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_6x5] = VK_FORMAT_ASTC_6x5_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_6x5_SRGB] = VK_FORMAT_ASTC_6x5_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_6x6] = VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_6x6_SRGB] = VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_8x5] = VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_8x5_SRGB] = VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_8x6] = VK_FORMAT_ASTC_8x6_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_8x6_SRGB] = VK_FORMAT_ASTC_8x6_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_8x8] = VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_8x8_SRGB] = VK_FORMAT_ASTC_8x8_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_10x5] = VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_10x5_SRGB] = VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_10x6] = VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_10x6_SRGB] = VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_10x8] = VK_FORMAT_ASTC_10x8_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_10x8_SRGB] = VK_FORMAT_ASTC_10x8_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_10x10] = VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_10x10_SRGB] = VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_12x10] = VK_FORMAT_ASTC_12x10_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_12x10_SRGB] = VK_FORMAT_ASTC_12x10_SRGB_BLOCK,
   [PIPE_FORMAT_ASTC_12x12] = VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
   [PIPE_FORMAT_ASTC_12x12_SRGB] = VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
};

enum pipe_format
zink_decompose_vertex_format(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   unsigned first_non_void = util_format_get_first_non_void_channel(format);
   enum pipe_format new_format;
   assert(first_non_void == 0);
   if (!desc->is_array)
      return PIPE_FORMAT_NONE;
   if (desc->is_unorm) {
      enum pipe_format unorm_formats[] = {
         PIPE_FORMAT_R8_UNORM,
         PIPE_FORMAT_R16_UNORM,
         PIPE_FORMAT_R32_UNORM
      };
      return unorm_formats[desc->channel[first_non_void].size >> 4];
   } else if (desc->is_snorm) {
      enum pipe_format snorm_formats[] = {
         PIPE_FORMAT_R8_SNORM,
         PIPE_FORMAT_R16_SNORM,
         PIPE_FORMAT_R32_SNORM
      };
      return snorm_formats[desc->channel[first_non_void].size >> 4];
   } else {
      enum pipe_format uint_formats[][3] = {
         {PIPE_FORMAT_R8_USCALED, PIPE_FORMAT_R16_USCALED, PIPE_FORMAT_R32_USCALED},
         {PIPE_FORMAT_R8_UINT, PIPE_FORMAT_R16_UINT, PIPE_FORMAT_R32_UINT},
      };
      enum pipe_format sint_formats[][3] = {
         {PIPE_FORMAT_R8_SSCALED, PIPE_FORMAT_R16_SSCALED, PIPE_FORMAT_R32_SSCALED},
         {PIPE_FORMAT_R8_SINT, PIPE_FORMAT_R16_SINT, PIPE_FORMAT_R32_SINT},
      };
      switch (desc->channel[first_non_void].type) {
      case UTIL_FORMAT_TYPE_UNSIGNED:
         return uint_formats[desc->channel[first_non_void].pure_integer][desc->channel[first_non_void].size >> 4];
      case UTIL_FORMAT_TYPE_SIGNED:
         return sint_formats[desc->channel[first_non_void].pure_integer][desc->channel[first_non_void].size >> 4];
      case UTIL_FORMAT_TYPE_FLOAT:
         return desc->channel[first_non_void].size == 16 ? PIPE_FORMAT_R16_FLOAT : PIPE_FORMAT_R32_FLOAT;
         break;
      default:
         return PIPE_FORMAT_NONE;
      }
   }
   return new_format;
}

VkFormat
zink_pipe_format_to_vk_format(enum pipe_format format)
{
   return formats[format];
}

bool
zink_format_is_red_alpha(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R4A4_UNORM:
   case PIPE_FORMAT_R8A8_SINT:
   case PIPE_FORMAT_R8A8_SNORM:
   case PIPE_FORMAT_R8A8_UINT:
   case PIPE_FORMAT_R8A8_UNORM:
   case PIPE_FORMAT_R16A16_SINT:
   case PIPE_FORMAT_R16A16_SNORM:
   case PIPE_FORMAT_R16A16_UINT:
   case PIPE_FORMAT_R16A16_UNORM:
   case PIPE_FORMAT_R16A16_FLOAT:
   case PIPE_FORMAT_R32A32_SINT:
   case PIPE_FORMAT_R32A32_UINT:
   case PIPE_FORMAT_R32A32_FLOAT:
      return true;
   default: break;
   }
   return false;
}

bool
zink_format_is_emulated_alpha(enum pipe_format format)
{
   return util_format_is_alpha(format) ||
          util_format_is_luminance(format) ||
          util_format_is_luminance_alpha(format) ||
          zink_format_is_red_alpha(format);
}

static enum pipe_format
emulate_alpha(enum pipe_format format)
{
   if (format == PIPE_FORMAT_A8_UNORM)
      return PIPE_FORMAT_R8_UNORM;
   if (format == PIPE_FORMAT_A8_UINT)
      return PIPE_FORMAT_R8_UINT;
   if (format == PIPE_FORMAT_A8_SNORM)
      return PIPE_FORMAT_R8_SNORM;
   if (format == PIPE_FORMAT_A8_SINT)
      return PIPE_FORMAT_R8_SINT;
   if (format == PIPE_FORMAT_A16_UNORM)
      return PIPE_FORMAT_R16_UNORM;
   if (format == PIPE_FORMAT_A16_UINT)
      return PIPE_FORMAT_R16_UINT;
   if (format == PIPE_FORMAT_A16_SNORM)
      return PIPE_FORMAT_R16_SNORM;
   if (format == PIPE_FORMAT_A16_SINT)
      return PIPE_FORMAT_R16_SINT;
   if (format == PIPE_FORMAT_A16_FLOAT)
      return PIPE_FORMAT_R16_FLOAT;
   if (format == PIPE_FORMAT_A32_UINT)
      return PIPE_FORMAT_R32_UINT;
   if (format == PIPE_FORMAT_A32_SINT)
      return PIPE_FORMAT_R32_SINT;
   if (format == PIPE_FORMAT_A32_FLOAT)
      return PIPE_FORMAT_R32_FLOAT;
   return format;
}

static enum pipe_format
emulate_red_alpha(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R8A8_SINT:
      return PIPE_FORMAT_R8G8_SINT;
   case PIPE_FORMAT_R8A8_SNORM:
      return PIPE_FORMAT_R8G8_SNORM;
   case PIPE_FORMAT_R8A8_UINT:
      return PIPE_FORMAT_R8G8_UINT;
   case PIPE_FORMAT_R8A8_UNORM:
      return PIPE_FORMAT_R8G8_UNORM;
   case PIPE_FORMAT_R16A16_SINT:
      return PIPE_FORMAT_R16G16_SINT;
   case PIPE_FORMAT_R16A16_SNORM:
      return PIPE_FORMAT_R16G16_SNORM;
   case PIPE_FORMAT_R16A16_UINT:
      return PIPE_FORMAT_R16G16_UINT;
   case PIPE_FORMAT_R16A16_UNORM:
      return PIPE_FORMAT_R16G16_UNORM;
   case PIPE_FORMAT_R16A16_FLOAT:
      return PIPE_FORMAT_R16G16_FLOAT;
   case PIPE_FORMAT_R32A32_SINT:
      return PIPE_FORMAT_R32G32_SINT;
   case PIPE_FORMAT_R32A32_UINT:
      return PIPE_FORMAT_R32G32_UINT;
   case PIPE_FORMAT_R32A32_FLOAT:
      return PIPE_FORMAT_R32G32_FLOAT;
   default: break;
   }
   return format;
}

enum pipe_format
zink_format_get_emulated_alpha(enum pipe_format format)
{
   if (util_format_is_alpha(format))
      return emulate_alpha(format);
   if (util_format_is_luminance(format))
      return util_format_luminance_to_red(format);
   if (util_format_is_luminance_alpha(format)) {
      if (format == PIPE_FORMAT_LATC2_UNORM)
         return PIPE_FORMAT_RGTC2_UNORM;
      if (format == PIPE_FORMAT_LATC2_SNORM)
         return PIPE_FORMAT_RGTC2_SNORM;

      format = util_format_luminance_to_red(format);
   }

   return emulate_red_alpha(format);
}

bool
zink_format_is_voidable_rgba_variant(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   unsigned chan;

   if(desc->block.width != 1 ||
      desc->block.height != 1 ||
      (desc->block.bits != 32 && desc->block.bits != 64 &&
       desc->block.bits != 128))
      return false;

   if (desc->nr_channels != 4)
      return false;

   unsigned size = desc->channel[0].size;
   for(chan = 0; chan < 4; ++chan) {
      if(desc->channel[chan].size != size)
         return false;
   }

   return true;
}


void
zink_format_clamp_channel_color(const struct util_format_description *desc, union pipe_color_union *dst, const union pipe_color_union *src, unsigned i)
{
   int non_void = util_format_get_first_non_void_channel(desc->format);
   switch (desc->channel[i].type) {
   case UTIL_FORMAT_TYPE_VOID:
      if (non_void != -1) {
         if (desc->channel[non_void].type == UTIL_FORMAT_TYPE_FLOAT) {
            dst->f[i] = uif(UINT32_MAX);
         } else {
            if (desc->channel[non_void].normalized)
               dst->f[i] = 1.0;
            else if (desc->channel[non_void].type == UTIL_FORMAT_TYPE_SIGNED)
               dst->i[i] = INT32_MAX;
            else
               dst->ui[i] = UINT32_MAX;
         }
      } else {
         dst->ui[i] = src->ui[i];
      }
      break;
   case UTIL_FORMAT_TYPE_SIGNED:
      if (desc->channel[i].normalized)
         dst->i[i] = src->i[i];
      else {
         dst->i[i] = MAX2(src->i[i], -(1<<(desc->channel[i].size - 1)));
         dst->i[i] = MIN2(dst->i[i], (1 << (desc->channel[i].size - 1)) - 1);
      }
      break;
   case UTIL_FORMAT_TYPE_UNSIGNED:
      if (desc->channel[i].normalized)
         dst->ui[i] = src->ui[i];
      else
         dst->ui[i] = MIN2(src->ui[i], BITFIELD_MASK(desc->channel[i].size));
      break;
   case UTIL_FORMAT_TYPE_FIXED:
   case UTIL_FORMAT_TYPE_FLOAT:
      dst->ui[i] = src->ui[i];
      break;
   }
}

void
zink_format_clamp_channel_srgb(const struct util_format_description *desc, union pipe_color_union *dst, const union pipe_color_union *src, unsigned i)
{
   if (desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB)
      return;
   switch (desc->channel[i].type) {
   case UTIL_FORMAT_TYPE_SIGNED:
   case UTIL_FORMAT_TYPE_UNSIGNED:
      dst->f[i] = CLAMP(src->f[i], 0.0, 1.0);
      break;
   default:
      break;
   }
}
