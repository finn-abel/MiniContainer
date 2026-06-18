#ifndef MINICTL_CGROUPS_H
#define MINICTL_CGROUPS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Optional cgroup v2 resource limits parsed from the run command.
 * Each boolean records whether the matching controller should be configured.
 */
typedef struct CgroupLimits {
    bool memory_max_set;
    unsigned long long memory_max;
    bool pids_max_set;
    long pids_max;
    bool cpu_max_set;
    long cpu_quota;
    long cpu_period;
} CgroupLimits;

/*
 * Parse human-facing resource flag values into cgroup v2 file values.
 * Memory accepts bytes or K/M/G suffixes; CPU accepts quota:period.
 */
int cgroup_parse_memory(const char *value, unsigned long long *bytes);
int cgroup_parse_cpu(const char *value, long *quota, long *period);

/*
 * Return true when no resource limit flags were provided.
 * Callers use this to skip cgroup filesystem work entirely.
 */
bool cgroup_limits_empty(const CgroupLimits *limits);

/*
 * Create the minictl cgroup and write any requested limits.
 * The returned path points at /sys/fs/cgroup/minictl/<container_id>.
 */
int cgroup_create(const char *container_id, const CgroupLimits *limits, char *path, size_t path_size);

/*
 * Move a process into an existing cgroup by writing cgroup.procs.
 * This should happen soon after clone so limits apply to the container init.
 */
int cgroup_add_pid(const char *cgroup_path, pid_t pid);

/*
 * Remove one minictl cgroup directory when container state is removed.
 * ENOENT is treated as already-clean because cgroups are optional.
 */
int cgroup_remove(const char *cgroup_path);

#endif
