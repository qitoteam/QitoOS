/*
 * Qira OS - Network
 *
 * Shows what the network stack is doing: interfaces and their counters, the
 * TCP socket table, and interactive tools for ping, DNS lookup and HTTP
 * fetches. Useful in its own right, and the fastest way to tell whether a
 * networking problem is the link, the resolver or the server.
 */

#include <kernel/desktop.h>
#include <kernel/net.h>
#include <kernel/http.h>
#include <kernel/git.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/font.h>

#define OUTPUT_MAX_LINES 300
#define OUTPUT_LINE_MAX  128

typedef enum {
    TAB_INTERFACES = 0,
    TAB_SOCKETS,
    TAB_TOOLS,
} network_tab_t;

typedef enum {
    ACTION_NONE = 0,
    ACTION_PING,
    ACTION_LOOKUP,
    ACTION_FETCH,
    ACTION_GIT,
} action_t;

struct network_state {
    network_tab_t tab;

    char   input[192];
    bool_t editing;
    int    tool_index;        /* which tool the input applies to */

    char   output[OUTPUT_MAX_LINES][OUTPUT_LINE_MAX];
    int    output_count;
    int    scroll;

    action_t pending;
    char     pending_argument[192];
    bool_t   busy;
};

static void output_clear(struct network_state *state)
{
    state->output_count = 0;
    state->scroll       = 0;
}

static void output_line(struct network_state *state, const char *fmt, ...)
{
    if (state->output_count >= OUTPUT_MAX_LINES) {
        /* Scroll the buffer rather than stopping. */
        for (int i = 0; i < OUTPUT_MAX_LINES - 1; i++) {
            strlcpy(state->output[i], state->output[i + 1], OUTPUT_LINE_MAX);
        }
        state->output_count = OUTPUT_MAX_LINES - 1;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->output[state->output_count], OUTPUT_LINE_MAX, fmt, ap);
    va_end(ap);

    state->output_count++;
}

static const char *tool_labels[] = {"ping", "lookup", "fetch", "git ls-remote"};

/* --- the actions ------------------------------------------------------- */

static void run_ping(struct network_state *state, const char *target)
{
    ipv4_addr_t address;

    output_line(state, "ping %s", target);

    if (dns_resolve(target, &address, 4000) != 0) {
        output_line(state, "  cannot resolve %s", target);
        return;
    }

    char text[24];
    net_format_ip(address, text, sizeof(text));
    output_line(state, "  resolved to %s", text);

    /*
     * net_ping writes through a shell sink, which this application does not
     * have, so the ICMP exchange is summarised from the interface counters
     * instead.
     */
    struct net_interface *iface = net_default_interface();
    if (!iface) {
        output_line(state, "  no usable interface");
        return;
    }

    uint64_t before = iface->stats.icmp_echo_replies;
    uint64_t start  = time_uptime_ms();

    net_ping_quiet(address, 4);

    uint64_t elapsed = time_uptime_ms() - start;
    uint64_t replies = iface->stats.icmp_echo_replies - before;

    output_line(state, "  %llu of 4 replies in %llu ms",
                (unsigned long long)replies, (unsigned long long)elapsed);
    output_line(state, "");
}

static void run_lookup(struct network_state *state, const char *host)
{
    output_line(state, "lookup %s", host);

    ipv4_addr_t server = net_dns_server();
    char server_text[24];
    net_format_ip(server, server_text, sizeof(server_text));
    output_line(state, "  using resolver %s", server_text);

    uint64_t start = time_uptime_ms();
    ipv4_addr_t address;

    if (dns_resolve(host, &address, 5000) == 0) {
        char text[24];
        net_format_ip(address, text, sizeof(text));
        output_line(state, "  %s has address %s", host, text);
        output_line(state, "  answered in %llu ms",
                    (unsigned long long)(time_uptime_ms() - start));
    } else {
        output_line(state, "  no answer from the resolver");
    }
    output_line(state, "");
}

