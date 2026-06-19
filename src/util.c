#include "util.h"

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Keep the externally visible error format consistent from the first module.
 * Later syscall-heavy code can layer specific context on top of this helper.
 */
void minictl_error(const char *context, const char *message)
{
    if (context == NULL || context[0] == '\0') {
        fprintf(stderr, "minictl: %s\n", message);
        return;
    }

    fprintf(stderr, "minictl: %s: %s\n", context, message);
}

void minictl_perror(const char *context)
{
    /*
     * Capture errno text at the call site through strerror.
     * Callers should invoke this immediately after the failing syscall.
     */
    minictl_error(context, strerror(errno));
}

int minictl_str_copy(char *dst, size_t dst_size, const char *src)
{
    size_t src_len;

    /* Treat invalid buffers as programmer errors and report them through errno. */
    if (dst == NULL || src == NULL || dst_size == 0) {
        errno = EINVAL;
        return -1;
    }

    src_len = strlen(src);
    if (src_len >= dst_size) {
        /*
         * Leave callers with an empty string instead of a partial value.
         * Partial IDs or paths can be misleading during cleanup/debugging.
         */
        dst[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(dst, src, src_len + 1);
    return 0;
}

int minictl_path_join(char *dst, size_t dst_size, const char *left, const char *right)
{
    size_t left_len;
    size_t right_len;
    bool needs_slash;

    if (dst == NULL || left == NULL || right == NULL || dst_size == 0) {
        errno = EINVAL;
        return -1;
    }

    left_len = strlen(left);
    right_len = strlen(right);
    needs_slash = left_len > 0 && left[left_len - 1] != '/';

    /* The size check includes the optional slash and the trailing NUL byte. */
    if (left_len + (needs_slash ? 1U : 0U) + right_len >= dst_size) {
        dst[0] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(dst, left, left_len);
    if (needs_slash) {
        /*
         * Copy the right side including its trailing NUL.
         * This keeps the two branches symmetric and avoids strcat-style scans.
         */
        dst[left_len] = '/';
        memcpy(dst + left_len + 1, right, right_len + 1);
    } else {
        memcpy(dst + left_len, right, right_len + 1);
    }

    return 0;
}

static bool minictl_path_is_dir(const char *path)
{
    struct stat st;

    /*
     * Directory checks deliberately follow stat semantics.
     * Later state code can add lstat-specific behavior if symlinks matter.
     */
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int minictl_mkdir_p(const char *path, mode_t mode)
{
    char partial[4096];
    size_t len;
    size_t i;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (minictl_str_copy(partial, sizeof(partial), path) != 0) {
        return -1;
    }

    /* Normalize trailing slashes so the final mkdir/stat checks one path. */
    len = strlen(partial);
    while (len > 1 && partial[len - 1] == '/') {
        partial[len - 1] = '\0';
        len--;
    }

    /* Walk each path component, creating missing directories as we go. */
    for (i = 1; partial[i] != '\0'; i++) {
        if (partial[i] != '/') {
            continue;
        }

        partial[i] = '\0';
        if (partial[0] != '\0' && mkdir(partial, mode) != 0 && errno != EEXIST) {
            return -1;
        }
        /*
         * EEXIST is only acceptable when the component is already a directory.
         * This catches files blocking the path with a useful ENOTDIR errno.
         */
        if (!minictl_path_is_dir(partial)) {
            errno = ENOTDIR;
            return -1;
        }
        partial[i] = '/';
    }

    if (mkdir(partial, mode) != 0 && errno != EEXIST) {
        return -1;
    }

    /*
     * Validate the final component too; mkdir may have reported EEXIST for a file.
     * Callers depend on success meaning the full path is usable as a directory.
     */
    if (!minictl_path_is_dir(partial)) {
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

bool minictl_file_exists(const char *path)
{
    struct stat st;

    /*
     * False covers both "does not exist" and "cannot stat".
     * Callers that need diagnostics should call stat directly.
     */
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool minictl_dir_exists(const char *path)
{
    return path != NULL && minictl_path_is_dir(path);
}

int minictl_rm_rf(const char *path)
{
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /*
     * lstat (not stat) so a symlink is removed as a link rather than followed
     * into its target. A missing path is treated as already removed so cleanup
     * callers can stay idempotent.
     */
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *dir = opendir(path);
    struct dirent *entry;

    if (dir == NULL) {
        return -1;
    }

    /* Remove directory contents depth-first before removing the directory. */
    while ((entry = readdir(dir)) != NULL) {
        char child[MINICTL_MAX_PATH_SIZE];
        int written;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }

        if (minictl_rm_rf(child) != 0) {
            int saved_errno = errno;

            closedir(dir);
            errno = saved_errno;
            return -1;
        }
    }

    if (closedir(dir) != 0) {
        return -1;
    }

    return rmdir(path);
}
