#include "kobox/shim.h"

#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
/* A few narrow host-only smoke binaries link the allocator without loader
 * context support.  Their caches have no module ctor, so keep these optional
 * while using the real GS bridge in the full runtime. */
extern unsigned long kb_module_kernel_gs_for_address(const void *address)
    __attribute__((weak));
extern void kb_linux_call_void_ptr(void (*fn)(void *), void *arg)
    __attribute__((weak));
extern void kb_linux_call_void_ptr_gs(
    void (*fn)(void *),
    void *arg,
    unsigned long kernel_gs) __attribute__((weak));
#endif

enum {
    KB_LINUX___GFP_ZERO = 0x100u,
    /* Linux 6.12 struct kmem_cache_args::ctor. */
    KB_KMEM_CACHE_ARGS_CTOR_OFFSET = 24u,
    KB_KMALLOC_GUARD_SIZE = 256u,
    KB_KMALLOC_RECORD_ALIGN = 16u,
    KB_KMALLOC_MMAP_ALIGN = 4096u,
    KB_KMALLOC_ARENA_CHUNK_MIN = 1024u * 1024u,
};

typedef struct kb_kmem_cache_page {
    void *addr;
    struct kb_kmem_cache_page *next;
} kb_kmem_cache_page_t;

typedef struct kb_mempool {
    uint64_t magic;
    int minimum;
    int current;
    void *(*alloc_fn)(unsigned int gfp, void *pool_data);
    void (*free_fn)(void *element, void *pool_data);
    void *pool_data;
    void **elements;
    atomic_flag lock;
} kb_mempool_t;

typedef struct kb_shrinker_record {
    void *shrinker;
    int registered;
    struct kb_shrinker_record *next;
} kb_shrinker_record_t;

typedef struct kb_shrink_control {
    unsigned int gfp_mask;
    int nid;
    unsigned long nr_to_scan;
    unsigned long nr_scanned;
    void *memcg;
} kb_shrink_control_t;

#define KB_MEMPOOL_MAGIC UINT64_C(0x4b424d454d504f4f)

static void mempool_lock(kb_mempool_t *pool)
{
    while (atomic_flag_test_and_set_explicit(&pool->lock, memory_order_acquire)) {
    }
}

static void mempool_unlock(kb_mempool_t *pool)
{
    atomic_flag_clear_explicit(&pool->lock, memory_order_release);
}

static unsigned long mempool_callback_gs(const void *callback)
{
#if defined(__GNUC__) || defined(__clang__)
    if (kb_module_kernel_gs_for_address != NULL) {
        return kb_module_kernel_gs_for_address(callback);
    }
#else
    (void)callback;
#endif
    return 0;
}

