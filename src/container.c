#include "container.h"

#include "process.h"
#include "state.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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

int container_run(const MinictlCommand *command)
{
    (void)command;
    return not_implemented("run");
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
    (void)command;
    return not_implemented("stop");
}

int container_logs(const MinictlCommand *command)
{
    (void)command;
    return not_implemented("logs");
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
