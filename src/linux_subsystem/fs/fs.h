#pragma once

#include "kobox/interface.h"
#include "kobox/platform.h"

typedef enum kb_fs_operation {
    KB_FS_OPERATION_NONE = 0,
    KB_FS_OPERATION_MOUNT = 1,
    KB_FS_OPERATION_READ = 2,
    KB_FS_OPERATION_WRITE = 3,
    KB_FS_OPERATION_READDIR = 4,
} kb_fs_operation_t;

typedef struct kb_fs_ipc_request {
    uint32_t version;
    kb_fs_operation_t operation;
    uint64_t request_id;
    uint64_t handle;
    uint64_t offset;
    uint64_t flags;
    const char *source;
    const char *path;
    const char *fs_type;
    const void *input;
    size_t input_size;
    void *output;
    size_t output_capacity;
    size_t output_size;
    uint32_t handled;
    int result_code;
} kb_fs_ipc_request_t;

typedef struct kb_fs_mount_desc {
    const char *source;
    const char *target;
    const char *fs_type;
    uint64_t flags;
    const void *data;
    size_t data_size;
    void *block_disk;
} kb_fs_mount_desc_t;

typedef struct kb_fs_type_snapshot {
    void *fs_type;
    const char *name;
    void *owner_module;
    uint32_t register_count;
} kb_fs_type_snapshot_t;

typedef struct kb_fs_mount_snapshot {
    uint64_t handle;
    const char *source;
    const char *target;
    const char *fs_type;
    void *block_disk;
} kb_fs_mount_snapshot_t;

#define KB_FS_MOUNT_PATH_BLOCK_READ_MAX 8u

typedef struct kb_fs_mount_path_probe {
    void *fs_type;
    void *init_fs_context;
    void *fs_context;
    void *fs_context_ops;
    void *get_tree;
    void *get_tree_bdev_fc;
    void *get_tree_bdev_fill_super;
    void *super_block;
    void *block_device;
    void *root_inode;
    void *root_dentry;
    void *root_vfsmount;
    void *lookup_inode;
    void *lookup_dentry;
    int fill_super_result;
    uint64_t bdev_getblk_calls;
    uint64_t last_block_number;
    uint32_t last_block_size;
    uint16_t observed_ext4_magic;
    uint32_t block_read_count;
    uint64_t block_read_numbers[KB_FS_MOUNT_PATH_BLOCK_READ_MAX];
    uint32_t block_read_sizes[KB_FS_MOUNT_PATH_BLOCK_READ_MAX];
    int init_result;
    int get_tree_result;
    uint64_t get_tree_bdev_calls;
} kb_fs_mount_path_probe_t;

typedef kb_fs_mount_path_probe_t kb_fs_mount_result_t;

extern unsigned char kb_fs_subsystem_blockdev_superblock[];

typedef struct kb_fs_block_device kb_fs_block_device_t;

typedef int (*kb_fs_block_read_fn)(void *ctx, uint64_t offset, void *buffer, size_t size);
typedef int (*kb_fs_block_write_fn)(void *ctx, uint64_t offset, const void *buffer, size_t size);
typedef int (*kb_fs_block_flush_fn)(void *ctx);
typedef void (*kb_fs_block_destroy_fn)(void *ctx);

typedef struct kb_fs_block_read_request {
    uint64_t offset;
    void *buffer;
    size_t size;
} kb_fs_block_read_request_t;

typedef struct kb_fs_block_write_request {
    uint64_t offset;
    const void *buffer;
    size_t size;
} kb_fs_block_write_request_t;

typedef int (*kb_fs_block_read_batch_fn)(
    void *ctx,
    const kb_fs_block_read_request_t *requests,
    size_t request_count);
typedef int (*kb_fs_block_write_batch_fn)(
    void *ctx,
    const kb_fs_block_write_request_t *requests,
    size_t request_count);
typedef int (*kb_fs_block_write_flags_fn)(
    void *ctx,
    uint64_t offset,
    const void *buffer,
    size_t size,
    uint32_t flags);

enum {
    KB_FS_BLOCK_WRITE_FUA = 1u << 0,
};

