#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oci.h"

static void write_config(char *path_template, const char *contents) {
    int fd = mkstemp(path_template);
    size_t len = strlen(contents);

    assert(fd >= 0);
    assert(write(fd, contents, len) == (ssize_t)len);
    assert(close(fd) == 0);
}

static void test_load_full_config(void) {
    char path[] = "/tmp/minictl-oci-XXXXXX";
    OciConfig config;

    /* Absolute root.path is kept as-is; nested process/linux fields are mapped. */
    const char *json =
        "{\n"
        "  \"hostname\": \"oci-host\",\n"
        "  \"root\": { \"path\": \"/abs/rootfs\", \"readonly\": false },\n"
        "  \"process\": {\n"
        "    \"args\": [\"/bin/sh\", \"-c\", \"echo hi\"],\n"
        "    \"env\": [\"PATH=/bin\", \"FOO=bar\"]\n"
        "  },\n"
        "  \"linux\": { \"resources\": {\n"
        "    \"memory\": { \"limit\": 134217728 },\n"
        "    \"pids\": { \"limit\": 64 },\n"
        "    \"cpu\": { \"quota\": 50000, \"period\": 100000 }\n"
        "  } }\n"
        "}\n";

    write_config(path, json);
    assert(oci_load_config(path, &config) == 0);

    assert(config.rootfs != NULL && strcmp(config.rootfs, "/abs/rootfs") == 0);
    assert(config.hostname != NULL && strcmp(config.hostname, "oci-host") == 0);

    assert(config.args_count == 3);
    assert(strcmp(config.args[0], "/bin/sh") == 0);
    assert(strcmp(config.args[1], "-c") == 0);
    assert(strcmp(config.args[2], "echo hi") == 0);
    assert(config.args[3] == NULL);

    assert(config.env_count == 2);
    assert(strcmp(config.env[0], "PATH=/bin") == 0);
    assert(strcmp(config.env[1], "FOO=bar") == 0);
    assert(config.env[2] == NULL);

    assert(config.limits.memory_max_set && config.limits.memory_max == 134217728ULL);
    assert(config.limits.pids_max_set && config.limits.pids_max == 64);
    assert(config.limits.cpu_max_set && config.limits.cpu_quota == 50000 && config.limits.cpu_period == 100000);

    oci_config_free(&config);
    assert(config.rootfs == NULL && config.args == NULL && config.env == NULL);
    unlink(path);
}

static void test_relative_root_resolves_against_bundle(void) {
    char path[] = "/tmp/minictl-oci-XXXXXX";
    OciConfig config;
    char expected[512];
    const char *slash;

    write_config(path, "{ \"root\": { \"path\": \"rootfs\" } }");
    assert(oci_load_config(path, &config) == 0);

    /* Relative root.path joins the directory holding the config file. */
    slash = strrchr(path, '/');
    assert(slash != NULL);
    snprintf(expected, sizeof(expected), "%.*s/rootfs", (int)(slash - path), path);
    assert(config.rootfs != NULL && strcmp(config.rootfs, expected) == 0);

    oci_config_free(&config);
    unlink(path);
}

static void test_partial_config_leaves_unset_fields(void) {
    char path[] = "/tmp/minictl-oci-XXXXXX";
    OciConfig config;

    write_config(path, "{ \"process\": { \"args\": [\"/bin/true\"] } }");
    assert(oci_load_config(path, &config) == 0);

    assert(config.rootfs == NULL);
    assert(config.hostname == NULL);
    assert(config.env == NULL && config.env_count == 0);
    assert(config.args_count == 1 && strcmp(config.args[0], "/bin/true") == 0);
    assert(!config.limits.memory_max_set && !config.limits.pids_max_set && !config.limits.cpu_max_set);

    oci_config_free(&config);
    unlink(path);
}

static void test_malformed_json_fails(void) {
    char path[] = "/tmp/minictl-oci-XXXXXX";
    OciConfig config;

    write_config(path, "{ \"root\": { \"path\": \"/x\" ");
    errno = 0;
    assert(oci_load_config(path, &config) == -1);
    assert(errno == EINVAL);
    unlink(path);
}

static void test_trailing_junk_fails(void) {
    char path[] = "/tmp/minictl-oci-XXXXXX";
    OciConfig config;

    write_config(path, "{}  garbage");
    assert(oci_load_config(path, &config) == -1);
    unlink(path);
}

static void test_missing_file_fails(void) {
    OciConfig config;

    errno = 0;
    assert(oci_load_config("/tmp/minictl-oci-does-not-exist-12345", &config) == -1);
}

static void test_invalid_arguments(void) {
    OciConfig config;

    errno = 0;
    assert(oci_load_config(NULL, &config) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(oci_load_config("/tmp/whatever", NULL) == -1);
    assert(errno == EINVAL);
}

int main(void) {
    test_load_full_config();
    test_relative_root_resolves_against_bundle();
    test_partial_config_leaves_unset_fields();
    test_malformed_json_fails();
    test_trailing_junk_fails();
    test_missing_file_fails();
    test_invalid_arguments();

    printf("All oci tests passed.\n");
    return 0;
}
