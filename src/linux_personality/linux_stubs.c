#include "kobox/shim.h"

#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static kb_ida_record_t *ida_records;
static kb_kthread_record_t *kthread_records;
static kb_jbd2_journal_record_t *jbd2_journal_records;

static int crypto_trace_enabled(void);

enum {
    KB_JBD2_JOURNAL_SUPERBLOCK_OFFSET = 0x3b0,
    KB_JBD2_JOURNAL_COMMIT_SEQUENCE_OFFSET = 0x428,
    KB_JBD2_JOURNAL_COMMIT_REQUEST_OFFSET = 0x42c,
    KB_JBD2_JOURNAL_TASK_OFFSET = 0x440,
};

void kb_noop_stub(void)
{
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
    void *handle = kb_kzalloc(256, 0);
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

void *kb_alloc_stub(void)
{
    return kb_kzalloc(4096, 0);
}

int kb_percpu_counter_init_many_stub(void *counters, long amount, unsigned int batch, unsigned int count, void *key)
{
    (void)counters;
    (void)amount;
    (void)batch;
    (void)count;
    (void)key;
    if (crypto_trace_enabled()) {
        fprintf(stderr, "kobox-core: percpu_counter_init_many amount=%ld batch=%u count=%u\n", amount, batch, count);
    }
    return 0;
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
