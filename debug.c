#include <stdarg.h>
#include <stdlib.h>
#include "debug.h"

#define DEBUG_MSG_BUFSIZE 256

int debug_init(dbg_desc_t *d, int level, const char *filename)
{
  d->buf = malloc(DEBUG_MSG_BUFSIZE);

  if (!d->buf)
   goto error;

  d->file=0;
  if (filename) {
    d->file = fopen(filename, "at+");
    if (!d->file) {
      fprintf(stderr, "Can't open log file: '%s'!!!\n", filename);
      goto error;
    }
  } else
    d->file = stdout;

  d->level = level;
  d->bufsize = DEBUG_MSG_BUFSIZE;
  pthread_mutex_init(&d->mtx, 0);  

  return 0;

error:
  debug_free(d);
  return -1;
}

void debug_free(dbg_desc_t *d)
{
  if ((d->file) && (d->file!=stdout)) {
    fflush(d->file);
    fclose(d->file);
    d->file=0;
  }

  if (d->buf) {
    free(d->buf);
    d->buf=0;
  }
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
