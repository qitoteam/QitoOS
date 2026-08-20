/*
 * QitoOS - component/package registry
 *
 * Qito does not download software from a network repository. Instead the
 * package manager tracks the components that make up the running system:
 * which are installed, what they provide, and which optional ones are enabled.
 * This gives `pkg` real, honest work to do rather than pretending to be apt.
 */
#ifndef QITO_PKG_H
#define QITO_PKG_H

#include <kernel/types.h>

struct shell;

#define PKG_NAME_MAX 24
#define PKG_MAX      48

typedef enum {
    PKG_CORE = 0,      /* always present, cannot be removed */
    PKG_DRIVER,
    PKG_APPLICATION,
    PKG_LIBRARY,
    PKG_OPTIONAL,
} pkg_kind_t;

struct package {
    char       name[PKG_NAME_MAX];
    char       version[16];
    char       summary[72];
    pkg_kind_t kind;
    bool_t     installed;
    bool_t     enabled;
    uint32_t   size_kb;
};

void pkg_init(void);

int  pkg_count(void);
const struct package *pkg_at(int index);
const struct package *pkg_find(const char *name);

/* The `pkg` shell command. */
int pkg_command(struct shell *sh, int argc, char **argv);

#endif /* QITO_PKG_H */
