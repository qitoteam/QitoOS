/*
 * Qira OS - Web Browser
 *
 * Fetches pages over HTTP and renders them. The renderer handles the subset
 * of HTML that carries meaning in a text document: headings, paragraphs,
 * lists, links, preformatted blocks, emphasis and rules. It is not a layout
 * engine — there is no CSS, no tables and no images — but it makes real pages
 * readable, and links are navigable.
 *
 * Qira has no TLS, so https:// is refused with an explanation rather than a
 * confusing failure.
 */

#include <kernel/desktop.h>
#include <kernel/http.h>
#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/font.h>

#define BROWSER_MAX_LINES 800
#define BROWSER_LINE_MAX  180
#define BROWSER_MAX_LINKS 64
#define BROWSER_HISTORY   16

typedef enum {
    LINE_TEXT = 0,
    LINE_HEADING,
    LINE_SUBHEADING,
    LINE_LIST,
    LINE_PRE,
    LINE_RULE,
    LINE_LINK,
    LINE_BLANK,
} line_kind_t;

struct rendered_line {
    char        text[BROWSER_LINE_MAX];
    line_kind_t kind;
    int         link_index;   /* -1 when the line is not a link */
};

struct browser_state {
    char url[256];
    char address_input[256];
    bool_t editing_address;
    int  address_cursor;

    struct rendered_line lines[BROWSER_MAX_LINES];
    int    line_count;
    int    scroll;

    char   links[BROWSER_MAX_LINKS][224];
    int    link_count;
    int    selected_link;

    char   history[BROWSER_HISTORY][256];
    int    history_count;
    int    history_position;

    char   status[160];
    char   title[96];
    bool_t loading;
    bool_t load_requested;
    char   pending_url[256];
};

/* --- HTML rendering ---------------------------------------------------- */

/* Decode the handful of entities that appear in ordinary prose. */
static void decode_entities(char *text)
{
    static const struct {
        const char *entity;
        const char *replacement;
    } table[] = {
        {"&amp;", "&"},   {"&lt;", "<"},    {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"},   {"&apos;", "'"},
        {"&nbsp;", " "},  {"&mdash;", "-"}, {"&ndash;", "-"},
        {"&hellip;", "..."},
    };

    for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
        size_t entity_len = strlen(table[i].entity);
        size_t replace_len = strlen(table[i].replacement);

        char *found;
        while ((found = strstr(text, table[i].entity)) != NULL) {
            memmove(found + replace_len, found + entity_len,
                    strlen(found + entity_len) + 1);
            memcpy(found, table[i].replacement, replace_len);
        }
    }
}

static void add_line(struct browser_state *state, const char *text,
                     line_kind_t kind, int link_index)
{
    if (state->line_count >= BROWSER_MAX_LINES) {
        return;
    }

    struct rendered_line *line = &state->lines[state->line_count++];
    strlcpy(line->text, text ? text : "", sizeof(line->text));
    line->kind       = kind;
    line->link_index = link_index;
}

/* Emit a paragraph of text, wrapped to the available width. */
static void emit_wrapped(struct browser_state *state, const char *text,
                         line_kind_t kind, int link_index, int columns)
{
    char buffer[BROWSER_LINE_MAX];
    int  length = 0;

    /* Collapse runs of whitespace as HTML does. */
    bool_t pending_space = false;
    bool_t wrote_any     = false;

    for (const char *p = text; *p; p++) {
        char c = *p;

        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (length > 0) {
                pending_space = true;
            }
            continue;
        }

        if (pending_space && length > 0) {
            /* Break the line if this word will not fit. */
            int word_len = 0;
            const char *scan = p;
            while (*scan && *scan != ' ' && *scan != '\n' && *scan != '\t') {
                word_len++;
                scan++;
            }

            if (length + 1 + word_len > columns) {
                buffer[length] = '\0';
                add_line(state, buffer, kind, link_index);
                wrote_any = true;
                length    = 0;
            } else {
                buffer[length++] = ' ';
            }
            pending_space = false;
        }

        if (length < (int)sizeof(buffer) - 1) {
            buffer[length++] = c;
        }
    }

    if (length > 0) {
        buffer[length] = '\0';
        add_line(state, buffer, kind, link_index);
        wrote_any = true;
    }

    if (!wrote_any && !link_index) {
        /* Nothing to show; leave the document unchanged. */
    }
}

