#include "kobox/device.h"
#include "kobox/device_linux_mock.h"
#include "kobox/interface_linux.h"
#include "kobox/platform.h"
#include "linux_subsystem/fs/fs.h"

#include <stdint.h>
#include <string.h>

typedef struct fs_ipc_smoke {
    int mount_seen;
    int read_seen;
    int write_seen;
    int readdir_seen;
} fs_ipc_smoke_t;

static kb_status_t fs_ipc_dispatch(void *ctx, const void *message, size_t message_size)
{
    fs_ipc_smoke_t *smoke = (fs_ipc_smoke_t *)ctx;
    kb_fs_ipc_request_t *request = (kb_fs_ipc_request_t *)message;
    if (smoke == NULL ||
        request == NULL ||
        message_size != sizeof(*request) ||
        request->version != 1)
    {
        return KB_ERR_INVALID;
    }

    switch (request->operation) {
    case KB_FS_OPERATION_MOUNT:
        if (strcmp(request->source, "/dev/kobox0") != 0 ||
            strcmp(request->path, "/mnt/kobox") != 0 ||
            strcmp(request->fs_type, "ext4") != 0)
        {
            return KB_ERR_INVALID;
        }
        smoke->mount_seen++;
        request->handled = 1;
        request->handle = 7;
        request->result_code = 0;
        return KB_OK;
    case KB_FS_OPERATION_READ:
        if (request->handle != 7 || request->output == NULL || request->output_capacity < 5) {
            return KB_ERR_INVALID;
        }
        if (request->path != NULL && strcmp(request->path, "/mnt/kobox/empty") == 0) {
            request->handled = 1;
            request->output_size = 0;
            request->result_code = 0;
            smoke->read_seen++;
            return KB_OK;
        }
        memcpy(request->output, "hello", 5);
        request->handled = 1;
        request->output_size = 5;
        request->result_code = 0;
        smoke->read_seen++;
        return KB_OK;
    case KB_FS_OPERATION_WRITE:
        if (request->handle != 7 ||
            request->input == NULL ||
            request->input_size != 5 ||
            memcmp(request->input, "world", 5) != 0)
        {
            return KB_ERR_INVALID;
        }
        request->handled = 1;
        request->output_size = request->input_size;
        request->result_code = 0;
        smoke->write_seen++;
        return KB_OK;
    case KB_FS_OPERATION_READDIR:
        if (request->handle != 7 || request->output == NULL || request->output_capacity < 8) {
            return KB_ERR_INVALID;
        }
        memcpy(request->output, "a\nb\n", 4);
        request->handled = 1;
        request->output_size = 4;
        request->result_code = 0;
        smoke->readdir_seen++;
        return KB_OK;
    default:
        return KB_ERR_INVALID;
    }
}

typedef struct fake_file_system_type {
    const char *name;
} fake_file_system_type_t;

static int run_local_fs_model_smoke(void)
{
    fake_file_system_type_t ext4_type = {
        .name = "ext4",
    };
    if (kb_fs_subsystem_register_filesystem(&ext4_type) != 0 ||
        kb_fs_subsystem_registered_type_count() == 0)
    {
        return 20;
    }

    kb_fs_type_snapshot_t type_snapshot;
    if (kb_fs_subsystem_type_snapshot("ext4", &type_snapshot) != 0 ||
        type_snapshot.fs_type != &ext4_type ||
        strcmp(type_snapshot.name, "ext4") != 0 ||
        type_snapshot.register_count != 1)
    {
        return 21;
    }

    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 22;
    }

    kb_interface_t *fs_interface = NULL;
    kb_linux_interface_desc_t fs_interface_desc = {
        .name = "linux-ipc-fs-local",
        .subsystem = "fs",
        .endpoint = "kobox.fs",
    };
    if (kb_linux_ipc_interface_create(&fs_interface_desc, &fs_interface) != KB_OK || fs_interface == NULL) {
        kb_device_backend_destroy(backend);
        return 23;
    }

    kb_interface_t *interfaces[] = {
        fs_interface,
    };
    kb_platform_desc_t platform_desc = {
        "linux-fs-local-platform",
        backend,
        interfaces,
        1,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK || platform == NULL) {
        kb_interface_destroy(fs_interface);
        kb_device_backend_destroy(backend);
        return 24;
    }

    kb_interface_t *bound = NULL;
    if (kb_fs_subsystem_bind_ipc_interface(platform, &bound) != KB_OK || bound != fs_interface) {
        kb_platform_destroy(platform);
        return 25;
    }

    uint64_t handle = 0;
    void *block_disk = (void *)(uintptr_t)0x55;
    kb_fs_mount_desc_t mount_desc = {
        .source = "/dev/kobox-ext4",
        .target = "/mnt/ext4",
        .fs_type = "ext4",
        .block_disk = block_disk,
    };
    if (kb_fs_subsystem_mount(bound, &mount_desc, &handle) != KB_OK || handle == 0) {
        kb_platform_destroy(platform);
        return 26;
    }

    kb_fs_mount_snapshot_t mount_snapshot;
    if (kb_fs_subsystem_mount_snapshot(handle, &mount_snapshot) != 0 ||
        strcmp(mount_snapshot.fs_type, "ext4") != 0 ||
        strcmp(mount_snapshot.target, "/mnt/ext4") != 0 ||
        mount_snapshot.block_disk != block_disk)
    {
        kb_platform_destroy(platform);
        return 27;
    }

    size_t written = 0;
    kb_fs_write_desc_t write_desc = {
        .handle = handle,
        .path = "/mnt/ext4/hello.txt",
        .offset = 0,
        .buffer = "real-ext4-path",
        .byte_count = 14,
        .out_bytes = &written,
    };
    if (kb_fs_subsystem_write(bound, &write_desc) != KB_OK || written != 14) {
        kb_platform_destroy(platform);
        return 28;
    }

    char read_buffer[32] = {0};
    size_t read = 0;
    kb_fs_read_desc_t read_desc = {
        .handle = handle,
        .path = "/mnt/ext4/hello.txt",
        .offset = 5,
        .buffer = read_buffer,
        .byte_count = sizeof(read_buffer),
        .out_bytes = &read,
    };
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK ||
        read != 9 ||
        memcmp(read_buffer, "ext4-path", 9) != 0)
    {
        kb_platform_destroy(platform);
        return 29;
    }

    char dir_buffer[128] = {0};
    size_t dir_bytes = 0;
    kb_fs_readdir_desc_t readdir_desc = {
        .handle = handle,
        .path = "/mnt/ext4",
        .buffer = dir_buffer,
        .byte_count = sizeof(dir_buffer),
        .out_bytes = &dir_bytes,
    };
    if (kb_fs_subsystem_readdir(bound, &readdir_desc) != KB_OK ||
        dir_bytes == 0 ||
        strstr(dir_buffer, "/mnt/ext4/hello.txt") == NULL)
    {
        kb_platform_destroy(platform);
        return 30;
    }

    kb_interface_unbind(bound);
    kb_platform_destroy(platform);
    if (kb_fs_subsystem_unregister_filesystem(&ext4_type) != 0) {
        return 31;
    }
    return 0;
}