typedef struct kb_fs_block_device_desc {
    const char *name;
    uint64_t size_bytes;
    uint32_t logical_block_size;
    void *ctx;
    kb_fs_block_read_fn read;
    kb_fs_block_read_batch_fn read_batch;
    kb_fs_block_write_fn write;
    kb_fs_block_write_batch_fn write_batch;
    kb_fs_block_write_flags_fn write_flags;
    kb_fs_block_flush_fn flush;
    kb_fs_block_destroy_fn destroy;
} kb_fs_block_device_desc_t;

typedef enum kb_fs_bio_operation {
    KB_FS_BIO_OP_READ = 0,
    KB_FS_BIO_OP_WRITE = 1,
    KB_FS_BIO_OP_FLUSH = 2,
    KB_FS_BIO_OP_DISCARD = 3,
} kb_fs_bio_operation_t;

/* Linux 6.12 blk_opf_t request flags consumed by the Kobox block bridge. */
enum {
    KB_FS_BIO_REQ_FUA = 1u << 17,
    KB_FS_BIO_REQ_PREFLUSH = 1u << 18,
};

typedef struct kb_fs_bio_snapshot {
    void *bio;
    void *block_device;
    uint32_t operation;
    uint64_t sector;
    size_t length;
    size_t offset;
    int result;
    uint32_t submitted;
    uint32_t queued;
    uint32_t completed;
} kb_fs_bio_snapshot_t;

typedef struct kb_fs_read_desc {
    uint64_t handle;
    const char *path;
    uint64_t offset;
    void *buffer;
    size_t byte_count;
    size_t *out_bytes;
} kb_fs_read_desc_t;

typedef struct kb_fs_write_desc {
    uint64_t handle;
    const char *path;
    uint64_t offset;
    const void *buffer;
    size_t byte_count;
    size_t *out_bytes;
} kb_fs_write_desc_t;

typedef struct kb_fs_readdir_desc {
    uint64_t handle;
    const char *path;
    void *buffer;
    size_t byte_count;
    size_t *out_bytes;
} kb_fs_readdir_desc_t;

typedef struct kb_fs_read_profile {
    uint64_t calls;
    uint64_t bytes;
    uint64_t total_cycles;
    uint64_t extent_lookup_calls;
    uint64_t extent_lookup_cycles;
    uint64_t device_read_calls;
    uint64_t device_read_cycles;
    uint64_t overlay_calls;
    uint64_t overlay_cycles;
    uint64_t partial_copy_calls;
    uint64_t partial_copy_cycles;
} kb_fs_read_profile_t;

typedef struct kb_fs_storage_trace {
    uint64_t block_read_calls;
    uint64_t block_read_bytes;
    uint64_t block_write_calls;
    uint64_t block_write_bytes;
    uint64_t bio_submit[4];
    uint64_t bio_complete[4];
    uint64_t bio_errors[4];
    uint64_t readahead_requests;
    uint64_t readahead_folios;
    uint64_t readahead_aops_calls;
    uint64_t readahead_fallback_calls;
} kb_fs_storage_trace_t;

typedef struct kb_fs_hotpath_profile {
    uint64_t xarray_refresh_calls;
    uint64_t xarray_refresh_cycles;
    uint64_t xarray_load_calls;
    uint64_t xarray_load_cycles;
    uint64_t folio_lookup_calls;
    uint64_t folio_lookup_cycles;
    uint64_t buffer_lookup_calls;
    uint64_t buffer_lookup_cycles;
    uint64_t inode_find_calls;
    uint64_t inode_find_cycles;
    uint64_t inode_claim_calls;
    uint64_t inode_claim_cycles;
    uint64_t dentry_find_calls;
    uint64_t dentry_find_cycles;
    uint64_t dentry_claim_calls;
    uint64_t dentry_claim_cycles;
    uint64_t write_begin_calls;
    uint64_t write_begin_cycles;
    uint64_t write_end_calls;
    uint64_t write_end_cycles;
    uint64_t bio_submit_calls;
    uint64_t bio_preflush_calls;
    uint64_t bio_fua_calls;
    uint64_t bio_flush_op_calls;
    uint64_t plug_start_calls;
    uint64_t plug_finish_calls;
    uint64_t plug_queued_bios;
    uint64_t plug_max_queued_bios;
} kb_fs_hotpath_profile_t;

