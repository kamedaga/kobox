#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "kobox/device_linux_mock.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/kvm/kvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <ucontext.h>
#endif
#endif

#if defined(_WIN32)
struct kvm_regs {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rsp, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
};

struct kvm_segment {
    uint64_t base;
    uint32_t limit;
    uint16_t selector;
    uint8_t type;
    uint8_t present, dpl, db, s, l, g, avl;
    uint8_t unusable;
    uint8_t padding;
};

struct kvm_dtable {
    uint64_t base;
    uint16_t limit;
    uint16_t padding[3];
};

struct kvm_sregs {
    struct kvm_segment cs, ds, es, fs, gs, ss;
    struct kvm_segment tr, ldt;
    struct kvm_dtable gdt, idt;
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer;
    uint64_t apic_base;
    uint64_t interrupt_bitmap[4];
};
#endif

enum {
    KB_KVM_GET_API_VERSION = 0xae00,
    KB_KVM_CREATE_VM = 0xae01,
    KB_KVM_CHECK_EXTENSION = 0xae03,
    KB_KVM_CREATE_VCPU = 0xae41,
    KB_KVM_CREATE_IRQCHIP = 0xae60,
    KB_KVM_IRQ_LINE = 0x4008ae61,
    KB_KVM_SET_USER_MEMORY_REGION = 0x4020ae46,
    KB_KVM_RUN = 0xae80,
    KB_KVM_EXIT_IO = 2,
    KB_KVM_EXIT_HLT = 5,
    KB_KVM_EXIT_MMIO = 6,
    KB_KVM_RUN_MMIO_DATA_OFFSET = 0x28,
    KB_KVM_RUN_STORAGE_BYTES = 16384,
    KB_KVM_FAKE_FILE_BYTES = 512,
};

typedef struct kb_kvm_irq_level {
    uint32_t irq;
    uint32_t level;
} kb_kvm_irq_level_t;

static const unsigned int KB_KVM_GET_REGS = 0x8090ae81u;
static const unsigned int KB_KVM_SET_REGS = 0x4090ae82u;
static const unsigned int KB_KVM_GET_SREGS = 0x8138ae83u;
static const unsigned int KB_KVM_SET_SREGS = 0x4138ae84u;
static const unsigned int KB_KVM_SET_CPUID2 = 0x4008ae90u;

typedef struct kb_kvm_userspace_memory_region {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
} kb_kvm_userspace_memory_region_t;

typedef struct loaded_module {
    const char *path;
    void *data;
    size_t size;
    kb_module_t *module;
    int initialized;
} loaded_module_t;

typedef struct guest_region_backing {
    const char *name;
    uint32_t slot;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    void *data;
} guest_region_backing_t;

typedef struct kvm_block_fixture {
    void *queue;
    void *disk;
    void *part0;
    unsigned char *image;
    size_t image_size;
    const char *image_path;
    uint8_t last_command;
    uint32_t virtio_status;
    uint32_t virtio_device_features_sel;
    uint32_t virtio_driver_features_sel;
    uint64_t virtio_driver_features;
    uint32_t virtio_queue_sel;
    uint32_t virtio_queue_num;
    uint32_t virtio_queue_ready;
    uint64_t virtio_queue_desc;
    uint64_t virtio_queue_driver;
    uint64_t virtio_queue_device;
    uint16_t virtio_last_avail_idx;
    uint32_t virtio_interrupt_status;
} kvm_block_fixture_t;

typedef struct kvm_guest_memory_record {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    unsigned char *userspace_addr;
    int active;
} kvm_guest_memory_record_t;

static kvm_guest_memory_record_t kvm_guest_memory_records[16];

#if !defined(_WIN32) && defined(__x86_64__)
static void signal_diagnostics_handler(int signal_number, siginfo_t *info, void *uctx)
{
    ucontext_t *context = (ucontext_t *)uctx;
    void *rip = (void *)context->uc_mcontext.gregs[REG_RIP];
    void *rsp = (void *)context->uc_mcontext.gregs[REG_RSP];
    void *rdi = (void *)context->uc_mcontext.gregs[REG_RDI];
    void *rsi = (void *)context->uc_mcontext.gregs[REG_RSI];
    void *rdx = (void *)context->uc_mcontext.gregs[REG_RDX];
    void *rcx = (void *)context->uc_mcontext.gregs[REG_RCX];
    const uint8_t *insn = (const uint8_t *)rip;
    fprintf(stderr,
        "kobox-kvm-real-ops: signal=%d rip=%p rsp=%p fault=%p external_target=%p caller_gs=0x%lx callee_gs=0x%lx rdi=%p rsi=%p rdx=%p rcx=%p\n",
        signal_number,
        rip,
        rsp,
        info == NULL ? NULL : info->si_addr,
        kb_module_current_external_call_target(),
        kb_module_current_external_call_caller_gs(),
        kb_module_current_external_call_callee_gs(),
        rdi,
        rsi,
        rdx,
        rcx);
    kb_module_debug_describe_address(rip);
    {
        const uintptr_t *stack = (const uintptr_t *)rsp;
        for (size_t i = 0; stack != NULL && i < 4; i++) {
            uintptr_t value = stack[i];
            fprintf(stderr, "kobox-kvm-real-ops: stack[%zu]=%p\n", i, (void *)value);
            kb_module_debug_describe_address((const void *)value);
        }
    }
    if (insn != NULL && insn[0] == 0xff && insn[1] == 0x15) {
        int32_t displacement = 0;
        uintptr_t slot = 0;
        uintptr_t target = 0;
        memcpy(&displacement, insn + 2, sizeof(displacement));
        slot = (uintptr_t)(insn + 6) + (intptr_t)displacement;
        memcpy(&target, (const void *)slot, sizeof(target));
        fprintf(stderr,
            "kobox-kvm-real-ops: indirect_call slot=%p target=%p displacement=%d\n",
            (void *)slot,
            (void *)target,
            displacement);
        kb_module_debug_describe_address((const void *)slot);
        kb_module_debug_describe_address((const void *)target);
    }
    {
        Dl_info rip_info;
        Dl_info target_info;
        void *external_target = kb_module_current_external_call_target();
        if (dladdr(rip, &rip_info) != 0) {
            fprintf(stderr,
                "kobox-kvm-real-ops: rip_symbol=%s object=%s base=%p\n",
                rip_info.dli_sname == NULL ? "(unknown)" : rip_info.dli_sname,
                rip_info.dli_fname == NULL ? "(unknown)" : rip_info.dli_fname,
                rip_info.dli_fbase);
        }
        if (external_target != NULL && dladdr(external_target, &target_info) != 0) {
            fprintf(stderr,
                "kobox-kvm-real-ops: external_symbol=%s object=%s base=%p offset=0x%lx\n",
                target_info.dli_sname == NULL ? "(unknown)" : target_info.dli_sname,
                target_info.dli_fname == NULL ? "(unknown)" : target_info.dli_fname,
                target_info.dli_fbase,
                (unsigned long)((uintptr_t)external_target - (uintptr_t)target_info.dli_fbase));
        }
    }
    _Exit(128 + signal_number);
}

static void install_signal_diagnostics(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_diagnostics_handler;
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

static kb_status_t load_module(kb_device_backend_t *backend, const char *path, loaded_module_t *out_module)
{
    memset(out_module, 0, sizeof(*out_module));
    out_module->path = path;
    kb_status_t status = read_file(path, &out_module->data, &out_module->size);
    if (status != KB_OK) {
        return status;
    }
    kb_module_image_t image = {
        .data = out_module->data,
        .size = out_module->size,
        .name = path,
    };
    status = kb_module_open_image(&image, backend, &out_module->module);
    if (status != KB_OK) {
        free(out_module->data);
        memset(out_module, 0, sizeof(*out_module));
    }
    return status;
}

static void unload_module(loaded_module_t *module)
{
    if (module == NULL) {
        return;
    }
    if (module->module != NULL) {
        kb_module_close(module->module);
    }
    free(module->data);
    memset(module, 0, sizeof(*module));
}

static long call_kvm_ioctl(void *ioctl_fn, void *file, unsigned int cmd, unsigned long arg)
{
    long (*fn)(void *, unsigned int, unsigned long) = NULL;
    memcpy(&fn, &ioctl_fn, sizeof(fn));
    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ioctl_fn);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long result = fn(file, cmd, arg);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static void kvm_guest_memory_record_region(uint32_t slot, uint64_t guest_phys_addr, uint64_t memory_size, void *userspace_addr)
{
    if (slot >= sizeof(kvm_guest_memory_records) / sizeof(kvm_guest_memory_records[0])) {
        return;
    }
    kvm_guest_memory_records[slot].guest_phys_addr = guest_phys_addr;
    kvm_guest_memory_records[slot].memory_size = memory_size;
    kvm_guest_memory_records[slot].userspace_addr = userspace_addr;
    kvm_guest_memory_records[slot].active = memory_size != 0 && userspace_addr != NULL;
}

static unsigned char *kvm_guest_translate(uint64_t gpa, size_t size)
{
    for (size_t i = 0; i < sizeof(kvm_guest_memory_records) / sizeof(kvm_guest_memory_records[0]); i++) {
        kvm_guest_memory_record_t *record = &kvm_guest_memory_records[i];
        if (!record->active || gpa < record->guest_phys_addr) {
            continue;
        }
        uint64_t offset = gpa - record->guest_phys_addr;
        if (offset <= record->memory_size && size <= record->memory_size - offset) {
            return record->userspace_addr + offset;
        }
    }
    return NULL;
}

static int kvm_guest_read(uint64_t gpa, void *buffer, size_t size)
{
    unsigned char *src = kvm_guest_translate(gpa, size);
    if (src == NULL || buffer == NULL) {
        return -22;
    }
    memcpy(buffer, src, size);
    return 0;
}

static int kvm_guest_write(uint64_t gpa, const void *buffer, size_t size)
{
    unsigned char *dst = kvm_guest_translate(gpa, size);
    if (dst == NULL || buffer == NULL) {
        return -22;
    }
    memcpy(dst, buffer, size);
    return 0;
}

static int linux_kvm_backend_enabled(void)
{
    const char *backend = getenv("KOBOX_KVM_RUN_BACKEND");
    return backend != NULL && strcmp(backend, "linux-kvm") == 0;
}

static int kvm_block_fixture_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    kvm_block_fixture_t *fixture = (kvm_block_fixture_t *)ctx;
    uint64_t offset = 0;
    if (fixture == NULL || buffer == NULL || byte_count == 0) {
        return -22;
    }
    if (__builtin_mul_overflow(sector, 512ull, &offset) ||
        offset > fixture->image_size ||
        byte_count > fixture->image_size - (size_t)offset)
    {
        return -34;
    }
    memcpy(buffer, fixture->image + (size_t)offset, byte_count);
    return 0;
}

static int kvm_block_fixture_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    kvm_block_fixture_t *fixture = (kvm_block_fixture_t *)ctx;
    uint64_t offset = 0;
    if (fixture == NULL || buffer == NULL || byte_count == 0) {
        return -22;
    }
    if (__builtin_mul_overflow(sector, 512ull, &offset) ||
        offset > fixture->image_size ||
        byte_count > fixture->image_size - (size_t)offset)
    {
        return -34;
    }
    memcpy(fixture->image + (size_t)offset, buffer, byte_count);
    return 0;
}

static int kvm_block_fixture_load_image(kvm_block_fixture_t *fixture, const char *image_path)
{
    if (fixture == NULL) {
        return 1;
    }
    if (image_path != NULL && image_path[0] != '\0') {
        void *data = NULL;
        size_t data_size = 0;
        if (read_file(image_path, &data, &data_size) != KB_OK) {
            fprintf(stderr, "kvm-block-route: failed to read image path=%s\n", image_path);
            return 1;
        }
        if (data_size < 1024 || (data_size % 512u) != 0) {
            fprintf(stderr, "kvm-block-route: image must be 512-byte aligned and at least 1024 bytes path=%s size=%zu\n",
                image_path,
                data_size);
            free(data);
            return 1;
        }
        fixture->image = (unsigned char *)data;
        fixture->image_size = data_size;
        fixture->image_path = image_path;
        return 0;
    }

    fixture->image_size = 4096;
    fixture->image = (unsigned char *)calloc(1, fixture->image_size);
    if (fixture->image == NULL) {
        return 1;
    }
    fixture->image[0] = 'B';
    fixture->image[1] = 'L';
    fixture->image[2] = 'K';
    fixture->image[3] = '\n';
    return 0;
}

static int kvm_block_fixture_init(kvm_block_fixture_t *fixture, const char *image_path)
{
    memset(fixture, 0, sizeof(*fixture));
    if (kvm_block_fixture_load_image(fixture, image_path) != 0) {
        return 1;
    }

    fixture->queue = kb_block_subsystem_queue_alloc(NULL);
    fixture->disk = kb_block_subsystem_disk_alloc();
    fixture->part0 = kb_block_subsystem_block_device_alloc();
    if (fixture->queue == NULL ||
        fixture->disk == NULL ||
        fixture->part0 == NULL ||
        kb_block_subsystem_disk_attach(fixture->disk, fixture->queue, fixture->part0) != 0)
    {
        fprintf(stderr, "kvm-block-route: failed to create kobox block disk\n");
        return 1;
    }
    kb_block_subsystem_disk_set_capacity(fixture->disk, fixture->image_size / 512u);
    kb_block_subsystem_disk_set_io(
        fixture->disk,
        fixture,
        kvm_block_fixture_read,
        kvm_block_fixture_write);
    if (kb_block_subsystem_disk_register(NULL, fixture->disk, NULL) != 0) {
        fprintf(stderr, "kvm-block-route: failed to register kobox block disk\n");
        return 1;
    }
    printf("kvm-block-route: disk=%p sectors=%zu bytes=%zu image=%s\n",
        fixture->disk,
        fixture->image_size / 512u,
        fixture->image_size,
        fixture->image_path == NULL ? "(synthetic)" : fixture->image_path);
    return 0;
}

