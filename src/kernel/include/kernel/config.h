/*
 * Qira OS - system configuration store
 *
 * A small typed key/value database persisted to /etc/qira.conf. Both the
 * shells and the desktop read and write settings through this interface so
 * there is a single source of truth for how the system is configured.
 */
#ifndef QIRA_CONFIG_H
#define QIRA_CONFIG_H

#include <kernel/types.h>

#define CONFIG_PATH        "/etc/qira.conf"
#define CONFIG_KEY_MAX     64
#define CONFIG_VALUE_MAX   192
#define CONFIG_MAX_ENTRIES 96

struct shell;

typedef enum {
    CONFIG_STRING = 0,
    CONFIG_INT,
    CONFIG_BOOL,
} config_type_t;

struct config_entry {
    char          key[CONFIG_KEY_MAX];
    char          value[CONFIG_VALUE_MAX];
    char          description[96];
    config_type_t type;
    bool_t        dirty;
};

void config_init(void);
void config_reset_defaults(void);

int  config_load(void);
int  config_save(void);

const char *config_get_string(const char *key, const char *fallback);
int         config_get_int(const char *key, int fallback);
bool_t      config_get_bool(const char *key, bool_t fallback);

int config_set_string(const char *key, const char *value);
int config_set_int(const char *key, int value);
int config_set_bool(const char *key, bool_t value);

/* Introspection. */
int  config_count(void);
const struct config_entry *config_at(int index);
void config_list(struct shell *sh);

#endif /* QIRA_CONFIG_H */
