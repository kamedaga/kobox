#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/elf.h"
#include "kobox/module.h"
#include "kobox/shim.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#if defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

typedef struct loaded_section {
    void *base;
    uint64_t size;
    uint64_t offset;
    uint64_t alignment;
    uint64_t flags;
} loaded_section_t;

enum {
    KB_LOCAL_SHIM_STUB_SIZE = 48,
    KB_LOCAL_SHIM_STUB_COUNT = 2048,
    KB_LOCAL_SHIM_DATA_SIZE = 32768,
    KB_LOCAL_SHIM_REGION_SIZE = (KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT) + KB_LOCAL_SHIM_DATA_SIZE,
    KB_LOCAL_SHIM_DATA_OFFSET = KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT,
    KB_LOCAL_NODE_DATA_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 6144,
    KB_LOCAL_NODE_STATES_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 6400,
    KB_LOCAL_BOOT_CPU_DATA_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 6656,
    KB_LOCAL_CURRENT_MM_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 4608,
    KB_LOCAL_CURRENT_TASK_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 8192,
    KB_LOCAL_USB_DATA_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 11264,
    KB_LOCAL_JIFFIES_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 4096,
    KB_LOCAL_GS_SIZE = 4096,
    KB_LOCAL_GS_PCPU_HOT_OFFSET = 0x100,
    KB_LOCAL_USB_NUM_ONLINE_CPUS_OFFSET = 0,
    KB_LOCAL_USB_PCPU_HOT_OFFSET = 64,
    KB_LOCAL_USB_PM_SUSPEND_TARGET_STATE_OFFSET = 320,
    KB_LOCAL_USB_HCD_PCI_PM_OPS_OFFSET = 384,
    KB_LOCAL_USB_XHCI_TRACEPOINT_OFFSET = 704,
    KB_LOCAL_USB_MISC_DATA_OFFSET = 1024,
    KB_LOCAL_USB_MISC_DATA_STRIDE = 128,
    KB_LOCAL_USB_CONTROL_MSG_STUB_OFFSET = KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_MISC_DATA_OFFSET,
};

static int trace_modules_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_MODULES");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

struct kb_module {
    kb_backend_t *backend;
    kb_elf_file_t elf;
    void *image_base;
    uint64_t image_size;
    uint8_t *shim_region;
    uint8_t *shim_symbol_stubs;
    uint8_t *shim_printk;
    uint8_t *shim_kfree;
    uint8_t *shim_kmalloc;
    uint8_t *shim_kzalloc;
    uint8_t *shim_kmalloc_trace;
    uint8_t *shim_request_threaded_irq;
    uint8_t *shim_free_irq;
    uint8_t *shim_dma_alloc_attrs;
    uint8_t *shim_dma_free_attrs;
    uint8_t *shim_stack_chk_fail;
    uint8_t *shim_pci_register_driver;
    uint8_t *shim_pci_unregister_driver;
    uint8_t *shim_pci_enable_device;
    uint8_t *shim_pci_disable_device;
    uint8_t *shim_pci_set_master;
    uint8_t *shim_pci_iomap;
    uint8_t *shim_pci_iounmap;
    uint8_t *shim_ioread32;
    uint8_t *shim_iowrite32;
    void *shim_kmalloc_caches;
    void *shim_random_kmalloc_seed;
    void *shim_param_ops_int;
    void *shim_param_ops_uint;
    void *shim_param_ops_bool;
    void *shim_param_ops_byte;
    void *shim_param_ops_ulong;
    void *shim_cpu_possible_mask;
    void *shim_cpu_online_mask;
    void *shim_nr_cpu_ids;
    void *shim_this_cpu_off;
    void *shim_pernet_ops_rwsem;
    void *shim_panic_notifier_list;
    void *shim_pv_ops;
    void *shim_misc_data;
    void *shim_node_data;
    void *shim_node_states;
    void *shim_boot_cpu_data;
    void *shim_node0;
    void *shim_current_mm;
    void *shim_current_task;
    char *module_name;
    loaded_section_t *sections;
    size_t section_count;
    size_t external_stub_count;
    int (*init_module)(void);
    void (*cleanup_module)(void);
#if !defined(_WIN32) && defined(__x86_64__)
    uint8_t kernel_gs[KB_LOCAL_GS_SIZE];
#endif
    struct kb_module *next_loaded;
};

typedef struct shim_symbol {
    const char *name;
    void *address;
} shim_symbol_t;

typedef struct exported_symbol {
    const char *name;
    uint64_t address;
    const kb_module_t *owner;
} exported_symbol_t;

enum {
    KB_EXPORTED_SYMBOL_MAX = 4096,
    KB_KERNEL_OBJECT_MAX = 256,
    KB_FAKE_FD_MAX = 256,
    KB_FAKE_VMA_MAX = 256,
    KB_FAKE_FILE_SIZE = 512,
    KB_FAKE_INODE_SIZE = 512,
    KB_FAKE_MAPPING_SIZE = 512,
    KB_FAKE_VMA_SIZE = 512,
    KB_FAKE_PAGE_SIZE = 128,
};

static exported_symbol_t exported_symbols[KB_EXPORTED_SYMBOL_MAX];
static kb_module_t *kb_active_module;
static kb_module_t *kb_loaded_modules;
uintptr_t kb_current_external_call_target;
uintptr_t kb_current_external_call_caller_gs;
uintptr_t kb_current_external_call_callee_gs;

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

static kb_chrdev_record_t kb_chrdev_records[KB_KERNEL_OBJECT_MAX];
static kb_proc_record_t kb_proc_records[KB_KERNEL_OBJECT_MAX];
static kb_class_record_t kb_class_records[KB_KERNEL_OBJECT_MAX];
static kb_cdev_record_t kb_cdev_records[KB_KERNEL_OBJECT_MAX];
static kb_fd_record_t kb_fd_records[KB_FAKE_FD_MAX];
static kb_fake_file_record_t kb_fake_file_records[KB_KERNEL_OBJECT_MAX];
static kb_vma_record_t kb_vma_records[KB_FAKE_VMA_MAX];
static int kb_kernel_object_summary_registered;
static unsigned kb_next_dynamic_major = 240;
static unsigned kb_next_fake_fd = 3;

static void kb_noop(void)
{
}

static unsigned long kb_copy_from_user(void *to, const void *from, unsigned long n)
{
    if (n == 0) {
        return 0;
    }
    if (to == NULL || from == NULL) {
        return n;
    }
    memcpy(to, from, (size_t)n);
    return 0;
}

static unsigned long kb_copy_to_user(void *to, const void *from, unsigned long n)
{
    if (n == 0) {
        return 0;
    }
    if (to == NULL || from == NULL) {
        return n;
    }
    memcpy(to, from, (size_t)n);
    return 0;
}

static long kb_strncpy_from_user(char *dst, const char *src, long count)
{
    if (dst == NULL || src == NULL || count <= 0) {
        return 0;
    }
    size_t copied = 0;
    while (copied + 1u < (size_t)count && src[copied] != '\0') {
        dst[copied] = src[copied];
        copied++;
    }
    if (copied < (size_t)count) {
        dst[copied] = '\0';
    }
    return (long)copied;
}

static long kb_strnlen_user(const char *src, long count)
{
    if (src == NULL || count <= 0) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        if (src[i] == '\0') {
            return i + 1;
        }
    }
    return count + 1;
}

static void *kb_err_ptr_noent(void)
{
    return (void *)(intptr_t)-2;
}

static unsigned long kb_x86_save_flags_if_enabled(void)
{
    return 0x200ul;
}

static uint64_t kb_x86_read_pat_msr(void)
{
    return 0x106ull;
}

static unsigned long kb_find_first_bit_shim(const unsigned long *addr, unsigned long size)
{
    return kb_find_next_bit(addr, size, 0);
}

static unsigned long kb_find_next_zero_bit_shim(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    if (addr == NULL || offset >= size) {
        return size;
    }
    const unsigned long word_bits = sizeof(unsigned long) * 8ul;
    for (unsigned long bit = offset; bit < size; bit++) {
        if ((addr[bit / word_bits] & (1ul << (bit % word_bits))) == 0) {
            return bit;
        }
    }
    return size;
}

static unsigned long kb_find_first_zero_bit_shim(const unsigned long *addr, unsigned long size)
{
    return kb_find_next_zero_bit_shim(addr, size, 0);
}

static unsigned long kb_find_last_bit_shim(const unsigned long *addr, unsigned long size)
{
    if (addr == NULL || size == 0) {
        return size;
    }
    const unsigned long word_bits = sizeof(unsigned long) * 8ul;
    for (unsigned long bit = size; bit > 0; bit--) {
        const unsigned long index = bit - 1;
        if ((addr[index / word_bits] & (1ul << (index % word_bits))) != 0) {
            return index;
        }
    }
    return size;
}

static uint32_t kb_hweight32(uint32_t value)
{
    uint32_t count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

static uint64_t kb_hweight64(uint64_t value)
{
    uint64_t count = 0;
    while (value != 0) {
        count += value & 1ull;
        value >>= 1ull;
    }
    return count;
}

static unsigned long kb_bitmap_word_mask(unsigned int bit)
{
    return 1ul << (bit % (unsigned int)(sizeof(unsigned long) * 8u));
}

static void kb_bitmap_set_shim(unsigned long *map, unsigned int start, int len)
{
    if (map == NULL || len <= 0) {
        return;
    }
    const unsigned int word_bits = (unsigned int)(sizeof(unsigned long) * 8u);
    for (int i = 0; i < len; i++) {
        const unsigned int bit = start + (unsigned int)i;
        map[bit / word_bits] |= kb_bitmap_word_mask(bit);
    }
}

static void kb_bitmap_clear_shim(unsigned long *map, unsigned int start, int len)
{
    if (map == NULL || len <= 0) {
        return;
    }
    const unsigned int word_bits = (unsigned int)(sizeof(unsigned long) * 8u);
    for (int i = 0; i < len; i++) {
        const unsigned int bit = start + (unsigned int)i;
        map[bit / word_bits] &= ~kb_bitmap_word_mask(bit);
    }
}

static int kb_bitmap_and_shim(
    unsigned long *dst,
    const unsigned long *src1,
    const unsigned long *src2,
    unsigned int bits)
{
    if (dst == NULL || src1 == NULL || src2 == NULL) {
        return 0;
    }
    const unsigned int words = (bits + (unsigned int)(sizeof(unsigned long) * 8u) - 1u) /
        (unsigned int)(sizeof(unsigned long) * 8u);
    int any = 0;
    for (unsigned int i = 0; i < words; i++) {
        dst[i] = src1[i] & src2[i];
        any |= dst[i] != 0;
    }
    return any;
}

static int kb_bitmap_andnot_shim(
    unsigned long *dst,
    const unsigned long *src1,
    const unsigned long *src2,
    unsigned int bits)
{
    if (dst == NULL || src1 == NULL || src2 == NULL) {
        return 0;
    }
    const unsigned int words = (bits + (unsigned int)(sizeof(unsigned long) * 8u) - 1u) /
        (unsigned int)(sizeof(unsigned long) * 8u);
    int any = 0;
    for (unsigned int i = 0; i < words; i++) {
        dst[i] = src1[i] & ~src2[i];
        any |= dst[i] != 0;
    }
    return any;
}

static void kb_bitmap_or_shim(
    unsigned long *dst,
    const unsigned long *src1,
    const unsigned long *src2,
    unsigned int bits)
{
    if (dst == NULL || src1 == NULL || src2 == NULL) {
        return;
    }
    const unsigned int words = (bits + (unsigned int)(sizeof(unsigned long) * 8u) - 1u) /
        (unsigned int)(sizeof(unsigned long) * 8u);
    for (unsigned int i = 0; i < words; i++) {
        dst[i] = src1[i] | src2[i];
    }
}

static void kb_bitmap_complement_shim(unsigned long *dst, const unsigned long *src, unsigned int bits)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    const unsigned int words = (bits + (unsigned int)(sizeof(unsigned long) * 8u) - 1u) /
        (unsigned int)(sizeof(unsigned long) * 8u);
    for (unsigned int i = 0; i < words; i++) {
        dst[i] = ~src[i];
    }
}

static int kb_bitmap_intersects_shim(const unsigned long *src1, const unsigned long *src2, unsigned int bits)
{
    if (src1 == NULL || src2 == NULL) {
        return 0;
    }
    const unsigned int words = (bits + (unsigned int)(sizeof(unsigned long) * 8u) - 1u) /
        (unsigned int)(sizeof(unsigned long) * 8u);
    for (unsigned int i = 0; i < words; i++) {
        if ((src1[i] & src2[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

static void kb_bitmap_shift_left_shim(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int bits)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    const unsigned int word_bits = (unsigned int)(sizeof(unsigned long) * 8u);
    const unsigned int words = (bits + word_bits - 1u) / word_bits;
    memset(dst, 0, (size_t)words * sizeof(unsigned long));
    for (unsigned int bit = 0; bit < bits; bit++) {
        if ((src[bit / word_bits] & kb_bitmap_word_mask(bit)) != 0 && bit + shift < bits) {
            dst[(bit + shift) / word_bits] |= kb_bitmap_word_mask(bit + shift);
        }
    }
}

static void kb_bitmap_shift_right_shim(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int bits)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    const unsigned int word_bits = (unsigned int)(sizeof(unsigned long) * 8u);
    const unsigned int words = (bits + word_bits - 1u) / word_bits;
    memset(dst, 0, (size_t)words * sizeof(unsigned long));
    for (unsigned int bit = shift; bit < bits; bit++) {
        if ((src[bit / word_bits] & kb_bitmap_word_mask(bit)) != 0) {
            dst[(bit - shift) / word_bits] |= kb_bitmap_word_mask(bit - shift);
        }
    }
}

static void *kb_krealloc_shim(void *ptr, size_t size, unsigned int flags)
{
    return kb_krealloc_managed(ptr, size, flags);
}

static size_t kb_ksize_shim(const void *ptr)
{
    size_t size = kb_kmalloc_usable_size(ptr);
    return size != 0 ? size : (ptr == NULL ? 0 : 4096);
}

static void kb_memset_io_shim(void *addr, int value, size_t size)
{
    if (addr != NULL) {
        memset(addr, value, size);
    }
}

static void kb_kernel_sort_shim(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *), void *swap)
{
    (void)swap;
    if (base != NULL && cmp != NULL && size != 0) {
        qsort(base, num, size, cmp);
    }
}

static char *kb_kvasprintf_shim(unsigned int flags, const char *fmt, va_list args)
{
    if (fmt == NULL) {
        return NULL;
    }
    va_list copy;
    va_copy(copy, args);
    const int needed = kb_vsnprintf_safe(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        return NULL;
    }
    (void)flags;
    char *buffer = kb_kmalloc((size_t)needed + 1u, flags);
    if (buffer == NULL) {
        return NULL;
    }
    kb_vsnprintf_safe(buffer, (size_t)needed + 1u, fmt, args);
    return buffer;
}

static char *kb_kasprintf_shim(unsigned int flags, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *result = kb_kvasprintf_shim(flags, fmt, args);
    va_end(args);
    return result;
}

#if defined(__x86_64__) && !defined(_WIN32)
__attribute__((naked)) static void kb_shim_external_call_trampoline(void)
{
    __asm__ volatile(
        "mov %rax, -8(%rsp)\n\t"
        "mov %rsp, %rax\n\t"
        "lea -704(%rsp), %rsp\n\t"
        "and $-16, %rsp\n\t"
        "mov %rax, 512(%rsp)\n\t"
        "mov %r11, 520(%rsp)\n\t"
        "mov %rdi, 528(%rsp)\n\t"
        "mov %rsi, 536(%rsp)\n\t"
        "mov %rdx, 544(%rsp)\n\t"
        "mov %rcx, 552(%rsp)\n\t"
        "mov %r8, 560(%rsp)\n\t"
        "mov %r9, 568(%rsp)\n\t"
        "mov %rbx, 576(%rsp)\n\t"
        "mov %rbp, 584(%rsp)\n\t"
        "mov %r12, 592(%rsp)\n\t"
        "mov %r13, 600(%rsp)\n\t"
        "mov %r14, 608(%rsp)\n\t"
        "mov %r15, 616(%rsp)\n\t"
        "mov %r10, 624(%rsp)\n\t"
        "mov -8(%rax), %r10\n\t"
        "mov %r10, 648(%rsp)\n\t"
        "mov 512(%rsp), %rsi\n\t"
        "lea 8(%rsi), %rsi\n\t"
        "mov %rsp, %rdi\n\t"
        "mov $64, %ecx\n\t"
        "cld\n\t"
        "rep movsq\n\t"
        "mov 648(%rsp), %rsi\n\t"
        "test %rsi, %rsi\n\t"
        "jz 1f\n\t"
        "mov $0x1001, %edi\n\t"
        "mov $158, %eax\n\t"
        "syscall\n\t"
        "1:\n\t"
        "mov 528(%rsp), %rdi\n\t"
        "mov 536(%rsp), %rsi\n\t"
        "mov 544(%rsp), %rdx\n\t"
        "mov 552(%rsp), %rcx\n\t"
        "mov 560(%rsp), %r8\n\t"
        "mov 568(%rsp), %r9\n\t"
        "mov 520(%rsp), %r11\n\t"
        "mov %r11, kb_current_external_call_target(%rip)\n\t"
        "mov 624(%rsp), %r10\n\t"
        "mov %r10, kb_current_external_call_caller_gs(%rip)\n\t"
        "mov 648(%rsp), %r10\n\t"
        "mov %r10, kb_current_external_call_callee_gs(%rip)\n\t"
        "xor %eax, %eax\n\t"
        "call *%r11\n\t"
        "mov %rax, 632(%rsp)\n\t"
        "mov %rdx, 640(%rsp)\n\t"
        "movq $0, kb_current_external_call_target(%rip)\n\t"
        "mov 624(%rsp), %rsi\n\t"
        "mov $0x1001, %edi\n\t"
        "mov $158, %eax\n\t"
        "syscall\n\t"
        "2:\n\t"
        "mov 632(%rsp), %rax\n\t"
        "mov 640(%rsp), %rdx\n\t"
        "mov 576(%rsp), %rbx\n\t"
        "mov 584(%rsp), %rbp\n\t"
        "mov 592(%rsp), %r12\n\t"
        "mov 600(%rsp), %r13\n\t"
        "mov 608(%rsp), %r14\n\t"
        "mov 616(%rsp), %r15\n\t"
        "mov 512(%rsp), %r11\n\t"
        "mov %r11, %rsp\n\t"
        "ret\n\t");
}

#define KB_DEFINE_X86_INDIRECT_THUNK(name, reg) \
    __attribute__((naked)) static void name(void) \
    { \
        __asm__("jmp *%" reg); \
    }
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rax, "rax")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rbx, "rbx")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rcx, "rcx")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rdx, "rdx")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r8, "r8")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r9, "r9")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r10, "r10")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r11, "r11")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r12, "r12")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r13, "r13")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r14, "r14")
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r15, "r15")
#undef KB_DEFINE_X86_INDIRECT_THUNK
#else
static void kb_shim_external_call_trampoline(void)
{
    abort();
}

#define KB_DEFINE_X86_INDIRECT_THUNK(name) static void name(void) { kb_noop(); }
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rax)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rbx)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rcx)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rdx)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r8)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r9)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r10)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r11)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r12)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r13)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r14)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_r15)
#undef KB_DEFINE_X86_INDIRECT_THUNK
#endif

static int kb_nvidia_mock_nv_pci_count_devices(void)
{
    return 1;
}

static int kb_low_or_err_pointer(const void *ptr);

static int kb_ascii_strcasecmp(const char *a, const char *b)
{
    if (kb_low_or_err_pointer(a)) {
        a = "";
    }
    if (kb_low_or_err_pointer(b)) {
        b = "";
    }
    for (;;) {
        const unsigned char ca = (unsigned char)*a++;
        const unsigned char cb = (unsigned char)*b++;
        const int da = tolower(ca);
        const int db = tolower(cb);
        if (da != db || ca == '\0' || cb == '\0') {
            return da - db;
        }
    }
}

static int kb_ascii_strncasecmp(const char *a, const char *b, size_t n)
{
    if (kb_low_or_err_pointer(a)) {
        a = "";
    }
    if (kb_low_or_err_pointer(b)) {
        b = "";
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char ca = (unsigned char)a[i];
        const unsigned char cb = (unsigned char)b[i];
        const int da = tolower(ca);
        const int db = tolower(cb);
        if (da != db || ca == '\0' || cb == '\0') {
            return da - db;
        }
    }
    return 0;
}

