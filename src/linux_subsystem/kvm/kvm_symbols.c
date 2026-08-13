#include "linux_subsystem/kvm/kvm_symbols.h"

#include "kobox/shim.h"
#include "loader/module_context.h"
#include "linux_subsystem/dma/dma.h"
#include "linux_subsystem/kvm/kvm.h"

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

enum {
    KB_KVM_MISC_NAME_OFFSET = 0x8,
    KB_KVM_MISC_FOPS_OFFSET = 0x10,
    KB_KVM_FOPS_UNLOCKED_IOCTL_OFFSET = 0x50,
    KB_KVM_FILE_PRIVATE_DATA_OFFSET = 0x18,
    KB_KVM_FAKE_FILE_BYTES = 512,
    KB_KVM_X86_OPS_VM_SIZE_OFFSET = 0x3c,
    KB_KVM_VM_STORAGE_BYTES = 8192,
    KB_KVM_VCPU_RUN_OFFSET = 0x68,
    KB_KVM_VCPU_LAPIC_OFFSET = 0x228,
    KB_KVM_VCPU_ARCH_APIC_OFFSET = 0x810,
    KB_KVM_APIC_REGS_OFFSET = 0xa8,
    KB_KVM_VM_APICV_STATE_OFFSET = 0x1851,
    KB_KVM_FAKE_APIC_BYTES = 512,
    KB_KVM_RUN_STORAGE_BYTES = 16384,
    KB_KVM_RUN_EXIT_REASON_OFFSET = 0x8,
    KB_KVM_RUN_READY_FOR_INTERRUPT_OFFSET = 0xc,
    KB_KVM_RUN_IF_FLAG_OFFSET = 0xd,
    KB_KVM_RUN_FLAGS_OFFSET = 0xe,
    KB_KVM_RUN_CR8_OFFSET = 0x10,
    KB_KVM_RUN_APIC_BASE_OFFSET = 0x18,
    KB_KVM_RUN_IO_DIRECTION_OFFSET = 0x20,
    KB_KVM_RUN_IO_SIZE_OFFSET = 0x21,
    KB_KVM_RUN_IO_PORT_OFFSET = 0x22,
    KB_KVM_RUN_IO_COUNT_OFFSET = 0x24,
    KB_KVM_RUN_IO_DATA_OFFSET_OFFSET = 0x28,
    KB_KVM_RUN_MMIO_PHYS_ADDR_OFFSET = 0x20,
    KB_KVM_RUN_MMIO_DATA_OFFSET = 0x28,
    KB_KVM_RUN_MMIO_LEN_OFFSET = 0x30,
    KB_KVM_RUN_MMIO_IS_WRITE_OFFSET = 0x34,
    KB_KVM_RUN_EXIT_IO_DATA_OFFSET = 0x100,
    KB_KVM_EXIT_IO = 2,
    KB_KVM_EXIT_HLT = 5,
    KB_KVM_EXIT_MMIO = 6,
    KB_KVM_MISC_RECORD_MAX = 16,
    KB_KVM_FD_RECORD_MAX = 32,
    KB_KVM_MEMORY_SLOT_MAX = 16,
    KB_KVM_FD_BASE = 100,
    KB_KVM_PAGE_SHIFT = 12,
    KB_KVM_PAGE_SIZE = 1u << KB_KVM_PAGE_SHIFT,
    KB_KVM_STRUCT_PAGE_SIZE = 64,
    KB_KVM_PAGE_RECORD_MAX = 4096,
    KB_KVM_PAGE_PAYLOAD_BYTES = KB_KVM_PAGE_RECORD_MAX * KB_KVM_PAGE_SIZE,
    KB_KVM_PAGE_RECORD_BYTES = KB_KVM_PAGE_RECORD_MAX * KB_KVM_STRUCT_PAGE_SIZE,
    KB_KVM_PAGE_ALLOC_HEAD = 0x80,
    KB_KVM_PAGE_ALLOC_TAIL = 0x40,
    KB_KVM_PAGE_ALLOC_ORDER_MASK = 0x0f,
};

static const unsigned int KB_KVM_GET_REGS_IOCTL = 0x8090ae81u;
static const unsigned int KB_KVM_SET_REGS_IOCTL = 0x4090ae82u;
static const unsigned int KB_KVM_GET_SREGS_IOCTL = 0x8138ae83u;
static const unsigned int KB_KVM_SET_SREGS_IOCTL = 0x4138ae84u;
static const unsigned int KB_KVM_SET_CPUID2_IOCTL = 0x4008ae90u;
static const unsigned int KB_KVM_SET_USER_MEMORY_REGION_IOCTL = 0x4020ae46u;
static const unsigned int KB_KVM_CREATE_VM_IOCTL = 0xae01u;
static const unsigned int KB_KVM_GET_VCPU_MMAP_SIZE_IOCTL = 0xae04u;
static const unsigned int KB_KVM_CREATE_VCPU_IOCTL = 0xae41u;
static const unsigned int KB_KVM_SET_TSS_ADDR_IOCTL = 0xae47u;
static const unsigned int KB_KVM_CREATE_IRQCHIP_IOCTL = 0xae60u;
static const unsigned int KB_KVM_IRQ_LINE_IOCTL = 0x4008ae61u;
static const unsigned int KB_KVM_RUN_IOCTL = 0xae80u;

enum {
    KB_KVM_PENDING_NONE = 0,
    KB_KVM_PENDING_IO_IN = 1,
    KB_KVM_PENDING_MMIO_READ = 2,
};

typedef struct kb_kvm_userspace_memory_region {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
} kb_kvm_userspace_memory_region_t;

typedef struct kb_kvm_regs_shadow {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t rflags;
} kb_kvm_regs_shadow_t;

typedef struct kb_kvm_misc_record {
    int registered;
    void *misc_device;
    const char *name;
    void *fops;
    void *unlocked_ioctl;
} kb_kvm_misc_record_t;

typedef struct kb_kvm_fd_record {
    int active;
    int installed;
    int fd;
    char name_storage[64];
    const char *name;
    void *file;
    void *private_data;
    void *fops;
    void *unlocked_ioctl;
    void *original_unlocked_ioctl;
    int has_memory_region;
    kb_kvm_userspace_memory_region_t memory_region;
    kb_kvm_userspace_memory_region_t memory_regions[KB_KVM_MEMORY_SLOT_MAX];
    unsigned char memory_region_active[KB_KVM_MEMORY_SLOT_MAX];
    int has_regs;
    kb_kvm_regs_shadow_t regs;
    int pending_exit;
    int host_fd;
    void *host_run;
    size_t host_run_size;
} kb_kvm_fd_record_t;

void kb_linux_kvm_put_unused_fd(int fd);
long kb_linux_kvm_vm_ioctl_bridge(void *file, unsigned int cmd, unsigned long arg);
long kb_linux_kvm_vcpu_ioctl_bridge(void *file, unsigned int cmd, unsigned long arg);
int kb_kvm_vcpu_run_exit(void *vcpu, kb_kvm_fd_record_t *vcpu_record);
static kb_kvm_fd_record_t *find_vm_record_for_vcpu(void *vcpu);

static kb_kvm_misc_record_t misc_records[KB_KVM_MISC_RECORD_MAX];
static kb_kvm_fd_record_t fd_records[KB_KVM_FD_RECORD_MAX];
static int kb_kvm_next_fd = KB_KVM_FD_BASE;
static int kb_kvm_host_fd = -1;
static size_t kb_kvm_host_vcpu_mmap_size;
static unsigned char *kb_kvm_page_payloads;
static unsigned char *kb_kvm_page_records;
static void *kb_kvm_page_payloads_alloc;
static void *kb_kvm_page_records_alloc;
static unsigned char kb_kvm_page_alloc_state[KB_KVM_PAGE_RECORD_MAX];
static kb_device_backend_t *kb_kvm_dma_arena_backend;
static int kb_kvm_dma_arena_mapped;

uint64_t kb_kvm_empty_zero_page[512];
uint8_t kb_kvm_boot_cpu_data[1024] = {
    [2] = 2,      /* X86_VENDOR_AMD */
    [0x32] = 0x10 /* X86_FEATURE_NX in boot_cpu_data+0x2c bit 52 */
};
uint8_t kb_kvm_cpu_info[1024] = {
    [2] = 2,    /* X86_VENDOR_AMD */
    [0x48] = 4 /* X86_FEATURE_SVM */
};
uint64_t kb_kvm_const_current_task[64];
uint64_t kb_kvm_kmalloc_caches[64];
uint64_t kb_kvm_system_wq;
uint64_t kb_kvm_system_long_wq;
uint64_t kb_kvm_system_percpu_wq;
uint64_t kb_kvm_param_ops_bool;
uint64_t kb_kvm_param_ops_bint;
uint64_t kb_kvm_param_ops_int;
uint64_t kb_kvm_param_ops_uint;
uint64_t kb_kvm_pv_ops;
uint64_t kb_kvm_smp_ops;
uint64_t kb_kvm_cpu_dr7;
uint64_t kb_kvm_mem_section;
uint64_t kb_kvm_page_offset_base;
uint64_t kb_kvm_phys_base;
uint64_t kb_kvm_physical_mask = UINT64_MAX;
uint64_t kb_kvm_vmemmap_base;
static uint64_t kb_kvm_page_payload_base;
static uint64_t kb_kvm_vmemmap_record_base;
static int kb_kvm_phys_base_valid;
uint64_t kb_kvm_pgdir_shift = 39;
uint64_t kb_kvm_ptrs_per_p4d = 512;
uint64_t kb_kvm_tsc_khz = 1000000;
uint64_t kb_kvm_x86_hyper_type;
uint64_t kb_kvm_system_state;
uint64_t kb_kvm_cpu_entry_area[512];

static int trace_kvm_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_TRACE_KVM");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    return cached;
}

static int linux_kvm_provider_enabled(void)
{
    const char *backend = getenv("KOBOX_KVM_RUN_BACKEND");
    return backend != NULL && strcmp(backend, "linux-kvm") == 0;
}

static void *kb_kvm_alloc_aligned_zeroed(size_t size, size_t alignment, void **allocation)
{
    if (allocation == NULL) {
        return NULL;
    }
    *allocation = NULL;
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0) {
        return NULL;
    }
    if (size > SIZE_MAX - alignment + 1u) {
        return NULL;
    }

    void *raw = calloc(1, size + alignment - 1u);
    if (raw == NULL) {
        return NULL;
    }

    const uintptr_t aligned =
        ((uintptr_t)raw + (uintptr_t)alignment - 1u) & ~((uintptr_t)alignment - 1u);
    *allocation = raw;
    return (void *)aligned;
}

__attribute__((weak)) void *kb_kvm_host_alloc_dma_arena(size_t bytes)
{
    (void)bytes;
    return NULL;
}

static int kb_kvm_ensure_page_arena(void)
{
    if (kb_kvm_page_payloads != NULL && kb_kvm_page_records != NULL) {
        return 0;
    }

    void *payloads_alloc = NULL;
    void *records_alloc = NULL;
    void *records = kb_kvm_alloc_aligned_zeroed(
        KB_KVM_PAGE_RECORD_BYTES, KB_KVM_STRUCT_PAGE_SIZE, &records_alloc);
    if (records == NULL) {
        return -ENOMEM;
    }
    void *payloads = kb_kvm_host_alloc_dma_arena(KB_KVM_PAGE_PAYLOAD_BYTES);
    if (payloads == NULL) {
        payloads = kb_kvm_alloc_aligned_zeroed(
            KB_KVM_PAGE_PAYLOAD_BYTES, KB_KVM_PAGE_SIZE, &payloads_alloc);
    }
    if (payloads == NULL) {
        free(records_alloc);
        return -ENOMEM;
    }
    memset(kb_kvm_page_alloc_state, 0, sizeof(kb_kvm_page_alloc_state));
    kb_kvm_page_payloads = payloads;
    kb_kvm_page_records = records;
    kb_kvm_page_payloads_alloc = payloads_alloc;
    kb_kvm_page_records_alloc = records_alloc;
    return 0;
}

