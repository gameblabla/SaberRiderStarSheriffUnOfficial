#include "namehash.h"
#include <string.h>
uint32_t namehash(const char *s)
{
    const unsigned char *b = (const unsigned char *)s;
    size_t n = strlen(s);
    if (n == 0) return 0;
    uint32_t A = 0, B = 0;
    for (size_t i = 0; i < n; i++) A += b[i] * (uint32_t)(i + 1);
    if (n == 1) { int c = (signed char)b[0]; return ((uint32_t)(c | (c << 8)) << 16) | A; }
    for (size_t i = 0; i + 1 < n; i++) B += b[i + 1] * (uint32_t)(i + 1);
    uint32_t h = (B << 16) | A;
    for (size_t i = 0; i + 1 < n; i++) h += ~(((uint32_t)(signed char)b[i] << 24) | (uint32_t)(signed char)b[i + 1]);
    int c = (signed char)b[n - 1]; size_t j = n - 1;
    do {
        uint32_t u = (uint32_t)c;
        j--; c = (signed char)b[j];
        u = ((uint32_t)(c >> 8)) | u;
        h ^= (u & 0xff) | ((~u) << 8);
    } while (j != 0);
    return h;
}
