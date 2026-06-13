#include "kobox/shim.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct kb_ida_record {
    void *ida;
    uint64_t *words;
    size_t word_count;
    struct kb_ida_record *next;
} kb_ida_record_t;

static kb_ida_record_t *ida_records;

static int crypto_trace_enabled(void);

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

static int ida_ensure_words(kb_ida_record_t *record, size_t required_words)
{
    if (record == NULL) {
        return 0;
    }
    if (record->word_count >= required_words) {
        return 1;
    }
    uint64_t *words = kb_krealloc_managed(record->words, required_words * sizeof(*words), 0);
    if (words == NULL) {
        return 0;
    }
    memset(words + record->word_count, 0, (required_words - record->word_count) * sizeof(*words));
    record->words = words;
    record->word_count = required_words;
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
    size_t required_words = ((size_t)max / 64u) + 1u;
    if (!ida_ensure_words(record, required_words)) {
        return -12;
    }
    for (unsigned int id = min; id <= max; id++) {
        size_t word_index = (size_t)id / 64u;
        uint64_t bit = 1ull << (id % 64u);
        if ((record->words[word_index] & bit) == 0) {
            record->words[word_index] |= bit;
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
    size_t word_index = (size_t)id / 64u;
    if (word_index >= record->word_count) {
        return;
    }
    record->words[word_index] &= ~(1ull << (id % 64u));
}

void kb_ida_destroy(void *ida)
{
    kb_ida_record_t **cursor = &ida_records;
    while (*cursor != NULL) {
        kb_ida_record_t *record = *cursor;
        if (record->ida == ida) {
            *cursor = record->next;
            kb_kfree(record->words);
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