static void *mempool_call_alloc(kb_mempool_t *pool, unsigned int gfp)
{
    if (pool == NULL || pool->alloc_fn == NULL) {
        return NULL;
    }
    const unsigned long kernel_gs =
        mempool_callback_gs((const void *)pool->alloc_fn);
    unsigned long old_gs = 0;
    const int has_gs = kernel_gs != 0 &&
        kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    void *result = pool->alloc_fn(gfp, pool->pool_data);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static void mempool_call_free(kb_mempool_t *pool, void *element)
{
    if (pool == NULL || pool->free_fn == NULL || element == NULL) {
        return;
    }
    const unsigned long kernel_gs =
        mempool_callback_gs((const void *)pool->free_fn);
    unsigned long old_gs = 0;
    const int has_gs = kernel_gs != 0 &&
        kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    pool->free_fn(element, pool->pool_data);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
}

void *kb_mempool_create_node(
    int minimum,
    void *(*alloc_fn)(unsigned int gfp, void *pool_data),
    void (*free_fn)(void *element, void *pool_data),
    void *pool_data,
    unsigned int gfp,
    int node_id)
{
    (void)node_id;
    if (minimum < 0 || alloc_fn == NULL || free_fn == NULL) {
        return NULL;
    }
    kb_mempool_t *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->elements = minimum == 0 ? NULL :
        calloc((size_t)minimum, sizeof(*pool->elements));
    if (minimum != 0 && pool->elements == NULL) {
        free(pool);
        return NULL;
    }
    pool->magic = KB_MEMPOOL_MAGIC;
    pool->minimum = minimum;
    pool->alloc_fn = alloc_fn;
    pool->free_fn = free_fn;
    pool->pool_data = pool_data;
    atomic_flag_clear_explicit(&pool->lock, memory_order_release);
    while (pool->current < pool->minimum) {
        void *element = mempool_call_alloc(pool, gfp);
        if (element == NULL) {
            kb_mempool_destroy(pool);
            return NULL;
        }
        pool->elements[pool->current++] = element;
    }
    return pool;
}

void *kb_mempool_create(
    int minimum,
    void *(*alloc_fn)(unsigned int gfp, void *pool_data),
    void (*free_fn)(void *element, void *pool_data),
    void *pool_data)
{
    return kb_mempool_create_node(
        minimum,
        alloc_fn,
        free_fn,
        pool_data,
        0,
        -1);
}

void *kb_mempool_alloc(void *handle, unsigned int gfp)
{
    kb_mempool_t *pool = handle;
    if (pool == NULL || pool->magic != KB_MEMPOOL_MAGIC) {
        return NULL;
    }
    void *element = mempool_call_alloc(pool, gfp);
    if (element != NULL) {
        return element;
    }
    mempool_lock(pool);
    if (pool->current != 0) {
        element = pool->elements[--pool->current];
        pool->elements[pool->current] = NULL;
    }
    mempool_unlock(pool);
    return element;
}

void kb_mempool_free(void *element, void *handle)
{
    kb_mempool_t *pool = handle;
    if (element == NULL || pool == NULL || pool->magic != KB_MEMPOOL_MAGIC) {
        return;
    }
    mempool_lock(pool);
    if (pool->current < pool->minimum) {
        pool->elements[pool->current++] = element;
        element = NULL;
    }
    mempool_unlock(pool);
    if (element != NULL) {
        mempool_call_free(pool, element);
    }
}

void kb_mempool_destroy(void *handle)
{
    kb_mempool_t *pool = handle;
    if (pool == NULL || pool->magic != KB_MEMPOOL_MAGIC) {
        return;
    }
    while (pool->current != 0) {
        void *element = pool->elements[--pool->current];
        pool->elements[pool->current] = NULL;
        mempool_call_free(pool, element);
    }
    pool->magic = 0;
    free(pool->elements);
    free(pool);
}

void *kb_mempool_alloc_slab(unsigned int gfp, void *pool_data)
{
    return kb_kmem_cache_alloc(pool_data, gfp);
}

void kb_mempool_free_slab(void *element, void *pool_data)
{
    kb_kmem_cache_free(pool_data, element);
}

void *kb_mempool_kmalloc(unsigned int gfp, void *pool_data)
{
    return kb_kmalloc((size_t)(uintptr_t)pool_data, gfp);
}

void kb_mempool_kfree(void *element, void *pool_data)
{
    (void)pool_data;
    kb_kfree(element);
}

typedef struct kb_kmem_cache_free_slot {
    struct kb_kmem_cache_free_slot *next;
} kb_kmem_cache_free_slot_t;

typedef struct kb_kmem_cache {
    size_t size;
    size_t align;
    size_t slot_size;
    void (*ctor)(void *);
    kb_kmem_cache_page_t *pages;
    kb_kmem_cache_free_slot_t *free_slots;
} kb_kmem_cache_t;

typedef struct kb_heap_allocation {
    void *ptr;
    void *raw;
    size_t size;
    size_t raw_size;
    size_t guard_offset;
    int mmap_backed;
    int arena_backed;
    int page_backed;
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
static kb_memory_hotpath_profile_t memory_hotpath_profile;

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE && defined(__x86_64__)
static uint64_t memory_profile_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
#define MEMORY_PROFILE_BEGIN(name) const uint64_t name = memory_profile_tsc()
#define MEMORY_PROFILE_END(field, name) \
    (memory_hotpath_profile.field += memory_profile_tsc() - (name))
#define MEMORY_PROFILE_INCREMENT(field) (++memory_hotpath_profile.field)
#else
#define MEMORY_PROFILE_BEGIN(name) ((void)0)
#define MEMORY_PROFILE_END(field, name) ((void)0)
#define MEMORY_PROFILE_INCREMENT(field) ((void)0)
#endif

void kb_memory_hotpath_profile_reset(void)
{
    memset(&memory_hotpath_profile, 0, sizeof(memory_hotpath_profile));
}

void kb_memory_hotpath_profile_snapshot(kb_memory_hotpath_profile_t *out_profile)
{
    if (out_profile != NULL) {
        *out_profile = memory_hotpath_profile;
    }
}
static kb_shrinker_record_t *shrinker_records;
static atomic_flag shrinker_records_lock = ATOMIC_FLAG_INIT;

enum {
    KB_SHRINKER_BYTES = 256,
    KB_SHRINKER_COUNT_OBJECTS_OFFSET = 0,
    KB_SHRINKER_SCAN_OBJECTS_OFFSET = sizeof(void *),
    KB_SHRINKER_SNAPSHOT_MAX = 64,
};

static void shrinker_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
        &shrinker_records_lock, memory_order_acquire))
    {
    }
}

