#define _GNU_SOURCE
#include "tar.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <zlib.h>

#define CHUNK_SIZE 16384
#define TAR_BLOCK_SIZE 512

void distill_file_list_init(distill_file_list *list) {
    list->paths = NULL;
    list->count = 0;
    list->capacity = 0;
}

void distill_file_list_add(distill_file_list *list, const char *path) {
    if (list->count + 1 > list->capacity) {
        size_t new_cap = list->capacity == 0 ? 32 : list->capacity * 2;
        char **new_paths = realloc(list->paths, new_cap * sizeof(char *));
        if (!new_paths) return;
        list->paths = new_paths;
        list->capacity = new_cap;
    }
    list->paths[list->count++] = strdup(path);
}

void distill_file_list_free(distill_file_list *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->paths[i]);
    }
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int is_safe_path(const char *path) {
    if (!path || path[0] == '\0') return 0;
    while (*path == '/') path++;
    if (path[0] == '\0') return 0;
    if (strncmp(path, "../", 3) == 0 || strcmp(path, "..") == 0) return 0;
    if (strstr(path, "/../") != NULL) return 0;
    size_t len = strlen(path);
    if (len >= 3 && strcmp(path + len - 3, "/..") == 0) return 0;
    return 1;
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int make_parent_dirs(const char *filepath) {
    char dirpath[PATH_MAX];
    strncpy(dirpath, filepath, sizeof(dirpath) - 1);
    dirpath[sizeof(dirpath) - 1] = '\0';
    char *slash = strrchr(dirpath, '/');
    if (!slash) return 0;
    *slash = '\0';
    return mkdir_p(dirpath, 0755);
}

static uint64_t parse_octal(const char *str, size_t len) {
    uint64_t val = 0;
    while (len > 0 && (*str == ' ' || *str == '\0')) {
        str++;
        len--;
    }
    while (len > 0 && *str >= '0' && *str <= '7') {
        val = (val << 3) | (*str - '0');
        str++;
        len--;
    }
    return val;
}

static int verify_tar_checksum(const distill_tar_header *header) {
    const unsigned char *p = (const unsigned char *)header;
    unsigned int sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        if (i >= 148 && i < 156) {
            sum += ' ';
        } else {
            sum += p[i];
        }
    }
    uint64_t expected = parse_octal(header->chksum, sizeof(header->chksum));
    return (sum == expected);
}

typedef struct {
    FILE *in;
    z_stream strm;
    distill_sha256_ctx sha_ctx;
    unsigned char in_buf[CHUNK_SIZE];
    unsigned char out_buf[CHUNK_SIZE];
    size_t out_avail;
    size_t out_offset;
    int eof_in;
    int stream_end;
} gz_stream_reader;

static int gz_reader_init(gz_stream_reader *r, FILE *in) {
    memset(r, 0, sizeof(*r));
    r->in = in;
    distill_sha256_init(&r->sha_ctx);
    r->strm.zalloc = Z_NULL;
    r->strm.zfree = Z_NULL;
    r->strm.opaque = Z_NULL;
    if (inflateInit2(&r->strm, 16 + MAX_WBITS) != Z_OK) {
        return -1;
    }
    return 0;
}

static ssize_t gz_reader_read(gz_stream_reader *r, void *dest, size_t count) {
    unsigned char *p = (unsigned char *)dest;
    size_t total = 0;

    while (total < count) {
        if (r->out_avail > 0) {
            size_t take = r->out_avail;
            if (take > count - total) take = count - total;
            memcpy(p + total, r->out_buf + r->out_offset, take);
            r->out_offset += take;
            r->out_avail -= take;
            total += take;
            continue;
        }

        if (r->stream_end) {
            break;
        }

        if (r->strm.avail_in == 0 && !r->eof_in) {
            size_t n = fread(r->in_buf, 1, sizeof(r->in_buf), r->in);
            if (n > 0) {
                distill_sha256_update(&r->sha_ctx, r->in_buf, n);
                r->strm.next_in = r->in_buf;
                r->strm.avail_in = (uInt)n;
            } else {
                r->eof_in = 1;
            }
        }

        r->strm.next_out = r->out_buf;
        r->strm.avail_out = sizeof(r->out_buf);
        int ret = inflate(&r->strm, Z_NO_FLUSH);
        r->out_offset = 0;
        r->out_avail = sizeof(r->out_buf) - r->strm.avail_out;

        if (ret == Z_STREAM_END) {
            r->stream_end = 1;
        } else if (ret != Z_OK && ret != Z_BUF_ERROR) {
            return -1;
        }

        if (r->out_avail == 0 && r->stream_end) {
            break;
        }
    }

    return (ssize_t)total;
}

