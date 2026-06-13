#include "linux_subsystem/fs/fs.h"
#include "kobox/shim.h"
#include "loader/module_context.h"
#include "linux_subsystem/block/block.h"

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
    KB_FS_FAKE_DENTRY_BYTES = 512,
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
    KB_FS_ADDRESS_SPACE_OFFSET = 0x40,
    KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET = 0x18,
    KB_FS_BUFFER_HEAD_SIZE_OFFSET = 0x20,
    KB_FS_BUFFER_HEAD_DATA_OFFSET = 0x28,
    KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET = 0x60,
    KB_FS_INODE_MODE_OFFSET = 0x0,
    KB_FS_INODE_SB_OFFSET = 0x28,
    KB_FS_INODE_MAPPING_OFFSET = 0x30,
    KB_FS_INODE_NUMBER_OFFSET = 0x40,
    KB_FS_INODE_NLINK_OFFSET = 0x48,
    KB_FS_INODE_SIZE_OFFSET = 0x50,
    KB_FS_INODE_BLOCKS_OFFSET = 0x88,
    KB_FS_INODE_STATE_OFFSET = 0x90,
    KB_FS_INODE_STATE_NEW = 0x1,
    KB_FS_INODE_MODE_DIRECTORY = 0040000 | 0755,
    KB_FS_INODE_EXT4_DIRECT_BLOCK0_BACK_OFFSET = 0x128,
    KB_FS_INODE_EXT4_DIRECT_BLOCKS = 12,
    KB_FS_INODE_EXT4_ES_LRU_OFFSET = 0x2c0,
    KB_FS_INODE_EXT4_ES_SHRINK_COUNT_OFFSET = 0x2d4,
    KB_FS_EXT4_SBI_ES_LRU_OFFSET = 0x520,
    KB_FS_EXT4_SBI_ES_SHRINK_COUNT_OFFSET = 0x530,
    KB_FS_EXT4_EXTENT_HEADER_MAGIC = 0xf30a,
    KB_FS_DENTRY_INODE_OFFSET = 0x38,
    KB_FS_FILE_INODE_OFFSET = 0x28,
    KB_FS_KIOCB_FILE_OFFSET = 0x0,
    KB_FS_KIOCB_POS_OFFSET = 0x8,
    KB_FS_IOV_ITER_COUNT_OFFSET = 0x18,
    KB_FS_IOV_ITER_BUFFER_OFFSET = 0x20,
    KB_FS_BIO_MAGIC = 0x6b62696f,
    KB_FS_BIO_OP_MASK = 0xff,
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

static kb_fs_type_record_t fs_types[KB_FS_TYPE_MAX];
static kb_fs_mount_record_t fs_mounts[KB_FS_MOUNT_MAX];
static kb_fs_file_record_t fs_files[KB_FS_FILE_MAX];
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

static void clear_mount_probe_objects(void)
{
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

static int fs_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_FS");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int low_or_err_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return ptr == NULL || value < 4096u || value >= UINTPTR_MAX - 4095u;
}

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
        return kb_block_subsystem_disk_read(disk_ctx->disk, offset / 512u, buffer, size);
    }

    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    uint64_t start_sector = offset / 512u;
    uint64_t end_sector = (end + 511u) / 512u;
    uint64_t sector_count = end_sector - start_sector;
    uint64_t bounce_size64 = 0;
    if (__builtin_mul_overflow(sector_count, 512ull, &bounce_size64) || bounce_size64 > SIZE_MAX) {
        return -34;
    }
    void *bounce = calloc(1, (size_t)bounce_size64);
    if (bounce == NULL) {
        return -12;
    }
    int status = kb_block_subsystem_disk_read(disk_ctx->disk, start_sector, bounce, (size_t)bounce_size64);
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
        return kb_block_subsystem_disk_write(disk_ctx->disk, offset / 512u, buffer, size);
    }

    uint64_t end = 0;
    if (__builtin_add_overflow(offset, (uint64_t)size, &end)) {
        return -34;
    }
    uint64_t start_sector = offset / 512u;
    uint64_t end_sector = (end + 511u) / 512u;
    uint64_t sector_count = end_sector - start_sector;
    uint64_t bounce_size64 = 0;
    if (__builtin_mul_overflow(sector_count, 512ull, &bounce_size64) || bounce_size64 > SIZE_MAX) {
        return -34;
    }
    void *bounce = calloc(1, (size_t)bounce_size64);
    if (bounce == NULL) {
        return -12;
    }
    int status = kb_block_subsystem_disk_read(disk_ctx->disk, start_sector, bounce, (size_t)bounce_size64);
    if (status == 0) {
        memcpy((uint8_t *)bounce + (offset % 512u), buffer, size);
        status = kb_block_subsystem_disk_write(disk_ctx->disk, start_sector, bounce, (size_t)bounce_size64);
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
    if (disk == NULL || out_device == NULL) {
        return -22;
    }
    *out_device = NULL;

    kb_block_disk_snapshot_t snapshot;
    int status = kb_block_subsystem_disk_snapshot(disk, &snapshot);
    if (status != 0) {
        return status;
    }

    uint64_t size_bytes = 0;
    if (__builtin_mul_overflow(snapshot.capacity_sectors, 512ull, &size_bytes)) {
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

    void *buffer_head = calloc(1, KB_FS_FAKE_BUFFER_HEAD_BYTES);
    void *data = calloc(1, block_size);
    if (buffer_head == NULL || data == NULL) {
        free(buffer_head);
        free(data);
        return NULL;
    }

    uint64_t offset = 0;
    if (__builtin_mul_overflow(block_number, (uint64_t)block_size, &offset)) {
        free(buffer_head);
        free(data);
        return NULL;
    }
    int status = kb_fs_block_device_read(device, offset, data, block_size);
    if (status != 0) {
        free(buffer_head);
        free(data);
        return NULL;
    }

    uint64_t flags = 0x11u;
    uint32_t refcount = 1u;
    memcpy(buffer_head, &flags, sizeof(flags));
    write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_SIZE_OFFSET, block_size);
    write_u64_field(buffer_head, KB_FS_BUFFER_HEAD_BLOCKNR_OFFSET, block_number);
    write_pointer_field(buffer_head, KB_FS_BUFFER_HEAD_DATA_OFFSET, data);
    memcpy((uint8_t *)buffer_head + KB_FS_BUFFER_HEAD_REFCOUNT_OFFSET, &refcount, sizeof(refcount));

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
            "kobox-fs: bdev_getblk device=%s bdev=%p block=%llu size=%u magic=0x%04x\n",
            device->name == NULL ? "" : device->name,
            bdev,
            (unsigned long long)block_number,
            block_size,
            last_mount_path_probe.observed_ext4_magic);
    }
    return buffer_head;
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

