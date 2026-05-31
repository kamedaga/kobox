#include "subsystem/usb.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_USB_HCD_MAX = 16,
};

static kb_usb_hcd_record_t usb_hcd_records[KB_USB_HCD_MAX];
static kb_usb_hub_event_injection_t usb_hub_event_injections[KB_USB_HCD_MAX];

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_alloc(size_t storage_size)
{
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (usb_hcd_records[i].active) {
            continue;
        }
        memset(&usb_hcd_records[i], 0, sizeof(usb_hcd_records[i]));
        usb_hcd_records[i].hcd = calloc(1, storage_size);
        if (usb_hcd_records[i].hcd == NULL) {
            return NULL;
        }
        usb_hcd_records[i].active = 1;
        return &usb_hcd_records[i];
    }
    return NULL;
}

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_for_hcd(void *hcd)
{
    if (hcd == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (usb_hcd_records[i].active && usb_hcd_records[i].hcd == hcd) {
            return &usb_hcd_records[i];
        }
    }
    return NULL;
}

kb_usb_hcd_record_t *kb_usb_subsystem_primary_hcd_for_owner(void *owner)
{
    if (owner == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (usb_hcd_records[i].active && usb_hcd_records[i].owner == owner && usb_hcd_records[i].primary) {
            return &usb_hcd_records[i];
        }
    }
    return NULL;
}

void kb_usb_subsystem_hcd_release(kb_usb_hcd_record_t *record)
{
    if (record == NULL || !record->active) {
        return;
    }
    free(record->hcd);
    memset(record, 0, sizeof(*record));
}

int kb_usb_subsystem_for_each_hcd(int (*callback)(kb_usb_hcd_record_t *record, void *ctx), void *ctx)
{
    if (callback == NULL) {
        return 0;
    }

    int visited = 0;
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (!usb_hcd_records[i].active || usb_hcd_records[i].hcd == NULL) {
            continue;
        }
        visited++;
        if (callback(&usb_hcd_records[i], ctx) != 0) {
            break;
        }
    }
    return visited;
}

kb_usb_hub_event_injection_t *kb_usb_subsystem_hub_event_injection_for_hcd(void *hcd)
{
    if (hcd == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (usb_hub_event_injections[i].hcd == hcd) {
            return &usb_hub_event_injections[i];
        }
    }
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (usb_hub_event_injections[i].hcd == NULL) {
            usb_hub_event_injections[i].hcd = hcd;
            usb_hub_event_injections[i].bits = 0;
            return &usb_hub_event_injections[i];
        }
    }
    return NULL;
}

long kb_usb_subsystem_hub_event_injection_index(const kb_usb_hub_event_injection_t *injection)
{
    if (injection == NULL) {
        return -1;
    }
    return (long)(injection - usb_hub_event_injections);
}
