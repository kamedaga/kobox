#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "linux_subsystem/fs/kernel_object_registry.h"
#include "loader/module_context.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

kb_chrdev_record_t kb_chrdev_records[KB_KERNEL_OBJECT_MAX];
kb_proc_record_t kb_proc_records[KB_KERNEL_OBJECT_MAX];
kb_class_record_t kb_class_records[KB_KERNEL_OBJECT_MAX];
kb_cdev_record_t kb_cdev_records[KB_KERNEL_OBJECT_MAX];
kb_fd_record_t kb_fd_records[KB_FAKE_FD_MAX];
kb_fake_file_record_t kb_fake_file_records[KB_KERNEL_OBJECT_MAX];
kb_vma_record_t kb_vma_records[KB_FAKE_VMA_MAX];
int kb_kernel_object_summary_registered;
unsigned kb_next_dynamic_major = 240;
unsigned kb_next_fake_fd = 3;

uint32_t kb_linux_kernel_decode_major(uint64_t dev)
{
    return (uint32_t)(dev >> 20);
}

uint32_t kb_linux_kernel_decode_minor(uint64_t dev)
{
    return (uint32_t)(dev & 0xfffffu);
}

uint32_t kb_linux_kernel_encode_dev(unsigned major, unsigned minor)
{
    return (major << 20) | (minor & 0xfffffu);
}

const char *kb_linux_kernel_chrdev_name_for_dev(uint64_t dev)
{
    const uint32_t major = kb_linux_kernel_decode_major(dev);
    const uint32_t minor = kb_linux_kernel_decode_minor(dev);
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_chrdev_records[i].name == NULL || kb_chrdev_records[i].major != major) {
            continue;
        }
        const unsigned first = kb_chrdev_records[i].baseminor;
        const unsigned last = first + kb_chrdev_records[i].count;
        if (minor >= first && minor < last) {
            return kb_chrdev_records[i].name;
        }
    }
    return "(unmatched)";
}

static void kb_linux_kernel_write_ptr_field(void *base, size_t offset, const void *value)
{
    if (base != NULL) {
        memcpy((uint8_t *)base + offset, &value, sizeof(value));
    }
}

static void kb_linux_kernel_write_u64_field(void *base, size_t offset, uint64_t value)
{
    if (base != NULL) {
        memcpy((uint8_t *)base + offset, &value, sizeof(value));
    }
}

void kb_linux_kernel_prepare_fake_vma(uint8_t *vma, void *mm, uint64_t start, uint64_t end, uint64_t pgoff)
{
    if (vma == NULL) {
        return;
    }
    memset(vma, 0, KB_FAKE_VMA_SIZE);
    kb_linux_kernel_write_u64_field(vma, 0x00, start);
    kb_linux_kernel_write_u64_field(vma, 0x08, end);
    kb_linux_kernel_write_ptr_field(vma, 0x10, mm);
    kb_linux_kernel_write_u64_field(vma, 0x20, 0x0bull);
    kb_linux_kernel_write_u64_field(vma, 0x28, 0);
    kb_linux_kernel_write_ptr_field(vma, 0x30, vma + 0x30);
    kb_linux_kernel_write_ptr_field(vma, 0x40, mm);
    kb_linux_kernel_write_u64_field(vma, 0x78, 0);
    kb_linux_kernel_write_u64_field(vma, 0x80, pgoff);
    kb_linux_kernel_write_ptr_field(vma, 0x88, NULL);
    kb_linux_kernel_write_ptr_field(vma, 0x90, NULL);
}

static int kb_low_or_err_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return ptr == NULL || value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static char *kb_copy_string(const char *value)
{
    if (kb_low_or_err_pointer(value)) {
        value = "(unnamed)";
    }
    const size_t len = strnlen(value, 4096);
    char *copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
}

