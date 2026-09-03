#define _GNU_SOURCE
#include "port.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

void port_file_list_init(port_file_list *list) {
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

void port_file_list_free(port_file_list *list) {
    if (!list) return;
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

void port_file_list_add(port_file_list *list, const char *path, const char *sha256,
                        const char *symlink_target, port_file_type type, uint64_t size) {
    if (list->count + 1 > list->capacity) {
        size_t ncap = list->capacity == 0 ? 32 : list->capacity * 2;
        port_file_entry *ne = realloc(list->entries, ncap * sizeof(port_file_entry));
        if (!ne) return;
        list->entries = ne;
        list->capacity = ncap;
    }
    port_file_entry *e = &list->entries[list->count++];
    memset(e, 0, sizeof(*e));
    if (path) {
        strncpy(e->path, path, sizeof(e->path) - 1);
    }
    if (sha256) {
        strncpy(e->sha256, sha256, sizeof(e->sha256) - 1);
    }
    if (symlink_target) {
        strncpy(e->symlink_target, symlink_target, sizeof(e->symlink_target) - 1);
    }
    e->type = type;
    e->size = size;
}

void distill_port_init(distill_port *p) {
    memset(p->name, 0, sizeof(p->name));
    memset(p->version, 0, sizeof(p->version));
    memset(p->release, 0, sizeof(p->release));
    memset(p->desc, 0, sizeof(p->desc));
    memset(p->url, 0, sizeof(p->url));
    memset(p->commit, 0, sizeof(p->commit));
    memset(p->build_system, 0, sizeof(p->build_system));
    memset(p->build_deps, 0, sizeof(p->build_deps));
    memset(p->run_deps, 0, sizeof(p->run_deps));
    p->build_script = NULL;
    p->build_script_len = 0;
    p->timestamp = 0;
    p->installed_size = 0;
    port_file_list_init(&p->files);
}

void distill_port_free(distill_port *p) {
    if (!p) return;
    free(p->build_script);
    p->build_script = NULL;
    p->build_script_len = 0;
    port_file_list_free(&p->files);
}

static char *trim_in_place(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static void unquote(char *val) {
    size_t len = strlen(val);
    if (len >= 2) {
        if ((val[0] == '"' && val[len - 1] == '"') ||
            (val[0] == '\'' && val[len - 1] == '\'')) {
            memmove(val, val + 1, len - 2);
            val[len - 2] = '\0';
        }
    }
}

static void parse_file_line(port_file_list *list, const char *raw_line) {
    char line[2048];
    strncpy(line, raw_line, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    char *trimmed = trim_in_place(line);
    if (trimmed[0] == '\0') return;

    /* Format 1: /path:SYMLINK:target */
    char *sym = strstr(trimmed, ":SYMLINK:");
    if (sym) {
        *sym = '\0';
        const char *target = sym + 9;
        port_file_list_add(list, trimmed, "", target, PORT_FILE_SYMLINK, 0);
        return;
    }

    /* Format 2: /path/:DIR or ending with / */
    char *dir_tag = strstr(trimmed, ":DIR");
    if (dir_tag) {
        *dir_tag = '\0';
        port_file_list_add(list, trimmed, "", "", PORT_FILE_DIR, 0);
        return;
    }
    size_t tlen = strlen(trimmed);
    if (tlen > 0 && trimmed[tlen - 1] == '/') {
        port_file_list_add(list, trimmed, "", "", PORT_FILE_DIR, 0);
        return;
    }

    /* Format 3: /path:sha256hash... */
    char *colon = strrchr(trimmed, ':');
    if (colon) {
        *colon = '\0';
        const char *sha = colon + 1;
        port_file_list_add(list, trimmed, sha, "", PORT_FILE_REGULAR, 0);
        return;
    }

    /* Format 4: legacy plain path */
    port_file_list_add(list, trimmed, "", "", PORT_FILE_REGULAR, 0);
}

int distill_port_parse(const char *data, size_t len, distill_port *out_port) {
    if (!data || !out_port) return -1;

    distill_port_init(out_port);

    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, data, len);
    copy[len] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\r\n", &saveptr);
    int in_build = 0;
    int in_files = 0;
    size_t script_cap = 0;

    while (line) {
        if (in_build) {
            if (strncmp(line, "FILES:", 6) == 0) {
                in_build = 0;
                in_files = 1;
                line = strtok_r(NULL, "\r\n", &saveptr);
                continue;
            }
            size_t llen = strlen(line);
            if (out_port->build_script_len + llen + 2 > script_cap) {
                size_t ncap = script_cap == 0 ? 2048 : script_cap * 2;
                while (out_port->build_script_len + llen + 2 > ncap) ncap *= 2;
                char *ns = realloc(out_port->build_script, ncap);
                if (!ns) {
                    free(copy);
                    return -1;
                }
                out_port->build_script = ns;
                script_cap = ncap;
            }
            memcpy(out_port->build_script + out_port->build_script_len, line, llen);
            out_port->build_script_len += llen;
            out_port->build_script[out_port->build_script_len++] = '\n';
            out_port->build_script[out_port->build_script_len] = '\0';
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (in_files) {
            parse_file_line(&out_port->files, line);
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        char *p = trim_in_place(line);
        if (*p == '#' || *p == '\0') {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (strncmp(p, "BUILD:", 6) == 0) {
            in_build = 1;
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (strncmp(p, "FILES:", 6) == 0) {
            in_files = 1;
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char *key = trim_in_place(p);
            char *val = trim_in_place(eq + 1);
            unquote(val);

            if (strcmp(key, "PORT_NAME") == 0 || strcmp(key, "NAME") == 0) {
                strncpy(out_port->name, val, sizeof(out_port->name) - 1);
            } else if (strcmp(key, "PORT_VERSION") == 0 || strcmp(key, "VERSION") == 0) {
                strncpy(out_port->version, val, sizeof(out_port->version) - 1);
            } else if (strcmp(key, "PORT_RELEASE") == 0 || strcmp(key, "RELEASE") == 0) {
                strncpy(out_port->release, val, sizeof(out_port->release) - 1);
            } else if (strcmp(key, "PORT_DESC") == 0 || strcmp(key, "DESC") == 0) {
                strncpy(out_port->desc, val, sizeof(out_port->desc) - 1);
            } else if (strcmp(key, "PORT_URL") == 0 || strcmp(key, "PORT_REPO") == 0 || strcmp(key, "URL") == 0) {
                strncpy(out_port->url, val, sizeof(out_port->url) - 1);
            } else if (strcmp(key, "PORT_COMMIT") == 0 || strcmp(key, "COMMIT") == 0) {
                strncpy(out_port->commit, val, sizeof(out_port->commit) - 1);
            } else if (strcmp(key, "BUILD_SYSTEM") == 0) {
                strncpy(out_port->build_system, val, sizeof(out_port->build_system) - 1);
            } else if (strcmp(key, "BUILD_DEPS") == 0) {
                strncpy(out_port->build_deps, val, sizeof(out_port->build_deps) - 1);
            } else if (strcmp(key, "RUN_DEPS") == 0 || strcmp(key, "DEPENDS") == 0) {
                strncpy(out_port->run_deps, val, sizeof(out_port->run_deps) - 1);
            } else if (strcmp(key, "TIMESTAMP") == 0) {
                out_port->timestamp = strtoull(val, NULL, 10);
            } else if (strcmp(key, "INSTALLED_SIZE") == 0 || strcmp(key, "SIZE") == 0) {
                out_port->installed_size = strtoull(val, NULL, 10);
            }
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(copy);
    return (out_port->name[0] != '\0') ? 0 : -1;
}

int distill_port_load(const char *filepath, distill_port *out_port) {
    if (!filepath || !out_port) return -1;
    FILE *f = fopen(filepath, "rb");
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

    int res = distill_port_parse(buf, n, out_port);
    free(buf);
    return res;
}

static int mkdir_p_for_file(const char *filepath) {
    char tmp[PATH_MAX * 2];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int distill_port_save(const char *filepath, const distill_port *port, int atomic) {
    if (!filepath || !port) return -1;

    mkdir_p_for_file(filepath);

    char target_path[PATH_MAX * 2];
    if (atomic) {
        snprintf(target_path, sizeof(target_path), "%s.tmp", filepath);
    } else {
        snprintf(target_path, sizeof(target_path), "%s", filepath);
    }

    FILE *f = fopen(target_path, "w");
    if (!f) return -1;

    fprintf(f, "PORT_NAME=\"%s\"\n", port->name);
    fprintf(f, "PORT_VERSION=\"%s\"\n", port->version);
    if (port->release[0] != '\0') {
        fprintf(f, "PORT_RELEASE=\"%s\"\n", port->release);
    } else {
        fprintf(f, "PORT_RELEASE=\"1\"\n");
    }
    if (port->desc[0] != '\0') {
        fprintf(f, "PORT_DESC=\"%s\"\n", port->desc);
    }
    if (port->url[0] != '\0') {
        fprintf(f, "PORT_URL=\"%s\"\n", port->url);
    }
    if (port->commit[0] != '\0') {
        fprintf(f, "PORT_COMMIT=\"%s\"\n", port->commit);
    }
    if (port->build_system[0] != '\0') {
        fprintf(f, "BUILD_SYSTEM=\"%s\"\n", port->build_system);
    }
    if (port->build_deps[0] != '\0') {
        fprintf(f, "BUILD_DEPS=\"%s\"\n", port->build_deps);
    }
    fprintf(f, "RUN_DEPS=\"%s\"\n", port->run_deps);

    if (port->timestamp > 0) {
        fprintf(f, "TIMESTAMP=\"%llu\"\n", (unsigned long long)port->timestamp);
    }
    if (port->installed_size > 0) {
        fprintf(f, "INSTALLED_SIZE=\"%llu\"\n", (unsigned long long)port->installed_size);
    }

    if (port->build_script && port->build_script_len > 0) {
        fprintf(f, "\nBUILD:\n%s", port->build_script);
        if (port->build_script[port->build_script_len - 1] != '\n') {
            fprintf(f, "\n");
        }
    }

    if (port->files.count > 0) {
        fprintf(f, "\nFILES:\n");
        for (size_t i = 0; i < port->files.count; ++i) {
            const port_file_entry *e = &port->files.entries[i];
            if (e->type == PORT_FILE_SYMLINK) {
                fprintf(f, "%s:SYMLINK:%s\n", e->path, e->symlink_target);
            } else if (e->type == PORT_FILE_DIR) {
                fprintf(f, "%s:DIR\n", e->path);
            } else {
                if (e->sha256[0] != '\0') {
                    fprintf(f, "%s:%s\n", e->path, e->sha256);
                } else {
                    fprintf(f, "%s\n", e->path);
                }
            }
        }
    }

    fflush(f);
    fclose(f);

    if (atomic) {
        if (rename(target_path, filepath) != 0) {
            unlink(target_path);
            return -1;
        }
    }

    return 0;
}

static int scan_fakeroot_rec(const char *base_dir, const char *rel_dir,
                             port_file_list *out_files, uint64_t *out_total_size) {
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
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char ent_rel[PATH_MAX * 2];
        if (rel_dir && rel_dir[0] != '\0') {
            snprintf(ent_rel, sizeof(ent_rel), "%s/%s", rel_dir, ent->d_name);
        } else {
            snprintf(ent_rel, sizeof(ent_rel), "%s", ent->d_name);
        }

        if (strcmp(ent_rel, ".PORT") == 0) continue;

        char ent_full[PATH_MAX * 4];
        snprintf(ent_full, sizeof(ent_full), "%s/%s", base_dir, ent_rel);

        struct stat st;
        if (lstat(ent_full, &st) != 0) continue;

        char entry_path[PATH_MAX * 4];
        if (ent_rel[0] == '/') {
            snprintf(entry_path, sizeof(entry_path), "%s", ent_rel);
        } else {
            snprintf(entry_path, sizeof(entry_path), "/%s", ent_rel);
        }

        if (S_ISDIR(st.st_mode)) {
            port_file_list_add(out_files, entry_path, "", "", PORT_FILE_DIR, 0);
            scan_fakeroot_rec(base_dir, ent_rel, out_files, out_total_size);
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t len = readlink(ent_full, target, sizeof(target) - 1);
            if (len >= 0) {
                target[len] = '\0';
                port_file_list_add(out_files, entry_path, "", target, PORT_FILE_SYMLINK, (uint64_t)st.st_size);
                if (out_total_size) *out_total_size += (uint64_t)st.st_size;
            }
        } else if (S_ISREG(st.st_mode)) {
            char hex[65] = {0};
            if (distill_sha256_file(ent_full, hex) == 0) {
                port_file_list_add(out_files, entry_path, hex, "", PORT_FILE_REGULAR, (uint64_t)st.st_size);
                if (out_total_size) *out_total_size += (uint64_t)st.st_size;
            }
        }
    }

    closedir(d);
    return 0;
}

int distill_port_scan_fakeroot(const char *fakeroot_dir, port_file_list *out_files, uint64_t *out_total_size) {
    if (!fakeroot_dir || !out_files) return -1;
    if (out_total_size) *out_total_size = 0;
    return scan_fakeroot_rec(fakeroot_dir, "", out_files, out_total_size);
}
