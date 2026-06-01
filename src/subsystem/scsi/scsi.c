#include "subsystem/scsi/scsi.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_SCSI_HOST_MAX = 32,
    KB_SCSI_HOST_PRIVATE_OFFSET = 0x820,
};

typedef struct kb_scsi_host_record {
    int active;
    kb_scsi_host_snapshot_t snapshot;
} kb_scsi_host_record_t;

static kb_scsi_host_record_t scsi_hosts[KB_SCSI_HOST_MAX];

static kb_scsi_host_record_t *host_record_find(const void *host)
{
    if (host == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_SCSI_HOST_MAX; i++) {
        if (scsi_hosts[i].active && scsi_hosts[i].snapshot.host == host) {
            return &scsi_hosts[i];
        }
    }
    return NULL;
}

void *kb_scsi_subsystem_host_alloc(void *host_template, size_t private_size)
{
    for (size_t i = 0; i < KB_SCSI_HOST_MAX; i++) {
        if (scsi_hosts[i].active) {
            continue;
        }
        size_t alloc_size = KB_SCSI_HOST_PRIVATE_OFFSET + private_size + 256;
        void *host = calloc(1, alloc_size);
        if (host == NULL) {
            return NULL;
        }
        memset(&scsi_hosts[i], 0, sizeof(scsi_hosts[i]));
        scsi_hosts[i].active = 1;
        scsi_hosts[i].snapshot.host = host;
        scsi_hosts[i].snapshot.host_template = host_template;
        scsi_hosts[i].snapshot.private_data = (unsigned char *)host + KB_SCSI_HOST_PRIVATE_OFFSET;
        scsi_hosts[i].snapshot.private_size = private_size;
        return host;
    }
    return NULL;
}

void *kb_scsi_subsystem_host_private(void *host)
{
    kb_scsi_host_record_t *record = host_record_find(host);
    return record == NULL ? NULL : record->snapshot.private_data;
}

int kb_scsi_subsystem_host_add(void *host, void *dev, void *dma_dev)
{
    kb_scsi_host_record_t *record = host_record_find(host);
    if (record == NULL) {
        return -22;
    }
    record->snapshot.dev = dev;
    record->snapshot.dma_dev = dma_dev;
    record->snapshot.added = 1;
    return 0;
}

void kb_scsi_subsystem_host_scan(void *host)
{
    kb_scsi_host_record_t *record = host_record_find(host);
    if (record != NULL) {
        record->snapshot.scan_count++;
    }
}

void kb_scsi_subsystem_host_remove(void *host)
{
    kb_scsi_host_record_t *record = host_record_find(host);
    if (record != NULL) {
        record->snapshot.added = 0;
        record->snapshot.remove_count++;
    }
}

void kb_scsi_subsystem_host_put(void *host)
{
    kb_scsi_host_record_t *record = host_record_find(host);
    if (record == NULL) {
        return;
    }
    record->snapshot.put_count++;
    free(record->snapshot.host);
    memset(record, 0, sizeof(*record));
}

int kb_scsi_subsystem_host_snapshot(const void *host, kb_scsi_host_snapshot_t *out_snapshot)
{
    kb_scsi_host_record_t *record = host_record_find(host);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}
