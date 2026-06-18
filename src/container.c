#include "container.h"

#include "logging.h"
#include "namespaces.h"
#include "process.h"
#include "rootfs.h"
#include "state.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int not_implemented(const char *command_name)
{
    /*
     * Keep temporary runtime stubs centralized while the command signatures settle.
     * These messages should disappear as real lifecycle handlers land.
     */
    fprintf(stderr, "minictl: %s not implemented yet.\n", command_name);
    return 1;
}

static int print_state_not_found(void)
{
    /*
     * State lookups should produce the same not-found text across inspect/rm.
     * That makes CLI behavior predictable and keeps errno formatting out of ENOENT.
     */
    fprintf(stderr, "minictl: state: container not found\n");
    return 1;
}

static int load_container_for_command(const char *id, MinictlContainerState *container)
{
    /*
     * Translate storage errors into command-facing exit codes.
     * Callers can stay focused on command behavior after this succeeds.
     */
    if (state_load_container(id, container) != 0) {
        if (errno == ENOENT) {
            return print_state_not_found();
        }

        minictl_perror("state");
        return 1;
    }

    return 0;
}

static const char *container_status_string(ContainerStatus status)
{
    switch (status) {
        case CONTAINER_STATUS_RUNNING:
            return "running";
        case CONTAINER_STATUS_EXITED:
            return "exited";
    }

    return "exited";
}

static int refresh_container_status(MinictlContainerState *container)
{
    ContainerStatus status;

    /*
     * Only running containers need a liveness probe.
     * Terminal states are trusted until later lifecycle code records richer details.
     */
    if (strcmp(container->status, "running") != 0) {
        return 0;
    }

    if (process_status_from_pid(container->pid, &status) != 0) {
        return -1;
    }

    if (status == CONTAINER_STATUS_RUNNING) {
        return 0;
    }

    /*
     * State can outlive the process while real lifecycle handling is not present.
     * Mark dead running PIDs as exited so ps/rm do not keep stale records alive.
     */
    if (minictl_str_copy(container->status, sizeof(container->status), container_status_string(status)) != 0) {
        return -1;
    }

    return state_save_container(container);
}

static int load_and_refresh(const char *id, MinictlContainerState *container)
{
    int result = load_container_for_command(id, container);

    /*
     * Inspect and remove both want the freshest state for one container.
     * Keeping this shared avoids one command allowing stale running metadata.
     */
    if (result != 0) {
        return result;
    }

    if (refresh_container_status(container) != 0) {
        minictl_perror("state");
        return 1;
    }

    return 0;
}

