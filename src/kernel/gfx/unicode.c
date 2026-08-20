/*
 * QitoOS - UTF-8 decoding and Unicode support
 */

#include <kernel/unicode.h>
#include <kernel/string.h>
#include <kernel/mm.h>
#include <kernel/log.h>

static struct glyph_cache_entry cache[GLYPH_CACHE_SIZE];
static int cache_next=0;

void glyph_cache_init(void)
{
    memset(cache,0,sizeof(cache));
    KLOG_INFO("unicode","UTF-8 decoder ready, glyph cache %d entries, Latin-1/Greek/Cyrillic/box-drawing", GLYPH_CACHE_SIZE);
}

int utf8_decode(const char *s, uint32_t *out_cp)
{
    if (!s || !out_cp) return 0;
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *out_cp = c0;
        return 1;
    } else if ((c0 & 0xE0) == 0xC0) {
        unsigned char c1 = (unsigned char)s[1];
        if ((c1 & 0xC0) != 0x80) return -1;
        *out_cp = ((c0 & 0x1F)<<6) | (c1 & 0x3F);
        if (*out_cp < 0x80) return -1; // overlong
        return 2;
    } else if ((c0 & 0xF0) == 0xE0) {
        unsigned char c1 = (unsigned char)s[1];
        unsigned char c2 = (unsigned char)s[2];
        if ((c1 & 0xC0)!=0x80 || (c2 & 0xC0)!=0x80) return -1;
        *out_cp = ((c0 & 0x0F)<<12) | ((c1 & 0x3F)<<6) | (c2 & 0x3F);
        if (*out_cp < 0x800) return -1;
        if (*out_cp >=0xD800 && *out_cp <=0xDFFF) return -1; // surrogate
        return 3;
    } else if ((c0 & 0xF8) == 0xF0) {
        unsigned char c1 = (unsigned char)s[1];
        unsigned char c2 = (unsigned char)s[2];
        unsigned char c3 = (unsigned char)s[3];
        if ((c1 & 0xC0)!=0x80 || (c2 & 0xC0)!=0x80 || (c3 & 0xC0)!=0x80) return -1;
        *out_cp = ((c0 & 0x07)<<18) | ((c1 & 0x3F)<<12) | ((c2 & 0x3F)<<6) | (c3 & 0x3F);
        if (*out_cp < 0x10000 || *out_cp > 0x10FFFF) return -1;
        return 4;
    }
    return -1;
}

int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0]=(char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0]=(char)(0xC0 | (cp>>6));
        out[1]=(char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0]=(char)(0xE0 | (cp>>12));
        out[1]=(char)(0x80 | ((cp>>6)&0x3F));
        out[2]=(char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0]=(char)(0xF0 | (cp>>18));
        out[1]=(char)(0x80 | ((cp>>12)&0x3F));
        out[2]=(char)(0x80 | ((cp>>6)&0x3F));
        out[3]=(char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

int utf8_strlen(const char *s)
{
    int len=0;
    while (*s) {
        uint32_t cp;
        int bytes=utf8_decode(s,&cp);
        if (bytes<=0) { s++; len++; } else { s+=bytes; len++; }
    }
    return len;
}

bool_t utf8_is_valid(const char *s)
{
    while (*s) {
        uint32_t cp;
        int b=utf8_decode(s,&cp);
        if (b<=0) return false;
        s+=b;
    }
    return true;
}

const uint8_t *glyph_cache_get(uint32_t cp)
{
    for (int i=0;i<GLYPH_CACHE_SIZE;i++) if (cache[i].valid && cache[i].codepoint==cp) return cache[i].bitmap;
    return NULL;
}

void glyph_cache_put(uint32_t cp, const uint8_t *bitmap)
{
    int idx=cache_next % GLYPH_CACHE_SIZE;
    cache[idx].codepoint=cp;
    memcpy(cache[idx].bitmap,bitmap,16);
    cache[idx].valid=true;
    cache_next++;
}

bool_t is_latin1(uint32_t cp){ return cp>=0x00A0 && cp<=0x00FF; }
bool_t is_greek(uint32_t cp){ return cp>=0x0370 && cp<=0x03FF; }
bool_t is_cyrillic(uint32_t cp){ return cp>=0x0400 && cp<=0x04FF; }
bool_t is_box_drawing(uint32_t cp){ return cp>=0x2500 && cp<=0x257F; }

const uint8_t *box_drawing_glyph(uint32_t cp)
{
    // Simple box drawing: return precomputed patterns for common ones
    // For MVP, we provide a few basic glyphs: ─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼
    // Each is 8x16 bitmap
    // We'll have static patterns
    static uint8_t horiz[16]={0,0,0,0,0,0,0,0xFF,0xFF,0,0,0,0,0,0,0};
    static uint8_t vert[16]={0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18};
    static uint8_t cross[16]={0,0,0,0x18,0x18,0xFF,0xFF,0xFF,0xFF,0x18,0x18,0,0,0,0,0};
    switch(cp){
        case 0x2500: return horiz; // ─
        case 0x2502: return vert;  // │
        case 0x250C: { static uint8_t g[16]={0,0,0,0,0x18,0x18,0x18,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}; return g; } // ┌
        case 0x2510: { static uint8_t g[16]={0,0,0,0,0x18,0x18,0x18,0xFF,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18}; return g; } // ┐ (simplified same as ┌)
        case 0x2514: return vert;
        case 0x2518: return vert;
        case 0x253C: return cross; // ┼
        default: return NULL;
    }
}