static long host_kvm_ioctl(int fd, unsigned int cmd, unsigned long arg)
{
#if !defined(_WIN32)
    return ioctl(fd, cmd, arg);
#else
    (void)fd;
    (void)cmd;
    (void)arg;
    return -95;
#endif
}

static int ensure_host_kvm_open(void)
{
#if !defined(_WIN32)
    if (kb_kvm_host_fd >= 0) {
        return 0;
    }
    int fd = open("/dev/kvm", O_RDWR);
    if (fd < 0) {
        return -errno;
    }
    long mmap_size = host_kvm_ioctl(fd, KB_KVM_GET_VCPU_MMAP_SIZE_IOCTL, 0);
    if (mmap_size <= 0) {
        int saved_errno = errno;
        close(fd);
        return saved_errno == 0 ? -22 : -saved_errno;
    }
    kb_kvm_host_fd = fd;
    kb_kvm_host_vcpu_mmap_size = (size_t)mmap_size;
    if (trace_kvm_enabled()) {
        fprintf(stderr, "kobox-kvm: linux-kvm provider opened fd=%d vcpu_mmap_size=%zu\n", fd, kb_kvm_host_vcpu_mmap_size);
    }
    return 0;
#else
    return -95;
#endif
}

static void close_host_fd_record(kb_kvm_fd_record_t *record)
{
#if !defined(_WIN32)
    if (record == NULL) {
        return;
    }
    if (record->host_run != NULL && record->host_run_size != 0) {
        munmap(record->host_run, record->host_run_size);
    }
    if (record->host_fd >= 0) {
        close(record->host_fd);
    }
#else
    (void)record;
#endif
}

static int init_host_vm_record(kb_kvm_fd_record_t *record)
{
    if (!linux_kvm_provider_enabled() || record == NULL) {
        return 0;
    }
    int open_result = ensure_host_kvm_open();
    if (open_result != 0) {
        return open_result;
    }
    long host_vm_fd = host_kvm_ioctl(kb_kvm_host_fd, KB_KVM_CREATE_VM_IOCTL, 0);
    if (host_vm_fd < 0) {
        return errno == 0 ? (int)host_vm_fd : -errno;
    }
    record->host_fd = (int)host_vm_fd;
    (void)host_kvm_ioctl(record->host_fd, KB_KVM_SET_TSS_ADDR_IOCTL, 0xfffbd000UL);
    if (trace_kvm_enabled()) {
        fprintf(stderr, "kobox-kvm: linux-kvm vm mirror fd=%d\n", record->host_fd);
    }
    return 0;
}

static int vcpu_id_from_name(const char *name)
{
    if (name == NULL || strncmp(name, "kvm-vcpu:", 9) != 0) {
        return 0;
    }
    return (int)strtol(name + 9, NULL, 10);
}

static int init_host_vcpu_record(kb_kvm_fd_record_t *record, const char *name, void *private_data)
{
    if (!linux_kvm_provider_enabled() || record == NULL || private_data == NULL) {
        return 0;
    }
    kb_kvm_fd_record_t *vm_record = find_vm_record_for_vcpu(private_data);
    if (vm_record == NULL || vm_record->host_fd < 0) {
        return -2;
    }
    long host_vcpu_fd = host_kvm_ioctl(vm_record->host_fd, KB_KVM_CREATE_VCPU_IOCTL, (unsigned long)vcpu_id_from_name(name));
    if (host_vcpu_fd < 0) {
        return errno == 0 ? (int)host_vcpu_fd : -errno;
    }
    record->host_fd = (int)host_vcpu_fd;
#if !defined(_WIN32)
    record->host_run_size = kb_kvm_host_vcpu_mmap_size;
    record->host_run = mmap(NULL, record->host_run_size, PROT_READ | PROT_WRITE, MAP_SHARED, record->host_fd, 0);
    if (record->host_run == MAP_FAILED) {
        int saved_errno = errno;
        close(record->host_fd);
        record->host_fd = -1;
        record->host_run = NULL;
        record->host_run_size = 0;
        return -saved_errno;
    }
#endif
    if (trace_kvm_enabled()) {
        fprintf(stderr,
            "kobox-kvm: linux-kvm vcpu mirror fd=%d run=%p size=%zu\n",
            record->host_fd,
            record->host_run,
            record->host_run_size);
    }
    return 0;
}

static void kb_kvm_sync_page_model(void)
{
    if (kb_kvm_ensure_page_arena() != 0) {
        kb_kvm_page_payload_base = 0;
        kb_kvm_page_offset_base = 0;
        kb_kvm_vmemmap_record_base = 0;
        kb_kvm_vmemmap_base = 0;
        kb_kvm_phys_base_valid = 0;
        return;
    }
    kb_kvm_page_payload_base = (uint64_t)(uintptr_t)kb_kvm_page_payloads;
    /* Linux's direct-map helpers intentionally do unsigned address arithmetic:
     *
     *   page_address(page) = page_offset_base + page_to_pfn(page) * PAGE_SIZE
     *
     * A device IOVA is allowed to be numerically above the userspace virtual
     * arena.  Preserve the modular subtraction instead of clamping it; the
     * later addition then wraps back to the payload virtual address. */
    kb_kvm_page_offset_base = kb_kvm_page_payload_base - kb_kvm_phys_base;
    kb_kvm_vmemmap_record_base = (uint64_t)(uintptr_t)kb_kvm_page_records;
    uint64_t phys_pfn = kb_kvm_phys_base >> KB_KVM_PAGE_SHIFT;
    uint64_t adjust = phys_pfn * KB_KVM_STRUCT_PAGE_SIZE;
    kb_kvm_vmemmap_base = kb_kvm_vmemmap_record_base - adjust;
}

uintptr_t kb_linux_kvm_page_offset_base(void)
{
    kb_kvm_sync_page_model();
    return (uintptr_t)kb_kvm_page_payload_base;
}

uintptr_t kb_linux_kvm_exported_page_offset_base(void)
{
    kb_kvm_sync_page_model();
    return (uintptr_t)kb_kvm_page_offset_base;
}

uintptr_t kb_linux_kvm_vmemmap_base(void)
{
    kb_kvm_sync_page_model();
    return (uintptr_t)kb_kvm_vmemmap_record_base;
}

uintptr_t kb_linux_kvm_exported_vmemmap_base(void)
{
    kb_kvm_sync_page_model();
    return (uintptr_t)kb_kvm_vmemmap_base;
}

uintptr_t kb_linux_kvm_phys_base(void)
{
    kb_kvm_sync_page_model();
    return (uintptr_t)kb_kvm_phys_base;
}

int kb_linux_kvm_payload_dma_addr(const void *cpu_addr, size_t size, uint64_t *out_dma_addr)
{
    if (cpu_addr == NULL || size == 0 || out_dma_addr == NULL) {
        return 0;
    }
    kb_kvm_sync_page_model();
    uintptr_t base = (uintptr_t)kb_kvm_page_payloads;
    uintptr_t addr = (uintptr_t)cpu_addr;
    size_t arena_size = KB_KVM_PAGE_PAYLOAD_BYTES;
    if (addr < base || addr - base >= arena_size) {
        return 0;
    }
    size_t offset = (size_t)(addr - base);
    if (size > arena_size - offset) {
        return 0;
    }
    if (!kb_kvm_phys_base_valid) {
        return 0;
    }
    *out_dma_addr = kb_kvm_phys_base + offset;
    return 1;
}

int kb_linux_kvm_dma_addr_in_payload_arena(uint64_t dma_addr, size_t size)
{
    if (dma_addr == 0 || size == 0) {
        return 0;
    }
    kb_kvm_sync_page_model();
    if (!kb_kvm_phys_base_valid || dma_addr < kb_kvm_phys_base) {
        return 0;
    }
    uint64_t offset = dma_addr - kb_kvm_phys_base;
    uint64_t arena_size = KB_KVM_PAGE_PAYLOAD_BYTES;
    return offset < arena_size && size <= arena_size - offset;
}

void *kb_linux_kvm_page_payload(void *page, unsigned long offset, size_t size)
{
    if (page == NULL || size == 0) {
        return 0;
    }
    if (offset >= KB_KVM_PAGE_SIZE) {
        return 0;
    }
    kb_kvm_sync_page_model();
    uintptr_t page_base = (uintptr_t)kb_kvm_page_records;
    uintptr_t page_addr = (uintptr_t)page;
    size_t records_size = KB_KVM_PAGE_RECORD_BYTES;
    if (page_addr < page_base || page_addr - page_base >= records_size) {
        return 0;
    }
    size_t record_offset = (size_t)(page_addr - page_base);
    if ((record_offset % KB_KVM_STRUCT_PAGE_SIZE) != 0) {
        return 0;
    }
    size_t page_index = record_offset / KB_KVM_STRUCT_PAGE_SIZE;
    if (page_index >= KB_KVM_PAGE_RECORD_MAX) {
        return 0;
    }
    const size_t span = offset + size;
    if (span < size) {
        return 0;
    }
    const size_t page_count = (span + KB_KVM_PAGE_SIZE - 1u) / KB_KVM_PAGE_SIZE;
    if (page_count == 0 || page_count > KB_KVM_PAGE_RECORD_MAX - page_index) {
        return 0;
    }
    for (size_t index = 0; index < page_count; index++) {
        if (kb_kvm_page_alloc_state[page_index + index] == 0) {
            return 0;
        }
    }
    return kb_kvm_page_payloads + (page_index * KB_KVM_PAGE_SIZE) + offset;
}

int kb_linux_kvm_page_payload_dma_addr(
    void *page,
    unsigned long offset,
    size_t size,
    void **out_cpu_addr,
    uint64_t *out_dma_addr)
{
    if (out_cpu_addr == NULL || out_dma_addr == NULL) {
        return 0;
    }
    void *cpu_addr = kb_linux_kvm_page_payload(page, offset, size);
    if (cpu_addr == NULL) {
        return 0;
    }
    uint64_t dma_addr = 0;
    if (!kb_linux_kvm_payload_dma_addr(cpu_addr, size, &dma_addr)) {
        return 0;
    }
    *out_cpu_addr = cpu_addr;
    *out_dma_addr = dma_addr;
    return 1;
}

