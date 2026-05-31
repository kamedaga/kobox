#include "subsystem/input/input.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_INPUT_DEVICE_MAX = 64,
    KB_LINUX_INPUT_DEV_NAME_OFFSET = 0,
    KB_LINUX_INPUT_DEV_PHYS_OFFSET = 8,
    KB_LINUX_INPUT_DEV_UNIQ_OFFSET = 16,
    KB_LINUX_INPUT_DEV_ID_OFFSET = 24,
};

typedef struct kb_input_device_record {
    kb_input_device_snapshot_t snapshot;
    int allocated;
    int absinfo_allocated;
} kb_input_device_record_t;

static kb_input_device_record_t input_devices[KB_INPUT_DEVICE_MAX];
static kb_input_event_t input_events[KB_INPUT_EVENT_QUEUE_MAX];
static size_t input_event_head;
static size_t input_event_count;
static uint64_t next_event_sequence = 1;
static unsigned int next_device_id = 1;

static int pointer_is_error_or_low(const void *ptr)
{
    const uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static void copy_known_string(char *dst, size_t dst_size, const char *src, const char *fallback)
{
    if (dst_size == 0) {
        return;
    }
    const char *value = fallback;
    if (src != NULL && !pointer_is_error_or_low(src)) {
        value = src;
    }
    if (value == NULL) {
        value = "";
    }
    snprintf(dst, dst_size, "%s", value);
}

static const char *linux_input_string(void *dev, size_t offset)
{
    if (dev == NULL) {
        return NULL;
    }
    const char *const *slot = (const char *const *)((unsigned char *)dev + offset);
    return *slot;
}

static kb_input_id_t linux_input_id(void *dev)
{
    kb_input_id_t id;
    memset(&id, 0, sizeof(id));
    if (dev != NULL) {
        memcpy(&id, (unsigned char *)dev + KB_LINUX_INPUT_DEV_ID_OFFSET, sizeof(id));
    }
    return id;
}

static long input_device_index(const kb_input_device_record_t *record)
{
    if (record == NULL) {
        return -1;
    }
    return (long)(record - input_devices);
}

static kb_input_device_record_t *find_device(void *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].snapshot.linux_dev == dev) {
            return &input_devices[i];
        }
    }
    return NULL;
}

static kb_input_device_record_t *alloc_record(void *dev)
{
    kb_input_device_record_t *record = find_device(dev);
    if (record != NULL || dev == NULL) {
        return record;
    }
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].snapshot.linux_dev == NULL) {
            memset(&input_devices[i], 0, sizeof(input_devices[i]));
            input_devices[i].snapshot.linux_dev = dev;
            input_devices[i].snapshot.id = next_device_id++;
            return &input_devices[i];
        }
    }
    return NULL;
}

static void refresh_device_snapshot(kb_input_device_record_t *record)
{
    if (record == NULL || record->snapshot.linux_dev == NULL) {
        return;
    }
    void *dev = record->snapshot.linux_dev;
    copy_known_string(
        record->snapshot.name,
        sizeof(record->snapshot.name),
        linux_input_string(dev, KB_LINUX_INPUT_DEV_NAME_OFFSET),
        "input");
    copy_known_string(
        record->snapshot.phys,
        sizeof(record->snapshot.phys),
        linux_input_string(dev, KB_LINUX_INPUT_DEV_PHYS_OFFSET),
        "");
    copy_known_string(
        record->snapshot.uniq,
        sizeof(record->snapshot.uniq),
        linux_input_string(dev, KB_LINUX_INPUT_DEV_UNIQ_OFFSET),
        "");
    record->snapshot.input_id = linux_input_id(dev);
}

void *kb_input_subsystem_allocate_device(void)
{
    void *dev = calloc(1, KB_INPUT_LINUX_DEVICE_STORAGE_SIZE);
    if (dev == NULL) {
        return NULL;
    }
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL) {
        free(dev);
        return NULL;
    }
    record->allocated = 1;
    return dev;
}

void kb_input_subsystem_free_device(void *dev)
{
    kb_input_device_record_t *record = find_device(dev);
    if (record == NULL) {
        return;
    }
    if (record->allocated) {
        free(dev);
    }
    memset(record, 0, sizeof(*record));
}

