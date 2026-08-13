#include "kobox/shim.h"
#include "linux_subsystem/usb/usb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define fprintf(stream, ...) kb_tracef(__VA_ARGS__)

typedef struct kb_rcu_callback {
    void *head;
    void (*callback)(void *head);
    void *free_ptr;
    struct kb_rcu_callback *next;
} kb_rcu_callback_t;

static atomic_uint rcu_readers;
static atomic_flag rcu_callbacks_lock = ATOMIC_FLAG_INIT;
static kb_rcu_callback_t *rcu_callbacks_head;
static kb_rcu_callback_t *rcu_callbacks_tail;

static void rcu_callbacks_acquire(void)
{
    while (atomic_flag_test_and_set_explicit(
        &rcu_callbacks_lock, memory_order_acquire))
    {
    }
}

static void rcu_callbacks_release(void)
{
    atomic_flag_clear_explicit(&rcu_callbacks_lock, memory_order_release);
}

static void rcu_enqueue(void *head, void (*callback)(void *), void *free_ptr)
{
    kb_rcu_callback_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        /* Losing a reclaim callback is safer than freeing before a grace
         * period.  rcu_barrier() will still drain every recorded callback. */
        return;
    }
    record->head = head;
    record->callback = callback;
    record->free_ptr = free_ptr;
    rcu_callbacks_acquire();
    if (rcu_callbacks_tail != NULL) {
        rcu_callbacks_tail->next = record;
    } else {
        rcu_callbacks_head = record;
    }
    rcu_callbacks_tail = record;
    rcu_callbacks_release();
}

static kb_rcu_callback_t *rcu_pop(void)
{
    rcu_callbacks_acquire();
    kb_rcu_callback_t *record = rcu_callbacks_head;
    if (record != NULL) {
        rcu_callbacks_head = record->next;
        if (rcu_callbacks_head == NULL) {
            rcu_callbacks_tail = NULL;
        }
        record->next = NULL;
    }
    rcu_callbacks_release();
    return record;
}

static void rcu_drain_callbacks(void)
{
    if (atomic_load_explicit(&rcu_readers, memory_order_acquire) != 0) {
        return;
    }
    for (;;) {
        kb_rcu_callback_t *record = rcu_pop();
        if (record == NULL) {
            return;
        }
        if (record->callback != NULL) {
            const unsigned long kernel_gs =
                kb_module_kernel_gs_for_address((const void *)record->callback);
            kb_linux_call_void_ptr_gs(
                record->callback,
                record->head,
                kernel_gs);
        } else if (record->free_ptr != NULL) {
            kb_kfree(record->free_ptr);
        }
        free(record);
    }
}

void kb_rcu_read_lock(void)
{
    (void)atomic_fetch_add_explicit(&rcu_readers, 1u, memory_order_acquire);
}

void kb_rcu_read_unlock(void)
{
    unsigned int readers = atomic_load_explicit(&rcu_readers, memory_order_relaxed);
    while (readers != 0 &&
        !atomic_compare_exchange_weak_explicit(
            &rcu_readers,
            &readers,
            readers - 1u,
            memory_order_release,
            memory_order_relaxed))
    {
    }
    if (readers == 1u) {
        rcu_drain_callbacks();
    }
}

void kb_call_rcu(void *head, void (*callback)(void *head))
{
    if (callback == NULL) {
        return;
    }
    rcu_enqueue(head, callback, NULL);
    rcu_drain_callbacks();
}

void kb_kvfree_call_rcu(void *head, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    rcu_enqueue(head, NULL, ptr);
    rcu_drain_callbacks();
}

void kb_synchronize_rcu(void)
{
    while (atomic_load_explicit(&rcu_readers, memory_order_acquire) != 0) {
        if (!kb_kthread_yield_current()) {
            kb_kthread_run_ready();
        }
    }
    rcu_drain_callbacks();
}

void kb_rcu_barrier(void)
{
    for (;;) {
        kb_synchronize_rcu();
        rcu_callbacks_acquire();
        const int empty = rcu_callbacks_head == NULL;
        rcu_callbacks_release();
        if (empty) {
            return;
        }
    }
}

static int trace_work_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
#if defined(__pachaos__)
    cached = 0;
#else
    const char *value = getenv("KOBOX_TRACE_WORK");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
