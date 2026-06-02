#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/shim.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

kb_backend_t *kb_shim_current_backend(void);

enum {
    KB_LINUX_HZ = 1000,
    KB_NSEC_PER_USEC = 1000,
    KB_NSEC_PER_MSEC = 1000000,
    KB_NSEC_PER_SEC = 1000000000,
    KB_CONST_UDELAY_SCALE = 0x10c7,
    KB_LINUX_WORK_DATA_OFFSET = 0,
    KB_LINUX_WORK_FUNC_OFFSET = 24,
    KB_LINUX_TASKLET_STATE_OFFSET = 8,
    KB_LINUX_TASKLET_USE_CALLBACK_OFFSET = 20,
    KB_LINUX_TASKLET_CALLBACK_OFFSET = 24,
    KB_LINUX_TASKLET_DATA_OFFSET = 32,
    KB_LINUX_TIMER_ENTRY_PPREV_OFFSET = 8,
    KB_LINUX_TIMER_EXPIRES_OFFSET = 16,
    KB_LINUX_TIMER_FUNCTION_OFFSET = 24,
    KB_DEFERRED_DRAIN_LIMIT = 1024,
    KB_JIFFIES_STORAGE_MAX = 64,
    KB_WORK_STRUCT_PENDING_BIT = 0,
    KB_TASKLET_STATE_SCHED = 0,
};

typedef enum kb_deferred_kind {
    KB_DEFERRED_WORK,
    KB_DEFERRED_TASKLET,
    KB_DEFERRED_TIMER,
} kb_deferred_kind_t;

typedef struct kb_deferred_item {
    kb_deferred_kind_t kind;
    void *object;
    uint64_t due_ns;
    unsigned long kernel_gs;
    struct kb_deferred_item *next;
} kb_deferred_item_t;

static kb_deferred_item_t *deferred_head;
static kb_deferred_item_t *deferred_tail;
static unsigned int draining_deferred_depth;
static uint64_t time_base_ns;
static unsigned long linux_jiffies;
static void *jiffies_storages[KB_JIFFIES_STORAGE_MAX];

static int trace_work_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_WORK");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static uint64_t monotonic_ns(void)
{
    kb_backend_t *backend = kb_shim_current_backend();
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops != NULL && ops->monotonic_ns != NULL) {
        uint64_t now = ops->monotonic_ns(backend);
        if (now != 0) {
            return now;
        }
    }

    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * KB_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static uint64_t elapsed_ns(void)
{
    uint64_t now = monotonic_ns();
    if (time_base_ns == 0 || now < time_base_ns) {
        time_base_ns = now;
        return 0;
    }
    return now - time_base_ns;
}

static void write_timespec64(void *out, uint64_t ns)
{
    if (out == NULL) {
        return;
    }

    int64_t sec = (int64_t)(ns / KB_NSEC_PER_SEC);
    long nsec = (long)(ns % KB_NSEC_PER_SEC);
    memcpy(out, &sec, sizeof(sec));
    memcpy((unsigned char *)out + sizeof(sec), &nsec, sizeof(nsec));
}

static unsigned long elapsed_jiffies(void)
{
    uint64_t jiffies = (elapsed_ns() * KB_LINUX_HZ) / KB_NSEC_PER_SEC;
    if (jiffies > ULONG_MAX) {
        return ULONG_MAX;
    }
    return (unsigned long)jiffies;
}

static void refresh_linux_jiffies(void)
{
    linux_jiffies = elapsed_jiffies();
    for (size_t i = 0; i < KB_JIFFIES_STORAGE_MAX; i++) {
        if (jiffies_storages[i] != NULL) {
            memcpy(jiffies_storages[i], &linux_jiffies, sizeof(linux_jiffies));
        }
    }
}

void kb_register_jiffies_storage(void *storage)
{
    if (storage == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_JIFFIES_STORAGE_MAX; i++) {
        if (jiffies_storages[i] == storage) {
            refresh_linux_jiffies();
            return;
        }
    }
    for (size_t i = 0; i < KB_JIFFIES_STORAGE_MAX; i++) {
        if (jiffies_storages[i] == NULL) {
            jiffies_storages[i] = storage;
            break;
        }
    }
    refresh_linux_jiffies();
}

