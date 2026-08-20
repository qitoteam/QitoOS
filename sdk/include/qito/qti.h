/* QitoOS SDK - QTI */
#ifndef QITO_SDK_QTI_H
#define QITO_SDK_QTI_H

#include <stdint.h>

#define QTI_MAGIC "QTI1"
#define QTI_DEFAULT_SIZE 64

typedef struct {
    int width, height;
    uint32_t *pixels; // 0xAARRGGBB
} qti_image_t;

int qti_load(const char *path, int preferred, qti_image_t *out);
void qti_free(qti_image_t *img);
void qti_draw(const qti_image_t *img, int x, int y);
void qti_draw_scaled(const qti_image_t *img, int x, int y, int size);

#endif