kb_status_t kb_fs_subsystem_bind_ipc_interface(kb_platform_t *platform, kb_interface_t **out_interface);
kb_status_t kb_fs_subsystem_dispatch(kb_interface_t *interface, kb_fs_ipc_request_t *request);
kb_status_t kb_fs_subsystem_mount(kb_interface_t *interface, const kb_fs_mount_desc_t *desc, uint64_t *out_handle);
kb_status_t kb_fs_subsystem_read(kb_interface_t *interface, const kb_fs_read_desc_t *desc);
kb_status_t kb_fs_subsystem_write(kb_interface_t *interface, const kb_fs_write_desc_t *desc);
kb_status_t kb_fs_subsystem_readdir(kb_interface_t *interface, const kb_fs_readdir_desc_t *desc);
void kb_fs_read_profile_snapshot(kb_fs_read_profile_t *out_profile);
void kb_fs_storage_trace_snapshot(kb_fs_storage_trace_t *out_trace);
void kb_fs_hotpath_profile_reset(void);
void kb_fs_hotpath_profile_snapshot(kb_fs_hotpath_profile_t *out_profile);
int kb_fs_subsystem_register_filesystem(void *fs_type);
int kb_fs_subsystem_unregister_filesystem(void *fs_type);
void *kb_fs_subsystem_mount_registered(const char *name, int flags, const char *dev_name, void *data);
int kb_fs_block_device_create(const kb_fs_block_device_desc_t *desc, kb_fs_block_device_t **out_device);
int kb_fs_block_device_create_image(const char *name, const char *image_path, kb_fs_block_device_t **out_device);
int kb_fs_block_device_create_from_disk(const char *name, void *disk, kb_fs_block_device_t **out_device);
int kb_fs_block_device_create_from_disk_range(
    const char *name,
    void *disk,
    uint64_t start_sector,
    uint64_t sector_count,
    kb_fs_block_device_t **out_device);
int kb_fs_block_device_create_from_disk_gpt_partition(
    const char *name,
    void *disk,
    uint32_t partition_index,
    kb_fs_block_device_t **out_device);
int kb_fs_block_device_read(kb_fs_block_device_t *device, uint64_t offset, void *buffer, size_t size);
int kb_fs_block_device_read_batch(
    kb_fs_block_device_t *device,
    const kb_fs_block_read_request_t *requests,
    size_t request_count);
int kb_fs_block_device_write(kb_fs_block_device_t *device, uint64_t offset, const void *buffer, size_t size);
int kb_fs_block_device_write_batch(
    kb_fs_block_device_t *device,
    const kb_fs_block_write_request_t *requests,
    size_t request_count);
