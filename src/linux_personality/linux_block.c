#include "kobox/shim.h"
#include "linux_personality/linux_block.h"
#include "linux_personality/linux_nvme.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/dma/dma.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef KOBOX_FILE_READ_BULK_BYTES
#define KOBOX_FILE_READ_BULK_BYTES (512u * 1024u)
#endif

kb_device_backend_t *kb_shim_current_device_backend(void);

typedef struct shim_linux_hctx {
    void *tags;
    unsigned char reserved_008[0xc8 - 0x08];
    void *driver_data;
} shim_linux_hctx_t;

typedef struct shim_linux_queue_data {
    void *rq;
    uint8_t last;
    unsigned char reserved_009[0x100 - 0x09];
} shim_linux_queue_data_t;

enum {
    KB_LINUX_BLK_MQ_OPS_QUEUE_RQ_OFFSET = 0x00,
    KB_LINUX_BLK_MQ_OPS_COMMIT_RQS_OFFSET = 0x08,
    KB_LINUX_BLK_MQ_TAG_SET_OPS_OFFSET = 0x00,
    KB_LINUX_BLK_MQ_TAG_SET_DRIVER_DATA_OFFSET = 0x58,
    KB_LINUX_BLK_MQ_TAG_SET_TAGS_OFFSET = 0x60,
    KB_LINUX_6_8_GENDISK_PART0_OFFSET = 0x40,
    KB_LINUX_6_8_GENDISK_QUEUE_OFFSET = 0x50,

    KB_LINUX_REQUEST_HCTX_OFFSET = 0x00,
    KB_LINUX_REQUEST_QUEUE_OFFSET = 0x10,
    KB_LINUX_REQUEST_CMD_FLAGS_OFFSET = 0x18,
    KB_LINUX_REQUEST_TAG_OFFSET = 0x20,
    KB_LINUX_REQUEST_SPECIAL_OFFSET = 0x110,
    KB_LINUX_REQUEST_RESULT_OFFSET = 0x118,
    KB_LINUX_REQUEST_STATUS_OFFSET = 0x124,
    KB_LINUX_REQUEST_CTRL_OFFSET = 0x130,
    KB_LINUX_REQUEST_NVME_CMD_OFFSET = 0x138,

    KB_SHIM_REQUEST_SIZE = 4096,
    KB_SHIM_REQUEST_HCTX_OFFSET = 0x800,
    KB_SHIM_REQUEST_QUEUE_DATA_OFFSET = 0x900,
    KB_SHIM_REQUEST_DMA_ADDR_OFFSET = 0xa00,
    KB_SHIM_REQUEST_DMA_LEN_OFFSET = 0xa08,
    KB_SHIM_REQUEST_PRP_LIST_CPU_OFFSET = 0xa10,
    KB_SHIM_REQUEST_PRP_LIST_DMA_OFFSET = 0xa18,
    KB_SHIM_REQUEST_PRP_LIST_LEN_OFFSET = 0xa20,
    KB_SHIM_REQUEST_OWNS_QUEUE_OFFSET = 0xa24,
    KB_LINUX_BLOCK_MAX_DRIVER_OPS = 8,
    KB_LINUX_BLOCK_REQUEST_CACHE_MAX = 8,
};

typedef struct shim_linux_request {
    void *hctx;
    unsigned char reserved_008[0x10 - 0x08];
    void *queue;
    uint32_t cmd_flags;
    unsigned char reserved_01c[0x20 - 0x1c];
    uint32_t tag;
    unsigned char reserved_024[0x48 - 0x24];
    void *batch_next;
    unsigned char reserved_050[0x110 - 0x50];
    void *special;
    uint64_t result;
    unsigned char reserved_120[0x124 - 0x120];
    uint16_t status;
    unsigned char reserved_126[0x130 - 0x126];
    void *ctrl;
    unsigned char nvme_cmd[64];
    unsigned char reserved_178[0x800 - 0x178];
    shim_linux_hctx_t hctx_storage;
    unsigned char reserved_8d0[0x900 - 0x8d0];
    shim_linux_queue_data_t queue_data;
    uint64_t dma_addr;
    uint32_t dma_len;
    unsigned char reserved_a0c[0xa10 - 0xa0c];
    void *prp_list_cpu;
    uint64_t prp_list_dma;
    uint32_t prp_list_len;
    uint32_t owns_queue;
    uint32_t completed;
    uint32_t end_status;
    kb_dma_dir_t dma_dir;
    kb_dma_dir_t prp_list_dma_dir;
    const kb_linux_block_driver_ops_t *driver_ops;
    uint32_t started;
    uint32_t prp_list_cached;
    kb_device_backend_t *dma_backend;
    kb_device_t *dma_device;
    kb_device_backend_t *prp_list_backend;
    kb_device_t *prp_list_device;
    uint32_t dma_preallocated;
    unsigned char reserved_a6c[KB_SHIM_REQUEST_SIZE - 0xa6c];
} shim_linux_request_t;

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
static kb_linux_block_profile_t linux_block_profile;
#endif

typedef struct kb_linux_dma_read_scope {
    uintptr_t target_start;
    size_t target_size;
    unsigned char *staging_buffer;
    size_t staging_size;
    uint8_t active;
} kb_linux_dma_read_scope_t;

enum {
    KB_LINUX_DMA_READ_STAGING_SIZE = KOBOX_FILE_READ_BULK_BYTES,
    KB_LINUX_DMA_READ_BATCH_DEPTH = 128u,
    KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE =
        KB_LINUX_DMA_READ_STAGING_SIZE * 4u,
    KB_LINUX_DMA_WRITE_STAGING_SIZE = KOBOX_FILE_READ_BULK_BYTES,
    KB_LINUX_DMA_PAGE_SIZE = 4096u,
};

_Static_assert(KB_LINUX_DMA_READ_STAGING_SIZE >= KB_LINUX_DMA_PAGE_SIZE,
    "DMA read staging must cover at least one page");
_Static_assert((KB_LINUX_DMA_READ_STAGING_SIZE % KB_LINUX_DMA_PAGE_SIZE) == 0,
    "DMA read staging must be page aligned");
_Static_assert(
    KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE <=
        KB_DEVICE_DMA_MAPPING_MAX_PAGES * KB_LINUX_DMA_PAGE_SIZE,
    "DMA read staging exceeds the NVMe PRP page capacity");
static kb_linux_dma_read_scope_t dma_read_scope;
static unsigned char *dma_read_staging_buffer;
static unsigned char *dma_write_staging_buffer;
static uint64_t dma_write_staging_handle;
static shim_linux_request_t *cached_requests[KB_LINUX_BLOCK_REQUEST_CACHE_MAX];
static size_t cached_request_count;
static atomic_flag cached_request_lock = ATOMIC_FLAG_INIT;

static void cached_request_lock_acquire(void)
{
    while (atomic_flag_test_and_set_explicit(
        &cached_request_lock, memory_order_acquire))
    {
    }
}

static void cached_request_lock_release(void)
{
    atomic_flag_clear_explicit(&cached_request_lock, memory_order_release);
}

static shim_linux_request_t *cached_request_take(void)
{
    shim_linux_request_t *request = NULL;
    cached_request_lock_acquire();
    if (cached_request_count != 0) {
        request = cached_requests[--cached_request_count];
        cached_requests[cached_request_count] = NULL;
    }
    cached_request_lock_release();
    return request;
}

static int cached_request_store(shim_linux_request_t *request)
{
    int stored = 0;
    cached_request_lock_acquire();
    if (cached_request_count < KB_LINUX_BLOCK_REQUEST_CACHE_MAX) {
        cached_requests[cached_request_count++] = request;
        stored = 1;
    }
    cached_request_lock_release();
    return stored;
}

static void cached_request_discard_all(void)
{
    cached_request_lock_acquire();
    while (cached_request_count != 0) {
        free(cached_requests[--cached_request_count]);
        cached_requests[cached_request_count] = NULL;
    }
    cached_request_lock_release();
}

typedef struct kb_linux_cached_aux_dma {
    kb_device_backend_t *backend;
    kb_device_t *device;
    void *cpu_addr;
    uint64_t dma_addr;
    void *owner;
} kb_linux_cached_aux_dma_t;

enum {
    KB_LINUX_CACHED_AUX_DMA_SIZE = 4096u,
    KB_LINUX_CACHED_AUX_DMA_MAX = 64u,
};

static kb_linux_cached_aux_dma_t
    cached_aux_dma[KB_LINUX_CACHED_AUX_DMA_MAX];
static atomic_flag cached_aux_dma_lock = ATOMIC_FLAG_INIT;

