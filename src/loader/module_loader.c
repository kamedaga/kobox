#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/elf.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "loader/module_context.h"
#include "loader/symbol_registry.h"
#include "linux_personality/linux_core_symbols.h"
#include "linux_personality/linux_symbol_registry.h"
#include "linux_personality/linux_stub_symbols.h"
#include "linux_subsystem/block/block_symbols.h"
#include "linux_subsystem/dma/dma_symbols.h"
#include "linux_subsystem/fs/fs_symbols.h"
#include "linux_subsystem/fs/kernel_object_registry.h"
#include "linux_subsystem/input/input_symbols.h"
#include "linux_subsystem/kvm/kvm_symbols.h"
#include "linux_subsystem/net/net_symbols.h"
#include "linux_subsystem/pci/pci_symbols.h"
#include "linux_subsystem/security/security_symbols.h"
#include "linux_subsystem/sound/sound_symbols.h"
#include "linux_subsystem/usb/usb_symbols.h"

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
    KB_LOCAL_SHIM_STUB_COUNT = 8192,
    KB_LOCAL_SHIM_DATA_SIZE = 32768,
    KB_LOCAL_SHIM_REGION_SIZE = (KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT) + KB_LOCAL_SHIM_DATA_SIZE,
    KB_LOCAL_SHIM_DATA_OFFSET = KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT,
    KB_LOCAL_NODE_DATA_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 6144,
    KB_LOCAL_NODE_STATES_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 6400,
    KB_LOCAL_BOOT_CPU_DATA_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 6656,
    KB_LOCAL_PAGE_OFFSET_BASE_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 7168,
    KB_LOCAL_VMEMMAP_BASE_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 7176,
    KB_LOCAL_PHYS_BASE_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 7184,
    KB_LOCAL_CURRENT_MM_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 4608,
    KB_LOCAL_CURRENT_TASK_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 8192,
    KB_LOCAL_USB_DATA_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 11264,
    KB_LOCAL_JIFFIES_OFFSET = KB_LOCAL_SHIM_DATA_OFFSET + 4096,
    KB_LOCAL_GS_SIZE = 4096,
    KB_LOCAL_GS_PCPU_HOT_OFFSET = 0x100,
    KB_LOCAL_GS_STACK_CHK_GUARD_OFFSET = 0x180,
    KB_LOCAL_GS_PREEMPT_COUNT_OFFSET = 0x188,
    KB_LOCAL_GS_CPU_NUMBER_OFFSET = 0x190,
    KB_LOCAL_GS_NUMA_NODE_OFFSET = 0x198,
    KB_LOCAL_GS_CPU_INFO_OFFSET = 0x200,
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

static int trace_kvm_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_KVM");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int trace_kvm_relocations_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_KVM_RELOC");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

