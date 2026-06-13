#include "linux_subsystem/block/block.h"
#include "linux_subsystem/fs/fs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    SMOKE_SECTOR_SIZE = 512,
    SMOKE_SECTOR_COUNT = 16,
    SMOKE_BUFFER_HEAD_DATA_OFFSET = 0x28,
};

static unsigned int bio_completion_count;

static void smoke_bio_complete(void *bio)
{
    if (bio != NULL && kb_fs_subsystem_bio_result(bio) == 0) {
        bio_completion_count++;
    }
}

static int smoke_disk_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    unsigned char *backing = (unsigned char *)ctx;
    memcpy(buffer, backing + sector * SMOKE_SECTOR_SIZE, byte_count);
    return 0;
}

static int smoke_disk_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    unsigned char *backing = (unsigned char *)ctx;
    memcpy(backing + sector * SMOKE_SECTOR_SIZE, buffer, byte_count);
    return 0;
}

static void free_bh(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    void *data = NULL;
    memcpy(&data, (const unsigned char *)buffer_head + SMOKE_BUFFER_HEAD_DATA_OFFSET, sizeof(data));
    free(data);
    free(buffer_head);
}

int main(void)
{
    unsigned char backing[SMOKE_SECTOR_SIZE * SMOKE_SECTOR_COUNT];
    unsigned char write_buffer[SMOKE_SECTOR_SIZE];
    unsigned char read_buffer[SMOKE_SECTOR_SIZE];
    memset(backing, 0, sizeof(backing));
    memset(read_buffer, 0, sizeof(read_buffer));
    for (size_t i = 0; i < sizeof(write_buffer); i++) {
        write_buffer[i] = (unsigned char)(0x5au ^ i);
    }

    void *queue = kb_block_subsystem_queue_alloc(NULL);
    void *disk = kb_block_subsystem_disk_alloc();
    void *part0 = kb_block_subsystem_block_device_alloc();
    if (queue == NULL || disk == NULL || part0 == NULL) {
        return 1;
    }
    if (kb_block_subsystem_disk_attach(disk, queue, part0) != 0) {
        return 2;
    }
    kb_block_subsystem_disk_set_capacity(disk, SMOKE_SECTOR_COUNT);
    kb_block_subsystem_disk_set_io(disk, backing, smoke_disk_read, smoke_disk_write);
    if (kb_block_subsystem_disk_register(NULL, disk, NULL) != 0) {
        return 3;
    }

    kb_fs_block_device_t *fs_device = NULL;
    if (kb_fs_block_device_create_from_disk("pachaos-virtio-blk-smoke", disk, &fs_device) != 0 ||
        fs_device == NULL)
    {
        return 4;
    }

    if (kb_fs_block_device_write(fs_device, 2u * SMOKE_SECTOR_SIZE, write_buffer, sizeof(write_buffer)) != 0 ||
        kb_fs_block_device_read(fs_device, 2u * SMOKE_SECTOR_SIZE, read_buffer, sizeof(read_buffer)) != 0 ||
        memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0)
    {
        kb_fs_block_device_destroy(fs_device);
        return 5;
    }

    unsigned char partial[16];
    memset(partial, 0, sizeof(partial));
    if (kb_fs_block_device_read(fs_device, 2u * SMOKE_SECTOR_SIZE + 3u, partial, sizeof(partial)) != 0 ||
        memcmp(partial, write_buffer + 3, sizeof(partial)) != 0 ||
        kb_fs_block_device_write(fs_device, sizeof(backing), write_buffer, sizeof(write_buffer)) != -34)
    {
        kb_fs_block_device_destroy(fs_device);
        return 6;
    }

    if (kb_fs_subsystem_set_mount_probe_block_device(fs_device) != 0) {
        kb_fs_block_device_destroy(fs_device);
        return 7;
    }
    void *buffer_head = kb_fs_subsystem_bdev_getblk(NULL, 2, SMOKE_SECTOR_SIZE, 0);
    if (buffer_head == NULL) {
        kb_fs_block_device_destroy(fs_device);
        return 8;
    }
    void *b_data = NULL;
    memcpy(&b_data, (const unsigned char *)buffer_head + SMOKE_BUFFER_HEAD_DATA_OFFSET, sizeof(b_data));
    if (b_data == NULL || memcmp(write_buffer, b_data, sizeof(write_buffer)) != 0) {
        free_bh(buffer_head);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    free_bh(buffer_head);

    unsigned char bio_read_buffer[SMOKE_SECTOR_SIZE];
    memset(bio_read_buffer, 0, sizeof(bio_read_buffer));
    void *read_bio = kb_fs_subsystem_bio_alloc_bioset(NULL, 1, KB_FS_BIO_OP_READ, 0, NULL);
    if (read_bio == NULL ||
        kb_fs_subsystem_bio_add_page(read_bio, bio_read_buffer, sizeof(bio_read_buffer), 0) != (int)sizeof(bio_read_buffer))
    {
        kb_fs_subsystem_bio_put(read_bio);
        kb_fs_block_device_destroy(fs_device);
        return 10;
    }
    kb_fs_subsystem_bio_set_sector(read_bio, 2);
    kb_fs_subsystem_bio_set_end_io(read_bio, smoke_bio_complete);
    kb_fs_subsystem_submit_bio(read_bio);
    kb_fs_bio_snapshot_t read_bio_snapshot;
    if (kb_fs_subsystem_bio_snapshot(read_bio, &read_bio_snapshot) != 0 ||
        read_bio_snapshot.submitted != 1 ||
        read_bio_snapshot.queued != 0 ||
        read_bio_snapshot.completed != 1 ||
        read_bio_snapshot.result != 0 ||
        memcmp(write_buffer, bio_read_buffer, sizeof(bio_read_buffer)) != 0)
    {
        kb_fs_subsystem_bio_put(read_bio);
        kb_fs_block_device_destroy(fs_device);
        return 11;
    }
    kb_fs_subsystem_bio_put(read_bio);

    unsigned char bio_write_buffer[SMOKE_SECTOR_SIZE];
    for (size_t i = 0; i < sizeof(bio_write_buffer); i++) {
        bio_write_buffer[i] = (unsigned char)(0xa5u ^ i);
    }
    void *write_bio = kb_fs_subsystem_bio_alloc_bioset(NULL, 1, KB_FS_BIO_OP_WRITE, 0, NULL);
    if (write_bio == NULL ||
        kb_fs_subsystem_bio_add_page(write_bio, bio_write_buffer, sizeof(bio_write_buffer), 0) != (int)sizeof(bio_write_buffer))
    {
        kb_fs_subsystem_bio_put(write_bio);
        kb_fs_block_device_destroy(fs_device);
        return 12;
    }
    kb_fs_subsystem_bio_set_sector(write_bio, 4);
    kb_fs_subsystem_bio_set_end_io(write_bio, smoke_bio_complete);
    kb_fs_subsystem_submit_bio_noacct(write_bio);
    kb_fs_bio_snapshot_t write_bio_snapshot;
    if (kb_fs_subsystem_bio_snapshot(write_bio, &write_bio_snapshot) != 0 ||
        write_bio_snapshot.submitted != 1 ||
        write_bio_snapshot.queued != 0 ||
        write_bio_snapshot.completed != 1 ||
        write_bio_snapshot.result != 0 ||
        memcmp(backing + 4u * SMOKE_SECTOR_SIZE, bio_write_buffer, sizeof(bio_write_buffer)) != 0 ||
        bio_completion_count != 2)
    {
        kb_fs_subsystem_bio_put(write_bio);
        kb_fs_block_device_destroy(fs_device);
        return 13;
    }
    kb_fs_subsystem_bio_put(write_bio);

    unsigned char async_write_buffer[SMOKE_SECTOR_SIZE];
    for (size_t i = 0; i < sizeof(async_write_buffer); i++) {
        async_write_buffer[i] = (unsigned char)(0xc3u ^ i);
    }
    kb_fs_subsystem_bio_set_auto_drain(0);
    void *async_write_bio = kb_fs_subsystem_bio_alloc_bioset(NULL, 1, KB_FS_BIO_OP_WRITE, 0, NULL);
    void *flush_bio = kb_fs_subsystem_bio_alloc_bioset(NULL, 0, KB_FS_BIO_OP_FLUSH, 0, NULL);
    if (async_write_bio == NULL ||
        flush_bio == NULL ||
        kb_fs_subsystem_bio_add_page(async_write_bio, async_write_buffer, sizeof(async_write_buffer), 0) != (int)sizeof(async_write_buffer))
    {
        kb_fs_subsystem_bio_put(async_write_bio);
        kb_fs_subsystem_bio_put(flush_bio);
        kb_fs_block_device_destroy(fs_device);
        return 14;
    }
    kb_fs_subsystem_bio_set_sector(async_write_bio, 5);
    kb_fs_subsystem_bio_set_end_io(async_write_bio, smoke_bio_complete);
    kb_fs_subsystem_bio_set_end_io(flush_bio, smoke_bio_complete);
    kb_fs_subsystem_submit_bio(async_write_bio);
    kb_fs_subsystem_submit_bio(flush_bio);
    kb_fs_bio_snapshot_t async_write_snapshot;
    kb_fs_bio_snapshot_t flush_snapshot;
    if (kb_fs_subsystem_bio_queue_depth() != 2 ||
        kb_fs_subsystem_bio_snapshot(async_write_bio, &async_write_snapshot) != 0 ||
        kb_fs_subsystem_bio_snapshot(flush_bio, &flush_snapshot) != 0 ||
        async_write_snapshot.queued != 1 ||
        async_write_snapshot.completed != 0 ||
        flush_snapshot.queued != 1 ||
        flush_snapshot.completed != 0 ||
        bio_completion_count != 2)
    {
        kb_fs_subsystem_bio_put(async_write_bio);
        kb_fs_subsystem_bio_put(flush_bio);
        kb_fs_block_device_destroy(fs_device);
        return 15;
    }
    if (kb_fs_subsystem_bio_drain() != 2 ||
        kb_fs_subsystem_bio_queue_depth() != 0 ||
        kb_fs_subsystem_bio_snapshot(async_write_bio, &async_write_snapshot) != 0 ||
        kb_fs_subsystem_bio_snapshot(flush_bio, &flush_snapshot) != 0 ||
        async_write_snapshot.queued != 0 ||
        async_write_snapshot.completed != 1 ||
        async_write_snapshot.result != 0 ||
        flush_snapshot.queued != 0 ||
        flush_snapshot.completed != 1 ||
        flush_snapshot.result != 0 ||
        memcmp(backing + 5u * SMOKE_SECTOR_SIZE, async_write_buffer, sizeof(async_write_buffer)) != 0 ||
        bio_completion_count != 4)
    {
        kb_fs_subsystem_bio_put(async_write_bio);
        kb_fs_subsystem_bio_put(flush_bio);
        kb_fs_block_device_destroy(fs_device);
        return 16;
    }
    kb_fs_subsystem_bio_put(async_write_bio);
    kb_fs_subsystem_bio_put(flush_bio);
    kb_fs_subsystem_bio_set_auto_drain(1);

    kb_block_disk_snapshot_t snapshot;
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0 ||
        snapshot.read_count != 4 ||
        snapshot.write_count != 3 ||
        snapshot.bytes_read != sizeof(read_buffer) * 4u ||
        snapshot.bytes_written != sizeof(write_buffer) * 3u)
    {
        kb_fs_block_device_destroy(fs_device);
        return 17;
    }

    kb_fs_subsystem_set_mount_probe_block_device(NULL);
    kb_fs_block_device_destroy(fs_device);
    kb_block_subsystem_object_free(part0);
    kb_block_subsystem_object_free(disk);
    kb_block_subsystem_object_free(queue);
    return 0;
}
