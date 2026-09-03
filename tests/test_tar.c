#define _GNU_SOURCE
#include "../src/common/tar.h"
#include "../src/common/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>

static void test_drop_create_and_extract(void) {
    printf("[TEST] Testing ustar create and extract for .drop archive...\n");

    system("rm -rf /tmp/test_fakeroot /tmp/test_dest /tmp/test_pkg.drop");
    system("mkdir -p /tmp/test_fakeroot/usr/bin /tmp/test_dest");

    /* Create mock installed file */
    FILE *bin = fopen("/tmp/test_fakeroot/usr/bin/hello", "w");
    assert(bin);
    fprintf(bin, "#!/bin/sh\necho Hello Distill Drop\n");
    fclose(bin);
    chmod("/tmp/test_fakeroot/usr/bin/hello", 0755);

    /* Create mock .PORT */
    FILE *m = fopen("/tmp/test_fakeroot/.PORT", "w");
    assert(m);
    fprintf(m, "PORT_NAME=\"hello\"\nPORT_VERSION=\"1.0.0\"\nPORT_RELEASE=\"1\"\nRUN_DEPS=\"\"\nFILES:\n/usr/bin/hello\n");
    fclose(m);

    /* Create .drop archive */
    int r = distill_tar_gz_create("/tmp/test_pkg.drop", "/tmp/test_fakeroot", "/tmp/test_fakeroot/.PORT");
    assert(r == 0);

    /* Verify first header in archive is strictly .PORT */
    gzFile gz = gzopen("/tmp/test_pkg.drop", "rb");
    assert(gz);
    distill_tar_header hdr;
    assert(gzread(gz, &hdr, sizeof(hdr)) == sizeof(hdr));
    gzclose(gz);
    assert(strcmp(hdr.name, ".PORT") == 0);
    printf("  -> Verified entry #1 in .drop archive is .PORT\n");

    /* Extract archive */
    FILE *in = fopen("/tmp/test_pkg.drop", "rb");
    assert(in);
    char *port_content = NULL;
    size_t port_len = 0;
    distill_file_list files;
    distill_file_list_init(&files);
    char sha256[65] = {0};

    r = distill_tar_gz_extract(in, "/tmp/test_dest", &port_content, &port_len, &files, sha256);
    fclose(in);
    assert(r == 0);
    assert(port_content != NULL);
    assert(strstr(port_content, "PORT_NAME=\"hello\"") != NULL);
    assert(files.count > 0);
    assert(strlen(sha256) == 64);
    printf("  -> .drop Archive SHA-256: %s\n", sha256);

    /* Verify extracted file exists and has executable permissions */
    struct stat st;
    assert(stat("/tmp/test_dest/usr/bin/hello", &st) == 0);
    assert(st.st_mode & S_IXUSR);
    printf("  -> Extracted file verified with correct permissions.\n");

    free(port_content);
    distill_file_list_free(&files);
}

static void test_path_traversal_protection(void) {
    printf("[TEST] Testing path traversal security protection...\n");

    system("rm -rf /tmp/test_exploit.drop /tmp/test_dest2");
    system("mkdir -p /tmp/test_dest2");

    gzFile gz = gzopen("/tmp/test_exploit.drop", "wb");
    assert(gz);

    /* Entry 1: .PORT */
    distill_tar_header hdr1;
    memset(&hdr1, 0, sizeof(hdr1));
    strcpy(hdr1.name, ".PORT");
    strcpy(hdr1.mode, "0000644");
    strcpy(hdr1.size, "00000000020");
    hdr1.typeflag[0] = '0';
    memcpy(hdr1.magic, "ustar", 5);
    memset(hdr1.chksum, ' ', 8);
    unsigned int sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += ((unsigned char *)&hdr1)[i];
    snprintf(hdr1.chksum, sizeof(hdr1.chksum), "%06o", sum);
    gzwrite(gz, &hdr1, 512);
    char dummy_port[512] = "PORT_NAME=\"hack\"\n";
    gzwrite(gz, dummy_port, 512);

    /* Entry 2: malicious traversal path "../evil.txt" */
    distill_tar_header hdr2;
    memset(&hdr2, 0, sizeof(hdr2));
    strcpy(hdr2.name, "../evil.txt");
    strcpy(hdr2.mode, "0000644");
    strcpy(hdr2.size, "00000000010");
    hdr2.typeflag[0] = '0';
    memcpy(hdr2.magic, "ustar", 5);
    memset(hdr2.chksum, ' ', 8);
    sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += ((unsigned char *)&hdr2)[i];
    snprintf(hdr2.chksum, sizeof(hdr2.chksum), "%06o", sum);
    gzwrite(gz, &hdr2, 512);
    char dummy_evil[512] = "pwned\n";
    gzwrite(gz, dummy_evil, 512);

    char zeros[1024] = {0};
    gzwrite(gz, zeros, sizeof(zeros));
    gzclose(gz);

    FILE *in = fopen("/tmp/test_exploit.drop", "rb");
    assert(in);
    char *port_content = NULL;
    size_t port_len = 0;
    distill_file_list files;
    distill_file_list_init(&files);
    char sha256[65] = {0};

    int r = distill_tar_gz_extract(in, "/tmp/test_dest2", &port_content, &port_len, &files, sha256);
    fclose(in);

    /* Extraction MUST fail due to security violation */
    assert(r != 0);
    assert(access("/tmp/evil.txt", F_OK) != 0);
    printf("  -> Illegal path traversal was blocked successfully!\n");

    free(port_content);
    distill_file_list_free(&files);
}

int main(void) {
    printf("=== Starting ustar / .drop Security Unit Tests ===\n");
    test_drop_create_and_extract();
    test_path_traversal_protection();
    printf("=== All ustar / .drop Security Unit Tests Passed! ===\n");
    return 0;
}
