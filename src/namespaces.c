#include "namespaces.h"

#include "config.h"
#include "process.h"
#include "rootfs.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#include <signal.h>
#endif

#ifdef __linux__
typedef struct NamespaceHandles {
    int mnt_fd;
    int uts_fd;
    int ipc_fd;
    int pid_fd;
    int net_fd;
    int root_fd;
} NamespaceHandles;

static void init_namespace_handles(NamespaceHandles *handles)
{
    /*
     * Initialize every fd to -1 so cleanup is reliable from any partial-open
     * failure path while walking /proc/<pid>/ns.
     */
    handles->mnt_fd = -1;
    handles->uts_fd = -1;
    handles->ipc_fd = -1;
    handles->pid_fd = -1;
    handles->net_fd = -1;
    handles->root_fd = -1;
}

static void close_namespace_handles(NamespaceHandles *handles)
{
    /*
     * Close every handle independently and ignore close errors during cleanup.
     * The primary syscall failure has already been preserved by the caller.
     */
    if (handles->mnt_fd >= 0) {
        close(handles->mnt_fd);
    }
    if (handles->uts_fd >= 0) {
        close(handles->uts_fd);
    }
    if (handles->ipc_fd >= 0) {
        close(handles->ipc_fd);
    }
    if (handles->pid_fd >= 0) {
        close(handles->pid_fd);
    }
    if (handles->net_fd >= 0) {
        close(handles->net_fd);
    }
    if (handles->root_fd >= 0) {
        close(handles->root_fd);
    }
}

static int open_proc_path(pid_t target_pid, const char *suffix, int flags)
{
    char path[MINICTL_MAX_PATH_SIZE];
    int written;

    /*
     * All exec handles come from the target init process under /proc.
     * Opening them before setns prevents path lookup surprises after joining.
     */
    written = snprintf(path, sizeof(path), "/proc/%ld/%s", (long)target_pid, suffix);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return open(path, flags | O_CLOEXEC);
}

static int open_namespace_handles(pid_t target_pid, NamespaceHandles *handles, bool join_netns)
{
    init_namespace_handles(handles);

    /*
     * The namespace fd names match the kernel's /proc/<pid>/ns layout.
     * The root fd is separate because it feeds chroot rather than setns.
     */
    handles->mnt_fd = open_proc_path(target_pid, "ns/mnt", O_RDONLY);
    if (handles->mnt_fd < 0) {
        return -1;
    }

    /*
     * Open each namespace separately rather than deriving from one fd.
     * That keeps failure messages tied to the exact /proc handle that failed.
     */
    handles->uts_fd = open_proc_path(target_pid, "ns/uts", O_RDONLY);
    if (handles->uts_fd < 0) {
        close_namespace_handles(handles);
        return -1;
    }
    handles->ipc_fd = open_proc_path(target_pid, "ns/ipc", O_RDONLY);
    if (handles->ipc_fd < 0) {
        close_namespace_handles(handles);
        return -1;
    }
    handles->pid_fd = open_proc_path(target_pid, "ns/pid", O_RDONLY);
    if (handles->pid_fd < 0) {
        close_namespace_handles(handles);
        return -1;
    }

    /*
     * The network namespace is only joined for bridge/none-mode containers.
     * Host-mode containers share the host netns, so opening it would be a no-op
     * and is skipped to keep exec behavior identical to the pre-network runtime.
     */
    if (join_netns) {
        handles->net_fd = open_proc_path(target_pid, "ns/net", O_RDONLY);
        if (handles->net_fd < 0) {
            close_namespace_handles(handles);
            return -1;
        }
    }

    handles->root_fd = open_proc_path(target_pid, "root", O_RDONLY | O_DIRECTORY);
    if (handles->root_fd < 0) {
        close_namespace_handles(handles);
        return -1;
    }

    return 0;
}

