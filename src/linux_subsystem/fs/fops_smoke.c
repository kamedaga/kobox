#include "kobox/module.h"
#include "loader/module_context.h"
#include "linux_subsystem/fs/kernel_object_registry.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
typedef int (*kb_fops_open_fn)(void *inode, void *file);
typedef int (*kb_fops_release_fn)(void *inode, void *file);
typedef long (*kb_fops_ioctl_fn)(void *file, unsigned int cmd, unsigned long arg);
typedef int (*kb_fops_mmap_fn)(void *file, void *vma);
typedef long (*kb_proc_read_fn)(void *file, char *buffer, size_t size, int64_t *pos);

#define KB_NV_ESC_CARD_INFO_CMD 0xc04846c8u
#define KB_NV_ESC_CHECK_VERSION_STR_CMD 0xc04846d2u
#define KB_NV_ESC_SYS_PARAMS_CMD 0xc00846d6u
#define KB_NV_RM_API_VERSION_CMD_QUERY 0x32u
#define KB_UVM_INITIALIZE_CMD 0x30000001u
#define KB_UVM_DEINITIALIZE_CMD 0x30000002u
#define KB_UVM_PAGEABLE_MEM_ACCESS_CMD 39u
#define KB_UVM_PAGEABLE_MEM_ACCESS_ON_GPU_CMD 70u

typedef struct kb_ioctl_smoke_case {
    const char *name;
    unsigned int cmd;
    void *arg;
    size_t arg_size;
    size_t status_offset;
} kb_ioctl_smoke_case_t;

static void kb_store_ptr_field(void *base, size_t offset, const void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void kb_store_u32_field(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static uint32_t kb_load_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void kb_prepare_fake_file(
    uint8_t *inode,
    size_t inode_size,
    uint8_t *mapping,
    size_t mapping_size,
    uint8_t *file,
    size_t file_size,
    const void *fops,
    uint64_t dev,
    void *cdev,
    void *private_data)
{
    memset(inode, 0, inode_size);
    memset(mapping, 0, mapping_size);
    memset(file, 0, file_size);

    kb_store_ptr_field(inode, 0x40, mapping);
    kb_store_u32_field(inode, 0x5c, (uint32_t)dev);
    kb_store_ptr_field(inode, 0x1b8, cdev);
    kb_store_ptr_field(inode, 0x1c0, cdev);
    kb_store_ptr_field(inode, 0x1c8, cdev);

    kb_store_ptr_field(file, 0x20, inode);
    kb_store_ptr_field(file, 0x28, fops);
    kb_store_ptr_field(file, 0xa8, inode);
    kb_store_ptr_field(file, 0xc8, private_data);
    kb_store_ptr_field(file, 0xd0, private_data);
    kb_store_ptr_field(file, 0xd8, private_data);
}

static void kb_run_ioctl_smoke_case(const char *target, const kb_ioctl_smoke_case_t *test, kb_fops_ioctl_fn ioctl_fn, void *file)
{
    const unsigned long arg = test->arg == NULL ? 0ul : (unsigned long)(uintptr_t)test->arg;
    long result = ioctl_fn(file, test->cmd, arg);
    fprintf(stderr,
            "kobox-fops-smoke: target=%s op=ioctl name=%s cmd=0x%x arg_size=%zu result=%ld\n",
            target,
            test->name,
            test->cmd,
            test->arg_size,
            result);
    if (test->status_offset != SIZE_MAX && test->status_offset + sizeof(uint32_t) <= test->arg_size) {
        fprintf(stderr,
                "kobox-fops-smoke: target=%s op=ioctl name=%s rmStatus=0x%x\n",
                target,
                test->name,
                kb_load_u32_field(test->arg, test->status_offset));
    }
}

static void kb_run_mmap_smoke_case(const char *target, const kb_file_ops_view_t *ops, void *file)
{
    if (ops->mmap == NULL) {
        return;
    }
    uint8_t vma[KB_FAKE_VMA_SIZE];
    const uint64_t start = 0x100000000ull;
    void *mm = kb_loader_module_current_mm(kb_loader_active_module());
    kb_linux_kernel_prepare_fake_vma(vma, mm, start, start + 0x10000u, start >> 12);
    kb_store_ptr_field(vma, 0x88, file);
    kb_store_ptr_field(vma, 0x98, file);
    kb_store_ptr_field(vma, 0xa0, file);
    int result = ((kb_fops_mmap_fn)ops->mmap)(file, vma);
    fprintf(stderr, "kobox-fops-smoke: target=%s op=mmap start=0x%llx result=%d\n", target, (unsigned long long)start, result);
}

static void kb_run_ioctl_smoke_cases(const char *target, const kb_file_ops_view_t *ops, void *file)
{
    if (ops->unlocked_ioctl == NULL) {
        return;
    }

    kb_fops_ioctl_fn ioctl_fn = (kb_fops_ioctl_fn)ops->unlocked_ioctl;
    if (strcmp(target, "chrdev:nvidia-frontend") == 0) {
        uint8_t card_info[72];
        uint8_t version[72];
        uint8_t sys_params[8];
        memset(card_info, 0, sizeof(card_info));
        memset(version, 0, sizeof(version));
        memset(sys_params, 0, sizeof(sys_params));
        kb_store_u32_field(version, 0, KB_NV_RM_API_VERSION_CMD_QUERY);

        kb_ioctl_smoke_case_t tests[] = {
            {"NV_ESC_CARD_INFO", KB_NV_ESC_CARD_INFO_CMD, card_info, sizeof(card_info), SIZE_MAX},
            {"NV_ESC_CHECK_VERSION_STR", KB_NV_ESC_CHECK_VERSION_STR_CMD, version, sizeof(version), 68},
            {"NV_ESC_SYS_PARAMS", KB_NV_ESC_SYS_PARAMS_CMD, sys_params, sizeof(sys_params), SIZE_MAX},
        };
        for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
            kb_run_ioctl_smoke_case(target, &tests[i], ioctl_fn, file);
        }
        return;
    }

    if (strcmp(target, "cdev:nvidia-uvm:0") == 0) {
        uint8_t initialize[16];
        uint8_t pageable_mem_access[8];
        uint8_t pageable_mem_access_on_gpu[24];
        memset(initialize, 0, sizeof(initialize));
        memset(pageable_mem_access, 0, sizeof(pageable_mem_access));
        memset(pageable_mem_access_on_gpu, 0, sizeof(pageable_mem_access_on_gpu));

        kb_ioctl_smoke_case_t tests[] = {
            {"UVM_INITIALIZE", KB_UVM_INITIALIZE_CMD, initialize, sizeof(initialize), 8},
            {"UVM_PAGEABLE_MEM_ACCESS", KB_UVM_PAGEABLE_MEM_ACCESS_CMD, pageable_mem_access, sizeof(pageable_mem_access), 4},
            {"UVM_PAGEABLE_MEM_ACCESS_ON_GPU", KB_UVM_PAGEABLE_MEM_ACCESS_ON_GPU_CMD, pageable_mem_access_on_gpu, sizeof(pageable_mem_access_on_gpu), 20},
        };
        for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
            kb_run_ioctl_smoke_case(target, &tests[i], ioctl_fn, file);
        }
        kb_run_mmap_smoke_case(target, ops, file);
        kb_ioctl_smoke_case_t deinitialize = {"UVM_DEINITIALIZE", KB_UVM_DEINITIALIZE_CMD, NULL, 0, SIZE_MAX};
        kb_run_ioctl_smoke_case(target, &deinitialize, ioctl_fn, file);
        return;
    }

    kb_ioctl_smoke_case_t fallback = {"UNKNOWN", 0u, NULL, 0, SIZE_MAX};
    kb_run_ioctl_smoke_case(target, &fallback, ioctl_fn, file);
}

