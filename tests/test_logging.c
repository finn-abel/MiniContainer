#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "logging.h"
#include "state.h"
#include "util.h"

static void fill_container(MinictlContainerState *container, const char *id) {
    memset(container, 0, sizeof(*container));

    assert(minictl_str_copy(container->id, sizeof(container->id), id) == 0);
    assert(minictl_str_copy(container->name, sizeof(container->name), id) == 0);
    assert(minictl_str_copy(container->status, sizeof(container->status), "created") == 0);
    assert(minictl_str_copy(container->rootfs, sizeof(container->rootfs), "/") == 0);
    assert(minictl_str_copy(container->command, sizeof(container->command), "/bin/echo hello") == 0);
    container->exit_code = -1;
}

static void setup_state_root(char *root_template, const char **root) {
    *root = mkdtemp(root_template);
    assert(*root != NULL);
    assert(setenv("MINICTL_STATE_DIR", *root, 1) == 0);
    assert(state_init() == 0);
}

static void cleanup_state_root(const char *root, const char *id) {
    char containers_path[MINICTL_MAX_PATH_SIZE];

    assert(state_remove_container(id) == 0);
    assert(minictl_path_join(containers_path, sizeof(containers_path), root, MINICTL_CONTAINERS_DIR) == 0);
    assert(rmdir(containers_path) == 0);
    assert(rmdir(root) == 0);
}

static void read_file(const char *path, char *buf, size_t buf_size) {
    FILE *file = fopen(path, "r");
    size_t bytes_read;

    assert(file != NULL);
    bytes_read = fread(buf, 1, buf_size - 1, file);
    assert(ferror(file) == 0);
    buf[bytes_read] = '\0';
    assert(fclose(file) == 0);
}

static void test_logs_capture_parent_writes_files(void) {
    char root_template[] = "/tmp/minictl-logs-capture-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlLogs logs;
    char stdout_buf[64];
    char stderr_buf[64];

    setup_state_root(root_template, &root);
    fill_container(&container, "capture");
    assert(state_create_container(&container) == 0);

    assert(logs_create(container.id, true, &logs) == 0);
    assert(write(logs.stdout_pipe[1], "hello\n", 6) == 6);
    assert(write(logs.stderr_pipe[1], "warn\n", 5) == 5);
    assert(logs_capture_parent(&logs, false) == 0);

    read_file(logs.stdout_path, stdout_buf, sizeof(stdout_buf));
    read_file(logs.stderr_path, stderr_buf, sizeof(stderr_buf));
    assert(strcmp(stdout_buf, "hello\n") == 0);
    assert(strcmp(stderr_buf, "warn\n") == 0);

    cleanup_state_root(root, container.id);
}

static void test_logs_create_direct_files_for_detach(void) {
    char root_template[] = "/tmp/minictl-logs-direct-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlLogs logs;
    char stdout_buf[64];
    char stderr_buf[64];

    setup_state_root(root_template, &root);
    fill_container(&container, "direct");
    assert(state_create_container(&container) == 0);

    assert(logs_create(container.id, false, &logs) == 0);
    assert(write(logs.stdout_file, "out\n", 4) == 4);
    assert(write(logs.stderr_file, "err\n", 4) == 4);
    logs_close_parent(&logs);

    read_file(logs.stdout_path, stdout_buf, sizeof(stdout_buf));
    read_file(logs.stderr_path, stderr_buf, sizeof(stderr_buf));
    assert(strcmp(stdout_buf, "out\n") == 0);
    assert(strcmp(stderr_buf, "err\n") == 0);

    cleanup_state_root(root, container.id);
}

static void test_logs_print_outputs_saved_logs(void) {
    char root_template[] = "/tmp/minictl-logs-print-XXXXXX";
    char output_template[] = "/tmp/minictl-logs-output-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlLogs logs;
    int output_fd;
    int stdout_fd;
    char output_buf[128];

    setup_state_root(root_template, &root);
    fill_container(&container, "print");
    assert(state_create_container(&container) == 0);

    assert(logs_create(container.id, false, &logs) == 0);
    assert(write(logs.stdout_file, "hello\n", 6) == 6);
    assert(write(logs.stderr_file, "warn\n", 5) == 5);
    logs_close_parent(&logs);

    output_fd = mkstemp(output_template);
    assert(output_fd >= 0);

    fflush(stdout);
    stdout_fd = dup(STDOUT_FILENO);
    assert(stdout_fd >= 0);
    assert(dup2(output_fd, STDOUT_FILENO) >= 0);
    assert(logs_print(container.id) == 0);
    fflush(stdout);
    assert(dup2(stdout_fd, STDOUT_FILENO) >= 0);
    assert(close(stdout_fd) == 0);
    assert(close(output_fd) == 0);

    read_file(output_template, output_buf, sizeof(output_buf));
    assert(strcmp(output_buf, "hello\nwarn\n") == 0);

    assert(remove(output_template) == 0);
    cleanup_state_root(root, container.id);
}

int main(void) {
    test_logs_capture_parent_writes_files();
    test_logs_create_direct_files_for_detach();
    test_logs_print_outputs_saved_logs();

    printf("All logging tests passed.\n");
    return 0;
}
