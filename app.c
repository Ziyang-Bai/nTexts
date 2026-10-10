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

#ifndef NTEXTS_BUILD_VERSION
#define NTEXTS_BUILD_VERSION "Beta Testing v0.0.1"
#endif

#ifndef NTEXTS_BUILD_COMMIT
#define NTEXTS_BUILD_COMMIT "local"
#endif

#define SCREEN_W 320
#define SCREEN_H 240
#define DIRECTORY_INITIAL_CAPACITY 32
#define MAX_DIRECTORY_ITEMS 4096
#define HOME_VISIBLE_ROWS 8
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
    int ascii_width[128];
    uint16_t wide_chars[256];
    int wide_width[256];
    TextPosition top;
    uint32_t page;
    uint32_t hit_offset;
    uint32_t hit_end;
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
static int last_key_ctrl;
static uint32_t app_locale;
static const char build_version[] = NTEXTS_BUILD_VERSION;
static const char build_commit[] = NTEXTS_BUILD_COMMIT;

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
    gui_gc_blit_to_screen(gc);
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
    debug_error_page(L("未知错误", "Unexpected error", "Erreur inattendue"), debug_state.where, debug_state.detail, 0);
}

static void debug_fail_errno(const char *where, const char *what) {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s (errno=%d: %s)", what, errno, strerror(errno));
    debug_fail(where, "%s", buf);
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
    snprintf(meta, sizeof(meta), L("Ndless %u  HW subtype %u  lcd %d", "Ndless %u  HW subtype %u  lcd %d", "Ndless %u  mat. %u  écran %d"),
             nl_ndless_rev(), nl_hwsubtype(), (int)screen_type);
    gui_gc_begin(gc);
    set_color(gc, 0x2b0d0d);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    set_color(gc, 0x922f2f);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, 28);
    draw_text(gc, head, 10, 6, Bold10, 0xffffff);
    draw_text(gc, fatal ? L("程序已进入致命错误处理", "Fatal error", "Erreur fatale") :
              L("程序捕获到错误", "An error occurred", "Une erreur est survenue"), 10, 38, Regular10, 0xffffff);
    draw_text(gc, L("位置:", "Where:", "Emplacement :"), 10, 70, Bold10, 0xffd8d8);
    (void)draw_wrapped_text(gc, where ? where : L("未知", "unknown", "inconnu"), 10, 88, SCREEN_W - 20, Regular10, 0xffffff);
    draw_text(gc, L("详情:", "Details:", "Détails :"), 10, 122, Bold10, 0xffd8d8);
    (void)draw_wrapped_text(gc, detail ? detail : L("无", "none", "aucun"), 10, 140, SCREEN_W - 20, Regular10, 0xffffff);
    draw_text(gc, meta, 10, 198, Regular9, 0xf0c0c0);
    draw_text(gc, fatal ? TXT_ESC_OR_ENTER_STAY : TXT_ESC_OR_ENTER_BACK, 10, 216, Regular9, t->muted);
    gui_gc_finish(gc);
    gui_gc_blit_to_screen(gc);
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
    snprintf(debug_state.detail, sizeof(debug_state.detail),
             L("捕获到运行时信号 %s (%d)", "Runtime signal %s (%d)", "Signal reçu %s (%d)"), name, sig);
    debug_error_page(L("致命错误", "Fatal error", "Erreur fatale"), debug_state.where, debug_state.detail, 1);
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

static int filename_matches(const char *name, const uint16_t *filter) {
    uint16_t decoded[512];
    int length = utf8_to_u16(name, decoded, (int)(sizeof(decoded) / sizeof(decoded[0])));
    int start;
    for (start = 0; start < length; ++start) {
        int i = 0;
        while (filter[i] && decoded[start + i] &&
               text_fold_latin(decoded[start + i]) == text_fold_latin(filter[i])) ++i;
        if (!filter[i]) return 1;
    }
    return 0;
}

static int load_dir(const char *dir, const uint16_t *filter, FileItem **out_items, int *count) {
    DIR *dp = opendir(dir);
    FileItem *items = NULL;
    struct dirent *de;
    int capacity = 0;
    *out_items = NULL;
    *count = 0;
    if (!dp) {
        debug_fail_errno("load_dir:opendir", dir);
        return 0;
    }
    while ((de = readdir(dp))) {
        char path[512];
        struct stat st;
        FileItem *next;
        int next_capacity;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!join_path(path, sizeof(path), dir, de->d_name)) continue;
        if (stat(path, &st)) continue;
        if (!S_ISDIR(st.st_mode) && !has_txt_ext(de->d_name)) continue;
        if (!S_ISDIR(st.st_mode) && filter[0] && !filename_matches(de->d_name, filter)) continue;
        if (*count >= MAX_DIRECTORY_ITEMS) continue;
        if (*count == capacity) {
            next_capacity = capacity ? capacity * 2 : DIRECTORY_INITIAL_CAPACITY;
            if (next_capacity > MAX_DIRECTORY_ITEMS) next_capacity = MAX_DIRECTORY_ITEMS;
            next = realloc(items, (size_t)next_capacity * sizeof(*next));
            if (!next) {
                int saved_errno = errno ? errno : ENOMEM;
                free(items);
                closedir(dp);
                errno = saved_errno;
                debug_fail_errno("load_dir:realloc", dir);
                return 0;
            }
            items = next;
            capacity = next_capacity;
        }
        snprintf(items[*count].path, sizeof(items[*count].path), "%s", path);
        snprintf(items[*count].name, sizeof(items[*count].name), "%s", de->d_name);
        items[*count].is_dir = S_ISDIR(st.st_mode);
        (*count)++;
    }
    closedir(dp);
    if (*count > 1) qsort(items, (size_t)*count, sizeof(items[0]), cmp_items);
    *out_items = items;
    return 1;
}

static void wait_release(void) {
    while (any_key_pressed()) msleep(20);
}