/* Read a tag's attribute value, e.g. href="...". */
static bool_t tag_attribute(const char *tag, const char *name, char *out,
                            size_t size)
{
    size_t name_len = strlen(name);
    const char *cursor = tag;

    while ((cursor = strstr(cursor, name)) != NULL) {
        /* The attribute must be preceded by whitespace. */
        if (cursor != tag && cursor[-1] != ' ' && cursor[-1] != '\t') {
            cursor += name_len;
            continue;
        }

        const char *value = cursor + name_len;
        while (*value == ' ' || *value == '=') {
            value++;
        }

        char quote = 0;
        if (*value == '"' || *value == '\'') {
            quote = *value++;
        }

        size_t index = 0;
        while (*value && index < size - 1) {
            if (quote && *value == quote) {
                break;
            }
            if (!quote && (*value == ' ' || *value == '>')) {
                break;
            }
            out[index++] = *value++;
        }
        out[index] = '\0';
        return index > 0;
    }
    return false;
}

/* Resolve a possibly relative href against the page's URL. */
static void resolve_link(const char *base, const char *href, char *out,
                         size_t size)
{
    if (strstr(href, "://") != NULL) {
        strlcpy(out, href, size);
        return;
    }

    struct http_url parsed;
    if (http_parse_url(base, &parsed) != 0) {
        strlcpy(out, href, size);
        return;
    }

    if (href[0] == '/') {
        snprintf(out, size, "http://%s%s", parsed.host, href);
        return;
    }

    /* Relative to the current directory. */
    char directory[256];
    strlcpy(directory, parsed.path, sizeof(directory));
    char *last_slash = strrchr(directory, '/');
    if (last_slash) {
        last_slash[1] = '\0';
    } else {
        strlcpy(directory, "/", sizeof(directory));
    }

    snprintf(out, size, "http://%s%s%s", parsed.host, directory, href);
}

