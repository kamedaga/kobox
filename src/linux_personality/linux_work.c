#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/shim.h"
#include "loader/module_context.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define fprintf(stream, ...) kb_tracef(__VA_ARGS__)

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

kb_device_backend_t *kb_shim_current_device_backend(void);

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
    KB_DEFERRED_MAX_DRAIN_DEPTH = 8,
    KB_DEFERRED_LIST_WALK_LIMIT = 65536,
    KB_WORK_CONTEXT_MAX_DEPTH = 16,
    KB_JIFFIES_STORAGE_MAX = 64,
    KB_WORK_STRUCT_PENDING_BIT = 0,
    KB_TASKLET_STATE_SCHED = 0,
};

typedef enum kb_deferred_kind {
    KB_DEFERRED_WORK,
    KB_DEFERRED_TASKLET,
    KB_DEFERRED_TIMER,
} kb_deferred_kind_t;

typedef enum kb_work_run_result {
    KB_WORK_RUN_RETRY = -1,
    KB_WORK_RUN_SKIPPED = 0,
    KB_WORK_RUN_EXECUTED = 1,
} kb_work_run_result_t;

typedef struct kb_deferred_item {
    kb_deferred_kind_t kind;
    void *object;
    void *workqueue;
    uint64_t sequence;
    uint64_t due_ns;
    kb_device_backend_t *backend;
    unsigned long kernel_gs;
    struct kb_deferred_item *next;
} kb_deferred_item_t;

typedef struct kb_workqueue {
    uint64_t magic;
    unsigned int flags;
    int max_active;
    int destroying;
    struct kb_workqueue *next;
} kb_workqueue_t;

#define KB_WORKQUEUE_MAGIC UINT64_C(0x4b42575155455545)

typedef struct kb_deferred_item_block {
    struct kb_deferred_item_block *next;
    size_t capacity;
    size_t used;
    kb_deferred_item_t items[];
} kb_deferred_item_block_t;

static kb_deferred_item_t *deferred_head;
static kb_deferred_item_t *deferred_tail;
static kb_deferred_item_t *deferred_free_list;
static kb_deferred_item_block_t *deferred_blocks;
static kb_workqueue_t *workqueues;
static uint64_t deferred_sequence;
static unsigned int draining_deferred_depth;
static uint64_t time_base_ns;
static unsigned long linux_jiffies;
static void *jiffies_storages[KB_JIFFIES_STORAGE_MAX];
static void *worker_tasks[KB_WORK_CONTEXT_MAX_DEPTH];
static void *active_work_stack[KB_WORK_CONTEXT_MAX_DEPTH];
static unsigned char worker_task_poisoned[KB_WORK_CONTEXT_MAX_DEPTH];
static unsigned int active_work_depth;

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

static uint64_t monotonic_ns(void)
{
    kb_device_backend_t *backend = kb_shim_current_device_backend();
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
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

static uint64_t host_time_ns(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * KB_NSEC_PER_SEC) + (uint64_t)ts.tv_nsec;
}

static int pachaos_backend_active(void)
{
#if defined(__pachaos__)
    if (kb_shim_current_device_backend() != NULL) {
        return 1;
    }
#endif
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *backend = getenv("KOBOX_DEVICE_BACKEND");
    if (backend != NULL && (strcmp(backend, "pachaos") == 0 || strcmp(backend, "pachaos_capsule") == 0)) {
        cached = 1;
        return cached;
    }
    cached = getenv("KOBOX_PACHAOS_DEVICE_CAPSULE") != NULL || getenv("KOBOX_PACHAOS_DEVICE_CATALOG") != NULL;
    return cached;
}

