#include <ngc.h>
#include <stdarg.h>

void app_log(const char *tag, const char *fmt, ...) {
    (void)tag;
    (void)fmt;
}

int gui_gc_getCharWidth(Gc gc, gui_gc_Font font, short value) {
    (void)gc;
    (void)font;
    (void)value;
    return 8;
}

const char *get_documents_dir(void) {
    return ".";
}
