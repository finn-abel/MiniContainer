#include "cli.h"

#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int set_error(char *error, size_t error_size, const char *message)
{
    /*
     * Parser helpers return -1 directly so callers can write compact failure paths.
     * A missing error buffer is allowed for tests or callers that only need status.
     */
    if (error != NULL && error_size > 0) {
        if (minictl_str_copy(error, error_size, message) != 0) {
            error[error_size - 1] = '\0';
        }
    }

    return -1;
}

static int set_error_with_value(char *error, size_t error_size, const char *prefix, const char *value)
{
    int written;

    /*
     * Dynamic parser errors still go through one bounded formatting point.
     * Truncation is acceptable here because the command still fails clearly.
     */
    if (error == NULL || error_size == 0) {
        return -1;
    }

    written = snprintf(error, error_size, "%s%s", prefix, value);
    if (written < 0 || (size_t)written >= error_size) {
        error[error_size - 1] = '\0';
    }

    return -1;
}

static bool is_help_command(int argc, char **argv)
{
    /*
     * Treat no arguments as a help command during parsing.
     * main decides that help is a successful user-facing result.
     */
    return argc == 1 || (argc == 2 && strcmp(argv[1], "--help") == 0);
}

static int copy_cli_value(char *dst, size_t dst_size, const char *src, char *error, size_t error_size)
{
    /*
     * All fixed-size fields flow through this wrapper so CLI errors stay stable.
     * The lower-level utility keeps errno details for callers that need them.
     */
    if (minictl_str_copy(dst, dst_size, src) == 0) {
        return 0;
    }

    if (errno == ENAMETOOLONG) {
        return set_error(error, error_size, "value is too long");
    }

    return set_error(error, error_size, "invalid value");
}

static int require_exact_argc(int argc, int expected, char *error, size_t error_size, const char *message)
{
    /*
     * Commands without option parsing should reject extra words immediately.
     * That prevents accidental acceptance of mistyped future flags.
     */
    if (argc == expected) {
        return 0;
    }

    return set_error(error, error_size, message);
}

static int parse_id_command(
    int argc,
    char **argv,
    MinictlCommand *command,
    MinictlCommandType type,
    char *error,
    size_t error_size)
{
    /*
     * stop/logs/inspect/rm all have the same shape at this stage.
     * Keeping that validation shared avoids tiny differences in error behavior.
     */
    if (require_exact_argc(argc, 3, error, error_size, "command requires exactly one container ID") != 0) {
        return -1;
    }

    command->type = type;
    return copy_cli_value(command->id, sizeof(command->id), argv[2], error, error_size);
}

static int parse_run(int argc, char **argv, MinictlCommand *command, char *error, size_t error_size)
{
    bool has_rootfs = false;
    int i = 2;

    command->type = MINICTL_COMMAND_RUN;

    /*
     * Run options are parsed only before "--".
     * Everything after the separator is preserved as the container command.
     */
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            if (i >= argc) {
                return set_error(error, error_size, "run requires a command after --");
            }

            /*
             * Report missing rootfs after seeing "--" so
             * `minictl run -- /bin/sh` gets the most useful error.
             */
            if (!has_rootfs) {
                return set_error(error, error_size, "run requires --rootfs");
            }

            /*
             * Store the original argv slice rather than copying command words.
             * exec-style runtime code can later consume the same vector shape.
             */
            command->command_argc = argc - i;
            command->command_argv = &argv[i];
            return 0;
        }

        if (strcmp(argv[i], "--rootfs") == 0) {
            if (i + 1 >= argc || strcmp(argv[i + 1], "--") == 0) {
                return set_error(error, error_size, "run --rootfs requires a value");
            }

            if (copy_cli_value(command->rootfs, sizeof(command->rootfs), argv[i + 1], error, error_size) != 0) {
                return -1;
            }
            has_rootfs = true;
            i += 2;
            continue;
        }

        if (strcmp(argv[i], "--hostname") == 0) {
            if (i + 1 >= argc || strcmp(argv[i + 1], "--") == 0) {
                return set_error(error, error_size, "run --hostname requires a value");
            }

            if (copy_cli_value(command->hostname, sizeof(command->hostname), argv[i + 1], error, error_size) != 0) {
                return -1;
            }
            i += 2;
            continue;
        }

        if (strcmp(argv[i], "--name") == 0) {
            if (i + 1 >= argc || strcmp(argv[i + 1], "--") == 0) {
                return set_error(error, error_size, "run --name requires a value");
            }

            if (copy_cli_value(command->name, sizeof(command->name), argv[i + 1], error, error_size) != 0) {
                return -1;
            }
            i += 2;
            continue;
        }

        if (strcmp(argv[i], "--detach") == 0) {
            command->detach = true;
            i++;
            continue;
        }

        if (strncmp(argv[i], "--", 2) == 0) {
            return set_error_with_value(error, error_size, "unknown run flag: ", argv[i]);
        }

        /*
         * A non-flag before "--" is almost certainly the intended container command.
         * Fail with the separator rule so the fix is obvious.
         */
        return set_error(error, error_size, "run requires -- before command");
    }

    if (!has_rootfs) {
        return set_error(error, error_size, "run requires --rootfs");
    }

    return set_error(error, error_size, "run requires -- before command");
}