static void *kb_err_ptr_noent(void)
{
    return (void *)(intptr_t)-2;
}
static int trace_kernel_objects_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_KERNEL_OBJECTS");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void *kb_read_ptr_field(const void *base, size_t offset)
{
    if (kb_low_or_err_pointer(base)) {
        return NULL;
    }
    void *value = NULL;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void kb_write_ptr_field(void *base, size_t offset, const void *value)
{
    if (!kb_low_or_err_pointer(base)) {
        memcpy((uint8_t *)base + offset, &value, sizeof(value));
    }
}

static void kb_write_u32_field(void *base, size_t offset, uint32_t value)
{
    if (!kb_low_or_err_pointer(base)) {
        memcpy((uint8_t *)base + offset, &value, sizeof(value));
    }
}

static kb_file_ops_view_t kb_decode_file_ops(const void *fops)
{
    kb_file_ops_view_t view;
    memset(&view, 0, sizeof(view));
    if (kb_low_or_err_pointer(fops)) {
        return view;
    }
    view.owner = kb_read_ptr_field(fops, 0);
    view.llseek = kb_read_ptr_field(fops, 8);
    view.read = kb_read_ptr_field(fops, 16);
    view.write = kb_read_ptr_field(fops, 24);
    view.read_iter = kb_read_ptr_field(fops, 32);
    view.write_iter = kb_read_ptr_field(fops, 40);
    view.poll = kb_read_ptr_field(fops, 64);
    view.unlocked_ioctl = kb_read_ptr_field(fops, 72);
    view.compat_ioctl = kb_read_ptr_field(fops, 80);
    view.mmap = kb_read_ptr_field(fops, 88);
    view.open = kb_read_ptr_field(fops, 104);
    view.release = kb_read_ptr_field(fops, 120);
    return view;
}

static kb_proc_ops_view_t kb_decode_proc_ops(const void *ops)
{
    kb_proc_ops_view_t view;
    memset(&view, 0, sizeof(view));
    if (kb_low_or_err_pointer(ops)) {
        return view;
    }
    view.open = kb_read_ptr_field(ops, 8);
    view.read = kb_read_ptr_field(ops, 16);
    view.read_iter = kb_read_ptr_field(ops, 24);
    view.write = kb_read_ptr_field(ops, 32);
    view.lseek = kb_read_ptr_field(ops, 40);
    view.release = kb_read_ptr_field(ops, 48);
    view.poll = kb_read_ptr_field(ops, 56);
    view.ioctl = kb_read_ptr_field(ops, 64);
    view.compat_ioctl = kb_read_ptr_field(ops, 72);
    view.mmap = kb_read_ptr_field(ops, 80);
    return view;
}

static const char *kb_proc_path_for_parent(void *parent)
{
    if (parent == NULL) {
        return "/proc";
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if ((void *)&kb_proc_records[i] == parent && kb_proc_records[i].path != NULL) {
            return kb_proc_records[i].path;
        }
    }
    return "/proc/?";
}

static char *kb_join_proc_path(const char *parent, const char *name)
{
    if (name == NULL) {
        name = "(null)";
    }
    if (parent == NULL || strcmp(parent, "/proc") == 0) {
        const size_t len = strlen("/proc/") + strlen(name);
        char *path = malloc(len + 1u);
        if (path != NULL) {
            snprintf(path, len + 1u, "/proc/%s", name);
        }
        return path;
    }
    const size_t len = strlen(parent) + 1u + strlen(name);
    char *path = malloc(len + 1u);
    if (path != NULL) {
        snprintf(path, len + 1u, "%s/%s", parent, name);
    }
    return path;
}

static void kb_print_kernel_object_summary(void)
{
    if (!trace_kernel_objects_enabled()) {
        return;
    }

    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_chrdev_records[i].name != NULL) {
            const kb_file_ops_view_t fops = kb_chrdev_records[i].fops_view;
            fprintf(
                stderr,
                "kobox-kobj-summary: chrdev active=%d major=%u baseminor=%u count=%u name=%s fops=%p\n",
                kb_chrdev_records[i].active,
                kb_chrdev_records[i].major,
                kb_chrdev_records[i].baseminor,
                kb_chrdev_records[i].count,
                kb_chrdev_records[i].name,
                kb_chrdev_records[i].fops);
            if (kb_chrdev_records[i].has_fops_view) {
                fprintf(
                    stderr,
                    "kobox-fops-summary: kind=chrdev name=%s fops=%p owner=%p llseek=%p read=%p write=%p read_iter=%p write_iter=%p poll=%p ioctl=%p compat_ioctl=%p mmap=%p open=%p release=%p\n",
                    kb_chrdev_records[i].name,
                    kb_chrdev_records[i].fops,
                    fops.owner,
                    fops.llseek,
                    fops.read,
                    fops.write,
                    fops.read_iter,
                    fops.write_iter,
                    fops.poll,
                    fops.unlocked_ioctl,
                    fops.compat_ioctl,
                    fops.mmap,
                    fops.open,
                    fops.release);
            }
        }
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_proc_records[i].path != NULL) {
            const kb_proc_ops_view_t ops = kb_proc_records[i].ops_view;
            fprintf(
                stderr,
                "kobox-kobj-summary: proc active=%d mode=%o path=%s ops=%p data=%p\n",
                kb_proc_records[i].active,
                kb_proc_records[i].mode,
                kb_proc_records[i].path,
                kb_proc_records[i].ops,
                kb_proc_records[i].data);
            if (kb_proc_records[i].has_ops_view) {
                fprintf(
                    stderr,
                    "kobox-fops-summary: kind=proc path=%s ops=%p open=%p read=%p read_iter=%p write=%p lseek=%p release=%p poll=%p ioctl=%p compat_ioctl=%p mmap=%p\n",
                    kb_proc_records[i].path,
                    kb_proc_records[i].ops,
                    ops.open,
                    ops.read,
                    ops.read_iter,
                    ops.write,
                    ops.lseek,
                    ops.release,
                    ops.poll,
                    ops.ioctl,
                    ops.compat_ioctl,
                    ops.mmap);
            }
        }
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_class_records[i].name != NULL) {
            fprintf(
                stderr,
                "kobox-kobj-summary: class active=%d name=%s\n",
                kb_class_records[i].active,
                kb_class_records[i].name);
        }
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_cdev_records[i].cdev != NULL) {
            const kb_file_ops_view_t fops = kb_cdev_records[i].fops_view;
            const char *name = kb_linux_kernel_chrdev_name_for_dev(kb_cdev_records[i].dev);
            fprintf(
                stderr,
                "kobox-kobj-summary: cdev active=%d major=%u minor=%u count=%u name=%s cdev=%p fops=%p\n",
                kb_cdev_records[i].active,
                kb_linux_kernel_decode_major(kb_cdev_records[i].dev),
                kb_linux_kernel_decode_minor(kb_cdev_records[i].dev),
                kb_cdev_records[i].count,
                name,
                kb_cdev_records[i].cdev,
                kb_cdev_records[i].fops);
            if (kb_cdev_records[i].has_fops_view) {
                fprintf(
                    stderr,
                    "kobox-fops-summary: kind=cdev name=%s major=%u minor=%u fops=%p owner=%p llseek=%p read=%p write=%p read_iter=%p write_iter=%p poll=%p ioctl=%p compat_ioctl=%p mmap=%p open=%p release=%p\n",
                    name,
                    kb_linux_kernel_decode_major(kb_cdev_records[i].dev),
                    kb_linux_kernel_decode_minor(kb_cdev_records[i].dev),
                    kb_cdev_records[i].fops,
                    fops.owner,
                    fops.llseek,
                    fops.read,
                    fops.write,
                    fops.read_iter,
                    fops.write_iter,
                    fops.poll,
                    fops.unlocked_ioctl,
                    fops.compat_ioctl,
                    fops.mmap,
                    fops.open,
                    fops.release);
            }
        }
    }
}

