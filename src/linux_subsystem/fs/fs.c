#include "linux_subsystem/fs/fs.h"
#include "kobox/shim.h"
#include "loader/module_context.h"
#include "loader/symbol_registry.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_FS_IPC_REQUEST_VERSION = 1,
    KB_FS_TYPE_MAX = 64,
    KB_FS_MOUNT_MAX = 64,
    KB_FS_FILE_MAX = 256,
    KB_FS_FILEMAP_FOLIO_CACHE_MAX = 512,
    KB_FS_CONTEXT_BYTES = 512,
    KB_FS_SUPER_BLOCK_BYTES = 2048,
    KB_FS_FAKE_INODE_HEADROOM_BYTES = 512,
    KB_FS_FAKE_INODE_BYTES = 1024,
    KB_FS_FAKE_INODE_MAPPING_BYTES = 256,
    KB_FS_FAKE_BDEV_BYTES = 256,
    KB_FS_FAKE_BDEV_INODE_BYTES = 512,
    KB_FS_FAKE_ADDRESS_SPACE_BYTES = 128,
    KB_FS_FAKE_DISK_BYTES = 256,
    KB_FS_FAKE_BDEV_STATS_BYTES = 128,
    KB_FS_FAKE_QUEUE_BYTES = 128,
    KB_FS_FAKE_BUFFER_HEAD_BYTES = 128,
    KB_FS_FAKE_FOLIO_BYTES = 128,
    KB_FS_FAKE_DENTRY_BYTES = 512,
    KB_FS_BUFFER_CACHE_MAX = 512,
    KB_FS_TYPE_INIT_FS_CONTEXT_OFFSET = 16,
    KB_FS_CONTEXT_OPS_OFFSET = 0,
    KB_FS_CONTEXT_OPS_GET_TREE_OFFSET = 32,
    KB_FS_SUPER_BLOCK_BLOCKSIZE_BITS_OFFSET = 0x14,
    KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET = 0x18,
    KB_FS_SUPER_BLOCK_BDEV_OFFSET = 0xe0,
    KB_FS_SUPER_BLOCK_FS_INFO_OFFSET = 0x380,
    KB_FS_SUPER_BLOCK_DEVNAME_OFFSET = 0x3b0,
    KB_FS_BDEV_SECTOR_COUNT_OFFSET = 0x8,
    KB_FS_BDEV_INODE_OFFSET = 0x10,
    KB_FS_BDEV_DISK_OFFSET = 0x18,
    KB_FS_BDEV_STATS_OFFSET = 0x20,
    KB_FS_BDEV_QUEUE_OFFSET = 0x38,
    KB_FS_ADDRESS_SPACE_HOST_OFFSET = 0x0,
    KB_FS_ADDRESS_SPACE_AOPS_OFFSET = 0x68,
    KB_FS_ADDRESS_SPACE_OFFSET = 0x40,
    KB_FS_ADDRESS_SPACE_OP_WRITE_BEGIN_OFFSET = 0x28,
    KB_FS_ADDRESS_SPACE_OP_WRITE_END_OFFSET = 0x30,
    KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET = 0x8,
    KB_FS_BUFFER_HEAD_FOLIO_OFFSET = 0x10,
    KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET = 0x18,
    KB_FS_BUFFER_HEAD_SIZE_OFFSET = 0x20,
    KB_FS_BUFFER_HEAD_DATA_OFFSET = 0x28,
    KB_FS_BUFFER_HEAD_BDEV_OFFSET = 0x30,
    KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET = 0x60,
    KB_FS_FOLIO_MAPPING_OFFSET = 0x18,
    KB_FS_FOLIO_INDEX_OFFSET = 0x20,
    KB_FS_FOLIO_PRIVATE_OFFSET = 0x28,
    KB_FS_FOLIO_REFCOUNT_OFFSET = 0x34,
    KB_FS_FOLIO_FLAG_LOCKED = 0x1,
    KB_FS_PAGE_SIZE = 4096,
    KB_FS_KVM_STRUCT_PAGE_SIZE = 64,
    KB_FS_INODE_MODE_OFFSET = 0x0,
    KB_FS_INODE_SB_OFFSET = 0x28,
    KB_FS_INODE_MAPPING_OFFSET = 0x30,
    KB_FS_INODE_NUMBER_OFFSET = 0x40,
    KB_FS_INODE_NLINK_OFFSET = 0x48,
    KB_FS_INODE_SIZE_OFFSET = 0x50,
    KB_FS_INODE_BLOCKS_OFFSET = 0x88,
    KB_FS_INODE_STATE_OFFSET = 0x90,
    KB_FS_INODE_BLKBITS_OFFSET = 0x86,
    KB_FS_INODE_STATE_FREEING = 0x1,
    KB_FS_INODE_STATE_NEW = 0x1,
    KB_FS_INODE_RWSEM_OFFSET = 0x98,
    KB_FS_INODE_RWSEM_HELD = 1,
    KB_FS_INODE_MODE_DIRECTORY = 0040000 | 0755,
    KB_FS_INODE_EXT4_DISKSIZE_BACK_OFFSET = 0x30,
    KB_FS_INODE_EXT4_DATA_SEM_BACK_OFFSET = 0x28,
    KB_FS_INODE_EXT4_DIRECT_BLOCK0_BACK_OFFSET = 0x128,
    KB_FS_INODE_EXT4_DIRECT_BLOCKS = 12,
    KB_FS_INODE_EXT4_ES_LRU_OFFSET = 0x2c0,
    KB_FS_INODE_EXT4_ES_SHRINK_COUNT_OFFSET = 0x2d4,
    KB_FS_EXT4_SBI_ES_LRU_OFFSET = 0x520,
    KB_FS_EXT4_SBI_ES_SHRINK_COUNT_OFFSET = 0x530,
    KB_FS_EXT4_SBI_CLUSTER_BITS_OFFSET = 0x54,
    KB_FS_EXT4_SBI_SUPER_BUFFER_HEAD_OFFSET = 0x60,
    KB_FS_EXT4_SBI_EXT4_SUPER_OFFSET = 0x68,
    KB_FS_EXT4_SBI_FREECLUSTERS_COUNTER_OFFSET = 0xe0,
    KB_FS_EXT4_SBI_GROUP_COUNT_OFFSET = 0x40,
    KB_FS_EXT4_EXTENT_HEADER_MAGIC = 0xf30a,
    KB_FS_EXT4_GROUP_INFO_BB_FREE_OFFSET = 0x14,
    KB_FS_EXT4_GROUP_INFO_GROUP_OFFSET = 0x24,
    KB_FS_EXT4_GROUP_INFO_PREALLOC_LIST_OFFSET = 0x28,
    KB_FS_DENTRY_PARENT_OFFSET = 0x18,
    KB_FS_DENTRY_QSTR_OFFSET = 0x20,
    KB_FS_DENTRY_INODE_OFFSET = 0x30,
    KB_FS_DENTRY_INODE_COMPAT_OFFSET = 0x30,
    KB_FS_DENTRY_SB_OFFSET = 0x68,
    KB_FS_QSTR_HASH_LEN_OFFSET = 0x0,
    KB_FS_QSTR_NAME_OFFSET = 0x8,
    KB_FS_FILE_MAPPING_OFFSET = 0x20,
    KB_FS_FILE_INODE_OFFSET = 0x28,
    KB_FS_KIOCB_FILE_OFFSET = 0x0,
    KB_FS_KIOCB_POS_OFFSET = 0x8,
    KB_FS_IOV_ITER_COUNT_OFFSET = 0x18,
    KB_FS_IOV_ITER_BUFFER_OFFSET = 0x20,
    KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET = 0x78,
    KB_FS_INODE_OP_OFFSET = 0x20,
    KB_FS_INODE_OP_CREATE_OFFSET = 0x28,
    KB_FS_INODE_OP_UNLINK_OFFSET = 0x38,
    KB_FS_INODE_OP_MKDIR_OFFSET = 0x48,
    KB_FS_INODE_OP_RMDIR_OFFSET = 0x50,
    KB_FS_INODE_OP_RENAME_OFFSET = 0x60,
    KB_FS_WRITEBACK_CONTROL_BYTES = 384,
    KB_FS_WRITEBACK_CONTROL_SYNC_MODE_OFFSET = 0x20,
    KB_FS_WRITEBACK_CONTROL_WB_SYNC_ALL = 1,
    KB_FS_EXT4_SUPER_OFFSET = 1024,
    KB_FS_EXT4_SUPER_INODES_COUNT_OFFSET = 0x0,
    KB_FS_EXT4_SUPER_BLOCKS_COUNT_LO_OFFSET = 0x4,
    KB_FS_EXT4_SUPER_FREE_BLOCKS_COUNT_LO_OFFSET = 0xc,
    KB_FS_EXT4_SUPER_FREE_INODES_COUNT_OFFSET = 0x10,
    KB_FS_EXT4_SUPER_FIRST_DATA_BLOCK_OFFSET = 0x14,
    KB_FS_EXT4_SUPER_LOG_BLOCK_SIZE_OFFSET = 0x18,
    KB_FS_EXT4_SUPER_BLOCKS_PER_GROUP_OFFSET = 0x20,
    KB_FS_EXT4_SUPER_INODES_PER_GROUP_OFFSET = 0x28,
    KB_FS_EXT4_SUPER_MAGIC_OFFSET = 0x38,
    KB_FS_EXT4_SUPER_INODE_SIZE_OFFSET = 0x58,
    KB_FS_EXT4_SUPER_DESC_SIZE_OFFSET = 0xfe,
    KB_FS_EXT4_SUPER_FREE_BLOCKS_COUNT_HI_OFFSET = 0x158,
    KB_FS_EXT4_GROUP_DESC_BLOCK_BITMAP_LO_OFFSET = 0x0,
    KB_FS_EXT4_GROUP_DESC_INODE_BITMAP_LO_OFFSET = 0x4,
    KB_FS_EXT4_GROUP_DESC_INODE_TABLE_LO_OFFSET = 0x8,
    KB_FS_EXT4_GROUP_DESC_FREE_BLOCKS_COUNT_LO_OFFSET = 0xc,
    KB_FS_EXT4_GROUP_DESC_FREE_INODES_COUNT_LO_OFFSET = 0xe,
    KB_FS_EXT4_DISK_INODE_DTIME_OFFSET = 0x14,
    KB_FS_EXT4_EXTENT_HEADER_ENTRIES_OFFSET = 0x2,
    KB_FS_EXT4_EXTENT_HEADER_MAX_OFFSET = 0x4,
    KB_FS_EXT4_EXTENT_HEADER_DEPTH_OFFSET = 0x6,
    KB_FS_EXT4_EXTENT_BYTES = 12,
    KB_FS_EXT4_EXTENT_BLOCK_OFFSET = 0x0,
    KB_FS_EXT4_EXTENT_LEN_OFFSET = 0x4,
    KB_FS_EXT4_EXTENT_START_HI_OFFSET = 0x6,
    KB_FS_EXT4_EXTENT_START_LO_OFFSET = 0x8,
    KB_FS_MODE_REGULAR_0644 = 0100000 | 0644,
    KB_FS_MODE_DIRECTORY_0755 = 0040000 | 0755,
    KB_FS_BIO_MAGIC = 0x6b62696f,
    KB_FS_BIO_OP_MASK = 0xff,
    KB_FS_FGP_LOCK = 0x2,
    KB_FS_BH_UPTODATE = 1ull << 0,
    KB_FS_BH_DIRTY = 1ull << 1,
    KB_FS_BH_MAPPED = 1ull << 4,
    KB_FS_BH_NEW = 1ull << 5,
};

typedef struct kb_fs_type_record {
    int active;
    void *fs_type;
    char *name;
    kb_module_t *owner_module;
    uint32_t register_count;
} kb_fs_type_record_t;

typedef struct kb_fs_mount_record {
    int active;
    uint64_t handle;
    kb_fs_type_record_t *type;
    char *source;
    char *target;
    void *block_disk;
} kb_fs_mount_record_t;

typedef struct kb_fs_file_record {
    int active;
    uint64_t mount_handle;
    char *path;
    uint8_t *data;
    size_t size;
} kb_fs_file_record_t;

struct kb_fs_block_device {
    char *name;
    uint64_t size_bytes;
    uint32_t logical_block_size;
    void *ctx;
    kb_fs_block_read_fn read;
    kb_fs_block_write_fn write;
    kb_fs_block_destroy_fn destroy;
};

typedef struct kb_fs_image_block_ctx {
    char *path;
} kb_fs_image_block_ctx_t;

typedef struct kb_fs_block_disk_ctx {
    void *disk;
    uint64_t start_sector;
    uint64_t sector_count;
} kb_fs_block_disk_ctx_t;

typedef struct kb_fs_bdev_binding {
    void *bdev;
    kb_fs_block_device_t *device;
} kb_fs_bdev_binding_t;

typedef struct kb_fs_bio_record {
    uint32_t magic;
    uint32_t opf;
    void *bdev;
    uint64_t sector;
    void *buffer;
    size_t len;
    size_t page_offset;
    int result;
    uint32_t submitted;
    uint32_t queued;
    uint32_t completed;
    void (*end_io)(void *);
    struct kb_fs_bio_record *next;
} kb_fs_bio_record_t;

typedef struct kb_fs_buffer_cache_record {
    int active;
    void *bdev;
    uint64_t block_number;
    uint64_t block_size;
    void *buffer_head;
    void *folio;
    void *data;
} kb_fs_buffer_cache_record_t;

typedef struct kb_fs_filemap_folio_record {
    int active;
    void *mapping;
    unsigned long index;
    void *folio;
} kb_fs_filemap_folio_record_t;

static kb_fs_type_record_t fs_types[KB_FS_TYPE_MAX];
static kb_fs_mount_record_t fs_mounts[KB_FS_MOUNT_MAX];
static kb_fs_file_record_t fs_files[KB_FS_FILE_MAX];
unsigned char kb_fs_subsystem_blockdev_superblock[KB_FS_SUPER_BLOCK_BYTES];
static uint64_t next_mount_handle = 1;
static kb_fs_mount_path_probe_t last_mount_path_probe;
static kb_fs_block_device_t *mount_probe_block_device;
static kb_fs_bdev_binding_t active_bdev_binding;
static void *mount_probe_super_block;
static void *mount_probe_bdev;
static void *mount_probe_bdev_inode;
static void *mount_probe_bdev_mapping;
static void *mount_probe_disk;
static void *mount_probe_bdev_stats;
static void *mount_probe_queue;
static kb_fs_bio_record_t *bio_queue_head;
static kb_fs_bio_record_t *bio_queue_tail;
static size_t bio_queue_depth;
static int bio_auto_drain = 1;
static kb_fs_buffer_cache_record_t buffer_cache[KB_FS_BUFFER_CACHE_MAX];
static kb_fs_filemap_folio_record_t filemap_folio_cache[KB_FS_FILEMAP_FOLIO_CACHE_MAX];

static uint16_t read_le16_fs(const uint8_t *bytes);
static uint32_t read_le32_fs(const uint8_t *bytes);
static void write_le16_fs(uint8_t *bytes, uint16_t value);
static void write_le32_fs(uint8_t *bytes, uint32_t value);
static int active_bdev_read_exact(uint64_t offset, void *buffer, size_t size);
static int active_bdev_write_exact(uint64_t offset, const void *buffer, size_t size);

static void update_buffer_cache_from_write(uint64_t offset, const void *buffer, size_t size)
{
    if (buffer == NULL || size == 0) {
        return;
    }
    uint64_t write_end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &write_end)) {
        return;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (!buffer_cache[i].active || buffer_cache[i].data == NULL || buffer_cache[i].block_size == 0) {
            continue;
        }
        uint64_t block_start = 0;
        uint64_t block_end = 0;
        if (__builtin_mul_overflow(buffer_cache[i].block_number, buffer_cache[i].block_size, &block_start) ||
            __builtin_add_overflow(block_start, buffer_cache[i].block_size, &block_end))
        {
            continue;
        }
        if (write_end <= block_start || offset >= block_end) {
            continue;
        }
        uint64_t copy_start = offset > block_start ? offset : block_start;
        uint64_t copy_end = write_end < block_end ? write_end : block_end;
        memcpy(
            (uint8_t *)buffer_cache[i].data + (copy_start - block_start),
            (const uint8_t *)buffer + (copy_start - offset),
            (size_t)(copy_end - copy_start));
    }
}

static void clear_mount_probe_objects(void)
{
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (!buffer_cache[i].active) {
            continue;
        }
        kb_kfree(buffer_cache[i].buffer_head);
        kb_kfree(buffer_cache[i].folio);
        kb_kfree(buffer_cache[i].data);
    }
    memset(buffer_cache, 0, sizeof(buffer_cache));
    memset(filemap_folio_cache, 0, sizeof(filemap_folio_cache));
    free(mount_probe_super_block);
    free(mount_probe_bdev);
    free(mount_probe_bdev_inode);
    free(mount_probe_bdev_mapping);
    free(mount_probe_disk);
    free(mount_probe_bdev_stats);
    free(mount_probe_queue);
    mount_probe_super_block = NULL;
    mount_probe_bdev = NULL;
    mount_probe_bdev_inode = NULL;
    mount_probe_bdev_mapping = NULL;
    mount_probe_disk = NULL;
    mount_probe_bdev_stats = NULL;
    mount_probe_queue = NULL;
    memset(&active_bdev_binding, 0, sizeof(active_bdev_binding));
}

static int low_or_err_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return ptr == NULL || value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static int fs_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_FS");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void *read_pointer_field(const void *base, size_t offset);
static void write_u64_field(void *base, size_t offset, uint64_t value);
static int kb_fs_enter_ext4_call(void *function, unsigned long *old_gs);
static uint32_t ext4_extent_block_number(const void *inode, uint64_t file_block);

