#ifndef MINICTL_STATE_H
#define MINICTL_STATE_H

#include <stddef.h>
#include <sys/types.h>

#include "config.h"

/*
 * Durable metadata for one container.
 * String fields mirror the line-based meta file stored on disk.
 */
typedef struct MinictlContainerState {
    char id[MINICTL_MAX_ID_SIZE];
    char name[MINICTL_MAX_ID_SIZE];
    pid_t pid;
    char status[MINICTL_MAX_ID_SIZE];
    char rootfs[MINICTL_MAX_PATH_SIZE];
    char hostname[MINICTL_MAX_ID_SIZE];
    char command[MINICTL_MAX_COMMAND_SIZE];
    char created_at[64];
    char started_at[64];
    char finished_at[64];
    int exit_code;
    char cgroup_path[MINICTL_MAX_PATH_SIZE];
    char network_mode[MINICTL_MAX_ID_SIZE];
    char ip_address[MINICTL_MAX_ID_SIZE];
    char published_ports[MINICTL_MAX_COMMAND_SIZE];
    pid_t proxy_pid;
    char rootfs_mode[MINICTL_MAX_ID_SIZE];
} MinictlContainerState;

/*
 * Owned result returned by state_list_containers.
 * Call state_free_container_list after consuming the items.
 */
typedef struct MinictlContainerList {
    MinictlContainerState *items;
    size_t count;
} MinictlContainerList;

/*
 * Ensure the state root and containers directory exist.
 * Uses MINICTL_STATE_DIR when set, otherwise MINICTL_DEFAULT_STATE_DIR.
 */
int state_init(void);

/*
 * Create a new container directory with metadata and empty log files.
 * Fails if the container already exists or required metadata is invalid.
 */
int state_create_container(const MinictlContainerState *container);

/*
 * Load or save the line-based metadata file for a single container.
 * The id field determines which container directory is used.
 */
int state_load_container(const char *id, MinictlContainerState *container);
int state_save_container(const MinictlContainerState *container);

/*
 * Build a path to one file inside a container state directory.
 * This exposes state-owned layout for commands that need to print paths.
 */
int state_container_file_path(const char *id, const char *file_name, char *path, size_t path_size);

/*
 * Load all container metadata entries currently present in the state store.
 * Missing state directories are initialized before listing.
 */
int state_list_containers(MinictlContainerList *list);
void state_free_container_list(MinictlContainerList *list);

/*
 * Remove metadata and logs for one container, then remove its directory.
 * Runtime cleanup is handled elsewhere; this only edits state on disk.
 */
int state_remove_container(const char *id);

#endif
