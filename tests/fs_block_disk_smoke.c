#include "linux_subsystem/block/block.h"
#include "linux_subsystem/fs/fs.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SMOKE_SECTOR_SIZE = 512,
    SMOKE_SECTOR_COUNT = 16,
    SMOKE_BUFFER_HEAD_BYTES = 128,
    SMOKE_BUFFER_HEAD_BLOCKNR_OFFSET = 0x18,
    SMOKE_BUFFER_HEAD_SIZE_OFFSET = 0x20,
    SMOKE_BUFFER_HEAD_DATA_OFFSET = 0x28,
    SMOKE_BUFFER_HEAD_END_IO_OFFSET = 0x38,
    SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET = 0x60,
    SMOKE_BUFFER_HEAD_FOLIO_OFFSET = 0x10,
    SMOKE_BH_UPTODATE = 1u << 0,
    SMOKE_BH_DIRTY = 1u << 1,
    SMOKE_BH_LOCK = 1u << 2,
    SMOKE_BH_REQ = 1u << 3,
    SMOKE_BH_MAPPED = 1u << 4,
};

static unsigned int bio_completion_count;
static unsigned int disk_flush_count;
static unsigned int disk_read_batch_count;
static unsigned int disk_write_batch_count;
static unsigned int io_event_count;
static unsigned char io_events[256];

enum {
    SMOKE_IO_EVENT_WRITE = 1,
    SMOKE_IO_EVENT_FLUSH = 2,
};

static void smoke_record_io_event(unsigned char event)
{
    if (io_event_count < sizeof(io_events)) {
        io_events[io_event_count++] = event;
    }
}

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
    smoke_record_io_event(SMOKE_IO_EVENT_WRITE);
    memcpy(backing + sector * SMOKE_SECTOR_SIZE, buffer, byte_count);
    return 0;
}

