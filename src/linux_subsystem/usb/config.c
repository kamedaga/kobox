#include "config.h"

#include <stdlib.h>
#include <string.h>

static int usb_event_injection_runtime_allowed;

static int env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

int trace_usb_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_TRACE_USB");
    return cached;
}

int trace_usb_hub_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_TRACE_USB_HUB");
    return cached;
}

int trace_usb_control_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_TRACE_USB_CONTROL");
    return cached;
}

int trace_usb_direct_hub_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_TRACE_USB_DIRECT_HUB");
    return cached;
}

int trace_usb_descriptor_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_TRACE_USB_DESCRIPTOR");
    return cached;
}

int usb_hid_mouse_live_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_USB_HID_MOUSE_LIVE");
    return cached;
}

int usb_hid_mouse_xhci_only_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_USB_HID_MOUSE_XHCI_ONLY");
    if (value != NULL && value[0] != '\0') {
        cached = strcmp(value, "0") != 0;
        return cached;
    }
    cached = usb_hid_mouse_live_enabled() && usb_real_device_enabled() && usb_pachaos_backend_active();
    return cached;
}

int usb_real_device_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_USB_REAL_DEVICE");
    return cached;
}

int usb_real_root_hub_shim_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_USB_REAL_ROOT_HUB_SHIM");
    return cached;
}

int usb_pachaos_backend_active(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *backend = getenv("KOBOX_DEVICE_BACKEND");
    cached = backend != NULL && (strcmp(backend, "pachaos") == 0 || strcmp(backend, "pachaos_capsule") == 0);
    return cached;
}

int usb_pachaos_fake_root_hub_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = env_flag_enabled("KOBOX_USB_PACHAOS_FAKE_ROOT_HUB");
    return cached;
}

int usb_event_injection_enabled(void)
{
    static int cached_env = -1;
    if (cached_env < 0) {
        cached_env = env_flag_enabled("KOBOX_ENABLE_USB_EVENT_INJECT");
        if (!cached_env && usb_hid_mouse_live_enabled() &&
            usb_real_device_enabled() && usb_pachaos_backend_active())
        {
            cached_env = 1;
        }
    }
    return usb_event_injection_runtime_allowed && cached_env;
}

int usb_synthetic_hid_mouse_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *device = getenv("KOBOX_USB_SYNTHETIC_DEVICE");
    const char *smoke = getenv("KOBOX_USB_HID_MOUSE_SMOKE");
    const char *live = getenv("KOBOX_USB_HID_MOUSE_LIVE");
    const int explicit_hid_mouse =
        device != NULL && strstr(device, "hid") != NULL && strstr(device, "mouse") != NULL;
    if (usb_hid_mouse_xhci_only_enabled()) {
        cached = 0;
        return cached;
    }
    if (smoke != NULL && smoke[0] != '\0' && strcmp(smoke, "0") != 0) {
        cached = 1;
        return cached;
    }
    if (live != NULL && live[0] != '\0' && strcmp(live, "0") != 0) {
        cached = explicit_hid_mouse;
        return cached;
    }
    cached = explicit_hid_mouse;
    return cached;
}

int usb_synthetic_storage_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *device = getenv("KOBOX_USB_SYNTHETIC_DEVICE");
    const char *io_smoke = getenv("KOBOX_USB_STORAGE_IO_SMOKE");
    const char *summary = getenv("KOBOX_USB_STORAGE_SUMMARY");
    if (io_smoke != NULL && io_smoke[0] != '\0' && strcmp(io_smoke, "0") != 0) {
        cached = 1;
        return cached;
    }
    if (summary != NULL && summary[0] != '\0' && strcmp(summary, "0") != 0) {
        cached = 1;
        return cached;
    }
    cached = device != NULL &&
        (strstr(device, "storage") != NULL || strstr(device, "mass") != NULL || strstr(device, "scsi") != NULL);
    return cached;
}

void kb_usb_set_event_injection_runtime_allowed(int allowed)
{
    usb_event_injection_runtime_allowed = allowed != 0;
}
