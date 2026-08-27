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
    loaded = index_load(&r->index, &r->text, r->layout, &r->app->chapter_rules, path);
    if (loaded) {
        app_log("reader", "index cache loaded");
        return 1;
    }
    app_log("reader", "index build needed");
    if (index_build(&r->index, &r->text, r->gc, r->layout, &r->app->chapter_rules,
                    progress_cb, r) <= 0) {
        debug_fail("ensure_index:index_build", "索引构建被取消，或构建失败");
        return 0;
    }
    if (!index_save(&r->index, path))
        debug_fail_errno("ensure_index:index_save", path);
    return 1;
}

static void save_reader(Reader *r) {
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
    gui_gc_finish(r->gc);
    blit_gc(r->gc);
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
    text_make_excerpt(&r->text, r->top, b->excerpt, EXCERPT_LEN);
    save_reader(r);
    app_message(r->gc, r->app, "书签", "已添加书签");
}

static int choose_list_rows(Gc gc, AppState *app, const char *title, const char **items,
                            const uint16_t *first, size_t stride, int count) {
    int sel = 0, top = 0, key, visible = 9;
    const Theme *t = &themes[app->theme];
    if (count <= 0) return -1;
    for (;;) {
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, title, 8, 6, Bold10, t->fg);
        for (int i = 0; i < visible && top + i < count; ++i) {
            int row = top + i;
            int y = 30 + i * 21;
            int color = row == sel ? t->highlight_fg : t->fg;
            if (row == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            if (items) draw_text(gc, items[row], 10, y, Regular10, color);
            else draw_u16(gc, first + (size_t)row * stride, 10, y, Regular10, color);
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
        } else if (key == 5) return sel;
        else if (key == 6) return -1;
    }
}

static int choose_list(Gc gc, AppState *app, const char *title, const char **items, int count) {
    return choose_list_rows(gc, app, title, items, NULL, 0, count);
}

static int choose_u16_list(Gc gc, AppState *app, const char *title, const uint16_t *first,
                           size_t stride, int count) {
    return choose_list_rows(gc, app, title, NULL, first, stride, count);
}

static size_t append_u16(uint16_t *out, size_t capacity, size_t length, const uint16_t *text) {
    while (length + 1 < capacity && *text) out[length++] = *text++;
    if (capacity) out[length] = 0;
    return length;
}

static int manage_bookmarks(Reader *r) {
    uint16_t labels[MAX_BOOKMARKS][72];
    uint16_t blank[16];
    char prefix[24];
    int backfilled = 0;
    int i, pick;
    if (!r->book.bookmark_count) {
        app_message(r->gc, r->app, "书签", "暂无书签");
        return 0;
    }
    utf8_to_u16("（空白）", blank, (int)(sizeof(blank) / sizeof(blank[0])));
    for (i = 0; i < (int)r->book.bookmark_count; ++i) {
        Bookmark *bookmark = &r->book.bookmarks[i];
        uint32_t percent = r->text.size ?
            (uint32_t)(((uint64_t)bookmark->position.byte_offset * 100u) / r->text.size) : 0;
        size_t length;
        if (!bookmark->excerpt[0]) {
            text_make_excerpt(&r->text, bookmark->position, bookmark->excerpt, EXCERPT_LEN);
            if (bookmark->excerpt[0]) backfilled = 1;
        }
        snprintf(prefix, sizeof(prefix), "%02d %lu%% ", i + 1, (unsigned long)percent);
        length = (size_t)utf8_to_u16(prefix, labels[i],
                                    (int)(sizeof(labels[i]) / sizeof(labels[i][0])));
        length = append_u16(labels[i], sizeof(labels[i]) / sizeof(labels[i][0]), length,
                            bookmark->excerpt[0] ? bookmark->excerpt : blank);
        (void)length;
    }
    if (backfilled) save_reader(r);
    pick = choose_u16_list(r->gc, r->app, "管理书签", labels[0],
                           sizeof(labels[0]) / sizeof(labels[0][0]), r->book.bookmark_count);
    if (pick >= 0) {
        const char *actions[] = {"跳转", "删除", "取消"};
        int ans = choose_list(r->gc, r->app, "书签操作", actions, 3);
        if (ans == 0) {
            reader_goto(r, r->book.bookmarks[pick].position);
            save_reader(r);
            return 1;
        } else if (ans == 1) {
            memmove(&r->book.bookmarks[pick], &r->book.bookmarks[pick + 1],
                    (r->book.bookmark_count - pick - 1) * sizeof(r->book.bookmarks[0]));
            r->book.bookmark_count--;
            save_reader(r);
        }
    }
    return 0;
}