static void render_html(struct browser_state *state, const char *html,
                        int columns)
{
    state->line_count = 0;
    state->link_count = 0;
    state->title[0]   = '\0';

    char text[BROWSER_LINE_MAX * 4];
    int  text_length = 0;

    bool_t in_script  = false;
    bool_t in_style   = false;
    bool_t in_pre     = false;
    bool_t in_title   = false;
    int    pending_link = -1;
    line_kind_t pending_kind = LINE_TEXT;

    const char *cursor = html;

    #define FLUSH()                                                          \
        do {                                                                 \
            if (text_length > 0) {                                           \
                text[text_length] = '\0';                                    \
                decode_entities(text);                                       \
                emit_wrapped(state, text, pending_kind, pending_link,        \
                             columns);                                       \
                text_length = 0;                                             \
            }                                                                \
        } while (0)

    while (*cursor) {
        if (*cursor == '<') {
            /* Read the whole tag. */
            char tag[512];
            int  tag_length = 0;
            cursor++;

            while (*cursor && *cursor != '>' && tag_length < (int)sizeof(tag) - 1) {
                tag[tag_length++] = *cursor++;
            }
            tag[tag_length] = '\0';
            if (*cursor == '>') {
                cursor++;
            }

            /* Lowercase the tag name for comparison. */
            char name[24];
            int  name_length = 0;
            const char *scan = tag;
            if (*scan == '/') {
                name[name_length++] = '/';
                scan++;
            }
            while (*scan && *scan != ' ' && *scan != '/' &&
                   name_length < (int)sizeof(name) - 1) {
                name[name_length++] = (char)tolower((uint8_t)*scan);
                scan++;
            }
            name[name_length] = '\0';

            /* Skip comments and declarations. */
            if (tag[0] == '!') {
                continue;
            }

            if (strcmp(name, "script") == 0) {
                in_script = true;
                continue;
            }
            if (strcmp(name, "/script") == 0) {
                in_script = false;
                continue;
            }
            if (strcmp(name, "style") == 0) {
                in_style = true;
                continue;
            }
            if (strcmp(name, "/style") == 0) {
                in_style = false;
                continue;
            }
            if (strcmp(name, "title") == 0) {
                in_title = true;
                FLUSH();
                continue;
            }
            if (strcmp(name, "/title") == 0) {
                in_title = false;
                text[text_length] = '\0';
                decode_entities(text);
                strlcpy(state->title, text, sizeof(state->title));
                text_length = 0;
                continue;
            }

            if (in_script || in_style) {
                continue;
            }

            /* Block-level tags end the current run of text. */
            if (strcmp(name, "p") == 0 || strcmp(name, "/p") == 0 ||
                strcmp(name, "div") == 0 || strcmp(name, "/div") == 0 ||
                strcmp(name, "br") == 0 || strcmp(name, "/li") == 0 ||
                strcmp(name, "section") == 0 || strcmp(name, "article") == 0 ||
                strcmp(name, "header") == 0 || strcmp(name, "footer") == 0 ||
                strcmp(name, "nav") == 0 || strcmp(name, "/tr") == 0) {
                FLUSH();
                pending_kind = LINE_TEXT;
                if (strcmp(name, "p") == 0 || strcmp(name, "/p") == 0) {
                    add_line(state, "", LINE_BLANK, -1);
                }
                continue;
            }

            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' &&
                name[2] == '\0') {
                FLUSH();
                add_line(state, "", LINE_BLANK, -1);
                pending_kind = (name[1] <= '2') ? LINE_HEADING : LINE_SUBHEADING;
                continue;
            }
            if (name[0] == '/' && name[1] == 'h' && name[2] >= '1' &&
                name[2] <= '6') {
                FLUSH();
                pending_kind = LINE_TEXT;
                continue;
            }

            if (strcmp(name, "li") == 0) {
                FLUSH();
                pending_kind = LINE_LIST;
                continue;
            }
            if (strcmp(name, "hr") == 0) {
                FLUSH();
                add_line(state, "", LINE_RULE, -1);
                continue;
            }
            if (strcmp(name, "pre") == 0) {
                FLUSH();
                in_pre       = true;
                pending_kind = LINE_PRE;
                continue;
            }
            if (strcmp(name, "/pre") == 0) {
                FLUSH();
                in_pre       = false;
                pending_kind = LINE_TEXT;
                continue;
            }

            if (strcmp(name, "a") == 0) {
                FLUSH();
                char href[224];
                if (tag_attribute(tag, "href", href, sizeof(href)) &&
                    state->link_count < BROWSER_MAX_LINKS) {
                    /* Skip fragments and non-HTTP schemes. */
                    if (href[0] != '#' && strncmp(href, "javascript:", 11) != 0 &&
                        strncmp(href, "mailto:", 7) != 0) {
                        resolve_link(state->url, href,
                                     state->links[state->link_count],
                                     sizeof(state->links[0]));
                        pending_link = state->link_count++;
                        pending_kind = LINE_LINK;
                    }
                }
                continue;
            }
            if (strcmp(name, "/a") == 0) {
                FLUSH();
                pending_link = -1;
                pending_kind = LINE_TEXT;
                continue;
            }

            /* Every other tag is ignored, keeping its text content. */
            continue;
        }

        if (in_script || in_style) {
            cursor++;
            continue;
        }

        if (in_pre && *cursor == '\n') {
            FLUSH();
            cursor++;
            continue;
        }

        if (text_length < (int)sizeof(text) - 1) {
            text[text_length++] = *cursor;
        }
        cursor++;

        UNUSED(in_title);
    }

    FLUSH();
    #undef FLUSH

    if (state->line_count == 0) {
        add_line(state, "(the page had no readable content)", LINE_TEXT, -1);
    }
}

/* --- fetching ---------------------------------------------------------- */

