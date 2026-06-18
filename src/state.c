#include "state.h"

#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *state_root(void)
{
    const char *override = getenv("MINICTL_STATE_DIR");

    /*
     * Empty overrides are ignored so accidental environment values do not
     * redirect state writes into the current working directory.
     */
    if (override != NULL && override[0] != '\0') {
        return override;
    }

    return MINICTL_DEFAULT_STATE_DIR;
}

static int build_containers_path(char *path, size_t path_size)
{
    /*
     * All container directories live under one fixed child directory.
     * Keeping this helper tiny prevents path layout drift across operations.
     */
    return minictl_path_join(path, path_size, state_root(), MINICTL_CONTAINERS_DIR);
}

static int build_container_path(char *path, size_t path_size, const char *id)
{
    char containers_path[MINICTL_MAX_PATH_SIZE];

    /*
     * Build paths in stages so every join gets the same overflow checks.
     * Direct snprintf would duplicate bounds behavior in every caller.
     */
    if (build_containers_path(containers_path, sizeof(containers_path)) != 0) {
        return -1;
    }

    return minictl_path_join(path, path_size, containers_path, id);
}

static int build_container_file_path(char *path, size_t path_size, const char *id, const char *file_name)
{
    char container_path[MINICTL_MAX_PATH_SIZE];

    if (build_container_path(container_path, sizeof(container_path), id) != 0) {
        return -1;
    }

    return minictl_path_join(path, path_size, container_path, file_name);
}