static char *copy_string(const char *value)
{
    if (value == NULL) {
        value = "";
    }
    size_t len = strlen(value);
    char *copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

static const char *fs_type_name_from_kernel_type(void *fs_type)
{
    if (low_or_err_pointer(fs_type)) {
        return NULL;
    }
    const char *name = NULL;
    memcpy(&name, fs_type, sizeof(name));
    return low_or_err_pointer(name) ? NULL : name;
}

static void *read_pointer_field(const void *base, size_t offset)
{
    if (low_or_err_pointer(base)) {
        return NULL;
    }
    void *value = NULL;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return low_or_err_pointer(value) ? NULL : value;
}

static void write_pointer_field(void *base, size_t offset, void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u64_field(void *base, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u8_field(void *base, size_t offset, uint8_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u32_field(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void kb_fs_prepare_dentry_name(void *dentry, const char *name)
{
    if (dentry == NULL || name == NULL) {
        return;
    }
    uint64_t hash_len = ((uint64_t)strlen(name)) << 32;
    write_u64_field((uint8_t *)dentry + KB_FS_DENTRY_QSTR_OFFSET, KB_FS_QSTR_HASH_LEN_OFFSET, hash_len);
    write_pointer_field((uint8_t *)dentry + KB_FS_DENTRY_QSTR_OFFSET, KB_FS_QSTR_NAME_OFFSET, (void *)name);
}

static void kb_fs_prepare_named_dentry(
    void *dentry,
    void *parent,
    void *inode,
    void *super_block,
    const char *name)
{
    if (dentry == NULL) {
        return;
    }
    memset(dentry, 0, KB_FS_FAKE_DENTRY_BYTES);
    write_pointer_field(dentry, KB_FS_DENTRY_PARENT_OFFSET, parent == NULL ? dentry : parent);
    write_pointer_field(dentry, KB_FS_DENTRY_SB_OFFSET, super_block);
    kb_fs_prepare_dentry_name(dentry, name);
    if (!low_or_err_pointer(inode)) {
        write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
    }
}

static int kb_fs_enter_ext4_call(void *function, unsigned long *old_gs)
{
    if (function == NULL || old_gs == NULL) {
        return 0;
    }
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(function);
    return kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, old_gs) == 0;
}

typedef struct kb_fs_ext4_size_sync_ops {
    void (*dirty_inode)(void *, int);
    int (*write_inode)(void *, void *);
    int (*force_commit)(void *);
    int (*truncate_inode)(void *);
} kb_fs_ext4_size_sync_ops_t;

static int kb_fs_ext4_sync_inode_size(
    void *super_block,
    void *inode,
    uint64_t size,
    const kb_fs_ext4_size_sync_ops_t *ops,
    const char *label,
    int run_truncate)
{
    if (super_block == NULL || low_or_err_pointer(inode) || ops == NULL ||
        ops->dirty_inode == NULL || ops->write_inode == NULL || ops->force_commit == NULL)
    {
        return -22;
    }

    uint8_t writeback_control[KB_FS_WRITEBACK_CONTROL_BYTES];
    memset(writeback_control, 0, sizeof(writeback_control));
    write_u32_field(
        writeback_control,
        KB_FS_WRITEBACK_CONTROL_SYNC_MODE_OFFSET,
        KB_FS_WRITEBACK_CONTROL_WB_SYNC_ALL);
    write_u64_field(inode, KB_FS_INODE_SIZE_OFFSET, size);
    write_u64_field((uint8_t *)inode - KB_FS_INODE_EXT4_DISKSIZE_BACK_OFFSET, 0, size);

    int truncate_result = 0;
    if (run_truncate) {
        if (ops->truncate_inode == NULL) {
            return -95;
        }
        uint64_t saved_rwsem = 0;
        uint64_t saved_data_sem = 0;
        memcpy(&saved_rwsem, (const uint8_t *)inode + KB_FS_INODE_RWSEM_OFFSET, sizeof(saved_rwsem));
        memcpy(&saved_data_sem, (const uint8_t *)inode - KB_FS_INODE_EXT4_DATA_SEM_BACK_OFFSET, sizeof(saved_data_sem));
        write_u64_field(inode, KB_FS_INODE_RWSEM_OFFSET, KB_FS_INODE_RWSEM_HELD);
        write_u64_field((uint8_t *)inode - KB_FS_INODE_EXT4_DATA_SEM_BACK_OFFSET, 0, KB_FS_INODE_RWSEM_HELD);
        unsigned long truncate_old_gs = 0;
        int truncate_has_gs = kb_fs_enter_ext4_call((void *)ops->truncate_inode, &truncate_old_gs);
        truncate_result = ops->truncate_inode(inode);
        if (truncate_has_gs) {
            kb_shim_leave_kernel_gs(truncate_old_gs);
        }
        write_u64_field(inode, KB_FS_INODE_RWSEM_OFFSET, saved_rwsem);
        write_u64_field((uint8_t *)inode - KB_FS_INODE_EXT4_DATA_SEM_BACK_OFFSET, 0, saved_data_sem);
    }
    if (run_truncate) {
        uint64_t block_size = 0;
        memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
        if (block_size >= 512 && (block_size % 512u) == 0) {
            uint64_t logical_blocks = size == 0 ? 0 : ((size - 1u) / block_size) + 1u;
            uint64_t allocated_blocks = 0;
            for (uint64_t block = 0; block < logical_blocks; block++) {
                if (ext4_extent_block_number(inode, block) != 0) {
                    allocated_blocks++;
                }
            }
            uint64_t sectors = 0;
            if (!__builtin_mul_overflow(allocated_blocks, block_size / 512u, &sectors)) {
                write_u64_field(inode, KB_FS_INODE_BLOCKS_OFFSET, sectors);
            }
        }
    }

    unsigned long old_gs = 0;
    int has_gs = kb_fs_enter_ext4_call((void *)ops->dirty_inode, &old_gs);
    ops->dirty_inode(inode, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ops->write_inode, &old_gs);
    int write_result = ops->write_inode(inode, writeback_control);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ops->force_commit, &old_gs);
    int commit_result = ops->force_commit(super_block);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    uint64_t observed_size = 0;
    memcpy(&observed_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(observed_size));
    if (truncate_result != 0 || write_result != 0 || commit_result != 0 || observed_size != size) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s failed truncate=%d write=%d commit=%d size=%llu expected=%llu\n",
            label == NULL ? "truncate" : label,
            truncate_result,
            write_result,
            commit_result,
            (unsigned long long)observed_size,
            (unsigned long long)size);
        return truncate_result != 0 ? truncate_result : (write_result != 0 ? write_result : (commit_result != 0 ? commit_result : -5));
    }
    return 0;
}

static int kb_fs_ext4_smoke_write_payload(
    void *inode,
    const void *payload,
    uint64_t payload_len,
    const kb_fs_ext4_size_sync_ops_t *ops,
    const char *label)
{
    if (low_or_err_pointer(inode) || payload == NULL || payload_len == 0 || ops == NULL) {
        return -22;
    }

    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    uint8_t file[KB_FS_FAKE_INODE_BYTES];
    uint8_t kiocb[64];
    uint8_t iter[64];
    memset(file, 0, sizeof(file));
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, payload_len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, (void *)payload);
    long write_result = kb_fs_subsystem_generic_perform_write(kiocb, iter);
    if (write_result != (long)payload_len) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s write failed result=%ld expected=%llu\n",
            label == NULL ? "payload" : label,
            write_result,
            (unsigned long long)payload_len);
        return write_result < 0 ? (int)write_result : -5;
    }
    return kb_fs_ext4_sync_inode_size(super_block, inode, payload_len, ops, label, 0);
}

int kb_fs_subsystem_ext4_sync_group_free_counts(void *super_block)
{
    if (super_block == NULL) {
        return -22;
    }
    typedef void *(*ext4_get_group_desc_fn)(void *, unsigned int, void *);
    typedef void *(*ext4_get_group_info_fn)(void *, unsigned int);
    typedef unsigned int (*ext4_free_group_clusters_fn)(void *, void *);
    ext4_get_group_desc_fn ext4_get_group_desc =
        (ext4_get_group_desc_fn)kb_module_lookup_exported_symbol("ext4_get_group_desc");
    ext4_get_group_info_fn ext4_get_group_info =
        (ext4_get_group_info_fn)kb_module_lookup_exported_symbol("ext4_get_group_info");
    ext4_free_group_clusters_fn ext4_free_group_clusters =
        (ext4_free_group_clusters_fn)kb_module_lookup_exported_symbol("ext4_free_group_clusters");
    if (ext4_get_group_desc == NULL || ext4_get_group_info == NULL || ext4_free_group_clusters == NULL) {
        return -95;
    }

    void *sbi = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_FS_INFO_OFFSET);
    uint32_t group_count = 0;
    if (sbi != NULL) {
        memcpy(&group_count, (const uint8_t *)sbi + KB_FS_EXT4_SBI_GROUP_COUNT_OFFSET, sizeof(group_count));
    }
    if (group_count == 0 || group_count > 1024u) {
        return -5;
    }

    for (uint32_t group = 0; group < group_count; group++) {
        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call((void *)ext4_get_group_desc, &old_gs);
        void *group_desc = ext4_get_group_desc(super_block, group, NULL);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_get_group_info, &old_gs);
        void *group_info = ext4_get_group_info(super_block, group);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (group_desc == NULL || group_info == NULL) {
            return -5;
        }
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_free_group_clusters, &old_gs);
        unsigned int free_clusters = ext4_free_group_clusters(super_block, group_desc);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        uint32_t old_bb_free = 0;
        memcpy(&old_bb_free, (const uint8_t *)group_info + KB_FS_EXT4_GROUP_INFO_BB_FREE_OFFSET, sizeof(old_bb_free));
        write_u32_field(group_info, KB_FS_EXT4_GROUP_INFO_BB_FREE_OFFSET, free_clusters);
        write_u32_field(group_info, KB_FS_EXT4_GROUP_INFO_GROUP_OFFSET, group);
        void *prealloc_list = (uint8_t *)group_info + KB_FS_EXT4_GROUP_INFO_PREALLOC_LIST_OFFSET;
        write_pointer_field(prealloc_list, 0, prealloc_list);
        write_pointer_field(prealloc_list, sizeof(void *), prealloc_list);
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: sync_group_free group=%u info=%p old=%u desc=%u\n",
                group,
                group_info,
                old_bb_free,
                free_clusters);
        }
    }
    return 0;
}

int kb_fs_subsystem_ext4_recount_allocator_counts(void *super_block)
{
    if (super_block == NULL) {
        return -22;
    }
    typedef void *(*ext4_get_group_desc_fn)(void *, unsigned int, void *);
    typedef void *(*ext4_get_group_info_fn)(void *, unsigned int);
    ext4_get_group_desc_fn ext4_get_group_desc =
        (ext4_get_group_desc_fn)kb_module_lookup_exported_symbol("ext4_get_group_desc");
    ext4_get_group_info_fn ext4_get_group_info =
        (ext4_get_group_info_fn)kb_module_lookup_exported_symbol("ext4_get_group_info");
    if (ext4_get_group_desc == NULL || ext4_get_group_info == NULL) {
        return -95;
    }

    void *sbi = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_FS_INFO_OFFSET);
    uint32_t group_count = 0;
    if (sbi != NULL) {
        memcpy(&group_count, (const uint8_t *)sbi + KB_FS_EXT4_SBI_GROUP_COUNT_OFFSET, sizeof(group_count));
    }
    if (group_count == 0 || group_count > 1024u) {
        return -5;
    }

    uint64_t block_size = 0;
    memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    if (block_size == 0 || block_size > SIZE_MAX || (block_size & (block_size - 1u)) != 0) {
        return -22;
    }

    uint8_t super_disk[1024];
    if (active_bdev_read_exact(KB_FS_EXT4_SUPER_OFFSET, super_disk, sizeof(super_disk)) != 0) {
        return -5;
    }
    if (read_le16_fs(super_disk + KB_FS_EXT4_SUPER_MAGIC_OFFSET) != 0xef53u) {
        return -5;
    }

    uint32_t blocks_count = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_BLOCKS_COUNT_LO_OFFSET);
    uint32_t first_data_block = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_FIRST_DATA_BLOCK_OFFSET);
    uint32_t blocks_per_group = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_BLOCKS_PER_GROUP_OFFSET);
    uint32_t inodes_count = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_INODES_COUNT_OFFSET);
    uint32_t inodes_per_group = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_INODES_PER_GROUP_OFFSET);
    uint16_t desc_size = read_le16_fs(super_disk + KB_FS_EXT4_SUPER_DESC_SIZE_OFFSET);
    if (blocks_count == 0 || blocks_per_group == 0 || inodes_count == 0 || inodes_per_group == 0 ||
        first_data_block >= blocks_count)
    {
        return -5;
    }
    if (desc_size < 32) {
        desc_size = 32;
    }
    if (desc_size > 1024) {
        return -5;
    }

    uint8_t *block_bitmap = calloc(1, (size_t)block_size);
    uint8_t *inode_bitmap = calloc(1, (size_t)block_size);
    uint8_t group_desc_disk[1024];
    if (block_bitmap == NULL || inode_bitmap == NULL) {
        free(inode_bitmap);
        free(block_bitmap);
        return -12;
    }

    const uint64_t group_desc_table = (block_size == 1024u ? 2u : 1u) * block_size;
    for (uint32_t group = 0; group < group_count; group++) {
        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call((void *)ext4_get_group_desc, &old_gs);
        uint8_t *group_desc = ext4_get_group_desc(super_block, group, NULL);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_get_group_info, &old_gs);
        void *group_info = ext4_get_group_info(super_block, group);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (group_desc == NULL || group_info == NULL) {
            free(inode_bitmap);
            free(block_bitmap);
            return -5;
        }

        const uint64_t desc_offset = group_desc_table + ((uint64_t)group * desc_size);
        if (active_bdev_read_exact(desc_offset, group_desc_disk, desc_size) != 0) {
            free(inode_bitmap);
            free(block_bitmap);
            return -5;
        }

        const uint32_t block_bitmap_block =
            read_le32_fs(group_desc + KB_FS_EXT4_GROUP_DESC_BLOCK_BITMAP_LO_OFFSET);
        const uint32_t inode_bitmap_block =
            read_le32_fs(group_desc + KB_FS_EXT4_GROUP_DESC_INODE_BITMAP_LO_OFFSET);
        if (block_bitmap_block == 0 || inode_bitmap_block == 0) {
            free(inode_bitmap);
            free(block_bitmap);
            return -5;
        }
        if (active_bdev_read_exact((uint64_t)block_bitmap_block * block_size, block_bitmap, (size_t)block_size) != 0 ||
            active_bdev_read_exact((uint64_t)inode_bitmap_block * block_size, inode_bitmap, (size_t)block_size) != 0)
        {
            free(inode_bitmap);
            free(block_bitmap);
            return -5;
        }

        uint64_t group_first_block = (uint64_t)first_data_block + ((uint64_t)group * blocks_per_group);
        uint64_t group_block_count = blocks_per_group;
        if (group_first_block + group_block_count > blocks_count) {
            group_block_count = blocks_count - group_first_block;
        }
        uint32_t free_blocks = 0;
        for (uint64_t bit = 0; bit < group_block_count; bit++) {
            if ((block_bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) == 0) {
                free_blocks++;
            }
        }

        uint64_t group_first_inode = (uint64_t)group * inodes_per_group;
        uint64_t group_inode_count = inodes_per_group;
        if (group_first_inode + group_inode_count > inodes_count) {
            group_inode_count = inodes_count - group_first_inode;
        }
        uint32_t free_inodes = 0;
        for (uint64_t bit = 0; bit < group_inode_count; bit++) {
            if ((inode_bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) == 0) {
                free_inodes++;
            }
        }

        uint32_t old_bb_free = 0;
        memcpy(&old_bb_free, (const uint8_t *)group_info + KB_FS_EXT4_GROUP_INFO_BB_FREE_OFFSET, sizeof(old_bb_free));
        write_u32_field(group_info, KB_FS_EXT4_GROUP_INFO_BB_FREE_OFFSET, free_blocks);
        write_u32_field(group_info, KB_FS_EXT4_GROUP_INFO_GROUP_OFFSET, group);
        void *prealloc_list = (uint8_t *)group_info + KB_FS_EXT4_GROUP_INFO_PREALLOC_LIST_OFFSET;
        write_pointer_field(prealloc_list, 0, prealloc_list);
        write_pointer_field(prealloc_list, sizeof(void *), prealloc_list);

        write_le16_fs(group_desc + KB_FS_EXT4_GROUP_DESC_FREE_BLOCKS_COUNT_LO_OFFSET, (uint16_t)free_blocks);
        write_le16_fs(group_desc + KB_FS_EXT4_GROUP_DESC_FREE_INODES_COUNT_LO_OFFSET, (uint16_t)free_inodes);
        write_le16_fs(group_desc_disk + KB_FS_EXT4_GROUP_DESC_FREE_BLOCKS_COUNT_LO_OFFSET, (uint16_t)free_blocks);
        write_le16_fs(group_desc_disk + KB_FS_EXT4_GROUP_DESC_FREE_INODES_COUNT_LO_OFFSET, (uint16_t)free_inodes);
        if (active_bdev_write_exact(desc_offset, group_desc_disk, desc_size) != 0) {
            free(inode_bitmap);
            free(block_bitmap);
            return -5;
        }

        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: sync_group_free group=%u info=%p old=%u blocks=%u inodes=%u\n",
                group,
                group_info,
                old_bb_free,
                free_blocks,
                free_inodes);
        }
    }
    free(inode_bitmap);
    free(block_bitmap);
    return 0;
}

static int block_size_bits(uint64_t size)
{
    if (size == 0 || (size & (size - 1u)) != 0) {
        return -1;
    }
    int bits = 0;
    while (size > 1u) {
        size >>= 1;
        bits++;
    }
    return bits;
}

static int set_super_blocksize(void *super_block, uint64_t size)
{
    int bits = block_size_bits(size);
    if (super_block == NULL || bits < 0 || bits > UINT8_MAX) {
        return 0;
    }
    write_u8_field(super_block, KB_FS_SUPER_BLOCK_BLOCKSIZE_BITS_OFFSET, (uint8_t)bits);
    write_u64_field(super_block, KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, size);
    return 1;
}

static void ext4_seed_extent_status_stats(void *super_block)
{
    void *sbi = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_FS_INFO_OFFSET);
    if (sbi == NULL) {
        return;
    }
    void *es_lru = (uint8_t *)sbi + KB_FS_EXT4_SBI_ES_LRU_OFFSET;
    write_pointer_field(sbi, KB_FS_EXT4_SBI_ES_LRU_OFFSET, es_lru);
    write_pointer_field(sbi, KB_FS_EXT4_SBI_ES_LRU_OFFSET + sizeof(void *), es_lru);
    write_u64_field(sbi, KB_FS_EXT4_SBI_ES_SHRINK_COUNT_OFFSET, 0);
}

static int image_block_read(void *ctx, uint64_t offset, void *buffer, size_t size)
{
    kb_fs_image_block_ctx_t *image = (kb_fs_image_block_ctx_t *)ctx;
    if (image == NULL || image->path == NULL || buffer == NULL || size == 0) {
        return -22;
    }
    FILE *file = fopen(image->path, "rb");
    if (file == NULL) {
        return -5;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        return -5;
    }
    size_t read_count = fread(buffer, 1, size, file);
    fclose(file);
    return read_count == size ? 0 : -5;
}

static int image_block_write(void *ctx, uint64_t offset, const void *buffer, size_t size)
{
    kb_fs_image_block_ctx_t *image = (kb_fs_image_block_ctx_t *)ctx;
    if (image == NULL || image->path == NULL || buffer == NULL || size == 0) {
        return -22;
    }
    FILE *file = fopen(image->path, "r+b");
    if (file == NULL) {
        return -5;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        return -5;
    }
    size_t written = fwrite(buffer, 1, size, file);
    fclose(file);
    return written == size ? 0 : -5;
}

static void image_block_destroy(void *ctx)
{
    kb_fs_image_block_ctx_t *image = (kb_fs_image_block_ctx_t *)ctx;
    if (image == NULL) {
        return;
    }
    free(image->path);
    free(image);
}

static int block_disk_read(void *ctx, uint64_t offset, void *buffer, size_t size)
{
    kb_fs_block_disk_ctx_t *disk_ctx = (kb_fs_block_disk_ctx_t *)ctx;
    if (disk_ctx == NULL || disk_ctx->disk == NULL) {
        return -22;
    }
    if ((offset % 512u) == 0 && (size % 512u) == 0) {
        uint64_t sector = 0;
        if (__builtin_add_overflow(disk_ctx->start_sector, offset / 512u, &sector)) {
            return -34;
        }
        return kb_block_subsystem_disk_read(disk_ctx->disk, sector, buffer, size);
    }

    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    uint64_t start_sector = offset / 512u;
    uint64_t end_sector = (end + 511u) / 512u;
    uint64_t sector_count = end_sector - start_sector;
    uint64_t disk_sector = 0;
    if (__builtin_add_overflow(disk_ctx->start_sector, start_sector, &disk_sector)) {
        return -34;
    }
    uint64_t bounce_size64 = 0;
    if (__builtin_mul_overflow(sector_count, 512ull, &bounce_size64) || bounce_size64 > SIZE_MAX) {
        return -34;
    }
    void *bounce = calloc(1, (size_t)bounce_size64);
    if (bounce == NULL) {
        return -12;
    }
    int status = kb_block_subsystem_disk_read(disk_ctx->disk, disk_sector, bounce, (size_t)bounce_size64);
    if (status == 0) {
        memcpy(buffer, (const uint8_t *)bounce + (offset % 512u), size);
    }
    free(bounce);
    return status;
}

static int block_disk_write(void *ctx, uint64_t offset, const void *buffer, size_t size)
{
    kb_fs_block_disk_ctx_t *disk_ctx = (kb_fs_block_disk_ctx_t *)ctx;
    if (disk_ctx == NULL || disk_ctx->disk == NULL) {
        return -22;
    }
    if ((offset % 512u) == 0 && (size % 512u) == 0) {
        uint64_t sector = 0;
        if (__builtin_add_overflow(disk_ctx->start_sector, offset / 512u, &sector)) {
            return -34;
        }
        return kb_block_subsystem_disk_write(disk_ctx->disk, sector, buffer, size);
    }

    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    uint64_t start_sector = offset / 512u;
    uint64_t end_sector = (end + 511u) / 512u;
    uint64_t sector_count = end_sector - start_sector;
    uint64_t disk_sector = 0;
    if (__builtin_add_overflow(disk_ctx->start_sector, start_sector, &disk_sector)) {
        return -34;
    }
    uint64_t bounce_size64 = 0;
    if (__builtin_mul_overflow(sector_count, 512ull, &bounce_size64) || bounce_size64 > SIZE_MAX) {
        return -34;
    }
    void *bounce = calloc(1, (size_t)bounce_size64);
    if (bounce == NULL) {
        return -12;
    }
    int status = kb_block_subsystem_disk_read(disk_ctx->disk, disk_sector, bounce, (size_t)bounce_size64);
    if (status == 0) {
        memcpy((uint8_t *)bounce + (offset % 512u), buffer, size);
        status = kb_block_subsystem_disk_write(disk_ctx->disk, disk_sector, bounce, (size_t)bounce_size64);
    }
    free(bounce);
    return status;
}

static void block_disk_destroy(void *ctx)
{
    free(ctx);
}

static int image_size_bytes(const char *path, uint64_t *out_size)
{
    if (path == NULL || out_size == NULL) {
        return -22;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -5;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -5;
    }
    long size = ftell(file);
    fclose(file);
    if (size < 0) {
        return -5;
    }
    *out_size = (uint64_t)size;
    return 0;
}

