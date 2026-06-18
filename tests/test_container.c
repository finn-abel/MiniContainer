#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.h"
#include "container.h"
#include "config.h"
#include "process.h"
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

static void test_exec_rejects_missing_command(void) {
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

static void test_exec_refuses_non_running_container(void) {
    char root_template[] = "/tmp/minictl-container-exec-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlCommand command;
    char *argv[] = {"/bin/sh"};
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);
    fill_container(&container, "execme", "exited", 0);
    assert(state_create_container(&container) == 0);

    command = command_for_id(MINICTL_COMMAND_EXEC, "execme");
    command.command_argc = 1;
    command.command_argv = argv;

    suppress_output(&stdout_fd, &stderr_fd);
    result = container_exec(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);

    assert(state_remove_container("execme") == 0);
    cleanup_empty_state_root(root);
}

static void test_id_handlers_report_missing_container(void) {
    char root_template[] = "/tmp/minictl-container-missing-XXXXXX";
    const char *root;
    MinictlCommand stop_command;
    MinictlCommand logs_command;
    MinictlCommand exec_command;
    char *exec_argv[] = {"/bin/sh", NULL};
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);

    stop_command = command_for_id(MINICTL_COMMAND_STOP, "missingid");
    logs_command = command_for_id(MINICTL_COMMAND_LOGS, "missingid");
    exec_command = command_for_id(MINICTL_COMMAND_EXEC, "missingid");
    exec_command.command_argc = 1;
    exec_command.command_argv = exec_argv;

    suppress_output(&stdout_fd, &stderr_fd);
    result = container_stop(&stop_command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);

    suppress_output(&stdout_fd, &stderr_fd);
    result = container_logs(&logs_command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);

    suppress_output(&stdout_fd, &stderr_fd);
    result = container_exec(&exec_command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);

    cleanup_empty_state_root(root);
}

static void test_container_run_rejects_invalid_args(void) {
    MinictlCommand command;
    char *argv[] = {"/bin/sh", NULL};
    int stdout_fd;
    int stderr_fd;

    suppress_output(&stdout_fd, &stderr_fd);
    assert(container_run(NULL) == 1);
    restore_output(stdout_fd, stderr_fd);

    /* Empty rootfs is rejected before any namespace work. */
    memset(&command, 0, sizeof(command));
    command.type = MINICTL_COMMAND_RUN;
    command.command_argc = 1;
    command.command_argv = argv;
    suppress_output(&stdout_fd, &stderr_fd);
    assert(container_run(&command) == 1);
    restore_output(stdout_fd, stderr_fd);

    /* rootfs given but no command vector. */
    memset(&command, 0, sizeof(command));
    command.type = MINICTL_COMMAND_RUN;
    assert(minictl_str_copy(command.rootfs, sizeof(command.rootfs), "/tmp") == 0);
    suppress_output(&stdout_fd, &stderr_fd);
    assert(container_run(&command) == 1);
    restore_output(stdout_fd, stderr_fd);
}

static void test_container_run_rejects_missing_rootfs(void) {
    MinictlCommand command;
    char *argv[] = {"/bin/sh", NULL};
    int stdout_fd;
    int stderr_fd;
    int result;

    memset(&command, 0, sizeof(command));
    command.type = MINICTL_COMMAND_RUN;
    assert(minictl_str_copy(command.rootfs, sizeof(command.rootfs), "/tmp/minictl-no-such-rootfs-xyz") == 0);
    command.command_argc = 1;
    command.command_argv = argv;

    suppress_output(&stdout_fd, &stderr_fd);
    result = container_run(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);
}

static void test_container_logs_outputs_saved_logs(void) {
    char root_template[] = "/tmp/minictl-container-logs-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlCommand command;
    char stdout_path[MINICTL_MAX_PATH_SIZE];
    FILE *file;
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);
    fill_container(&container, "logme", "exited", 0);
    assert(state_create_container(&container) == 0);

    assert(state_container_file_path("logme", MINICTL_STDOUT_LOG_FILE, stdout_path, sizeof(stdout_path)) == 0);
    file = fopen(stdout_path, "w");
    assert(file != NULL);
    assert(fputs("log line\n", file) != EOF);
    assert(fclose(file) == 0);

    command = command_for_id(MINICTL_COMMAND_LOGS, "logme");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_logs(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 0);

    assert(state_remove_container("logme") == 0);
    cleanup_empty_state_root(root);
}

