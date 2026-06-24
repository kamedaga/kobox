#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "kobox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
#define KB_STACK_REALIGN __attribute__((force_align_arg_pointer))
#else
#define KB_STACK_REALIGN
#endif

void *kb_kmalloc(size_t size, unsigned int flags);
void *kb_kzalloc(size_t size, unsigned int flags);
void *kb_kmalloc_trace(void *cache, unsigned int flags, size_t size);
void *kb_kmalloc_cache_noprof(void *cache, unsigned int flags, size_t size);
void *kb_kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags, void *ctor);
void *kb_kmem_cache_create_args(const char *name, unsigned int object_size, void *args, unsigned int flags);
void kb_kmem_cache_destroy(void *cache);
void *kb_kmem_cache_alloc(void *cache, unsigned int flags);
void kb_kmem_cache_free(void *cache, void *ptr);
void kb_kfree(void *ptr);
void *kb_krealloc_managed(void *ptr, size_t size, unsigned int flags);
size_t kb_kmalloc_usable_size(const void *ptr);
int KB_STACK_REALIGN kb_printk(const char *fmt, ...);
int KB_STACK_REALIGN kb_vprintk_safe(const char *fmt, va_list args);
int KB_STACK_REALIGN kb_vsnprintf_safe(char *buf, size_t size, const char *fmt, va_list args);
int KB_STACK_REALIGN kb_snprintf_safe(char *buf, size_t size, const char *fmt, ...);
int KB_STACK_REALIGN kb_sprintf_safe(char *buf, const char *fmt, ...);
char *kb_strreplace(char *s, char old_char, char new_char);
size_t kb_memweight(const void *ptr, size_t bytes);
int KB_STACK_REALIGN kb_tracef(const char *fmt, ...);

void kb_shim_set_device_backend(kb_device_backend_t *backend);
unsigned long kb_shim_current_kernel_gs(void);
int kb_shim_enter_kernel_gs(unsigned long kernel_gs, unsigned long *out_old_gs);
void kb_shim_leave_kernel_gs(unsigned long old_gs);
void *kb_module_lookup_exported_symbol(const char *name);
unsigned long kb_module_kernel_gs_for_address(const void *address);
int kb_module_is_executable_address(const void *address);
void *kb_module_make_gs_call_stub(const void *target, unsigned long caller_gs);
void *kb_module_current_external_call_target(void);
unsigned long kb_module_current_external_call_caller_gs(void);
unsigned long kb_module_current_external_call_callee_gs(void);
void kb_linux_call_void_ptr(void (*fn)(void *), void *arg);
void kb_linux_call_void_ptr_gs(void (*fn)(void *), void *arg, unsigned long kernel_gs);
void kb_linux_call_void_ulong(void (*fn)(unsigned long), unsigned long arg);
void kb_linux_call_void_ulong_gs(void (*fn)(unsigned long), unsigned long arg, unsigned long kernel_gs);
void *kb_linux_call_ptr_ptr(void *(*fn)(void *), void *arg0);
int kb_linux_call_int_ptr(int (*fn)(void *), void *arg0);
int kb_linux_call_int_ptr_uint(int (*fn)(void *, unsigned int), void *arg0, unsigned int arg1);
int kb_linux_call_int_int_ptr(int (*fn)(int, void *), int arg0, void *arg1);
int kb_linux_call_int_ptr_ptr(int (*fn)(void *, char *), void *arg0, char *arg1);
int kb_linux_call_int_ptr_ptr_raw(int (*fn)(void *, void *), void *arg0, void *arg1);
int kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
    int (*fn)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t),
    void *arg0,
    uint16_t arg1,
    uint16_t arg2,
    uint16_t arg3,
    char *arg4,
    uint16_t arg5);
