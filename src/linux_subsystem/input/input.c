#include "linux_subsystem/input/input.h"
#include "kobox/shim.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_INPUT_DEVICE_MAX = 64,
    KB_LINUX_INPUT_DEV_NAME_OFFSET = 0,
    KB_LINUX_INPUT_DEV_PHYS_OFFSET = 8,
    KB_LINUX_INPUT_DEV_UNIQ_OFFSET = 16,
    KB_LINUX_INPUT_DEV_ID_OFFSET = 24,
    KB_LINUX_6_8_INPUT_DEV_PROPBIT_OFFSET = 32,
    KB_LINUX_6_8_INPUT_DEV_EVBIT_OFFSET = 40,
    KB_LINUX_6_8_INPUT_DEV_KEYBIT_OFFSET = 48,
    KB_LINUX_6_8_INPUT_DEV_RELBIT_OFFSET = 144,
    KB_LINUX_6_8_INPUT_DEV_ABSBIT_OFFSET = 152,
    KB_LINUX_6_8_INPUT_DEV_MSCBIT_OFFSET = 160,
    KB_LINUX_6_8_INPUT_DEV_LEDBIT_OFFSET = 168,
    KB_LINUX_6_8_INPUT_DEV_SNDBIT_OFFSET = 176,
    KB_LINUX_6_8_INPUT_DEV_FFBIT_OFFSET = 184,
    KB_LINUX_6_8_INPUT_DEV_SWBIT_OFFSET = 200,
    KB_LINUX_6_8_INPUT_DEV_ABSINFO_OFFSET = 328,
    KB_LINUX_6_8_INPUT_DEV_OPEN_OFFSET = 456,
    KB_LINUX_6_8_INPUT_DEV_CLOSE_OFFSET = 464,
    KB_LINUX_6_8_INPUT_ABSINFO_SIZE = 24,
};

typedef struct kb_input_device_record {
    kb_input_device_snapshot_t snapshot;
    int allocated;
    int absinfo_allocated;
    void *absinfo;
} kb_input_device_record_t;

typedef int (*kb_linux_input_open_fn)(void *dev);
typedef void (*kb_linux_input_close_fn)(void *dev);

static kb_input_device_record_t input_devices[KB_INPUT_DEVICE_MAX];
static kb_input_event_t input_events[KB_INPUT_EVENT_QUEUE_MAX];
static size_t input_event_head;
static size_t input_event_count;
static uint64_t next_event_sequence = 1;
static unsigned int next_device_id = 1;

static int trace_input_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_TRACE_INPUT");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    return cached;
}

static int pointer_is_error_or_low(const void *ptr)
{
    const uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static int trust_device_strings_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_INPUT_TRUST_DEVICE_STRINGS");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    return cached;
}

static size_t bounded_strlen(const char *value, size_t max_len)
{
    size_t length = 0;
    if (value == NULL) {
        return 0;
    }
    while (length < max_len && value[length] != '\0') {
        length++;
    }
    return length;
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
    size_t length = bounded_strlen(value, dst_size - 1u);
    memcpy(dst, value, length);
    dst[length] = '\0';
}

static const char *summary_string(char *scratch, size_t scratch_size, const char *value, const char *fallback)
{
    if (scratch_size == 0) {
        return "";
    }
    if (value == NULL || value[0] == '\0') {
        value = fallback != NULL ? fallback : "";
    }
    size_t length = bounded_strlen(value, scratch_size - 1u);
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)value[i];
        scratch[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    scratch[length] = '\0';
    return scratch;
}

static const char *linux_input_string(void *dev, size_t offset)
{
    if (dev == NULL) {
        return NULL;
    }
    const char *const *slot = (const char *const *)((unsigned char *)dev + offset);
    return *slot;
}

static void *linux_input_ptr(void *dev, size_t offset)
{
    void *value = NULL;
    if (dev != NULL) {
        memcpy(&value, (unsigned char *)dev + offset, sizeof(value));
    }
    return value;
}

