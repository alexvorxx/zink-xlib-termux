/*
 * Copyright © 2022 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "util/u_math.h"

#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "freedreno_pm4.h"

#include "a6xx.xml.h"
#include "common/freedreno_dev_info.h"

#include "util/hash_table.h"
#include "util/os_time.h"
#include "util/ralloc.h"
#include "util/rb_tree.h"
#include "util/set.h"
#include "util/u_vector.h"
#include "buffers.h"
#include "cffdec.h"
#include "disasm.h"
#include "io.h"
#include "rdutil.h"
#include "redump.h"
#include "rnnutil.h"

/* Decompiles a single cmdstream from .rd into compilable C source.
 * Given the address space bounds the generated program creates
 * a new .rd which could be used to override cmdstream with 'replay'.
 * Generated .rd is not replayable on its own and depends on buffers
 * provided by the source .rd.
 *
 * C source could be compiled using rdcompiler-meson.build as an example.
 *
 * The workflow would look like this:
 * 1) Find the cmdstream № you want to edit;
 * 2) Decompile it:
 *   rddecompiler -s %cmd_stream_№% example.rd > generate_rd.c
 * 3) Edit the command stream;
 * 4) Compile it back, see rdcompiler-meson.build for the instructions;
 * 5) Plug the generator into cmdstream replay:
 *   replay --override=%cmd_stream_№% --generator=~/generate_rd
 * 6) Repeat 3-5.
 */

static int handle_file(const char *filename, uint32_t submit_to_decompile);

static const char *levels[] = {
   "\t",
   "\t\t",
   "\t\t\t",
   "\t\t\t\t",
   "\t\t\t\t\t",
   "\t\t\t\t\t\t",
   "\t\t\t\t\t\t\t",
   "\t\t\t\t\t\t\t\t",
   "\t\t\t\t\t\t\t\t\t",
};

static void
printlvl(int lvl, const char *fmt, ...)
{
   assert(lvl < ARRAY_SIZE(levels));

   va_list args;
   va_start(args, fmt);
   fputs(levels[lvl], stdout);
   vprintf(fmt, args);
   va_end(args);
}

static void
print_usage(const char *name)
{
   /* clang-format off */
   fprintf(stderr, "Usage:\n\n"
           "\t%s [OPTSIONS]... FILE...\n\n"
           "Options:\n"
           "\t-s, --submit=№   - № of the submit to decompile\n"
           "\t-h, --help       - show this message\n"
           , name);
   /* clang-format on */
   exit(2);
}

/* clang-format off */
static const struct option opts[] = {
      { "submit",    required_argument, 0, 's' },
      { "help",      no_argument,       0, 'h' },
};
/* clang-format on */

int
main(int argc, char **argv)
{
   int ret = -1;
   int c;
   uint32_t submit_to_decompile = -1;

   while ((c = getopt_long(argc, argv, "s:h", opts, NULL)) != -1) {
      switch (c) {
      case 0:
         /* option that set a flag, nothing to do */
         break;
      case 's':
         submit_to_decompile = strtoul(optarg, NULL, 0);
         break;
      case 'h':
      default:
         print_usage(argv[0]);
      }
   }

   if (submit_to_decompile == -1) {
      fprintf(stderr, "Submit to decompile has to be specified\n");
      print_usage(argv[0]);
   }

   while (optind < argc) {
      ret = handle_file(argv[optind], submit_to_decompile);
      if (ret) {
         fprintf(stderr, "error reading: %s\n", argv[optind]);
         break;
      }
      optind++;
   }

   if (ret)
      print_usage(argv[0]);

   return ret;
}

static struct rnn *rnn;
static struct fd_dev_id dev_id;
static void *mem_ctx;

static struct set decompiled_shaders;

static void
init_rnn(const char *gpuname)
{
   rnn = rnn_new(true);
   rnn_load(rnn, gpuname);
}

const char *
pktname(unsigned opc)
{
   return rnn_enumname(rnn, "adreno_pm4_type3_packets", opc);
}

