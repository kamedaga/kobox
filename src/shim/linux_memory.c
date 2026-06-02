#include "kobox/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_LINUX___GFP_ZERO = 0x100u,
};

typedef struct kb_kmem_cache {
    size_t size;
    size_t align;
} kb_kmem_cache_t;

static int trace_memory(void)
{
    return getenv("KOBOX_TRACE_SHIMS") != NULL;
}

static int is_kernel_non_heap_pointer(const void *ptr)
{
    const uintptr_t value = (uintptr_t)ptr;
    if (value == 0) {
        return 1;
    }
    if (value < 4096u) {
        return 1;
    }
    return value >= (uintptr_t)-4095;
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

void *kb_kmalloc_node(size_t size, unsigned int flags, int node)
{
    (void)node;
    return kb_kmalloc(size, flags);
}

void *kb_kmalloc_node_trace(void *cache, unsigned int flags, int node, size_t size)
{
    (void)node;
    return kb_kmalloc_trace(cache, flags, size);
}

void *kb_kmemdup(const void *src, size_t len, unsigned int flags)
{
    if (src == NULL) {
        return NULL;
    }
    void *dst = kb_kmalloc(len, flags);
    if (dst != NULL) {
        memcpy(dst, src, len);
    }
    return dst;
}

void kb_kfree_sensitive(const void *ptr)
{
    kb_kfree((void *)ptr);
}

void *kb_kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags, void *ctor)
{
    (void)name;
    (void)flags;
    (void)ctor;
    kb_kmem_cache_t *cache = malloc(sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    cache->size = size == 0 ? 1 : size;
    cache->align = align;
    if (trace_memory()) {
        fprintf(stderr, "kobox-shim: kmem_cache_create size=%zu align=%zu cache=%p\n", size, align, (void *)cache);
    }
    return cache;
}

void kb_kmem_cache_destroy(void *cache)
{
    free(cache);
}

void *kb_kmem_cache_alloc(void *cache, unsigned int flags)
{
    size_t size = 4096;
    if (cache != NULL) {
        size = ((const kb_kmem_cache_t *)cache)->size;
    }
    return kb_kmalloc(size, flags);
}

void kb_kmem_cache_free(void *cache, void *ptr)
{
    (void)cache;
    kb_kfree(ptr);
}

void kb_kfree(void *ptr)
{
    if (is_kernel_non_heap_pointer(ptr)) {
        if (trace_memory() && ptr != NULL) {
            fprintf(stderr, "kobox-shim: kfree ignored sentinel ptr=%p\n", ptr);
        }
        return;
    }
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

void *__kmalloc_node(size_t size, unsigned int flags, int node)
{
    return kb_kmalloc_node(size, flags, node);
}

void *kmem_cache_alloc(void *cache, unsigned int flags)
{
    return kb_kmem_cache_alloc(cache, flags);
}

void *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags, void *ctor)
{
    return kb_kmem_cache_create(name, size, align, flags, ctor);
}

void kmem_cache_destroy(void *cache)
{
    kb_kmem_cache_destroy(cache);
}

void kmem_cache_free(void *cache, void *ptr)
{
    kb_kmem_cache_free(cache, ptr);
}

void kfree(void *ptr)
{
    kb_kfree(ptr);
}