static void kvm_block_fixture_destroy(kvm_block_fixture_t *fixture)
{
    if (fixture == NULL) {
        return;
    }
    if (fixture->disk != NULL) {
        kb_block_subsystem_disk_unregister(fixture->disk);
        kb_block_subsystem_disk_put(fixture->disk);
    }
    kb_block_subsystem_object_free(fixture->part0);
    kb_block_subsystem_object_free(fixture->disk);
    kb_block_subsystem_object_free(fixture->queue);
    free(fixture->image);
    memset(fixture, 0, sizeof(*fixture));
}

static void write_mmio_response(void *run, uint32_t value, uint32_t len)
{
    unsigned char *data = (unsigned char *)run + KB_KVM_RUN_MMIO_DATA_OFFSET;
    memset(data, 0, 8);
    for (uint32_t i = 0; i < len && i < 4; i++) {
        data[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }
}

static uint32_t read_mmio_request_value(const kb_kvm_run_snapshot_t *run_snapshot)
{
    uint32_t value = 0;
    uint32_t len = run_snapshot->mmio_len;
    if (len > 4) {
        len = 4;
    }
    for (uint32_t i = 0; i < len; i++) {
        value |= (uint32_t)run_snapshot->mmio_data[i] << (i * 8);
    }
    return value;
}

static uint16_t kvm_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t kvm_le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t kvm_le64(const unsigned char *p)
{
    return (uint64_t)kvm_le32(p) | ((uint64_t)kvm_le32(p + 4) << 32);
}

static void kvm_store16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)(value >> 8);
}

static void kvm_store32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

typedef struct kvm_virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} kvm_virtq_desc_t;

static int kvm_virtq_read_desc(uint64_t desc_table, uint16_t index, kvm_virtq_desc_t *out_desc)
{
    unsigned char raw[16];
    if (out_desc == NULL || kvm_guest_read(desc_table + (uint64_t)index * sizeof(raw), raw, sizeof(raw)) != 0) {
        return -22;
    }
    out_desc->addr = kvm_le64(raw);
    out_desc->len = kvm_le32(raw + 8);
    out_desc->flags = kvm_le16(raw + 12);
    out_desc->next = kvm_le16(raw + 14);
    return 0;
}

static int kvm_virtio_blk_copy_from_disk(kvm_block_fixture_t *block, uint64_t sector, uint64_t guest_addr, uint32_t len)
{
    unsigned char *buffer = (unsigned char *)malloc(len);
    if (buffer == NULL) {
        return -12;
    }
    int status = kb_block_subsystem_disk_read(block->disk, sector, buffer, len);
    if (status == 0) {
        status = kvm_guest_write(guest_addr, buffer, len);
    }
    free(buffer);
    return status;
}

static int kvm_virtio_blk_copy_to_disk(kvm_block_fixture_t *block, uint64_t sector, uint64_t guest_addr, uint32_t len)
{
    unsigned char *buffer = (unsigned char *)malloc(len);
    if (buffer == NULL) {
        return -12;
    }
    int status = kvm_guest_read(guest_addr, buffer, len);
    if (status == 0) {
        status = kb_block_subsystem_disk_write(block->disk, sector, buffer, len);
    }
    free(buffer);
    return status;
}

static int kvm_virtio_process_queue(kvm_block_fixture_t *block)
{
    enum {
        VRING_DESC_F_NEXT = 1,
        VRING_DESC_F_WRITE = 2,
        VIRTIO_BLK_T_IN = 0,
        VIRTIO_BLK_T_OUT = 1,
        VIRTIO_BLK_T_FLUSH = 4,
    };
    if (block == NULL ||
        block->virtio_queue_num == 0 ||
        block->virtio_queue_desc == 0 ||
        block->virtio_queue_driver == 0 ||
        block->virtio_queue_device == 0)
    {
        return 0;
    }

    unsigned char idx_raw[2];
    if (kvm_guest_read(block->virtio_queue_driver + 2, idx_raw, sizeof(idx_raw)) != 0) {
        return -22;
    }
    uint16_t avail_idx = kvm_le16(idx_raw);
    while (block->virtio_last_avail_idx != avail_idx) {
        uint16_t ring_pos = (uint16_t)(block->virtio_last_avail_idx % block->virtio_queue_num);
        unsigned char head_raw[2];
        if (kvm_guest_read(block->virtio_queue_driver + 4 + (uint64_t)ring_pos * 2, head_raw, sizeof(head_raw)) != 0) {
            return -22;
        }
        uint16_t head = kvm_le16(head_raw);
        kvm_virtq_desc_t desc;
        if (head >= block->virtio_queue_num ||
            kvm_virtq_read_desc(block->virtio_queue_desc, head, &desc) != 0 ||
            desc.len < 16)
        {
            return -22;
        }

        unsigned char header[16];
        if (kvm_guest_read(desc.addr, header, sizeof(header)) != 0) {
            return -22;
        }
        uint32_t type = kvm_le32(header);
        uint64_t sector = kvm_le64(header + 8);
        uint32_t bytes = 0;
        uint8_t request_status = 0;
        uint64_t data_sector = sector;
        uint16_t current = desc.next;
        uint64_t status_addr = 0;
        uint32_t chain_guard = 0;
        while ((desc.flags & VRING_DESC_F_NEXT) != 0 && chain_guard++ < block->virtio_queue_num) {
            if (current >= block->virtio_queue_num ||
                kvm_virtq_read_desc(block->virtio_queue_desc, current, &desc) != 0)
            {
                return -22;
            }
            int is_status_desc = (desc.flags & VRING_DESC_F_WRITE) != 0 && desc.len == 1;
            if (is_status_desc) {
                status_addr = desc.addr;
            } else if (type == VIRTIO_BLK_T_IN && (desc.flags & VRING_DESC_F_WRITE) != 0) {
                int status = kvm_virtio_blk_copy_from_disk(block, data_sector, desc.addr, desc.len);
                if (status != 0) {
                    request_status = 1;
                } else {
                    bytes += desc.len;
                    data_sector += desc.len / 512u;
                }
            } else if (type == VIRTIO_BLK_T_OUT && (desc.flags & VRING_DESC_F_WRITE) == 0) {
                int status = kvm_virtio_blk_copy_to_disk(block, data_sector, desc.addr, desc.len);
                if (status != 0) {
                    request_status = 1;
                } else {
                    bytes += desc.len;
                    data_sector += desc.len / 512u;
                }
            } else if (type == VIRTIO_BLK_T_FLUSH) {
                request_status = 0;
            } else {
                request_status = 2;
            }
            current = desc.next;
        }
        if (status_addr != 0) {
            (void)kvm_guest_write(status_addr, &request_status, sizeof(request_status));
        }

        unsigned char used_idx_raw[2];
        if (kvm_guest_read(block->virtio_queue_device + 2, used_idx_raw, sizeof(used_idx_raw)) != 0) {
            return -22;
        }
        uint16_t used_idx = kvm_le16(used_idx_raw);
        uint16_t used_pos = (uint16_t)(used_idx % block->virtio_queue_num);
        unsigned char used_elem[8];
        kvm_store32(used_elem, head);
        kvm_store32(used_elem + 4, bytes);
        if (kvm_guest_write(block->virtio_queue_device + 4 + (uint64_t)used_pos * sizeof(used_elem), used_elem, sizeof(used_elem)) != 0) {
            return -22;
        }
        kvm_store16(used_idx_raw, (uint16_t)(used_idx + 1));
        if (kvm_guest_write(block->virtio_queue_device + 2, used_idx_raw, sizeof(used_idx_raw)) != 0) {
            return -22;
        }
        block->virtio_last_avail_idx++;
        block->virtio_interrupt_status |= 1;
        printf("kvm-virtio-blk: request head=%u type=%u sector=%llu bytes=%u status=%u used_idx=%u\n",
            (unsigned int)head,
            type,
            (unsigned long long)sector,
            bytes,
            (unsigned int)request_status,
            (unsigned int)(uint16_t)(used_idx + 1));
    }
    return 0;
}

static int kvm_inject_irq_line(void *vm_ioctl_fn, void *vm_file, uint32_t irq)
{
    if (!linux_kvm_backend_enabled() || vm_ioctl_fn == NULL || vm_file == NULL) {
        return 0;
    }
    kb_kvm_irq_level_t level;
    memset(&level, 0, sizeof(level));
    level.irq = irq;
    level.level = 1;
    long high = call_kvm_ioctl(vm_ioctl_fn, vm_file, KB_KVM_IRQ_LINE, (unsigned long)(uintptr_t)&level);
    level.level = 0;
    long low = call_kvm_ioctl(vm_ioctl_fn, vm_file, KB_KVM_IRQ_LINE, (unsigned long)(uintptr_t)&level);
    printf("kvm-irq-line: irq=%u high=%ld low=%ld\n", irq, high, low);
    return high < 0 || low < 0 ? 1 : 0;
}

static int handle_virtio_mmio_exit(
    void *vm_ioctl_fn,
    void *vm_file,
    kvm_block_fixture_t *block,
    const kb_kvm_run_snapshot_t *run_snapshot)
{
    enum {
        VIRTIO_MMIO_BASE = 0x10001000,
        VIRTIO_MMIO_MAGIC_VALUE = 0x000,
        VIRTIO_MMIO_VERSION = 0x004,
        VIRTIO_MMIO_DEVICE_ID = 0x008,
        VIRTIO_MMIO_VENDOR_ID = 0x00c,
        VIRTIO_MMIO_DEVICE_FEATURES = 0x010,
        VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014,
        VIRTIO_MMIO_DRIVER_FEATURES = 0x020,
        VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024,
        VIRTIO_MMIO_QUEUE_SEL = 0x030,
        VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034,
        VIRTIO_MMIO_QUEUE_NUM = 0x038,
        VIRTIO_MMIO_QUEUE_READY = 0x044,
        VIRTIO_MMIO_QUEUE_NOTIFY = 0x050,
        VIRTIO_MMIO_INTERRUPT_STATUS = 0x060,
        VIRTIO_MMIO_INTERRUPT_ACK = 0x064,
        VIRTIO_MMIO_STATUS = 0x070,
        VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080,
        VIRTIO_MMIO_QUEUE_DESC_HIGH = 0x084,
        VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
        VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094,
        VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
        VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0a4,
        VIRTIO_MMIO_CONFIG_GENERATION = 0x0fc,
        VIRTIO_MMIO_BLK_CAPACITY = 0x100,
    };
    if (block == NULL || run_snapshot == NULL ||
        run_snapshot->mmio_phys_addr < VIRTIO_MMIO_BASE ||
        run_snapshot->mmio_phys_addr >= VIRTIO_MMIO_BASE + 0x1000)
    {
        return 0;
    }

    uint32_t offset = (uint32_t)(run_snapshot->mmio_phys_addr - VIRTIO_MMIO_BASE);
    if (run_snapshot->mmio_is_write) {
        uint32_t value = read_mmio_request_value(run_snapshot);
        switch (offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            block->virtio_device_features_sel = value;
            break;
        case VIRTIO_MMIO_DRIVER_FEATURES:
            if (block->virtio_driver_features_sel == 0) {
                block->virtio_driver_features = (block->virtio_driver_features & 0xffffffff00000000ULL) | value;
            } else if (block->virtio_driver_features_sel == 1) {
                block->virtio_driver_features = (block->virtio_driver_features & 0xffffffffULL) | ((uint64_t)value << 32);
            }
            break;
        case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
            block->virtio_driver_features_sel = value;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            block->virtio_queue_sel = value;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            block->virtio_queue_num = value;
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            block->virtio_queue_ready = value;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            block->virtio_queue_desc = (block->virtio_queue_desc & 0xffffffff00000000ULL) | value;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            block->virtio_queue_desc = (block->virtio_queue_desc & 0xffffffffULL) | ((uint64_t)value << 32);
            break;
        case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
            block->virtio_queue_driver = (block->virtio_queue_driver & 0xffffffff00000000ULL) | value;
            break;
        case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
            block->virtio_queue_driver = (block->virtio_queue_driver & 0xffffffffULL) | ((uint64_t)value << 32);
            break;
        case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
            block->virtio_queue_device = (block->virtio_queue_device & 0xffffffff00000000ULL) | value;
            break;
        case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
            block->virtio_queue_device = (block->virtio_queue_device & 0xffffffffULL) | ((uint64_t)value << 32);
            break;
        case VIRTIO_MMIO_INTERRUPT_ACK:
            block->virtio_interrupt_status &= ~value;
            break;
        case VIRTIO_MMIO_STATUS:
            block->virtio_status = value;
            break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            if (kvm_virtio_process_queue(block) != 0) {
                block->virtio_interrupt_status |= 1;
            }
            (void)kvm_inject_irq_line(vm_ioctl_fn, vm_file, 5);
            break;
        default:
            break;
        }
        printf("kvm-virtio-mmio: write offset=0x%x len=%u value=0x%x status=0x%x queue=%u num=%u ready=%u desc=0x%llx driver=0x%llx device=0x%llx features=0x%llx\n",
            offset,
            run_snapshot->mmio_len,
            value,
            block->virtio_status,
            block->virtio_queue_sel,
            block->virtio_queue_num,
            block->virtio_queue_ready,
            (unsigned long long)block->virtio_queue_desc,
            (unsigned long long)block->virtio_queue_driver,
            (unsigned long long)block->virtio_queue_device,
            (unsigned long long)block->virtio_driver_features);
        return 1;
    }

    uint64_t capacity = block->image_size / 512u;
    uint32_t value = 0;
    uint64_t device_features = 1ULL << 32;
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        value = 0x74726976u;
        break;
    case VIRTIO_MMIO_VERSION:
        value = 2;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        value = 2;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        value = 0x4b424f58u;
        break;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (block->virtio_device_features_sel == 0) {
            value = (uint32_t)(device_features & 0xffffffffu);
        } else if (block->virtio_device_features_sel == 1) {
            value = (uint32_t)(device_features >> 32);
        } else {
            value = 0;
        }
        break;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        value = 8;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        value = block->virtio_queue_num;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        value = block->virtio_queue_ready;
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        value = block->virtio_interrupt_status;
        break;
    case VIRTIO_MMIO_STATUS:
        value = block->virtio_status;
        break;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        value = 0;
        break;
    case VIRTIO_MMIO_BLK_CAPACITY:
        value = (uint32_t)(capacity & 0xffffffffu);
        break;
    case VIRTIO_MMIO_BLK_CAPACITY + 4:
        value = (uint32_t)(capacity >> 32);
        break;
    default:
        value = 0;
        break;
    }
    write_mmio_response(run_snapshot->run, value, run_snapshot->mmio_len);
    printf("kvm-virtio-mmio: read offset=0x%x len=%u value=0x%x capacity=%llu\n",
        offset,
        run_snapshot->mmio_len,
        value,
        (unsigned long long)capacity);
    return 1;
}