static void sleep_ns(uint64_t ns)
{
    refresh_linux_jiffies();
    kb_run_deferred_bottom_halves();
    (void)kb_handle_any_irq_no_work(0);
    if (ns == 0) {
        refresh_linux_jiffies();
        return;
    }

#if defined(_WIN32)
    DWORD msecs = (DWORD)((ns + KB_NSEC_PER_MSEC - 1) / KB_NSEC_PER_MSEC);
    Sleep(msecs);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ns / KB_NSEC_PER_SEC);
    req.tv_nsec = (long)(ns % KB_NSEC_PER_SEC);
    while (nanosleep(&req, &req) != 0) {
    }
#endif

    refresh_linux_jiffies();
    kb_run_deferred_bottom_halves();
    (void)kb_handle_any_irq_no_work(0);
    refresh_linux_jiffies();
}

static uint64_t jiffies_to_ns(unsigned long jiffies)
{
    return ((uint64_t)jiffies * KB_NSEC_PER_SEC) / KB_LINUX_HZ;
}

static unsigned long ns_to_jiffies_ceil(uint64_t ns)
{
    if (ns == 0) {
        return 0;
    }
    uint64_t jiffies = ((ns * KB_LINUX_HZ) + KB_NSEC_PER_SEC - 1) / KB_NSEC_PER_SEC;
    if (jiffies == 0) {
        jiffies = 1;
    }
    if (jiffies > ULONG_MAX) {
        return ULONG_MAX;
    }
    return (unsigned long)jiffies;
}

static uintptr_t read_pointer(const void *base, size_t offset)
{
    uintptr_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static void write_pointer(void *base, size_t offset, void *value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static unsigned long read_ulong(const void *base, size_t offset)
{
    unsigned long value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static void write_ulong(void *base, size_t offset, unsigned long value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static int deferred_contains(kb_deferred_kind_t kind, void *object)
{
    for (kb_deferred_item_t *item = deferred_head; item != NULL; item = item->next) {
        if (item->kind == kind && item->object == object) {
            return 1;
        }
    }
    return 0;
}

static kb_deferred_item_t *find_deferred(kb_deferred_kind_t kind, void *object)
{
    for (kb_deferred_item_t *item = deferred_head; item != NULL; item = item->next) {
        if (item->kind == kind && item->object == object) {
            return item;
        }
    }
    return NULL;
}

static void refresh_deferred_tail(void);

static int remove_deferred(kb_deferred_kind_t kind, void *object)
{
    kb_deferred_item_t **cursor = &deferred_head;
    int removed = 0;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == kind && item->object == object) {
            *cursor = item->next;
            free(item);
            removed = 1;
            continue;
        }
        cursor = &item->next;
    }
    refresh_deferred_tail();
    return removed;
}

static int remove_due_deferred(kb_deferred_kind_t kind, void *object)
{
    const uint64_t now_ns = elapsed_ns();
    kb_deferred_item_t **cursor = &deferred_head;
    int removed = 0;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == kind &&
            item->object == object &&
            (item->due_ns == 0 || item->due_ns <= now_ns))
        {
            *cursor = item->next;
            free(item);
            removed = 1;
            continue;
        }
        cursor = &item->next;
    }
    refresh_deferred_tail();
    return removed;
}

static int queue_deferred(kb_deferred_kind_t kind, void *object, uint64_t due_ns)
{
    if (object == NULL) {
        return 0;
    }
    if (deferred_contains(kind, object)) {
        return 0;
    }

    kb_deferred_item_t *item = calloc(1, sizeof(*item));
    if (item == NULL) {
        return 0;
    }
    item->kind = kind;
    item->object = object;
    item->due_ns = due_ns;
    item->kernel_gs = kb_shim_current_kernel_gs();
    if (deferred_tail != NULL) {
        deferred_tail->next = item;
    } else {
        deferred_head = item;
    }
    deferred_tail = item;
    return 1;
}

static void refresh_deferred_tail(void)
{
    deferred_tail = NULL;
    for (kb_deferred_item_t *item = deferred_head; item != NULL; item = item->next) {
        deferred_tail = item;
    }
}

static kb_deferred_item_t *pop_due_deferred(int include_work)
{
    const uint64_t now_ns = elapsed_ns();
    kb_deferred_item_t **cursor = &deferred_head;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if ((include_work || item->kind != KB_DEFERRED_WORK) &&
            (item->due_ns == 0 || item->due_ns <= now_ns))
        {
            *cursor = item->next;
            if (deferred_tail == item) {
                refresh_deferred_tail();
            }
            item->next = NULL;
            return item;
        }
        cursor = &item->next;
    }
    return NULL;
}

