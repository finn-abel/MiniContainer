#ifndef MINICTL_OCI_H
#define MINICTL_OCI_H

#include <stddef.h>

#include "cgroups.h"

/*
 * Minimal mapping of an OCI runtime config.json into minictl runtime settings.
 *
 * This is an intentionally small starting point: only root.path, process.args,
 * process.env, hostname, and linux.resources (memory/pids/cpu) are read. Mounts,
 * capabilities, seccomp, hooks, and namespace configuration are not yet mapped.
 * Any field absent from the config is left NULL/zero so callers can apply their
 * own defaults or CLI overrides.
 */
typedef struct OciConfig {
    /* Resolved root filesystem path (root.path), or NULL when absent. */
    char *rootfs;
    /* Container hostname, or NULL when absent. */
    char *hostname;
    /* NULL-terminated process.args vector usable directly as argv, or NULL. */
    char **args;
    size_t args_count;
    /* NULL-terminated process.env vector of "KEY=value" strings, or NULL. */
    char **env;
    size_t env_count;
    /* Cgroup limits from linux.resources; each *_set flag marks presence. */
    CgroupLimits limits;
} OciConfig;

/*
 * Load and map an OCI runtime config.json from path into out.
 * root.path is resolved relative to the config file's directory when relative.
 * Returns 0 on success and -1 with errno set on read or parse failure. On
 * failure out is left zeroed. Call oci_config_free on a successful result.
 */
int oci_load_config(const char *path, OciConfig *out);

/*
 * Release every allocation owned by an OciConfig and zero it.
 * Safe to call on a zeroed config.
 */
void oci_config_free(OciConfig *config);

#endif