static void browser_load(struct browser_state *state, const char *url,
                         int columns)
{
    char target[256];

    /* Assume http:// when no scheme is given. */
    if (strstr(url, "://") == NULL) {
        snprintf(target, sizeof(target), "http://%s", url);
    } else {
        strlcpy(target, url, sizeof(target));
    }

    state->loading = true;
    snprintf(state->status, sizeof(state->status), "Loading %s...", target);

    struct http_response response;
    int result = http_get(target, &response);

    state->loading = false;

    if (result != 0) {
        state->line_count = 0;
        state->link_count = 0;
        add_line(state, "Could not load the page", LINE_HEADING, -1);
        add_line(state, "", LINE_BLANK, -1);
        add_line(state, response.error, LINE_TEXT, -1);
        add_line(state, "", LINE_BLANK, -1);

        if (strstr(response.error, "https")) {
            add_line(state, "Qira OS has no TLS implementation, so only",
                     LINE_TEXT, -1);
            add_line(state, "http:// addresses can be opened.", LINE_TEXT, -1);
        } else {
            add_line(state, "Check that networking is up with 'qcsh netinfo',",
                     LINE_TEXT, -1);
            add_line(state, "and that the address is reachable.", LINE_TEXT, -1);
        }

        snprintf(state->status, sizeof(state->status), "Failed: %s",
                 response.error);
        strlcpy(state->url, target, sizeof(state->url));
        state->scroll = 0;
        return;
    }

    strlcpy(state->url, target, sizeof(state->url));
    render_html(state, response.body, columns);

    snprintf(state->status, sizeof(state->status),
             "%d %s   %llu bytes   %d links", response.status,
             http_status_text(response.status),
             (unsigned long long)response.body_length, state->link_count);

    /* Record the visit for the back button. */
    if (state->history_count < BROWSER_HISTORY) {
        strlcpy(state->history[state->history_count++], target,
                sizeof(state->history[0]));
    } else {
        for (int i = 0; i < BROWSER_HISTORY - 1; i++) {
            strlcpy(state->history[i], state->history[i + 1],
                    sizeof(state->history[0]));
        }
        strlcpy(state->history[BROWSER_HISTORY - 1], target,
                sizeof(state->history[0]));
    }
    state->history_position = state->history_count - 1;

    state->scroll        = 0;
    state->selected_link = 0;

    http_free(&response);
}

/* --- the application --------------------------------------------------- */

static void browser_on_open(struct window *win)
{
    struct browser_state *state = (struct browser_state *)win->app_state;

    state->selected_link = -1;
    strlcpy(state->url, "about:start", sizeof(state->url));
    strlcpy(state->status, "Type an address and press Enter",
            sizeof(state->status));

    add_line(state, "Qira Browser", LINE_HEADING, -1);
    add_line(state, "", LINE_BLANK, -1);
    add_line(state, "Enter an http:// address in the bar above.", LINE_TEXT, -1);
    add_line(state, "", LINE_BLANK, -1);
    add_line(state, "What is supported", LINE_SUBHEADING, -1);
    add_line(state, "HTTP/1.1 with redirects and chunked transfer encoding.",
             LINE_LIST, -1);
    add_line(state, "Headings, paragraphs, lists, links and preformatted text.",
             LINE_LIST, -1);
    add_line(state, "DNS resolution, or a numeric address.", LINE_LIST, -1);
    add_line(state, "", LINE_BLANK, -1);
    add_line(state, "What is not", LINE_SUBHEADING, -1);
    add_line(state, "HTTPS: Qira OS has no TLS implementation.", LINE_LIST, -1);
    add_line(state, "CSS, JavaScript, images and tables.", LINE_LIST, -1);
    add_line(state, "", LINE_BLANK, -1);
    add_line(state, "Tab selects a link, Enter opens it, Backspace goes back.",
             LINE_TEXT, -1);

    window_set_title(win, "Browser");
}

