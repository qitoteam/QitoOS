/*
 * QitoOS - Calculator
 *
 * An integer calculator with a clickable keypad and keyboard entry. Shares the
 * expression evaluator design used by UltraShell's `calc` command.
 */

#include <kernel/desktop.h>
#include <kernel/string.h>
#include <kernel/printf.h>

#define EXPR_MAX 64

struct calc_state {
    char    expression[EXPR_MAX];
    char    display[EXPR_MAX];
    int64_t last_result;
    bool_t  has_error;
    bool_t  showing_result;
    int     hover_button;
};

/* --- expression evaluation -------------------------------------------- */

struct parser {
    const char *p;
    bool_t      error;
};

static int64_t parse_expression(struct parser *st);

static void skip_spaces(struct parser *st)
{
    while (*st->p == ' ') {
        st->p++;
    }
}

static int64_t parse_primary(struct parser *st)
{
    skip_spaces(st);

    if (*st->p == '(') {
        st->p++;
        int64_t value = parse_expression(st);
        skip_spaces(st);
        if (*st->p == ')') {
            st->p++;
        } else {
            st->error = true;
        }
        return value;
    }
    if (*st->p == '-') {
        st->p++;
        return -parse_primary(st);
    }
    if (!isdigit((uint8_t)*st->p)) {
        st->error = true;
        return 0;
    }

    int64_t value = 0;
    while (isdigit((uint8_t)*st->p)) {
        value = value * 10 + (*st->p - '0');
        st->p++;
    }
    return value;
}

static int64_t parse_term(struct parser *st)
{
    int64_t value = parse_primary(st);

    for (;;) {
        skip_spaces(st);
        char op = *st->p;
        if (op != '*' && op != '/' && op != '%') {
            return value;
        }
        st->p++;
        int64_t rhs = parse_primary(st);
        if ((op == '/' || op == '%') && rhs == 0) {
            st->error = true;
            return 0;
        }
        value = (op == '*') ? value * rhs : (op == '/') ? value / rhs : value % rhs;
    }
}

static int64_t parse_expression(struct parser *st)
{
    int64_t value = parse_term(st);

    for (;;) {
        skip_spaces(st);
        char op = *st->p;
        if (op != '+' && op != '-') {
            return value;
        }
        st->p++;
        int64_t rhs = parse_term(st);
        value = (op == '+') ? value + rhs : value - rhs;
    }
}

static void calc_evaluate(struct calc_state *state)
{
    if (state->expression[0] == '\0') {
        return;
    }

    struct parser st = {state->expression, false};
    int64_t       value = parse_expression(&st);

    skip_spaces(&st);
    if (st.error || *st.p) {
        strlcpy(state->display, "error", sizeof(state->display));
        state->has_error = true;
    } else {
        snprintf(state->display, sizeof(state->display), "%lld", (long long)value);
        state->last_result = value;
        state->has_error   = false;
    }
    state->showing_result = true;
}

/* --- keypad ----------------------------------------------------------- */

static const char *keypad[20] = {
    "7", "8", "9", "/", "C",
    "4", "5", "6", "*", "(",
    "1", "2", "3", "-", ")",
    "0", "%", "=", "+", "<",
};

static void calc_append(struct calc_state *state, char c)
{
    if (state->showing_result && (isdigit((uint8_t)c) || c == '(')) {
        /* Typing a digit after a result starts a fresh expression. */
        state->expression[0] = '\0';
        state->showing_result = false;
    } else if (state->showing_result) {
        /* An operator continues from the previous result. */
        snprintf(state->expression, sizeof(state->expression), "%lld",
                 (long long)state->last_result);
        state->showing_result = false;
    }

    size_t len = strlen(state->expression);
    if (len + 1 < sizeof(state->expression)) {
        state->expression[len]     = c;
        state->expression[len + 1] = '\0';
    }
    strlcpy(state->display, state->expression, sizeof(state->display));
    state->has_error = false;
}

static void calc_press(struct calc_state *state, const char *label)
{
    if (strcmp(label, "C") == 0) {
        state->expression[0]  = '\0';
        state->display[0]     = '0';
        state->display[1]     = '\0';
        state->has_error      = false;
        state->showing_result = false;
        return;
    }
    if (strcmp(label, "<") == 0) {
        size_t len = strlen(state->expression);
        if (len > 0) {
            state->expression[len - 1] = '\0';
        }
        strlcpy(state->display, state->expression[0] ? state->expression : "0",
                sizeof(state->display));
        return;
    }
    if (strcmp(label, "=") == 0) {
        calc_evaluate(state);
        return;
    }
    calc_append(state, label[0]);
}

static void calc_on_open(struct window *win)
{
    struct calc_state *state = (struct calc_state *)win->app_state;
    strlcpy(state->display, "0", sizeof(state->display));
    state->hover_button = -1;
    window_set_title(win, "Calculator");
}