#endif
    return cached;
}

int kb_autoremove_wake_function(void *wq_entry, unsigned mode, int sync, void *key);

static void kb_mutex_set_owner(void *lock, uintptr_t owner)
{
    if (lock == NULL) {
        return;
    }
    __atomic_store_n((uintptr_t *)lock, owner, __ATOMIC_RELEASE);
}

static uintptr_t kb_mutex_owner(void *lock)
{
    uintptr_t owner = 0;
    if (lock != NULL) {
        owner = __atomic_load_n((uintptr_t *)lock, __ATOMIC_ACQUIRE);
    }
    return owner;
}

void kb_mutex_init(void *lock)
{
    kb_mutex_set_owner(lock, 0);
}

void kb_mutex_lock(void *lock)
{
    if (lock == NULL) {
        return;
    }
    for (;;) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(
                (uintptr_t *)lock,
                &expected,
                1u,
                0,
                __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED))
        {
            return;
        }
        if (!kb_kthread_yield_current()) {
            kb_kthread_run_ready();
        }
    }
}

void kb_mutex_unlock(void *lock)
{
    kb_mutex_set_owner(lock, 0);
}

int kb_mutex_trylock(void *lock)
{
    if (lock == NULL) {
        return 0;
    }
    uintptr_t expected = 0;
    return __atomic_compare_exchange_n(
        (uintptr_t *)lock,
        &expected,
        1u,
        0,
        __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED);
}

int kb_mutex_lock_interruptible(void *lock)
{
    kb_mutex_lock(lock);
    return 0;
}

int kb_mutex_lock_killable(void *lock)
{
    kb_mutex_lock(lock);
    return 0;
}

int kb_mutex_is_locked(void *lock)
{
    return kb_mutex_owner(lock) != 0;
}

void kb_complete(void *completion)
{
    if (completion != NULL) {
        uint32_t done = 1;
        memcpy(completion, &done, sizeof(done));
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: complete completion=%p done=%u\n", completion, done);
        }
    }
}

void kb_complete_all(void *completion)
{
    if (completion != NULL) {
        uint32_t done = UINT32_MAX / 2u;
        memcpy(completion, &done, sizeof(done));
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: complete_all completion=%p done=%u\n", completion, done);
        }
    }
}

void kb_init_completion(void *completion)
{
    if (completion != NULL) {
        uint32_t done = 0;
        memcpy(completion, &done, sizeof(done));
    }
}

void kb_init_waitqueue_head(void *wq_head)
{
    if (wq_head == NULL) {
        return;
    }
    void *head = (unsigned char *)wq_head + sizeof(void *);
    memcpy(head, &head, sizeof(head));
    memcpy((unsigned char *)head + sizeof(void *), &head, sizeof(head));
}

void kb_init_swait_queue_head(void *wq_head)
{
    (void)wq_head;
}

enum {
    KB_WAITQUEUE_HEAD_OFFSET = sizeof(void *),
    KB_WAITQUEUE_ENTRY_FLAGS_OFFSET = 0,
    KB_WAITQUEUE_ENTRY_PRIVATE_OFFSET = sizeof(void *),
    KB_WAITQUEUE_ENTRY_FUNC_OFFSET = sizeof(void *) * 2u,
    KB_WAITQUEUE_ENTRY_LIST_OFFSET = sizeof(void *) * 3u,
    KB_WQ_FLAG_EXCLUSIVE = 0x01,
    KB_WQ_FLAG_WOKEN = 0x02,
    KB_WAIT_BIT_KEY_SIZE = sizeof(void *) * 3u,
    KB_VAR_WAITQUEUE_COUNT = 256,
};

typedef struct kb_wait_bit_key {
    void *flags;
    int bit_nr;
    unsigned long timeout;
} kb_wait_bit_key_t;

typedef struct kb_wait_bit_queue_entry {
    kb_wait_bit_key_t key;
    _Alignas(void *) unsigned char wq_entry[sizeof(void *) * 5u];
} kb_wait_bit_queue_entry_t;

_Static_assert(sizeof(kb_wait_bit_key_t) == KB_WAIT_BIT_KEY_SIZE,
    "Linux wait_bit_key layout changed");

