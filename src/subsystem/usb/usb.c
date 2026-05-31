#include "subsystem/usb/usb.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_USB_HCD_MAX = 16,
};

static kb_usb_hcd_record_t usb_hcd_records[KB_USB_HCD_MAX];

typedef struct kb_usb_hub_event_injection {
    void *hcd;
    unsigned long bits;
} kb_usb_hub_event_injection_t;

static kb_usb_hub_event_injection_t usb_hub_event_injections[KB_USB_HCD_MAX];

static long hub_event_injection_index(const kb_usb_hub_event_injection_t *injection)
{
    if (injection == NULL) {
        return -1;
    }
    return (long)(injection - usb_hub_event_injections);
}

static kb_usb_hub_event_injection_t *hub_event_injection_find(void *hcd)
{
    if (hcd == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_HCD_MAX; i++) {
        if (usb_hub_event_injections[i].hcd == hcd) {
            return &usb_hub_event_injections[i];
        }
    }
    return NULL;
}

static kb_usb_hub_event_injection_t *hub_event_injection_for_hcd(void *hcd)
{
    kb_usb_hub_event_injection_t *injection = hub_event_injection_find(hcd);
    if (injection != NULL || hcd == NULL) {
        return injection;
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

static unsigned long hub_status_bits(const unsigned char *status, size_t status_len)
{
    unsigned long bits = 0;
    size_t limit = status_len;
    if (limit > sizeof(bits)) {
        limit = sizeof(bits);
    }
    for (size_t i = 0; i < limit; i++) {
        bits |= ((unsigned long)status[i]) << (i * 8);
    }
    return bits;
}

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
    kb_usb_hub_event_injection_t *injection = hub_event_injection_find(record->hcd);
    if (injection != NULL) {
        memset(injection, 0, sizeof(*injection));
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

int kb_usb_subsystem_hub_event_prepare(
    void *hcd,
    const unsigned char *status,
    size_t status_len,
    kb_usb_hub_event_update_t *update)
{
    if (update != NULL) {
        memset(update, 0, sizeof(*update));
        update->slot = -1;
    }

    if (hcd == NULL || status == NULL || status_len == 0) {
        return -22;
    }

    kb_usb_hub_event_injection_t *injection = hub_event_injection_for_hcd(hcd);
    if (injection == NULL) {
        return -12;
    }

    size_t copied = status_len;
    if (copied > KB_USB_HUB_STATUS_MAX) {
        copied = KB_USB_HUB_STATUS_MAX;
    }

    unsigned long bits = hub_status_bits(status, status_len);
    unsigned long injected_before = injection->bits;
    if (bits == 0) {
        injection->bits = 0;
    }
    unsigned long new_bits = bits & ~injection->bits;

    if (update != NULL) {
        memcpy(update->status, status, copied);
        update->status_len = (int)copied;
        update->bits = bits;
        update->new_bits = new_bits;
        update->injected_before = injected_before;
        update->injected_after = injection->bits;
        update->slot = hub_event_injection_index(injection);
    }
    return 0;
}

int kb_usb_subsystem_hub_event_commit(
    void *hcd,
    unsigned long bits,
    kb_usb_hub_event_update_t *update)
{
    if (hcd == NULL) {
        return -22;
    }

    kb_usb_hub_event_injection_t *injection = hub_event_injection_for_hcd(hcd);
    if (injection == NULL) {
        return -12;
    }

    unsigned long injected_before = injection->bits;
    injection->bits |= bits;

    if (update != NULL) {
        if (update->slot < 0) {
            update->slot = hub_event_injection_index(injection);
        }
        update->injected_before = injected_before;
        update->injected_after = injection->bits;
    }
    return 0;
}
