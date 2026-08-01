#include "kobox/shim.h"
#include "linux_subsystem/dma/dma.h"
#include "linux_subsystem/input/input.h"
#include "linux_subsystem/usb/config.h"
#include "linux_subsystem/usb/port_state.h"
#include "linux_subsystem/usb/storage.h"
#include "linux_subsystem/usb/synthetic.h"
#include "linux_subsystem/usb/usb.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define fprintf(stream, ...) kb_tracef(__VA_ARGS__)

kb_device_backend_t *kb_shim_current_device_backend(void);

enum {
    KB_USB_HCD_STORAGE_SIZE = 4096,
    KB_LINUX_6_8_DEVICE_PARENT_OFFSET = 0x40,
    KB_LINUX_6_8_DEVICE_TYPE_OFFSET = 0x58,
    KB_LINUX_6_8_DEVICE_DRIVER_OFFSET = 0x68,
    KB_LINUX_6_6_PCI_DEV_DRIVER_DATA_OFFSET = 0x138,
    KB_LINUX_6_8_PCI_DEV_DRIVER_DATA_OFFSET = 0x140,
    KB_LINUX_6_8_USB_HCD_ROOT_HUB_OFFSET = 0x060,
    KB_LINUX_6_8_USB_HCD_SHARED_HCD_OFFSET = 0x210,
    KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET = 0x0d8,
    KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET = 0x120,
    KB_LINUX_6_8_USB_HCD_FLAGS_OFFSET = 0x138,
    KB_LINUX_6_8_USB_HCD_RH_STATE_OFFSET = 0x144,
    KB_LINUX_6_8_HC_DRIVER_IRQ_OFFSET = 0x18,
    KB_LINUX_6_8_HC_DRIVER_RESET_OFFSET = 0x28,
    KB_LINUX_6_8_HC_DRIVER_START_OFFSET = 0x30,
    KB_LINUX_6_8_HC_DRIVER_PCI_SUSPEND_OFFSET = 0x38,
    KB_LINUX_6_8_HC_DRIVER_PCI_RESUME_OFFSET = 0x40,
    KB_LINUX_6_8_HC_DRIVER_PCI_POWEROFF_LATE_OFFSET = 0x48,
    KB_LINUX_6_8_HC_DRIVER_STOP_OFFSET = 0x50,
    KB_LINUX_6_8_HC_DRIVER_SHUTDOWN_OFFSET = 0x58,
    KB_LINUX_6_8_HC_DRIVER_GET_FRAME_NUMBER_OFFSET = 0x60,
    KB_LINUX_6_8_HC_DRIVER_URB_ENQUEUE_OFFSET = 0x68,
    KB_LINUX_6_8_HC_DRIVER_URB_DEQUEUE_OFFSET = 0x70,
    KB_LINUX_6_8_HC_DRIVER_MAP_URB_FOR_DMA_OFFSET = 0x78,
    KB_LINUX_6_8_HC_DRIVER_UNMAP_URB_FOR_DMA_OFFSET = 0x80,
    KB_LINUX_6_8_HC_DRIVER_ENDPOINT_DISABLE_OFFSET = 0x88,
    KB_LINUX_6_8_HC_DRIVER_ENDPOINT_RESET_OFFSET = 0x90,
    KB_LINUX_6_8_HC_DRIVER_HUB_STATUS_DATA_OFFSET = 0x98,
    KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET = 0xa0,
    KB_LINUX_6_8_HC_DRIVER_BUS_SUSPEND_OFFSET = 0xa8,
    KB_LINUX_6_8_HC_DRIVER_BUS_RESUME_OFFSET = 0xb0,
    KB_LINUX_6_8_HC_DRIVER_START_PORT_RESET_OFFSET = 0xb8,
    KB_LINUX_6_8_HC_DRIVER_GET_RESUMING_PORTS_OFFSET = 0xc0,
    KB_LINUX_6_8_HC_DRIVER_RELINQUISH_PORT_OFFSET = 0xc8,
    KB_LINUX_6_8_HC_DRIVER_PORT_HANDED_OVER_OFFSET = 0xd0,
    KB_LINUX_6_8_HC_DRIVER_CLEAR_TT_BUFFER_COMPLETE_OFFSET = 0xd8,
    KB_LINUX_6_8_HC_DRIVER_ALLOC_DEV_OFFSET = 0xe0,
    KB_LINUX_6_8_HC_DRIVER_FREE_DEV_OFFSET = 0xe8,
    KB_LINUX_6_8_HC_DRIVER_ALLOC_STREAMS_OFFSET = 0xf0,
    KB_LINUX_6_8_HC_DRIVER_FREE_STREAMS_OFFSET = 0xf8,
    KB_LINUX_6_8_USB_DEVICE_ACTCONFIG_OFFSET = 0x3b0,
    KB_LINUX_6_8_USB_CONFIG_DESC_NUM_INTERFACES_OFFSET = 0x04,
    KB_LINUX_6_8_USB_CONFIG_DESC_CONFIGURATION_VALUE_OFFSET = 0x05,
    KB_LINUX_6_8_USB_HOST_CONFIG_INTERFACE0_OFFSET = 0x98,
    KB_LINUX_6_8_USB_INTERFACE_CUR_ALTSETTING_OFFSET = 0x08,
    KB_LINUX_6_8_USB_INTERFACE_DEV_OFFSET = 0x50,
    KB_LINUX_6_8_USB_INTERFACE_DRIVER_DATA_OFFSET = 0x0c8,
    KB_LINUX_6_8_USB_INTERFACE_DESC_NUMBER_OFFSET = 0x02,
    KB_LINUX_6_8_USB_INTERFACE_DESC_ALTSETTING_OFFSET = 0x03,
    KB_LINUX_6_8_USB_INTERFACE_DESC_ENDPOINT_COUNT_OFFSET = 0x04,
    KB_LINUX_6_8_USB_INTERFACE_DESC_CLASS_OFFSET = 0x05,
    KB_LINUX_6_8_USB_INTERFACE_DESC_SUBCLASS_OFFSET = 0x06,
    KB_LINUX_6_8_USB_INTERFACE_DESC_PROTOCOL_OFFSET = 0x07,
    KB_LINUX_6_8_USB_HOST_INTERFACE_ENDPOINT_OFFSET = 0x18,
    KB_LINUX_6_8_USB_ENDPOINT_DESC_ADDRESS_OFFSET = 0x02,
    KB_LINUX_6_8_USB_ENDPOINT_DESC_ATTRIBUTES_OFFSET = 0x03,
    KB_LINUX_6_8_USB_ENDPOINT_DESC_MAX_PACKET_OFFSET = 0x04,
    KB_LINUX_6_8_USB_ENDPOINT_DESC_INTERVAL_OFFSET = 0x06,
    KB_LINUX_6_8_USB_HUB_HDEV_OFFSET = 0x008,
    KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET = 0x058,
    KB_LINUX_6_8_USB_HUB_EVENTS_OFFSET = 0x218,
    KB_LINUX_6_8_USB_HUB_PORTS_OFFSET = 0x268,
    KB_LINUX_6_8_URB_HCPRIV_OFFSET = 0x008,
    KB_LINUX_6_8_URB_DEV_OFFSET = 0x040,
    KB_LINUX_6_8_URB_EP_OFFSET = 0x048,
    KB_LINUX_6_8_URB_PIPE_OFFSET = 0x050,
    KB_LINUX_6_8_URB_STATUS_OFFSET = 0x058,
    KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET = 0x05c,
    KB_LINUX_6_8_URB_TRANSFER_BUFFER_OFFSET = 0x060,
    KB_LINUX_6_8_URB_TRANSFER_DMA_OFFSET = 0x068,
    KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET = 0x080,
    KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET = 0x084,
    KB_LINUX_6_8_URB_SETUP_PACKET_OFFSET = 0x088,
    KB_LINUX_6_8_URB_SETUP_DMA_OFFSET = 0x090,
    KB_LINUX_6_8_URB_COMPLETE_OFFSET = 0x0b0,
    KB_USB_REQ_GET_PORT_STATUS = 0xa300,
    KB_USB_URB_NO_TRANSFER_DMA_MAP = 0x0004,
    KB_USB_URB_DIR_IN = 0x0200,
    KB_USB_URB_DMA_MAP_SINGLE = 0x00010000,
    KB_USB_URB_SETUP_MAP_SINGLE = 0x00100000,
    KB_USB_CONTROL_SETUP_SIZE = 8,
    KB_USB_CONFIG_INTERFACE_MAX = 32,
};

static unsigned char usb_hcd_pci_pm_ops[256];
static unsigned char usb_xhci_tracepoint[128];
static unsigned int usb_num_online_cpus = 1;
static unsigned char usb_pcpu_hot[256];
static int usb_pm_suspend_target_state;
static unsigned char usb_synthetic_storage_udev[64];
static unsigned char usb_synthetic_storage_interface[64];
static unsigned char usb_synthetic_storage_device[64];
static unsigned char usb_synthetic_storage_parent_device[64];
static unsigned int usb_hid_mouse_report_index;
static int usb_hid_mouse_completion_depth;
static unsigned char usb_synthetic_hid_mouse_input[KB_INPUT_LINUX_DEVICE_STORAGE_SIZE];
static int usb_synthetic_hid_mouse_registered;
static int usb_synthetic_hid_mouse_events_sent;
static int usb_root_hub_poll_paused_for_live;

static int usb_root_hub_status_complete(void *hcd);

static int usb_pointer_is_error_or_low(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

typedef int (*usb_hub_control_fn_t)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t);

typedef struct usb_hub_control_patch {
    void *driver;
    usb_hub_control_fn_t original;
    struct usb_hub_control_patch *next;
} usb_hub_control_patch_t;

typedef struct usb_complete_gs_stub {
    void *original;
    void *stub;
    unsigned long caller_gs;
    struct usb_complete_gs_stub *next;
} usb_complete_gs_stub_t;

static usb_hub_control_patch_t *usb_hub_control_patches;
static usb_complete_gs_stub_t *usb_complete_gs_stubs;
static void *usb_active_probe_dev;
static const void *usb_active_probe_driver;

static int usb_pachaos_port_matches_hcd(void *hcd, uint16_t port_status);
static uint16_t usb_pachaos_normalize_port_status_for_hcd(void *hcd, uint16_t port_status);
static unsigned int usb_pachaos_actual_port_for_virtual1(void *hcd, kb_usb_port_state_t *state);
static void usb_patch_hc_driver_callbacks_for_usbcore(const void *driver_arg);
static void usb_patch_hub_control_for_hcd(void *hcd);
static void usb_patch_urb_complete_for_usbcore(void *urb);

static const int usb_hid_mouse_input_reports[][3] = {
    { 1, 12, 4 },
    { 1, 8, -3 },
    { 0, -5, 6 },
    { 0, 3, -2 },
};

static int usb_enter_function_gs(const void *function, unsigned long *old_gs)
{
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(function);
    if (kernel_gs == 0) {
        kernel_gs = kb_shim_current_kernel_gs();
    }
    if (kernel_gs == 0) {
        return 0;
    }
    return kb_shim_enter_kernel_gs(kernel_gs, old_gs) == 0;
}

static void usb_call_void_ptr_with_fresh_gs(void (*function)(void *), void *arg)
{
    if (function == NULL) {
        return;
    }
    unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)function);
    if (kernel_gs == 0) {
        kernel_gs = kb_shim_current_kernel_gs();
    }
    unsigned long old_gs = 0;
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    kb_linux_call_void_ptr_gs(function, arg, kernel_gs);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
}

static usb_hub_control_patch_t *usb_hub_control_patch_for_driver(void *driver)
{
    for (usb_hub_control_patch_t *patch = usb_hub_control_patches; patch != NULL; patch = patch->next) {
        if (patch->driver == driver) {
            return patch;
        }
    }
    return NULL;
}

static void *usb_read_ptr_field(const void *base, size_t offset)
{
    void *value = NULL;
    if ((uintptr_t)base >= 4096u) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint8_t usb_read_u8_field(const void *base, size_t offset)
{
    uint8_t value = 0;
    if ((uintptr_t)base >= 4096u) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint16_t usb_read_le16_field(const void *base, size_t offset)
{
    const unsigned char *p = (const unsigned char *)base;
    if ((uintptr_t)p < 4096u) {
        return 0;
    }
    return (uint16_t)(p[offset] | ((uint16_t)p[offset + 1] << 8));
}

static uint32_t usb_read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    if ((uintptr_t)base >= 4096u) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint64_t usb_read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    if ((uintptr_t)base >= 4096u) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static void usb_write_u32_field(void *base, size_t offset, uint32_t value)
{
    if ((uintptr_t)base >= 4096u) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void usb_write_int_field(void *base, size_t offset, int value)
{
    if ((uintptr_t)base >= 4096u) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void usb_write_u64_field(void *base, size_t offset, uint64_t value)
{
    if ((uintptr_t)base >= 4096u) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void usb_write_ptr_field(void *base, size_t offset, void *value)
{
    if ((uintptr_t)base >= 4096u) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static kb_dma_dir_t usb_urb_dma_direction(uint32_t transfer_flags)
{
    return (transfer_flags & KB_USB_URB_DIR_IN) != 0 ? KB_DMA_FROM_DEVICE : KB_DMA_TO_DEVICE;
}

static int usb_status_from_kb_status(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return 0;
    case KB_ERR_INVALID:
        return -22;
    case KB_ERR_NOT_FOUND:
        return -19;
    case KB_ERR_DENIED:
        return -13;
    case KB_ERR_NOMEM:
        return -12;
    case KB_ERR_UNSUPPORTED:
        return -95;
    case KB_ERR_IO:
    default:
        return -5;
    }
}

static size_t usb_copy_descriptor(void *data, uint16_t size, const unsigned char *descriptor, size_t descriptor_len)
{
    if (usb_pointer_is_error_or_low(data) || size == 0 || descriptor == NULL || descriptor_len == 0) {
        return 0;
    }
    size_t copied = descriptor_len;
    if (copied > size) {
        copied = size;
    }
    memcpy(data, descriptor, copied);
    return copied;
}

static int usb_synthetic_hid_mouse_complete_urb(void *urb)
{
    if (!usb_event_injection_enabled() || !usb_synthetic_hid_mouse_enabled() || urb == NULL) {
        return 0;
    }
    if (usb_hid_mouse_completion_depth > 8) {
        return 0;
    }

    uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
    if ((transfer_flags & KB_USB_URB_DIR_IN) == 0) {
        return 0;
    }
    void *transfer_buffer = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_OFFSET);
    uint32_t transfer_len = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
    void (*complete)(void *) = (void (*)(void *))usb_read_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET);
    if (transfer_buffer == NULL || transfer_len < 3 || complete == NULL) {
        return 0;
    }
    if (!kb_module_is_executable_address((const void *)complete)) {
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: skip invalid synthetic completion urb=%p complete=%p\n", urb, (void *)complete);
        }
        return 0;
    }

    size_t report_count = sizeof(usb_hid_mouse_input_reports) / sizeof(usb_hid_mouse_input_reports[0]);
    if (usb_hid_mouse_report_index >= report_count && !usb_hid_mouse_live_enabled()) {
        return 0;
    }
    size_t report_index = usb_hid_mouse_report_index % report_count;

    unsigned char *buf = transfer_buffer;
    memset(buf, 0, transfer_len);
    buf[0] = (unsigned char)usb_hid_mouse_input_reports[report_index][0];
    buf[1] = (unsigned char)usb_hid_mouse_input_reports[report_index][1];
    buf[2] = (unsigned char)usb_hid_mouse_input_reports[report_index][2];
    usb_hid_mouse_report_index++;
    usb_write_int_field(urb, KB_LINUX_6_8_URB_STATUS_OFFSET, 0);
    usb_write_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET, 3);
    kb_usb_subsystem_urb_giveback(NULL, urb, 0);

    usb_hid_mouse_completion_depth++;
    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)complete);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    kb_linux_call_void_ptr_gs(complete, urb, kernel_gs);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    usb_hid_mouse_completion_depth--;
    return 1;
}

static int usb_hub_ready_for_events(void *hub)
{
    if (hub == NULL || usb_pointer_is_error_or_low(hub)) {
        return 0;
    }
    void *ports = usb_read_ptr_field(hub, KB_LINUX_6_8_USB_HUB_PORTS_OFFSET);
    if (usb_pointer_is_error_or_low(ports)) {
        return 0;
    }
    void *port0 = usb_read_ptr_field(ports, 0);
    return !usb_pointer_is_error_or_low(port0);
}

static void usb_hcd_release_record(kb_usb_hcd_record_t *record)
{
    if (record == NULL || !record->active) {
        return;
    }
    if (record->irq_registered) {
        kb_free_irq(record->irq, record->hcd);
    }
    if (record->regs != NULL && record->owner != NULL) {
        kb_pci_iounmap(record->owner, record->regs);
    }
    kb_usb_port_state_release(record->hcd);
    kb_usb_subsystem_hcd_release(record);
}

static void usb_store_pci_driver_data(void *pdev, void *data)
{
    if (pdev != NULL) {
        memcpy((unsigned char *)pdev + KB_LINUX_6_6_PCI_DEV_DRIVER_DATA_OFFSET, &data, sizeof(data));
        memcpy((unsigned char *)pdev + KB_LINUX_6_8_PCI_DEV_DRIVER_DATA_OFFSET, &data, sizeof(data));
    }
}

static void *usb_load_pci_driver_data(void *pdev)
{
    void *data = NULL;
    if (pdev != NULL) {
        memcpy(&data, (const unsigned char *)pdev + KB_LINUX_6_8_PCI_DEV_DRIVER_DATA_OFFSET, sizeof(data));
        if (data == NULL) {
            memcpy(&data, (const unsigned char *)pdev + KB_LINUX_6_6_PCI_DEV_DRIVER_DATA_OFFSET, sizeof(data));
        }
    }
    return data;
}

static void *usb_root_hub_for_hcd(void *hcd)
{
    void *root_hub = NULL;
    if (hcd != NULL) {
        memcpy(&root_hub, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_ROOT_HUB_OFFSET, sizeof(root_hub));
    }
    return root_hub;
}

static void usb_track_probe_hcd(void *hcd, int primary, void *primary_hcd)
{
    if (usb_pointer_is_error_or_low(hcd)) {
        return;
    }
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_track(hcd);
    if (record == NULL) {
        return;
    }
    record->primary = primary != 0;
    record->owner = usb_active_probe_dev;
    record->driver = primary ? usb_active_probe_driver : usb_read_ptr_field(hcd, KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET);
    record->primary_hcd = primary ? NULL : primary_hcd;
    usb_patch_hub_control_for_hcd(hcd);
}

