#ifndef MINICTL_LOGGING_H
#define MINICTL_LOGGING_H

#include <stdbool.h>

#include "config.h"

/*
 * Runtime log file and pipe handles for one container.
 * Foreground mode uses pipes; detached mode writes directly to files.
 */
typedef struct MinictlLogs {
    bool use_pipes;
    int stdout_pipe[2];
    int stderr_pipe[2];
    int stdout_file;
    int stderr_file;
    char stdout_path[MINICTL_MAX_PATH_SIZE];
    char stderr_path[MINICTL_MAX_PATH_SIZE];
} MinictlLogs;

/*
 * Open durable log files and optionally create stdout/stderr capture pipes.
 * The container state directory must already exist before this is called.
 */
int logs_create(const char *container_id, bool use_pipes, MinictlLogs *logs);

/*
 * Redirect child stdout/stderr to the logging destination.
 * This runs in the child after clone and before exec.
 */
int logs_redirect_child(MinictlLogs *logs);

/*
 * Drain parent-side pipes into durable log files.
 * Foreground callers can also mirror captured output to the terminal.
 */
int logs_capture_parent(MinictlLogs *logs, bool mirror);

/*
 * Print durable logs for one container.
 * Missing empty log files are tolerated so logs never crashes on partial state.
 */
int logs_print(const char *container_id);

/*
 * Close file descriptors owned by parent or child code paths.
 * These helpers are safe to call during error cleanup.
 */
void logs_close_parent(MinictlLogs *logs);
void logs_close_child(MinictlLogs *logs);

#endif
