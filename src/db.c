#define _GNU_SOURCE
#include "db.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

void drop_db_get_port_path(const char *root, const char *pkg_name, char *out_path, size_t sz) {
    if (!root || root[0] == '\0' || strcmp(root, "/") == 0) {
        snprintf(out_path, sz, "/var/db/drop/ports/%s/.PORT", pkg_name);
    } else {
        snprintf(out_path, sz, "%s/var/db/drop/ports/%s/.PORT", root, pkg_name);
    }
}

static void get_ports_dir(const char *root, char *out_path, size_t sz) {
    if (!root || root[0] == '\0' || strcmp(root, "/") == 0) {
        snprintf(out_path, sz, "/var/db/drop/ports");
    } else {
        snprintf(out_path, sz, "%s/var/db/drop/ports", root);
    }
}

int drop_db_save(const char *root, const distill_port *port) {
    char path[PATH_MAX * 2];
    drop_db_get_port_path(root, port->name, path, sizeof(path));
    return distill_port_save(path, port, 1 /* atomic */);
}

int drop_db_load(const char *root, const char *pkg_name, distill_port *out_port) {
    char path[PATH_MAX * 2];
    drop_db_get_port_path(root, pkg_name, path, sizeof(path));
    return distill_port_load(path, out_port);
}

int drop_db_is_installed(const char *root, const char *pkg_name) {
    char path[PATH_MAX * 2];
    drop_db_get_port_path(root, pkg_name, path, sizeof(path));
    return (access(path, F_OK) == 0);
}

static void clean_empty_parents(const char *root, const char *disk_path) {
    char full[PATH_MAX * 2];
    strncpy(full, disk_path, sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';

    size_t root_len = (root && strcmp(root, "/") != 0) ? strlen(root) : 0;

    char *slash = strrchr(full, '/');
    while (slash && (size_t)(slash - full) > root_len) {
        *slash = '\0';
        if (rmdir(full) != 0) {
            break;
        }
        slash = strrchr(full, '/');
    }
}

int drop_db_uninstall(const char *root, const char *pkg_name) {
    distill_port p;
    if (drop_db_load(root, pkg_name, &p) != 0) {
        fprintf(stderr, "drop: package '%s' is not installed\n", pkg_name);
        return -1;
    }

    printf("Removing %s-%s...\n", p.name, p.version);

    /* Delete files in reverse order */
    for (size_t i = p.files.count; i > 0; --i) {
        const port_file_entry *e = &p.files.entries[i - 1];
        const char *rel = e->path;
        while (*rel == '/') rel++;

        char full[PATH_MAX * 2];
        if (!root || root[0] == '\0' || strcmp(root, "/") == 0) {
            snprintf(full, sizeof(full), "/%s", rel);
        } else {
            snprintf(full, sizeof(full), "%s/%s", root, rel);
        }

        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                rmdir(full);
            } else {
                unlink(full);
            }
        }
        clean_empty_parents(root, full);
    }

    /* Remove .PORT manifest and port container directory */
    char manifest_path[PATH_MAX * 2];
    drop_db_get_port_path(root, pkg_name, manifest_path, sizeof(manifest_path));
    unlink(manifest_path);

    char *slash = strrchr(manifest_path, '/');
    if (slash) {
        *slash = '\0';
        rmdir(manifest_path);
    }

    distill_port_free(&p);
    printf("Successfully removed '%s'.\n", pkg_name);
    return 0;
}

int drop_db_list(const char *root) {
    char ports_dir[PATH_MAX * 2];
    get_ports_dir(root, ports_dir, sizeof(ports_dir));

    DIR *d = opendir(ports_dir);
    if (!d) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "drop: cannot open database directory '%s': %s\n", ports_dir, strerror(errno));
        return -1;
    }

    printf("%-20s %-12s %12s\n", "NAME", "VERSION", "SIZE");
    printf("%-20s %-12s %12s\n", "--------------------", "------------", "------------");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        distill_port p;
        if (drop_db_load(root, ent->d_name, &p) == 0) {
            char sz_buf[32];
            if (p.installed_size > 0) {
                snprintf(sz_buf, sizeof(sz_buf), "%llu B", (unsigned long long)p.installed_size);
            } else {
                snprintf(sz_buf, sizeof(sz_buf), "-");
            }
            printf("%-20s %-12s %12s\n", p.name, p.version, sz_buf);
            distill_port_free(&p);
        }
    }
    closedir(d);
    return 0;
}

