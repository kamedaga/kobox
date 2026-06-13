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
void kb_fs_block_device_destroy(kb_fs_block_device_t *device);
int kb_fs_subsystem_set_mount_probe_block_device(kb_fs_block_device_t *device);
int kb_fs_subsystem_get_tree_bdev(void *fs_context, int (*fill_super)(void *super_block, void *fs_context));
void *kb_fs_subsystem_bdev_getblk(void *bdev, uint64_t block_number, unsigned int block_size, unsigned int gfp);
void *kb_fs_subsystem_iget_locked(void *super_block, unsigned long inode_number);
void *kb_fs_subsystem_new_inode(void *super_block);
int kb_fs_subsystem_sb_min_blocksize(void *super_block, int size);
int kb_fs_subsystem_sb_set_blocksize(void *super_block, int size);
int kb_fs_subsystem_probe_registered_mount_path(const char *name, kb_fs_mount_path_probe_t *out_probe);
size_t kb_fs_subsystem_registered_type_count(void);
int kb_fs_subsystem_type_snapshot(const char *name, kb_fs_type_snapshot_t *out_snapshot);
int kb_fs_subsystem_mount_snapshot(uint64_t handle, kb_fs_mount_snapshot_t *out_snapshot);