static void *read_ptr_field(const void *base, size_t offset)
{
    void *value = NULL;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint32_t read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint16_t read_u16_field(const void *base, size_t offset)
{
    uint16_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint8_t read_u8_field(const void *base, size_t offset)
{
    uint8_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint64_t read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static void write_u8_field(void *base, size_t offset, uint8_t value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void write_u16_field(void *base, size_t offset, uint16_t value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void write_u32_field(void *base, size_t offset, uint32_t value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void write_u64_field(void *base, size_t offset, uint64_t value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static kb_kvm_fd_record_t *find_fd_record(int fd)
{
    for (size_t i = 0; i < KB_KVM_FD_RECORD_MAX; i++) {
        if (fd_records[i].active && fd_records[i].fd == fd) {
            return &fd_records[i];
        }
    }
    return NULL;
}

static kb_kvm_fd_record_t *find_fd_record_by_file(const void *file)
{
    for (size_t i = 0; i < KB_KVM_FD_RECORD_MAX; i++) {
        if (fd_records[i].active && fd_records[i].file == file) {
            return &fd_records[i];
        }
    }
    return NULL;
}

static kb_kvm_fd_record_t *find_fd_record_by_private_data(const void *private_data)
{
    for (size_t i = 0; i < KB_KVM_FD_RECORD_MAX; i++) {
        if (fd_records[i].active && fd_records[i].private_data == private_data) {
            return &fd_records[i];
        }
    }
    return NULL;
}

static kb_kvm_fd_record_t *alloc_fd_record(void)
{
    for (size_t i = 0; i < KB_KVM_FD_RECORD_MAX; i++) {
        if (!fd_records[i].active) {
            memset(&fd_records[i], 0, sizeof(fd_records[i]));
            fd_records[i].active = 1;
            fd_records[i].host_fd = -1;
            return &fd_records[i];
        }
    }
    return NULL;
}

static long call_original_ioctl(kb_kvm_fd_record_t *record, void *file, unsigned int cmd, unsigned long arg)
{
    if (record == NULL || record->original_unlocked_ioctl == NULL) {
        return -25;
    }
    long (*fn)(void *, unsigned int, unsigned long) = NULL;
    memcpy(&fn, &record->original_unlocked_ioctl, sizeof(fn));
    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(record->original_unlocked_ioctl);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long result = fn(file, cmd, arg);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static void fixup_kvm_x86_ops_vm_size(void)
{
    void *kvm_x86_ops = kb_module_lookup_exported_symbol("kvm_x86_ops");
    if (kvm_x86_ops == NULL) {
        return;
    }
    unsigned char *vm_size_field = (unsigned char *)kvm_x86_ops + KB_KVM_X86_OPS_VM_SIZE_OFFSET;
    uint32_t vm_size = 0;
    memcpy(&vm_size, vm_size_field, sizeof(vm_size));
    if (vm_size >= KB_KVM_VM_STORAGE_BYTES) {
        return;
    }
    vm_size = KB_KVM_VM_STORAGE_BYTES;
    memcpy(vm_size_field, &vm_size, sizeof(vm_size));
    if (trace_kvm_enabled()) {
        fprintf(stderr,
            "kobox-kvm: kvm_x86_ops vm_size fixed address=%p vm_size=%u\n",
            kvm_x86_ops,
            vm_size);
    }
}

static const char *copy_fd_name(kb_kvm_fd_record_t *record, const char *name)
{
    if (record == NULL || name == NULL) {
        return name;
    }
    size_t len = strlen(name);
    if (len >= sizeof(record->name_storage)) {
        len = sizeof(record->name_storage) - 1;
    }
    memcpy(record->name_storage, name, len);
    record->name_storage[len] = '\0';
    return record->name_storage;
}

static void prepare_vcpu_private_data(const char *name, void *private_data)
{
    if (name == NULL || private_data == NULL || strncmp(name, "kvm-vcpu:", 9) != 0) {
        return;
    }
    unsigned char *vcpu = (unsigned char *)private_data;
    void *run = read_ptr_field(vcpu, KB_KVM_VCPU_RUN_OFFSET);
    if (run == NULL) {
        run = kb_kzalloc(KB_KVM_RUN_STORAGE_BYTES, 0);
        memcpy(vcpu + KB_KVM_VCPU_RUN_OFFSET, &run, sizeof(run));
    }
    void *apic = read_ptr_field(vcpu, KB_KVM_VCPU_LAPIC_OFFSET);
    if (apic == NULL) {
        apic = read_ptr_field(vcpu, KB_KVM_VCPU_ARCH_APIC_OFFSET);
    }
    if (apic == NULL) {
        apic = kb_kzalloc(KB_KVM_FAKE_APIC_BYTES, 0);
    }
    memcpy(vcpu + KB_KVM_VCPU_LAPIC_OFFSET, &apic, sizeof(apic));
    memcpy(vcpu + KB_KVM_VCPU_ARCH_APIC_OFFSET, &apic, sizeof(apic));
    void *apic_regs = read_ptr_field(apic, KB_KVM_APIC_REGS_OFFSET);
    if (apic_regs == NULL) {
        apic_regs = apic;
        memcpy((unsigned char *)apic + KB_KVM_APIC_REGS_OFFSET, &apic_regs, sizeof(apic_regs));
    }
    void *vm = read_ptr_field(vcpu, 0);
    if (vm != NULL) {
        ((unsigned char *)vm)[KB_KVM_VM_APICV_STATE_OFFSET] = 1;
    }
    if (trace_kvm_enabled()) {
        fprintf(stderr, "kobox-kvm: vcpu prepared vcpu=%p vm=%p run=%p apic=%p\n", private_data, vm, run, apic);
    }
}

int kb_linux_kvm_misc_register(void *misc)
{
    if (misc == NULL) {
        return -22;
    }
    const char *name = read_ptr_field(misc, KB_KVM_MISC_NAME_OFFSET);
    void *fops = read_ptr_field(misc, KB_KVM_MISC_FOPS_OFFSET);
    void *unlocked_ioctl = read_ptr_field(fops, KB_KVM_FOPS_UNLOCKED_IOCTL_OFFSET);

    for (size_t i = 0; i < KB_KVM_MISC_RECORD_MAX; i++) {
        if (misc_records[i].misc_device != NULL && misc_records[i].misc_device != misc) {
            continue;
        }
        misc_records[i].registered = 1;
        misc_records[i].misc_device = misc;
        misc_records[i].name = name;
        misc_records[i].fops = fops;
        misc_records[i].unlocked_ioctl = unlocked_ioctl;
        fixup_kvm_x86_ops_vm_size();
        if (trace_kvm_enabled()) {
            fprintf(stderr,
                "kobox-kvm: misc_register name=%s misc=%p fops=%p ioctl=%p\n",
                name == NULL ? "(null)" : name,
                misc,
                fops,
                unlocked_ioctl);
        }
        return 0;
    }
    return -12;
}

int kb_linux_kvm_misc_deregister(void *misc)
{
    for (size_t i = 0; i < KB_KVM_MISC_RECORD_MAX; i++) {
        if (misc_records[i].misc_device == misc) {
            misc_records[i].registered = 0;
            return 0;
        }
    }
    return 0;
}

int kb_linux_kvm_misc_snapshot(const char *name, kb_kvm_misc_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return -22;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    for (size_t i = 0; i < KB_KVM_MISC_RECORD_MAX; i++) {
        if (misc_records[i].misc_device == NULL) {
            continue;
        }
        if (name != NULL && (misc_records[i].name == NULL || strcmp(misc_records[i].name, name) != 0)) {
            continue;
        }
        out_snapshot->misc_device = misc_records[i].misc_device;
        out_snapshot->name = misc_records[i].name;
        out_snapshot->fops = misc_records[i].fops;
        out_snapshot->unlocked_ioctl = misc_records[i].unlocked_ioctl;
        out_snapshot->registered = misc_records[i].registered;
        return 0;
    }
    return -2;
}

int kb_linux_kvm_fd_snapshot(int fd, kb_kvm_fd_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return -22;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    for (size_t i = 0; i < KB_KVM_FD_RECORD_MAX; i++) {
        if (!fd_records[i].active || !fd_records[i].installed || fd_records[i].fd != fd) {
            continue;
        }
        out_snapshot->fd = fd_records[i].fd;
        out_snapshot->name = fd_records[i].name;
        out_snapshot->file = fd_records[i].file;
        out_snapshot->private_data = fd_records[i].private_data;
        out_snapshot->fops = fd_records[i].fops;
        out_snapshot->unlocked_ioctl = fd_records[i].unlocked_ioctl;
        out_snapshot->active = fd_records[i].active;
        return 0;
    }
    return -2;
}

int kb_linux_kvm_run_snapshot(int vcpu_fd, kb_kvm_run_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return -22;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    kb_kvm_fd_record_t *record = find_fd_record(vcpu_fd);
    if (record == NULL || !record->installed || record->private_data == NULL) {
        return -2;
    }
    if (record->name == NULL || strncmp(record->name, "kvm-vcpu:", 9) != 0) {
        return -22;
    }
    void *run = read_ptr_field(record->private_data, KB_KVM_VCPU_RUN_OFFSET);
    if (run == NULL) {
        return -2;
    }
    out_snapshot->run = run;
    out_snapshot->exit_reason = read_u32_field(run, KB_KVM_RUN_EXIT_REASON_OFFSET);
    out_snapshot->ready_for_interrupt_injection = read_u8_field(run, KB_KVM_RUN_READY_FOR_INTERRUPT_OFFSET);
    out_snapshot->if_flag = read_u8_field(run, KB_KVM_RUN_IF_FLAG_OFFSET);
    out_snapshot->flags = read_u16_field(run, KB_KVM_RUN_FLAGS_OFFSET);
    out_snapshot->cr8 = read_u64_field(run, KB_KVM_RUN_CR8_OFFSET);
    out_snapshot->apic_base = read_u64_field(run, KB_KVM_RUN_APIC_BASE_OFFSET);
    out_snapshot->io_direction = read_u8_field(run, KB_KVM_RUN_IO_DIRECTION_OFFSET);
    out_snapshot->io_size = read_u8_field(run, KB_KVM_RUN_IO_SIZE_OFFSET);
    out_snapshot->io_port = read_u16_field(run, KB_KVM_RUN_IO_PORT_OFFSET);
    out_snapshot->io_count = read_u32_field(run, KB_KVM_RUN_IO_COUNT_OFFSET);
    out_snapshot->io_data_offset = read_u64_field(run, KB_KVM_RUN_IO_DATA_OFFSET_OFFSET);
    if (out_snapshot->io_data_offset + sizeof(out_snapshot->io_data) <= KB_KVM_RUN_STORAGE_BYTES) {
        memcpy(out_snapshot->io_data, (const unsigned char *)run + out_snapshot->io_data_offset, sizeof(out_snapshot->io_data));
    }
    out_snapshot->mmio_phys_addr = read_u64_field(run, KB_KVM_RUN_MMIO_PHYS_ADDR_OFFSET);
    memcpy(out_snapshot->mmio_data, (const unsigned char *)run + KB_KVM_RUN_MMIO_DATA_OFFSET, sizeof(out_snapshot->mmio_data));
    out_snapshot->mmio_len = read_u32_field(run, KB_KVM_RUN_MMIO_LEN_OFFSET);
    out_snapshot->mmio_is_write = read_u8_field(run, KB_KVM_RUN_MMIO_IS_WRITE_OFFSET);
    return 0;
}

static size_t host_run_copy_size(const kb_kvm_fd_record_t *record)
{
    if (record == NULL || record->host_run_size == 0 || record->host_run_size > KB_KVM_RUN_STORAGE_BYTES) {
        return KB_KVM_RUN_STORAGE_BYTES;
    }
    return record->host_run_size;
}

static long host_kvm_run_vcpu(kb_kvm_fd_record_t *record)
{
    if (record == NULL || record->host_fd < 0 || record->host_run == NULL || record->private_data == NULL) {
        return -22;
    }
    void *run = read_ptr_field(record->private_data, KB_KVM_VCPU_RUN_OFFSET);
    if (run == NULL) {
        return -22;
    }
    size_t copy_size = host_run_copy_size(record);
    memcpy(record->host_run, run, copy_size);
    long result = host_kvm_ioctl(record->host_fd, KB_KVM_RUN_IOCTL, 0);
    memcpy(run, record->host_run, copy_size);
    if (result == 0) {
        (void)host_kvm_ioctl(record->host_fd, KB_KVM_GET_REGS_IOCTL, (unsigned long)(uintptr_t)&record->regs);
        record->has_regs = 1;
    }
    return result;
}

long kb_linux_kvm_vm_ioctl_bridge(void *file, unsigned int cmd, unsigned long arg)
{
    kb_kvm_fd_record_t *record = find_fd_record_by_file(file);
    if (record == NULL || record->private_data == NULL) {
        return -22;
    }
    if (linux_kvm_provider_enabled() &&
        record->host_fd >= 0 &&
        (cmd == KB_KVM_CREATE_IRQCHIP_IOCTL || cmd == KB_KVM_IRQ_LINE_IOCTL))
    {
        long host_result = host_kvm_ioctl(record->host_fd, cmd, arg);
        if (host_result < 0) {
            return errno == 0 ? host_result : -errno;
        }
        return host_result;
    }
    long result = call_original_ioctl(record, file, cmd, arg);
    if (result >= 0 && cmd == KB_KVM_SET_USER_MEMORY_REGION_IOCTL && arg != 0) {
        kb_kvm_userspace_memory_region_t region;
        memcpy(&region, (const void *)(uintptr_t)arg, sizeof(region));
        record->memory_region = region;
        record->has_memory_region = record->memory_region.memory_size != 0;
        if (region.slot < KB_KVM_MEMORY_SLOT_MAX) {
            record->memory_regions[region.slot] = region;
            record->memory_region_active[region.slot] = region.memory_size != 0;
        }
        if (record->host_fd >= 0) {
            long host_result = host_kvm_ioctl(record->host_fd, cmd, arg);
            if (host_result < 0) {
                return errno == 0 ? host_result : -errno;
            }
        }
    }
    return result;
}

long kb_linux_kvm_vcpu_ioctl_bridge(void *file, unsigned int cmd, unsigned long arg)
{
    kb_kvm_fd_record_t *record = find_fd_record_by_file(file);
    if (record == NULL || record->private_data == NULL) {
        return -22;
    }
    if (cmd == KB_KVM_RUN_IOCTL) {
        if (linux_kvm_provider_enabled() && record->host_fd >= 0) {
            return host_kvm_run_vcpu(record);
        }
        return kb_kvm_vcpu_run_exit(record->private_data, record);
    }
    if (cmd == KB_KVM_SET_CPUID2_IOCTL &&
        linux_kvm_provider_enabled() &&
        record->host_fd >= 0 &&
        arg != 0)
    {
        long host_result = host_kvm_ioctl(record->host_fd, cmd, arg);
        if (host_result < 0) {
            return errno == 0 ? host_result : -errno;
        }
        return host_result;
    }
    if ((cmd == KB_KVM_GET_REGS_IOCTL || cmd == KB_KVM_GET_SREGS_IOCTL) &&
        linux_kvm_provider_enabled() &&
        record->host_fd >= 0 &&
        arg != 0)
    {
        long host_result = host_kvm_ioctl(record->host_fd, cmd, arg);
        if (host_result < 0) {
            return errno == 0 ? host_result : -errno;
        }
        if (cmd == KB_KVM_GET_REGS_IOCTL) {
            memcpy(&record->regs, (const void *)(uintptr_t)arg, sizeof(record->regs));
            record->has_regs = 1;
        }
        return host_result;
    }
    if (cmd == KB_KVM_GET_REGS_IOCTL && record->has_regs && arg != 0) {
        memcpy((void *)(uintptr_t)arg, &record->regs, sizeof(record->regs));
        return 0;
    }
    long result = call_original_ioctl(record, file, cmd, arg);
    if (result >= 0 && cmd == KB_KVM_SET_REGS_IOCTL && arg != 0) {
        memcpy(&record->regs, (const void *)(uintptr_t)arg, sizeof(record->regs));
        record->has_regs = 1;
        record->pending_exit = KB_KVM_PENDING_NONE;
    }
    if (result >= 0 &&
        linux_kvm_provider_enabled() &&
        record->host_fd >= 0 &&
        (cmd == KB_KVM_SET_REGS_IOCTL || cmd == KB_KVM_SET_SREGS_IOCTL) &&
        arg != 0)
    {
        long host_result = host_kvm_ioctl(record->host_fd, cmd, arg);
        if (host_result < 0) {
            return errno == 0 ? host_result : -errno;
        }
    }
    return result;
}

int kb_linux_kvm_get_unused_fd_flags(unsigned int flags)
{
    (void)flags;
    kb_kvm_fd_record_t *record = alloc_fd_record();
    if (record == NULL) {
        return -24;
    }
    record->fd = kb_kvm_next_fd++;
    if (trace_kvm_enabled()) {
        fprintf(stderr, "kobox-kvm: get_unused_fd_flags fd=%d flags=0x%x\n", record->fd, flags);
    }
    return record->fd;
}

void *kb_linux_kvm_anon_inode_getfile(const char *name, const void *fops, void *private_data, int flags)
{
    (void)flags;
    kb_kvm_fd_record_t *record = alloc_fd_record();
    if (record == NULL) {
        return NULL;
    }
    void *file = kb_kzalloc(KB_KVM_FAKE_FILE_BYTES, 0);
    if (file == NULL) {
        memset(record, 0, sizeof(*record));
        return NULL;
    }
    record->host_fd = -1;
    void *unlocked_ioctl = read_ptr_field(fops, KB_KVM_FOPS_UNLOCKED_IOCTL_OFFSET);
    prepare_vcpu_private_data(name, private_data);
    memcpy((unsigned char *)file + KB_KVM_FILE_PRIVATE_DATA_OFFSET, &private_data, sizeof(private_data));
    record->fd = -1;
    record->name = copy_fd_name(record, name);
    record->file = file;
    record->private_data = private_data;
    record->fops = (void *)fops;
    record->original_unlocked_ioctl = unlocked_ioctl;
    if (name != NULL && strncmp(name, "kvm-vcpu:", 9) == 0) {
        record->unlocked_ioctl = (void *)(uintptr_t)&kb_linux_kvm_vcpu_ioctl_bridge;
        int host_result = init_host_vcpu_record(record, name, private_data);
        if (host_result != 0) {
            kb_kfree(file);
            memset(record, 0, sizeof(*record));
            return NULL;
        }
    } else if (name != NULL && strcmp(name, "kvm-vm") == 0) {
        record->unlocked_ioctl = (void *)(uintptr_t)&kb_linux_kvm_vm_ioctl_bridge;
        int host_result = init_host_vm_record(record);
        if (host_result != 0) {
            kb_kfree(file);
            memset(record, 0, sizeof(*record));
            return NULL;
        }
    } else {
        record->unlocked_ioctl = unlocked_ioctl;
    }
    if (trace_kvm_enabled()) {
        fprintf(stderr,
            "kobox-kvm: anon_inode_getfile name=%s file=%p private=%p fops=%p ioctl=%p\n",
            name == NULL ? "(null)" : name,
            file,
            private_data,
            fops,
            record->unlocked_ioctl);
    }
    return file;
}

void *kb_linux_kvm_anon_inode_getfile_fmode(const char *name, const void *fops, void *private_data, int flags, int fmode)
{
    (void)fmode;
    return kb_linux_kvm_anon_inode_getfile(name, fops, private_data, flags);
}

void kb_linux_kvm_fd_install(int fd, void *file)
{
    kb_kvm_fd_record_t *reserved = find_fd_record(fd);
    kb_kvm_fd_record_t *file_record = find_fd_record_by_file(file);
    if (reserved == NULL) {
        reserved = alloc_fd_record();
        if (reserved == NULL) {
            return;
        }
        reserved->fd = fd;
    }
    if (file_record != NULL && file_record != reserved) {
        reserved->name = copy_fd_name(reserved, file_record->name);
        reserved->file = file_record->file;
        reserved->private_data = file_record->private_data;
        reserved->fops = file_record->fops;
        reserved->unlocked_ioctl = file_record->unlocked_ioctl;
        reserved->original_unlocked_ioctl = file_record->original_unlocked_ioctl;
        reserved->has_memory_region = file_record->has_memory_region;
        reserved->memory_region = file_record->memory_region;
        memcpy(reserved->memory_regions, file_record->memory_regions, sizeof(reserved->memory_regions));
        memcpy(reserved->memory_region_active, file_record->memory_region_active, sizeof(reserved->memory_region_active));
        reserved->has_regs = file_record->has_regs;
        reserved->regs = file_record->regs;
        reserved->pending_exit = file_record->pending_exit;
        reserved->host_fd = file_record->host_fd;
        reserved->host_run = file_record->host_run;
        reserved->host_run_size = file_record->host_run_size;
        memset(file_record, 0, sizeof(*file_record));
    } else if (reserved->file == NULL) {
        reserved->file = file;
    }
    reserved->installed = 1;
    if (trace_kvm_enabled()) {
        fprintf(stderr,
            "kobox-kvm: fd_install fd=%d file=%p private=%p fops=%p ioctl=%p\n",
            fd,
            reserved->file,
            reserved->private_data,
            reserved->fops,
            reserved->unlocked_ioctl);
    }
}

int kb_linux_kvm_anon_inode_getfd(const char *name, const void *fops, void *private_data, int flags)
{
    int fd = kb_linux_kvm_get_unused_fd_flags((unsigned int)flags);
    if (fd < 0) {
        return fd;
    }
    void *file = kb_linux_kvm_anon_inode_getfile(name, fops, private_data, flags);
    if (file == NULL) {
        kb_linux_kvm_put_unused_fd(fd);
        return -12;
    }
    kb_linux_kvm_fd_install(fd, file);
    return fd;
}

void kb_linux_kvm_put_unused_fd(int fd)
{
    for (size_t i = 0; i < KB_KVM_FD_RECORD_MAX; i++) {
        if (!fd_records[i].active || fd_records[i].fd != fd) {
            continue;
        }
        close_host_fd_record(&fd_records[i]);
        kb_kfree(fd_records[i].file);
        memset(&fd_records[i], 0, sizeof(fd_records[i]));
        return;
    }
}

void *kb_kvm_alloc_stub(void)
{
    return kb_kzalloc(4096, 0);
}

static kb_status_t kb_kvm_ensure_dma_arena_mapped(kb_device_backend_t *backend)
{
    if (kb_kvm_ensure_page_arena() != 0) {
        return KB_ERR_NOMEM;
    }
    if (backend == NULL) {
        return KB_ERR_INVALID;
    }
    if (kb_kvm_dma_arena_mapped) {
        /* The page arena and its direct-map model have process lifetime.
         * Rebinding it would orphan the old mapping and invalidate every
         * struct-page-derived DMA address already handed to Linux modules. */
        return kb_kvm_dma_arena_backend == backend && kb_kvm_phys_base_valid ?
            KB_OK : KB_ERR_INVALID;
    }

    kb_status_t status = KB_ERR_INVALID;
    uint64_t dma_addr = kb_subsystem_dma_map_persistent_bidirectional(
        backend,
        NULL,
        kb_kvm_page_payloads,
        KB_KVM_PAGE_PAYLOAD_BYTES,
        &status);
    if (status != KB_OK || dma_addr == 0) {
        kb_kvm_phys_base_valid = 0;
        kb_kvm_dma_arena_backend = NULL;
        kb_kvm_dma_arena_mapped = 0;
        return status == KB_OK ? KB_ERR_IO : status;
    }

    kb_kvm_phys_base = dma_addr;
    kb_kvm_phys_base_valid = 1;
    kb_kvm_dma_arena_backend = backend;
    kb_kvm_dma_arena_mapped = 1;
    kb_loader_refresh_page_model_for_all_modules();
    return KB_OK;
}

int kb_kvm_prepare_dma_arena(kb_device_backend_t *backend)
{
    kb_status_t status = kb_kvm_ensure_dma_arena_mapped(backend);
    return status == KB_OK ? 0 : -(int)status;
}

void *kb_kvm_alloc_pages_stub(unsigned int flags, unsigned int order)
{
    (void)flags;
    if (order >= 8) {
        return NULL;
    }
    size_t page_count = (size_t)1u << order;
    if (page_count == 0 || page_count > KB_KVM_PAGE_RECORD_MAX) {
        return NULL;
    }
    kb_device_backend_t *backend = kb_shim_current_device_backend();
    if (kb_kvm_ensure_page_arena() != 0) {
        return NULL;
    }
    /* CPU-only page-cache and buffer-cache users do not need a DMA mapping.
     * Establish it eagerly when a device context exists; BIO mapping will
     * still fail closed until one has been established. */
    if (backend != NULL && kb_kvm_ensure_dma_arena_mapped(backend) != KB_OK) {
        return NULL;
    }

    size_t index = KB_KVM_PAGE_RECORD_MAX;
    for (size_t candidate = 0; candidate <= KB_KVM_PAGE_RECORD_MAX - page_count; candidate++) {
        size_t n = 0;
        while (n < page_count && kb_kvm_page_alloc_state[candidate + n] == 0) {
            n++;
        }
        if (n == page_count) {
            index = candidate;
            break;
        }
        candidate += n;
    }
    if (index == KB_KVM_PAGE_RECORD_MAX) {
        return NULL;
    }

    kb_kvm_page_alloc_state[index] = (unsigned char)(KB_KVM_PAGE_ALLOC_HEAD | (order & KB_KVM_PAGE_ALLOC_ORDER_MASK));
    for (size_t n = 1; n < page_count; n++) {
        kb_kvm_page_alloc_state[index + n] = KB_KVM_PAGE_ALLOC_TAIL;
    }
    memset(kb_kvm_page_records + (index * KB_KVM_STRUCT_PAGE_SIZE), 0, page_count * KB_KVM_STRUCT_PAGE_SIZE);
    memset(kb_kvm_page_payloads + (index * KB_KVM_PAGE_SIZE), 0, page_count * KB_KVM_PAGE_SIZE);
    if (trace_kvm_enabled()) {
        fprintf(stderr,
            "kobox-kvm: alloc_pages order=%u struct_page=%p payload=%p count=%zu\n",
            order,
            (void *)(kb_kvm_page_records + (index * KB_KVM_STRUCT_PAGE_SIZE)),
            (void *)(kb_kvm_page_payloads + (index * KB_KVM_PAGE_SIZE)),
            page_count);
    }
    return kb_kvm_page_records + (index * KB_KVM_STRUCT_PAGE_SIZE);
}

static int kb_kvm_release_page_index(size_t index, unsigned int order)
{
    if (order >= 8 || index >= KB_KVM_PAGE_RECORD_MAX) {
        return 0;
    }
    unsigned char state = kb_kvm_page_alloc_state[index];
    if ((state & KB_KVM_PAGE_ALLOC_HEAD) == 0 ||
        (state & KB_KVM_PAGE_ALLOC_ORDER_MASK) != order)
    {
        return 0;
    }
    size_t page_count = (size_t)1u << order;
    if (page_count == 0 || page_count > KB_KVM_PAGE_RECORD_MAX - index) {
        return 0;
    }
    for (size_t n = 1; n < page_count; n++) {
        if (kb_kvm_page_alloc_state[index + n] != KB_KVM_PAGE_ALLOC_TAIL) {
            return 0;
        }
    }
    memset(kb_kvm_page_alloc_state + index, 0, page_count);
    return 1;
}

int kb_kvm_release_pages(void *page, unsigned int order)
{
    if (page == NULL || kb_kvm_ensure_page_arena() != 0) {
        return 0;
    }
    uintptr_t base = (uintptr_t)kb_kvm_page_records;
    uintptr_t addr = (uintptr_t)page;
    if (addr < base || addr - base >= KB_KVM_PAGE_RECORD_BYTES) {
        return 0;
    }
    size_t offset = (size_t)(addr - base);
    if ((offset % KB_KVM_STRUCT_PAGE_SIZE) != 0) {
        return 0;
    }
    return kb_kvm_release_page_index(offset / KB_KVM_STRUCT_PAGE_SIZE, order);
}

void kb_kvm_free_pages_stub(void *page, unsigned int order)
{
    (void)kb_kvm_release_pages(page, order);
}

unsigned long kb_kvm_get_free_pages_stub(unsigned int flags, unsigned int order)
{
    void *page = kb_kvm_alloc_pages_stub(flags, order);
    if (page == NULL) {
        return 0;
    }
    uintptr_t records = (uintptr_t)kb_kvm_page_records;
    size_t index = ((uintptr_t)page - records) / KB_KVM_STRUCT_PAGE_SIZE;
    return (unsigned long)(uintptr_t)(kb_kvm_page_payloads + index * KB_KVM_PAGE_SIZE);
}

void kb_kvm_free_pages_addr_stub(unsigned long addr, unsigned int order)
{
    if (addr == 0 || kb_kvm_ensure_page_arena() != 0) {
        return;
    }
    uintptr_t base = (uintptr_t)kb_kvm_page_payloads;
    uintptr_t value = (uintptr_t)addr;
    if (value < base || value - base >= KB_KVM_PAGE_PAYLOAD_BYTES) {
        return;
    }
    size_t offset = (size_t)(value - base);
    if ((offset % KB_KVM_PAGE_SIZE) != 0) {
        return;
    }
    (void)kb_kvm_release_page_index(offset / KB_KVM_PAGE_SIZE, order);
}

void kb_kvm_free_pages_exact(void *virt, size_t size)
{
    if (virt == NULL) {
        return;
    }
    if (kb_kvm_ensure_page_arena() != 0) {
        return;
    }
    uintptr_t base = (uintptr_t)kb_kvm_page_payloads;
    uintptr_t addr = (uintptr_t)virt;
    size_t arena_size = KB_KVM_PAGE_PAYLOAD_BYTES;
    if (addr < base || addr - base >= arena_size) {
        return;
    }
    size_t offset = (size_t)(addr - base);
    if ((offset % KB_KVM_PAGE_SIZE) != 0) {
        return;
    }
    size_t index = offset / KB_KVM_PAGE_SIZE;
    unsigned char state = kb_kvm_page_alloc_state[index];
    if ((state & KB_KVM_PAGE_ALLOC_HEAD) == 0) {
        return;
    }
    unsigned int order = state & KB_KVM_PAGE_ALLOC_ORDER_MASK;
    size_t allocation_size = ((size_t)1u << order) * KB_KVM_PAGE_SIZE;
    if (size == 0 || size > allocation_size) {
        return;
    }
    (void)kb_kvm_release_page_index(index, order);
}

void kb_kvm_free_stub(void *ptr)
{
    kb_kfree(ptr);
}

int kb_kvm_return_minus_eopnotsupp(void)
{
    return -95;
}

uint64_t kb_kvm_return_zero_u64(void)
{
    return 0;
}

void *kb_kvm_get_cpu_entry_area(int cpu)
{
    (void)cpu;
    return kb_kvm_cpu_entry_area;
}

unsigned int kb_kvm_x86_family(unsigned int sig)
{
    unsigned int family = (sig >> 8) & 0xf;
    unsigned int extended_family = (sig >> 20) & 0xff;
    if (family == 0xf) {
        family += extended_family;
    }
    return family;
}

unsigned int kb_kvm_x86_model(unsigned int sig)
{
    unsigned int family = (sig >> 8) & 0xf;
    unsigned int model = (sig >> 4) & 0xf;
    unsigned int extended_model = (sig >> 16) & 0xf;
    if (family == 0x6 || family == 0xf) {
        model += extended_model << 4;
    }
    return model;
}

uint32_t kb_kvm_cpu_to_apicid(uint32_t cpu)
{
    return cpu;
}

static kb_kvm_fd_record_t *find_vm_record_for_vcpu(void *vcpu)
{
    void *vm = read_ptr_field(vcpu, 0);
    if (vm == NULL) {
        return NULL;
    }
    return find_fd_record_by_private_data(vm);
}

static uint8_t *translate_guest_physical(kb_kvm_fd_record_t *vm_record, uint64_t guest_phys_addr)
{
    if (vm_record == NULL || !vm_record->has_memory_region) {
        return NULL;
    }
    for (size_t i = 0; i < KB_KVM_MEMORY_SLOT_MAX; i++) {
        if (!vm_record->memory_region_active[i]) {
            continue;
        }
        const kb_kvm_userspace_memory_region_t *region = &vm_record->memory_regions[i];
        uint64_t start = region->guest_phys_addr;
        uint64_t end = start + region->memory_size;
        if (end < start || guest_phys_addr < start || guest_phys_addr >= end) {
            continue;
        }
        uint64_t offset = guest_phys_addr - start;
        return (uint8_t *)(uintptr_t)(region->userspace_addr + offset);
    }
    return NULL;
}

static uint8_t *translate_guest_instruction(kb_kvm_fd_record_t *vcpu_record, void *vcpu)
{
    kb_kvm_fd_record_t *vm_record = find_vm_record_for_vcpu(vcpu);
    if (vcpu_record == NULL || !vcpu_record->has_regs) {
        return NULL;
    }
    return translate_guest_physical(vm_record, vcpu_record->regs.rip);
}

static void clear_kvm_run_exit_area(void *run)
{
    if (run == NULL) {
        return;
    }
    memset((unsigned char *)run + KB_KVM_RUN_EXIT_REASON_OFFSET,
        0,
        KB_KVM_RUN_STORAGE_BYTES - KB_KVM_RUN_EXIT_REASON_OFFSET);
}

static void write_kvm_io_exit(void *run, uint8_t direction, uint16_t port, uint8_t value)
{
    write_u32_field(run, KB_KVM_RUN_EXIT_REASON_OFFSET, KB_KVM_EXIT_IO);
    write_u8_field(run, KB_KVM_RUN_IO_DIRECTION_OFFSET, direction);
    write_u8_field(run, KB_KVM_RUN_IO_SIZE_OFFSET, 1);
    write_u16_field(run, KB_KVM_RUN_IO_PORT_OFFSET, port);
    write_u32_field(run, KB_KVM_RUN_IO_COUNT_OFFSET, 1);
    write_u64_field(run, KB_KVM_RUN_IO_DATA_OFFSET_OFFSET, KB_KVM_RUN_EXIT_IO_DATA_OFFSET);
    write_u8_field(run, KB_KVM_RUN_EXIT_IO_DATA_OFFSET, value);
}

static void write_kvm_mmio_exit(void *run, uint64_t phys_addr, uint8_t is_write, uint8_t value)
{
    write_u32_field(run, KB_KVM_RUN_EXIT_REASON_OFFSET, KB_KVM_EXIT_MMIO);
    write_u64_field(run, KB_KVM_RUN_MMIO_PHYS_ADDR_OFFSET, phys_addr);
    write_u8_field(run, KB_KVM_RUN_MMIO_DATA_OFFSET, value);
    write_u32_field(run, KB_KVM_RUN_MMIO_LEN_OFFSET, 1);
    write_u8_field(run, KB_KVM_RUN_MMIO_IS_WRITE_OFFSET, is_write);
}

static int complete_pending_read_exit(kb_kvm_fd_record_t *vcpu_record, void *run)
{
    if (vcpu_record == NULL || run == NULL || vcpu_record->pending_exit == KB_KVM_PENDING_NONE) {
        return 0;
    }

    uint8_t value = 0;
    if (vcpu_record->pending_exit == KB_KVM_PENDING_IO_IN) {
        uint64_t offset = read_u64_field(run, KB_KVM_RUN_IO_DATA_OFFSET_OFFSET);
        if (offset >= KB_KVM_RUN_STORAGE_BYTES) {
            return -22;
        }
        value = read_u8_field(run, (size_t)offset);
    } else if (vcpu_record->pending_exit == KB_KVM_PENDING_MMIO_READ) {
        value = read_u8_field(run, KB_KVM_RUN_MMIO_DATA_OFFSET);
    } else {
        return -22;
    }

    vcpu_record->regs.rax = (vcpu_record->regs.rax & ~0xffULL) | value;
    vcpu_record->pending_exit = KB_KVM_PENDING_NONE;
    return 0;
}

int kb_kvm_vcpu_run_exit(void *vcpu, kb_kvm_fd_record_t *vcpu_record)
{
    if (vcpu == NULL) {
        return -22;
    }
    if (vcpu_record == NULL) {
        vcpu_record = find_fd_record_by_private_data(vcpu);
    }
    void *run = read_ptr_field(vcpu, KB_KVM_VCPU_RUN_OFFSET);
    if (run == NULL) {
        return -22;
    }
    int completion_result = complete_pending_read_exit(vcpu_record, run);
    if (completion_result != 0) {
        return completion_result;
    }
    clear_kvm_run_exit_area(run);

    for (int step = 0; step < 1024; step++) {
        uint8_t *insn = translate_guest_instruction(vcpu_record, vcpu);
        if (insn == NULL) {
            return -22;
        }

        switch (insn[0]) {
        case 0xb0: /* mov al, imm8 */
            vcpu_record->regs.rax = (vcpu_record->regs.rax & ~0xffULL) | insn[1];
            vcpu_record->regs.rip += 2;
            break;
        case 0xba: { /* mov dx, imm16 */
            uint16_t value = 0;
            memcpy(&value, insn + 1, sizeof(value));
            vcpu_record->regs.rdx = (vcpu_record->regs.rdx & ~0xffffULL) | value;
            vcpu_record->regs.rip += 3;
            break;
        }
        case 0x66:
            if (insn[1] == 0xba) { /* operand-size override mov dx, imm16 */
                uint16_t value = 0;
                memcpy(&value, insn + 2, sizeof(value));
                vcpu_record->regs.rdx = (vcpu_record->regs.rdx & ~0xffffULL) | value;
                vcpu_record->regs.rip += 4;
                break;
            }
            return -95;
        case 0xbe: { /* mov si, imm16 */
            uint16_t value = 0;
            memcpy(&value, insn + 1, sizeof(value));
            vcpu_record->regs.rsi = (vcpu_record->regs.rsi & ~0xffffULL) | value;
            vcpu_record->regs.rip += 3;
            break;
        }
        case 0xac: { /* lodsb */
            kb_kvm_fd_record_t *vm_record = find_vm_record_for_vcpu(vcpu);
            uint8_t *value = translate_guest_physical(vm_record, (uint16_t)vcpu_record->regs.rsi);
            if (value == NULL) {
                return -22;
            }
            vcpu_record->regs.rax = (vcpu_record->regs.rax & ~0xffULL) | *value;
            vcpu_record->regs.rsi = (vcpu_record->regs.rsi & ~0xffffULL) | (uint16_t)(vcpu_record->regs.rsi + 1);
            vcpu_record->regs.rip += 1;
            break;
        }
        case 0x84:
            if (insn[1] != 0xc0) {
                return -95;
            }
            if ((vcpu_record->regs.rax & 0xffULL) == 0) {
                vcpu_record->regs.rflags |= 0x40ULL;
            } else {
                vcpu_record->regs.rflags &= ~0x40ULL;
            }
            vcpu_record->regs.rip += 2;
            break;
        case 0x74:
            if ((vcpu_record->regs.rflags & 0x40ULL) != 0) {
                vcpu_record->regs.rip = vcpu_record->regs.rip + 2 + (int8_t)insn[1];
            } else {
                vcpu_record->regs.rip += 2;
            }
            break;
        case 0xeb:
            vcpu_record->regs.rip = vcpu_record->regs.rip + 2 + (int8_t)insn[1];
            break;
        case 0xf4: /* hlt */
            write_u32_field(run, KB_KVM_RUN_EXIT_REASON_OFFSET, KB_KVM_EXIT_HLT);
            vcpu_record->regs.rip += 1;
            return 0;
        case 0xe4: /* in al, imm8 */
            write_kvm_io_exit(run, 0, insn[1], 0);
            vcpu_record->regs.rip += 2;
            vcpu_record->pending_exit = KB_KVM_PENDING_IO_IN;
            return 0;
        case 0xe6: /* out imm8, al */
            write_kvm_io_exit(run, 1, insn[1], (uint8_t)vcpu_record->regs.rax);
            vcpu_record->regs.rip += 2;
            return 0;
        case 0xec: /* in al, dx */
            write_kvm_io_exit(run, 0, (uint16_t)vcpu_record->regs.rdx, 0);
            vcpu_record->regs.rip += 1;
            vcpu_record->pending_exit = KB_KVM_PENDING_IO_IN;
            return 0;
        case 0xee: /* out dx, al */
            write_kvm_io_exit(run, 1, (uint16_t)vcpu_record->regs.rdx, (uint8_t)vcpu_record->regs.rax);
            vcpu_record->regs.rip += 1;
            return 0;
        case 0x67:
            if (insn[1] == 0xa0) { /* addr32 mov al, moffs8 */
                uint32_t phys_addr32 = 0;
                memcpy(&phys_addr32, insn + 2, sizeof(phys_addr32));
                write_kvm_mmio_exit(run, phys_addr32, 0, 0);
                vcpu_record->regs.rip += 6;
                vcpu_record->pending_exit = KB_KVM_PENDING_MMIO_READ;
                return 0;
            }
            if (insn[1] == 0xa2) { /* addr32 mov moffs8, al */
                uint32_t phys_addr32 = 0;
                memcpy(&phys_addr32, insn + 2, sizeof(phys_addr32));
                write_kvm_mmio_exit(run, phys_addr32, 1, (uint8_t)vcpu_record->regs.rax);
                vcpu_record->regs.rip += 6;
                return 0;
            }
            return -95;
        case 0xa0: { /* mov al, moffs8 */
            uint64_t phys_addr = 0;
            memcpy(&phys_addr, insn + 1, sizeof(phys_addr));
            write_kvm_mmio_exit(run, phys_addr, 0, 0);
            vcpu_record->regs.rip += 9;
            vcpu_record->pending_exit = KB_KVM_PENDING_MMIO_READ;
            return 0;
        }
        case 0xa2: { /* mov moffs8, al */
            uint64_t phys_addr = 0;
            memcpy(&phys_addr, insn + 1, sizeof(phys_addr));
            write_kvm_mmio_exit(run, phys_addr, 1, (uint8_t)vcpu_record->regs.rax);
            vcpu_record->regs.rip += 9;
            return 0;
        }
        default:
            return -95;
        }
    }
    return -95;
}

int kb_kvm_vcpu_run_static_call(void *vcpu, unsigned long flags)
{
    (void)flags;
    return kb_kvm_vcpu_run_exit(vcpu, NULL);
}

static const kb_linux_symbol_t symbols[] = {
    {"__SCK__tp_func_ipi_send_cpu", (void *)(uintptr_t)&kb_noop_stub},
    {"__SCT__apic_call_send_IPI_mask", (void *)(uintptr_t)&kb_noop_stub},
    {"__SCT__apic_call_send_IPI_self", (void *)(uintptr_t)&kb_noop_stub},
    {"__SCT__tp_func_ipi_send_cpu", (void *)(uintptr_t)&kb_noop_stub},
    {"__alloc_pages_noprof", (void *)(uintptr_t)&kb_kvm_alloc_pages_stub},
    {"__cond_resched_rwlock_read", (void *)(uintptr_t)&kb_return_zero},
    {"__cond_resched_rwlock_write", (void *)(uintptr_t)&kb_return_zero},
    {"__cpuhp_remove_state", (void *)(uintptr_t)&kb_noop_stub},
    {"__cpuhp_setup_state", (void *)(uintptr_t)&kb_return_zero},
    {"__delay", (void *)(uintptr_t)&kb_noop_stub},
    {"__find_nth_bit", (void *)(uintptr_t)&kb_return_zero},
    {"__flush_tlb_all", (void *)(uintptr_t)&kb_noop_stub},
    {"__flush_tlb_global", (void *)(uintptr_t)&kb_noop_stub},
    {"__get_current_cr3_fast", (void *)(uintptr_t)&kb_return_zero},
    {"__get_user_nocheck_4", (void *)(uintptr_t)&kb_return_zero},
    {"__get_user_nocheck_8", (void *)(uintptr_t)&kb_return_zero},
    {"__hrtimer_get_remaining", (void *)(uintptr_t)&kb_kvm_return_zero_u64},
    {"__kfifo_out", (void *)(uintptr_t)&kb_return_zero},
    {"__kmalloc_large_noprof", (void *)(uintptr_t)&kb_kmalloc_alias},
    {"__local_bh_enable_ip", (void *)(uintptr_t)&kb_noop_stub},
    {"__lruvec_stat_mod_folio", (void *)(uintptr_t)&kb_noop_stub},
    {"__mmdrop", (void *)(uintptr_t)&kb_noop_stub},
    {"__put_task_struct_rcu_cb", (void *)(uintptr_t)&kb_noop_stub},
    {"__put_user_nocheck_4", (void *)(uintptr_t)&kb_return_zero},
    {"__kobox_kvm_vcpu_run_exit", (void *)(uintptr_t)&kb_kvm_vcpu_run_static_call},
    {"__static_call_return0", (void *)(uintptr_t)&kb_return_zero},
    {"__static_call_return1", (void *)(uintptr_t)&kb_return_one},
    {"__static_call_update", (void *)(uintptr_t)&kb_noop_stub},
    {"__static_key_deferred_flush", (void *)(uintptr_t)&kb_noop_stub},
    {"__static_key_slow_dec_deferred", (void *)(uintptr_t)&kb_noop_stub},
    {"__symbol_get", (void *)(uintptr_t)&kb_return_zero},
    {"__symbol_put", (void *)(uintptr_t)&kb_noop_stub},
    {"__trace_set_current_state", (void *)(uintptr_t)&kb_noop_stub},
    {"__tracepoint_ipi_send_cpu", (void *)(uintptr_t)&kb_alloc_stub},
    {"__tracepoint_sched_set_state_tp", (void *)(uintptr_t)&kb_alloc_stub},
    {"__vcalloc_noprof", (void *)(uintptr_t)&kb_kzalloc},
    {"_raw_read_lock_irq", (void *)(uintptr_t)&kb_raw_spin_lock},
    {"_raw_read_lock_irqsave", (void *)(uintptr_t)&kb_raw_spin_lock_irqsave},
    {"_raw_read_trylock", (void *)(uintptr_t)&kb_raw_spin_trylock},
    {"_raw_read_unlock_irq", (void *)(uintptr_t)&kb_raw_spin_unlock},
    {"_raw_read_unlock_irqrestore", (void *)(uintptr_t)&kb_raw_spin_unlock_irqrestore},
    {"_raw_write_lock_irq", (void *)(uintptr_t)&kb_raw_spin_lock},
    {"_raw_write_unlock_irq", (void *)(uintptr_t)&kb_raw_spin_unlock},
    {"add_wait_queue_priority_exclusive", (void *)(uintptr_t)&kb_noop_stub},
    {"alloc_cpumask_var_node", (void *)(uintptr_t)&kb_return_one},
    {"alloc_pages_noprof", (void *)(uintptr_t)&kb_kvm_alloc_pages_stub},
    {"alloc_workqueue_noprof", (void *)(uintptr_t)&kb_alloc_workqueue},
    {"anon_inode_create_getfile", (void *)(uintptr_t)&kb_alloc_stub},
    {"anon_inode_getfd", (void *)(uintptr_t)&kb_linux_kvm_anon_inode_getfd},
    {"anon_inode_getfile", (void *)(uintptr_t)&kb_linux_kvm_anon_inode_getfile},
    {"anon_inode_getfile_fmode", (void *)(uintptr_t)&kb_linux_kvm_anon_inode_getfile_fmode},
    {"amd_iommu_activate_guest_mode", (void *)(uintptr_t)&kb_return_zero},
    {"amd_iommu_deactivate_guest_mode", (void *)(uintptr_t)&kb_noop_stub},
    {"amd_iommu_register_ga_log_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"amd_iommu_unregister_ga_log_notifier", (void *)(uintptr_t)&kb_noop_stub},
    {"amd_iommu_update_ga", (void *)(uintptr_t)&kb_return_zero},
    {"amd_pmu_disable_virt", (void *)(uintptr_t)&kb_noop_stub},
    {"amd_pmu_enable_virt", (void *)(uintptr_t)&kb_noop_stub},
    {"asm_fred_entry_from_kvm", (void *)(uintptr_t)&kb_noop_stub},
    {"asm_exc_nmi_kvm_vmx", (void *)(uintptr_t)&kb_noop_stub},
    {"bpf_trace_run7", (void *)(uintptr_t)&kb_noop_stub},
    {"bpf_trace_run9", (void *)(uintptr_t)&kb_noop_stub},
    {"bsearch", (void *)(uintptr_t)&bsearch},
    {"call_srcu", (void *)(uintptr_t)&kb_noop_stub},
    {"cc_platform_has", (void *)(uintptr_t)&kb_return_zero},
    {"check_tsc_unstable", (void *)(uintptr_t)&kb_return_zero},
    {"clear_hv_tscchange_cb", (void *)(uintptr_t)&kb_noop_stub},
    {"clear_bhb_loop", (void *)(uintptr_t)&kb_noop_stub},
    {"clear_page_erms", (void *)(uintptr_t)&kb_noop_stub},
    {"clear_page_orig", (void *)(uintptr_t)&kb_noop_stub},
    {"clear_page_rep", (void *)(uintptr_t)&kb_noop_stub},
    {"copy_to_user_nofault", (void *)(uintptr_t)&kb_return_zero},
    {"cpu_buf_vm_clear", (void *)(uintptr_t)&kb_noop_stub},
    {"cpu_emergency_register_virt_callback", (void *)(uintptr_t)&kb_return_zero},
    {"cpu_emergency_unregister_virt_callback", (void *)(uintptr_t)&kb_noop_stub},
    {"cpu_mitigations_off", (void *)(uintptr_t)&kb_return_zero},
    {"cpu_smt_possible", (void *)(uintptr_t)&kb_return_zero},
    {"cr4_read_shadow", (void *)(uintptr_t)&kb_return_zero},
    {"cr4_update_irqsoff", (void *)(uintptr_t)&kb_noop_stub},
    {"current_save_fsgs", (void *)(uintptr_t)&kb_noop_stub},
    {"cpufreq_cpu_get", (void *)(uintptr_t)&kb_alloc_stub},
    {"cpufreq_cpu_put", (void *)(uintptr_t)&kb_noop_stub},
    {"cpufreq_quick_get", (void *)(uintptr_t)&kb_return_zero},
    {"cpufreq_register_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"cpufreq_unregister_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"debugfs_create_file_full", (void *)(uintptr_t)&kb_alloc_stub},
    {"debugfs_initialized", (void *)(uintptr_t)&kb_return_one},
    {"debugfs_lookup", (void *)(uintptr_t)&kb_return_zero},
    {"debug_locks_off", (void *)(uintptr_t)&kb_return_zero},
    {"default_cpu_present_to_apicid", (void *)(uintptr_t)&kb_kvm_cpu_to_apicid},
    {"dentry_path_raw", (void *)(uintptr_t)&kb_return_zero},
    {"do_machine_check", (void *)(uintptr_t)&kb_noop_stub},
    {"do_trace_read_msr", (void *)(uintptr_t)&kb_noop_stub},
    {"do_trace_write_msr", (void *)(uintptr_t)&kb_noop_stub},
    {"entry_untrain_ret", (void *)(uintptr_t)&kb_noop_stub},
    {"e820__mapped_raw_any", (void *)(uintptr_t)&kb_return_zero},
    {"eventfd_ctx_do_read", (void *)(uintptr_t)&kb_return_zero},
    {"eventfd_ctx_fdget", (void *)(uintptr_t)&kb_alloc_stub},
    {"eventfd_ctx_fileget", (void *)(uintptr_t)&kb_alloc_stub},
    {"eventfd_ctx_put", (void *)(uintptr_t)&kb_noop_stub},
    {"eventfd_ctx_remove_wait_queue", (void *)(uintptr_t)&kb_return_zero},
    {"eventfd_signal_mask", (void *)(uintptr_t)&kb_return_zero},
    {"finish_rcuwait", (void *)(uintptr_t)&kb_noop_stub},
    {"fixup_user_fault", (void *)(uintptr_t)&kb_return_zero},
    {"follow_pfnmap_end", (void *)(uintptr_t)&kb_noop_stub},
    {"follow_pfnmap_start", (void *)(uintptr_t)&kb_return_zero},
    {"fpstate_clear_xstate_component", (void *)(uintptr_t)&kb_noop_stub},
    {"fpu_alloc_guest_fpstate", (void *)(uintptr_t)&kb_alloc_stub},
    {"fpu_copy_guest_fpstate_to_uabi", (void *)(uintptr_t)&kb_return_zero},
    {"fpu_copy_uabi_to_guest_fpstate", (void *)(uintptr_t)&kb_return_zero},
    {"fpu_enable_guest_xfd_features", (void *)(uintptr_t)&kb_return_zero},
    {"fpu_free_guest_fpstate", (void *)(uintptr_t)&kb_kfree},
    {"fpu_swap_kvm_fpstate", (void *)(uintptr_t)&kb_noop_stub},
    {"fpu_sync_guest_vmexit_xfd_state", (void *)(uintptr_t)&kb_noop_stub},
    {"fpu_update_guest_xfd", (void *)(uintptr_t)&kb_noop_stub},
    {"fd_install", (void *)(uintptr_t)&kb_linux_kvm_fd_install},
    {"free_cpumask_var", (void *)(uintptr_t)&kb_noop_stub},
    {"gds_ucode_mitigated", (void *)(uintptr_t)&kb_return_one},
    {"generic_file_llseek", (void *)(uintptr_t)&kb_return_zero},
    {"generic_file_open", (void *)(uintptr_t)&kb_return_zero},
    {"get_compat_sigset", (void *)(uintptr_t)&kb_return_zero},
    {"get_file_active", (void *)(uintptr_t)&kb_return_one},
    {"get_cpu_entry_area", (void *)(uintptr_t)&kb_kvm_get_cpu_entry_area},
    {"get_pid_task", (void *)(uintptr_t)&kb_alloc_stub},
    {"get_task_pid", (void *)(uintptr_t)&kb_alloc_stub},
    {"get_unused_fd_flags", (void *)(uintptr_t)&kb_linux_kvm_get_unused_fd_flags},
    {"get_user_pages_fast_only", (void *)(uintptr_t)&kb_return_zero},
    {"get_user_pages_unlocked", (void *)(uintptr_t)&kb_return_zero},
    {"handle_guest_split_lock", (void *)(uintptr_t)&kb_return_zero},
    {"housekeeping_enabled", (void *)(uintptr_t)&kb_return_zero},
    {"hrtimer_cancel", (void *)(uintptr_t)&kb_return_zero},
    {"hrtimer_setup", (void *)(uintptr_t)&kb_noop_stub},
    {"hrtimer_start_range_ns", (void *)(uintptr_t)&kb_return_zero},
    {"hrtimer_try_to_cancel", (void *)(uintptr_t)&kb_return_zero},
    {"hugetlb_optimize_vmemmap_key", (void *)(uintptr_t)&kb_return_zero},
    {"hv_get_tsc_page", (void *)(uintptr_t)&kb_return_zero},
    {"hw_breakpoint_restore", (void *)(uintptr_t)&kb_noop_stub},
    {"hyperv_fill_flush_guest_mapping_list", (void *)(uintptr_t)&kb_return_zero},
    {"hyperv_flush_guest_mapping", (void *)(uintptr_t)&kb_return_zero},
    {"hyperv_flush_guest_mapping_range", (void *)(uintptr_t)&kb_return_zero},
    {"hyperv_stop_tsc_emulation", (void *)(uintptr_t)&kb_noop_stub},
    {"idr_find", (void *)(uintptr_t)&kb_return_zero},
    {"intel_pt_handle_vmx", (void *)(uintptr_t)&kb_noop_stub},
    {"intel_pt_validate_cap", (void *)(uintptr_t)&kb_return_zero},
    {"intel_pt_validate_hw_cap", (void *)(uintptr_t)&kb_return_zero},
    {"interval_tree_insert", (void *)(uintptr_t)&kb_noop_stub},
    {"interval_tree_iter_first", (void *)(uintptr_t)&kb_return_zero},
    {"interval_tree_iter_next", (void *)(uintptr_t)&kb_return_zero},
    {"interval_tree_remove", (void *)(uintptr_t)&kb_noop_stub},
    {"irq_bypass_register_consumer", (void *)(uintptr_t)&kb_return_zero},
    {"irq_bypass_unregister_consumer", (void *)(uintptr_t)&kb_noop_stub},
    {"irq_remapping_cap", (void *)(uintptr_t)&kb_return_zero},
    {"irq_set_vcpu_affinity", (void *)(uintptr_t)&kb_return_zero},
    {"irq_work_queue", (void *)(uintptr_t)&kb_return_zero},
    {"irq_work_sync", (void *)(uintptr_t)&kb_noop_stub},
    {"itlb_multihit_kvm_mitigation", (void *)(uintptr_t)&kb_return_zero},
    {"jump_label_update_timeout", (void *)(uintptr_t)&kb_return_zero},
    {"kthread_create_worker_on_node", (void *)(uintptr_t)&kb_alloc_stub},
    {"kthread_destroy_worker", (void *)(uintptr_t)&kb_noop_stub},
    {"kthread_flush_work", (void *)(uintptr_t)&kb_noop_stub},
    {"kthread_queue_work", (void *)(uintptr_t)&kb_return_one},
    {"ktime_get_raw", (void *)(uintptr_t)&kb_ktime_get},
    {"kvm_async_pf_task_wait_schedule", (void *)(uintptr_t)&kb_noop_stub},
    {"kvm_configure_mmu", (void *)(uintptr_t)&kb_return_zero},
    {"kvm_mmu_set_me_spte_mask", (void *)(uintptr_t)&kb_noop_stub},
    {"kvm_mmu_set_mmio_spte_mask", (void *)(uintptr_t)&kb_noop_stub},
    {"kvm_read_and_reset_apf_flags", (void *)(uintptr_t)&kb_return_zero},
    {"kvm_set_posted_intr_wakeup_handler", (void *)(uintptr_t)&kb_noop_stub},
    {"l1tf_vmx_mitigation", (void *)(uintptr_t)&kb_return_zero},
    {"load_fixmap_gdt", (void *)(uintptr_t)&kb_noop_stub},
    {"mark_page_accessed", (void *)(uintptr_t)&kb_noop_stub},
    {"mark_tsc_unstable", (void *)(uintptr_t)&kb_noop_stub},
    {"memremap", (void *)(uintptr_t)&kb_kvm_alloc_stub},
    {"memunmap", (void *)(uintptr_t)&kb_kvm_free_stub},
    {"migrate_disable", (void *)(uintptr_t)&kb_noop_stub},
    {"migrate_enable", (void *)(uintptr_t)&kb_noop_stub},
    {"misc_deregister", (void *)(uintptr_t)&kb_linux_kvm_misc_deregister},
    {"misc_register", (void *)(uintptr_t)&kb_linux_kvm_misc_register},
    {"mmput", (void *)(uintptr_t)&kb_noop_stub},
    {"msr_clear_bit", (void *)(uintptr_t)&kb_return_zero},
    {"msr_set_bit", (void *)(uintptr_t)&kb_return_zero},
    {"mmu_notifier_register", (void *)(uintptr_t)&kb_return_zero},
    {"mmu_notifier_unregister", (void *)(uintptr_t)&kb_noop_stub},
    {"mtree_load", (void *)(uintptr_t)&kb_return_zero},
    {"param_get_bint", (void *)(uintptr_t)&kb_return_zero},
    {"param_get_bool", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_bint", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_uint", (void *)(uintptr_t)&kb_return_zero},
    {"pat_enabled", (void *)(uintptr_t)&kb_return_one},
    {"pat_pfn_immune_to_uc_mtrr", (void *)(uintptr_t)&kb_return_one},
    {"perf_event_create_kernel_counter", (void *)(uintptr_t)&kb_alloc_stub},
    {"perf_event_enable", (void *)(uintptr_t)&kb_noop_stub},
    {"perf_event_create_kernel_counter", (void *)(uintptr_t)&kb_return_zero},
    {"perf_event_pause", (void *)(uintptr_t)&kb_noop_stub},
    {"perf_event_period", (void *)(uintptr_t)&kb_kvm_return_zero_u64},
    {"perf_event_read_value", (void *)(uintptr_t)&kb_return_zero},
    {"perf_event_release_kernel", (void *)(uintptr_t)&kb_noop_stub},
    {"perf_guest_get_msrs", (void *)(uintptr_t)&kb_return_zero},
    {"perf_get_hw_event_config", (void *)(uintptr_t)&kb_return_zero},
    {"perf_get_x86_pmu_capability", (void *)(uintptr_t)&kb_return_zero},
    {"perf_register_guest_info_callbacks", (void *)(uintptr_t)&kb_return_zero},
    {"perf_unregister_guest_info_callbacks", (void *)(uintptr_t)&kb_return_zero},
    {"pid_vnr", (void *)(uintptr_t)&kb_return_zero},
    {"pin_user_pages_fast", (void *)(uintptr_t)&kb_return_zero},
    {"pin_user_pages_unlocked", (void *)(uintptr_t)&kb_return_zero},
    {"preempt_model_full", (void *)(uintptr_t)&kb_return_zero},
    {"preempt_model_lazy", (void *)(uintptr_t)&kb_return_zero},
    {"preempt_notifier_dec", (void *)(uintptr_t)&kb_noop_stub},
    {"preempt_notifier_inc", (void *)(uintptr_t)&kb_noop_stub},
    {"preempt_notifier_register", (void *)(uintptr_t)&kb_return_zero},
    {"preempt_notifier_unregister", (void *)(uintptr_t)&kb_noop_stub},
    {"prof_on", (void *)(uintptr_t)&kb_return_zero},
    {"profile_hits", (void *)(uintptr_t)&kb_noop_stub},
    {"put_unused_fd", (void *)(uintptr_t)&kb_linux_kvm_put_unused_fd},
    {"pvclock_gtod_register_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"pvclock_gtod_unregister_notifier", (void *)(uintptr_t)&kb_noop_stub},
    {"rb_last", (void *)(uintptr_t)&kb_return_zero},
    {"rb_replace_node", (void *)(uintptr_t)&kb_noop_stub},
    {"rcu_note_context_switch", (void *)(uintptr_t)&kb_noop_stub},
    {"register_pm_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"register_syscore_ops", (void *)(uintptr_t)&kb_noop_stub},
    {"rep_movs_alternative", (void *)(uintptr_t)&memcpy},
    {"rep_stos_alternative", (void *)(uintptr_t)&kb_noop_stub},
    {"send_sig_mceerr", (void *)(uintptr_t)&kb_return_zero},
    {"set_hv_tscchange_cb", (void *)(uintptr_t)&kb_noop_stub},
    {"set_memory_decrypted", (void *)(uintptr_t)&kb_return_zero},
    {"set_memory_encrypted", (void *)(uintptr_t)&kb_return_zero},
    {"sgx_set_attribute", (void *)(uintptr_t)&kb_return_zero},
    {"sgx_virt_ecreate", (void *)(uintptr_t)&kb_return_zero},
    {"sgx_virt_einit", (void *)(uintptr_t)&kb_return_zero},
    {"sigprocmask", (void *)(uintptr_t)&kb_return_zero},
    {"simple_attr_open", (void *)(uintptr_t)&kb_return_zero},
    {"simple_attr_read", (void *)(uintptr_t)&kb_return_zero},
    {"simple_attr_release", (void *)(uintptr_t)&kb_return_zero},
    {"simple_attr_write", (void *)(uintptr_t)&kb_return_zero},
    {"single_task_running", (void *)(uintptr_t)&kb_return_one},
    {"smp_call_function_many", (void *)(uintptr_t)&kb_noop_stub},
    {"smp_call_function_single", (void *)(uintptr_t)&kb_return_zero},
    {"srso_alias_untrain_ret", (void *)(uintptr_t)&kb_noop_stub},
    {"srso_alias_safe_ret", (void *)(uintptr_t)&kb_noop_stub},
    {"srcu_barrier", (void *)(uintptr_t)&kb_noop_stub},
    {"static_key_slow_dec", (void *)(uintptr_t)&kb_noop_stub},
    {"static_key_disable", (void *)(uintptr_t)&kb_noop_stub},
    {"static_key_enable", (void *)(uintptr_t)&kb_noop_stub},
    {"static_key_slow_inc", (void *)(uintptr_t)&kb_return_zero},
    {"switch_fpu_return", (void *)(uintptr_t)&kb_noop_stub},
    {"switch_vcpu_ibpb", (void *)(uintptr_t)&kb_noop_stub},
    {"synchronize_srcu_expedited", (void *)(uintptr_t)&kb_noop_stub},
    {"task_cputime_adjusted", (void *)(uintptr_t)&kb_noop_stub},
    {"timer_init_key", (void *)(uintptr_t)&kb_noop_stub},
    {"trace_print_hex_seq", (void *)(uintptr_t)&kb_return_zero},
    {"unregister_pm_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"unregister_syscore_ops", (void *)(uintptr_t)&kb_noop_stub},
    {"user_return_notifier_register", (void *)(uintptr_t)&kb_return_zero},
    {"user_return_notifier_unregister", (void *)(uintptr_t)&kb_noop_stub},
    {"validate_usercopy_range", (void *)(uintptr_t)&kb_return_one},
    {"vhost_task_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"vhost_task_start", (void *)(uintptr_t)&kb_noop_stub},
    {"vhost_task_stop", (void *)(uintptr_t)&kb_noop_stub},
    {"vhost_task_wake", (void *)(uintptr_t)&kb_noop_stub},
    {"vm_mmap", (void *)(uintptr_t)&kb_kvm_return_zero_u64},
    {"vm_munmap", (void *)(uintptr_t)&kb_return_zero},
    {"vma_kernel_pagesize", (void *)(uintptr_t)&kb_kvm_return_zero_u64},
    {"vmemdup_user", (void *)(uintptr_t)&kb_kmemdup},
    {"vzalloc_noprof", (void *)(uintptr_t)&kb_kzalloc},
    {"wbinvd_on_cpu", (void *)(uintptr_t)&kb_return_zero},
    {"wbinvd_on_cpus_mask", (void *)(uintptr_t)&kb_noop_stub},
    {"write_ibpb", (void *)(uintptr_t)&kb_noop_stub},
    {"x86_ibpb_exit_to_user", (void *)(uintptr_t)&kb_noop_stub},
    {"x86_match_cpu", (void *)(uintptr_t)&kb_return_zero},
    {"x86_family", (void *)(uintptr_t)&kb_kvm_x86_family},
    {"x86_model", (void *)(uintptr_t)&kb_kvm_x86_model},
    {"x86_msi_msg_get_destid", (void *)(uintptr_t)&kb_return_zero},
    {"x86_perf_get_lbr", (void *)(uintptr_t)&kb_return_zero},
    {"xa_store_range", (void *)(uintptr_t)&kb_return_zero},
    {"xfer_to_guest_mode_handle_work", (void *)(uintptr_t)&kb_return_zero},
    {"xstate_get_guest_group_perm", (void *)(uintptr_t)&kb_return_zero},
    {"yield_to", (void *)(uintptr_t)&kb_return_zero},
    {"zero_pfn", (void *)(uintptr_t)&kb_kvm_return_zero_u64},

    {"const_current_task", (void *)(uintptr_t)&kb_kvm_const_current_task},
    {"cpu_dr7", (void *)(uintptr_t)&kb_kvm_cpu_dr7},
    {"cpu_info", (void *)(uintptr_t)&kb_kvm_cpu_info},
    {"empty_zero_page", (void *)(uintptr_t)&kb_kvm_empty_zero_page},
    {"mem_section", (void *)(uintptr_t)&kb_kvm_mem_section},
    {"page_offset_base", (void *)(uintptr_t)&kb_kvm_page_offset_base},
    {"phys_base", (void *)(uintptr_t)&kb_kvm_phys_base},
    {"pgdir_shift", (void *)(uintptr_t)&kb_kvm_pgdir_shift},
    {"physical_mask", (void *)(uintptr_t)&kb_kvm_physical_mask},
    {"ptrs_per_p4d", (void *)(uintptr_t)&kb_kvm_ptrs_per_p4d},
    {"smp_ops", (void *)(uintptr_t)&kb_kvm_smp_ops},
    {"tsc_khz", (void *)(uintptr_t)&kb_kvm_tsc_khz},
    {"vmemmap_base", (void *)(uintptr_t)&kb_kvm_vmemmap_base},
    {"x86_hyper_type", (void *)(uintptr_t)&kb_kvm_x86_hyper_type},
};

const kb_linux_symbol_t *kb_linux_kvm_symbols(size_t *out_count)
{
    kb_kvm_sync_page_model();
    if (out_count != NULL) {
        *out_count = sizeof(symbols) / sizeof(symbols[0]);
    }
    return symbols;
}
