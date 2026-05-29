#include "kobox/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_LINUX___GFP_ZERO = 0x100u,
};

static int trace_memory(void)
{
    return getenv("KOBOX_TRACE_SHIMS") != NULL;
}

void *kb_kmalloc(size_t size, unsigned int flags)
{
    void *ptr = malloc(size);
    if (ptr != NULL && (flags & KB_LINUX___GFP_ZERO) != 0) {
        memset(ptr, 0, size);
    }
    if (trace_memory()) {
        fprintf(stderr, "kobox-shim: kmalloc size=%zu flags=0x%x ptr=%p\n", size, flags, ptr);
    }
    return ptr;
}

void *kb_kzalloc(size_t size, unsigned int flags)
{
    (void)flags;
    return calloc(1, size);
}

void *kb_kmalloc_trace(void *cache, unsigned int flags, size_t size)
{
    (void)cache;
    return kb_kmalloc(size, flags);
}

void kb_kfree(void *ptr)
{
    free(ptr);
}

void *kmalloc(size_t size, unsigned int flags)
{
    return kb_kmalloc(size, flags);
}

void *kzalloc(size_t size, unsigned int flags)
{
    return kb_kzalloc(size, flags);
}

void *kmalloc_trace(void *cache, unsigned int flags, size_t size)
{
    return kb_kmalloc_trace(cache, flags, size);
}

void kfree(void *ptr)
{
    kb_kfree(ptr);
}
