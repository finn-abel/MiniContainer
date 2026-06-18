#include "network.h"

#include "config.h"
#include "process.h"
#include "state.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Host-side veth interfaces use the "veth" prefix and the container id; the peer
 * adds one extra character. Both must fit the kernel interface name limit
 * (IFNAMSIZ includes the trailing NUL, so IFNAMSIZ - 1 usable characters).
 */
#define VETH_HOST_PREFIX "veth"
#define VETH_PEER_PREFIX "vethc"

static int build_iface_name(const char *prefix, const char *id, char *out, size_t out_size)
{
    int written;

    if (prefix == NULL || id == NULL || id[0] == '\0' || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    written = snprintf(out, out_size, "%s%s", prefix, id);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /*
     * The kernel rejects interface names at or beyond IFNAMSIZ, so guard the
     * generated name here to fail with a clear error instead of at `ip` time.
     */
    if ((size_t)written >= IFNAMSIZ) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int network_veth_host_name(const char *id, char *out, size_t out_size)
{
    return build_iface_name(VETH_HOST_PREFIX, id, out, out_size);
}

int network_veth_peer_name(const char *id, char *out, size_t out_size)
{
    return build_iface_name(VETH_PEER_PREFIX, id, out, out_size);
}

int network_pick_ip(const bool *used, size_t used_size, char *out, size_t out_size)
{
    int host;

    if (used == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Allocate the lowest free host octet above the reserved gateway. Keeping the
     * search deterministic makes allocation predictable across runs and tests.
     */
    for (host = MINICTL_NETWORK_FIRST_HOST; host <= MINICTL_NETWORK_LAST_HOST; host++) {
        int written;

        if ((size_t)host >= used_size || used[host]) {
            continue;
        }

        written = snprintf(out, out_size, "%s.%d", MINICTL_NETWORK_SUBNET, host);
        if (written < 0 || (size_t)written >= out_size) {
            errno = ENAMETOOLONG;
            return -1;
        }

        return 0;
    }

    errno = EADDRNOTAVAIL;
    return -1;
}

static void mark_used_octet(bool *used, size_t used_size, const char *ip_address)
{
    size_t prefix_len = strlen(MINICTL_NETWORK_SUBNET);
    char *end = NULL;
    long host;

    /*
     * Only addresses on our subnet contribute to allocation. Anything else in
     * metadata (empty, host mode, or a foreign address) is simply ignored.
     */
    if (ip_address == NULL || strncmp(ip_address, MINICTL_NETWORK_SUBNET, prefix_len) != 0) {
        return;
    }
    if (ip_address[prefix_len] != '.') {
        return;
    }

    errno = 0;
    host = strtol(ip_address + prefix_len + 1, &end, 10);
    if (errno != 0 || end == ip_address + prefix_len + 1 || *end != '\0') {
        return;
    }
    if (host >= 0 && (size_t)host < used_size) {
        used[host] = true;
    }
}

int network_allocate_ip(char *out, size_t out_size)
{
    MinictlContainerList list;
    bool used[MINICTL_NETWORK_LAST_HOST + 1];
    size_t i;
    int result;

    memset(used, 0, sizeof(used));
    used[MINICTL_NETWORK_GATEWAY_HOST] = true;

    if (state_list_containers(&list) != 0) {
        return -1;
    }

    for (i = 0; i < list.count; i++) {
        mark_used_octet(used, sizeof(used) / sizeof(used[0]), list.items[i].ip_address);
    }
    state_free_container_list(&list);

    result = network_pick_ip(used, sizeof(used) / sizeof(used[0]), out, out_size);
    return result;
}

static int run_command(char *const argv[], bool quiet)
{
    pid_t pid;
    int exit_code;

    pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        /*
         * Cleanup commands are expected to fail noisily (deleting an interface
         * that already vanished), so their output is silenced. Setup commands
         * keep stderr so the precise `ip` diagnostic reaches the operator.
         */
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) {
                    close(devnull);
                }
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (process_wait(pid, &exit_code) != 0) {
        return -1;
    }

    if (exit_code != 0) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int run_setup_command(char *const argv[], const char *step)
{
    if (run_command(argv, false) != 0) {
        minictl_error("network", step);
        return -1;
    }

    return 0;
}

static bool bridge_exists(void)
{
    char *const argv[] = {"ip", "link", "show", MINICTL_BRIDGE_NAME, NULL};

    /* A zero exit from `ip link show` means the bridge is already present. */
    return run_command(argv, true) == 0;
}

static int ensure_bridge(void)
{
    char bridge_cidr[MINICTL_MAX_ID_SIZE];
    int written;

    if (bridge_exists()) {
        return 0;
    }

    written = snprintf(bridge_cidr, sizeof(bridge_cidr), "%s/%d", MINICTL_NETWORK_GATEWAY, MINICTL_NETWORK_PREFIX_LEN);
    if (written < 0 || (size_t)written >= sizeof(bridge_cidr)) {
        errno = ENAMETOOLONG;
        minictl_error("network", "bridge address too long");
        return -1;
    }

    {
        char *const add_argv[] = {"ip", "link", "add", MINICTL_BRIDGE_NAME, "type", "bridge", NULL};
        char *const addr_argv[] = {"ip", "addr", "add", bridge_cidr, "dev", MINICTL_BRIDGE_NAME, NULL};
        char *const up_argv[] = {"ip", "link", "set", MINICTL_BRIDGE_NAME, "up", NULL};

        if (run_setup_command(add_argv, "create bridge failed") != 0) {
            return -1;
        }
        if (run_setup_command(addr_argv, "assign bridge address failed") != 0) {
            return -1;
        }
        if (run_setup_command(up_argv, "bring bridge up failed") != 0) {
            return -1;
        }
    }

    return 0;
}

static int format_pid(pid_t pid, char *out, size_t out_size)
{
    int written = snprintf(out, out_size, "%ld", (long)pid);

    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int configure_container_side(pid_t child_pid, const char *peer, const char *ip_address)
{
    char pid_str[32];
    char ip_cidr[MINICTL_MAX_ID_SIZE];
    int written;

    if (format_pid(child_pid, pid_str, sizeof(pid_str)) != 0) {
        minictl_error("network", "container pid too long");
        return -1;
    }

    written = snprintf(ip_cidr, sizeof(ip_cidr), "%s/%d", ip_address, MINICTL_NETWORK_PREFIX_LEN);
    if (written < 0 || (size_t)written >= sizeof(ip_cidr)) {
        errno = ENAMETOOLONG;
        minictl_error("network", "container address too long");
        return -1;
    }

    /*
     * Everything inside the container netns is driven from the host via nsenter,
     * so the rootfs does not need an `ip` binary. The peer arrives down, so it is
     * renamed to eth0 first, then loopback and eth0 are addressed and brought up,
     * and finally a default route points at the shared bridge gateway.
     */
    {
        char *const rename_argv[] = {"nsenter", "-t", pid_str, "-n", "ip", "link", "set", (char *)peer, "name", "eth0", NULL};
        char *const lo_argv[] = {"nsenter", "-t", pid_str, "-n", "ip", "link", "set", "lo", "up", NULL};
        char *const addr_argv[] = {"nsenter", "-t", pid_str, "-n", "ip", "addr", "add", ip_cidr, "dev", "eth0", NULL};
        char *const up_argv[] = {"nsenter", "-t", pid_str, "-n", "ip", "link", "set", "eth0", "up", NULL};
        char *const route_argv[] = {"nsenter", "-t", pid_str, "-n", "ip", "route", "add", "default", "via", MINICTL_NETWORK_GATEWAY, NULL};

        if (run_setup_command(rename_argv, "rename container interface failed") != 0) {
            return -1;
        }
        if (run_setup_command(lo_argv, "bring loopback up failed") != 0) {
            return -1;
        }
        if (run_setup_command(addr_argv, "assign container address failed") != 0) {
            return -1;
        }
        if (run_setup_command(up_argv, "bring container interface up failed") != 0) {
            return -1;
        }
        if (run_setup_command(route_argv, "add default route failed") != 0) {
            return -1;
        }
    }

    return 0;
}

int network_setup_bridge(const char *id, pid_t child_pid, const char *ip_address)
{
    char host_name[IFNAMSIZ];
    char peer_name[IFNAMSIZ];
    char pid_str[32];

    if (id == NULL || child_pid <= 0 || ip_address == NULL || ip_address[0] == '\0') {
        errno = EINVAL;
        minictl_error("network", "invalid bridge setup request");
        return -1;
    }

    if (network_veth_host_name(id, host_name, sizeof(host_name)) != 0 ||
        network_veth_peer_name(id, peer_name, sizeof(peer_name)) != 0) {
        minictl_error("network", "container id produces an invalid interface name");
        return -1;
    }

    if (format_pid(child_pid, pid_str, sizeof(pid_str)) != 0) {
        minictl_error("network", "container pid too long");
        return -1;
    }

    if (ensure_bridge() != 0) {
        return -1;
    }

    {
        char *const create_argv[] = {"ip", "link", "add", host_name, "type", "veth", "peer", "name", peer_name, NULL};
        char *const master_argv[] = {"ip", "link", "set", host_name, "master", MINICTL_BRIDGE_NAME, NULL};
        char *const host_up_argv[] = {"ip", "link", "set", host_name, "up", NULL};
        char *const move_argv[] = {"ip", "link", "set", peer_name, "netns", pid_str, NULL};

        if (run_setup_command(create_argv, "create veth pair failed") != 0) {
            return -1;
        }
        if (run_setup_command(master_argv, "attach veth to bridge failed") != 0) {
            network_cleanup(id);
            return -1;
        }
        if (run_setup_command(host_up_argv, "bring host veth up failed") != 0) {
            network_cleanup(id);
            return -1;
        }
        if (run_setup_command(move_argv, "move peer into container netns failed") != 0) {
            network_cleanup(id);
            return -1;
        }
    }

    if (configure_container_side(child_pid, peer_name, ip_address) != 0) {
        network_cleanup(id);
        return -1;
    }

    return 0;
}

int network_setup_none(pid_t child_pid)
{
    char pid_str[32];

    if (child_pid <= 0) {
        errno = EINVAL;
        minictl_error("network", "invalid isolated setup request");
        return -1;
    }

    if (format_pid(child_pid, pid_str, sizeof(pid_str)) != 0) {
        minictl_error("network", "container pid too long");
        return -1;
    }

    {
        char *const lo_argv[] = {"nsenter", "-t", pid_str, "-n", "ip", "link", "set", "lo", "up", NULL};

        return run_setup_command(lo_argv, "bring loopback up failed");
    }
}

void network_cleanup(const char *id)
{
    char host_name[IFNAMSIZ];

    /*
     * Cleanup only runs on the failure path. Once the host end is named, deleting
     * it also removes the peer, so a single best-effort delete is enough.
     */
    if (id == NULL || network_veth_host_name(id, host_name, sizeof(host_name)) != 0) {
        return;
    }

    {
        char *const del_argv[] = {"ip", "link", "del", host_name, NULL};

        run_command(del_argv, true);
    }
}
