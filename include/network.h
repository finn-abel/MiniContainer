#ifndef MINICTL_NETWORK_H
#define MINICTL_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Bridge networking for the optional --network bridge mode.
 *
 * The model is a single shared host bridge (minictl0) acting as the gateway on
 * MINICTL_NETWORK_SUBNET. Each bridge container gets a veth pair: the host end
 * is attached to the bridge, and the peer is moved into the container's network
 * namespace, renamed eth0, addressed, and given a default route to the gateway.
 * There is no NAT in this step, so containers reach the host and each other but
 * not the external internet. All work shells out to `ip`/`nsenter` and reports
 * the failing step; no syscall or command failure is hidden.
 */

/*
 * Choose the lowest free host address on the subnet given which host octets are
 * already in use. The used array is indexed by octet value and must have room
 * for MINICTL_NETWORK_LAST_HOST + 1 entries. Writes a dotted-quad string such
 * as "10.0.0.2" to out. Returns -1 with errno EADDRNOTAVAIL when the usable
 * range is exhausted. This function performs no I/O and is unit tested directly.
 */
int network_pick_ip(const bool *used, size_t used_size, char *out, size_t out_size);

/*
 * Allocate an IP for a new bridge container by scanning existing container
 * metadata for addresses already in use, then picking the lowest free host.
 * Returns -1 with errno EADDRNOTAVAIL when the subnet is full.
 */
int network_allocate_ip(char *out, size_t out_size);

/*
 * Build the host-side and peer veth interface names for a container id.
 * Both names are bounded by the kernel interface name limit; an id long enough
 * to overflow that limit fails with errno ENAMETOOLONG.
 */
int network_veth_host_name(const char *id, char *out, size_t out_size);
int network_veth_peer_name(const char *id, char *out, size_t out_size);

/*
 * Set up bridge connectivity for a freshly cloned container init.
 * Ensures the shared bridge exists, creates and attaches the veth pair, moves
 * the peer into child_pid's network namespace, and configures eth0 plus a
 * default route inside that namespace. Returns -1 after printing the failing
 * step. Must be called after clone and before the child is released to exec.
 */
int network_setup_bridge(const char *id, pid_t child_pid, const char *ip_address);

/*
 * Bring up loopback inside an isolated (--network none) container namespace.
 * Returns -1 after printing the failing step.
 */
int network_setup_none(pid_t child_pid);

/*
 * Best-effort teardown of a container's host-side veth. The peer disappears
 * with the container's network namespace when its init exits, so this only
 * matters when setup or startup failed before the namespace was torn down.
 * Missing interfaces are ignored.
 */
void network_cleanup(const char *id);

#endif