static int validate_container_id(const char *id)
{
    /*
     * IDs become directory names, so reject empty strings and path separators.
     * More detailed ID policy can live here when random ID generation arrives.
     */
    if (id == NULL || id[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (strchr(id, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int create_empty_file(const char *path)
{
    FILE *file = fopen(path, "a");

    /*
     * Logs are created eagerly so later logging code can assume stable paths.
     * Append mode creates the file without truncating an existing one.
     */
    if (file == NULL) {
        return -1;
    }

    if (fclose(file) != 0) {
        return -1;
    }

    return 0;
}

static int write_metadata(FILE *file, const MinictlContainerState *container)
{
    /*
     * Keep the order stable to make state files easy to inspect and diff.
     * Empty optional timestamps are still written so the schema is explicit.
     */
    if (fprintf(file, "id=%s\n", container->id) < 0) {
        return -1;
    }
    if (fprintf(file, "name=%s\n", container->name) < 0) {
        return -1;
    }
    if (fprintf(file, "pid=%ld\n", (long)container->pid) < 0) {
        return -1;
    }
    if (fprintf(file, "status=%s\n", container->status) < 0) {
        return -1;
    }
    if (fprintf(file, "rootfs=%s\n", container->rootfs) < 0) {
        return -1;
    }
    if (fprintf(file, "hostname=%s\n", container->hostname) < 0) {
        return -1;
    }
    if (fprintf(file, "command=%s\n", container->command) < 0) {
        return -1;
    }
    if (fprintf(file, "created_at=%s\n", container->created_at) < 0) {
        return -1;
    }
    if (fprintf(file, "started_at=%s\n", container->started_at) < 0) {
        return -1;
    }
    if (fprintf(file, "finished_at=%s\n", container->finished_at) < 0) {
        return -1;
    }
    if (fprintf(file, "exit_code=%d\n", container->exit_code) < 0) {
        return -1;
    }
    if (fprintf(file, "cgroup_path=%s\n", container->cgroup_path) < 0) {
        return -1;
    }
    if (fprintf(file, "network_mode=%s\n", container->network_mode) < 0) {
        return -1;
    }
    if (fprintf(file, "ip_address=%s\n", container->ip_address) < 0) {
        return -1;
    }

    return 0;
}

static int save_metadata_file(const MinictlContainerState *container, const char *path)
{
    char tmp_path[MINICTL_MAX_PATH_SIZE];
    FILE *file;
    int written;

    /*
     * Write metadata to a sibling temp file and then rename it into place.
     * That keeps interrupted saves from leaving a truncated primary meta file.
     */
    written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    file = fopen(tmp_path, "w");
    if (file == NULL) {
        return -1;
    }

    if (write_metadata(file, container) != 0) {
        int saved_errno = errno;

        fclose(file);
        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }

    if (fflush(file) != 0) {
        int saved_errno = errno;

        fclose(file);
        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }

    /*
     * fsync catches storage errors before rename.
     * Ignore only platforms where the descriptor cannot be synchronized.
     */
    if (fsync(fileno(file)) != 0 && errno != EINVAL) {
        int saved_errno = errno;

        fclose(file);
        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }

    if (fclose(file) != 0) {
        int saved_errno = errno;

        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }

    return rename(tmp_path, path);
}

static int parse_long_value(const char *value, long *result)
{
    char *end = NULL;
    long parsed;

    /*
     * Require the entire value to be numeric.
     * Accepting prefixes like "123abc" would hide corrupted metadata.
     */
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        errno = EINVAL;
        return -1;
    }

    *result = parsed;
    return 0;
}

static int copy_loaded_value(char *dst, size_t dst_size, const char *value)
{
    if (minictl_str_copy(dst, dst_size, value) != 0) {
        return -1;
    }

    return 0;
}

static int apply_metadata_pair(MinictlContainerState *container, const char *key, const char *value)
{
    long parsed;

    /*
     * Unknown keys are ignored for forward compatibility with later metadata.
     * Known keys still validate sizes and numeric formats as they are loaded.
     */
    if (strcmp(key, "id") == 0) {
        return copy_loaded_value(container->id, sizeof(container->id), value);
    }
    if (strcmp(key, "name") == 0) {
        return copy_loaded_value(container->name, sizeof(container->name), value);
    }
    if (strcmp(key, "pid") == 0) {
        if (parse_long_value(value, &parsed) != 0) {
            return -1;
        }
        container->pid = (pid_t)parsed;
        return 0;
    }
    if (strcmp(key, "status") == 0) {
        return copy_loaded_value(container->status, sizeof(container->status), value);
    }
    if (strcmp(key, "rootfs") == 0) {
        return copy_loaded_value(container->rootfs, sizeof(container->rootfs), value);
    }
    if (strcmp(key, "hostname") == 0) {
        return copy_loaded_value(container->hostname, sizeof(container->hostname), value);
    }
    if (strcmp(key, "command") == 0) {
        return copy_loaded_value(container->command, sizeof(container->command), value);
    }
    if (strcmp(key, "created_at") == 0) {
        return copy_loaded_value(container->created_at, sizeof(container->created_at), value);
    }
    if (strcmp(key, "started_at") == 0) {
        return copy_loaded_value(container->started_at, sizeof(container->started_at), value);
    }
    if (strcmp(key, "finished_at") == 0) {
        return copy_loaded_value(container->finished_at, sizeof(container->finished_at), value);
    }
    if (strcmp(key, "exit_code") == 0) {
        if (parse_long_value(value, &parsed) != 0) {
            return -1;
        }
        container->exit_code = (int)parsed;
        return 0;
    }
    if (strcmp(key, "cgroup_path") == 0) {
        return copy_loaded_value(container->cgroup_path, sizeof(container->cgroup_path), value);
    }
    if (strcmp(key, "network_mode") == 0) {
        return copy_loaded_value(container->network_mode, sizeof(container->network_mode), value);
    }
    if (strcmp(key, "ip_address") == 0) {
        return copy_loaded_value(container->ip_address, sizeof(container->ip_address), value);
    }

    return 0;
}

static int load_metadata_file(const char *path, MinictlContainerState *container)
{
    FILE *file = fopen(path, "r");
    char line[MINICTL_MAX_COMMAND_SIZE + 64];

    if (file == NULL) {
        return -1;
    }

    memset(container, 0, sizeof(*container));
    container->exit_code = -1;

    /*
     * Start with default values before applying file keys.
     * This keeps old metadata files readable if later optional fields are absent.
     */
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals;
        char *newline;

        newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        equals = strchr(line, '=');
        if (equals == NULL) {
            /*
             * Every non-empty metadata line must use key=value.
             * Failing fast makes hand-edited or truncated files obvious.
             */
            fclose(file);
            errno = EINVAL;
            return -1;
        }

        *equals = '\0';
        if (apply_metadata_pair(container, line, equals + 1) != 0) {
            fclose(file);
            return -1;
        }
    }

    if (ferror(file)) {
        fclose(file);
        return -1;
    }

    return fclose(file);
}

int state_init(void)
{
    char containers_path[MINICTL_MAX_PATH_SIZE];

    /*
     * Initialize both the root and containers directory.
     * This is safe to call repeatedly and keeps callers from preflighting state.
     */
    if (minictl_mkdir_p(state_root(), 0755) != 0) {
        return -1;
    }

    if (build_containers_path(containers_path, sizeof(containers_path)) != 0) {
        return -1;
    }

    return minictl_mkdir_p(containers_path, 0755);
}

int state_create_container(const MinictlContainerState *container)
{
    char container_path[MINICTL_MAX_PATH_SIZE];
    char meta_path[MINICTL_MAX_PATH_SIZE];
    char stdout_path[MINICTL_MAX_PATH_SIZE];
    char stderr_path[MINICTL_MAX_PATH_SIZE];

    if (container == NULL || validate_container_id(container->id) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (state_init() != 0) {
        return -1;
    }

    if (build_container_path(container_path, sizeof(container_path), container->id) != 0) {
        return -1;
    }

    /*
     * create should fail on duplicate IDs.
     * Later lifecycle code can decide whether an existing stopped container is reusable.
     */
    if (mkdir(container_path, 0755) != 0) {
        return -1;
    }

    if (build_container_file_path(meta_path, sizeof(meta_path), container->id, MINICTL_META_FILE) != 0) {
        return -1;
    }
    if (build_container_file_path(stdout_path, sizeof(stdout_path), container->id, MINICTL_STDOUT_LOG_FILE) != 0) {
        return -1;
    }
    if (build_container_file_path(stderr_path, sizeof(stderr_path), container->id, MINICTL_STDERR_LOG_FILE) != 0) {
        return -1;
    }

    /*
     * Metadata is written before logs so a directory never looks valid
     * unless its primary state file exists.
     */
    if (save_metadata_file(container, meta_path) != 0 ||
        create_empty_file(stdout_path) != 0 ||
        create_empty_file(stderr_path) != 0) {
        int saved_errno = errno;

        unlink(stderr_path);
        unlink(stdout_path);
        unlink(meta_path);
        rmdir(container_path);
        errno = saved_errno;
        return -1;
    }

    return 0;
}

int state_load_container(const char *id, MinictlContainerState *container)
{
    char meta_path[MINICTL_MAX_PATH_SIZE];

    /*
     * Load by ID only; callers should not pass arbitrary metadata paths.
     * That keeps all state access inside the configured state root.
     */
    if (container == NULL || validate_container_id(id) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (build_container_file_path(meta_path, sizeof(meta_path), id, MINICTL_META_FILE) != 0) {
        return -1;
    }

    return load_metadata_file(meta_path, container);
}

int state_save_container(const MinictlContainerState *container)
{
    char meta_path[MINICTL_MAX_PATH_SIZE];

    /*
     * Save updates an existing metadata path but does not create the container dir.
     * New containers should flow through state_create_container first.
     */
    if (container == NULL || validate_container_id(container->id) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (build_container_file_path(meta_path, sizeof(meta_path), container->id, MINICTL_META_FILE) != 0) {
        return -1;
    }

    return save_metadata_file(container, meta_path);
}

int state_container_file_path(const char *id, const char *file_name, char *path, size_t path_size)
{
    /*
     * Keep public path construction behind the same validation and join helpers
     * used by the internal state loader/saver.
     */
    if (file_name == NULL || file_name[0] == '\0' || strchr(file_name, '/') != NULL) {
        errno = EINVAL;
        return -1;
    }

    if (validate_container_id(id) != 0) {
        return -1;
    }

    return build_container_file_path(path, path_size, id, file_name);
}

int state_list_containers(MinictlContainerList *list)
{
    char containers_path[MINICTL_MAX_PATH_SIZE];
    DIR *dir;
    struct dirent *entry;

    if (list == NULL) {
        errno = EINVAL;
        return -1;
    }

    list->items = NULL;
    list->count = 0;

    /*
     * Listing an empty fresh machine should succeed and return count zero.
     * state_init provides that behavior by creating the empty store first.
     */
    if (state_init() != 0) {
        return -1;
    }

    if (build_containers_path(containers_path, sizeof(containers_path)) != 0) {
        return -1;
    }

    dir = opendir(containers_path);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        MinictlContainerState loaded;
        MinictlContainerState *new_items;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /*
         * Directory names are treated as container IDs. Entries with no metadata
         * file (ENOENT) or that are not directories (ENOTDIR) are stray files or
         * half-created dirs, not containers, so skip them instead of failing the
         * whole listing. Genuinely corrupt metadata still fails loudly so it stays
         * visible.
         */
        if (state_load_container(entry->d_name, &loaded) != 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                continue;
            }
            closedir(dir);
            state_free_container_list(list);
            return -1;
        }

        /*
         * Lists are small in v1, so realloc per item keeps the implementation direct.
         * The API still owns the array, making it easy to optimize later.
         */
        new_items = realloc(list->items, sizeof(list->items[0]) * (list->count + 1));
        if (new_items == NULL) {
            closedir(dir);
            state_free_container_list(list);
            return -1;
        }

        list->items = new_items;
        list->items[list->count] = loaded;
        list->count++;
    }

    if (closedir(dir) != 0) {
        state_free_container_list(list);
        return -1;
    }

    return 0;
}

void state_free_container_list(MinictlContainerList *list)
{
    if (list == NULL) {
        return;
    }

    /*
     * Reset fields after free so callers can safely reuse the list object
     * or call this helper during error cleanup.
     */
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

int state_remove_container(const char *id)
{
    char container_path[MINICTL_MAX_PATH_SIZE];
    char meta_path[MINICTL_MAX_PATH_SIZE];
    char meta_tmp_path[MINICTL_MAX_PATH_SIZE];
    char stdout_path[MINICTL_MAX_PATH_SIZE];
    char stderr_path[MINICTL_MAX_PATH_SIZE];
    int written;

    if (validate_container_id(id) != 0) {
        return -1;
    }

    if (build_container_path(container_path, sizeof(container_path), id) != 0) {
        return -1;
    }
    if (build_container_file_path(meta_path, sizeof(meta_path), id, MINICTL_META_FILE) != 0) {
        return -1;
    }
    written = snprintf(meta_tmp_path, sizeof(meta_tmp_path), "%s.tmp", meta_path);
    if (written < 0 || (size_t)written >= sizeof(meta_tmp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (build_container_file_path(stdout_path, sizeof(stdout_path), id, MINICTL_STDOUT_LOG_FILE) != 0) {
        return -1;
    }
    if (build_container_file_path(stderr_path, sizeof(stderr_path), id, MINICTL_STDERR_LOG_FILE) != 0) {
        return -1;
    }

    /*
     * Remove files before the directory because rmdir only succeeds on empty dirs.
     * Missing log files should not block metadata removal.
     * Missing meta or container directories still fail so callers notice bad IDs.
     */
    if (unlink(stdout_path) != 0 && errno != ENOENT) {
        return -1;
    }
    if (unlink(stderr_path) != 0 && errno != ENOENT) {
        return -1;
    }
    if (unlink(meta_tmp_path) != 0 && errno != ENOENT) {
        return -1;
    }
    if (unlink(meta_path) != 0) {
        return -1;
    }

    return rmdir(container_path);
}