static void run_fetch(struct network_state *state, const char *url)
{
    output_line(state, "fetch %s", url);

    struct http_response response;
    uint64_t start = time_uptime_ms();

    if (http_get(url, &response) != 0) {
        output_line(state, "  failed: %s", response.error);
        output_line(state, "");
        return;
    }

    output_line(state, "  %d %s in %llu ms", response.status,
                http_status_text(response.status),
                (unsigned long long)(time_uptime_ms() - start));
    output_line(state, "  content-type: %s",
                response.content_type[0] ? response.content_type : "(none)");
    output_line(state, "  %llu bytes received",
                (unsigned long long)response.body_length);
    output_line(state, "");

    /* Show the first few lines of the body. */
    const char *cursor = response.body;
    for (int i = 0; i < 12 && *cursor; i++) {
        char line[OUTPUT_LINE_MAX];
        int  length = 0;

        while (*cursor && *cursor != '\n' && length < (int)sizeof(line) - 1) {
            char c = *cursor++;
            line[length++] = (c >= 32 && c < 127) ? c : '.';
        }
        line[length] = '\0';
        if (*cursor == '\n') {
            cursor++;
        }
        output_line(state, "  | %s", line);
    }
    output_line(state, "");

    http_free(&response);
}

static void run_git(struct network_state *state, const char *url)
{
    output_line(state, "git ls-remote %s", url);

    static struct git_ref refs[GIT_MAX_REFS];
    char error[128] = "";

    int count = git_ls_remote(url, refs, GIT_MAX_REFS, error, sizeof(error));

    if (count < 0) {
        output_line(state, "  failed: %s", error);
        output_line(state, "");
        return;
    }

    output_line(state, "  %d reference(s)", count);
    for (int i = 0; i < count && i < 40; i++) {
        char hash[12];
        strlcpy(hash, refs[i].hash, sizeof(hash));
        output_line(state, "  %s  %s", hash, refs[i].name);
    }
    output_line(state, "");
}

/* --- the application --------------------------------------------------- */

static void network_on_open(struct window *win)
{
    struct network_state *state = (struct network_state *)win->app_state;

    output_line(state, "Qira network tools");
    output_line(state, "");
    output_line(state, "Pick a tool below, type a target and press Enter.");
    output_line(state, "");
    output_line(state, "Note: Qira has no TLS, so fetch and git work over");
    output_line(state, "http:// only.");
    output_line(state, "");

    strlcpy(state->input, "example.com", sizeof(state->input));
    window_set_title(win, "Network");
}

