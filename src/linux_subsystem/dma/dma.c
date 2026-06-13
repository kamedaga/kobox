#include "linux_subsystem/dma/dma.h"
#include "kobox/shim.h"

typedef struct kb_subsystem_dma_mapping {
    void *cpu_addr;
    uint64_t dma_addr;
    uint64_t size;
    kb_device_t *device;
    struct kb_subsystem_dma_mapping *next;
} kb_subsystem_dma_mapping_t;

static kb_subsystem_dma_mapping_t *dma_mappings;

kb_device_t *kb_subsystem_dma_default_device(kb_device_backend_t *backend)
{
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    kb_device_t *device = NULL;
    if (ops == NULL || ops->device_at == NULL || ops->device_at(backend, 0, &device) != KB_OK) {
        return NULL;
    }
    return device;
}

void *kb_subsystem_dma_alloc(
    kb_device_backend_t *backend,
    kb_device_t *device,
    size_t size,
    uint64_t *dma_handle)
{
    if (backend == NULL || size == 0 || dma_handle == NULL) {
        return NULL;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }
    if (device == NULL) {
        return NULL;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    kb_dma_buffer_t buffer;
    if (ops == NULL ||
        ops->dma_alloc == NULL ||
        ops->dma_alloc(device, size, 4096, KB_DMA_BIDIRECTIONAL, &buffer) != KB_OK)
    {
        return NULL;
    }

    kb_subsystem_dma_mapping_t *entry = kb_kzalloc(sizeof(*entry), 0);
    if (entry == NULL) {
        if (ops->dma_free != NULL) {
            ops->dma_free(device, &buffer);
        }
        return NULL;
    }
    entry->cpu_addr = buffer.cpu_addr;
    entry->dma_addr = buffer.dma_addr;
    entry->size = buffer.size;
    entry->device = device;
    entry->next = dma_mappings;
    dma_mappings = entry;

    *dma_handle = buffer.dma_addr;
    return buffer.cpu_addr;
}

void kb_subsystem_dma_free(
    kb_device_backend_t *backend,
    size_t size,
    void *cpu_addr,
    uint64_t dma_handle)
{
    kb_subsystem_dma_mapping_t **cursor = &dma_mappings;
    while (*cursor != NULL) {
        kb_subsystem_dma_mapping_t *entry = *cursor;
        if (entry->cpu_addr == cpu_addr && entry->dma_addr == dma_handle) {
            *cursor = entry->next;
            kb_dma_buffer_t buffer = {
                .cpu_addr = entry->cpu_addr,
                .dma_addr = entry->dma_addr,
                .size = size == 0 ? entry->size : size,
                .flags = 0,
            };
            const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
            if (ops != NULL && ops->dma_free != NULL) {
                ops->dma_free(entry->device, &buffer);
            }
            kb_kfree(entry);
            return;
        }
        cursor = &entry->next;
    }
}

void *kb_subsystem_dma_cpu_addr(uint64_t dma_addr, size_t *out_available)
{
    for (kb_subsystem_dma_mapping_t *entry = dma_mappings; entry != NULL; entry = entry->next) {
        if (dma_addr < entry->dma_addr || dma_addr >= entry->dma_addr + entry->size) {
            continue;
        }

        uint64_t offset = dma_addr - entry->dma_addr;
        if (out_available != NULL) {
            *out_available = (size_t)(entry->size - offset);
        }
        return (unsigned char *)entry->cpu_addr + offset;
    }

    if (out_available != NULL) {
        *out_available = 0;
    }
    return NULL;
}

uint64_t kb_subsystem_dma_map(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    kb_status_t *out_status)
{
    if (out_status != NULL) {
        *out_status = KB_ERR_INVALID;
    }
    if (backend == NULL || cpu_addr == NULL || size == 0) {
        return 0;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }
    if (device == NULL) {
        return 0;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops == NULL || ops->dma_map == NULL) {
        if (out_status != NULL) {
            *out_status = KB_ERR_UNSUPPORTED;
        }
        return 0;
    }

    uint64_t dma_addr = 0;
    kb_status_t status = ops->dma_map(device, cpu_addr, size, direction, &dma_addr);
    if (out_status != NULL) {
        *out_status = status;
    }
    return status == KB_OK ? dma_addr : 0;
}

void kb_subsystem_dma_unmap(
    kb_device_backend_t *backend,
    kb_device_t *device,
    uint64_t dma_addr,
    size_t size,
    kb_dma_dir_t direction)
{
    if (backend == NULL || dma_addr == 0 || size == 0) {
        return;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (device != NULL && ops != NULL && ops->dma_unmap != NULL) {
        ops->dma_unmap(device, dma_addr, size, direction);
    }
}

int kb_subsystem_dma_mapping_error(uint64_t dma_addr)
{
    return dma_addr == 0;
}
