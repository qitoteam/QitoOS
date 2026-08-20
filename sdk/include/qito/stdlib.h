/* QitoOS SDK - minimal stdlib */
#ifndef QITO_SDK_STDLIB_H
#define QITO_SDK_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);

long time_ms(void);
void sleep_ms(unsigned long ms);

#ifdef __cplusplus
}
#endif

#endif