typedef struct usb_root_hub_mark_ctx {
    void *root_hub;
    void (*mark)(void *hcd);
} usb_root_hub_mark_ctx_t;

static int usb_mark_hcd_for_root_hub_record(kb_usb_hcd_record_t *record, void *ctx)
{
    usb_root_hub_mark_ctx_t *mark_ctx = ctx;
    if (record == NULL || mark_ctx == NULL || mark_ctx->mark == NULL) {
        return 0;
    }
    if (usb_root_hub_for_hcd(record->hcd) == mark_ctx->root_hub) {
        mark_ctx->mark(record->hcd);
        return 1;
    }
    return 0;
}

static void usb_mark_hcd_for_root_hub(void *root_hub, void (*mark)(void *hcd))
{
    usb_root_hub_mark_ctx_t ctx = {
        .root_hub = root_hub,
        .mark = mark,
    };
    (void)kb_usb_subsystem_for_each_hcd(usb_mark_hcd_for_root_hub_record, &ctx);
}

typedef struct usb_root_hub_find_ctx {
    void *root_hub;
    void *hcd;
} usb_root_hub_find_ctx_t;

static int usb_find_hcd_for_root_hub_record(kb_usb_hcd_record_t *record, void *ctx)
{
    usb_root_hub_find_ctx_t *find_ctx = ctx;
    if (record == NULL || find_ctx == NULL || find_ctx->hcd != NULL) {
        return 0;
    }
    if (usb_root_hub_for_hcd(record->hcd) == find_ctx->root_hub) {
        find_ctx->hcd = record->hcd;
        return 1;
    }
    return 0;
}

static void *usb_hcd_for_root_hub(void *root_hub)
{
    usb_root_hub_find_ctx_t ctx = {
        .root_hub = root_hub,
        .hcd = NULL,
    };
    (void)kb_usb_subsystem_for_each_hcd(usb_find_hcd_for_root_hub_record, &ctx);
    if (ctx.hcd != NULL || usb_active_probe_dev == NULL) {
        return ctx.hcd;
    }

    void *primary_hcd = usb_load_pci_driver_data(usb_active_probe_dev);
    if (usb_pointer_is_error_or_low(primary_hcd)) {
        return NULL;
    }
    if (usb_root_hub_for_hcd(primary_hcd) == root_hub) {
        usb_track_probe_hcd(primary_hcd, 1, NULL);
        return primary_hcd;
    }

    void *shared_hcd = usb_read_ptr_field(primary_hcd, KB_LINUX_6_8_USB_HCD_SHARED_HCD_OFFSET);
    if (!usb_pointer_is_error_or_low(shared_hcd) && usb_root_hub_for_hcd(shared_hcd) == root_hub) {
        usb_track_probe_hcd(primary_hcd, 1, NULL);
        usb_track_probe_hcd(shared_hcd, 0, primary_hcd);
        return shared_hcd;
    }
    return NULL;
}

static void *usb_hub_for_root_hub(void *root_hub)
{
    void *(*hub_to_struct_hub)(void *) =
        (void *(*)(void *))kb_module_lookup_exported_symbol("usb_hub_to_struct_hub");
    if (hub_to_struct_hub != NULL && !usb_real_device_enabled()) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)hub_to_struct_hub, &old_gs);
        void *hub = kb_linux_call_ptr_ptr(hub_to_struct_hub, root_hub);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        return usb_pointer_is_error_or_low(hub) ? NULL : hub;
    }

    void *config = NULL;
    void *interface0 = NULL;
    void *hub = NULL;
    if (root_hub == NULL) {
        return NULL;
    }

    memcpy(&config, (const unsigned char *)root_hub + KB_LINUX_6_8_USB_DEVICE_ACTCONFIG_OFFSET, sizeof(config));
    if (config == NULL) {
        return NULL;
    }

    memcpy(
        &interface0,
        (const unsigned char *)config + KB_LINUX_6_8_USB_HOST_CONFIG_INTERFACE0_OFFSET,
        sizeof(interface0));
    if (interface0 == NULL) {
        return NULL;
    }

    memcpy(
        &hub,
        (const unsigned char *)interface0 + KB_LINUX_6_8_USB_INTERFACE_DRIVER_DATA_OFFSET,
        sizeof(hub));
    return hub;
}

static void usb_observe_endpoint(void *udev, void *interface, void *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    const uint8_t address = usb_read_u8_field(endpoint, KB_LINUX_6_8_USB_ENDPOINT_DESC_ADDRESS_OFFSET);
    const uint8_t attributes = usb_read_u8_field(endpoint, KB_LINUX_6_8_USB_ENDPOINT_DESC_ATTRIBUTES_OFFSET);
    kb_usb_endpoint_update_t update = {
        .udev = udev,
        .interface = interface,
        .endpoint = endpoint,
        .address = address,
        .attributes = attributes,
        .interval = usb_read_u8_field(endpoint, KB_LINUX_6_8_USB_ENDPOINT_DESC_INTERVAL_OFFSET),
        .type = (uint8_t)(attributes & 0x03u),
        .direction_in = (uint8_t)((address & 0x80u) != 0),
        .max_packet_size = usb_read_le16_field(endpoint, KB_LINUX_6_8_USB_ENDPOINT_DESC_MAX_PACKET_OFFSET),
    };
    (void)kb_usb_subsystem_endpoint_observe(&update);
}

typedef struct usb_altsetting_match {
    void *altsetting;
    kb_usb_interface_snapshot_t interface;
    int found;
} usb_altsetting_match_t;

static int usb_match_interface_altsetting(const kb_usb_interface_snapshot_t *snapshot, void *ctx)
{
    usb_altsetting_match_t *match = ctx;
    if (snapshot == NULL || match == NULL || snapshot->cur_altsetting != match->altsetting) {
        return 0;
    }
    match->interface = *snapshot;
    match->found = 1;
    return 1;
}

static void usb_observe_endpoint_for_altsetting(void *altsetting, void *endpoint)
{
    if (altsetting == NULL || endpoint == NULL) {
        return;
    }
    usb_altsetting_match_t match = {
        .altsetting = altsetting,
    };
    (void)kb_usb_subsystem_for_each_interface(usb_match_interface_altsetting, &match);
    if (match.found) {
        usb_observe_endpoint(match.interface.udev, match.interface.interface, endpoint);
    }
}

static void usb_observe_interface(void *udev, void *interface, void *driver_hint)
{
    if (interface == NULL) {
        return;
    }

    void *cur_altsetting =
        usb_read_ptr_field(interface, KB_LINUX_6_8_USB_INTERFACE_CUR_ALTSETTING_OFFSET);
    void *linux_device = (unsigned char *)interface + KB_LINUX_6_8_USB_INTERFACE_DEV_OFFSET;
    void *driver = driver_hint != NULL ? driver_hint : usb_read_ptr_field(linux_device, KB_LINUX_6_8_DEVICE_DRIVER_OFFSET);
    kb_usb_interface_update_t update = {
        .udev = udev,
        .interface = interface,
        .linux_device = linux_device,
        .parent_linux_device = usb_read_ptr_field(linux_device, KB_LINUX_6_8_DEVICE_PARENT_OFFSET),
        .driver = driver,
        .driver_data = usb_read_ptr_field(interface, KB_LINUX_6_8_USB_INTERFACE_DRIVER_DATA_OFFSET),
        .cur_altsetting = cur_altsetting,
    };

    if (cur_altsetting != NULL) {
        update.interface_number =
            usb_read_u8_field(cur_altsetting, KB_LINUX_6_8_USB_INTERFACE_DESC_NUMBER_OFFSET);
        update.alternate_setting =
            usb_read_u8_field(cur_altsetting, KB_LINUX_6_8_USB_INTERFACE_DESC_ALTSETTING_OFFSET);
        update.endpoint_count =
            usb_read_u8_field(cur_altsetting, KB_LINUX_6_8_USB_INTERFACE_DESC_ENDPOINT_COUNT_OFFSET);
        update.interface_class =
            usb_read_u8_field(cur_altsetting, KB_LINUX_6_8_USB_INTERFACE_DESC_CLASS_OFFSET);
        update.interface_subclass =
            usb_read_u8_field(cur_altsetting, KB_LINUX_6_8_USB_INTERFACE_DESC_SUBCLASS_OFFSET);
        update.interface_protocol =
            usb_read_u8_field(cur_altsetting, KB_LINUX_6_8_USB_INTERFACE_DESC_PROTOCOL_OFFSET);
    }

    (void)kb_usb_subsystem_interface_observe(&update);
    if (trace_usb_enabled()) {
        fprintf(stderr,
            "kobox usb: observe_interface interface=%p linux_dev=%p udev=%p alt=%p driver=%p driver_data=%p num=%u altsetting=%u eps=%u class=0x%02x subclass=0x%02x proto=0x%02x\n",
            interface,
            linux_device,
            udev,
            cur_altsetting,
            driver,
            update.driver_data,
            update.interface_number,
            update.alternate_setting,
            update.endpoint_count,
            update.interface_class,
            update.interface_subclass,
            update.interface_protocol);
    }

    void *endpoint = usb_read_ptr_field(cur_altsetting, KB_LINUX_6_8_USB_HOST_INTERFACE_ENDPOINT_OFFSET);
    if (endpoint != NULL && update.endpoint_count != 0) {
        usb_observe_endpoint(udev, interface, endpoint);
    }
}

static void usb_observe_device_graph_with_devnum(void *hcd, void *udev, uint32_t devnum)
{
    if (udev == NULL) {
        return;
    }

    void *config = usb_read_ptr_field(udev, KB_LINUX_6_8_USB_DEVICE_ACTCONFIG_OFFSET);
    uint8_t interface_count = 0;
    uint8_t configuration_value = 0;
    if (config != NULL) {
        interface_count = usb_read_u8_field(config, KB_LINUX_6_8_USB_CONFIG_DESC_NUM_INTERFACES_OFFSET);
        configuration_value =
            usb_read_u8_field(config, KB_LINUX_6_8_USB_CONFIG_DESC_CONFIGURATION_VALUE_OFFSET);
    }

    kb_usb_device_update_t update = {
        .hcd = hcd,
        .udev = udev,
        .active_config = config,
        .devnum = devnum,
        .configuration_value = configuration_value,
        .interface_count = interface_count,
    };
    (void)kb_usb_subsystem_device_observe(&update);

    size_t limit = interface_count;
    if (limit > KB_USB_CONFIG_INTERFACE_MAX) {
        limit = KB_USB_CONFIG_INTERFACE_MAX;
    }
    for (size_t i = 0; i < limit; i++) {
        void *interface = usb_read_ptr_field(
            (const unsigned char *)config + KB_LINUX_6_8_USB_HOST_CONFIG_INTERFACE0_OFFSET,
            i * sizeof(void *));
        usb_observe_interface(udev, interface, NULL);
    }
}

static void usb_observe_device_graph(void *hcd, void *udev)
{
    usb_observe_device_graph_with_devnum(hcd, udev, 0);
}

void kb_usb_observe_linux_device(void *dev)
{
    if (dev == NULL) {
        return;
    }

    void *device_type = usb_read_ptr_field(dev, KB_LINUX_6_8_DEVICE_TYPE_OFFSET);
    if ((uintptr_t)device_type < 4096u) {
        return;
    }

    void *usb_if_device_type = kb_module_lookup_exported_symbol("usb_if_device_type");
    if (usb_if_device_type == NULL) {
        return;
    }

    if (device_type == usb_if_device_type) {
        void *interface = (unsigned char *)dev - KB_LINUX_6_8_USB_INTERFACE_DEV_OFFSET;
        usb_observe_interface(NULL, interface, NULL);
    }
}

static int usb_root_hub_status_data(void *hcd, unsigned char status[8])
{
    void *driver = NULL;
    if (hcd == NULL) {
        return -22;
    }

    memcpy(&driver, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET, sizeof(driver));
    if (driver == NULL) {
        return -19;
    }

    int (*hub_status_data)(void *, char *) = NULL;
    memcpy(
        &hub_status_data,
        (const unsigned char *)driver + KB_LINUX_6_8_HC_DRIVER_HUB_STATUS_DATA_OFFSET,
        sizeof(hub_status_data));
    if (hub_status_data == NULL) {
        return -95;
    }
    unsigned long old_gs = 0;
    int has_gs = usb_enter_function_gs((const void *)hub_status_data, &old_gs);
    int result = kb_linux_call_int_ptr_ptr(hub_status_data, hcd, (char *)status);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static int usb_root_hub_get_port_status(void *hcd, unsigned int port, unsigned char port_data[4])
{
    void *driver = NULL;
    if (hcd == NULL || port == 0 || port >= (sizeof(unsigned long) * 8u) || port_data == NULL) {
        return -22;
    }

    memcpy(&driver, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET, sizeof(driver));
    if (driver == NULL) {
        return -19;
    }

    int (*hub_control)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t) = NULL;
    memcpy(
        &hub_control,
        (const unsigned char *)driver + KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET,
        sizeof(hub_control));
    if (hub_control == NULL) {
        return -95;
    }

    memset(port_data, 0, 4);
    unsigned long old_gs = 0;
    int has_gs = usb_enter_function_gs((const void *)hub_control, &old_gs);
    int result = kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
        hub_control,
        hcd,
        KB_USB_REQ_GET_PORT_STATUS,
        0,
        (uint16_t)port,
        (char *)port_data,
        4);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static int usb_real_root_hub_synthesize_connected_changes(void *hcd, unsigned char status[8], int status_len)
{
    enum {
        USB_PORT_STAT_CONNECTION = 0x0001,
        USB_PORT_STAT_C_CONNECTION = 0x0001,
    };

    if (hcd == NULL || status == NULL || status_len <= 0) {
        return status_len;
    }

    kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
    unsigned long prior_connected = state != NULL ? state->connected_bits : 0;
    unsigned long connected_now = 0;
    unsigned long changed_now = 0;
    unsigned int max_port = (unsigned int)(status_len * 8 - 1);
    if (max_port >= sizeof(unsigned long) * 8u) {
        max_port = (unsigned int)(sizeof(unsigned long) * 8u - 1u);
    }

    for (unsigned int port = 1; port <= max_port; port++) {
        unsigned char port_data[4] = { 0 };
        int result = usb_root_hub_get_port_status(hcd, port, port_data);
        if (result != 0) {
            continue;
        }

        uint16_t port_status = (uint16_t)port_data[0] | ((uint16_t)port_data[1] << 8);
        uint16_t port_change = (uint16_t)port_data[2] | ((uint16_t)port_data[3] << 8);
        unsigned long port_bit = 1ul << port;
        if ((port_status & USB_PORT_STAT_CONNECTION) != 0) {
            connected_now |= port_bit;
            if ((prior_connected & port_bit) == 0 || (port_change & USB_PORT_STAT_C_CONNECTION) != 0) {
                changed_now |= port_bit;
                status[port / 8u] |= (unsigned char)(1u << (port % 8u));
            }
        }
    }

    if (state != NULL) {
        state->connected_bits = connected_now;
        state->change_bits |= changed_now;
    }

    if (changed_now != 0 && status_len == 0) {
        status_len = 1;
    }
    return status_len;
}

static int usb_real_root_hub_status_data_from_xhci(void *hcd, unsigned char status[8])
{
    if (status == NULL || !usb_real_device_enabled()) {
        return -95;
    }

    memset(status, 0, 8);
    int real_len = usb_root_hub_status_data(hcd, status);
    if (!usb_pachaos_fake_root_hub_enabled()) {
        memset(status, 0, 8);
        kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
        unsigned long virtual_bit = 1ul << 1;
        unsigned long prior_connected = state != NULL ? state->connected_bits : 0;
        unsigned long pending_changes = state != NULL ?
            (state->change_bits | state->enable_change_bits | state->reset_change_bits) :
            0;
        unsigned int actual_port = usb_pachaos_actual_port_for_virtual1(hcd, state);
        uint16_t port_status = 0;
        uint16_t port_change = 0;
        int connected = kb_pci_xhci_port_status(actual_port, &port_status, &port_change) == 0 &&
            usb_pachaos_port_matches_hcd(hcd, port_status);
        uint16_t effective_change = port_change;
        if (state != NULL) {
            if ((state->cleared_backend_change_bits & virtual_bit) != 0) {
                effective_change &= (uint16_t)~0x0001u;
            }
            if ((state->cleared_backend_enable_change_bits & virtual_bit) != 0) {
                effective_change &= (uint16_t)~0x0002u;
            }
            if ((state->cleared_backend_reset_change_bits & virtual_bit) != 0) {
                effective_change &= (uint16_t)~0x0010u;
            }
        }
        if (trace_usb_direct_hub_enabled()) {
            fprintf(stderr,
                "kobox usb: pachaos status_data hcd=%p primary=%d actual_port=%u status=0x%04x change=0x%04x effective_change=0x%04x connected=%d pending=0x%lx\n",
                hcd,
                kb_usb_hcd_is_primary_hcd(hcd),
                actual_port,
                port_status,
                port_change,
                effective_change,
                connected,
                pending_changes);
        }
        if (connected) {
            if ((prior_connected & virtual_bit) == 0 || effective_change != 0) {
                pending_changes |= virtual_bit;
            }
            if (state != NULL) {
                state->connected_bits |= virtual_bit;
            }
        } else {
            if ((prior_connected & virtual_bit) != 0) {
                pending_changes |= virtual_bit;
            }
            if (state != NULL) {
                state->connected_bits &= ~virtual_bit;
                state->enabled_bits &= ~virtual_bit;
            }
        }
        if ((pending_changes & virtual_bit) != 0) {
            status[0] = (unsigned char)(1u << 1);
            if (state != NULL && connected) {
                state->change_bits |= virtual_bit;
            }
            return 1;
        }
        return real_len > 0 ? 0 : real_len;
    }
    if (real_len > 0) {
        return usb_real_root_hub_synthesize_connected_changes(hcd, status, real_len);
    }
    kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
    if (state == NULL || (state->change_bits & (1ul << 1)) == 0) {
        return 0;
    }
    status[0] = 1u << 1;
    return 1;
}

