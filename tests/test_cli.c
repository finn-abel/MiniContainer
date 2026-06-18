#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"

static void assert_parse_ok(int argc, char **argv, MinictlCommand *command) {
    char error[256];

    assert(minictl_cli_parse(argc, argv, command, error, sizeof(error)) == 0);
}

static void assert_parse_error(int argc, char **argv, const char *expected_error) {
    MinictlCommand command;
    char error[256];

    assert(minictl_cli_parse(argc, argv, &command, error, sizeof(error)) == -1);
    assert(strcmp(error, expected_error) == 0);
}

static void test_parse_run_minimal(void) {
    char *argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--", "/bin/sh"};
    MinictlCommand command;

    assert_parse_ok(6, argv, &command);

    assert(command.type == MINICTL_COMMAND_RUN);
    assert(strcmp(command.rootfs, "./rootfs/alpine") == 0);
    assert(command.detach == false);
    assert(command.command_argc == 1);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
}

static void test_parse_run_hostname(void) {
    char *argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--hostname", "demo", "--", "/bin/sh"};
    MinictlCommand command;

    assert_parse_ok(8, argv, &command);

    assert(command.type == MINICTL_COMMAND_RUN);
    assert(strcmp(command.rootfs, "./rootfs/alpine") == 0);
    assert(strcmp(command.hostname, "demo") == 0);
    assert(command.command_argc == 1);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
}

static void test_parse_run_detach_name(void) {
    char *argv[] = {
        "minictl", "run", "--rootfs", "./rootfs/alpine", "--detach", "--name", "demo", "--", "/bin/sh"
    };
    MinictlCommand command;

    assert_parse_ok(9, argv, &command);

    assert(command.type == MINICTL_COMMAND_RUN);
    assert(command.detach == true);
    assert(strcmp(command.name, "demo") == 0);
    assert(command.command_argc == 1);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
}

static void test_parse_run_cgroup_limits(void) {
    char *argv[] = {
        "minictl", "run", "--rootfs", "./rootfs/alpine", "--memory", "128M",
        "--pids", "64", "--cpu", "50000:100000", "--", "/bin/sh"
    };
    MinictlCommand command;

    assert_parse_ok(12, argv, &command);

    assert(command.type == MINICTL_COMMAND_RUN);
    assert(command.cgroup_limits.memory_max_set == true);
    assert(command.cgroup_limits.memory_max == 128ULL * 1024ULL * 1024ULL);
    assert(command.cgroup_limits.pids_max_set == true);
    assert(command.cgroup_limits.pids_max == 64);
    assert(command.cgroup_limits.cpu_max_set == true);
    assert(command.cgroup_limits.cpu_quota == 50000);
    assert(command.cgroup_limits.cpu_period == 100000);
    assert(command.command_argc == 1);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
}

static void test_parse_ps(void) {
    char *argv[] = {"minictl", "ps"};
    MinictlCommand command;

    assert_parse_ok(2, argv, &command);

    assert(command.type == MINICTL_COMMAND_PS);
}

static void test_parse_id_commands(void) {
    char *stop_argv[] = {"minictl", "stop", "abc123"};
    char *logs_argv[] = {"minictl", "logs", "abc123"};
    char *inspect_argv[] = {"minictl", "inspect", "abc123"};
    char *rm_argv[] = {"minictl", "rm", "abc123"};
    MinictlCommand command;

    assert_parse_ok(3, stop_argv, &command);
    assert(command.type == MINICTL_COMMAND_STOP);
    assert(strcmp(command.id, "abc123") == 0);

    assert_parse_ok(3, logs_argv, &command);
    assert(command.type == MINICTL_COMMAND_LOGS);
    assert(strcmp(command.id, "abc123") == 0);

    assert_parse_ok(3, inspect_argv, &command);
    assert(command.type == MINICTL_COMMAND_INSPECT);
    assert(strcmp(command.id, "abc123") == 0);

    assert_parse_ok(3, rm_argv, &command);
    assert(command.type == MINICTL_COMMAND_RM);
    assert(strcmp(command.id, "abc123") == 0);
}

static void test_parse_exec(void) {
    char *argv[] = {"minictl", "exec", "abc123", "--", "/bin/sh"};
    MinictlCommand command;

    assert_parse_ok(5, argv, &command);

    assert(command.type == MINICTL_COMMAND_EXEC);
    assert(strcmp(command.id, "abc123") == 0);
    assert(command.command_argc == 1);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
}

static void test_run_requires_rootfs(void) {
    char *argv[] = {"minictl", "run", "--", "/bin/sh"};

    assert_parse_error(4, argv, "run requires --rootfs");
}

static void test_run_requires_separator(void) {
    char *argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "/bin/sh"};

    assert_parse_error(5, argv, "run requires -- before command");
}

static void test_run_unknown_flag(void) {
    char *argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--bad", "--", "/bin/sh"};

    assert_parse_error(7, argv, "unknown run flag: --bad");
}

static void test_run_missing_option_value(void) {
    char *argv[] = {"minictl", "run", "--rootfs"};

    assert_parse_error(3, argv, "run --rootfs requires a value");
}

static void test_run_invalid_cgroup_flags(void) {
    char *bad_memory_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--memory", "bad", "--", "/bin/sh"};
    char *missing_pids_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--pids", "--", "/bin/sh"};
    char *bad_cpu_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--cpu", "50000", "--", "/bin/sh"};

    assert_parse_error(8, bad_memory_argv, "invalid --memory value");
    assert_parse_error(7, missing_pids_argv, "run --pids requires a value");
    assert_parse_error(8, bad_cpu_argv, "invalid --cpu value");
}

static void test_exec_requires_id_and_separator(void) {
    char *missing_id_argv[] = {"minictl", "exec"};
    char *missing_separator_argv[] = {"minictl", "exec", "abc123", "/bin/sh"};

    assert_parse_error(2, missing_id_argv, "exec requires container ID");
    assert_parse_error(4, missing_separator_argv, "exec requires -- before command");
}

int main(void) {
    test_parse_run_minimal();
    test_parse_run_hostname();
    test_parse_run_detach_name();
    test_parse_run_cgroup_limits();
    test_parse_ps();
    test_parse_id_commands();
    test_parse_exec();
    test_run_requires_rootfs();
    test_run_requires_separator();
    test_run_unknown_flag();
    test_run_missing_option_value();
    test_run_invalid_cgroup_flags();
    test_exec_requires_id_and_separator();

    printf("All CLI tests passed.\n");
    return 0;
}