static void network_draw(struct window *win, int x, int y, int w, int h)
{
    struct network_state *state = (struct network_state *)win->app_state;
    const struct theme   *theme = desktop_theme();

    /* Tabs. */
    static const char *tabs[] = {"Interfaces", "Sockets", "Tools"};
    fb_fill_rect(x, y, w, 26, theme->titlebar_inactive);
    for (int i = 0; i < 3; i++) {
        int tx = x + i * 110;
        if (state->tab == (network_tab_t)i) {
            fb_fill_rect(tx, y, 110, 26, theme->window_bg);
            fb_fill_rect(tx, y, 110, 2, theme->accent);
        }
        fb_draw_string(tx + 16, y + 5, tabs[i],
                       state->tab == (network_tab_t)i
                           ? theme->accent
                           : theme->titlebar_text_inactive);
    }
    fb_fill_rect(x, y + 25, w, 1, theme->border);

    int body_y = y + 32;

    if (state->tab == TAB_INTERFACES) {
        int line = 0;

        for (int i = 0; i < net_interface_count(); i++) {
            struct net_interface *iface = net_interface_at(i);
            if (!iface) {
                continue;
            }

            char address[24], netmask[24], gateway[24];
            net_format_ip(iface->address, address, sizeof(address));
            net_format_ip(iface->netmask, netmask, sizeof(netmask));
            net_format_ip(iface->gateway, gateway, sizeof(gateway));

            char text[160];

            snprintf(text, sizeof(text), "%s", iface->name);
            fb_draw_string(x + 16, body_y + line * 18, text, theme->accent);

            snprintf(text, sizeof(text), "%s%s", iface->up ? "UP" : "DOWN",
                     iface->loopback ? ", LOOPBACK" : ", BROADCAST");
            fb_draw_string(x + 90, body_y + line * 18, text,
                           iface->up ? RGB(140, 220, 150) : RGB(220, 140, 140));
            line++;

            snprintf(text, sizeof(text), "inet %s  netmask %s", address, netmask);
            fb_draw_string(x + 32, body_y + line * 18, text, theme->window_text);
            line++;

            if (iface->gateway) {
                snprintf(text, sizeof(text), "gateway %s", gateway);
                fb_draw_string(x + 32, body_y + line * 18, text,
                               theme->window_text);
                line++;
            }

            if (!iface->loopback) {
                snprintf(text, sizeof(text),
                         "ether %02x:%02x:%02x:%02x:%02x:%02x", iface->mac[0],
                         iface->mac[1], iface->mac[2], iface->mac[3],
                         iface->mac[4], iface->mac[5]);
                fb_draw_string(x + 32, body_y + line * 18, text,
                               theme->titlebar_text_inactive);
                line++;
            }

            snprintf(text, sizeof(text),
                     "RX %llu packets, %llu bytes, %llu dropped",
                     (unsigned long long)iface->stats.rx_packets,
                     (unsigned long long)iface->stats.rx_bytes,
                     (unsigned long long)iface->stats.rx_dropped);
            fb_draw_string(x + 32, body_y + line * 18, text,
                           theme->titlebar_text_inactive);
            line++;

            snprintf(text, sizeof(text),
                     "TX %llu packets, %llu bytes, %llu errors",
                     (unsigned long long)iface->stats.tx_packets,
                     (unsigned long long)iface->stats.tx_bytes,
                     (unsigned long long)iface->stats.tx_errors);
            fb_draw_string(x + 32, body_y + line * 18, text,
                           theme->titlebar_text_inactive);
            line += 2;
        }

        char resolver[64];
        char server[24];
        net_format_ip(net_dns_server(), server, sizeof(server));
        snprintf(resolver, sizeof(resolver), "Resolver: %s", server);
        fb_draw_string(x + 16, body_y + line * 18, resolver, theme->accent);
        line++;

        fb_draw_string(x + 16, body_y + line * 18,
                       "Protocols: Ethernet, ARP, IPv4, ICMP, TCP, UDP, DNS",
                       theme->titlebar_text_inactive);
        line++;
        fb_draw_string(x + 16, body_y + line * 18,
                       "No TLS, so HTTPS is unavailable.",
                       theme->titlebar_text_inactive);

    } else if (state->tab == TAB_SOCKETS) {
        struct net_stats stats;
        tcp_get_stats(&stats);

        char text[160];
        int  line = 0;

        snprintf(text, sizeof(text), "Active TCP sockets: %d",
                 tcp_active_sockets());
        fb_draw_string(x + 16, body_y, text, theme->accent);
        line += 2;

        snprintf(text, sizeof(text), "Segments received  %llu",
                 (unsigned long long)stats.rx_packets);
        fb_draw_string(x + 16, body_y + line++ * 18, text, theme->window_text);

        snprintf(text, sizeof(text), "Segments sent      %llu",
                 (unsigned long long)stats.tx_packets);
        fb_draw_string(x + 16, body_y + line++ * 18, text, theme->window_text);

        snprintf(text, sizeof(text), "Bytes received     %llu",
                 (unsigned long long)stats.rx_bytes);
        fb_draw_string(x + 16, body_y + line++ * 18, text, theme->window_text);

        snprintf(text, sizeof(text), "Bytes sent         %llu",
                 (unsigned long long)stats.tx_bytes);
        fb_draw_string(x + 16, body_y + line++ * 18, text, theme->window_text);

        snprintf(text, sizeof(text), "Dropped            %llu",
                 (unsigned long long)stats.rx_dropped);
        fb_draw_string(x + 16, body_y + line++ * 18, text, theme->window_text);

        snprintf(text, sizeof(text), "Transmit errors    %llu",
                 (unsigned long long)stats.tx_errors);
        fb_draw_string(x + 16, body_y + line++ * 18, text, theme->window_text);

        line++;
        fb_draw_string(x + 16, body_y + line++ * 18,
                       "TCP implements connect, stream and close with",
                       theme->titlebar_text_inactive);
        fb_draw_string(x + 16, body_y + line++ * 18,
                       "retransmission. There is no congestion control",
                       theme->titlebar_text_inactive);
        fb_draw_string(x + 16, body_y + line++ * 18,
                       "beyond a fixed window.",
                       theme->titlebar_text_inactive);

    } else {
        /* Tool selector. */
        int tx = x + 12;
        for (int i = 0; i < (int)ARRAY_SIZE(tool_labels); i++) {
            int width = fb_text_width(tool_labels[i]) + 16;
            bool_t active = (state->tool_index == i);

            fb_fill_round_rect(tx, body_y, width, 22, 4,
                               active ? theme->accent : theme->input_bg);
            fb_draw_string(tx + 8, body_y + 3, tool_labels[i],
                           active ? RGB(255, 255, 255) : theme->window_text);
            tx += width + 6;
        }

        /* Input field. */
        int field_y = body_y + 32;
        fb_fill_rect(x + 12, field_y, w - 24, 22,
                     state->editing ? theme->input_bg : theme->window_bg);
        fb_draw_rect(x + 12, field_y, w - 24, 22,
                     state->editing ? theme->accent : theme->border);
        fb_draw_string_clipped(x + 18, field_y + 3, state->input,
                               theme->window_text, w - 36);

        if (state->editing) {
            int cursor_x = x + 18 + fb_text_width(state->input);
            fb_fill_rect(cursor_x, field_y + 3, 2, FONT_HEIGHT, theme->accent);
        }

        if (state->busy) {
            fb_draw_string(x + 12, field_y + 28, "Working...", theme->accent);
        }

        /* Output. */
        int out_y    = field_y + 48;
        int out_h    = h - (out_y - y) - 20;
        int visible  = out_h / FONT_HEIGHT;

        state->scroll = CLAMP(state->scroll, 0,
                              MAX(state->output_count - visible, 0));

        for (int row = 0; row < visible; row++) {
            int index = state->scroll + row;
            if (index >= state->output_count) {
                break;
            }

            const char *line = state->output[index];
            color_t colour = theme->window_text;

            if (strstr(line, "failed") || strstr(line, "cannot") ||
                strstr(line, "no answer")) {
                colour = RGB(240, 140, 130);
            } else if (line[0] != ' ' && line[0] != '\0') {
                colour = theme->accent;
            } else if (strstr(line, "  | ")) {
                colour = RGB(150, 200, 160);
            }

            fb_draw_string_clipped_in(font_terminal(), x + 12,
                                      out_y + row * FONT_HEIGHT, line, colour,
                                      w - 24, 1);
        }
    }

    /* Status bar. */
    int status_y = y + h - 18;
    fb_fill_rect(x, status_y, w, 18, theme->titlebar_inactive);
    fb_draw_string(x + 8, status_y + 1,
                   "Tab switches view   T cycles tool   Enter runs",
                   theme->titlebar_text_inactive);
}