static void usb_port_state_clear_change_feature(
    kb_usb_port_state_t *state,
    unsigned long port_bit,
    uint16_t feature)
{
    enum {
        KB_USB_PORT_FEAT_C_CONNECTION = 16,
        KB_USB_PORT_FEAT_C_ENABLE = 17,
        KB_USB_PORT_FEAT_C_RESET = 20,
    };

    if (state == NULL) {
        return;
    }
    switch (feature) {
        case KB_USB_PORT_FEAT_C_CONNECTION:
            state->change_bits &= ~port_bit;
            state->cleared_backend_change_bits |= port_bit;
            break;
        case KB_USB_PORT_FEAT_C_ENABLE:
            state->enable_change_bits &= ~port_bit;
            state->cleared_backend_enable_change_bits |= port_bit;
            break;
        case KB_USB_PORT_FEAT_C_RESET:
            state->reset_change_bits &= ~port_bit;
            state->cleared_backend_reset_change_bits |= port_bit;
            break;
        default:
            break;
    }
}

static void usb_port_state_record_reset_done(
    kb_usb_port_state_t *state,
    unsigned long port_bit,
    uint16_t real_status,
    int assume_enabled)
{
    enum {
        KB_USB_PORT_STAT_CONNECTION = 0x0001,
        KB_USB_PORT_STAT_ENABLE = 0x0002,
    };

    if (state == NULL) {
        return;
    }
    if ((real_status & KB_USB_PORT_STAT_CONNECTION) != 0) {
        state->connected_bits |= port_bit;
    }
    if (assume_enabled || (real_status & KB_USB_PORT_STAT_ENABLE) != 0) {
        state->enabled_bits |= port_bit;
        state->enable_change_bits |= port_bit;
        state->cleared_backend_enable_change_bits &= ~port_bit;
    }
    state->reset_change_bits |= port_bit;
    state->cleared_backend_reset_change_bits &= ~port_bit;
}

static void usb_port_state_overlay_status(
    kb_usb_port_state_t *state,
    unsigned long port_bit,
    uint16_t *status,
    uint16_t *change)
{
    enum {
        KB_USB_PORT_STAT_CONNECTION = 0x0001,
        KB_USB_PORT_STAT_ENABLE = 0x0002,
        KB_USB_PORT_STAT_C_CONNECTION = 0x0001,
        KB_USB_PORT_STAT_C_ENABLE = 0x0002,
        KB_USB_PORT_STAT_C_RESET = 0x0010,
    };

    if (state == NULL || status == NULL || change == NULL) {
        return;
    }
    unsigned long was_connected = state->connected_bits & port_bit;
    if ((*status & KB_USB_PORT_STAT_CONNECTION) != 0) {
        if (was_connected == 0) {
            state->change_bits |= port_bit;
            state->cleared_backend_change_bits &= ~port_bit;
        }
        state->connected_bits |= port_bit;
    } else {
        if (was_connected != 0) {
            state->change_bits |= port_bit;
            state->cleared_backend_change_bits &= ~port_bit;
        }
        state->connected_bits &= ~port_bit;
        state->enabled_bits &= ~port_bit;
        state->cleared_backend_enable_change_bits &= ~port_bit;
        state->cleared_backend_reset_change_bits &= ~port_bit;
    }
    if ((*status & KB_USB_PORT_STAT_ENABLE) != 0) {
        state->enabled_bits |= port_bit;
    }
    if ((state->connected_bits & port_bit) != 0) {
        *status |= KB_USB_PORT_STAT_CONNECTION;
    }
    if ((state->enabled_bits & port_bit) != 0) {
        *status |= KB_USB_PORT_STAT_ENABLE;
    }
    if ((state->cleared_backend_change_bits & port_bit) != 0) {
        *change &= (uint16_t)~KB_USB_PORT_STAT_C_CONNECTION;
    }
    if ((state->cleared_backend_enable_change_bits & port_bit) != 0) {
        *change &= (uint16_t)~KB_USB_PORT_STAT_C_ENABLE;
    }
    if ((state->cleared_backend_reset_change_bits & port_bit) != 0) {
        *change &= (uint16_t)~KB_USB_PORT_STAT_C_RESET;
    }
    if ((state->change_bits & port_bit) != 0) {
        *change |= KB_USB_PORT_STAT_C_CONNECTION;
    }
    if ((state->enable_change_bits & port_bit) != 0) {
        *change |= KB_USB_PORT_STAT_C_ENABLE;
    }
    if ((state->reset_change_bits & port_bit) != 0) {
        *change |= KB_USB_PORT_STAT_C_RESET;
    }
}

static int usb_pachaos_port_matches_hcd(void *hcd, uint16_t port_status)
{
    enum {
        KB_USB_PORT_STAT_CONNECTION = 0x0001,
        KB_USB_PORT_STAT_SUPER_SPEED = 0x0800,
    };

    if ((port_status & KB_USB_PORT_STAT_CONNECTION) == 0) {
        return 0;
    }
    int primary = kb_usb_hcd_is_primary_hcd(hcd);
    int superspeed = (port_status & KB_USB_PORT_STAT_SUPER_SPEED) != 0;
    return primary ? !superspeed : superspeed;
}

static uint16_t usb_pachaos_normalize_port_status_for_hcd(void *hcd, uint16_t port_status)
{
    enum {
        KB_USB_PORT_STAT_CONNECTION = 0x0001,
        KB_USB_PORT_STAT_ENABLE = 0x0002,
        KB_USB_PORT_STAT_POWER = 0x0100,
        KB_USB_PORT_STAT_LOW_SPEED = 0x0200,
        KB_USB_PORT_STAT_HIGH_SPEED = 0x0400,
        KB_USB_PORT_STAT_SUPER_SPEED = 0x0800,
    };

    if (!usb_hid_mouse_live_enabled() ||
        !usb_real_device_enabled() ||
        !usb_pachaos_backend_active())
    {
        return port_status;
    }
    if (!kb_usb_hcd_is_primary_hcd(hcd)) {
        port_status = (uint16_t)(port_status & ~KB_USB_PORT_STAT_LOW_SPEED);
        port_status = (uint16_t)(port_status & ~KB_USB_PORT_STAT_HIGH_SPEED);
        if ((port_status & KB_USB_PORT_STAT_CONNECTION) != 0) {
            port_status = (uint16_t)(port_status | KB_USB_PORT_STAT_POWER);
        }
        return port_status;
    }
    port_status = (uint16_t)(port_status & ~KB_USB_PORT_STAT_SUPER_SPEED);
    if ((port_status & KB_USB_PORT_STAT_CONNECTION) != 0) {
        port_status = (uint16_t)(port_status | KB_USB_PORT_STAT_POWER);
    }
    return port_status;
}

static unsigned int usb_pachaos_actual_port_for_virtual1(void *hcd, kb_usb_port_state_t *state)
{
    if (state != NULL && state->actual_port_for_virtual1 != 0) {
        uint16_t status = 0;
        uint16_t change = 0;
        if (kb_pci_xhci_port_status(state->actual_port_for_virtual1, &status, &change) == 0 &&
            usb_pachaos_port_matches_hcd(hcd, status))
        {
            return state->actual_port_for_virtual1;
        }
    }

    unsigned int max_ports = kb_pci_xhci_max_ports();
    if (max_ports >= sizeof(unsigned long) * 8u) {
        max_ports = (unsigned int)(sizeof(unsigned long) * 8u - 1u);
    }
    for (unsigned int port = 1; port <= max_ports; port++) {
        uint16_t status = 0;
        uint16_t change = 0;
        if (kb_pci_xhci_port_status(port, &status, &change) == 0 &&
            usb_pachaos_port_matches_hcd(hcd, status))
        {
            if (state != NULL) {
                state->actual_port_for_virtual1 = port;
            }
            return port;
        }
    }

    if (state != NULL && state->actual_port_for_virtual1 != 0) {
        return state->actual_port_for_virtual1;
    }
    return 1;
}

static void usb_filter_empty_port_changes(void *hcd, unsigned char status[8], int status_len)
{
    void *driver = NULL;
    if (hcd == NULL || status == NULL || status_len <= 0) {
        return;
    }
    memcpy(&driver, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET, sizeof(driver));
    if (driver == NULL) {
        return;
    }

    int (*hub_control)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t) = NULL;
    memcpy(
        &hub_control,
        (const unsigned char *)driver + KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET,
        sizeof(hub_control));
    if (hub_control == NULL) {
        return;
    }

    kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
    unsigned long connected_now = 0;
    unsigned long prior_connected = state != NULL ? state->connected_bits : 0;

    for (int byte_index = 0; byte_index < status_len && byte_index < 8; byte_index++) {
        unsigned char bits = status[byte_index];
        for (int bit = 0; bit < 8; bit++) {
            unsigned char mask = (unsigned char)(1u << bit);
            if ((bits & mask) == 0) {
                continue;
            }
            int port = (byte_index * 8) + bit;
            if (port == 0) {
                continue;
            }

            unsigned char port_data[4] = { 0 };
            unsigned long old_gs = 0;
            int has_gs = usb_enter_function_gs((const void *)hub_control, &old_gs);
            int result = kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
                hub_control,
                hcd,
                KB_USB_REQ_GET_PORT_STATUS,
                0,
                (uint16_t)port,
                (char *)port_data,
                sizeof(port_data));
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            unsigned long port_bit = 1ul << (unsigned)port;
            int connected = result == 0 && (port_data[0] & 0x01u) != 0;
            if (connected) {
                connected_now |= port_bit;
            }
            if (!connected && (prior_connected & port_bit) == 0) {
                status[byte_index] &= (unsigned char)~mask;
            }
        }
    }

    if (state != NULL) {
        state->connected_bits = connected_now;
    }
}

static void trace_root_hub_ports(const char *label, void *hcd, const unsigned char status[8], int status_len)
{
    void *driver = NULL;
    if (hcd == NULL || status_len <= 0) {
        return;
    }
    memcpy(&driver, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET, sizeof(driver));
    if (driver == NULL) {
        return;
    }

    int (*hub_control)(void *, uint16_t, uint16_t, uint16_t, char *, uint16_t) = NULL;
    memcpy(
        &hub_control,
        (const unsigned char *)driver + KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET,
        sizeof(hub_control));
    if (hub_control == NULL) {
        return;
    }

    int byte_limit = status_len < 8 ? status_len : 8;
    for (int byte_index = 0; byte_index < byte_limit; byte_index++) {
        unsigned char bits = usb_real_device_enabled() ? (byte_index == 0 ? 0x02u : 0) : status[byte_index];
        for (int bit = 0; bit < 8; bit++) {
            if ((bits & (unsigned char)(1u << bit)) == 0) {
                continue;
            }
            int port = (byte_index * 8) + bit;
            if (port == 0) {
                continue;
            }
            unsigned char port_data[4] = { 0 };
            unsigned long old_gs = 0;
            int has_gs = usb_enter_function_gs((const void *)hub_control, &old_gs);
            int result = kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
                hub_control,
                hcd,
                KB_USB_REQ_GET_PORT_STATUS,
                0,
                (uint16_t)port,
                (char *)port_data,
                sizeof(port_data));
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
            fprintf(stderr,
                "kobox usb: %s port%d result=%d status=%02x %02x change=%02x %02x\n",
                label,
                port,
                result,
                port_data[0],
                port_data[1],
                port_data[2],
                port_data[3]);
        }
    }
}

static void trace_root_hub_state(const char *label, void *hcd)
{
    if (!trace_usb_enabled() || hcd == NULL) {
        return;
    }

    unsigned long flags = 0;
    void *status_urb = NULL;
    void *driver = NULL;
    void *root_hub = NULL;
    void *hub = NULL;
    unsigned char state = 0;
    memcpy(&flags, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_FLAGS_OFFSET, sizeof(flags));
    memcpy(&status_urb, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET, sizeof(status_urb));
    memcpy(&driver, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET, sizeof(driver));
    root_hub = usb_root_hub_for_hcd(hcd);
    hub = usb_hub_for_root_hub(root_hub);
    memcpy(&state, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_RH_STATE_OFFSET, sizeof(state));

    unsigned char status[8] = { 0 };
    int status_len = usb_real_device_enabled() ?
        usb_real_root_hub_status_data_from_xhci(hcd, status) :
        usb_root_hub_status_data(hcd, status);

    fprintf(stderr,
        "kobox usb: %s hcd=%p root_hub=%p hub=%p flags=0x%lx state=0x%02x status_urb=%p driver=%p status_len=%d status=%02x %02x %02x %02x\n",
        label,
        hcd,
        root_hub,
        hub,
        flags,
        (unsigned)state,
        status_urb,
        driver,
        status_len,
        status[0],
        status[1],
        status[2],
        status[3]);
    trace_root_hub_ports(label, hcd, status, status_len);
}

static int kick_root_hub_if_changed(void *hcd)
{
    void (*kick_hub_wq)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_kick_hub_wq");
    int inject_events = usb_event_injection_enabled() || usb_real_device_enabled();
    if (hcd == NULL) {
        return 0;
    }
    if (trace_usb_direct_hub_enabled()) {
        static int printed_runtime_addresses;
        if (!printed_runtime_addresses) {
            printed_runtime_addresses = 1;
            fprintf(stderr,
                "kobox usb: runtime addrs giveback=%p for_each_endpoint=%p hcd_giveback=%p kick_hub_wq=%p\n",
                (void *)&kb_usb_subsystem_urb_giveback,
                (void *)&kb_usb_subsystem_for_each_endpoint,
                (void *)&kb_usb_hcd_giveback_urb,
                (void *)kick_hub_wq);
        }
        fprintf(stderr, "kobox usb: kick_root_hub begin hcd=%p inject=%d\n", hcd, inject_events);
    }
    if (inject_events && kick_hub_wq == NULL && !usb_real_device_enabled()) {
        return 0;
    }

    unsigned char status[8] = { 0 };
    int status_len = usb_real_device_enabled() ?
        usb_real_root_hub_status_data_from_xhci(hcd, status) :
        usb_root_hub_status_data(hcd, status);
    if (trace_usb_direct_hub_enabled()) {
        fprintf(stderr,
            "kobox usb: kick_root_hub status hcd=%p len=%d data=%02x %02x %02x %02x\n",
            hcd,
            status_len,
            status[0],
            status[1],
            status[2],
            status[3]);
    }
    if (!usb_real_device_enabled()) {
        usb_filter_empty_port_changes(hcd, status, status_len);
    }
    if (status_len <= 0) {
        return 0;
    }

    if (usb_real_device_enabled() && !usb_pachaos_fake_root_hub_enabled() &&
        usb_root_hub_status_complete(hcd))
    {
        return 1;
    }

    void *root_hub = usb_root_hub_for_hcd(hcd);
    if (trace_usb_direct_hub_enabled()) {
        fprintf(stderr, "kobox usb: kick_root_hub root_hub hcd=%p root_hub=%p\n", hcd, root_hub);
    }
    if (root_hub == NULL) {
        return 0;
    }

    void *hub = usb_hub_for_root_hub(root_hub);
    if (trace_usb_direct_hub_enabled()) {
        fprintf(stderr, "kobox usb: kick_root_hub hub root_hub=%p hub=%p\n", root_hub, hub);
    }
    int hub_ready = usb_hub_ready_for_events(hub);
    kb_usb_hub_event_update_t event;
    if (kb_usb_subsystem_hub_event_prepare(hcd, status, (size_t)status_len, &event) != 0) {
        return 0;
    }

    int manual_kick = inject_events;
    int handled = inject_events && hub != NULL && hub_ready;
    if (inject_events && hub != NULL && hub_ready && event.bits != 0) {
        kb_usb_port_state_t *port_state = kb_usb_port_state_for_hcd(hcd);
        if (port_state != NULL) {
            port_state->connected_bits |= event.bits;
            port_state->change_bits |= event.new_bits;
        }
        unsigned long existing_bits = 0;
        memcpy(
            &existing_bits,
            (const unsigned char *)hub + KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET,
            sizeof(existing_bits));
        if (event.new_bits != 0) {
            existing_bits |= event.new_bits;
            memcpy(
                (unsigned char *)hub + KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET,
                &existing_bits,
                sizeof(existing_bits));
            (void)kb_usb_subsystem_hub_event_commit(hcd, event.new_bits, &event);
        }
    }

    const int trace_hub = trace_usb_hub_enabled() || trace_usb_enabled();
    const int trace_changed = event.bits != 0 || event.new_bits != 0 || event.injected_before != event.injected_after;
    if (trace_hub && trace_changed) {
        unsigned long hub_event_bits = 0;
        if (hub != NULL) {
            memcpy(
                &hub_event_bits,
                (const unsigned char *)hub + KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET,
                sizeof(hub_event_bits));
        }
        fprintf(stderr,
            "kobox usb: kick_root_hub hcd=%p root_hub=%p hub=%p slot=%ld injected=0x%lx->0x%lx hub_event_bits=0x%lx bits=0x%lx new_bits=0x%lx status_len=%d status=%02x %02x %02x %02x\n",
            hcd,
            root_hub,
            hub,
            event.slot,
            event.injected_before,
            event.injected_after,
            hub_event_bits,
            event.bits,
            event.new_bits,
            event.status_len,
            event.status[0],
            event.status[1],
            event.status[2],
            event.status[3]);
    }
    if (manual_kick &&
        hub != NULL &&
        hub_ready &&
        event.new_bits != 0)
    {
        if (kick_hub_wq != NULL) {
            if (trace_usb_direct_hub_enabled()) {
                fprintf(stderr,
                    "kobox usb: kick_hub_wq hcd=%p root_hub=%p hub=%p fn=%p\n",
                    hcd,
                    root_hub,
                    hub,
                    (void *)kick_hub_wq);
            }
            unsigned long old_gs = 0;
            int has_gs = usb_enter_function_gs((const void *)kick_hub_wq, &old_gs);
            kb_linux_call_void_ptr(kick_hub_wq, root_hub);
            if (has_gs) {
                kb_shim_leave_kernel_gs(old_gs);
            }
        } else if (trace_usb_direct_hub_enabled()) {
            fprintf(stderr, "kobox usb: kick_hub_wq unavailable hcd=%p root_hub=%p hub=%p\n", hcd, root_hub, hub);
        }
    }
    return handled;
}

