#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "linux_subsystem/fs/fs.h"
#include "kobox/shim.h"
#include "loader/module_context.h"
#include "loader/symbol_registry.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef KOBOX_FILE_READ_BULK_BYTES
#define KOBOX_FILE_READ_BULK_BYTES (512u * 1024u)
#endif

_Static_assert(KOBOX_FILE_READ_BULK_BYTES >= 4096u,
    "bulk file reads must cover at least one page");
_Static_assert((KOBOX_FILE_READ_BULK_BYTES % 4096u) == 0,
    "bulk file reads must be page aligned");

enum {
    KB_FS_IPC_REQUEST_VERSION = 1,
    KB_FS_TYPE_MAX = 64,
    KB_FS_MOUNT_MAX = 64,
    KB_FS_FILE_MAX = 256,
    KB_FS_DEVPTS_DENTRY_MAX = 256,
    /* Keep file-backed folios within half of the 4096-page KVM arena, leaving
     * the other half for buffer heads, BIOs and module allocations. */
    KB_FS_FILEMAP_FOLIO_CACHE_MAX = 2048,
    KB_FS_CONTEXT_BYTES = 512,
    KB_FS_SUPER_BLOCK_BYTES = 2048,
    KB_FS_FAKE_INODE_HEADROOM_BYTES = 512,
    KB_FS_FAKE_INODE_BYTES = 1024,
    KB_FS_FAKE_INODE_MAPPING_BYTES = 256,
    KB_FS_FAKE_BDEV_BYTES = 256,
    KB_FS_FAKE_BDEV_INODE_BYTES = 512,
    KB_FS_FAKE_ADDRESS_SPACE_BYTES = 128,
    /* Linux 6.12 bdev_read_only() reaches gendisk::state at +0x1f0 while
     * ext4 loads its journal.  Keep the synthetic mount-probe gendisk a full
     * object page, matching the block subsystem's native disk allocation,
     * rather than relying on allocator tail room past a 256-byte object. */
    KB_FS_FAKE_DISK_BYTES = 4096,
    KB_FS_FAKE_BDEV_STATS_BYTES = 128,
    KB_FS_FAKE_QUEUE_BYTES = 128,
    KB_FS_FAKE_BUFFER_HEAD_BYTES = 128,
    KB_FS_FAKE_DENTRY_BYTES = 512,
    KB_FS_BUFFER_CACHE_MAX = 512,
    /* Linux 6.12 LTS struct inode layout used by the storage modules. */
    KB_FS_INODE_BYTES = 0x270,
    KB_FS_TYPE_INIT_FS_CONTEXT_OFFSET = 16,
    KB_FS_TYPE_KILL_SB_OFFSET = 0x28,
    KB_FS_CONTEXT_OPS_OFFSET = 0,
    KB_FS_CONTEXT_OPS_FREE_OFFSET = 0,
    KB_FS_CONTEXT_OPS_PARSE_PARAM_OFFSET = 16,
    KB_FS_CONTEXT_OPS_GET_TREE_OFFSET = 32,
    KB_FS_SUPER_BLOCK_DEV_OFFSET = 0x10,
    KB_FS_SUPER_BLOCK_BLOCKSIZE_BITS_OFFSET = 0x14,
    KB_FS_SUPER_BLOCK_BLOCKSIZE_OFFSET = 0x18,
    KB_FS_SUPER_BLOCK_MAXBYTES_OFFSET = 0x20,
    KB_FS_SUPER_BLOCK_OPS_OFFSET = 0x30,
    KB_FS_SUPER_BLOCK_FLAGS_OFFSET = 0x50,
    KB_FS_SUPER_BLOCK_MAGIC_OFFSET = 0x60,
    KB_FS_SUPER_BLOCK_BDEV_OFFSET = 0xe0,
    KB_FS_SUPER_BLOCK_FS_INFO_OFFSET = 0x380,
    KB_FS_SUPER_BLOCK_DEVNAME_OFFSET = 0x3b0,
    KB_FS_BDEV_SECTOR_COUNT_OFFSET = 0x8,
    KB_FS_BDEV_DISK_OFFSET = 0x10,
    KB_FS_BDEV_QUEUE_OFFSET = 0x18,
    KB_FS_BDEV_STATS_OFFSET = 0x20,
    KB_FS_BDEV_MAPPING_OFFSET = 0x38,
    KB_FS_GENDISK_PART0_OFFSET = 0x40,
    KB_FS_GENDISK_STATE_OFFSET = 0x1f0,
    KB_FS_ADDRESS_SPACE_HOST_OFFSET = 0x0,
    KB_FS_ADDRESS_SPACE_XARRAY_FLAGS_OFFSET = 0x0c,
    KB_FS_ADDRESS_SPACE_NRPAGES_OFFSET = 0x58,
    KB_FS_ADDRESS_SPACE_WRITEBACK_INDEX_OFFSET = 0x60,
    KB_FS_ADDRESS_SPACE_AOPS_OFFSET = 0x68,
    KB_FS_ADDRESS_SPACE_FLAGS_OFFSET = 0x70,
    KB_FS_ADDRESS_SPACE_WB_ERR_OFFSET = 0x78,
    KB_FS_ADDRESS_SPACE_OP_WRITEPAGES_OFFSET = 0x10,
    KB_FS_ADDRESS_SPACE_OP_DIRTY_FOLIO_OFFSET = 0x18,
    KB_FS_ADDRESS_SPACE_OP_READAHEAD_OFFSET = 0x20,
    KB_FS_ADDRESS_SPACE_OP_READ_FOLIO_OFFSET = 0x8,
    KB_FS_ADDRESS_SPACE_OP_WRITE_BEGIN_OFFSET = 0x28,
    KB_FS_ADDRESS_SPACE_OP_WRITE_END_OFFSET = 0x30,
    KB_FS_ADDRESS_SPACE_OP_BMAP_OFFSET = 0x38,
    KB_FS_ADDRESS_SPACE_OP_RELEASE_FOLIO_OFFSET = 0x48,
    KB_FS_ADDRESS_SPACE_OP_IS_PARTIALLY_UPTODATE_OFFSET = 0x70,
    KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET = 0x8,
    KB_FS_BUFFER_HEAD_FOLIO_OFFSET = 0x10,
    KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET = 0x18,
    KB_FS_BUFFER_HEAD_SIZE_OFFSET = 0x20,
    KB_FS_BUFFER_HEAD_DATA_OFFSET = 0x28,
    KB_FS_BUFFER_HEAD_BDEV_OFFSET = 0x30,
    KB_FS_BUFFER_HEAD_END_IO_OFFSET = 0x38,
    KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET = 0x48,
    KB_FS_BUFFER_HEAD_ASSOC_MAP_OFFSET = 0x58,
    KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET = 0x60,
    KB_FS_FOLIO_MAPPING_OFFSET = 0x18,
    KB_FS_FOLIO_INDEX_OFFSET = 0x20,
    KB_FS_FOLIO_PRIVATE_OFFSET = 0x28,
    KB_FS_FOLIO_REFCOUNT_OFFSET = 0x34,
    KB_FS_FOLIO_FLAG_LOCKED = 0x1,
    KB_FS_FOLIO_FLAG_UPTODATE = 0x8,
    KB_FS_FOLIO_FLAG_DIRTY = 0x10,
    KB_FS_FOLIO_FLAG_WRITEBACK = 0x100,
    KB_FS_FOLIO_FLAG_READAHEAD = 1u << 16,
    KB_FS_PAGE_SIZE = 4096,
    KB_FS_KVM_STRUCT_PAGE_SIZE = 64,
    KB_FS_INODE_MODE_OFFSET = 0x0,
    KB_FS_INODE_UID_OFFSET = 0x4,
    KB_FS_INODE_GID_OFFSET = 0x8,
    KB_FS_INODE_FLAGS_OFFSET = 0x0c,
    KB_FS_INODE_SB_OFFSET = 0x28,
    KB_FS_INODE_MAPPING_OFFSET = 0x30,
    KB_FS_INODE_NUMBER_OFFSET = 0x40,
    KB_FS_INODE_NLINK_OFFSET = 0x48,
    KB_FS_INODE_RDEV_OFFSET = 0x4c,
    KB_FS_INODE_SIZE_OFFSET = 0x50,
    KB_FS_INODE_ATIME_SEC_OFFSET = 0x58,
    KB_FS_INODE_MTIME_SEC_OFFSET = 0x60,
    KB_FS_INODE_CTIME_SEC_OFFSET = 0x68,
    KB_FS_INODE_ATIME_NSEC_OFFSET = 0x70,
    KB_FS_INODE_MTIME_NSEC_OFFSET = 0x74,
    KB_FS_INODE_CTIME_NSEC_OFFSET = 0x78,
    KB_FS_INODE_BLOCKS_OFFSET = 0x88,
    KB_FS_INODE_STATE_OFFSET = 0x90,
    KB_FS_INODE_BLKBITS_OFFSET = 0x86,
    /* Match the inode-state layout of the loaded Linux 6.12 LTS modules.
     * ext4_truncate() tests I_NEW|I_FREEING as 0x81 before requiring i_rwsem. */
    KB_FS_INODE_STATE_NEW = 1u << 0,
    KB_FS_INODE_STATE_DIRTY_SYNC = 1u << 3,
    KB_FS_INODE_STATE_DIRTY_DATASYNC = 1u << 4,
    KB_FS_INODE_STATE_DIRTY_PAGES = 1u << 5,
    KB_FS_INODE_STATE_DIRTY_TIME = 1u << 11,
    KB_FS_INODE_STATE_WILL_FREE = 1u << 6,
    KB_FS_INODE_STATE_FREEING = 1u << 7,
    KB_FS_INODE_STATE_CLEAR = 1u << 8,
    KB_FS_INODE_STATE_CREATING = 1u << 14,
    KB_FS_INODE_RWSEM_OFFSET = 0x98,
    KB_FS_INODE_RWSEM_HELD = 1,
    KB_FS_INODE_HASH_OFFSET = 0xd0,
    KB_FS_INODE_IO_LIST_OFFSET = 0xe0,
    KB_FS_INODE_LRU_OFFSET = 0x100,
    KB_FS_INODE_SB_LIST_OFFSET = 0x110,
    KB_FS_INODE_WB_LIST_OFFSET = 0x120,
    KB_FS_INODE_VERSION_OFFSET = 0x140,
    KB_FS_INODE_DATA_OFFSET = 0x170,
    KB_FS_INODE_COUNT_OFFSET = 0x150,
    KB_FS_INODE_DIO_COUNT_OFFSET = 0x154,
    KB_FS_INODE_WRITECOUNT_OFFSET = 0x158,
    KB_FS_INODE_DEVICES_OFFSET = 0x230,
    KB_FS_INODE_LINK_OFFSET = 0x240,
    KB_FS_INODE_GENERATION_OFFSET = 0x248,
    KB_FS_INODE_FILE_OP_OFFSET = 0x160,
    KB_FS_ADDRESS_SPACE_PRIVATE_LIST_OFFSET = 0x80,
    KB_FS_NATIVE_FILE_OP_OFFSET = 0x10,
    KB_FS_NATIVE_FILE_MODE_OFFSET = 0x0c,
    KB_FS_NATIVE_FILE_PATH_MNT_OFFSET = 0x40,
    KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET = 0x48,
    KB_FS_NATIVE_FILE_POSITION_OFFSET = 0x70,
    KB_FS_NATIVE_FILE_VERSION_OFFSET = 0x30,
    KB_FS_NATIVE_FILE_WB_ERR_OFFSET = 0x88,
    KB_FS_NATIVE_FILE_RA_OFFSET = 0x98,
    KB_FS_FILE_RA_START_OFFSET = 0x0,
    KB_FS_FILE_RA_SIZE_OFFSET = 0x8,
    KB_FS_FILE_RA_ASYNC_SIZE_OFFSET = 0xc,
    KB_FS_FILE_RA_PAGES_OFFSET = 0x10,
    KB_FS_FILE_RA_PREV_POS_OFFSET = 0x18,
    KB_FS_READAHEAD_FILE_OFFSET = 0x0,
    KB_FS_READAHEAD_MAPPING_OFFSET = 0x8,
    KB_FS_READAHEAD_RA_OFFSET = 0x10,
    KB_FS_READAHEAD_INDEX_OFFSET = 0x18,
    KB_FS_READAHEAD_NR_PAGES_OFFSET = 0x20,
    KB_FS_READAHEAD_BATCH_COUNT_OFFSET = 0x24,
    KB_FS_VMA_FILE_OFFSET = 0x80,
    KB_FS_VM_FAULT_GFP_MASK_OFFSET = 0x08,
    KB_FS_VM_FAULT_PGOFF_OFFSET = 0x10,
    KB_FS_VM_FAULT_PAGE_OFFSET = 0x50,
    KB_FS_FILE_OP_ITERATE_SHARED_OFFSET = 0x40,
    KB_FS_FILE_OP_LLSEEK_OFFSET = 0x10,
    KB_FS_FILE_OP_READ_OFFSET = 0x18,
    KB_FS_FILE_OP_WRITE_OFFSET = 0x20,
    KB_FS_FILE_OP_READ_ITER_OFFSET = 0x28,
    KB_FS_FILE_OP_WRITE_ITER_OFFSET = 0x30,
    KB_FS_FILE_OP_OPEN_OFFSET = 0x68,
    KB_FS_FILE_OP_RELEASE_OFFSET = 0x78,
    KB_FS_FILE_OP_FSYNC_OFFSET = 0x80,
    KB_FS_FMODE_READ = 1u << 0,
    KB_FS_FMODE_WRITE = 1u << 1,
    KB_FS_FMODE_LSEEK = 0x4,
    KB_FS_FMODE_PREAD = 0x8,
    KB_FS_FMODE_PWRITE = 0x10,
    KB_FS_FMODE_ATOMIC_POS = 0x8000,
    KB_FS_FMODE_WRITER = 0x10000,
    KB_FS_FMODE_CAN_READ = 0x20000,
    KB_FS_FMODE_CAN_WRITE = 0x40000,
    KB_FS_FMODE_OPENED = 0x80000,
    KB_FS_FMODE_CAN_ODIRECT = 0x400000,
    KB_FS_FMODE_UNSIGNED_OFFSET = 0x2000,
    KB_FS_SUPER_OP_ALLOC_INODE_OFFSET = 0x0,
    KB_FS_SUPER_OP_DESTROY_INODE_OFFSET = 0x8,
    KB_FS_SUPER_OP_FREE_INODE_OFFSET = 0x10,
    KB_FS_SUPER_OP_DIRTY_INODE_OFFSET = 0x18,
    KB_FS_SUPER_OP_WRITE_INODE_OFFSET = 0x20,
    KB_FS_SUPER_OP_EVICT_INODE_OFFSET = 0x30,
    KB_FS_SUPER_OP_PUT_SUPER_OFFSET = 0x38,
    KB_FS_SUPER_OP_SYNC_FS_OFFSET = 0x40,
    KB_FS_INODE_MODE_DIRECTORY = 0040000 | 0755,
    KB_FS_DENTRY_PARENT_OFFSET = 0x18,
    KB_FS_DENTRY_QSTR_OFFSET = 0x20,
    KB_FS_DENTRY_INODE_OFFSET = 0x30,
    KB_FS_DENTRY_INODE_COMPAT_OFFSET = 0x30,
    /* Linux 6.12 moved d_lockref behind d_fsdata, so its d_sb is at +0x68.
     * The Linux 6.8 TTY island still has d_lockref before d_op and therefore
     * reaches d_sb at +0x70.  Keep the layouts distinct: writing the 6.12
     * field into a devpts dentry overwrites its d_op and leaves d_sb NULL. */
    KB_FS_DENTRY_SB_OFFSET = 0x68,
    KB_FS_DEVPTS_DENTRY_SB_OFFSET = 0x70,
    KB_FS_DENTRY_FLAGS_OFFSET = 0x0,
    KB_FS_DENTRY_INLINE_NAME_OFFSET = 0x38,
    KB_FS_DENTRY_INLINE_NAME_BYTES = 0x28,
    KB_FS_DENTRY_ENTRY_TYPE_MASK = 7u << 20,
    KB_FS_DENTRY_DIRECTORY_TYPE = 2u << 20,
    KB_FS_DENTRY_REGULAR_TYPE = 4u << 20,
    KB_FS_QSTR_HASH_LEN_OFFSET = 0x0,
    KB_FS_QSTR_NAME_OFFSET = 0x8,
    KB_FS_PATH_MNT_OFFSET = 0x0,
    KB_FS_PATH_DENTRY_OFFSET = 0x8,
    KB_FS_VFSMOUNT_ROOT_OFFSET = 0x0,
    KB_FS_VFSMOUNT_SB_OFFSET = 0x8,
    KB_FS_VFSMOUNT_FLAGS_OFFSET = 0x10,
    KB_FS_MNT_NOATIME = 0x08,
    KB_FS_MNT_NODIRATIME = 0x10,
    KB_FS_MNT_RELATIME = 0x20,
    KB_FS_MNT_READONLY = 0x40,
    KB_FS_SB_RDONLY = 1u << 0,
    KB_FS_SB_SYNCHRONOUS = 1u << 4,
    KB_FS_SB_DIRSYNC = 1u << 7,
    KB_FS_SB_NOATIME = 1u << 10,
    KB_FS_SB_NODIRATIME = 1u << 11,
    KB_FS_SB_I_VERSION = 1u << 23,
    KB_FS_SB_LAZYTIME = 1u << 25,
    KB_FS_INODE_NOATIME = 1u << 1,
    KB_FS_INODE_SYNC = 1u << 0,
    KB_FS_INODE_DIRSYNC = 1u << 6,
    KB_FS_INODE_SWAPFILE = 1u << 8,
    KB_FS_KSTAT_RESULT_MASK_OFFSET = 0x00,
    KB_FS_KSTAT_MODE_OFFSET = 0x04,
    KB_FS_KSTAT_NLINK_OFFSET = 0x08,
    KB_FS_KSTAT_BLOCKSIZE_OFFSET = 0x0c,
    KB_FS_KSTAT_INODE_OFFSET = 0x20,
    KB_FS_KSTAT_DEVICE_OFFSET = 0x28,
    KB_FS_KSTAT_RDEV_OFFSET = 0x2c,
    KB_FS_KSTAT_UID_OFFSET = 0x30,
    KB_FS_KSTAT_GID_OFFSET = 0x34,
    KB_FS_KSTAT_SIZE_OFFSET = 0x38,
    KB_FS_KSTAT_ATIME_OFFSET = 0x40,
    KB_FS_KSTAT_MTIME_OFFSET = 0x50,
    KB_FS_KSTAT_CTIME_OFFSET = 0x60,
    KB_FS_KSTAT_BLOCKS_OFFSET = 0x80,
    KB_FS_KSTAT_CHANGE_COOKIE_OFFSET = 0x98,
    KB_FS_STATX_CHANGE_COOKIE = 0x40000000u,
    KB_FS_DEVPTS_SUPER_MAGIC_OFFSET = 0x60,
    KB_FS_DEVPTS_SUPER_ROOT_OFFSET = 0x68,
    KB_FS_DEVPTS_SUPER_MAGIC = 0x1cd1,
    KB_FS_NATIVE_FILE_MAPPING_OFFSET = 0x18,
    KB_FS_FILE_INODE_OFFSET = 0x28,
    KB_FS_KIOCB_FILE_OFFSET = 0x0,
    KB_FS_KIOCB_POS_OFFSET = 0x8,
    KB_FS_KIOCB_FLAGS_OFFSET = 0x20,
    KB_FS_IOV_ITER_DATA_SOURCE_OFFSET = 0x2,
    KB_FS_IOV_ITER_COUNT_OFFSET = 0x18,
    KB_FS_IOV_ITER_BUFFER_OFFSET = 0x20,
    KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET = 0x78,
    KB_FS_INODE_OP_OFFSET = 0x20,
    KB_FS_INODE_OP_CREATE_OFFSET = 0x28,
    KB_FS_INODE_OP_UNLINK_OFFSET = 0x38,
    KB_FS_INODE_OP_MKDIR_OFFSET = 0x48,
    KB_FS_INODE_OP_RMDIR_OFFSET = 0x50,
    KB_FS_INODE_OP_RENAME_OFFSET = 0x60,
    KB_FS_INODE_OP_TMPFILE_OFFSET = 0x98,
    KB_FS_IATTR_VALID_OFFSET = 0x0,
    KB_FS_IATTR_MODE_OFFSET = 0x4,
    KB_FS_IATTR_UID_OFFSET = 0x8,
    KB_FS_IATTR_GID_OFFSET = 0xc,
    KB_FS_IATTR_SIZE_OFFSET = 0x10,
    KB_FS_IATTR_ATIME_OFFSET = 0x18,
    KB_FS_IATTR_MTIME_OFFSET = 0x28,
    KB_FS_IATTR_CTIME_OFFSET = 0x38,
    KB_FS_ATTR_MODE = 1u << 0,
    KB_FS_ATTR_UID = 1u << 1,
    KB_FS_ATTR_GID = 1u << 2,
    KB_FS_ATTR_SIZE = 1u << 3,
    KB_FS_ATTR_ATIME = 1u << 4,
    KB_FS_ATTR_MTIME = 1u << 5,
    KB_FS_ATTR_CTIME = 1u << 6,
    KB_FS_WRITEBACK_CONTROL_BYTES = 384,
    KB_FS_WRITEBACK_CONTROL_SYNC_MODE_OFFSET = 0x20,
    KB_FS_WRITEBACK_CONTROL_WB_SYNC_ALL = 1,
    KB_FS_XARRAY_MARK_DIRTY = 1u << 25,
    KB_FS_XARRAY_MARK_WRITEBACK = 1u << 26,
    KB_FS_XARRAY_MARK_TOWRITE = 1u << 27,
    KB_FS_PAGECACHE_TAG_DIRTY = 0,
    KB_FS_PAGECACHE_TAG_WRITEBACK = 1,
    KB_FS_PAGECACHE_TAG_TOWRITE = 2,
    KB_FS_FOLIO_BATCH_MAX = 31,
    KB_FS_MODE_REGULAR_0644 = 0100000 | 0644,
    KB_FS_MODE_REGULAR_0600 = 0100000 | 0600,
    KB_FS_MODE_DIRECTORY_0755 = 0040000 | 0755,
    KB_FS_MODE_TYPE_MASK = 0170000,
    KB_FS_MODE_PERM_MASK = 07777,
    KB_FS_BIO_MAGIC = 0x6b62696f,
    KB_FS_BIO_OP_MASK = 0xff,
    KB_FS_BIO_READ_BATCH_MAX = 128,
    /* Preserve a large ext4 BIO as queue-sized write batches.  The NVMe
     * bridge applies the runtime max_hw_sectors limit before merging. */
    KB_FS_BIO_WRITE_BATCH_MAX = 128,
    KB_FS_FGP_LOCK = 0x2,
    KB_FS_FGP_CREAT = 0x4,
    KB_FS_FGP_FOR_MMAP = 0x40,
    KB_FS_BH_UPTODATE = 1ull << 0,
    KB_FS_BH_DIRTY = 1ull << 1,
    KB_FS_BH_LOCK = 1ull << 2,
    KB_FS_BH_REQ = 1ull << 3,
    KB_FS_BH_MAPPED = 1ull << 4,
    KB_FS_BH_NEW = 1ull << 5,
    KB_FS_BH_DELAY = 1ull << 8,
    KB_FS_BH_WRITE_EIO = 1ull << 10,
    KB_FS_BH_UNWRITTEN = 1ull << 11,
    KB_FS_AOP_WRITEPAGE_ACTIVATE = 0x80000,
    KB_FS_INODE_ALLOCATION_MAX = 2048,
    KB_FS_DENTRY_ALLOCATION_MAX = 2048,
    KB_FS_INODE_VERSION_QUERIED = 1u,
    KB_FS_INODE_VERSION_INCREMENT = 2u,
};

_Static_assert(KB_FS_FAKE_DISK_BYTES >= KB_FS_GENDISK_STATE_OFFSET + sizeof(unsigned long),
    "synthetic gendisk must include Linux 6.12 state");
_Static_assert(KB_FS_INODE_DATA_OFFSET + KB_FS_FAKE_INODE_MAPPING_BYTES <= KB_FS_INODE_BYTES,
    "Linux 6.12 inode must contain its embedded address_space");

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
    kb_fs_block_read_batch_fn read_batch;
    kb_fs_block_write_fn write;
    kb_fs_block_write_batch_fn write_batch;
    kb_fs_block_write_flags_fn write_flags;
    kb_fs_block_flush_fn flush;
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

/* Linux 6.12 BIO_MAX_VECS.  Filesystems request this full capacity even when
 * the particular bio ultimately carries only a handful of pages. */
enum { KB_FS_BIO_MAX_VECS = 256 };

typedef struct kb_fs_linux_bio_vec {
    void *page;
    uint32_t length;
    uint32_t offset;
} kb_fs_linux_bio_vec_t;

/* Linux 6.12 LTS struct bio prefix.  The loaded modules access these fields
 * directly; Kobox bookkeeping lives after the inline vector storage. */
typedef struct kb_fs_linux_bio {
    void *next;
    void *bdev;
    uint32_t opf;
    uint16_t flags;
    uint16_t ioprio;
    uint8_t write_hint;
    uint8_t status;
    uint16_t padding0;
    uint32_t remaining;
    uint64_t sector;
    uint32_t size;
    uint32_t index;
    uint32_t bvec_done;
    uint32_t cookie;
    void (*end_io)(void *);
    void *private_data;
    void *blkg;
    uint64_t issue;
    void *integrity;
    uint16_t vector_count;
    uint16_t max_vectors;
    uint32_t refcount;
    kb_fs_linux_bio_vec_t *vectors;
    void *pool;
    kb_fs_linux_bio_vec_t inline_vectors[KB_FS_BIO_MAX_VECS];
} kb_fs_linux_bio_t;

_Static_assert(offsetof(kb_fs_linux_bio_t, bdev) == 0x08, "Linux bio bdev offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, opf) == 0x10, "Linux bio opf offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, status) == 0x19, "Linux bio status offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, sector) == 0x20, "Linux bio sector offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, end_io) == 0x38, "Linux bio end_io offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, vector_count) == 0x60, "Linux bio vcnt offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, vectors) == 0x68, "Linux bio io_vec offset");
_Static_assert(offsetof(kb_fs_linux_bio_t, inline_vectors) == 0x78,
    "Linux bio inline vectors offset");

typedef struct kb_fs_bio_record {
    kb_fs_linux_bio_t bio;
    uint32_t magic;
    uint32_t reserved;
    void *buffer;
    size_t len;
    size_t page_offset;
    int result;
    uint32_t submitted;
    uint32_t queued;
    uint32_t completed;
    struct kb_fs_bio_record *queue_next;
} kb_fs_bio_record_t;

_Static_assert(offsetof(kb_fs_bio_record_t, bio) == 0, "Linux bio must prefix record");

typedef struct kb_fs_buffer_cache_record {
    int active;
    void *bdev;
    uint64_t block_number;
    uint64_t block_size;
    void *buffer_head;
    void *folio;
    void *data;
    unsigned int folio_order;
    int dirty;
} kb_fs_buffer_cache_record_t;

typedef struct kb_fs_filemap_folio_record {
    int active;
    int towrite;
    void *mapping;
    unsigned long index;
    void *folio;
} kb_fs_filemap_folio_record_t;

typedef struct kb_fs_xarray_entry {
    unsigned long index;
    void *entry;
    struct kb_fs_xarray_entry *next;
} kb_fs_xarray_entry_t;

typedef struct kb_fs_xarray_record {
    void *xarray;
    kb_fs_xarray_entry_t *entries;
    struct kb_fs_xarray_record *next;
} kb_fs_xarray_record_t;

typedef struct kb_fs_inode_allocation_record {
    int active;
    int native;
    int hashed;
    int dirty_metadata;
    void *inode;
    void *mapping;
    void *storage;
    int acl_access_cached;
    int acl_default_cached;
    void *acl_access;
    void *acl_default;
} kb_fs_inode_allocation_record_t;

typedef struct kb_fs_posix_acl_entry {
    int16_t tag;
    uint16_t permission;
    uint32_t id;
} kb_fs_posix_acl_entry_t;

typedef struct kb_fs_posix_acl {
    uint32_t reference_count;
    uint32_t reserved;
    uint8_t rcu_head[16];
    uint32_t count;
    kb_fs_posix_acl_entry_t entries[];
} kb_fs_posix_acl_t;

_Static_assert(
    offsetof(kb_fs_posix_acl_t, entries) == 28,
    "Linux 6.12 POSIX ACL entries must start at +0x1c");

typedef struct kb_fs_dentry_allocation_record {
    int active;
    int devpts_layout;
    int hashed;
    int parent_ref_held;
    unsigned int refcount;
    void *dentry;
    char *name;
} kb_fs_dentry_allocation_record_t;

static kb_fs_type_record_t fs_types[KB_FS_TYPE_MAX];
static kb_fs_mount_record_t fs_mounts[KB_FS_MOUNT_MAX];
static kb_fs_file_record_t fs_files[KB_FS_FILE_MAX];
unsigned char kb_fs_subsystem_blockdev_superblock[KB_FS_SUPER_BLOCK_BYTES];
static uint64_t next_mount_handle = 1;
static kb_fs_mount_path_probe_t last_mount_path_probe;
static uint8_t last_nodev_vfsmount[32];
static uint8_t last_root_vfsmount[32];
static void *devpts_index_dentries[KB_FS_DEVPTS_DENTRY_MAX];
static kb_fs_block_device_t *mount_probe_block_device;
static kb_fs_bdev_binding_t active_bdev_binding;
static void *mount_probe_super_block;
static void *mount_probe_bdev;
static void *mount_probe_bdev_inode;
static void *mount_probe_bdev_mapping;
static void *mount_probe_disk;
static void *mount_probe_bdev_stats;
static void *mount_probe_queue;
static int mount_probe_mounted;
static kb_fs_bio_record_t *bio_queue_head;
static kb_fs_bio_record_t *bio_queue_tail;
static size_t bio_queue_depth;
static int bio_auto_drain = 1;
static int bio_last_drain_status;
static unsigned int block_plug_depth;
static kb_fs_buffer_cache_record_t buffer_cache[KB_FS_BUFFER_CACHE_MAX];
static size_t buffer_cache_dirty_count;
static size_t buffer_cache_evict_cursor;
static kb_fs_filemap_folio_record_t filemap_folio_cache[KB_FS_FILEMAP_FOLIO_CACHE_MAX];
static atomic_flag xarray_records_lock = ATOMIC_FLAG_INIT;
static kb_fs_xarray_record_t *xarray_records;
static kb_fs_inode_allocation_record_t inode_allocations[KB_FS_INODE_ALLOCATION_MAX];
static kb_fs_dentry_allocation_record_t dentry_allocations[KB_FS_DENTRY_ALLOCATION_MAX];
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
static kb_fs_read_profile_t fs_read_profile;
#endif
static kb_fs_storage_trace_t fs_storage_trace;
static kb_fs_hotpath_profile_t fs_hotpath_profile;
static uint8_t bad_inode_operations[8];
static uint8_t bad_file_operations[8];

#define KB_FS_TRACE_INCREMENT(field) \
    __atomic_fetch_add(&fs_storage_trace.field, 1u, __ATOMIC_RELAXED)
#define KB_FS_TRACE_ADD(field, value) \
    __atomic_fetch_add(&fs_storage_trace.field, (uint64_t)(value), __ATOMIC_RELAXED)

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE && defined(__x86_64__)
static uint64_t fs_hotpath_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
#define FS_HOTPATH_BEGIN(name) const uint64_t name = fs_hotpath_tsc()
#define FS_HOTPATH_END(field, name) \
    (fs_hotpath_profile.field##_calls++, \
     fs_hotpath_profile.field##_cycles += fs_hotpath_tsc() - (name))
#else
#define FS_HOTPATH_BEGIN(name) ((void)0)
#define FS_HOTPATH_END(field, name) ((void)0)
#endif

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE && defined(__x86_64__)
#define FS_READ_PROFILE_BEGIN(name) const uint64_t name = fs_hotpath_tsc()
#define FS_READ_PROFILE_END(field, name) \
    do { \
        const uint64_t profile_end = fs_hotpath_tsc(); \
        if (profile_end >= (name)) { \
            __atomic_fetch_add( \
                &fs_read_profile.field##_cycles, \
                profile_end - (name), \
                __ATOMIC_RELAXED); \
            __atomic_fetch_add( \
                &fs_read_profile.field##_calls, \
                1u, \
                __ATOMIC_RELAXED); \
        } \
    } while (0)
#define FS_READ_PROFILE_EXTENT_BEGIN(name) \
    const uint64_t name = fs_hotpath_tsc(); \
    const uint64_t name##_device = __atomic_load_n( \
        &fs_read_profile.device_read_cycles, __ATOMIC_RELAXED); \
    const uint64_t name##_overlay = __atomic_load_n( \
        &fs_read_profile.overlay_cycles, __ATOMIC_RELAXED)
#define FS_READ_PROFILE_EXTENT_END(name) \
    do { \
        const uint64_t profile_end = fs_hotpath_tsc(); \
        const uint64_t profile_device_end = __atomic_load_n( \
            &fs_read_profile.device_read_cycles, __ATOMIC_RELAXED); \
        const uint64_t profile_overlay_end = __atomic_load_n( \
            &fs_read_profile.overlay_cycles, __ATOMIC_RELAXED); \
        if (profile_end >= (name)) { \
            uint64_t profile_cycles = profile_end - (name); \
            const uint64_t profile_device_cycles = \
                profile_device_end >= name##_device ? \
                    profile_device_end - name##_device : 0; \
            const uint64_t profile_overlay_cycles = \
                profile_overlay_end >= name##_overlay ? \
                    profile_overlay_end - name##_overlay : 0; \
            const uint64_t profile_nested_cycles = \
                profile_device_cycles <= UINT64_MAX - profile_overlay_cycles ? \
                    profile_device_cycles + profile_overlay_cycles : UINT64_MAX; \
            profile_cycles = profile_cycles > profile_nested_cycles ? \
                profile_cycles - profile_nested_cycles : 0; \
            __atomic_fetch_add( \
                &fs_read_profile.extent_lookup_cycles, \
                profile_cycles, \
                __ATOMIC_RELAXED); \
            __atomic_fetch_add( \
                &fs_read_profile.extent_lookup_calls, \
                1u, \
                __ATOMIC_RELAXED); \
        } \
    } while (0)
static long fs_read_profile_finish(uint64_t start, long result)
{
    const uint64_t end = fs_hotpath_tsc();
    if (start != 0 && end >= start) {
        __atomic_fetch_add(&fs_read_profile.calls, 1u, __ATOMIC_RELAXED);
        if (result > 0) {
            __atomic_fetch_add(
                &fs_read_profile.bytes,
                (uint64_t)result,
                __ATOMIC_RELAXED);
        }
        __atomic_fetch_add(
            &fs_read_profile.total_cycles,
            end - start,
            __ATOMIC_RELAXED);
    }
    return result;
}
static kb_status_t fs_read_profile_finish_status(
    uint64_t start,
    kb_status_t status,
    uint64_t bytes)
{
    const uint64_t end = fs_hotpath_tsc();
    if (start != 0 && end >= start) {
        __atomic_fetch_add(&fs_read_profile.calls, 1u, __ATOMIC_RELAXED);
        if (status == KB_OK && bytes != 0) {
            __atomic_fetch_add(
                &fs_read_profile.bytes,
                bytes,
                __ATOMIC_RELAXED);
        }
        __atomic_fetch_add(
            &fs_read_profile.total_cycles,
            end - start,
            __ATOMIC_RELAXED);
    }
    return status;
}
#define FS_READ_PROFILE_RETURN(start, result) \
    return fs_read_profile_finish((start), (result))
#define FS_READ_PROFILE_STATUS_RETURN(start, status, bytes) \
    return fs_read_profile_finish_status((start), (status), (bytes))
#else
#define FS_READ_PROFILE_BEGIN(name) ((void)0)
#define FS_READ_PROFILE_END(field, name) ((void)0)
#define FS_READ_PROFILE_EXTENT_BEGIN(name) ((void)0)
#define FS_READ_PROFILE_EXTENT_END(name) ((void)0)
#define FS_READ_PROFILE_RETURN(start, result) return (result)
#define FS_READ_PROFILE_STATUS_RETURN(start, status, bytes) return (status)
#endif

void kb_fs_hotpath_profile_reset(void)
{
    memset(&fs_hotpath_profile, 0, sizeof(fs_hotpath_profile));
}

void kb_fs_hotpath_profile_snapshot(kb_fs_hotpath_profile_t *out_profile)
{
    if (out_profile != NULL) {
        *out_profile = fs_hotpath_profile;
    }
}

void kb_fs_storage_trace_snapshot(kb_fs_storage_trace_t *out_trace)
{
    if (out_trace == NULL) {
        return;
    }
    memcpy(out_trace, &fs_storage_trace, sizeof(*out_trace));
}

void kb_fs_read_profile_snapshot(kb_fs_read_profile_t *out_profile)
{
    if (out_profile == NULL) {
        return;
    }
    memset(out_profile, 0, sizeof(*out_profile));
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
#define KB_FS_PROFILE_LOAD(field) \
    out_profile->field = __atomic_load_n(&fs_read_profile.field, __ATOMIC_RELAXED)
    KB_FS_PROFILE_LOAD(calls);
    KB_FS_PROFILE_LOAD(bytes);
    KB_FS_PROFILE_LOAD(total_cycles);
    KB_FS_PROFILE_LOAD(extent_lookup_calls);
    KB_FS_PROFILE_LOAD(extent_lookup_cycles);
    KB_FS_PROFILE_LOAD(device_read_calls);
    KB_FS_PROFILE_LOAD(device_read_cycles);
    KB_FS_PROFILE_LOAD(overlay_calls);
    KB_FS_PROFILE_LOAD(overlay_cycles);
    KB_FS_PROFILE_LOAD(partial_copy_calls);
    KB_FS_PROFILE_LOAD(partial_copy_cycles);
#undef KB_FS_PROFILE_LOAD
#endif
}

static void *read_pointer_field(const void *base, size_t offset);
static int kb_fs_enter_ext4_call(void *function, unsigned long *old_gs);
static void *folio_page_payload(void *folio);
static int filemap_writeback_range(void *mapping, int64_t start, int64_t end);

static void xarray_records_acquire(void)
{
    while (atomic_flag_test_and_set_explicit(
        &xarray_records_lock, memory_order_acquire))
    {
    }
}

static void xarray_records_release(void)
{
    atomic_flag_clear_explicit(&xarray_records_lock, memory_order_release);
}

static kb_fs_xarray_record_t *xarray_record_find_locked(void *xarray)
{
    for (kb_fs_xarray_record_t *record = xarray_records;
         record != NULL;
         record = record->next)
    {
        if (record->xarray == xarray) {
            return record;
        }
    }
    return NULL;
}

static void xarray_write_head(void *xarray, void *head)
{
    if (xarray != NULL) {
        memcpy((uint8_t *)xarray + 8u, &head, sizeof(head));
    }
}

static void filemap_refresh_xarray_head(void *mapping)
{
    if (mapping == NULL) {
        return;
    }
    FS_HOTPATH_BEGIN(profile_start);
    void *head = NULL;
    unsigned long first_index = ULONG_MAX;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        const kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (record->active && record->mapping == mapping &&
            record->folio != NULL && record->index < first_index)
        {
            first_index = record->index;
            head = record->folio;
        }
    }
    xarray_write_head((uint8_t *)mapping + 8u, head);
    FS_HOTPATH_END(xarray_refresh, profile_start);
}

static void *filemap_xarray_load(void *xarray, unsigned long index)
{
    if (xarray == NULL) {
        return NULL;
    }
    FS_HOTPATH_BEGIN(profile_start);
    void *mapping = (uint8_t *)xarray - 8u;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (filemap_folio_cache[i].active &&
            filemap_folio_cache[i].mapping == mapping &&
            filemap_folio_cache[i].index == index)
        {
            void *folio = filemap_folio_cache[i].folio;
            FS_HOTPATH_END(xarray_load, profile_start);
            return folio;
        }
    }
    FS_HOTPATH_END(xarray_load, profile_start);
    return NULL;
}

int kb_fs_subsystem_xa_insert(
    void *xarray,
    unsigned long index,
    void *entry,
    unsigned int gfp)
{
    (void)gfp;
    if (xarray == NULL || entry == NULL) {
        return -22;
    }
    xarray_records_acquire();
    kb_fs_xarray_record_t *record = xarray_record_find_locked(xarray);
    if (record == NULL) {
        record = calloc(1, sizeof(*record));
        if (record == NULL) {
            xarray_records_release();
            return -12;
        }
        record->xarray = xarray;
        record->next = xarray_records;
        xarray_records = record;
    }
    kb_fs_xarray_entry_t **cursor = &record->entries;
    while (*cursor != NULL && (*cursor)->index < index) {
        cursor = &(*cursor)->next;
    }
    if (*cursor != NULL && (*cursor)->index == index) {
        xarray_records_release();
        return -16;
    }
    kb_fs_xarray_entry_t *new_entry = calloc(1, sizeof(*new_entry));
    if (new_entry == NULL) {
        xarray_records_release();
        return -12;
    }
    new_entry->index = index;
    new_entry->entry = entry;
    new_entry->next = *cursor;
    *cursor = new_entry;
    xarray_write_head(xarray, record->entries->entry);
    xarray_records_release();
    return 0;
}

void *kb_fs_subsystem_xa_store(
    void *xarray,
    unsigned long index,
    void *entry,
    unsigned int gfp)
{
    (void)gfp;
    if (xarray == NULL) {
        return (void *)(intptr_t)-22;
    }
    if (entry == NULL) {
        return kb_fs_subsystem_xa_erase(xarray, index);
    }
    xarray_records_acquire();
    kb_fs_xarray_record_t *record = xarray_record_find_locked(xarray);
    if (record != NULL) {
        for (kb_fs_xarray_entry_t *item = record->entries;
             item != NULL;
             item = item->next)
        {
            if (item->index == index) {
                void *old = item->entry;
                item->entry = entry;
                xarray_write_head(xarray, record->entries->entry);
                xarray_records_release();
                return old;
            }
        }
    }
    xarray_records_release();
    int status = kb_fs_subsystem_xa_insert(xarray, index, entry, gfp);
    return status == 0 ? NULL : (void *)(intptr_t)status;
}

void *kb_fs_subsystem_xa_load(void *xarray, unsigned long index)
{
    if (xarray == NULL) {
        return NULL;
    }
    xarray_records_acquire();
    kb_fs_xarray_record_t *record = xarray_record_find_locked(xarray);
    if (record != NULL) {
        for (kb_fs_xarray_entry_t *item = record->entries;
             item != NULL && item->index <= index;
             item = item->next)
        {
            if (item->index == index) {
                void *entry = item->entry;
                xarray_records_release();
                return entry;
            }
        }
    }
    xarray_records_release();
    return filemap_xarray_load(xarray, index);
}

void *kb_fs_subsystem_xa_erase(void *xarray, unsigned long index)
{
    if (xarray == NULL) {
        return NULL;
    }
    xarray_records_acquire();
    kb_fs_xarray_record_t *record = xarray_record_find_locked(xarray);
    if (record == NULL) {
        xarray_records_release();
        return NULL;
    }
    kb_fs_xarray_entry_t **cursor = &record->entries;
    while (*cursor != NULL && (*cursor)->index < index) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == NULL || (*cursor)->index != index) {
        xarray_records_release();
        return NULL;
    }
    kb_fs_xarray_entry_t *removed = *cursor;
    void *entry = removed->entry;
    *cursor = removed->next;
    free(removed);
    xarray_write_head(xarray, record->entries == NULL ? NULL : record->entries->entry);
    xarray_records_release();
    return entry;
}

static void *xarray_find_from(
    void *xarray,
    unsigned long *index,
    unsigned long max,
    int after)
{
    if (xarray == NULL || index == NULL) {
        return NULL;
    }
    unsigned long minimum = *index;
    if (after) {
        if (minimum == ULONG_MAX) {
            return NULL;
        }
        minimum++;
    }
    xarray_records_acquire();
    kb_fs_xarray_record_t *record = xarray_record_find_locked(xarray);
    if (record != NULL) {
        for (kb_fs_xarray_entry_t *item = record->entries;
             item != NULL;
             item = item->next)
        {
            if (item->index >= minimum && item->index <= max) {
                *index = item->index;
                void *entry = item->entry;
                xarray_records_release();
                return entry;
            }
        }
    }
    xarray_records_release();
    return NULL;
}

void *kb_fs_subsystem_xa_find(
    void *xarray,
    unsigned long *index,
    unsigned long max,
    unsigned int filter)
{
    (void)filter;
    return xarray_find_from(xarray, index, max, 0);
}

void *kb_fs_subsystem_xa_find_after(
    void *xarray,
    unsigned long *index,
    unsigned long max,
    unsigned int filter)
{
    (void)filter;
    return xarray_find_from(xarray, index, max, 1);
}

void kb_fs_subsystem_xa_destroy(void *xarray)
{
    if (xarray == NULL) {
        return;
    }
    xarray_records_acquire();
    kb_fs_xarray_record_t **cursor = &xarray_records;
    while (*cursor != NULL && (*cursor)->xarray != xarray) {
        cursor = &(*cursor)->next;
    }
    kb_fs_xarray_record_t *record = *cursor;
    if (record != NULL) {
        *cursor = record->next;
        while (record->entries != NULL) {
            kb_fs_xarray_entry_t *entry = record->entries;
            record->entries = entry->next;
            free(entry);
        }
        free(record);
    }
    xarray_write_head(xarray, NULL);
    xarray_records_release();
}

static void buffer_cache_set_dirty(kb_fs_buffer_cache_record_t *record, int dirty)
{
    if (record == NULL || !record->active) {
        return;
    }
    const int was_dirty = record->dirty != 0;
    const int is_dirty = dirty != 0;
    if (was_dirty == is_dirty) {
        return;
    }
    record->dirty = is_dirty;
    if (is_dirty) {
        buffer_cache_dirty_count++;
    } else if (buffer_cache_dirty_count > 0) {
        buffer_cache_dirty_count--;
    }
}

static kb_fs_buffer_cache_record_t *buffer_cache_record_for_head(void *buffer_head)
{
    if (buffer_head == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (buffer_cache[i].active && buffer_cache[i].buffer_head == buffer_head) {
            return &buffer_cache[i];
        }
    }
    return NULL;
}

static void buffer_cache_release_storage(kb_fs_buffer_cache_record_t *record)
{
    if (record == NULL) {
        return;
    }
    kb_kfree(record->buffer_head);
    if (record->folio != NULL) {
        kb_kvm_free_pages_stub(record->folio, record->folio_order);
    } else {
        kb_kfree(record->data);
    }
}

static void overlay_dirty_buffer_cache_on_read(uint64_t offset, void *buffer, size_t size)
{
    FS_READ_PROFILE_BEGIN(profile_start);
    if (buffer == NULL || size == 0 || buffer_cache_dirty_count == 0) {
        FS_READ_PROFILE_END(overlay, profile_start);
        return;
    }
    uint64_t read_end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &read_end)) {
        FS_READ_PROFILE_END(overlay, profile_start);
        return;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (!buffer_cache[i].active ||
            !buffer_cache[i].dirty ||
            buffer_cache[i].data == NULL ||
            buffer_cache[i].block_size == 0)
        {
            continue;
        }
        uint64_t block_start = 0;
        uint64_t block_end = 0;
        if (__builtin_mul_overflow(buffer_cache[i].block_number, buffer_cache[i].block_size, &block_start) ||
            __builtin_add_overflow(block_start, buffer_cache[i].block_size, &block_end))
        {
            continue;
        }
        if (read_end <= block_start || offset >= block_end) {
            continue;
        }
        uint64_t copy_start = offset > block_start ? offset : block_start;
        uint64_t copy_end = read_end < block_end ? read_end : block_end;
        memcpy(
            (uint8_t *)buffer + (copy_start - offset),
            (const uint8_t *)buffer_cache[i].data + (copy_start - block_start),
            (size_t)(copy_end - copy_start));
    }
    FS_READ_PROFILE_END(overlay, profile_start);
}

int kb_fs_subsystem_flush_dirty_buffers(void)
{
    void *submitted[KB_FS_BUFFER_CACHE_MAX];
    size_t submitted_count = 0;
    int first_error = 0;

    /* Linux buffer writeback submits the dirty set before waiting for it.
     * Serial sync_dirty_buffer() calls made every metadata block pay a full
     * NVMe completion/IRQ round trip and also prevented the existing BIO
     * drain from batching adjacent writes. */
    const int previous_auto_drain = bio_auto_drain;
    bio_auto_drain = 0;
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (!buffer_cache[i].active || !buffer_cache[i].dirty) {
            continue;
        }
        void *buffer_head = buffer_cache[i].buffer_head;
        kb_fs_subsystem_write_dirty_buffer(buffer_head, 0);
        uint64_t state = 0;
        memcpy(&state, buffer_head, sizeof(state));
        if ((state & KB_FS_BH_LOCK) != 0) {
            submitted[submitted_count++] = buffer_head;
        } else if ((state & KB_FS_BH_DIRTY) != 0 && first_error == 0) {
            first_error = -75;
        }
    }

    (void)kb_fs_subsystem_bio_drain();
    if (bio_last_drain_status != 0 && first_error == 0) {
        first_error = bio_last_drain_status;
    }
    bio_auto_drain = previous_auto_drain;

    for (size_t i = 0; i < submitted_count; ++i) {
        kb_fs_subsystem_wait_on_buffer(submitted[i]);
        uint64_t state = 0;
        memcpy(&state, submitted[i], sizeof(state));
        if ((state & (KB_FS_BH_WRITE_EIO | KB_FS_BH_UPTODATE)) !=
                KB_FS_BH_UPTODATE &&
            first_error == 0)
        {
            first_error = -5;
        }
    }
    return first_error;
}

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

static int fs_trace_enabled(void);

static void call_put_super(void *super_block)
{
    if (super_block == NULL) {
        return;
    }
    void *super_ops = read_pointer_field(
        super_block,
        KB_FS_SUPER_BLOCK_OPS_OFFSET);
    if (super_ops == NULL) {
        return;
    }
    void *put_super_op = read_pointer_field(super_ops, KB_FS_SUPER_OP_PUT_SUPER_OFFSET);
    if (put_super_op != NULL) {
        if (fs_trace_enabled()) {
            fprintf(stderr, "kobox-fs: probe unmount put_super begin\n");
        }
        void (*put_super_fn)(void *) = NULL;
        memcpy(&put_super_fn, &put_super_op, sizeof(put_super_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(put_super_op, &old_gs);
        put_super_fn(super_block);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (fs_trace_enabled()) {
            fprintf(stderr, "kobox-fs: probe unmount put_super end\n");
        }
    }
}

void kb_fs_subsystem_kill_block_super(void *super_block)
{
    if (super_block == NULL) {
        return;
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: kill_block_super begin super=%p\n", super_block);
    }
    void *bdev = read_pointer_field(
        super_block,
        KB_FS_SUPER_BLOCK_BDEV_OFFSET);
    (void)kb_fs_subsystem_sync_filesystem(super_block);
    call_put_super(super_block);
    if (bdev != NULL) {
        (void)kb_fs_subsystem_sync_blockdev(bdev);
    }
    if (super_block == mount_probe_super_block) {
        mount_probe_mounted = 0;
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: kill_block_super end super=%p\n", super_block);
    }
}

static void unmount_probe_super_block(void)
{
    if (!mount_probe_mounted || mount_probe_super_block == NULL) {
        return;
    }
    void *kill_sb = last_mount_path_probe.fs_type == NULL ? NULL :
        read_pointer_field(
            last_mount_path_probe.fs_type,
            KB_FS_TYPE_KILL_SB_OFFSET);
    if (kill_sb != NULL) {
        void (*kill_sb_fn)(void *) = NULL;
        memcpy(&kill_sb_fn, &kill_sb, sizeof(kill_sb_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(kill_sb, &old_gs);
        kill_sb_fn(mount_probe_super_block);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    } else {
        (void)kb_fs_subsystem_sync_filesystem(mount_probe_super_block);
        call_put_super(mount_probe_super_block);
    }
    mount_probe_mounted = 0;
}

static void clear_mount_probe_objects(void)
{
    unmount_probe_super_block();
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (!buffer_cache[i].active) {
            continue;
        }
        buffer_cache_release_storage(&buffer_cache[i]);
    }
    memset(buffer_cache, 0, sizeof(buffer_cache));
    buffer_cache_dirty_count = 0;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (!filemap_folio_cache[i].active || filemap_folio_cache[i].folio == NULL) {
            continue;
        }
        kb_fs_subsystem_folio_put(filemap_folio_cache[i].folio);
    }
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
    mount_probe_mounted = 0;
    memset(&active_bdev_binding, 0, sizeof(active_bdev_binding));
}

static int low_or_err_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    if (ptr == NULL || value < 4096u || value >= UINTPTR_MAX - 4095u) {
        return 1;
    }
#if defined(__x86_64__)
    if (value > UINT64_C(0x00007fffffffffff) &&
        value < UINT64_C(0xffff800000000000))
    {
        return 1;
    }
#endif
    return 0;
}

static void *fs_err_ptr(int status)
{
    int err = status < 0 ? status : -status;
    return (void *)(intptr_t)err;
}

static int fs_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_FS");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void *read_pointer_field(const void *base, size_t offset);
static void write_u64_field(void *base, size_t offset, uint64_t value);
static int kb_fs_enter_ext4_call(void *function, unsigned long *old_gs);

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

static uint32_t read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
}

static uint64_t read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
}

static uint8_t read_u8_field(const void *base, size_t offset)
{
    uint8_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
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

static uint32_t kb_fs_dentry_type_for_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return 0;
    }
    uint32_t mode = 0;
    memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
    return (mode & 0170000u) == 0040000u ?
        KB_FS_DENTRY_DIRECTORY_TYPE :
        KB_FS_DENTRY_REGULAR_TYPE;
}

static void kb_fs_set_dentry_entry_type(void *dentry, void *inode)
{
    if (dentry == NULL) {
        return;
    }
    uint32_t flags = 0;
    memcpy(&flags, (const uint8_t *)dentry + KB_FS_DENTRY_FLAGS_OFFSET, sizeof(flags));
    flags &= ~KB_FS_DENTRY_ENTRY_TYPE_MASK;
    flags |= kb_fs_dentry_type_for_inode(inode);
    memcpy((uint8_t *)dentry + KB_FS_DENTRY_FLAGS_OFFSET, &flags, sizeof(flags));
}

static int kb_fs_inode_is_devpts(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return 0;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    if (super_block == NULL) {
        return 0;
    }
    uint64_t magic = 0;
    memcpy(&magic, (const uint8_t *)super_block + KB_FS_DEVPTS_SUPER_MAGIC_OFFSET, sizeof(magic));
    return magic == KB_FS_DEVPTS_SUPER_MAGIC;
}

static int kb_fs_super_is_devpts(void *super_block)
{
    if (low_or_err_pointer(super_block)) {
        return 0;
    }
    uint64_t magic = 0;
    memcpy(&magic,
        (const uint8_t *)super_block + KB_FS_DEVPTS_SUPER_MAGIC_OFFSET,
        sizeof(magic));
    return magic == KB_FS_DEVPTS_SUPER_MAGIC;
}

static size_t kb_fs_dentry_sb_offset_for_super(void *super_block)
{
    return kb_fs_super_is_devpts(super_block) ?
        KB_FS_DEVPTS_DENTRY_SB_OFFSET : KB_FS_DENTRY_SB_OFFSET;
}

static void kb_fs_write_dentry_super(void *dentry, void *super_block)
{
    if (dentry == NULL) {
        return;
    }
    write_pointer_field(
        dentry,
        kb_fs_dentry_sb_offset_for_super(super_block),
        super_block);
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
    kb_fs_write_dentry_super(dentry, super_block);
    kb_fs_prepare_dentry_name(dentry, name);
    if (!low_or_err_pointer(inode)) {
        write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
        kb_fs_set_dentry_entry_type(dentry, inode);
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

typedef struct kb_fs_ext4_smoke_ops {
    int (*setattr)(void *, void *, void *);
} kb_fs_ext4_smoke_ops_t;

static long kb_fs_ext4_smoke_perform_write_locked(
    void *inode,
    void *kiocb,
    void *iter)
{
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    void *rwsem = (uint8_t *)inode + KB_FS_INODE_RWSEM_OFFSET;
    kb_down_write(rwsem);
    const long result = kb_fs_subsystem_generic_perform_write(kiocb, iter);
    kb_up_write(rwsem);
    return result;
}

typedef struct kb_fs_ext4_smoke_inode_lock_set {
    void *inodes[4];
    size_t count;
} kb_fs_ext4_smoke_inode_lock_set_t;

static void kb_fs_ext4_smoke_inode_lock_set_add(
    kb_fs_ext4_smoke_inode_lock_set_t *set,
    void *inode)
{
    if (set == NULL || low_or_err_pointer(inode)) {
        return;
    }
    for (size_t i = 0; i < set->count; ++i) {
        if (set->inodes[i] == inode) {
            return;
        }
    }
    if (set->count < sizeof(set->inodes) / sizeof(set->inodes[0])) {
        set->inodes[set->count++] = inode;
    }
}

static void kb_fs_ext4_smoke_inode_lock_set_acquire(
    kb_fs_ext4_smoke_inode_lock_set_t *set)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = 1; i < set->count; ++i) {
        void *inode = set->inodes[i];
        size_t j = i;
        while (j != 0 && (uintptr_t)set->inodes[j - 1] > (uintptr_t)inode) {
            set->inodes[j] = set->inodes[j - 1];
            --j;
        }
        set->inodes[j] = inode;
    }
    for (size_t i = 0; i < set->count; ++i) {
        kb_down_write((uint8_t *)set->inodes[i] + KB_FS_INODE_RWSEM_OFFSET);
    }
}

static void kb_fs_ext4_smoke_inode_lock_set_release(
    const kb_fs_ext4_smoke_inode_lock_set_t *set)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = set->count; i != 0; --i) {
        kb_up_write((uint8_t *)set->inodes[i - 1] + KB_FS_INODE_RWSEM_OFFSET);
    }
}

static int kb_fs_ext4_smoke_apply_iattr(
    void *super_block,
    void *inode,
    void *dentry,
    void *iattr,
    const kb_fs_ext4_smoke_ops_t *ops,
    const char *label)
{
    if (super_block == NULL || low_or_err_pointer(inode) || dentry == NULL ||
        iattr == NULL || ops == NULL || ops->setattr == NULL)
    {
        return -22;
    }

    static uint8_t smoke_mnt_idmap[136];
    void *rwsem = (uint8_t *)inode + KB_FS_INODE_RWSEM_OFFSET;
    kb_down_write(rwsem);
    unsigned long old_gs = 0;
    int has_gs = kb_fs_enter_ext4_call((void *)ops->setattr, &old_gs);
    const int setattr_result = ops->setattr(smoke_mnt_idmap, dentry, iattr);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_up_write(rwsem);
    if (setattr_result != 0) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s setattr failed result=%d\n",
            label == NULL ? "setattr" : label,
            setattr_result);
        return setattr_result;
    }

    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    void *file_operations = read_pointer_field(inode, KB_FS_INODE_FILE_OP_OFFSET);
    if (mapping == NULL || file_operations == NULL) {
        return -95;
    }
    uint8_t file[KB_FS_FAKE_INODE_BYTES];
    memset(file, 0, sizeof(file));
    write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, file_operations);
    write_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    const int fsync_result = kb_fs_subsystem_vfs_fsync_range(
        file,
        0,
        INT64_MAX,
        0);
    if (fsync_result != 0) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s fsync failed result=%d\n",
            label == NULL ? "setattr" : label,
            fsync_result);
    }
    return fsync_result;
}

static int kb_fs_ext4_sync_inode_size(
    void *super_block,
    void *inode,
    void *dentry,
    uint64_t size,
    const kb_fs_ext4_smoke_ops_t *ops,
    const char *label)
{
    if (size > INT64_MAX) {
        return -22;
    }
    uint8_t iattr[80];
    memset(iattr, 0, sizeof(iattr));
    write_u32_field(iattr, KB_FS_IATTR_VALID_OFFSET, KB_FS_ATTR_SIZE);
    write_u64_field(iattr, KB_FS_IATTR_SIZE_OFFSET, size);
    const int result = kb_fs_ext4_smoke_apply_iattr(
        super_block,
        inode,
        dentry,
        iattr,
        ops,
        label);

    uint64_t observed_size = 0;
    memcpy(&observed_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(observed_size));
    if (result != 0 || observed_size != size) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s failed result=%d size=%llu expected=%llu\n",
            label == NULL ? "truncate" : label,
            result,
            (unsigned long long)observed_size,
            (unsigned long long)size);
        return result != 0 ? result : -5;
    }
    return 0;
}

static int kb_fs_ext4_sync_inode_metadata(
    void *super_block,
    void *inode,
    void *dentry,
    uint16_t mode,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec,
    const kb_fs_ext4_smoke_ops_t *ops,
    const char *label)
{
    if (super_block == NULL || low_or_err_pointer(inode) || dentry == NULL ||
        ops == NULL || ops->setattr == NULL ||
        atime_nsec < 0 || atime_nsec >= 1000000000ll ||
        mtime_nsec < 0 || mtime_nsec >= 1000000000ll)
    {
        return -22;
    }

    uint16_t old_mode = 0;
    memcpy(&old_mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(old_mode));
    const uint16_t new_mode = (uint16_t)((old_mode & KB_FS_MODE_TYPE_MASK) | (mode & KB_FS_MODE_PERM_MASK));
    uint8_t iattr[80];
    memset(iattr, 0, sizeof(iattr));
    write_u32_field(
        iattr,
        KB_FS_IATTR_VALID_OFFSET,
        KB_FS_ATTR_MODE | KB_FS_ATTR_ATIME | KB_FS_ATTR_MTIME | KB_FS_ATTR_CTIME);
    memcpy(iattr + KB_FS_IATTR_MODE_OFFSET, &new_mode, sizeof(new_mode));
    write_u64_field(iattr, KB_FS_IATTR_ATIME_OFFSET, (uint64_t)atime_sec);
    write_u64_field(iattr, KB_FS_IATTR_ATIME_OFFSET + sizeof(int64_t), (uint64_t)atime_nsec);
    write_u64_field(iattr, KB_FS_IATTR_MTIME_OFFSET, (uint64_t)mtime_sec);
    write_u64_field(iattr, KB_FS_IATTR_MTIME_OFFSET + sizeof(int64_t), (uint64_t)mtime_nsec);
    write_u64_field(iattr, KB_FS_IATTR_CTIME_OFFSET, (uint64_t)mtime_sec);
    write_u64_field(iattr, KB_FS_IATTR_CTIME_OFFSET + sizeof(int64_t), (uint64_t)mtime_nsec);
    const int result = kb_fs_ext4_smoke_apply_iattr(
        super_block,
        inode,
        dentry,
        iattr,
        ops,
        label);

    uint16_t observed_mode = 0;
    memcpy(&observed_mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(observed_mode));
    if (result != 0 || observed_mode != new_mode) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s failed result=%d mode=%o expected=%o\n",
            label == NULL ? "metadata" : label,
            result,
            observed_mode,
            new_mode);
        return result != 0 ? result : -5;
    }
    return 0;
}

static int kb_fs_ext4_smoke_write_payload(
    void *inode,
    void *dentry,
    void *vfsmount,
    const void *payload,
    uint64_t payload_len,
    const char *label)
{
    if (low_or_err_pointer(inode) || dentry == NULL || vfsmount == NULL ||
        payload == NULL || payload_len == 0)
    {
        return -22;
    }

    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    void *file_operations = read_pointer_field(inode, KB_FS_INODE_FILE_OP_OFFSET);
    void *open_operation = file_operations == NULL ? NULL :
        read_pointer_field(file_operations, KB_FS_FILE_OP_OPEN_OFFSET);
    void *release_operation = file_operations == NULL ? NULL :
        read_pointer_field(file_operations, KB_FS_FILE_OP_RELEASE_OFFSET);
    if (mapping == NULL || file_operations == NULL ||
        open_operation == NULL || release_operation == NULL)
    {
        return -95;
    }
    uint8_t file[KB_FS_FAKE_INODE_BYTES];
    uint8_t kiocb[64];
    uint8_t iter[64];
    memset(file, 0, sizeof(file));
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET, KB_FS_FMODE_WRITE);
    write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, file_operations);
    write_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    write_pointer_field(
        file,
        KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET - sizeof(void *),
        vfsmount);
    write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET, dentry);

    uint32_t writecount = read_u32_field(inode, KB_FS_INODE_WRITECOUNT_OFFSET);
    if (writecount == UINT32_MAX) {
        return -75;
    }
    write_u32_field(inode, KB_FS_INODE_WRITECOUNT_OFFSET, writecount + 1u);
    int (*open_fn)(void *, void *) = NULL;
    memcpy(&open_fn, &open_operation, sizeof(open_fn));
    unsigned long old_gs = 0;
    int has_gs = kb_fs_enter_ext4_call(open_operation, &old_gs);
    int open_result = open_fn(inode, file);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (open_result != 0) {
        write_u32_field(inode, KB_FS_INODE_WRITECOUNT_OFFSET, writecount);
        return open_result;
    }
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, payload_len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, (void *)payload);
    long write_result = kb_fs_ext4_smoke_perform_write_locked(inode, kiocb, iter);
    int result = 0;
    if (write_result != (long)payload_len) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s write failed result=%ld expected=%llu\n",
            label == NULL ? "payload" : label,
            write_result,
            (unsigned long long)payload_len);
        result = write_result < 0 ? (int)write_result : -5;
    }
    uint64_t observed_size = 0;
    memcpy(
        &observed_size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(observed_size));
    if (result == 0 && observed_size != payload_len) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s native size update failed size=%llu expected=%llu\n",
            label == NULL ? "payload" : label,
            (unsigned long long)observed_size,
            (unsigned long long)payload_len);
        result = -5;
    }
    if (result == 0) {
        result = kb_fs_subsystem_vfs_fsync_range(
            file,
            0,
            (int64_t)payload_len - 1,
            0);
    }
    if (result != 0 && write_result == (long)payload_len) {
        fprintf(stderr,
            "kobox-ext4-smoke: %s fsync failed result=%d\n",
            label == NULL ? "payload" : label,
            result);
    }
    int (*release_fn)(void *, void *) = NULL;
    memcpy(&release_fn, &release_operation, sizeof(release_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(release_operation, &old_gs);
    const int release_result = release_fn(inode, file);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    const uint32_t final_writecount = read_u32_field(
        inode,
        KB_FS_INODE_WRITECOUNT_OFFSET);
    if (final_writecount == 0) {
        return result != 0 ? result : -5;
    }
    write_u32_field(
        inode,
        KB_FS_INODE_WRITECOUNT_OFFSET,
        final_writecount - 1u);
    return result != 0 ? result : release_result;
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

static int image_block_flush(void *ctx)
{
    kb_fs_image_block_ctx_t *image = (kb_fs_image_block_ctx_t *)ctx;
    if (image == NULL || image->path == NULL) {
        return -22;
    }
#if defined(__pachaos__)
    /* File-backed block images are a host test facility.  Do not claim
     * durability on PachaOS, where this backend is not connected to filed. */
    return -95;
#else
    FILE *file = fopen(image->path, "r+b");
    if (file == NULL) {
        return -5;
    }
    int status = 0;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        status = -5;
    }
    if (fclose(file) != 0 && status == 0) {
        status = -5;
    }
    return status;
#endif
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

static int block_disk_read_batch(
    void *ctx,
    const kb_fs_block_read_request_t *requests,
    size_t request_count)
{
    enum { BLOCK_DISK_READ_BATCH_MAX = KB_FS_BIO_READ_BATCH_MAX };
    kb_fs_block_disk_ctx_t *disk_ctx = (kb_fs_block_disk_ctx_t *)ctx;
    if (disk_ctx == NULL || disk_ctx->disk == NULL || requests == NULL ||
        request_count < 2 || request_count > BLOCK_DISK_READ_BATCH_MAX)
    {
        return -22;
    }
    kb_block_disk_read_request_t batch[BLOCK_DISK_READ_BATCH_MAX];
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL || requests[i].size == 0 ||
            (requests[i].offset % 512u) != 0 ||
            (requests[i].size % 512u) != 0)
        {
            return -22;
        }
        uint64_t sector = 0;
        if (__builtin_add_overflow(
                disk_ctx->start_sector,
                requests[i].offset / 512u,
                &sector))
        {
            return -34;
        }
        batch[i].sector = sector;
        batch[i].buffer = requests[i].buffer;
        batch[i].byte_count = requests[i].size;
    }
    return kb_block_subsystem_disk_read_batch(
        disk_ctx->disk, batch, request_count);
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

static int block_disk_write_batch(
    void *ctx,
    const kb_fs_block_write_request_t *requests,
    size_t request_count)
{
    enum { BLOCK_DISK_WRITE_BATCH_MAX = KB_FS_BIO_WRITE_BATCH_MAX };
    kb_fs_block_disk_ctx_t *disk_ctx = (kb_fs_block_disk_ctx_t *)ctx;
    if (disk_ctx == NULL || disk_ctx->disk == NULL || requests == NULL ||
        request_count < 2 || request_count > BLOCK_DISK_WRITE_BATCH_MAX)
    {
        return -22;
    }
    kb_block_disk_write_request_t batch[BLOCK_DISK_WRITE_BATCH_MAX];
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL || requests[i].size == 0 ||
            (requests[i].offset % 512u) != 0 ||
            (requests[i].size % 512u) != 0)
        {
            return -22;
        }
        uint64_t sector = 0;
        if (__builtin_add_overflow(
                disk_ctx->start_sector,
                requests[i].offset / 512u,
                &sector))
        {
            return -34;
        }
        batch[i].sector = sector;
        batch[i].buffer = requests[i].buffer;
        batch[i].byte_count = requests[i].size;
    }
    return kb_block_subsystem_disk_write_batch(
        disk_ctx->disk, batch, request_count);
}

static int block_disk_write_flags(
    void *ctx,
    uint64_t offset,
    const void *buffer,
    size_t size,
    uint32_t flags)
{
    kb_fs_block_disk_ctx_t *disk_ctx = (kb_fs_block_disk_ctx_t *)ctx;
    if (disk_ctx == NULL || disk_ctx->disk == NULL ||
        (flags & ~KB_FS_BLOCK_WRITE_FUA) != 0 ||
        (offset % 512u) != 0 || (size % 512u) != 0)
    {
        return -22;
    }
    uint64_t sector = 0;
    if (__builtin_add_overflow(
            disk_ctx->start_sector, offset / 512u, &sector))
    {
        return -34;
    }
    uint32_t block_flags = 0;
    if ((flags & KB_FS_BLOCK_WRITE_FUA) != 0) {
        block_flags |= KB_BLOCK_DISK_WRITE_FUA;
    }
    return kb_block_subsystem_disk_write_flags(
        disk_ctx->disk,
        sector,
        buffer,
        size,
        block_flags);
}

static int block_disk_flush(void *ctx)
{
    kb_fs_block_disk_ctx_t *disk_ctx = (kb_fs_block_disk_ctx_t *)ctx;
    if (disk_ctx == NULL || disk_ctx->disk == NULL) {
        return -22;
    }
    return kb_block_subsystem_disk_flush(disk_ctx->disk);
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
    FS_READ_PROFILE_BEGIN(profile_start);
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
        FS_READ_PROFILE_STATUS_RETURN(profile_start, status, 0);
    }
    if (desc->out_bytes != NULL) {
        *desc->out_bytes = request.output_size;
    }
    if (!request.handled) {
        status = fs_local_read(desc);
        FS_READ_PROFILE_STATUS_RETURN(
            profile_start,
            status,
            status == KB_OK && desc->out_bytes != NULL ? *desc->out_bytes : 0);
    }
    status = request.result_code == 0 ? KB_OK : KB_ERR_IO;
    FS_READ_PROFILE_STATUS_RETURN(
        profile_start,
        status,
        status == KB_OK ? request.output_size : 0);
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

void *kb_fs_subsystem_mount_registered(const char *name, int flags, const char *dev_name, void *data)
{
    kb_fs_type_record_t *record = find_fs_type_by_name(name);
    if (record == NULL || !record->active || record->fs_type == NULL) {
        return fs_err_ptr(-19);
    }
    void *mount_fn = read_pointer_field(record->fs_type, 0x20);
    if (mount_fn == NULL) {
        return fs_err_ptr(-95);
    }

    typedef void *(*kb_fs_mount_fn_t)(void *, int, const char *, void *);
    unsigned long old_gs = 0;
    int has_context = record->owner_module != NULL &&
        kb_loader_enter_module_context(record->owner_module, &old_gs) == KB_OK;
    void *root = ((kb_fs_mount_fn_t)mount_fn)(
        record->fs_type,
        flags,
        dev_name == NULL ? name : dev_name,
        data);
    if (has_context) {
        kb_loader_leave_module_context(old_gs);
    }
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: mount_registered name=%s root=%p owner=%p\n",
            name == NULL ? "(null)" : name,
            root,
            (void *)record->owner_module);
    }
    return root;
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
    device->read_batch = desc->read_batch;
    device->write = desc->write;
    device->write_batch = desc->write_batch;
    device->write_flags = desc->write_flags;
    device->flush = desc->flush;
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
        .flush = image_block_flush,
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
        .read_batch = block_disk_read_batch,
        .write = block_disk_write,
        .write_batch = block_disk_write_batch,
        .write_flags = block_disk_write_flags,
        .flush = block_disk_flush,
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
    KB_FS_TRACE_INCREMENT(block_read_calls);
    KB_FS_TRACE_ADD(block_read_bytes, size);
    FS_READ_PROFILE_BEGIN(profile_start);
    status = device->read(device->ctx, offset, buffer, size);
    FS_READ_PROFILE_END(device_read, profile_start);
    if (status == 0 && device == active_bdev_binding.device) {
        overlay_dirty_buffer_cache_on_read(offset, buffer, size);
    }
    return status;
}

int kb_fs_block_device_read_batch(
    kb_fs_block_device_t *device,
    const kb_fs_block_read_request_t *requests,
    size_t request_count)
{
    if (device == NULL || requests == NULL || request_count == 0) {
        return -22;
    }
    if (request_count == 1) {
        return kb_fs_block_device_read(
            device,
            requests[0].offset,
            requests[0].buffer,
            requests[0].size);
    }
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL ||
            block_device_range_valid(
                device, requests[i].offset, requests[i].size) != 0)
        {
            return -22;
        }
    }
    int status = 0;
    FS_READ_PROFILE_BEGIN(profile_start);
    if (device->read_batch != NULL) {
        status = device->read_batch(device->ctx, requests, request_count);
    } else {
        for (size_t i = 0; i < request_count; ++i) {
            status = device->read(
                device->ctx,
                requests[i].offset,
                requests[i].buffer,
                requests[i].size);
            if (status != 0) {
                break;
            }
        }
    }
    FS_READ_PROFILE_END(device_read, profile_start);
    if (status == 0) {
        for (size_t i = 0; i < request_count; ++i) {
            KB_FS_TRACE_INCREMENT(block_read_calls);
            KB_FS_TRACE_ADD(block_read_bytes, requests[i].size);
            if (device == active_bdev_binding.device) {
                overlay_dirty_buffer_cache_on_read(
                    requests[i].offset,
                    requests[i].buffer,
                    requests[i].size);
            }
        }
    }
    return status;
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
    KB_FS_TRACE_INCREMENT(block_write_calls);
    KB_FS_TRACE_ADD(block_write_bytes, size);
    status = device->write(device->ctx, offset, buffer, size);
    if (status == 0 && device == active_bdev_binding.device) {
        update_buffer_cache_from_write(offset, buffer, size);
    }
    return status;
}

int kb_fs_block_device_write_batch(
    kb_fs_block_device_t *device,
    const kb_fs_block_write_request_t *requests,
    size_t request_count)
{
    if (device == NULL || requests == NULL || request_count == 0) {
        return -22;
    }
    if (request_count == 1) {
        return kb_fs_block_device_write(
            device,
            requests[0].offset,
            requests[0].buffer,
            requests[0].size);
    }
    for (size_t i = 0; i < request_count; ++i) {
        if (requests[i].buffer == NULL ||
            block_device_range_valid(device, requests[i].offset, requests[i].size) != 0)
        {
            return -22;
        }
    }
    int status = 0;
    if (device->write_batch != NULL) {
        status = device->write_batch(device->ctx, requests, request_count);
    } else {
        for (size_t i = 0; i < request_count; ++i) {
            status = device->write == NULL ? -95 : device->write(
                device->ctx,
                requests[i].offset,
                requests[i].buffer,
                requests[i].size);
            if (status != 0) {
                break;
            }
        }
    }
    if (status == 0) {
        for (size_t i = 0; i < request_count; ++i) {
            KB_FS_TRACE_INCREMENT(block_write_calls);
            KB_FS_TRACE_ADD(block_write_bytes, requests[i].size);
            if (device == active_bdev_binding.device) {
                update_buffer_cache_from_write(
                    requests[i].offset,
                    requests[i].buffer,
                    requests[i].size);
            }
        }
    }
    return status;
}

int kb_fs_subsystem_issue_flush(void *bdev)
{
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (device == NULL) {
        return -19;
    }
    return device->flush == NULL ? 0 : device->flush(device->ctx);
}

int kb_fs_subsystem_issue_discard(
    void *bdev,
    uint64_t sector,
    uint64_t sector_count,
    unsigned int gfp)
{
    (void)gfp;
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (device == NULL) {
        return -19;
    }
    uint64_t offset = 0;
    uint64_t byte_count = 0;
    if (__builtin_mul_overflow(sector, UINT64_C(512), &offset) ||
        __builtin_mul_overflow(sector_count, UINT64_C(512), &byte_count) ||
        offset > device->size_bytes || byte_count > device->size_bytes - offset)
    {
        return -34;
    }
    /* Discard is advisory, but reporting success without issuing it makes
     * FITRIM and ext4 discard accounting lie.  The current block abstraction
     * has no discard callback, so expose the real lack of support. */
    return -95;
}

int kb_fs_subsystem_issue_zeroout(
    void *bdev,
    uint64_t sector,
    uint64_t sector_count,
    unsigned int gfp,
    unsigned int flags)
{
    (void)gfp;
    enum {
        ZERO_CHUNK_BYTES = 64 * 1024,
        BLKDEV_ZERO_NOFALLBACK = 1u << 1,
    };
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (device == NULL) {
        return -19;
    }
    if ((flags & BLKDEV_ZERO_NOFALLBACK) != 0) {
        return -95;
    }
    uint64_t offset = 0;
    uint64_t byte_count = 0;
    if (__builtin_mul_overflow(sector, UINT64_C(512), &offset) ||
        __builtin_mul_overflow(sector_count, UINT64_C(512), &byte_count) ||
        offset > device->size_bytes || byte_count > device->size_bytes - offset)
    {
        return -34;
    }
    if (byte_count == 0) {
        return 0;
    }
    uint8_t *zeros = calloc(1, ZERO_CHUNK_BYTES);
    if (zeros == NULL) {
        return -12;
    }
    int status = 0;
    while (byte_count != 0) {
        const size_t chunk = byte_count > ZERO_CHUNK_BYTES ?
            ZERO_CHUNK_BYTES : (size_t)byte_count;
        status = kb_fs_block_device_write(device, offset, zeros, chunk);
        if (status != 0) {
            break;
        }
        offset += chunk;
        byte_count -= chunk;
    }
    free(zeros);
    return status;
}

int kb_fs_subsystem_sync_blockdev(void *bdev)
{
    const int write_status = kb_fs_subsystem_flush_dirty_buffers();
    const int flush_status = kb_fs_subsystem_issue_flush(bdev);
    return write_status != 0 ? write_status : flush_status;
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
    (void)gfp;
    (void)bioset;
    if (nr_vecs > KB_FS_BIO_MAX_VECS) {
        return NULL;
    }
    kb_fs_bio_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return NULL;
    }
    record->bio.bdev = bdev;
    record->bio.opf = opf;
    record->bio.max_vectors = nr_vecs;
    record->bio.refcount = 1;
    record->bio.vectors = record->bio.inline_vectors;
    record->magic = KB_FS_BIO_MAGIC;
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

static int bio_add_vector(
    kb_fs_bio_record_t *record,
    void *page,
    void *buffer,
    size_t len,
    size_t page_offset)
{
    if (record == NULL || page == NULL || buffer == NULL || len == 0 ||
        len > UINT32_MAX || page_offset > UINT32_MAX ||
        record->bio.vector_count >= record->bio.max_vectors ||
        record->bio.vector_count >= KB_FS_BIO_MAX_VECS ||
        record->bio.size > UINT32_MAX - (uint32_t)len)
    {
        return 0;
    }
    kb_fs_linux_bio_vec_t *vector =
        &record->bio.vectors[record->bio.vector_count++];
    vector->page = page;
    vector->length = (uint32_t)len;
    vector->offset = (uint32_t)page_offset;
    record->bio.size += (uint32_t)len;
    if (record->buffer == NULL) {
        record->buffer = buffer;
        record->page_offset = page_offset;
    }
    record->len += len;
    return (int)len;
}

int kb_fs_subsystem_bio_add_folio(void *bio, void *folio, size_t len, size_t offset)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL || folio == NULL) {
        return 0;
    }
    void *payload = folio_page_payload(folio);
    if (payload == NULL) {
        payload = folio;
    }
    return bio_add_vector(
        record,
        folio,
        (uint8_t *)payload + offset,
        len,
        offset) != 0;
}

int kb_fs_subsystem_bio_add_page(void *bio, void *page, unsigned int len, unsigned int offset)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL || page == NULL) {
        return 0;
    }
    void *payload = folio_page_payload(page);
    if (payload == NULL) {
        payload = page;
    }
    return bio_add_vector(
        record,
        page,
        (uint8_t *)payload + offset,
        len,
        offset);
}

void kb_fs_subsystem_bio_set_sector(void *bio, uint64_t sector)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record != NULL) {
        record->bio.sector = sector;
    }
}

void kb_fs_subsystem_bio_set_end_io(void *bio, void (*end_io)(void *))
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record != NULL) {
        record->bio.end_io = end_io;
    }
}

static void bio_call_end_io(kb_fs_bio_record_t *record)
{
    if (record == NULL || record->completed) {
        return;
    }
    record->completed = 1;
    if (record->bio.end_io == NULL) {
        return;
    }
    void (*end_io)(void *) = record->bio.end_io;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)end_io);
    if (kernel_gs != 0) {
        kb_linux_call_void_ptr_gs(end_io, record, kernel_gs);
    } else {
        kb_linux_call_void_ptr(end_io, record);
    }
}

void kb_fs_subsystem_bio_endio(void *bio)
{
    bio_call_end_io(bio_record_from_handle(bio));
}

static int bio_flush_device(kb_fs_block_device_t *device)
{
    if (device == NULL) {
        return -19;
    }
    if (device->flush == NULL) {
        return -95;
    }
    return device->flush(device->ctx);
}

static int bio_submit_now(kb_fs_bio_record_t *record)
{
    if (record == NULL) {
        return -22;
    }
    kb_fs_block_device_t *device = block_device_for_bdev(record->bio.bdev);
    if (device == NULL) {
        return -19;
    }
    unsigned int op = record->bio.opf & KB_FS_BIO_OP_MASK;
    if (op == KB_FS_BIO_OP_FLUSH) {
        const int status = bio_flush_device(device);
        record->bio.status = status == 0 ? 0 : 1;
        return status;
    }
    const int needs_preflush =
        op == KB_FS_BIO_OP_WRITE &&
        (record->bio.opf & KB_FS_BIO_REQ_PREFLUSH) != 0;
    const int needs_fua =
        op == KB_FS_BIO_OP_WRITE &&
        (record->bio.opf & KB_FS_BIO_REQ_FUA) != 0;
    if (needs_preflush) {
        const int status = bio_flush_device(device);
        if (status != 0) {
            record->bio.status = 1;
            return status;
        }
        /* A zero-length REQ_OP_WRITE|REQ_PREFLUSH is Linux's ordinary
         * standalone cache-flush request. */
        if (record->bio.vector_count == 0 && record->bio.size == 0) {
            record->bio.status = 0;
            return 0;
        }
    }
    if (record->bio.vector_count == 0 || record->bio.size == 0) {
        return -22;
    }
    uint64_t offset = 0;
    if (__builtin_mul_overflow(record->bio.sector, 512ull, &offset)) {
        return -34;
    }
    int status = 0;
    if (op == KB_FS_BIO_OP_READ && record->bio.vector_count >= 2) {
        uint16_t vector_index = 0;
        while (vector_index < record->bio.vector_count) {
            kb_fs_block_read_request_t requests[KB_FS_BIO_READ_BATCH_MAX];
            size_t request_count = 0;
            while (vector_index < record->bio.vector_count &&
                request_count < KB_FS_BIO_READ_BATCH_MAX)
            {
                const kb_fs_linux_bio_vec_t *vector =
                    &record->bio.vectors[vector_index++];
                void *payload = folio_page_payload(vector->page);
                if (payload == NULL) {
                    payload = vector->page;
                }
                if (payload == NULL || vector->length == 0) {
                    status = -22;
                    break;
                }
                requests[request_count].offset = offset;
                requests[request_count].buffer =
                    (uint8_t *)payload + vector->offset;
                requests[request_count].size = vector->length;
                request_count++;
                if (__builtin_add_overflow(
                        offset,
                        (uint64_t)vector->length,
                        &offset))
                {
                    status = -34;
                    break;
                }
            }
            if (status != 0) {
                break;
            }
            status = kb_fs_block_device_read_batch(
                device,
                requests,
                request_count);
            if (status != 0) {
                break;
            }
        }
        record->bio.status = status == 0 ? 0 : 1;
        return status;
    }
    if (op == KB_FS_BIO_OP_WRITE && record->bio.vector_count >= 2) {
        uint16_t vector_index = 0;
        while (vector_index < record->bio.vector_count) {
            kb_fs_block_write_request_t requests[KB_FS_BIO_WRITE_BATCH_MAX];
            size_t request_count = 0;
            while (vector_index < record->bio.vector_count &&
                request_count < KB_FS_BIO_WRITE_BATCH_MAX)
            {
                const kb_fs_linux_bio_vec_t *vector =
                    &record->bio.vectors[vector_index++];
                void *payload = folio_page_payload(vector->page);
                if (payload == NULL) {
                    payload = vector->page;
                }
                if (payload == NULL || vector->length == 0) {
                    status = -22;
                    break;
                }
                requests[request_count].offset = offset;
                requests[request_count].buffer =
                    (const uint8_t *)payload + vector->offset;
                requests[request_count].size = vector->length;
                request_count++;
                if (__builtin_add_overflow(offset, (uint64_t)vector->length, &offset)) {
                    status = -34;
                    break;
                }
            }
            if (status != 0) {
                break;
            }
            status = kb_fs_block_device_write_batch(
                device, requests, request_count);
            if (status != 0) {
                break;
            }
        }
        if (status == 0 && needs_fua) {
            /* The generic bridge does not expose a hardware-FUA bit.  Match
             * Linux's no-native-FUA fallback with a post-write cache flush. */
            status = bio_flush_device(device);
        }
        record->bio.status = status == 0 ? 0 : 1;
        return status;
    }
    int fua_satisfied = 0;
    for (uint16_t i = 0; i < record->bio.vector_count; ++i) {
        const kb_fs_linux_bio_vec_t *vector = &record->bio.vectors[i];
        void *payload = folio_page_payload(vector->page);
        if (payload == NULL) {
            payload = vector->page;
        }
        void *buffer = (uint8_t *)payload + vector->offset;
        if (op == KB_FS_BIO_OP_READ) {
            status = kb_fs_block_device_read(
                device, offset, buffer, vector->length);
        } else if (op == KB_FS_BIO_OP_WRITE) {
            if (needs_fua && record->bio.vector_count == 1 &&
                device->write_flags != NULL)
            {
                status = device->write_flags(
                    device->ctx,
                    offset,
                    buffer,
                    vector->length,
                    KB_FS_BLOCK_WRITE_FUA);
                fua_satisfied = status == 0;
            } else {
                status = kb_fs_block_device_write(
                    device, offset, buffer, vector->length);
            }
        } else {
            status = -95;
        }
        if (status != 0) {
            break;
        }
        if (__builtin_add_overflow(offset, (uint64_t)vector->length, &offset)) {
            status = -34;
            break;
        }
    }
    if (status == 0 && needs_fua && !fua_satisfied) {
        status = bio_flush_device(device);
    }
    record->bio.status = status == 0 ? 0 : 1;
    return status;
}

static void bio_queue_push(kb_fs_bio_record_t *record)
{
    if (record == NULL || record->queued) {
        return;
    }
    record->queued = 1;
    record->completed = 0;
    record->queue_next = NULL;
    if (bio_queue_tail == NULL) {
        bio_queue_head = record;
        bio_queue_tail = record;
    } else {
        bio_queue_tail->queue_next = record;
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
                bio_queue_head = current->queue_next;
            } else {
                prev->queue_next = current->queue_next;
            }
            if (bio_queue_tail == current) {
                bio_queue_tail = prev;
            }
            record->queued = 0;
            record->queue_next = NULL;
            if (bio_queue_depth > 0) {
                bio_queue_depth--;
            }
            return;
        }
        prev = current;
        current = current->queue_next;
    }
    record->queued = 0;
    record->queue_next = NULL;
}

void kb_fs_subsystem_bio_set_auto_drain(int enabled)
{
    bio_auto_drain = enabled ? 1 : 0;
}

size_t kb_fs_subsystem_bio_queue_depth(void)
{
    return bio_queue_depth;
}

static kb_fs_bio_record_t *bio_queue_pop(void)
{
    kb_fs_bio_record_t *record = bio_queue_head;
    if (record == NULL) {
        return NULL;
    }
    bio_queue_head = record->queue_next;
    if (bio_queue_head == NULL) {
        bio_queue_tail = NULL;
    }
    record->queue_next = NULL;
    record->queued = 0;
    if (bio_queue_depth > 0) {
        bio_queue_depth--;
    }
    return record;
}

static int bio_append_write_requests(
    kb_fs_bio_record_t *record,
    kb_fs_block_device_t **out_device,
    kb_fs_block_write_request_t *out_requests,
    size_t request_capacity,
    size_t *out_request_count)
{
    if (record == NULL || out_device == NULL || out_requests == NULL ||
        out_request_count == NULL ||
        (record->bio.opf & KB_FS_BIO_OP_MASK) != KB_FS_BIO_OP_WRITE ||
        (record->bio.opf &
            (KB_FS_BIO_REQ_PREFLUSH | KB_FS_BIO_REQ_FUA)) != 0 ||
        record->bio.vector_count == 0 || record->bio.size == 0 ||
        record->bio.vector_count > request_capacity)
    {
        return -22;
    }
    kb_fs_block_device_t *device = block_device_for_bdev(record->bio.bdev);
    uint64_t offset = 0;
    if (device == NULL ||
        __builtin_mul_overflow(record->bio.sector, 512ull, &offset))
    {
        return -22;
    }
    size_t total_size = 0;
    for (uint16_t i = 0; i < record->bio.vector_count; ++i) {
        const kb_fs_linux_bio_vec_t *vector = &record->bio.vectors[i];
        void *payload = folio_page_payload(vector->page);
        if (payload == NULL) {
            payload = vector->page;
        }
        if (payload == NULL || vector->length == 0 ||
            total_size > SIZE_MAX - vector->length)
        {
            return -22;
        }
        out_requests[i].offset = offset;
        out_requests[i].buffer =
            (const uint8_t *)payload + vector->offset;
        out_requests[i].size = vector->length;
        total_size += vector->length;
        if (__builtin_add_overflow(
                offset, (uint64_t)vector->length, &offset))
        {
            return -34;
        }
    }
    if (total_size != record->bio.size) {
        return -22;
    }
    *out_device = device;
    *out_request_count = record->bio.vector_count;
    return 0;
}

static int bio_prepare_read_request(
    kb_fs_bio_record_t *record,
    kb_fs_block_device_t **out_device,
    kb_fs_block_read_request_t *out_request)
{
    if (record == NULL || out_device == NULL || out_request == NULL ||
        (record->bio.opf & KB_FS_BIO_OP_MASK) != KB_FS_BIO_OP_READ ||
        record->bio.vector_count != 1 || record->bio.size == 0)
    {
        return -22;
    }
    kb_fs_block_device_t *device = block_device_for_bdev(record->bio.bdev);
    const kb_fs_linux_bio_vec_t *vector = &record->bio.vectors[0];
    void *payload = folio_page_payload(vector->page);
    if (payload == NULL) {
        payload = vector->page;
    }
    uint64_t offset = 0;
    if (device == NULL || payload == NULL || vector->length != record->bio.size ||
        __builtin_mul_overflow(record->bio.sector, 512ull, &offset))
    {
        return -22;
    }
    out_request->offset = offset;
    out_request->buffer = (uint8_t *)payload + vector->offset;
    out_request->size = vector->length;
    *out_device = device;
    return 0;
}

static void bio_finish_record(kb_fs_bio_record_t *record, int status)
{
    if (record == NULL) {
        return;
    }
    record->result = status;
    record->bio.status = status == 0 ? 0 : 1;
    const unsigned int op = record->bio.opf & KB_FS_BIO_OP_MASK;
    if (op < 4u) {
        KB_FS_TRACE_ADD(bio_complete[op], 1u);
        if (status != 0) {
            KB_FS_TRACE_ADD(bio_errors[op], 1u);
        }
    }
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: drain_bio bio=%p bdev=%p op=%u sector=%llu len=%zu result=%d\n",
            (void *)record,
            record->bio.bdev,
            op,
            (unsigned long long)record->bio.sector,
            record->len,
            status);
    }
    bio_call_end_io(record);
}

size_t kb_fs_subsystem_bio_drain(void)
{
    size_t drained = 0;
    bio_last_drain_status = 0;
    while (bio_queue_head != NULL) {
        kb_fs_bio_record_t *read_records[KB_FS_BIO_READ_BATCH_MAX];
        kb_fs_block_read_request_t read_requests[KB_FS_BIO_READ_BATCH_MAX];
        kb_fs_block_device_t *read_device = NULL;
        kb_fs_bio_record_t *read_candidate = bio_queue_head;
        size_t read_count = 0;
        while (read_candidate != NULL &&
            read_count < KB_FS_BIO_READ_BATCH_MAX)
        {
            kb_fs_block_device_t *device = NULL;
            if (bio_prepare_read_request(
                    read_candidate,
                    &device,
                    &read_requests[read_count]) != 0 ||
                (read_device != NULL && device != read_device))
            {
                break;
            }
            read_device = device;
            read_records[read_count++] = read_candidate;
            read_candidate = read_candidate->queue_next;
        }

        if (read_count >= 2 && read_device != NULL) {
            for (size_t i = 0; i < read_count; ++i) {
                (void)bio_queue_pop();
            }
            const int status = kb_fs_block_device_read_batch(
                read_device, read_requests, read_count);
            if (status != 0 && bio_last_drain_status == 0) {
                bio_last_drain_status = status;
            }
            if (status != 0) {
                fprintf(stderr,
                    "kobox-fs-error: read batch count=%zu status=%d\n",
                    read_count,
                    status);
            }
            for (size_t i = 0; i < read_count; ++i) {
                bio_finish_record(read_records[i], status);
            }
            drained += read_count;
            continue;
        }

        kb_fs_bio_record_t *batch_records[KB_FS_BIO_WRITE_BATCH_MAX];
        kb_fs_block_write_request_t batch_requests[KB_FS_BIO_WRITE_BATCH_MAX];
        kb_fs_block_device_t *batch_device = NULL;
        kb_fs_bio_record_t *candidate = bio_queue_head;
        size_t batch_record_count = 0;
        size_t batch_request_count = 0;
        while (candidate != NULL &&
            batch_request_count < KB_FS_BIO_WRITE_BATCH_MAX)
        {
            kb_fs_block_device_t *device = NULL;
            size_t appended = 0;
            if (bio_append_write_requests(
                    candidate,
                    &device,
                    &batch_requests[batch_request_count],
                    KB_FS_BIO_WRITE_BATCH_MAX - batch_request_count,
                    &appended) != 0 ||
                (batch_device != NULL && device != batch_device))
            {
                break;
            }
            batch_device = device;
            batch_records[batch_record_count++] = candidate;
            batch_request_count += appended;
            candidate = candidate->queue_next;
        }

        if (batch_request_count >= 2 && batch_device != NULL) {
            for (size_t i = 0; i < batch_record_count; ++i) {
                (void)bio_queue_pop();
            }
            const int status = kb_fs_block_device_write_batch(
                batch_device, batch_requests, batch_request_count);
            if (status != 0 && bio_last_drain_status == 0) {
                bio_last_drain_status = status;
            }
            for (size_t i = 0; i < batch_record_count; ++i) {
                bio_finish_record(batch_records[i], status);
            }
            drained += batch_record_count;
            continue;
        }

        kb_fs_bio_record_t *record = bio_queue_pop();
        const int status = bio_submit_now(record);
        if (status != 0 && bio_last_drain_status == 0) {
            bio_last_drain_status = status;
        }
        bio_finish_record(record, status);
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
    const unsigned int op = record->bio.opf & KB_FS_BIO_OP_MASK;
    if (op < 4u) {
        KB_FS_TRACE_ADD(bio_submit[op], 1u);
    }
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    fs_hotpath_profile.bio_submit_calls++;
    if ((record->bio.opf & KB_FS_BIO_REQ_PREFLUSH) != 0) {
        fs_hotpath_profile.bio_preflush_calls++;
    }
    if ((record->bio.opf & KB_FS_BIO_REQ_FUA) != 0) {
        fs_hotpath_profile.bio_fua_calls++;
    }
    if (op == KB_FS_BIO_OP_FLUSH) {
        fs_hotpath_profile.bio_flush_op_calls++;
    }
#endif
    record->result = -115;
    bio_queue_push(record);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: submit_bio bio=%p bdev=%p op=%u sector=%llu len=%zu queued=%u depth=%zu\n",
            (void *)record,
            record->bio.bdev,
            record->bio.opf & KB_FS_BIO_OP_MASK,
            (unsigned long long)record->bio.sector,
            record->len,
            record->queued,
            bio_queue_depth);
    }
    if (bio_auto_drain && block_plug_depth == 0) {
        (void)kb_fs_subsystem_bio_drain();
    }
}

void kb_fs_subsystem_submit_bio_noacct(void *bio)
{
    kb_fs_subsystem_submit_bio(bio);
}

static void buffer_head_drop_io_reference(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    uint32_t refcount = 0;
    memcpy(
        &refcount,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        sizeof(refcount));
    if (refcount != 0) {
        refcount--;
        memcpy(
            (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
            &refcount,
            sizeof(refcount));
    }
}

void kb_fs_subsystem_end_buffer_read_sync(void *buffer_head, int uptodate)
{
    if (buffer_head == NULL) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if (uptodate) {
        state |= KB_FS_BH_UPTODATE;
    } else {
        state &= ~KB_FS_BH_UPTODATE;
    }
    state &= ~(KB_FS_BH_LOCK | KB_FS_BH_REQ);
    write_u64_field(buffer_head, 0, state);
    buffer_head_drop_io_reference(buffer_head);
}

void kb_fs_subsystem_end_buffer_write_sync(void *buffer_head, int uptodate)
{
    if (buffer_head == NULL) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if (uptodate) {
        state |= KB_FS_BH_UPTODATE;
        state &= ~(KB_FS_BH_DIRTY | KB_FS_BH_WRITE_EIO);
    } else {
        state &= ~KB_FS_BH_UPTODATE;
        state |= KB_FS_BH_WRITE_EIO;
    }
    state &= ~(KB_FS_BH_LOCK | KB_FS_BH_REQ);
    write_u64_field(buffer_head, 0, state);
    if (uptodate) {
        buffer_cache_set_dirty(buffer_cache_record_for_head(buffer_head), 0);
    }
    buffer_head_drop_io_reference(buffer_head);
}

static void buffer_head_complete(void *buffer_head, int success)
{
    void (*end_io)(void *, int) = NULL;
    memcpy(
        &end_io,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_END_IO_OFFSET,
        sizeof(end_io));
    if (success) {
        uint64_t block_number = 0;
        uint64_t block_size = 0;
        void *data = NULL;
        memcpy(&block_number,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET,
            sizeof(block_number));
        memcpy(&block_size,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
            sizeof(block_size));
        memcpy(&data,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_DATA_OFFSET,
            sizeof(data));
        if (data != NULL && block_size == 1024u && block_number == 1u) {
            memcpy(&last_mount_path_probe.observed_ext4_magic,
                (const uint8_t *)data + 0x38u,
                sizeof(last_mount_path_probe.observed_ext4_magic));
        } else if (data != NULL && block_size >= 0x440u && block_number == 0u) {
            memcpy(&last_mount_path_probe.observed_ext4_magic,
                (const uint8_t *)data + 0x438u,
                sizeof(last_mount_path_probe.observed_ext4_magic));
        }
    }
    if (end_io == NULL) {
        kb_fs_subsystem_end_buffer_read_sync(buffer_head, success);
        return;
    }
    unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)end_io);
    if (kernel_gs != 0) {
        kb_linux_call_void_ptr_int_gs(end_io, buffer_head, success, kernel_gs);
    } else {
        kb_linux_call_void_ptr_int(end_io, buffer_head, success);
    }
}

static void buffer_head_bio_end_io(void *bio)
{
    kb_fs_bio_record_t *record = bio_record_from_handle(bio);
    if (record == NULL) {
        return;
    }
    void *buffer_head = record->bio.private_data;
    const int success = record->result == 0 && record->bio.status == 0;
    buffer_head_complete(buffer_head, success);
    kb_fs_subsystem_bio_put(record);
}

void kb_fs_subsystem_blk_start_plug(void *plug)
{
    (void)plug;
    if (block_plug_depth != UINT_MAX) {
        block_plug_depth++;
    }
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    fs_hotpath_profile.plug_start_calls++;
#endif
}

void kb_fs_subsystem_blk_finish_plug(void *plug)
{
    (void)plug;
    if (block_plug_depth == 0) {
        return;
    }
    block_plug_depth--;
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    fs_hotpath_profile.plug_finish_calls++;
    fs_hotpath_profile.plug_queued_bios += bio_queue_depth;
    if (bio_queue_depth > fs_hotpath_profile.plug_max_queued_bios) {
        fs_hotpath_profile.plug_max_queued_bios = bio_queue_depth;
    }
#endif
    if (block_plug_depth == 0) {
        (void)kb_fs_subsystem_bio_drain();
    }
}

void kb_fs_subsystem_submit_bh(unsigned int opf, void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }

    uint64_t state = 0;
    uint64_t block_number = 0;
    uint64_t block_size = 0;
    void *data = NULL;
    void *folio = NULL;
    void *bdev = NULL;
    memcpy(&state, buffer_head, sizeof(state));
    memcpy(
        &block_number,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET,
        sizeof(block_number));
    memcpy(
        &block_size,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
        sizeof(block_size));
    memcpy(
        &data,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_DATA_OFFSET,
        sizeof(data));
    memcpy(
        &folio,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_FOLIO_OFFSET,
        sizeof(folio));
    memcpy(
        &bdev,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_BDEV_OFFSET,
        sizeof(bdev));

    const unsigned int op = opf & KB_FS_BIO_OP_MASK;
    state |= KB_FS_BH_REQ;
    if (op == KB_FS_BIO_OP_WRITE) {
        state &= ~KB_FS_BH_WRITE_EIO;
    }
    write_u64_field(buffer_head, 0, state);

    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    uint64_t offset = 0;
    int status = -22;
    if (device != NULL &&
        data != NULL &&
        block_size != 0 &&
        block_size <= SIZE_MAX &&
        !__builtin_mul_overflow(block_number, block_size, &offset))
    {
        if (op == KB_FS_BIO_OP_READ || op == KB_FS_BIO_OP_WRITE) {
            void *bio_page = data;
            unsigned int bio_page_offset = 0;
            int page_range_valid = 1;
            if (folio != NULL) {
                void *payload = folio_page_payload(folio);
                const uintptr_t payload_address = (uintptr_t)payload;
                const uintptr_t data_address = (uintptr_t)data;
                if (payload == NULL || data_address < payload_address ||
                    data_address - payload_address >= KB_FS_PAGE_SIZE ||
                    block_size > KB_FS_PAGE_SIZE -
                        (data_address - payload_address))
                {
                    page_range_valid = 0;
                } else {
                    bio_page = folio;
                    bio_page_offset =
                        (unsigned int)(data_address - payload_address);
                }
            }
            kb_fs_bio_record_t *record = kb_fs_subsystem_bio_alloc_bioset(
                bdev, 1u, opf, 0u, NULL);
            if (page_range_valid && record != NULL &&
                (offset % 512u) == 0 &&
                block_size <= UINT32_MAX &&
                kb_fs_subsystem_bio_add_page(
                    record,
                    bio_page,
                    (unsigned int)block_size,
                    bio_page_offset) ==
                    (int)block_size)
            {
                record->bio.sector = offset / 512u;
                record->bio.private_data = buffer_head;
                record->bio.end_io = buffer_head_bio_end_io;
                status = 0;
                kb_fs_subsystem_submit_bio(record);
                return;
            }
            if (record != NULL) {
                kb_fs_subsystem_bio_put(record);
            }
        } else {
            status = -95;
        }
    }

    if (op < 4u) {
        KB_FS_TRACE_ADD(bio_complete[op], 1u);
        if (status != 0) {
            KB_FS_TRACE_ADD(bio_errors[op], 1u);
        }
    }
    if (fs_trace_enabled() || status != 0) {
        fprintf(stderr,
            "kobox-fs%s: submit_bh bh=%p bdev=%p device=%p data=%p op=%u "
            "block=%llu size=%llu status=%d caller=%p\n",
            status == 0 ? "" : "-error",
            buffer_head,
            bdev,
            (void *)device,
            data,
            op,
            (unsigned long long)block_number,
            (unsigned long long)block_size,
            status,
            __builtin_return_address(0));
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: submit_bh completion begin bh=%p\n", buffer_head);
    }
    buffer_head_complete(buffer_head, status == 0);
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: submit_bh completion end bh=%p\n", buffer_head);
    }
}

int kb_fs_subsystem_bh_read(void *buffer_head, unsigned int op_flags, int wait)
{
    if (buffer_head == NULL) {
        return -22;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_LOCK) == 0) {
        return -22;
    }
    uint32_t refcount = 0;
    memcpy(&refcount,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        sizeof(refcount));
    if (refcount == UINT32_MAX) {
        return -75;
    }
    refcount++;
    memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        &refcount,
        sizeof(refcount));
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_END_IO_OFFSET,
        (void *)&kb_fs_subsystem_end_buffer_read_sync);
    kb_fs_subsystem_submit_bh(KB_FS_BIO_OP_READ | op_flags, buffer_head);
    if (!wait) {
        return 0;
    }
    kb_fs_subsystem_wait_on_buffer(buffer_head);
    memcpy(&state, buffer_head, sizeof(state));
    return (state & KB_FS_BH_UPTODATE) != 0 ? 0 : -5;
}

void kb_fs_subsystem_bh_read_batch(
    int count,
    void **buffer_heads,
    unsigned int op_flags,
    int force_lock)
{
    if (count <= 0 || buffer_heads == NULL) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        void *buffer_head = buffer_heads[i];
        if (buffer_head == NULL) {
            continue;
        }
        uint64_t state = 0;
        memcpy(&state, buffer_head, sizeof(state));
        if ((state & KB_FS_BH_UPTODATE) != 0) {
            continue;
        }
        if (force_lock) {
            kb_fs_subsystem_lock_buffer(buffer_head);
        } else if ((state & KB_FS_BH_LOCK) != 0) {
            continue;
        } else {
            write_u64_field(buffer_head, 0, state | KB_FS_BH_LOCK);
        }
        memcpy(&state, buffer_head, sizeof(state));
        if ((state & KB_FS_BH_UPTODATE) != 0) {
            kb_fs_subsystem_unlock_buffer(buffer_head);
            continue;
        }
        (void)kb_fs_subsystem_bh_read(buffer_head, op_flags, 0);
    }
}

void kb_fs_subsystem_lock_buffer(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    for (;;) {
        uint64_t state = 0;
        memcpy(&state, buffer_head, sizeof(state));
        if ((state & KB_FS_BH_LOCK) == 0) {
            state |= KB_FS_BH_LOCK;
            write_u64_field(buffer_head, 0, state);
            return;
        }
        (void)kb_fs_subsystem_bio_drain();
        kb_run_deferred_work();
    }
}

void kb_fs_subsystem_unlock_buffer(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    state &= ~KB_FS_BH_LOCK;
    write_u64_field(buffer_head, 0, state);
}

void kb_fs_subsystem_write_dirty_buffer(void *buffer_head, unsigned int op_flags)
{
    if (buffer_head == NULL) {
        return;
    }
    kb_fs_subsystem_lock_buffer(buffer_head);
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_DIRTY) == 0) {
        kb_fs_subsystem_unlock_buffer(buffer_head);
        return;
    }
    state &= ~KB_FS_BH_DIRTY;
    write_u64_field(buffer_head, 0, state);

    uint32_t refcount = 0;
    memcpy(&refcount,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        sizeof(refcount));
    if (refcount == UINT32_MAX) {
        state |= KB_FS_BH_DIRTY;
        write_u64_field(buffer_head, 0, state);
        kb_fs_subsystem_unlock_buffer(buffer_head);
        return;
    }
    refcount++;
    memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        &refcount,
        sizeof(refcount));
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_END_IO_OFFSET,
        (void *)&kb_fs_subsystem_end_buffer_write_sync);
    kb_fs_subsystem_submit_bh(KB_FS_BIO_OP_WRITE | op_flags, buffer_head);
}

void kb_fs_subsystem_wait_on_buffer(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    uint64_t state = 0;
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: wait_on_buffer begin bh=%p\n", buffer_head);
    }
    memcpy(&state, buffer_head, sizeof(state));
    while ((state & KB_FS_BH_LOCK) != 0) {
        (void)kb_fs_subsystem_bio_drain();
        kb_run_deferred_work();
        memcpy(&state, buffer_head, sizeof(state));
    }
    if ((state & KB_FS_BH_UPTODATE) == 0) {
        fprintf(stderr,
            "kobox-fs-error: wait_on_buffer not-uptodate bh=%p state=0x%llx caller=%p\n",
            buffer_head,
            (unsigned long long)state,
            __builtin_return_address(0));
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: wait_on_buffer end bh=%p state=0x%llx\n",
            buffer_head,
            (unsigned long long)state);
    }
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
    out_snapshot->block_device = record->bio.bdev;
    out_snapshot->operation = record->bio.opf & KB_FS_BIO_OP_MASK;
    out_snapshot->sector = record->bio.sector;
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
    return kb_linux_kvm_page_payload(folio, 0, KB_FS_PAGE_SIZE);
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
        void *association_head =
            (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET;
        write_pointer_field(
            buffer_head,
            KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET,
            association_head);
        write_pointer_field(
            buffer_head,
            KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET + sizeof(void *),
            association_head);
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

static void filemap_folio_release_buffers(void *folio);
static void filemap_folio_discard(void *folio);

static void filemap_mapping_set_mark(void *mapping, uint32_t mark, int set)
{
    if (mapping == NULL) {
        return;
    }
    uint32_t flags = 0;
    memcpy(&flags,
        (const uint8_t *)mapping + KB_FS_ADDRESS_SPACE_XARRAY_FLAGS_OFFSET,
        sizeof(flags));
    if (set) {
        flags |= mark;
    } else {
        flags &= ~mark;
    }
    write_u32_field(mapping, KB_FS_ADDRESS_SPACE_XARRAY_FLAGS_OFFSET, flags);
}

static void filemap_mapping_refresh_marks(void *mapping)
{
    int has_dirty = 0;
    int has_writeback = 0;
    int has_towrite = 0;
    if (mapping == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (!record->active || record->mapping != mapping || record->folio == NULL) {
            continue;
        }
        uint64_t flags = 0;
        memcpy(&flags, record->folio, sizeof(flags));
        has_dirty |= (flags & KB_FS_FOLIO_FLAG_DIRTY) != 0;
        has_writeback |= (flags & KB_FS_FOLIO_FLAG_WRITEBACK) != 0;
        has_towrite |= record->towrite != 0;
    }
    filemap_mapping_set_mark(mapping, KB_FS_XARRAY_MARK_DIRTY, has_dirty);
    filemap_mapping_set_mark(mapping, KB_FS_XARRAY_MARK_WRITEBACK, has_writeback);
    filemap_mapping_set_mark(mapping, KB_FS_XARRAY_MARK_TOWRITE, has_towrite);
    void *host = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (!low_or_err_pointer(host)) {
        uint64_t state = 0;
        memcpy(
            &state,
            (const uint8_t *)host + KB_FS_INODE_STATE_OFFSET,
            sizeof(state));
        if (has_dirty) {
            state |= KB_FS_INODE_STATE_DIRTY_PAGES;
        } else {
            state &= ~(uint64_t)KB_FS_INODE_STATE_DIRTY_PAGES;
        }
        write_u64_field(host, KB_FS_INODE_STATE_OFFSET, state);
    }
}

static size_t filemap_mapping_folio_count(void *mapping)
{
    size_t count = 0;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
        if (filemap_folio_cache[i].active &&
            filemap_folio_cache[i].mapping == mapping)
        {
            ++count;
        }
    }
    return count;
}

static kb_fs_filemap_folio_record_t *filemap_folio_record(void *folio)
{
    if (folio == NULL) {
        return NULL;
    }
    FS_HOTPATH_BEGIN(profile_start);
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (filemap_folio_cache[i].active && filemap_folio_cache[i].folio == folio) {
            kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
            FS_HOTPATH_END(folio_lookup, profile_start);
            return record;
        }
    }
    FS_HOTPATH_END(folio_lookup, profile_start);
    return NULL;
}

static void *filemap_get_folio_internal(
    void *mapping,
    unsigned long index,
    unsigned int fgp_flags,
    unsigned int gfp,
    int preserve_unconsumed_readahead)
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
        uint32_t *refcount =
            (uint32_t *)((uint8_t *)folio + KB_FS_FOLIO_REFCOUNT_OFFSET);
        uint32_t observed = __atomic_load_n(refcount, __ATOMIC_ACQUIRE);
        for (;;) {
            if (observed == 0 || observed == UINT32_MAX) {
                return (void *)(intptr_t)-75;
            }
            if (__atomic_compare_exchange_n(
                    refcount,
                    &observed,
                    observed + 1u,
                    0,
                    __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE))
            {
                break;
            }
        }
        if ((fgp_flags & KB_FS_FGP_LOCK) != 0) {
            kb_fs_subsystem_folio_lock(folio);
        }
        return folio;
    }

    if ((fgp_flags & KB_FS_FGP_CREAT) == 0) {
        return (void *)(intptr_t)-2;
    }

    size_t cache_slot = KB_FS_FILEMAP_FOLIO_CACHE_MAX;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (!filemap_folio_cache[i].active) {
            cache_slot = i;
            break;
        }
    }
    if (cache_slot == KB_FS_FILEMAP_FOLIO_CACHE_MAX) {
        /* Give registered filesystem metadata caches their native pressure
         * notification before reclaiming a page-cache slot. */
        (void)kb_shrinker_reclaim(32, gfp);
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
            void *cached = filemap_folio_cache[i].folio;
            uint32_t refcount = UINT32_MAX;
            if (cached != NULL) {
                memcpy(&refcount,
                    (const uint8_t *)cached + KB_FS_FOLIO_REFCOUNT_OFFSET,
                    sizeof(refcount));
            }
            uint64_t flags = 0;
            if (cached != NULL) {
                memcpy(&flags, cached, sizeof(flags));
            }
            if (refcount > 1u ||
                (flags & (KB_FS_FOLIO_FLAG_DIRTY | KB_FS_FOLIO_FLAG_WRITEBACK)) != 0)
            {
                continue;
            }
            /* Keep this stream's unconsumed readahead resident; evicting it
             * here turns the next sequential access into the same RA again. */
            if (preserve_unconsumed_readahead &&
                filemap_folio_cache[i].mapping == mapping &&
                filemap_folio_cache[i].index >= index)
            {
                continue;
            }
            filemap_folio_discard(cached);
            cache_slot = i;
            break;
        }
    }
    if (cache_slot == KB_FS_FILEMAP_FOLIO_CACHE_MAX) {
        /* A bounded Kobox page cache must reclaim through the filesystem's
         * native writepages path.  Dropping dirty folios here would bypass
         * ext4 ordering and merely enlarging the array only postpones the
         * same failure under package-sized workloads. */
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
            kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
            if (!record->active || record->folio == NULL || record->mapping == NULL) {
                continue;
            }
            uint32_t refcount = 0;
            uint64_t flags = 0;
            memcpy(&refcount,
                (const uint8_t *)record->folio + KB_FS_FOLIO_REFCOUNT_OFFSET,
                sizeof(refcount));
            memcpy(&flags, record->folio, sizeof(flags));
            if (refcount > 1u || (flags & KB_FS_FOLIO_FLAG_WRITEBACK) != 0) {
                continue;
            }
            if (preserve_unconsumed_readahead &&
                record->mapping == mapping && record->index >= index)
            {
                continue;
            }
            if ((flags & KB_FS_FOLIO_FLAG_DIRTY) != 0 &&
                filemap_writeback_range(record->mapping, 0, INT64_MAX) != 0)
            {
                continue;
            }
            memcpy(&flags, record->folio, sizeof(flags));
            if ((flags & (KB_FS_FOLIO_FLAG_DIRTY | KB_FS_FOLIO_FLAG_WRITEBACK)) != 0) {
                continue;
            }
            filemap_folio_discard(record->folio);
            cache_slot = i;
            break;
        }
    }
    if (cache_slot == KB_FS_FILEMAP_FOLIO_CACHE_MAX) {
        size_t active = 0;
        size_t pinned = 0;
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
            if (!filemap_folio_cache[i].active) {
                continue;
            }
            active++;
            uint32_t refcount = 0;
            if (filemap_folio_cache[i].folio != NULL) {
                memcpy(&refcount,
                    (const uint8_t *)filemap_folio_cache[i].folio + KB_FS_FOLIO_REFCOUNT_OFFSET,
                    sizeof(refcount));
            }
            if (refcount > 1u) {
                pinned++;
            }
        }
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=filemap_folio_capacity status=-12 "
            "active=%zu pinned=%zu mapping=%p index=%lu\n",
            active,
            pinned,
            mapping,
            index);
        return (void *)(uintptr_t)-12;
    }

    void *folio = kb_kvm_alloc_pages_stub(gfp, 0);
    if (folio == NULL) {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=filemap_folio_page_alloc status=-12 "
            "mapping=%p index=%lu\n",
            mapping,
            index);
        return (void *)(uintptr_t)-12;
    }
    uint64_t flags = 0;
    if ((fgp_flags & KB_FS_FGP_LOCK) != 0) {
        flags |= KB_FS_FOLIO_FLAG_LOCKED;
    }
    write_u64_field(folio, 0, flags);
    write_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET, mapping);
    write_u64_field(folio, KB_FS_FOLIO_INDEX_OFFSET, index);
    /* One reference belongs to the page cache and one to this caller. */
    write_u32_field(folio, KB_FS_FOLIO_REFCOUNT_OFFSET, 2);
    filemap_folio_cache[cache_slot].active = 1;
    filemap_folio_cache[cache_slot].mapping = mapping;
    filemap_folio_cache[cache_slot].index = index;
    filemap_folio_cache[cache_slot].folio = folio;
    filemap_refresh_xarray_head(mapping);
    uint64_t nrpages = 0;
    memcpy(&nrpages,
        (const uint8_t *)mapping + KB_FS_ADDRESS_SPACE_NRPAGES_OFFSET,
        sizeof(nrpages));
    if (nrpages != UINT64_MAX) {
        write_u64_field(mapping, KB_FS_ADDRESS_SPACE_NRPAGES_OFFSET, nrpages + 1u);
    }
    return folio;
}

void *kb_fs_subsystem_filemap_get_folio(
    void *mapping,
    unsigned long index,
    unsigned int fgp_flags,
    unsigned int gfp)
{
    return filemap_get_folio_internal(mapping, index, fgp_flags, gfp, 0);
}

void kb_fs_subsystem_folio_end_read(void *folio, int success)
{
    if (folio == NULL) {
        return;
    }
    uint64_t observed = __atomic_load_n((uint64_t *)folio, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t replacement = observed & (uint64_t)~KB_FS_FOLIO_FLAG_LOCKED;
        if (success) {
            replacement |= KB_FS_FOLIO_FLAG_UPTODATE;
        } else {
            replacement &= (uint64_t)~KB_FS_FOLIO_FLAG_UPTODATE;
        }
        if (__atomic_compare_exchange_n(
                (uint64_t *)folio,
                &observed,
                replacement,
                0,
                __ATOMIC_RELEASE,
                __ATOMIC_ACQUIRE))
        {
            break;
        }
    }
    kb_wake_up_bit(folio, 0);
}

void *kb_fs_subsystem_read_cache_folio(
    void *mapping,
    unsigned long index,
    int (*filler)(void *, void *),
    void *file)
{
    if (mapping == NULL) {
        return (void *)(uintptr_t)-22;
    }

    void *folio = kb_fs_subsystem_filemap_get_folio(
        mapping,
        index,
        KB_FS_FGP_CREAT,
        0);
    if (low_or_err_pointer(folio)) {
        return folio;
    }

    uint64_t flags = 0;
    memcpy(&flags, folio, sizeof(flags));
    if ((flags & KB_FS_FOLIO_FLAG_UPTODATE) != 0) {
        return folio;
    }

    if (filler == NULL) {
        void *a_ops = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
        if (a_ops != NULL) {
            memcpy(&filler,
                (const uint8_t *)a_ops + KB_FS_ADDRESS_SPACE_OP_READ_FOLIO_OFFSET,
                sizeof(filler));
        }
    }
    if (filler == NULL) {
        kb_fs_subsystem_folio_put(folio);
        return (void *)(uintptr_t)-95;
    }

    flags |= KB_FS_FOLIO_FLAG_LOCKED;
    write_u64_field(folio, 0, flags);
    unsigned long old_gs = 0;
    int has_gs = kb_fs_enter_ext4_call((void *)filler, &old_gs);
    FS_READ_PROFILE_EXTENT_BEGIN(profile_start);
    int status = filler(file, folio);
    FS_READ_PROFILE_EXTENT_END(profile_start);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (status < 0) {
        kb_fs_subsystem_folio_unlock(folio);
        kb_fs_subsystem_folio_put(folio);
        return (void *)(intptr_t)status;
    }
    return folio;
}

static void file_ra_state_init(void *ra)
{
    if (ra == NULL) {
        return;
    }
    /* Linux initialises f_ra while opening the file.  Kobox has no backing
     * device object carrying ra_pages, so use the native NVMe/BIO batching
     * ceiling as the VFS policy limit. */
    memset(ra, 0, 32u);
    write_u32_field(
        ra,
        KB_FS_FILE_RA_PAGES_OFFSET,
        KB_FS_BIO_READ_BATCH_MAX);
    write_u64_field(
        ra,
        KB_FS_FILE_RA_PREV_POS_OFFSET,
        UINT64_MAX);
}

static void *page_cache_readahead_next(void *readahead_control)
{
    if (readahead_control == NULL) {
        return NULL;
    }
    unsigned int nr_pages = read_u32_field(
        readahead_control,
        KB_FS_READAHEAD_NR_PAGES_OFFSET);
    unsigned int batch_count = read_u32_field(
        readahead_control,
        KB_FS_READAHEAD_BATCH_COUNT_OFFSET);
    unsigned long index = (unsigned long)read_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET);
    if (batch_count > nr_pages || index > ULONG_MAX - batch_count) {
        write_u32_field(
            readahead_control,
            KB_FS_READAHEAD_NR_PAGES_OFFSET,
            0);
        write_u32_field(
            readahead_control,
            KB_FS_READAHEAD_BATCH_COUNT_OFFSET,
            0);
        return NULL;
    }
    nr_pages -= batch_count;
    index += batch_count;
    write_u32_field(
        readahead_control,
        KB_FS_READAHEAD_NR_PAGES_OFFSET,
        nr_pages);
    write_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET,
        index);
    if (nr_pages == 0) {
        write_u32_field(
            readahead_control,
            KB_FS_READAHEAD_BATCH_COUNT_OFFSET,
            0);
        return NULL;
    }

    void *mapping = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_MAPPING_OFFSET);
    void *folio = mapping == NULL ? NULL :
        kb_fs_subsystem_xa_load((uint8_t *)mapping + 8u, index);
    if (folio == NULL) {
        write_u32_field(
            readahead_control,
            KB_FS_READAHEAD_NR_PAGES_OFFSET,
            0);
        write_u32_field(
            readahead_control,
            KB_FS_READAHEAD_BATCH_COUNT_OFFSET,
            0);
        return NULL;
    }
    write_u32_field(
        readahead_control,
        KB_FS_READAHEAD_BATCH_COUNT_OFFSET,
        1);
    /* This is the out-of-line equivalent of Linux readahead_folio(): the
     * allocation reference is transferred to the filesystem before return. */
    kb_fs_subsystem_folio_put(folio);
    return folio;
}

static void page_cache_read_pages(void *readahead_control)
{
    if (readahead_control == NULL ||
        read_u32_field(
            readahead_control,
            KB_FS_READAHEAD_NR_PAGES_OFFSET) == 0)
    {
        return;
    }
    void *mapping = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_MAPPING_OFFSET);
    void *a_ops = mapping == NULL ? NULL :
        read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    void *readahead_operation = a_ops == NULL ? NULL :
        read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_READAHEAD_OFFSET);
    void *read_folio_operation = a_ops == NULL ? NULL :
        read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_READ_FOLIO_OFFSET);
    uint8_t plug[64] = {0};
    kb_fs_subsystem_blk_start_plug(plug);
    if (readahead_operation != NULL) {
        void (*readahead_fn)(void *) = NULL;
        memcpy(&readahead_fn, &readahead_operation, sizeof(readahead_fn));
        KB_FS_TRACE_INCREMENT(readahead_aops_calls);
        unsigned long old_gs = 0;
        const int has_gs =
            kb_fs_enter_ext4_call(readahead_operation, &old_gs);
        FS_READ_PROFILE_EXTENT_BEGIN(profile_start);
        readahead_fn(readahead_control);
        FS_READ_PROFILE_EXTENT_END(profile_start);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    } else if (read_folio_operation != NULL) {
        int (*read_folio_fn)(void *, void *) = NULL;
        memcpy(&read_folio_fn, &read_folio_operation, sizeof(read_folio_fn));
        KB_FS_TRACE_INCREMENT(readahead_fallback_calls);
        void *folio = NULL;
        while ((folio = page_cache_readahead_next(readahead_control)) != NULL) {
            unsigned long old_gs = 0;
            const int has_gs =
                kb_fs_enter_ext4_call(read_folio_operation, &old_gs);
            FS_READ_PROFILE_EXTENT_BEGIN(profile_start);
            const int status = read_folio_fn(
                read_pointer_field(
                    readahead_control,
                    KB_FS_READAHEAD_FILE_OFFSET),
                folio);
            FS_READ_PROFILE_EXTENT_END(profile_start);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (status < 0) {
                kb_fs_subsystem_folio_unlock(folio);
            }
        }
    }

    /* A filesystem may decline the tail of a readahead request.  Consume and
     * unlock only those unclaimed folios, just as Linux read_pages() does. */
    void *leftover = NULL;
    while ((leftover = page_cache_readahead_next(readahead_control)) != NULL) {
        kb_fs_subsystem_folio_unlock(leftover);
    }
    kb_fs_subsystem_blk_finish_plug(plug);
}

void kb_fs_subsystem_page_cache_ra_unbounded(
    void *readahead_control,
    unsigned long nr_to_read,
    unsigned long lookahead_size)
{
    if (readahead_control == NULL || nr_to_read == 0) {
        return;
    }
    void *mapping = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_MAPPING_OFFSET);
    if (mapping == NULL ||
        read_u32_field(
            readahead_control,
            KB_FS_READAHEAD_NR_PAGES_OFFSET) != 0 ||
        read_u32_field(
            readahead_control,
            KB_FS_READAHEAD_BATCH_COUNT_OFFSET) != 0)
    {
        return;
    }
    unsigned long index = (unsigned long)read_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET);
    if (nr_to_read - 1u > ULONG_MAX - index) {
        nr_to_read = ULONG_MAX - index + 1u;
    }
    if (lookahead_size > nr_to_read) {
        lookahead_size = nr_to_read;
    }
    const unsigned long mark = lookahead_size == 0 ? ULONG_MAX :
        index + nr_to_read - lookahead_size;
    const unsigned long end = index + nr_to_read;
    KB_FS_TRACE_INCREMENT(readahead_requests);

    while (index != end) {
        void *present = kb_fs_subsystem_xa_load(
            (uint8_t *)mapping + 8u,
            index);
        if (present != NULL) {
            page_cache_read_pages(readahead_control);
            if (index == ULONG_MAX) {
                break;
            }
            index++;
            write_u64_field(
                readahead_control,
                KB_FS_READAHEAD_INDEX_OFFSET,
                index);
            continue;
        }

        void *folio = filemap_get_folio_internal(
            mapping,
            index,
            KB_FS_FGP_CREAT | KB_FS_FGP_LOCK,
            0,
            1);
        if (low_or_err_pointer(folio)) {
            break;
        }
        if (index == mark) {
            write_u64_field(
                folio,
                0,
                read_u64_field(folio, 0) |
                    KB_FS_FOLIO_FLAG_READAHEAD);
        }
        unsigned int nr_pages = read_u32_field(
            readahead_control,
            KB_FS_READAHEAD_NR_PAGES_OFFSET);
        if (nr_pages == UINT_MAX) {
            kb_fs_subsystem_folio_unlock(folio);
            kb_fs_subsystem_folio_put(folio);
            break;
        }
        write_u32_field(
            readahead_control,
            KB_FS_READAHEAD_NR_PAGES_OFFSET,
            nr_pages + 1u);
        KB_FS_TRACE_INCREMENT(readahead_folios);
        if (index == ULONG_MAX) {
            break;
        }
        index++;
        if (nr_pages + 1u >= KB_FS_BIO_READ_BATCH_MAX) {
            page_cache_read_pages(readahead_control);
            write_u64_field(
                readahead_control,
                KB_FS_READAHEAD_INDEX_OFFSET,
                index);
        }
    }
    page_cache_read_pages(readahead_control);
}

static unsigned long page_cache_initial_ra_size(
    unsigned long requested,
    unsigned long maximum)
{
    if (requested == 0 || maximum == 0) {
        return 0;
    }
    unsigned long rounded = 1;
    while (rounded < requested && rounded <= ULONG_MAX / 2u) {
        rounded <<= 1u;
    }
    if (rounded > maximum) {
        return maximum;
    }
    if (rounded <= maximum / 32u && rounded <= maximum / 4u) {
        return rounded * 4u;
    }
    if (rounded <= maximum / 4u && rounded <= maximum / 2u) {
        return rounded * 2u;
    }
    return maximum;
}

static unsigned long page_cache_previous_miss(
    void *mapping,
    unsigned long index,
    unsigned long maximum)
{
    for (unsigned long scanned = 0; scanned < maximum; ++scanned) {
        if (kb_fs_subsystem_xa_load(
                (uint8_t *)mapping + 8u,
                index) == NULL)
        {
            return index;
        }
        if (index == 0) {
            return ULONG_MAX;
        }
        index--;
    }
    return ULONG_MAX;
}

static void page_cache_do_ra(
    void *readahead_control,
    unsigned long nr_to_read,
    unsigned long lookahead_size)
{
    void *mapping = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_MAPPING_OFFSET);
    void *inode = mapping == NULL ? NULL :
        read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (inode == NULL || nr_to_read == 0) {
        return;
    }
    const int64_t file_size = (int64_t)read_u64_field(
        inode,
        KB_FS_INODE_SIZE_OFFSET);
    if (file_size <= 0) {
        return;
    }
    const unsigned long index = (unsigned long)read_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET);
    const unsigned long last_index =
        (unsigned long)(((uint64_t)file_size - 1u) / KB_FS_PAGE_SIZE);
    if (index > last_index) {
        return;
    }
    const unsigned long available = last_index - index + 1u;
    if (nr_to_read > available) {
        nr_to_read = available;
    }
    if (lookahead_size > nr_to_read) {
        lookahead_size = nr_to_read;
    }
    kb_fs_subsystem_page_cache_ra_unbounded(
        readahead_control,
        nr_to_read,
        lookahead_size);
}

void kb_fs_subsystem_page_cache_sync_ra(
    void *readahead_control,
    unsigned long requested_count)
{
    if (readahead_control == NULL || requested_count == 0) {
        return;
    }
    void *mapping = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_MAPPING_OFFSET);
    void *file = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_FILE_OFFSET);
    void *ra = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_RA_OFFSET);
    if (mapping == NULL) {
        return;
    }
    if (ra == NULL) {
        page_cache_do_ra(readahead_control, requested_count, 0);
        return;
    }

    const unsigned long index = (unsigned long)read_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET);
    unsigned long maximum = read_u32_field(
        ra,
        KB_FS_FILE_RA_PAGES_OFFSET);
    int forced = file != NULL &&
        (read_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET) &
            (1u << 12)) != 0;
    if (maximum == 0) {
        if (file == NULL) {
            return;
        }
        requested_count = 1;
        forced = 1;
    }
    if (maximum > KB_FS_BIO_READ_BATCH_MAX) {
        maximum = KB_FS_BIO_READ_BATCH_MAX;
    }
    if (maximum < requested_count) {
        maximum = requested_count > KB_FS_BIO_READ_BATCH_MAX ?
            KB_FS_BIO_READ_BATCH_MAX : requested_count;
    }
    if (forced) {
        page_cache_do_ra(readahead_control, requested_count, 0);
        return;
    }

    const unsigned long previous_index = (unsigned long)(
        read_u64_field(ra, KB_FS_FILE_RA_PREV_POS_OFFSET) /
            KB_FS_PAGE_SIZE);
    unsigned long size = 0;
    unsigned long async_size = 0;
    if (index == 0 || requested_count > maximum ||
        index - previous_index <= 1u)
    {
        size = page_cache_initial_ra_size(requested_count, maximum);
        async_size = size > requested_count ?
            size - requested_count : size / 2u;
    } else {
        const unsigned long miss = page_cache_previous_miss(
            mapping,
            index - 1u,
            maximum);
        unsigned long contiguous = index - miss - 1u;
        if (contiguous <= requested_count) {
            page_cache_do_ra(readahead_control, requested_count, 0);
            return;
        }
        if (miss == ULONG_MAX && contiguous <= ULONG_MAX / 2u) {
            contiguous *= 2u;
        }
        size = contiguous > maximum - requested_count ?
            maximum : contiguous + requested_count;
        async_size = 1;
    }
    write_u64_field(ra, KB_FS_FILE_RA_START_OFFSET, index);
    write_u32_field(ra, KB_FS_FILE_RA_SIZE_OFFSET, (uint32_t)size);
    write_u32_field(
        ra,
        KB_FS_FILE_RA_ASYNC_SIZE_OFFSET,
        (uint32_t)async_size);
    write_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET,
        index);
    page_cache_do_ra(readahead_control, size, async_size);
}

static void filemap_folio_release_buffers(void *folio)
{
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        return;
    }

    write_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET, NULL);
    void *buffer_head = head;
    for (size_t i = 0; i < KB_FS_PAGE_SIZE; i++) {
        void *next = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        free(buffer_head);
        if (next == NULL || next == head) {
            return;
        }
        buffer_head = next;
    }
}

static void filemap_folio_discard(void *folio)
{
    if (folio == NULL) {
        return;
    }
    void *mapping = NULL;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (!filemap_folio_cache[i].active || filemap_folio_cache[i].folio != folio) {
            continue;
        }
        mapping = filemap_folio_cache[i].mapping;
        memset(&filemap_folio_cache[i], 0, sizeof(filemap_folio_cache[i]));
        filemap_refresh_xarray_head(mapping);
        break;
    }
    if (mapping != NULL) {
        uint64_t nrpages = 0;
        memcpy(&nrpages,
            (const uint8_t *)mapping + KB_FS_ADDRESS_SPACE_NRPAGES_OFFSET,
            sizeof(nrpages));
        if (nrpages != 0) {
            write_u64_field(mapping, KB_FS_ADDRESS_SPACE_NRPAGES_OFFSET, nrpages - 1u);
        }
        filemap_mapping_refresh_marks(mapping);
    }
    write_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET, NULL);
    uint32_t *refcount =
        (uint32_t *)((uint8_t *)folio + KB_FS_FOLIO_REFCOUNT_OFFSET);
    uint32_t observed = __atomic_load_n(refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == 0) {
            return;
        }
        if (__atomic_compare_exchange_n(
                refcount,
                &observed,
                observed - 1u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            if (observed > 1u) {
                return;
            }
            break;
        }
    }
    filemap_folio_release_buffers(folio);
    kb_kvm_free_pages_stub(folio, 0);
}

void kb_fs_subsystem_truncate_inode_pages_range(
    void *mapping,
    int64_t start,
    int64_t end)
{
    if (mapping == NULL || start < 0 || (end >= 0 && end < start)) {
        return;
    }
    const uint64_t range_start = (uint64_t)start;
    const uint64_t range_end = end < 0 ? UINT64_MAX : (uint64_t)end;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (!record->active || record->mapping != mapping || record->folio == NULL)
        {
            continue;
        }
        uint64_t folio_start = 0;
        if (__builtin_mul_overflow(
                (uint64_t)record->index,
                (uint64_t)KB_FS_PAGE_SIZE,
                &folio_start))
        {
            continue;
        }
        const uint64_t folio_end = folio_start <= UINT64_MAX - (KB_FS_PAGE_SIZE - 1u) ?
            folio_start + (KB_FS_PAGE_SIZE - 1u) : UINT64_MAX;
        if (folio_end < range_start || folio_start > range_end) {
            continue;
        }
        if (range_start <= folio_start && range_end >= folio_end) {
            filemap_folio_discard(record->folio);
            continue;
        }
        void *payload = folio_page_payload(record->folio);
        if (payload == NULL) {
            continue;
        }
        const uint64_t zero_start = range_start > folio_start ?
            range_start - folio_start : 0;
        const uint64_t zero_end = range_end < folio_end ?
            range_end - folio_start : KB_FS_PAGE_SIZE - 1u;
        if (zero_start < KB_FS_PAGE_SIZE && zero_end >= zero_start) {
            memset(
                (uint8_t *)payload + zero_start,
                0,
                (size_t)(zero_end - zero_start + 1u));
        }
    }
    filemap_mapping_refresh_marks(mapping);
}

void kb_fs_subsystem_truncate_inode_pages(void *mapping, int64_t start)
{
    kb_fs_subsystem_truncate_inode_pages_range(mapping, start, -1);
}

void kb_fs_subsystem_truncate_inode_pages_final(void *mapping)
{
    kb_fs_subsystem_truncate_inode_pages_range(mapping, 0, -1);
    const size_t remaining = filemap_mapping_folio_count(mapping);
    if (remaining != 0) {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=truncate_inode_pages_final "
            "mapping=%p remaining=%zu\n",
            mapping,
            remaining);
    }
}

unsigned long kb_fs_subsystem_invalidate_mapping_pages(
    void *mapping,
    unsigned long start,
    unsigned long end)
{
    if (mapping == NULL || end < start) {
        return 0;
    }
    unsigned long invalidated = 0;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
        kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (!record->active || record->mapping != mapping ||
            record->folio == NULL || record->index < start || record->index > end)
        {
            continue;
        }
        uint64_t flags = 0;
        uint32_t refcount = 0;
        memcpy(&flags, record->folio, sizeof(flags));
        memcpy(
            &refcount,
            (const uint8_t *)record->folio + KB_FS_FOLIO_REFCOUNT_OFFSET,
            sizeof(refcount));
        if (refcount > 1u ||
            (flags & (KB_FS_FOLIO_FLAG_LOCKED |
                      KB_FS_FOLIO_FLAG_DIRTY |
                      KB_FS_FOLIO_FLAG_WRITEBACK)) != 0)
        {
            continue;
        }
        filemap_folio_discard(record->folio);
        invalidated++;
    }
    filemap_mapping_refresh_marks(mapping);
    return invalidated;
}

void kb_fs_subsystem_invalidate_bdev(void *bdev)
{
    if (bdev == NULL) {
        return;
    }
    void *mapping = read_pointer_field(bdev, KB_FS_BDEV_MAPPING_OFFSET);
    if (mapping != NULL) {
        (void)kb_fs_subsystem_invalidate_mapping_pages(mapping, 0, ULONG_MAX);
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; ++i) {
        kb_fs_buffer_cache_record_t *record = &buffer_cache[i];
        if (!record->active || record->bdev != bdev || record->dirty ||
            record->buffer_head == NULL)
        {
            continue;
        }
        uint64_t state = 0;
        uint32_t refcount = 0;
        memcpy(&state, record->buffer_head, sizeof(state));
        memcpy(
            &refcount,
            (const uint8_t *)record->buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
            sizeof(refcount));
        if (refcount != 0 || (state & KB_FS_BH_LOCK) != 0) {
            continue;
        }
        buffer_cache_release_storage(record);
        memset(record, 0, sizeof(*record));
    }
}

void kb_fs_subsystem_truncate_pagecache(void *inode, int64_t new_size)
{
    if (inode == NULL || new_size < 0) {
        return;
    }
    kb_fs_subsystem_truncate_inode_pages(
        read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET),
        new_size);
}

void kb_fs_subsystem_truncate_pagecache_range(
    void *inode,
    int64_t start,
    int64_t end)
{
    if (inode == NULL) {
        return;
    }
    kb_fs_subsystem_truncate_inode_pages_range(
        read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET),
        start,
        end);
}

void kb_fs_subsystem_pagecache_isize_extended(
    void *inode,
    int64_t from,
    int64_t to)
{
    if (inode == NULL || from < 0 || to <= from ||
        (from / KB_FS_PAGE_SIZE) != ((to - 1) / KB_FS_PAGE_SIZE))
    {
        return;
    }
    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    kb_fs_filemap_folio_record_t *record = NULL;
    const unsigned long index =
        (unsigned long)((uint64_t)from / KB_FS_PAGE_SIZE);
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
        if (filemap_folio_cache[i].active &&
            filemap_folio_cache[i].mapping == mapping &&
            filemap_folio_cache[i].index == index)
        {
            record = &filemap_folio_cache[i];
            break;
        }
    }
    void *payload = record == NULL ? NULL : folio_page_payload(record->folio);
    if (payload != NULL) {
        memset(
            (uint8_t *)payload + ((uint64_t)from % KB_FS_PAGE_SIZE),
            0,
            (size_t)(to - from));
    }
}

void kb_fs_subsystem_block_commit_write(
    void *page,
    unsigned int from,
    unsigned int to)
{
    if (page == NULL || from > to || to > KB_FS_PAGE_SIZE) {
        return;
    }
    void *head = read_pointer_field(page, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        return;
    }
    int partial = 0;
    void *buffer_head = head;
    size_t block_start = 0;
    size_t visited = 0;
    do {
        uint64_t state = 0;
        uint64_t block_size = 0;
        memcpy(&state, buffer_head, sizeof(state));
        memcpy(&block_size,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
            sizeof(block_size));
        if (block_size == 0 || block_size > KB_FS_PAGE_SIZE - block_start) {
            return;
        }
        const size_t block_end = block_start + (size_t)block_size;
        if (block_end <= from || block_start >= to) {
            partial |= (state & KB_FS_BH_UPTODATE) == 0;
            state &= ~KB_FS_BH_NEW;
            write_u64_field(buffer_head, 0, state);
        } else {
            state = (state | KB_FS_BH_UPTODATE) & ~KB_FS_BH_NEW;
            write_u64_field(buffer_head, 0, state);
            kb_fs_subsystem_mark_buffer_dirty(buffer_head);
        }
        block_start = block_end;
        buffer_head = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        visited++;
    } while (buffer_head != NULL && buffer_head != head &&
        block_start < KB_FS_PAGE_SIZE && visited <= KB_FS_PAGE_SIZE);
    if (!partial) {
        uint64_t flags = 0;
        memcpy(&flags, page, sizeof(flags));
        write_u64_field(page, 0, flags | KB_FS_FOLIO_FLAG_UPTODATE);
    }
}

int kb_fs_subsystem_block_is_partially_uptodate(
    void *folio,
    size_t from,
    size_t count)
{
    if (folio == NULL || from >= KB_FS_PAGE_SIZE) {
        return 0;
    }
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        return 0;
    }
    uint64_t first_size = 0;
    memcpy(&first_size,
        (const uint8_t *)head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
        sizeof(first_size));
    if (first_size == 0 || first_size > KB_FS_PAGE_SIZE) {
        return 0;
    }
    const size_t bytes = count > KB_FS_PAGE_SIZE - from ?
        KB_FS_PAGE_SIZE - from : count;
    const size_t to = from + bytes;
    if (from < first_size && to > KB_FS_PAGE_SIZE - first_size) {
        return 0;
    }
    void *buffer_head = head;
    size_t block_start = 0;
    size_t visited = 0;
    do {
        uint64_t state = 0;
        uint64_t block_size = 0;
        memcpy(&state, buffer_head, sizeof(state));
        memcpy(&block_size,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
            sizeof(block_size));
        if (block_size == 0 || block_size > KB_FS_PAGE_SIZE - block_start) {
            return 0;
        }
        const size_t block_end = block_start + (size_t)block_size;
        if (block_end > from && block_start < to) {
            if ((state & KB_FS_BH_UPTODATE) == 0) {
                return 0;
            }
            if (block_end >= to) {
                return 1;
            }
        }
        block_start = block_end;
        buffer_head = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        visited++;
    } while (buffer_head != NULL && buffer_head != head &&
        block_start < KB_FS_PAGE_SIZE && visited <= KB_FS_PAGE_SIZE);
    return 1;
}

int kb_fs_subsystem_try_to_free_buffers(void *folio)
{
    if (folio == NULL) {
        return 0;
    }
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        return 1;
    }
    void *buffer_head = head;
    size_t visited = 0;
    do {
        uint64_t state = 0;
        uint32_t refcount = 0;
        memcpy(&state, buffer_head, sizeof(state));
        memcpy(&refcount,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
            sizeof(refcount));
        if ((state & (KB_FS_BH_DIRTY | KB_FS_BH_LOCK)) != 0 || refcount > 1u) {
            return 0;
        }
        buffer_head = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        visited++;
    } while (buffer_head != NULL && buffer_head != head && visited <= KB_FS_PAGE_SIZE);
    filemap_folio_release_buffers(folio);
    return 1;
}

int kb_fs_subsystem_filemap_release_folio(void *folio, unsigned int gfp)
{
    if (folio == NULL) {
        return 0;
    }
    uint64_t flags = 0;
    memcpy(&flags, folio, sizeof(flags));
    if ((flags & KB_FS_FOLIO_FLAG_WRITEBACK) != 0) {
        return 0;
    }
    if (read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET) == NULL) {
        return 1;
    }
    void *mapping = read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET);
    void *a_ops = mapping == NULL ? NULL :
        read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    void *release_operation = a_ops == NULL ? NULL :
        read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_RELEASE_FOLIO_OFFSET);
    if (release_operation == NULL) {
        return kb_fs_subsystem_try_to_free_buffers(folio);
    }
    int (*release_fn)(void *, unsigned int) = NULL;
    memcpy(&release_fn, &release_operation, sizeof(release_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(release_operation, &old_gs);
    const int released = release_fn(folio, gfp);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return released != 0;
}

int kb_fs_subsystem_folio_mkclean(void *folio)
{
    /* Kobox currently has no reverse-map entries for shared writable VMAs.
     * Therefore a page-cache folio cannot have PTE dirty bits to clean. */
    (void)folio;
    return 0;
}

int kb_fs_subsystem_block_page_mkwrite(
    void *vma,
    void *vm_fault,
    int (*get_block)(void *, uint64_t, void *, int))
{
    if (low_or_err_pointer(vma) || low_or_err_pointer(vm_fault) ||
        get_block == NULL)
    {
        return -22;
    }
    void *file = read_pointer_field(vma, KB_FS_VMA_FILE_OFFSET);
    void *inode = file == NULL ? NULL :
        read_pointer_field(file, KB_FS_FILE_INODE_OFFSET);
    void *mapping = inode == NULL ? NULL :
        read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    void *folio = read_pointer_field(vm_fault, KB_FS_VM_FAULT_PAGE_OFFSET);
    if (low_or_err_pointer(inode) || mapping == NULL ||
        low_or_err_pointer(folio))
    {
        return -22;
    }

    kb_fs_subsystem_folio_lock(folio);
    const uint64_t index = read_u64_field(folio, KB_FS_FOLIO_INDEX_OFFSET);
    int64_t size = 0;
    memcpy(
        &size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(size));
    uint64_t folio_position = 0;
    if (size < 0 ||
        __builtin_mul_overflow(index, (uint64_t)KB_FS_PAGE_SIZE, &folio_position) ||
        read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET) != mapping ||
        folio_position >= (uint64_t)size)
    {
        kb_fs_subsystem_folio_unlock(folio);
        return -14;
    }
    size_t end = KB_FS_PAGE_SIZE;
    if ((uint64_t)size - folio_position < end) {
        end = (size_t)((uint64_t)size - folio_position);
    }

    const uint8_t block_bits = read_u8_field(
        inode,
        KB_FS_INODE_BLKBITS_OFFSET);
    if (block_bits < 9 || block_bits > 12) {
        kb_fs_subsystem_folio_unlock(folio);
        return -5;
    }
    const uint64_t block_size = UINT64_C(1) << block_bits;
    if (read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET) == NULL) {
        filemap_folio_attach_buffers_with_state(
            mapping,
            folio,
            block_size,
            0);
    }
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        kb_fs_subsystem_folio_unlock(folio);
        return -12;
    }

    uint64_t logical_block = folio_position >> block_bits;
    size_t block_start = 0;
    void *buffer_head = head;
    int status = 0;
    do {
        const size_t block_end = block_start + (size_t)block_size;
        uint64_t state = read_u64_field(buffer_head, 0);
        if (block_start < end) {
            if ((state & KB_FS_BH_NEW) != 0) {
                state &= ~KB_FS_BH_NEW;
                write_u64_field(buffer_head, 0, state);
            }
            if ((state & KB_FS_BH_MAPPED) == 0) {
                unsigned long old_gs = 0;
                const int has_gs = kb_fs_enter_ext4_call(
                    (void *)(uintptr_t)get_block,
                    &old_gs);
                status = get_block(inode, logical_block, buffer_head, 1);
                if (has_gs) {
                    kb_shim_leave_kernel_gs(old_gs);
                }
                if (status != 0) {
                    break;
                }
                state = read_u64_field(buffer_head, 0);
                if ((state & KB_FS_BH_NEW) != 0) {
                    void *payload = folio_page_payload(folio);
                    if (payload == NULL) {
                        status = -12;
                        break;
                    }
                    if (block_end > end) {
                        memset(
                            (uint8_t *)payload + end,
                            0,
                            block_end - end);
                    }
                }
            }
            const uint64_t folio_flags = read_u64_field(folio, 0);
            state = read_u64_field(buffer_head, 0);
            if ((folio_flags & KB_FS_FOLIO_FLAG_UPTODATE) != 0) {
                write_u64_field(
                    buffer_head,
                    0,
                    state | KB_FS_BH_UPTODATE);
            } else if ((state & (KB_FS_BH_UPTODATE | KB_FS_BH_DELAY |
                    KB_FS_BH_UNWRITTEN)) == 0 && block_end > end)
            {
                kb_fs_subsystem_lock_buffer(buffer_head);
                status = kb_fs_subsystem_bh_read(buffer_head, 0, 1);
                if (status != 0) {
                    break;
                }
            }
        } else if ((read_u64_field(folio, 0) &
                KB_FS_FOLIO_FLAG_UPTODATE) != 0)
        {
            write_u64_field(
                buffer_head,
                0,
                state | KB_FS_BH_UPTODATE);
        }
        logical_block++;
        block_start = block_end;
        buffer_head = read_pointer_field(
            buffer_head,
            KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
    } while (buffer_head != NULL && buffer_head != head &&
        block_start < KB_FS_PAGE_SIZE);

    if (status != 0) {
        kb_fs_subsystem_folio_zero_new_buffers(folio, 0, end);
        kb_fs_subsystem_folio_unlock(folio);
        return status;
    }
    kb_fs_subsystem_block_commit_write(folio, 0, (unsigned int)end);
    (void)kb_fs_subsystem_folio_mark_dirty(folio);
    kb_fs_subsystem_folio_wait_stable(folio);
    return 0;
}

unsigned int kb_fs_subsystem_filemap_fault(void *vm_fault)
{
    enum {
        KB_FS_VM_FAULT_OOM = 0x000001,
        KB_FS_VM_FAULT_SIGBUS = 0x000002,
        KB_FS_VM_FAULT_MAJOR = 0x000004,
        KB_FS_VM_FAULT_LOCKED = 0x000200,
    };
    if (low_or_err_pointer(vm_fault)) {
        return KB_FS_VM_FAULT_SIGBUS;
    }
    void *vma = read_pointer_field(vm_fault, 0);
    void *file = vma == NULL ? NULL :
        read_pointer_field(vma, KB_FS_VMA_FILE_OFFSET);
    void *mapping = file == NULL ? NULL :
        read_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET);
    void *inode = mapping == NULL ? NULL :
        read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    const uint64_t index = read_u64_field(
        vm_fault,
        KB_FS_VM_FAULT_PGOFF_OFFSET);
    int64_t size = 0;
    if (low_or_err_pointer(inode)) {
        return KB_FS_VM_FAULT_SIGBUS;
    }
    memcpy(
        &size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(size));
    const uint64_t maximum_index = size <= 0 ? 0 :
        ((uint64_t)size + KB_FS_PAGE_SIZE - 1u) / KB_FS_PAGE_SIZE;
    if (index >= maximum_index) {
        return KB_FS_VM_FAULT_SIGBUS;
    }

    unsigned int result = 0;
    void *folio = kb_fs_subsystem_filemap_get_folio(
        mapping,
        (unsigned long)index,
        0,
        read_u32_field(vm_fault, KB_FS_VM_FAULT_GFP_MASK_OFFSET));
    if (low_or_err_pointer(folio)) {
        result |= KB_FS_VM_FAULT_MAJOR;
        folio = kb_fs_subsystem_read_cache_folio(
            mapping,
            (unsigned long)index,
            NULL,
            file);
    } else if ((read_u64_field(folio, 0) &
            KB_FS_FOLIO_FLAG_UPTODATE) == 0)
    {
        kb_fs_subsystem_folio_put(folio);
        folio = kb_fs_subsystem_read_cache_folio(
            mapping,
            (unsigned long)index,
            NULL,
            file);
    }
    if (low_or_err_pointer(folio)) {
        return (intptr_t)folio == -12 ?
            KB_FS_VM_FAULT_OOM : KB_FS_VM_FAULT_SIGBUS;
    }
    kb_fs_subsystem_folio_lock(folio);
    memcpy(
        &size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(size));
    if (size <= 0 || index >=
            ((uint64_t)size + KB_FS_PAGE_SIZE - 1u) / KB_FS_PAGE_SIZE ||
        read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET) != mapping ||
        (read_u64_field(folio, 0) & KB_FS_FOLIO_FLAG_UPTODATE) == 0)
    {
        kb_fs_subsystem_folio_unlock(folio);
        kb_fs_subsystem_folio_put(folio);
        return KB_FS_VM_FAULT_SIGBUS;
    }
    write_pointer_field(
        vm_fault,
        KB_FS_VM_FAULT_PAGE_OFFSET,
        folio);
    return result | KB_FS_VM_FAULT_LOCKED;
}

unsigned int kb_fs_subsystem_filemap_map_pages(
    void *vm_fault,
    unsigned long start_index,
    unsigned long end_index)
{
    if (low_or_err_pointer(vm_fault) || start_index > end_index) {
        return 0;
    }
    /* Fault-around is only an optimisation.  CapabilityOS' MM asks the main
     * ->fault callback for each PTE and does not provide Linux page-table
     * ownership to Kobox, so mapping zero adjacent pages is the correct
     * result; unlike the old symbol stub, this never claims a page was mapped. */
    return 0;
}

void kb_fs_subsystem_block_invalidate_folio(
    void *folio,
    size_t offset,
    size_t length)
{
    if (folio == NULL || offset > KB_FS_PAGE_SIZE ||
        length > KB_FS_PAGE_SIZE - offset)
    {
        return;
    }
    const size_t stop = offset + length;
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        return;
    }
    void *buffer_head = head;
    size_t block_start = 0;
    size_t visited = 0;
    do {
        uint64_t block_size = 0;
        memcpy(&block_size,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
            sizeof(block_size));
        if (block_size == 0 || block_size > KB_FS_PAGE_SIZE - block_start) {
            return;
        }
        const size_t block_end = block_start + (size_t)block_size;
        if (block_end > stop) {
            break;
        }
        if (offset <= block_start) {
            kb_fs_subsystem_lock_buffer(buffer_head);
            uint64_t state = 0;
            memcpy(&state, buffer_head, sizeof(state));
            state &= ~(KB_FS_BH_DIRTY | KB_FS_BH_MAPPED | KB_FS_BH_NEW |
                KB_FS_BH_REQ | KB_FS_BH_DELAY | KB_FS_BH_UNWRITTEN);
            write_u64_field(buffer_head, 0, state);
            write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_BDEV_OFFSET, NULL);
            buffer_cache_set_dirty(buffer_cache_record_for_head(buffer_head), 0);
            kb_fs_subsystem_unlock_buffer(buffer_head);
        }
        block_start = block_end;
        buffer_head = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        visited++;
    } while (buffer_head != NULL && buffer_head != head &&
        block_start < KB_FS_PAGE_SIZE && visited <= KB_FS_PAGE_SIZE);
    if (length == KB_FS_PAGE_SIZE) {
        (void)kb_fs_subsystem_try_to_free_buffers(folio);
    }
}

void kb_fs_subsystem_invalidate_inode_folios(void *inode)
{
    if (inode == NULL) {
        return;
    }
    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return;
    }
    kb_fs_subsystem_truncate_inode_pages_final(mapping);
}

void kb_fs_subsystem_folio_put(void *folio)
{
    if (folio == NULL) {
        return;
    }

    uint32_t *refcount =
        (uint32_t *)((uint8_t *)folio + KB_FS_FOLIO_REFCOUNT_OFFSET);
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        if (!filemap_folio_cache[i].active || filemap_folio_cache[i].folio != folio) {
            continue;
        }
        /* The shim cache owns one non-releasable reference. Linux may drop
         * more than the caller reference while completing buffered I/O. */
        uint32_t observed = __atomic_load_n(refcount, __ATOMIC_ACQUIRE);
        while (observed > 1u &&
            !__atomic_compare_exchange_n(
                refcount,
                &observed,
                observed - 1u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
        }
        return;
    }
    uint32_t observed = __atomic_load_n(refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == 0) {
            return;
        }
        if (__atomic_compare_exchange_n(
                refcount,
                &observed,
                observed - 1u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            if (observed > 1u) {
                return;
            }
            break;
        }
    }
    filemap_folio_release_buffers(folio);
    kb_kvm_free_pages_stub(folio, 0);
}

void kb_fs_subsystem_folio_unlock(void *folio)
{
    if (folio == NULL) {
        return;
    }
    (void)__atomic_fetch_and(
        (uint64_t *)folio,
        (uint64_t)~KB_FS_FOLIO_FLAG_LOCKED,
        __ATOMIC_RELEASE);
    kb_wake_up_bit(folio, 0);
}

void kb_fs_subsystem_folio_lock(void *folio)
{
    if (folio == NULL) {
        return;
    }
    (void)kb_out_of_line_wait_on_bit_lock(
        folio,
        0,
        kb_bit_wait_action,
        0);
}

int kb_fs_subsystem_filemap_dirty_folio(void *mapping, void *folio)
{
    if (low_or_err_pointer(mapping) || low_or_err_pointer(folio)) {
        return 0;
    }
    const uint64_t old_flags = __atomic_fetch_or(
        (uint64_t *)folio,
        KB_FS_FOLIO_FLAG_DIRTY,
        __ATOMIC_ACQ_REL);
    const int newly_dirty = (old_flags & KB_FS_FOLIO_FLAG_DIRTY) == 0;
    filemap_mapping_set_mark(mapping, KB_FS_XARRAY_MARK_DIRTY, 1);
    if (newly_dirty) {
        void *inode = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
        kb_fs_subsystem_mark_inode_dirty(inode, 1 << 5);
    }
    return newly_dirty;
}

int kb_fs_subsystem_block_dirty_folio(void *mapping, void *folio)
{
    if (mapping == NULL || folio == NULL) {
        return 0;
    }
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    void *buffer_head = head;
    if (head != NULL) {
        for (size_t i = 0; i < KB_FS_PAGE_SIZE; i++) {
            uint64_t state = 0;
            memcpy(&state, buffer_head, sizeof(state));
            state |= KB_FS_BH_DIRTY;
            write_u64_field(buffer_head, 0, state);
            buffer_head = read_pointer_field(
                buffer_head,
                KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
            if (buffer_head == NULL || buffer_head == head) {
                break;
            }
        }
    }
    return kb_fs_subsystem_filemap_dirty_folio(mapping, folio);
}

int kb_fs_subsystem_folio_mark_dirty(void *folio)
{
    if (folio == NULL) {
        return 0;
    }
    void *mapping = read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET);
    if (mapping == NULL) {
        const uint64_t old_flags = __atomic_fetch_or(
            (uint64_t *)folio,
            KB_FS_FOLIO_FLAG_DIRTY,
            __ATOMIC_ACQ_REL);
        return (old_flags & KB_FS_FOLIO_FLAG_DIRTY) == 0;
    }
    void *a_ops = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    void *operation = a_ops == NULL ? NULL :
        read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_DIRTY_FOLIO_OFFSET);
    if (operation == NULL) {
        return kb_fs_subsystem_filemap_dirty_folio(mapping, folio);
    }
    int (*dirty_folio_fn)(void *, void *) = NULL;
    memcpy(&dirty_folio_fn, &operation, sizeof(dirty_folio_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    const int result = dirty_folio_fn(mapping, folio);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

int kb_fs_subsystem_folio_clear_dirty_for_io(void *folio)
{
    if (folio == NULL) {
        return 0;
    }
    const uint64_t old_flags = __atomic_fetch_and(
        (uint64_t *)folio,
        (uint64_t)~KB_FS_FOLIO_FLAG_DIRTY,
        __ATOMIC_ACQ_REL);
    const int was_dirty = (old_flags & KB_FS_FOLIO_FLAG_DIRTY) != 0;
    kb_fs_filemap_folio_record_t *record = filemap_folio_record(folio);
    if (record != NULL) {
        record->towrite = 0;
        filemap_mapping_refresh_marks(record->mapping);
    }
    return was_dirty;
}

int kb_fs_subsystem_folio_redirty_for_writepage(void *writeback_control, void *folio)
{
    (void)writeback_control;
    return kb_fs_subsystem_folio_mark_dirty(folio);
}

void kb_fs_subsystem_folio_start_writeback(void *folio, int keep_write)
{
    if (folio == NULL) {
        return;
    }
    (void)__atomic_fetch_or(
        (uint64_t *)folio,
        KB_FS_FOLIO_FLAG_WRITEBACK,
        __ATOMIC_ACQ_REL);
    kb_fs_filemap_folio_record_t *record = filemap_folio_record(folio);
    if (record != NULL) {
        if (!keep_write) {
            record->towrite = 0;
        }
        filemap_mapping_refresh_marks(record->mapping);
    }
}

void kb_fs_subsystem_folio_end_writeback(void *folio)
{
    if (folio == NULL) {
        return;
    }
    (void)__atomic_fetch_and(
        (uint64_t *)folio,
        (uint64_t)~KB_FS_FOLIO_FLAG_WRITEBACK,
        __ATOMIC_RELEASE);
    kb_fs_filemap_folio_record_t *record = filemap_folio_record(folio);
    if (record != NULL) {
        filemap_mapping_refresh_marks(record->mapping);
    }
    kb_wake_up_bit(folio, 8);
}

void kb_fs_subsystem_folio_wait_writeback(void *folio)
{
    if (folio == NULL) {
        return;
    }
    uint64_t flags = __atomic_load_n((uint64_t *)folio, __ATOMIC_ACQUIRE);
    while ((flags & KB_FS_FOLIO_FLAG_WRITEBACK) != 0) {
        (void)kb_fs_subsystem_bio_drain();
        kb_run_deferred_work();
        flags = __atomic_load_n((uint64_t *)folio, __ATOMIC_ACQUIRE);
    }
}

void kb_fs_subsystem_folio_wait_stable(void *folio)
{
    kb_fs_subsystem_folio_wait_writeback(folio);
}

void kb_fs_subsystem_folio_zero_new_buffers(void *folio, size_t from, size_t to)
{
    if (folio == NULL || from > to || to > KB_FS_PAGE_SIZE) {
        return;
    }
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    void *payload = folio_page_payload(folio);
    if (head == NULL || payload == NULL) {
        return;
    }
    uint64_t folio_flags = 0;
    memcpy(&folio_flags, folio, sizeof(folio_flags));
    void *buffer_head = head;
    size_t block_start = 0;
    size_t visited = 0;
    do {
        uint64_t state = 0;
        uint64_t block_size = 0;
        memcpy(&state, buffer_head, sizeof(state));
        memcpy(&block_size,
            (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_SIZE_OFFSET,
            sizeof(block_size));
        if (block_size == 0 || block_size > KB_FS_PAGE_SIZE - block_start) {
            return;
        }
        const size_t block_end = block_start + (size_t)block_size;
        if ((state & KB_FS_BH_NEW) != 0 && block_end > from && block_start < to) {
            if ((folio_flags & KB_FS_FOLIO_FLAG_UPTODATE) == 0) {
                const size_t zero_start = from > block_start ? from : block_start;
                const size_t zero_end = to < block_end ? to : block_end;
                memset((uint8_t *)payload + zero_start, 0, zero_end - zero_start);
                state |= KB_FS_BH_UPTODATE;
            }
            state &= ~KB_FS_BH_NEW;
            write_u64_field(buffer_head, 0, state);
            kb_fs_subsystem_mark_buffer_dirty(buffer_head);
        }
        block_start = block_end;
        buffer_head = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        visited++;
    } while (buffer_head != NULL && buffer_head != head &&
        block_start < KB_FS_PAGE_SIZE && visited <= KB_FS_PAGE_SIZE);
}

void kb_fs_subsystem_tag_pages_for_writeback(void *mapping, unsigned long start, unsigned long end)
{
    if (mapping == NULL) {
        return;
    }
    int any = 0;
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
        kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (!record->active || record->mapping != mapping ||
            record->index < start || record->index > end || record->folio == NULL)
        {
            continue;
        }
        uint64_t flags = 0;
        memcpy(&flags, record->folio, sizeof(flags));
        if ((flags & KB_FS_FOLIO_FLAG_DIRTY) != 0) {
            record->towrite = 1;
            any = 1;
        }
    }
    filemap_mapping_set_mark(mapping, KB_FS_XARRAY_MARK_TOWRITE, any);
}

unsigned int kb_fs_subsystem_filemap_get_folios(
    void *mapping,
    unsigned long *start,
    unsigned long end,
    void *folio_batch)
{
    if (mapping == NULL || start == NULL || folio_batch == NULL) {
        return 0;
    }
    uint8_t *batch = folio_batch;
    batch[0] = 0;
    batch[1] = 0;
    unsigned int count = 0;
    unsigned long cursor = *start;
    while (count < KB_FS_FOLIO_BATCH_MAX && cursor <= end) {
        kb_fs_filemap_folio_record_t *best = NULL;
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
            kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
            if (!record->active || record->mapping != mapping ||
                record->index < cursor || record->index > end || record->folio == NULL)
            {
                continue;
            }
            if (best == NULL || record->index < best->index) {
                best = record;
            }
        }
        if (best == NULL) {
            break;
        }
        uint32_t refcount = 0;
        memcpy(
            &refcount,
            (const uint8_t *)best->folio + KB_FS_FOLIO_REFCOUNT_OFFSET,
            sizeof(refcount));
        if (refcount != UINT32_MAX) {
            write_u32_field(best->folio, KB_FS_FOLIO_REFCOUNT_OFFSET, refcount + 1u);
        }
        write_pointer_field(batch, 8u + count * sizeof(void *), best->folio);
        batch[0] = (uint8_t)++count;
        if (best->index == ULONG_MAX) {
            cursor = ULONG_MAX;
            break;
        }
        cursor = best->index + 1u;
    }
    if (count == KB_FS_FOLIO_BATCH_MAX) {
        *start = cursor;
    } else {
        *start = end == ULONG_MAX ? ULONG_MAX : end + 1u;
    }
    return count;
}

unsigned int kb_fs_subsystem_filemap_get_folios_tag(
    void *mapping,
    unsigned long *start,
    unsigned long end,
    unsigned int tag,
    void *folio_batch)
{
    if (mapping == NULL || start == NULL || folio_batch == NULL) {
        return 0;
    }
    uint8_t *batch = folio_batch;
    batch[0] = 0;
    batch[1] = 0;
    unsigned int count = 0;
    unsigned long cursor = *start;
    while (count < KB_FS_FOLIO_BATCH_MAX && cursor <= end) {
        kb_fs_filemap_folio_record_t *best = NULL;
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; i++) {
            kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
            if (!record->active || record->mapping != mapping ||
                record->index < cursor || record->index > end || record->folio == NULL)
            {
                continue;
            }
            uint64_t flags = 0;
            memcpy(&flags, record->folio, sizeof(flags));
            int matches = 0;
            if (tag == KB_FS_PAGECACHE_TAG_DIRTY) {
                matches = (flags & KB_FS_FOLIO_FLAG_DIRTY) != 0;
            } else if (tag == KB_FS_PAGECACHE_TAG_WRITEBACK) {
                matches = (flags & KB_FS_FOLIO_FLAG_WRITEBACK) != 0;
            } else if (tag == KB_FS_PAGECACHE_TAG_TOWRITE) {
                matches = record->towrite && (flags & KB_FS_FOLIO_FLAG_DIRTY) != 0;
            }
            if (matches && (best == NULL || record->index < best->index)) {
                best = record;
            }
        }
        if (best == NULL) {
            break;
        }
        uint32_t refcount = 0;
        memcpy(&refcount,
            (const uint8_t *)best->folio + KB_FS_FOLIO_REFCOUNT_OFFSET,
            sizeof(refcount));
        if (refcount != UINT32_MAX) {
            write_u32_field(best->folio, KB_FS_FOLIO_REFCOUNT_OFFSET, refcount + 1u);
        }
        write_pointer_field(batch, 8u + count * sizeof(void *), best->folio);
        batch[0] = (uint8_t)++count;
        if (best->index == ULONG_MAX) {
            cursor = ULONG_MAX;
            break;
        }
        cursor = best->index + 1u;
    }
    if (count == KB_FS_FOLIO_BATCH_MAX) {
        *start = cursor;
    } else {
        *start = end == ULONG_MAX ? ULONG_MAX : end + 1u;
    }
    return count;
}

void kb_fs_subsystem_folio_batch_release(void *folio_batch)
{
    if (folio_batch == NULL) {
        return;
    }
    uint8_t *batch = folio_batch;
    const unsigned int count = batch[0] <= KB_FS_FOLIO_BATCH_MAX ?
        batch[0] : KB_FS_FOLIO_BATCH_MAX;
    for (unsigned int i = 0; i < count; i++) {
        void *folio = read_pointer_field(batch, 8u + i * sizeof(void *));
        kb_fs_subsystem_folio_put(folio);
        write_pointer_field(batch, 8u + i * sizeof(void *), NULL);
    }
    batch[0] = 0;
    batch[1] = 0;
}

int kb_fs_subsystem_write_cache_pages(
    void *mapping,
    void *writeback_control,
    int (*writepage)(void *, void *, void *),
    void *data)
{
    if (mapping == NULL || writeback_control == NULL || writepage == NULL) {
        return -22;
    }
    int64_t nr_to_write = 0;
    int64_t range_start = 0;
    int64_t range_end = 0;
    memcpy(&nr_to_write, writeback_control, sizeof(nr_to_write));
    memcpy(&range_start,
        (const uint8_t *)writeback_control + 0x10,
        sizeof(range_start));
    memcpy(&range_end,
        (const uint8_t *)writeback_control + 0x18,
        sizeof(range_end));
    if (nr_to_write <= 0 || range_start < 0 || range_end < range_start) {
        return 0;
    }
    const unsigned long first_index =
        (unsigned long)((uint64_t)range_start / KB_FS_PAGE_SIZE);
    const unsigned long last_index = range_end == INT64_MAX ? ULONG_MAX :
        (unsigned long)((uint64_t)range_end / KB_FS_PAGE_SIZE);
    unsigned long cursor = first_index;
    int first_error = 0;
    while (nr_to_write > 0 && cursor <= last_index) {
        kb_fs_filemap_folio_record_t *best = NULL;
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
            kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
            if (!record->active || record->mapping != mapping ||
                record->folio == NULL || record->index < cursor ||
                record->index > last_index)
            {
                continue;
            }
            uint64_t flags = 0;
            memcpy(&flags, record->folio, sizeof(flags));
            if ((flags & KB_FS_FOLIO_FLAG_DIRTY) == 0) {
                continue;
            }
            if (best == NULL || record->index < best->index) {
                best = record;
            }
        }
        if (best == NULL) {
            break;
        }
        const unsigned long index = best->index;
        uint64_t flags = 0;
        uint32_t refcount = 0;
        memcpy(&flags, best->folio, sizeof(flags));
        memcpy(&refcount,
            (const uint8_t *)best->folio + KB_FS_FOLIO_REFCOUNT_OFFSET,
            sizeof(refcount));
        if (refcount != UINT32_MAX) {
            write_u32_field(best->folio, KB_FS_FOLIO_REFCOUNT_OFFSET, refcount + 1u);
        }
        write_u64_field(best->folio, 0, flags | KB_FS_FOLIO_FLAG_LOCKED);
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call((void *)writepage, &old_gs);
        int status = writepage(best->folio, writeback_control, data);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status == KB_FS_AOP_WRITEPAGE_ACTIVATE) {
            kb_fs_subsystem_folio_unlock(best->folio);
            status = 0;
        }
        kb_fs_subsystem_folio_put(best->folio);
        if (status != 0) {
            first_error = status;
            break;
        }
        nr_to_write--;
        write_u64_field(writeback_control, 0, (uint64_t)nr_to_write);
        if (index == ULONG_MAX) {
            break;
        }
        cursor = index + 1u;
    }
    filemap_mapping_refresh_marks(mapping);
    return first_error;
}

int kb_fs_subsystem_block_read_full_folio(
    void *folio,
    int (*get_block)(void *, uint64_t, void *, int))
{
    if (folio == NULL || get_block == NULL) {
        return -22;
    }
    void *mapping = read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -22;
    }
    void *inode = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (inode == NULL) {
        return -22;
    }
    uint8_t block_bits = 0;
    uint64_t file_size = 0;
    uint64_t folio_index = 0;
    memcpy(&block_bits,
        (const uint8_t *)inode + KB_FS_INODE_BLKBITS_OFFSET,
        sizeof(block_bits));
    memcpy(&file_size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(file_size));
    memcpy(&folio_index,
        (const uint8_t *)folio + KB_FS_FOLIO_INDEX_OFFSET,
        sizeof(folio_index));
    if (block_bits >= 63u) {
        return -22;
    }
    const uint64_t block_size = UINT64_C(1) << block_bits;
    if (block_size == 0 || block_size > KB_FS_PAGE_SIZE ||
        (KB_FS_PAGE_SIZE % block_size) != 0)
    {
        return -22;
    }
    void *head = read_pointer_field(folio, KB_FS_FOLIO_PRIVATE_OFFSET);
    if (head == NULL) {
        head = kb_fs_subsystem_create_empty_buffers(
            folio,
            (unsigned long)block_size,
            0);
    }
    void *payload = folio_page_payload(folio);
    if (head == NULL || payload == NULL ||
        folio_index > UINT64_MAX / KB_FS_PAGE_SIZE)
    {
        return -12;
    }
    uint64_t logical_block = (folio_index * KB_FS_PAGE_SIZE) / block_size;
    const uint64_t logical_limit =
        file_size > UINT64_MAX - (block_size - 1u) ?
            UINT64_MAX / block_size :
            (file_size + block_size - 1u) / block_size;
    int page_error = 0;
    void *buffer_head = head;
    size_t block_start = 0;
    size_t visited = 0;
    do {
        uint64_t state = 0;
        memcpy(&state, buffer_head, sizeof(state));
        if ((state & KB_FS_BH_UPTODATE) == 0) {
            if ((state & KB_FS_BH_MAPPED) == 0 && logical_block < logical_limit) {
                unsigned long old_gs = 0;
                const int has_gs =
                    kb_fs_enter_ext4_call((void *)get_block, &old_gs);
                const int status =
                    get_block(inode, logical_block, buffer_head, 0);
                if (has_gs) {
                    kb_shim_leave_kernel_gs(old_gs);
                }
                if (status != 0) {
                    page_error = 1;
                }
                memcpy(&state, buffer_head, sizeof(state));
            }
            if ((state & KB_FS_BH_MAPPED) == 0) {
                memset(
                    (uint8_t *)payload + block_start,
                    0,
                    (size_t)block_size);
                write_u64_field(
                    buffer_head,
                    0,
                    state | KB_FS_BH_UPTODATE);
            } else if ((state & KB_FS_BH_UPTODATE) == 0) {
                kb_fs_subsystem_lock_buffer(buffer_head);
                if (kb_fs_subsystem_bh_read(buffer_head, 0, 1) != 0) {
                    page_error = 1;
                }
            }
        }
        block_start += (size_t)block_size;
        logical_block++;
        buffer_head = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        visited++;
    } while (buffer_head != NULL && buffer_head != head &&
        block_start < KB_FS_PAGE_SIZE && visited <= KB_FS_PAGE_SIZE);
    kb_fs_subsystem_folio_end_read(folio, !page_error);
    return 0;
}

static int filemap_writeback_range(void *mapping, int64_t start, int64_t end)
{
    if (mapping == NULL || start < 0 || end < start) {
        return -22;
    }
    uint32_t xa_flags = 0;
    memcpy(&xa_flags,
        (const uint8_t *)mapping + KB_FS_ADDRESS_SPACE_XARRAY_FLAGS_OFFSET,
        sizeof(xa_flags));
    if ((xa_flags & KB_FS_XARRAY_MARK_DIRTY) == 0) {
        return 0;
    }
    void *a_ops = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    void *operation = a_ops == NULL ? NULL :
        read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_WRITEPAGES_OFFSET);
    if (operation == NULL) {
        return -95;
    }
    uint8_t writeback_control[KB_FS_WRITEBACK_CONTROL_BYTES];
    memset(writeback_control, 0, sizeof(writeback_control));
    write_u64_field(writeback_control, 0, (uint64_t)LONG_MAX);
    write_u64_field(writeback_control, 0x10, (uint64_t)start);
    write_u64_field(writeback_control, 0x18, (uint64_t)end);
    write_u32_field(
        writeback_control,
        KB_FS_WRITEBACK_CONTROL_SYNC_MODE_OFFSET,
        KB_FS_WRITEBACK_CONTROL_WB_SYNC_ALL);
    int (*writepages_fn)(void *, void *) = NULL;
    memcpy(&writepages_fn, &operation, sizeof(writepages_fn));
    const int previous_auto_drain = bio_auto_drain;
    bio_auto_drain = 0;
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    int status = writepages_fn(mapping, writeback_control);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    (void)kb_fs_subsystem_bio_drain();
    const int drain_status = bio_last_drain_status;
    bio_auto_drain = previous_auto_drain;
    filemap_mapping_refresh_marks(mapping);
    return status != 0 ? status : drain_status;
}

int kb_fs_subsystem_filemap_write_and_wait_range(void *mapping, int64_t start, int64_t end)
{
    return filemap_writeback_range(mapping, start, end);
}

int kb_fs_subsystem_filemap_flush(void *mapping)
{
    return filemap_writeback_range(mapping, 0, INT64_MAX);
}

int kb_fs_subsystem_filemap_wait_range(
    void *mapping,
    int64_t start,
    int64_t end)
{
    (void)mapping;
    if (start < 0 || end < start) {
        return -22;
    }
    (void)kb_fs_subsystem_bio_drain();
    return 0;
}

int kb_fs_subsystem_file_write_and_wait_range(void *file, int64_t start, int64_t end)
{
    if (file == NULL) {
        return -22;
    }
    return filemap_writeback_range(
        read_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET),
        start,
        end);
}

int kb_fs_subsystem_file_update_time(void *file)
{
    if (file == NULL) {
        return -22;
    }
    void *inode = read_pointer_field(file, KB_FS_FILE_INODE_OFFSET);
    if (inode == NULL) {
        return -22;
    }
    const int64_t now = kb_ktime_get_real_seconds();
    const uint32_t nsec = 0;
    memcpy((uint8_t *)inode + KB_FS_INODE_MTIME_SEC_OFFSET, &now, sizeof(now));
    memcpy((uint8_t *)inode + KB_FS_INODE_CTIME_SEC_OFFSET, &now, sizeof(now));
    memcpy((uint8_t *)inode + KB_FS_INODE_MTIME_NSEC_OFFSET, &nsec, sizeof(nsec));
    memcpy((uint8_t *)inode + KB_FS_INODE_CTIME_NSEC_OFFSET, &nsec, sizeof(nsec));
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    uint64_t super_flags = 0;
    if (super_block != NULL) {
        memcpy(
            &super_flags,
            (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_FLAGS_OFFSET,
            sizeof(super_flags));
    }
    if ((super_flags & KB_FS_SB_I_VERSION) != 0) {
        (void)kb_fs_subsystem_inode_maybe_inc_iversion(inode, 1);
    }
    kb_fs_subsystem_mark_inode_dirty(inode, KB_FS_INODE_STATE_DIRTY_SYNC);
    return 0;
}

int kb_fs_subsystem_file_modified(void *file)
{
    return kb_fs_subsystem_file_update_time(file);
}

void kb_fs_subsystem_touch_atime(void *path)
{
    if (path == NULL) {
        return;
    }
    void *mount = read_pointer_field(path, KB_FS_PATH_MNT_OFFSET);
    void *dentry = read_pointer_field(path, KB_FS_PATH_DENTRY_OFFSET);
    void *inode = dentry == NULL ? NULL :
        read_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (mount == NULL || low_or_err_pointer(inode)) {
        return;
    }

    uint32_t inode_flags = 0;
    uint32_t mount_flags = 0;
    uint16_t mode = 0;
    uint64_t super_flags = 0;
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    memcpy(
        &inode_flags,
        (const uint8_t *)inode + KB_FS_INODE_FLAGS_OFFSET,
        sizeof(inode_flags));
    memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
    memcpy(
        &mount_flags,
        (const uint8_t *)mount + KB_FS_VFSMOUNT_FLAGS_OFFSET,
        sizeof(mount_flags));
    if (super_block != NULL) {
        memcpy(
            &super_flags,
            (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_FLAGS_OFFSET,
            sizeof(super_flags));
    }
    const int is_directory =
        (mode & KB_FS_MODE_TYPE_MASK) == 0040000u;
    if ((inode_flags & KB_FS_INODE_NOATIME) != 0 ||
        (mount_flags & (KB_FS_MNT_NOATIME | KB_FS_MNT_READONLY)) != 0 ||
        (super_flags & (KB_FS_SB_RDONLY | KB_FS_SB_NOATIME)) != 0 ||
        (is_directory &&
         ((mount_flags & KB_FS_MNT_NODIRATIME) != 0 ||
          (super_flags & KB_FS_SB_NODIRATIME) != 0)))
    {
        return;
    }

    int64_t atime_sec = 0;
    int64_t mtime_sec = 0;
    int64_t ctime_sec = 0;
    uint32_t atime_nsec = 0;
    uint32_t mtime_nsec = 0;
    uint32_t ctime_nsec = 0;
    memcpy(&atime_sec, (const uint8_t *)inode + KB_FS_INODE_ATIME_SEC_OFFSET,
        sizeof(atime_sec));
    memcpy(&mtime_sec, (const uint8_t *)inode + KB_FS_INODE_MTIME_SEC_OFFSET,
        sizeof(mtime_sec));
    memcpy(&ctime_sec, (const uint8_t *)inode + KB_FS_INODE_CTIME_SEC_OFFSET,
        sizeof(ctime_sec));
    memcpy(&atime_nsec, (const uint8_t *)inode + KB_FS_INODE_ATIME_NSEC_OFFSET,
        sizeof(atime_nsec));
    memcpy(&mtime_nsec, (const uint8_t *)inode + KB_FS_INODE_MTIME_NSEC_OFFSET,
        sizeof(mtime_nsec));
    memcpy(&ctime_nsec, (const uint8_t *)inode + KB_FS_INODE_CTIME_NSEC_OFFSET,
        sizeof(ctime_nsec));

    const int64_t now_sec = kb_ktime_get_real_seconds();
    const uint32_t now_nsec = 0;
    if ((mount_flags & KB_FS_MNT_RELATIME) != 0) {
        const int mtime_not_older = mtime_sec > atime_sec ||
            (mtime_sec == atime_sec && mtime_nsec >= atime_nsec);
        const int ctime_not_older = ctime_sec > atime_sec ||
            (ctime_sec == atime_sec && ctime_nsec >= atime_nsec);
        if (!mtime_not_older && !ctime_not_older &&
            now_sec - atime_sec < 24 * 60 * 60)
        {
            return;
        }
    }
    if (atime_sec == now_sec && atime_nsec == now_nsec) {
        return;
    }

    memcpy((uint8_t *)inode + KB_FS_INODE_ATIME_SEC_OFFSET,
        &now_sec, sizeof(now_sec));
    memcpy((uint8_t *)inode + KB_FS_INODE_ATIME_NSEC_OFFSET,
        &now_nsec, sizeof(now_nsec));
    kb_fs_subsystem_mark_inode_dirty(
        inode,
        (super_flags & KB_FS_SB_LAZYTIME) != 0 ?
            KB_FS_INODE_STATE_DIRTY_TIME :
            KB_FS_INODE_STATE_DIRTY_SYNC);
}

uint32_t kb_fs_subsystem_errseq_set(uint32_t *sequence, int error)
{
    enum {
        KB_FS_ERRSEQ_ERROR_MASK = 4095u,
        KB_FS_ERRSEQ_SEEN = 1u << 12,
        KB_FS_ERRSEQ_COUNTER_INCREMENT = 1u << 13,
    };
    if (sequence == NULL) {
        return 0;
    }
    uint32_t observed = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    if (error >= 0 || -(int64_t)error > KB_FS_ERRSEQ_ERROR_MASK) {
        return observed;
    }
    for (;;) {
        const uint32_t old = observed;
        uint32_t replacement =
            (old & ~(KB_FS_ERRSEQ_ERROR_MASK | KB_FS_ERRSEQ_SEEN)) |
            (uint32_t)(-error);
        if ((old & KB_FS_ERRSEQ_SEEN) != 0) {
            replacement += KB_FS_ERRSEQ_COUNTER_INCREMENT;
        }
        if (replacement == old) {
            return old;
        }
        if (__atomic_compare_exchange_n(
                sequence,
                &observed,
                replacement,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            return old;
        }
        if (observed == replacement) {
            return observed;
        }
    }
}

uint32_t kb_fs_subsystem_errseq_sample(uint32_t *sequence)
{
    if (sequence == NULL) {
        return 0;
    }
    const uint32_t observed = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    return (observed & (1u << 12)) != 0 ? observed : 0;
}

int kb_fs_subsystem_errseq_check(uint32_t *sequence, uint32_t since)
{
    if (sequence == NULL) {
        return 0;
    }
    const uint32_t observed = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    return observed == since ? 0 : -(int)(observed & 4095u);
}

int kb_fs_subsystem_errseq_check_and_advance(
    uint32_t *sequence,
    uint32_t *since)
{
    if (sequence == NULL || since == NULL) {
        return 0;
    }
    uint32_t old = __atomic_load_n(sequence, __ATOMIC_ACQUIRE);
    if (old == *since) {
        return 0;
    }
    const uint32_t replacement = old | (1u << 12);
    if (replacement != old) {
        (void)__atomic_compare_exchange_n(
            sequence,
            &old,
            replacement,
            0,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE);
    }
    *since = replacement;
    return -(int)(replacement & 4095u);
}

void kb_fs_subsystem_filemap_set_wb_err(void *mapping, int error)
{
    if (mapping == NULL || error == 0) {
        return;
    }
    (void)kb_fs_subsystem_errseq_set(
        (uint32_t *)((uint8_t *)mapping + KB_FS_ADDRESS_SPACE_WB_ERR_OFFSET),
        error);
}

int kb_fs_subsystem_file_check_and_advance_wb_err(void *file)
{
    if (file == NULL) {
        return -22;
    }
    void *mapping = read_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -22;
    }
    uint32_t *mapping_error =
        (uint32_t *)((uint8_t *)mapping + KB_FS_ADDRESS_SPACE_WB_ERR_OFFSET);
    uint32_t *file_error =
        (uint32_t *)((uint8_t *)file + KB_FS_NATIVE_FILE_WB_ERR_OFFSET);
    int status = 0;
    const uint32_t old = __atomic_load_n(file_error, __ATOMIC_ACQUIRE);
    if (kb_fs_subsystem_errseq_check(mapping_error, old) != 0) {
        status = kb_fs_subsystem_errseq_check_and_advance(mapping_error, file_error);
    }
    (void)__atomic_fetch_and(
        (unsigned long *)((uint8_t *)mapping + KB_FS_ADDRESS_SPACE_FLAGS_OFFSET),
        ~3ul,
        __ATOMIC_ACQ_REL);
    return status;
}

int kb_fs_subsystem_vfs_fsync_range(
    void *file,
    int64_t start,
    int64_t end,
    int datasync)
{
    if (file == NULL || start < 0 || end < start) {
        return -22;
    }
    void *file_operations = read_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET);
    void *fsync_operation = file_operations == NULL ? NULL :
        read_pointer_field(file_operations, KB_FS_FILE_OP_FSYNC_OFFSET);
    if (fsync_operation == NULL) {
        return -22;
    }
    int (*fsync_fn)(void *, int64_t, int64_t, int) = NULL;
    memcpy(&fsync_fn, &fsync_operation, sizeof(fsync_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(fsync_operation, &old_gs);
    const int status = fsync_fn(file, start, end, datasync);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return status;
}

int kb_fs_subsystem_sync_super(void *super_block, int wait)
{
    if (super_block == NULL) {
        return -22;
    }
    void *super_ops = read_pointer_field(
        super_block,
        KB_FS_SUPER_BLOCK_OPS_OFFSET);
    void *sync_operation = super_ops == NULL ? NULL :
        read_pointer_field(super_ops, KB_FS_SUPER_OP_SYNC_FS_OFFSET);
    if (sync_operation == NULL) {
        return 0;
    }
    int (*sync_fn)(void *, int) = NULL;
    memcpy(&sync_fn, &sync_operation, sizeof(sync_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(sync_operation, &old_gs);
    const int status = sync_fn(super_block, wait);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return status;
}

int kb_fs_subsystem_generic_buffers_fsync_noflush(
    void *file,
    int64_t start,
    int64_t end,
    int datasync)
{
    (void)datasync;
    int status = kb_fs_subsystem_file_write_and_wait_range(file, start, end);
    if (status != 0) {
        return status;
    }
    return kb_fs_subsystem_flush_dirty_buffers();
}

void *kb_fs_subsystem_find_get_block(
    void *bdev,
    uint64_t block_number,
    unsigned int block_size)
{
    if (block_size == 0) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; ++i) {
        kb_fs_buffer_cache_record_t *record = &buffer_cache[i];
        if (!record->active ||
            record->bdev != bdev ||
            record->block_number != block_number ||
            record->block_size != block_size ||
            record->buffer_head == NULL)
        {
            continue;
        }

        /* __find_get_block() only searches the block-device address-space.
         * A file-backed buffer_head is unhashed even when it describes the
         * same disk block, and JBD2 relies on that identity distinction. */
        void *folio = read_pointer_field(
            record->buffer_head, KB_FS_BUFFER_HEAD_FOLIO_OFFSET);
        void *mapping = read_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET);
        void *host = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
        void *super_block = read_pointer_field(host, KB_FS_INODE_SB_OFFSET);
        void *blockdev_superblock = NULL;
        void *blockdev_superblock_symbol = kb_module_shared_blockdev_superblock();
        if (blockdev_superblock_symbol != NULL) {
            memcpy(&blockdev_superblock, blockdev_superblock_symbol, sizeof(blockdev_superblock));
        }
        if (super_block != blockdev_superblock) {
            continue;
        }

        uint32_t refcount = 0;
        memcpy(&refcount,
            (const uint8_t *)record->buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
            sizeof(refcount));
        if (refcount != UINT32_MAX) {
            refcount++;
            memcpy((uint8_t *)record->buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
                &refcount,
                sizeof(refcount));
        }
        return record->buffer_head;
    }
    return NULL;
}

void *kb_fs_subsystem_bdev_getblk(void *bdev, uint64_t block_number, unsigned int block_size, unsigned int gfp)
{
    if (block_size == 0 || block_size > 65536u) {
        fprintf(stderr,
            "kobox-fs-error: bdev_getblk invalid-size bdev=%p block=%llu size=%u\n",
            bdev,
            (unsigned long long)block_number,
            block_size);
        return NULL;
    }
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (device == NULL) {
        fprintf(stderr,
            "kobox-fs-error: bdev_getblk no-device bdev=%p block=%llu size=%u\n",
            bdev,
            (unsigned long long)block_number,
            block_size);
        return NULL;
    }

    FS_HOTPATH_BEGIN(buffer_lookup_start);
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
    FS_HOTPATH_END(buffer_lookup, buffer_lookup_start);
    int cache_hit = buffer_head != NULL && data != NULL;
    if (cache_hit) {
        uint32_t refcount = 0;
        memcpy(&refcount, (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, sizeof(refcount));
        refcount++;
        memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));
    }
    if (!cache_hit) {
        size_t cache_slot = KB_FS_BUFFER_CACHE_MAX;
        for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
            if (!buffer_cache[i].active) {
                cache_slot = i;
                break;
            }
        }
        if (cache_slot == KB_FS_BUFFER_CACHE_MAX) {
            for (size_t distance = 0; distance < KB_FS_BUFFER_CACHE_MAX; distance++) {
                size_t i = (buffer_cache_evict_cursor + distance) % KB_FS_BUFFER_CACHE_MAX;
                kb_fs_buffer_cache_record_t *record = &buffer_cache[i];
                if (record->dirty || record->buffer_head == NULL) {
                    continue;
                }
                uint32_t refcount = 0;
                memcpy(&refcount,
                    (const uint8_t *)record->buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
                    sizeof(refcount));
                if (refcount != 0) {
                    continue;
                }
                buffer_cache_release_storage(record);
                memset(record, 0, sizeof(*record));
                cache_slot = i;
                buffer_cache_evict_cursor = (i + 1u) % KB_FS_BUFFER_CACHE_MAX;
                break;
            }
        }

        unsigned int folio_order = 0;
        size_t folio_bytes = KB_FS_PAGE_SIZE;
        while (folio_bytes < block_size && folio_order < 7u) {
            folio_order++;
            folio_bytes <<= 1u;
        }
        if (folio_bytes < block_size) {
            fprintf(stderr,
                "kobox-fs-error: bdev_getblk unsupported-order block=%llu size=%u\n",
                (unsigned long long)block_number,
                block_size);
            return NULL;
        }
        buffer_head = kb_kzalloc(KB_FS_FAKE_BUFFER_HEAD_BYTES, 0);
        folio = kb_kvm_alloc_pages_stub(gfp, folio_order);
        data = folio == NULL ? NULL :
            kb_linux_kvm_page_payload(folio, 0, (size_t)block_size);
        if (buffer_head == NULL || folio == NULL || data == NULL) {
            fprintf(stderr,
                "kobox-fs-error: bdev_getblk allocation block=%llu size=%u bh=%p folio=%p data=%p\n",
                (unsigned long long)block_number,
                block_size,
                buffer_head,
                folio,
                data);
            kb_kfree(buffer_head);
            if (folio != NULL) {
                kb_kvm_free_pages_stub(folio, folio_order);
            }
            return NULL;
        }

        /* Linux getblk only materializes a mapped buffer.  Reading here used
         * to make every caller synchronous and prevented ext4's blk_plug
         * bitmap prefetch from ever reaching submit_bh(). */
        uint64_t flags = KB_FS_BH_MAPPED;
        uint32_t refcount = 1u;
        memcpy(buffer_head, &flags, sizeof(flags));
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET, buffer_head);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_FOLIO_OFFSET, folio);
        write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_SIZE_OFFSET, block_size);
        write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET, block_number);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_DATA_OFFSET, data);
        write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_BDEV_OFFSET, bdev);
        void *association_head =
            (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET;
        write_pointer_field(
            buffer_head,
            KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET,
            association_head);
        write_pointer_field(
            buffer_head,
            KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET + sizeof(void *),
            association_head);
        write_pointer_field(folio, KB_FS_FOLIO_MAPPING_OFFSET, mount_probe_bdev_mapping);
        memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));

        if (cache_slot != KB_FS_BUFFER_CACHE_MAX) {
            buffer_cache[cache_slot].active = 1;
            buffer_cache[cache_slot].bdev = bdev;
            buffer_cache[cache_slot].block_number = block_number;
            buffer_cache[cache_slot].block_size = block_size;
            buffer_cache[cache_slot].buffer_head = buffer_head;
            buffer_cache[cache_slot].folio = folio;
            buffer_cache[cache_slot].data = data;
            buffer_cache[cache_slot].folio_order = folio_order;
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

void *kb_fs_subsystem_bread_gfp(
    void *bdev,
    uint64_t block_number,
    unsigned int block_size,
    unsigned int gfp)
{
    void *buffer_head = kb_fs_subsystem_bdev_getblk(
        bdev, block_number, block_size, gfp);
    if (buffer_head == NULL) {
        return NULL;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_UPTODATE) != 0) {
        return buffer_head;
    }
    kb_fs_subsystem_lock_buffer(buffer_head);
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_UPTODATE) != 0) {
        kb_fs_subsystem_unlock_buffer(buffer_head);
        return buffer_head;
    }
    if (kb_fs_subsystem_bh_read(buffer_head, 0u, 1) != 0) {
        kb_fs_subsystem_buffer_head_put(buffer_head);
        return NULL;
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
    uint32_t refcount = 0;
    memcpy(
        &refcount,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        sizeof(refcount));
    if (refcount != 0) {
        refcount--;
        memcpy(
            (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
            &refcount,
            sizeof(refcount));
    }
}

void *kb_fs_subsystem_alloc_buffer_head(unsigned int gfp)
{
    void *buffer_head = kb_kzalloc(KB_FS_FAKE_BUFFER_HEAD_BYTES, gfp);
    if (buffer_head == NULL) {
        return NULL;
    }

    void *association_head =
        (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET;
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET,
        association_head);
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET + sizeof(void *),
        association_head);
    const uint32_t refcount = 1u;
    memcpy(
        (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        &refcount,
        sizeof(refcount));
    return buffer_head;
}

void kb_fs_subsystem_free_buffer_head(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; i++) {
        if (buffer_cache[i].active && buffer_cache[i].buffer_head == buffer_head) {
            return;
        }
    }
    kb_kfree(buffer_head);
}

void kb_fs_subsystem_folio_set_bh(void *buffer_head, void *folio, unsigned long offset)
{
    if (buffer_head == NULL || folio == NULL || offset >= KB_FS_PAGE_SIZE) {
        return;
    }
    void *payload = kb_linux_kvm_page_payload(
        folio,
        offset,
        KB_FS_PAGE_SIZE - (size_t)offset);
    if (payload == NULL) {
        return;
    }
    write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_FOLIO_OFFSET, folio);
    write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_DATA_OFFSET, payload);
}

int kb_fs_subsystem_setattr_prepare(void *idmap, void *dentry, void *iattr)
{
    (void)idmap;
    if (dentry == NULL || iattr == NULL) {
        return -22;
    }
    void *inode = read_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (inode == NULL) {
        return -2;
    }
    uint32_t valid = 0;
    memcpy(&valid, (const uint8_t *)iattr + KB_FS_IATTR_VALID_OFFSET, sizeof(valid));
    if ((valid & KB_FS_ATTR_SIZE) != 0) {
        int64_t size = 0;
        uint16_t mode = 0;
        memcpy(&size, (const uint8_t *)iattr + KB_FS_IATTR_SIZE_OFFSET, sizeof(size));
        memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
        if (size < 0 || (mode & 0170000u) != 0100000u) {
            return -22;
        }
    }
    const uint32_t time_bits[] = {KB_FS_ATTR_ATIME, KB_FS_ATTR_MTIME, KB_FS_ATTR_CTIME};
    const size_t time_offsets[] = {KB_FS_IATTR_ATIME_OFFSET, KB_FS_IATTR_MTIME_OFFSET, KB_FS_IATTR_CTIME_OFFSET};
    for (size_t i = 0; i < sizeof(time_bits) / sizeof(time_bits[0]); ++i) {
        if ((valid & time_bits[i]) == 0) {
            continue;
        }
        int64_t nsec = 0;
        memcpy(&nsec, (const uint8_t *)iattr + time_offsets[i] + sizeof(int64_t), sizeof(nsec));
        if (nsec < 0 || nsec >= 1000000000ll) {
            return -22;
        }
    }
    return 0;
}

void kb_fs_subsystem_setattr_copy(void *idmap, void *inode, const void *iattr)
{
    (void)idmap;
    if (inode == NULL || iattr == NULL) {
        return;
    }
    uint32_t valid = 0;
    memcpy(&valid, (const uint8_t *)iattr + KB_FS_IATTR_VALID_OFFSET, sizeof(valid));
    if ((valid & KB_FS_ATTR_MODE) != 0) {
        memcpy((uint8_t *)inode + KB_FS_INODE_MODE_OFFSET,
            (const uint8_t *)iattr + KB_FS_IATTR_MODE_OFFSET,
            sizeof(uint16_t));
    }
    if ((valid & KB_FS_ATTR_UID) != 0) {
        memcpy((uint8_t *)inode + KB_FS_INODE_UID_OFFSET,
            (const uint8_t *)iattr + KB_FS_IATTR_UID_OFFSET,
            sizeof(uint32_t));
    }
    if ((valid & KB_FS_ATTR_GID) != 0) {
        memcpy((uint8_t *)inode + KB_FS_INODE_GID_OFFSET,
            (const uint8_t *)iattr + KB_FS_IATTR_GID_OFFSET,
            sizeof(uint32_t));
    }
    if ((valid & KB_FS_ATTR_SIZE) != 0) {
        memcpy((uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
            (const uint8_t *)iattr + KB_FS_IATTR_SIZE_OFFSET,
            sizeof(int64_t));
    }
    const uint32_t time_bits[] = {KB_FS_ATTR_ATIME, KB_FS_ATTR_MTIME, KB_FS_ATTR_CTIME};
    const size_t source_offsets[] = {KB_FS_IATTR_ATIME_OFFSET, KB_FS_IATTR_MTIME_OFFSET, KB_FS_IATTR_CTIME_OFFSET};
    const size_t sec_offsets[] = {KB_FS_INODE_ATIME_SEC_OFFSET, KB_FS_INODE_MTIME_SEC_OFFSET, KB_FS_INODE_CTIME_SEC_OFFSET};
    const size_t nsec_offsets[] = {KB_FS_INODE_ATIME_NSEC_OFFSET, KB_FS_INODE_MTIME_NSEC_OFFSET, KB_FS_INODE_CTIME_NSEC_OFFSET};
    for (size_t i = 0; i < sizeof(time_bits) / sizeof(time_bits[0]); ++i) {
        if ((valid & time_bits[i]) == 0) {
            continue;
        }
        memcpy((uint8_t *)inode + sec_offsets[i],
            (const uint8_t *)iattr + source_offsets[i],
            sizeof(int64_t));
        memcpy((uint8_t *)inode + nsec_offsets[i],
            (const uint8_t *)iattr + source_offsets[i] + sizeof(int64_t),
            sizeof(uint32_t));
    }
}

void kb_fs_subsystem_mark_buffer_dirty(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    state |= KB_FS_BH_DIRTY;
    write_u64_field(buffer_head, 0, state);
    void *folio = read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_FOLIO_OFFSET);
    kb_fs_filemap_folio_record_t *folio_record = filemap_folio_record(folio);
    if (folio_record != NULL) {
        (void)kb_fs_subsystem_filemap_dirty_folio(folio_record->mapping, folio);
        return;
    }
    kb_fs_buffer_cache_record_t *record = buffer_cache_record_for_head(buffer_head);
    if (record != NULL) {
        buffer_cache_set_dirty(record, 1);
    }
}

void kb_fs_subsystem_mark_buffer_dirty_inode(
    void *buffer_head,
    void *mapping)
{
    if (buffer_head == NULL) {
        return;
    }
    kb_fs_subsystem_mark_buffer_dirty(buffer_head);
    if (mapping != NULL) {
        write_pointer_field(
            buffer_head,
            KB_FS_BUFFER_HEAD_ASSOC_MAP_OFFSET,
            mapping);
    }
}

static void buffer_head_clear_association(void *buffer_head, void *mapping)
{
    if (buffer_head == NULL ||
        read_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_ASSOC_MAP_OFFSET) != mapping)
    {
        return;
    }
    write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_ASSOC_MAP_OFFSET, NULL);
    void *association_head =
        (uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET;
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET,
        association_head);
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_ASSOC_LIST_OFFSET + sizeof(void *),
        association_head);
}

void kb_fs_subsystem_invalidate_inode_buffers(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_FS_BUFFER_CACHE_MAX; ++i) {
        if (buffer_cache[i].active) {
            buffer_head_clear_association(
                buffer_cache[i].buffer_head,
                mapping);
        }
    }
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
        kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (!record->active || record->folio == NULL) {
            continue;
        }
        void *head = read_pointer_field(
            record->folio,
            KB_FS_FOLIO_PRIVATE_OFFSET);
        if (head == NULL) {
            continue;
        }
        void *buffer_head = head;
        for (size_t visited = 0; visited < KB_FS_PAGE_SIZE; ++visited) {
            buffer_head_clear_association(buffer_head, mapping);
            void *next = read_pointer_field(
                buffer_head,
                KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
            if (next == NULL || next == head) {
                break;
            }
            buffer_head = next;
        }
    }
}

void kb_fs_subsystem_bforget(void *buffer_head)
{
    if (buffer_head == NULL) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    write_u64_field(buffer_head, 0, state & ~KB_FS_BH_DIRTY);
    buffer_cache_set_dirty(buffer_cache_record_for_head(buffer_head), 0);
    kb_fs_subsystem_buffer_head_put(buffer_head);
}

int kb_fs_subsystem_bh_uptodate_or_lock(void *buffer_head)
{
    if (buffer_head == NULL) {
        return 0;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_UPTODATE) != 0) {
        return 1;
    }
    kb_fs_subsystem_lock_buffer(buffer_head);
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_UPTODATE) == 0) {
        return 0;
    }
    kb_fs_subsystem_unlock_buffer(buffer_head);
    return 1;
}

int kb_fs_subsystem_sync_dirty_buffer(void *buffer_head)
{
    if (buffer_head == NULL) {
        return -22;
    }
    kb_fs_subsystem_lock_buffer(buffer_head);
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    if ((state & KB_FS_BH_DIRTY) == 0) {
        kb_fs_subsystem_unlock_buffer(buffer_head);
        return 0;
    }
    if ((state & KB_FS_BH_MAPPED) == 0) {
        kb_fs_subsystem_unlock_buffer(buffer_head);
        return -5;
    }
    state &= ~KB_FS_BH_DIRTY;
    write_u64_field(buffer_head, 0, state);
    uint32_t refcount = 0;
    memcpy(&refcount,
        (const uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        sizeof(refcount));
    if (refcount == UINT32_MAX) {
        state |= KB_FS_BH_DIRTY;
        write_u64_field(buffer_head, 0, state);
        kb_fs_subsystem_unlock_buffer(buffer_head);
        return -75;
    }
    refcount++;
    memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET,
        &refcount,
        sizeof(refcount));
    write_pointer_field(
        buffer_head,
        KB_FS_BUFFER_HEAD_END_IO_OFFSET,
        (void *)&kb_fs_subsystem_end_buffer_write_sync);
    kb_fs_subsystem_submit_bh(KB_FS_BIO_OP_WRITE, buffer_head);
    kb_fs_subsystem_wait_on_buffer(buffer_head);
    memcpy(&state, buffer_head, sizeof(state));
    return (state & (KB_FS_BH_WRITE_EIO | KB_FS_BH_UPTODATE)) == KB_FS_BH_UPTODATE ? 0 : -5;
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
    int partial = 0;
    int dirtied = 0;
    void *payload = folio_page_payload(page);
    void *bh = head;
    do {
        uint64_t block_size = 0;
        memcpy(&block_size, (const uint8_t *)bh + KB_FS_BUFFER_HEAD_SIZE_OFFSET, sizeof(block_size));
        if (block_size == 0 || block_size > KB_FS_PAGE_SIZE) {
            break;
        }
        uint64_t block_end = block_start + block_size;
        if (block_end > start && block_start < end) {
            void *data = NULL;
            memcpy(&data, (const uint8_t *)bh + KB_FS_BUFFER_HEAD_DATA_OFFSET, sizeof(data));
            if (payload != NULL && data != NULL) {
                const uint64_t copy_start = start > block_start ? start : block_start;
                const uint64_t copy_end = end < block_end ? end : block_end;
                /* File-backed buffer_heads normally point directly into the
                 * folio payload.  generic_perform_write() has already copied
                 * the iterator there, so copying that range onto itself is
                 * redundant.  Keep the detached-data fallback for callers
                 * whose buffer_head owns separate storage. */
                if (copy_end > copy_start &&
                    data != (uint8_t *)payload + block_start)
                {
                    memcpy(
                        (uint8_t *)data + (copy_start - block_start),
                        (const uint8_t *)payload + copy_start,
                        (size_t)(copy_end - copy_start));
                }
            }
            uint64_t state = 0;
            memcpy(&state, bh, sizeof(state));
            state |= KB_FS_BH_UPTODATE | KB_FS_BH_DIRTY;
            state &= ~KB_FS_BH_NEW;
            write_u64_field(bh, 0, state);
            dirtied = 1;
        } else {
            uint64_t state = 0;
            memcpy(&state, bh, sizeof(state));
            if ((state & KB_FS_BH_UPTODATE) == 0) {
                partial = 1;
            }
        }
        bh = read_pointer_field(bh, KB_FS_BUFFER_HEAD_THIS_PAGE_OFFSET);
        block_start = block_end;
    } while (bh != NULL && bh != head && block_start < KB_FS_PAGE_SIZE);

    if (dirtied) {
        void *dirty_mapping = mapping != NULL ? mapping :
            read_pointer_field(page, KB_FS_FOLIO_MAPPING_OFFSET);
        (void)kb_fs_subsystem_filemap_dirty_folio(dirty_mapping, page);
    }

    if (!partial) {
        uint64_t flags = 0;
        memcpy(&flags, page, sizeof(flags));
        flags |= KB_FS_FOLIO_FLAG_UPTODATE;
        write_u64_field(page, 0, flags);
    }

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
    void *bdev = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_BDEV_OFFSET);
    if (bdev != NULL && kb_fs_subsystem_set_blocksize(bdev, size) != 0) {
        return 0;
    }
    return set_super_blocksize(super_block, (uint64_t)size);
}

int kb_fs_subsystem_set_blocksize(void *bdev, int size)
{
    if (bdev == NULL || size < 512 || size > KB_FS_PAGE_SIZE ||
        (size & (size - 1)) != 0)
    {
        return -22;
    }
    uint32_t logical_block_size = 512;
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (device != NULL && device->logical_block_size != 0) {
        logical_block_size = device->logical_block_size;
    }
    if ((uint32_t)size < logical_block_size) {
        return -22;
    }
    void *mapping = read_pointer_field(bdev, KB_FS_BDEV_MAPPING_OFFSET);
    void *inode = mapping == NULL ? NULL :
        read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (inode == NULL) {
        return -22;
    }
    uint8_t block_bits = 0;
    for (unsigned int value = (unsigned int)size; value > 1u; value >>= 1u) {
        ++block_bits;
    }
    uint8_t current_bits = 0;
    memcpy(
        &current_bits,
        (const uint8_t *)inode + KB_FS_INODE_BLKBITS_OFFSET,
        sizeof(current_bits));
    if (current_bits != block_bits) {
        /* Linux set_blocksize() writes and invalidates the block-device page
         * cache here; it does not issue a cache-flush command to hardware.
         * Keep REQ_PREFLUSH/FUA responsibility in the journal and fsync paths. */
        const int sync_status = kb_fs_subsystem_flush_dirty_buffers();
        if (sync_status != 0) {
            return sync_status;
        }
        memcpy(
            (uint8_t *)inode + KB_FS_INODE_BLKBITS_OFFSET,
            &block_bits,
            sizeof(block_bits));
        kb_fs_subsystem_invalidate_bdev(bdev);
    }
    return 0;
}

int kb_fs_subsystem_generic_check_addressable(
    unsigned int blocksize_bits,
    uint64_t num_blocks)
{
    if (num_blocks == 0) {
        return 0;
    }
    if (blocksize_bits < 9u || blocksize_bits > 12u) {
        return -22;
    }
    const uint64_t last_block = num_blocks - 1u;
    return last_block > UINT64_MAX >> (blocksize_bits - 9u) ? -27 : 0;
}

int kb_fs_subsystem_inode_newsize_ok(void *inode, int64_t size)
{
    if (low_or_err_pointer(inode) || size < 0) {
        return -22;
    }
    uint64_t old_size = 0;
    uint32_t inode_flags = 0;
    memcpy(&old_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(old_size));
    memcpy(&inode_flags, (const uint8_t *)inode + KB_FS_INODE_FLAGS_OFFSET,
        sizeof(inode_flags));
    if ((uint64_t)size <= old_size) {
        return (inode_flags & KB_FS_INODE_SWAPFILE) != 0 ? -26 : 0;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    if (super_block == NULL) {
        return -22;
    }
    int64_t max_bytes = 0;
    memcpy(
        &max_bytes,
        (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_MAXBYTES_OFFSET,
        sizeof(max_bytes));
    return max_bytes > 0 && size > max_bytes ? -27 : 0;
}

static kb_fs_inode_allocation_record_t *inode_allocation_find(void *inode)
{
    FS_HOTPATH_BEGIN(profile_start);
    for (size_t i = 0; i < KB_FS_INODE_ALLOCATION_MAX; ++i) {
        if (inode_allocations[i].active && inode_allocations[i].inode == inode) {
            kb_fs_inode_allocation_record_t *record = &inode_allocations[i];
            FS_HOTPATH_END(inode_find, profile_start);
            return record;
        }
    }
    FS_HOTPATH_END(inode_find, profile_start);
    return NULL;
}

enum {
    KB_FS_ACL_TYPE_ACCESS = 0x8000,
    KB_FS_ACL_TYPE_DEFAULT = 0x4000,
    KB_FS_ACL_USER_OBJ = 0x01,
    KB_FS_ACL_USER = 0x02,
    KB_FS_ACL_GROUP_OBJ = 0x04,
    KB_FS_ACL_GROUP = 0x08,
    KB_FS_ACL_MASK = 0x10,
    KB_FS_ACL_OTHER = 0x20,
    KB_FS_ACL_PERMISSION_BITS = 07,
    KB_FS_INODE_OP_GET_INODE_ACL_OFFSET = 0x18,
    KB_FS_INODE_OP_SET_ACL_OFFSET = 0xa8,
};

static void kb_fs_posix_acl_get(void *opaque_acl)
{
    if (low_or_err_pointer(opaque_acl)) {
        return;
    }
    kb_fs_posix_acl_t *acl = opaque_acl;
    __atomic_fetch_add(&acl->reference_count, 1u, __ATOMIC_RELAXED);
}

void kb_fs_subsystem_posix_acl_release(void *opaque_acl)
{
    if (low_or_err_pointer(opaque_acl)) {
        return;
    }
    kb_fs_posix_acl_t *acl = opaque_acl;
    if (__atomic_fetch_sub(
            &acl->reference_count,
            1u,
            __ATOMIC_ACQ_REL) == 1u)
    {
        kb_kfree(acl);
    }
}

void *kb_fs_subsystem_posix_acl_alloc(int count, unsigned int flags)
{
    (void)flags;
    if (count < 0 ||
        (size_t)count >
            (SIZE_MAX - sizeof(kb_fs_posix_acl_t)) /
                sizeof(kb_fs_posix_acl_entry_t))
    {
        return NULL;
    }
    const size_t bytes = sizeof(kb_fs_posix_acl_t) +
        (size_t)count * sizeof(kb_fs_posix_acl_entry_t);
    kb_fs_posix_acl_t *acl = kb_kmalloc(bytes, 0);
    if (acl == NULL) {
        return NULL;
    }
    memset(acl, 0, bytes);
    acl->reference_count = 1;
    acl->count = (uint32_t)count;
    return acl;
}

static int kb_fs_acl_cache_slot(
    kb_fs_inode_allocation_record_t *allocation,
    int type,
    void ***out_acl,
    int **out_cached)
{
    if (allocation == NULL || out_acl == NULL || out_cached == NULL) {
        return -22;
    }
    if (type == KB_FS_ACL_TYPE_ACCESS) {
        *out_acl = &allocation->acl_access;
        *out_cached = &allocation->acl_access_cached;
        return 0;
    }
    if (type == KB_FS_ACL_TYPE_DEFAULT) {
        *out_acl = &allocation->acl_default;
        *out_cached = &allocation->acl_default_cached;
        return 0;
    }
    return -22;
}

void kb_fs_subsystem_set_cached_acl(void *inode, int type, void *acl)
{
    kb_fs_inode_allocation_record_t *allocation =
        inode_allocation_find(inode);
    void **slot = NULL;
    int *cached = NULL;
    if (kb_fs_acl_cache_slot(allocation, type, &slot, &cached) != 0) {
        return;
    }
    kb_fs_posix_acl_get(acl);
    void *old_acl = *slot;
    *slot = acl;
    *cached = 1;
    kb_fs_subsystem_posix_acl_release(old_acl);
}

void *kb_fs_subsystem_get_inode_acl(void *inode, int type)
{
    if (low_or_err_pointer(inode)) {
        return fs_err_ptr(-22);
    }
    kb_fs_inode_allocation_record_t *allocation =
        inode_allocation_find(inode);
    void **slot = NULL;
    int *cached = NULL;
    if (kb_fs_acl_cache_slot(allocation, type, &slot, &cached) != 0) {
        return fs_err_ptr(-22);
    }
    if (*cached) {
        kb_fs_posix_acl_get(*slot);
        return *slot;
    }

    void *inode_operations = read_pointer_field(inode, KB_FS_INODE_OP_OFFSET);
    void *operation = inode_operations == NULL ? NULL :
        read_pointer_field(
            inode_operations,
            KB_FS_INODE_OP_GET_INODE_ACL_OFFSET);
    if (operation == NULL) {
        kb_fs_subsystem_set_cached_acl(inode, type, NULL);
        return NULL;
    }
    void *(*get_acl_fn)(void *, int, int) = NULL;
    memcpy(&get_acl_fn, &operation, sizeof(get_acl_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    void *acl = get_acl_fn(inode, type, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (!((intptr_t)acl < 0 && (intptr_t)acl >= -4095)) {
        kb_fs_subsystem_set_cached_acl(inode, type, acl);
    }
    return acl;
}

static void *kb_fs_posix_acl_clone(void *opaque_acl)
{
    if (low_or_err_pointer(opaque_acl)) {
        return NULL;
    }
    const kb_fs_posix_acl_t *acl = opaque_acl;
    kb_fs_posix_acl_t *clone =
        kb_fs_subsystem_posix_acl_alloc((int)acl->count, 0);
    if (clone == NULL) {
        return NULL;
    }
    memcpy(
        clone->entries,
        acl->entries,
        (size_t)acl->count * sizeof(*acl->entries));
    return clone;
}

static int kb_fs_posix_acl_equiv_mode(
    const kb_fs_posix_acl_t *acl,
    uint16_t *mode_pointer)
{
    if (acl == NULL) {
        return 0;
    }
    uint16_t mode = 0;
    int not_equivalent = 0;
    for (uint32_t i = 0; i < acl->count; ++i) {
        const kb_fs_posix_acl_entry_t *entry = &acl->entries[i];
        if ((entry->permission & ~KB_FS_ACL_PERMISSION_BITS) != 0) {
            return -22;
        }
        switch (entry->tag) {
        case KB_FS_ACL_USER_OBJ:
            mode |= (uint16_t)(entry->permission << 6);
            break;
        case KB_FS_ACL_GROUP_OBJ:
            mode |= (uint16_t)(entry->permission << 3);
            break;
        case KB_FS_ACL_OTHER:
            mode |= entry->permission;
            break;
        case KB_FS_ACL_MASK:
            mode = (uint16_t)((mode & ~0070u) |
                (entry->permission << 3));
            not_equivalent = 1;
            break;
        case KB_FS_ACL_USER:
        case KB_FS_ACL_GROUP:
            not_equivalent = 1;
            break;
        default:
            return -22;
        }
    }
    if (mode_pointer != NULL) {
        *mode_pointer = (uint16_t)((*mode_pointer & ~0777u) | mode);
    }
    return not_equivalent;
}

static int kb_fs_posix_acl_create_mask(
    kb_fs_posix_acl_t *acl,
    uint16_t *mode_pointer)
{
    kb_fs_posix_acl_entry_t *group = NULL;
    kb_fs_posix_acl_entry_t *mask = NULL;
    uint16_t mode = *mode_pointer;
    int not_equivalent = 0;
    for (uint32_t i = 0; i < acl->count; ++i) {
        kb_fs_posix_acl_entry_t *entry = &acl->entries[i];
        switch (entry->tag) {
        case KB_FS_ACL_USER_OBJ:
            entry->permission &= (mode >> 6) & 07u;
            mode &= (uint16_t)((entry->permission << 6) | ~0700u);
            break;
        case KB_FS_ACL_USER:
        case KB_FS_ACL_GROUP:
            not_equivalent = 1;
            break;
        case KB_FS_ACL_GROUP_OBJ:
            group = entry;
            break;
        case KB_FS_ACL_OTHER:
            entry->permission &= mode & 07u;
            mode &= (uint16_t)(entry->permission | ~0007u);
            break;
        case KB_FS_ACL_MASK:
            mask = entry;
            not_equivalent = 1;
            break;
        default:
            return -5;
        }
    }
    kb_fs_posix_acl_entry_t *group_class = mask != NULL ? mask : group;
    if (group_class == NULL) {
        return -5;
    }
    group_class->permission &= (mode >> 3) & 07u;
    mode &= (uint16_t)((group_class->permission << 3) | ~0070u);
    *mode_pointer = (uint16_t)((*mode_pointer & ~0777u) | mode);
    return not_equivalent;
}

static int kb_fs_posix_acl_chmod_mask(
    kb_fs_posix_acl_t *acl,
    uint16_t mode)
{
    kb_fs_posix_acl_entry_t *group = NULL;
    kb_fs_posix_acl_entry_t *mask = NULL;
    for (uint32_t i = 0; i < acl->count; ++i) {
        kb_fs_posix_acl_entry_t *entry = &acl->entries[i];
        switch (entry->tag) {
        case KB_FS_ACL_USER_OBJ:
            entry->permission = (mode >> 6) & 07u;
            break;
        case KB_FS_ACL_USER:
        case KB_FS_ACL_GROUP:
            break;
        case KB_FS_ACL_GROUP_OBJ:
            group = entry;
            break;
        case KB_FS_ACL_MASK:
            mask = entry;
            break;
        case KB_FS_ACL_OTHER:
            entry->permission = mode & 07u;
            break;
        default:
            return -5;
        }
    }
    kb_fs_posix_acl_entry_t *group_class = mask != NULL ? mask : group;
    if (group_class == NULL) {
        return -5;
    }
    group_class->permission = (mode >> 3) & 07u;
    return 0;
}

int kb_fs_subsystem_posix_acl_create(
    void *dir,
    unsigned short *mode,
    void **default_acl,
    void **acl)
{
    if (low_or_err_pointer(dir) || mode == NULL ||
        default_acl == NULL || acl == NULL)
    {
        return -22;
    }
    *default_acl = NULL;
    *acl = NULL;
    if ((*mode & 0170000u) == 0120000u) {
        return 0;
    }
    void *parent_acl = kb_fs_subsystem_get_inode_acl(
        dir,
        KB_FS_ACL_TYPE_DEFAULT);
    if (parent_acl == NULL || (intptr_t)parent_acl == -95) {
        /* Filed receives the caller's already-umask-filtered mode.  Applying
         * filed's own umask here would incorrectly mask it a second time. */
        return 0;
    }
    if ((intptr_t)parent_acl < 0 && (intptr_t)parent_acl >= -4095) {
        return (int)(intptr_t)parent_acl;
    }
    kb_fs_posix_acl_t *clone = kb_fs_posix_acl_clone(parent_acl);
    if (clone == NULL) {
        kb_fs_subsystem_posix_acl_release(parent_acl);
        return -12;
    }
    const int status = kb_fs_posix_acl_create_mask(clone, mode);
    if (status < 0) {
        kb_fs_subsystem_posix_acl_release(clone);
        kb_fs_subsystem_posix_acl_release(parent_acl);
        return status;
    }
    if (status == 0) {
        kb_fs_subsystem_posix_acl_release(clone);
    } else {
        *acl = clone;
    }
    if ((*mode & 0170000u) == 0040000u) {
        *default_acl = parent_acl;
    } else {
        kb_fs_subsystem_posix_acl_release(parent_acl);
    }
    return 0;
}

int kb_fs_subsystem_posix_acl_update_mode(
    void *idmap,
    void *inode,
    unsigned short *mode,
    void **acl)
{
    (void)idmap;
    if (low_or_err_pointer(inode) || mode == NULL || acl == NULL) {
        return -22;
    }
    uint16_t updated_mode = 0;
    memcpy(
        &updated_mode,
        (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET,
        sizeof(updated_mode));
    const int status = kb_fs_posix_acl_equiv_mode(*acl, &updated_mode);
    if (status < 0) {
        return status;
    }
    if (status == 0) {
        *acl = NULL;
    }
    *mode = updated_mode;
    return 0;
}

int kb_fs_subsystem_posix_acl_chmod(
    void *idmap,
    void *dentry,
    unsigned short mode)
{
    if (dentry == NULL) {
        return -22;
    }
    void *inode = read_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    void *inode_operations = read_pointer_field(inode, KB_FS_INODE_OP_OFFSET);
    void *operation = inode_operations == NULL ? NULL :
        read_pointer_field(inode_operations, KB_FS_INODE_OP_SET_ACL_OFFSET);
    if (operation == NULL) {
        return -95;
    }
    void *old_acl = kb_fs_subsystem_get_inode_acl(
        inode,
        KB_FS_ACL_TYPE_ACCESS);
    if (old_acl == NULL || (intptr_t)old_acl == -95) {
        return 0;
    }
    if ((intptr_t)old_acl < 0 && (intptr_t)old_acl >= -4095) {
        return (int)(intptr_t)old_acl;
    }
    kb_fs_posix_acl_t *new_acl = kb_fs_posix_acl_clone(old_acl);
    kb_fs_subsystem_posix_acl_release(old_acl);
    if (new_acl == NULL) {
        return -12;
    }
    int status = kb_fs_posix_acl_chmod_mask(new_acl, mode);
    if (status == 0) {
        int (*set_acl_fn)(void *, void *, void *, int) = NULL;
        memcpy(&set_acl_fn, &operation, sizeof(set_acl_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
        status = set_acl_fn(idmap, dentry, new_acl, KB_FS_ACL_TYPE_ACCESS);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    kb_fs_subsystem_posix_acl_release(new_acl);
    return status;
}

static kb_fs_inode_allocation_record_t *inode_allocation_claim(void)
{
    FS_HOTPATH_BEGIN(profile_start);
    for (size_t i = 0; i < KB_FS_INODE_ALLOCATION_MAX; ++i) {
        if (!inode_allocations[i].active) {
            memset(&inode_allocations[i], 0, sizeof(inode_allocations[i]));
            inode_allocations[i].active = 1;
            kb_fs_inode_allocation_record_t *record = &inode_allocations[i];
            FS_HOTPATH_END(inode_claim, profile_start);
            return record;
        }
    }
    FS_HOTPATH_END(inode_claim, profile_start);
    return NULL;
}

static void *super_inode_operation(void *super_block, size_t operation_offset)
{
    if (super_block == NULL) {
        return NULL;
    }
    void *operations = read_pointer_field(super_block, KB_FS_SUPER_BLOCK_OPS_OFFSET);
    return operations != NULL ? read_pointer_field(operations, operation_offset) : NULL;
}

void kb_fs_subsystem_mark_inode_dirty(void *inode, int flags)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
    if (allocation != NULL) {
        allocation->dirty_metadata = 1;
    }
    uint64_t state = 0;
    memcpy(
        &state,
        (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET,
        sizeof(state));
    state |= (uint64_t)(unsigned int)flags &
        (KB_FS_INODE_STATE_DIRTY_SYNC |
         KB_FS_INODE_STATE_DIRTY_DATASYNC |
         KB_FS_INODE_STATE_DIRTY_PAGES |
         KB_FS_INODE_STATE_DIRTY_TIME);
    write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, state);
    if ((flags & (KB_FS_INODE_STATE_DIRTY_SYNC |
                  KB_FS_INODE_STATE_DIRTY_DATASYNC)) == 0)
    {
        return;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    void *operation = super_inode_operation(
        super_block,
        KB_FS_SUPER_OP_DIRTY_INODE_OFFSET);
    if (operation == NULL) {
        return;
    }
    void (*dirty_inode_fn)(void *, int) = NULL;
    memcpy(&dirty_inode_fn, &operation, sizeof(dirty_inode_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    dirty_inode_fn(inode, flags);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
}

int kb_fs_subsystem_inode_maybe_inc_iversion(void *inode, int force)
{
    if (low_or_err_pointer(inode)) {
        return 0;
    }

    uint64_t *version = (uint64_t *)((uint8_t *)inode +
        KB_FS_INODE_VERSION_OFFSET);
    uint64_t observed = __atomic_load_n(version, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!force &&
            (observed & KB_FS_INODE_VERSION_QUERIED) == 0)
        {
            return 0;
        }
        const uint64_t replacement =
            (observed & ~(uint64_t)KB_FS_INODE_VERSION_QUERIED) +
            KB_FS_INODE_VERSION_INCREMENT;
        if (__atomic_compare_exchange_n(
                version,
                &observed,
                replacement,
                0,
                __ATOMIC_SEQ_CST,
                __ATOMIC_ACQUIRE))
        {
            return 1;
        }
    }
}

uint64_t kb_fs_subsystem_inode_query_iversion(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return 0;
    }

    uint64_t *version = (uint64_t *)((uint8_t *)inode +
        KB_FS_INODE_VERSION_OFFSET);
    uint64_t observed = __atomic_load_n(version, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((observed & KB_FS_INODE_VERSION_QUERIED) != 0) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return observed >> 1u;
        }
        const uint64_t replacement =
            observed | KB_FS_INODE_VERSION_QUERIED;
        if (__atomic_compare_exchange_n(
                version,
                &observed,
                replacement,
                0,
                __ATOMIC_SEQ_CST,
                __ATOMIC_ACQUIRE))
        {
            return observed >> 1u;
        }
    }
}

void kb_fs_subsystem_inode_set_flags(
    void *inode,
    unsigned int flags,
    unsigned int mask)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint32_t *inode_flags = (uint32_t *)((uint8_t *)inode +
        KB_FS_INODE_FLAGS_OFFSET);
    uint32_t observed = __atomic_load_n(inode_flags, __ATOMIC_RELAXED);
    for (;;) {
        const uint32_t replacement =
            (observed & ~(uint32_t)mask) | (flags & mask);
        if (__atomic_compare_exchange_n(
                inode_flags,
                &observed,
                replacement,
                0,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED))
        {
            return;
        }
    }
}

kb_fs_timespec64_t kb_fs_subsystem_simple_inode_init_ts(void *inode)
{
    kb_fs_timespec64_t now = {
        .tv_sec = kb_ktime_get_real_seconds(),
        .tv_nsec = 0,
    };
    if (low_or_err_pointer(inode)) {
        return now;
    }

    const uint32_t nsec = (uint32_t)now.tv_nsec;
    memcpy((uint8_t *)inode + KB_FS_INODE_ATIME_SEC_OFFSET,
        &now.tv_sec, sizeof(now.tv_sec));
    memcpy((uint8_t *)inode + KB_FS_INODE_MTIME_SEC_OFFSET,
        &now.tv_sec, sizeof(now.tv_sec));
    memcpy((uint8_t *)inode + KB_FS_INODE_CTIME_SEC_OFFSET,
        &now.tv_sec, sizeof(now.tv_sec));
    memcpy((uint8_t *)inode + KB_FS_INODE_ATIME_NSEC_OFFSET,
        &nsec, sizeof(nsec));
    memcpy((uint8_t *)inode + KB_FS_INODE_MTIME_NSEC_OFFSET,
        &nsec, sizeof(nsec));
    memcpy((uint8_t *)inode + KB_FS_INODE_CTIME_NSEC_OFFSET,
        &nsec, sizeof(nsec));
    return now;
}

int kb_fs_subsystem_inode_needs_sync(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return 0;
    }

    uint16_t mode = 0;
    uint32_t inode_flags = 0;
    uint64_t super_flags = 0;
    memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET,
        sizeof(mode));
    memcpy(&inode_flags,
        (const uint8_t *)inode + KB_FS_INODE_FLAGS_OFFSET,
        sizeof(inode_flags));
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    if (super_block != NULL) {
        memcpy(&super_flags,
            (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_FLAGS_OFFSET,
            sizeof(super_flags));
    }

    if ((super_flags & KB_FS_SB_SYNCHRONOUS) != 0 ||
        (inode_flags & KB_FS_INODE_SYNC) != 0)
    {
        return 1;
    }
    return (mode & KB_FS_MODE_TYPE_MASK) == 0040000u &&
        ((super_flags & (KB_FS_SB_SYNCHRONOUS | KB_FS_SB_DIRSYNC)) != 0 ||
         (inode_flags & (KB_FS_INODE_SYNC | KB_FS_INODE_DIRSYNC)) != 0);
}

static int64_t kb_fs_vfs_setpos(void *file, int64_t offset, int64_t maxsize)
{
    if (file == NULL) {
        return -22;
    }
    const uint32_t mode = read_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET);
    if ((offset < 0 && (mode & KB_FS_FMODE_UNSIGNED_OFFSET) == 0) ||
        offset > maxsize)
    {
        return -22;
    }
    const int64_t old_position = (int64_t)read_u64_field(
        file,
        KB_FS_NATIVE_FILE_POSITION_OFFSET);
    if (old_position != offset) {
        write_u64_field(
            file,
            KB_FS_NATIVE_FILE_POSITION_OFFSET,
            (uint64_t)offset);
        write_u64_field(file, KB_FS_NATIVE_FILE_VERSION_OFFSET, 0);
    }
    return offset;
}

int64_t kb_fs_subsystem_generic_file_llseek_size(
    void *file,
    int64_t offset,
    int whence,
    int64_t maxsize,
    int64_t eof)
{
    enum {
        KB_FS_SEEK_SET = 0,
        KB_FS_SEEK_CUR = 1,
        KB_FS_SEEK_END = 2,
        KB_FS_SEEK_DATA = 3,
        KB_FS_SEEK_HOLE = 4,
    };
    if (file == NULL || maxsize < 0 || eof < 0) {
        return -22;
    }
    switch (whence) {
    case KB_FS_SEEK_SET:
        break;
    case KB_FS_SEEK_END:
        if ((offset > 0 && eof > INT64_MAX - offset) ||
            (offset < 0 && eof < INT64_MIN - offset))
        {
            return -22;
        }
        offset += eof;
        break;
    case KB_FS_SEEK_CUR: {
        const int64_t current = (int64_t)read_u64_field(
            file,
            KB_FS_NATIVE_FILE_POSITION_OFFSET);
        if (offset == 0) {
            return current;
        }
        if ((offset > 0 && current > INT64_MAX - offset) ||
            (offset < 0 && current < INT64_MIN - offset))
        {
            return -22;
        }
        return kb_fs_vfs_setpos(file, current + offset, maxsize);
    }
    case KB_FS_SEEK_DATA:
        if ((uint64_t)offset >= (uint64_t)eof) {
            return -6;
        }
        break;
    case KB_FS_SEEK_HOLE:
        if ((uint64_t)offset >= (uint64_t)eof) {
            return -6;
        }
        offset = eof;
        break;
    default:
        return -22;
    }
    return kb_fs_vfs_setpos(file, offset, maxsize);
}

void kb_fs_subsystem_generic_fillattr(
    void *idmap,
    uint32_t request_mask,
    void *inode,
    void *stat)
{
    (void)idmap;
    if (low_or_err_pointer(inode) || stat == NULL) {
        return;
    }

    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    uint32_t device = 0;
    uint64_t super_flags = 0;
    if (super_block != NULL) {
        /* In the Linux 6.12 randomized super_block layout, s_dev precedes
         * s_blocksize_bits at +0x10. */
        device = read_u32_field(super_block, 0x10);
        memcpy(&super_flags,
            (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_FLAGS_OFFSET,
            sizeof(super_flags));
    }

    const uint16_t mode = (uint16_t)read_u32_field(
        inode,
        KB_FS_INODE_MODE_OFFSET);
    const uint32_t uid = read_u32_field(inode, KB_FS_INODE_UID_OFFSET);
    const uint32_t gid = read_u32_field(inode, KB_FS_INODE_GID_OFFSET);
    const uint32_t nlink = read_u32_field(inode, KB_FS_INODE_NLINK_OFFSET);
    const uint32_t rdev = read_u32_field(inode, KB_FS_INODE_RDEV_OFFSET);
    const uint64_t ino = read_u64_field(inode, KB_FS_INODE_NUMBER_OFFSET);
    const uint64_t size = read_u64_field(inode, KB_FS_INODE_SIZE_OFFSET);
    const uint64_t blocks = read_u64_field(inode, KB_FS_INODE_BLOCKS_OFFSET);
    const uint8_t block_bits = read_u8_field(inode, KB_FS_INODE_BLKBITS_OFFSET);
    const uint32_t block_size = block_bits < 32u ? 1u << block_bits : 0u;

    write_u32_field(stat, KB_FS_KSTAT_MODE_OFFSET, mode);
    write_u32_field(stat, KB_FS_KSTAT_NLINK_OFFSET, nlink);
    write_u32_field(stat, KB_FS_KSTAT_BLOCKSIZE_OFFSET, block_size);
    write_u64_field(stat, KB_FS_KSTAT_INODE_OFFSET, ino);
    write_u32_field(stat, KB_FS_KSTAT_DEVICE_OFFSET, device);
    write_u32_field(stat, KB_FS_KSTAT_RDEV_OFFSET, rdev);
    write_u32_field(stat, KB_FS_KSTAT_UID_OFFSET, uid);
    write_u32_field(stat, KB_FS_KSTAT_GID_OFFSET, gid);
    write_u64_field(stat, KB_FS_KSTAT_SIZE_OFFSET, size);
    write_u64_field(stat, KB_FS_KSTAT_BLOCKS_OFFSET, blocks);

    const size_t inode_sec_offsets[] = {
        KB_FS_INODE_ATIME_SEC_OFFSET,
        KB_FS_INODE_MTIME_SEC_OFFSET,
        KB_FS_INODE_CTIME_SEC_OFFSET,
    };
    const size_t inode_nsec_offsets[] = {
        KB_FS_INODE_ATIME_NSEC_OFFSET,
        KB_FS_INODE_MTIME_NSEC_OFFSET,
        KB_FS_INODE_CTIME_NSEC_OFFSET,
    };
    const size_t stat_time_offsets[] = {
        KB_FS_KSTAT_ATIME_OFFSET,
        KB_FS_KSTAT_MTIME_OFFSET,
        KB_FS_KSTAT_CTIME_OFFSET,
    };
    for (size_t i = 0; i < 3; ++i) {
        const int64_t sec = (int64_t)read_u64_field(
            inode,
            inode_sec_offsets[i]);
        const int64_t nsec = (int64_t)read_u32_field(
            inode,
            inode_nsec_offsets[i]);
        write_u64_field(stat, stat_time_offsets[i], (uint64_t)sec);
        write_u64_field(stat, stat_time_offsets[i] + sizeof(uint64_t),
            (uint64_t)nsec);
    }

    if ((request_mask & KB_FS_STATX_CHANGE_COOKIE) != 0 &&
        (super_flags & KB_FS_SB_I_VERSION) != 0)
    {
        uint32_t result_mask = read_u32_field(
            stat,
            KB_FS_KSTAT_RESULT_MASK_OFFSET);
        result_mask |= KB_FS_STATX_CHANGE_COOKIE;
        write_u32_field(stat, KB_FS_KSTAT_RESULT_MASK_OFFSET, result_mask);
        write_u64_field(
            stat,
            KB_FS_KSTAT_CHANGE_COOKIE_OFFSET,
            kb_fs_subsystem_inode_query_iversion(inode));
    }
}

void kb_fs_subsystem_fileattr_fill_flags(void *fileattr, uint32_t flags)
{
    enum {
        KB_FS_FILEATTR_BYTES = 28,
        KB_FS_FILEATTR_FLAGS_OFFSET = 0,
        KB_FS_FILEATTR_XFLAGS_OFFSET = 4,
        KB_FS_FILEATTR_SELECTORS_OFFSET = 24,
        KB_FS_FILEATTR_FLAGS_VALID = 1u << 0,
        KB_FS_SYNC_FL = 0x00000008u,
        KB_FS_IMMUTABLE_FL = 0x00000010u,
        KB_FS_APPEND_FL = 0x00000020u,
        KB_FS_NODUMP_FL = 0x00000040u,
        KB_FS_NOATIME_FL = 0x00000080u,
        KB_FS_DAX_FL = 0x02000000u,
        KB_FS_PROJINHERIT_FL = 0x20000000u,
        KB_FS_XFLAG_IMMUTABLE = 0x00000008u,
        KB_FS_XFLAG_APPEND = 0x00000010u,
        KB_FS_XFLAG_SYNC = 0x00000020u,
        KB_FS_XFLAG_NOATIME = 0x00000040u,
        KB_FS_XFLAG_NODUMP = 0x00000080u,
        KB_FS_XFLAG_PROJINHERIT = 0x00000200u,
        KB_FS_XFLAG_DAX = 0x00008000u,
    };
    if (fileattr == NULL) {
        return;
    }
    memset(fileattr, 0, KB_FS_FILEATTR_BYTES);
    write_u32_field(fileattr, KB_FS_FILEATTR_FLAGS_OFFSET, flags);
    uint32_t xflags = 0;
    if ((flags & KB_FS_SYNC_FL) != 0) xflags |= KB_FS_XFLAG_SYNC;
    if ((flags & KB_FS_IMMUTABLE_FL) != 0) xflags |= KB_FS_XFLAG_IMMUTABLE;
    if ((flags & KB_FS_APPEND_FL) != 0) xflags |= KB_FS_XFLAG_APPEND;
    if ((flags & KB_FS_NODUMP_FL) != 0) xflags |= KB_FS_XFLAG_NODUMP;
    if ((flags & KB_FS_NOATIME_FL) != 0) xflags |= KB_FS_XFLAG_NOATIME;
    if ((flags & KB_FS_DAX_FL) != 0) xflags |= KB_FS_XFLAG_DAX;
    if ((flags & KB_FS_PROJINHERIT_FL) != 0) xflags |= KB_FS_XFLAG_PROJINHERIT;
    write_u32_field(fileattr, KB_FS_FILEATTR_XFLAGS_OFFSET, xflags);
    write_u8_field(
        fileattr,
        KB_FS_FILEATTR_SELECTORS_OFFSET,
        KB_FS_FILEATTR_FLAGS_VALID);
}

int kb_fs_subsystem_fiemap_fill_next_extent(
    void *extent_info,
    uint64_t logical,
    uint64_t physical,
    uint64_t length,
    uint32_t flags)
{
    enum {
        KB_FS_FIEMAP_INFO_MAPPED_OFFSET = 4,
        KB_FS_FIEMAP_INFO_MAX_OFFSET = 8,
        KB_FS_FIEMAP_INFO_START_OFFSET = 16,
        KB_FS_FIEMAP_EXTENT_BYTES = 56,
        KB_FS_FIEMAP_EXTENT_FLAGS_OFFSET = 40,
        KB_FS_FIEMAP_EXTENT_LAST = 0x00000001u,
        KB_FS_FIEMAP_EXTENT_UNKNOWN = 0x00000002u,
        KB_FS_FIEMAP_EXTENT_DELALLOC = 0x00000004u,
        KB_FS_FIEMAP_EXTENT_ENCODED = 0x00000008u,
        KB_FS_FIEMAP_EXTENT_DATA_ENCRYPTED = 0x00000080u,
        KB_FS_FIEMAP_EXTENT_NOT_ALIGNED = 0x00000100u,
        KB_FS_FIEMAP_EXTENT_DATA_INLINE = 0x00000200u,
        KB_FS_FIEMAP_EXTENT_DATA_TAIL = 0x00000400u,
    };
    if (extent_info == NULL) {
        return -22;
    }
    uint32_t mapped = read_u32_field(
        extent_info,
        KB_FS_FIEMAP_INFO_MAPPED_OFFSET);
    const uint32_t max = read_u32_field(
        extent_info,
        KB_FS_FIEMAP_INFO_MAX_OFFSET);
    if (max == 0) {
        write_u32_field(
            extent_info,
            KB_FS_FIEMAP_INFO_MAPPED_OFFSET,
            mapped + 1u);
        return (flags & KB_FS_FIEMAP_EXTENT_LAST) != 0 ? 1 : 0;
    }
    if (mapped >= max) {
        return 1;
    }
    void *start = read_pointer_field(
        extent_info,
        KB_FS_FIEMAP_INFO_START_OFFSET);
    if (start == NULL) {
        return -14;
    }
    if ((flags & KB_FS_FIEMAP_EXTENT_DELALLOC) != 0) {
        flags |= KB_FS_FIEMAP_EXTENT_UNKNOWN;
    }
    if ((flags & KB_FS_FIEMAP_EXTENT_DATA_ENCRYPTED) != 0) {
        flags |= KB_FS_FIEMAP_EXTENT_ENCODED;
    }
    if ((flags & (KB_FS_FIEMAP_EXTENT_DATA_TAIL |
                  KB_FS_FIEMAP_EXTENT_DATA_INLINE)) != 0)
    {
        flags |= KB_FS_FIEMAP_EXTENT_NOT_ALIGNED;
    }
    uint8_t extent[KB_FS_FIEMAP_EXTENT_BYTES];
    memset(extent, 0, sizeof(extent));
    write_u64_field(extent, 0, logical);
    write_u64_field(extent, 8, physical);
    write_u64_field(extent, 16, length);
    write_u32_field(extent, KB_FS_FIEMAP_EXTENT_FLAGS_OFFSET, flags);
    memcpy(
        (uint8_t *)start + (size_t)mapped * KB_FS_FIEMAP_EXTENT_BYTES,
        extent,
        sizeof(extent));
    ++mapped;
    write_u32_field(
        extent_info,
        KB_FS_FIEMAP_INFO_MAPPED_OFFSET,
        mapped);
    return mapped == max || (flags & KB_FS_FIEMAP_EXTENT_LAST) != 0 ? 1 : 0;
}

int kb_fs_subsystem_fiemap_prep(
    void *inode,
    void *extent_info,
    uint64_t start,
    uint64_t *length,
    uint32_t supported_flags)
{
    enum {
        KB_FS_FIEMAP_INFO_FLAGS_OFFSET = 0,
        KB_FS_FIEMAP_FLAG_SYNC = 0x00000001u,
        KB_FS_FIEMAP_FLAGS_COMPAT = 0x00000003u,
    };
    if (low_or_err_pointer(inode) || extent_info == NULL || length == NULL) {
        return -22;
    }
    if (*length == 0) {
        return -22;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    if (super_block == NULL) {
        return -22;
    }
    int64_t signed_maxbytes = 0;
    memcpy(
        &signed_maxbytes,
        (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_MAXBYTES_OFFSET,
        sizeof(signed_maxbytes));
    if (signed_maxbytes <= 0 || start >= (uint64_t)signed_maxbytes) {
        return -27;
    }
    const uint64_t maxbytes = (uint64_t)signed_maxbytes;
    if (*length > maxbytes || maxbytes - *length < start) {
        *length = maxbytes - start;
    }
    supported_flags |= KB_FS_FIEMAP_FLAG_SYNC;
    supported_flags &= KB_FS_FIEMAP_FLAGS_COMPAT;
    const uint32_t requested_flags = read_u32_field(
        extent_info,
        KB_FS_FIEMAP_INFO_FLAGS_OFFSET);
    const uint32_t incompatible_flags = requested_flags & ~supported_flags;
    if (incompatible_flags != 0) {
        write_u32_field(
            extent_info,
            KB_FS_FIEMAP_INFO_FLAGS_OFFSET,
            incompatible_flags);
        return -53;
    }
    if ((requested_flags & KB_FS_FIEMAP_FLAG_SYNC) == 0) {
        return 0;
    }
    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    return kb_fs_subsystem_filemap_write_and_wait_range(
        mapping,
        0,
        INT64_MAX);
}

static int kb_fs_iomap_to_fiemap(
    void *extent_info,
    const uint8_t *iomap,
    uint32_t flags)
{
    enum {
        KB_FS_IOMAP_ADDR_OFFSET = 0,
        KB_FS_IOMAP_FILE_OFFSET = 8,
        KB_FS_IOMAP_LENGTH_OFFSET = 16,
        KB_FS_IOMAP_TYPE_OFFSET = 24,
        KB_FS_IOMAP_FLAGS_OFFSET = 26,
        KB_FS_IOMAP_HOLE = 0,
        KB_FS_IOMAP_DELALLOC = 1,
        KB_FS_IOMAP_MAPPED = 2,
        KB_FS_IOMAP_UNWRITTEN = 3,
        KB_FS_IOMAP_INLINE = 4,
        KB_FS_IOMAP_F_SHARED = 1u << 2,
        KB_FS_IOMAP_F_MERGED = 1u << 3,
        KB_FS_FIEMAP_EXTENT_DELALLOC = 0x00000004u,
        KB_FS_FIEMAP_EXTENT_UNKNOWN = 0x00000002u,
        KB_FS_FIEMAP_EXTENT_DATA_INLINE = 0x00000200u,
        KB_FS_FIEMAP_EXTENT_UNWRITTEN = 0x00000800u,
        KB_FS_FIEMAP_EXTENT_MERGED = 0x00001000u,
        KB_FS_FIEMAP_EXTENT_SHARED = 0x00002000u,
    };
    const uint16_t type = (uint16_t)read_u32_field(
        iomap,
        KB_FS_IOMAP_TYPE_OFFSET);
    const uint16_t iomap_flags = (uint16_t)read_u32_field(
        iomap,
        KB_FS_IOMAP_FLAGS_OFFSET);
    switch (type) {
    case KB_FS_IOMAP_HOLE:
        return 0;
    case KB_FS_IOMAP_DELALLOC:
        flags |= KB_FS_FIEMAP_EXTENT_DELALLOC |
            KB_FS_FIEMAP_EXTENT_UNKNOWN;
        break;
    case KB_FS_IOMAP_MAPPED:
        break;
    case KB_FS_IOMAP_UNWRITTEN:
        flags |= KB_FS_FIEMAP_EXTENT_UNWRITTEN;
        break;
    case KB_FS_IOMAP_INLINE:
        flags |= KB_FS_FIEMAP_EXTENT_DATA_INLINE;
        break;
    default:
        return -5;
    }
    if ((iomap_flags & KB_FS_IOMAP_F_MERGED) != 0) {
        flags |= KB_FS_FIEMAP_EXTENT_MERGED;
    }
    if ((iomap_flags & KB_FS_IOMAP_F_SHARED) != 0) {
        flags |= KB_FS_FIEMAP_EXTENT_SHARED;
    }
    const uint64_t address = read_u64_field(iomap, KB_FS_IOMAP_ADDR_OFFSET);
    return kb_fs_subsystem_fiemap_fill_next_extent(
        extent_info,
        read_u64_field(iomap, KB_FS_IOMAP_FILE_OFFSET),
        address == UINT64_MAX ? 0 : address,
        read_u64_field(iomap, KB_FS_IOMAP_LENGTH_OFFSET),
        flags);
}

int kb_fs_subsystem_iomap_fiemap(
    void *inode,
    void *extent_info,
    uint64_t start,
    uint64_t length,
    const void *iomap_ops)
{
    enum {
        KB_FS_IOMAP_BYTES = 80,
        KB_FS_IOMAP_FILE_OFFSET = 8,
        KB_FS_IOMAP_LENGTH_OFFSET = 16,
        KB_FS_IOMAP_TYPE_OFFSET = 24,
        KB_FS_IOMAP_HOLE = 0,
        KB_FS_IOMAP_REPORT = 1u << 2,
        KB_FS_FIEMAP_EXTENT_LAST = 0x00000001u,
    };
    if (low_or_err_pointer(inode) || extent_info == NULL || iomap_ops == NULL) {
        return -22;
    }
    void *begin_operation = read_pointer_field(iomap_ops, 0);
    void *end_operation = read_pointer_field(iomap_ops, sizeof(void *));
    if (begin_operation == NULL) {
        return -95;
    }
    int status = kb_fs_subsystem_fiemap_prep(
        inode,
        extent_info,
        start,
        &length,
        0);
    if (status != 0) {
        return status;
    }

    int (*begin_fn)(void *, int64_t, int64_t, unsigned int, void *, void *) = NULL;
    int (*end_fn)(void *, int64_t, int64_t, int64_t, unsigned int, void *) = NULL;
    memcpy(&begin_fn, &begin_operation, sizeof(begin_fn));
    if (end_operation != NULL) {
        memcpy(&end_fn, &end_operation, sizeof(end_fn));
    }
    uint8_t previous[KB_FS_IOMAP_BYTES] = {0};
    uint64_t position = start;
    int iteration_status = 0;
    while (length != 0) {
        uint8_t iomap[KB_FS_IOMAP_BYTES] = {0};
        uint8_t source_iomap[KB_FS_IOMAP_BYTES] = {0};
        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call(begin_operation, &old_gs);
        iteration_status = begin_fn(
            inode,
            (int64_t)position,
            (int64_t)(length > INT64_MAX ? INT64_MAX : length),
            KB_FS_IOMAP_REPORT,
            iomap,
            source_iomap);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (iteration_status < 0) {
            break;
        }

        const uint64_t map_offset = read_u64_field(
            iomap,
            KB_FS_IOMAP_FILE_OFFSET);
        const uint64_t map_length = read_u64_field(
            iomap,
            KB_FS_IOMAP_LENGTH_OFFSET);
        if (map_offset > position || map_length == 0 ||
            map_offset > UINT64_MAX - map_length ||
            map_offset + map_length <= position)
        {
            iteration_status = -5;
            break;
        }
        uint64_t processed = map_offset + map_length - position;
        if (processed > length) {
            processed = length;
        }
        const uint16_t source_type = (uint16_t)read_u32_field(
            source_iomap,
            KB_FS_IOMAP_TYPE_OFFSET);
        if (source_type != KB_FS_IOMAP_HOLE) {
            const uint64_t source_offset = read_u64_field(
                source_iomap,
                KB_FS_IOMAP_FILE_OFFSET);
            const uint64_t source_length = read_u64_field(
                source_iomap,
                KB_FS_IOMAP_LENGTH_OFFSET);
            if (source_offset > position || source_length == 0 ||
                source_offset > UINT64_MAX - source_length ||
                source_offset + source_length <= position)
            {
                iteration_status = -5;
                break;
            }
            const uint64_t source_processed =
                source_offset + source_length - position;
            if (processed > source_processed) {
                processed = source_processed;
            }
        }
        const uint64_t iteration_length = processed;

        const uint16_t type = (uint16_t)read_u32_field(
            iomap,
            KB_FS_IOMAP_TYPE_OFFSET);
        if (type != KB_FS_IOMAP_HOLE) {
            iteration_status = kb_fs_iomap_to_fiemap(
                extent_info,
                previous,
                0);
            memcpy(previous, iomap, sizeof(previous));
            if (iteration_status != 0) {
                processed = 0;
            }
        }

        if (end_fn != NULL) {
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call(end_operation, &old_gs);
            const int end_status = end_fn(
                inode,
                (int64_t)position,
                (int64_t)iteration_length,
                (int64_t)processed,
                KB_FS_IOMAP_REPORT,
                iomap);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (end_status < 0 && processed == 0) {
                iteration_status = end_status;
            }
        }
        if (iteration_status < 0 || processed == 0) {
            break;
        }
        position += processed;
        length -= processed;
        iteration_status = 0;
    }

    if ((uint16_t)read_u32_field(previous, KB_FS_IOMAP_TYPE_OFFSET) !=
        KB_FS_IOMAP_HOLE)
    {
        const int final_status = kb_fs_iomap_to_fiemap(
            extent_info,
            previous,
            KB_FS_FIEMAP_EXTENT_LAST);
        if (final_status < 0) {
            return final_status;
        }
    }
    return iteration_status < 0 && iteration_status != -2 ?
        iteration_status : 0;
}

int64_t kb_fs_subsystem_mapping_seek_hole_data(
    void *mapping,
    int64_t start,
    int64_t end,
    int whence)
{
    enum {
        KB_FS_SEEK_DATA = 3,
        KB_FS_SEEK_HOLE = 4,
    };
    if (mapping == NULL || start < 0 || end <= start ||
        (whence != KB_FS_SEEK_DATA && whence != KB_FS_SEEK_HOLE))
    {
        return -6;
    }
    const int seek_data = whence == KB_FS_SEEK_DATA;
    void *inode = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    const uint8_t block_bits = read_u8_field(inode, KB_FS_INODE_BLKBITS_OFFSET);
    if (block_bits >= 63) {
        return -5;
    }
    const uint64_t block_size = 1ull << block_bits;
    if (block_size == 0 || block_size > KB_FS_PAGE_SIZE ||
        (KB_FS_PAGE_SIZE % block_size) != 0)
    {
        return -5;
    }

    void *a_ops = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    void *partial_operation = a_ops == NULL ? NULL : read_pointer_field(
        a_ops,
        KB_FS_ADDRESS_SPACE_OP_IS_PARTIALLY_UPTODATE_OFFSET);
    int (*partial_fn)(void *, size_t, size_t) = NULL;
    if (partial_operation != NULL) {
        memcpy(&partial_fn, &partial_operation, sizeof(partial_fn));
    }

    uint64_t position = (uint64_t)start;
    const uint64_t limit = (uint64_t)end;
    while (position < limit) {
        const unsigned long wanted_index =
            (unsigned long)(position / KB_FS_PAGE_SIZE);
        kb_fs_filemap_folio_record_t *next = NULL;
        for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
            kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
            if (!record->active || record->mapping != mapping ||
                record->folio == NULL || record->index < wanted_index)
            {
                continue;
            }
            if (next == NULL || record->index < next->index) {
                next = record;
            }
        }
        if (next == NULL) {
            return seek_data ? -6 : (int64_t)position;
        }
        uint64_t folio_start = 0;
        if (__builtin_mul_overflow(
                (uint64_t)next->index,
                (uint64_t)KB_FS_PAGE_SIZE,
                &folio_start))
        {
            return seek_data ? -6 : (int64_t)position;
        }
        if (position < folio_start) {
            if (!seek_data) {
                return (int64_t)position;
            }
            position = folio_start;
            if (position >= limit) {
                return -6;
            }
        }
        const uint64_t folio_end = folio_start > UINT64_MAX - KB_FS_PAGE_SIZE ?
            UINT64_MAX : folio_start + KB_FS_PAGE_SIZE;
        uint64_t flags = 0;
        memcpy(&flags, next->folio, sizeof(flags));
        if ((flags & KB_FS_FOLIO_FLAG_UPTODATE) != 0) {
            if (seek_data) {
                return (int64_t)position;
            }
            position = folio_end;
            continue;
        }
        if (partial_fn == NULL) {
            if (!seek_data) {
                return (int64_t)position;
            }
            position = folio_end;
            continue;
        }

        size_t offset = (size_t)(position - folio_start);
        offset &= ~((size_t)block_size - 1u);
        while (position < limit && position < folio_end) {
            unsigned long old_gs = 0;
            const int has_gs = kb_fs_enter_ext4_call(
                partial_operation,
                &old_gs);
            const int is_data = partial_fn(
                next->folio,
                offset,
                (size_t)block_size) != 0;
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (is_data == seek_data) {
                return (int64_t)position;
            }
            const uint64_t next_block =
                (position + block_size) & ~(block_size - 1u);
            if (next_block <= position) {
                return -5;
            }
            position = next_block;
            offset += (size_t)block_size;
        }
    }
    return seek_data ? -6 : end;
}

static int64_t kb_fs_subsystem_iomap_seek(
    void *inode,
    int64_t position,
    const void *iomap_ops,
    int seek_data)
{
    enum {
        KB_FS_IOMAP_BYTES = 80,
        KB_FS_IOMAP_FILE_OFFSET = 8,
        KB_FS_IOMAP_LENGTH_OFFSET = 16,
        KB_FS_IOMAP_TYPE_OFFSET = 24,
        KB_FS_IOMAP_HOLE = 0,
        KB_FS_IOMAP_DELALLOC = 1,
        KB_FS_IOMAP_MAPPED = 2,
        KB_FS_IOMAP_UNWRITTEN = 3,
        KB_FS_IOMAP_INLINE = 4,
        KB_FS_IOMAP_REPORT = 1u << 2,
        KB_FS_SEEK_DATA = 3,
        KB_FS_SEEK_HOLE = 4,
    };
    if (low_or_err_pointer(inode) || iomap_ops == NULL) {
        return -22;
    }
    int64_t size = 0;
    memcpy(
        &size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(size));
    if (position < 0 || position >= size) {
        return -6;
    }
    void *begin_operation = read_pointer_field(iomap_ops, 0);
    void *end_operation = read_pointer_field(iomap_ops, sizeof(void *));
    if (begin_operation == NULL) {
        return -95;
    }
    int (*begin_fn)(void *, int64_t, int64_t, unsigned int, void *, void *) = NULL;
    int (*end_fn)(void *, int64_t, int64_t, int64_t, unsigned int, void *) = NULL;
    memcpy(&begin_fn, &begin_operation, sizeof(begin_fn));
    if (end_operation != NULL) {
        memcpy(&end_fn, &end_operation, sizeof(end_fn));
    }
    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);

    while (position < size) {
        uint8_t iomap[KB_FS_IOMAP_BYTES] = {0};
        uint8_t source_iomap[KB_FS_IOMAP_BYTES] = {0};
        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call(begin_operation, &old_gs);
        int status = begin_fn(
            inode,
            position,
            size - position,
            KB_FS_IOMAP_REPORT,
            iomap,
            source_iomap);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status < 0) {
            return status;
        }

        const uint64_t map_offset = read_u64_field(
            iomap,
            KB_FS_IOMAP_FILE_OFFSET);
        const uint64_t map_length = read_u64_field(
            iomap,
            KB_FS_IOMAP_LENGTH_OFFSET);
        if (map_offset > (uint64_t)position || map_length == 0 ||
            map_offset > UINT64_MAX - map_length ||
            map_offset + map_length <= (uint64_t)position)
        {
            return -5;
        }
        uint64_t processed = map_offset + map_length - (uint64_t)position;
        const uint64_t remaining = (uint64_t)(size - position);
        if (processed > remaining) {
            processed = remaining;
        }
        const uint16_t source_type = (uint16_t)read_u32_field(
            source_iomap,
            KB_FS_IOMAP_TYPE_OFFSET);
        if (source_type != KB_FS_IOMAP_HOLE) {
            const uint64_t source_offset = read_u64_field(
                source_iomap,
                KB_FS_IOMAP_FILE_OFFSET);
            const uint64_t source_length = read_u64_field(
                source_iomap,
                KB_FS_IOMAP_LENGTH_OFFSET);
            if (source_offset > (uint64_t)position || source_length == 0 ||
                source_offset > UINT64_MAX - source_length ||
                source_offset + source_length <= (uint64_t)position)
            {
                return -5;
            }
            const uint64_t source_processed =
                source_offset + source_length - (uint64_t)position;
            if (processed > source_processed) {
                processed = source_processed;
            }
        }
        const uint64_t iteration_length = processed;
        const uint16_t type = (uint16_t)read_u32_field(
            iomap,
            KB_FS_IOMAP_TYPE_OFFSET);
        int found = 0;
        int64_t result = position;
        switch (type) {
        case KB_FS_IOMAP_HOLE:
            if (seek_data) {
                break;
            }
            found = 1;
            processed = 0;
            break;
        case KB_FS_IOMAP_UNWRITTEN:
            result = kb_fs_subsystem_mapping_seek_hole_data(
                mapping,
                position,
                position + (int64_t)processed,
                seek_data ? KB_FS_SEEK_DATA : KB_FS_SEEK_HOLE);
            if (seek_data) {
                if (result >= 0) {
                    found = 1;
                    processed = 0;
                } else if (result != -6) {
                    return result;
                }
            } else if (result != position + (int64_t)processed) {
                if (result < 0) {
                    return result;
                }
                found = 1;
                processed = 0;
            }
            break;
        case KB_FS_IOMAP_DELALLOC:
        case KB_FS_IOMAP_MAPPED:
        case KB_FS_IOMAP_INLINE:
            if (seek_data) {
                found = 1;
                processed = 0;
            }
            break;
        default:
            return -5;
        }

        if (end_fn != NULL) {
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call(end_operation, &old_gs);
            const int end_status = end_fn(
                inode,
                position,
                (int64_t)iteration_length,
                found ? 0 : (int64_t)processed,
                KB_FS_IOMAP_REPORT,
                iomap);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (end_status < 0 && (found || processed == 0)) {
                return end_status;
            }
        }
        if (found) {
            return result;
        }
        if (processed == 0 || processed > (uint64_t)INT64_MAX ||
            position > INT64_MAX - (int64_t)processed)
        {
            return -5;
        }
        position += (int64_t)processed;
    }
    return seek_data ? -6 : size;
}

int64_t kb_fs_subsystem_iomap_seek_data(
    void *inode,
    int64_t position,
    const void *iomap_ops)
{
    return kb_fs_subsystem_iomap_seek(inode, position, iomap_ops, 1);
}

int64_t kb_fs_subsystem_iomap_seek_hole(
    void *inode,
    int64_t position,
    const void *iomap_ops)
{
    return kb_fs_subsystem_iomap_seek(inode, position, iomap_ops, 0);
}

static int kb_fs_iomap_dio_invalidate_range(
    void *mapping,
    uint64_t start,
    uint64_t length)
{
    if (mapping == NULL || length == 0 || start > UINT64_MAX - length) {
        return -22;
    }
    const unsigned long first = (unsigned long)(start / KB_FS_PAGE_SIZE);
    const uint64_t last_byte = start + length - 1u;
    const unsigned long last = (unsigned long)(last_byte / KB_FS_PAGE_SIZE);
    (void)kb_fs_subsystem_invalidate_mapping_pages(mapping, first, last);
    for (size_t i = 0; i < KB_FS_FILEMAP_FOLIO_CACHE_MAX; ++i) {
        const kb_fs_filemap_folio_record_t *record = &filemap_folio_cache[i];
        if (record->active && record->mapping == mapping &&
            record->index >= first && record->index <= last)
        {
            return -15;
        }
    }
    return 0;
}

static int64_t kb_fs_iomap_dio_transfer(
    const uint8_t *iomap,
    uint64_t position,
    uint64_t length,
    void *iter,
    int write,
    int fua)
{
    enum {
        KB_FS_IOMAP_ADDR_OFFSET = 0,
        KB_FS_IOMAP_FILE_OFFSET = 8,
        KB_FS_IOMAP_BDEV_OFFSET = 32,
    };
    const uint64_t address = read_u64_field(iomap, KB_FS_IOMAP_ADDR_OFFSET);
    const uint64_t map_offset = read_u64_field(
        iomap,
        KB_FS_IOMAP_FILE_OFFSET);
    void *bdev = read_pointer_field(iomap, KB_FS_IOMAP_BDEV_OFFSET);
    kb_fs_block_device_t *device = block_device_for_bdev(bdev);
    if (address == UINT64_MAX || position < map_offset || device == NULL ||
        address > UINT64_MAX - (position - map_offset))
    {
        return -5;
    }
    const uint32_t logical_block_size = device->logical_block_size == 0 ?
        512u : device->logical_block_size;
    const uint64_t disk_offset = address + position - map_offset;
    void *iter_buffer = read_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET);
    if (logical_block_size == 0 ||
        (disk_offset % logical_block_size) != 0 ||
        (length % logical_block_size) != 0 ||
        ((uintptr_t)iter_buffer % logical_block_size) != 0)
    {
        return -22;
    }

    uint64_t transferred = 0;
    while (transferred < length) {
        size_t chunk = (size_t)(length - transferred);
        if (chunk > KB_FS_PAGE_SIZE) {
            chunk = KB_FS_PAGE_SIZE;
        }
        if ((chunk % logical_block_size) != 0) {
            return transferred == 0 ? -22 : (int64_t)transferred;
        }
        void *page = kb_kvm_alloc_pages_stub(0, 0);
        void *payload = page == NULL ? NULL : folio_page_payload(page);
        if (payload == NULL) {
            if (page != NULL) {
                kb_kvm_free_pages_stub(page, 0);
            }
            return transferred == 0 ? -12 : (int64_t)transferred;
        }
        if (write && kb_fs_subsystem_copy_from_iter(payload, chunk, iter) != chunk) {
            kb_kvm_free_pages_stub(page, 0);
            return transferred == 0 ? -14 : (int64_t)transferred;
        }

        unsigned int operation = write ? KB_FS_BIO_OP_WRITE : KB_FS_BIO_OP_READ;
        if (write && fua) {
            operation |= KB_FS_BIO_REQ_FUA;
        }
        void *bio = kb_fs_subsystem_bio_alloc_bioset(
            bdev,
            1,
            operation,
            0,
            NULL);
        if (bio == NULL ||
            kb_fs_subsystem_bio_add_page(bio, page, (unsigned int)chunk, 0) !=
                (int)chunk)
        {
            if (bio != NULL) {
                kb_fs_subsystem_bio_put(bio);
            }
            if (write) {
                kb_fs_subsystem_iov_iter_revert(iter, chunk);
            }
            kb_kvm_free_pages_stub(page, 0);
            return transferred == 0 ? -12 : (int64_t)transferred;
        }
        kb_fs_subsystem_bio_set_sector(
            bio,
            (disk_offset + transferred) >> 9);
        kb_fs_subsystem_submit_bio(bio);
        if (kb_fs_subsystem_bio_result(bio) == -115) {
            (void)kb_fs_subsystem_bio_drain();
        }
        const int status = kb_fs_subsystem_bio_result(bio);
        if (status == 0 && !write &&
            kb_fs_subsystem_copy_to_iter(payload, chunk, iter) != chunk)
        {
            kb_fs_subsystem_bio_put(bio);
            kb_kvm_free_pages_stub(page, 0);
            return transferred == 0 ? -14 : (int64_t)transferred;
        }
        kb_fs_subsystem_bio_put(bio);
        kb_kvm_free_pages_stub(page, 0);
        if (status != 0) {
            if (write) {
                kb_fs_subsystem_iov_iter_revert(iter, chunk);
            }
            return transferred == 0 ? status : (int64_t)transferred;
        }
        transferred += chunk;
    }
    return (int64_t)transferred;
}

long kb_fs_subsystem_iomap_dio_rw(
    void *kiocb,
    void *iter,
    const void *iomap_ops,
    const void *dio_ops,
    unsigned int dio_flags,
    void *private_data,
    size_t done_before)
{
    enum {
        KB_FS_IOMAP_BYTES = 80,
        KB_FS_IOMAP_ADDR_OFFSET = 0,
        KB_FS_IOMAP_FILE_OFFSET = 8,
        KB_FS_IOMAP_LENGTH_OFFSET = 16,
        KB_FS_IOMAP_TYPE_OFFSET = 24,
        KB_FS_IOMAP_FLAGS_OFFSET = 26,
        KB_FS_IOMAP_INLINE_DATA_OFFSET = 48,
        KB_FS_IOMAP_HOLE = 0,
        KB_FS_IOMAP_DELALLOC = 1,
        KB_FS_IOMAP_MAPPED = 2,
        KB_FS_IOMAP_UNWRITTEN = 3,
        KB_FS_IOMAP_INLINE = 4,
        KB_FS_IOMAP_F_SHARED = 1u << 2,
        KB_FS_IOMAP_WRITE = 1u << 0,
        KB_FS_IOMAP_DIRECT = 1u << 4,
        KB_FS_IOMAP_NOWAIT = 1u << 5,
        KB_FS_IOMAP_OVERWRITE_ONLY = 1u << 6,
        KB_FS_IOMAP_DIO_FORCE_WAIT = 1u << 0,
        KB_FS_IOMAP_DIO_OVERWRITE_ONLY = 1u << 1,
        KB_FS_IOMAP_DIO_UNWRITTEN = 1u << 0,
        KB_FS_IOMAP_DIO_COW = 1u << 1,
        KB_FS_IOCB_DSYNC = 1u << 1,
        KB_FS_IOCB_SYNC = 1u << 2,
        KB_FS_IOCB_NOWAIT = 1u << 3,
    };
    (void)private_data;
    (void)KB_FS_IOMAP_DIO_FORCE_WAIT;
    if (kiocb == NULL || iter == NULL || iomap_ops == NULL) {
        return -22;
    }
    void *file = read_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET);
    void *inode = file == NULL ? NULL :
        read_pointer_field(file, KB_FS_FILE_INODE_OFFSET);
    void *mapping = file == NULL ? NULL :
        read_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET);
    void *begin_operation = read_pointer_field(iomap_ops, 0);
    void *end_operation = read_pointer_field(iomap_ops, sizeof(void *));
    if (low_or_err_pointer(inode) || mapping == NULL || begin_operation == NULL) {
        return -22;
    }
    const uint32_t ki_flags = read_u32_field(kiocb, KB_FS_KIOCB_FLAGS_OFFSET);
    if ((ki_flags & KB_FS_IOCB_NOWAIT) != 0) {
        return -11;
    }
    uint64_t count = read_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET);
    int64_t position = 0;
    int64_t file_size = 0;
    memcpy(
        &position,
        (const uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET,
        sizeof(position));
    memcpy(
        &file_size,
        (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET,
        sizeof(file_size));
    if (position < 0 || file_size < 0 || count > INT64_MAX ||
        count > (uint64_t)INT64_MAX - (uint64_t)position)
    {
        return -22;
    }
    if (count == 0) {
        return 0;
    }
    const int write = read_u8_field(
        iter,
        KB_FS_IOV_ITER_DATA_SOURCE_OFFSET) != 0;
    uint64_t readable_count = count;
    if (!write) {
        if (position >= file_size) {
            return 0;
        }
        readable_count = (uint64_t)(file_size - position);
        if (readable_count > count) {
            readable_count = count;
        }
    } else if ((dio_flags & KB_FS_IOMAP_DIO_OVERWRITE_ONLY) != 0 &&
        ((uint64_t)position >= (uint64_t)file_size ||
         count > (uint64_t)file_size - (uint64_t)position))
    {
        return -11;
    }

    const uint64_t range_end = (uint64_t)position + count - 1u;
    int status = kb_fs_subsystem_filemap_write_and_wait_range(
        mapping,
        position,
        range_end > INT64_MAX ? INT64_MAX : (int64_t)range_end);
    if (status != 0) {
        return status;
    }
    if (write) {
        status = kb_fs_iomap_dio_invalidate_range(
            mapping,
            (uint64_t)position,
            count);
        if (status != 0) {
            return status;
        }
    }

    int (*begin_fn)(void *, int64_t, int64_t, unsigned int, void *, void *) = NULL;
    int (*end_fn)(void *, int64_t, int64_t, int64_t, unsigned int, void *) = NULL;
    memcpy(&begin_fn, &begin_operation, sizeof(begin_fn));
    if (end_operation != NULL) {
        memcpy(&end_fn, &end_operation, sizeof(end_fn));
    }
    void *dio_end_operation = dio_ops == NULL ? NULL :
        read_pointer_field(dio_ops, 0);
    void *dio_submit_operation = dio_ops == NULL ? NULL :
        read_pointer_field(dio_ops, sizeof(void *));
    if (dio_submit_operation != NULL) {
        return -95;
    }

    uint32_t *dio_count = (uint32_t *)((uint8_t *)inode +
        KB_FS_INODE_DIO_COUNT_OFFSET);
    __atomic_fetch_add(dio_count, 1u, __ATOMIC_ACQ_REL);
    const int64_t initial_position = position;
    uint64_t remaining = count;
    uint64_t transferred = 0;
    unsigned int completion_flags = 0;
    unsigned int iomap_flags = KB_FS_IOMAP_DIRECT;
    if (write) {
        iomap_flags |= KB_FS_IOMAP_WRITE;
    }
    if ((ki_flags & KB_FS_IOCB_NOWAIT) != 0) {
        iomap_flags |= KB_FS_IOMAP_NOWAIT;
    }
    if ((dio_flags & KB_FS_IOMAP_DIO_OVERWRITE_ONLY) != 0) {
        iomap_flags |= KB_FS_IOMAP_OVERWRITE_ONLY;
    }

    int error = 0;
    while (remaining != 0) {
        uint8_t iomap[KB_FS_IOMAP_BYTES] = {0};
        uint8_t source_iomap[KB_FS_IOMAP_BYTES] = {0};
        unsigned long old_gs = 0;
        int has_gs = kb_fs_enter_ext4_call(begin_operation, &old_gs);
        status = begin_fn(
            inode,
            position,
            remaining > INT64_MAX ? INT64_MAX : (int64_t)remaining,
            iomap_flags,
            iomap,
            source_iomap);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status < 0) {
            error = status;
            break;
        }
        const uint64_t map_offset = read_u64_field(
            iomap,
            KB_FS_IOMAP_FILE_OFFSET);
        const uint64_t map_length = read_u64_field(
            iomap,
            KB_FS_IOMAP_LENGTH_OFFSET);
        if (map_offset > (uint64_t)position || map_length == 0 ||
            map_offset > UINT64_MAX - map_length ||
            map_offset + map_length <= (uint64_t)position)
        {
            error = -5;
            break;
        }
        uint64_t iteration_length =
            map_offset + map_length - (uint64_t)position;
        if (iteration_length > remaining) {
            iteration_length = remaining;
        }
        const uint16_t source_type = (uint16_t)read_u32_field(
            source_iomap,
            KB_FS_IOMAP_TYPE_OFFSET);
        if (source_type != KB_FS_IOMAP_HOLE) {
            const uint64_t source_offset = read_u64_field(
                source_iomap,
                KB_FS_IOMAP_FILE_OFFSET);
            const uint64_t source_length = read_u64_field(
                source_iomap,
                KB_FS_IOMAP_LENGTH_OFFSET);
            if (source_offset > (uint64_t)position || source_length == 0 ||
                source_offset > UINT64_MAX - source_length ||
                source_offset + source_length <= (uint64_t)position)
            {
                error = -5;
                break;
            }
            const uint64_t source_iteration =
                source_offset + source_length - (uint64_t)position;
            if (iteration_length > source_iteration) {
                iteration_length = source_iteration;
            }
        }

        const uint16_t type = (uint16_t)read_u32_field(
            iomap,
            KB_FS_IOMAP_TYPE_OFFSET);
        const uint16_t map_flags = (uint16_t)read_u32_field(
            iomap,
            KB_FS_IOMAP_FLAGS_OFFSET);
        int64_t processed = 0;
        if ((map_flags & KB_FS_IOMAP_F_SHARED) != 0) {
            completion_flags |= KB_FS_IOMAP_DIO_COW;
        }
        switch (type) {
        case KB_FS_IOMAP_HOLE:
            if (write) {
                processed = -5;
                break;
            }
            while ((uint64_t)processed < iteration_length) {
                uint8_t zeros[KB_FS_PAGE_SIZE] = {0};
                size_t chunk = (size_t)(iteration_length - (uint64_t)processed);
                if (chunk > sizeof(zeros)) {
                    chunk = sizeof(zeros);
                }
                if (kb_fs_subsystem_copy_to_iter(zeros, chunk, iter) != chunk) {
                    processed = processed == 0 ? -14 : processed;
                    break;
                }
                processed += (int64_t)chunk;
            }
            break;
        case KB_FS_IOMAP_UNWRITTEN:
            if (!write) {
                while ((uint64_t)processed < iteration_length) {
                    uint8_t zeros[KB_FS_PAGE_SIZE] = {0};
                    size_t chunk = (size_t)(iteration_length - (uint64_t)processed);
                    if (chunk > sizeof(zeros)) {
                        chunk = sizeof(zeros);
                    }
                    if (kb_fs_subsystem_copy_to_iter(zeros, chunk, iter) != chunk) {
                        processed = processed == 0 ? -14 : processed;
                        break;
                    }
                    processed += (int64_t)chunk;
                }
                break;
            }
            completion_flags |= KB_FS_IOMAP_DIO_UNWRITTEN;
            /* fall through */
        case KB_FS_IOMAP_MAPPED:
            processed = kb_fs_iomap_dio_transfer(
                iomap,
                (uint64_t)position,
                iteration_length,
                iter,
                write,
                write && (ki_flags & KB_FS_IOCB_DSYNC) != 0);
            break;
        case KB_FS_IOMAP_INLINE: {
            void *inline_data = read_pointer_field(
                iomap,
                KB_FS_IOMAP_INLINE_DATA_OFFSET);
            const uint64_t inline_offset = (uint64_t)position - map_offset;
            if (inline_data == NULL || inline_offset > SIZE_MAX ||
                iteration_length > SIZE_MAX - (size_t)inline_offset)
            {
                processed = -5;
                break;
            }
            processed = write ? (int64_t)kb_fs_subsystem_copy_from_iter(
                (uint8_t *)inline_data + inline_offset,
                (size_t)iteration_length,
                iter) : (int64_t)kb_fs_subsystem_copy_to_iter(
                (const uint8_t *)inline_data + inline_offset,
                (size_t)iteration_length,
                iter);
            if (processed == 0) {
                processed = -14;
            }
            break;
        }
        case KB_FS_IOMAP_DELALLOC:
        default:
            processed = -5;
            break;
        }

        if (end_fn != NULL) {
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call(end_operation, &old_gs);
            const int end_status = end_fn(
                inode,
                position,
                (int64_t)iteration_length,
                processed > 0 ? processed : 0,
                iomap_flags,
                iomap);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (end_status < 0 && processed <= 0) {
                processed = end_status;
            }
        }
        if (processed <= 0) {
            error = processed == 0 ? -5 : processed;
            break;
        }
        position += processed;
        transferred += (uint64_t)processed;
        remaining -= (uint64_t)processed;
    }

    if (!write && transferred > readable_count) {
        kb_fs_subsystem_iov_iter_revert(
            iter,
            (size_t)(transferred - readable_count));
        transferred = readable_count;
    }
    long result = error;
    if (dio_end_operation != NULL) {
        int (*dio_end_fn)(void *, int64_t, int, unsigned int) = NULL;
        memcpy(&dio_end_fn, &dio_end_operation, sizeof(dio_end_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(dio_end_operation, &old_gs);
        result = dio_end_fn(
            kiocb,
            (int64_t)transferred,
            error,
            completion_flags);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    } else if (error == 0) {
        result = (long)transferred;
    }
    if (result == 0 && error == 0) {
        result = (long)transferred;
    }
    if (result > 0) {
        write_u64_field(
            kiocb,
            KB_FS_KIOCB_POS_OFFSET,
            (uint64_t)initial_position + transferred);
        if (write && (ki_flags & KB_FS_IOCB_DSYNC) != 0) {
            const int sync_status = kb_fs_subsystem_vfs_fsync_range(
                file,
                initial_position,
                initial_position + (int64_t)transferred - 1,
                (ki_flags & KB_FS_IOCB_SYNC) == 0);
            if (sync_status != 0) {
                result = sync_status;
            }
        }
        if (result > 0 && done_before <= (size_t)(LONG_MAX - result)) {
            result += (long)done_before;
        }
    }
    if (write && transferred != 0) {
        (void)kb_fs_iomap_dio_invalidate_range(
            mapping,
            (uint64_t)initial_position,
            transferred);
    }
    __atomic_fetch_sub(dio_count, 1u, __ATOMIC_ACQ_REL);
    return result;
}

void kb_fs_subsystem_inode_dio_wait(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    const uint32_t *dio_count = (const uint32_t *)((const uint8_t *)inode +
        KB_FS_INODE_DIO_COUNT_OFFSET);
    const uint32_t initial_count = __atomic_load_n(
        dio_count, __ATOMIC_ACQUIRE);
    if (initial_count != 0) {
        fprintf(stderr,
            "kobox fs: inode_dio_wait pending inode=%p count=%u\n",
            inode,
            initial_count);
    }
    while (__atomic_load_n(dio_count, __ATOMIC_ACQUIRE) != 0) {
        (void)kb_fs_subsystem_bio_drain();
        kb_run_deferred_work();
    }
}

int kb_fs_subsystem_generic_error_remove_folio(void *mapping, void *folio)
{
    if (mapping == NULL || folio == NULL) {
        return -22;
    }
    void *inode = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    const uint16_t mode = (uint16_t)read_u32_field(
        inode,
        KB_FS_INODE_MODE_OFFSET);
    if ((mode & KB_FS_MODE_TYPE_MASK) != 0100000u) {
        return -5;
    }
    const unsigned long index = (unsigned long)read_u64_field(
        folio,
        KB_FS_FOLIO_INDEX_OFFSET);
    kb_fs_subsystem_truncate_inode_pages_range(
        mapping,
        (int64_t)index * KB_FS_PAGE_SIZE,
        (int64_t)index * KB_FS_PAGE_SIZE + KB_FS_PAGE_SIZE - 1);
    return 0;
}

void kb_fs_subsystem_lock_two_nondirectories(void *inode1, void *inode2)
{
    if ((uintptr_t)inode1 > (uintptr_t)inode2) {
        void *swap = inode1;
        inode1 = inode2;
        inode2 = swap;
    }
    if (!low_or_err_pointer(inode1)) {
        kb_down_write((uint8_t *)inode1 + KB_FS_INODE_RWSEM_OFFSET);
    }
    if (!low_or_err_pointer(inode2) && inode2 != inode1) {
        kb_down_write((uint8_t *)inode2 + KB_FS_INODE_RWSEM_OFFSET);
    }
}

void kb_fs_subsystem_unlock_two_nondirectories(void *inode1, void *inode2)
{
    if (!low_or_err_pointer(inode1)) {
        kb_up_write((uint8_t *)inode1 + KB_FS_INODE_RWSEM_OFFSET);
    }
    if (!low_or_err_pointer(inode2) && inode2 != inode1) {
        kb_up_write((uint8_t *)inode2 + KB_FS_INODE_RWSEM_OFFSET);
    }
}

int kb_fs_subsystem_sync_inode_metadata(void *inode, int wait)
{
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    void *operation = super_inode_operation(
        super_block,
        KB_FS_SUPER_OP_WRITE_INODE_OFFSET);
    if (operation == NULL) {
        kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
        if (allocation != NULL) {
            allocation->dirty_metadata = 0;
        }
        uint64_t state = 0;
        memcpy(
            &state,
            (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET,
            sizeof(state));
        state &= ~(uint64_t)(KB_FS_INODE_STATE_DIRTY_SYNC |
                             KB_FS_INODE_STATE_DIRTY_DATASYNC |
                             KB_FS_INODE_STATE_DIRTY_TIME);
        write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, state);
        return 0;
    }
    uint8_t writeback_control[KB_FS_WRITEBACK_CONTROL_BYTES];
    memset(writeback_control, 0, sizeof(writeback_control));
    write_u32_field(
        writeback_control,
        KB_FS_WRITEBACK_CONTROL_SYNC_MODE_OFFSET,
        wait ? KB_FS_WRITEBACK_CONTROL_WB_SYNC_ALL : 0u);
    int (*write_inode_fn)(void *, void *) = NULL;
    memcpy(&write_inode_fn, &operation, sizeof(write_inode_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    const int status = write_inode_fn(inode, writeback_control);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (status == 0) {
        kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
        if (allocation != NULL) {
            allocation->dirty_metadata = 0;
        }
        uint64_t state = 0;
        memcpy(
            &state,
            (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET,
            sizeof(state));
        state &= ~(uint64_t)(KB_FS_INODE_STATE_DIRTY_SYNC |
                             KB_FS_INODE_STATE_DIRTY_DATASYNC |
                             KB_FS_INODE_STATE_DIRTY_TIME);
        write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, state);
    }
    return status;
}

static int writeback_inodes_for_super(void *super_block, int wait)
{
    if (super_block == NULL) {
        return -22;
    }
    int first_error = 0;
    for (size_t i = 0; i < KB_FS_INODE_ALLOCATION_MAX; ++i) {
        kb_fs_inode_allocation_record_t *allocation = &inode_allocations[i];
        if (!allocation->active || allocation->inode == NULL ||
            read_pointer_field(allocation->inode, KB_FS_INODE_SB_OFFSET) != super_block)
        {
            continue;
        }
        const int data_status = allocation->mapping == NULL ? 0 :
            filemap_writeback_range(allocation->mapping, 0, INT64_MAX);
        if (first_error == 0 && data_status != 0) {
            first_error = data_status;
        }
        if (allocation->dirty_metadata) {
            const int metadata_status =
                kb_fs_subsystem_sync_inode_metadata(allocation->inode, wait);
            if (first_error == 0 && metadata_status != 0) {
                first_error = metadata_status;
            }
        }
    }
    if (wait) {
        (void)kb_fs_subsystem_bio_drain();
    }
    return first_error;
}

void kb_fs_subsystem_try_to_writeback_inodes_sb(
    void *super_block,
    unsigned int reason)
{
    (void)reason;
    (void)writeback_inodes_for_super(super_block, 0);
}

int kb_fs_subsystem_sync_mapping_buffers(void *mapping)
{
    if (mapping == NULL) {
        return -22;
    }
    return kb_fs_subsystem_flush_dirty_buffers();
}

int kb_fs_subsystem_sync_filesystem(void *super_block)
{
    if (super_block == NULL) {
        return -22;
    }
    int status = writeback_inodes_for_super(super_block, 0);
    if (status != 0) {
        return status;
    }
    status = kb_fs_subsystem_sync_super(super_block, 0);
    if (status != 0) {
        return status;
    }
    status = kb_fs_subsystem_flush_dirty_buffers();
    if (status != 0) {
        return status;
    }
    status = writeback_inodes_for_super(super_block, 1);
    if (status != 0) {
        return status;
    }
    status = kb_fs_subsystem_sync_super(super_block, 1);
    if (status != 0) {
        return status;
    }
    return kb_fs_subsystem_sync_blockdev(
        read_pointer_field(super_block, KB_FS_SUPER_BLOCK_BDEV_OFFSET));
}

static void *allocate_native_inode(void *super_block, int *out_supported)
{
    if (out_supported != NULL) {
        *out_supported = 0;
    }
    void *operation = super_inode_operation(
        super_block,
        KB_FS_SUPER_OP_ALLOC_INODE_OFFSET);
    if (operation == NULL) {
        return NULL;
    }
    if (out_supported != NULL) {
        *out_supported = 1;
    }
    void *(*allocate_fn)(void *) = NULL;
    memcpy(&allocate_fn, &operation, sizeof(allocate_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    void *inode = allocate_fn(super_block);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return inode;
}

static void release_native_inode(void *super_block, void *inode, int destroy)
{
    if (super_block == NULL || inode == NULL) {
        return;
    }
    if (destroy) {
        void *destroy_operation = super_inode_operation(
            super_block,
            KB_FS_SUPER_OP_DESTROY_INODE_OFFSET);
        if (destroy_operation != NULL) {
            void (*destroy_fn)(void *) = NULL;
            memcpy(&destroy_fn, &destroy_operation, sizeof(destroy_fn));
            const unsigned long kernel_gs =
                kb_module_kernel_gs_for_address(destroy_operation);
            if (kernel_gs != 0) {
                kb_linux_call_void_ptr_gs(destroy_fn, inode, kernel_gs);
            } else {
                kb_linux_call_void_ptr(destroy_fn, inode);
            }
        }
    }
    void *free_operation = super_inode_operation(
        super_block,
        KB_FS_SUPER_OP_FREE_INODE_OFFSET);
    if (free_operation == NULL) {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=vfs_free_inode status=-5 inode=%p\n",
            inode);
        return;
    }
    void (*free_fn)(void *) = NULL;
    memcpy(&free_fn, &free_operation, sizeof(free_fn));
    const unsigned long kernel_gs =
        kb_module_kernel_gs_for_address(free_operation);
    if (kernel_gs != 0) {
        kb_linux_call_void_ptr_gs(free_fn, inode, kernel_gs);
    } else {
        kb_linux_call_void_ptr(free_fn, inode);
    }
}

void *kb_fs_subsystem_iget_locked(void *super_block, unsigned long inode_number)
{
    if (super_block == NULL) {
        return NULL;
    }
    if (inode_number != 0) {
        for (size_t i = 0; i < KB_FS_INODE_ALLOCATION_MAX; i++) {
            void *cached = inode_allocations[i].active && inode_allocations[i].hashed ?
                inode_allocations[i].inode : NULL;
            if (cached == NULL ||
                read_pointer_field(cached, KB_FS_INODE_SB_OFFSET) != super_block)
            {
                continue;
            }
            uint64_t cached_number = 0;
            uint32_t references = 0;
            memcpy(&cached_number,
                (const uint8_t *)cached + KB_FS_INODE_NUMBER_OFFSET,
                sizeof(cached_number));
            memcpy(&references,
                (const uint8_t *)cached + KB_FS_INODE_COUNT_OFFSET,
                sizeof(references));
            if (cached_number == inode_number && references != 0 && references != UINT32_MAX) {
                write_u32_field(cached, KB_FS_INODE_COUNT_OFFSET, references + 1u);
                return cached;
            }
        }
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: iget_locked inode=%lu\n", inode_number);
    }
    void *storage = NULL;
    void *mapping = NULL;
    int native = 0;
    void *inode = allocate_native_inode(super_block, &native);
    if (native && inode == NULL) {
        return NULL;
    }
    if (native) {
        /* Linux embeds struct address_space as inode::i_data and initializes
         * i_mapping to that object.  ext4_evict_inode() invalidates
         * &inode->i_data directly.  A separately allocated mapping therefore
         * survives native eviction in Kobox and can be reused at the same
         * address while stale UPTODATE folios are still keyed to it, causing
         * cross-inode reads after package-sized churn. */
        mapping = (uint8_t *)inode + KB_FS_INODE_DATA_OFFSET;
    } else {
        storage = calloc(
            1,
            KB_FS_FAKE_INODE_HEADROOM_BYTES +
                KB_FS_FAKE_INODE_BYTES +
                KB_FS_FAKE_INODE_MAPPING_BYTES);
        if (storage == NULL) {
            return NULL;
        }
        inode = (uint8_t *)storage + KB_FS_FAKE_INODE_HEADROOM_BYTES;
        mapping = (uint8_t *)inode + KB_FS_FAKE_INODE_BYTES;
    }
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_claim();
    if (allocation == NULL) {
        if (native) {
            release_native_inode(super_block, inode, 0);
        } else {
            free(storage);
        }
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=vfs_inode_registry status=-12 inode=%lu\n",
            inode_number);
        return NULL;
    }
    allocation->native = native;
    allocation->hashed = inode_number != 0;
    allocation->inode = inode;
    allocation->mapping = mapping;
    allocation->storage = storage;
    write_pointer_field(inode, KB_FS_INODE_SB_OFFSET, super_block);
    write_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET, mapping);
    write_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET, inode);
    uint8_t block_bits = 0;
    memcpy(&block_bits, (const uint8_t *)super_block + KB_FS_SUPER_BLOCK_BLOCKSIZE_BITS_OFFSET, sizeof(block_bits));
    memcpy((uint8_t *)inode + KB_FS_INODE_BLKBITS_OFFSET, &block_bits, sizeof(block_bits));
    write_u64_field(inode, KB_FS_INODE_NUMBER_OFFSET, (uint64_t)inode_number);
    write_u64_field(
        inode,
        KB_FS_INODE_STATE_OFFSET,
        inode_number == 0 ? 0 : KB_FS_INODE_STATE_NEW);
    if (inode_number == 0) {
        write_u32_field(inode, KB_FS_INODE_NLINK_OFFSET, 1u);
    }
    write_u32_field(inode, KB_FS_INODE_COUNT_OFFSET, 1u);
    return inode;
}

void *kb_fs_subsystem_new_inode(void *super_block)
{
    return kb_fs_subsystem_iget_locked(super_block, 0);
}

int kb_fs_subsystem_insert_inode_locked(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return -22;
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    uint64_t inode_number = 0;
    memcpy(
        &inode_number,
        (const uint8_t *)inode + KB_FS_INODE_NUMBER_OFFSET,
        sizeof(inode_number));
    for (size_t i = 0; i < KB_FS_INODE_ALLOCATION_MAX; ++i) {
        void *old = inode_allocations[i].active && inode_allocations[i].hashed ?
            inode_allocations[i].inode : NULL;
        if (old == NULL || old == inode ||
            read_pointer_field(old, KB_FS_INODE_SB_OFFSET) != super_block)
        {
            continue;
        }
        uint64_t old_number = 0;
        uint64_t old_state = 0;
        memcpy(
            &old_number,
            (const uint8_t *)old + KB_FS_INODE_NUMBER_OFFSET,
            sizeof(old_number));
        memcpy(
            &old_state,
            (const uint8_t *)old + KB_FS_INODE_STATE_OFFSET,
            sizeof(old_state));
        if (old_number == inode_number &&
            (old_state & (KB_FS_INODE_STATE_FREEING |
                          KB_FS_INODE_STATE_WILL_FREE)) == 0)
        {
            return -16;
        }
    }
    uint64_t state = 0;
    memcpy(
        &state,
        (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET,
        sizeof(state));
    state |= KB_FS_INODE_STATE_NEW | KB_FS_INODE_STATE_CREATING;
    write_u64_field(inode, KB_FS_INODE_STATE_OFFSET, state);
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
    if (allocation != NULL) {
        allocation->hashed = 1;
    }
    return 0;
}

static void init_list_head_field(void *base, size_t offset)
{
    void *head = (uint8_t *)base + offset;
    write_pointer_field(base, offset, head);
    write_pointer_field(base, offset + sizeof(void *), head);
}

void kb_fs_subsystem_inode_init_once(void *inode)
{
    if (inode == NULL) {
        return;
    }

    /* This is the slab-constructor half of Linux 6.12 inode_init_once().
     * Kobox does not attach these inodes to global VFS caches, but the
     * filesystem modules still consume their intrusive list heads. */
    memset(inode, 0, KB_FS_INODE_BYTES);
    init_list_head_field(inode, KB_FS_INODE_DEVICES_OFFSET);
    init_list_head_field(inode, KB_FS_INODE_IO_LIST_OFFSET);
    init_list_head_field(inode, KB_FS_INODE_WB_LIST_OFFSET);
    init_list_head_field(inode, KB_FS_INODE_LRU_OFFSET);
    init_list_head_field(inode, KB_FS_INODE_SB_LIST_OFFSET);
    init_list_head_field(
        inode,
        KB_FS_INODE_DATA_OFFSET + KB_FS_ADDRESS_SPACE_PRIVATE_LIST_OFFSET);

    /* INIT_HLIST_NODE(i_hash) is represented by the zeroed pair. */
    write_pointer_field(inode, KB_FS_INODE_HASH_OFFSET, NULL);
    write_pointer_field(inode, KB_FS_INODE_HASH_OFFSET + sizeof(void *), NULL);
}

void kb_fs_subsystem_free_fake_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
    if (allocation == NULL) {
        free((uint8_t *)inode - KB_FS_FAKE_INODE_HEADROOM_BYTES);
        return;
    }
    const int native = allocation->native;
    void *storage = allocation->storage;
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    void *access_acl = allocation->acl_access;
    void *default_acl = allocation->acl_default;
    memset(allocation, 0, sizeof(*allocation));
    kb_fs_subsystem_posix_acl_release(access_acl);
    kb_fs_subsystem_posix_acl_release(default_acl);
    if (native) {
        release_native_inode(super_block, inode, 1);
    } else {
        free(storage);
    }
}

void kb_fs_subsystem_ihold(void *inode)
{
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
    if (allocation == NULL) {
        return;
    }
    uint32_t references = 0;
    memcpy(&references,
        (const uint8_t *)inode + KB_FS_INODE_COUNT_OFFSET,
        sizeof(references));
    if (references != 0 && references != UINT32_MAX) {
        write_u32_field(inode, KB_FS_INODE_COUNT_OFFSET, references + 1u);
    }
}

void *kb_fs_subsystem_igrab(void *inode)
{
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
    if (allocation == NULL) {
        return NULL;
    }
    uint32_t references = 0;
    memcpy(&references,
        (const uint8_t *)inode + KB_FS_INODE_COUNT_OFFSET,
        sizeof(references));
    if (references == 0 || references == UINT32_MAX) {
        return NULL;
    }
    write_u32_field(inode, KB_FS_INODE_COUNT_OFFSET, references + 1u);
    return inode;
}

void kb_fs_subsystem_iput(void *inode)
{
    kb_fs_inode_allocation_record_t *allocation = inode_allocation_find(inode);
    if (allocation == NULL) {
        return;
    }
    uint32_t references = 0;
    memcpy(&references,
        (const uint8_t *)inode + KB_FS_INODE_COUNT_OFFSET,
        sizeof(references));
    if (references == 0) {
        return;
    }
    references--;
    write_u32_field(inode, KB_FS_INODE_COUNT_OFFSET, references);
    if (references != 0) {
        return;
    }
    uint32_t nlink = 0;
    memcpy(&nlink,
        (const uint8_t *)inode + KB_FS_INODE_NLINK_OFFSET,
        sizeof(nlink));
    if (fs_trace_enabled()) {
        uint64_t inode_number = 0;
        memcpy(&inode_number,
            (const uint8_t *)inode + KB_FS_INODE_NUMBER_OFFSET,
            sizeof(inode_number));
        fprintf(stderr,
            "kobox-fs: iput final inode=%p number=%llu nlink=%u bad=%d\n",
            inode,
            (unsigned long long)inode_number,
            nlink,
            kb_fs_subsystem_is_bad_inode(inode));
    }

    /*
     * Kobox has no background inode LRU/writeback thread to retain a linked
     * zero-reference inode after iput_final().  Perform the same reclaim-time
     * data and metadata writeback before evicting it.  Unlinked inodes stay on
     * ext4's normal ->evict_inode path so orphan/truncate handling remains
     * owned by ext4 rather than being duplicated here.
     */
    if (nlink != 0) {
        int writeback_status = allocation->mapping == NULL ? 0 :
            filemap_writeback_range(allocation->mapping, 0, INT64_MAX);
        if (writeback_status == 0 && allocation->dirty_metadata) {
            writeback_status = kb_fs_subsystem_sync_inode_metadata(inode, 1);
        }
        if (writeback_status != 0) {
            if (allocation->mapping != NULL) {
                kb_fs_subsystem_filemap_set_wb_err(
                    allocation->mapping, writeback_status);
            }
            fprintf(stderr,
                "FILED_STORAGE_FAULT layer=vfs_iput_writeback status=%d inode=%p\n",
                writeback_status,
                inode);
        }
    }

    /* Linux iput_final() moves a final-reference inode into I_FREEING before
     * ->evict_inode().  Filesystems such as ext4 assert that lifecycle
     * contract; callers must not have to manufacture the state transition. */
    kb_fs_subsystem_mark_inode_freeing(inode);
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    void *evict_operation = super_inode_operation(
        super_block,
        KB_FS_SUPER_OP_EVICT_INODE_OFFSET);
    if (evict_operation != NULL) {
        void (*evict_fn)(void *) = NULL;
        memcpy(&evict_fn, &evict_operation, sizeof(evict_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(evict_operation, &old_gs);
        evict_fn(inode);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    const size_t remaining_folios =
        filemap_mapping_folio_count(allocation->mapping);
    if (remaining_folios != 0) {
        uint64_t inode_number = 0;
        memcpy(
            &inode_number,
            (const uint8_t *)inode + KB_FS_INODE_NUMBER_OFFSET,
            sizeof(inode_number));
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=vfs_iput_evict_cache "
            "inode=%llu mapping=%p remaining=%zu\n",
            (unsigned long long)inode_number,
            allocation->mapping,
            remaining_folios);
    }
    kb_fs_subsystem_free_fake_inode(inode);
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

void kb_fs_subsystem_init_special_inode(void *inode, unsigned int mode, unsigned int rdev)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    write_u32_field(inode, KB_FS_INODE_MODE_OFFSET, mode);
    write_u32_field(inode, KB_FS_INODE_RDEV_OFFSET, rdev);
    write_u32_field(inode, KB_FS_INODE_NLINK_OFFSET, 1);
    if (fs_trace_enabled() || kb_fs_inode_is_devpts(inode)) {
        fprintf(stderr,
            "kobox-fs: init_special_inode inode=%p mode=0%o rdev=%u\n",
            inode,
            mode,
            rdev);
    }
}

const char *kb_fs_subsystem_simple_get_link(void *dentry, void *inode, void *done)
{
    (void)dentry;
    (void)done;
    if (low_or_err_pointer(inode)) {
        return (const char *)(intptr_t)-22;
    }
    const char *link = NULL;
    memcpy(&link, (const uint8_t *)inode + KB_FS_INODE_LINK_OFFSET, sizeof(link));
    return link == NULL ? (const char *)(intptr_t)-5 : link;
}

static kb_fs_dentry_allocation_record_t *dentry_allocation_find(void *dentry)
{
    FS_HOTPATH_BEGIN(profile_start);
    for (size_t i = 0; i < KB_FS_DENTRY_ALLOCATION_MAX; ++i) {
        if (dentry_allocations[i].active && dentry_allocations[i].dentry == dentry) {
            kb_fs_dentry_allocation_record_t *record = &dentry_allocations[i];
            FS_HOTPATH_END(dentry_find, profile_start);
            return record;
        }
    }
    FS_HOTPATH_END(dentry_find, profile_start);
    return NULL;
}

static kb_fs_dentry_allocation_record_t *dentry_allocation_claim(
    void *dentry,
    char *name,
    int devpts_layout,
    int parent_ref_held)
{
    FS_HOTPATH_BEGIN(profile_start);
    for (size_t i = 0; i < KB_FS_DENTRY_ALLOCATION_MAX; ++i) {
        if (!dentry_allocations[i].active) {
            dentry_allocations[i].active = 1;
            dentry_allocations[i].devpts_layout = devpts_layout;
            dentry_allocations[i].hashed = 0;
            dentry_allocations[i].parent_ref_held = parent_ref_held;
            dentry_allocations[i].refcount = 1;
            dentry_allocations[i].dentry = dentry;
            dentry_allocations[i].name = name;
            kb_fs_dentry_allocation_record_t *record = &dentry_allocations[i];
            FS_HOTPATH_END(dentry_claim, profile_start);
            return record;
        }
    }
    FS_HOTPATH_END(dentry_claim, profile_start);
    return NULL;
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
    if (dentry_allocation_claim(
            dentry,
            NULL,
            kb_fs_super_is_devpts(super_block),
            0) == NULL)
    {
        free(dentry);
        return NULL;
    }
    last_mount_path_probe.root_inode = inode;
    last_mount_path_probe.root_dentry = dentry;
    memset(last_root_vfsmount, 0, sizeof(last_root_vfsmount));
    write_pointer_field(last_root_vfsmount, KB_FS_VFSMOUNT_ROOT_OFFSET, dentry);
    write_pointer_field(last_root_vfsmount, KB_FS_VFSMOUNT_SB_OFFSET, super_block);
    write_u32_field(
        last_root_vfsmount,
        KB_FS_VFSMOUNT_FLAGS_OFFSET,
        KB_FS_MNT_RELATIME);
    last_mount_path_probe.root_vfsmount = last_root_vfsmount;
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

void *kb_fs_subsystem_mount_nodev(void *fs_type, int flags, void *data, int (*fill_super)(void *, void *, int))
{
    (void)fs_type;
    if (fill_super == NULL) {
        return fs_err_ptr(-22);
    }
    void *super_block = calloc(1, KB_FS_SUPER_BLOCK_BYTES);
    if (super_block == NULL) {
        return fs_err_ptr(-12);
    }
    last_mount_path_probe.root_dentry = NULL;
    last_mount_path_probe.root_inode = NULL;
    last_mount_path_probe.root_vfsmount = NULL;
    memset(devpts_index_dentries, 0, sizeof(devpts_index_dentries));
    int status = fill_super(super_block, data, (flags & 0x8000) ? 1 : 0);
    if (status != 0) {
        free(super_block);
        return fs_err_ptr(status);
    }
    if (last_mount_path_probe.root_dentry == NULL) {
        free(super_block);
        return fs_err_ptr(-2);
    }
    last_mount_path_probe.super_block = super_block;
    write_u64_field(super_block, KB_FS_DEVPTS_SUPER_MAGIC_OFFSET, KB_FS_DEVPTS_SUPER_MAGIC);
    write_pointer_field(super_block, KB_FS_DEVPTS_SUPER_ROOT_OFFSET, last_mount_path_probe.root_dentry);
    kb_fs_write_dentry_super(last_mount_path_probe.root_dentry, super_block);
    write_pointer_field(last_nodev_vfsmount, KB_FS_VFSMOUNT_ROOT_OFFSET, last_mount_path_probe.root_dentry);
    write_pointer_field(last_nodev_vfsmount, KB_FS_VFSMOUNT_SB_OFFSET, super_block);
    write_u32_field(
        last_nodev_vfsmount,
        KB_FS_VFSMOUNT_FLAGS_OFFSET,
        KB_FS_MNT_RELATIME);
    last_mount_path_probe.root_vfsmount = last_nodev_vfsmount;
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: mount_nodev super=%p root=%p inode=%p\n",
            super_block,
            last_mount_path_probe.root_dentry,
            last_mount_path_probe.root_inode);
    }
    return last_mount_path_probe.root_dentry;
}

int kb_fs_subsystem_path_pts(void *path)
{
    if (path == NULL || last_mount_path_probe.root_dentry == NULL || last_mount_path_probe.super_block == NULL) {
        return -2;
    }

    write_pointer_field(path, KB_FS_PATH_MNT_OFFSET, last_nodev_vfsmount);
    write_pointer_field(path, KB_FS_PATH_DENTRY_OFFSET, last_mount_path_probe.root_dentry);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: path_pts path=%p mnt=%p root=%p super=%p\n",
            path,
            (void *)last_nodev_vfsmount,
            last_mount_path_probe.root_dentry,
            last_mount_path_probe.super_block);
    }
    return 0;
}

static int parse_devpts_index_name(const char *name, unsigned *out_index)
{
    if (name == NULL || name[0] == '\0' || out_index == NULL) {
        return 0;
    }
    unsigned value = 0;
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
        value = value * 10u + (unsigned)(name[i] - '0');
        if (value >= KB_FS_DEVPTS_DENTRY_MAX) {
            return 0;
        }
    }
    *out_index = value;
    return 1;
}

static void remember_devpts_dentry(void *parent, void *dentry, const char *name)
{
    if (parent != last_mount_path_probe.root_dentry || dentry == NULL) {
        return;
    }
    unsigned index = 0;
    if (!parse_devpts_index_name(name, &index)) {
        return;
    }
    devpts_index_dentries[index] = dentry;
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: devpts remember index=%u dentry=%p\n", index, dentry);
    }
}

int kb_fs_subsystem_path_devpts_index(void *path, unsigned index)
{
    if (path == NULL ||
        index >= KB_FS_DEVPTS_DENTRY_MAX ||
        devpts_index_dentries[index] == NULL ||
        last_mount_path_probe.super_block == NULL)
    {
        return -2;
    }
    write_pointer_field(path, KB_FS_PATH_MNT_OFFSET, last_nodev_vfsmount);
    write_pointer_field(path, KB_FS_PATH_DENTRY_OFFSET, devpts_index_dentries[index]);
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: path_devpts_index path=%p index=%u mnt=%p dentry=%p\n",
            path,
            index,
            (void *)last_nodev_vfsmount,
            devpts_index_dentries[index]);
    }
    return 0;
}

void *kb_fs_subsystem_d_splice_alias(void *inode, void *dentry)
{
    if ((intptr_t)inode < 0 && (intptr_t)inode >= -4095) {
        return inode;
    }
    if (dentry == NULL) {
        return NULL;
    }
    if (inode == NULL) {
        return NULL;
    }
    write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
    kb_fs_set_dentry_entry_type(dentry, inode);
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    const size_t sb_offset = kb_fs_dentry_sb_offset_for_super(super_block);
    if (read_pointer_field(dentry, sb_offset) == NULL) {
        write_pointer_field(dentry, sb_offset, super_block);
    }
    kb_fs_dentry_allocation_record_t *allocation =
        dentry_allocation_find(dentry);
    if (allocation != NULL) {
        allocation->hashed = 1;
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
    /* The ordinary Linux d_splice_alias() case instantiates the supplied
     * negative dentry and returns NULL.  Returning the supplied dentry here
     * made ext4 ->lookup appear to have found an alternate alias. */
    return NULL;
}

static void *kb_fs_subsystem_d_alloc_len(
    void *parent,
    const char *name,
    size_t name_len)
{
    if (parent == NULL || low_or_err_pointer(name) ||
        name_len > UINT32_MAX)
    {
        return NULL;
    }
    void *dentry = calloc(1, KB_FS_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return NULL;
    }
    kb_fs_dentry_allocation_record_t *parent_allocation =
        dentry_allocation_find(parent);
    const int devpts_layout = parent_allocation != NULL &&
        parent_allocation->devpts_layout;
    void *super_block = read_pointer_field(
        parent,
        devpts_layout ? KB_FS_DEVPTS_DENTRY_SB_OFFSET : KB_FS_DENTRY_SB_OFFSET);
    char *owned_name = NULL;
    char *stored_name = (char *)dentry + KB_FS_DENTRY_INLINE_NAME_OFFSET;
    if (name_len >= KB_FS_DENTRY_INLINE_NAME_BYTES) {
        owned_name = malloc(name_len + 1u);
        if (owned_name == NULL) {
            free(dentry);
            return NULL;
        }
        stored_name = owned_name;
    }
    if (owned_name != NULL) {
        memcpy(stored_name, name, name_len);
        stored_name[name_len] = '\0';
    }
    const int parent_ref_held = parent_allocation != NULL;
    if (dentry_allocation_claim(
            dentry,
            owned_name,
            devpts_layout,
            parent_ref_held) == NULL)
    {
        free(owned_name);
        free(dentry);
        return NULL;
    }
    if (parent_ref_held) {
        parent_allocation->refcount++;
    }
    kb_fs_prepare_named_dentry(
        dentry,
        parent,
        NULL,
        super_block,
        owned_name != NULL ? stored_name : name);
    if (owned_name == NULL) {
        memcpy(stored_name, name, name_len);
        stored_name[name_len] = '\0';
        kb_fs_prepare_dentry_name(dentry, stored_name);
    }
    remember_devpts_dentry(parent, dentry, stored_name);
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: d_alloc_name parent=%p dentry=%p name=%s\n", parent, dentry, stored_name);
    }
    return dentry;
}

void *kb_fs_subsystem_d_alloc(void *parent, const void *name)
{
    if (low_or_err_pointer(name)) {
        return NULL;
    }
    const uint32_t name_len = read_u32_field(name, sizeof(uint32_t));
    const char *name_pointer = read_pointer_field(name, sizeof(uint64_t));
    if (low_or_err_pointer(name_pointer)) {
        return NULL;
    }
    return kb_fs_subsystem_d_alloc_len(parent, name_pointer, name_len);
}

void *kb_fs_subsystem_d_alloc_name(void *parent, const char *name)
{
    if (low_or_err_pointer(name)) {
        return NULL;
    }
    return kb_fs_subsystem_d_alloc_len(parent, name, strlen(name));
}

void kb_fs_subsystem_d_drop(void *dentry)
{
    kb_fs_dentry_allocation_record_t *allocation =
        dentry_allocation_find(dentry);
    if (allocation != NULL) {
        allocation->hashed = 0;
    }
}

void *kb_fs_subsystem_d_find_any_alias(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_DENTRY_ALLOCATION_MAX; ++i) {
        kb_fs_dentry_allocation_record_t *allocation =
            &dentry_allocations[i];
        if (!allocation->active ||
            read_pointer_field(
                allocation->dentry,
                KB_FS_DENTRY_INODE_OFFSET) != inode)
        {
            continue;
        }
        allocation->refcount++;
        return allocation->dentry;
    }
    return NULL;
}

void *kb_fs_subsystem_dget_parent(void *dentry)
{
    kb_fs_dentry_allocation_record_t *allocation =
        dentry_allocation_find(dentry);
    if (allocation == NULL) {
        return NULL;
    }
    void *parent = read_pointer_field(dentry, KB_FS_DENTRY_PARENT_OFFSET);
    kb_fs_dentry_allocation_record_t *parent_allocation =
        dentry_allocation_find(parent);
    if (parent_allocation == NULL) {
        return NULL;
    }
    parent_allocation->refcount++;
    return parent;
}

void kb_fs_subsystem_d_mark_dontcache(void *dentry)
{
    if (dentry_allocation_find(dentry) == NULL) {
        return;
    }
    uint32_t flags = read_u32_field(dentry, KB_FS_DENTRY_FLAGS_OFFSET);
    write_u32_field(
        dentry,
        KB_FS_DENTRY_FLAGS_OFFSET,
        flags | (1u << 7));
}

void *kb_fs_subsystem_d_obtain_alias(void *inode)
{
    if (inode == NULL) {
        return fs_err_ptr(-116);
    }
    if ((intptr_t)inode < 0 && (intptr_t)inode >= -4095) {
        return inode;
    }
    void *alias = kb_fs_subsystem_d_find_any_alias(inode);
    if (alias != NULL) {
        kb_fs_subsystem_iput(inode);
        return alias;
    }

    void *dentry = calloc(1, KB_FS_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        kb_fs_subsystem_iput(inode);
        return fs_err_ptr(-12);
    }
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    kb_fs_prepare_named_dentry(dentry, dentry, inode, super_block, "");
    if (dentry_allocation_claim(
            dentry,
            NULL,
            kb_fs_super_is_devpts(super_block),
            0) == NULL)
    {
        free(dentry);
        kb_fs_subsystem_iput(inode);
        return fs_err_ptr(-12);
    }
    uint32_t flags = read_u32_field(dentry, KB_FS_DENTRY_FLAGS_OFFSET);
    write_u32_field(
        dentry,
        KB_FS_DENTRY_FLAGS_OFFSET,
        flags | (1u << 5));
    return dentry;
}

int kb_fs_subsystem_generic_encode_ino32_fh(
    void *inode,
    uint32_t *file_handle,
    int *max_length,
    void *parent)
{
    enum {
        KB_FS_FILEID_INO32_GEN = 1,
        KB_FS_FILEID_INO32_GEN_PARENT = 2,
        KB_FS_FILEID_INVALID = 0xff,
    };
    if (low_or_err_pointer(inode) || file_handle == NULL ||
        max_length == NULL)
    {
        return KB_FS_FILEID_INVALID;
    }
    if (parent != NULL && low_or_err_pointer(parent)) {
        return KB_FS_FILEID_INVALID;
    }
    if (parent != NULL && *max_length < 4) {
        *max_length = 4;
        return KB_FS_FILEID_INVALID;
    }
    if (*max_length < 2) {
        *max_length = 2;
        return KB_FS_FILEID_INVALID;
    }

    file_handle[0] = (uint32_t)read_u64_field(
        inode,
        KB_FS_INODE_NUMBER_OFFSET);
    file_handle[1] = read_u32_field(
        inode,
        KB_FS_INODE_GENERATION_OFFSET);
    *max_length = 2;
    if (parent == NULL) {
        return KB_FS_FILEID_INO32_GEN;
    }
    file_handle[2] = (uint32_t)read_u64_field(
        parent,
        KB_FS_INODE_NUMBER_OFFSET);
    file_handle[3] = read_u32_field(
        parent,
        KB_FS_INODE_GENERATION_OFFSET);
    *max_length = 4;
    return KB_FS_FILEID_INO32_GEN_PARENT;
}

static void *generic_fh_get_inode(
    void *super_block,
    uint64_t inode_number,
    uint32_t generation,
    void *get_inode_operation)
{
    if (low_or_err_pointer(super_block) || get_inode_operation == NULL) {
        return NULL;
    }
    void *(*get_inode_fn)(void *, uint64_t, uint32_t) = NULL;
    memcpy(&get_inode_fn, &get_inode_operation, sizeof(get_inode_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(
        get_inode_operation,
        &old_gs);
    void *inode = get_inode_fn(super_block, inode_number, generation);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return inode;
}

void *kb_fs_subsystem_generic_fh_to_dentry(
    void *super_block,
    const uint32_t *file_handle,
    int handle_length,
    int handle_type,
    void *get_inode_operation)
{
    enum {
        KB_FS_FILEID_INO32_GEN = 1,
        KB_FS_FILEID_INO32_GEN_PARENT = 2,
    };
    if (file_handle == NULL || handle_length < 2) {
        return NULL;
    }
    void *inode = NULL;
    if (handle_type == KB_FS_FILEID_INO32_GEN ||
        handle_type == KB_FS_FILEID_INO32_GEN_PARENT)
    {
        inode = generic_fh_get_inode(
            super_block,
            file_handle[0],
            file_handle[1],
            get_inode_operation);
    }
    return kb_fs_subsystem_d_obtain_alias(inode);
}

void *kb_fs_subsystem_generic_fh_to_parent(
    void *super_block,
    const uint32_t *file_handle,
    int handle_length,
    int handle_type,
    void *get_inode_operation)
{
    enum { KB_FS_FILEID_INO32_GEN_PARENT = 2 };
    if (file_handle == NULL || handle_length <= 2) {
        return NULL;
    }
    void *inode = NULL;
    if (handle_type == KB_FS_FILEID_INO32_GEN_PARENT) {
        inode = generic_fh_get_inode(
            super_block,
            file_handle[2],
            handle_length > 3 ? file_handle[3] : 0,
            get_inode_operation);
    }
    return kb_fs_subsystem_d_obtain_alias(inode);
}

void *kb_fs_subsystem_find_inode_by_ino_rcu(
    void *super_block,
    unsigned long inode_number)
{
    if (low_or_err_pointer(super_block)) {
        return NULL;
    }
    for (size_t i = 0; i < KB_FS_INODE_ALLOCATION_MAX; ++i) {
        kb_fs_inode_allocation_record_t *allocation = &inode_allocations[i];
        if (!allocation->active || !allocation->hashed ||
            low_or_err_pointer(allocation->inode) ||
            read_pointer_field(
                allocation->inode,
                KB_FS_INODE_SB_OFFSET) != super_block ||
            read_u64_field(
                allocation->inode,
                KB_FS_INODE_NUMBER_OFFSET) != inode_number)
        {
            continue;
        }
        const uint64_t state = read_u64_field(
            allocation->inode,
            KB_FS_INODE_STATE_OFFSET);
        if ((state & (KB_FS_INODE_STATE_FREEING |
                KB_FS_INODE_STATE_WILL_FREE)) == 0)
        {
            return allocation->inode;
        }
    }
    return NULL;
}

long kb_fs_subsystem_generic_read_dir(
    void *file,
    char *buffer,
    size_t size,
    int64_t *position)
{
    (void)file;
    (void)buffer;
    (void)size;
    (void)position;
    return -21;
}

void kb_fs_subsystem_path_get(void *path)
{
    if (low_or_err_pointer(path)) {
        return;
    }
    void *dentry = read_pointer_field(path, KB_FS_PATH_DENTRY_OFFSET);
    kb_fs_dentry_allocation_record_t *allocation =
        dentry_allocation_find(dentry);
    if (allocation != NULL && allocation->refcount != UINT_MAX) {
        allocation->refcount++;
    }
}

void kb_fs_subsystem_path_put(void *path)
{
    if (low_or_err_pointer(path)) {
        return;
    }
    void *dentry = read_pointer_field(path, KB_FS_PATH_DENTRY_OFFSET);
    if (dentry_allocation_find(dentry) != NULL) {
        kb_fs_subsystem_dput(dentry);
    }
}

int kb_fs_subsystem_finish_open(
    void *file,
    void *dentry,
    void *open_operation)
{
    if (low_or_err_pointer(file) || low_or_err_pointer(dentry)) {
        return -22;
    }
    uint32_t mode = read_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET);
    if ((mode & KB_FS_FMODE_OPENED) != 0) {
        return -16;
    }
    void *inode = read_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET);
    void *mapping = low_or_err_pointer(inode) ? NULL :
        read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    void *file_operations = low_or_err_pointer(inode) ? NULL :
        read_pointer_field(inode, KB_FS_INODE_FILE_OP_OFFSET);
    if (low_or_err_pointer(inode) || mapping == NULL ||
        file_operations == NULL)
    {
        return -19;
    }

    void *path = (uint8_t *)file + KB_FS_NATIVE_FILE_PATH_MNT_OFFSET;
    write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET, dentry);
    kb_fs_subsystem_path_get(path);
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    write_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, file_operations);
    write_u32_field(
        file,
        KB_FS_NATIVE_FILE_WB_ERR_OFFSET,
        read_u32_field(mapping, KB_FS_ADDRESS_SPACE_WB_ERR_OFFSET));
    file_ra_state_init(
        (uint8_t *)file + KB_FS_NATIVE_FILE_RA_OFFSET);

    const uint16_t inode_mode = (uint16_t)read_u32_field(
        inode,
        KB_FS_INODE_MODE_OFFSET);
    const uint16_t inode_type = inode_mode & KB_FS_MODE_TYPE_MASK;
    int writer_acquired = 0;
    if ((mode & KB_FS_FMODE_WRITE) != 0 &&
        (inode_type == 0100000u || inode_type == 0040000u ||
            inode_type == 0120000u))
    {
        uint32_t writecount = read_u32_field(
            inode,
            KB_FS_INODE_WRITECOUNT_OFFSET);
        if (writecount == UINT32_MAX) {
            kb_fs_subsystem_path_put(path);
            write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET, NULL);
            write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, NULL);
            write_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET, NULL);
            write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, NULL);
            return -75;
        }
        write_u32_field(
            inode,
            KB_FS_INODE_WRITECOUNT_OFFSET,
            writecount + 1u);
        mode |= KB_FS_FMODE_WRITER;
        writer_acquired = 1;
    }
    if (inode_type == 0100000u || inode_type == 0040000u) {
        mode |= KB_FS_FMODE_ATOMIC_POS;
    }
    mode |= KB_FS_FMODE_LSEEK | KB_FS_FMODE_PREAD | KB_FS_FMODE_PWRITE;
    write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET, mode);

    if (open_operation == NULL) {
        open_operation = read_pointer_field(
            file_operations,
            KB_FS_FILE_OP_OPEN_OFFSET);
    }
    int status = 0;
    if (open_operation != NULL) {
        int (*open_fn)(void *, void *) = NULL;
        memcpy(&open_fn, &open_operation, sizeof(open_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(open_operation, &old_gs);
        status = open_fn(inode, file);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status > 0) {
            status = -22;
        }
    }
    if (status != 0) {
        if (writer_acquired) {
            uint32_t writecount = read_u32_field(
                inode,
                KB_FS_INODE_WRITECOUNT_OFFSET);
            if (writecount != 0) {
                write_u32_field(
                    inode,
                    KB_FS_INODE_WRITECOUNT_OFFSET,
                    writecount - 1u);
            }
        }
        mode = read_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET);
        mode &= ~(uint32_t)(KB_FS_FMODE_WRITER | KB_FS_FMODE_ATOMIC_POS |
            KB_FS_FMODE_LSEEK | KB_FS_FMODE_PREAD | KB_FS_FMODE_PWRITE |
            KB_FS_FMODE_CAN_READ | KB_FS_FMODE_CAN_WRITE |
            KB_FS_FMODE_CAN_ODIRECT | KB_FS_FMODE_OPENED);
        write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET, mode);
        kb_fs_subsystem_path_put(path);
        write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_MNT_OFFSET, NULL);
        write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET, NULL);
        write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, NULL);
        write_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET, NULL);
        write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, NULL);
        return status;
    }

    mode = read_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET);
    mode |= KB_FS_FMODE_OPENED;
    if ((mode & KB_FS_FMODE_READ) != 0 &&
        (read_pointer_field(file_operations, KB_FS_FILE_OP_READ_OFFSET) != NULL ||
            read_pointer_field(
                file_operations,
                KB_FS_FILE_OP_READ_ITER_OFFSET) != NULL))
    {
        mode |= KB_FS_FMODE_CAN_READ;
    }
    if ((mode & KB_FS_FMODE_WRITE) != 0 &&
        (read_pointer_field(file_operations, KB_FS_FILE_OP_WRITE_OFFSET) != NULL ||
            read_pointer_field(
                file_operations,
                KB_FS_FILE_OP_WRITE_ITER_OFFSET) != NULL))
    {
        mode |= KB_FS_FMODE_CAN_WRITE;
    }
    if (read_pointer_field(file_operations, KB_FS_FILE_OP_LLSEEK_OFFSET) == NULL) {
        mode &= ~KB_FS_FMODE_LSEEK;
    }
    void *address_space_operations = read_pointer_field(
        mapping,
        KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    if (address_space_operations != NULL &&
        read_pointer_field(address_space_operations, 0x58) != NULL)
    {
        mode |= KB_FS_FMODE_CAN_ODIRECT;
    }
    write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET, mode);
    uint32_t flags = read_u32_field(file, 0x30);
    write_u32_field(file, 0x30, flags & ~(uint32_t)0x3c0u);
    return 0;
}

int kb_fs_subsystem_file_open(
    void *vfsmount,
    void *dentry,
    unsigned int access,
    void **out_file)
{
    if (low_or_err_pointer(vfsmount) || low_or_err_pointer(dentry) ||
        out_file == NULL ||
        (access & ~(unsigned int)(KB_FS_FILE_ACCESS_READ |
            KB_FS_FILE_ACCESS_WRITE)) != 0 ||
        access == 0)
    {
        return -22;
    }
    *out_file = NULL;

    void *file = calloc(1, KB_FS_FAKE_INODE_BYTES);
    if (file == NULL) {
        return -12;
    }
    uint32_t mode = 0;
    if ((access & KB_FS_FILE_ACCESS_READ) != 0) {
        mode |= KB_FS_FMODE_READ;
    }
    if ((access & KB_FS_FILE_ACCESS_WRITE) != 0) {
        mode |= KB_FS_FMODE_WRITE;
    }
    write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET, mode);
    write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_MNT_OFFSET, vfsmount);

    const int status = kb_fs_subsystem_finish_open(file, dentry, NULL);
    if (status != 0) {
        free(file);
        return status;
    }
    *out_file = file;
    return 0;
}

int kb_fs_subsystem_file_close(void *file)
{
    if (low_or_err_pointer(file)) {
        return -22;
    }
    const uint32_t mode = read_u32_field(
        file,
        KB_FS_NATIVE_FILE_MODE_OFFSET);
    if ((mode & KB_FS_FMODE_OPENED) == 0) {
        return -9;
    }

    void *inode = read_pointer_field(file, KB_FS_FILE_INODE_OFFSET);
    void *file_operations = read_pointer_field(
        file,
        KB_FS_NATIVE_FILE_OP_OFFSET);
    void *release_operation = file_operations == NULL ? NULL :
        read_pointer_field(file_operations, KB_FS_FILE_OP_RELEASE_OFFSET);
    int status = 0;
    if (release_operation != NULL) {
        int (*release_fn)(void *, void *) = NULL;
        memcpy(&release_fn, &release_operation, sizeof(release_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(
            release_operation,
            &old_gs);
        status = release_fn(inode, file);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (status > 0) {
            status = -22;
        }
    }

    if ((mode & KB_FS_FMODE_WRITER) != 0 && inode != NULL) {
        uint32_t writecount = read_u32_field(
            inode,
            KB_FS_INODE_WRITECOUNT_OFFSET);
        if (writecount == 0) {
            if (status == 0) {
                status = -5;
            }
        } else {
            write_u32_field(
                inode,
                KB_FS_INODE_WRITECOUNT_OFFSET,
                writecount - 1u);
        }
    }

    void *path = (uint8_t *)file + KB_FS_NATIVE_FILE_PATH_MNT_OFFSET;
    kb_fs_subsystem_path_put(path);
    free(file);
    return status;
}

void kb_fs_subsystem_d_tmpfile(void *file, void *inode)
{
    if (low_or_err_pointer(file) || low_or_err_pointer(inode)) {
        return;
    }
    void *dentry = read_pointer_field(
        file,
        KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET);
    kb_fs_dentry_allocation_record_t *allocation =
        dentry_allocation_find(dentry);
    if (allocation == NULL) {
        return;
    }

    uint32_t nlink = read_u32_field(inode, KB_FS_INODE_NLINK_OFFSET);
    if (nlink != 0) {
        write_u32_field(inode, KB_FS_INODE_NLINK_OFFSET, nlink - 1u);
    }
    uint64_t inode_number = 0;
    memcpy(
        &inode_number,
        (const uint8_t *)inode + KB_FS_INODE_NUMBER_OFFSET,
        sizeof(inode_number));
    char *inline_name =
        (char *)dentry + KB_FS_DENTRY_INLINE_NAME_OFFSET;
    const int length = snprintf(
        inline_name,
        KB_FS_DENTRY_INLINE_NAME_BYTES,
        "#%llu",
        (unsigned long long)inode_number);
    if (length < 0 || (size_t)length >= KB_FS_DENTRY_INLINE_NAME_BYTES) {
        return;
    }
    free(allocation->name);
    allocation->name = NULL;
    kb_fs_prepare_dentry_name(dentry, inline_name);
    kb_fs_subsystem_d_instantiate(dentry, inode);
    allocation->hashed = 0;
}

void kb_fs_subsystem_dput(void *dentry)
{
    if (dentry == NULL) {
        return;
    }
    kb_fs_dentry_allocation_record_t *allocation = dentry_allocation_find(dentry);
    if (allocation == NULL) {
        return;
    }
    if (allocation->refcount > 1u) {
        allocation->refcount--;
        return;
    }
    void *inode = read_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET);
    void *parent = read_pointer_field(dentry, KB_FS_DENTRY_PARENT_OFFSET);
    const int release_parent = allocation->parent_ref_held &&
        parent != NULL && parent != dentry;
    for (size_t i = 0; i < KB_FS_DEVPTS_DENTRY_MAX; ++i) {
        if (devpts_index_dentries[i] == dentry) {
            devpts_index_dentries[i] = NULL;
        }
    }
    if (last_mount_path_probe.root_dentry == dentry) {
        last_mount_path_probe.root_dentry = NULL;
        last_mount_path_probe.root_inode = NULL;
    }
    char *name = allocation->name;
    memset(allocation, 0, sizeof(*allocation));
    free(name);
    free(dentry);
    /* The positive dentry owns the inode reference installed by d_add(),
     * d_splice_alias(), d_obtain_alias(), or d_tmpfile().  Dropping the last
     * dentry reference must release it so an unlinked inode reaches the
     * filesystem's real ->evict_inode orphan cleanup path. */
    if (!low_or_err_pointer(inode)) {
        kb_fs_subsystem_iput(inode);
    }
    if (release_parent) {
        kb_fs_subsystem_dput(parent);
    }
}

void kb_fs_subsystem_d_add(void *dentry, void *inode)
{
    kb_fs_subsystem_d_instantiate(dentry, inode);
}

void kb_fs_subsystem_d_instantiate(void *dentry, void *inode)
{
    if (dentry == NULL || low_or_err_pointer(inode)) {
        return;
    }
    write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
    kb_fs_set_dentry_entry_type(dentry, inode);
    void *super_block = read_pointer_field(inode, KB_FS_INODE_SB_OFFSET);
    const size_t sb_offset = kb_fs_dentry_sb_offset_for_super(super_block);
    if (read_pointer_field(dentry, sb_offset) == NULL) {
        write_pointer_field(dentry, sb_offset, super_block);
    }
    kb_fs_dentry_allocation_record_t *allocation =
        dentry_allocation_find(dentry);
    if (allocation != NULL) {
        allocation->hashed = 1;
    }
    if (fs_trace_enabled()) {
        fprintf(stderr, "kobox-fs: d_instantiate dentry=%p inode=%p\n", dentry, inode);
    } else if (kb_fs_inode_is_devpts(inode)) {
        uint32_t flags = 0;
        uint32_t mode = 0;
        memcpy(&flags, (const uint8_t *)dentry + KB_FS_DENTRY_FLAGS_OFFSET, sizeof(flags));
        memcpy(&mode, (const uint8_t *)inode + KB_FS_INODE_MODE_OFFSET, sizeof(mode));
        fprintf(stderr,
            "kobox-fs: devpts d_instantiate dentry=%p inode=%p flags=0x%x mode=0%o\n",
            dentry,
            inode,
            flags,
            mode);
    }
}

void kb_fs_subsystem_d_instantiate_new(void *dentry, void *inode)
{
    kb_fs_subsystem_d_instantiate(dentry, inode);
    kb_fs_subsystem_unlock_new_inode(inode);
}

int kb_fs_subsystem_bmap(void *inode, uint64_t *block)
{
    if (inode == NULL || block == NULL) {
        return -22;
    }
    void *mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    void *a_ops = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    void *operation = read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_BMAP_OFFSET);
    if (operation == NULL) {
        return -95;
    }
    uint64_t (*bmap_fn)(void *, uint64_t) = NULL;
    memcpy(&bmap_fn, &operation, sizeof(bmap_fn));
    const uint64_t logical = *block;
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(operation, &old_gs);
    const uint64_t mapped = bmap_fn(mapping, logical);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (fs_trace_enabled()) {
        fprintf(stderr,
            "kobox-fs: bmap inode=%p mapping=%p logical=%llu mapped=%llu operation=%p\n",
            inode,
            mapping,
            (unsigned long long)logical,
            (unsigned long long)mapped,
            operation);
    }
    *block = mapped;
    return 0;
}

uint64_t kb_fs_subsystem_iomap_bmap(void *mapping, uint64_t block, const void *ops)
{
    if (mapping == NULL || ops == NULL) {
        return 0;
    }
    void *inode = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET);
    if (inode == NULL) {
        return 0;
    }
    uint8_t block_bits = 0;
    memcpy(&block_bits, (const uint8_t *)inode + KB_FS_INODE_BLKBITS_OFFSET, sizeof(block_bits));
    if (block_bits < 9u || block_bits >= 63u || block > (UINT64_MAX >> block_bits)) {
        return 0;
    }
    if (filemap_writeback_range(mapping, 0, INT64_MAX) != 0) {
        return 0;
    }
    void *begin_op = read_pointer_field(ops, 0);
    if (begin_op == NULL) {
        return 0;
    }
    int (*begin_fn)(void *, int64_t, int64_t, unsigned int, void *, void *) = NULL;
    memcpy(&begin_fn, &begin_op, sizeof(begin_fn));
    uint8_t iomap[96];
    uint8_t srcmap[96];
    memset(iomap, 0, sizeof(iomap));
    memset(srcmap, 0, sizeof(srcmap));
    const uint64_t pos = block << block_bits;
    const uint64_t length = UINT64_C(1) << block_bits;
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(begin_op, &old_gs);
    const int status = begin_fn(inode, (int64_t)pos, (int64_t)length, 1u << 2, iomap, srcmap);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    uint16_t type = 0;
    uint64_t address = 0;
    uint64_t mapped_offset = 0;
    memcpy(&type, iomap + 24, sizeof(type));
    memcpy(&address, iomap, sizeof(address));
    memcpy(&mapped_offset, iomap + 8, sizeof(mapped_offset));
    if (status != 0 || type != 2u || address == UINT64_MAX || pos < mapped_offset ||
        address > UINT64_MAX - (pos - mapped_offset))
    {
        return 0;
    }
    return (address + pos - mapped_offset) >> block_bits;
}

static unsigned long page_cache_next_miss(
    void *mapping,
    unsigned long index,
    unsigned long maximum)
{
    for (unsigned long scanned = 0; scanned < maximum; ++scanned) {
        if (kb_fs_subsystem_xa_load(
                (uint8_t *)mapping + 8u,
                index) == NULL)
        {
            return index;
        }
        if (index == ULONG_MAX) {
            return 0;
        }
        index++;
    }
    return 0;
}

static unsigned long page_cache_next_ra_size(
    unsigned long current,
    unsigned long maximum)
{
    if (current == 0 || maximum == 0) {
        return 0;
    }
    if (current < maximum / 16u && current <= ULONG_MAX / 4u) {
        return current * 4u;
    }
    if (current <= maximum / 2u && current <= ULONG_MAX / 2u) {
        return current * 2u;
    }
    return maximum;
}

static void page_cache_async_ra(
    void *readahead_control,
    void *folio,
    unsigned long requested_count)
{
    if (readahead_control == NULL || low_or_err_pointer(folio) ||
        requested_count == 0)
    {
        return;
    }
    void *ra = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_RA_OFFSET);
    void *mapping = read_pointer_field(
        readahead_control,
        KB_FS_READAHEAD_MAPPING_OFFSET);
    if (ra == NULL || mapping == NULL) {
        return;
    }
    unsigned long maximum = read_u32_field(
        ra,
        KB_FS_FILE_RA_PAGES_OFFSET);
    if (maximum == 0) {
        return;
    }
    if (maximum > KB_FS_BIO_READ_BATCH_MAX) {
        maximum = KB_FS_BIO_READ_BATCH_MAX;
    }
    if (maximum < requested_count) {
        maximum = requested_count > KB_FS_BIO_READ_BATCH_MAX ?
            KB_FS_BIO_READ_BATCH_MAX : requested_count;
    }
    uint64_t flags = read_u64_field(folio, 0);
    if ((flags & KB_FS_FOLIO_FLAG_WRITEBACK) != 0) {
        return;
    }
    write_u64_field(
        folio,
        0,
        flags & ~(uint64_t)KB_FS_FOLIO_FLAG_READAHEAD);

    const unsigned long index = (unsigned long)read_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET);
    unsigned long start = read_u64_field(ra, KB_FS_FILE_RA_START_OFFSET);
    unsigned long size = read_u32_field(ra, KB_FS_FILE_RA_SIZE_OFFSET);
    const unsigned long async_size = read_u32_field(
        ra,
        KB_FS_FILE_RA_ASYNC_SIZE_OFFSET);
    const unsigned long expected = start + size - async_size;
    if (index == expected && size <= ULONG_MAX - start) {
        start += size;
        size = page_cache_next_ra_size(size, maximum);
    } else {
        if (index == ULONG_MAX) {
            return;
        }
        start = page_cache_next_miss(mapping, index + 1u, maximum);
        if (start == 0 || start - index > maximum) {
            return;
        }
        size = start - index;
        if (requested_count > ULONG_MAX - size) {
            size = maximum;
        } else {
            size += requested_count;
            size = page_cache_next_ra_size(size, maximum);
        }
    }
    write_u64_field(ra, KB_FS_FILE_RA_START_OFFSET, start);
    write_u32_field(ra, KB_FS_FILE_RA_SIZE_OFFSET, (uint32_t)size);
    write_u32_field(ra, KB_FS_FILE_RA_ASYNC_SIZE_OFFSET, (uint32_t)size);
    write_u64_field(
        readahead_control,
        KB_FS_READAHEAD_INDEX_OFFSET,
        start);
    page_cache_do_ra(readahead_control, size, size);
}

static unsigned long filemap_read_requested_pages(
    size_t page_offset,
    uint64_t remaining)
{
    uint64_t pages = remaining / KB_FS_PAGE_SIZE;
    const uint64_t tail = remaining % KB_FS_PAGE_SIZE;
    if (tail != 0 || page_offset != 0) {
        pages++;
    }
    if (page_offset != 0 && tail != 0 &&
        page_offset + tail > KB_FS_PAGE_SIZE)
    {
        pages++;
    }
    if (pages == 0) {
        pages = 1;
    }
    return pages > KB_FS_BIO_READ_BATCH_MAX ?
        KB_FS_BIO_READ_BATCH_MAX : (unsigned long)pages;
}

static void *filemap_read_cache_folio(
    void *mapping,
    unsigned long index,
    size_t page_offset,
    uint64_t remaining,
    void *file)
{
    const unsigned long requested_pages = filemap_read_requested_pages(
        page_offset,
        remaining);
    void *folio = kb_fs_subsystem_filemap_get_folio(mapping, index, 0, 0);
    if (!low_or_err_pointer(folio)) {
        const uint64_t flags = read_u64_field(folio, 0);
        if ((flags & KB_FS_FOLIO_FLAG_READAHEAD) != 0) {
            uint8_t readahead_control[56] = {0};
            write_pointer_field(
                readahead_control,
                KB_FS_READAHEAD_FILE_OFFSET,
                file);
            write_pointer_field(
                readahead_control,
                KB_FS_READAHEAD_MAPPING_OFFSET,
                mapping);
            write_pointer_field(
                readahead_control,
                KB_FS_READAHEAD_RA_OFFSET,
                (uint8_t *)file + KB_FS_NATIVE_FILE_RA_OFFSET);
            write_u64_field(
                readahead_control,
                KB_FS_READAHEAD_INDEX_OFFSET,
                index);
            page_cache_async_ra(
                readahead_control,
                folio,
                requested_pages);
        }
        if ((flags & KB_FS_FOLIO_FLAG_UPTODATE) != 0) {
            return folio;
        }
        kb_fs_subsystem_folio_put(folio);
    } else if ((intptr_t)folio != -2) {
        return folio;
    } else {
        uint8_t readahead_control[56] = {0};
        write_pointer_field(
            readahead_control,
            KB_FS_READAHEAD_FILE_OFFSET,
            file);
        write_pointer_field(
            readahead_control,
            KB_FS_READAHEAD_MAPPING_OFFSET,
            mapping);
        write_pointer_field(
            readahead_control,
            KB_FS_READAHEAD_RA_OFFSET,
            (uint8_t *)file + KB_FS_NATIVE_FILE_RA_OFFSET);
        write_u64_field(
            readahead_control,
            KB_FS_READAHEAD_INDEX_OFFSET,
            index);
        kb_fs_subsystem_page_cache_sync_ra(
            readahead_control,
            requested_pages);
    }

    /* If readahead did not instantiate or complete the requested folio,
     * fall back to the filesystem's native synchronous ->read_folio path. */
    return kb_fs_subsystem_read_cache_folio(mapping, index, NULL, file);
}

long kb_fs_subsystem_generic_file_read_iter(void *kiocb, void *iter)
{
    if (kiocb == NULL || iter == NULL) {
        return -22;
    }
    void *file = read_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET);
    if (file == NULL) {
        return -22;
    }
    void *inode = read_pointer_field(file, KB_FS_FILE_INODE_OFFSET);
    void *mapping = read_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET);
    if (inode == NULL || mapping == NULL) {
        return -22;
    }
    uint64_t pos = 0;
    uint64_t count = 0;
    uint64_t file_size = 0;
    memcpy(&pos, (const uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, sizeof(pos));
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    memcpy(&file_size, (const uint8_t *)inode + KB_FS_INODE_SIZE_OFFSET, sizeof(file_size));
    if (pos >= file_size || count == 0) {
        return 0;
    }
    FS_READ_PROFILE_BEGIN(profile_start);
    uint64_t remaining = file_size - pos;
    if (remaining > count) {
        remaining = count;
    }
    uint64_t copied = 0;
    while (remaining != 0) {
        const unsigned long index = (unsigned long)(pos / KB_FS_PAGE_SIZE);
        const size_t page_offset = (size_t)(pos % KB_FS_PAGE_SIZE);
        size_t chunk = KB_FS_PAGE_SIZE - page_offset;
        if ((uint64_t)chunk > remaining) {
            chunk = (size_t)remaining;
        }
        void *folio = filemap_read_cache_folio(
            mapping,
            index,
            page_offset,
            remaining,
            file);
        if (low_or_err_pointer(folio)) {
            const int error = (int)(intptr_t)folio;
            FS_READ_PROFILE_RETURN(
                profile_start,
                copied != 0 ? (long)copied : error);
        }
        void *payload = folio_page_payload(folio);
        size_t copied_to_iter = 0;
        if (payload != NULL) {
            if (page_offset != 0 || chunk != KB_FS_PAGE_SIZE) {
                FS_READ_PROFILE_BEGIN(copy_start);
                copied_to_iter = kb_fs_subsystem_copy_to_iter(
                    (const uint8_t *)payload + page_offset,
                    chunk,
                    iter);
                FS_READ_PROFILE_END(partial_copy, copy_start);
            } else {
                copied_to_iter = kb_fs_subsystem_copy_to_iter(
                    payload,
                    chunk,
                    iter);
            }
        }
        if (payload == NULL || copied_to_iter != chunk) {
            kb_fs_subsystem_folio_put(folio);
            FS_READ_PROFILE_RETURN(
                profile_start,
                copied != 0 ? (long)copied : -14);
        }
        kb_fs_subsystem_folio_put(folio);
        pos += chunk;
        copied += chunk;
        remaining -= chunk;
    }
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(
        (uint8_t *)file + KB_FS_NATIVE_FILE_RA_OFFSET,
        KB_FS_FILE_RA_PREV_POS_OFFSET,
        pos);
    if (copied != 0) {
        kb_fs_subsystem_touch_atime(
            (uint8_t *)file + KB_FS_NATIVE_FILE_PATH_MNT_OFFSET);
    }
    FS_READ_PROFILE_RETURN(profile_start, (long)copied);
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

static size_t kb_fs_subsystem_iov_iter_limit(void *iter, size_t bytes, void **out_buffer)
{
    if (iter == NULL || out_buffer == NULL) {
        return 0;
    }
    uint64_t count = 0;
    void *buffer = NULL;
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    memcpy(&buffer, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, sizeof(buffer));
    if (buffer == NULL || count == 0) {
        *out_buffer = NULL;
        return 0;
    }
    *out_buffer = buffer;
    if (bytes > count) {
        bytes = (size_t)count;
    }
    return bytes;
}

static void kb_fs_subsystem_iov_iter_advance(void *iter, size_t bytes)
{
    uint64_t count = 0;
    uint64_t capacity = 0;
    void *buffer = NULL;
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    memcpy(&buffer, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, sizeof(buffer));
    memcpy(&capacity, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, sizeof(capacity));
    if (bytes > count) {
        bytes = (size_t)count;
    }
    count -= bytes;
    buffer = (uint8_t *)buffer + bytes;
    if (capacity >= bytes) {
        capacity -= bytes;
    }
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, &count, sizeof(count));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, &buffer, sizeof(buffer));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, &capacity, sizeof(capacity));
}

size_t kb_fs_subsystem_copy_to_iter(const void *addr, size_t bytes, void *iter)
{
    void *buffer = NULL;
    bytes = kb_fs_subsystem_iov_iter_limit(iter, bytes, &buffer);
    if (addr == NULL || buffer == NULL || bytes == 0) {
        return 0;
    }
    memcpy(buffer, addr, bytes);
    kb_fs_subsystem_iov_iter_advance(iter, bytes);
    return bytes;
}

size_t kb_fs_subsystem_copy_from_iter(void *addr, size_t bytes, void *iter)
{
    void *buffer = NULL;
    bytes = kb_fs_subsystem_iov_iter_limit(iter, bytes, &buffer);
    if (addr == NULL || buffer == NULL || bytes == 0) {
        return 0;
    }
    memcpy(addr, buffer, bytes);
    kb_fs_subsystem_iov_iter_advance(iter, bytes);
    return bytes;
}

unsigned long kb_fs_subsystem_iov_iter_alignment(const void *iter)
{
    if (iter == NULL) {
        return 0;
    }
    const uint64_t count = read_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET);
    void *buffer = read_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET);
    return (unsigned long)((uintptr_t)buffer | count);
}

void kb_fs_subsystem_iov_iter_revert(void *iter, size_t bytes)
{
    if (iter == NULL || bytes == 0) {
        return;
    }
    uint64_t count = 0;
    uint64_t capacity = 0;
    void *buffer = NULL;
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    memcpy(&buffer, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, sizeof(buffer));
    memcpy(&capacity, (const uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, sizeof(capacity));
    buffer = (uint8_t *)buffer - bytes;
    count += bytes;
    capacity += bytes;
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, &count, sizeof(count));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_OFFSET, &buffer, sizeof(buffer));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, &capacity, sizeof(capacity));
}

long kb_fs_subsystem_generic_perform_write(void *kiocb, void *iter)
{
    if (kiocb == NULL || iter == NULL) {
        return -22;
    }
    void *file = read_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET);
    if (file == NULL) {
        return -22;
    }
    void *mapping = read_pointer_field(file, KB_FS_NATIVE_FILE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -95;
    }
    void *a_ops = read_pointer_field(mapping, KB_FS_ADDRESS_SPACE_AOPS_OFFSET);
    if (a_ops == NULL) {
        return -95;
    }
    void *write_begin_op = read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_WRITE_BEGIN_OFFSET);
    void *write_end_op = read_pointer_field(a_ops, KB_FS_ADDRESS_SPACE_OP_WRITE_END_OFFSET);
    if (write_begin_op == NULL || write_end_op == NULL) {
        return -95;
    }
    int (*write_begin_fn)(void *, void *, int64_t, unsigned int, void **, void **) = NULL;
    int (*write_end_fn)(void *, void *, int64_t, unsigned int, unsigned int, void *, void *) = NULL;
    memcpy(&write_begin_fn, &write_begin_op, sizeof(write_begin_fn));
    memcpy(&write_end_fn, &write_end_op, sizeof(write_end_fn));
    uint64_t pos = 0;
    uint64_t count = 0;
    memcpy(&pos, (const uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, sizeof(pos));
    memcpy(&count, (const uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, sizeof(count));
    uint64_t written = 0;
    while (count != 0) {
        const size_t page_offset = (size_t)(pos & (KB_FS_PAGE_SIZE - 1u));
        unsigned int bytes = (unsigned int)(KB_FS_PAGE_SIZE - page_offset);
        if ((uint64_t)bytes > count) {
            bytes = (unsigned int)count;
        }
        void *folio = NULL;
        void *fsdata = NULL;
        FS_HOTPATH_BEGIN(write_begin_start);
        /* generic_perform_write is entered by the Linux module through its
         * shim while that module's GS context is still active.  Returning to
         * its a_ops must therefore not perform another GS syscall pair. */
        int status = write_begin_fn(file, mapping, (int64_t)pos, bytes, &folio, &fsdata);
        FS_HOTPATH_END(write_begin, write_begin_start);
        if (status < 0) {
            return written != 0 ? (long)written : status;
        }
        void *payload = folio_page_payload(folio);
        const size_t copied = payload == NULL ? 0 :
            kb_fs_subsystem_copy_from_iter((uint8_t *)payload + page_offset, bytes, iter);
        FS_HOTPATH_BEGIN(write_end_start);
        status = write_end_fn(file, mapping, (int64_t)pos, bytes, (unsigned int)copied, folio, fsdata);
        FS_HOTPATH_END(write_end, write_end_start);
        if (status < 0) {
            kb_fs_subsystem_iov_iter_revert(iter, copied);
            return written != 0 ? (long)written : status;
        }
        if ((size_t)status < copied) {
            kb_fs_subsystem_iov_iter_revert(iter, copied - (size_t)status);
        }
        if (status == 0) {
            break;
        }
        pos += (uint64_t)status;
        written += (uint64_t)status;
        count -= (uint64_t)status;
    }
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    return (long)written;
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
    kb_fs_subsystem_make_bad_inode(inode);
    kb_fs_subsystem_unlock_new_inode(inode);
    kb_fs_subsystem_iput(inode);
}

void kb_fs_subsystem_make_bad_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    write_u32_field(inode, KB_FS_INODE_MODE_OFFSET, 0100000u);
    write_pointer_field(inode, KB_FS_INODE_OP_OFFSET, bad_inode_operations);
    write_pointer_field(inode, KB_FS_INODE_FILE_OP_OFFSET, bad_file_operations);
}

int kb_fs_subsystem_is_bad_inode(void *inode)
{
    return !low_or_err_pointer(inode) &&
        read_pointer_field(inode, KB_FS_INODE_OP_OFFSET) == bad_inode_operations;
}

void kb_fs_subsystem_unlock_new_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET, sizeof(state));
    state &= ~(uint64_t)(KB_FS_INODE_STATE_NEW |
                         KB_FS_INODE_STATE_CREATING);
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

void kb_fs_subsystem_clear_inode(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return;
    }
    uint64_t state = 0;
    memcpy(&state, (const uint8_t *)inode + KB_FS_INODE_STATE_OFFSET, sizeof(state));
    state = KB_FS_INODE_STATE_FREEING | KB_FS_INODE_STATE_CLEAR;
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

typedef int (*kb_fs_param_type_fn_t)(
    void *log,
    const void *spec,
    void *parameter,
    void *result);

typedef struct kb_fs_parameter_spec_abi {
    const char *name;
    kb_fs_param_type_fn_t type;
    uint8_t option;
    uint8_t reserved0;
    uint16_t flags;
    uint32_t reserved1;
    const void *data;
} kb_fs_parameter_spec_abi_t;

typedef struct kb_fs_parameter_abi {
    const char *key;
    uint8_t type;
    uint8_t reserved0[7];
    union {
        char *string;
        void *blob;
        void *name;
        void *file;
    } value;
    size_t size;
    int dirfd;
    uint32_t reserved1;
} kb_fs_parameter_abi_t;

typedef struct kb_fs_parse_result_abi {
    uint8_t negated;
    uint8_t reserved[7];
    union {
        uint8_t boolean;
        int32_t signed_32;
        uint32_t unsigned_32;
        uint64_t unsigned_64;
    } value;
} kb_fs_parse_result_abi_t;

typedef struct kb_fs_constant_abi {
    const char *name;
    int value;
    uint32_t reserved;
} kb_fs_constant_abi_t;

_Static_assert(sizeof(kb_fs_parameter_spec_abi_t) == 32,
    "Linux 6.12 fs_parameter_spec ABI changed");
_Static_assert(offsetof(kb_fs_parameter_spec_abi_t, flags) == 18,
    "Linux 6.12 fs_parameter_spec flags offset changed");
_Static_assert(offsetof(kb_fs_parameter_spec_abi_t, data) == 24,
    "Linux 6.12 fs_parameter_spec data offset changed");
_Static_assert(sizeof(kb_fs_parameter_abi_t) == 40,
    "Linux 6.12 fs_parameter ABI changed");
_Static_assert(offsetof(kb_fs_parameter_abi_t, value) == 16,
    "Linux 6.12 fs_parameter value offset changed");
_Static_assert(sizeof(kb_fs_parse_result_abi_t) == 16,
    "Linux 6.12 fs_parse_result ABI changed");

enum {
    KB_FS_VALUE_IS_FLAG = 1,
    KB_FS_VALUE_IS_STRING = 2,
    KB_FS_VALUE_IS_FILENAME = 4,
    KB_FS_PARAM_NEG_WITH_NO = 0x0002,
    KB_FS_PARAM_CAN_BE_EMPTY = 0x0004,
    KB_FS_ENOPARAM = 519,
};

static int kb_fs_parse_unsigned_integer(
    const char *text,
    unsigned int base,
    uint64_t maximum,
    uint64_t *out_value)
{
    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == ' ' || text[0] == '\t' ||
        text[0] == '\n' || (base != 0 && (base < 2 || base > 16)))
    {
        return -22;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, (int)base);
    if (errno == ERANGE || end == text || end == NULL || *end != '\0' ||
        parsed > maximum)
    {
        return -22;
    }
    *out_value = (uint64_t)parsed;
    return 0;
}

static int kb_fs_parameter_string(
    const kb_fs_parameter_spec_abi_t *spec,
    const kb_fs_parameter_abi_t *parameter,
    const char **out_string)
{
    if (spec == NULL || parameter == NULL || out_string == NULL ||
        parameter->type != KB_FS_VALUE_IS_STRING ||
        parameter->value.string == NULL)
    {
        return -22;
    }
    if (parameter->value.string[0] == '\0' &&
        (spec->flags & KB_FS_PARAM_CAN_BE_EMPTY) == 0)
    {
        return -22;
    }
    *out_string = parameter->value.string;
    return 0;
}

int kb_fs_subsystem_fs_param_is_u32(
    void *log,
    const void *raw_spec,
    void *raw_parameter,
    void *raw_result)
{
    (void)log;
    const kb_fs_parameter_spec_abi_t *spec = raw_spec;
    const kb_fs_parameter_abi_t *parameter = raw_parameter;
    kb_fs_parse_result_abi_t *result = raw_result;
    const char *text = NULL;
    if (result == NULL ||
        kb_fs_parameter_string(spec, parameter, &text) != 0)
    {
        return -22;
    }
    if (text[0] == '\0') {
        return 0;
    }
    uint64_t value = 0;
    const unsigned int base = (unsigned int)(uintptr_t)spec->data;
    if (kb_fs_parse_unsigned_integer(text, base, UINT32_MAX, &value) != 0) {
        return -22;
    }
    result->value.unsigned_32 = (uint32_t)value;
    return 0;
}

int kb_fs_subsystem_fs_param_is_s32(
    void *log,
    const void *raw_spec,
    void *raw_parameter,
    void *raw_result)
{
    (void)log;
    const kb_fs_parameter_spec_abi_t *spec = raw_spec;
    const kb_fs_parameter_abi_t *parameter = raw_parameter;
    kb_fs_parse_result_abi_t *result = raw_result;
    const char *text = NULL;
    if (result == NULL ||
        kb_fs_parameter_string(spec, parameter, &text) != 0)
    {
        return -22;
    }
    if (text[0] == '\0') {
        return 0;
    }
    if (text[0] == ' ' || text[0] == '\t' || text[0] == '\n') {
        return -22;
    }
    errno = 0;
    char *end = NULL;
    const long long parsed = strtoll(text, &end, 0);
    if (errno == ERANGE || end == text || end == NULL || *end != '\0' ||
        parsed < INT32_MIN || parsed > INT32_MAX)
    {
        return -22;
    }
    result->value.signed_32 = (int32_t)parsed;
    return 0;
}

int kb_fs_subsystem_fs_param_is_enum(
    void *log,
    const void *raw_spec,
    void *raw_parameter,
    void *raw_result)
{
    (void)log;
    const kb_fs_parameter_spec_abi_t *spec = raw_spec;
    const kb_fs_parameter_abi_t *parameter = raw_parameter;
    kb_fs_parse_result_abi_t *result = raw_result;
    const char *text = NULL;
    if (result == NULL || spec == NULL || spec->data == NULL ||
        kb_fs_parameter_string(spec, parameter, &text) != 0)
    {
        return -22;
    }
    if (text[0] == '\0') {
        return 0;
    }
    const kb_fs_constant_abi_t *table = spec->data;
    for (size_t index = 0; index < 256; ++index) {
        if (table[index].name == NULL) {
            return -22;
        }
        if (strcmp(table[index].name, text) == 0) {
            result->value.unsigned_32 = (uint32_t)table[index].value;
            return 0;
        }
    }
    return -22;
}

int kb_fs_subsystem_fs_param_is_string(
    void *log,
    const void *raw_spec,
    void *raw_parameter,
    void *raw_result)
{
    (void)log;
    (void)raw_result;
    const char *text = NULL;
    return kb_fs_parameter_string(raw_spec, raw_parameter, &text);
}

int kb_fs_subsystem_fs_param_is_blockdev(
    void *log,
    const void *spec,
    void *parameter,
    void *result)
{
    (void)log;
    (void)spec;
    (void)parameter;
    (void)result;
    return 0;
}

int kb_fs_subsystem_fs_param_is_uid(
    void *log,
    const void *spec,
    void *parameter,
    void *result)
{
    return kb_fs_subsystem_fs_param_is_u32(log, spec, parameter, result);
}

int kb_fs_subsystem_fs_param_is_gid(
    void *log,
    const void *spec,
    void *parameter,
    void *result)
{
    return kb_fs_subsystem_fs_param_is_u32(log, spec, parameter, result);
}

int kb_fs_subsystem_fs_parse(
    void *log,
    const void *raw_description,
    void *raw_parameter,
    void *raw_result)
{
    const kb_fs_parameter_spec_abi_t *description = raw_description;
    const kb_fs_parameter_abi_t *parameter = raw_parameter;
    kb_fs_parse_result_abi_t *result = raw_result;
    if (description == NULL || parameter == NULL || result == NULL ||
        parameter->key == NULL)
    {
        return -22;
    }
    memset(result, 0, sizeof(*result));
    const int want_flag = parameter->type == KB_FS_VALUE_IS_FLAG;
    const kb_fs_parameter_spec_abi_t *match = NULL;
    const kb_fs_parameter_spec_abi_t *type_mismatch = NULL;
    for (size_t index = 0; index < 256; ++index) {
        const kb_fs_parameter_spec_abi_t *spec = &description[index];
        if (spec->name == NULL) {
            break;
        }
        if (strcmp(spec->name, parameter->key) != 0) {
            continue;
        }
        if ((spec->type == NULL) == want_flag) {
            match = spec;
            break;
        }
        type_mismatch = spec;
    }
    if (match == NULL && want_flag && parameter->key[0] == 'n' &&
        parameter->key[1] == 'o' && parameter->key[2] != '\0')
    {
        const char *positive_key = parameter->key + 2;
        for (size_t index = 0; index < 256; ++index) {
            const kb_fs_parameter_spec_abi_t *spec = &description[index];
            if (spec->name == NULL) {
                break;
            }
            if ((spec->flags & KB_FS_PARAM_NEG_WITH_NO) != 0 &&
                strcmp(spec->name, positive_key) == 0)
            {
                match = spec;
                result->negated = 1;
                break;
            }
        }
    }
    if (match == NULL) {
        match = type_mismatch;
    }
    if (match == NULL) {
        return -KB_FS_ENOPARAM;
    }
    if (match->type == NULL) {
        if (!want_flag) {
            return -22;
        }
        result->value.boolean = result->negated == 0;
    } else {
        const int status = match->type(log, match, raw_parameter, raw_result);
        if (status != 0) {
            return status;
        }
    }
    return match->option;
}

int kb_fs_subsystem_fs_lookup_param(
    void *fs_context,
    void *raw_parameter,
    int want_block_device,
    unsigned int flags,
    void *path)
{
    (void)fs_context;
    (void)want_block_device;
    (void)flags;
    const kb_fs_parameter_abi_t *parameter = raw_parameter;
    if (parameter == NULL || path == NULL) {
        return -22;
    }
    memset(path, 0, 2 * sizeof(void *));
    if (parameter->type != KB_FS_VALUE_IS_STRING &&
        parameter->type != KB_FS_VALUE_IS_FILENAME)
    {
        return -22;
    }
    /* Kobox does not expose a mount-time namespace containing block-special
     * dentries yet.  External journal_path must therefore fail closed instead
     * of manufacturing a path and letting ext4 dereference a fake inode. */
    return -95;
}

int kb_fs_subsystem_simple_statfs(void *dentry, void *buffer)
{
    if (low_or_err_pointer(dentry) || buffer == NULL) {
        return -22;
    }
    void *super_block = read_pointer_field(dentry, KB_FS_DENTRY_SB_OFFSET);
    if (low_or_err_pointer(super_block)) {
        return -22;
    }
    const uint32_t device = read_u32_field(
        super_block,
        KB_FS_SUPER_BLOCK_DEV_OFFSET);
    const uint32_t major = device >> 20;
    const uint32_t minor = device & ((1u << 20) - 1u);
    const uint64_t encoded_device =
        (uint64_t)((minor & 0xffu) | (major << 8) |
            ((minor & ~0xffu) << 12));
    write_u64_field(
        buffer,
        0,
        read_u64_field(super_block, KB_FS_SUPER_BLOCK_MAGIC_OFFSET));
    write_u64_field(buffer, 8, KB_FS_PAGE_SIZE);
    write_u32_field(buffer, 56, (uint32_t)encoded_device);
    write_u32_field(buffer, 60, (uint32_t)(encoded_device >> 32));
    write_u64_field(buffer, 64, 255u);
    return 0;
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
        write_pointer_field(bdev, KB_FS_BDEV_DISK_OFFSET, disk);
        write_pointer_field(bdev, KB_FS_BDEV_QUEUE_OFFSET, queue);
        write_pointer_field(bdev, KB_FS_BDEV_MAPPING_OFFSET, bdev_mapping);
        write_pointer_field(disk, KB_FS_GENDISK_PART0_OFFSET, bdev);
        write_pointer_field(bdev_inode, KB_FS_INODE_SB_OFFSET, kb_fs_subsystem_blockdev_superblock);
        write_pointer_field(bdev_inode, KB_FS_INODE_MAPPING_OFFSET, bdev_mapping);
        write_pointer_field(bdev_mapping, KB_FS_ADDRESS_SPACE_HOST_OFFSET, bdev_inode);
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
        mount_probe_mounted = last_mount_path_probe.fill_super_result == 0;
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

static void kb_fs_subsystem_release_fs_context(void *fs_context, void *operations)
{
    void *free_operation = operations == NULL ? NULL :
        read_pointer_field(operations, KB_FS_CONTEXT_OPS_FREE_OFFSET);
    if (free_operation == NULL) {
        return;
    }
    void (*free_fn)(void *) = NULL;
    memcpy(&free_fn, &free_operation, sizeof(free_fn));
    unsigned long old_gs = 0;
    const int has_gs = kb_fs_enter_ext4_call(free_operation, &old_gs);
    free_fn(fs_context);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
}

static int kb_fs_subsystem_probe_registered_mount_path_parameter(
    const char *name,
    const char *parameter_key,
    const char *parameter_value,
    kb_fs_mount_path_probe_t *out_probe)
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
    if (ops == NULL) {
        *out_probe = last_mount_path_probe;
        free(fs_context);
        last_mount_path_probe.fs_context = NULL;
        return -95;
    }
    if (parameter_key != NULL) {
        void *parse_operation = ops == NULL ? NULL : read_pointer_field(
            ops,
            KB_FS_CONTEXT_OPS_PARSE_PARAM_OFFSET);
        if (parse_operation == NULL) {
            *out_probe = last_mount_path_probe;
            kb_fs_subsystem_release_fs_context(fs_context, ops);
            free(fs_context);
            last_mount_path_probe.fs_context = NULL;
            return -95;
        }
        kb_fs_parameter_abi_t parameter = {
            .key = parameter_key,
            .type = parameter_value == NULL ?
                KB_FS_VALUE_IS_FLAG : KB_FS_VALUE_IS_STRING,
            .value.string = (char *)parameter_value,
            .size = parameter_value == NULL ? 0 : strlen(parameter_value),
            .dirfd = -100,
        };
        int (*parse_fn)(void *, void *) = NULL;
        memcpy(&parse_fn, &parse_operation, sizeof(parse_fn));
        unsigned long old_gs = 0;
        const int has_gs = kb_fs_enter_ext4_call(parse_operation, &old_gs);
        const int parse_result = parse_fn(fs_context, &parameter);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (parse_result != 0) {
            *out_probe = last_mount_path_probe;
            kb_fs_subsystem_release_fs_context(fs_context, ops);
            free(fs_context);
            last_mount_path_probe.fs_context = NULL;
            return parse_result;
        }
    }
    int (*get_tree)(void *) =
        (int (*)(void *))read_pointer_field(ops, KB_FS_CONTEXT_OPS_GET_TREE_OFFSET);
    last_mount_path_probe.get_tree = (void *)get_tree;
    if (get_tree == NULL) {
        *out_probe = last_mount_path_probe;
        kb_fs_subsystem_release_fs_context(fs_context, ops);
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
    kb_fs_subsystem_release_fs_context(fs_context, ops);
    free(fs_context);
    last_mount_path_probe.fs_context = NULL;
    return get_tree_result;
}

int kb_fs_subsystem_probe_registered_mount_path(
    const char *name,
    kb_fs_mount_path_probe_t *out_probe)
{
    return kb_fs_subsystem_probe_registered_mount_path_parameter(
        name,
        NULL,
        NULL,
        out_probe);
}

int kb_fs_subsystem_mount_registered_root(const char *name, kb_fs_mount_result_t *out_mount)
{
    return kb_fs_subsystem_probe_registered_mount_path(name, out_mount);
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

int kb_fs_subsystem_fscrypt_prepare_symlink(
    void *dir,
    const char *target,
    unsigned int target_length,
    unsigned int max_length,
    void *disk_link)
{
    (void)dir;
    if (target == NULL || disk_link == NULL || target_length == UINT_MAX) {
        return -22;
    }
    const unsigned int stored_length = target_length + 1u;
    if (stored_length > max_length) {
        return -36;
    }
    write_pointer_field(disk_link, 0, (void *)(uintptr_t)target);
    write_u32_field(disk_link, sizeof(void *), stored_length);
    return 0;
}

static uint8_t ext4_ldlike_expected_byte(uint64_t offset)
{
    const uint64_t line = offset / 17u;
    const uint64_t col = offset % 17u;
    uint32_t value = 0;
    unsigned shift = 0;

    if (col == 16u) {
        return (uint8_t)'\n';
    }
    if (col < 8u) {
        value = (uint32_t)line;
        shift = (unsigned)((7u - col) * 4u);
    } else {
        value = (uint32_t)(65535u - (uint32_t)line);
        shift = (unsigned)((15u - col) * 4u);
    }
    const uint8_t nibble = (uint8_t)((value >> shift) & 0xfu);
    return (uint8_t)(nibble < 10u ? ('0' + nibble) : ('a' + (nibble - 10u)));
}

static int ext4_ldlike_verify_pattern(const uint8_t *bytes, uint64_t offset, size_t length)
{
    if (bytes == NULL) {
        return -22;
    }
    for (size_t i = 0; i < length; ++i) {
        const uint8_t expected = ext4_ldlike_expected_byte(offset + (uint64_t)i);
        if (bytes[i] != expected) {
            fprintf(stderr,
                "kobox-ext4-smoke: ldlike byte mismatch offset=%llu got=0x%02x expected=0x%02x\n",
                (unsigned long long)(offset + (uint64_t)i),
                bytes[i],
                expected);
            return -5;
        }
    }
    return 0;
}

typedef struct kb_fs_ext4_smoke_dir_context {
    int (*actor)(
        void *context,
        const char *name,
        int name_length,
        int64_t position,
        uint64_t inode_number,
        unsigned int d_type);
    int64_t position;
    const char *target;
    size_t target_length;
    int found;
} kb_fs_ext4_smoke_dir_context_t;

static int kb_fs_ext4_smoke_dir_actor(
    void *context,
    const char *name,
    int name_length,
    int64_t position,
    uint64_t inode_number,
    unsigned int d_type)
{
    (void)position;
    (void)inode_number;
    (void)d_type;
    kb_fs_ext4_smoke_dir_context_t *scan =
        (kb_fs_ext4_smoke_dir_context_t *)context;
    if (scan == NULL || name == NULL || name_length < 0) {
        return 0;
    }
    if ((size_t)name_length == scan->target_length &&
        memcmp(name, scan->target, scan->target_length) == 0)
    {
        scan->found = 1;
    }
    return 1;
}

static int kb_fs_ext4_smoke_dir_contains(
    void *inode,
    void *dentry,
    const char *target,
    int *out_found)
{
    if (low_or_err_pointer(inode) || dentry == NULL || target == NULL ||
        out_found == NULL)
    {
        return -22;
    }
    *out_found = 0;
    void *file_operations =
        read_pointer_field(inode, KB_FS_INODE_FILE_OP_OFFSET);
    if (file_operations == NULL) {
        return -95;
    }
    void *open_operation =
        read_pointer_field(file_operations, KB_FS_FILE_OP_OPEN_OFFSET);
    void *iterate_operation =
        read_pointer_field(file_operations, KB_FS_FILE_OP_ITERATE_SHARED_OFFSET);
    void *release_operation =
        read_pointer_field(file_operations, KB_FS_FILE_OP_RELEASE_OFFSET);
    if (open_operation == NULL || iterate_operation == NULL ||
        release_operation == NULL)
    {
        return -95;
    }

    uint8_t file[KB_FS_FAKE_INODE_BYTES];
    memset(file, 0, sizeof(file));
    write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, file_operations);
    write_pointer_field(
        file,
        KB_FS_NATIVE_FILE_MAPPING_OFFSET,
        read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET));
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    write_pointer_field(file, KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET, dentry);
    write_u64_field(file, KB_FS_NATIVE_FILE_POSITION_OFFSET, 0);
    file_ra_state_init(
        file + KB_FS_NATIVE_FILE_RA_OFFSET);

    int (*open_fn)(void *, void *) = NULL;
    int (*iterate_fn)(void *, void *) = NULL;
    int (*release_fn)(void *, void *) = NULL;
    memcpy(&open_fn, &open_operation, sizeof(open_fn));
    memcpy(&iterate_fn, &iterate_operation, sizeof(iterate_fn));
    memcpy(&release_fn, &release_operation, sizeof(release_fn));

    unsigned long old_gs = 0;
    int has_gs = kb_fs_enter_ext4_call(open_operation, &old_gs);
    int status = open_fn(inode, file);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (status != 0) {
        return status;
    }

    kb_fs_ext4_smoke_dir_context_t context = {
        .actor = kb_fs_ext4_smoke_dir_actor,
        .position = 0,
        .target = target,
        .target_length = strlen(target),
        .found = 0,
    };
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(iterate_operation, &old_gs);
    status = iterate_fn(file, &context);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(release_operation, &old_gs);
    const int release_status = release_fn(inode, file);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (status == 0) {
        status = release_status;
    }
    if (status == 0) {
        *out_found = context.found;
    }
    return status;
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
        status = kb_fs_subsystem_probe_registered_mount_path_parameter(
            "ext4",
            "errors",
            "remount-ro",
            &mount);
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

    uint8_t smoke_inode_dentry[KB_FS_FAKE_DENTRY_BYTES];
    kb_fs_prepare_named_dentry(
        smoke_inode_dentry,
        mount.root_dentry,
        inode,
        mount.super_block,
        "kobox-smoke.txt");

    /* Validate ext4's real export_operations through the generic Linux file
     * handle helpers.  This catches fake-success encoders (type 0 / empty
     * handles) and decoders that return NULL instead of a referenced alias. */
    enum {
        KB_FS_SUPER_EXPORT_OP_OFFSET = 0x48,
        KB_FS_EXPORT_ENCODE_FH_OFFSET = 0x00,
        KB_FS_EXPORT_FH_TO_DENTRY_OFFSET = 0x08,
        KB_FS_EXPORT_FH_TO_PARENT_OFFSET = 0x10,
    };
    void *export_operations = read_pointer_field(
        mount.super_block,
        KB_FS_SUPER_EXPORT_OP_OFFSET);
    void *encode_fh_operation = export_operations == NULL ? NULL :
        read_pointer_field(export_operations, KB_FS_EXPORT_ENCODE_FH_OFFSET);
    void *fh_to_dentry_operation = export_operations == NULL ? NULL :
        read_pointer_field(export_operations, KB_FS_EXPORT_FH_TO_DENTRY_OFFSET);
    void *fh_to_parent_operation = export_operations == NULL ? NULL :
        read_pointer_field(export_operations, KB_FS_EXPORT_FH_TO_PARENT_OFFSET);
    void *export_parent_inode = read_pointer_field(
        mount.root_dentry,
        KB_FS_DENTRY_INODE_OFFSET);
    if (encode_fh_operation == NULL || fh_to_dentry_operation == NULL ||
        fh_to_parent_operation == NULL || low_or_err_pointer(export_parent_inode))
    {
        fprintf(stderr,
            "kobox-ext4-smoke: export operations missing ops=%p encode=%p "
            "dentry=%p parent=%p parent_inode=%p\n",
            export_operations,
            encode_fh_operation,
            fh_to_dentry_operation,
            fh_to_parent_operation,
            export_parent_inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }
    uint32_t export_handle[4] = {0};
    int export_handle_length = 4;
    int (*encode_fh_fn)(void *, uint32_t *, int *, void *) = NULL;
    memcpy(&encode_fh_fn, &encode_fh_operation, sizeof(encode_fh_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(encode_fh_operation, &old_gs);
    const int export_handle_type = encode_fh_fn(
        inode,
        export_handle,
        &export_handle_length,
        export_parent_inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (export_handle_type != 2 || export_handle_length != 4 ||
        export_handle[0] != (uint32_t)inode_number)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: export encode failed type=%d len=%d ino=%u\n",
            export_handle_type,
            export_handle_length,
            export_handle[0]);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    void *(*decode_fh_fn)(void *, void *, int, int) = NULL;
    memcpy(&decode_fh_fn, &fh_to_dentry_operation, sizeof(decode_fh_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(fh_to_dentry_operation, &old_gs);
    void *decoded_dentry = decode_fh_fn(
        mount.super_block,
        export_handle,
        export_handle_length,
        export_handle_type);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (low_or_err_pointer(decoded_dentry) ||
        read_pointer_field(decoded_dentry, KB_FS_DENTRY_INODE_OFFSET) != inode)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: export dentry decode failed result=%p\n",
            decoded_dentry);
        if (!low_or_err_pointer(decoded_dentry)) {
            kb_fs_subsystem_dput(decoded_dentry);
        }
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    kb_fs_subsystem_dput(decoded_dentry);

    memcpy(&decode_fh_fn, &fh_to_parent_operation, sizeof(decode_fh_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(fh_to_parent_operation, &old_gs);
    void *decoded_parent = decode_fh_fn(
        mount.super_block,
        export_handle,
        export_handle_length,
        export_handle_type);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (low_or_err_pointer(decoded_parent) ||
        read_pointer_field(decoded_parent, KB_FS_DENTRY_INODE_OFFSET) !=
            export_parent_inode)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: export parent decode failed result=%p\n",
            decoded_parent);
        if (!low_or_err_pointer(decoded_parent)) {
            kb_fs_subsystem_dput(decoded_parent);
        }
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    kb_fs_subsystem_dput(decoded_parent);
    fprintf(stderr,
        "kobox-ext4-smoke: native export handle ok type=%d len=%d\n",
        export_handle_type,
        export_handle_length);

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
    write_pointer_field(
        file,
        KB_FS_NATIVE_FILE_MAPPING_OFFSET,
        read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET));
    write_pointer_field(file, KB_FS_FILE_INODE_OFFSET, inode);
    file_ra_state_init(file + KB_FS_NATIVE_FILE_RA_OFFSET);
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

    enum {
        KB_FS_FILE_OP_MMAP_OFFSET = 0x60,
        KB_FS_VMA_OPS_OFFSET = 0x70,
        KB_FS_VM_OP_FAULT_OFFSET = 0x28,
        KB_FS_VM_OP_PAGE_MKWRITE_OFFSET = 0x48,
        KB_FS_VM_FAULT_LOCKED = 0x200,
        KB_FS_VM_FAULT_ERROR_MASK = 0x3u,
    };
    void *mmap_file_operations = read_pointer_field(
        inode,
        KB_FS_INODE_FILE_OP_OFFSET);
    void *mmap_operation = mmap_file_operations == NULL ? NULL :
        read_pointer_field(mmap_file_operations, KB_FS_FILE_OP_MMAP_OFFSET);
    uint8_t mmap_vma[192] = {0};
    uint8_t mmap_fault[112] = {0};
    write_pointer_field(
        file,
        KB_FS_NATIVE_FILE_OP_OFFSET,
        mmap_file_operations);
    write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET, KB_FS_FMODE_READ);
    write_pointer_field(
        file,
        KB_FS_NATIVE_FILE_PATH_MNT_OFFSET,
        mount.root_vfsmount);
    write_pointer_field(
        file,
        KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET,
        smoke_inode_dentry);
    write_pointer_field(mmap_vma, KB_FS_VMA_FILE_OFFSET, file);
    if (mmap_operation == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: mmap operation missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }
    int (*mmap_fn)(void *, void *) = NULL;
    memcpy(&mmap_fn, &mmap_operation, sizeof(mmap_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(mmap_operation, &old_gs);
    const int mmap_status = mmap_fn(file, mmap_vma);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *vm_operations = read_pointer_field(
        mmap_vma,
        KB_FS_VMA_OPS_OFFSET);
    void *fault_operation = vm_operations == NULL ? NULL :
        read_pointer_field(vm_operations, KB_FS_VM_OP_FAULT_OFFSET);
    void *page_mkwrite_operation = vm_operations == NULL ? NULL :
        read_pointer_field(vm_operations, KB_FS_VM_OP_PAGE_MKWRITE_OFFSET);
    if (mmap_status != 0 || fault_operation == NULL ||
        page_mkwrite_operation == NULL)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: native mmap setup failed status=%d "
            "vm_ops=%p page_mkwrite=%p\n",
            mmap_status,
            vm_operations,
            page_mkwrite_operation);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return mmap_status != 0 ? mmap_status : -5;
    }
    write_pointer_field(mmap_fault, 0, mmap_vma);
    unsigned int (*fault_fn)(void *) = NULL;
    memcpy(&fault_fn, &fault_operation, sizeof(fault_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(fault_operation, &old_gs);
    const unsigned int fault_status = fault_fn(mmap_fault);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *mmap_page = read_pointer_field(
        mmap_fault,
        KB_FS_VM_FAULT_PAGE_OFFSET);
    void *mmap_payload = low_or_err_pointer(mmap_page) ? NULL :
        folio_page_payload(mmap_page);
    if ((fault_status & KB_FS_VM_FAULT_ERROR_MASK) != 0 ||
        (fault_status & KB_FS_VM_FAULT_LOCKED) == 0 ||
        mmap_payload == NULL ||
        memcmp(mmap_payload, original, (size_t)read_result) != 0)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: native mmap fault failed status=0x%x page=%p\n",
            fault_status,
            mmap_page);
        if (!low_or_err_pointer(mmap_page)) {
            kb_fs_subsystem_folio_unlock(mmap_page);
            kb_fs_subsystem_folio_put(mmap_page);
        }
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    kb_fs_subsystem_folio_unlock(mmap_page);
    kb_fs_subsystem_folio_put(mmap_page);
    fprintf(stderr,
        "kobox-ext4-smoke: native mmap fault ok bytes=%ld\n",
        read_result);

    enum {
        KB_FS_FILE_OP_LLSEEK_OFFSET = 0x10,
        KB_FS_SEEK_DATA = 3,
        KB_FS_SEEK_HOLE = 4,
    };
    void *file_operations = read_pointer_field(
        inode,
        KB_FS_INODE_FILE_OP_OFFSET);
    void *llseek_operation = file_operations == NULL ? NULL :
        read_pointer_field(file_operations, KB_FS_FILE_OP_LLSEEK_OFFSET);
    if (llseek_operation == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: llseek operation missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }
    write_pointer_field(file, KB_FS_NATIVE_FILE_OP_OFFSET, file_operations);
    int64_t (*llseek_fn)(void *, int64_t, int) = NULL;
    memcpy(&llseek_fn, &llseek_operation, sizeof(llseek_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(llseek_operation, &old_gs);
    const int64_t seek_data_result = llseek_fn(file, 0, KB_FS_SEEK_DATA);
    const int64_t seek_hole_result = llseek_fn(file, 0, KB_FS_SEEK_HOLE);
    const int64_t seek_eof_result = llseek_fn(
        file,
        (int64_t)original_size,
        KB_FS_SEEK_DATA);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (seek_data_result != 0 ||
        seek_hole_result != (int64_t)original_size ||
        seek_eof_result != -6)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: seek data/hole failed data=%lld hole=%lld "
            "eof=%lld size=%llu\n",
            (long long)seek_data_result,
            (long long)seek_hole_result,
            (long long)seek_eof_result,
            (unsigned long long)original_size);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    fprintf(stderr,
        "kobox-ext4-smoke: seek data/hole ok data=%lld hole=%lld eof=%lld\n",
        (long long)seek_data_result,
        (long long)seek_hole_result,
        (long long)seek_eof_result);

    enum {
        KB_FS_INODE_OP_FIEMAP_OFFSET = 0x80,
        KB_FS_INODE_OP_FILEATTR_GET_OFFSET = 0xb8,
        KB_FS_EXT4_SMOKE_FIEMAP_EXTENTS = 8,
        KB_FS_EXT4_SMOKE_FIEMAP_EXTENT_BYTES = 56,
    };
    void *inode_operations = read_pointer_field(inode, KB_FS_INODE_OP_OFFSET);
    void *fiemap_operation = inode_operations == NULL ? NULL :
        read_pointer_field(inode_operations, KB_FS_INODE_OP_FIEMAP_OFFSET);
    if (fiemap_operation == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: fiemap operation missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }
    uint8_t fiemap_info[24] = {0};
    uint8_t fiemap_extents[
        KB_FS_EXT4_SMOKE_FIEMAP_EXTENTS *
        KB_FS_EXT4_SMOKE_FIEMAP_EXTENT_BYTES] = {0};
    write_u32_field(fiemap_info, 0, 1u);
    write_u32_field(fiemap_info, 8, KB_FS_EXT4_SMOKE_FIEMAP_EXTENTS);
    write_pointer_field(fiemap_info, 16, fiemap_extents);
    int (*fiemap_fn)(void *, void *, uint64_t, uint64_t) = NULL;
    memcpy(&fiemap_fn, &fiemap_operation, sizeof(fiemap_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(fiemap_operation, &old_gs);
    const int fiemap_result = fiemap_fn(inode, fiemap_info, 0, UINT64_MAX);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    const uint32_t mapped_extents = read_u32_field(fiemap_info, 4);
    const uint64_t first_logical = read_u64_field(fiemap_extents, 0);
    const uint64_t first_physical = read_u64_field(fiemap_extents, 8);
    const uint64_t first_length = read_u64_field(fiemap_extents, 16);
    if (fiemap_result != 0 || mapped_extents == 0 || first_logical != 0 ||
        first_physical == 0 || first_length == 0)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: fiemap failed result=%d mapped=%u "
            "logical=%llu physical=%llu length=%llu\n",
            fiemap_result,
            mapped_extents,
            (unsigned long long)first_logical,
            (unsigned long long)first_physical,
            (unsigned long long)first_length);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return fiemap_result != 0 ? fiemap_result : -5;
    }
    fprintf(stderr,
        "kobox-ext4-smoke: fiemap ok mapped=%u physical=%llu length=%llu\n",
        mapped_extents,
        (unsigned long long)first_physical,
        (unsigned long long)first_length);

    void *fileattr_get_operation = read_pointer_field(
        inode_operations,
        KB_FS_INODE_OP_FILEATTR_GET_OFFSET);
    if (fileattr_get_operation == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: fileattr_get operation missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }
    uint8_t fileattr[28];
    memset(fileattr, 0xa5, sizeof(fileattr));
    int (*fileattr_get_fn)(void *, void *) = NULL;
    memcpy(&fileattr_get_fn, &fileattr_get_operation, sizeof(fileattr_get_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(fileattr_get_operation, &old_gs);
    const int fileattr_result = fileattr_get_fn(smoke_inode_dentry, fileattr);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    const uint32_t fileattr_flags = read_u32_field(fileattr, 0);
    const uint32_t fileattr_xflags = read_u32_field(fileattr, 4);
    if (fileattr_result != 0 || (read_u8_field(fileattr, 24) & 1u) == 0) {
        fprintf(stderr,
            "kobox-ext4-smoke: fileattr_get failed result=%d selectors=0x%x\n",
            fileattr_result,
            read_u8_field(fileattr, 24));
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return fileattr_result != 0 ? fileattr_result : -5;
    }
    fprintf(stderr,
        "kobox-ext4-smoke: fileattr_get ok flags=0x%x xflags=0x%x\n",
        fileattr_flags,
        fileattr_xflags);

    const char *metadata_only = getenv("KOBOX_EXT4_IMAGE_SMOKE_METADATA_ONLY");
    if (metadata_only != NULL && metadata_only[0] != '\0' && strcmp(metadata_only, "0") != 0) {
        typedef int (*ext4_setattr_fn)(void *, void *, void *);
        ext4_setattr_fn ext4_setattr =
            (ext4_setattr_fn)kb_module_lookup_exported_symbol("ext4_setattr");
        if (ext4_setattr == NULL) {
            fprintf(stderr,
                "kobox-ext4-smoke: metadata-only setattr symbol missing\n");
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -95;
        }
        kb_fs_ext4_smoke_ops_t metadata_ops;
        memset(&metadata_ops, 0, sizeof(metadata_ops));
        metadata_ops.setattr = ext4_setattr;

        const int64_t metadata_time_sec = 97445;
        const int64_t metadata_time_nsec = 0;
        int metadata_result = kb_fs_ext4_sync_inode_metadata(
            mount.super_block,
            inode,
            smoke_inode_dentry,
            KB_FS_MODE_REGULAR_0600,
            metadata_time_sec,
            metadata_time_nsec,
            metadata_time_sec,
            metadata_time_nsec,
            &metadata_ops,
            "metadata-only");
        if (metadata_result != 0) {
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return metadata_result;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: metadata-only ok mode=%o mtime=%lld\n",
            KB_FS_MODE_REGULAR_0600,
            (long long)metadata_time_sec);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return 0;
    }

    const char *read_only = getenv("KOBOX_EXT4_IMAGE_SMOKE_READ_ONLY");
    if (read_only != NULL && read_only[0] != '\0' && strcmp(read_only, "0") != 0) {
        goto ldlike_checks;
    }

    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    pos = 0;
    len = payload_len;
    write_pointer_field(kiocb, KB_FS_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_FS_KIOCB_POS_OFFSET, pos);
    write_u64_field(iter, KB_FS_IOV_ITER_COUNT_OFFSET, len);
    write_pointer_field(iter, KB_FS_IOV_ITER_BUFFER_OFFSET, (void *)payload);
    long write_result = kb_fs_ext4_smoke_perform_write_locked(inode, kiocb, iter);
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
    write_result = kb_fs_ext4_smoke_perform_write_locked(inode, kiocb, iter);
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

    /* Exercise ext4's real shared-write fault path.  The first fault above
     * only proved filemap_fault; this invokes ext4_page_mkwrite, which in
     * turn reaches block_page_mkwrite and the buffer-head mapping path. */
    write_u32_field(file, KB_FS_NATIVE_FILE_MODE_OFFSET,
        KB_FS_FMODE_READ | KB_FS_FMODE_WRITE);
    write_pointer_field(mmap_fault, KB_FS_VM_FAULT_PAGE_OFFSET, NULL);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(fault_operation, &old_gs);
    const unsigned int write_fault_status = fault_fn(mmap_fault);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *write_fault_page = read_pointer_field(
        mmap_fault,
        KB_FS_VM_FAULT_PAGE_OFFSET);
    void *write_fault_payload = low_or_err_pointer(write_fault_page) ? NULL :
        folio_page_payload(write_fault_page);
    if ((write_fault_status & KB_FS_VM_FAULT_ERROR_MASK) != 0 ||
        (write_fault_status & KB_FS_VM_FAULT_LOCKED) == 0 ||
        write_fault_payload == NULL)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: native mmap write fault failed "
            "status=0x%x page=%p\n",
            write_fault_status,
            write_fault_page);
        if (!low_or_err_pointer(write_fault_page)) {
            kb_fs_subsystem_folio_unlock(write_fault_page);
            kb_fs_subsystem_folio_put(write_fault_page);
        }
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    kb_fs_subsystem_folio_unlock(write_fault_page);

    unsigned int (*page_mkwrite_fn)(void *) = NULL;
    memcpy(&page_mkwrite_fn,
        &page_mkwrite_operation,
        sizeof(page_mkwrite_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(page_mkwrite_operation, &old_gs);
    const unsigned int page_mkwrite_status = page_mkwrite_fn(mmap_fault);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if ((page_mkwrite_status & KB_FS_VM_FAULT_ERROR_MASK) != 0 ||
        (page_mkwrite_status & KB_FS_VM_FAULT_LOCKED) == 0)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: native page_mkwrite failed status=0x%x\n",
            page_mkwrite_status);
        if ((page_mkwrite_status & KB_FS_VM_FAULT_LOCKED) != 0) {
            kb_fs_subsystem_folio_unlock(write_fault_page);
        }
        kb_fs_subsystem_folio_put(write_fault_page);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    ((uint8_t *)write_fault_payload)[7] = 'M';
    /* A zero return means the folio was already dirty, not failure.  The
     * preceding buffered write commonly leaves it in exactly that state. */
    (void)kb_fs_subsystem_folio_mark_dirty(write_fault_page);
    kb_fs_subsystem_folio_unlock(write_fault_page);
    kb_fs_subsystem_folio_put(write_fault_page);
    fprintf(stderr,
        "kobox-ext4-smoke: native shared mmap write ok offset=7 value=M\n");

    void *written_mapping = read_pointer_field(inode, KB_FS_INODE_MAPPING_OFFSET);
    const int data_writeback_result =
        kb_fs_subsystem_filemap_write_and_wait_range(written_mapping, 0, INT64_MAX);
    if (data_writeback_result != 0) {
        fprintf(stderr,
            "kobox-ext4-smoke: native data writeback failed result=%d\n",
            data_writeback_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return data_writeback_result;
    }
    fprintf(stderr, "kobox-ext4-smoke: native data writeback ok\n");

ldlike_checks:
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
        write_pointer_field(
            ldlike_file,
            KB_FS_NATIVE_FILE_MAPPING_OFFSET,
            read_pointer_field(ldlike_inode, KB_FS_INODE_MAPPING_OFFSET));
        write_pointer_field(ldlike_file, KB_FS_FILE_INODE_OFFSET, ldlike_inode);
        file_ra_state_init(ldlike_file + KB_FS_NATIVE_FILE_RA_OFFSET);
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
        write_pointer_field(ext4_file, KB_FS_NATIVE_FILE_MAPPING_OFFSET, ldlike_mapping);
        write_pointer_field(ext4_file, KB_FS_FILE_INODE_OFFSET, ldlike_inode);
        write_u32_field(ext4_file, KB_FS_NATIVE_FILE_MODE_OFFSET, KB_FS_FMODE_READ);
        file_ra_state_init(ext4_file + KB_FS_NATIVE_FILE_RA_OFFSET);

        kb_fs_subsystem_truncate_inode_pages_final(ldlike_mapping);
        kb_fs_storage_trace_t sequential_readahead_before;
        kb_fs_storage_trace_t sequential_readahead_after;
        kb_fs_storage_trace_snapshot(&sequential_readahead_before);

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
        kb_fs_storage_trace_snapshot(&sequential_readahead_after);
        if (sequential_readahead_after.readahead_aops_calls <=
            sequential_readahead_before.readahead_aops_calls)
        {
            fprintf(stderr,
                "kobox-ext4-smoke: ldlike native readahead was bypassed "
                "before=%llu after=%llu\n",
                (unsigned long long)
                    sequential_readahead_before.readahead_aops_calls,
                (unsigned long long)
                    sequential_readahead_after.readahead_aops_calls);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: ldlike native readahead ok aops=%llu\n",
            (unsigned long long)(
                sequential_readahead_after.readahead_aops_calls -
                sequential_readahead_before.readahead_aops_calls));

        const struct {
            uint64_t offset;
            size_t length;
        } ext4_bulk_cases[] = {
            {0x000000u, 0x04d1cdu},
            {0x04d1d0u, 0x076ee0u},
            {0x0c40b0u, 0x000620u},
            {0x0c46d0u, 0x000458u},
        };
        for (size_t case_index = 0; case_index < sizeof(ext4_bulk_cases) / sizeof(ext4_bulk_cases[0]); ++case_index) {
            const uint64_t bulk_offset = ext4_bulk_cases[case_index].offset;
            const size_t bulk_length = ext4_bulk_cases[case_index].length;
            if (bulk_length == 0 ||
                bulk_offset > ldlike_size ||
                (uint64_t)bulk_length > ldlike_size - bulk_offset)
            {
                continue;
            }
            uint8_t *generic_bulk_readback = malloc(bulk_length);
            if (generic_bulk_readback == NULL) {
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return -12;
            }
            memset(ext4_kiocb, 0, sizeof(ext4_kiocb));
            memset(ext4_iter, 0, sizeof(ext4_iter));
            memset(generic_bulk_readback, 0, bulk_length);
            write_pointer_field(ext4_kiocb, KB_FS_KIOCB_FILE_OFFSET, ext4_file);
            write_u64_field(ext4_kiocb, KB_FS_KIOCB_POS_OFFSET, bulk_offset);
            write_u64_field(ext4_iter, KB_FS_IOV_ITER_COUNT_OFFSET, (uint64_t)bulk_length);
            write_pointer_field(ext4_iter, KB_FS_IOV_ITER_BUFFER_OFFSET, generic_bulk_readback);
            write_u64_field(ext4_iter, KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, (uint64_t)bulk_length);
            read_result = kb_fs_subsystem_generic_file_read_iter(ext4_kiocb, ext4_iter);
            if (read_result != (long)bulk_length) {
                fprintf(stderr,
                    "kobox-ext4-smoke: ldlike generic_file_read_iter bulk failed inode=%lu case=%zu offset=%llu result=%ld expected=%zu\n",
                    ldlike_inode_number,
                    case_index,
                    (unsigned long long)bulk_offset,
                    read_result,
                    bulk_length);
                free(generic_bulk_readback);
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return read_result < 0 ? (int)read_result : -5;
            }
            int verify_status = ext4_ldlike_verify_pattern(generic_bulk_readback, bulk_offset, bulk_length);
            free(generic_bulk_readback);
            if (verify_status != 0) {
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return verify_status;
            }
            const char *skip_ext4_bulk = getenv("KOBOX_EXT4_IMAGE_SMOKE_SKIP_EXT4_BULK");
            if (skip_ext4_bulk != NULL && skip_ext4_bulk[0] != '\0' && strcmp(skip_ext4_bulk, "0") != 0) {
                continue;
            }

            uint8_t *bulk_readback = malloc(bulk_length);
            if (bulk_readback == NULL) {
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return -12;
            }
            memset(ext4_kiocb, 0, sizeof(ext4_kiocb));
            memset(ext4_iter, 0, sizeof(ext4_iter));
            memset(bulk_readback, 0, bulk_length);
            write_pointer_field(ext4_kiocb, KB_FS_KIOCB_FILE_OFFSET, ext4_file);
            write_u64_field(ext4_kiocb, KB_FS_KIOCB_POS_OFFSET, bulk_offset);
            write_u64_field(ext4_iter, KB_FS_IOV_ITER_COUNT_OFFSET, (uint64_t)bulk_length);
            write_pointer_field(ext4_iter, KB_FS_IOV_ITER_BUFFER_OFFSET, bulk_readback);
            write_u64_field(ext4_iter, KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET, (uint64_t)bulk_length);

            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call((void *)ext4_file_read_iter, &old_gs);
            read_result = ext4_file_read_iter(ext4_kiocb, ext4_iter);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (read_result != (long)bulk_length) {
                fprintf(stderr,
                    "kobox-ext4-smoke: ldlike ext4_file_read_iter bulk failed inode=%lu case=%zu offset=%llu result=%ld expected=%zu\n",
                    ldlike_inode_number,
                    case_index,
                    (unsigned long long)bulk_offset,
                    read_result,
                    bulk_length);
                free(bulk_readback);
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return read_result < 0 ? (int)read_result : -5;
            }
            verify_status = ext4_ldlike_verify_pattern(bulk_readback, bulk_offset, bulk_length);
            free(bulk_readback);
            if (verify_status != 0) {
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return verify_status;
            }
        }
        fprintf(stderr,
            "kobox-ext4-smoke: ldlike bulk read ok inode=%lu cases=%zu\n",
            ldlike_inode_number,
            sizeof(ext4_bulk_cases) / sizeof(ext4_bulk_cases[0]));
    }

    if (read_only != NULL && read_only[0] != '\0' && strcmp(read_only, "0") != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return 0;
    }

    typedef int (*ext4_setattr_fn)(void *, void *, void *);
    ext4_setattr_fn ext4_setattr =
        (ext4_setattr_fn)kb_module_lookup_exported_symbol("ext4_setattr");
    if (ext4_setattr == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: ext4_setattr missing\n");
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    kb_fs_ext4_smoke_ops_t smoke_ops;
    smoke_ops.setattr = ext4_setattr;

    const uint64_t truncated_size = 8;
    int truncate_result = kb_fs_ext4_sync_inode_size(
        mount.super_block,
        inode,
        smoke_inode_dentry,
        truncated_size,
        &smoke_ops,
        "truncate");
    if (truncate_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return truncate_result;
    }
    fprintf(stderr, "kobox-ext4-smoke: truncate ok size=%llu\n", (unsigned long long)truncated_size);

    const int64_t metadata_time_sec = 97445;
    const int64_t metadata_time_nsec = 0;
    int metadata_result = kb_fs_ext4_sync_inode_metadata(
        mount.super_block,
        inode,
        smoke_inode_dentry,
        KB_FS_MODE_REGULAR_0600,
        metadata_time_sec,
        metadata_time_nsec,
        metadata_time_sec,
        metadata_time_nsec,
        &smoke_ops,
        "metadata");
    if (metadata_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return metadata_result;
    }
    fprintf(stderr,
        "kobox-ext4-smoke: metadata ok mode=%o mtime=%lld\n",
        KB_FS_MODE_REGULAR_0600,
        (long long)metadata_time_sec);

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
        uint8_t large_dentry[KB_FS_FAKE_DENTRY_BYTES];
        kb_fs_prepare_named_dentry(
            large_dentry,
            mount.root_dentry,
            large_inode,
            mount.super_block,
            "kobox-large.txt");

        enum {
            KB_FS_FILE_OP_READ_ITER_OFFSET = 0x28,
            KB_FS_FILE_OP_WRITE_ITER_OFFSET = 0x30,
            KB_FS_IOCB_DIRECT = 1u << 17,
            KB_FS_IOCB_WRITE = 1u << 18,
        };
        void *large_file_operations = read_pointer_field(
            large_inode,
            KB_FS_INODE_FILE_OP_OFFSET);
        void *direct_read_operation = large_file_operations == NULL ? NULL :
            read_pointer_field(
                large_file_operations,
                KB_FS_FILE_OP_READ_ITER_OFFSET);
        void *direct_write_operation = large_file_operations == NULL ? NULL :
            read_pointer_field(
                large_file_operations,
                KB_FS_FILE_OP_WRITE_ITER_OFFSET);
        if (direct_read_operation == NULL || direct_write_operation == NULL) {
            fprintf(stderr,
                "kobox-ext4-smoke: direct I/O file operations missing\n");
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -95;
        }
        void *direct_write_page = kb_kvm_alloc_pages_stub(0, 0);
        void *direct_read_page = kb_kvm_alloc_pages_stub(0, 0);
        uint8_t *direct_write_payload = direct_write_page == NULL ? NULL :
            folio_page_payload(direct_write_page);
        uint8_t *direct_read_payload = direct_read_page == NULL ? NULL :
            folio_page_payload(direct_read_page);
        if (direct_write_payload == NULL || direct_read_payload == NULL) {
            if (direct_write_page != NULL) {
                kb_kvm_free_pages_stub(direct_write_page, 0);
            }
            if (direct_read_page != NULL) {
                kb_kvm_free_pages_stub(direct_read_page, 0);
            }
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -12;
        }
        for (size_t i = 0; i < KB_FS_PAGE_SIZE; ++i) {
            direct_write_payload[i] = (uint8_t)((i * 29u + 7u) & 0xffu);
        }
        memset(direct_read_payload, 0, KB_FS_PAGE_SIZE);
        uint8_t direct_file[KB_FS_FAKE_INODE_BYTES] = {0};
        uint8_t direct_kiocb[64] = {0};
        uint8_t direct_iter[128] = {0};
        write_pointer_field(
            direct_file,
            KB_FS_NATIVE_FILE_OP_OFFSET,
            large_file_operations);
        write_u32_field(
            direct_file,
            KB_FS_NATIVE_FILE_MODE_OFFSET,
            KB_FS_FMODE_READ | KB_FS_FMODE_WRITE);
        write_pointer_field(
            direct_file,
            KB_FS_NATIVE_FILE_MAPPING_OFFSET,
            read_pointer_field(large_inode, KB_FS_INODE_MAPPING_OFFSET));
        write_pointer_field(direct_file, KB_FS_FILE_INODE_OFFSET, large_inode);
        write_pointer_field(
            direct_file,
            KB_FS_NATIVE_FILE_PATH_MNT_OFFSET,
            mount.root_vfsmount);
        write_pointer_field(
            direct_file,
            KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET,
            large_dentry);
        file_ra_state_init(
            direct_file + KB_FS_NATIVE_FILE_RA_OFFSET);

        void *large_mapping = read_pointer_field(
            large_inode,
            KB_FS_INODE_MAPPING_OFFSET);
        kb_fs_subsystem_truncate_inode_pages_final(large_mapping);
        kb_fs_storage_trace_t readahead_before;
        kb_fs_storage_trace_t readahead_after;
        kb_fs_storage_trace_snapshot(&readahead_before);
        uint8_t readahead_control[56] = {0};
        write_pointer_field(
            readahead_control,
            KB_FS_READAHEAD_FILE_OFFSET,
            direct_file);
        write_pointer_field(
            readahead_control,
            KB_FS_READAHEAD_MAPPING_OFFSET,
            large_mapping);
        write_pointer_field(
            readahead_control,
            KB_FS_READAHEAD_RA_OFFSET,
            direct_file + KB_FS_NATIVE_FILE_RA_OFFSET);
        kb_fs_subsystem_page_cache_sync_ra(readahead_control, 4);
        kb_fs_storage_trace_snapshot(&readahead_after);
        const uint64_t large_size = read_u64_field(
            large_inode,
            KB_FS_INODE_SIZE_OFFSET);
        const unsigned long expected_readahead_pages =
            large_size == 0 ? 0 :
            (unsigned long)((large_size + KB_FS_PAGE_SIZE - 1u) /
                KB_FS_PAGE_SIZE) < 4u ?
                (unsigned long)((large_size + KB_FS_PAGE_SIZE - 1u) /
                    KB_FS_PAGE_SIZE) : 4u;
        int readahead_pages_ok = 1;
        for (unsigned long readahead_index = 0;
             readahead_index < expected_readahead_pages;
             ++readahead_index)
        {
            void *readahead_folio = kb_fs_subsystem_filemap_get_folio(
                large_mapping,
                readahead_index,
                0,
                0);
            if (low_or_err_pointer(readahead_folio) ||
                (read_u64_field(readahead_folio, 0) &
                    KB_FS_FOLIO_FLAG_UPTODATE) == 0)
            {
                readahead_pages_ok = 0;
            }
            if (!low_or_err_pointer(readahead_folio)) {
                kb_fs_subsystem_folio_put(readahead_folio);
            }
        }
        if (expected_readahead_pages < 2u || !readahead_pages_ok ||
            readahead_after.readahead_aops_calls <=
                readahead_before.readahead_aops_calls ||
            readahead_after.readahead_folios <
                readahead_before.readahead_folios +
                    expected_readahead_pages)
        {
            fprintf(stderr,
                "kobox-ext4-smoke: native readahead failed pages=%d "
                "aops=%llu->%llu folios=%llu->%llu\n",
                readahead_pages_ok,
                (unsigned long long)readahead_before.readahead_aops_calls,
                (unsigned long long)readahead_after.readahead_aops_calls,
                (unsigned long long)readahead_before.readahead_folios,
                (unsigned long long)readahead_after.readahead_folios);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: native readahead ok aops=%llu pages=%llu\n",
            (unsigned long long)(readahead_after.readahead_aops_calls -
                readahead_before.readahead_aops_calls),
            (unsigned long long)(readahead_after.readahead_folios -
                readahead_before.readahead_folios));
        kb_fs_subsystem_truncate_inode_pages_final(large_mapping);
        write_pointer_field(
            direct_kiocb,
            KB_FS_KIOCB_FILE_OFFSET,
            direct_file);
        write_u32_field(
            direct_kiocb,
            KB_FS_KIOCB_FLAGS_OFFSET,
            KB_FS_IOCB_DIRECT | KB_FS_IOCB_WRITE);
        write_u64_field(direct_kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
        direct_iter[KB_FS_IOV_ITER_DATA_SOURCE_OFFSET] = 1;
        write_u64_field(
            direct_iter,
            KB_FS_IOV_ITER_COUNT_OFFSET,
            KB_FS_PAGE_SIZE);
        write_pointer_field(
            direct_iter,
            KB_FS_IOV_ITER_BUFFER_OFFSET,
            direct_write_payload);
        write_u64_field(
            direct_iter,
            KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET,
            KB_FS_PAGE_SIZE);
        long (*direct_write_fn)(void *, void *) = NULL;
        long (*direct_read_fn)(void *, void *) = NULL;
        memcpy(
            &direct_write_fn,
            &direct_write_operation,
            sizeof(direct_write_fn));
        memcpy(
            &direct_read_fn,
            &direct_read_operation,
            sizeof(direct_read_fn));
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(direct_write_operation, &old_gs);
        const long direct_write_result = direct_write_fn(
            direct_kiocb,
            direct_iter);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        memset(direct_kiocb, 0, sizeof(direct_kiocb));
        memset(direct_iter, 0, sizeof(direct_iter));
        write_pointer_field(
            direct_kiocb,
            KB_FS_KIOCB_FILE_OFFSET,
            direct_file);
        write_u32_field(
            direct_kiocb,
            KB_FS_KIOCB_FLAGS_OFFSET,
            KB_FS_IOCB_DIRECT);
        write_u64_field(direct_kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
        write_u64_field(
            direct_iter,
            KB_FS_IOV_ITER_COUNT_OFFSET,
            KB_FS_PAGE_SIZE);
        write_pointer_field(
            direct_iter,
            KB_FS_IOV_ITER_BUFFER_OFFSET,
            direct_read_payload);
        write_u64_field(
            direct_iter,
            KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET,
            KB_FS_PAGE_SIZE);
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(direct_read_operation, &old_gs);
        const long direct_read_result = direct_read_fn(
            direct_kiocb,
            direct_iter);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        const int direct_data_matches = memcmp(
            direct_write_payload,
            direct_read_payload,
            KB_FS_PAGE_SIZE) == 0;
        kb_kvm_free_pages_stub(direct_write_page, 0);
        kb_kvm_free_pages_stub(direct_read_page, 0);
        if (direct_write_result != KB_FS_PAGE_SIZE ||
            direct_read_result != KB_FS_PAGE_SIZE || !direct_data_matches)
        {
            fprintf(stderr,
                "kobox-ext4-smoke: native direct I/O failed write=%ld "
                "read=%ld match=%d\n",
                direct_write_result,
                direct_read_result,
                direct_data_matches);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return direct_write_result < 0 ? (int)direct_write_result :
                (direct_read_result < 0 ? (int)direct_read_result : -5);
        }
        fprintf(stderr,
            "kobox-ext4-smoke: native direct I/O ok write=%ld read=%ld\n",
            direct_write_result,
            direct_read_result);

        const uint64_t large_truncated_size = 4096;
        truncate_result = kb_fs_ext4_sync_inode_size(
            mount.super_block,
            large_inode,
            large_dentry,
            large_truncated_size,
            &smoke_ops,
            "truncate-large");
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
        uint8_t zero_dentry[KB_FS_FAKE_DENTRY_BYTES];
        kb_fs_prepare_named_dentry(
            zero_dentry,
            mount.root_dentry,
            zero_inode,
            mount.super_block,
            "kobox-zero.txt");
        truncate_result = kb_fs_ext4_sync_inode_size(
            mount.super_block,
            zero_inode,
            zero_dentry,
            0,
            &smoke_ops,
            "truncate-zero");
        if (truncate_result != 0) {
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return truncate_result;
        }
        fprintf(stderr, "kobox-ext4-smoke: truncate-zero ok inode=%lu size=0\n", zero_inode_number);
    }

    const char *skip_directory_ops = getenv("KOBOX_EXT4_IMAGE_SMOKE_SKIP_DIRECTORY_OPS");
    if (skip_directory_ops != NULL && skip_directory_ops[0] != '\0' &&
        strcmp(skip_directory_ops, "0") != 0)
    {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return 0;
    }

    void *root_inode = mount.root_inode;
    void *root_dentry = mount.root_dentry;
    void *root_iop = read_pointer_field(root_inode, KB_FS_INODE_OP_OFFSET);
    void *create_op = read_pointer_field(root_iop, KB_FS_INODE_OP_CREATE_OFFSET);
    void *rename_op = read_pointer_field(root_iop, KB_FS_INODE_OP_RENAME_OFFSET);
    void *unlink_op = read_pointer_field(root_iop, KB_FS_INODE_OP_UNLINK_OFFSET);
    void *mkdir_op = read_pointer_field(root_iop, KB_FS_INODE_OP_MKDIR_OFFSET);
    void *rmdir_op = read_pointer_field(root_iop, KB_FS_INODE_OP_RMDIR_OFFSET);
    void *tmpfile_op = read_pointer_field(root_iop, KB_FS_INODE_OP_TMPFILE_OFFSET);
    if (root_inode == NULL || root_dentry == NULL || create_op == NULL || rename_op == NULL ||
        unlink_op == NULL || mkdir_op == NULL || rmdir_op == NULL ||
        tmpfile_op == NULL)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: inode ops missing root=%p dentry=%p "
            "create=%p rename=%p unlink=%p mkdir=%p rmdir=%p tmpfile=%p\n",
            root_inode,
            root_dentry,
            create_op,
            rename_op,
            unlink_op,
            mkdir_op,
            rmdir_op,
            tmpfile_op);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -95;
    }

    static uint8_t smoke_mnt_idmap[136];
    void *tmp_dentry = kb_fs_subsystem_d_alloc_name(
        root_dentry,
        ".kobox-anonymous-tmpfile");
    if (low_or_err_pointer(tmp_dentry)) {
        fprintf(stderr,
            "kobox-ext4-smoke: tmpfile dentry allocation failed result=%p\n",
            tmp_dentry);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -12;
    }
    uint8_t tmp_file[KB_FS_FAKE_INODE_BYTES] = {0};
    write_u32_field(
        tmp_file,
        KB_FS_NATIVE_FILE_MODE_OFFSET,
        KB_FS_FMODE_READ | KB_FS_FMODE_WRITE);
    write_pointer_field(
        tmp_file,
        KB_FS_NATIVE_FILE_PATH_MNT_OFFSET,
        mount.root_vfsmount);
    write_pointer_field(
        tmp_file,
        KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET,
        tmp_dentry);
    int (*tmpfile_fn)(void *, void *, void *, uint16_t) = NULL;
    memcpy(&tmpfile_fn, &tmpfile_op, sizeof(tmpfile_fn));
    kb_fs_ext4_smoke_inode_lock_set_t tmpfile_locks = {0};
    kb_fs_ext4_smoke_inode_lock_set_add(&tmpfile_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&tmpfile_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(tmpfile_op, &old_gs);
    int tmpfile_result = tmpfile_fn(
        smoke_mnt_idmap,
        root_inode,
        tmp_file,
        KB_FS_MODE_REGULAR_0600);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&tmpfile_locks);
    void *tmp_inode = read_pointer_field(tmp_file, KB_FS_FILE_INODE_OFFSET);
    void *tmp_mapping = read_pointer_field(
        tmp_file,
        KB_FS_NATIVE_FILE_MAPPING_OFFSET);
    void *tmp_file_operations = read_pointer_field(
        tmp_file,
        KB_FS_NATIVE_FILE_OP_OFFSET);
    const uint32_t tmp_file_mode = read_u32_field(
        tmp_file,
        KB_FS_NATIVE_FILE_MODE_OFFSET);
    if (tmpfile_result != 0 || low_or_err_pointer(tmp_inode) ||
        tmp_mapping == NULL || tmp_file_operations == NULL ||
        read_pointer_field(tmp_dentry, KB_FS_DENTRY_INODE_OFFSET) != tmp_inode ||
        (tmp_file_mode & (KB_FS_FMODE_OPENED | KB_FS_FMODE_CAN_READ |
            KB_FS_FMODE_CAN_WRITE | KB_FS_FMODE_WRITER)) !=
            (KB_FS_FMODE_OPENED | KB_FS_FMODE_CAN_READ |
                KB_FS_FMODE_CAN_WRITE | KB_FS_FMODE_WRITER))
    {
        fprintf(stderr,
            "kobox-ext4-smoke: native tmpfile open failed result=%d "
            "inode=%p mapping=%p fops=%p mode=0x%x\n",
            tmpfile_result,
            tmp_inode,
            tmp_mapping,
            tmp_file_operations,
            tmp_file_mode);
        kb_fs_subsystem_dput(tmp_dentry);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return tmpfile_result != 0 ? tmpfile_result : -5;
    }

    const uint8_t tmpfile_payload[] = "native-ext4-anonymous-tmpfile";
    uint8_t tmpfile_kiocb[64] = {0};
    uint8_t tmpfile_iter[64] = {0};
    write_pointer_field(tmpfile_kiocb, KB_FS_KIOCB_FILE_OFFSET, tmp_file);
    write_u64_field(tmpfile_kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
    tmpfile_iter[KB_FS_IOV_ITER_DATA_SOURCE_OFFSET] = 1;
    write_u64_field(
        tmpfile_iter,
        KB_FS_IOV_ITER_COUNT_OFFSET,
        sizeof(tmpfile_payload) - 1u);
    write_pointer_field(
        tmpfile_iter,
        KB_FS_IOV_ITER_BUFFER_OFFSET,
        (void *)tmpfile_payload);
    write_u64_field(
        tmpfile_iter,
        KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET,
        sizeof(tmpfile_payload) - 1u);
    void *tmp_write_operation = read_pointer_field(
        tmp_file_operations,
        KB_FS_FILE_OP_WRITE_ITER_OFFSET);
    long (*tmp_write_fn)(void *, void *) = NULL;
    memcpy(&tmp_write_fn, &tmp_write_operation, sizeof(tmp_write_fn));
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call(tmp_write_operation, &old_gs);
    const long tmp_write_result = tmp_write_operation == NULL ? -95 :
        tmp_write_fn(tmpfile_kiocb, tmpfile_iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    uint8_t tmpfile_readback[sizeof(tmpfile_payload)] = {0};
    memset(tmpfile_kiocb, 0, sizeof(tmpfile_kiocb));
    memset(tmpfile_iter, 0, sizeof(tmpfile_iter));
    write_pointer_field(tmpfile_kiocb, KB_FS_KIOCB_FILE_OFFSET, tmp_file);
    write_u64_field(tmpfile_kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
    write_u64_field(
        tmpfile_iter,
        KB_FS_IOV_ITER_COUNT_OFFSET,
        sizeof(tmpfile_payload) - 1u);
    write_pointer_field(
        tmpfile_iter,
        KB_FS_IOV_ITER_BUFFER_OFFSET,
        tmpfile_readback);
    write_u64_field(
        tmpfile_iter,
        KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET,
        sizeof(tmpfile_payload) - 1u);
    const long tmp_read_result = kb_fs_subsystem_generic_file_read_iter(
        tmpfile_kiocb,
        tmpfile_iter);
    int tmp_sync_result = kb_fs_subsystem_vfs_fsync_range(
        tmp_file,
        0,
        (int64_t)sizeof(tmpfile_payload) - 2,
        0);
    if (tmp_write_result != (long)(sizeof(tmpfile_payload) - 1u) ||
        tmp_read_result != (long)(sizeof(tmpfile_payload) - 1u) ||
        memcmp(tmpfile_readback,
            tmpfile_payload,
            sizeof(tmpfile_payload) - 1u) != 0 ||
        tmp_sync_result != 0)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: native tmpfile I/O failed write=%ld "
            "read=%ld sync=%d\n",
            tmp_write_result,
            tmp_read_result,
            tmp_sync_result);
        kb_fs_subsystem_dput(tmp_dentry);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return tmp_write_result < 0 ? (int)tmp_write_result :
            (tmp_read_result < 0 ? (int)tmp_read_result :
                (tmp_sync_result != 0 ? tmp_sync_result : -5));
    }

    void *tmp_release_operation = read_pointer_field(
        tmp_file_operations,
        KB_FS_FILE_OP_RELEASE_OFFSET);
    if (tmp_release_operation != NULL) {
        int (*tmp_release_fn)(void *, void *) = NULL;
        memcpy(&tmp_release_fn, &tmp_release_operation, sizeof(tmp_release_fn));
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(tmp_release_operation, &old_gs);
        tmpfile_result = tmp_release_fn(tmp_inode, tmp_file);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (tmpfile_result != 0) {
            fprintf(stderr,
                "kobox-ext4-smoke: native tmpfile release failed result=%d\n",
                tmpfile_result);
            kb_fs_subsystem_dput(tmp_dentry);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return tmpfile_result;
        }
    }
    uint32_t tmp_writecount = read_u32_field(
        tmp_inode,
        KB_FS_INODE_WRITECOUNT_OFFSET);
    if (tmp_writecount == 0) {
        kb_fs_subsystem_dput(tmp_dentry);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    write_u32_field(
        tmp_inode,
        KB_FS_INODE_WRITECOUNT_OFFSET,
        tmp_writecount - 1u);
    kb_fs_subsystem_path_put(
        (uint8_t *)tmp_file + KB_FS_NATIVE_FILE_PATH_MNT_OFFSET);
    kb_fs_subsystem_dput(tmp_dentry);
    tmpfile_result = kb_fs_subsystem_sync_filesystem(mount.super_block);
    if (tmpfile_result != 0) {
        fprintf(stderr,
            "kobox-ext4-smoke: native tmpfile close sync failed result=%d\n",
            tmpfile_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return tmpfile_result;
    }
    fprintf(stderr,
        "kobox-ext4-smoke: native tmpfile open/write/read/fsync/close ok "
        "bytes=%zu\n",
        sizeof(tmpfile_payload) - 1u);

    const char *acl_parent_inode_text = getenv(
        "KOBOX_EXT4_IMAGE_SMOKE_ACL_PARENT_INODE");
    const unsigned long acl_parent_inode_number =
        acl_parent_inode_text == NULL ? 0 :
            strtoul(acl_parent_inode_text, NULL, 10);
    if (acl_parent_inode_number != 0) {
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_iget, &old_gs);
        void *acl_parent_inode = ext4_iget(
            mount.super_block,
            acl_parent_inode_number,
            0,
            "kobox_ext4_acl_parent",
            0);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (low_or_err_pointer(acl_parent_inode)) {
            fprintf(stderr,
                "kobox-ext4-smoke: ACL parent iget failed inode=%lu ptr=%p\n",
                acl_parent_inode_number,
                acl_parent_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -5;
        }

        uint8_t acl_parent_dentry[KB_FS_FAKE_DENTRY_BYTES];
        uint8_t acl_child_dentry[KB_FS_FAKE_DENTRY_BYTES];
        kb_fs_prepare_named_dentry(
            acl_parent_dentry,
            root_dentry,
            acl_parent_inode,
            mount.super_block,
            "acl-parent");
        kb_fs_prepare_named_dentry(
            acl_child_dentry,
            acl_parent_dentry,
            NULL,
            mount.super_block,
            "kobox-acl-child");
        void *acl_parent_iop = read_pointer_field(
            acl_parent_inode,
            KB_FS_INODE_OP_OFFSET);
        void *acl_create_operation = read_pointer_field(
            acl_parent_iop,
            KB_FS_INODE_OP_CREATE_OFFSET);
        void *acl_unlink_operation = read_pointer_field(
            acl_parent_iop,
            KB_FS_INODE_OP_UNLINK_OFFSET);
        if (acl_create_operation == NULL || acl_unlink_operation == NULL) {
            fprintf(stderr,
                "kobox-ext4-smoke: ACL parent inode operations missing create=%p unlink=%p\n",
                acl_create_operation,
                acl_unlink_operation);
            kb_fs_subsystem_iput(acl_parent_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -95;
        }

        int (*acl_create_fn)(void *, void *, void *, uint16_t, int) = NULL;
        int (*acl_unlink_fn)(void *, void *) = NULL;
        memcpy(
            &acl_create_fn,
            &acl_create_operation,
            sizeof(acl_create_fn));
        memcpy(
            &acl_unlink_fn,
            &acl_unlink_operation,
            sizeof(acl_unlink_fn));
        kb_fs_ext4_smoke_inode_lock_set_t acl_locks = {0};
        kb_fs_ext4_smoke_inode_lock_set_add(&acl_locks, acl_parent_inode);
        kb_fs_ext4_smoke_inode_lock_set_acquire(&acl_locks);
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(acl_create_operation, &old_gs);
        int acl_result = acl_create_fn(
            smoke_mnt_idmap,
            acl_parent_inode,
            acl_child_dentry,
            0100640,
            0);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        kb_fs_ext4_smoke_inode_lock_set_release(&acl_locks);
        void *acl_child_inode = read_pointer_field(
            acl_child_dentry,
            KB_FS_DENTRY_INODE_OFFSET);
        uint16_t acl_child_mode = 0;
        if (!low_or_err_pointer(acl_child_inode)) {
            memcpy(
                &acl_child_mode,
                (const uint8_t *)acl_child_inode + KB_FS_INODE_MODE_OFFSET,
                sizeof(acl_child_mode));
        }
        void *inherited_acl = low_or_err_pointer(acl_child_inode) ? NULL :
            kb_fs_subsystem_get_inode_acl(
                acl_child_inode,
                KB_FS_ACL_TYPE_ACCESS);
        kb_fs_posix_acl_t *inherited =
            low_or_err_pointer(inherited_acl) ? NULL : inherited_acl;
        if (acl_result != 0 || inherited == NULL || inherited->count != 5 ||
            acl_child_mode != 0100640 ||
            inherited->entries[0].tag != KB_FS_ACL_USER_OBJ ||
            inherited->entries[0].permission != 6 ||
            inherited->entries[1].tag != KB_FS_ACL_USER ||
            inherited->entries[1].permission != 4 ||
            inherited->entries[3].tag != KB_FS_ACL_MASK ||
            inherited->entries[3].permission != 4 ||
            inherited->entries[4].tag != KB_FS_ACL_OTHER ||
            inherited->entries[4].permission != 0)
        {
            fprintf(stderr,
                "kobox-ext4-smoke: ACL inheritance failed result=%d inode=%p mode=0%o acl=%p count=%u\n",
                acl_result,
                acl_child_inode,
                acl_child_mode,
                inherited_acl,
                inherited == NULL ? 0 : inherited->count);
            kb_fs_subsystem_posix_acl_release(inherited_acl);
            kb_fs_subsystem_iput(acl_child_inode);
            kb_fs_subsystem_iput(acl_parent_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return acl_result != 0 ? acl_result : -5;
        }
        kb_fs_subsystem_posix_acl_release(inherited_acl);

        acl_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
        kb_fs_ext4_smoke_inode_lock_set_add(&acl_locks, acl_parent_inode);
        kb_fs_ext4_smoke_inode_lock_set_add(&acl_locks, acl_child_inode);
        kb_fs_ext4_smoke_inode_lock_set_acquire(&acl_locks);
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(acl_unlink_operation, &old_gs);
        acl_result = acl_unlink_fn(acl_parent_inode, acl_child_dentry);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        kb_fs_ext4_smoke_inode_lock_set_release(&acl_locks);
        if (acl_result != 0) {
            fprintf(stderr,
                "kobox-ext4-smoke: ACL child unlink failed result=%d\n",
                acl_result);
            kb_fs_subsystem_iput(acl_child_inode);
            kb_fs_subsystem_iput(acl_parent_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return acl_result;
        }
        kb_fs_subsystem_iput(acl_child_inode);
        kb_fs_subsystem_iput(acl_parent_inode);
        fprintf(stderr,
            "kobox-ext4-smoke: POSIX ACL inheritance ok parent=%lu mode=0640 named-user=4 mask=4 other=0\n",
            acl_parent_inode_number);
    }

    uint8_t create_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t rename_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t mkdir_dentry[KB_FS_FAKE_DENTRY_BYTES];
    kb_fs_prepare_named_dentry(
        create_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-created.txt");

    typedef int (*ext4_create_fn)(void *, void *, void *, uint16_t, int);
    ext4_create_fn ext4_create = (ext4_create_fn)create_op;
    kb_fs_ext4_smoke_inode_lock_set_t inode_locks = {0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    int op_result = ext4_create(smoke_mnt_idmap, root_inode, create_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
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
    int64_t created_atime = 0;
    int64_t created_mtime = 0;
    int64_t created_ctime = 0;
    memcpy(&created_atime,
        (const uint8_t *)created_inode + KB_FS_INODE_ATIME_SEC_OFFSET,
        sizeof(created_atime));
    memcpy(&created_mtime,
        (const uint8_t *)created_inode + KB_FS_INODE_MTIME_SEC_OFFSET,
        sizeof(created_mtime));
    memcpy(&created_ctime,
        (const uint8_t *)created_inode + KB_FS_INODE_CTIME_SEC_OFFSET,
        sizeof(created_ctime));
    if (created_atime <= 0 || created_atime != created_mtime ||
        created_atime != created_ctime)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: create timestamp initialization failed atime=%lld mtime=%lld ctime=%lld\n",
            (long long)created_atime,
            (long long)created_mtime,
            (long long)created_ctime);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    fprintf(stderr, "kobox-ext4-smoke: create ok name=kobox-created.txt inode=%p\n", created_inode);
    int created_visible = 0;
    op_result = kb_fs_ext4_smoke_dir_contains(
        root_inode,
        root_dentry,
        "kobox-created.txt",
        &created_visible);
    if (op_result != 0 || !created_visible) {
        fprintf(stderr,
            "kobox-ext4-smoke: native readdir after create failed result=%d visible=%d\n",
            op_result,
            created_visible);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }

    const uint8_t open_unlink_payload[] = "open-unlink-live-data";
    op_result = kb_fs_ext4_smoke_write_payload(
        created_inode,
        create_dentry,
        mount.root_vfsmount,
        open_unlink_payload,
        sizeof(open_unlink_payload) - 1u,
        "open-unlink");
    if (op_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    uint8_t open_unlink_file[KB_FS_FAKE_INODE_BYTES];
    memset(open_unlink_file, 0, sizeof(open_unlink_file));
    void *open_unlink_fops = read_pointer_field(
        created_inode,
        KB_FS_INODE_FILE_OP_OFFSET);
    write_pointer_field(
        open_unlink_file,
        KB_FS_NATIVE_FILE_OP_OFFSET,
        open_unlink_fops);
    write_pointer_field(
        open_unlink_file,
        KB_FS_NATIVE_FILE_MAPPING_OFFSET,
        read_pointer_field(created_inode, KB_FS_INODE_MAPPING_OFFSET));
    write_pointer_field(
        open_unlink_file,
        KB_FS_FILE_INODE_OFFSET,
        created_inode);
    write_pointer_field(
        open_unlink_file,
        KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET - sizeof(void *),
        mount.root_vfsmount);
    write_pointer_field(
        open_unlink_file,
        KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET,
        create_dentry);
    write_u32_field(
        open_unlink_file,
        KB_FS_NATIVE_FILE_MODE_OFFSET,
        KB_FS_FMODE_READ | KB_FS_FMODE_WRITE);
    void *open_unlink_open_op = read_pointer_field(
        open_unlink_fops,
        KB_FS_FILE_OP_OPEN_OFFSET);
    void *open_unlink_release_op = read_pointer_field(
        open_unlink_fops,
        KB_FS_FILE_OP_RELEASE_OFFSET);
    uint32_t open_unlink_writecount = read_u32_field(
        created_inode,
        KB_FS_INODE_WRITECOUNT_OFFSET);
    write_u32_field(
        created_inode,
        KB_FS_INODE_WRITECOUNT_OFFSET,
        open_unlink_writecount + 1u);
    if (open_unlink_open_op != NULL) {
        int (*open_fn)(void *, void *) = NULL;
        memcpy(&open_fn, &open_unlink_open_op, sizeof(open_fn));
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(open_unlink_open_op, &old_gs);
        op_result = open_fn(created_inode, open_unlink_file);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (op_result != 0) {
            write_u32_field(
                created_inode,
                KB_FS_INODE_WRITECOUNT_OFFSET,
                open_unlink_writecount);
            fprintf(stderr,
                "kobox-ext4-smoke: open-unlink open failed result=%d\n",
                op_result);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return op_result;
        }
    }

    kb_fs_prepare_named_dentry(
        rename_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-renamed.txt");
    typedef int (*ext4_rename_fn)(void *, void *, void *, void *, void *, unsigned int);
    ext4_rename_fn ext4_rename = (ext4_rename_fn)rename_op;
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, created_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_rename, &old_gs);
    op_result = ext4_rename(smoke_mnt_idmap, root_inode, create_dentry, root_inode, rename_dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rename failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    write_pointer_field(rename_dentry, KB_FS_DENTRY_INODE_OFFSET, created_inode);
    fprintf(stderr, "kobox-ext4-smoke: rename ok old=kobox-created.txt new=kobox-renamed.txt\n");
    int old_visible = 0;
    int renamed_visible = 0;
    op_result = kb_fs_ext4_smoke_dir_contains(
        root_inode,
        root_dentry,
        "kobox-created.txt",
        &old_visible);
    if (op_result == 0) {
        op_result = kb_fs_ext4_smoke_dir_contains(
            root_inode,
            root_dentry,
            "kobox-renamed.txt",
            &renamed_visible);
    }
    if (op_result != 0 || old_visible || !renamed_visible) {
        fprintf(stderr,
            "kobox-ext4-smoke: native readdir after rename failed result=%d old=%d new=%d\n",
            op_result,
            old_visible,
            renamed_visible);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }

    typedef int (*ext4_unlink_fn)(void *, void *);
    ext4_unlink_fn ext4_unlink = (ext4_unlink_fn)unlink_op;
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, created_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
    op_result = ext4_unlink(root_inode, rename_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: unlink failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    uint8_t open_unlink_readback[sizeof(open_unlink_payload)] = {0};
    uint8_t open_unlink_kiocb[64] = {0};
    uint8_t open_unlink_iter[64] = {0};
    write_pointer_field(
        open_unlink_kiocb,
        KB_FS_KIOCB_FILE_OFFSET,
        open_unlink_file);
    write_u64_field(open_unlink_kiocb, KB_FS_KIOCB_POS_OFFSET, 0);
    write_u64_field(
        open_unlink_iter,
        KB_FS_IOV_ITER_COUNT_OFFSET,
        sizeof(open_unlink_payload) - 1u);
    write_pointer_field(
        open_unlink_iter,
        KB_FS_IOV_ITER_BUFFER_OFFSET,
        open_unlink_readback);
    long open_unlink_read = kb_fs_subsystem_generic_file_read_iter(
        open_unlink_kiocb,
        open_unlink_iter);
    if (open_unlink_read != (long)(sizeof(open_unlink_payload) - 1u) ||
        memcmp(
            open_unlink_readback,
            open_unlink_payload,
            sizeof(open_unlink_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "kobox-ext4-smoke: open-unlink retained read failed result=%ld\n",
            open_unlink_read);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return open_unlink_read < 0 ? (int)open_unlink_read : -5;
    }
    op_result = kb_fs_subsystem_sync_filesystem(mount.super_block);
    if (op_result != 0) {
        fprintf(stderr,
            "kobox-ext4-smoke: open-unlink sync failed result=%d\n",
            op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    if (open_unlink_release_op != NULL) {
        int (*release_fn)(void *, void *) = NULL;
        memcpy(&release_fn, &open_unlink_release_op, sizeof(release_fn));
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call(open_unlink_release_op, &old_gs);
        const int release_result = release_fn(created_inode, open_unlink_file);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (release_result != 0) {
            fprintf(stderr,
                "kobox-ext4-smoke: open-unlink release failed result=%d\n",
                release_result);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return release_result;
        }
    }
    uint32_t open_unlink_final_writecount = read_u32_field(
        created_inode,
        KB_FS_INODE_WRITECOUNT_OFFSET);
    if (open_unlink_final_writecount == 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return -5;
    }
    write_u32_field(
        created_inode,
        KB_FS_INODE_WRITECOUNT_OFFSET,
        open_unlink_final_writecount - 1u);
    kb_fs_subsystem_iput(created_inode);
    fprintf(stderr,
        "kobox-ext4-smoke: open-unlink ok name=kobox-renamed.txt bytes=%zu\n",
        sizeof(open_unlink_payload) - 1u);
    renamed_visible = 0;
    op_result = kb_fs_ext4_smoke_dir_contains(
        root_inode,
        root_dentry,
        "kobox-renamed.txt",
        &renamed_visible);
    if (op_result != 0 || renamed_visible) {
        fprintf(stderr,
            "kobox-ext4-smoke: native readdir after unlink failed result=%d visible=%d\n",
            op_result,
            renamed_visible);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }

    uint8_t replace_old_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t replace_new_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t replace_path_dentry[KB_FS_FAKE_DENTRY_BYTES];
    kb_fs_prepare_named_dentry(
        replace_old_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-replace-old.txt");
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    op_result = ext4_create(smoke_mnt_idmap, root_inode, replace_old_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
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
        replace_old_dentry,
        mount.root_vfsmount,
        replace_old_payload,
        sizeof(replace_old_payload) - 1u,
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
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    op_result = ext4_create(smoke_mnt_idmap, root_inode, replace_new_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
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
        replace_new_dentry,
        mount.root_vfsmount,
        replace_new_payload,
        sizeof(replace_new_payload) - 1u,
        "replace-new");
    if (op_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }

    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, replace_old_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, replace_new_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
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
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rename replace failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }

    kb_fs_subsystem_iput(replace_new_inode);

    kb_fs_prepare_named_dentry(
        replace_path_dentry,
        root_dentry,
        replace_old_inode,
        mount.super_block,
        "kobox-replace-new.txt");
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, replace_old_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
    op_result = ext4_unlink(root_inode, replace_path_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
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
    kb_fs_subsystem_iput(replace_old_inode);
    fprintf(stderr, "kobox-ext4-smoke: rename replace cleanup ok\n");

    kb_fs_prepare_named_dentry(
        mkdir_dentry,
        root_dentry,
        NULL,
        mount.super_block,
        "kobox-created-dir");
    typedef int (*ext4_mkdir_fn)(void *, void *, void *, uint16_t);
    ext4_mkdir_fn ext4_mkdir = (ext4_mkdir_fn)mkdir_op;
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_mkdir, &old_gs);
    op_result = ext4_mkdir(smoke_mnt_idmap, root_inode, mkdir_dentry, KB_FS_MODE_DIRECTORY_0755);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
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

    uint8_t nested_create_dentry[KB_FS_FAKE_DENTRY_BYTES];
    uint8_t nested_rename_dentry[KB_FS_FAKE_DENTRY_BYTES];
    kb_fs_prepare_named_dentry(
        nested_create_dentry,
        mkdir_dentry,
        NULL,
        mount.super_block,
        "nested.txt");
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, mkdir_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
    op_result = ext4_create(smoke_mnt_idmap, mkdir_inode, nested_create_dentry, KB_FS_MODE_REGULAR_0644, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    void *nested_inode = read_pointer_field(nested_create_dentry, KB_FS_DENTRY_INODE_OFFSET);
    if (op_result != 0 || nested_inode == NULL) {
        fprintf(stderr, "kobox-ext4-smoke: nested create failed result=%d inode=%p\n", op_result, nested_inode);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result != 0 ? op_result : -5;
    }
    const uint8_t nested_payload[] = "nested-live";
    op_result = kb_fs_ext4_smoke_write_payload(
        nested_inode,
        nested_create_dentry,
        mount.root_vfsmount,
        nested_payload,
        sizeof(nested_payload) - 1u,
        "nested");
    if (op_result != 0) {
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }

    kb_fs_prepare_named_dentry(
        nested_rename_dentry,
        mkdir_dentry,
        NULL,
        mount.super_block,
        "nested.renamed");
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, mkdir_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, nested_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_rename, &old_gs);
    op_result = ext4_rename(
        smoke_mnt_idmap,
        mkdir_inode,
        nested_create_dentry,
        mkdir_inode,
        nested_rename_dentry,
        0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: nested rename failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    write_pointer_field(nested_rename_dentry, KB_FS_DENTRY_INODE_OFFSET, nested_inode);

    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, mkdir_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, nested_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
    op_result = ext4_unlink(mkdir_inode, nested_rename_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: nested unlink failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    kb_fs_subsystem_iput(nested_inode);
    fprintf(stderr, "kobox-ext4-smoke: nested unlink ok name=nested.renamed\n");

    typedef int (*ext4_rmdir_fn)(void *, void *);
    ext4_rmdir_fn ext4_rmdir = (ext4_rmdir_fn)rmdir_op;
    inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
    kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, mkdir_inode);
    kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
    old_gs = 0;
    has_gs = kb_fs_enter_ext4_call((void *)ext4_rmdir, &old_gs);
    op_result = ext4_rmdir(root_inode, mkdir_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
    if (op_result != 0) {
        fprintf(stderr, "kobox-ext4-smoke: rmdir failed result=%d\n", op_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(device);
        return op_result;
    }
    kb_fs_subsystem_iput(mkdir_inode);
    fprintf(stderr, "kobox-ext4-smoke: rmdir ok name=kobox-created-dir\n");

    size_t churn_count = 0;
    const char *churn_count_text = getenv("KOBOX_EXT4_IMAGE_SMOKE_CHURN_COUNT");
    if (churn_count_text != NULL && churn_count_text[0] != '\0') {
        char *end = NULL;
        const unsigned long parsed = strtoul(churn_count_text, &end, 10);
        if (end == churn_count_text || *end != '\0' || parsed > 512u) {
            fprintf(stderr,
                "kobox-ext4-smoke: invalid churn count value=%s max=512\n",
                churn_count_text);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -22;
        }
        churn_count = (size_t)parsed;
    }

    if (churn_count != 0) {
        enum {
            KB_FS_EXT4_CHURN_NAME_BYTES = 48,
            KB_FS_EXT4_CHURN_PAYLOAD_BYTES = 16384,
        };
        void **churn_inodes = calloc(churn_count, sizeof(*churn_inodes));
        char (*churn_names)[KB_FS_EXT4_CHURN_NAME_BYTES] =
            calloc(churn_count, sizeof(*churn_names));
        uint8_t *churn_payload = malloc(KB_FS_EXT4_CHURN_PAYLOAD_BYTES);
        if (churn_inodes == NULL || churn_names == NULL || churn_payload == NULL) {
            free(churn_payload);
            free(churn_names);
            free(churn_inodes);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -12;
        }

        int churn_result = 0;
        size_t live_count = 0;
        for (size_t i = 0; i < churn_count; ++i) {
            char temporary_name[KB_FS_EXT4_CHURN_NAME_BYTES];
            const int temporary_length = snprintf(
                temporary_name,
                sizeof(temporary_name),
                ".apk-churn-%04zu.tmp",
                i);
            const int final_length = snprintf(
                churn_names[i],
                sizeof(churn_names[i]),
                ".apk-churn-%04zu.dat",
                i);
            if (temporary_length <= 0 ||
                (size_t)temporary_length >= sizeof(temporary_name) ||
                final_length <= 0 ||
                (size_t)final_length >= sizeof(churn_names[i]))
            {
                churn_result = -36;
                break;
            }

            uint8_t temporary_dentry[KB_FS_FAKE_DENTRY_BYTES];
            uint8_t final_dentry[KB_FS_FAKE_DENTRY_BYTES];
            kb_fs_prepare_named_dentry(
                temporary_dentry,
                root_dentry,
                NULL,
                mount.super_block,
                temporary_name);
            inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
            kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
            kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
            churn_result = ext4_create(
                smoke_mnt_idmap,
                root_inode,
                temporary_dentry,
                KB_FS_MODE_REGULAR_0644,
                0);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
            void *churn_inode = read_pointer_field(
                temporary_dentry,
                KB_FS_DENTRY_INODE_OFFSET);
            if (churn_result != 0 || churn_inode == NULL) {
                churn_result = churn_result != 0 ? churn_result : -5;
                break;
            }

            const size_t payload_length =
                1u + ((i * 7919u) % KB_FS_EXT4_CHURN_PAYLOAD_BYTES);
            for (size_t byte = 0; byte < payload_length; ++byte) {
                churn_payload[byte] = (uint8_t)((i * 131u + byte * 17u) & 0xffu);
            }
            churn_result = kb_fs_ext4_smoke_write_payload(
                churn_inode,
                temporary_dentry,
                mount.root_vfsmount,
                churn_payload,
                payload_length,
                "apk-churn");
            if (churn_result != 0) {
                break;
            }

            kb_fs_prepare_named_dentry(
                final_dentry,
                root_dentry,
                NULL,
                mount.super_block,
                churn_names[i]);
            inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
            kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
            kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, churn_inode);
            kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call((void *)ext4_rename, &old_gs);
            churn_result = ext4_rename(
                smoke_mnt_idmap,
                root_inode,
                temporary_dentry,
                root_inode,
                final_dentry,
                0);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
            if (churn_result != 0) {
                break;
            }
            churn_inodes[i] = churn_inode;
            live_count++;
        }

        if (churn_result == 0) {
            churn_result = kb_fs_subsystem_sync_filesystem(mount.super_block);
        }
        if (churn_result == 0) {
            for (size_t i = live_count; i != 0; --i) {
                const size_t index = i - 1u;
                uint8_t final_dentry[KB_FS_FAKE_DENTRY_BYTES];
                kb_fs_prepare_named_dentry(
                    final_dentry,
                    root_dentry,
                    churn_inodes[index],
                    mount.super_block,
                    churn_names[index]);
                inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
                kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
                kb_fs_ext4_smoke_inode_lock_set_add(
                    &inode_locks,
                    churn_inodes[index]);
                kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
                old_gs = 0;
                has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
                churn_result = ext4_unlink(root_inode, final_dentry);
                if (has_gs) {
                    kb_shim_leave_kernel_gs(old_gs);
                }
                kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
                if (churn_result != 0) {
                    break;
                }
                kb_fs_subsystem_iput(churn_inodes[index]);
                churn_inodes[index] = NULL;
            }
        }
        if (churn_result == 0) {
            churn_result = kb_fs_subsystem_sync_filesystem(mount.super_block);
        }
        free(churn_payload);
        free(churn_names);
        free(churn_inodes);
        if (churn_result != 0) {
            fprintf(stderr,
                "kobox-ext4-smoke: apk churn failed result=%d live=%zu requested=%zu\n",
                churn_result,
                live_count,
                churn_count);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return churn_result;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: apk churn ok files=%zu max_payload=%u\n",
            churn_count,
            KB_FS_EXT4_CHURN_PAYLOAD_BYTES);
    }

    const char *run_enospc = getenv("KOBOX_EXT4_IMAGE_SMOKE_ENOSPC");
    if (run_enospc != NULL && run_enospc[0] != '\0' &&
        strcmp(run_enospc, "0") != 0)
    {
        enum {
            KB_FS_EXT4_ENOSPC_PAYLOAD_BYTES = 64u * 1024u,
            KB_FS_EXT4_ENOSPC_MAX_WRITES = 32768,
        };
        uint8_t enospc_dentry[KB_FS_FAKE_DENTRY_BYTES];
        kb_fs_prepare_named_dentry(
            enospc_dentry,
            root_dentry,
            NULL,
            mount.super_block,
            "kobox-enospc.dat");
        inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
        kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
        kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_create, &old_gs);
        op_result = ext4_create(
            smoke_mnt_idmap,
            root_inode,
            enospc_dentry,
            KB_FS_MODE_REGULAR_0644,
            0);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
        void *enospc_inode = read_pointer_field(
            enospc_dentry,
            KB_FS_DENTRY_INODE_OFFSET);
        if (op_result != 0 || enospc_inode == NULL) {
            fprintf(stderr,
                "kobox-ext4-smoke: enospc create failed result=%d inode=%p\n",
                op_result,
                enospc_inode);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return op_result != 0 ? op_result : -5;
        }

        void *enospc_fops = read_pointer_field(
            enospc_inode,
            KB_FS_INODE_FILE_OP_OFFSET);
        uint8_t enospc_file[KB_FS_FAKE_INODE_BYTES];
        memset(enospc_file, 0, sizeof(enospc_file));
        write_pointer_field(
            enospc_file,
            KB_FS_NATIVE_FILE_OP_OFFSET,
            enospc_fops);
        write_pointer_field(
            enospc_file,
            KB_FS_NATIVE_FILE_MAPPING_OFFSET,
            read_pointer_field(enospc_inode, KB_FS_INODE_MAPPING_OFFSET));
        write_pointer_field(enospc_file, KB_FS_FILE_INODE_OFFSET, enospc_inode);
        write_pointer_field(
            enospc_file,
            KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET - sizeof(void *),
            mount.root_vfsmount);
        write_pointer_field(
            enospc_file,
            KB_FS_NATIVE_FILE_PATH_DENTRY_OFFSET,
            enospc_dentry);
        write_u32_field(
            enospc_file,
            KB_FS_NATIVE_FILE_MODE_OFFSET,
            KB_FS_FMODE_WRITE);

        void *enospc_open_op = read_pointer_field(
            enospc_fops,
            KB_FS_FILE_OP_OPEN_OFFSET);
        void *enospc_release_op = read_pointer_field(
            enospc_fops,
            KB_FS_FILE_OP_RELEASE_OFFSET);
        int enospc_opened = 0;
        uint32_t enospc_writecount = read_u32_field(
            enospc_inode,
            KB_FS_INODE_WRITECOUNT_OFFSET);
        write_u32_field(
            enospc_inode,
            KB_FS_INODE_WRITECOUNT_OFFSET,
            enospc_writecount + 1u);
        if (enospc_open_op != NULL) {
            int (*open_fn)(void *, void *) = NULL;
            memcpy(&open_fn, &enospc_open_op, sizeof(open_fn));
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call(enospc_open_op, &old_gs);
            op_result = open_fn(enospc_inode, enospc_file);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (op_result != 0) {
                write_u32_field(
                    enospc_inode,
                    KB_FS_INODE_WRITECOUNT_OFFSET,
                    enospc_writecount);
                fprintf(stderr,
                    "kobox-ext4-smoke: enospc open failed result=%d\n",
                    op_result);
                kb_fs_subsystem_set_mount_probe_block_device(NULL);
                kb_fs_block_device_destroy(device);
                return op_result;
            }
            enospc_opened = 1;
        }

        void *write_iter_address = NULL;
        if (kb_module_find_symbol(
                kb_loader_active_module(),
                "ext4_file_write_iter",
                &write_iter_address) != KB_OK ||
            write_iter_address == NULL)
        {
            fprintf(stderr,
                "kobox-ext4-smoke: enospc ext4_file_write_iter missing\n");
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -95;
        }
        long (*write_iter_fn)(void *, void *) = NULL;
        memcpy(&write_iter_fn, &write_iter_address, sizeof(write_iter_fn));
        uint8_t *enospc_payload = malloc(KB_FS_EXT4_ENOSPC_PAYLOAD_BYTES);
        if (enospc_payload == NULL) {
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return -12;
        }
        for (size_t i = 0; i < KB_FS_EXT4_ENOSPC_PAYLOAD_BYTES; ++i) {
            enospc_payload[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
        }

        uint64_t enospc_bytes = 0;
        int saw_enospc = 0;
        int enospc_failure = 0;
        for (unsigned int attempt = 0;
             attempt < KB_FS_EXT4_ENOSPC_MAX_WRITES;
             ++attempt)
        {
            uint8_t enospc_kiocb[64] = {0};
            uint8_t enospc_iter[128] = {0};
            write_pointer_field(
                enospc_kiocb,
                KB_FS_KIOCB_FILE_OFFSET,
                enospc_file);
            write_u64_field(
                enospc_kiocb,
                KB_FS_KIOCB_POS_OFFSET,
                enospc_bytes);
            write_u64_field(
                enospc_iter,
                KB_FS_IOV_ITER_COUNT_OFFSET,
                KB_FS_EXT4_ENOSPC_PAYLOAD_BYTES);
            write_pointer_field(
                enospc_iter,
                KB_FS_IOV_ITER_BUFFER_OFFSET,
                enospc_payload);
            write_u64_field(
                enospc_iter,
                KB_FS_IOV_ITER_BUFFER_CAPACITY_OFFSET,
                KB_FS_EXT4_ENOSPC_PAYLOAD_BYTES);
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call(write_iter_address, &old_gs);
            const long write_status = write_iter_fn(enospc_kiocb, enospc_iter);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (write_status == -28) {
                saw_enospc = 1;
                break;
            }
            if (write_status <= 0) {
                enospc_failure = write_status < 0 ? (int)write_status : -5;
                break;
            }
            enospc_bytes += (uint64_t)write_status;
            const int fsync_status = kb_fs_subsystem_vfs_fsync_range(
                enospc_file,
                0,
                INT64_MAX,
                0);
            if (fsync_status == -28) {
                saw_enospc = 1;
                break;
            }
            if (fsync_status != 0) {
                enospc_failure = fsync_status;
                break;
            }
        }
        free(enospc_payload);

        inode_locks = (kb_fs_ext4_smoke_inode_lock_set_t){0};
        kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, root_inode);
        kb_fs_ext4_smoke_inode_lock_set_add(&inode_locks, enospc_inode);
        kb_fs_ext4_smoke_inode_lock_set_acquire(&inode_locks);
        old_gs = 0;
        has_gs = kb_fs_enter_ext4_call((void *)ext4_unlink, &old_gs);
        const int enospc_unlink_status = ext4_unlink(
            root_inode,
            enospc_dentry);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        kb_fs_ext4_smoke_inode_lock_set_release(&inode_locks);
        if (enospc_opened && enospc_release_op != NULL) {
            int (*release_fn)(void *, void *) = NULL;
            memcpy(&release_fn, &enospc_release_op, sizeof(release_fn));
            old_gs = 0;
            has_gs = kb_fs_enter_ext4_call(enospc_release_op, &old_gs);
            const int release_status = release_fn(enospc_inode, enospc_file);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            if (enospc_failure == 0 && release_status != 0) {
                enospc_failure = release_status;
            }
        }
        const uint32_t enospc_final_writecount = read_u32_field(
            enospc_inode,
            KB_FS_INODE_WRITECOUNT_OFFSET);
        if (enospc_final_writecount == 0) {
            if (enospc_failure == 0) {
                enospc_failure = -5;
            }
        } else {
            write_u32_field(
                enospc_inode,
                KB_FS_INODE_WRITECOUNT_OFFSET,
                enospc_final_writecount - 1u);
        }
        kb_fs_subsystem_iput(enospc_inode);
        if (enospc_failure == 0 && enospc_unlink_status != 0) {
            enospc_failure = enospc_unlink_status;
        }
        if (enospc_failure == 0 && !saw_enospc) {
            enospc_failure = -5;
        }
        if (enospc_failure != 0) {
            fprintf(stderr,
                "kobox-ext4-smoke: enospc failed result=%d bytes=%llu observed=%d\n",
                enospc_failure,
                (unsigned long long)enospc_bytes,
                saw_enospc);
            kb_fs_subsystem_set_mount_probe_block_device(NULL);
            kb_fs_block_device_destroy(device);
            return enospc_failure;
        }
        fprintf(stderr,
            "kobox-ext4-smoke: enospc ok bytes=%llu status=-28 cleanup=ok\n",
            (unsigned long long)enospc_bytes);
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