static int wait_key(void) {
    for (;;) {
        last_key_ctrl = isKeyPressed(KEY_NSPIRE_CTRL);
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

static Layout layout_from_state(const AppState *state) {
    const int fonts[] = {Regular9, Regular10, Regular12};
    const int margins[] = {6, 14};
    const int line_heights[] = {20, 24, 28};
    Layout l;
    int fi = state->font_choice < 0 || state->font_choice > 2 ? 1 : state->font_choice;
    int mi = state->margin_choice ? 1 : 0;
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
    snprintf(msg, sizeof(msg), L("首次索引: %lu%%  Esc取消", "Indexing: %lu%%  Esc cancel", "Indexation : %lu%%  Esc annuler"),
             total ? (unsigned long)(((uint64_t)done * 100u) / total) : 0ul);
    draw_text(r->gc, msg, 42, 104, Regular10, themes[r->app->theme].fg);
    gui_gc_finish(r->gc);
    gui_gc_blit_to_screen(r->gc);
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
        debug_fail("ensure_index:index_build", "%s", L("索引构建被取消，或构建失败", "Index canceled or failed", "Indexation annulée ou échouée"));
        return 0;
    }
    if (!index_save(&r->index, path))
        debug_fail_errno("ensure_index:index_save", path);
    return 1;
}

static int save_reader(Reader *r) {
    r->book.progress = r->top;
    if (!storage_save_book(&r->book, r->data_dir)) {
        debug_fail_errno("save_reader:book", r->book.path);
        return 0;
    }
    storage_touch_recent(r->app, &r->book);
    return 1;
}

static void save_reader_all(Reader *r) {
    save_reader(r);
    if (!storage_save_app(r->app, r->data_dir))
        debug_fail_errno("save_reader:app", r->data_dir);
}

static uint32_t percent_for_reader(const Reader *r) {
    if (!r->text.size) return 0;
    return (uint32_t)(((uint64_t)r->top.byte_offset * 100u) / r->text.size);
}

typedef struct {
    int left, right;
    int first, last;
} HitSpan;

static int wrap_push(uint16_t lines[][NTEXTS_MAX_LINE_U16], HitSpan spans[],
                     int *line, const uint16_t *buf, int len, HitSpan span) {
    if (*line >= 32) return 0;
    memcpy(lines[*line], buf, (len + 1) * sizeof(uint16_t));
    spans[*line] = span;
    (*line)++;
    return 1;
}

static int reader_char_width(Reader *r, uint16_t ch) {
    int width;
    if (ch < 128) {
        width = r->ascii_width[ch];
    } else {
        unsigned slot = ch & 255u;
        width = r->wide_chars[slot] == ch ? r->wide_width[slot] : 0;
    }
    if (width) return width;
    width = gui_gc_getCharWidth(r->gc, (gui_gc_Font)r->layout.font_size, (short)ch);
    if (width <= 0) width = r->layout.font_size & 31;
    if (ch < 128) {
        r->ascii_width[ch] = width;
    } else {
        unsigned slot = ch & 255u;
        r->wide_chars[slot] = ch;
        r->wide_width[slot] = width;
    }
    return width;
}

static void draw_page(Reader *r) {
    const Theme *t = &themes[r->app->theme];
    TextDecoder d;
    uint16_t lines[32][NTEXTS_MAX_LINE_U16], linebuf[NTEXTS_MAX_LINE_U16], ch;
    HitSpan spans[32], span = {-1, 0, 0, 0};
    int line = 0, len = 0, width = 0, maxw = SCREEN_W - r->layout.margin * 2;
    uint32_t off;
    char status[96];
    text_decoder_at(&r->text, &d, r->top);
    while (line < r->layout.lines_per_page && text_next(&d, &ch, &off)) {
        int cw;
        if (ch == '\r') continue;
        if (ch == '\n') {
            linebuf[len] = 0;
            wrap_push(lines, spans, &line, linebuf, len, span);
            span = (HitSpan){-1, 0, 0, 0};
            len = width = 0;
            continue;
        }
        if (ch == '\t') ch = ' ';
        if (ch < 32) continue;
        cw = reader_char_width(r, ch);
        if (len && width + cw > maxw) {
            linebuf[len] = 0;
            wrap_push(lines, spans, &line, linebuf, len, span);
            span = (HitSpan){-1, 0, 0, 0};
            len = width = 0;
            if (line >= r->layout.lines_per_page) break;
        }
        if (len + 1 < NTEXTS_MAX_LINE_U16) {
            if (r->search_active && off >= r->hit_offset && off < r->hit_end) {
                if (span.left < 0) {
                    span.left = width;
                    span.first = len;
                }
                span.right = width + cw;
                span.last = len + 1;
            }
            linebuf[len++] = ch;
        }
        width += cw;
    }
    if (line < r->layout.lines_per_page && len) {
        linebuf[len] = 0;
        wrap_push(lines, spans, &line, linebuf, len, span);
    }

    gui_gc_begin(r->gc);
    set_color(r->gc, t->bg);
    gui_gc_fillRect(r->gc, 0, 0, SCREEN_W, SCREEN_H);
    for (int i = 0; i < line; ++i) {
        int y = r->layout.margin + i * r->layout.line_height;
        draw_u16(r->gc, lines[i], r->layout.margin, y, (gui_gc_Font)r->layout.font_size, t->fg);
        if (spans[i].left >= 0) {
            uint16_t saved = lines[i][spans[i].last];
            set_color(r->gc, t->highlight_bg);
            gui_gc_fillRect(r->gc, r->layout.margin + spans[i].left - 1, y - 1,
                            spans[i].right - spans[i].left + 2, r->layout.line_height);
            lines[i][spans[i].last] = 0;
            draw_u16(r->gc, lines[i] + spans[i].first, r->layout.margin + spans[i].left, y,
                     (gui_gc_Font)r->layout.font_size, t->highlight_fg);
            lines[i][spans[i].last] = saved;
        }
    }
    set_color(r->gc, t->muted);
    gui_gc_drawLine(r->gc, 0, SCREEN_H - 18, SCREEN_W, SCREEN_H - 18);
    if (r->search_active) {
        snprintf(status, sizeof(status), "%s", TXT_SEARCH_MODE_HINT);
    } else {
        snprintf(status, sizeof(status), L("%lu/%lu页  %lu%%  %s", "%lu/%lu pages  %lu%%  %s", "%lu/%lu pages  %lu%%  %s"),
                 (unsigned long)(r->page + 1), (unsigned long)r->index.page_count,
                 (unsigned long)percent_for_reader(r), text_encoding_name(r->text.encoding));
    }
    draw_text(r->gc, status, 6, SCREEN_H - 15, Regular9, t->muted);
    gui_gc_finish(r->gc);
    gui_gc_blit_to_screen(r->gc);
}

static void reader_goto(Reader *r, TextPosition pos) {
    if (pos.byte_offset >= r->index.last_visible_end)
        pos = index_position_for_page(&r->index, r->index.page_count - 1);
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
        app_message(r->gc, r->app, L("书签", "Bookmarks", "Signets"), L("书签已满，请先删除", "Bookmark list full; delete one first", "Signets pleins ; supprimez-en un"));
        return;
    }
    b = &r->book.bookmarks[r->book.bookmark_count++];
    memset(b, 0, sizeof(*b));
    b->position = r->top;
    b->timestamp = (uint32_t)time(NULL);
    text_make_excerpt(&r->text, r->top, b->excerpt, EXCERPT_LEN);
    save_reader(r);
    app_message(r->gc, r->app, L("书签", "Bookmarks", "Signets"), L("已添加书签", "Bookmark added", "Signet ajouté"));
}

