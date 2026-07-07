#include "storage.h"
#include <libndls.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define APP_MAGIC 0x5053544eu
#define BOOK_MAGIC 0x4b42544eu
#define STATE_VERSION 2u

uint32_t storage_hash_path(const char *path) {
    uint32_t h = 2166136261u;
    while (*path) { h ^= (unsigned char)*path++; h *= 16777619u; }
    return h;
}

static void storage_join_path(char *out, size_t capacity, const char *base, const char *leaf) {
    size_t len;
    if (!capacity) return;
    if (!base) base = "";
    if (!leaf) leaf = "";
    len = strlen(base);
    snprintf(out, capacity, "%s%s%s", base,
             (len && base[len - 1] != '/' && base[len - 1] != '\\') ? "/" : "",
             leaf);
}

int storage_init(char *directory, size_t capacity) {
    struct stat st;
    const char *documents = get_documents_dir();
    int mkdir_errno;
    if (!documents || !*documents) {
        errno = ENOENT;
        if (capacity) directory[0] = 0;
        return 0;
    }
    storage_join_path(directory, capacity, documents, "nTexts");
    errno = 0;
    if (mkdir(directory, 0777) == 0) return 1;
    mkdir_errno = errno;
    errno = 0;
    if (stat(directory, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = 0;
        return 1;
    }
    errno = mkdir_errno ? mkdir_errno : (errno ? errno : ENOTDIR);
    return 0;
}

void storage_book_path(char *out, size_t capacity, const char *directory, const char *book, const char *suffix) {
    snprintf(out, capacity, "%s/%08lx.%s", directory, (unsigned long)storage_hash_path(book), suffix);
}

static int atomic_write(const char *path, const void *data, size_t size) {
    char tmp[600];
    FILE *fp;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) return 0;
    if (fwrite(data, 1, size, fp) != size) { fclose(fp); remove(tmp); return 0; }
    fclose(fp); remove(path);
    return rename(tmp, path) == 0;
}

void storage_app_defaults(AppState *state) {
    memset(state, 0, sizeof(*state));
    state->magic = APP_MAGIC; state->version = STATE_VERSION;
    state->font_choice = 1; state->theme = 0; state->margin_choice = 0;
}

int storage_load_app(AppState *state, const char *directory) {
    char path[600]; FILE *fp;
    snprintf(path, sizeof(path), "%s/app.state", directory);
    fp = fopen(path, "rb");
    if (!fp || fread(state, sizeof(*state), 1, fp) != 1 || state->magic != APP_MAGIC ||
        state->version != STATE_VERSION || state->recent_count > MAX_RECENTS || state->history_count > MAX_HISTORY) {
        if (fp) fclose(fp);
        storage_app_defaults(state);
        return 0;
    }
    fclose(fp); return 1;
}

int storage_save_app(const AppState *state, const char *directory) {
    char path[600]; snprintf(path, sizeof(path), "%s/app.state", directory);
    return atomic_write(path, state, sizeof(*state));
}

void storage_book_defaults(BookState *state, TextFile *text) {
    memset(state, 0, sizeof(*state)); state->magic = BOOK_MAGIC; state->version = STATE_VERSION;
    strncpy(state->path, text->path, sizeof(state->path) - 1);
    state->file_size = text->size; state->file_mtime = text->mtime;
    state->progress.byte_offset = text->data_start; state->progress.source_line = 1;
}

int storage_load_book(BookState *state, TextFile *text, const char *directory) {
    char path[600]; FILE *fp;
    Bookmark kept[MAX_BOOKMARKS];
    uint32_t kept_count = 0, i;
    storage_book_path(path, sizeof(path), directory, text->path, "state");
    fp = fopen(path, "rb");
    if (!fp || fread(state, sizeof(*state), 1, fp) != 1 || state->magic != BOOK_MAGIC ||
        state->version != STATE_VERSION || strcmp(state->path, text->path) || state->bookmark_count > MAX_BOOKMARKS) {
        if (fp) fclose(fp);
        storage_book_defaults(state, text);
        return 0;
    }
    fclose(fp);
    if (state->file_size != text->size || state->file_mtime != text->mtime) {
        for (i = 0; i < state->bookmark_count; ++i) {
            kept[kept_count] = state->bookmarks[i];
            if (kept[kept_count].position.byte_offset > text->size)
                kept[kept_count].position.byte_offset = text->size;
            if (!kept[kept_count].position.source_line)
                kept[kept_count].position.source_line = 1;
            kept_count++;
        }
        storage_book_defaults(state, text);
        state->bookmark_count = kept_count;
        for (i = 0; i < kept_count; ++i) state->bookmarks[i] = kept[i];
        return 1;
    }
    if (state->progress.byte_offset > text->size) state->progress.byte_offset = text->size;
    state->file_size = text->size; state->file_mtime = text->mtime;
    return 1;
}

int storage_save_book(const BookState *state, const char *directory) {
    char path[600]; storage_book_path(path, sizeof(path), directory, state->path, "state");
    return atomic_write(path, state, sizeof(*state));
}

void storage_touch_recent(AppState *state, const BookState *book) {
    uint32_t i, found = state->recent_count;
    if (!book->path[0]) return;
    for (i = 0; i < state->recent_count; ++i) if (!strcmp(state->recents[i].path, book->path)) { found = i; break; }
    if (found >= MAX_RECENTS) found = MAX_RECENTS - 1;
    for (i = found; i > 0; --i) state->recents[i] = state->recents[i - 1];
    memset(&state->recents[0], 0, sizeof(state->recents[0]));
    strncpy(state->recents[0].path, book->path, sizeof(state->recents[0].path) - 1);
    state->recents[0].offset = book->progress.byte_offset; state->recents[0].file_size = book->file_size;
    if (state->recent_count < MAX_RECENTS) state->recent_count++;
}

int storage_prune_recents(AppState *state) {
    uint32_t write = 0;
    uint32_t kept;
    int changed = 0;
    for (uint32_t read = 0; read < state->recent_count && read < MAX_RECENTS; ++read) {
        struct stat st;
        RecentBook keep = state->recents[read];
        if (!keep.path[0] || stat(keep.path, &st) != 0 || !S_ISREG(st.st_mode)) {
            changed = 1;
            continue;
        }
        if (write != read) {
            state->recents[write] = keep;
            changed = 1;
        }
        write++;
    }
    kept = write;
    while (write < MAX_RECENTS) {
        if (state->recents[write].path[0]) changed = 1;
        memset(&state->recents[write], 0, sizeof(state->recents[write]));
        write++;
    }
    if (state->recent_count != kept) changed = 1;
    state->recent_count = kept <= MAX_RECENTS ? kept : MAX_RECENTS;
    return changed;
}

void storage_add_history(AppState *state, const uint16_t *query) {
    uint32_t i, found = state->history_count;
    for (i = 0; i < state->history_count; ++i)
        if (!memcmp(state->history[i], query, QUERY_LEN * sizeof(uint16_t))) { found = i; break; }
    if (found >= MAX_HISTORY) found = MAX_HISTORY - 1;
    for (i = found; i > 0; --i) memcpy(state->history[i], state->history[i - 1], sizeof(state->history[i]));
    memcpy(state->history[0], query, sizeof(state->history[0]));
    if (state->history_count < MAX_HISTORY) state->history_count++;
}
