#ifndef MINICTL_CLI_H
#define MINICTL_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cgroups.h"
#include "config.h"

/*
 * Final command kinds accepted by the minictl CLI.
 * Runtime modules will dispatch from this parsed shape in later steps.
 */
typedef enum MinictlCommandType {
    MINICTL_COMMAND_HELP = 0,
    MINICTL_COMMAND_RUN,
    MINICTL_COMMAND_PS,
    MINICTL_COMMAND_STOP,
    MINICTL_COMMAND_LOGS,
    MINICTL_COMMAND_INSPECT,
    MINICTL_COMMAND_RM,
    MINICTL_COMMAND_EXEC
} MinictlCommandType;

/*
 * Parsed command-line data for run and exec commands.
 * Argument vectors point into the original argv array and are not owned here.
 * Resource limits are optional and only applied when their set flags are true.
 */
typedef struct MinictlCommand {
    MinictlCommandType type;
    char id[MINICTL_MAX_ID_SIZE];
    char rootfs[MINICTL_MAX_PATH_SIZE];
    char hostname[MINICTL_MAX_ID_SIZE];
    char name[MINICTL_MAX_ID_SIZE];
    bool detach;
    CgroupLimits cgroup_limits;
    int command_argc;
    char **command_argv;
} MinictlCommand;

/*
 * Parse argv into a MinictlCommand or write a user-facing error message.
 * The parser validates command shape only; it does not touch runtime state.
 */
int minictl_cli_parse(int argc, char **argv, MinictlCommand *command, char *error, size_t error_size);

/*
 * Print the short usage line shared by help, parser failures, and main.
 * Keep the first line stable because tests and README examples rely on it.
 */
void minictl_cli_print_usage(FILE *stream);

#endif
