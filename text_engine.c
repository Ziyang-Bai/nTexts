#include "text_engine.h"
#include "app_log.h"
#include "gb18030_table.h"
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static uint16_t gb_pair_lookup(uint16_t key) {
    int lo = 0, hi = (int)ARRAY_LEN(gb_pairs) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (gb_pairs[mid].key == key) return gb_pairs[mid].value;
        if (gb_pairs[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0xfffd;
}

static uint16_t gb_range_lookup(unsigned pointer) {
    int lo = 0, hi = (int)ARRAY_LEN(gb_ranges) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (pointer < gb_ranges[mid].start) hi = mid - 1;
        else if (pointer > gb_ranges[mid].end) lo = mid + 1;
        else return (uint16_t)(gb_ranges[mid].unicode + pointer - gb_ranges[mid].start);
    }
    return 0xfffd;
}

int text_utf8_valid_sample(FILE *fp, uint32_t start, uint32_t max_bytes) {
    uint32_t read = 0;
    int c;
    fseek(fp, start, SEEK_SET);
    while (read < max_bytes && (c = fgetc(fp)) != EOF) {
        int need = 0, i;
        unsigned value, min;
        read++;
        if (c < 0x80) continue;
        if ((c & 0xe0) == 0xc0) { need = 1; value = c & 0x1f; min = 0x80; }
        else if ((c & 0xf0) == 0xe0) { need = 2; value = c & 0x0f; min = 0x800; }
        else if ((c & 0xf8) == 0xf0) { need = 3; value = c & 7; min = 0x10000; }
        else return 0;
        for (i = 0; i < need; ++i) {
            c = fgetc(fp); read++;
            if (c == EOF || (c & 0xc0) != 0x80) return 0;
            value = (value << 6) | (c & 0x3f);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}

int text_open(TextFile *text, const char *path) {
    struct stat st;
    unsigned char bom[3] = {0};
    memset(text, 0, sizeof(*text));
    app_log("text", "open begin %s", path ? path : "(null)");
    if (stat(path, &st)) {
        app_log("text", "stat failed %s", path ? path : "(null)");
        return 0;
    }
    text->fp = fopen(path, "rb");
    if (!text->fp) {
        app_log("text", "fopen failed %s", path ? path : "(null)");
        return 0;
    }
    strncpy(text->path, path, sizeof(text->path) - 1);
    text->size = (uint32_t)st.st_size;
    text->mtime = (uint32_t)st.st_mtime;
    fread(bom, 1, 3, text->fp);
    if (bom[0] == 0xef && bom[1] == 0xbb && bom[2] == 0xbf) {
        text->encoding = TEXT_UTF8; text->data_start = 3;
    } else {
        text->data_start = 0;
        text->encoding = text_utf8_valid_sample(text->fp, 0, 65536) ? TEXT_UTF8 : TEXT_GB18030;
    }
    app_log("text", "open ok size=%lu encoding=%s data_start=%lu",
            (unsigned long)text->size, text_encoding_name(text->encoding),
            (unsigned long)text->data_start);
    return 1;
}

void text_close(TextFile *text) {
    if (text->fp) fclose(text->fp);
    text->fp = NULL;
}

void text_decoder_at(TextFile *text, TextDecoder *decoder, TextPosition position) {
    decoder->text = text;
    decoder->offset = position.byte_offset < text->data_start ? text->data_start : position.byte_offset;
    if (decoder->offset > text->size) decoder->offset = text->size;
    decoder->line = position.source_line ? position.source_line : 1;
    fseek(text->fp, decoder->offset, SEEK_SET);
}

TextPosition text_safe_position(TextFile *text, uint32_t offset, uint32_t fallback_line) {
    TextPosition p;
    int c;
    uint32_t i;
    if (offset < text->data_start) offset = text->data_start;
    if (offset > text->size) offset = text->size;
    p.byte_offset = offset;
    p.source_line = fallback_line ? fallback_line : 1;
    if (text->encoding == TEXT_GB18030 || offset == text->data_start || offset == text->size) return p;
    fseek(text->fp, offset, SEEK_SET);
    c = fgetc(text->fp);
    if (c == EOF || ((unsigned char)c & 0xc0) != 0x80) return p;
    for (i = 1; i <= 3 && offset >= text->data_start + i; ++i) {
        fseek(text->fp, offset - i, SEEK_SET);
        c = fgetc(text->fp);
        if (c == EOF || ((unsigned char)c & 0xc0) != 0x80) {
            p.byte_offset = offset - i;
            return p;
        }
    }
    p.byte_offset = text->data_start;
    return p;
}

static int next_utf8(TextDecoder *d, int first, uint16_t *out) {
    int need, i, c;
    unsigned value, min;
    if (first < 0x80) { *out = (uint16_t)first; return 1; }
    if ((first & 0xe0) == 0xc0) { need = 1; value = first & 0x1f; min = 0x80; }
    else if ((first & 0xf0) == 0xe0) { need = 2; value = first & 0x0f; min = 0x800; }
    else if ((first & 0xf8) == 0xf0) { need = 3; value = first & 7; min = 0x10000; }
    else { *out = 0xfffd; return 1; }
    for (i = 0; i < need; ++i) {
        c = fgetc(d->text->fp);
        if (c == EOF || (c & 0xc0) != 0x80) {
            if (c != EOF) ungetc(c, d->text->fp);
            *out = 0xfffd; return 1;
        }
        d->offset++;
        value = (value << 6) | (c & 0x3f);
    }
    if (value < min || value > 0xffff || (value >= 0xd800 && value <= 0xdfff)) value = 0xfffd;
    *out = (uint16_t)value;
    return 1;
}

static int next_gb(TextDecoder *d, int first, uint16_t *out) {
    int b2, b3, b4;
    if (first < 0x80) { *out = (uint16_t)first; return 1; }
    if (first < 0x81 || first > 0xfe) { *out = 0xfffd; return 1; }
    b2 = fgetc(d->text->fp);
    if (b2 == EOF) { *out = 0xfffd; return 1; }
    d->offset++;
    if (b2 >= 0x30 && b2 <= 0x39) {
        b3 = fgetc(d->text->fp); b4 = fgetc(d->text->fp);
        if (b3 == EOF || b4 == EOF) { *out = 0xfffd; return 1; }
        d->offset += 2;
        if (b3 >= 0x81 && b3 <= 0xfe && b4 >= 0x30 && b4 <= 0x39) {
            unsigned p = (((first - 0x81) * 10u + (b2 - 0x30)) * 126u + (b3 - 0x81)) * 10u + (b4 - 0x30);
            *out = gb_range_lookup(p); return 1;
        }
        *out = 0xfffd; return 1;
    }
    *out = gb_pair_lookup((uint16_t)((first << 8) | b2));
    return 1;
}

int text_next(TextDecoder *decoder, uint16_t *value, uint32_t *char_offset) {
    int first;
    if (decoder->offset >= decoder->text->size) return 0;
    if (char_offset) *char_offset = decoder->offset;
    first = fgetc(decoder->text->fp);
    if (first == EOF) return 0;
    decoder->offset++;
    if (decoder->text->encoding == TEXT_UTF8) next_utf8(decoder, first, value);
    else next_gb(decoder, first, value);
    if (*value == '\n') decoder->line++;
    return 1;
}

const char *text_encoding_name(TextEncoding encoding) {
    return encoding == TEXT_UTF8 ? "UTF-8" : "GB18030";
}

uint16_t text_fold_ascii(uint16_t value) {
    return value >= 'A' && value <= 'Z' ? (uint16_t)(value + ('a' - 'A')) : value;
}
