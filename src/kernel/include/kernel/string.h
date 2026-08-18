/*
 * Qira OS - freestanding string and memory routines
 */
#ifndef QIRA_STRING_H
#define QIRA_STRING_H

#include <kernel/types.h>

void  *memset(void *dst, int value, size_t count);
void  *memcpy(void *dst, const void *src, size_t count);
void  *memmove(void *dst, const void *src, size_t count);
int    memcmp(const void *a, const void *b, size_t count);
void  *memchr(const void *src, int value, size_t count);

/* 32-bit fill, used heavily by the framebuffer compositor. */
void  memset32(void *dst, uint32_t value, size_t count);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strtok_r(char *str, const char *delim, char **saveptr);

int    isspace(int c);
int    isdigit(int c);
int    isxdigit(int c);
int    isalpha(int c);
int    isalnum(int c);
int    isprint(int c);
int    tolower(int c);
int    toupper(int c);

long      strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int       atoi(const char *s);

#endif /* QIRA_STRING_H */
