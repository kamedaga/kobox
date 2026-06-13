#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "kobox/device.h"
#include "kobox/device_linux_mock.h"
#include "kobox/interface_linux.h"
#include "kobox/module.h"
#include "kobox/platform.h"
#include "kobox/shim.h"
#include "kobox/device_linux_vfio_nvme.h"
#include "kobox/device_linux_vfio_virtio_blk.h"
#include "linux_subsystem/fs/fs.h"
#include "linux_subsystem/block/block.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <ucontext.h>
#endif
#endif

enum {
    KB_EXT4_DEPS_MAX = 16,
    KB_EXT4_COMMAND_MAX = 1024,
    KB_EXT4_IO_MAX = 8192,
    KB_EXT4_MULTI_PAYLOAD_SIZE = 3072,
    KB_EXT4_BOUNDARY_OFFSET = 900,
    KB_EXT4_BOUNDARY_SIZE = 512,
    KB_EXT4_FAKE_FILE_BYTES = 512,
    KB_EXT4_FAKE_KIOCB_BYTES = 128,
    KB_EXT4_FAKE_IOV_ITER_BYTES = 128,
    KB_EXT4_FAKE_MAPPING_BYTES = 256,
    KB_EXT4_FAKE_DENTRY_BYTES = 512,
    KB_EXT4_READDIR_OUTPUT_MAX = 1024,
    KB_EXT4_DENTRY_FLAGS_OFFSET = 0x0,
    KB_EXT4_DENTRY_NAME_HASH_OFFSET = 0x20,
    KB_EXT4_DENTRY_NAME_LEN_OFFSET = 0x24,
    KB_EXT4_DENTRY_NAME_PTR_OFFSET = 0x28,
    KB_EXT4_DENTRY_INODE_OFFSET = 0x38,
    KB_EXT4_FILE_MAPPING_OFFSET = 0x20,
    KB_EXT4_FILE_INODE_OFFSET = 0x28,
    KB_EXT4_KIOCB_FILE_OFFSET = 0x0,
    KB_EXT4_KIOCB_POS_OFFSET = 0x8,
    KB_EXT4_KIOCB_FLAGS_OFFSET = 0x20,
    KB_EXT4_IOV_ITER_COUNT_OFFSET = 0x18,
    KB_EXT4_IOV_ITER_BUFFER_OFFSET = 0x20,
};

typedef struct loaded_module {
    const char *path;
    void *data;
    size_t size;
    kb_module_t *module;
} loaded_module_t;

typedef struct ext4_ipc_target {
    const char *image_path;
    const char *work_dir;
    uint64_t mount_handle;
} ext4_ipc_target_t;

typedef struct ext4_operation_probe {
    void *dir_operations;
    void *file_operations;
    void *dir_inode_operations;
    void *file_inode_operations;
    void *readdir;
    void *file_read_iter;
    void *file_write_iter;
    void *lookup;
    int dir_operations_has_readdir;
    int file_operations_has_read_iter;
    int file_operations_has_write_iter;
    int dir_inode_operations_has_lookup;
} ext4_operation_probe_t;

typedef struct ext4_dir_context {
    int (*actor)(struct ext4_dir_context *ctx, const char *name, int name_len, int64_t offset, uint64_t inode, unsigned int d_type);
    int64_t pos;
} ext4_dir_context_t;

typedef struct ext4_readdir_capture {
    ext4_dir_context_t context;
    char names[KB_EXT4_READDIR_OUTPUT_MAX];
    size_t names_size;
    unsigned int entry_count;
} ext4_readdir_capture_t;

static const char kb_ext4_seed_payload[] = "kobox-ext4-module-read";
static const char kb_ext4_write_payload[] = "kobox-ext4-module-put!";

static void fill_multi_payload(uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)('A' + ((i * 7u) % 26u));
    }
}

static void fill_boundary_payload(uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)('a' + ((i * 5u) % 26u));
    }
}

#if !defined(_WIN32) && defined(__x86_64__)
static void segv_handler(int signal_number, siginfo_t *info, void *uctx)
{
    ucontext_t *context = (ucontext_t *)uctx;
    void *rip = (void *)context->uc_mcontext.gregs[REG_RIP];
    void *rdi = (void *)context->uc_mcontext.gregs[REG_RDI];
    void *rsi = (void *)context->uc_mcontext.gregs[REG_RSI];
    void *rbp = (void *)context->uc_mcontext.gregs[REG_RBP];
    void *r12 = (void *)context->uc_mcontext.gregs[REG_R12];
    char buffer[256];
    int length = snprintf(buffer, sizeof(buffer),
        "kobox-ext4-real-ops: signal=%d rip=%p fault=%p rdi=%p rsi=%p rbp=%p r12=%p\n",
        signal_number,
        rip,
        info == NULL ? NULL : info->si_addr,
        rdi,
        rsi,
        rbp,
        r12);
    if (length > 0) {
        (void)write(STDERR_FILENO, buffer, (size_t)length);
    }
    _exit(128 + signal_number);
}

static void install_signal_diagnostics(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = segv_handler;
    action.sa_flags = SA_SIGINFO;
    (void)sigaction(SIGSEGV, &action, NULL);
    (void)sigaction(SIGILL, &action, NULL);
    (void)sigaction(SIGBUS, &action, NULL);
    (void)sigaction(SIGABRT, &action, NULL);
}
#else
static void install_signal_diagnostics(void)
{
}
#endif

static kb_status_t read_file(const char *path, void **out_data, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return KB_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return KB_ERR_IO;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return KB_ERR_INVALID;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return KB_ERR_IO;
    }

    void *data = malloc((size_t)size);
    if (data == NULL) {
        fclose(file);
        return KB_ERR_NOMEM;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return KB_ERR_IO;
    }

    fclose(file);
    *out_data = data;
    *out_size = (size_t)size;
    return KB_OK;
}

