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

void *kb_block_subsystem_queue_alloc(void *tag_set)
{
    kb_block_queue_t *queue = calloc(1, KB_BLOCK_SUBSYSTEM_OBJECT_SIZE);
    if (queue == NULL) {
        return NULL;
    }
    queue->tag_set = tag_set;
    return queue;
}

void *kb_block_subsystem_queue_tag_set(const void *queue)
{
    const kb_block_queue_t *block_queue = queue;
    return block_queue == NULL ? NULL : block_queue->tag_set;
}

void *kb_block_subsystem_disk_alloc(void)
{
    return calloc(1, KB_BLOCK_SUBSYSTEM_OBJECT_SIZE);
}

void *kb_block_subsystem_block_device_alloc(void)
{
    return calloc(1, KB_BLOCK_SUBSYSTEM_OBJECT_SIZE);
}

void kb_block_subsystem_object_free(void *object)
{
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
