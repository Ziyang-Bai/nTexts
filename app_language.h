#ifndef NTEXTS_APP_LANGUAGE_H
#define NTEXTS_APP_LANGUAGE_H

#include <ngc.h>
#include <stddef.h>
#include <stdint.h>

#define NTEXTS_MAX_LINE_U16 192

int utf8_to_u16(const char *s, uint16_t *out, int cap);
void draw_text(Gc gc, const char *utf8, int x, int y, gui_gc_Font font, int color);
void draw_u16(Gc gc, const uint16_t *s, int x, int y, gui_gc_Font font, int color);
int copy_u16(uint16_t *dst, int cap, const uint16_t *src);
int draw_wrapped_text(Gc gc, const char *text, int x, int y, int width, gui_gc_Font font, int color);
void shorten_tail(char *out, size_t cap, const char *text);

#endif
