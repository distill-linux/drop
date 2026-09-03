#define _GNU_SOURCE
#include "db.h"
#include "index.h"
#include "extractor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#define DEFAULT_REPO_URL "https://repo.distilllinux.org/packages"
#define DEFAULT_ROOT_DIR "/"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Distill Linux Binary Package Manager (drop)\n"
        "Usage: %s [options] <verb> [arguments]\n\n"
        "Core verbs:\n"
        "  in, install <pkg.drop | name>   Install package from archive or repository\n"
        "  rm, remove <name>               Uninstall package and tracked artifacts\n"
        "  ls, list                        List installed packages, versions, and sizes\n"
        "  info <name>                     Display package metadata from database/catalog\n"
        "  check [name]                    Audit disk contents against stored SHA-256 hashes\n"
        "  update, sync                    Update repository index\n"
        "  help                            Show this help screen\n\n"
        "Options:\n"
        "  -r, --root <path>               Target root prefix (default: /)\n"
        "  --repo <url>                    Repository URL or local path\n",
        prog);
}

int main(int argc, char *argv[]) {
    const char *root_dir = getenv("DROP_ROOT");
    if (!root_dir || root_dir[0] == '\0') root_dir = DEFAULT_ROOT_DIR;

    const char *repo_url = getenv("DROP_REPO_URL");
    if (!repo_url || repo_url[0] == '\0') repo_url = DEFAULT_REPO_URL;

    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "-r") == 0 || strcmp(argv[arg_idx], "--root") == 0) {
            if (arg_idx + 1 >= argc) {
                fprintf(stderr, "drop: option '%s' requires an argument\n", argv[arg_idx]);
                return 1;
            }
            root_dir = argv[++arg_idx];
        } else if (strcmp(argv[arg_idx], "--repo") == 0) {
            if (arg_idx + 1 >= argc) {
                fprintf(stderr, "drop: option '--repo' requires an argument\n");
                return 1;
            }
            repo_url = argv[++arg_idx];
        } else if (strcmp(argv[arg_idx], "-h") == 0 || strcmp(argv[arg_idx], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "drop: unrecognized option '%s'\n", argv[arg_idx]);
            print_usage(argv[0]);
            return 1;
        }
        arg_idx++;
    }

    if (arg_idx >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char *verb = argv[arg_idx++];

    char index_cache[PATH_MAX * 4];
    if (strcmp(root_dir, "/") == 0) {
        snprintf(index_cache, sizeof(index_cache), "/var/db/drop/index.tsv");
    } else {
        snprintf(index_cache, sizeof(index_cache), "%s/var/db/drop/index.tsv", root_dir);
    }

    if (strcmp(verb, "in") == 0 || strcmp(verb, "install") == 0) {
        if (arg_idx >= argc) {
            fprintf(stderr, "drop: 'in' requires a package name or .drop archive path\n");
            return 1;
        }
        const char *target = argv[arg_idx];

        /* Check if local archive file */
        size_t tlen = strlen(target);
        int is_archive = (tlen > 5 && strcmp(target + tlen - 5, ".drop") == 0) ||
                         (tlen > 7 && strcmp(target + tlen - 7, ".tar.gz") == 0) ||
                         (strchr(target, '/') != NULL) ||
                         (access(target, F_OK) == 0);

        if (is_archive) {
            return drop_install_local_file(target, root_dir, NULL);
        }

        /* Remote / repository index lookup */
        drop_repo_index idx;
        drop_repo_index_init(&idx);

        if (drop_repo_fetch_index(repo_url, index_cache, &idx) != 0) {
            if (access(index_cache, R_OK) != 0 || drop_repo_fetch_index(index_cache, NULL, &idx) != 0) {
                fprintf(stderr, "drop: cannot fetch repository index\n");
                drop_repo_index_free(&idx);
                return 1;
            }
        }

        const drop_index_entry *entry = drop_repo_index_find(&idx, target);
        if (!entry) {
            fprintf(stderr, "drop: package '%s' not found in repository\n", target);
            drop_repo_index_free(&idx);
            return 1;
        }

        char pkg_url[4096];
        if (strncmp(repo_url, "http://", 7) == 0 || strncmp(repo_url, "https://", 8) == 0) {
            snprintf(pkg_url, sizeof(pkg_url), "%s/%s", repo_url, entry->filename);
            int res = drop_install_remote_url(pkg_url, root_dir, entry->sha256);
            drop_repo_index_free(&idx);
            return res;
        } else {
            const char *base = repo_url;
            if (strncmp(base, "file://", 7) == 0) base += 7;
            snprintf(pkg_url, sizeof(pkg_url), "%s/%s", base, entry->filename);
            int res = drop_install_local_file(pkg_url, root_dir, entry->sha256);
            drop_repo_index_free(&idx);
            return res;
        }
    } else if (strcmp(verb, "rm") == 0 || strcmp(verb, "remove") == 0) {
        if (arg_idx >= argc) {
            fprintf(stderr, "drop: 'rm' requires package name\n");
            return 1;
        }
        return drop_db_uninstall(root_dir, argv[arg_idx]);
    } else if (strcmp(verb, "ls") == 0 || strcmp(verb, "list") == 0) {
        return drop_db_list(root_dir);
    } else if (strcmp(verb, "info") == 0) {
        if (arg_idx >= argc) {
            fprintf(stderr, "drop: 'info' requires package name\n");
            return 1;
        }
        const char *name = argv[arg_idx];

        distill_port p;
        if (drop_db_load(root_dir, name, &p) == 0) {
            printf("Package:      %s\n", p.name);
            printf("Version:      %s (release %s)\n", p.version, p.release[0] ? p.release : "1");
            if (p.desc[0] != '\0') printf("Description:  %s\n", p.desc);
            if (p.url[0] != '\0')  printf("Upstream URL: %s\n", p.url);
            if (p.run_deps[0] != '\0') printf("Depends:      %s\n", p.run_deps);
            printf("Installed:    %llu bytes (%zu tracked entries)\n",
                   (unsigned long long)p.installed_size, p.files.count);
            if (p.timestamp > 0) printf("Timestamp:    %llu\n", (unsigned long long)p.timestamp);
            distill_port_free(&p);
            return 0;
        }

        /* Check in repository index */
        drop_repo_index idx;
        drop_repo_index_init(&idx);
        if (drop_repo_fetch_index(repo_url, index_cache, &idx) == 0 ||
            drop_repo_fetch_index(index_cache, NULL, &idx) == 0) {
            const drop_index_entry *e = drop_repo_index_find(&idx, name);
            if (e) {
                printf("Package:      %s (repository: not installed)\n", e->name);
                printf("Version:      %s\n", e->version);
                printf("Package File: %s\n", e->filename);
                printf("Archive Size: %llu bytes\n", (unsigned long long)e->size);
                printf("SHA-256:      %s\n", e->sha256);
                if (e->depends[0]) printf("Depends:      %s\n", e->depends);
                drop_repo_index_free(&idx);
                return 0;
            }
        }
        drop_repo_index_free(&idx);

        fprintf(stderr, "drop: package '%s' not found\n", name);
        return 1;
    } else if (strcmp(verb, "check") == 0) {
        const char *name = (arg_idx < argc) ? argv[arg_idx] : NULL;
        return drop_db_check(root_dir, name);
    } else if (strcmp(verb, "update") == 0 || strcmp(verb, "sync") == 0) {
        drop_repo_index idx;
        drop_repo_index_init(&idx);
        int res = drop_repo_fetch_index(repo_url, index_cache, &idx);
        if (res == 0) {
            printf("Updated repository catalog: %zu packages available.\n", idx.count);
        } else {
            fprintf(stderr, "drop: failed to update catalog from %s\n", repo_url);
        }
        drop_repo_index_free(&idx);
        return res;
    } else if (strcmp(verb, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    } else {
        fprintf(stderr, "drop: unknown verb '%s'\n", verb);
        print_usage(argv[0]);
        return 1;
    }
}
