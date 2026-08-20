/* QitoOS SDK - filesystem */
#ifndef QITO_SDK_FS_H
#define QITO_SDK_FS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { unsigned long size; int is_dir; } qito_stat_t;

int fs_read_file(const char *path, void *buffer, unsigned long max, unsigned long *out_got);
int fs_write_file(const char *path, const void *data, unsigned long len);
int fs_stat(const char *path, qito_stat_t *out);
int fs_unlink(const char *path);
int fs_mkdir(const char *path, unsigned int perms);
void *fs_lookup(const char *path); // returns opaque node

#ifdef __cplusplus
}
#endif

#endif
