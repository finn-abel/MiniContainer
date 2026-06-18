#include "cgroups.h"

#include "config.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/vfs.h>
#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif
#endif

/*
 * cgroup v2 is mounted as one unified hierarchy.
 * MiniContainer keeps all runtime-created groups under one predictable parent.
 */
#define MINICTL_CGROUP_ROOT "/sys/fs/cgroup"
#define MINICTL_CGROUP_PARENT "/sys/fs/cgroup/minictl"

static int parse_positive_long(const char *value, long *out)
{
    char *end = NULL;
    long parsed;

    /*
     * Controller values such as pids.max and cpu.max do not accept partial
     * numbers. Rejecting loose input here keeps parser errors close to source.
     */
    if (value == NULL || value[0] == '\0' || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0) {
        errno = EINVAL;
        return -1;
    }

    *out = parsed;
    return 0;
}

static int multiply_memory(unsigned long long base, unsigned long long multiplier, unsigned long long *bytes)
{
    /*
     * Detect overflow before multiplying so invalid huge user input does not
     * silently wrap into a much smaller cgroup memory.max value.
     */
    if (base > ULLONG_MAX / multiplier) {
        errno = ERANGE;
        return -1;
    }

    *bytes = base * multiplier;
    return 0;
}

int cgroup_parse_memory(const char *value, unsigned long long *bytes)
{
    char *end = NULL;
    unsigned long long parsed;
    unsigned long long multiplier = 1;

    /*
     * Memory limits must be explicit positive byte counts after parsing.
     * Zero is rejected because it would make the container unusable immediately.
     */
    if (value == NULL || value[0] == '\0' || bytes == NULL) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || parsed == 0) {
        errno = errno == 0 ? EINVAL : errno;
        return -1;
    }

    /*
     * Keep suffix support intentionally small and obvious.
     * cgroup v2 receives the final byte count, not the human suffix form.
     */
    if (*end != '\0') {
        if ((end[0] == 'K' || end[0] == 'k') && end[1] == '\0') {
            multiplier = 1024ULL;
        } else if ((end[0] == 'M' || end[0] == 'm') && end[1] == '\0') {
            multiplier = 1024ULL * 1024ULL;
        } else if ((end[0] == 'G' || end[0] == 'g') && end[1] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else {
            errno = EINVAL;
            return -1;
        }
    }

    return multiply_memory(parsed, multiplier, bytes);
}