static void cached_aux_dma_lock_acquire(void)
{
    while (atomic_flag_test_and_set_explicit(
        &cached_aux_dma_lock, memory_order_acquire))
    {
    }
}

static void cached_aux_dma_lock_release(void)
{
    atomic_flag_clear_explicit(&cached_aux_dma_lock, memory_order_release);
}

static void cached_aux_dma_discard_unused(void)
{
    cached_aux_dma_lock_acquire();
    for (size_t i = 0; i < KB_LINUX_CACHED_AUX_DMA_MAX; i++) {
        kb_linux_cached_aux_dma_t *entry = &cached_aux_dma[i];
        if (entry->owner != NULL || entry->cpu_addr == NULL) {
            continue;
        }
        kb_subsystem_dma_free(
            entry->backend,
            KB_LINUX_CACHED_AUX_DMA_SIZE,
            entry->cpu_addr,
            entry->dma_addr);
        memset(entry, 0, sizeof(*entry));
    }
    cached_aux_dma_lock_release();
}

uint64_t kb_linux_block_profile_begin(void)
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

void kb_linux_block_profile_record(
    kb_linux_block_profile_stage_t stage,
    uint64_t start,
    uint64_t bytes)
{
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    const uint64_t end = kb_linux_block_profile_begin();
    if ((unsigned)stage >= KB_LINUX_BLOCK_PROFILE_STAGE_COUNT ||
        start == 0 || end < start)
    {
        return;
    }
    __atomic_fetch_add(&linux_block_profile.cycles[stage], end - start, __ATOMIC_RELAXED);
    __atomic_fetch_add(&linux_block_profile.calls[stage], 1u, __ATOMIC_RELAXED);
    if (stage == KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL && bytes != 0) {
        __atomic_fetch_add(&linux_block_profile.disk_read_bytes, bytes, __ATOMIC_RELAXED);
    }
#else
    (void)stage;
    (void)start;
    (void)bytes;
#endif
}

void kb_linux_block_profile_snapshot(kb_linux_block_profile_t *out_profile)
{
    if (out_profile == NULL) {
        return;
    }
    memset(out_profile, 0, sizeof(*out_profile));
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    for (size_t i = 0; i < KB_LINUX_BLOCK_PROFILE_STAGE_COUNT; i++) {
        out_profile->cycles[i] =
            __atomic_load_n(&linux_block_profile.cycles[i], __ATOMIC_RELAXED);
        out_profile->calls[i] =
            __atomic_load_n(&linux_block_profile.calls[i], __ATOMIC_RELAXED);
    }
    out_profile->disk_read_bytes =
        __atomic_load_n(&linux_block_profile.disk_read_bytes, __ATOMIC_RELAXED);
    for (size_t i = 0; i < 8u; i++) {
        out_profile->disk_read_command_calls[i] = __atomic_load_n(
            &linux_block_profile.disk_read_command_calls[i], __ATOMIC_RELAXED);
        out_profile->disk_read_command_bytes[i] = __atomic_load_n(
            &linux_block_profile.disk_read_command_bytes[i], __ATOMIC_RELAXED);
        out_profile->disk_write_command_calls[i] = __atomic_load_n(
            &linux_block_profile.disk_write_command_calls[i], __ATOMIC_RELAXED);
        out_profile->disk_write_command_bytes[i] = __atomic_load_n(
            &linux_block_profile.disk_write_command_bytes[i], __ATOMIC_RELAXED);
    }
    out_profile->disk_read_command_count = __atomic_load_n(
        &linux_block_profile.disk_read_command_count, __ATOMIC_RELAXED);
    out_profile->disk_write_command_count = __atomic_load_n(
        &linux_block_profile.disk_write_command_count, __ATOMIC_RELAXED);
    out_profile->native_fua_commands = __atomic_load_n(
        &linux_block_profile.native_fua_commands, __ATOMIC_RELAXED);
    for (size_t i = 0; i < 64u; i++) {
        out_profile->disk_read_command_sectors[i] = __atomic_load_n(
            &linux_block_profile.disk_read_command_sectors[i], __ATOMIC_RELAXED);
        out_profile->disk_read_command_lengths[i] = __atomic_load_n(
            &linux_block_profile.disk_read_command_lengths[i], __ATOMIC_RELAXED);
        out_profile->disk_write_command_sectors[i] = __atomic_load_n(
            &linux_block_profile.disk_write_command_sectors[i], __ATOMIC_RELAXED);
        out_profile->disk_write_command_lengths[i] = __atomic_load_n(
            &linux_block_profile.disk_write_command_lengths[i], __ATOMIC_RELAXED);
        out_profile->disk_write_command_flags[i] = __atomic_load_n(
            &linux_block_profile.disk_write_command_flags[i], __ATOMIC_RELAXED);
    }
#endif
}

void kb_linux_block_profile_record_native_fua(void)
{
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    __atomic_fetch_add(
        &linux_block_profile.native_fua_commands,
        1u,
        __ATOMIC_RELAXED);
#endif
}

void kb_linux_block_profile_record_read_command(
    uint64_t sector,
    size_t byte_count)
{
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    size_t bucket = 7u;
    switch (byte_count) {
    case 4u * 1024u: bucket = 0u; break;
    case 16u * 1024u: bucket = 1u; break;
    case 32u * 1024u: bucket = 2u; break;
    case 64u * 1024u: bucket = 3u; break;
    case 128u * 1024u: bucket = 4u; break;
    case 256u * 1024u: bucket = 5u; break;
    case 512u * 1024u: bucket = 6u; break;
    default: break;
    }
    __atomic_fetch_add(
        &linux_block_profile.disk_read_command_calls[bucket],
        1u,
        __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &linux_block_profile.disk_read_command_bytes[bucket],
        byte_count,
        __ATOMIC_RELAXED);
    const uint64_t command_index = __atomic_fetch_add(
        &linux_block_profile.disk_read_command_count,
        1u,
        __ATOMIC_RELAXED);
    const size_t slot = (size_t)(command_index % 64u);
    __atomic_store_n(
        &linux_block_profile.disk_read_command_sectors[slot],
        sector,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &linux_block_profile.disk_read_command_lengths[slot],
        byte_count,
        __ATOMIC_RELAXED);
#else
    (void)sector;
    (void)byte_count;
#endif
}

void kb_linux_block_profile_record_write_command(
    uint64_t sector,
    size_t byte_count,
    uint32_t flags)
{
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    size_t bucket = 7u;
    switch (byte_count) {
    case 4u * 1024u: bucket = 0u; break;
    case 16u * 1024u: bucket = 1u; break;
    case 32u * 1024u: bucket = 2u; break;
    case 64u * 1024u: bucket = 3u; break;
    case 128u * 1024u: bucket = 4u; break;
    case 256u * 1024u: bucket = 5u; break;
    case 512u * 1024u: bucket = 6u; break;
    default: break;
    }
    __atomic_fetch_add(
        &linux_block_profile.disk_write_command_calls[bucket],
        1u,
        __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &linux_block_profile.disk_write_command_bytes[bucket],
        byte_count,
        __ATOMIC_RELAXED);
    const uint64_t command_index = __atomic_fetch_add(
        &linux_block_profile.disk_write_command_count,
        1u,
        __ATOMIC_RELAXED);
    const size_t slot = (size_t)(command_index % 64u);
    __atomic_store_n(
        &linux_block_profile.disk_write_command_sectors[slot],
        sector,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &linux_block_profile.disk_write_command_lengths[slot],
        byte_count,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &linux_block_profile.disk_write_command_flags[slot],
        flags,
        __ATOMIC_RELAXED);
#else
    (void)sector;
    (void)byte_count;
    (void)flags;
#endif
}

int kb_linux_block_dma_read_window_begin(void *buffer, size_t length)
{
    if (buffer == NULL || length == 0 ||
        ((uintptr_t)buffer & (KB_LINUX_DMA_PAGE_SIZE - 1u)) != 0 ||
        (uintptr_t)buffer > UINTPTR_MAX - length ||
        dma_read_scope.active)
    {
        return -22;
    }
    kb_device_backend_t *backend = kb_shim_current_device_backend();
    if (backend == NULL) {
        return -19;
    }
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return -19;
    }
    if (dma_read_staging_buffer == NULL) {
        dma_read_staging_buffer = aligned_alloc(
            KB_LINUX_DMA_PAGE_SIZE,
            KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE);
        if (dma_read_staging_buffer == NULL) {
            return -12;
        }
    }
    size_t staging_size = length < KB_LINUX_DMA_READ_STAGING_SIZE ?
        length : KB_LINUX_DMA_READ_STAGING_SIZE;
    staging_size = (staging_size + KB_LINUX_DMA_PAGE_SIZE - 1u) &
        ~(size_t)(KB_LINUX_DMA_PAGE_SIZE - 1u);
    const kb_status_t status = kb_subsystem_dma_cached_window_begin(
        backend,
        device,
        dma_read_staging_buffer,
        KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE,
        KB_DMA_FROM_DEVICE);
    if (status != KB_OK) {
        return -5;
    }
    dma_read_scope.target_start = (uintptr_t)buffer;
    dma_read_scope.target_size = length;
    dma_read_scope.staging_buffer = dma_read_staging_buffer;
    dma_read_scope.staging_size = staging_size;
    dma_read_scope.active = 1;
    return 0;
}

