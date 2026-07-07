#include "app_log.h"
#include "app_language.h"
#include "app_strings.h"
#include "reader_index.h"
#include "storage.h"
#include "text_engine.h"
#include <dirent.h>
#include <errno.h>
#include <libndls.h>
#include <ngc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define MAX_ITEMS 160
#define HOME_VISIBLE_RECENTS 5
#define FOOTER_Y 216
#define TUTORIAL_DEMO_FILE "tutorial_demo.txt.tns"

typedef struct {
    char path[512];
    char name[128];
    int is_dir;
} FileItem;

typedef struct {
    TextFile text;
    PageIndex index;
    BookState book;
    AppState *app;
    char data_dir[512];
    Gc gc;
    Layout layout;
    TextPosition top;
    uint32_t page;
    uint32_t hit_offset;
    int search_active;
    int tutorial_mode;
    int tutorial_step;
    uint16_t last_query[QUERY_LEN];
} Reader;

typedef struct {
    int bg, fg, muted, accent, highlight_bg, highlight_fg;
} Theme;

typedef struct {
    Gc gc;
    AppState *app;
    int app_ready;
    int fatal_active;
    int error_count;
    char where[64];
    char detail[192];
} DebugState;

static scr_type_t screen_type = SCR_320x240_565;
static DebugState debug_state;

static const Theme themes[] = {
    {0xffffff, 0x111111, 0x777777, 0x005bbb, 0xd8ebff, 0x000000},
    {0x111111, 0xe8e8e8, 0x999999, 0x80bfff, 0x26384a, 0xffffff},
    {0xf3ecd4, 0x24210f, 0x766f51, 0x4f7f3f, 0xdde7bf, 0x111111},
};

static void wait_release(void);
static void debug_error_page(const char *title, const char *where, const char *detail, int fatal);
static void draw_tutorial_overlay(Reader *r);

static void set_color(Gc gc, int color) {
    gui_gc_setColorRGB(gc, (color >> 16) & 255, (color >> 8) & 255, color & 255);
}

static void blit_gc(Gc gc) {
    gui_gc_blit_to_screen(gc);
}

static void fill_dim_outside(Gc gc, int x, int y, int w, int h) {
    gui_gc_setAlpha(gc, GC_A_HALF);
    set_color(gc, 0x000000);
    if (y > 0) gui_gc_fillRect(gc, 0, 0, SCREEN_W, y);
    if (x > 0) gui_gc_fillRect(gc, 0, y, x, h);
    if (x + w < SCREEN_W) gui_gc_fillRect(gc, x + w, y, SCREEN_W - x - w, h);
    if (y + h < SCREEN_H) gui_gc_fillRect(gc, 0, y + h, SCREEN_W, SCREEN_H - y - h);
    gui_gc_setAlpha(gc, GC_A_OFF);
    set_color(gc, 0xffcc33);
    gui_gc_drawRect(gc, x, y, w, h);
}

static void app_message(Gc gc, const AppState *app, const char *title, const char *msg) {
    const Theme *t = &themes[app->theme];
    gui_gc_begin(gc);
    set_color(gc, t->bg);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    draw_text(gc, title, 16, 36, Bold12, t->fg);
    draw_text(gc, msg, 16, 84, Regular10, t->fg);
    draw_text(gc, TXT_ENTER_CONTINUE, 16, 202, Regular9, t->muted);
    gui_gc_finish(gc);
    blit_gc(gc);
    for (;;) {
        if (isKeyPressed(KEY_NSPIRE_RET) || isKeyPressed(KEY_NSPIRE_ENTER) ||
            isKeyPressed(KEY_NSPIRE_ESC) || isKeyPressed(KEY_NSPIRE_CLICK)) {
            wait_release();
            return;
        }
        msleep(25);
    }
}

static void debug_set_runtime(Gc gc, AppState *app) {
    debug_state.gc = gc;
    debug_state.app = app;
    debug_state.app_ready = 1;
}

static void debug_format(char *out, size_t cap, const char *fmt, va_list ap) {
    if (!fmt || !cap) return;
    vsnprintf(out, cap, fmt, ap);
    out[cap - 1] = 0;
}

static void debug_fail(const char *where, const char *fmt, ...) {
    va_list ap;
    debug_state.error_count++;
    snprintf(debug_state.where, sizeof(debug_state.where), "%s", where ? where : "unknown");
    va_start(ap, fmt);
    debug_format(debug_state.detail, sizeof(debug_state.detail), fmt, ap);
    va_end(ap);
    debug_error_page("未知错误", debug_state.where, debug_state.detail, 0);
}

static void debug_fail_errno(const char *where, const char *what) {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s (errno=%d: %s)", what, errno, strerror(errno));
    debug_fail(where, "%s", buf);
}

static void debug_wrap_text(Gc gc, const char *text, int x, int y, int width, gui_gc_Font font, int color) {
    (void)draw_wrapped_text(gc, text, x, y, width, font, color);
}

static void debug_error_page(const char *title, const char *where, const char *detail, int fatal) {
    AppState fallback_app;
    AppState *app = debug_state.app_ready && debug_state.app ? debug_state.app : &fallback_app;
    const Theme *t;
    Gc gc = debug_state.gc;
    char head[64];
    char meta[96];
    if (!gc) return;
    if (app == &fallback_app) storage_app_defaults(&fallback_app);
    if (app->theme < 0 || app->theme > 2) app->theme = 0;
    t = &themes[app->theme];
    snprintf(head, sizeof(head), "%s #%d", title, debug_state.error_count ? debug_state.error_count : 1);
    snprintf(meta, sizeof(meta), "Ndless %u  HW subtype %u  lcd %d",
             nl_ndless_rev(), nl_hwsubtype(), (int)screen_type);
    gui_gc_begin(gc);
    set_color(gc, 0x2b0d0d);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    set_color(gc, 0x922f2f);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, 28);
    draw_text(gc, head, 10, 6, Bold10, 0xffffff);
    draw_text(gc, fatal ? "程序已进入致命错误处理" : "程序捕获到错误", 10, 38, Regular10, 0xffffff);
    draw_text(gc, "位置:", 10, 70, Bold10, 0xffd8d8);
    debug_wrap_text(gc, where ? where : "unknown", 10, 88, SCREEN_W - 20, Regular10, 0xffffff);
    draw_text(gc, "详情:", 10, 122, Bold10, 0xffd8d8);
    debug_wrap_text(gc, detail ? detail : "(none)", 10, 140, SCREEN_W - 20, Regular10, 0xffffff);
    draw_text(gc, meta, 10, 198, Regular9, 0xf0c0c0);
    draw_text(gc, fatal ? TXT_ESC_OR_ENTER_STAY : TXT_ESC_OR_ENTER_BACK, 10, 216, Regular9, t->muted);
    gui_gc_finish(gc);
    blit_gc(gc);
    for (;;) {
        if (isKeyPressed(KEY_NSPIRE_RET) || isKeyPressed(KEY_NSPIRE_ENTER) ||
            isKeyPressed(KEY_NSPIRE_ESC) || isKeyPressed(KEY_NSPIRE_CLICK)) {
            wait_release();
            if (!fatal) return;
        }
        msleep(25);
    }
}

static void debug_signal_handler(int sig) {
    const char *name = "SIGNAL";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGBUS: name = "SIGBUS"; break;
        case SIGILL: name = "SIGILL"; break;
        case SIGFPE: name = "SIGFPE"; break;
        default: break;
    }
    if (debug_state.fatal_active) for (;;) msleep(1000);
    debug_state.fatal_active = 1;
    debug_state.error_count++;
    snprintf(debug_state.where, sizeof(debug_state.where), "signal:%s", name);
    snprintf(debug_state.detail, sizeof(debug_state.detail), "捕获到运行时信号 %s (%d)", name, sig);
    debug_error_page("致命错误", debug_state.where, debug_state.detail, 1);
}

static void debug_install_handlers(void) {
    signal(SIGSEGV, debug_signal_handler);
    signal(SIGABRT, debug_signal_handler);
    signal(SIGBUS, debug_signal_handler);
    signal(SIGILL, debug_signal_handler);
    signal(SIGFPE, debug_signal_handler);
}

static const char *base_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int has_txt_ext(const char *name) {
    size_t n = strlen(name);
    return (n >= 4 && !strcmp(name + n - 4, ".txt")) ||
           (n >= 8 && !strcmp(name + n - 8, ".txt.tns"));
}

