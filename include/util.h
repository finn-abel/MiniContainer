#ifndef MINICTL_UTIL_H
#define MINICTL_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Print minictl-prefixed errors for normal messages or errno-backed failures.
 * Callers provide the subsystem or syscall name as context when available.
 */
void minictl_error(const char *context, const char *message);
void minictl_perror(const char *context);

/*
 * Copy strings and build filesystem paths with explicit bounds checks.
 * These helpers return -1 and set errno when the destination is too small.
 */
int minictl_str_copy(char *dst, size_t dst_size, const char *src);
int minictl_path_join(char *dst, size_t dst_size, const char *left, const char *right);

/*
 * Create a directory tree using mkdir-style permissions for new components.
 * Existing directories are accepted, but non-directory path components fail.
 */
int minictl_mkdir_p(const char *path, mode_t mode);

/*
 * Test whether a path exists and has the expected filesystem type.
 * These helpers deliberately return false for missing paths and stat failures.
 */
bool minictl_file_exists(const char *path);
bool minictl_dir_exists(const char *path);

#endif
