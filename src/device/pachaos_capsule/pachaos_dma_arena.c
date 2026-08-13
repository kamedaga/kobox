#include "linux_subsystem/kvm/kvm_symbols.h"

#if defined(__pachaos__)

#include "pacha/ipc.h"

#include <stdio.h>

void *kb_kvm_host_alloc_dma_arena(size_t bytes)
{
    const int vmo_fd = pacha_vmo_create_contiguous(bytes, 0, 0);
    if (vmo_fd < 16) {
        fprintf(stderr, "kobox-kvm: dma arena fallback bytes=%zu\n", bytes);
        return NULL;
    }

    void *mapping = pacha_mmap(
        vmo_fd,
        bytes,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapping == NULL) {
        (void)pacha_fd_close(vmo_fd);
        fprintf(stderr, "kobox-kvm: dma arena fallback bytes=%zu\n", bytes);
        return NULL;
    }

    fprintf(stderr, "kobox-kvm: dma arena vmo fd=%d bytes=%zu\n", vmo_fd, bytes);
    return mapping;
}

#endif
