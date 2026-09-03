#ifndef DISTILL_DROP_INDEX_H
#define DISTILL_DROP_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[128];
    char version[64];
    char sha256[65];
    uint64_t size;
    char filename[256];
    char depends[512];
} drop_index_entry;

typedef struct {
    drop_index_entry *entries;
    size_t count;
    size_t capacity;
} drop_repo_index;

void drop_repo_index_init(drop_repo_index *idx);
void drop_repo_index_free(drop_repo_index *idx);
int drop_repo_index_parse(const char *data, size_t len, drop_repo_index *out_idx);
const drop_index_entry *drop_repo_index_find(const drop_repo_index *idx, const char *name);
int drop_repo_fetch_index(const char *repo_url, const char *cache_path, drop_repo_index *out_idx);

#endif /* DISTILL_DROP_INDEX_H */