typedef struct kb_var_waitqueue_slot {
    _Alignas(void *) unsigned char head[sizeof(void *) * 3u];
} kb_var_waitqueue_slot_t;

static kb_var_waitqueue_slot_t var_waitqueues[KB_VAR_WAITQUEUE_COUNT];
static atomic_flag var_waitqueues_lock = ATOMIC_FLAG_INIT;
static int var_waitqueues_initialized;

static int kb_wait_low_pointer(const void *ptr)
{
    return (uintptr_t)ptr < 4096u || (uintptr_t)ptr >= UINTPTR_MAX - 4095u;
}

static void kb_wait_list_init(void *entry)
{
    if (kb_wait_low_pointer(entry)) {
        return;
    }
    memcpy(entry, &entry, sizeof(entry));
    memcpy((unsigned char *)entry + sizeof(void *), &entry, sizeof(entry));
}

static int kb_wait_list_empty(const void *entry)
{
    if (kb_wait_low_pointer(entry)) {
        return 1;
    }
    void *next = NULL;
    void *prev = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(&prev, (const unsigned char *)entry + sizeof(void *), sizeof(prev));
    return next == entry && prev == entry;
}

static void kb_wait_list_add(void *entry, void *head)
{
    if (kb_wait_low_pointer(entry) || kb_wait_low_pointer(head)) {
        return;
    }
    void *next = NULL;
    memcpy(&next, head, sizeof(next));
    if (kb_wait_low_pointer(next)) {
        next = head;
        kb_wait_list_init(head);
    }
    memcpy(entry, &next, sizeof(next));
    memcpy((unsigned char *)entry + sizeof(void *), &head, sizeof(head));
    memcpy(head, &entry, sizeof(entry));
    memcpy((unsigned char *)next + sizeof(void *), &entry, sizeof(entry));
}

static void kb_wait_list_add_tail(void *entry, void *head)
{
    if (kb_wait_low_pointer(entry) || kb_wait_low_pointer(head)) {
        return;
    }
    void *prev = NULL;
    memcpy(&prev, (const unsigned char *)head + sizeof(void *), sizeof(prev));
    if (kb_wait_low_pointer(prev)) {
        prev = head;
        kb_wait_list_init(head);
    }
    memcpy(entry, &head, sizeof(head));
    memcpy((unsigned char *)entry + sizeof(void *), &prev, sizeof(prev));
    memcpy(prev, &entry, sizeof(entry));
    memcpy((unsigned char *)head + sizeof(void *), &entry, sizeof(entry));
}

static void kb_wait_list_del_init(void *entry)
{
    if (kb_wait_low_pointer(entry) || kb_wait_list_empty(entry)) {
        kb_wait_list_init(entry);
        return;
    }
    void *next = NULL;
    void *prev = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(&prev, (const unsigned char *)entry + sizeof(void *), sizeof(prev));
    if (!kb_wait_low_pointer(next) && !kb_wait_low_pointer(prev)) {
        memcpy(prev, &next, sizeof(next));
        memcpy((unsigned char *)next + sizeof(void *), &prev, sizeof(prev));
    }
    kb_wait_list_init(entry);
}

static void *kb_waitqueue_head_list(void *wq_head)
{
    return kb_wait_low_pointer(wq_head) ? NULL : (unsigned char *)wq_head + KB_WAITQUEUE_HEAD_OFFSET;
}