static int choose_list_rows(Gc gc, AppState *app, const char *title, const char **items,
                            const uint16_t *first, size_t stride, int count, int zero_pick) {
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
        draw_text(gc, zero_pick >= 0 ?
                  L("0编码  上下滚动  Enter选择  Esc返回", "0 encoding  Up/Down  Enter open  Esc back", "0 encodage  Haut/Bas  Enter ouvrir  Esc retour") :
                  count > visible ? L("上下滚动  Enter选择  Esc返回", "Up/Down scroll  Enter open  Esc back", "Haut/Bas défiler  Enter ouvrir  Esc retour") :
                  TXT_FOOTER_LIST, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        gui_gc_blit_to_screen(gc);
        key = wait_key(); wait_release();
        if (key >= 21 && key <= 29) {
            int pick = key - 21;
            if (pick < count) return pick;
        }
        if (key == 12 && zero_pick >= 0) return zero_pick;
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
    return choose_list_rows(gc, app, title, items, NULL, 0, count, -1);
}

static int choose_u16_list(Gc gc, AppState *app, const char *title, const uint16_t *first,
                           size_t stride, int count) {
    return choose_list_rows(gc, app, title, NULL, first, stride, count, -1);
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
        app_message(r->gc, r->app, L("书签", "Bookmarks", "Signets"), L("暂无书签", "No bookmarks", "Aucun signet"));
        return 0;
    }
    utf8_to_u16(L("（空白）", "(blank)", "(vide)"), blank, (int)(sizeof(blank) / sizeof(blank[0])));
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
    pick = choose_u16_list(r->gc, r->app, L("管理书签", "Bookmarks", "Signets"), labels[0],
                           sizeof(labels[0]) / sizeof(labels[0][0]), r->book.bookmark_count);
    if (pick >= 0) {
        const char *actions[] = {L("跳转", "Go to", "Aller à"), L("删除", "Delete", "Supprimer"), L("取消", "Cancel", "Annuler")};
        int ans = choose_list(r->gc, r->app, L("书签操作", "Bookmark action", "Action sur le signet"), actions, 3);
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
    wait_release();
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
    return request_u16_input(L("搜索", "Search", "Recherche"), L("输入搜索词", "Search for", "Texte à chercher"), initial, query, QUERY_LEN);
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
    uint32_t hit;
    uint16_t query[QUERY_LEN], value;
    TextDecoder match;
    uint32_t anchor, off;
    int continuing;
    memcpy(query, r->last_query, sizeof(query));
    if (reuse && !query[0] && r->app->history_count)
        memcpy(query, r->app->history[0], sizeof(query));
    if (!reuse && !query_from_user(r, query)) return 0;
    if (!query[0]) return 0;
    memcpy(r->last_query, query, sizeof(r->last_query));
    if (!reuse) {
        storage_add_history(r->app, query);
        if (!storage_save_app(r->app, r->data_dir))
            debug_fail_errno("do_search:save_history", r->data_dir);
    }
    continuing = reuse && r->search_active;
    if (direction >= 0) {
        uint32_t from = continuing ?
            next_char_offset(r, (TextPosition){r->hit_offset, r->top.source_line}) :
            r->top.byte_offset;
        if (!text_find_forward(&r->text, query, from, &hit)) {
            app_message(r->gc, r->app, L("搜索", "Search", "Recherche"), continuing ? TXT_SEARCH_NO_NEXT : TXT_SEARCH_NOT_FOUND);
            return 0;
        }
    } else {
        anchor = continuing ? r->hit_offset : r->top.byte_offset;
        if (!text_find_backward(&r->text, query, anchor, r->index.pages, r->index.page_count, &hit)) {
            app_message(r->gc, r->app, L("搜索", "Search", "Recherche"), continuing ? TXT_SEARCH_NO_PREV : TXT_SEARCH_NOT_FOUND);
            return 0;
        }
    }
    text_decoder_at(&r->text, &match, (TextPosition){hit, r->top.source_line});
    for (int i = 0; query[i]; ++i)
        if (!text_next(&match, &value, &off)) break;
    r->hit_offset = hit;
    r->hit_end = match.offset;
    r->search_active = 1;
    reader_goto(r, index_position_for_offset(&r->index, hit));
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
                           sizeof(r->index.chapters[0]) / sizeof(r->index.chapters[0].title[0]),
                           (int)r->index.chapter_count);
    if (pick >= 0) reader_goto(r, r->index.chapters[pick].position);
}

static void reader_chapter_step(Reader *r, int direction) {
    TextPosition pos;
    if (index_chapter_for_offset(&r->index, r->top.byte_offset, direction, &pos))
        reader_goto(r, pos);
}