static unsigned long read_work_data(void *work)
{
    return read_ulong(work, KB_LINUX_WORK_DATA_OFFSET);
}

static void write_work_data(void *work, unsigned long data)
{
    write_ulong(work, KB_LINUX_WORK_DATA_OFFSET, data);
}

static int work_pending(void *work)
{
    return (read_work_data(work) & (1ul << KB_WORK_STRUCT_PENDING_BIT)) != 0;
}

static void set_work_pending(void *work)
{
    write_work_data(work, read_work_data(work) | (1ul << KB_WORK_STRUCT_PENDING_BIT));
}

static void clear_work_pending(void *work)
{
    write_work_data(work, read_work_data(work) & ~(1ul << KB_WORK_STRUCT_PENDING_BIT));
}

static int is_usb_lpm_work_function(void (*func)(void *))
{
    const unsigned char *code = (const unsigned char *)func;
    if (code == NULL) {
        return 0;
    }

    if (code[0] == 0xf3 && code[1] == 0x0f && code[2] == 0x1e && code[3] == 0xfa) {
        code += 4;
    }
    if (code[0] == 0xe8) {
        code += 5;
    }
    for (unsigned int i = 0; i < 8 && code[0] == 0x90; i++) {
        code++;
    }
    if (code[0] == 0x55) {
        code++;
    }

    const unsigned char sub_rdi_lpm_work[] = { 0x48, 0x81, 0xef, 0x68, 0x01, 0x00, 0x00 };
    if (memcmp(code, sub_rdi_lpm_work, sizeof(sub_rdi_lpm_work)) != 0) {
        return 0;
    }

    code += sizeof(sub_rdi_lpm_work);
    if (code[0] != 0xbe || code[2] != 0x00 || code[3] != 0x00 || code[4] != 0x00) {
        return 0;
    }

    return code[1] >= 1 && code[1] <= 4;
}

static int run_work(void *work)
{
    if (work == NULL) {
        return 0;
    }

    void (*func)(void *) = (void (*)(void *))read_pointer(work, KB_LINUX_WORK_FUNC_OFFSET);
    clear_work_pending(work);
    if (func == NULL) {
        return 0;
    }
    if (is_usb_lpm_work_function(func)) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: skip usb lpm work=%p func=%p\n", work, (void *)func);
        }
        return 1;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: run work=%p func=%p\n", work, (void *)func);
    }
    func(work);
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: done work=%p func=%p\n", work, (void *)func);
    }
    return 1;
}

static int run_timer(void *timer)
{
    if (timer == NULL) {
        return 0;
    }

    void (*callback)(void *) = (void (*)(void *))read_pointer(timer, KB_LINUX_TIMER_FUNCTION_OFFSET);
    if (callback == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: run timer=%p callback=%p\n", timer, (void *)callback);
    }
    callback(timer);
    return 1;
}

static int run_tasklet(void *tasklet)
{
    if (tasklet == NULL) {
        return 0;
    }

    unsigned long state = read_ulong(tasklet, KB_LINUX_TASKLET_STATE_OFFSET);
    state &= ~(1ul << KB_TASKLET_STATE_SCHED);
    write_ulong(tasklet, KB_LINUX_TASKLET_STATE_OFFSET, state);

    unsigned char use_callback = 0;
    memcpy(&use_callback, (unsigned char *)tasklet + KB_LINUX_TASKLET_USE_CALLBACK_OFFSET, sizeof(use_callback));

    void (*callback)(void *) = (void (*)(void *))read_pointer(tasklet, KB_LINUX_TASKLET_CALLBACK_OFFSET);
    if (trace_work_enabled()) {
        unsigned long data = 0;
        memcpy(&data, (unsigned char *)tasklet + KB_LINUX_TASKLET_DATA_OFFSET, sizeof(data));
        fprintf(stderr,
            "kobox work: tasklet=%p state=0x%lx use_callback=%u fn=%p data=0x%lx\n",
            tasklet,
            state,
            (unsigned)use_callback,
            (void *)callback,
            data);
    }
    if (use_callback != 0 && callback != NULL) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: run tasklet=%p callback=%p\n", tasklet, (void *)callback);
        }
        callback(tasklet);
        return 1;
    }

    void (*func)(unsigned long) = (void (*)(unsigned long))read_pointer(tasklet, KB_LINUX_TASKLET_CALLBACK_OFFSET);
    unsigned long data = 0;
    memcpy(&data, (unsigned char *)tasklet + KB_LINUX_TASKLET_DATA_OFFSET, sizeof(data));
    if (func == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: run tasklet=%p func=%p data=0x%lx\n", tasklet, (void *)func, data);
    }
    func(data);
    return 1;
}