static void *kb_waitqueue_entry_list(void *wq_entry)
{
    return kb_wait_low_pointer(wq_entry) ? NULL : (unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_LIST_OFFSET;
}

static uint32_t kb_waitqueue_entry_flags(void *wq_entry)
{
    uint32_t flags = 0;
    if (!kb_wait_low_pointer(wq_entry)) {
        memcpy(&flags, (const unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FLAGS_OFFSET, sizeof(flags));
    }
    return flags;
}

static void kb_waitqueue_entry_set_flags(void *wq_entry, uint32_t flags)
{
    if (!kb_wait_low_pointer(wq_entry)) {
        memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FLAGS_OFFSET, &flags, sizeof(flags));
    }
}

static void *kb_waitqueue_entry_task(void *wq_entry)
{
    void *task = NULL;
    if (!kb_wait_low_pointer(wq_entry)) {
        memcpy(
            &task,
            (const unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_PRIVATE_OFFSET,
            sizeof(task));
    }
    return task;
}

static void kb_waitqueue_prepare_task(void *wq_entry)
{
    void *task = kb_waitqueue_entry_task(wq_entry);
    if (task == NULL) {
        task = kb_kthread_current_task();
        if (!kb_wait_low_pointer(wq_entry)) {
            memcpy(
                (unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_PRIVATE_OFFSET,
                &task,
                sizeof(task));
        }
    }
    kb_kthread_prepare_wait(task);
}

void kb_add_wait_queue(void *wq_head, void *wq_entry)
{
    void *head = kb_waitqueue_head_list(wq_head);
    void *entry = kb_waitqueue_entry_list(wq_entry);
    if (head == NULL || entry == NULL) {
        return;
    }
    kb_waitqueue_entry_set_flags(wq_entry, kb_waitqueue_entry_flags(wq_entry) & ~KB_WQ_FLAG_EXCLUSIVE);
    if (kb_wait_list_empty(entry)) {
        kb_wait_list_add(entry, head);
    }
}

void kb_add_wait_queue_exclusive(void *wq_head, void *wq_entry)
{
    void *head = kb_waitqueue_head_list(wq_head);
    void *entry = kb_waitqueue_entry_list(wq_entry);
    if (head == NULL || entry == NULL) {
        return;
    }
    kb_waitqueue_entry_set_flags(wq_entry, kb_waitqueue_entry_flags(wq_entry) | KB_WQ_FLAG_EXCLUSIVE);
    if (kb_wait_list_empty(entry)) {
        kb_wait_list_add_tail(entry, head);
    }
}

void kb_remove_wait_queue(void *wq_head, void *wq_entry)
{
    (void)wq_head;
    kb_wait_list_del_init(kb_waitqueue_entry_list(wq_entry));
}

void kb_prepare_to_wait(void *wq_head, void *wq_entry, int state)
{
    (void)state;
    kb_add_wait_queue(wq_head, wq_entry);
    kb_waitqueue_prepare_task(wq_entry);
}

int kb_prepare_to_wait_exclusive(void *wq_head, void *wq_entry, int state)
{
    (void)state;
    void *head = kb_waitqueue_head_list(wq_head);
    const int was_empty = head == NULL || kb_wait_list_empty(head);
    kb_add_wait_queue_exclusive(wq_head, wq_entry);
    kb_waitqueue_prepare_task(wq_entry);
    return was_empty;
}

long kb_prepare_to_wait_event(void *wq_head, void *wq_entry, int state)
{
    (void)state;
    if ((kb_waitqueue_entry_flags(wq_entry) & KB_WQ_FLAG_EXCLUSIVE) != 0) {
        kb_add_wait_queue_exclusive(wq_head, wq_entry);
    } else {
        kb_add_wait_queue(wq_head, wq_entry);
    }
    kb_waitqueue_prepare_task(wq_entry);
    return 0;
}

void kb_finish_wait(void *wq_head, void *wq_entry)
{
    (void)wq_head;
    kb_kthread_finish_wait(kb_waitqueue_entry_task(wq_entry));
    kb_wait_list_del_init(kb_waitqueue_entry_list(wq_entry));
}

void kb_init_wait_entry(void *wq_entry, int flags)
{
    if (kb_wait_low_pointer(wq_entry)) {
        return;
    }
    uint32_t stored_flags = (uint32_t)flags;
    void *task = kb_kthread_current_task();
    void *func = (void *)(uintptr_t)&kb_autoremove_wake_function;
    memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FLAGS_OFFSET, &stored_flags, sizeof(stored_flags));
    memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_PRIVATE_OFFSET, &task, sizeof(task));
    memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FUNC_OFFSET, &func, sizeof(func));
    kb_wait_list_init((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_LIST_OFFSET);
}

static void kb_var_waitqueues_initialize(void)
{
    if (__atomic_load_n(&var_waitqueues_initialized, __ATOMIC_ACQUIRE)) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(
        &var_waitqueues_lock, memory_order_acquire))
    {
    }
    if (!var_waitqueues_initialized) {
        for (size_t i = 0; i < KB_VAR_WAITQUEUE_COUNT; ++i) {
            kb_init_waitqueue_head(var_waitqueues[i].head);
        }
        __atomic_store_n(&var_waitqueues_initialized, 1, __ATOMIC_RELEASE);
    }
    atomic_flag_clear_explicit(&var_waitqueues_lock, memory_order_release);
}

