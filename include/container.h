#ifndef MINICTL_CONTAINER_H
#define MINICTL_CONTAINER_H

#include "cli.h"

/*
 * Command handlers for the final minictl command surface.
 * Runtime execution is added later, but these signatures are stable now.
 */

/*
 * Parse-complete run handler entry point.
 * Current development behavior forks and execs on the host, then records state.
 */
int container_run(const MinictlCommand *command);

/*
 * List containers from durable state and refresh stale running PIDs.
 * This is the command-layer implementation for `minictl ps`.
 */
int container_list(void);

/*
 * Stop a running container by ID.
 * Signal handling is added later, so this currently reports that clearly.
 */
int container_stop(const MinictlCommand *command);

/*
 * Print saved container logs by ID.
 * Log capture and reading are added later, so this currently reports that clearly.
 */
int container_logs(const MinictlCommand *command);

/*
 * Load one container by ID and print all metadata fields.
 * Missing containers produce a clean state-layer not-found error.
 */
int container_inspect(const MinictlCommand *command);

/*
 * Remove stopped or exited container state by ID.
 * Running containers are refused so state is not deleted out from under a live process.
 */
int container_remove(const MinictlCommand *command);

/*
 * Execute a command inside an existing container.
 * Namespace entry is added later, so this currently reports that clearly.
 */
int container_exec(const MinictlCommand *command);

#endif