static int write_file_bytes(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    int ok = fwrite(data, 1, size, file) == size;
    fclose(file);
    return ok;
}

static size_t read_file_bytes(const char *path, void *buffer, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    size_t size = fread(buffer, 1, capacity, file);
    fclose(file);
    return size;
}

static int run_command(const char *command)
{
    int rc = system(command);
    return rc == 0;
}

static int run_commandf(const char *fmt, const char *a, const char *b, const char *c)
{
    char command[KB_EXT4_COMMAND_MAX];
    int length = snprintf(command, sizeof(command), fmt, a, b, c);
    if (length <= 0 || (size_t)length >= sizeof(command)) {
        return 0;
    }
    return run_command(command);
}

static int prepare_ext4_image(const char *work_dir, const char *image_path, const char *mkfs_features)
{
    if (!run_commandf("mkdir -p '%s'", work_dir, "", "")) {
        return 0;
    }
    if (!run_commandf("dd if=/dev/zero of='%s' bs=1M count=32 status=none", image_path, "", "")) {
        return 0;
    }
    if (mkfs_features == NULL || mkfs_features[0] == '\0') {
        mkfs_features = "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index";
    }
    if (!run_commandf("mkfs.ext4 -q -F -O '%s' '%s'", mkfs_features, image_path, "")) {
        return 0;
    }
    char seed_path[KB_EXT4_COMMAND_MAX];
    int length = snprintf(seed_path, sizeof(seed_path), "%s/seed-read.txt", work_dir);
    if (length <= 0 ||
        (size_t)length >= sizeof(seed_path) ||
        !write_file_bytes(seed_path, kb_ext4_seed_payload, sizeof(kb_ext4_seed_payload) - 1u))
    {
        return 0;
    }
    if (!run_commandf("debugfs -w -R 'write %s /hello.txt' '%s' >/dev/null 2>&1", seed_path, image_path, "")) {
        return 0;
    }

    uint8_t multi_payload[KB_EXT4_MULTI_PAYLOAD_SIZE];
    fill_multi_payload(multi_payload, sizeof(multi_payload));
    char multi_path[KB_EXT4_COMMAND_MAX];
    length = snprintf(multi_path, sizeof(multi_path), "%s/seed-multi.bin", work_dir);
    if (length <= 0 ||
        (size_t)length >= sizeof(multi_path) ||
        !write_file_bytes(multi_path, multi_payload, sizeof(multi_payload)))
    {
        return 0;
    }
    return run_commandf("debugfs -w -R 'write %s /multi.txt' '%s' >/dev/null 2>&1", multi_path, image_path, "");
}

static kb_status_t load_module(
    kb_device_backend_t *backend,
    const char *path,
    loaded_module_t *out_loaded)
{
    memset(out_loaded, 0, sizeof(*out_loaded));
    out_loaded->path = path;

    kb_status_t status = read_file(path, &out_loaded->data, &out_loaded->size);
    if (status != KB_OK) {
        return status;
    }

    kb_module_image_t image = {
        .data = out_loaded->data,
        .size = out_loaded->size,
        .name = path,
    };
    status = kb_module_open_image(&image, backend, &out_loaded->module);
    if (status != KB_OK) {
        free(out_loaded->data);
        memset(out_loaded, 0, sizeof(*out_loaded));
    }
    return status;
}

static void unload_module(loaded_module_t *loaded)
{
    if (loaded->module != NULL) {
        kb_module_close(loaded->module);
    }
    free(loaded->data);
    memset(loaded, 0, sizeof(*loaded));
}

static int module_symbol(kb_module_t *module, const char *name, void **out_address)
{
    kb_status_t status = kb_module_find_symbol(module, name, out_address);
    if (status != KB_OK || *out_address == NULL) {
        fprintf(stderr, "missing module symbol %s status=%d\n", name, (int)status);
        return 0;
    }
    return 1;
}

static int table_contains_pointer(const void *table, size_t bytes, const void *target)
{
    if (table == NULL || target == NULL || bytes < sizeof(void *)) {
        return 0;
    }
    const unsigned char *cursor = (const unsigned char *)table;
    for (size_t offset = 0; offset + sizeof(void *) <= bytes; offset += sizeof(void *)) {
        void *entry = NULL;
        memcpy(&entry, cursor + offset, sizeof(entry));
        if (entry == target) {
            return 1;
        }
    }
    return 0;
}

