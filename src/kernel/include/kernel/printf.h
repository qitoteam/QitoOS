/*
 * QitoOS - formatted output
 */
#ifndef QITO_PRINTF_H
#define QITO_PRINTF_H

#include <kernel/types.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_FMT(3, 4);

/* Format into a caller-provided sink, one character at a time. */
typedef void (*putc_fn)(void *ctx, char c);
int vcbprintf(putc_fn emit, void *ctx, const char *fmt, va_list ap);

#endif /* QITO_PRINTF_H */
