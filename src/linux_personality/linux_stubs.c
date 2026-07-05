#include "kobox/shim.h"
#include "loader/module_context.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__pachaos__)
#include <time.h>
#endif

enum {
    KB_STUB_PAGE_SIZE = 4096,
    KB_STUB_STRUCT_PAGE_SIZE = 64,
    KB_STUB_TASK_FLAGS_OFFSET = 0x0,
    KB_STUB_TASK_COMM_OFFSET = 0xbd8,
    KB_STUB_TASK_COMM_LEN = 16,
    KB_STUB_TIF_SIGPENDING_BIT = 2,
};

typedef struct kb_ida_record {
    void *ida;
    unsigned int *ids;
    size_t count;
    size_t capacity;
    struct kb_ida_record *next;
} kb_ida_record_t;

typedef struct kb_kthread_record {
    void *task;
    int (*threadfn)(void *data);
    void *data;
    int node;
    int activated;
    char name[64];
    struct kb_kthread_record *next;
} kb_kthread_record_t;

typedef struct kb_jbd2_journal_record {
    void *journal;
    struct kb_jbd2_journal_record *next;
} kb_jbd2_journal_record_t;

typedef struct kb_pending_pgrp_signal {
    void *pgrp;
    int sig;
    int priv;
    uint64_t sequence;
} kb_pending_pgrp_signal_t;

typedef struct kb_pending_task_signal {
    void *task;
    void *info;
    int sig;
    int type;
    uint64_t sequence;
} kb_pending_task_signal_t;

typedef struct kb_stub_pid {
    int count;
    unsigned int level;
    int nr;
    void *task;
} kb_stub_pid_t;

typedef struct kb_sysctl_registration {
    char path[96];
    const char *table_name;
    void *table;
    size_t table_size;
    uint64_t sequence;
    struct kb_sysctl_registration *next;
} kb_sysctl_registration_t;

static kb_ida_record_t *ida_records;
static kb_kthread_record_t *kthread_records;
static kb_jbd2_journal_record_t *jbd2_journal_records;
static kb_sysctl_registration_t *sysctl_registrations;
static kb_pending_pgrp_signal_t pending_pgrp_signals[64];
static kb_pending_task_signal_t pending_task_signals[64];
static kb_stub_pid_t stub_pids[64];
static uint64_t sysctl_registration_sequence;
static uint64_t pending_pgrp_signal_sequence;
static uint64_t pending_task_signal_sequence;

static size_t pending_pgrp_signal_count(void)
{
    return sizeof(pending_pgrp_signals) / sizeof(pending_pgrp_signals[0]);
}

static size_t pending_task_signal_count(void)
{
    return sizeof(pending_task_signals) / sizeof(pending_task_signals[0]);
}

static int crypto_trace_enabled(void);