static int parse_exec(int argc, char **argv, MinictlCommand *command, char *error, size_t error_size)
{
    /*
     * exec intentionally accepts no options yet:
     * minictl exec <id> -- <command> [args...]
     */
    if (argc < 3) {
        return set_error(error, error_size, "exec requires container ID");
    }

    if (copy_cli_value(command->id, sizeof(command->id), argv[2], error, error_size) != 0) {
        return -1;
    }

    if (argc < 4 || strcmp(argv[3], "--") != 0) {
        return set_error(error, error_size, "exec requires -- before command");
    }

    if (argc == 4) {
        return set_error(error, error_size, "exec requires a command after --");
    }

    command->type = MINICTL_COMMAND_EXEC;
    command->command_argc = argc - 4;
    command->command_argv = &argv[4];
    return 0;
}

int minictl_cli_parse(int argc, char **argv, MinictlCommand *command, char *error, size_t error_size)
{
    if (command == NULL || argv == NULL || argc < 1) {
        return set_error(error, error_size, "invalid parser input");
    }

    /*
     * Start from a fully zeroed command so optional strings are empty,
     * booleans are false, and failed parses never expose stale fields.
     */
    memset(command, 0, sizeof(*command));

    if (is_help_command(argc, argv)) {
        command->type = MINICTL_COMMAND_HELP;
        return 0;
    }

    if (strcmp(argv[1], "run") == 0) {
        return parse_run(argc, argv, command, error, error_size);
    }

    if (strcmp(argv[1], "ps") == 0) {
        if (require_exact_argc(argc, 2, error, error_size, "ps does not take arguments") != 0) {
            return -1;
        }
        command->type = MINICTL_COMMAND_PS;
        return 0;
    }

    if (strcmp(argv[1], "stop") == 0) {
        return parse_id_command(argc, argv, command, MINICTL_COMMAND_STOP, error, error_size);
    }

    if (strcmp(argv[1], "logs") == 0) {
        return parse_id_command(argc, argv, command, MINICTL_COMMAND_LOGS, error, error_size);
    }

    if (strcmp(argv[1], "inspect") == 0) {
        return parse_id_command(argc, argv, command, MINICTL_COMMAND_INSPECT, error, error_size);
    }

    if (strcmp(argv[1], "rm") == 0) {
        return parse_id_command(argc, argv, command, MINICTL_COMMAND_RM, error, error_size);
    }

    if (strcmp(argv[1], "exec") == 0) {
        return parse_exec(argc, argv, command, error, error_size);
    }

    return set_error_with_value(error, error_size, "unknown command: ", argv[1]);
}

void minictl_cli_print_usage(FILE *stream)
{
    fprintf(stream, "Usage: minictl <command> [options]\n");
}