static int trace_work_or_pachaos_enabled(void)
{
    return trace_work_enabled();
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
    if (draining_deferred_depth == 0) {
        kb_run_deferred_bottom_halves();
        (void)kb_handle_any_irq_no_work(0);
    }
    if (ns == 0) {
        refresh_linux_jiffies();
        return;
    }

    if (pachaos_backend_active()) {
        uint64_t start = host_time_ns();
        if (start != 0 && ns <= UINT64_MAX - start) {
            uint64_t deadline = start + ns;
            while (host_time_ns() < deadline) {
            }
        }
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

static void refresh_deferred_tail(void);
static int run_work(void *work, unsigned long fallback_gs);

static void repair_deferred_tail(const char *op)
{
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: repair deferred list op=%s\n", op != NULL ? op : "?");
    }
    deferred_tail = NULL;
    kb_deferred_item_t *item = deferred_head;
    for (unsigned int count = 0; item != NULL; count++) {
        if (count >= KB_DEFERRED_LIST_WALK_LIMIT) {
            if (deferred_tail != NULL) {
                deferred_tail->next = NULL;
            }
            return;
        }
        deferred_tail = item;
        item = item->next;
    }
}

static int deferred_contains(kb_deferred_kind_t kind, void *object)
{
    unsigned int count = 0;
    for (kb_deferred_item_t *item = deferred_head; item != NULL; item = item->next) {
        if (count++ >= KB_DEFERRED_LIST_WALK_LIMIT) {
            repair_deferred_tail("contains");
            return 0;
        }
        if (item->kind == kind && item->object == object) {
            return 1;
        }
    }
    return 0;
}

static int work_is_active(const void *work)
{
    for (unsigned int depth = 0; depth < active_work_depth; depth++) {
        if (active_work_stack[depth] == work) {
            return 1;
        }
    }
    return 0;
}

static kb_deferred_item_t *find_deferred(kb_deferred_kind_t kind, void *object)
{
    unsigned int count = 0;
    for (kb_deferred_item_t *item = deferred_head; item != NULL; item = item->next) {
        if (count++ >= KB_DEFERRED_LIST_WALK_LIMIT) {
            repair_deferred_tail("find");
            return NULL;
        }
        if (item->kind == kind && item->object == object) {
            return item;
        }
    }
    return NULL;
}

static kb_deferred_item_block_t *allocate_deferred_block(void)
{
    enum {
        KB_DEFERRED_BLOCK_BYTES = 4096,
    };

    size_t header_size = sizeof(kb_deferred_item_block_t);
    if (header_size >= KB_DEFERRED_BLOCK_BYTES) {
        return NULL;
    }
#if defined(_WIN32)
    kb_deferred_item_block_t *block = calloc(1, KB_DEFERRED_BLOCK_BYTES);
#else
    void *memory = mmap(
        NULL,
        KB_DEFERRED_BLOCK_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    kb_deferred_item_block_t *block = memory == MAP_FAILED ? NULL : (kb_deferred_item_block_t *)memory;
#endif
    if (block == NULL) {
        return NULL;
    }
    block->capacity = (KB_DEFERRED_BLOCK_BYTES - header_size) / sizeof(kb_deferred_item_t);
    if (block->capacity == 0) {
        return NULL;
    }
    block->next = deferred_blocks;
    deferred_blocks = block;
    return block;
}

static kb_deferred_item_t *allocate_deferred_item(void)
{
    kb_deferred_item_t *item = deferred_free_list;
    if (item != NULL) {
        deferred_free_list = item->next;
        memset(item, 0, sizeof(*item));
        return item;
    }
    kb_deferred_item_block_t *block = deferred_blocks;
    if (block == NULL || block->used >= block->capacity) {
        block = allocate_deferred_block();
    }
    if (block == NULL || block->used >= block->capacity) {
        return NULL;
    }
    item = &block->items[block->used++];
    memset(item, 0, sizeof(*item));
    return item;
}

static void release_deferred_item(kb_deferred_item_t *item)
{
    if (item == NULL) {
        return;
    }
    memset(item, 0, sizeof(*item));
    item->next = deferred_free_list;
    deferred_free_list = item;
}

static void prepend_deferred_item(kb_deferred_item_t *item)
{
    if (item == NULL) {
        return;
    }
    item->next = deferred_head;
    deferred_head = item;
    if (deferred_tail == NULL) {
        deferred_tail = item;
    }
}

static int remove_deferred(kb_deferred_kind_t kind, void *object)
{
    kb_deferred_item_t **cursor = &deferred_head;
    int removed = 0;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == kind && item->object == object) {
            *cursor = item->next;
            release_deferred_item(item);
            removed = 1;
            continue;
        }
        cursor = &item->next;
    }
    refresh_deferred_tail();
    return removed;
}

static kb_deferred_item_t *take_deferred_work(void *object, int require_due)
{
    if (object == NULL || work_is_active(object)) {
        return NULL;
    }
    const uint64_t now_ns = elapsed_ns();
    kb_deferred_item_t **cursor = &deferred_head;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == KB_DEFERRED_WORK &&
            item->object == object &&
            (!require_due || item->due_ns == 0 || item->due_ns <= now_ns))
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