static int kb_low_or_err_pointer(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return ptr == NULL || value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static size_t kb_safe_strlen(const char *value)
{
    if (kb_low_or_err_pointer(value)) {
        return 0;
    }
    return strlen(value);
}

static size_t kb_safe_strnlen(const char *value, size_t maxlen)
{
    if (kb_low_or_err_pointer(value)) {
        return 0;
    }
    return strnlen(value, maxlen);
}

static int kb_safe_strcmp(const char *a, const char *b)
{
    if (kb_low_or_err_pointer(a)) {
        a = "";
    }
    if (kb_low_or_err_pointer(b)) {
        b = "";
    }
    return strcmp(a, b);
}

static int kb_safe_strncmp(const char *a, const char *b, size_t n)
{
    if (kb_low_or_err_pointer(a)) {
        a = "";
    }
    if (kb_low_or_err_pointer(b)) {
        b = "";
    }
    return strncmp(a, b, n);
}

static char *kb_safe_strchr(const char *value, int c)
{
    if (kb_low_or_err_pointer(value)) {
        return NULL;
    }
    return strchr(value, c);
}

static char *kb_safe_strrchr(const char *value, int c)
{
    if (kb_low_or_err_pointer(value)) {
        return NULL;
    }
    return strrchr(value, c);
}

static char *kb_safe_strstr(const char *haystack, const char *needle)
{
    if (kb_low_or_err_pointer(haystack)) {
        return NULL;
    }
    if (kb_low_or_err_pointer(needle)) {
        needle = "";
    }
    return strstr(haystack, needle);
}

static char *kb_safe_strcpy(char *dst, const char *src)
{
    if (kb_low_or_err_pointer(dst)) {
        return dst;
    }
    if (kb_low_or_err_pointer(src)) {
        src = "";
    }
    return strcpy(dst, src);
}

static char *kb_safe_strncpy(char *dst, const char *src, size_t n)
{
    if (kb_low_or_err_pointer(dst)) {
        return dst;
    }
    if (kb_low_or_err_pointer(src)) {
        src = "";
    }
    return strncpy(dst, src, n);
}

static char *kb_safe_strncat(char *dst, const char *src, size_t n)
{
    if (kb_low_or_err_pointer(dst)) {
        return dst;
    }
    if (kb_low_or_err_pointer(src)) {
        src = "";
    }
    return strncat(dst, src, n);
}

static void *kb_safe_memset(void *dst, int value, size_t n)
{
    if (n == 0 || kb_low_or_err_pointer(dst)) {
        return dst;
    }
    return memset(dst, value, n);
}

static void *kb_safe_memcpy(void *dst, const void *src, size_t n)
{
    if (n == 0 || kb_low_or_err_pointer(dst) || kb_low_or_err_pointer(src)) {
        return dst;
    }
    return memcpy(dst, src, n);
}

static void *kb_safe_memmove(void *dst, const void *src, size_t n)
{
    if (n == 0 || kb_low_or_err_pointer(dst) || kb_low_or_err_pointer(src)) {
        return dst;
    }
    return memmove(dst, src, n);
}

static int kb_safe_memcmp(const void *a, const void *b, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (kb_low_or_err_pointer(a) || kb_low_or_err_pointer(b)) {
        return a == b ? 0 : 1;
    }
    return memcmp(a, b, n);
}

static int kb_sscanf_safe(const char *str, const char *fmt, ...)
{
    (void)str;
    (void)fmt;
    return 0;
}

static char *kb_ascii_strsep(char **stringp, const char *delim)
{
    if (kb_low_or_err_pointer(stringp) || kb_low_or_err_pointer(delim)) {
        return NULL;
    }
    char *start = *stringp;
    if (start == NULL) {
        return NULL;
    }
    char *p = start;
    while (*p != '\0') {
        if (strchr(delim, *p) != NULL) {
            *p = '\0';
            *stringp = p + 1;
            return start;
        }
        p++;
    }
    *stringp = NULL;
    return start;
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

static char *kb_kobject_get_path_shim(void *kobj, unsigned int flags)
{
    (void)kobj;
    (void)flags;
    return NULL;
}

static int trace_kernel_objects_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_KERNEL_OBJECTS");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static uint32_t kb_decode_major(uint64_t dev)
{
    return (uint32_t)(dev >> 20);
}

static uint32_t kb_decode_minor(uint64_t dev)
{
    return (uint32_t)(dev & 0xfffffu);
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

static void kb_write_u64_field(void *base, size_t offset, uint64_t value)
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

static const char *kb_chrdev_name_for_dev(uint64_t dev)
{
    const uint32_t major = kb_decode_major(dev);
    const uint32_t minor = kb_decode_minor(dev);
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
            const char *name = kb_chrdev_name_for_dev(kb_cdev_records[i].dev);
            fprintf(
                stderr,
                "kobox-kobj-summary: cdev active=%d major=%u minor=%u count=%u name=%s cdev=%p fops=%p\n",
                kb_cdev_records[i].active,
                kb_decode_major(kb_cdev_records[i].dev),
                kb_decode_minor(kb_cdev_records[i].dev),
                kb_cdev_records[i].count,
                name,
                kb_cdev_records[i].cdev,
                kb_cdev_records[i].fops);
            if (kb_cdev_records[i].has_fops_view) {
                fprintf(
                    stderr,
                    "kobox-fops-summary: kind=cdev name=%s major=%u minor=%u fops=%p owner=%p llseek=%p read=%p write=%p read_iter=%p write_iter=%p poll=%p ioctl=%p compat_ioctl=%p mmap=%p open=%p release=%p\n",
                    name,
                    kb_decode_major(kb_cdev_records[i].dev),
                    kb_decode_minor(kb_cdev_records[i].dev),
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

static int kb_vprintk(const char *fmt, va_list args)
{
    return kb_vprintk_safe(fmt, args);
}

static int kb_register_chrdev_stub(unsigned major, unsigned baseminor, unsigned count, const char *name, void *fops)
{
    kb_register_kernel_object_summary();
    const unsigned assigned_major = major == 0 ? kb_next_dynamic_major++ : major;
    const size_t slot = kb_find_free_chrdev_slot();
    if (slot == SIZE_MAX) {
        return -12;
    }
    kb_chrdev_records[slot].active = 1;
    kb_chrdev_records[slot].owner_module = kb_active_module;
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

static uint32_t kb_encode_dev(unsigned major, unsigned minor)
{
    return (major << 20) | (minor & 0xfffffu);
}

static int kb_alloc_chrdev_region_stub(uint32_t *dev, unsigned baseminor, unsigned count, const char *name)
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
    *dev = kb_encode_dev(assigned_major, baseminor);
    kb_chrdev_records[slot].active = 1;
    kb_chrdev_records[slot].owner_module = kb_active_module;
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

static void kb_unregister_chrdev_stub(unsigned major, unsigned baseminor, unsigned count, const char *name)
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

static void kb_unregister_chrdev_region_stub(uint32_t dev, unsigned count)
{
    const unsigned major = kb_decode_major(dev);
    const unsigned minor = kb_decode_minor(dev);
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

static void *kb_proc_create_data_stub(const char *name, unsigned mode, void *parent, void *ops, void *data)
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
    kb_proc_records[slot].owner_module = kb_active_module;
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

static void *kb_proc_mkdir_mode_stub(const char *name, unsigned mode, void *parent)
{
    return kb_proc_create_data_stub(name, mode, parent, NULL, NULL);
}

static void kb_proc_remove_stub(void *entry)
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

static void kb_remove_proc_entry_stub(const char *name, void *parent)
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

static void *kb_class_create_stub(const char *name)
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

static void kb_class_destroy_stub(void *class_ptr)
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

static void kb_cdev_init_stub(void *cdev, void *fops)
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
    kb_cdev_records[slot].owner_module = kb_active_module;
    if (fops != NULL) {
        kb_cdev_records[slot].fops = fops;
        kb_cdev_records[slot].fops_view = kb_decode_file_ops(fops);
        kb_cdev_records[slot].has_fops_view = 1;
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-kobj: cdev_init cdev=%p fops=%p\n", cdev, fops);
    }
}

static int kb_cdev_add_stub(void *cdev, uint64_t dev, unsigned count)
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
        kb_cdev_records[slot].owner_module = kb_active_module;
    }
    kb_cdev_records[slot].dev = dev;
    kb_cdev_records[slot].count = count;
    if (trace_kernel_objects_enabled()) {
        fprintf(
            stderr,
            "kobox-kobj: cdev_add major=%u minor=%u count=%u cdev=%p\n",
            kb_decode_major(dev),
            kb_decode_minor(dev),
            count,
            cdev);
    }
    return 0;
}

static void kb_cdev_del_stub(void *cdev)
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

static int kb_get_unused_fd_flags(unsigned flags)
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

static void kb_fd_install(unsigned fd, void *file)
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

static void *kb_fget(unsigned fd)
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

static void kb_fput(void *file)
{
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: fput file=%p\n", file);
    }
}

static int kb_close_fd(unsigned fd)
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

static void *kb_filp_open(const char *path, int flags, unsigned mode)
{
    (void)flags;
    (void)mode;
    void *file = kb_alloc_fake_file(path);
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-fd: filp_open path=%s file=%p\n", path == NULL ? "(null)" : path, file);
    }
    return file == NULL ? kb_err_ptr_noent() : file;
}

static int kb_filp_close(void *file, void *id)
{
    (void)id;
    kb_release_fake_file(file);
    return 0;
}

typedef int (*kb_iterate_fd_fn)(const void *file, unsigned fd, void *data);

static int kb_iterate_fd(void *files, unsigned first, kb_iterate_fd_fn fn, void *data)
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

static void kb_prepare_fake_vma(uint8_t *vma, void *mm, uint64_t start, uint64_t end, uint64_t pgoff)
{
    memset(vma, 0, KB_FAKE_VMA_SIZE);
    kb_write_u64_field(vma, 0x00, start);
    kb_write_u64_field(vma, 0x08, end);
    kb_write_ptr_field(vma, 0x10, mm);
    kb_write_u64_field(vma, 0x20, 0x0bull);
    kb_write_u64_field(vma, 0x28, 0);
    kb_write_ptr_field(vma, 0x30, vma + 0x30);
    kb_write_ptr_field(vma, 0x40, mm);
    kb_write_u64_field(vma, 0x78, 0);
    kb_write_u64_field(vma, 0x80, pgoff);
    kb_write_ptr_field(vma, 0x88, NULL);
    kb_write_ptr_field(vma, 0x90, NULL);
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
            kb_prepare_fake_vma(vma, mm, kb_vma_records[i].start, kb_vma_records[i].end, kb_vma_records[i].pgoff);
            return &kb_vma_records[i];
        }
    }
    return NULL;
}

static kb_vma_record_t *kb_find_vma_record(void *mm, uint64_t addr)
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

static void *kb_find_vma(void *mm, unsigned long addr)
{
    kb_vma_record_t *record = kb_find_vma_record(mm, addr);
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: find_vma mm=%p addr=0x%lx vma=%p\n", mm, addr, record == NULL ? NULL : (void *)record->vma);
    }
    return record == NULL ? NULL : record->vma;
}

static void *kb_find_vma_intersection(void *mm, unsigned long start, unsigned long end)
{
    (void)end;
    return kb_find_vma(mm, start);
}

static int kb_follow_pfn(void *vma, unsigned long address, unsigned long *pfn)
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

static long kb_pin_user_pages_common(void *mm, unsigned long start, unsigned long nr_pages, void **pages)
{
    if (nr_pages == 0) {
        return 0;
    }
    for (unsigned long i = 0; i < nr_pages; i++) {
        kb_vma_record_t *record = kb_find_vma_record(mm, start + (i << 12));
        if (pages != NULL) {
            pages[i] = record == NULL ? NULL : record->page;
        }
    }
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: pin_user_pages mm=%p start=0x%lx nr=%lu\n", mm, start, nr_pages);
    }
    return (long)nr_pages;
}

static long kb_pin_user_pages(unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *vmas)
{
    (void)gup_flags;
    (void)vmas;
    return kb_pin_user_pages_common(NULL, start, nr_pages, pages);
}

static long kb_pin_user_pages_remote(void *mm, unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *locked)
{
    (void)gup_flags;
    if (locked != NULL) {
        *(int *)locked = 0;
    }
    return kb_pin_user_pages_common(mm, start, nr_pages, pages);
}

static long kb_get_user_pages_remote(void *mm, unsigned long start, unsigned long nr_pages, unsigned int gup_flags, void **pages, void *vmas, void *locked)
{
    (void)gup_flags;
    (void)vmas;
    if (locked != NULL) {
        *(int *)locked = 0;
    }
    return kb_pin_user_pages_common(mm, start, nr_pages, pages);
}

static void kb_unpin_user_page(void *page)
{
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: unpin_user_page page=%p\n", page);
    }
}

static int kb_remap_pfn_range(void *vma, unsigned long addr, unsigned long pfn, unsigned long size, uint64_t prot)
{
    (void)prot;
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: remap_pfn_range vma=%p addr=0x%lx pfn=0x%lx size=0x%lx\n", vma, addr, pfn, size);
    }
    return 0;
}

static int kb_vmf_insert_pfn(void *vma, unsigned long addr, unsigned long pfn)
{
    if (trace_kernel_objects_enabled()) {
        fprintf(stderr, "kobox-vma: vmf_insert_pfn vma=%p addr=0x%lx pfn=0x%lx\n", vma, addr, pfn);
    }
    return 0;
}

static size_t kb_ascii_strlcat(char *dst, const char *src, size_t size)
{
    const size_t dst_len = strnlen(dst, size);
    const size_t src_len = strlen(src);
    if (dst_len == size) {
        return size + src_len;
    }
    const size_t room = size - dst_len - 1;
    const size_t copy_len = src_len < room ? src_len : room;
    if (copy_len != 0) {
        memcpy(dst + dst_len, src, copy_len);
    }
    dst[dst_len + copy_len] = '\0';
    return dst_len + src_len;
}

static long kb_strscpy_shim(char *dst, const char *src, size_t size)
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
    if (i < size) {
        dst[i] = '\0';
    }
    if (src[i] != '\0') {
        return -7;
    }
    return (long)i;
}

static int kb_utf16s_to_utf8s_shim(
    const uint16_t *src,
    int utf16_len,
    int endian,
    char *dst,
    int dst_len)
{
    if (dst == NULL || dst_len <= 0) {
        return 0;
    }
    if (src == NULL || utf16_len <= 0) {
        dst[0] = '\0';
        return 0;
    }

    int written = 0;
    for (int i = 0; i < utf16_len && written < dst_len; i++) {
        uint16_t ch = src[i];
        if (endian == 2) {
            ch = (uint16_t)((ch >> 8) | (ch << 8));
        }
        if (ch == 0) {
            break;
        }
        if (ch < 0x80) {
            dst[written++] = (char)ch;
        } else if (ch < 0x800) {
            if (written + 2 > dst_len) {
                break;
            }
            dst[written++] = (char)(0xc0u | (ch >> 6));
            dst[written++] = (char)(0x80u | (ch & 0x3fu));
        } else {
            if (written + 3 > dst_len) {
                break;
            }
            dst[written++] = (char)(0xe0u | (ch >> 12));
            dst[written++] = (char)(0x80u | ((ch >> 6) & 0x3fu));
            dst[written++] = (char)(0x80u | (ch & 0x3fu));
        }
    }
    return written;
}

static char *kb_kstrdup_shim(const char *src, unsigned int flags)
{
    if (src == NULL) {
        return NULL;
    }
    const size_t len = strlen(src) + 1;
    char *dst = kb_kmalloc(len, flags);
    if (dst != NULL) {
        memcpy(dst, src, len);
    }
    return dst;
}

static void *kb_memdup_user_shim(const void *src, size_t len)
{
    if (src == NULL && len != 0) {
        return NULL;
    }
    void *dst = kb_kmalloc(len == 0 ? 1 : len, 0);
    if (dst != NULL && len != 0) {
        memcpy(dst, src, len);
    }
    return dst;
}

static int kb_scnprintf_shim(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vsnprintf_safe(buf, size, fmt, args);
    va_end(args);
    if (written < 0) {
        return written;
    }
    if (size == 0) {
        return 0;
    }
    const size_t max_written = size - 1u;
    return (size_t)written > max_written ? (int)max_written : written;
}

static int kb_vscnprintf_shim(char *buf, size_t size, const char *fmt, va_list args)
{
    int written = kb_vsnprintf_safe(buf, size, fmt, args);
    if (written < 0) {
        return written;
    }
    if (size == 0) {
        return 0;
    }
    const size_t max_written = size - 1u;
    return (size_t)written > max_written ? (int)max_written : written;
}

static void *kb_devm_ioremap_shim(void *dev, uint64_t phys_addr, size_t size)
{
    (void)dev;
    return kb_ioremap(phys_addr, size);
}

static unsigned char kb_tracepoint_disabled[128];