static int join_path(char *out, size_t cap, const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    if (a + 1 + b >= cap) return 0;
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return 1;
}

static int cmp_items(const void *a, const void *b) {
    const FileItem *ia = (const FileItem *)a, *ib = (const FileItem *)b;
    if (ia->is_dir != ib->is_dir) return ib->is_dir - ia->is_dir;
    return strcmp(ia->name, ib->name);
}

static int load_dir(const char *dir, FileItem *items, int *count) {
    DIR *dp = opendir(dir);
    struct dirent *de;
    *count = 0;
    if (!dp) {
        debug_fail_errno("load_dir:opendir", dir);
        return 0;
    }
    while ((de = readdir(dp)) && *count < MAX_ITEMS) {
        char path[512];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!join_path(path, sizeof(path), dir, de->d_name)) continue;
        if (stat(path, &st)) continue;
        if (!S_ISDIR(st.st_mode) && !has_txt_ext(de->d_name)) continue;
        strncpy(items[*count].path, path, sizeof(items[*count].path) - 1);
        strncpy(items[*count].name, de->d_name, sizeof(items[*count].name) - 1);
        items[*count].is_dir = S_ISDIR(st.st_mode);
        (*count)++;
    }
    closedir(dp);
    qsort(items, *count, sizeof(items[0]), cmp_items);
    return 1;
}

static void wait_release(void) {
    while (any_key_pressed()) msleep(20);
}

static int wait_key(void) {
    for (;;) {
        if (isKeyPressed(KEY_NSPIRE_UP)) return 1;
        if (isKeyPressed(KEY_NSPIRE_DOWN)) return 2;
        if (isKeyPressed(KEY_NSPIRE_LEFT)) return 3;
        if (isKeyPressed(KEY_NSPIRE_RIGHT)) return 4;
        if (isKeyPressed(KEY_NSPIRE_RET) || isKeyPressed(KEY_NSPIRE_ENTER) || isKeyPressed(KEY_NSPIRE_CLICK)) return 5;
        if (isKeyPressed(KEY_NSPIRE_ESC)) return 6;
        if (isKeyPressed(KEY_NSPIRE_MENU)) return 7;
        if (isKeyPressed(KEY_NSPIRE_B)) return 8;
        if (isKeyPressed(KEY_NSPIRE_F)) return 9;
        if (isKeyPressed(KEY_NSPIRE_G)) return 10;
        if (isKeyPressed(KEY_NSPIRE_HOME)) return 11;
        if (isKeyPressed(KEY_NSPIRE_0)) return 12;
        if (isKeyPressed(KEY_NSPIRE_NEGATIVE) || isKeyPressed(KEY_NSPIRE_MINUS)) return 13;
        if (isKeyPressed(KEY_NSPIRE_1)) return 21;
        if (isKeyPressed(KEY_NSPIRE_2)) return 22;
        if (isKeyPressed(KEY_NSPIRE_3)) return 23;
        if (isKeyPressed(KEY_NSPIRE_4)) return 24;
        if (isKeyPressed(KEY_NSPIRE_5)) return 25;
        if (isKeyPressed(KEY_NSPIRE_6)) return 26;
        if (isKeyPressed(KEY_NSPIRE_7)) return 27;
        if (isKeyPressed(KEY_NSPIRE_8)) return 28;
        if (isKeyPressed(KEY_NSPIRE_9)) return 29;
        msleep(25);
    }
}

static Layout layout_from_state(Gc gc, const AppState *state) {
    const int fonts[] = {Regular9, Regular10, Regular12};
    const int margins[] = {6, 14};
    const int line_heights[] = {20, 24, 28};
    Layout l;
    int fi = state->font_choice < 0 || state->font_choice > 2 ? 1 : state->font_choice;
    int mi = state->margin_choice ? 1 : 0;
    (void)gc;
    l.font_size = fonts[fi];
    l.margin = margins[mi];
    l.line_height = line_heights[fi];
    l.lines_per_page = (SCREEN_H - 22 - l.margin * 2) / l.line_height;
    if (l.lines_per_page < 1) l.lines_per_page = 1;
    return l;
}

static int progress_cb(uint32_t done, uint32_t total, void *ctx) {
    Reader *r = (Reader *)ctx;
    char msg[64];
    if (isKeyPressed(KEY_NSPIRE_ESC)) return 0;
    gui_gc_begin(r->gc);
    set_color(r->gc, themes[r->app->theme].bg);
    gui_gc_fillRect(r->gc, 0, 0, SCREEN_W, SCREEN_H);
    snprintf(msg, sizeof(msg), "首次索引: %lu%%  Esc取消", total ? (unsigned long)(done * 100u / total) : 0ul);
    draw_text(r->gc, msg, 42, 104, Regular10, themes[r->app->theme].fg);
    gui_gc_finish(r->gc);
    blit_gc(r->gc);
    return 1;
}

static int ensure_index(Reader *r) {
    char path[600];
    int loaded;
    storage_book_path(path, sizeof(path), r->data_dir, r->text.path, "idx");
    app_log("reader", "ensure index %s", path);
    loaded = index_load(&r->index, &r->text, r->layout, path);
    if (loaded) {
        app_log("reader", "index cache loaded");
        return 1;
    }
    app_log("reader", "index build needed");
    if (index_build(&r->index, &r->text, r->gc, r->layout, progress_cb, r) <= 0) {
        debug_fail("ensure_index:index_build", "索引构建被取消，或构建失败");
        return 0;
    }
    if (!index_save(&r->index, path))
        debug_fail_errno("ensure_index:index_save", path);
    return 1;
}

static void save_reader(Reader *r) {
    if (r->tutorial_mode) return;
    r->book.progress = r->top;
    if (!storage_save_book(&r->book, r->data_dir))
        debug_fail_errno("save_reader:book", r->book.path);
    storage_touch_recent(r->app, &r->book);
    if (!storage_save_app(r->app, r->data_dir))
        debug_fail_errno("save_reader:app", r->data_dir);
}

static uint32_t percent_for_reader(const Reader *r) {
    if (!r->text.size) return 0;
    return (uint32_t)(((uint64_t)r->top.byte_offset * 100u) / r->text.size);
}

static int wrap_push(uint16_t lines[][NTEXTS_MAX_LINE_U16], int *lens, uint32_t starts[], int *line,
                     const uint16_t *buf, int len, uint32_t start) {
    if (*line >= 32) return 0;
    memcpy(lines[*line], buf, (len + 1) * sizeof(uint16_t));
    lens[*line] = len;
    starts[*line] = start;
    (*line)++;
    return 1;
}