void kb_linux_block_dma_read_window_end(void)
{
    if (!dma_read_scope.active) {
        return;
    }
    kb_subsystem_dma_cached_window_end();
    memset(&dma_read_scope, 0, sizeof(dma_read_scope));
}

/* Borrow the staging buffer for a read that has no scope of its own.  The
 * staging mapping is derived once and stays cached, so metadata and ordinary
 * file reads stop paying a device mapping derive each time.  Returns 0 when the
 * read was served here, or -95 when the caller must issue it directly. */
static int block_staged_read_borrow(
    const kb_linux_block_driver_ops_t *ops,
    void *queue,
    uint64_t sector,
    void *buffer,
    size_t byte_count,
    int *out_status)
{
    if (ops == NULL || ops->disk_read == NULL || buffer == NULL ||
        byte_count == 0 || byte_count > KB_LINUX_DMA_READ_STAGING_SIZE)
    {
        return -95;
    }
    kb_device_backend_t *backend = kb_shim_current_device_backend();
    if (backend == NULL) {
        return -95;
    }
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return -95;
    }
    if (dma_read_staging_buffer == NULL) {
        dma_read_staging_buffer = aligned_alloc(
            KB_LINUX_DMA_PAGE_SIZE,
            KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE);
        if (dma_read_staging_buffer == NULL) {
            return -95;
        }
    }
    if (kb_subsystem_dma_cached_window_begin(
            backend,
            device,
            dma_read_staging_buffer,
            KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE,
            KB_DMA_FROM_DEVICE) != KB_OK)
    {
        return -95;
    }
    const int status = ops->disk_read(
        queue, sector, dma_read_staging_buffer, byte_count);
    if (status == 0) {
        const uint64_t copy_start = kb_linux_block_profile_begin();
        memcpy(buffer, dma_read_staging_buffer, byte_count);
        const uint64_t copy_end = kb_linux_block_profile_begin();
        kb_subsystem_dma_window_profile_record_staging(
            byte_count, copy_end >= copy_start ? copy_end - copy_start : 0);
    }
    kb_subsystem_dma_cached_window_end();
    *out_status = status;
    return 0;
}

_Static_assert(offsetof(shim_linux_request_t, hctx) == KB_LINUX_REQUEST_HCTX_OFFSET, "request.hctx offset");
_Static_assert(offsetof(shim_linux_request_t, queue) == KB_LINUX_REQUEST_QUEUE_OFFSET, "request.queue offset");
_Static_assert(offsetof(shim_linux_request_t, cmd_flags) == KB_LINUX_REQUEST_CMD_FLAGS_OFFSET, "request.cmd_flags offset");
_Static_assert(offsetof(shim_linux_request_t, tag) == KB_LINUX_REQUEST_TAG_OFFSET, "request.tag offset");
_Static_assert(offsetof(shim_linux_request_t, batch_next) == 0x48, "request.batch_next offset");
_Static_assert(offsetof(shim_linux_request_t, special) == KB_LINUX_REQUEST_SPECIAL_OFFSET, "request.special offset");
_Static_assert(offsetof(shim_linux_request_t, result) == KB_LINUX_REQUEST_RESULT_OFFSET, "request.result offset");
_Static_assert(offsetof(shim_linux_request_t, status) == KB_LINUX_REQUEST_STATUS_OFFSET, "request.status offset");
_Static_assert(offsetof(shim_linux_request_t, ctrl) == KB_LINUX_REQUEST_CTRL_OFFSET, "request.ctrl offset");
_Static_assert(offsetof(shim_linux_request_t, nvme_cmd) == KB_LINUX_REQUEST_NVME_CMD_OFFSET, "request.nvme_cmd offset");
_Static_assert(offsetof(shim_linux_request_t, hctx_storage) == KB_SHIM_REQUEST_HCTX_OFFSET, "shim hctx offset");
_Static_assert(offsetof(shim_linux_request_t, queue_data) == KB_SHIM_REQUEST_QUEUE_DATA_OFFSET, "shim queue_data offset");
_Static_assert(offsetof(shim_linux_request_t, dma_addr) == KB_SHIM_REQUEST_DMA_ADDR_OFFSET, "shim dma_addr offset");
_Static_assert(offsetof(shim_linux_request_t, dma_len) == KB_SHIM_REQUEST_DMA_LEN_OFFSET, "shim dma_len offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_cpu) == KB_SHIM_REQUEST_PRP_LIST_CPU_OFFSET, "shim prp_list_cpu offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_dma) == KB_SHIM_REQUEST_PRP_LIST_DMA_OFFSET, "shim prp_list_dma offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_len) == KB_SHIM_REQUEST_PRP_LIST_LEN_OFFSET, "shim prp_list_len offset");
_Static_assert(offsetof(shim_linux_request_t, owns_queue) == KB_SHIM_REQUEST_OWNS_QUEUE_OFFSET, "shim owns_queue offset");
_Static_assert(offsetof(shim_linux_request_t, driver_ops) == 0xa38, "shim driver_ops offset");
_Static_assert(offsetof(shim_linux_request_t, started) == 0xa40, "shim started offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_cached) == 0xa44, "shim prp_list_cached offset");
_Static_assert(offsetof(shim_linux_request_t, dma_backend) == 0xa48, "shim dma_backend offset");
_Static_assert(offsetof(shim_linux_request_t, dma_device) == 0xa50, "shim dma_device offset");
_Static_assert(offsetof(shim_linux_request_t, prp_list_backend) == 0xa58, "shim prp_list_backend offset");
_Static_assert(sizeof(shim_linux_request_t) == KB_SHIM_REQUEST_SIZE, "shim request size");
_Static_assert(offsetof(shim_linux_hctx_t, driver_data) == 0xc8, "hctx.driver_data offset");
_Static_assert(offsetof(shim_linux_queue_data_t, rq) == 0x00, "queue_data.rq offset");
_Static_assert(offsetof(shim_linux_queue_data_t, last) == 0x08, "queue_data.last offset");

static const kb_linux_block_driver_ops_t *registered_driver_ops[KB_LINUX_BLOCK_MAX_DRIVER_OPS];
static size_t registered_driver_ops_count;
static int builtin_drivers_registered;

static void *read_ptr(const void *ptr)
{
    uintptr_t value;
    memcpy(&value, ptr, sizeof(value));
    return (void *)value;
}

static void write_ptr(void *ptr, void *value)
{
    memcpy(ptr, &value, sizeof(value));
}

static int trace_block_enabled(void)
{
#if defined(__pachaos__)
    return 0;
#else
    return getenv("KOBOX_TRACE_BLOCK") != NULL || getenv("KOBOX_TRACE_NVME") != NULL;
#endif
}

void kb_linux_block_register_driver_ops(const kb_linux_block_driver_ops_t *ops)
{
    if (ops == NULL) {
        return;
    }
    for (size_t i = 0; i < registered_driver_ops_count; i++) {
        if (registered_driver_ops[i] == ops) {
            return;
        }
    }
    if (registered_driver_ops_count < KB_LINUX_BLOCK_MAX_DRIVER_OPS) {
        registered_driver_ops[registered_driver_ops_count++] = ops;
    }
}

static void ensure_builtin_driver_ops(void)
{
    if (builtin_drivers_registered) {
        return;
    }
    builtin_drivers_registered = 1;
    kb_nvme_shim_register_block_driver();
}

static const kb_linux_block_driver_ops_t *block_driver_ops_for_tag_set(void *tag_set)
{
    ensure_builtin_driver_ops();
    for (size_t i = 0; i < registered_driver_ops_count; i++) {
        const kb_linux_block_driver_ops_t *ops = registered_driver_ops[i];
        if (ops->match_tag_set == NULL || ops->match_tag_set(tag_set)) {
            return ops;
        }
    }
    return NULL;
}

static const kb_linux_block_driver_ops_t *block_driver_ops_for_disk(void *disk, void **out_queue)
{
    kb_block_disk_snapshot_t snapshot;
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0 || snapshot.queue == NULL) {
        return NULL;
    }

    void *tag_set = kb_block_subsystem_queue_tag_set(snapshot.queue);
    if (tag_set == NULL) {
        return NULL;
    }
    if (out_queue != NULL) {
        *out_queue = snapshot.queue;
    }
    return block_driver_ops_for_tag_set(tag_set);
}

