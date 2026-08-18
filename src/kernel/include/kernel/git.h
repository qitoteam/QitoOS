/*
 * Qira OS - git client
 *
 * Implements git's smart HTTP protocol. Reference discovery is complete;
 * fetching downloads a real packfile but does not unpack it, because that
 * needs zlib inflate and delta resolution which are not implemented. The
 * shell command says so rather than implying a working tree was created.
 */
#ifndef QIRA_GIT_H
#define QIRA_GIT_H

#include <kernel/types.h>

#define GIT_MAX_REFS 64

typedef enum {
    GIT_REF_BRANCH = 0,
    GIT_REF_TAG,
    GIT_REF_HEAD,
    GIT_REF_OTHER,
} git_ref_kind_t;

struct git_ref {
    char hash[41];      /* 40 hexadecimal characters plus a terminator */
    char name[128];     /* the full reference name                     */
};

struct git_fetch_result {
    size_t   pack_size;
    uint32_t pack_version;
    uint32_t object_count;
    bool_t   saved;
    char     path[128];
};

/*
 * List the references a remote advertises. Returns how many were found, or
 * negative on failure with `error` describing why.
 */
int git_ls_remote(const char *url, struct git_ref *refs, int max_refs,
                  char *error, size_t error_size);

/*
 * Download the packfile containing `want_hash`. The packfile is written to
 * `destination` if that is non-NULL. The pack is not unpacked.
 */
int git_fetch_pack(const char *url, const char *want_hash,
                   const char *destination, struct git_fetch_result *out,
                   char *error, size_t error_size);

const char    *git_ref_short_name(const char *full);
git_ref_kind_t git_ref_kind(const char *full);

#endif /* QIRA_GIT_H */