static void draw_page(Reader *r) {
    const Theme *t = &themes[r->app->theme];
    TextDecoder d;
    uint16_t lines[32][NTEXTS_MAX_LINE_U16], linebuf[NTEXTS_MAX_LINE_U16], ch;
    int lens[32], line = 0, len = 0, width = 0, maxw = SCREEN_W - r->layout.margin * 2;
    uint32_t starts[32], off, line_start = r->top.byte_offset;
    char status[96];
    text_decoder_at(&r->text, &d, r->top);
    memset(lines, 0, sizeof(lines));
    while (line < r->layout.lines_per_page && text_next(&d, &ch, &off)) {
        int cw;
        if (ch == '\r') continue;
        if (ch == '\n') {
            linebuf[len] = 0;
            wrap_push(lines, lens, starts, &line, linebuf, len, line_start);
            len = width = 0;
            line_start = d.offset;
            continue;
        }
        if (ch == '\t') ch = ' ';
        if (ch < 32) continue;
        cw = gui_gc_getCharWidth(r->gc, (gui_gc_Font)r->layout.font_size, (short)ch);
        if (cw <= 0) cw = 10;
        if (len && width + cw > maxw) {
            linebuf[len] = 0;
            wrap_push(lines, lens, starts, &line, linebuf, len, line_start);
            len = width = 0;
            line_start = off;
            if (line >= r->layout.lines_per_page) break;
        }
        if (len + 1 < NTEXTS_MAX_LINE_U16) linebuf[len++] = ch;
        width += cw;
    }
    if (line < r->layout.lines_per_page && len) {
        linebuf[len] = 0;
        wrap_push(lines, lens, starts, &line, linebuf, len, line_start);
    }

    gui_gc_begin(r->gc);
    set_color(r->gc, t->bg);
    gui_gc_fillRect(r->gc, 0, 0, SCREEN_W, SCREEN_H);
    for (int i = 0; i < line; ++i) {
        int y = r->layout.margin + i * r->layout.line_height;
        if (r->hit_offset && starts[i] <= r->hit_offset &&
            (i + 1 == line || starts[i + 1] > r->hit_offset)) {
            set_color(r->gc, t->highlight_bg);
            gui_gc_fillRect(r->gc, r->layout.margin - 2, y - 1, maxw + 4, r->layout.line_height);
            draw_u16(r->gc, lines[i], r->layout.margin, y, (gui_gc_Font)r->layout.font_size, t->highlight_fg);
        } else {
            draw_u16(r->gc, lines[i], r->layout.margin, y, (gui_gc_Font)r->layout.font_size, t->fg);
        }
    }
    if (r->tutorial_mode) {
        draw_tutorial_overlay(r);
    } else {
        set_color(r->gc, t->muted);
        gui_gc_drawLine(r->gc, 0, SCREEN_H - 18, SCREEN_W, SCREEN_H - 18);
        if (r->search_active && r->hit_offset) {
            snprintf(status, sizeof(status), "%s", TXT_SEARCH_MODE_HINT);
        } else {
            snprintf(status, sizeof(status), "%lu/%lu页  %lu%%  %s",
                     (unsigned long)(r->page + 1), (unsigned long)r->index.page_count,
                     (unsigned long)percent_for_reader(r), text_encoding_name(r->text.encoding));
        }
        draw_text(r->gc, status, 6, SCREEN_H - 15, Regular9, t->muted);
    }
    gui_gc_finish(r->gc);
    blit_gc(r->gc);
}

static void draw_tutorial_overlay(Reader *r) {
    const Theme *t = &themes[r->app->theme];
    int box_y = 126;
    if (r->tutorial_step == 0) {
        fill_dim_outside(r->gc, r->layout.margin - 2, r->layout.margin - 2,
                         SCREEN_W - r->layout.margin * 2 + 4, SCREEN_H - 44);
        set_color(r->gc, t->bg);
        gui_gc_fillRect(r->gc, 8, box_y, SCREEN_W - 16, 58);
        draw_text(r->gc, TXT_TUTORIAL_STEP1_TITLE, 14, box_y + 6, Bold10, t->fg);
        draw_text(r->gc, TXT_TUTORIAL_STEP1_HINT, 14, box_y + 25, Regular9, t->muted);
    } else if (r->tutorial_step == 1) {
        fill_dim_outside(r->gc, 0, SCREEN_H - 22, SCREEN_W, 22);
        set_color(r->gc, t->bg);
        gui_gc_fillRect(r->gc, 8, 34, SCREEN_W - 16, 43);
        draw_text(r->gc, TXT_TUTORIAL_STEP2_TITLE, 14, 40, Bold10, t->fg);
        draw_text(r->gc, TXT_TUTORIAL_STEP2_HINT, 14, 59, Regular9, t->muted);
    } else if (r->tutorial_step == 2) {
        fill_dim_outside(r->gc, 0, SCREEN_H - 22, SCREEN_W, 22);
        set_color(r->gc, t->bg);
        gui_gc_fillRect(r->gc, 8, 34, SCREEN_W - 16, 48);
        draw_text(r->gc, TXT_TUTORIAL_STEP3_TITLE, 14, 40, Bold10, t->fg);
        draw_text(r->gc, TXT_TUTORIAL_STEP3_HINT, 14, 60, Regular9, t->muted);
    } else {
        fill_dim_outside(r->gc, r->layout.margin - 2, r->layout.margin - 2,
                         SCREEN_W - r->layout.margin * 2 + 4, SCREEN_H - 44);
        set_color(r->gc, t->bg);
        gui_gc_fillRect(r->gc, 8, box_y, SCREEN_W - 16, 58);
        draw_text(r->gc, TXT_TUTORIAL_STEP4_TITLE, 14, box_y + 6, Bold10, t->fg);
        draw_text(r->gc, TXT_TUTORIAL_STEP4_HINT, 14, box_y + 26, Regular9, t->muted);
    }
}

static void reader_goto(Reader *r, TextPosition pos) {
    r->top = text_safe_position(&r->text, pos.byte_offset, pos.source_line);
    r->page = index_page_for_offset(&r->index, r->top.byte_offset);
}

static void reader_goto_page(Reader *r, uint32_t page) {
    if (!r->index.page_count) return;
    if (page >= r->index.page_count) page = r->index.page_count - 1;
    r->page = page;
    r->top = text_safe_position(&r->text, r->index.pages[page].byte_offset,
                                r->index.pages[page].source_line);
}

static TextPosition position_for_line_exact(Reader *r, uint32_t line) {
    TextPosition pos = index_position_for_line(&r->index, line);
    TextDecoder d;
    uint16_t ch;
    uint32_t off;
    if (line <= 1) return r->index.pages[0];
    text_decoder_at(&r->text, &d, pos);
    while (d.line < line && text_next(&d, &ch, &off)) {
        if (ch == '\n' && d.line == line) {
            pos.byte_offset = d.offset;
            pos.source_line = d.line;
            return pos;
        }
    }
    pos.byte_offset = d.offset;
    pos.source_line = d.line;
    return pos;
}

static void make_excerpt(Reader *r, TextPosition pos, uint16_t *out, int cap) {
    TextDecoder d;
    uint16_t ch;
    uint32_t off;
    int n = 0;
    text_decoder_at(&r->text, &d, pos);
    while (n + 1 < cap && text_next(&d, &ch, &off)) {
        if (ch == '\r') continue;
        if (ch == '\n') break;
        if (ch < 32) continue;
        out[n++] = ch;
    }
    out[n] = 0;
}

static void add_bookmark(Reader *r) {
    Bookmark *b;
    if (r->book.bookmark_count >= MAX_BOOKMARKS) {
        app_message(r->gc, r->app, "书签", "书签已满，请先删除");
        return;
    }
    b = &r->book.bookmarks[r->book.bookmark_count++];
    memset(b, 0, sizeof(*b));
    b->position = r->top;
    b->timestamp = (uint32_t)time(NULL);
    make_excerpt(r, r->top, b->excerpt, EXCERPT_LEN);
    save_reader(r);
    app_message(r->gc, r->app, "书签", "已添加书签");
}

static int choose_list(Gc gc, AppState *app, const char *title, const char **items, int count) {
    int sel = 0, top = 0, key, visible = 9;
    const Theme *t = &themes[app->theme];
    if (count <= 0) return -1;
    for (;;) {
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, title, 8, 6, Bold10, t->fg);
        for (int i = 0; i < visible && top + i < count; ++i) {
            int y = 30 + i * 21;
            if (top + i == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            draw_text(gc, items[top + i], 10, y, Regular10, top + i == sel ? t->highlight_fg : t->fg);
        }
        draw_text(gc, TXT_FOOTER_LIST, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);
        key = wait_key(); wait_release();
        if (key >= 21 && key <= 29) {
            int pick = key - 21;
            if (pick < count) return pick;
        }
        if (key == 1) {
            sel = sel > 0 ? sel - 1 : count - 1;
            if (sel < top) top = sel;
            if (sel >= top + visible) top = sel - visible + 1;
        } else if (key == 2) {
            sel = sel + 1 < count ? sel + 1 : 0;
            if (sel >= top + visible) top = sel - visible + 1;
            if (sel < top) top = sel;
        }
        else if (key == 5) return sel;
        else if (key == 6) return -1;
    }
}

static int manage_bookmarks(Reader *r) {
    const char *labels[MAX_BOOKMARKS];
    char bufs[MAX_BOOKMARKS][96];
    int i, pick;
    if (!r->book.bookmark_count) { app_message(r->gc, r->app, "书签", "暂无书签"); return 0; }
    for (i = 0; i < (int)r->book.bookmark_count; ++i) {
        snprintf(bufs[i], sizeof(bufs[i]), "%02d  %lu%%  行%lu", i + 1,
                 (unsigned long)(r->text.size ? r->book.bookmarks[i].position.byte_offset * 100u / r->text.size : 0),
                 (unsigned long)r->book.bookmarks[i].position.source_line);
        labels[i] = bufs[i];
    }
    pick = choose_list(r->gc, r->app, "管理书签", labels, r->book.bookmark_count);
    if (pick >= 0) {
        const char *actions[] = {"跳转", "删除", "取消"};
        int ans = choose_list(r->gc, r->app, "书签操作", actions, 3);
        if (ans == 0) {
            reader_goto(r, r->book.bookmarks[pick].position);
            save_reader(r);
            return 1;
        }
        else if (ans == 1) {
            memmove(&r->book.bookmarks[pick], &r->book.bookmarks[pick + 1],
                    (r->book.bookmark_count - pick - 1) * sizeof(r->book.bookmarks[0]));
            r->book.bookmark_count--;
            save_reader(r);
        }
    }
    return 0;
}