static int usb_real_root_hub_control_msg(
    void *dev,
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
        USB_TYPE_CLASS = 0x20,
        USB_RECIP_DEVICE = 0x00,
        USB_RECIP_OTHER = 0x03,
        USB_REQ_GET_STATUS = 0x00,
        USB_REQ_CLEAR_FEATURE = 0x01,
        USB_REQ_SET_FEATURE = 0x03,
        USB_REQ_GET_DESCRIPTOR = 0x06,
        USB_REQ_GET_CONFIGURATION = 0x08,
        USB_REQ_SET_CONFIGURATION = 0x09,
        USB_DT_DEVICE = 0x01,
        USB_DT_CONFIG = 0x02,
        USB_DT_INTERFACE = 0x04,
        USB_DT_ENDPOINT = 0x05,
        USB_DT_HUB = 0x29,
        USB_DT_BOS = 0x0f,
        USB_DT_DEVICE_CAPABILITY = 0x10,
        USB_DT_SS_ENDPOINT_COMP = 0x30,
        USB_DT_SS_HUB = 0x2a,
        USB_CAP_TYPE_USB2_EXTENSION = 0x02,
        USB_CAP_TYPE_SS_USB = 0x03,
        USB_PORT_FEAT_RESET = 4,
        USB_PORT_STAT_CONNECTION = 0x0001,
        USB_PORT_STAT_ENABLE = 0x0002,
        USB_PORT_STAT_POWER = 0x0100,
    };

    if (!usb_real_device_enabled() ||
        (!usb_real_root_hub_shim_enabled() && !usb_pachaos_backend_active()) ||
        dev == NULL)
    {
        return -95;
    }
    void *hcd = usb_hcd_for_root_hub(dev);
    if (hcd == NULL) {
        return -95;
    }
    if (request == USB_REQ_GET_STATUS &&
        requesttype == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE) &&
        data != NULL &&
        size >= 4)
    {
        unsigned char *out = data;
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        return 4;
    }

    if (request == USB_REQ_GET_STATUS &&
        requesttype == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
        data != NULL &&
        size >= 2)
    {
        unsigned char *out = data;
        out[0] = 1;
        out[1] = 0;
        return 2;
    }

    if (request == USB_REQ_SET_CONFIGURATION &&
        requesttype == (USB_TYPE_STANDARD | USB_RECIP_DEVICE))
    {
        return 0;
    }

    if (request == USB_REQ_GET_CONFIGURATION &&
        requesttype == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
        data != NULL &&
        size >= 1)
    {
        unsigned char *out = data;
        out[0] = 1;
        return 1;
    }

    if (request == USB_REQ_GET_DESCRIPTOR &&
        requesttype == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE) &&
        data != NULL)
    {
        uint8_t descriptor_type = (uint8_t)(value >> 8);
        static const unsigned char hub_descriptor[] = {
            9, USB_DT_HUB,
            1,
            0x09, 0x00,
            10,
            0,
            0x00,
            0xff,
        };
        static const unsigned char ss_hub_descriptor[] = {
            12, USB_DT_SS_HUB,
            1,
            0x09, 0x00,
            10,
            0,
            0,
            0,
            0, 0,
            0,
        };
        if (descriptor_type == USB_DT_HUB) {
            size_t copied = usb_copy_descriptor(data, size, hub_descriptor, sizeof(hub_descriptor));
            if (trace_usb_descriptor_enabled()) {
                fprintf(stderr,
                    "kobox usb: rh-control class descriptor hcd=%p primary=%d type=0x%02x len=%u copied=%zu\n",
                    hcd,
                    kb_usb_hcd_is_primary_hcd(hcd),
                    descriptor_type,
                    (unsigned)size,
                    copied);
            }
            return (int)copied;
        }
        if (descriptor_type == USB_DT_SS_HUB) {
            if (kb_usb_hcd_is_primary_hcd(hcd)) {
                return -32;
            }
            size_t copied = usb_copy_descriptor(data, size, ss_hub_descriptor, sizeof(ss_hub_descriptor));
            if (trace_usb_descriptor_enabled()) {
                fprintf(stderr,
                    "kobox usb: rh-control class descriptor hcd=%p primary=%d type=0x%02x len=%u copied=%zu\n",
                    hcd,
                    kb_usb_hcd_is_primary_hcd(hcd),
                    descriptor_type,
                    (unsigned)size,
                    copied);
            }
            return (int)copied;
        }
    }

    if (request == USB_REQ_GET_DESCRIPTOR &&
        requesttype == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
        data != NULL)
    {
        uint8_t descriptor_type = (uint8_t)(value >> 8);
        static const unsigned char usb2_device_descriptor[] = {
            18, USB_DT_DEVICE,
            0x00, 0x02,
            0x09, 0x00, 0x01,
            64,
            0x6b, 0x1d,
            0x02, 0x00,
            0x08, 0x06,
            0, 0, 0,
            1,
        };
        static const unsigned char usb3_device_descriptor[] = {
            18, USB_DT_DEVICE,
            0x00, 0x03,
            0x09, 0x00, 0x03,
            64,
            0x6b, 0x1d,
            0x03, 0x00,
            0x08, 0x06,
            0, 0, 0,
            1,
        };
        static const unsigned char usb2_config_descriptor[] = {
            9, USB_DT_CONFIG,
            25, 0,
            1,
            1,
            0,
            0xe0,
            0,
            9, USB_DT_INTERFACE,
            0,
            0,
            1,
            0x09, 0x00, 0x00,
            0,
            7, USB_DT_ENDPOINT,
            0x81,
            0x03,
            2, 0,
            12,
        };
        static const unsigned char usb3_config_descriptor[] = {
            9, USB_DT_CONFIG,
            31, 0,
            1,
            1,
            0,
            0xe0,
            0,
            9, USB_DT_INTERFACE,
            0,
            0,
            1,
            0x09, 0x00, 0x00,
            0,
            7, USB_DT_ENDPOINT,
            0x81,
            0x03,
            2, 0,
            12,
            6, USB_DT_SS_ENDPOINT_COMP,
            0,
            0,
            2, 0,
        };
        static const unsigned char bos_descriptor[] = {
            5, USB_DT_BOS,
            22, 0,
            2,
            7, USB_DT_DEVICE_CAPABILITY, USB_CAP_TYPE_USB2_EXTENSION,
            0x02, 0x00, 0x00, 0x00,
            10, USB_DT_DEVICE_CAPABILITY, USB_CAP_TYPE_SS_USB,
            0x00,
            0x0e, 0x00,
            1,
            10,
            0x20, 0x00,
        };
        static const unsigned char usb2_bos_descriptor[] = {
            5, USB_DT_BOS,
            12, 0,
            1,
            7, USB_DT_DEVICE_CAPABILITY, USB_CAP_TYPE_USB2_EXTENSION,
            0x02, 0x00, 0x00, 0x00,
        };
        if (descriptor_type == USB_DT_DEVICE) {
            const unsigned char *descriptor =
                kb_usb_hcd_is_primary_hcd(hcd) ? usb2_device_descriptor : usb3_device_descriptor;
            size_t descriptor_len =
                kb_usb_hcd_is_primary_hcd(hcd) ? sizeof(usb2_device_descriptor) : sizeof(usb3_device_descriptor);
            size_t copied = usb_copy_descriptor(data, size, descriptor, descriptor_len);
            if (trace_usb_descriptor_enabled()) {
                fprintf(stderr,
                    "kobox usb: rh-control standard descriptor hcd=%p primary=%d type=0x%02x len=%u desc_len=%zu copied=%zu\n",
                    hcd,
                    kb_usb_hcd_is_primary_hcd(hcd),
                    descriptor_type,
                    (unsigned)size,
                    descriptor_len,
                    copied);
            }
            return (int)copied;
        }
        if (descriptor_type == USB_DT_CONFIG) {
            const unsigned char *descriptor =
                kb_usb_hcd_is_primary_hcd(hcd) ? usb2_config_descriptor : usb3_config_descriptor;
            size_t descriptor_len =
                kb_usb_hcd_is_primary_hcd(hcd) ? sizeof(usb2_config_descriptor) : sizeof(usb3_config_descriptor);
            size_t copied = usb_copy_descriptor(data, size, descriptor, descriptor_len);
            if (trace_usb_descriptor_enabled()) {
                fprintf(stderr,
                    "kobox usb: rh-control standard descriptor hcd=%p primary=%d type=0x%02x len=%u desc_len=%zu copied=%zu\n",
                    hcd,
                    kb_usb_hcd_is_primary_hcd(hcd),
                    descriptor_type,
                    (unsigned)size,
                    descriptor_len,
                    copied);
            }
            return (int)copied;
        }
        if (descriptor_type == USB_DT_BOS) {
            if (kb_usb_hcd_is_primary_hcd(hcd)) {
                size_t copied = usb_copy_descriptor(data, size, usb2_bos_descriptor, sizeof(usb2_bos_descriptor));
                if (trace_usb_descriptor_enabled()) {
                    fprintf(stderr,
                        "kobox usb: rh-control standard descriptor hcd=%p primary=%d type=0x%02x len=%u desc_len=%zu copied=%zu\n",
                        hcd,
                        1,
                        descriptor_type,
                        (unsigned)size,
                        sizeof(usb2_bos_descriptor),
                        copied);
                }
                return (int)copied;
            }
            size_t copied = usb_copy_descriptor(data, size, bos_descriptor, sizeof(bos_descriptor));
            if (trace_usb_descriptor_enabled()) {
                fprintf(stderr,
                    "kobox usb: rh-control standard descriptor hcd=%p primary=0 type=0x%02x len=%u desc_len=%zu copied=%zu\n",
                    hcd,
                    descriptor_type,
                    (unsigned)size,
                    sizeof(bos_descriptor),
                    copied);
            }
            return (int)copied;
        }
    }

    uint16_t port = index;
    if (port == 0 || port >= (sizeof(unsigned long) * 8u)) {
        return -95;
    }
    unsigned long port_bit = 1ul << port;
    kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);

    if (request == USB_REQ_GET_STATUS &&
        requesttype == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER) &&
        data != NULL &&
        size >= 4)
    {
        uint16_t port_status = USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE | USB_PORT_STAT_POWER;
        uint16_t port_change = 0;
        if (!usb_pachaos_fake_root_hub_enabled()) {
            uint16_t real_status = 0;
            uint16_t real_change = 0;
            unsigned int actual_port = port == 1 ?
                usb_pachaos_actual_port_for_virtual1(hcd, state) :
                port;
            if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) == 0 &&
                (port != 1 || usb_pachaos_port_matches_hcd(hcd, real_status)))
            {
                port_status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
                port_change = real_change;
            } else {
                port_status = USB_PORT_STAT_POWER;
                port_change = 0;
            }
        }
        usb_port_state_overlay_status(state, port_bit, &port_status, &port_change);
        unsigned char *out = data;
        out[0] = (unsigned char)(port_status & 0xffu);
        out[1] = (unsigned char)(port_status >> 8);
        out[2] = (unsigned char)(port_change & 0xffu);
        out[3] = (unsigned char)(port_change >> 8);
        return 4;
    }

    if (request == USB_REQ_CLEAR_FEATURE &&
        requesttype == (USB_TYPE_CLASS | USB_RECIP_OTHER))
    {
        usb_port_state_clear_change_feature(state, port_bit, value);
        return 0;
    }

    if (request == USB_REQ_SET_FEATURE &&
        requesttype == (USB_TYPE_CLASS | USB_RECIP_OTHER))
    {
        if (state != NULL && value == USB_PORT_FEAT_RESET) {
            uint16_t real_status = USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE;
            if (!usb_pachaos_fake_root_hub_enabled()) {
                uint16_t real_change = 0;
                unsigned int actual_port = port == 1 ?
                    usb_pachaos_actual_port_for_virtual1(hcd, state) :
                    port;
                (void)kb_pci_xhci_reset_port(actual_port);
                if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) != 0 ||
                    (port == 1 && !usb_pachaos_port_matches_hcd(hcd, real_status)))
                {
                    real_status = USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE;
                }
                real_status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
            }
            usb_port_state_record_reset_done(state, port_bit, real_status, 1);
        }
        return 0;
    }

    return -95;
}

static int usb_pachaos_root_hub_hub_control(
    void *hcd,
    uint16_t type_req,
    uint16_t value,
    uint16_t index,
    char *data,
    uint16_t length)
{
    enum {
        ClearPortFeature = 0x2301,
        ClearHubFeature = 0x2001,
        GetConfiguration = 0x8008,
        GetDescriptor = 0x8006,
        GetStatus = 0x8000,
        GetHubDescriptor = 0xa006,
        GetHubStatus = 0xa000,
        GetPortStatus = 0xa300,
        SetHubFeature = 0x2003,
        SetConfiguration = 0x0009,
        SetPortFeature = 0x2303,
        USB_PORT_FEAT_RESET = 4,
        USB_PORT_STAT_CONNECTION = 0x0001,
        USB_PORT_STAT_ENABLE = 0x0002,
        USB_PORT_STAT_POWER = 0x0100,
        USB_DT_DEVICE = 0x01,
        USB_DT_CONFIG = 0x02,
        USB_DT_INTERFACE = 0x04,
        USB_DT_ENDPOINT = 0x05,
        USB_DT_BOS = 0x0f,
        USB_DT_DEVICE_CAPABILITY = 0x10,
        USB_DT_SS_ENDPOINT_COMP = 0x30,
        USB_CAP_TYPE_USB2_EXTENSION = 0x02,
        USB_CAP_TYPE_SS_USB = 0x03,
    };

    if (!usb_real_device_enabled() || hcd == NULL)
    {
        return -95;
    }
    if (type_req == GetHubStatus && data != NULL && length >= 4) {
        memset(data, 0, 4);
        return 0;
    }

    if (type_req == GetStatus && data != NULL && length >= 2) {
        data[0] = 1;
        data[1] = 0;
        return 2;
    }

    if (type_req == SetConfiguration) {
        return 0;
    }

    if (type_req == GetConfiguration && data != NULL && length >= 1) {
        data[0] = 1;
        return 1;
    }

    if (type_req == GetHubDescriptor && data != NULL && length != 0) {
        static const unsigned char hub_descriptor[] = {
            9, 0x29,
            1,
            0x09, 0x00,
            10,
            0,
            0x00,
            0xff,
        };
        static const unsigned char ss_hub_descriptor[] = {
            12, 0x2a,
            1,
            0x09, 0x00,
            10,
            0,
            0,
            0,
            0, 0,
            0,
        };
        uint8_t descriptor_type = (uint8_t)(value >> 8);
        const unsigned char *descriptor = NULL;
        size_t descriptor_len = 0;
        if (descriptor_type == 0x2a) {
            descriptor = ss_hub_descriptor;
            descriptor_len = sizeof(ss_hub_descriptor);
        } else if (descriptor_type == 0x29) {
            descriptor = hub_descriptor;
            descriptor_len = sizeof(hub_descriptor);
        } else {
            return -32;
        }
        size_t copied = usb_copy_descriptor(data, length, descriptor, descriptor_len);
        if (trace_usb_descriptor_enabled()) {
            fprintf(stderr,
                "kobox usb: hub-control class descriptor hcd=%p primary=%d type=0x%02x len=%u desc_len=%zu copied=%zu\n",
                hcd,
                kb_usb_hcd_is_primary_hcd(hcd),
                descriptor_type,
                (unsigned)length,
                descriptor_len,
                copied);
        }
        return (int)copied;
    }

    if (type_req == GetDescriptor && data != NULL && length != 0) {
        static const unsigned char usb2_device_descriptor[] = {
            18, USB_DT_DEVICE,
            0x00, 0x02,
            0x09, 0x00, 0x01,
            64,
            0x6b, 0x1d,
            0x02, 0x00,
            0x08, 0x06,
            0, 0, 0,
            1,
        };
        static const unsigned char usb3_device_descriptor[] = {
            18, USB_DT_DEVICE,
            0x00, 0x03,
            0x09, 0x00, 0x03,
            64,
            0x6b, 0x1d,
            0x03, 0x00,
            0x08, 0x06,
            0, 0, 0,
            1,
        };
        static const unsigned char usb2_config_descriptor[] = {
            9, USB_DT_CONFIG,
            25, 0,
            1,
            1,
            0,
            0xe0,
            0,
            9, USB_DT_INTERFACE,
            0,
            0,
            1,
            0x09, 0x00, 0x00,
            0,
            7, USB_DT_ENDPOINT,
            0x81,
            0x03,
            2, 0,
            12,
        };
        static const unsigned char usb3_config_descriptor[] = {
            9, USB_DT_CONFIG,
            31, 0,
            1,
            1,
            0,
            0xe0,
            0,
            9, USB_DT_INTERFACE,
            0,
            0,
            1,
            0x09, 0x00, 0x00,
            0,
            7, USB_DT_ENDPOINT,
            0x81,
            0x03,
            2, 0,
            12,
            6, USB_DT_SS_ENDPOINT_COMP,
            0,
            0,
            2, 0,
        };
        static const unsigned char bos_descriptor[] = {
            5, USB_DT_BOS,
            22, 0,
            2,
            7, USB_DT_DEVICE_CAPABILITY, USB_CAP_TYPE_USB2_EXTENSION,
            0x02, 0x00, 0x00, 0x00,
            10, USB_DT_DEVICE_CAPABILITY, USB_CAP_TYPE_SS_USB,
            0x00,
            0x0e, 0x00,
            1,
            10,
            0x20, 0x00,
        };
        static const unsigned char usb2_bos_descriptor[] = {
            5, USB_DT_BOS,
            12, 0,
            1,
            7, USB_DT_DEVICE_CAPABILITY, USB_CAP_TYPE_USB2_EXTENSION,
            0x02, 0x00, 0x00, 0x00,
        };
        uint8_t descriptor_type = (uint8_t)(value >> 8);
        const unsigned char *descriptor = NULL;
        size_t descriptor_len = 0;
        if (descriptor_type == USB_DT_DEVICE) {
            if (kb_usb_hcd_is_primary_hcd(hcd)) {
                descriptor = usb2_device_descriptor;
                descriptor_len = sizeof(usb2_device_descriptor);
            } else {
                descriptor = usb3_device_descriptor;
                descriptor_len = sizeof(usb3_device_descriptor);
            }
        } else if (descriptor_type == USB_DT_CONFIG) {
            if (kb_usb_hcd_is_primary_hcd(hcd)) {
                descriptor = usb2_config_descriptor;
                descriptor_len = sizeof(usb2_config_descriptor);
            } else {
                descriptor = usb3_config_descriptor;
                descriptor_len = sizeof(usb3_config_descriptor);
            }
        } else if (descriptor_type == USB_DT_BOS) {
            if (kb_usb_hcd_is_primary_hcd(hcd)) {
                descriptor = usb2_bos_descriptor;
                descriptor_len = sizeof(usb2_bos_descriptor);
            } else {
                descriptor = bos_descriptor;
                descriptor_len = sizeof(bos_descriptor);
            }
        }
        if (descriptor != NULL) {
            size_t copied = usb_copy_descriptor(data, length, descriptor, descriptor_len);
            if (trace_usb_descriptor_enabled()) {
                fprintf(stderr,
                    "kobox usb: hub-control standard descriptor hcd=%p primary=%d type=0x%02x len=%u desc_len=%zu copied=%zu\n",
                    hcd,
                    kb_usb_hcd_is_primary_hcd(hcd),
                    descriptor_type,
                    (unsigned)length,
                    descriptor_len,
                    copied);
            }
            return (int)copied;
        }
    }

    if (type_req == ClearHubFeature || type_req == SetHubFeature) {
        return 0;
    }

    uint16_t port = index;
    if (port == 0 || port >= (sizeof(unsigned long) * 8u)) {
        return -95;
    }
    unsigned long port_bit = 1ul << port;
    kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);

    if (type_req == GetPortStatus && data != NULL && length >= 4) {
        uint16_t port_status = USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE | USB_PORT_STAT_POWER;
        uint16_t port_change = 0;
        if (!usb_pachaos_fake_root_hub_enabled()) {
            uint16_t real_status = 0;
            uint16_t real_change = 0;
            unsigned int actual_port = port == 1 ?
                usb_pachaos_actual_port_for_virtual1(hcd, state) :
                port;
            if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) == 0 &&
                (port != 1 || usb_pachaos_port_matches_hcd(hcd, real_status)))
            {
                port_status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
                port_change = real_change;
            } else {
                port_status = USB_PORT_STAT_POWER;
                port_change = 0;
            }
        }
        usb_port_state_overlay_status(state, port_bit, &port_status, &port_change);
        data[0] = (char)(port_status & 0xffu);
        data[1] = (char)(port_status >> 8);
        data[2] = (char)(port_change & 0xffu);
        data[3] = (char)(port_change >> 8);
        return 0;
    }

    if (type_req == ClearPortFeature) {
        usb_port_state_clear_change_feature(state, port_bit, value);
        return 0;
    }

    if (type_req == SetPortFeature) {
        if (state != NULL && value == USB_PORT_FEAT_RESET) {
            uint16_t real_status = USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE;
            if (!usb_pachaos_fake_root_hub_enabled()) {
                uint16_t real_change = 0;
                unsigned int actual_port = port == 1 ?
                    usb_pachaos_actual_port_for_virtual1(hcd, state) :
                    port;
                (void)kb_pci_xhci_reset_port(actual_port);
                if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) != 0 ||
                    (port == 1 && !usb_pachaos_port_matches_hcd(hcd, real_status)))
                {
                    real_status = USB_PORT_STAT_CONNECTION | USB_PORT_STAT_ENABLE;
                }
                real_status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
            }
            usb_port_state_record_reset_done(state, port_bit, real_status, 1);
        }
        return 0;
    }

    return -95;
}

