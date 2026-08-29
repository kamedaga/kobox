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

enum {
    /* One attempt includes a cooperative yield or a full ready-list scan. */
    KB_RWSEM_ORPHAN_GATE_ATTEMPTS = 256,
    KB_RWSEM_LONG_WAIT_REPORT_INTERVAL = 4096,
};

/* Which task holds a write lock, and where it took it.
 *
 * A stalled reader that cannot name the holder forces a rebuild to find out,
 * and it cannot tell a real holder apart from the waiter itself.  The
 * semaphore is Linux's own struct inside the module, so the owner lives in a
 * side table keyed by its address rather than in the lock word. */
enum { KB_RWSEM_OWNER_SLOTS = 256 };

typedef struct kb_rwsem_owner {
    void *sem;
    void *task;
    void *caller;
    const char *site;
} kb_rwsem_owner_t;

/* Module calls reach the shim through one shared trampoline, so the return
 * address is the same for every lock operation and cannot name a call site.
 * A holder that wants to be identifiable leaves its own label instead. */
static const char *kb_rwsem_pending_site;

void kb_rwsem_note_site(const char *site)
{
    kb_rwsem_pending_site = site;
}

static const char *kb_rwsem_take_pending_site(void)
{
    const char *site = kb_rwsem_pending_site;
    kb_rwsem_pending_site = NULL;
    return site;
}

static kb_rwsem_owner_t kb_rwsem_owners[KB_RWSEM_OWNER_SLOTS];

static size_t kb_rwsem_owner_hash(void *sem)
{
    uintptr_t value = (uintptr_t)sem >> 4;
    value *= 0x9e3779b97f4a7c15ull;
    return (size_t)(value >> 56) % KB_RWSEM_OWNER_SLOTS;
}

/* Clearing a slot must not cut a probe chain: a later entry for another
 * semaphore that hashed here would become unreachable, and the table would
 * then answer with a stale owner.  A freed slot is marked, not emptied. */
static void *const kb_rwsem_owner_free = (void *)(uintptr_t)1;

static void kb_rwsem_owner_set(void *sem, void *caller, const char *site)
{
    const size_t start = kb_rwsem_owner_hash(sem);
    void *task = kb_kthread_current_task();
    size_t reusable = KB_RWSEM_OWNER_SLOTS;
    for (size_t probe = 0; probe < KB_RWSEM_OWNER_SLOTS; probe++) {
        const size_t index = (start + probe) % KB_RWSEM_OWNER_SLOTS;
        kb_rwsem_owner_t *slot = &kb_rwsem_owners[index];
        void *held = __atomic_load_n(&slot->sem, __ATOMIC_ACQUIRE);
        if (held == sem) {
            slot->task = task;
            slot->caller = caller;
            slot->site = site;
            return;
        }
        if (held == NULL) {
            if (reusable == KB_RWSEM_OWNER_SLOTS) {
                reusable = index;
            }
            break;
        }
        if (held == kb_rwsem_owner_free && reusable == KB_RWSEM_OWNER_SLOTS) {
            reusable = index;
        }
    }
    if (reusable == KB_RWSEM_OWNER_SLOTS) {
        /* The table is a diagnostic, not a correctness requirement: a full
         * table loses the owner of this lock and nothing else. */
        return;
    }
    kb_rwsem_owner_t *slot = &kb_rwsem_owners[reusable];
    slot->task = task;
    slot->caller = caller;
    slot->site = site;
    __atomic_store_n(&slot->sem, sem, __ATOMIC_RELEASE);
}

/* Read ownership is diagnostic only.  A writer blocked on reader bias must
 * be able to distinguish a live kthread from a recursive lock attempt by the
 * task currently dispatching the filesystem request. */
enum { KB_RWSEM_READER_OWNER_SLOTS = 512 };

typedef struct kb_rwsem_reader_owner {
    void *sem;
    void *task;
    void *caller;
    const char *site;
    unsigned long depth;
} kb_rwsem_reader_owner_t;

