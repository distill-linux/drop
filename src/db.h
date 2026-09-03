#ifndef DISTILL_DROP_DB_H
#define DISTILL_DROP_DB_H

#include "port.h"
#include <stddef.h>

void drop_db_get_port_path(const char *root, const char *pkg_name, char *out_path, size_t sz);
int drop_db_save(const char *root, const distill_port *port);
int drop_db_load(const char *root, const char *pkg_name, distill_port *out_port);
int drop_db_is_installed(const char *root, const char *pkg_name);
int drop_db_uninstall(const char *root, const char *pkg_name);
int drop_db_list(const char *root);
int drop_db_check(const char *root, const char *pkg_name);

#endif /* DISTILL_DROP_DB_H */
