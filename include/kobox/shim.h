#pragma once

#include <stddef.h>
#include <stdint.h>
#include "kobox/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

void *kb_kmalloc(size_t size, unsigned int flags);
void *kb_kzalloc(size_t size, unsigned int flags);
void *kb_kmalloc_trace(void *cache, unsigned int flags, size_t size);
void kb_kfree(void *ptr);
int kb_printk(const char *fmt, ...);

void kb_shim_set_backend(kb_backend_t *backend);

int kb_request_threaded_irq(
    unsigned int irq,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    unsigned long flags,
    const char *name,
    void *dev_id);
void kb_free_irq(unsigned int irq, void *dev_id);

void *kb_dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs);
void kb_dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs);

void kb_stack_chk_fail(void);

#ifdef __cplusplus
}
#endif
