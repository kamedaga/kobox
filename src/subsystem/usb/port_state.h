#ifndef KOBOX_SUBSYSTEM_USB_PORT_STATE_H
#define KOBOX_SUBSYSTEM_USB_PORT_STATE_H

typedef struct kb_usb_port_state {
    void *hcd;
    unsigned long connected_bits;
    unsigned long enabled_bits;
    unsigned long change_bits;
    unsigned long enable_change_bits;
    unsigned long reset_change_bits;
    unsigned long cleared_backend_change_bits;
    unsigned long cleared_backend_enable_change_bits;
    unsigned long cleared_backend_reset_change_bits;
    unsigned int actual_port_for_virtual1;
    struct kb_usb_port_state *next;
} kb_usb_port_state_t;

kb_usb_port_state_t *kb_usb_port_state_for_hcd(void *hcd);
void kb_usb_port_state_release(void *hcd);

#endif