static int shim_blk_disk_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops = block_driver_ops_for_disk(ctx, &queue);
    if (ops == NULL || ops->disk_read == NULL) {
        return -95;
    }
    const uintptr_t request_start = (uintptr_t)buffer;
    if (dma_read_scope.active &&
        byte_count <= dma_read_scope.staging_size &&
        request_start <= UINTPTR_MAX - byte_count)
    {
        const uintptr_t request_end = request_start + byte_count;
        const uintptr_t target_end =
            dma_read_scope.target_start + dma_read_scope.target_size;
        if (request_start >= dma_read_scope.target_start && request_end <= target_end) {
            const int status = ops->disk_read(
                queue,
                sector,
                dma_read_scope.staging_buffer,
                byte_count);
            if (status == 0) {
                const uint64_t copy_start = kb_linux_block_profile_begin();
                memcpy(buffer, dma_read_scope.staging_buffer, byte_count);
                const uint64_t copy_end = kb_linux_block_profile_begin();
                kb_subsystem_dma_window_profile_record_staging(
                    byte_count,
                    copy_end >= copy_start ? copy_end - copy_start : 0);
            }
            return status;
        }
    }
    if (!dma_read_scope.active) {
        int staged_status = 0;
        if (block_staged_read_borrow(
                ops, queue, sector, buffer, byte_count, &staged_status) == 0)
        {
            return staged_status;
        }
    }
    return ops->disk_read(queue, sector, buffer, byte_count);
}

static int shim_blk_disk_read_batch(
    void *ctx,
    const kb_block_disk_read_request_t *requests,
    size_t request_count)
{
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops = block_driver_ops_for_disk(ctx, &queue);
    if (requests == NULL || request_count < 2 ||
        request_count > KB_LINUX_DMA_READ_BATCH_DEPTH)
    {
        return -22;
    }
    if (ops == NULL || ops->disk_read_batch == NULL) {
        for (size_t i = 0; i < request_count; ++i) {
            const int status = shim_blk_disk_read(
                ctx,
                requests[i].sector,
                requests[i].buffer,
                requests[i].byte_count);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }
    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_device_t *device = backend == NULL ? NULL :
        kb_subsystem_dma_default_device(backend);
    if (backend == NULL || device == NULL) {
        return -19;
    }
    if (dma_read_staging_buffer == NULL) {
        dma_read_staging_buffer = aligned_alloc(
            KB_LINUX_DMA_PAGE_SIZE,
            KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE);
        if (dma_read_staging_buffer == NULL) {
            return -12;
        }
    }
    size_t stride = 0;
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL || requests[i].byte_count == 0 ||
            requests[i].byte_count > KB_LINUX_DMA_READ_STAGING_SIZE)
        {
            return -22;
        }
        if (requests[i].byte_count > stride) {
            stride = requests[i].byte_count;
        }
    }
    stride = (stride + KB_LINUX_DMA_PAGE_SIZE - 1u) &
        ~(size_t)(KB_LINUX_DMA_PAGE_SIZE - 1u);
    if (stride == 0 ||
        request_count > KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE / stride)
    {
        for (size_t i = 0; i < request_count; ++i) {
            const int status = shim_blk_disk_read(
                ctx,
                requests[i].sector,
                requests[i].buffer,
                requests[i].byte_count);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }
    const int borrowed_window = !dma_read_scope.active;
    if (borrowed_window &&
        kb_subsystem_dma_cached_window_begin(
            backend,
            device,
            dma_read_staging_buffer,
            KB_LINUX_DMA_READ_STAGING_TOTAL_SIZE,
            KB_DMA_FROM_DEVICE) != KB_OK)
    {
        return -5;
    }
    kb_linux_block_read_request_t batch[KB_LINUX_DMA_READ_BATCH_DEPTH];
    for (size_t i = 0; i < request_count; ++i) {
        batch[i].sector = requests[i].sector;
        batch[i].buffer = dma_read_staging_buffer +
            i * stride;
        batch[i].byte_count = requests[i].byte_count;
    }
    const int status = ops->disk_read_batch(queue, batch, request_count);
    if (status != 0) {
        if (borrowed_window) {
            kb_subsystem_dma_cached_window_end();
        }
        return status;
    }
    for (size_t i = 0; i < request_count; ++i) {
        const uint64_t copy_start = kb_linux_block_profile_begin();
        memcpy(requests[i].buffer, batch[i].buffer, requests[i].byte_count);
        const uint64_t copy_end = kb_linux_block_profile_begin();
        kb_subsystem_dma_window_profile_record_staging(
            requests[i].byte_count,
            copy_end >= copy_start ? copy_end - copy_start : 0);
    }
    if (borrowed_window) {
        kb_subsystem_dma_cached_window_end();
    }
    return 0;
}

static int shim_blk_disk_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops = block_driver_ops_for_disk(ctx, &queue);
    if (ops == NULL || ops->disk_write == NULL) {
        return -95;
    }
    return ops->disk_write(queue, sector, buffer, byte_count);
}

static int shim_blk_disk_write_flags(
    void *ctx,
    uint64_t sector,
    const void *buffer,
    size_t byte_count,
    uint32_t flags)
{
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops =
        block_driver_ops_for_disk(ctx, &queue);
    if (ops == NULL ||
        (flags & ~KB_BLOCK_DISK_WRITE_FUA) != 0 ||
        ops->disk_write == NULL)
    {
        return -95;
    }
    if ((flags & KB_BLOCK_DISK_WRITE_FUA) != 0) {
        kb_block_queue_limits_t limits;
        if (ops->disk_write_flags != NULL &&
            kb_block_subsystem_queue_limits(queue, &limits) == 0 &&
            limits.fua)
        {
            return ops->disk_write_flags(
                queue,
                sector,
                buffer,
                byte_count,
                KB_LINUX_BLOCK_WRITE_FUA);
        }
    }
    int status = ops->disk_write(queue, sector, buffer, byte_count);
    if (status == 0 && (flags & KB_BLOCK_DISK_WRITE_FUA) != 0) {
        status = ops->disk_flush == NULL ? -95 : ops->disk_flush(queue);
    }
    return status;
}

static int shim_blk_disk_write_batch(
    void *ctx,
    const kb_block_disk_write_request_t *requests,
    size_t request_count)
{
    enum { KB_LINUX_BLOCK_WRITE_BATCH_MAX = 128 };
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops = block_driver_ops_for_disk(ctx, &queue);
    if (requests == NULL || request_count < 2 ||
        request_count > KB_LINUX_BLOCK_WRITE_BATCH_MAX)
    {
        return -22;
    }
    if (ops == NULL || ops->disk_write_batch == NULL) {
        for (size_t i = 0; i < request_count; ++i) {
            const int status = shim_blk_disk_write(
                ctx,
                requests[i].sector,
                requests[i].buffer,
                requests[i].byte_count);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }
    kb_linux_block_write_request_t batch[KB_LINUX_BLOCK_WRITE_BATCH_MAX];
    int can_stage = 1;
    size_t staged_bytes = 0;
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL || requests[i].byte_count == 0 ||
            requests[i].byte_count >
                KB_LINUX_DMA_WRITE_STAGING_SIZE - staged_bytes)
        {
            can_stage = 0;
            break;
        }
        staged_bytes += requests[i].byte_count;
    }
    kb_device_backend_t *backend = can_stage ?
        kb_shim_current_device_backend() : NULL;
    kb_device_t *device = backend == NULL ? NULL :
        kb_subsystem_dma_default_device(backend);
    if (backend != NULL && device != NULL) {
        if (dma_write_staging_buffer == NULL) {
            dma_write_staging_buffer = kb_subsystem_dma_alloc(
                backend,
                device,
                KB_LINUX_DMA_WRITE_STAGING_SIZE,
                &dma_write_staging_handle);
        }
        if (dma_write_staging_buffer != NULL) {
            size_t offset = 0;
            for (size_t i = 0; i < request_count; ++i) {
                memcpy(
                    dma_write_staging_buffer + offset,
                    requests[i].buffer,
                    requests[i].byte_count);
                batch[i].sector = requests[i].sector;
                batch[i].buffer = dma_write_staging_buffer + offset;
                batch[i].byte_count = requests[i].byte_count;
                offset += requests[i].byte_count;
            }
            return ops->disk_write_batch(queue, batch, request_count);
        }
    }
    for (size_t i = 0; i < request_count; ++i) {
        batch[i].sector = requests[i].sector;
        batch[i].buffer = requests[i].buffer;
        batch[i].byte_count = requests[i].byte_count;
    }
    return ops->disk_write_batch(queue, batch, request_count);
}