static int queue_deferred(kb_deferred_kind_t kind, void *object, void *workqueue, uint64_t due_ns)
{
    if (object == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(
            stderr,
            "kobox work: queue_deferred begin kind=%u object=%p due_ns=%llu head=%p tail=%p free=%p\n",
            (unsigned)kind,
            object,
            (unsigned long long)due_ns,
            (void *)deferred_head,
            (void *)deferred_tail,
            (void *)deferred_free_list);
    }
    if (deferred_contains(kind, object)) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: queue_deferred duplicate kind=%u object=%p\n", (unsigned)kind, object);
        }
        return 0;
    }

    kb_deferred_item_t *item = allocate_deferred_item();
    if (item == NULL) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: queue_deferred alloc failed kind=%u object=%p\n", (unsigned)kind, object);
        }
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: queue_deferred item=%p kind=%u object=%p\n", (void *)item, (unsigned)kind, object);
    }
    item->kind = kind;
    item->object = object;
    item->workqueue = workqueue;
    deferred_sequence++;
    if (deferred_sequence == 0) {
        deferred_sequence++;
    }
    item->sequence = deferred_sequence;
    item->due_ns = due_ns;
    item->backend = kb_shim_current_device_backend();
    item->kernel_gs = kb_shim_current_kernel_gs();
    refresh_deferred_tail();
    if (deferred_tail != NULL) {
        deferred_tail->next = item;
    } else {
        deferred_head = item;
    }
    deferred_tail = item;
    if (trace_work_enabled()) {
        fprintf(
            stderr,
            "kobox work: queue_deferred linked kind=%u object=%p head=%p tail=%p\n",
            (unsigned)kind,
            object,
            (void *)deferred_head,
            (void *)deferred_tail);
    }
    return 1;
}

static void refresh_deferred_tail(void)
{
    deferred_tail = NULL;
    kb_deferred_item_t *item = deferred_head;
    for (unsigned int count = 0; item != NULL; count++) {
        if (count >= KB_DEFERRED_LIST_WALK_LIMIT) {
            if (deferred_tail != NULL) {
                deferred_tail->next = NULL;
            }
            if (trace_work_enabled()) {
                fprintf(stderr, "kobox work: truncated deferred list while refreshing tail\n");
            }
            return;
        }
        deferred_tail = item;
        item = item->next;
    }
}