static int configure_vcpu_supported_cpuid(void *ioctl_fn, void *file, const char *label)
{
#if !defined(_WIN32)
    if (!linux_kvm_backend_enabled()) {
        return 0;
    }
    enum {
        CPUID_ENTRY_CAPACITY = 256,
    };
    size_t cpuid_size = sizeof(struct kvm_cpuid2) + CPUID_ENTRY_CAPACITY * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = (struct kvm_cpuid2 *)calloc(1, cpuid_size);
    if (cpuid == NULL) {
        return 1;
    }
    int kvm_fd = open("/dev/kvm", O_RDONLY | O_CLOEXEC);
    if (kvm_fd < 0) {
        fprintf(stderr, "failed to open /dev/kvm for CPUID\n");
        free(cpuid);
        return 1;
    }
    cpuid->nent = CPUID_ENTRY_CAPACITY;
    if (ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) != 0) {
        fprintf(stderr, "KVM_GET_SUPPORTED_CPUID failed\n");
        close(kvm_fd);
        free(cpuid);
        return 1;
    }
    close(kvm_fd);
    long set_cpuid = call_kvm_ioctl(ioctl_fn, file, KB_KVM_SET_CPUID2, (unsigned long)(uintptr_t)cpuid);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_SET_CPUID2 cmd=0x%x result=%ld entries=%u\n",
        label,
        KB_KVM_SET_CPUID2,
        set_cpuid,
        cpuid->nent);
    free(cpuid);
    if (set_cpuid < 0) {
        return 1;
    }
#else
    (void)ioctl_fn;
    (void)file;
    (void)label;
#endif
    return 0;
}

static int configure_vcpu_for_guest(void *ioctl_fn, void *file, const char *label, uint64_t rip, uint64_t rax, uint64_t rdx)
{
    struct kvm_sregs sregs;
    memset(&sregs, 0, sizeof(sregs));
    long get_sregs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_SREGS, (unsigned long)(uintptr_t)&sregs);
    printf("kvm-vcpu-ioctl: name=KVM_GET_SREGS cmd=0x%x result=%ld\n", KB_KVM_GET_SREGS, get_sregs);
    if (get_sregs < 0) {
        fprintf(stderr, "KVM_GET_SREGS failed: %ld\n", get_sregs);
        return 1;
    }

    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    long set_sregs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_SET_SREGS, (unsigned long)(uintptr_t)&sregs);
    printf("kvm-vcpu-ioctl: name=KVM_SET_SREGS cmd=0x%x result=%ld cs.base=0x%llx cs.selector=0x%x\n",
        KB_KVM_SET_SREGS,
        set_sregs,
        (unsigned long long)sregs.cs.base,
        (unsigned int)sregs.cs.selector);
    if (set_sregs < 0) {
        fprintf(stderr, "KVM_SET_SREGS failed: %ld\n", set_sregs);
        return 1;
    }

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rax = rax;
    regs.rdx = rdx;
    regs.rip = rip;
    regs.rsp = 0x1000;
    regs.rflags = 0x2;
    long set_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_SET_REGS, (unsigned long)(uintptr_t)&regs);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_SET_REGS cmd=0x%x result=%ld rip=0x%llx rax=0x%llx rdx=0x%llx rsp=0x%llx rflags=0x%llx\n",
        label,
        KB_KVM_SET_REGS,
        set_regs,
        (unsigned long long)regs.rip,
        (unsigned long long)regs.rax,
        (unsigned long long)regs.rdx,
        (unsigned long long)regs.rsp,
        (unsigned long long)regs.rflags);
    if (set_regs < 0) {
        fprintf(stderr, "KVM_SET_REGS failed: %ld\n", set_regs);
        return 1;
    }

    struct kvm_regs readback_regs;
    memset(&readback_regs, 0, sizeof(readback_regs));
    long get_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_REGS, (unsigned long)(uintptr_t)&readback_regs);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_GET_REGS cmd=0x%x result=%ld rip=0x%llx rax=0x%llx rdx=0x%llx rsp=0x%llx rflags=0x%llx\n",
        label,
        KB_KVM_GET_REGS,
        get_regs,
        (unsigned long long)readback_regs.rip,
        (unsigned long long)readback_regs.rax,
        (unsigned long long)readback_regs.rdx,
        (unsigned long long)readback_regs.rsp,
        (unsigned long long)readback_regs.rflags);
    if (get_regs < 0) {
        fprintf(stderr, "KVM_GET_REGS failed: %ld\n", get_regs);
        return 1;
    }
    return 0;
}

static void set_flat_segment(struct kvm_segment *segment, uint16_t selector, uint8_t type)
{
    memset(segment, 0, sizeof(*segment));
    segment->base = 0;
    segment->limit = 0xffffffffu;
    segment->selector = selector;
    segment->type = type;
    segment->present = 1;
    segment->dpl = 0;
    segment->db = 1;
    segment->s = 1;
    segment->l = 0;
    segment->g = 1;
}

static int configure_vcpu_for_linux_protected_entry(
    void *ioctl_fn,
    void *file,
    const char *label,
    uint64_t rip,
    uint64_t boot_params_gpa)
{
    struct kvm_sregs sregs;
    memset(&sregs, 0, sizeof(sregs));
    long get_sregs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_SREGS, (unsigned long)(uintptr_t)&sregs);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_GET_SREGS phase=linux-entry cmd=0x%x result=%ld\n",
        label,
        KB_KVM_GET_SREGS,
        get_sregs);
    if (get_sregs < 0) {
        fprintf(stderr, "KVM_GET_SREGS failed for linux entry: %ld\n", get_sregs);
        return 1;
    }

    set_flat_segment(&sregs.cs, 0x8, 0xb);
    set_flat_segment(&sregs.ds, 0x10, 0x3);
    set_flat_segment(&sregs.es, 0x10, 0x3);
    set_flat_segment(&sregs.fs, 0x10, 0x3);
    set_flat_segment(&sregs.gs, 0x10, 0x3);
    set_flat_segment(&sregs.ss, 0x10, 0x3);
    sregs.gdt.base = 0;
    sregs.gdt.limit = 0x17;
    sregs.cr0 |= 1;
    sregs.efer = 0;
    long set_sregs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_SET_SREGS, (unsigned long)(uintptr_t)&sregs);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_SET_SREGS phase=linux-entry cmd=0x%x result=%ld cr0=0x%llx cs.selector=0x%x cs.type=0x%x ds.selector=0x%x\n",
        label,
        KB_KVM_SET_SREGS,
        set_sregs,
        (unsigned long long)sregs.cr0,
        (unsigned int)sregs.cs.selector,
        (unsigned int)sregs.cs.type,
        (unsigned int)sregs.ds.selector);
    if (set_sregs < 0) {
        fprintf(stderr, "KVM_SET_SREGS failed for linux entry: %ld\n", set_sregs);
        return 1;
    }

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = rip;
    regs.rsi = boot_params_gpa;
    regs.rsp = 0x9000;
    regs.rflags = 0x2;
    long set_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_SET_REGS, (unsigned long)(uintptr_t)&regs);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_SET_REGS phase=linux-entry cmd=0x%x result=%ld rip=0x%llx rsi=0x%llx rsp=0x%llx rflags=0x%llx\n",
        label,
        KB_KVM_SET_REGS,
        set_regs,
        (unsigned long long)regs.rip,
        (unsigned long long)regs.rsi,
        (unsigned long long)regs.rsp,
        (unsigned long long)regs.rflags);
    if (set_regs < 0) {
        fprintf(stderr, "KVM_SET_REGS failed for linux entry: %ld\n", set_regs);
        return 1;
    }
    return 0;
}

static int run_guest_exit_case(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    void *guest_page,
    const char *label,
    const unsigned char *program,
    size_t program_size,
    uint64_t rax,
    uint64_t rdx,
    unsigned int expected_exit_reason,
    unsigned char expected_io_direction,
    unsigned short expected_io_port,
    unsigned char expected_io_data,
    uint64_t expected_mmio_phys_addr,
    unsigned char expected_mmio_is_write,
    unsigned char expected_mmio_data)
{
    memset(guest_page, 0, 4096);
    memcpy(guest_page, program, program_size);
    if (configure_vcpu_for_guest(ioctl_fn, file, label, 0, rax, rdx) != 0) {
        return 1;
    }

    long run_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN cmd=0x%x result=%ld\n", label, KB_KVM_RUN, run_result);
    if (run_result < 0) {
        fprintf(stderr, "KVM_RUN failed case=%s result=%ld\n", label, run_result);
        return 1;
    }

    kb_kvm_run_snapshot_t run_snapshot;
    if (kb_linux_kvm_run_snapshot(vcpu_fd, &run_snapshot) != 0 || run_snapshot.run == NULL) {
        fprintf(stderr, "KVM_RUN completed without a readable kvm_run area case=%s fd=%d\n", label, vcpu_fd);
        return 1;
    }
    printf("kvm-run-after: case=%s area=%p exit_reason=%u ready=%u if=%u flags=0x%x cr8=0x%llx apic_base=0x%llx\n",
        label,
        run_snapshot.run,
        run_snapshot.exit_reason,
        (unsigned int)run_snapshot.ready_for_interrupt_injection,
        (unsigned int)run_snapshot.if_flag,
        (unsigned int)run_snapshot.flags,
        run_snapshot.cr8,
        run_snapshot.apic_base);
    if (run_snapshot.exit_reason != expected_exit_reason) {
        fprintf(stderr,
            "KVM_RUN exit mismatch case=%s exit_reason=%u expected=%u\n",
            label,
            run_snapshot.exit_reason,
            expected_exit_reason);
        return 1;
    }
    if (expected_exit_reason == KB_KVM_EXIT_IO) {
        printf("kvm-run-io: case=%s direction=%u size=%u port=0x%x count=%u data_offset=0x%llx data0=0x%x\n",
            label,
            (unsigned int)run_snapshot.io_direction,
            (unsigned int)run_snapshot.io_size,
            (unsigned int)run_snapshot.io_port,
            run_snapshot.io_count,
            run_snapshot.io_data_offset,
            (unsigned int)run_snapshot.io_data[0]);
        if (run_snapshot.io_direction != expected_io_direction ||
            run_snapshot.io_size != 1 ||
            run_snapshot.io_port != expected_io_port ||
            run_snapshot.io_count != 1 ||
            run_snapshot.io_data[0] != expected_io_data)
        {
            fprintf(stderr, "KVM IO exit payload mismatch case=%s\n", label);
            return 1;
        }
    }
    if (expected_exit_reason == KB_KVM_EXIT_MMIO) {
        printf("kvm-run-mmio: case=%s phys=0x%llx len=%u is_write=%u data0=0x%x\n",
            label,
            run_snapshot.mmio_phys_addr,
            run_snapshot.mmio_len,
            (unsigned int)run_snapshot.mmio_is_write,
            (unsigned int)run_snapshot.mmio_data[0]);
        if (run_snapshot.mmio_phys_addr != expected_mmio_phys_addr ||
            run_snapshot.mmio_len != 1 ||
            run_snapshot.mmio_is_write != expected_mmio_is_write ||
            run_snapshot.mmio_data[0] != expected_mmio_data)
        {
            fprintf(stderr, "KVM MMIO exit payload mismatch case=%s\n", label);
            return 1;
        }
    }
    if (expected_exit_reason != KB_KVM_EXIT_HLT) {
        long drain_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
        printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN phase=drain cmd=0x%x result=%ld\n",
            label,
            KB_KVM_RUN,
            drain_result);
        if (drain_result < 0) {
            fprintf(stderr, "KVM_RUN drain phase failed case=%s result=%ld\n", label, drain_result);
            return 1;
        }
        kb_kvm_run_snapshot_t drain_snapshot;
        if (kb_linux_kvm_run_snapshot(vcpu_fd, &drain_snapshot) != 0 || drain_snapshot.run == NULL) {
            fprintf(stderr, "KVM_RUN drain phase did not expose kvm_run case=%s\n", label);
            return 1;
        }
        printf("kvm-run-drain-after: case=%s exit_reason=%u\n", label, drain_snapshot.exit_reason);
        if (drain_snapshot.exit_reason != KB_KVM_EXIT_HLT) {
            fprintf(stderr,
                "KVM_RUN drain did not reach HLT case=%s exit_reason=%u expected=%u\n",
                label,
                drain_snapshot.exit_reason,
                KB_KVM_EXIT_HLT);
            return 1;
        }
    }
    return 0;
}

