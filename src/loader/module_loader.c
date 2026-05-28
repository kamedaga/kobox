#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/elf.h"
#include "kobox/module.h"
#include "kobox/shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
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
} loaded_section_t;

enum {
    KB_LOCAL_SHIM_STUB_SIZE = 48,
    KB_LOCAL_SHIM_STUB_COUNT = 128,
    KB_LOCAL_SHIM_DATA_SIZE = 4096,
    KB_LOCAL_SHIM_REGION_SIZE = (KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT) + KB_LOCAL_SHIM_DATA_SIZE,
    KB_LOCAL_SHIM_DATA_OFFSET = KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT,
};

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
    void *shim_cpu_possible_mask;
    void *shim_nr_cpu_ids;
    void *shim_this_cpu_off;
    void *shim_pernet_ops_rwsem;
    void *shim_pvpanic_dev_groups;
    loaded_section_t *sections;
    size_t section_count;
    int (*init_module)(void);
    void (*cleanup_module)(void);
#if !defined(_WIN32) && defined(__x86_64__)
    uint8_t kernel_gs[64];
#endif
};

typedef struct shim_symbol {
    const char *name;
    void *address;
} shim_symbol_t;

static void kb_noop(void)
{
}

static const shim_symbol_t shim_symbols[] = {
    {"__fentry__", (void *)(uintptr_t)&kb_noop},
    {"__x86_return_thunk", (void *)(uintptr_t)&kb_noop},
    {"_printk", (void *)(uintptr_t)&kb_printk},
    {"printk", (void *)(uintptr_t)&kb_printk},
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
    {"ioread32", (void *)(uintptr_t)&kb_ioread32},
    {"iowrite32", (void *)(uintptr_t)&kb_iowrite32},
    {"__platform_driver_register", (void *)(uintptr_t)&kb_platform_driver_register},
    {"platform_driver_unregister", (void *)(uintptr_t)&kb_platform_driver_unregister},
    {"__devm_add_action", (void *)(uintptr_t)&kb_devm_add_action},
    {"__devm_uio_register_device", (void *)(uintptr_t)&kb_devm_uio_register_device},
    {"__dynamic_dev_dbg", (void *)(uintptr_t)&kb_dynamic_dev_dbg},
    {"_dev_err", (void *)(uintptr_t)&kb_dev_err},
    {"_dev_warn", (void *)(uintptr_t)&kb_dev_warn},
    {"pm_runtime_enable", (void *)(uintptr_t)&kb_pm_runtime_enable},
    {"__pm_runtime_disable", (void *)(uintptr_t)&kb_pm_runtime_disable},
    {"__pm_runtime_idle", (void *)(uintptr_t)&kb_pm_runtime_idle},
    {"__pm_runtime_resume", (void *)(uintptr_t)&kb_pm_runtime_resume},
    {"devm_kmalloc", (void *)(uintptr_t)&kb_devm_kmalloc},
    {"devm_kasprintf", (void *)(uintptr_t)&kb_devm_kasprintf},
    {"platform_get_irq_optional", (void *)(uintptr_t)&kb_platform_get_irq_optional},
    {"disable_irq_nosync", (void *)(uintptr_t)&kb_disable_irq_nosync},
    {"enable_irq", (void *)(uintptr_t)&kb_enable_irq},
    {"irq_get_irq_data", (void *)(uintptr_t)&kb_irq_get_irq_data},
    {"irq_modify_status", (void *)(uintptr_t)&kb_irq_modify_status},
    {"_raw_spin_lock", (void *)(uintptr_t)&kb_raw_spin_lock},
    {"_raw_spin_lock_irqsave", (void *)(uintptr_t)&kb_raw_spin_lock_irqsave},
    {"_raw_spin_unlock", (void *)(uintptr_t)&kb_raw_spin_unlock},
    {"_raw_spin_unlock_irqrestore", (void *)(uintptr_t)&kb_raw_spin_unlock_irqrestore},
    {"__kmalloc", (void *)(uintptr_t)&kb_kmalloc_alias},
    {"__SCT__might_resched", (void *)(uintptr_t)&kb_might_resched},
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
    {"input_set_abs_params", (void *)(uintptr_t)&kb_input_set_abs_params},
    {"input_alloc_absinfo", (void *)(uintptr_t)&kb_input_alloc_absinfo},
    {"input_mt_init_slots", (void *)(uintptr_t)&kb_input_mt_init_slots},
    {"snprintf", (void *)(uintptr_t)&snprintf},
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
    {"devm_pvpanic_probe", (void *)(uintptr_t)&kb_devm_pvpanic_probe},
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

static void write_abs_jump_stub(uint8_t *p, void *target)
{
    memset(p, 0x90, KB_LOCAL_SHIM_STUB_SIZE);
    p[0] = 0x49;
    p[1] = 0x89;
    p[2] = 0xe3;
    p[3] = 0x48;
    p[4] = 0x83;
    p[5] = 0xe4;
    p[6] = 0xf0;
    p[7] = 0x48;
    p[8] = 0x83;
    p[9] = 0xec;
    p[10] = 0x10;
    p[11] = 0x4c;
    p[12] = 0x89;
    p[13] = 0x5c;
    p[14] = 0x24;
    p[15] = 0x08;
    p[16] = 0x48;
    p[17] = 0xb8;
    write_u64le(p + 18, (uint64_t)(uintptr_t)target);
    p[26] = 0xff;
    p[27] = 0xd0;
    p[28] = 0x48;
    p[29] = 0x8b;
    p[30] = 0x64;
    p[31] = 0x24;
    p[32] = 0x08;
    p[33] = 0xc3;
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

static void *lookup_module_shim_symbol(kb_module_t *module, const char *name)
{
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
    if (strcmp(name, "__cpu_possible_mask") == 0) {
        return module->shim_cpu_possible_mask;
    }
    if (strcmp(name, "nr_cpu_ids") == 0) {
        return module->shim_nr_cpu_ids;
    }
    if (strcmp(name, "this_cpu_off") == 0) {
        return module->shim_this_cpu_off;
    }
    if (strcmp(name, "pernet_ops_rwsem") == 0) {
        return module->shim_pernet_ops_rwsem;
    }
    if (strcmp(name, "pvpanic_dev_groups") == 0) {
        return module->shim_pvpanic_dev_groups;
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
    module->shim_cpu_possible_mask = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3264;
    module->shim_nr_cpu_ids = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3280;
    module->shim_this_cpu_off = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3296;
    module->shim_pernet_ops_rwsem = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3312;
    module->shim_pvpanic_dev_groups = module->shim_region + KB_LOCAL_SHIM_DATA_OFFSET + 3376;
    write_u64le((uint8_t *)module->shim_cpu_possible_mask, 1);
    write_u32le((uint8_t *)module->shim_nr_cpu_ids, 1);
    write_u64le((uint8_t *)module->shim_this_cpu_off, 0);

    for (size_t i = 0; i < sizeof(shim_symbols) / sizeof(shim_symbols[0]); i++) {
        write_abs_jump_stub(
            module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE),
            shim_symbols[i].address);
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

        void *address = lookup_module_shim_symbol(module, symbol.name);
        if (address == 0) {
            return KB_ERR_UNSUPPORTED;
        }
        symbol_address = (uint64_t)(uintptr_t)address;
    } else {
        status = loaded_section_address(module, symbol.section_index, symbol.value, &symbol_address);
        if (status != KB_OK) {
            return status;
        }
    }

    const int64_t addend = relocation->has_addend ? relocation->addend : 0;
    const uint64_t place = (uint64_t)(uintptr_t)target;
    const int64_t value = (int64_t)symbol_address + addend;
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
            return KB_ERR_UNSUPPORTED;
        }
        write_u32le(target, (uint32_t)value);
        return KB_OK;
    default:
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
    free_loaded_sections(module);
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
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, out_old_gs) != 0) {
        kb_shim_set_backend(0);
        return KB_ERR_UNSUPPORTED;
    }
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)(uintptr_t)module->kernel_gs) != 0) {
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

    uint64_t init_address = 0;
    status = find_symbol_address(module, "init_module", &init_address);
    if (status != KB_OK) {
        return status;
    }
    module->init_module = (int (*)(void))(uintptr_t)init_address;

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

    *out_module = module;
    return KB_OK;
}

kb_status_t kb_module_call_init(kb_module_t *module, int *out_result)
{
    if (module == 0 || module->init_module == 0 || out_result == 0) {
        return KB_ERR_INVALID;
    }
    unsigned long old_gs = 0;
    kb_status_t status = enter_module_context(module, &old_gs);
    if (status != KB_OK) {
        return status;
    }
    *out_result = module->init_module();
    leave_module_context(old_gs);
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
        return status;
    }
    module->cleanup_module();
    leave_module_context(old_gs);
    return KB_OK;
}

void kb_module_close(kb_module_t *module)
{
    destroy_module(module);
}