static kb_deferred_item_t *pop_due_deferred_at(int include_work, uint64_t now_ns)
{
    kb_deferred_item_t **cursor = &deferred_head;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if ((include_work || item->kind != KB_DEFERRED_WORK) &&
            (item->kind != KB_DEFERRED_WORK || !work_is_active(item->object)) &&
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

static kb_deferred_item_t *pop_due_deferred(int include_work)
{
    return pop_due_deferred_at(include_work, elapsed_ns());
}

static kb_deferred_item_t *pop_workqueue_deferred(void *workqueue, uint64_t through_sequence)
{
    kb_deferred_item_t **cursor = &deferred_head;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == KB_DEFERRED_WORK &&
            item->workqueue == workqueue &&
            !work_is_active(item->object) &&
            item->sequence <= through_sequence)
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

static kb_workqueue_t *find_workqueue(void *workqueue)
{
    for (kb_workqueue_t *record = workqueues; record != NULL; record = record->next) {
        if ((void *)record == workqueue && record->magic == KB_WORKQUEUE_MAGIC) {
            return record;
        }
    }
    return NULL;
}

void *kb_alloc_workqueue(const char *name, unsigned int flags, int max_active, ...)
{
    (void)name;
    kb_workqueue_t *record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->magic = KB_WORKQUEUE_MAGIC;
    record->flags = flags;
    record->max_active = max_active;
    record->next = workqueues;
    workqueues = record;
    return record;
}

void kb_flush_workqueue(void *workqueue)
{
    /*
     * Linux flushes the work items queued before the flush began.  Work
     * callbacks may enqueue another generation without making this call
     * spin forever, so retain the same boundary in the cooperative runner.
     * Delayed work in that generation is made runnable by an explicit flush.
     */
    const uint64_t through_sequence = deferred_sequence;
    if (draining_deferred_depth >= 8) {
        return;
    }
    draining_deferred_depth++;
    for (unsigned int count = 0; count < KB_DEFERRED_DRAIN_LIMIT; count++) {
        kb_deferred_item_t *item = pop_workqueue_deferred(workqueue, through_sequence);
        if (item == NULL) {
            break;
        }
        kb_device_backend_t *old_backend = kb_shim_current_device_backend();
        kb_shim_set_device_backend(item->backend);
        refresh_linux_jiffies();
        const int run_result = run_work(item->object, item->kernel_gs);
        refresh_linux_jiffies();
        kb_shim_set_device_backend(old_backend);
        if (run_result == KB_WORK_RUN_RETRY) {
            prepend_deferred_item(item);
            break;
        }
        release_deferred_item(item);
    }
    draining_deferred_depth--;
}

void kb_destroy_workqueue(void *workqueue)
{
    kb_workqueue_t **cursor = &workqueues;
    while (*cursor != NULL && (void *)*cursor != workqueue) {
        cursor = &(*cursor)->next;
    }
    kb_workqueue_t *record = *cursor;
    if (record == NULL || record->magic != KB_WORKQUEUE_MAGIC) {
        return;
    }

    /* Reject requeue from callbacks while draining the final generation. */
    record->destroying = 1;
    kb_flush_workqueue(workqueue);
    for (kb_deferred_item_t *item = deferred_head; item != NULL; item = item->next) {
        if (item->kind == KB_DEFERRED_WORK && item->workqueue == workqueue) {
            record->destroying = 0;
            if (trace_work_enabled()) {
                fprintf(stderr, "kobox work: destroy left queued work wq=%p\n", workqueue);
            }
            return;
        }
    }
    *cursor = record->next;
    record->magic = 0;
    kb_kfree(record);
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

static unsigned long callback_kernel_gs(const void *callback, unsigned long fallback_gs)
{
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(callback);
    if (kernel_gs == 0) {
        kernel_gs = fallback_gs;
    }
    return kernel_gs;
}

static int enter_callback_gs(unsigned long kernel_gs, unsigned long *old_gs)
{
    if (kernel_gs == 0) {
        return 0;
    }
    return kb_shim_enter_kernel_gs(kernel_gs, old_gs) == 0;
}

static int is_usb_root_hub_poll_function(void (*func)(void *))
{
    void *poll_rh_status = kb_module_lookup_exported_symbol("usb_hcd_poll_rh_status");
    return poll_rh_status != NULL && (const void *)func == poll_rh_status;
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

static const unsigned char *skip_linux_function_entry_prefix(const unsigned char *code)
{
    if (code == NULL) {
        return NULL;
    }
    for (unsigned int pass = 0; pass < 3; pass++) {
        for (unsigned int i = 0; i < 16 && code[0] == 0x90; i++) {
            code++;
        }
        if (code[0] == 0xf3 && code[1] == 0x0f && code[2] == 0x1e && code[3] == 0xfa) {
            code += 4;
            continue;
        }
        if (code[0] == 0xe8) {
            code += 5;
            continue;
        }
        break;
    }
    return code;
}

static int is_obviously_not_work_function(void (*func)(void *))
{
    const unsigned char *entry = (const unsigned char *)func;
    if (entry == NULL) {
        return 1;
    }

    /*
     * A work callback has one argument in %rdi. xHCI can pass non-work
     * objects through delayed-work-looking paths while updating command
     * timers; those functions immediately consume %rsi as a second
     * argument. Treating them as work_structs corrupts the object by
     * setting the pending bit at offset 0.
     */
    const unsigned char test_rsi_rsi[] = { 0x48, 0x85, 0xf6 };
    if (memcmp(entry + 16u + 4u + 5u, test_rsi_rsi, sizeof(test_rsi_rsi)) == 0) {
        return 1;
    }

    const unsigned char *code = skip_linux_function_entry_prefix(entry);
    if (code == NULL) {
        return 1;
    }
    return memcmp(code, test_rsi_rsi, sizeof(test_rsi_rsi)) == 0;
}

static int work_function_usable(void *work, void (*func)(void *), const char *op)
{
    if (func == NULL) {
        return 0;
    }
    if (!kb_module_is_executable_address((const void *)func)) {
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: skip invalid %s work=%p func=%p\n", op, work, (void *)func);
        }
        return 0;
    }
    if (is_obviously_not_work_function(func)) {
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: skip non-work %s work=%p func=%p\n", op, work, (void *)func);
        }
        return 0;
    }
    return 1;
}

static int run_work(void *work, unsigned long fallback_gs)
{
    if (work == NULL) {
        return 0;
    }

    void (*func)(void *) = (void (*)(void *))read_pointer(work, KB_LINUX_WORK_FUNC_OFFSET);
    if (!work_function_usable(work, func, "run")) {
        clear_work_pending(work);
        return 0;
    }
    if (work_is_active(work)) {
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: defer active work=%p func=%p\n", work, (void *)func);
        }
        return KB_WORK_RUN_RETRY;
    }
    if (active_work_depth >= KB_WORK_CONTEXT_MAX_DEPTH) {
        fprintf(
            stderr,
            "kobox work: execution context depth exceeded work=%p func=%p depth=%u\n",
            work,
            (void *)func,
            active_work_depth);
        return KB_WORK_RUN_RETRY;
    }
    const unsigned int context_depth = active_work_depth;
    if (worker_task_poisoned[context_depth]) {
        fprintf(
            stderr,
            "kobox work: refusing poisoned execution context work=%p func=%p depth=%u\n",
            work,
            (void *)func,
            context_depth);
        return KB_WORK_RUN_RETRY;
    }
    if (worker_tasks[context_depth] == NULL) {
        worker_tasks[context_depth] = kb_loader_clone_execution_task();
    }
    void *worker_task = worker_tasks[context_depth];
    if (worker_task == NULL) {
        fprintf(stderr, "kobox work: execution task allocation failed depth=%u\n", context_depth);
        return KB_WORK_RUN_RETRY;
    }
    void *worker_journal_before = kb_loader_task_journal_info(worker_task);
    if (worker_journal_before != NULL) {
        worker_task_poisoned[context_depth] = 1;
        fprintf(
            stderr,
            "kobox work: dirty execution task before callback task=%p journal_info=%p "
            "work=%p func=%p depth=%u\n",
            worker_task,
            worker_journal_before,
            work,
            (void *)func,
            context_depth);
        return KB_WORK_RUN_RETRY;
    }

    clear_work_pending(work);
    if (is_usb_lpm_work_function(func)) {
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: skip usb lpm work=%p func=%p\n", work, (void *)func);
        }
        return 1;
    }
    if (trace_work_or_pachaos_enabled()) {
        fprintf(stderr, "kobox work: run work=%p func=%p\n", work, (void *)func);
    }
    if (!kb_usb_root_hub_poll_needed() && is_usb_root_hub_poll_function(func)) {
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: skip paused root hub poll work=%p func=%p\n", work, (void *)func);
        }
        return 1;
    }
    unsigned long kernel_gs = callback_kernel_gs((const void *)func, fallback_gs);
    void *previous_task = kb_loader_current_task();
    void *previous_journal = kb_loader_task_journal_info(previous_task);
    kb_module_t *previous_module = kb_loader_active_module();
    kb_module_t *callback_module = kb_module_find_owner_for_address((const void *)func);
    active_work_stack[context_depth] = work;
    active_work_depth++;
    kb_loader_set_current_task_for_all_modules(worker_task);
    kb_loader_set_active_module(callback_module);
    unsigned long old_gs = 0;
    int has_gs = enter_callback_gs(kernel_gs, &old_gs);
    if (kernel_gs != 0 && !has_gs) {
        kb_loader_set_active_module(previous_module);
        kb_loader_set_current_task_for_all_modules(previous_task);
        active_work_depth--;
        active_work_stack[context_depth] = NULL;
        set_work_pending(work);
        fprintf(
            stderr,
            "kobox work: callback GS switch failed work=%p func=%p gs=0x%lx depth=%u\n",
            work,
            (void *)func,
            kernel_gs,
            context_depth);
        return KB_WORK_RUN_RETRY;
    }
    kb_linux_call_void_ptr_gs(func, work, kernel_gs);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    void *worker_journal_after = kb_loader_task_journal_info(worker_task);
    kb_loader_set_active_module(previous_module);
    kb_loader_set_current_task_for_all_modules(previous_task);
    active_work_depth--;
    active_work_stack[context_depth] = NULL;
    void *previous_journal_after = kb_loader_task_journal_info(previous_task);
    if (worker_journal_after != NULL || previous_journal_after != previous_journal) {
        worker_task_poisoned[context_depth] = 1;
        fprintf(
            stderr,
            "kobox work: execution task invariant failed task=%p journal_before=%p "
            "journal_after=%p outer_task=%p outer_before=%p outer_after=%p "
            "work=%p func=%p depth=%u\n",
            worker_task,
            worker_journal_before,
            worker_journal_after,
            previous_task,
            previous_journal,
            previous_journal_after,
            work,
            (void *)func,
            context_depth);
        return KB_WORK_RUN_EXECUTED;
    }
    if (trace_work_or_pachaos_enabled()) {
        fprintf(stderr, "kobox work: done work=%p func=%p\n", work, (void *)func);
    }
    return KB_WORK_RUN_EXECUTED;
}

