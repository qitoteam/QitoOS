/*
 * QitoOS - system console
 *
 * The console multiplexes text output between the serial port and an
 * on-screen text surface, and provides a line-buffered keyboard input queue
 * for the shells.
 */
#ifndef QITO_CONSOLE_H
#define QITO_CONSOLE_H

#include <kernel/types.h>
#include <kernel/fb.h>

#define CONSOLE_COLS_MAX 256
#define CONSOLE_ROWS_MAX 128

void console_init(void);

/* Text output. */
void console_write(const char *text, size_t len);
void console_puts(const char *text);
void console_printf(const char *fmt, ...) PRINTF_FMT(1, 2);
void console_putc(char c);
void console_clear(void);

/* Colours used by subsequent writes. */
void console_set_color(color_t fg, color_t bg);
void console_reset_color(void);

/* Input, fed by the keyboard driver. */
void console_push_char(int ch);
int  console_getchar(void);           /* blocking          */
int  console_getchar_nonblock(void);  /* -1 when empty     */
ssize_t console_read(void *buf, size_t len);
bool_t console_has_input(void);

/* Read a full line with editing support. Returns the length. */
int console_read_line(char *buf, size_t size);

/* Render the console surface to the framebuffer. */
void console_render(void);
void console_set_visible(bool_t visible);
bool_t console_visible(void);

/* Geometry. */
int console_columns(void);
int console_rows(void);

/* Redirect console output into a callback, used by the terminal app. */
typedef void (*console_hook_fn)(const char *text, size_t len);
void console_set_hook(console_hook_fn hook);

#endif /* QITO_CONSOLE_H */
