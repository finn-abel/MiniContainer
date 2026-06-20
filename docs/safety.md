# Safety

MiniContainer-C is an educational project for trusted Linux virtual machines.

It is not Docker, it is not a sandbox for untrusted programs, and it should not
be treated as production-grade isolation. Future privileged tests and real
container runs should be performed only inside a disposable Linux VM with
trusted root filesystems and commands.

## Error Handling

Runtime errors should be explicit and errno-backed:

```text
minictl: <operation>: <specific errno message>
```

Do not ignore syscall failures. Parent and child paths should check return
values, close file descriptors they own, and preserve the original errno while
running best-effort cleanup.

## Networking

`--network bridge` mutates host network state: it creates a shared `minictl0`
bridge and per-container veth interfaces, and runs `ip`/`nsenter` as root. Keep
this on a disposable VM.

- The bridge is shared by all bridge containers and persists across runs by
  design; it is not removed automatically.
- There is no NAT and no IP forwarding in this step, so containers reach the
  host and each other on `10.0.0.0/24` but not the external internet.
- A container's veth pair is removed automatically when its network namespace is
  torn down at exit; host-side cleanup only runs on the early-failure path.
- This is not network isolation hardening. Do not rely on it to contain
  untrusted programs.

Default tests stay non-privileged and never touch host networking. Bridge
behavior is exercised only behind `sudo ROOTFS=./rootfs/alpine make integration`.

## Port publishing

`--publish HOST:CONTAINER` runs a userspace TCP proxy on the host, not iptables,
so it adds no NAT/firewall state. Even so, treat it as exposure, not isolation:

- The proxy binds `0.0.0.0` on the host, so a published port is reachable from
  anything that can reach the VM, not just `localhost`. Publish only trusted
  services on a disposable VM.
- It is TCP only and forwards blindly to the container IP; it performs no
  authentication, filtering, or rate limiting.
- One proxy process runs per published container, with its PID in metadata. It is
  reaped when the container exits, and `rm` best-effort stops any leftover proxy
  so host ports are freed.

## Cleanup

Repeated VM runs should not leave broken state behind. MiniContainer should:

- write metadata through a temporary file and atomic rename where possible
- remove partial state directories when container creation fails
- remove partial cgroup directories when limit setup fails
- detach rootfs bind mounts when setup fails before exec
- leave privileged integration tests outside the default `make test`

Default tests must run without sudo, without a rootfs, and without mutating
`/var/lib/minictl`. Real namespace/cgroup checks belong behind
`sudo ROOTFS=./rootfs/alpine make integration`.
