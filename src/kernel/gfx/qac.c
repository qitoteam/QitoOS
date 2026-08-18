/*
 * Qira OS - QAC icon decoder
 *
 * See kernel/qac.h for the file layout. The decoder is defensive throughout:
 * icon files may come from the ramdisk or be written by a user, so every
 * offset and length is validated against the buffer before it is used.
 */

#include <kernel/qac.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/fb.h>
#include <kernel/printf.h>

#define QAC_REGISTRY_MAX 32

struct registry_entry {
    char             name[24];
    struct qac_image image;
    bool_t           used;
};

static struct registry_entry registry[QAC_REGISTRY_MAX];
static int                   registry_count;

int qac_probe(const void *data, size_t len, struct qac_header *out)
{
    if (!data || len < sizeof(struct qac_header)) {
        return -1;
    }

    const struct qac_header *header = (const struct qac_header *)data;

    if (memcmp(header->magic, QAC_MAGIC, 4) != 0) {
        return -1;
    }
    if (header->version != QAC_VERSION) {
        return -1;
    }
    if (header->frame_count == 0 || header->frame_count > QAC_MAX_FRAMES) {
        return -1;
    }

    if (out) {
        *out = *header;
    }
    return 0;
}

/* Expand one frame into a freshly allocated ARGB buffer. */
static int decode_frame(const struct qac_entry *entry, const uint8_t *payload,
                        size_t payload_len, struct qac_image *out)
{
    int width  = entry->width;
    int height = entry->height;

    if (width <= 0 || height <= 0 || width > QAC_MAX_DIM || height > QAC_MAX_DIM) {
        return -1;
    }
    if ((size_t)entry->offset + entry->size > payload_len) {
        return -1;
    }

    size_t pixel_count = (size_t)width * height;
    uint32_t *pixels   = kmalloc(pixel_count * sizeof(uint32_t));
    if (!pixels) {
        return -1;
    }

    const uint8_t *src = payload + entry->offset;
    size_t         len = entry->size;

    switch (entry->encoding) {
    case QAC_RAW: {
        if (len < pixel_count * 4) {
            kfree(pixels);
            return -1;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            /* Stored BGRA, presented as 0xAARRGGBB. */
            pixels[i] = ((uint32_t)src[i * 4 + 3] << 24) |
                        ((uint32_t)src[i * 4 + 2] << 16) |
                        ((uint32_t)src[i * 4 + 1] << 8) |
                        (uint32_t)src[i * 4 + 0];
        }
        break;
    }

    case QAC_RLE: {
        size_t written = 0;
        size_t offset  = 0;

        while (written < pixel_count && offset + 5 <= len) {
            uint8_t  count = src[offset];
            uint32_t pixel = ((uint32_t)src[offset + 4] << 24) |
                             ((uint32_t)src[offset + 3] << 16) |
                             ((uint32_t)src[offset + 2] << 8) |
                             (uint32_t)src[offset + 1];
            offset += 5;

            if (count == 0) {
                break;   /* malformed: a run must cover at least one pixel */
            }
            for (uint8_t i = 0; i < count && written < pixel_count; i++) {
                pixels[written++] = pixel;
            }
        }

        /* Any shortfall is transparent rather than uninitialised. */
        while (written < pixel_count) {
            pixels[written++] = 0;
        }
        break;
    }

    case QAC_INDEX: {
        int palette_size = entry->palette_size ? entry->palette_size : 256;
        size_t table_bytes = (size_t)palette_size * 4;

        if (len < table_bytes + pixel_count) {
            kfree(pixels);
            return -1;
        }

        uint32_t palette[256];
        for (int i = 0; i < palette_size; i++) {
            palette[i] = ((uint32_t)src[i * 4 + 3] << 24) |
                         ((uint32_t)src[i * 4 + 2] << 16) |
                         ((uint32_t)src[i * 4 + 1] << 8) |
                         (uint32_t)src[i * 4 + 0];
        }

        const uint8_t *indices = src + table_bytes;
        for (size_t i = 0; i < pixel_count; i++) {
            uint8_t index = indices[i];
            pixels[i] = (index < palette_size) ? palette[index] : 0;
        }
        break;
    }

    default:
        kfree(pixels);
        return -1;
    }

    out->width  = width;
    out->height = height;
    out->pixels = pixels;
    return 0;
}

int qac_decode(const void *data, size_t len, int preferred, struct qac_image *out)
{
    struct qac_header header;

    if (!out || qac_probe(data, len, &header) != 0) {
        return -1;
    }

    size_t table_bytes = (size_t)header.frame_count * sizeof(struct qac_entry);
    if (len < sizeof(header) + table_bytes) {
        return -1;
    }

    const struct qac_entry *entries =
        (const struct qac_entry *)((const uint8_t *)data + sizeof(header));
    const uint8_t *payload = (const uint8_t *)data + sizeof(header) + table_bytes;
    size_t payload_len     = len - sizeof(header) - table_bytes;

    /* Pick the frame whose dimension is closest to what was asked for. */
    int best      = 0;
    int best_diff = 0x7FFFFFFF;

    for (int i = 0; i < header.frame_count; i++) {
        int diff = (int)entries[i].width - preferred;
        if (diff < 0) {
            diff = -diff;
        }
        /* Prefer a larger frame when two are equally distant: scaling down
         * looks better than scaling up. */
        if (diff < best_diff ||
            (diff == best_diff && entries[i].width > entries[best].width)) {
            best_diff = diff;
            best      = i;
        }
    }

    return decode_frame(&entries[best], payload, payload_len, out);
}

