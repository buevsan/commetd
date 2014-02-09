#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <pthread.h>

typedef struct
{
  pthread_mutex_t mtx;
  int   bufsize;
  char *buf;
  FILE *file;
  int   level;
} dbg_desc_t;

int debug_init(dbg_desc_t *, int level);
void debug_free(dbg_desc_t *);
void debug_print(dbg_desc_t *, int mlevel, const char *format, ...);

#endif
