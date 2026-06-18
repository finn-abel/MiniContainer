#include "logging.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *logging_state_root(void)
{
    const char *override = getenv("MINICTL_STATE_DIR");

    /*
     * Logging reconstructs state paths rather than depending on state.c internals.
     * Keep the env override behavior identical to the state store.
     */
    if (override != NULL && override[0] != '\0') {
        return override;
    }

    return MINICTL_DEFAULT_STATE_DIR;
}

static void init_logs(MinictlLogs *logs)
{
    memset(logs, 0, sizeof(*logs));

    /*
     * Every fd starts at -1 so cleanup helpers can be called safely from any
     * partially initialized error path.
     */
    logs->stdout_pipe[0] = -1;
    logs->stdout_pipe[1] = -1;
    logs->stderr_pipe[0] = -1;
    logs->stderr_pipe[1] = -1;
    logs->stdout_file = -1;
    logs->stderr_file = -1;
}

static int build_log_path(char *dst, size_t dst_size, const char *container_id, const char *file_name)
{
    char containers_path[MINICTL_MAX_PATH_SIZE];
    char container_path[MINICTL_MAX_PATH_SIZE];

    if (container_id == NULL || container_id[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /*
     * Log files live beside the metadata file:
     * <state_root>/containers/<id>/stdout.log and stderr.log.
     */
    if (minictl_path_join(containers_path, sizeof(containers_path), logging_state_root(), MINICTL_CONTAINERS_DIR) != 0) {
        return -1;
    }
    if (minictl_path_join(container_path, sizeof(container_path), containers_path, container_id) != 0) {
        return -1;
    }

    return minictl_path_join(dst, dst_size, container_path, file_name);
}

static int close_fd(int *fd)
{
    /*
     * Closing resets the fd even on close errors.
     * That prevents later cleanup from accidentally closing a reused descriptor.
     */
    if (*fd < 0) {
        return 0;
    }

    if (close(*fd) != 0) {
        *fd = -1;
        return -1;
    }

    *fd = -1;
    return 0;
}

static int open_log_file(const char *path)
{
    /*
     * Append mode preserves logs if a later lifecycle step reopens the file.
     * state_create_container already creates empty files, but O_CREAT is harmless.
     */
    return open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

static int write_all(int fd, const char *buf, ssize_t len)
{
    ssize_t written_total = 0;

    /*
     * Pipes and terminals may accept partial writes.
     * Loop until each captured chunk reaches both durable logs and mirrors.
     */
    while (written_total < len) {
        ssize_t written = write(fd, buf + written_total, (size_t)(len - written_total));

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        written_total += written;
    }

    return 0;
}

static int copy_file_to_fd(const char *path, int fd)
{
    char buf[4096];
    int input;

    input = open(path, O_RDONLY);
    if (input < 0) {
        /*
         * A missing log is treated as empty output.
         * This keeps `minictl logs` useful for partially-created state dirs.
         */
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    for (;;) {
        ssize_t bytes_read = read(input, buf, sizeof(buf));

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(input);
            return -1;
        }

        if (bytes_read == 0) {
            break;
        }

        if (write_all(fd, buf, bytes_read) != 0) {
            close(input);
            return -1;
        }
    }

    return close(input);
}

int logs_create(const char *container_id, bool use_pipes, MinictlLogs *logs)
{
    if (logs == NULL) {
        errno = EINVAL;
        return -1;
    }

    init_logs(logs);
    logs->use_pipes = use_pipes;

    /*
     * Foreground mode uses pipes so the parent can tee output to files and the
     * terminal. Detached mode skips pipes and lets the child write files directly.
     */
    if (build_log_path(logs->stdout_path, sizeof(logs->stdout_path), container_id, MINICTL_STDOUT_LOG_FILE) != 0) {
        return -1;
    }
    if (build_log_path(logs->stderr_path, sizeof(logs->stderr_path), container_id, MINICTL_STDERR_LOG_FILE) != 0) {
        return -1;
    }

    logs->stdout_file = open_log_file(logs->stdout_path);
    if (logs->stdout_file < 0) {
        return -1;
    }

    logs->stderr_file = open_log_file(logs->stderr_path);
    if (logs->stderr_file < 0) {
        logs_close_parent(logs);
        return -1;
    }

    if (use_pipes) {
        if (pipe(logs->stdout_pipe) != 0) {
            logs_close_parent(logs);
            return -1;
        }
        if (pipe(logs->stderr_pipe) != 0) {
            logs_close_parent(logs);
            return -1;
        }
    }

    return 0;
}

int logs_redirect_child(MinictlLogs *logs)
{
    if (logs == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * After dup2, stdout/stderr own duplicate descriptors. The original handles
     * can be closed so children do not keep extra pipe ends open forever.
     */
    if (logs->use_pipes) {
        if (dup2(logs->stdout_pipe[1], STDOUT_FILENO) < 0) {
            return -1;
        }
        if (dup2(logs->stderr_pipe[1], STDERR_FILENO) < 0) {
            return -1;
        }
    } else {
        if (dup2(logs->stdout_file, STDOUT_FILENO) < 0) {
            return -1;
        }
        if (dup2(logs->stderr_file, STDERR_FILENO) < 0) {
            return -1;
        }
    }

    logs_close_child(logs);
    return 0;
}

int logs_capture_parent(MinictlLogs *logs, bool mirror)
{
    int stdout_open;
    int stderr_open;

    if (logs == NULL || !logs->use_pipes) {
        errno = EINVAL;
        return -1;
    }

    /*
     * The parent only reads from pipes. Closing write ends is essential so EOF
     * arrives when the child exits instead of making capture block forever.
     */
    close_fd(&logs->stdout_pipe[1]);
    close_fd(&logs->stderr_pipe[1]);

    stdout_open = logs->stdout_pipe[0] >= 0;
    stderr_open = logs->stderr_pipe[0] >= 0;

    while (stdout_open || stderr_open) {
        fd_set read_fds;
        int max_fd = -1;
        int ready;

        /*
         * Drain stdout and stderr concurrently. A simple blocking read on one
         * stream can deadlock if the child fills the other pipe first.
         */
        FD_ZERO(&read_fds);
        if (stdout_open) {
            FD_SET(logs->stdout_pipe[0], &read_fds);
            max_fd = logs->stdout_pipe[0] > max_fd ? logs->stdout_pipe[0] : max_fd;
        }
        if (stderr_open) {
            FD_SET(logs->stderr_pipe[0], &read_fds);
            max_fd = logs->stderr_pipe[0] > max_fd ? logs->stderr_pipe[0] : max_fd;
        }

        ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (stdout_open && FD_ISSET(logs->stdout_pipe[0], &read_fds)) {
            char buf[4096];
            ssize_t bytes_read = read(logs->stdout_pipe[0], buf, sizeof(buf));

            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (bytes_read == 0) {
                /*
                 * EOF means every writer for this stream is closed.
                 * Once both streams hit EOF, foreground capture is complete.
                 */
                close_fd(&logs->stdout_pipe[0]);
                stdout_open = 0;
            } else {
                if (write_all(logs->stdout_file, buf, bytes_read) != 0) {
                    return -1;
                }
                if (mirror && write_all(STDOUT_FILENO, buf, bytes_read) != 0) {
                    return -1;
                }
            }
        }

        if (stderr_open && FD_ISSET(logs->stderr_pipe[0], &read_fds)) {
            char buf[4096];
            ssize_t bytes_read = read(logs->stderr_pipe[0], buf, sizeof(buf));

            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (bytes_read == 0) {
                close_fd(&logs->stderr_pipe[0]);
                stderr_open = 0;
            } else {
                if (write_all(logs->stderr_file, buf, bytes_read) != 0) {
                    return -1;
                }
                if (mirror && write_all(STDERR_FILENO, buf, bytes_read) != 0) {
                    return -1;
                }
            }
        }
    }

    logs_close_parent(logs);
    return 0;
}

int logs_print(const char *container_id)
{
    char stdout_path[MINICTL_MAX_PATH_SIZE];
    char stderr_path[MINICTL_MAX_PATH_SIZE];

    if (build_log_path(stdout_path, sizeof(stdout_path), container_id, MINICTL_STDOUT_LOG_FILE) != 0) {
        return -1;
    }
    if (build_log_path(stderr_path, sizeof(stderr_path), container_id, MINICTL_STDERR_LOG_FILE) != 0) {
        return -1;
    }

    /*
     * Print stdout first and stderr second for the simple v1 UX.
     * Later log handling can add labels or interleaving timestamps.
     */
    if (copy_file_to_fd(stdout_path, STDOUT_FILENO) != 0) {
        return -1;
    }

    return copy_file_to_fd(stderr_path, STDOUT_FILENO);
}

void logs_close_parent(MinictlLogs *logs)
{
    if (logs == NULL) {
        return;
    }

    /*
     * Parent cleanup owns all descriptors after clone returns.
     * In foreground mode some pipe ends may already be closed after capture.
     */
    close_fd(&logs->stdout_pipe[0]);
    close_fd(&logs->stdout_pipe[1]);
    close_fd(&logs->stderr_pipe[0]);
    close_fd(&logs->stderr_pipe[1]);
    close_fd(&logs->stdout_file);
    close_fd(&logs->stderr_file);
}

void logs_close_child(MinictlLogs *logs)
{
    if (logs == NULL) {
        return;
    }

    /*
     * Child cleanup runs after dup2. Closing inherited originals prevents the
     * parent from waiting forever for pipe EOF.
     */
    close_fd(&logs->stdout_pipe[0]);
    close_fd(&logs->stdout_pipe[1]);
    close_fd(&logs->stderr_pipe[0]);
    close_fd(&logs->stderr_pipe[1]);
    close_fd(&logs->stdout_file);
    close_fd(&logs->stderr_file);
}
