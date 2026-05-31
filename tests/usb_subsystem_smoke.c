#include "subsystem/usb/usb.h"

static int expect_event(
    kb_usb_hub_event_update_t *event,
    unsigned long bits,
    unsigned long new_bits,
    unsigned long injected_before,
    unsigned long injected_after)
{
    if (event->bits != bits ||
        event->new_bits != new_bits ||
        event->injected_before != injected_before ||
        event->injected_after != injected_after)
    {
        return 1;
    }
    return event->slot < 0 ? 2 : 0;
}

int main(void)
{
    int fake_hcd_storage;
    void *hcd = &fake_hcd_storage;
    unsigned char port1_changed[1] = { 0x02 };
    unsigned char no_change[1] = { 0x00 };
    kb_usb_hub_event_update_t event;

    if (kb_usb_subsystem_hub_event_prepare(hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 1;
    }
    if (event.status_len != 1 || event.status[0] != port1_changed[0]) {
        return 2;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x00) != 0) {
        return 3;
    }

    if (kb_usb_subsystem_hub_event_commit(hcd, event.new_bits, &event) != 0) {
        return 4;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x02) != 0) {
        return 5;
    }

    if (kb_usb_subsystem_hub_event_prepare(hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 6;
    }
    if (expect_event(&event, 0x02, 0x00, 0x02, 0x02) != 0) {
        return 7;
    }

    if (kb_usb_subsystem_hub_event_prepare(hcd, no_change, sizeof(no_change), &event) != 0) {
        return 8;
    }
    if (expect_event(&event, 0x00, 0x00, 0x02, 0x00) != 0) {
        return 9;
    }

    if (kb_usb_subsystem_hub_event_prepare(hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 10;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x00) != 0) {
        return 11;
    }

    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_alloc(64);
    if (record == 0 || record->hcd == 0) {
        return 12;
    }
    void *allocated_hcd = record->hcd;

    if (kb_usb_subsystem_hub_event_prepare(allocated_hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 13;
    }
    if (kb_usb_subsystem_hub_event_commit(allocated_hcd, event.new_bits, &event) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 14;
    }
    kb_usb_subsystem_hcd_release(record);

    if (kb_usb_subsystem_hub_event_prepare(allocated_hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 15;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x00) != 0) {
        return 16;
    }

    return 0;
}