struct kb_module {
    kb_device_backend_t *backend;
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
    void *shim_percpu_counter_batch;
    void *shim_const_pcpu_hot;
    void *shim_this_cpu_off;
    void *shim_per_cpu_offset;
    void *shim_var_waitqueue;
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

static kb_module_t *kb_active_module;
static kb_module_t *kb_loaded_modules;
uintptr_t kb_current_external_call_target;
uintptr_t kb_current_external_call_caller_gs;
uintptr_t kb_current_external_call_callee_gs;

kb_module_t *kb_loader_active_module(void)
{
    return kb_active_module;
}

void kb_loader_set_active_module(kb_module_t *module)
{
    kb_active_module = module;
}

void *kb_loader_module_current_mm(const kb_module_t *module)
{
    return module == NULL ? NULL : module->shim_current_mm;
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

static unsigned long kb_x86_save_flags_if_enabled(void)
{
    return 0x200ul;
}

static void kb_x86_cpuid_shim(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    uint32_t in_eax = eax == NULL ? 0 : *eax;
    uint32_t in_ecx = ecx == NULL ? 0 : *ecx;
    uint32_t out_eax = 0;
    uint32_t out_ebx = 0;
    uint32_t out_ecx = 0;
    uint32_t out_edx = 0;
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("cpuid"
                     : "=a"(out_eax), "=b"(out_ebx), "=c"(out_ecx), "=d"(out_edx)
                     : "a"(in_eax), "c"(in_ecx));
#endif
    if (eax != NULL) {
        *eax = out_eax;
    }
    if (ebx != NULL) {
        *ebx = out_ebx;
    }
    if (ecx != NULL) {
        *ecx = out_ecx;
    }
    if (edx != NULL) {
        *edx = out_edx;
    }
}

static uint64_t kb_x86_read_pat_msr(void)
{
    return 0x106ull;
}

static uint64_t kb_x86_read_msr_shim(uint32_t msr)
{
    switch (msr) {
    case 0x277u: /* MSR_IA32_CR_PAT */
        return kb_x86_read_pat_msr();
    case 0xc0000080u: /* MSR_EFER */
        return 0x500ull;
    case 0x48u: /* MSR_IA32_SPEC_CTRL */
        return 0;
    default:
        return 0;
    }
}

static int kb_x86_write_msr_shim(uint32_t msr, uint64_t value)
{
    (void)msr;
    (void)value;
    return 0;
}

static int kb_x86_read_msr_safe_shim(uint32_t msr, uint64_t *value)
{
    if (value != NULL) {
        *value = kb_x86_read_msr_shim(msr);
    }
    return 0;
}

static int kb_x86_write_msr_safe_shim(uint32_t msr, uint64_t value)
{
    return kb_x86_write_msr_shim(msr, value);
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
        "mov 624(%rsp), %rsi\n\t"
        "mov $0x1001, %edi\n\t"
        "mov $158, %eax\n\t"
        "syscall\n\t"
        "movq $0, kb_current_external_call_target(%rip)\n\t"
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
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rbp, "rbp")
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

#define KB_DEFINE_X86_INDIRECT_THUNK(name) static void name(void) { kb_noop_stub(); }
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rax)
KB_DEFINE_X86_INDIRECT_THUNK(kb_x86_indirect_thunk_rbp)
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
    if (trace_kvm_enabled() && n > (1u << 20)) {
        fprintf(stderr,
            "kobox-kvm: memset dst=%p value=0x%x n=0x%llx external_target=%p caller_gs=0x%llx callee_gs=0x%llx\n",
            dst,
            value,
            (unsigned long long)n,
            (void *)kb_current_external_call_target,
            (unsigned long long)kb_current_external_call_caller_gs,
            (unsigned long long)kb_current_external_call_callee_gs);
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

static int kb_vprintk(const char *fmt, va_list args)
{
    return kb_vprintk_safe(fmt, args);
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

static const kb_linux_symbol_t shim_symbols[] = {
    {"kobox_x86_save_flags_if_enabled", (void *)(uintptr_t)&kb_x86_save_flags_if_enabled},
    {"kobox_x86_cpuid", (void *)(uintptr_t)&kb_x86_cpuid_shim},
    {"kobox_x86_read_msr", (void *)(uintptr_t)&kb_x86_read_msr_shim},
    {"kobox_x86_read_msr_safe", (void *)(uintptr_t)&kb_x86_read_msr_safe_shim},
    {"kobox_x86_read_pat_msr", (void *)(uintptr_t)&kb_x86_read_pat_msr},
    {"kobox_x86_write_msr", (void *)(uintptr_t)&kb_x86_write_msr_shim},
    {"kobox_x86_write_msr_safe", (void *)(uintptr_t)&kb_x86_write_msr_safe_shim},
    {"kobox_nvidia_mock_nv_pci_count_devices", (void *)(uintptr_t)&kb_nvidia_mock_nv_pci_count_devices},
    {"vprintk", (void *)(uintptr_t)&kb_vprintk},
    {"kobox_xhci_ring_cmd_db", (void *)(uintptr_t)&kb_pci_xhci_ring_cmd_db},
    {"__bitmap_and", (void *)(uintptr_t)&kb_bitmap_and_shim},
    {"__bitmap_andnot", (void *)(uintptr_t)&kb_bitmap_andnot_shim},
    {"__bitmap_clear", (void *)(uintptr_t)&kb_bitmap_clear_shim},
    {"__bitmap_complement", (void *)(uintptr_t)&kb_bitmap_complement_shim},
    {"__bitmap_intersects", (void *)(uintptr_t)&kb_bitmap_intersects_shim},
    {"__bitmap_or", (void *)(uintptr_t)&kb_bitmap_or_shim},
    {"__bitmap_set", (void *)(uintptr_t)&kb_bitmap_set_shim},
    {"__bitmap_shift_left", (void *)(uintptr_t)&kb_bitmap_shift_left_shim},
    {"__bitmap_shift_right", (void *)(uintptr_t)&kb_bitmap_shift_right_shim},
    {"__sw_hweight32", (void *)(uintptr_t)&kb_hweight32},
    {"__sw_hweight64", (void *)(uintptr_t)&kb_hweight64},
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
    {"__tracepoint_mmap_lock_acquire_returned", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_mmap_lock_released", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_mmap_lock_start_locking", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_block_bio_complete", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_block_bio_remap", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_nvme_sq", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_read_msr", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__tracepoint_write_msr", (void *)(uintptr_t)&kb_tracepoint_disabled},
    {"__x86_indirect_thunk_r10", (void *)(uintptr_t)&kb_x86_indirect_thunk_r10},
    {"__x86_indirect_thunk_r11", (void *)(uintptr_t)&kb_x86_indirect_thunk_r11},
    {"__x86_indirect_thunk_r12", (void *)(uintptr_t)&kb_x86_indirect_thunk_r12},
    {"__x86_indirect_thunk_r13", (void *)(uintptr_t)&kb_x86_indirect_thunk_r13},
    {"__x86_indirect_thunk_r14", (void *)(uintptr_t)&kb_x86_indirect_thunk_r14},
    {"__x86_indirect_thunk_r15", (void *)(uintptr_t)&kb_x86_indirect_thunk_r15},
    {"__x86_indirect_thunk_r8", (void *)(uintptr_t)&kb_x86_indirect_thunk_r8},
    {"__x86_indirect_thunk_r9", (void *)(uintptr_t)&kb_x86_indirect_thunk_r9},
    {"__x86_indirect_thunk_rax", (void *)(uintptr_t)&kb_x86_indirect_thunk_rax},
    {"__x86_indirect_thunk_rbp", (void *)(uintptr_t)&kb_x86_indirect_thunk_rbp},
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
    {"kasprintf", (void *)(uintptr_t)&kb_kasprintf_shim},
    {"krealloc", (void *)(uintptr_t)&kb_krealloc_shim},
    {"ksize", (void *)(uintptr_t)&kb_ksize_shim},
    {"kvasprintf", (void *)(uintptr_t)&kb_kvasprintf_shim},
    {"memset_io", (void *)(uintptr_t)&kb_memset_io_shim},
    {"simple_strtoul", (void *)(uintptr_t)&strtoul},
    {"sort", (void *)(uintptr_t)&kb_kernel_sort_shim},
    {"strscpy", (void *)(uintptr_t)&kb_strscpy_shim},
    {"devm_ioremap", (void *)(uintptr_t)&kb_devm_ioremap_shim},
    {"kobject_get_path", (void *)(uintptr_t)&kb_kobject_get_path_shim},
    {"kstrdup", (void *)(uintptr_t)&kb_kstrdup_shim},
    {"memchr", (void *)(uintptr_t)&memchr},
    {"memdup_user", (void *)(uintptr_t)&kb_memdup_user_shim},
    {"scnprintf", (void *)(uintptr_t)&kb_scnprintf_shim},
    {"vscnprintf", (void *)(uintptr_t)&kb_vscnprintf_shim},
    {"utf16s_to_utf8s", (void *)(uintptr_t)&kb_utf16s_to_utf8s_shim},
};

_Static_assert(
    sizeof(shim_symbols) / sizeof(shim_symbols[0]) <= KB_LOCAL_SHIM_STUB_COUNT,
    "increase KB_LOCAL_SHIM_STUB_COUNT");

static size_t shim_symbol_count(void)
{
    size_t block_count = 0;
    size_t core_count = 0;
    size_t dma_count = 0;
    size_t fs_count = 0;
    size_t input_count = 0;
    size_t kvm_count = 0;
    size_t net_count = 0;
    size_t pci_count = 0;
    size_t security_count = 0;
    size_t sound_count = 0;
    size_t stub_count = 0;
    size_t usb_count = 0;
    (void)kb_linux_block_symbols(&block_count);
    (void)kb_linux_core_symbols(&core_count);
    (void)kb_linux_dma_symbols(&dma_count);
    (void)kb_linux_fs_symbols(&fs_count);
    (void)kb_linux_input_symbols(&input_count);
    (void)kb_linux_kvm_symbols(&kvm_count);
    (void)kb_linux_net_symbols(&net_count);
    (void)kb_linux_pci_symbols(&pci_count);
    (void)kb_linux_security_symbols(&security_count);
    (void)kb_linux_sound_symbols(&sound_count);
    (void)kb_linux_stub_symbols(&stub_count);
    (void)kb_linux_usb_symbols(&usb_count);
    return (sizeof(shim_symbols) / sizeof(shim_symbols[0])) +
        core_count + stub_count + pci_count + usb_count + block_count + dma_count + fs_count + input_count +
        net_count + security_count + sound_count + kvm_count;
}

static const kb_linux_symbol_t *shim_symbol_at(size_t index)
{
    const size_t core_count = sizeof(shim_symbols) / sizeof(shim_symbols[0]);
    if (index < core_count) {
        return &shim_symbols[index];
    }

    size_t core_provider_count = 0;
    const kb_linux_symbol_t *core_symbols = kb_linux_core_symbols(&core_provider_count);
    index -= core_count;
    if (index < core_provider_count) {
        return &core_symbols[index];
    }

    size_t stub_count = 0;
    const kb_linux_symbol_t *stub_symbols = kb_linux_stub_symbols(&stub_count);
    index -= core_provider_count;
    if (index < stub_count) {
        return &stub_symbols[index];
    }

    size_t pci_count = 0;
    const kb_linux_symbol_t *pci_symbols = kb_linux_pci_symbols(&pci_count);
    index -= stub_count;
    if (index < pci_count) {
        return &pci_symbols[index];
    }

    size_t usb_count = 0;
    const kb_linux_symbol_t *usb_symbols = kb_linux_usb_symbols(&usb_count);
    index -= pci_count;
    if (index < usb_count) {
        return &usb_symbols[index];
    }

    size_t block_count = 0;
    const kb_linux_symbol_t *block_symbols = kb_linux_block_symbols(&block_count);
    index -= usb_count;
    if (index < block_count) {
        return &block_symbols[index];
    }

    size_t dma_count = 0;
    const kb_linux_symbol_t *dma_symbols = kb_linux_dma_symbols(&dma_count);
    index -= block_count;
    if (index < dma_count) {
        return &dma_symbols[index];
    }

    size_t fs_count = 0;
    const kb_linux_symbol_t *fs_symbols = kb_linux_fs_symbols(&fs_count);
    index -= dma_count;
    if (index < fs_count) {
        return &fs_symbols[index];
    }

    size_t input_count = 0;
    const kb_linux_symbol_t *input_symbols = kb_linux_input_symbols(&input_count);
    index -= fs_count;
    if (index < input_count) {
        return &input_symbols[index];
    }

    size_t net_count = 0;
    const kb_linux_symbol_t *net_symbols = kb_linux_net_symbols(&net_count);
    index -= input_count;
    if (index < net_count) {
        return &net_symbols[index];
    }

    size_t security_count = 0;
    const kb_linux_symbol_t *security_symbols = kb_linux_security_symbols(&security_count);
    index -= net_count;
    if (index < security_count) {
        return &security_symbols[index];
    }

    size_t sound_count = 0;
    const kb_linux_symbol_t *sound_symbols = kb_linux_sound_symbols(&sound_count);
    index -= security_count;
    if (index < sound_count) {
        return &sound_symbols[index];
    }

    size_t kvm_count = 0;
    const kb_linux_symbol_t *kvm_symbols = kb_linux_kvm_symbols(&kvm_count);
    index -= sound_count;
    return index < kvm_count ? &kvm_symbols[index] : NULL;
}

static uint8_t *module_shim_stub_by_name(kb_module_t *module, const char *name)
{
    if (module == NULL || module->shim_symbol_stubs == NULL || name == NULL) {
        return NULL;
    }

    const size_t symbol_count = shim_symbol_count();
    for (size_t i = 0; i < symbol_count; i++) {
        const kb_linux_symbol_t *symbol = shim_symbol_at(i);
        if (symbol != NULL && strcmp(symbol->name, name) == 0) {
            return module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE);
        }
    }
    return NULL;
}

static kb_status_t bind_named_shim_stubs(kb_module_t *module)
{
    module->shim_printk = module_shim_stub_by_name(module, "_printk");
    module->shim_kfree = module_shim_stub_by_name(module, "kfree");
    module->shim_kmalloc = module_shim_stub_by_name(module, "kmalloc");
    module->shim_kzalloc = module_shim_stub_by_name(module, "kzalloc");
    module->shim_kmalloc_trace = module_shim_stub_by_name(module, "kmalloc_trace");
    module->shim_request_threaded_irq = module_shim_stub_by_name(module, "request_threaded_irq");
    module->shim_free_irq = module_shim_stub_by_name(module, "free_irq");
    module->shim_dma_alloc_attrs = module_shim_stub_by_name(module, "dma_alloc_attrs");
    module->shim_dma_free_attrs = module_shim_stub_by_name(module, "dma_free_attrs");
    module->shim_stack_chk_fail = module_shim_stub_by_name(module, "__stack_chk_fail");
    module->shim_pci_register_driver = module_shim_stub_by_name(module, "__pci_register_driver");
    module->shim_pci_unregister_driver = module_shim_stub_by_name(module, "pci_unregister_driver");
    module->shim_pci_enable_device = module_shim_stub_by_name(module, "pci_enable_device");
    module->shim_pci_disable_device = module_shim_stub_by_name(module, "pci_disable_device");
    module->shim_pci_set_master = module_shim_stub_by_name(module, "pci_set_master");
    module->shim_pci_iomap = module_shim_stub_by_name(module, "pci_iomap");
    module->shim_pci_iounmap = module_shim_stub_by_name(module, "pci_iounmap");
    module->shim_ioread32 = module_shim_stub_by_name(module, "ioread32");
    module->shim_iowrite32 = module_shim_stub_by_name(module, "iowrite32");

    if (module->shim_printk == NULL ||
        module->shim_kfree == NULL ||
        module->shim_kmalloc == NULL ||
        module->shim_kzalloc == NULL ||
        module->shim_kmalloc_trace == NULL ||
        module->shim_request_threaded_irq == NULL ||
        module->shim_free_irq == NULL ||
        module->shim_dma_alloc_attrs == NULL ||
        module->shim_dma_free_attrs == NULL ||
        module->shim_stack_chk_fail == NULL ||
        module->shim_pci_register_driver == NULL ||
        module->shim_pci_unregister_driver == NULL ||
        module->shim_pci_enable_device == NULL ||
        module->shim_pci_disable_device == NULL ||
        module->shim_pci_set_master == NULL ||
        module->shim_pci_iomap == NULL ||
        module->shim_pci_iounmap == NULL ||
        module->shim_ioread32 == NULL ||
        module->shim_iowrite32 == NULL)
    {
        return KB_ERR_INVALID;
    }

    return KB_OK;
}

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
    const size_t symbol_count = shim_symbol_count();
    if (module == NULL || module->shim_symbol_stubs == NULL || target == NULL || caller_gs == NULL || callee_gs == NULL) {
        return NULL;
    }
    if (symbol_count + module->external_stub_count >= KB_LOCAL_SHIM_STUB_COUNT) {
        return NULL;
    }
    uint8_t *stub = module->shim_symbol_stubs +
        ((symbol_count + module->external_stub_count) * KB_LOCAL_SHIM_STUB_SIZE);
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

static void write_ret_stub(uint8_t *p)
{
    memset(p, 0x90, KB_LOCAL_SHIM_STUB_SIZE);
    p[0] = 0xc3;
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
    const size_t symbol_count = shim_symbol_count();
    for (size_t i = 0; i < symbol_count; i++) {
        const kb_linux_symbol_t *symbol = shim_symbol_at(i);
        if (symbol != NULL && strcmp(symbol->name, name) == 0) {
            return symbol->address;
        }
    }
    return 0;
}

static int module_prefers_kvm_symbols(const kb_module_t *module)
{
    if (module == NULL || module->module_name == NULL) {
        return 0;
    }
    const char *name = module->module_name;
    return strstr(name, "kvm.ko") != NULL ||
        strstr(name, "kvm-amd.ko") != NULL ||
        strstr(name, "kvm-intel.ko") != NULL ||
        strstr(name, "/kvm/") != NULL ||
        strstr(name, "\\kvm\\") != NULL;
}

static void *lookup_kvm_provider_symbol(const char *name)
{
    if (strcmp(name, "anon_inode_getfd") != 0 &&
        strcmp(name, "anon_inode_getfile") != 0 &&
        strcmp(name, "anon_inode_getfile_fmode") != 0 &&
        strcmp(name, "fd_install") != 0 &&
        strcmp(name, "get_unused_fd_flags") != 0 &&
        strcmp(name, "put_unused_fd") != 0)
    {
        return NULL;
    }
    size_t kvm_count = 0;
    const kb_linux_symbol_t *kvm_symbols = kb_linux_kvm_symbols(&kvm_count);
    for (size_t i = 0; i < kvm_count; i++) {
        if (strcmp(kvm_symbols[i].name, name) == 0) {
            return kvm_symbols[i].address;
        }
    }
    return NULL;
}

static uint8_t *module_kvm_provider_stub_by_name(kb_module_t *module, const char *name)
{
    if (module == NULL || module->shim_symbol_stubs == NULL || name == NULL) {
        return NULL;
    }
    void *provider_symbol = lookup_kvm_provider_symbol(name);
    if (provider_symbol == NULL) {
        return NULL;
    }
    const size_t symbol_count = shim_symbol_count();
    for (size_t i = 0; i < symbol_count; i++) {
        const kb_linux_symbol_t *symbol = shim_symbol_at(i);
        if (symbol != NULL && strcmp(symbol->name, name) == 0 && symbol->address == provider_symbol) {
            return module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE);
        }
    }
    return NULL;
}

static int symbol_name_pointer_is_valid(const char *name)
{
    return !kb_low_or_err_pointer(name);
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

void kb_module_debug_describe_address(const void *address)
{
    const uintptr_t value = (uintptr_t)address;
    for (kb_module_t *module = kb_loaded_modules; module != NULL; module = module->next_loaded) {
        for (size_t i = 0; i < module->section_count; i++) {
            const loaded_section_t *section = &module->sections[i];
            if (section->base == NULL || section->size == 0) {
                continue;
            }
            const uintptr_t begin = (uintptr_t)section->base;
            const uintptr_t end = begin + (uintptr_t)section->size;
            if (value >= begin && value < end) {
                fprintf(stderr,
                    "kobox-loader: address=%p module=%s section_index=%zu section_offset=0x%llx section_flags=0x%llx\n",
                    address,
                    module->module_name == NULL ? "(unnamed)" : module->module_name,
                    i,
                    (unsigned long long)(value - begin),
                    (unsigned long long)section->flags);
                return;
            }
        }
        const uintptr_t shim_begin = (uintptr_t)module->shim_region;
        const uintptr_t shim_end = shim_begin + KB_LOCAL_SHIM_REGION_SIZE;
        if (value >= shim_begin && value < shim_end) {
            fprintf(stderr,
                "kobox-loader: address=%p module=%s shim_offset=0x%llx\n",
                address,
                module->module_name == NULL ? "(unnamed)" : module->module_name,
                (unsigned long long)(value - shim_begin));
            return;
        }
    }
    fprintf(stderr, "kobox-loader: address=%p module=(unknown)\n", address);
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

static void *lookup_module_shim_symbol(kb_module_t *module, const char *name)
{
    if (strcmp(name, "__num_online_cpus") == 0) {
        return module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_NUM_ONLINE_CPUS_OFFSET;
    }
    if (strcmp(name, "pcpu_hot") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_PCPU_HOT_OFFSET;
    }
    if (strcmp(name, "const_current_task") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_PCPU_HOT_OFFSET;
    }
    if (strcmp(name, "__ref_stack_chk_guard") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_STACK_CHK_GUARD_OFFSET;
    }
    if (strcmp(name, "__preempt_count") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_PREEMPT_COUNT_OFFSET;
    }
    if (strcmp(name, "cpu_number") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_CPU_NUMBER_OFFSET;
    }
    if (strcmp(name, "numa_node") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_NUMA_NODE_OFFSET;
    }
    if (strcmp(name, "cpu_info") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_CPU_INFO_OFFSET;
    }
    if (strcmp(name, "page_offset_base") == 0) {
        return module->shim_region + KB_LOCAL_PAGE_OFFSET_BASE_OFFSET;
    }
    if (strcmp(name, "vmemmap_base") == 0) {
        return module->shim_region + KB_LOCAL_VMEMMAP_BASE_OFFSET;
    }
    if (strcmp(name, "phys_base") == 0) {
        return module->shim_region + KB_LOCAL_PHYS_BASE_OFFSET;
    }
    if (module_prefers_kvm_symbols(module)) {
        uint8_t *kvm_stub = module_kvm_provider_stub_by_name(module, name);
        if (kvm_stub != NULL) {
            return kvm_stub;
        }
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
    uint8_t *symbol_stub = module_shim_stub_by_name(module, name);
    if (symbol_stub != NULL) {
        return symbol_stub;
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
    if (strcmp(name, "param_ops_bint") == 0) {
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
    if (strcmp(name, "percpu_counter_batch") == 0) {
        return module->shim_percpu_counter_batch;
    }
    if (strcmp(name, "const_pcpu_hot") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_PCPU_HOT_OFFSET;
    }
    if (strcmp(name, "this_cpu_off") == 0) {
        return module->shim_this_cpu_off;
    }
    if (strcmp(name, "__per_cpu_offset") == 0) {
        return module->shim_per_cpu_offset;
    }
    if (strcmp(name, "__var_waitqueue") == 0) {
        return module->shim_var_waitqueue;
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
        strcmp(name, "cpu_bit_bitmap") == 0 ||
        strcmp(name, "debug_locks_silent") == 0 ||
        strcmp(name, "devmap_managed_key") == 0 ||
        strcmp(name, "dma_ops") == 0 ||
        strcmp(name, "dotdot_name") == 0 ||
        strcmp(name, "efi") == 0 ||
        strcmp(name, "hugetlb_optimize_vmemmap_key") == 0 ||
        strcmp(name, "iomem_resource") == 0 ||
        strcmp(name, "ioport_resource") == 0 ||
        strcmp(name, "init_uts_ns") == 0 ||
        strcmp(name, "init_user_ns") == 0 ||
        strcmp(name, "hv_vp_assist_page") == 0 ||
        strcmp(name, "fs_overflowgid") == 0 ||
        strcmp(name, "fs_overflowuid") == 0 ||
        strcmp(name, "fs_bio_set") == 0 ||
        strcmp(name, "fs_holder_ops") == 0 ||
        strcmp(name, "gdt_page") == 0 ||
        strcmp(name, "__tss_limit_invalid") == 0 ||
        strcmp(name, "fs_kobj") == 0 ||
        strcmp(name, "latent_entropy") == 0 ||
        strcmp(name, "l1tf_mitigation") == 0 ||
        strcmp(name, "kfree_link") == 0 ||
        strcmp(name, "kmalloc_caches") == 0 ||
        strcmp(name, "ms_hyperv") == 0 ||
        strcmp(name, "nop_posix_acl_access") == 0 ||
        strcmp(name, "nop_posix_acl_default") == 0 ||
        strcmp(name, "nop_mnt_idmap") == 0 ||
        strcmp(name, "oops_in_progress") == 0 ||
        strcmp(name, "dquot_quotactl_sysfile_ops") == 0 ||
        strcmp(name, "pci_bus_type") == 0 ||
        strcmp(name, "pci_power_names") == 0 ||
        strcmp(name, "pm_wq") == 0 ||
        strcmp(name, "preempt_model_full") == 0 ||
        strcmp(name, "power_group_name") == 0 ||
        strcmp(name, "screen_info") == 0 ||
        strcmp(name, "sme_me_mask") == 0 ||
        strcmp(name, "system_freezable_wq") == 0 ||
        strcmp(name, "system_long_wq") == 0 ||
        strcmp(name, "system_percpu_wq") == 0 ||
        strcmp(name, "system_state") == 0 ||
        strcmp(name, "system_unbound_wq") == 0 ||
        strcmp(name, "system_power_efficient_wq") == 0 ||
        strcmp(name, "system_wq") == 0 ||
        strcmp(name, "param_ops_string") == 0 ||
        strcmp(name, "param_ops_ushort") == 0 ||
        strcmp(name, "param_ops_ullong") == 0 ||
        strcmp(name, "usb_debug_root") == 0 ||
        strcmp(name, "uuid_null") == 0 ||
           strcmp(name, "pm_suspend_global_flags") == 0 ||
           strcmp(name, "freezer_active") == 0 ||
           strcmp(name, "x86_cpu_to_apicid") == 0 ||
           strcmp(name, "x2apic_mode") == 0 ||
           strcmp(name, "x86_verw_sel") == 0 ||
           strcmp(name, "x86_virt_spec_ctrl") == 0 ||
           strcmp(name, "__x86_call_depth") == 0 ||
           strcmp(name, "irq_stat") == 0 ||
           strcmp(name, "x86_spec_ctrl_current") == 0 ||
           strcmp(name, "sched_smt_present") == 0)
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

static int symbol_is_percpu_offset(const kb_module_t *module, const kb_elf_symbol_t *symbol)
{
    if (module == NULL || symbol == NULL || symbol->section_index >= module->section_count) {
        return 0;
    }
    kb_elf_section_t section;
    if (kb_elf_section(&module->elf, symbol->section_index, &section) != KB_OK) {
        return 0;
    }
    return section.name != NULL && strcmp(section.name, ".data..percpu") == 0;
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

    if (symbol_is_percpu_offset(module, &symbol)) {
        *out_address = symbol.value;
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

            status = kb_loader_symbol_registry_add_export(symbol.name, address, module);
            if (status != KB_OK) {
                return status;
            }
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
    module->shim_percpu_counter_batch = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3432;
    module->shim_const_pcpu_hot = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3436;
    module->shim_this_cpu_off = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3440;
    module->shim_per_cpu_offset = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3568;
    module->shim_var_waitqueue = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3448;
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
    write_u32le((uint8_t *)module->shim_percpu_counter_batch, 32);
    write_u32le((uint8_t *)module->shim_const_pcpu_hot, 0);
    write_u64le((uint8_t *)module->shim_this_cpu_off, 0);
    write_u64le((uint8_t *)module->shim_per_cpu_offset, (uint64_t)(uintptr_t)module->kernel_gs);
    write_u64le((uint8_t *)module->shim_pv_ops + 0x98, (uint64_t)(uintptr_t)&kb_return_zero);
    write_u32le(module->shim_region + KB_LOCAL_USB_DATA_OFFSET + KB_LOCAL_USB_NUM_ONLINE_CPUS_OFFSET, 1);
    write_u64le(
        module->shim_region + KB_LOCAL_PAGE_OFFSET_BASE_OFFSET,
        (uint64_t)kb_linux_kvm_page_offset_base());
    write_u64le(
        module->shim_region + KB_LOCAL_VMEMMAP_BASE_OFFSET,
        (uint64_t)kb_linux_kvm_vmemmap_base());
    write_u64le(
        module->shim_region + KB_LOCAL_PHYS_BASE_OFFSET,
        (uint64_t)kb_linux_kvm_phys_base());

    const size_t symbol_count = shim_symbol_count();
    if (symbol_count > KB_LOCAL_SHIM_STUB_COUNT) {
        return KB_ERR_NOMEM;
    }
    for (size_t i = 0; i < symbol_count; i++) {
        const kb_linux_symbol_t *symbol = shim_symbol_at(i);
        if (symbol == NULL) {
            return KB_ERR_INVALID;
        }
        if (strcmp(symbol->name, "stackleak_track_stack") == 0) {
            write_ret_stub(module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE));
            continue;
        }
        write_abs_jump_stub(
            module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE),
            symbol->address,
            module->kernel_gs,
            module->kernel_gs);
    }
    kb_status_t bind_status = bind_named_shim_stubs(module);
    if (bind_status != KB_OK) {
        return bind_status;
    }
    write_abs_jump_raw_stub(
        module->shim_region + KB_LOCAL_USB_CONTROL_MSG_STUB_OFFSET,
        (void *)(uintptr_t)&kb_usb_control_msg_entry);
    const uint64_t pv_return_zero = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "crc32_le");
    const uint64_t pv_cpuid = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_cpuid");
    const uint64_t pv_read_msr = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_read_msr");
    const uint64_t pv_read_msr_safe = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_read_msr_safe");
    const uint64_t pv_save_flags = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_save_flags_if_enabled");
    const uint64_t pv_write_msr = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_write_msr");
    const uint64_t pv_write_msr_safe = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "kobox_x86_write_msr_safe");
    for (size_t offset = 0; offset + sizeof(uint64_t) <= 512; offset += sizeof(uint64_t)) {
        write_u64le((uint8_t *)module->shim_pv_ops + offset, pv_return_zero);
    }
    write_u64le((uint8_t *)module->shim_pv_ops + 0x94, pv_cpuid);
    write_u64le((uint8_t *)module->shim_pv_ops + 0x9c, pv_read_msr);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xa4, pv_write_msr);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xac, pv_read_msr_safe);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xb4, pv_write_msr_safe);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xd4, pv_return_zero);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xdc, pv_return_zero);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xe4, pv_return_zero);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xf0, pv_save_flags);
    write_u64le(module->kernel_gs + KB_LOCAL_GS_PCPU_HOT_OFFSET, (uint64_t)(uintptr_t)module->shim_current_task);
    write_u64le(module->kernel_gs + KB_LOCAL_GS_STACK_CHK_GUARD_OFFSET, 0x6b6f626f785f7370ull);
    write_u32le(module->kernel_gs + KB_LOCAL_GS_PREEMPT_COUNT_OFFSET, 0);
    write_u32le(module->kernel_gs + KB_LOCAL_GS_CPU_NUMBER_OFFSET, 0);
    write_u32le(module->kernel_gs + KB_LOCAL_GS_NUMA_NODE_OFFSET, 0);
    memset(module->kernel_gs + KB_LOCAL_GS_CPU_INFO_OFFSET, 0, 512);
    memset(module->shim_boot_cpu_data, 0, 512);
    module->kernel_gs[KB_LOCAL_GS_CPU_INFO_OFFSET + 2] = 2; /* X86_VENDOR_AMD */
    ((uint8_t *)module->shim_boot_cpu_data)[2] = 2; /* X86_VENDOR_AMD */
    write_u64le(module->kernel_gs + KB_LOCAL_GS_CPU_INFO_OFFSET + 0x48, 1ull << 2); /* X86_FEATURE_SVM */
    write_u64le((uint8_t *)module->shim_current_task + 0xe8, (uint64_t)(uintptr_t)module->shim_current_mm);
    write_u64le((uint8_t *)module->shim_current_task + 0x938, (uint64_t)(uintptr_t)module->shim_current_mm);
    write_u64le(
        (uint8_t *)module->shim_current_task + 0x950,
        (uint64_t)(uintptr_t)((uint8_t *)module->shim_current_task + 0x9b8));
    write_u32le((uint8_t *)module->shim_current_task + 0x9b8, 1);
    memcpy((uint8_t *)module->shim_current_task + 0xbd8, "kobox-run", sizeof("kobox-run"));
    write_u64le((uint8_t *)module->shim_node_data, (uint64_t)(uintptr_t)module->shim_node0);
    write_u64le((uint8_t *)module->shim_node_states + 0x80, 1);
    write_u32le((uint8_t *)module->shim_boot_cpu_data + 0x30, 0x10000);
    write_u64le((uint8_t *)module->shim_boot_cpu_data + 0x2c, 1ull << 52); /* X86_FEATURE_NX */
    if (trace_kvm_enabled()) {
        uint64_t boot_caps = 0;
        uint64_t cpu_info_caps = 0;
        memcpy(&boot_caps, (uint8_t *)module->shim_boot_cpu_data + 0x2c, sizeof(boot_caps));
        memcpy(&cpu_info_caps, module->kernel_gs + KB_LOCAL_GS_CPU_INFO_OFFSET + 0x48, sizeof(cpu_info_caps));
        fprintf(stderr,
            "kobox-kvm: cpu-image boot_cpu_data[2]=0x%02x boot+0x2c=0x%016llx cpu_info[2]=0x%02x cpu_info+0x48=0x%016llx per_cpu_offset=%p\n",
            (unsigned)((uint8_t *)module->shim_boot_cpu_data)[2],
            (unsigned long long)boot_caps,
            (unsigned)module->kernel_gs[KB_LOCAL_GS_CPU_INFO_OFFSET + 2],
            (unsigned long long)cpu_info_caps,
            module->shim_per_cpu_offset);
    }
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
    const char *backend = getenv("KOBOX_DEVICE_BACKEND");
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
    } else if (strcmp(symbol->name, "__x86_indirect_thunk_rbp") == 0) {
        indirect_thunk_register = 5;
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
        const char *backend = getenv("KOBOX_DEVICE_BACKEND");
        if (backend != NULL && strcmp(backend, "pachaos") == 0) {
            return lookup_module_shim_symbol(module, "hub_port_debounce");
        }
    }
    if (strcmp(name, "kvm_configure_mmu") == 0 ||
        strcmp(name, "kvm_mmu_set_me_spte_mask") == 0 ||
        strcmp(name, "kvm_mmu_set_mmio_spte_mask") == 0)
    {
        return lookup_module_shim_symbol(module, name);
    }
    if (strcmp(name, "__SCT__kvm_x86_vcpu_precreate") == 0) {
        return lookup_module_shim_symbol(module, "__static_call_return0");
    }
    if (strcmp(name, "__SCT__kvm_x86_vcpu_pre_run") == 0) {
        return lookup_module_shim_symbol(module, "__static_call_return1");
    }
    if (strcmp(name, "__SCT__kvm_x86_vcpu_run") == 0) {
        return lookup_module_shim_symbol(module, "__kobox_kvm_vcpu_run_exit");
    }
    if (strcmp(name, "__SCT__kvm_x86_is_valid_cr0") == 0 ||
        strcmp(name, "__SCT__kvm_x86_is_valid_cr4") == 0)
    {
        return lookup_module_shim_symbol(module, "__static_call_return1");
    }
    if (strcmp(name, "kvm_lapic_hv_timer_in_use") == 0) {
        return lookup_module_shim_symbol(module, "__static_call_return0");
    }
    if (strcmp(name, "__kvm_vcpu_update_apicv") == 0) {
        return lookup_module_shim_symbol(module, "rep_stos_alternative");
    }
    if (strcmp(name, "__ext4_fc_track_create") == 0 ||
        strcmp(name, "__ext4_fc_track_link") == 0 ||
        strcmp(name, "__ext4_fc_track_unlink") == 0 ||
        strcmp(name, "ext4_fc_track_create") == 0 ||
        strcmp(name, "ext4_fc_track_link") == 0 ||
        strcmp(name, "ext4_fc_track_unlink") == 0 ||
        strcmp(name, "ext4_fc_track_inode") == 0 ||
        strcmp(name, "ext4_fc_track_range") == 0)
    {
        return lookup_module_shim_symbol(module, "rep_stos_alternative");
    }
    if (strcmp(name, "ext4_load_and_init_journal") == 0) {
        return lookup_module_shim_symbol(module, "rep_stos_alternative");
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
           strcmp(name, "usb_unlink_urb") == 0 ||
           strcmp(name, "kvm_configure_mmu") == 0 ||
           strcmp(name, "kvm_mmu_set_me_spte_mask") == 0 ||
           strcmp(name, "kvm_mmu_set_mmio_spte_mask") == 0 ||
           strcmp(name, "jbd2__journal_restart") == 0 ||
           strcmp(name, "jbd2__journal_start") == 0 ||
           strcmp(name, "jbd2_journal_abort") == 0 ||
           strcmp(name, "jbd2_journal_begin_ordered_truncate") == 0 ||
           strcmp(name, "jbd2_journal_blocks_per_page") == 0 ||
           strcmp(name, "jbd2_journal_check_available_features") == 0 ||
           strcmp(name, "jbd2_journal_clear_err") == 0 ||
           strcmp(name, "jbd2_journal_clear_features") == 0 ||
           strcmp(name, "jbd2_journal_destroy") == 0 ||
           strcmp(name, "jbd2_journal_dirty_metadata") == 0 ||
           strcmp(name, "jbd2_journal_errno") == 0 ||
           strcmp(name, "jbd2_journal_extend") == 0 ||
           strcmp(name, "jbd2_journal_finish_inode_data_buffers") == 0 ||
           strcmp(name, "jbd2_journal_flush") == 0 ||
           strcmp(name, "jbd2_journal_force_commit") == 0 ||
           strcmp(name, "jbd2_journal_force_commit_nested") == 0 ||
           strcmp(name, "jbd2_journal_forget") == 0 ||
           strcmp(name, "jbd2_journal_free_reserved") == 0 ||
           strcmp(name, "jbd2_journal_get_create_access") == 0 ||
           strcmp(name, "jbd2_journal_get_write_access") == 0 ||
           strcmp(name, "jbd2_journal_init_dev") == 0 ||
           strcmp(name, "jbd2_journal_init_inode") == 0 ||
           strcmp(name, "jbd2_journal_init_jbd_inode") == 0 ||
           strcmp(name, "jbd2_journal_inode_ranged_wait") == 0 ||
           strcmp(name, "jbd2_journal_inode_ranged_write") == 0 ||
           strcmp(name, "jbd2_journal_invalidate_folio") == 0 ||
           strcmp(name, "jbd2_journal_load") == 0 ||
           strcmp(name, "jbd2_journal_lock_updates") == 0 ||
           strcmp(name, "jbd2_journal_release_jbd_inode") == 0 ||
           strcmp(name, "jbd2_journal_revoke") == 0 ||
           strcmp(name, "jbd2_journal_set_features") == 0 ||
           strcmp(name, "jbd2_journal_set_triggers") == 0 ||
           strcmp(name, "jbd2_journal_start") == 0 ||
           strcmp(name, "jbd2_journal_start_commit") == 0 ||
           strcmp(name, "jbd2_journal_start_reserved") == 0 ||
           strcmp(name, "jbd2_journal_stop") == 0 ||
           strcmp(name, "jbd2_journal_try_to_free_buffers") == 0 ||
           strcmp(name, "jbd2_journal_unlock_updates") == 0 ||
           strcmp(name, "jbd2_journal_update_sb_errno") == 0 ||
           strcmp(name, "jbd2_journal_wipe") == 0;
}

