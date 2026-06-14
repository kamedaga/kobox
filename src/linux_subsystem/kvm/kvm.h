#pragma once

#include <stddef.h>

typedef struct kb_kvm_misc_snapshot {
    void *misc_device;
    const char *name;
    void *fops;
    void *unlocked_ioctl;
    int registered;
} kb_kvm_misc_snapshot_t;

typedef struct kb_kvm_fd_snapshot {
    int fd;
    const char *name;
    void *file;
    void *private_data;
    void *fops;
    void *unlocked_ioctl;
    int active;
} kb_kvm_fd_snapshot_t;

typedef struct kb_kvm_run_snapshot {
    void *run;
    unsigned int exit_reason;
    unsigned char ready_for_interrupt_injection;
    unsigned char if_flag;
    unsigned short flags;
    unsigned long long cr8;
    unsigned long long apic_base;
    unsigned char io_direction;
    unsigned char io_size;
    unsigned short io_port;
    unsigned int io_count;
    unsigned long long io_data_offset;
    unsigned char io_data[8];
    unsigned long long mmio_phys_addr;
    unsigned char mmio_data[8];
    unsigned int mmio_len;
    unsigned char mmio_is_write;
} kb_kvm_run_snapshot_t;

int kb_linux_kvm_misc_snapshot(const char *name, kb_kvm_misc_snapshot_t *out_snapshot);
int kb_linux_kvm_fd_snapshot(int fd, kb_kvm_fd_snapshot_t *out_snapshot);
int kb_linux_kvm_run_snapshot(int vcpu_fd, kb_kvm_run_snapshot_t *out_snapshot);