int kb_linux_call_int_ptr_uint_u8_u8_u16_u16_ptr_u16_int(
    int (*fn)(void *, unsigned int, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t, int),
    void *arg0,
    unsigned int arg1,
    uint8_t arg2,
    uint8_t arg3,
    uint16_t arg4,
    uint16_t arg5,
    void *arg6,
    uint16_t arg7,
    int arg8);
int kb_usb_control_msg_entry(
    void *dev,
    unsigned int pipe,
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size,
    int timeout);

int kb_request_threaded_irq(
    unsigned int irq,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    unsigned long flags,
    const char *name,
    void *dev_id);
int kb_devm_request_threaded_irq(
    void *dev,
    unsigned int irq,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    unsigned long flags,
    const char *name,
    void *dev_id);
void kb_free_irq(unsigned int irq, void *dev_id);
void kb_free_all_irqs(void);
int kb_wait_irq_for_dev_id(void *dev_id, uint64_t timeout_ns);
int kb_wait_irq_signal_for_dev_id(void *dev_id, uint64_t timeout_ns);
int kb_handle_irq_for_dev_id(void *dev_id, uint64_t timeout_ns);
int kb_trigger_irq_for_dev_id(void *dev_id);
int kb_handle_any_irq(uint64_t timeout_ns);
int kb_handle_any_irq_no_work(uint64_t timeout_ns);
int kb_irq_allocate_mapping(unsigned int backend_kind, unsigned int backend_vector, unsigned int *out_linux_irq);
unsigned int kb_irq_backend_vector(unsigned int linux_irq);
void kb_irq_clear_mappings(void);
int kb_queue_work_on(int cpu, void *wq, void *work);
int kb_kblockd_schedule_work(void *work);
int kb_queue_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay);
int kb_mod_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay);
int kb_flush_work(void *work);
int kb_flush_delayed_work(void *dwork);
int kb_cancel_work_sync(void *work);
int kb_cancel_delayed_work(void *dwork);
int kb_cancel_delayed_work_sync(void *dwork);
void kb_tasklet_setup(void *tasklet, void (*callback)(void *tasklet));
void kb_tasklet_schedule(void *tasklet);
void kb_init_timer_key(void *timer, void (*callback)(void *timer), unsigned int flags, const char *name, void *key);
int kb_mod_timer(void *timer, unsigned long expires);
void kb_add_timer(void *timer);
int kb_timer_delete(void *timer);
unsigned long kb_schedule_timeout(unsigned long timeout);
void kb_run_deferred_work(void);
void kb_run_deferred_bottom_halves(void);
int kb_deferred_work_is_draining(void);
void kb_jbd2_progress_registered_journals(void);
void *kb_jbd2_journal_init_stub(void);
void *kb_jbd2_journal_start_stub(void);
int kb_jbd2_journal_stop_stub(void *handle);
int kb_jbd2_journal_blocks_per_page_stub(void);
void kb_jbd2_journal_destroy_stub(void *journal);
void kb_register_jiffies_storage(void *storage);
unsigned long kb_msecs_to_jiffies(unsigned int msecs);
unsigned long kb_usecs_to_jiffies(unsigned int usecs);
unsigned long kb_round_jiffies_up(unsigned long jiffies);
unsigned int kb_jiffies_to_msecs(unsigned long jiffies);
unsigned int kb_jiffies_to_usecs(unsigned long jiffies);
void kb_jiffies_to_timespec64(unsigned long jiffies, void *timespec64);
int64_t kb_ktime_get(void);
int64_t kb_ktime_get_with_offset(int offset);
uint64_t kb_ktime_get_mono_fast_ns(void);
void kb_ktime_get_raw_ts64(void *timespec64);
void kb_ktime_get_real_ts64(void *timespec64);
void kb_msleep(unsigned int msecs);
void kb_usleep_range_state(unsigned long min, unsigned long max, unsigned int state);
void kb_udelay(unsigned long usecs);
void kb_const_udelay(unsigned long xloops);
void kb_ndelay(unsigned long nsecs);

