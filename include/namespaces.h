#ifndef MINICTL_NAMESPACES_H
#define MINICTL_NAMESPACES_H

#include <sys/types.h>

/*
 * Child setup passed to the namespace clone entrypoint.
 * String and argv pointers are borrowed from the already-parsed command and
 * must remain valid until namespaces_clone_child returns in the parent.
 */
typedef struct NamespaceChildConfig {
    const char *hostname;
    char **argv;
} NamespaceChildConfig;

/*
 * Clone a child into the v1 namespace set and execute its command.
 * The child becomes PID 1 inside the new PID namespace when CLONE_NEWPID
 * succeeds; this call requires Linux namespace privileges.
 */
int namespaces_clone_child(const NamespaceChildConfig *config, pid_t *child_pid);

#endif
