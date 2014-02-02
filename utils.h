#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdint.h>

void ut_mac2s(uint8_t * mac, char *s);
int ut_s2mac(uint8_t * mac, char *s);
int ut_s2n10(char *s, uint16_t *n);
int ut_s2nl10(char *s, uint32_t *n);
int ut_s2n16(char *s, uint16_t *n);
int ut_s2nl16(char *s, uint32_t *n);
int ut_changecase(char *s, char up);
int ut_hexdump(FILE *f, void *buf, size_t size);
int ut_ip2s(uint8_t *d, char *s);

#endif
