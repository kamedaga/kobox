# Platforms

`platform` binds together OS-specific capabilities for a kobox process.

A platform chooses device backends, host interfaces, time/event/log plumbing,
and future OS-level policy.  Linux, PachaOS, and OpenBSD support should be
added here without forking the Linux personality layer.

The current `kb_platform_t` API is a small aggregate over independent facets:

- `device`: PCI config, BAR/MMIO, DMA, IRQ, and device enumeration.  This is
  currently backed by `kb_device_backend_t`.
- `memory`: process-local allocation for runtime-owned platform resources.
- `time`: monotonic clock and sleep.
- `log`: host logging.
- `event`: minimal polling hook for future event-loop integration.

`kb_platform_create` takes ownership of the provided device backend and host
interface pointers after successful construction.  The Linux personality should
not call OS-specific APIs directly; it should reach host capabilities through
runtime/platform/device boundaries.
