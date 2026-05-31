#include "kobox/shim.h"
#include "subsystem/usb.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_USB_HCD_STORAGE_SIZE = 4096,
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
    KB_LINUX_6_8_USB_HOST_CONFIG_INTERFACE0_OFFSET = 0x98,
    KB_LINUX_6_8_USB_INTERFACE_DRIVER_DATA_OFFSET = 0x0c8,
    KB_LINUX_6_8_USB_HUB_HDEV_OFFSET = 0x008,
    KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET = 0x060,
    KB_LINUX_6_8_USB_HUB_EVENTS_WORK_OFFSET = 0x218,
    KB_USB_REQ_GET_PORT_STATUS = 0xa300,
};

static unsigned char usb_hcd_pci_pm_ops[256];
static unsigned char usb_xhci_tracepoint[128];
static unsigned int usb_num_online_cpus = 1;
static unsigned char usb_pcpu_hot[256];
static int usb_pm_suspend_target_state;

static int trace_usb_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_USB");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int usb_event_injection_enabled(void)
{
    const char *value = getenv("KOBOX_ENABLE_USB_EVENT_INJECT");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
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

static int usb_root_hub_can_run_events(void *hcd)
{
    unsigned char state = 0;
    if (hcd == NULL) {
        return 0;
    }
    memcpy(&state, (const unsigned char *)hcd + KB_LINUX_6_8_USB_HCD_RH_STATE_OFFSET, sizeof(state));
    return (state & 1u) != 0;
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

static void *usb_hub_for_root_hub(void *root_hub)
{
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
    if (status_len <= 0) {
        return;
    }

    void *root_hub = usb_root_hub_for_hcd(hcd);
    if (root_hub == NULL) {
        return;
    }

    void *hub = usb_hub_for_root_hub(root_hub);
    int hub_ready = usb_root_hub_can_run_events(hcd) && usb_hub_ready_for_events(hub);
    unsigned long bits = 0;
    for (int i = 0; i < status_len && i < (int)sizeof(bits); i++) {
        bits |= ((unsigned long)status[i]) << (i * 8);
    }

    kb_usb_hub_event_injection_t *injection = kb_usb_subsystem_hub_event_injection_for_hcd(hcd);
    unsigned long new_bits = bits;
    unsigned long injected_before = injection != NULL ? injection->bits : 0;
    if (injection != NULL) {
        if (bits == 0) {
            injection->bits = 0;
        }
        new_bits = bits & ~injection->bits;
    }
    if (inject_events && hub != NULL && hub_ready && bits != 0) {
        unsigned long existing_bits = 0;
        memcpy(
            &existing_bits,
            (const unsigned char *)hub + KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET,
            sizeof(existing_bits));
        if (new_bits != 0) {
            existing_bits |= new_bits;
            memcpy(
                (unsigned char *)hub + KB_LINUX_6_8_USB_HUB_EVENT_BITS_OFFSET,
                &existing_bits,
                sizeof(existing_bits));
            if (injection != NULL) {
                injection->bits |= new_bits;
            }
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
            "kobox usb: kick_root_hub hcd=%p root_hub=%p hub=%p injection=%p slot=%ld injected=0x%lx->0x%lx hub_event_bits=0x%lx bits=0x%lx new_bits=0x%lx status_len=%d status=%02x %02x %02x %02x\n",
            hcd,
            root_hub,
            hub,
            (void *)injection,
            kb_usb_subsystem_hub_event_injection_index(injection),
            injected_before,
            injection != NULL ? injection->bits : 0,
            hub_event_bits,
            bits,
            new_bits,
            status_len,
            status[0],
            status[1],
            status[2],
            status[3]);
    }
    if (inject_events && hub != NULL && hub_ready && new_bits != 0) {
        kick_hub_wq(root_hub);
        (void)kb_queue_work_on(0, NULL, (unsigned char *)hub + KB_LINUX_6_8_USB_HUB_EVENTS_WORK_OFFSET);
    }
}

int kb_usb_hcd_irq(int irq, void *hcd)
{
    (void)irq;
    (void)hcd;
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
    trace_root_hub_state("poll_root_hub", hcd);
    if (resume_root_hub != NULL) {
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
        trace_root_hub_state("poll_shared_root_hub", shared_hcd);
        if (resume_root_hub != NULL) {
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
    (void)hcd;
    (void)udev;
    (void)tt;
    (void)devnum;
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
