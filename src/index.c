#define _GNU_SOURCE
#include "index.h"
#include "spawn_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

void drop_repo_index_init(drop_repo_index *idx) {
    idx->entries = NULL;
    idx->count = 0;
    idx->capacity = 0;
}

void drop_repo_index_free(drop_repo_index *idx) {
    if (!idx) return;
    free(idx->entries);
    idx->entries = NULL;
    idx->count = 0;
    idx->capacity = 0;
}

static void add_entry(drop_repo_index *idx, const drop_index_entry *entry) {
    if (idx->count + 1 > idx->capacity) {
        size_t new_cap = idx->capacity == 0 ? 16 : idx->capacity * 2;
        drop_index_entry *ne = realloc(idx->entries, new_cap * sizeof(drop_index_entry));
        if (!ne) return;
        idx->entries = ne;
        idx->capacity = new_cap;
    }
    idx->entries[idx->count++] = *entry;
}

int drop_repo_index_parse(const char *data, size_t len, drop_repo_index *out_idx) {
    if (!data || !out_idx) return -1;

    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, data, len);
    copy[len] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\r\n", &saveptr);

    while (line) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line != '#' && *line != '\0') {
            drop_index_entry e;
            memset(&e, 0, sizeof(e));

            char *tabsave = NULL;
            char *name = strtok_r(line, "\t", &tabsave);
            char *ver = strtok_r(NULL, "\t", &tabsave);
            char *sha = strtok_r(NULL, "\t", &tabsave);
            char *sz = strtok_r(NULL, "\t", &tabsave);
            char *fn = strtok_r(NULL, "\t", &tabsave);
            char *deps = strtok_r(NULL, "\t", &tabsave);

            if (name && ver && sha && fn) {
                strncpy(e.name, name, sizeof(e.name) - 1);
                strncpy(e.version, ver, sizeof(e.version) - 1);
                strncpy(e.sha256, sha, sizeof(e.sha256) - 1);
                e.size = sz ? strtoull(sz, NULL, 10) : 0;
                strncpy(e.filename, fn, sizeof(e.filename) - 1);
                if (deps) {
                    strncpy(e.depends, deps, sizeof(e.depends) - 1);
                }
                add_entry(out_idx, &e);
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(copy);
    return 0;
}

const drop_index_entry *drop_repo_index_find(const drop_repo_index *idx, const char *name) {
    if (!idx || !name) return NULL;
    for (size_t i = 0; i < idx->count; ++i) {
        if (strcmp(idx->entries[i].name, name) == 0) {
            return &idx->entries[i];
        }
    }
    return NULL;
}

static int read_entire_file(const char *path, char **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 0;
}

int drop_repo_fetch_index(const char *repo_url, const char *cache_path, drop_repo_index *out_idx) {
    if (!repo_url || !out_idx) return -1;

    char *buf = NULL;
    size_t len = 0;

    if (strncmp(repo_url, "http://", 7) == 0 || strncmp(repo_url, "https://", 8) == 0) {
        char url[2048];
        snprintf(url, sizeof(url), "%s/index.tsv", repo_url);

        char *argv[] = {"curl", "-sSL", url, NULL};
        int status = 0;
        int r = distill_spawn_capture_stdout(argv, NULL, NULL, &buf, &len, &status);
        if (r != 0 || status != 0 || !buf) {
            fprintf(stderr, "drop: error fetching repository index from %s\n", url);
            free(buf);
            return -1;
        }

        if (cache_path) {
            FILE *fc = fopen(cache_path, "wb");
            if (fc) {
                fwrite(buf, 1, len, fc);
                fclose(fc);
            }
        }
    } else {
        char path[2048];
        const char *actual_repo = repo_url;
        if (strncmp(actual_repo, "file://", 7) == 0) {
            actual_repo += 7;
        }

        struct stat st;
        if (stat(actual_repo, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(path, sizeof(path), "%s/index.tsv", actual_repo);
        } else {
            snprintf(path, sizeof(path), "%s", actual_repo);
        }

        if (read_entire_file(path, &buf, &len) != 0) {
            fprintf(stderr, "drop: unable to open local repository index at %s: %s\n", path, strerror(errno));
            return -1;
        }
    }

    int res = drop_repo_index_parse(buf, len, out_idx);
    free(buf);
    return res;
}