static uint32_t read_le32_local(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64_local(const uint8_t *bytes)
{
    return (uint64_t)bytes[0] |
        ((uint64_t)bytes[1] << 8) |
        ((uint64_t)bytes[2] << 16) |
        ((uint64_t)bytes[3] << 24) |
        ((uint64_t)bytes[4] << 32) |
        ((uint64_t)bytes[5] << 40) |
        ((uint64_t)bytes[6] << 48) |
        ((uint64_t)bytes[7] << 56);
}

static int disk_gpt_partition_range(void *disk, uint32_t partition_index, uint64_t *out_start_sector, uint64_t *out_sector_count)
{
    if (disk == NULL || partition_index == 0 || out_start_sector == NULL || out_sector_count == NULL) {
        return -22;
    }

    uint8_t header[512];
    int status = kb_block_subsystem_disk_read(disk, 1, header, sizeof(header));
    if (status != 0) {
        return status;
    }
    if (memcmp(header, "EFI PART", 8) != 0) {
        return -22;
    }

    const uint64_t entry_lba = read_le64_local(header + 72);
    const uint32_t entry_count = read_le32_local(header + 80);
    const uint32_t entry_size = read_le32_local(header + 84);
    if (partition_index > entry_count || entry_size < 48 || entry_size > 512) {
        return -22;
    }

    const uint64_t entry_offset = (uint64_t)(partition_index - 1u) * (uint64_t)entry_size;
    const uint64_t entry_sector = entry_lba + entry_offset / 512u;
    const size_t within_sector = (size_t)(entry_offset % 512u);
    uint8_t entry_storage[1024];
    uint8_t *entry = entry_storage + within_sector;
    status = kb_block_subsystem_disk_read(disk, entry_sector, entry_storage, sizeof(entry_storage));
    if (status != 0) {
        return status;
    }

    int unused = 1;
    for (size_t i = 0; i < 16; i++) {
        if (entry[i] != 0) {
            unused = 0;
            break;
        }
    }
    if (unused) {
        return -2;
    }

    const uint64_t first_lba = read_le64_local(entry + 32);
    const uint64_t last_lba = read_le64_local(entry + 40);
    if (last_lba < first_lba) {
        return -22;
    }

    *out_start_sector = first_lba;
    *out_sector_count = last_lba - first_lba + 1u;
    return 0;
}

static kb_fs_block_device_t *block_device_for_bdev(void *bdev)
{
    if (active_bdev_binding.bdev == bdev && active_bdev_binding.device != NULL) {
        return active_bdev_binding.device;
    }
    return mount_probe_block_device;
}

static kb_fs_type_record_t *find_fs_type_by_ptr(void *fs_type)
{
    for (size_t i = 0; i < KB_FS_TYPE_MAX; i++) {
        if (fs_types[i].active && fs_types[i].fs_type == fs_type) {
            return &fs_types[i];
        }
    }
    return NULL;
}

static kb_fs_type_record_t *find_fs_type_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_TYPE_MAX; i++) {
        if (fs_types[i].active && fs_types[i].name != NULL && strcmp(fs_types[i].name, name) == 0) {
            return &fs_types[i];
        }
    }
    return NULL;
}

static kb_fs_mount_record_t *find_mount(uint64_t handle)
{
    if (handle == 0) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_MOUNT_MAX; i++) {
        if (fs_mounts[i].active && fs_mounts[i].handle == handle) {
            return &fs_mounts[i];
        }
    }
    return NULL;
}

static kb_fs_file_record_t *find_file(uint64_t mount_handle, const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_FILE_MAX; i++) {
        if (fs_files[i].active &&
            fs_files[i].mount_handle == mount_handle &&
            fs_files[i].path != NULL &&
            strcmp(fs_files[i].path, path) == 0)
        {
            return &fs_files[i];
        }
    }
    return NULL;
}

static kb_fs_file_record_t *file_for_write(uint64_t mount_handle, const char *path)
{
    kb_fs_file_record_t *file = find_file(mount_handle, path);
    if (file != NULL || path == NULL) {
        return file;
    }
    for (size_t i = 0; i < KB_FS_FILE_MAX; i++) {
        if (!fs_files[i].active) {
            fs_files[i].path = copy_string(path);
            if (fs_files[i].path == NULL) {
                return NULL;
            }
            fs_files[i].active = 1;
            fs_files[i].mount_handle = mount_handle;
            return &fs_files[i];
        }
    }
    return NULL;
}

static int fs_interface_is_ipc(const kb_interface_t *interface)
{
    const char *subsystem = kb_interface_subsystem(interface);
    return kb_interface_kind(interface) == KB_INTERFACE_IPC &&
        subsystem != NULL &&
        strcmp(subsystem, "fs") == 0;
}

kb_status_t kb_fs_subsystem_bind_ipc_interface(kb_platform_t *platform, kb_interface_t **out_interface)
{
    if (platform == NULL || out_interface == NULL) {
        return KB_ERR_INVALID;
    }
    *out_interface = NULL;

    size_t interface_count = 0;
    kb_status_t status = kb_platform_interface_count(platform, &interface_count);
    if (status != KB_OK) {
        return status;
    }

    for (size_t i = 0; i < interface_count; i++) {
        kb_interface_t *interface = NULL;
        status = kb_platform_interface_at(platform, i, &interface);
        if (status != KB_OK || interface == NULL) {
            continue;
        }
        if (fs_interface_is_ipc(interface)) {
            status = kb_interface_bind(interface, platform);
            if (status != KB_OK) {
                return status;
            }
            *out_interface = interface;
            return KB_OK;
        }
    }

    return KB_ERR_NOT_FOUND;
}

kb_status_t kb_fs_subsystem_dispatch(kb_interface_t *interface, kb_fs_ipc_request_t *request)
{
    if (interface == NULL || request == NULL || !fs_interface_is_ipc(interface)) {
        return KB_ERR_INVALID;
    }
    if (request->version == 0) {
        request->version = KB_FS_IPC_REQUEST_VERSION;
    }
    if (request->version != KB_FS_IPC_REQUEST_VERSION ||
        request->operation == KB_FS_OPERATION_NONE)
    {
        return KB_ERR_INVALID;
    }
    return kb_interface_dispatch(interface, request, sizeof(*request));
}

static kb_status_t fs_local_mount(const kb_fs_mount_desc_t *desc, uint64_t *out_handle)
{
    kb_fs_type_record_t *type = find_fs_type_by_name(desc->fs_type);
    if (type == NULL) {
        return KB_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < KB_FS_MOUNT_MAX; i++) {
        if (!fs_mounts[i].active) {
            char *source = copy_string(desc->source);
            char *target = copy_string(desc->target);
            if (source == NULL || target == NULL) {
                free(source);
                free(target);
                return KB_ERR_NOMEM;
            }
            fs_mounts[i].handle = next_mount_handle++;
            if (next_mount_handle == 0) {
                next_mount_handle = 1;
            }
            fs_mounts[i].type = type;
            fs_mounts[i].source = source;
            fs_mounts[i].target = target;
            fs_mounts[i].block_disk = desc->block_disk;
            fs_mounts[i].active = 1;
            if (out_handle != NULL) {
                *out_handle = fs_mounts[i].handle;
            }
            if (fs_trace_enabled()) {
                fprintf(stderr, "kobox-fs: mount fs=%s source=%s target=%s handle=%llu block_disk=%p\n",
                    type->name,
                    source,
                    target,
                    (unsigned long long)fs_mounts[i].handle,
                    desc->block_disk);
            }
            return KB_OK;
        }
    }
    return KB_ERR_NOMEM;
}

static kb_status_t fs_local_read(const kb_fs_read_desc_t *desc)
{
    if (find_mount(desc->handle) == NULL) {
        return KB_ERR_NOT_FOUND;
    }
    kb_fs_file_record_t *file = find_file(desc->handle, desc->path);
    if (file == NULL) {
        return KB_ERR_NOT_FOUND;
    }
    if (desc->offset >= file->size) {
        if (desc->out_bytes != NULL) {
            *desc->out_bytes = 0;
        }
        return KB_OK;
    }
    size_t available = file->size - (size_t)desc->offset;
    size_t copy_len = available < desc->byte_count ? available : desc->byte_count;
    memcpy(desc->buffer, file->data + desc->offset, copy_len);
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = copy_len;
    }
    return KB_OK;
}

static kb_status_t fs_local_write(const kb_fs_write_desc_t *desc)
{
    if (find_mount(desc->handle) == NULL) {
        return KB_ERR_NOT_FOUND;
    }
    kb_fs_file_record_t *file = file_for_write(desc->handle, desc->path);
    if (file == NULL) {
        return KB_ERR_NOMEM;
    }
    size_t end = (size_t)desc->offset + desc->byte_count;
    if (end < desc->byte_count) {
        return KB_ERR_INVALID;
    }
    if (end > file->size) {
        uint8_t *data = realloc(file->data, end);
        if (data == NULL) {
            return KB_ERR_NOMEM;
        }
        if (end > file->size) {
            memset(data + file->size, 0, end - file->size);
        }
        file->data = data;
        file->size = end;
    }
    memcpy(file->data + desc->offset, desc->buffer, desc->byte_count);
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = desc->byte_count;
    }
    return KB_OK;
}

static kb_status_t fs_local_readdir(const kb_fs_readdir_desc_t *desc)
{
    kb_fs_mount_record_t *mount = find_mount(desc->handle);
    if (mount == NULL) {
        return KB_ERR_NOT_FOUND;
    }
    size_t written = 0;
    for (size_t i = 0; i < KB_FS_FILE_MAX; i++) {
        if (!fs_files[i].active || fs_files[i].mount_handle != mount->handle || fs_files[i].path == NULL) {
            continue;
        }
        size_t len = strlen(fs_files[i].path);
        if (written + len + 1u > desc->byte_count) {
            break;
        }
        memcpy((uint8_t *)desc->buffer + written, fs_files[i].path, len);
        written += len;
        ((uint8_t *)desc->buffer)[written++] = '\n';
    }
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = written;
    }
    return KB_OK;
}

kb_status_t kb_fs_subsystem_mount(kb_interface_t *interface, const kb_fs_mount_desc_t *desc, uint64_t *out_handle)
{
    if (desc == NULL || desc->target == NULL || desc->fs_type == NULL) {
        return KB_ERR_INVALID;
    }
    kb_fs_ipc_request_t request = {
        .operation = KB_FS_OPERATION_MOUNT,
        .flags = desc->flags,
        .source = desc->source,
        .path = desc->target,
        .fs_type = desc->fs_type,
        .input = desc->data,
        .input_size = desc->data_size,
    };
    kb_status_t status = kb_fs_subsystem_dispatch(interface, &request);
    if (status != KB_OK) {
        return status;
    }
    if (out_handle != NULL) {
        *out_handle = request.handle;
    }
    if (!request.handled) {
        return fs_local_mount(desc, out_handle);
    }
    return request.result_code == 0 ? KB_OK : KB_ERR_IO;
}

kb_status_t kb_fs_subsystem_read(kb_interface_t *interface, const kb_fs_read_desc_t *desc)
{
    if (desc == NULL || desc->buffer == NULL || desc->byte_count == 0) {
        return KB_ERR_INVALID;
    }
    kb_fs_ipc_request_t request = {
        .operation = KB_FS_OPERATION_READ,
        .handle = desc->handle,
        .path = desc->path,
        .offset = desc->offset,
        .output = desc->buffer,
        .output_capacity = desc->byte_count,
    };
    kb_status_t status = kb_fs_subsystem_dispatch(interface, &request);
    if (status != KB_OK) {
        return status;
    }
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = request.output_size;
    }
    if (!request.handled) {
        return fs_local_read(desc);
    }
    return request.result_code == 0 ? KB_OK : KB_ERR_IO;
}

kb_status_t kb_fs_subsystem_write(kb_interface_t *interface, const kb_fs_write_desc_t *desc)
{
    if (desc == NULL || desc->buffer == NULL || desc->byte_count == 0) {
        return KB_ERR_INVALID;
    }
    kb_fs_ipc_request_t request = {
        .operation = KB_FS_OPERATION_WRITE,
        .handle = desc->handle,
        .path = desc->path,
        .offset = desc->offset,
        .input = desc->buffer,
        .input_size = desc->byte_count,
    };
    kb_status_t status = kb_fs_subsystem_dispatch(interface, &request);
    if (status != KB_OK) {
        return status;
    }
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = request.output_size;
    }
    if (!request.handled) {
        return fs_local_write(desc);
    }
    return request.result_code == 0 ? KB_OK : KB_ERR_IO;
}

kb_status_t kb_fs_subsystem_readdir(kb_interface_t *interface, const kb_fs_readdir_desc_t *desc)
{
    if (desc == NULL || desc->buffer == NULL || desc->byte_count == 0) {
        return KB_ERR_INVALID;
    }
    kb_fs_ipc_request_t request = {
        .operation = KB_FS_OPERATION_READDIR,
        .handle = desc->handle,
        .path = desc->path,
        .output = desc->buffer,
        .output_capacity = desc->byte_count,
    };
    kb_status_t status = kb_fs_subsystem_dispatch(interface, &request);
    if (status != KB_OK) {
        return status;
    }
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = request.output_size;
    }
    if (!request.handled) {
        return fs_local_readdir(desc);
    }
    return request.result_code == 0 ? KB_OK : KB_ERR_IO;
}

int kb_fs_subsystem_register_filesystem(void *fs_type)
{
    const char *name = fs_type_name_from_kernel_type(fs_type);
    if (fs_type == NULL || name == NULL) {
        return -22;
    }
    kb_fs_type_record_t *record = find_fs_type_by_ptr(fs_type);
    if (record == NULL) {
        record = find_fs_type_by_name(name);
    }
    if (record == NULL) {
        for (size_t i = 0; i < KB_FS_TYPE_MAX; i++) {
            if (!fs_types[i].active) {
                record = &fs_types[i];
                break;
            }
        }
    }
    if (record == NULL) {
        return -12;
    }
    if (!record->active) {
        record->name = copy_string(name);
        if (record->name == NULL) {
            return -12;
        }
    }
    record->active = 1;
    record->fs_type = fs_type;
    record->owner_module = kb_loader_active_module();
    record->register_count++;
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: register_filesystem name=%s fs_type=%p owner=%p\n",
            record->name,
            fs_type,
            (void *)record->owner_module);
    }
    return 0;
}

int kb_fs_subsystem_unregister_filesystem(void *fs_type)
{
    kb_fs_type_record_t *record = find_fs_type_by_ptr(fs_type);
    if (record == NULL) {
        return -2;
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: unregister_filesystem name=%s fs_type=%p\n", record->name, fs_type);
    }
    free(record->name);
    memset(record, 0, sizeof(*record));
    return 0;
}

int kb_fs_block_device_create(const kb_fs_block_device_desc_t *desc, kb_fs_block_device_t **out_device)
{
    if (desc == NULL || out_device == NULL || desc->read == NULL) {
        return -22;
    }
    *out_device = NULL;
    kb_fs_block_device_t *device = calloc(1, sizeof(*device));
    if (device == NULL) {
        return -12;
    }
    device->name = copy_string(desc->name == NULL ? "kobox-block" : desc->name);
    if (device->name == NULL) {
        free(device);
        return -12;
    }
    device->size_bytes = desc->size_bytes;
    device->logical_block_size = desc->logical_block_size == 0 ? 512u : desc->logical_block_size;
    device->ctx = desc->ctx;
    device->read = desc->read;
    device->write = desc->write;
    device->destroy = desc->destroy;
    *out_device = device;
    return 0;
}

int kb_fs_block_device_create_image(const char *name, const char *image_path, kb_fs_block_device_t **out_device)
{
    if (image_path == NULL || image_path[0] == '\0' || out_device == NULL) {
        return -22;
    }
    *out_device = NULL;
    uint64_t size_bytes = 0;
    int status = image_size_bytes(image_path, &size_bytes);
    if (status != 0) {
        return status;
    }
    kb_fs_image_block_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -12;
    }
    ctx->path = copy_string(image_path);
    if (ctx->path == NULL) {
        free(ctx);
        return -12;
    }
    kb_fs_block_device_desc_t desc = {
        .name = name == NULL ? image_path : name,
        .size_bytes = size_bytes,
        .logical_block_size = 512u,
        .ctx = ctx,
        .read = image_block_read,
        .write = image_block_write,
        .destroy = image_block_destroy,
    };
    status = kb_fs_block_device_create(&desc, out_device);
    if (status != 0) {
        image_block_destroy(ctx);
    }
    return status;
}

int kb_fs_block_device_create_from_disk(const char *name, void *disk, kb_fs_block_device_t **out_device)
{
    kb_block_disk_snapshot_t snapshot;
    int status = kb_block_subsystem_disk_snapshot(disk, &snapshot);
    if (status != 0) {
        return status;
    }
    return kb_fs_block_device_create_from_disk_range(name, disk, 0, snapshot.capacity_sectors, out_device);
}

int kb_fs_block_device_create_from_disk_range(
    const char *name,
    void *disk,
    uint64_t start_sector,
    uint64_t sector_count,
    kb_fs_block_device_t **out_device)
{
    if (disk == NULL || out_device == NULL) {
        return -22;
    }
    *out_device = NULL;

    kb_block_disk_snapshot_t snapshot;
    int status = kb_block_subsystem_disk_snapshot(disk, &snapshot);
    if (status != 0) {
        return status;
    }
    if (sector_count == 0 || start_sector > snapshot.capacity_sectors || sector_count > snapshot.capacity_sectors - start_sector) {
        return -34;
    }

    uint64_t size_bytes = 0;
    if (__builtin_mul_overflow(sector_count, 512ull, &size_bytes)) {
        return -34;
    }

    uint32_t logical_block_size = 512u;
    kb_block_queue_limits_t limits;
    if (snapshot.queue != NULL &&
        kb_block_subsystem_queue_limits(snapshot.queue, &limits) == 0 &&
        limits.logical_block_size != 0)
    {
        logical_block_size = limits.logical_block_size;
    }

    kb_fs_block_disk_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -12;
    }
    ctx->disk = disk;
    ctx->start_sector = start_sector;
    ctx->sector_count = sector_count;

    kb_fs_block_device_desc_t desc = {
        .name = name == NULL ? "kobox-block-disk" : name,
        .size_bytes = size_bytes,
        .logical_block_size = logical_block_size,
        .ctx = ctx,
        .read = block_disk_read,
        .write = block_disk_write,
        .destroy = block_disk_destroy,
    };
    status = kb_fs_block_device_create(&desc, out_device);
    if (status != 0) {
        block_disk_destroy(ctx);
    }
    return status;
}

int kb_fs_block_device_create_from_disk_gpt_partition(
    const char *name,
    void *disk,
    uint32_t partition_index,
    kb_fs_block_device_t **out_device)
{
    uint64_t start_sector = 0;
    uint64_t sector_count = 0;
    int status = disk_gpt_partition_range(disk, partition_index, &start_sector, &sector_count);
    if (status != 0) {
        return status;
    }
    return kb_fs_block_device_create_from_disk_range(name, disk, start_sector, sector_count, out_device);
}

static int block_device_range_valid(const kb_fs_block_device_t *device, uint64_t offset, size_t size)
{
    if (device == NULL || size == 0) {
        return -22;
    }
    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    if (end > device->size_bytes) {
        return -34;
    }
    return 0;
}

int kb_fs_block_device_read(kb_fs_block_device_t *device, uint64_t offset, void *buffer, size_t size)
{
    if (buffer == NULL || device == NULL || device->read == NULL) {
        return -22;
    }
    int status = block_device_range_valid(device, offset, size);
    if (status != 0) {
        return status;
    }
    return device->read(device->ctx, offset, buffer, size);
}

int kb_fs_block_device_write(kb_fs_block_device_t *device, uint64_t offset, const void *buffer, size_t size)
{
    if (buffer == NULL || device == NULL) {
        return -22;
    }
    if (device->write == NULL) {
        return -95;
    }
    int status = block_device_range_valid(device, offset, size);
    if (status != 0) {
        return status;
    }
    return device->write(device->ctx, offset, buffer, size);
}

void kb_fs_block_device_destroy(kb_fs_block_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (mount_probe_block_device == device) {
        mount_probe_block_device = NULL;
    }
    if (active_bdev_binding.device == device) {
        memset(&active_bdev_binding, 0, sizeof(active_bdev_binding));
    }
    if (device->destroy != NULL) {
        device->destroy(device->ctx);
    }
    free(device->name);
    free(device);
}

int kb_fs_subsystem_set_mount_probe_block_device(kb_fs_block_device_t *device)
{
    mount_probe_block_device = device;
    if (device == NULL) {
        clear_mount_probe_objects();
    }
    return 0;
}

int kb_fs_subsystem_set_mount_block_device(kb_fs_block_device_t *device)
{
    return kb_fs_subsystem_set_mount_probe_block_device(device);
}

static kb_fs_bio_record_t *bio_record_from_handle(void *bio)
{
    kb_fs_bio_record_t *record = (kb_fs_bio_record_t *)bio;
    if (record == NULL || record->magic != KB_FS_BIO_MAGIC) {
        return NULL;
    }
    return record;
}

void *kb_fs_subsystem_bio_alloc_bioset(void *bdev, unsigned short nr_vecs, unsigned int opf, unsigned int gfp, void *bioset)
{
    (void)nr_vecs;
    (void)gfp;
    (void)bioset;
    kb_fs_bio_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return NULL;
    }
    record->magic = KB_FS_BIO_MAGIC;
    record->opf = opf;
    record->bdev = bdev;
    record->result = -115;
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: bio_alloc_bioset bio=%p bdev=%p opf=0x%x\n",
            (void *)record,
            bdev,
            opf);
    }
    return record;
}

static int bio_set_buffer(kb_fs_bio_record_t *record, void *buffer, size_t len, size_t page_offset)
{
    if (record == NULL || buffer == NULL || len == 0) {
        return 0;
    }
    record->buffer = buffer;
    record->len = len;
    record->page_offset = page_offset;
    return (int)len;
}

int kb_fs_subsystem_bio_add_folio(void *bio, void *folio, size_t len, size_t offset)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL || folio == NULL) {
        return 0;
    }
    return bio_set_buffer(record, (uint8_t *)folio + offset, len, offset);
}

int kb_fs_subsystem_bio_add_page(void *bio, void *page, unsigned int len, unsigned int offset)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL || page == NULL) {
        return 0;
    }
    return bio_set_buffer(record, (uint8_t *)page + offset, len, offset);
}

void kb_fs_subsystem_bio_set_sector(void *bio, uint64_t sector)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record != NULL) {
        record->sector = sector;
    }
}