static int run_timer(void *timer, unsigned long fallback_gs)
{
    if (timer == NULL) {
        return 0;
    }

    void (*callback)(void *) = (void (*)(void *))read_pointer(timer, KB_LINUX_TIMER_FUNCTION_OFFSET);
    if (callback == NULL) {
        return 0;
    }
    if (!kb_module_is_executable_address((const void *)callback)) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: skip invalid timer=%p callback=%p\n", timer, (void *)callback);
        }
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: run timer=%p callback=%p\n", timer, (void *)callback);
    }
    unsigned long kernel_gs = callback_kernel_gs((const void *)callback, fallback_gs);
    unsigned long old_gs = 0;
    int has_gs = enter_callback_gs(kernel_gs, &old_gs);
    kb_linux_call_void_ptr_gs(callback, timer, kernel_gs);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return 1;
}

static int run_tasklet(void *tasklet, unsigned long fallback_gs)
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
        if (!kb_module_is_executable_address((const void *)callback)) {
            if (trace_work_enabled()) {
                fprintf(stderr, "kobox work: skip invalid tasklet=%p callback=%p\n", tasklet, (void *)callback);
            }
            return 0;
        }
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: run tasklet=%p callback=%p\n", tasklet, (void *)callback);
        }
        unsigned long kernel_gs = callback_kernel_gs((const void *)callback, fallback_gs);
        unsigned long old_gs = 0;
        int has_gs = enter_callback_gs(kernel_gs, &old_gs);
        kb_linux_call_void_ptr_gs(callback, tasklet, kernel_gs);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        return 1;
    }

    void (*func)(unsigned long) = (void (*)(unsigned long))read_pointer(tasklet, KB_LINUX_TASKLET_CALLBACK_OFFSET);
    unsigned long data = 0;
    memcpy(&data, (unsigned char *)tasklet + KB_LINUX_TASKLET_DATA_OFFSET, sizeof(data));
    if (func == NULL) {
        return 0;
    }
    if (!kb_module_is_executable_address((const void *)func)) {
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: skip invalid tasklet=%p func=%p\n", tasklet, (void *)func);
        }
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr, "kobox work: run tasklet=%p func=%p data=0x%lx\n", tasklet, (void *)func, data);
    }
    unsigned long kernel_gs = callback_kernel_gs((const void *)func, fallback_gs);
    unsigned long old_gs = 0;
    int has_gs = enter_callback_gs(kernel_gs, &old_gs);
    kb_linux_call_void_ulong_gs(func, data, kernel_gs);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return 1;
}

