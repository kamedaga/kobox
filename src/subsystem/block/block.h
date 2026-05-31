#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct kb_block_queue_limits {
    uint32_t logical_block_size;
    uint32_t physical_block_size;
    uint32_t io_min;
    uint32_t io_opt;
    uint32_t max_hw_sectors;
    uint32_t max_segments;
    uint32_t max_discard_sectors;
    uint32_t max_discard_segments;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_zone_append_sectors;
    uint32_t chunk_sectors;
    uint32_t dma_alignment;
    uint64_t virt_boundary_mask;
    uint32_t flags;
    uint8_t write_cache;
    uint8_t fua;
} kb_block_queue_limits_t;

typedef struct kb_block_disk_snapshot {
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
} kb_block_disk_snapshot_t;

void *kb_block_subsystem_queue_alloc(void *tag_set);
void *kb_block_subsystem_queue_tag_set(const void *queue);
void kb_block_subsystem_queue_put(void *queue);
void kb_block_subsystem_queue_set_logical_block_size(void *queue, uint32_t size);
void kb_block_subsystem_queue_set_physical_block_size(void *queue, uint32_t size);
void kb_block_subsystem_queue_set_io_min(void *queue, uint32_t size);
void kb_block_subsystem_queue_set_io_opt(void *queue, uint32_t size);
void kb_block_subsystem_queue_set_max_hw_sectors(void *queue, uint32_t sectors);
void kb_block_subsystem_queue_set_max_segments(void *queue, uint32_t segments);
void kb_block_subsystem_queue_set_max_discard_sectors(void *queue, uint32_t sectors);
void kb_block_subsystem_queue_set_max_discard_segments(void *queue, uint32_t segments);
void kb_block_subsystem_queue_set_max_write_zeroes_sectors(void *queue, uint32_t sectors);
void kb_block_subsystem_queue_set_max_zone_append_sectors(void *queue, uint32_t sectors);
void kb_block_subsystem_queue_set_chunk_sectors(void *queue, uint32_t sectors);
void kb_block_subsystem_queue_set_dma_alignment(void *queue, uint32_t mask);
void kb_block_subsystem_queue_set_virt_boundary(void *queue, uint64_t mask);
void kb_block_subsystem_queue_set_write_cache(void *queue, int write_cache, int fua);
void kb_block_subsystem_queue_flag_set(void *queue, uint32_t flag);
int kb_block_subsystem_queue_limits(const void *queue, kb_block_queue_limits_t *out_limits);

void *kb_block_subsystem_disk_alloc(void);
int kb_block_subsystem_disk_attach(void *disk, void *queue, void *part0);
int kb_block_subsystem_disk_register(void *parent, void *disk, void *groups);
void kb_block_subsystem_disk_unregister(void *disk);
void kb_block_subsystem_disk_put(void *disk);
void kb_block_subsystem_disk_set_capacity(void *disk, uint64_t sectors);
int kb_block_subsystem_disk_set_capacity_and_notify(void *disk, uint64_t sectors);
void kb_block_subsystem_disk_set_read_only(void *disk, int read_only);
int kb_block_subsystem_disk_snapshot(const void *disk, kb_block_disk_snapshot_t *out_snapshot);

void *kb_block_subsystem_block_device_alloc(void);
void kb_block_subsystem_object_free(void *object);

void *kb_block_subsystem_tagset_array(void *tag_set);
uint32_t kb_block_subsystem_tagset_alloc_tag(void *tag_set, size_t queue_index);
int kb_block_subsystem_tagset_bind_request(void *tag_set, size_t queue_index, uint32_t tag, void *request);
void kb_block_subsystem_tagset_unbind_request(void *tag_set, size_t queue_index, uint32_t tag, void *request);
void *kb_block_subsystem_tagset_request(void *tag_set, size_t queue_index, uint32_t tag);
