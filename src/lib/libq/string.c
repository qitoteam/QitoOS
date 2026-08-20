/*
 * QitoOS - freestanding implementations of the C string/memory routines.
 *
 * These are shared by the kernel and by userspace programs, so they must not
 * depend on any kernel service.
 */

#include <kernel/string.h>

void *memset(void *dst, int value, size_t count)
{
    uint8_t *p = (uint8_t *)dst;
    uint8_t  v = (uint8_t)value;

    /* Align to 8 bytes, then fill with 64-bit stores. */
    while (count && ((uintptr_t)p & 7)) {
        *p++ = v;
        count--;
    }

    if (count >= 8) {
        uint64_t pattern = v;
        pattern |= pattern << 8;
        pattern |= pattern << 16;
        pattern |= pattern << 32;

        uint64_t *q = (uint64_t *)p;
        while (count >= 8) {
            *q++ = pattern;
            count -= 8;
        }
        p = (uint8_t *)q;
    }

    while (count--) {
        *p++ = v;
    }
    return dst;
}

void memset32(void *dst, uint32_t value, size_t count)
{
    uint32_t *p = (uint32_t *)dst;

    /* Pair up writes when the destination is 8-byte aligned. */
    if (count >= 2 && ((uintptr_t)p & 7) == 0) {
        uint64_t  pattern = ((uint64_t)value << 32) | value;
        uint64_t *q       = (uint64_t *)p;
        size_t    pairs   = count / 2;

        while (pairs--) {
            *q++ = pattern;
        }
        p     = (uint32_t *)q;
        count &= 1;
    }

    while (count--) {
        *p++ = value;
    }
}

void *memcpy(void *dst, const void *src, size_t count)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        while (count && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            count--;
        }
        uint64_t       *qd = (uint64_t *)d;
        const uint64_t *qs = (const uint64_t *)s;
        while (count >= 64) {
            qd[0] = qs[0];
            qd[1] = qs[1];
            qd[2] = qs[2];
            qd[3] = qs[3];
            qd[4] = qs[4];
            qd[5] = qs[5];
            qd[6] = qs[6];
            qd[7] = qs[7];
            qd += 8;
            qs += 8;
            count -= 64;
        }
        while (count >= 8) {
            *qd++ = *qs++;
            count -= 8;
        }
        d = (uint8_t *)qd;
        s = (const uint8_t *)qs;
    }

    while (count--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || count == 0) {
        return dst;
    }
    if (d < s || d >= s + count) {
        return memcpy(dst, src, count);
    }

    /* Overlapping and moving upward: copy backwards. */
    d += count;
    s += count;
    while (count--) {
        *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    while (count--) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *src, int value, size_t count)
{
    const uint8_t *p = (const uint8_t *)src;
    uint8_t        v = (uint8_t)value;

    while (count--) {
        if (*p == v) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((uint8_t)*a) == tolower((uint8_t)*b)) {
        a++;
        b++;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && tolower((uint8_t)*a) == tolower((uint8_t)*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size) {
        size_t copy = (len >= size) ? size - 1 : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = strnlen(dst, size);
    size_t slen = strlen(src);

    if (dlen == size) {
        return size + slen;
    }
    size_t space = size - dlen - 1;
    size_t copy  = (slen > space) ? space : slen;
    memcpy(dst + dlen, src, copy);
    dst[dlen + copy] = '\0';
    return dlen + slen;
}

char *strcat(char *dst, const char *src)
{
    char *out = dst;
    while (*dst) {
        dst++;
    }
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char *strchr(const char *s, int c)
{
    char target = (char)c;
    for (; *s; s++) {
        if (*s == target) {
            return (char *)s;
        }
    }
    return target ? NULL : (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *last   = NULL;
    char        target = (char)c;

    for (; *s; s++) {
        if (*s == target) {
            last = s;
        }
    }
    if (target == '\0') {
        return (char *)s;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return (char *)haystack;
    }
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *token;

    if (str == NULL) {
        str = *saveptr;
    }
    if (str == NULL) {
        return NULL;
    }

    while (*str && strchr(delim, *str)) {
        str++;
    }
    if (*str == '\0') {
        *saveptr = str;
        return NULL;
    }

    token = str;
    while (*str && !strchr(delim, *str)) {
        str++;
    }
    if (*str) {
        *str++ = '\0';
    }
    *saveptr = str;
    return token;
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isprint(int c)
{
    return c >= 0x20 && c < 0x7F;
}

int tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static int digit_value(int c)
{
    if (isdigit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return -1;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    const char   *p        = s;
    unsigned long value    = 0;
    int           negative = 0;

    while (isspace((uint8_t)*p)) {
        p++;
    }
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        base = 2;
    } else if (base == 0 && p[0] == '0' && p[1]) {
        p += 1;
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    int any = 0;
    for (;;) {
        int d = digit_value((uint8_t)*p);
        if (d < 0 || d >= base) {
            break;
        }
        value = value * (unsigned long)base + (unsigned long)d;
        p++;
        any = 1;
    }

    if (end) {
        *end = (char *)(any ? p : s);
    }
    return negative ? (unsigned long)(-(long)value) : value;
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoul(s, end, base);
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}