static int check_single_port(const char *root, const char *pkg_name) {
    distill_port p;
    if (drop_db_load(root, pkg_name, &p) != 0) {
        fprintf(stderr, "drop: package '%s' is not installed\n", pkg_name);
        return -1;
    }

    printf("==> Checking %s-%s (%zu tracked entries)...\n", p.name, p.version, p.files.count);

    size_t ok_count = 0;
    size_t fail_count = 0;
    size_t missing_count = 0;

    for (size_t i = 0; i < p.files.count; ++i) {
        const port_file_entry *e = &p.files.entries[i];
        const char *rel = e->path;
        while (*rel == '/') rel++;

        char full[PATH_MAX * 2];
        if (!root || root[0] == '\0' || strcmp(root, "/") == 0) {
            snprintf(full, sizeof(full), "/%s", rel);
        } else {
            snprintf(full, sizeof(full), "%s/%s", root, rel);
        }

        struct stat st;
        if (lstat(full, &st) != 0) {
            printf("  [MISSING] %s\n", e->path);
            missing_count++;
            continue;
        }

        if (e->type == PORT_FILE_SYMLINK) {
            char target[PATH_MAX];
            ssize_t len = readlink(full, target, sizeof(target) - 1);
            if (len < 0) {
                printf("  [FAIL]    %s (cannot readlink: %s)\n", e->path, strerror(errno));
                fail_count++;
            } else {
                target[len] = '\0';
                if (e->symlink_target[0] != '\0' && strcmp(target, e->symlink_target) != 0) {
                    printf("  [FAIL]    %s -> %s (expected -> %s)\n", e->path, target, e->symlink_target);
                    fail_count++;
                } else {
                    ok_count++;
                }
            }
        } else if (e->type == PORT_FILE_DIR) {
            if (S_ISDIR(st.st_mode)) {
                ok_count++;
            } else {
                printf("  [FAIL]    %s (not a directory)\n", e->path);
                fail_count++;
            }
        } else {
            if (e->sha256[0] != '\0') {
                char hex[65] = {0};
                if (distill_sha256_file(full, hex) != 0) {
                    printf("  [FAIL]    %s (cannot read file for hash)\n", e->path);
                    fail_count++;
                } else if (strcasecmp(hex, e->sha256) != 0) {
                    printf("  [FAIL]    %s (checksum mismatch: expected %.8s..., got %.8s...)\n",
                           e->path, e->sha256, hex);
                    fail_count++;
                } else {
                    ok_count++;
                }
            } else {
                ok_count++;
            }
        }
    }

    printf("==> %s: %zu OK, %zu failed, %zu missing.\n",
           p.name, ok_count, fail_count, missing_count);

    distill_port_free(&p);
    return (fail_count == 0 && missing_count == 0) ? 0 : 1;
}

int drop_db_check(const char *root, const char *pkg_name) {
    if (pkg_name && pkg_name[0] != '\0') {
        return check_single_port(root, pkg_name);
    }

    char ports_dir[PATH_MAX * 2];
    get_ports_dir(root, ports_dir, sizeof(ports_dir));

    DIR *d = opendir(ports_dir);
    if (!d) {
        if (errno == ENOENT) {
            printf("drop: database is empty.\n");
            return 0;
        }
        fprintf(stderr, "drop: cannot open database directory '%s': %s\n", ports_dir, strerror(errno));
        return -1;
    }

    int overall_status = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (check_single_port(root, ent->d_name) != 0) {
            overall_status = 1;
        }
    }
    closedir(d);
    return overall_status;
}
