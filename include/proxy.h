#ifndef MINICTL_PROXY_H
#define MINICTL_PROXY_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Userspace TCP port publishing for --publish HOST:CONTAINER.
 *
 * Because Step 1 bridge networking puts the host on the same subnet as the
 * container, a host-side process can reach the container IP as an ordinary TCP
 * client. The proxy listens on each published host port and forwards accepted
 * connections to the container's IP and port. This avoids NAT, ip_forward, and
 * any global firewall state. Only TCP is supported.
 */

/*
 * One published port mapping: a host-side listen port forwarded to a container
 * port. Both ports are validated to the inclusive TCP port range.
 */
typedef struct PublishSpec {
    int host_port;
    int container_port;
} PublishSpec;

/*
 * Parse a "HOST:CONTAINER" mapping such as "8080:80" into a PublishSpec.
 * Both sides must be fully numeric and within the valid port range; anything
 * else fails with errno EINVAL. This function performs no I/O and is unit tested.
 */
int proxy_parse_publish(const char *value, PublishSpec *out);

/*
 * Render published mappings as a compact "8080:80,9090:90" string for metadata
 * and inspect output. Fails with ENAMETOOLONG when the destination is too small.
 */
int proxy_publishes_to_string(const PublishSpec *specs, size_t count, char *out, size_t out_size);

/*
 * Start a per-container proxy process that binds every published host port and
 * forwards connections to container_ip. The proxy runs in the caller's (host)
 * network namespace and becomes its own process group leader so it and its
 * per-connection children can be torn down with one group signal. Returns the
 * proxy PID through proxy_pid. Returns -1 (errno preserved) if the proxy fails
 * to bind any port; no proxy remains in that case.
 */
int proxy_start(const char *container_ip, const PublishSpec *specs, size_t count, pid_t *proxy_pid);

/*
 * Stop a running proxy and its in-flight connection children by signalling its
 * process group. Safe to call with a non-positive pid (no-op). Best effort.
 */
void proxy_stop(pid_t proxy_pid);

#endif