static void do_jump(Reader *r) {
    const char *items[] = {L("百分比", "Percent", "Pourcentage"), L("页码", "Page", "Page"),
                           L("物理行号", "Source line", "Ligne source"), TXT_CHAPTER_DIRECTORY};
    int pick = choose_list(r->gc, r->app, L("跳转", "Go to", "Aller à"), items, 4);
    int value = 0;
    if (pick < 0) return;
    if (pick == 0) {
        value = (int)percent_for_reader(r);
        if (show_1numeric_input(L("跳转", "Go to", "Aller à"), "", L("百分比 0-100", "Percent 0-100", "Pourcentage 0-100"), &value, 0, 100))
            reader_goto(r, index_position_for_percent(&r->index, value));
    } else if (pick == 1) {
        value = (int)r->page + 1;
        if (show_1numeric_input(L("跳转", "Go to", "Aller à"), "", L("页码", "Page", "Page"), &value, 1, (int)r->index.page_count))
            reader_goto(r, index_position_for_page(&r->index, (uint32_t)value - 1));
    } else if (pick == 2) {
        value = (int)r->top.source_line;
        if (show_1numeric_input(L("跳转", "Go to", "Aller à"), "", L("原始物理行号", "Source line", "Ligne source"), &value, 1, (int)r->index.total_source_lines))
            reader_goto(r, position_for_line_exact(r, (uint32_t)value));
    } else {
        do_chapter_jump(r);
    }
}
static int apply_setting_change(Reader *r, AppState *app, const char *data_dir, int pick, int delta) {
    int old_font = app->font_choice;
    int old_theme = app->theme;
    int old_margin = app->margin_choice;
    if (pick == 0) app->font_choice = (app->font_choice + delta + 3) % 3;
    else if (pick == 1) app->theme = (app->theme + delta + 3) % 3;
    else app->margin_choice = app->margin_choice ? 0 : 1;
    if (!storage_save_app(app, data_dir)) {
        app->font_choice = old_font;
        app->theme = old_theme;
        app->margin_choice = old_margin;
        debug_fail_errno("apply_setting_change:save", data_dir);
        return -1;
    }
    if (r) {
        uint32_t pct = percent_for_reader(r);
        r->layout = layout_from_state(r->app);
        if (pick == 0) {
            memset(r->ascii_width, 0, sizeof(r->ascii_width));
            memset(r->wide_chars, 0, sizeof(r->wide_chars));
        }
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
        const char *actions[] = {L("删除", "Delete", "Supprimer"), L("取消", "Cancel", "Annuler")};
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
    char line[96];
    for (;;) {
        int key;
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, L("关于 nTexts", "About nTexts", "À propos de nTexts"), 10, 8, Bold12, t->fg);
        draw_text(gc, L("nTexts阅读器", "nTexts Text Reader", "nTexts Lecteur de textes"), 10, 42, Regular10, t->fg);
        snprintf(line, sizeof(line), L("版本: %s", "Version: %s", "Version : %s"), build_version);
        draw_text(gc, line, 10, 68, Regular10, t->fg);
        snprintf(line, sizeof(line), L("提交: %.12s", "Commit: %.12s", "Révision : %.12s"), build_commit);
        draw_text(gc, line, 10, 92, Regular9, t->fg);
        draw_text(gc, L("作者: Ziyang-Bai", "Author: Ziyang-Bai", "Auteur : Ziyang-Bai"), 10, 118, Regular10, t->fg);
        draw_text(gc, "Copyright (c) 2026 Ziyang-Bai", 10, 146, Regular9, t->fg);
        draw_text(gc, L("许可证: GNU GPL v3.0", "License: GNU GPL v3.0", "Licence : GNU GPL v3.0"), 10, 166, Regular9, t->fg);
        draw_text(gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        gui_gc_blit_to_screen(gc);
        key = wait_key();
        wait_release();
        if (key == 5 || key == 6) return;
    }
}

static void usage_page(Gc gc, AppState *app) {
    const Theme *t = &themes[app->theme];
    static const char *zh_pages[][8] = {
        {"主页与阅读", "0 浏览 Documents，Enter 打开最近阅读",
         "浏览器上下选文件，Enter 打开或进入目录",
         "F按文件名筛选，0清除筛选",
         "左右翻页，上下按物理行移动",
         "Ctrl+上/下 跳到开头/结尾",
         "Menu 功能菜单，Esc 返回书库",
         "阅读进度自动保存，重新打开可续读"},
        {"搜索与书签", "F 输入搜索词并跳到结果",
         "搜索中 Enter/下 找下一处，上找上一处",
         "Menu 中向后/向前查找沿用搜索词",
         "B 添加当前位置书签",
         "Menu 管理书签可跳转或删除",
         "命中词精确高亮，跨页可左右查看", NULL},
        {"跳转与设置", "G 按百分比、页码、物理行号跳转",
         "G 进入章节目录，Ctrl+左/右 换章",
         "主页 (-) 或阅读 Menu 打开设置",
         "设置第8项切换中文/English/Français",
         "阅读 Menu 按0手动选择单本编码",
         "编码: 自动/UTF-8/GB18030/UTF-16LE/BE",
         "章节规则: *多字符，?单字符"}
    };
    static const char *en_pages[][8] = {
        {"Home and reading", "0 browse Documents; Enter resumes a book",
         "Up/Down pick; Enter opens file/folder",
         "Browser: F filters names; 0 clears filter",
         "Left/Right pages; Up/Down source lines",
         "Ctrl+Up/Down jumps to start/end",
         "Menu for actions; Esc returns home",
         "Progress saves automatically"},
        {"Search and bookmarks", "F searches text",
         "Search: Enter/Down next; Up previous",
         "Menu repeats forward/backward search",
         "B bookmarks this position",
         "Menu manages bookmarks: go to or delete",
         "Matches are highlighted across pages", NULL},
        {"Go to and settings", "G goes to percent, page or source line",
         "G lists chapters; Ctrl+Left/Right changes chapter",
         "Home (-) or reader Menu opens settings",
         "Settings item 8: Chinese/English/Français",
         "Reader Menu: 0 picks this book's encoding",
         "Auto, UTF-8, GB18030, UTF-16 LE/BE",
         "Chapter rules: * many, ? one character"}
    };
    static const char *fr_pages[][8] = {
        {"Accueil et lecture", "0 parcourt Documents ; Enter reprend un livre",
         "Haut/Bas : choisir ; Enter : ouvrir",
         "F filtre les noms ; 0 efface le filtre",
         "Gauche/Droite : pages ; Haut/Bas : lignes",
         "Ctrl+Haut/Bas : début/fin du texte",
         "Menu : actions ; Esc : accueil",
         "La progression est sauvegardée"},
        {"Recherche et signets", "F cherche dans le texte",
         "Enter/Bas : suivant ; Haut : précédent",
         "Menu relance la recherche dans les 2 sens",
         "B ajoute un signet ici",
         "Menu : voir, rejoindre ou supprimer un signet",
         "Résultats surlignés même entre pages", NULL},
        {"Navigation et réglages", "G : pourcentage, page ou ligne source",
         "G : chapitres ; Ctrl+Gauche/Droite : chapitre",
         "(-) accueil ou Menu lecture : réglages",
         "Réglage 8 : 中文/English/Français",
         "Menu lecture, 0 : encodage du livre",
         "Auto, UTF-8, GB18030, UTF-16 LE/BE",
         "Règles : * plusieurs, ? un caractère"}
    };
    const char *(*pages)[8] = app_locale == 2 ? fr_pages : app_locale == 1 ? en_pages : zh_pages;
    int page = 0;
    for (;;) {
        int key;
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, pages[page][0], 10, 8, Bold12, t->fg);
        for (int i = 1; i < 8 && pages[page][i]; ++i)
            draw_text(gc, pages[page][i], 10, 20 + i * 23, Regular9, t->fg);
        draw_text(gc, L("左右切换  Enter/Esc返回", "Left/Right pages  Enter/Esc back", "Gauche/Droite pages  Enter/Esc retour"), 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        gui_gc_blit_to_screen(gc);
        key = wait_key();
        wait_release();
        if (key == 5 || key == 6) return;
        if (key == 3 && page > 0) --page;
        if (key == 4 && page < 2) ++page;
    }
}

typedef enum {
    TUTORIAL_SCREEN_HOME,
    TUTORIAL_SCREEN_BROWSER,
    TUTORIAL_SCREEN_BROWSER_SELECTED,
    TUTORIAL_SCREEN_READER,
    TUTORIAL_SCREEN_READER_PAGE2
} TutorialScreen;

typedef struct {
    TutorialScreen screen;
    const char *title;
    const char *body1;
    const char *body2;
    int key;
    int x, y, w, h;
} TutorialStep;

static const char *tutorial_key_label(const TutorialStep *step) {
    switch (step->key) {
        case 2: return L("下", "Down", "Bas");
        case 3: return L("左", "Left", "Gauche");
        case 4: return L("右", "Right", "Droite");
        case 5: return "Enter";
        case 6: return "Esc";
        case 12: return "0";
        default: return "Enter";
    }
}

static void tutorial_mock_screen(Gc gc, const Theme *t, TutorialScreen screen) {
    set_color(gc, t->bg);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    if (screen == TUTORIAL_SCREEN_HOME) {
        draw_text(gc, L("nTexts阅读器", "nTexts Text Reader", "nTexts Lecteur de textes"), 8, 6, Bold12, t->fg);
        draw_text(gc, L("最近阅读", "Recent books", "Lectures récentes"), 8, 32, Bold10, t->fg);
        draw_text(gc, "1. tutorial_demo.txt.tns  12%", 10, 54, Regular10, t->fg);
        draw_text(gc, TXT_HOME_BROWSE_DOCS, 10, 96, Regular10, t->fg);
        draw_text(gc, TXT_SETTINGS, 10, 118, Regular10, t->fg);
        draw_text(gc, TXT_FOOTER_HOME, 8, FOOTER_Y, Regular9, t->muted);
    } else if (screen == TUTORIAL_SCREEN_BROWSER || screen == TUTORIAL_SCREEN_BROWSER_SELECTED) {
        draw_text(gc, "Documents", 8, 6, Bold10, t->fg);
        if (screen == TUTORIAL_SCREEN_BROWSER) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, 30, SCREEN_W - 8, 20);
        } else {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, 52, SCREEN_W - 8, 20);
        }
        draw_text(gc, L("[目录] Books", "[Folder] Books", "[Dossier] Books"), 10, 32, Regular10, t->fg);
        draw_text(gc, "tutorial_demo.txt.tns", 10, 54, Regular10, t->fg);
        draw_text(gc, "notes.txt.tns", 10, 76, Regular10, t->fg);
        draw_text(gc, TXT_FOOTER_BROWSER, 8, FOOTER_Y, Regular9, t->muted);
    } else {
        draw_text(gc, L("nTexts 教学演示文档", "nTexts tutorial text", "Texte de démonstration nTexts"), 10, 12, Bold10, t->fg);
        if (screen == TUTORIAL_SCREEN_READER_PAGE2) {
            draw_wrapped_text(gc, L("这是第二页。刚才按右键以后，页面已经前进。",
                                    "This is page two. Right advanced the page.",
                                    "Voici la deuxième page. Droite avance."), 10, 40,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, L("再按左键就会回到上一页。", "Left returns to the previous page.",
                                    "Gauche revient à la page précédente."), 10, 86,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_text(gc, L("2/8页  25%  UTF-8", "2/8 pages  25%  UTF-8", "2/8 pages  25%  UTF-8"), 6, SCREEN_H - 15, Regular9, t->muted);
        } else {
            draw_wrapped_text(gc, L("这是第一页。按右键翻到下一页。",
                                    "This is page one. Press Right for the next page.",
                                    "Première page. Droite passe à la suivante."), 10, 40,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_wrapped_text(gc, L("阅读进度会自动保存。", "Progress saves automatically.",
                                    "La progression est sauvegardée."), 10, 86,
                              SCREEN_W - 20, Regular10, t->fg);
            draw_text(gc, L("1/8页  12%  UTF-8", "1/8 pages  12%  UTF-8", "1/8 pages  12%  UTF-8"), 6, SCREEN_H - 15, Regular9, t->muted);
        }
    }
}

static void tutorial_center_page(Gc gc, AppState *app, const char *data_dir) {
    const Theme *t = &themes[app->theme];
    const TutorialStep steps[] = {
        {TUTORIAL_SCREEN_HOME, L("打开书库", "Open library", "Ouvrir la bibliothèque"), L("主页按 0 进入 Documents 浏览器。", "Press 0 to browse Documents.", "Accueil : 0 parcourt Documents."), L("也可以从最近阅读续读。", "Recent books resume your reading.", "Reprenez une lecture récente."), 12, 6, 92, 180, 22},
        {TUTORIAL_SCREEN_BROWSER, L("选择文件", "Choose file", "Choisir un fichier"), L("按下键选中演示文本文件。", "Press Down to select the demo file.", "Bas sélectionne le fichier exemple."), L("浏览器也支持进入文件夹。", "Enter opens folders too.", "Enter ouvre aussi les dossiers."), 2, 6, 30, 220, 44},
        {TUTORIAL_SCREEN_BROWSER_SELECTED, L("打开文本", "Open text", "Ouvrir le texte"), L("按 Enter 打开选中的文本。", "Enter opens the selected text.", "Enter ouvre le fichier sélectionné."), L("支持 .txt 和 .txt.tns 文件。", "Supports .txt and .txt.tns files.", "Formats : .txt et .txt.tns."), 5, 6, 50, 220, 24},
        {TUTORIAL_SCREEN_READER, L("翻到下一页", "Next page", "Page suivante"), L("按右键阅读下一页。", "Press Right for the next page.", "Droite affiche la page suivante."), L("阅读进度会自动保存。", "Progress saves automatically.", "La progression est sauvegardée."), 4, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER_PAGE2, L("返回上一页", "Previous page", "Page précédente"), L("按左键回到上一页。", "Press Left to go back.", "Gauche revient à la page précédente."), L("设置第8项切换语言。", "Settings item 8 changes language.", "Réglage 8 : changer de langue."), 3, 0, SCREEN_H - 20, SCREEN_W, 20},
        {TUTORIAL_SCREEN_READER, L("返回书库", "Return home", "Retour à l'accueil"), L("按 Esc 返回书库。", "Press Esc to return home.", "Esc revient à l'accueil."), L("Menu按0选择单本编码。", "Menu then 0 selects book encoding.", "Menu puis 0 : encodage du livre."), 6, 0, SCREEN_H - 20, SCREEN_W, 20}
    };
    int count = (int)(sizeof(steps) / sizeof(steps[0]));
    for (int i = 0; i < count;) {
        int key;
        char footer[96];
        int panel_y;
        const TutorialStep *step = &steps[i];
        app_log("tutorial", "step %d/%d title=%s expect=%s",
                i + 1, count, step->title, tutorial_key_label(step));
        panel_y = step->y > 118 ? 18 : 136;
        gui_gc_begin(gc);
        tutorial_mock_screen(gc, t, step->screen);
        fill_dim_outside(gc, step->x, step->y, step->w, step->h);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 8, panel_y, SCREEN_W - 16, 76);
        draw_text(gc, step->title, 14, panel_y + 6, Bold10, t->fg);
        draw_text(gc, step->body1, 14, panel_y + 24, Regular9, t->fg);
        draw_text(gc, step->body2, 14, panel_y + 42, Regular9, t->fg);
        snprintf(footer, sizeof(footer), L("%d/%d  按%s继续  0跳过", "%d/%d  Press %s  0 skip", "%d/%d  %s pour continuer  0 ignorer"),
                 i + 1, count, tutorial_key_label(step));
        draw_text(gc, footer, 14, panel_y + 60, Regular9, t->muted);
        gui_gc_finish(gc);
        gui_gc_blit_to_screen(gc);

        key = wait_key();
        wait_release();
        app_log("tutorial", "key step=%d got=%d expect=%d",
                i + 1, key, step->key);
        if (key == 12 && step->key != 12) {
            uint32_t old_flags = app->tutorial_flags;
            app->tutorial_flags = TUTORIAL_ALL_SKIPPED | TUTORIAL_READER_SEEN;
            if (!storage_save_app(app, data_dir)) {
                app->tutorial_flags = old_flags;
                debug_fail_errno("tutorial_center_page:skip", data_dir);
                return;
            }
            app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_SKIP_DONE);
            return;
        }
        if (key == step->key) {
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
    else if (selection == 7) {
        uint32_t old_locale = app->locale;
        app->locale = (app->locale + (delta < 0 ? 2 : 1)) % 3;
        if (!storage_save_app(app, data_dir)) {
            app->locale = old_locale;
            debug_fail_errno("run_settings_item:locale", data_dir);
            return -1;
        }
        app_locale = app->locale;
    } else {
        uint32_t old_flags = app->tutorial_flags;
        app->tutorial_flags = 0;
        if (!storage_save_app(app, data_dir)) {
            app->tutorial_flags = old_flags;
            debug_fail_errno("run_settings_item:reset_tutorial", data_dir);
            return -1;
        }
        app_message(gc, app, TXT_TUTORIAL_DONE, TXT_TUTORIAL_RESET_DONE);
    }
    return 1;
}

static int settings_menu(Gc gc, AppState *app, const char *data_dir, Reader *r) {
    const Theme *t;
    const char *font_labels[3], *theme_labels[3], *margin_labels[2];
    char items[8][96];
    int sel = 0;
    for (;;) {
        int key;
        font_labels[0] = L("小", "Small", "Petit");
        font_labels[1] = L("中", "Medium", "Moyen");
        font_labels[2] = L("大", "Large", "Grand");
        theme_labels[0] = L("浅色", "Light", "Clair");
        theme_labels[1] = L("深色", "Dark", "Sombre");
        theme_labels[2] = L("护眼", "Sepia", "Sépia");
        margin_labels[0] = L("窄", "Narrow", "Étroite");
        margin_labels[1] = L("宽", "Wide", "Large");
        t = &themes[app->theme];
        snprintf(items[0], sizeof(items[0]), L("1. 字号: %s/%s/%s  当前:%s", "1. Font: %s/%s/%s [%s]", "1. Taille : %s/%s/%s [%s]"),
                 font_labels[0], font_labels[1], font_labels[2], font_labels[app->font_choice]);
        snprintf(items[1], sizeof(items[1]), L("2. 主题: %s/%s/%s  当前:%s", "2. Theme: %s/%s/%s [%s]", "2. Thème : %s/%s/%s [%s]"),
                 theme_labels[0], theme_labels[1], theme_labels[2], theme_labels[app->theme]);
        snprintf(items[2], sizeof(items[2]), L("3. 边距: %s/%s  当前:%s", "3. Margin: %s/%s [%s]", "3. Marges : %s/%s [%s]"),
                 margin_labels[0], margin_labels[1], margin_labels[app->margin_choice ? 1 : 0]);
        snprintf(items[3], sizeof(items[3]), L("4. 章节规则: 自定义%lu条", "4. Chapter rules: %lu", "4. Règles de chapitre : %lu"),
                 (unsigned long)app->chapter_rules.count);
        snprintf(items[4], sizeof(items[4]), "%s", L("5. 关于", "5. About", "5. À propos"));
        snprintf(items[5], sizeof(items[5]), "%s", L("6. 使用说明", "6. Help", "6. Aide"));
        snprintf(items[6], sizeof(items[6]), "%s", L("7. 重置教学进度", "7. Reset tutorial", "7. Réinit. tutoriel"));
        snprintf(items[7], sizeof(items[7]), L("8. 语言: %s", "8. Language: %s", "8. Langue : %s"),
                 app->locale == 2 ? "Français" : app->locale == 1 ? "English" : L("中文", "Chinese", "Chinois"));
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, L("阅读设置", "Reader settings", "Réglages de lecture"), 8, 6, Bold12, t->fg);
        for (int i = 0; i < 8; ++i) {
            int y = 29 + i * 23;
            if (i == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            draw_text(gc, items[i], 10, y, Regular10, i == sel ? t->highlight_fg : t->fg);
        }
        draw_text(gc, L("1-8直达  左右/Enter执行  Esc返回", "1-8 select  Left/Right/Enter  Esc back", "1-8 choisir  G/D/Enter  Esc retour"), 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        gui_gc_blit_to_screen(gc);
        key = wait_key(); wait_release();
        if (key == 1) sel = sel > 0 ? sel - 1 : 7;
        else if (key == 2) sel = sel < 7 ? sel + 1 : 0;
        else if (key >= 21 && key <= 28) {
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
    char path[68];
    char name[72];
    char line[128];
    int y;
    shorten_tail(name, sizeof(name), base_name(r->text.path));
    shorten_tail(path, sizeof(path), r->text.path);
    for (;;) {
        gui_gc_begin(r->gc);
        set_color(r->gc, t->bg);
        gui_gc_fillRect(r->gc, 0, 0, SCREEN_W, SCREEN_H);

        draw_text(r->gc, L("文件信息", "File info", "Infos sur le fichier"), 10, 8, Bold12, t->fg);
        snprintf(line, sizeof(line), L("名称: %s", "Name: %s", "Nom : %s"), name);
        draw_text(r->gc, line, 10, 42, Regular10, t->fg);
        draw_text(r->gc, L("路径:", "Path:", "Chemin :"), 10, 64, Regular10, t->fg);
        y = draw_wrapped_text(r->gc, path, 10, 82, SCREEN_W - 20, Regular9, t->fg);
        if (y < 112) y = 112;

        snprintf(line, sizeof(line), L("大小: %lu 字节", "Size: %lu bytes", "Taille : %lu octets"), (unsigned long)r->text.size);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), L("编码: %s", "Encoding: %s", "Encodage : %s"), text_encoding_name(r->text.encoding));
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), L("行数: %lu", "Lines: %lu", "Lignes : %lu"), (unsigned long)r->index.total_source_lines);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), L("页数: %lu", "Pages: %lu", "Pages : %lu"), (unsigned long)r->index.page_count);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);
        y += 18;
        snprintf(line, sizeof(line), L("修改: %lu", "Modified: %lu", "Modifié : %lu"), (unsigned long)r->text.mtime);
        draw_text(r->gc, line, 10, y, Regular9, t->fg);

        draw_text(r->gc, TXT_ESC_OR_ENTER_BACK, 8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(r->gc);
        gui_gc_blit_to_screen(r->gc);

        y = wait_key();
        wait_release();
        if (y == 5 || y == 6) return;
    }
}