static int run_guest_read_resume_case(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    void *guest_page,
    const char *label,
    const unsigned char *program,
    size_t program_size,
    unsigned int expected_exit_reason,
    unsigned char expected_io_direction,
    unsigned short expected_io_port,
    uint64_t expected_mmio_phys_addr,
    unsigned char response_value)
{
    memset(guest_page, 0, 4096);
    memcpy(guest_page, program, program_size);
    if (configure_vcpu_for_guest(ioctl_fn, file, label, 0, 0, 0) != 0) {
        return 1;
    }

    long first_run = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN phase=exit cmd=0x%x result=%ld\n", label, KB_KVM_RUN, first_run);
    if (first_run < 0) {
        fprintf(stderr, "KVM_RUN first phase failed case=%s result=%ld\n", label, first_run);
        return 1;
    }

    kb_kvm_run_snapshot_t exit_snapshot;
    if (kb_linux_kvm_run_snapshot(vcpu_fd, &exit_snapshot) != 0 || exit_snapshot.run == NULL) {
        fprintf(stderr, "KVM_RUN first phase did not expose kvm_run case=%s\n", label);
        return 1;
    }
    if (exit_snapshot.exit_reason != expected_exit_reason) {
        fprintf(stderr,
            "KVM_RUN first phase exit mismatch case=%s exit_reason=%u expected=%u\n",
            label,
            exit_snapshot.exit_reason,
            expected_exit_reason);
        return 1;
    }
    if (expected_exit_reason == KB_KVM_EXIT_IO) {
        printf("kvm-run-resume-exit: case=%s type=io direction=%u port=0x%x data_offset=0x%llx\n",
            label,
            (unsigned int)exit_snapshot.io_direction,
            (unsigned int)exit_snapshot.io_port,
            exit_snapshot.io_data_offset);
        if (exit_snapshot.io_direction != expected_io_direction ||
            exit_snapshot.io_size != 1 ||
            exit_snapshot.io_port != expected_io_port ||
            exit_snapshot.io_count != 1 ||
            exit_snapshot.io_data_offset >= KB_KVM_RUN_STORAGE_BYTES)
        {
            fprintf(stderr, "KVM IO resume exit payload mismatch case=%s\n", label);
            return 1;
        }
        ((unsigned char *)exit_snapshot.run)[exit_snapshot.io_data_offset] = response_value;
    } else if (expected_exit_reason == KB_KVM_EXIT_MMIO) {
        printf("kvm-run-resume-exit: case=%s type=mmio phys=0x%llx len=%u is_write=%u\n",
            label,
            exit_snapshot.mmio_phys_addr,
            exit_snapshot.mmio_len,
            (unsigned int)exit_snapshot.mmio_is_write);
        if (exit_snapshot.mmio_phys_addr != expected_mmio_phys_addr ||
            exit_snapshot.mmio_len != 1 ||
            exit_snapshot.mmio_is_write != 0)
        {
            fprintf(stderr, "KVM MMIO resume exit payload mismatch case=%s\n", label);
            return 1;
        }
        ((unsigned char *)exit_snapshot.run)[KB_KVM_RUN_MMIO_DATA_OFFSET] = response_value;
    } else {
        fprintf(stderr, "unsupported resume case exit reason case=%s exit_reason=%u\n", label, expected_exit_reason);
        return 1;
    }

    long second_run = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN phase=resume cmd=0x%x result=%ld response=0x%x\n",
        label,
        KB_KVM_RUN,
        second_run,
        (unsigned int)response_value);
    if (second_run < 0) {
        fprintf(stderr, "KVM_RUN resume phase failed case=%s result=%ld\n", label, second_run);
        return 1;
    }

    kb_kvm_run_snapshot_t resume_snapshot;
    if (kb_linux_kvm_run_snapshot(vcpu_fd, &resume_snapshot) != 0 || resume_snapshot.run == NULL) {
        fprintf(stderr, "KVM_RUN resume phase did not expose kvm_run case=%s\n", label);
        return 1;
    }
    printf("kvm-run-resume-after: case=%s exit_reason=%u\n", label, resume_snapshot.exit_reason);
    if (resume_snapshot.exit_reason != KB_KVM_EXIT_HLT) {
        fprintf(stderr,
            "KVM_RUN resume did not reach HLT case=%s exit_reason=%u expected=%u\n",
            label,
            resume_snapshot.exit_reason,
            KB_KVM_EXIT_HLT);
        return 1;
    }

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    long get_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_REGS, (unsigned long)(uintptr_t)&regs);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_GET_REGS phase=resume cmd=0x%x result=%ld rax=0x%llx rip=0x%llx\n",
        label,
        KB_KVM_GET_REGS,
        get_regs,
        (unsigned long long)regs.rax,
        (unsigned long long)regs.rip);
    if (get_regs < 0 || (uint8_t)regs.rax != response_value) {
        fprintf(stderr,
            "KVM_RUN resume did not inject AL case=%s rax=0x%llx expected_al=0x%x\n",
            label,
            (unsigned long long)regs.rax,
            (unsigned int)response_value);
        return 1;
    }
    return 0;
}

static int run_guest_serial_output_case(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    void *guest_page,
    uint64_t entry_rip,
    size_t program_offset,
    int clear_page,
    const char *label,
    const unsigned char *program,
    size_t program_size,
    const char *expected_output)
{
    if (clear_page) {
        memset(guest_page, 0, 4096);
    }
    if (program_offset > 4096 || program_size > 4096 - program_offset) {
        fprintf(stderr, "serial guest program does not fit case=%s\n", label);
        return 1;
    }
    memcpy((unsigned char *)guest_page + program_offset, program, program_size);
    if (configure_vcpu_for_guest(ioctl_fn, file, label, entry_rip, 0, 0) != 0) {
        return 1;
    }

    char output[128];
    size_t output_size = 0;
    memset(output, 0, sizeof(output));

    size_t step_limit = strlen(expected_output) + 8;
    if (step_limit < 64) {
        step_limit = 64;
    }
    if (step_limit > sizeof(output) - 1) {
        step_limit = sizeof(output) - 1;
    }

    for (size_t step = 0; step < step_limit; step++) {
        long run_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
        printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN step=%zu cmd=0x%x result=%ld\n",
            label,
            step,
            KB_KVM_RUN,
            run_result);
        if (run_result < 0) {
            fprintf(stderr, "KVM_RUN failed case=%s step=%zu result=%ld\n", label, step, run_result);
            return 1;
        }

        kb_kvm_run_snapshot_t run_snapshot;
        if (kb_linux_kvm_run_snapshot(vcpu_fd, &run_snapshot) != 0 || run_snapshot.run == NULL) {
            fprintf(stderr, "KVM_RUN serial phase did not expose kvm_run case=%s\n", label);
            return 1;
        }
        if (run_snapshot.exit_reason == KB_KVM_EXIT_HLT) {
            output[output_size] = '\0';
            printf("kvm-serial-output: case=%s text=%s\n", label, output);
            if (strcmp(output, expected_output) != 0) {
                fprintf(stderr,
                    "serial output mismatch case=%s text=%s expected=%s\n",
                    label,
                    output,
                    expected_output);
                return 1;
            }
            return 0;
        }
        if (run_snapshot.exit_reason != KB_KVM_EXIT_IO ||
            run_snapshot.io_direction != 1 ||
            run_snapshot.io_size != 1 ||
            run_snapshot.io_port != 0x3f8 ||
            run_snapshot.io_count != 1)
        {
            fprintf(stderr,
                "unexpected serial exit case=%s exit_reason=%u direction=%u port=0x%x\n",
                label,
                run_snapshot.exit_reason,
                (unsigned int)run_snapshot.io_direction,
                (unsigned int)run_snapshot.io_port);
            return 1;
        }
        if (output_size + 1 >= sizeof(output)) {
            fprintf(stderr, "serial output too long case=%s\n", label);
            return 1;
        }
        output[output_size++] = (char)run_snapshot.io_data[0];
        printf("kvm-serial-byte: case=%s port=0x%x value=0x%x char=%c\n",
            label,
            (unsigned int)run_snapshot.io_port,
            (unsigned int)run_snapshot.io_data[0],
            run_snapshot.io_data[0] >= 0x20 && run_snapshot.io_data[0] < 0x7f ? (char)run_snapshot.io_data[0] : '.');
    }

    fprintf(stderr, "serial guest did not halt case=%s\n", label);
    return 1;
}

static int run_guest_block_route_case(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    void *guest_page,
    kvm_block_fixture_t *block)
{
    enum {
        KB_KVM_BLOCK_CMD_PORT = 0x510,
        KB_KVM_BLOCK_READ_PORT = 0x511,
        KB_KVM_BLOCK_WRITE_PORT = 0x512,
    };
    const unsigned char program[] = {
        0xba, 0x10, 0x05,
        0xb0, 0x01,
        0xee,
        0xba, 0x11, 0x05,
        0xec,
        0xba, 0xf8, 0x03,
        0xee,
        0xba, 0x12, 0x05,
        0xb0, 0x5a,
        0xee,
        0xf4
    };
    unsigned char sector[512];
    char serial[8];
    size_t serial_size = 0;
    memset(sector, 0, sizeof(sector));
    memset(serial, 0, sizeof(serial));
    memset(guest_page, 0, 4096);
    memcpy(guest_page, program, sizeof(program));
    block->last_command = 0;

    if (configure_vcpu_for_guest(ioctl_fn, file, "kobox-block-route-pio", 0, 0, 0) != 0) {
        return 1;
    }

    for (size_t step = 0; step < 32; step++) {
        long run_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
        printf("kvm-vcpu-ioctl: case=kobox-block-route-pio name=KVM_RUN step=%zu cmd=0x%x result=%ld\n",
            step,
            KB_KVM_RUN,
            run_result);
        if (run_result < 0) {
            fprintf(stderr, "KVM_RUN failed case=kobox-block-route-pio step=%zu result=%ld\n", step, run_result);
            return 1;
        }

        kb_kvm_run_snapshot_t run_snapshot;
        if (kb_linux_kvm_run_snapshot(vcpu_fd, &run_snapshot) != 0 || run_snapshot.run == NULL) {
            fprintf(stderr, "KVM_RUN block route did not expose kvm_run\n");
            return 1;
        }
        if (run_snapshot.exit_reason == KB_KVM_EXIT_HLT) {
            kb_block_disk_snapshot_t disk_snapshot;
            if (kb_block_subsystem_disk_snapshot(block->disk, &disk_snapshot) != 0 ||
                disk_snapshot.read_count == 0 ||
                disk_snapshot.write_count == 0 ||
                block->image[512] != 0x5a ||
                serial_size != 1 ||
                serial[0] != (char)block->image[0])
            {
                fprintf(stderr,
                    "kvm-block-route: snapshot mismatch read=%llu write=%llu image0=0x%x image512=0x%x serial_size=%zu serial0=0x%x\n",
                    (unsigned long long)disk_snapshot.read_count,
                    (unsigned long long)disk_snapshot.write_count,
                    (unsigned int)block->image[0],
                    (unsigned int)block->image[512],
                    serial_size,
                    serial_size == 0 ? 0 : (unsigned int)(unsigned char)serial[0]);
                return 1;
            }
            printf("kvm-block-route: pio=ok serial0=0x%x disk_reads=%llu disk_writes=%llu written_sector1_byte0=0x%x\n",
                (unsigned int)(unsigned char)serial[0],
                (unsigned long long)disk_snapshot.read_count,
                (unsigned long long)disk_snapshot.write_count,
                (unsigned int)block->image[512]);
            return 0;
        }
        if (run_snapshot.exit_reason != KB_KVM_EXIT_IO) {
            fprintf(stderr, "kvm-block-route: unexpected exit_reason=%u\n", run_snapshot.exit_reason);
            return 1;
        }
        printf("kvm-block-route-io: step=%zu direction=%u port=0x%x size=%u count=%u data0=0x%x\n",
            step,
            (unsigned int)run_snapshot.io_direction,
            (unsigned int)run_snapshot.io_port,
            (unsigned int)run_snapshot.io_size,
            run_snapshot.io_count,
            (unsigned int)run_snapshot.io_data[0]);

        if (run_snapshot.io_direction == 1 && run_snapshot.io_port == KB_KVM_BLOCK_CMD_PORT) {
            block->last_command = run_snapshot.io_data[0];
            continue;
        }
        if (run_snapshot.io_direction == 0 && run_snapshot.io_port == KB_KVM_BLOCK_READ_PORT) {
            if (run_snapshot.io_data_offset >= KB_KVM_RUN_STORAGE_BYTES ||
                block->last_command != 0x01 ||
                kb_block_subsystem_disk_read(block->disk, 0, sector, sizeof(sector)) != 0)
            {
                fprintf(stderr, "kvm-block-route: failed read command\n");
                return 1;
            }
            ((unsigned char *)run_snapshot.run)[run_snapshot.io_data_offset] = sector[0];
            continue;
        }
        if (run_snapshot.io_direction == 1 && run_snapshot.io_port == KB_KVM_BLOCK_WRITE_PORT) {
            if (kb_block_subsystem_disk_read(block->disk, 1, sector, sizeof(sector)) != 0) {
                fprintf(stderr, "kvm-block-route: failed read-before-write sector=1\n");
                return 1;
            }
            sector[0] = run_snapshot.io_data[0];
            if (kb_block_subsystem_disk_write(block->disk, 1, sector, sizeof(sector)) != 0) {
                fprintf(stderr, "kvm-block-route: failed write sector=1\n");
                return 1;
            }
            continue;
        }
        if (run_snapshot.io_direction == 1 && run_snapshot.io_port == 0x3f8) {
            if (serial_size + 1 < sizeof(serial)) {
                serial[serial_size++] = (char)run_snapshot.io_data[0];
                serial[serial_size] = '\0';
            }
            continue;
        }

        fprintf(stderr,
            "kvm-block-route: unhandled io direction=%u port=0x%x\n",
            (unsigned int)run_snapshot.io_direction,
            (unsigned int)run_snapshot.io_port);
        return 1;
    }

    fprintf(stderr, "kvm-block-route: did not halt\n");
    return 1;
}

static int run_linux_protected_entry_serial_case(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    const char *label,
    uint64_t entry_rip,
    const char *expected_output)
{
    if (configure_vcpu_for_linux_protected_entry(ioctl_fn, file, label, entry_rip, 0x7000) != 0) {
        return 1;
    }

    char output[128];
    size_t output_size = 0;
    memset(output, 0, sizeof(output));

    for (size_t step = 0; step < 64; step++) {
        long run_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
        printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN phase=linux-entry step=%zu cmd=0x%x result=%ld\n",
            label,
            step,
            KB_KVM_RUN,
            run_result);
        if (run_result < 0) {
            fprintf(stderr, "KVM_RUN failed for linux entry case=%s step=%zu result=%ld\n", label, step, run_result);
            return 1;
        }

        kb_kvm_run_snapshot_t run_snapshot;
        if (kb_linux_kvm_run_snapshot(vcpu_fd, &run_snapshot) != 0 || run_snapshot.run == NULL) {
            fprintf(stderr, "KVM_RUN linux entry phase did not expose kvm_run case=%s\n", label);
            return 1;
        }
        if (run_snapshot.exit_reason == KB_KVM_EXIT_HLT) {
            output[output_size] = '\0';
            printf("kvm-linux-entry-output: case=%s text=%s\n", label, output);
            if (strcmp(output, expected_output) != 0) {
                fprintf(stderr,
                    "linux entry serial output mismatch case=%s text=%s expected=%s\n",
                    label,
                    output,
                    expected_output);
                return 1;
            }
            return 0;
        }
        if (run_snapshot.exit_reason != KB_KVM_EXIT_IO ||
            run_snapshot.io_direction != 1 ||
            run_snapshot.io_size != 1 ||
            run_snapshot.io_port != 0x3f8 ||
            run_snapshot.io_count != 1)
        {
            fprintf(stderr,
                "unexpected linux entry exit case=%s exit_reason=%u direction=%u port=0x%x\n",
                label,
                run_snapshot.exit_reason,
                (unsigned int)run_snapshot.io_direction,
                (unsigned int)run_snapshot.io_port);
            return 1;
        }
        if (output_size + 1 >= sizeof(output)) {
            fprintf(stderr, "linux entry serial output too long case=%s\n", label);
            return 1;
        }
        output[output_size++] = (char)run_snapshot.io_data[0];
        printf("kvm-linux-entry-byte: case=%s port=0x%x value=0x%x char=%c\n",
            label,
            (unsigned int)run_snapshot.io_port,
            (unsigned int)run_snapshot.io_data[0],
            run_snapshot.io_data[0] >= 0x20 && run_snapshot.io_data[0] < 0x7f ? (char)run_snapshot.io_data[0] : '.');
    }

    fprintf(stderr, "linux protected entry did not halt case=%s\n", label);
    return 1;
}

