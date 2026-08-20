/*
 * QitoOS - font registry
 */

#include <kernel/font.h>
#include <kernel/string.h>
#include <kernel/log.h>
#include <kernel/config.h>

static const struct font *ui_font;
static const struct font *terminal_font;

int font_count(void)
{
    return qito_font_count;
}

const struct font *font_at(int index)
{
    if (index < 0 || index >= qito_font_count) {
        return NULL;
    }
    return &qito_fonts[index];
}

const struct font *font_find(const char *id)
{
    if (!id) {
        return NULL;
    }
    for (int i = 0; i < qito_font_count; i++) {
        if (strcmp(qito_fonts[i].id, id) == 0) {
            return &qito_fonts[i];
        }
    }
    /* Accept the display name too, which is what Settings shows. */
    for (int i = 0; i < qito_font_count; i++) {
        if (strcasecmp(qito_fonts[i].name, id) == 0) {
            return &qito_fonts[i];
        }
    }
    return NULL;
}

const struct font *font_ui(void)
{
    return ui_font ? ui_font : &qito_fonts[0];
}

const struct font *font_terminal(void)
{
    return terminal_font ? terminal_font : font_ui();
}

void font_set_ui(const char *id)
{
    const struct font *font = font_find(id);
    if (font) {
        ui_font = font;
        KLOG_INFO("font", "interface face set to %s", font->name);
    } else {
        KLOG_WARN("font", "no such font: %s", id ? id : "(null)");
    }
}

void font_set_terminal(const char *id)
{
    const struct font *font = font_find(id);
    if (font) {
        terminal_font = font;
        KLOG_INFO("font", "terminal face set to %s", font->name);
    } else {
        KLOG_WARN("font", "no such font: %s", id ? id : "(null)");
    }
}

const uint8_t *font_glyph(const struct font *font, uint32_t character)
{
    if (!font) {
        return NULL;
    }
    if (character < font->first || character >= font->first + font->count) {
        /* Fall back to the final slot, which is the solid placeholder. */
        return font->glyphs + (size_t)(font->count - 1) * font->height;
    }
    return font->glyphs + (size_t)(character - font->first) * font->height;
}

int font_text_width(const struct font *font, const char *text, int scale)
{
    if (!font || !text) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    return (int)strlen(text) * font->width * scale;
}

void font_init(void)
{
    /* Defaults, overridden by the configuration if it names a valid face. */
    ui_font       = font_find("qito-sans");
    terminal_font = font_find("qito-mono");

    const char *configured_ui = config_get_string("desktop.font", NULL);
    if (configured_ui) {
        const struct font *font = font_find(configured_ui);
        if (font) {
            ui_font = font;
        }
    }

    const char *configured_terminal = config_get_string("terminal.font", NULL);
    if (configured_terminal) {
        const struct font *font = font_find(configured_terminal);
        if (font) {
            terminal_font = font;
        }
    }

    KLOG_INFO("font", "%d faces available; interface %s, terminal %s",
              qito_font_count, font_ui()->name, font_terminal()->name);
}
