#ifndef KOBOX_SUBSYSTEM_USB_SYNTHETIC_H
#define KOBOX_SUBSYSTEM_USB_SYNTHETIC_H

#include <stdint.h>

int kb_usb_synthetic_control_msg(
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size);

#endif
