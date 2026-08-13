#include "kobox/shim.h"
#include "loader/module_context.h"
#include "linux_subsystem/fs/kernel_object_registry.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__pachaos__)
#include <time.h>
#endif

enum {
    KB_STUB_PAGE_SIZE = 4096,
    KB_STUB_STRUCT_PAGE_SIZE = 64,
    KB_STUB_TASK_FLAGS_OFFSET = 0x0,
    KB_STUB_TASK_PID_LINKS_OFFSET = 0x638,
    KB_STUB_TASK_PID_LINK_STRIDE = 0x10,
    KB_STUB_PID_TYPE_COUNT = 4,
    KB_STUB_TASK_COMM_OFFSET = 0xbd8,
    KB_STUB_TASK_COMM_LEN = 16,
    KB_STUB_TIF_SIGPENDING_BIT = 2,
};

typedef struct kb_ida_record {
    void *ida;
    unsigned int *ids;
    size_t count;
    size_t capacity;
    struct kb_ida_record *next;
} kb_ida_record_t;

typedef struct kb_kthread_record {
    void *task;
    int (*threadfn)(void *data);
    void *data;
    kb_module_t *owner;
    void *stack;
    size_t stack_size;
    jmp_buf context;
    uintptr_t saved_context_rsp;
    uintptr_t saved_context_rip;
    uint64_t context_generation;
    int node;
    int activated;
    int wait_prepared;
    int wake_pending;
    int started;
    int finished;
    int stop_requested;
    int result;
    char name[64];
    struct kb_kthread_record *next;
} kb_kthread_record_t;

typedef struct kb_kthread_dispatch_frame {
    kb_kthread_record_t *record;
    jmp_buf context;
    struct kb_kthread_dispatch_frame *previous;
} kb_kthread_dispatch_frame_t;

typedef struct kb_pending_pgrp_signal {
    void *pgrp;
    int sig;
    int priv;
    uint64_t sequence;
} kb_pending_pgrp_signal_t;

typedef struct kb_pending_task_signal {
    void *task;
    void *info;
    int sig;
    int type;
    uint64_t sequence;
} kb_pending_task_signal_t;

typedef struct kb_stub_pid {
    int count;
    unsigned int level;
    uint32_t lock;
    uint32_t reserved0;
    void *tasks[KB_STUB_PID_TYPE_COUNT];
    int nr;
    uint32_t reserved1;
    void *task;
    uint64_t last_used;
} kb_stub_pid_t;

typedef struct kb_sysctl_registration {
    char path[96];
    const char *table_name;
    void *table;
    size_t table_size;
    uint64_t sequence;
    struct kb_sysctl_registration *next;
} kb_sysctl_registration_t;

static kb_ida_record_t *ida_records;
static kb_kthread_record_t *kthread_records;
static kb_kthread_record_t *active_kthread;
static kb_kthread_dispatch_frame_t *kthread_dispatch_frame;
static kb_sysctl_registration_t *sysctl_registrations;
static kb_pending_pgrp_signal_t pending_pgrp_signals[64];
static kb_pending_task_signal_t pending_task_signals[64];
static kb_stub_pid_t stub_pids[64];
static uint64_t sysctl_registration_sequence;
static uint64_t pending_pgrp_signal_sequence;
static uint64_t pending_task_signal_sequence;
static uint64_t stub_pid_use_sequence;

enum {
    KB_DRM_DEVICE_BYTES = 0x610,
    KB_DRM_FILE_BYTES = 0x170,
    KB_DRM_DEVICE_DEV_OFFSET = 0x08,
    KB_DRM_DEVICE_DRIVER_OFFSET = 0x30,
    KB_DRM_DEVICE_PRIVATE_OFFSET = 0x38,
    KB_DRM_DEVICE_FEATURES_OFFSET = 0x68,
    KB_DRM_DRIVER_OPEN_OFFSET = 0x08,
    KB_DRM_DRIVER_POSTCLOSE_OFFSET = 0x10,
    KB_DRM_DRIVER_DUMB_CREATE_OFFSET = 0x70,
    KB_DRM_DRIVER_NAME_OFFSET = 0x98,
    KB_DRM_DRIVER_DESC_OFFSET = 0xa0,
    KB_DRM_DRIVER_DATE_OFFSET = 0xa8,
    KB_DRM_DRIVER_FEATURES_OFFSET = 0xb0,
    KB_DRM_DRIVER_MAJOR_OFFSET = 0x88,
    KB_DRM_DRIVER_MINOR_OFFSET = 0x8c,
    KB_DRM_DRIVER_PATCHLEVEL_OFFSET = 0x90,
    KB_DRM_DRIVER_IOCTLS_OFFSET = 0xb8,
    KB_DRM_DRIVER_NUM_IOCTLS_OFFSET = 0xc0,
    KB_DRM_DRIVER_FOPS_OFFSET = 0xc8,
    KB_DRM_LINUX_FILE_PRIVATE_DATA_OFFSET = 0xc8,
    KB_DRM_IOCTL_DESC_BYTES = 24,
    KB_DRM_IOCTL_DESC_CMD_OFFSET = 0,
    KB_DRM_IOCTL_DESC_FLAGS_OFFSET = 4,
    KB_DRM_IOCTL_DESC_FUNC_OFFSET = 8,
    KB_DRM_COMMAND_BASE = 0x40,
    KB_DRM_RENDER_ALLOW = 1u << 5,
    KB_DRM_MAJOR = 226,
    KB_DRM_CARD0_MINOR = 0,
    KB_DRM_RENDERD128_MINOR = 128,
    KB_DRM_MINOR_COUNT = 256,
    KB_DRM_DRIVER_RENDER = 1u << 3,
    KB_DRM_DEVICE_MAX = 4,
    KB_DRM_FILE_MAX = 64,
};

typedef struct kb_drm_device_record {
    void *device;
    void *driver;
    void *cdev;
    void *render_cdev;
    int registered;
    int unplugged;
} kb_drm_device_record_t;

typedef struct kb_drm_file_record {
    void *linux_file;
    void *drm_file;
    kb_drm_device_record_t *device;
    int render_node;
} kb_drm_file_record_t;

typedef struct kb_drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
} kb_drm_version_t;

typedef struct kb_drm_get_cap {
    uint64_t capability;
    uint64_t value;
} kb_drm_get_cap_t;

static kb_drm_device_record_t drm_devices[KB_DRM_DEVICE_MAX];
static kb_drm_file_record_t drm_files[KB_DRM_FILE_MAX];

static size_t pending_pgrp_signal_count(void)
{
    return sizeof(pending_pgrp_signals) / sizeof(pending_pgrp_signals[0]);
}

static size_t pending_task_signal_count(void)
{
    return sizeof(pending_task_signals) / sizeof(pending_task_signals[0]);
}

static int crypto_trace_enabled(void);