static int query_from_user(Reader *r, uint16_t *query) {
    String request_value = string_new();
    String s_title = string_new();
    String s_msg = string_new();
    String request_struct[] = {s_msg, request_value};
    uint16_t title_u16[16];
    uint16_t msg_u16[40];
    int no_error, len;
    (void)r;
    utf8_to_u16("搜索", title_u16, (int)(sizeof(title_u16) / sizeof(title_u16[0])));
    utf8_to_u16("输入搜索词", msg_u16, (int)(sizeof(msg_u16) / sizeof(msg_u16[0])));
    string_set_utf16(s_title, (const char *)title_u16);
    string_set_utf16(s_msg, (const char *)msg_u16);
    if (r->app->history_count) {
        string_set_utf16(request_value, (const char *)r->app->history[0]);
    } else {
        static const uint16_t empty[1] = {0};
        string_set_utf16(request_value, (const char *)empty);
    }
    no_error = _show_msgUserInput(0, request_struct, s_title->str, s_msg->str);
    string_free(s_title);
    string_free(s_msg);
    if (!(no_error && request_value->len > 0 && request_value->str)) {
        string_free(request_value);
        if (r->app->history_count) {
            memcpy(query, r->app->history[0], sizeof(r->app->history[0]));
            return query[0] != 0;
        }
        return 0;
    }
    len = copy_u16(query, QUERY_LEN, (const uint16_t *)request_value->str);
    string_free(request_value);
    if (!len && r->app->history_count) {
        memcpy(query, r->app->history[0], sizeof(r->app->history[0]));
        return query[0] != 0;
    }
    return query[0] != 0;
}

static int match_char(uint16_t a, uint16_t b) {
    if (a < 128 && b < 128) return text_fold_ascii(a) == text_fold_ascii(b);
    return a == b;
}

static int search_forward(Reader *r, const uint16_t *query, uint32_t from, TextPosition *hit) {
    TextDecoder d;
    uint16_t ring[QUERY_LEN], ch;
    uint32_t offs[QUERY_LEN], off, qlen = 0, seen = 0;
    while (qlen < QUERY_LEN && query[qlen]) qlen++;
    if (!qlen) return 0;
    text_decoder_at(&r->text, &d, (TextPosition){from, 1});
    while (text_next(&d, &ch, &off)) {
        ring[seen % qlen] = ch;
        offs[seen % qlen] = off;
        seen++;
        if (seen >= qlen) {
            uint32_t i;
            for (i = 0; i < qlen; ++i) {
                uint32_t idx = (seen - qlen + i) % qlen;
                if (!match_char(ring[idx], query[i])) break;
            }
            if (i == qlen) {
                hit->byte_offset = offs[(seen - qlen) % qlen];
                hit->source_line = d.line;
                return 1;
            }
        }
    }
    return 0;
}

static uint32_t next_char_offset(Reader *r, TextPosition pos);

static int search_backward(Reader *r, const uint16_t *query, uint32_t before, TextPosition *hit) {
    TextPosition found;
    uint32_t from = r->text.data_start;
    int ok = 0;
    while (search_forward(r, query, from, &found) && found.byte_offset < before) {
        *hit = found;
        from = next_char_offset(r, found);
        ok = 1;
    }
    return ok;
}

static uint32_t next_char_offset(Reader *r, TextPosition pos) {
    TextDecoder d;
    uint16_t ch;
    uint32_t off;
    text_decoder_at(&r->text, &d, pos);
    if (text_next(&d, &ch, &off)) return d.offset;
    return pos.byte_offset;
}

static int do_search(Reader *r, int direction, int reuse) {
    TextPosition hit;
    uint16_t query[QUERY_LEN];
    uint32_t anchor;
    memcpy(query, r->last_query, sizeof(query));
    if (!reuse && !query_from_user(r, query)) return 0;
    memcpy(r->last_query, query, sizeof(r->last_query));
    if (!r->tutorial_mode) {
        storage_add_history(r->app, query);
        storage_save_app(r->app, r->data_dir);
    }
    if (direction >= 0) {
        anchor = (reuse && r->hit_offset) ? r->hit_offset : r->top.byte_offset;
        if (!search_forward(r, query, next_char_offset(r, (TextPosition){anchor, r->top.source_line}), &hit)) {
            app_message(r->gc, r->app, "搜索", "已到末尾，未找到");
            return 0;
        }
    } else {
        anchor = (reuse && r->hit_offset) ? r->hit_offset : r->top.byte_offset;
        if (!search_backward(r, query, anchor, &hit)) {
            app_message(r->gc, r->app, "搜索", "已到开头，未找到");
            return 0;
        }
    }
    r->hit_offset = hit.byte_offset;
    r->search_active = 1;
    reader_goto(r, index_position_for_offset(&r->index, hit.byte_offset));
    return 1;
}

static void do_jump(Reader *r) {
    const char *items[] = {"百分比", "页码", "物理行号"};
    int pick = choose_list(r->gc, r->app, "跳转", items, 3);
    int value = 0;
    if (pick < 0) return;
    if (pick == 0) {
        value = (int)percent_for_reader(r);
        if (show_1numeric_input("跳转", "", "百分比 0-100", &value, 0, 100))
            reader_goto(r, index_position_for_percent(&r->index, value));
    } else if (pick == 1) {
        value = (int)r->page + 1;
        if (show_1numeric_input("跳转", "", "页码", &value, 1, (int)r->index.page_count))
            reader_goto(r, index_position_for_page(&r->index, (uint32_t)value - 1));
    } else {
        value = (int)r->top.source_line;
        if (show_1numeric_input("跳转", "", "原始物理行号", &value, 1, (int)r->index.total_source_lines))
            reader_goto(r, position_for_line_exact(r, (uint32_t)value));
    }
}

static int apply_setting_change(Reader *r, AppState *app, const char *data_dir, int pick, int delta) {
    if (pick == 0) app->font_choice = (app->font_choice + delta + 3) % 3;
    else if (pick == 1) app->theme = (app->theme + delta + 3) % 3;
    else app->margin_choice = app->margin_choice ? 0 : 1;
    storage_save_app(app, data_dir);
    if (r) {
        uint32_t pct = percent_for_reader(r);
        r->layout = layout_from_state(r->gc, r->app);
        index_free(&r->index);
        if (!ensure_index(r)) return -1;
        reader_goto(r, index_position_for_percent(&r->index, pct));
    }
    return 1;
}

static void about_page(Gc gc, AppState *app) {
    const Theme *t = &themes[app->theme];
    for (;;) {
        int key;
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, "关于 nTexts", 10, 8, Bold12, t->fg);
        draw_text(gc, "nTexts 中文阅读器", 10, 48, Regular10, t->fg);
        draw_text(gc, "版本: v.0.0 Alpha", 10, 78, Regular10, t->fg);
        draw_text(gc, "作者: Ziyang-Bai", 10, 108, Regular10, t->fg);
        draw_text(gc, "Copyright (c) 2026 Ziyang-Bai", 10, 140, Regular9, t->fg);
        draw_text(gc, "All rights reserved.", 10, 160, Regular9, t->fg);
        draw_text(gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);
        key = wait_key();
        wait_release();
        if (key == 5 || key == 6) return;
    }
}