void *kb_dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs);
void kb_dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs);
void *kb_dma_cpu_addr(uint64_t dma_addr, size_t *out_available);
uint64_t kb_dma_map_page_attrs(void *dev, void *page, unsigned long offset, size_t size, int dir, unsigned long attrs);
uint64_t kb_dma_map_single_attrs(void *dev, void *ptr, size_t size, int dir, unsigned long attrs);
void kb_dma_unmap_page_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs);
void kb_dma_unmap_single_attrs(void *dev, uint64_t dma_addr, size_t size, int dir, unsigned long attrs);
int kb_dma_mapping_error(void *dev, uint64_t dma_addr);

int kb_pci_register_driver(void *driver, void *owner, const char *mod_name);
void kb_pci_unregister_driver(void *driver);
int kb_pci_enable_device(void *dev);
int kb_pcim_enable_device(void *dev);
void kb_pci_disable_device(void *dev);
void kb_pci_set_master(void *dev);
int kb_pci_read_config_byte(void *dev, int where, uint8_t *value);
int kb_pci_read_config_word(void *dev, int where, uint16_t *value);
int kb_pci_read_config_dword(void *dev, int where, uint32_t *value);
int kb_pci_write_config_byte(void *dev, int where, uint8_t value);
int kb_pci_write_config_word(void *dev, int where, uint16_t value);
int kb_pci_write_config_dword(void *dev, int where, uint32_t value);
int kb_pci_find_capability(void *dev, int cap);
int kb_pci_xhci_irq_pending(void);
void kb_pci_xhci_ack_pending(void);
unsigned int kb_pci_xhci_max_ports(void);
int kb_pci_xhci_port_status(unsigned int port, uint16_t *status_out, uint16_t *change_out);
int kb_pci_xhci_reset_port(unsigned int port);
void kb_pci_xhci_ring_cmd_db(void *xhci);
int kb_pci_enable_msi(void *dev);
int kb_pci_enable_msix_range(void *dev, void *entries, int minvec, int maxvec);
void kb_pci_disable_msi(void *dev);
void kb_pci_disable_msix(void *dev);
int kb_pci_msix_unmask_entries(void *dev, const uint16_t *entries, unsigned int count);
void *kb_pci_get_class(unsigned int class, void *from);
void *kb_pci_iomap(void *dev, int bar, unsigned long max);
void *kb_pcim_iomap(void *dev, int bar, unsigned long max);
int kb_pcim_iomap_regions_request_all(void *dev, int mask, const char *name);
void **kb_pcim_iomap_table(void *dev);
void kb_pci_iounmap(void *dev, void *addr);
void kb_pci_release_all_mmio_mappings(void);
void *kb_ioremap(uint64_t phys_addr, size_t size);
void kb_iounmap(void *addr);
uint8_t kb_ioread8(const void *addr);
void kb_iowrite8(uint8_t value, void *addr);
uint32_t kb_ioread32(const void *addr);
void kb_iowrite32(uint32_t value, void *addr);
typedef int (*kb_mmio_write32_hook_t)(void *user, void *addr, uint32_t value);
int kb_mmio_register_write32_hook(void *base, size_t size, kb_mmio_write32_hook_t hook, void *user);
void kb_mmio_unregister_write32_hook(void *base, void *user);
void *kb_pci_dev_get(void *dev);
void kb_pci_dev_put(void *dev);
void kb_pci_d3cold_disable(void *dev);
int kb_pci_choose_state(void *dev, int state);
int kb_pci_set_power_state(void *dev, int state);
int kb_pci_set_mwi(void *dev);
const void *kb_pci_match_id(const void *id_table, void *dev);

void kb_stack_chk_fail(void);
extern uintptr_t kb_ref_stack_chk_guard;
extern uint32_t kb_preempt_count;
extern uint32_t kb_num_online_cpus;
extern uint64_t kb_cpu_possible_mask;
extern uint64_t kb_cpu_present_mask;
extern uint64_t kb_user_ptr_max;
extern uint32_t kb_cpu_number;