static void shrinker_unlock(void)
{
    atomic_flag_clear_explicit(&shrinker_records_lock, memory_order_release);
}

void *kb_shrinker_alloc(unsigned int flags, const char *format, ...)
{
    (void)format;
    void *shrinker = kb_kzalloc(KB_SHRINKER_BYTES, flags);
    if (shrinker == NULL) {
        return NULL;
    }
    kb_shrinker_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        kb_kfree(shrinker);
        return NULL;
    }
    record->shrinker = shrinker;
    shrinker_lock();
    record->next = shrinker_records;
    shrinker_records = record;
    shrinker_unlock();
    return shrinker;
}

void kb_shrinker_register(void *shrinker)
{
    if (shrinker == NULL) {
        return;
    }
    shrinker_lock();
    for (kb_shrinker_record_t *record = shrinker_records;
         record != NULL;
         record = record->next)
    {
        if (record->shrinker == shrinker) {
            record->registered = 1;
            break;
        }
    }
    shrinker_unlock();
}

void kb_shrinker_free(void *shrinker)
{
    if (shrinker == NULL) {
        return;
    }
    kb_shrinker_record_t *removed = NULL;
    shrinker_lock();
    kb_shrinker_record_t **cursor = &shrinker_records;
    while (*cursor != NULL) {
        if ((*cursor)->shrinker == shrinker) {
            removed = *cursor;
            *cursor = removed->next;
            removed->next = NULL;
            break;
        }
        cursor = &(*cursor)->next;
    }
    shrinker_unlock();
    if (removed == NULL) {
        return;
    }
    kb_kfree(shrinker);
    free(removed);
}

