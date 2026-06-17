#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
        if (!minictl_path_is_dir(partial)) {
            errno = ENOTDIR;
            return -1;
        }
        partial[i] = '/';
    }

    if (mkdir(partial, mode) != 0 && errno != EEXIST) {
        return -1;
    }

    if (!minictl_path_is_dir(partial)) {
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

bool minictl_file_exists(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool minictl_dir_exists(const char *path)
{
    return path != NULL && minictl_path_is_dir(path);
}