static int relocation_is_direct_call(const uint8_t *target)
{
    return target != 0 && target[-1] == 0xe8;
}

static int relocation_is_relative_branch(const uint8_t *target)
{
    if (target == NULL) {
        return 0;
    }
    if (target[-1] == 0xe8 || target[-1] == 0xe9) {
        return 1;
    }
    return target[-2] == 0x0f && target[-1] >= 0x80 && target[-1] <= 0x8f;
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
            address = kb_loader_symbol_registry_lookup_export(symbol.name);
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
        if (trace_kvm_relocations_enabled() &&
            (strcmp(symbol.name, "boot_cpu_data") == 0 ||
             strcmp(symbol.name, "cpu_info") == 0 ||
             strcmp(symbol.name, "__per_cpu_offset") == 0 ||
             strcmp(symbol.name, "cpu_number") == 0 ||
             strcmp(symbol.name, "page_offset_base") == 0 ||
             strcmp(symbol.name, "vmemmap_base") == 0 ||
             strcmp(symbol.name, "phys_base") == 0))
        {
            uint64_t probe = 0;
            if ((uintptr_t)address > 4096 && strcmp(symbol.name, "boot_cpu_data") == 0) {
                memcpy(&probe, (uint8_t *)address + 0x2c, sizeof(probe));
            } else if ((uintptr_t)address > 4096 && strcmp(symbol.name, "cpu_info") == 0) {
                memcpy(&probe, (uint8_t *)address + 0x48, sizeof(probe));
            } else if ((uintptr_t)address > 4096 && strcmp(symbol.name, "__per_cpu_offset") == 0) {
                memcpy(&probe, address, sizeof(probe));
            } else if ((uintptr_t)address > 4096 &&
                (strcmp(symbol.name, "page_offset_base") == 0 ||
                 strcmp(symbol.name, "vmemmap_base") == 0 ||
                 strcmp(symbol.name, "phys_base") == 0))
            {
                memcpy(&probe, address, sizeof(probe));
            }
            fprintf(
                stderr,
                "kobox-kvm: relocation symbol=%s type=%u offset=0x%llx addend=0x%llx address=%p exported=%d probe=0x%016llx\n",
                symbol.name,
                (unsigned)relocation->type,
                (unsigned long long)relocation->offset,
                (unsigned long long)(relocation->has_addend ? relocation->addend : 0),
                address,
                address_from_exported_module,
                (unsigned long long)probe);
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
        if (symbol_is_percpu_offset(module, &symbol)) {
            symbol_address = symbol.value;
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
        const int64_t pc_base =
            relocation_is_relative_branch(target) || addend == -4 ? (int64_t)place : (int64_t)place + 4;
        if (!value_fits_i32(value - pc_base)) {
            return KB_ERR_UNSUPPORTED;
        }
        write_u32le(target, (uint32_t)(value - pc_base));
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
                if (trace_modules_enabled() || trace_kvm_relocations_enabled()) {
                    kb_elf_symbol_t failed_symbol;
                    memset(&failed_symbol, 0, sizeof(failed_symbol));
                    (void)kb_elf_symbol(
                        &module->elf,
                        relocation.symbol_table_section_index,
                        relocation.symbol_index,
                        &failed_symbol);
                    fprintf(
                        stderr,
                        "kobox-loader: relocation failed status=%d module=%s symbol=%s type=%u offset=0x%llx addend=0x%llx\n",
                        (int)status,
                        module->module_name == NULL ? "(unnamed)" : module->module_name,
                        failed_symbol.name == NULL ? "(unknown)" : failed_symbol.name,
                        (unsigned)relocation.type,
                        (unsigned long long)relocation.offset,
                        (unsigned long long)(relocation.has_addend ? relocation.addend : 0));
                }
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

static kb_status_t patch_module_ext4_journal_boundary(kb_module_t *module)
{
    if (module == NULL || module->module_name == NULL || strstr(module->module_name, "ext4.ko") == NULL) {
        return KB_OK;
    }

    uint64_t address = 0;
    kb_status_t status = find_symbol_address(module, "ext4_load_and_init_journal", &address);
    if (status == KB_ERR_NOT_FOUND) {
        return KB_OK;
    }
    if (status != KB_OK) {
        return status;
    }
    uint8_t *code = (uint8_t *)(uintptr_t)address;
    if (!module_contains_executable_address(module, (uintptr_t)code)) {
        return KB_ERR_UNSUPPORTED;
    }

    code[0] = 0x31;
    code[1] = 0xc0;
    code[2] = 0xc3;
    code[3] = 0x90;
    code[4] = 0x90;
    if (trace_modules_enabled()) {
        fprintf(
            stderr,
            "kobox-loader: patched ext4 journal boundary module=%s symbol=ext4_load_and_init_journal addr=%p\n",
            module->module_name,
            (void *)code);
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
    kb_loader_symbol_registry_remove_owner(module);
    free_loaded_sections(module);
    free(module->shim_node0);
    free(module->module_name);
    free(module->sections);
    free(module);
}

kb_status_t kb_loader_enter_module_context(kb_module_t *module, unsigned long *out_old_gs)
{
    if (module == 0 || out_old_gs == 0) {
        return KB_ERR_INVALID;
    }
    kb_shim_set_device_backend(module->backend);
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
        kb_shim_set_device_backend(0);
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
        kb_shim_set_device_backend(0);
        return KB_ERR_UNSUPPORTED;
    }
#else
    *out_old_gs = 0;
#endif
    return KB_OK;
}

void kb_loader_leave_module_context(unsigned long old_gs)
{
#if !defined(_WIN32) && defined(__x86_64__)
    (void)syscall(SYS_arch_prctl, ARCH_SET_GS, old_gs);
#else
    (void)old_gs;
#endif
    kb_shim_set_device_backend(0);
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
    status = patch_module_ext4_journal_boundary(module);
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
    kb_device_backend_t *backend,
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
    kb_status_t status = kb_loader_enter_module_context(module, &old_gs);
    if (status != KB_OK) {
        return status;
    }
    kb_loader_set_active_module(module);
    *out_result = module->init_module();
    kb_loader_leave_module_context(old_gs);
    kb_loader_set_active_module(NULL);
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
    kb_status_t status = kb_loader_enter_module_context(module, &old_gs);
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
    kb_loader_set_active_module(module);
    module->cleanup_module();
    kb_loader_leave_module_context(old_gs);
    kb_loader_set_active_module(NULL);
    return KB_OK;
}

kb_status_t kb_module_find_symbol(kb_module_t *module, const char *name, void **out_address)
{
    if (module == NULL || name == NULL || out_address == NULL) {
        return KB_ERR_INVALID;
    }
    *out_address = NULL;
    uint64_t address = 0;
    kb_status_t status = find_symbol_address(module, name, &address);
    if (status != KB_OK) {
        return status;
    }
    *out_address = (void *)(uintptr_t)address;
    return KB_OK;
}

void kb_module_close(kb_module_t *module)
{
    destroy_module(module);
}