static void kb_register_kernel_object_summary(void)
{
    if (!kb_kernel_object_summary_registered) {
        atexit(kb_print_kernel_object_summary);
        kb_kernel_object_summary_registered = 1;
    }
}

static size_t kb_find_free_chrdev_slot(void)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_chrdev_records[i].name == NULL) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t kb_find_free_proc_slot(void)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_proc_records[i].path == NULL) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t kb_find_free_class_slot(void)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_class_records[i].name == NULL) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t kb_find_free_cdev_slot(void)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_cdev_records[i].cdev == NULL) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void kb_proc_deactivate_tree(const char *path)
{
    if (path == NULL) {
        return;
    }
    const size_t path_len = strlen(path);
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_proc_records[i].path == NULL) {
            continue;
        }
        if (strcmp(kb_proc_records[i].path, path) == 0 ||
            (strncmp(kb_proc_records[i].path, path, path_len) == 0 && kb_proc_records[i].path[path_len] == '/'))
        {
            kb_proc_records[i].active = 0;
        }
    }
}


int kb_linux_kernel_register_chrdev(unsigned major, unsigned baseminor, unsigned count, const char *name, void *fops)
{
    kb_register_kernel_object_summary();
    const unsigned assigned_major = major == 0 ? kb_next_dynamic_major++ : major;
    const size_t slot = kb_find_free_chrdev_slot();
    if (slot == SIZE_MAX) {
        return -12;
    }
    kb_chrdev_records[slot].active = 1;
    kb_chrdev_records[slot].owner_module = kb_loader_active_module();
    kb_chrdev_records[slot].major = assigned_major;
    kb_chrdev_records[slot].baseminor = baseminor;
    kb_chrdev_records[slot].count = count;
    kb_chrdev_records[slot].name = kb_copy_string(name == NULL ? "(unnamed)" : name);
    kb_chrdev_records[slot].fops = fops;
    if (fops != NULL) {
        kb_chrdev_records[slot].fops_view = kb_decode_file_ops(fops);
        kb_chrdev_records[slot].has_fops_view = 1;
    }
    if (kb_chrdev_records[slot].name == NULL) {
        memset(&kb_chrdev_records[slot], 0, sizeof(kb_chrdev_records[slot]));
        return -12;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(
            stderr,
            "kobox-kobj: register_chrdev major=%u baseminor=%u count=%u name=%s fops=%p\n",
            assigned_major,
            baseminor,
            count,
            kb_chrdev_records[slot].name,
            fops);
    }
    return major == 0 ? (int)assigned_major : 0;
}

