#include "linux_subsystem/block/block.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_BLOCK_SUBSYSTEM_OBJECT_SIZE = 4096,
    KB_BLOCK_SUBSYSTEM_QUEUE_MAX = 4,
    KB_BLOCK_SUBSYSTEM_TAGS_PER_QUEUE = 64,
};

typedef struct kb_block_queue {
    void *tag_set;
} kb_block_queue_t;

typedef struct kb_block_queue_state {
    void *queue;
    void *tag_set;
    kb_block_queue_limits_t limits;
    uint32_t put_count;
    uint32_t destroy_count;
    struct kb_block_queue_state *next;
} kb_block_queue_state_t;

typedef struct kb_block_disk_state {
    void *disk;
    void *queue;
    void *part0;
    void *parent;
    void *groups;
    uint64_t capacity_sectors;
    uint32_t disk_number;
    uint32_t registered;
    uint32_t read_only;
    uint32_t notify_count;
    uint32_t put_count;
    uint32_t dead;
    uint32_t zoned_model;
    uint32_t readahead_update_count;
    uint32_t zone_revalidate_count;
    void *io_ctx;
    kb_block_disk_read_fn read_fn;
    kb_block_disk_read_batch_fn read_batch_fn;
    kb_block_disk_write_fn write_fn;
    kb_block_disk_write_batch_fn write_batch_fn;
    kb_block_disk_write_flags_fn write_flags_fn;
    kb_block_disk_flush_fn flush_fn;
    uint64_t read_count;
    uint64_t write_count;
    uint64_t bytes_read;
    uint64_t bytes_written;
    struct kb_block_disk_state *next;
} kb_block_disk_state_t;

typedef struct kb_block_mq_tags {
    uint32_t nr_tags;
    uint32_t nr_reserved_tags;
    uint32_t active_queues;
    unsigned char reserved_00c[0x90 - 0x0c];
    void **rqs;
} kb_block_mq_tags_t;

typedef struct kb_block_tagset_state {
    void *tag_set;
    void *tag_array[KB_BLOCK_SUBSYSTEM_QUEUE_MAX];
    kb_block_mq_tags_t tags[KB_BLOCK_SUBSYSTEM_QUEUE_MAX];
    void *rqs[KB_BLOCK_SUBSYSTEM_QUEUE_MAX][KB_BLOCK_SUBSYSTEM_TAGS_PER_QUEUE];
    uint32_t next_tag[KB_BLOCK_SUBSYSTEM_QUEUE_MAX];
    uint32_t prepared_count;
    uint32_t nr_hw_queues;
    uint32_t map_queues_count;
    uint32_t pci_map_queues_count;
    uint32_t busy_iter_count;
    uint32_t wait_completed_count;
    struct kb_block_tagset_state *next;
} kb_block_tagset_state_t;

_Static_assert(offsetof(kb_block_mq_tags_t, rqs) == 0x90, "blk_mq_tags.rqs offset");

static kb_block_tagset_state_t *tagsets;
static kb_block_queue_state_t *queues;
static kb_block_disk_state_t *disks;
static uint32_t next_disk_number = 1;

static kb_block_tagset_state_t *tagset_state_find(void *tag_set)
{
    if (tag_set == NULL) {
        return NULL;
    }

    for (kb_block_tagset_state_t *state = tagsets; state != NULL; state = state->next) {
        if (state->tag_set == tag_set) {
            return state;
        }
    }
    return NULL;
}

static kb_block_tagset_state_t *tagset_state_for(void *tag_set)
{
    kb_block_tagset_state_t *existing = tagset_state_find(tag_set);
    if (existing != NULL || tag_set == NULL) {
        return existing;
    }

    kb_block_tagset_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    state->tag_set = tag_set;
    state->nr_hw_queues = 1;
    for (size_t i = 0; i < KB_BLOCK_SUBSYSTEM_QUEUE_MAX; i++) {
        state->tag_array[i] = &state->tags[i];
        state->tags[i].nr_tags = KB_BLOCK_SUBSYSTEM_TAGS_PER_QUEUE;
        state->tags[i].active_queues = 1;
        state->tags[i].rqs = state->rqs[i];
        state->next_tag[i] = 1;
    }
    state->next = tagsets;
    tagsets = state;
    return state;
}