static void usage_page(Gc gc, AppState *app) {
    const Theme *t = &themes[app->theme];
    for (;;) {
        int key;
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, "使用说明", 10, 8, Bold12, t->fg);
        draw_text(gc, "主页: " TXT_ENTER_OPEN "，" TXT_HOME_BROWSE_DOCS "，" TXT_SETTINGS, 10, 42, Regular9, t->fg);
        draw_text(gc, "阅读: 左右翻页，上下逐行移动", 10, 66, Regular9, t->fg);
        draw_text(gc, "Ctrl+上/下 跳到开头/结尾", 10, 90, Regular9, t->fg);
        draw_text(gc, "Menu打开功能菜单，" TXT_ESC_BACK, 10, 114, Regular9, t->fg);
        draw_text(gc, "快捷键: B书签  F搜索  G跳转", 10, 138, Regular9, t->fg);
        draw_text(gc, "列表中可按数字键直接选择项目", 10, 162, Regular9, t->fg);
        draw_text(gc, "支持 .txt 和 .txt.tns 文本文档", 10, 186, Regular9, t->fg);
        draw_text(gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);
        key = wait_key();
        wait_release();
        if (key == 5 || key == 6) return;
    }
}

typedef enum {
    TUTORIAL_SCREEN_HOME,
    TUTORIAL_SCREEN_BROWSER,
    TUTORIAL_SCREEN_LOADING,
    TUTORIAL_SCREEN_READER,
    TUTORIAL_SCREEN_SEARCH,
    TUTORIAL_SCREEN_MENU,
    TUTORIAL_SCREEN_SETTINGS,
    TUTORIAL_SCREEN_TRANSFER
} TutorialScreen;

typedef struct {
    TutorialScreen screen;
    const char *title;
    const char *body1;
    const char *body2;
    const char *body3;
    int key;
    int need_ctrl;
    int x, y, w, h;
} TutorialStep;

static const char *tutorial_key_label(const TutorialStep *step) {
    if (step->need_ctrl && step->key == 1) return "Ctrl+上";
    if (step->need_ctrl && step->key == 2) return "Ctrl+下";
    switch (step->key) {
        case 1: return "上";
        case 2: return "下";
        case 3: return "左";
        case 4: return "右";
        case 5: return "Enter";
        case 6: return "Esc";
        case 7: return "Menu";
        case 8: return "B";
        case 9: return "F";
        case 10: return "G";
        case 12: return "0";
        case 13: return "(-)";
        case 21: return "1";
        case 22: return "2";
        case 23: return "3";
        case 24: return "4";
        case 25: return "5";
        case 26: return "6";
        case 27: return "7";
        case 28: return "8";
        case 29: return "9";
        default: return "Enter";
    }
}

static void tutorial_mock_screen(Gc gc, const Theme *t, TutorialScreen screen) {
    set_color(gc, t->bg);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    if (screen == TUTORIAL_SCREEN_HOME) {
        draw_text(gc, "nTexts 中文阅读器", 8, 6, Bold12, t->fg);
        draw_text(gc, "最近阅读", 8, 32, Bold10, t->fg);
        draw_text(gc, "1. tutorial_demo.txt.tns  12%", 10, 54, Regular10, t->fg);
        draw_text(gc, TXT_HOME_BROWSE_DOCS, 10, 96, Regular10, t->fg);
        draw_text(gc, TXT_SETTINGS, 10, 118, Regular10, t->fg);
        draw_text(gc, TXT_FOOTER_HOME, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_BROWSER) {
        draw_text(gc, "Documents", 8, 6, Bold10, t->fg);
        draw_text(gc, "[目录] Books", 10, 32, Regular10, t->fg);
        draw_text(gc, "tutorial_demo.txt.tns", 10, 54, Regular10, t->fg);
        draw_text(gc, "my_book.txt.tns", 10, 76, Regular10, t->fg);
        draw_text(gc, TXT_FOOTER_BROWSER, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_LOADING) {
        draw_text(gc, "首次索引: 42%  Esc取消", 52, 104, Regular10, t->fg);
    } else if (screen == TUTORIAL_SCREEN_READER || screen == TUTORIAL_SCREEN_SEARCH) {
        draw_text(gc, "nTexts 教学演示文档", 10, 12, Bold10, t->fg);
        draw_wrapped_text(gc, "这是正文阅读区。左右翻页，上下按行移动。", 10, 40,
                          SCREEN_W - 20, Regular10, t->fg);
        if (screen == TUTORIAL_SCREEN_SEARCH) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 8, 66, SCREEN_W - 16, 22);
            draw_text(gc, "搜索命中的这一行会保持高亮。", 10, 68, Regular10, t->highlight_fg);
            draw_text(gc, "搜索: Enter/下 下一处", 6, SCREEN_H - 28, Regular9, t->muted);
            draw_text(gc, "上 上一处  Esc退出", 6, SCREEN_H - 14, Regular9, t->muted);
        } else {
            draw_wrapped_text(gc, "按快捷键可以搜索、跳转、书签和打开菜单。", 10, 68,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_text(gc, "1/8页  12%  UTF-8", 6, SCREEN_H - 15, Regular9, t->muted);
        }
    } else if (screen == TUTORIAL_SCREEN_MENU) {
        const char *items[] = {"1. 添加书签", "2. 管理书签", "3. 搜索", "4. 向后查找",
                               "5. 向前查找", "6. 跳转", "7. 阅读设置", "8. 文件信息", "9. 返回书库"};
        draw_text(gc, "功能菜单", 8, 6, Bold10, t->fg);
        for (int i = 0; i < 9; ++i) draw_text(gc, items[i], 10, 30 + i * 19, Regular9, t->fg);
        draw_text(gc, TXT_FOOTER_LIST, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_SETTINGS) {
        draw_text(gc, "阅读设置", 8, 6, Bold12, t->fg);
        draw_text(gc, "1. 字号: 小/中/大  当前:中", 10, 32, Regular10, t->fg);
        draw_text(gc, "2. 主题: 浅色/深色/护眼  当前:浅色", 10, 54, Regular10, t->fg);
        draw_text(gc, "3. 边距: 窄/宽  当前:窄", 10, 76, Regular10, t->fg);
        draw_text(gc, "6. 教学中心", 10, 120, Regular10, t->fg);
        draw_text(gc, "7. 重置教学进度", 10, 142, Regular10, t->fg);
        draw_text(gc, "8. 跳过所有教学", 10, 164, Regular10, t->fg);
    } else {
        draw_text(gc, "传入自己的文档", 8, 8, Bold12, t->fg);
        draw_text(gc, "1. 将 nTexts.tns 传入计算器", 10, 42, Regular9, t->fg);
        draw_text(gc, "2. 文本文档命名为 book.txt.tns", 10, 66, Regular9, t->fg);
        draw_text(gc, "3. 放入 Documents 或子文件夹", 10, 90, Regular9, t->fg);
        draw_text(gc, "4. 在 nTexts 中浏览并打开", 10, 114, Regular9, t->fg);
    }
}

static void tutorial_center_page(Gc gc, AppState *app, const char *data_dir) {
    const Theme *t = &themes[app->theme];
    const TutorialStep steps[] = {
        {TUTORIAL_SCREEN_TRANSFER, "安装和传书", "先学如何把程序和自己的书放进计算器。", "文本文档建议命名为 book.txt.tns。", "现在按 Enter 继续。", 5, 0, 8, 34, 304, 104},
        {TUTORIAL_SCREEN_HOME, "主页", "这里显示最近阅读、浏览文档和设置。", "按 0 直接进入 Documents 浏览器。", "请按 0。", 12, 0, 6, 92, 180, 22},
        {TUTORIAL_SCREEN_BROWSER, "浏览文档", "浏览器只显示目录、.txt 和 .txt.tns。", "上下移动光标。", "请按下键选择文件。", 2, 0, 6, 50, 220, 24},
        {TUTORIAL_SCREEN_BROWSER, "进入或打开", "选中文件夹时 Enter 进入。", "选中文本文件时 Enter 打开。", "请按 Enter 打开演示文档。", 5, 0, 6, 50, 220, 24},
        {TUTORIAL_SCREEN_LOADING, "加载和索引", "首次打开会识别编码并建立分页索引。", "索引会缓存，下次打开会更快。", "请按 Enter 继续。", 5, 0, 42, 96, 236, 36},
        {TUTORIAL_SCREEN_READER, "阅读正文", "这是阅读区。", "左右翻页，上下按原文本物理行移动。", "请按右键翻到下一页。", 4, 0, 6, 8, 308, 176},
        {TUTORIAL_SCREEN_READER, "返回上一页", "左键回到上一页。", "阅读进度会自动保存。", "请按左键。", 3, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "快捷跳到开头", "Ctrl+上 会跳到整本书开头。", "适合快速回到第一页。", "请按 Ctrl+上。", 1, 1, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "快捷跳到结尾", "Ctrl+下 会跳到整本书结尾。", "适合快速查看末尾。", "请按 Ctrl+下。", 2, 1, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "搜索快捷键", "F 打开搜索。", "命中后会自动定位并高亮。", "请按 F。", 9, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_SEARCH, "继续搜索", "高亮后 Enter、下键、F 都找下一处。", "上键找上一处，Esc 退出搜索模式。", "请按 Enter 找下一处。", 5, 0, 6, 62, 308, 30},
        {TUTORIAL_SCREEN_SEARCH, "向前搜索", "现在练习向前查找。", "搜索模式里上键找上一处。", "请按上键。", 1, 0, 6, 62, 308, 30},
        {TUTORIAL_SCREEN_READER, "跳转快捷键", "G 打开跳转。", "可以按百分比、页码或物理行号跳转。", "请按 G。", 10, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "书签快捷键", "B 可以直接添加书签。", "Menu 里可以管理书签。", "请按 B。", 8, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "打开功能菜单", "Menu 打开完整功能菜单。", "下面会逐项练习菜单。", "请按 Menu。", 7, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 1: 添加书签", "添加当前位置书签。", "也可以直接按 B。", "请按 1。", 21, 0, 6, 28, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 2: 管理书签", "查看、跳转或删除已有书签。", "书签保存在当前书状态中。", "请按 2。", 22, 0, 6, 47, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 3: 搜索", "输入搜索词并定位第一个命中。", "搜索结果会高亮。", "请按 3。", 23, 0, 6, 66, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 4/5: 继续查找", "4 向后查找，5 向前查找。", "用于复用上一次搜索词。", "请按 4。", 24, 0, 6, 85, 190, 40},
        {TUTORIAL_SCREEN_MENU, "菜单 6: 跳转", "按百分比、页码或物理行号跳转。", "也可以直接按 G。", "请按 6。", 26, 0, 6, 123, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 7: 阅读设置", "调整字号、主题和边距。", "设置变化会重建当前书索引。", "请按 7。", 27, 0, 6, 142, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 8/9: 信息和返回", "8 查看文件信息，9 返回书库。", "返回书库也可以按 Esc。", "请按 8。", 28, 0, 6, 161, 190, 40},
        {TUTORIAL_SCREEN_HOME, "从最近阅读进入", "打开过的书会出现在最近阅读。", "选中最近阅读后 Enter 继续阅读。", "请按 Enter。", 5, 0, 6, 50, 240, 24},
        {TUTORIAL_SCREEN_HOME, "打开阅读设置", "主页按 (-) 打开阅读设置。", "也可在阅读菜单中打开。", "请按 (-)。", 13, 0, 6, 114, 180, 24},
        {TUTORIAL_SCREEN_SETTINGS, "更改字号", "设置里按 1 或 Enter/左右切换字号。", "大字号会重新分页。", "请按 1。", 21, 0, 6, 28, 292, 22},
        {TUTORIAL_SCREEN_SETTINGS, "更改主题", "按 2 切换浅色、深色和护眼主题。", "适合不同环境阅读。", "请按 2。", 22, 0, 6, 50, 292, 22},
        {TUTORIAL_SCREEN_SETTINGS, "更改边距", "按 3 切换窄边距和宽边距。", "宽边距更舒适，窄边距显示更多字。", "请按 3。", 23, 0, 6, 72, 292, 22},
        {TUTORIAL_SCREEN_SETTINGS, "教学控制", "6 重新进入交互教学。", "7 重置进度，8 跳过所有教学。", "请按 Enter 完成教学。", 5, 0, 6, 116, 292, 72}
    };
    int count = (int)(sizeof(steps) / sizeof(steps[0]));
    for (int i = 0; i < count;) {
        int key, ctrl;
        char footer[96];
        const TutorialStep *step = &steps[i];
        gui_gc_begin(gc);
        tutorial_mock_screen(gc, t, step->screen);
        fill_dim_outside(gc, step->x, step->y, step->w, step->h);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 8, 132, SCREEN_W - 16, 82);
        draw_text(gc, step->title, 14, 140, Bold10, t->fg);
        draw_text(gc, step->body1, 14, 158, Regular9, t->fg);
        draw_text(gc, step->body2, 14, 174, Regular9, t->fg);
        draw_text(gc, step->body3, 14, 190, Regular9, t->muted);
        snprintf(footer, sizeof(footer), "%d/%d  按%s继续  0跳过",
                 i + 1, count, tutorial_key_label(step));
        draw_text(gc, footer, 14, 206, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);

        key = wait_key();
        ctrl = isKeyPressed(KEY_NSPIRE_CTRL);
        wait_release();
        if (key == 12 && step->key != 12) {
            app->tutorial_flags = TUTORIAL_ALL_SKIPPED | TUTORIAL_READER_SEEN;
            storage_save_app(app, data_dir);
            app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_SKIP_DONE);
            return;
        }
        if (key == step->key && (!step->need_ctrl || ctrl)) i++;
    }
}

