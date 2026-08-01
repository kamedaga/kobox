#include "linux_subsystem/input/input.h"

#include <string.h>

typedef struct fake_input_dev {
    const char *name;
    const char *phys;
    const char *uniq;
    kb_input_id_t id;
    unsigned char storage[
        KB_INPUT_LINUX_DEVICE_STORAGE_SIZE -
        (3u * sizeof(const char *) + sizeof(kb_input_id_t))];
} fake_input_dev_t;

_Static_assert(sizeof(fake_input_dev_t) == KB_INPUT_LINUX_DEVICE_STORAGE_SIZE,
    "fake input_dev storage size");

enum {
    LINUX_INPUT_DEV_EVBIT_OFFSET = 40,
    LINUX_INPUT_DEV_KEYBIT_OFFSET = 48,
    EV_KEY = 1,
    EV_ABS = 3,
    BTN_LEFT = 0x110,
};

static void set_raw_bit(void *dev, size_t offset, unsigned int bit)
{
    uint64_t word = 0;
    unsigned char *slot = (unsigned char *)dev + offset +
        ((size_t)(bit / 64u) * sizeof(word));
    memcpy(&word, slot, sizeof(word));
    word |= UINT64_C(1) << (bit % 64u);
    memcpy(slot, &word, sizeof(word));
}

typedef struct seen_device {
    int seen;
    kb_input_device_snapshot_t snapshot;
} seen_device_t;

typedef struct selected_device {
    unsigned int id;
    int found;
    kb_input_device_snapshot_t snapshot;
} selected_device_t;

static int capture_device(const kb_input_device_snapshot_t *device, void *ctx)
{
    seen_device_t *seen = ctx;
    seen->seen++;
    seen->snapshot = *device;
    return 0;
}

static int capture_selected_device(
    const kb_input_device_snapshot_t *device,
    void *ctx)
{
    selected_device_t *selected = ctx;
    if (device->id != selected->id) {
        return 0;
    }
    selected->found = 1;
    selected->snapshot = *device;
    return 1;
}

