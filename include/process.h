#ifndef MINICTL_PROCESS_H
#define MINICTL_PROCESS_H

#include <sys/types.h>

/*
 * Process-derived container status values.
 * The state layer stores strings, while process helpers expose typed results.
 */
typedef enum ContainerStatus {
    CONTAINER_STATUS_RUNNING = 0,
    CONTAINER_STATUS_EXITED
} ContainerStatus;

/*
 * Check whether a PID currently exists.
 * EPERM counts as alive because the process exists even when signaling is denied.
 */
int process_is_alive(pid_t pid);

/*
 * Send a signal to one positive PID.
 * Invalid non-positive PIDs fail with EINVAL instead of invoking group semantics.
 */
int process_send_signal(pid_t pid, int sig);

/*
 * Wait for a child process and convert its status into an exit code.
 * Signal exits use the conventional 128 + signal number form.
 */
int process_wait(pid_t pid, int *exit_code);

/*
 * Convert current PID liveness into a typed container status.
 * Dead or invalid PIDs map to CONTAINER_STATUS_EXITED.
 */
int process_status_from_pid(pid_t pid, ContainerStatus *out);

#endif
