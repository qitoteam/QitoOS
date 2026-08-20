/*
 * QitoOS - UTF-8 and Unicode support
 * Fonts are ASCII-only (U+0020-U+007F) in fixed 8x16 cell.
 * Add UTF-8 decoding, glyph cache, Latin-1/Greek/Cyrillic coverage plus box-drawing.
 */

#ifndef QITO_UNICODE_H
#define QITO_UNICODE_H

#include <kernel/types.h>

/* UTF-8 decoding */
int utf8_decode(const char *s, uint32_t *out_codepoint);
int utf8_encode(uint32_t codepoint, char *out);
int utf8_strlen(const char *s);
bool_t utf8_is_valid(const char *s);

/* Glyph cache */
#define GLYPH_CACHE_SIZE 512

struct glyph_cache_entry {
    uint32_t codepoint;
    uint8_t bitmap[16];
    bool_t valid;
};

void glyph_cache_init(void);
const uint8_t *glyph_cache_get(uint32_t codepoint);
void glyph_cache_put(uint32_t codepoint, const uint8_t *bitmap);

/* Unicode blocks */
bool_t is_latin1(uint32_t cp);
bool_t is_greek(uint32_t cp);
bool_t is_cyrillic(uint32_t cp);
bool_t is_box_drawing(uint32_t cp);

/* Box drawing characters U+2500-U+257F */
const uint8_t *box_drawing_glyph(uint32_t cp);

#endif