int kb_platform_driver_register(void *driver, void *owner, const char *mod_name);
void kb_platform_driver_unregister(void *driver);
int kb_devm_add_action(void *dev, void (*action)(void *), void *data);
int kb_devm_uio_register_device(void *dev, void *info);
void kb_dynamic_dev_dbg(void *descriptor, const void *dev, const char *fmt, ...);
void kb_dev_err(const void *dev, const char *fmt, ...);
void kb_dev_warn(const void *dev, const char *fmt, ...);
void kb_dev_printk(const char *level, const void *dev, const char *fmt, ...);
void kb_pm_runtime_enable(void *dev);
void kb_pm_runtime_disable(void *dev, int check_resume);
int kb_pm_runtime_idle(void *dev, int rpmflags);
int kb_pm_runtime_resume(void *dev, int rpmflags);
void *kb_devm_kmalloc(void *dev, size_t size, unsigned int flags);
char *kb_devm_kasprintf(void *dev, unsigned int flags, const char *fmt, ...);
int kb_platform_get_irq_optional(void *pdev, unsigned int num);
void kb_disable_irq_nosync(unsigned int irq);
void kb_enable_irq(unsigned int irq);
void *kb_irq_get_irq_data(unsigned int irq);
void kb_irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set);
void kb_raw_spin_lock(void *lock);
int kb_raw_spin_trylock(void *lock);
unsigned long kb_raw_spin_lock_irqsave(void *lock);
void kb_raw_spin_unlock(void *lock);
void kb_raw_spin_unlock_irqrestore(void *lock, unsigned long flags);
int kb_atomic_notifier_chain_register(void *list, void *notifier);
int kb_atomic_notifier_chain_unregister(void *list, void *notifier);
int kb_kexec_crash_loaded(void);
int kb_kstrtouint(const char *s, unsigned int base, unsigned int *res);
int kb_sysfs_emit(char *buf, const char *fmt, ...);

void *kb_kmalloc_alias(size_t size, unsigned int flags);
void kb_might_resched(void);
void kb_ubsan_handle_load_invalid_value(void *data, void *ptr);
void kb_ubsan_handle_out_of_bounds(void *data, void *index);
void kb_sg_init_one(void *sg, const void *buf, unsigned int buflen);
void *kb_input_allocate_device(void);
void kb_input_free_device(void *dev);
int kb_input_register_device(void *dev);
void kb_input_unregister_device(void *dev);
void kb_input_event(void *dev, unsigned int type, unsigned int code, int value);
void kb_input_set_abs_params(void *dev, unsigned int axis, int min, int max, int fuzz, int flat);
void kb_input_alloc_absinfo(void *dev);
int kb_input_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags);

void kb_cond_resched(void);
void *kb_alloc_percpu_gfp(size_t size, size_t align, unsigned int flags);
void kb_free_percpu(void *ptr);
int kb_rtnl_link_register(void *ops);
void kb_rtnl_link_unregister(void *ops);
void kb_rtnl_lock(void);
void kb_rtnl_unlock(void);
int kb_register_netdevice(void *dev);
void *kb_alloc_netdev_mqs(int sizeof_priv, const char *name, unsigned char name_assign_type, void (*setup)(void *), unsigned int txqs, unsigned int rxqs);
void kb_free_netdev(void *dev);
void kb_ether_setup(void *dev);
int kb_eth_mac_addr(void *dev, void *p);
int kb_eth_validate_addr(void *dev);
void kb_dev_addr_mod(void *dev, unsigned int offset, const void *addr, size_t len);
void kb_netif_carrier_on(void *dev);
void kb_netif_carrier_off(void *dev);
void kb_get_random_bytes(void *buf, int len);
void kb_consume_skb(void *skb);
void kb_skb_tstamp_tx(void *skb, void *hwtstamps);
void kb_skb_clone_tx_timestamp(void *skb);
uint64_t kb_dev_lstats_read(void *dev, uint64_t *packets, uint64_t *bytes);
int kb_ethtool_op_get_ts_info(void *dev, void *info);
void kb_init_rwsem(void *sem);
void kb_down_read(void *sem);
void kb_up_read(void *sem);
void kb_down_write(void *sem);
void kb_up_write(void *sem);
unsigned long kb_find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long kb_find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned int kb_bitmap_weight(const unsigned long *addr, unsigned int bits);

