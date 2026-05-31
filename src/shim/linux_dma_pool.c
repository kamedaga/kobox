#include "kobox/shim.h"

#include <stdint.h>
#include <stdlib.h>

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

void *kb_dma_pool_create(const char *name, void *dev, size_t size, size_t align, size_t allocation)
{
    (void)name;
    (void)dev;
    (void)allocation;
    if (size == 0) {
        return NULL;
    }
    shim_dma_pool_t *pool = calloc(1, sizeof(*pool));
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

    shim_dma_pool_alloc_t *alloc = calloc(1, sizeof(*alloc));
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
            free(alloc);
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
        free(alloc);
    }
    free(dma_pool);
}

int kb_dma_set_mask(void *dev, uint64_t mask)
{
    (void)dev;
    (void)mask;
    return 0;
}

int kb_dma_set_coherent_mask(void *dev, uint64_t mask)
{
    (void)dev;
    (void)mask;
    return 0;
}