static int join_namespace_handles(const NamespaceHandles *handles)
{
    /*
     * mnt/uts/ipc affect the current process immediately.
     * The PID namespace affects only children forked after setns succeeds.
     * Keep PID last so any future diagnostics happen before the fork boundary.
     */
    if (setns(handles->mnt_fd, CLONE_NEWNS) != 0) {
        return -1;
    }
    if (setns(handles->uts_fd, CLONE_NEWUTS) != 0) {
        return -1;
    }
    if (setns(handles->ipc_fd, CLONE_NEWIPC) != 0) {
        return -1;
    }

    /*
     * Join the network namespace when it was opened. Like mnt/uts/ipc this
     * affects the current process immediately, so the forked exec child inherits
     * the container's interfaces.
     */
    if (handles->net_fd >= 0 && setns(handles->net_fd, CLONE_NEWNET) != 0) {
        return -1;
    }

    if (setns(handles->pid_fd, CLONE_NEWPID) != 0) {
        return -1;
    }

    return 0;
}
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

    /*
     * Hostname setup happens before the root switch.
     * That makes hostname failures easier to diagnose against the host-side logs.
     */
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
     * Mount and root setup happens inside the cloned mount namespace. With an
     * overlay layer the child mounts overlay and pivots into the merged tree so
     * writes land in the per-container upper dir; otherwise it bind-mounts the
     * base rootfs directly (legacy --no-overlay behavior). On failure the child
     * exits, which tears down this mount namespace and any mount it created.
     */
    if (config->overlay_merged != NULL) {
        if (rootfs_prepare_overlay(config->rootfs, config->overlay_upper, config->overlay_work, config->overlay_merged) != 0) {
            minictl_perror("overlay");
            return 1;
        }
        if (rootfs_switch_root(config->overlay_merged) != 0) {
            minictl_perror("switch_root");
            return 1;
        }
    } else {
        if (rootfs_prepare_mounts(config->rootfs) != 0) {
            minictl_perror("rootfs");
            return 1;
        }
        if (rootfs_switch_root(config->rootfs) != 0) {
            int saved_errno = errno;

            rootfs_cleanup_mounts(config->rootfs);
            errno = saved_errno;
            minictl_perror("switch_root");
            return 1;
        }
    }

    if (rootfs_mount_proc("/") != 0) {
        minictl_perror("proc");
        return 1;
    }

    /*
     * Wait until the parent has moved this process into its cgroup before running
     * the user command, so resource limits (especially pids.max) apply from the
     * first instruction. Closing our copy of the write end first means the read
     * unblocks with EOF if the parent dies, rather than hanging forever.
     */
    if (config->sync_read_fd >= 0) {
        char go = 0;
        ssize_t ready;

        if (config->sync_write_fd >= 0) {
            close(config->sync_write_fd);
        }
        do {
            ready = read(config->sync_read_fd, &go, 1);
        } while (ready < 0 && errno == EINTR);
        close(config->sync_read_fd);

        if (ready != 1) {
            minictl_error("start", "container start aborted before exec");
            return 1;
        }
    }

    /*
     * Apply any OCI-provided environment just before exec. putenv stores each
     * borrowed "KEY=value" pointer directly; the strings live in the parent's
     * copy-on-write memory and stay valid through execvp.
     */
    if (config->env != NULL) {
        size_t i;

        for (i = 0; i < config->env_count; i++) {
            if (putenv(config->env[i]) != 0) {
                minictl_perror("env");
                return 1;
            }
        }
    }

    /*
     * After pivot_root, execvp resolves absolute paths inside the container
     * rootfs and PATH lookups use the environment now in place for the child.
     */
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
     * These are the base namespace boundaries for MiniContainer-C.
     * SIGCHLD keeps parent wait behavior aligned with the previous fork path.
     * CLONE_NEWNET is added only when bridge/none networking was requested, so
     * host-mode containers keep sharing the host network namespace.
     */
    flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC | SIGCHLD;
    if (config->enable_network) {
        flags |= CLONE_NEWNET;
    }
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

int namespaces_exec_in_container(pid_t target_pid, char **argv, int *exit_code, bool join_netns)
{
    /*
     * Validate arguments before opening /proc so ordinary unit tests can cover
     * API misuse without depending on Linux namespace privileges.
     */
    if (target_pid <= 0 || argv == NULL || argv[0] == NULL || exit_code == NULL) {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    NamespaceHandles handles;
    pid_t child_pid;

    if (open_namespace_handles(target_pid, &handles, join_netns) != 0) {
        return -1;
    }

    /*
     * After setns and chroot, this minictl process is intentionally committed
     * to the target container context for the rest of its short exec lifetime.
     */
    if (join_namespace_handles(&handles) != 0) {
        close_namespace_handles(&handles);
        return -1;
    }
    if (rootfs_enter_from_fd(handles.root_fd) != 0) {
        close_namespace_handles(&handles);
        return -1;
    }

    /*
     * PID namespace setns affects only future children.
     * Forking here is the step that actually places the exec command there.
     */
    child_pid = fork();
    if (child_pid < 0) {
        close_namespace_handles(&handles);
        return -1;
    }

    if (child_pid == 0) {
        /*
         * The child should not inherit namespace/root handles across exec.
         * They are marked CLOEXEC, but close explicitly for the failure path too.
         */
        close_namespace_handles(&handles);
        execvp(argv[0], argv);
        minictl_perror("exec");
        _exit(127);
    }

    close_namespace_handles(&handles);
    return process_wait(child_pid, exit_code);
#else
    (void)target_pid;
    (void)argv;
    (void)exit_code;
    (void)join_netns;

    errno = ENOSYS;
    return -1;
#endif
}
