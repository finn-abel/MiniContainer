#ifndef MINICTL_NAMESPACES_H
#define MINICTL_NAMESPACES_H

#include <sys/types.h>

#include "logging.h"

/*
 * Child setup passed to the namespace clone entrypoint.
 * String and argv pointers are borrowed from the already-parsed command and
 * must remain valid until namespaces_clone_child returns in the parent.
 */
typedef struct NamespaceChildConfig {
    const char *hostname;
    const char *rootfs;
    char **argv;
    MinictlLogs *logs;
} NamespaceChildConfig;

/*
 * Clone a child into the v1 namespace set and execute its command.
 * The child becomes PID 1 inside the new PID namespace when CLONE_NEWPID
 * succeeds; this call requires Linux namespace privileges.
 */
int namespaces_clone_child(const NamespaceChildConfig *config, pid_t *child_pid);

/*
 * Execute a new command inside an existing container's namespaces.
 * The target PID is the host-visible init PID saved in container metadata.
 */
int namespaces_exec_in_container(pid_t target_pid, char **argv, int *exit_code);

#endif
