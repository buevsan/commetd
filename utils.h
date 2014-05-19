#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdint.h>

void ut_mac2s(uint8_t * mac, char *s);
int ut_s2mac(uint8_t * mac, char *s);
int ut_s2n10(const char *s, uint16_t *n);
int ut_s2nl10(const char *s, uint32_t *n);
int ut_s2nll10(const char *s, uint64_t *n);
int ut_s2n16(const char *s, uint16_t *n);
int ut_s2nl16(const char *s, uint32_t *n);
int ut_changecase(char *s, char up);
int ut_hexdump(FILE *f, void *buf, size_t size);
int ut_ip2s(uint8_t *d, char *s);
void ut_gettime(uint64_t *t, uint32_t s);

#define ut_gettime_s(t) ut_gettime(t, 1)
#define ut_gettime_ms(t) ut_gettime(t, 1000)
#define ut_gettime_us(t) ut_gettime(t, 1000000)

#endif