void kb_fs_block_device_destroy(kb_fs_block_device_t *device);
int kb_fs_subsystem_set_mount_block_device(kb_fs_block_device_t *device);
int kb_fs_subsystem_set_mount_probe_block_device(kb_fs_block_device_t *device);
int kb_fs_subsystem_get_tree_bdev(void *fs_context, int (*fill_super)(void *super_block, void *fs_context));
int kb_fs_subsystem_fs_parse(
    void *log,
    const void *description,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_lookup_param(
    void *fs_context,
    void *parameter,
    int want_block_device,
    unsigned int flags,
    void *path);
int kb_fs_subsystem_fs_param_is_blockdev(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_param_is_enum(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_param_is_s32(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_param_is_string(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_param_is_u32(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_param_is_uid(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_fs_param_is_gid(
    void *log,
    const void *spec,
    void *parameter,
    void *result);
int kb_fs_subsystem_simple_statfs(void *dentry, void *buffer);
void *kb_fs_subsystem_bdev_getblk(void *bdev, uint64_t block_number, unsigned int block_size, unsigned int gfp);
void *kb_fs_subsystem_bread_gfp(
    void *bdev,
    uint64_t block_number,
    unsigned int block_size,
    unsigned int gfp);
void *kb_fs_subsystem_find_get_block(void *bdev, uint64_t block_number, unsigned int block_size);
void *kb_fs_subsystem_alloc_buffer_head(unsigned int gfp);
void kb_fs_subsystem_buffer_head_put(void *buffer_head);
void kb_fs_subsystem_free_buffer_head(void *buffer_head);
void kb_fs_subsystem_folio_set_bh(void *buffer_head, void *folio, unsigned long offset);
int kb_fs_subsystem_setattr_prepare(void *idmap, void *dentry, void *iattr);
void kb_fs_subsystem_setattr_copy(void *idmap, void *inode, const void *iattr);
const char *kb_fs_subsystem_simple_get_link(void *dentry, void *inode, void *done);
void kb_fs_subsystem_mark_buffer_dirty(void *buffer_head);
void kb_fs_subsystem_mark_buffer_dirty_inode(void *buffer_head, void *mapping);
void kb_fs_subsystem_invalidate_inode_buffers(void *inode);
void kb_fs_subsystem_bforget(void *buffer_head);
int kb_fs_subsystem_bh_uptodate_or_lock(void *buffer_head);
int kb_fs_subsystem_sync_dirty_buffer(void *buffer_head);
int kb_fs_subsystem_flush_dirty_buffers(void);
int kb_fs_subsystem_issue_flush(void *bdev);
int kb_fs_subsystem_issue_discard(
    void *bdev,
    uint64_t sector,
    uint64_t sector_count,
    unsigned int gfp);
int kb_fs_subsystem_issue_zeroout(
    void *bdev,
    uint64_t sector,
    uint64_t sector_count,
    unsigned int gfp,
    unsigned int flags);
int kb_fs_subsystem_sync_blockdev(void *bdev);
int kb_fs_subsystem_sync_filesystem(void *super_block);
void kb_fs_subsystem_kill_block_super(void *super_block);
int kb_fs_subsystem_sync_inode_metadata(void *inode, int wait);
int kb_fs_subsystem_sync_mapping_buffers(void *mapping);
void kb_fs_subsystem_try_to_writeback_inodes_sb(void *super_block, unsigned int reason);
void *kb_fs_subsystem_create_empty_buffers(void *folio, unsigned long block_size, unsigned long state);
void kb_fs_subsystem_folio_put(void *folio);
int kb_fs_subsystem_filemap_dirty_folio(void *mapping, void *folio);
int kb_fs_subsystem_block_dirty_folio(void *mapping, void *folio);
int kb_fs_subsystem_folio_mark_dirty(void *folio);
int kb_fs_subsystem_folio_clear_dirty_for_io(void *folio);
int kb_fs_subsystem_folio_redirty_for_writepage(void *writeback_control, void *folio);
void kb_fs_subsystem_folio_start_writeback(void *folio, int keep_write);
void kb_fs_subsystem_folio_end_writeback(void *folio);
void kb_fs_subsystem_folio_wait_writeback(void *folio);
void kb_fs_subsystem_folio_wait_stable(void *folio);
void kb_fs_subsystem_folio_zero_new_buffers(void *folio, size_t from, size_t to);
void kb_fs_subsystem_tag_pages_for_writeback(void *mapping, unsigned long start, unsigned long end);
unsigned int kb_fs_subsystem_filemap_get_folios(
    void *mapping,
    unsigned long *start,
    unsigned long end,
    void *folio_batch);
unsigned int kb_fs_subsystem_filemap_get_folios_tag(
    void *mapping,
    unsigned long *start,
    unsigned long end,
    unsigned int tag,
    void *folio_batch);
void kb_fs_subsystem_folio_batch_release(void *folio_batch);
int kb_fs_subsystem_xa_insert(
    void *xarray,
    unsigned long index,
    void *entry,
    unsigned int gfp);
void *kb_fs_subsystem_xa_store(
    void *xarray,
    unsigned long index,
    void *entry,
    unsigned int gfp);
void *kb_fs_subsystem_xa_load(void *xarray, unsigned long index);
void *kb_fs_subsystem_xa_erase(void *xarray, unsigned long index);
void *kb_fs_subsystem_xa_find(
    void *xarray,
    unsigned long *index,
    unsigned long max,
    unsigned int filter);
void *kb_fs_subsystem_xa_find_after(
    void *xarray,
    unsigned long *index,
    unsigned long max,
    unsigned int filter);
void kb_fs_subsystem_xa_destroy(void *xarray);
int kb_fs_subsystem_filemap_write_and_wait_range(void *mapping, int64_t start, int64_t end);
int kb_fs_subsystem_filemap_flush(void *mapping);
int kb_fs_subsystem_filemap_wait_range(void *mapping, int64_t start, int64_t end);
int kb_fs_subsystem_file_write_and_wait_range(void *file, int64_t start, int64_t end);
int kb_fs_subsystem_file_update_time(void *file);
int kb_fs_subsystem_file_modified(void *file);
void kb_fs_subsystem_touch_atime(void *path);
uint32_t kb_fs_subsystem_errseq_set(uint32_t *sequence, int error);
uint32_t kb_fs_subsystem_errseq_sample(uint32_t *sequence);
int kb_fs_subsystem_errseq_check(uint32_t *sequence, uint32_t since);
int kb_fs_subsystem_errseq_check_and_advance(uint32_t *sequence, uint32_t *since);
void kb_fs_subsystem_filemap_set_wb_err(void *mapping, int error);
int kb_fs_subsystem_file_check_and_advance_wb_err(void *file);
int kb_fs_subsystem_vfs_fsync_range(
    void *file,
    int64_t start,
    int64_t end,
    int datasync);
int kb_fs_subsystem_sync_super(void *super_block, int wait);
int kb_fs_subsystem_generic_buffers_fsync_noflush(
    void *file,
    int64_t start,
    int64_t end,
    int datasync);
void kb_fs_subsystem_invalidate_inode_folios(void *inode);
void kb_fs_subsystem_truncate_inode_pages(void *mapping, int64_t start);
void kb_fs_subsystem_truncate_inode_pages_range(void *mapping, int64_t start, int64_t end);
void kb_fs_subsystem_truncate_inode_pages_final(void *mapping);
unsigned long kb_fs_subsystem_invalidate_mapping_pages(
    void *mapping,
    unsigned long start,
    unsigned long end);
void kb_fs_subsystem_invalidate_bdev(void *bdev);
void kb_fs_subsystem_truncate_pagecache(void *inode, int64_t new_size);
void kb_fs_subsystem_truncate_pagecache_range(void *inode, int64_t start, int64_t end);
void kb_fs_subsystem_pagecache_isize_extended(void *inode, int64_t from, int64_t to);
void kb_fs_subsystem_block_commit_write(void *page, unsigned int from, unsigned int to);
void kb_fs_subsystem_block_invalidate_folio(void *folio, size_t offset, size_t length);
int kb_fs_subsystem_block_is_partially_uptodate(void *folio, size_t from, size_t count);
int kb_fs_subsystem_block_read_full_folio(
    void *folio,
    int (*get_block)(void *, uint64_t, void *, int));
int kb_fs_subsystem_try_to_free_buffers(void *folio);
int kb_fs_subsystem_filemap_release_folio(void *folio, unsigned int gfp);
int kb_fs_subsystem_folio_mkclean(void *folio);
int kb_fs_subsystem_block_page_mkwrite(
    void *vma,
    void *vm_fault,
    int (*get_block)(void *, uint64_t, void *, int));
unsigned int kb_fs_subsystem_filemap_fault(void *vm_fault);
unsigned int kb_fs_subsystem_filemap_map_pages(
    void *vm_fault,
    unsigned long start_index,
    unsigned long end_index);
int kb_fs_subsystem_write_cache_pages(
    void *mapping,
    void *writeback_control,
    int (*writepage)(void *, void *, void *),
    void *data);
int kb_fs_subsystem_block_write_end(
    void *file,
    void *mapping,
    int64_t pos,
    unsigned int len,
    unsigned int copied,
    void *page,
    void *fsdata);
void *kb_fs_subsystem_bio_alloc_bioset(void *bdev, unsigned short nr_vecs, unsigned int opf, unsigned int gfp, void *bioset);
int kb_fs_subsystem_bio_add_folio(void *bio, void *folio, size_t len, size_t offset);
int kb_fs_subsystem_bio_add_page(void *bio, void *page, unsigned int len, unsigned int offset);
void kb_fs_subsystem_submit_bio(void *bio);
void kb_fs_subsystem_submit_bio_noacct(void *bio);
void kb_fs_subsystem_submit_bh(unsigned int opf, void *buffer_head);
void kb_fs_subsystem_blk_start_plug(void *plug);
void kb_fs_subsystem_blk_finish_plug(void *plug);
int kb_fs_subsystem_bh_read(void *buffer_head, unsigned int op_flags, int wait);
void kb_fs_subsystem_bh_read_batch(
    int count,
    void **buffer_heads,
    unsigned int op_flags,
    int force_lock);
void kb_fs_subsystem_lock_buffer(void *buffer_head);
void kb_fs_subsystem_unlock_buffer(void *buffer_head);
void kb_fs_subsystem_write_dirty_buffer(void *buffer_head, unsigned int op_flags);
void kb_fs_subsystem_wait_on_buffer(void *buffer_head);
void kb_fs_subsystem_end_buffer_read_sync(void *buffer_head, int uptodate);
void kb_fs_subsystem_end_buffer_write_sync(void *buffer_head, int uptodate);
void kb_fs_subsystem_bio_endio(void *bio);
void kb_fs_subsystem_bio_put(void *bio);
void kb_fs_subsystem_bio_set_auto_drain(int enabled);
size_t kb_fs_subsystem_bio_drain(void);
size_t kb_fs_subsystem_bio_queue_depth(void);
void kb_fs_subsystem_bio_set_sector(void *bio, uint64_t sector);
void kb_fs_subsystem_bio_set_end_io(void *bio, void (*end_io)(void *));
int kb_fs_subsystem_bio_result(void *bio);
int kb_fs_subsystem_bio_snapshot(void *bio, kb_fs_bio_snapshot_t *out_snapshot);
void *kb_fs_subsystem_filemap_get_folio(void *mapping, unsigned long index, unsigned int fgp_flags, unsigned int gfp);
void *kb_fs_subsystem_read_cache_folio(
    void *mapping,
    unsigned long index,
    int (*filler)(void *, void *),
    void *file);
void kb_fs_subsystem_page_cache_ra_unbounded(
    void *readahead_control,
    unsigned long nr_to_read,
    unsigned long lookahead_size);
void kb_fs_subsystem_page_cache_sync_ra(
    void *readahead_control,
    unsigned long requested_count);
void kb_fs_subsystem_folio_end_read(void *folio, int success);
void kb_fs_subsystem_folio_lock(void *folio);
void kb_fs_subsystem_folio_unlock(void *folio);
void kb_fs_subsystem_mark_inode_dirty(void *inode, int flags);
int kb_fs_subsystem_inode_maybe_inc_iversion(void *inode, int force);
uint64_t kb_fs_subsystem_inode_query_iversion(void *inode);
void kb_fs_subsystem_inode_set_flags(
    void *inode,
    unsigned int flags,
    unsigned int mask);
typedef struct kb_fs_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
} kb_fs_timespec64_t;
kb_fs_timespec64_t kb_fs_subsystem_simple_inode_init_ts(void *inode);
int kb_fs_subsystem_inode_needs_sync(void *inode);
int64_t kb_fs_subsystem_generic_file_llseek_size(
    void *file,
    int64_t offset,
    int whence,
    int64_t maxsize,
    int64_t eof);
void kb_fs_subsystem_generic_fillattr(
    void *idmap,
    uint32_t request_mask,
    void *inode,
    void *stat);
void kb_fs_subsystem_fileattr_fill_flags(void *fileattr, uint32_t flags);
int kb_fs_subsystem_fiemap_fill_next_extent(
    void *extent_info,
    uint64_t logical,
    uint64_t physical,
    uint64_t length,
    uint32_t flags);
int kb_fs_subsystem_fiemap_prep(
    void *inode,
    void *extent_info,
    uint64_t start,
    uint64_t *length,
    uint32_t supported_flags);
int kb_fs_subsystem_iomap_fiemap(
    void *inode,
    void *extent_info,
    uint64_t start,
    uint64_t length,
    const void *iomap_ops);
int64_t kb_fs_subsystem_mapping_seek_hole_data(
    void *mapping,
    int64_t start,
    int64_t end,
    int whence);
int64_t kb_fs_subsystem_iomap_seek_data(
    void *inode,
    int64_t position,
    const void *iomap_ops);
int64_t kb_fs_subsystem_iomap_seek_hole(
    void *inode,
    int64_t position,
    const void *iomap_ops);
long kb_fs_subsystem_iomap_dio_rw(
    void *kiocb,
    void *iter,
    const void *iomap_ops,
    const void *dio_ops,
    unsigned int dio_flags,
    void *private_data,
    size_t done_before);
void kb_fs_subsystem_inode_dio_wait(void *inode);
int kb_fs_subsystem_generic_error_remove_folio(void *mapping, void *folio);
void kb_fs_subsystem_lock_two_nondirectories(void *inode1, void *inode2);
void kb_fs_subsystem_unlock_two_nondirectories(void *inode1, void *inode2);
void *kb_fs_subsystem_get_inode_acl(void *inode, int type);
void *kb_fs_subsystem_posix_acl_alloc(int count, unsigned int flags);
int kb_fs_subsystem_posix_acl_chmod(void *idmap, void *dentry, unsigned short mode);
int kb_fs_subsystem_posix_acl_create(
    void *dir,
    unsigned short *mode,
    void **default_acl,
    void **acl);
int kb_fs_subsystem_posix_acl_update_mode(
    void *idmap,
    void *inode,
    unsigned short *mode,
    void **acl);
void kb_fs_subsystem_set_cached_acl(void *inode, int type, void *acl);
void kb_fs_subsystem_posix_acl_release(void *acl);
void *kb_fs_subsystem_iget_locked(void *super_block, unsigned long inode_number);
void kb_fs_subsystem_ihold(void *inode);
void *kb_fs_subsystem_igrab(void *inode);
void kb_fs_subsystem_iput(void *inode);
void *kb_fs_subsystem_new_inode(void *super_block);
int kb_fs_subsystem_insert_inode_locked(void *inode);
void kb_fs_subsystem_inode_init_once(void *inode);
void kb_fs_subsystem_free_fake_inode(void *inode);
int kb_fs_subsystem_inode_init_owner(void *idmap, void *inode, void *dir, unsigned short mode);
void kb_fs_subsystem_init_special_inode(void *inode, unsigned int mode, unsigned int rdev);
void *kb_fs_subsystem_d_make_root(void *inode);
void *kb_fs_subsystem_d_splice_alias(void *inode, void *dentry);
void *kb_fs_subsystem_d_alloc(void *parent, const void *name);
void *kb_fs_subsystem_d_alloc_name(void *parent, const char *name);
void kb_fs_subsystem_d_drop(void *dentry);
void *kb_fs_subsystem_d_find_any_alias(void *inode);
void *kb_fs_subsystem_dget_parent(void *dentry);
void kb_fs_subsystem_d_mark_dontcache(void *dentry);
void *kb_fs_subsystem_d_obtain_alias(void *inode);
int kb_fs_subsystem_generic_encode_ino32_fh(
    void *inode,
    uint32_t *file_handle,
    int *max_length,
    void *parent);
void *kb_fs_subsystem_generic_fh_to_dentry(
    void *super_block,
    const uint32_t *file_handle,
    int handle_length,
    int handle_type,
    void *get_inode_operation);
void *kb_fs_subsystem_generic_fh_to_parent(
    void *super_block,
    const uint32_t *file_handle,
    int handle_length,
    int handle_type,
    void *get_inode_operation);
void *kb_fs_subsystem_find_inode_by_ino_rcu(
    void *super_block,
    unsigned long inode_number);
long kb_fs_subsystem_generic_read_dir(
    void *file,
    char *buffer,
    size_t size,
    int64_t *position);
void kb_fs_subsystem_path_get(void *path);
void kb_fs_subsystem_path_put(void *path);
int kb_fs_subsystem_finish_open(
    void *file,
    void *dentry,
    void *open_operation);
enum {
    KB_FS_FILE_ACCESS_READ = 1u << 0,
    KB_FS_FILE_ACCESS_WRITE = 1u << 1,
};
int kb_fs_subsystem_file_open(
    void *vfsmount,
    void *dentry,
    unsigned int access,
    void **out_file);
int kb_fs_subsystem_file_close(void *file);
void kb_fs_subsystem_d_tmpfile(void *file, void *inode);
void kb_fs_subsystem_dput(void *dentry);
void kb_fs_subsystem_d_add(void *dentry, void *inode);
void kb_fs_subsystem_d_instantiate(void *dentry, void *inode);
void kb_fs_subsystem_d_instantiate_new(void *dentry, void *inode);
void kb_fs_subsystem_iget_failed(void *inode);
void kb_fs_subsystem_make_bad_inode(void *inode);
int kb_fs_subsystem_is_bad_inode(void *inode);
void *kb_fs_subsystem_mount_nodev(void *fs_type, int flags, void *data, int (*fill_super)(void *, void *, int));
int kb_fs_subsystem_path_pts(void *path);
int kb_fs_subsystem_path_devpts_index(void *path, unsigned index);
void kb_fs_subsystem_unlock_new_inode(void *inode);
void kb_fs_subsystem_set_nlink(void *inode, unsigned int nlink);
void kb_fs_subsystem_clear_nlink(void *inode);
void kb_fs_subsystem_drop_nlink(void *inode);
void kb_fs_subsystem_inc_nlink(void *inode);
void kb_fs_subsystem_mark_inode_freeing(void *inode);
void kb_fs_subsystem_clear_inode(void *inode);
int kb_fs_subsystem_dquot_alloc_space(void *inode, uint64_t bytes, int flags);
void kb_fs_subsystem_dquot_free_space(void *inode, uint64_t bytes, int flags);
int kb_fs_subsystem_fscrypt_match_name(const void *fname, const void *de_name, unsigned int de_name_len);
int kb_fs_subsystem_fscrypt_setup_filename(void *dir, const void *qstr, int lookup, void *fname);
int kb_fs_subsystem_fscrypt_prepare_symlink(
    void *dir,
    const char *target,
    unsigned int target_length,
    unsigned int max_length,
    void *disk_link);
long kb_fs_subsystem_generic_file_read_iter(void *kiocb, void *iter);
long kb_fs_subsystem_generic_write_checks(void *kiocb, void *iter);
long kb_fs_subsystem_generic_perform_write(void *kiocb, void *iter);
size_t kb_fs_subsystem_copy_to_iter(const void *addr, size_t bytes, void *iter);
size_t kb_fs_subsystem_copy_from_iter(void *addr, size_t bytes, void *iter);
unsigned long kb_fs_subsystem_iov_iter_alignment(const void *iter);
void kb_fs_subsystem_iov_iter_revert(void *iter, size_t bytes);
int kb_fs_subsystem_bmap(void *inode, uint64_t *block);
uint64_t kb_fs_subsystem_iomap_bmap(void *mapping, uint64_t block, const void *ops);
int kb_fs_subsystem_sb_min_blocksize(void *super_block, int size);
int kb_fs_subsystem_sb_set_blocksize(void *super_block, int size);
int kb_fs_subsystem_set_blocksize(void *bdev, int size);
int kb_fs_subsystem_generic_check_addressable(
    unsigned int blocksize_bits,
    uint64_t num_blocks);
int kb_fs_subsystem_inode_newsize_ok(void *inode, int64_t size);
int kb_fs_subsystem_mount_registered_root(const char *name, kb_fs_mount_result_t *out_mount);
int kb_fs_subsystem_probe_registered_mount_path(const char *name, kb_fs_mount_path_probe_t *out_probe);
int kb_fs_subsystem_run_ext4_image_smoke(
    const char *image_path,
    unsigned long inode_number,
    unsigned long large_inode_number,
    unsigned long ldlike_inode_number,
    unsigned long zero_inode_number);
size_t kb_fs_subsystem_registered_type_count(void);
int kb_fs_subsystem_type_snapshot(const char *name, kb_fs_type_snapshot_t *out_snapshot);
int kb_fs_subsystem_mount_snapshot(uint64_t handle, kb_fs_mount_snapshot_t *out_snapshot);
