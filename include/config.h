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

/*
 * State store filenames and directory names.
 * These constants define the durable on-disk layout for container metadata.
 */
#define MINICTL_CONTAINERS_DIR "containers"
#define MINICTL_META_FILE "meta"
#define MINICTL_STDOUT_LOG_FILE "stdout.log"
#define MINICTL_STDERR_LOG_FILE "stderr.log"

/*
 * Bridge networking layout for the optional --network bridge mode.
 * Containers share one host bridge and receive an address on a fixed /24.
 * The gateway octet is reserved for the bridge itself, so container hosts
 * are allocated from MINICTL_NETWORK_FIRST_HOST upward.
 */
#define MINICTL_BRIDGE_NAME "minictl0"
#define MINICTL_NETWORK_SUBNET "10.0.0"
#define MINICTL_NETWORK_GATEWAY "10.0.0.1"
#define MINICTL_NETWORK_PREFIX_LEN 24
#define MINICTL_NETWORK_GATEWAY_HOST 1
#define MINICTL_NETWORK_FIRST_HOST 2
#define MINICTL_NETWORK_LAST_HOST 254

/*
 * Network mode strings recorded in metadata and accepted by --network.
 * "host" shares the host network namespace (the default, pre-network behavior).
 */
#define MINICTL_NETWORK_MODE_HOST "host"
#define MINICTL_NETWORK_MODE_BRIDGE "bridge"
#define MINICTL_NETWORK_MODE_NONE "none"

/*
 * Port publishing limits for --publish HOST:CONTAINER.
 * The published set is small and fixed per container; ports are TCP only.
 */
#define MINICTL_MAX_PUBLISH 16
#define MINICTL_MIN_PORT 1
#define MINICTL_MAX_PORT 65535

#endif