static int shim_blk_disk_flush(void *ctx)
{
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops = block_driver_ops_for_disk(ctx, &queue);
    if (ops == NULL) {
        return -19;
    }
    return ops->disk_flush == NULL ? 0 : ops->disk_flush(queue);
}

static void shim_blk_disk_attach_io(void *disk)
{
    void *queue = NULL;
    const kb_linux_block_driver_ops_t *ops = block_driver_ops_for_disk(disk, &queue);
    if (ops == NULL || (ops->disk_read == NULL && ops->disk_write == NULL)) {
        return;
    }
    kb_block_subsystem_disk_set_io(disk, disk, shim_blk_disk_read, shim_blk_disk_write);
    kb_block_subsystem_disk_set_read_batch(disk, shim_blk_disk_read_batch);
    kb_block_subsystem_disk_set_write_batch(disk, shim_blk_disk_write_batch);
    kb_block_subsystem_disk_set_write_flags(disk, shim_blk_disk_write_flags);
    kb_block_subsystem_disk_set_flush(disk, shim_blk_disk_flush);
}

void *kb_linux_block_tag_set_driver_data(void *tag_set)
{
    if (tag_set == NULL) {
        return NULL;
    }
    return read_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_DRIVER_DATA_OFFSET);
}

static int shim_blk_tagset_prepare(void *tag_set)
{
    if (kb_block_subsystem_tagset_prepare(tag_set) != 0) {
        return -12;
    }
    void *tag_array = kb_block_subsystem_tagset_array(tag_set);
    if (tag_array == NULL) {
        return -12;
    }
    write_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_TAGS_OFFSET, tag_array);
    return 0;
}

static size_t shim_blk_request_queue_index(const shim_linux_request_t *request)
{
    if (request == NULL || request->driver_ops == NULL || request->driver_ops->queue_index == NULL) {
        return 0;
    }
    return request->driver_ops->queue_index(request->hctx_storage.driver_data);
}

static uint32_t shim_blk_tagset_alloc_tag(
    void *tag_set,
    const kb_linux_block_driver_ops_t *driver_ops,
    void *driver_data)
{
    if (tag_set == NULL || shim_blk_tagset_prepare(tag_set) != 0) {
        return 1;
    }
    size_t queue_index = 0;
    if (driver_ops != NULL && driver_ops->queue_index != NULL) {
        queue_index = driver_ops->queue_index(driver_data);
    }
    return kb_block_subsystem_tagset_alloc_tag(tag_set, queue_index);
}

static void shim_blk_request_init(
    shim_linux_request_t *request,
    void *queue,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue,
    const kb_linux_block_driver_ops_t *driver_ops)
{
    void *tag_set = kb_block_subsystem_queue_tag_set(queue);
    memset(request, 0, sizeof(*request));
    request->hctx = &request->hctx_storage;
    request->queue = queue;
    request->cmd_flags = op;
    request->tag = shim_blk_tagset_alloc_tag(tag_set, driver_ops, driver_data);
    request->special = request->nvme_cmd;
    request->ctrl = ctrl;
    request->owns_queue = owns_queue ? 1u : 0u;
    request->hctx_storage.driver_data = driver_data;
    request->driver_ops = driver_ops;
}

static shim_linux_request_t *shim_blk_request_alloc(
    void *queue,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue,
    const kb_linux_block_driver_ops_t *driver_ops)
{
    if (queue == NULL || kb_block_subsystem_queue_tag_set(queue) == NULL || ctrl == NULL || driver_data == NULL) {
        return NULL;
    }

    shim_linux_request_t *request = owns_queue ? NULL : cached_request_take();
    if (request == NULL) {
        request = calloc(1, sizeof(*request));
    }
    if (request == NULL) {
        return NULL;
    }

    shim_blk_request_init(request, queue, ctrl, driver_data, op, owns_queue, driver_ops);
    if (request->tag == UINT32_MAX) {
        free(request);
        return NULL;
    }
    return request;
}

void *kb_linux_block_alloc_driver_request(
    void *tag_set,
    void *ctrl,
    void *driver_data,
    unsigned int op,
    int owns_queue)
{
    if (tag_set == NULL || ctrl == NULL || driver_data == NULL) {
        return NULL;
    }
    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops == NULL) {
        return NULL;
    }

    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }

    shim_linux_request_t *request = shim_blk_request_alloc(queue, ctrl, driver_data, op, owns_queue, driver_ops);
    if (request == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }
    return request;
}

static void shim_blk_tagset_bind_request(shim_linux_request_t *request, void *tag_set)
{
    if (request == NULL || tag_set == NULL || shim_blk_tagset_prepare(tag_set) != 0) {
        return;
    }

    size_t queue_index = shim_blk_request_queue_index(request);
    if (kb_block_subsystem_tagset_bind_request(tag_set, queue_index, request->tag, request) != 0) {
        return;
    }
    if (trace_block_enabled()) {
        fprintf(
            stderr,
            "kobox blk-mq: bind tagset=%p queue=%zu tag=%u request=%p\n",
            tag_set,
            queue_index,
            request->tag,
            (void *)request);
    }
}

static void shim_blk_tagset_unbind_request(shim_linux_request_t *request)
{
    if (request == NULL || request->queue == NULL) {
        return;
    }

    void *tag_set = kb_block_subsystem_queue_tag_set(request->queue);
    if (tag_set == NULL || request->hctx_storage.driver_data == NULL) {
        return;
    }

    kb_block_subsystem_tagset_unbind_request(
        tag_set,
        shim_blk_request_queue_index(request),
        request->tag,
        request);
}

static void *shim_blk_alloc_disk_with_queue(void *queue)
{
    void *disk = kb_block_subsystem_disk_alloc();
    if (disk == NULL) {
        return NULL;
    }
    void *part0 = kb_block_subsystem_block_device_alloc();
    if (part0 == NULL) {
        kb_block_subsystem_object_free(disk);
        return NULL;
    }

    write_ptr((unsigned char *)disk + KB_LINUX_6_8_GENDISK_PART0_OFFSET, part0);
    write_ptr((unsigned char *)disk + KB_LINUX_6_8_GENDISK_QUEUE_OFFSET, queue);
    if (kb_block_subsystem_disk_attach(disk, queue, part0) != 0) {
        kb_block_subsystem_object_free(part0);
        kb_block_subsystem_object_free(disk);
        return NULL;
    }
    return disk;
}

void *kb_blk_alloc_disk(int node, void *lock_class_key)
{
    (void)node;
    (void)lock_class_key;
    void *queue = kb_block_subsystem_queue_alloc(NULL);
    if (queue == NULL) {
        return NULL;
    }
    void *disk = shim_blk_alloc_disk_with_queue(queue);
    if (disk == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }
    return disk;
}

void *kb_blk_mq_alloc_disk(void *tag_set, void *queuedata, void *lock_class_key)
{
    (void)queuedata;
    (void)lock_class_key;
    if (tag_set == NULL) {
        return NULL;
    }

    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }
    void *disk = shim_blk_alloc_disk_with_queue(queue);
    if (disk == NULL) {
        kb_block_subsystem_object_free(queue);
        return NULL;
    }

    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops != NULL && driver_ops->track_tag_set != NULL) {
        driver_ops->track_tag_set(tag_set);
    }
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: alloc_disk tag_set=%p disk=%p queue=%p\n", tag_set, disk, queue);
    }
    return disk;
}

void *kb_blk_mq_init_queue(void *tag_set)
{
    if (tag_set == NULL) {
        return NULL;
    }
    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == NULL) {
        return NULL;
    }

    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops != NULL && driver_ops->track_tag_set != NULL) {
        driver_ops->track_tag_set(tag_set);
    }
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: init_queue tag_set=%p queue=%p\n", tag_set, (void *)queue);
    }
    return queue;
}

int kb_blk_mq_alloc_tag_set(void *tag_set)
{
    if (tag_set == NULL) {
        return -22;
    }
    int result = shim_blk_tagset_prepare(tag_set);
    if (result != 0) {
        return result;
    }
    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops != NULL && driver_ops->track_tag_set != NULL) {
        driver_ops->track_tag_set(tag_set);
    }
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: alloc_tag_set tag_set=%p driver=%s\n",
            tag_set,
            driver_ops == NULL || driver_ops->name == NULL ? "(unknown)" : driver_ops->name);
    }
    return 0;
}