static int format_timestamp(char *dst, size_t dst_size)
{
    time_t now;
    struct tm tm;

    /*
     * Store timestamps in UTC so metadata is stable across host time zones.
     * The line-based state file stays human-readable without needing parsing libs.
     */
    now = time(NULL);
    if (now == (time_t)-1) {
        return -1;
    }

    if (gmtime_r(&now, &tm) == NULL) {
        return -1;
    }

    if (strftime(dst, dst_size, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static unsigned int random_id_value(void)
{
    unsigned int value = 0;
    int fd;

    /*
     * IDs only need to be unique enough for local educational state directories.
     * /dev/urandom gives a simple Linux source without adding dependencies.
     */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t bytes_read = read(fd, &value, sizeof(value));
        close(fd);
        if (bytes_read == (ssize_t)sizeof(value)) {
            return value;
        }
    }

    return (unsigned int)time(NULL) ^ (unsigned int)getpid();
}

static int assign_container_identity(MinictlContainerState *container, const MinictlCommand *command)
{
    unsigned int value = random_id_value();
    int written;

    /*
     * The ID drives both the state directory name and cgroup path placeholder.
     * If a user did not provide --name, use the ID as a readable fallback name.
     */
    written = snprintf(container->id, sizeof(container->id), "%08x", value);
    if (written < 0 || (size_t)written >= sizeof(container->id)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (command->name[0] != '\0') {
        if (minictl_str_copy(container->name, sizeof(container->name), command->name) != 0) {
            return -1;
        }
    } else if (minictl_str_copy(container->name, sizeof(container->name), container->id) != 0) {
        return -1;
    }

    written = snprintf(container->cgroup_path, sizeof(container->cgroup_path), "/sys/fs/cgroup/minictl/%s", container->id);
    if (written < 0 || (size_t)written >= sizeof(container->cgroup_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int command_to_string(const MinictlCommand *command, char *dst, size_t dst_size)
{
    size_t used = 0;
    int i;

    /*
     * Metadata stores the command as a simple display string.
     * The child still receives the original argv vector for correct exec behavior.
     */
    if (command->command_argc <= 0 || command->command_argv == NULL) {
        errno = EINVAL;
        return -1;
    }

    dst[0] = '\0';
    for (i = 0; i < command->command_argc; i++) {
        size_t part_len = strlen(command->command_argv[i]);
        size_t extra_space = i == 0 ? 0U : 1U;

        if (used + extra_space + part_len >= dst_size) {
            errno = ENAMETOOLONG;
            return -1;
        }

        if (extra_space != 0) {
            dst[used] = ' ';
            used++;
        }

        memcpy(dst + used, command->command_argv[i], part_len);
        used += part_len;
        dst[used] = '\0';
    }

    return 0;
}

static int initialize_run_state(const MinictlCommand *command, MinictlContainerState *container)
{
    char resolved_rootfs[MINICTL_MAX_PATH_SIZE];

    memset(container, 0, sizeof(*container));
    container->pid = 0;
    container->exit_code = -1;

    /*
     * This step records the final metadata shape, but execution still happens
     * on the host. Rootfs is resolved now for later namespace/root switching.
     */
    if (realpath(command->rootfs, resolved_rootfs) == NULL) {
        return -1;
    }

    if (assign_container_identity(container, command) != 0) {
        return -1;
    }
    if (minictl_str_copy(container->rootfs, sizeof(container->rootfs), resolved_rootfs) != 0) {
        return -1;
    }
    if (minictl_str_copy(container->hostname, sizeof(container->hostname), command->hostname) != 0) {
        return -1;
    }
    if (command_to_string(command, container->command, sizeof(container->command)) != 0) {
        return -1;
    }
    if (format_timestamp(container->created_at, sizeof(container->created_at)) != 0) {
        return -1;
    }
    if (minictl_str_copy(container->status, sizeof(container->status), "created") != 0) {
        return -1;
    }

    return 0;
}

static int create_run_state(MinictlContainerState *container, const MinictlCommand *command)
{
    int attempts;

    /*
     * Retry rare ID collisions by regenerating the ID-dependent fields.
     * Other state errors are returned immediately because they indicate storage problems.
     */
    for (attempts = 0; attempts < 8; attempts++) {
        if (assign_container_identity(container, command) != 0) {
            return -1;
        }

        if (state_create_container(container) == 0) {
            return 0;
        }

        if (errno != EEXIST) {
            return -1;
        }
    }

    errno = EEXIST;
    return -1;
}

static int mark_container_running(MinictlContainerState *container, pid_t pid)
{
    container->pid = pid;

    /*
     * Save the PID only after a successful fork.
     * Foreground and detached execution both share this durable running state.
     */
    if (format_timestamp(container->started_at, sizeof(container->started_at)) != 0) {
        return -1;
    }
    if (minictl_str_copy(container->status, sizeof(container->status), "running") != 0) {
        return -1;
    }

    return state_save_container(container);
}

static int mark_container_finished(MinictlContainerState *container, int exit_code)
{
    container->exit_code = exit_code;

    /*
     * The parent owns final state after wait.
     * Later logging and namespace code should preserve this update point.
     */
    if (format_timestamp(container->finished_at, sizeof(container->finished_at)) != 0) {
        return -1;
    }
    if (minictl_str_copy(container->status, sizeof(container->status), "exited") != 0) {
        return -1;
    }

    return state_save_container(container);
}

static int mark_container_stopped(MinictlContainerState *container, int exit_code)
{
    container->exit_code = exit_code;

    if (format_timestamp(container->finished_at, sizeof(container->finished_at)) != 0) {
        return -1;
    }
    if (minictl_str_copy(container->status, sizeof(container->status), "stopped") != 0) {
        return -1;
    }

    return state_save_container(container);
}

static int start_namespace_child(const MinictlCommand *command, MinictlContainerState *container, MinictlLogs *logs, pid_t *pid)
{
    NamespaceChildConfig child_config;

    child_config.hostname = command->hostname;
    child_config.rootfs = container->rootfs;
    child_config.argv = command->command_argv;
    child_config.logs = logs;

    return namespaces_clone_child(&child_config, pid);
}

static int write_ready_status(int fd, char status)
{
    ssize_t written;

    do {
        written = write(fd, &status, 1);
    } while (written < 0 && errno == EINTR);

    if (written != 1) {
        return -1;
    }

    return 0;
}

static int read_ready_status(int fd, char *status)
{
    ssize_t bytes_read;

    do {
        bytes_read = read(fd, status, 1);
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read != 1) {
        return -1;
    }

    return 0;
}

static void detached_monitor_main(MinictlContainerState container, const MinictlCommand *command, int ready_fd)
{
    MinictlLogs logs;
    pid_t pid;
    int exit_code;

    /*
     * The monitor owns the namespace child for detached runs. That lets it reap
     * the child and update state after the CLI parent prints the ID and exits.
     */
    if (logs_create(container.id, false, &logs) != 0) {
        minictl_perror("logs");
        state_remove_container(container.id);
        write_ready_status(ready_fd, 'F');
        close(ready_fd);
        _exit(1);
    }

    if (start_namespace_child(command, &container, &logs, &pid) != 0) {
        minictl_perror("clone");
        logs_close_parent(&logs);
        state_remove_container(container.id);
        write_ready_status(ready_fd, 'F');
        close(ready_fd);
        _exit(1);
    }

    logs_close_parent(&logs);

    if (mark_container_running(&container, pid) != 0) {
        minictl_perror("state");
        process_send_signal(pid, SIGKILL);
        process_wait(pid, &exit_code);
        state_remove_container(container.id);
        write_ready_status(ready_fd, 'F');
        close(ready_fd);
        _exit(1);
    }

    write_ready_status(ready_fd, 'S');
    close(ready_fd);

    if (process_wait(pid, &exit_code) == 0) {
        if (mark_container_finished(&container, exit_code) != 0) {
            minictl_perror("state");
            _exit(1);
        }
    } else {
        minictl_perror("wait");
        _exit(1);
    }

    _exit(0);
}

static int run_detached(const MinictlCommand *command, MinictlContainerState *container)
{
    int ready_pipe[2];
    pid_t monitor_pid;
    char status;

    if (pipe(ready_pipe) != 0) {
        minictl_perror("pipe");
        return 1;
    }

    monitor_pid = fork();
    if (monitor_pid < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        minictl_perror("fork");
        return 1;
    }

    if (monitor_pid == 0) {
        close(ready_pipe[0]);
        detached_monitor_main(*container, command, ready_pipe[1]);
    }

    close(ready_pipe[1]);
    if (read_ready_status(ready_pipe[0], &status) != 0) {
        int monitor_exit;

        close(ready_pipe[0]);
        process_wait(monitor_pid, &monitor_exit);
        fprintf(stderr, "minictl: detach: monitor failed to start container\n");
        return 1;
    }
    close(ready_pipe[0]);

    if (status != 'S') {
        int monitor_exit;

        process_wait(monitor_pid, &monitor_exit);
        return 1;
    }

    printf("container_id: %s\n", container->id);
    return 0;
}

int container_run(const MinictlCommand *command)
{
    MinictlContainerState container;
    MinictlLogs logs;
    pid_t pid;
    int exit_code;

    if (command == NULL) {
        errno = EINVAL;
        minictl_perror("run");
        return 1;
    }

    /*
     * Build and persist a created record before fork so failed parent/child
     * transitions still leave useful debugging state.
     */
    if (initialize_run_state(command, &container) != 0) {
        minictl_perror("run");
        return 1;
    }

    if (rootfs_validate(container.rootfs, command->command_argv[0]) != 0) {
        minictl_perror("rootfs");
        return 1;
    }

    if (create_run_state(&container, command) != 0) {
        minictl_perror("state");
        return 1;
    }

    if (command->detach) {
        return run_detached(command, &container);
    }

    if (logs_create(container.id, !command->detach, &logs) != 0) {
        minictl_perror("logs");
        state_remove_container(container.id);
        return 1;
    }

    /*
     * Replace the temporary fork path with clone so parent lifecycle code now
     * follows the final namespace shape. Rootfs and /proc setup are still later.
     */
    if (start_namespace_child(command, &container, &logs, &pid) != 0) {
        int clone_errno = errno;

        /*
         * No child exists if clone fails, so remove the pre-created state record.
         * Preserve clone errno so users still see the real failure reason.
         */
        state_remove_container(container.id);
        errno = clone_errno;
        logs_close_parent(&logs);
        minictl_perror("clone");
        return 1;
    }

    if (command->detach) {
        logs_close_parent(&logs);
    }

    if (mark_container_running(&container, pid) != 0) {
        logs_close_parent(&logs);
        minictl_perror("state");
        return 1;
    }

    if (logs_capture_parent(&logs, true) != 0) {
        minictl_perror("logs");
        return 1;
    }

    if (process_wait(pid, &exit_code) != 0) {
        minictl_perror("wait");
        return 1;
    }

    if (mark_container_finished(&container, exit_code) != 0) {
        minictl_perror("state");
        return 1;
    }

    return exit_code;
}

int container_list(void)
{
    MinictlContainerList list;
    size_t i;

    /*
     * Listing owns the full state list while printing.
     * Every return path after a successful load must release that list.
     */
    if (state_list_containers(&list) != 0) {
        minictl_perror("state");
        return 1;
    }

    if (list.count == 0) {
        printf("No containers found.\n");
        state_free_container_list(&list);
        return 0;
    }

    /*
     * Refresh each entry before printing so stale running PIDs are corrected.
     * The table stays intentionally small until richer lifecycle data exists.
     */
    printf("ID\tSTATUS\tPID\tNAME\tCOMMAND\n");
    for (i = 0; i < list.count; i++) {
        if (refresh_container_status(&list.items[i]) != 0) {
            state_free_container_list(&list);
            minictl_perror("state");
            return 1;
        }

        printf("%s\t%s\t%ld\t%s\t%s\n",
               list.items[i].id,
               list.items[i].status,
               (long)list.items[i].pid,
               list.items[i].name,
               list.items[i].command);
    }

    state_free_container_list(&list);
    return 0;
}

int container_stop(const MinictlCommand *command)
{
    MinictlContainerState container;

    if (load_and_refresh(command->id, &container) != 0) {
        return 1;
    }

    if (strcmp(container.status, "running") != 0) {
        fprintf(stderr, "minictl: stop: container is not running\n");
        return 1;
    }

    if (process_send_signal(container.pid, SIGTERM) != 0) {
        if (errno == ESRCH) {
            if (mark_container_stopped(&container, -1) != 0) {
                minictl_perror("state");
                return 1;
            }
            printf("stopped %s\n", container.id);
            return 0;
        }

        minictl_perror("stop");
        return 1;
    }

    if (process_wait_until_dead(container.pid, 2000) != 0) {
        if (errno != ETIMEDOUT) {
            minictl_perror("stop");
            return 1;
        }

        if (process_send_signal(container.pid, SIGKILL) != 0 && errno != ESRCH) {
            minictl_perror("kill");
            return 1;
        }

        if (process_wait_until_dead(container.pid, 2000) != 0 && errno != ESRCH) {
            fprintf(stderr, "minictl: stop: process is still running\n");
            return 1;
        }
    }

    if (mark_container_stopped(&container, 143) != 0) {
        minictl_perror("state");
        return 1;
    }

    printf("stopped %s\n", container.id);
    return 0;
}

int container_logs(const MinictlCommand *command)
{
    MinictlContainerState container;

    if (load_container_for_command(command->id, &container) != 0) {
        return 1;
    }

    if (logs_print(container.id) != 0) {
        minictl_perror("logs");
        return 1;
    }

    return 0;
}

int container_inspect(const MinictlCommand *command)
{
    MinictlContainerState container;

    /*
     * Inspect prints the same key names used in the metadata file.
     * That keeps the command useful while a richer output format does not exist.
     */
    if (load_and_refresh(command->id, &container) != 0) {
        return 1;
    }

    printf("id=%s\n", container.id);
    printf("name=%s\n", container.name);
    printf("pid=%ld\n", (long)container.pid);
    printf("status=%s\n", container.status);
    printf("rootfs=%s\n", container.rootfs);
    printf("hostname=%s\n", container.hostname);
    printf("command=%s\n", container.command);
    printf("created_at=%s\n", container.created_at);
    printf("started_at=%s\n", container.started_at);
    printf("finished_at=%s\n", container.finished_at);
    printf("exit_code=%d\n", container.exit_code);
    printf("cgroup_path=%s\n", container.cgroup_path);

    return 0;
}

int container_remove(const MinictlCommand *command)
{
    MinictlContainerState container;

    /*
     * Removal checks refreshed state first so stale dead PIDs can be removed,
     * while genuinely live running containers are protected.
     */
    if (load_and_refresh(command->id, &container) != 0) {
        return 1;
    }

    if (strcmp(container.status, "running") == 0) {
        fprintf(stderr, "minictl: rm: container is running\n");
        return 1;
    }

    if (state_remove_container(container.id) != 0) {
        minictl_perror("state");
        return 1;
    }

    return 0;
}

int container_exec(const MinictlCommand *command)
{
    (void)command;
    return not_implemented("exec");
}