static void test_container_stop_refuses_non_running(void) {
    char root_template[] = "/tmp/minictl-container-stopx-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlCommand command;
    int stdout_fd;
    int stderr_fd;
    int result;

    setup_state_root(root_template, &root);
    fill_container(&container, "donerun", "exited", 0);
    assert(state_create_container(&container) == 0);

    command = command_for_id(MINICTL_COMMAND_STOP, "donerun");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_stop(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 1);

    assert(state_remove_container("donerun") == 0);
    cleanup_empty_state_root(root);
}

static pid_t spawn_orphan_target(void) {
    int pipefd[2];
    pid_t intermediate;
    pid_t target = -1;

    /*
     * Double-fork so the target is reparented to init (and reaped by it), the same
     * way a real container init is not stop's child. That lets container_stop's
     * liveness poll observe the process actually disappearing instead of seeing a
     * zombie that the test process would have to reap.
     */
    assert(pipe(pipefd) == 0);
    intermediate = fork();
    assert(intermediate >= 0);
    if (intermediate == 0) {
        pid_t grandchild;

        assert(close(pipefd[0]) == 0);
        grandchild = fork();
        if (grandchild == 0) {
            assert(close(pipefd[1]) == 0);
            /* Stay alive until signaled; self-terminate if the test abandons us. */
            sleep(30);
            _exit(0);
        }
        assert(grandchild > 0);
        assert(write(pipefd[1], &grandchild, sizeof(grandchild)) == (ssize_t)sizeof(grandchild));
        assert(close(pipefd[1]) == 0);
        _exit(0);
    }

    assert(close(pipefd[1]) == 0);
    assert(read(pipefd[0], &target, sizeof(target)) == (ssize_t)sizeof(target));
    assert(close(pipefd[0]) == 0);
    assert(waitpid(intermediate, NULL, 0) == intermediate);
    assert(target > 0);

    return target;
}

static void test_container_stop_terminates_running_container(void) {
    char root_template[] = "/tmp/minictl-container-stop-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlContainerState loaded;
    MinictlCommand command;
    pid_t target;
    int stdout_fd;
    int stderr_fd;
    int result;

    target = spawn_orphan_target();

    setup_state_root(root_template, &root);
    fill_container(&container, "killme", "running", target);
    assert(state_create_container(&container) == 0);

    command = command_for_id(MINICTL_COMMAND_STOP, "killme");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_stop(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 0);

    /* The process is gone, and stop deliberately left the metadata untouched. */
    assert(process_is_alive(target) == 0);
    assert(state_load_container("killme", &loaded) == 0);
    assert(strcmp(loaded.status, "running") == 0);

    /* inspect refreshes the now-stale running status to exited (self-heal path). */
    command = command_for_id(MINICTL_COMMAND_INSPECT, "killme");
    suppress_output(&stdout_fd, &stderr_fd);
    result = container_inspect(&command);
    restore_output(stdout_fd, stderr_fd);
    assert(result == 0);
    assert(state_load_container("killme", &loaded) == 0);
    assert(strcmp(loaded.status, "exited") == 0);

    assert(state_remove_container("killme") == 0);
    cleanup_empty_state_root(root);
}

int main(void) {
    test_container_list_refreshes_stale_running_status();
    test_container_inspect_existing_and_missing();
    test_container_remove_exited();
    test_container_remove_refuses_live_running();
    test_exec_rejects_missing_command();
    test_exec_refuses_non_running_container();
    test_id_handlers_report_missing_container();
    test_container_run_rejects_invalid_args();
    test_container_run_rejects_missing_rootfs();
    test_container_logs_outputs_saved_logs();
    test_container_stop_refuses_non_running();
    test_container_stop_terminates_running_container();

    printf("All container tests passed.\n");
    return 0;
}