static int settings_menu(Gc gc, AppState *app, const char *data_dir, Reader *r) {
    const Theme *t;
    const char *font_labels[] = {"小", "中", "大"};
    const char *theme_labels[] = {"浅色", "深色", "护眼"};
    const char *margin_labels[] = {"窄", "宽"};
    char items[6][72];
    int sel = 0;
    for (;;) {
        int key;
        t = &themes[app->theme];
        snprintf(items[0], sizeof(items[0]), "1. 字号: %s/%s/%s  当前:%s",
                 font_labels[0], font_labels[1], font_labels[2], font_labels[app->font_choice]);
        snprintf(items[1], sizeof(items[1]), "2. 主题: %s/%s/%s  当前:%s",
                 theme_labels[0], theme_labels[1], theme_labels[2], theme_labels[app->theme]);
        snprintf(items[2], sizeof(items[2]), "3. 边距: %s/%s  当前:%s",
                 margin_labels[0], margin_labels[1], margin_labels[app->margin_choice ? 1 : 0]);
        snprintf(items[3], sizeof(items[3]), "4. 关于");
        snprintf(items[4], sizeof(items[4]), "5. 使用说明");
        snprintf(items[5], sizeof(items[5]), "6. 重置教学进度");
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, "阅读设置", 8, 6, Bold12, t->fg);
        for (int i = 0; i < 6; ++i) {
            int y = 34 + i * 25;
            if (i == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            draw_text(gc, items[i], 10, y, Regular10, i == sel ? t->highlight_fg : t->fg);
        }
        draw_text(gc, "1-6直达  左右/Enter执行  Esc返回", 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);
        key = wait_key(); wait_release();
        if (key == 1) sel = sel > 0 ? sel - 1 : 5;
        else if (key == 2) sel = sel < 5 ? sel + 1 : 0;
        else if (key >= 21 && key <= 26) {
            sel = key - 21;
            if (sel == 3) about_page(gc, app);
            else if (sel == 4) usage_page(gc, app);
            else if (sel == 5) {
                app->tutorial_flags = 0;
                storage_save_app(app, data_dir);
                app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_RESET_DONE);
            }
            else if (apply_setting_change(r, app, data_dir, sel, 1) < 0) return -1;
        }
        else if (key == 3) {
            if (sel == 3) about_page(gc, app);
            else if (sel == 4) usage_page(gc, app);
            else if (sel == 5) {
                app->tutorial_flags = 0;
                storage_save_app(app, data_dir);
                app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_RESET_DONE);
            }
            else if (apply_setting_change(r, app, data_dir, sel, -1) < 0) return -1;
        } else if (key == 4 || key == 5) {
            if (sel == 3) about_page(gc, app);
            else if (sel == 4) usage_page(gc, app);
            else if (sel == 5) {
                app->tutorial_flags = 0;
                storage_save_app(app, data_dir);
                app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_RESET_DONE);
            }
            else if (apply_setting_change(r, app, data_dir, sel, 1) < 0) return -1;
        } else if (key == 6) {
            return 0;
        }
    }
}

static int settings(Reader *r) {
    return settings_menu(r->gc, r->app, r->data_dir, r);
}