static void run_deferred_items_body(int include_work, int has_initial_now, uint64_t initial_now)
{
    for (unsigned i = 0; i < KB_DEFERRED_DRAIN_LIMIT; i++) {
        kb_deferred_item_t *item = has_initial_now ?
            pop_due_deferred_at(include_work, initial_now) :
            pop_due_deferred(include_work);
        has_initial_now = 0;
        if (item == NULL) {
            break;
        }

        kb_device_backend_t *old_backend = kb_shim_current_device_backend();
        kb_shim_set_device_backend(item->backend);
        refresh_linux_jiffies();
        int run_result = KB_WORK_RUN_EXECUTED;
        switch (item->kind) {
        case KB_DEFERRED_WORK:
            run_result = run_work(item->object, item->kernel_gs);
            break;
        case KB_DEFERRED_TASKLET:
            (void)run_tasklet(item->object, item->kernel_gs);
            break;
        case KB_DEFERRED_TIMER:
            (void)run_timer(item->object, item->kernel_gs);
            break;
        }
        refresh_linux_jiffies();
        kb_shim_set_device_backend(old_backend);
        if (run_result == KB_WORK_RUN_RETRY) {
            prepend_deferred_item(item);
            break;
        }
        release_deferred_item(item);
    }
}

static void run_deferred_items(int include_work)
{
    if (draining_deferred_depth >= KB_DEFERRED_MAX_DRAIN_DEPTH) {
        return;
    }

    draining_deferred_depth++;
    run_deferred_items_body(include_work, 0, 0);
    draining_deferred_depth--;
}