static int kb_usb_hub_control_wrapper(
    void *hcd,
    uint16_t type_req,
    uint16_t value,
    uint16_t index,
    char *data,
    uint16_t length)
{
    enum {
        ClearPortFeature = 0x2301,
        GetHubStatus = 0xa000,
        GetPortStatus = 0xa300,
        SetPortFeature = 0x2303,
        USB_PORT_FEAT_RESET = 4,
    };

    const int full_xhci_root_hub =
        usb_real_device_enabled() && !usb_pachaos_fake_root_hub_enabled();
    const int original_can_handle_request =
        type_req == GetPortStatus ||
        type_req == ClearPortFeature ||
        type_req == SetPortFeature;
    const int prefer_original =
        full_xhci_root_hub && original_can_handle_request && !usb_pachaos_backend_active();

    int shim_result = -95;
    if (!prefer_original) {
        shim_result = usb_pachaos_root_hub_hub_control(hcd, type_req, value, index, data, length);
    }
    if (shim_result != -95) {
        if (trace_usb_hub_enabled()) {
            fprintf(stderr,
                "kobox usb: hub_control shim hcd=%p type=0x%04x value=0x%04x index=%u len=%u result=%d\n",
                hcd,
                (unsigned)type_req,
                (unsigned)value,
                (unsigned)index,
                (unsigned)length,
                shim_result);
        }
        return shim_result;
    }
    if (usb_real_device_enabled() && !prefer_original) {
        if (hcd == NULL) {
            return -95;
        }
        if (trace_usb_hub_enabled()) {
            fprintf(stderr,
                "kobox usb: hub_control shim unsupported hcd=%p type=0x%04x value=0x%04x index=%u len=%u\n",
                hcd,
                (unsigned)type_req,
                (unsigned)value,
                (unsigned)index,
                (unsigned)length);
        }
        return -95;
    }

    void *driver = hcd != NULL ? usb_read_ptr_field(hcd, KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET) : NULL;
    usb_hub_control_patch_t *patch = usb_hub_control_patch_for_driver(driver);
    if (patch == NULL || patch->original == NULL) {
        if (trace_usb_hub_enabled()) {
            fprintf(stderr,
                "kobox usb: hub_control fallback missing hcd=%p driver=%p type=0x%04x value=0x%04x index=%u len=%u\n",
                hcd,
                driver,
                (unsigned)type_req,
                (unsigned)value,
                (unsigned)index,
                (unsigned)length);
        }
        return -95;
    }
    int trace_hub_control = trace_usb_hub_enabled();
    if (trace_hub_control) {
        fprintf(stderr,
            "kobox usb: hub_control fallback hcd=%p driver=%p original=%p type=0x%04x value=0x%04x index=%u len=%u\n",
            hcd,
            driver,
            (void *)patch->original,
            (unsigned)type_req,
            (unsigned)value,
            (unsigned)index,
            (unsigned)length);
    }
    unsigned long old_gs = 0;
    int has_gs = usb_enter_function_gs((const void *)patch->original, &old_gs);
    int result = kb_linux_call_int_ptr_u16_u16_u16_ptr_u16(
        patch->original,
        hcd,
        type_req,
        value,
        index,
        data,
        length);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (prefer_original && result != 0) {
        shim_result = usb_pachaos_root_hub_hub_control(hcd, type_req, value, index, data, length);
        if (shim_result != -95) {
            result = shim_result;
        }
    }
    if (usb_real_device_enabled() && result == 0 &&
        type_req == KB_USB_REQ_GET_PORT_STATUS &&
        data != NULL && length >= 4 && index > 0 && index < sizeof(unsigned long) * 8u)
    {
        kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
        uint16_t port_status = (uint16_t)(unsigned char)data[0] | ((uint16_t)(unsigned char)data[1] << 8);
        uint16_t port_change = (uint16_t)(unsigned char)data[2] | ((uint16_t)(unsigned char)data[3] << 8);
        if (full_xhci_root_hub) {
            uint16_t real_status = 0;
            uint16_t real_change = 0;
            unsigned int actual_port = index == 1 ?
                usb_pachaos_actual_port_for_virtual1(hcd, state) :
                index;
            if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) == 0 &&
                (index != 1 || usb_pachaos_port_matches_hcd(hcd, real_status)))
            {
                port_status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
                port_change = real_change;
            } else {
                port_status = 0x0100u;
                port_change = 0;
            }
        }
        usb_port_state_overlay_status(state, 1ul << (unsigned)index, &port_status, &port_change);
        data[0] = (char)(port_status & 0xffu);
        data[1] = (char)(port_status >> 8);
        data[2] = (char)(port_change & 0xffu);
        data[3] = (char)(port_change >> 8);
    }
    if (usb_real_device_enabled() &&
        type_req == SetPortFeature && value == USB_PORT_FEAT_RESET &&
        result == 0 && index > 0 && index < sizeof(unsigned long) * 8u)
    {
        kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
        uint16_t real_status = 0;
        uint16_t real_change = 0;
        unsigned int actual_port = index == 1 ?
            usb_pachaos_actual_port_for_virtual1(hcd, state) :
            index;
        if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) != 0 ||
            (index == 1 && full_xhci_root_hub && !usb_pachaos_port_matches_hcd(hcd, real_status)))
        {
            real_status = 0x0001u | 0x0002u;
        }
        if (full_xhci_root_hub) {
            real_status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
        }
        usb_port_state_record_reset_done(state, 1ul << (unsigned)index, real_status, 1);
    }
    if (trace_hub_control) {
        unsigned char b0 = data != NULL && length > 0 ? (unsigned char)data[0] : 0;
        unsigned char b1 = data != NULL && length > 1 ? (unsigned char)data[1] : 0;
        unsigned char b2 = data != NULL && length > 2 ? (unsigned char)data[2] : 0;
        unsigned char b3 = data != NULL && length > 3 ? (unsigned char)data[3] : 0;
        fprintf(stderr,
            "kobox usb: hub_control fallback result hcd=%p type=0x%04x value=0x%04x index=%u result=%d data=%02x %02x %02x %02x\n",
            hcd,
            (unsigned)type_req,
            (unsigned)value,
            (unsigned)index,
            result,
            b0,
            b1,
            b2,
            b3);
    }
    if (usb_real_device_enabled() && result == 0 && type_req == ClearPortFeature) {
        kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
        if (state != NULL && index > 0 && index < sizeof(unsigned long) * 8u) {
            usb_port_state_clear_change_feature(state, 1ul << (unsigned)index, value);
        }
    }
    return result;
}

static int usb_root_hub_control_msg_via_hub_control(
    void *dev,
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size)
{
    enum {
        GetHubStatus = 0xa000,
        GetPortStatus = 0xa300,
    };

    void *hcd = usb_hcd_for_root_hub(dev);
    if (hcd == NULL) {
        return -95;
    }

    uint16_t type_req = (uint16_t)(((uint16_t)requesttype << 8) | (uint16_t)request);
    int result = kb_usb_hub_control_wrapper(hcd, type_req, value, index, (char *)data, size);
    if (result == 0) {
        if (type_req == GetHubStatus || type_req == GetPortStatus) {
            return size < 4u ? (int)size : 4;
        }
    }
    return result;
}

int kb_usb_hub_port_debounce(void *hub, int port1, int must_be_connected)
{
    enum {
        USB_DIR_IN = 0x80,
        USB_TYPE_CLASS = 0x20,
        USB_RECIP_OTHER = 0x03,
        USB_REQ_GET_STATUS = 0x00,
        USB_REQ_CLEAR_FEATURE = 0x01,
        USB_PORT_FEAT_C_CONNECTION = 16,
        USB_PORT_STAT_CONNECTION = 0x0001,
        USB_PORT_STAT_C_CONNECTION = 0x0001,
    };

    if (hub == NULL || port1 <= 0) {
        return -22;
    }

    void *hdev = usb_read_ptr_field(hub, KB_LINUX_6_8_USB_HUB_HDEV_OFFSET);
    if (hdev == NULL) {
        return -19;
    }

    if (usb_real_device_enabled() && !usb_pachaos_fake_root_hub_enabled()) {
        void *hcd = usb_hcd_for_root_hub(hdev);
        kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
        unsigned int actual_port = port1 == 1 ?
            usb_pachaos_actual_port_for_virtual1(hcd, state) :
            (unsigned int)port1;
        uint16_t real_status = 0;
        uint16_t real_change = 0;
        if (kb_pci_xhci_port_status(actual_port, &real_status, &real_change) != 0 ||
            (port1 == 1 && !usb_pachaos_port_matches_hcd(hcd, real_status)))
        {
            return must_be_connected ? -19 : 0;
        }

        uint16_t status = usb_pachaos_normalize_port_status_for_hcd(hcd, real_status);
        if (must_be_connected && (status & USB_PORT_STAT_CONNECTION) == 0) {
            return -19;
        }
        if (port1 > 0 && port1 < (int)(sizeof(unsigned long) * 8u)) {
            unsigned long port_bit = 1ul << (unsigned)port1;
            uint16_t change = real_change;
            usb_port_state_overlay_status(state, port_bit, &status, &change);
            (void)change;
        }
        return (int)status;
    }

    uint16_t last_status = 0xffffu;
    int stable_reads = 0;
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        unsigned char data[4] = { 0 };
        int result = usb_root_hub_control_msg_via_hub_control(
            hdev,
            USB_REQ_GET_STATUS,
            USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
            0,
            (uint16_t)port1,
            data,
            sizeof(data));
        if (result < 0) {
            return result;
        }
        uint16_t status = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        uint16_t change = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
        if ((change & USB_PORT_STAT_C_CONNECTION) != 0) {
            (void)usb_root_hub_control_msg_via_hub_control(
                hdev,
                USB_REQ_CLEAR_FEATURE,
                USB_TYPE_CLASS | USB_RECIP_OTHER,
                USB_PORT_FEAT_C_CONNECTION,
                (uint16_t)port1,
                NULL,
                0);
            last_status = 0xffffu;
            stable_reads = 0;
            continue;
        }

        if (must_be_connected && (status & USB_PORT_STAT_CONNECTION) == 0) {
            return -19;
        }
        if (status == last_status) {
            stable_reads++;
        } else {
            last_status = status;
            stable_reads = 1;
        }
        if (stable_reads >= 2) {
            return (int)status;
        }
        kb_msleep(25);
    }

    return last_status != 0xffffu ? (int)last_status : -19;
}

static void usb_patch_hub_control_for_driver(const void *driver_arg)
{
    (void)trace_usb_hub_enabled();
    (void)trace_usb_enabled();
    if (driver_arg == NULL || !usb_real_device_enabled()) {
        return;
    }

    void *driver = (void *)driver_arg;
    if (usb_hub_control_patch_for_driver(driver) != NULL) {
        return;
    }

    usb_hub_control_fn_t original = NULL;
    memcpy(
        &original,
        (const unsigned char *)driver + KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET,
        sizeof(original));
    if (original == NULL || original == kb_usb_hub_control_wrapper) {
        return;
    }

    usb_hub_control_patch_t *patch = kb_kzalloc(sizeof(*patch), 0);
    if (patch == NULL) {
        return;
    }
    patch->driver = driver;
    patch->original = original;
    patch->next = usb_hub_control_patches;
    usb_hub_control_patches = patch;

    usb_hub_control_fn_t wrapper = kb_usb_hub_control_wrapper;
    memcpy(
        (unsigned char *)driver + KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET,
        &wrapper,
        sizeof(wrapper));
    if (trace_usb_enabled()) {
        fprintf(stderr,
            "kobox usb: patched hub_control driver=%p original=%p wrapper=%p\n",
            driver,
            (void *)original,
            (void *)wrapper);
    }
}

static unsigned long usbcore_callback_caller_gs(void)
{
    void *usb_add_hcd = kb_module_lookup_exported_symbol("usb_add_hcd");
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(usb_add_hcd);
    if (kernel_gs == 0) {
        kernel_gs = kb_shim_current_kernel_gs();
    }
    return kernel_gs;
}

static int usb_complete_pointer_is_stub(void *complete)
{
    for (usb_complete_gs_stub_t *entry = usb_complete_gs_stubs; entry != NULL; entry = entry->next) {
        if (entry->stub == complete) {
            return 1;
        }
    }
    return 0;
}

static void *usb_complete_stub_for(void *original, unsigned long caller_gs)
{
    for (usb_complete_gs_stub_t *entry = usb_complete_gs_stubs; entry != NULL; entry = entry->next) {
        if (entry->original == original && entry->caller_gs == caller_gs) {
            return entry->stub;
        }
    }

    void *stub = kb_module_make_gs_call_stub(original, caller_gs);
    if (stub == NULL) {
        return NULL;
    }
    usb_complete_gs_stub_t *entry = kb_kzalloc(sizeof(*entry), 0);
    if (entry == NULL) {
        return NULL;
    }
    entry->original = original;
    entry->stub = stub;
    entry->caller_gs = caller_gs;
    entry->next = usb_complete_gs_stubs;
    usb_complete_gs_stubs = entry;
    return stub;
}

static void usb_patch_urb_complete_for_usbcore(void *urb)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return;
    }
    void *complete = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET);
    if (complete == NULL || usb_complete_pointer_is_stub(complete)) {
        return;
    }
    if (!kb_module_is_executable_address(complete)) {
        return;
    }

    unsigned long caller_gs = usbcore_callback_caller_gs();
    unsigned long callee_gs = kb_module_kernel_gs_for_address(complete);
    if (caller_gs == 0 || callee_gs == 0 || caller_gs == callee_gs) {
        return;
    }

    void *stub = usb_complete_stub_for(complete, caller_gs);
    if (stub == NULL) {
        if (trace_usb_enabled()) {
            fprintf(stderr,
                "kobox usb: urb complete stub failed urb=%p complete=%p caller_gs=0x%lx callee_gs=0x%lx\n",
                urb,
                complete,
                caller_gs,
                callee_gs);
        }
        return;
    }
    usb_write_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET, stub);
    if (trace_usb_enabled()) {
        fprintf(stderr,
            "kobox usb: patched urb complete urb=%p original=%p stub=%p caller_gs=0x%lx callee_gs=0x%lx\n",
            urb,
            complete,
            stub,
            caller_gs,
            callee_gs);
    }
}

static void usb_patch_one_hc_driver_callback(void *driver, size_t offset, const char *name, unsigned long caller_gs)
{
    void *original = NULL;
    memcpy(&original, (const unsigned char *)driver + offset, sizeof(original));
    if (original == NULL || caller_gs == 0) {
        return;
    }
    if (!kb_module_is_executable_address(original)) {
        return;
    }
    unsigned long callee_gs = kb_module_kernel_gs_for_address(original);
    if (callee_gs == 0 || callee_gs == caller_gs) {
        return;
    }
    void *stub = kb_module_make_gs_call_stub(original, caller_gs);
    if (stub == NULL) {
        if (trace_usb_enabled()) {
            fprintf(stderr,
                "kobox usb: hc_driver callback stub failed driver=%p offset=0x%zx name=%s original=%p\n",
                driver,
                offset,
                name != NULL ? name : "?",
                original);
        }
        return;
    }
    memcpy((unsigned char *)driver + offset, &stub, sizeof(stub));
    if (trace_usb_enabled()) {
        fprintf(stderr,
            "kobox usb: patched hc_driver callback driver=%p offset=0x%zx name=%s original=%p stub=%p caller_gs=0x%lx callee_gs=0x%lx\n",
            driver,
            offset,
            name != NULL ? name : "?",
            original,
            stub,
            caller_gs,
            callee_gs);
    }
}

