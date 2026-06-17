#ifndef MINICTL_CONFIG_H
#define MINICTL_CONFIG_H

/*
 * Project-wide constants used by multiple modules.
 * Keep these values centralized so later runtime code shares one limit policy.
 */
#define MINICTL_DEFAULT_STATE_DIR "/var/lib/minictl"

#define MINICTL_MAX_PATH_SIZE 4096
#define MINICTL_MAX_COMMAND_SIZE 4096
#define MINICTL_MAX_ID_SIZE 64
#define MINICTL_CHILD_STACK_SIZE (1024 * 1024)

#endif
