
# kobox

Run Linux .ko kernel modules in userspace, on any OS — binary compatible, no recompilation.

## What is kobox?

kobox loads precompiled Linux kernel modules (.ko) and executes them
in userspace by resolving kernel symbols to its own shim implementations.
No kernel source required. No recompilation. Just the .ko binary.

## Why?

• Lightweight and simple because it only executes the driver binary.

• Supports closed-loop drivers because it executes at the binary level.

• Allows secure execution of Linux's rich resources in user space.

## Architecture

```
┌─────────────────────────────────┐
│     Linux Driver (.ko binary)    │
├─────────────────────────────────┤
│     kobox shim layer             │
│  (kmalloc, dma_*, pci_*, ...)   │
├─────────────────────────────────┤
│     Backend (OS-specific)        │
│  linux_vfio / pachaos / freebsd │
└─────────────────────────────────┘
```

## Roadmap

1. NVMe — single .ko, PCI + DMA + IRQ shim foundation
2. USB (xHCI) — multi .ko loading, subsystem support
3. Network (e1000e / r8169) — reuse PCI + DMA shim
4. SATA (AHCI) — storage shim shared with NVMe
5. NVIDIA GPU — the final boss

## Design Principles

- **Don't reinvent — reuse and isolate**
- libc-based shim for maximum portability
- Backend abstraction: one shim layer, multiple OS targets
- Capability-friendly: designed for sandboxed driver execution

## Status

Early development. ELF loader and shim architecture in progress.

## License

Apache-2.0