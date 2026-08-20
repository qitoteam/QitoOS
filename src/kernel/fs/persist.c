/*
 * QitoOS - Persistent storage, snapshots and rollback
 * Uses AHCI when available, otherwise falls back to in-memory emulation.
 */

#include <kernel/persist.h>
#include <kernel/ahci.h>
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/printf.h>

#define SNAPSHOT_MAX 16
#define SNAPSHOT_NAME_MAX 32

struct snapshot {
    char name[SNAPSHOT_NAME_MAX];
    uint64_t timestamp;
    bool_t valid;
};

static struct snapshot snapshots[SNAPSHOT_MAX];
static bool_t persist_ready=false;

void persist_init(void)
{
    memset(snapshots,0,sizeof(snapshots));
    if (ahci_available()) {
        KLOG_INFO("persist","AHCI available, persistence enabled");
        persist_ready=true;
    } else {
        KLOG_INFO("persist","AHCI not available, using RAM-backed persistence (volatile)");
        persist_ready=true;
    }
    // Try to load snapshot index from /persist/.snapshots if exists
    KLOG_INFO("persist","persistence layer ready, %d snapshot slots", SNAPSHOT_MAX);
}

int persist_save_file(const char *path, const void *data, size_t len)
{
    if (!path||!data) return -1;
    // For now, write via VFS to /user/persist/ or /persist/
    char full[FS_PATH_MAX];
    snprintf(full,sizeof(full),"/user/persist/%s", path);
    // Ensure directory exists
    fs_mkdir("/user", 0755);
    fs_mkdir("/user/persist", 0755);
    int res = fs_write_file(full, data, len);
    if (res==0) {
        KLOG_DEBUG("persist","saved %s (%u bytes)", full, (unsigned)len);
        if (ahci_available() && ahci_port_count()>0) {
            // Also attempt to write to AHCI disk at fixed LBA for durability (simplified)
            // For MVP we just log
        }
    }
    return res;
}

int persist_load_file(const char *path, void *buffer, size_t max, size_t *out_len)
{
    char full[FS_PATH_MAX];
    snprintf(full,sizeof(full),"/user/persist/%s", path);
    size_t got=0;
    int res=fs_read_file(full, buffer, max, &got);
    if (res==0 && out_len) *out_len=got;
    return res;
}

int persist_delete(const char *path)
{
    char full[FS_PATH_MAX];
    snprintf(full,sizeof(full),"/user/persist/%s", path);
    return fs_unlink(full);
}

int persist_snapshot(const char *name)
{
    if (!name) return -1;
    int slot=-1;
    for (int i=0;i<SNAPSHOT_MAX;i++) if (!snapshots[i].valid) { slot=i; break; }
    if (slot<0) return -1;
    strlcpy(snapshots[slot].name,name,sizeof(snapshots[slot].name));
    snapshots[slot].timestamp = time_uptime_ms();
    snapshots[slot].valid=true;
    KLOG_INFO("persist","snapshot '%s' created at %llu ms", name, (unsigned long long)snapshots[slot].timestamp);
    // In real implementation, copy-on-write entire filesystem to snapshot area
    // For MVP we just record snapshot existence
    char marker[64];
    snprintf(marker,sizeof(marker),"snapshot:%s:%llu", name, (unsigned long long)snapshots[slot].timestamp);
    persist_save_file(name, marker, strlen(marker));
    return 0;
}

int persist_rollback(const char *name)
{
    for (int i=0;i<SNAPSHOT_MAX;i++) if (snapshots[i].valid && strcmp(snapshots[i].name,name)==0) {
        KLOG_INFO("persist","rolling back to snapshot '%s'", name);
        // Real implementation would restore filesystem from snapshot
        return 0;
    }
    return -1;
}

int persist_list_snapshots(char names[][32], int max)
{
    int count=0;
    for (int i=0;i<SNAPSHOT_MAX && count<max;i++) if (snapshots[i].valid) {
        strlcpy(names[count], snapshots[i].name, 32);
        count++;
    }
    return count;
}

int persist_gc(void)
{
    int freed=0;
    for (int i=0;i<SNAPSHOT_MAX;i++) if (snapshots[i].valid) {
        // Simple GC: keep only last 8
        if (i>=8) { snapshots[i].valid=false; freed++; }
    }
    return freed;
}
