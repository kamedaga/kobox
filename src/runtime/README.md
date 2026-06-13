# Runtime Core

`runtime` is the OS-neutral kobox core.

It owns module lifecycle orchestration, object registries, event loop integration,
timers, deferred work scheduling, and resource routing.

It must not know Linux kernel ABI details such as `struct pci_dev`,
`struct file`, `struct inode`, or Linux symbol names.  Linux-specific behavior
belongs in `linux_personality` or `linux_subsystem`.

It must not call OS-specific APIs directly.  OS capabilities enter through
`platform`, `device`, and `host_interface`.