static int choose_encoding(Reader *r) {
    const char *names[] = {L("自动检测", "Auto detect", "Détection auto"), "UTF-8", "GB18030",
                           "UTF-16 LE", "UTF-16 BE"};
    const char *items[5];
    char labels[5][40];
    int pick;
    TextFile detected;
    TextEncoding old_encoding = r->text.encoding;
    uint32_t old_start = r->text.data_start;
    uint32_t old_override = r->book.encoding_override;
    int changed;
    uint32_t i;
    for (i = 0; i < 5; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "%s%s",
                 i == old_override ? "* " : "  ", names[i]);
        items[i] = labels[i];
    }
    pick = choose_list(r->gc, r->app, L("文本编码 (*当前)", "Text encoding (* current)", "Encodage (* actuel)"), items, 5);
    if (pick < 0 || (uint32_t)pick == old_override) return 0;
    if (pick == 0) {
        if (!text_open(&detected, r->text.path)) {
            debug_fail_errno("choose_encoding:text_open", r->text.path);
            return 0;
        }
        r->text.encoding = detected.encoding;
        r->text.data_start = detected.data_start;
        text_close(&detected);
    } else if (!text_select_encoding(&r->text, (TextEncoding)pick)) {
        debug_fail_errno("choose_encoding:text_select_encoding", r->text.path);
        return 0;
    }
    r->book.encoding_override = (uint32_t)pick;
    r->book.encoding = (uint32_t)r->text.encoding;
    changed = r->text.encoding != old_encoding || r->text.data_start != old_start;
    if (changed) {
        index_free(&r->index);
        if (!ensure_index(r)) {
            r->text.encoding = old_encoding;
            r->text.data_start = old_start;
            r->book.encoding_override = old_override;
            r->book.encoding = (uint32_t)old_encoding;
            return -1;
        }
        for (i = 0; i < r->book.bookmark_count; ++i) {
            Bookmark *b = &r->book.bookmarks[i];
            TextDecoder d;
            TextPosition pos = index_position_for_offset(&r->index, b->position.byte_offset);
            uint16_t ch;
            uint32_t off;
            text_decoder_at(&r->text, &d, pos);
            while (d.offset < b->position.byte_offset && text_next(&d, &ch, &off)) {}
            b->position.byte_offset = d.offset;
            b->position.source_line = d.line;
            text_make_excerpt(&r->text, b->position, b->excerpt, EXCERPT_LEN);
        }
        r->search_active = 0;
        r->hit_offset = 0;
        reader_goto_page(r, 0);
    }
    if (!save_reader(r)) return -1;
    if (changed)
        app_message(r->gc, r->app, L("文本编码", "Text encoding", "Encodage du texte"),
                    L("编码已切换，阅读进度从开头开始", "Encoding changed; reading starts at beginning", "Encodage modifié ; reprise au début"));
    return 1;
}