static int kb_enter_owner_context(kb_module_t *owner, unsigned long *old_gs)
{
    if (owner == NULL) {
        return 0;
    }
    if (kb_loader_enter_module_context(owner, old_gs) != KB_OK) {
        return -1;
    }
    kb_loader_set_active_module(owner);
    return 0;
}

static void kb_leave_owner_context(kb_module_t *owner, unsigned long old_gs)
{
    if (owner == NULL) {
        return;
    }
    kb_loader_leave_module_context(old_gs);
    kb_loader_set_active_module(NULL);
}

#if !defined(_WIN32)
static void kb_child_run_proc_ops(const kb_proc_record_t *record)
{
    uint8_t inode[512];
    uint8_t mapping[512];
    uint8_t file[512];
    int64_t pos = 0;
    char buffer[512];
    unsigned long old_gs = 0;
    int exit_code = 0;

    kb_prepare_fake_file(inode, sizeof(inode), mapping, sizeof(mapping), file, sizeof(file), record->ops, 0, NULL, record->data);
    if (kb_enter_owner_context(record->owner_module, &old_gs) != 0) {
        _exit(125);
    }
    if (record->ops_view.open != NULL) {
        int result = ((kb_fops_open_fn)record->ops_view.open)(inode, file);
        fprintf(stderr, "kobox-fops-smoke: target=proc:%s op=open result=%d\n", record->path, result);
    }
    if (record->ops_view.read != NULL) {
        long result = ((kb_proc_read_fn)record->ops_view.read)(file, buffer, sizeof(buffer), &pos);
        fprintf(stderr, "kobox-fops-smoke: target=proc:%s op=read result=%ld pos=%lld\n", record->path, result, (long long)pos);
    }
    if (record->ops_view.release != NULL) {
        int result = ((kb_fops_release_fn)record->ops_view.release)(inode, file);
        fprintf(stderr, "kobox-fops-smoke: target=proc:%s op=release result=%d\n", record->path, result);
    }
    kb_leave_owner_context(record->owner_module, old_gs);
    _exit(exit_code);
}

