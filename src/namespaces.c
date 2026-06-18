#include "namespaces.h"

#include "config.h"
#include "rootfs.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#include <signal.h>
#endif

#ifdef __linux__
static int namespace_child_main(void *arg)
{
    const NamespaceChildConfig *config = arg;

    /*
     * Keep child setup strict and ordered: logs first, then namespace-local
     * hostname, mount/root setup, proc mounting, and finally exec.
     * Parent-side validation should catch this first; this is a last guardrail.
     */
    if (config == NULL || config->rootfs == NULL || config->argv == NULL || config->argv[0] == NULL) {
        errno = EINVAL;
        minictl_perror("child");
        return 1;
    }

    if (logs_redirect_child(config->logs) != 0) {
        minictl_perror("logs");
        return 1;
    }

    if (config->hostname != NULL && config->hostname[0] != '\0') {
        /*
         * sethostname only affects the cloned UTS namespace when CLONE_NEWUTS
         * succeeds, which is the key behavior checked by the VM acceptance test.
         */
        if (sethostname(config->hostname, strlen(config->hostname)) != 0) {
            minictl_perror("hostname");
            return 1;
        }
    }

    /*
     * Mount and root setup happens inside the cloned mount namespace.
     * After rootfs_switch_root succeeds, exec sees the supplied rootfs as /.
     */
    if (rootfs_prepare_mounts(config->rootfs) != 0) {
        minictl_perror("rootfs");
        return 1;
    }

    if (rootfs_switch_root(config->rootfs) != 0) {
        minictl_perror("switch_root");
        return 1;
    }

    if (rootfs_mount_proc("/") != 0) {
        minictl_perror("proc");
        return 1;
    }

    execvp(config->argv[0], config->argv);
    minictl_perror("exec");
    return 127;
}
#endif

int namespaces_clone_child(const NamespaceChildConfig *config, pid_t *child_pid)
{
    /*
     * Validate command shape before allocating a stack or asking the kernel for
     * namespaces. This keeps ordinary unit tests non-privileged and deterministic.
     */
    if (config == NULL || child_pid == NULL || config->rootfs == NULL || config->argv == NULL || config->argv[0] == NULL ||
        config->logs == NULL) {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    char *stack;
    char *stack_top;
    int flags;
    pid_t pid;

    /*
     * clone expects the stack pointer to reference the top of a child stack.
     * The parent can free its mapping after clone because the child has a copy.
     */
    stack = malloc(MINICTL_CHILD_STACK_SIZE);
    if (stack == NULL) {
        return -1;
    }
    stack_top = stack + MINICTL_CHILD_STACK_SIZE;

    /*
     * These are the v1 namespace boundaries for MiniContainer-C.
     * SIGCHLD keeps parent wait behavior aligned with the previous fork path.
     */
    flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC | SIGCHLD;
    pid = clone(namespace_child_main, stack_top, flags, (void *)config);
    free(stack);

    /*
     * clone failures are deliberately returned raw through errno.
     * container_run adds the user-facing "clone" context at the command layer.
     */
    if (pid < 0) {
        return -1;
    }

    *child_pid = pid;
    return 0;
#else
    (void)config;
    (void)child_pid;

    errno = ENOSYS;
    return -1;
#endif
}
