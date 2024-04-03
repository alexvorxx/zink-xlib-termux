/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "error_decode_xe_lib.h"

#include <stdlib.h>
#include <string.h>

#include "util/macros.h"

static const char *
read_parameter_helper(const char *line, const char *parameter)
{
   if (!strstr(line, parameter))
      return NULL;

   while (*line != ':')
      line++;
   /* skip ':' and ' ' */
   line += 2;

   return line;
}

/* parse lines like 'batch_addr[0]: 0x0000effeffff5000 */
bool
error_decode_xe_read_u64_hexacimal_parameter(const char *line, const char *parameter, uint64_t *value)
{
   line = read_parameter_helper(line, parameter);
   if (!line)
      return false;

   *value = (uint64_t)strtoull(line, NULL, 0);
   return true;
}

/* parse lines like 'PCI ID: 0x9a49' */
bool
error_decode_xe_read_hexacimal_parameter(const char *line, const char *parameter, int *value)
{
   line = read_parameter_helper(line, parameter);
   if (!line)
      return false;

   *value = (int)strtol(line, NULL, 0);
   return true;
}

/* parse lines like 'rcs0 (physical), logical instance=0' */
bool
error_decode_xe_read_engine_name(const char *line, char *ring_name)
{
   int i;

   if (!strstr(line, " (physical), logical instance="))
      return false;

   i = 0;
   for (i = 0; *line != ' '; i++, line++)
      ring_name[i] = *line;

   ring_name[i] = 0;
   return true;
}

/*
 * when a topic string is parsed it sets new_topic and returns true, otherwise
 * does nothing.
 */
bool
error_decode_xe_decode_topic(const char *line, enum xe_topic *new_topic)
{
   static const char *xe_topic_strings[] = {
      "**** Xe Device Coredump ****",
      "**** GuC CT ****",
      "**** Job ****",
      "**** HW Engines ****",
      "**** VM state ****",
   };
   bool topic_changed = false;

   for (int i = 0; i < ARRAY_SIZE(xe_topic_strings); i++) {
      if (strncmp(xe_topic_strings[i], line, strlen(xe_topic_strings[i])) == 0) {
         topic_changed = true;
         *new_topic = i;
         break;
      }
   }

   return topic_changed;
}
