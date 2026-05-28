# kobox

Run Linux `.ko` kernel modules in userspace with a portable libc-based shim and OS-specific backends.

## What is kobox?

kobox loads precompiled Linux kernel modules (`.ko`) into a userspace process. Linux kernel symbols are resolved by a compatibility shim, while device access is delegated to a backend such as Linux VFIO, PachaOS, or OpenBSD.

The first development target is Linux. Linux makes behavior and performance measurable against native kernel drivers before the runtime is ported to other operating systems.

## Design Goals

- Run existing `.ko` binaries without recompilation.
- Keep the Linux compatibility shim portable and libc-based.
- Keep OS-specific device access behind a backend API.
- Make Linux, PachaOS, and OpenBSD support backend work, not shim rewrites.
- Measure overhead against native Linux drivers before claiming portability wins.

## Architecture

```text
Linux Driver (.ko binary)
        |
        | Linux kernel symbols
        v
libc-based Linux shim layer
  kmalloc, mutex, workqueue, pci_*, dma_*, request_irq, ...
        |
        | kobox backend API only
        v
OS backend
  linux_mock / linux_vfio / pachaos / openbsd
```

The shim intentionally uses libc and standard userspace primitives such as `malloc`, `pthread`, `mmap`, `clock_gettime`, and C atomics. It does not pretend to be a kernel internally.

Backends are responsible for OS-specific device access:

- device enumeration
- PCI config access
- BAR/MMIO mapping
- DMA allocation and mapping
- IRQ delivery
- time, logging, and event integration

## Current Status

Early design stage. The next milestones are:

1. define the architecture and backend API
2. extend `kobox-inspect` from ELF header/section listing to symbol and relocation analysis
3. implement a `linux_mock` backend
4. implement a minimal userspace module loader
5. add a `linux_vfio` backend for real hardware

Versioning and releases are intentionally out of scope at this stage. Until the loader, shim boundary, and backend API stabilize, kobox should be treated as a design/prototype project rather than a versioned runtime.

## Build

kobox is written in C11 and currently requires CMake with clang.

```sh
cmake -S . -B .artifacts/build -DCMAKE_C_COMPILER=clang
cmake --build .artifacts/build
ctest --test-dir .artifacts/build
```

## Roadmap

The public roadmap is Linux-first:

1. **Module introspection**: parse ELF sections, relocations, `modinfo`, `vermagic`, undefined symbols, and dependencies.
2. **Userspace loader**: place `.text`, `.data`, `.bss`, apply x86_64 relocations, and call `init_module` / `cleanup_module`.
3. **libc-based shim core**: implement memory, logging, synchronization, timers, workqueues, and common Linux helper APIs.
4. **linux_mock backend**: test loader and shim behavior without real hardware.
5. **linux_vfio backend**: map PCI devices, BARs, DMA, and IRQs through VFIO.
6. **Performance track**: compare with native Linux drivers using `perf`, `ftrace`, `fio`, and workload-specific benchmarks.
7. **Portable backends**: add PachaOS and OpenBSD backends after the Linux runtime is measurable and stable.

## License

Apache-2.0