static void kb_child_run_file_ops(const char *target, const kb_file_ops_view_t *ops, kb_module_t *owner, const void *fops, uint64_t dev, void *cdev)
{
    uint8_t inode[512];
    uint8_t mapping[512];
    uint8_t file[512];
    unsigned long old_gs = 0;

    kb_prepare_fake_file(inode, sizeof(inode), mapping, sizeof(mapping), file, sizeof(file), fops, dev, cdev, NULL);
    if (kb_enter_owner_context(owner, &old_gs) != 0) {
        _exit(125);
    }
    if (ops->open != NULL) {
        int result = ((kb_fops_open_fn)ops->open)(inode, file);
        fprintf(stderr, "kobox-fops-smoke: target=%s op=open result=%d\n", target, result);
    }
    kb_run_ioctl_smoke_cases(target, ops, file);
    if (ops->release != NULL) {
        int result = ((kb_fops_release_fn)ops->release)(inode, file);
        fprintf(stderr, "kobox-fops-smoke: target=%s op=release result=%d\n", target, result);
    }
    kb_leave_owner_context(owner, old_gs);
    _exit(0);
}

static int kb_wait_for_fops_child(const char *target, pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "kobox-fops-smoke: target=%s wait=failed\n", target);
        return -1;
    }
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        fprintf(stderr, "kobox-fops-smoke: target=%s child_exit=%d\n", target, code);
        return code == 0 ? 0 : -1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "kobox-fops-smoke: target=%s child_signal=%d\n", target, WTERMSIG(status));
        return -1;
    }
    fprintf(stderr, "kobox-fops-smoke: target=%s child_status=0x%x\n", target, status);
    return -1;
}

static int kb_run_proc_ops_child(const kb_proc_record_t *record)
{
    const char *target = record->path == NULL ? "proc:(null)" : record->path;
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "kobox-fops-smoke: target=proc:%s fork=failed\n", target);
        return -1;
    }
    if (pid == 0) {
        kb_child_run_proc_ops(record);
    }
    char label[512];
    snprintf(label, sizeof(label), "proc:%s", target);
    return kb_wait_for_fops_child(label, pid);
}

static int kb_run_file_ops_child(
    const char *target,
    const kb_file_ops_view_t *ops,
    kb_module_t *owner,
    const void *fops,
    uint64_t dev,
    void *cdev)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "kobox-fops-smoke: target=%s fork=failed\n", target);
        return -1;
    }
    if (pid == 0) {
        kb_child_run_file_ops(target, ops, owner, fops, dev, cdev);
    }
    return kb_wait_for_fops_child(target, pid);
}

static const kb_proc_record_t *kb_find_proc_record(const char *path)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_proc_records[i].path != NULL && kb_proc_records[i].active && strcmp(kb_proc_records[i].path, path) == 0) {
            return &kb_proc_records[i];
        }
    }
    return NULL;
}

static const kb_chrdev_record_t *kb_find_chrdev_record(const char *name)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_chrdev_records[i].name != NULL && kb_chrdev_records[i].active && strcmp(kb_chrdev_records[i].name, name) == 0) {
            return &kb_chrdev_records[i];
        }
    }
    return NULL;
}

static const kb_cdev_record_t *kb_find_cdev_record(const char *name, unsigned minor)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_cdev_records[i].cdev == NULL || !kb_cdev_records[i].active) {
            continue;
        }
        if (strcmp(kb_linux_kernel_chrdev_name_for_dev(kb_cdev_records[i].dev), name) == 0 && kb_linux_kernel_decode_minor(kb_cdev_records[i].dev) == minor) {
            return &kb_cdev_records[i];
        }
    }
    return NULL;
}

int kb_module_run_registered_ops_smoke(void)
{
    int failures = 0;

    const kb_proc_record_t *version = kb_find_proc_record("/proc/driver/nvidia/version");
    if (version != NULL && version->has_ops_view) {
        failures += kb_run_proc_ops_child(version) != 0;
    } else {
        fprintf(stderr, "kobox-fops-smoke: target=proc:/proc/driver/nvidia/version missing\n");
        failures++;
    }

    const kb_chrdev_record_t *frontend = kb_find_chrdev_record("nvidia-frontend");
    if (frontend != NULL && frontend->has_fops_view) {
        failures += kb_run_file_ops_child("chrdev:nvidia-frontend", &frontend->fops_view, frontend->owner_module, frontend->fops, kb_linux_kernel_encode_dev(frontend->major, 255), NULL) != 0;
    } else {
        fprintf(stderr, "kobox-fops-smoke: target=chrdev:nvidia-frontend missing\n");
        failures++;
    }

    const kb_cdev_record_t *uvm = kb_find_cdev_record("nvidia-uvm", 0);
    if (uvm != NULL && uvm->has_fops_view) {
        failures += kb_run_file_ops_child("cdev:nvidia-uvm:0", &uvm->fops_view, uvm->owner_module, uvm->fops, uvm->dev, uvm->cdev) != 0;
    } else {
        fprintf(stderr, "kobox-fops-smoke: target=cdev:nvidia-uvm:0 missing\n");
        failures++;
    }

    return failures == 0 ? 0 : -1;
}
#else
int kb_module_run_registered_ops_smoke(void)
{
    fprintf(stderr, "kobox-fops-smoke: unsupported on Windows host\n");
    return -1;
}
#endif
