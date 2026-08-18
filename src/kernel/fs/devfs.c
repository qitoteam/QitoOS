/*
 * Qira OS - device filesystem
 *
 * Exposes the standard character devices under /dev. These are ordinary VFS
 * nodes with an ops table, so `cat /dev/random` and friends work from either
 * shell.
 */

#include <kernel/fs.h>
#include <kernel/random.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/console.h>
#include <kernel/serial.h>
#include <kernel/time.h>
#include <kernel/io.h>
#include <kernel/mm.h>

/* --- /dev/null: reads return EOF, writes are discarded ---------------- */

static ssize_t null_read(struct fs_node *node, uint64_t offset, void *buf,
                         size_t len)
{
    UNUSED(node);
    UNUSED(offset);
    UNUSED(buf);
    UNUSED(len);
    return 0;
}

static ssize_t null_write(struct fs_node *node, uint64_t offset, const void *buf,
                          size_t len)
{
    UNUSED(node);
    UNUSED(offset);
    UNUSED(buf);
    return (ssize_t)len;
}

static const struct fs_ops null_ops = {
    .read  = null_read,
    .write = null_write,
};

/* --- /dev/zero: reads return zeroes ----------------------------------- */

static ssize_t zero_read(struct fs_node *node, uint64_t offset, void *buf,
                         size_t len)
{
    UNUSED(node);
    UNUSED(offset);
    memset(buf, 0, len);
    return (ssize_t)len;
}

static const struct fs_ops zero_ops = {
    .read  = zero_read,
    .write = null_write,
};

/* --- /dev/random and /dev/urandom ------------------------------------- */

/*
 * The device nodes draw from the system generator in kernel/random.h. It is
 * fast and well distributed but not cryptographically secure, which is why
 * /dev/random and /dev/urandom behave identically here: pretending one of
 * them blocked for entropy would be a lie.
 */

static ssize_t random_read(struct fs_node *node, uint64_t offset, void *buf,
                           size_t len)
{
    UNUSED(node);
    UNUSED(offset);

    uint8_t *out = (uint8_t *)buf;
    size_t   i   = 0;

    while (i < len) {
        uint64_t value = random_u64();
        size_t   chunk = MIN(sizeof(value), len - i);
        memcpy(out + i, &value, chunk);
        i += chunk;
    }
    return (ssize_t)len;
}

static const struct fs_ops random_ops = {
    .read  = random_read,
    .write = null_write,
};

/* --- /dev/console: writes go to the console, reads to the input queue -- */

static ssize_t console_dev_read(struct fs_node *node, uint64_t offset, void *buf,
                                size_t len)
{
    UNUSED(node);
    UNUSED(offset);
    return console_read(buf, len);
}

static ssize_t console_dev_write(struct fs_node *node, uint64_t offset,
                                 const void *buf, size_t len)
{
    UNUSED(node);
    UNUSED(offset);
    console_write((const char *)buf, len);
    return (ssize_t)len;
}

static const struct fs_ops console_ops = {
    .read  = console_dev_read,
    .write = console_dev_write,
};

/* --- /dev/serial ------------------------------------------------------ */

static ssize_t serial_dev_write(struct fs_node *node, uint64_t offset,
                                const void *buf, size_t len)
{
    UNUSED(node);
    UNUSED(offset);
    serial_write_len((const char *)buf, len);
    return (ssize_t)len;
}

static ssize_t serial_dev_read(struct fs_node *node, uint64_t offset, void *buf,
                               size_t len)
{
    UNUSED(node);
    UNUSED(offset);

    uint8_t *out   = (uint8_t *)buf;
    size_t   count = 0;

    while (count < len) {
        int ch = serial_getc_nonblock();
        if (ch < 0) {
            break;
        }
        out[count++] = (uint8_t)ch;
    }
    return (ssize_t)count;
}

static const struct fs_ops serial_ops = {
    .read  = serial_dev_read,
    .write = serial_dev_write,
};

/* --- /dev/kmsg: the kernel log ring ----------------------------------- */

static ssize_t kmsg_read(struct fs_node *node, uint64_t offset, void *buf,
                         size_t len)
{
    UNUSED(node);
    return (ssize_t)log_read((char *)buf, len, (size_t)offset);
}

static int kmsg_refresh(struct fs_node *node)
{
    node->size = log_size();
    return 0;
}

static const struct fs_ops kmsg_ops = {
    .read    = kmsg_read,
    .refresh = kmsg_refresh,
};

void devfs_init(void)
{
    random_init();

    fs_register_node("/dev", "null", FS_DEV, &null_ops, NULL);
    fs_register_node("/dev", "zero", FS_DEV, &zero_ops, NULL);
    fs_register_node("/dev", "random", FS_DEV, &random_ops, NULL);
    fs_register_node("/dev", "urandom", FS_DEV, &random_ops, NULL);
    fs_register_node("/dev", "console", FS_DEV, &console_ops, NULL);
    fs_register_node("/dev", "serial", FS_DEV, &serial_ops, NULL);
    fs_register_node("/dev", "kmsg", FS_DEV, &kmsg_ops, NULL);

    /* /dev/null must be writable by everyone. */
    struct fs_node *node = fs_lookup("/dev/null");
    if (node) {
        node->permissions = 0666;
    }

    KLOG_INFO("devfs", "device nodes registered under /dev");
}