static int request_u16_input(const char *title, const char *message, const uint16_t *initial,
                             uint16_t *out, size_t capacity) {
    String request_value;
    String s_title;
    String s_msg;
    String request_struct[2];
    uint16_t title_u16[64];
    uint16_t msg_u16[96];
    static const uint16_t empty[1] = {0};
    int accepted = 0;
    int no_error;
    if (!capacity) return 0;
    out[0] = 0;
    request_value = string_new();
    s_title = string_new();
    s_msg = string_new();
    request_struct[0] = s_msg;
    request_struct[1] = request_value;
    utf8_to_u16(title, title_u16, (int)(sizeof(title_u16) / sizeof(title_u16[0])));
    utf8_to_u16(message, msg_u16, (int)(sizeof(msg_u16) / sizeof(msg_u16[0])));
    string_set_utf16(s_title, (const char *)title_u16);
    string_set_utf16(s_msg, (const char *)msg_u16);
    string_set_utf16(request_value, (const char *)(initial ? initial : empty));
    no_error = _show_msgUserInput(0, request_struct, s_title->str, s_msg->str);
    if (no_error && request_value->len > 0 && request_value->str) {
        const uint16_t *source = (const uint16_t *)request_value->str;
        size_t length = 0;
        while (length < capacity && source[length]) ++length;
        if (length > 0 && length < capacity) {
            memcpy(out, source, length * sizeof(*out));
            out[length] = 0;
            accepted = 1;
        }
    }
    string_free(s_title);
    string_free(s_msg);
    string_free(request_value);
    return accepted;
}

static int query_from_user(Reader *r, uint16_t *query) {
    static const uint16_t empty[1] = {0};
    const uint16_t *initial = r->app->history_count ? r->app->history[0] : empty;
    if (request_u16_input("搜索", "输入搜索词", initial, query, QUERY_LEN)) return 1;
    if (r->app->history_count) {
        memcpy(query, r->app->history[0], sizeof(r->app->history[0]));
        return query[0] != 0;
    }
    return 0;
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
    int continuing;
    memcpy(query, r->last_query, sizeof(query));
    if (!reuse && !query_from_user(r, query)) return 0;
    memcpy(r->last_query, query, sizeof(r->last_query));
    storage_add_history(r->app, query);
    storage_save_app(r->app, r->data_dir);
    continuing = reuse && r->hit_offset;
    if (direction >= 0) {
        anchor = continuing ? r->hit_offset : r->top.byte_offset;
        if (!search_forward(r, query, next_char_offset(r, (TextPosition){anchor, r->top.source_line}), &hit)) {
            app_message(r->gc, r->app, "搜索", continuing ? TXT_SEARCH_NO_NEXT : TXT_SEARCH_NOT_FOUND);
            return 0;
        }
    } else {
        anchor = continuing ? r->hit_offset : r->top.byte_offset;
        if (!search_backward(r, query, anchor, &hit)) {
            app_message(r->gc, r->app, "搜索", continuing ? TXT_SEARCH_NO_PREV : TXT_SEARCH_NOT_FOUND);
            return 0;
        }
    }
    r->hit_offset = hit.byte_offset;
    r->search_active = 1;
    reader_goto(r, index_position_for_offset(&r->index, hit.byte_offset));
    return 1;
}

static void do_chapter_jump(Reader *r) {
    int pick;
    const char *title;
    if (!r->index.chapter_count) {
        app_message(r->gc, r->app, TXT_CHAPTER_DIRECTORY, TXT_NO_CHAPTERS);
        return;
    }
    title = r->index.chapters_truncated ? TXT_CHAPTER_DIRECTORY_LIMIT : TXT_CHAPTER_DIRECTORY;
    pick = choose_u16_list(r->gc, r->app, title, r->index.chapters[0].title,
                           CHAPTER_TITLE_LEN, (int)r->index.chapter_count);
    if (pick >= 0) reader_goto(r, r->index.chapters[pick].position);
}