static bool_t network_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct network_state *state = (struct network_state *)win->app_state;
    UNUSED(modifiers);

    if (state->editing) {
        if (key == KEY_ENTER || key == '\r') {
            state->editing = false;

            static const action_t actions[] = {ACTION_PING, ACTION_LOOKUP,
                                               ACTION_FETCH, ACTION_GIT};
            state->pending = actions[state->tool_index];
            strlcpy(state->pending_argument, state->input,
                    sizeof(state->pending_argument));
            state->busy = true;
            return true;
        }
        if (key == KEY_ESCAPE) {
            state->editing = false;
            return true;
        }
        if (key == KEY_BACKSPACE || key == 127) {
            size_t len = strlen(state->input);
            if (len > 0) {
                state->input[len - 1] = '\0';
            }
            return true;
        }
        if (key >= 32 && key < 127) {
            size_t len = strlen(state->input);
            if (len + 1 < sizeof(state->input)) {
                state->input[len]     = (char)key;
                state->input[len + 1] = '\0';
            }
            return true;
        }
        return true;
    }

    switch (key) {
    case KEY_TAB:
        state->tab = (network_tab_t)((state->tab + 1) % 3);
        return true;
    case 't':
    case 'T':
        state->tool_index =
            (state->tool_index + 1) % (int)ARRAY_SIZE(tool_labels);
        state->tab = TAB_TOOLS;
        return true;
    case 'e':
    case 'E':
    case KEY_ENTER:
        state->tab     = TAB_TOOLS;
        state->editing = true;
        return true;
    case 'c':
    case 'C':
        output_clear(state);
        return true;
    case KEY_UP:
        state->scroll = MAX(state->scroll - 1, 0);
        return true;
    case KEY_DOWN:
        state->scroll++;
        return true;
    case KEY_PAGEUP:
        state->scroll = MAX(state->scroll - 12, 0);
        return true;
    case KEY_PAGEDOWN:
        state->scroll += 12;
        return true;
    default:
        break;
    }
    return false;
}

