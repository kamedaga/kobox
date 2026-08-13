#include "kobox/shim.h"
#include "linux_subsystem/net/net_device.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long possible_cpu_mask_storage = 1;
unsigned long *__cpu_possible_mask = &possible_cpu_mask_storage;
unsigned int nr_cpu_ids = 1;
unsigned long this_cpu_off = 0;
char pernet_ops_rwsem[64];
typedef struct kb_percpu_alloc_record {
    void *base;
    void *relative;
} kb_percpu_alloc_record_t;

static kb_percpu_alloc_record_t percpu_alloc_records[256];
enum {
    KB_PERCPU_RWSEM_READ_COUNT_OFFSET = 0x30,
    KB_PERCPU_RWSEM_WRITER_OFFSET = 0x38,
    KB_PERCPU_RWSEM_WAITERS_OFFSET = 0x40,
    KB_PERCPU_RWSEM_BLOCK_OFFSET = 0x58,
};

static const unsigned long kb_rwsem_writer_locked = 1ul << 0;
static const unsigned long kb_rwsem_flag_waiters = 1ul << 1;
static const unsigned long kb_rwsem_flag_handoff = 1ul << 2;
static const unsigned long kb_rwsem_reader_bias = 1ul << 8;
static const unsigned long kb_rwsem_readfail =
    1ul << (sizeof(unsigned long) * 8u - 1u);

static int trace_lock_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_TRACE_LOCK");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    return cached;
}

static unsigned long kb_rwsem_read_failed_mask(void)
{
    return kb_rwsem_writer_locked |
        kb_rwsem_flag_waiters |
        kb_rwsem_flag_handoff |
        kb_rwsem_readfail;
}

static void kb_rwsem_report_long_wait(
    const char *operation,
    void *sem,
    unsigned long state,
    unsigned long attempts,
    void *caller)
{
    if (attempts != 1) {
        return;
    }
    fprintf(stderr,
        "kobox rwsem: long wait op=%s sem=%p state=%#lx caller=%p\n",
        operation,
        sem,
        state,
        caller);
}

void kb_cond_resched(void)
{
}

void *kb_alloc_percpu_gfp(size_t size, size_t align, unsigned int flags)
{
    (void)align;
    (void)flags;
    void *base = calloc(1, size);
    if (base == NULL) {
        return NULL;
    }
    void *relative = base;
    unsigned long kernel_gs = kb_shim_current_kernel_gs();
    if (kernel_gs != 0) {
        relative = (void *)((uintptr_t)base - (uintptr_t)kernel_gs);
    }
    const char *trace_fs = getenv("KOBOX_TRACE_FS");
    if (trace_fs != NULL && trace_fs[0] != '\0' && strcmp(trace_fs, "0") != 0) {
        fprintf(stderr,
            "kobox-core: alloc_percpu size=%zu align=%zu base=%p relative=%p kernel_gs=0x%lx\n",
            size,
            align,
            base,
            relative,
            kernel_gs);
    }
    for (size_t i = 0; i < sizeof(percpu_alloc_records) / sizeof(percpu_alloc_records[0]); i++) {
        if (percpu_alloc_records[i].base == NULL) {
            percpu_alloc_records[i].base = base;
            percpu_alloc_records[i].relative = relative;
            break;
        }
    }
    return relative;
}

void kb_free_percpu(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof(percpu_alloc_records) / sizeof(percpu_alloc_records[0]); i++) {
        if (percpu_alloc_records[i].base == ptr || percpu_alloc_records[i].relative == ptr) {
            void *base = percpu_alloc_records[i].base;
            const char *trace_fs = getenv("KOBOX_TRACE_FS");
            if (trace_fs != NULL && trace_fs[0] != '\0' && strcmp(trace_fs, "0") != 0) {
                fprintf(stderr, "kobox-core: free_percpu ptr=%p base=%p\n", ptr, base);
            }
            memset(&percpu_alloc_records[i], 0, sizeof(percpu_alloc_records[i]));
            free(base);
            return;
        }
    }
}

