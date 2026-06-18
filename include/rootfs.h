#ifndef MINICTL_ROOTFS_H
#define MINICTL_ROOTFS_H

/*
 * Validate that a root filesystem path is usable for the requested command.
 * The command path must be absolute so it can be checked under the rootfs.
 */
int rootfs_validate(const char *rootfs, const char *command_path);

/*
 * Prepare mount namespace state for the provided root filesystem.
 * This makes propagation private, bind mounts rootfs onto itself, and creates /proc.
 */
int rootfs_prepare_mounts(const char *rootfs);

/*
 * Mount procfs at <rootfs>/proc.
 * After rootfs_switch_root, callers pass "/" so this mounts at /proc.
 */
int rootfs_mount_proc(const char *rootfs);

/*
 * Switch the current process into the prepared root filesystem.
 * Uses pivot_root and keeps all old-root cleanup inside rootfs.c.
 */
int rootfs_switch_root(const char *rootfs);

/*
 * Best-effort cleanup for rootfs mounts created before exec.
 * Callers use this only on setup failure inside the mount namespace.
 */
int rootfs_cleanup_mounts(const char *rootfs);

/*
 * Make an already-open directory fd become the current process root.
 * Exec uses this with /proc/<pid>/root after joining container namespaces.
 */
int rootfs_enter_from_fd(int root_fd);

#endif