static void do_jump(Reader *r) {
    const char *items[] = {"百分比", "页码", "物理行号", TXT_CHAPTER_DIRECTORY};
    int pick = choose_list(r->gc, r->app, "跳转", items, 4);
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
    } else if (pick == 2) {
        value = (int)r->top.source_line;
        if (show_1numeric_input("跳转", "", "原始物理行号", &value, 1, (int)r->index.total_source_lines))
            reader_goto(r, position_for_line_exact(r, (uint32_t)value));
    } else {
        do_chapter_jump(r);
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

static int manage_chapter_rules(Gc gc, AppState *app, const char *data_dir, Reader *reader) {
    uint16_t rows[MAX_CHAPTER_PATTERNS + 1][CHAPTER_PATTERN_LEN];
    uint16_t input[CHAPTER_PATTERN_LEN];
    static const uint16_t empty[1] = {0};
    ChapterRules before;
    int pick;
    int result;
    int changed = 0;
    uint32_t i;

    memset(rows, 0, sizeof(rows));
    utf8_to_u16(TXT_ADD_CHAPTER_RULE, rows[0], CHAPTER_PATTERN_LEN);
    for (i = 0; i < app->chapter_rules.count; ++i)
        copy_u16(rows[i + 1], CHAPTER_PATTERN_LEN, app->chapter_rules.patterns[i]);
    pick = choose_u16_list(gc, app, TXT_CHAPTER_RULES, rows[0], CHAPTER_PATTERN_LEN,
                           (int)app->chapter_rules.count + 1);
    if (pick < 0) return 0;
    before = app->chapter_rules;
    if (pick == 0) {
        if (!request_u16_input(TXT_CHAPTER_RULES, TXT_RULE_INPUT_HINT, empty,
                               input, CHAPTER_PATTERN_LEN)) return 0;
        result = chapter_rules_add(&app->chapter_rules, input);
        if (result == 1) changed = 1;
        else if (result == -1) app_message(gc, app, TXT_CHAPTER_RULES, TXT_RULE_DUPLICATE);
        else if (result == -2) app_message(gc, app, TXT_CHAPTER_RULES, TXT_RULE_FULL);
        else app_message(gc, app, TXT_CHAPTER_RULES, TXT_RULE_INVALID);
    } else {
        const char *actions[] = {"删除", "取消"};
        if (choose_list(gc, app, TXT_CHAPTER_RULES, actions, 2) == 0)
            changed = chapter_rules_remove(&app->chapter_rules, (uint32_t)pick - 1);
    }
    if (!changed) return 0;
    if (!storage_save_app(app, data_dir)) {
        app->chapter_rules = before;
        debug_fail_errno("manage_chapter_rules:save", data_dir);
        return -1;
    }
    if (reader) {
        TextPosition current = reader->top;
        index_free(&reader->index);
        if (!ensure_index(reader)) return -1;
        reader_goto(reader, current);
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
        draw_text(gc, "快捷键: B书签  F搜索  G跳转/章节", 10, 138, Regular9, t->fg);
        draw_text(gc, "章节规则: *多字符，?单字符，整行匹配", 10, 162, Regular9, t->fg);
        draw_text(gc, "支持UTF-8、GB18030、带BOM的UTF-16", 10, 186, Regular9, t->fg);
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
    TUTORIAL_SCREEN_HOME_RECENT,
    TUTORIAL_SCREEN_BROWSER,
    TUTORIAL_SCREEN_BROWSER_SELECTED,
    TUTORIAL_SCREEN_LOADING,
    TUTORIAL_SCREEN_READER,
    TUTORIAL_SCREEN_READER_PAGE2,
    TUTORIAL_SCREEN_READER_TOP,
    TUTORIAL_SCREEN_READER_END,
    TUTORIAL_SCREEN_SEARCH,
    TUTORIAL_SCREEN_SEARCH_NEXT,
    TUTORIAL_SCREEN_JUMP,
    TUTORIAL_SCREEN_JUMP_INPUT,
    TUTORIAL_SCREEN_JUMP_RESULT,
    TUTORIAL_SCREEN_BOOKMARK,
    TUTORIAL_SCREEN_BOOKMARK_LIST,
    TUTORIAL_SCREEN_BOOKMARK_ACTION,
    TUTORIAL_SCREEN_BOOKMARK_JUMP,
    TUTORIAL_SCREEN_BOOKMARK_DELETE,
    TUTORIAL_SCREEN_FILE_INFO,
    TUTORIAL_SCREEN_MENU,
    TUTORIAL_SCREEN_SETTINGS,
    TUTORIAL_SCREEN_SETTINGS_FONT,
    TUTORIAL_SCREEN_SETTINGS_THEME,
    TUTORIAL_SCREEN_SETTINGS_MARGIN,
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
    if (screen == TUTORIAL_SCREEN_HOME || screen == TUTORIAL_SCREEN_HOME_RECENT) {
        draw_text(gc, "nTexts 中文阅读器", 8, 6, Bold12, t->fg);
        draw_text(gc, "最近阅读", 8, 32, Bold10, t->fg);
        if (screen == TUTORIAL_SCREEN_HOME_RECENT) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, 52, SCREEN_W - 8, 20);
        }
        draw_text(gc, "1. tutorial_demo.txt.tns  12%", 10, 54, Regular10, t->fg);
        draw_text(gc, TXT_HOME_BROWSE_DOCS, 10, 96, Regular10, t->fg);
        draw_text(gc, TXT_SETTINGS, 10, 118, Regular10, t->fg);
        draw_text(gc, TXT_FOOTER_HOME, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_BROWSER || screen == TUTORIAL_SCREEN_BROWSER_SELECTED) {
        draw_text(gc, "Documents", 8, 6, Bold10, t->fg);
        draw_text(gc, "[目录] Books", 10, 32, Regular10, t->fg);
        if (screen == TUTORIAL_SCREEN_BROWSER_SELECTED) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, 52, SCREEN_W - 8, 20);
        }
        draw_text(gc, "tutorial_demo.txt.tns", 10, 54, Regular10, t->fg);
        draw_text(gc, "notes.txt.tns", 10, 76, Regular10, t->fg);
        draw_text(gc, TXT_FOOTER_BROWSER, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_LOADING) {
        draw_text(gc, "正在加载 tutorial_demo.txt.tns", 34, 84, Regular10, t->fg);
        draw_text(gc, "首次索引: 42%  Esc取消", 52, 108, Regular10, t->fg);
        draw_text(gc, "支持UTF-8、GB18030和带BOM的UTF-16", 30, 132, Regular9, t->fg);
        draw_text(gc, "UTF-16 自动识别大端/小端", 48, 154, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_READER || screen == TUTORIAL_SCREEN_READER_PAGE2 ||
               screen == TUTORIAL_SCREEN_READER_TOP || screen == TUTORIAL_SCREEN_READER_END ||
               screen == TUTORIAL_SCREEN_SEARCH || screen == TUTORIAL_SCREEN_SEARCH_NEXT ||
               screen == TUTORIAL_SCREEN_JUMP || screen == TUTORIAL_SCREEN_JUMP_INPUT ||
               screen == TUTORIAL_SCREEN_JUMP_RESULT || screen == TUTORIAL_SCREEN_BOOKMARK ||
               screen == TUTORIAL_SCREEN_BOOKMARK_JUMP) {
        const char *page = "1/8页  12%  UTF-8";
        draw_text(gc, "nTexts 教学演示文档", 10, 12, Bold10, t->fg);
        if (screen == TUTORIAL_SCREEN_READER_PAGE2) {
            page = "2/8页  25%  UTF-8";
            draw_wrapped_text(gc, "这是第二页。刚才按右键以后，页面已经前进。", 10, 40,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, "再按左键就会回到上一页。", 10, 86, SCREEN_W - 20, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_READER_TOP) {
            page = "1/8页  0%  UTF-8";
            draw_wrapped_text(gc, "Ctrl+上 已经把阅读位置带回开头。", 10, 40,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, "这种跳转不会丢失书签和设置。", 10, 86, SCREEN_W - 20, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_READER_END) {
            page = "8/8页  100%  UTF-8";
            draw_wrapped_text(gc, "Ctrl+下 已经跳到结尾。", 10, 40, SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, "适合快速检查文档末尾。", 10, 86, SCREEN_W - 20, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_JUMP) {
            draw_text(gc, "跳转", 8, 36, Bold10, t->fg);
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 14, 62, 116, 20);
            draw_text(gc, "1. 百分比", 18, 66, Regular10, t->fg);
            draw_text(gc, "2. 页码", 18, 90, Regular10, t->fg);
            draw_text(gc, "3. 物理行号", 18, 114, Regular10, t->fg);
            draw_text(gc, "4. 章节目录", 18, 138, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_JUMP_INPUT) {
            draw_text(gc, "跳转到百分比", 8, 42, Bold10, t->fg);
            draw_text(gc, "输入: 50", 28, 78, Regular10, t->fg);
            draw_text(gc, "确认后会跳到全书中间。", 28, 110, Regular9, t->muted);
        } else if (screen == TUTORIAL_SCREEN_JUMP_RESULT) {
            page = "4/8页  50%  UTF-8";
            draw_wrapped_text(gc, "已经跳到 50% 附近。", 10, 40, SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, "页码、百分比、物理行号和章节目录都可跳转。", 10, 82, SCREEN_W - 20, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_BOOKMARK) {
            draw_wrapped_text(gc, "已在当前位置添加书签。", 10, 40, SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, "以后可在 Menu 的管理书签里返回这里。", 10, 86, SCREEN_W - 20, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_BOOKMARK_JUMP) {
            page = "2/8页  25%  UTF-8";
            draw_wrapped_text(gc, "已经跳回刚才那个书签。", 10, 40, SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, "书签适合记住章节或重要位置。", 10, 82, SCREEN_W - 20, Regular10, t->fg);
        } else if (screen == TUTORIAL_SCREEN_SEARCH || screen == TUTORIAL_SCREEN_SEARCH_NEXT) {
            const char *hit = screen == TUTORIAL_SCREEN_SEARCH ? "找到的内容会这样标出来。" : "这里是下一处结果，页面已经跟着跳过去了。";
            page = screen == TUTORIAL_SCREEN_SEARCH ? "3/8页  搜索中" : "5/8页  搜索中";
            draw_wrapped_text(gc, "输入要找的词，nTexts 会跳到第一个结果。", 10, 40, SCREEN_W - 20, Regular10, t->fg);
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 8, 66, SCREEN_W - 16, 22);
            draw_text(gc, hit, 10, 68, Regular10, t->highlight_fg);
            draw_text(gc, "Enter/下 找下一处", 6, SCREEN_H - 28, Regular9, t->muted);
            draw_text(gc, "上 找上一处  Esc退出", 6, SCREEN_H - 14, Regular9, t->muted);
        } else {
            draw_wrapped_text(gc, "按快捷键可以搜索、跳转、书签和打开菜单。", 10, 68,
                              SCREEN_W - 20, Regular10, t->fg);
        }
        if (screen != TUTORIAL_SCREEN_SEARCH && screen != TUTORIAL_SCREEN_SEARCH_NEXT)
            draw_text(gc, page, 6, SCREEN_H - 15, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_BOOKMARK_LIST || screen == TUTORIAL_SCREEN_BOOKMARK_ACTION ||
               screen == TUTORIAL_SCREEN_BOOKMARK_DELETE) {
        draw_text(gc, screen == TUTORIAL_SCREEN_BOOKMARK_ACTION ? "书签操作" : "管理书签", 8, 6, Bold10, t->fg);
        if (screen == TUTORIAL_SCREEN_BOOKMARK_LIST) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, 34, SCREEN_W - 8, 20);
            draw_text(gc, "01 25%  教学演示文档第二章", 10, 36, Regular10, t->fg);
            draw_text(gc, TXT_FOOTER_LIST, 8, FOOTER_Y, Regular9, t->muted);
        } else if (screen == TUTORIAL_SCREEN_BOOKMARK_ACTION) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, 34, SCREEN_W - 8, 20);
            draw_text(gc, "1. 跳转", 10, 36, Regular10, t->fg);
            draw_text(gc, "2. 删除", 10, 58, Regular10, t->fg);
            draw_text(gc, "3. 取消", 10, 80, Regular10, t->fg);
            draw_text(gc, TXT_FOOTER_LIST, 8, FOOTER_Y, Regular9, t->muted);
        } else {
            draw_text(gc, "书签已删除", 10, 54, Regular10, t->fg);
            draw_text(gc, "这里暂时没有书签。", 10, 84, Regular10, t->muted);
            draw_text(gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
        }
    } else if (screen == TUTORIAL_SCREEN_FILE_INFO) {
        draw_text(gc, "文件信息", 10, 8, Bold12, t->fg);
        draw_text(gc, "名称: tutorial_demo.txt.tns", 10, 42, Regular10, t->fg);
        draw_text(gc, "路径:", 10, 68, Regular10, t->fg);
        draw_wrapped_text(gc, "/documents/nTexts/tutorial_demo.txt.tns", 10, 90, SCREEN_W - 20, Regular9, t->fg);
        draw_text(gc, "大小: 2048 字节", 10, 128, Regular9, t->fg);
        draw_text(gc, "编码: UTF-8", 10, 146, Regular9, t->fg);
        draw_text(gc, "行数: 48", 10, 164, Regular9, t->fg);
        draw_text(gc, "页数: 8", 10, 182, Regular9, t->fg);
        draw_text(gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_MENU) {
        const char *items[] = {"1. 添加书签", "2. 管理书签", "3. 搜索", "4. 向后查找",
                               "5. 向前查找", "6. 跳转", "7. 阅读设置", "8. 文件信息", "9. 返回书库"};
        draw_text(gc, "功能菜单", 8, 6, Bold10, t->fg);
        for (int i = 0; i < 9; ++i) draw_text(gc, items[i], 10, 30 + i * 19, Regular9, t->fg);
        draw_text(gc, TXT_FOOTER_LIST, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_SETTINGS || screen == TUTORIAL_SCREEN_SETTINGS_FONT ||
               screen == TUTORIAL_SCREEN_SETTINGS_THEME || screen == TUTORIAL_SCREEN_SETTINGS_MARGIN) {
        draw_text(gc, "阅读设置", 8, 6, Bold12, t->fg);
        draw_text(gc, screen == TUTORIAL_SCREEN_SETTINGS_FONT ? "1. 字号: 小/中/大  当前:大" : "1. 字号: 小/中/大  当前:中", 10, 32, Regular10, t->fg);
        draw_text(gc, screen == TUTORIAL_SCREEN_SETTINGS_THEME ? "2. 主题: 浅色/深色/护眼  当前:护眼" : "2. 主题: 浅色/深色/护眼  当前:浅色", 10, 54, Regular10, t->fg);
        draw_text(gc, screen == TUTORIAL_SCREEN_SETTINGS_MARGIN ? "3. 边距: 窄/宽  当前:宽" : "3. 边距: 窄/宽  当前:窄", 10, 76, Regular10, t->fg);
        draw_text(gc, "4. 章节规则: 自定义0条", 10, 98, Regular10, t->fg);
        draw_text(gc, "5. 关于", 10, 120, Regular10, t->fg);
        draw_text(gc, "6. 使用说明", 10, 142, Regular10, t->fg);
        draw_text(gc, "7. 重置教学进度", 10, 164, Regular10, t->fg);
    } else {
        draw_text(gc, "传入自己的文档", 8, 8, Bold12, t->fg);
        draw_text(gc, "1. 将 nTexts.tns 传入计算器", 10, 42, Regular9, t->fg);
        draw_text(gc, "2. 把你的文本放入 Documents", 10, 66, Regular9, t->fg);
        draw_text(gc, "3. 也可以放进子文件夹整理", 10, 90, Regular9, t->fg);
        draw_text(gc, "4. 回到 nTexts 浏览并打开", 10, 114, Regular9, t->fg);
    }
}

static void tutorial_center_page(Gc gc, AppState *app, const char *data_dir) {
    const Theme *t = &themes[app->theme];
    const TutorialStep steps[] = {
        {TUTORIAL_SCREEN_TRANSFER, "安装和传书", "先把 nTexts.tns 传进计算器。", "再把想看的文本放进 Documents。", "现在按 Enter 继续。", 5, 0, 8, 34, 304, 104},
        {TUTORIAL_SCREEN_HOME, "主页", "这里显示最近阅读、浏览文档和设置。", "按 0 直接进入 Documents 浏览器。", "请按 0。", 12, 0, 6, 92, 180, 22},
        {TUTORIAL_SCREEN_BROWSER, "浏览文档", "浏览器只显示目录、.txt 和 .txt.tns。", "上下移动光标。", "请按下键选择文件。", 2, 0, 6, 50, 220, 24},
        {TUTORIAL_SCREEN_BROWSER_SELECTED, "进入或打开", "选中文件夹时 Enter 进入。", "选中文本文件时 Enter 打开。", "请按 Enter 打开演示文档。", 5, 0, 6, 50, 220, 24},
        {TUTORIAL_SCREEN_LOADING, "加载和索引", "支持 UTF-8、GB18030 和带BOM的 UTF-16。", "UTF-16 大端/小端会自动识别。", "请按 Enter 继续。", 5, 0, 42, 96, 236, 64},
        {TUTORIAL_SCREEN_READER, "阅读正文", "这是阅读区。", "左右翻页，上下按原文本物理行移动。", "请按右键翻到下一页。", 4, 0, 6, 8, 308, 176},
        {TUTORIAL_SCREEN_READER_PAGE2, "返回上一页", "阅读进度将会保存，", "重新进入将会自动续读。", "请按左键。", 3, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "快捷跳到开头", "先回到了第一页。", "Ctrl+上 会跳到整本书开头。", "请按 Ctrl+上。", 1, 1, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER_TOP, "快捷跳到结尾", "现在已经在开头。", "Ctrl+下 会跳到整本书结尾。", "请按 Ctrl+下。", 2, 1, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER_END, "搜索快捷键", "已经跳到结尾。", "按 F 搜索文字，找到后会直接跳过去并标出来。", "请按 F。", 9, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_SEARCH, "继续找下一个", "现在已经找到一处。", "按 Enter 或下键继续找下一个，上键回到上一个。", "请按 Enter 找下一处。", 5, 0, 6, 62, 308, 30},
        {TUTORIAL_SCREEN_SEARCH_NEXT, "回到上一个结果", "页面已经跳到下一处结果。", "如果跳过头了，按上键回到上一处。", "请按上键。", 1, 0, 6, 62, 308, 30},
        {TUTORIAL_SCREEN_READER, "跳转快捷键", "G 打开跳转。", "可按百分比、页码、物理行号或章节目录跳转。", "请按 G。", 10, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_JUMP, "跳转菜单", "先选择跳转方式。", "这里选百分比。", "请按 1。", 21, 0, 14, 62, 116, 20},
        {TUTORIAL_SCREEN_JUMP_INPUT, "输入跳转位置", "输入 50，表示跳到全书中间。", "确认后就会过去。", "请按 Enter。", 5, 0, 24, 72, 180, 48},
        {TUTORIAL_SCREEN_JUMP_RESULT, "已经跳转", "现在到了 50% 附近。", "实际阅读时会回到对应页面。", "请按 Enter 继续。", 5, 0, 6, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, "书签快捷键", "B 可以直接添加书签。", "Menu 里可以管理书签。", "请按 B。", 8, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_BOOKMARK, "书签已保存", "当前位置已经保存为书签。", "以后可以从管理书签跳回来。", "请按 Enter 继续。", 5, 0, 6, 38, 308, 68},
        {TUTORIAL_SCREEN_READER, "打开功能菜单", "Menu 打开完整功能菜单。", "下面会逐项练习菜单。", "请按 Menu。", 7, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 1: 添加书签", "添加当前位置书签。", "也可以直接按 B。", "请按 1。", 21, 0, 6, 28, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 2: 管理书签", "这里可以回到书签，", "也可以删掉不用的书签。", "请按 2。", 22, 0, 6, 47, 170, 20},
        {TUTORIAL_SCREEN_BOOKMARK_LIST, "选择书签", "先选一个书签。", "按 Enter 打开操作菜单。", "请按 Enter。", 5, 0, 6, 32, 170, 24},
        {TUTORIAL_SCREEN_BOOKMARK_ACTION, "跳到书签", "选“跳转”会回到这个位置。", "这和真正阅读时一样。", "请按 1。", 21, 0, 6, 32, 130, 24},
        {TUTORIAL_SCREEN_BOOKMARK_JUMP, "已经跳到书签", "页面回到了书签位置。", "现在再演示删除书签。", "请按 Menu。", 7, 0, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_MENU, "再次打开书签", "回到菜单后再次进入管理书签。", "这次删掉刚才的书签。", "请按 2。", 22, 0, 6, 47, 170, 20},
        {TUTORIAL_SCREEN_BOOKMARK_LIST, "选择要删除的书签", "还是先选中书签。", "按 Enter 打开操作菜单。", "请按 Enter。", 5, 0, 6, 32, 170, 24},
        {TUTORIAL_SCREEN_BOOKMARK_ACTION, "删除书签", "在操作菜单里选“删除”。", "删除后正文不会受影响。", "请按 2。", 22, 0, 6, 54, 130, 24},
        {TUTORIAL_SCREEN_BOOKMARK_DELETE, "书签已删除", "书签列表已经清空。", "以后也可以这样整理书签。", "请按 Enter 继续。", 5, 0, 6, 52, 220, 48},
        {TUTORIAL_SCREEN_MENU, "菜单 3: 搜索", "输入想找的字词。", "找到后会跳到对应位置并标出来。", "请按 3。", 23, 0, 6, 66, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 4/5: 继续查找", "4 继续找下一个，5 回到上一个。", "会沿用刚才输入的搜索词。", "请按 4。", 24, 0, 6, 85, 190, 40},
        {TUTORIAL_SCREEN_MENU, "菜单 6: 跳转", "按百分比、页码、物理行号或章节目录跳转。", "也可以直接按 G。", "请按 6。", 26, 0, 6, 123, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 7: 阅读设置", "调整字号、主题和边距。", "设置变化会重建当前书索引。", "请按 7。", 27, 0, 6, 142, 170, 20},
        {TUTORIAL_SCREEN_MENU, "菜单 8: 文件信息", "这里能看到文件名、路径、编码和页数。", "需要确认文件时很有用。", "请按 8。", 28, 0, 6, 161, 170, 20},
        {TUTORIAL_SCREEN_FILE_INFO, "查看文件信息", "这就是文件信息页。", "看完按 Enter 或 Esc 返回。", "请按 Enter。", 5, 0, 8, 8, 304, 198},
        {TUTORIAL_SCREEN_MENU, "菜单 9: 返回书库", "9 返回书库。", "返回书库也可以直接按 Esc。", "请按 9。", 29, 0, 6, 180, 170, 20},
        {TUTORIAL_SCREEN_HOME_RECENT, "从最近阅读进入", "打开过的书会出现在最近阅读。", "选中最近阅读后 Enter 继续阅读。", "请按 Enter。", 5, 0, 6, 50, 240, 24},
        {TUTORIAL_SCREEN_HOME, "打开阅读设置", "主页按 (-) 打开阅读设置。", "也可在阅读菜单中打开。", "请按 (-)。", 13, 0, 6, 114, 180, 24},
        {TUTORIAL_SCREEN_SETTINGS, "更改字号", "设置里按 1 或 Enter/左右切换字号。", "大字号会重新分页。", "请按 1。", 21, 0, 6, 28, 292, 22},
        {TUTORIAL_SCREEN_SETTINGS_FONT, "更改主题", "字号已经切换。", "按 2 切换浅色、深色和护眼主题。", "请按 2。", 22, 0, 6, 50, 292, 22},
        {TUTORIAL_SCREEN_SETTINGS_THEME, "更改边距", "主题已经切换。", "按 3 切换窄边距和宽边距。", "请按 3。", 23, 0, 6, 72, 292, 22},
        {TUTORIAL_SCREEN_SETTINGS_MARGIN, "章节规则和教学重置", "第4项可添加全局章节规则，*匹配多个字符，?匹配一个。", "第7项可让下次启动重新教学。", "请按 Enter 完成教学。", 5, 0, 6, 94, 292, 94}
    };
    int count = (int)(sizeof(steps) / sizeof(steps[0]));
    for (int i = 0; i < count;) {
        int key, ctrl;
        char footer[96];
        int panel_y;
        const TutorialStep *step = &steps[i];
        app_log("tutorial", "step %d/%d title=%s expect=%s ctrl=%d",
                i + 1, count, step->title, tutorial_key_label(step), step->need_ctrl);
        panel_y = step->y > 118 ? 18 : 136;
        gui_gc_begin(gc);
        tutorial_mock_screen(gc, t, step->screen);
        fill_dim_outside(gc, step->x, step->y, step->w, step->h);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 8, panel_y, SCREEN_W - 16, 76);
        draw_text(gc, step->title, 14, panel_y + 6, Bold10, t->fg);
        draw_text(gc, step->body1, 14, panel_y + 24, Regular9, t->fg);
        draw_text(gc, step->body2, 14, panel_y + 42, Regular9, t->fg);
        snprintf(footer, sizeof(footer), "%d/%d  按%s继续  0跳过",
                 i + 1, count, tutorial_key_label(step));
        draw_text(gc, footer, 14, panel_y + 60, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);

        key = wait_key();
        ctrl = isKeyPressed(KEY_NSPIRE_CTRL);
        wait_release();
        app_log("tutorial", "key step=%d got=%d ctrl=%d expect=%d need_ctrl=%d",
                i + 1, key, ctrl, step->key, step->need_ctrl);
        if (key == 12 && step->key != 12) {
            app->tutorial_flags = TUTORIAL_ALL_SKIPPED | TUTORIAL_READER_SEEN;
            storage_save_app(app, data_dir);
            app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_SKIP_DONE);
            return;
        }
        if (key == step->key && (!step->need_ctrl || ctrl)) {
            i++;
            app_log("tutorial", "advance next=%d/%d", i + 1, count);
        }
    }
}

static int run_settings_item(Gc gc, AppState *app, const char *data_dir, Reader *r,
                             int selection, int delta) {
    if (selection < 3) return apply_setting_change(r, app, data_dir, selection, delta);
    if (selection == 3) return manage_chapter_rules(gc, app, data_dir, r);
    if (selection == 4) about_page(gc, app);
    else if (selection == 5) usage_page(gc, app);
    else {
        app->tutorial_flags = 0;
        storage_save_app(app, data_dir);
        app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_RESET_DONE);
    }
    return 1;
}

static int settings_menu(Gc gc, AppState *app, const char *data_dir, Reader *r) {
    const Theme *t;
    const char *font_labels[] = {"小", "中", "大"};
    const char *theme_labels[] = {"浅色", "深色", "护眼"};
    const char *margin_labels[] = {"窄", "宽"};
    char items[7][96];
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
        snprintf(items[3], sizeof(items[3]), "4. 章节规则: 自定义%lu条",
                 (unsigned long)app->chapter_rules.count);
        snprintf(items[4], sizeof(items[4]), "5. 关于");
        snprintf(items[5], sizeof(items[5]), "6. 使用说明");
        snprintf(items[6], sizeof(items[6]), "7. 重置教学进度");
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, "阅读设置", 8, 6, Bold12, t->fg);
        for (int i = 0; i < 7; ++i) {
            int y = 34 + i * 25;
            if (i == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            draw_text(gc, items[i], 10, y, Regular10, i == sel ? t->highlight_fg : t->fg);
        }
        draw_text(gc, "1-7直达  左右/Enter执行  Esc返回", 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        blit_gc(gc);
        key = wait_key(); wait_release();
        if (key == 1) sel = sel > 0 ? sel - 1 : 6;
        else if (key == 2) sel = sel < 6 ? sel + 1 : 0;
        else if (key >= 21 && key <= 27) {
            sel = key - 21;
            if (run_settings_item(gc, app, data_dir, r, sel, 1) < 0) return -1;
        } else if (key == 3) {
            if (run_settings_item(gc, app, data_dir, r, sel, -1) < 0) return -1;
        } else if (key == 4 || key == 5) {
            if (run_settings_item(gc, app, data_dir, r, sel, 1) < 0) return -1;
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

static int open_reader(const char *path, Gc gc, AppState *app, const char *data_dir) {
    Reader r;
    int key, menu_res;
    app_log("reader", "open begin %s", path ? path : "(null)");
    memset(&r, 0, sizeof(r));
    r.gc = gc;
    r.app = app;
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
        index_free(&r.index);
        text_close(&r.text);
        return 0;
    }
    reader_goto(&r, r.book.progress.byte_offset ? r.book.progress : r.index.pages[0]);
    app_log("reader", "loop begin page_count=%lu start_offset=%lu",
            (unsigned long)r.index.page_count, (unsigned long)r.top.byte_offset);
    for (;;) {
        draw_page(&r);
        key = wait_key(); wait_release();
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

static void run_tutorial_if_needed(Gc gc, AppState *app, const char *data_dir) {
    if (app->tutorial_flags & TUTORIAL_ALL_SKIPPED) return;
    if (app->tutorial_flags & TUTORIAL_READER_SEEN) return;
    app_log("tutorial", "start");
    tutorial_center_page(gc, app, data_dir);
    if (app->tutorial_flags & TUTORIAL_ALL_SKIPPED) return;
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
        open_reader(argv[1], gc, &app, data_dir);
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
                if (browse_files(gc, &app, selected, sizeof(selected))) open_reader(selected, gc, &app, data_dir);
            } else if (key == 13) {
                settings_menu(gc, &app, data_dir, NULL);
            }
            else if (key == 5) {
                if (sel < shown_recents) {
                    char recent_path[512];
                    snprintf(recent_path, sizeof(recent_path), "%s", app.recents[sel].path);
                    app_log("home", "open recent[%d]=%s", sel, recent_path);
                    if (!open_reader(recent_path, gc, &app, data_dir)) {
                        storage_remove_recent(&app, recent_path);
                        storage_save_app(&app, data_dir);
                    }
                }
                else if (sel == shown_recents) {
                    memset(selected, 0, sizeof(selected));
                    if (browse_files(gc, &app, selected, sizeof(selected))) open_reader(selected, gc, &app, data_dir);
                } else {
                    settings_menu(gc, &app, data_dir, NULL);
                }
            } else if (key == 7) {
                memset(selected, 0, sizeof(selected));
                if (browse_files(gc, &app, selected, sizeof(selected))) open_reader(selected, gc, &app, data_dir);
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
