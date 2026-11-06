/* string.h - the few calls we have (src/32x/libc_min.c, src/md/md_libc.c):
 * the toolchain is built without a C library. */
#ifndef MIDI32X_STRING_H
#define MIDI32X_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
char *strcpy(char *dst, const char *src);
char *stpcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strncat(char *dst, const char *src, size_t n);
#endif
