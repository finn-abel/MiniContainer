#ifndef MINICTL_NAMESPACES_H
#define MINICTL_NAMESPACES_H

#include <stdbool.h>
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
    /*
     * Optional readiness barrier. When sync_read_fd >= 0 the child closes its
     * inherited copy of sync_write_fd and then blocks reading one byte before exec,
     * so the parent can place the child in its cgroup before the user command runs.
     * A short read or EOF (parent died/aborted) makes the child exit without exec.
     * Set sync_read_fd to -1 to disable the barrier.
     */
    int sync_read_fd;
    int sync_write_fd;
    /*
     * Add CLONE_NEWNET to the clone flags when true, giving the child its own
     * network namespace. The parent wires up connectivity (veth/bridge) after
     * clone and before releasing the start barrier. When false the child shares
     * the host network namespace, which is the default pre-network behavior.
     */
    bool enable_network;
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
 * When join_netns is true the target's network namespace is joined too, so
 * exec sees the same interfaces as a bridge/none-mode container's init.
 */
int namespaces_exec_in_container(pid_t target_pid, char **argv, int *exit_code, bool join_netns);

#endif
