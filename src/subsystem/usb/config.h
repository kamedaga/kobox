#ifndef KOBOX_SUBSYSTEM_USB_CONFIG_H
#define KOBOX_SUBSYSTEM_USB_CONFIG_H

int trace_usb_enabled(void);
int trace_usb_hub_enabled(void);
int trace_usb_control_enabled(void);
int trace_usb_direct_hub_enabled(void);
int trace_usb_descriptor_enabled(void);

int usb_event_injection_enabled(void);
int usb_synthetic_hid_mouse_enabled(void);
int usb_synthetic_storage_enabled(void);
int usb_hid_mouse_live_enabled(void);
int usb_hid_mouse_xhci_only_enabled(void);
int usb_real_device_enabled(void);
int usb_real_root_hub_shim_enabled(void);
int usb_pachaos_backend_active(void);
int usb_pachaos_fake_root_hub_enabled(void);

void kb_usb_set_event_injection_runtime_allowed(int allowed);

#endif