static uint32_t
decompile_shader(const char *name, uint32_t regbase, uint32_t *dwords, int level)
{
   uint64_t gpuaddr = ((uint64_t)dwords[1] << 32) | dwords[0];
   gpuaddr &= 0xfffffffffffffff0;

   /* Shader's iova is referenced in two places, so we have to remember it. */
   if (_mesa_set_search(&decompiled_shaders, &gpuaddr)) {
      printlvl(level, "emit_shader_iova(&ctx, cs, 0x%" PRIx64 ");\n", gpuaddr);
   } else {
      uint64_t *key = ralloc(mem_ctx, uint64_t);
      *key = gpuaddr;
      _mesa_set_add(&decompiled_shaders, key);

      void *buf = hostptr(gpuaddr);
      assert(buf);

      uint32_t sizedwords = hostlen(gpuaddr) / 4;

      char *stream_data = NULL;
      size_t stream_size = 0;
      FILE *stream = open_memstream(&stream_data, &stream_size);

      try_disasm_a3xx(buf, sizedwords, 0, stream, dev_id.gpu_id);
      fclose(stream);

      printlvl(level, "{\n");
      printlvl(level + 1, "const char *source = R\"(\n");
      printf("%s", stream_data);
      printlvl(level + 1, ")\";\n");
      printlvl(level + 1, "upload_shader(&ctx, 0x%" PRIx64 ", source);\n", gpuaddr);
      printlvl(level + 1, "emit_shader_iova(&ctx, cs, 0x%" PRIx64 ");\n", gpuaddr);
      printlvl(level, "}\n");
      free(stream_data);
   }

   return 2;
}

static struct {
   uint32_t regbase;
   uint32_t (*fxn)(const char *name, uint32_t regbase, uint32_t *dwords, int level);
} reg_a6xx[] = {
   {REG_A6XX_SP_VS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_HS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_DS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_GS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_FS_OBJ_START, decompile_shader},
   {REG_A6XX_SP_CS_OBJ_START, decompile_shader},

   {0, NULL},
}, *type0_reg;

static uint32_t
decompile_register(uint32_t regbase, uint32_t *dwords, uint16_t cnt, int level)
{
   struct rnndecaddrinfo *info = rnn_reginfo(rnn, regbase);

   for (unsigned idx = 0; type0_reg[idx].regbase; idx++) {
      if (type0_reg[idx].regbase == regbase) {
         return type0_reg[idx].fxn(info->name, regbase, dwords, level);
      }
   }

   const uint32_t dword = *dwords;

   if (info && info->typeinfo) {
      char *decoded = rnndec_decodeval(rnn->vc, info->typeinfo, dword);
      printlvl(level, "/* pkt4: %s = %s */\n", info->name, decoded);

      if (cnt == 0) {
         printlvl(level, "pkt(cs, %u);\n", dword);
      } else {
         char reg_name[32];
         char field_name[32];
         char reg_idx[32];

         /* reginfo doesn't return reg name in a compilable format, for now just
          * parse it into a compilable reg name.
          * TODO: Make RNN optionally return compilable reg name.
          */
         if (sscanf(info->name, "%32[A-Z0-6_][%32[x0-9]].%32s", reg_name,
                    reg_idx, field_name) != 3) {
            printlvl(level, "pkt4(cs, REG_%s_%s, (%u), %u);\n", rnn->variant,
                     info->name, cnt, dword);
         } else {
            printlvl(level, "pkt4(cs, REG_%s_%s_%s(%s), (%u), %u);\n",
                     rnn->variant, reg_name, field_name, reg_idx, cnt, dword);
         }
      }
   } else {
      printlvl(level, "/* unknown pkt4 */\n");
      printlvl(level, "pkt4(cs, %u, (%u), %u);\n", regbase, cnt, dword);
   }

   return 1;
}

static void
decompile_registers(uint32_t regbase, uint32_t *dwords, uint32_t sizedwords,
                    int level)
{
   if (!sizedwords)
      return;
   uint32_t consumed = decompile_register(regbase, dwords, sizedwords, level);
   sizedwords -= consumed;
   while (sizedwords > 0) {
      regbase += consumed;
      dwords += consumed;
      consumed = decompile_register(regbase, dwords, 0, level);
      sizedwords -= consumed;
   }
}

