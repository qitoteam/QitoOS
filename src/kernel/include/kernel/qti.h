/*
 * QitoOS - QTI, QiTo Icons
 *
 * Replacement for QAC. Real binary images, not ASCII art — QTI stores actual
 * pixel data with hex colour values.
 *
 * Five sizes: 16, 32, 64, 128, 256. Default is 64 (third). Built-in apps can
 * switch default size; external apps may ship all five or any subset.
 *
 * Header (32 B, little-endian):
 *   magic "QTI1", version, frame_count (≤5), payload_size, checksum, flags, name[12]
 * Then one 16-byte entry per frame:
 *   width, height, encoding, palette_size, reserved, offset, size
 * Encodings: 0 RAW (BGRA), 1 RLE (5-byte runs: count 1–255, B,G,R,A), 2 INDEX
 * (palette + 1 byte/pixel). Store frames largest-first so size selection is a
 * single forward scan. Validate every offset and checksum before decoding.
 */

#ifndef QITO_QTI_H
#define QITO_QTI_H

#include <kernel/types.h>

#define QTI_MAGIC       "QTI1"
#define QTI_VERSION     1
#define QTI_MAX_FRAMES  5
#define QTI_MAX_DIM     256
#define QTI_NAME_MAX    12

#define QTI_SIZE_16   16
#define QTI_SIZE_32   32
#define QTI_SIZE_64   64
#define QTI_SIZE_128 128
#define QTI_SIZE_256 256
#define QTI_DEFAULT_SIZE QTI_SIZE_64

typedef enum {
    QTI_RAW   = 0,  /* BGRA, 4 bytes per pixel */
    QTI_RLE   = 1,  /* runs: count 1-255, B,G,R,A */
    QTI_INDEX = 2,  /* palette + 1 byte/pixel */
} qti_encoding_t;

struct qti_header {
    char     magic[4];        /* "QTI1" */
    uint16_t version;
    uint16_t frame_count;     /* ≤5 */
    uint32_t payload_size;
    uint32_t checksum;        /* wrapping sum of payload */
    uint32_t flags;
    char     name[QTI_NAME_MAX];
} PACKED;

struct qti_entry {
    uint16_t width;
    uint16_t height;
    uint8_t  encoding;        /* qti_encoding_t */
    uint8_t  palette_size;    /* colours for INDEX; 0 otherwise */
    uint16_t reserved;
    uint32_t offset;          /* from start of payload */
    uint32_t size;            /* encoded bytes */
} PACKED;

struct qti_image {
    int       width;
    int       height;
    uint32_t *pixels;         /* 0xAARRGGBB, heap owned */
};

/* Decode frame best matching preferred size */
int  qti_decode(const void *data, size_t len, int preferred, struct qti_image *out);
void qti_free(struct qti_image *image);

int  qti_load(const char *path, int preferred, struct qti_image *out);
int  qti_probe(const void *data, size_t len, struct qti_header *out);

void qti_draw(const struct qti_image *image, int x, int y);
void qti_draw_scaled(const struct qti_image *image, int x, int y, int size);

void qti_init(void);
const struct qti_image *qti_get(const char *name);

int  qti_registry_count(void);
const char *qti_registry_name(int index);

/* Legacy QAC compatibility wrappers */
#define QAC_MAGIC QTI_MAGIC
#define QAC_VERSION QTI_VERSION
#define QAC_MAX_FRAMES QTI_MAX_FRAMES
#define QAC_MAX_DIM QTI_MAX_DIM
#define QAC_RAW QTI_RAW
#define QAC_RLE QTI_RLE
#define QAC_INDEX QTI_INDEX
typedef struct qti_header qac_header;
typedef struct qti_entry qac_entry;
typedef struct qti_image qac_image;
#define qac_decode qti_decode
#define qac_free qti_free
#define qac_load qti_load
#define qac_probe qti_probe
#define qac_draw qti_draw
#define qac_draw_scaled qti_draw_scaled
#define qac_init qti_init
#define qac_get qti_get
#define qac_registry_count qti_registry_count
#define qac_registry_name qti_registry_name

#endif /* QITO_QTI_H */
