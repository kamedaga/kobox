#include "linux_subsystem/dma/dma.h"
#include "kobox/shim.h"

#include <string.h>

typedef struct kb_subsystem_dma_mapping {
    void *cpu_addr;
    uint64_t dma_addr;
    uint64_t size;
    kb_device_t *device;
    uint8_t borrowed_window;
    uint8_t persistent_allocation;
    struct kb_subsystem_dma_mapping *next;
} kb_subsystem_dma_mapping_t;

typedef struct kb_subsystem_dma_window {
    kb_device_backend_t *backend;
    kb_device_t *device;
    uintptr_t scope_start;
    size_t scope_size;
    kb_dma_dir_t direction;
    uintptr_t mapped_start;
    size_t mapped_size;
    size_t mapped_page_count;
    uint64_t page_dma[KB_DEVICE_DMA_MAPPING_MAX_PAGES];
    size_t borrowers;
    uint8_t active;
    uint8_t mapped;
} kb_subsystem_dma_window_t;

enum {
    KB_SUBSYSTEM_DMA_PAGE_SIZE = 4096,
};

static kb_subsystem_dma_mapping_t *dma_mappings;
static kb_subsystem_dma_window_t dma_window;
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
static kb_subsystem_dma_window_profile_t dma_window_profile;
#endif