int kb_linux_kernel_alloc_chrdev_region(uint32_t *dev, unsigned baseminor, unsigned count, const char *name)
{
    if (dev == NULL) {
        return -22;
    }
    kb_register_kernel_object_summary();
    const unsigned assigned_major = kb_next_dynamic_major++;
    const size_t slot = kb_find_free_chrdev_slot();
    if (slot == SIZE_MAX) {
        return -12;
    }
    *dev = kb_linux_kernel_encode_dev(assigned_major, baseminor);
    kb_chrdev_records[slot].active = 1;
    kb_chrdev_records[slot].owner_module = kb_loader_active_module();
    kb_chrdev_records[slot].major = assigned_major;
    kb_chrdev_records[slot].baseminor = baseminor;
    kb_chrdev_records[slot].count = count;
    kb_chrdev_records[slot].name = kb_copy_string(name == NULL ? "(unnamed)" : name);
    if (kb_chrdev_records[slot].name == NULL) {
        memset(&kb_chrdev_records[slot], 0, sizeof(kb_chrdev_records[slot]));
        return -12;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(
            stderr,
            "kobox-kobj: alloc_chrdev_region major=%u baseminor=%u count=%u name=%s dev=0x%x\n",
            assigned_major,
            baseminor,
            count,
            kb_chrdev_records[slot].name,
            *dev);
    }
    return 0;
}

void kb_linux_kernel_unregister_chrdev(unsigned major, unsigned baseminor, unsigned count, const char *name)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_chrdev_records[i].name != NULL && kb_chrdev_records[i].active &&
            kb_chrdev_records[i].major == major &&
            kb_chrdev_records[i].baseminor == baseminor)
        {
            kb_chrdev_records[i].active = 0;
            if (trace_kernel_objects_enabled()) {
                fprintf(
                    stderr,
                    "kobox-kobj: unregister_chrdev major=%u baseminor=%u count=%u name=%s\n",
                    major,
                    baseminor,
                    count,
                    name == NULL ? kb_chrdev_records[i].name : name);
            }
            return;
        }
    }
}

void kb_linux_kernel_unregister_chrdev_region(uint32_t dev, unsigned count)
{
    const unsigned major = kb_linux_kernel_decode_major(dev);
    const unsigned minor = kb_linux_kernel_decode_minor(dev);
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_chrdev_records[i].name != NULL && kb_chrdev_records[i].active &&
            kb_chrdev_records[i].major == major &&
            kb_chrdev_records[i].baseminor == minor)
        {
            kb_chrdev_records[i].active = 0;
            if (trace_kernel_objects_enabled()) {
                fprintf(
                    stderr,
                    "kobox-kobj: unregister_chrdev_region major=%u baseminor=%u count=%u name=%s\n",
                    major,
                    minor,
                    count,
                    kb_chrdev_records[i].name);
            }
            return;
        }
    }
}

