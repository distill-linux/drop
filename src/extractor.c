#define _GNU_SOURCE
#include "extractor.h"
#include "db.h"
#include "tar.h"
#include "spawn_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

int drop_install_stream(FILE *in, const char *target_prefix, const char *expected_sha256) {
    if (!in || !target_prefix) return -1;

    char *port_content = NULL;
    size_t port_len = 0;
    distill_file_list raw_files;
    distill_file_list_init(&raw_files);
    char actual_sha256[65] = {0};

    int ret = distill_tar_gz_extract(in, target_prefix, &port_content, &port_len,
                                     &raw_files, actual_sha256);
    if (ret != 0) {
        fprintf(stderr, "drop: extraction failed (error code %d)\n", ret);
        free(port_content);
        distill_file_list_free(&raw_files);
        return -1;
    }

    if (expected_sha256 && expected_sha256[0] != '\0') {
        if (strcasecmp(actual_sha256, expected_sha256) != 0) {
            fprintf(stderr, "drop: SHA-256 mismatch!\n  Expected: %s\n  Actual:   %s\n",
                    expected_sha256, actual_sha256);
            free(port_content);
            distill_file_list_free(&raw_files);
            return -2;
        }
    }

    if (!port_content) {
        fprintf(stderr, "drop: missing .PORT manifest inside archive\n");
        distill_file_list_free(&raw_files);
        return -3;
    }

    distill_port port;
    if (distill_port_parse(port_content, port_len, &port) != 0) {
        fprintf(stderr, "drop: failed to parse .PORT manifest\n");
        free(port_content);
        distill_file_list_free(&raw_files);
        return -4;
    }
    free(port_content);

    /* If manifest had no FILES list recorded, populate from extracted list */
    if (port.files.count == 0) {
        for (size_t i = 0; i < raw_files.count; ++i) {
            char fmt_path[1024];
            if (raw_files.paths[i][0] == '/') {
                snprintf(fmt_path, sizeof(fmt_path), "%s", raw_files.paths[i]);
            } else {
                snprintf(fmt_path, sizeof(fmt_path), "/%s", raw_files.paths[i]);
            }
            port_file_list_add(&port.files, fmt_path, "", "", PORT_FILE_REGULAR, 0);
        }
    }
    distill_file_list_free(&raw_files);

    if (port.timestamp == 0) {
        port.timestamp = (uint64_t)time(NULL);
    }

    if (drop_db_save(target_prefix, &port) != 0) {
        fprintf(stderr, "drop: failed to register port '%s' in database\n", port.name);
        distill_port_free(&port);
        return -5;
    }

    printf("Installed %s-%s (%zu entries, sha256: %.8s...)\n",
           port.name, port.version, port.files.count, actual_sha256);

    distill_port_free(&port);
    return 0;
}

int drop_install_local_file(const char *file_path, const char *target_prefix,
                            const char *expected_sha256) {
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        fprintf(stderr, "drop: cannot open archive '%s': %s\n", file_path, strerror(errno));
        return -1;
    }

    int res = drop_install_stream(f, target_prefix, expected_sha256);
    fclose(f);
    return res;
}

int drop_install_remote_url(const char *url, const char *target_prefix,
                            const char *expected_sha256) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "drop: pipe error: %s\n", strerror(errno));
        return -1;
    }

    char *argv[] = {"curl", "-sSL", (char *)url, NULL};
    pid_t pid = 0;
    int err = distill_spawn(argv, NULL, NULL, -1, pipefd[1], -1, &pid);
    if (err != 0) {
        /* Fallback to wget */
        char *wargv[] = {"wget", "-qO-", (char *)url, NULL};
        err = distill_spawn(wargv, NULL, NULL, -1, pipefd[1], -1, &pid);
    }
    close(pipefd[1]);
    if (err != 0) {
        close(pipefd[0]);
        fprintf(stderr, "drop: failed to spawn curl or wget for '%s': %s\n", url, strerror(err));
        return -1;
    }

    FILE *in = fdopen(pipefd[0], "rb");
    if (!in) {
        close(pipefd[0]);
        return -1;
    }

    int res = drop_install_stream(in, target_prefix, expected_sha256);
    fclose(in);

    int status = 0;
    distill_spawn_wait(pid, &status);
    if (status != 0 && res == 0) {
        fprintf(stderr, "drop: curl exited with code %d\n", status);
        return -1;
    }

    return res;
}