static int reader_menu(Reader *r) {
    for (;;) {
        const char *items[] = {
            L("1. 添加书签", "1. Add bookmark", "1. Ajouter un signet"),
            L("2. 管理书签", "2. Bookmarks", "2. Signets"),
            L("3. 搜索", "3. Search", "3. Rechercher"),
            L("4. 向后查找", "4. Find next", "4. Résultat suivant"),
            L("5. 向前查找", "5. Find previous", "5. Résultat précédent"),
            L("6. 跳转", "6. Go to", "6. Aller à"),
            L("7. 阅读设置", "7. Reader settings", "7. Réglages de lecture"),
            L("8. 文件信息", "8. File info", "8. Infos sur le fichier"),
            L("9. 返回书库", "9. Return home", "9. Retour à l'accueil"),
            L("0. 文本编码", "0. Text encoding", "0. Encodage du texte")
        };
        int pick = choose_list_rows(r->gc, r->app, L("功能菜单", "Reader menu", "Menu de lecture"),
                                    items, NULL, 0, 10, 9);
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
        } else if (pick == 9) {
            if (choose_encoding(r) < 0) return -1;
            return 0;
        }
    }
}

static int open_reader(const char *path, Gc gc, AppState *app, const char *data_dir) {
    Reader r;
    int key, menu_res, page_drawn = 0;
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
    if (strcmp(path, r.text.path))
        storage_remove_recent(app, path);
    storage_load_book(&r.book, &r.text, data_dir);
    r.layout = layout_from_state(app);
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
        TextPosition before;
        if (!page_drawn) draw_page(&r);
        page_drawn = 0;
        key = wait_key();
        if (!last_key_ctrl &&
            ((key == 4 && r.page + 1 < r.index.page_count) || (key == 3 && r.page > 0))) {
            uint32_t page = key == 4 ? r.page + 1 : r.page - 1;
            uint32_t start = r.index.pages[page].byte_offset;
            uint32_t end = page + 1 < r.index.page_count ?
                           r.index.pages[page + 1].byte_offset : r.text.size;
            if (!r.search_active || start >= r.hit_end || end <= r.hit_offset) {
                r.search_active = 0;
                r.hit_offset = 0;
            }
            reader_goto_page(&r, page);
            draw_page(&r);
            page_drawn = 1;
            wait_release();
            if (!save_reader(&r)) page_drawn = 0;
            continue;
        }
        wait_release();
        before = r.top;
        if (r.search_active) {
            if (key == 5 || key == 2 || key == 9) {
                if (do_search(&r, 1, 1)) save_reader(&r);
                continue;
            } else if (key == 1) {
                if (do_search(&r, -1, 1)) save_reader(&r);
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
        if (last_key_ctrl && (key == 3 || key == 4))
            reader_chapter_step(&r, key == 4 ? 1 : -1);
        else if (key == 4 && r.page + 1 < r.index.page_count) reader_goto_page(&r, r.page + 1);
        else if (key == 3 && r.page > 0) reader_goto_page(&r, r.page - 1);
        else if (key == 1 && last_key_ctrl) reader_goto_page(&r, 0);
        else if (key == 2 && last_key_ctrl) reader_goto_page(&r, r.index.page_count - 1);
        else if (key == 2 && r.top.source_line < r.index.total_source_lines) reader_goto(&r, position_for_line_exact(&r, r.top.source_line + 1));
        else if (key == 1 && r.top.source_line > 1) reader_goto(&r, position_for_line_exact(&r, r.top.source_line - 1));
        else if (key == 8) add_bookmark(&r);
        else if (key == 9) do_search(&r, 1, 0);
        else if (key == 10) do_jump(&r);
        else if (key == 7) {
            menu_res = reader_menu(&r);
            if (menu_res < 0) break;
        } else if (key == 6) break;
        if (r.top.byte_offset != before.byte_offset || r.top.source_line != before.source_line)
            save_reader(&r);
    }
    save_reader_all(&r);
    index_free(&r.index);
    text_close(&r.text);
    app_log("reader", "open end");
    return 1;
}

static void draw_home(Gc gc, AppState *app, int sel, int top) {
    const Theme *t = &themes[app->theme];
    char line[160];
    int recent_count = app->recent_count > MAX_RECENTS ? MAX_RECENTS : (int)app->recent_count;
    int total = recent_count + 2;
    static int logged_once = 0;
    if (sel >= total) sel = total - 1;
    if (sel < 0) sel = 0;
    if (top > sel) top = sel;
    if (top + HOME_VISIBLE_ROWS <= sel) top = sel - HOME_VISIBLE_ROWS + 1;
    if (!logged_once) app_log("home", "draw begin recents=%lu sel=%d theme=%d",
                              (unsigned long)app->recent_count, sel, app->theme);
    gui_gc_begin(gc);
    set_color(gc, t->bg);
    gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
    draw_text(gc, L("nTexts阅读器", "nTexts Text Reader", "nTexts Lecteur de textes"), 8, 6, Bold12, t->fg);
    draw_text(gc, recent_count ? L("最近阅读", "Recent books", "Lectures récentes") : L("暂无最近阅读", "No recent books", "Aucune lecture récente"), 8, 30, Bold10,
              recent_count ? t->fg : t->muted);
    for (int i = 0; i < HOME_VISIBLE_ROWS && top + i < total; ++i) {
        int row = top + i;
        int y = 52 + i * 20;
        if (row == sel) {
            set_color(gc, t->highlight_bg);
            gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 19);
        }
        if (row < recent_count) {
            unsigned long percent = app->recents[row].file_size ?
                (unsigned long)(((uint64_t)app->recents[row].offset * 100u) /
                                app->recents[row].file_size) : 0ul;
            snprintf(line, sizeof(line), "%d. %s  %lu%%", row + 1,
                     base_name(app->recents[row].path), percent);
        } else {
            snprintf(line, sizeof(line), "%s",
                     row == recent_count ? TXT_HOME_BROWSE_DOCS : TXT_SETTINGS);
        }
        draw_text(gc, line, 10, y, Regular10, row == sel ? t->highlight_fg : t->fg);
    }
    draw_text(gc, TXT_FOOTER_HOME, 8, FOOTER_Y, Regular9, t->muted);
    gui_gc_finish(gc);
    gui_gc_blit_to_screen(gc);
    if (!logged_once) {
        app_log("home", "draw ok");
        logged_once = 1;
    }
}

