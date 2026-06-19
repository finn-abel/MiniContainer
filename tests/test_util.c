#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

static void test_str_copy(void) {
    char buf[8];

    assert(minictl_str_copy(buf, sizeof(buf), "abc") == 0);
    assert(strcmp(buf, "abc") == 0);

    errno = 0;
    assert(minictl_str_copy(buf, 4, "abcd") == -1);
    assert(errno == ENAMETOOLONG);
    assert(strcmp(buf, "") == 0);
}

static void test_path_join(void) {
    char path[MINICTL_MAX_PATH_SIZE];
    char small[8];

    assert(minictl_path_join(path, sizeof(path), "/tmp", "minictl") == 0);
    assert(strcmp(path, "/tmp/minictl") == 0);

    assert(minictl_path_join(path, sizeof(path), "/tmp/", "minictl") == 0);
    assert(strcmp(path, "/tmp/minictl") == 0);

    errno = 0;
    assert(minictl_path_join(small, sizeof(small), "/tmp", "minictl") == -1);
    assert(errno == ENAMETOOLONG);
    assert(strcmp(small, "") == 0);
}

static void test_mkdir_and_exists(void) {
    char template[] = "/tmp/minictl-test-XXXXXX";
    char nested[MINICTL_MAX_PATH_SIZE];
    char file_path[MINICTL_MAX_PATH_SIZE];
    FILE *file;

    assert(mkdtemp(template) != NULL);
    assert(minictl_path_join(nested, sizeof(nested), template, "a/b") == 0);
    assert(minictl_mkdir_p(nested, 0700) == 0);
    assert(minictl_dir_exists(nested));
    assert(!minictl_file_exists(nested));

    assert(minictl_path_join(file_path, sizeof(file_path), template, "file") == 0);
    file = fopen(file_path, "w");
    assert(file != NULL);
    assert(fclose(file) == 0);
    assert(minictl_file_exists(file_path));
    assert(!minictl_dir_exists(file_path));

    assert(remove(file_path) == 0);
    assert(rmdir(nested) == 0);
    assert(strcpy(nested, template) == nested);
    assert(strcat(nested, "/a") == nested);
    assert(rmdir(nested) == 0);
    assert(rmdir(template) == 0);
}

static void test_str_copy_exact_fit(void) {
    char buf[4];

    assert(minictl_str_copy(buf, sizeof(buf), "abc") == 0);
    assert(strcmp(buf, "abc") == 0);
}

static void test_path_join_empty_sides(void) {
    char path[64];

    assert(minictl_path_join(path, sizeof(path), "/tmp", "") == 0);
    assert(strcmp(path, "/tmp/") == 0);

    assert(minictl_path_join(path, sizeof(path), "", "rel") == 0);
    assert(strcmp(path, "rel") == 0);
}

static void test_mkdir_p_rejects_file_in_path(void) {
    char tmpl[] = "/tmp/minictl-util-file-XXXXXX";
    char nested[MINICTL_MAX_PATH_SIZE];
    int fd;

    fd = mkstemp(tmpl);
    assert(fd >= 0);
    assert(close(fd) == 0);

    /* A regular file blocking the path must surface as ENOTDIR. */
    assert(minictl_path_join(nested, sizeof(nested), tmpl, "sub") == 0);
    errno = 0;
    assert(minictl_mkdir_p(nested, 0700) == -1);
    assert(errno == ENOTDIR);

    assert(remove(tmpl) == 0);
}

static void test_rm_rf_removes_tree(void) {
    char root_template[] = "/tmp/minictl-util-rmrf-XXXXXX";
    const char *root = mkdtemp(root_template);
    char nested[MINICTL_MAX_PATH_SIZE];
    char file[MINICTL_MAX_PATH_SIZE];
    FILE *f;

    assert(root != NULL);

    /* Build root/sub/leaf and a file inside it, then remove the whole tree. */
    assert(minictl_path_join(nested, sizeof(nested), root, "sub/leaf") == 0);
    assert(minictl_mkdir_p(nested, 0700) == 0);
    assert(minictl_path_join(file, sizeof(file), nested, "data.txt") == 0);
    f = fopen(file, "w");
    assert(f != NULL);
    assert(fputs("payload", f) >= 0);
    assert(fclose(f) == 0);

    assert(minictl_rm_rf(root) == 0);
    assert(!minictl_dir_exists(root));
}

static void test_rm_rf_missing_path_is_ok(void) {
    /* Idempotent cleanup: a missing path is treated as already removed. */
    assert(minictl_rm_rf("/tmp/minictl-util-rmrf-absent-98765") == 0);
}

static void test_rm_rf_single_file(void) {
    char tmpl[] = "/tmp/minictl-util-rmrf-file-XXXXXX";
    int fd = mkstemp(tmpl);

    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(minictl_rm_rf(tmpl) == 0);
    assert(!minictl_file_exists(tmpl));
}

int main(void) {
    test_str_copy();
    test_path_join();
    test_mkdir_and_exists();
    test_str_copy_exact_fit();
    test_path_join_empty_sides();
    test_mkdir_p_rejects_file_in_path();
    test_rm_rf_removes_tree();
    test_rm_rf_missing_path_is_ok();
    test_rm_rf_single_file();

    printf("All util tests passed.\n");
    return 0;
}