void kb_fs_subsystem_bio_set_end_io(void *bio, void (*end_io)(void *))
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record != NULL) {
        record->end_io = end_io;
    }
}

static void bio_call_end_io(kb_fs_bio_record_t *record)
{
    if (record == NULL || record->completed) {
        return;
    }
    record->completed = 1;
    if (record->end_io == NULL) {
        return;
    }
    unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)record->end_io);
    if (kernel_gs != 0) {
        kb_linux_call_void_ptr_gs(record->end_io, record, kernel_gs);
    } else {
        kb_linux_call_void_ptr(record->end_io, record);
    }
}

void kb_fs_subsystem_bio_endio(void *bio)
{
    bio_call_end_io(bio_record_from_handle(bio));
}

static int bio_submit_now(kb_fs_bio_record_t *record)
{
    if (record == NULL) {
        return -22;
    }
    kb_fs_block_device_t *device = block_device_for_bdev(record->bdev);
    if (device == NULL) {
        return -19;
    }
    unsigned int op = record->opf & KB_FS_BIO_OP_MASK;
    if (op == KB_FS_BIO_OP_FLUSH) {
        return 0;
    }
    if (record->buffer == NULL || record->len == 0) {
        return -22;
    }
    uint64_t offset = 0;
    if (__builtin_mul_overflow(record->sector, 512ull, &offset)) {
        return -34;
    }
    if (op == KB_FS_BIO_OP_READ) {
        return kb_fs_block_device_read(device, offset, record->buffer, record->len);
    }
    if (op == KB_FS_BIO_OP_WRITE) {
        return kb_fs_block_device_write(device, offset, record->buffer, record->len);
    }
    return -95;
}

static void bio_queue_push(kb_fs_bio_record_t *record)
{
    if (record == NULL || record->queued) {
        return;
    }
    record->queued = 1;
    record->completed = 0;
    record->next = NULL;
    if (bio_queue_tail == NULL) {
        bio_queue_head = record;
        bio_queue_tail = record;
    } else {
        bio_queue_tail->next = record;
        bio_queue_tail = record;
    }
    bio_queue_depth++;
}

static void bio_queue_remove(kb_fs_bio_record_t *record)
{
    if (record == NULL || !record->queued) {
        return;
    }
    kb_fs_bio_record_t *prev = NULL;
    kb_fs_bio_record_t *current = bio_queue_head;
    while (current != NULL) {
        if (current == record) {
            if (prev == NULL) {
                bio_queue_head = current->next;
            } else {
                prev->next = current->next;
            }
            if (bio_queue_tail == current) {
                bio_queue_tail = prev;
            }
            record->queued = 0;
            record->next = NULL;
            if (bio_queue_depth > 0) {
                bio_queue_depth--;
            }
            return;
        }
        prev = current;
        current = current->next;
    }
    record->queued = 0;
    record->next = NULL;
}

void kb_fs_subsystem_bio_set_auto_drain(int enabled)
{
    bio_auto_drain = enabled ? 1 : 0;
}

size_t kb_fs_subsystem_bio_queue_depth(void)
{
    return bio_queue_depth;
}

size_t kb_fs_subsystem_bio_drain(void)
{
    size_t drained = 0;
    while (bio_queue_head != NULL) {
        kb_fs_bio_record_t *record = bio_queue_head;
        bio_queue_head = record->next;
        if (bio_queue_head == NULL) {
            bio_queue_tail = NULL;
        }
        record->next = NULL;
        record->queued = 0;
        if (bio_queue_depth > 0) {
            bio_queue_depth--;
        }
        record->result = bio_submit_now(record);
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: drain_bio bio=%p bdev=%p op=%u sector=%llu len=%zu result=%d\n",
                (void *)record,
                record->bdev,
                record->opf & KB_FS_BIO_OP_MASK,
                (unsigned long long)record->sector,
                record->len,
                record->result);
        }
        bio_call_end_io(record);
        drained++;
    }
    return drained;
}

void kb_fs_subsystem_submit_bio(void *bio)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL) {
        return;
    }
    record->submitted++;
    record->result = -115;
    bio_queue_push(record);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: submit_bio bio=%p bdev=%p op=%u sector=%llu len=%zu queued=%u depth=%zu\n",
            (void *)record,
            record->bdev,
            record->opf & KB_FS_BIO_OP_MASK,
            (unsigned long long)record->sector,
            record->len,
            record->queued,
            bio_queue_depth);
    }
    if (bio_auto_drain) {
        (void)kb_fs_subsystem_bio_drain();
    }
}

void kb_fs_subsystem_submit_bio_noacct(void *bio)
{
    kb_fs_subsystem_submit_bio(bio);
}

int kb_fs_subsystem_bio_result(void *bio)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    return record == NULL ? -22 : record->result;
}

int kb_fs_subsystem_bio_snapshot(void *bio, kb_fs_bio_snapshot_t *out_snapshot)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    out_snapshot->bio = record;
    out_snapshot->block_device = record->bdev;
    out_snapshot->operation = record->opf & KB_FS_BIO_OP_MASK;
    out_snapshot->sector = record->sector;
    out_snapshot->length = record->len;
    out_snapshot->offset = record->page_offset;
    out_snapshot->result = record->result;
    out_snapshot->submitted = record->submitted;
    out_snapshot->queued = record->queued;
    out_snapshot->completed = record->completed;
    return 0;
}

void kb_fs_subsystem_bio_put(void *bio)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL) {
        return;
    }
    bio_queue_remove(record);
    record->magic = 0;
    free(record);
}

static void *folio_page_payload(void *folio)
{
    uintptr_t vmemmap_base = kb_linux_kvm_vmemmap_base();
    uintptr_t page_offset_base = kb_linux_kvm_page_offset_base();
    uintptr_t folio_addr = (uintptr_t)folio;
    if (folio_addr < vmemmap_base || page_offset_base == 0) {
        return NULL;
    }
    uintptr_t page_index = (folio_addr - vmemmap_base) / KB_FS_KVM_STRUCT_PAGE_SIZE;
    return (void *)(page_offset_base + page_index * KB_FS_PAGE_SIZE);
}

static void filemap_folio_attach_buffers_with_state(
    void *mapping,
    void *folio,
    uint64_t block_size,
    uint64_t state)
{
    if (mapping == NULL || folio == NULL || read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET) != NULL) {
        return;
    }

    if (block_size == 0 || block_size > KB_FS_PAGE_SIZE || (KB_FS_PAGE_SIZE % block_size) != 0) {
        block_size = KB_FS_PAGE_SIZE;
    }

    void *payload = folio_page_payload(folio);
    if (payload == NULL) {
        payload = calloc(1, KB_FS_PAGE_SIZE);
    }
    if (payload == NULL) {
        return;
    }

    const size_t buffer_count = KB_FS_PAGE_SIZE / (size_t)block_size;
    void *first = NULL;
    void *previous = NULL;
    for (size_t i = 0; i < buffer_count; i++) {
        void *buffer_head = calloc(1, KB_FS_FAKE_BUFFER_HEAD_BYTES);
        if (buffer_head == NULL) {
            break;
        }
        uint32_t refcount = 1u;
        write_u64_field(buffer_head, 0, state);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_FOLIO_OFFSET, folio);
        write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_SIZE_OFFSET, block_size);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_DATA_OFFSET, (uint8_t *)payload + i * block_size);
        memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));
        if (first == NULL) {
            first = buffer_head;
        }
        if (previous != NULL) {
            write_pointer_field(previous, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET, buffer_head);
        }
        previous = buffer_head;
    }
    if (first == NULL || previous == NULL) {
        return;
    }
    write_pointer_field(previous, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET, first);
    write_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET, first);
}

static void filemap_folio_attach_buffers(void *mapping, void *folio)
{
    void *inode = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    void *super_block = inode != NULL ? read_pointer_field(inode, KB_FS_INODE_SB_OFFSET) : NULL;
    uint64_t block_size = 0;
    if (super_block != NULL) {
        memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    }
    filemap_folio_attach_buffers_with_state(mapping, folio, block_size, 0);
}

void *kb_fs_subsystem_create_empty_buffers(void *folio, unsigned long block_size, unsigned long state)
{
    if (folio == NULL) {
        return NULL;
    }
    void *mapping = read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET);
    if (mapping == NULL) {
        return NULL;
    }
    filemap_folio_attach_buffers_with_state(mapping, folio, block_size, state);
    return read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
}

void *kb_fs_subsystem_filemap_get_folio(void *mapping, unsigned long index, unsigned int fgp_flags, unsigned int gfp)
{
    if (mapping == NULL) {
        return (void *)(uintptr_t)-22;
    }
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (!filemap_folio_cache[i].active ||
            filemap_folio_cache[i].mapping != mapping ||
            filemap_folio_cache[i].index != index)
        {
            continue;
        }
        void *folio = filemap_folio_cache[i].folio;
        uint32_t refcount = 0;
        memcpy(&refcount, (const uint8_t *)folio + KB_FS_FOLIO_REFCOUNT_OFFSET, sizeof(refcount));
        refcount++;
        write_u32_field(folio, KB_FS_FOLIO_REFCOUNT_OFFSET, refcount);
        if ((fgp_flags & KB_FS_FGP_LOCK) != 0) {
            uint64_t flags = 0;
            memcpy(&flags, folio, sizeof(flags));
            flags |= KB_FS_FOLIO_FLAG_LOCKED;
            write_u64_field(folio, 0, flags);
        }
        filemap_folio_attach_buffers(mapping, folio);
        return folio;
    }

    void *folio = kb_kvm_alloc_pages_stub(gfp, 0);
    if (folio == NULL) {
        return (void *)(uintptr_t)-12;
    }
    uint64_t flags = 0;
    if ((fgp_flags & KB_FS_FGP_LOCK) != 0) {
        flags |= KB_FS_FOLIO_FLAG_LOCKED;
    }
    write_u64_field(folio, 0, flags);
    write_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET, mapping);
    write_u64_field(folio, KB_FS_FOLIO_INDEX_OFFSET, index);
    write_u32_field(folio, KB_FS_FOLIO_REFCOUNT_OFFSET, 1);
    filemap_folio_attach_buffers(mapping, folio);
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (filemap_folio_cache[i].active) {
            continue;
        }
        filemap_folio_cache[i].active = 1;
        filemap_folio_cache[i].mapping = mapping;
        filemap_folio_cache[i].index = index;
        filemap_folio_cache[i].folio = folio;
        return folio;
    }
    return folio;
}

void kb_fs_subsystem_folio_unlock(void *folio)
{
    if (folio == NULL) {
        return;
    }
    uint64_t flags = 0;
    memcpy(&flags, folio, sizeof(flags));
    flags &= (uint64_t)~KB_FS_FOLIO_FLAG_LOCKED;
    write_u64_field(folio, 0, flags);
}

void *kb_fs_subsystem_bdev_getblk(void *bdev, uint64_t block_number, unsigned int block_size, unsigned int gfp)
{
    (void)gfp;
    if (block_size == 0 || block_size > 65536u) {
        return NULL;
    }
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (device == NULL) {
        return NULL;
    }

    void *buffer_head = NULL;
    void *folio = NULL;
    void *data = NULL;
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (buffer_cache[i].active &&
            buffer_cache[i].bdev == bdev &&
            buffer_cache[i].block_number == block_number &&
            buffer_cache[i].block_size == block_size)
        {
            buffer_head = buffer_cache[i].buffer_head;
            folio = buffer_cache[i].folio;
            data = buffer_cache[i].data;
            break;
        }
    }
    int cache_hit = buffer_head != NULL && data != NULL;
    if (cache_hit) {
        uint32_t refcount = 0;
        memcpy(&refcount, (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, sizeof(refcount));
        refcount++;
        memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));
    }
    if (!cache_hit) {
        buffer_head = kb_kzalloc(KB_FS_FAKE_BUFFER_HEAD_BYTES, 0);
        folio = kb_kzalloc(KB_FS_FAKE_FOLIO_BYTES, 0);
        data = kb_kzalloc(block_size, 0);
        if (buffer_head == NULL || folio == NULL || data == NULL) {
            kb_kfree(buffer_head);
            kb_kfree(folio);
            kb_kfree(data);
            return NULL;
        }

        uint64_t offset = 0;
        if (__builtin_mul_overflow(block_number, (uint64_t)block_size, &offset)) {
            kb_kfree(buffer_head);
            kb_kfree(folio);
            kb_kfree(data);
            return NULL;
        }
        int status = kb_fs_block_device_read(device, offset, data, block_size);
        if (status != 0) {
            kb_kfree(buffer_head);
            kb_kfree(folio);
            kb_kfree(data);
            return NULL;
        }

        uint64_t flags = 0x19u;
        uint32_t refcount = 1u;
        memcpy(buffer_head, &flags, sizeof(flags));
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET, buffer_head);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_FOLIO_OFFSET, folio);
        write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_SIZE_OFFSET, block_size);
        write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET, block_number);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_DATA_OFFSET, data);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_BDEV_OFFSET, bdev);
        write_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET, mount_probe_bdev_mapping);
        memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));

        for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
            if (buffer_cache[i].active) {
                continue;
            }
            buffer_cache[i].active = 1;
            buffer_cache[i].bdev = bdev;
            buffer_cache[i].block_number = block_number;
            buffer_cache[i].block_size = block_size;
            buffer_cache[i].buffer_head = buffer_head;
            buffer_cache[i].folio = folio;
            buffer_cache[i].data = data;
            break;
        }
    }

    uint64_t read_index = last_mount_path_probe.bdev_getblk_calls;
    if (read_index < KB_FS_MOUNT_PATH_BLOCK_READ_MAX) {
        last_mount_path_probe.block_read_numbers[read_index] = block_number;
        last_mount_path_probe.block_read_sizes[read_index] = block_size;
        last_mount_path_probe.block_read_count = (uint32_t)(read_index + 1u);
    }
    last_mount_path_probe.bdev_getblk_calls++;
    last_mount_path_probe.last_block_number = block_number;
    last_mount_path_probe.last_block_size = block_size;
    if (block_size == 1024u && block_number == 1u) {
        uint16_t magic = 0;
        memcpy(&magic, (const uint8_t *)data + 0x38u, sizeof(magic));
        last_mount_path_probe.observed_ext4_magic = magic;
    } else if (block_size >= 0x440u && block_number == 0u) {
        uint16_t magic = 0;
        memcpy(&magic, (const uint8_t *)data + 0x438u, sizeof(magic));
        last_mount_path_probe.observed_ext4_magic = magic;
    }
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: bdev_getblk device=%s bdev=%p block=%llu size=%u magic=0x%04x cache=%s\n",
            device->name == NULL ? "" : device->name,
            bdev,
            (unsigned long long)block_number,
            block_size,
            last_mount_path_probe.observed_ext4_magic,
            cache_hit ? "hit" : "miss");
    }
    return buffer_head;
}

void kb_fs_subsystem_buffer_head_put(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (!buffer_cache[i].active || buffer_cache[i].buffer_head != buffer_head) {
            continue;
        }
        uint32_t refcount = 0;
        memcpy(&refcount, (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, sizeof(refcount));
        if (refcount > 1u) {
            refcount--;
            memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));
            return;
        }
        refcount = 0;
        memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));
        return;
    }
    kb_kfree(buffer_head);
}

void kb_fs_subsystem_mark_buffer_dirty(void *buffer_head)
{
    if (buffer_head == NULL || active_bdev_binding.device == NULL || active_bdev_binding.device->write == NULL) {
        return;
    }
    uint64_t block_number = 0;
    uint64_t block_size = 0;
    void *data = NULL;
    memcpy(&block_number, (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET, sizeof(block_number));
    memcpy(&block_size, (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET, sizeof(block_size));
    memcpy(&data, (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_DATA_OFFSET, sizeof(data));
    if (data == NULL || block_size == 0 || block_size > SIZE_MAX) {
        return;
    }
    uint64_t offset = block_number * block_size;
    int status = active_bdev_binding.device->write(active_bdev_binding.device->ctx, offset, data, (size_t)block_size);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: mark_buffer_dirty block=%llu size=%llu status=%d\n",
            (unsigned long long)block_number,
            (unsigned long long)block_size,
            status);
    }
}

int kb_fs_subsystem_jbd2_journal_dirty_metadata(void *handle, void *buffer_head)
{
    (void)handle;
    kb_fs_subsystem_mark_buffer_dirty(buffer_head);
    return 0;
}

int kb_fs_subsystem_sync_dirty_buffer(void *buffer_head)
{
    kb_fs_subsystem_mark_buffer_dirty(buffer_head);
    return 0;
}

int kb_fs_subsystem_block_write_end(
    void *file,
    void *mapping,
    int64_t pos,
    unsigned int len,
    unsigned int copied,
    void *page,
    void *fsdata)
{
    (void)file;
    (void)mapping;
    (void)fsdata;
    if (page == NULL || pos < 0) {
        return 0;
    }
    if (copied > len) {
        copied = len;
    }

    void *head = read_pointer_field(page, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL || copied == 0) {
        return (int)copied;
    }

    uint64_t start = (uint64_t)pos & (KB_FS_PAGE_SIZE - 1u);
    uint64_t end = start + copied;
    if (end > KB_FS_PAGE_SIZE || end < start) {
        end = KB_FS_PAGE_SIZE;
    }

    uint64_t block_start = 0;
    void *bh = head;
    do {
        uint64_t block_size = 0;
        memcpy(&block_size, (const uint8_t *)bh + KB_FS_BUFFER_HEAD_SIZE_OFFSET, sizeof(block_size));
        if (block_size == 0 || block_size > KB_FS_PAGE_SIZE) {
            break;
        }
        uint64_t block_end = block_start + block_size;
        if (block_end > start && block_start < end) {
            uint64_t state = 0;
            memcpy(&state, bh, sizeof(state));
            state |= KB_FS_BH_UPTODATE | KB_FS_BH_DIRTY;
            state &= ~KB_FS_BH_NEW;
            write_u64_field(bh, 0, state);
            kb_fs_subsystem_mark_buffer_dirty(bh);
        }
        bh = read_pointer_field(bh, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        block_start = block_end;
    } while (bh != NULL && bh != head && block_start < KB_FS_PAGE_SIZE);

    return (int)copied;
}

int kb_fs_subsystem_sb_min_blocksize(void *super_block, int size)
{
    if (super_block == NULL || size <= 0) {
        return 0;
    }
    return set_super_blocksize(super_block, (uint64_t)size) ? size : 0;
}

int kb_fs_subsystem_sb_set_blocksize(void *super_block, int size)
{
    if (super_block == NULL || size <= 0) {
        return 0;
    }
    return set_super_blocksize(super_block, (uint64_t)size);
}

void *kb_fs_subsystem_iget_locked(void *super_block, unsigned long inode_number)
{
    if (super_block == NULL) {
        return NULL;
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: iget_locked inode=%lu\n", inode_number);
    }
    void *storage = calloc(1, KB_FS_FAKE_INODE_HEADROOM_BYTES + KB_FS_FAKE_INODE_BYTES + KB_FS_FAKE_INODE_MAPPING_BYTES);
    if (storage == NULL) {
        return NULL;
    }
    void *inode = (uint8_t *)storage + KB_FS_FAKE_INODE_HEADROOM_BYTES;
    void *mapping = (uint8_t *)inode + KB_FS_FAKE_INODE_BYTES;
    write_pointer_field(inode, KB_FS_INODE_SB_OFFSET, super_block);
    write_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET, mapping);
    write_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET, inode);
    uint8_t block_bits = 0;
    memcpy(&block_bits, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_BITS_OFFSET, sizeof(block_bits));
    memcpy((uint8_t *)inode + KB_FS_INODE_BLKBITS_OFFSET, &block_bits, sizeof(block_bits));
    write_u64_field(inode, KB_FS_INODE_NUMBER_OFFSET, (uint64_t)inode_number);
    write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, KB_FS_INODE_STATE_NEW);
    void *es_lru = (uint8_t *)inode + KB_FS_INODE_EXT4_ES_LRU_OFFSET;
    write_pointer_field(inode, KB_FS_INODE_EXT4_ES_LRU_OFFSET, es_lru);
    write_pointer_field(inode, KB_FS_INODE_EXT4_ES_LRU_OFFSET + sizeof(void *), es_lru);
    write_u32_field(inode, KB_FS_INODE_EXT4_ES_SHRINK_COUNT_OFFSET, 0);
    return inode;
}

void *kb_fs_subsystem_new_inode(void *super_block)
{
    return kb_fs_subsystem_iget_locked(super_block, 0);
}

void kb_fs_subsystem_free_fake_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    free((uint8_t *)inode - KB_FS_FAKE_INODE_HEADROOM_BYTES);
}

int kb_fs_subsystem_inode_init_owner(void *idmap, void *inode, void *dir, unsigned short mode)
{
    (void)idmap;
    (void)dir;
    if (inode == NULL) {
        return -22;
    }
    write_u32_field(inode, KB_FS_INODE_MODE_OFFSET, mode);
    write_u32_field(inode, KB_FS_INODE_NLINK_OFFSET, 1);
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: inode_init_owner inode=%p mode=0%o\n", inode, mode);
    }
    return 0;
}

