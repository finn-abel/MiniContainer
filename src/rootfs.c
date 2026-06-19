#include "rootfs.h"

#include "config.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mount.h>
#include <sys/syscall.h>
#endif

static int build_rootfs_command_path(char *dst, size_t dst_size, const char *rootfs, const char *command_path)
{
    if (command_path == NULL || command_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    /*
     * command_path is absolute from the future container's perspective.
     * Drop the leading slash before joining it under the host-side rootfs path.
     */
    return minictl_path_join(dst, dst_size, rootfs, command_path + 1);
}

int rootfs_validate(const char *rootfs, const char *command_path)
{
    char command_host_path[MINICTL_MAX_PATH_SIZE];
    struct stat st;
    struct stat command_st;

    if (rootfs == NULL || rootfs[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (stat(rootfs, &st) != 0) {
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    if (build_rootfs_command_path(command_host_path, sizeof(command_host_path), rootfs, command_path) != 0) {
        return -1;
    }

    /*
     * Use lstat so absolute symlinks inside the rootfs are validated as present
     * without being resolved against the host root before root switching exists.
     */
    if (lstat(command_host_path, &command_st) != 0) {
        return -1;
    }

    if (S_ISDIR(command_st.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    return 0;
}

#ifdef __linux__
static int build_rootfs_proc_path(char *dst, size_t dst_size, const char *rootfs)
{
    return minictl_path_join(dst, dst_size, rootfs, "proc");
}
#endif

int rootfs_prepare_mounts(const char *rootfs)
{
    if (rootfs == NULL || rootfs[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    char proc_path[MINICTL_MAX_PATH_SIZE];

    /*
     * Make propagation private before adding mounts so bind/proc changes stay
     * inside this mount namespace and do not leak back to the host.
     */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        return -1;
    }

    /*
     * Bind mount rootfs onto itself so pivot_root/chroot work can treat it as
     * an independent mount point in the next step.
     */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) != 0) {
        return -1;
    }

    if (build_rootfs_proc_path(proc_path, sizeof(proc_path), rootfs) != 0) {
        int saved_errno = errno;

        rootfs_cleanup_mounts(rootfs);
        errno = saved_errno;
        return -1;
    }

    if (minictl_mkdir_p(proc_path, 0555) != 0) {
        int saved_errno = errno;

        rootfs_cleanup_mounts(rootfs);
        errno = saved_errno;
        return -1;
    }

    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int rootfs_overlay_supported(void)
{
#ifdef __linux__
    FILE *file = fopen("/proc/filesystems", "r");
    char line[256];
    int supported = 0;

    /*
     * /proc/filesystems lists one filesystem per line; the overlay row ends in
     * "overlay". If the file cannot be read, assume unsupported so callers fail
     * with a clear message rather than attempting an impossible mount.
     */
    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "overlay") != NULL) {
            supported = 1;
            break;
        }
    }

    fclose(file);
    return supported;
#else
    return 0;
#endif
}

int rootfs_create_overlay_dirs(const char *upper, const char *work, const char *merged)
{
    if (upper == NULL || work == NULL || merged == NULL ||
        upper[0] == '\0' || work[0] == '\0' || merged[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /*
     * The writable layer and workdir must share a filesystem; both live under
     * the container state directory. merged is the pivot_root target.
     */
    if (minictl_mkdir_p(upper, 0755) != 0) {
        return -1;
    }
    if (minictl_mkdir_p(work, 0755) != 0) {
        return -1;
    }
    if (minictl_mkdir_p(merged, 0755) != 0) {
        return -1;
    }

    return 0;
}

int rootfs_prepare_overlay(const char *lower, const char *upper, const char *work, const char *merged)
{
    if (lower == NULL || upper == NULL || work == NULL || merged == NULL ||
        lower[0] == '\0' || upper[0] == '\0' || work[0] == '\0' || merged[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    char options[3 * MINICTL_MAX_PATH_SIZE + 64];
    char proc_path[MINICTL_MAX_PATH_SIZE];
    int written;

    /*
     * Make propagation private before mounting so the overlay stays inside this
     * mount namespace and disappears automatically when the container exits.
     */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        return -1;
    }

    written = snprintf(options, sizeof(options), "lowerdir=%s,upperdir=%s,workdir=%s", lower, upper, work);
    if (written < 0 || (size_t)written >= sizeof(options)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /*
     * The kernel copies mount data into a single page; reject overlong options
     * with a clear errno instead of a cryptic EINVAL from mount.
     */
    if ((size_t)written >= 4096) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (mount("overlay", merged, "overlay", 0, options) != 0) {
        return -1;
    }

    if (build_rootfs_proc_path(proc_path, sizeof(proc_path), merged) != 0) {
        int saved_errno = errno;

        umount2(merged, MNT_DETACH);
        errno = saved_errno;
        return -1;
    }

    if (minictl_mkdir_p(proc_path, 0555) != 0) {
        int saved_errno = errno;

        umount2(merged, MNT_DETACH);
        errno = saved_errno;
        return -1;
    }

    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int rootfs_mount_proc(const char *rootfs)
{
    if (rootfs == NULL || rootfs[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    char proc_path[MINICTL_MAX_PATH_SIZE];

    if (build_rootfs_proc_path(proc_path, sizeof(proc_path), rootfs) != 0) {
        return -1;
    }

    /*
     * Mount proc at the future container path only.
     * The process still executes against the host root until root switching lands.
     */
    return mount("proc", proc_path, "proc", 0, NULL);
#else
    errno = ENOSYS;
    return -1;
#endif
}

int rootfs_switch_root(const char *rootfs)
{
    if (rootfs == NULL || rootfs[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    char old_root_path[MINICTL_MAX_PATH_SIZE];

    if (minictl_path_join(old_root_path, sizeof(old_root_path), rootfs, ".old_root") != 0) {
        return -1;
    }

    /*
     * pivot_root requires new_root and put_old to be directories on the same
     * mount. rootfs_prepare_mounts already made rootfs a bind mount point.
     */
    if (minictl_mkdir_p(old_root_path, 0700) != 0) {
        return -1;
    }

    if (chdir(rootfs) != 0) {
        rmdir(old_root_path);
        return -1;
    }

    if (syscall(SYS_pivot_root, ".", ".old_root") != 0) {
        int saved_errno = errno;

        rmdir(old_root_path);
        errno = saved_errno;
        return -1;
    }

    if (chdir("/") != 0) {
        return -1;
    }

    /*
     * Detach the old host root from the new namespace and remove the temporary
     * mount point so the container cannot walk back to it.
     */
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        return -1;
    }

    return rmdir("/.old_root");
#else
    errno = ENOSYS;
    return -1;
#endif
}

int rootfs_cleanup_mounts(const char *rootfs)
{
    if (rootfs == NULL || rootfs[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    /*
     * This is a best-effort failure helper for setup before pivot_root.
     * EINVAL means rootfs was not mounted, which is already clean enough.
     */
    if (umount2(rootfs, MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
        return -1;
    }

    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int rootfs_enter_from_fd(int root_fd)
{
    if (root_fd < 0) {
        errno = EINVAL;
        return -1;
    }

#ifdef __linux__
    /*
     * setns into a mount namespace does not change a process's root directory.
     * Exec opens /proc/<pid>/root first, then this helper makes that fd the root.
     */
    if (fchdir(root_fd) != 0) {
        return -1;
    }

    if (chroot(".") != 0) {
        return -1;
    }

    return chdir("/");
#else
    errno = ENOSYS;
    return -1;
#endif
}
