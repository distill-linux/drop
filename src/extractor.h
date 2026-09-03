#ifndef DISTILL_DROP_EXTRACTOR_H
#define DISTILL_DROP_EXTRACTOR_H

#include <stdio.h>

int drop_install_stream(FILE *in, const char *target_prefix, const char *expected_sha256);
int drop_install_local_file(const char *file_path, const char *target_prefix, const char *expected_sha256);
int drop_install_remote_url(const char *url, const char *target_prefix, const char *expected_sha256);

#endif /* DISTILL_DROP_EXTRACTOR_H */
