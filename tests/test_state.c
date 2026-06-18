#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "state.h"
#include "util.h"

static void fill_container(MinictlContainerState *container) {
    memset(container, 0, sizeof(*container));

    assert(minictl_str_copy(container->id, sizeof(container->id), "8f3a1c2d") == 0);
    assert(minictl_str_copy(container->name, sizeof(container->name), "web") == 0);
    container->pid = 42110;
    assert(minictl_str_copy(container->status, sizeof(container->status), "running") == 0);
    assert(minictl_str_copy(container->rootfs, sizeof(container->rootfs), "/abs/path/rootfs/alpine") == 0);
    assert(minictl_str_copy(container->hostname, sizeof(container->hostname), "web") == 0);
    assert(minictl_str_copy(container->command, sizeof(container->command), "/bin/sh") == 0);
    assert(minictl_str_copy(container->created_at, sizeof(container->created_at), "2026-06-17T09:00:00Z") == 0);
    assert(minictl_str_copy(container->started_at, sizeof(container->started_at), "2026-06-17T09:01:00Z") == 0);
    assert(minictl_str_copy(container->finished_at, sizeof(container->finished_at), "") == 0);
    container->exit_code = -1;
    assert(minictl_str_copy(
               container->cgroup_path,
               sizeof(container->cgroup_path),
               "/sys/fs/cgroup/minictl/8f3a1c2d") == 0);
}

static void build_state_file_path(char *path, size_t path_size, const char *root, const char *id, const char *file_name) {
    char containers_path[MINICTL_MAX_PATH_SIZE];
    char container_path[MINICTL_MAX_PATH_SIZE];

    assert(minictl_path_join(containers_path, sizeof(containers_path), root, MINICTL_CONTAINERS_DIR) == 0);
    assert(minictl_path_join(container_path, sizeof(container_path), containers_path, id) == 0);
    assert(minictl_path_join(path, path_size, container_path, file_name) == 0);
}

static void cleanup_state_root(const char *root, const char *id) {
    char containers_path[MINICTL_MAX_PATH_SIZE];

    assert(state_remove_container(id) == 0);
    assert(minictl_path_join(containers_path, sizeof(containers_path), root, MINICTL_CONTAINERS_DIR) == 0);
    assert(rmdir(containers_path) == 0);
    assert(rmdir(root) == 0);
}

static void test_state_create_load_update_list_remove(void) {
    char root_template[] = "/tmp/minictl-state-test-XXXXXX";
    char *root;
    char meta_path[MINICTL_MAX_PATH_SIZE];
    char meta_tmp_path[MINICTL_MAX_PATH_SIZE];
    char stdout_path[MINICTL_MAX_PATH_SIZE];
    char stderr_path[MINICTL_MAX_PATH_SIZE];
    MinictlContainerState container;
    MinictlContainerState loaded;
    MinictlContainerList list;

    root = mkdtemp(root_template);
    assert(root != NULL);
    assert(setenv("MINICTL_STATE_DIR", root, 1) == 0);

    fill_container(&container);

    assert(state_init() == 0);
    assert(state_create_container(&container) == 0);

    build_state_file_path(meta_path, sizeof(meta_path), root, container.id, MINICTL_META_FILE);
    assert(snprintf(meta_tmp_path, sizeof(meta_tmp_path), "%s.tmp", meta_path) > 0);
    build_state_file_path(stdout_path, sizeof(stdout_path), root, container.id, MINICTL_STDOUT_LOG_FILE);
    build_state_file_path(stderr_path, sizeof(stderr_path), root, container.id, MINICTL_STDERR_LOG_FILE);

    assert(minictl_file_exists(meta_path));
    assert(minictl_file_exists(stdout_path));
    assert(minictl_file_exists(stderr_path));

    assert(state_load_container(container.id, &loaded) == 0);
    assert(strcmp(loaded.id, "8f3a1c2d") == 0);
    assert(strcmp(loaded.name, "web") == 0);
    assert(loaded.pid == 42110);
    assert(strcmp(loaded.status, "running") == 0);
    assert(strcmp(loaded.rootfs, "/abs/path/rootfs/alpine") == 0);
    assert(strcmp(loaded.hostname, "web") == 0);
    assert(strcmp(loaded.command, "/bin/sh") == 0);
    assert(strcmp(loaded.created_at, "2026-06-17T09:00:00Z") == 0);
    assert(strcmp(loaded.started_at, "2026-06-17T09:01:00Z") == 0);
    assert(strcmp(loaded.finished_at, "") == 0);
    assert(loaded.exit_code == -1);
    assert(strcmp(loaded.cgroup_path, "/sys/fs/cgroup/minictl/8f3a1c2d") == 0);

    assert(minictl_str_copy(loaded.status, sizeof(loaded.status), "exited") == 0);
    assert(minictl_str_copy(loaded.finished_at, sizeof(loaded.finished_at), "2026-06-17T09:02:00Z") == 0);
    loaded.exit_code = 0;
    assert(state_save_container(&loaded) == 0);
    assert(!minictl_file_exists(meta_tmp_path));

    memset(&loaded, 0, sizeof(loaded));
    assert(state_load_container(container.id, &loaded) == 0);
    assert(strcmp(loaded.status, "exited") == 0);
    assert(strcmp(loaded.finished_at, "2026-06-17T09:02:00Z") == 0);
    assert(loaded.exit_code == 0);

    assert(state_list_containers(&list) == 0);
    assert(list.count == 1);
    assert(strcmp(list.items[0].id, "8f3a1c2d") == 0);
    assert(strcmp(list.items[0].status, "exited") == 0);
    state_free_container_list(&list);

    cleanup_state_root(root, container.id);
}

