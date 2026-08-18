/*
 * Qira OS - QAC, the Qira Application iCon format
 *
 * A small, self-describing raster image format for application icons. It is
 * deliberately simple enough to decode inside a kernel with no library
 * support, while still doing the things an icon format needs: several sizes
 * in one file, an alpha channel, and cheap lossless compression.
 *
 * File layout
 * -----------
 *
 *   struct qac_header          32 bytes
 *   struct qac_entry[frames]   16 bytes each, one per size
 *   payload                    the frames' pixel data, back to back
 *
 * Every multi-byte field is little-endian.
 *
 * Pixel data is stored in one of three encodings, chosen per frame:
 *
 *   QAC_RAW    width * height BGRA pixels, 4 bytes each
 *   QAC_RLE    runs of (count, B, G, R, A); count is 1-255
 *   QAC_INDEX  a palette of up to 256 BGRA colours followed by one byte per
 *              pixel. Icons are usually flat-shaded, so this is typically the
 *              smallest of the three.
 *
 * The encoder in tools/mkqac.py tries all three and keeps whichever is
 * smallest, so a file never costs more than raw storage.
 */
#ifndef QIRA_QAC_H
#define QIRA_QAC_H

#include <kernel/types.h>

#define QAC_MAGIC       "QACI"
#define QAC_VERSION     1
#define QAC_MAX_FRAMES  8
#define QAC_MAX_DIM     256

typedef enum {
    QAC_RAW   = 0,
    QAC_RLE   = 1,
    QAC_INDEX = 2,
} qac_encoding_t;

struct qac_header {
    char     magic[4];        /* "QACI"                                    */
    uint16_t version;
    uint16_t frame_count;
    uint32_t payload_size;    /* total bytes of frame data                 */
    uint32_t checksum;        /* 32-bit wrapping sum of the payload        */
    uint32_t flags;
    char     name[12];        /* short label, for tooling                  */
} PACKED;

struct qac_entry {
    uint16_t width;
    uint16_t height;
    uint8_t  encoding;        /* qac_encoding_t                            */
    uint8_t  palette_size;    /* colours, for QAC_INDEX; 0 otherwise       */
    uint16_t reserved;
    uint32_t offset;          /* from the start of the payload             */
    uint32_t size;            /* encoded bytes                             */
} PACKED;

/* A decoded icon, ready to blit. */
struct qac_image {
    int       width;
    int       height;
    uint32_t *pixels;         /* 0xAARRGGBB, top-left origin, heap owned   */
};

/*
 * Decode the frame whose size best matches `preferred`. Returns 0 on success,
 * and fills `out`; the caller owns out->pixels and must qac_free() it.
 */
int  qac_decode(const void *data, size_t len, int preferred, struct qac_image *out);
void qac_free(struct qac_image *image);

/* Read and decode in one step. */
int  qac_load(const char *path, int preferred, struct qac_image *out);

/* Inspect a file without decoding it. */
int  qac_probe(const void *data, size_t len, struct qac_header *out);

/* Draw a decoded icon with alpha blending. */
void qac_draw(const struct qac_image *image, int x, int y);

/* Draw scaled into a box, preserving the aspect ratio. */
void qac_draw_scaled(const struct qac_image *image, int x, int y, int size);

/* Register the built-in icons and load any found in /usr/share/icons. */
void qac_init(void);

/* Look up a built-in or loaded icon by name, or NULL. */
const struct qac_image *qac_get(const char *name);

int  qac_registry_count(void);
const char *qac_registry_name(int index);

#endif /* QIRA_QAC_H */