static void tagset_state_remove(void *tag_set)
{
    kb_block_tagset_state_t **link = &tagsets;
    while (*link != NULL) {
        kb_block_tagset_state_t *state = *link;
        if (state->tag_set == tag_set) {
            *link = state->next;
            free(state);
            return;
        }
        link = &state->next;
    }
}

static kb_block_queue_state_t *queue_state_for(const void *queue)
{
    if (queue == NULL) {
        return NULL;
    }
    for (kb_block_queue_state_t *state = queues; state != NULL; state = state->next) {
        if (state->queue == queue) {
            return state;
        }
    }
    return NULL;
}

static kb_block_queue_state_t *queue_state_create(void *queue, void *tag_set)
{
    kb_block_queue_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    state->queue = queue;
    state->tag_set = tag_set;
    state->limits.logical_block_size = 512;
    state->limits.physical_block_size = 512;
    state->limits.max_hw_sectors = 1024;
    state->limits.max_segments = 1;
    state->next = queues;
    queues = state;
    return state;
}

static void queue_state_remove(void *queue)
{
    kb_block_queue_state_t **link = &queues;
    while (*link != NULL) {
        kb_block_queue_state_t *state = *link;
        if (state->queue == queue) {
            *link = state->next;
            free(state);
            return;
        }
        link = &state->next;
    }
}

static kb_block_disk_state_t *disk_state_for(const void *disk)
{
    if (disk == NULL) {
        return NULL;
    }
    for (kb_block_disk_state_t *state = disks; state != NULL; state = state->next) {
        if (state->disk == disk) {
            return state;
        }
    }
    return NULL;
}

static kb_block_disk_state_t *disk_state_create(void *disk)
{
    kb_block_disk_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    state->disk = disk;
    state->disk_number = next_disk_number++;
    state->next = disks;
    disks = state;
    return state;
}

static void disk_state_remove(void *disk)
{
    kb_block_disk_state_t **link = &disks;
    while (*link != NULL) {
        kb_block_disk_state_t *state = *link;
        if (state->disk == disk) {
            *link = state->next;
            free(state);
            return;
        }
        link = &state->next;
    }
}

static uint32_t disk_logical_block_size(const kb_block_disk_state_t *state)
{
    if (state == NULL) {
        return 512;
    }
    kb_block_queue_state_t *queue = queue_state_for(state->queue);
    if (queue == NULL || queue->limits.logical_block_size == 0) {
        return 512;
    }
    return queue->limits.logical_block_size;
}

static int disk_io_range_valid(const kb_block_disk_state_t *state, uint64_t sector, size_t byte_count)
{
    if (state == NULL || byte_count == 0) {
        return -22;
    }

    uint64_t offset = 0;
    if (__builtin_mul_overflow(sector, 512ull, &offset)) {
        return -34;
    }
    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)byte_count, &end)) {
        return -34;
    }
    uint64_t capacity_bytes = 0;
    if (__builtin_mul_overflow(state->capacity_sectors, 512ull, &capacity_bytes)) {
        return -34;
    }
    if (end > capacity_bytes) {
        return -34;
    }

    uint32_t logical_block_size = disk_logical_block_size(state);
    if ((offset % logical_block_size) != 0 || (byte_count % logical_block_size) != 0) {
        return -22;
    }
    return 0;
}

void *kb_block_subsystem_queue_alloc(void *tag_set)
{
    kb_block_queue_t *queue = calloc(1, KB_BLOCK_SUBSYSTEM_OBJECT_SIZE);
    if (queue == NULL) {
        return NULL;
    }
    queue->tag_set = tag_set;
    if (queue_state_create(queue, tag_set) == NULL) {
        free(queue);
        return NULL;
    }
    return queue;
}

void *kb_block_subsystem_queue_tag_set(const void *queue)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        return state->tag_set;
    }
    const kb_block_queue_t *block_queue = queue;
    return block_queue == NULL ? NULL : block_queue->tag_set;
}

void kb_block_subsystem_queue_put(void *queue)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->put_count++;
    }
}

