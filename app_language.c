#include "app_language.h"
#include <stdio.h>
#include <string.h>

static void set_text_color(Gc gc, int color) {
    gui_gc_setColorRGB(gc, (color >> 16) & 255, (color >> 8) & 255, color & 255);
}

int utf8_to_u16(const char *s, uint16_t *out, int cap) {
    int n = 0;
    while (*s && n + 1 < cap) {
        unsigned char c = (unsigned char)*s++;
        uint32_t v;
        int need = 0, i;
        if (c < 0x80) v = c;
        else if ((c & 0xe0) == 0xc0) { v = c & 0x1f; need = 1; }
        else if ((c & 0xf0) == 0xe0) { v = c & 0x0f; need = 2; }
        else if ((c & 0xf8) == 0xf0) { v = c & 7; need = 3; }
        else v = 0xfffd;
        for (i = 0; i < need; ++i) {
            unsigned char d = (unsigned char)*s;
            if ((d & 0xc0) != 0x80) { v = 0xfffd; break; }
            s++;
            v = (v << 6) | (d & 0x3f);
        }
        if (v > 0xffff) v = 0xfffd;
        out[n++] = (uint16_t)v;
    }
    out[n] = 0;
    return n;
}

void draw_text(Gc gc, const char *utf8, int x, int y, gui_gc_Font font, int color) {
    uint16_t buf[NTEXTS_MAX_LINE_U16];
    utf8_to_u16(utf8, buf, NTEXTS_MAX_LINE_U16);
    gui_gc_setFont(gc, font);
    set_text_color(gc, color);
    gui_gc_drawString(gc, (char *)buf, x, y, GC_SM_TOP);
}

void draw_u16(Gc gc, const uint16_t *s, int x, int y, gui_gc_Font font, int color) {
    gui_gc_setFont(gc, font);
    set_text_color(gc, color);
    gui_gc_drawString(gc, (char *)s, x, y, GC_SM_TOP);
}

int copy_u16(uint16_t *dst, int cap, const uint16_t *src) {
    int n = 0;
    if (!cap) return 0;
    while (n + 1 < cap && src && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

int draw_wrapped_text(Gc gc, const char *text, int x, int y, int width, gui_gc_Font font, int color) {
    uint16_t utf16[NTEXTS_MAX_LINE_U16];
    uint16_t line[NTEXTS_MAX_LINE_U16];
    int n = utf8_to_u16(text, utf16, NTEXTS_MAX_LINE_U16);
    int line_len = 0;
    int line_width = 0;
    int line_height = gui_gc_getFontHeight(gc, font) + 2;
    int draw_y = y;
    for (int i = 0; i < n; ++i) {
        int cw;
        if (utf16[i] == '\n') {
            line[line_len] = 0;
            draw_u16(gc, line, x, draw_y, font, color);
            line_len = 0;
            line_width = 0;
            draw_y += line_height;
            continue;
        }
        cw = gui_gc_getCharWidth(gc, font, (short)utf16[i]);
        if (cw <= 0) cw = (int)font & 31;
        if (line_len && line_width + cw > width) {
            line[line_len] = 0;
            draw_u16(gc, line, x, draw_y, font, color);
            line_len = 0;
            line_width = 0;
            draw_y += line_height;
        }
        if (line_len + 1 < NTEXTS_MAX_LINE_U16) line[line_len++] = utf16[i];
        line_width += cw;
    }
    if (line_len) {
        line[line_len] = 0;
        draw_u16(gc, line, x, draw_y, font, color);
        draw_y += line_height;
    }
    return draw_y;
}

void shorten_tail(char *out, size_t cap, const char *text) {
    size_t len;
    if (!cap) return;
    if (!text) {
        out[0] = 0;
        return;
    }
    len = strlen(text);
    if (len + 1 <= cap) {
        memcpy(out, text, len + 1);
        return;
    }
    if (cap <= 4) {
        out[0] = 0;
        return;
    }
    snprintf(out, cap, "...%s", text + len - (cap - 4));
}
