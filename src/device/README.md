# Device Backends

`device` is the platform facet for hardware and device access.

It owns PCI inventory, PCI config space access, BAR/MMIO mapping, DMA mapping,
IRQ delivery, and device-oriented time/log hooks.

This directory intentionally replaces the old `backend` API name.  Do not add
`kb_backend_*` aliases; kobox is still unstable and the canonical API is
`kb_device_backend_*`.

Device backends must not include Linux personality internals unless they are a
deliberate test/mock helper.  Linux kernel object semantics belong above this
layer.