void *kb_var_waitqueue(void *var)
{
    kb_var_waitqueues_initialize();
    uintptr_t value = (uintptr_t)var;
    value ^= value >> 17u;
    value ^= value >> 9u;
    return var_waitqueues[value & (KB_VAR_WAITQUEUE_COUNT - 1u)].head;
}

void *kb_bit_waitqueue(void *word, int bit)
{
    /* Match the kernel's hashed wait-table contract without allocating a
     * queue per buffer.  Include the bit number so independent buffer_head
     * state bits do not systematically share a queue. */
    uintptr_t key = (uintptr_t)word;
    key ^= (uintptr_t)(unsigned int)bit * UINT64_C(0x9e3779b97f4a7c15);
    return kb_var_waitqueue((void *)key);
}

static int kb_var_wake_function(
    void *wq_entry,
    unsigned mode,
    int sync,
    void *key)
{
    if (kb_wait_low_pointer(wq_entry) || kb_wait_low_pointer(key)) {
        return 0;
    }
    const kb_wait_bit_key_t *waited =
        (const kb_wait_bit_key_t *)((const unsigned char *)wq_entry - KB_WAIT_BIT_KEY_SIZE);
    const kb_wait_bit_key_t *wake = key;
    if (waited->flags != wake->flags || waited->bit_nr != wake->bit_nr) {
        return 0;
    }
    return kb_autoremove_wake_function(wq_entry, mode, sync, key);
}