static kb_rwsem_reader_owner_t
    kb_rwsem_reader_owners[KB_RWSEM_READER_OWNER_SLOTS];

static void kb_rwsem_reader_owner_set(
    void *sem,
    void *caller,
    const char *site)
{
    void *task = kb_kthread_current_task();
    size_t reusable = KB_RWSEM_READER_OWNER_SLOTS;
    for (size_t index = 0; index < KB_RWSEM_READER_OWNER_SLOTS; index++) {
        kb_rwsem_reader_owner_t *slot = &kb_rwsem_reader_owners[index];
        void *held = __atomic_load_n(&slot->sem, __ATOMIC_ACQUIRE);
        if (held == sem && slot->task == task) {
            slot->caller = caller;
            slot->site = site;
            slot->depth++;
            return;
        }
        if (held == NULL && reusable == KB_RWSEM_READER_OWNER_SLOTS) {
            reusable = index;
        }
    }
    if (reusable == KB_RWSEM_READER_OWNER_SLOTS) {
        return;
    }
    kb_rwsem_reader_owner_t *slot = &kb_rwsem_reader_owners[reusable];
    slot->task = task;
    slot->caller = caller;
    slot->site = site;
    slot->depth = 1;
    __atomic_store_n(&slot->sem, sem, __ATOMIC_RELEASE);
}

static void kb_rwsem_reader_owner_clear(void *sem)
{
    void *task = kb_kthread_current_task();
    for (size_t index = 0; index < KB_RWSEM_READER_OWNER_SLOTS; index++) {
        kb_rwsem_reader_owner_t *slot = &kb_rwsem_reader_owners[index];
        if (__atomic_load_n(&slot->sem, __ATOMIC_ACQUIRE) != sem ||
            slot->task != task)
        {
            continue;
        }
        if (slot->depth > 1) {
            slot->depth--;
            return;
        }
        slot->task = NULL;
        slot->caller = NULL;
        slot->site = NULL;
        slot->depth = 0;
        __atomic_store_n(&slot->sem, NULL, __ATOMIC_RELEASE);
        return;
    }
}

static int kb_rwsem_reader_owner_lookup(
    void *sem,
    void **out_task,
    void **out_caller,
    const char **out_site,
    unsigned long *out_depth)
{
    for (size_t index = 0; index < KB_RWSEM_READER_OWNER_SLOTS; index++) {
        kb_rwsem_reader_owner_t *slot = &kb_rwsem_reader_owners[index];
        if (__atomic_load_n(&slot->sem, __ATOMIC_ACQUIRE) != sem) {
            continue;
        }
        *out_task = slot->task;
        *out_caller = slot->caller;
        *out_site = slot->site;
        *out_depth = slot->depth;
        return 1;
    }
    return 0;
}

static void kb_rwsem_owner_clear(void *sem)
{
    const size_t start = kb_rwsem_owner_hash(sem);
    for (size_t probe = 0; probe < KB_RWSEM_OWNER_SLOTS; probe++) {
        kb_rwsem_owner_t *slot = &kb_rwsem_owners[(start + probe) % KB_RWSEM_OWNER_SLOTS];
        void *held = __atomic_load_n(&slot->sem, __ATOMIC_ACQUIRE);
        if (held == NULL) {
            return;
        }
        if (held != sem) {
            continue;
        }
        slot->task = NULL;
        slot->caller = NULL;
        slot->site = NULL;
        __atomic_store_n(&slot->sem, kb_rwsem_owner_free, __ATOMIC_RELEASE);
        return;
    }
}

