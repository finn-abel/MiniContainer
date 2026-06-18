#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

int process_is_alive(pid_t pid)
{
    if (pid <= 0) {
        return 0;
    }

    /*
     * kill(pid, 0) asks the kernel whether the process exists without sending
     * a signal. EPERM means it exists but this user cannot signal it.
     */
    if (kill(pid, 0) == 0) {
        return 1;
    }

    if (errno == EPERM) {
        return 1;
    }

    return 0;
}

int process_send_signal(pid_t pid, int sig)
{
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    return kill(pid, sig);
}

int process_wait(pid_t pid, int *exit_code)
{
    int status;
    pid_t waited;

    if (pid <= 0 || exit_code == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * waitpid can be interrupted by signals delivered to the parent.
     * Retry EINTR so callers get the child result instead of a transient failure.
     */
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        return -1;
    }

    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
        return 0;
    }

    if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
        return 0;
    }

    errno = ECHILD;
    return -1;
}

int process_wait_until_dead(pid_t pid, int timeout_ms)
{
    int elapsed_ms = 0;
    const int interval_ms = 100;

    if (pid <= 0 || timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * This helper intentionally polls liveness instead of waitpid.
     * A later `minictl stop` process usually is not the container's parent.
     */
    while (elapsed_ms <= timeout_ms) {
        if (!process_is_alive(pid)) {
            return 0;
        }

        usleep((useconds_t)interval_ms * 1000U);
        elapsed_ms += interval_ms;
    }

    errno = ETIMEDOUT;
    return -1;
}

int process_status_from_pid(pid_t pid, ContainerStatus *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (process_is_alive(pid)) {
        *out = CONTAINER_STATUS_RUNNING;
    } else {
        *out = CONTAINER_STATUS_EXITED;
    }

    return 0;
}
