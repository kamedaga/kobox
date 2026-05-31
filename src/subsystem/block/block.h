#pragma once

#include <stddef.h>
#include <stdint.h>

void *kb_block_subsystem_queue_alloc(void *tag_set);
void *kb_block_subsystem_queue_tag_set(const void *queue);
void *kb_block_subsystem_disk_alloc(void);
void *kb_block_subsystem_block_device_alloc(void);
void kb_block_subsystem_object_free(void *object);

void *kb_block_subsystem_tagset_array(void *tag_set);
uint32_t kb_block_subsystem_tagset_alloc_tag(void *tag_set, size_t queue_index);
int kb_block_subsystem_tagset_bind_request(void *tag_set, size_t queue_index, uint32_t tag, void *request);
void kb_block_subsystem_tagset_unbind_request(void *tag_set, size_t queue_index, uint32_t tag, void *request);
void *kb_block_subsystem_tagset_request(void *tag_set, size_t queue_index, uint32_t tag);