static void browser_draw(struct window *win, int x, int y, int w, int h)
{
    struct browser_state *state = (struct browser_state *)win->app_state;
    const struct theme   *theme = desktop_theme();

    /* Address bar. */
    fb_fill_rect(x, y, w, 30, theme->titlebar_inactive);

    const char *back_label = "<";
    fb_fill_round_rect(x + 6, y + 5, 22, 20, 3,
                       state->history_count > 1 ? theme->window_bg
                                                : theme->titlebar_inactive);
    fb_draw_string(x + 14, y + 8, back_label,
                   state->history_count > 1 ? theme->window_text
                                            : theme->titlebar_text_inactive);

    int field_x = x + 34;
    int field_w = w - field_x + x - 10;

    fb_fill_rect(field_x, y + 5, field_w, 20,
                 state->editing_address ? theme->input_bg : theme->window_bg);
    fb_draw_rect(field_x, y + 5, field_w, 20,
                 state->editing_address ? theme->accent : theme->border);

    const char *shown = state->editing_address ? state->address_input : state->url;
    fb_draw_string_clipped(field_x + 6, y + 7, shown, theme->window_text,
                           field_w - 12);

    if (state->editing_address) {
        int cursor_x = field_x + 6 + fb_text_width(state->address_input);
        if (cursor_x < field_x + field_w - 4) {
            fb_fill_rect(cursor_x, y + 7, 2, FONT_HEIGHT, theme->accent);
        }
    }

    fb_fill_rect(x, y + 29, w, 1, theme->border);

    /* Page body. */
    int body_y  = y + 34;
    int body_h  = h - (body_y - y) - 20;
    int visible = body_h / (FONT_HEIGHT + 2);

    state->scroll = CLAMP(state->scroll, 0,
                          MAX(state->line_count - visible, 0));

    for (int row = 0; row < visible; row++) {
        int index = state->scroll + row;
        if (index >= state->line_count) {
            break;
        }

        struct rendered_line *line = &state->lines[index];
        int ry = body_y + row * (FONT_HEIGHT + 2);
        int tx = x + 10;

        switch (line->kind) {
        case LINE_HEADING:
            fb_draw_string_clipped(tx, ry, line->text, theme->accent, w - 20);
            break;

        case LINE_SUBHEADING:
            fb_draw_string_clipped(tx, ry, line->text,
                                   RGB(MIN(COLOR_R(theme->accent) + 40, 255),
                                       MIN(COLOR_G(theme->accent) + 40, 255),
                                       255),
                                   w - 20);
            break;

        case LINE_LIST:
            fb_draw_string(tx, ry, "-", theme->accent);
            fb_draw_string_clipped(tx + 16, ry, line->text, theme->window_text,
                                   w - 36);
            break;

        case LINE_RULE:
            fb_fill_rect(tx, ry + FONT_HEIGHT / 2, w - 20, 1, theme->border);
            break;

        case LINE_PRE:
            fb_fill_rect(tx - 4, ry - 1, w - 12, FONT_HEIGHT + 2,
                         theme->input_bg);
            fb_draw_string_clipped_in(font_terminal(), tx, ry, line->text,
                                      RGB(160, 220, 170), w - 24, 1);
            break;

        case LINE_LINK: {
            bool_t selected = (line->link_index == state->selected_link);
            color_t colour  = selected ? RGB(255, 255, 255) : RGB(120, 180, 255);

            if (selected) {
                fb_fill_rect(tx - 4, ry - 1, w - 12, FONT_HEIGHT + 2,
                             theme->accent);
            }
            fb_draw_string_clipped(tx, ry, line->text, colour, w - 20);

            /* Underline, as a link should be. */
            int underline = MIN(fb_text_width(line->text), w - 20);
            fb_fill_rect(tx, ry + FONT_HEIGHT, underline, 1, colour);
            break;
        }

        case LINE_BLANK:
            break;

        default:
            fb_draw_string_clipped(tx, ry, line->text, theme->window_text,
                                   w - 20);
            break;
        }
    }

    /* Scrollbar. */
    if (state->line_count > visible) {
        int track_h = body_h;
        int thumb_h = MAX(track_h * visible / state->line_count, 20);
        int thumb_y = body_y + (track_h - thumb_h) * state->scroll /
                                   MAX(state->line_count - visible, 1);
        fb_fill_rect(x + w - 6, body_y, 4, track_h, theme->input_bg);
        fb_fill_round_rect(x + w - 6, thumb_y, 4, thumb_h, 2, theme->accent);
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);
    fb_draw_string_clipped(x + 8, status_y + 1, state->status,
                           theme->titlebar_text_inactive, w - 16);
}

