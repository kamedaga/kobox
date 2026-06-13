#include "linux_subsystem/block/block.h"

#include <stdint.h>
#include <string.h>

static int smoke_disk_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    unsigned char *backing = ctx;
    memcpy(buffer, backing + sector * 512u, byte_count);
    return 0;
}

static int smoke_disk_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    unsigned char *backing = ctx;
    memcpy(backing + sector * 512u, buffer, byte_count);
    return 0;
}

int main(void)
{
    int fake_tag_set_storage;
    void *tag_set = &fake_tag_set_storage;
    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    if (queue == 0 || kb_block_subsystem_queue_tag_set(queue) != tag_set) {
        return 1;
    }
    if (kb_block_subsystem_tagset_prepare(tag_set) != 0) {
        return 2;
    }
    kb_block_subsystem_tagset_set_hw_queues(tag_set, 2);
    kb_block_subsystem_tagset_note_map_queues(tag_set, 0);
    kb_block_subsystem_tagset_note_map_queues(tag_set, 1);
    kb_block_subsystem_tagset_note_busy_iter(tag_set);
    kb_block_subsystem_tagset_note_wait_completed(tag_set);

    kb_block_tagset_snapshot_t tagset_snapshot;
    if (kb_block_subsystem_tagset_snapshot(tag_set, &tagset_snapshot) != 0 ||
        tagset_snapshot.tag_set != tag_set ||
        tagset_snapshot.tag_array == 0 ||
        tagset_snapshot.prepared_count != 1 ||
        tagset_snapshot.nr_hw_queues != 2 ||
        tagset_snapshot.map_queues_count != 1 ||
        tagset_snapshot.pci_map_queues_count != 1 ||
        tagset_snapshot.busy_iter_count != 1 ||
        tagset_snapshot.wait_completed_count != 1)
    {
        return 3;
    }

    kb_block_subsystem_queue_set_logical_block_size(queue, 4096);
    kb_block_subsystem_queue_set_physical_block_size(queue, 4096);
    kb_block_subsystem_queue_set_io_min(queue, 4096);
    kb_block_subsystem_queue_set_io_opt(queue, 8192);
    kb_block_subsystem_queue_set_max_hw_sectors(queue, 1024);
    kb_block_subsystem_queue_set_max_segments(queue, 128);
    kb_block_subsystem_queue_set_write_cache(queue, 1, 1);
    kb_block_subsystem_queue_flag_set(queue, 3);

    kb_block_queue_limits_t limits;
    if (kb_block_subsystem_queue_limits(queue, &limits) != 0) {
        return 4;
    }
    if (limits.logical_block_size != 4096 ||
        limits.physical_block_size != 4096 ||
        limits.io_min != 4096 ||
        limits.io_opt != 8192 ||
        limits.max_hw_sectors != 1024 ||
        limits.max_segments != 128 ||
        limits.write_cache != 1 ||
        limits.fua != 1 ||
        (limits.flags & (1u << 3)) == 0)
    {
        return 5;
    }

    void *disk = kb_block_subsystem_disk_alloc();
    void *part0 = kb_block_subsystem_block_device_alloc();
    if (disk == 0 || part0 == 0) {
        return 6;
    }
    if (kb_block_subsystem_disk_attach(disk, queue, part0) != 0) {
        return 7;
    }
    kb_block_subsystem_disk_set_capacity(disk, 65536);
    if (kb_block_subsystem_disk_register((void *)(uintptr_t)0x55, disk, (void *)(uintptr_t)0x66) != 0) {
        return 8;
    }
    unsigned char backing[4096 * 16];
    unsigned char write_buffer[4096];
    unsigned char read_buffer[sizeof(write_buffer)];
    memset(backing, 0, sizeof(backing));
    for (size_t i = 0; i < sizeof(write_buffer); i++) {
        write_buffer[i] = (unsigned char)(i ^ 0xa5u);
        read_buffer[i] = 0;
    }
    kb_block_subsystem_disk_set_io(disk, backing, smoke_disk_read, smoke_disk_write);
    if (kb_block_subsystem_disk_write(disk, 8, write_buffer, sizeof(write_buffer)) != 0 ||
        kb_block_subsystem_disk_read(disk, 8, read_buffer, sizeof(read_buffer)) != 0 ||
        memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0)
    {
        return 30;
    }
    kb_block_subsystem_disk_set_read_only(disk, 1);
    if (kb_block_subsystem_disk_write(disk, 8, write_buffer, sizeof(write_buffer)) != -30) {
        return 31;
    }
    if (kb_block_subsystem_disk_set_capacity_and_notify(disk, 131072) != 1) {
        return 9;
    }
    kb_block_subsystem_disk_set_zoned(disk, 2);
    kb_block_subsystem_disk_update_readahead(disk);
    if (kb_block_subsystem_disk_revalidate_zones(disk) != 0) {
        return 10;
    }

    kb_block_disk_snapshot_t snapshot;
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0) {
        return 11;
    }
    if (snapshot.queue != queue ||
        snapshot.part0 != part0 ||
        snapshot.parent != (void *)(uintptr_t)0x55 ||
        snapshot.groups != (void *)(uintptr_t)0x66 ||
        snapshot.capacity_sectors != 131072 ||
        snapshot.registered != 1 ||
        snapshot.read_only != 1 ||
        snapshot.notify_count != 1 ||
        snapshot.zoned_model != 2 ||
        snapshot.readahead_update_count != 1 ||
        snapshot.zone_revalidate_count != 1 ||
        snapshot.read_count != 1 ||
        snapshot.write_count != 1 ||
        snapshot.bytes_read != sizeof(read_buffer) ||
        snapshot.bytes_written != sizeof(write_buffer))
    {
        return 12;
    }

    kb_block_subsystem_disk_mark_dead(disk);
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0 ||
        snapshot.dead != 1 ||
        snapshot.registered != 0)
    {
        return 13;
    }
    kb_block_subsystem_disk_unregister(disk);
    kb_block_subsystem_disk_put(disk);
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0 ||
        snapshot.registered != 0 ||
        snapshot.put_count != 1)
    {
        return 14;
    }

    kb_block_subsystem_tagset_free(tag_set);
    if (kb_block_subsystem_tagset_snapshot(tag_set, &tagset_snapshot) == 0) {
        return 15;
    }
    kb_block_subsystem_object_free(part0);
    kb_block_subsystem_object_free(disk);
    kb_block_subsystem_object_free(queue);
    return 0;
}