static uint64_t dma_profile_begin(void)
{
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE && defined(__x86_64__)
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

static kb_status_t remember_dma_mapping_full(
    void *cpu_addr,
    uint64_t dma_addr,
    uint64_t size,
    kb_device_t *device,
    int borrowed_window,
    int persistent_allocation)
{
    if (cpu_addr == NULL || size == 0) {
        return KB_ERR_INVALID;
    }
    kb_subsystem_dma_mapping_t *entry = kb_kzalloc(sizeof(*entry), 0);
    if (entry == NULL) {
        return KB_ERR_NOMEM;
    }
    entry->cpu_addr = cpu_addr;
    entry->dma_addr = dma_addr;
    entry->size = size;
    entry->device = device;
    entry->borrowed_window = borrowed_window != 0;
    entry->persistent_allocation = persistent_allocation != 0;
    entry->next = dma_mappings;
    dma_mappings = entry;
    return KB_OK;
}

static kb_status_t remember_dma_mapping(
    void *cpu_addr,
    uint64_t dma_addr,
    uint64_t size,
    kb_device_t *device,
    int borrowed_window)
{
    return remember_dma_mapping_full(
        cpu_addr, dma_addr, size, device, borrowed_window, 0);
}

static kb_subsystem_dma_mapping_t *take_dma_mapping(uint64_t dma_addr)
{
    kb_subsystem_dma_mapping_t **cursor = &dma_mappings;
    while (*cursor != NULL) {
        kb_subsystem_dma_mapping_t *entry = *cursor;
        if (entry->dma_addr == dma_addr) {
            *cursor = entry->next;
            entry->next = NULL;
            return entry;
        }
        cursor = &entry->next;
    }
    return NULL;
}

static void dma_window_release_mapping(void)
{
    if (!dma_window.mapped || dma_window.borrowers != 0) {
        return;
    }
    const kb_device_backend_ops_t *ops =
        kb_device_backend_get_ops(dma_window.backend);
    if (ops != NULL && ops->dma_unmap != NULL) {
        ops->dma_unmap(
            dma_window.device,
            dma_window.page_dma[0],
            dma_window.mapped_size,
            dma_window.direction);
    }
    dma_window.mapped = 0;
    dma_window.mapped_start = 0;
    dma_window.mapped_size = 0;
    dma_window.mapped_page_count = 0;
    memset(dma_window.page_dma, 0, sizeof(dma_window.page_dma));
}

static kb_status_t dma_window_begin_common(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction)
{
    if (backend == NULL || cpu_addr == NULL || size == 0 || dma_window.active) {
        return KB_ERR_INVALID;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    const uintptr_t start = (uintptr_t)cpu_addr;
    if (device == NULL || ops == NULL || ops->dma_map_pages == NULL ||
        ops->dma_unmap == NULL || start > UINTPTR_MAX - size)
    {
        return KB_ERR_INVALID;
    }
    if (dma_window.mapped &&
        (backend != dma_window.backend ||
         device != dma_window.device ||
         direction != dma_window.direction ||
         start != dma_window.mapped_start))
    {
        dma_window_release_mapping();
    }
    if (!dma_window.mapped) {
        memset(&dma_window, 0, sizeof(dma_window));
    }
    dma_window.backend = backend;
    dma_window.device = device;
    dma_window.scope_start = start;
    dma_window.scope_size = size;
    dma_window.direction = direction;
    dma_window.active = 1;
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    __atomic_fetch_add(&dma_window_profile.begin_calls, 1u, __ATOMIC_RELAXED);
#endif
    return KB_OK;
}

kb_status_t kb_subsystem_dma_window_begin(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction)
{
    if (dma_window.mapped && !dma_window.active) {
        dma_window_release_mapping();
        memset(&dma_window, 0, sizeof(dma_window));
    }
    return dma_window_begin_common(backend, device, cpu_addr, size, direction);
}

kb_status_t kb_subsystem_dma_cached_window_begin(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction)
{
    return dma_window_begin_common(backend, device, cpu_addr, size, direction);
}

void kb_subsystem_dma_window_end(void)
{
    if (!dma_window.active || dma_window.borrowers != 0) {
        return;
    }
    dma_window_release_mapping();
    memset(&dma_window, 0, sizeof(dma_window));
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    __atomic_fetch_add(&dma_window_profile.end_calls, 1u, __ATOMIC_RELAXED);
#endif
}

void kb_subsystem_dma_cached_window_end(void)
{
    if (!dma_window.active || dma_window.borrowers != 0) {
        return;
    }
    dma_window.active = 0;
    dma_window.scope_start = 0;
    dma_window.scope_size = 0;
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    __atomic_fetch_add(&dma_window_profile.end_calls, 1u, __ATOMIC_RELAXED);
#endif
}

void kb_subsystem_dma_cached_window_discard(void)
{
    if (dma_window.active || dma_window.borrowers != 0) {
        return;
    }
    dma_window_release_mapping();
    memset(&dma_window, 0, sizeof(dma_window));
}

void kb_subsystem_dma_window_profile_snapshot(
    kb_subsystem_dma_window_profile_t *out_profile)
{
    if (out_profile == NULL) {
        return;
    }
    memset(out_profile, 0, sizeof(*out_profile));
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
#define KB_DMA_WINDOW_PROFILE_LOAD(field) \
    out_profile->field = __atomic_load_n(&dma_window_profile.field, __ATOMIC_RELAXED)
    KB_DMA_WINDOW_PROFILE_LOAD(begin_calls);
    KB_DMA_WINDOW_PROFILE_LOAD(mapping_calls);
    KB_DMA_WINDOW_PROFILE_LOAD(mapping_cycles);
    KB_DMA_WINDOW_PROFILE_LOAD(reuse_calls);
    KB_DMA_WINDOW_PROFILE_LOAD(reuse_cycles);
    KB_DMA_WINDOW_PROFILE_LOAD(end_calls);
    KB_DMA_WINDOW_PROFILE_LOAD(mapped_bytes);
    KB_DMA_WINDOW_PROFILE_LOAD(direct_mapping_calls);
    KB_DMA_WINDOW_PROFILE_LOAD(direct_mapping_cycles);
    KB_DMA_WINDOW_PROFILE_LOAD(direct_mapped_bytes);
    KB_DMA_WINDOW_PROFILE_LOAD(staged_read_calls);
    KB_DMA_WINDOW_PROFILE_LOAD(staged_bytes);
    KB_DMA_WINDOW_PROFILE_LOAD(staging_copy_cycles);
#undef KB_DMA_WINDOW_PROFILE_LOAD
#endif
}

void kb_subsystem_dma_window_profile_record_staging(
    uint64_t bytes,
    uint64_t copy_cycles)
{
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    __atomic_fetch_add(&dma_window_profile.staged_read_calls, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&dma_window_profile.staged_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &dma_window_profile.staging_copy_cycles,
        copy_cycles,
        __ATOMIC_RELAXED);
#else
    (void)bytes;
    (void)copy_cycles;
#endif
}

static kb_status_t dma_window_map_pages(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    uint64_t *out_page_dma,
    size_t out_capacity,
    int *out_used)
{
    const uint64_t profile_window_start = dma_profile_begin();
    *out_used = 0;
    if (!dma_window.active ||
        backend != dma_window.backend ||
        device != dma_window.device ||
        direction != dma_window.direction)
    {
        return KB_ERR_INVALID;
    }
    const uintptr_t request_start = (uintptr_t)cpu_addr;
    if (request_start > UINTPTR_MAX - size) {
        return KB_ERR_INVALID;
    }
    const uintptr_t request_end = request_start + size;
    const uintptr_t scope_end = dma_window.scope_start + dma_window.scope_size;
    if (request_start < dma_window.scope_start || request_end > scope_end ||
        (request_start & (KB_SUBSYSTEM_DMA_PAGE_SIZE - 1u)) != 0)
    {
        return KB_ERR_INVALID;
    }
    *out_used = 1;
    const size_t request_pages =
        size / KB_SUBSYSTEM_DMA_PAGE_SIZE +
        (size % KB_SUBSYSTEM_DMA_PAGE_SIZE != 0);
    if (request_pages == 0 || request_pages > out_capacity ||
        request_pages > KB_DEVICE_DMA_MAPPING_MAX_PAGES)
    {
        return KB_ERR_UNSUPPORTED;
    }

    int reused = 0;
    if (dma_window.mapped &&
        request_start >= dma_window.mapped_start &&
        request_end <= dma_window.mapped_start + dma_window.mapped_size)
    {
        reused = 1;
    } else {
        if (dma_window.borrowers != 0) {
            return KB_ERR_INVALID;
        }
        dma_window_release_mapping();
        const size_t remaining = (size_t)(scope_end - request_start);
        const size_t maximum =
            KB_DEVICE_DMA_MAPPING_MAX_PAGES * KB_SUBSYSTEM_DMA_PAGE_SIZE;
        const size_t map_size = remaining < maximum ? remaining : maximum;
        if (map_size < size) {
            return KB_ERR_UNSUPPORTED;
        }
        const size_t map_pages =
            map_size / KB_SUBSYSTEM_DMA_PAGE_SIZE +
            (map_size % KB_SUBSYSTEM_DMA_PAGE_SIZE != 0);
        const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
        if (ops == NULL || ops->dma_map_pages == NULL) {
            return KB_ERR_UNSUPPORTED;
        }
        const uint64_t profile_mapping_start = dma_profile_begin();
        const kb_status_t status = ops->dma_map_pages(
            device,
            (void *)request_start,
            map_size,
            direction,
            dma_window.page_dma,
            KB_DEVICE_DMA_MAPPING_MAX_PAGES);
        if (status != KB_OK) {
            return status;
        }
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
        const uint64_t profile_mapping_end = dma_profile_begin();
        if (profile_mapping_start != 0 && profile_mapping_end >= profile_mapping_start) {
            __atomic_fetch_add(
                &dma_window_profile.mapping_cycles,
                profile_mapping_end - profile_mapping_start,
            __ATOMIC_RELAXED);
        }
#else
        (void)profile_mapping_start;
#endif
        dma_window.mapped_start = request_start;
        dma_window.mapped_size = map_size;
        dma_window.mapped_page_count = map_pages;
        dma_window.mapped = 1;
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
        __atomic_fetch_add(&dma_window_profile.mapping_calls, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&dma_window_profile.mapped_bytes, map_size, __ATOMIC_RELAXED);
#endif
    }

    const size_t first_page =
        (size_t)((request_start - dma_window.mapped_start) / KB_SUBSYSTEM_DMA_PAGE_SIZE);
    if (first_page > dma_window.mapped_page_count ||
        request_pages > dma_window.mapped_page_count - first_page)
    {
        return KB_ERR_INVALID;
    }
    memcpy(out_page_dma,
        dma_window.page_dma + first_page,
        request_pages * sizeof(*out_page_dma));
    const kb_status_t remember_status = remember_dma_mapping(
        cpu_addr,
        out_page_dma[0],
        size,
        device,
        1);
    if (remember_status != KB_OK) {
        return remember_status;
    }
    dma_window.borrowers++;
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    if (reused) {
        __atomic_fetch_add(&dma_window_profile.reuse_calls, 1u, __ATOMIC_RELAXED);
        const uint64_t profile_window_end = dma_profile_begin();
        if (profile_window_start != 0 && profile_window_end >= profile_window_start) {
            __atomic_fetch_add(
                &dma_window_profile.reuse_cycles,
                profile_window_end - profile_window_start,
                __ATOMIC_RELAXED);
        }
    }
#else
    (void)profile_window_start;
    (void)reused;
#endif
    return KB_OK;
}

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

    if (remember_dma_mapping_full(
            buffer.cpu_addr,
            buffer.dma_addr,
            buffer.size,
            device,
            0,
            1) != KB_OK)
    {
        if (ops->dma_free != NULL) {
            ops->dma_free(device, &buffer);
        }
        return NULL;
    }

    *dma_handle = buffer.dma_addr;
    return buffer.cpu_addr;
}

kb_status_t kb_subsystem_dma_preallocated_pages(
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    uint64_t *out_page_dma,
    size_t out_capacity)
{
    if (device == NULL || cpu_addr == NULL || size == 0 ||
        out_page_dma == NULL || out_capacity == 0)
    {
        return KB_ERR_INVALID;
    }
    const uintptr_t request_start = (uintptr_t)cpu_addr;
    if (request_start > UINTPTR_MAX - size) {
        return KB_ERR_INVALID;
    }
    const uintptr_t request_end = request_start + size;
    for (kb_subsystem_dma_mapping_t *entry = dma_mappings;
         entry != NULL;
         entry = entry->next)
    {
        const uintptr_t mapping_start = (uintptr_t)entry->cpu_addr;
        if (!entry->persistent_allocation || entry->device != device ||
            mapping_start > UINTPTR_MAX - entry->size)
        {
            continue;
        }
        const uintptr_t mapping_end = mapping_start + (uintptr_t)entry->size;
        if (request_start < mapping_start || request_end > mapping_end) {
            continue;
        }
        const size_t page_offset =
            request_start & (KB_SUBSYSTEM_DMA_PAGE_SIZE - 1u);
        if (size > SIZE_MAX - page_offset) {
            return KB_ERR_INVALID;
        }
        const size_t span = page_offset + size;
        const size_t page_count =
            span / KB_SUBSYSTEM_DMA_PAGE_SIZE +
            (span % KB_SUBSYSTEM_DMA_PAGE_SIZE != 0);
        if (page_count == 0 || page_count > out_capacity) {
            return KB_ERR_UNSUPPORTED;
        }
        const uintptr_t mapping_offset = request_start - mapping_start;
        if ((uint64_t)mapping_offset > UINT64_MAX - entry->dma_addr) {
            return KB_ERR_INVALID;
        }
        const uint64_t dma_start =
            entry->dma_addr + (uint64_t)mapping_offset;
        const uint64_t page_base =
            dma_start & ~(uint64_t)(KB_SUBSYSTEM_DMA_PAGE_SIZE - 1u);
        if (page_count > 1) {
            const size_t last_index = page_count - 1;
            if (last_index >
                UINT64_MAX / (uint64_t)KB_SUBSYSTEM_DMA_PAGE_SIZE)
            {
                return KB_ERR_INVALID;
            }
            const uint64_t last_delta =
                (uint64_t)last_index *
                (uint64_t)KB_SUBSYSTEM_DMA_PAGE_SIZE;
            if (last_delta > UINT64_MAX - page_base) {
                return KB_ERR_INVALID;
            }
        }
        out_page_dma[0] = dma_start;
        for (size_t i = 1; i < page_count; ++i) {
            const uint64_t page_delta =
                (uint64_t)i * (uint64_t)KB_SUBSYSTEM_DMA_PAGE_SIZE;
            out_page_dma[i] = page_base + page_delta;
        }
        return KB_OK;
    }
    return KB_ERR_NOT_FOUND;
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

static uint64_t dma_map_with_lifetime(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    int persistent_allocation,
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
    if (ops == NULL || ops->dma_map == NULL ||
        (persistent_allocation && ops->dma_unmap == NULL)) {
        if (out_status != NULL) {
            *out_status = KB_ERR_UNSUPPORTED;
        }
        return 0;
    }

    uint64_t dma_addr = 0;
    kb_status_t status = ops->dma_map(device, cpu_addr, size, direction, &dma_addr);
    if (status == KB_OK && dma_addr == 0) {
        /* Zero is the public mapping-error sentinel.  A backend returning it
         * with KB_OK has nevertheless created a mapping, so roll that mapping
         * back before reporting the contract violation. */
        if (ops->dma_unmap != NULL) {
            ops->dma_unmap(device, dma_addr, size, direction);
        }
        status = KB_ERR_IO;
    } else if (status == KB_OK) {
        kb_status_t remember_status = remember_dma_mapping_full(
            cpu_addr,
            dma_addr,
            size,
            device,
            0,
            persistent_allocation);
        if (remember_status != KB_OK) {
            if (ops->dma_unmap != NULL) {
                ops->dma_unmap(device, dma_addr, size, direction);
            }
            status = remember_status;
        }
    }
    if (out_status != NULL) {
        *out_status = status;
    }
    return status == KB_OK ? dma_addr : 0;
}

uint64_t kb_subsystem_dma_map(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    kb_status_t *out_status)
{
    return dma_map_with_lifetime(
        backend,
        device,
        cpu_addr,
        size,
        direction,
        0,
        out_status);
}

uint64_t kb_subsystem_dma_map_persistent_bidirectional(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_status_t *out_status)
{
    /* Preallocated subranges do not carry a direction into their lookup.
     * Only a mapping valid for both device reads and writes may therefore be
     * published as a persistent parent. */
    return dma_map_with_lifetime(
        backend,
        device,
        cpu_addr,
        size,
        KB_DMA_BIDIRECTIONAL,
        1,
        out_status);
}

kb_status_t kb_subsystem_dma_map_pages(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    kb_dma_dir_t direction,
    uint64_t *out_page_dma,
    size_t out_capacity)
{
    if (backend == NULL || cpu_addr == NULL || size == 0 || out_page_dma == NULL || out_capacity == 0) {
        return KB_ERR_INVALID;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }
    if (device == NULL) {
        return KB_ERR_INVALID;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops == NULL || ops->dma_map_pages == NULL) {
        return KB_ERR_UNSUPPORTED;
    }

    int used_window = 0;
    kb_status_t status = dma_window_map_pages(
        backend,
        device,
        cpu_addr,
        size,
        direction,
        out_page_dma,
        out_capacity,
        &used_window);
    if (used_window) {
        return status;
    }

    const uint64_t profile_direct_start = dma_profile_begin();
    status = ops->dma_map_pages(
        device,
        cpu_addr,
        size,
        direction,
        out_page_dma,
        out_capacity);
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    const uint64_t profile_direct_end = dma_profile_begin();
    __atomic_fetch_add(&dma_window_profile.direct_mapping_calls, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&dma_window_profile.direct_mapped_bytes, size, __ATOMIC_RELAXED);
    if (profile_direct_start != 0 && profile_direct_end >= profile_direct_start) {
        __atomic_fetch_add(
            &dma_window_profile.direct_mapping_cycles,
            profile_direct_end - profile_direct_start,
            __ATOMIC_RELAXED);
    }
#else
    (void)profile_direct_start;
#endif
    if (status != KB_OK) {
        return status;
    }

    status = remember_dma_mapping_full(
        cpu_addr, out_page_dma[0], size, device, 0, 0);
    if (status != KB_OK) {
        if (ops->dma_unmap != NULL) {
            ops->dma_unmap(device, out_page_dma[0], size, direction);
        }
        return status;
    }
    return KB_OK;
}

kb_status_t kb_subsystem_dma_map_fixed(
    kb_device_backend_t *backend,
    kb_device_t *device,
    void *cpu_addr,
    size_t size,
    uint64_t dma_addr,
    kb_dma_dir_t direction)
{
    if (backend == NULL || cpu_addr == NULL || size == 0) {
        return KB_ERR_INVALID;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }
    if (device == NULL) {
        return KB_ERR_INVALID;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops == NULL || ops->dma_map_fixed == NULL) {
        return KB_ERR_UNSUPPORTED;
    }

    kb_status_t status = ops->dma_map_fixed(device, cpu_addr, size, dma_addr, direction);
    if (status != KB_OK) {
        return status;
    }
    status = remember_dma_mapping(cpu_addr, dma_addr, size, device, 0);
    if (status != KB_OK) {
        if (ops->dma_unmap != NULL) {
            ops->dma_unmap(device, dma_addr, size, direction);
        }
    }
    return status;
}

void kb_subsystem_dma_unmap(
    kb_device_backend_t *backend,
    kb_device_t *device,
    uint64_t dma_addr,
    size_t size,
    kb_dma_dir_t direction)
{
    if (backend == NULL || size == 0) {
        return;
    }
    if (device == NULL) {
        device = kb_subsystem_dma_default_device(backend);
    }

    kb_subsystem_dma_mapping_t *mapping = take_dma_mapping(dma_addr);
    if (mapping != NULL && mapping->borrowed_window) {
        if (dma_window.borrowers != 0) {
            dma_window.borrowers--;
        }
        kb_kfree(mapping);
        return;
    }
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (device != NULL && ops != NULL && ops->dma_unmap != NULL) {
        ops->dma_unmap(device, dma_addr, size, direction);
    }
    if (mapping != NULL) {
        kb_kfree(mapping);
    }
}

int kb_subsystem_dma_mapping_error(uint64_t dma_addr)
{
    if (dma_addr == 0) {
        return 1;
    }
    for (kb_subsystem_dma_mapping_t *entry = dma_mappings; entry != NULL; entry = entry->next) {
        if (dma_addr >= entry->dma_addr && dma_addr < entry->dma_addr + entry->size) {
            return 0;
        }
    }
    return dma_addr == 0;
}