static void usb_patch_hc_driver_callbacks_for_usbcore(const void *driver_arg)
{
    static const struct {
        size_t offset;
        const char *name;
    } callbacks[] = {
        { KB_LINUX_6_8_HC_DRIVER_IRQ_OFFSET, "irq" },
        { KB_LINUX_6_8_HC_DRIVER_RESET_OFFSET, "reset" },
        { KB_LINUX_6_8_HC_DRIVER_START_OFFSET, "start" },
        { KB_LINUX_6_8_HC_DRIVER_PCI_SUSPEND_OFFSET, "pci_suspend" },
        { KB_LINUX_6_8_HC_DRIVER_PCI_RESUME_OFFSET, "pci_resume" },
        { KB_LINUX_6_8_HC_DRIVER_PCI_POWEROFF_LATE_OFFSET, "pci_poweroff_late" },
        { KB_LINUX_6_8_HC_DRIVER_STOP_OFFSET, "stop" },
        { KB_LINUX_6_8_HC_DRIVER_SHUTDOWN_OFFSET, "shutdown" },
        { KB_LINUX_6_8_HC_DRIVER_GET_FRAME_NUMBER_OFFSET, "get_frame_number" },
        { KB_LINUX_6_8_HC_DRIVER_URB_ENQUEUE_OFFSET, "urb_enqueue" },
        { KB_LINUX_6_8_HC_DRIVER_URB_DEQUEUE_OFFSET, "urb_dequeue" },
        { KB_LINUX_6_8_HC_DRIVER_MAP_URB_FOR_DMA_OFFSET, "map_urb_for_dma" },
        { KB_LINUX_6_8_HC_DRIVER_UNMAP_URB_FOR_DMA_OFFSET, "unmap_urb_for_dma" },
        { KB_LINUX_6_8_HC_DRIVER_ENDPOINT_DISABLE_OFFSET, "endpoint_disable" },
        { KB_LINUX_6_8_HC_DRIVER_ENDPOINT_RESET_OFFSET, "endpoint_reset" },
        { KB_LINUX_6_8_HC_DRIVER_HUB_STATUS_DATA_OFFSET, "hub_status_data" },
        { KB_LINUX_6_8_HC_DRIVER_BUS_SUSPEND_OFFSET, "bus_suspend" },
        { KB_LINUX_6_8_HC_DRIVER_BUS_RESUME_OFFSET, "bus_resume" },
        { KB_LINUX_6_8_HC_DRIVER_START_PORT_RESET_OFFSET, "start_port_reset" },
        { KB_LINUX_6_8_HC_DRIVER_GET_RESUMING_PORTS_OFFSET, "get_resuming_ports" },
        { KB_LINUX_6_8_HC_DRIVER_RELINQUISH_PORT_OFFSET, "relinquish_port" },
        { KB_LINUX_6_8_HC_DRIVER_PORT_HANDED_OVER_OFFSET, "port_handed_over" },
        { KB_LINUX_6_8_HC_DRIVER_CLEAR_TT_BUFFER_COMPLETE_OFFSET, "clear_tt_buffer_complete" },
        { KB_LINUX_6_8_HC_DRIVER_ALLOC_DEV_OFFSET, "alloc_dev" },
        { KB_LINUX_6_8_HC_DRIVER_FREE_DEV_OFFSET, "free_dev" },
        { KB_LINUX_6_8_HC_DRIVER_ALLOC_STREAMS_OFFSET, "alloc_streams" },
        { KB_LINUX_6_8_HC_DRIVER_FREE_STREAMS_OFFSET, "free_streams" },
    };

    (void)trace_usb_enabled();
    if (driver_arg == NULL || !usb_real_device_enabled()) {
        return;
    }

    unsigned long caller_gs = usbcore_callback_caller_gs();
    if (caller_gs == 0) {
        return;
    }

    void *driver = (void *)driver_arg;
    for (size_t i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); i++) {
        usb_patch_one_hc_driver_callback(driver, callbacks[i].offset, callbacks[i].name, caller_gs);
    }
}

static void usb_patch_hub_control_for_hcd(void *hcd)
{
    (void)trace_usb_hub_enabled();
    (void)trace_usb_enabled();
    if (hcd == NULL || !usb_real_device_enabled()) {
        return;
    }

    kb_usb_port_state_t *state = kb_usb_port_state_for_hcd(hcd);
    if (state != NULL && usb_pachaos_fake_root_hub_enabled()) {
        unsigned long first_port_bit = 1ul << 1;
        state->connected_bits |= first_port_bit;
        state->change_bits |= first_port_bit;
    }

    void *driver = usb_read_ptr_field(hcd, KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET);
    usb_patch_hc_driver_callbacks_for_usbcore(driver);
    usb_patch_hub_control_for_driver(driver);
}

int kb_usb_hcd_irq(int irq, void *hcd)
{
    (void)irq;
    kb_usb_subsystem_hcd_note_irq(hcd);
    return 1;
}

static void usb_track_real_shared_hcd(void *primary_hcd, kb_usb_hcd_record_t *primary_record)
{
    if (primary_hcd == NULL) {
        return;
    }
    if (primary_record == NULL || !primary_record->primary) {
        return;
    }
    void *shared_hcd = usb_read_ptr_field(primary_hcd, KB_LINUX_6_8_USB_HCD_SHARED_HCD_OFFSET);
    if (shared_hcd == NULL || shared_hcd == primary_hcd) {
        return;
    }
    kb_usb_hcd_record_t *shared_record = kb_usb_subsystem_hcd_track(shared_hcd);
    if (shared_record == NULL) {
        return;
    }
    shared_record->primary = 0;
    shared_record->primary_hcd = primary_hcd;
    if (primary_record != NULL) {
        shared_record->owner = primary_record->owner;
        shared_record->driver = usb_read_ptr_field(shared_hcd, KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET);
        if (shared_record->driver == NULL) {
            shared_record->driver = primary_record->driver;
        }
        shared_record->irq = primary_record->irq;
        shared_record->added = primary_record->added;
    }
    usb_patch_hub_control_for_hcd(shared_hcd);
    if (trace_usb_enabled()) {
        fprintf(stderr,
            "kobox usb: tracked real shared hcd=%p primary=%p owner=%p irq=%u\n",
            shared_hcd,
            primary_hcd,
            shared_record->owner,
            shared_record->irq);
    }
}

int kb_usb_hcd_is_primary_hcd(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    return record == NULL || record->primary;
}

void kb_usb_reset_root_hub_poll_live_state(void)
{
    usb_root_hub_poll_paused_for_live = 0;
}

void kb_usb_pause_root_hub_poll_for_live(void)
{
    if (usb_hid_mouse_live_enabled()) {
        usb_root_hub_poll_paused_for_live = 1;
    }
}

int kb_usb_root_hub_poll_needed(void)
{
    if (usb_hid_mouse_live_enabled() &&
        (usb_root_hub_poll_paused_for_live || kb_input_subsystem_device_count() != 0))
    {
        return 0;
    }
    return 1;
}

int kb_usb_poll_root_hub(void *hcd)
{
    if (!kb_usb_root_hub_poll_needed()) {
        return 0;
    }
    void (*poll_rh_status)(void *) =
        (void (*)(void *))kb_module_lookup_exported_symbol("usb_hcd_poll_rh_status");
    void (*resume_root_hub)(void *) =
        (void (*)(void *))kb_module_lookup_exported_symbol("usb_hcd_resume_root_hub");
    if (poll_rh_status == NULL) {
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: poll_root_hubs missing usb_hcd_poll_rh_status\n");
        }
        return 0;
    }
    if (hcd == NULL) {
        return 0;
    }
    kb_usb_subsystem_hcd_note_root_hub_poll(hcd);
    trace_root_hub_state("poll_root_hub", hcd);
    usb_observe_device_graph(hcd, usb_root_hub_for_hcd(hcd));
    if (resume_root_hub != NULL) {
        kb_usb_subsystem_hcd_note_root_hub_resume(hcd);
        usb_call_void_ptr_with_fresh_gs(resume_root_hub, hcd);
    }
    int kicked = kick_root_hub_if_changed(hcd);
    if (!kicked) {
        usb_call_void_ptr_with_fresh_gs(poll_rh_status, hcd);
    }
    void *shared_hcd = NULL;
    memcpy(&shared_hcd, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_SHARED_HCD_OFFSET, sizeof(shared_hcd));
    if (shared_hcd != NULL && shared_hcd != hcd) {
        kb_usb_hcd_record_t *primary_record = kb_usb_subsystem_hcd_for_hcd(hcd);
        usb_track_real_shared_hcd(hcd, primary_record);
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: poll_shared_root_hub hcd=%p shared=%p\n", hcd, shared_hcd);
        }
        kb_usb_subsystem_hcd_note_root_hub_poll(shared_hcd);
        trace_root_hub_state("poll_shared_root_hub", shared_hcd);
        usb_observe_device_graph(shared_hcd, usb_root_hub_for_hcd(shared_hcd));
        if (resume_root_hub != NULL) {
            kb_usb_subsystem_hcd_note_root_hub_resume(shared_hcd);
            usb_call_void_ptr_with_fresh_gs(resume_root_hub, shared_hcd);
        }
        kicked = kick_root_hub_if_changed(shared_hcd);
        if (!kicked) {
            usb_call_void_ptr_with_fresh_gs(poll_rh_status, shared_hcd);
        }
        return 2;
    }
    return 1;
}

static int usb_poll_root_hub_record(kb_usb_hcd_record_t *record, void *ctx)
{
    int *polled = ctx;
    if (record == NULL || record->hcd == NULL || polled == NULL) {
        return 0;
    }
    if (!record->primary) {
        return 0;
    }
    if (trace_usb_enabled()) {
        fprintf(stderr, "kobox usb: poll_root_hub hcd=%p primary=%d\n", record->hcd, record->primary);
    }
    *polled += kb_usb_poll_root_hub(record->hcd);
    return 0;
}

int kb_usb_poll_root_hubs(void)
{
    int polled = 0;
    (void)kb_usb_subsystem_for_each_hcd(usb_poll_root_hub_record, &polled);
    (void)kb_usb_synthesize_connected_storage();
    (void)kb_usb_synthesize_connected_hid_mouse();
    return polled;
}

int kb_usb_synthesize_connected_storage(void)
{
    if (!usb_event_injection_enabled() || usb_synthetic_hid_mouse_enabled() || !usb_synthetic_storage_enabled()) {
        return 0;
    }
    return kb_usb_storage_subsystem_synthesize_mass_storage(
        usb_synthetic_storage_udev,
        usb_synthetic_storage_interface,
        usb_synthetic_storage_device,
        usb_synthetic_storage_parent_device);
}

int kb_usb_synthesize_connected_hid_mouse(void)
{
    enum {
        KB_LINUX_INPUT_DEV_NAME_OFFSET = 0,
        KB_LINUX_INPUT_DEV_PHYS_OFFSET = 8,
        KB_LINUX_INPUT_DEV_UNIQ_OFFSET = 16,
        KB_LINUX_INPUT_DEV_ID_OFFSET = 24,
        KB_LINUX_INPUT_DEV_EVBIT_OFFSET = 40,
        KB_LINUX_INPUT_DEV_KEYBIT_OFFSET = 48,
        KB_LINUX_INPUT_DEV_RELBIT_OFFSET = 144,
        BUS_USB = 0x03,
        EV_SYN = 0x00,
        EV_KEY = 0x01,
        EV_REL = 0x02,
        REL_X = 0x00,
        REL_Y = 0x01,
        BTN_LEFT = 0x110,
    };

    if (!usb_event_injection_enabled() || !usb_synthetic_hid_mouse_enabled()) {
        return 0;
    }

    if (!usb_synthetic_hid_mouse_registered) {
        const char *name = "Kobox USB HID Mouse";
        const char *phys = "kobox-usb/input0";
        const char *uniq = "0001";
        kb_input_id_t id = {
            .bustype = BUS_USB,
            .vendor = 0x1d6b,
            .product = 0x1002,
            .version = 0x0100,
        };
        memset(usb_synthetic_hid_mouse_input, 0, sizeof(usb_synthetic_hid_mouse_input));
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_NAME_OFFSET, &name, sizeof(name));
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_PHYS_OFFSET, &phys, sizeof(phys));
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_UNIQ_OFFSET, &uniq, sizeof(uniq));
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_ID_OFFSET, &id, sizeof(id));
        const uint64_t event_bits =
            (UINT64_C(1) << EV_SYN) |
            (UINT64_C(1) << EV_KEY) |
            (UINT64_C(1) << EV_REL);
        const uint64_t key_bits = UINT64_C(1) << (BTN_LEFT % 64u);
        const uint64_t rel_bits =
            (UINT64_C(1) << REL_X) | (UINT64_C(1) << REL_Y);
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_EVBIT_OFFSET,
            &event_bits, sizeof(event_bits));
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_KEYBIT_OFFSET +
            ((BTN_LEFT / 64u) * sizeof(key_bits)), &key_bits, sizeof(key_bits));
        memcpy(usb_synthetic_hid_mouse_input + KB_LINUX_INPUT_DEV_RELBIT_OFFSET,
            &rel_bits, sizeof(rel_bits));
        if (kb_input_subsystem_register_device(usb_synthetic_hid_mouse_input) != 0) {
            return 0;
        }
        usb_synthetic_hid_mouse_registered = 1;
    }

    if (usb_synthetic_hid_mouse_events_sent && !usb_hid_mouse_live_enabled()) {
        return 1;
    }

    size_t report_count = sizeof(usb_hid_mouse_input_reports) / sizeof(usb_hid_mouse_input_reports[0]);
    size_t begin = 0;
    size_t end = report_count;
    if (usb_hid_mouse_live_enabled()) {
        begin = usb_hid_mouse_report_index % report_count;
        end = begin + 1;
    }

    for (size_t i = begin; i < end; i++) {
        size_t report_index = i % report_count;
        kb_input_subsystem_record_event(usb_synthetic_hid_mouse_input, EV_KEY, BTN_LEFT, usb_hid_mouse_input_reports[report_index][0]);
        kb_input_subsystem_record_event(usb_synthetic_hid_mouse_input, EV_REL, REL_X, usb_hid_mouse_input_reports[report_index][1]);
        kb_input_subsystem_record_event(usb_synthetic_hid_mouse_input, EV_REL, REL_Y, usb_hid_mouse_input_reports[report_index][2]);
        kb_input_subsystem_record_event(usb_synthetic_hid_mouse_input, EV_SYN, 0, 0);
    }
    if (usb_hid_mouse_live_enabled()) {
        usb_hid_mouse_report_index++;
    }
    usb_synthetic_hid_mouse_events_sent = 1;
    return 1;
}

int kb_usb_add_hcd(void *hcd, unsigned int irqnum, unsigned long irqflags)
{
    (void)irqflags;
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record == NULL) {
        return -22;
    }
    record->irq = irqnum;
    record->added = 1;
    if (trace_usb_enabled()) {
        fprintf(stderr, "kobox usb: usb_add_hcd hcd=%p irq=%u primary=%d\n", hcd, irqnum, record->primary);
    }
    return 0;
}

void kb_usb_remove_hcd(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    if (record != NULL) {
        record->added = 0;
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: usb_remove_hcd hcd=%p\n", hcd);
        }
    }
}

void *kb_usb_create_shared_hcd(const void *driver, void *dev, const char *bus_name, void *primary_hcd)
{
    (void)bus_name;
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_alloc(KB_USB_HCD_STORAGE_SIZE);
    if (record == NULL) {
        return NULL;
    }
    record->primary = 0;
    record->owner = dev;
    record->driver = driver;
    record->primary_hcd = primary_hcd;
    if (trace_usb_enabled()) {
        fprintf(stderr, "kobox usb: usb_create_shared_hcd hcd=%p primary=%p\n", record->hcd, primary_hcd);
    }
    return record->hcd;
}

void kb_usb_put_hcd(void *hcd)
{
    usb_hcd_release_record(kb_usb_subsystem_hcd_for_hcd(hcd));
}

int kb_usb_hcd_pci_probe(void *dev, const void *driver)
{
    if (dev == NULL) {
        return -22;
    }
    int (*real_probe)(void *, const void *) =
        (int (*)(void *, const void *))kb_module_lookup_exported_symbol("usb_hcd_pci_probe");
    if (real_probe != NULL) {
        usb_patch_hc_driver_callbacks_for_usbcore(driver);
        usb_patch_hub_control_for_driver(driver);
        void *previous_probe_dev = usb_active_probe_dev;
        const void *previous_probe_driver = usb_active_probe_driver;
        usb_active_probe_dev = dev;
        usb_active_probe_driver = driver;
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_probe, &old_gs);
        int result = real_probe(dev, driver);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        usb_active_probe_dev = previous_probe_dev;
        usb_active_probe_driver = previous_probe_driver;
        if (result != 0) {
            if (trace_usb_enabled()) {
                fprintf(stderr,
                    "kobox usb: usb_hcd_pci_probe real failed dev=%p driver=%p result=%d drvdata=%p\n",
                    dev,
                    driver,
                    result,
                    usb_load_pci_driver_data(dev));
            }
            return result;
        }
        void *hcd = usb_load_pci_driver_data(dev);
        kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_track(hcd);
        if (record == NULL) {
            if (trace_usb_enabled()) {
                fprintf(stderr,
                    "kobox usb: usb_hcd_pci_probe tracking failed dev=%p driver=%p hcd=%p\n",
                    dev,
                    driver,
                    hcd);
            }
            return -12;
        }
        record->primary = 1;
        record->owner = dev;
        record->driver = driver;
        record->added = 1;
        int vector = kb_pci_irq_vector(dev, 0);
        if (vector >= 0) {
            record->irq = (unsigned int)vector;
        }
        usb_patch_hub_control_for_hcd(hcd);
        usb_track_real_shared_hcd(hcd, record);
        if (trace_usb_enabled()) {
            fprintf(stderr,
                "kobox usb: usb_hcd_pci_probe tracked real dev=%p hcd=%p driver=%p irq=%u\n",
                dev,
                record->hcd,
                driver,
                record->irq);
        }
        return 0;
    }
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_alloc(KB_USB_HCD_STORAGE_SIZE);
    if (record == NULL) {
        return -12;
    }
    record->primary = 1;
    record->owner = dev;
    record->driver = driver;
    usb_store_pci_driver_data(dev, record->hcd);

    int result = kb_pci_enable_device(dev);
    if (result != 0) {
        usb_hcd_release_record(record);
        return result;
    }
    kb_pci_set_master(dev);
    record->regs = kb_pci_iomap(dev, 0, 0);
    (void)kb_pci_alloc_irq_vectors(dev, 1, 1, 0);
    int vector = kb_pci_irq_vector(dev, 0);
    if (vector < 0) {
        vector = 0;
    }
    record->irq = (unsigned int)vector;
    result = kb_request_threaded_irq(record->irq, kb_usb_hcd_irq, NULL, 0, "kobox-usb-hcd", record->hcd);
    if (result == 0) {
        record->irq_registered = 1;
    }
    result = kb_usb_add_hcd(record->hcd, record->irq, 0);
    if (result != 0) {
        usb_hcd_release_record(record);
        return result;
    }
    if (trace_usb_enabled()) {
        fprintf(stderr, "kobox usb: usb_hcd_pci_probe dev=%p hcd=%p driver=%p regs=%p irq=%u\n",
            dev,
            record->hcd,
            driver,
            record->regs,
            record->irq);
    }
    return 0;
}

