# KVM Subsystem

Placeholder for KVM-style module support.

Expected responsibilities include VM objects, vCPU lifecycle, guest memory,
ioevent/irqfd-style event routing, and preserving Linux KVM semantics while
using a thin host IPC endpoint until a specialized VM transport is justified.