static int kb_rwsem_owner_lookup(
    void *sem,
    void **out_task,
    void **out_caller,
    const char **out_site)
{
    const size_t start = kb_rwsem_owner_hash(sem);
    for (size_t probe = 0; probe < KB_RWSEM_OWNER_SLOTS; probe++) {
        kb_rwsem_owner_t *slot = &kb_rwsem_owners[(start + probe) % KB_RWSEM_OWNER_SLOTS];
        void *held = __atomic_load_n(&slot->sem, __ATOMIC_ACQUIRE);
        if (held == NULL) {
            return 0;
        }
        if (held != sem) {
            continue;
        }
        *out_task = slot->task;
        *out_caller = slot->caller;
        *out_site = slot->site;
        return 1;
    }
    return 0;
}

int kb_rwsem_writer_held_by_current(const void *sem)
{
    void *owner_task = NULL;
    void *owner_caller = NULL;
    const char *owner_site = NULL;
    return sem != NULL &&
        kb_rwsem_owner_lookup(
            (void *)sem,
            &owner_task,
            &owner_caller,
            &owner_site) &&
        owner_task == kb_kthread_current_task();
}

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

static unsigned long kb_rwsem_writer_gate_mask(void)
{
    return kb_rwsem_flag_waiters | kb_rwsem_flag_handoff;
}

static int kb_rwsem_has_only_writer_gate(unsigned long state)
{
    const unsigned long gate = state & kb_rwsem_writer_gate_mask();
    return gate != 0 && (state & ~kb_rwsem_writer_gate_mask()) == 0;
}

static const char *kb_rwsem_daemon_name(void)
{
    const char *name = getenv("KOBOX_DAEMON_NAME");
    return name == NULL || name[0] == '\0' ? "(unknown)" : name;
}

static void kb_rwsem_report_long_wait(
    const char *operation,
    void *sem,
    unsigned long state,
    unsigned long attempts,
    void *caller,
    const char *wait_site)
{
    if (attempts < KB_RWSEM_LONG_WAIT_REPORT_INTERVAL ||
        (attempts & (attempts - 1ul)) != 0)
    {
        return;
    }
    char comm[17];
    void *task = kb_kthread_current_task();
    (void)kb_get_task_comm(comm, sizeof(comm), task);
    void *owner_task = NULL;
    void *owner_caller = NULL;
    const char *owner_site = NULL;
    const int owner_known =
        kb_rwsem_owner_lookup(sem, &owner_task, &owner_caller, &owner_site);
    void *reader_task = NULL;
    void *reader_caller = NULL;
    const char *reader_site = NULL;
    unsigned long reader_depth = 0;
    const int reader_known = kb_rwsem_reader_owner_lookup(
        sem,
        &reader_task,
        &reader_caller,
        &reader_site,
        &reader_depth);
    fprintf(stderr,
        "kobox rwsem: long wait op=%s sem=%p state=%#lx attempts=%lu "
        "daemon=%s task=%p comm=%s caller=%p owner=%s owner_task=%p "
        "owner_caller=%p self=%d owner_site=%s wait_site=%s "
        "reader=%s reader_task=%p reader_caller=%p reader_self=%d "
        "reader_depth=%lu reader_site=%s\n",
        operation,
        sem,
        state,
        attempts,
        kb_rwsem_daemon_name(),
        task,
        comm[0] == '\0' ? "(unknown)" : comm,
        caller,
        owner_known ? "yes" : "none",
        owner_task,
        owner_caller,
        owner_known && owner_task == task ? 1 : 0,
        owner_site == NULL ? "(unlabelled)" : owner_site,
        wait_site == NULL ? "(unlabelled)" : wait_site,
        reader_known ? "yes" : "none",
        reader_task,
        reader_caller,
        reader_known && reader_task == task ? 1 : 0,
        reader_depth,
        reader_site == NULL ? "(unlabelled)" : reader_site);
}

