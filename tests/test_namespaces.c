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
    MinictlLogs logs;
    NamespaceChildConfig config = {"demo", "/", argv, &logs, -1, -1, false};

    errno = 0;
    assert(namespaces_clone_child(&config, NULL) == -1);
    assert(errno == EINVAL);
}

static void test_clone_rejects_missing_rootfs(void) {
    char *argv[] = {"/bin/true", NULL};
    MinictlLogs logs;
    NamespaceChildConfig config = {"demo", NULL, argv, &logs, -1, -1, false};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_clone_rejects_missing_argv(void) {
    MinictlLogs logs;
    NamespaceChildConfig config = {"demo", "/", NULL, &logs, -1, -1, false};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_clone_rejects_missing_command(void) {
    char *argv[] = {NULL};
    MinictlLogs logs;
    NamespaceChildConfig config = {"demo", "/", argv, &logs, -1, -1, false};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_clone_rejects_missing_logs(void) {
    char *argv[] = {"/bin/true", NULL};
    NamespaceChildConfig config = {"demo", "/", argv, NULL, -1, -1, false};
    pid_t pid = -1;

    errno = 0;
    assert(namespaces_clone_child(&config, &pid) == -1);
    assert(errno == EINVAL);
    assert(pid == -1);
}

static void test_exec_rejects_invalid_pid(void) {
    char *argv[] = {"/bin/true", NULL};
    int exit_code = -1;

    errno = 0;
    assert(namespaces_exec_in_container(0, argv, &exit_code, false) == -1);
    assert(errno == EINVAL);
    assert(exit_code == -1);
}

static void test_exec_rejects_missing_argv(void) {
    int exit_code = -1;

    errno = 0;
    assert(namespaces_exec_in_container(1, NULL, &exit_code, false) == -1);
    assert(errno == EINVAL);
    assert(exit_code == -1);
}

static void test_exec_rejects_missing_command(void) {
    char *argv[] = {NULL};
    int exit_code = -1;

    errno = 0;
    assert(namespaces_exec_in_container(1, argv, &exit_code, false) == -1);
    assert(errno == EINVAL);
    assert(exit_code == -1);
}

static void test_exec_rejects_missing_exit_code(void) {
    char *argv[] = {"/bin/true", NULL};

    errno = 0;
    assert(namespaces_exec_in_container(1, argv, NULL, false) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    test_clone_rejects_null_config();
    test_clone_rejects_null_pid_output();
    test_clone_rejects_missing_rootfs();
    test_clone_rejects_missing_argv();
    test_clone_rejects_missing_command();
    test_clone_rejects_missing_logs();
    test_exec_rejects_invalid_pid();
    test_exec_rejects_missing_argv();
    test_exec_rejects_missing_command();
    test_exec_rejects_missing_exit_code();

    printf("All namespaces tests passed.\n");
    return 0;
}
