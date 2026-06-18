# MiniContainer-C

MiniContainer-C is a small educational Linux container runtime written in C11.

This repository is in an early rootfs isolation stage. The `minictl` binary
currently accepts the final command shapes, stores line-based container
metadata, implements state-backed `ps`, `inspect`, and `rm`, and runs commands
inside Linux namespaces with the supplied rootfs mounted as `/`.

```text
Usage: minictl <command> [options]
```

Supported parse shapes:

```sh
./minictl run --rootfs ./rootfs/alpine -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --hostname demo -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --detach --name demo -- /bin/sh
./minictl run --rootfs ./rootfs/alpine --memory 128M --pids 64 --cpu 50000:100000 -- /bin/sh
./minictl ps
./minictl stop <id>
./minictl logs <id>
./minictl inspect <id>
./minictl rm <id>
./minictl exec <id> -- /bin/sh
```

`run` uses `clone` with UTS, PID, mount, and IPC namespaces, validates the
supplied rootfs, makes mount propagation private, bind mounts the rootfs, uses
`pivot_root` so the command sees that rootfs as `/`, mounts `/proc`, sets the
requested hostname, and executes the requested command. Cgroups are not applied
unless resource flags are provided. Run it inside a disposable Linux VM.

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

Foreground runs mirror stdout/stderr to the terminal and also save them to
`stdout.log` and `stderr.log`. Detached runs write directly to those log files.
Detached runs use a small monitor process, not a daemon, to wait for the
container init process and update final state after the CLI exits.

`stop` sends SIGTERM to the recorded init PID, waits briefly, then sends SIGKILL
if needed. It updates metadata and preserves logs.

`inspect` refreshes the recorded PID status before printing metadata, then shows
the ID, name, PID, status, rootfs, hostname, command, timestamps, exit code,
cgroup path, and durable stdout/stderr log paths.

`exec` still returns a clear not-implemented error.

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
sudo MINICTL_STATE_DIR=./.minictl-state ./minictl stop <id>
```

## Build

```sh
make
./minictl --help
make test
```

## Safety

This project is for learning container mechanics in a trusted Linux VM. It is
not a production isolation tool.