void kb_init_wait_var_entry(void *wait_entry, void *var, int flags)
{
    if (kb_wait_low_pointer(wait_entry)) {
        return;
    }

    /* Linux wait_bit_queue_entry starts with a three-word wait_bit_key and
     * embeds wait_queue_entry immediately after it.  mbcache passes that
     * embedded entry to prepare_to_wait_event(). */
    kb_wait_bit_key_t *key = wait_entry;
    memset(key, 0, sizeof(*key));
    key->flags = var;
    key->bit_nr = -1;
    void *wq_entry = (unsigned char *)wait_entry + KB_WAIT_BIT_KEY_SIZE;
    kb_init_wait_entry(wq_entry, flags);
    void *func = (void *)(uintptr_t)&kb_var_wake_function;
    memcpy(
        (unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FUNC_OFFSET,
        &func,
        sizeof(func));
}

void kb_wake_up_var(void *var)
{
    if (var == NULL) {
        return;
    }
    kb_wait_bit_key_t key = {
        .flags = var,
        .bit_nr = -1,
        .timeout = 0,
    };
    (void)kb_wake_up_waitqueue(kb_var_waitqueue(var), 3u, 1, &key);
    if (!kb_kthread_yield_current()) {
        kb_kthread_run_ready();
    }
}

int kb_default_wake_function(void *wq_entry, unsigned mode, int sync, void *key)
{
    (void)mode;
    (void)sync;
    (void)key;
    void *task = kb_waitqueue_entry_task(wq_entry);
    return task == NULL ? 0 : kb_wake_up_process(task);
}

int kb_autoremove_wake_function(void *wq_entry, unsigned mode, int sync, void *key)
{
    int result = kb_default_wake_function(wq_entry, mode, sync, key);
    if (result != 0) {
        kb_wait_list_del_init(kb_waitqueue_entry_list(wq_entry));
    }
    return result;
}

long kb_wait_woken(void *wq_entry, unsigned mode, long timeout)
{
    (void)mode;
    uint32_t flags = kb_waitqueue_entry_flags(wq_entry);
    if ((flags & KB_WQ_FLAG_WOKEN) == 0) {
        timeout = (long)kb_schedule_timeout((unsigned long)timeout);
        flags = kb_waitqueue_entry_flags(wq_entry);
    }
    kb_waitqueue_entry_set_flags(wq_entry, flags & ~KB_WQ_FLAG_WOKEN);
    return timeout;
}

int kb_woken_wake_function(void *wq_entry, unsigned mode, int sync, void *key)
{
    kb_waitqueue_entry_set_flags(wq_entry, kb_waitqueue_entry_flags(wq_entry) | KB_WQ_FLAG_WOKEN);
    return kb_default_wake_function(wq_entry, mode, sync, key);
}

int kb_wake_up_waitqueue(void *wq_head, unsigned mode, int nr, void *key)
{
    void *head = kb_waitqueue_head_list(wq_head);
    if (head == NULL) {
        return 0;
    }
    int woken = 0;
    void *entry = NULL;
    memcpy(&entry, head, sizeof(entry));
    while (!kb_wait_low_pointer(entry) && entry != head) {
        void *next = NULL;
        memcpy(&next, entry, sizeof(next));
        void *wq_entry = (unsigned char *)entry - KB_WAITQUEUE_ENTRY_LIST_OFFSET;
        void *func = NULL;
        memcpy(&func, (const unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FUNC_OFFSET, sizeof(func));
        int result = 1;
        if (!kb_wait_low_pointer(func)) {
            int (*wake_fn)(void *, unsigned, int, void *) = (int (*)(void *, unsigned, int, void *))func;
            result = wake_fn(wq_entry, mode, 0, key);
        }
        if (result > 0) {
            woken++;
            if (nr > 0 && woken >= nr) {
                break;
            }
        }
        entry = next;
    }
    return woken;
}

static int kb_wait_bit_test(const kb_wait_bit_key_t *key)
{
    if (key == NULL || key->flags == NULL || key->bit_nr < 0 ||
        key->bit_nr >= (int)(sizeof(unsigned long) * 8u))
    {
        return 0;
    }
    const unsigned long mask = 1ul << (unsigned int)key->bit_nr;
    return (__atomic_load_n((unsigned long *)key->flags, __ATOMIC_ACQUIRE) & mask) != 0;
}

static void kb_init_wait_bit_entry(
    kb_wait_bit_queue_entry_t *entry,
    void *word,
    int bit)
{
    memset(entry, 0, sizeof(*entry));
    entry->key.flags = word;
    entry->key.bit_nr = bit;
    kb_init_wait_entry(entry->wq_entry, 0);
    void *func = (void *)(uintptr_t)&kb_bit_wake_function;
    memcpy(
        entry->wq_entry + KB_WAITQUEUE_ENTRY_FUNC_OFFSET,
        &func,
        sizeof(func));
}

int kb_bit_wait_action(void *key, int mode)
{
    (void)key;
    (void)mode;
    kb_schedule();
    return 0;
}

int kb_bit_wake_function(
    void *wq_entry,
    unsigned mode,
    int sync,
    void *key)
{
    if (kb_wait_low_pointer(wq_entry) || kb_wait_low_pointer(key)) {
        return 0;
    }
    const kb_wait_bit_key_t *waited =
        (const kb_wait_bit_key_t *)((const unsigned char *)wq_entry - KB_WAIT_BIT_KEY_SIZE);
    const kb_wait_bit_key_t *wake = key;
    if (waited->flags != wake->flags || waited->bit_nr != wake->bit_nr ||
        kb_wait_bit_test(wake))
    {
        return 0;
    }
    return kb_autoremove_wake_function(wq_entry, mode, sync, key);
}

int kb_out_of_line_wait_on_bit(
    void *word,
    int bit,
    int (*action)(void *, int),
    unsigned int mode)
{
    if (word == NULL || action == NULL || bit < 0 ||
        bit >= (int)(sizeof(unsigned long) * 8u))
    {
        return -22;
    }

    kb_wait_bit_queue_entry_t entry;
    kb_init_wait_bit_entry(&entry, word, bit);
    void *wq_head = kb_bit_waitqueue(word, bit);
    int result = 0;
    do {
        kb_prepare_to_wait(wq_head, entry.wq_entry, (int)mode);
        if (kb_wait_bit_test(&entry.key)) {
            result = action(&entry.key, (int)mode);
        }
    } while (kb_wait_bit_test(&entry.key) && result == 0);
    kb_finish_wait(wq_head, entry.wq_entry);
    return result;
}

int kb_out_of_line_wait_on_bit_lock(
    void *word,
    int bit,
    int (*action)(void *, int),
    unsigned int mode)
{
    if (word == NULL || action == NULL || bit < 0 ||
        bit >= (int)(sizeof(unsigned long) * 8u))
    {
        return -22;
    }

    kb_wait_bit_queue_entry_t entry;
    kb_init_wait_bit_entry(&entry, word, bit);
    void *wq_head = kb_bit_waitqueue(word, bit);
    const unsigned long mask = 1ul << (unsigned int)bit;
    int result = 0;
    for (;;) {
        (void)kb_prepare_to_wait_exclusive(wq_head, entry.wq_entry, (int)mode);
        if (kb_wait_bit_test(&entry.key)) {
            result = action(&entry.key, (int)mode);
            if (result != 0) {
                kb_finish_wait(wq_head, entry.wq_entry);
            }
        }
        const unsigned long old = __atomic_fetch_or(
            (unsigned long *)word,
            mask,
            __ATOMIC_ACQUIRE);
        if ((old & mask) == 0) {
            if (result == 0) {
                kb_finish_wait(wq_head, entry.wq_entry);
            }
            return 0;
        }
        if (result != 0) {
            return result;
        }
    }
}

void kb_wake_up_bit(void *word, int bit)
{
    if (word == NULL || bit < 0 || bit >= (int)(sizeof(unsigned long) * 8u)) {
        return;
    }
    kb_wait_bit_key_t key = {
        .flags = word,
        .bit_nr = bit,
        .timeout = 0,
    };
    (void)kb_wake_up_waitqueue(kb_bit_waitqueue(word, bit), 3u, 1, &key);
    /* Linux can preempt the caller after making a waiter runnable.  Preserve
     * that progress point in the cooperative runtime: an active kthread
     * yields once, while an outer filed call dispatches ready kthreads.  This
     * does not drain unrelated work or recursively dispatch a kthread. */
    if (!kb_kthread_yield_current()) {
        kb_kthread_run_ready();
    }
}

static void run_wait_progress(void)
{
    if (kb_deferred_work_is_draining()) {
        kb_run_deferred_bottom_halves();
    } else {
        kb_run_deferred_work();
    }
    if (kb_usb_root_hub_poll_needed()) {
        (void)kb_usb_poll_root_hubs();
    }
}

unsigned long kb_wait_for_completion(void *completion)
{
    if (completion == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion completion=%p caller=%p\n",
            completion,
            __builtin_return_address(0));
    }
    for (unsigned i = 0; i < 10000; i++) {
        run_wait_progress();
        (void)kb_handle_any_irq_no_work(0);
        uint32_t done = 0;
        memcpy(&done, completion, sizeof(done));
        if (done != 0) {
            done--;
            memcpy(completion, &done, sizeof(done));
            if (trace_work_enabled()) {
                fprintf(stderr,
                    "kobox work: wait_for_completion done completion=%p remaining=%u caller=%p\n",
                    completion,
                    done,
                    __builtin_return_address(0));
            }
            return 1;
        }
        (void)kb_handle_any_irq_no_work(1000000ull);
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion timeout completion=%p caller=%p\n",
            completion,
            __builtin_return_address(0));
    }
    return 0;
}

unsigned long kb_wait_for_completion_io_timeout(void *completion, unsigned long timeout)
{
    if (completion == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion_timeout completion=%p timeout=%lu caller=%p\n",
            completion,
            timeout,
            __builtin_return_address(0));
    }
    const unsigned long loops = timeout == 0 ? 20 : (timeout > 10000 ? 10000 : timeout);
    for (unsigned long i = 0; i < loops; i++) {
        run_wait_progress();
        (void)kb_handle_any_irq_no_work(0);
        uint32_t done = 0;
        memcpy(&done, completion, sizeof(done));
        if (done != 0) {
            done--;
            memcpy(completion, &done, sizeof(done));
            if (trace_work_enabled()) {
                fprintf(
                    stderr,
                    "kobox work: wait_for_completion_timeout done completion=%p remaining=%u left=%lu caller=%p\n",
                    completion,
                    done,
                    timeout == 0 ? 1 : timeout - i,
                    __builtin_return_address(0));
            }
            return timeout == 0 ? 1 : timeout - i;
        }
        (void)kb_handle_any_irq_no_work(1000000ull);
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion_timeout expired completion=%p timeout=%lu caller=%p\n",
            completion,
            timeout,
            __builtin_return_address(0));
    }
    return 0;
}