static int enter_function_gs(const void *function, unsigned long *old_gs)
{
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(function);
    if (kernel_gs == 0) {
        kernel_gs = kb_shim_current_kernel_gs();
    }
    if (kernel_gs == 0) {
        return 0;
    }
    return kb_shim_enter_kernel_gs(kernel_gs, old_gs) == 0;
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

static kb_input_device_record_t *fallback_event_record(void)
{
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].snapshot.active && input_devices[i].snapshot.opened) {
            return &input_devices[i];
        }
    }
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].snapshot.active) {
            return &input_devices[i];
        }
    }
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].snapshot.id == 0 && input_devices[i].snapshot.linux_dev == NULL) {
            memset(&input_devices[i], 0, sizeof(input_devices[i]));
            input_devices[i].snapshot.id = next_device_id++;
            input_devices[i].snapshot.active = 1;
            snprintf(input_devices[i].snapshot.name, sizeof(input_devices[i].snapshot.name), "%s", "input");
            return &input_devices[i];
        }
    }
    return NULL;
}

static int input_record_can_open(const kb_input_device_record_t *record)
{
    return record != NULL &&
        record->snapshot.active &&
        !record->snapshot.opened &&
        record->snapshot.linux_dev != NULL;
}

static int input_record_can_close(const kb_input_device_record_t *record)
{
    return record != NULL &&
        record->snapshot.active &&
        record->snapshot.opened &&
        record->snapshot.linux_dev != NULL;
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
        trust_device_strings_enabled() ? linux_input_string(dev, KB_LINUX_INPUT_DEV_NAME_OFFSET) : NULL,
        "input");
    copy_known_string(
        record->snapshot.phys,
        sizeof(record->snapshot.phys),
        trust_device_strings_enabled() ? linux_input_string(dev, KB_LINUX_INPUT_DEV_PHYS_OFFSET) : NULL,
        "");
    copy_known_string(
        record->snapshot.uniq,
        sizeof(record->snapshot.uniq),
        trust_device_strings_enabled() ? linux_input_string(dev, KB_LINUX_INPUT_DEV_UNIQ_OFFSET) : NULL,
        "");
    record->snapshot.input_id = linux_input_id(dev);
    memcpy(&record->snapshot.prop_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_PROPBIT_OFFSET, sizeof(record->snapshot.prop_bits));
    memcpy(&record->snapshot.event_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_EVBIT_OFFSET, sizeof(record->snapshot.event_bits));
    memcpy(record->snapshot.key_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_KEYBIT_OFFSET, sizeof(record->snapshot.key_bits));
    memcpy(&record->snapshot.rel_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_RELBIT_OFFSET, sizeof(record->snapshot.rel_bits));
    memcpy(&record->snapshot.abs_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_ABSBIT_OFFSET, sizeof(record->snapshot.abs_bits));
    memcpy(&record->snapshot.msc_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_MSCBIT_OFFSET, sizeof(record->snapshot.msc_bits));
    memcpy(&record->snapshot.led_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_LEDBIT_OFFSET, sizeof(record->snapshot.led_bits));
    memcpy(&record->snapshot.snd_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_SNDBIT_OFFSET, sizeof(record->snapshot.snd_bits));
    memcpy(record->snapshot.ff_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_FFBIT_OFFSET, sizeof(record->snapshot.ff_bits));
    memcpy(&record->snapshot.sw_bits, (unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_SWBIT_OFFSET, sizeof(record->snapshot.sw_bits));
    if (record->absinfo != NULL) {
        for (size_t axis = 0; axis < KB_INPUT_ABS_MAX; axis++) {
            int values[6];
            memcpy(values, (unsigned char *)record->absinfo +
                (axis * KB_LINUX_6_8_INPUT_ABSINFO_SIZE), sizeof(values));
            if (((record->snapshot.abs_bits >> axis) & 1u) != 0) {
                record->snapshot.abs[axis] = (kb_input_abs_params_t){
                    .active = 1,
                    .value = values[0],
                    .minimum = values[1],
                    .maximum = values[2],
                    .fuzz = values[3],
                    .flat = values[4],
                    .resolution = values[5],
                };
            }
        }
    }
}