void *kb_fs_subsystem_d_make_root(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return NULL;
    }
    void *dentry = calloc(1, KB_FS_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return NULL;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    kb_fs_prepare_named_dentry(dentry, dentry, inode, super_block, "/");
    last_mount_path_probe.root_inode = inode;
    last_mount_path_probe.root_dentry = dentry;
    if (fs_trace_enabled()) {
        uint16_t mode = 0;
        uint32_t nlink = 0;
        uint64_t size = 0;
        memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
        memcpy(&nlink, (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, sizeof(nlink));
        memcpy(&size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(size));
        fprintf(stderr,
            "kobox-fs: d_make_root inode=%p dentry=%p mode=0%o nlink=%u size=%llu\n",
            inode,
            dentry,
            mode,
            nlink,
            (unsigned long long)size);
    }
    return dentry;
}

void *kb_fs_subsystem_d_splice_alias(void *inode, void *dentry)
{
    if (low_or_err_pointer(inode)) {
        return inode;
    }
    if (dentry == NULL) {
        return NULL;
    }
    write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    if (read_pointer_field(dentry, KB_FS_DENTRY_SB_OFFSET) == NULL) {
        write_pointer_field(dentry, KB_FS_DENTRY_SB_OFFSET, super_block);
    }
    last_mount_path_probe.lookup_inode = inode;
    last_mount_path_probe.lookup_dentry = dentry;
    if (fs_trace_enabled()) {
        uint16_t mode = 0;
        uint32_t nlink = 0;
        uint64_t size = 0;
        memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
        memcpy(&nlink, (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, sizeof(nlink));
        memcpy(&size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(size));
        fprintf(stderr,
            "kobox-fs: d_splice_alias inode=%p dentry=%p mode=0%o nlink=%u size=%llu\n",
            inode,
            dentry,
            mode,
            nlink,
            (unsigned long long)size);
    }
    return dentry;
}

void kb_fs_subsystem_d_instantiate(void *dentry, void *inode)
{
    if (dentry == NULL || low_or_err_pointer(inode)) {
        return;
    }
    write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    if (read_pointer_field(dentry, KB_FS_DENTRY_SB_OFFSET) == NULL) {
        write_pointer_field(dentry, KB_FS_DENTRY_SB_OFFSET, super_block);
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: d_instantiate dentry=%p inode=%p\n", dentry, inode);
    }
}

void kb_fs_subsystem_d_instantiate_new(void *dentry, void *inode)
{
    kb_fs_subsystem_d_instantiate(dentry, inode);
    kb_fs_subsystem_unlock_new_inode(inode);
}

int kb_fs_subsystem_fscrypt_match_name(const void *fname, const void *de_name, unsigned int de_name_len)
{
    if (fname == NULL || de_name == NULL) {
        return 0;
    }
    const void *target_name = NULL;
    uint32_t target_len = 0;
    memcpy(&target_name, (const uint8_t *)fname + 0x8, sizeof(target_name));
    memcpy(&target_len, (const uint8_t *)fname + 0x10, sizeof(target_len));
    if (target_name == NULL || target_len != de_name_len) {
        return 0;
    }
    return memcmp(target_name, de_name, de_name_len) == 0;
}

int kb_fs_subsystem_fscrypt_setup_filename(void *dir, const void *qstr, int lookup, void *fname)
{
    (void)dir;
    (void)lookup;
    if (qstr == NULL || fname == NULL) {
        return -22;
    }
    uint64_t hash_len = 0;
    const void *name = NULL;
    memcpy(&hash_len, qstr, sizeof(hash_len));
    memcpy(&name, (const uint8_t *)qstr + 0x8, sizeof(name));
    uint32_t len = (uint32_t)(hash_len >> 32);
    if (len == 0) {
        len = (uint32_t)hash_len;
    }
    memset(fname, 0, 0x30);
    write_pointer_field(fname, 0x0, (void *)(uintptr_t)qstr);
    write_pointer_field(fname, 0x8, (void *)(uintptr_t)name);
    write_u64_field(fname, 0x10, (uint64_t)len);
    write_pointer_field(fname, 0x20, (void *)(uintptr_t)name);
    write_u64_field(fname, 0x28, (uint64_t)len);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: fscrypt_setup_filename name=%.*s len=%u lookup=%d\n",
            (int)(len < 80 ? len : 80),
            name == NULL ? "" : (const char *)name,
            len,
            lookup);
    }
    return 0;
}

static uint32_t ext4_direct_block_number(const void *inode, uint64_t file_block)
{
    if (inode == NULL || file_block >= KB_FS_INODE_EXT4_DIRECT_BLOCKS) {
        return 0;
    }
    uint32_t block = 0;
    memcpy(&block,
        (const uint8_t *)inode - KB_FS_INODE_EXT4_DIRECT_BLOCK0_BACK_OFFSET + (file_block * sizeof(block)),
        sizeof(block));
    return block;
}

static uint32_t ext4_extent_node_block_number(const uint8_t *node, uint64_t file_block, uint64_t block_size, unsigned int depth_limit)
{
    if (node == NULL || file_block > UINT32_MAX || block_size == 0 || depth_limit == 0) {
        return 0;
    }
    uint16_t magic = 0;
    uint16_t entries = 0;
    uint16_t max_entries = 0;
    uint16_t depth = 0;
    memcpy(&magic, node, sizeof(magic));
    if (magic != KB_FS_EXT4_EXTENT_HEADER_MAGIC) {
        return 0;
    }
    memcpy(&entries, node + 0x2, sizeof(entries));
    memcpy(&max_entries, node + 0x4, sizeof(max_entries));
    memcpy(&depth, node + 0x6, sizeof(depth));
    if (entries == 0 || (max_entries != 0 && entries > max_entries)) {
        return 0;
    }
    if (depth == 0) {
        for (uint16_t i = 0; i < entries; i++) {
            const uint8_t *extent = node + 0x0c + ((size_t)i * 0x0c);
            uint32_t ee_block = 0;
            uint16_t ee_len = 0;
            uint16_t ee_start_hi = 0;
            uint32_t ee_start_lo = 0;
            memcpy(&ee_block, extent, sizeof(ee_block));
            memcpy(&ee_len, extent + 0x4, sizeof(ee_len));
            memcpy(&ee_start_hi, extent + 0x6, sizeof(ee_start_hi));
            memcpy(&ee_start_lo, extent + 0x8, sizeof(ee_start_lo));
            uint32_t len = ee_len & 0x7fffu;
            if (len == 0 ||
                file_block < ee_block ||
                file_block >= (uint64_t)ee_block + len)
            {
                continue;
            }
            uint64_t physical = ((uint64_t)ee_start_hi << 32) | ee_start_lo;
            physical += file_block - ee_block;
            return physical > UINT32_MAX ? 0 : (uint32_t)physical;
        }
        return 0;
    }

    const uint8_t *selected = NULL;
    for (uint16_t i = 0; i < entries; i++) {
        const uint8_t *index = node + 0x0c + ((size_t)i * 0x0c);
        uint32_t ei_block = 0;
        memcpy(&ei_block, index, sizeof(ei_block));
        if (file_block < ei_block) {
            break;
        }
        selected = index;
    }
    if (selected == NULL || active_bdev_binding.device == NULL || active_bdev_binding.device->read == NULL) {
        return 0;
    }

    uint32_t leaf_lo = 0;
    uint16_t leaf_hi = 0;
    memcpy(&leaf_lo, selected + 0x4, sizeof(leaf_lo));
    memcpy(&leaf_hi, selected + 0x8, sizeof(leaf_hi));
    uint64_t leaf_block = ((uint64_t)leaf_hi << 32) | leaf_lo;
    if (leaf_block == 0 || leaf_block > UINT32_MAX || block_size > SIZE_MAX) {
        return 0;
    }

    uint8_t *leaf = calloc(1, (size_t)block_size);
    if (leaf == NULL) {
        return 0;
    }
    uint64_t leaf_offset = leaf_block * block_size;
    uint32_t mapped = 0;
    if (active_bdev_binding.device->read(active_bdev_binding.device->ctx, leaf_offset, leaf, (size_t)block_size) == 0) {
        mapped = ext4_extent_node_block_number(leaf, file_block, block_size, depth_limit - 1u);
    }
    free(leaf);
    return mapped;
}

static uint32_t ext4_extent_block_number(const void *inode, uint64_t file_block)
{
    if (inode == NULL || file_block > UINT32_MAX) {
        return 0;
    }
    const uint8_t *i_block = (const uint8_t *)inode - KB_FS_INODE_EXT4_DIRECT_BLOCK0_BACK_OFFSET;
    uint16_t magic = 0;
    uint16_t entries = 0;
    uint16_t depth = 0;
    memcpy(&magic, i_block, sizeof(magic));
    if (magic != KB_FS_EXT4_EXTENT_HEADER_MAGIC) {
        return ext4_direct_block_number(inode, file_block);
    }
    void *super_block = NULL;
    uint64_t block_size = 0;
    memcpy(&super_block, (const uint8_t *)inode + KB_FS_INODE_SB_OFFSET, sizeof(super_block));
    if (super_block != NULL) {
        memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    }
    if (block_size == 0) {
        block_size = 4096;
    }
    (void)entries;
    (void)depth;
    return ext4_extent_node_block_number(i_block, file_block, block_size, 4);
}

static uint16_t read_le16_fs(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32_fs(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static void write_le16_fs(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32_fs(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static int active_bdev_read_exact(uint64_t offset, void *buffer, size_t size);
static int active_bdev_write_exact(uint64_t offset, const void *buffer, size_t size);

static int ext4_sum_group_desc_free_counts(
    void *super_block,
    uint64_t *out_free_blocks,
    uint64_t *out_free_inodes)
{
    if (super_block == NULL || out_free_blocks == NULL || out_free_inodes == NULL) {
        return -22;
    }
    *out_free_blocks = 0;
    *out_free_inodes = 0;
    typedef void *(*ext4_get_group_desc_fn)(void *, unsigned int, void *);
    ext4_get_group_desc_fn ext4_get_group_desc =
        (ext4_get_group_desc_fn)kb_module_lookup_exported_symbol("ext4_get_group_desc");
    if (ext4_get_group_desc == NULL) {
        return -95;
    }

    void *sbi = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_FS_INFO_OFFSET);
    uint32_t group_count = 0;
    if (sbi != NULL) {
        memcpy(&group_count, (const uint8_t *)sbi + KB_FS_EXT4_SBI_GROUP_COUNT_OFFSET, sizeof(group_count));
    }
    if (group_count == 0 || group_count > 4096u) {
        return -5;
    }

    uint64_t free_blocks = 0;
    uint64_t free_inodes = 0;
    for (uint32_t group = 0; group < group_count; group++) {
        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call((void *)ext4_get_group_desc, &old_gs);
        uint8_t *group_desc = ext4_get_group_desc(super_block, group, NULL);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (group_desc == NULL) {
            return -5;
        }
        free_blocks += read_le16_fs(group_desc + KB_FS_EXT4_GROUP_DESC_FREE_BLOCKS_COUNT_LO_OFFSET);
        free_inodes += read_le16_fs(group_desc + KB_FS_EXT4_GROUP_DESC_FREE_INODES_COUNT_LO_OFFSET);
    }
    *out_free_blocks = free_blocks;
    *out_free_inodes = free_inodes;
    return 0;
}

static int active_bdev_read_exact(uint64_t offset, void *buffer, size_t size)
{
    if (buffer == NULL ||
        active_bdev_binding.device == NULL ||
        active_bdev_binding.device->read == NULL)
    {
        return -5;
    }
    uint64_t logical = active_bdev_binding.device->logical_block_size;
    if (logical == 0) {
        logical = 512u;
    }
    if (logical < 512u) {
        logical = 512u;
    }
    if ((logical & (logical - 1u)) != 0 || logical > SIZE_MAX) {
        return -5;
    }
    if ((offset % logical) == 0 && (size % logical) == 0) {
        return active_bdev_binding.device->read(
            active_bdev_binding.device->ctx,
            offset,
            buffer,
            size);
    }

    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    uint64_t aligned_start = offset & ~(logical - 1u);
    uint64_t aligned_end = (end + logical - 1u) & ~(logical - 1u);
    uint64_t bounce_size64 = aligned_end - aligned_start;
    if (bounce_size64 > SIZE_MAX) {
        return -34;
    }
    uint8_t *bounce = calloc(1, (size_t)bounce_size64);
    if (bounce == NULL) {
        return -12;
    }
    int status = active_bdev_binding.device->read(
        active_bdev_binding.device->ctx,
        aligned_start,
        bounce,
        (size_t)bounce_size64);
    if (status == 0) {
        memcpy(buffer, bounce + (offset - aligned_start), size);
    }
    free(bounce);
    return status;
}

static int active_bdev_write_exact(uint64_t offset, const void *buffer, size_t size)
{
    if (buffer == NULL ||
        active_bdev_binding.device == NULL ||
        active_bdev_binding.device->write == NULL)
    {
        return -5;
    }
    uint64_t logical = active_bdev_binding.device->logical_block_size;
    if (logical == 0) {
        logical = 512u;
    }
    if (logical < 512u) {
        logical = 512u;
    }
    if ((logical & (logical - 1u)) != 0 || logical > SIZE_MAX) {
        return -5;
    }
    if ((offset % logical) == 0 && (size % logical) == 0) {
        int status = active_bdev_binding.device->write(
            active_bdev_binding.device->ctx,
            offset,
            buffer,
            size);
        if (status == 0) {
            update_buffer_cache_from_write(offset, buffer, size);
        }
        return status;
    }

    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    uint64_t aligned_start = offset & ~(logical - 1u);
    uint64_t aligned_end = (end + logical - 1u) & ~(logical - 1u);
    uint64_t bounce_size64 = aligned_end - aligned_start;
    if (bounce_size64 > SIZE_MAX) {
        return -34;
    }
    uint8_t *bounce = calloc(1, (size_t)bounce_size64);
    if (bounce == NULL) {
        return -12;
    }
    int status = active_bdev_binding.device->read(
        active_bdev_binding.device->ctx,
        aligned_start,
        bounce,
        (size_t)bounce_size64);
    if (status == 0) {
        memcpy(bounce + (offset - aligned_start), buffer, size);
        status = active_bdev_binding.device->write(
            active_bdev_binding.device->ctx,
            aligned_start,
            bounce,
            (size_t)bounce_size64);
        if (status == 0) {
            update_buffer_cache_from_write(offset, buffer, size);
        }
    }
    free(bounce);
    return status;
}

int kb_fs_subsystem_ext4_sync_super_free_blocks(void *super_block)
{
    if (super_block == NULL) {
        return -22;
    }
    void *sbi = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_FS_INFO_OFFSET);
    if (sbi == NULL) {
        return -22;
    }
    void *super_buffer = read_pointer_field(sbi, KB_FS_EXT4_SBI_SUPER_BUFFER_HEAD_OFFSET);
    uint8_t *ext4_super = read_pointer_field(sbi, KB_FS_EXT4_SBI_EXT4_SUPER_OFFSET);
    if (super_buffer == NULL || ext4_super == NULL) {
        return -22;
    }

    uint64_t free_blocks = 0;
    uint64_t free_inodes = 0;
    int status = ext4_sum_group_desc_free_counts(super_block, &free_blocks, &free_inodes);
    if (status != 0) {
        return status;
    }
    write_le32_fs(ext4_super + KB_FS_EXT4_SUPER_FREE_BLOCKS_COUNT_LO_OFFSET, (uint32_t)free_blocks);
    write_le32_fs(ext4_super + KB_FS_EXT4_SUPER_FREE_BLOCKS_COUNT_HI_OFFSET, (uint32_t)(free_blocks >> 32));
    write_le32_fs(ext4_super + KB_FS_EXT4_SUPER_FREE_INODES_COUNT_OFFSET, (uint32_t)free_inodes);

    typedef void (*ext4_superblock_csum_set_fn)(void *);
    ext4_superblock_csum_set_fn ext4_superblock_csum_set =
        (ext4_superblock_csum_set_fn)kb_module_lookup_exported_symbol("ext4_superblock_csum_set");
    if (ext4_superblock_csum_set == NULL) {
        return -95;
    }
    unsigned long old_gs = 0;
    int has_gs = kb_fs_enter_ext4_call((void *)ext4_superblock_csum_set, &old_gs);
    ext4_superblock_csum_set(super_block);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_subsystem_mark_buffer_dirty(super_buffer);
    return active_bdev_write_exact(KB_FS_EXT4_SUPER_OFFSET, ext4_super, 1024);
}

int kb_fs_subsystem_ext4_release_inode_record(void *super_block, uint64_t ino64)
{
    if (super_block == NULL ||
        active_bdev_binding.device == NULL ||
        active_bdev_binding.device->read == NULL ||
        active_bdev_binding.device->write == NULL)
    {
        return -22;
    }

    uint64_t block_size = 0;
    memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    if (ino64 == 0 || ino64 > UINT32_MAX ||
        block_size == 0 || block_size > SIZE_MAX ||
        (block_size & (block_size - 1u)) != 0)
    {
        return -22;
    }

    uint8_t super_disk[1024];
    if (active_bdev_read_exact(KB_FS_EXT4_SUPER_OFFSET, super_disk, sizeof(super_disk)) != 0) {
        return -5;
    }
    if (read_le16_fs(super_disk + KB_FS_EXT4_SUPER_MAGIC_OFFSET) != 0xef53u) {
        return -5;
    }

    uint32_t inodes_count = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_INODES_COUNT_OFFSET);
    uint32_t inodes_per_group = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_INODES_PER_GROUP_OFFSET);
    uint16_t inode_size = read_le16_fs(super_disk + KB_FS_EXT4_SUPER_INODE_SIZE_OFFSET);
    uint16_t desc_size = read_le16_fs(super_disk + KB_FS_EXT4_SUPER_DESC_SIZE_OFFSET);
    if (inodes_count == 0 || inodes_per_group == 0 ||
        ino64 > inodes_count || inode_size == 0 || inode_size > block_size)
    {
        return -22;
    }
    if (desc_size < 32) {
        desc_size = 32;
    }
    if (desc_size > 1024) {
        return -22;
    }

    uint64_t group = (ino64 - 1u) / inodes_per_group;
    uint64_t index = (ino64 - 1u) % inodes_per_group;
    uint64_t group_desc_table = (block_size == 1024u ? 2u : 1u) * block_size;
    uint64_t desc_offset = group_desc_table + (group * desc_size);

    uint8_t group_desc[1024];
    if (active_bdev_read_exact(desc_offset, group_desc, desc_size) != 0) {
        return -5;
    }
    uint32_t inode_bitmap_block = read_le32_fs(group_desc + KB_FS_EXT4_GROUP_DESC_INODE_BITMAP_LO_OFFSET);
    uint32_t inode_table_block = read_le32_fs(group_desc + KB_FS_EXT4_GROUP_DESC_INODE_TABLE_LO_OFFSET);
    if (inode_bitmap_block == 0 || inode_table_block == 0) {
        return -5;
    }

    uint8_t *bitmap = calloc(1, (size_t)block_size);
    uint8_t *disk_inode = calloc(1, inode_size);
    if (bitmap == NULL || disk_inode == NULL) {
        free(disk_inode);
        free(bitmap);
        return -12;
    }

    int status = 0;
    uint64_t bitmap_offset = (uint64_t)inode_bitmap_block * block_size;
    if (active_bdev_read_exact(bitmap_offset, bitmap, (size_t)block_size) != 0) {
        status = -5;
        goto done;
    }

    uint8_t mask = (uint8_t)(1u << (index & 7u));
    uint8_t *byte = &bitmap[index >> 3];
    if ((*byte & mask) != 0) {
        *byte &= (uint8_t)~mask;
        if (active_bdev_write_exact(bitmap_offset, bitmap, (size_t)block_size) != 0) {
            status = -5;
            goto done;
        }

        uint16_t group_free = read_le16_fs(group_desc + KB_FS_EXT4_GROUP_DESC_FREE_INODES_COUNT_LO_OFFSET);
        if (group_free != UINT16_MAX) {
            write_le16_fs(
                group_desc + KB_FS_EXT4_GROUP_DESC_FREE_INODES_COUNT_LO_OFFSET,
                (uint16_t)(group_free + 1u));
            if (active_bdev_write_exact(desc_offset, group_desc, desc_size) != 0) {
                status = -5;
                goto done;
            }
        }

        uint32_t super_free = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_FREE_INODES_COUNT_OFFSET);
        if (super_free != UINT32_MAX) {
            uint32_t next_super_free = super_free + 1u;
            write_le32_fs(super_disk + KB_FS_EXT4_SUPER_FREE_INODES_COUNT_OFFSET, next_super_free);
            (void)active_bdev_write_exact(KB_FS_EXT4_SUPER_OFFSET, super_disk, sizeof(super_disk));

            void *sbi = super_block != NULL ? read_pointer_field(super_block, KB_FS_SUPER_BLOCK_FS_INFO_OFFSET) : NULL;
            uint8_t *ext4_super = sbi != NULL ? read_pointer_field(sbi, KB_FS_EXT4_SBI_EXT4_SUPER_OFFSET) : NULL;
            void *super_buffer = sbi != NULL ? read_pointer_field(sbi, KB_FS_EXT4_SBI_SUPER_BUFFER_HEAD_OFFSET) : NULL;
            if (ext4_super != NULL) {
                write_le32_fs(ext4_super + KB_FS_EXT4_SUPER_FREE_INODES_COUNT_OFFSET, next_super_free);
                typedef void (*ext4_superblock_csum_set_fn)(void *);
                ext4_superblock_csum_set_fn ext4_superblock_csum_set =
                    (ext4_superblock_csum_set_fn)kb_module_lookup_exported_symbol("ext4_superblock_csum_set");
                if (ext4_superblock_csum_set != NULL) {
                    unsigned long old_gs = 0;
                    int has_gs = kb_fs_enter_ext4_call((void *)ext4_superblock_csum_set, &old_gs);
                    ext4_superblock_csum_set(super_block);
                    if (has_gs) {
                        kb_shim_leave_kernel_gs(old_gs);
                    }
                }
                if (super_buffer != NULL) {
                    kb_fs_subsystem_mark_buffer_dirty(super_buffer);
                }
            }
        }
    }

    uint64_t disk_inode_offset = ((uint64_t)inode_table_block * block_size) + (index * inode_size);
    if (active_bdev_read_exact(disk_inode_offset, disk_inode, inode_size) == 0 &&
        read_le32_fs(disk_inode + KB_FS_EXT4_DISK_INODE_DTIME_OFFSET) == 0)
    {
        write_le32_fs(disk_inode + KB_FS_EXT4_DISK_INODE_DTIME_OFFSET, 1);
        (void)active_bdev_write_exact(disk_inode_offset, disk_inode, inode_size);
    }

