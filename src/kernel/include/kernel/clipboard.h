/*
 * QitoOS - system clipboard
 *
 * A single shared buffer that any application or shell can read and write.
 * Having one owner in the kernel rather than per-application copies is what
 * makes copy and paste work *between* programs, which is the whole point.
 */
#ifndef QITO_CLIPBOARD_H
#define QITO_CLIPBOARD_H

#include <kernel/types.h>

#define CLIPBOARD_MAX 8192

typedef enum {
    CLIP_TEXT = 0,
    CLIP_PATH,
    CLIP_IMAGE,
} clipboard_kind_t;

void clipboard_init(void);

/* Replace the contents. `owner` names the application, for diagnostics. */
int  clipboard_set(const char *text, clipboard_kind_t kind, const char *owner);
int  clipboard_set_len(const void *data, size_t len, clipboard_kind_t kind,
                       const char *owner);

/* Read the contents; returns NULL when the clipboard is empty. */
const char      *clipboard_get(void);
size_t           clipboard_length(void);
clipboard_kind_t clipboard_kind(void);
const char      *clipboard_owner(void);
uint64_t         clipboard_changed_at(void);

void clipboard_clear(void);

/* A short, single-line preview for the user interface. */
void clipboard_preview(char *out, size_t size);

#endif /* QITO_CLIPBOARD_H */