static int run_linux_protected_entry_probe_once(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    const char *label,
    uint64_t entry_rip,
    const unsigned char *kernel_area,
    uint64_t kernel_gpa,
    size_t kernel_region_size)
{
    if (configure_vcpu_for_linux_protected_entry(ioctl_fn, file, label, entry_rip, 0x7000) != 0) {
        return 1;
    }
    long run_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
    printf("kvm-vcpu-ioctl: case=%s name=KVM_RUN phase=linux-entry-once cmd=0x%x result=%ld\n",
        label,
        KB_KVM_RUN,
        run_result);
    if (run_result < 0) {
        fprintf(stderr, "KVM_RUN failed for linux entry once case=%s result=%ld\n", label, run_result);
        return 1;
    }
    kb_kvm_run_snapshot_t run_snapshot;
    if (kb_linux_kvm_run_snapshot(vcpu_fd, &run_snapshot) != 0 || run_snapshot.run == NULL) {
        fprintf(stderr, "KVM_RUN linux entry once did not expose kvm_run case=%s\n", label);
        return 1;
    }
    printf("kvm-linux-entry-once: case=%s entry_rip=0x%llx exit_reason=%u io_port=0x%x mmio_phys=0x%llx\n",
        label,
        (unsigned long long)entry_rip,
        run_snapshot.exit_reason,
        (unsigned int)run_snapshot.io_port,
        run_snapshot.mmio_phys_addr);
    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    long get_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_REGS, (unsigned long)(uintptr_t)&regs);
    printf("kvm-linux-entry-regs: case=%s name=KVM_GET_REGS cmd=0x%x result=%ld rip=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rsp=0x%llx rflags=0x%llx\n",
        label,
        KB_KVM_GET_REGS,
        get_regs,
        (unsigned long long)regs.rip,
        (unsigned long long)regs.rax,
        (unsigned long long)regs.rbx,
        (unsigned long long)regs.rcx,
        (unsigned long long)regs.rdx,
        (unsigned long long)regs.rsi,
        (unsigned long long)regs.rsp,
        (unsigned long long)regs.rflags);
    if (get_regs < 0) {
        return 1;
    }
    if (kernel_area != NULL &&
        regs.rip >= kernel_gpa &&
        regs.rip < kernel_gpa + kernel_region_size)
    {
        size_t offset = (size_t)(regs.rip - kernel_gpa);
        size_t bytes_before = offset < 16 ? offset : 16;
        size_t dump_offset = offset - bytes_before;
        size_t dump_size = kernel_region_size - dump_offset;
        if (dump_size > 64) {
            dump_size = 64;
        }
        printf("kvm-linux-entry-memory: case=%s rip=0x%llx gpa=0x%llx offset=0x%zx bytes=",
            label,
            (unsigned long long)regs.rip,
            (unsigned long long)kernel_gpa,
            dump_offset);
        for (size_t i = 0; i < dump_size; i++) {
            printf("%02x", (unsigned int)kernel_area[dump_offset + i]);
        }
        printf("\n");
    }
    return 0;
}

static int run_linux_protected_entry_boot_loop(
    void *ioctl_fn,
    void *file,
    int vcpu_fd,
    void *vm_ioctl_fn,
    void *vm_file,
    const char *label,
    uint64_t entry_rip,
    const unsigned char *kernel_area,
    uint64_t kernel_gpa,
    size_t kernel_region_size,
    kvm_block_fixture_t *block)
{
    if (configure_vcpu_for_linux_protected_entry(ioctl_fn, file, label, entry_rip, 0x7000) != 0) {
        return 1;
    }

    char serial_output[65536];
    size_t serial_output_size = 0;
    memset(serial_output, 0, sizeof(serial_output));
    const char *serial_until = getenv("KOBOX_KVM_LINUX_ENTRY_SERIAL_UNTIL");

    size_t step_limit = 65536;
    const char *step_limit_env = getenv("KOBOX_KVM_LINUX_ENTRY_BOOT_STEPS");
    if (step_limit_env != NULL && step_limit_env[0] != '\0') {
        unsigned long long parsed_step_limit = strtoull(step_limit_env, NULL, 0);
        if (parsed_step_limit > 0) {
            step_limit = (size_t)parsed_step_limit;
        }
    }

    for (size_t step = 0; step < step_limit; step++) {
        long run_result = call_kvm_ioctl(ioctl_fn, file, KB_KVM_RUN, 0);
        if (run_result < 0) {
            fprintf(stderr, "KVM_RUN failed for linux boot loop case=%s step=%zu result=%ld\n", label, step, run_result);
            return 1;
        }

        kb_kvm_run_snapshot_t run_snapshot;
        if (kb_linux_kvm_run_snapshot(vcpu_fd, &run_snapshot) != 0 || run_snapshot.run == NULL) {
            fprintf(stderr, "KVM_RUN linux boot loop did not expose kvm_run case=%s\n", label);
            return 1;
        }

        if (run_snapshot.exit_reason == KB_KVM_EXIT_HLT) {
            struct kvm_regs regs;
            memset(&regs, 0, sizeof(regs));
            long get_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_REGS, (unsigned long)(uintptr_t)&regs);
            printf("kvm-linux-boot-loop-hlt: case=%s step=%zu get_regs=%ld rip=0x%llx rax=0x%llx rsi=0x%llx rsp=0x%llx serial=%s\n",
                label,
                step,
                get_regs,
                (unsigned long long)regs.rip,
                (unsigned long long)regs.rax,
                (unsigned long long)regs.rsi,
                (unsigned long long)regs.rsp,
                serial_output);
            if (kernel_area != NULL &&
                regs.rip >= kernel_gpa &&
                regs.rip < kernel_gpa + kernel_region_size)
            {
                size_t offset = (size_t)(regs.rip - kernel_gpa);
                size_t bytes_before = offset < 16 ? offset : 16;
                size_t dump_offset = offset - bytes_before;
                size_t dump_size = kernel_region_size - dump_offset;
                if (dump_size > 64) {
                    dump_size = 64;
                }
                printf("kvm-linux-boot-loop-memory: case=%s rip=0x%llx offset=0x%zx bytes=",
                    label,
                    (unsigned long long)regs.rip,
                    dump_offset);
                for (size_t i = 0; i < dump_size; i++) {
                    printf("%02x", (unsigned int)kernel_area[dump_offset + i]);
                }
                printf("\n");
            }
            return get_regs < 0 ? 1 : 0;
        }

        if (run_snapshot.exit_reason == KB_KVM_EXIT_IO) {
            if (step < 128 || (step % 1024) == 0 || run_snapshot.io_port == 0x3f8) {
                printf("kvm-linux-boot-loop-io: case=%s step=%zu direction=%u port=0x%x size=%u count=%u data_offset=0x%llx data0=0x%x\n",
                    label,
                    step,
                    (unsigned int)run_snapshot.io_direction,
                    (unsigned int)run_snapshot.io_port,
                    (unsigned int)run_snapshot.io_size,
                    run_snapshot.io_count,
                    run_snapshot.io_data_offset,
                    (unsigned int)run_snapshot.io_data[0]);
            }
            if (run_snapshot.io_direction == 1) {
                if (run_snapshot.io_port == 0x3f8 && serial_output_size + 1 < sizeof(serial_output)) {
                    serial_output[serial_output_size++] = (char)run_snapshot.io_data[0];
                    serial_output[serial_output_size] = '\0';
                    if (serial_until != NULL &&
                        serial_until[0] != '\0' &&
                        strstr(serial_output, serial_until) != NULL)
                    {
                        printf("kvm-linux-boot-loop-serial-match: case=%s step=%zu needle=%s\n",
                            label,
                            step,
                            serial_until);
                        return 0;
                    }
                }
            } else if (run_snapshot.io_data_offset < KB_KVM_RUN_STORAGE_BYTES) {
                size_t response_bytes = (size_t)run_snapshot.io_size * (size_t)run_snapshot.io_count;
                size_t max_response_bytes = KB_KVM_RUN_STORAGE_BYTES - (size_t)run_snapshot.io_data_offset;
                if (response_bytes > max_response_bytes) {
                    response_bytes = max_response_bytes;
                }
                memset((unsigned char *)run_snapshot.run + run_snapshot.io_data_offset, 0xff, response_bytes);
            }
            continue;
        }

        if (run_snapshot.exit_reason == KB_KVM_EXIT_MMIO) {
            int is_virtio_mmio_probe =
                run_snapshot.mmio_phys_addr >= 0x10001000ULL &&
                run_snapshot.mmio_phys_addr < 0x10002000ULL;
            if (step < 128 || (step % 1024) == 0 || is_virtio_mmio_probe) {
                printf("kvm-linux-boot-loop-mmio: case=%s step=%zu phys=0x%llx len=%u is_write=%u data0=0x%x\n",
                    label,
                    step,
                    run_snapshot.mmio_phys_addr,
                    run_snapshot.mmio_len,
                    (unsigned int)run_snapshot.mmio_is_write,
                    (unsigned int)run_snapshot.mmio_data[0]);
            }
            if (handle_virtio_mmio_exit(vm_ioctl_fn, vm_file, block, &run_snapshot)) {
                continue;
            }
            if (!run_snapshot.mmio_is_write) {
                memset((unsigned char *)run_snapshot.run + KB_KVM_RUN_MMIO_DATA_OFFSET, 0, sizeof(run_snapshot.mmio_data));
            }
            continue;
        }

        struct kvm_regs regs;
        memset(&regs, 0, sizeof(regs));
        long get_regs = call_kvm_ioctl(ioctl_fn, file, KB_KVM_GET_REGS, (unsigned long)(uintptr_t)&regs);
        printf("kvm-linux-boot-loop-stop: case=%s step=%zu exit_reason=%u get_regs=%ld rip=0x%llx rax=0x%llx\n",
            label,
            step,
            run_snapshot.exit_reason,
            get_regs,
            (unsigned long long)regs.rip,
            (unsigned long long)regs.rax);
        return get_regs < 0 ? 1 : 0;
    }

    fprintf(stderr, "linux boot loop reached step limit case=%s step_limit=%zu\n", label, step_limit);
    return 1;
}

static int set_guest_memory_region(
    void *ioctl_fn,
    void *file,
    uint32_t slot,
    uint64_t guest_phys_addr,
    uint64_t memory_size,
    void *userspace_addr)
{
    kb_kvm_userspace_memory_region_t region;
    memset(&region, 0, sizeof(region));
    region.slot = slot;
    region.guest_phys_addr = guest_phys_addr;
    region.memory_size = memory_size;
    region.userspace_addr = (uint64_t)(uintptr_t)userspace_addr;
    long set_memory = call_kvm_ioctl(
        ioctl_fn,
        file,
        KB_KVM_SET_USER_MEMORY_REGION,
        (unsigned long)(uintptr_t)&region);
    printf("kvm-vm-ioctl: name=KVM_SET_USER_MEMORY_REGION cmd=0x%x slot=%u gpa=0x%llx size=0x%llx result=%ld\n",
        KB_KVM_SET_USER_MEMORY_REGION,
        slot,
        (unsigned long long)guest_phys_addr,
        (unsigned long long)memory_size,
        set_memory);
    if (set_memory < 0) {
        fprintf(stderr,
            "KVM_SET_USER_MEMORY_REGION failed slot=%u gpa=0x%llx result=%ld\n",
            slot,
            (unsigned long long)guest_phys_addr,
            set_memory);
        return 1;
    }
    kvm_guest_memory_record_region(slot, guest_phys_addr, memory_size, userspace_addr);
    return 0;
}

static int allocate_zeroed_guest_region(
    void *ioctl_fn,
    void *file,
    guest_region_backing_t *region)
{
    if (region == NULL || region->memory_size == 0) {
        return 1;
    }
    if (posix_memalign(&region->data, 4096, (size_t)region->memory_size) != 0 || region->data == NULL) {
        fprintf(stderr,
            "failed to allocate guest region name=%s slot=%u gpa=0x%llx size=0x%llx\n",
            region->name == NULL ? "(unnamed)" : region->name,
            region->slot,
            (unsigned long long)region->guest_phys_addr,
            (unsigned long long)region->memory_size);
        region->data = NULL;
        return 1;
    }
    memset(region->data, 0, (size_t)region->memory_size);
    if (set_guest_memory_region(
            ioctl_fn,
            file,
            region->slot,
            region->guest_phys_addr,
            region->memory_size,
            region->data) != 0)
    {
        free(region->data);
        region->data = NULL;
        return 1;
    }
    return 0;
}

static void free_guest_region_backings(guest_region_backing_t *regions, size_t region_count)
{
    if (regions == NULL) {
        return;
    }
    for (size_t i = 0; i < region_count; i++) {
        free(regions[i].data);
        regions[i].data = NULL;
    }
}

static void write_u16le(unsigned char *base, size_t offset, uint16_t value)
{
    base[offset] = (unsigned char)(value & 0xffu);
    base[offset + 1] = (unsigned char)((value >> 8) & 0xffu);
}