static void calc_draw(struct window *win, int x, int y, int w, int h)
{
    struct calc_state  *state = (struct calc_state *)win->app_state;
    const struct theme *theme = desktop_theme();

    /* Display. */
    int display_h = 62;
    fb_fill_rect(x + 8, y + 8, w - 16, display_h, theme->input_bg);
    fb_draw_rect(x + 8, y + 8, w - 16, display_h, theme->border);

    /* The expression, small, above the result. */
    if (!state->showing_result && state->expression[0]) {
        fb_draw_string_clipped(x + 16, y + 14, state->expression,
                               theme->titlebar_text_inactive, w - 32);
    } else if (state->showing_result) {
        fb_draw_string_clipped(x + 16, y + 14, state->expression,
                               theme->titlebar_text_inactive, w - 32);
    }

    color_t value_color = state->has_error ? RGB(250, 120, 110) : theme->window_text;
    int value_w = fb_text_width_large(state->display);
    fb_draw_string_large(x + w - 16 - MIN(value_w, w - 40), y + 32, state->display,
                         value_color);

    /* Keypad. */
    int pad_y   = y + display_h + 16;
    int pad_h   = h - (pad_y - y) - 10;
    int cols    = 5;
    int rows    = 4;
    int cell_w  = (w - 16 - (cols - 1) * 6) / cols;
    int cell_h  = (pad_h - (rows - 1) * 6) / rows;

    for (int i = 0; i < 20; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx  = x + 8 + col * (cell_w + 6);
        int by  = pad_y + row * (cell_h + 6);

        const char *label = keypad[i];
        bool_t is_operator = !isdigit((uint8_t)label[0]);
        bool_t is_equals   = (strcmp(label, "=") == 0);
        bool_t hovered     = (state->hover_button == i);

        color_t face = is_equals ? theme->accent
                       : is_operator ? theme->titlebar_active
                                     : RGB(COLOR_R(theme->window_bg) + 14,
                                           COLOR_G(theme->window_bg) + 14,
                                           COLOR_B(theme->window_bg) + 18);
        if (hovered) {
            face = RGB(MIN(COLOR_R(face) + 30, 255), MIN(COLOR_G(face) + 30, 255),
                       MIN(COLOR_B(face) + 30, 255));
        }

        fb_fill_round_rect(bx, by, cell_w, cell_h, 5, face);
        fb_draw_round_rect(bx, by, cell_w, cell_h, 5, theme->border);

        color_t text = is_equals ? RGB(255, 255, 255) : theme->window_text;
        if (strcmp(label, "C") == 0) {
            text = RGB(250, 140, 130);
        }
        fb_draw_string(bx + cell_w / 2 - fb_text_width(label) / 2,
                       by + cell_h / 2 - FONT_HEIGHT / 2, label, text);
    }
}

static bool_t calc_on_key(struct window *win, uint32_t key, uint32_t modifiers)
{
    struct calc_state *state = (struct calc_state *)win->app_state;
    UNUSED(modifiers);

    if (key == KEY_ENTER || key == '\n' || key == '\r' || key == '=') {
        calc_evaluate(state);
        return true;
    }
    if (key == KEY_BACKSPACE || key == '\b' || key == 127) {
        calc_press(state, "<");
        return true;
    }
    if (key == KEY_ESCAPE || key == 'c' || key == 'C') {
        calc_press(state, "C");
        return true;
    }
    if ((key >= '0' && key <= '9') || key == '+' || key == '-' || key == '*' ||
        key == '/' || key == '%' || key == '(' || key == ')') {
        calc_append(state, (char)key);
        return true;
    }
    return false;
}

static bool_t calc_on_mouse(struct window *win, int x, int y, uint32_t buttons,
                            input_event_type_t type)
{
    struct calc_state *state = (struct calc_state *)win->app_state;

    struct rect client;
    window_client_rect(win, &client);

    int display_h = 62;
    int pad_y     = 8 + display_h + 16;
    int pad_h     = client.h - pad_y - 10;
    int cols = 5, rows = 4;
    int cell_w = (client.w - 16 - (cols - 1) * 6) / cols;
    int cell_h = (pad_h - (rows - 1) * 6) / rows;

    int hovered = -1;
    for (int i = 0; i < 20; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx  = 8 + col * (cell_w + 6);
        int by  = pad_y + row * (cell_h + 6);

        if (x >= bx && x < bx + cell_w && y >= by && y < by + cell_h) {
            hovered = i;
            break;
        }
    }

    bool_t changed = (hovered != state->hover_button);
    state->hover_button = hovered;

    if (type == INPUT_MOUSE_DOWN && (buttons & MOUSE_LEFT) && hovered >= 0) {
        calc_press(state, keypad[hovered]);
        return true;
    }
    return changed;
}

static const struct app_ops calc_ops = {
    .draw     = calc_draw,
    .on_key   = calc_on_key,
    .on_mouse = calc_on_mouse,
    .on_open  = calc_on_open,
};

void app_calculator_register(void)
{
    desktop_register_app("Calculator", "=", &calc_ops, 320, 380,
                         RGB(250, 190, 120), sizeof(struct calc_state), true);
}
