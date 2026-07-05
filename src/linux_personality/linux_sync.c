#include "kobox/shim.h"
#include "linux_subsystem/usb/usb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define fprintf(stream, ...) kb_tracef(__VA_ARGS__)

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
    memcpy(lock, &owner, sizeof(owner));
}

static uintptr_t kb_mutex_owner(void *lock)
{
    uintptr_t owner = 0;
    if (lock != NULL) {
        memcpy(&owner, lock, sizeof(owner));
    }
    return owner;
}

void kb_mutex_init(void *lock)
{
    kb_mutex_set_owner(lock, 0);
}

void kb_mutex_lock(void *lock)
{
    kb_mutex_set_owner(lock, 1);
}

void kb_mutex_unlock(void *lock)
{
    kb_mutex_set_owner(lock, 0);
}

int kb_mutex_trylock(void *lock)
{
    kb_mutex_set_owner(lock, 1);
    return 1;
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
};

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
}

int kb_prepare_to_wait_exclusive(void *wq_head, void *wq_entry, int state)
{
    (void)state;
    void *head = kb_waitqueue_head_list(wq_head);
    const int was_empty = head == NULL || kb_wait_list_empty(head);
    kb_add_wait_queue_exclusive(wq_head, wq_entry);
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
    return 0;
}

void kb_finish_wait(void *wq_head, void *wq_entry)
{
    (void)wq_head;
    kb_wait_list_del_init(kb_waitqueue_entry_list(wq_entry));
}

void kb_init_wait_entry(void *wq_entry, int flags)
{
    if (kb_wait_low_pointer(wq_entry)) {
        return;
    }
    uint32_t stored_flags = (uint32_t)flags;
    void *task = NULL;
    void *func = (void *)(uintptr_t)&kb_autoremove_wake_function;
    memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FLAGS_OFFSET, &stored_flags, sizeof(stored_flags));
    memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_PRIVATE_OFFSET, &task, sizeof(task));
    memcpy((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_FUNC_OFFSET, &func, sizeof(func));
    kb_wait_list_init((unsigned char *)wq_entry + KB_WAITQUEUE_ENTRY_LIST_OFFSET);
}

int kb_default_wake_function(void *wq_entry, unsigned mode, int sync, void *key)
{
    (void)wq_entry;
    (void)mode;
    (void)sync;
    (void)key;
    return 1;
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