static void
decompile_domain(uint32_t pkt, uint32_t *dwords, uint32_t sizedwords,
                 const char *dom_name, const char *packet_name, int level)
{
   struct rnndomain *dom;
   int i;

   dom = rnn_finddomain(rnn->db, dom_name);

   printlvl(level, "pkt7(cs, %s, %u);\n", packet_name, sizedwords);

   if (pkt == CP_LOAD_STATE6_FRAG || pkt == CP_LOAD_STATE6_GEOM) {
      enum a6xx_state_type state_type =
         (dwords[0] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >>
         CP_LOAD_STATE6_0_STATE_TYPE__SHIFT;
      enum a6xx_state_src state_src =
         (dwords[0] & CP_LOAD_STATE6_0_STATE_SRC__MASK) >>
         CP_LOAD_STATE6_0_STATE_SRC__SHIFT;

      /* TODO: decompile all other state */
      if (state_type == ST6_SHADER && state_src == SS6_INDIRECT) {
         printlvl(level, "pkt(cs, %u);\n", dwords[0]);
         decompile_shader(NULL, 0, dwords + 1, level);
         return;
      }
   }

   for (i = 0; i < sizedwords; i++) {
      struct rnndecaddrinfo *info = NULL;
      if (dom) {
         info = rnndec_decodeaddr(rnn->vc, dom, i, 0);
      }

      char *decoded;
      if (!(info && info->typeinfo)) {
         printlvl(level, "pkt(cs, %u);\n", dwords[i]);
         continue;
      }
      uint64_t value = dwords[i];
      if (info->typeinfo->high >= 32 && i < sizedwords - 1) {
         value |= (uint64_t)dwords[i + 1] << 32;
         i++; /* skip the next dword since we're printing it now */
      }
      decoded = rnndec_decodeval(rnn->vc, info->typeinfo, value);

      printlvl(level, "/* %s */\n", decoded);
      printlvl(level, "pkt(cs, %u);\n", dwords[i]);

      free(decoded);
      free(info->name);
      free(info);
   }
}

static void
decompile_commands(uint32_t *dwords, uint32_t sizedwords, int level)
{
   int dwords_left = sizedwords;
   uint32_t count = 0; /* dword count including packet header */
   uint32_t val;

   if (!dwords) {
      fprintf(stderr, "NULL cmd buffer!\n");
      return;
   }

   while (dwords_left > 0) {
      if (pkt_is_regwrite(dwords[0], &val, &count)) {
         assert(val < 0xffff);
         decompile_registers(val, dwords + 1, count - 1, level);
      } else if (pkt_is_opcode(dwords[0], &val, &count)) {
         if (val == CP_INDIRECT_BUFFER) {
            uint64_t ibaddr;
            uint32_t ibsize;
            ibaddr = dwords[1];
            ibaddr |= ((uint64_t)dwords[2]) << 32;
            ibsize = dwords[3];

            printlvl(level, "{\n");
            printlvl(level + 1, "begin_ib();\n");

            if (!has_dumped(ibaddr, 0x7)) {
               uint32_t *ptr = hostptr(ibaddr);
               decompile_commands(ptr, ibsize, level + 1);
            }

            printlvl(level + 1, "end_ib();\n");
            printlvl(level, "}\n");
         } else if (val == CP_SET_DRAW_STATE) {
            for (int i = 1; i < count; i += 3) {
               uint32_t state_count = dwords[i] & 0xffff;
               if (state_count != 0) {
                  uint32_t unchanged = dwords[i] & (~0xffff);
                  uint64_t ibaddr = dwords[i + 1];
                  ibaddr |= ((uint64_t)dwords[i + 2]) << 32;

                  printlvl(level, "{\n");
                  printlvl(level + 1, "begin_draw_state();\n");

                  uint32_t *ptr = hostptr(ibaddr);
                  decompile_commands(ptr, state_count, level + 1);

                  printlvl(level + 1, "end_draw_state(%u);\n", unchanged);
                  printlvl(level, "}\n");
               } else {
                  decompile_domain(val, dwords + i, 3, "CP_SET_DRAW_STATE",
                                   "CP_SET_DRAW_STATE", level);
               }
            }
         } else {
            const char *packet_name = pktname(val);
            const char *dom_name = packet_name;
            if (packet_name) {
               /* special hack for two packets that decode the same way
                * on a6xx:
                */
               if (!strcmp(packet_name, "CP_LOAD_STATE6_FRAG") ||
                   !strcmp(packet_name, "CP_LOAD_STATE6_GEOM"))
                  dom_name = "CP_LOAD_STATE6";
               decompile_domain(val, dwords + 1, count - 1, dom_name, packet_name,
                                level);
            } else {
               errx(1, "unknown pkt7 %u", val);
            }
         }
      } else {
         errx(1, "unknown packet %u", dwords[0]);
      }

      dwords += count;
      dwords_left -= count;
   }

   if (dwords_left < 0)
      fprintf(stderr, "**** this ain't right!! dwords_left=%d\n", dwords_left);
}

static void
emit_header()
{
   if (!dev_id.gpu_id || !dev_id.chip_id)
      return;

   static bool emitted = false;
   if (emitted)
      return;
   emitted = true;

   printf("#include \"decode/rdcompiler-utils.h\"\n"
          "int main(int argc, char **argv)\n"
          "{\n"
          "\tstruct replay_context ctx;\n"
          "\tstruct fd_dev_id dev_id = {%u, %" PRIu64 "};\n"
          "\treplay_context_init(&ctx, &dev_id, argc, argv);\n"
          "\tstruct cmdstream *cs = ctx.submit_cs;\n\n",
          dev_id.gpu_id, dev_id.chip_id);
}

static inline uint32_t
u64_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(uint64_t));
}