void kb_run_deferred_work(void)
{
    if (kb_kthread_yield_current()) {
        return;
    }
    const int no_deferred = deferred_head == NULL;
    const int has_runnable = no_deferred ? kb_kthread_has_runnable() : 0;
    if (no_deferred && !has_runnable) {
        /*
         * Preserve the empty full-path ordering around elapsed_ns(): the
         * depth is observable to a reentrant clock backend, and that backend
         * can make deferred work or a kthread runnable.  Reuse this first
         * timestamp if either state changed instead of taking it twice.
         */
        if (draining_deferred_depth < KB_DEFERRED_MAX_DRAIN_DEPTH) {
            draining_deferred_depth++;
            const uint64_t now_ns = elapsed_ns();
            const int recheck_deferred = deferred_head != NULL;
            const int recheck_runnable = recheck_deferred ? 0 : kb_kthread_has_runnable();
            if (recheck_deferred || recheck_runnable) {
                run_deferred_items_body(1, 1, now_ns);
                draining_deferred_depth--;
                kb_kthread_run_ready();
                return;
            }
            draining_deferred_depth--;
        }
        return;
    }
    kb_kthread_run_ready();
    run_deferred_items(1);
    kb_kthread_run_ready();
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
    kb_workqueue_t *record = find_workqueue(wq);
    if (record != NULL && record->destroying) {
        return 0;
    }
    if (work == NULL) {
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: queue_work skip wq=%p work=%p func=%p pending=%d\n",
                wq,
                work,
                NULL,
                0);
        }
        return 0;
    }

    void (*func)(void *) = (void (*)(void *))read_pointer(work, KB_LINUX_WORK_FUNC_OFFSET);
    if (!work_function_usable(work, func, "queue")) {
        clear_work_pending(work);
        return 0;
    }

    if (work_pending(work)) {
        if (deferred_contains(KB_DEFERRED_WORK, work)) {
            if (trace_work_or_pachaos_enabled()) {
                fprintf(stderr, "kobox work: queue_work skip wq=%p work=%p func=%p pending=1\n",
                    wq,
                    work,
                    func);
            }
            return 0;
        }
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: queue_work clear stale pending wq=%p work=%p func=%p\n",
                wq,
                work,
                func);
        }
        clear_work_pending(work);
    }

    if (trace_work_or_pachaos_enabled()) {
        fprintf(stderr, "kobox work: queue_work wq=%p work=%p func=%p\n",
            wq,
            work,
            (void *)func);
    }
    set_work_pending(work);
    if (!queue_deferred(KB_DEFERRED_WORK, work, wq, 0)) {
        clear_work_pending(work);
        return 0;
    }
    return 1;
}

int kb_kblockd_schedule_work(void *work)
{
    return kb_queue_work_on(-1, NULL, work);
}

