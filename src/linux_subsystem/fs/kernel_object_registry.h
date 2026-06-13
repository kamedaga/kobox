#pragma once

#include "kobox/module.h"

#include <stdint.h>

enum {
    KB_KERNEL_OBJECT_MAX = 256,
    KB_FAKE_FD_MAX = 256,
    KB_FAKE_VMA_MAX = 256,
    KB_FAKE_FILE_SIZE = 512,
    KB_FAKE_INODE_SIZE = 512,
    KB_FAKE_MAPPING_SIZE = 512,
    KB_FAKE_VMA_SIZE = 512,
    KB_FAKE_PAGE_SIZE = 128,
};

typedef struct kb_file_ops_view {
    void *owner;
    void *llseek;
    void *read;
    void *write;
    void *read_iter;
    void *write_iter;
    void *poll;
    void *unlocked_ioctl;
    void *compat_ioctl;
    void *mmap;
    void *open;
    void *release;
} kb_file_ops_view_t;

typedef struct kb_proc_ops_view {
    void *open;
    void *read;
    void *read_iter;
    void *write;
    void *lseek;
    void *release;
    void *poll;
    void *ioctl;
    void *compat_ioctl;
    void *mmap;
} kb_proc_ops_view_t;

typedef struct kb_chrdev_record {
    int active;
    kb_module_t *owner_module;
    unsigned major;
    unsigned baseminor;
    unsigned count;
    char *name;
    void *fops;
    kb_file_ops_view_t fops_view;
    int has_fops_view;
} kb_chrdev_record_t;

typedef struct kb_proc_record {
    int active;
    kb_module_t *owner_module;
    char *name;
    char *path;
    unsigned mode;
    void *parent;
    void *ops;
    void *data;
    kb_proc_ops_view_t ops_view;
    int has_ops_view;
} kb_proc_record_t;

typedef struct kb_class_record {
    int active;
    char *name;
} kb_class_record_t;

typedef struct kb_cdev_record {
    int active;
    kb_module_t *owner_module;
    void *cdev;
    uint64_t dev;
    unsigned count;
    void *fops;
    kb_file_ops_view_t fops_view;
    int has_fops_view;
} kb_cdev_record_t;

typedef struct kb_fd_record {
    int active;
    unsigned fd;
    void *file;
    int owned;
    char *path;
} kb_fd_record_t;

typedef struct kb_fake_file_record {
    int active;
    uint8_t *inode;
    uint8_t *mapping;
    uint8_t *file;
    char *path;
} kb_fake_file_record_t;

typedef struct kb_vma_record {
    int active;
    void *mm;
    uint64_t start;
    uint64_t end;
    uint64_t pgoff;
    uint8_t *vma;
    uint8_t *page;
} kb_vma_record_t;

extern kb_chrdev_record_t kb_chrdev_records[KB_KERNEL_OBJECT_MAX];
extern kb_proc_record_t kb_proc_records[KB_KERNEL_OBJECT_MAX];
extern kb_class_record_t kb_class_records[KB_KERNEL_OBJECT_MAX];
extern kb_cdev_record_t kb_cdev_records[KB_KERNEL_OBJECT_MAX];
extern kb_fd_record_t kb_fd_records[KB_FAKE_FD_MAX];
extern kb_fake_file_record_t kb_fake_file_records[KB_KERNEL_OBJECT_MAX];
extern kb_vma_record_t kb_vma_records[KB_FAKE_VMA_MAX];
extern int kb_kernel_object_summary_registered;
extern unsigned kb_next_dynamic_major;
extern unsigned kb_next_fake_fd;

uint32_t kb_linux_kernel_decode_major(uint64_t dev);
uint32_t kb_linux_kernel_decode_minor(uint64_t dev);
uint32_t kb_linux_kernel_encode_dev(unsigned major, unsigned minor);
const char *kb_linux_kernel_chrdev_name_for_dev(uint64_t dev);
void kb_linux_kernel_prepare_fake_vma(uint8_t *vma, void *mm, uint64_t start, uint64_t end, uint64_t pgoff);

int kb_linux_kernel_register_chrdev(unsigned major, unsigned baseminor, unsigned count, const char *name, void *fops);
int kb_linux_kernel_alloc_chrdev_region(uint32_t *dev, unsigned baseminor, unsigned count, const char *name);
void kb_linux_kernel_unregister_chrdev(unsigned major, unsigned baseminor, unsigned count, const char *name);
void kb_linux_kernel_unregister_chrdev_region(uint32_t dev, unsigned count);
void *kb_linux_kernel_proc_create_data(const char *name, unsigned mode, void *parent, void *ops, void *data);
void *kb_linux_kernel_proc_mkdir_mode(const char *name, unsigned mode, void *parent);
void kb_linux_kernel_proc_remove(void *entry);
void kb_linux_kernel_remove_proc_entry(const char *name, void *parent);
void *kb_linux_kernel_class_create(const char *name);
void kb_linux_kernel_class_destroy(void *class_ptr);
void kb_linux_kernel_cdev_init(void *cdev, void *fops);
int kb_linux_kernel_cdev_add(void *cdev, uint64_t dev, unsigned count);
void kb_linux_kernel_cdev_del(void *cdev);
int kb_linux_kernel_get_unused_fd_flags(unsigned flags);
void kb_linux_kernel_fd_install(unsigned fd, void *file);
void *kb_linux_kernel_fget(unsigned fd);
void kb_linux_kernel_fput(void *file);
int kb_linux_kernel_close_fd(unsigned fd);
void *kb_linux_kernel_filp_open(const char *path, int flags, unsigned mode);
int kb_linux_kernel_filp_close(void *file, void *id);
int kb_linux_kernel_iterate_fd(void *files, unsigned first, int (*fn)(const void *file, unsigned fd, void *data), void *data);
void *kb_linux_kernel_find_vma(void *mm, unsigned long addr);
void *kb_linux_kernel_find_vma_intersection(void *mm, unsigned long start, unsigned long end);
int kb_linux_kernel_follow_pfn(void *vma, unsigned long address, unsigned long *pfn);
long kb_linux_kernel_pin_user_pages(unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *vmas);
long kb_linux_kernel_pin_user_pages_remote(
    void *mm,
    unsigned long start,
    unsigned long nr_pages,
    unsigned int gup_flags,
    void **pages,
    void *locked);
long kb_linux_kernel_get_user_pages_remote(
    void *mm,
    unsigned long start,
    unsigned long nr_pages,
    unsigned int gup_flags,
    void **pages,
    void *vmas,
    void *locked);
void kb_linux_kernel_unpin_user_page(void *page);
int kb_linux_kernel_remap_pfn_range(void *vma, unsigned long addr, unsigned long pfn, unsigned long size, uint64_t prot);
int kb_linux_kernel_vmf_insert_pfn(void *vma, unsigned long addr, unsigned long pfn);
