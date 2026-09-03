# kobox

> Linux `.ko` kernel modules, running in userspace. No kernel patches. No custom kernel. Just libc.

![Language: C11](https://img.shields.io/badge/language-C11-blue?style=flat-square&logo=c)
![Build: CMake](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake)
![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue?style=flat-square)
![Status](https://img.shields.io/badge/status-active-brightgreen?style=flat-square)

kobox loads a precompiled `.ko` binary into a normal userspace process and hooks up its Linux kernel symbol calls. The compatibility layer is pure libc — `malloc`, `pthread`, `mmap`, `clock_gettime`, C atomics — not a kernel reimplementation. OS-specific stuff (devices, memory, events) is pushed behind platform facets so the Linux compat layer stays portable.

---

## Why

Most approaches to running kernel modules outside the kernel require either a patched kernel, a VM, or a custom OS build. kobox does none of that.

The bet: a faithful-enough Linux personality built on libc can run real `.ko` binaries without touching the kernel.

Design constraints:

- Run existing `.ko` binaries as-is, no recompile.
- Linux compat layer stays libc-only and portable.
- OS-specific surfaces (device access, IPC, sockets) live in platform facets and host interfaces — not baked into the compat layer.
- Device access is one facet. Not the whole runtime.
- Adding Linux / PachaOS / OpenBSD support means writing platform/interface code, not touching Linux compat.
- Benchmark overhead against native Linux drivers before claiming portability wins.

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

The Linux personality is intentionally not a kernel — it's libc + pthread + C atomics + standard POSIX primitives. No internal kernel emulation, no special memory models. Just enough to make the module's symbol calls land somewhere sensible.

The current device backend is the device platform facet. It covers:

- Device enumeration
- PCI config space access
- BAR / MMIO mapping
- DMA allocation + mapping
- IRQ delivery
- Time, logging, event loop integration

Host interfaces are separate from device backends — they're how a loaded module exposes itself to the outside world. Linux sockets, IPC, FUSE, ALSA/PipeWire bridges, PachaOS service endpoints, etc.

---

## Platform Facets & Host Interfaces

### Platform Facets

A platform bundles a device backend with OS-level abstractions. Four facets are defined:

| Facet | What it provides | Default |
|---|---|---|
| memory | alloc/free for runtime-owned resources | `malloc`/`free` (or device backend if available) |
| time | `monotonic_ns`, `sleep_ns` | `clock_gettime(CLOCK_MONOTONIC)` + `nanosleep` |
| log | host logging | delegates to device backend |
| event | `poll_once(timeout_ns)` | sleeps for timeout (event loop placeholder) |

Platform composition for Linux, PachaOS, and OpenBSD lives under `src/platform/` — each selects a device backend and wires up the right interfaces for that OS.

### Host Interfaces

Host interfaces are how a loaded module exposes itself outward. They're independent from device backends and wired into the platform at creation time via `kb_platform_desc_t`.

Each interface has a `subsystem` field that identifies which module family it serves — `"fs"`, `"sound"`, `"security"`, etc. The interface vtable covers:

| Op | What it does |
|---|---|
| `bind(platform)` | connect to platform at load time |
| `unbind()` | disconnect |
| `poll(timeout_ns)` | check for incoming events |
| `dispatch(msg, size)` | send a message through the interface |

Currently one kind is implemented: **IPC** (`KB_INTERFACE_IPC`) via `kb_linux_ipc_interface_create()`. A PachaOS IPC interface is next.


---

## Status

NVMe, USB, and ext4 are working end-to-end on the Linux VFIO backend.

| Driver / Module | Linux VFIO | PachaOS Capsule |
|---|---|---|
| NVMe | Working | Working |
| USB Storage (xHCI / BOT / SCSI) | Working | Working |
| KVM (Linux guest, `/sbin/init`) | Working | — |
| Network (e1000e / r8169) | In progress | — |
| SATA (AHCI) | Planned | — |
| NVIDIA GPU | `init_module` passes | — |

### KVM — Linux guest on kobox

Real `kvm.ko` + `kvm-amd.ko` loaded into kobox, booting a Linux `bzImage` guest to `/sbin/init`.

- [x] `kvm.ko` + `kvm-amd.ko` loaded and running in kobox
- [x] Guest Linux `bzImage` reaches `/sbin/init`
- [x] `/sbin/init` executes inside the guest

---

## Build

C11, CMake, clang.

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

---

## PachaOS Capsule Backend

`pachaos_capsule` wraps a PachaOS `DeviceCapsule` token into a kobox backend and routes Capsule ops through PachaOS native syscalls.

```sh
KOBOX_PACHAOS_DEVICE_CAPSULE=0xca12000000000001 kobox-ls-devices pachaos
kobox-run --device=pachaos --capsule=0xca12000000000001 run driver.ko
```

The PachaOS Capsule ABI doesn't expose config/BAR info yet, so pass them via env vars in the meantime:

```sh
KOBOX_PACHAOS_PCI_ID=8086:10d3:02:00:00
KOBOX_PACHAOS_BAR0_SIZE=0x1000
```

---

## Roadmap

1. NVMe — done
2. USB (xHCI) — done: HID + Mass Storage (BOT / SCSI / block I/O), multi-device
3. ext4 — next: wire through the NVMe-backed block abstraction
4. Network (e1000e / r8169) — PCI + DMA shim reuse
5. SATA (AHCI) — storage shim shared with NVMe
6. NVIDIA GPU — `init_module` confirmed passing; rest TBD
7. Runtime generalization — platform facets, host interfaces, subsystem-owned symbol registration
8. Non-driver modules — filesystems and KVM continue on provider abstractions

---

## License

GPLv2
Everyone is permitted to copy and distribute verbatim copies of this license document, but changing it is not allowed.
