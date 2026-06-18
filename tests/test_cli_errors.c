#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"

static void assert_parse_error(int argc, char **argv, const char *expected_error) {
    MinictlCommand command;
    char error[256];

    assert(minictl_cli_parse(argc, argv, &command, error, sizeof(error)) == -1);
    assert(strcmp(error, expected_error) == 0);
}

static void test_run_without_options_reports_rootfs(void) {
    char *argv[] = {"minictl", "run"};

    assert_parse_error(2, argv, "run requires --rootfs");
}

static void test_run_missing_separator_reports_separator(void) {
    char *argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine"};

    assert_parse_error(4, argv, "run requires -- before command");
}

static void test_run_missing_resource_values(void) {
    char *memory_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--memory"};
    char *pids_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--pids"};
    char *cpu_argv[] = {"minictl", "run", "--rootfs", "./rootfs/alpine", "--cpu"};

    assert_parse_error(5, memory_argv, "run --memory requires a value");
    assert_parse_error(5, pids_argv, "run --pids requires a value");
    assert_parse_error(5, cpu_argv, "run --cpu requires a value");
}

static void test_id_command_rejects_missing_id(void) {
    char *stop_argv[] = {"minictl", "stop"};
    char *logs_argv[] = {"minictl", "logs"};
    char *inspect_argv[] = {"minictl", "inspect"};
    char *rm_argv[] = {"minictl", "rm"};

    assert_parse_error(2, stop_argv, "command requires exactly one container ID");
    assert_parse_error(2, logs_argv, "command requires exactly one container ID");
    assert_parse_error(2, inspect_argv, "command requires exactly one container ID");
    assert_parse_error(2, rm_argv, "command requires exactly one container ID");
}

static void test_unknown_command(void) {
    char *argv[] = {"minictl", "nope"};

    assert_parse_error(2, argv, "unknown command: nope");
}

int main(void) {
    test_run_without_options_reports_rootfs();
    test_run_missing_separator_reports_separator();
    test_run_missing_resource_values();
    test_id_command_rejects_missing_id();
    test_unknown_command();

    printf("All cli_errors tests passed.\n");
    return 0;
}
