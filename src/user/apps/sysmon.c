/*
 * Qira OS - System Monitor
 *
 * Live view of tasks, memory and interrupt activity, with a scrolling CPU
 * history graph. Tasks can be selected and terminated.
 */

#include <kernel/desktop.h>
#include <kernel/sched.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/time.h>
#include <kernel/irq.h>
#include <kernel/input.h>

#define HISTORY_POINTS 120
#define ROW_HEIGHT     18

struct sysmon_state {
    struct task tasks[MAX_TASKS];
    int         task_count;
    int         selected;
    int         scroll;
    int         tab;          /* 0 tasks, 1 memory, 2 interrupts */

    /* Rolling history for the graphs. */
    uint8_t cpu_history[HISTORY_POINTS];
    uint8_t memory_history[HISTORY_POINTS];
    int     history_index;

    uint64_t last_idle_ticks;
    uint64_t last_total_ticks;
    uint64_t last_sample_ms;
    int      cpu_percent;
};

static void sysmon_sample(struct sysmon_state *state)
{
    state->task_count = sched_snapshot(state->tasks, MAX_TASKS);

    /* CPU load, estimated from how much time the idle task consumed. */
    uint64_t idle  = sched_idle_ticks();
    uint64_t total = time_ticks();

    uint64_t idle_delta  = idle - state->last_idle_ticks;
    uint64_t total_delta = total - state->last_total_ticks;

    if (total_delta > 0) {
        uint64_t busy = (idle_delta < total_delta) ? total_delta - idle_delta : 0;
        state->cpu_percent = (int)((busy * 100) / total_delta);
        state->cpu_percent = CLAMP(state->cpu_percent, 0, 100);
    }
    state->last_idle_ticks  = idle;
    state->last_total_ticks = total;

    uint64_t used  = pmm_used_bytes();
    uint64_t whole = pmm_total_bytes();
    int memory_percent = whole ? (int)((used * 100) / whole) : 0;

    state->cpu_history[state->history_index]    = (uint8_t)state->cpu_percent;
    state->memory_history[state->history_index] = (uint8_t)memory_percent;
    state->history_index = (state->history_index + 1) % HISTORY_POINTS;
}

static void sysmon_on_open(struct window *win)
{
    struct sysmon_state *state = (struct sysmon_state *)win->app_state;
    sysmon_sample(state);
    window_set_title(win, "System Monitor");
}

/* Draw a filled area graph of a percentage history. */
static void draw_graph(int x, int y, int w, int h, const uint8_t *history,
                       int index, color_t line, color_t fill, const char *label,
                       int current)
{
    const struct theme *theme = desktop_theme();

    fb_fill_rect(x, y, w, h, theme->input_bg);
    fb_draw_rect(x, y, w, h, theme->border);

    /* Horizontal guides at 25%, 50% and 75%. */
    for (int i = 1; i < 4; i++) {
        int gy = y + h - (h * i) / 4;
        for (int gx = x + 1; gx < x + w - 1; gx += 4) {
            fb_put_pixel(gx, gy, theme->border);
        }
    }

    int points = MIN(w - 2, HISTORY_POINTS);
    for (int i = 0; i < points; i++) {
        int sample = history[(index + HISTORY_POINTS - points + i) % HISTORY_POINTS];
        int bar_h  = (sample * (h - 2)) / 100;
        int bx     = x + 1 + i * (w - 2) / points;
        int bw     = MAX((w - 2) / points, 1);

        if (bar_h > 0) {
            fb_fill_rect(bx, y + h - 1 - bar_h, bw, bar_h, fill);
            fb_fill_rect(bx, y + h - 1 - bar_h, bw, 1, line);
        }
    }

    char text[48];
    snprintf(text, sizeof(text), "%s %d%%", label, current);
    fb_draw_string(x + 6, y + 4, text, line);
}