void kb_block_subsystem_queue_destroy(void *queue)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->destroy_count++;
    }
}

void kb_block_subsystem_queue_set_logical_block_size(void *queue, uint32_t size)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL && size != 0) {
        state->limits.logical_block_size = size;
    }
}

void kb_block_subsystem_queue_set_physical_block_size(void *queue, uint32_t size)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL && size != 0) {
        state->limits.physical_block_size = size;
    }
}

void kb_block_subsystem_queue_set_io_min(void *queue, uint32_t size)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.io_min = size;
    }
}

void kb_block_subsystem_queue_set_io_opt(void *queue, uint32_t size)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.io_opt = size;
    }
}

void kb_block_subsystem_queue_set_max_hw_sectors(void *queue, uint32_t sectors)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.max_hw_sectors = sectors;
    }
}

void kb_block_subsystem_queue_set_max_segments(void *queue, uint32_t segments)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.max_segments = segments;
    }
}

void kb_block_subsystem_queue_set_max_discard_sectors(void *queue, uint32_t sectors)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.max_discard_sectors = sectors;
    }
}

void kb_block_subsystem_queue_set_max_discard_segments(void *queue, uint32_t segments)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.max_discard_segments = segments;
    }
}

void kb_block_subsystem_queue_set_max_write_zeroes_sectors(void *queue, uint32_t sectors)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.max_write_zeroes_sectors = sectors;
    }
}

void kb_block_subsystem_queue_set_max_zone_append_sectors(void *queue, uint32_t sectors)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.max_zone_append_sectors = sectors;
    }
}

void kb_block_subsystem_queue_set_chunk_sectors(void *queue, uint32_t sectors)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.chunk_sectors = sectors;
    }
}

void kb_block_subsystem_queue_set_dma_alignment(void *queue, uint32_t mask)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.dma_alignment = mask;
    }
}

void kb_block_subsystem_queue_set_virt_boundary(void *queue, uint64_t mask)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.virt_boundary_mask = mask;
    }
}

void kb_block_subsystem_queue_set_write_cache(void *queue, int write_cache, int fua)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL) {
        state->limits.write_cache = write_cache != 0;
        state->limits.fua = fua != 0;
    }
}

void kb_block_subsystem_queue_flag_set(void *queue, uint32_t flag)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state != NULL && flag < 32) {
        state->limits.flags |= 1u << flag;
    }
}

int kb_block_subsystem_queue_limits(const void *queue, kb_block_queue_limits_t *out_limits)
{
    kb_block_queue_state_t *state = queue_state_for(queue);
    if (state == NULL || out_limits == NULL) {
        return -22;
    }
    *out_limits = state->limits;
    return 0;
}

void *kb_block_subsystem_disk_alloc(void)
{
    void *disk = calloc(1, KB_BLOCK_SUBSYSTEM_OBJECT_SIZE);
    if (disk == NULL) {
        return NULL;
    }
    if (disk_state_create(disk) == NULL) {
        free(disk);
        return NULL;
    }
    return disk;
}

int kb_block_subsystem_disk_attach(void *disk, void *queue, void *part0)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL || queue == NULL || part0 == NULL) {
        return -22;
    }
    state->queue = queue;
    state->part0 = part0;
    return 0;
}

int kb_block_subsystem_disk_register(void *parent, void *disk, void *groups)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL) {
        return -22;
    }
    state->parent = parent;
    state->groups = groups;
    state->registered = 1;
    return 0;
}

void kb_block_subsystem_disk_unregister(void *disk)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->registered = 0;
    }
}

void kb_block_subsystem_disk_put(void *disk)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->put_count++;
    }
}

void kb_block_subsystem_disk_set_capacity(void *disk, uint64_t sectors)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->capacity_sectors = sectors;
    }
}

int kb_block_subsystem_disk_set_capacity_and_notify(void *disk, uint64_t sectors)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL) {
        return 0;
    }
    int changed = state->capacity_sectors != sectors;
    state->capacity_sectors = sectors;
    state->notify_count++;
    return changed;
}

void kb_block_subsystem_disk_set_read_only(void *disk, int read_only)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->read_only = read_only != 0;
    }
}

