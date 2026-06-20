# MiniContainer-C

MiniContainer-C is a small educational Linux container runtime written in C11.

The `minictl` binary stores line-based container metadata, implements
state-backed lifecycle commands, and runs commands inside Linux namespaces with
a supplied rootfs mounted as `/`. Containers get an OverlayFS writable layer by
default so the base rootfs is not modified; `--no-overlay` keeps the older
direct bind-mount behavior.

```text
Usage: minictl <command> [options]
```

Supported parse shapes:

```sh
./minictl run --rootfs ./rootfs/alpine -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --hostname demo -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --detach --name demo -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --memory 128M --pids 64 --cpu 50000:100000 -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --network bridge -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --publish 8080:80 --detach -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --no-overlay -- /bin/sh
./minictl run --oci-config ./bundle/config.json
./minictl run --oci-config ./bundle/config.json --hostname demo -- /bin/sh
./minictl ps
./minictl stop <id>
./minictl logs <id>
./minictl inspect <id>
./minictl rm <id>
./minictl exec <id> -- /bin/sh
```

`run` uses `clone` with UTS, PID, mount, and IPC namespaces, validates the
effective rootfs, makes mount propagation private, mounts an OverlayFS merged
root by default, uses `pivot_root` so the command sees that rootfs as `/`,
mounts `/proc`, sets the requested hostname, and executes the requested command.
Cgroups are not applied unless resource flags are provided. Run it inside a
disposable Linux VM.

Rootfs modes:

```sh
# default: writable per-container OverlayFS layer under the container state dir
./minictl run --rootfs ./rootfs/alpine -- /bin/sh

# direct mode: bind mount the supplied rootfs itself; writes can modify it
./minictl run --rootfs ./rootfs/alpine --no-overlay -- /bin/sh
```

Overlay mode requires kernel OverlayFS support. If unavailable, `minictl`
reports the overlay error and suggests `--no-overlay`.

Optional minimal OCI runtime config support:

```sh
./minictl run --oci-config ./bundle/config.json
./minictl run --oci-config ./bundle/config.json -- /bin/sh
```

`--oci-config` reads a small subset of `config.json`: `root.path`,
`process.args`, `process.env`, `hostname`, and `linux.resources` for
memory/pids/cpu limits. Relative `root.path` values are resolved against the
config file directory. Explicit CLI flags and command arguments override OCI
values. Mounts, capabilities, seccomp, hooks, and OCI namespace settings are not
mapped yet.

Optional cgroup v2 resource flags:

```sh
--memory 128M
--pids 64
--cpu 50000:100000
```

When any resource flag is provided, `minictl` requires cgroup v2 at
`/sys/fs/cgroup`, creates `/sys/fs/cgroup/minictl/<id>/`, writes the requested
limits, and moves the container init process into that cgroup. Cgroup setup
errors are reported directly instead of being ignored.

Optional networking with `--network <mode>`:

```sh
--network host     # default: share the host network namespace
--network bridge   # own netns connected to the shared minictl0 bridge
--network none     # isolated netns with only loopback up
```

With `--network bridge`, `minictl` adds `CLONE_NEWNET`, ensures a shared host
bridge `minictl0` (gateway `10.0.0.1/24`), allocates the lowest free
`10.0.0.0/24` address, creates a veth pair, attaches the host end to the bridge,
moves the peer into the container as `eth0`, and adds a default route via the
gateway. Setup runs from the host (via `ip`/`nsenter`), so the rootfs does not
need an `ip` binary. This reaches the host and sibling containers; there is no
NAT yet, so the external internet is not reachable. Requires root and an
existing `ip`/`nsenter` (iproute2 + util-linux) on the host.

Optional port publishing with `--publish HOST:CONTAINER` (repeatable):

```sh
sudo ./minictl run --rootfs ./rootfs/alpine --publish 8080:80 --detach --name web \
  -- /bin/sh -c "httpd -f -p 80"
curl localhost:8080
```

`--publish` makes a container TCP service reachable on a host port. Because the
host already sits on the `minictl0` bridge, `minictl` forwards traffic with a
small userspace TCP proxy that listens on the host port and dials the
container's bridge IP directly — no NAT, `ip_forward`, or firewall rules. It
therefore requires bridge networking: `--publish` implies `--network bridge`
when no mode is given, and pairing it with `--network host`/`none` is rejected.
The proxy is TCP only, binds `0.0.0.0` on the host, runs as a per-container
process tracked in metadata, and is torn down when the container exits or on
`rm`.

Foreground runs mirror stdout/stderr to the terminal and also save them to
`stdout.log` and `stderr.log`. Detached runs write directly to those log files.
Detached runs use a small monitor process, not a daemon, to wait for the
container init process and update final state after the CLI exits.

`stop` sends SIGTERM to the recorded init PID, waits briefly, then sends SIGKILL
if needed. It does not write metadata itself: the owning run or monitor process
records the final status and exit code when it reaps the container, and a later
`ps`/`inspect` reconciles orphaned state. Logs are preserved. Note the container
init is PID 1 of its namespace, so a command that installs no SIGTERM handler is
ended by the SIGKILL fallback.

`exec` joins the recorded running container's mount, UTS, IPC, and PID
namespaces with `setns` (and the network namespace too for bridge/none-mode
containers), enters the container root through `/proc/<pid>/root`, forks so the
command is created inside the target PID namespace, and executes the requested
command. v1 exec intentionally does not add complex TTY handling.

`inspect` refreshes the recorded PID status before printing metadata, then shows
the ID, name, PID, status, rootfs, rootfs mode, hostname, command, timestamps,
exit code, cgroup path, network mode, IP address, published ports, and durable
stdout/stderr log paths.

Error handling is intentionally explicit. Runtime failures use:

```text
minictl: <operation>: <specific errno message>
```

Container state writes use a temporary metadata file and atomic rename where
possible, and failed setup paths clean up partial state, cgroups, overlay
directories, proxies, and rootfs mounts before returning.

The default state directory is:

```text
/var/lib/minictl
```

For local non-root development, override it with:

```sh
MINICTL_STATE_DIR=./.minictl-state ./minictl ps
```

Foreground run example:

```sh
sudo MINICTL_STATE_DIR=./.minictl-state ./minictl run --rootfs / --hostname demo -- /bin/hostname
MINICTL_STATE_DIR=./.minictl-state ./minictl ps
MINICTL_STATE_DIR=./.minictl-state ./minictl inspect <id>
MINICTL_STATE_DIR=./.minictl-state ./minictl logs <id>
sudo MINICTL_STATE_DIR=./.minictl-state ./minictl exec <id> -- /bin/hostname
sudo MINICTL_STATE_DIR=./.minictl-state ./minictl stop <id>
```

## Build

Build and runtime support are Linux-focused. Normal tests are non-privileged,
but actual container runs require Linux namespace support and usually root.
Bridge networking additionally needs host `ip` and `nsenter`.

```sh
make
./minictl --help
make test
```

`make test` runs non-privileged tests only. It does not require a rootfs, sudo,
or writes to `/var/lib/minictl`.

Optional privileged integration test:

```sh
sudo ROOTFS=./rootfs/alpine make integration
```

The integration target requires Linux, root privileges, cgroup/namespace
support, and a valid rootfs. It starts a small detached container, checks
hostname exec, rootfs visibility, `/proc`, logs, stop, and rm. When host
`ip`/`nsenter` are available it also checks bridge networking and gateway
reachability; when `curl` and a rootfs `httpd` are available it checks published
TCP ports and proxy cleanup.

## Safety

This project is for learning container mechanics in a trusted Linux VM. It is
not a production isolation tool.
