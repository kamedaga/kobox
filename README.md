# kobox

> Run Linux `.ko` kernel modules in userspace — portable, libc-based, no kernel patches required.

![Language: C11](https://img.shields.io/badge/language-C11-blue?style=flat-square&logo=c)
![Build: CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake)
![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue?style=flat-square)
![Status](https://img.shields.io/badge/status-active-brightgreen?style=flat-square)

kobox loads precompiled Linux kernel modules (`.ko`) into a userspace process.
Linux kernel symbols are resolved by a compatibility layer, while OS-specific capabilities are delegated to platform facets and host interfaces.

The current implementation is strongest for device drivers, especially PCI-backed storage and USB stacks.  The architecture is being generalized so the same runtime can also host major Linux module families such as filesystems, security modules, sound, networking, and KVM-style virtualization modules.

---

## Design Goals

- Run existing `.ko` binaries without recompilation.
- Keep the Linux compatibility layer portable and libc-based.
- Keep OS-specific access behind platform facets and host interfaces.
- Treat device access as one platform facet, not the whole runtime boundary.
- Make Linux, PachaOS, and OpenBSD support a matter of platform/interface work, not Linux compatibility rewrites.
- Measure overhead against native Linux drivers before claiming portability wins.

## Architecture

```text
Linux Module (.ko binary)
        |
        | Linux kernel symbols
        v
Linux personality
  kmalloc, mutex, workqueue, VFS, driver model, LSM, ALSA, KVM, ...
        |
        | kobox runtime API only
        v
kobox runtime core
  lifecycle, object registry, event loop, timers, resources
        |
        +--> platform facets
        |      device, memory, event, time, log
        |
        +--> host interfaces
               socket, IPC, FUSE, sound, VM, ...
```

The Linux personality intentionally uses libc and standard userspace primitives — `malloc`, `pthread`, `mmap`, `clock_gettime`, C atomics — rather than reimplementing a kernel internally.

The existing device backend API is the current device platform facet. It handles:

- Device enumeration
- PCI config access
- BAR / MMIO mapping
- DMA allocation and mapping
- IRQ delivery
- Time, logging, and event integration

Host interfaces are separate from device backends. They are the OS-specific surfaces that expose a loaded module to the outside world, such as Linux sockets, IPC, FUSE, ALSA/PipeWire bridges, or a PachaOS service endpoint.

---

## Current Status

NVMe and USB are working end-to-end on both the Linux VFIO backend and the PachaOS Capsule backend.

| Driver | Linux VFIO | PachaOS Capsule |
|---|---|---|
| NVMe | Working | Working |
| USB Storage (xHCI / BOT / SCSI) | Working | Working |
| Network (e1000e / r8169) | In progress | — |
| SATA (AHCI) | Planned | — |
| NVIDIA GPU | `init_module` passes | — |

---

## Build

kobox is written in C11 and requires CMake with clang.

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

---

## PachaOS Capsule Backend

`pachaos_capsule` creates a kobox backend from a PachaOS `DeviceCapsule` token and uses PachaOS native syscalls for Capsule operations.

```sh
KOBOX_PACHAOS_DEVICE_CAPSULE=0xca12000000000001 kobox-ls-devices pachaos
kobox-run --device=pachaos --capsule=0xca12000000000001 run driver.ko
```

PCI identity and BAR sizes can be supplied via environment variables until the PachaOS Capsule ABI grows config/BAR info calls:

```sh
KOBOX_PACHAOS_PCI_ID=8086:10d3:02:00:00
KOBOX_PACHAOS_BAR0_SIZE=0x1000
```

---

## Roadmap

1. NVMe — complete
2. USB (xHCI) — complete: HID + Mass Storage (BOT / SCSI / block I/O), multi-device
3. Network (e1000e / r8169) — reusing PCI + DMA shim
4. SATA (AHCI) — storage shim shared with NVMe
5. NVIDIA GPU — `init_module` confirmed passing
6. Runtime generalization — platform facets, host interfaces, and subsystem-owned symbol registration
7. Non-driver module families — filesystems first, then security, sound, and KVM

---

## License

Apache-2.0
