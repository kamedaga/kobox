#pragma once

#include "kobox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_linux_vfio_virtio_blk_provider kb_linux_vfio_virtio_blk_provider_t;

kb_status_t kb_linux_vfio_virtio_blk_provider_create(
    const char *pci_bdf,
    kb_linux_vfio_virtio_blk_provider_t **out_provider);
void kb_linux_vfio_virtio_blk_provider_destroy(kb_linux_vfio_virtio_blk_provider_t *provider);
void *kb_linux_vfio_virtio_blk_provider_disk(kb_linux_vfio_virtio_blk_provider_t *provider);

#ifdef __cplusplus
}
#endif