static void gz_reader_finish(gz_stream_reader *r, char out_sha256[65]) {
    if (!r->eof_in) {
        size_t n;
        while ((n = fread(r->in_buf, 1, sizeof(r->in_buf), r->in)) > 0) {
            distill_sha256_update(&r->sha_ctx, r->in_buf, n);
        }
    }
    uint8_t digest[32];
    distill_sha256_final(&r->sha_ctx, digest);
    distill_sha256_to_hex(digest, out_sha256);
    inflateEnd(&r->strm);
}

int distill_tar_gz_extract(FILE *in, const char *target_prefix,
                           char **out_port_content, size_t *out_port_len,
                           distill_file_list *out_files,
                           char out_sha256[65]) {
    gz_stream_reader reader;
    if (gz_reader_init(&reader, in) != 0) {
        return -1;
    }

    if (out_port_content) *out_port_content = NULL;
    if (out_port_len) *out_port_len = 0;

    distill_tar_header hdr;
    int is_first_entry = 1;
    int zero_blocks = 0;
    int status = 0;

    while (1) {
        ssize_t n = gz_reader_read(&reader, &hdr, TAR_BLOCK_SIZE);
        if (n == 0) break;
        if (n != TAR_BLOCK_SIZE) {
            status = -1;
            break;
        }

        int all_zero = 1;
        const unsigned char *b = (const unsigned char *)&hdr;
        for (int i = 0; i < TAR_BLOCK_SIZE; ++i) {
            if (b[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            zero_blocks++;
            if (zero_blocks >= 2) break;
            continue;
        }
        zero_blocks = 0;

        if (!verify_tar_checksum(&hdr)) {
            fprintf(stderr, "tar: corrupt header checksum\n");
            status = -2;
            break;
        }

        char member_name[PATH_MAX];
        member_name[0] = '\0';
        if (hdr.prefix[0] != '\0') {
            size_t plen = strnlen(hdr.prefix, sizeof(hdr.prefix));
            size_t nlen = strnlen(hdr.name, sizeof(hdr.name));
            snprintf(member_name, sizeof(member_name), "%.*s/%.*s", (int)plen, hdr.prefix, (int)nlen, hdr.name);
        } else {
            size_t nlen = strnlen(hdr.name, sizeof(hdr.name));
            snprintf(member_name, sizeof(member_name), "%.*s", (int)nlen, hdr.name);
        }

        char *cleaned = member_name;
        while (*cleaned == '.' && *(cleaned + 1) == '/') cleaned += 2;
        while (*cleaned == '/') cleaned++;

        if (is_first_entry) {
            if (strcmp(cleaned, ".PORT") != 0) {
                fprintf(stderr, "tar: package violation: first entry must be .PORT (found '%s')\n", cleaned);
                status = -3;
                break;
            }
            is_first_entry = 0;
        }

        if (!is_safe_path(cleaned)) {
            fprintf(stderr, "tar: security violation: illegal path traversal detected ('%s')\n", cleaned);
            status = -4;
            break;
        }

        uint64_t file_size = parse_octal(hdr.size, sizeof(hdr.size));
        mode_t file_mode = (mode_t)parse_octal(hdr.mode, sizeof(hdr.mode));
        char typeflag = hdr.typeflag[0];
        if (typeflag == '\0') typeflag = '0';

        char dest_path[PATH_MAX * 2];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", target_prefix, cleaned);

        size_t padding = (TAR_BLOCK_SIZE - (file_size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;

        if (strcmp(cleaned, ".PORT") == 0) {
            char *port_buf = malloc(file_size + 1);
            if (!port_buf) {
                status = -5;
                break;
            }
            size_t rtotal = 0;
            while (rtotal < file_size) {
                ssize_t rd = gz_reader_read(&reader, port_buf + rtotal, file_size - rtotal);
                if (rd <= 0) {
                    free(port_buf);
                    status = -6;
                    break;
                }
                rtotal += rd;
            }
            if (status != 0) break;
            port_buf[file_size] = '\0';

            if (out_port_content) {
                *out_port_content = port_buf;
                if (out_port_len) *out_port_len = file_size;
            } else {
                free(port_buf);
            }

            if (padding > 0) {
                char pad_buf[TAR_BLOCK_SIZE];
                gz_reader_read(&reader, pad_buf, padding);
            }
            continue;
        }

        if (typeflag == '5' || cleaned[strlen(cleaned) - 1] == '/') {
            mkdir_p(dest_path, file_mode ? (file_mode & 07777) : 0755);
            if (out_files) distill_file_list_add(out_files, cleaned);
        } else if (typeflag == '2') {
            char target[PATH_MAX];
            snprintf(target, sizeof(target), "%.*s", (int)sizeof(hdr.linkname), hdr.linkname);
            make_parent_dirs(dest_path);
            unlink(dest_path);
            symlink(target, dest_path);
            if (out_files) distill_file_list_add(out_files, cleaned);
        } else if (typeflag == '0') {
            make_parent_dirs(dest_path);
            FILE *fout = fopen(dest_path, "wb");
            if (!fout) {
                fprintf(stderr, "tar: failed to open output file '%s': %s\n", dest_path, strerror(errno));
                status = -7;
                break;
            }

            size_t remaining = file_size;
            char chunk[8192];
            while (remaining > 0) {
                size_t to_read = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
                ssize_t rd = gz_reader_read(&reader, chunk, to_read);
                if (rd <= 0) {
                    fclose(fout);
                    status = -8;
                    break;
                }
                fwrite(chunk, 1, rd, fout);
                remaining -= rd;
            }
            fclose(fout);
            if (status != 0) break;

            if (file_mode) {
                chmod(dest_path, file_mode & 07777);
            }
            if (out_files) distill_file_list_add(out_files, cleaned);

            if (padding > 0) {
                char pad_buf[TAR_BLOCK_SIZE];
                gz_reader_read(&reader, pad_buf, padding);
            }
        } else {
            size_t total_skip = file_size + padding;
            char dummy[8192];
            while (total_skip > 0) {
                size_t to_skip = total_skip > sizeof(dummy) ? sizeof(dummy) : total_skip;
                ssize_t rd = gz_reader_read(&reader, dummy, to_skip);
                if (rd <= 0) break;
                total_skip -= rd;
            }
        }
    }

    gz_reader_finish(&reader, out_sha256);
    return status;
}

static void fill_tar_header(distill_tar_header *hdr, const char *name,
                            uint64_t size, mode_t mode, char typeflag,
                            const char *linkname) {
    memset(hdr, 0, sizeof(*hdr));

    if (strlen(name) > 99) {
        const char *slash = strchr(name + (strlen(name) - 99), '/');
        if (slash) {
            size_t plen = slash - name;
            size_t nlen = strlen(slash + 1);
            strncpy(hdr->prefix, name, plen > sizeof(hdr->prefix) ? sizeof(hdr->prefix) : plen);
            strncpy(hdr->name, slash + 1, nlen > sizeof(hdr->name) ? sizeof(hdr->name) : nlen);
        } else {
            strncpy(hdr->name, name, sizeof(hdr->name) - 1);
        }
    } else {
        strncpy(hdr->name, name, sizeof(hdr->name));
    }

    snprintf(hdr->mode, sizeof(hdr->mode), "%07o", (unsigned int)(mode & 07777));
    snprintf(hdr->uid, sizeof(hdr->uid), "%07o", 0);
    snprintf(hdr->gid, sizeof(hdr->gid), "%07o", 0);
    snprintf(hdr->size, sizeof(hdr->size), "%011llo", (unsigned long long)size);
    snprintf(hdr->mtime, sizeof(hdr->mtime), "%011llo", (unsigned long long)time(NULL));
    hdr->typeflag[0] = typeflag;

    if (linkname) {
        strncpy(hdr->linkname, linkname, sizeof(hdr->linkname) - 1);
    }

    memcpy(hdr->magic, "ustar", 5);
    hdr->magic[5] = '\0';
    hdr->version[0] = '0';
    hdr->version[1] = '0';
    strcpy(hdr->uname, "root");
    strcpy(hdr->gname, "root");

    memset(hdr->chksum, ' ', sizeof(hdr->chksum));
    unsigned int sum = 0;
    const unsigned char *p = (const unsigned char *)hdr;
    for (size_t i = 0; i < 512; ++i) {
        sum += p[i];
    }
    snprintf(hdr->chksum, sizeof(hdr->chksum), "%06o", sum);
    hdr->chksum[6] = '\0';
    hdr->chksum[7] = ' ';
}

static int gz_write_header_and_data(gzFile gz, distill_tar_header *hdr, FILE *data_file, uint64_t size) {
    if (gzwrite(gz, hdr, TAR_BLOCK_SIZE) != TAR_BLOCK_SIZE) return -1;
    if (data_file && size > 0) {
        char buf[8192];
        uint64_t remaining = size;
        while (remaining > 0) {
            size_t to_read = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            size_t n = fread(buf, 1, to_read, data_file);
            if (n == 0) return -1;
            if (gzwrite(gz, buf, (unsigned int)n) != (int)n) return -1;
            remaining -= n;
        }
    }
    size_t padding = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
    if (padding > 0) {
        char zeros[TAR_BLOCK_SIZE] = {0};
        if (gzwrite(gz, zeros, (unsigned int)padding) != (int)padding) return -1;
    }
    return 0;
}

static int archive_dir_recursive(gzFile gz, const char *base_dir, const char *rel_dir) {
    char current_full[PATH_MAX * 2];
    if (rel_dir && rel_dir[0] != '\0') {
        snprintf(current_full, sizeof(current_full), "%s/%s", base_dir, rel_dir);
    } else {
        snprintf(current_full, sizeof(current_full), "%s", base_dir);
    }

    DIR *d = opendir(current_full);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char ent_rel[PATH_MAX];
        if (rel_dir && rel_dir[0] != '\0') {
            snprintf(ent_rel, sizeof(ent_rel), "%s/%s", rel_dir, ent->d_name);
        } else {
            snprintf(ent_rel, sizeof(ent_rel), "%s", ent->d_name);
        }

        if (strcmp(ent_rel, ".PORT") == 0) {
            continue;
        }

        char ent_full[PATH_MAX * 2];
        snprintf(ent_full, sizeof(ent_full), "%s/%s", base_dir, ent_rel);

        struct stat st;
        if (lstat(ent_full, &st) != 0) continue;

        distill_tar_header hdr;
        if (S_ISDIR(st.st_mode)) {
            char dir_name[PATH_MAX * 2];
            snprintf(dir_name, sizeof(dir_name), "%s/", ent_rel);
            fill_tar_header(&hdr, dir_name, 0, st.st_mode, '5', NULL);
            if (gz_write_header_and_data(gz, &hdr, NULL, 0) != 0) {
                closedir(d);
                return -1;
            }
            if (archive_dir_recursive(gz, base_dir, ent_rel) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISLNK(st.st_mode)) {
            char linktarget[PATH_MAX];
            ssize_t len = readlink(ent_full, linktarget, sizeof(linktarget) - 1);
            if (len >= 0) {
                linktarget[len] = '\0';
                fill_tar_header(&hdr, ent_rel, 0, st.st_mode, '2', linktarget);
                if (gz_write_header_and_data(gz, &hdr, NULL, 0) != 0) {
                    closedir(d);
                    return -1;
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(ent_full, "rb");
            if (!f) continue;
            fill_tar_header(&hdr, ent_rel, st.st_size, st.st_mode, '0', NULL);
            int r = gz_write_header_and_data(gz, &hdr, f, st.st_size);
            fclose(f);
            if (r != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

int distill_tar_gz_create(const char *archive_path,
                          const char *fakeroot_dir,
                          const char *port_manifest_path) {
    gzFile gz = gzopen(archive_path, "wb9");
    if (!gz) {
        fprintf(stderr, "tar: unable to create archive '%s': %s\n", archive_path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (stat(port_manifest_path, &st) != 0) {
        fprintf(stderr, "tar: missing manifest '%s': %s\n", port_manifest_path, strerror(errno));
        gzclose(gz);
        return -1;
    }

    FILE *f_port = fopen(port_manifest_path, "rb");
    if (!f_port) {
        gzclose(gz);
        return -1;
    }

    distill_tar_header hdr;
    fill_tar_header(&hdr, ".PORT", st.st_size, 0644, '0', NULL);
    if (gz_write_header_and_data(gz, &hdr, f_port, st.st_size) != 0) {
        fclose(f_port);
        gzclose(gz);
        return -1;
    }
    fclose(f_port);

    if (archive_dir_recursive(gz, fakeroot_dir, "") != 0) {
        gzclose(gz);
        return -1;
    }

    char zero_blocks[TAR_BLOCK_SIZE * 2] = {0};
    gzwrite(gz, zero_blocks, sizeof(zero_blocks));

    gzclose(gz);
    return 0;
}
