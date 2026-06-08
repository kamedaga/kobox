#include "kobox/shim.h"
#include "subsystem/dma/dma.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

kb_backend_t *kb_shim_current_backend(void);

typedef struct shim_dma_pool_alloc {
    void *vaddr;
    uint64_t dma_addr;
    struct shim_dma_pool_alloc *next;
} shim_dma_pool_alloc_t;

typedef struct shim_dma_pool {
    size_t size;
    size_t align;
    shim_dma_pool_alloc_t *allocs;
} shim_dma_pool_t;

static int low_or_error_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value != 0 && (value < 4096u || value >= (uintptr_t)-4095);
}

static int trace_dma_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

void *kb_dma_pool_create(const char *name, void *dev, size_t size, size_t align, size_t allocation)
{
    (void)name;
    (void)dev;
    (void)allocation;
    if (size == 0) {
        return NULL;
    }
    shim_dma_pool_t *pool = kb_kzalloc(sizeof(*pool), 0);
    if (pool == NULL) {
        return NULL;
    }
    pool->size = size;
    pool->align = align == 0 ? 4096 : align;
    return pool;
}

void *kb_dma_pool_alloc(void *pool, unsigned int flags, uint64_t *dma_handle)
{
    (void)flags;
    shim_dma_pool_t *dma_pool = pool;
    if (dma_pool == NULL || dma_handle == NULL) {
        return NULL;
    }
    void *vaddr = kb_dma_alloc_attrs(NULL, dma_pool->size, dma_handle, 0, 0);
    if (vaddr == NULL) {
        return NULL;
    }
    if (low_or_error_pointer(vaddr)) {
        kb_dma_free_attrs(NULL, dma_pool->size, vaddr, *dma_handle, 0);
        *dma_handle = 0;
        return NULL;
    }

    shim_dma_pool_alloc_t *alloc = kb_kzalloc(sizeof(*alloc), 0);
    if (alloc == NULL) {
        kb_dma_free_attrs(NULL, dma_pool->size, vaddr, *dma_handle, 0);
        return NULL;
    }
    alloc->vaddr = vaddr;
    alloc->dma_addr = *dma_handle;
    alloc->next = dma_pool->allocs;
    dma_pool->allocs = alloc;
    return vaddr;
}

void kb_dma_pool_free(void *pool, void *vaddr, uint64_t dma_addr)
{
    shim_dma_pool_t *dma_pool = pool;
    if (dma_pool == NULL || vaddr == NULL) {
        return;
    }
    shim_dma_pool_alloc_t **cursor = &dma_pool->allocs;
    while (*cursor != NULL) {
        shim_dma_pool_alloc_t *alloc = *cursor;
        if (alloc->vaddr == vaddr && alloc->dma_addr == dma_addr) {
            *cursor = alloc->next;
            kb_kfree(alloc);
            break;
        }
        cursor = &alloc->next;
    }
    kb_dma_free_attrs(NULL, dma_pool->size, vaddr, dma_addr, 0);
}

void kb_dma_pool_destroy(void *pool)
{
    shim_dma_pool_t *dma_pool = pool;
    if (dma_pool == NULL) {
        return;
    }
    while (dma_pool->allocs != NULL) {
        shim_dma_pool_alloc_t *alloc = dma_pool->allocs;
        dma_pool->allocs = alloc->next;
        kb_dma_free_attrs(NULL, dma_pool->size, alloc->vaddr, alloc->dma_addr, 0);
        kb_kfree(alloc);
    }
    kb_kfree(dma_pool);
}

int kb_dma_set_mask(void *dev, uint64_t mask)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (trace_dma_enabled()) {
        fprintf(stderr, "kobox dma: set_mask dev=%p mask=0x%llx\n", dev, (unsigned long long)mask);
    }
    if (device != NULL && ops != NULL && ops->dma_set_mask != NULL) {
        return ops->dma_set_mask(device, mask, 0) == KB_OK ? 0 : -22;
    }
    return 0;
}

int kb_dma_set_coherent_mask(void *dev, uint64_t mask)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (trace_dma_enabled()) {
        fprintf(stderr, "kobox dma: set_coherent_mask dev=%p mask=0x%llx\n", dev, (unsigned long long)mask);
    }
    if (device != NULL && ops != NULL && ops->dma_set_mask != NULL) {
        return ops->dma_set_mask(device, mask, 1) == KB_OK ? 0 : -22;
    }
    return 0;
}
