#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "namespaces.h"

static void test_clone_rejects_null_config(void) {
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(NULL, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_clone_rejects_null_pid_output(void) {
    char *argv[] = {"/bin/true", NULL};
    NamespaceChildConfig config = {"demo", "/", argv};

    errno = 0;
    assert(namespaces_clone_child(&config, NULL) == -1);
    assert(errno == EINVAL);
}

static void test_clone_rejects_missing_rootfs(void) {
    char *argv[] = {"/bin/true", NULL};
    NamespaceChildConfig config = {"demo", NULL, argv};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_clone_rejects_missing_argv(void) {
    NamespaceChildConfig config = {"demo", "/", NULL};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_clone_rejects_missing_command(void) {
    char *argv[] = {NULL};
    NamespaceChildConfig config = {"demo", "/", argv};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

int main(void) {
    test_clone_rejects_null_config();
    test_clone_rejects_null_pid_output();
    test_clone_rejects_missing_rootfs();
    test_clone_rejects_missing_argv();
    test_clone_rejects_missing_command();

    printf("All namespace tests passed.\n");
    return 0;
}