static void *percpu_allocation_base(void *relative)
{
    if (relative == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(percpu_alloc_records) / sizeof(percpu_alloc_records[0]); i++) {
        if (percpu_alloc_records[i].relative == relative) {
            return percpu_alloc_records[i].base;
        }
    }
    return relative;
}

int kb_percpu_init_rwsem(void *sem, const char *name, void *key)
{
    (void)name;
    (void)key;
    if (sem == NULL) {
        return -22;
    }
    uint32_t *read_count = kb_alloc_percpu_gfp(sizeof(*read_count), _Alignof(uint32_t), 0);
    if (read_count == NULL) {
        return -12;
    }
    memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_READ_COUNT_OFFSET, &read_count, sizeof(read_count));

    void *rss_waiters = (uint8_t *)sem + 0x10;
    memcpy((uint8_t *)sem + 0x10, &rss_waiters, sizeof(rss_waiters));
    memcpy((uint8_t *)sem + 0x18, &rss_waiters, sizeof(rss_waiters));
    void *waiters = (uint8_t *)sem + KB_PERCPU_RWSEM_WAITERS_OFFSET + 8u;
    memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_WAITERS_OFFSET + 8u, &waiters, sizeof(waiters));
    memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_WAITERS_OFFSET + 16u, &waiters, sizeof(waiters));
    memset((uint8_t *)sem + KB_PERCPU_RWSEM_WRITER_OFFSET, 0, sizeof(void *));
    uint32_t block = 0;
    memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_BLOCK_OFFSET, &block, sizeof(block));
    return 0;
}

void kb_percpu_free_rwsem(void *sem)
{
    if (sem == NULL) {
        return;
    }
    void *read_count = NULL;
    memcpy(&read_count, (uint8_t *)sem + KB_PERCPU_RWSEM_READ_COUNT_OFFSET, sizeof(read_count));
    if (read_count != NULL) {
        kb_free_percpu(read_count);
        read_count = NULL;
        memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_READ_COUNT_OFFSET, &read_count, sizeof(read_count));
    }
}

int kb_percpu_down_read(void *sem, int try_lock)
{
    (void)try_lock;
    if (sem == NULL) {
        return 0;
    }
    void *relative = NULL;
    memcpy(&relative, (uint8_t *)sem + KB_PERCPU_RWSEM_READ_COUNT_OFFSET, sizeof(relative));
    uint32_t *read_count = percpu_allocation_base(relative);
    if (read_count == NULL) {
        return 0;
    }
    (*read_count)++;
    return 1;
}

void kb_percpu_up_read(void *sem)
{
    if (sem == NULL) {
        return;
    }
    void *relative = NULL;
    memcpy(&relative, (uint8_t *)sem + KB_PERCPU_RWSEM_READ_COUNT_OFFSET, sizeof(relative));
    uint32_t *read_count = percpu_allocation_base(relative);
    if (read_count != NULL && *read_count != 0) {
        (*read_count)--;
    }
}

int kb_percpu_is_read_locked(void *sem)
{
    if (sem == NULL) {
        return 0;
    }
    void *relative = NULL;
    memcpy(&relative, (uint8_t *)sem + KB_PERCPU_RWSEM_READ_COUNT_OFFSET, sizeof(relative));
    const uint32_t *read_count = percpu_allocation_base(relative);
    uint32_t block = 0;
    memcpy(&block, (uint8_t *)sem + KB_PERCPU_RWSEM_BLOCK_OFFSET, sizeof(block));
    return read_count != NULL && *read_count != 0 && block == 0;
}

void kb_percpu_down_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    uint32_t block = 1;
    memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_BLOCK_OFFSET, &block, sizeof(block));
}

void kb_percpu_up_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    uint32_t block = 0;
    memcpy((uint8_t *)sem + KB_PERCPU_RWSEM_BLOCK_OFFSET, &block, sizeof(block));
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
    return kb_net_device_register(dev);
}

void *kb_alloc_netdev_mqs(
    int sizeof_priv,
    const char *name,
    void (*setup)(void *),
    unsigned int txqs,
    unsigned int rxqs)
{
    return kb_net_device_alloc(sizeof_priv, name, 0, setup, txqs, rxqs);
}