static void file_info(Reader *r) {
    const Theme *t = &themes[r->app->theme];
    char path[112];
    char name[72];
    char line[128];
    int y;
    shorten_tail(name, sizeof(name), base_name(r->text.path));
    shorten_tail(path, sizeof(path), r->text.path);
    for (;;) {
        gui_gc_begin(r->gc);
        set_color(r->gc, t->bg);
        gui_gc_fillRect(r->gc, 0, 0, SCREEN_W, SCREEN_H);

        draw_text(r->gc, "文件信息", 10, 8, Bold12, t->fg);
        snprintf(line, sizeof(line), "名称: %s", name);
        draw_text(r->gc, line, 10, 42, Regular10, t->fg);
        draw_text(r->gc, "路径:", 10, 68, Regular10, t->fg);
        y = draw_wrapped_text(r->gc, path, 10, 90, SCREEN_W - 20, Regular9, t->fg);
        if (y < 126) y = 126;

        snprintf(line, sizeof(line), "大小: %lu 字节", (unsigned long)r->text.size);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), "编码: %s", text_encoding_name(r->text.encoding));
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), "行数: %lu", (unsigned long)r->index.total_source_lines);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), "页数: %lu", (unsigned long)r->index.page_count);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), "修改: %lu", (unsigned long)r->text.mtime);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);

        draw_text(r->gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(r->gc);
        blit_gc(r->gc);

        y = wait_key();
        wait_release();
        if (y == 5 || y == 6) return;
    }
}

static int reader_menu(Reader *r) {
    const char *items[] = {
        "1. 添加书签",
        "2. 管理书签",
        "3. 搜索",
        "4. 向后查找",
        "5. 向前查找",
        "6. 跳转",
        "7. 阅读设置",
        "8. 文件信息",
        "9. 返回书库"
    };
    for (;;) {
        int pick = choose_list(r->gc, r->app, "功能菜单", items, 9);
        if (pick < 0) return 0;
        if (pick == 8) return -1;
        if (pick == 0) add_bookmark(r);
        else if (pick == 1) {
            if (manage_bookmarks(r) > 0) return 0;
        }
        else if (pick == 2) { do_search(r, 1, 0); return 0; }
        else if (pick == 3) { do_search(r, 1, 1); return 0; }
        else if (pick == 4) { do_search(r, -1, 1); return 0; }
        else if (pick == 5) { do_jump(r); return 0; }
        else if (pick == 6) {
            if (settings(r) < 0) return -1;
        } else if (pick == 7) {
            file_info(r);
        }
    }
}

static int open_reader(const char *path, Gc gc, AppState *app, const char *data_dir, int tutorial_mode) {
    Reader r;
    int key, menu_res;
    app_log("reader", "open begin %s", path ? path : "(null)");
    memset(&r, 0, sizeof(r));
    r.gc = gc;
    r.app = app;
    r.tutorial_mode = tutorial_mode;
    strncpy(r.data_dir, data_dir, sizeof(r.data_dir) - 1);
    index_init(&r.index);
    if (!text_open(&r.text, path)) {
        debug_fail_errno("open_reader:text_open", path);
        app_log("reader", "open text failed");
        return 0;
    }
    storage_load_book(&r.book, &r.text, data_dir);
    r.layout = layout_from_state(gc, app);
    if (!ensure_index(&r)) {
        app_log("reader", "ensure index failed");
        text_close(&r.text);
        return 0;
    }
    reader_goto(&r, r.book.progress.byte_offset ? r.book.progress : r.index.pages[0]);
    app_log("reader", "loop begin page_count=%lu start_offset=%lu",
            (unsigned long)r.index.page_count, (unsigned long)r.top.byte_offset);
    for (;;) {
        draw_page(&r);
        key = wait_key(); wait_release();
        if (r.tutorial_mode) {
            if (r.tutorial_step == 0 && key == 5) {
                r.tutorial_step = 1;
                continue;
            }
            if (r.tutorial_step == 1 && key == 4) {
                if (r.page + 1 < r.index.page_count) reader_goto_page(&r, r.page + 1);
                r.tutorial_step = 2;
                continue;
            }
            if (r.tutorial_step == 2 && key == 5) {
                utf8_to_u16("高亮", r.last_query, QUERY_LEN);
                do_search(&r, 1, 1);
                r.tutorial_step = 3;
                continue;
            }
            if (r.tutorial_step >= 3 && key == 6) break;
            continue;
        }
        if (r.search_active) {
            if (key == 5 || key == 2 || key == 9) {
                do_search(&r, 1, 1);
                save_reader(&r);
                continue;
            } else if (key == 1) {
                do_search(&r, -1, 1);
                save_reader(&r);
                continue;
            } else if (key == 6) {
                r.search_active = 0;
                r.hit_offset = 0;
                continue;
            }
            r.search_active = 0;
            r.hit_offset = 0;
        }
        r.hit_offset = 0;
        if (key == 4 && r.page + 1 < r.index.page_count) reader_goto_page(&r, r.page + 1);
        else if (key == 3 && r.page > 0) reader_goto_page(&r, r.page - 1);
        else if (key == 1 && isKeyPressed(KEY_NSPIRE_CTRL)) reader_goto_page(&r, 0);
        else if (key == 2 && isKeyPressed(KEY_NSPIRE_CTRL)) reader_goto_page(&r, r.index.page_count - 1);
        else if (key == 2 && r.top.source_line < r.index.total_source_lines) reader_goto(&r, position_for_line_exact(&r, r.top.source_line + 1));
        else if (key == 1 && r.top.source_line > 1) reader_goto(&r, position_for_line_exact(&r, r.top.source_line - 1));
        else if (key == 8) add_bookmark(&r);
        else if (key == 9) do_search(&r, 1, 0);
        else if (key == 10) do_jump(&r);
        else if (key == 7) {
            menu_res = reader_menu(&r);
            if (menu_res < 0) break;
        } else if (key == 6) break;
        save_reader(&r);
    }
    save_reader(&r);
    index_free(&r.index);
    text_close(&r.text);
    app_log("reader", "open end");
    return 1;
}

static void draw_home(Gc gc, AppState *app, int sel) {
    const Theme *t = &themes[app->theme];
    char line[128];
    int shown_recents = (int)app->recent_count > HOME_VISIBLE_RECENTS ? HOME_VISIBLE_RECENTS : (int)app->recent_count;
    int total = shown_recents + 2;
    int draw_sel = sel;
    int shortcut_base_y;
    static int logged_once = 0;
    if (!logged_once) app_log("home", "draw begin recents=%lu sel=%d theme=%d",
                              (unsigned long)app->recent_count, sel, app->theme);
    if (draw_sel >= total) draw_sel = total - 1;
    if (draw_sel < 0) draw_sel = 0;
    shortcut_base_y = shown_recents ? 54 + shown_recents * 20 + 10 : 92;
    if (!logged_once) app_log("home", "gc begin");
    gui_gc_begin(gc);
    if (!logged_once) app_log("home", "fill bg");
    set_color(gc, t->bg);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    if (!logged_once) app_log("home", "draw title");
    draw_text(gc, "nTexts 中文阅读器", 8, 6, Bold12, t->fg);
    draw_text(gc, "最近阅读", 8, 32, Bold10, t->fg);
    for (int i = 0; i < shown_recents; ++i) {
        int y = 54 + i * 20;
        if (i == draw_sel) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 19);
        }
        snprintf(line, sizeof(line), "%d. %s  %lu%%", i + 1, base_name(app->recents[i].path),
                 app->recents[i].file_size ? (unsigned long)(app->recents[i].offset * 100u / app->recents[i].file_size) : 0ul);
        draw_text(gc, line, 10, y, Regular10, i == draw_sel ? t->highlight_fg : t->fg);
    }
    if (!app->recent_count) draw_text(gc, "暂无最近阅读", 10, 58, Regular10, t->muted);
    for (int i = 0; i < 2; ++i) {
        int idx = shown_recents + i;
        int y = shortcut_base_y + i * 20;
        const char *label = i == 0 ? TXT_HOME_BROWSE_DOCS : TXT_SETTINGS;
        if (idx == draw_sel) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 19);
        }
        snprintf(line, sizeof(line), "%s", label);
        draw_text(gc, line, 10, y, Regular10, idx == draw_sel ? t->highlight_fg : t->fg);
    }
    draw_text(gc, TXT_FOOTER_HOME, 8, FOOTER_Y, Regular9, t->muted);
    if (!logged_once) app_log("home", "finish");
    gui_gc_finish(gc);
    if (!logged_once) app_log("home", "blit");
    blit_gc(gc);
    if (!logged_once) {
        app_log("home", "draw ok");
        logged_once = 1;
    }
}