void *kb_fs_subsystem_d_make_root(void *inode)
{
    if (low_or_err_pointer(inode)) {
        return NULL;
    }
    void *dentry = calloc(1, KB_FS_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return NULL;
    }
    write_pointer_field(dentry, KB_FS_DENTRY_INODE_OFFSET, inode);
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
    memcpy(&entries, i_block + 0x2, sizeof(entries));
    memcpy(&depth, i_block + 0x6, sizeof(depth));
    if (depth != 0) {
        return 0;
    }
    for (uint16_t i = 0; i < entries && i < 4u; i++) {
        const uint8_t *extent = i_block + 0x0c + ((size_t)i * 0x0c);
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
    if (buffer == NULL || block_size == 0 || active_bdev_binding.device == NULL) {
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
        uint64_t chunk = read_size - total_read;
        if (chunk > block_available) {
            chunk = block_available;
        }
        uint32_t disk_block = ext4_extent_block_number(inode, file_block);
        if (disk_block == 0 || chunk > SIZE_MAX) {
            break;
        }
        uint64_t disk_offset = ((uint64_t)disk_block * block_size) + block_offset;
        if (active_bdev_binding.device->read(active_bdev_binding.device->ctx,
                disk_offset,
                (uint8_t *)buffer + total_read,
                (size_t)chunk) != 0)
        {
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
    void *inode = NULL;
    void *super_block = NULL;
    void *buffer = NULL;
    uint64_t pos = 0;
    uint64_t file_size = 0;
    uint64_t block_size = 0;
    uint64_t count = 0;
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
    if (buffer == NULL || block_size == 0 || active_bdev_binding.device == NULL || active_bdev_binding.device->write == NULL) {
        return -5;
    }
    if (pos >= file_size || count == 0) {
        return 0;
    }
    uint64_t available = file_size - pos;
    uint64_t write_size = count < available ? count : available;
    if (write_size > SIZE_MAX) {
        return -22;
    }
    uint64_t total_written = 0;
    while (total_written < write_size) {
        uint64_t current_pos = pos + total_written;
        uint64_t file_block = current_pos / block_size;
        uint64_t block_offset = current_pos % block_size;
        uint64_t block_available = block_size - block_offset;
        uint64_t chunk = write_size - total_written;
        if (chunk > block_available) {
            chunk = block_available;
        }
        uint32_t disk_block = ext4_extent_block_number(inode, file_block);
        if (disk_block == 0 || chunk > SIZE_MAX) {
            break;
        }
        uint64_t disk_offset = ((uint64_t)disk_block * block_size) + block_offset;
        if (active_bdev_binding.device->write(active_bdev_binding.device->ctx,
                disk_offset,
                (const uint8_t *)buffer + total_written,
                (size_t)chunk) != 0)
        {
            break;
        }
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: generic_perform_write inode=%p file_block=%llu disk_block=%u offset=%llu bytes=%llu\n",
                inode,
                (unsigned long long)file_block,
                disk_block,
                (unsigned long long)disk_offset,
                (unsigned long long)chunk);
        }
        total_written += chunk;
    }
    if (total_written == 0) {
        return -5;
    }
    pos += total_written;
    count -= total_written;
    memcpy((uint8_t *)kiocb + KB_FS_KIOCB_POS_OFFSET, &pos, sizeof(pos));
    memcpy((uint8_t *)iter + KB_FS_IOV_ITER_COUNT_OFFSET, &count, sizeof(count));
    return (long)total_written;
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
        write_pointer_field(bdev_inode, KB_FS_ADDRESS_SPACE_OFFSET, bdev_mapping);
        write_pointer_field(bdev, KB_FS_BDEV_STATS_OFFSET, bdev_stats);
        write_pointer_field(bdev, KB_FS_BDEV_QUEUE_OFFSET, queue);
        active_bdev_binding.bdev = bdev;
        active_bdev_binding.device = mount_probe_block_device;
        memcpy((uint8_t *)super_block + KB_FS_SUPER_BLOCK_DEVNAME_OFFSET,
            "kobox-block-image",
            sizeof("kobox-block-image"));
        unsigned long old_gs = 0;
        unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)fill_super);
        int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
        if (fs_trace_enabled()) {
            fprintf(stderr,
                "kobox-fs: calling fill_super super=%p fc=%p bdev=%p queue=%p kernel_gs=0x%lx has_gs=%d\n",
                super_block,
                fs_context,
                bdev,
                queue,
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