static void sysmon_draw(struct window *win, int x, int y, int w, int h)
{
    struct sysmon_state *state = (struct sysmon_state *)win->app_state;
    const struct theme  *theme = desktop_theme();

    /* Tab strip. */
    static const char *tabs[] = {"Tasks", "Memory", "Interrupts"};
    int tab_width = 110;

    fb_fill_rect(x, y, w, 26, theme->titlebar_inactive);
    for (int i = 0; i < 3; i++) {
        int tx = x + i * tab_width;
        if (state->tab == i) {
            fb_fill_rect(tx, y, tab_width, 26, theme->window_bg);
            fb_fill_rect(tx, y, tab_width, 2, theme->accent);
        }
        fb_draw_string(tx + 16, y + 5, tabs[i],
                       state->tab == i ? theme->accent
                                       : theme->titlebar_text_inactive);
    }
    fb_fill_rect(x, y + 25, w, 1, theme->border);

    int content_y = y + 30;

    if (state->tab == 0) {
        /* Summary graphs. */
        int graph_h = 62;
        int graph_w = (w - 24) / 2;
        draw_graph(x + 8, content_y, graph_w, graph_h, state->cpu_history,
                   state->history_index, RGB(120, 200, 255), RGB(40, 80, 130),
                   "CPU", state->cpu_percent);

        uint64_t used  = pmm_used_bytes();
        uint64_t whole = pmm_total_bytes();
        int memory_percent = whole ? (int)((used * 100) / whole) : 0;
        draw_graph(x + 16 + graph_w, content_y, graph_w, graph_h,
                   state->memory_history, state->history_index, RGB(160, 240, 160),
                   RGB(50, 110, 60), "Memory", memory_percent);

        int list_y = content_y + graph_h + 10;

        /* Headings. */
        fb_fill_rect(x, list_y, w, 18, theme->titlebar_inactive);
        fb_draw_string(x + 8, list_y + 1, "PID", theme->titlebar_text_inactive);
        fb_draw_string(x + 56, list_y + 1, "NAME", theme->titlebar_text_inactive);
        fb_draw_string(x + 210, list_y + 1, "STATE", theme->titlebar_text_inactive);
        fb_draw_string(x + 300, list_y + 1, "PRIORITY",
                       theme->titlebar_text_inactive);
        fb_draw_string(x + 400, list_y + 1, "TICKS", theme->titlebar_text_inactive);
        list_y += 20;

        int visible = (h - (list_y - y) - 22) / ROW_HEIGHT;
        if (state->selected < state->scroll) {
            state->scroll = state->selected;
        }
        if (state->selected >= state->scroll + visible) {
            state->scroll = state->selected - visible + 1;
        }

        for (int row = 0; row < visible; row++) {
            int index = state->scroll + row;
            if (index >= state->task_count) {
                break;
            }

            struct task *task = &state->tasks[index];
            int ry = list_y + row * ROW_HEIGHT;

            if (index == state->selected) {
                fb_fill_rect(x + 2, ry, w - 4, ROW_HEIGHT, theme->accent);
            }
            color_t text = (index == state->selected) ? RGB(255, 255, 255)
                                                      : theme->window_text;

            char field[32];
            snprintf(field, sizeof(field), "%d", task->pid);
            fb_draw_string(x + 8, ry + 1, field, text);
            fb_draw_string_clipped(x + 56, ry + 1, task->name, text, 148);
            fb_draw_string(x + 210, ry + 1, sched_state_name(task->state), text);
            fb_draw_string(x + 300, ry + 1, sched_priority_name(task->priority),
                           text);
            snprintf(field, sizeof(field), "%llu",
                     (unsigned long long)task->ticks_total);
            fb_draw_string(x + 400, ry + 1, field, text);
        }
    } else if (state->tab == 1) {
        struct heap_stats heap;
        heap_get_stats(&heap);

        int line = 0;
        #define ROW(label, fmt, ...)                                            \
            do {                                                                \
                char value[64];                                                 \
                snprintf(value, sizeof(value), fmt, __VA_ARGS__);               \
                fb_draw_string(x + 16, content_y + line * 20, label,            \
                               theme->titlebar_text_inactive);                  \
                fb_draw_string(x + 230, content_y + line * 20, value,           \
                               theme->window_text);                             \
                line++;                                                         \
            } while (0)

        ROW("Physical memory total", "%llu MiB",
            (unsigned long long)(pmm_total_bytes() / (1024 * 1024)));
        ROW("Physical memory used", "%llu MiB",
            (unsigned long long)(pmm_used_bytes() / (1024 * 1024)));
        ROW("Physical memory free", "%llu MiB",
            (unsigned long long)(pmm_free_bytes() / (1024 * 1024)));
        ROW("Reserved by firmware", "%llu MiB",
            (unsigned long long)(pmm_reserved_bytes() / (1024 * 1024)));
        line++;
        ROW("Kernel heap committed", "%llu KiB",
            (unsigned long long)(heap.total_bytes / 1024));
        ROW("Kernel heap in use", "%llu KiB",
            (unsigned long long)(heap.used_bytes / 1024));
        ROW("Kernel heap free", "%llu KiB",
            (unsigned long long)(heap.free_bytes / 1024));
        ROW("Largest free block", "%llu KiB",
            (unsigned long long)(heap.largest_free / 1024));
        ROW("Heap blocks", "%llu", (unsigned long long)heap.block_count);
        ROW("Allocations", "%llu", (unsigned long long)heap.allocations);
        ROW("Frees", "%llu", (unsigned long long)heap.frees);
        #undef ROW

        /* Memory usage bar. */
        int bar_y = content_y + line * 20 + 14;
        int used_percent =
            pmm_total_bytes()
                ? (int)((pmm_used_bytes() * 100) / pmm_total_bytes())
                : 0;
        fb_draw_string(x + 16, bar_y, "Utilisation",
                       theme->titlebar_text_inactive);
        fb_fill_round_rect(x + 16, bar_y + 20, w - 40, 14, 4, theme->input_bg);
        fb_fill_round_rect(x + 16, bar_y + 20, (w - 40) * used_percent / 100, 14, 4,
                           used_percent > 85 ? RGB(230, 110, 100)
                                             : theme->accent);
        char percent[16];
        snprintf(percent, sizeof(percent), "%d%%", used_percent);
        fb_draw_string(x + 16 + (w - 40) / 2 - 12, bar_y + 20, percent,
                       RGB(255, 255, 255));
    } else {
        fb_draw_string(x + 16, content_y, "IRQ", theme->titlebar_text_inactive);
        fb_draw_string(x + 70, content_y, "COUNT", theme->titlebar_text_inactive);
        fb_draw_string(x + 200, content_y, "HANDLER",
                       theme->titlebar_text_inactive);

        int line = 1;
        for (uint8_t irq = 0; irq < IRQ_COUNT; irq++) {
            uint64_t count = irq_get_count(irq);
            const char *name = irq_get_name(irq);
            if (count == 0 && strcmp(name, "-") == 0) {
                continue;
            }

            int ry = content_y + line * 18;
            char field[32];
            snprintf(field, sizeof(field), "%u", irq);
            fb_draw_string(x + 16, ry, field, theme->accent);
            snprintf(field, sizeof(field), "%llu", (unsigned long long)count);
            fb_draw_string(x + 70, ry, field, theme->window_text);
            fb_draw_string(x + 200, ry, name, theme->window_text);
            line++;
        }

        char total[64];
        snprintf(total, sizeof(total), "Total interrupts: %llu",
                 (unsigned long long)interrupt_total_count());
        fb_draw_string(x + 16, content_y + (line + 1) * 18, total, theme->accent);
    }

    /* Status bar. */
    int status_y = y + h - 20;
    fb_fill_rect(x, status_y, w, 20, theme->titlebar_inactive);
    fb_fill_rect(x, status_y, w, 1, theme->border);

    char status[128];
    uint64_t seconds = time_uptime_ms() / 1000;
    snprintf(status, sizeof(status),
             "%d tasks   up %llu:%02llu:%02llu   %llu switches", state->task_count,
             (unsigned long long)(seconds / 3600),
             (unsigned long long)((seconds % 3600) / 60),
             (unsigned long long)(seconds % 60),
             (unsigned long long)sched_context_switches());
    fb_draw_string(x + 8, status_y + 2, status, theme->titlebar_text_inactive);
    fb_draw_string(x + w - 180, status_y + 2, "Tab switch  Del end task",
                   theme->titlebar_text_inactive);
}