static void setup_root(char *root_template, const char **root) {
    *root = mkdtemp(root_template);
    assert(*root != NULL);
    assert(setenv("MINICTL_STATE_DIR", *root, 1) == 0);
    assert(state_init() == 0);
}

static void test_state_load_missing_and_corrupt(void) {
    char root_template[] = "/tmp/minictl-state-load-XXXXXX";
    const char *root;
    MinictlContainerState container;
    char meta_path[MINICTL_MAX_PATH_SIZE];
    FILE *file;

    setup_root(root_template, &root);

    /* A container that does not exist loads as ENOENT. */
    errno = 0;
    assert(state_load_container("ghost", &container) == -1);
    assert(errno == ENOENT);

    /* A metadata file with a malformed line loads as EINVAL. */
    fill_container(&container);
    assert(state_create_container(&container) == 0);

    build_state_file_path(meta_path, sizeof(meta_path), root, container.id, MINICTL_META_FILE);
    file = fopen(meta_path, "w");
    assert(file != NULL);
    assert(fputs("not-a-key-value-line\n", file) != EOF);
    assert(fclose(file) == 0);

    errno = 0;
    assert(state_load_container(container.id, &container) == -1);
    assert(errno == EINVAL);

    cleanup_state_root(root, "8f3a1c2d");
}

static void test_state_create_duplicate_fails(void) {
    char root_template[] = "/tmp/minictl-state-dup-XXXXXX";
    const char *root;
    MinictlContainerState container;

    setup_root(root_template, &root);
    fill_container(&container);
    assert(state_create_container(&container) == 0);

    errno = 0;
    assert(state_create_container(&container) == -1);
    assert(errno == EEXIST);

    cleanup_state_root(root, "8f3a1c2d");
}

static void test_state_list_skips_stray_entries(void) {
    char root_template[] = "/tmp/minictl-state-list-XXXXXX";
    const char *root;
    MinictlContainerState container;
    MinictlContainerList list;
    char containers_path[MINICTL_MAX_PATH_SIZE];
    char stray_file[MINICTL_MAX_PATH_SIZE];
    char stray_dir[MINICTL_MAX_PATH_SIZE];
    FILE *file;

    setup_root(root_template, &root);
    fill_container(&container);
    assert(state_create_container(&container) == 0);

    assert(minictl_path_join(containers_path, sizeof(containers_path), root, MINICTL_CONTAINERS_DIR) == 0);

    /* A stray non-container file under containers/ must be skipped, not fail ps. */
    assert(minictl_path_join(stray_file, sizeof(stray_file), containers_path, "README") == 0);
    file = fopen(stray_file, "w");
    assert(file != NULL);
    assert(fclose(file) == 0);

    /* A half-created directory with no meta file must also be skipped. */
    assert(minictl_path_join(stray_dir, sizeof(stray_dir), containers_path, "halfbaked") == 0);
    assert(mkdir(stray_dir, 0755) == 0);

    assert(state_list_containers(&list) == 0);
    assert(list.count == 1);
    assert(strcmp(list.items[0].id, "8f3a1c2d") == 0);
    state_free_container_list(&list);

    assert(remove(stray_file) == 0);
    assert(rmdir(stray_dir) == 0);
    cleanup_state_root(root, "8f3a1c2d");
}

static void test_state_file_path_rejects_bad_names(void) {
    char path[MINICTL_MAX_PATH_SIZE];

    errno = 0;
    assert(state_container_file_path("abc", "../escape", path, sizeof(path)) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(state_container_file_path("ab/cd", MINICTL_META_FILE, path, sizeof(path)) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    test_state_create_load_update_list_remove();
    test_state_load_missing_and_corrupt();
    test_state_create_duplicate_fails();
    test_state_list_skips_stray_entries();
    test_state_file_path_rejects_bad_names();

    printf("All state tests passed.\n");
    return 0;
}
