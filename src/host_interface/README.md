# Host Interfaces

`host_interface` exposes loaded modules to the outside host OS.

The first target is intentionally thin: every host-facing module surface is
connected through an IPC endpoint unless a concrete subsystem proves that it
needs a richer transport.

The interface layer should not hide subsystem semantics. Filesystems, security
modules, sound, and KVM-style modules keep their Linux-facing meaning in
`linux_subsystem`; the host interface only binds that subsystem to an endpoint.

Host interfaces are separate from device backends.  A filesystem module may
need a host interface without a device backend, while a PCI driver may need a
device backend without any external interface.
