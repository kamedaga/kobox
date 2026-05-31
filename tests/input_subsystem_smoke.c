#include "subsystem/input/input.h"

#include <string.h>

typedef struct fake_input_dev {
    const char *name;
    const char *phys;
    const char *uniq;
    kb_input_id_t id;
} fake_input_dev_t;

typedef struct seen_device {
    int seen;
    kb_input_device_snapshot_t snapshot;
} seen_device_t;

static int capture_device(const kb_input_device_snapshot_t *device, void *ctx)
{
    seen_device_t *seen = ctx;
    seen->seen++;
    seen->snapshot = *device;
    return 0;
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

    if (kb_input_subsystem_register_device(&fake) != 0) {
        return 1;
    }
    if (kb_input_subsystem_device_count() != 1) {
        return 2;
    }

    kb_input_subsystem_set_abs_params(&fake, 0, -100, 100, 1, 2);
    if (kb_input_subsystem_mt_init_slots(&fake, 5, 7) != 0) {
        return 3;
    }
    kb_input_subsystem_record_event(&fake, 3, 0, 42);
    kb_input_subsystem_record_event(&fake, 0, 0, 0);

    seen_device_t seen;
    memset(&seen, 0, sizeof(seen));
    if (kb_input_subsystem_for_each_device(capture_device, &seen) != 1 || seen.seen != 1) {
        return 4;
    }
    if (strcmp(seen.snapshot.name, "demo tablet") != 0 ||
        strcmp(seen.snapshot.phys, "usb-demo/input0") != 0 ||
        seen.snapshot.input_id.vendor != 0x1234 ||
        seen.snapshot.abs[0].minimum != -100 ||
        seen.snapshot.abs[0].maximum != 100 ||
        seen.snapshot.mt_slots != 5 ||
        seen.snapshot.event_count != 2)
    {
        return 5;
    }

    kb_input_event_t events[4];
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

    kb_input_subsystem_unregister_device(&fake);
    if (kb_input_subsystem_device_count() != 0) {
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

    return 0;
}