static bool_t sysmon_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct sysmon_state *state = (struct sysmon_state *)win->app_state;
    UNUSED(modifiers);

    switch (key) {
    case KEY_TAB:
        state->tab = (state->tab + 1) % 3;
        return true;
    case KEY_UP:
        state->selected = MAX(state->selected - 1, 0);
        return true;
    case KEY_DOWN:
        state->selected = MIN(state->selected + 1, MAX(state->task_count - 1, 0));
        return true;
    case KEY_DELETE:
        if (state->tab == 0 && state->selected < state->task_count) {
            struct task *task = &state->tasks[state->selected];
            if (task->pid > 1) {
                sched_kill(task->pid, 9);
                desktop_notify("System Monitor", "Task terminated");
                sysmon_sample(state);
            } else {
                desktop_notify("System Monitor", "Cannot terminate that task");
            }
        }
        return true;
    default:
        break;
    }
    return false;
}

static bool_t sysmon_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                              input_event_type_t type)
{
    struct sysmon_state *state = (struct sysmon_state *)win->app_state;

    if (type != INPUT_MOUSE_DOWN || !(buttons & MOUSE_LEFT)) {
        return false;
    }

    if (y < 26) {
        int tab = x / 110;
        if (tab >= 0 && tab < 3) {
            state->tab = tab;
            return true;
        }
        return false;
    }

    if (state->tab == 0) {
        int list_top = 30 + 62 + 10 + 20;
        if (y >= list_top) {
            int index = state->scroll + (y - list_top) / ROW_HEIGHT;
            if (index < state->task_count) {
                state->selected = index;
                return true;
            }
        }
    }
    return false;
}

static bool_t sysmon_tick(struct window *win)
{
    struct sysmon_state *state = (struct sysmon_state *)win->app_state;
    uint64_t now = time_uptime_ms();

    if (now - state->last_sample_ms >= 500) {
        state->last_sample_ms = now;
        sysmon_sample(state);
        return true;
    }
    return false;
}

static const struct app_ops sysmon_ops = {
    .draw     = sysmon_draw,
    .on_key   = sysmon_on_key,
    .on_mouse = sysmon_on_mouse,
    .tick     = sysmon_tick,
    .on_open  = sysmon_on_open,
};

void app_sysmon_register(void)
{
    desktop_register_app("System Monitor", "##", &sysmon_ops, 620, 480,
                         RGB(255, 150, 120), sizeof(struct sysmon_state), true);
}
