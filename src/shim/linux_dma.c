#include "kobox/shim.h"

#include <stdlib.h>

kb_backend_t *kb_shim_current_backend(void);

typedef struct shim_dma {
    void *cpu_addr;
    uint64_t dma_addr;
    uint64_t size;
    kb_device_t *device;
    struct shim_dma *next;
} shim_dma_t;

static shim_dma_t *dma_list;

static kb_status_t first_device(kb_backend_t *backend, kb_device_t **out_device)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_at == NULL) {
        return KB_ERR_INVALID;
    }
    return ops->device_at(backend, 0, out_device);
}

void *kb_dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs)
{
    (void)dev;
    (void)flags;
    (void)attrs;
    if (size == 0 || dma_handle == NULL) {
        return NULL;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = NULL;
    if (first_device(backend, &device) != KB_OK) {
        return NULL;
    }

    kb_dma_buffer_t buffer;
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL ||
        ops->dma_alloc == NULL ||
        ops->dma_alloc(device, size, 4096, KB_DMA_BIDIRECTIONAL, &buffer) != KB_OK)
    {
        return NULL;
    }

    shim_dma_t *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        ops->dma_free(device, &buffer);
        return NULL;
    }
    entry->cpu_addr = buffer.cpu_addr;
    entry->dma_addr = buffer.dma_addr;
    entry->size = buffer.size;
    entry->device = device;
    entry->next = dma_list;
    dma_list = entry;

    *dma_handle = buffer.dma_addr;
    return buffer.cpu_addr;
}

void kb_dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs)
{
    (void)dev;
    (void)attrs;
    shim_dma_t **cursor = &dma_list;
    while (*cursor != NULL) {
        shim_dma_t *entry = *cursor;
        if (entry->cpu_addr == cpu_addr && entry->dma_addr == dma_handle) {
            *cursor = entry->next;
            kb_dma_buffer_t buffer = {
                .cpu_addr = entry->cpu_addr,
                .dma_addr = entry->dma_addr,
                .size = size == 0 ? entry->size : size,
                .flags = 0,
            };
            const kb_backend_ops_t *ops = kb_backend_get_ops(kb_shim_current_backend());
            if (ops != NULL && ops->dma_free != NULL) {
                ops->dma_free(entry->device, &buffer);
            }
            free(entry);
            return;
        }
        cursor = &entry->next;
    }
}

void *dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs)
{
    return kb_dma_alloc_attrs(dev, size, dma_handle, flags, attrs);
}

void dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs)
{
    kb_dma_free_attrs(dev, size, cpu_addr, dma_handle, attrs);
}
