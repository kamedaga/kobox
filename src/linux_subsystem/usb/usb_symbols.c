#include "linux_subsystem/usb/usb_symbols.h"
#include "kobox/shim.h"

#include <stdint.h>

static const kb_linux_symbol_t usb_symbols[] = {
    {"hub_port_debounce", (void *)(uintptr_t)&kb_usb_hub_port_debounce},
    {"renesas_xhci_check_request_fw", (void *)(uintptr_t)&kb_return_zero},
    {"usb_acpi_port_lpm_incapable", (void *)(uintptr_t)&kb_return_zero},
    {"usb_amd_quirk_pll_check", (void *)(uintptr_t)&kb_return_zero},
    {"usb_enable_intel_xhci_ports", (void *)(uintptr_t)&kb_return_zero},
    {"usb_acpi_power_manageable", (void *)(uintptr_t)&kb_return_zero},
    {"usb_acpi_set_power_state", (void *)(uintptr_t)&kb_return_zero},
    {"usb_amd_pt_check_port", (void *)(uintptr_t)&kb_return_zero},
    {"usb_add_hcd", (void *)(uintptr_t)&kb_usb_add_hcd},
    {"usb_create_shared_hcd", (void *)(uintptr_t)&kb_usb_create_shared_hcd},
    {"usb_decode_interval", (void *)(uintptr_t)&kb_usb_decode_interval},
    {"usb_deregister", (void *)(uintptr_t)&kb_usb_deregister},
    {"usb_deregister_dev", (void *)(uintptr_t)&kb_usb_deregister_dev},
    {"usb_disable_xhci_ports", (void *)(uintptr_t)&kb_return_zero},
    {"usb_disabled", (void *)(uintptr_t)&kb_return_zero},
    {"usb_ep_type_string", (void *)(uintptr_t)&kb_usb_ep_type_string},
    {"usb_find_common_endpoints", (void *)(uintptr_t)&kb_usb_find_common_endpoints},
    {"usb_find_interface", (void *)(uintptr_t)&kb_usb_find_interface},
    {"usb_hc_died", (void *)(uintptr_t)&kb_usb_hc_died},
    {"usb_hcd_amd_remote_wakeup_quirk", (void *)(uintptr_t)&kb_usb_hcd_amd_remote_wakeup_quirk},
    {"usb_hcd_check_unlink_urb", (void *)(uintptr_t)&kb_usb_hcd_check_unlink_urb},
    {"usb_hcd_end_port_resume", (void *)(uintptr_t)&kb_usb_hcd_end_port_resume},
    {"usb_hcd_giveback_urb", (void *)(uintptr_t)&kb_usb_hcd_giveback_urb},
    {"usb_hcd_link_urb_to_ep", (void *)(uintptr_t)&kb_usb_hcd_link_urb_to_ep},
    {"usb_hcd_map_urb_for_dma", (void *)(uintptr_t)&kb_usb_hcd_map_urb_for_dma},
    {"usb_hcd_pci_probe", (void *)(uintptr_t)&kb_usb_hcd_pci_probe},
    {"usb_hcd_pci_remove", (void *)(uintptr_t)&kb_usb_hcd_pci_remove},
    {"usb_hcd_pci_shutdown", (void *)(uintptr_t)&kb_usb_hcd_pci_shutdown},
    {"usb_hcd_poll_rh_status", (void *)(uintptr_t)&kb_usb_hcd_poll_rh_status},
    {"usb_hcd_resume_root_hub", (void *)(uintptr_t)&kb_usb_hcd_resume_root_hub},
    {"usb_hcd_start_port_resume", (void *)(uintptr_t)&kb_usb_hcd_start_port_resume},
    {"usb_hcd_unlink_urb_from_ep", (void *)(uintptr_t)&kb_usb_hcd_unlink_urb_from_ep},
    {"usb_hcd_unmap_urb_for_dma", (void *)(uintptr_t)&kb_usb_hcd_unmap_urb_for_dma},
    {"usb_hub_clear_tt_buffer", (void *)(uintptr_t)&kb_usb_hub_clear_tt_buffer},
    {"usb_kill_urb", (void *)(uintptr_t)&kb_usb_kill_urb},
    {"usb_put_hcd", (void *)(uintptr_t)&kb_usb_put_hcd},
    {"usb_register_driver", (void *)(uintptr_t)&kb_usb_register_driver},
    {"usb_remove_hcd", (void *)(uintptr_t)&kb_usb_remove_hcd},
    {"usb_root_hub_lost_power", (void *)(uintptr_t)&kb_usb_root_hub_lost_power},
    {"usb_speed_string", (void *)(uintptr_t)&kb_usb_speed_string},
    {"usb_state_string", (void *)(uintptr_t)&kb_usb_state_string},
    {"usb_submit_urb", (void *)(uintptr_t)&kb_usb_submit_urb},
    {"usb_unlink_urb", (void *)(uintptr_t)&kb_usb_unlink_urb},
    {"usb_wakeup_notification", (void *)(uintptr_t)&kb_usb_wakeup_notification},
    {"kobox_usb_control_msg_shim", (void *)(uintptr_t)&kb_usb_control_msg_shim},
};

const kb_linux_symbol_t *kb_linux_usb_symbols(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(usb_symbols) / sizeof(usb_symbols[0]);
    }
    return usb_symbols;
}
