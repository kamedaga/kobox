#include "kobox/shim.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_NETDEV_TRACKED_MAX = 32,
    KB_NETDEV_OPS_OFFSET = 0x8,
    KB_NETDEV_ADDR_MAX = 32,
};

typedef int (*kb_netdev_ndo_init_t)(void *dev);

typedef struct kb_netdev_record {
    void *dev;
    size_t size;
    unsigned char dev_addr[KB_NETDEV_ADDR_MAX];
    int registered;
} kb_netdev_record_t;

static unsigned long possible_cpu_mask_storage = 1;
unsigned long *__cpu_possible_mask = &possible_cpu_mask_storage;
unsigned int nr_cpu_ids = 1;
unsigned long this_cpu_off = 0;
char pernet_ops_rwsem[64];
static kb_netdev_record_t netdev_records[KB_NETDEV_TRACKED_MAX];

static kb_netdev_record_t *find_netdev_record(void *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_NETDEV_TRACKED_MAX; i++) {
        if (netdev_records[i].dev == dev) {
            return &netdev_records[i];
        }
    }
    return NULL;
}

static void track_netdev(void *dev, size_t size)
{
    if (dev == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_NETDEV_TRACKED_MAX; i++) {
        if (netdev_records[i].dev == NULL) {
            netdev_records[i].dev = dev;
            netdev_records[i].size = size;
            memset(netdev_records[i].dev_addr, 0, sizeof(netdev_records[i].dev_addr));
            netdev_records[i].registered = 0;
            return;
        }
    }
}

static void untrack_netdev(void *dev)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record != NULL) {
        memset(record, 0, sizeof(*record));
    }
}

void kb_cond_resched(void)
{
}

void *kb_alloc_percpu_gfp(size_t size, size_t align, unsigned int flags)
{
    (void)align;
    (void)flags;
    return calloc(1, size);
}

void kb_free_percpu(void *ptr)
{
    free(ptr);
}

int kb_rtnl_link_register(void *ops)
{
    (void)ops;
    return 0;
}

void kb_rtnl_link_unregister(void *ops)
{
    (void)ops;
}

void kb_rtnl_lock(void)
{
}

void kb_rtnl_unlock(void)
{
}

int kb_register_netdevice(void *dev)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record == NULL || record->size < KB_NETDEV_OPS_OFFSET + sizeof(void *)) {
        return 0;
    }

    void *ops = NULL;
    memcpy(&ops, (const unsigned char *)dev + KB_NETDEV_OPS_OFFSET, sizeof(ops));
    if (ops != NULL) {
        kb_netdev_ndo_init_t ndo_init = NULL;
        memcpy(&ndo_init, ops, sizeof(ndo_init));
        if (ndo_init != NULL) {
            int result = ndo_init(dev);
            if (result < 0) {
                return result;
            }
        }
    }
    record->registered = 1;
    return 0;
}

void *kb_alloc_netdev_mqs(
    int sizeof_priv,
    const char *name,
    unsigned char name_assign_type,
    void (*setup)(void *),
    unsigned int txqs,
    unsigned int rxqs)
{
    (void)name;
    (void)name_assign_type;
    (void)txqs;
    (void)rxqs;
    size_t size = sizeof_priv < 0 ? 65536 : (size_t)sizeof_priv + 65536;
    void *dev = calloc(1, size);
    track_netdev(dev, size);
    if (dev != NULL && setup != NULL) {
        setup(dev);
    }
    return dev;
}

void kb_free_netdev(void *dev)
{
    untrack_netdev(dev);
    free(dev);
}

void kb_ether_setup(void *dev)
{
    (void)dev;
}

int kb_eth_mac_addr(void *dev, void *p)
{
    (void)dev;
    (void)p;
    return 0;
}

int kb_eth_validate_addr(void *dev)
{
    (void)dev;
    return 0;
}

void kb_dev_addr_mod(void *dev, unsigned int offset, const void *addr, size_t len)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record != NULL && addr != NULL && offset < KB_NETDEV_ADDR_MAX && len <= KB_NETDEV_ADDR_MAX - offset) {
        memcpy(record->dev_addr + offset, addr, len);
    }
}

void kb_netif_carrier_on(void *dev)
{
    (void)dev;
}

void kb_netif_carrier_off(void *dev)
{
    (void)dev;
}

