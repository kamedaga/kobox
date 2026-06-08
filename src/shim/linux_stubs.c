#include "kobox/shim.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct kb_ida_record {
    void *ida;
    uint64_t *words;
    size_t word_count;
    struct kb_ida_record *next;
} kb_ida_record_t;

static kb_ida_record_t *ida_records;

int kb_return_zero(void)
{
    return 0;
}

int kb_return_one(void)
{
    return 1;
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

void *kb_identity_ptr(void *ptr)
{
    return ptr;
}

const char *kb_empty_string(void)
{
    return "";
}