done:
    free(disk_inode);
    free(bitmap);
    return status;
}

static int ext4_free_direct_data_block(void *super_block, uint32_t disk_block, uint64_t block_size)
{
    if (super_block == NULL || disk_block == 0 ||
        active_bdev_binding.device == NULL ||
        active_bdev_binding.device->read == NULL ||
        active_bdev_binding.device->write == NULL)
    {
        return -5;
    }
    if (block_size == 0 || block_size > SIZE_MAX || (block_size & (block_size - 1u)) != 0) {
        return -22;
    }

    uint8_t super_disk[1024];
    if (active_bdev_read_exact(KB_FS_EXT4_SUPER_OFFSET, super_disk, sizeof(super_disk)) != 0)
    {
        return -5;
    }
    if (read_le16_fs(super_disk + KB_FS_EXT4_SUPER_MAGIC_OFFSET) != 0xef53u) {
        return -5;
    }

    uint32_t blocks_count = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_BLOCKS_COUNT_LO_OFFSET);
    uint32_t first_data_block = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_FIRST_DATA_BLOCK_OFFSET);
    uint32_t blocks_per_group = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_BLOCKS_PER_GROUP_OFFSET);
    uint16_t desc_size = read_le16_fs(super_disk + KB_FS_EXT4_SUPER_DESC_SIZE_OFFSET);
    if (blocks_count == 0 || blocks_per_group == 0 ||
        disk_block >= blocks_count || disk_block < first_data_block)
    {
        return -22;
    }
    if (desc_size < 32) {
        desc_size = 32;
    }
    if (desc_size > 1024) {
        return -22;
    }

    uint64_t group = ((uint64_t)disk_block - first_data_block) / blocks_per_group;
    uint64_t group_first_block = (uint64_t)first_data_block + (group * blocks_per_group);
    uint64_t bit = (uint64_t)disk_block - group_first_block;
    if (bit >= blocks_per_group) {
        return -22;
    }

    uint64_t group_desc_table = (block_size == 1024u ? 2u : 1u) * block_size;
    uint64_t desc_offset = group_desc_table + (group * desc_size);
    uint8_t group_desc[1024];
    if (active_bdev_read_exact(desc_offset, group_desc, desc_size) != 0)
    {
        return -5;
    }
    uint32_t bitmap_block = read_le32_fs(group_desc + KB_FS_EXT4_GROUP_DESC_BLOCK_BITMAP_LO_OFFSET);
    if (bitmap_block == 0) {
        return -5;
    }

    uint8_t *bitmap = calloc(1, (size_t)block_size);
    uint8_t *zero = calloc(1, (size_t)block_size);
    if (bitmap == NULL || zero == NULL) {
        free(zero);
        free(bitmap);
        return -12;
    }

    int status = 0;
    if (active_bdev_read_exact((uint64_t)bitmap_block * block_size, bitmap, (size_t)block_size) != 0)
    {
        status = -5;
        goto done;
    }

    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    uint8_t *byte = &bitmap[bit >> 3];
    if ((*byte & mask) == 0) {
        goto done;
    }
    *byte &= (uint8_t)~mask;
    if (active_bdev_write_exact((uint64_t)bitmap_block * block_size, bitmap, (size_t)block_size) != 0)
    {
        status = -5;
        goto done;
    }
    (void)active_bdev_write_exact((uint64_t)disk_block * block_size, zero, (size_t)block_size);

done:
    free(zero);
    free(bitmap);
    return status;
}

int kb_fs_subsystem_ext4_detach_inode_data_blocks(void *inode)
{
    if (inode == NULL) {
        return -22;
    }

    void *super_block = NULL;
    memcpy(&super_block, (const uint8_t *)inode + KB_FS_INODE_SB_OFFSET, sizeof(super_block));
    uint64_t block_size = 0;
    if (super_block != NULL) {
        memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    }
    if (super_block == NULL || block_size == 0) {
        return -22;
    }

    uint8_t *i_block = (uint8_t *)inode - KB_FS_INODE_EXT4_DIRECT_BLOCK0_BACK_OFFSET;
    uint16_t magic = read_le16_fs(i_block);
    uint16_t entries = read_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_ENTRIES_OFFSET);
    uint16_t depth = read_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_DEPTH_OFFSET);
    if (magic != KB_FS_EXT4_EXTENT_HEADER_MAGIC) {
        return 0;
    }
    if (depth != 0) {
        return -95;
    }
    if (entries > 4) {
        return -95;
    }

    int first_error = 0;
    for (uint16_t entry_index = 0; entry_index < entries; entry_index++) {
        uint8_t *extent = i_block + 0x0c + ((size_t)entry_index * KB_FS_EXT4_EXTENT_BYTES);
        uint16_t len = read_le16_fs(extent + KB_FS_EXT4_EXTENT_LEN_OFFSET) & 0x7fffu;
        uint32_t start_lo = read_le32_fs(extent + KB_FS_EXT4_EXTENT_START_LO_OFFSET);
        uint16_t start_hi = read_le16_fs(extent + KB_FS_EXT4_EXTENT_START_HI_OFFSET);
        uint64_t start = ((uint64_t)start_hi << 32) | start_lo;
        if (len == 0 || start == 0 || start > UINT32_MAX) {
            continue;
        }
        for (uint16_t block_index = 0; block_index < len; block_index++) {
            uint64_t disk_block = start + block_index;
            if (disk_block > UINT32_MAX) {
                if (first_error == 0) {
                    first_error = -34;
                }
                continue;
            }
            int status = ext4_free_direct_data_block(super_block, (uint32_t)disk_block, block_size);
            if (status != 0 && first_error == 0) {
                first_error = status;
            }
        }
    }

    write_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_ENTRIES_OFFSET, 0);
    write_u64_field(inode, KB_FS_INODE_SIZE_OFFSET, 0);
    write_u64_field(inode, KB_FS_INODE_BLOCKS_OFFSET, 0);
    int sync_status = kb_fs_subsystem_ext4_sync_super_free_blocks(super_block);
    if (first_error != 0) {
        return first_error;
    }
    return sync_status;
}

static int ext4_extent_record_block(const void *inode, uint64_t file_block, uint32_t disk_block)
{
    if (inode == NULL || file_block > UINT32_MAX || disk_block == 0) {
        return -22;
    }

    uint8_t *i_block = (uint8_t *)inode - KB_FS_INODE_EXT4_DIRECT_BLOCK0_BACK_OFFSET;
    uint16_t magic = read_le16_fs(i_block);
    uint16_t entries = read_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_ENTRIES_OFFSET);
    uint16_t max_entries = read_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_MAX_OFFSET);
    uint16_t depth = read_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_DEPTH_OFFSET);

    if (magic == 0 || magic != KB_FS_EXT4_EXTENT_HEADER_MAGIC) {
        memset(i_block, 0, 60);
        write_le16_fs(i_block, KB_FS_EXT4_EXTENT_HEADER_MAGIC);
        write_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_MAX_OFFSET, 4);
        magic = KB_FS_EXT4_EXTENT_HEADER_MAGIC;
        entries = 0;
        max_entries = 4;
        depth = 0;
    }
    if (magic != KB_FS_EXT4_EXTENT_HEADER_MAGIC || depth != 0) {
        return -95;
    }
    if (max_entries == 0 || max_entries > 4) {
        max_entries = 4;
        write_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_MAX_OFFSET, max_entries);
    }
    if (entries > max_entries) {
        return -5;
    }

    if (entries > 0) {
        uint8_t *last = i_block + 0x0c + ((size_t)(entries - 1u) * KB_FS_EXT4_EXTENT_BYTES);
        uint32_t last_block = read_le32_fs(last + KB_FS_EXT4_EXTENT_BLOCK_OFFSET);
        uint16_t last_len = read_le16_fs(last + KB_FS_EXT4_EXTENT_LEN_OFFSET) & 0x7fffu;
        uint32_t last_start_lo = read_le32_fs(last + KB_FS_EXT4_EXTENT_START_LO_OFFSET);
        uint16_t last_start_hi = read_le16_fs(last + KB_FS_EXT4_EXTENT_START_HI_OFFSET);
        uint64_t last_start = ((uint64_t)last_start_hi << 32) | last_start_lo;
        if ((uint32_t)file_block == last_block + last_len &&
            (uint64_t)disk_block == last_start + last_len &&
            last_len < 0x7fffu)
        {
            write_le16_fs(last + KB_FS_EXT4_EXTENT_LEN_OFFSET, (uint16_t)(last_len + 1u));
            return 0;
        }
    }

    if (entries >= max_entries) {
        return -28;
    }

    uint8_t *extent = i_block + 0x0c + ((size_t)entries * KB_FS_EXT4_EXTENT_BYTES);
    write_le32_fs(extent + KB_FS_EXT4_EXTENT_BLOCK_OFFSET, (uint32_t)file_block);
    write_le16_fs(extent + KB_FS_EXT4_EXTENT_LEN_OFFSET, 1);
    write_le16_fs(extent + KB_FS_EXT4_EXTENT_START_HI_OFFSET, (uint16_t)((uint64_t)disk_block >> 32));
    write_le32_fs(extent + KB_FS_EXT4_EXTENT_START_LO_OFFSET, disk_block);
    write_le16_fs(i_block + KB_FS_EXT4_EXTENT_HEADER_ENTRIES_OFFSET, (uint16_t)(entries + 1u));
    return 0;
}

static int ext4_allocate_data_block_for_inode(void *inode, uint64_t file_block, uint32_t *out_disk_block)
{
    if (inode == NULL || out_disk_block == NULL || file_block > UINT32_MAX) {
        return -22;
    }
    *out_disk_block = 0;
    if (active_bdev_binding.device == NULL ||
        active_bdev_binding.device->read == NULL ||
        active_bdev_binding.device->write == NULL)
    {
        return -5;
    }

    void *super_block = NULL;
    memcpy(&super_block, (const uint8_t *)inode + KB_FS_INODE_SB_OFFSET, sizeof(super_block));
    uint64_t block_size = 0;
    if (super_block != NULL) {
        memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    }
    if (block_size == 0 || block_size > SIZE_MAX || (block_size & (block_size - 1u)) != 0) {
        return -22;
    }

    uint8_t super_disk[1024];
    if (active_bdev_binding.device->read(
            active_bdev_binding.device->ctx,
            KB_FS_EXT4_SUPER_OFFSET,
            super_disk,
            sizeof(super_disk)) != 0)
    {
        return -5;
    }
    if (read_le16_fs(super_disk + KB_FS_EXT4_SUPER_MAGIC_OFFSET) != 0xef53u) {
        return -5;
    }

    uint32_t blocks_count = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_BLOCKS_COUNT_LO_OFFSET);
    uint32_t first_data_block = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_FIRST_DATA_BLOCK_OFFSET);
    uint32_t blocks_per_group = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_BLOCKS_PER_GROUP_OFFSET);
    uint32_t free_blocks = read_le32_fs(super_disk + KB_FS_EXT4_SUPER_FREE_BLOCKS_COUNT_LO_OFFSET);
    uint16_t desc_size = read_le16_fs(super_disk + KB_FS_EXT4_SUPER_DESC_SIZE_OFFSET);
    if (blocks_count == 0 || blocks_per_group == 0 || first_data_block >= blocks_count) {
        return -5;
    }
    if (desc_size < 32) {
        desc_size = 32;
    }
    if (desc_size > 1024) {
        return -5;
    }

    uint64_t groups64 = ((uint64_t)blocks_count - first_data_block + blocks_per_group - 1u) / blocks_per_group;
    if (groups64 == 0 || groups64 > 4096u) {
        return -5;
    }
    uint64_t group_desc_table = (block_size == 1024u ? 2u : 1u) * block_size;
    uint8_t group_desc[1024];
    uint8_t *bitmap = calloc(1, (size_t)block_size);
    uint8_t *zero = calloc(1, (size_t)block_size);
    if (bitmap == NULL || zero == NULL) {
        free(zero);
        free(bitmap);
        return -12;
    }

    int status = -28;
    for (uint64_t group = 0; group < groups64; group++) {
        uint64_t desc_offset = group_desc_table + (group * desc_size);
        if (active_bdev_binding.device->read(
                active_bdev_binding.device->ctx,
                desc_offset,
                group_desc,
                desc_size) != 0)
        {
            status = -5;
            break;
        }
        uint32_t bitmap_block = read_le32_fs(group_desc + KB_FS_EXT4_GROUP_DESC_BLOCK_BITMAP_LO_OFFSET);
        uint16_t group_free = read_le16_fs(group_desc + KB_FS_EXT4_GROUP_DESC_FREE_BLOCKS_COUNT_LO_OFFSET);
        if (bitmap_block == 0 || group_free == 0) {
            continue;
        }
        if (active_bdev_binding.device->read(
                active_bdev_binding.device->ctx,
                (uint64_t)bitmap_block * block_size,
                bitmap,
                (size_t)block_size) != 0)
        {
            status = -5;
            break;
        }

        uint64_t group_first_block = (uint64_t)first_data_block + (group * blocks_per_group);
        uint64_t group_block_count = blocks_per_group;
        if (group_first_block + group_block_count > blocks_count) {
            group_block_count = blocks_count - group_first_block;
        }
        for (uint64_t bit = 0; bit < group_block_count; bit++) {
            uint8_t mask = (uint8_t)(1u << (bit & 7u));
            if ((bitmap[bit >> 3] & mask) != 0) {
                continue;
            }

            uint64_t disk_block64 = group_first_block + bit;
            if (disk_block64 == 0 || disk_block64 > UINT32_MAX) {
                continue;
            }
            bitmap[bit >> 3] |= mask;
            if (active_bdev_binding.device->write(
                    active_bdev_binding.device->ctx,
                    (uint64_t)bitmap_block * block_size,
                    bitmap,
                    (size_t)block_size) != 0)
            {
                status = -5;
                goto done;
            }
            if (active_bdev_binding.device->write(
                    active_bdev_binding.device->ctx,
                    disk_block64 * block_size,
                    zero,
                    (size_t)block_size) != 0)
            {
                status = -5;
                goto done;
            }

            write_le16_fs(
                group_desc + KB_FS_EXT4_GROUP_DESC_FREE_BLOCKS_COUNT_LO_OFFSET,
                group_free == 0 ? 0 : (uint16_t)(group_free - 1u));
            (void)active_bdev_binding.device->write(
                active_bdev_binding.device->ctx,
                desc_offset,
                group_desc,
                desc_size);
            if (free_blocks > 0) {
                write_le32_fs(
                    super_disk + KB_FS_EXT4_SUPER_FREE_BLOCKS_COUNT_LO_OFFSET,
                    free_blocks - 1u);
                (void)active_bdev_binding.device->write(
                    active_bdev_binding.device->ctx,
                    KB_FS_EXT4_SUPER_OFFSET,
                    super_disk,
                    sizeof(super_disk));
            }

            status = ext4_extent_record_block(inode, file_block, (uint32_t)disk_block64);
            if (status == 0) {
                uint64_t blocks = 0;
                memcpy(&blocks, (const uint8_t *)inode + KB_FS_INODE_BLOCKS_OFFSET, sizeof(blocks));
                blocks += block_size / 512u;
                write_u64_field(inode, KB_FS_INODE_BLOCKS_OFFSET, blocks);
                *out_disk_block = (uint32_t)disk_block64;
            }
            goto done;
        }
    }

done:
    free(zero);
    free(bitmap);
    return status;
}

int kb_fs_subsystem_bmap(void *inode, uint64_t *block)
{
    if (inode == NULL || block == NULL) {
        return -22;
    }
    uint64_t logical = 0;
    memcpy(&logical, block, sizeof(logical));
    uint32_t mapped = ext4_extent_block_number(inode, logical);
    uint64_t mapped64 = mapped;
    memcpy(block, &mapped64, sizeof(mapped64));
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: bmap inode=%p block=%llu mapped=%u\n",
            inode,
            (unsigned long long)logical,
            mapped);
    }
    return 0;
}

long kb_fs_subsystem_generic_file_read_iter(void *kiocb, void *iter)
{
    if (kiocb == NULL || iter == NULL) {
        return -22;
    }
    void *file = NULL;
    void *inode = NULL;
    void *super_block = NULL;
    void *buffer = NULL;
    uint64_t pos = 0;
    uint64_t file_size = 0;
    uint64_t block_size = 0;
    uint64_t count = 0;
    uint64_t buffer_capacity = 0;
    memcpy(&file, (const uint8_t *)kiocb + KB_FS_KIOCB_FILE_OFFSET, sizeof(file));
    if (file == NULL) {
        return -22;
    }
    memcpy(&pos, (const uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, sizeof(pos));
    memcpy(&inode, (const uint8_t *)file + KB_FS_FILE_INODE_OFFSET, sizeof(inode));
    if (inode == NULL) {
        return -22;
    }
    memcpy(&super_block, (const uint8_t *)inode + KB_FS_INODE_SB_OFFSET, sizeof(super_block));
    memcpy(&file_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(file_size));
    memcpy(&block_size, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET, sizeof(block_size));
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    memcpy(&buffer, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, sizeof(buffer));
    memcpy(&buffer_capacity, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, sizeof(buffer_capacity));
    if (buffer == NULL || block_size == 0 || active_bdev_binding.device == NULL) {
        fprintf(stderr,
            "kobox-fs: generic_file_read_iter setup failed inode=%p buffer=%p block_size=%llu device=%p\n",
            inode,
            buffer,
            (unsigned long long)block_size,
            (void *)active_bdev_binding.device);
        return -5;
    }
    if (pos >= file_size || count == 0) {
        return 0;
    }
    uint64_t available = file_size - pos;
    uint64_t read_size = count < available ? count : available;
    if (read_size > SIZE_MAX) {
        return -22;
    }
    uint64_t total_read = 0;
    while (total_read < read_size) {
        uint64_t current_pos = pos + total_read;
        uint64_t file_block = current_pos / block_size;
        uint64_t block_offset = current_pos % block_size;
        uint64_t block_available = block_size - block_offset;
        uint64_t requested = read_size - total_read;
        uint64_t chunk = requested < block_available ? requested : block_available;
        uint32_t disk_block = ext4_extent_block_number(inode, file_block);
        if (chunk > SIZE_MAX) {
            break;
        }
        if (disk_block == 0) {
            memset((uint8_t *)buffer + total_read, 0, (size_t)chunk);
            if (fs_trace_enabled()) {
                fprintf(stderr,
                    "kobox-fs: generic_file_read_iter inode=%p file_block=%llu sparse-hole offset=%llu bytes=%llu\n",
                    inode,
                    (unsigned long long)file_block,
                    (unsigned long long)current_pos,
                    (unsigned long long)chunk);
            }
            total_read += chunk;
            continue;
        }
        if (block_offset == 0 && requested >= block_size) {
            uint64_t max_blocks = requested / block_size;
            uint64_t max_bulk_blocks = (128u * 1024u) / block_size;
            if (max_bulk_blocks == 0) {
                max_bulk_blocks = 1;
            }
            if (max_blocks > max_bulk_blocks) {
                max_blocks = max_bulk_blocks;
            }
            uint64_t run_blocks = 1;
            while (run_blocks < max_blocks) {
                uint32_t next_disk_block = ext4_extent_block_number(inode, file_block + run_blocks);
                if (next_disk_block != disk_block + run_blocks) {
                    break;
                }
                run_blocks++;
            }
            chunk = run_blocks * block_size;
            if (chunk > SIZE_MAX) {
                break;
            }
        }
        uint64_t disk_offset = ((uint64_t)disk_block * block_size) + block_offset;
        uint64_t submit_offset = disk_offset;
        size_t submit_size = (size_t)chunk;
        void *submit_buffer = (uint8_t *)buffer + total_read;
        int read_status = 0;
        if (block_offset == 0 && chunk == block_size) {
            read_status = active_bdev_binding.device->read(active_bdev_binding.device->ctx,
                    disk_offset,
                    submit_buffer,
                    submit_size);
        } else {
            if (block_size > SIZE_MAX || block_size > 1024u * 1024u) {
                break;
            }
            submit_offset = (uint64_t)disk_block * block_size;
            submit_size = (size_t)block_size;
            const int can_use_caller_bounce =
                buffer_capacity >= total_read &&
                buffer_capacity - total_read >= block_size;
            uint8_t *block_buffer = NULL;
            if (can_use_caller_bounce) {
                block_buffer = (uint8_t *)buffer + total_read;
            } else {
                block_buffer = malloc((size_t)block_size);
                if (block_buffer == NULL) {
                    break;
                }
            }
            submit_buffer = block_buffer;
            read_status = active_bdev_binding.device->read(active_bdev_binding.device->ctx,
                    submit_offset,
                    submit_buffer,
                    submit_size);
            if (read_status == 0) {
                memmove((uint8_t *)buffer + total_read, block_buffer + block_offset, (size_t)chunk);
            }
            if (!can_use_caller_bounce) {
                free(block_buffer);
            }
        }
        if (read_status != 0)
        {
            fprintf(stderr,
                "kobox-fs: read block failed inode=%p file_block=%llu disk_block=%u offset=%llu bytes=%llu status=%d\n",
                inode,
                (unsigned long long)file_block,
                disk_block,
                (unsigned long long)submit_offset,
                (unsigned long long)chunk,
                read_status);
            break;
        }
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: generic_file_read_iter inode=%p file_block=%llu disk_block=%u offset=%llu bytes=%llu\n",
                inode,
                (unsigned long long)file_block,
                disk_block,
                (unsigned long long)disk_offset,
                (unsigned long long)chunk);
        }
        total_read += chunk;
    }
    if (total_read == 0) {
        fprintf(stderr,
            "kobox-fs: generic_file_read_iter no data inode=%p pos=%llu count=%llu file_size=%llu block_size=%llu\n",
            inode,
            (unsigned long long)pos,
            (unsigned long long)count,
            (unsigned long long)file_size,
            (unsigned long long)block_size);
        return -5;
    }
    pos += total_read;
    count -= total_read;
    memcpy((uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, &pos, sizeof(pos));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, &count, sizeof(count));
    return (long)total_read;
}