static void write_pointer_field(void *base, size_t offset, void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void *read_pointer_field(const void *base, size_t offset)
{
    void *value = NULL;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void write_u32_field(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u64_field(void *base, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static int ext4_capture_dirent(
    ext4_dir_context_t *ctx,
    const char *name,
    int name_len,
    int64_t offset,
    uint64_t inode,
    unsigned int d_type)
{
    (void)offset;
    (void)inode;
    (void)d_type;
    ext4_readdir_capture_t *capture = (ext4_readdir_capture_t *)ctx;
    if (capture == NULL || name == NULL || name_len <= 0) {
        return 1;
    }
    size_t len = (size_t)name_len;
    if (capture->names_size + len + 1u < sizeof(capture->names)) {
        memcpy(capture->names + capture->names_size, name, len);
        capture->names_size += len;
        capture->names[capture->names_size++] = '\n';
    }
    capture->entry_count++;
    return 1;
}

static int probe_ext4_readdir(kb_module_t *module, const ext4_operation_probe_t *operation_probe, const kb_fs_mount_path_probe_t *mount_probe)
{
    if (module == NULL ||
        operation_probe == NULL ||
        mount_probe == NULL ||
        operation_probe->readdir == NULL ||
        mount_probe->root_inode == NULL)
    {
        return -22;
    }

    void *file = calloc(1, KB_EXT4_FAKE_FILE_BYTES);
    void *mapping = calloc(1, KB_EXT4_FAKE_MAPPING_BYTES);
    if (file == NULL || mapping == NULL) {
        free(file);
        free(mapping);
        return -12;
    }
    write_pointer_field(file, KB_EXT4_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KB_EXT4_FILE_INODE_OFFSET, mount_probe->root_inode);

    ext4_readdir_capture_t capture;
    memset(&capture, 0, sizeof(capture));
    capture.context.actor = ext4_capture_dirent;

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(operation_probe->readdir);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    int (*readdir_fn)(void *, void *) = NULL;
    memcpy(&readdir_fn, &operation_probe->readdir, sizeof(readdir_fn));
    int result = kb_linux_call_int_ptr_ptr_raw(
        readdir_fn,
        file,
        &capture.context);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    printf("module-vfs: readdir_result=%d entries=%u names=%.*s\n",
        result,
        capture.entry_count,
        (int)capture.names_size,
        capture.names);

    free(mapping);
    free(file);
    return result == 0 && capture.entry_count != 0 ? 0 : -5;
}

static int probe_ext4_lookup_name(
    kb_module_t *module,
    const ext4_operation_probe_t *operation_probe,
    const kb_fs_mount_path_probe_t *mount_probe,
    const char *lookup_name,
    void **out_inode)
{
    if (module == NULL ||
        operation_probe == NULL ||
        mount_probe == NULL ||
        operation_probe->lookup == NULL ||
        mount_probe->root_inode == NULL ||
        lookup_name == NULL)
    {
        return -22;
    }
    if (out_inode != NULL) {
        *out_inode = NULL;
    }

    void *dentry = calloc(1, KB_EXT4_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return -12;
    }

    write_u32_field(dentry, KB_EXT4_DENTRY_FLAGS_OFFSET, 0);
    write_u32_field(dentry, KB_EXT4_DENTRY_NAME_HASH_OFFSET, 0);
    write_u32_field(dentry, KB_EXT4_DENTRY_NAME_LEN_OFFSET, (uint32_t)strlen(lookup_name));
    write_pointer_field(dentry, KB_EXT4_DENTRY_NAME_PTR_OFFSET, (void *)(uintptr_t)lookup_name);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(operation_probe->lookup);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    void *(*lookup_fn)(void *, void *, unsigned int) = NULL;
    memcpy(&lookup_fn, &operation_probe->lookup, sizeof(lookup_fn));
    void *result = lookup_fn(mount_probe->root_inode, dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    void *inode = read_pointer_field(dentry, KB_EXT4_DENTRY_INODE_OFFSET);
    printf("module-vfs: lookup name=%s result=%p dentry=%p inode=%p\n",
        lookup_name,
        result,
        dentry,
        inode);

    int ok = result == dentry && inode != NULL;
    if (ok && out_inode != NULL) {
        *out_inode = inode;
    }
    free(dentry);
    return ok ? 0 : -5;
}

static int probe_ext4_file_read_iter(
    kb_module_t *module,
    const ext4_operation_probe_t *operation_probe,
    void *file_inode,
    const void *expected_payload,
    size_t expected_size,
    uint64_t offset,
    const char *label)
{
    if (module == NULL ||
        operation_probe == NULL ||
        operation_probe->file_read_iter == NULL ||
        file_inode == NULL ||
        expected_payload == NULL ||
        label == NULL)
    {
        return -22;
    }

    void *file = calloc(1, KB_EXT4_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, KB_EXT4_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, KB_EXT4_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, KB_EXT4_FAKE_MAPPING_BYTES);
    uint8_t *read_buffer = calloc(1, expected_size + 1u);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL || read_buffer == NULL) {
        free(file);
        free(kiocb);
        free(iter);
        free(mapping);
        free(read_buffer);
        return -12;
    }

    write_pointer_field(file, KB_EXT4_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KB_EXT4_FILE_INODE_OFFSET, file_inode);
    write_pointer_field(kiocb, KB_EXT4_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_EXT4_KIOCB_POS_OFFSET, offset);
    write_u32_field(kiocb, KB_EXT4_KIOCB_FLAGS_OFFSET, 0);
    write_u64_field(iter, KB_EXT4_IOV_ITER_COUNT_OFFSET, (uint64_t)expected_size);
    write_pointer_field(iter, KB_EXT4_IOV_ITER_BUFFER_OFFSET, read_buffer);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(operation_probe->file_read_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*read_iter_fn)(void *, void *) = NULL;
    memcpy(&read_iter_fn, &operation_probe->file_read_iter, sizeof(read_iter_fn));
    long result = read_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    printf("module-vfs: read_iter label=%s offset=%llu result=%ld sample=%.*s\n",
        label,
        (unsigned long long)offset,
        result,
        result > 0 ? (int)(result < 64 ? result : 64) : 0,
        read_buffer);

    int ok = result == (long)expected_size &&
        memcmp(read_buffer, expected_payload, expected_size) == 0;
    free(read_buffer);
    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
    return ok ? 0 : -5;
}

static int probe_ext4_file_write_iter(
    kb_module_t *module,
    const ext4_operation_probe_t *operation_probe,
    void *file_inode,
    const void *payload,
    size_t write_size,
    uint64_t offset,
    const char *label)
{
    if (module == NULL ||
        operation_probe == NULL ||
        operation_probe->file_write_iter == NULL ||
        file_inode == NULL ||
        payload == NULL ||
        label == NULL)
    {
        return -22;
    }

    void *file = calloc(1, KB_EXT4_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, KB_EXT4_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, KB_EXT4_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, KB_EXT4_FAKE_MAPPING_BYTES);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL) {
        free(file);
        free(kiocb);
        free(iter);
        free(mapping);
        return -12;
    }

    write_pointer_field(file, KB_EXT4_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KB_EXT4_FILE_INODE_OFFSET, file_inode);
    write_pointer_field(kiocb, KB_EXT4_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KB_EXT4_KIOCB_POS_OFFSET, offset);
    write_u32_field(kiocb, KB_EXT4_KIOCB_FLAGS_OFFSET, 0x2u);
    write_u64_field(iter, KB_EXT4_IOV_ITER_COUNT_OFFSET, (uint64_t)write_size);
    write_pointer_field(iter, KB_EXT4_IOV_ITER_BUFFER_OFFSET, (void *)(uintptr_t)payload);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(operation_probe->file_write_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*write_iter_fn)(void *, void *) = NULL;
    memcpy(&write_iter_fn, &operation_probe->file_write_iter, sizeof(write_iter_fn));
    long result = write_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    printf("module-vfs: write_iter label=%s offset=%llu result=%ld sample=%.*s\n",
        label,
        (unsigned long long)offset,
        result,
        result > 0 ? (int)(result < 64 ? result : 64) : 0,
        (const char *)payload);

    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
    return result == (long)write_size ? 0 : -5;
}

static int probe_ext4_operation_tables(kb_module_t *module, ext4_operation_probe_t *out_probe)
{
    if (module == NULL || out_probe == NULL) {
        return 0;
    }
    memset(out_probe, 0, sizeof(*out_probe));
    if (!module_symbol(module, "ext4_dir_operations", &out_probe->dir_operations) ||
        !module_symbol(module, "ext4_file_operations", &out_probe->file_operations) ||
        !module_symbol(module, "ext4_dir_inode_operations", &out_probe->dir_inode_operations) ||
        !module_symbol(module, "ext4_file_inode_operations", &out_probe->file_inode_operations) ||
        !module_symbol(module, "ext4_readdir", &out_probe->readdir) ||
        !module_symbol(module, "ext4_file_read_iter", &out_probe->file_read_iter) ||
        !module_symbol(module, "ext4_file_write_iter", &out_probe->file_write_iter) ||
        !module_symbol(module, "ext4_lookup", &out_probe->lookup))
    {
        return 0;
    }

    out_probe->dir_operations_has_readdir =
        table_contains_pointer(out_probe->dir_operations, 256u, out_probe->readdir);
    out_probe->file_operations_has_read_iter =
        table_contains_pointer(out_probe->file_operations, 256u, out_probe->file_read_iter);
    out_probe->file_operations_has_write_iter =
        table_contains_pointer(out_probe->file_operations, 256u, out_probe->file_write_iter);
    out_probe->dir_inode_operations_has_lookup =
        table_contains_pointer(out_probe->dir_inode_operations, 256u, out_probe->lookup);

    return out_probe->dir_operations_has_readdir &&
        out_probe->file_operations_has_read_iter &&
        out_probe->file_operations_has_write_iter &&
        out_probe->dir_inode_operations_has_lookup;
}

static kb_status_t ext4_mount(ext4_ipc_target_t *target, kb_fs_ipc_request_t *request)
{
    if (request->fs_type == NULL || strcmp(request->fs_type, "ext4") != 0) {
        return KB_ERR_NOT_FOUND;
    }
    if (!run_commandf("debugfs -w -R 'mkdir /kobox' '%s' >/dev/null 2>&1 || true",
            target->image_path,
            "",
            ""))
    {
        return KB_ERR_IO;
    }
    target->mount_handle = 1;
    request->handle = target->mount_handle;
    request->handled = 1;
    request->result_code = 0;
    return KB_OK;
}

static kb_status_t ext4_write(ext4_ipc_target_t *target, kb_fs_ipc_request_t *request)
{
    if (request->handle != target->mount_handle ||
        request->path == NULL ||
        strcmp(request->path, "/kobox/hello.txt") != 0 ||
        request->offset != 0 ||
        request->input == NULL)
    {
        return KB_ERR_INVALID;
    }

    char input_path[KB_EXT4_COMMAND_MAX];
    int length = snprintf(input_path, sizeof(input_path), "%s/write-input.bin", target->work_dir);
    if (length <= 0 || (size_t)length >= sizeof(input_path) ||
        !write_file_bytes(input_path, request->input, request->input_size))
    {
        return KB_ERR_IO;
    }

    if (!run_commandf("debugfs -w -R 'rm /kobox/hello.txt' '%s' >/dev/null 2>&1 || true",
            target->image_path,
            "",
            ""))
    {
        return KB_ERR_IO;
    }
    if (!run_commandf("debugfs -w -R 'write %s /kobox/hello.txt' '%s' >/dev/null 2>&1",
            input_path,
            target->image_path,
            ""))
    {
        return KB_ERR_IO;
    }

    request->handled = 1;
    request->output_size = request->input_size;
    request->result_code = 0;
    return KB_OK;
}

static kb_status_t ext4_read(ext4_ipc_target_t *target, kb_fs_ipc_request_t *request)
{
    if (request->handle != target->mount_handle ||
        request->path == NULL ||
        strcmp(request->path, "/kobox/hello.txt") != 0 ||
        request->output == NULL)
    {
        return KB_ERR_INVALID;
    }

    char output_path[KB_EXT4_COMMAND_MAX];
    int length = snprintf(output_path, sizeof(output_path), "%s/read-output.bin", target->work_dir);
    if (length <= 0 || (size_t)length >= sizeof(output_path)) {
        return KB_ERR_IO;
    }

    if (!run_commandf("debugfs -R 'cat /kobox/hello.txt' '%s' > '%s' 2>/dev/null",
            target->image_path,
            output_path,
            ""))
    {
        return KB_ERR_IO;
    }

    uint8_t temp[KB_EXT4_IO_MAX];
    size_t size = read_file_bytes(output_path, temp, sizeof(temp));
    if (request->offset > size) {
        request->output_size = 0;
    } else {
        size_t available = size - (size_t)request->offset;
        size_t copy_size = available < request->output_capacity ? available : request->output_capacity;
        memcpy(request->output, temp + request->offset, copy_size);
        request->output_size = copy_size;
    }

    request->handled = 1;
    request->result_code = 0;
    return KB_OK;
}

static kb_status_t ext4_readdir(ext4_ipc_target_t *target, kb_fs_ipc_request_t *request)
{
    if (request->handle != target->mount_handle ||
        request->path == NULL ||
        strcmp(request->path, "/kobox") != 0 ||
        request->output == NULL)
    {
        return KB_ERR_INVALID;
    }

    char output_path[KB_EXT4_COMMAND_MAX];
    int length = snprintf(output_path, sizeof(output_path), "%s/readdir-output.txt", target->work_dir);
    if (length <= 0 || (size_t)length >= sizeof(output_path)) {
        return KB_ERR_IO;
    }
    if (!run_commandf("debugfs -R 'ls -p /kobox' '%s' > '%s' 2>/dev/null",
            target->image_path,
            output_path,
            ""))
    {
        return KB_ERR_IO;
    }

    request->output_size = read_file_bytes(output_path, request->output, request->output_capacity);
    request->handled = 1;
    request->result_code = 0;
    return KB_OK;
}

static kb_status_t ext4_ipc_dispatch(void *ctx, const void *message, size_t message_size)
{
    ext4_ipc_target_t *target = (ext4_ipc_target_t *)ctx;
    kb_fs_ipc_request_t *request = (kb_fs_ipc_request_t *)message;
    if (target == NULL || request == NULL || message_size != sizeof(*request)) {
        return KB_ERR_INVALID;
    }

    switch (request->operation) {
    case KB_FS_OPERATION_MOUNT:
        return ext4_mount(target, request);
    case KB_FS_OPERATION_WRITE:
        return ext4_write(target, request);
    case KB_FS_OPERATION_READ:
        return ext4_read(target, request);
    case KB_FS_OPERATION_READDIR:
        return ext4_readdir(target, request);
    default:
        return KB_ERR_INVALID;
    }
}

static int run_fs_ops(const char *work_dir, const char *image_path)
{
    ext4_ipc_target_t target = {
        .image_path = image_path,
        .work_dir = work_dir,
    };

    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        fprintf(stderr, "failed to create mock backend\n");
        return 1;
    }

    kb_interface_t *fs_interface = NULL;
    kb_linux_interface_desc_t fs_interface_desc = {
        .name = "linux-ipc-ext4-real-ops",
        .subsystem = "fs",
        .endpoint = "kobox.fs.ext4.image",
        .dispatch = ext4_ipc_dispatch,
        .dispatch_ctx = &target,
    };
    if (kb_linux_ipc_interface_create(&fs_interface_desc, &fs_interface) != KB_OK || fs_interface == NULL) {
        kb_device_backend_destroy(backend);
        fprintf(stderr, "failed to create fs ipc interface\n");
        return 1;
    }

    kb_interface_t *interfaces[] = {
        fs_interface,
    };
    kb_platform_desc_t platform_desc = {
        "linux-ext4-real-ops-platform",
        backend,
        interfaces,
        1,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK || platform == NULL) {
        kb_interface_destroy(fs_interface);
        kb_device_backend_destroy(backend);
        fprintf(stderr, "failed to create platform\n");
        return 1;
    }

    kb_interface_t *bound = NULL;
    if (kb_fs_subsystem_bind_ipc_interface(platform, &bound) != KB_OK || bound == NULL) {
        kb_platform_destroy(platform);
        fprintf(stderr, "failed to bind fs ipc interface\n");
        return 1;
    }

    uint64_t handle = 0;
    kb_fs_mount_desc_t mount_desc = {
        .source = image_path,
        .target = "/kobox",
        .fs_type = "ext4",
    };
    if (kb_fs_subsystem_mount(bound, &mount_desc, &handle) != KB_OK || handle == 0) {
        kb_platform_destroy(platform);
        fprintf(stderr, "ext4 image mount operation failed\n");
        return 1;
    }

    const char payload[] = "kobox-real-ext4-write";
    size_t written = 0;
    kb_fs_write_desc_t write_desc = {
        .handle = handle,
        .path = "/kobox/hello.txt",
        .buffer = payload,
        .byte_count = sizeof(payload) - 1u,
        .out_bytes = &written,
    };
    if (kb_fs_subsystem_write(bound, &write_desc) != KB_OK || written != sizeof(payload) - 1u) {
        kb_platform_destroy(platform);
        fprintf(stderr, "ext4 image write operation failed\n");
        return 1;
    }

    char read_buffer[128] = {0};
    size_t read_bytes = 0;
    kb_fs_read_desc_t read_desc = {
        .handle = handle,
        .path = "/kobox/hello.txt",
        .buffer = read_buffer,
        .byte_count = sizeof(read_buffer),
        .out_bytes = &read_bytes,
    };
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK ||
        read_bytes != sizeof(payload) - 1u ||
        memcmp(read_buffer, payload, sizeof(payload) - 1u) != 0)
    {
        kb_platform_destroy(platform);
        fprintf(stderr, "ext4 image read operation failed\n");
        return 1;
    }

    char dir_buffer[KB_EXT4_IO_MAX] = {0};
    size_t dir_bytes = 0;
    kb_fs_readdir_desc_t readdir_desc = {
        .handle = handle,
        .path = "/kobox",
        .buffer = dir_buffer,
        .byte_count = sizeof(dir_buffer) - 1u,
        .out_bytes = &dir_bytes,
    };
    if (kb_fs_subsystem_readdir(bound, &readdir_desc) != KB_OK ||
        dir_bytes == 0 ||
        strstr(dir_buffer, "hello.txt") == NULL)
    {
        kb_platform_destroy(platform);
        fprintf(stderr, "ext4 image readdir operation failed\n");
        return 1;
    }

    printf("mount: source=%s target=/kobox fs=ext4 handle=%llu\n",
        image_path,
        (unsigned long long)handle);
    printf("write: /kobox/hello.txt bytes=%zu\n", written);
    printf("read: /kobox/hello.txt bytes=%zu data=%.*s\n", read_bytes, (int)read_bytes, read_buffer);
    printf("readdir: /kobox contains hello.txt\n");

    kb_interface_unbind(bound);
    kb_platform_destroy(platform);
    return 0;
}

static const char *mount_path_stop_reason(const kb_fs_mount_path_probe_t *probe)
{
    if (probe == NULL) {
        return "not-observed";
    }
    if (probe->fill_super_result == -117 && probe->observed_ext4_magic == 0xef53) {
        int read_superblock = 0;
        int read_group_descriptor = 0;
        int read_inode_table = 0;
        for (uint32_t i = 0; i < probe->block_read_count; i++) {
            uint64_t block = probe->block_read_numbers[i];
            uint32_t size = probe->block_read_sizes[i];
            if ((block == 1u && size == 1024u) || (block == 0u && size >= 4096u)) {
                read_superblock = 1;
            }
            if (block == 1u && size >= 4096u) {
                read_group_descriptor = 1;
            }
            if (block > 1u && size >= 4096u) {
                read_inode_table = 1;
            }
        }
        if (read_superblock != 0 && read_group_descriptor != 0 && read_inode_table != 0) {
            return "metadata-integrity/inode-table";
        }
        if (read_superblock != 0 && read_group_descriptor != 0) {
            return "metadata-integrity/group-descriptor";
        }
        return "metadata-integrity";
    }
    if (probe->fill_super_result == 0) {
        return "mounted";
    }
    if (probe->bdev_getblk_calls == 0) {
        return "before-block-read";
    }
    return "not-classified";
}

int main(int argc, char **argv)
{
    install_signal_diagnostics();

    const char *deps[KB_EXT4_DEPS_MAX];
    size_t dep_count = 0;
    const char *ext4_path = NULL;
    const char *work_dir = ".artifacts/ext4-real-ops";
    const char *image_path = ".artifacts/ext4-real-ops/probe.img";
    const char *mkfs_features = "^has_journal,^extent,^64bit,^metadata_csum,^extra_isize,^dir_index";
    const char *vfio_nvme_bdf = NULL;
    const char *vfio_virtio_blk_bdf = NULL;
    int skip_prepare = 0;
    int skip_interface_ops = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--dep=", 6) == 0) {
            if (dep_count == KB_EXT4_DEPS_MAX) {
                fprintf(stderr, "too many dependencies\n");
                return 2;
            }
            deps[dep_count++] = argv[i] + 6;
        } else if (strncmp(argv[i], "--work-dir=", 11) == 0) {
            work_dir = argv[i] + 11;
        } else if (strncmp(argv[i], "--image=", 8) == 0) {
            image_path = argv[i] + 8;
        } else if (strncmp(argv[i], "--mkfs-features=", 16) == 0) {
            mkfs_features = argv[i] + 16;
        } else if (strncmp(argv[i], "--vfio-nvme=", 12) == 0) {
            vfio_nvme_bdf = argv[i] + 12;
        } else if (strncmp(argv[i], "--vfio-virtio-blk=", 18) == 0) {
            vfio_virtio_blk_bdf = argv[i] + 18;
        } else if (strcmp(argv[i], "--skip-prepare") == 0) {
            skip_prepare = 1;
        } else if (strcmp(argv[i], "--skip-interface-ops") == 0) {
            skip_interface_ops = 1;
        } else if (ext4_path == NULL) {
            ext4_path = argv[i];
        } else {
            fprintf(stderr, "usage: %s [--dep=module.ko ...] [--work-dir=dir] [--image=img] [--mkfs-features=features] [--vfio-nvme=BDF] [--vfio-virtio-blk=BDF] [--skip-prepare] [--skip-interface-ops] ext4.ko\n", argv[0]);
            return 2;
        }
    }

    if (ext4_path == NULL) {
        fprintf(stderr, "usage: %s [--dep=module.ko ...] [--work-dir=dir] [--image=img] [--mkfs-features=features] [--vfio-nvme=BDF] [--vfio-virtio-blk=BDF] [--skip-prepare] [--skip-interface-ops] ext4.ko\n", argv[0]);
        return 2;
    }
    if (vfio_nvme_bdf != NULL && vfio_virtio_blk_bdf != NULL) {
        fprintf(stderr, "--vfio-nvme and --vfio-virtio-blk are mutually exclusive\n");
        return 2;
    }
    if (!skip_prepare) {
        if (!run_command("command -v mkfs.ext4 >/dev/null 2>&1") ||
            !run_command("command -v debugfs >/dev/null 2>&1"))
        {
            fprintf(stderr, "mkfs.ext4 and debugfs are required for real ext4 image operations\n");
            return 77;
        }
        if (!prepare_ext4_image(work_dir, image_path, mkfs_features)) {
            fprintf(stderr, "failed to prepare ext4 image\n");
            return 1;
        }
    }

    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        fprintf(stderr, "failed to create module backend\n");
        return 1;
    }

    loaded_module_t loaded_deps[KB_EXT4_DEPS_MAX];
    memset(loaded_deps, 0, sizeof(loaded_deps));
    loaded_module_t ext4_module;
    memset(&ext4_module, 0, sizeof(ext4_module));

    for (size_t i = 0; i < dep_count; i++) {
        kb_status_t status = load_module(backend, deps[i], &loaded_deps[i]);
        if (status != KB_OK) {
            fprintf(stderr, "failed to open dependency %s\n", deps[i]);
            kb_device_backend_destroy(backend);
            return 1;
        }
        int init_result = 0;
        status = kb_module_call_init(loaded_deps[i].module, &init_result);
        if (status == KB_OK) {
            printf("dependency %s init_module returned %d\n", deps[i], init_result);
        } else if (status == KB_ERR_NOT_FOUND) {
            printf("dependency %s has no init_module\n", deps[i]);
        } else {
            fprintf(stderr, "dependency %s init failed\n", deps[i]);
            kb_device_backend_destroy(backend);
            return 1;
        }
    }

    if (load_module(backend, ext4_path, &ext4_module) != KB_OK) {
        fprintf(stderr, "failed to open ext4 module %s\n", ext4_path);
        kb_device_backend_destroy(backend);
        return 1;
    }

    int ext4_init_result = 0;
    kb_status_t status = kb_module_call_init(ext4_module.module, &ext4_init_result);
    if (status != KB_OK || ext4_init_result != 0) {
        fprintf(stderr, "ext4 init failed status=%d result=%d\n", (int)status, ext4_init_result);
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("ext4 init_module returned %d\n", ext4_init_result);

    ext4_operation_probe_t operation_probe;
    if (!probe_ext4_operation_tables(ext4_module.module, &operation_probe)) {
        fprintf(stderr, "ext4 operation table probe failed\n");
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("module-vfs: dir_operations=%p readdir=%p linked=%d\n",
        operation_probe.dir_operations,
        operation_probe.readdir,
        operation_probe.dir_operations_has_readdir);
    printf("module-vfs: file_operations=%p read_iter=%p write_iter=%p linked=%d/%d\n",
        operation_probe.file_operations,
        operation_probe.file_read_iter,
        operation_probe.file_write_iter,
        operation_probe.file_operations_has_read_iter,
        operation_probe.file_operations_has_write_iter);
    printf("module-vfs: dir_inode_operations=%p lookup=%p linked=%d file_inode_operations=%p\n",
        operation_probe.dir_inode_operations,
        operation_probe.lookup,
        operation_probe.dir_inode_operations_has_lookup,
        operation_probe.file_inode_operations);

    kb_fs_type_snapshot_t snapshot;
    if (kb_fs_subsystem_type_snapshot("ext4", &snapshot) != 0 || snapshot.fs_type == NULL) {
        fprintf(stderr, "ext4 filesystem type was not registered by the module\n");
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("registered filesystem: name=%s fs_type=%p\n", snapshot.name, snapshot.fs_type);

    kb_linux_vfio_nvme_provider_t *vfio_nvme = NULL;
    kb_linux_vfio_virtio_blk_provider_t *vfio_virtio_blk = NULL;
    kb_fs_block_device_t *block_device = NULL;
    int block_status = 0;
    if (vfio_nvme_bdf != NULL) {
        block_status = kb_linux_vfio_nvme_provider_create(vfio_nvme_bdf, &vfio_nvme);
        if (block_status == KB_OK) {
            block_status = kb_fs_block_device_create_from_disk(
                "ext4-vfio-nvme",
                kb_linux_vfio_nvme_provider_disk(vfio_nvme),
                &block_device);
        }
    } else if (vfio_virtio_blk_bdf != NULL) {
        block_status = kb_linux_vfio_virtio_blk_provider_create(vfio_virtio_blk_bdf, &vfio_virtio_blk);
        if (block_status == KB_OK) {
            block_status = kb_fs_block_device_create_from_disk(
                "ext4-vfio-virtio-blk",
                kb_linux_vfio_virtio_blk_provider_disk(vfio_virtio_blk),
                &block_device);
        }
    } else {
        block_status = kb_fs_block_device_create_image("ext4-probe-image", image_path, &block_device);
    }
    if (block_status != 0 || block_device == NULL || kb_fs_subsystem_set_mount_probe_block_device(block_device) != 0) {
        fprintf(stderr, "failed to attach ext4 image block device to fs mount probe\n");
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }

    kb_fs_mount_path_probe_t mount_path_probe;
    int mount_path_result = kb_fs_subsystem_probe_registered_mount_path("ext4", &mount_path_probe);
    if (mount_path_probe.get_tree_bdev_calls == 0 ||
        mount_path_probe.get_tree_bdev_fill_super == NULL ||
        mount_path_probe.bdev_getblk_calls == 0 ||
        mount_path_probe.observed_ext4_magic != 0xef53 ||
        mount_path_result != -95)
    {
        fprintf(stderr,
            "ext4 module VFS mount path did not read ext4 superblock result=%d calls=%llu fill_super=%p bdev_calls=%llu magic=0x%04x\n",
            mount_path_result,
            (unsigned long long)mount_path_probe.get_tree_bdev_calls,
            mount_path_probe.get_tree_bdev_fill_super,
            (unsigned long long)mount_path_probe.bdev_getblk_calls,
            mount_path_probe.observed_ext4_magic);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("module-vfs: init_fs_context=%p init_result=%d get_tree=%p get_tree_result=%d\n",
        mount_path_probe.init_fs_context,
        mount_path_probe.init_result,
        mount_path_probe.get_tree,
        mount_path_probe.get_tree_result);
    printf("module-vfs: get_tree_bdev fc=%p fill_super=%p calls=%llu\n",
        mount_path_probe.get_tree_bdev_fc,
        mount_path_probe.get_tree_bdev_fill_super,
        (unsigned long long)mount_path_probe.get_tree_bdev_calls);
    printf("module-vfs: fill_super_result=%d bdev_getblk_calls=%llu block=%llu block_size=%u magic=0x%04x\n",
        mount_path_probe.fill_super_result,
        (unsigned long long)mount_path_probe.bdev_getblk_calls,
        (unsigned long long)mount_path_probe.last_block_number,
        mount_path_probe.last_block_size,
        mount_path_probe.observed_ext4_magic);
    printf("module-vfs: root_inode=%p root_dentry=%p\n",
        mount_path_probe.root_inode,
        mount_path_probe.root_dentry);
    printf("module-vfs: block_reads=");
    for (uint32_t i = 0; i < mount_path_probe.block_read_count; i++) {
        printf("%s%llu:%u",
            i == 0 ? "" : ",",
            (unsigned long long)mount_path_probe.block_read_numbers[i],
            mount_path_probe.block_read_sizes[i]);
    }
    if (mount_path_probe.block_read_count == 0) {
        printf("none");
    }
    printf("\n");
    printf("module-vfs: stop=%s\n", mount_path_stop_reason(&mount_path_probe));
    int readdir_probe_result = probe_ext4_readdir(ext4_module.module, &operation_probe, &mount_path_probe);
    if (readdir_probe_result != 0) {
        fprintf(stderr, "ext4 module readdir operation probe failed result=%d\n", readdir_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    int lookup_probe_result = probe_ext4_lookup_name(
        ext4_module.module,
        &operation_probe,
        &mount_path_probe,
        "lost+found",
        NULL);
    if (lookup_probe_result != 0) {
        fprintf(stderr, "ext4 module lookup operation probe failed name=lost+found result=%d\n", lookup_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    void *hello_inode = NULL;
    lookup_probe_result = probe_ext4_lookup_name(
        ext4_module.module,
        &operation_probe,
        &mount_path_probe,
        "hello.txt",
        &hello_inode);
    if (lookup_probe_result != 0) {
        fprintf(stderr, "ext4 module lookup operation probe failed name=hello.txt result=%d\n", lookup_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("module-vfs: regular_file_inode=%p\n", hello_inode);
    int read_iter_probe_result = probe_ext4_file_read_iter(
        ext4_module.module,
        &operation_probe,
        hello_inode,
        kb_ext4_seed_payload,
        sizeof(kb_ext4_seed_payload) - 1u,
        0,
        "hello-initial");
    if (read_iter_probe_result != 0) {
        fprintf(stderr, "ext4 module read_iter operation probe failed result=%d\n", read_iter_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    int write_iter_probe_result = probe_ext4_file_write_iter(
        ext4_module.module,
        &operation_probe,
        hello_inode,
        kb_ext4_write_payload,
        sizeof(kb_ext4_write_payload) - 1u,
        0,
        "hello-overwrite");
    if (write_iter_probe_result != 0) {
        fprintf(stderr, "ext4 module write_iter operation probe failed result=%d\n", write_iter_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    read_iter_probe_result = probe_ext4_file_read_iter(
        ext4_module.module,
        &operation_probe,
        hello_inode,
        kb_ext4_write_payload,
        sizeof(kb_ext4_write_payload) - 1u,
        0,
        "hello-post-write");
    if (read_iter_probe_result != 0) {
        fprintf(stderr, "ext4 module read_iter post-write probe failed result=%d\n", read_iter_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }

    void *multi_inode = NULL;
    lookup_probe_result = probe_ext4_lookup_name(
        ext4_module.module,
        &operation_probe,
        &mount_path_probe,
        "multi.txt",
        &multi_inode);
    if (lookup_probe_result != 0) {
        fprintf(stderr, "ext4 module lookup operation probe failed name=multi.txt result=%d\n", lookup_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    uint8_t multi_payload[KB_EXT4_MULTI_PAYLOAD_SIZE];
    fill_multi_payload(multi_payload, sizeof(multi_payload));
    read_iter_probe_result = probe_ext4_file_read_iter(
        ext4_module.module,
        &operation_probe,
        multi_inode,
        multi_payload,
        sizeof(multi_payload),
        0,
        "multi-full");
    if (read_iter_probe_result != 0) {
        fprintf(stderr, "ext4 module multi-block read_iter probe failed result=%d\n", read_iter_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    uint8_t boundary_payload[KB_EXT4_BOUNDARY_SIZE];
    fill_boundary_payload(boundary_payload, sizeof(boundary_payload));
    write_iter_probe_result = probe_ext4_file_write_iter(
        ext4_module.module,
        &operation_probe,
        multi_inode,
        boundary_payload,
        sizeof(boundary_payload),
        KB_EXT4_BOUNDARY_OFFSET,
        "multi-boundary-write");
    if (write_iter_probe_result != 0) {
        fprintf(stderr, "ext4 module multi-block write_iter probe failed result=%d\n", write_iter_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    read_iter_probe_result = probe_ext4_file_read_iter(
        ext4_module.module,
        &operation_probe,
        multi_inode,
        boundary_payload,
        sizeof(boundary_payload),
        KB_EXT4_BOUNDARY_OFFSET,
        "multi-boundary-post-write");
    if (read_iter_probe_result != 0) {
        fprintf(stderr, "ext4 module multi-block read_iter post-write probe failed result=%d\n", read_iter_probe_result);
        kb_fs_subsystem_set_mount_probe_block_device(NULL);
        kb_fs_block_device_destroy(block_device);
        kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
        kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);
        kb_device_backend_destroy(backend);
        return 1;
    }
    kb_fs_subsystem_set_mount_probe_block_device(NULL);
    kb_fs_block_device_destroy(block_device);
    kb_linux_vfio_nvme_provider_destroy(vfio_nvme);
    kb_linux_vfio_virtio_blk_provider_destroy(vfio_virtio_blk);

    int result = skip_interface_ops ? 0 : run_fs_ops(work_dir, image_path);

    if (kb_module_call_cleanup(ext4_module.module) == KB_OK) {
        printf("ext4 cleanup_module returned\n");
    }
    unload_module(&ext4_module);

    for (size_t i = dep_count; i > 0; i--) {
        loaded_module_t *dep = &loaded_deps[i - 1u];
        if (dep->module != NULL && kb_module_call_cleanup(dep->module) == KB_OK) {
            printf("dependency %s cleanup_module returned\n", dep->path);
        }
        unload_module(dep);
    }

    kb_device_backend_destroy(backend);
    return result;
}