static bool_t browser_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct browser_state *state = (struct browser_state *)win->app_state;
    UNUSED(modifiers);

    struct rect client;
    window_client_rect(win, &client);
    int columns = MAX((client.w - 30) / FONT_WIDTH, 20);

    /* Address bar editing takes most keys. */
    if (state->editing_address) {
        if (key == KEY_ENTER || key == '\r') {
            state->editing_address = false;
            if (state->address_input[0]) {
                /* The fetch blocks, so tell the user before it starts. */
                strlcpy(state->pending_url, state->address_input,
                        sizeof(state->pending_url));
                state->load_requested = true;
                snprintf(state->status, sizeof(state->status), "Connecting to %s...",
                         state->address_input);
            }
            return true;
        }
        if (key == KEY_ESCAPE) {
            state->editing_address = false;
            return true;
        }
        if (key == KEY_BACKSPACE || key == 127) {
            size_t len = strlen(state->address_input);
            if (len > 0) {
                state->address_input[len - 1] = '\0';
            }
            return true;
        }
        if (key >= 32 && key < 127) {
            size_t len = strlen(state->address_input);
            if (len + 1 < sizeof(state->address_input)) {
                state->address_input[len]     = (char)key;
                state->address_input[len + 1] = '\0';
            }
            return true;
        }
        return true;
    }

    switch (key) {
    case 'l':
    case 'L':
    case KEY_F6:
        state->editing_address = true;
        strlcpy(state->address_input, state->url, sizeof(state->address_input));
        if (strcmp(state->address_input, "about:start") == 0) {
            state->address_input[0] = '\0';
        }
        return true;

    case KEY_TAB:
        if (state->link_count > 0) {
            state->selected_link = (state->selected_link + 1) % state->link_count;

            /* Scroll the selected link into view. */
            for (int i = 0; i < state->line_count; i++) {
                if (state->lines[i].link_index == state->selected_link) {
                    state->scroll = MAX(i - 4, 0);
                    break;
                }
            }
        }
        return true;

    case KEY_ENTER:
    case '\r':
        if (state->selected_link >= 0 &&
            state->selected_link < state->link_count) {
            strlcpy(state->pending_url, state->links[state->selected_link],
                    sizeof(state->pending_url));
            state->load_requested = true;
            snprintf(state->status, sizeof(state->status), "Loading %s...",
                     state->pending_url);
        }
        return true;

    case KEY_BACKSPACE:
        if (state->history_count > 1) {
            state->history_count--;
            strlcpy(state->pending_url,
                    state->history[state->history_count - 1],
                    sizeof(state->pending_url));
            state->history_count--;   /* the load re-adds it */
            state->load_requested = true;
        }
        return true;

    case 'r':
    case 'R':
    case KEY_F5:
        if (strcmp(state->url, "about:start") != 0) {
            strlcpy(state->pending_url, state->url, sizeof(state->pending_url));
            state->load_requested = true;
        }
        return true;

    case KEY_UP:
        state->scroll = MAX(state->scroll - 1, 0);
        return true;
    case KEY_DOWN:
        state->scroll++;
        return true;
    case KEY_PAGEUP:
        state->scroll = MAX(state->scroll - 15, 0);
        return true;
    case KEY_PAGEDOWN:
        state->scroll += 15;
        return true;
    case KEY_HOME:
        state->scroll = 0;
        return true;
    case KEY_END:
        state->scroll = state->line_count;
        return true;
    default:
        break;
    }

    UNUSED(columns);
    return false;
}

static bool_t browser_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                               input_event_type_t type)
{
    struct browser_state *state = (struct browser_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }

    /* The address field. */
    if (y >= 5 && y < 30) {
        if (x < 30) {
            if (state->history_count > 1) {
                state->history_count -= 2;
                strlcpy(state->pending_url, state->history[state->history_count],
                        sizeof(state->pending_url));
                state->load_requested = true;
            }
        } else {
            state->editing_address = true;
            strlcpy(state->address_input, state->url,
                    sizeof(state->address_input));
            if (strcmp(state->address_input, "about:start") == 0) {
                state->address_input[0] = '\0';
            }
        }
        return true;
    }

    /* Clicking a link line follows it. */
    int row = (y - 34) / (FONT_HEIGHT + 2);
    int index = state->scroll + row;

    if (index >= 0 && index < state->line_count) {
        int link = state->lines[index].link_index;
        if (link >= 0 && link < state->link_count) {
            state->selected_link = link;
            strlcpy(state->pending_url, state->links[link],
                    sizeof(state->pending_url));
            state->load_requested = true;
            return true;
        }
    }
    return false;
}

/*
 * Fetching blocks for as long as the network takes, so it happens on the tick
 * rather than inside the key handler. That lets the "Loading..." status paint
 * before the compositor stalls, which is the difference between the window
 * looking busy and looking hung.
 */
static bool_t browser_tick(struct window *win)
{
    struct browser_state *state = (struct browser_state *)win->app_state;

    if (!state->load_requested) {
        return false;
    }
    state->load_requested = false;

    struct rect client;
    window_client_rect(win, &client);
    int columns = MAX((client.w - 30) / FONT_WIDTH, 20);

    browser_load(state, state->pending_url, columns);

    if (state->title[0]) {
        char caption[WINDOW_TITLE_MAX];
        snprintf(caption, sizeof(caption), "%s - Browser", state->title);
        window_set_title(win, caption);
    }
    return true;
}

static const struct app_ops browser_ops = {
    .draw     = browser_draw,
    .on_key   = browser_on_key,
    .on_mouse = browser_on_mouse,
    .tick     = browser_tick,
    .on_open  = browser_on_open,
};

void app_browser_register(void)
{
    desktop_register_app("Browser", "()", &browser_ops, 780, 520,
                         RGB(90, 160, 240), sizeof(struct browser_state), true);
}