int main(void)
{
    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 1;
    }

    kb_interface_t *fs_interface = NULL;
    fs_ipc_smoke_t smoke = {0};
    kb_linux_interface_desc_t fs_interface_desc = {
        .name = "linux-ipc-fs",
        .subsystem = "fs",
        .endpoint = "kobox.fs",
        .dispatch = fs_ipc_dispatch,
        .dispatch_ctx = &smoke,
    };
    if (kb_linux_ipc_interface_create(&fs_interface_desc, &fs_interface) != KB_OK || fs_interface == NULL) {
        kb_device_backend_destroy(backend);
        return 2;
    }

    kb_interface_t *interfaces[] = {
        fs_interface,
    };
    kb_platform_desc_t platform_desc = {
        "linux-fs-platform",
        backend,
        interfaces,
        1,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK || platform == NULL) {
        kb_interface_destroy(fs_interface);
        kb_device_backend_destroy(backend);
        return 3;
    }

    kb_interface_t *bound = NULL;
    if (kb_fs_subsystem_bind_ipc_interface(platform, &bound) != KB_OK || bound != fs_interface) {
        kb_platform_destroy(platform);
        return 4;
    }

    uint64_t mount_handle = 0;
    kb_fs_mount_desc_t mount_desc = {
        .source = "/dev/kobox0",
        .target = "/mnt/kobox",
        .fs_type = "ext4",
    };
    if (kb_fs_subsystem_mount(bound, &mount_desc, &mount_handle) != KB_OK || mount_handle != 7) {
        kb_platform_destroy(platform);
        return 5;
    }

    char read_buffer[8] = {0};
    size_t read_bytes = 0;
    kb_fs_read_desc_t read_desc = {
        .handle = mount_handle,
        .path = "/mnt/kobox/file",
        .buffer = read_buffer,
        .byte_count = sizeof(read_buffer),
        .out_bytes = &read_bytes,
    };
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK ||
        read_bytes != 5 ||
        memcmp(read_buffer, "hello", 5) != 0)
    {
        kb_platform_destroy(platform);
        return 6;
    }

    read_bytes = 99;
    read_desc.path = "/mnt/kobox/empty";
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK || read_bytes != 0) {
        kb_platform_destroy(platform);
        return 16;
    }

    size_t written_bytes = 0;
    kb_fs_write_desc_t write_desc = {
        .handle = mount_handle,
        .path = "/mnt/kobox/file",
        .buffer = "world",
        .byte_count = 5,
        .out_bytes = &written_bytes,
    };
    if (kb_fs_subsystem_write(bound, &write_desc) != KB_OK || written_bytes != 5) {
        kb_platform_destroy(platform);
        return 7;
    }

    char dir_buffer[8] = {0};
    size_t dir_bytes = 0;
    kb_fs_readdir_desc_t readdir_desc = {
        .handle = mount_handle,
        .path = "/mnt/kobox",
        .buffer = dir_buffer,
        .byte_count = sizeof(dir_buffer),
        .out_bytes = &dir_bytes,
    };
    if (kb_fs_subsystem_readdir(bound, &readdir_desc) != KB_OK ||
        dir_bytes != 4 ||
        memcmp(dir_buffer, "a\nb\n", 4) != 0)
    {
        kb_platform_destroy(platform);
        return 8;
    }

    if (smoke.mount_seen != 1 || smoke.read_seen != 2 || smoke.write_seen != 1 || smoke.readdir_seen != 1) {
        kb_platform_destroy(platform);
        return 9;
    }

    kb_interface_unbind(bound);
    kb_platform_destroy(platform);
    int local_result = run_local_fs_model_smoke();
    if (local_result != 0) {
        return local_result;
    }
    return 0;
}