int main(void)
{
    kb_input_subsystem_reset();

    fake_input_dev_t fake = {
        .name = "demo tablet",
        .phys = "usb-demo/input0",
        .uniq = "unit-test",
        .id = {
            .bustype = 3,
            .vendor = 0x1234,
            .product = 0x5678,
            .version = 0x0001,
        },
    };
    set_raw_bit(&fake, LINUX_INPUT_DEV_EVBIT_OFFSET, EV_KEY);
    set_raw_bit(&fake, LINUX_INPUT_DEV_KEYBIT_OFFSET, BTN_LEFT);

    if (kb_input_subsystem_register_device(&fake) != 0) {
        return 1;
    }
    if (kb_input_subsystem_device_count() != 1) {
        return 2;
    }

    kb_input_subsystem_set_abs_params(&fake, 0, -100, 100, 1, 2);
    kb_input_subsystem_record_event(&fake, 3, 0, 42);
    kb_input_event_t events[4];
    if (kb_input_subsystem_pop_events(events, 4) != 0) {
        return 25;
    }
    kb_input_subsystem_record_event(&fake, 0, 0, 0);

    seen_device_t seen;
    memset(&seen, 0, sizeof(seen));
    if (kb_input_subsystem_for_each_device(capture_device, &seen) != 1 || seen.seen != 1) {
        return 4;
    }
    if (strcmp(seen.snapshot.name, "demo tablet") != 0 ||
        strcmp(seen.snapshot.phys, "usb-demo/input0") != 0 ||
        seen.snapshot.input_id.vendor != 0x1234 ||
        seen.snapshot.abs[0].value != 42 ||
        seen.snapshot.abs[0].minimum != -100 ||
        seen.snapshot.abs[0].maximum != 100 ||
        seen.snapshot.event_count != 2)
    {
        return 5;
    }

    if (kb_input_subsystem_pop_events(events, 4) != 2) {
        return 6;
    }
    if (events[0].device_id != seen.snapshot.id ||
        events[0].type != 3 ||
        events[0].code != 0 ||
        events[0].value != 42 ||
        events[1].sequence != events[0].sequence + 1)
    {
        return 7;
    }
    if (kb_input_subsystem_event_count() != 0) {
        return 8;
    }

    kb_input_subsystem_record_event(&fake, 3, 0, 42);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 0) {
        return 13;
    }

    kb_input_subsystem_record_event(&fake, EV_KEY, BTN_LEFT, 1);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 2 ||
        events[0].type != EV_KEY || events[0].code != BTN_LEFT ||
        events[0].value != 1 || events[1].type != 0)
    {
        return 14;
    }
    memset(&seen, 0, sizeof(seen));
    (void)kb_input_subsystem_for_each_device(capture_device, &seen);
    if ((seen.snapshot.key_state[BTN_LEFT / 64u] &
         (UINT64_C(1) << (BTN_LEFT % 64u))) == 0)
    {
        return 15;
    }

    kb_input_subsystem_record_event(&fake, EV_KEY, BTN_LEFT, 1);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 0) {
        return 16;
    }

    kb_input_subsystem_record_event(&fake, EV_KEY, BTN_LEFT, 0);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 2 ||
        events[0].type != EV_KEY || events[0].value != 0)
    {
        return 17;
    }
    memset(&seen, 0, sizeof(seen));
    (void)kb_input_subsystem_for_each_device(capture_device, &seen);
    if ((seen.snapshot.key_state[BTN_LEFT / 64u] &
         (UINT64_C(1) << (BTN_LEFT % 64u))) != 0)
    {
        return 18;
    }

    const unsigned int primary_id = seen.snapshot.id;
    fake_input_dev_t second = { .name = "second tablet" };
    if (kb_input_subsystem_register_device(&second) != 0) {
        return 27;
    }
    kb_input_subsystem_set_abs_params(&second, 0, 0, 10000, 0, 0);
    kb_input_subsystem_record_event(&second, EV_ABS, 0, 100);
    kb_input_subsystem_record_event(&second, 0, 0, 0);
    kb_input_subsystem_record_event(&second, EV_ABS, 0, 200);

    const size_t fill_frames = (KB_INPUT_EVENT_QUEUE_MAX - 2u) / 2u;
    for (size_t frame = 0; frame < fill_frames; frame++) {
        kb_input_subsystem_record_event(&fake, EV_ABS, 0, 1000 + (int)frame * 10);
        kb_input_subsystem_record_event(&fake, 0, 0, 0);
    }
    if (kb_input_subsystem_event_count() != KB_INPUT_EVENT_QUEUE_MAX) {
        return 19;
    }

    const int overflow_value = 1000 +
        (int)fill_frames * 10;
    kb_input_subsystem_record_event(&fake, EV_ABS, 0, overflow_value);
    kb_input_subsystem_record_event(&fake, EV_ABS, 0, overflow_value + 10);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 2 ||
        events[0].type != 0 || events[0].code != 3 ||
        events[0].device_id != primary_id ||
        events[1].type != 0 || events[1].code != 0 ||
        events[1].device_id != primary_id)
    {
        return 20;
    }
    selected_device_t selected = { .id = primary_id };
    (void)kb_input_subsystem_for_each_device(capture_selected_device, &selected);
    if (!selected.found || selected.snapshot.abs[0].value != overflow_value + 10 ||
        selected.snapshot.dropped_events < KB_INPUT_EVENT_QUEUE_MAX - 1u)
    {
        return 21;
    }

    if (kb_input_subsystem_event_count() != 0) {
        return 28;
    }
    kb_input_subsystem_record_event(&second, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 2 ||
        events[0].type != 0 || events[0].code != 3 ||
        events[1].type != 0 || events[1].code != 0 ||
        events[0].device_id == primary_id ||
        events[1].device_id != events[0].device_id)
    {
        return 29;
    }
    selected = (selected_device_t){ .id = events[0].device_id };
    (void)kb_input_subsystem_for_each_device(capture_selected_device, &selected);
    if (!selected.found || selected.snapshot.abs[0].value != 200) {
        return 30;
    }

    kb_input_subsystem_record_event(&fake, EV_ABS, 0, overflow_value + 20);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);
    if (kb_input_subsystem_pop_events(events, 4) != 2 ||
        events[0].type != EV_ABS || events[0].value != overflow_value + 20 ||
        events[1].type != 0 || events[1].code != 0)
    {
        return 22;
    }

    kb_input_subsystem_unregister_device(&second);

    kb_input_subsystem_record_event(&fake, EV_ABS, 0, overflow_value + 30);
    if (kb_input_subsystem_event_count() != 0) {
        return 26;
    }
    kb_input_subsystem_unregister_device(&fake);
    if (kb_input_subsystem_device_count() != 0 ||
        kb_input_subsystem_event_count() != 0)
    {
        return 9;
    }

    fake_input_dev_t *allocated = kb_input_subsystem_allocate_device();
    if (allocated == 0) {
        return 10;
    }
    allocated->name = "allocated";
    if (kb_input_subsystem_register_device(allocated) != 0) {
        return 11;
    }
    kb_input_subsystem_unregister_device(allocated);
    if (kb_input_subsystem_device_count() != 0) {
        return 12;
    }

    fake_input_dev_t mt_fake = { .name = "unsupported mt" };
    kb_input_subsystem_set_abs_params(&mt_fake, 0x2f, 0, 9, 0, 0);
    if (kb_input_subsystem_mt_init_slots(&mt_fake, 5, 0) != 0) {
        return 23;
    }
    if (kb_input_subsystem_register_device(&mt_fake) != -95) {
        return 24;
    }
    kb_input_subsystem_free_device(&mt_fake);

    fake_input_dev_t preregister = { .name = "preregistered tablet" };
    kb_input_subsystem_set_abs_params(&preregister, 0, 0, 1000, 0, 0);
    kb_input_subsystem_record_event(&preregister, EV_ABS, 0, 123);
    kb_input_subsystem_record_event(&preregister, 0, 0, 0);
    if (kb_input_subsystem_event_count() != 0 ||
        kb_input_subsystem_register_device(&preregister) != 0)
    {
        return 31;
    }
    kb_input_subsystem_record_event(&preregister, 0, 0, 0);
    if (kb_input_subsystem_event_count() != 0) {
        return 32;
    }
    kb_input_subsystem_unregister_device(&preregister);

    return 0;
}