static void run_deferred_items(int include_work)
{
    if (draining_deferred_depth >= 8) {
        return;
    }

    draining_deferred_depth++;
    for (unsigned i = 0; i < KB_DEFERRED_DRAIN_LIMIT; i++) {
        kb_deferred_item_t *item = pop_due_deferred(include_work);
        if (item == NULL) {
            break;
        }

        unsigned long old_gs = 0;
        int has_gs = kb_shim_enter_kernel_gs(item->kernel_gs, &old_gs) == 0;
        switch (item->kind) {
        case KB_DEFERRED_WORK:
            (void)run_work(item->object);
            break;
        case KB_DEFERRED_TASKLET:
            (void)run_tasklet(item->object);
            break;
        case KB_DEFERRED_TIMER:
            (void)run_timer(item->object);
            break;
        }
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        free(item);
    }
    draining_deferred_depth--;
}

void kb_run_deferred_work(void)
{
    run_deferred_items(1);
}

void kb_run_deferred_bottom_halves(void)
{
    run_deferred_items(0);
}

int kb_deferred_work_is_draining(void)
{
    return draining_deferred_depth != 0;
}

int kb_queue_work_on(int cpu, void *wq, void *work)
{
    (void)cpu;
    (void)wq;
    if (work == NULL) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: queue_work skip wq=%p work=%p func=%p pending=%d\n",
                wq,
                work,
                NULL,
                0);
        }
        return 0;
    }

    if (work_pending(work)) {
        void *func = (void *)read_pointer(work, KB_LINUX_WORK_FUNC_OFFSET);
        if (deferred_contains(KB_DEFERRED_WORK, work)) {
            if (trace_work_enabled()) {
                fprintf(stderr, "kobox work: queue_work skip wq=%p work=%p func=%p pending=1\n",
                    wq,
                    work,
                    func);
            }
            return 0;
        }
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: queue_work clear stale pending wq=%p work=%p func=%p\n",
                wq,
                work,
                func);
        }
        clear_work_pending(work);
    }

    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: queue_work wq=%p work=%p func=%p\n",
            wq,
            work,
            (void *)read_pointer(work, KB_LINUX_WORK_FUNC_OFFSET));
    }
    set_work_pending(work);
    if (!queue_deferred(KB_DEFERRED_WORK, work, 0)) {
        clear_work_pending(work);
        return 0;
    }
    return 1;
}

int kb_queue_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay)
{
    (void)cpu;
    (void)wq;
    if (dwork == NULL) {
        return 0;
    }
    if (work_pending(dwork)) {
        if (deferred_contains(KB_DEFERRED_WORK, dwork)) {
            return 0;
        }
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: queue_delayed_work clear stale pending wq=%p work=%p func=%p delay=%lu\n",
                wq,
                dwork,
                (void *)read_pointer(dwork, KB_LINUX_WORK_FUNC_OFFSET),
                delay);
        }
        clear_work_pending(dwork);
    }
    set_work_pending(dwork);
    uint64_t due_ns = delay == 0 ? 0 : elapsed_ns() + jiffies_to_ns(delay);
    if (!queue_deferred(KB_DEFERRED_WORK, dwork, due_ns)) {
        clear_work_pending(dwork);
        return 0;
    }
    return 1;
}

int kb_mod_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay)
{
    (void)remove_deferred(KB_DEFERRED_WORK, dwork);
    clear_work_pending(dwork);
    return kb_queue_delayed_work_on(cpu, wq, dwork, delay);
}

int kb_flush_work(void *work)
{
    const int was_pending = work != NULL && work_pending(work);
    const int was_queued = remove_due_deferred(KB_DEFERRED_WORK, work);
    if (!was_pending && !was_queued) {
        return 0;
    }
    if (!was_queued) {
        return 0;
    }
    return run_work(work);
}

