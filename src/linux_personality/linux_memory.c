#include "kobox/shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

enum {
    KB_LINUX___GFP_ZERO = 0x100u,
    KB_KMALLOC_GUARD_SIZE = 256u,
    KB_KMALLOC_RECORD_ALIGN = 16u,
    KB_KMALLOC_MMAP_ALIGN = 4096u,
    KB_KMALLOC_ARENA_CHUNK_MIN = 1024u * 1024u,
};

typedef struct kb_kmem_cache {
    size_t size;
    size_t align;
} kb_kmem_cache_t;

typedef struct kb_heap_allocation {
    void *ptr;
    void *raw;
    size_t size;
    size_t raw_size;
    size_t guard_offset;
    int mmap_backed;
    int arena_backed;
    struct kb_heap_allocation *next;
} kb_heap_allocation_t;

typedef struct kb_heap_arena_chunk {
    struct kb_heap_arena_chunk *next;
    size_t capacity;
    size_t used;
} kb_heap_arena_chunk_t;

typedef struct kb_heap_arena_free_block {
    struct kb_heap_arena_free_block *next;
    size_t size;
} kb_heap_arena_free_block_t;

static kb_heap_allocation_t *heap_allocations;
static kb_heap_arena_chunk_t *heap_arena_chunks;
static kb_heap_arena_free_block_t *heap_arena_free_blocks;