void *kb_linux_kernel_proc_create_data(const char *name, unsigned mode, void *parent, void *ops, void *data)
{
    kb_register_kernel_object_summary();
    const size_t slot = kb_find_free_proc_slot();
    if (slot == SIZE_MAX) {
        return NULL;
    }
    const char *parent_path = kb_proc_path_for_parent(parent);
    kb_proc_records[slot].name = kb_copy_string(name == NULL ? "(null)" : name);
    kb_proc_records[slot].path = kb_join_proc_path(parent_path, name);
    if (kb_proc_records[slot].name == NULL || kb_proc_records[slot].path == NULL) {
        free(kb_proc_records[slot].name);
        free(kb_proc_records[slot].path);
        memset(&kb_proc_records[slot], 0, sizeof(kb_proc_records[slot]));
        return NULL;
    }
    kb_proc_records[slot].active = 1;
    kb_proc_records[slot].owner_module = kb_loader_active_module();
    kb_proc_records[slot].mode = mode;
    kb_proc_records[slot].parent = parent;
    kb_proc_records[slot].ops = ops;
    kb_proc_records[slot].data = data;
    if (ops != NULL) {
        kb_proc_records[slot].ops_view = kb_decode_proc_ops(ops);
        kb_proc_records[slot].has_ops_view = 1;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-kobj: proc_create mode=%o path=%s ops=%p data=%p\n", mode, kb_proc_records[slot].path, ops, data);
    }
    return &kb_proc_records[slot];
}

void *kb_linux_kernel_proc_mkdir_mode(const char *name, unsigned mode, void *parent)
{
    return kb_linux_kernel_proc_create_data(name, mode, parent, NULL, NULL);
}

void kb_linux_kernel_proc_remove(void *entry)
{
    if (entry == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if ((void *)&kb_proc_records[i] == entry && kb_proc_records[i].path != NULL) {
            kb_proc_deactivate_tree(kb_proc_records[i].path);
            if (trace_kernel_objects_enabled()) {
                fprintf(stderr, "kobox-kobj: proc_remove path=%s\n", kb_proc_records[i].path);
            }
            return;
        }
    }
}

void kb_linux_kernel_remove_proc_entry(const char *name, void *parent)
{
    const char *parent_path = kb_proc_path_for_parent(parent);
    char *path = kb_join_proc_path(parent_path, name);
    if (path == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_proc_records[i].path != NULL && strcmp(kb_proc_records[i].path, path) == 0) {
            kb_proc_deactivate_tree(kb_proc_records[i].path);
            if (trace_kernel_objects_enabled()) {
                fprintf(stderr, "kobox-kobj: remove_proc_entry path=%s\n", kb_proc_records[i].path);
            }
            free(path);
            return;
        }
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-kobj: remove_proc_entry path=%s missing\n", path);
    }
    free(path);
}

void *kb_linux_kernel_class_create(const char *name)
{
    kb_register_kernel_object_summary();
    const size_t slot = kb_find_free_class_slot();
    if (slot == SIZE_MAX) {
        return NULL;
    }
    kb_class_records[slot].active = 1;
    kb_class_records[slot].name = kb_copy_string(name == NULL ? "(unnamed)" : name);
    if (kb_class_records[slot].name == NULL) {
        memset(&kb_class_records[slot], 0, sizeof(kb_class_records[slot]));
        return NULL;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-kobj: class_create name=%s\n", kb_class_records[slot].name);
    }
    return &kb_class_records[slot];
}

void kb_linux_kernel_class_destroy(void *class_ptr)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if ((void *)&kb_class_records[i] == class_ptr && kb_class_records[i].name != NULL) {
            kb_class_records[i].active = 0;
            if (trace_kernel_objects_enabled()) {
                fprintf(stderr, "kobox-kobj: class_destroy name=%s\n", kb_class_records[i].name);
            }
            return;
        }
    }
}

void kb_linux_kernel_cdev_init(void *cdev, void *fops)
{
    if (cdev == NULL) {
        return;
    }
    kb_register_kernel_object_summary();
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_cdev_records[i].cdev == cdev) {
            slot = i;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = kb_find_free_cdev_slot();
    }
    if (slot == SIZE_MAX) {
        return;
    }
    kb_cdev_records[slot].cdev = cdev;
    kb_cdev_records[slot].owner_module = kb_loader_active_module();
    if (fops != NULL) {
        kb_cdev_records[slot].fops = fops;
        kb_cdev_records[slot].fops_view = kb_decode_file_ops(fops);
        kb_cdev_records[slot].has_fops_view = 1;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-kobj: cdev_init cdev=%p fops=%p\n", cdev, fops);
    }
}