void kb_block_subsystem_disk_mark_dead(void *disk)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->dead = 1;
        state->registered = 0;
    }
}

void kb_block_subsystem_disk_set_zoned(void *disk, uint32_t model)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->zoned_model = model;
    }
}

void kb_block_subsystem_disk_update_readahead(void *disk)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->readahead_update_count++;
    }
}

int kb_block_subsystem_disk_revalidate_zones(void *disk)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL) {
        return -22;
    }
    state->zone_revalidate_count++;
    return 0;
}

int kb_block_subsystem_disk_snapshot(const void *disk, kb_block_disk_snapshot_t *out_snapshot)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL || out_snapshot == NULL) {
        return -22;
    }
    out_snapshot->disk = state->disk;
    out_snapshot->queue = state->queue;
    out_snapshot->part0 = state->part0;
    out_snapshot->parent = state->parent;
    out_snapshot->groups = state->groups;
    out_snapshot->capacity_sectors = state->capacity_sectors;
    out_snapshot->disk_number = state->disk_number;
    out_snapshot->registered = state->registered;
    out_snapshot->read_only = state->read_only;
    out_snapshot->notify_count = state->notify_count;
    out_snapshot->put_count = state->put_count;
    out_snapshot->dead = state->dead;
    out_snapshot->zoned_model = state->zoned_model;
    out_snapshot->readahead_update_count = state->readahead_update_count;
    out_snapshot->zone_revalidate_count = state->zone_revalidate_count;
    out_snapshot->read_count = state->read_count;
    out_snapshot->write_count = state->write_count;
    out_snapshot->bytes_read = state->bytes_read;
    out_snapshot->bytes_written = state->bytes_written;
    return 0;
}

void *kb_block_subsystem_first_registered_disk(void)
{
    for (kb_block_disk_state_t *state = disks; state != NULL; state = state->next) {
        if (state->registered && !state->dead) {
            return state->disk;
        }
    }
    return NULL;
}

void kb_block_subsystem_disk_set_io(
    void *disk,
    void *ctx,
    kb_block_disk_read_fn read_fn,
    kb_block_disk_write_fn write_fn)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL) {
        return;
    }
    state->io_ctx = ctx;
    state->read_fn = read_fn;
    state->write_fn = write_fn;
}

void kb_block_subsystem_disk_set_read_batch(
    void *disk,
    kb_block_disk_read_batch_fn read_batch_fn)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->read_batch_fn = read_batch_fn;
    }
}

void kb_block_subsystem_disk_set_write_batch(
    void *disk,
    kb_block_disk_write_batch_fn write_batch_fn)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->write_batch_fn = write_batch_fn;
    }
}

void kb_block_subsystem_disk_set_write_flags(
    void *disk,
    kb_block_disk_write_flags_fn write_flags_fn)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->write_flags_fn = write_flags_fn;
    }
}

void kb_block_subsystem_disk_set_flush(
    void *disk,
    kb_block_disk_flush_fn flush_fn)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state != NULL) {
        state->flush_fn = flush_fn;
    }
}

int kb_block_subsystem_disk_read(void *disk, uint64_t sector, void *buffer, size_t byte_count)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL || buffer == NULL) {
        return -22;
    }
    if (state->dead || !state->registered) {
        return -19;
    }
    int valid = disk_io_range_valid(state, sector, byte_count);
    if (valid != 0) {
        return valid;
    }
    if (state->read_fn == NULL) {
        return -95;
    }

    int result = state->read_fn(state->io_ctx, sector, buffer, byte_count);
    if (result == 0) {
        state->read_count++;
        state->bytes_read += byte_count;
    }
    return result;
}

