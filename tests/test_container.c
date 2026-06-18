#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "container.h"
#include "config.h"
#include "state.h"
#include "util.h"

static void setup_state_root(char *root_template, const char **root) {
    *root = mkdtemp(root_template);
    assert(*root != NULL);
    assert(setenv("MINICTL_STATE_DIR", *root, 1) == 0);
    assert(state_init() == 0);
}

static void cleanup_empty_state_root(const char *root) {
    char containers_path[MINICTL_MAX_PATH_SIZE];

    assert(minictl_path_join(containers_path, sizeof(containers_path), root, MINICTL_CONTAINERS_DIR) == 0);
    assert(rmdir(containers_path) == 0);
    assert(rmdir(root) == 0);
}

static void suppress_output(int *stdout_fd, int *stderr_fd) {
    FILE *null_file;

    fflush(stdout);
    fflush(stderr);

    *stdout_fd = dup(STDOUT_FILENO);
    *stderr_fd = dup(STDERR_FILENO);
    assert(*stdout_fd >= 0);
    assert(*stderr_fd >= 0);

    null_file = fopen("/dev/null", "w");
    assert(null_file != NULL);
    assert(dup2(fileno(null_file), STDOUT_FILENO) >= 0);
    assert(dup2(fileno(null_file), STDERR_FILENO) >= 0);
    assert(fclose(null_file) == 0);
}

static void restore_output(int stdout_fd, int stderr_fd) {
    fflush(stdout);
    fflush(stderr);

    assert(dup2(stdout_fd, STDOUT_FILENO) >= 0);
    assert(dup2(stderr_fd, STDERR_FILENO) >= 0);
    assert(close(stdout_fd) == 0);
    assert(close(stderr_fd) == 0);
}

static void fill_container(MinictlContainerState *container, const char *id, const char *status, pid_t pid) {
    memset(container, 0, sizeof(*container));

    assert(minictl_str_copy(container->id, sizeof(container->id), id) == 0);
    assert(minictl_str_copy(container->name, sizeof(container->name), "web") == 0);
    container->pid = pid;
    assert(minictl_str_copy(container->status, sizeof(container->status), status) == 0);
    assert(minictl_str_copy(container->rootfs, sizeof(container->rootfs), "/abs/path/rootfs/alpine") == 0);
    assert(minictl_str_copy(container->hostname, sizeof(container->hostname), "web") == 0);
    assert(minictl_str_copy(container->command, sizeof(container->command), "/bin/sh") == 0);
    assert(minictl_str_copy(container->created_at, sizeof(container->created_at), "2026-06-17T09:00:00Z") == 0);
    assert(minictl_str_copy(container->started_at, sizeof(container->started_at), "2026-06-17T09:01:00Z") == 0);
    assert(minictl_str_copy(container->finished_at, sizeof(container->finished_at), "") == 0);
    container->exit_code = -1;
    assert(minictl_str_copy(container->cgroup_path, sizeof(container->cgroup_path), "/sys/fs/cgroup/minictl/test") == 0);
}

static MinictlCommand command_for_id(MinictlCommandType type, const char *id) {
    MinictlCommand command;

    memset(&command, 0, sizeof(command));
    command.type = type;
    assert(minictl_str_copy(command.id, sizeof(command.id), id) == 0);

    return command;
}

static void test_container_list_refreshes_stale_running_status(void) {
    char root_template[] = "/tmp/minictl-container-list-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlContainerState loaded;
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);
    fill_container(&container, "stale", "running", -1);

    assert(state_create_container(&container) == 0);
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_list();
    restore_output(stdout_fd, stderr_fd);
    assert(result == 0);

    assert(state_load_container("stale", &loaded) == 0);
    assert(strcmp(loaded.status, "exited") == 0);

    assert(state_remove_container("stale") == 0);
    cleanup_empty_state_root(root);
}

static void test_container_inspect_existing_and_missing(void) {
    char root_template[] = "/tmp/minictl-container-inspect-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlCommand command;
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);
    fill_container(&container, "inspectme", "exited", 0);
    assert(state_create_container(&container) == 0);

    command = command_for_id(MINICTL_COMMAND_INSPECT, "inspectme");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_inspect(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 0);

    command = command_for_id(MINICTL_COMMAND_INSPECT, "missing");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_inspect(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);

    assert(state_remove_container("inspectme") == 0);
    cleanup_empty_state_root(root);
}

static void test_container_remove_exited(void) {
    char root_template[] = "/tmp/minictl-container-rm-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlCommand command;

    setup_state_root(root_template, &root);
    fill_container(&container, "remove-me", "exited", 0);
    assert(state_create_container(&container) == 0);

    command = command_for_id(MINICTL_COMMAND_RM, "remove-me");
    assert(container_remove(&command) == 0);
    assert(state_load_container("remove-me", &container) == -1);

    cleanup_empty_state_root(root);
}

static void test_container_remove_refuses_live_running(void) {
    char root_template[] = "/tmp/minictl-container-running-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlContainerState loaded;
    MinictlCommand command;
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);
    fill_container(&container, "running", "running", getpid());
    assert(state_create_container(&container) == 0);

    command = command_for_id(MINICTL_COMMAND_RM, "running");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_remove(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);
    assert(state_load_container("running", &loaded) == 0);
    assert(strcmp(loaded.status, "running") == 0);

    assert(minictl_str_copy(loaded.status, sizeof(loaded.status), "exited") == 0);
    assert(state_save_container(&loaded) == 0);
    assert(state_remove_container("running") == 0);
    cleanup_empty_state_root(root);
}

static void test_exec_handler_is_not_implemented(void) {
    MinictlCommand command;
    int stdout_fd;
    int stderr_fd;
    int exec_result;

    memset(&command, 0, sizeof(command));

    suppress_output(&stdout_fd, &stderr_fd);
    exec_result = container_exec(&command);
    restore_output(stdout_fd, stderr_fd);

    assert(exec_result == 1);
}

int main(void) {
    test_container_list_refreshes_stale_running_status();
    test_container_inspect_existing_and_missing();
    test_container_remove_exited();
    test_container_remove_refuses_live_running();
    test_exec_handler_is_not_implemented();

    printf("All container tests passed.\n");
    return 0;
}
