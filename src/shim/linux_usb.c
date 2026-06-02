#include "kobox/shim.h"
#include "subsystem/dma/dma.h"
#include "subsystem/usb/usb.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

kb_backend_t *kb_shim_current_backend(void);

enum {
    KB_USB_HCD_STORAGE_SIZE = 4096,
    KB_LINUX_6_8_DEVICE_PARENT_OFFSET = 0x40,
    KB_LINUX_6_8_DEVICE_TYPE_OFFSET = 0x58,
    KB_LINUX_6_8_DEVICE_DRIVER_OFFSET = 0x68,
    KB_LINUX_6_8_PCI_DEV_DRIVER_DATA_OFFSET = 0x140,
    KB_LINUX_6_8_USB_HCD_ROOT_HUB_OFFSET = 0x060,
    KB_LINUX_6_8_USB_HCD_SHARED_HCD_OFFSET = 0x210,
    KB_LINUX_6_8_USB_HCD_STATUS_URB_OFFSET = 0x0d8,
    KB_LINUX_6_8_USB_HCD_DRIVER_OFFSET = 0x120,
    KB_LINUX_6_8_USB_HCD_FLAGS_OFFSET = 0x138,
    KB_LINUX_6_8_USB_HCD_RH_STATE_OFFSET = 0x144,
    KB_LINUX_6_8_HC_DRIVER_HUB_STATUS_DATA_OFFSET = 0x98,
    KB_LINUX_6_8_HC_DRIVER_HUB_CONTROL_OFFSET = 0xa0,
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
    KB_USB_TRACKED_HCD_MAX = 32,
};

static unsigned char usb_hcd_pci_pm_ops[256];
static unsigned char usb_xhci_tracepoint[128];
static unsigned int usb_num_online_cpus = 1;
static unsigned char usb_pcpu_hot[256];
static int usb_pm_suspend_target_state;
static int usb_event_injection_runtime_allowed;

typedef struct usb_hcd_port_state {
    void *hcd;
    unsigned long connected_bits;
} usb_hcd_port_state_t;

static usb_hcd_port_state_t usb_hcd_port_states[KB_USB_TRACKED_HCD_MAX];