void *kb_alloc_netdev_mqs_modern(
    int sizeof_priv,
    const char *name,
    unsigned char name_assign_type,
    void (*setup)(void *),
    unsigned int txqs,
    unsigned int rxqs)
{
    return kb_net_device_alloc(sizeof_priv, name, name_assign_type, setup, txqs, rxqs);
}

void *kb_alloc_etherdev_mqs_rh(int sizeof_priv, unsigned int txqs, unsigned int rxqs)
{
    return kb_net_device_alloc(sizeof_priv, "eth%d", 0, kb_ether_setup, txqs, rxqs);
}

void kb_free_netdev(void *dev)
{
    kb_net_device_free(dev);
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
    kb_net_device_addr_mod(dev, offset, addr, len);
}

void kb_netif_carrier_on(void *dev)
{
    kb_net_device_set_carrier(dev, 1);
}

void kb_netif_carrier_off(void *dev)
{
    kb_net_device_set_carrier(dev, 0);
}

int kb_dev_open(void *dev)
{
    return kb_net_device_open(dev);
}

int kb_dev_queue_xmit(void *skb)
{
    (void)skb;
    return 0;
}

void *__netdev_alloc_skb(void *dev, unsigned int length, unsigned int gfp)
{
    return kb_netdev_alloc_skb(dev, length, gfp);
}

void *skb_put(void *skb, unsigned int len)
{
    return kb_skb_put(skb, len);
}

int skb_to_sgvec(void *skb, void *sg, int offset, int len)
{
    return kb_skb_to_sgvec(skb, sg, offset, len);
}

int napi_gro_receive(void *napi, void *skb)
{
    return kb_napi_gro_receive(napi, skb);
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
    kb_net_device_consume_skb(skb);
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

void kb_init_rwsem(void *sem)
{
    if (sem == NULL) {
        return;
    }
    __atomic_store_n((unsigned long *)sem, 0ul, __ATOMIC_RELEASE);
    if (trace_lock_enabled()) {
        fprintf(stderr, "kobox lock: init_rwsem sem=%p\n", sem);
    }
}

void kb_down_read(void *sem)
{
    if (sem == NULL) {
        return;
    }
    unsigned long attempts = 0;
    void *caller = __builtin_return_address(0);
    for (;;) {
        unsigned long observed = __atomic_load_n(
            (unsigned long *)sem, __ATOMIC_RELAXED);
        if ((observed & kb_rwsem_read_failed_mask()) == 0 &&
            observed <= ULONG_MAX - kb_rwsem_reader_bias &&
            __atomic_compare_exchange_n(
                (unsigned long *)sem,
                &observed,
                observed + kb_rwsem_reader_bias,
                0,
                __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED))
        {
            if (trace_lock_enabled()) {
                fprintf(stderr, "kobox lock: down_read sem=%p\n", sem);
            }
            return;
        }
        attempts++;
        kb_rwsem_report_long_wait(
            "read", sem, observed, attempts, caller);
        if (!kb_kthread_yield_current()) {
            kb_kthread_run_ready();
        }
    }
}

int kb_down_read_trylock(void *sem)
{
    if (sem == NULL) {
        return 0;
    }
    unsigned long observed = __atomic_load_n(
        (unsigned long *)sem, __ATOMIC_RELAXED);
    return (observed & kb_rwsem_read_failed_mask()) == 0 &&
        observed <= ULONG_MAX - kb_rwsem_reader_bias &&
        __atomic_compare_exchange_n(
            (unsigned long *)sem,
            &observed,
            observed + kb_rwsem_reader_bias,
            0,
            __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED);
}

void kb_up_read(void *sem)
{
    if (sem == NULL) {
        return;
    }
    unsigned long observed = __atomic_load_n(
        (unsigned long *)sem, __ATOMIC_RELAXED);
    while ((observed & ~(kb_rwsem_reader_bias - 1ul)) >=
        kb_rwsem_reader_bias)
    {
        if (__atomic_compare_exchange_n(
                (unsigned long *)sem,
                &observed,
                observed - kb_rwsem_reader_bias,
                0,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED))
        {
            break;
        }
    }
    if (trace_lock_enabled()) {
        fprintf(stderr, "kobox lock: up_read sem=%p\n", sem);
    }
}

void kb_down_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    unsigned long attempts = 0;
    void *caller = __builtin_return_address(0);
    for (;;) {
        unsigned long expected = __atomic_load_n(
            (unsigned long *)sem, __ATOMIC_RELAXED);
        if ((expected & ~(kb_rwsem_flag_waiters |
                          kb_rwsem_flag_handoff)) == 0)
        {
            unsigned long desired = kb_rwsem_writer_locked;
            if (__atomic_compare_exchange_n(
                    (unsigned long *)sem,
                    &expected,
                    desired,
                    0,
                    __ATOMIC_ACQUIRE,
                    __ATOMIC_RELAXED))
            {
                if (trace_lock_enabled()) {
                    fprintf(stderr, "kobox lock: down_write sem=%p\n", sem);
                }
                return;
            }
        } else if ((expected & kb_rwsem_flag_waiters) == 0) {
            (void)__atomic_fetch_or(
                (unsigned long *)sem,
                kb_rwsem_flag_waiters,
                __ATOMIC_ACQ_REL);
            expected |= kb_rwsem_flag_waiters;
        }
        attempts++;
        kb_rwsem_report_long_wait(
            "write", sem, expected, attempts, caller);
        if (!kb_kthread_yield_current()) {
            kb_kthread_run_ready();
        }
    }
}

