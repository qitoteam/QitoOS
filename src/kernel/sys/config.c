/*
 * Qira OS - system configuration store
 *
 * Settings live in a fixed table seeded with sensible defaults, then merged
 * with whatever /etc/qira.conf contains. Saving rewrites the file in a simple
 * `key = value` format with comments preserved for the section headers.
 */

#include <kernel/config.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/log.h>
#include <kernel/shell.h>
#include <kernel/mm.h>

static struct config_entry entries[CONFIG_MAX_ENTRIES];
static int                 entry_count;

/* The defaults that ship with the system. */
struct default_entry {
    const char   *key;
    const char   *value;
    config_type_t type;
    const char   *description;
};

static const struct default_entry defaults[] = {
    /* identity */
    {"hostname", "qira", CONFIG_STRING, "network name of this machine"},
    {"timezone", "UTC", CONFIG_STRING, "display timezone"},
    {"locale", "en_US", CONFIG_STRING, "language and region"},

    /* desktop */
    {"desktop.theme", "aurora", CONFIG_STRING, "desktop colour theme"},
    {"desktop.wallpaper", "gradient", CONFIG_STRING,
     "wallpaper style: gradient, solid, stars"},
    {"desktop.animations", "1", CONFIG_BOOL, "enable window animations"},
    {"desktop.panel_position", "top", CONFIG_STRING, "panel edge: top or bottom"},
    {"desktop.show_clock", "1", CONFIG_BOOL, "show the clock in the panel"},
    {"desktop.double_click_ms", "400", CONFIG_INT,
     "double click interval in milliseconds"},

    /* window manager */
    {"wm.focus_follows_mouse", "0", CONFIG_BOOL, "focus windows on hover"},
    {"wm.snap_to_edges", "1", CONFIG_BOOL, "snap windows to screen edges"},
    {"wm.window_shadows", "1", CONFIG_BOOL, "draw shadows under windows"},

    /* terminal */
    {"terminal.default_shell", "ush", CONFIG_STRING,
     "shell started by the terminal: ush or qcsh"},
    {"terminal.colors", "1", CONFIG_BOOL, "enable coloured shell output"},
    {"terminal.scrollback", "500", CONFIG_INT, "scrollback buffer in lines"},

    /* system */
    {"log.level", "3", CONFIG_INT, "kernel log verbosity (0-5)"},
    {"log.serial", "1", CONFIG_BOOL, "mirror the kernel log to the serial port"},
    {"power.reboot_on_panic", "0", CONFIG_BOOL, "restart automatically on panic"},
    {"security.enforce_permissions", "1", CONFIG_BOOL,
     "enforce filesystem permission checks"},

    /* input */
    {"input.mouse_speed", "100", CONFIG_INT, "pointer speed percentage"},
    {"input.key_repeat_ms", "40", CONFIG_INT, "key repeat interval"},

    /* network */
    {"net.hostname", "qira", CONFIG_STRING, "hostname advertised on the network"},
    {"net.enabled", "1", CONFIG_BOOL, "bring network interfaces up at boot"},
};

static struct config_entry *find(const char *key)
{
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].key, key) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static struct config_entry *insert(const char *key, const char *value,
                                   config_type_t type, const char *description)
{
    struct config_entry *entry = find(key);

    if (!entry) {
        if (entry_count >= CONFIG_MAX_ENTRIES) {
            return NULL;
        }
        entry = &entries[entry_count++];
        strlcpy(entry->key, key, sizeof(entry->key));
        strlcpy(entry->description, description ? description : "",
                sizeof(entry->description));
        entry->type = type;
    }
    strlcpy(entry->value, value, sizeof(entry->value));
    return entry;
}

void config_reset_defaults(void)
{
    entry_count = 0;
    memset(entries, 0, sizeof(entries));

    for (size_t i = 0; i < ARRAY_SIZE(defaults); i++) {
        insert(defaults[i].key, defaults[i].value, defaults[i].type,
               defaults[i].description);
    }
}

void config_init(void)
{
    config_reset_defaults();

    if (config_load() == 0) {
        KLOG_INFO("config", "loaded %d settings from %s", entry_count, CONFIG_PATH);
    } else {
        KLOG_INFO("config", "using %d built-in defaults", entry_count);
        config_save();
    }
}