void qac_free(struct qac_image *image)
{
    if (image && image->pixels) {
        kfree(image->pixels);
        image->pixels = NULL;
        image->width  = 0;
        image->height = 0;
    }
}

int qac_load(const char *path, int preferred, struct qac_image *out)
{
    struct fs_stat stat;
    if (fs_stat(path, &stat) != 0) {
        return -1;
    }
    if (stat.size == 0 || stat.size > 512 * 1024) {
        return -1;
    }

    void *buffer = kmalloc(stat.size);
    if (!buffer) {
        return -1;
    }

    size_t got = 0;
    if (fs_read_file(path, buffer, stat.size, &got) != 0) {
        kfree(buffer);
        return -1;
    }

    int result = qac_decode(buffer, got, preferred, out);
    kfree(buffer);
    return result;
}

void qac_draw(const struct qac_image *image, int x, int y)
{
    if (!image || !image->pixels) {
        return;
    }

    for (int row = 0; row < image->height; row++) {
        for (int col = 0; col < image->width; col++) {
            uint32_t pixel = image->pixels[row * image->width + col];
            uint8_t  alpha = (uint8_t)(pixel >> 24);

            if (alpha == 0) {
                continue;
            }
            if (alpha == 255) {
                fb_put_pixel(x + col, y + row, pixel & 0x00FFFFFF);
            } else {
                fb_blend_pixel(x + col, y + row, pixel & 0x00FFFFFF, alpha);
            }
        }
    }
}

void qac_draw_scaled(const struct qac_image *image, int x, int y, int size)
{
    if (!image || !image->pixels || size <= 0) {
        return;
    }

    /* Nearest-neighbour, which is what a bitmap icon wants at these sizes. */
    for (int row = 0; row < size; row++) {
        int source_row = row * image->height / size;

        for (int col = 0; col < size; col++) {
            int source_col = col * image->width / size;

            uint32_t pixel =
                image->pixels[source_row * image->width + source_col];
            uint8_t alpha = (uint8_t)(pixel >> 24);

            if (alpha == 0) {
                continue;
            }
            if (alpha == 255) {
                fb_put_pixel(x + col, y + row, pixel & 0x00FFFFFF);
            } else {
                fb_blend_pixel(x + col, y + row, pixel & 0x00FFFFFF, alpha);
            }
        }
    }
}

/* --- registry --------------------------------------------------------- */

static int registry_add(const char *name, struct qac_image *image)
{
    if (registry_count >= QAC_REGISTRY_MAX) {
        return -1;
    }

    struct registry_entry *entry = &registry[registry_count++];
    strlcpy(entry->name, name, sizeof(entry->name));
    entry->image = *image;
    entry->used  = true;
    return 0;
}

const struct qac_image *qac_get(const char *name)
{
    for (int i = 0; i < registry_count; i++) {
        if (registry[i].used && strcmp(registry[i].name, name) == 0) {
            return &registry[i].image;
        }
    }
    return NULL;
}

int qac_registry_count(void)
{
    return registry_count;
}

const char *qac_registry_name(int index)
{
    if (index < 0 || index >= registry_count) {
        return NULL;
    }
    return registry[index].name;
}

void qac_init(void)
{
    registry_count = 0;

    /* Load every icon the ramdisk placed under /usr/share/icons. */
    struct fs_node *dir = fs_lookup("/usr/share/icons");
    if (!dir) {
        KLOG_INFO("qac", "no icon directory; using drawn fallbacks");
        return;
    }

    struct fs_dirent dirent;
    int loaded = 0;

    for (int i = 0; fs_readdir(dir, i, &dirent) == 0; i++) {
        size_t len = strlen(dirent.name);
        if (len < 5 || strcmp(dirent.name + len - 4, ".qac") != 0) {
            continue;
        }

        char path[FS_PATH_MAX];
        snprintf(path, sizeof(path), "/usr/share/icons/%s", dirent.name);

        struct qac_image image;
        if (qac_load(path, 32, &image) != 0) {
            KLOG_WARN("qac", "%s: not a valid icon", dirent.name);
            continue;
        }

        /* Register under the name without its extension. */
        char name[24];
        strlcpy(name, dirent.name, sizeof(name));
        size_t name_len = strlen(name);
        if (name_len > 4) {
            name[name_len - 4] = '\0';
        }

        if (registry_add(name, &image) != 0) {
            qac_free(&image);
            break;
        }
        loaded++;
    }

    KLOG_INFO("qac", "%d icon(s) loaded from /usr/share/icons", loaded);
}