static int browse_files(Gc gc, AppState *app, char *out, size_t outcap) {
    const char *documents = get_documents_dir();
    const Theme *t = &themes[app->theme];
    char cwd[512];
    uint16_t filter[QUERY_LEN] = {0};
    FileItem *items = NULL;
    int count = 0, file_count = 0, sel = 0, top = 0, visible = 8;
    int reload = 1;
    if (!documents || !*documents || !out || !outcap) {
        errno = EINVAL;
        return 0;
    }
    snprintf(cwd, sizeof(cwd), "%s", documents);
    for (;;) {
        int key;
        if (reload) {
            free(items);
            items = NULL;
            if (!load_dir(cwd, filter, &items, &count)) return 0;
            file_count = 0;
            for (int i = 0; i < count; ++i) if (!items[i].is_dir) ++file_count;
            if (sel >= count) sel = count ? count - 1 : 0;
            if (top > sel) top = sel;
            reload = 0;
        }
        gui_gc_begin(gc);
        set_color(gc, t->bg);
        gui_gc_fillRect(gc, 0, 0, SCREEN_W, SCREEN_H);
        draw_text(gc, strcmp(cwd, documents) ? base_name(cwd) : "Documents",
                  8, 6, Bold10, t->fg);
        if (filter[0]) {
            char label[48];
            uint16_t preview[18];
            snprintf(label, sizeof(label), "%s (%d):", L("筛选", "Filter", "Filtre"), file_count);
            draw_text(gc, label, 8, 23, Regular9, t->muted);
            copy_u16(preview, (int)(sizeof(preview) / sizeof(preview[0])), filter);
            draw_u16(gc, preview, 100, 23, Regular9, t->fg);
        }
        for (int i = 0; i < visible && top + i < count; ++i) {
            char label[150];
            int y = (filter[0] ? 42 : 30) + i * 21;
            if (top + i == sel) {
                set_color(gc, t->highlight_bg);
                gui_gc_fillRect(gc, 4, y - 2, SCREEN_W - 8, 20);
            }
            snprintf(label, sizeof(label), "%s%s",
                     items[top + i].is_dir ? L("[目录] ", "[Folder] ", "[Dossier] ") : "", items[top + i].name);
            draw_text(gc, label, 10, y, Regular10,
                      top + i == sel ? t->highlight_fg : t->fg);
        }
        draw_text(gc, filter[0] ?
                  L("F筛选  0清除  Esc上级/返回", "F filter  0 clear  Esc up/back",
                    "F filtrer  0 effacer  Esc retour") : TXT_FOOTER_BROWSER,
                  8, FOOTER_Y, Regular9, t->muted);
        gui_gc_finish(gc);
        gui_gc_blit_to_screen(gc);
        key = wait_key();
        wait_release();
        if (key == 9) {
            uint16_t input[QUERY_LEN];
            if (request_u16_input(L("文件名筛选", "Filter filename", "Filtrer le nom"),
                                  L("输入部分文件名", "Part of filename", "Partie du nom"),
                                  filter, input, QUERY_LEN)) {
                copy_u16(filter, QUERY_LEN, input);
                sel = top = 0;
                reload = 1;
            }
            continue;
        }
        if (key == 12 && filter[0]) {
            filter[0] = 0;
            sel = top = 0;
            reload = 1;
            continue;
        }
        if (key == 1) {
            sel = count ? (sel > 0 ? sel - 1 : count - 1) : 0;
            if (sel < top) top = sel;
            if (sel >= top + visible) top = sel - visible + 1;
        } else if (key == 2) {
            sel = count ? (sel + 1 < count ? sel + 1 : 0) : 0;
            if (sel >= top + visible) top = sel - visible + 1;
            if (sel < top) top = sel;
        } else if (key == 5 && count) {
            if (items[sel].is_dir) {
                snprintf(cwd, sizeof(cwd), "%s", items[sel].path);
                sel = top = 0;
                reload = 1;
            } else {
                snprintf(out, outcap, "%s", items[sel].path);
                free(items);
                return 1;
            }
        } else if (key == 6) {
            char *slash = strrchr(cwd, '/');
            if (slash && strcmp(cwd, documents)) {
                *slash = 0;
                sel = top = 0;
                reload = 1;
            } else {
                free(items);
                return 0;
            }
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
    if (!storage_save_app(app, data_dir)) {
        app->tutorial_flags &= ~TUTORIAL_READER_SEEN;
        debug_fail_errno("run_tutorial_if_needed:save", data_dir);
        return;
    }
    app_log("tutorial", "done");
}

int ntexts_app_main(int argc, char **argv) {
    Gc gc;
    AppState app;
    char data_dir[512], selected[512];
    int sel = 0;
    int home_top = 0;
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
    if (!storage_init(data_dir, sizeof(data_dir))) {
        debug_fail_errno("main:storage_init", data_dir[0] ? data_dir : "Documents/nTexts");
        lcd_init(SCR_TYPE_INVALID);
        return 1;
    }
    app_log("startup", "storage dir=%s", data_dir);
    storage_load_app(&app, data_dir);
    app_locale = app.locale;
    if (storage_prune_recents(&app)) {
        app_log("startup", "pruned invalid recents");
        if (!storage_save_app(&app, data_dir))
            debug_fail_errno("main:prune_recents", data_dir);
    }
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
        int recent_count;
        int total;
        draw_home(gc, &app, sel, home_top);
        key = wait_key();
        wait_release();
        recent_count = app.recent_count > MAX_RECENTS ? MAX_RECENTS : (int)app.recent_count;
        total = recent_count + 2;
        if (key == 1) sel = sel > 0 ? sel - 1 : total - 1;
        else if (key == 2) sel = sel + 1 < total ? sel + 1 : 0;
        else if ((key == 3 || key == 4) && sel == recent_count + 1)
            settings_menu(gc, &app, data_dir, NULL);
        else if (key == 12) {
            memset(selected, 0, sizeof(selected));
            if (browse_files(gc, &app, selected, sizeof(selected))) {
                open_reader(selected, gc, &app, data_dir);
                sel = home_top = 0;
            }
        } else if (key == 13) {
            settings_menu(gc, &app, data_dir, NULL);
        } else if (key == 5) {
            if (sel < recent_count) {
                char recent_path[512];
                snprintf(recent_path, sizeof(recent_path), "%s", app.recents[sel].path);
                app_log("home", "open recent[%d]=%s", sel, recent_path);
                if (!open_reader(recent_path, gc, &app, data_dir)) {
                    storage_remove_recent(&app, recent_path);
                    if (!storage_save_app(&app, data_dir))
                        debug_fail_errno("main:remove_recent", recent_path);
                }
                sel = home_top = 0;
            } else if (sel == recent_count) {
                memset(selected, 0, sizeof(selected));
                if (browse_files(gc, &app, selected, sizeof(selected))) {
                    open_reader(selected, gc, &app, data_dir);
                    sel = home_top = 0;
                }
            } else {
                settings_menu(gc, &app, data_dir, NULL);
            }
        } else if (key == 7) {
            memset(selected, 0, sizeof(selected));
            if (browse_files(gc, &app, selected, sizeof(selected))) {
                open_reader(selected, gc, &app, data_dir);
                sel = home_top = 0;
            }
        } else if (key == 6) break;
        recent_count = app.recent_count > MAX_RECENTS ? MAX_RECENTS : (int)app.recent_count;
        total = recent_count + 2;
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (home_top > sel) home_top = sel;
        if (home_top + HOME_VISIBLE_ROWS <= sel) home_top = sel - HOME_VISIBLE_ROWS + 1;
    }
    if (!storage_save_app(&app, data_dir))
        debug_fail_errno("main:save_app", data_dir);
    lcd_init(SCR_TYPE_INVALID);
    app_log("shutdown", "normal exit");
    return 0;
}
