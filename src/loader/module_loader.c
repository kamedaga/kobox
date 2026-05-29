#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/elf.h"
#include "kobox/module.h"
#include "kobox/shim.h"

#include <ctype.h>
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
    KB_LOCAL_SHIM_STUB_COUNT = 1024,
    KB_LOCAL_SHIM_DATA_SIZE = 4096,
    KB_LOCAL_SHIM_REGION_SIZE = (KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT) + KB_LOCAL_SHIM_DATA_SIZE,
    KB_LOCAL_SHIM_DATA_OFFSET = KB_LOCAL_SHIM_STUB_SIZE * KB_LOCAL_SHIM_STUB_COUNT,
    KB_LOCAL_GS_SIZE = 4096,
    KB_LOCAL_GS_PCPU_HOT_OFFSET = 0x100,
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
    loaded_section_t *sections;
    size_t section_count;
    int (*init_module)(void);
    void (*cleanup_module)(void);
#if !defined(_WIN32) && defined(__x86_64__)
    uint8_t kernel_gs[KB_LOCAL_GS_SIZE];
#endif
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
};

static exported_symbol_t exported_symbols[KB_EXPORTED_SYMBOL_MAX];

static void kb_noop(void)
{
}

static int kb_ascii_strcasecmp(const char *a, const char *b)
{
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

static char *kb_ascii_strsep(char **stringp, const char *delim)
{
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

static int kb_vprintk(const char *fmt, va_list args)
{
    return vprintf(fmt, args);
}

static int kb_chrdev_major_stub(void)
{
    return 240;
}

static uint32_t kb_encode_dev(unsigned major, unsigned minor)
{
    return (minor & 0xffu) | (major << 8) | ((minor & ~0xffu) << 12);
}

static int kb_alloc_chrdev_region_stub(uint32_t *dev, unsigned baseminor, unsigned count, const char *name)
{
    (void)count;
    (void)name;
    if (dev == NULL) {
        return -22;
    }
    *dev = kb_encode_dev(240, baseminor);
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

static unsigned char kb_tracepoint_disabled[128];

static const shim_symbol_t shim_symbols[] = {
    {"__fentry__", (void *)(uintptr_t)&kb_noop},
    {"__x86_return_thunk", (void *)(uintptr_t)&kb_noop},
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
    {"ioread8", (void *)(uintptr_t)&kb_ioread8},
    {"iowrite8", (void *)(uintptr_t)&kb_iowrite8},
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
    {"pm_vt_switch_register", (void *)(uintptr_t)&kb_return_zero},
    {"pm_vt_switch_required", (void *)(uintptr_t)&kb_return_zero},
    {"pm_vt_switch_unregister", (void *)(uintptr_t)&kb_noop},
    {"devm_kmalloc", (void *)(uintptr_t)&kb_devm_kmalloc},
    {"devm_kasprintf", (void *)(uintptr_t)&kb_devm_kasprintf},
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
    {"input_set_abs_params", (void *)(uintptr_t)&kb_input_set_abs_params},
    {"input_alloc_absinfo", (void *)(uintptr_t)&kb_input_alloc_absinfo},
    {"input_mt_init_slots", (void *)(uintptr_t)&kb_input_mt_init_slots},
    {"snprintf", (void *)(uintptr_t)&snprintf},
    {"vsnprintf", (void *)(uintptr_t)&vsnprintf},
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
    {"__do_once_done", (void *)(uintptr_t)&kb_noop},
    {"__do_once_start", (void *)(uintptr_t)&kb_return_zero},
    {"__dynamic_pr_debug", (void *)(uintptr_t)&kb_dynamic_dev_dbg},
    {"__flush_workqueue", (void *)(uintptr_t)&kb_noop},
    {"__init_swait_queue_head", (void *)(uintptr_t)&kb_init_swait_queue_head},
    {"__init_waitqueue_head", (void *)(uintptr_t)&kb_init_waitqueue_head},
    {"__init_rwsem", (void *)(uintptr_t)&kb_noop},
    {"__kmalloc_node", (void *)(uintptr_t)&kb_kmalloc_node},
    {"__msecs_to_jiffies", (void *)(uintptr_t)&kb_return_zero},
    {"__ndelay", (void *)(uintptr_t)&kb_noop},
    {"__mmap_lock_do_trace_acquire_returned", (void *)(uintptr_t)&kb_noop},
    {"__mmap_lock_do_trace_released", (void *)(uintptr_t)&kb_noop},
    {"__mmap_lock_do_trace_start_locking", (void *)(uintptr_t)&kb_noop},
    {"__mutex_init", (void *)(uintptr_t)&kb_mutex_init},
    {"__ubsan_handle_shift_out_of_bounds", (void *)(uintptr_t)&kb_noop},
    {"__warn_printk", (void *)(uintptr_t)&kb_printk},
    {"_dev_info", (void *)(uintptr_t)&kb_dev_warn},
    {"_raw_spin_lock_irq", (void *)(uintptr_t)&kb_raw_spin_lock},
    {"_raw_spin_unlock_irq", (void *)(uintptr_t)&kb_raw_spin_unlock},
    {"base64_decode", (void *)(uintptr_t)&kb_return_zero},
    {"complete", (void *)(uintptr_t)&kb_complete},
    {"complete_all", (void *)(uintptr_t)&kb_noop},
    {"console_lock", (void *)(uintptr_t)&kb_noop},
    {"console_unlock", (void *)(uintptr_t)&kb_noop},
    {"cpufreq_get", (void *)(uintptr_t)&kb_return_zero},
    {"crc32_le", (void *)(uintptr_t)&kb_return_zero},
    {"cachemode2protval", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_page_attrs", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_resource", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_sgtable", (void *)(uintptr_t)&kb_return_zero},
    {"dma_map_sg_attrs", (void *)(uintptr_t)&kb_return_zero},
    {"dma_opt_mapping_size", (void *)(uintptr_t)&kb_return_zero},
    {"dma_pci_p2pdma_supported", (void *)(uintptr_t)&kb_return_zero},
    {"dma_pool_alloc", (void *)(uintptr_t)&kb_dma_pool_alloc},
    {"dma_pool_create", (void *)(uintptr_t)&kb_dma_pool_create},
    {"dma_pool_destroy", (void *)(uintptr_t)&kb_dma_pool_destroy},
    {"dma_pool_free", (void *)(uintptr_t)&kb_dma_pool_free},
    {"dma_set_coherent_mask", (void *)(uintptr_t)&kb_dma_set_coherent_mask},
    {"dma_set_mask", (void *)(uintptr_t)&kb_dma_set_mask},
    {"dma_unmap_page_attrs", (void *)(uintptr_t)&kb_noop},
    {"dma_unmap_resource", (void *)(uintptr_t)&kb_noop},
    {"dma_unmap_sg_attrs", (void *)(uintptr_t)&kb_noop},
    {"dma_sync_single_for_cpu", (void *)(uintptr_t)&kb_noop},
    {"dma_sync_single_for_device", (void *)(uintptr_t)&kb_noop},
    {"dump_stack", (void *)(uintptr_t)&kb_noop},
    {"down", (void *)(uintptr_t)&kb_noop},
    {"down_interruptible", (void *)(uintptr_t)&kb_return_zero},
    {"down_read", (void *)(uintptr_t)&kb_noop},
    {"down_read_trylock", (void *)(uintptr_t)&kb_return_one},
    {"down_trylock", (void *)(uintptr_t)&kb_return_zero},
    {"down_write_trylock", (void *)(uintptr_t)&kb_return_one},
    {"flush_work", (void *)(uintptr_t)&kb_return_zero},
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
    {"mutex_trylock", (void *)(uintptr_t)&kb_mutex_trylock},
    {"mutex_unlock", (void *)(uintptr_t)&kb_mutex_unlock},
    {"pci_alloc_irq_vectors", (void *)(uintptr_t)&kb_pci_alloc_irq_vectors},
    {"pci_alloc_irq_vectors_affinity", (void *)(uintptr_t)&kb_pci_alloc_irq_vectors_affinity},
    {"pci_device_is_present", (void *)(uintptr_t)&kb_pci_device_is_present},
    {"pci_enable_device_mem", (void *)(uintptr_t)&kb_pci_enable_device_mem},
    {"pci_free_irq", (void *)(uintptr_t)&kb_pci_free_irq},
    {"pci_free_irq_vectors", (void *)(uintptr_t)&kb_pci_free_irq_vectors},
    {"pci_irq_vector", (void *)(uintptr_t)&kb_pci_irq_vector},
    {"pci_read_config_word", (void *)(uintptr_t)&kb_return_zero},
    {"pci_release_selected_regions", (void *)(uintptr_t)&kb_pci_release_selected_regions},
    {"pci_request_irq", (void *)(uintptr_t)&kb_pci_request_irq},
    {"pci_request_selected_regions", (void *)(uintptr_t)&kb_pci_request_selected_regions},
    {"pci_restore_state", (void *)(uintptr_t)&kb_return_zero},
    {"pci_save_state", (void *)(uintptr_t)&kb_return_zero},
    {"pci_select_bars", (void *)(uintptr_t)&kb_pci_select_bars},
    {"pcie_aspm_enabled", (void *)(uintptr_t)&kb_return_zero},
    {"pcie_reset_flr", (void *)(uintptr_t)&kb_return_zero},
    {"sg_init_table", (void *)(uintptr_t)&kb_sg_init_one},
    {"sg_alloc_table_from_pages_segment", (void *)(uintptr_t)&kb_return_zero},
    {"sg_free_table", (void *)(uintptr_t)&kb_noop},
    {"sg_next", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_streq", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_update_group", (void *)(uintptr_t)&kb_return_zero},
    {"wait_for_completion", (void *)(uintptr_t)&kb_wait_for_completion},
    {"wait_for_completion_interruptible", (void *)(uintptr_t)&kb_wait_for_completion},
    {"wait_for_completion_io_timeout", (void *)(uintptr_t)&kb_wait_for_completion_io_timeout},
    {"strlen", (void *)(uintptr_t)&strlen},
    {"strchr", (void *)(uintptr_t)&strchr},
    {"strcpy", (void *)(uintptr_t)&strcpy},
    {"strcasecmp", (void *)(uintptr_t)&kb_ascii_strcasecmp},
    {"strlcat", (void *)(uintptr_t)&kb_ascii_strlcat},
    {"strncat", (void *)(uintptr_t)&strncat},
    {"strncasecmp", (void *)(uintptr_t)&kb_ascii_strncasecmp},
    {"strncmp", (void *)(uintptr_t)&strncmp},
    {"strncpy", (void *)(uintptr_t)&strncpy},
    {"strsep", (void *)(uintptr_t)&kb_ascii_strsep},
    {"strstr", (void *)(uintptr_t)&strstr},
    {"strnlen", (void *)(uintptr_t)&strnlen},
    {"strrchr", (void *)(uintptr_t)&strrchr},
    {"sscanf", (void *)(uintptr_t)&sscanf},
    {"memset", (void *)(uintptr_t)&memset},
    {"memcpy", (void *)(uintptr_t)&memcpy},
    {"memcmp", (void *)(uintptr_t)&memcmp},
    {"strcmp", (void *)(uintptr_t)&strcmp},
    {"sprintf", (void *)(uintptr_t)&sprintf},
    {"__SCK__tp_func_block_bio_complete", (void *)(uintptr_t)&kb_noop},
    {"__SCK__tp_func_block_bio_remap", (void *)(uintptr_t)&kb_noop},
    {"__SCK__tp_func_nvme_sq", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_block_bio_complete", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_block_bio_remap", (void *)(uintptr_t)&kb_noop},
    {"__SCT__tp_func_nvme_sq", (void *)(uintptr_t)&kb_noop},
    {"__blk_alloc_disk", (void *)(uintptr_t)&kb_alloc_stub},
    {"__blk_mq_alloc_disk", (void *)(uintptr_t)&kb_alloc_stub},
    {"__blk_rq_map_sg", (void *)(uintptr_t)&kb_return_zero},
    {"__alloc_pages", (void *)(uintptr_t)&kb_alloc_stub},
    {"__check_object_size", (void *)(uintptr_t)&kb_noop},
    {"__const_udelay", (void *)(uintptr_t)&kb_noop},
    {"__folio_put", (void *)(uintptr_t)&kb_noop},
    {"__free_pages", (void *)(uintptr_t)&kb_noop},
    {"__get_free_pages", (void *)(uintptr_t)&kb_alloc_stub},
    {"__io_uring_cmd_do_in_task", (void *)(uintptr_t)&kb_return_zero},
    {"__node_distance", (void *)(uintptr_t)&kb_return_zero},
    {"__put_user_4", (void *)(uintptr_t)&kb_return_zero},
    {"__put_user_8", (void *)(uintptr_t)&kb_return_zero},
    {"__put_devmap_managed_page_refs", (void *)(uintptr_t)&kb_noop},
    {"__printk_ratelimit", (void *)(uintptr_t)&kb_return_one},
    {"__udelay", (void *)(uintptr_t)&kb_noop},
    {"__usecs_to_jiffies", (void *)(uintptr_t)&kb_return_zero},
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
    {"__register_chrdev", (void *)(uintptr_t)&kb_chrdev_major_stub},
    {"__unregister_chrdev", (void *)(uintptr_t)&kb_noop},
    {"__virt_addr_valid", (void *)(uintptr_t)&kb_return_one},
    {"__vmalloc", (void *)(uintptr_t)&kb_kzalloc},
    {"__wake_up", (void *)(uintptr_t)&kb_noop},
    {"_copy_from_user", (void *)(uintptr_t)&kb_return_zero},
    {"_copy_to_user", (void *)(uintptr_t)&kb_return_zero},
    {"_find_first_bit", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_storage_d3", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_evaluate_integer", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_evaluate_object", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_get_handle", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_get_next_object", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_install_notify_handler", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_remove_notify_handler", (void *)(uintptr_t)&kb_return_zero},
    {"acpi_walk_namespace", (void *)(uintptr_t)&kb_return_zero},
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
    {"blk_mark_disk_dead", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_alloc_request", (void *)(uintptr_t)&kb_blk_mq_alloc_request},
    {"blk_mq_alloc_request_hctx", (void *)(uintptr_t)&kb_blk_mq_alloc_request},
    {"blk_mq_alloc_tag_set", (void *)(uintptr_t)&kb_return_zero},
    {"blk_mq_complete_request", (void *)(uintptr_t)&kb_blk_mq_complete_request},
    {"blk_mq_complete_request_remote", (void *)(uintptr_t)&kb_blk_mq_complete_request_remote},
    {"blk_mq_delay_kick_requeue_list", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_destroy_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_end_request", (void *)(uintptr_t)&kb_blk_mq_end_request},
    {"blk_mq_end_request_batch", (void *)(uintptr_t)&kb_blk_mq_end_request_batch},
    {"blk_mq_free_request", (void *)(uintptr_t)&kb_blk_mq_free_request},
    {"blk_mq_free_tag_set", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_freeze_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_freeze_queue_wait", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_freeze_queue_wait_timeout", (void *)(uintptr_t)&kb_return_zero},
    {"blk_mq_init_queue", (void *)(uintptr_t)&kb_blk_mq_init_queue},
    {"blk_mq_map_queues", (void *)(uintptr_t)&kb_return_zero},
    {"blk_mq_pci_map_queues", (void *)(uintptr_t)&kb_return_zero},
    {"blk_mq_quiesce_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_quiesce_tagset", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_requeue_request", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_start_request", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_tagset_busy_iter", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_tagset_wait_completed_request", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_unfreeze_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_unquiesce_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_unquiesce_tagset", (void *)(uintptr_t)&kb_noop},
    {"blk_mq_update_nr_hw_queues", (void *)(uintptr_t)&kb_return_zero},
    {"blk_mq_wait_quiesce_done", (void *)(uintptr_t)&kb_noop},
    {"blk_op_str", (void *)(uintptr_t)&kb_empty_string},
    {"blk_put_queue", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_chunk_sectors", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_dma_alignment", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_flag_set", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_io_min", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_io_opt", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_logical_block_size", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_max_discard_sectors", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_max_discard_segments", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_max_hw_sectors", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_max_segments", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_max_write_zeroes_sectors", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_max_zone_append_sectors", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_physical_block_size", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_virt_boundary", (void *)(uintptr_t)&kb_noop},
    {"blk_queue_write_cache", (void *)(uintptr_t)&kb_noop},
    {"blk_revalidate_disk_zones", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_is_poll", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_map_kern", (void *)(uintptr_t)&kb_blk_rq_map_kern},
    {"blk_rq_map_user_io", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_map_user_iov", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_poll", (void *)(uintptr_t)&kb_return_zero},
    {"blk_rq_unmap_user", (void *)(uintptr_t)&kb_noop},
    {"blk_set_stacking_limits", (void *)(uintptr_t)&kb_noop},
    {"blk_stack_limits", (void *)(uintptr_t)&kb_return_zero},
    {"blk_status_to_errno", (void *)(uintptr_t)&kb_return_zero},
    {"blk_steal_bios", (void *)(uintptr_t)&kb_noop},
    {"blk_sync_queue", (void *)(uintptr_t)&kb_noop},
    {"blkdev_compat_ptr_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"bpf_trace_run1", (void *)(uintptr_t)&kb_noop},
    {"bpf_trace_run2", (void *)(uintptr_t)&kb_noop},
    {"bpf_trace_run3", (void *)(uintptr_t)&kb_noop},
    {"cancel_delayed_work_sync", (void *)(uintptr_t)&kb_return_zero},
    {"cancel_work_sync", (void *)(uintptr_t)&kb_return_zero},
    {"capable", (void *)(uintptr_t)&kb_return_zero},
    {"cdev_device_add", (void *)(uintptr_t)&kb_return_zero},
    {"cdev_device_del", (void *)(uintptr_t)&kb_noop},
    {"cdev_add", (void *)(uintptr_t)&kb_return_zero},
    {"cdev_del", (void *)(uintptr_t)&kb_noop},
    {"cdev_init", (void *)(uintptr_t)&kb_noop},
    {"class_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"class_destroy", (void *)(uintptr_t)&kb_noop},
    {"cleanup_srcu_struct", (void *)(uintptr_t)&kb_noop},
    {"close_fd", (void *)(uintptr_t)&kb_noop},
    {"compat_ptr_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_alloc_kpp", (void *)(uintptr_t)&kb_alloc_stub},
    {"crypto_alloc_shash", (void *)(uintptr_t)&kb_alloc_stub},
    {"crypto_destroy_tfm", (void *)(uintptr_t)&kb_noop},
    {"crypto_req_done", (void *)(uintptr_t)&kb_noop},
    {"crypto_shash_final", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_shash_setkey", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_shash_tfm_digest", (void *)(uintptr_t)&kb_return_zero},
    {"crypto_shash_update", (void *)(uintptr_t)&kb_return_zero},
    {"del_gendisk", (void *)(uintptr_t)&kb_noop},
    {"delayed_work_timer_fn", (void *)(uintptr_t)&kb_noop},
    {"destroy_workqueue", (void *)(uintptr_t)&kb_noop},
    {"dev_driver_string", (void *)(uintptr_t)&kb_empty_string},
    {"dev_pm_qos_expose_latency_tolerance", (void *)(uintptr_t)&kb_return_zero},
    {"dev_pm_qos_hide_latency_tolerance", (void *)(uintptr_t)&kb_noop},
    {"dev_pm_qos_update_user_latency_tolerance", (void *)(uintptr_t)&kb_return_zero},
    {"dev_set_name", (void *)(uintptr_t)&kb_return_zero},
    {"device_add", (void *)(uintptr_t)&kb_return_zero},
    {"device_add_disk", (void *)(uintptr_t)&kb_return_zero},
    {"device_del", (void *)(uintptr_t)&kb_noop},
    {"device_initialize", (void *)(uintptr_t)&kb_noop},
    {"device_remove_file_self", (void *)(uintptr_t)&kb_return_zero},
    {"disable_irq", (void *)(uintptr_t)&kb_disable_irq_nosync},
    {"disk_set_zoned", (void *)(uintptr_t)&kb_return_zero},
    {"disk_uevent", (void *)(uintptr_t)&kb_noop},
    {"disk_update_readahead", (void *)(uintptr_t)&kb_noop},
    {"dmi_match", (void *)(uintptr_t)&kb_return_zero},
    {"dmi_get_system_info", (void *)(uintptr_t)&kb_empty_string},
    {"drm_gem_object_free", (void *)(uintptr_t)&kb_noop},
    {"ext_pi_type1_crc64", (void *)(uintptr_t)&kb_return_zero},
    {"ext_pi_type3_crc64", (void *)(uintptr_t)&kb_return_zero},
    {"finish_wait", (void *)(uintptr_t)&kb_noop},
    {"fd_install", (void *)(uintptr_t)&kb_noop},
    {"fget", (void *)(uintptr_t)&kb_return_zero},
    {"filp_close", (void *)(uintptr_t)&kb_return_zero},
    {"filp_open", (void *)(uintptr_t)&kb_return_zero},
    {"find_vma_intersection", (void *)(uintptr_t)&kb_return_zero},
    {"follow_pfn", (void *)(uintptr_t)&kb_return_zero},
    {"fput", (void *)(uintptr_t)&kb_noop},
    {"free_opal_dev", (void *)(uintptr_t)&kb_noop},
    {"free_pages", (void *)(uintptr_t)&kb_noop},
    {"full_name_hash", (void *)(uintptr_t)&kb_return_zero},
    {"get_device", (void *)(uintptr_t)&kb_identity_ptr},
    {"get_unused_fd_flags", (void *)(uintptr_t)&kb_return_zero},
    {"hwmon_device_register_with_info", (void *)(uintptr_t)&kb_hwmon_device_register_with_info},
    {"hwmon_device_unregister", (void *)(uintptr_t)&kb_noop},
    {"i2c_add_adapter", (void *)(uintptr_t)&kb_return_zero},
    {"i2c_del_adapter", (void *)(uintptr_t)&kb_noop},
    {"ida_alloc_range", (void *)(uintptr_t)&kb_return_zero},
    {"ida_destroy", (void *)(uintptr_t)&kb_noop},
    {"ida_free", (void *)(uintptr_t)&kb_noop},
    {"init_opal_dev", (void *)(uintptr_t)&kb_return_zero},
    {"init_srcu_struct", (void *)(uintptr_t)&kb_return_zero},
    {"init_timer_key", (void *)(uintptr_t)&kb_noop},
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
    {"iterate_fd", (void *)(uintptr_t)&kb_return_zero},
    {"jiffies_to_msecs", (void *)(uintptr_t)&kb_return_zero},
    {"jiffies_to_timespec64", (void *)(uintptr_t)&kb_noop},
    {"jiffies_to_usecs", (void *)(uintptr_t)&kb_return_zero},
    {"kasprintf", (void *)(uintptr_t)&kb_return_zero},
    {"kblockd_schedule_work", (void *)(uintptr_t)&kb_return_zero},
    {"kernel_read", (void *)(uintptr_t)&kb_return_zero},
    {"kernel_write", (void *)(uintptr_t)&kb_return_zero},
    {"kfree_const", (void *)(uintptr_t)&kb_kfree},
    {"kthread_create_on_node", (void *)(uintptr_t)&kb_alloc_stub},
    {"kthread_should_stop", (void *)(uintptr_t)&kb_return_one},
    {"kthread_stop", (void *)(uintptr_t)&kb_return_zero},
    {"kmem_cache_create", (void *)(uintptr_t)&kb_alloc_stub},
    {"kmem_cache_destroy", (void *)(uintptr_t)&kb_noop},
    {"kmem_cache_free", (void *)(uintptr_t)&kb_kmem_cache_free},
    {"kobject_uevent_env", (void *)(uintptr_t)&kb_return_zero},
    {"ktime_get_with_offset", (void *)(uintptr_t)&kb_return_zero},
    {"ktime_get_raw_ts64", (void *)(uintptr_t)&kb_noop},
    {"ktime_get_real_ts64", (void *)(uintptr_t)&kb_noop},
    {"kvfree", (void *)(uintptr_t)&kb_kfree},
    {"kvmalloc_node", (void *)(uintptr_t)&kb_kzalloc},
    {"memchr_inv", (void *)(uintptr_t)&kb_return_zero},
    {"mempool_alloc_slab", (void *)(uintptr_t)&kb_alloc_stub},
    {"mempool_free_slab", (void *)(uintptr_t)&kb_noop},
    {"memremap_compat_align", (void *)(uintptr_t)&kb_return_zero},
    {"mod_timer", (void *)(uintptr_t)&kb_return_zero},
    {"module_put", (void *)(uintptr_t)&kb_noop},
    {"msleep", (void *)(uintptr_t)&kb_noop},
    {"numa_node", (void *)(uintptr_t)&kb_return_zero},
    {"ndelay", (void *)(uintptr_t)&kb_noop},
    {"opal_unlock_from_suspend", (void *)(uintptr_t)&kb_return_zero},
    {"on_each_cpu_cond_mask", (void *)(uintptr_t)&kb_noop},
    {"param_get_uint", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_uint", (void *)(uintptr_t)&kb_return_zero},
    {"param_set_uint_minmax", (void *)(uintptr_t)&kb_return_zero},
    {"panic", (void *)(uintptr_t)&kb_stack_chk_fail},
    {"pcibios_resource_to_bus", (void *)(uintptr_t)&kb_noop},
    {"pci_alloc_p2pmem", (void *)(uintptr_t)&kb_return_zero},
    {"pci_clear_master", (void *)(uintptr_t)&kb_noop},
    {"pci_dev_present", (void *)(uintptr_t)&kb_return_one},
    {"pci_dev_put", (void *)(uintptr_t)&kb_noop},
    {"pci_disable_msi", (void *)(uintptr_t)&kb_noop},
    {"pci_disable_msix", (void *)(uintptr_t)&kb_noop},
    {"pci_enable_atomic_ops_to_root", (void *)(uintptr_t)&kb_return_zero},
    {"pci_enable_msi", (void *)(uintptr_t)&kb_return_zero},
    {"pci_enable_msix_range", (void *)(uintptr_t)&kb_return_one},
    {"pci_find_capability", (void *)(uintptr_t)&kb_return_zero},
    {"pci_free_p2pmem", (void *)(uintptr_t)&kb_noop},
    {"pci_get_class", (void *)(uintptr_t)&kb_return_zero},
    {"pci_get_domain_bus_and_slot", (void *)(uintptr_t)&kb_return_zero},
    {"pin_user_pages", (void *)(uintptr_t)&kb_return_zero},
    {"pci_load_saved_state", (void *)(uintptr_t)&kb_return_zero},
    {"pci_p2pdma_add_resource", (void *)(uintptr_t)&kb_return_zero},
    {"pci_p2pmem_publish", (void *)(uintptr_t)&kb_return_zero},
    {"pci_p2pmem_virt_to_bus", (void *)(uintptr_t)&kb_return_zero},
    {"pci_read_config_byte", (void *)(uintptr_t)&kb_return_zero},
    {"pci_read_config_dword", (void *)(uintptr_t)&kb_return_zero},
    {"pci_release_regions", (void *)(uintptr_t)&kb_noop},
    {"pci_request_regions", (void *)(uintptr_t)&kb_return_zero},
    {"pci_sriov_configure_simple", (void *)(uintptr_t)&kb_return_zero},
    {"pci_stop_and_remove_bus_device", (void *)(uintptr_t)&kb_noop},
    {"pci_write_config_byte", (void *)(uintptr_t)&kb_return_zero},
    {"pci_write_config_dword", (void *)(uintptr_t)&kb_return_zero},
    {"pci_write_config_word", (void *)(uintptr_t)&kb_return_zero},
    {"pcie_capability_read_word", (void *)(uintptr_t)&kb_return_zero},
    {"perf_trace_buf_alloc", (void *)(uintptr_t)&kb_alloc_stub},
    {"perf_trace_run_bpf_submit", (void *)(uintptr_t)&kb_noop},
    {"prepare_to_wait_event", (void *)(uintptr_t)&kb_return_zero},
    {"proc_create_data", (void *)(uintptr_t)&kb_alloc_stub},
    {"proc_mkdir_mode", (void *)(uintptr_t)&kb_alloc_stub},
    {"proc_remove", (void *)(uintptr_t)&kb_noop},
    {"put_device", (void *)(uintptr_t)&kb_noop},
    {"put_disk", (void *)(uintptr_t)&kb_noop},
    {"queue_delayed_work_on", (void *)(uintptr_t)&kb_return_one},
    {"queue_work_on", (void *)(uintptr_t)&kb_return_one},
    {"rb_erase", (void *)(uintptr_t)&kb_noop},
    {"rb_first", (void *)(uintptr_t)&kb_return_zero},
    {"rb_insert_color", (void *)(uintptr_t)&kb_noop},
    {"refcount_warn_saturate", (void *)(uintptr_t)&kb_noop},
    {"remap_pfn_range", (void *)(uintptr_t)&kb_return_zero},
    {"register_acpi_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"release_firmware", (void *)(uintptr_t)&kb_noop},
    {"remove_proc_entry", (void *)(uintptr_t)&kb_noop},
    {"request_firmware", (void *)(uintptr_t)&kb_return_zero},
    {"schedule", (void *)(uintptr_t)&kb_noop},
    {"schedule_timeout", (void *)(uintptr_t)&kb_return_zero},
    {"sed_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"set_capacity", (void *)(uintptr_t)&kb_noop},
    {"set_capacity_and_notify", (void *)(uintptr_t)&kb_noop},
    {"set_disk_ro", (void *)(uintptr_t)&kb_noop},
    {"set_memory_uc", (void *)(uintptr_t)&kb_return_zero},
    {"set_memory_wb", (void *)(uintptr_t)&kb_return_zero},
    {"set_normalized_timespec64", (void *)(uintptr_t)&kb_noop},
    {"set_page_dirty_lock", (void *)(uintptr_t)&kb_return_zero},
    {"set_pages_array_uc", (void *)(uintptr_t)&kb_return_zero},
    {"set_pages_array_wb", (void *)(uintptr_t)&kb_return_zero},
    {"seq_lseek", (void *)(uintptr_t)&kb_return_zero},
    {"seq_printf", (void *)(uintptr_t)&kb_return_zero},
    {"seq_puts", (void *)(uintptr_t)&kb_return_zero},
    {"seq_read", (void *)(uintptr_t)&kb_return_zero},
    {"seq_read_iter", (void *)(uintptr_t)&kb_return_zero},
    {"single_open", (void *)(uintptr_t)&kb_return_zero},
    {"single_release", (void *)(uintptr_t)&kb_return_zero},
    {"simple_strtoul", (void *)(uintptr_t)&strtoul},
    {"strscpy", (void *)(uintptr_t)&kb_return_zero},
    {"submit_bio_noacct", (void *)(uintptr_t)&kb_noop},
    {"synchronize_rcu", (void *)(uintptr_t)&kb_noop},
    {"synchronize_srcu", (void *)(uintptr_t)&kb_noop},
    {"sysfs_create_link", (void *)(uintptr_t)&kb_return_zero},
    {"sysfs_remove_link", (void *)(uintptr_t)&kb_noop},
    {"t10_pi_type1_crc", (void *)(uintptr_t)&kb_return_zero},
    {"t10_pi_type3_crc", (void *)(uintptr_t)&kb_return_zero},
    {"timer_delete_sync", (void *)(uintptr_t)&kb_return_zero},
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
    {"up_read", (void *)(uintptr_t)&kb_noop},
    {"unregister_chrdev_region", (void *)(uintptr_t)&kb_noop},
    {"unregister_acpi_notifier", (void *)(uintptr_t)&kb_return_zero},
    {"unmap_mapping_range", (void *)(uintptr_t)&kb_noop},
    {"unpin_user_page", (void *)(uintptr_t)&kb_noop},
    {"usleep_range_state", (void *)(uintptr_t)&kb_noop},
    {"vmap", (void *)(uintptr_t)&kb_alloc_stub},
    {"vfree", (void *)(uintptr_t)&kb_kfree},
    {"vmalloc", (void *)(uintptr_t)&kb_kzalloc},
    {"vmalloc_to_page", (void *)(uintptr_t)&kb_return_zero},
    {"vm_insert_page", (void *)(uintptr_t)&kb_return_zero},
    {"vmf_insert_pfn", (void *)(uintptr_t)&kb_return_zero},
    {"vmf_insert_pfn_prot", (void *)(uintptr_t)&kb_return_zero},
    {"vunmap", (void *)(uintptr_t)&kb_kfree},
    {"vga_set_legacy_decoding", (void *)(uintptr_t)&kb_noop},
    {"wait_for_random_bytes", (void *)(uintptr_t)&kb_return_zero},
    {"wake_up_process", (void *)(uintptr_t)&kb_return_one},
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

static void *lookup_exported_symbol(const char *name)
{
    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; i < KB_EXPORTED_SYMBOL_MAX; i++) {
        if (exported_symbols[i].name != 0 && strcmp(exported_symbols[i].name, name) == 0) {
            return (void *)(uintptr_t)exported_symbols[i].address;
        }
    }
    return 0;
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
    if (strcmp(name, "pcpu_hot") == 0) {
        return (void *)(uintptr_t)KB_LOCAL_GS_PCPU_HOT_OFFSET;
    }
    if (strcmp(name, "__default_kernel_pte_mask") == 0 ||
        strcmp(name, "_ctype") == 0 ||
        strcmp(name, "acpi_gbl_FADT") == 0 ||
        strcmp(name, "boot_cpu_data") == 0 ||
        strcmp(name, "devmap_managed_key") == 0 ||
        strcmp(name, "dma_ops") == 0 ||
        strcmp(name, "efi") == 0 ||
        strcmp(name, "hugetlb_optimize_vmemmap_key") == 0 ||
        strcmp(name, "iomem_resource") == 0 ||
        strcmp(name, "ioport_resource") == 0 ||
        strcmp(name, "node_data") == 0 ||
        strcmp(name, "pci_bus_type") == 0 ||
        strcmp(name, "screen_info") == 0 ||
        strcmp(name, "sme_me_mask") == 0 ||
        strcmp(name, "system_wq") == 0 || strcmp(name, "node_states") == 0 || strcmp(name, "page_offset_base") == 0 ||
        strcmp(name, "phys_base") == 0 || strcmp(name, "vmemmap_base") == 0 ||
        strcmp(name, "jiffies") == 0 || strcmp(name, "uuid_null") == 0 ||
        strcmp(name, "pm_suspend_global_flags") == 0)
    {
        return module->shim_misc_data;
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
                symbol.name == 0 || symbol.name[0] == '\0')
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
                if (exported_symbols[i].name != 0 && strcmp(exported_symbols[i].name, symbol.name) == 0) {
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
    write_u64le((uint8_t *)module->shim_cpu_possible_mask, 1);
    write_u64le((uint8_t *)module->shim_cpu_online_mask, 1);
    write_u32le((uint8_t *)module->shim_nr_cpu_ids, 1);
    write_u64le((uint8_t *)module->shim_this_cpu_off, 0);

    for (size_t i = 0; i < sizeof(shim_symbols) / sizeof(shim_symbols[0]); i++) {
        write_abs_jump_stub(
            module->shim_symbol_stubs + (i * KB_LOCAL_SHIM_STUB_SIZE),
            shim_symbols[i].address);
    }
    const uint64_t pv_return_zero = (uint64_t)(uintptr_t)lookup_module_shim_symbol(module, "crc32_le");
    write_u64le((uint8_t *)module->shim_pv_ops + 0xb0, pv_return_zero);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xb8, pv_return_zero);
    write_u64le((uint8_t *)module->shim_pv_ops + 0xf0, pv_return_zero);

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
        if (getenv("KOBOX_TRACE_MODULES") != NULL) {
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

        void *address = lookup_exported_symbol(symbol.name);
        if (address == 0) {
            address = lookup_module_shim_symbol(module, symbol.name);
        }
        if (address == 0) {
            if (getenv("KOBOX_TRACE_MODULES") != NULL) {
                fprintf(
                    stderr,
                    "kobox-loader: unresolved symbol name=%s relocation_type=%u relocation_offset=0x%llx\n",
                    symbol.name,
                    (unsigned)relocation->type,
                    (unsigned long long)relocation->offset);
            }
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
            if (getenv("KOBOX_TRACE_MODULES") != NULL) {
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
        if (getenv("KOBOX_TRACE_MODULES") != NULL) {
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
    unregister_module_exports(module);
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
    if (getenv("KOBOX_TRACE_MODULES") != NULL) {
        fprintf(
            stderr,
            "kobox-loader: loaded %s base=%p size=0x%llx\n",
            image->name == NULL ? "(unnamed)" : image->name,
            module->image_base,
            (unsigned long long)module->image_size);
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
