/*
 * QitoOS - QTI icon decoder
 *
 * Real binary images, not ASCII art — QTI stores actual pixel data.
 * Five sizes: 16,32,64,128,256 default 64.
 * Header 32B: magic "QTI1", version, frame_count ≤5, payload_size, checksum, flags, name[12]
 * Entry 16B: width, height, encoding, palette_size, reserved, offset, size
 * Encodings: 0 RAW (BGRA), 1 RLE (count, B,G,R,A), 2 INDEX (palette + 1 byte/pixel)
 * Frames stored largest-first, size selection forward scan.
 */

#include <kernel/qti.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/fb.h>
#include <kernel/printf.h>

#define QTI_REGISTRY_MAX 64

struct registry_entry {
    char             name[32];
    struct qti_image image;
    bool_t           used;
    /* Keep original data for listing? */
    int              default_size;
};

static struct registry_entry registry[QTI_REGISTRY_MAX];
static int registry_count;

int qti_probe(const void *data, size_t len, struct qti_header *out)
{
    if (!data || len < sizeof(struct qti_header)) return -1;
    const struct qti_header *header = (const struct qti_header *)data;
    if (memcmp(header->magic, QTI_MAGIC, 4) != 0) return -1;
    if (header->version != QTI_VERSION) return -1;
    if (header->frame_count == 0 || header->frame_count > QTI_MAX_FRAMES) return -1;
    if (header->payload_size > 4*1024*1024) return -1;
    if (out) *out = *header;
    return 0;
}

static int decode_frame(const struct qti_entry *entry, const uint8_t *payload,
                        size_t payload_len, struct qti_image *out)
{
    int width = entry->width;
    int height = entry->height;
    if (width <=0 || height <=0 || width>QTI_MAX_DIM || height>QTI_MAX_DIM) return -1;
    if ((size_t)entry->offset + entry->size > payload_len) return -1;
    size_t pixel_count = (size_t)width * height;
    uint32_t *pixels = kmalloc(pixel_count * sizeof(uint32_t));
    if (!pixels) return -1;
    const uint8_t *src = payload + entry->offset;
    size_t len = entry->size;
    switch (entry->encoding) {
    case QTI_RAW: {
        if (len < pixel_count*4) { kfree(pixels); return -1; }
        for (size_t i=0;i<pixel_count;i++) {
            pixels[i] = ((uint32_t)src[i*4+3]<<24) |
                        ((uint32_t)src[i*4+2]<<16) |
                        ((uint32_t)src[i*4+1]<<8) |
                        (uint32_t)src[i*4+0];
        }
        break;
    }
    case QTI_RLE: {
        size_t written=0, offset=0;
        while (written<pixel_count && offset+5 <= len) {
            uint8_t count = src[offset];
            uint32_t pixel = ((uint32_t)src[offset+4]<<24) |
                             ((uint32_t)src[offset+3]<<16) |
                             ((uint32_t)src[offset+2]<<8) |
                             (uint32_t)src[offset+1];
            offset+=5;
            if (count==0) break;
            for (uint8_t i=0;i<count && written<pixel_count;i++) {
                pixels[written++]=pixel;
            }
        }
        while (written<pixel_count) pixels[written++]=0;
        break;
    }
    case QTI_INDEX: {
        int palette_size = entry->palette_size ? entry->palette_size : 256;
        size_t table_bytes = (size_t)palette_size*4;
        if (len < table_bytes+pixel_count) { kfree(pixels); return -1; }
        uint32_t palette[256];
        for (int i=0;i<palette_size;i++) {
            palette[i] = ((uint32_t)src[i*4+3]<<24) |
                         ((uint32_t)src[i*4+2]<<16) |
                         ((uint32_t)src[i*4+1]<<8) |
                         (uint32_t)src[i*4+0];
        }
        const uint8_t *indices = src+table_bytes;
        for (size_t i=0;i<pixel_count;i++) {
            uint8_t idx=indices[i];
            pixels[i]=(idx<palette_size)?palette[idx]:0;
        }
        break;
    }
    default:
        kfree(pixels);
        return -1;
    }
    out->width=width;
    out->height=height;
    out->pixels=pixels;
    return 0;
}

