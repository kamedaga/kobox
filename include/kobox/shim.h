#pragma once

#include <stddef.h>
#include <stdint.h>
#include "kobox/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

void *kb_kmalloc(size_t size, unsigned int flags);
void *kb_kzalloc(size_t size, unsigned int flags);
void *kb_kmalloc_trace(void *cache, unsigned int flags, size_t size);
void kb_kfree(void *ptr);
int kb_printk(const char *fmt, ...);

void kb_shim_set_backend(kb_backend_t *backend);

int kb_request_threaded_irq(
    unsigned int irq,
    int (*handler)(int, void *),
    int (*thread_fn)(int, void *),
    unsigned long flags,
    const char *name,
    void *dev_id);
void kb_free_irq(unsigned int irq, void *dev_id);

void *kb_dma_alloc_attrs(void *dev, size_t size, uint64_t *dma_handle, unsigned int flags, unsigned long attrs);
void kb_dma_free_attrs(void *dev, size_t size, void *cpu_addr, uint64_t dma_handle, unsigned long attrs);

int kb_pci_register_driver(void *driver, void *owner, const char *mod_name);
void kb_pci_unregister_driver(void *driver);
int kb_pci_enable_device(void *dev);
int kb_pcim_enable_device(void *dev);
void kb_pci_disable_device(void *dev);
void kb_pci_set_master(void *dev);
void *kb_pci_iomap(void *dev, int bar, unsigned long max);
void *kb_pcim_iomap(void *dev, int bar, unsigned long max);
void kb_pci_iounmap(void *dev, void *addr);
uint32_t kb_ioread32(const void *addr);
void kb_iowrite32(uint32_t value, void *addr);
int kb_devm_pvpanic_probe(void *dev, void *base);

void kb_stack_chk_fail(void);

int kb_platform_driver_register(void *driver, void *owner, const char *mod_name);
void kb_platform_driver_unregister(void *driver);
int kb_devm_add_action(void *dev, void (*action)(void *), void *data);
int kb_devm_uio_register_device(void *dev, void *info);
void kb_dynamic_dev_dbg(void *descriptor, const void *dev, const char *fmt, ...);
void kb_dev_err(const void *dev, const char *fmt, ...);
void kb_dev_warn(const void *dev, const char *fmt, ...);
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
unsigned long kb_raw_spin_lock_irqsave(void *lock);
void kb_raw_spin_unlock(void *lock);
void kb_raw_spin_unlock_irqrestore(void *lock, unsigned long flags);

void *kb_kmalloc_alias(size_t size, unsigned int flags);
void kb_might_resched(void);
void kb_ubsan_handle_load_invalid_value(void *data, void *ptr);
void kb_ubsan_handle_out_of_bounds(void *data, void *index);
int kb_register_virtio_driver(void *driver);
void kb_unregister_virtio_driver(void *driver);
void kb_virtio_reset_device(void *device);
int kb_virtqueue_add_inbuf(void *vq, void *sgs, unsigned int num, void *data, unsigned int gfp);
int kb_virtqueue_add_outbuf(void *vq, void *sgs, unsigned int num, void *data, unsigned int gfp);
void *kb_virtqueue_detach_unused_buf(void *vq);
void *kb_virtqueue_get_buf(void *vq, unsigned int *len);
unsigned int kb_virtqueue_get_vring_size(void *vq);
int kb_virtqueue_kick(void *vq);
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
void kb_down_write(void *sem);
void kb_up_write(void *sem);
unsigned long kb_find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);

#ifdef __cplusplus
}
#endif
