#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>

#include "utils.h"

void ut_mac2s(uint8_t * mac, char *s)
{
  int i;
  for (i = 0; i < 5; ++i)
    sprintf(&s[3*i], "%02X:", (uint16_t)mac[i]);
  sprintf(&s[15], "%02X", (uint16_t)mac[5]);
}

int ut_ishex_str(const char *s)
{
  if ( (!s) || (!s[0]) )
      return 1;

  while (*s) {
    if (!isxdigit(*s))
      return 1;
    s++;
  }
  return 0;
}

int ut_s2ul(const char *s, int base, unsigned long int *r)
{
  char *p;

  errno=0;
  (*r) = strtoul(s, &p, base);

  if (errno)
    return 1;

  if (*p)
    return 1;

  return 0;
}

int ut_s2ull(const char *s, int base, unsigned long long int *r)
{
  char *p;

  errno=0;
  (*r) = strtoull(s, &p, base);

  if (errno)
    return 1;

  if (*p)
    return 1;

  return 0;
}

int ut_s2n16(const char *s, uint16_t *n)
{
  unsigned long int r;

  if (ut_ishex_str(s))
    return 1;

  if (ut_s2ul(s, 16, &r))
    return 1;

  if (r>65535)
    return 1;

  (*n) = r;

  return 0;
}

int ut_s2nl16(const char *s, uint32_t *n)
{
  unsigned long int r;

  if (ut_ishex_str(s))
    return 1;

  if (ut_s2ul(s, 16, &r))
    return 1;

  (*n) = r;

  return 0;
}

int ut_s2mac(uint8_t * mac, char *s)
{
  int len, sidx, eidx, i;
  char *c, ch;
  uint16_t n;

  len = strlen(s);

  if (len < 11)
    return 1;

  sidx = 0;

  for (i = 0; i < 6; ++i) {

    /* find delimeter */

    if (i < 5) {
      c = strchr(&s[sidx], ':');
      if (c)
        eidx = (int)(c - s);
      else
        return 1;
    } else
       eidx = (len-1);

   /* printf("idx: %i\n",sidx); */

   /* read hex number */

   if (i < 5) {
     ch = s[eidx];
     s[eidx]=0;
   }

   if (ut_s2n16(&s[sidx], &n))
     return 1;

   if (i<5)
    s[eidx]=ch;

   if (n>255)
     return 1;

   mac[i] = (uint8_t)n;

   /* next */

   if ((eidx+1) < len)
     sidx = (eidx+1);

 }


 return 0;

}

int ut_isdec_str(const char *s)
{    
  if ( (!s) || (!s[0]) )
    return 1;

  while (*s) {
    if (!isdigit(*s))
      return 1;
    s++;
  }
  return 0;
}

int ut_s2n10(const char *s, uint16_t *n)
{
  unsigned long int r;

  if (ut_isdec_str(s))
    return 1;

  if (ut_s2ul(s, 10, &r))
    return 1;

  if (r>65535)
    return 1;

  (*n) = r;

  return 0;
}

int ut_s2nl10(const char *s, uint32_t *n)
{
  unsigned long int r;

  if (ut_isdec_str(s))
    return 1;

  if (ut_s2ul(s, 10, &r))
    return 1;

  (*n) = r;

  return 0;
}

int ut_s2nll10(const char *s, uint64_t *n)
{
  unsigned long long int r;
  if (ut_isdec_str(s))
    return 1;

  if (ut_s2ull(s, 10, &r))
    return 1;

  (*n) = r;
  return 0;
}

int ut_changecase(char *s, char up)
{
  while (*s) {
    (*s)=(up)?toupper(*s):tolower(*s);
    s++;
  }
  return 0;
}

int ut_hexdump(FILE *f, void *buf, size_t size)
{
  size_t i, j;

  for(i = 0; i < size; i += 16) {

    fprintf(f, "%04X : ", (uint32_t)i);

    for (j = 0; j < 16 && i + j < size; j++)
      fprintf(f, "%2.2X ", ((uint8_t*)buf)[i + j]);

    for (; j < 16; j++)
      fprintf(f, "   ");

    fprintf(f,": ");
    for (j = 0; j < 16 && i + j < size; j++) {
      char c = toascii(((uint8_t*)buf)[i + j]);
      fprintf(f, "%c", isalnum(c) ? c : '.');
    }

    fprintf(f, "\n");
  }

  return 0;
}

int ut_ip2s(uint8_t *d, char *s)
{
  sprintf(s, "%d.%d.%d.%d", (uint16_t)d[0], (uint16_t)d[1], (uint16_t)d[2], (uint16_t)d[3]);
  return 0;
}

void ut_gettime(uint64_t *t, uint32_t s)
{
  struct timeval tv;
  memset(&tv, 0, sizeof(tv));
  gettimeofday(&tv, 0);
  (*t) = tv.tv_sec*s+tv.tv_usec/(1000000/s);
}

void ut_strncpy(char *d, const char *s, size_t len)
{
  if (!len)
    return;
  strncpy(d, s, len);
  d[len-1] = 0;
}