static void write_u32le(unsigned char *base, size_t offset, uint32_t value)
{
    base[offset] = (unsigned char)(value & 0xffu);
    base[offset + 1] = (unsigned char)((value >> 8) & 0xffu);
    base[offset + 2] = (unsigned char)((value >> 16) & 0xffu);
    base[offset + 3] = (unsigned char)((value >> 24) & 0xffu);
}

static void write_u64le(unsigned char *base, size_t offset, uint64_t value)
{
    write_u32le(base, offset, (uint32_t)(value & 0xffffffffu));
    write_u32le(base, offset + 4, (uint32_t)(value >> 32));
}

static uint16_t read_u16le(const unsigned char *base, size_t offset)
{
    return (uint16_t)base[offset] | ((uint16_t)base[offset + 1] << 8);
}

static uint32_t read_u32le(const unsigned char *base, size_t offset)
{
    return (uint32_t)base[offset] |
        ((uint32_t)base[offset + 1] << 8) |
        ((uint32_t)base[offset + 2] << 16) |
        ((uint32_t)base[offset + 3] << 24);
}

static uint64_t read_u64le(const unsigned char *base, size_t offset)
{
    return (uint64_t)read_u32le(base, offset) | ((uint64_t)read_u32le(base, offset + 4) << 32);
}

static size_t align_up_size(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

enum {
    KB_LINUX_CMDLINE_GPA = 0x20000,
    KB_LINUX_BOOT_PARAMS_E820_ENTRIES = 0x1e8,
    KB_LINUX_BOOT_PARAMS_SENTINEL = 0x1ef,
    KB_LINUX_BOOT_PARAMS_HDR = 0x1f1,
    KB_LINUX_BOOT_PARAMS_E820_TABLE = 0x2d0,
    KB_LINUX_BOOT_PARAMS_SETUP_HEADER_END = 0x290,
    KB_LINUX_SETUP_SETUP_SECTS = 0x00,
    KB_LINUX_SETUP_BOOT_FLAG = 0x0d,
    KB_LINUX_SETUP_HEADER = 0x11,
    KB_LINUX_SETUP_VERSION = 0x15,
    KB_LINUX_SETUP_TYPE_OF_LOADER = 0x1f,
    KB_LINUX_SETUP_LOADFLAGS = 0x20,
    KB_LINUX_SETUP_CODE32_START = 0x23,
    KB_LINUX_SETUP_RAMDISK_IMAGE = 0x27,
    KB_LINUX_SETUP_RAMDISK_SIZE = 0x2b,
    KB_LINUX_SETUP_HEAP_END_PTR = 0x33,
    KB_LINUX_SETUP_CMD_LINE_PTR = 0x37,
    KB_LINUX_SETUP_INITRD_ADDR_MAX = 0x3b,
    KB_LINUX_SETUP_KERNEL_ALIGNMENT = 0x3f,
    KB_LINUX_SETUP_RELOCATABLE_KERNEL = 0x43,
    KB_LINUX_SETUP_CMDLINE_SIZE = 0x47,
    KB_LINUX_SETUP_PREF_ADDRESS = 0x67,
    KB_LINUX_SETUP_INIT_SIZE = 0x6f,
    KB_LINUX_E820_ENTRY_SIZE = 20,
    KB_LINUX_E820_TYPE_RAM = 1,
    KB_LINUX_HIGH_RAM_BYTES = 0x07f00000,
};

typedef struct linux_bzimage_header {
    uint8_t setup_sects;
    uint16_t boot_flag;
    uint16_t version;
    uint32_t code32_start;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t relocatable_kernel;
    uint32_t cmdline_size;
    uint64_t pref_address;
    uint32_t init_size;
    size_t setup_size;
    size_t payload_offset;
    size_t payload_size;
} linux_bzimage_header_t;

static int parse_linux_bzimage(const unsigned char *image, size_t image_size, linux_bzimage_header_t *out_header)
{
    enum {
        MIN_BZIMAGE_HEADER_SIZE = 0x264,
        SECTOR_SIZE = 512,
    };
    if (image == NULL || out_header == NULL || image_size < MIN_BZIMAGE_HEADER_SIZE) {
        fprintf(stderr, "kvm-bzimage: image too small size=%zu\n", image_size);
        return 1;
    }
    memset(out_header, 0, sizeof(*out_header));
    out_header->setup_sects = image[KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_SETUP_SECTS];
    out_header->boot_flag = read_u16le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_BOOT_FLAG);
    if (out_header->boot_flag != 0xaa55 ||
        memcmp(image + KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_HEADER, "HdrS", 4) != 0)
    {
        fprintf(stderr,
            "kvm-bzimage: invalid linux setup header boot_flag=0x%x magic=%.4s\n",
            (unsigned int)out_header->boot_flag,
            (const char *)(image + KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_HEADER));
        return 1;
    }
    out_header->version = read_u16le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_VERSION);
    out_header->code32_start = read_u32le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_CODE32_START);
    out_header->initrd_addr_max = read_u32le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_INITRD_ADDR_MAX);
    out_header->kernel_alignment = read_u32le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_KERNEL_ALIGNMENT);
    out_header->relocatable_kernel = image[KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_RELOCATABLE_KERNEL];
    out_header->cmdline_size = read_u32le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_CMDLINE_SIZE);
    out_header->pref_address = read_u64le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_PREF_ADDRESS);
    out_header->init_size = read_u32le(image, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_INIT_SIZE);
    {
        uint8_t setup_sects = out_header->setup_sects == 0 ? 4 : out_header->setup_sects;
        out_header->setup_size = ((size_t)setup_sects + 1) * SECTOR_SIZE;
    }
    if (out_header->setup_size >= image_size) {
        fprintf(stderr,
            "kvm-bzimage: setup area consumes image setup_size=%zu image_size=%zu\n",
            out_header->setup_size,
            image_size);
        return 1;
    }
    out_header->payload_offset = out_header->setup_size;
    out_header->payload_size = image_size - out_header->payload_offset;
    printf("kvm-bzimage-header: setup_sects=%u setup_size=%zu payload_offset=0x%zx payload_size=%zu version=0x%x code32_start=0x%x alignment=0x%x relocatable=%u pref_address=0x%llx init_size=0x%x cmdline_size=0x%x\n",
        (unsigned int)out_header->setup_sects,
        out_header->setup_size,
        out_header->payload_offset,
        out_header->payload_size,
        (unsigned int)out_header->version,
        out_header->code32_start,
        out_header->kernel_alignment,
        (unsigned int)out_header->relocatable_kernel,
        (unsigned long long)out_header->pref_address,
        out_header->init_size,
        out_header->cmdline_size);
    return 0;
}

static void apply_bzimage_header_to_boot_params(
    unsigned char *boot_page,
    const unsigned char *image,
    const linux_bzimage_header_t *header)
{
    memcpy(
        boot_page + KB_LINUX_BOOT_PARAMS_HDR,
        image + KB_LINUX_BOOT_PARAMS_HDR,
        KB_LINUX_BOOT_PARAMS_SETUP_HEADER_END - KB_LINUX_BOOT_PARAMS_HDR);
    boot_page[KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_SETUP_SECTS] = header->setup_sects;
    write_u16le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_BOOT_FLAG, header->boot_flag);
    memcpy(boot_page + KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_HEADER, "HdrS", 4);
    write_u16le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_VERSION, header->version);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_CODE32_START, header->code32_start);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_INITRD_ADDR_MAX, header->initrd_addr_max);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_KERNEL_ALIGNMENT, header->kernel_alignment);
    boot_page[KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_RELOCATABLE_KERNEL] = header->relocatable_kernel;
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_CMDLINE_SIZE, header->cmdline_size);
    write_u64le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_PREF_ADDRESS, header->pref_address);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_INIT_SIZE, header->init_size);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_CMD_LINE_PTR, KB_LINUX_CMDLINE_GPA);
    boot_page[KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_TYPE_OF_LOADER] = 0xff;
    boot_page[KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_LOADFLAGS] = 0x80;
    write_u16le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_HEAP_END_PTR, 0xe000);
    printf("kvm-bzimage-boot-params: version=0x%x code32_start=0x%x cmdline_ptr=0x%x initrd_addr_max=0x%x\n",
        (unsigned int)header->version,
        header->code32_start,
        read_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_CMD_LINE_PTR),
        header->initrd_addr_max);
}

static int register_bzimage_layout(
    void *ioctl_fn,
    void *vm_file,
    const unsigned char *image,
    const linux_bzimage_header_t *header,
    void **out_setup_area,
    void **out_kernel_area,
    size_t *out_kernel_region_size)
{
    enum {
        SETUP_GPA = 0x90000,
        KERNEL_GPA = 0x100000,
        PAGE_SIZE = 4096,
    };
    size_t setup_region_size = align_up_size(header->setup_size, PAGE_SIZE);
    size_t kernel_required_size = header->init_size > header->payload_size ? header->init_size : header->payload_size;
    if (header->pref_address > KERNEL_GPA) {
        uint64_t preferred_end_size = (header->pref_address - KERNEL_GPA) + header->init_size;
        if (preferred_end_size > kernel_required_size) {
            kernel_required_size = (size_t)preferred_end_size;
        }
    }
    if (kernel_required_size < KB_LINUX_HIGH_RAM_BYTES) {
        kernel_required_size = KB_LINUX_HIGH_RAM_BYTES;
    }
    size_t kernel_region_size = align_up_size(kernel_required_size, PAGE_SIZE);
    void *setup_area = NULL;
    void *kernel_area = NULL;
    if (posix_memalign(&setup_area, PAGE_SIZE, setup_region_size) != 0 || setup_area == NULL) {
        fprintf(stderr, "failed to allocate bzImage setup area\n");
        return 1;
    }
    if (posix_memalign(&kernel_area, PAGE_SIZE, kernel_region_size) != 0 || kernel_area == NULL) {
        fprintf(stderr, "failed to allocate bzImage kernel payload area\n");
        free(setup_area);
        return 1;
    }
    memset(setup_area, 0, setup_region_size);
    memset(kernel_area, 0, kernel_region_size);
    memcpy(setup_area, image, header->setup_size);
    memcpy(kernel_area, image + header->payload_offset, header->payload_size);
    printf("kvm-bzimage-placement: setup_gpa=0x%x setup_size=%zu setup_region_size=%zu kernel_gpa=0x%x payload_size=%zu init_size=0x%x kernel_region_size=%zu\n",
        SETUP_GPA,
        header->setup_size,
        setup_region_size,
        KERNEL_GPA,
        header->payload_size,
        header->init_size,
        kernel_region_size);
    if (set_guest_memory_region(ioctl_fn, vm_file, 2, SETUP_GPA, setup_region_size, setup_area) != 0 ||
        set_guest_memory_region(ioctl_fn, vm_file, 3, KERNEL_GPA, kernel_region_size, kernel_area) != 0)
    {
        free(kernel_area);
        free(setup_area);
        return 1;
    }
    *out_setup_area = setup_area;
    *out_kernel_area = kernel_area;
    *out_kernel_region_size = kernel_region_size;
    return 0;
}

static int install_linux_initrd(
    unsigned char *boot_page,
    unsigned char *kernel_area,
    size_t kernel_region_size,
    const char *initrd_path)
{
    enum {
        KERNEL_GPA = 0x100000,
        INITRD_GPA = 0x6000000,
    };
    if (initrd_path == NULL || initrd_path[0] == '\0') {
        return 0;
    }

    void *initrd_data = NULL;
    size_t initrd_size = 0;
    if (read_file(initrd_path, &initrd_data, &initrd_size) != KB_OK) {
        fprintf(stderr, "kvm-initrd: failed to read path=%s\n", initrd_path);
        return 1;
    }
    if (initrd_size == 0 || INITRD_GPA < KERNEL_GPA) {
        free(initrd_data);
        return 1;
    }
    size_t initrd_offset = INITRD_GPA - KERNEL_GPA;
    if (initrd_offset > kernel_region_size || initrd_size > kernel_region_size - initrd_offset) {
        fprintf(stderr,
            "kvm-initrd: does not fit path=%s size=%zu gpa=0x%x kernel_region=%zu\n",
            initrd_path,
            initrd_size,
            INITRD_GPA,
            kernel_region_size);
        free(initrd_data);
        return 1;
    }
    memcpy(kernel_area + initrd_offset, initrd_data, initrd_size);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_RAMDISK_IMAGE, INITRD_GPA);
    write_u32le(boot_page, KB_LINUX_BOOT_PARAMS_HDR + KB_LINUX_SETUP_RAMDISK_SIZE, (uint32_t)initrd_size);
    printf("kvm-initrd: path=%s gpa=0x%x size=%zu\n", initrd_path, INITRD_GPA, initrd_size);
    free(initrd_data);
    return 0;
}