int kb_input_subsystem_register_device(void *dev)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL) {
        return -12;
    }
    record->snapshot.active = 1;
    refresh_device_snapshot(record);
    return 0;
}

void kb_input_subsystem_unregister_device(void *dev)
{
    kb_input_device_record_t *record = find_device(dev);
    if (record == NULL) {
        return;
    }
    record->snapshot.active = 0;
    if (record->allocated) {
        free(dev);
        memset(record, 0, sizeof(*record));
    }
}

void kb_input_subsystem_record_event(void *dev, unsigned int type, unsigned int code, int value)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL) {
        return;
    }
    refresh_device_snapshot(record);
    record->snapshot.event_count++;

    size_t index = (input_event_head + input_event_count) % KB_INPUT_EVENT_QUEUE_MAX;
    if (input_event_count == KB_INPUT_EVENT_QUEUE_MAX) {
        index = input_event_head;
        input_event_head = (input_event_head + 1u) % KB_INPUT_EVENT_QUEUE_MAX;
        record->snapshot.dropped_events++;
    } else {
        input_event_count++;
    }

    input_events[index] = (kb_input_event_t){
        .sequence = next_event_sequence++,
        .device_id = record->snapshot.id,
        .linux_dev = dev,
        .type = type,
        .code = code,
        .value = value,
    };
}

void kb_input_subsystem_set_abs_params(
    void *dev,
    unsigned int axis,
    int minimum,
    int maximum,
    int fuzz,
    int flat)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL || axis >= KB_INPUT_ABS_MAX) {
        return;
    }
    record->snapshot.abs[axis] = (kb_input_abs_params_t){
        .active = 1,
        .minimum = minimum,
        .maximum = maximum,
        .fuzz = fuzz,
        .flat = flat,
    };
}

void kb_input_subsystem_alloc_absinfo(void *dev)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record != NULL) {
        record->absinfo_allocated = 1;
    }
}

int kb_input_subsystem_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL) {
        return -12;
    }
    record->snapshot.mt_slots = num_slots;
    record->snapshot.mt_flags = flags;
    return 0;
}

size_t kb_input_subsystem_device_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        count += input_devices[i].snapshot.active != 0;
    }
    return count;
}

int kb_input_subsystem_for_each_device(
    int (*callback)(const kb_input_device_snapshot_t *device, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (!input_devices[i].snapshot.active) {
            continue;
        }
        visited++;
        if (callback(&input_devices[i].snapshot, ctx) != 0) {
            break;
        }
    }
    return visited;
}

size_t kb_input_subsystem_pop_events(kb_input_event_t *events, size_t max_events)
{
    if (events == NULL || max_events == 0) {
        return 0;
    }
    size_t copied = input_event_count < max_events ? input_event_count : max_events;
    for (size_t i = 0; i < copied; i++) {
        events[i] = input_events[(input_event_head + i) % KB_INPUT_EVENT_QUEUE_MAX];
    }
    input_event_head = (input_event_head + copied) % KB_INPUT_EVENT_QUEUE_MAX;
    input_event_count -= copied;
    return copied;
}

size_t kb_input_subsystem_event_count(void)
{
    return input_event_count;
}

void kb_input_subsystem_print_summary(FILE *out)
{
    if (out == NULL) {
        out = stdout;
    }
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        const kb_input_device_snapshot_t *device = &input_devices[i].snapshot;
        if (!device->active) {
            continue;
        }
        fprintf(
            out,
            "kobox-input: device id=%u linux=%p name=%s phys=%s bus=%04x vendor=%04x product=%04x version=%04x events=%lu dropped=%lu slot=%ld\n",
            device->id,
            device->linux_dev,
            device->name[0] == '\0' ? "input" : device->name,
            device->phys,
            device->input_id.bustype,
            device->input_id.vendor,
            device->input_id.product,
            device->input_id.version,
            device->event_count,
            device->dropped_events,
            input_device_index(&input_devices[i]));
    }
}

void kb_input_subsystem_reset(void)
{
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].allocated && input_devices[i].snapshot.linux_dev != NULL) {
            free(input_devices[i].snapshot.linux_dev);
        }
        memset(&input_devices[i], 0, sizeof(input_devices[i]));
    }
    memset(input_events, 0, sizeof(input_events));
    input_event_head = 0;
    input_event_count = 0;
    next_event_sequence = 1;
    next_device_id = 1;
}