long kb_fs_subsystem_generic_write_checks(void *kiocb, void *iter)
{
    (void)kiocb;
    if (iter == NULL) {
        return -22;
    }
    uint64_t count = 0;
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    if (count > (uint64_t)LONG_MAX) {
        return -22;
    }
    return (long)count;
}

long kb_fs_subsystem_generic_perform_write(void *kiocb, void *iter)
{
    if (kiocb == NULL || iter == NULL) {
        return -22;
    }
    void *file = NULL;
    void *mapping = NULL;
    void *a_ops = NULL;
    void *buffer = NULL;
    uint64_t pos = 0;
    uint64_t count = 0;
    memcpy(&file, (const uint8_t *)kiocb + KB_FS_KIOCB_FILE_OFFSET, sizeof(file));
    if (file == NULL) {
        return -22;
    }
    memcpy(&pos, (const uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, sizeof(pos));
    memcpy(&mapping, (const uint8_t *)file + KB_FS_FILE_MAPPING_OFFSET, sizeof(mapping));
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    memcpy(&buffer, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, sizeof(buffer));
    if (mapping == NULL || buffer == NULL) {
        return -22;
    }
    if (count == 0) {
        return 0;
    }
    if (pos > (uint64_t)INT64_MAX || count > (uint64_t)LONG_MAX) {
        return -22;
    }
    memcpy(&a_ops, (const uint8_t *)mapping + KB_FS_ADDRESS_SPACE_AOPS_OFFSET, sizeof(a_ops));
    if (a_ops == NULL) {
        return -95;
    }

    int (*write_begin_fn)(void *, void *, int64_t, unsigned int, void **, void **) = NULL;
    int (*write_end_fn)(void *, void *, int64_t, unsigned int, unsigned int, void *, void *) = NULL;
    memcpy(&write_begin_fn, (const uint8_t *)a_ops + KB_FS_ADDRESS_SPACE_OP_WRITE_BEGIN_OFFSET, sizeof(write_begin_fn));
    memcpy(&write_end_fn, (const uint8_t *)a_ops + KB_FS_ADDRESS_SPACE_OP_WRITE_END_OFFSET, sizeof(write_end_fn));
    if (write_begin_fn == NULL || write_end_fn == NULL) {
        return -95;
    }

    uint64_t written = 0;
    while (count != 0) {
        const uint64_t page_offset = pos & (KB_FS_PAGE_SIZE - 1u);
        uint64_t bytes64 = KB_FS_PAGE_SIZE - page_offset;
        if (bytes64 > count) {
            bytes64 = count;
        }
        if (bytes64 > UINT_MAX || pos > (uint64_t)INT64_MAX) {
            return written != 0 ? (long)written : -22;
        }
        const unsigned int bytes = (unsigned int)bytes64;
        void *page = NULL;
        void *fsdata = NULL;

        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call((void *)write_begin_fn, &old_gs);
        int status = write_begin_fn(file, mapping, (int64_t)pos, bytes, &page, &fsdata);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status < 0) {
            return written != 0 ? (long)written : status;
        }

        void *payload = folio_page_payload(page);
        if (payload == NULL) {
            return written != 0 ? (long)written : -12;
        }
        memcpy((uint8_t *)payload + page_offset, (const uint8_t *)buffer + written, bytes);

        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)write_end_fn, &old_gs);
        status = write_end_fn(file, mapping, (int64_t)pos, bytes, bytes, page, fsdata);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status < 0) {
            return written != 0 ? (long)written : status;
        }
        if (status == 0) {
            break;
        }
        if ((uint64_t)status > count) {
            status = (int)count;
        }
        if ((uint64_t)status > bytes64) {
            status = (int)bytes64;
        }
        uint64_t advanced = (uint64_t)status;
        if (pos + advanced < pos || written + advanced < written) {
            return written != 0 ? (long)written : -75;
        }
        pos += advanced;
        written += advanced;
        count -= advanced;
    }

    memcpy((uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, &pos, sizeof(pos));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, &count, sizeof(count));
    return written != 0 ? (long)written : -5;
}

void kb_fs_subsystem_iget_failed(void *inode)
{
    if (fs_trace_enabled()) {
        uint16_t mode = 0;
        uint32_t nlink = 0;
        uint64_t size = 0;
        uint64_t blocks = 0;
        uint64_t state = 0;
        if (!low_or_err_pointer(inode)) {
            memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
            memcpy(&nlink, (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, sizeof(nlink));
            memcpy(&size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(size));
            memcpy(&blocks, (const uint8_t *)inode + KB_FS_INODE_BLOCKS_OFFSET, sizeof(blocks));
            memcpy(&state, (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET, sizeof(state));
        }
        fprintf(stderr,
            "kobox-fs: iget_failed inode=%p mode=0%o nlink=%u size=%llu blocks=%llu state=0x%llx\n",
            inode,
            mode,
            nlink,
            (unsigned long long)size,
            (unsigned long long)blocks,
            (unsigned long long)state);
    }
}

void kb_fs_subsystem_unlock_new_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET, sizeof(state));
    state &= ~KB_FS_INODE_STATE_NEW;
    write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, state);
    if (fs_trace_enabled()) {
        uint16_t mode = 0;
        uint32_t nlink = 0;
        uint64_t size = 0;
        uint64_t blocks = 0;
        memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
        memcpy(&nlink, (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, sizeof(nlink));
        memcpy(&size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(size));
        memcpy(&blocks, (const uint8_t *)inode + KB_FS_INODE_BLOCKS_OFFSET, sizeof(blocks));
        fprintf(stderr,
            "kobox-fs: unlock_new_inode inode=%p mode=0%o nlink=%u size=%llu blocks=%llu state=0x%llx\n",
            inode,
            mode,
            nlink,
            (unsigned long long)size,
            (unsigned long long)blocks,
            (unsigned long long)state);
    }
}

void kb_fs_subsystem_set_nlink(void *inode, unsigned int nlink)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    memcpy((uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, &nlink, sizeof(nlink));
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: set_nlink inode=%p nlink=%u\n", inode, nlink);
    }
}

void kb_fs_subsystem_clear_nlink(void *inode)
{
    kb_fs_subsystem_set_nlink(inode, 0);
}

void kb_fs_subsystem_drop_nlink(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint32_t nlink = 0;
    memcpy(&nlink, (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, sizeof(nlink));
    if (nlink != 0) {
        nlink--;
        memcpy((uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, &nlink, sizeof(nlink));
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: drop_nlink inode=%p nlink=%u\n", inode, nlink);
    }
}

void kb_fs_subsystem_inc_nlink(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint32_t nlink = 0;
    memcpy(&nlink, (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, sizeof(nlink));
    if (nlink != UINT32_MAX) {
        nlink++;
        memcpy((uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET, &nlink, sizeof(nlink));
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: inc_nlink inode=%p nlink=%u\n", inode, nlink);
    }
}

void kb_fs_subsystem_mark_inode_freeing(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET, sizeof(state));
    state |= KB_FS_INODE_STATE_FREEING;
    write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, state);
}

int kb_fs_subsystem_dquot_alloc_space(void *inode, uint64_t bytes, int flags)
{
    (void)flags;
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    uint64_t blocks = 0;
    memcpy(&blocks, (const uint8_t *)inode + KB_FS_INODE_BLOCKS_OFFSET, sizeof(blocks));
    uint64_t sectors = (bytes + 511u) / 512u;
    if (UINT64_MAX - blocks < sectors) {
        return -75;
    }
    blocks += sectors;
    write_u64_field(inode, KB_FS_INODE_BLOCKS_OFFSET, blocks);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: dquot_alloc_space inode=%p bytes=%llu blocks=%llu\n",
            inode,
            (unsigned long long)bytes,
            (unsigned long long)blocks);
    }
    return 0;
}

void kb_fs_subsystem_dquot_free_space(void *inode, uint64_t bytes, int flags)
{
    (void)flags;
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint64_t blocks = 0;
    memcpy(&blocks, (const uint8_t *)inode + KB_FS_INODE_BLOCKS_OFFSET, sizeof(blocks));
    uint64_t sectors = (bytes + 511u) / 512u;
    blocks = blocks > sectors ? blocks - sectors : 0;
    write_u64_field(inode, KB_FS_INODE_BLOCKS_OFFSET, blocks);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: dquot_free_space inode=%p bytes=%llu blocks=%llu\n",
            inode,
            (unsigned long long)bytes,
            (unsigned long long)blocks);
    }
}

int kb_fs_subsystem_get_tree_bdev(void *fs_context, int (*fill_super)(void *super_block, void *fs_context))
{
    last_mount_path_probe.get_tree_bdev_calls++;
    last_mount_path_probe.get_tree_bdev_fc = fs_context;
    last_mount_path_probe.get_tree_bdev_fill_super = (void *)fill_super;
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: get_tree_bdev fc=%p fill_super=%p calls=%llu\n",
            fs_context,
            (void *)fill_super,
            (unsigned long long)last_mount_path_probe.get_tree_bdev_calls);
    }
    if (mount_probe_block_device != NULL && fill_super != NULL) {
        clear_mount_probe_objects();
        void *super_block = calloc(1, KB_FS_SUPER_BLOCK_BYTES);
        void *bdev = calloc(1, KB_FS_FAKE_BDEV_BYTES);
        void *bdev_inode = calloc(1, KB_FS_FAKE_BDEV_INODE_BYTES);
        void *bdev_mapping = calloc(1, KB_FS_FAKE_ADDRESS_SPACE_BYTES);
        void *disk = calloc(1, KB_FS_FAKE_DISK_BYTES);
        void *bdev_stats = calloc(1, KB_FS_FAKE_BDEV_STATS_BYTES);
        void *queue = calloc(1, KB_FS_FAKE_QUEUE_BYTES);
        if (super_block == NULL || bdev == NULL || bdev_inode == NULL || bdev_mapping == NULL || disk == NULL || bdev_stats == NULL || queue == NULL) {
            free(super_block);
            free(bdev);
            free(bdev_inode);
            free(bdev_mapping);
            free(disk);
            free(bdev_stats);
            free(queue);
            return -12;
        }
        mount_probe_super_block = super_block;
        mount_probe_bdev = bdev;
        mount_probe_bdev_inode = bdev_inode;
        mount_probe_bdev_mapping = bdev_mapping;
        mount_probe_disk = disk;
        mount_probe_bdev_stats = bdev_stats;
        mount_probe_queue = queue;
        last_mount_path_probe.super_block = super_block;
        last_mount_path_probe.block_device = bdev;
        set_super_blocksize(super_block, 1024u);
        write_pointer_field(super_block, KB_FS_SUPER_BLOCK_BDEV_OFFSET, bdev);
        write_u64_field(bdev, KB_FS_BDEV_SECTOR_COUNT_OFFSET, mount_probe_block_device->size_bytes / 512u);
        write_pointer_field(bdev, KB_FS_BDEV_INODE_OFFSET, bdev_inode);
        write_pointer_field(bdev, KB_FS_BDEV_DISK_OFFSET, disk);
        write_pointer_field(bdev_inode, KB_FS_INODE_SB_OFFSET, kb_fs_subsystem_blockdev_superblock);
        write_pointer_field(bdev_mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET, bdev_inode);
        write_pointer_field(bdev_inode, KB_FS_ADDRESS_SPACE_OFFSET, bdev_mapping);
        write_pointer_field(bdev, KB_FS_BDEV_QUEUE_OFFSET, queue);
        active_bdev_binding.bdev = bdev;
        active_bdev_binding.device = mount_probe_block_device;
        memcpy((uint8_t *)super_block + KB_FS_SUPER_BLOCK_DEVNAME_OFFSET,
            "kobox-block-image",
            sizeof("kobox-block-image"));
        unsigned long old_gs = 0;
        unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)fill_super);
        void *percpu_bdev_stats = bdev_stats;
        if (kernel_gs != 0) {
            percpu_bdev_stats = (void *)((uintptr_t)bdev_stats - (uintptr_t)kernel_gs);
        }
        write_pointer_field(bdev, KB_FS_BDEV_STATS_OFFSET, percpu_bdev_stats);
        int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: calling fill_super super=%p fc=%p bdev=%p queue=%p bdev_stats=%p percpu_bdev_stats=%p kernel_gs=0x%lx has_gs=%d\n",
                super_block,
                fs_context,
                bdev,
                queue,
                bdev_stats,
                percpu_bdev_stats,
                kernel_gs,
                has_gs);
        }
        last_mount_path_probe.fill_super_result = kb_linux_call_int_ptr_ptr_raw(fill_super, super_block, fs_context);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (last_mount_path_probe.fill_super_result == 0) {
            ext4_seed_extent_status_stats(super_block);
        }
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: fill_super result=%d bdev_getblk_calls=%llu last_block=%llu block_size=%u magic=0x%04x\n",
                last_mount_path_probe.fill_super_result,
                (unsigned long long)last_mount_path_probe.bdev_getblk_calls,
                (unsigned long long)last_mount_path_probe.last_block_number,
                last_mount_path_probe.last_block_size,
                last_mount_path_probe.observed_ext4_magic);
        }
    }
    return -95;
}

int kb_fs_subsystem_probe_registered_mount_path(const char *name, kb_fs_mount_path_probe_t *out_probe)
{
    if (name == NULL || out_probe == NULL) {
        return -22;
    }
    kb_fs_type_record_t *record = find_fs_type_by_name(name);
    if (record == NULL || low_or_err_pointer(record->fs_type)) {
        return -2;
    }

    memset(&last_mount_path_probe, 0, sizeof(last_mount_path_probe));
    last_mount_path_probe.fs_type = record->fs_type;

    int (*init_fs_context)(void *) =
        (int (*)(void *))read_pointer_field(record->fs_type, KB_FS_TYPE_INIT_FS_CONTEXT_OFFSET);
    if (init_fs_context == NULL) {
        *out_probe = last_mount_path_probe;
        return -95;
    }
    last_mount_path_probe.init_fs_context = (void *)init_fs_context;

    void *fs_context = calloc(1, KB_FS_CONTEXT_BYTES);
    if (fs_context == NULL) {
        return -12;
    }
    last_mount_path_probe.fs_context = fs_context;

    int init_result = kb_linux_call_int_ptr(init_fs_context, fs_context);
    last_mount_path_probe.init_result = init_result;
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: after init_fs_context fc=%p fs_private=%p\n",
            fs_context,
            read_pointer_field(fs_context, 0x30));
    }
    if (init_result != 0) {
        *out_probe = last_mount_path_probe;
        free(fs_context);
        last_mount_path_probe.fs_context = NULL;
        return init_result;
    }

    void *ops = read_pointer_field(fs_context, KB_FS_CONTEXT_OPS_OFFSET);
    last_mount_path_probe.fs_context_ops = ops;
    int (*get_tree)(void *) =
        (int (*)(void *))read_pointer_field(ops, KB_FS_CONTEXT_OPS_GET_TREE_OFFSET);
    last_mount_path_probe.get_tree = (void *)get_tree;
    if (get_tree == NULL) {
        *out_probe = last_mount_path_probe;
        free(fs_context);
        last_mount_path_probe.fs_context = NULL;
        return -95;
    }

    int get_tree_result = kb_linux_call_int_ptr(get_tree, fs_context);
    last_mount_path_probe.get_tree_result = get_tree_result;
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: mount-path fs=%s fs_type=%p init=%p init_result=%d ops=%p get_tree=%p get_tree_result=%d\n",
            record->name,
            record->fs_type,
            (void *)init_fs_context,
            init_result,
            ops,
            (void *)get_tree,
            get_tree_result);
    }

    *out_probe = last_mount_path_probe;
    free(fs_context);
    last_mount_path_probe.fs_context = NULL;
    return get_tree_result;
}

int kb_fs_subsystem_mount_registered_root(const char *name, kb_fs_mount_result_t *out_mount)
{
    return kb_fs_subsystem_probe_registered_mount_path(name, out_mount);
}

