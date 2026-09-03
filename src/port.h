#ifndef DISTILL_PORT_H
#define DISTILL_PORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    PORT_FILE_REGULAR = 0,
    PORT_FILE_SYMLINK = 1,
    PORT_FILE_DIR = 2
} port_file_type;

typedef struct {
    char path[1024];           /* formatted as /usr/bin/... */
    char sha256[65];           /* 64 hex characters or empty */
    char symlink_target[1024]; /* target if symlink */
    port_file_type type;
    uint64_t size;
} port_file_entry;

typedef struct {
    port_file_entry *entries;
    size_t count;
    size_t capacity;
} port_file_list;

typedef struct {
    char name[128];
    char version[64];
    char release[32];
    char desc[256];
    char url[512];
    char commit[128];
    char build_system[64];
    char build_deps[512];
    char run_deps[512];
    char *build_script;
    size_t build_script_len;
    uint64_t timestamp;
    uint64_t installed_size;
    port_file_list files;
} distill_port;

void distill_port_init(distill_port *p);
void distill_port_free(distill_port *p);

void port_file_list_init(port_file_list *list);
void port_file_list_free(port_file_list *list);
void port_file_list_add(port_file_list *list, const char *path, const char *sha256,
                        const char *symlink_target, port_file_type type, uint64_t size);

/* Parses .PORT data from buffer */
int distill_port_parse(const char *data, size_t len, distill_port *out_port);

/* Loads .PORT file from disk */
int distill_port_load(const char *filepath, distill_port *out_port);

/* Saves .PORT file to disk (atomic=1 writes to .tmp and renames) */
int distill_port_save(const char *filepath, const distill_port *port, int atomic);

/* Traverses fakeroot_dir, hashes regular files, inspects symlinks, and populates files */
int distill_port_scan_fakeroot(const char *fakeroot_dir, port_file_list *out_files, uint64_t *out_total_size);

#endif /* DISTILL_PORT_H */
