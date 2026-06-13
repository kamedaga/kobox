# Linux Personality

`linux_personality` implements the Linux kernel ABI surface seen by loaded
`.ko` modules.

This layer owns Linux symbol implementations, Linux calling context, Linux
allocation and synchronization semantics, module-facing helper functions, and
Linux object conventions.

It may model Linux concepts such as `struct device`, `struct file`, `jiffies`,
workqueues, and error pointers.

It must not call host OS APIs such as VFIO ioctls, PachaOS syscalls, host IPC,
audio services, or virtualization services directly.  Those belong behind
`device`, `platform`, or `host_interface`.

Symbol providers owned by this layer:

- `linux_core_symbols.c`: common implemented Linux ABI symbols such as printk,
  memory allocation, IRQ helpers, sync primitives, timers, workqueues, and
  virtio/input-adjacent generic helpers.
- `linux_stub_symbols.c`: temporary generic fallback symbols such as no-op,
  return-zero, return-one, allocation stub, identity pointer, and empty string
  mappings.