static int open_input_record(kb_input_device_record_t *record)
{
    if (!input_record_can_open(record)) {
        return 0;
    }

    void *dev = record->snapshot.linux_dev;
    kb_linux_input_open_fn open_fn =
        (kb_linux_input_open_fn)linux_input_ptr(dev, KB_LINUX_6_8_INPUT_DEV_OPEN_OFFSET);
    if (open_fn == NULL || pointer_is_error_or_low((const void *)open_fn)) {
        record->snapshot.open_result = 0;
        record->snapshot.opened = 1;
        return 1;
    }

    unsigned long old_gs = 0;
    if (trace_input_enabled()) {
        fprintf(stderr, "kobox input: open begin dev=%p open=%p\n", dev, (void *)open_fn);
    }
    int has_gs = enter_function_gs((const void *)open_fn, &old_gs);
    if (trace_input_enabled()) {
        fprintf(stderr, "kobox input: open call dev=%p open=%p has_gs=%d old_gs=0x%lx\n", dev, (void *)open_fn, has_gs, old_gs);
    }
    int result = kb_linux_call_int_ptr(open_fn, dev);
    if (trace_input_enabled()) {
        fprintf(stderr, "kobox input: open returned dev=%p open=%p result=%d\n", dev, (void *)open_fn, result);
    }
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    record->snapshot.open_result = result;
    if (result == 0) {
        record->snapshot.opened = 1;
    }
    if (trace_input_enabled()) {
        fprintf(
            stderr,
            "kobox input: open dev=%p open=%p result=%d opened=%d\n",
            dev,
            (void *)open_fn,
            result,
            record->snapshot.opened);
    }
    return result == 0;
}

static void close_input_record(kb_input_device_record_t *record)
{
    if (!input_record_can_close(record)) {
        return;
    }

    void *dev = record->snapshot.linux_dev;
    kb_linux_input_close_fn close_fn =
        (kb_linux_input_close_fn)linux_input_ptr(dev, KB_LINUX_6_8_INPUT_DEV_CLOSE_OFFSET);
    if (close_fn != NULL && !pointer_is_error_or_low((const void *)close_fn)) {
        unsigned long old_gs = 0;
        int has_gs = enter_function_gs((const void *)close_fn, &old_gs);
        kb_linux_call_void_ptr(close_fn, dev);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    if (trace_input_enabled()) {
        fprintf(stderr, "kobox input: close dev=%p close=%p\n", dev, (void *)close_fn);
    }
    record->snapshot.opened = 0;
}

void *kb_input_subsystem_allocate_device(void)
{
    void *dev = kb_kzalloc(KB_INPUT_LINUX_DEVICE_STORAGE_SIZE, 0);
    if (dev == NULL) {
        return NULL;
    }
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL) {
        kb_kfree(dev);
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
    free(record->absinfo);
    record->absinfo = NULL;
    if (record->allocated) {
        kb_kfree(dev);
    }
    memset(record, 0, sizeof(*record));
}

int kb_input_subsystem_register_device(void *dev)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record == NULL) {
        return -12;
    }
    *(unsigned long *)((unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_EVBIT_OFFSET) |= 1ul;
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
    close_input_record(record);
    record->snapshot.active = 0;
    if (record->allocated) {
        free(record->absinfo);
        record->absinfo = NULL;
        kb_kfree(dev);
        memset(record, 0, sizeof(*record));
    }
}

void kb_input_subsystem_record_event(void *dev, unsigned int type, unsigned int code, int value)
{
    kb_input_device_record_t *record = dev != NULL ? alloc_record(dev) : fallback_event_record();
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
        .linux_dev = record->snapshot.linux_dev,
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
    kb_input_subsystem_alloc_absinfo(dev);
    if (record->absinfo != NULL) {
        int values[6] = { 0, minimum, maximum, fuzz, flat, 0 };
        memcpy((unsigned char *)record->absinfo +
            ((size_t)axis * KB_LINUX_6_8_INPUT_ABSINFO_SIZE), values, sizeof(values));
    }
}