void kb_blk_mq_free_tag_set(void *tag_set)
{
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: free_tag_set tag_set=%p\n", tag_set);
    }
    kb_block_subsystem_tagset_free(tag_set);
    cached_aux_dma_discard_unused();
    cached_request_discard_all();
}

void kb_blk_mq_destroy_queue(void *queue)
{
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: destroy_queue queue=%p\n", queue);
    }
    kb_block_subsystem_queue_destroy(queue);
}

void *kb_blk_mq_alloc_request(void *queue, unsigned int op, unsigned int flags)
{
    (void)flags;
    if (queue == NULL) {
        return NULL;
    }

    void *tag_set = kb_block_subsystem_queue_tag_set(queue);
    if (tag_set == NULL) {
        return NULL;
    }

    const kb_linux_block_driver_ops_t *driver_ops = block_driver_ops_for_tag_set(tag_set);
    if (driver_ops == NULL || driver_ops->request_driver_data == NULL || driver_ops->request_ctrl == NULL) {
        return NULL;
    }

    void *driver_data = driver_ops->request_driver_data(queue, tag_set);
    if (driver_data == NULL) {
        if (trace_block_enabled()) {
            fprintf(stderr, "kobox blk-mq: alloc_request missing driver_data tag_set=%p\n", tag_set);
        }
        return NULL;
    }

    void *ctrl = driver_ops->request_ctrl(tag_set, driver_data);
    if (ctrl == NULL) {
        if (trace_block_enabled()) {
            fprintf(stderr, "kobox blk-mq: alloc_request missing ctrl tag_set=%p driver_data=%p\n", tag_set, driver_data);
        }
        return NULL;
    }

    shim_linux_request_t *request = shim_blk_request_alloc(queue, ctrl, driver_data, op, 0, driver_ops);
    if (request == NULL) {
        return NULL;
    }

    if (trace_block_enabled()) {
        fprintf(
            stderr,
            "kobox blk-mq: alloc_request queue=%p request=%p hctx=%p driver_data=%p op=0x%x driver=%s\n",
            queue,
            (void *)request,
            (void *)&request->hctx_storage,
            driver_data,
            op,
            driver_ops->name == NULL ? "(unknown)" : driver_ops->name);
    }
    return request;
}

void *kb_linux_block_request_command(void *request)
{
    shim_linux_request_t *rq = request;
    return rq == NULL ? NULL : rq->special;
}

void *kb_linux_block_request_driver_data(void *request)
{
    shim_linux_request_t *rq = request;
    return rq == NULL ? NULL : rq->hctx_storage.driver_data;
}

void *kb_linux_block_request_tag_set(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || rq->queue == NULL) {
        return NULL;
    }
    return kb_block_subsystem_queue_tag_set(rq->queue);
}

void *kb_linux_block_request_tagset_request(void *request)
{
    shim_linux_request_t *rq = request;
    void *tag_set = kb_linux_block_request_tag_set(request);
    if (rq == NULL || tag_set == NULL) {
        return NULL;
    }
    return kb_block_subsystem_tagset_request(tag_set, shim_blk_request_queue_index(rq), rq->tag);
}

uint32_t kb_linux_block_request_tag(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq == NULL ? 0 : rq->tag;
}

uint8_t kb_linux_block_request_generation(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq == NULL ? 0 : (uint8_t)(rq->reserved_120[0] & 0xfu);
}

int kb_linux_block_request_completed(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq != NULL &&
        __atomic_load_n(&rq->completed, __ATOMIC_ACQUIRE) != 0;
}

unsigned int kb_linux_block_request_end_status(const void *request)
{
    const shim_linux_request_t *rq = request;
    return rq == NULL ? 0 : rq->end_status;
}

void kb_linux_block_request_set_result_status(void *request, uint64_t result, uint16_t status)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL) {
        return;
    }
    rq->result = result;
    rq->status = status;
}

void kb_linux_block_request_mark_complete(void *request, unsigned int status)
{
    if (request == NULL) {
        return;
    }

    shim_linux_request_t *rq = request;
    rq->end_status = status;
    __atomic_store_n(&rq->completed, 1u, __ATOMIC_RELEASE);
    if (trace_block_enabled()) {
        fprintf(stderr, "kobox blk-mq: request complete request=%p status=0x%x\n", request, status);
    }
}

static int map_request_dma_raw(
    shim_linux_request_t *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma,
    kb_device_backend_t **out_backend,
    kb_device_t **out_device)
{
    if (request == NULL || cpu_addr == NULL || length == 0 || out_dma == NULL ||
        out_backend == NULL || out_device == NULL)
    {
        return -22;
    }

    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        fprintf(stderr,
            "kobox blk-mq: dma map no default device backend=%p request=%p cpu=%p len=%u dir=%u\n",
            (void *)backend,
            (void *)request,
            cpu_addr,
            length,
            (unsigned)direction);
        return -19;
    }

    kb_status_t dma_status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map(backend, device, cpu_addr, length, direction, &dma_status);
    if (dma_status == KB_ERR_UNSUPPORTED) {
        fprintf(stderr,
            "kobox blk-mq: dma map unsupported backend=%p device=%p request=%p cpu=%p len=%u dir=%u\n",
            (void *)backend,
            (void *)device,
            (void *)request,
            cpu_addr,
            length,
            (unsigned)direction);
        return -95;
    }
    if (dma_status != KB_OK) {
        fprintf(stderr,
            "kobox blk-mq: dma map failed status=%d backend=%p device=%p request=%p cpu=%p len=%u dir=%u\n",
            (int)dma_status,
            (void *)backend,
            (void *)device,
            (void *)request,
            cpu_addr,
            length,
            (unsigned)direction);
        return -5;
    }

    *out_dma = dma_addr;
    *out_backend = backend;
    *out_device = device;
    return 0;
}

int kb_linux_block_request_map_dma_pages(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_page_dma,
    size_t out_capacity)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || cpu_addr == NULL || length == 0 || out_page_dma == NULL || out_capacity == 0) {
        return -22;
    }
    kb_linux_block_request_unmap_dma(request);

    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        fprintf(stderr,
            "kobox blk-mq: dma map pages no default device backend=%p request=%p cpu=%p len=%u dir=%u\n",
            (void *)backend,
            request,
            cpu_addr,
            length,
            (unsigned)direction);
        return -19;
    }

    kb_status_t dma_status = kb_subsystem_dma_preallocated_pages(
        device, cpu_addr, length, out_page_dma, out_capacity);
    if (dma_status == KB_OK) {
        rq->dma_addr = out_page_dma[0];
        rq->dma_len = length;
        rq->dma_dir = direction;
        rq->dma_backend = backend;
        rq->dma_device = device;
        rq->dma_preallocated = 1;
        return 0;
    }
    dma_status = kb_subsystem_dma_map_pages(
        backend,
        device,
        cpu_addr,
        length,
        direction,
        out_page_dma,
        out_capacity);
    if (dma_status != KB_OK) {
        fprintf(stderr,
            "kobox blk-mq: dma map pages %s status=%d backend=%p device=%p request=%p cpu=%p len=%u dir=%u pages=%zu\n",
            dma_status == KB_ERR_UNSUPPORTED ? "unsupported" : "failed",
            (int)dma_status,
            (void *)backend,
            (void *)device,
            request,
            cpu_addr,
            length,
            (unsigned)direction,
            out_capacity);
        return dma_status == KB_ERR_UNSUPPORTED ? -95 : -5;
    }

    rq->dma_addr = out_page_dma[0];
    rq->dma_len = length;
    rq->dma_dir = direction;
    rq->dma_backend = backend;
    rq->dma_device = device;
    return 0;
}

void kb_linux_block_request_unmap_dma(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL) {
        return;
    }
    if (rq->dma_len != 0 && !rq->dma_preallocated) {
        kb_subsystem_dma_unmap(
            rq->dma_backend,
            rq->dma_device,
            rq->dma_addr,
            rq->dma_len,
            rq->dma_dir);
    }
    rq->dma_addr = 0;
    rq->dma_len = 0;
    rq->dma_backend = NULL;
    rq->dma_device = NULL;
    rq->dma_preallocated = 0;
}

int kb_linux_block_request_map_owned_aux_dma(
    void *request,
    void *cpu_addr,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma)
{
    kb_linux_block_request_unmap_owned_aux_dma(request);
    shim_linux_request_t *rq = request;
    uint64_t dma_addr = 0;
    kb_device_backend_t *backend = NULL;
    kb_device_t *device = NULL;
    int result = map_request_dma_raw(
        rq,
        cpu_addr,
        length,
        direction,
        &dma_addr,
        &backend,
        &device);
    if (result != 0) {
        return result;
    }
    rq->prp_list_cpu = cpu_addr;
    rq->prp_list_dma = dma_addr;
    rq->prp_list_len = length;
    rq->prp_list_dma_dir = direction;
    rq->prp_list_backend = backend;
    rq->prp_list_device = device;
    *out_dma = dma_addr;
    return 0;
}

