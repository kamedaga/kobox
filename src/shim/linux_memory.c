#include "kobox/shim.h"

#include <stdlib.h>
#include <string.h>

void *kb_kmalloc(size_t size, unsigned int flags)
{
    (void)flags;
    return malloc(size);
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
