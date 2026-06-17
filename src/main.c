#include "cli.h"

#include <stdio.h>

static const char *not_implemented_message(MinictlCommandType type)
{
    switch (type) {
        case MINICTL_COMMAND_RUN:
            return "run not implemented yet.";
        case MINICTL_COMMAND_PS:
            return "ps not implemented yet.";
        case MINICTL_COMMAND_STOP:
            return "stop not implemented yet.";
        case MINICTL_COMMAND_LOGS:
            return "logs not implemented yet.";
        case MINICTL_COMMAND_INSPECT:
            return "inspect not implemented yet.";
        case MINICTL_COMMAND_RM:
            return "rm not implemented yet.";
        case MINICTL_COMMAND_EXEC:
            return "exec not implemented yet.";
        case MINICTL_COMMAND_HELP:
            break;
    }

    return "command not implemented yet.";
}

int main(int argc, char **argv)
{
    MinictlCommand command;
    char error[256];

    /*
     * Main owns user-facing parse errors for now.
     * Runtime command handlers will replace these temporary dispatch messages.
     */
    if (minictl_cli_parse(argc, argv, &command, error, sizeof(error)) != 0) {
        fprintf(stderr, "minictl: %s\n", error);
        return 1;
    }

    if (command.type == MINICTL_COMMAND_HELP) {
        minictl_cli_print_usage(stdout);
        return 0;
    }

    printf("%s\n", not_implemented_message(command.type));
    return 0;
}