void kb_get_random_bytes(void *buf, int len)
{
    unsigned char *bytes = buf;
    if (bytes == NULL || len <= 0) {
        return;
    }
    for (int i = 0; i < len; i++) {
        bytes[i] = (unsigned char)(0xa5u + (unsigned)i);
    }
}

void kb_consume_skb(void *skb)
{
    (void)skb;
}

void kb_skb_tstamp_tx(void *skb, void *hwtstamps)
{
    (void)skb;
    (void)hwtstamps;
}

void kb_skb_clone_tx_timestamp(void *skb)
{
    (void)skb;
}

uint64_t kb_dev_lstats_read(void *dev, uint64_t *packets, uint64_t *bytes)
{
    (void)dev;
    if (packets != NULL) {
        *packets = 0;
    }
    if (bytes != NULL) {
        *bytes = 0;
    }
    return 0;
}

int kb_ethtool_op_get_ts_info(void *dev, void *info)
{
    (void)dev;
    (void)info;
    return 0;
}

void kb_down_write(void *sem)
{
    (void)sem;
}

void kb_up_write(void *sem)
{
    (void)sem;
}

unsigned long kb_find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    if (addr == NULL || offset >= size) {
        return size;
    }
    for (unsigned long bit = offset; bit < size; bit++) {
        if ((addr[bit / (sizeof(unsigned long) * 8)] & (1ul << (bit % (sizeof(unsigned long) * 8)))) != 0) {
            return bit;
        }
    }
    return size;
}

void __SCT__cond_resched(void)
{
    kb_cond_resched();
}

void *__alloc_percpu_gfp(size_t size, size_t align, unsigned int flags)
{
    return kb_alloc_percpu_gfp(size, align, flags);
}

void free_percpu(void *ptr)
{
    kb_free_percpu(ptr);
}

int __rtnl_link_register(void *ops)
{
    return kb_rtnl_link_register(ops);
}

void __rtnl_link_unregister(void *ops)
{
    kb_rtnl_link_unregister(ops);
}

void rtnl_link_unregister(void *ops)
{
    kb_rtnl_link_unregister(ops);
}

void rtnl_lock(void)
{
    kb_rtnl_lock();
}

void rtnl_unlock(void)
{
    kb_rtnl_unlock();
}

int register_netdevice(void *dev)
{
    return kb_register_netdevice(dev);
}

void *alloc_netdev_mqs(
    int sizeof_priv,
    const char *name,
    unsigned char name_assign_type,
    void (*setup)(void *),
    unsigned int txqs,
    unsigned int rxqs)
{
    return kb_alloc_netdev_mqs(sizeof_priv, name, name_assign_type, setup, txqs, rxqs);
}

void free_netdev(void *dev)
{
    kb_free_netdev(dev);
}

void ether_setup(void *dev)
{
    kb_ether_setup(dev);
}

int eth_mac_addr(void *dev, void *p)
{
    return kb_eth_mac_addr(dev, p);
}

int eth_validate_addr(void *dev)
{
    return kb_eth_validate_addr(dev);
}

void dev_addr_mod(void *dev, unsigned int offset, const void *addr, size_t len)
{
    kb_dev_addr_mod(dev, offset, addr, len);
}

void netif_carrier_on(void *dev)
{
    kb_netif_carrier_on(dev);
}

void netif_carrier_off(void *dev)
{
    kb_netif_carrier_off(dev);
}

void get_random_bytes(void *buf, int len)
{
    kb_get_random_bytes(buf, len);
}

void consume_skb(void *skb)
{
    kb_consume_skb(skb);
}

void skb_tstamp_tx(void *skb, void *hwtstamps)
{
    kb_skb_tstamp_tx(skb, hwtstamps);
}

void skb_clone_tx_timestamp(void *skb)
{
    kb_skb_clone_tx_timestamp(skb);
}

uint64_t dev_lstats_read(void *dev, uint64_t *packets, uint64_t *bytes)
{
    return kb_dev_lstats_read(dev, packets, bytes);
}

int ethtool_op_get_ts_info(void *dev, void *info)
{
    return kb_ethtool_op_get_ts_info(dev, info);
}

void down_write(void *sem)
{
    kb_down_write(sem);
}

void up_write(void *sem)
{
    kb_up_write(sem);
}

unsigned long _find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    return kb_find_next_bit(addr, size, offset);
}
