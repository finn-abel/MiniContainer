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

static void test_parse_run_network(void) {
    char *argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--network", "bridge", "--", "/bin/sh"};
    MinictlCommand command;

    assert_parse_ok(8, argv, &command);

    assert(command.type == MINICTL_COMMAND_RUN);
    assert(strcmp(command.network_mode, "bridge") == 0);
    assert(command.command_argc == 1);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
}

static void test_run_invalid_network(void) {
    char *bad_value_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--network", "wifi", "--", "/bin/sh"};
    char *missing_value_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--network", "--", "/bin/sh"};

    assert_parse_error(8, bad_value_argv, "invalid --network value");
    assert_parse_error(7, missing_value_argv, "run --network requires a value");
}

static void test_parse_run_publish(void) {
    char *argv[] = {
        "minictl", "run", "--rootfs", "./rootfs/alpine", "--publish", "8080:80", "--publish", "9090:90", "--", "/bin/sh"
    };
    MinictlCommand command;

    assert_parse_ok(10, argv, &command);

    assert(command.type == MINICTL_COMMAND_RUN);
    assert(command.publish_count == 2);
    assert(command.publishes[0].host_port == 8080);
    assert(command.publishes[0].container_port == 80);
    assert(command.publishes[1].host_port == 9090);
    assert(command.publishes[1].container_port == 90);
}

static void test_publish_requires_bridge_network(void) {
    char *host_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--network", "host", "--publish", "8080:80", "--", "/bin/sh"};
    char *bridge_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--network", "bridge", "--publish", "8080:80", "--", "/bin/sh"};
    MinictlCommand command;

    assert_parse_error(10, host_argv, "publish requires bridge networking");

    /* Explicit bridge is allowed. */
    assert_parse_ok(10, bridge_argv, &command);
    assert(command.publish_count == 1);
}

static void test_publish_invalid_value(void) {
    char *bad_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--publish", "notaport", "--", "/bin/sh"};
    char *missing_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--publish", "--", "/bin/sh"};

    assert_parse_error(8, bad_argv, "invalid --publish value");
    assert_parse_error(7, missing_argv, "run --publish requires a value");
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

static void test_parse_help(void) {
    char *no_args[] = {"minictl"};
    char *help_flag[] = {"minictl", "--help"};
    MinictlCommand command;

    assert_parse_ok(1, no_args, &command);
    assert(command.type == MINICTL_COMMAND_HELP);

    assert_parse_ok(2, help_flag, &command);
    assert(command.type == MINICTL_COMMAND_HELP);
}

static void test_run_rejects_flaglike_values(void) {
    char *rootfs_argv[] = {"minictl", "run", "--rootfs", "--hostname", "demo", "--", "/bin/sh"};
    char *hostname_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--hostname", "--name", "x", "--", "/bin/sh"};
    char *memory_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--memory", "--pids", "64", "--", "/bin/sh"};

    assert_parse_error(7, rootfs_argv, "run --rootfs requires a value");
    assert_parse_error(9, hostname_argv, "run --hostname requires a value");
    assert_parse_error(9, memory_argv, "run --memory requires a value");
}

static void test_parse_exec_multiarg(void) {
    char *argv[] = {"minictl", "exec", "abc123", "--", "/bin/sh", "-c", "echo hi"};
    MinictlCommand command;

    assert_parse_ok(7, argv, &command);

    assert(command.type == MINICTL_COMMAND_EXEC);
    assert(command.command_argc == 3);
    assert(strcmp(command.command_argv[0], "/bin/sh") == 0);
    assert(strcmp(command.command_argv[2], "echo hi") == 0);
}

static void test_exec_requires_command_after_separator(void) {
    char *argv[] = {"minictl", "exec", "abc123", "--"};

    assert_parse_error(4, argv, "exec requires a command after --");
}

int main(void) {
    test_parse_run_minimal();
    test_parse_run_hostname();
    test_parse_run_detach_name();
    test_parse_run_network();
    test_run_invalid_network();
    test_parse_run_publish();
    test_publish_requires_bridge_network();
    test_publish_invalid_value();
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
    test_parse_help();
    test_run_rejects_flaglike_values();
    test_parse_exec_multiarg();
    test_exec_requires_command_after_separator();

    printf("All CLI tests passed.\n");
    return 0;
}