unsigned long kb_shrinker_reclaim(unsigned long nr_to_scan, unsigned int gfp_mask)
{
    if (nr_to_scan == 0) {
        return 0;
    }
    void *snapshot[KB_SHRINKER_SNAPSHOT_MAX] = {0};
    size_t count = 0;
    shrinker_lock();
    for (kb_shrinker_record_t *record = shrinker_records;
         record != NULL && count < KB_SHRINKER_SNAPSHOT_MAX;
         record = record->next)
    {
        if (record->registered) {
            snapshot[count++] = record->shrinker;
        }
    }
    shrinker_unlock();

    unsigned long reclaimed = 0;
    for (size_t i = 0; i < count && reclaimed < nr_to_scan; ++i) {
        void *shrinker = snapshot[i];
        unsigned long (*count_objects)(void *, void *) = NULL;
        unsigned long (*scan_objects)(void *, void *) = NULL;
        memcpy(
            &count_objects,
            (const unsigned char *)shrinker + KB_SHRINKER_COUNT_OBJECTS_OFFSET,
            sizeof(count_objects));
        memcpy(
            &scan_objects,
            (const unsigned char *)shrinker + KB_SHRINKER_SCAN_OBJECTS_OFFSET,
            sizeof(scan_objects));
        if (scan_objects == NULL) {
            continue;
        }
        kb_shrink_control_t control = {
            .gfp_mask = gfp_mask,
            .nid = -1,
            .nr_to_scan = nr_to_scan - reclaimed,
        };
        const unsigned long kernel_gs =
            mempool_callback_gs((const void *)scan_objects);
        unsigned long old_gs = 0;
        const int has_gs = kernel_gs != 0 &&
            kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
        unsigned long available = count_objects == NULL ? control.nr_to_scan :
            count_objects(shrinker, &control);
        unsigned long scanned = 0;
        if (available != 0) {
            scanned = scan_objects(shrinker, &control);
        }
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (scanned != ULONG_MAX && scanned <= nr_to_scan - reclaimed) {
            reclaimed += scanned;
        }
    }
    return reclaimed;
}

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
        MEMORY_PROFILE_INCREMENT(arena_chunk_search_steps);
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
            MEMORY_PROFILE_INCREMENT(arena_free_search_steps);
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
    if (ptr == NULL) {
        fprintf(stderr, "kobox-shim: arena alloc failed size=%zu\n", size);
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
    if (record->page_backed) {
        return 1;
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
        MEMORY_PROFILE_INCREMENT(allocation_search_steps);
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
    MEMORY_PROFILE_BEGIN(profile_start);
    MEMORY_PROFILE_INCREMENT(kmalloc_calls);
    size_t actual_size = size == 0 ? 1 : size;
    if (actual_size >= KB_KMALLOC_MMAP_ALIGN && kmalloc_arena_enabled()) {
        void *page = kb_alloc_pages_exact(actual_size, flags);
        if (page != NULL) {
            kb_heap_allocation_t *record = malloc(sizeof(*record));
            if (record != NULL) {
                memset(record, 0, sizeof(*record));
                record->ptr = page;
                record->raw = page;
                record->size = actual_size;
                record->raw_size = actual_size;
                record->page_backed = 1;
                track_heap_allocation(record);
            }
            if ((flags & KB_LINUX___GFP_ZERO) != 0) {
                memset(page, 0, actual_size);
            }
            if (trace_memory()) {
                fprintf(stderr, "kobox-shim: kmalloc page size=%zu flags=0x%x ptr=%p\n", size, flags, page);
            }
            MEMORY_PROFILE_END(kmalloc_cycles, profile_start);
            return page;
        }
    }

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
        fprintf(stderr, "kobox-shim: kmalloc failed size=%zu raw_size=%zu flags=0x%x\n", size, raw_size, flags);
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
    MEMORY_PROFILE_END(kmalloc_cycles, profile_start);
    return ptr;
}

void *kb_kzalloc(size_t size, unsigned int flags)
{
    MEMORY_PROFILE_BEGIN(profile_start);
    MEMORY_PROFILE_INCREMENT(kzalloc_calls);
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
        fprintf(stderr, "kobox-shim: kzalloc failed size=%zu raw_size=%zu flags=0x%x\n", size, raw_size, flags);
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
    MEMORY_PROFILE_END(kzalloc_cycles, profile_start);
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
    kb_kmem_cache_t *cache = kb_kzalloc(sizeof(*cache), 0);
    if (cache == NULL) {
        return NULL;
    }
    cache->size = size == 0 ? 1 : size;
    memcpy(&cache->ctor, &ctor, sizeof(cache->ctor));
    cache->align = align < sizeof(void *) ? sizeof(void *) : align;
    if ((cache->align & (cache->align - 1u)) != 0) {
        cache->align = sizeof(void *);
    }
    const size_t minimum_slot = cache->size < sizeof(kb_kmem_cache_free_slot_t) ?
        sizeof(kb_kmem_cache_free_slot_t) : cache->size;
    cache->slot_size = align_up_size(minimum_slot, cache->align);
    if (cache->slot_size > KB_KMALLOC_MMAP_ALIGN) {
        kb_kfree(cache);
        return NULL;
    }
    if (trace_memory()) {
        fprintf(stderr, "kobox-shim: kmem_cache_create size=%zu align=%zu cache=%p\n", size, align, (void *)cache);
    }
    return cache;
}

void *kb_kmem_cache_create_args(const char *name, unsigned int object_size, void *args, unsigned int flags)
{
    size_t align = 0;
    void *ctor = NULL;
    if (args != NULL) {
        memcpy(&align, args, sizeof(align));
        memcpy(
            &ctor,
            (const unsigned char *)args + KB_KMEM_CACHE_ARGS_CTOR_OFFSET,
            sizeof(ctor));
    }
    return kb_kmem_cache_create(name, object_size, align, flags, ctor);
}

void kb_kmem_cache_destroy(void *cache)
{
    kb_kmem_cache_t *typed = cache;
    if (typed == NULL) {
        return;
    }
    while (typed->pages != NULL) {
        kb_kmem_cache_page_t *page = typed->pages;
        typed->pages = page->next;
        kb_free_pages_exact(page->addr, KB_KMALLOC_MMAP_ALIGN);
        free(page);
    }
    kb_kfree(typed);
}

void *kb_kmem_cache_alloc(void *cache, unsigned int flags)
{
    MEMORY_PROFILE_BEGIN(profile_start);
    MEMORY_PROFILE_INCREMENT(cache_alloc_calls);
    kb_kmem_cache_t *typed = cache;
    if (typed == NULL || typed->slot_size == 0 || typed->slot_size > KB_KMALLOC_MMAP_ALIGN) {
        return NULL;
    }
    if (typed->free_slots == NULL) {
        void *page_addr = kb_alloc_pages_exact(KB_KMALLOC_MMAP_ALIGN, flags);
        if (page_addr == NULL) {
            return NULL;
        }
        kb_kmem_cache_page_t *page = malloc(sizeof(*page));
        if (page == NULL) {
            kb_free_pages_exact(page_addr, KB_KMALLOC_MMAP_ALIGN);
            return NULL;
        }
        page->addr = page_addr;
        page->next = typed->pages;
        typed->pages = page;

        const size_t slot_count = KB_KMALLOC_MMAP_ALIGN / typed->slot_size;
        for (size_t index = slot_count; index != 0; index--) {
            kb_kmem_cache_free_slot_t *slot =
                (kb_kmem_cache_free_slot_t *)((unsigned char *)page_addr + ((index - 1u) * typed->slot_size));
            slot->next = typed->free_slots;
            typed->free_slots = slot;
        }
    }
    kb_kmem_cache_free_slot_t *slot = typed->free_slots;
    typed->free_slots = slot->next;
    if ((flags & KB_LINUX___GFP_ZERO) != 0 || typed->ctor != NULL) {
        memset(slot, 0, typed->size);
    }
    if (typed->ctor != NULL) {
        const unsigned long kernel_gs =
            kb_module_kernel_gs_for_address != NULL ?
                kb_module_kernel_gs_for_address((const void *)typed->ctor) :
                0;
        if (kernel_gs != 0 && kb_linux_call_void_ptr_gs != NULL) {
            kb_linux_call_void_ptr_gs(typed->ctor, slot, kernel_gs);
        } else if (kb_linux_call_void_ptr != NULL) {
            kb_linux_call_void_ptr(typed->ctor, slot);
        } else {
            typed->ctor(slot);
        }
    }
    MEMORY_PROFILE_END(cache_alloc_cycles, profile_start);
    return slot;
}

void kb_kmem_cache_free(void *cache, void *ptr)
{
    MEMORY_PROFILE_BEGIN(profile_start);
    MEMORY_PROFILE_INCREMENT(cache_free_calls);
    kb_kmem_cache_t *typed = cache;
    if (typed == NULL || ptr == NULL) {
        return;
    }
    kb_kmem_cache_free_slot_t *slot = ptr;
    slot->next = typed->free_slots;
    typed->free_slots = slot;
    MEMORY_PROFILE_END(cache_free_cycles, profile_start);
}

void kb_kfree(void *ptr)
{
    MEMORY_PROFILE_BEGIN(profile_start);
    MEMORY_PROFILE_INCREMENT(kfree_calls);
    if (is_kernel_non_heap_pointer(ptr)) {
        if (trace_memory() && ptr != NULL) {
            fprintf(stderr, "kobox-shim: kfree ignored sentinel ptr=%p\n", ptr);
        }
        MEMORY_PROFILE_END(kfree_cycles, profile_start);
        return;
    }
    kb_heap_allocation_t *record = take_heap_allocation(ptr);
    if (record == NULL) {
        if (trace_memory()) {
            fprintf(stderr, "kobox-shim: kfree ignored untracked ptr=%p\n", ptr);
        }
        MEMORY_PROFILE_END(kfree_cycles, profile_start);
        return;
    }
    if (!allocation_guards_ok(record)) {
        fprintf(stderr, "kobox-shim: kmalloc guard corrupted ptr=%p size=%zu\n", ptr, record->size);
    }
    void *raw = record->raw;
    size_t raw_size = record->raw_size;
    int mmap_backed = record->mmap_backed;
    int arena_backed = record->arena_backed;
    int page_backed = record->page_backed;
    if (page_backed) {
        kb_free_pages_exact(ptr, raw_size);
        free(record);
        MEMORY_PROFILE_END(kfree_cycles, profile_start);
        return;
    }
    if (arena_backed) {
        if (raw_size >= sizeof(kb_heap_arena_free_block_t)) {
            kb_heap_arena_free_block_t *block = (kb_heap_arena_free_block_t *)raw;
            block->size = raw_size;
            block->next = heap_arena_free_blocks;
            heap_arena_free_blocks = block;
        }
        MEMORY_PROFILE_END(kfree_cycles, profile_start);
        return;
    }
    kernel_heap_os_free(raw, raw_size, mmap_backed);
    MEMORY_PROFILE_END(kfree_cycles, profile_start);
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
