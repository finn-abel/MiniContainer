#include "cli.h"
#include "state.h"
#include "util.h"

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

static int handle_ps(void)
{
    MinictlContainerList list;
    size_t i;

    if (state_list_containers(&list) != 0) {
        minictl_perror("state");
        return 1;
    }

    if (list.count == 0) {
        printf("No containers found.\n");
        state_free_container_list(&list);
        return 0;
    }

    printf("ID\tSTATUS\tPID\tNAME\tCOMMAND\n");
    for (i = 0; i < list.count; i++) {
        printf("%s\t%s\t%ld\t%s\t%s\n",
               list.items[i].id,
               list.items[i].status,
               (long)list.items[i].pid,
               list.items[i].name,
               list.items[i].command);
    }

    state_free_container_list(&list);
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

    if (command.type == MINICTL_COMMAND_PS) {
        return handle_ps();
    }

    printf("%s\n", not_implemented_message(command.type));
    return 0;
}
