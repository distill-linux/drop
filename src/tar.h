#ifndef DISTILL_TAR_H
#define DISTILL_TAR_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} distill_tar_header;

typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} distill_file_list;

void distill_file_list_init(distill_file_list *list);
void distill_file_list_add(distill_file_list *list, const char *path);
void distill_file_list_free(distill_file_list *list);

/*
 * Extracts a .tar.gz stream from `in` directly to `target_prefix`.
 * - First entry MUST be .PORT. The .PORT file content is captured in *out_port_content.
 * - All extracted file relative paths are stored in *out_files.
 * - Computes SHA-256 of the raw incoming .tar.gz stream and writes 64 hex chars to out_sha256.
 * - Sanitizes all paths to prevent directory traversal (e.g. '../').
 * Returns 0 on success, non-zero on error.
 */
int distill_tar_gz_extract(FILE *in, const char *target_prefix,
                           char **out_port_content, size_t *out_port_len,
                           distill_file_list *out_files,
                           char out_sha256[65]);

/*
 * Creates a .tar.gz package at `archive_path` containing:
 * 1. The .PORT manifest (written as the VERY FIRST ENTRY).
 * 2. All files/directories/symlinks located inside `fakeroot_dir`.
 * Returns 0 on success, non-zero on error.
 */
int distill_tar_gz_create(const char *archive_path,
                          const char *fakeroot_dir,
                          const char *port_manifest_path);

#endif /* DISTILL_TAR_H */