static int trace_usb_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_USB");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int usb_event_injection_enabled(void)
{
    const char *value = getenv("KOBOX_ENABLE_USB_EVENT_INJECT");
    return usb_event_injection_runtime_allowed &&
           value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

void kb_usb_set_event_injection_runtime_allowed(int allowed)
{
    usb_event_injection_runtime_allowed = allowed != 0;
}

static void *usb_read_ptr_field(const void *base, size_t offset)
{
    void *value = NULL;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint8_t usb_read_u8_field(const void *base, size_t offset)
{
    uint8_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint16_t usb_read_le16_field(const void *base, size_t offset)
{
    const unsigned char *p = (const unsigned char *)base;
    if (p == NULL) {
        return 0;
    }
    return (uint16_t)(p[offset] | ((uint16_t)p[offset + 1] << 8));
}

static uint32_t usb_read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static uint64_t usb_read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static void usb_write_u32_field(void *base, size_t offset, uint32_t value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void usb_write_int_field(void *base, size_t offset, int value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static void usb_write_u64_field(void *base, size_t offset, uint64_t value)
{
    if (base != NULL) {
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

static int usb_hub_ready_for_events(void *hub)
{
    void *hdev = NULL;
    if (hub == NULL) {
        return 0;
    }
    memcpy(&hdev, (const unsigned char *)hub + KB_LINUX_6_8_USB_HUB_HDEV_OFFSET, sizeof(hdev));
    return hdev != NULL;
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
    kb_usb_subsystem_hcd_release(record);
}

static void usb_store_pci_driver_data(void *pdev, void *data)
{
    if (pdev != NULL) {
        memcpy((unsigned char *)pdev + KB_LINUX_6_8_PCI_DEV_DRIVER_DATA_OFFSET, &data, sizeof(data));
    }
}

static void *usb_root_hub_for_hcd(void *hcd)
{
    void *root_hub = NULL;
    if (hcd != NULL) {
        memcpy(&root_hub, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_ROOT_HUB_OFFSET, sizeof(root_hub));
    }
    return root_hub;
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

static void *usb_hub_for_root_hub(void *root_hub)
{
    void *(*hub_to_struct_hub)(void *) =
        (void *(*)(void *))kb_module_lookup_exported_symbol("usb_hub_to_struct_hub");
    if (hub_to_struct_hub != NULL) {
        return hub_to_struct_hub(root_hub);
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
    return hub_status_data(hcd, (char *)status);
}

static usb_hcd_port_state_t *usb_port_state_for_hcd(void *hcd)
{
    if (hcd == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_TRACKED_HCD_MAX; i++) {
        if (usb_hcd_port_states[i].hcd == hcd) {
            return &usb_hcd_port_states[i];
        }
    }
    for (size_t i = 0; i < KB_USB_TRACKED_HCD_MAX; i++) {
        if (usb_hcd_port_states[i].hcd == NULL) {
            usb_hcd_port_states[i].hcd = hcd;
            return &usb_hcd_port_states[i];
        }
    }
    return NULL;
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

    usb_hcd_port_state_t *state = usb_port_state_for_hcd(hcd);
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
            int result =
                hub_control(hcd, KB_USB_REQ_GET_PORT_STATUS, 0, (uint16_t)port, (char *)port_data, sizeof(port_data));
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

    for (int byte_index = 0; byte_index < status_len && byte_index < 8; byte_index++) {
        unsigned char bits = status[byte_index];
        for (int bit = 0; bit < 8; bit++) {
            if ((bits & (unsigned char)(1u << bit)) == 0) {
                continue;
            }
            int port = (byte_index * 8) + bit;
            if (port == 0) {
                continue;
            }
            unsigned char port_data[4] = { 0 };
            int result =
                hub_control(hcd, KB_USB_REQ_GET_PORT_STATUS, 0, (uint16_t)port, (char *)port_data, sizeof(port_data));
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
    int status_len = usb_root_hub_status_data(hcd, status);

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

static void kick_root_hub_if_changed(void *hcd)
{
    void (*kick_hub_wq)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_kick_hub_wq");
    int inject_events = usb_event_injection_enabled();
    if (hcd == NULL) {
        return;
    }
    if (inject_events && kick_hub_wq == NULL) {
        return;
    }

    unsigned char status[8] = { 0 };
    int status_len = usb_root_hub_status_data(hcd, status);
    usb_filter_empty_port_changes(hcd, status, status_len);
    if (status_len <= 0) {
        return;
    }

    void *root_hub = usb_root_hub_for_hcd(hcd);
    if (root_hub == NULL) {
        return;
    }

    void *hub = usb_hub_for_root_hub(root_hub);
    int hub_ready = usb_hub_ready_for_events(hub);
    kb_usb_hub_event_update_t event;
    if (kb_usb_subsystem_hub_event_prepare(hcd, status, (size_t)status_len, &event) != 0) {
        return;
    }

    if (inject_events && hub != NULL && hub_ready && event.bits != 0) {
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

    if (trace_usb_enabled()) {
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
    if (inject_events && hub != NULL && hub_ready && event.new_bits != 0) {
        kick_hub_wq(root_hub);
    }
}

int kb_usb_hcd_irq(int irq, void *hcd)
{
    (void)irq;
    kb_usb_subsystem_hcd_note_irq(hcd);
    return 1;
}

int kb_usb_hcd_is_primary_hcd(void *hcd)
{
    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_for_hcd(hcd);
    return record == NULL || record->primary;
}

int kb_usb_poll_root_hub(void *hcd)
{
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
        resume_root_hub(hcd);
    }
    kick_root_hub_if_changed(hcd);
    poll_rh_status(hcd);

    void *shared_hcd = NULL;
    memcpy(&shared_hcd, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_SHARED_HCD_OFFSET, sizeof(shared_hcd));
    if (shared_hcd != NULL && shared_hcd != hcd) {
        if (trace_usb_enabled()) {
            fprintf(stderr, "kobox usb: poll_shared_root_hub hcd=%p shared=%p\n", hcd, shared_hcd);
        }
        kb_usb_subsystem_hcd_note_root_hub_poll(shared_hcd);
        trace_root_hub_state("poll_shared_root_hub", shared_hcd);
        usb_observe_device_graph(shared_hcd, usb_root_hub_for_hcd(shared_hcd));
        if (resume_root_hub != NULL) {
            kb_usb_subsystem_hcd_note_root_hub_resume(shared_hcd);
            resume_root_hub(shared_hcd);
        }
        kick_root_hub_if_changed(shared_hcd);
        poll_rh_status(shared_hcd);
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
    return polled;
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
    return kb_usb_subsystem_urb_check_unlink(hcd, urb, status);
}

void kb_usb_hcd_end_port_resume(void *hcd)
{
    kb_usb_subsystem_hcd_note_port_resume_end(hcd);
}

void kb_usb_hcd_giveback_urb(void *hcd, void *urb, int status)
{
    if (urb != NULL) {
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        uint32_t actual_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_ACTUAL_LENGTH_OFFSET);
        uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
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
    void *complete_ptr = usb_read_ptr_field(urb, KB_LINUX_6_8_URB_COMPLETE_OFFSET);
    void (*complete)(void *) = NULL;
    memcpy(&complete, &complete_ptr, sizeof(complete));
    if (complete != NULL) {
        complete(urb);
    }
}

int kb_usb_hcd_link_urb_to_ep(void *hcd, void *urb)
{
    return kb_usb_subsystem_urb_link(hcd, urb);
}

int kb_usb_hcd_map_urb_for_dma(void *hcd, void *urb, unsigned int mem_flags)
{
    if (hcd == NULL || urb == NULL) {
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

    kb_backend_t *backend = kb_shim_current_backend();
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
    kb_usb_subsystem_urb_unlink(hcd, urb, 0);
}

void kb_usb_hcd_unmap_urb_for_dma(void *hcd, void *urb)
{
    if (urb != NULL) {
        uint32_t transfer_flags = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_FLAGS_OFFSET);
        uint32_t transfer_length = usb_read_u32_field(urb, KB_LINUX_6_8_URB_TRANSFER_BUFFER_LENGTH_OFFSET);
        uint64_t transfer_dma = usb_read_u64_field(urb, KB_LINUX_6_8_URB_TRANSFER_DMA_OFFSET);
        uint64_t setup_dma = usb_read_u64_field(urb, KB_LINUX_6_8_URB_SETUP_DMA_OFFSET);
        kb_backend_t *backend = kb_shim_current_backend();
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
    int result = real_register != NULL ? real_register(driver, owner, mod_name) : -19;
    kb_usb_subsystem_driver_register(driver, owner, mod_name, result);
    return result;
}

void kb_usb_deregister(void *driver)
{
    void (*real_deregister)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_deregister");
    if (real_deregister != NULL) {
        real_deregister(driver);
    }
    kb_usb_subsystem_driver_deregister(driver);
}

void kb_usb_deregister_dev(void *interface, const void *class_driver)
{
    void (*real_deregister_dev)(void *, const void *) =
        (void (*)(void *, const void *))kb_module_lookup_exported_symbol("usb_deregister_dev");
    usb_observe_interface(NULL, interface, NULL);
    if (real_deregister_dev != NULL) {
        real_deregister_dev(interface, class_driver);
    }
}

void *kb_usb_find_interface(void *driver, int minor)
{
    void *(*real_find_interface)(void *, int) =
        (void *(*)(void *, int))kb_module_lookup_exported_symbol("usb_find_interface");
    void *interface = real_find_interface != NULL ? real_find_interface(driver, minor) : NULL;
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
    int result = real_find_common_endpoints != NULL ?
        real_find_common_endpoints(altsetting, bulk_in, bulk_out, int_in, int_out) :
        -19;
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

int kb_usb_submit_urb(void *urb, unsigned int mem_flags)
{
    int (*real_submit_urb)(void *, unsigned int) =
        (int (*)(void *, unsigned int))kb_module_lookup_exported_symbol("usb_submit_urb");
    int result = real_submit_urb != NULL ? real_submit_urb(urb, mem_flags) : -19;
    kb_usb_subsystem_urb_submit(NULL, urb, mem_flags, result);
    return result;
}

int kb_usb_unlink_urb(void *urb)
{
    int (*real_unlink_urb)(void *) = (int (*)(void *))kb_module_lookup_exported_symbol("usb_unlink_urb");
    int result = real_unlink_urb != NULL ? real_unlink_urb(urb) : -19;
    kb_usb_subsystem_urb_unlink(NULL, urb, result);
    return result;
}

void kb_usb_kill_urb(void *urb)
{
    void (*real_kill_urb)(void *) = (void (*)(void *))kb_module_lookup_exported_symbol("usb_kill_urb");
    if (real_kill_urb != NULL) {
        real_kill_urb(urb);
    }
    kb_usb_subsystem_urb_kill(NULL, urb);
}

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
    vfprintf(stderr, fmt, args);
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
    vfprintf(stderr, fmt, args);
    va_end(args);
}