static void kb_mark_task_signal_pending(void *task)
{
    if (task == NULL) {
        task = kb_loader_module_current_task(kb_loader_active_module());
    }
    if (task == NULL) {
        return;
    }
    unsigned long flags = 0;
    memcpy(&flags, (const uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, sizeof(flags));
    flags |= 1ul << KB_STUB_TIF_SIGPENDING_BIT;
    memcpy((uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, &flags, sizeof(flags));
}

static void kb_clear_task_signal_pending(void *task)
{
    if (task == NULL) {
        return;
    }
    unsigned long flags = 0;
    memcpy(&flags, (const uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, sizeof(flags));
    flags &= ~(1ul << KB_STUB_TIF_SIGPENDING_BIT);
    memcpy((uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, &flags, sizeof(flags));
}

void kb_clear_current_signal_pending(void)
{
    kb_clear_task_signal_pending(
        kb_loader_module_current_task(kb_loader_active_module()));
}

static int trace_dma_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void *page_payload_from_struct_page(void *page)
{
    if (page == NULL) {
        return NULL;
    }
    uintptr_t vmemmap = kb_linux_kvm_vmemmap_base();
    uintptr_t page_offset = kb_linux_kvm_page_offset_base();
    uintptr_t page_addr = (uintptr_t)page;
    if (vmemmap == 0 || page_offset == 0 || page_addr < vmemmap) {
        return NULL;
    }
    uintptr_t index = (page_addr - vmemmap) / KB_STUB_STRUCT_PAGE_SIZE;
    return (void *)(page_offset + index * KB_STUB_PAGE_SIZE);
}

static uint64_t page_phys_from_struct_page(void *page)
{
    uintptr_t vmemmap = kb_linux_kvm_vmemmap_base();
    uintptr_t page_addr = (uintptr_t)page;
    if (vmemmap == 0 || page_addr < vmemmap) {
        return 0;
    }
    uint64_t index = (uint64_t)((page_addr - vmemmap) / KB_STUB_STRUCT_PAGE_SIZE);
    return (uint64_t)kb_linux_kvm_phys_base() + (index * KB_STUB_PAGE_SIZE);
}

enum {
    KB_PERCPU_COUNTER_STRIDE = 0x28,
    KB_PERCPU_COUNTER_COUNT_OFFSET = 0x8,
    KB_PERCPU_COUNTER_POINTER_OFFSET = 0x20,
    KB_KFIFO_IN_OFFSET = 0,
    KB_KFIFO_OUT_OFFSET = 4,
    KB_KFIFO_MASK_OFFSET = 8,
    KB_KFIFO_ESIZE_OFFSET = 12,
    KB_KFIFO_DATA_OFFSET = 16,
};

void kb_noop_stub(void)
{
}

/*
 * A Linux static-call nop is an instruction-level no-op: callers may keep a
 * value in any volatile register across it.  A normal C function is not an
 * equivalent replacement because its generated body may clobber those
 * registers (notably %rax in jbd2_journal_grab_journal_head()).
 */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((naked)) void kb_static_call_preserve_noop(void)
{
    __asm__ volatile("ret");
}
#else
void kb_static_call_preserve_noop(void)
{
}
#endif

const int kb_sysctl_vals[] = {0, 1, 2, 3, 4, 100, 200, 1000, 3000, INT32_MAX, 65535, -1};

static int sysctl_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_SYSCTL");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void *register_sysctl_common(const char *path, void *table, const char *table_name, size_t table_size)
{
    kb_sysctl_registration_t *registration = calloc(1, sizeof(*registration));
    if (registration == NULL) {
        return NULL;
    }
    if (path != NULL) {
        snprintf(registration->path, sizeof(registration->path), "%s", path);
    }
    registration->table_name = table_name;
    registration->table = table;
    registration->table_size = table_size;
    registration->sequence = ++sysctl_registration_sequence;
    registration->next = sysctl_registrations;
    sysctl_registrations = registration;
    if (sysctl_trace_enabled()) {
        fprintf(stderr,
            "kobox-sysctl: register path=%s table=%p name=%s size=%zu seq=%llu\n",
            registration->path,
            table,
            table_name != NULL ? table_name : "",
            table_size,
            (unsigned long long)registration->sequence);
    }
    return registration;
}

void *kb_register_sysctl_init(const char *path, void *table, const char *table_name, size_t table_size)
{
    return register_sysctl_common(path, table, table_name, table_size);
}

void *kb_register_sysctl_sz(const char *path, void *table, size_t table_size)
{
    return register_sysctl_common(path, table, NULL, table_size);
}

void kb_unregister_sysctl_table(void *header)
{
    kb_sysctl_registration_t **cursor = &sysctl_registrations;
    while (*cursor != NULL) {
        if (*cursor == header) {
            kb_sysctl_registration_t *dead = *cursor;
            *cursor = dead->next;
            if (sysctl_trace_enabled()) {
                fprintf(stderr,
                    "kobox-sysctl: unregister path=%s seq=%llu\n",
                    dead->path,
                    (unsigned long long)dead->sequence);
            }
            free(dead);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

int kb_proc_dointvec(void *table, int write, void *buffer, size_t *lenp, long long *ppos)
{
    (void)table;
    (void)write;
    (void)buffer;
    (void)lenp;
    (void)ppos;
    return 0;
}

int kb_proc_dointvec_minmax(void *table, int write, void *buffer, size_t *lenp, long long *ppos)
{
    return kb_proc_dointvec(table, write, buffer, lenp, ppos);
}

int kb_proc_dobool(void *table, int write, void *buffer, size_t *lenp, long long *ppos)
{
    return kb_proc_dointvec(table, write, buffer, lenp, ppos);
}

#if defined(__x86_64__) && !defined(_MSC_VER)
__attribute__((naked)) void kb_stackleak_track_stack_stub(void)
{
    __asm__("ret");
}
#else
void kb_stackleak_track_stack_stub(void)
{
}
#endif

int kb_return_zero(void)
{
    return 0;
}

int kb_return_one(void)
{
    return 1;
}

static void *kb_read_ptr(const void *base, size_t offset)
{
    void *value = NULL;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
}

static uint64_t kb_read_u64(const void *base, size_t offset)
{
    uint64_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
}

static int kb_read_int(const void *base, size_t offset)
{
    int value = 0;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
}

static uint32_t kb_read_u32(const void *base, size_t offset)
{
    uint32_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    }
    return value;
}

static void kb_write_ptr(void *base, size_t offset, const void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void kb_write_u64(void *base, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static int kb_drm_call_driver_open(void *function, void *device, void *file)
{
    if (function == NULL) {
        return 0;
    }
    kb_module_t *owner = kb_module_find_owner_for_address(function);
    kb_module_t *previous = kb_loader_active_module();
    unsigned long old_gs = 0;
    const int entered = owner != NULL && kb_loader_enter_module_context(owner, &old_gs) == KB_OK;
    if (entered) {
        kb_loader_set_active_module(owner);
    }
    const int result = ((int (*)(void *, void *))function)(device, file);
    if (entered) {
        kb_loader_leave_module_context(old_gs);
        kb_loader_set_active_module(previous);
    }
    return result;
}

static void kb_drm_call_driver_postclose(void *function, void *device, void *file)
{
    if (function == NULL) {
        return;
    }
    kb_module_t *owner = kb_module_find_owner_for_address(function);
    kb_module_t *previous = kb_loader_active_module();
    unsigned long old_gs = 0;
    const int entered = owner != NULL && kb_loader_enter_module_context(owner, &old_gs) == KB_OK;
    if (entered) {
        kb_loader_set_active_module(owner);
    }
    ((void (*)(void *, void *))function)(device, file);
    if (entered) {
        kb_loader_leave_module_context(old_gs);
        kb_loader_set_active_module(previous);
    }
}

static int kb_drm_call_driver_ioctl(
    void *function,
    void *device,
    void *data,
    void *file)
{
    if (function == NULL) {
        return -25;
    }
    kb_module_t *owner = kb_module_find_owner_for_address(function);
    kb_module_t *previous = kb_loader_active_module();
    unsigned long old_gs = 0;
    const int entered = owner != NULL &&
        kb_loader_enter_module_context(owner, &old_gs) == KB_OK;
    if (entered) {
        kb_loader_set_active_module(owner);
    }
    const int result = ((int (*)(void *, void *, void *))function)(
        device, data, file);
    if (entered) {
        kb_loader_leave_module_context(old_gs);
        kb_loader_set_active_module(previous);
    }
    return result;
}

static kb_drm_device_record_t *kb_drm_find_device(void *device)
{
    for (size_t i = 0; i < KB_DRM_DEVICE_MAX; i++) {
        if (drm_devices[i].device == device) {
            return &drm_devices[i];
        }
    }
    return NULL;
}

void *kb_drm_primary_device(void)
{
    for (size_t i = 0; i < KB_DRM_DEVICE_MAX; i++) {
        if (drm_devices[i].device != NULL && drm_devices[i].registered) {
            return drm_devices[i].device;
        }
    }
    return NULL;
}

void *kb_drm_device_private(void *device)
{
    kb_drm_device_record_t *record = kb_drm_find_device(device);
    return record == NULL ? NULL : kb_read_ptr(record->device, KB_DRM_DEVICE_PRIVATE_OFFSET);
}

static kb_drm_file_record_t *kb_drm_find_file(void *linux_file)
{
    for (size_t i = 0; i < KB_DRM_FILE_MAX; i++) {
        if (drm_files[i].linux_file == linux_file) {
            return &drm_files[i];
        }
    }
    return NULL;
}

void *kb_drm_dev_alloc(void *driver, void *parent)
{
    if (driver == NULL) {
        return (void *)(intptr_t)-22;
    }
    kb_drm_device_record_t *record = NULL;
    for (size_t i = 0; i < KB_DRM_DEVICE_MAX; i++) {
        if (drm_devices[i].device == NULL) {
            record = &drm_devices[i];
            break;
        }
    }
    if (record == NULL) {
        return (void *)(intptr_t)-12;
    }
    void *device = kb_kzalloc(KB_DRM_DEVICE_BYTES, 0);
    if (device == NULL) {
        return (void *)(intptr_t)-12;
    }
    kb_write_ptr(device, KB_DRM_DEVICE_DEV_OFFSET, parent);
    kb_write_ptr(device, KB_DRM_DEVICE_DRIVER_OFFSET, driver);
    kb_write_u64(device, KB_DRM_DEVICE_FEATURES_OFFSET,
        kb_read_u64(driver, KB_DRM_DRIVER_FEATURES_OFFSET));
    record->device = device;
    record->driver = driver;
    return device;
}

int kb_drm_dev_register(void *device, unsigned long flags)
{
    (void)flags;
    kb_drm_device_record_t *record = kb_drm_find_device(device);
    if (record == NULL || record->registered) {
        return record != NULL ? 0 : -22;
    }
    void *fops = kb_read_ptr(record->driver, KB_DRM_DRIVER_FOPS_OFFSET);
    if (fops == NULL) {
        return -22;
    }
    int status = kb_linux_kernel_register_chrdev(
        KB_DRM_MAJOR, KB_DRM_CARD0_MINOR, KB_DRM_MINOR_COUNT, "drm", fops);
    if (status != 0) {
        return status;
    }
    record->cdev = kb_linux_kernel_cdev_alloc();
    if (record->cdev == NULL) {
        kb_linux_kernel_unregister_chrdev(
            KB_DRM_MAJOR, KB_DRM_CARD0_MINOR, KB_DRM_MINOR_COUNT, "drm");
        return -12;
    }
    kb_linux_kernel_cdev_init(record->cdev, fops);
    status = kb_linux_kernel_cdev_add(
        record->cdev,
        kb_linux_kernel_encode_dev(KB_DRM_MAJOR, KB_DRM_CARD0_MINOR),
        1);
    if (status != 0) {
        kb_linux_kernel_unregister_chrdev(
            KB_DRM_MAJOR, KB_DRM_CARD0_MINOR, KB_DRM_MINOR_COUNT, "drm");
        return status;
    }
    if ((kb_read_u64(record->driver, KB_DRM_DRIVER_FEATURES_OFFSET) &
            KB_DRM_DRIVER_RENDER) != 0) {
        record->render_cdev = kb_linux_kernel_cdev_alloc();
        if (record->render_cdev == NULL) {
            kb_linux_kernel_cdev_del(record->cdev);
            kb_linux_kernel_unregister_chrdev(
                KB_DRM_MAJOR, KB_DRM_CARD0_MINOR, KB_DRM_MINOR_COUNT, "drm");
            return -12;
        }
        kb_linux_kernel_cdev_init(record->render_cdev, fops);
        status = kb_linux_kernel_cdev_add(
            record->render_cdev,
            kb_linux_kernel_encode_dev(KB_DRM_MAJOR, KB_DRM_RENDERD128_MINOR),
            1);
        if (status != 0) {
            kb_linux_kernel_cdev_del(record->cdev);
            kb_linux_kernel_unregister_chrdev(
                KB_DRM_MAJOR, KB_DRM_CARD0_MINOR, KB_DRM_MINOR_COUNT, "drm");
            return status;
        }
    }
    record->registered = 1;
    return 0;
}

void *kb_drm_dev_get(void *device)
{
    return kb_drm_find_device(device) != NULL ? device : NULL;
}

void kb_drm_dev_put(void *device)
{
    (void)device;
}

int kb_drm_dev_enter(void *device, int *index)
{
    kb_drm_device_record_t *record = kb_drm_find_device(device);
    /*
     * drm_dev_enter() guards unplug, not character-device publication.
     * Drivers issue initialization commands before drm_dev_register(), so
     * tying this guard to node registration drops their first requests.
     */
    if (record == NULL || record->unplugged) {
        return 0;
    }
    if (index != NULL) {
        *index = 0;
    }
    return 1;
}

void kb_drm_dev_exit(int index)
{
    (void)index;
}

void kb_drm_dev_unplug(void *device)
{
    kb_drm_device_record_t *record = kb_drm_find_device(device);
    if (record != NULL) {
        record->unplugged = 1;
        record->registered = 0;
    }
}

int kb_drm_open(void *inode, void *linux_file)
{
    if (linux_file == NULL || kb_drm_find_file(linux_file) != NULL) {
        return -22;
    }
    kb_drm_device_record_t *device = NULL;
    void *inode_cdev = kb_read_ptr(inode, 0x238);
    for (size_t i = 0; i < KB_DRM_DEVICE_MAX; i++) {
        if (drm_devices[i].registered &&
            (drm_devices[i].cdev == inode_cdev ||
                drm_devices[i].render_cdev == inode_cdev)) {
            device = &drm_devices[i];
            break;
        }
    }
    if (device == NULL) {
        return -19;
    }
    kb_drm_file_record_t *record = NULL;
    for (size_t i = 0; i < KB_DRM_FILE_MAX; i++) {
        if (drm_files[i].linux_file == NULL) {
            record = &drm_files[i];
            break;
        }
    }
    if (record == NULL) {
        return -24;
    }
    void *drm_file = kb_kzalloc(KB_DRM_FILE_BYTES, 0);
    if (drm_file == NULL) {
        return -12;
    }
    const int status = kb_drm_call_driver_open(
        kb_read_ptr(device->driver, KB_DRM_DRIVER_OPEN_OFFSET),
        device->device,
        drm_file);
    if (status != 0) {
        kb_kfree(drm_file);
        return status;
    }
    record->linux_file = linux_file;
    record->drm_file = drm_file;
    record->device = device;
    record->render_node = device->render_cdev == inode_cdev;
    kb_write_ptr(linux_file, KB_DRM_LINUX_FILE_PRIVATE_DATA_OFFSET, drm_file);
    return 0;
}

int kb_drm_release(void *inode, void *linux_file)
{
    (void)inode;
    kb_drm_file_record_t *record = kb_drm_find_file(linux_file);
    if (record == NULL) {
        return -9;
    }
    kb_drm_call_driver_postclose(
        kb_read_ptr(record->device->driver, KB_DRM_DRIVER_POSTCLOSE_OFFSET),
        record->device->device,
        record->drm_file);
    kb_kfree(record->drm_file);
    kb_write_ptr(linux_file, KB_DRM_LINUX_FILE_PRIVATE_DATA_OFFSET, NULL);
    memset(record, 0, sizeof(*record));
    return 0;
}

static void kb_drm_copy_version_string(size_t *length, char *destination, const char *source)
{
    const size_t capacity = *length;
    const size_t source_length = source != NULL ? strlen(source) : 0;
    if (destination != NULL && capacity != 0 && source_length != 0) {
        const size_t copy_length = capacity < source_length ? capacity : source_length;
        memcpy(destination, source, copy_length);
    }
    *length = source_length;
}

long kb_drm_ioctl(void *linux_file, unsigned int command, unsigned long argument)
{
    static const unsigned int DRM_IOCTL_VERSION = 0xc0406400u;
    static const unsigned int DRM_IOCTL_GET_CAP = 0xc010640cu;
    enum {
        DRM_CAP_DUMB_BUFFER = 1,
    };
    kb_drm_file_record_t *record = kb_drm_find_file(linux_file);
    if (record == NULL || argument == 0) {
        return record == NULL ? -9 : -14;
    }
    void *driver = record->device->driver;
    if (command == DRM_IOCTL_VERSION) {
        kb_drm_version_t *version = (kb_drm_version_t *)(uintptr_t)argument;
        version->version_major = kb_read_int(driver, KB_DRM_DRIVER_MAJOR_OFFSET);
        version->version_minor = kb_read_int(driver, KB_DRM_DRIVER_MINOR_OFFSET);
        version->version_patchlevel = kb_read_int(driver, KB_DRM_DRIVER_PATCHLEVEL_OFFSET);
        kb_drm_copy_version_string(
            &version->name_len, version->name,
            (const char *)kb_read_ptr(driver, KB_DRM_DRIVER_NAME_OFFSET));
        kb_drm_copy_version_string(
            &version->date_len, version->date,
            (const char *)kb_read_ptr(driver, KB_DRM_DRIVER_DATE_OFFSET));
        kb_drm_copy_version_string(
            &version->desc_len, version->desc,
            (const char *)kb_read_ptr(driver, KB_DRM_DRIVER_DESC_OFFSET));
        return 0;
    }
    if (command == DRM_IOCTL_GET_CAP) {
        kb_drm_get_cap_t *cap = (kb_drm_get_cap_t *)(uintptr_t)argument;
        if (cap->capability == DRM_CAP_DUMB_BUFFER) {
            cap->value = kb_read_ptr(driver, KB_DRM_DRIVER_DUMB_CREATE_OFFSET) != NULL;
            return 0;
        }
        return -22;
    }

    const uint32_t command_type = (command >> 8u) & 0xffu;
    const uint32_t command_nr = command & 0xffu;
    const uint32_t ioctl_count = kb_read_u32(
        driver, KB_DRM_DRIVER_NUM_IOCTLS_OFFSET);
    if (command_type != 'd' || command_nr < KB_DRM_COMMAND_BASE ||
        command_nr - KB_DRM_COMMAND_BASE >= ioctl_count) {
        return -25;
    }
    void *table = kb_read_ptr(driver, KB_DRM_DRIVER_IOCTLS_OFFSET);
    if (table == NULL) {
        return -25;
    }
    const void *descriptor = (const uint8_t *)table +
        (size_t)(command_nr - KB_DRM_COMMAND_BASE) * KB_DRM_IOCTL_DESC_BYTES;
    const uint32_t descriptor_command = kb_read_u32(
        descriptor, KB_DRM_IOCTL_DESC_CMD_OFFSET);
    const uint32_t descriptor_flags = kb_read_u32(
        descriptor, KB_DRM_IOCTL_DESC_FLAGS_OFFSET);
    void *function = kb_read_ptr(descriptor, KB_DRM_IOCTL_DESC_FUNC_OFFSET);
    if (descriptor_command != command || function == NULL) {
        return -25;
    }
    if (record->render_node &&
        (descriptor_flags & KB_DRM_RENDER_ALLOW) == 0) {
        return -13;
    }
    return kb_drm_call_driver_ioctl(
        function,
        record->device->device,
        (void *)(uintptr_t)argument,
        record->drm_file);
}

long kb_drm_compat_ioctl(void *linux_file, unsigned int command, unsigned long argument)
{
    return kb_drm_ioctl(linux_file, command, argument);
}

void *kb_drmm_kmalloc(void *device, size_t size, unsigned int flags)
{
    (void)device;
    return kb_kmalloc(size, flags);
}

void kb_drmm_kfree(void *device, void *memory)
{
    (void)device;
    kb_kfree(memory);
}

void *kb_drmm_universal_plane_alloc(void *device, size_t size)
{
    (void)device;
    return kb_kzalloc(size, 0);
}

char *kb_get_task_comm(char *buf, size_t buf_size, void *task)
{
    if (buf == NULL || buf_size == 0) {
        return buf;
    }

    memset(buf, 0, buf_size);
    if (task == NULL) {
        task = kb_loader_module_current_task(kb_loader_active_module());
    }

    const char *comm = "kobox";
    if (task != NULL) {
        comm = (const char *)((const unsigned char *)task + KB_STUB_TASK_COMM_OFFSET);
    }

    size_t limit = buf_size - 1u;
    if (limit > KB_STUB_TASK_COMM_LEN) {
        limit = KB_STUB_TASK_COMM_LEN;
    }
    size_t i = 0;
    while (i < limit && comm[i] != '\0') {
        buf[i] = comm[i];
        i++;
    }
    return buf;
}

int kb_kill_pgrp(void *pgrp, int sig, int priv)
{
    if (sig < 0 || sig > 64) {
        return -22;
    }

    const uint64_t sequence = ++pending_pgrp_signal_sequence;
    pending_pgrp_signals[sequence % pending_pgrp_signal_count()] =
        (kb_pending_pgrp_signal_t){
            .pgrp = pgrp,
            .sig = sig,
            .priv = priv,
            .sequence = sequence,
        };
    kb_stub_pid_t *record = (kb_stub_pid_t *)pgrp;
    kb_mark_task_signal_pending(record != NULL ? record->task : NULL);

    const char *trace = getenv("KOBOX_TRACE_TTY_SIGNALS");
    if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
        fprintf(stderr,
            "kobox tty: kill_pgrp pgrp=%p sig=%d priv=%d seq=%llu\n",
            pgrp,
            sig,
            priv,
            (unsigned long long)sequence);
    }

    return 0;
}

int kb_take_pending_pgrp_signal(uint64_t since, int *out_pgrp, int *out_sig, uint64_t *out_sequence)
{
    if (out_pgrp == NULL || out_sig == NULL || out_sequence == NULL) {
        return -22;
    }
    *out_pgrp = 0;
    *out_sig = 0;
    *out_sequence = pending_pgrp_signal_sequence;

    kb_pending_pgrp_signal_t *selected = NULL;
    for (size_t i = 0; i < pending_pgrp_signal_count(); i++) {
        kb_pending_pgrp_signal_t *candidate = &pending_pgrp_signals[i];
        if (candidate->sequence == 0 || candidate->sequence <= since) {
            continue;
        }
        if (selected == NULL || candidate->sequence < selected->sequence) {
            selected = candidate;
        }
    }
    if (selected == NULL) {
        return 0;
    }

    const kb_stub_pid_t *record = (const kb_stub_pid_t *)selected->pgrp;
    if (record == NULL || record->nr <= 0) {
        *out_sequence = selected->sequence;
        return 0;
    }
    *out_pgrp = record->nr;
    *out_sig = selected->sig;
    *out_sequence = selected->sequence;
    kb_clear_task_signal_pending(record->task);
    memset(selected, 0, sizeof(*selected));
    return 1;
}

static int kb_record_task_signal(const char *source, int sig, void *info, void *task, int type)
{
    if (sig < 0 || sig > 64) {
        return -22;
    }

    const uint64_t sequence = ++pending_task_signal_sequence;
    pending_task_signals[sequence % pending_task_signal_count()] =
        (kb_pending_task_signal_t){
            .task = task,
            .info = info,
            .sig = sig,
            .type = type,
            .sequence = sequence,
        };
    kb_mark_task_signal_pending(task);

    const char *trace = getenv("KOBOX_TRACE_TTY_SIGNALS");
    if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
        fprintf(stderr,
            "kobox tty: %s task=%p sig=%d type=%d info=%p seq=%llu\n",
            source,
            task,
            sig,
            type,
            info,
            (unsigned long long)sequence);
    }

    return 0;
}

int kb_group_send_sig_info(int sig, void *info, void *task, int type)
{
    return kb_record_task_signal("group_send_sig_info", sig, info, task, type);
}

int kb_send_signal_locked(int sig, void *info, void *task, int type)
{
    return kb_record_task_signal("send_signal_locked", sig, info, task, type);
}

int kb_llist_add_batch(void *new_first, void *new_last, void *head)
{
    if (new_first == NULL || new_last == NULL || head == NULL) {
        return 0;
    }
    void *old_first = NULL;
    memcpy(&old_first, head, sizeof(old_first));
    memcpy(new_last, &old_first, sizeof(old_first));
    memcpy(head, &new_first, sizeof(new_first));
    return old_first == NULL;
}

void *kb_llist_del_first(void *head)
{
    if (head == NULL) {
        return NULL;
    }
    void *entry = NULL;
    memcpy(&entry, head, sizeof(entry));
    if (entry == NULL) {
        return NULL;
    }
    void *next = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(head, &next, sizeof(next));
    return entry;
}

static unsigned int kb_pow2_floor_u32(unsigned int value)
{
    if (value == 0) {
        return 0;
    }
    unsigned int power = 1;
    while (power <= value / 2u) {
        power <<= 1u;
    }
    return power;
}

static void kb_kfifo_load(void *fifo, unsigned int *in, unsigned int *out, unsigned int *mask, unsigned int *esize, void **data)
{
    memcpy(in, (const uint8_t *)fifo + KB_KFIFO_IN_OFFSET, sizeof(*in));
    memcpy(out, (const uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, sizeof(*out));
    memcpy(mask, (const uint8_t *)fifo + KB_KFIFO_MASK_OFFSET, sizeof(*mask));
    memcpy(esize, (const uint8_t *)fifo + KB_KFIFO_ESIZE_OFFSET, sizeof(*esize));
    memcpy(data, (const uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, sizeof(*data));
}

int kb_kfifo_init(void *fifo, void *buffer, unsigned int size, size_t esize)
{
    if (fifo == NULL || buffer == NULL || esize == 0 || esize > UINT32_MAX) {
        return -22;
    }
    unsigned int elements = size / (unsigned int)esize;
    elements = kb_pow2_floor_u32(elements);
    unsigned int in = 0;
    unsigned int out = 0;
    unsigned int mask = elements < 2 ? 0 : elements - 1u;
    unsigned int esize32 = (unsigned int)esize;
    memcpy((uint8_t *)fifo + KB_KFIFO_IN_OFFSET, &in, sizeof(in));
    memcpy((uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, &out, sizeof(out));
    memcpy((uint8_t *)fifo + KB_KFIFO_MASK_OFFSET, &mask, sizeof(mask));
    memcpy((uint8_t *)fifo + KB_KFIFO_ESIZE_OFFSET, &esize32, sizeof(esize32));
    memcpy((uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, &buffer, sizeof(buffer));
    return elements < 2 ? -22 : 0;
}

unsigned int kb_kfifo_in(void *fifo, const void *buffer, unsigned int len)
{
    if (fifo == NULL || buffer == NULL) {
        return 0;
    }
    unsigned int in = 0;
    unsigned int out = 0;
    unsigned int mask = 0;
    unsigned int esize = 0;
    void *data = NULL;
    kb_kfifo_load(fifo, &in, &out, &mask, &esize, &data);
    if (data == NULL || esize == 0 || mask == 0) {
        return 0;
    }
    unsigned int size = mask + 1u;
    unsigned int unused = size - (in - out);
    if (len > unused) {
        len = unused;
    }
    unsigned int off = in & mask;
    unsigned int first = size - off;
    if (first > len) {
        first = len;
    }
    memcpy((uint8_t *)data + (off * esize), buffer, (size_t)first * esize);
    memcpy(data, (const uint8_t *)buffer + ((size_t)first * esize), (size_t)(len - first) * esize);
    in += len;
    memcpy((uint8_t *)fifo + KB_KFIFO_IN_OFFSET, &in, sizeof(in));
    return len;
}

unsigned int kb_kfifo_out(void *fifo, void *buffer, unsigned int len)
{
    if (fifo == NULL || buffer == NULL) {
        return 0;
    }
    unsigned int in = 0;
    unsigned int out = 0;
    unsigned int mask = 0;
    unsigned int esize = 0;
    void *data = NULL;
    kb_kfifo_load(fifo, &in, &out, &mask, &esize, &data);
    if (data == NULL || esize == 0 || mask == 0) {
        return 0;
    }
    unsigned int available = in - out;
    if (len > available) {
        len = available;
    }
    unsigned int size = mask + 1u;
    unsigned int off = out & mask;
    unsigned int first = size - off;
    if (first > len) {
        first = len;
    }
    memcpy(buffer, (const uint8_t *)data + (off * esize), (size_t)first * esize);
    memcpy((uint8_t *)buffer + ((size_t)first * esize), data, (size_t)(len - first) * esize);
    out += len;
    memcpy((uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, &out, sizeof(out));
    return len;
}

int kb_kfifo_to_user(void *fifo, void *to, unsigned int len, unsigned int *copied)
{
    unsigned int done = kb_kfifo_out(fifo, to, len);
    if (copied != NULL) {
        *copied = done;
    }
    return done == len ? 0 : -14;
}

void kb_kfifo_free(void *fifo)
{
    if (fifo == NULL) {
        return;
    }
    unsigned int zero = 0;
    void *data = NULL;
    memcpy(&data, (const uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, sizeof(data));
    kb_kfree(data);
    data = NULL;
    memcpy((uint8_t *)fifo + KB_KFIFO_IN_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_MASK_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_ESIZE_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, &data, sizeof(data));
}

void *kb_find_vpid(int nr)
{
    if (nr <= 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(stub_pids) / sizeof(stub_pids[0]); i++) {
        if (stub_pids[i].nr == nr) {
            stub_pids[i].last_used = ++stub_pid_use_sequence;
            if (stub_pids[i].task == NULL) {
                stub_pids[i].task = kb_loader_module_current_task(kb_loader_active_module());
            }
            return &stub_pids[i];
        }
    }
    for (size_t i = 0; i < sizeof(stub_pids) / sizeof(stub_pids[0]); i++) {
        if (stub_pids[i].nr == 0) {
            stub_pids[i] = (kb_stub_pid_t){
                .count = 1,
                .level = 0,
                .nr = nr,
                .task = kb_loader_module_current_task(kb_loader_active_module()),
                .last_used = ++stub_pid_use_sequence,
            };
            return &stub_pids[i];
        }
    }

    /*
     * find_vpid() itself does not acquire a reference.  Keep a bounded cache
     * for the synthetic pid objects and recycle only an old registry-only
     * entry.  Linux-held tty/session references raise count through get_pid()
     * and are released through kb_put_pid(), so they are never selected here.
     * A task PID link is an uncounted Linux reference, so every link must also
     * have moved to a newer record before an entry is eligible for reuse.
     */
    kb_stub_pid_t *reclaim = NULL;
    for (size_t i = 0; i < sizeof(stub_pids) / sizeof(stub_pids[0]); i++) {
        kb_stub_pid_t *candidate = &stub_pids[i];
        if (__atomic_load_n(&candidate->count, __ATOMIC_RELAXED) != 1) {
            continue;
        }
        int linked = 0;
        for (size_t type = 0; type < KB_STUB_PID_TYPE_COUNT; type++) {
            if (candidate->tasks[type] != NULL) {
                linked = 1;
                break;
            }
        }
        if (linked) {
            continue;
        }
        int pending = 0;
        for (size_t signal_index = 0;
             signal_index < pending_pgrp_signal_count();
             signal_index++)
        {
            if (pending_pgrp_signals[signal_index].sequence != 0 &&
                pending_pgrp_signals[signal_index].pgrp == candidate)
            {
                pending = 1;
                break;
            }
        }
        if (pending || (reclaim != NULL && reclaim->last_used <= candidate->last_used)) {
            continue;
        }
        reclaim = candidate;
    }
    if (reclaim != NULL) {
        *reclaim = (kb_stub_pid_t){
            .count = 1,
            .level = 0,
            .nr = nr,
            .task = kb_loader_module_current_task(kb_loader_active_module()),
            .last_used = ++stub_pid_use_sequence,
        };
        return reclaim;
    }
    return NULL;
}

void kb_put_pid(void *pid)
{
    kb_stub_pid_t *record = (kb_stub_pid_t *)pid;
    if (record == NULL) {
        return;
    }
    int count = __atomic_load_n(&record->count, __ATOMIC_RELAXED);
    while (count > 1 &&
           !__atomic_compare_exchange_n(
               &record->count,
               &count,
               count - 1,
               0,
               __ATOMIC_RELEASE,
               __ATOMIC_RELAXED))
    {
    }
}

void *kb_pid_task(void *pid, int type)
{
    kb_stub_pid_t *record = (kb_stub_pid_t *)pid;
    if (record == NULL) {
        return NULL;
    }
    void *task = record->task;
    if (task == NULL) {
        task = kb_loader_module_current_task(kb_loader_active_module());
        record->task = task;
    }
    if (task != NULL && type >= 0 && type < KB_STUB_PID_TYPE_COUNT) {
        void *node = (uint8_t *)task + KB_STUB_TASK_PID_LINKS_OFFSET +
            ((size_t)type * KB_STUB_TASK_PID_LINK_STRIDE);
        for (size_t i = 0; i < sizeof(stub_pids) / sizeof(stub_pids[0]); i++) {
            if (stub_pids[i].tasks[type] == node) {
                stub_pids[i].tasks[type] = NULL;
            }
        }
        void *next = NULL;
        void *pprev = &record->tasks[type];
        memcpy(node, &next, sizeof(next));
        memcpy((uint8_t *)node + sizeof(void *), &pprev, sizeof(pprev));
        record->tasks[type] = node;
    }
    return task;
}

int kb_pid_vnr(void *pid)
{
    kb_stub_pid_t *record = (kb_stub_pid_t *)pid;
    return record == NULL ? 0 : record->nr;
}

typedef struct kb_substring {
    char *from;
    char *to;
} kb_substring_t;

static int kb_parse_substring_int(const void *substring, int base, int *result)
{
    if (substring == NULL || result == NULL) {
        return -22;
    }
    const kb_substring_t *span = (const kb_substring_t *)substring;
    if (span->from == NULL || span->to == NULL || span->to < span->from) {
        return -22;
    }
    size_t len = (size_t)(span->to - span->from);
    if (len >= 64) {
        return -22;
    }
    char buffer[64];
    memcpy(buffer, span->from, len);
    buffer[len] = '\0';
    char *end = NULL;
    long value = strtol(buffer, &end, base);
    if (end == buffer || *end != '\0') {
        return -22;
    }
    *result = (int)value;
    return 0;
}

int kb_match_token(char *string, const void *table, void *args)
{
    (void)table;
    (void)args;
    return string == NULL || string[0] == '\0' ? 0 : -1;
}

int kb_match_int(const void *substring, int *result)
{
    return kb_parse_substring_int(substring, 10, result);
}

int kb_match_octal(const void *substring, int *result)
{
    return kb_parse_substring_int(substring, 8, result);
}

int64_t kb_ktime_get_real_seconds(void)
{
#if defined(__pachaos__)
    static int64_t synthetic_seconds = 1;
    return synthetic_seconds++;
#else
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 1;
#endif
}

void kb_string_get_size(uint64_t size, uint64_t blk_size, int units, char *buf, int len)
{
    (void)units;
    if (buf == NULL || len <= 0) {
        return;
    }
    const uint64_t bytes = blk_size == 0 ? size : size * blk_size;
    (void)snprintf(buf, (size_t)len, "%llu B", (unsigned long long)bytes);
}

uint32_t kb_get_random_u32_below(uint32_t ceil)
{
    if (ceil == 0) {
        return 0;
    }
    return 1u % ceil;
}

uint16_t kb_get_random_u16(void)
{
    return 1;
}

void kb_generate_random_uuid(unsigned char *uuid)
{
    if (uuid == NULL) {
        return;
    }
    memset(uuid, 0, 16);
    uuid[15] = 1;
}

char *kb_d_path(void *path, char *buffer, int buffer_length)
{
    (void)path;
    if (buffer == NULL || buffer_length <= 0) {
        return NULL;
    }
    buffer[0] = '\0';
    return buffer;
}

size_t kb_strlcpy(char *dst, const char *src, size_t size)
{
    if (src == NULL) {
        if (dst != NULL && size > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    size_t len = strlen(src);
    if (dst != NULL && size > 0) {
        size_t copy = len >= size ? size - 1u : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

void *kb_kmemdup_nul(const void *src, size_t len, unsigned int flags)
{
    char *dst = kb_kmalloc(len + 1u, flags);
    if (dst == NULL) {
        return NULL;
    }
    if (src != NULL && len > 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
    return dst;
}

long kb_sized_strscpy(char *dst, const char *src, size_t size)
{
    if (dst == NULL || src == NULL) {
        return -22;
    }
    if (size == 0) {
        return -7;
    }
    size_t i = 0;
    for (; i + 1u < size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return src[i] == '\0' ? (long)i : -7;
}

char *kb_skip_spaces(const char *str)
{
    if (str == NULL) {
        return NULL;
    }
    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    return (char *)str;
}

void kb_memcpy_and_pad(void *dest, size_t dest_len, const void *src, size_t count, int pad)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }
    size_t copy_len = count < dest_len ? count : dest_len;
    if (copy_len > 0 && src != NULL) {
        memcpy(dest, src, copy_len);
    }
    if (dest_len > copy_len) {
        memset((unsigned char *)dest + copy_len, pad, dest_len - copy_len);
    }
}

int64_t kb_vfs_setpos(void *file, int64_t offset, int64_t maxsize)
{
    enum {
        FILE_MODE_OFFSET = 0x0c,
        FILE_VERSION_OFFSET = 0x30,
        FILE_POSITION_OFFSET = 0x70,
        FMODE_UNSIGNED_OFFSET = 0x2000,
    };
    if (file == NULL) {
        return -22;
    }
    uint32_t mode = 0;
    memcpy(&mode, (const unsigned char *)file + FILE_MODE_OFFSET, sizeof(mode));
    if ((offset < 0 && (mode & FMODE_UNSIGNED_OFFSET) == 0) || offset > maxsize) {
        return -22;
    }
    int64_t current = 0;
    memcpy(&current, (const unsigned char *)file + FILE_POSITION_OFFSET, sizeof(current));
    if (current != offset) {
        memcpy((unsigned char *)file + FILE_POSITION_OFFSET, &offset, sizeof(offset));
        const uint64_t version = 0;
        memcpy((unsigned char *)file + FILE_VERSION_OFFSET, &version, sizeof(version));
    }
    return offset;
}

char *kb_strreplace(char *s, char old_char, char new_char)
{
    if (s == NULL) {
        return NULL;
    }
    for (char *p = s; *p != '\0'; p++) {
        if (*p == old_char) {
            *p = new_char;
        }
    }
    return s;
}

size_t kb_memweight(const void *ptr, size_t bytes)
{
    if (ptr == NULL) {
        return 0;
    }
    const unsigned char *p = ptr;
    size_t count = 0;
    for (size_t i = 0; i < bytes; i++) {
        unsigned char value = p[i];
        while (value != 0) {
            count += (size_t)(value & 1u);
            value >>= 1;
        }
    }
    return count;
}

static int low_or_error_ptr(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

int kb_list_add_valid_or_report(void *new_entry, void *prev, void *next)
{
    int result = 0;
    if (low_or_error_ptr(new_entry) || low_or_error_ptr(prev) || low_or_error_ptr(next)) {
        goto out;
    }

    void *prev_next = NULL;
    void *next_prev = NULL;
    memcpy(&prev_next, prev, sizeof(prev_next));
    memcpy(&next_prev, (const unsigned char *)next + sizeof(void *), sizeof(next_prev));
    if (prev_next != next || next_prev != prev) {
        goto out;
    }
    if (new_entry == prev || new_entry == next) {
        goto out;
    }
    result = 1;

out:
    return result;
}

int kb_list_del_entry_valid_or_report(void *entry)
{
    int result = 0;
    if (low_or_error_ptr(entry)) {
        goto out;
    }

    void *next = NULL;
    void *prev = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(&prev, (const unsigned char *)entry + sizeof(void *), sizeof(prev));
    if (low_or_error_ptr(next) || low_or_error_ptr(prev)) {
        goto out;
    }

    void *prev_next = NULL;
    void *next_prev = NULL;
    memcpy(&prev_next, prev, sizeof(prev_next));
    memcpy(&next_prev, (const unsigned char *)next + sizeof(void *), sizeof(next_prev));
    result = prev_next == entry && next_prev == entry;

out:
    return result;
}

void kb_list_add(void *new_entry, void *prev, void *next)
{
    if (low_or_error_ptr(new_entry) || low_or_error_ptr(prev) || low_or_error_ptr(next)) {
        return;
    }
    memcpy(new_entry, &next, sizeof(next));
    memcpy((unsigned char *)new_entry + sizeof(void *), &prev, sizeof(prev));
    memcpy(prev, &new_entry, sizeof(new_entry));
    memcpy((unsigned char *)next + sizeof(void *), &new_entry, sizeof(new_entry));
}

void kb_list_del(void *entry)
{
    if (low_or_error_ptr(entry)) {
        return;
    }
    void *next = NULL;
    void *prev = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(&prev, (const unsigned char *)entry + sizeof(void *), sizeof(prev));
    if (low_or_error_ptr(next) || low_or_error_ptr(prev)) {
        return;
    }
    memcpy(prev, &next, sizeof(next));
    memcpy((unsigned char *)next + sizeof(void *), &prev, sizeof(prev));
    memcpy(entry, &entry, sizeof(entry));
    memcpy((unsigned char *)entry + sizeof(void *), &entry, sizeof(entry));
}

enum {
    KB_KTHREAD_TASK_BYTES = 4096,
    KB_KTHREAD_STACK_BYTES = 256 * 1024,
};

static void kb_kthread_finish_active(int result) __attribute__((noreturn));

static void kb_kthread_finish_active(int result)
{
    kb_kthread_dispatch_frame_t *frame = kthread_dispatch_frame;
    if (frame != NULL && frame->record != NULL) {
        frame->record->result = result;
        frame->record->finished = 1;
        frame->record->activated = 0;
    }
    if (frame == NULL) {
        abort();
    }
    longjmp(frame->context, 1);
}

static void kb_kthread_bootstrap(kb_kthread_record_t *record) __attribute__((noreturn));

static void kb_kthread_bootstrap(kb_kthread_record_t *record)
{
    if (record == NULL || record->threadfn == NULL) {
        kb_kthread_finish_active(-22);
    }
    const int result = record->threadfn(record->data);
    kb_kthread_finish_active(result);
}

static void kb_kthread_start_on_stack(kb_kthread_record_t *record) __attribute__((noreturn, noinline));

static void kb_kthread_start_on_stack(kb_kthread_record_t *record)
{
#if defined(__x86_64__) && !defined(_MSC_VER)
    uintptr_t stack_top = (uintptr_t)record->stack + record->stack_size;
    stack_top &= ~(uintptr_t)15u;
    void (*entry)(kb_kthread_record_t *) = kb_kthread_bootstrap;
    __asm__ volatile(
        "mov %[stack_top], %%rsp\n\t"
        "mov %[record], %%rdi\n\t"
        "call *%[entry]\n\t"
        "ud2\n\t"
        :
        : [stack_top] "r"(stack_top),
          [record] "r"(record),
          [entry] "r"(entry)
        : "memory", "rdi");
    __builtin_unreachable();
#else
    (void)record;
    kb_kthread_finish_active(-95);
#endif
}

int kb_kthread_yield_current(void)
{
    if (active_kthread == NULL || kthread_dispatch_frame == NULL ||
        kb_loader_current_task() != active_kthread->task)
    {
        return 0;
    }
    if (setjmp(active_kthread->context) == 0) {
        const uintptr_t *context_words =
            (const uintptr_t *)(const void *)active_kthread->context;
        active_kthread->saved_context_rsp = context_words[6];
        active_kthread->saved_context_rip = context_words[7];
        active_kthread->context_generation++;
        longjmp(kthread_dispatch_frame->context, 1);
    }
    return 1;
}

void kb_schedule(void)
{
    if (active_kthread == NULL || kthread_dispatch_frame == NULL ||
        kb_loader_current_task() != active_kthread->task)
    {
        kb_run_deferred_work();
        return;
    }

    /*
     * schedule() only blocks a task after a wait primitive changed its
     * state away from TASK_RUNNING.  Kobox does not expose Linux task state
     * to the host scheduler, so prepare_to_wait*() records that transition
     * explicitly.  A plain schedule() remains a cooperative yield.
     *
     * A wake between prepare_to_wait*() and schedule() must not be lost.
     * In that case Linux would observe TASK_RUNNING and schedule() may return
     * immediately, which is exactly what the wake_pending branch models.
     */
    if (active_kthread->wait_prepared) {
        if (active_kthread->wake_pending) {
            active_kthread->wait_prepared = 0;
            active_kthread->wake_pending = 0;
            return;
        }
        active_kthread->activated = 0;
    }

    (void)kb_kthread_yield_current();
    active_kthread->wait_prepared = 0;
    active_kthread->wake_pending = 0;
}

void *kb_kthread_current_task(void)
{
    return kb_loader_current_task();
}

void kb_kthread_prepare_wait(void *task)
{
    for (kb_kthread_record_t *record = kthread_records; record != NULL; record = record->next) {
        if (record->task == task) {
            record->wait_prepared = 1;
            record->wake_pending = 0;
            return;
        }
    }
}

void kb_kthread_finish_wait(void *task)
{
    for (kb_kthread_record_t *record = kthread_records; record != NULL; record = record->next) {
        if (record->task == task) {
            record->wait_prepared = 0;
            record->wake_pending = 0;
            return;
        }
    }
}

static void kb_kthread_run_one(kb_kthread_record_t *record)
{
    if (record == NULL || !record->activated || record->finished) {
        return;
    }
    for (kb_kthread_dispatch_frame_t *frame = kthread_dispatch_frame;
         frame != NULL;
         frame = frame->previous)
    {
        if (frame->record == record) {
            return;
        }
    }
    void *previous_task = kb_loader_current_task();
    kb_module_t *previous_module = kb_loader_active_module();
    kb_kthread_record_t *previous_active = active_kthread;
    kb_kthread_dispatch_frame_t frame = {
        .record = record,
        .previous = kthread_dispatch_frame,
    };
    const unsigned long kernel_gs =
        kb_module_kernel_gs_for_address((const void *)record->threadfn);
    unsigned long old_gs = 0;
    const int has_gs = kernel_gs != 0 &&
        kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    if (kernel_gs != 0 && !has_gs) {
        record->result = -95;
        record->finished = 1;
        record->activated = 0;
        return;
    }
    active_kthread = record;
    kthread_dispatch_frame = &frame;
    kb_loader_set_current_task_for_all_modules(record->task);
    kb_loader_set_active_module(record->owner);
    if (setjmp(frame.context) == 0) {
        if (!record->started) {
            record->started = 1;
            kb_kthread_start_on_stack(record);
        }
        const uintptr_t *context_words =
            (const uintptr_t *)(const void *)record->context;
        if (record->context_generation == 0 ||
            context_words[6] != record->saved_context_rsp ||
            context_words[7] != record->saved_context_rip)
        {
            fprintf(
                stderr,
                "kobox kthread: corrupt resume context name=%s record=%p task=%p "
                "threadfn=%p generation=%llu rsp=%p expected_rsp=%p "
                "rip=%p expected_rip=%p\n",
                record->name,
                (void *)record,
                record->task,
                (void *)record->threadfn,
                (unsigned long long)record->context_generation,
                (void *)context_words[6],
                (void *)record->saved_context_rsp,
                (void *)context_words[7],
                (void *)record->saved_context_rip);
            record->result = -14;
            record->finished = 1;
            record->activated = 0;
            longjmp(frame.context, 1);
        }
        longjmp(record->context, 1);
    }
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_loader_set_active_module(previous_module);
    kb_loader_set_current_task_for_all_modules(previous_task);
    kthread_dispatch_frame = frame.previous;
    active_kthread = previous_active;
}

void kb_kthread_run_ready(void)
{
    for (kb_kthread_record_t *record = kthread_records; record != NULL; record = record->next) {
        kb_kthread_run_one(record);
    }
}

int kb_kthread_should_stop(void)
{
    return active_kthread != NULL &&
        kb_loader_current_task() == active_kthread->task &&
        active_kthread->stop_requested;
}

int kb_kthread_stop(void *task)
{
    for (kb_kthread_record_t *record = kthread_records; record != NULL; record = record->next) {
        if (record->task != task) {
            continue;
        }
        record->stop_requested = 1;
        record->activated = 1;
        for (unsigned int i = 0; i < 1024 && !record->finished; ++i) {
            kb_kthread_run_one(record);
        }
        return record->finished ? record->result : -16;
    }
    return -22;
}

void *kb_kthread_create_on_node(int (*threadfn)(void *data), void *data, int node, const char *namefmt, ...)
{
    kb_kthread_record_t *record = calloc(1, sizeof(*record));
    void *task = kb_loader_clone_execution_task();
    void *stack = calloc(1, KB_KTHREAD_STACK_BYTES);
    if (record == NULL || task == NULL || stack == NULL) {
        free(record);
        free(task);
        free(stack);
        return NULL;
    }
    record->task = task;
    record->threadfn = threadfn;
    record->data = data;
    record->owner = kb_module_find_owner_for_address((const void *)threadfn);
    record->stack = stack;
    record->stack_size = KB_KTHREAD_STACK_BYTES;
    record->node = node;
    if (namefmt != NULL) {
        va_list ap;
        va_start(ap, namefmt);
        vsnprintf(record->name, sizeof(record->name), namefmt, ap);
        va_end(ap);
    }
    record->next = kthread_records;
    kthread_records = record;
    return task;
}

int kb_wake_up_process(void *task)
{
    for (kb_kthread_record_t *record = kthread_records; record != NULL; record = record->next) {
        if (record->task != task) {
            continue;
        }
        if (record->wait_prepared) {
            record->wake_pending = 1;
        }
        record->activated = 1;
        return 1;
    }
    return task != NULL ? 1 : 0;
}

#define KB_RB_PARENT_MASK (~(uintptr_t)3u)

enum {
    KB_RB_PARENT_COLOR_OFFSET = 0x0,
    KB_RB_RIGHT_OFFSET = 0x8,
    KB_RB_LEFT_OFFSET = 0x10,
};

static void *rb_parent(const void *node)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    uintptr_t parent_color = 0;
    memcpy(&parent_color, node, sizeof(parent_color));
    return (void *)(parent_color & KB_RB_PARENT_MASK);
}

static void rb_set_parent_keep_color(void *node, void *parent)
{
    if (low_or_error_ptr(node)) {
        return;
    }
    uintptr_t parent_color = 0;
    memcpy(&parent_color, node, sizeof(parent_color));
    parent_color = ((uintptr_t)parent & KB_RB_PARENT_MASK) | (parent_color & 3u);
    memcpy(node, &parent_color, sizeof(parent_color));
}

static void *rb_child(const void *node, size_t offset)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    void *child = NULL;
    memcpy(&child, (const unsigned char *)node + offset, sizeof(child));
    return low_or_error_ptr(child) ? NULL : child;
}

static void rb_set_child(void *node, size_t offset, void *child)
{
    if (!low_or_error_ptr(node)) {
        memcpy((unsigned char *)node + offset, &child, sizeof(child));
    }
}

void kb_rb_insert_color(void *node, void *root)
{
    (void)node;
    (void)root;
}

void *kb_rb_first(void *root)
{
    if (low_or_error_ptr(root)) {
        return NULL;
    }
    void *node = NULL;
    memcpy(&node, root, sizeof(node));
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    for (;;) {
        void *left = rb_child(node, KB_RB_LEFT_OFFSET);
        if (left == NULL) {
            return node;
        }
        node = left;
    }
}

void *kb_rb_next(void *node)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    void *right = rb_child(node, KB_RB_RIGHT_OFFSET);
    if (right != NULL) {
        node = right;
        for (;;) {
            void *left = rb_child(node, KB_RB_LEFT_OFFSET);
            if (left == NULL) {
                return node;
            }
            node = left;
        }
    }
    void *parent = rb_parent(node);
    while (parent != NULL && node == rb_child(parent, KB_RB_RIGHT_OFFSET)) {
        node = parent;
        parent = rb_parent(parent);
    }
    return parent;
}

void *kb_rb_prev(void *node)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    void *left = rb_child(node, KB_RB_LEFT_OFFSET);
    if (left != NULL) {
        node = left;
        for (;;) {
            void *right = rb_child(node, KB_RB_RIGHT_OFFSET);
            if (right == NULL) {
                return node;
            }
            node = right;
        }
    }
    void *parent = rb_parent(node);
    while (parent != NULL && node == rb_child(parent, KB_RB_LEFT_OFFSET)) {
        node = parent;
        parent = rb_parent(parent);
    }
    return parent;
}

void kb_rb_erase(void *node, void *root)
{
    if (low_or_error_ptr(node) || low_or_error_ptr(root)) {
        return;
    }

    void *left = rb_child(node, KB_RB_LEFT_OFFSET);
    void *right = rb_child(node, KB_RB_RIGHT_OFFSET);
    void *replacement = NULL;
    if (left == NULL) {
        replacement = right;
    } else if (right == NULL) {
        replacement = left;
    } else {
        replacement = right;
        while (rb_child(replacement, KB_RB_LEFT_OFFSET) != NULL) {
            replacement = rb_child(replacement, KB_RB_LEFT_OFFSET);
        }
        kb_rb_erase(replacement, root);
        left = rb_child(node, KB_RB_LEFT_OFFSET);
        right = rb_child(node, KB_RB_RIGHT_OFFSET);
        rb_set_child(replacement, KB_RB_LEFT_OFFSET, left);
        rb_set_child(replacement, KB_RB_RIGHT_OFFSET, right);
        if (left != NULL) {
            rb_set_parent_keep_color(left, replacement);
        }
        if (right != NULL) {
            rb_set_parent_keep_color(right, replacement);
        }
    }

    void *parent = rb_parent(node);
    if (parent == NULL) {
        memcpy(root, &replacement, sizeof(replacement));
    } else if (node == rb_child(parent, KB_RB_LEFT_OFFSET)) {
        rb_set_child(parent, KB_RB_LEFT_OFFSET, replacement);
    } else {
        rb_set_child(parent, KB_RB_RIGHT_OFFSET, replacement);
    }
    if (replacement != NULL) {
        rb_set_parent_keep_color(replacement, parent);
    }
    rb_set_child(node, KB_RB_LEFT_OFFSET, NULL);
    rb_set_child(node, KB_RB_RIGHT_OFFSET, NULL);
    rb_set_parent_keep_color(node, NULL);
}

static kb_ida_record_t *ida_record_for(void *ida, int create)
{
    if (ida == NULL) {
        return NULL;
    }
    for (kb_ida_record_t *record = ida_records; record != NULL; record = record->next) {
        if (record->ida == ida) {
            return record;
        }
    }
    if (!create) {
        return NULL;
    }
    kb_ida_record_t *record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->ida = ida;
    record->next = ida_records;
    ida_records = record;
    return record;
}

static int ida_contains(const kb_ida_record_t *record, unsigned int id)
{
    if (record == NULL) {
        return 0;
    }
    for (size_t i = 0; i < record->count; i++) {
        if (record->ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

static int ida_ensure_capacity(kb_ida_record_t *record, size_t required_capacity)
{
    if (record == NULL) {
        return 0;
    }
    if (record->capacity >= required_capacity) {
        return 1;
    }
    size_t next_capacity = record->capacity == 0 ? 8 : record->capacity * 2u;
    while (next_capacity < required_capacity) {
        if (next_capacity > SIZE_MAX / 2u) {
            return 0;
        }
        next_capacity *= 2u;
    }
    if (next_capacity > SIZE_MAX / sizeof(*record->ids)) {
        return 0;
    }
    unsigned int *ids = kb_krealloc_managed(record->ids, next_capacity * sizeof(*ids), 0);
    if (ids == NULL) {
        return 0;
    }
    record->ids = ids;
    record->capacity = next_capacity;
    return 1;
}

int kb_ida_alloc_range(void *ida, unsigned int min, unsigned int max, unsigned int flags)
{
    (void)flags;
    if (ida == NULL || max < min) {
        return -22;
    }
    kb_ida_record_t *record = ida_record_for(ida, 1);
    if (record == NULL) {
        return -12;
    }
    for (unsigned int id = min; id <= max; id++) {
        if (!ida_contains(record, id)) {
            if (!ida_ensure_capacity(record, record->count + 1u)) {
                return -12;
            }
            record->ids[record->count++] = id;
            return (int)id;
        }
        if (id == UINT32_MAX) {
            break;
        }
    }
    return -28;
}

void kb_ida_free(void *ida, unsigned int id)
{
    kb_ida_record_t *record = ida_record_for(ida, 0);
    if (record == NULL) {
        return;
    }
    for (size_t i = 0; i < record->count; i++) {
        if (record->ids[i] == id) {
            record->ids[i] = record->ids[record->count - 1u];
            record->count--;
            return;
        }
    }
}

void kb_ida_destroy(void *ida)
{
    kb_ida_record_t **cursor = &ida_records;
    while (*cursor != NULL) {
        kb_ida_record_t *record = *cursor;
        if (record->ida == ida) {
            *cursor = record->next;
            kb_kfree(record->ids);
            kb_kfree(record);
            return;
        }
        cursor = &record->next;
    }
}

int kb_ida_simple_get(void *ida, unsigned int start, unsigned int end, unsigned int flags)
{
    const unsigned int max = end == 0 ? UINT32_MAX : end - 1u;
    if (end != 0 && end <= start) {
        return -28;
    }
    return kb_ida_alloc_range(ida, start, max, flags);
}

void kb_ida_simple_remove(void *ida, unsigned int id)
{
    kb_ida_free(ida, id);
}

void *kb_alloc_pages_exact(size_t size, unsigned int flags)
{
    if (size == 0) {
        size = 1;
    }

    size_t page_count = (size + KB_STUB_PAGE_SIZE - 1u) / KB_STUB_PAGE_SIZE;
    unsigned int order = 0;
    size_t order_pages = 1;
    while (order_pages < page_count) {
        order_pages <<= 1u;
        order++;
    }

    void *page = kb_kvm_alloc_pages_stub(flags, order);
    void *payload = page_payload_from_struct_page(page);
    if (page == NULL || payload == NULL) {
        return NULL;
    }

    uint64_t expected = page_phys_from_struct_page(page);
    if (trace_dma_enabled()) {
        fprintf(stderr,
            "kobox dma: alloc_pages_exact size=0x%zx order=%u page=%p payload=%p phys=0x%llx\n",
            size,
            order,
            page,
            payload,
            (unsigned long long)expected);
    }
    return payload;
}

void kb_free_pages_exact(void *virt, size_t size)
{
    kb_kvm_free_pages_exact(virt, size);
}

int kb_alloc_cpumask_var(void *mask_out, unsigned int flags)
{
    if (mask_out == NULL) {
        return 0;
    }
    unsigned long *mask = kb_kzalloc(sizeof(*mask), flags);
    if (mask == NULL) {
        return 0;
    }
    *mask = 1;
    memcpy(mask_out, &mask, sizeof(mask));
    return 1;
}

void kb_free_cpumask_var(void *mask)
{
    kb_kfree(mask);
}

void *kb_alloc_stub(void)
{
    return kb_kzalloc(4096, 0);
}

void kb_free_first_arg_stub(void *ptr, void *ignored)
{
    (void)ignored;
    kb_kfree(ptr);
}

int kb_percpu_counter_init_many_stub(void *counters, long amount, unsigned int batch, unsigned int count, void *key)
{
    (void)batch;
    (void)key;
    if (counters == NULL || count == 0) {
        return -22;
    }
    int32_t *local_counts = kb_kzalloc((size_t)count * sizeof(*local_counts), 0);
    if (local_counts == NULL) {
        void *none = NULL;
        memcpy((uint8_t *)counters + KB_PERCPU_COUNTER_POINTER_OFFSET, &none, sizeof(none));
        return -12;
    }
    for (unsigned int i = 0; i < count; i++) {
        int64_t value = (int64_t)amount;
        uint8_t *counter = (uint8_t *)counters + (size_t)i * KB_PERCPU_COUNTER_STRIDE;
        int32_t *local_count = &local_counts[i];
        memcpy(counter + KB_PERCPU_COUNTER_COUNT_OFFSET, &value, sizeof(value));
        memcpy(counter + KB_PERCPU_COUNTER_POINTER_OFFSET, &local_count, sizeof(local_count));
    }
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-core: percpu_counter_init_many amount=%ld batch=%u count=%u\n", amount, batch, count);
    }
    return 0;
}

void kb_percpu_counter_destroy_many(void *counters, unsigned int count)
{
    if (counters == NULL || count == 0) {
        return;
    }
    int32_t *local_counts = NULL;
    memcpy(
        &local_counts,
        (const uint8_t *)counters + KB_PERCPU_COUNTER_POINTER_OFFSET,
        sizeof(local_counts));
    for (unsigned int i = 0; i < count; i++) {
        uint8_t *counter = (uint8_t *)counters + (size_t)i * KB_PERCPU_COUNTER_STRIDE;
        void *none = NULL;
        memcpy(counter + KB_PERCPU_COUNTER_POINTER_OFFSET, &none, sizeof(none));
    }
    kb_kfree(local_counts);
}

void kb_percpu_counter_add_batch_stub(void *counter, int64_t amount, int32_t batch)
{
    (void)batch;
    if (counter == NULL) {
        return;
    }
    int64_t value = 0;
    memcpy(&value, (uint8_t *)counter + KB_PERCPU_COUNTER_COUNT_OFFSET, sizeof(value));
    value += amount;
    memcpy((uint8_t *)counter + KB_PERCPU_COUNTER_COUNT_OFFSET, &value, sizeof(value));
}

int64_t kb_percpu_counter_sum_stub(void *counter)
{
    if (counter == NULL) {
        return 0;
    }
    int64_t value = 0;
    memcpy(&value, (uint8_t *)counter + KB_PERCPU_COUNTER_COUNT_OFFSET, sizeof(value));
    return value;
}

void *kb_crypto_alloc_shash_stub(const char *alg_name, unsigned int type, unsigned int mask)
{
    (void)alg_name;
    (void)type;
    (void)mask;
    void *tfm = kb_kzalloc(4096, 0);
    if (tfm != NULL) {
        uint32_t digest_size = 4;
        memcpy(tfm, &digest_size, sizeof(digest_size));
    }
    return tfm;
}

static uint32_t crc32c_update_software(uint32_t crc, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82f63b78u & mask);
        }
    }
    return crc;
}

#if defined(__x86_64__)
static int crc32c_hardware_available(void)
{
    static int cached = -1;
    int available = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (available >= 0) {
        return available;
    }
    uint32_t eax = 1;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ __volatile__(
        "cpuid"
        : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    available = (ecx & (UINT32_C(1) << 20)) != 0;
    __atomic_store_n(&cached, available, __ATOMIC_RELEASE);
    return available;
}

static uint32_t crc32c_update_hardware(uint32_t crc, const unsigned char *data, size_t len)
{
    uint64_t accumulator = crc;
    while (len >= sizeof(uint64_t)) {
        uint64_t word = 0;
        memcpy(&word, data, sizeof(word));
        __asm__ __volatile__("crc32q %1, %0" : "+r"(accumulator) : "rm"(word));
        data += sizeof(word);
        len -= sizeof(word);
    }
    crc = (uint32_t)accumulator;
    while (len != 0) {
        const uint8_t byte = *data++;
        __asm__ __volatile__("crc32b %1, %0" : "+r"(crc) : "rm"(byte));
        --len;
    }
    return crc;
}
#endif

static uint32_t crc32c_update(uint32_t crc, const unsigned char *data, size_t len)
{
#if defined(__x86_64__)
    if (crc32c_hardware_available()) {
        return crc32c_update_hardware(crc, data, len);
    }
#endif
    return crc32c_update_software(crc, data, len);
}

uint32_t kb_crc32_le(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *bytes = data;
    if (bytes == NULL && len != 0) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

uint32_t kb_crc32_be(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *bytes = data;
    if (bytes == NULL && len != 0) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)bytes[i] << 24;
        for (unsigned int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc >> 31);
            crc = (crc << 1) ^ (0x04c11db7u & mask);
        }
    }
    return crc;
}

void *kb_memchr_inv(const void *start, int value, size_t bytes)
{
    if (start == NULL) {
        return NULL;
    }
    const unsigned char expected = (unsigned char)value;
    const unsigned char *cursor = start;
    for (size_t i = 0; i < bytes; i++) {
        if (cursor[i] != expected) {
            return (void *)(cursor + i);
        }
    }
    return NULL;
}

static int crypto_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_CRYPTO");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

int kb_crypto_shash_update_stub(void *desc, const void *data, unsigned int len)
{
    if (desc == NULL || (data == NULL && len != 0)) {
        return -22;
    }
    uint32_t crc = 0;
    memcpy(&crc, (const unsigned char *)desc + sizeof(void *), sizeof(crc));
    uint32_t before = crc;
    crc = crc32c_update(crc, (const unsigned char *)data, len);
    memcpy((unsigned char *)desc + sizeof(void *), &crc, sizeof(crc));
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-crypto: shash_update len=%u before=0x%08x after=0x%08x data=", len, before, crc);
        unsigned int preview = len < 64u ? len : 64u;
        for (unsigned int i = 0; i < preview; i++) {
            fprintf(stderr, "%02x", ((const unsigned char *)data)[i]);
        }
        fprintf(stderr, "\n");
    }
    return 0;
}

int kb_crypto_shash_final_stub(void *desc, void *out)
{
    if (desc == NULL || out == NULL) {
        return -22;
    }
    uint32_t crc = 0;
    memcpy(&crc, (const unsigned char *)desc + sizeof(void *), sizeof(crc));
    memcpy(out, &crc, sizeof(crc));
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-crypto: shash_final out=0x%08x\n", crc);
    }
    return 0;
}

int kb_crypto_shash_tfm_digest_stub(void *tfm, const void *data, unsigned int len, void *out)
{
    (void)tfm;
    if ((data == NULL && len != 0) || out == NULL) {
        return -22;
    }
    uint32_t crc = crc32c_update(0xffffffffu, (const unsigned char *)data, len);
    memcpy(out, &crc, sizeof(crc));
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-crypto: shash_tfm_digest len=%u out=0x%08x\n", len, crc);
    }
    return 0;
}

void *kb_identity_ptr(void *ptr)
{
    return ptr;
}

const char *kb_empty_string(void)
{
    return "";
}
