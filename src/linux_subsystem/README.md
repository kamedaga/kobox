# Linux Subsystems

`linux_subsystem` contains Linux module-family adapters built on top of the
Linux personality.

Existing adapters cover PCI, USB, block, SCSI, NVMe, input, and DMA support.
New non-driver module families should land here first:

- `fs`
- `security`
- `sound`
- `kvm`
- `net`
- `char`
- `procfs`
- `sysfs`

Subsystem adapters may understand Linux ABI semantics, but they must not call
host OS APIs directly.  They route host work through `device` and
`host_interface`.

Subsystem-owned symbol providers live next to the subsystem implementation.
The current provider split is:

- `block/block_symbols.c`
- `dma/dma_symbols.c`
- `fs/fs_symbols.c`
- `input/input_symbols.c`
- `kvm/kvm_symbols.c`
- `net/net_symbols.c`
- `pci/pci_symbols.c`
- `security/security_symbols.c`
- `sound/sound_symbols.c`
- `usb/usb_symbols.c`

`loader/module_loader.c` aggregates these providers when it builds local shim
stubs.  New module families should add their own provider instead of appending
large symbol tables to the loader.

Generic fallback symbols that are not yet owned by a subsystem live in
`linux_personality/linux_stub_symbols.c`; they are a temporary compatibility
surface, not a subsystem implementation.

Loader-local symbols should stay in `loader/module_loader.c` only when their
implementation depends on loader-private state or code patching details.