int kb_flush_delayed_work(void *dwork)
{
    const int was_pending = dwork != NULL && work_pending(dwork);
    const int was_queued = remove_deferred(KB_DEFERRED_WORK, dwork);
    if (!was_pending && !was_queued) {
        return 0;
    }
    return run_work(dwork);
}

int kb_cancel_work_sync(void *work)
{
    const int was_pending = work != NULL && work_pending(work);
    const int was_queued = remove_deferred(KB_DEFERRED_WORK, work);
    clear_work_pending(work);
    return was_pending || was_queued;
}

int kb_cancel_delayed_work(void *dwork)
{
    return kb_cancel_work_sync(dwork);
}

int kb_cancel_delayed_work_sync(void *dwork)
{
    return kb_cancel_work_sync(dwork);
}

void kb_tasklet_setup(void *tasklet, void (*callback)(void *tasklet))
{
    if (tasklet != NULL) {
        unsigned char use_callback = 1;
        memcpy((unsigned char *)tasklet + KB_LINUX_TASKLET_USE_CALLBACK_OFFSET, &use_callback, sizeof(use_callback));
    }
    write_pointer(tasklet, KB_LINUX_TASKLET_CALLBACK_OFFSET, (void *)callback);
}

void kb_tasklet_schedule(void *tasklet)
{
    unsigned long state = read_ulong(tasklet, KB_LINUX_TASKLET_STATE_OFFSET);
    state |= 1ul << KB_TASKLET_STATE_SCHED;
    write_ulong(tasklet, KB_LINUX_TASKLET_STATE_OFFSET, state);
    (void)queue_deferred(KB_DEFERRED_TASKLET, tasklet, 0);
}

void kb_init_timer_key(void *timer, void (*callback)(void *timer), unsigned int flags, const char *name, void *key)
{
    (void)flags;
    (void)name;
    (void)key;
    write_pointer(timer, KB_LINUX_TIMER_FUNCTION_OFFSET, (void *)callback);
}

int kb_mod_timer(void *timer, unsigned long expires)
{
    if (timer != NULL) {
        memcpy((unsigned char *)timer + KB_LINUX_TIMER_EXPIRES_OFFSET, &expires, sizeof(expires));
        write_pointer(timer, KB_LINUX_TIMER_ENTRY_PPREV_OFFSET, timer);
    }
    const uint64_t due_ns = jiffies_to_ns(expires);
    kb_deferred_item_t *item = find_deferred(KB_DEFERRED_TIMER, timer);
    if (item != NULL) {
        item->due_ns = due_ns;
        item->kernel_gs = kb_shim_current_kernel_gs();
        return 1;
    }
    return queue_deferred(KB_DEFERRED_TIMER, timer, due_ns);
}

void kb_add_timer(void *timer)
{
    write_pointer(timer, KB_LINUX_TIMER_ENTRY_PPREV_OFFSET, timer);
    unsigned long expires = read_ulong(timer, KB_LINUX_TIMER_EXPIRES_OFFSET);
    (void)queue_deferred(KB_DEFERRED_TIMER, timer, jiffies_to_ns(expires));
}

int kb_timer_delete(void *timer)
{
    kb_deferred_item_t **cursor = &deferred_head;
    int removed = 0;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == KB_DEFERRED_TIMER && item->object == timer) {
            *cursor = item->next;
            free(item);
            removed = 1;
            continue;
        }
        cursor = &item->next;
    }
    write_pointer(timer, KB_LINUX_TIMER_ENTRY_PPREV_OFFSET, NULL);
    refresh_deferred_tail();
    return removed;
}

unsigned long kb_schedule_timeout(unsigned long timeout)
{
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: schedule_timeout timeout=%lu\n", timeout);
    }
    if (timeout == ULONG_MAX) {
        kb_run_deferred_bottom_halves();
        (void)kb_handle_any_irq_no_work(1000000ull);
        return timeout;
    }
    uint64_t ns = jiffies_to_ns(timeout);
    const uint64_t max_real_wait_ns = 10ull * KB_NSEC_PER_MSEC;
    if (ns > max_real_wait_ns) {
        ns = max_real_wait_ns;
    }
    sleep_ns(ns);
    return 0;
}

unsigned long kb_msecs_to_jiffies(unsigned int msecs)
{
    return ns_to_jiffies_ceil((uint64_t)msecs * KB_NSEC_PER_MSEC);
}

