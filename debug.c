#include <stdarg.h>
#include <stdlib.h>
#include "debug.h"

#define DEBUG_MSG_BUFSIZE 256

int debug_init(dbg_desc_t *d, int level)
{
  d->buf = malloc(DEBUG_MSG_BUFSIZE);
  if (!d->buf)
   return -1;
  d->file = stdout;
  d->level = level;
  d->bufsize = DEBUG_MSG_BUFSIZE;
  pthread_mutex_init(&d->mtx, 0);
  return 0;
}

void debug_free(dbg_desc_t *d)
{
  if ((d->file) && (d->file!=stdout))
    fclose(d->file);
  free(d->buf);
  pthread_mutex_destroy(&d->mtx);
}

void debug_print(dbg_desc_t *d, int mlevel, const char *format, ...)
{
  va_list args;

  if ( !((d) && (d->buf) && (d->file)) )
      return;

  pthread_mutex_lock(&d->mtx);

  if ( d->level >= mlevel) {
    va_start( args, format );
    vsnprintf(d->buf, DEBUG_MSG_BUFSIZE, format, args );
    va_end (args);
    fprintf(d->file, "%s", d->buf);
    fflush(d->file);
  }

  pthread_mutex_unlock(&d->mtx);

}
