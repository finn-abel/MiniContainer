# Rootfs

MiniContainer-C expects a trusted extracted Linux root filesystem.

Before a command is cloned, `rootfs_validate` checks that the rootfs path exists,
is a directory, and contains the requested absolute command path. For example,
running `/bin/sh` requires:

```text
<rootfs>/bin/sh
```

During child setup, the runtime makes mount propagation private, bind mounts the
rootfs onto itself, ensures `<rootfs>/proc` exists, and then switches into the
rootfs with `pivot_root`.

The switch sequence is contained in `rootfs_switch_root`:

```text
create <rootfs>/.old_root
chdir(<rootfs>)
pivot_root(".", ".old_root")
chdir("/")
umount /.old_root
remove /.old_root
mount proc at /proc
```

After this step, commands should see the supplied rootfs as `/`. The rootfs must
contain the command and any dynamic linker or shared libraries that command
needs.