int kb_fs_subsystem_run_ext4_image_smoke(
    const char *image_path,
    unsigned long inode_number,
    unsigned long large_inode_number,
    unsigned long ldlike_inode_number,
    unsigned long zero_inode_number)
{
    if (image_path == NULL || image_path[0] == '\0' || inode_number == 0) {
        return -22;
    }

    kb_fs_block_device_t *device = NULL;
    int status = kb_fs_block_device_create_image("kobox-ext4-smoke", image_path, &device);
    if (status != 0) {
        fprintf(stderr, "kobox-ext4-smoke: image open failed status=%d path=%s\n", status, image_path);
        return status;
    }

    kb_fs_mount_result_t mount;
    memset(&mount, 0, sizeof(mount));
    status = kb_fs_subsystem_set_mount_probe_block_device(device);
    if (status == 0) {
        status = kb_fs_subsystem_mount_registered_root("ext4", &mount);
    }
    if (mount.fill_super_result != 0 || mount.super_block == NULL) {
        fprintf(stderr,
            "kobox-ext4-smoke: probe failed status=%d fill_super=%d get_tree=%d magic=0x%04x reads=%u\n",
            status,
            mount.fill_super_result,
            mount.get_tree_result,
            mount.observed_ext4_magic,
            mount.block_read_count);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return mount.fill_super_result != 0 ? mount.fill_super_result : -5;
    }
    fprintf(stderr,
        "kobox-ext4-smoke: probe ok magic=0x%04x reads=%u inode=%lu\n",
        mount.observed_ext4_magic,
        mount.block_read_count,
        inode_number);

    typedef void *(*ext4_iget_fn)(void *, unsigned long, unsigned int, const char *, unsigned int);
    ext4_iget_fn ext4_iget = (ext4_iget_fn)kb_module_lookup_exported_symbol("__ext4_iget");
    if (ext4_iget == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: __ext4_iget missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)ext4_iget);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    void *inode = ext4_iget(mount.super_block, inode_number, 0, "kobox_ext4_smoke", 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (low_or_err_pointer(inode)) {
        fprintf(stderr, "kobox-ext4-smoke: iget failed inode=%lu ptr=%p\n", inode_number, inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }

    uint8_t file[KB_FS_FAKE_INODE_BYTES];
    uint8_t kiocb[64];
    uint8_t iter[64];
    uint8_t original[64];
    uint8_t readback[64];
    const uint8_t payload[] = "kobox-host-write";
    const uint8_t append_payload[] = "+append-noalloc";
    const uint64_t payload_len = sizeof(payload) - 1u;
    const uint64_t append_payload_len = sizeof(append_payload) - 1u;
    uint64_t pos = 0;
    uint64_t len = sizeof(original);
    uint64_t original_size = 0;
    memset(file, 0, sizeof(file));
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    memset(original, 0, sizeof(original));
    memset(readback, 0, sizeof(readback));
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, original);

    long read_result = kb_fs_subsystem_generic_file_read_iter(kiocb, iter);
    if (read_result <= 0) {
        fprintf(stderr, "kobox-ext4-smoke: read failed result=%ld\n", read_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return read_result == 0 ? -5 : (int)read_result;
    }
    fprintf(stderr, "kobox-ext4-smoke: read ok bytes=%ld\n", read_result);
    memcpy(&original_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(original_size));
    if (original_size == 0) {
        fprintf(stderr, "kobox-ext4-smoke: inode size is zero\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }

    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    pos = 0;
    len = payload_len;
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, (void *)payload);
    long write_result = kb_fs_subsystem_generic_perform_write(kiocb, iter);
    if (write_result != (long)payload_len) {
        fprintf(stderr, "kobox-ext4-smoke: write failed result=%ld expected=%llu\n",
            write_result,
            (unsigned long long)payload_len);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return write_result < 0 ? (int)write_result : -5;
    }
    fprintf(stderr, "kobox-ext4-smoke: write ok bytes=%ld\n", write_result);

    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    pos = 0;
    len = payload_len;
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, readback);
    read_result = kb_fs_subsystem_generic_file_read_iter(kiocb, iter);
    if (read_result != (long)payload_len || memcmp(readback, payload, (size_t)payload_len) != 0) {
        fprintf(stderr, "kobox-ext4-smoke: readback failed result=%ld\n", read_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }

    fprintf(stderr, "kobox-ext4-smoke: readback ok bytes=%ld\n", read_result);

    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    pos = original_size;
    len = append_payload_len;
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, (void *)append_payload);
    write_result = kb_fs_subsystem_generic_perform_write(kiocb, iter);
    uint64_t appended_size = 0;
    memcpy(&appended_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(appended_size));
    if (write_result != (long)append_payload_len || appended_size != original_size + append_payload_len) {
        fprintf(stderr,
            "kobox-ext4-smoke: append-noalloc failed result=%ld size=%llu expected_size=%llu\n",
            write_result,
            (unsigned long long)appended_size,
            (unsigned long long)(original_size + append_payload_len));
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return write_result < 0 ? (int)write_result : -5;
    }

    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    memset(readback, 0, sizeof(readback));
    pos = original_size;
    len = append_payload_len;
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, readback);
    read_result = kb_fs_subsystem_generic_file_read_iter(kiocb, iter);
    if (read_result != (long)append_payload_len ||
        memcmp(readback, append_payload, (size_t)append_payload_len) != 0)
    {
        fprintf(stderr, "kobox-ext4-smoke: append-noalloc readback failed result=%ld\n", read_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }

    fprintf(stderr,
        "kobox-ext4-smoke: append-noalloc ok bytes=%ld size=%llu\n",
        read_result,
        (unsigned long long)appended_size);

    if (ldlike_inode_number != 0) {
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_iget, &old_gs);
        void *ldlike_inode = ext4_iget(mount.super_block, ldlike_inode_number, 0, "kobox_ext4_ldlike", 0);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (low_or_err_pointer(ldlike_inode)) {
            fprintf(stderr, "kobox-ext4-smoke: ldlike iget failed inode=%lu ptr=%p\n", ldlike_inode_number, ldlike_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }

        uint8_t ldlike_file[KB_FS_FAKE_INODE_BYTES];
        uint8_t ldlike_kiocb[64];
        uint8_t ldlike_iter[64];
        uint8_t ldlike_readback[4096];
        uint64_t ldlike_size = 0;
        memcpy(&ldlike_size, (const uint8_t *)ldlike_inode + KB_FS_INODE_SIZE_OFFSET, sizeof(ldlike_size));
        if (ldlike_size == 0) {
            fprintf(stderr, "kobox-ext4-smoke: ldlike inode size is zero inode=%lu\n", ldlike_inode_number);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }
        const uint64_t ldlike_deep_offset =
            ldlike_size >= 761856u ? 757760u : 0u;
        const size_t ldlike_deep_length =
            ldlike_size - ldlike_deep_offset >= sizeof(ldlike_readback) ?
                sizeof(ldlike_readback) :
                (size_t)(ldlike_size - ldlike_deep_offset);
        memset(ldlike_file, 0, sizeof(ldlike_file));
        memset(ldlike_kiocb, 0, sizeof(ldlike_kiocb));
        memset(ldlike_iter, 0, sizeof(ldlike_iter));
        memset(ldlike_readback, 0, sizeof(ldlike_readback));
        write_pointer_field(ldlike_file, KB_FS_FILE_INODE_OFFSET, ldlike_inode);
        write_pointer_field(ldlike_kiocb, KB_FS_KIOCB_FILE_OFFSET, ldlike_file);
        write_u64_field(ldlike_kiocb, KB_FS_KIOCB_POS_OFFSET, ldlike_deep_offset);
        write_u64_field(ldlike_iter, KB_FS_IOV_ITER_COUNT_OFFSET, ldlike_deep_length);
        write_pointer_field(ldlike_iter, KB_FS_IOV_ITER_BUFFER_OFFSET, ldlike_readback);

        read_result = kb_fs_subsystem_generic_file_read_iter(ldlike_kiocb, ldlike_iter);
        if (read_result != (long)ldlike_deep_length) {
            fprintf(stderr,
                "kobox-ext4-smoke: ldlike read failed inode=%lu offset=%llu result=%ld expected=%zu\n",
                ldlike_inode_number,
                (unsigned long long)ldlike_deep_offset,
                read_result,
                ldlike_deep_length);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return read_result < 0 ? (int)read_result : -5;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: ldlike read ok inode=%lu offset=%llu bytes=%ld\n",
            ldlike_inode_number,
            (unsigned long long)ldlike_deep_offset,
            read_result);

        typedef long (*ext4_file_read_iter_fn)(void *, void *);
        ext4_file_read_iter_fn ext4_file_read_iter = NULL;
        void *ext4_file_read_iter_addr = NULL;
        if (kb_module_find_symbol(kb_loader_active_module(), "ext4_file_read_iter", &ext4_file_read_iter_addr) != KB_OK ||
            ext4_file_read_iter_addr == NULL)
        {
            fprintf(stderr, "kobox-ext4-smoke: ext4_file_read_iter missing\n");
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -95;
        }
        memcpy(&ext4_file_read_iter, &ext4_file_read_iter_addr, sizeof(ext4_file_read_iter));

        uint8_t ext4_file[512];
        uint8_t ext4_kiocb[128];
        uint8_t ext4_iter[128];
        uint8_t ext4_readback[4096];
        void *ldlike_mapping = read_pointer_field(ldlike_inode, KB_FS_INODE_MAPPING_OFFSET);
        memset(ext4_file, 0, sizeof(ext4_file));
        if (ldlike_mapping == NULL) {
            fprintf(stderr, "kobox-ext4-smoke: ldlike mapping missing inode=%lu\n", ldlike_inode_number);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }
        write_pointer_field(ext4_file, KB_FS_FILE_MAPPING_OFFSET, ldlike_mapping);
        write_pointer_field(ext4_file, KB_FS_FILE_INODE_OFFSET, ldlike_inode);

        const uint64_t ext4_limit = ldlike_size >= 761856u ? 761856u : ldlike_size;
        for (uint64_t ext4_offset = 0; ext4_offset < ext4_limit; ext4_offset += sizeof(ext4_readback)) {
            const size_t ext4_want =
                ext4_limit - ext4_offset >= sizeof(ext4_readback) ?
                    sizeof(ext4_readback) :
                    (size_t)(ext4_limit - ext4_offset);
            memset(ext4_kiocb, 0, sizeof(ext4_kiocb));
            memset(ext4_iter, 0, sizeof(ext4_iter));
            memset(ext4_readback, 0, sizeof(ext4_readback));
            write_pointer_field(ext4_kiocb, KB_FS_KIOCB_FILE_OFFSET, ext4_file);
            write_u64_field(ext4_kiocb, KB_FS_KIOCB_POS_OFFSET, ext4_offset);
            write_u64_field(ext4_iter, KB_FS_IOV_ITER_COUNT_OFFSET, ext4_want);
            write_pointer_field(ext4_iter, KB_FS_IOV_ITER_BUFFER_OFFSET, ext4_readback);

            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call((void *)ext4_file_read_iter, &old_gs);
            read_result = ext4_file_read_iter(ext4_kiocb, ext4_iter);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (read_result != (long)ext4_want) {
                fprintf(stderr,
                    "kobox-ext4-smoke: ldlike ext4_file_read_iter failed inode=%lu offset=%llu result=%ld expected=%zu\n",
                    ldlike_inode_number,
                    (unsigned long long)ext4_offset,
                    read_result,
                    ext4_want);
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return read_result < 0 ? (int)read_result : -5;
            }
        }
        fprintf(stderr,
            "kobox-ext4-smoke: ldlike ext4_file_read_iter sequential ok inode=%lu bytes=%llu\n",
            ldlike_inode_number,
            (unsigned long long)ext4_limit);
    }

    typedef void (*ext4_dirty_inode_fn)(void *, int);
    ext4_dirty_inode_fn ext4_dirty_inode =
        (ext4_dirty_inode_fn)kb_module_lookup_exported_symbol("ext4_dirty_inode");
    if (ext4_dirty_inode == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: ext4_dirty_inode missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    typedef int (*ext4_write_inode_fn)(void *, void *);
    ext4_write_inode_fn ext4_write_inode =
        (ext4_write_inode_fn)kb_module_lookup_exported_symbol("ext4_write_inode");
    if (ext4_write_inode == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: ext4_write_inode missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    typedef int (*ext4_force_commit_fn)(void *);
    ext4_force_commit_fn ext4_force_commit =
        (ext4_force_commit_fn)kb_module_lookup_exported_symbol("ext4_force_commit");
    if (ext4_force_commit == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: ext4_force_commit missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    typedef int (*ext4_truncate_fn)(void *);
    ext4_truncate_fn ext4_truncate =
        (ext4_truncate_fn)kb_module_lookup_exported_symbol("ext4_truncate");
    if (ext4_truncate == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: ext4_truncate missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    typedef void (*ext4_evict_inode_fn)(void *);
    ext4_evict_inode_fn ext4_evict_inode =
        (ext4_evict_inode_fn)kb_module_lookup_exported_symbol("ext4_evict_inode");
    if (ext4_evict_inode == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: ext4_evict_inode missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    kb_fs_ext4_size_sync_ops_t size_sync_ops;
    size_sync_ops.dirty_inode = ext4_dirty_inode;
    size_sync_ops.write_inode = ext4_write_inode;
    size_sync_ops.force_commit = ext4_force_commit;
    size_sync_ops.truncate_inode = ext4_truncate;

    int group_sync_result = kb_fs_subsystem_ext4_sync_group_free_counts(mount.super_block);
    if (group_sync_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: group free count sync failed result=%d\n", group_sync_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return group_sync_result;
    }

    const uint64_t truncated_size = 8;
    int truncate_result = kb_fs_ext4_sync_inode_size(
        mount.super_block,
        inode,
        truncated_size,
        &size_sync_ops,
        "truncate",
        0);
    if (truncate_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return truncate_result;
    }
    fprintf(stderr, "kobox-ext4-smoke: truncate ok size=%llu\n", (unsigned long long)truncated_size);

    if (large_inode_number != 0) {
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_iget, &old_gs);
        void *large_inode = ext4_iget(mount.super_block, large_inode_number, 0, "kobox_ext4_large", 0);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (low_or_err_pointer(large_inode)) {
            fprintf(stderr, "kobox-ext4-smoke: large iget failed inode=%lu ptr=%p\n", large_inode_number, large_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }
        const uint64_t large_truncated_size = 4096;
        truncate_result = kb_fs_ext4_sync_inode_size(
            mount.super_block,
            large_inode,
            large_truncated_size,
            &size_sync_ops,
            "truncate-large",
            1);
        if (truncate_result != 0) {
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return truncate_result;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: truncate-large ok inode=%lu size=%llu\n",
            large_inode_number,
            (unsigned long long)large_truncated_size);
    }

    if (zero_inode_number != 0) {
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_iget, &old_gs);
        void *zero_inode = ext4_iget(mount.super_block, zero_inode_number, 0, "kobox_ext4_zero", 0);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (low_or_err_pointer(zero_inode)) {
            fprintf(stderr, "kobox-ext4-smoke: zero iget failed inode=%lu ptr=%p\n", zero_inode_number, zero_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }
        truncate_result = kb_fs_ext4_sync_inode_size(
            mount.super_block,
            zero_inode,
            0,
            &size_sync_ops,
            "truncate-zero",
            1);
        if (truncate_result != 0) {
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return truncate_result;
        }
        fprintf(stderr, "kobox-ext4-smoke: truncate-zero ok inode=%lu size=0\n", zero_inode_number);
    }

    void *root_inode = mount.root_inode;
    void *root_dentry = mount.root_dentry;
    void *root_iop = read_pointer_field(root_inode, KB_FS_INODE_OP_OFFSET);
    void *create_op = read_pointer_field(root_iop, KB_FS_INODE_OP_CREATE_OFFSET);
    void *rename_op = read_pointer_field(root_iop, KB_FS_INODE_OP_RENAME_OFFSET);
    void *unlink_op = read_pointer_field(root_iop, KB_FS_INODE_OP_UNLINK_OFFSET);
    void *mkdir_op = read_pointer_field(root_iop, KB_FS_INODE_OP_MKDIR_OFFSET);
    void *rmdir_op = read_pointer_field(root_iop, KB_FS_INODE_OP_RMDIR_OFFSET);
    if (root_inode == NULL || root_dentry == NULL || create_op == NULL || rename_op == NULL ||
        unlink_op == NULL || mkdir_op == NULL || rmdir_op == NULL)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: inode ops missing root=%p dentry=%p create=%p rename=%p unlink=%p mkdir=%p rmdir=%p\n",
            root_inode,
            root_dentry,
            create_op,
            rename_op,
            unlink_op,
            mkdir_op,
            rmdir_op);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    uint8_t create_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t rename_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t mkdir_dentry[KB_FS_FAKE_DENTRY_BYTES];
    static uint8_t smoke_mnt_idmap[136];
    kb_fs_prepare_named_dentry(
        create_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-created.txt");

    typedef int (*ext4_create_fn)(void *, void *, void *, uint16_t, int);
    ext4_create_fn ext4_create = (ext4_create_fn)create_op;
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    int op_result = ext4_create(smoke_mnt_idmap, root_inode, create_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *created_inode = read_pointer_field(create_dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (op_result != 0 || created_inode == NULL) {
        fprintf(stderr,
            "kobox-ext4-smoke: create failed result=%d inode=%p\n",
            op_result,
            created_inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }
    fprintf(stderr, "kobox-ext4-smoke: create ok name=kobox-created.txt inode=%p\n", created_inode);

    kb_fs_prepare_named_dentry(
        rename_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-renamed.txt");
    typedef int (*ext4_rename_fn)(void *, void *, void *, void *, void *, unsigned int);
    ext4_rename_fn ext4_rename = (ext4_rename_fn)rename_op;
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_rename, &old_gs);
    op_result = ext4_rename(smoke_mnt_idmap, root_inode, create_dentry, root_inode, rename_dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rename failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    write_pointer_field(rename_dentry, KB_FS_DENTRY_INODE_OFFSET, created_inode);
    fprintf(stderr, "kobox-ext4-smoke: rename ok old=kobox-created.txt new=kobox-renamed.txt\n");

    typedef int (*ext4_unlink_fn)(void *, void *);
    ext4_unlink_fn ext4_unlink = (ext4_unlink_fn)unlink_op;
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
    op_result = ext4_unlink(root_inode, rename_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: unlink failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    old_gs = 0;
    kb_fs_subsystem_mark_inode_freeing(created_inode);
    has_gs = kb_fs_enter_ext4_call((void *)ext4_evict_inode, &old_gs);
    ext4_evict_inode(created_inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fprintf(stderr, "kobox-ext4-smoke: unlink ok name=kobox-renamed.txt\n");

    uint8_t replace_old_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t replace_new_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t replace_path_dentry[KB_FS_FAKE_DENTRY_BYTES];
    kb_fs_prepare_named_dentry(
        replace_old_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-replace-old.txt");
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    op_result = ext4_create(smoke_mnt_idmap, root_inode, replace_old_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *replace_old_inode = read_pointer_field(replace_old_dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (op_result != 0 || replace_old_inode == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: replace old create failed result=%d inode=%p\n", op_result, replace_old_inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }
    const uint8_t replace_old_payload[] = "replace-old-live";
    op_result = kb_fs_ext4_smoke_write_payload(
        replace_old_inode,
        replace_old_payload,
        sizeof(replace_old_payload) - 1u,
        &size_sync_ops,
        "replace-old");
    if (op_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }

    kb_fs_prepare_named_dentry(
        replace_new_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-replace-new.txt");
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    op_result = ext4_create(smoke_mnt_idmap, root_inode, replace_new_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *replace_new_inode = read_pointer_field(replace_new_dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (op_result != 0 || replace_new_inode == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: replace new create failed result=%d inode=%p\n", op_result, replace_new_inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }
    const uint8_t replace_new_payload[] = "replace-new-live";
    op_result = kb_fs_ext4_smoke_write_payload(
        replace_new_inode,
        replace_new_payload,
        sizeof(replace_new_payload) - 1u,
        &size_sync_ops,
        "replace-new");
    if (op_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }

    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_rename, &old_gs);
    op_result = ext4_rename(
        smoke_mnt_idmap,
        root_inode,
        replace_old_dentry,
        root_inode,
        replace_new_dentry,
        0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rename replace failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }

    op_result = kb_fs_subsystem_ext4_detach_inode_data_blocks(replace_new_inode);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: replaced detach failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    old_gs = 0;
    kb_fs_subsystem_mark_inode_freeing(replace_new_inode);
    has_gs = kb_fs_enter_ext4_call((void *)ext4_evict_inode, &old_gs);
    ext4_evict_inode(replace_new_inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    kb_fs_prepare_named_dentry(
        replace_path_dentry,
        root_dentry,
        replace_old_inode,
        mount.super_block,
        "kobox-replace-new.txt");
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
    op_result = ext4_unlink(root_inode, replace_path_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    uint32_t replace_old_nlink = 0;
    memcpy(
        &replace_old_nlink,
        (const uint8_t *)replace_old_inode + KB_FS_INODE_NLINK_OFFSET,
        sizeof(replace_old_nlink));
    if (op_result != 0 && replace_old_nlink != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rename cleanup unlink failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    op_result = kb_fs_subsystem_ext4_detach_inode_data_blocks(replace_old_inode);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rename cleanup detach failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    old_gs = 0;
    kb_fs_subsystem_mark_inode_freeing(replace_old_inode);
    has_gs = kb_fs_enter_ext4_call((void *)ext4_evict_inode, &old_gs);
    ext4_evict_inode(replace_old_inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fprintf(stderr, "kobox-ext4-smoke: rename replace cleanup ok\n");

    group_sync_result = kb_fs_subsystem_ext4_sync_group_free_counts(mount.super_block);
    if (group_sync_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: group free count sync before mkdir failed result=%d\n", group_sync_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return group_sync_result;
    }

    kb_fs_prepare_named_dentry(
        mkdir_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-created-dir");
    typedef int (*ext4_mkdir_fn)(void *, void *, void *, uint16_t);
    ext4_mkdir_fn ext4_mkdir = (ext4_mkdir_fn)mkdir_op;
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_mkdir, &old_gs);
    op_result = ext4_mkdir(smoke_mnt_idmap, root_inode, mkdir_dentry, KB_FS_MODE_DIRECTORY_0755);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *mkdir_inode = read_pointer_field(mkdir_dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (op_result != 0 || mkdir_inode == NULL) {
        fprintf(stderr,
            "kobox-ext4-smoke: mkdir failed result=%d inode=%p\n",
            op_result,
            mkdir_inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }
    fprintf(stderr, "kobox-ext4-smoke: mkdir ok name=kobox-created-dir inode=%p\n", mkdir_inode);

    typedef int (*ext4_rmdir_fn)(void *, void *);
    ext4_rmdir_fn ext4_rmdir = (ext4_rmdir_fn)rmdir_op;
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_rmdir, &old_gs);
    op_result = ext4_rmdir(root_inode, mkdir_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rmdir failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    old_gs = 0;
    kb_fs_subsystem_mark_inode_freeing(mkdir_inode);
    has_gs = kb_fs_enter_ext4_call((void *)ext4_evict_inode, &old_gs);
    ext4_evict_inode(mkdir_inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fprintf(stderr, "kobox-ext4-smoke: rmdir ok name=kobox-created-dir\n");

    int super_sync_result = kb_fs_subsystem_ext4_sync_super_free_blocks(mount.super_block);
    if (super_sync_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: super free count sync failed result=%d\n", super_sync_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return super_sync_result;
    }

    kb_fs_subsystem_set_mount_probe_block_device(NULL);
    kb_fs_block_device_destroy(device);
    return 0;
}

size_t kb_fs_subsystem_registered_type_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < KB_FS_TYPE_MAX; i++) {
        if (fs_types[i].active) {
            count++;
        }
    }
    return count;
}

int kb_fs_subsystem_type_snapshot(const char *name, kb_fs_type_snapshot_t *out_snapshot)
{
    kb_fs_type_record_t *record = find_fs_type_by_name(name);
    if (record == NULL || out_snapshot == NULL) {
        return -2;
    }
    out_snapshot->fs_type = record->fs_type;
    out_snapshot->name = record->name;
    out_snapshot->owner_module = record->owner_module;
    out_snapshot->register_count = record->register_count;
    return 0;
}

int kb_fs_subsystem_mount_snapshot(uint64_t handle, kb_fs_mount_snapshot_t *out_snapshot)
{
    kb_fs_mount_record_t *mount = find_mount(handle);
    if (mount == NULL || out_snapshot == NULL) {
        return -2;
    }
    out_snapshot->handle = mount->handle;
    out_snapshot->source = mount->source;
    out_snapshot->target = mount->target;
    out_snapshot->fs_type = mount->type == NULL ? NULL : mount->type->name;
    out_snapshot->block_disk = mount->block_disk;
    return 0;
}