static int trace_memory(void)
{
#if defined(__pachaos__)
    return 0;
#else
    return getenv("KOBOX_TRACE_SHIMS") != NULL;
#endif
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

static int kmalloc_mmap_enabled(void)
{
#if defined(__pachaos__)
    return 0;
#else
    const char *override = getenv("KOBOX_KMALLOC_MMAP");
    if (override != NULL && override[0] != '\0') {
        return strcmp(override, "0") != 0;
    }
    return 0;
#endif
}

static int kmalloc_arena_enabled(void)
{
#if defined(__pachaos__)
    return 1;
#else
    const char *override = getenv("KOBOX_KMALLOC_ARENA");
    if (override != NULL && override[0] != '\0') {
        return strcmp(override, "0") != 0;
    }
    const char *backend = getenv("KOBOX_DEVICE_BACKEND");
    return backend != NULL && (strcmp(backend, "pachaos") == 0 || strcmp(backend, "pachaos_capsule") == 0);
#endif
}

static size_t align_up_size(size_t value, size_t align)
{
    size_t mask = align - 1u;
    return (value + mask) & ~mask;
}

static void *kernel_heap_arena_alloc(size_t size, size_t *allocated_size)
{
    size_t aligned_size = align_up_size(size, KB_KMALLOC_RECORD_ALIGN);
    size_t header_size = align_up_size(sizeof(kb_heap_arena_chunk_t), KB_KMALLOC_RECORD_ALIGN);
    for (kb_heap_arena_chunk_t *chunk = heap_arena_chunks; chunk != NULL; chunk = chunk->next) {
        if (aligned_size <= chunk->capacity - chunk->used) {
            unsigned char *data = (unsigned char *)chunk + header_size + chunk->used;
            chunk->used += aligned_size;
            if (allocated_size != NULL) {
                *allocated_size = aligned_size;
            }
            return data;
        }
    }

    size_t capacity = aligned_size > KB_KMALLOC_ARENA_CHUNK_MIN ? aligned_size : KB_KMALLOC_ARENA_CHUNK_MIN;
    if (capacity > SIZE_MAX - header_size) {
        return NULL;
    }
    size_t chunk_size = header_size + capacity;
#if defined(_WIN32)
    kb_heap_arena_chunk_t *chunk = malloc(chunk_size);
#else
    void *chunk_memory = mmap(NULL, chunk_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    kb_heap_arena_chunk_t *chunk = chunk_memory == MAP_FAILED ? NULL : (kb_heap_arena_chunk_t *)chunk_memory;
#endif
    if (chunk == NULL) {
        return NULL;
    }
    chunk->capacity = capacity;
    chunk->used = aligned_size;
    chunk->next = heap_arena_chunks;
    heap_arena_chunks = chunk;
    if (allocated_size != NULL) {
        *allocated_size = aligned_size;
    }
    return (unsigned char *)chunk + header_size;
}

static void *kernel_heap_os_alloc(size_t size, size_t *allocated_size, int *mmap_backed, int *arena_backed)
{
    if (allocated_size != NULL) {
        *allocated_size = size;
    }
    if (mmap_backed != NULL) {
        *mmap_backed = 0;
    }
    if (arena_backed != NULL) {
        *arena_backed = 0;
    }
    if (kmalloc_arena_enabled()) {
        kb_heap_arena_free_block_t **cursor = &heap_arena_free_blocks;
        while (*cursor != NULL) {
            kb_heap_arena_free_block_t *block = *cursor;
            if (block->size >= size) {
                *cursor = block->next;
                if (allocated_size != NULL) {
                    *allocated_size = block->size;
                }
                if (arena_backed != NULL) {
                    *arena_backed = 1;
                }
                return block;
            }
            cursor = &block->next;
        }
        void *ptr = kernel_heap_arena_alloc(size, allocated_size);
        if (ptr != NULL && arena_backed != NULL) {
            *arena_backed = 1;
        }
        return ptr;
    }
#if !defined(_WIN32)
    if (kmalloc_mmap_enabled()) {
        size_t mmap_size = align_up_size(size, KB_KMALLOC_MMAP_ALIGN);
        void *ptr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            return NULL;
        }
        if (allocated_size != NULL) {
            *allocated_size = mmap_size;
        }
        if (mmap_backed != NULL) {
            *mmap_backed = 1;
        }
        return ptr;
    }
#endif
    return malloc(size);
}

static void kernel_heap_os_free(void *ptr, size_t size, int mmap_backed)
{
    if (ptr == NULL) {
        return;
    }
#if !defined(_WIN32)
    if (mmap_backed) {
        (void)munmap(ptr, size);
        return;
    }
#else
    (void)size;
    (void)mmap_backed;
#endif
    free(ptr);
}

static void fill_guard(unsigned char *guard, size_t size, unsigned char value)
{
    memset(guard, value, size);
}

static int guard_matches(const unsigned char *guard, size_t size, unsigned char value)
{
    for (size_t i = 0; i < size; i++) {
        if (guard[i] != value) {
            return 0;
        }
    }
    return 1;
}

static int allocation_guards_ok(const kb_heap_allocation_t *record)
{
    if (record == NULL || record->raw == NULL || record->ptr == NULL) {
        return 0;
    }
    const unsigned char *raw = (const unsigned char *)record->raw + record->guard_offset;
    const unsigned char *user = record->ptr;
    return guard_matches(raw, KB_KMALLOC_GUARD_SIZE, 0xa5u) &&
        guard_matches(user + record->size, KB_KMALLOC_GUARD_SIZE, 0x5au);
}

static void track_heap_allocation(kb_heap_allocation_t *record)
{
    if (record == NULL) {
        return;
    }
    record->next = heap_allocations;
    heap_allocations = record;
}

static kb_heap_allocation_t *take_heap_allocation(void *ptr)
{
    kb_heap_allocation_t **cursor = &heap_allocations;
    while (*cursor != NULL) {
        kb_heap_allocation_t *record = *cursor;
        if (record->ptr == ptr) {
            *cursor = record->next;
            record->next = NULL;
            return record;
        }
        cursor = &record->next;
    }
    return NULL;
}

size_t kb_kmalloc_usable_size(const void *ptr)
{
    for (kb_heap_allocation_t *record = heap_allocations; record != NULL; record = record->next) {
        if (record->ptr == ptr) {
            return record->size;
        }
    }
    return 0;
}

void *kb_kmalloc(size_t size, unsigned int flags)
{
    size_t actual_size = size == 0 ? 1 : size;
    size_t record_size = align_up_size(sizeof(kb_heap_allocation_t), KB_KMALLOC_RECORD_ALIGN);
    if (record_size > SIZE_MAX - (2u * KB_KMALLOC_GUARD_SIZE) ||
        actual_size > SIZE_MAX - record_size - (2u * KB_KMALLOC_GUARD_SIZE))
    {
        return NULL;
    }
    size_t raw_size = record_size + actual_size + (2u * KB_KMALLOC_GUARD_SIZE);
    int mmap_backed = 0;
    int arena_backed = 0;
    size_t allocated_size = raw_size;
    unsigned char *raw = kernel_heap_os_alloc(raw_size, &allocated_size, &mmap_backed, &arena_backed);
    if (raw == NULL) {
        return NULL;
    }
    kb_heap_allocation_t *record = (kb_heap_allocation_t *)raw;
    memset(record, 0, sizeof(*record));
    unsigned char *guard = raw + record_size;
    fill_guard(guard, KB_KMALLOC_GUARD_SIZE, 0xa5u);
    fill_guard(guard + KB_KMALLOC_GUARD_SIZE + actual_size, KB_KMALLOC_GUARD_SIZE, 0x5au);
    void *ptr = guard + KB_KMALLOC_GUARD_SIZE;
    record->ptr = ptr;
    record->raw = raw;
    record->size = actual_size;
    record->raw_size = allocated_size;
    record->guard_offset = record_size;
    record->mmap_backed = mmap_backed;
    record->arena_backed = arena_backed;
    track_heap_allocation(record);
    if (ptr != NULL && (flags & KB_LINUX___GFP_ZERO) != 0) {
        memset(ptr, 0, actual_size);
    }
    if (trace_memory()) {
        fprintf(stderr, "kobox-shim: kmalloc size=%zu flags=0x%x ptr=%p\n", size, flags, ptr);
    }
    return ptr;
}

void *kb_kzalloc(size_t size, unsigned int flags)
{
    (void)flags;
    size_t actual_size = size == 0 ? 1 : size;
    size_t record_size = align_up_size(sizeof(kb_heap_allocation_t), KB_KMALLOC_RECORD_ALIGN);
    if (record_size > SIZE_MAX - (2u * KB_KMALLOC_GUARD_SIZE) ||
        actual_size > SIZE_MAX - record_size - (2u * KB_KMALLOC_GUARD_SIZE))
    {
        return NULL;
    }
    size_t raw_size = record_size + actual_size + (2u * KB_KMALLOC_GUARD_SIZE);
    int mmap_backed = 0;
    int arena_backed = 0;
    size_t allocated_size = raw_size;
    unsigned char *raw = kernel_heap_os_alloc(raw_size, &allocated_size, &mmap_backed, &arena_backed);
    if (raw == NULL) {
        return NULL;
    }
    kb_heap_allocation_t *record = (kb_heap_allocation_t *)raw;
    memset(record, 0, sizeof(*record));
    unsigned char *guard = raw + record_size;
    fill_guard(guard, KB_KMALLOC_GUARD_SIZE, 0xa5u);
    memset(guard + KB_KMALLOC_GUARD_SIZE, 0, actual_size);
    fill_guard(guard + KB_KMALLOC_GUARD_SIZE + actual_size, KB_KMALLOC_GUARD_SIZE, 0x5au);
    void *ptr = guard + KB_KMALLOC_GUARD_SIZE;
    record->ptr = ptr;
    record->raw = raw;
    record->size = actual_size;
    record->raw_size = allocated_size;
    record->guard_offset = record_size;
    record->mmap_backed = mmap_backed;
    record->arena_backed = arena_backed;
    track_heap_allocation(record);
    return ptr;
}

void *kb_kmalloc_trace(void *cache, unsigned int flags, size_t size)
{
    (void)cache;
    return kb_kmalloc(size, flags);
}

void *kb_kmalloc_cache_noprof(void *cache, unsigned int flags, size_t size)
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
    kb_kmem_cache_t *cache = kb_kzalloc(sizeof(*cache), 0);
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

void *kb_kmem_cache_create_args(const char *name, unsigned int object_size, void *args, unsigned int flags)
{
    (void)args;
    return kb_kmem_cache_create(name, object_size, 0, flags, NULL);
}

void kb_kmem_cache_destroy(void *cache)
{
    kb_kfree(cache);
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
    kb_heap_allocation_t *record = take_heap_allocation(ptr);
    if (record == NULL) {
        if (trace_memory()) {
            fprintf(stderr, "kobox-shim: kfree ignored untracked ptr=%p\n", ptr);
        }
        return;
    }
    if (!allocation_guards_ok(record)) {
        fprintf(stderr, "kobox-shim: kmalloc guard corrupted ptr=%p size=%zu\n", ptr, record->size);
    }
    void *raw = record->raw;
    size_t raw_size = record->raw_size;
    int mmap_backed = record->mmap_backed;
    int arena_backed = record->arena_backed;
    if (arena_backed) {
        if (raw_size >= sizeof(kb_heap_arena_free_block_t)) {
            kb_heap_arena_free_block_t *block = (kb_heap_arena_free_block_t *)raw;
            block->size = raw_size;
            block->next = heap_arena_free_blocks;
            heap_arena_free_blocks = block;
        }
        return;
    }
    kernel_heap_os_free(raw, raw_size, mmap_backed);
}

void *kb_krealloc_managed(void *ptr, size_t size, unsigned int flags)
{
    if (ptr == NULL) {
        return kb_kmalloc(size, flags);
    }
    if (size == 0) {
        kb_kfree(ptr);
        return NULL;
    }
    size_t old_size = kb_kmalloc_usable_size(ptr);
    if (old_size == 0) {
        return kb_kmalloc(size, flags);
    }
    void *next = kb_kmalloc(size, flags);
    if (next == NULL) {
        return NULL;
    }
    size_t copied = old_size < size ? old_size : size;
    memcpy(next, ptr, copied);
    kb_kfree(ptr);
    return next;
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