int config_load(void)
{
    char   buffer[8192];
    size_t got = 0;

    if (fs_read_file(CONFIG_PATH, buffer, sizeof(buffer) - 1, &got) != 0) {
        return -1;
    }
    buffer[got] = '\0';

    int applied = 0;
    char *save  = NULL;

    for (char *line = strtok_r(buffer, "\n", &save); line;
         line       = strtok_r(NULL, "\n", &save)) {
        /* Skip blank lines and comments. */
        while (isspace((uint8_t)*line)) {
            line++;
        }
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';

        char *key   = line;
        char *value = equals + 1;

        /* Trim whitespace from both. */
        char *key_end = key + strlen(key);
        while (key_end > key && isspace((uint8_t)key_end[-1])) {
            *--key_end = '\0';
        }
        while (isspace((uint8_t)*value)) {
            value++;
        }
        char *value_end = value + strlen(value);
        while (value_end > value && isspace((uint8_t)value_end[-1])) {
            *--value_end = '\0';
        }

        struct config_entry *entry = find(key);
        if (entry) {
            strlcpy(entry->value, value, sizeof(entry->value));
        } else {
            insert(key, value, CONFIG_STRING, "user defined");
        }
        applied++;
    }

    return (applied > 0) ? 0 : -1;
}

int config_save(void)
{
    char   buffer[8192];
    size_t pos = 0;

    pos += (size_t)snprintf(buffer + pos, sizeof(buffer) - pos,
                            "# Qira OS system configuration\n"
                            "# Written by the configuration service; edit with\n"
                            "# 'qcsh config set <key> <value>' or by hand.\n\n");

    for (int i = 0; i < entry_count && pos < sizeof(buffer) - 256; i++) {
        if (entries[i].description[0]) {
            pos += (size_t)snprintf(buffer + pos, sizeof(buffer) - pos, "# %s\n",
                                    entries[i].description);
        }
        pos += (size_t)snprintf(buffer + pos, sizeof(buffer) - pos, "%s = %s\n\n",
                                entries[i].key, entries[i].value);
    }

    return fs_write_file(CONFIG_PATH, buffer, pos);
}

const char *config_get_string(const char *key, const char *fallback)
{
    struct config_entry *entry = find(key);
    return entry ? entry->value : fallback;
}

int config_get_int(const char *key, int fallback)
{
    struct config_entry *entry = find(key);
    return entry ? atoi(entry->value) : fallback;
}

bool_t config_get_bool(const char *key, bool_t fallback)
{
    struct config_entry *entry = find(key);
    if (!entry) {
        return fallback;
    }
    return entry->value[0] == '1' || strcasecmp(entry->value, "true") == 0 ||
           strcasecmp(entry->value, "yes") == 0 ||
           strcasecmp(entry->value, "on") == 0;
}

int config_set_string(const char *key, const char *value)
{
    struct config_entry *entry = find(key);

    if (!entry) {
        entry = insert(key, value, CONFIG_STRING, "user defined");
        return entry ? 0 : -1;
    }
    strlcpy(entry->value, value, sizeof(entry->value));
    entry->dirty = true;
    return 0;
}

int config_set_int(const char *key, int value)
{
    char text[24];
    snprintf(text, sizeof(text), "%d", value);
    return config_set_string(key, text);
}

int config_set_bool(const char *key, bool_t value)
{
    return config_set_string(key, value ? "1" : "0");
}

int config_count(void)
{
    return entry_count;
}

const struct config_entry *config_at(int index)
{
    if (index < 0 || index >= entry_count) {
        return NULL;
    }
    return &entries[index];
}

void config_list(struct shell *sh)
{
    char last_group[CONFIG_KEY_MAX] = "";

    for (int i = 0; i < entry_count; i++) {
        const struct config_entry *entry = &entries[i];

        /* Group by the prefix before the first dot. */
        char group[CONFIG_KEY_MAX];
        const char *dot = strchr(entry->key, '.');
        if (dot) {
            size_t len = MIN((size_t)(dot - entry->key), sizeof(group) - 1);
            memcpy(group, entry->key, len);
            group[len] = '\0';
        } else {
            strlcpy(group, "general", sizeof(group));
        }

        if (strcmp(group, last_group) != 0) {
            shell_printf(sh, "\n  [%s]\n", group);
            strlcpy(last_group, group, sizeof(last_group));
        }

        shell_printf(sh, "    %-32s %s\n", entry->key, entry->value);
    }
    shell_printf(sh, "\n  %d settings. Use 'config set <key> <value>' to change one.\n",
                 entry_count);
}
