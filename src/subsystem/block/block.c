#include "subsystem/block/block.h"

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
    struct kb_block_tagset_state *next;
} kb_block_tagset_state_t;

_Static_assert(offsetof(kb_block_mq_tags_t, rqs) == 0x90, "blk_mq_tags.rqs offset");

static kb_block_tagset_state_t *tagsets;
static kb_block_queue_state_t *queues;
static kb_block_disk_state_t *disks;
static uint32_t next_disk_number = 1;

static kb_block_tagset_state_t *tagset_state_for(void *tag_set)
{
    if (tag_set == NULL) {
        return NULL;
    }

    for (kb_block_tagset_state_t *state = tagsets; state != NULL; state = state->next) {
        if (state->tag_set == tag_set) {
            return state;
        }
    }

    kb_block_tagset_state_t *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return NULL;
    }
    state->tag_set = tag_set;
    for (size_t i = 0; i < KB_BLOCK_SUBSYSTEM_QUEUE_MAX; i++) {
        state->tag_array[i] = &state->tags[i];
        state->tags[i].nr_tags = KB_BLOCK_SUBSYSTEM_TAGS_PER_QUEUE;
        state->tags[i].rqs = state->rqs[i];
        state->next_tag[i] = 1;
    }
    state->next = tagsets;
    tagsets = state;
    return state;
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
    return 0;
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
    if (tag == 0 || tag >= limit) {
        tag = 1;
    }
    state->next_tag[queue_index] = tag + 1;
    if (state->next_tag[queue_index] >= limit) {
        state->next_tag[queue_index] = 1;
    }
    return tag;
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