int kb_linux_kernel_cdev_add(void *cdev, uint64_t dev, unsigned count)
{
    if (cdev == NULL) {
        return -22;
    }
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_cdev_records[i].cdev == cdev) {
            slot = i;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        slot = kb_find_free_cdev_slot();
    }
    if (slot == SIZE_MAX) {
        return -12;
    }
    kb_cdev_records[slot].cdev = cdev;
    kb_cdev_records[slot].active = 1;
    if (kb_cdev_records[slot].owner_module == NULL) {
        kb_cdev_records[slot].owner_module = kb_loader_active_module();
    }
    kb_cdev_records[slot].dev = dev;
    kb_cdev_records[slot].count = count;
    if (trace_kernel_objects_enabled()) {
        fprintf(
            stderr,
            "kobox-kobj: cdev_add major=%u minor=%u count=%u cdev=%p\n",
            kb_linux_kernel_decode_major(dev),
            kb_linux_kernel_decode_minor(dev),
            count,
            cdev);
    }
    return 0;
}

void kb_linux_kernel_cdev_del(void *cdev)
{
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_cdev_records[i].cdev == cdev) {
            kb_cdev_records[i].active = 0;
            if (trace_kernel_objects_enabled()) {
                fprintf(stderr, "kobox-kobj: cdev_del cdev=%p\n", cdev);
            }
            return;
        }
    }
}

static void kb_prepare_fake_file_layout(uint8_t *inode, uint8_t *mapping, uint8_t *file, const char *path)
{
    memset(inode, 0, KB_FAKE_INODE_SIZE);
    memset(mapping, 0, KB_FAKE_MAPPING_SIZE);
    memset(file, 0, KB_FAKE_FILE_SIZE);

    kb_write_ptr_field(inode, 0x40, mapping);
    kb_write_u32_field(inode, 0x5c, 0);

    kb_write_ptr_field(file, 0x20, inode);
    kb_write_ptr_field(file, 0xa8, inode);
    kb_write_ptr_field(file, 0xc8, NULL);
    kb_write_ptr_field(file, 0xd0, NULL);
    kb_write_ptr_field(file, 0xd8, NULL);
    if (path != NULL) {
        kb_write_ptr_field(file, 0xe0, path);
    }
}

static void *kb_alloc_fake_file(const char *path)
{
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (!kb_fake_file_records[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        return NULL;
    }

    uint8_t *inode = calloc(1, KB_FAKE_INODE_SIZE);
    uint8_t *mapping = calloc(1, KB_FAKE_MAPPING_SIZE);
    uint8_t *file = calloc(1, KB_FAKE_FILE_SIZE);
    char *path_copy = kb_copy_string(path == NULL ? "(anonymous)" : path);
    if (inode == NULL || mapping == NULL || file == NULL || path_copy == NULL) {
        free(inode);
        free(mapping);
        free(file);
        free(path_copy);
        return NULL;
    }

    kb_prepare_fake_file_layout(inode, mapping, file, path_copy);
    kb_fake_file_records[slot].active = 1;
    kb_fake_file_records[slot].inode = inode;
    kb_fake_file_records[slot].mapping = mapping;
    kb_fake_file_records[slot].file = file;
    kb_fake_file_records[slot].path = path_copy;
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: filp_alloc file=%p path=%s\n", (void *)file, path_copy);
    }
    return file;
}

static void kb_release_fake_file(void *file)
{
    if (file == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_KERNEL_OBJECT_MAX; i++) {
        if (kb_fake_file_records[i].active && kb_fake_file_records[i].file == file) {
            if (trace_kernel_objects_enabled()) {
                fprintf(stderr, "kobox-fd: filp_release file=%p path=%s\n", file, kb_fake_file_records[i].path);
            }
            free(kb_fake_file_records[i].inode);
            free(kb_fake_file_records[i].mapping);
            free(kb_fake_file_records[i].file);
            free(kb_fake_file_records[i].path);
            memset(&kb_fake_file_records[i], 0, sizeof(kb_fake_file_records[i]));
            return;
        }
    }
}

static kb_fd_record_t *kb_find_fd_record(unsigned fd)
{
    for (size_t i = 0; i < KB_FAKE_FD_MAX; i++) {
        if (kb_fd_records[i].active && kb_fd_records[i].fd == fd) {
            return &kb_fd_records[i];
        }
    }
    return NULL;
}