static const shim_symbol_t shim_symbols[] = {
    {"__fentry__", (void *)(uintptr_t)&kb_noop},
    {"__x86_return_thunk", (void *)(uintptr_t)&kb_noop},
    {"kobox_x86_save_flags_if_enabled", (void *)(uintptr_t)&kb_x86_save_flags_if_enabled},
    {"kobox_x86_read_pat_msr", (void *)(uintptr_t)&kb_x86_read_pat_msr},
    {"kobox_nvidia_mock_nv_pci_count_devices", (void *)(uintptr_t)&kb_nvidia_mock_nv_pci_count_devices},
    {"BUG_func", (void *)(uintptr_t)&kb_stack_chk_fail},
    {"_printk", (void *)(uintptr_t)&kb_printk},
    {"printk", (void *)(uintptr_t)&kb_printk},
    {"vprintk", (void *)(uintptr_t)&kb_vprintk},
    {"kfree", (void *)(uintptr_t)&kb_kfree},
    {"kmalloc", (void *)(uintptr_t)&kb_kmalloc},
    {"kzalloc", (void *)(uintptr_t)&kb_kzalloc},
    {"kmalloc_trace", (void *)(uintptr_t)&kb_kmalloc_trace},
    {"request_threaded_irq", (void *)(uintptr_t)&kb_request_threaded_irq},
    {"free_irq", (void *)(uintptr_t)&kb_free_irq},
    {"dma_alloc_attrs", (void *)(uintptr_t)&kb_dma_alloc_attrs},
    {"dma_free_attrs", (void *)(uintptr_t)&kb_dma_free_attrs},
    {"__stack_chk_fail", (void *)(uintptr_t)&kb_stack_chk_fail},
    {"__pci_register_driver", (void *)(uintptr_t)&kb_pci_register_driver},
    {"pci_unregister_driver", (void *)(uintptr_t)&kb_pci_unregister_driver},
    {"pci_enable_device", (void *)(uintptr_t)&kb_pci_enable_device},
    {"pcim_enable_device", (void *)(uintptr_t)&kb_pcim_enable_device},
    {"pci_disable_device", (void *)(uintptr_t)&kb_pci_disable_device},
    {"pci_set_master", (void *)(uintptr_t)&kb_pci_set_master},
    {"pci_iomap", (void *)(uintptr_t)&kb_pci_iomap},
    {"pcim_iomap", (void *)(uintptr_t)&kb_pcim_iomap},
    {"pci_iounmap", (void *)(uintptr_t)&kb_pci_iounmap},
    {"kobox_xhci_ring_cmd_db", (void *)(uintptr_t)&kb_pci_xhci_ring_cmd_db},
    {"ioread8", (void *)(uintptr_t)&kb_ioread8},
    {"iowrite8", (void *)(uintptr_t)&kb_iowrite8},
    {"ioread32", (void *)(uintptr_t)&kb_ioread32},
    {"iowrite32", (void *)(uintptr_t)&kb_iowrite32},
    {"__platform_driver_register", (void *)(uintptr_t)&kb_platform_driver_register},
    {"platform_driver_unregister", (void *)(uintptr_t)&kb_platform_driver_unregister},
    {"__devm_add_action", (void *)(uintptr_t)&kb_devm_add_action},
    {"__devm_reset_control_get", (void *)(uintptr_t)&kb_return_zero},
    {"__devm_uio_register_device", (void *)(uintptr_t)&kb_devm_uio_register_device},
    {"__dynamic_dev_dbg", (void *)(uintptr_t)&kb_dynamic_dev_dbg},
    {"_dev_err", (void *)(uintptr_t)&kb_dev_err},
    {"_dev_warn", (void *)(uintptr_t)&kb_dev_warn},
    {"hub_port_debounce", (void *)(uintptr_t)&kb_usb_hub_port_debounce},
    {"___drm_dbg", (void *)(uintptr_t)&kb_noop},
    {"__drm_err", (void *)(uintptr_t)&kb_noop},
    {"pm_runtime_enable", (void *)(uintptr_t)&kb_pm_runtime_enable},
    {"__pm_runtime_disable", (void *)(uintptr_t)&kb_pm_runtime_disable},
    {"__pm_runtime_idle", (void *)(uintptr_t)&kb_pm_runtime_idle},
    {"__pm_runtime_resume", (void *)(uintptr_t)&kb_pm_runtime_resume},
    {"pm_vt_switch_register", (void *)(uintptr_t)&kb_return_zero},
    {"pm_vt_switch_required", (void *)(uintptr_t)&kb_return_zero},
    {"pm_vt_switch_unregister", (void *)(uintptr_t)&kb_noop},
    {"devm_kmalloc", (void *)(uintptr_t)&kb_devm_kmalloc},
    {"devm_kasprintf", (void *)(uintptr_t)&kb_devm_kasprintf},
    {"devres_close_group", (void *)(uintptr_t)&kb_noop},
    {"devres_open_group", (void *)(uintptr_t)&kb_identity_ptr},
    {"devres_release_group", (void *)(uintptr_t)&kb_return_zero},
    {"devres_remove_group", (void *)(uintptr_t)&kb_noop},
    {"platform_get_irq_optional", (void *)(uintptr_t)&kb_platform_get_irq_optional},
    {"disable_irq_nosync", (void *)(uintptr_t)&kb_disable_irq_nosync},
    {"enable_irq", (void *)(uintptr_t)&kb_enable_irq},
    {"irq_get_irq_data", (void *)(uintptr_t)&kb_irq_get_irq_data},
    {"irq_modify_status", (void *)(uintptr_t)&kb_irq_modify_status},
    {"_raw_spin_lock", (void *)(uintptr_t)&kb_raw_spin_lock},
    {"_raw_spin_trylock", (void *)(uintptr_t)&kb_raw_spin_trylock},
    {"_raw_spin_lock_irqsave", (void *)(uintptr_t)&kb_raw_spin_lock_irqsave},
    {"_raw_spin_unlock", (void *)(uintptr_t)&kb_raw_spin_unlock},
    {"_raw_spin_unlock_irqrestore", (void *)(uintptr_t)&kb_raw_spin_unlock_irqrestore},
    {"_raw_read_lock", (void *)(uintptr_t)&kb_noop},
    {"_raw_read_unlock", (void *)(uintptr_t)&kb_noop},
    {"atomic_notifier_chain_register", (void *)(uintptr_t)&kb_atomic_notifier_chain_register},
    {"atomic_notifier_chain_unregister", (void *)(uintptr_t)&kb_atomic_notifier_chain_unregister},
    {"kexec_crash_loaded", (void *)(uintptr_t)&kb_kexec_crash_loaded},
    {"kstrtouint", (void *)(uintptr_t)&kb_kstrtouint},
    {"sysfs_emit", (void *)(uintptr_t)&kb_sysfs_emit},
    {"__kmalloc", (void *)(uintptr_t)&kb_kmalloc_alias},
    {"__SCT__might_resched", (void *)(uintptr_t)&kb_might_resched},
    {"__SCT__preempt_schedule", (void *)(uintptr_t)&kb_noop},
    {"__ubsan_handle_load_invalid_value", (void *)(uintptr_t)&kb_ubsan_handle_load_invalid_value},
    {"__ubsan_handle_out_of_bounds", (void *)(uintptr_t)&kb_ubsan_handle_out_of_bounds},
    {"register_virtio_driver", (void *)(uintptr_t)&kb_register_virtio_driver},
    {"unregister_virtio_driver", (void *)(uintptr_t)&kb_unregister_virtio_driver},
    {"virtio_reset_device", (void *)(uintptr_t)&kb_virtio_reset_device},
    {"virtqueue_add_inbuf", (void *)(uintptr_t)&kb_virtqueue_add_inbuf},
    {"virtqueue_add_outbuf", (void *)(uintptr_t)&kb_virtqueue_add_outbuf},
    {"virtqueue_detach_unused_buf", (void *)(uintptr_t)&kb_virtqueue_detach_unused_buf},
    {"virtqueue_get_buf", (void *)(uintptr_t)&kb_virtqueue_get_buf},
    {"virtqueue_get_vring_size", (void *)(uintptr_t)&kb_virtqueue_get_vring_size},
    {"virtqueue_kick", (void *)(uintptr_t)&kb_virtqueue_kick},
    {"sg_init_one", (void *)(uintptr_t)&kb_sg_init_one},
    {"input_allocate_device", (void *)(uintptr_t)&kb_input_allocate_device},
    {"input_free_device", (void *)(uintptr_t)&kb_input_free_device},
    {"input_register_device", (void *)(uintptr_t)&kb_input_register_device},
    {"input_unregister_device", (void *)(uintptr_t)&kb_input_unregister_device},
    {"input_event", (void *)(uintptr_t)&kb_input_event},
    {"input_ff_event", (void *)(uintptr_t)&kb_return_zero},
    {"input_scancode_to_scalar", (void *)(uintptr_t)&kb_return_zero},
    {"input_set_abs_params", (void *)(uintptr_t)&kb_input_set_abs_params},
    {"input_alloc_absinfo", (void *)(uintptr_t)&kb_input_alloc_absinfo},
    {"input_mt_init_slots", (void *)(uintptr_t)&kb_input_mt_init_slots},
    {"snprintf", (void *)(uintptr_t)&kb_snprintf_safe},
    {"vsnprintf", (void *)(uintptr_t)&kb_vsnprintf_safe},
    {"__SCT__cond_resched", (void *)(uintptr_t)&kb_cond_resched},
    {"__alloc_percpu_gfp", (void *)(uintptr_t)&kb_alloc_percpu_gfp},
    {"free_percpu", (void *)(uintptr_t)&kb_free_percpu},
    {"__rtnl_link_register", (void *)(uintptr_t)&kb_rtnl_link_register},
    {"__rtnl_link_unregister", (void *)(uintptr_t)&kb_rtnl_link_unregister},
    {"rtnl_link_unregister", (void *)(uintptr_t)&kb_rtnl_link_unregister},
    {"rtnl_lock", (void *)(uintptr_t)&kb_rtnl_lock},
    {"rtnl_unlock", (void *)(uintptr_t)&kb_rtnl_unlock},
    {"register_netdevice", (void *)(uintptr_t)&kb_register_netdevice},
    {"alloc_netdev_mqs", (void *)(uintptr_t)&kb_alloc_netdev_mqs},
    {"free_netdev", (void *)(uintptr_t)&kb_free_netdev},
    {"ether_setup", (void *)(uintptr_t)&kb_ether_setup},
    {"eth_mac_addr", (void *)(uintptr_t)&kb_eth_mac_addr},
    {"eth_validate_addr", (void *)(uintptr_t)&kb_eth_validate_addr},
    {"dev_addr_mod", (void *)(uintptr_t)&kb_dev_addr_mod},
    {"netif_carrier_on", (void *)(uintptr_t)&kb_netif_carrier_on},
    {"netif_carrier_off", (void *)(uintptr_t)&kb_netif_carrier_off},
    {"get_random_bytes", (void *)(uintptr_t)&kb_get_random_bytes},
    {"consume_skb", (void *)(uintptr_t)&kb_consume_skb},
    {"skb_tstamp_tx", (void *)(uintptr_t)&kb_skb_tstamp_tx},
    {"skb_clone_tx_timestamp", (void *)(uintptr_t)&kb_skb_clone_tx_timestamp},
    {"dev_lstats_read", (void *)(uintptr_t)&kb_dev_lstats_read},
    {"ethtool_op_get_ts_info", (void *)(uintptr_t)&kb_ethtool_op_get_ts_info},
    {"down_write", (void *)(uintptr_t)&kb_down_write},
    {"up_write", (void *)(uintptr_t)&kb_up_write},
    {"_find_next_bit", (void *)(uintptr_t)&kb_find_next_bit},
    {"__SCT__preempt_schedule_notrace", (void *)(uintptr_t)&kb_noop},
    {"___ratelimit", (void *)(uintptr_t)&kb_return_zero},
    {"__bitmap_weight", (void *)(uintptr_t)&kb_bitmap_weight},
    {"__bitmap_and", (void *)(uintptr_t)&kb_bitmap_and_shim},
    {"__bitmap_andnot", (void *)(uintptr_t)&kb_bitmap_andnot_shim},
    {"__bitmap_clear", (void *)(uintptr_t)&kb_bitmap_clear_shim},
    {"__bitmap_complement", (void *)(uintptr_t)&kb_bitmap_complement_shim},
    {"__bitmap_intersects", (void *)(uintptr_t)&kb_bitmap_intersects_shim},
    {"__bitmap_or", (void *)(uintptr_t)&kb_bitmap_or_shim},
    {"__bitmap_set", (void *)(uintptr_t)&kb_bitmap_set_shim},
    {"__bitmap_shift_left", (void *)(uintptr_t)&kb_bitmap_shift_left_shim},
    {"__bitmap_shift_right", (void *)(uintptr_t)&kb_bitmap_shift_right_shim},
    {"__do_once_done", (void *)(uintptr_t)&kb_noop},
    {"__do_once_start", (void *)(uintptr_t)&kb_return_zero},
    {"__dynamic_pr_debug", (void *)(uintptr_t)&kb_noop},
    {"__flush_workqueue", (void *)(uintptr_t)&kb_noop},
    {"__init_swait_queue_head", (void *)(uintptr_t)&kb_init_swait_queue_head},
    {"__init_waitqueue_head", (void *)(uintptr_t)&kb_init_waitqueue_head},
    {"__init_rwsem", (void *)(uintptr_t)&kb_init_rwsem},
    {"__refrigerator", (void *)(uintptr_t)&kb_noop},
    {"__kmalloc_node", (void *)(uintptr_t)&kb_kmalloc_node},
    {"__msecs_to_jiffies", (void *)(uintptr_t)&kb_msecs_to_jiffies},
    {"__ndelay", (void *)(uintptr_t)&kb_ndelay},
    {"__mmap_lock_do_trace_acquire_returned", (void *)(uintptr_t)&kb_noop},
    {"__mmap_lock_do_trace_released", (void *)(uintptr_t)&kb_noop},
    {"__mmap_lock_do_trace_start_locking", (void *)(uintptr_t)&kb_noop},
    {"__mutex_init", (void *)(uintptr_t)&kb_mutex_init},
    {"__sw_hweight32", (void *)(uintptr_t)&kb_hweight32},
    {"__sw_hweight64", (void *)(uintptr_t)&kb_hweight64},
    {"__task_pid_nr_ns", (void *)(uintptr_t)&kb_return_zero},
    {"__ubsan_handle_shift_out_of_bounds", (void *)(uintptr_t)&kb_noop},
    {"__warn_printk", (void *)(uintptr_t)&kb_printk},
    {"_dev_info", (void *)(uintptr_t)&kb_dev_warn},
    {"_raw_spin_lock_irq", (void *)(uintptr_t)&kb_raw_spin_lock},
    {"_raw_spin_unlock_irq", (void *)(uintptr_t)&kb_raw_spin_unlock},
    {"base64_decode", (void *)(uintptr_t)&kb_return_zero},
    {"complete", (void *)(uintptr_t)&kb_complete},
    {"complete_all", (void *)(uintptr_t)&kb_complete_all},
    {"console_lock", (void *)(uintptr_t)&kb_noop},
    {"console_unlock", (void *)(uintptr_t)&kb_noop},
    {"cpufreq_get", (void *)(uintptr_t)&kb_return_zero},
    {"crc32_le", (void *)(uintptr_t)&kb_return_zero},
    {"cachemode2protval", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_page_attrs", (void *)(uintptr_t)&kb_dma_map_page_attrs},
    {"dma_map_resource", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_sgtable", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_sg_attrs", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_single_attrs", (void *)(uintptr_t)&kb_dma_map_single_attrs},
    {"dma_mapping_error", (void *)(uintptr_t)&kb_dma_mapping_error},
    {"dma_max_mapping_size", (void *)(uintptr_t)&kb_dma_max_mapping_size},
    {"dma_opt_mapping_size", (void *)(uintptr_t)&kb_return_zero},
    {"dma_pci_p2pdma_supported", (void *)(uintptr_t)&kb_return_zero},
    {"dma_pool_alloc", (void *)(uintptr_t)&kb_dma_pool_alloc},
    {"dma_pool_create", (void *)(uintptr_t)&kb_dma_pool_create},
    {"dma_pool_destroy", (void *)(uintptr_t)&kb_dma_pool_destroy},
    {"dma_pool_free", (void *)(uintptr_t)&kb_dma_pool_free},
    {"dma_fence_context_alloc", (void *)(uintptr_t)&kb_return_zero},
    {"dma_fence_default_wait", (void *)(uintptr_t)&kb_return_zero},
    {"dma_fence_init", (void *)(uintptr_t)&kb_noop},
    {"dma_fence_release", (void *)(uintptr_t)&kb_noop},
    {"dma_fence_signal", (void *)(uintptr_t)&kb_return_zero},
    {"dma_resv_add_fence", (void *)(uintptr_t)&kb_noop},
    {"dma_resv_reserve_fences", (void *)(uintptr_t)&kb_return_zero},
    {"dma_set_coherent_mask", (void *)(uintptr_t)&kb_dma_set_coherent_mask},
    {"dma_set_mask", (void *)(uintptr_t)&kb_dma_set_mask},
    {"dma_unmap_page_attrs", (void *)(uintptr_t)&kb_dma_unmap_page_attrs},
    {"dma_unmap_resource", (void *)(uintptr_t)&kb_noop},
    {"dma_unmap_sg_attrs", (void *)(uintptr_t)&kb_noop},
    {"dma_unmap_single_attrs", (void *)(uintptr_t)&kb_dma_unmap_single_attrs},
    {"dma_sync_single_for_cpu", (void *)(uintptr_t)&kb_noop},
    {"dma_sync_single_for_device", (void *)(uintptr_t)&kb_noop},
    {"dump_stack", (void *)(uintptr_t)&kb_noop},
    {"down", (void *)(uintptr_t)&kb_noop},
    {"down_interruptible", (void *)(uintptr_t)&kb_return_zero},
    {"down_read", (void *)(uintptr_t)&kb_down_read},
    {"down_read_trylock", (void *)(uintptr_t)&kb_return_one},
    {"down_trylock", (void *)(uintptr_t)&kb_return_zero},
    {"down_write_trylock", (void *)(uintptr_t)&kb_return_one},
    {"downgrade_write", (void *)(uintptr_t)&kb_noop},
    {"fasync_helper", (void *)(uintptr_t)&kb_return_zero},
    {"flush_work", (void *)(uintptr_t)&kb_flush_work},
    {"freezing_slow_path", (void *)(uintptr_t)&kb_return_zero},
    {"fortify_panic", (void *)(uintptr_t)&kb_stack_chk_fail},
    {"get_random_u32", (void *)(uintptr_t)&kb_return_zero},
    {"kfree_sensitive", (void *)(uintptr_t)&kb_kfree_sensitive},
    {"kmalloc_node_trace", (void *)(uintptr_t)&kb_kmalloc_node_trace},
    {"kmem_cache_alloc", (void *)(uintptr_t)&kb_kmem_cache_alloc},
    {"kmemdup", (void *)(uintptr_t)&kb_kmemdup},
    {"kstrtobool", (void *)(uintptr_t)&kb_return_zero},
    {"kstrtoint", (void *)(uintptr_t)&kb_return_zero},
    {"mempool_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"mempool_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"mempool_create_node", (void *)(uintptr_t)&kb_alloc_stub},
    {"mempool_destroy", (void *)(uintptr_t)&kb_noop},
    {"mempool_free", (void *)(uintptr_t)&kb_noop},
    {"mempool_kfree", (void *)(uintptr_t)&kb_kfree},
    {"mempool_kmalloc", (void *)(uintptr_t)&kb_kmalloc},
    {"mutex_lock", (void *)(uintptr_t)&kb_mutex_lock},
    {"mutex_lock_interruptible", (void *)(uintptr_t)&kb_return_zero},
    {"mutex_lock_killable", (void *)(uintptr_t)&kb_return_zero},
    {"mutex_is_locked", (void *)(uintptr_t)&kb_return_zero},
    {"mutex_trylock", (void *)(uintptr_t)&kb_mutex_trylock},
    {"mutex_unlock", (void *)(uintptr_t)&kb_mutex_unlock},
    {"pci_alloc_irq_vectors", (void *)(uintptr_t)&kb_pci_alloc_irq_vectors},
    {"pci_alloc_irq_vectors_affinity", (void *)(uintptr_t)&kb_pci_alloc_irq_vectors_affinity},
    {"pci_choose_state", (void *)(uintptr_t)&kb_pci_choose_state},
    {"pci_d3cold_disable", (void *)(uintptr_t)&kb_pci_d3cold_disable},
    {"pci_dev_get", (void *)(uintptr_t)&kb_pci_dev_get},
    {"pci_device_is_present", (void *)(uintptr_t)&kb_pci_device_is_present},
    {"pci_enable_device_mem", (void *)(uintptr_t)&kb_pci_enable_device_mem},
    {"pci_free_irq", (void *)(uintptr_t)&kb_pci_free_irq},
    {"pci_free_irq_vectors", (void *)(uintptr_t)&kb_pci_free_irq_vectors},
    {"pci_irq_vector", (void *)(uintptr_t)&kb_pci_irq_vector},
    {"pci_match_id", (void *)(uintptr_t)&kb_pci_match_id},
    {"pci_read_config_word", (void *)(uintptr_t)&kb_pci_read_config_word},
    {"pci_release_selected_regions", (void *)(uintptr_t)&kb_pci_release_selected_regions},
    {"pci_request_irq", (void *)(uintptr_t)&kb_pci_request_irq_shim},
    {"pci_request_selected_regions", (void *)(uintptr_t)&kb_pci_request_selected_regions},
    {"pci_restore_state", (void *)(uintptr_t)&kb_return_zero},
    {"pci_save_state", (void *)(uintptr_t)&kb_return_zero},
    {"pci_select_bars", (void *)(uintptr_t)&kb_pci_select_bars},
    {"pci_set_mwi", (void *)(uintptr_t)&kb_pci_set_mwi},
    {"pci_set_power_state", (void *)(uintptr_t)&kb_pci_set_power_state},
    {"pcie_aspm_enabled", (void *)(uintptr_t)&kb_return_zero},
    {"pcie_reset_flr", (void *)(uintptr_t)&kb_return_zero},
    {"sg_init_table", (void *)(uintptr_t)&kb_sg_init_one},
    {"sg_alloc_table_from_pages_segment", (void *)(uintptr_t)&kb_return_zero},
    {"sg_free_table", (void *)(uintptr_t)&kb_noop},
    {"sg_miter_next", (void *)(uintptr_t)&kb_sg_miter_next},
    {"sg_miter_skip", (void *)(uintptr_t)&kb_sg_miter_skip},
    {"sg_miter_start", (void *)(uintptr_t)&kb_sg_miter_start},
    {"sg_miter_stop", (void *)(uintptr_t)&kb_sg_miter_stop},
    {"sg_nents", (void *)(uintptr_t)&kb_sg_nents},
    {"sg_next", (void *)(uintptr_t)&kb_sg_next},
    {"sysfs_streq", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_update_group", (void *)(uintptr_t)&kb_return_zero},
    {"wait_for_completion", (void *)(uintptr_t)&kb_wait_for_completion},
    {"wait_for_completion_interruptible", (void *)(uintptr_t)&kb_wait_for_completion},
    {"wait_for_completion_interruptible_timeout", (void *)(uintptr_t)&kb_wait_for_completion_io_timeout},
    {"wait_for_completion_io_timeout", (void *)(uintptr_t)&kb_wait_for_completion_io_timeout},
    {"strlen", (void *)(uintptr_t)&kb_safe_strlen},
    {"strchr", (void *)(uintptr_t)&kb_safe_strchr},
    {"strcpy", (void *)(uintptr_t)&kb_safe_strcpy},
    {"strcasecmp", (void *)(uintptr_t)&kb_ascii_strcasecmp},
    {"strlcat", (void *)(uintptr_t)&kb_ascii_strlcat},
    {"strncat", (void *)(uintptr_t)&kb_safe_strncat},
    {"strncasecmp", (void *)(uintptr_t)&kb_ascii_strncasecmp},
    {"strncmp", (void *)(uintptr_t)&kb_safe_strncmp},
    {"strncpy", (void *)(uintptr_t)&kb_safe_strncpy},
    {"strncpy_from_user", (void *)(uintptr_t)&kb_strncpy_from_user},
    {"strsep", (void *)(uintptr_t)&kb_ascii_strsep},
    {"strstr", (void *)(uintptr_t)&kb_safe_strstr},
    {"strnlen", (void *)(uintptr_t)&kb_safe_strnlen},
    {"strnlen_user", (void *)(uintptr_t)&kb_strnlen_user},
    {"strrchr", (void *)(uintptr_t)&kb_safe_strrchr},
    {"sscanf", (void *)(uintptr_t)&kb_sscanf_safe},
    {"memset", (void *)(uintptr_t)&kb_safe_memset},
    {"memcpy", (void *)(uintptr_t)&kb_safe_memcpy},
    {"memmove", (void *)(uintptr_t)&kb_safe_memmove},
    {"memcmp", (void *)(uintptr_t)&kb_safe_memcmp},
    {"strcmp", (void *)(uintptr_t)&kb_safe_strcmp},
    {"sprintf", (void *)(uintptr_t)&kb_sprintf_safe},
    {"__SCK__tp_func_block_bio_complete", (void *)(uintptr_t)&kb_noop},
    {"__SCK__tp_func_block_bio_remap", (void *)(uintptr_t)&kb_noop},
    {"__SCK__tp_func_nvme_sq", (void *)(uintptr_t)&kb_noop},
    {"__SCK__tp_func_xhci_dbg_init", (void *)(uintptr_t)&kb_noop},
    {"__SCK__tp_func_xhci_dbg_quirks", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_block_bio_complete", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_block_bio_remap", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_nvme_sq", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_xhci_dbg_init", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_xhci_dbg_quirks", (void *)(uintptr_t)&kb_noop},
    {"__acpi_video_get_backlight_type", (void *)(uintptr_t)&kb_return_zero},
    {"__blk_alloc_disk", (void *)(uintptr_t)&kb_blk_alloc_disk},
    {"__blk_mq_alloc_disk", (void *)(uintptr_t)&kb_blk_mq_alloc_disk},
    {"__blk_rq_map_sg", (void *)(uintptr_t)&kb_return_zero},
    {"__alloc_pages", (void *)(uintptr_t)&kb_alloc_stub},
    {"__check_object_size", (void *)(uintptr_t)&kb_noop},
    {"__copy_overflow", (void *)(uintptr_t)&kb_noop},
    {"__const_udelay", (void *)(uintptr_t)&kb_const_udelay},
    {"__folio_lock", (void *)(uintptr_t)&kb_noop},
    {"__folio_put", (void *)(uintptr_t)&kb_noop},
    {"__free_pages", (void *)(uintptr_t)&kb_noop},
    {"__get_free_pages", (void *)(uintptr_t)&kb_alloc_stub},
    {"__io_uring_cmd_do_in_task", (void *)(uintptr_t)&kb_return_zero},
    {"__node_distance", (void *)(uintptr_t)&kb_return_zero},
    {"__put_user_4", (void *)(uintptr_t)&kb_return_zero},
    {"__put_user_8", (void *)(uintptr_t)&kb_return_zero},
    {"__put_devmap_managed_page_refs", (void *)(uintptr_t)&kb_noop},
    {"__printk_ratelimit", (void *)(uintptr_t)&kb_return_one},
    {"__udelay", (void *)(uintptr_t)&kb_udelay},
    {"__usecs_to_jiffies", (void *)(uintptr_t)&kb_usecs_to_jiffies},
    {"__release_region", (void *)(uintptr_t)&kb_noop},
    {"__request_region", (void *)(uintptr_t)&kb_alloc_stub},
    {"__srcu_read_lock", (void *)(uintptr_t)&kb_return_zero},
    {"__srcu_read_unlock", (void *)(uintptr_t)&kb_noop},
    {"__trace_trigger_soft_disabled", (void *)(uintptr_t)&kb_return_zero},
    {"__tracepoint_mmap_lock_acquire_returned", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_mmap_lock_released", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_mmap_lock_start_locking", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_block_bio_complete", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_block_bio_remap", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_nvme_sq", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__register_chrdev", (void *)(uintptr_t)&kb_register_chrdev_stub},
    {"__unregister_chrdev", (void *)(uintptr_t)&kb_unregister_chrdev_stub},
    {"__virt_addr_valid", (void *)(uintptr_t)&kb_return_one},
    {"__vmalloc", (void *)(uintptr_t)&kb_kzalloc},
    {"__wake_up", (void *)(uintptr_t)&kb_noop},
    {"__x86_indirect_thunk_r10", (void *)(uintptr_t)&kb_x86_indirect_thunk_r10},
    {"__x86_indirect_thunk_r11", (void *)(uintptr_t)&kb_x86_indirect_thunk_r11},
    {"__x86_indirect_thunk_r12", (void *)(uintptr_t)&kb_x86_indirect_thunk_r12},
    {"__x86_indirect_thunk_r13", (void *)(uintptr_t)&kb_x86_indirect_thunk_r13},
    {"__x86_indirect_thunk_r14", (void *)(uintptr_t)&kb_x86_indirect_thunk_r14},
    {"__x86_indirect_thunk_r15", (void *)(uintptr_t)&kb_x86_indirect_thunk_r15},
    {"__x86_indirect_thunk_r8", (void *)(uintptr_t)&kb_x86_indirect_thunk_r8},
    {"__x86_indirect_thunk_r9", (void *)(uintptr_t)&kb_x86_indirect_thunk_r9},
    {"__x86_indirect_thunk_rax", (void *)(uintptr_t)&kb_x86_indirect_thunk_rax},
    {"__x86_indirect_thunk_rbx", (void *)(uintptr_t)&kb_x86_indirect_thunk_rbx},
    {"__x86_indirect_thunk_rcx", (void *)(uintptr_t)&kb_x86_indirect_thunk_rcx},
    {"__x86_indirect_thunk_rdx", (void *)(uintptr_t)&kb_x86_indirect_thunk_rdx},
    {"__copy_from_user", (void *)(uintptr_t)&kb_copy_from_user},
    {"__copy_to_user", (void *)(uintptr_t)&kb_copy_to_user},
    {"_copy_from_user", (void *)(uintptr_t)&kb_copy_from_user},
    {"_copy_to_user", (void *)(uintptr_t)&kb_copy_to_user},
    {"copy_from_user", (void *)(uintptr_t)&kb_copy_from_user},
    {"copy_to_user", (void *)(uintptr_t)&kb_copy_to_user},
    {"raw_copy_from_user", (void *)(uintptr_t)&kb_copy_from_user},
    {"raw_copy_to_user", (void *)(uintptr_t)&kb_copy_to_user},
    {"_find_first_bit", (void *)(uintptr_t)&kb_find_first_bit_shim},
    {"_find_first_zero_bit", (void *)(uintptr_t)&kb_find_first_zero_bit_shim},
    {"_find_last_bit", (void *)(uintptr_t)&kb_find_last_bit_shim},
    {"_find_next_zero_bit", (void *)(uintptr_t)&kb_find_next_zero_bit_shim},
    {"acpi_storage_d3", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_evaluate_dsm", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_evaluate_integer", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_evaluate_object", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_get_handle", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_get_next_object", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_install_notify_handler", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_remove_notify_handler", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_walk_namespace", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_video_register_backlight", (void *)(uintptr_t)&kb_return_zero},
    {"add_uevent_var", (void *)(uintptr_t)&kb_return_zero},
    {"address_space_init_once", (void *)(uintptr_t)&kb_noop},
    {"alloc_chrdev_region", (void *)(uintptr_t)&kb_alloc_chrdev_region_stub},
    {"alloc_pages", (void *)(uintptr_t)&kb_alloc_stub},
    {"alloc_workqueue", (void *)(uintptr_t)&kb_alloc_stub},
    {"bdev_disk_changed", (void *)(uintptr_t)&kb_return_zero},
    {"bdev_end_io_acct", (void *)(uintptr_t)&kb_noop},
    {"bdev_start_io_acct", (void *)(uintptr_t)&kb_return_zero},
    {"bio_associate_blkg", (void *)(uintptr_t)&kb_noop},
    {"bio_endio", (void *)(uintptr_t)&kb_noop},
    {"bio_integrity_map_user", (void *)(uintptr_t)&kb_return_zero},
    {"bio_split_to_limits", (void *)(uintptr_t)&kb_return_zero},
    {"blk_execute_rq", (void *)(uintptr_t)&kb_blk_execute_rq},
    {"blk_execute_rq_nowait", (void *)(uintptr_t)&kb_blk_execute_rq},
    {"blk_freeze_queue_start", (void *)(uintptr_t)&kb_noop},
    {"blk_integrity_register", (void *)(uintptr_t)&kb_return_zero},
    {"blk_integrity_unregister", (void *)(uintptr_t)&kb_noop},
    {"blk_mark_disk_dead", (void *)(uintptr_t)&kb_blk_mark_disk_dead},
    {"blk_mq_alloc_request", (void *)(uintptr_t)&kb_blk_mq_alloc_request},
    {"blk_mq_alloc_request_hctx", (void *)(uintptr_t)&kb_blk_mq_alloc_request},
    {"blk_mq_alloc_tag_set", (void *)(uintptr_t)&kb_blk_mq_alloc_tag_set},
    {"blk_mq_complete_request", (void *)(uintptr_t)&kb_blk_mq_complete_request},
    {"blk_mq_complete_request_remote", (void *)(uintptr_t)&kb_blk_mq_complete_request_remote},
    {"blk_mq_delay_kick_requeue_list", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_destroy_queue", (void *)(uintptr_t)&kb_blk_mq_destroy_queue},
    {"blk_mq_end_request", (void *)(uintptr_t)&kb_blk_mq_end_request},
    {"blk_mq_end_request_batch", (void *)(uintptr_t)&kb_blk_mq_end_request_batch},
    {"blk_mq_free_request", (void *)(uintptr_t)&kb_blk_mq_free_request},
    {"blk_mq_free_tag_set", (void *)(uintptr_t)&kb_blk_mq_free_tag_set},
    {"blk_mq_freeze_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_freeze_queue_wait", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_freeze_queue_wait_timeout", (void *)(uintptr_t)&kb_return_zero},
    {"blk_mq_init_queue", (void *)(uintptr_t)&kb_blk_mq_init_queue},
    {"blk_mq_map_queues", (void *)(uintptr_t)&kb_blk_mq_map_queues},
    {"blk_mq_pci_map_queues", (void *)(uintptr_t)&kb_blk_mq_pci_map_queues},
    {"blk_mq_quiesce_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_quiesce_tagset", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_requeue_request", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_start_request", (void *)(uintptr_t)&kb_blk_mq_start_request},
    {"blk_mq_tagset_busy_iter", (void *)(uintptr_t)&kb_blk_mq_tagset_busy_iter},
    {"blk_mq_tagset_wait_completed_request", (void *)(uintptr_t)&kb_blk_mq_tagset_wait_completed_request},
    {"blk_mq_unfreeze_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_unquiesce_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_unquiesce_tagset", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_update_nr_hw_queues", (void *)(uintptr_t)&kb_blk_mq_update_nr_hw_queues},
    {"blk_mq_wait_quiesce_done", (void *)(uintptr_t)&kb_noop},
    {"blk_op_str", (void *)(uintptr_t)&kb_empty_string},
    {"blk_put_queue", (void *)(uintptr_t)&kb_blk_put_queue},
    {"blk_queue_bounce_limit", (void *)(uintptr_t)&kb_blk_queue_bounce_limit},
    {"blk_queue_chunk_sectors", (void *)(uintptr_t)&kb_blk_queue_chunk_sectors},
    {"blk_queue_dma_alignment", (void *)(uintptr_t)&kb_blk_queue_dma_alignment},
    {"blk_queue_flag_set", (void *)(uintptr_t)&kb_blk_queue_flag_set},
    {"blk_queue_io_min", (void *)(uintptr_t)&kb_blk_queue_io_min},
    {"blk_queue_io_opt", (void *)(uintptr_t)&kb_blk_queue_io_opt},
    {"blk_queue_logical_block_size", (void *)(uintptr_t)&kb_blk_queue_logical_block_size},
    {"blk_queue_max_discard_sectors", (void *)(uintptr_t)&kb_blk_queue_max_discard_sectors},
    {"blk_queue_max_discard_segments", (void *)(uintptr_t)&kb_blk_queue_max_discard_segments},
    {"blk_queue_max_hw_sectors", (void *)(uintptr_t)&kb_blk_queue_max_hw_sectors},
    {"blk_queue_max_segments", (void *)(uintptr_t)&kb_blk_queue_max_segments},
    {"blk_queue_max_write_zeroes_sectors", (void *)(uintptr_t)&kb_blk_queue_max_write_zeroes_sectors},
    {"blk_queue_max_zone_append_sectors", (void *)(uintptr_t)&kb_blk_queue_max_zone_append_sectors},
    {"blk_queue_physical_block_size", (void *)(uintptr_t)&kb_blk_queue_physical_block_size},
    {"blk_queue_update_dma_alignment", (void *)(uintptr_t)&kb_blk_queue_update_dma_alignment},
    {"blk_queue_virt_boundary", (void *)(uintptr_t)&kb_blk_queue_virt_boundary},
    {"blk_queue_write_cache", (void *)(uintptr_t)&kb_blk_queue_write_cache},
    {"blk_revalidate_disk_zones", (void *)(uintptr_t)&kb_blk_revalidate_disk_zones},
    {"blk_rq_is_poll", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_map_kern", (void *)(uintptr_t)&kb_blk_rq_map_kern},
    {"blk_rq_map_user_io", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_map_user_iov", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_poll", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_unmap_user", (void *)(uintptr_t)&kb_noop},
    {"blk_set_stacking_limits", (void *)(uintptr_t)&kb_blk_set_stacking_limits},
    {"blk_stack_limits", (void *)(uintptr_t)&kb_blk_stack_limits},
    {"blk_status_to_errno", (void *)(uintptr_t)&kb_blk_status_to_errno},
    {"blk_steal_bios", (void *)(uintptr_t)&kb_noop},
    {"blk_sync_queue", (void *)(uintptr_t)&kb_noop},
    {"blkdev_compat_ptr_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"backlight_device_register", (void *)(uintptr_t)&kb_alloc_stub},
    {"backlight_device_unregister", (void *)(uintptr_t)&kb_noop},
    {"bit_wait", (void *)(uintptr_t)&kb_noop},
    {"bpf_trace_run1", (void *)(uintptr_t)&kb_noop},
    {"bpf_trace_run2", (void *)(uintptr_t)&kb_noop},
    {"bpf_trace_run3", (void *)(uintptr_t)&kb_noop},
    {"cancel_delayed_work_sync", (void *)(uintptr_t)&kb_cancel_delayed_work_sync},
    {"cancel_delayed_work", (void *)(uintptr_t)&kb_cancel_delayed_work},
    {"cancel_work_sync", (void *)(uintptr_t)&kb_cancel_work_sync},
    {"capable", (void *)(uintptr_t)&kb_return_zero},
    {"cdev_device_add", (void *)(uintptr_t)&kb_return_zero},
    {"cdev_device_del", (void *)(uintptr_t)&kb_noop},
    {"cdev_add", (void *)(uintptr_t)&kb_cdev_add_stub},
    {"cdev_del", (void *)(uintptr_t)&kb_cdev_del_stub},
    {"cdev_init", (void *)(uintptr_t)&kb_cdev_init_stub},
    {"class_create", (void *)(uintptr_t)&kb_class_create_stub},
    {"class_destroy", (void *)(uintptr_t)&kb_class_destroy_stub},
    {"cleanup_srcu_struct", (void *)(uintptr_t)&kb_noop},
    {"close_fd", (void *)(uintptr_t)&kb_close_fd},
    {"compat_ptr_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_alloc_kpp", (void *)(uintptr_t)&kb_alloc_stub},
    {"crypto_alloc_shash", (void *)(uintptr_t)&kb_alloc_stub},
    {"crypto_destroy_tfm", (void *)(uintptr_t)&kb_noop},
    {"crypto_req_done", (void *)(uintptr_t)&kb_noop},
    {"crypto_shash_final", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_shash_setkey", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_shash_tfm_digest", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_shash_update", (void *)(uintptr_t)&kb_return_zero},
    {"del_gendisk", (void *)(uintptr_t)&kb_del_gendisk},
    {"delayed_work_timer_fn", (void *)(uintptr_t)&kb_noop},
    {"destroy_workqueue", (void *)(uintptr_t)&kb_noop},
    {"dev_driver_string", (void *)(uintptr_t)&kb_empty_string},
    {"dev_pm_qos_expose_latency_tolerance", (void *)(uintptr_t)&kb_return_zero},
    {"dev_pm_qos_hide_latency_tolerance", (void *)(uintptr_t)&kb_noop},
    {"dev_pm_qos_update_user_latency_tolerance", (void *)(uintptr_t)&kb_return_zero},
    {"dev_set_name", (void *)(uintptr_t)&kb_dev_set_name},
    {"device_add", (void *)(uintptr_t)&kb_device_add},
    {"device_add_disk", (void *)(uintptr_t)&kb_device_add_disk},
    {"device_del", (void *)(uintptr_t)&kb_noop},
    {"device_initialize", (void *)(uintptr_t)&kb_noop},
    {"device_remove_file_self", (void *)(uintptr_t)&kb_return_zero},
    {"disable_irq", (void *)(uintptr_t)&kb_disable_irq_nosync},
    {"disk_set_zoned", (void *)(uintptr_t)&kb_disk_set_zoned},
    {"disk_uevent", (void *)(uintptr_t)&kb_noop},
    {"disk_update_readahead", (void *)(uintptr_t)&kb_disk_update_readahead},
    {"dmi_match", (void *)(uintptr_t)&kb_return_zero},
    {"dmi_get_system_info", (void *)(uintptr_t)&kb_empty_string},
    {"__drm_atomic_helper_crtc_destroy_state", (void *)(uintptr_t)&kb_noop},
    {"__drm_atomic_helper_crtc_duplicate_state", (void *)(uintptr_t)&kb_alloc_stub},
    {"__drm_atomic_helper_plane_destroy_state", (void *)(uintptr_t)&kb_noop},
    {"__drm_atomic_helper_plane_duplicate_state", (void *)(uintptr_t)&kb_alloc_stub},
    {"__drm_atomic_state_free", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_add_affected_connectors", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_add_affected_planes", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_commit", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_get_crtc_state", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_atomic_get_plane_state", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_atomic_helper_check", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_helper_connector_destroy_state", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_helper_connector_duplicate_state", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_atomic_helper_connector_reset", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_helper_crtc_reset", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_helper_disable_plane", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_helper_page_flip", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_helper_plane_reset", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_helper_set_config", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_helper_swap_state", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_helper_update_legacy_modeset_state", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_helper_update_plane", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_set_crtc_for_connector", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_set_crtc_for_plane", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_set_fb_for_plane", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_set_mode_prop_for_crtc", (void *)(uintptr_t)&kb_return_zero},
    {"drm_atomic_state_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_atomic_state_default_clear", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_state_default_release", (void *)(uintptr_t)&kb_noop},
    {"drm_atomic_state_init", (void *)(uintptr_t)&kb_return_zero},
    {"drm_compat_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_attach_encoder", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_attach_vrr_capable_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_cleanup", (void *)(uintptr_t)&kb_noop},
    {"drm_connector_init", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_list_iter_begin", (void *)(uintptr_t)&kb_noop},
    {"drm_connector_list_iter_end", (void *)(uintptr_t)&kb_noop},
    {"drm_connector_list_iter_next", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_register", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_set_vrr_capable_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_connector_unregister", (void *)(uintptr_t)&kb_noop},
    {"drm_connector_update_edid_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_crtc_cleanup", (void *)(uintptr_t)&kb_noop},
    {"drm_crtc_init_with_planes", (void *)(uintptr_t)&kb_return_zero},
    {"drm_crtc_send_vblank_event", (void *)(uintptr_t)&kb_noop},
    {"drm_dev_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_dev_put", (void *)(uintptr_t)&kb_noop},
    {"drm_dev_register", (void *)(uintptr_t)&kb_return_zero},
    {"drm_dev_unregister", (void *)(uintptr_t)&kb_noop},
    {"drm_edid_override_connector_update", (void *)(uintptr_t)&kb_return_zero},
    {"drm_encoder_cleanup", (void *)(uintptr_t)&kb_noop},
    {"drm_encoder_init", (void *)(uintptr_t)&kb_return_zero},
    {"drm_file_get_master", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_format_info", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_framebuffer_cleanup", (void *)(uintptr_t)&kb_noop},
    {"drm_framebuffer_init", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_object_free", (void *)(uintptr_t)&kb_noop},
    {"drm_gem_create_mmap_offset", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_handle_create", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_mmap_obj", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_object_lookup", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_gem_object_release", (void *)(uintptr_t)&kb_noop},
    {"drm_gem_prime_export", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_gem_prime_fd_to_handle", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_prime_handle_to_fd", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_prime_import", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_gem_private_object_init", (void *)(uintptr_t)&kb_return_zero},
    {"drm_gem_vm_close", (void *)(uintptr_t)&kb_noop},
    {"drm_gem_vm_open", (void *)(uintptr_t)&kb_noop},
    {"drm_helper_hpd_irq_event", (void *)(uintptr_t)&kb_noop},
    {"drm_helper_mode_fill_fb_struct", (void *)(uintptr_t)&kb_noop},
    {"drm_helper_probe_single_connector_modes", (void *)(uintptr_t)&kb_return_zero},
    {"drm_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"drm_kms_helper_hotplug_event", (void *)(uintptr_t)&kb_noop},
    {"drm_kms_helper_poll_disable", (void *)(uintptr_t)&kb_noop},
    {"drm_kms_helper_poll_fini", (void *)(uintptr_t)&kb_noop},
    {"drm_kms_helper_poll_init", (void *)(uintptr_t)&kb_noop},
    {"drm_master_put", (void *)(uintptr_t)&kb_noop},
    {"drm_mode_config_cleanup", (void *)(uintptr_t)&kb_noop},
    {"drm_mode_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_mode_create_dvi_i_properties", (void *)(uintptr_t)&kb_return_zero},
    {"drm_mode_object_find", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_mode_object_put", (void *)(uintptr_t)&kb_noop},
    {"drm_mode_probed_add", (void *)(uintptr_t)&kb_noop},
    {"drm_mode_set_name", (void *)(uintptr_t)&kb_noop},
    {"drm_mode_vrefresh", (void *)(uintptr_t)&kb_return_zero},
    {"drm_modeset_acquire_fini", (void *)(uintptr_t)&kb_noop},
    {"drm_modeset_acquire_init", (void *)(uintptr_t)&kb_noop},
    {"drm_modeset_backoff", (void *)(uintptr_t)&kb_return_zero},
    {"drm_modeset_drop_locks", (void *)(uintptr_t)&kb_noop},
    {"drm_modeset_lock_all", (void *)(uintptr_t)&kb_noop},
    {"drm_modeset_lock_all_ctx", (void *)(uintptr_t)&kb_return_zero},
    {"drm_modeset_unlock_all", (void *)(uintptr_t)&kb_noop},
    {"drm_object_attach_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_object_property_set_value", (void *)(uintptr_t)&kb_return_zero},
    {"drm_open", (void *)(uintptr_t)&kb_return_zero},
    {"drm_plane_cleanup", (void *)(uintptr_t)&kb_noop},
    {"drm_plane_create_alpha_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_plane_create_blend_mode_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_plane_create_rotation_property", (void *)(uintptr_t)&kb_return_zero},
    {"drm_poll", (void *)(uintptr_t)&kb_return_zero},
    {"drm_prime_gem_destroy", (void *)(uintptr_t)&kb_noop},
    {"drm_prime_pages_to_sg", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_property_blob_get", (void *)(uintptr_t)&kb_identity_ptr},
    {"drm_property_blob_put", (void *)(uintptr_t)&kb_noop},
    {"drm_property_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_property_create_enum", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_property_lookup_blob", (void *)(uintptr_t)&kb_alloc_stub},
    {"drm_property_replace_blob", (void *)(uintptr_t)&kb_return_zero},
    {"drm_read", (void *)(uintptr_t)&kb_return_zero},
    {"drm_release", (void *)(uintptr_t)&kb_return_zero},
    {"drm_universal_plane_init", (void *)(uintptr_t)&kb_return_zero},
    {"drm_vma_node_is_allowed", (void *)(uintptr_t)&kb_return_one},
    {"drm_vma_offset_lookup_locked", (void *)(uintptr_t)&kb_alloc_stub},
    {"drmm_mode_config_init", (void *)(uintptr_t)&kb_return_zero},
    {"ext_pi_type1_crc64", (void *)(uintptr_t)&kb_return_zero},
    {"ext_pi_type3_crc64", (void *)(uintptr_t)&kb_return_zero},
    {"finish_wait", (void *)(uintptr_t)&kb_noop},
    {"fd_install", (void *)(uintptr_t)&kb_fd_install},
    {"fget", (void *)(uintptr_t)&kb_fget},
    {"filp_close", (void *)(uintptr_t)&kb_filp_close},
    {"filp_open", (void *)(uintptr_t)&kb_filp_open},
    {"find_vma_intersection", (void *)(uintptr_t)&kb_find_vma_intersection},
    {"find_vma", (void *)(uintptr_t)&kb_find_vma},
    {"follow_pfn", (void *)(uintptr_t)&kb_follow_pfn},
    {"fput", (void *)(uintptr_t)&kb_fput},
    {"free_opal_dev", (void *)(uintptr_t)&kb_noop},
    {"free_pages", (void *)(uintptr_t)&kb_noop},
    {"full_name_hash", (void *)(uintptr_t)&kb_return_zero},
    {"get_device", (void *)(uintptr_t)&kb_identity_ptr},
    {"get_unused_fd_flags", (void *)(uintptr_t)&kb_get_unused_fd_flags},
    {"get_user_pages_remote", (void *)(uintptr_t)&kb_get_user_pages_remote},
    {"hwmon_device_register_with_info", (void *)(uintptr_t)&kb_hwmon_device_register_with_info},
    {"hwmon_device_unregister", (void *)(uintptr_t)&kb_noop},
    {"i2c_add_adapter", (void *)(uintptr_t)&kb_return_zero},
    {"i2c_del_adapter", (void *)(uintptr_t)&kb_noop},
    {"ida_alloc_range", (void *)(uintptr_t)&kb_ida_alloc_range},
    {"ida_destroy", (void *)(uintptr_t)&kb_ida_destroy},
    {"ida_free", (void *)(uintptr_t)&kb_ida_free},
    {"idr_get_next", (void *)(uintptr_t)&kb_return_zero},
    {"init_opal_dev", (void *)(uintptr_t)&kb_return_zero},
    {"init_srcu_struct", (void *)(uintptr_t)&kb_return_zero},
    {"init_timer_key", (void *)(uintptr_t)&kb_init_timer_key},
    {"init_wait_entry", (void *)(uintptr_t)&kb_noop},
    {"io_uring_cmd_done", (void *)(uintptr_t)&kb_noop},
    {"io_uring_cmd_import_fixed", (void *)(uintptr_t)&kb_return_zero},
    {"is_acpi_device_node", (void *)(uintptr_t)&kb_return_zero},
    {"ioremap", (void *)(uintptr_t)&kb_ioremap},
    {"ioremap_cache", (void *)(uintptr_t)&kb_ioremap},
    {"ioremap_wc", (void *)(uintptr_t)&kb_ioremap},
    {"iounmap", (void *)(uintptr_t)&kb_iounmap},
    {"iov_iter_kvec", (void *)(uintptr_t)&kb_noop},
    {"is_vmalloc_addr", (void *)(uintptr_t)&kb_return_zero},
    {"iterate_fd", (void *)(uintptr_t)&kb_iterate_fd},
    {"jiffies_to_msecs", (void *)(uintptr_t)&kb_jiffies_to_msecs},
    {"jiffies_to_timespec64", (void *)(uintptr_t)&kb_jiffies_to_timespec64},
    {"jiffies_to_usecs", (void *)(uintptr_t)&kb_jiffies_to_usecs},
    {"kasprintf", (void *)(uintptr_t)&kb_kasprintf_shim},
    {"kblockd_schedule_work", (void *)(uintptr_t)&kb_return_zero},
    {"kernel_read", (void *)(uintptr_t)&kb_return_zero},
    {"kernel_write", (void *)(uintptr_t)&kb_return_zero},
    {"kill_fasync", (void *)(uintptr_t)&kb_noop},
    {"kfree_const", (void *)(uintptr_t)&kb_kfree},
    {"kthread_create_on_node", (void *)(uintptr_t)&kb_alloc_stub},
    {"kthread_should_stop", (void *)(uintptr_t)&kb_return_one},
    {"kthread_stop", (void *)(uintptr_t)&kb_return_zero},
    {"kmem_cache_create", (void *)(uintptr_t)&kb_kmem_cache_create},
    {"kmem_cache_destroy", (void *)(uintptr_t)&kb_kmem_cache_destroy},
    {"kmem_cache_free", (void *)(uintptr_t)&kb_kmem_cache_free},
    {"kmalloc_large", (void *)(uintptr_t)&kb_kmalloc_alias},
    {"krealloc", (void *)(uintptr_t)&kb_krealloc_shim},
    {"ksize", (void *)(uintptr_t)&kb_ksize_shim},
    {"kobject_uevent_env", (void *)(uintptr_t)&kb_return_zero},
    {"ktime_get_with_offset", (void *)(uintptr_t)&kb_ktime_get_with_offset},
    {"ktime_get_raw_ts64", (void *)(uintptr_t)&kb_ktime_get_raw_ts64},
    {"ktime_get_real_ts64", (void *)(uintptr_t)&kb_ktime_get_real_ts64},
    {"kvasprintf", (void *)(uintptr_t)&kb_kvasprintf_shim},
    {"kvfree", (void *)(uintptr_t)&kb_kfree},
    {"kvmalloc_node", (void *)(uintptr_t)&kb_kzalloc},
    {"memchr_inv", (void *)(uintptr_t)&kb_return_zero},
    {"memset_io", (void *)(uintptr_t)&kb_memset_io_shim},
    {"mempool_alloc_slab", (void *)(uintptr_t)&kb_alloc_stub},
    {"mempool_free_slab", (void *)(uintptr_t)&kb_noop},
    {"memremap_compat_align", (void *)(uintptr_t)&kb_return_zero},
    {"migrate_vma_finalize", (void *)(uintptr_t)&kb_noop},
    {"migrate_vma_pages", (void *)(uintptr_t)&kb_return_zero},
    {"migrate_vma_setup", (void *)(uintptr_t)&kb_return_zero},
    {"mod_timer", (void *)(uintptr_t)&kb_mod_timer},
    {"module_put", (void *)(uintptr_t)&kb_noop},
    {"msleep", (void *)(uintptr_t)&kb_msleep},
    {"numa_node", (void *)(uintptr_t)&kb_return_zero},
    {"ndelay", (void *)(uintptr_t)&kb_ndelay},
    {"noop_llseek", (void *)(uintptr_t)&kb_return_zero},
    {"opal_unlock_from_suspend", (void *)(uintptr_t)&kb_return_zero},
    {"on_each_cpu_cond_mask", (void *)(uintptr_t)&kb_noop},
    {"out_of_line_wait_on_bit_lock", (void *)(uintptr_t)&kb_return_zero},
    {"param_get_uint", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_uint", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_uint_minmax", (void *)(uintptr_t)&kb_return_zero},
    {"panic", (void *)(uintptr_t)&kb_stack_chk_fail},
    {"pcibios_resource_to_bus", (void *)(uintptr_t)&kb_noop},
    {"pci_alloc_p2pmem", (void *)(uintptr_t)&kb_return_zero},
    {"pci_clear_master", (void *)(uintptr_t)&kb_noop},
    {"pci_dev_present", (void *)(uintptr_t)&kb_return_one},
    {"pci_dev_put", (void *)(uintptr_t)&kb_pci_dev_put},
    {"pci_disable_msi", (void *)(uintptr_t)&kb_pci_disable_msi},
    {"pci_disable_msix", (void *)(uintptr_t)&kb_pci_disable_msix},
    {"pci_enable_atomic_ops_to_root", (void *)(uintptr_t)&kb_return_zero},
    {"pci_enable_msi", (void *)(uintptr_t)&kb_pci_enable_msi},
    {"pci_enable_msix_range", (void *)(uintptr_t)&kb_pci_enable_msix_range},
    {"pci_find_capability", (void *)(uintptr_t)&kb_pci_find_capability},
    {"pci_free_p2pmem", (void *)(uintptr_t)&kb_noop},
    {"pci_get_class", (void *)(uintptr_t)&kb_pci_get_class},
    {"pci_get_domain_bus_and_slot", (void *)(uintptr_t)&kb_return_zero},
    {"pin_user_pages", (void *)(uintptr_t)&kb_pin_user_pages},
    {"pin_user_pages_remote", (void *)(uintptr_t)&kb_pin_user_pages_remote},
    {"pci_load_saved_state", (void *)(uintptr_t)&kb_return_zero},
    {"pci_p2pdma_add_resource", (void *)(uintptr_t)&kb_return_zero},
    {"pci_p2pmem_publish", (void *)(uintptr_t)&kb_return_zero},
    {"pci_p2pmem_virt_to_bus", (void *)(uintptr_t)&kb_return_zero},
    {"pci_read_config_byte", (void *)(uintptr_t)&kb_pci_read_config_byte},
    {"pci_read_config_dword", (void *)(uintptr_t)&kb_pci_read_config_dword},
    {"pci_release_regions", (void *)(uintptr_t)&kb_noop},
    {"pci_request_regions", (void *)(uintptr_t)&kb_return_zero},
    {"pci_sriov_configure_simple", (void *)(uintptr_t)&kb_return_zero},
    {"pci_stop_and_remove_bus_device", (void *)(uintptr_t)&kb_noop},
    {"pci_write_config_byte", (void *)(uintptr_t)&kb_pci_write_config_byte},
    {"pci_write_config_dword", (void *)(uintptr_t)&kb_pci_write_config_dword},
    {"pci_write_config_word", (void *)(uintptr_t)&kb_pci_write_config_word},
    {"pcie_capability_read_word", (void *)(uintptr_t)&kb_return_zero},
    {"perf_trace_buf_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"perf_trace_run_bpf_submit", (void *)(uintptr_t)&kb_noop},
    {"prepare_to_wait", (void *)(uintptr_t)&kb_noop},
    {"prepare_to_wait_event", (void *)(uintptr_t)&kb_return_zero},
    {"proc_create_data", (void *)(uintptr_t)&kb_proc_create_data_stub},
    {"proc_mkdir_mode", (void *)(uintptr_t)&kb_proc_mkdir_mode_stub},
    {"proc_remove", (void *)(uintptr_t)&kb_proc_remove_stub},
    {"proc_symlink", (void *)(uintptr_t)&kb_alloc_stub},
    {"put_device", (void *)(uintptr_t)&kb_noop},
    {"put_disk", (void *)(uintptr_t)&kb_put_disk},
    {"queue_delayed_work_on", (void *)(uintptr_t)&kb_queue_delayed_work_on},
    {"queue_work_on", (void *)(uintptr_t)&kb_queue_work_on},
    {"rb_erase", (void *)(uintptr_t)&kb_noop},
    {"rb_first", (void *)(uintptr_t)&kb_return_zero},
    {"rb_insert_color", (void *)(uintptr_t)&kb_noop},
    {"radix_tree_delete", (void *)(uintptr_t)&kb_return_zero},
    {"radix_tree_gang_lookup", (void *)(uintptr_t)&kb_return_zero},
    {"radix_tree_insert", (void *)(uintptr_t)&kb_return_zero},
    {"radix_tree_lookup", (void *)(uintptr_t)&kb_return_zero},
    {"radix_tree_lookup_slot", (void *)(uintptr_t)&kb_return_zero},
    {"radix_tree_replace_slot", (void *)(uintptr_t)&kb_noop},
    {"refcount_warn_saturate", (void *)(uintptr_t)&kb_noop},
    {"remap_pfn_range", (void *)(uintptr_t)&kb_remap_pfn_range},
    {"register_acpi_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"release_firmware", (void *)(uintptr_t)&kb_noop},
    {"firmware_request_nowarn", (void *)(uintptr_t)&kb_return_zero},
    {"remove_proc_entry", (void *)(uintptr_t)&kb_remove_proc_entry_stub},
    {"request_firmware", (void *)(uintptr_t)&kb_return_zero},
    {"reset_control_reset", (void *)(uintptr_t)&kb_return_zero},
    {"renesas_xhci_check_request_fw", (void *)(uintptr_t)&kb_return_zero},
    {"schedule", (void *)(uintptr_t)&kb_run_deferred_work},
    {"schedule_timeout", (void *)(uintptr_t)&kb_schedule_timeout},
    {"sed_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"set_capacity", (void *)(uintptr_t)&kb_set_capacity},
    {"set_capacity_and_notify", (void *)(uintptr_t)&kb_set_capacity_and_notify},
    {"set_disk_ro", (void *)(uintptr_t)&kb_set_disk_ro},
    {"set_memory_uc", (void *)(uintptr_t)&kb_return_zero},
    {"set_memory_wb", (void *)(uintptr_t)&kb_return_zero},
    {"set_normalized_timespec64", (void *)(uintptr_t)&kb_noop},
    {"set_page_dirty", (void *)(uintptr_t)&kb_return_zero},
    {"set_page_dirty_lock", (void *)(uintptr_t)&kb_return_zero},
    {"set_pages_array_uc", (void *)(uintptr_t)&kb_return_zero},
    {"set_pages_array_wb", (void *)(uintptr_t)&kb_return_zero},
    {"seq_lseek", (void *)(uintptr_t)&kb_return_zero},
    {"seq_printf", (void *)(uintptr_t)&kb_return_zero},
    {"seq_putc", (void *)(uintptr_t)&kb_return_zero},
    {"seq_puts", (void *)(uintptr_t)&kb_return_zero},
    {"seq_read", (void *)(uintptr_t)&kb_return_zero},
    {"seq_read_iter", (void *)(uintptr_t)&kb_return_zero},
    {"scsi_add_host_with_dma", (void *)(uintptr_t)&kb_scsi_add_host_with_dma},
    {"scsi_done", (void *)(uintptr_t)&kb_scsi_done},
    {"scsi_done_direct", (void *)(uintptr_t)&kb_scsi_done},
    {"scsi_eh_prep_cmnd", (void *)(uintptr_t)&kb_scsi_eh_prep_cmnd},
    {"scsi_eh_restore_cmnd", (void *)(uintptr_t)&kb_scsi_eh_restore_cmnd},
    {"scsi_host_alloc", (void *)(uintptr_t)&kb_scsi_host_alloc},
    {"scsi_host_put", (void *)(uintptr_t)&kb_scsi_host_put},
    {"scsi_is_host_device", (void *)(uintptr_t)&kb_scsi_is_host_device},
    {"scsi_normalize_sense", (void *)(uintptr_t)&kb_scsi_normalize_sense},
    {"scsi_remove_host", (void *)(uintptr_t)&kb_scsi_remove_host},
    {"scsi_report_bus_reset", (void *)(uintptr_t)&kb_scsi_report_bus_reset},
    {"scsi_report_device_reset", (void *)(uintptr_t)&kb_scsi_report_device_reset},
    {"scsi_scan_host", (void *)(uintptr_t)&kb_scsi_scan_host},
    {"scsi_sense_desc_find", (void *)(uintptr_t)&kb_scsi_sense_desc_find},
    {"single_open", (void *)(uintptr_t)&kb_return_zero},
    {"single_release", (void *)(uintptr_t)&kb_return_zero},
    {"simple_strtoul", (void *)(uintptr_t)&strtoul},
    {"sort", (void *)(uintptr_t)&kb_kernel_sort_shim},
    {"strscpy", (void *)(uintptr_t)&kb_strscpy_shim},
    {"submit_bio_noacct", (void *)(uintptr_t)&kb_noop},
    {"synchronize_rcu", (void *)(uintptr_t)&kb_noop},
    {"synchronize_irq", (void *)(uintptr_t)&kb_noop},
    {"synchronize_srcu", (void *)(uintptr_t)&kb_noop},
    {"sysfs_create_link", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_remove_link", (void *)(uintptr_t)&kb_noop},
    {"t10_pi_type1_crc", (void *)(uintptr_t)&kb_return_zero},
    {"t10_pi_type3_crc", (void *)(uintptr_t)&kb_return_zero},
    {"timer_delete_sync", (void *)(uintptr_t)&kb_timer_delete},
    {"trace_event_buffer_commit", (void *)(uintptr_t)&kb_noop},
    {"trace_event_buffer_reserve", (void *)(uintptr_t)&kb_alloc_stub},
    {"trace_event_printf", (void *)(uintptr_t)&kb_noop},
    {"trace_event_raw_init", (void *)(uintptr_t)&kb_return_zero},
    {"trace_event_reg", (void *)(uintptr_t)&kb_return_zero},
    {"trace_handle_return", (void *)(uintptr_t)&kb_return_zero},
    {"trace_print_symbols_seq", (void *)(uintptr_t)&kb_return_zero},
    {"trace_raw_output_prep", (void *)(uintptr_t)&kb_return_zero},
    {"trace_seq_printf", (void *)(uintptr_t)&kb_return_zero},
    {"trace_seq_putc", (void *)(uintptr_t)&kb_noop},
    {"try_module_get", (void *)(uintptr_t)&kb_return_one},
    {"up", (void *)(uintptr_t)&kb_noop},
    {"up_read", (void *)(uintptr_t)&kb_up_read},
    {"unregister_chrdev_region", (void *)(uintptr_t)&kb_unregister_chrdev_region_stub},
    {"unregister_acpi_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"unmap_mapping_range", (void *)(uintptr_t)&kb_noop},
    {"unpin_user_page", (void *)(uintptr_t)&kb_unpin_user_page},
    {"unlock_page", (void *)(uintptr_t)&kb_noop},
    {"usleep_range_state", (void *)(uintptr_t)&kb_usleep_range_state},
    {"vmap", (void *)(uintptr_t)&kb_alloc_stub},
    {"vfree", (void *)(uintptr_t)&kb_kfree},
    {"vm_get_page_prot", (void *)(uintptr_t)&kb_return_zero},
    {"vmalloc", (void *)(uintptr_t)&kb_kzalloc},
    {"vmalloc_to_page", (void *)(uintptr_t)&kb_return_zero},
    {"vm_insert_page", (void *)(uintptr_t)&kb_return_zero},
    {"vmf_insert_pfn", (void *)(uintptr_t)&kb_vmf_insert_pfn},
    {"vmf_insert_pfn_prot", (void *)(uintptr_t)&kb_vmf_insert_pfn},
    {"vunmap", (void *)(uintptr_t)&kb_kfree},
    {"vzalloc", (void *)(uintptr_t)&kb_kzalloc},
    {"vga_set_legacy_decoding", (void *)(uintptr_t)&kb_noop},
    {"pm_runtime_allow", (void *)(uintptr_t)&kb_noop},
    {"pm_runtime_forbid", (void *)(uintptr_t)&kb_noop},
    {"usb_acpi_port_lpm_incapable", (void *)(uintptr_t)&kb_return_zero},
    {"usb_amd_quirk_pll_check", (void *)(uintptr_t)&kb_return_zero},
    {"usb_enable_intel_xhci_ports", (void *)(uintptr_t)&kb_return_zero},
    {"__devm_request_region", (void *)(uintptr_t)&kb_alloc_stub},
    {"__get_user_1", (void *)(uintptr_t)&kb_return_zero},
    {"__get_user_4", (void *)(uintptr_t)&kb_return_zero},
    {"__kfifo_alloc", (void *)(uintptr_t)&kb_return_zero},
    {"__kfifo_free", (void *)(uintptr_t)&kb_noop},
    {"__kfifo_in", (void *)(uintptr_t)&kb_return_zero},
    {"__kfifo_to_user", (void *)(uintptr_t)&kb_return_zero},
    {"__list_add_valid_or_report", (void *)(uintptr_t)&kb_list_add_valid_or_report},
    {"__list_del_entry_valid_or_report", (void *)(uintptr_t)&kb_list_del_entry_valid_or_report},
    {"__pm_runtime_set_status", (void *)(uintptr_t)&kb_return_zero},
    {"__pm_runtime_suspend", (void *)(uintptr_t)&kb_return_zero},
    {"__pm_runtime_use_autosuspend", (void *)(uintptr_t)&kb_noop},
    {"__put_cred", (void *)(uintptr_t)&kb_noop},
    {"__suspend_report_result", (void *)(uintptr_t)&kb_noop},
    {"__tasklet_hi_schedule", (void *)(uintptr_t)&kb_tasklet_schedule},
    {"__tasklet_schedule", (void *)(uintptr_t)&kb_tasklet_schedule},
    {"_dev_notice", (void *)(uintptr_t)&kb_dev_warn},
    {"_dev_printk", (void *)(uintptr_t)&kb_dev_printk},
    {"acpi_bus_power_manageable", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_bus_set_power", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_check_dsm", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_fetch_acpi_dev", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_find_child_by_adr", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_find_child_device", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_get_physical_device_location", (void *)(uintptr_t)&kb_return_zero},
    {"add_device_randomness", (void *)(uintptr_t)&kb_noop},
    {"add_timer", (void *)(uintptr_t)&kb_add_timer},
    {"add_wait_queue", (void *)(uintptr_t)&kb_noop},
    {"blocking_notifier_call_chain", (void *)(uintptr_t)&kb_return_zero},
    {"blocking_notifier_chain_register", (void *)(uintptr_t)&kb_return_zero},
    {"blocking_notifier_chain_unregister", (void *)(uintptr_t)&kb_return_zero},
    {"bus_find_device", (void *)(uintptr_t)&kb_return_zero},
    {"bus_for_each_dev", (void *)(uintptr_t)&kb_bus_for_each_dev},
    {"bus_for_each_drv", (void *)(uintptr_t)&kb_bus_for_each_drv},
    {"bus_register", (void *)(uintptr_t)&kb_return_zero},
    {"bus_register_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"bus_rescan_devices", (void *)(uintptr_t)&kb_return_zero},
    {"bus_unregister", (void *)(uintptr_t)&kb_noop},
    {"bus_unregister_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"class_register", (void *)(uintptr_t)&kb_return_zero},
    {"class_unregister", (void *)(uintptr_t)&kb_noop},
    {"component_add", (void *)(uintptr_t)&kb_return_zero},
    {"component_del", (void *)(uintptr_t)&kb_noop},
    {"current_time", (void *)(uintptr_t)&kb_return_zero},
    {"debugfs_create_dir", (void *)(uintptr_t)&kb_alloc_stub},
    {"debugfs_create_file", (void *)(uintptr_t)&kb_alloc_stub},
    {"debugfs_create_regset32", (void *)(uintptr_t)&kb_alloc_stub},
    {"debugfs_lookup_and_remove", (void *)(uintptr_t)&kb_return_zero},
    {"debugfs_remove", (void *)(uintptr_t)&kb_noop},
    {"autoremove_wake_function", (void *)(uintptr_t)&kb_return_zero},
    {"default_wake_function", (void *)(uintptr_t)&kb_return_zero},
    {"dev_pm_qos_add_request", (void *)(uintptr_t)&kb_return_zero},
    {"dev_pm_qos_expose_flags", (void *)(uintptr_t)&kb_return_zero},
    {"dev_pm_qos_flags", (void *)(uintptr_t)&kb_return_zero},
    {"dev_pm_qos_remove_request", (void *)(uintptr_t)&kb_return_zero},
    {"dev_get_drvdata", (void *)(uintptr_t)&kb_dev_get_drvdata},
    {"device_attach", (void *)(uintptr_t)&kb_return_zero},
    {"device_bind_driver", (void *)(uintptr_t)&kb_return_zero},
    {"device_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"device_create_bin_file", (void *)(uintptr_t)&kb_return_zero},
    {"device_create_file", (void *)(uintptr_t)&kb_return_zero},
    {"device_create_managed_software_node", (void *)(uintptr_t)&kb_return_zero},
    {"device_destroy", (void *)(uintptr_t)&kb_noop},
    {"device_match_devt", (void *)(uintptr_t)&kb_return_zero},
    {"device_pm_wait_for_dev", (void *)(uintptr_t)&kb_return_zero},
    {"device_register", (void *)(uintptr_t)&kb_device_register},
    {"device_release_driver", (void *)(uintptr_t)&kb_noop},
    {"device_remove_bin_file", (void *)(uintptr_t)&kb_noop},
    {"device_remove_file", (void *)(uintptr_t)&kb_noop},
    {"device_reprobe", (void *)(uintptr_t)&kb_return_zero},
    {"device_set_of_node_from_dev", (void *)(uintptr_t)&kb_noop},
    {"device_set_wakeup_capable", (void *)(uintptr_t)&kb_noop},
    {"device_set_wakeup_enable", (void *)(uintptr_t)&kb_return_zero},
    {"device_unregister", (void *)(uintptr_t)&kb_noop},
    {"device_wakeup_disable", (void *)(uintptr_t)&kb_return_zero},
    {"device_wakeup_enable", (void *)(uintptr_t)&kb_return_zero},
    {"dev_set_drvdata", (void *)(uintptr_t)&kb_dev_set_drvdata},
    {"devm_gen_pool_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"devm_ioremap", (void *)(uintptr_t)&kb_devm_ioremap_shim},
    {"devm_memremap", (void *)(uintptr_t)&kb_alloc_stub},
    {"dma_mmap_attrs", (void *)(uintptr_t)&kb_return_zero},
    {"dmam_alloc_attrs", (void *)(uintptr_t)&kb_dma_alloc_attrs},
    {"driver_attach", (void *)(uintptr_t)&kb_driver_attach},
    {"driver_create_file", (void *)(uintptr_t)&kb_return_zero},
    {"driver_register", (void *)(uintptr_t)&kb_driver_register},
    {"driver_unregister", (void *)(uintptr_t)&kb_driver_unregister},
    {"driver_remove_file", (void *)(uintptr_t)&kb_noop},
    {"flush_delayed_work", (void *)(uintptr_t)&kb_flush_delayed_work},
    {"gen_pool_add_owner", (void *)(uintptr_t)&kb_return_zero},
    {"gen_pool_dma_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"gen_pool_dma_alloc_align", (void *)(uintptr_t)&kb_alloc_stub},
    {"gen_pool_free_owner", (void *)(uintptr_t)&kb_noop},
    {"guid_parse", (void *)(uintptr_t)&kb_return_zero},
    {"idr_alloc", (void *)(uintptr_t)&kb_return_zero},
    {"idr_destroy", (void *)(uintptr_t)&kb_noop},
    {"idr_remove", (void *)(uintptr_t)&kb_noop},
    {"inode_set_ctime_current", (void *)(uintptr_t)&kb_noop},
    {"iommu_get_domain_for_dev", (void *)(uintptr_t)&kb_return_zero},
    {"kernfs_find_and_get_ns", (void *)(uintptr_t)&kb_kernfs_find_and_get_ns},
    {"kernfs_notify", (void *)(uintptr_t)&kb_noop},
    {"kernfs_put", (void *)(uintptr_t)&kb_noop},
    {"kill_pid_usb_asyncio", (void *)(uintptr_t)&kb_noop},
    {"kobject_get_path", (void *)(uintptr_t)&kb_kobject_get_path_shim},
    {"kobject_uevent", (void *)(uintptr_t)&kb_return_zero},
    {"kstrdup", (void *)(uintptr_t)&kb_kstrdup_shim},
    {"kstrtou16", (void *)(uintptr_t)&kb_return_zero},
    {"kstrtou16_from_user", (void *)(uintptr_t)&kb_return_zero},
    {"kstrtou8", (void *)(uintptr_t)&kb_return_zero},
    {"ktime_get", (void *)(uintptr_t)&kb_ktime_get},
    {"ktime_get_mono_fast_ns", (void *)(uintptr_t)&kb_ktime_get_mono_fast_ns},
    {"memchr", (void *)(uintptr_t)&memchr},
    {"memdup_user", (void *)(uintptr_t)&kb_memdup_user_shim},
    {"mod_delayed_work_on", (void *)(uintptr_t)&kb_mod_delayed_work_on},
    {"no_seek_end_llseek", (void *)(uintptr_t)&kb_return_zero},
    {"param_get_string", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_copystring", (void *)(uintptr_t)&kb_return_zero},
    {"pci_dev_run_wake", (void *)(uintptr_t)&kb_return_zero},
    {"pci_get_device", (void *)(uintptr_t)&kb_return_zero},
    {"pci_prepare_to_sleep", (void *)(uintptr_t)&kb_return_zero},
    {"phy_calibrate", (void *)(uintptr_t)&kb_return_zero},
    {"phy_exit", (void *)(uintptr_t)&kb_return_zero},
    {"phy_init", (void *)(uintptr_t)&kb_return_zero},
    {"phy_power_off", (void *)(uintptr_t)&kb_return_zero},
    {"phy_power_on", (void *)(uintptr_t)&kb_return_zero},
    {"phy_set_mode_ext", (void *)(uintptr_t)&kb_return_zero},
    {"platform_device_add", (void *)(uintptr_t)&kb_return_zero},
    {"platform_device_add_resources", (void *)(uintptr_t)&kb_return_zero},
    {"platform_device_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"platform_device_put", (void *)(uintptr_t)&kb_noop},
    {"platform_device_unregister", (void *)(uintptr_t)&kb_noop},
    {"pm_runtime_barrier", (void *)(uintptr_t)&kb_return_zero},
    {"pm_runtime_no_callbacks", (void *)(uintptr_t)&kb_noop},
    {"pm_runtime_set_autosuspend_delay", (void *)(uintptr_t)&kb_noop},
    {"pm_wakeup_dev_event", (void *)(uintptr_t)&kb_noop},
    {"print_hex_dump", (void *)(uintptr_t)&kb_noop},
    {"put_pid", (void *)(uintptr_t)&kb_noop},
    {"radix_tree_maybe_preload", (void *)(uintptr_t)&kb_return_zero},
    {"register_acpi_bus_type", (void *)(uintptr_t)&kb_return_zero},
    {"register_chrdev_region", (void *)(uintptr_t)&kb_return_zero},
    {"remove_wait_queue", (void *)(uintptr_t)&kb_noop},
    {"schedule_timeout_uninterruptible", (void *)(uintptr_t)&kb_schedule_timeout},
    {"scnprintf", (void *)(uintptr_t)&kb_scnprintf_shim},
    {"vscnprintf", (void *)(uintptr_t)&kb_vscnprintf_shim},
    {"set_primary_fwnode", (void *)(uintptr_t)&kb_noop},
    {"sg_pcopy_from_buffer", (void *)(uintptr_t)&kb_return_zero},
    {"sg_pcopy_to_buffer", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_add_file_to_group", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_break_active_protection", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_create_group", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_get_dirent", (void *)(uintptr_t)&kb_sysfs_get_dirent},
    {"sysfs_merge_group", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_notify", (void *)(uintptr_t)&kb_noop},
    {"sysfs_remove_file_from_group", (void *)(uintptr_t)&kb_noop},
    {"sysfs_remove_group", (void *)(uintptr_t)&kb_noop},
    {"sysfs_unbreak_active_protection", (void *)(uintptr_t)&kb_noop},
    {"sysfs_unmerge_group", (void *)(uintptr_t)&kb_noop},
    {"tasklet_setup", (void *)(uintptr_t)&kb_tasklet_setup},
    {"timer_delete", (void *)(uintptr_t)&kb_timer_delete},
    {"trace_seq_acquire", (void *)(uintptr_t)&kb_return_zero},
    {"unregister_acpi_bus_type", (void *)(uintptr_t)&kb_return_zero},
    {"utf16s_to_utf8s", (void *)(uintptr_t)&kb_utf16s_to_utf8s_shim},
    {"usb_acpi_power_manageable", (void *)(uintptr_t)&kb_return_zero},
    {"usb_acpi_set_power_state", (void *)(uintptr_t)&kb_return_zero},
    {"usb_amd_dev_put", (void *)(uintptr_t)&kb_noop},
    {"usb_amd_pt_check_port", (void *)(uintptr_t)&kb_return_zero},
    {"usb_amd_quirk_pll_disable", (void *)(uintptr_t)&kb_noop},
    {"usb_amd_quirk_pll_enable", (void *)(uintptr_t)&kb_noop},
    {"usb_asmedia_modifyflowcontrol", (void *)(uintptr_t)&kb_noop},
    {"usb_add_hcd", (void *)(uintptr_t)&kb_usb_add_hcd},
    {"usb_create_shared_hcd", (void *)(uintptr_t)&kb_usb_create_shared_hcd},
    {"usb_decode_interval", (void *)(uintptr_t)&kb_usb_decode_interval},
    {"usb_deregister", (void *)(uintptr_t)&kb_usb_deregister},
    {"usb_deregister_dev", (void *)(uintptr_t)&kb_usb_deregister_dev},
    {"usb_disable_xhci_ports", (void *)(uintptr_t)&kb_return_zero},
    {"usb_disabled", (void *)(uintptr_t)&kb_return_zero},
    {"usb_ep_type_string", (void *)(uintptr_t)&kb_usb_ep_type_string},
    {"usb_find_common_endpoints", (void *)(uintptr_t)&kb_usb_find_common_endpoints},
    {"usb_find_interface", (void *)(uintptr_t)&kb_usb_find_interface},
    {"usb_hc_died", (void *)(uintptr_t)&kb_usb_hc_died},
    {"usb_hcd_amd_remote_wakeup_quirk", (void *)(uintptr_t)&kb_usb_hcd_amd_remote_wakeup_quirk},
    {"usb_hcd_check_unlink_urb", (void *)(uintptr_t)&kb_usb_hcd_check_unlink_urb},
    {"usb_hcd_end_port_resume", (void *)(uintptr_t)&kb_usb_hcd_end_port_resume},
    {"usb_hcd_giveback_urb", (void *)(uintptr_t)&kb_usb_hcd_giveback_urb},
    {"usb_hcd_link_urb_to_ep", (void *)(uintptr_t)&kb_usb_hcd_link_urb_to_ep},
    {"usb_hcd_map_urb_for_dma", (void *)(uintptr_t)&kb_usb_hcd_map_urb_for_dma},
    {"usb_hcd_pci_probe", (void *)(uintptr_t)&kb_usb_hcd_pci_probe},
    {"usb_hcd_pci_remove", (void *)(uintptr_t)&kb_usb_hcd_pci_remove},
    {"usb_hcd_pci_shutdown", (void *)(uintptr_t)&kb_usb_hcd_pci_shutdown},
    {"usb_hcd_poll_rh_status", (void *)(uintptr_t)&kb_usb_hcd_poll_rh_status},
    {"usb_hcd_resume_root_hub", (void *)(uintptr_t)&kb_usb_hcd_resume_root_hub},
    {"usb_hcd_start_port_resume", (void *)(uintptr_t)&kb_usb_hcd_start_port_resume},
    {"usb_hcd_unlink_urb_from_ep", (void *)(uintptr_t)&kb_usb_hcd_unlink_urb_from_ep},
    {"usb_hcd_unmap_urb_for_dma", (void *)(uintptr_t)&kb_usb_hcd_unmap_urb_for_dma},
    {"usb_hub_clear_tt_buffer", (void *)(uintptr_t)&kb_usb_hub_clear_tt_buffer},
    {"usb_kill_urb", (void *)(uintptr_t)&kb_usb_kill_urb},
    {"usb_put_hcd", (void *)(uintptr_t)&kb_usb_put_hcd},
    {"usb_register_driver", (void *)(uintptr_t)&kb_usb_register_driver},
    {"usb_remove_hcd", (void *)(uintptr_t)&kb_usb_remove_hcd},
    {"usb_root_hub_lost_power", (void *)(uintptr_t)&kb_usb_root_hub_lost_power},
    {"usb_speed_string", (void *)(uintptr_t)&kb_usb_speed_string},
    {"usb_state_string", (void *)(uintptr_t)&kb_usb_state_string},
    {"usb_submit_urb", (void *)(uintptr_t)&kb_usb_submit_urb},
    {"usb_unlink_urb", (void *)(uintptr_t)&kb_usb_unlink_urb},
    {"usb_wakeup_notification", (void *)(uintptr_t)&kb_usb_wakeup_notification},
    {"kobox_usb_control_msg_shim", (void *)(uintptr_t)&kb_usb_control_msg_shim},
    {"wait_for_completion_killable_timeout", (void *)(uintptr_t)&kb_wait_for_completion_io_timeout},
    {"wait_for_completion_timeout", (void *)(uintptr_t)&kb_wait_for_completion_io_timeout},
    {"yield", (void *)(uintptr_t)&kb_noop},
    {"wait_for_random_bytes", (void *)(uintptr_t)&kb_return_zero},
    {"wake_up_bit", (void *)(uintptr_t)&kb_noop},
    {"wake_up_process", (void *)(uintptr_t)&kb_return_one},
    {"ww_mutex_lock", (void *)(uintptr_t)&kb_return_zero},
    {"ww_mutex_unlock", (void *)(uintptr_t)&kb_noop},
    {"xa_destroy", (void *)(uintptr_t)&kb_noop},
    {"xa_erase", (void *)(uintptr_t)&kb_return_zero},
    {"xa_find", (void *)(uintptr_t)&kb_return_zero},
    {"xa_find_after", (void *)(uintptr_t)&kb_return_zero},
    {"xa_load", (void *)(uintptr_t)&kb_return_zero},
    {"xa_store", (void *)(uintptr_t)&kb_return_zero},
};

_Static_assert(
    sizeof(shim_symbols) / sizeof(shim_symbols[0]) <= KB_LOCAL_SHIM_STUB_COUNT,
    "increase KB_LOCAL_SHIM_STUB_COUNT");

static void write_u32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void write_u64le(uint8_t *p, uint64_t value)
{
    write_u32le(p, (uint32_t)(value & 0xffffffffu));
    write_u32le(p + 4, (uint32_t)(value >> 32));
}

static void write_abs_jump_stub(uint8_t *p, void *target, void *caller_gs, void *callee_gs)
{
    memset(p, 0x90, KB_LOCAL_SHIM_STUB_SIZE);
    size_t i = 0;
    p[i++] = 0x49;
    p[i++] = 0xbb;
    write_u64le(p + i, (uint64_t)(uintptr_t)target);
    i += 8;
    p[i++] = 0x49;
    p[i++] = 0xba;
    write_u64le(p + i, (uint64_t)(uintptr_t)caller_gs);
    i += 8;
    p[i++] = 0x48;
    p[i++] = 0xb8;
    write_u64le(p + i, (uint64_t)(uintptr_t)callee_gs);
    i += 8;
    p[i++] = 0xff;
    p[i++] = 0x25;
    write_u32le(p + i, 0);
    i += 4;
    write_u64le(p + i, (uint64_t)(uintptr_t)&kb_shim_external_call_trampoline);
    i += 8;
}

static void *allocate_external_call_stub_for_caller(kb_module_t *module, void *target, void *caller_gs, void *callee_gs)
{
    const size_t shim_symbol_count = sizeof(shim_symbols) / sizeof(shim_symbols[0]);
    if (module == NULL || module->shim_symbol_stubs == NULL || target == NULL || caller_gs == NULL || callee_gs == NULL) {
        return NULL;
    }
    if (shim_symbol_count + module->external_stub_count >= KB_LOCAL_SHIM_STUB_COUNT) {
        return NULL;
    }
    uint8_t *stub = module->shim_symbol_stubs +
        ((shim_symbol_count + module->external_stub_count) * KB_LOCAL_SHIM_STUB_SIZE);
    module->external_stub_count++;
    write_abs_jump_stub(stub, target, caller_gs, callee_gs);
    return stub;
}

static void *allocate_external_call_stub(kb_module_t *module, void *target, void *callee_gs)
{
    return allocate_external_call_stub_for_caller(module, target, module->kernel_gs, callee_gs);
}

static void write_abs_jump_raw_stub(uint8_t *p, void *target)
{
    memset(p, 0x90, KB_LOCAL_SHIM_STUB_SIZE);
    size_t i = 0;
    p[i++] = 0x48;
    p[i++] = 0xb8;
    write_u64le(p + i, (uint64_t)(uintptr_t)target);
    i += 8;
    p[i++] = 0xff;
    p[i++] = 0xe0;
}

static uint64_t page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (uint64_t)value : 4096;
#endif
}

static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

static void *alloc_section_memory(uint64_t size)
{
    if (size == 0) {
        return 0;
    }
    const uint64_t rounded_size = align_up_u64(size, page_size());
#if defined(_WIN32)
    return VirtualAlloc(0, (SIZE_T)rounded_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__) && defined(MAP_32BIT)
    flags |= MAP_32BIT;
    static uintptr_t next_module_hint = UINT64_C(0x40000000);
    const uintptr_t min_exec_hint = UINT64_C(0x40000000);
    const uintptr_t max_exec_hint = UINT64_C(0x78000000);
    const uintptr_t stride = UINT64_C(0x02000000);
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        uintptr_t hint = next_module_hint;
        if (hint < min_exec_hint || hint >= max_exec_hint) {
            hint = min_exec_hint;
        }
        next_module_hint = hint + stride;
        void *memory = mmap(
            (void *)hint,
            (size_t)rounded_size,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            flags,
            -1,
            0);
        if (memory == MAP_FAILED) {
            continue;
        }
        const uintptr_t mapped = (uintptr_t)memory;
        if (mapped >= min_exec_hint && mapped < max_exec_hint) {
            return memory;
        }
        munmap(memory, (size_t)rounded_size);
    }
#endif
    void *memory = mmap(0, (size_t)rounded_size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    return memory == MAP_FAILED ? 0 : memory;
#endif
}

static void free_section_memory(void *memory, uint64_t size)
{
    if (memory == 0 || size == 0) {
        return;
    }
    const uint64_t rounded_size = align_up_u64(size, page_size());
#if defined(_WIN32)
    (void)rounded_size;
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, (size_t)rounded_size);
#endif
}

static int range_fits(size_t size, uint64_t offset, uint64_t length)
{
    if (offset > (uint64_t)size) {
        return 0;
    }
    if (length > (uint64_t)size - offset) {
        return 0;
    }
    return 1;
}

static void *lookup_shim_symbol(const char *name)
{
    for (size_t i = 0; i < sizeof(shim_symbols) / sizeof(shim_symbols[0]); i++) {
        if (strcmp(shim_symbols[i].name, name) == 0) {
            return shim_symbols[i].address;
        }
    }
    return 0;
}

static int symbol_name_pointer_is_valid(const char *name)
{
    return !kb_low_or_err_pointer(name);
}

static void *lookup_exported_symbol(const char *name)
{
    if (!symbol_name_pointer_is_valid(name) || name[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; i < KB_EXPORTED_SYMBOL_MAX; i++) {
        if (symbol_name_pointer_is_valid(exported_symbols[i].name) && kb_safe_strcmp(exported_symbols[i].name, name) == 0) {
            return (void *)(uintptr_t)exported_symbols[i].address;
        }
    }
    return 0;
}

void *kb_module_lookup_exported_symbol(const char *name)
{
    return lookup_exported_symbol(name);
}

static int module_contains_address(const kb_module_t *module, uintptr_t address)
{
    if (module == NULL || address == 0) {
        return 0;
    }
    for (size_t i = 0; i < module->section_count; i++) {
        const loaded_section_t *section = &module->sections[i];
        if (section->base == NULL || section->size == 0) {
            continue;
        }
        const uintptr_t begin = (uintptr_t)section->base;
        const uintptr_t end = begin + (uintptr_t)section->size;
        if (address >= begin && address < end) {
            return 1;
        }
    }
    return 0;
}

static int module_contains_executable_address(const kb_module_t *module, uintptr_t address)
{
    if (module == NULL || address == 0) {
        return 0;
    }
    for (size_t i = 0; i < module->section_count; i++) {
        const loaded_section_t *section = &module->sections[i];
        if (section->base == NULL ||
            section->size == 0 ||
            (section->flags & KB_ELF_SHF_EXECINSTR) == 0)
        {
            continue;
        }
        const uintptr_t begin = (uintptr_t)section->base;
        const uintptr_t end = begin + (uintptr_t)section->size;
        if (address >= begin && address < end) {
            return 1;
        }
    }
    return 0;
}

static kb_module_t *module_for_address(const void *address)
{
    const uintptr_t value = (uintptr_t)address;
    for (kb_module_t *module = kb_loaded_modules; module != NULL; module = module->next_loaded) {
        if (module_contains_address(module, value)) {
            return module;
        }
    }
    return NULL;
}

int kb_module_is_executable_address(const void *address)
{
    const uintptr_t value = (uintptr_t)address;
    for (kb_module_t *module = kb_loaded_modules; module != NULL; module = module->next_loaded) {
        if (module_contains_executable_address(module, value)) {
            return 1;
        }
    }
    return 0;
}

unsigned long kb_module_kernel_gs_for_address(const void *address)
{
#if !defined(_WIN32) && defined(__x86_64__)
    kb_module_t *module = module_for_address(address);
    return module == NULL ? 0 : (unsigned long)(uintptr_t)module->kernel_gs;
#else
    (void)address;
    return 0;
#endif
}

void *kb_module_make_gs_call_stub(const void *target, unsigned long caller_gs)
{
#if !defined(_WIN32) && defined(__x86_64__)
    kb_module_t *callee = module_for_address(target);
    unsigned long callee_gs = kb_module_kernel_gs_for_address(target);
    if (callee == NULL || caller_gs == 0 || callee_gs == 0) {
        return NULL;
    }
    return allocate_external_call_stub_for_caller(
        callee,
        (void *)(uintptr_t)target,
        (void *)(uintptr_t)caller_gs,
        (void *)(uintptr_t)callee_gs);
#else
    (void)target;
    (void)caller_gs;
    return NULL;
#endif
}

void *kb_module_current_external_call_target(void)
{
    return (void *)kb_current_external_call_target;
}

unsigned long kb_module_current_external_call_caller_gs(void)
{
    return (unsigned long)kb_current_external_call_caller_gs;
}

unsigned long kb_module_current_external_call_callee_gs(void)
{
    return (unsigned long)kb_current_external_call_callee_gs;
}

static void register_loaded_module(kb_module_t *module)
{
    if (module == NULL) {
        return;
    }
    module->next_loaded = kb_loaded_modules;
    kb_loaded_modules = module;
}

static void unregister_loaded_module(kb_module_t *module)
{
    kb_module_t **cursor = &kb_loaded_modules;
    while (*cursor != NULL) {
        if (*cursor == module) {
            *cursor = module->next_loaded;
            module->next_loaded = NULL;
            return;
        }
        cursor = &(*cursor)->next_loaded;
    }
}

static void unregister_module_exports(const kb_module_t *module)
{
    for (size_t i = 0; i < KB_EXPORTED_SYMBOL_MAX; i++) {
        if (exported_symbols[i].owner == module) {
            memset(&exported_symbols[i], 0, sizeof(exported_symbols[i]));
        }
    }
}

static void *lookup_module_shim_symbol(kb_module_t *module, const char *name)
{
    if (strcmp(name, "__num_online_cpus") == 0) {
        return module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_NUM_ONLINE_CPUS_OFFSET;
    }
    if (strcmp(name, "pcpu_hot") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_PCPU_HOT_OFFSET;
    }
    if (strcmp(name, "pm_suspend_target_state") == 0) {
        return module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_PM_SUSPEND_TARGET_STATE_OFFSET;
    }
    if (strcmp(name, "usb_hcd_pci_pm_ops") == 0) {
        return module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_HCD_PCI_PM_OPS_OFFSET;
    }
    if (strcmp(name, "__tracepoint_xhci_dbg_init") == 0 ||
        strcmp(name, "__tracepoint_xhci_dbg_quirks") == 0)
    {
        return module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_XHCI_TRACEPOINT_OFFSET;
    }
    for (size_t i = 0; i < sizeof(shim_symbols) / sizeof(shim_symbols[0]); i++) {
        if (strcmp(shim_symbols[i].name, name) == 0) {
            return module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE);
        }
    }
    if (strcmp(name, "_printk") == 0 || strcmp(name, "printk") == 0) {
        return module->shim_printk;
    }
    if (strcmp(name, "kfree") == 0) {
        return module->shim_kfree;
    }
    if (strcmp(name, "kmalloc") == 0) {
        return module->shim_kmalloc;
    }
    if (strcmp(name, "kzalloc") == 0) {
        return module->shim_kzalloc;
    }
    if (strcmp(name, "kmalloc_trace") == 0) {
        return module->shim_kmalloc_trace;
    }
    if (strcmp(name, "request_threaded_irq") == 0) {
        return module->shim_request_threaded_irq;
    }
    if (strcmp(name, "free_irq") == 0) {
        return module->shim_free_irq;
    }
    if (strcmp(name, "dma_alloc_attrs") == 0) {
        return module->shim_dma_alloc_attrs;
    }
    if (strcmp(name, "dma_free_attrs") == 0) {
        return module->shim_dma_free_attrs;
    }
    if (strcmp(name, "__stack_chk_fail") == 0) {
        return module->shim_stack_chk_fail;
    }
    if (strcmp(name, "__pci_register_driver") == 0) {
        return module->shim_pci_register_driver;
    }
    if (strcmp(name, "pci_unregister_driver") == 0) {
        return module->shim_pci_unregister_driver;
    }
    if (strcmp(name, "pci_enable_device") == 0) {
        return module->shim_pci_enable_device;
    }
    if (strcmp(name, "pci_disable_device") == 0) {
        return module->shim_pci_disable_device;
    }
    if (strcmp(name, "pci_set_master") == 0) {
        return module->shim_pci_set_master;
    }
    if (strcmp(name, "pci_iomap") == 0) {
        return module->shim_pci_iomap;
    }
    if (strcmp(name, "pci_iounmap") == 0) {
        return module->shim_pci_iounmap;
    }
    if (strcmp(name, "ioread32") == 0) {
        return module->shim_ioread32;
    }
    if (strcmp(name, "iowrite32") == 0) {
        return module->shim_iowrite32;
    }
    if (strcmp(name, "kmalloc_caches") == 0) {
        return module->shim_kmalloc_caches;
    }
    if (strcmp(name, "random_kmalloc_seed") == 0) {
        return module->shim_random_kmalloc_seed;
    }
    if (strcmp(name, "param_ops_int") == 0) {
        return module->shim_param_ops_int;
    }
    if (strcmp(name, "param_ops_uint") == 0) {
        return module->shim_param_ops_uint;
    }
    if (strcmp(name, "param_ops_bool") == 0) {
        return module->shim_param_ops_bool;
    }
    if (strcmp(name, "param_ops_byte") == 0) {
        return module->shim_param_ops_byte;
    }
    if (strcmp(name, "param_ops_ulong") == 0) {
        return module->shim_param_ops_ulong;
    }
    if (strcmp(name, "param_ops_charp") == 0) {
        return module->shim_param_ops_ulong;
    }
    if (strcmp(name, "param_array_ops") == 0) {
        return module->shim_param_ops_ulong;
    }
    if (strcmp(name, "__cpu_possible_mask") == 0) {
        return module->shim_cpu_possible_mask;
    }
    if (strcmp(name, "__cpu_online_mask") == 0) {
        return module->shim_cpu_online_mask;
    }
    if (strcmp(name, "nr_cpu_ids") == 0) {
        return module->shim_nr_cpu_ids;
    }
    if (strcmp(name, "this_cpu_off") == 0) {
        return module->shim_this_cpu_off;
    }
    if (strcmp(name, "__per_cpu_offset") == 0) {
        return module->shim_this_cpu_off;
    }
    if (strcmp(name, "pernet_ops_rwsem") == 0) {
        return module->shim_pernet_ops_rwsem;
    }
    if (strcmp(name, "panic_notifier_list") == 0) {
        return module->shim_panic_notifier_list;
    }
    if (strcmp(name, "pv_ops") == 0) {
        return module->shim_pv_ops;
    }
    if (strcmp(name, "node_data") == 0) {
        return module->shim_node_data;
    }
    if (strcmp(name, "node_states") == 0) {
        return module->shim_node_states;
    }
    if (strcmp(name, "boot_cpu_data") == 0) {
        return module->shim_boot_cpu_data;
    }
    if (strcmp(name, "__default_kernel_pte_mask") == 0 ||
        strcmp(name, "_ctype") == 0 ||
        strcmp(name, "acpi_gbl_FADT") == 0 ||
        strcmp(name, "devmap_managed_key") == 0 ||
        strcmp(name, "dma_ops") == 0 ||
        strcmp(name, "efi") == 0 ||
        strcmp(name, "hugetlb_optimize_vmemmap_key") == 0 ||
        strcmp(name, "iomem_resource") == 0 ||
        strcmp(name, "ioport_resource") == 0 ||
        strcmp(name, "init_uts_ns") == 0 ||
        strcmp(name, "pci_bus_type") == 0 ||
        strcmp(name, "pci_power_names") == 0 ||
        strcmp(name, "pm_wq") == 0 ||
        strcmp(name, "power_group_name") == 0 ||
        strcmp(name, "screen_info") == 0 ||
        strcmp(name, "sme_me_mask") == 0 ||
        strcmp(name, "system_freezable_wq") == 0 ||
        strcmp(name, "system_power_efficient_wq") == 0 ||
        strcmp(name, "system_wq") == 0 || strcmp(name, "page_offset_base") == 0 ||
        strcmp(name, "phys_base") == 0 || strcmp(name, "vmemmap_base") == 0 ||
        strcmp(name, "param_ops_string") == 0 ||
        strcmp(name, "param_ops_ullong") == 0 ||
        strcmp(name, "usb_debug_root") == 0 ||
        strcmp(name, "uuid_null") == 0 ||
        strcmp(name, "pm_suspend_global_flags") == 0 ||
        strcmp(name, "freezer_active") == 0)
    {
        return module->shim_misc_data;
    }
    if (strcmp(name, "jiffies") == 0) {
        void *storage = module->shim_region + KB_LOCAL_JIFFIES_OFFSET;
        kb_register_jiffies_storage(storage);
        return storage;
    }
    return lookup_shim_symbol(name);
}

static kb_status_t loaded_section_address(const kb_module_t *module, uint16_t section_index, uint64_t value, uint64_t *out_address)
{
    if (section_index >= module->section_count) {
        return KB_ERR_INVALID;
    }
    const loaded_section_t *section = &module->sections[section_index];
    if (section->base == 0 || value > section->size) {
        return KB_ERR_INVALID;
    }
    *out_address = (uint64_t)(uintptr_t)section->base + value;
    return KB_OK;
}

static kb_status_t resolve_symbol(
    const kb_module_t *module,
    uint32_t symbol_table_section_index,
    uint32_t symbol_index,
    uint64_t *out_address)
{
    kb_elf_symbol_t symbol;
    kb_status_t status = kb_elf_symbol(&module->elf, symbol_table_section_index, symbol_index, &symbol);
    if (status != KB_OK) {
        return status;
    }

    if (symbol.section_index == KB_ELF_SHN_UNDEF) {
        void *address = lookup_shim_symbol(symbol.name);
        if (address == 0) {
            return KB_ERR_UNSUPPORTED;
        }
        *out_address = (uint64_t)(uintptr_t)address;
        return KB_OK;
    }

    return loaded_section_address(module, symbol.section_index, symbol.value, out_address);
}

static kb_status_t find_symbol_address(const kb_module_t *module, const char *name, uint64_t *out_address)
{
    const size_t section_count = kb_elf_section_count(&module->elf);
    for (size_t section_index = 0; section_index < section_count; section_index++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(&module->elf, section_index, &section);
        if (status != KB_OK) {
            return status;
        }
        if (section.type != KB_ELF_SHT_SYMTAB && section.type != KB_ELF_SHT_DYNSYM) {
            continue;
        }
        size_t symbol_count = 0;
        status = kb_elf_symbol_count(&module->elf, section_index, &symbol_count);
        if (status != KB_OK) {
            return status;
        }
        for (size_t symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
            kb_elf_symbol_t symbol;
            status = kb_elf_symbol(&module->elf, section_index, symbol_index, &symbol);
            if (status != KB_OK) {
                return status;
            }
            if (strcmp(symbol.name, name) != 0) {
                continue;
            }
            return resolve_symbol(module, (uint32_t)section_index, (uint32_t)symbol_index, out_address);
        }
    }
    return KB_ERR_NOT_FOUND;
}

static kb_status_t register_module_exports(kb_module_t *module)
{
    const size_t section_count = kb_elf_section_count(&module->elf);
    for (size_t section_index = 0; section_index < section_count; section_index++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(&module->elf, section_index, &section);
        if (status != KB_OK) {
            return status;
        }
        if (section.type != KB_ELF_SHT_SYMTAB && section.type != KB_ELF_SHT_DYNSYM) {
            continue;
        }

        size_t symbol_count = 0;
        status = kb_elf_symbol_count(&module->elf, section_index, &symbol_count);
        if (status != KB_OK) {
            return status;
        }
        for (size_t symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
            kb_elf_symbol_t symbol;
            status = kb_elf_symbol(&module->elf, section_index, symbol_index, &symbol);
            if (status != KB_OK) {
                return status;
            }
            if (symbol.binding != KB_ELF_STB_GLOBAL || symbol.section_index == KB_ELF_SHN_UNDEF ||
                !symbol_name_pointer_is_valid(symbol.name) || symbol.name[0] == '\0')
            {
                continue;
            }

            uint64_t address = 0;
            status = loaded_section_address(module, symbol.section_index, symbol.value, &address);
            if (status != KB_OK) {
                continue;
            }

            int already_registered = 0;
            for (size_t i = 0; i < KB_EXPORTED_SYMBOL_MAX; i++) {
                if (symbol_name_pointer_is_valid(exported_symbols[i].name) &&
                    kb_safe_strcmp(exported_symbols[i].name, symbol.name) == 0)
                {
                    already_registered = 1;
                    break;
                }
            }
            if (already_registered) {
                continue;
            }

            size_t slot = KB_EXPORTED_SYMBOL_MAX;
            for (size_t i = 0; i < KB_EXPORTED_SYMBOL_MAX; i++) {
                if (exported_symbols[i].name == 0) {
                    slot = i;
                    break;
                }
            }
            if (slot == KB_EXPORTED_SYMBOL_MAX) {
                return KB_ERR_NOMEM;
            }
            exported_symbols[slot].name = symbol.name;
            exported_symbols[slot].address = address;
            exported_symbols[slot].owner = module;
        }
    }
    return KB_OK;
}

static kb_status_t load_sections(kb_module_t *module)
{
    uint64_t image_size = 0;
    for (size_t i = 0; i < module->section_count; i++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(&module->elf, i, &section);
        if (status != KB_OK) {
            return status;
        }
        if ((section.flags & KB_ELF_SHF_ALLOC) == 0) {
            continue;
        }

        const uint64_t alignment = section.alignment == 0 ? 1 : section.alignment;
        image_size = align_up_u64(image_size, alignment);
        module->sections[i].offset = image_size;
        module->sections[i].size = section.size;
        module->sections[i].alignment = alignment;
        module->sections[i].flags = section.flags;
        image_size += section.size;
    }

    if (image_size == 0 || image_size > SIZE_MAX) {
        return KB_ERR_INVALID;
    }

    image_size = align_up_u64(image_size, 16);
    const uint64_t shim_region_offset = image_size;
    image_size += KB_LOCAL_SHIM_REGION_SIZE;

    module->image_base = alloc_section_memory(image_size);
    if (module->image_base == 0) {
        return KB_ERR_NOMEM;
    }
    module->image_size = image_size;
    memset(module->image_base, 0, (size_t)image_size);
    module->shim_region = (uint8_t *)module->image_base + shim_region_offset;
    module->shim_symbol_stubs = module->shim_region;
    module->shim_printk = module->shim_region;
    module->shim_kfree = module->shim_printk + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_kmalloc = module->shim_kfree + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_kzalloc = module->shim_kmalloc + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_kmalloc_trace = module->shim_kzalloc + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_request_threaded_irq = module->shim_kmalloc_trace + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_free_irq = module->shim_request_threaded_irq + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_dma_alloc_attrs = module->shim_free_irq + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_dma_free_attrs = module->shim_dma_alloc_attrs + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_stack_chk_fail = module->shim_dma_free_attrs + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_register_driver = module->shim_stack_chk_fail + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_unregister_driver = module->shim_pci_register_driver + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_enable_device = module->shim_pci_unregister_driver + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_disable_device = module->shim_pci_enable_device + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_set_master = module->shim_pci_disable_device + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_iomap = module->shim_pci_set_master + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_pci_iounmap = module->shim_pci_iomap + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_ioread32 = module->shim_pci_iounmap + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_iowrite32 = module->shim_ioread32 + KB_LOCAL_SHIM_STUB_SIZE;
    module->shim_kmalloc_caches = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET;
    module->shim_random_kmalloc_seed = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 2048;
    module->shim_param_ops_int = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3072;
    module->shim_param_ops_uint = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3136;
    module->shim_param_ops_bool = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3200;
    module->shim_param_ops_byte = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3264;
    module->shim_param_ops_ulong = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3328;
    module->shim_cpu_possible_mask = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3392;
    module->shim_cpu_online_mask = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3408;
    module->shim_nr_cpu_ids = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3424;
    module->shim_this_cpu_off = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3440;
    module->shim_pernet_ops_rwsem = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3456;
    module->shim_panic_notifier_list = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3520;
    module->shim_pv_ops = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3840;
    module->shim_misc_data = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3584;
    module->shim_node_data = module->shim_region + KB_LOCAL_NODE_DATA_OFFSET;
    module->shim_node_states = module->shim_region + KB_LOCAL_NODE_STATES_OFFSET;
    module->shim_boot_cpu_data = module->shim_region + KB_LOCAL_BOOT_CPU_DATA_OFFSET;
    module->shim_current_mm = module->shim_region + KB_LOCAL_CURRENT_MM_OFFSET;
    module->shim_current_task = module->shim_region + KB_LOCAL_CURRENT_TASK_OFFSET;
    write_u64le((uint8_t *)module->shim_cpu_possible_mask, 1);
    write_u64le((uint8_t *)module->shim_cpu_online_mask, 1);
    write_u32le((uint8_t *)module->shim_nr_cpu_ids, 1);
    write_u64le((uint8_t *)module->shim_this_cpu_off, 0);
    write_u32le(module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_NUM_ONLINE_CPUS_OFFSET, 1);

    for (size_t i = 0; i < sizeof(shim_symbols) / sizeof(shim_symbols[0]); i++) {
        write_abs_jump_stub(
            module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE),
            shim_symbols[i].address,
            module->kernel_gs,
            module->kernel_gs);
    }
    write_abs_jump_raw_stub(
        module->shim_region + KB_LOCAL_USB_CONTROL_MSG_STUB_OFFSET,
        (void *)(uintptr_t)&kb_usb_control_msg_entry);
    const uint64_t pv_return_zero = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "crc32_le");
    const uint64_t pv_save_flags = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_save_flags_if_enabled");
    const uint64_t pv_read_pat = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_read_pat_msr");
    for (size_t offset = 0; offset + sizeof(uint64_t) <= 512; offset += sizeof(uint64_t)) {
        write_u64le((uint8_t *)module->shim_pv_ops + offset, pv_return_zero);
    }
    write_u64le((uint8_t *)module->shim_pv_ops + 0xb0, pv_return_zero);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xb8, pv_read_pat);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xf0, pv_save_flags);
    write_u64le(module->kernel_gs + KB_LOCAL_GS_PCPU_HOT_OFFSET, (uint64_t)(uintptr_t)module->shim_current_task);
    write_u64le((uint8_t *)module->shim_current_task + 0xe8, (uint64_t)(uintptr_t)module->shim_current_mm);
    write_u64le((uint8_t *)module->shim_current_task + 0x938, (uint64_t)(uintptr_t)module->shim_current_mm);
    write_u32le((uint8_t *)module->shim_current_task + 0x9b8, 1);
    memcpy((uint8_t *)module->shim_current_task + 0xbd8, "kobox-run", sizeof("kobox-run"));
    write_u64le((uint8_t *)module->shim_node_data, (uint64_t)(uintptr_t)module->shim_node0);
    write_u64le((uint8_t *)module->shim_node_states + 0x80, 1);
    write_u32le((uint8_t *)module->shim_boot_cpu_data + 0x30, 0x10000);
    if (module->shim_node0 != NULL) {
        write_u64le((uint8_t *)module->shim_node0 + 0x29e30, 262144);
    }

    for (size_t i = 0; i < module->section_count; i++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(&module->elf, i, &section);
        if (status != KB_OK) {
            return status;
        }
        if ((section.flags & KB_ELF_SHF_ALLOC) == 0) {
            continue;
        }

        void *memory = (uint8_t *)module->image_base + module->sections[i].offset;
        if (trace_modules_enabled()) {
            fprintf(
                stderr,
                "kobox-loader: section[%zu] %s base=%p offset=0x%llx size=0x%llx\n",
                i,
                section.name,
                memory,
                (unsigned long long)module->sections[i].offset,
                (unsigned long long)module->sections[i].size);
        }
        if (module->sections[i].size == 0) {
            module->sections[i].base = memory;
            continue;
        }
        if (section.type != KB_ELF_SHT_NOBITS) {
            if (!range_fits(module->elf.size, section.offset, section.size)) {
                return KB_ERR_INVALID;
            }
            memcpy(memory, module->elf.data + section.offset, (size_t)section.size);
        }

        module->sections[i].base = memory;
    }
    return KB_OK;
}

static kb_status_t relocation_target(const kb_module_t *module, const kb_elf_relocation_t *relocation, uint8_t **out_target)
{
    if (relocation->target_section_index >= module->section_count) {
        return KB_ERR_INVALID;
    }
    const loaded_section_t *section = &module->sections[relocation->target_section_index];
    if (section->base == 0 || relocation->offset >= section->size) {
        return KB_ERR_INVALID;
    }
    *out_target = (uint8_t *)section->base + relocation->offset;
    return KB_OK;
}

static int relocation_operand_fits(const kb_module_t *module, const kb_elf_relocation_t *relocation, uint64_t width)
{
    if (relocation->target_section_index >= module->section_count) {
        return 0;
    }
    const loaded_section_t *section = &module->sections[relocation->target_section_index];
    return section->base != 0 &&
        relocation->offset <= section->size &&
        width <= section->size - relocation->offset;
}

static int value_fits_u32(int64_t value)
{
    return value >= 0 && (uint64_t)value <= UINT32_MAX;
}

static int value_fits_i32(int64_t value)
{
    return value >= INT32_MIN && value <= INT32_MAX;
}

static int module_backend_is_pachaos(const kb_module_t *module)
{
    (void)module;
    const char *backend = getenv("KOBOX_BACKEND");
    return backend != NULL && (strcmp(backend, "pachaos") == 0 || strcmp(backend, "pachaos_capsule") == 0);
}

static int patch_local_x86_64_external(const kb_elf_symbol_t *symbol, const kb_elf_relocation_t *relocation, uint8_t *target)
{
    if (relocation->type != KB_ELF_R_X86_64_PC32 && relocation->type != KB_ELF_R_X86_64_PLT32) {
        return 0;
    }
    if (strcmp(symbol->name, "__x86_return_thunk") == 0) {
        target[-1] = 0xc3;
        memset(target, 0x90, 4);
        return 1;
    }
    if (strcmp(symbol->name, "__fentry__") == 0) {
        memset(target - 1, 0x90, 5);
        return 1;
    }
    int indirect_thunk_register = -1;
    if (strcmp(symbol->name, "__x86_indirect_thunk_rax") == 0) {
        indirect_thunk_register = 0;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_rcx") == 0) {
        indirect_thunk_register = 1;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_rdx") == 0) {
        indirect_thunk_register = 2;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_rbx") == 0) {
        indirect_thunk_register = 3;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_rsi") == 0) {
        indirect_thunk_register = 6;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r8") == 0) {
        indirect_thunk_register = 8;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r9") == 0) {
        indirect_thunk_register = 9;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r10") == 0) {
        indirect_thunk_register = 10;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r11") == 0) {
        indirect_thunk_register = 11;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r12") == 0) {
        indirect_thunk_register = 12;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r13") == 0) {
        indirect_thunk_register = 13;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r14") == 0) {
        indirect_thunk_register = 14;
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_r15") == 0) {
        indirect_thunk_register = 15;
    }
    if (indirect_thunk_register >= 0 && (target[-1] == 0xe8 || target[-1] == 0xe9)) {
        const uint8_t modrm_base = target[-1] == 0xe8 ? 0xd0 : 0xe0;
        if (indirect_thunk_register < 8) {
            target[-1] = 0xff;
            target[0] = (uint8_t)(modrm_base + indirect_thunk_register);
            memset(target + 1, 0x90, 3);
        } else {
            target[-1] = 0x41;
            target[0] = 0xff;
            target[1] = (uint8_t)(modrm_base + (indirect_thunk_register - 8));
            memset(target + 2, 0x90, 2);
        }
        return 1;
    }
    if (strcmp(symbol->name, "__x86_indirect_thunk_rax") == 0) {
        if (target[-1] == 0xe8) {
            target[-1] = 0xff;
            target[0] = 0xd0;
            memset(target + 1, 0x90, 3);
            return 1;
        }
        if (target[-1] == 0xe9) {
            target[-1] = 0xff;
            target[0] = 0xe0;
            memset(target + 1, 0x90, 3);
            return 1;
        }
    }
    if (strcmp(symbol->name, "__x86_indirect_thunk_rdx") == 0) {
        if (target[-1] == 0xe8) {
            target[-1] = 0xff;
            target[0] = 0xd2;
            memset(target + 1, 0x90, 3);
            return 1;
        }
        if (target[-1] == 0xe9) {
            target[-1] = 0xff;
            target[0] = 0xe2;
            memset(target + 1, 0x90, 3);
            return 1;
        }
    }
    if (strcmp(symbol->name, "__x86_indirect_thunk_r8") == 0) {
        if (target[-1] == 0xe8) {
            target[-1] = 0x41;
            target[0] = 0xff;
            target[1] = 0xd0;
            memset(target + 2, 0x90, 2);
            return 1;
        }
        if (target[-1] == 0xe9) {
            target[-1] = 0x41;
            target[0] = 0xff;
            target[1] = 0xe0;
            memset(target + 2, 0x90, 2);
            return 1;
        }
    }
    if (strcmp(symbol->name, "__x86_indirect_thunk_r10") == 0) {
        if (target[-1] == 0xe8) {
            target[-1] = 0x41;
            target[0] = 0xff;
            target[1] = 0xd2;
            memset(target + 2, 0x90, 2);
            return 1;
        }
        if (target[-1] == 0xe9) {
            target[-1] = 0x41;
            target[0] = 0xff;
            target[1] = 0xe2;
            memset(target + 2, 0x90, 2);
            return 1;
        }
    }
    return 0;
}

static void *lookup_internal_symbol_override(kb_module_t *module, const char *name)
{
    if (strcmp(name, "_nv037805rm") == 0 || strcmp(name, "nv_kthread_q_stop") == 0 ||
        strcmp(name, "os_flush_cpu_cache") == 0 || strcmp(name, "os_flush_cpu_cache_all") == 0)
    {
        return lookup_module_shim_symbol(module, "crc32_le");
    }
    if (strcmp(name, "usb_enable_lpm") == 0 ||
        strcmp(name, "usb_disable_lpm") == 0 ||
        strcmp(name, "usb_unlocked_enable_lpm") == 0 ||
        strcmp(name, "usb_unlocked_disable_lpm") == 0 ||
        strcmp(name, "usb_device_supports_lpm") == 0 ||
        strcmp(name, "usb_enable_usb2_hardware_lpm") == 0 ||
        strcmp(name, "usb_disable_usb2_hardware_lpm") == 0)
    {
        return lookup_module_shim_symbol(module, "crc32_le");
    }
    if (strcmp(name, "usb_control_msg") == 0) {
        return module->shim_region + KB_LOCAL_USB_CONTROL_MSG_STUB_OFFSET;
    }
    if (strcmp(name, "hub_port_debounce") == 0) {
        const char *backend = getenv("KOBOX_BACKEND");
        if (backend != NULL && strcmp(backend, "pachaos") == 0) {
            return lookup_module_shim_symbol(module, "hub_port_debounce");
        }
    }
    return 0;
}

static int should_interpose_exported_symbol(const char *name)
{
    return strcmp(name, "usb_deregister") == 0 ||
           strcmp(name, "usb_deregister_dev") == 0 ||
           strcmp(name, "usb_find_common_endpoints") == 0 ||
           strcmp(name, "usb_find_interface") == 0 ||
           strcmp(name, "usb_hcd_pci_probe") == 0 ||
           strcmp(name, "usb_hcd_pci_remove") == 0 ||
           strcmp(name, "usb_hcd_pci_shutdown") == 0 ||
           strcmp(name, "usb_kill_urb") == 0 ||
           strcmp(name, "usb_register_driver") == 0 ||
           strcmp(name, "usb_submit_urb") == 0 ||
           strcmp(name, "usb_unlink_urb") == 0;
}

static int relocation_is_direct_call(const uint8_t *target)
{
    return target != 0 && target[-1] == 0xe8;
}

static kb_status_t apply_one_relocation(kb_module_t *module, const kb_elf_relocation_t *relocation)
{
    uint8_t *target = 0;
    kb_status_t status = relocation_target(module, relocation, &target);
    if (status != KB_OK) {
        return status;
    }

    kb_elf_symbol_t symbol;
    status = kb_elf_symbol(
        &module->elf,
        relocation->symbol_table_section_index,
        relocation->symbol_index,
        &symbol);
    if (status != KB_OK) {
        return status;
    }

    uint64_t symbol_address = 0;
    if (symbol.section_index == KB_ELF_SHN_UNDEF) {
        if (relocation->offset > 0 &&
            relocation_operand_fits(module, relocation, 4) &&
            patch_local_x86_64_external(&symbol, relocation, target))
        {
            return KB_OK;
        }

        void *address = 0;
        int address_from_exported_module = 0;
        if (should_interpose_exported_symbol(symbol.name)) {
            address = lookup_module_shim_symbol(module, symbol.name);
        }
        if (address == 0) {
            address = lookup_exported_symbol(symbol.name);
            address_from_exported_module = address != 0;
        }
        if (address == 0) {
            address = lookup_module_shim_symbol(module, symbol.name);
        }
        if (address == 0) {
            if (trace_modules_enabled()) {
                fprintf(
                    stderr,
                    "kobox-loader: unresolved symbol name=%s relocation_type=%u relocation_offset=0x%llx\n",
                    symbol.name,
                    (unsigned)relocation->type,
                    (unsigned long long)relocation->offset);
            }
            return KB_ERR_UNSUPPORTED;
        }
        if (address_from_exported_module && kb_module_is_executable_address(address)) {
            unsigned long callee_gs = kb_module_kernel_gs_for_address(address);
            if (callee_gs != 0 && callee_gs != (unsigned long)(uintptr_t)module->kernel_gs) {
                void *stub = allocate_external_call_stub(module, address, (void *)(uintptr_t)callee_gs);
                if (stub == NULL) {
                    return KB_ERR_NOMEM;
                }
                if (trace_modules_enabled()) {
                    fprintf(
                        stderr,
                        "kobox-loader: cross-module stub symbol=%s target=%p caller_gs=%p callee_gs=%p stub=%p\n",
                        symbol.name,
                        address,
                        (void *)module->kernel_gs,
                        (void *)(uintptr_t)callee_gs,
                        stub);
                }
                address = stub;
            }
        }
        symbol_address = (uint64_t)(uintptr_t)address;
    } else {
        status = loaded_section_address(module, symbol.section_index, symbol.value, &symbol_address);
        if (status != KB_OK) {
            return status;
        }
        void *override = lookup_internal_symbol_override(module, symbol.name);
        if (override != 0) {
            symbol_address = (uint64_t)(uintptr_t)override;
        }
    }

    const int64_t addend = relocation->has_addend ? relocation->addend : 0;
    const uint64_t place = (uint64_t)(uintptr_t)target;
    uint64_t relocated_symbol_address = symbol_address;
    if (symbol.section_index != KB_ELF_SHN_UNDEF &&
        (relocation->type == KB_ELF_R_X86_64_PC32 || relocation->type == KB_ELF_R_X86_64_PLT32) &&
        relocation->offset > 0 &&
        relocation_is_direct_call(target) &&
        strcmp(symbol.name, "nv_pci_count_devices") == 0 &&
        getenv("KOBOX_MOCK_NVIDIA_PROBE_COUNT") != NULL)
    {
        void *stub = lookup_module_shim_symbol(module, "kobox_nvidia_mock_nv_pci_count_devices");
        if (stub != NULL) {
            relocated_symbol_address = (uint64_t)(uintptr_t)stub;
        }
    }
    const int64_t value = (int64_t)relocated_symbol_address + addend;
    switch (relocation->type) {
    case KB_ELF_R_X86_64_NONE:
        return KB_OK;
    case KB_ELF_R_X86_64_64:
        if (!relocation_operand_fits(module, relocation, 8)) {
            return KB_ERR_INVALID;
        }
        write_u64le(target, (uint64_t)value);
        return KB_OK;
    case KB_ELF_R_X86_64_PC32:
    case KB_ELF_R_X86_64_PLT32:
        if (!relocation_operand_fits(module, relocation, 4)) {
            return KB_ERR_INVALID;
        }
        if (!value_fits_i32(value - (int64_t)place)) {
            return KB_ERR_UNSUPPORTED;
        }
        write_u32le(target, (uint32_t)(value - (int64_t)place));
        return KB_OK;
    case KB_ELF_R_X86_64_PC64:
        if (!relocation_operand_fits(module, relocation, 8)) {
            return KB_ERR_INVALID;
        }
        write_u64le(target, (uint64_t)(value - (int64_t)place));
        return KB_OK;
    case KB_ELF_R_X86_64_32:
        if (!relocation_operand_fits(module, relocation, 4)) {
            return KB_ERR_INVALID;
        }
        if (!value_fits_u32(value)) {
            return KB_ERR_UNSUPPORTED;
        }
        write_u32le(target, (uint32_t)value);
        return KB_OK;
    case KB_ELF_R_X86_64_32S:
        if (!relocation_operand_fits(module, relocation, 4)) {
            return KB_ERR_INVALID;
        }
        if (!value_fits_i32(value)) {
            if (trace_modules_enabled()) {
                fprintf(
                    stderr,
                    "kobox-loader: relocation value out of range symbol=%s type=%u value=0x%llx place=0x%llx\n",
                    symbol.name,
                    (unsigned)relocation->type,
                    (unsigned long long)value,
                    (unsigned long long)place);
            }
            return KB_ERR_UNSUPPORTED;
        }
        write_u32le(target, (uint32_t)value);
        return KB_OK;
    default:
        if (trace_modules_enabled()) {
            fprintf(
                stderr,
                "kobox-loader: unsupported relocation symbol=%s type=%u offset=0x%llx\n",
                symbol.name,
                (unsigned)relocation->type,
                (unsigned long long)relocation->offset);
        }
        return KB_ERR_UNSUPPORTED;
    }
}

static kb_status_t apply_relocations(kb_module_t *module)
{
    const size_t section_count = kb_elf_section_count(&module->elf);
    for (size_t section_index = 0; section_index < section_count; section_index++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(&module->elf, section_index, &section);
        if (status != KB_OK) {
            return status;
        }
        if (section.type != KB_ELF_SHT_RELA && section.type != KB_ELF_SHT_REL) {
            continue;
        }
        if (section.info >= module->section_count || module->sections[section.info].base == 0) {
            continue;
        }

        size_t relocation_count = 0;
        status = kb_elf_relocation_count(&module->elf, section_index, &relocation_count);
        if (status != KB_OK) {
            return status;
        }
        for (size_t relocation_index = 0; relocation_index < relocation_count; relocation_index++) {
            kb_elf_relocation_t relocation;
            status = kb_elf_relocation(&module->elf, section_index, relocation_index, &relocation);
            if (status != KB_OK) {
                return status;
            }
            status = apply_one_relocation(module, &relocation);
            if (status != KB_OK) {
                return status;
            }
        }
    }
    return KB_OK;
}

static int symbol_is_static_trace_call(const kb_elf_symbol_t *symbol)
{
    static const char prefix[] = "__SCT__tp_func_";
    return symbol != NULL &&
        symbol_name_pointer_is_valid(symbol->name) &&
        strncmp(symbol->name, prefix, sizeof(prefix) - 1u) == 0;
}

static kb_status_t patch_module_static_trace_calls(kb_module_t *module)
{
    const size_t section_count = kb_elf_section_count(&module->elf);
    size_t patched_count = 0;

    for (size_t section_index = 0; section_index < section_count; section_index++) {
        kb_elf_section_t symbol_section;
        kb_status_t status = kb_elf_section(&module->elf, section_index, &symbol_section);
        if (status != KB_OK) {
            return status;
        }
        if (symbol_section.type != KB_ELF_SHT_SYMTAB && symbol_section.type != KB_ELF_SHT_DYNSYM) {
            continue;
        }

        size_t symbol_count = 0;
        status = kb_elf_symbol_count(&module->elf, section_index, &symbol_count);
        if (status != KB_OK) {
            return status;
        }
        for (size_t symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
            kb_elf_symbol_t symbol;
            status = kb_elf_symbol(&module->elf, section_index, symbol_index, &symbol);
            if (status != KB_OK) {
                return status;
            }
            if (!symbol_is_static_trace_call(&symbol) ||
                symbol.section_index == KB_ELF_SHN_UNDEF ||
                symbol.section_index >= module->section_count)
            {
                continue;
            }

            kb_elf_section_t target_section;
            status = kb_elf_section(&module->elf, symbol.section_index, &target_section);
            if (status != KB_OK) {
                return status;
            }
            loaded_section_t *loaded = &module->sections[symbol.section_index];
            if (loaded->base == NULL || (target_section.flags & KB_ELF_SHF_EXECINSTR) == 0 ||
                symbol.value >= loaded->size)
            {
                continue;
            }

            uint8_t *code = (uint8_t *)loaded->base + symbol.value;
            size_t remaining = (size_t)(loaded->size - symbol.value);
            size_t patch_size = symbol.size != 0 && symbol.size < remaining ? (size_t)symbol.size : remaining;
            if (patch_size > 8u) {
                patch_size = 8u;
            }
            if (patch_size == 0) {
                continue;
            }

            code[0] = 0xc3;
            if (patch_size > 1u) {
                memset(code + 1, 0x90, patch_size - 1u);
            }
            patched_count++;
            if (trace_modules_enabled()) {
                fprintf(
                    stderr,
                    "kobox-loader: patched static trace call module=%s symbol=%s addr=%p size=%zu\n",
                    module->module_name != NULL ? module->module_name : "(unnamed)",
                    symbol.name,
                    (void *)code,
                    patch_size);
            }
        }
    }

    if (patched_count != 0 && trace_modules_enabled()) {
        fprintf(
            stderr,
            "kobox-loader: patched static trace calls module=%s count=%zu\n",
            module->module_name != NULL ? module->module_name : "(unnamed)",
            patched_count);
    }
    return KB_OK;
}

static int xhci_command_doorbell_helper_matches(const uint8_t *code, size_t remaining)
{
    static const uint8_t suffix[] = {
        0x55,
        0x48, 0x89, 0xe5,
        0x53,
        0x48, 0x89, 0xfb,
        0x66, 0x90,
        0x66, 0x90,
        0x48, 0x8b, 0x53, 0x28,
        0x31, 0xc0,
        0x89, 0x02,
        0x48, 0x8b, 0x43, 0x28,
        0x8b, 0x00,
    };
    if (remaining < 5u + sizeof(suffix)) {
        return 0;
    }
    int has_fentry_call = code[0] == 0xe8;
    int has_patched_fentry_nops =
        code[0] == 0x90 &&
        code[1] == 0x90 &&
        code[2] == 0x90 &&
        code[3] == 0x90 &&
        code[4] == 0x90;
    if (!has_fentry_call && !has_patched_fentry_nops) {
        return 0;
    }
    return memcmp(code + 5, suffix, sizeof(suffix)) == 0;
}

static kb_status_t patch_relative_jump(uint8_t *code, const void *target)
{
    const int64_t rel = (int64_t)(uintptr_t)target - ((int64_t)(uintptr_t)code + 5);
    if (!value_fits_i32(rel)) {
        return KB_ERR_UNSUPPORTED;
    }
    code[0] = 0xe9;
    write_u32le(code + 1, (uint32_t)rel);
    return KB_OK;
}

static kb_status_t patch_module_xhci_command_doorbell(kb_module_t *module)
{
    if (module == NULL || module->module_name == NULL ||
        strstr(module->module_name, "xhci-hcd") == NULL ||
        !module_backend_is_pachaos(module))
    {
        return KB_OK;
    }

    void *stub = lookup_module_shim_symbol(module, "kobox_xhci_ring_cmd_db");
    if (stub == NULL) {
        return KB_ERR_UNSUPPORTED;
    }

    size_t patched_count = 0;
    for (size_t section_index = 0; section_index < module->section_count; section_index++) {
        loaded_section_t *section = &module->sections[section_index];
        if (section->base == NULL || section->size == 0 ||
            (section->flags & KB_ELF_SHF_EXECINSTR) == 0)
        {
            continue;
        }
        uint8_t *begin = (uint8_t *)section->base;
        for (uint64_t offset = 0; offset + 31u <= section->size; offset++) {
            uint8_t *code = begin + offset;
            if (!xhci_command_doorbell_helper_matches(code, (size_t)(section->size - offset))) {
                continue;
            }
            kb_status_t status = patch_relative_jump(code, stub);
            if (status != KB_OK) {
                return status;
            }
            patched_count++;
            if (trace_modules_enabled()) {
                fprintf(
                    stderr,
                    "kobox-loader: patched xhci command doorbell module=%s addr=%p stub=%p\n",
                    module->module_name,
                    (void *)code,
                    stub);
            }
        }
    }

    if (patched_count == 0 && trace_modules_enabled()) {
        fprintf(
            stderr,
            "kobox-loader: xhci command doorbell helper not found module=%s\n",
            module->module_name);
    }
    return KB_OK;
}

static void free_loaded_sections(kb_module_t *module)
{
    if (module == 0) {
        return;
    }
    free_section_memory(module->image_base, module->image_size);
}

static void destroy_module(kb_module_t *module)
{
    if (module == 0) {
        return;
    }
    unregister_loaded_module(module);
    unregister_module_exports(module);
    free_loaded_sections(module);
    free(module->shim_node0);
    free(module->module_name);
    free(module->sections);
    free(module);
}

static kb_status_t enter_module_context(kb_module_t *module, unsigned long *out_old_gs)
{
    if (module == 0 || out_old_gs == 0) {
        return KB_ERR_INVALID;
    }
    kb_shim_set_backend(module->backend);
#if !defined(_WIN32) && defined(__x86_64__)
    if (module_backend_is_pachaos(module)) {
        (void)syscall(SYS_arch_prctl, ARCH_SET_GS, 0ul);
    }
    *out_old_gs = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, out_old_gs) != 0) {
        int saved_errno = errno;
        fprintf(
            stderr,
            "kobox-loader: enter module context failed phase=get-gs module=%s errno=%d kernel_gs=%p old_gs=0x%lx\n",
            module->module_name == NULL ? "(unnamed)" : module->module_name,
            saved_errno,
            (void *)module->kernel_gs,
            *out_old_gs);
        fflush(stderr);
        kb_shim_set_backend(0);
        return KB_ERR_UNSUPPORTED;
    }
    const unsigned long desired_gs = (unsigned long)(uintptr_t)module->kernel_gs;
    long set_gs_result = syscall(SYS_arch_prctl, ARCH_SET_GS, desired_gs);
    if (set_gs_result != 0) {
        int saved_errno = errno;
        unsigned long current_gs = 0;
        long get_after_set_result = syscall(SYS_arch_prctl, ARCH_GET_GS, &current_gs);
        if (get_after_set_result == 0 && current_gs == desired_gs) {
            return KB_OK;
        }
        fprintf(
            stderr,
            "kobox-loader: enter module context failed phase=set-gs module=%s result=%ld errno=%d get_after=%ld kernel_gs=%p old_gs=0x%lx current_gs=0x%lx\n",
            module->module_name == NULL ? "(unnamed)" : module->module_name,
            set_gs_result,
            saved_errno,
            get_after_set_result,
            (void *)module->kernel_gs,
            *out_old_gs,
            current_gs);
        fflush(stderr);
        kb_shim_set_backend(0);
        return KB_ERR_UNSUPPORTED;
    }
#else
    *out_old_gs = 0;
#endif
    return KB_OK;
}

static void leave_module_context(unsigned long old_gs)
{
#if !defined(_WIN32) && defined(__x86_64__)
    (void)syscall(SYS_arch_prctl, ARCH_SET_GS, old_gs);
#else
    (void)old_gs;
#endif
    kb_shim_set_backend(0);
}

static kb_status_t prepare_module(kb_module_t *module)
{
    kb_status_t status = load_sections(module);
    if (status != KB_OK) {
        return status;
    }
    status = apply_relocations(module);
    if (status != KB_OK) {
        return status;
    }
    status = patch_module_static_trace_calls(module);
    if (status != KB_OK) {
        return status;
    }
    status = patch_module_xhci_command_doorbell(module);
    if (status != KB_OK) {
        return status;
    }
    status = register_module_exports(module);
    if (status != KB_OK) {
        return status;
    }

    uint64_t init_address = 0;
    status = find_symbol_address(module, "init_module", &init_address);
    if (status == KB_ERR_NOT_FOUND) {
        module->init_module = NULL;
    } else if (status != KB_OK) {
        return status;
    } else {
        module->init_module = (int (*)(void))(uintptr_t)init_address;
    }

    uint64_t cleanup_address = 0;
    status = find_symbol_address(module, "cleanup_module", &cleanup_address);
    if (status == KB_OK) {
        module->cleanup_module = (void (*)(void))(uintptr_t)cleanup_address;
    } else if (status != KB_ERR_NOT_FOUND) {
        return status;
    }
    return KB_OK;
}

const char *kb_module_loader_version(void)
{
    return "kobox-loader/dev";
}

kb_status_t kb_module_open_image(
    const kb_module_image_t *image,
    kb_backend_t *backend,
    kb_module_t **out_module)
{
    if (image == 0 || image->data == 0 || image->size == 0 || backend == 0 || out_module == 0) {
        return KB_ERR_INVALID;
    }
    *out_module = 0;

    kb_module_t *module = calloc(1, sizeof(*module));
    if (module == 0) {
        return KB_ERR_NOMEM;
    }
    module->backend = backend;
    module->module_name = kb_copy_string(image->name);
    if (module->module_name == NULL) {
        destroy_module(module);
        return KB_ERR_NOMEM;
    }
    module->shim_node0 = calloc(1, 0x2a000);
    if (module->shim_node0 == 0) {
        destroy_module(module);
        return KB_ERR_NOMEM;
    }

    kb_status_t status = kb_elf_open(image->data, image->size, &module->elf);
    if (status != KB_OK) {
        destroy_module(module);
        return status;
    }
    if (module->elf.type != KB_ELF_ET_REL || module->elf.machine != KB_ELF_EM_X86_64) {
        destroy_module(module);
        return KB_ERR_UNSUPPORTED;
    }

    module->section_count = kb_elf_section_count(&module->elf);
    module->sections = calloc(module->section_count, sizeof(module->sections[0]));
    if (module->sections == 0) {
        destroy_module(module);
        return KB_ERR_NOMEM;
    }
#if !defined(_WIN32) && defined(__x86_64__)
    write_u64le(module->kernel_gs + 0x28, 0x6b6f626f785f6773ull);
#endif

    status = prepare_module(module);
    if (status != KB_OK) {
        destroy_module(module);
        return status;
    }
    register_loaded_module(module);

    *out_module = module;
    if (trace_modules_enabled()) {
        fprintf(
            stderr,
            "kobox-loader: loaded %s base=%p size=0x%llx shim=%p..%p usb_control_msg_stub=%p shim_stub_size=%u\n",
            image->name == NULL ? "(unnamed)" : image->name,
            module->image_base,
            (unsigned long long)module->image_size,
            (void *)module->shim_region,
            (void *)(module->shim_region + KB_LOCAL_SHIM_REGION_SIZE),
            (void *)(module->shim_region + KB_LOCAL_USB_CONTROL_MSG_STUB_OFFSET),
            (unsigned)KB_LOCAL_SHIM_STUB_SIZE);
    }
    return KB_OK;
}

kb_status_t kb_module_call_init(kb_module_t *module, int *out_result)
{
    if (module == 0 || out_result == 0) {
        return KB_ERR_INVALID;
    }
    if (module->init_module == 0) {
        return KB_ERR_NOT_FOUND;
    }
    unsigned long old_gs = 0;
    kb_status_t status = enter_module_context(module, &old_gs);
    if (status != KB_OK) {
        return status;
    }
    kb_active_module = module;
    *out_result = module->init_module();
    leave_module_context(old_gs);
    kb_active_module = NULL;
    return KB_OK;
}

kb_status_t kb_module_call_cleanup(kb_module_t *module)
{
    if (module == 0) {
        return KB_ERR_INVALID;
    }
    if (module->cleanup_module == 0) {
        return KB_ERR_NOT_FOUND;
    }
    unsigned long old_gs = 0;
    kb_status_t status = enter_module_context(module, &old_gs);
    if (status != KB_OK) {
        fprintf(
            stderr,
            "kobox-loader: cleanup context failed module=%s status=%d cleanup=%p\n",
            module->module_name == NULL ? "(unnamed)" : module->module_name,
            status,
            (void *)module->cleanup_module);
        fflush(stderr);
        return status;
    }
    kb_active_module = module;
    module->cleanup_module();
    leave_module_context(old_gs);
    kb_active_module = NULL;
    return KB_OK;
}

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
    void *mm = kb_active_module == NULL ? NULL : kb_active_module->shim_current_mm;
    kb_prepare_fake_vma(vma, mm, start, start + 0x10000u, start >> 12);
    kb_write_ptr_field(vma, 0x88, file);
    kb_write_ptr_field(vma, 0x98, file);
    kb_write_ptr_field(vma, 0xa0, file);
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
    if (enter_module_context(owner, old_gs) != KB_OK) {
        return -1;
    }
    kb_active_module = owner;
    return 0;
}

static void kb_leave_owner_context(kb_module_t *owner, unsigned long old_gs)
{
    if (owner == NULL) {
        return;
    }
    leave_module_context(old_gs);
    kb_active_module = NULL;
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
        if (strcmp(kb_chrdev_name_for_dev(kb_cdev_records[i].dev), name) == 0 && kb_decode_minor(kb_cdev_records[i].dev) == minor) {
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
        failures += kb_run_file_ops_child("chrdev:nvidia-frontend", &frontend->fops_view, frontend->owner_module, frontend->fops, kb_encode_dev(frontend->major, 255), NULL) != 0;
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

void kb_module_close(kb_module_t *module)
{
    destroy_module(module);
}