static int smoke_disk_read_batch(
    void *ctx,
    const kb_block_disk_read_request_t *requests,
    size_t request_count)
{
    if (requests == NULL || request_count < 2) {
        return -22;
    }
    disk_read_batch_count++;
    for (size_t i = 0; i < request_count; ++i) {
        const int status = smoke_disk_read(
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

static int smoke_disk_write_batch(
    void *ctx,
    const kb_block_disk_write_request_t *requests,
    size_t request_count)
{
    if (requests == NULL || request_count < 2) {
        return -22;
    }
    disk_write_batch_count++;
    for (size_t i = 0; i < request_count; ++i) {
        const int status = smoke_disk_write(
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

static int smoke_disk_flush(void *ctx)
{
    if (ctx == NULL) {
        return -22;
    }
    smoke_record_io_event(SMOKE_IO_EVENT_FLUSH);
    disk_flush_count++;
    return 0;
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
    kb_block_subsystem_disk_set_read_batch(disk, smoke_disk_read_batch);
    kb_block_subsystem_disk_set_write_batch(disk, smoke_disk_write_batch);
    kb_block_subsystem_disk_set_flush(disk, smoke_disk_flush);
    if (kb_block_subsystem_disk_register(NULL, disk, NULL) != 0) {
        return 3;
    }

    kb_fs_block_device_t *fs_device = NULL;
    if (kb_fs_block_device_create_from_disk("pachaos-block-smoke", disk, &fs_device) != 0 ||
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
    uint64_t buffer_state = 0;
    memcpy(&buffer_state, buffer_head, sizeof(buffer_state));
    if (b_data == NULL ||
        memcmp(write_buffer, b_data, sizeof(write_buffer)) == 0 ||
        (buffer_state & SMOKE_BH_MAPPED) == 0 ||
        (buffer_state & SMOKE_BH_UPTODATE) != 0)
    {
        kb_fs_subsystem_buffer_head_put(buffer_head);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    void *bread_head = kb_fs_subsystem_bread_gfp(
        NULL, 2, SMOKE_SECTOR_SIZE, 0);
    memcpy(&buffer_state, buffer_head, sizeof(buffer_state));
    if (bread_head != buffer_head ||
        memcmp(write_buffer, b_data, sizeof(write_buffer)) != 0 ||
        (buffer_state & SMOKE_BH_UPTODATE) == 0)
    {
        kb_fs_subsystem_buffer_head_put(bread_head);
        kb_fs_subsystem_buffer_head_put(buffer_head);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_buffer_head_put(bread_head);
    if (kb_fs_subsystem_find_get_block(NULL, 2, SMOKE_SECTOR_SIZE) != NULL ||
        kb_fs_subsystem_find_get_block(NULL, 3, SMOKE_SECTOR_SIZE) != NULL)
    {
        kb_fs_subsystem_buffer_head_put(buffer_head);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_buffer_head_put(buffer_head);

    unsigned char plug_pattern[2][SMOKE_SECTOR_SIZE];
    for (size_t block = 0; block < 2; ++block) {
        for (size_t i = 0; i < SMOKE_SECTOR_SIZE; ++i) {
            plug_pattern[block][i] = (unsigned char)(0x31u + block + i);
        }
        memcpy(
            backing + (4u + block) * SMOKE_SECTOR_SIZE,
            plug_pattern[block],
            SMOKE_SECTOR_SIZE);
    }
    void *plug_heads[2] = {
        kb_fs_subsystem_bdev_getblk(NULL, 4, SMOKE_SECTOR_SIZE, 0),
        kb_fs_subsystem_bdev_getblk(NULL, 5, SMOKE_SECTOR_SIZE, 0),
    };
    unsigned char plug_storage[64] = {0};
    if (plug_heads[0] == NULL || plug_heads[1] == NULL) {
        kb_fs_subsystem_buffer_head_put(plug_heads[0]);
        kb_fs_subsystem_buffer_head_put(plug_heads[1]);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_blk_start_plug(plug_storage);
    for (size_t i = 0; i < 2; ++i) {
        kb_fs_subsystem_lock_buffer(plug_heads[i]);
        if (kb_fs_subsystem_bh_read(plug_heads[i], 0, 0) != 0) {
            kb_fs_subsystem_blk_finish_plug(plug_storage);
            kb_fs_subsystem_buffer_head_put(plug_heads[0]);
            kb_fs_subsystem_buffer_head_put(plug_heads[1]);
            kb_fs_block_device_destroy(fs_device);
            return 9;
        }
    }
    if (disk_read_batch_count != 0) {
        kb_fs_subsystem_blk_finish_plug(plug_storage);
        kb_fs_subsystem_buffer_head_put(plug_heads[0]);
        kb_fs_subsystem_buffer_head_put(plug_heads[1]);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_blk_finish_plug(plug_storage);
    for (size_t i = 0; i < 2; ++i) {
        void *plug_data = NULL;
        memcpy(&plug_data,
            (const unsigned char *)plug_heads[i] + SMOKE_BUFFER_HEAD_DATA_OFFSET,
            sizeof(plug_data));
        memcpy(&buffer_state, plug_heads[i], sizeof(buffer_state));
        if (plug_data == NULL ||
            memcmp(plug_data, plug_pattern[i], SMOKE_SECTOR_SIZE) != 0 ||
            (buffer_state & SMOKE_BH_UPTODATE) == 0)
        {
            kb_fs_subsystem_buffer_head_put(plug_heads[0]);
            kb_fs_subsystem_buffer_head_put(plug_heads[1]);
            kb_fs_block_device_destroy(fs_device);
            return 9;
        }
    }
    if (disk_read_batch_count != 1) {
        kb_fs_subsystem_buffer_head_put(plug_heads[0]);
        kb_fs_subsystem_buffer_head_put(plug_heads[1]);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_buffer_head_put(plug_heads[0]);
    kb_fs_subsystem_buffer_head_put(plug_heads[1]);
    disk_read_batch_count = 0;

    unsigned char *temporary_head = calloc(1, SMOKE_BUFFER_HEAD_BYTES);
    uint32_t temporary_refcount = 1;
    if (temporary_head == NULL) {
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    memcpy(
        temporary_head + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET,
        &temporary_refcount,
        sizeof(temporary_refcount));
    kb_fs_subsystem_buffer_head_put(temporary_head);
    memcpy(
        &temporary_refcount,
        temporary_head + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET,
        sizeof(temporary_refcount));
    if (temporary_refcount != 0) {
        kb_fs_subsystem_free_buffer_head(temporary_head);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_free_buffer_head(temporary_head);

    temporary_head = calloc(1, SMOKE_BUFFER_HEAD_BYTES);
    void *temporary_folio = kb_kvm_alloc_pages_stub(0, 0);
    if (temporary_head == NULL || temporary_folio == NULL) {
        free(temporary_head);
        kb_kvm_free_pages_stub(temporary_folio, 0);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_folio_set_bh(temporary_head, temporary_folio, 123);
    void *temporary_b_folio = NULL;
    void *temporary_b_data = NULL;
    memcpy(&temporary_b_folio, temporary_head + 0x10, sizeof(temporary_b_folio));
    memcpy(
        &temporary_b_data,
        temporary_head + SMOKE_BUFFER_HEAD_DATA_OFFSET,
        sizeof(temporary_b_data));
    if (temporary_b_folio != temporary_folio ||
        temporary_b_data != kb_linux_kvm_page_payload(temporary_folio, 123, 1))
    {
        kb_fs_subsystem_free_buffer_head(temporary_head);
        kb_kvm_free_pages_stub(temporary_folio, 0);
        kb_fs_block_device_destroy(fs_device);
        return 9;
    }
    kb_fs_subsystem_free_buffer_head(temporary_head);
    kb_kvm_free_pages_stub(temporary_folio, 0);

    unsigned char bio_read_buffer[SMOKE_SECTOR_SIZE];
    memset(bio_read_buffer, 0, sizeof(bio_read_buffer));
    void *read_bio = kb_fs_subsystem_bio_alloc_bioset(NULL, 1, KB_FS_BIO_OP_READ, 0, NULL);
    if (read_bio == NULL ||
        kb_fs_subsystem_bio_add_folio(read_bio, bio_read_buffer, sizeof(bio_read_buffer), 0) != 1)
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

    unsigned char batch_read_buffer0[SMOKE_SECTOR_SIZE];
    unsigned char batch_read_buffer1[SMOKE_SECTOR_SIZE];
    memset(batch_read_buffer0, 0, sizeof(batch_read_buffer0));
    memset(batch_read_buffer1, 0, sizeof(batch_read_buffer1));
    memset(backing + 3u * SMOKE_SECTOR_SIZE, 0x3c, SMOKE_SECTOR_SIZE);
    kb_fs_subsystem_bio_set_auto_drain(0);
    void *batch_read_bio0 = kb_fs_subsystem_bio_alloc_bioset(
        NULL, 1, KB_FS_BIO_OP_READ, 0, NULL);
    void *batch_read_bio1 = kb_fs_subsystem_bio_alloc_bioset(
        NULL, 1, KB_FS_BIO_OP_READ, 0, NULL);
    if (batch_read_bio0 == NULL || batch_read_bio1 == NULL ||
        kb_fs_subsystem_bio_add_folio(
            batch_read_bio0,
            batch_read_buffer0,
            sizeof(batch_read_buffer0),
            0) != 1 ||
        kb_fs_subsystem_bio_add_folio(
            batch_read_bio1,
            batch_read_buffer1,
            sizeof(batch_read_buffer1),
            0) != 1)
    {
        kb_fs_subsystem_bio_put(batch_read_bio0);
        kb_fs_subsystem_bio_put(batch_read_bio1);
        kb_fs_block_device_destroy(fs_device);
        return 11;
    }
    kb_fs_subsystem_bio_set_sector(batch_read_bio0, 2);
    kb_fs_subsystem_bio_set_sector(batch_read_bio1, 3);
    kb_fs_subsystem_submit_bio(batch_read_bio0);
    kb_fs_subsystem_submit_bio(batch_read_bio1);
    if (kb_fs_subsystem_bio_queue_depth() != 2 ||
        kb_fs_subsystem_bio_drain() != 2 ||
        disk_read_batch_count != 1 ||
        memcmp(batch_read_buffer0, write_buffer, sizeof(batch_read_buffer0)) != 0)
    {
        kb_fs_subsystem_bio_put(batch_read_bio0);
        kb_fs_subsystem_bio_put(batch_read_bio1);
        kb_fs_block_device_destroy(fs_device);
        return 11;
    }
    for (size_t i = 0; i < sizeof(batch_read_buffer1); ++i) {
        if (batch_read_buffer1[i] != 0x3c) {
            kb_fs_subsystem_bio_put(batch_read_bio0);
            kb_fs_subsystem_bio_put(batch_read_bio1);
            kb_fs_block_device_destroy(fs_device);
            return 11;
        }
    }
    kb_fs_subsystem_bio_put(batch_read_bio0);
    kb_fs_subsystem_bio_put(batch_read_bio1);
    kb_fs_subsystem_bio_set_auto_drain(1);

    unsigned char multivector_read_buffer[2][SMOKE_SECTOR_SIZE];
    for (size_t vector = 0; vector < 2; ++vector) {
        memset(
            backing + (8u + vector) * SMOKE_SECTOR_SIZE,
            (int)(0x48u + vector),
            SMOKE_SECTOR_SIZE);
        memset(multivector_read_buffer[vector], 0, SMOKE_SECTOR_SIZE);
    }
    void *multivector_read_bio = kb_fs_subsystem_bio_alloc_bioset(
        NULL, 2, KB_FS_BIO_OP_READ, 0, NULL);
    if (multivector_read_bio == NULL ||
        kb_fs_subsystem_bio_add_page(
            multivector_read_bio,
            multivector_read_buffer[0],
            SMOKE_SECTOR_SIZE,
            0) != SMOKE_SECTOR_SIZE ||
        kb_fs_subsystem_bio_add_page(
            multivector_read_bio,
            multivector_read_buffer[1],
            SMOKE_SECTOR_SIZE,
            0) != SMOKE_SECTOR_SIZE)
    {
        kb_fs_subsystem_bio_put(multivector_read_bio);
        kb_fs_block_device_destroy(fs_device);
        return 11;
    }
    kb_fs_subsystem_bio_set_sector(multivector_read_bio, 8);
    kb_fs_subsystem_bio_set_end_io(
        multivector_read_bio,
        smoke_bio_complete);
    kb_fs_subsystem_submit_bio(multivector_read_bio);
    int multivector_read_ok =
        kb_fs_subsystem_bio_result(multivector_read_bio) == 0 &&
        disk_read_batch_count == 2;
    for (size_t vector = 0; vector < 2; ++vector) {
        for (size_t byte = 0; byte < SMOKE_SECTOR_SIZE; ++byte) {
            multivector_read_ok = multivector_read_ok &&
                multivector_read_buffer[vector][byte] ==
                    (unsigned char)(0x48u + vector);
        }
    }
    kb_fs_subsystem_bio_put(multivector_read_bio);
    if (!multivector_read_ok) {
        kb_fs_block_device_destroy(fs_device);
        return 11;
    }

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
        bio_completion_count != 3)
    {
        kb_fs_subsystem_bio_put(write_bio);
        kb_fs_block_device_destroy(fs_device);
        return 13;
    }
    kb_fs_subsystem_bio_put(write_bio);

    void *max_vec_bio = kb_fs_subsystem_bio_alloc_bioset(
        NULL, 256, KB_FS_BIO_OP_WRITE, 0, NULL);
    if (max_vec_bio == NULL) {
        kb_fs_block_device_destroy(fs_device);
        return 14;
    }
    unsigned char max_vec_payload[256];
    for (size_t i = 0; i < sizeof(max_vec_payload); ++i) {
        if (kb_fs_subsystem_bio_add_page(
                max_vec_bio, &max_vec_payload[i], 1, 0) != 1)
        {
            kb_fs_subsystem_bio_put(max_vec_bio);
            kb_fs_block_device_destroy(fs_device);
            return 14;
        }
    }
    if (kb_fs_subsystem_bio_add_page(max_vec_bio, max_vec_payload, 1, 0) != 0 ||
        kb_fs_subsystem_bio_alloc_bioset(NULL, 257, KB_FS_BIO_OP_WRITE, 0, NULL) != NULL)
    {
        kb_fs_subsystem_bio_put(max_vec_bio);
        kb_fs_block_device_destroy(fs_device);
        return 14;
    }
    kb_fs_subsystem_bio_put(max_vec_bio);

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
        bio_completion_count != 3)
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
        disk_flush_count != 1 ||
        memcmp(backing + 5u * SMOKE_SECTOR_SIZE, async_write_buffer, sizeof(async_write_buffer)) != 0 ||
        bio_completion_count != 5)
    {
        kb_fs_subsystem_bio_put(async_write_bio);
        kb_fs_subsystem_bio_put(flush_bio);
        kb_fs_block_device_destroy(fs_device);
        return 16;
    }
    kb_fs_subsystem_bio_put(async_write_bio);
    kb_fs_subsystem_bio_put(flush_bio);
    kb_fs_subsystem_bio_set_auto_drain(1);

    memset(backing + 7u * SMOKE_SECTOR_SIZE, 0x7f, SMOKE_SECTOR_SIZE);
    if (kb_fs_subsystem_issue_zeroout(NULL, 7, 1, 0, 0) != 0 ||
        kb_fs_subsystem_issue_discard(NULL, 7, 1, 0) != -95)
    {
        kb_fs_block_device_destroy(fs_device);
        return 16;
    }
    for (size_t i = 0; i < SMOKE_SECTOR_SIZE; ++i) {
        if (backing[7u * SMOKE_SECTOR_SIZE + i] != 0) {
            kb_fs_block_device_destroy(fs_device);
            return 16;
        }
    }

    unsigned char read_bh[SMOKE_BUFFER_HEAD_BYTES];
    unsigned char bh_read_buffer[SMOKE_SECTOR_SIZE];
    uint64_t bh_state = SMOKE_BH_LOCK | SMOKE_BH_MAPPED;
    uint64_t bh_block = 2;
    uint64_t bh_size = sizeof(bh_read_buffer);
    uint32_t bh_refs = 1;
    void *bh_data = bh_read_buffer;
    void (*bh_end_io)(void *, int) = kb_fs_subsystem_end_buffer_read_sync;
    memset(read_bh, 0, sizeof(read_bh));
    memset(bh_read_buffer, 0, sizeof(bh_read_buffer));
    memcpy(read_bh, &bh_state, sizeof(bh_state));
    memcpy(read_bh + SMOKE_BUFFER_HEAD_BLOCKNR_OFFSET, &bh_block, sizeof(bh_block));
    memcpy(read_bh + SMOKE_BUFFER_HEAD_SIZE_OFFSET, &bh_size, sizeof(bh_size));
    memcpy(read_bh + SMOKE_BUFFER_HEAD_DATA_OFFSET, &bh_data, sizeof(bh_data));
    memcpy(read_bh + SMOKE_BUFFER_HEAD_END_IO_OFFSET, &bh_end_io, sizeof(bh_end_io));
    memcpy(read_bh + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET, &bh_refs, sizeof(bh_refs));
    if (kb_fs_subsystem_bh_read(read_bh, 0, 1) != 0) {
        kb_fs_block_device_destroy(fs_device);
        return 17;
    }
    memcpy(&bh_state, read_bh, sizeof(bh_state));
    memcpy(&bh_refs, read_bh + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET, sizeof(bh_refs));
    if (memcmp(bh_read_buffer, write_buffer, sizeof(bh_read_buffer)) != 0 ||
        (bh_state & (SMOKE_BH_UPTODATE | SMOKE_BH_MAPPED)) !=
            (SMOKE_BH_UPTODATE | SMOKE_BH_MAPPED) ||
        (bh_state & (SMOKE_BH_LOCK | SMOKE_BH_REQ)) != 0 ||
        bh_refs != 1)
    {
        kb_fs_block_device_destroy(fs_device);
        return 17;
    }

    unsigned char write_bh[SMOKE_BUFFER_HEAD_BYTES];
    unsigned char bh_write_buffer[SMOKE_SECTOR_SIZE];
    for (size_t i = 0; i < sizeof(bh_write_buffer); i++) {
        bh_write_buffer[i] = (unsigned char)(0x69u ^ i);
    }
    bh_state = SMOKE_BH_DIRTY | SMOKE_BH_LOCK | SMOKE_BH_MAPPED;
    bh_block = 6;
    bh_size = sizeof(bh_write_buffer);
    bh_refs = 2;
    bh_data = bh_write_buffer;
    bh_end_io = kb_fs_subsystem_end_buffer_write_sync;
    memset(write_bh, 0, sizeof(write_bh));
    memcpy(write_bh, &bh_state, sizeof(bh_state));
    memcpy(write_bh + SMOKE_BUFFER_HEAD_BLOCKNR_OFFSET, &bh_block, sizeof(bh_block));
    memcpy(write_bh + SMOKE_BUFFER_HEAD_SIZE_OFFSET, &bh_size, sizeof(bh_size));
    memcpy(write_bh + SMOKE_BUFFER_HEAD_DATA_OFFSET, &bh_data, sizeof(bh_data));
    memcpy(write_bh + SMOKE_BUFFER_HEAD_END_IO_OFFSET, &bh_end_io, sizeof(bh_end_io));
    memcpy(write_bh + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET, &bh_refs, sizeof(bh_refs));
    kb_fs_subsystem_submit_bh(KB_FS_BIO_OP_WRITE, write_bh);
    kb_fs_subsystem_wait_on_buffer(write_bh);
    memcpy(&bh_state, write_bh, sizeof(bh_state));
    memcpy(&bh_refs, write_bh + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET, sizeof(bh_refs));
    if (memcmp(backing + 6u * SMOKE_SECTOR_SIZE, bh_write_buffer, sizeof(bh_write_buffer)) != 0 ||
        (bh_state & (SMOKE_BH_UPTODATE | SMOKE_BH_MAPPED)) !=
            (SMOKE_BH_UPTODATE | SMOKE_BH_MAPPED) ||
        (bh_state & (SMOKE_BH_DIRTY | SMOKE_BH_LOCK | SMOKE_BH_REQ)) != 0 ||
        bh_refs != 1)
    {
        kb_fs_block_device_destroy(fs_device);
        return 18;
    }

    /* A buffer_head for a sub-page block points b_data into the folio.  The
     * bio vector must preserve that page offset; using offset zero silently
     * corrupts a different block when ext4 uses 1 KiB or 2 KiB blocks. */
    enum { SMOKE_SUBPAGE_BLOCK_SIZE = 1024 };
    void *subpage_folio = kb_kvm_alloc_pages_stub(0, 0);
    unsigned char *subpage_payload = kb_linux_kvm_page_payload(
        subpage_folio, 0, 4096);
    unsigned char subpage_read_bh[SMOKE_BUFFER_HEAD_BYTES] = {0};
    if (subpage_folio == NULL || subpage_payload == NULL) {
        fprintf(stderr, "subpage allocation failed folio=%p payload=%p\n",
            subpage_folio, (void *)subpage_payload);
        kb_kvm_free_pages_stub(subpage_folio, 0);
        kb_fs_block_device_destroy(fs_device);
        return 19;
    }
    memset(subpage_payload, 0xc3, 4096);
    for (size_t i = 0; i < SMOKE_SUBPAGE_BLOCK_SIZE; ++i) {
        backing[3u * SMOKE_SUBPAGE_BLOCK_SIZE + i] =
            (unsigned char)(0x8du ^ i);
    }
    bh_state = SMOKE_BH_LOCK | SMOKE_BH_MAPPED;
    bh_block = 3;
    bh_size = SMOKE_SUBPAGE_BLOCK_SIZE;
    bh_refs = 1;
    bh_data = subpage_payload + 2u * SMOKE_SUBPAGE_BLOCK_SIZE;
    bh_end_io = kb_fs_subsystem_end_buffer_read_sync;
    memcpy(subpage_read_bh, &bh_state, sizeof(bh_state));
    memcpy(subpage_read_bh + SMOKE_BUFFER_HEAD_FOLIO_OFFSET,
        &subpage_folio, sizeof(subpage_folio));
    memcpy(subpage_read_bh + SMOKE_BUFFER_HEAD_BLOCKNR_OFFSET,
        &bh_block, sizeof(bh_block));
    memcpy(subpage_read_bh + SMOKE_BUFFER_HEAD_SIZE_OFFSET,
        &bh_size, sizeof(bh_size));
    memcpy(subpage_read_bh + SMOKE_BUFFER_HEAD_DATA_OFFSET,
        &bh_data, sizeof(bh_data));
    memcpy(subpage_read_bh + SMOKE_BUFFER_HEAD_END_IO_OFFSET,
        &bh_end_io, sizeof(bh_end_io));
    memcpy(subpage_read_bh + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET,
        &bh_refs, sizeof(bh_refs));
    if (kb_fs_subsystem_bh_read(subpage_read_bh, 0, 1) != 0 ||
        memcmp(subpage_payload + 2u * SMOKE_SUBPAGE_BLOCK_SIZE,
            backing + 3u * SMOKE_SUBPAGE_BLOCK_SIZE,
            SMOKE_SUBPAGE_BLOCK_SIZE) != 0)
    {
        fprintf(stderr, "subpage read offset/data verification failed\n");
        kb_kvm_free_pages_stub(subpage_folio, 0);
        kb_fs_block_device_destroy(fs_device);
        return 19;
    }
    for (size_t i = 0; i < 2u * SMOKE_SUBPAGE_BLOCK_SIZE; ++i) {
        if (subpage_payload[i] != 0xc3) {
            fprintf(stderr, "subpage read clobbered prefix offset=%zu\n", i);
            kb_kvm_free_pages_stub(subpage_folio, 0);
            kb_fs_block_device_destroy(fs_device);
            return 19;
        }
    }
    for (size_t i = 3u * SMOKE_SUBPAGE_BLOCK_SIZE; i < 4096; ++i) {
        if (subpage_payload[i] != 0xc3) {
            fprintf(stderr, "subpage read clobbered suffix offset=%zu\n", i);
            kb_kvm_free_pages_stub(subpage_folio, 0);
            kb_fs_block_device_destroy(fs_device);
            return 19;
        }
    }

    unsigned char subpage_write_bh[SMOKE_BUFFER_HEAD_BYTES] = {0};
    for (size_t i = 0; i < SMOKE_SUBPAGE_BLOCK_SIZE; ++i) {
        subpage_payload[SMOKE_SUBPAGE_BLOCK_SIZE + i] =
            (unsigned char)(0x47u + i);
    }
    bh_state = SMOKE_BH_DIRTY | SMOKE_BH_LOCK | SMOKE_BH_MAPPED;
    bh_block = 5;
    bh_size = SMOKE_SUBPAGE_BLOCK_SIZE;
    bh_refs = 1;
    bh_data = subpage_payload + SMOKE_SUBPAGE_BLOCK_SIZE;
    bh_end_io = kb_fs_subsystem_end_buffer_write_sync;
    memcpy(subpage_write_bh, &bh_state, sizeof(bh_state));
    memcpy(subpage_write_bh + SMOKE_BUFFER_HEAD_FOLIO_OFFSET,
        &subpage_folio, sizeof(subpage_folio));
    memcpy(subpage_write_bh + SMOKE_BUFFER_HEAD_BLOCKNR_OFFSET,
        &bh_block, sizeof(bh_block));
    memcpy(subpage_write_bh + SMOKE_BUFFER_HEAD_SIZE_OFFSET,
        &bh_size, sizeof(bh_size));
    memcpy(subpage_write_bh + SMOKE_BUFFER_HEAD_DATA_OFFSET,
        &bh_data, sizeof(bh_data));
    memcpy(subpage_write_bh + SMOKE_BUFFER_HEAD_END_IO_OFFSET,
        &bh_end_io, sizeof(bh_end_io));
    memcpy(subpage_write_bh + SMOKE_BUFFER_HEAD_REFCOUNT_OFFSET,
        &bh_refs, sizeof(bh_refs));
    kb_fs_subsystem_submit_bh(KB_FS_BIO_OP_WRITE, subpage_write_bh);
    kb_fs_subsystem_wait_on_buffer(subpage_write_bh);
    if (memcmp(backing + 5u * SMOKE_SUBPAGE_BLOCK_SIZE,
            subpage_payload + SMOKE_SUBPAGE_BLOCK_SIZE,
            SMOKE_SUBPAGE_BLOCK_SIZE) != 0)
    {
        fprintf(stderr, "subpage write offset/data verification failed\n");
        kb_kvm_free_pages_stub(subpage_folio, 0);
        kb_fs_block_device_destroy(fs_device);
        return 19;
    }
    kb_kvm_free_pages_stub(subpage_folio, 0);

    unsigned char batch_bio_a[SMOKE_SECTOR_SIZE];
    unsigned char batch_bio_b[SMOKE_SECTOR_SIZE];
    memset(batch_bio_a, 0x31, sizeof(batch_bio_a));
    memset(batch_bio_b, 0x72, sizeof(batch_bio_b));
    void *batch_write_bio = kb_fs_subsystem_bio_alloc_bioset(
        NULL, 2, KB_FS_BIO_OP_WRITE, 0, NULL);
    if (batch_write_bio == NULL ||
        kb_fs_subsystem_bio_add_page(
            batch_write_bio, batch_bio_a, sizeof(batch_bio_a), 0) !=
            (int)sizeof(batch_bio_a) ||
        kb_fs_subsystem_bio_add_page(
            batch_write_bio, batch_bio_b, sizeof(batch_bio_b), 0) !=
            (int)sizeof(batch_bio_b))
    {
        kb_fs_subsystem_bio_put(batch_write_bio);
        kb_fs_block_device_destroy(fs_device);
        return 20;
    }
    kb_fs_subsystem_bio_set_sector(batch_write_bio, 10);
    kb_fs_subsystem_bio_set_end_io(batch_write_bio, smoke_bio_complete);
    kb_fs_subsystem_submit_bio(batch_write_bio);
    if (kb_fs_subsystem_bio_result(batch_write_bio) != 0 ||
        disk_write_batch_count != 1 ||
        memcmp(backing + 10u * SMOKE_SECTOR_SIZE,
            batch_bio_a, sizeof(batch_bio_a)) != 0 ||
        memcmp(backing + 11u * SMOKE_SECTOR_SIZE,
            batch_bio_b, sizeof(batch_bio_b)) != 0)
    {
        kb_fs_subsystem_bio_put(batch_write_bio);
        kb_fs_block_device_destroy(fs_device);
        return 20;
    }
    kb_fs_subsystem_bio_put(batch_write_bio);

    kb_block_disk_snapshot_t snapshot;
    if (kb_block_subsystem_disk_snapshot(disk, &snapshot) != 0 ||
        snapshot.read_count != 12 ||
        snapshot.write_count != 8 ||
        snapshot.bytes_read !=
            sizeof(read_buffer) * 11u + SMOKE_SUBPAGE_BLOCK_SIZE ||
        snapshot.bytes_written !=
            sizeof(write_buffer) * 7u + SMOKE_SUBPAGE_BLOCK_SIZE)
    {
        fprintf(stderr,
            "block snapshot mismatch reads=%llu writes=%llu read_bytes=%llu write_bytes=%llu\n",
            (unsigned long long)snapshot.read_count,
            (unsigned long long)snapshot.write_count,
            (unsigned long long)snapshot.bytes_read,
            (unsigned long long)snapshot.bytes_written);
        kb_fs_block_device_destroy(fs_device);
        return 19;
    }

    unsigned char barrier_payload[3][SMOKE_SECTOR_SIZE];
    void *barrier_bios[3] = {0};
    const unsigned int flush_count_before_barrier = disk_flush_count;
    const unsigned int write_batch_count_before_barrier = disk_write_batch_count;
    io_event_count = 0;
    memset(io_events, 0, sizeof(io_events));
    kb_fs_subsystem_bio_set_auto_drain(0);
    for (size_t i = 0; i < 3; ++i) {
        memset(barrier_payload[i], (int)(0x81u + i), sizeof(barrier_payload[i]));
        unsigned int opf = KB_FS_BIO_OP_WRITE;
        if (i == 1) {
            opf |= KB_FS_BIO_REQ_PREFLUSH | KB_FS_BIO_REQ_FUA;
        }
        barrier_bios[i] = kb_fs_subsystem_bio_alloc_bioset(NULL, 1, opf, 0, NULL);
        if (barrier_bios[i] == NULL ||
            kb_fs_subsystem_bio_add_page(
                barrier_bios[i],
                barrier_payload[i],
                sizeof(barrier_payload[i]),
                0) != (int)sizeof(barrier_payload[i]))
        {
            for (size_t j = 0; j < 3; ++j) {
                kb_fs_subsystem_bio_put(barrier_bios[j]);
            }
            kb_fs_subsystem_bio_set_auto_drain(1);
            kb_fs_block_device_destroy(fs_device);
            return 21;
        }
        kb_fs_subsystem_bio_set_sector(barrier_bios[i], 12u + i);
        kb_fs_subsystem_submit_bio(barrier_bios[i]);
    }
    if (kb_fs_subsystem_bio_drain() != 3) {
        for (size_t i = 0; i < 3; ++i) {
            kb_fs_subsystem_bio_put(barrier_bios[i]);
        }
        kb_fs_subsystem_bio_set_auto_drain(1);
        kb_fs_block_device_destroy(fs_device);
        return 21;
    }
    kb_fs_subsystem_bio_set_auto_drain(1);
    const unsigned char expected_barrier_events[] = {
        SMOKE_IO_EVENT_WRITE,
        SMOKE_IO_EVENT_FLUSH,
        SMOKE_IO_EVENT_WRITE,
        SMOKE_IO_EVENT_FLUSH,
        SMOKE_IO_EVENT_WRITE,
    };
    int barrier_ok =
        io_event_count == sizeof(expected_barrier_events) &&
        memcmp(io_events, expected_barrier_events, sizeof(expected_barrier_events)) == 0 &&
        disk_flush_count == flush_count_before_barrier + 2u &&
        disk_write_batch_count == write_batch_count_before_barrier;
    for (size_t i = 0; i < 3; ++i) {
        barrier_ok = barrier_ok &&
            kb_fs_subsystem_bio_result(barrier_bios[i]) == 0 &&
            memcmp(
                backing + (12u + i) * SMOKE_SECTOR_SIZE,
                barrier_payload[i],
                sizeof(barrier_payload[i])) == 0;
        kb_fs_subsystem_bio_put(barrier_bios[i]);
    }
    if (!barrier_ok) {
        kb_fs_block_device_destroy(fs_device);
        return 21;
    }

    io_event_count = 0;
    memset(io_events, 0, sizeof(io_events));
    const unsigned int flush_count_before_flush_only = disk_flush_count;
    void *flush_only_bio = kb_fs_subsystem_bio_alloc_bioset(
        NULL,
        0,
        KB_FS_BIO_OP_WRITE | KB_FS_BIO_REQ_PREFLUSH,
        0,
        NULL);
    if (flush_only_bio == NULL) {
        kb_fs_block_device_destroy(fs_device);
        return 22;
    }
    kb_fs_subsystem_submit_bio(flush_only_bio);
    const int flush_only_ok =
        kb_fs_subsystem_bio_result(flush_only_bio) == 0 &&
        disk_flush_count == flush_count_before_flush_only + 1u &&
        io_event_count == 1 &&
        io_events[0] == SMOKE_IO_EVENT_FLUSH;
    kb_fs_subsystem_bio_put(flush_only_bio);
    if (!flush_only_ok) {
        kb_fs_block_device_destroy(fs_device);
        return 22;
    }

    kb_fs_subsystem_set_mount_probe_block_device(NULL);
    kb_fs_block_device_destroy(fs_device);
    kb_block_subsystem_object_free(part0);
    kb_block_subsystem_object_free(disk);
    kb_block_subsystem_object_free(queue);
    return 0;
}