unsigned long kb_usecs_to_jiffies(unsigned int usecs)
{
    return ns_to_jiffies_ceil((uint64_t)usecs * KB_NSEC_PER_USEC);
}

unsigned int kb_jiffies_to_msecs(unsigned long jiffies)
{
    uint64_t msecs = (jiffies_to_ns(jiffies) + KB_NSEC_PER_MSEC - 1) / KB_NSEC_PER_MSEC;
    return msecs > UINT_MAX ? UINT_MAX : (unsigned int)msecs;
}

unsigned int kb_jiffies_to_usecs(unsigned long jiffies)
{
    uint64_t usecs = (jiffies_to_ns(jiffies) + KB_NSEC_PER_USEC - 1) / KB_NSEC_PER_USEC;
    return usecs > UINT_MAX ? UINT_MAX : (unsigned int)usecs;
}

void kb_jiffies_to_timespec64(unsigned long jiffies, void *timespec64)
{
    write_timespec64(timespec64, jiffies_to_ns(jiffies));
}

int64_t kb_ktime_get(void)
{
    return (int64_t)monotonic_ns();
}

int64_t kb_ktime_get_with_offset(int offset)
{
    (void)offset;
    return kb_ktime_get();
}

uint64_t kb_ktime_get_mono_fast_ns(void)
{
    return monotonic_ns();
}

void kb_ktime_get_raw_ts64(void *timespec64)
{
    write_timespec64(timespec64, monotonic_ns());
}

void kb_ktime_get_real_ts64(void *timespec64)
{
    write_timespec64(timespec64, monotonic_ns());
}

void kb_msleep(unsigned int msecs)
{
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: msleep msecs=%u jiffies=%lu\n", msecs, linux_jiffies);
    }
    sleep_ns((uint64_t)msecs * KB_NSEC_PER_MSEC);
}

void kb_usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
    (void)max;
    (void)state;
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: usleep_range min=%lu jiffies=%lu\n", min, linux_jiffies);
    }
    sleep_ns((uint64_t)min * KB_NSEC_PER_USEC);
}

void kb_udelay(unsigned long usecs)
{
    sleep_ns((uint64_t)usecs * KB_NSEC_PER_USEC);
}

void kb_const_udelay(unsigned long xloops)
{
    unsigned long usecs = xloops / KB_CONST_UDELAY_SCALE;
    if (usecs == 0 && xloops != 0) {
        usecs = 1;
    }
    kb_udelay(usecs);
}

void kb_ndelay(unsigned long nsecs)
{
    sleep_ns(nsecs);
}

int queue_work_on(int cpu, void *wq, void *work)
{
    return kb_queue_work_on(cpu, wq, work);
}

int queue_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay)
{
    return kb_queue_delayed_work_on(cpu, wq, dwork, delay);
}

int mod_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay)
{
    return kb_mod_delayed_work_on(cpu, wq, dwork, delay);
}

int flush_delayed_work(void *dwork)
{
    return kb_flush_delayed_work(dwork);
}

int flush_work(void *work)
{
    return kb_flush_work(work);
}

int cancel_work_sync(void *work)
{
    return kb_cancel_work_sync(work);
}

int cancel_delayed_work(void *dwork)
{
    return kb_cancel_delayed_work(dwork);
}

int cancel_delayed_work_sync(void *dwork)
{
    return kb_cancel_delayed_work_sync(dwork);
}

void tasklet_setup(void *tasklet, void (*callback)(void *tasklet))
{
    kb_tasklet_setup(tasklet, callback);
}

void __tasklet_schedule(void *tasklet)
{
    kb_tasklet_schedule(tasklet);
}

void __tasklet_hi_schedule(void *tasklet)
{
    kb_tasklet_schedule(tasklet);
}

void init_timer_key(void *timer, void (*callback)(void *timer), unsigned int flags, const char *name, void *key)
{
    kb_init_timer_key(timer, callback, flags, name, key);
}

int mod_timer(void *timer, unsigned long expires)
{
    return kb_mod_timer(timer, expires);
}

void add_timer(void *timer)
{
    kb_add_timer(timer);
}

int timer_delete(void *timer)
{
    return kb_timer_delete(timer);
}

int timer_delete_sync(void *timer)
{
    return kb_timer_delete(timer);
}
