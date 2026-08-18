/*
 * Qira OS - 16550 UART driver
 */
#ifndef QIRA_SERIAL_H
#define QIRA_SERIAL_H

#include <kernel/types.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

void serial_init(void);
bool_t serial_available(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_write_len(const char *s, size_t len);
int  serial_getc_nonblock(void);

#endif /* QIRA_SERIAL_H */
