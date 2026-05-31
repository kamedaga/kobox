#pragma once

#include <stddef.h>

typedef struct kb_usb_hcd_record {
    int active;
    int primary;
    void *owner;
    const void *driver;
    void *hcd;
    void *primary_hcd;
    void *regs;
    unsigned int irq;
    int irq_registered;
    int added;
} kb_usb_hcd_record_t;

typedef struct kb_usb_hub_event_injection {
    void *hcd;
    unsigned long bits;
} kb_usb_hub_event_injection_t;

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_alloc(size_t storage_size);
kb_usb_hcd_record_t *kb_usb_subsystem_hcd_for_hcd(void *hcd);
kb_usb_hcd_record_t *kb_usb_subsystem_primary_hcd_for_owner(void *owner);
void kb_usb_subsystem_hcd_release(kb_usb_hcd_record_t *record);
int kb_usb_subsystem_for_each_hcd(int (*callback)(kb_usb_hcd_record_t *record, void *ctx), void *ctx);
kb_usb_hub_event_injection_t *kb_usb_subsystem_hub_event_injection_for_hcd(void *hcd);
long kb_usb_subsystem_hub_event_injection_index(const kb_usb_hub_event_injection_t *injection);