int kb_block_subsystem_disk_read_batch(
    void *disk,
    const kb_block_disk_read_request_t *requests,
    size_t request_count)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL || requests == NULL || request_count == 0) {
        return -22;
    }
    if (state->dead || !state->registered) {
        return -19;
    }
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL ||
            disk_io_range_valid(state, requests[i].sector, requests[i].byte_count) != 0)
        {
            return -22;
        }
    }
    int result = 0;
    if (state->read_batch_fn != NULL) {
        result = state->read_batch_fn(state->io_ctx, requests, request_count);
    } else {
        for (size_t i = 0; i < request_count; ++i) {
            result = state->read_fn == NULL ? -95 : state->read_fn(
                state->io_ctx,
                requests[i].sector,
                requests[i].buffer,
                requests[i].byte_count);
            if (result != 0) {
                break;
            }
        }
    }
    if (result == 0) {
        state->read_count += request_count;
        for (size_t i = 0; i < request_count; ++i) {
            state->bytes_read += requests[i].byte_count;
        }
    }
    return result;
}

int kb_block_subsystem_disk_write_flags(
    void *disk,
    uint64_t sector,
    const void *buffer,
    size_t byte_count,
    uint32_t flags)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL || buffer == NULL) {
        return -22;
    }
    if (state->dead || !state->registered) {
        return -19;
    }
    if (state->read_only) {
        return -30;
    }
    int valid = disk_io_range_valid(state, sector, byte_count);
    if (valid != 0) {
        return valid;
    }
    if ((flags & ~KB_BLOCK_DISK_WRITE_FUA) != 0 ||
        (state->write_fn == NULL && state->write_flags_fn == NULL))
    {
        return -95;
    }

    int result = state->write_flags_fn != NULL ?
        state->write_flags_fn(
            state->io_ctx, sector, buffer, byte_count, flags) :
        state->write_fn(state->io_ctx, sector, buffer, byte_count);
    if (result == 0 &&
        state->write_flags_fn == NULL &&
        (flags & KB_BLOCK_DISK_WRITE_FUA) != 0)
    {
        result = state->flush_fn == NULL ? -95 : state->flush_fn(state->io_ctx);
    }
    if (result == 0) {
        state->write_count++;
        state->bytes_written += byte_count;
    }
    return result;
}

int kb_block_subsystem_disk_write(
    void *disk,
    uint64_t sector,
    const void *buffer,
    size_t byte_count)
{
    return kb_block_subsystem_disk_write_flags(
        disk, sector, buffer, byte_count, 0);
}

int kb_block_subsystem_disk_write_batch(
    void *disk,
    const kb_block_disk_write_request_t *requests,
    size_t request_count)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL || requests == NULL || request_count == 0) {
        return -22;
    }
    if (state->dead || !state->registered) {
        return -19;
    }
    if (state->read_only) {
        return -30;
    }
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL ||
            disk_io_range_valid(state, requests[i].sector, requests[i].byte_count) != 0)
        {
            return -22;
        }
    }
    int result = 0;
    if (state->write_batch_fn != NULL) {
        result = state->write_batch_fn(state->io_ctx, requests, request_count);
    } else {
        for (size_t i = 0; i < request_count; ++i) {
            result = state->write_fn == NULL ? -95 : state->write_fn(
                state->io_ctx,
                requests[i].sector,
                requests[i].buffer,
                requests[i].byte_count);
            if (result != 0) {
                break;
            }
        }
    }
    if (result == 0) {
        state->write_count += request_count;
        for (size_t i = 0; i < request_count; ++i) {
            state->bytes_written += requests[i].byte_count;
        }
    }
    return result;
}

int kb_block_subsystem_disk_flush(void *disk)
{
    kb_block_disk_state_t *state = disk_state_for(disk);
    if (state == NULL) {
        return -22;
    }
    if (state->dead || !state->registered) {
        return -19;
    }
    return state->flush_fn == NULL ? 0 : state->flush_fn(state->io_ctx);
}

void *kb_block_subsystem_block_device_alloc(void)
{
    return calloc(1, KB_BLOCK_SUBSYSTEM_OBJECT_SIZE);
}

void kb_block_subsystem_object_free(void *object)
{
    disk_state_remove(object);
    queue_state_remove(object);
    free(object);
}

int kb_block_subsystem_tagset_prepare(void *tag_set)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL) {
        return -12;
    }
    state->prepared_count++;
    return 0;
}

void kb_block_subsystem_tagset_free(void *tag_set)
{
    tagset_state_remove(tag_set);
}