static kb_fd_record_t *kb_alloc_fd_record(unsigned fd)
{
    for (size_t i = 0; i < KB_FAKE_FD_MAX; i++) {
        if (!kb_fd_records[i].active) {
            kb_fd_records[i].active = 1;
            kb_fd_records[i].fd = fd;
            return &kb_fd_records[i];
        }
    }
    return NULL;
}

int kb_linux_kernel_get_unused_fd_flags(unsigned flags)
{
    (void)flags;
    for (unsigned step = 0; step < KB_FAKE_FD_MAX; step++) {
        const unsigned fd = kb_next_fake_fd++;
        if (kb_next_fake_fd >= KB_FAKE_FD_MAX + 3u) {
            kb_next_fake_fd = 3;
        }
        if (kb_find_fd_record(fd) == NULL) {
            if (kb_alloc_fd_record(fd) == NULL) {
                return -24;
            }
            if (trace_kernel_objects_enabled()) {
                fprintf(stderr, "kobox-fd: get_unused_fd fd=%u\n", fd);
            }
            return (int)fd;
        }
    }
    return -24;
}

void kb_linux_kernel_fd_install(unsigned fd, void *file)
{
    kb_fd_record_t *record = kb_find_fd_record(fd);
    if (record == NULL) {
        record = kb_alloc_fd_record(fd);
    }
    if (record == NULL) {
        return;
    }
    if (record->owned && record->file != file) {
        kb_release_fake_file(record->file);
    }
    free(record->path);
    record->file = file;
    record->owned = 0;
    record->path = NULL;
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: fd_install fd=%u file=%p\n", fd, file);
    }
}

void *kb_linux_kernel_fget(unsigned fd)
{
    kb_fd_record_t *record = kb_find_fd_record(fd);
    if (record == NULL || record->file == NULL) {
        if (trace_kernel_objects_enabled()) {
            fprintf(stderr, "kobox-fd: fget fd=%u result=NULL\n", fd);
        }
        return NULL;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: fget fd=%u file=%p\n", fd, record->file);
    }
    return record->file;
}

void kb_linux_kernel_fput(void *file)
{
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: fput file=%p\n", file);
    }
}

int kb_linux_kernel_close_fd(unsigned fd)
{
    kb_fd_record_t *record = kb_find_fd_record(fd);
    if (record == NULL) {
        return -9;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: close_fd fd=%u file=%p\n", fd, record->file);
    }
    if (record->owned) {
        kb_release_fake_file(record->file);
    }
    free(record->path);
    memset(record, 0, sizeof(*record));
    return 0;
}

void *kb_linux_kernel_filp_open(const char *path, int flags, unsigned mode)
{
    (void)flags;
    (void)mode;
    void *file = kb_alloc_fake_file(path);
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: filp_open path=%s file=%p\n", path == NULL ? "(null)" : path, file);
    }
    return file == NULL ? kb_err_ptr_noent() : file;
}

int kb_linux_kernel_filp_close(void *file, void *id)
{
    (void)id;
    kb_release_fake_file(file);
    return 0;
}

typedef int (*kb_linux_kernel_iterate_fd_fn)(const void *file, unsigned fd, void *data);

int kb_linux_kernel_iterate_fd(void *files, unsigned first, kb_linux_kernel_iterate_fd_fn fn, void *data)
{
    (void)files;
    if (fn == NULL) {
        return 0;
    }
    for (size_t i = 0; i < KB_FAKE_FD_MAX; i++) {
        if (kb_fd_records[i].active && kb_fd_records[i].file != NULL && kb_fd_records[i].fd >= first) {
            int result = fn(kb_fd_records[i].file, kb_fd_records[i].fd, data);
            if (result != 0) {
                return result;
            }
        }
    }
    return 0;
}

