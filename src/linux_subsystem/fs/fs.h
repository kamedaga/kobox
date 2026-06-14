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

extern unsigned char kb_fs_subsystem_blockdev_superblock[];

typedef struct kb_fs_block_device kb_fs_block_device_t;

typedef int (*kb_fs_block_read_fn)(void *ctx, uint64_t offset, void *buffer, size_t size);
typedef int (*kb_fs_block_write_fn)(void *ctx, uint64_t offset, const void *buffer, size_t size);
typedef void (*kb_fs_block_destroy_fn)(void *ctx);

typedef struct kb_fs_block_device_desc {
    const char *name;
    uint64_t size_bytes;
    uint32_t logical_block_size;
    void *ctx;
    kb_fs_block_read_fn read;
    kb_fs_block_write_fn write;
    kb_fs_block_destroy_fn destroy;
} kb_fs_block_device_desc_t;

typedef enum kb_fs_bio_operation {
    KB_FS_BIO_OP_READ = 0,
    KB_FS_BIO_OP_WRITE = 1,
    KB_FS_BIO_OP_FLUSH = 2,
    KB_FS_BIO_OP_DISCARD = 3,
} kb_fs_bio_operation_t;

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

kb_status_t kb_fs_subsystem_bind_ipc_interface(kb_platform_t *platform, kb_interface_t **out_interface);
kb_status_t kb_fs_subsystem_dispatch(kb_interface_t *interface, kb_fs_ipc_request_t *request);
kb_status_t kb_fs_subsystem_mount(kb_interface_t *interface, const kb_fs_mount_desc_t *desc, uint64_t *out_handle);
kb_status_t kb_fs_subsystem_read(kb_interface_t *interface, const kb_fs_read_desc_t *desc);
kb_status_t kb_fs_subsystem_write(kb_interface_t *interface, const kb_fs_write_desc_t *desc);
kb_status_t kb_fs_subsystem_readdir(kb_interface_t *interface, const kb_fs_readdir_desc_t *desc);
int kb_fs_subsystem_register_filesystem(void *fs_type);
int kb_fs_subsystem_unregister_filesystem(void *fs_type);
int kb_fs_block_device_create(const kb_fs_block_device_desc_t *desc, kb_fs_block_device_t **out_device);
int kb_fs_block_device_create_image(const char *name, const char *image_path, kb_fs_block_device_t **out_device);
int kb_fs_block_device_create_from_disk(const char *name, void *disk, kb_fs_block_device_t **out_device);
int kb_fs_block_device_read(kb_fs_block_device_t *device, uint64_t offset, void *buffer, size_t size);
int kb_fs_block_device_write(kb_fs_block_device_t *device, uint64_t offset, const void *buffer, size_t size);
void kb_fs_block_device_destroy(kb_fs_block_device_t *device);
int kb_fs_subsystem_set_mount_probe_block_device(kb_fs_block_device_t *device);
int kb_fs_subsystem_get_tree_bdev(void *fs_context, int (*fill_super)(void *super_block, void *fs_context));
void *kb_fs_subsystem_bdev_getblk(void *bdev, uint64_t block_number, unsigned int block_size, unsigned int gfp);
void kb_fs_subsystem_buffer_head_put(void *buffer_head);
void kb_fs_subsystem_mark_buffer_dirty(void *buffer_head);
int kb_fs_subsystem_sync_dirty_buffer(void *buffer_head);
void *kb_fs_subsystem_bio_alloc_bioset(void *bdev, unsigned short nr_vecs, unsigned int opf, unsigned int gfp, void *bioset);
int kb_fs_subsystem_bio_add_folio(void *bio, void *folio, size_t len, size_t offset);
int kb_fs_subsystem_bio_add_page(void *bio, void *page, unsigned int len, unsigned int offset);
void kb_fs_subsystem_submit_bio(void *bio);
void kb_fs_subsystem_submit_bio_noacct(void *bio);
void kb_fs_subsystem_bio_endio(void *bio);
void kb_fs_subsystem_bio_put(void *bio);
void kb_fs_subsystem_bio_set_auto_drain(int enabled);
size_t kb_fs_subsystem_bio_drain(void);
size_t kb_fs_subsystem_bio_queue_depth(void);
void kb_fs_subsystem_bio_set_sector(void *bio, uint64_t sector);
void kb_fs_subsystem_bio_set_end_io(void *bio, void (*end_io)(void *));
int kb_fs_subsystem_bio_result(void *bio);
int kb_fs_subsystem_bio_snapshot(void *bio, kb_fs_bio_snapshot_t *out_snapshot);
void *kb_fs_subsystem_iget_locked(void *super_block, unsigned long inode_number);
void *kb_fs_subsystem_new_inode(void *super_block);
int kb_fs_subsystem_inode_init_owner(void *idmap, void *inode, void *dir, unsigned short mode);
void *kb_fs_subsystem_d_make_root(void *inode);
void *kb_fs_subsystem_d_splice_alias(void *inode, void *dentry);
void kb_fs_subsystem_d_instantiate(void *dentry, void *inode);
void kb_fs_subsystem_d_instantiate_new(void *dentry, void *inode);
void kb_fs_subsystem_iget_failed(void *inode);
void kb_fs_subsystem_unlock_new_inode(void *inode);
void kb_fs_subsystem_set_nlink(void *inode, unsigned int nlink);
int kb_fs_subsystem_fscrypt_match_name(const void *fname, const void *de_name, unsigned int de_name_len);
int kb_fs_subsystem_fscrypt_setup_filename(void *dir, const void *qstr, int lookup, void *fname);
long kb_fs_subsystem_generic_file_read_iter(void *kiocb, void *iter);
long kb_fs_subsystem_generic_write_checks(void *kiocb, void *iter);
long kb_fs_subsystem_generic_perform_write(void *kiocb, void *iter);
int kb_fs_subsystem_bmap(void *inode, uint64_t *block);
int kb_fs_subsystem_sb_min_blocksize(void *super_block, int size);
int kb_fs_subsystem_sb_set_blocksize(void *super_block, int size);
int kb_fs_subsystem_probe_registered_mount_path(const char *name, kb_fs_mount_path_probe_t *out_probe);
size_t kb_fs_subsystem_registered_type_count(void);
int kb_fs_subsystem_type_snapshot(const char *name, kb_fs_type_snapshot_t *out_snapshot);
int kb_fs_subsystem_mount_snapshot(uint64_t handle, kb_fs_mount_snapshot_t *out_snapshot);
