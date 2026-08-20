/*
 * QitoOS - system clipboard
 */

#include <kernel/clipboard.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>

static char             buffer[CLIPBOARD_MAX];
static size_t           length;
static clipboard_kind_t kind;
static char             owner[32];
static uint64_t         changed_at;
static spinlock_t       lock;

void clipboard_init(void)
{
    spinlock_init(&lock, "clipboard");
    buffer[0] = '\0';
    length    = 0;
    kind      = CLIP_TEXT;
    KLOG_INFO("clipboard", "ready, %d byte capacity", CLIPBOARD_MAX);
}

int clipboard_set_len(const void *data, size_t len, clipboard_kind_t new_kind,
                      const char *new_owner)
{
    if (!data) {
        return -1;
    }

    bool_t irq = spinlock_acquire(&lock);

    length = MIN(len, (size_t)CLIPBOARD_MAX - 1);
    memcpy(buffer, data, length);
    buffer[length] = '\0';

    kind       = new_kind;
    changed_at = time_uptime_ms();
    strlcpy(owner, new_owner ? new_owner : "unknown", sizeof(owner));

    spinlock_release(&lock, irq);

    KLOG_DEBUG("clipboard", "%llu bytes from %s",
               (unsigned long long)length, owner);
    return 0;
}

int clipboard_set(const char *text, clipboard_kind_t new_kind,
                  const char *new_owner)
{
    return clipboard_set_len(text, strlen(text), new_kind, new_owner);
}

const char *clipboard_get(void)
{
    return length ? buffer : NULL;
}

size_t clipboard_length(void)
{
    return length;
}

clipboard_kind_t clipboard_kind(void)
{
    return kind;
}

const char *clipboard_owner(void)
{
    return owner[0] ? owner : "none";
}

uint64_t clipboard_changed_at(void)
{
    return changed_at;
}

void clipboard_clear(void)
{
    bool_t irq = spinlock_acquire(&lock);
    buffer[0] = '\0';
    length    = 0;
    spinlock_release(&lock, irq);
}

void clipboard_preview(char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (length == 0) {
        strlcpy(out, "(empty)", size);
        return;
    }

    /* One line, with control characters made visible. */
    size_t index = 0;
    for (size_t i = 0; i < length && index < size - 4; i++) {
        char c = buffer[i];
        if (c == '\n') {
            if (index < size - 6) {
                out[index++] = ' ';
                out[index++] = '/';
                out[index++] = ' ';
            } else {
                break;
            }
        } else if (c >= 32 && c < 127) {
            out[index++] = c;
        } else {
            out[index++] = '.';
        }
    }
    out[index] = '\0';

    if (index >= size - 4 && length > index) {
        strlcpy(out + size - 4, "...", 4);
    }
}
