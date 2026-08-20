/*
 * QitoOS - Persistent storage, snapshots and rollback
 */

#ifndef QITO_PERSIST_H
#define QITO_PERSIST_H

#include <kernel/types.h>

void persist_init(void);
int  persist_save_file(const char *path, const void *data, size_t len);
int  persist_load_file(const char *path, void *buffer, size_t max, size_t *out_len);
int  persist_delete(const char *path);
int  persist_snapshot(const char *name);
int  persist_rollback(const char *name);
int  persist_list_snapshots(char names[][32], int max);
int  persist_gc(void);

#endif /* QITO_PERSIST_H */