int kb_queue_delayed_work_on(int cpu, void *wq, void *dwork, unsigned long delay)
{
    (void)cpu;
    kb_workqueue_t *record = find_workqueue(wq);
    if (record != NULL && record->destroying) {
        return 0;
    }
    if (dwork == NULL) {
        return 0;
    }
    void (*func)(void *) = (void (*)(void *))read_pointer(dwork, KB_LINUX_WORK_FUNC_OFFSET);
    if (!work_function_usable(dwork, func, "delayed")) {
        clear_work_pending(dwork);
        return 0;
    }
    if (work_pending(dwork)) {
        if (deferred_contains(KB_DEFERRED_WORK, dwork)) {
            return 0;
        }
        if (trace_work_or_pachaos_enabled()) {
            fprintf(stderr, "kobox work: queue_delayed_work clear stale pending wq=%p work=%p func=%p delay=%lu\n",
                wq,
                dwork,
                (void *)func,
                delay);
        }
        clear_work_pending(dwork);
    }
    set_work_pending(dwork);
    uint64_t due_ns = delay == 0 ? 0 : elapsed_ns() + jiffies_to_ns(delay);
    if (trace_work_or_pachaos_enabled()) {
        fprintf(stderr,
            "kobox work: queue_delayed_work wq=%p work=%p func=%p delay=%lu due_ns=%llu caller=%p\n",
            wq,
            dwork,
            (void *)func,
            delay,
            (unsigned long long)due_ns,
            __builtin_return_address(0));
    }
    if (!queue_deferred(KB_DEFERRED_WORK, dwork, wq, due_ns)) {
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
    kb_deferred_item_t *item = take_deferred_work(work, 1);
    if (!was_pending && item == NULL) {
        return 0;
    }
    if (item == NULL) {
        return 0;
    }
    kb_device_backend_t *old_backend = kb_shim_current_device_backend();
    kb_shim_set_device_backend(item->backend);
    const int run_result = run_work(work, item->kernel_gs);
    kb_shim_set_device_backend(old_backend);
    if (run_result == KB_WORK_RUN_RETRY) {
        prepend_deferred_item(item);
        return 0;
    }
    release_deferred_item(item);
    return run_result == KB_WORK_RUN_EXECUTED;
}

int kb_flush_delayed_work(void *dwork)
{
    const int was_pending = dwork != NULL && work_pending(dwork);
    kb_deferred_item_t *item = take_deferred_work(dwork, 0);
    if (!was_pending && item == NULL) {
        return 0;
    }
    if (item == NULL) {
        return 0;
    }
    kb_device_backend_t *old_backend = kb_shim_current_device_backend();
    kb_shim_set_device_backend(item->backend);
    const int run_result = run_work(dwork, item->kernel_gs);
    kb_shim_set_device_backend(old_backend);
    if (run_result == KB_WORK_RUN_RETRY) {
        prepend_deferred_item(item);
        return 0;
    }
    release_deferred_item(item);
    return run_result == KB_WORK_RUN_EXECUTED;
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
    (void)queue_deferred(KB_DEFERRED_TASKLET, tasklet, NULL, 0);
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
        item->backend = kb_shim_current_device_backend();
        item->kernel_gs = kb_shim_current_kernel_gs();
        return 1;
    }
    return queue_deferred(KB_DEFERRED_TIMER, timer, NULL, due_ns);
}

void kb_add_timer(void *timer)
{
    write_pointer(timer, KB_LINUX_TIMER_ENTRY_PPREV_OFFSET, timer);
    unsigned long expires = read_ulong(timer, KB_LINUX_TIMER_EXPIRES_OFFSET);
    (void)queue_deferred(KB_DEFERRED_TIMER, timer, NULL, jiffies_to_ns(expires));
}

int kb_timer_delete(void *timer)
{
    kb_deferred_item_t **cursor = &deferred_head;
    int removed = 0;
    while (*cursor != NULL) {
        kb_deferred_item_t *item = *cursor;
        if (item->kind == KB_DEFERRED_TIMER && item->object == timer) {
            *cursor = item->next;
            release_deferred_item(item);
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
    if (timeout == 0) {
        return 0;
    }
    if (kb_kthread_yield_current()) {
        return timeout == ULONG_MAX ? timeout : timeout - 1u;
    }

    /*
     * schedule_timeout() is the sleep primitive behind wait_event_timeout().
     * IRQ handlers commonly queue ordinary work which makes the wait
     * condition true, so draining only tasklets/timers here loses that wakeup.
     * Sleep in one-jiffy quanta, run the full deferred queue after the IRQ,
     * then let the caller recheck its condition with an accurate remainder.
     */
    const uint64_t start_ns = monotonic_ns();
    kb_run_deferred_work();
    (void)kb_handle_any_irq_no_work(jiffies_to_ns(1));
    kb_run_deferred_work();
    if (timeout == ULONG_MAX) {
        return timeout;
    }
    const uint64_t end_ns = monotonic_ns();
    uint64_t elapsed = end_ns >= start_ns ? end_ns - start_ns : 0;
    unsigned long elapsed_jiffies = ns_to_jiffies_ceil(elapsed);
    if (elapsed_jiffies == 0) {
        elapsed_jiffies = 1;
    }
    return elapsed_jiffies < timeout ? timeout - elapsed_jiffies : 0;
}

unsigned long kb_msecs_to_jiffies(unsigned int msecs)
{
    return ns_to_jiffies_ceil((uint64_t)msecs * KB_NSEC_PER_MSEC);
}

unsigned long kb_usecs_to_jiffies(unsigned int usecs)
{
    return ns_to_jiffies_ceil((uint64_t)usecs * KB_NSEC_PER_USEC);
}

unsigned long kb_round_jiffies_up(unsigned long jiffies)
{
    return jiffies;
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

unsigned long kb_msleep_interruptible(unsigned int msecs)
{
    kb_msleep(msecs);
    return 0;
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