int kb_linux_block_request_map_cached_aux_dma(
    void *request,
    const void *data,
    uint32_t length,
    kb_dma_dir_t direction,
    uint64_t *out_dma)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || data == NULL || length == 0 ||
        length > KB_LINUX_CACHED_AUX_DMA_SIZE || out_dma == NULL ||
        direction != KB_DMA_TO_DEVICE)
    {
        return -22;
    }
    kb_linux_block_request_unmap_owned_aux_dma(request);

    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (backend == NULL || device == NULL) {
        return -19;
    }

    kb_linux_cached_aux_dma_t *entry = NULL;
    const uint64_t profile_cache_start = KB_LINUX_BLOCK_PROFILE_BEGIN();
    cached_aux_dma_lock_acquire();
    for (size_t i = 0; i < KB_LINUX_CACHED_AUX_DMA_MAX; i++) {
        if (cached_aux_dma[i].owner == NULL &&
            cached_aux_dma[i].cpu_addr != NULL &&
            cached_aux_dma[i].backend == backend &&
            cached_aux_dma[i].device == device)
        {
            entry = &cached_aux_dma[i];
            break;
        }
    }
    if (entry != NULL) {
        KB_LINUX_BLOCK_PROFILE_RECORD(
            KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_HIT,
            profile_cache_start,
            0);
    } else {
        for (size_t i = 0; i < KB_LINUX_CACHED_AUX_DMA_MAX; i++) {
            if (cached_aux_dma[i].cpu_addr == NULL) {
                entry = &cached_aux_dma[i];
                break;
            }
        }
        if (entry != NULL) {
            uint64_t dma_addr = 0;
            void *cpu_addr = kb_subsystem_dma_alloc(
                backend,
                device,
                KB_LINUX_CACHED_AUX_DMA_SIZE,
                &dma_addr);
            if (cpu_addr == NULL || dma_addr == 0) {
                entry = NULL;
            } else {
                entry->backend = backend;
                entry->device = device;
                entry->cpu_addr = cpu_addr;
                entry->dma_addr = dma_addr;
            }
        }
        KB_LINUX_BLOCK_PROFILE_RECORD(
            KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_MISS,
            profile_cache_start,
            0);
    }
    if (entry == NULL) {
        cached_aux_dma_lock_release();
        return -12;
    }

    memcpy(entry->cpu_addr, data, length);
    atomic_thread_fence(memory_order_seq_cst);
    entry->owner = rq;
    rq->prp_list_cpu = entry->cpu_addr;
    rq->prp_list_dma = entry->dma_addr;
    rq->prp_list_len = length;
    rq->prp_list_dma_dir = direction;
    rq->prp_list_backend = backend;
    rq->prp_list_device = device;
    rq->prp_list_cached = 1;
    *out_dma = entry->dma_addr;
    cached_aux_dma_lock_release();
    return 0;
}

void kb_linux_block_request_unmap_owned_aux_dma(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL || rq->prp_list_cpu == NULL) {
        return;
    }
    if (rq->prp_list_cached) {
        cached_aux_dma_lock_acquire();
        for (size_t i = 0; i < KB_LINUX_CACHED_AUX_DMA_MAX; i++) {
            if (cached_aux_dma[i].owner == rq &&
                cached_aux_dma[i].cpu_addr == rq->prp_list_cpu &&
                cached_aux_dma[i].dma_addr == rq->prp_list_dma)
            {
                cached_aux_dma[i].owner = NULL;
                break;
            }
        }
        cached_aux_dma_lock_release();
    } else if (rq->prp_list_dma != 0 && rq->prp_list_len != 0) {
        kb_subsystem_dma_unmap(
            rq->prp_list_backend,
            rq->prp_list_device,
            rq->prp_list_dma,
            rq->prp_list_len,
            rq->prp_list_dma_dir);
    }
    if (!rq->prp_list_cached) {
        free(rq->prp_list_cpu);
    }
    rq->prp_list_cpu = NULL;
    rq->prp_list_dma = 0;
    rq->prp_list_len = 0;
    rq->prp_list_cached = 0;
    rq->prp_list_backend = NULL;
    rq->prp_list_device = NULL;
}

int kb_blk_rq_map_kern(void *queue, void *request, void *buffer, unsigned int length, unsigned int gfp)
{
    (void)queue;
    if (request == NULL || buffer == NULL || length == 0) {
        return -22;
    }

    shim_linux_request_t *rq = request;
    if (rq->driver_ops == NULL || rq->driver_ops->map_kernel_buffer == NULL) {
        return -95;
    }
    return rq->driver_ops->map_kernel_buffer(request, buffer, length, gfp);
}

void kb_blk_mq_start_request(void *request)
{
    shim_linux_request_t *rq = request;
    if (rq == NULL) {
        return;
    }
    rq->started = 1;
    __atomic_store_n(&rq->completed, 0u, __ATOMIC_RELEASE);
    rq->end_status = 0;
}

void kb_blk_mq_complete_request(void *request)
{
    kb_linux_block_request_mark_complete(request, 0);
}

int kb_blk_mq_complete_request_remote(void *request)
{
    kb_linux_block_request_mark_complete(request, 0);
    return 1;
}

void kb_blk_mq_end_request(void *request, unsigned int status)
{
    kb_linux_block_request_mark_complete(request, status);
}

void kb_blk_mq_end_request_batch(void *batch)
{
    if (batch == NULL) {
        return;
    }

    shim_linux_request_t *rq = read_ptr(batch);
    while (rq != NULL) {
        shim_linux_request_t *next = rq->batch_next;
        rq->batch_next = NULL;
        kb_linux_block_request_mark_complete(rq, rq->status);
        rq = next;
    }
}

void kb_blk_mq_free_request(void *request)
{
    if (request == NULL) {
        return;
    }

    shim_linux_request_t *rq = request;
    const uint64_t profile_free_start = KB_LINUX_BLOCK_PROFILE_BEGIN();
    shim_blk_tagset_unbind_request(rq);
    const uint64_t profile_unmap_start = KB_LINUX_BLOCK_PROFILE_BEGIN();
    kb_linux_block_request_unmap_owned_aux_dma(rq);
    kb_linux_block_request_unmap_dma(rq);
    KB_LINUX_BLOCK_PROFILE_RECORD(
        KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK,
        profile_unmap_start,
        0);
    const int cacheable = rq->owns_queue == 0;
    if (rq->owns_queue) {
        kb_block_subsystem_object_free(rq->queue);
    }
    if (!cacheable || !cached_request_store(rq)) {
        free(request);
    }
    KB_LINUX_BLOCK_PROFILE_RECORD(
        KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL,
        profile_free_start,
        0);
}