static int populate_linux_boot_params(unsigned char *boot_page, unsigned char *cmdline_page, const char *cmdline)
{
    enum {
        BOOT_PARAMS_E820_ENTRIES = KB_LINUX_BOOT_PARAMS_E820_ENTRIES,
        BOOT_PARAMS_SENTINEL = KB_LINUX_BOOT_PARAMS_SENTINEL,
        BOOT_PARAMS_HDR = KB_LINUX_BOOT_PARAMS_HDR,
        BOOT_PARAMS_E820_TABLE = KB_LINUX_BOOT_PARAMS_E820_TABLE,
        SETUP_BOOT_FLAG = KB_LINUX_SETUP_BOOT_FLAG,
        SETUP_HEADER = KB_LINUX_SETUP_HEADER,
        SETUP_VERSION = KB_LINUX_SETUP_VERSION,
        SETUP_TYPE_OF_LOADER = KB_LINUX_SETUP_TYPE_OF_LOADER,
        SETUP_LOADFLAGS = KB_LINUX_SETUP_LOADFLAGS,
        SETUP_HEAP_END_PTR = KB_LINUX_SETUP_HEAP_END_PTR,
        SETUP_CMD_LINE_PTR = KB_LINUX_SETUP_CMD_LINE_PTR,
        SETUP_INITRD_ADDR_MAX = KB_LINUX_SETUP_INITRD_ADDR_MAX,
        SETUP_KERNEL_ALIGNMENT = KB_LINUX_SETUP_KERNEL_ALIGNMENT,
        SETUP_RELOCATABLE_KERNEL = KB_LINUX_SETUP_RELOCATABLE_KERNEL,
        SETUP_CMDLINE_SIZE = KB_LINUX_SETUP_CMDLINE_SIZE,
        E820_ENTRY_SIZE = KB_LINUX_E820_ENTRY_SIZE,
        E820_TYPE_RAM = KB_LINUX_E820_TYPE_RAM,
    };
    memset(boot_page, 0, 4096);
    const char *effective_cmdline = cmdline == NULL || cmdline[0] == '\0' ? "console=ttyS0 earlycon" : cmdline;
    size_t cmdline_len = strlen(effective_cmdline);
    if (cmdline_len >= 0x1000) {
        fprintf(stderr, "kvm-linux-boot-params: cmdline too long len=%zu\n", cmdline_len);
        return 1;
    }
    memcpy(boot_page, "KOBOXBOOT", 9);
    memcpy(boot_page + 0x100, effective_cmdline, cmdline_len + 1);
    memcpy(cmdline_page, effective_cmdline, cmdline_len + 1);
    boot_page[BOOT_PARAMS_SENTINEL] = 0;

    boot_page[BOOT_PARAMS_E820_ENTRIES] = 2;
    write_u64le(boot_page, BOOT_PARAMS_E820_TABLE, 0x00000000ULL);
    write_u64le(boot_page, BOOT_PARAMS_E820_TABLE + 8, 0x0009f000ULL);
    write_u32le(boot_page, BOOT_PARAMS_E820_TABLE + 16, E820_TYPE_RAM);
    write_u64le(boot_page, BOOT_PARAMS_E820_TABLE + E820_ENTRY_SIZE, 0x00100000ULL);
    write_u64le(boot_page, BOOT_PARAMS_E820_TABLE + E820_ENTRY_SIZE + 8, KB_LINUX_HIGH_RAM_BYTES);
    write_u32le(boot_page, BOOT_PARAMS_E820_TABLE + E820_ENTRY_SIZE + 16, E820_TYPE_RAM);

    write_u16le(boot_page, BOOT_PARAMS_HDR + SETUP_BOOT_FLAG, 0xaa55);
    memcpy(boot_page + BOOT_PARAMS_HDR + SETUP_HEADER, "HdrS", 4);
    write_u16le(boot_page, BOOT_PARAMS_HDR + SETUP_VERSION, 0x020f);
    boot_page[BOOT_PARAMS_HDR + SETUP_TYPE_OF_LOADER] = 0xff;
    boot_page[BOOT_PARAMS_HDR + SETUP_LOADFLAGS] = 0x80;
    write_u16le(boot_page, BOOT_PARAMS_HDR + SETUP_HEAP_END_PTR, 0xe000);
    write_u32le(boot_page, BOOT_PARAMS_HDR + SETUP_CMD_LINE_PTR, KB_LINUX_CMDLINE_GPA);
    write_u32le(boot_page, BOOT_PARAMS_HDR + SETUP_INITRD_ADDR_MAX, 0x7fffffff);
    write_u32le(boot_page, BOOT_PARAMS_HDR + SETUP_KERNEL_ALIGNMENT, 0x200000);
    boot_page[BOOT_PARAMS_HDR + SETUP_RELOCATABLE_KERNEL] = 1;
    write_u32le(boot_page, BOOT_PARAMS_HDR + SETUP_CMDLINE_SIZE, 0x1000);

    if (read_u16le(boot_page, BOOT_PARAMS_HDR + SETUP_BOOT_FLAG) != 0xaa55 ||
        memcmp(boot_page + BOOT_PARAMS_HDR + SETUP_HEADER, "HdrS", 4) != 0 ||
        read_u32le(boot_page, BOOT_PARAMS_HDR + SETUP_CMD_LINE_PTR) != KB_LINUX_CMDLINE_GPA ||
        boot_page[BOOT_PARAMS_E820_ENTRIES] != 2)
    {
        return 1;
    }
    printf("kvm-linux-boot-params: boot_flag=0x%x header=HdrS version=0x%x cmdline_ptr=0x%x e820_entries=%u cmdline=%s\n",
        (unsigned int)read_u16le(boot_page, BOOT_PARAMS_HDR + SETUP_BOOT_FLAG),
        (unsigned int)read_u16le(boot_page, BOOT_PARAMS_HDR + SETUP_VERSION),
        read_u32le(boot_page, BOOT_PARAMS_HDR + SETUP_CMD_LINE_PTR),
        (unsigned int)boot_page[BOOT_PARAMS_E820_ENTRIES],
        effective_cmdline);
    printf("kvm-linux-e820: index=0 addr=0x0 size=0x9f000 type=1\n");
    printf("kvm-linux-e820: index=1 addr=0x100000 size=0x7f00000 type=1\n");
    return 0;
}

