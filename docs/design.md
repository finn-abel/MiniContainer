# Design

MiniContainer-C grows the runtime in layers so each step keeps compiling and
keeps the parent lifecycle visible.

## Namespace Skeleton

`container_run` owns metadata and parent lifecycle:

- create a state record
- create a cgroup when resource limits were requested
- clone a child
- attach the child PID to the cgroup when limits are enabled
- save the child PID and running status
- wait in foreground mode
- save final exit status

`namespaces.c` owns the current child setup:

- `clone`
- `setns` for exec
- `CLONE_NEWUTS`
- `CLONE_NEWPID`
- `CLONE_NEWNS`
- `CLONE_NEWIPC`
- `CLONE_NEWNET` (only when `--network bridge`/`none` is requested)
- hostname setup
- rootfs mount preparation
- `pivot_root`
- `/proc` mounting
- command execution

With `CLONE_NEWPID`, the cloned child becomes PID 1 inside the new PID
namespace.

## Exec

`container_exec` loads metadata, refreshes stale running status, refuses
non-running containers, and validates absolute command paths against the stored
rootfs before entering namespaces.

`namespaces_exec_in_container` opens all required handles before calling
`setns`:

- `/proc/<pid>/ns/mnt`
- `/proc/<pid>/ns/uts`
- `/proc/<pid>/ns/ipc`
- `/proc/<pid>/ns/pid`
- `/proc/<pid>/ns/net` (only for bridge/none-mode containers)
- `/proc/<pid>/root`

Joining the PID namespace only affects future children, so exec forks after
`setns(CLONE_NEWPID)`. The command child is therefore created inside the target
PID namespace. Joining a mount namespace does not change the process root, so
`rootfs_enter_from_fd` makes the opened `/proc/<pid>/root` directory become `/`
before the fork/exec path continues.

## Cgroups

`cli.c` parses optional cgroup v2 resource flags into typed limits:

- `--memory 128M`
- `--pids 64`
- `--cpu 50000:100000`

`cgroups.c` owns all cgroup v2 details:

- verify `/sys/fs/cgroup` is cgroup v2
- create `/sys/fs/cgroup/minictl/<id>/`
- write `memory.max`, `pids.max`, and `cpu.max`
- write the container init PID to `cgroup.procs`
- remove the leaf cgroup during `rm`

If no resource flags are provided, `container_run` skips cgroup filesystem
operations. If any flag is provided, cgroup setup and attachment errors are
reported directly and the partial container state is cleaned up.

## Networking

Networking is opt-in through `--network <mode>`:

- `host` (default) shares the host network namespace, matching the original
  pre-network behavior. No extra namespace and no setup.
- `bridge` gives the container its own network namespace connected to a shared
  host bridge.
- `none` gives an isolated network namespace with only loopback brought up.

`cli.c` validates the mode. When the mode is `bridge` or `none`,
`namespaces_clone_child` adds `CLONE_NEWNET` to the clone flags.

`network.c` owns all connectivity details and shells out to `ip`/`nsenter`,
checking every exit status:

- ensure the shared bridge `minictl0` exists and is up with the gateway address
  `10.0.0.1/24` (idempotent across runs)
- allocate the lowest free host address on `10.0.0.0/24`, reserving `.1` for the
  gateway, by scanning existing container metadata
- create a veth pair (`veth<id>` on the host, `vethc<id>` as the peer), attach
  the host end to the bridge, and bring it up
- move the peer into the container network namespace by PID
- inside that namespace, rename the peer to `eth0`, bring up `lo` and `eth0`,
  assign the allocated address, and add a default route via the gateway

The setup runs in the parent after `clone` (the peer move needs the child PID)
and before the start barrier is released, so `eth0` exists before the container
command execs. Because all configuration is driven from the host via `nsenter`,
the rootfs does not need an `ip` binary.

This step provides container-to-host and container-to-container reachability on
the bridge subnet. There is **no NAT and no IP forwarding yet**, so containers
cannot reach the external internet; that is a planned follow-up. A container's
veth pair is destroyed automatically when its network namespace is torn down at
exit, so `network_cleanup` only runs on the early-failure path before the
namespace exists.

The allocated address and network mode are recorded in container metadata and
shown by `inspect`. Address allocation has a benign time-of-check/time-of-use
race under concurrent `run` invocations, which is acceptable for this
educational runtime.

## Port publishing

`--publish HOST:CONTAINER` (repeatable) exposes a container TCP service on a host
port. It is implemented as a userspace TCP proxy in `proxy.c` rather than
iptables, because the host is already on the `minictl0` bridge and can dial the
container IP directly — so forwarding needs **no NAT, no `ip_forward`, and no
firewall state**, which keeps it consistent with the no-NAT networking step.

Publishing requires a container IP, so it requires bridge networking:

- `cli.c` parses each `--publish` value via `proxy_parse_publish`, rejects bad
  mappings and out-of-range ports, and errors if `--publish` is combined with an
  explicit `--network host`/`none`.
- `container.c` implies `--network bridge` when `--publish` is given without a
  mode, and serializes the mappings into metadata.

Lifecycle (mirrors the detached-monitor and start-barrier patterns):

- After the container is marked running, the host-side run/monitor process forks
  one proxy process (`proxy_start`). The proxy becomes its own process group
  leader, sets `SIGCHLD` to `SIG_IGN` so per-connection children auto-reap,
  closes inherited descriptors (so it never holds the log pipe open), binds every
  published host port on `0.0.0.0`, and reports bind success/failure back over a
  readiness pipe. A bind failure tears the container down and fails the run.
- For each accepted connection the proxy forks a child that dials
  `<container_ip>:<container_port>` and pumps bytes both ways with `poll` until
  both halves close. A backend that is not listening yet just drops the
  connection.
- The proxy PID is stored in metadata. When the container init exits, its owner
  signals the proxy's process group and reaps it (`shutdown_owned_proxy`). `rm`
  best-effort stops any lingering proxy (`proxy_stop`) so published host ports
  are always freed.

Only TCP is supported; UDP and iptables-based forwarding are out of scope.

## Inspect

`inspect` loads one container, refreshes stale running status from the recorded
PID, saves metadata when that status changes, then prints the full debugging
view: identity, PID, status, rootfs, hostname, command, timestamps, exit code,
cgroup path, network mode, IP address, published ports, and durable
stdout/stderr log paths.
