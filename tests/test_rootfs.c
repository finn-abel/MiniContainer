#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "rootfs.h"
#include "util.h"

static void create_file(const char *path) {
    FILE *file = fopen(path, "w");

    assert(file != NULL);
    assert(fclose(file) == 0);
}

static void make_fake_rootfs(char *root_template, const char **root) {
    char bin_path[MINICTL_MAX_PATH_SIZE];
    char sh_path[MINICTL_MAX_PATH_SIZE];

    *root = mkdtemp(root_template);
    assert(*root != NULL);

    assert(minictl_path_join(bin_path, sizeof(bin_path), *root, "bin") == 0);
    assert(minictl_mkdir_p(bin_path, 0700) == 0);

    assert(minictl_path_join(sh_path, sizeof(sh_path), bin_path, "sh") == 0);
    create_file(sh_path);
}

static void cleanup_fake_rootfs(const char *root) {
    char bin_path[MINICTL_MAX_PATH_SIZE];
    char sh_path[MINICTL_MAX_PATH_SIZE];

    assert(minictl_path_join(bin_path, sizeof(bin_path), root, "bin") == 0);
    assert(minictl_path_join(sh_path, sizeof(sh_path), bin_path, "sh") == 0);

    assert(remove(sh_path) == 0);
    assert(rmdir(bin_path) == 0);
    assert(rmdir(root) == 0);
}

static void test_validate_accepts_existing_command(void) {
    char root_template[] = "/tmp/minictl-rootfs-valid-XXXXXX";
    const char *root;

    make_fake_rootfs(root_template, &root);
    assert(rootfs_validate(root, "/bin/sh") == 0);
    cleanup_fake_rootfs(root);
}

static void test_validate_rejects_missing_rootfs(void) {
    errno = 0;
    assert(rootfs_validate("/tmp/minictl-rootfs-does-not-exist", "/bin/sh") == -1);
    assert(errno == ENOENT);
}

static void test_validate_rejects_file_rootfs(void) {
    char template[] = "/tmp/minictl-rootfs-file-XXXXXX";
    int fd;

    fd = mkstemp(template);
    assert(fd >= 0);
    assert(close(fd) == 0);

    errno = 0;
    assert(rootfs_validate(template, "/bin/sh") == -1);
    assert(errno == ENOTDIR);
    assert(remove(template) == 0);
}

static void test_validate_rejects_relative_command(void) {
    char root_template[] = "/tmp/minictl-rootfs-relative-XXXXXX";
    const char *root;

    make_fake_rootfs(root_template, &root);

    errno = 0;
    assert(rootfs_validate(root, "bin/sh") == -1);
    assert(errno == EINVAL);

    cleanup_fake_rootfs(root);
}

static void test_validate_rejects_missing_command(void) {
    char root_template[] = "/tmp/minictl-rootfs-missing-XXXXXX";
    const char *root;

    make_fake_rootfs(root_template, &root);

    errno = 0;
    assert(rootfs_validate(root, "/bin/missing") == -1);
    assert(errno == ENOENT);

    cleanup_fake_rootfs(root);
}

static void test_validate_rejects_directory_command(void) {
    char root_template[] = "/tmp/minictl-rootfs-dir-XXXXXX";
    const char *root;

    make_fake_rootfs(root_template, &root);

    errno = 0;
    assert(rootfs_validate(root, "/bin") == -1);
    assert(errno == EISDIR);

    cleanup_fake_rootfs(root);
}

static void test_mount_helpers_reject_invalid_rootfs(void) {
    errno = 0;
    assert(rootfs_prepare_mounts(NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(rootfs_mount_proc(NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(rootfs_switch_root(NULL) == -1);
    assert(errno == EINVAL);
}

static void test_validate_rejects_null_and_empty_rootfs(void) {
    errno = 0;
    assert(rootfs_validate(NULL, "/bin/sh") == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(rootfs_validate("", "/bin/sh") == -1);
    assert(errno == EINVAL);
}

static void test_enter_and_cleanup_reject_invalid(void) {
    errno = 0;
    assert(rootfs_enter_from_fd(-1) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(rootfs_cleanup_mounts(NULL) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    test_validate_accepts_existing_command();
    test_validate_rejects_missing_rootfs();
    test_validate_rejects_file_rootfs();
    test_validate_rejects_relative_command();
    test_validate_rejects_missing_command();
    test_validate_rejects_directory_command();
    test_mount_helpers_reject_invalid_rootfs();
    test_validate_rejects_null_and_empty_rootfs();
    test_enter_and_cleanup_reject_invalid();

    printf("All rootfs tests passed.\n");
    return 0;
}