static inline bool
u64_compare(const void *key1, const void *key2)
{
   return memcmp(key1, key2, sizeof(uint64_t)) == 0;
}

static int
handle_file(const char *filename, uint32_t submit_to_decompile)
{
   struct io *io;
   int submit = 0;
   bool needs_reset = false;
   struct rd_parsed_section ps = {0};

   if (!strcmp(filename, "-"))
      io = io_openfd(0);
   else
      io = io_open(filename);

   if (!io) {
      fprintf(stderr, "could not open: %s\n", filename);
      return -1;
   }

   init_rnn("a6xx");
   type0_reg = reg_a6xx;
   mem_ctx = ralloc_context(NULL);
   _mesa_set_init(&decompiled_shaders, mem_ctx, u64_hash, u64_compare);

   struct {
      unsigned int len;
      uint64_t gpuaddr;
   } gpuaddr = {0};

   while (parse_rd_section(io, &ps)) {
      switch (ps.type) {
      case RD_TEST:
      case RD_VERT_SHADER:
      case RD_FRAG_SHADER:
      case RD_CMD:
         /* no-op */
         break;
      case RD_GPUADDR:
         if (needs_reset) {
            reset_buffers();
            needs_reset = false;
         }

         parse_addr(ps.buf, ps.sz, &gpuaddr.len, &gpuaddr.gpuaddr);
         break;
      case RD_BUFFER_CONTENTS:
         add_buffer(gpuaddr.gpuaddr, gpuaddr.len, ps.buf);
         ps.buf = NULL;
         break;
      case RD_CMDSTREAM_ADDR: {
         unsigned int sizedwords;
         uint64_t gpuaddr;
         parse_addr(ps.buf, ps.sz, &sizedwords, &gpuaddr);

         if (submit == submit_to_decompile) {
            decompile_commands(hostptr(gpuaddr), sizedwords, 0);
         }

         submit++;
         break;
      }
      case RD_GPU_ID: {
         dev_id.gpu_id = parse_gpu_id(ps.buf);
         emit_header();
         break;
      }
      case RD_CHIP_ID: {
         dev_id.chip_id = *(uint64_t *)ps.buf;
         emit_header();
         break;
      }
      default:
         break;
      }
   }

   printf("\treplay_context_finish(&ctx);\n}");

   io_close(io);
   fflush(stdout);

   if (ps.ret < 0) {
      fprintf(stderr, "corrupt file\n");
   }
   return 0;
}
