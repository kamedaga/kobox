#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct kb_scsi_host_snapshot {
    void *host;
    void *host_template;
    void *private_data;
    void *dev;
    void *dma_dev;
    size_t private_size;
    uint32_t added;
    uint32_t scan_count;
    uint32_t remove_count;
    uint32_t put_count;
} kb_scsi_host_snapshot_t;

void *kb_scsi_subsystem_host_alloc(void *host_template, size_t private_size);
void *kb_scsi_subsystem_host_private(void *host);
int kb_scsi_subsystem_host_add(void *host, void *dev, void *dma_dev);
void kb_scsi_subsystem_host_scan(void *host);
void kb_scsi_subsystem_host_remove(void *host);
void kb_scsi_subsystem_host_put(void *host);
int kb_scsi_subsystem_host_snapshot(const void *host, kb_scsi_host_snapshot_t *out_snapshot);