int cgroup_parse_cpu(const char *value, long *quota, long *period)
{
    char copy[64];
    char *separator;

    if (value == NULL || quota == NULL || period == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * The CLI uses quota:period because it is compact for humans.
     * cgroup v2 later receives the same pair as "quota period" in cpu.max.
     */
    if (minictl_str_copy(copy, sizeof(copy), value) != 0) {
        return -1;
    }

    separator = strchr(copy, ':');
    if (separator == NULL || strchr(separator + 1, ':') != NULL) {
        errno = EINVAL;
        return -1;
    }

    *separator = '\0';
    if (parse_positive_long(copy, quota) != 0) {
        return -1;
    }
    if (parse_positive_long(separator + 1, period) != 0) {
        return -1;
    }

    return 0;
}

bool cgroup_limits_empty(const CgroupLimits *limits)
{
    /*
     * Treat a NULL limits pointer as empty so cleanup and optional setup paths
     * can be defensive without forcing every caller to pre-check arguments.
     */
    return limits == NULL ||
           (!limits->memory_max_set && !limits->pids_max_set && !limits->cpu_max_set);
}

static int build_cgroup_path(char *path, size_t path_size, const char *container_id)
{
    /*
     * The container ID becomes a directory name under /sys/fs/cgroup/minictl.
     * Slash rejection prevents callers from escaping the minictl parent group.
     */
    if (container_id == NULL || container_id[0] == '\0' || strchr(container_id, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }

    return minictl_path_join(path, path_size, MINICTL_CGROUP_PARENT, container_id);
}

static int write_text_file(const char *path, const char *value)
{
    FILE *file;

    /*
     * cgroup controller files are tiny text files.
     * Use stdio for bounded formatting while still surfacing fclose errors.
     */
    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    if (fputs(value, file) == EOF) {
        int saved_errno = errno;

        fclose(file);
        errno = saved_errno;
        return -1;
    }

    return fclose(file);
}

#ifdef __linux__
static int write_unsigned_file(const char *path, unsigned long long value)
{
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%llu\n", value);

    /*
     * cgroup files expect text, including numeric values.
     * Format first so the lower helper only has to handle write semantics.
     */
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return write_text_file(path, buffer);
}
#endif

static int write_long_file(const char *path, long value)
{
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%ld\n", value);

    /*
     * This helper is shared by pids.max and cgroup.procs.
     * Both files accept a single signed decimal value followed by a newline.
     */
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return write_text_file(path, buffer);
}

#ifdef __linux__
static int write_limit_files(const char *cgroup_path, const CgroupLimits *limits)
{
    char file_path[MINICTL_MAX_PATH_SIZE];

    /*
     * Write only the controllers explicitly requested by the CLI.
     * Missing controller support or permission failures are returned directly.
     */
    if (limits->memory_max_set) {
        if (minictl_path_join(file_path, sizeof(file_path), cgroup_path, "memory.max") != 0) {
            return -1;
        }
        if (write_unsigned_file(file_path, limits->memory_max) != 0) {
            return -1;
        }
    }

    if (limits->pids_max_set) {
        if (minictl_path_join(file_path, sizeof(file_path), cgroup_path, "pids.max") != 0) {
            return -1;
        }
        if (write_long_file(file_path, limits->pids_max) != 0) {
            return -1;
        }
    }

    if (limits->cpu_max_set) {
        char cpu_value[64];
        int written;

        /*
         * The CLI accepts quota:period, but cgroup v2's cpu.max uses a space.
         * Example: "50000 100000" allows 50ms CPU time per 100ms period.
         */
        if (minictl_path_join(file_path, sizeof(file_path), cgroup_path, "cpu.max") != 0) {
            return -1;
        }

        written = snprintf(cpu_value, sizeof(cpu_value), "%ld %ld\n", limits->cpu_quota, limits->cpu_period);
        if (written < 0 || (size_t)written >= sizeof(cpu_value)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (write_text_file(file_path, cpu_value) != 0) {
            return -1;
        }
    }

    return 0;
}

static int cgroup_v2_available(void)
{
    struct statfs fs;

    /*
     * cgroup v2 has a distinct filesystem magic. If this check fails,
     * resource flags must fail clearly instead of being silently ignored.
     */
    if (statfs(MINICTL_CGROUP_ROOT, &fs) != 0) {
        return -1;
    }

    if ((unsigned long)fs.f_type != (unsigned long)CGROUP2_SUPER_MAGIC) {
        errno = EOPNOTSUPP;
        return -1;
    }

    return 0;
}
#endif

int cgroup_create(const char *container_id, const CgroupLimits *limits, char *path, size_t path_size)
{
    char cgroup_path[MINICTL_MAX_PATH_SIZE];

    if (path == NULL || path_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (build_cgroup_path(cgroup_path, sizeof(cgroup_path), container_id) != 0) {
        return -1;
    }
    if (minictl_str_copy(path, path_size, cgroup_path) != 0) {
        return -1;
    }

    /*
     * Always return the expected cgroup path to metadata, even when no limits
     * are active. Actual filesystem creation is skipped unless limits exist.
     */
    if (cgroup_limits_empty(limits)) {
        return 0;
    }

#ifdef __linux__
    if (cgroup_v2_available() != 0) {
        return -1;
    }

    /*
     * The parent minictl directory is shared by all containers.
     * Container-specific directories remain one level down for easy cleanup.
     */
    if (minictl_mkdir_p(MINICTL_CGROUP_PARENT, 0755) != 0) {
        return -1;
    }
    if (mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    if (write_limit_files(cgroup_path, limits) != 0) {
        int saved_errno = errno;

        rmdir(cgroup_path);
        errno = saved_errno;
        return -1;
    }

    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int cgroup_add_pid(const char *cgroup_path, pid_t pid)
{
    char procs_path[MINICTL_MAX_PATH_SIZE];

    /*
     * cgroup.procs takes host-visible PIDs.
     * The namespace child PID returned by clone is exactly what we need here.
     */
    if (cgroup_path == NULL || cgroup_path[0] == '\0' || pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (minictl_path_join(procs_path, sizeof(procs_path), cgroup_path, "cgroup.procs") != 0) {
        return -1;
    }

    return write_long_file(procs_path, (long)pid);
}

int cgroup_remove(const char *cgroup_path)
{
    /*
     * Empty paths are no-ops because containers without resource flags never
     * create their metadata cgroup path on disk.
     */
    if (cgroup_path == NULL || cgroup_path[0] == '\0') {
        return 0;
    }

    /*
     * Only remove the leaf directory. If processes or child cgroups remain,
     * rmdir reports that real cleanup problem to the caller.
     */
    if (rmdir(cgroup_path) != 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
}
