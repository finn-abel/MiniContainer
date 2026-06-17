#include "cli.h"
#include "container.h"

#include <stdio.h>

static int dispatch_command(const MinictlCommand *command)
{
    switch (command->type) {
        case MINICTL_COMMAND_RUN:
            return container_run(command);
        case MINICTL_COMMAND_PS:
            return container_list();
        case MINICTL_COMMAND_STOP:
            return container_stop(command);
        case MINICTL_COMMAND_LOGS:
            return container_logs(command);
        case MINICTL_COMMAND_INSPECT:
            return container_inspect(command);
        case MINICTL_COMMAND_RM:
            return container_remove(command);
        case MINICTL_COMMAND_EXEC:
            return container_exec(command);
        case MINICTL_COMMAND_HELP:
            break;
    }

    return 0;
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

    return dispatch_command(&command);
}