static bool_t network_on_mouse(struct window *win, int x, int y,
                               uint32_t buttons, input_event_type_t type)
{
    struct network_state *state = (struct network_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }

    if (y < 26) {
        int tab = x / 110;
        if (tab >= 0 && tab < 3) {
            state->tab = (network_tab_t)tab;
            return true;
        }
        return false;
    }

    if (state->tab == TAB_TOOLS) {
        /* The tool buttons. */
        if (y >= 32 && y < 54) {
            int tx = 12;
            for (int i = 0; i < (int)ARRAY_SIZE(tool_labels); i++) {
                int width = fb_text_width(tool_labels[i]) + 16;
                if (x >= tx && x < tx + width) {
                    state->tool_index = i;
                    return true;
                }
                tx += width + 6;
            }
        }
        /* The input field. */
        if (y >= 64 && y < 86) {
            state->editing = true;
            return true;
        }
    }
    return false;
}

/*
 * Network operations block for as long as the network takes, so they run on
 * the tick rather than in the key handler. That lets the "Working..." state
 * paint first, instead of the window appearing frozen.
 */
static bool_t network_tick(struct window *win)
{
    struct network_state *state = (struct network_state *)win->app_state;

    if (state->pending == ACTION_NONE) {
        return false;
    }

    action_t action = state->pending;
    state->pending  = ACTION_NONE;

    switch (action) {
    case ACTION_PING:   run_ping(state, state->pending_argument);   break;
    case ACTION_LOOKUP: run_lookup(state, state->pending_argument); break;
    case ACTION_FETCH:  run_fetch(state, state->pending_argument);  break;
    case ACTION_GIT:    run_git(state, state->pending_argument);    break;
    default: break;
    }

    state->busy   = false;
    state->scroll = MAX(state->output_count - 18, 0);
    return true;
}

static const struct app_ops network_ops = {
    .draw     = network_draw,
    .on_key   = network_on_key,
    .on_mouse = network_on_mouse,
    .tick     = network_tick,
    .on_open  = network_on_open,
};

void app_network_register(void)
{
    desktop_register_app("Network", "<>", &network_ops, 720, 520,
                         RGB(110, 210, 220), sizeof(struct network_state), true);
}
