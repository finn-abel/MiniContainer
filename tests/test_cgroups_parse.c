#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include "cgroups.h"

static void test_parse_memory_bytes(void) {
    unsigned long long bytes;

    assert(cgroup_parse_memory("4096", &bytes) == 0);
    assert(bytes == 4096ULL);
}

static void test_parse_memory_suffixes(void) {
    unsigned long long bytes;

    assert(cgroup_parse_memory("512K", &bytes) == 0);
    assert(bytes == 512ULL * 1024ULL);

    assert(cgroup_parse_memory("128M", &bytes) == 0);
    assert(bytes == 128ULL * 1024ULL * 1024ULL);

    assert(cgroup_parse_memory("1G", &bytes) == 0);
    assert(bytes == 1024ULL * 1024ULL * 1024ULL);
}

static void test_parse_memory_rejects_invalid_values(void) {
    unsigned long long bytes;

    assert(cgroup_parse_memory("", &bytes) == -1);
    assert(errno == EINVAL);

    assert(cgroup_parse_memory("0", &bytes) == -1);
    assert(errno == EINVAL);

    assert(cgroup_parse_memory("12MB", &bytes) == -1);
    assert(errno == EINVAL);

    assert(cgroup_parse_memory("-1", &bytes) == -1);
    assert(errno == EINVAL);
}

static void test_parse_cpu_pair(void) {
    long quota;
    long period;

    assert(cgroup_parse_cpu("50000:100000", &quota, &period) == 0);
    assert(quota == 50000);
    assert(period == 100000);
}

static void test_parse_cpu_rejects_invalid_values(void) {
    long quota;
    long period;

    assert(cgroup_parse_cpu("50000", &quota, &period) == -1);
    assert(errno == EINVAL);

    assert(cgroup_parse_cpu("0:100000", &quota, &period) == -1);
    assert(errno == EINVAL);

    assert(cgroup_parse_cpu("50000:bad", &quota, &period) == -1);
    assert(errno == EINVAL);
}

static void test_limits_empty(void) {
    CgroupLimits limits = {0};

    assert(cgroup_limits_empty(&limits) == true);

    limits.pids_max_set = true;
    limits.pids_max = 64;

    assert(cgroup_limits_empty(&limits) == false);
}

static void test_limits_empty_handles_null(void) {
    assert(cgroup_limits_empty(NULL) == true);
}

static void test_parse_memory_more_cases(void) {
    unsigned long long bytes;

    assert(cgroup_parse_memory("2g", &bytes) == 0);
    assert(bytes == 2ULL * 1024ULL * 1024ULL * 1024ULL);

    errno = 0;
    assert(cgroup_parse_memory("10X", &bytes) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(cgroup_parse_memory("M", &bytes) == -1);
    assert(errno == EINVAL);
}

static void test_parse_cpu_more_invalid(void) {
    long quota;
    long period;

    errno = 0;
    assert(cgroup_parse_cpu(":100000", &quota, &period) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(cgroup_parse_cpu("50000:", &quota, &period) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(cgroup_parse_cpu("50000:100000:1", &quota, &period) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(cgroup_parse_cpu("-5:100000", &quota, &period) == -1);
    assert(errno == EINVAL);
}

static void test_add_pid_and_remove_validation(void) {
    errno = 0;
    assert(cgroup_add_pid(NULL, 1) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(cgroup_add_pid("/sys/fs/cgroup/minictl/x", 0) == -1);
    assert(errno == EINVAL);

    /* Empty or NULL paths are no-ops: unconstrained containers have no cgroup. */
    assert(cgroup_remove(NULL) == 0);
    assert(cgroup_remove("") == 0);
}

int main(void) {
    test_parse_memory_bytes();
    test_parse_memory_suffixes();
    test_parse_memory_rejects_invalid_values();
    test_parse_cpu_pair();
    test_parse_cpu_rejects_invalid_values();
    test_limits_empty();
    test_limits_empty_handles_null();
    test_parse_memory_more_cases();
    test_parse_cpu_more_invalid();
    test_add_pid_and_remove_validation();

    printf("All cgroups_parse tests passed.\n");
    return 0;
}