int main(int argc, char **argv)
{
    install_signal_diagnostics();

    const char *dep_paths[8];
    size_t dep_count = 0;
    const char *target_path = NULL;
    const char *bzimage_path = NULL;
    const char *initrd_path = NULL;
    const char *linux_cmdline = "console=ttyS0 earlycon";
    const char *block_image_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--dep=", 6) == 0) {
            if (dep_count >= sizeof(dep_paths) / sizeof(dep_paths[0])) {
                fprintf(stderr, "too many dependencies\n");
                return 2;
            }
            dep_paths[dep_count++] = argv[i] + 6;
        } else if (strncmp(argv[i], "--bzimage=", 10) == 0) {
            bzimage_path = argv[i] + 10;
        } else if (strncmp(argv[i], "--initrd=", 9) == 0) {
            initrd_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--cmdline=", 10) == 0) {
            linux_cmdline = argv[i] + 10;
        } else if (strncmp(argv[i], "--block-image=", 14) == 0) {
            block_image_path = argv[i] + 14;
        } else if (target_path == NULL) {
            target_path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (target_path == NULL) {
        fprintf(stderr, "usage: %s [--dep=kvm.ko ...] [--bzimage=vmlinuz] [--initrd=initramfs.cpio] [--cmdline='...'] [--block-image=rootfs.img] kvm-arch.ko\n", argv[0]);
        return 2;
    }
    int linux_boot_irqchip =
        linux_kvm_backend_enabled() &&
        bzimage_path != NULL &&
        getenv("KOBOX_KVM_LINUX_ENTRY_BOOT_LOOP") != NULL;

    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        fprintf(stderr, "failed to create module backend\n");
        return 1;
    }

    loaded_module_t deps[8];
    memset(deps, 0, sizeof(deps));
    loaded_module_t target_module;
    memset(&target_module, 0, sizeof(target_module));

    for (size_t i = 0; i < dep_count; i++) {
        kb_status_t status = load_module(backend, dep_paths[i], &deps[i]);
        if (status != KB_OK) {
            fprintf(stderr, "dependency open failed status=%d path=%s\n", (int)status, dep_paths[i]);
            for (size_t j = i; j > 0; j--) {
                if (deps[j - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[j - 1].module);
                }
                unload_module(&deps[j - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
        int dep_init_result = 0;
        status = kb_module_call_init(deps[i].module, &dep_init_result);
        if (status != KB_OK || dep_init_result != 0) {
            fprintf(stderr, "dependency init failed status=%d result=%d path=%s\n", (int)status, dep_init_result, dep_paths[i]);
            unload_module(&deps[i]);
            for (size_t j = i; j > 0; j--) {
                if (deps[j - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[j - 1].module);
                }
                unload_module(&deps[j - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
        deps[i].initialized = 1;
        printf("dependency init_module returned %d path=%s\n", dep_init_result, dep_paths[i]);
    }

    kb_status_t status = load_module(backend, target_path, &target_module);
    if (status != KB_OK) {
        fprintf(stderr, "target open failed status=%d path=%s\n", (int)status, target_path);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    int init_result = 0;
    status = kb_module_call_init(target_module.module, &init_result);
    if (status != KB_OK || init_result != 0) {
        if (status == KB_OK && init_result == -95) {
            printf("target init_module returned -95 path=%s\n", target_path);
            printf("skip: KVM arch module reported unsupported CPU/capability under current host exposure\n");
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 77;
        }
        fprintf(stderr, "target init failed status=%d result=%d path=%s\n", (int)status, init_result, target_path);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    target_module.initialized = 1;
    printf("target init_module returned %d path=%s\n", init_result, target_path);

    kb_kvm_misc_snapshot_t snapshot;
    if (kb_linux_kvm_misc_snapshot("kvm", &snapshot) != 0 ||
        snapshot.fops == NULL ||
        snapshot.unlocked_ioctl == NULL)
    {
        fprintf(stderr, "kvm misc device was not registered with ioctl\n");
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("kvm-misc: name=%s misc=%p fops=%p ioctl=%p registered=%d\n",
        snapshot.name == NULL ? "(null)" : snapshot.name,
        snapshot.misc_device,
        snapshot.fops,
        snapshot.unlocked_ioctl,
        snapshot.registered);
    for (size_t i = 0; i < dep_count; i++) {
        void *kvm_x86_ops = NULL;
        if (kb_module_find_symbol(deps[i].module, "kvm_x86_ops", &kvm_x86_ops) == KB_OK && kvm_x86_ops != NULL) {
            uint32_t vm_size = 0;
            memcpy(&vm_size, (const unsigned char *)kvm_x86_ops + 0x3c, sizeof(vm_size));
            printf("kvm-x86-ops: module=%s address=%p vm_size=%u\n", deps[i].path, kvm_x86_ops, vm_size);
            break;
        }
    }

    void *fake_file = calloc(1, KB_KVM_FAKE_FILE_BYTES);
    if (fake_file == NULL) {
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    long api_version = call_kvm_ioctl(snapshot.unlocked_ioctl, fake_file, KB_KVM_GET_API_VERSION, 0);
    printf("kvm-ioctl: name=KVM_GET_API_VERSION cmd=0x%x result=%ld\n",
        KB_KVM_GET_API_VERSION,
        api_version);
    if (api_version != 12) {
        fprintf(stderr, "unexpected KVM API version: %ld\n", api_version);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    long extension_zero = call_kvm_ioctl(snapshot.unlocked_ioctl, fake_file, KB_KVM_CHECK_EXTENSION, 0);
    printf("kvm-ioctl: name=KVM_CHECK_EXTENSION cmd=0x%x extension=0 result=%ld\n",
        KB_KVM_CHECK_EXTENSION,
        extension_zero);
    if (extension_zero < 0) {
        fprintf(stderr, "KVM_CHECK_EXTENSION failed: %ld\n", extension_zero);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    long vm_fd = call_kvm_ioctl(snapshot.unlocked_ioctl, fake_file, KB_KVM_CREATE_VM, 0);
    printf("kvm-ioctl: name=KVM_CREATE_VM cmd=0x%x result=%ld\n",
        KB_KVM_CREATE_VM,
        vm_fd);
    if (vm_fd < 0) {
        fprintf(stderr, "KVM_CREATE_VM failed: %ld\n", vm_fd);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    kb_kvm_fd_snapshot_t vm_snapshot;
    if (kb_linux_kvm_fd_snapshot((int)vm_fd, &vm_snapshot) != 0 ||
        vm_snapshot.file == NULL ||
        vm_snapshot.private_data == NULL ||
        vm_snapshot.unlocked_ioctl == NULL)
    {
        fprintf(stderr, "KVM_CREATE_VM did not create a usable VM file fd=%ld\n", vm_fd);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("kvm-fd: name=%s fd=%d file=%p private=%p fops=%p ioctl=%p active=%d\n",
        vm_snapshot.name == NULL ? "(null)" : vm_snapshot.name,
        vm_snapshot.fd,
        vm_snapshot.file,
        vm_snapshot.private_data,
        vm_snapshot.fops,
        vm_snapshot.unlocked_ioctl,
        vm_snapshot.active);
    if (linux_boot_irqchip) {
        long create_irqchip = call_kvm_ioctl(vm_snapshot.unlocked_ioctl, vm_snapshot.file, KB_KVM_CREATE_IRQCHIP, 0);
        printf("kvm-vm-ioctl: name=KVM_CREATE_IRQCHIP cmd=0x%x phase=pre-vcpu result=%ld\n",
            KB_KVM_CREATE_IRQCHIP,
            create_irqchip);
        if (create_irqchip < 0) {
            fprintf(stderr, "KVM_CREATE_IRQCHIP failed before VCPU creation: %ld\n", create_irqchip);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
    }
    long vm_extension_zero = call_kvm_ioctl(
        vm_snapshot.unlocked_ioctl,
        vm_snapshot.file,
        KB_KVM_CHECK_EXTENSION,
        0);
    printf("kvm-vm-ioctl: name=KVM_CHECK_EXTENSION cmd=0x%x extension=0 result=%ld\n",
        KB_KVM_CHECK_EXTENSION,
        vm_extension_zero);
    if (vm_extension_zero < 0) {
        fprintf(stderr, "VM KVM_CHECK_EXTENSION failed: %ld\n", vm_extension_zero);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    long vcpu_fd = call_kvm_ioctl(vm_snapshot.unlocked_ioctl, vm_snapshot.file, KB_KVM_CREATE_VCPU, 0);
    printf("kvm-vm-ioctl: name=KVM_CREATE_VCPU cmd=0x%x vcpu=0 result=%ld\n",
        KB_KVM_CREATE_VCPU,
        vcpu_fd);
    if (vcpu_fd < 0) {
        fprintf(stderr, "KVM_CREATE_VCPU failed: %ld\n", vcpu_fd);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    kb_kvm_fd_snapshot_t vcpu_snapshot;
    if (kb_linux_kvm_fd_snapshot((int)vcpu_fd, &vcpu_snapshot) != 0 ||
        vcpu_snapshot.file == NULL ||
        vcpu_snapshot.private_data == NULL ||
        vcpu_snapshot.unlocked_ioctl == NULL)
    {
        fprintf(stderr, "KVM_CREATE_VCPU did not create a usable VCPU file fd=%ld\n", vcpu_fd);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("kvm-vcpu-fd: name=%s fd=%d file=%p private=%p fops=%p ioctl=%p active=%d\n",
        vcpu_snapshot.name == NULL ? "(null)" : vcpu_snapshot.name,
        vcpu_snapshot.fd,
        vcpu_snapshot.file,
        vcpu_snapshot.private_data,
        vcpu_snapshot.fops,
        vcpu_snapshot.unlocked_ioctl,
        vcpu_snapshot.active);
    if (configure_vcpu_supported_cpuid(vcpu_snapshot.unlocked_ioctl, vcpu_snapshot.file, "vcpu-create") != 0) {
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    void *guest_page = NULL;
    if (posix_memalign(&guest_page, 4096, 4096) != 0 || guest_page == NULL) {
        fprintf(stderr, "failed to allocate aligned guest page\n");
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    memset(guest_page, 0, 4096);

    if (set_guest_memory_region(vm_snapshot.unlocked_ioctl, vm_snapshot.file, 0, 0, 4096, guest_page) != 0) {
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    kb_kvm_run_snapshot_t before_run;
    if (kb_linux_kvm_run_snapshot((int)vcpu_fd, &before_run) != 0 || before_run.run == NULL) {
        fprintf(stderr, "KVM_CREATE_VCPU did not expose a usable kvm_run area fd=%ld\n", vcpu_fd);
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    printf("kvm-run-before: area=%p exit_reason=%u ready=%u if=%u flags=0x%x cr8=0x%llx apic_base=0x%llx\n",
        before_run.run,
        before_run.exit_reason,
        (unsigned int)before_run.ready_for_interrupt_injection,
        (unsigned int)before_run.if_flag,
        (unsigned int)before_run.flags,
        before_run.cr8,
        before_run.apic_base);
    printf("kvm-run-facet: mode=kobox-exit-decode delegated_ioctls=KVM_GET_SREGS,KVM_SET_SREGS,KVM_SET_REGS,KVM_GET_REGS decoded_exits=HLT,IO,MMIO\n");

    kvm_block_fixture_t block_fixture;
    int block_fixture_initialized = 0;
    if (kvm_block_fixture_init(&block_fixture, block_image_path) != 0) {
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    block_fixture_initialized = 1;

    const unsigned char hlt_program[] = {0xf4};
    const unsigned char io_out_program[] = {0xe6, 0x7f, 0xf4};
    const unsigned char serial_output_program[] = {
        0xba, 0xf8, 0x03,
        0xb0, 'K', 0xee,
        0xb0, 'O', 0xee,
        0xb0, 'B', 0xee,
        0xb0, 'O', 0xee,
        0xb0, 'X', 0xee,
        0xb0, '\n', 0xee,
        0xf4
    };
    const unsigned char boot_cmdline_serial_program[] = {
        0xba, 0xf8, 0x03,
        0xbe, 0x00, 0x71,
        0xac,
        0x84, 0xc0,
        0x74, 0x03,
        0xee,
        0xeb, 0xf8,
        0xf4
    };
    const unsigned char io_in_resume_program[] = {0xe4, 0x7c, 0xf4};
    const unsigned char mmio_read_resume_program[] = {
        0x67, 0xa0,
        0x00, 0x30, 0x00, 0x00,
        0xf4
    };
    const unsigned char mmio_write_program[] = {
        0x67, 0xa2,
        0x04, 0x30, 0x00, 0x00,
        0xf4
    };
    if (!linux_boot_irqchip &&
        (run_guest_exit_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            "hlt",
            hlt_program,
            sizeof(hlt_program),
            0,
            0,
            KB_KVM_EXIT_HLT,
            0,
            0,
            0,
            0,
            0,
            0) != 0 ||
        run_guest_serial_output_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            0,
            0,
            1,
            "serial-0x3f8-output",
            serial_output_program,
            sizeof(serial_output_program),
            "KOBOX\n") != 0 ||
        run_guest_exit_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            "io-out-imm8",
            io_out_program,
            sizeof(io_out_program),
            0x5a,
            0,
            KB_KVM_EXIT_IO,
            1,
            0x7f,
            0x5a,
            0,
            0,
            0) != 0 ||
        run_guest_read_resume_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            "io-in-resume",
            io_in_resume_program,
            sizeof(io_in_resume_program),
            KB_KVM_EXIT_IO,
            0,
            0x7c,
            0,
            0x3c) != 0 ||
        run_guest_read_resume_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            "mmio-read-resume",
            mmio_read_resume_program,
            sizeof(mmio_read_resume_program),
            KB_KVM_EXIT_MMIO,
            0,
            0,
            0x3000,
            0x9b) != 0 ||
        run_guest_exit_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            "mmio-write-moffs8",
            mmio_write_program,
            sizeof(mmio_write_program),
            0xa5,
            0,
            KB_KVM_EXIT_MMIO,
            0,
            0,
            0,
            0x3004,
            1,
            0xa5) != 0 ||
        run_guest_block_route_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            guest_page,
            &block_fixture) != 0)
        )
    {
        if (block_fixture_initialized) {
            kvm_block_fixture_destroy(&block_fixture);
        }
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }

    void *bzimage_data = NULL;
    size_t bzimage_size = 0;
    void *bzimage_setup_area = NULL;
    void *bzimage_kernel_area = NULL;
    size_t bzimage_kernel_region_size = 0;
    linux_bzimage_header_t bzimage_header;
    memset(&bzimage_header, 0, sizeof(bzimage_header));
    guest_region_backing_t bzimage_low_regions[] = {
        { "low-ram-gap-1000-7000", 5, 0x1000, 0x6000, NULL },
        { "low-ram-gap-8000-20000", 6, 0x8000, 0x18000, NULL },
        { "low-ram-gap-21000-90000", 10, 0x21000, 0x6f000, NULL },
        { "low-ram-gap-95000-a0000", 7, 0x95000, 0xb000, NULL },
        { "legacy-vga-option-rom-hole", 8, 0xa0000, 0x40000, NULL },
        { "legacy-bios-rom-window", 4, 0xe0000, 0x20000, NULL },
    };
    const size_t bzimage_low_region_count = sizeof(bzimage_low_regions) / sizeof(bzimage_low_regions[0]);

    void *boot_page = NULL;
    void *cmdline_page = NULL;
    if (posix_memalign(&boot_page, 4096, 4096) != 0 || boot_page == NULL) {
        fprintf(stderr, "failed to allocate aligned boot page\n");
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    if (posix_memalign(&cmdline_page, 4096, 4096) != 0 || cmdline_page == NULL) {
        fprintf(stderr, "failed to allocate aligned cmdline page\n");
        free(boot_page);
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    memset(cmdline_page, 0, 4096);
    if (populate_linux_boot_params((unsigned char *)boot_page, (unsigned char *)cmdline_page, linux_cmdline) != 0) {
        fprintf(stderr, "failed to populate linux boot params\n");
        free(cmdline_page);
        free(boot_page);
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    if (bzimage_path != NULL) {
        if (read_file(bzimage_path, &bzimage_data, &bzimage_size) != KB_OK) {
            fprintf(stderr, "failed to read bzImage path=%s\n", bzimage_path);
            free(boot_page);
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
        if (parse_linux_bzimage((const unsigned char *)bzimage_data, bzimage_size, &bzimage_header) != 0) {
            free(bzimage_data);
            free(boot_page);
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
        apply_bzimage_header_to_boot_params((unsigned char *)boot_page, (const unsigned char *)bzimage_data, &bzimage_header);
        if (register_bzimage_layout(
                vm_snapshot.unlocked_ioctl,
                vm_snapshot.file,
                (const unsigned char *)bzimage_data,
                &bzimage_header,
                &bzimage_setup_area,
                &bzimage_kernel_area,
                &bzimage_kernel_region_size) != 0)
        {
            free(bzimage_data);
            free(boot_page);
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
        if (install_linux_initrd(
                (unsigned char *)boot_page,
                (unsigned char *)bzimage_kernel_area,
                bzimage_kernel_region_size,
                initrd_path) != 0)
        {
            free(bzimage_kernel_area);
            free(bzimage_setup_area);
            free(bzimage_data);
            free(boot_page);
            if (block_fixture_initialized) {
                kvm_block_fixture_destroy(&block_fixture);
            }
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
        for (size_t i = 0; i < bzimage_low_region_count; i++) {
            if (allocate_zeroed_guest_region(
                    vm_snapshot.unlocked_ioctl,
                    vm_snapshot.file,
                    &bzimage_low_regions[i]) != 0)
            {
                free_guest_region_backings(bzimage_low_regions, bzimage_low_region_count);
                free(bzimage_kernel_area);
                free(bzimage_setup_area);
                free(bzimage_data);
                free(boot_page);
                if (block_fixture_initialized) {
                    kvm_block_fixture_destroy(&block_fixture);
                }
                free(guest_page);
                free(fake_file);
                (void)kb_module_call_cleanup(target_module.module);
                unload_module(&target_module);
                for (size_t dep_i = dep_count; dep_i > 0; dep_i--) {
                    if (deps[dep_i - 1].initialized) {
                        (void)kb_module_call_cleanup(deps[dep_i - 1].module);
                    }
                    unload_module(&deps[dep_i - 1]);
                }
                kb_device_backend_destroy(backend);
                return 1;
            }
        }
        if (linux_kvm_backend_enabled() && !linux_boot_irqchip) {
            long create_irqchip = call_kvm_ioctl(vm_snapshot.unlocked_ioctl, vm_snapshot.file, KB_KVM_CREATE_IRQCHIP, 0);
            printf("kvm-vm-ioctl: name=KVM_CREATE_IRQCHIP cmd=0x%x phase=linux-boot result=%ld\n",
                KB_KVM_CREATE_IRQCHIP,
                create_irqchip);
        }
    }
    printf("kvm-boot-layout: params_gpa=0x7000 cmdline_gpa=0x%x entry_gpa=0x7c00\n", KB_LINUX_CMDLINE_GPA);
    if (set_guest_memory_region(vm_snapshot.unlocked_ioctl, vm_snapshot.file, 1, 0x7000, 4096, boot_page) != 0 ||
        set_guest_memory_region(vm_snapshot.unlocked_ioctl, vm_snapshot.file, 9, KB_LINUX_CMDLINE_GPA, 4096, cmdline_page) != 0 ||
        (!linux_boot_irqchip && run_guest_serial_output_case(
            vcpu_snapshot.unlocked_ioctl,
            vcpu_snapshot.file,
            (int)vcpu_fd,
            boot_page,
            0x7c00,
            0xc00,
            0,
            "boot-layout-serial-0x7c00",
            boot_cmdline_serial_program,
            sizeof(boot_cmdline_serial_program),
            linux_cmdline) != 0))
    {
        free(bzimage_kernel_area);
        free(bzimage_setup_area);
        free_guest_region_backings(bzimage_low_regions, bzimage_low_region_count);
        free(bzimage_data);
        free(boot_page);
        free(guest_page);
        free(fake_file);
        (void)kb_module_call_cleanup(target_module.module);
        unload_module(&target_module);
        for (size_t i = dep_count; i > 0; i--) {
            if (deps[i - 1].initialized) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
            }
            unload_module(&deps[i - 1]);
        }
        kb_device_backend_destroy(backend);
        return 1;
    }
    if (bzimage_path != NULL && getenv("KOBOX_KVM_LINUX_ENTRY_SERIAL_PROOF") != NULL) {
        if (run_linux_protected_entry_serial_case(
                vcpu_snapshot.unlocked_ioctl,
                vcpu_snapshot.file,
                (int)vcpu_fd,
                "linux-protected-entry-serial",
                bzimage_header.code32_start,
                "LINUX\n") != 0)
        {
            free(bzimage_kernel_area);
            free(bzimage_setup_area);
            free_guest_region_backings(bzimage_low_regions, bzimage_low_region_count);
            free(bzimage_data);
            free(boot_page);
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
    } else if (bzimage_path != NULL && getenv("KOBOX_KVM_LINUX_ENTRY_RUN_ONCE") != NULL) {
        if (run_linux_protected_entry_probe_once(
                vcpu_snapshot.unlocked_ioctl,
                vcpu_snapshot.file,
                (int)vcpu_fd,
                "linux-protected-entry-once",
                bzimage_header.code32_start,
                (const unsigned char *)bzimage_kernel_area,
                0x100000,
                bzimage_kernel_region_size) != 0)
        {
            free(bzimage_kernel_area);
            free(bzimage_setup_area);
            free_guest_region_backings(bzimage_low_regions, bzimage_low_region_count);
            free(bzimage_data);
            free(boot_page);
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
    } else if (bzimage_path != NULL && getenv("KOBOX_KVM_LINUX_ENTRY_BOOT_LOOP") != NULL) {
        if (run_linux_protected_entry_boot_loop(
                vcpu_snapshot.unlocked_ioctl,
                vcpu_snapshot.file,
                (int)vcpu_fd,
                vm_snapshot.unlocked_ioctl,
                vm_snapshot.file,
                "linux-protected-entry-boot-loop",
                bzimage_header.code32_start,
                (const unsigned char *)bzimage_kernel_area,
                0x100000,
                bzimage_kernel_region_size,
                &block_fixture) != 0)
        {
            free(bzimage_kernel_area);
            free(bzimage_setup_area);
            free_guest_region_backings(bzimage_low_regions, bzimage_low_region_count);
            free(bzimage_data);
            free(boot_page);
            free(guest_page);
            free(fake_file);
            (void)kb_module_call_cleanup(target_module.module);
            unload_module(&target_module);
            for (size_t i = dep_count; i > 0; i--) {
                if (deps[i - 1].initialized) {
                    (void)kb_module_call_cleanup(deps[i - 1].module);
                }
                unload_module(&deps[i - 1]);
            }
            kb_device_backend_destroy(backend);
            return 1;
        }
    }

    free(bzimage_kernel_area);
    free(bzimage_setup_area);
    free_guest_region_backings(bzimage_low_regions, bzimage_low_region_count);
    free(bzimage_data);
    if (block_fixture_initialized) {
        kvm_block_fixture_destroy(&block_fixture);
    }
    free(cmdline_page);
    free(boot_page);
    free(guest_page);
    free(fake_file);
    if (kb_module_call_cleanup(target_module.module) == KB_OK) {
        printf("target cleanup_module returned\n");
    }
    unload_module(&target_module);
    for (size_t i = dep_count; i > 0; i--) {
        if (deps[i - 1].initialized && kb_module_call_cleanup(deps[i - 1].module) == KB_OK) {
            printf("dependency cleanup_module returned path=%s\n", deps[i - 1].path);
        }
        unload_module(&deps[i - 1]);
    }
    kb_device_backend_destroy(backend);
    return 0;
}