int kb_down_write_trylock(void *sem)
{
    if (sem == NULL) {
        return 0;
    }
    unsigned long expected = 0;
    return __atomic_compare_exchange_n(
        (unsigned long *)sem,
        &expected,
        kb_rwsem_writer_locked,
        0,
        __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED);
}

void kb_downgrade_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    unsigned long observed = __atomic_load_n(
        (unsigned long *)sem, __ATOMIC_RELAXED);
    while ((observed & kb_rwsem_writer_locked) != 0) {
        unsigned long desired =
            (observed & (kb_rwsem_flag_waiters | kb_rwsem_flag_handoff)) |
            kb_rwsem_reader_bias;
        if (__atomic_compare_exchange_n(
                (unsigned long *)sem,
                &observed,
                desired,
                0,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED))
        {
            break;
        }
    }
}

void kb_up_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    (void)__atomic_fetch_and(
        (unsigned long *)sem,
        ~kb_rwsem_writer_locked,
        __ATOMIC_RELEASE);
    if (trace_lock_enabled()) {
        fprintf(stderr, "kobox lock: up_write sem=%p\n", sem);
    }
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

unsigned long kb_find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
    if (addr == NULL || offset >= size) {
        return size;
    }
    for (unsigned long bit = offset; bit < size; bit++) {
        if ((addr[bit / (sizeof(unsigned long) * 8)] & (1ul << (bit % (sizeof(unsigned long) * 8)))) == 0) {
            return bit;
        }
    }
    return size;
}

unsigned int kb_bitmap_weight(const unsigned long *addr, unsigned int bits)
{
    if (addr == NULL || bits == 0) {
        return 0;
    }

    unsigned int count = 0;
    for (unsigned int bit = 0; bit < bits; bit++) {
        unsigned long word = addr[bit / (sizeof(unsigned long) * 8u)];
        if ((word & (1ul << (bit % (sizeof(unsigned long) * 8u)))) != 0) {
            count++;
        }
    }
    return count;
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
    return kb_alloc_netdev_mqs_modern(sizeof_priv, name, name_assign_type, setup, txqs, rxqs);
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

void __netif_napi_add(void *dev, void *napi, void *poll, int weight)
{
    kb_netif_napi_add(dev, napi, poll, weight);
}

void napi_disable(void *napi)
{
    kb_napi_disable(napi);
}

int napi_schedule_prep(void *napi)
{
    return kb_napi_schedule_prep(napi);
}

int dev_open(void *dev)
{
    return kb_dev_open(dev);
}

int dev_queue_xmit(void *skb)
{
    return kb_dev_queue_xmit(skb);
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

unsigned int __bitmap_weight(const unsigned long *addr, unsigned int bits)
{
    return kb_bitmap_weight(addr, bits);
}