void kb_usb_hcd_pci_remove(void *dev)
{
    void (*real_remove)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_hcd_pci_remove");
    if (real_remove != NULL) {
        kb_usb_hcd_record_t *record = kb_usb_subsystem_primary_hcd_for_owner(dev);
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_remove, &old_gs);
        real_remove(dev);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (record != NULL) {
            kb_usb_subsystem_hcd_release(record);
        }
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: usb_hcd_pci_remove real dev=%p\n", dev);
        }
        return;
    }
    kb_usb_hcd_record_t *record = kb_usb_subsystem_primary_hcd_for_owner(dev);
    if (record == NULL) {
        return;
    }
    usb_store_pci_driver_data(dev, NULL);
    kb_usb_remove_hcd(record->hcd);
    usb_hcd_release_record(record);
    kb_pci_free_irq_vectors(dev);
    kb_pci_disable_device(dev);
    if (trace_usb_enabled()) {
        fprintf(stderr, "kobox usb: usb_hcd_pci_remove dev=%p\n", dev);
    }
}

void kb_usb_hcd_pci_shutdown(void *dev)
{
    void (*real_shutdown)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_hcd_pci_shutdown");
    if (real_shutdown != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_shutdown, &old_gs);
        real_shutdown(dev);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    if (trace_usb_enabled()) {
        fprintf(stderr, "kobox usb: usb_hcd_pci_shutdown dev=%p\n", dev);
    }
}

void *kb_usb_hcd_pci_pm_ops_storage(void)
{
    return usb_hcd_pci_pm_ops;
}

void *kb_usb_xhci_tracepoint_storage(void)
{
    return usb_xhci_tracepoint;
}

void *kb_usb_num_online_cpus_storage(void)
{
    return &usb_num_online_cpus;
}

void *kb_usb_pcpu_hot_storage(void)
{
    return usb_pcpu_hot;
}

void *kb_usb_pm_suspend_target_state_storage(void)
{
    return &usb_pm_suspend_target_state;
}

void kb_usb_hc_died(void *hcd)
{
    kb_usb_subsystem_hcd_note_died(hcd);
}

int kb_usb_hcd_amd_remote_wakeup_quirk(void *hcd)
{
    kb_usb_subsystem_hcd_note_remote_wakeup_quirk(hcd);
    return 0;
}

int kb_usb_hcd_check_unlink_urb(void *hcd, void *urb, int status)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return -22;
    }
    return kb_usb_subsystem_urb_check_unlink(hcd, urb, status);
}

void kb_usb_hcd_end_port_resume(void *hcd)
{
    kb_usb_subsystem_hcd_note_port_resume_end(hcd);
}

void kb_usb_hcd_giveback_urb(void *hcd, void *urb, int status)
{
    if (usb_pointer_is_error_or_low(urb)) {
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: skip low giveback_urb hcd=%p urb=%p status=%d\n", hcd, urb, status);
        }
        return;
    }
    if (urb != NULL) {
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        uint32_t actual_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET);
        uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
        usb_write_ptr_field(urb, KB_LINUX_6_8_URB_HCPRIV_OFFSET, NULL);
        usb_write_int_field(urb, KB_LINUX_6_8_URB_STATUS_OFFSET, status);
        if (status == 0 &&
            actual_length == 0 &&
            transfer_length != 0 &&
            (transfer_flags & KB_USB_URB_DIR_IN) == 0)
        {
            usb_write_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET, transfer_length);
        }
    }
    kb_usb_subsystem_urb_giveback(hcd, urb, status);
    if (trace_usb_enabled()) {
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        uint32_t actual_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET);
        uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
        fprintf(stderr,
            "kobox usb: giveback_urb hcd=%p urb=%p status=%d flags=0x%08x len=%u actual=%u\n",
            hcd,
            urb,
            status,
            transfer_flags,
            transfer_length,
            actual_length);
    }
    void *complete_ptr = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET);
    void (*complete)(void *) = NULL;
    memcpy(&complete, &complete_ptr, sizeof(complete));
    if (complete != NULL) {
        if (!kb_module_is_executable_address((const void *)complete)) {
            if (trace_usb_enabled()) {
                fprintf(stderr, "kobox usb: skip invalid giveback completion hcd=%p urb=%p complete=%p\n", hcd, urb, (void *)complete);
            }
            return;
        }
        unsigned long old_gs = 0;
        unsigned long kernel_gs = kb_module_kernel_gs_for_address((const void *)complete);
        if (kernel_gs == 0) {
            kernel_gs = kb_shim_current_kernel_gs();
        }
        int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
        kb_linux_call_void_ptr_gs(complete, urb, kernel_gs);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
}

int kb_usb_hcd_link_urb_to_ep(void *hcd, void *urb)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return -22;
    }
    return kb_usb_subsystem_urb_link(hcd, urb);
}

int kb_usb_hcd_map_urb_for_dma(void *hcd, void *urb, unsigned int mem_flags)
{
    if (hcd == NULL || usb_pointer_is_error_or_low(urb)) {
        return -22;
    }

    uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
    uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
    uint32_t actual_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET);
    void *transfer_buffer = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_OFFSET);
    void *setup_packet = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_SETUP_PACKET_OFFSET);
    uint64_t transfer_dma = usb_read_u64_field(urb, KB_LINUX_6_8_URB_TRANSFER_DMA_OFFSET);
    uint64_t setup_dma = usb_read_u64_field(urb, KB_LINUX_6_8_URB_SETUP_DMA_OFFSET);
    uint32_t dma_map_flags = 0;

    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return -19;
    }

    if (transfer_length != 0 && (transfer_flags & KB_USB_URB_NO_TRANSFER_DMA_MAP) == 0) {
        if (transfer_buffer == NULL) {
            return -22;
        }
        kb_status_t map_status = KB_ERR_INVALID;
        transfer_dma = kb_subsystem_dma_map(
            backend,
            device,
            transfer_buffer,
            transfer_length,
            usb_urb_dma_direction(transfer_flags),
            &map_status);
        if (map_status != KB_OK) {
            return usb_status_from_kb_status(map_status);
        }
        if (trace_usb_enabled()) {
            fprintf(stderr,
                "kobox usb: map_urb transfer hcd=%p urb=%p buffer=%p len=%u flags=0x%08x dma=0x%llx\n",
                hcd,
                urb,
                transfer_buffer,
                transfer_length,
                transfer_flags,
                (unsigned long long)transfer_dma);
        }
        usb_write_u64_field(urb, KB_LINUX_6_8_URB_TRANSFER_DMA_OFFSET, transfer_dma);
        transfer_flags |= KB_USB_URB_DMA_MAP_SINGLE;
        dma_map_flags |= KB_USB_URB_DMA_MAP_SINGLE;
    }

    if (setup_packet != NULL && (transfer_flags & KB_USB_URB_SETUP_MAP_SINGLE) == 0) {
        kb_status_t map_status = KB_ERR_INVALID;
        setup_dma = kb_subsystem_dma_map(
            backend,
            device,
            setup_packet,
            KB_USB_CONTROL_SETUP_SIZE,
            KB_DMA_TO_DEVICE,
            &map_status);
        if (map_status != KB_OK) {
            if ((dma_map_flags & KB_USB_URB_DMA_MAP_SINGLE) != 0) {
                kb_subsystem_dma_unmap(
                    backend,
                    device,
                    transfer_dma,
                    transfer_length,
                    usb_urb_dma_direction(transfer_flags));
            }
            return usb_status_from_kb_status(map_status);
        }
        if (trace_usb_enabled()) {
            fprintf(stderr,
                "kobox usb: map_urb setup hcd=%p urb=%p setup=%p len=%u dma=0x%llx\n",
                hcd,
                urb,
                setup_packet,
                (unsigned)KB_USB_CONTROL_SETUP_SIZE,
                (unsigned long long)setup_dma);
        }
        usb_write_u64_field(urb, KB_LINUX_6_8_URB_SETUP_DMA_OFFSET, setup_dma);
        transfer_flags |= KB_USB_URB_SETUP_MAP_SINGLE;
        dma_map_flags |= KB_USB_URB_SETUP_MAP_SINGLE;
    }

    usb_write_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET, transfer_flags);
    kb_usb_urb_dma_update_t update = {
        .transfer_buffer = transfer_buffer,
        .setup_packet = setup_packet,
        .transfer_dma = transfer_dma,
        .setup_dma = setup_dma,
        .transfer_buffer_length = transfer_length,
        .actual_length = actual_length,
        .transfer_flags = transfer_flags,
        .dma_map_flags = dma_map_flags,
        .mem_flags = mem_flags,
    };
    return kb_usb_subsystem_urb_map_dma(hcd, urb, &update);
}

void kb_usb_hcd_poll_rh_status(void *hcd)
{
    kb_usb_subsystem_hcd_note_root_hub_poll(hcd);
    (void)usb_root_hub_status_complete(hcd);
}

void kb_usb_hcd_resume_root_hub(void *hcd)
{
    kb_usb_subsystem_hcd_note_root_hub_resume(hcd);
}

void kb_usb_hcd_start_port_resume(void *hcd)
{
    kb_usb_subsystem_hcd_note_port_resume_start(hcd);
}

void kb_usb_hcd_unlink_urb_from_ep(void *hcd, void *urb)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return;
    }
    kb_usb_subsystem_urb_unlink(hcd, urb, 0);
}

void kb_usb_hcd_unmap_urb_for_dma(void *hcd, void *urb)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return;
    }
    if (urb != NULL) {
        uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        uint64_t transfer_dma = usb_read_u64_field(urb, KB_LINUX_6_8_URB_TRANSFER_DMA_OFFSET);
        uint64_t setup_dma = usb_read_u64_field(urb, KB_LINUX_6_8_URB_SETUP_DMA_OFFSET);
        kb_device_backend_t *backend = kb_shim_current_device_backend();
        kb_device_t *device = kb_subsystem_dma_default_device(backend);
        if (device != NULL) {
            if ((transfer_flags & KB_USB_URB_DMA_MAP_SINGLE) != 0 && transfer_length != 0) {
                kb_subsystem_dma_unmap(
                    backend,
                    device,
                    transfer_dma,
                    transfer_length,
                    usb_urb_dma_direction(transfer_flags));
                transfer_flags &= ~((uint32_t)KB_USB_URB_DMA_MAP_SINGLE);
            }
            if ((transfer_flags & KB_USB_URB_SETUP_MAP_SINGLE) != 0) {
                kb_subsystem_dma_unmap(
                    backend,
                    device,
                    setup_dma,
                    KB_USB_CONTROL_SETUP_SIZE,
                    KB_DMA_TO_DEVICE);
                transfer_flags &= ~((uint32_t)KB_USB_URB_SETUP_MAP_SINGLE);
            }
            usb_write_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET, transfer_flags);
        }
    }
    kb_usb_subsystem_urb_unmap_dma(hcd, urb);
}

void kb_usb_root_hub_lost_power(void *root_hub)
{
    usb_mark_hcd_for_root_hub(root_hub, kb_usb_subsystem_hcd_note_lost_power);
}

int kb_usb_hub_clear_tt_buffer(void *udev, int pipe)
{
    (void)udev;
    (void)pipe;
    return 0;
}

void kb_usb_wakeup_notification(void *hdev, unsigned int portnum)
{
    (void)portnum;
    usb_mark_hcd_for_root_hub(hdev, kb_usb_subsystem_hcd_note_wakeup_notification);
}

int kb_usb_decode_interval(void *udev, void *ep)
{
    (void)udev;
    (void)ep;
    return 1;
}

const char *kb_usb_ep_type_string(int type)
{
    switch (type) {
    case 0:
        return "Control";
    case 1:
        return "Isoc";
    case 2:
        return "Bulk";
    case 3:
        return "Interrupt";
    default:
        return "Unknown";
    }
}

const char *kb_usb_speed_string(int speed)
{
    switch (speed) {
    case 1:
        return "low-speed";
    case 2:
        return "full-speed";
    case 3:
        return "high-speed";
    case 4:
        return "wireless";
    case 5:
        return "super-speed";
    case 6:
        return "super-speed-plus";
    default:
        return "UNKNOWN";
    }
}

const char *kb_usb_state_string(int state)
{
    switch (state) {
    case 0:
        return "not attached";
    case 1:
        return "attached";
    case 2:
        return "powered";
    case 3:
        return "reconnecting";
    case 4:
        return "unauthenticated";
    case 5:
        return "default";
    case 6:
        return "addressed";
    case 7:
        return "configured";
    case 8:
        return "suspended";
    default:
        return "unknown";
    }
}

int kb_usb_register_driver(void *driver, void *owner, const char *mod_name)
{
    int (*real_register)(void *, void *, const char *) =
        (int (*)(void *, void *, const char *))kb_module_lookup_exported_symbol("usb_register_driver");
    int result = -19;
    if (real_register != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_register, &old_gs);
        result = real_register(driver, owner, mod_name);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    kb_usb_subsystem_driver_register(driver, owner, mod_name, result);
    return result;
}

void kb_usb_deregister(void *driver)
{
    void (*real_deregister)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_deregister");
    if (real_deregister != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_deregister, &old_gs);
        real_deregister(driver);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    kb_usb_subsystem_driver_deregister(driver);
}

void kb_usb_deregister_dev(void *interface, const void *class_driver)
{
    void (*real_deregister_dev)(void *, const void *) =
        (void (*)(void *, const void *))kb_module_lookup_exported_symbol("usb_deregister_dev");
    usb_observe_interface(NULL, interface, NULL);
    if (real_deregister_dev != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_deregister_dev, &old_gs);
        real_deregister_dev(interface, class_driver);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
}

void *kb_usb_find_interface(void *driver, int minor)
{
    void *(*real_find_interface)(void *, int) =
        (void *(*)(void *, int))kb_module_lookup_exported_symbol("usb_find_interface");
    void *interface = NULL;
    if (real_find_interface != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_find_interface, &old_gs);
        interface = real_find_interface(driver, minor);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    usb_observe_interface(NULL, interface, driver);
    return interface;
}

int kb_usb_find_common_endpoints(
    void *altsetting,
    void **bulk_in,
    void **bulk_out,
    void **int_in,
    void **int_out)
{
    int (*real_find_common_endpoints)(void *, void **, void **, void **, void **) =
        (int (*)(void *, void **, void **, void **, void **))
            kb_module_lookup_exported_symbol("usb_find_common_endpoints");
    int result = -19;
    if (real_find_common_endpoints != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_find_common_endpoints, &old_gs);
        result = real_find_common_endpoints(altsetting, bulk_in, bulk_out, int_in, int_out);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    if (bulk_in != NULL) {
        usb_observe_endpoint_for_altsetting(altsetting, *bulk_in);
    }
    if (bulk_out != NULL) {
        usb_observe_endpoint_for_altsetting(altsetting, *bulk_out);
    }
    if (int_in != NULL) {
        usb_observe_endpoint_for_altsetting(altsetting, *int_in);
    }
    if (int_out != NULL) {
        usb_observe_endpoint_for_altsetting(altsetting, *int_out);
    }
    return result;
}

typedef struct usb_root_hub_status_urb_record {
    void *hcd;
    void *urb;
    struct usb_root_hub_status_urb_record *next;
} usb_root_hub_status_urb_record_t;

static usb_root_hub_status_urb_record_t *usb_root_hub_status_urbs;
static int usb_root_hub_status_completion_depth;

static int usb_urb_is_root_hub_status(void *urb, void **hcd_out)
{
    if (hcd_out != NULL) {
        *hcd_out = NULL;
    }
    if (urb == NULL) {
        return 0;
    }

    void *dev = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_DEV_OFFSET);
    void *hcd = usb_hcd_for_root_hub(dev);
    if (hcd == NULL) {
        return 0;
    }

    void *setup_packet = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_SETUP_PACKET_OFFSET);
    void *transfer_buffer = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_OFFSET);
    uint32_t transfer_len = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
    void *complete = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET);
    if (setup_packet != NULL || transfer_buffer == NULL || transfer_len == 0 || transfer_len > 8 || complete == NULL) {
        return 0;
    }

    if (hcd_out != NULL) {
        *hcd_out = hcd;
    }
    return 1;
}

static usb_root_hub_status_urb_record_t *usb_root_hub_status_record_for_hcd(void *hcd)
{
    for (usb_root_hub_status_urb_record_t *record = usb_root_hub_status_urbs;
         record != NULL;
         record = record->next)
    {
        if (record->hcd == hcd) {
            return record;
        }
    }
    return NULL;
}

static void usb_root_hub_status_urb_track(void *hcd, void *urb)
{
    if (hcd == NULL || urb == NULL) {
        return;
    }

    usb_root_hub_status_urb_record_t *record = usb_root_hub_status_record_for_hcd(hcd);
    if (record == NULL) {
        record = kb_kzalloc(sizeof(*record), 0);
        if (record == NULL) {
            return;
        }
        record->next = usb_root_hub_status_urbs;
        usb_root_hub_status_urbs = record;
    }
    record->hcd = hcd;
    record->urb = urb;
    usb_write_ptr_field(hcd, KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET, urb);

    if (trace_usb_enabled()) {
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        void *ep = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_EP_OFFSET);
        uint32_t pipe = usb_read_u32_field(urb, KB_LINUX_6_8_URB_PIPE_OFFSET);
        fprintf(stderr,
            "kobox usb: root_hub_status_track hcd=%p urb=%p ep=%p pipe=0x%08x len=%u\n",
            hcd,
            urb,
            ep,
            pipe,
            transfer_length);
    }
}

static void usb_root_hub_status_urb_forget(void *urb)
{
    if (urb == NULL) {
        return;
    }
    usb_root_hub_status_urb_record_t **cursor = &usb_root_hub_status_urbs;
    while (*cursor != NULL) {
        usb_root_hub_status_urb_record_t *record = *cursor;
        if (record->urb == urb) {
            void *status_urb = usb_read_ptr_field(record->hcd, KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET);
            if (status_urb == urb) {
                usb_write_ptr_field(record->hcd, KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET, NULL);
            }
            *cursor = record->next;
            kb_kfree(record);
            return;
        }
        cursor = &record->next;
    }
}