void kb_block_subsystem_tagset_set_hw_queues(void *tag_set, uint32_t nr_hw_queues)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL) {
        return;
    }
    if (nr_hw_queues == 0) {
        nr_hw_queues = 1;
    }
    if (nr_hw_queues > KB_BLOCK_SUBSYSTEM_QUEUE_MAX) {
        nr_hw_queues = KB_BLOCK_SUBSYSTEM_QUEUE_MAX;
    }
    state->nr_hw_queues = nr_hw_queues;
    for (size_t i = 0; i < KB_BLOCK_SUBSYSTEM_QUEUE_MAX; i++) {
        state->tags[i].active_queues = i < nr_hw_queues ? 1u : 0u;
    }
}

void kb_block_subsystem_tagset_note_map_queues(void *tag_set, int pci)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL) {
        return;
    }
    if (pci) {
        state->pci_map_queues_count++;
    } else {
        state->map_queues_count++;
    }
}

void kb_block_subsystem_tagset_note_busy_iter(void *tag_set)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state != NULL) {
        state->busy_iter_count++;
    }
}

void kb_block_subsystem_tagset_note_wait_completed(void *tag_set)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state != NULL) {
        state->wait_completed_count++;
    }
}

int kb_block_subsystem_tagset_snapshot(void *tag_set, kb_block_tagset_snapshot_t *out_snapshot)
{
    kb_block_tagset_state_t *state = tagset_state_find(tag_set);
    if (state == NULL || out_snapshot == NULL) {
        return -22;
    }
    out_snapshot->tag_set = state->tag_set;
    out_snapshot->tag_array = state->tag_array;
    out_snapshot->prepared_count = state->prepared_count;
    out_snapshot->nr_hw_queues = state->nr_hw_queues;
    out_snapshot->map_queues_count = state->map_queues_count;
    out_snapshot->pci_map_queues_count = state->pci_map_queues_count;
    out_snapshot->busy_iter_count = state->busy_iter_count;
    out_snapshot->wait_completed_count = state->wait_completed_count;
    return 0;
}

void *kb_block_subsystem_tagset_array(void *tag_set)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    return state == NULL ? NULL : state->tag_array;
}

uint32_t kb_block_subsystem_tagset_alloc_tag(void *tag_set, size_t queue_index)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL || queue_index >= KB_BLOCK_SUBSYSTEM_QUEUE_MAX) {
        return 1;
    }

    uint32_t limit = state->tags[queue_index].nr_tags;
    if (limit <= 1) {
        return 0;
    }

    uint32_t tag = state->next_tag[queue_index];
    for (uint32_t scanned = 0; scanned < limit - 1u; ++scanned) {
        if (tag == 0 || tag >= limit) {
            tag = 1;
        }
        if (state->rqs[queue_index][tag] == NULL) {
            state->next_tag[queue_index] = tag + 1u;
            if (state->next_tag[queue_index] >= limit) {
                state->next_tag[queue_index] = 1;
            }
            return tag;
        }
        tag++;
    }
    return UINT32_MAX;
}

int kb_block_subsystem_tagset_bind_request(void *tag_set, size_t queue_index, uint32_t tag, void *request)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL ||
        queue_index >= KB_BLOCK_SUBSYSTEM_QUEUE_MAX ||
        tag >= state->tags[queue_index].nr_tags)
    {
        return -22;
    }
    state->rqs[queue_index][tag] = request;
    return 0;
}

void kb_block_subsystem_tagset_unbind_request(void *tag_set, size_t queue_index, uint32_t tag, void *request)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL ||
        queue_index >= KB_BLOCK_SUBSYSTEM_QUEUE_MAX ||
        tag >= state->tags[queue_index].nr_tags)
    {
        return;
    }
    if (state->rqs[queue_index][tag] == request) {
        state->rqs[queue_index][tag] = NULL;
    }
}

void *kb_block_subsystem_tagset_request(void *tag_set, size_t queue_index, uint32_t tag)
{
    kb_block_tagset_state_t *state = tagset_state_for(tag_set);
    if (state == NULL ||
        queue_index >= KB_BLOCK_SUBSYSTEM_QUEUE_MAX ||
        tag >= state->tags[queue_index].nr_tags)
    {
        return NULL;
    }
    return state->rqs[queue_index][tag];
}