static kb_vma_record_t *kb_alloc_vma_record(void *mm, uint64_t start, uint64_t length)
{
    if (length == 0) {
        length = 4096;
    }
    for (size_t i = 0; i < KB_FAKE_VMA_MAX; i++) {
        if (!kb_vma_records[i].active) {
            uint8_t *vma = calloc(1, KB_FAKE_VMA_SIZE);
            uint8_t *page = calloc(1, KB_FAKE_PAGE_SIZE);
            if (vma == NULL || page == NULL) {
                free(vma);
                free(page);
                return NULL;
            }
            kb_vma_records[i].active = 1;
            kb_vma_records[i].mm = mm;
            kb_vma_records[i].start = start;
            kb_vma_records[i].end = start + length;
            kb_vma_records[i].pgoff = start >> 12;
            kb_vma_records[i].vma = vma;
            kb_vma_records[i].page = page;
            kb_linux_kernel_prepare_fake_vma(vma, mm, kb_vma_records[i].start, kb_vma_records[i].end, kb_vma_records[i].pgoff);
            return &kb_vma_records[i];
        }
    }
    return NULL;
}

static kb_vma_record_t *kb_linux_kernel_find_vma_record(void *mm, uint64_t addr)
{
    for (size_t i = 0; i < KB_FAKE_VMA_MAX; i++) {
        if (!kb_vma_records[i].active) {
            continue;
        }
        if ((mm == NULL || kb_vma_records[i].mm == NULL || kb_vma_records[i].mm == mm) &&
            addr >= kb_vma_records[i].start &&
            addr < kb_vma_records[i].end)
        {
            return &kb_vma_records[i];
        }
    }
    const uint64_t start = addr & ~0xfffull;
    return kb_alloc_vma_record(mm, start, 4096);
}

void *kb_linux_kernel_find_vma(void *mm, unsigned long addr)
{
    kb_vma_record_t *record = kb_linux_kernel_find_vma_record(mm, addr);
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: find_vma mm=%p addr=0x%lx vma=%p\n", mm, addr, record == NULL ? NULL : (void *)record->vma);
    }
    return record == NULL ? NULL : record->vma;
}

void *kb_linux_kernel_find_vma_intersection(void *mm, unsigned long start, unsigned long end)
{
    (void)end;
    return kb_linux_kernel_find_vma(mm, start);
}

int kb_linux_kernel_follow_pfn(void *vma, unsigned long address, unsigned long *pfn)
{
    (void)vma;
    if (pfn != NULL) {
        *pfn = address >> 12;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: follow_pfn vma=%p addr=0x%lx pfn=0x%lx\n", vma, address, pfn == NULL ? 0ul : *pfn);
    }
    return 0;
}

static long kb_linux_kernel_pin_user_pages_common(void *mm, unsigned long start, unsigned long nr_pages, void **pages)
{
    if (nr_pages == 0) {
        return 0;
    }
    for (unsigned long i = 0; i < nr_pages; i++) {
        kb_vma_record_t *record = kb_linux_kernel_find_vma_record(mm, start + (i << 12));
        if (pages != NULL) {
            pages[i] = record == NULL ? NULL : record->page;
        }
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: pin_user_pages mm=%p start=0x%lx nr=%lu\n", mm, start, nr_pages);
    }
    return (long)nr_pages;
}

long kb_linux_kernel_pin_user_pages(unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *vmas)
{
    (void)gup_flags;
    (void)vmas;
    return kb_linux_kernel_pin_user_pages_common(NULL, start, nr_pages, pages);
}

long kb_linux_kernel_pin_user_pages_remote(void *mm, unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *locked)
{
    (void)gup_flags;
    if (locked != NULL) {
        *(int *)locked = 0;
    }
    return kb_linux_kernel_pin_user_pages_common(mm, start, nr_pages, pages);
}

long kb_linux_kernel_get_user_pages_remote(void *mm, unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *vmas, void *locked)
{
    (void)gup_flags;
    (void)vmas;
    if (locked != NULL) {
        *(int *)locked = 0;
    }
    return kb_linux_kernel_pin_user_pages_common(mm, start, nr_pages, pages);
}

void kb_linux_kernel_unpin_user_page(void *page)
{
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: unpin_user_page page=%p\n", page);
    }
}

int kb_linux_kernel_remap_pfn_range(void *vma, unsigned long addr, unsigned long pfn, unsigned long size, uint64_t prot)
{
    (void)prot;
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: remap_pfn_range vma=%p addr=0x%lx pfn=0x%lx size=0x%lx\n", vma, addr, pfn, size);
    }
    return 0;
}

int kb_linux_kernel_vmf_insert_pfn(void *vma, unsigned long addr, unsigned long pfn)
{
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: vmf_insert_pfn vma=%p addr=0x%lx pfn=0x%lx\n", vma, addr, pfn);
    }
    return 0;
}