static void kb_rwsem_report_orphan_gate(
    void *sem,
    unsigned long state,
    unsigned long attempts,
    void *caller)
{
    char comm[17];
    void *task = kb_kthread_current_task();
    (void)kb_get_task_comm(comm, sizeof(comm), task);
    fprintf(stderr,
        "kobox rwsem: reclaimed orphan writer gate sem=%p state=%#lx "
        "attempts=%lu daemon=%s task=%p comm=%s caller=%p\n",
        sem,
        state,
        attempts,
        kb_rwsem_daemon_name(),
        task,
        comm[0] == '\0' ? "(unknown)" : comm,
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
    unsigned long orphan_gate_attempts = 0;
    unsigned long orphan_gate_state = 0;
    void *caller = __builtin_return_address(0);
    const char *site = kb_rwsem_take_pending_site();
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
            kb_rwsem_reader_owner_set(sem, caller, site);
            if (trace_lock_enabled()) {
                fprintf(stderr, "kobox lock: down_read sem=%p\n", sem);
            }
            return;
        }
        attempts++;
        if (kb_rwsem_has_only_writer_gate(observed)) {
            if (observed == orphan_gate_state) {
                orphan_gate_attempts++;
            } else {
                orphan_gate_state = observed;
                orphan_gate_attempts = 1;
            }
            if (orphan_gate_attempts >= KB_RWSEM_ORPHAN_GATE_ATTEMPTS) {
                unsigned long stale_gate = observed;
                if (__atomic_compare_exchange_n(
                        (unsigned long *)sem,
                        &stale_gate,
                        0ul,
                        0,
                        __ATOMIC_ACQ_REL,
                        __ATOMIC_RELAXED))
                {
                    kb_rwsem_report_orphan_gate(
                        sem, observed, attempts, caller);
                    orphan_gate_attempts = 0;
                    orphan_gate_state = 0;
                    continue;
                }
            }
        } else {
            orphan_gate_attempts = 0;
            orphan_gate_state = 0;
        }
        kb_rwsem_report_long_wait(
            "read", sem, observed, attempts, caller, site);
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
    const char *site = kb_rwsem_take_pending_site();
    unsigned long observed = __atomic_load_n(
        (unsigned long *)sem, __ATOMIC_RELAXED);
    const int acquired =
        (observed & kb_rwsem_read_failed_mask()) == 0 &&
        observed <= ULONG_MAX - kb_rwsem_reader_bias &&
        __atomic_compare_exchange_n(
            (unsigned long *)sem,
            &observed,
            observed + kb_rwsem_reader_bias,
            0,
            __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED);
    if (acquired) {
        kb_rwsem_reader_owner_set(
            sem,
            __builtin_return_address(0),
            site);
    }
    return acquired;
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
            kb_rwsem_reader_owner_clear(sem);
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
    const char *site = kb_rwsem_take_pending_site();
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
                kb_rwsem_owner_set(sem, caller, site);
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
            "write", sem, expected, attempts, caller, site);
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
    if (!__atomic_compare_exchange_n(
            (unsigned long *)sem,
            &expected,
            kb_rwsem_writer_locked,
            0,
            __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED))
    {
        return 0;
    }
    kb_rwsem_owner_set(
        sem,
        __builtin_return_address(0),
        kb_rwsem_take_pending_site());
    return 1;
}

void kb_downgrade_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    /* The writer becomes a reader, so it no longer holds the lock for write. */
    kb_rwsem_owner_clear(sem);
    unsigned long observed = __atomic_load_n(
        (unsigned long *)sem, __ATOMIC_RELAXED);
    while ((observed & kb_rwsem_writer_locked) != 0) {
        /* Preserve a real successor's gate.  If it was orphaned, the last
         * reader leaves a gate-only state that kb_down_read can reclaim. */
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
            kb_rwsem_reader_owner_set(
                sem,
                __builtin_return_address(0),
                "downgrade_write");
            break;
        }
    }
}

void kb_up_write(void *sem)
{
    if (sem == NULL) {
        return;
    }
    /* A queued writer may already own the gate.  Preserve it here; readers
     * reclaim it only if no holder exists and no writer takes it in time. */
    kb_rwsem_owner_clear(sem);
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
