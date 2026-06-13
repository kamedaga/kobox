#include "synthetic.h"

#include "config.h"

#include <stdint.h>
#include <string.h>

static int pointer_is_error_or_low(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static size_t copy_descriptor(void *data, uint16_t size, const unsigned char *descriptor, size_t descriptor_len)
{
    if (pointer_is_error_or_low(data) || size == 0 || descriptor == NULL || descriptor_len == 0) {
        return 0;
    }
    size_t copied = descriptor_len;
    if (copied > size) {
        copied = size;
    }
    memcpy(data, descriptor, copied);
    return copied;
}

int kb_usb_synthetic_control_msg(
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size)
{
    enum {
        USB_DIR_IN = 0x80,
        USB_TYPE_STANDARD = 0x00,
        USB_RECIP_DEVICE = 0x00,
        USB_RECIP_INTERFACE = 0x01,
        USB_REQ_GET_DESCRIPTOR = 0x06,
        USB_REQ_SET_ADDRESS = 0x05,
        USB_REQ_SET_CONFIGURATION = 0x09,
        USB_DT_DEVICE = 0x01,
        USB_DT_CONFIG = 0x02,
        USB_DT_STRING = 0x03,
        USB_DT_DEVICE_QUALIFIER = 0x06,
        USB_DT_HID = 0x21,
        USB_DT_REPORT = 0x22,
    };

    if (!usb_event_injection_enabled()) {
        return -95;
    }
    if (usb_real_device_enabled()) {
        return -95;
    }
    if ((requesttype & (USB_DIR_IN | 0x60 | 0x1f)) == (USB_TYPE_STANDARD | USB_RECIP_DEVICE)) {
        if (request == USB_REQ_SET_ADDRESS || request == USB_REQ_SET_CONFIGURATION) {
            return 0;
        }
    }
    if (request != USB_REQ_GET_DESCRIPTOR) {
        return -95;
    }

    static const unsigned char hid_device_descriptor[] = {
        18, USB_DT_DEVICE,
        0x00, 0x02,
        0x00, 0x00, 0x00,
        64,
        0x6b, 0x1d,
        0x02, 0x10,
        0x00, 0x01,
        1, 2, 3,
        1,
    };
    static const unsigned char hid_config_descriptor[] = {
        9, USB_DT_CONFIG,
        34, 0,
        1,
        1,
        0,
        0x80,
        50,
        9, 0x04,
        0,
        0,
        1,
        0x03,
        0x01,
        0x02,
        0,
        9, USB_DT_HID,
        0x11, 0x01,
        0,
        1,
        USB_DT_REPORT,
        50, 0,
        7, 0x05,
        0x81,
        0x03,
        8, 0,
        10,
    };
    static const unsigned char hid_report_descriptor[] = {
        0x05, 0x01,
        0x09, 0x02,
        0xa1, 0x01,
        0x09, 0x01,
        0xa1, 0x00,
        0x05, 0x09,
        0x19, 0x01,
        0x29, 0x03,
        0x15, 0x00,
        0x25, 0x01,
        0x95, 0x03,
        0x75, 0x01,
        0x81, 0x02,
        0x95, 0x01,
        0x75, 0x05,
        0x81, 0x03,
        0x05, 0x01,
        0x09, 0x30,
        0x09, 0x31,
        0x15, 0x81,
        0x25, 0x7f,
        0x75, 0x08,
        0x95, 0x02,
        0x81, 0x06,
        0xc0,
        0xc0,
    };
    static const unsigned char device_descriptor[] = {
        18, USB_DT_DEVICE,
        0x00, 0x02,
        0x00, 0x00, 0x00,
        64,
        0x6b, 0x1d,
        0x01, 0x10,
        0x00, 0x01,
        1, 2, 3,
        1,
    };
    static const unsigned char config_descriptor[] = {
        9, USB_DT_CONFIG,
        33, 0,
        1,
        1,
        0,
        0x80,
        50,
        9, 0x04,
        0,
        0,
        2,
        0x08,
        0x06,
        0x50,
        0,
        7, 0x05,
        0x81,
        0x02,
        0x00, 0x02,
        0,
        7, 0x05,
        0x02,
        0x02,
        0x00, 0x02,
        0,
    };
    static const unsigned char qualifier_descriptor[] = {
        10, USB_DT_DEVICE_QUALIFIER,
        0x00, 0x02,
        0x00, 0x00, 0x00,
        64,
        1,
        0,
    };
    static const unsigned char langid_descriptor[] = {
        4, USB_DT_STRING,
        0x09, 0x04,
    };
    static const unsigned char manufacturer_descriptor[] = {
        12, USB_DT_STRING,
        'K', 0, 'o', 0, 'b', 0, 'o', 0, 'x', 0,
    };
    static const unsigned char product_descriptor[] = {
        24, USB_DT_STRING,
        'U', 0, 'S', 0, 'B', 0, ' ', 0, 'S', 0, 't', 0, 'o', 0, 'r', 0, 'a', 0, 'g', 0, 'e', 0,
    };
    static const unsigned char hid_product_descriptor[] = {
        30, USB_DT_STRING,
        'U', 0, 'S', 0, 'B', 0, ' ', 0, 'H', 0, 'I', 0, 'D', 0, ' ', 0, 'M', 0, 'o', 0, 'u', 0, 's', 0, 'e', 0,
    };
    static const unsigned char serial_descriptor[] = {
        10, USB_DT_STRING,
        '0', 0, '0', 0, '0', 0, '1', 0,
    };

    uint8_t descriptor_type = (uint8_t)(value >> 8);
    uint8_t descriptor_index = (uint8_t)(value & 0xffu);
    const unsigned char *descriptor = NULL;
    size_t descriptor_len = 0;
    const int hid_mouse = usb_synthetic_hid_mouse_enabled();
    const uint8_t recipient = requesttype & 0x1fu;
    const int standard_in = (requesttype & (USB_DIR_IN | 0x60)) == (USB_DIR_IN | USB_TYPE_STANDARD);
    if (!standard_in) {
        return -95;
    }
    switch (descriptor_type) {
    case USB_DT_DEVICE:
        if (recipient != USB_RECIP_DEVICE) {
            return -95;
        }
        descriptor = hid_mouse ? hid_device_descriptor : device_descriptor;
        descriptor_len = hid_mouse ? sizeof(hid_device_descriptor) : sizeof(device_descriptor);
        break;
    case USB_DT_CONFIG:
        if (recipient != USB_RECIP_DEVICE) {
            return -95;
        }
        descriptor = hid_mouse ? hid_config_descriptor : config_descriptor;
        descriptor_len = hid_mouse ? sizeof(hid_config_descriptor) : sizeof(config_descriptor);
        break;
    case USB_DT_DEVICE_QUALIFIER:
        if (recipient != USB_RECIP_DEVICE) {
            return -95;
        }
        descriptor = qualifier_descriptor;
        descriptor_len = sizeof(qualifier_descriptor);
        break;
    case USB_DT_HID:
        if (!hid_mouse || recipient != USB_RECIP_INTERFACE) {
            return -95;
        }
        descriptor = hid_config_descriptor + 18;
        descriptor_len = 9;
        break;
    case USB_DT_REPORT:
        if (!hid_mouse || recipient != USB_RECIP_INTERFACE) {
            return -95;
        }
        descriptor = hid_report_descriptor;
        descriptor_len = sizeof(hid_report_descriptor);
        break;
    case USB_DT_STRING:
        if (recipient != USB_RECIP_DEVICE) {
            return -95;
        }
        switch (descriptor_index) {
        case 0:
            descriptor = langid_descriptor;
            descriptor_len = sizeof(langid_descriptor);
            break;
        case 1:
            descriptor = manufacturer_descriptor;
            descriptor_len = sizeof(manufacturer_descriptor);
            break;
        case 2:
            descriptor = hid_mouse ? hid_product_descriptor : product_descriptor;
            descriptor_len = hid_mouse ? sizeof(hid_product_descriptor) : sizeof(product_descriptor);
            break;
        case 3:
            descriptor = serial_descriptor;
            descriptor_len = sizeof(serial_descriptor);
            break;
        default:
            return -32;
        }
        break;
    default:
        return -95;
    }
    (void)index;
    return (int)copy_descriptor(data, size, descriptor, descriptor_len);
}