static void kb_mark_task_signal_pending(void *task)
{
    if (task == NULL) {
        task = kb_loader_module_current_task(kb_loader_active_module());
    }
    if (task == NULL) {
        return;
    }
    unsigned long flags = 0;
    memcpy(&flags, (const uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, sizeof(flags));
    flags |= 1ul << KB_STUB_TIF_SIGPENDING_BIT;
    memcpy((uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, &flags, sizeof(flags));
}

void kb_clear_current_signal_pending(void)
{
    void *task = kb_loader_module_current_task(kb_loader_active_module());
    if (task == NULL) {
        return;
    }
    unsigned long flags = 0;
    memcpy(&flags, (const uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, sizeof(flags));
    flags &= ~(1ul << KB_STUB_TIF_SIGPENDING_BIT);
    memcpy((uint8_t *)task + KB_STUB_TASK_FLAGS_OFFSET, &flags, sizeof(flags));
}

static int trace_dma_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DMA");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void *page_payload_from_struct_page(void *page)
{
    if (page == NULL) {
        return NULL;
    }
    uintptr_t vmemmap = kb_linux_kvm_vmemmap_base();
    uintptr_t page_offset = kb_linux_kvm_page_offset_base();
    uintptr_t page_addr = (uintptr_t)page;
    if (vmemmap == 0 || page_offset == 0 || page_addr < vmemmap) {
        return NULL;
    }
    uintptr_t index = (page_addr - vmemmap) / KB_STUB_STRUCT_PAGE_SIZE;
    return (void *)(page_offset + index * KB_STUB_PAGE_SIZE);
}

static uint64_t page_phys_from_struct_page(void *page)
{
    uintptr_t vmemmap = kb_linux_kvm_vmemmap_base();
    uintptr_t page_addr = (uintptr_t)page;
    if (vmemmap == 0 || page_addr < vmemmap) {
        return 0;
    }
    uint64_t index = (uint64_t)((page_addr - vmemmap) / KB_STUB_STRUCT_PAGE_SIZE);
    return (uint64_t)kb_linux_kvm_phys_base() + (index * KB_STUB_PAGE_SIZE);
}

enum {
    KB_JBD2_JOURNAL_SUPERBLOCK_OFFSET = 0x3b0,
    KB_JBD2_JOURNAL_COMMIT_SEQUENCE_OFFSET = 0x428,
    KB_JBD2_JOURNAL_COMMIT_REQUEST_OFFSET = 0x42c,
    KB_JBD2_JOURNAL_TASK_OFFSET = 0x440,
    KB_PERCPU_COUNTER_STRIDE = 0x28,
    KB_PERCPU_COUNTER_COUNT_OFFSET = 0x8,
    KB_KFIFO_IN_OFFSET = 0,
    KB_KFIFO_OUT_OFFSET = 4,
    KB_KFIFO_MASK_OFFSET = 8,
    KB_KFIFO_ESIZE_OFFSET = 12,
    KB_KFIFO_DATA_OFFSET = 16,
};

void kb_noop_stub(void)
{
}

const int kb_sysctl_vals[] = {0, 1, 2, 3, 4, 100, 200, 1000, 3000, INT32_MAX, 65535, -1};

static int sysctl_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_SYSCTL");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void *register_sysctl_common(const char *path, void *table, const char *table_name, size_t table_size)
{
    kb_sysctl_registration_t *registration = calloc(1, sizeof(*registration));
    if (registration == NULL) {
        return NULL;
    }
    if (path != NULL) {
        snprintf(registration->path, sizeof(registration->path), "%s", path);
    }
    registration->table_name = table_name;
    registration->table = table;
    registration->table_size = table_size;
    registration->sequence = ++sysctl_registration_sequence;
    registration->next = sysctl_registrations;
    sysctl_registrations = registration;
    if (sysctl_trace_enabled()) {
        fprintf(stderr,
            "kobox-sysctl: register path=%s table=%p name=%s size=%zu seq=%llu\n",
            registration->path,
            table,
            table_name != NULL ? table_name : "",
            table_size,
            (unsigned long long)registration->sequence);
    }
    return registration;
}

void *kb_register_sysctl_init(const char *path, void *table, const char *table_name, size_t table_size)
{
    return register_sysctl_common(path, table, table_name, table_size);
}

void *kb_register_sysctl_sz(const char *path, void *table, size_t table_size)
{
    return register_sysctl_common(path, table, NULL, table_size);
}

void kb_unregister_sysctl_table(void *header)
{
    kb_sysctl_registration_t **cursor = &sysctl_registrations;
    while (*cursor != NULL) {
        if (*cursor == header) {
            kb_sysctl_registration_t *dead = *cursor;
            *cursor = dead->next;
            if (sysctl_trace_enabled()) {
                fprintf(stderr,
                    "kobox-sysctl: unregister path=%s seq=%llu\n",
                    dead->path,
                    (unsigned long long)dead->sequence);
            }
            free(dead);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

int kb_proc_dointvec(void *table, int write, void *buffer, size_t *lenp, long long *ppos)
{
    (void)table;
    (void)write;
    (void)buffer;
    (void)lenp;
    (void)ppos;
    return 0;
}

int kb_proc_dointvec_minmax(void *table, int write, void *buffer, size_t *lenp, long long *ppos)
{
    return kb_proc_dointvec(table, write, buffer, lenp, ppos);
}

int kb_proc_dobool(void *table, int write, void *buffer, size_t *lenp, long long *ppos)
{
    return kb_proc_dointvec(table, write, buffer, lenp, ppos);
}

#if defined(__x86_64__) && !defined(_MSC_VER)
__attribute__((naked)) void kb_stackleak_track_stack_stub(void)
{
    __asm__("ret");
}
#else
void kb_stackleak_track_stack_stub(void)
{
}
#endif

int kb_return_zero(void)
{
    return 0;
}

int kb_return_one(void)
{
    return 1;
}

char *kb_get_task_comm(char *buf, size_t buf_size, void *task)
{
    if (buf == NULL || buf_size == 0) {
        return buf;
    }

    memset(buf, 0, buf_size);
    if (task == NULL) {
        task = kb_loader_module_current_task(kb_loader_active_module());
    }

    const char *comm = "kobox";
    if (task != NULL) {
        comm = (const char *)((const unsigned char *)task + KB_STUB_TASK_COMM_OFFSET);
    }

    size_t limit = buf_size - 1u;
    if (limit > KB_STUB_TASK_COMM_LEN) {
        limit = KB_STUB_TASK_COMM_LEN;
    }
    size_t i = 0;
    while (i < limit && comm[i] != '\0') {
        buf[i] = comm[i];
        i++;
    }
    return buf;
}

int kb_kill_pgrp(void *pgrp, int sig, int priv)
{
    if (sig < 0 || sig > 64) {
        return -22;
    }

    const uint64_t sequence = ++pending_pgrp_signal_sequence;
    pending_pgrp_signals[sequence % pending_pgrp_signal_count()] =
        (kb_pending_pgrp_signal_t){
            .pgrp = pgrp,
            .sig = sig,
            .priv = priv,
            .sequence = sequence,
        };
    kb_stub_pid_t *record = (kb_stub_pid_t *)pgrp;
    kb_mark_task_signal_pending(record != NULL ? record->task : NULL);

    const char *trace = getenv("KOBOX_TRACE_TTY_SIGNALS");
    if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
        fprintf(stderr,
            "kobox tty: kill_pgrp pgrp=%p sig=%d priv=%d seq=%llu\n",
            pgrp,
            sig,
            priv,
            (unsigned long long)sequence);
    }

    return 0;
}

static int kb_record_task_signal(const char *source, int sig, void *info, void *task, int type)
{
    if (sig < 0 || sig > 64) {
        return -22;
    }

    const uint64_t sequence = ++pending_task_signal_sequence;
    pending_task_signals[sequence % pending_task_signal_count()] =
        (kb_pending_task_signal_t){
            .task = task,
            .info = info,
            .sig = sig,
            .type = type,
            .sequence = sequence,
        };
    kb_mark_task_signal_pending(task);

    const char *trace = getenv("KOBOX_TRACE_TTY_SIGNALS");
    if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
        fprintf(stderr,
            "kobox tty: %s task=%p sig=%d type=%d info=%p seq=%llu\n",
            source,
            task,
            sig,
            type,
            info,
            (unsigned long long)sequence);
    }

    return 0;
}

int kb_group_send_sig_info(int sig, void *info, void *task, int type)
{
    return kb_record_task_signal("group_send_sig_info", sig, info, task, type);
}

int kb_send_signal_locked(int sig, void *info, void *task, int type)
{
    return kb_record_task_signal("send_signal_locked", sig, info, task, type);
}

int kb_llist_add_batch(void *new_first, void *new_last, void *head)
{
    if (new_first == NULL || new_last == NULL || head == NULL) {
        return 0;
    }
    void *old_first = NULL;
    memcpy(&old_first, head, sizeof(old_first));
    memcpy(new_last, &old_first, sizeof(old_first));
    memcpy(head, &new_first, sizeof(new_first));
    return old_first == NULL;
}

void *kb_llist_del_first(void *head)
{
    if (head == NULL) {
        return NULL;
    }
    void *entry = NULL;
    memcpy(&entry, head, sizeof(entry));
    if (entry == NULL) {
        return NULL;
    }
    void *next = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(head, &next, sizeof(next));
    return entry;
}

static unsigned int kb_pow2_floor_u32(unsigned int value)
{
    if (value == 0) {
        return 0;
    }
    unsigned int power = 1;
    while (power <= value / 2u) {
        power <<= 1u;
    }
    return power;
}

static void kb_kfifo_load(void *fifo, unsigned int *in, unsigned int *out, unsigned int *mask, unsigned int *esize, void **data)
{
    memcpy(in, (const uint8_t *)fifo + KB_KFIFO_IN_OFFSET, sizeof(*in));
    memcpy(out, (const uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, sizeof(*out));
    memcpy(mask, (const uint8_t *)fifo + KB_KFIFO_MASK_OFFSET, sizeof(*mask));
    memcpy(esize, (const uint8_t *)fifo + KB_KFIFO_ESIZE_OFFSET, sizeof(*esize));
    memcpy(data, (const uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, sizeof(*data));
}

int kb_kfifo_init(void *fifo, void *buffer, unsigned int size, size_t esize)
{
    if (fifo == NULL || buffer == NULL || esize == 0 || esize > UINT32_MAX) {
        return -22;
    }
    unsigned int elements = size / (unsigned int)esize;
    elements = kb_pow2_floor_u32(elements);
    unsigned int in = 0;
    unsigned int out = 0;
    unsigned int mask = elements < 2 ? 0 : elements - 1u;
    unsigned int esize32 = (unsigned int)esize;
    memcpy((uint8_t *)fifo + KB_KFIFO_IN_OFFSET, &in, sizeof(in));
    memcpy((uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, &out, sizeof(out));
    memcpy((uint8_t *)fifo + KB_KFIFO_MASK_OFFSET, &mask, sizeof(mask));
    memcpy((uint8_t *)fifo + KB_KFIFO_ESIZE_OFFSET, &esize32, sizeof(esize32));
    memcpy((uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, &buffer, sizeof(buffer));
    return elements < 2 ? -22 : 0;
}

unsigned int kb_kfifo_in(void *fifo, const void *buffer, unsigned int len)
{
    if (fifo == NULL || buffer == NULL) {
        return 0;
    }
    unsigned int in = 0;
    unsigned int out = 0;
    unsigned int mask = 0;
    unsigned int esize = 0;
    void *data = NULL;
    kb_kfifo_load(fifo, &in, &out, &mask, &esize, &data);
    if (data == NULL || esize == 0 || mask == 0) {
        return 0;
    }
    unsigned int size = mask + 1u;
    unsigned int unused = size - (in - out);
    if (len > unused) {
        len = unused;
    }
    unsigned int off = in & mask;
    unsigned int first = size - off;
    if (first > len) {
        first = len;
    }
    memcpy((uint8_t *)data + (off * esize), buffer, (size_t)first * esize);
    memcpy(data, (const uint8_t *)buffer + ((size_t)first * esize), (size_t)(len - first) * esize);
    in += len;
    memcpy((uint8_t *)fifo + KB_KFIFO_IN_OFFSET, &in, sizeof(in));
    return len;
}

unsigned int kb_kfifo_out(void *fifo, void *buffer, unsigned int len)
{
    if (fifo == NULL || buffer == NULL) {
        return 0;
    }
    unsigned int in = 0;
    unsigned int out = 0;
    unsigned int mask = 0;
    unsigned int esize = 0;
    void *data = NULL;
    kb_kfifo_load(fifo, &in, &out, &mask, &esize, &data);
    if (data == NULL || esize == 0 || mask == 0) {
        return 0;
    }
    unsigned int available = in - out;
    if (len > available) {
        len = available;
    }
    unsigned int size = mask + 1u;
    unsigned int off = out & mask;
    unsigned int first = size - off;
    if (first > len) {
        first = len;
    }
    memcpy(buffer, (const uint8_t *)data + (off * esize), (size_t)first * esize);
    memcpy((uint8_t *)buffer + ((size_t)first * esize), data, (size_t)(len - first) * esize);
    out += len;
    memcpy((uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, &out, sizeof(out));
    return len;
}

int kb_kfifo_to_user(void *fifo, void *to, unsigned int len, unsigned int *copied)
{
    unsigned int done = kb_kfifo_out(fifo, to, len);
    if (copied != NULL) {
        *copied = done;
    }
    return done == len ? 0 : -14;
}

void kb_kfifo_free(void *fifo)
{
    if (fifo == NULL) {
        return;
    }
    unsigned int zero = 0;
    void *data = NULL;
    memcpy(&data, (const uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, sizeof(data));
    kb_kfree(data);
    data = NULL;
    memcpy((uint8_t *)fifo + KB_KFIFO_IN_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_OUT_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_MASK_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_ESIZE_OFFSET, &zero, sizeof(zero));
    memcpy((uint8_t *)fifo + KB_KFIFO_DATA_OFFSET, &data, sizeof(data));
}

void *kb_find_vpid(int nr)
{
    if (nr <= 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(stub_pids) / sizeof(stub_pids[0]); i++) {
        if (stub_pids[i].nr == nr) {
            if (stub_pids[i].task == NULL) {
                stub_pids[i].task = kb_loader_module_current_task(kb_loader_active_module());
            }
            return &stub_pids[i];
        }
    }
    for (size_t i = 0; i < sizeof(stub_pids) / sizeof(stub_pids[0]); i++) {
        if (stub_pids[i].nr == 0) {
            stub_pids[i] = (kb_stub_pid_t){
                .count = 1,
                .level = 0,
                .nr = nr,
                .task = kb_loader_module_current_task(kb_loader_active_module()),
            };
            return &stub_pids[i];
        }
    }
    return NULL;
}

void *kb_pid_task(void *pid, int type)
{
    (void)type;
    kb_stub_pid_t *record = (kb_stub_pid_t *)pid;
    if (record == NULL) {
        return NULL;
    }
    if (record->task != NULL) {
        return record->task;
    }
    return kb_loader_module_current_task(kb_loader_active_module());
}

int kb_pid_vnr(void *pid)
{
    kb_stub_pid_t *record = (kb_stub_pid_t *)pid;
    return record == NULL ? 0 : record->nr;
}

typedef struct kb_substring {
    char *from;
    char *to;
} kb_substring_t;

static int kb_parse_substring_int(const void *substring, int base, int *result)
{
    if (substring == NULL || result == NULL) {
        return -22;
    }
    const kb_substring_t *span = (const kb_substring_t *)substring;
    if (span->from == NULL || span->to == NULL || span->to < span->from) {
        return -22;
    }
    size_t len = (size_t)(span->to - span->from);
    if (len >= 64) {
        return -22;
    }
    char buffer[64];
    memcpy(buffer, span->from, len);
    buffer[len] = '\0';
    char *end = NULL;
    long value = strtol(buffer, &end, base);
    if (end == buffer || *end != '\0') {
        return -22;
    }
    *result = (int)value;
    return 0;
}

int kb_match_token(char *string, const void *table, void *args)
{
    (void)table;
    (void)args;
    return string == NULL || string[0] == '\0' ? 0 : -1;
}

int kb_match_int(const void *substring, int *result)
{
    return kb_parse_substring_int(substring, 10, result);
}

int kb_match_octal(const void *substring, int *result)
{
    return kb_parse_substring_int(substring, 8, result);
}

int64_t kb_ktime_get_real_seconds(void)
{
#if defined(__pachaos__)
    static int64_t synthetic_seconds = 1;
    return synthetic_seconds++;
#else
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 1;
#endif
}

void kb_string_get_size(uint64_t size, uint64_t blk_size, int units, char *buf, int len)
{
    (void)units;
    if (buf == NULL || len <= 0) {
        return;
    }
    const uint64_t bytes = blk_size == 0 ? size : size * blk_size;
    (void)snprintf(buf, (size_t)len, "%llu B", (unsigned long long)bytes);
}

uint32_t kb_get_random_u32_below(uint32_t ceil)
{
    if (ceil == 0) {
        return 0;
    }
    return 1u % ceil;
}

uint16_t kb_get_random_u16(void)
{
    return 1;
}

void kb_generate_random_uuid(unsigned char *uuid)
{
    if (uuid == NULL) {
        return;
    }
    memset(uuid, 0, 16);
    uuid[15] = 1;
}

char *kb_d_path(void *path, char *buffer, int buffer_length)
{
    (void)path;
    if (buffer == NULL || buffer_length <= 0) {
        return NULL;
    }
    buffer[0] = '\0';
    return buffer;
}

size_t kb_strlcpy(char *dst, const char *src, size_t size)
{
    if (src == NULL) {
        if (dst != NULL && size > 0) {
            dst[0] = '\0';
        }
        return 0;
    }
    size_t len = strlen(src);
    if (dst != NULL && size > 0) {
        size_t copy = len >= size ? size - 1u : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

void *kb_kmemdup_nul(const void *src, size_t len, unsigned int flags)
{
    char *dst = kb_kmalloc(len + 1u, flags);
    if (dst == NULL) {
        return NULL;
    }
    if (src != NULL && len > 0) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
    return dst;
}

long kb_sized_strscpy(char *dst, const char *src, size_t size)
{
    if (dst == NULL || src == NULL) {
        return -22;
    }
    if (size == 0) {
        return -7;
    }
    size_t i = 0;
    for (; i + 1u < size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return src[i] == '\0' ? (long)i : -7;
}

char *kb_skip_spaces(const char *str)
{
    if (str == NULL) {
        return NULL;
    }
    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    return (char *)str;
}

void kb_memcpy_and_pad(void *dest, size_t dest_len, const void *src, size_t count, int pad)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }
    size_t copy_len = count < dest_len ? count : dest_len;
    if (copy_len > 0 && src != NULL) {
        memcpy(dest, src, copy_len);
    }
    if (dest_len > copy_len) {
        memset((unsigned char *)dest + copy_len, pad, dest_len - copy_len);
    }
}

int64_t kb_vfs_setpos(void *file, int64_t offset, int64_t maxsize)
{
    (void)file;
    if (offset < 0 || offset > maxsize) {
        return -22;
    }
    return offset;
}

char *kb_strreplace(char *s, char old_char, char new_char)
{
    if (s == NULL) {
        return NULL;
    }
    for (char *p = s; *p != '\0'; p++) {
        if (*p == old_char) {
            *p = new_char;
        }
    }
    return s;
}

size_t kb_memweight(const void *ptr, size_t bytes)
{
    if (ptr == NULL) {
        return 0;
    }
    const unsigned char *p = ptr;
    size_t count = 0;
    for (size_t i = 0; i < bytes; i++) {
        unsigned char value = p[i];
        while (value != 0) {
            count += (size_t)(value & 1u);
            value >>= 1;
        }
    }
    return count;
}

static int low_or_error_ptr(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static int jbd2_trace_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_TRACE_JBD2");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    return cached;
}

static void register_jbd2_journal(void *journal)
{
    if (low_or_error_ptr(journal)) {
        return;
    }
    for (kb_jbd2_journal_record_t *record = jbd2_journal_records; record != NULL; record = record->next) {
        if (record->journal == journal) {
            return;
        }
    }
    kb_jbd2_journal_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return;
    }
    record->journal = journal;
    record->next = jbd2_journal_records;
    jbd2_journal_records = record;
    if (jbd2_trace_enabled()) {
        fprintf(stderr, "kobox jbd2: register journal=%p\n", journal);
    }
}

void kb_jbd2_progress_registered_journals(void)
{
    for (kb_jbd2_journal_record_t *record = jbd2_journal_records; record != NULL; record = record->next) {
        unsigned char *journal = record->journal;
        if (low_or_error_ptr(journal)) {
            continue;
        }
        uint32_t commit_sequence = 0;
        uint32_t commit_request = 0;
        memcpy(&commit_sequence, journal + KB_JBD2_JOURNAL_COMMIT_SEQUENCE_OFFSET, sizeof(commit_sequence));
        memcpy(&commit_request, journal + KB_JBD2_JOURNAL_COMMIT_REQUEST_OFFSET, sizeof(commit_request));
        if ((int32_t)(commit_request - commit_sequence) <= 0) {
            continue;
        }
        memcpy(journal + KB_JBD2_JOURNAL_COMMIT_SEQUENCE_OFFSET, &commit_request, sizeof(commit_request));
        if (jbd2_trace_enabled()) {
            fprintf(stderr,
                "kobox jbd2: progress journal=%p commit_sequence=%u commit_request=%u\n",
                (void *)journal,
                commit_sequence,
                commit_request);
        }
    }
}

void *kb_jbd2_journal_init_stub(void)
{
    void *journal = kb_kzalloc(2048, 0);
    void *superblock = kb_kzalloc(1024, 0);
    if (journal != NULL && superblock != NULL) {
        uint32_t compatible_features = 0x100u;
        ((unsigned char *)journal)[0] = 0x20u;
        memcpy((unsigned char *)superblock + 0x30, &compatible_features, sizeof(compatible_features));
        memcpy((unsigned char *)journal + KB_JBD2_JOURNAL_SUPERBLOCK_OFFSET, &superblock, sizeof(superblock));
    } else {
        kb_kfree(journal);
        kb_kfree(superblock);
        journal = NULL;
    }
    register_jbd2_journal(journal);
    if (jbd2_trace_enabled()) {
        fprintf(stderr, "kobox jbd2: init journal=%p superblock=%p\n", journal, superblock);
    }
    return journal;
}

void *kb_jbd2_journal_start_stub(void)
{
    void *handle = (void *)1;
    if (jbd2_trace_enabled()) {
        fprintf(stderr, "kobox jbd2: start handle=%p\n", handle);
    }
    return handle;
}

int kb_jbd2_journal_stop_stub(void *handle)
{
    if (!low_or_error_ptr(handle)) {
        kb_kfree(handle);
    }
    if (jbd2_trace_enabled()) {
        fprintf(stderr, "kobox jbd2: stop handle=%p\n", handle);
    }
    return 0;
}

int kb_jbd2_journal_blocks_per_page_stub(void)
{
    return 1;
}

void kb_jbd2_journal_destroy_stub(void *journal)
{
    if (!low_or_error_ptr(journal)) {
        void *superblock = NULL;
        memcpy(&superblock, (unsigned char *)journal + KB_JBD2_JOURNAL_SUPERBLOCK_OFFSET, sizeof(superblock));
        if (!low_or_error_ptr(superblock)) {
            kb_kfree(superblock);
        }
        kb_kfree(journal);
    }
    if (jbd2_trace_enabled()) {
        fprintf(stderr, "kobox jbd2: destroy journal=%p\n", journal);
    }
}

int kb_list_add_valid_or_report(void *new_entry, void *prev, void *next)
{
    int result = 0;
    if (low_or_error_ptr(new_entry) || low_or_error_ptr(prev) || low_or_error_ptr(next)) {
        goto out;
    }

    void *prev_next = NULL;
    void *next_prev = NULL;
    memcpy(&prev_next, prev, sizeof(prev_next));
    memcpy(&next_prev, (const unsigned char *)next + sizeof(void *), sizeof(next_prev));
    if (prev_next != next || next_prev != prev) {
        goto out;
    }
    if (new_entry == prev || new_entry == next) {
        goto out;
    }
    result = 1;

out:
    return result;
}

int kb_list_del_entry_valid_or_report(void *entry)
{
    int result = 0;
    if (low_or_error_ptr(entry)) {
        goto out;
    }

    void *next = NULL;
    void *prev = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(&prev, (const unsigned char *)entry + sizeof(void *), sizeof(prev));
    if (low_or_error_ptr(next) || low_or_error_ptr(prev)) {
        goto out;
    }

    void *prev_next = NULL;
    void *next_prev = NULL;
    memcpy(&prev_next, prev, sizeof(prev_next));
    memcpy(&next_prev, (const unsigned char *)next + sizeof(void *), sizeof(next_prev));
    result = prev_next == entry && next_prev == entry;

out:
    return result;
}

void kb_list_add(void *new_entry, void *prev, void *next)
{
    if (low_or_error_ptr(new_entry) || low_or_error_ptr(prev) || low_or_error_ptr(next)) {
        return;
    }
    memcpy(new_entry, &next, sizeof(next));
    memcpy((unsigned char *)new_entry + sizeof(void *), &prev, sizeof(prev));
    memcpy(prev, &new_entry, sizeof(new_entry));
    memcpy((unsigned char *)next + sizeof(void *), &new_entry, sizeof(new_entry));
}

void kb_list_del(void *entry)
{
    if (low_or_error_ptr(entry)) {
        return;
    }
    void *next = NULL;
    void *prev = NULL;
    memcpy(&next, entry, sizeof(next));
    memcpy(&prev, (const unsigned char *)entry + sizeof(void *), sizeof(prev));
    if (low_or_error_ptr(next) || low_or_error_ptr(prev)) {
        return;
    }
    memcpy(prev, &next, sizeof(next));
    memcpy((unsigned char *)next + sizeof(void *), &prev, sizeof(prev));
    memcpy(entry, &entry, sizeof(entry));
    memcpy((unsigned char *)entry + sizeof(void *), &entry, sizeof(entry));
}

void *kb_kthread_create_on_node(int (*threadfn)(void *data), void *data, int node, const char *namefmt, ...)
{
    kb_kthread_record_t *record = calloc(1, sizeof(*record));
    void *task = calloc(1, 64);
    if (record == NULL || task == NULL) {
        free(record);
        free(task);
        return NULL;
    }
    record->task = task;
    record->threadfn = threadfn;
    record->data = data;
    record->node = node;
    if (namefmt != NULL) {
        va_list ap;
        va_start(ap, namefmt);
        vsnprintf(record->name, sizeof(record->name), namefmt, ap);
        va_end(ap);
    }
    record->next = kthread_records;
    kthread_records = record;
    return task;
}

int kb_wake_up_process(void *task)
{
    for (kb_kthread_record_t *record = kthread_records; record != NULL; record = record->next) {
        if (record->task != task) {
            continue;
        }
        record->activated = 1;
        /*
         * jbd2's thread function records current in journal->j_task before
         * entering its scheduler loop. The runtime does not yet host a real
         * kernel thread, so expose the same started state without running the
         * endless journal daemon body.
         */
        if (record->data != NULL && strncmp(record->name, "jbd2/", 5) == 0) {
            void *current = task;
            memcpy((unsigned char *)record->data + KB_JBD2_JOURNAL_TASK_OFFSET, &current, sizeof(current));
            register_jbd2_journal(record->data);
        }
        return 1;
    }
    return task != NULL ? 1 : 0;
}

#define KB_RB_PARENT_MASK (~(uintptr_t)3u)

enum {
    KB_RB_PARENT_COLOR_OFFSET = 0x0,
    KB_RB_RIGHT_OFFSET = 0x8,
    KB_RB_LEFT_OFFSET = 0x10,
};

static void *rb_parent(const void *node)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    uintptr_t parent_color = 0;
    memcpy(&parent_color, node, sizeof(parent_color));
    return (void *)(parent_color & KB_RB_PARENT_MASK);
}

static void rb_set_parent_keep_color(void *node, void *parent)
{
    if (low_or_error_ptr(node)) {
        return;
    }
    uintptr_t parent_color = 0;
    memcpy(&parent_color, node, sizeof(parent_color));
    parent_color = ((uintptr_t)parent & KB_RB_PARENT_MASK) | (parent_color & 3u);
    memcpy(node, &parent_color, sizeof(parent_color));
}

static void *rb_child(const void *node, size_t offset)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    void *child = NULL;
    memcpy(&child, (const unsigned char *)node + offset, sizeof(child));
    return low_or_error_ptr(child) ? NULL : child;
}

static void rb_set_child(void *node, size_t offset, void *child)
{
    if (!low_or_error_ptr(node)) {
        memcpy((unsigned char *)node + offset, &child, sizeof(child));
    }
}

void kb_rb_insert_color(void *node, void *root)
{
    (void)node;
    (void)root;
}

void *kb_rb_first(void *root)
{
    if (low_or_error_ptr(root)) {
        return NULL;
    }
    void *node = NULL;
    memcpy(&node, root, sizeof(node));
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    for (;;) {
        void *left = rb_child(node, KB_RB_LEFT_OFFSET);
        if (left == NULL) {
            return node;
        }
        node = left;
    }
}

void *kb_rb_next(void *node)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    void *right = rb_child(node, KB_RB_RIGHT_OFFSET);
    if (right != NULL) {
        node = right;
        for (;;) {
            void *left = rb_child(node, KB_RB_LEFT_OFFSET);
            if (left == NULL) {
                return node;
            }
            node = left;
        }
    }
    void *parent = rb_parent(node);
    while (parent != NULL && node == rb_child(parent, KB_RB_RIGHT_OFFSET)) {
        node = parent;
        parent = rb_parent(parent);
    }
    return parent;
}

void *kb_rb_prev(void *node)
{
    if (low_or_error_ptr(node)) {
        return NULL;
    }
    void *left = rb_child(node, KB_RB_LEFT_OFFSET);
    if (left != NULL) {
        node = left;
        for (;;) {
            void *right = rb_child(node, KB_RB_RIGHT_OFFSET);
            if (right == NULL) {
                return node;
            }
            node = right;
        }
    }
    void *parent = rb_parent(node);
    while (parent != NULL && node == rb_child(parent, KB_RB_LEFT_OFFSET)) {
        node = parent;
        parent = rb_parent(parent);
    }
    return parent;
}

void kb_rb_erase(void *node, void *root)
{
    if (low_or_error_ptr(node) || low_or_error_ptr(root)) {
        return;
    }

    void *left = rb_child(node, KB_RB_LEFT_OFFSET);
    void *right = rb_child(node, KB_RB_RIGHT_OFFSET);
    void *replacement = NULL;
    if (left == NULL) {
        replacement = right;
    } else if (right == NULL) {
        replacement = left;
    } else {
        replacement = right;
        while (rb_child(replacement, KB_RB_LEFT_OFFSET) != NULL) {
            replacement = rb_child(replacement, KB_RB_LEFT_OFFSET);
        }
        kb_rb_erase(replacement, root);
        left = rb_child(node, KB_RB_LEFT_OFFSET);
        right = rb_child(node, KB_RB_RIGHT_OFFSET);
        rb_set_child(replacement, KB_RB_LEFT_OFFSET, left);
        rb_set_child(replacement, KB_RB_RIGHT_OFFSET, right);
        if (left != NULL) {
            rb_set_parent_keep_color(left, replacement);
        }
        if (right != NULL) {
            rb_set_parent_keep_color(right, replacement);
        }
    }

    void *parent = rb_parent(node);
    if (parent == NULL) {
        memcpy(root, &replacement, sizeof(replacement));
    } else if (node == rb_child(parent, KB_RB_LEFT_OFFSET)) {
        rb_set_child(parent, KB_RB_LEFT_OFFSET, replacement);
    } else {
        rb_set_child(parent, KB_RB_RIGHT_OFFSET, replacement);
    }
    if (replacement != NULL) {
        rb_set_parent_keep_color(replacement, parent);
    }
    rb_set_child(node, KB_RB_LEFT_OFFSET, NULL);
    rb_set_child(node, KB_RB_RIGHT_OFFSET, NULL);
    rb_set_parent_keep_color(node, NULL);
}

static kb_ida_record_t *ida_record_for(void *ida, int create)
{
    if (ida == NULL) {
        return NULL;
    }
    for (kb_ida_record_t *record = ida_records; record != NULL; record = record->next) {
        if (record->ida == ida) {
            return record;
        }
    }
    if (!create) {
        return NULL;
    }
    kb_ida_record_t *record = kb_kzalloc(sizeof(*record), 0);
    if (record == NULL) {
        return NULL;
    }
    record->ida = ida;
    record->next = ida_records;
    ida_records = record;
    return record;
}

static int ida_contains(const kb_ida_record_t *record, unsigned int id)
{
    if (record == NULL) {
        return 0;
    }
    for (size_t i = 0; i < record->count; i++) {
        if (record->ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

static int ida_ensure_capacity(kb_ida_record_t *record, size_t required_capacity)
{
    if (record == NULL) {
        return 0;
    }
    if (record->capacity >= required_capacity) {
        return 1;
    }
    size_t next_capacity = record->capacity == 0 ? 8 : record->capacity * 2u;
    while (next_capacity < required_capacity) {
        if (next_capacity > SIZE_MAX / 2u) {
            return 0;
        }
        next_capacity *= 2u;
    }
    if (next_capacity > SIZE_MAX / sizeof(*record->ids)) {
        return 0;
    }
    unsigned int *ids = kb_krealloc_managed(record->ids, next_capacity * sizeof(*ids), 0);
    if (ids == NULL) {
        return 0;
    }
    record->ids = ids;
    record->capacity = next_capacity;
    return 1;
}

int kb_ida_alloc_range(void *ida, unsigned int min, unsigned int max, unsigned int flags)
{
    (void)flags;
    if (ida == NULL || max < min) {
        return -22;
    }
    kb_ida_record_t *record = ida_record_for(ida, 1);
    if (record == NULL) {
        return -12;
    }
    for (unsigned int id = min; id <= max; id++) {
        if (!ida_contains(record, id)) {
            if (!ida_ensure_capacity(record, record->count + 1u)) {
                return -12;
            }
            record->ids[record->count++] = id;
            return (int)id;
        }
        if (id == UINT32_MAX) {
            break;
        }
    }
    return -28;
}

void kb_ida_free(void *ida, unsigned int id)
{
    kb_ida_record_t *record = ida_record_for(ida, 0);
    if (record == NULL) {
        return;
    }
    for (size_t i = 0; i < record->count; i++) {
        if (record->ids[i] == id) {
            record->ids[i] = record->ids[record->count - 1u];
            record->count--;
            return;
        }
    }
}

void kb_ida_destroy(void *ida)
{
    kb_ida_record_t **cursor = &ida_records;
    while (*cursor != NULL) {
        kb_ida_record_t *record = *cursor;
        if (record->ida == ida) {
            *cursor = record->next;
            kb_kfree(record->ids);
            kb_kfree(record);
            return;
        }
        cursor = &record->next;
    }
}

int kb_ida_simple_get(void *ida, unsigned int start, unsigned int end, unsigned int flags)
{
    const unsigned int max = end == 0 ? UINT32_MAX : end - 1u;
    if (end != 0 && end <= start) {
        return -28;
    }
    return kb_ida_alloc_range(ida, start, max, flags);
}

void kb_ida_simple_remove(void *ida, unsigned int id)
{
    kb_ida_free(ida, id);
}

void *kb_alloc_pages_exact(size_t size, unsigned int flags)
{
    if (size == 0) {
        size = 1;
    }

    size_t page_count = (size + KB_STUB_PAGE_SIZE - 1u) / KB_STUB_PAGE_SIZE;
    unsigned int order = 0;
    size_t order_pages = 1;
    while (order_pages < page_count) {
        order_pages <<= 1u;
        order++;
    }

    void *page = kb_kvm_alloc_pages_stub(flags, order);
    void *payload = page_payload_from_struct_page(page);
    if (page == NULL || payload == NULL) {
        return NULL;
    }

    uint64_t expected = page_phys_from_struct_page(page);
    if (trace_dma_enabled()) {
        fprintf(stderr,
            "kobox dma: alloc_pages_exact size=0x%zx order=%u page=%p payload=%p phys=0x%llx\n",
            size,
            order,
            page,
            payload,
            (unsigned long long)expected);
    }
    return payload;
}

void kb_free_pages_exact(void *virt, size_t size)
{
    kb_kvm_free_pages_exact(virt, size);
}

int kb_alloc_cpumask_var(void *mask_out, unsigned int flags)
{
    if (mask_out == NULL) {
        return 0;
    }
    unsigned long *mask = kb_kzalloc(sizeof(*mask), flags);
    if (mask == NULL) {
        return 0;
    }
    *mask = 1;
    memcpy(mask_out, &mask, sizeof(mask));
    return 1;
}

void kb_free_cpumask_var(void *mask)
{
    kb_kfree(mask);
}

void *kb_alloc_stub(void)
{
    return kb_kzalloc(4096, 0);
}

void kb_free_first_arg_stub(void *ptr, void *ignored)
{
    (void)ignored;
    kb_kfree(ptr);
}

int kb_percpu_counter_init_many_stub(void *counters, long amount, unsigned int batch, unsigned int count, void *key)
{
    (void)batch;
    (void)key;
    if (counters != NULL) {
        for (unsigned int i = 0; i < count; i++) {
            int64_t value = (int64_t)amount;
            uint8_t *counter = (uint8_t *)counters + (size_t)i * KB_PERCPU_COUNTER_STRIDE;
            memcpy(counter + KB_PERCPU_COUNTER_COUNT_OFFSET, &value, sizeof(value));
        }
    }
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-core: percpu_counter_init_many amount=%ld batch=%u count=%u\n", amount, batch, count);
    }
    return 0;
}

void kb_percpu_counter_add_batch_stub(void *counter, int64_t amount, int32_t batch)
{
    (void)batch;
    if (counter == NULL) {
        return;
    }
    int64_t value = 0;
    memcpy(&value, (uint8_t *)counter + KB_PERCPU_COUNTER_COUNT_OFFSET, sizeof(value));
    value += amount;
    memcpy((uint8_t *)counter + KB_PERCPU_COUNTER_COUNT_OFFSET, &value, sizeof(value));
}

int64_t kb_percpu_counter_sum_stub(void *counter)
{
    if (counter == NULL) {
        return 0;
    }
    int64_t value = 0;
    memcpy(&value, (uint8_t *)counter + KB_PERCPU_COUNTER_COUNT_OFFSET, sizeof(value));
    return value;
}

void *kb_crypto_alloc_shash_stub(const char *alg_name, unsigned int type, unsigned int mask)
{
    (void)alg_name;
    (void)type;
    (void)mask;
    void *tfm = kb_kzalloc(4096, 0);
    if (tfm != NULL) {
        uint32_t digest_size = 4;
        memcpy(tfm, &digest_size, sizeof(digest_size));
    }
    return tfm;
}

static uint32_t crc32c_update(uint32_t crc, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82f63b78u & mask);
        }
    }
    return crc;
}

static int crypto_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_CRYPTO");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

int kb_crypto_shash_update_stub(void *desc, const void *data, unsigned int len)
{
    if (desc == NULL || (data == NULL && len != 0)) {
        return -22;
    }
    uint32_t crc = 0;
    memcpy(&crc, (const unsigned char *)desc + sizeof(void *), sizeof(crc));
    uint32_t before = crc;
    crc = crc32c_update(crc, (const unsigned char *)data, len);
    memcpy((unsigned char *)desc + sizeof(void *), &crc, sizeof(crc));
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-crypto: shash_update len=%u before=0x%08x after=0x%08x data=", len, before, crc);
        unsigned int preview = len < 64u ? len : 64u;
        for (unsigned int i = 0; i < preview; i++) {
            fprintf(stderr, "%02x", ((const unsigned char *)data)[i]);
        }
        fprintf(stderr, "\n");
    }
    return 0;
}

int kb_crypto_shash_final_stub(void *desc, void *out)
{
    if (desc == NULL || out == NULL) {
        return -22;
    }
    uint32_t crc = 0;
    memcpy(&crc, (const unsigned char *)desc + sizeof(void *), sizeof(crc));
    memcpy(out, &crc, sizeof(crc));
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-crypto: shash_final out=0x%08x\n", crc);
    }
    return 0;
}

int kb_crypto_shash_tfm_digest_stub(void *tfm, const void *data, unsigned int len, void *out)
{
    (void)tfm;
    if ((data == NULL && len != 0) || out == NULL) {
        return -22;
    }
    uint32_t crc = crc32c_update(0xffffffffu, (const unsigned char *)data, len);
    memcpy(out, &crc, sizeof(crc));
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-crypto: shash_tfm_digest len=%u out=0x%08x\n", len, crc);
    }
    return 0;
}

void *kb_identity_ptr(void *ptr)
{
    return ptr;
}

const char *kb_empty_string(void)
{
    return "";
}
