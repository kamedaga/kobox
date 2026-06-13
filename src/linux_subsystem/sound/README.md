# Sound Subsystem

Placeholder for Linux sound module support.

Expected responsibilities include ALSA-style card, PCM, control, and event
models. Host routing should start as a thin IPC endpoint and only grow a
specialized transport after a real sound target requires it.
