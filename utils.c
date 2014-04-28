#include "utils.h"
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

void ut_mac2s(uint8_t * mac, char *s)
{
  int i;
  for (i = 0; i < 5; ++i)
    sprintf(&s[3*i], "%02X:", (uint16_t)mac[i]);
  sprintf(&s[15], "%02X", (uint16_t)mac[5]);
}

int ut_s2n16(const char *s, uint16_t *n)
{
  uint8_t i=0;
  while (s[i])
    if (!isxdigit(s[i++]))
      return 1;

  (*n) = strtoul(s, 0, 16);
  return 0;
}

int ut_s2nl16(const char *s, uint32_t *n)
{
  uint8_t i=0;
  while (s[i])
    if (!isxdigit(s[i++]))
      return 1;

  (*n) = strtoul(s, 0, 16);
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

int ut_s2n10(const char *s, uint16_t *n)
{
  uint8_t i=0;
  while (s[i])
    if (!isdigit(s[i++]))
      return 1;

  (*n) = strtoul(s, 0, 10);
  return 0;
}

int ut_s2nl10(const char *s, uint32_t *n)
{
  uint8_t i=0;
  while (s[i])
    if (!isdigit(s[i++]))
      return 1;

  (*n) = strtoul(s, 0, 10);
  return 0;
}

int ut_s2nll10(const char *s, uint64_t *n)
{
  uint8_t i=0;
  while (s[i])
    if (!isdigit(s[i++]))
      return 1;

  (*n) = strtoull(s, 0, 10);
  return 0;
}


int ut_changecase(char *s, char up)
{
  int i = 0;
  while (s[i]) {
    s[i]=(up)?toupper(s[i]):tolower(s[i]);
    i++;
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
