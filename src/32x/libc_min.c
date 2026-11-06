/* libc_min.c - the few C library calls the SH-2 code (and GCC) uses; the
 * toolchain here is built without newlib. Word-at-a-time where aligned. */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {
        while (n >= 16) {
            uint32_t *dw = (uint32_t *)(void *)d;
            const uint32_t *sw = (const uint32_t *)(const void *)s;
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            d += 16; s += 16; n -= 16;
        }
        while (n >= 4) {
            *(uint32_t *)(void *)d = *(const uint32_t *)(const void *)s;
            d += 4; s += 4; n -= 4;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint32_t w = (uint8_t)c * 0x01010101u;
    while (n && ((uintptr_t)d & 3)) { *d++ = (uint8_t)c; n--; }
    while (n >= 4) { *(uint32_t *)(void *)d = w; d += 4; n -= 4; }
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src++)) dst++;
    return dst;
}

char *strcpy(char *dst, const char *src)
{
    stpcpy(dst, src);
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    stpcpy(dst + strlen(dst), src);
    return dst;
}
