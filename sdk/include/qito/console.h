/* QitoOS SDK - console, Freestanding QTX programs use import patching */
#ifndef QITO_SDK_CONSOLE_H
#define QITO_SDK_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* These are resolved at load time via QTX import table.
 * The loader patches the address at the given virtual address.
 * We define function pointers in .data that will be patched.
 */

__attribute__((section(".data"))) extern void (*console_puts_ptr)(const char *s);
__attribute__((section(".data"))) extern void (*console_write_ptr)(const char *data, unsigned long len);
__attribute__((section(".data"))) extern void (*console_clear_ptr)(void);

/* Macros that call via patched pointer – allows host linker to succeed because pointer is data, not undefined function */
#define console_puts(s) console_puts_ptr((s))
#define console_write(d,l) console_write_ptr((d),(l))
#define console_clear() console_clear_ptr()

/* For programs that want direct import (qcc --import will patch data section) */
void console_puts_impl(const char *s);
void console_write_impl(const char *data, unsigned long len);
void console_clear_impl(void);

int printf(const char *fmt, ...);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
