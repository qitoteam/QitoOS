/*
 * QitoOS - qtpkg package manager
 * Replaces git entirely.
 *
 * Entry file: /user/qtpkg/entry.var (user-facing c:root/user/qtpkg/entry.var)
 * Syntax: pkg1 = [1.001](url),[1.002](url2);
 *         pkg2 = # not implemented yet
 * One entry per line, name = then comma-separated [version](url) pairs, terminated by ;
 * # starts comment. Real parser with line numbers.
 *
 * Each URL points at pkg.qtpkg_profile manifest – metadata describing project.
 * Carries name, version, description, arch, dependencies, install paths, checksums, payload URL.
 */

#ifndef QITO_QTPKG_H
#define QITO_QTPKG_H

#include <kernel/types.h>

struct shell;

#define QTPKG_ENTRY_PATH "/user/qtpkg/entry.var"
#define QTPKG_PROFILE_SUFFIX ".qtpkg_profile"
#define QTPKG_MAX_PACKAGES 128
#define QTPKG_MAX_VERSIONS 16
#define QTPKG_MAX_DEPS 16
#define QTPKG_URL_MAX 256
#define QTPKG_NAME_MAX 64
#define QTPKG_VERSION_MAX 32
#define QTPKG_PATH_MAX 128

struct qtpkg_version {
    char version[QTPKG_VERSION_MAX];
    char url[QTPKG_URL_MAX];
};

struct qtpkg_entry {
    char name[QTPKG_NAME_MAX];
    struct qtpkg_version versions[QTPKG_MAX_VERSIONS];
    int version_count;
    int line_number;
};

struct qtpkg_profile {
    char name[QTPKG_NAME_MAX];
    char version[QTPKG_VERSION_MAX];
    char description[256];
    char arch[16];
    char depends[QTPKG_MAX_DEPS][QTPKG_NAME_MAX];
    int dep_count;
    char payload_url[QTPKG_URL_MAX];
    char install_path[QTPKG_PATH_MAX];
    char checksum[128]; /* sha256 */
    char signature[256]; /* ed25519 sig of profile */
};

#define QTPKG_MANIFEST_MAX_FILES 16
struct qtpkg_manifest_file {
    char path[QTPKG_PATH_MAX];
    char sha256[65];
};

struct qtpkg_manifest {
    struct qtpkg_profile profile;
    struct qtpkg_manifest_file files[QTPKG_MANIFEST_MAX_FILES];
    int file_count;
};

/* Parsing */
int qtpkg_parse_entry_file(const char *content, struct qtpkg_entry *out_entries, int max_entries, char *error, size_t err_size);

/* Fetch profile via HTTP */
int qtpkg_fetch_profile(const char *url, struct qtpkg_profile *out_profile, char *error, size_t err_size);

/* Commands */
int qtpkg_command(struct shell *sh, int argc, char **argv);

int qtpkg_install(struct shell *sh, const char *pkg_name);
int qtpkg_update(struct shell *sh);
int qtpkg_os_update(struct shell *sh);
int qtpkg_upgrade_self(struct shell *sh);
int qtpkg_fix_os(struct shell *sh);
int qtpkg_fix_driver(struct shell *sh, const char *driver);
int qtpkg_list(struct shell *sh, const char *filter);
int qtpkg_search(struct shell *sh, const char *query);
int qtpkg_remove(struct shell *sh, const char *pkg_name);
int qtpkg_info(struct shell *sh, const char *pkg_name);
int qtpkg_rollback(struct shell *sh, const char *snapshot);

/* Integrity */
int qtpkg_verify_checksum(const char *path, const char *expected_sha256);
int qtpkg_verify_signature(const char *data, size_t len, const char *sig);

/* TLS check */
bool_t qtpkg_is_https(const char *url);

#endif /* QITO_QTPKG_H */