int qti_decode(const void *data, size_t len, int preferred, struct qti_image *out)
{
    struct qti_header header;
    if (!out || qti_probe(data,len,&header)!=0) return -1;
    size_t table_bytes = (size_t)header.frame_count * sizeof(struct qti_entry);
    if (len < sizeof(header)+table_bytes) return -1;
    const struct qti_entry *entries = (const struct qti_entry *)((const uint8_t*)data + sizeof(header));
    const uint8_t *payload = (const uint8_t*)data + sizeof(header)+table_bytes;
    size_t payload_len = len - sizeof(header)-table_bytes;
    /* Verify checksum */
    uint32_t sum=0;
    for (size_t i=0;i<payload_len;i++) sum+=payload[i];
    if (sum != header.checksum) {
        // Allow if payload_len matches header.payload_size? Checksum mismatch is error
        // But for robustness, verify
        if (payload_len != header.payload_size) return -1;
        // actual checksum check
        if (sum != header.checksum) return -1;
    }
    /* Validate entries */
    for (int i=0;i<header.frame_count;i++) {
        if (entries[i].offset + entries[i].size > payload_len) return -1;
        if (entries[i].width==0 || entries[i].height==0) return -1;
        if (entries[i].encoding>2) return -1;
    }
    /* Pick best: forward scan largest-first, but choose closest to preferred.
     * Spec says size selection is a single forward scan. We'll implement:
     * - scan frames in order (largest first), first frame <= preferred? Or closest.
     * Simpler: find closest dimension.
     */
    int best=0;
    int best_diff=0x7FFF;
    for (int i=0;i<header.frame_count;i++) {
        int diff = (int)entries[i].width - preferred;
        if (diff<0) diff=-diff;
        if (diff<best_diff || (diff==best_diff && entries[i].width>entries[best].width)) {
            best_diff=diff;
            best=i;
        }
    }
    return decode_frame(&entries[best], payload, payload_len, out);
}

void qti_free(struct qti_image *image)
{
    if (image && image->pixels) {
        kfree(image->pixels);
        image->pixels=NULL;
        image->width=image->height=0;
    }
}

int qti_load(const char *path, int preferred, struct qti_image *out)
{
    struct fs_stat stat;
    if (fs_stat(path,&stat)!=0) return -1;
    if (stat.size==0 || stat.size>2*1024*1024) return -1;
    void *buffer = kmalloc(stat.size);
    if (!buffer) return -1;
    size_t got=0;
    if (fs_read_file(path,buffer,stat.size,&got)!=0) { kfree(buffer); return -1; }
    int r = qti_decode(buffer,got,preferred,out);
    kfree(buffer);
    return r;
}

void qti_draw(const struct qti_image *image, int x, int y)
{
    if (!image || !image->pixels) return;
    for (int row=0; row<image->height; row++) {
        for (int col=0; col<image->width; col++) {
            uint32_t pixel = image->pixels[row*image->width+col];
            uint8_t alpha = pixel>>24;
            if (alpha==0) continue;
            if (alpha==255) fb_put_pixel(x+col,y+row,pixel & 0x00FFFFFF);
            else fb_blend_pixel(x+col,y+row,pixel & 0x00FFFFFF, alpha);
        }
    }
}

void qti_draw_scaled(const struct qti_image *image, int x, int y, int size)
{
    if (!image || !image->pixels || size<=0) return;
    for (int row=0; row<size; row++) {
        int sr = row*image->height/size;
        for (int col=0; col<size; col++) {
            int sc = col*image->width/size;
            uint32_t pixel = image->pixels[sr*image->width+sc];
            uint8_t alpha = pixel>>24;
            if (alpha==0) continue;
            if (alpha==255) fb_put_pixel(x+col,y+row,pixel & 0x00FFFFFF);
            else fb_blend_pixel(x+col,y+row,pixel & 0x00FFFFFF, alpha);
        }
    }
}

static int registry_add(const char *name, struct qti_image *image, int default_size)
{
    if (registry_count>=QTI_REGISTRY_MAX) return -1;
    struct registry_entry *e = &registry[registry_count++];
    strlcpy(e->name,name,sizeof(e->name));
    e->image=*image;
    e->used=true;
    e->default_size=default_size;
    return 0;
}

const struct qti_image *qti_get(const char *name)
{
    for (int i=0;i<registry_count;i++) {
        if (registry[i].used && strcmp(registry[i].name,name)==0) return &registry[i].image;
    }
    return NULL;
}

int qti_registry_count(void){ return registry_count; }
const char *qti_registry_name(int index){
    if (index<0||index>=registry_count) return NULL;
    return registry[index].name;
}

void qti_init(void)
{
    registry_count=0;
    struct fs_node *dir = fs_lookup("/usr/share/icons");
    if (!dir) { KLOG_INFO("qti","no icon directory"); return; }
    struct fs_dirent dirent;
    int loaded=0;
    for (int i=0; fs_readdir(dir,i,&dirent)==0; i++) {
        size_t len=strlen(dirent.name);
        if (len<5) continue;
        if (strcmp(dirent.name+len-4,".qti")!=0 && strcmp(dirent.name+len-4,".qac")!=0) continue;
        char path[FS_PATH_MAX];
        snprintf(path,sizeof(path),"/usr/share/icons/%s",dirent.name);
        struct qti_image image;
        if (qti_load(path, QTI_DEFAULT_SIZE, &image)!=0) {
            KLOG_WARN("qti","%s invalid",dirent.name);
            continue;
        }
        char name[32];
        strlcpy(name,dirent.name,sizeof(name));
        size_t nlen=strlen(name);
        if (nlen>4) name[nlen-4]='\0';
        if (registry_add(name,&image,QTI_DEFAULT_SIZE)!=0) { qti_free(&image); break; }
        loaded++;
    }
    KLOG_INFO("qti","%d icon(s) loaded",loaded);
}
