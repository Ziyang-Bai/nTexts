#include "reader_index.h"
#include <stdlib.h>
#include <string.h>

#define INDEX_MAGIC 0x5849544eu
#define INDEX_VERSION 4u

typedef struct {
    uint32_t magic, version, file_size, file_mtime, encoding;
    int32_t font_size, margin, lines_per_page, line_height;
    uint32_t page_count, line_count, total_source_lines;
} IndexHeader;

static int append_position(TextPosition **items, uint32_t *count, uint32_t *capacity,
                           TextPosition value) {
    if (*count == *capacity) {
        uint32_t cap = *capacity ? *capacity * 2 : 256;
        TextPosition *next = realloc(*items, cap * sizeof(*next));
        if (!next) return 0;
        *items = next; *capacity = cap;
    }
    (*items)[(*count)++] = value;
    return 1;
}

static int append_unique_position(TextPosition **items, uint32_t *count, uint32_t *capacity,
                                  TextPosition value) {
    if (*count) {
        TextPosition last = (*items)[*count - 1];
        if (last.byte_offset == value.byte_offset && last.source_line == value.source_line) return 1;
    }
    return append_position(items, count, capacity, value);
}

void index_init(PageIndex *index) { memset(index, 0, sizeof(*index)); }

void index_free(PageIndex *index) {
    free(index->pages); free(index->lines);
    memset(index, 0, sizeof(*index));
}

static int same_layout(Layout a, Layout b) {
    return a.font_size == b.font_size && a.margin == b.margin &&
           a.lines_per_page == b.lines_per_page && a.line_height == b.line_height;
}

int index_build(PageIndex *index, TextFile *text, Gc gc, Layout layout,
                IndexProgress progress, void *context) {
    TextDecoder d;
    TextPosition first = {text->data_start, 1};
    uint16_t ch;
    uint32_t off, visual_line = 0, last_report = 0;
    int width = 0, max_width = 320 - layout.margin * 2;
    index_free(index);
    index->layout = layout; index->file_size = text->size;
    index->file_mtime = text->mtime; index->encoding = text->encoding;
    if (!append_unique_position(&index->pages, &index->page_count, &index->page_capacity, first) ||
        !append_unique_position(&index->lines, &index->line_count, &index->line_capacity, first)) return 0;
    text_decoder_at(text, &d, first);
    while (text_next(&d, &ch, &off)) {
        int cw;
        if (ch == '\r') continue;
        if (ch == '\n') {
            visual_line++; width = 0;
            if ((d.line - 1) % 64 == 0) {
                TextPosition p = {d.offset, d.line};
                if (!append_unique_position(&index->lines, &index->line_count, &index->line_capacity, p)) return 0;
            }
            if (visual_line % layout.lines_per_page == 0 && d.offset < text->size) {
                TextPosition p = {d.offset, d.line};
                if (!append_unique_position(&index->pages, &index->page_count, &index->page_capacity, p)) return 0;
            }
        } else {
            if (ch == '\t') ch = ' ';
            if (ch < 32) continue;
            cw = gui_gc_getCharWidth(gc, (gui_gc_Font)layout.font_size, (short)ch);
            if (cw <= 0) cw = layout.font_size;
            if (width && width + cw > max_width) {
                visual_line++; width = 0;
                if (visual_line % layout.lines_per_page == 0) {
                    TextPosition p = {off, d.line};
                    if (!append_unique_position(&index->pages, &index->page_count, &index->page_capacity, p)) return 0;
                }
            }
            width += cw;
        }
        if (progress && d.offset - last_report >= 32768) {
            last_report = d.offset;
            if (!progress(d.offset, text->size, context)) return -1;
        }
    }
    index->total_source_lines = d.line;
    if (progress) progress(text->size, text->size, context);
    return 1;
}

int index_save(const PageIndex *index, const char *path) {
    char tmp[600];
    FILE *fp;
    IndexHeader h;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) return 0;
    memset(&h, 0, sizeof(h));
    h.magic = INDEX_MAGIC; h.version = INDEX_VERSION;
    h.file_size = index->file_size; h.file_mtime = index->file_mtime; h.encoding = index->encoding;
    h.font_size = index->layout.font_size; h.margin = index->layout.margin;
    h.lines_per_page = index->layout.lines_per_page; h.line_height = index->layout.line_height;
    h.page_count = index->page_count; h.line_count = index->line_count;
    h.total_source_lines = index->total_source_lines;
    if (fwrite(&h, sizeof(h), 1, fp) != 1 ||
        fwrite(index->pages, sizeof(*index->pages), index->page_count, fp) != index->page_count ||
        fwrite(index->lines, sizeof(*index->lines), index->line_count, fp) != index->line_count) {
        fclose(fp); remove(tmp); return 0;
    }
    fclose(fp);
    remove(path);
    return rename(tmp, path) == 0;
}

int index_load(PageIndex *index, TextFile *text, Layout layout, const char *path) {
    FILE *fp = fopen(path, "rb");
    IndexHeader h;
    if (!fp) return 0;
    if (fread(&h, sizeof(h), 1, fp) != 1 || h.magic != INDEX_MAGIC || h.version != INDEX_VERSION ||
        h.file_size != text->size || h.file_mtime != text->mtime || h.encoding != (uint32_t)text->encoding ||
        !same_layout((Layout){h.font_size,h.margin,h.lines_per_page,h.line_height}, layout) ||
        !h.page_count || h.page_count > 1000000 || h.line_count > 1000000) {
        fclose(fp); return 0;
    }
    index_free(index);
    index->pages = malloc(h.page_count * sizeof(*index->pages));
    index->lines = malloc(h.line_count * sizeof(*index->lines));
    if (!index->pages || (h.line_count && !index->lines) ||
        fread(index->pages, sizeof(*index->pages), h.page_count, fp) != h.page_count ||
        fread(index->lines, sizeof(*index->lines), h.line_count, fp) != h.line_count) {
        fclose(fp); index_free(index); return 0;
    }
    fclose(fp);
    index->page_count = index->page_capacity = h.page_count;
    index->line_count = index->line_capacity = h.line_count;
    index->total_source_lines = h.total_source_lines;
    index->file_size = h.file_size; index->file_mtime = h.file_mtime;
    index->encoding = (TextEncoding)h.encoding; index->layout = layout;
    return 1;
}

uint32_t index_page_for_offset(const PageIndex *index, uint32_t offset) {
    uint32_t lo = 0, hi = index->page_count;
    if (!index->page_count) return 0;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (index->pages[mid].byte_offset <= offset) lo = mid; else hi = mid;
    }
    return lo;
}

TextPosition index_position_for_page(const PageIndex *index, uint32_t page) {
    if (!index->page_count) return (TextPosition){0, 1};
    if (page >= index->page_count) page = index->page_count - 1;
    return index->pages[page];
}

TextPosition index_position_for_percent(const PageIndex *index, uint32_t percent) {
    uint32_t page;
    if (!index->page_count) return (TextPosition){0, 1};
    if (percent > 100) percent = 100;
    page = (uint32_t)(((uint64_t)(index->page_count - 1) * percent) / 100u);
    return index->pages[page];
}

TextPosition index_position_for_offset(const PageIndex *index, uint32_t offset) {
    return index_position_for_page(index, index_page_for_offset(index, offset));
}

TextPosition index_position_for_line(const PageIndex *index, uint32_t line) {
    uint32_t lo = 0, hi = index->line_count;
    if (!index->line_count) return (TextPosition){0, 1};
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (index->lines[mid].source_line <= line) lo = mid; else hi = mid;
    }
    return index->lines[lo];
}
