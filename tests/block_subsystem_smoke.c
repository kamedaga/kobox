#include "subsystem/block/block.h"

#include <stdint.h>

int main(void)
{
    void *queue = kb_block_subsystem_queue_alloc((void *)(uintptr_t)0x1234);
    if (queue == 0 || kb_block_subsystem_queue_tag_set(queue) != (void *)(uintptr_t)0x1234) {
        return 1;
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
        return 2;
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
        return 3;
    }

    void *disk = kb_block_subsystem_disk_alloc();
    void *part0 = kb_block_subsystem_block_device_alloc();
    if (disk == 0 || part0 == 0) {
        return 4;
    }
    if (kb_block_subsystem_disk_attach(disk, queue, part0) != 0) {
        return 5;
    }
    kb_block_subsystem_disk_set_capacity(disk, 65536);
    kb_block_subsystem_disk_set_read_only(disk, 1);
    if (kb_block_subsystem_disk_register((void *)(uintptr_t)0x55, disk, (void *)(uintptr_t)0x66) != 0) {
        return 6;
    }
    if (kb_block_subsystem_disk_set_capacity_and_notify(disk, 131072) != 1) {
        return 7;
    }

    kb_block_disk_snapshot_t snapshot;
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0) {
        return 8;
    }
    if (snapshot.queue != queue ||
        snapshot.part0 != part0 ||
        snapshot.parent != (void *)(uintptr_t)0x55 ||
        snapshot.groups != (void *)(uintptr_t)0x66 ||
        snapshot.capacity_sectors != 131072 ||
        snapshot.registered != 1 ||
        snapshot.read_only != 1 ||
        snapshot.notify_count != 1)
    {
        return 9;
    }

    kb_block_subsystem_disk_unregister(disk);
    kb_block_subsystem_disk_put(disk);
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0 ||
        snapshot.registered != 0 ||
        snapshot.put_count != 1)
    {
        return 10;
    }

    kb_block_subsystem_object_free(part0);
    kb_block_subsystem_object_free(disk);
    kb_block_subsystem_object_free(queue);
    return 0;
}