void kb_input_subsystem_alloc_absinfo(void *dev)
{
    kb_input_device_record_t *record = alloc_record(dev);
    if (record != NULL && !record->absinfo_allocated) {
        record->absinfo = calloc(KB_INPUT_ABS_MAX, KB_LINUX_6_8_INPUT_ABSINFO_SIZE);
        if (record->absinfo == NULL) {
            return;
        }
        memcpy((unsigned char *)dev + KB_LINUX_6_8_INPUT_DEV_ABSINFO_OFFSET,
            &record->absinfo, sizeof(record->absinfo));
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

int kb_input_subsystem_open_registered_devices(void)
{
    int opened = 0;
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (open_input_record(&input_devices[i])) {
            opened++;
        }
    }
    return opened;
}

void kb_input_subsystem_close_registered_devices(void)
{
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        close_input_record(&input_devices[i]);
    }
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
        char name[KB_INPUT_NAME_MAX];
        char phys[KB_INPUT_PHYS_MAX];
        fprintf(
            out,
            "kobox-input: device id=%u linux=%p name=%s phys=%s bus=%04x vendor=%04x product=%04x version=%04x events=%lu dropped=%lu opened=%d open_result=%d slot=%ld\n",
            device->id,
            device->linux_dev,
            summary_string(name, sizeof(name), device->name, "input"),
            summary_string(phys, sizeof(phys), device->phys, ""),
            device->input_id.bustype,
            device->input_id.vendor,
            device->input_id.product,
            device->input_id.version,
            device->event_count,
            device->dropped_events,
            device->opened,
            device->open_result,
            input_device_index(&input_devices[i]));
    }
}

int kb_input_subsystem_run_mouse_smoke(FILE *out)
{
    enum {
        EV_SYN = 0x00,
        EV_KEY = 0x01,
        EV_REL = 0x02,
        EV_ABS = 0x03,
        REL_X = 0x00,
        REL_Y = 0x01,
        ABS_X = 0x00,
        ABS_Y = 0x01,
        BTN_LEFT = 0x110,
    };

    if (out == NULL) {
        out = stdout;
    }

    kb_input_event_t events[KB_INPUT_EVENT_QUEUE_MAX];
    size_t count = kb_input_subsystem_pop_events(events, KB_INPUT_EVENT_QUEUE_MAX);
    int x = 0;
    int y = 0;
    int left = 0;
    unsigned int rel_events = 0;
    unsigned int abs_events = 0;
    unsigned int key_events = 0;
    unsigned int syn_events = 0;
    unsigned int device_id = 0;

    for (size_t i = 0; i < count; i++) {
        if (device_id == 0) {
            device_id = events[i].device_id;
        }
        if (events[i].type == EV_REL && events[i].code == REL_X) {
            x += events[i].value;
            rel_events++;
        } else if (events[i].type == EV_REL && events[i].code == REL_Y) {
            y += events[i].value;
            rel_events++;
        } else if (events[i].type == EV_ABS && events[i].code == ABS_X) {
            x = events[i].value;
            abs_events++;
        } else if (events[i].type == EV_ABS && events[i].code == ABS_Y) {
            y = events[i].value;
            abs_events++;
        } else if (events[i].type == EV_KEY && events[i].code == BTN_LEFT) {
            left = events[i].value != 0;
            key_events++;
        } else if (events[i].type == EV_SYN) {
            syn_events++;
        }
    }

    fprintf(
        out,
        "kobox-usb-hid-mouse: device_id=%u events=%zu rel=%u abs=%u key=%u syn=%u x=%d y=%d left=%d result=%s\n",
        device_id,
        count,
        rel_events,
        abs_events,
        key_events,
        syn_events,
        x,
        y,
        left,
        count != 0 && (rel_events >= 2 || abs_events >= 2) ? "ok" : "no-events");
    return count != 0 && (rel_events >= 2 || abs_events >= 2) ? 0 : -5;
}

void kb_input_subsystem_reset(void)
{
    kb_input_subsystem_close_registered_devices();
    for (size_t i = 0; i < KB_INPUT_DEVICE_MAX; i++) {
        if (input_devices[i].allocated && input_devices[i].snapshot.linux_dev != NULL) {
            free(input_devices[i].absinfo);
            input_devices[i].absinfo = NULL;
            kb_kfree(input_devices[i].snapshot.linux_dev);
        }
        memset(&input_devices[i], 0, sizeof(input_devices[i]));
    }
    memset(input_events, 0, sizeof(input_events));
    input_event_head = 0;
    input_event_count = 0;
    next_event_sequence = 1;
    next_device_id = 1;
}
