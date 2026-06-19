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
 * Report whether the kernel exposes the overlay filesystem.
 * Lets callers fail early with a clear message instead of a raw mount error.
 */
int rootfs_overlay_supported(void);

/*
 * Create the per-container overlay directories (upper, work, merged).
 * Run on the host before clone so the child can mount overlay against them.
 */
int rootfs_create_overlay_dirs(const char *upper, const char *work, const char *merged);

/*
 * Mount an overlay whose lowerdir is the base rootfs and whose writable layer is
 * upper/work, exposing the result at merged. Makes propagation private first and
 * creates merged/proc. After this, switch into merged as the new root.
 */
int rootfs_prepare_overlay(const char *lower, const char *upper, const char *work, const char *merged);

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