void *kb_kmalloc_node(size_t size, unsigned int flags, int node);
void *kb_kmalloc_node_trace(void *cache, unsigned int flags, int node, size_t size);
void *kb_kmemdup(const void *src, size_t len, unsigned int flags);
void kb_kfree_sensitive(const void *ptr);
void *kb_dma_pool_create(const char *name, void *dev, size_t size, size_t align, size_t allocation);
void *kb_dma_pool_alloc(void *pool, unsigned int flags, uint64_t *dma_handle);
void kb_dma_pool_free(void *pool, void *vaddr, uint64_t dma_addr);
void kb_dma_pool_destroy(void *pool);
int kb_dma_set_mask(void *dev, uint64_t mask);
int kb_dma_set_coherent_mask(void *dev, uint64_t mask);
int kb_pci_alloc_irq_vectors(void *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags);
int kb_pci_alloc_irq_vectors_affinity(void *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags, void *affd);
void kb_pci_free_irq_vectors(void *dev);
int kb_pci_irq_vector(void *dev, unsigned int nr);
int kb_pci_request_irq(void *dev, unsigned int nr, int (*handler)(int, void *), int (*thread_fn)(int, void *), void *dev_id, const char *fmt, ...);
int kb_pci_request_irq_shim(void *dev, unsigned int nr, int (*handler)(int, void *), int (*thread_fn)(int, void *), void *dev_id, const char *fmt);
void kb_pci_free_irq(void *dev, unsigned int nr, void *dev_id);
int kb_pci_enable_device_mem(void *dev);
int kb_pci_request_selected_regions(void *dev, int bars, const char *name);
void kb_pci_release_selected_regions(void *dev, int bars);
int kb_pci_select_bars(void *dev, unsigned long flags);
int kb_pci_device_is_present(void *dev);
void kb_mutex_init(void *lock);
void kb_mutex_lock(void *lock);
void kb_mutex_unlock(void *lock);
int kb_mutex_trylock(void *lock);
void kb_complete(void *completion);
void kb_complete_all(void *completion);
void kb_init_completion(void *completion);
void kb_init_waitqueue_head(void *wq_head);
void kb_init_swait_queue_head(void *wq_head);
unsigned long kb_wait_for_completion(void *completion);
unsigned long kb_wait_for_completion_io_timeout(void *completion, unsigned long timeout);
void kb_noop_stub(void);
void kb_stackleak_track_stack_stub(void);
int kb_return_zero(void);
int kb_return_one(void);
int64_t kb_ktime_get_real_seconds(void);
uint32_t kb_get_random_u32_below(uint32_t ceil);
uint16_t kb_get_random_u16(void);
void kb_generate_random_uuid(unsigned char *uuid);
void kb_string_get_size(uint64_t size, uint64_t blk_size, int units, char *buf, int len);
char *kb_d_path(void *path, char *buffer, int buffer_length);
void *kb_kmemdup_nul(const void *src, size_t len, unsigned int flags);
long kb_sized_strscpy(char *dst, const char *src, size_t size);
char *kb_skip_spaces(const char *str);
void kb_memcpy_and_pad(void *dest, size_t dest_len, const void *src, size_t count, int pad);
int64_t kb_vfs_setpos(void *file, int64_t offset, int64_t maxsize);
int kb_list_add_valid_or_report(void *new_entry, void *prev, void *next);
int kb_list_del_entry_valid_or_report(void *entry);
void kb_rb_insert_color(void *node, void *root);
void kb_rb_erase(void *node, void *root);
void *kb_rb_first(void *root);
void *kb_rb_next(void *node);
void *kb_rb_prev(void *node);
void *kb_kthread_create_on_node(int (*threadfn)(void *data), void *data, int node, const char *namefmt, ...);
int kb_wake_up_process(void *task);
int kb_ida_alloc_range(void *ida, unsigned int min, unsigned int max, unsigned int flags);
void kb_ida_free(void *ida, unsigned int id);
void kb_ida_destroy(void *ida);
void *kb_alloc_stub(void);
int kb_percpu_counter_init_many_stub(void *counters, long amount, unsigned int batch, unsigned int count, void *key);
void kb_percpu_counter_add_batch_stub(void *counter, int64_t amount, int32_t batch);
int64_t kb_percpu_counter_sum_stub(void *counter);
void *kb_crypto_alloc_shash_stub(const char *alg_name, unsigned int type, unsigned int mask);
int kb_crypto_shash_final_stub(void *desc, void *out);
int kb_crypto_shash_tfm_digest_stub(void *tfm, const void *data, unsigned int len, void *out);
int kb_crypto_shash_update_stub(void *desc, const void *data, unsigned int len);
void *kb_identity_ptr(void *ptr);
const char *kb_empty_string(void);
int kb_dev_set_name(void *dev, const char *fmt, ...);
int kb_device_add(void *dev);
int kb_device_register(void *dev);
void *kb_dev_get_drvdata(void *dev);
void kb_dev_set_drvdata(void *dev, void *data);
int kb_driver_register(void *driver);
void kb_driver_unregister(void *driver);
int kb_driver_attach(void *driver);
void kb_usb_observe_linux_device(void *dev);
int kb_bus_for_each_dev(void *bus, void *start, void *data, int (*fn)(void *dev, void *data));
int kb_bus_for_each_drv(void *bus, void *start, void *data, int (*fn)(void *drv, void *data));
void *kb_sysfs_get_dirent(void *parent, const char *name);
void *kb_kernfs_find_and_get_ns(void *parent, const char *name, const void *ns);
void *kb_blk_alloc_disk(int node, void *lock_class_key);
void *kb_blk_mq_alloc_disk(void *tag_set, void *queuedata, void *lock_class_key);
void *kb_blk_mq_init_queue(void *tag_set);
int kb_blk_mq_alloc_tag_set(void *tag_set);
void kb_blk_mq_free_tag_set(void *tag_set);
void kb_blk_mq_destroy_queue(void *queue);
void *kb_blk_mq_alloc_request(void *queue, unsigned int op, unsigned int flags);
int kb_blk_rq_map_kern(void *queue, void *request, void *buffer, unsigned int length, unsigned int gfp);
int kb_blk_execute_rq(void *request, int at_head);
void kb_blk_mq_start_request(void *request);
void kb_blk_mq_complete_request(void *request);
int kb_blk_mq_complete_request_remote(void *request);
void kb_blk_mq_end_request(void *request, unsigned int status);
void kb_blk_mq_end_request_batch(void *batch);
void kb_blk_mq_free_request(void *request);
int kb_blk_status_to_errno(unsigned int status);
void kb_blk_mq_map_queues(void *queue_map);
int kb_blk_mq_pci_map_queues(void *queue_map, void *pdev, int offset);
void kb_blk_mq_tagset_busy_iter(void *tag_set, void *fn, void *priv);
void kb_blk_mq_tagset_wait_completed_request(void *tag_set);
void kb_blk_mq_update_nr_hw_queues(void *tag_set, unsigned int nr_hw_queues);
void kb_blk_put_queue(void *queue);
void kb_blk_queue_chunk_sectors(void *queue, unsigned int sectors);
void kb_blk_queue_dma_alignment(void *queue, int mask);
void kb_blk_queue_flag_set(unsigned int flag, void *queue);
void kb_blk_queue_io_min(void *queue, unsigned int size);
void kb_blk_queue_io_opt(void *queue, unsigned int size);
void kb_blk_queue_logical_block_size(void *queue, unsigned int size);
void kb_blk_queue_max_discard_sectors(void *queue, unsigned int sectors);
void kb_blk_queue_max_discard_segments(void *queue, unsigned int segments);
void kb_blk_queue_max_hw_sectors(void *queue, unsigned int sectors);
void kb_blk_queue_max_segments(void *queue, unsigned int segments);
void kb_blk_queue_max_write_zeroes_sectors(void *queue, unsigned int sectors);
void kb_blk_queue_max_zone_append_sectors(void *queue, unsigned int sectors);
void kb_blk_queue_physical_block_size(void *queue, unsigned int size);
void kb_blk_queue_virt_boundary(void *queue, unsigned long mask);
void kb_blk_queue_write_cache(void *queue, bool write_cache, bool fua);
int kb_device_add_disk(void *parent, void *disk, void *groups);
void kb_del_gendisk(void *disk);
void kb_blk_mark_disk_dead(void *disk);
int kb_blk_revalidate_disk_zones(void *disk, void *update_driver_data);
void kb_blk_set_stacking_limits(void *limits);
int kb_blk_stack_limits(void *top, void *bottom, unsigned int start);
void kb_disk_set_zoned(void *disk, unsigned int model);
void kb_disk_update_readahead(void *disk);
void kb_put_disk(void *disk);
void kb_set_capacity(void *disk, uint64_t sectors);
int kb_set_capacity_and_notify(void *disk, uint64_t sectors);
void kb_set_disk_ro(void *disk, int read_only);
void kb_blk_queue_bounce_limit(void *queue, uint64_t dma_mask);
void kb_blk_queue_update_dma_alignment(void *queue, int mask);
size_t kb_dma_max_mapping_size(void *dev);
void *kb_scsi_host_alloc(void *host_template, int private_size);
int kb_scsi_add_host_with_dma(void *host, void *dev, void *dma_dev);
void kb_scsi_scan_host(void *host);
void kb_scsi_remove_host(void *host);
void kb_scsi_host_put(void *host);
int kb_scsi_is_host_device(void *dev);
void kb_scsi_done(void *cmd);
void kb_scsi_report_bus_reset(void *host, int channel);
void kb_scsi_report_device_reset(void *host, int channel, int target);
int kb_scsi_normalize_sense(const void *sense_buffer, int sb_len, void *sshdr);
void *kb_scsi_sense_desc_find(const void *sense_buffer, int sb_len, int desc_type);
void kb_scsi_eh_prep_cmnd(void *scmd, void *ses, unsigned char *cmnd, int cmnd_size, unsigned sense_bytes);
void kb_scsi_eh_restore_cmnd(void *scmd, void *ses);
void kb_sg_miter_start(void *miter, void *sgl, unsigned int nents, unsigned int flags);
int kb_sg_miter_next(void *miter);
int kb_sg_miter_skip(void *miter, unsigned int offset);
void kb_sg_miter_stop(void *miter);
int kb_sg_nents(void *sg);
void *kb_sg_next(void *sg);
int kb_nvme_io_smoke(void);
void *kb_hwmon_device_register_with_info(void *dev, const char *name, void *data, const void *chip, const void *groups);
int kb_usb_hcd_pci_probe(void *dev, const void *driver);
void kb_usb_hcd_pci_remove(void *dev);
void kb_usb_hcd_pci_shutdown(void *dev);
int kb_usb_hcd_irq(int irq, void *hcd);
int kb_usb_hcd_is_primary_hcd(void *hcd);
void kb_usb_reset_root_hub_poll_live_state(void);
void kb_usb_pause_root_hub_poll_for_live(void);
int kb_usb_root_hub_poll_needed(void);
int kb_usb_poll_root_hub(void *hcd);
int kb_usb_poll_root_hubs(void);
int kb_usb_synthesize_connected_storage(void);
int kb_usb_synthesize_connected_hid_mouse(void);
void kb_usb_set_event_injection_runtime_allowed(int allowed);
int kb_usb_add_hcd(void *hcd, unsigned int irqnum, unsigned long irqflags);
void kb_usb_remove_hcd(void *hcd);
void *kb_usb_create_shared_hcd(const void *driver, void *dev, const char *bus_name, void *primary_hcd);
void kb_usb_put_hcd(void *hcd);
void *kb_usb_hcd_pci_pm_ops_storage(void);
void *kb_usb_xhci_tracepoint_storage(void);
void *kb_usb_num_online_cpus_storage(void);
void *kb_usb_pcpu_hot_storage(void);
void *kb_usb_pm_suspend_target_state_storage(void);
void kb_usb_hc_died(void *hcd);
int kb_usb_hcd_amd_remote_wakeup_quirk(void *hcd);
int kb_usb_hcd_check_unlink_urb(void *hcd, void *urb, int status);
void kb_usb_hcd_end_port_resume(void *hcd);
void kb_usb_hcd_giveback_urb(void *hcd, void *urb, int status);
int kb_usb_hcd_link_urb_to_ep(void *hcd, void *urb);
int kb_usb_hcd_map_urb_for_dma(void *hcd, void *urb, unsigned int mem_flags);
void kb_usb_hcd_poll_rh_status(void *hcd);
void kb_usb_hcd_resume_root_hub(void *hcd);
void kb_usb_hcd_start_port_resume(void *hcd);
void kb_usb_hcd_unlink_urb_from_ep(void *hcd, void *urb);
void kb_usb_hcd_unmap_urb_for_dma(void *hcd, void *urb);
void kb_usb_cleanup_tracked_urb_dma(void);
void kb_usb_root_hub_lost_power(void *root_hub);
int kb_usb_hub_clear_tt_buffer(void *udev, int pipe);
int kb_usb_hub_port_debounce(void *hub, int port1, int must_be_connected);
void kb_usb_wakeup_notification(void *hdev, unsigned int portnum);
int kb_usb_decode_interval(void *udev, void *ep);
const char *kb_usb_ep_type_string(int type);
const char *kb_usb_speed_string(int speed);
const char *kb_usb_state_string(int state);
int kb_usb_register_driver(void *driver, void *owner, const char *mod_name);
void kb_usb_deregister(void *driver);
void kb_usb_deregister_dev(void *interface, const void *class_driver);
void *kb_usb_find_interface(void *driver, int minor);
int kb_usb_find_common_endpoints(void *altsetting, void **bulk_in, void **bulk_out, void **int_in, void **int_out);
int kb_usb_submit_urb(void *urb, unsigned int mem_flags);
int kb_usb_unlink_urb(void *urb);
void kb_usb_kill_urb(void *urb);
int kb_usb_control_msg_shim(
    void *dev,
    unsigned int pipe,
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size,
    int timeout);
void kb_xhci_init_driver(void *driver, const void *overrides);
int kb_xhci_gen_setup(void *hcd, void *xhci, const void *quirks);
int kb_xhci_run(void *hcd);
void kb_xhci_stop(void *hcd);
void kb_xhci_shutdown(void *hcd);
int kb_xhci_suspend(void *xhci, bool do_wakeup);
int kb_xhci_resume(void *xhci, bool hibernated);
int kb_xhci_ext_cap_init(void *xhci);
int kb_xhci_update_hub_device(void *hcd, void *udev, void *tt, unsigned int devnum);
int kb_xhci_find_slot_id_by_port(void *hcd, void *xhci, unsigned int port);
uint32_t kb_xhci_port_state_to_neutral(uint32_t state);
int kb_xhci_msi_irq(int irq, void *hcd);
void kb_xhci_dbg_trace(void *xhci, void *trace, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
