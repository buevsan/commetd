#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

typedef struct
{
  int   bufsize;
  char *buf;
  FILE *file;
  int   level;
} dbg_desc_t;

int debug_init(dbg_desc_t *, int level);
void debug_free(dbg_desc_t *);
void debug_print(dbg_desc_t *, int mlevel, const char *format, ...);

#endif