static int kb_blk_submit_rq_internal(void *request, int at_head, int last)
{
    (void)at_head;
    if (request == NULL) {
        return -22;
    }

    shim_linux_request_t *rq = request;
    shim_linux_hctx_t *hctx = rq->hctx;
    void *tag_set = kb_block_subsystem_queue_tag_set(rq->queue);
    if (hctx == NULL || tag_set == NULL) {
        return -22;
    }

    unsigned char *ops = read_ptr((unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_OPS_OFFSET);
    if (ops == NULL) {
        return -22;
    }

    int (*queue_rq)(void *hctx, void *bd) =
        (int (*)(void *, void *))read_ptr(ops + KB_LINUX_BLK_MQ_OPS_QUEUE_RQ_OFFSET);
    if (queue_rq == NULL) {
        return -22;
    }

    if (rq->driver_ops != NULL && rq->driver_ops->before_execute != NULL) {
        const uint64_t profile_before_start = KB_LINUX_BLOCK_PROFILE_BEGIN();
        int result = rq->driver_ops->before_execute(request);
        KB_LINUX_BLOCK_PROFILE_RECORD(
            KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE,
            profile_before_start,
            0);
        if (result != 0) {
            return result;
        }
    }

    shim_linux_queue_data_t *bd = &rq->queue_data;
    bd->rq = rq;
    void (*commit_rqs)(void *hctx) =
        (void (*)(void *))read_ptr(ops + KB_LINUX_BLK_MQ_OPS_COMMIT_RQS_OFFSET);
    bd->last = last || commit_rqs == NULL ? 1 : 0;

    shim_blk_tagset_bind_request(rq, tag_set);
    const uint64_t profile_submit_start = KB_LINUX_BLOCK_PROFILE_BEGIN();
    int result = queue_rq(hctx, bd);
    KB_LINUX_BLOCK_PROFILE_RECORD(
        KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT,
        profile_submit_start,
        0);
    if (trace_block_enabled()) {
        fprintf(
            stderr,
            "kobox blk-mq: queue_rq request=%p hctx=%p bd=%p result=%d driver_data=%p driver=%s\n",
            request,
            (void *)hctx,
            (void *)bd,
            result,
            hctx->driver_data,
            rq->driver_ops == NULL || rq->driver_ops->name == NULL ? "(unknown)" : rq->driver_ops->name);
    }
    if (result != 0) {
        return result;
    }

    return 0;
}

int kb_blk_submit_rq(void *request, int at_head)
{
    return kb_blk_submit_rq_internal(request, at_head, 1);
}

int kb_blk_submit_rq_batch(void *request, int at_head, int last)
{
    return kb_blk_submit_rq_internal(request, at_head, last);
}

int kb_blk_commit_rqs(void *request)
{
    if (request == NULL) {
        return -22;
    }

    shim_linux_request_t *rq = request;
    shim_linux_hctx_t *hctx = rq->hctx;
    void *tag_set = kb_block_subsystem_queue_tag_set(rq->queue);
    if (hctx == NULL || tag_set == NULL) {
        return -22;
    }

    unsigned char *ops = read_ptr(
        (unsigned char *)tag_set + KB_LINUX_BLK_MQ_TAG_SET_OPS_OFFSET);
    if (ops == NULL) {
        return -22;
    }
    void (*commit_rqs)(void *hctx) =
        (void (*)(void *))read_ptr(ops + KB_LINUX_BLK_MQ_OPS_COMMIT_RQS_OFFSET);
    if (commit_rqs != NULL) {
        commit_rqs(hctx);
    }
    return 0;
}

int kb_blk_complete_rq(void *request)
{
    if (request == NULL) {
        return -22;
    }
    shim_linux_request_t *rq = request;
    int result = 0;

    if (rq->driver_ops != NULL && rq->driver_ops->complete_execute != NULL) {
        const uint64_t profile_wait_start = KB_LINUX_BLOCK_PROFILE_BEGIN();
        result = rq->driver_ops->complete_execute(request);
        KB_LINUX_BLOCK_PROFILE_RECORD(
            KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT,
            profile_wait_start,
            0);
        return result;
    }
    if (kb_linux_block_request_completed(rq)) {
        return rq->end_status == 0 ? 0 : -5;
    }
    return -95;
}

int kb_blk_execute_rq(void *request, int at_head)
{
    const int submit_status = kb_blk_submit_rq(request, at_head);
    return submit_status != 0 ? submit_status : kb_blk_complete_rq(request);
}

int kb_blk_status_to_errno(unsigned int status)
{
    switch (status) {
    case 0:
        return 0;
    case 1:
        return -95;
    case 2:
        return -110;
    case 3:
        return -28;
    case 4:
        return -67;
    case 5:
        return -121;
    case 6:
        return -52;
    case 7:
        return -61;
    case 8:
        return -84;
    case 9:
        return -12;
    case 10:
        return -16;
    case 11:
        return -11;
    case 16:
        return -19;
    default:
        return -5;
    }
}

void kb_blk_mq_map_queues(void *queue_map)
{
    (void)queue_map;
}

int kb_blk_mq_pci_map_queues(void *queue_map, void *pdev, int offset)
{
    (void)queue_map;
    (void)pdev;
    (void)offset;
    return 0;
}

void kb_blk_mq_tagset_busy_iter(void *tag_set, void *fn, void *priv)
{
    (void)fn;
    (void)priv;
    kb_block_subsystem_tagset_note_busy_iter(tag_set);
}

void kb_blk_mq_tagset_wait_completed_request(void *tag_set)
{
    kb_block_subsystem_tagset_note_wait_completed(tag_set);
}

void kb_blk_mq_update_nr_hw_queues(void *tag_set, unsigned int nr_hw_queues)
{
    kb_block_subsystem_tagset_set_hw_queues(tag_set, nr_hw_queues);
}

void kb_blk_put_queue(void *queue)
{
    kb_block_subsystem_queue_put(queue);
}

void kb_blk_queue_chunk_sectors(void *queue, unsigned int sectors)
{
    kb_block_subsystem_queue_set_chunk_sectors(queue, sectors);
}

void kb_blk_queue_dma_alignment(void *queue, int mask)
{
    kb_block_subsystem_queue_set_dma_alignment(queue, (uint32_t)mask);
}

void kb_blk_queue_flag_set(unsigned int flag, void *queue)
{
    kb_block_subsystem_queue_flag_set(queue, flag);
}

void kb_blk_queue_io_min(void *queue, unsigned int size)
{
    kb_block_subsystem_queue_set_io_min(queue, size);
}

void kb_blk_queue_io_opt(void *queue, unsigned int size)
{
    kb_block_subsystem_queue_set_io_opt(queue, size);
}

void kb_blk_queue_logical_block_size(void *queue, unsigned int size)
{
    kb_block_subsystem_queue_set_logical_block_size(queue, size);
}

void kb_blk_queue_max_discard_sectors(void *queue, unsigned int sectors)
{
    kb_block_subsystem_queue_set_max_discard_sectors(queue, sectors);
}

void kb_blk_queue_max_discard_segments(void *queue, unsigned int segments)
{
    kb_block_subsystem_queue_set_max_discard_segments(queue, segments);
}

void kb_blk_queue_max_hw_sectors(void *queue, unsigned int sectors)
{
    kb_block_subsystem_queue_set_max_hw_sectors(queue, sectors);
}

void kb_blk_queue_max_segments(void *queue, unsigned int segments)
{
    kb_block_subsystem_queue_set_max_segments(queue, segments);
}

void kb_blk_queue_max_write_zeroes_sectors(void *queue, unsigned int sectors)
{
    kb_block_subsystem_queue_set_max_write_zeroes_sectors(queue, sectors);
}

void kb_blk_queue_max_zone_append_sectors(void *queue, unsigned int sectors)
{
    kb_block_subsystem_queue_set_max_zone_append_sectors(queue, sectors);
}

void kb_blk_queue_physical_block_size(void *queue, unsigned int size)
{
    kb_block_subsystem_queue_set_physical_block_size(queue, size);
}

void kb_blk_queue_virt_boundary(void *queue, unsigned long mask)
{
    kb_block_subsystem_queue_set_virt_boundary(queue, mask);
}

void kb_blk_queue_write_cache(void *queue, bool write_cache, bool fua)
{
    kb_block_subsystem_queue_set_write_cache(queue, write_cache, fua);
}

void kb_blk_queue_bounce_limit(void *queue, uint64_t dma_mask)
{
    (void)queue;
    (void)dma_mask;
}

void kb_blk_queue_update_dma_alignment(void *queue, int mask)
{
    kb_block_subsystem_queue_set_dma_alignment(queue, (uint32_t)mask);
}

int kb_device_add_disk(void *parent, void *disk, void *groups)
{
    shim_blk_disk_attach_io(disk);
    return kb_block_subsystem_disk_register(parent, disk, groups);
}

void kb_del_gendisk(void *disk)
{
    kb_block_subsystem_disk_unregister(disk);
}

void kb_blk_mark_disk_dead(void *disk)
{
    kb_block_subsystem_disk_mark_dead(disk);
}

int kb_blk_revalidate_disk_zones(void *disk, void *update_driver_data)
{
    (void)update_driver_data;
    return kb_block_subsystem_disk_revalidate_zones(disk);
}

void kb_blk_set_stacking_limits(void *limits)
{
    (void)limits;
}

int kb_blk_stack_limits(void *top, void *bottom, unsigned int start)
{
    (void)top;
    (void)bottom;
    (void)start;
    return 0;
}

void kb_disk_set_zoned(void *disk, unsigned int model)
{
    kb_block_subsystem_disk_set_zoned(disk, model);
}

void kb_disk_update_readahead(void *disk)
{
    kb_block_subsystem_disk_update_readahead(disk);
}

void kb_put_disk(void *disk)
{
    kb_block_subsystem_disk_put(disk);
}

void kb_set_capacity(void *disk, uint64_t sectors)
{
    kb_block_subsystem_disk_set_capacity(disk, sectors);
}

int kb_set_capacity_and_notify(void *disk, uint64_t sectors)
{
    return kb_block_subsystem_disk_set_capacity_and_notify(disk, sectors);
}

void kb_set_disk_ro(void *disk, int read_only)
{
    kb_block_subsystem_disk_set_read_only(disk, read_only);
}