static int browse_files(Gc gc, AppState *app, char *out, size_t outcap) {
    char cwd[512];
    FileItem items[MAX_ITEMS];
    int count = 0, sel = 0, top = 0, visible = 9;
    const Theme *t = &themes[app->theme];
    snprintf(cwd, sizeof(cwd), "%s", get_documents_dir());
    for (;;) {
        int key;
        if (!load_dir(cwd, items, &count)) return 0;
        if (sel >= count) sel = count ? count - 1 : 0;
        if (top > sel) top = sel;
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, base_name(cwd), 8, 6, Bold10, t->fg);
        for (int i = 0; i < visible && top + i < count; ++i) {
            char label[150];
            int y = 30 + i * 21;
            if (top + i == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            snprintf(label, sizeof(label), "%s%s", items[top + i].is_dir ? "[目录] " : "", items[top + i].name);
            draw_text(gc, label, 10, y, Regular10, top + i == sel ? t->highlight_fg : t->fg);
        }
        draw_text(gc, TXT_FOOTER_BROWSER, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);
        key = wait_key(); wait_release();
        if (key == 1) {
            sel = count ? (sel > 0 ? sel - 1 : count - 1) : 0;
            if (sel < top) top = sel;
            if (sel >= top + visible) top = sel - visible + 1;
        } else if (key == 2) {
            sel = count ? (sel + 1 < count ? sel + 1 : 0) : 0;
            if (sel >= top + visible) top = sel - visible + 1;
            if (sel < top) top = sel;
        }
        else if (key == 5 && count) {
            if (items[sel].is_dir) { strncpy(cwd, items[sel].path, sizeof(cwd) - 1); sel = top = 0; }
            else { strncpy(out, items[sel].path, outcap - 1); return 1; }
        } else if (key == 6) {
            char *slash = strrchr(cwd, '/');
            if (slash && strcmp(cwd, get_documents_dir())) { *slash = 0; sel = top = 0; }
            else return 0;
        }
    }
}

static int create_tutorial_demo(const char *data_dir, char *path, size_t path_cap) {
    FILE *fp;
    snprintf(path, path_cap, "%s/%s", data_dir, TUTORIAL_DEMO_FILE);
    fp = fopen(path, "wb");
    if (!fp) return 0;
    fputs("nTexts 教学演示文档\n\n"
          "欢迎使用 nTexts。这个临时文档会带你快速了解阅读、翻页和搜索高亮。\n\n"
          "第一步会高亮正文区域。阅读时，正文会按照当前字号和边距自动分页。\n\n"
          "第二步请按右键翻到下一页。左键和右键用于上一页、下一页。\n\n"
          "第三步会演示搜索高亮。程序会自动搜索“高亮”这个词，并把命中行标出来。\n\n"
          "搜索高亮出现后，可以按 Enter 或下键继续找下一处，按上键找上一处，按 Esc 退出搜索模式。\n\n"
          "教学结束后，这个演示文档和它的索引缓存会自动删除，不会留在最近阅读中。\n\n"
          "这里再放一个高亮，用来保证搜索演示能找到内容。\n",
          fp);
    fclose(fp);
    return 1;
}

static void cleanup_tutorial_demo(AppState *app, const char *data_dir, const char *path) {
    char cache[600];
    remove(path);
    storage_book_path(cache, sizeof(cache), data_dir, path, "idx");
    remove(cache);
    storage_book_path(cache, sizeof(cache), data_dir, path, "state");
    remove(cache);
    storage_remove_recent(app, path);
}

static void run_tutorial_if_needed(Gc gc, AppState *app, const char *data_dir) {
    char path[600];
    if (app->tutorial_flags & TUTORIAL_ALL_SKIPPED) return;
    if (app->tutorial_flags & TUTORIAL_READER_SEEN) return;
    app_log("tutorial", "start");
    tutorial_center_page(gc, app, data_dir);
    if (app->tutorial_flags & TUTORIAL_ALL_SKIPPED) return;
    if (!create_tutorial_demo(data_dir, path, sizeof(path))) {
        app_log("tutorial", "create demo failed");
        return;
    }
    open_reader(path, gc, app, data_dir, 1);
    cleanup_tutorial_demo(app, data_dir, path);
    app->tutorial_flags |= TUTORIAL_READER_SEEN;
    storage_save_app(app, data_dir);
    app_log("tutorial", "done");
}

int ntexts_app_main(int argc, char **argv) {
    Gc gc;
    AppState app;
    char data_dir[512], selected[512];
    int sel = 0;
    app_log("startup", "enter argc=%d", argc);
    enable_relative_paths(argv);
    storage_app_defaults(&app);
    debug_install_handlers();
    app_log("startup", "handlers installed");
    screen_type = lcd_type() == SCR_320x240_4 ? SCR_320x240_4 : SCR_320x240_565;
    app_log("startup", "lcd_type selected=%d", (int)screen_type);
    lcd_init(screen_type);
    gc = gui_gc_global_GC();
    debug_set_runtime(gc, &app);
    app_log("startup", "graphics ready gc=%p", gc);
    if (!storage_init(data_dir, sizeof(data_dir))) debug_fail_errno("main:storage_init", data_dir[0] ? data_dir : "Documents/nTexts");
    app_log("startup", "storage dir=%s", data_dir);
    storage_load_app(&app, data_dir);
    app_log("startup", "app loaded recents=%lu history=%lu theme=%d",
            (unsigned long)app.recent_count, (unsigned long)app.history_count, app.theme);
    if (app.theme < 0 || app.theme > 2) app.theme = 0;
    if (argc > 1) {
        app_log("startup", "open argv path=%s", argv[1]);
        open_reader(argv[1], gc, &app, data_dir, 0);
        lcd_init(SCR_TYPE_INVALID);
        app_log("shutdown", "exit after argv path");
        return 0;
    }
    run_tutorial_if_needed(gc, &app, data_dir);
    app_log("startup", "home loop begin");
    for (;;) {
        int key;
        draw_home(gc, &app, sel);
        key = wait_key(); wait_release();
        {
            int shown_recents = (int)app.recent_count > HOME_VISIBLE_RECENTS ? HOME_VISIBLE_RECENTS : (int)app.recent_count;
            int total = shown_recents + 2;
            if (key == 1) sel = sel > 0 ? sel - 1 : total - 1;
            else if (key == 2) sel = sel + 1 < total ? sel + 1 : 0;
            else if (key == 3 && sel == shown_recents + 1) settings_menu(gc, &app, data_dir, NULL);
            else if (key == 4 && sel == shown_recents + 1) settings_menu(gc, &app, data_dir, NULL);
            else if (key == 12) {
                memset(selected, 0, sizeof(selected));
                if (browse_files(gc, &app, selected, sizeof(selected))) open_reader(selected, gc, &app, data_dir, 0);
            } else if (key == 13) {
                settings_menu(gc, &app, data_dir, NULL);
            }
            else if (key == 5) {
                if (sel < shown_recents) {
                    char recent_path[512];
                    snprintf(recent_path, sizeof(recent_path), "%s", app.recents[sel].path);
                    app_log("home", "open recent[%d]=%s", sel, recent_path);
                    if (!open_reader(recent_path, gc, &app, data_dir, 0)) {
                        storage_remove_recent(&app, recent_path);
                        storage_save_app(&app, data_dir);
                    }
                }
                else if (sel == shown_recents) {
                    memset(selected, 0, sizeof(selected));
                    if (browse_files(gc, &app, selected, sizeof(selected))) open_reader(selected, gc, &app, data_dir, 0);
                } else {
                    settings_menu(gc, &app, data_dir, NULL);
                }
            } else if (key == 7) {
                memset(selected, 0, sizeof(selected));
                if (browse_files(gc, &app, selected, sizeof(selected))) open_reader(selected, gc, &app, data_dir, 0);
            } else if (key == 6) break;
            if (sel >= total) sel = total - 1;
            if (sel < 0) sel = 0;
        }
    }
    storage_save_app(&app, data_dir);
    lcd_init(SCR_TYPE_INVALID);
    app_log("shutdown", "normal exit");
    return 0;
}