static int usb_root_hub_status_complete(void *hcd)
{
    if (hcd == NULL || usb_root_hub_status_completion_depth > 8) {
        return 0;
    }

    usb_root_hub_status_urb_record_t *record = usb_root_hub_status_record_for_hcd(hcd);
    void *urb = record != NULL ? record->urb : usb_read_ptr_field(hcd, KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET);
    if (urb == NULL) {
        return 0;
    }

    unsigned char status[8] = { 0 };
    int status_len = usb_real_device_enabled() ?
        usb_real_root_hub_status_data_from_xhci(hcd, status) :
        usb_root_hub_status_data(hcd, status);
    if (status_len <= 0) {
        return 0;
    }

    void *transfer_buffer = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_OFFSET);
    uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
    if (transfer_buffer == NULL || transfer_length == 0) {
        return 0;
    }

    int giveback_status = 0;
    uint32_t actual_length = (uint32_t)status_len;
    if (actual_length > transfer_length) {
        actual_length = transfer_length;
        giveback_status = -75;
    }
    memcpy(transfer_buffer, status, actual_length);
    usb_write_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET, actual_length);
    usb_write_ptr_field(hcd, KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET, NULL);
    if (record != NULL) {
        record->urb = NULL;
    }

    if (trace_usb_enabled()) {
        fprintf(stderr,
            "kobox usb: root_hub_status_complete hcd=%p urb=%p len=%u status=%d data=%02x %02x %02x %02x\n",
            hcd,
            urb,
            actual_length,
            giveback_status,
            status[0],
            status[1],
            status[2],
            status[3]);
    }

    usb_root_hub_status_urb_forget(urb);
    usb_root_hub_status_completion_depth++;
    kb_usb_hcd_unlink_urb_from_ep(hcd, urb);
    kb_usb_hcd_giveback_urb(hcd, urb, giveback_status);
    usb_root_hub_status_completion_depth--;
    return 1;
}

int kb_usb_submit_urb(void *urb, unsigned int mem_flags)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return -22;
    }
    int (*real_submit_urb)(void *, unsigned int) =
        (int (*)(void *, unsigned int))kb_module_lookup_exported_symbol("usb_submit_urb");
    if (real_submit_urb == kb_usb_submit_urb) {
        real_submit_urb = NULL;
    }
    void *root_hub_hcd = NULL;
    int is_root_hub_status = usb_urb_is_root_hub_status(urb, &root_hub_hcd);
    usb_patch_urb_complete_for_usbcore(urb);
    unsigned long old_gs = 0;
    int has_gs = usb_enter_function_gs((const void *)real_submit_urb, &old_gs);
    int result = real_submit_urb != NULL ? kb_linux_call_int_ptr_uint(real_submit_urb, urb, mem_flags) : -19;
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    kb_usb_subsystem_urb_submit(NULL, urb, mem_flags, result);
    if (trace_usb_enabled()) {
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
        void *complete = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET);
        fprintf(stderr,
            "kobox usb: submit_urb urb=%p flags=0x%08x len=%u complete=%p result=%d\n",
            urb,
            transfer_flags,
            transfer_length,
            complete,
            result);
    }
    if (result == 0 && is_root_hub_status) {
        usb_root_hub_status_urb_track(root_hub_hcd, urb);
    }
    if (result == 0) {
        (void)usb_synthetic_hid_mouse_complete_urb(urb);
    }
    return result;
}

int kb_usb_unlink_urb(void *urb)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return -22;
    }
    kb_usb_urb_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (kb_usb_subsystem_urb_snapshot(urb, &snapshot) == 0 && snapshot.linked) {
        kb_usb_hcd_unlink_urb_from_ep(snapshot.hcd, urb);
        kb_usb_hcd_giveback_urb(snapshot.hcd, urb, -104);
        return 0;
    }
    int (*real_unlink_urb)(void *) = (int (*)(void *))kb_module_lookup_exported_symbol("usb_unlink_urb");
    if (real_unlink_urb == kb_usb_unlink_urb) {
        real_unlink_urb = NULL;
    }
    unsigned long old_gs = 0;
    int has_gs = usb_enter_function_gs((const void *)real_unlink_urb, &old_gs);
    int result = real_unlink_urb != NULL ? kb_linux_call_int_ptr(real_unlink_urb, urb) : -19;
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    usb_root_hub_status_urb_forget(urb);
    kb_usb_subsystem_urb_unlink(NULL, urb, result);
    return result;
}

void kb_usb_kill_urb(void *urb)
{
    if (usb_pointer_is_error_or_low(urb)) {
        return;
    }
    kb_usb_urb_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (kb_usb_subsystem_urb_snapshot(urb, &snapshot) == 0 && snapshot.linked) {
        kb_usb_hcd_unlink_urb_from_ep(snapshot.hcd, urb);
        kb_usb_hcd_giveback_urb(snapshot.hcd, urb, -104);
        kb_usb_subsystem_urb_kill(snapshot.hcd, urb);
        return;
    }
    void (*real_kill_urb)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_kill_urb");
    if (real_kill_urb == kb_usb_kill_urb) {
        real_kill_urb = NULL;
    }
    if (real_kill_urb != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_kill_urb, &old_gs);
        kb_linux_call_void_ptr(real_kill_urb, urb);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    usb_root_hub_status_urb_forget(urb);
    kb_usb_subsystem_urb_kill(NULL, urb);
}

static int usb_cleanup_one_tracked_urb_dma(const kb_usb_urb_snapshot_t *snapshot, void *ctx)
{
    (void)ctx;
    if (snapshot == NULL || snapshot->mapped == 0 || snapshot->dma_map_flags == 0) {
        return 0;
    }

    kb_device_backend_t *backend = kb_shim_current_device_backend();
    kb_device_t *device = kb_subsystem_dma_default_device(backend);
    if (device == NULL) {
        return 0;
    }

    if ((snapshot->dma_map_flags & KB_USB_URB_DMA_MAP_SINGLE) != 0 &&
        snapshot->transfer_dma != 0 &&
        snapshot->transfer_buffer_length != 0)
    {
        kb_subsystem_dma_unmap(
            backend,
            device,
            snapshot->transfer_dma,
            snapshot->transfer_buffer_length,
            usb_urb_dma_direction(snapshot->transfer_flags));
    }
    if ((snapshot->dma_map_flags & KB_USB_URB_SETUP_MAP_SINGLE) != 0 && snapshot->setup_dma != 0) {
        kb_subsystem_dma_unmap(
            backend,
            device,
            snapshot->setup_dma,
            KB_USB_CONTROL_SETUP_SIZE,
            KB_DMA_TO_DEVICE);
    }
    kb_usb_subsystem_urb_unmap_dma(snapshot->hcd, snapshot->urb);
    return 0;
}

void kb_usb_cleanup_tracked_urb_dma(void)
{
    (void)kb_usb_subsystem_for_each_urb(usb_cleanup_one_tracked_urb_dma, NULL);
}

int kb_usb_control_msg_shim(
    void *dev,
    unsigned int pipe,
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size,
    int timeout)
{
    int (*real_control_msg)(
        void *,
        unsigned int,
        uint8_t,
        uint8_t,
        uint16_t,
        uint16_t,
        void *,
        uint16_t,
        int) =
        (int (*)(void *, unsigned int, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t, int))
            kb_module_lookup_exported_symbol("usb_control_msg");
    const int trace = trace_usb_control_enabled();
    if (trace) {
        fprintf(stderr,
            "kobox usb: control_msg dev=%p pipe=0x%x req=0x%02x type=0x%02x value=0x%04x index=0x%04x data=%p size=%u timeout=%d\n",
            dev,
            pipe,
            (unsigned)request,
            (unsigned)requesttype,
            (unsigned)value,
            (unsigned)index,
            data,
            (unsigned)size,
            timeout);
    }
    const int prefer_real_xhci_control =
        usb_real_device_enabled() && !usb_pachaos_fake_root_hub_enabled();
    const int root_hub_device = usb_hcd_for_root_hub(dev) != NULL;
    int real_tried = 0;
    int result = -19;
    if (prefer_real_xhci_control) {
        int root_hub_result = usb_root_hub_control_msg_via_hub_control(
            dev,
            request,
            requesttype,
            value,
            index,
            data,
            size);
        if (root_hub_result != -95) {
            if (trace) {
                unsigned char b0 = data != NULL && size > 0 ? ((unsigned char *)data)[0] : 0;
                unsigned char b1 = data != NULL && size > 1 ? ((unsigned char *)data)[1] : 0;
                unsigned char b2 = data != NULL && size > 2 ? ((unsigned char *)data)[2] : 0;
                unsigned char b3 = data != NULL && size > 3 ? ((unsigned char *)data)[3] : 0;
                fprintf(stderr,
                    "kobox usb: control_msg root-hub-bridge result=%d data=%02x %02x %02x %02x\n",
                    root_hub_result,
                    b0,
                    b1,
                    b2,
                    b3);
            }
            return root_hub_result;
        }
        if (root_hub_device) {
            root_hub_result = usb_real_root_hub_control_msg(dev, request, requesttype, value, index, data, size);
            if (root_hub_result != -95) {
                if (trace) {
                    unsigned char b0 = data != NULL && size > 0 ? ((unsigned char *)data)[0] : 0;
                    unsigned char b1 = data != NULL && size > 1 ? ((unsigned char *)data)[1] : 0;
                    unsigned char b2 = data != NULL && size > 2 ? ((unsigned char *)data)[2] : 0;
                    unsigned char b3 = data != NULL && size > 3 ? ((unsigned char *)data)[3] : 0;
                    fprintf(stderr,
                        "kobox usb: control_msg root-hub-shim result=%d data=%02x %02x %02x %02x\n",
                        root_hub_result,
                        b0,
                        b1,
                        b2,
                        b3);
                }
                return root_hub_result;
            }
            if (trace) {
                fprintf(stderr, "kobox usb: control_msg root-hub unsupported result=%d\n", root_hub_result);
            }
            return root_hub_result;
        }
    }
    if (prefer_real_xhci_control && real_control_msg != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_control_msg, &old_gs);
        result = kb_linux_call_int_ptr_uint_u8_u8_u16_u16_ptr_u16_int(
            real_control_msg,
            dev,
            pipe,
            request,
            requesttype,
            value,
            index,
            data,
            size,
            timeout);
        real_tried = 1;
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        if (result >= 0) {
            if (trace) {
                unsigned char b0 = data != NULL && size > 0 ? ((unsigned char *)data)[0] : 0;
                unsigned char b1 = data != NULL && size > 1 ? ((unsigned char *)data)[1] : 0;
                unsigned char b2 = data != NULL && size > 2 ? ((unsigned char *)data)[2] : 0;
                unsigned char b3 = data != NULL && size > 3 ? ((unsigned char *)data)[3] : 0;
                fprintf(stderr,
                    "kobox usb: control_msg real-xhci result=%d data=%02x %02x %02x %02x\n",
                    result,
                    b0,
                    b1,
                    b2,
                    b3);
            }
            return result;
        }
        if (trace) {
            fprintf(stderr, "kobox usb: control_msg real-xhci fallback result=%d\n", result);
        }
    }
    int root_hub_result = usb_real_root_hub_control_msg(dev, request, requesttype, value, index, data, size);
    if (root_hub_result != -95) {
        if (trace) {
            unsigned char b0 = data != NULL && size > 0 ? ((unsigned char *)data)[0] : 0;
            unsigned char b1 = data != NULL && size > 1 ? ((unsigned char *)data)[1] : 0;
            unsigned char b2 = data != NULL && size > 2 ? ((unsigned char *)data)[2] : 0;
            unsigned char b3 = data != NULL && size > 3 ? ((unsigned char *)data)[3] : 0;
            fprintf(stderr,
                "kobox usb: control_msg real-root-hub result=%d data=%02x %02x %02x %02x\n",
                root_hub_result,
                b0,
                b1,
                b2,
                b3);
        }
        return root_hub_result;
    }
    if (prefer_real_xhci_control) {
        if (trace) {
            fprintf(stderr, "kobox usb: control_msg real-xhci no-synthetic result=%d\n", result);
        }
        return result;
    }
    int synthetic_result = kb_usb_synthetic_control_msg(request, requesttype, value, index, data, size);
    if (synthetic_result != -95) {
        if (trace) {
            fprintf(stderr, "kobox usb: control_msg synthetic result=%d\n", synthetic_result);
        }
        return synthetic_result;
    }
    if (!real_tried && real_control_msg != NULL) {
        unsigned long old_gs = 0;
        int has_gs = usb_enter_function_gs((const void *)real_control_msg, &old_gs);
        result = kb_linux_call_int_ptr_uint_u8_u8_u16_u16_ptr_u16_int(
            real_control_msg,
            dev,
            pipe,
            request,
            requesttype,
            value,
            index,
            data,
            size,
            timeout);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
    }
    if (trace) {
        fprintf(stderr, "kobox usb: control_msg result=%d\n", result);
    }
    return result;
}

#if defined(__x86_64__) && !defined(_MSC_VER)
__attribute__((naked)) int kb_usb_control_msg_entry(
    void *dev,
    unsigned int pipe,
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size,
    int timeout)
{
    __asm__ volatile(
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rsp, %rbx\n\t"
        "mov 56(%rbx), %r10\n\t"
        "mov 64(%rbx), %r11\n\t"
        "mov 72(%rbx), %rax\n\t"
        "and $-16, %rsp\n\t"
        "sub $32, %rsp\n\t"
        "mov %r10, 0(%rsp)\n\t"
        "mov %r11, 8(%rsp)\n\t"
        "mov %rax, 16(%rsp)\n\t"
        "call kb_usb_control_msg_shim\n\t"
        "mov %rbx, %rsp\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t");
}
#else
int kb_usb_control_msg_entry(
    void *dev,
    unsigned int pipe,
    uint8_t request,
    uint8_t requesttype,
    uint16_t value,
    uint16_t index,
    void *data,
    uint16_t size,
    int timeout)
{
    return kb_usb_control_msg_shim(dev, pipe, request, requesttype, value, index, data, size, timeout);
}
#endif

void kb_xhci_init_driver(void *driver, const void *overrides)
{
    (void)driver;
    (void)overrides;
}

int kb_xhci_gen_setup(void *hcd, void *xhci, const void *quirks)
{
    (void)hcd;
    (void)xhci;
    (void)quirks;
    return 0;
}

int kb_xhci_run(void *hcd)
{
    (void)hcd;
    return 0;
}

void kb_xhci_stop(void *hcd)
{
    (void)hcd;
}

void kb_xhci_shutdown(void *hcd)
{
    (void)hcd;
}

int kb_xhci_suspend(void *xhci, bool do_wakeup)
{
    (void)xhci;
    (void)do_wakeup;
    return 0;
}

int kb_xhci_resume(void *xhci, bool hibernated)
{
    (void)xhci;
    (void)hibernated;
    return 0;
}

int kb_xhci_ext_cap_init(void *xhci)
{
    (void)xhci;
    return 0;
}

int kb_xhci_update_hub_device(void *hcd, void *udev, void *tt, unsigned int devnum)
{
    (void)tt;
    usb_observe_device_graph_with_devnum(hcd, udev, devnum);
    return 0;
}

int kb_xhci_find_slot_id_by_port(void *hcd, void *xhci, unsigned int port)
{
    (void)hcd;
    (void)xhci;
    (void)port;
    return 0;
}

uint32_t kb_xhci_port_state_to_neutral(uint32_t state)
{
    return state;
}

int kb_xhci_msi_irq(int irq, void *hcd)
{
    return kb_usb_hcd_irq(irq, hcd);
}

void kb_xhci_dbg_trace(void *xhci, void *trace, const char *fmt, ...)
{
    (void)xhci;
    (void)trace;
    if (!trace_usb_enabled() || fmt == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)kb_vprintk_safe(fmt, args);
    va_end(args);
}

int usb_hcd_pci_probe(void *dev, const void *driver)
{
    return kb_usb_hcd_pci_probe(dev, driver);
}

void usb_hcd_pci_remove(void *dev)
{
    kb_usb_hcd_pci_remove(dev);
}

void usb_hcd_pci_shutdown(void *dev)
{
    kb_usb_hcd_pci_shutdown(dev);
}

int usb_hcd_irq(int irq, void *hcd)
{
    return kb_usb_hcd_irq(irq, hcd);
}

int usb_hcd_is_primary_hcd(void *hcd)
{
    return kb_usb_hcd_is_primary_hcd(hcd);
}

int usb_add_hcd(void *hcd, unsigned int irqnum, unsigned long irqflags)
{
    return kb_usb_add_hcd(hcd, irqnum, irqflags);
}

void usb_remove_hcd(void *hcd)
{
    kb_usb_remove_hcd(hcd);
}

void *usb_create_shared_hcd(const void *driver, void *dev, const char *bus_name, void *primary_hcd)
{
    return kb_usb_create_shared_hcd(driver, dev, bus_name, primary_hcd);
}

void usb_put_hcd(void *hcd)
{
    kb_usb_put_hcd(hcd);
}

void xhci_init_driver(void *driver, const void *overrides)
{
    kb_xhci_init_driver(driver, overrides);
}

int xhci_gen_setup(void *hcd, void *xhci, const void *quirks)
{
    return kb_xhci_gen_setup(hcd, xhci, quirks);
}

int xhci_run(void *hcd)
{
    return kb_xhci_run(hcd);
}

void xhci_stop(void *hcd)
{
    kb_xhci_stop(hcd);
}

void xhci_shutdown(void *hcd)
{
    kb_xhci_shutdown(hcd);
}

int xhci_suspend(void *xhci, bool do_wakeup)
{
    return kb_xhci_suspend(xhci, do_wakeup);
}

int xhci_resume(void *xhci, bool hibernated)
{
    return kb_xhci_resume(xhci, hibernated);
}

int xhci_ext_cap_init(void *xhci)
{
    return kb_xhci_ext_cap_init(xhci);
}

int xhci_update_hub_device(void *hcd, void *udev, void *tt, unsigned int devnum)
{
    return kb_xhci_update_hub_device(hcd, udev, tt, devnum);
}

int xhci_find_slot_id_by_port(void *hcd, void *xhci, unsigned int port)
{
    return kb_xhci_find_slot_id_by_port(hcd, xhci, port);
}

uint32_t xhci_port_state_to_neutral(uint32_t state)
{
    return kb_xhci_port_state_to_neutral(state);
}

int xhci_msi_irq(int irq, void *hcd)
{
    return kb_xhci_msi_irq(irq, hcd);
}

void xhci_dbg_trace(void *xhci, void *trace, const char *fmt, ...)
{
    (void)xhci;
    (void)trace;
    if (!trace_usb_enabled() || fmt == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)kb_vprintk_safe(fmt, args);
    va_end(args);
}
