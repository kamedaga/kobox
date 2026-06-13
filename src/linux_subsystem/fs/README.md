# Filesystem Subsystem

This directory owns Linux filesystem-facing runtime state that is not part of
ELF loading.

Current responsibilities include kernel object registry state, char/proc/cdev
registration shims, fake file/fd/vma helpers, filesystem-facing symbol
registration, and the registered file-operations smoke runner. Planned
responsibilities include filesystem registration, superblock/inode models,
page-cache-facing shims, and routing filesystem operations through a thin host
IPC endpoint when external publication is needed.

The filesystem subsystem is a translation layer for whatever loaded `.ko`
requires from the Linux FS/VFS ABI. It should not hard-code ext4, btrfs, xfs,
or any other filesystem module name. The first real target can be an ext4
module, but ext4-specific coverage must be driven by that module's unresolved
symbols, init/register flow, and observed mount/read/write paths.

The host-facing side starts as a thin IPC endpoint for the `fs` subsystem.
Specialized transports should only be added when a real module path proves the
IPC endpoint is insufficient.
