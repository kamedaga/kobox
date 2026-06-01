#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    KB_USB_HUB_STATUS_MAX = 8,
    KB_USB_DEVPATH_MAX = 32,
};

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
    uint32_t irq_count;
    uint32_t died_count;
    uint32_t lost_power_count;
    uint32_t root_hub_poll_count;
    uint32_t root_hub_resume_count;
    uint32_t port_resume_start_count;
    uint32_t port_resume_end_count;
    uint32_t wakeup_notification_count;
    uint32_t remote_wakeup_quirk_count;
} kb_usb_hcd_record_t;

typedef struct kb_usb_hub_event_update {
    unsigned char status[KB_USB_HUB_STATUS_MAX];
    int status_len;
    unsigned long bits;
    unsigned long new_bits;
    unsigned long injected_before;
    unsigned long injected_after;
    long slot;
} kb_usb_hub_event_update_t;

typedef struct kb_usb_hcd_snapshot {
    void *hcd;
    void *owner;
    const void *driver;
    void *primary_hcd;
    void *regs;
    unsigned int irq;
    uint32_t primary;
    uint32_t irq_registered;
    uint32_t added;
    uint32_t irq_count;
    uint32_t died_count;
    uint32_t lost_power_count;
    uint32_t root_hub_poll_count;
    uint32_t root_hub_resume_count;
    uint32_t port_resume_start_count;
    uint32_t port_resume_end_count;
    uint32_t wakeup_notification_count;
    uint32_t remote_wakeup_quirk_count;
} kb_usb_hcd_snapshot_t;

typedef struct kb_usb_urb_snapshot {
    void *hcd;
    void *urb;
    void *transfer_buffer;
    void *setup_packet;
    uint64_t transfer_dma;
    uint64_t setup_dma;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    uint32_t transfer_flags;
    uint32_t dma_map_flags;
    int status;
    uint32_t linked;
    uint32_t mapped;
    uint32_t link_count;
    uint32_t unlink_count;
    uint32_t map_count;
    uint32_t unmap_count;
    uint32_t submit_count;
    uint32_t kill_count;
    uint32_t giveback_count;
    int last_unlink_status;
    int last_submit_status;
    int last_giveback_status;
    unsigned int last_mem_flags;
    unsigned int last_submit_mem_flags;
} kb_usb_urb_snapshot_t;

typedef struct kb_usb_urb_dma_update {
    void *transfer_buffer;
    void *setup_packet;
    uint64_t transfer_dma;
    uint64_t setup_dma;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    uint32_t transfer_flags;
    uint32_t dma_map_flags;
    unsigned int mem_flags;
} kb_usb_urb_dma_update_t;

typedef struct kb_usb_device_update {
    void *hcd;
    void *udev;
    void *linux_device;
    void *parent_linux_device;
    void *bus;
    void *active_config;
    uint32_t devnum;
    uint32_t portnum;
    uint32_t speed;
    uint32_t state;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint8_t configuration_value;
    uint8_t interface_count;
    char devpath[KB_USB_DEVPATH_MAX];
} kb_usb_device_update_t;

typedef struct kb_usb_device_snapshot {
    void *hcd;
    void *udev;
    void *linux_device;
    void *parent_linux_device;
    void *bus;
    void *active_config;
    uint32_t devnum;
    uint32_t portnum;
    uint32_t speed;
    uint32_t state;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint8_t configuration_value;
    uint8_t interface_count;
    char devpath[KB_USB_DEVPATH_MAX];
} kb_usb_device_snapshot_t;

typedef struct kb_usb_interface_update {
    void *udev;
    void *interface;
    void *linux_device;
    void *parent_linux_device;
    void *driver;
    void *driver_data;
    void *cur_altsetting;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
} kb_usb_interface_update_t;

typedef struct kb_usb_interface_snapshot {
    void *udev;
    void *interface;
    void *linux_device;
    void *parent_linux_device;
    void *driver;
    void *driver_data;
    void *cur_altsetting;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
} kb_usb_interface_snapshot_t;

typedef struct kb_usb_endpoint_update {
    void *udev;
    void *interface;
    void *endpoint;
    uint8_t address;
    uint8_t attributes;
    uint8_t interval;
    uint8_t type;
    uint8_t direction_in;
    uint16_t max_packet_size;
} kb_usb_endpoint_update_t;

typedef struct kb_usb_endpoint_snapshot {
    void *udev;
    void *interface;
    void *endpoint;
    uint8_t address;
    uint8_t attributes;
    uint8_t interval;
    uint8_t type;
    uint8_t direction_in;
    uint16_t max_packet_size;
} kb_usb_endpoint_snapshot_t;

typedef struct kb_usb_driver_snapshot {
    void *driver;
    void *owner;
    int registered;
    int last_register_status;
    uint32_t register_count;
    uint32_t deregister_count;
    char module_name[64];
} kb_usb_driver_snapshot_t;

kb_usb_hcd_record_t *kb_usb_subsystem_hcd_alloc(size_t storage_size);
kb_usb_hcd_record_t *kb_usb_subsystem_hcd_for_hcd(void *hcd);
kb_usb_hcd_record_t *kb_usb_subsystem_primary_hcd_for_owner(void *owner);
void kb_usb_subsystem_hcd_release(kb_usb_hcd_record_t *record);
int kb_usb_subsystem_for_each_hcd(int (*callback)(kb_usb_hcd_record_t *record, void *ctx), void *ctx);
void kb_usb_subsystem_hcd_note_irq(void *hcd);
void kb_usb_subsystem_hcd_note_died(void *hcd);
void kb_usb_subsystem_hcd_note_lost_power(void *hcd);
void kb_usb_subsystem_hcd_note_root_hub_poll(void *hcd);
void kb_usb_subsystem_hcd_note_root_hub_resume(void *hcd);
void kb_usb_subsystem_hcd_note_port_resume_start(void *hcd);
void kb_usb_subsystem_hcd_note_port_resume_end(void *hcd);
void kb_usb_subsystem_hcd_note_wakeup_notification(void *hcd);
void kb_usb_subsystem_hcd_note_remote_wakeup_quirk(void *hcd);
int kb_usb_subsystem_hcd_snapshot(const void *hcd, kb_usb_hcd_snapshot_t *out_snapshot);
int kb_usb_subsystem_hub_event_prepare(
    void *hcd,
    const unsigned char *status,
    size_t status_len,
    kb_usb_hub_event_update_t *update);
int kb_usb_subsystem_hub_event_commit(
    void *hcd,
    unsigned long bits,
    kb_usb_hub_event_update_t *update);
int kb_usb_subsystem_urb_link(void *hcd, void *urb);
void kb_usb_subsystem_urb_unlink(void *hcd, void *urb, int status);
int kb_usb_subsystem_urb_check_unlink(void *hcd, void *urb, int status);
int kb_usb_subsystem_urb_map_dma(void *hcd, void *urb, const kb_usb_urb_dma_update_t *update);
void kb_usb_subsystem_urb_unmap_dma(void *hcd, void *urb);
void kb_usb_subsystem_urb_submit(void *hcd, void *urb, unsigned int mem_flags, int status);
void kb_usb_subsystem_urb_kill(void *hcd, void *urb);
void kb_usb_subsystem_urb_giveback(void *hcd, void *urb, int status);
int kb_usb_subsystem_urb_snapshot(const void *urb, kb_usb_urb_snapshot_t *out_snapshot);
int kb_usb_subsystem_device_observe(const kb_usb_device_update_t *update);
void kb_usb_subsystem_device_remove(void *udev);
int kb_usb_subsystem_device_snapshot(const void *udev, kb_usb_device_snapshot_t *out_snapshot);
size_t kb_usb_subsystem_device_count(void);
int kb_usb_subsystem_for_each_device(
    int (*callback)(const kb_usb_device_snapshot_t *snapshot, void *ctx),
    void *ctx);
int kb_usb_subsystem_interface_observe(const kb_usb_interface_update_t *update);
void kb_usb_subsystem_interface_remove(void *interface);
int kb_usb_subsystem_interface_snapshot(const void *interface, kb_usb_interface_snapshot_t *out_snapshot);
size_t kb_usb_subsystem_interface_count(void);
int kb_usb_subsystem_for_each_interface(
    int (*callback)(const kb_usb_interface_snapshot_t *snapshot, void *ctx),
    void *ctx);
int kb_usb_subsystem_endpoint_observe(const kb_usb_endpoint_update_t *update);
void kb_usb_subsystem_endpoint_remove(void *endpoint);
int kb_usb_subsystem_endpoint_snapshot(const void *endpoint, kb_usb_endpoint_snapshot_t *out_snapshot);
size_t kb_usb_subsystem_endpoint_count(void);
int kb_usb_subsystem_for_each_endpoint(
    int (*callback)(const kb_usb_endpoint_snapshot_t *snapshot, void *ctx),
    void *ctx);
void kb_usb_subsystem_driver_register(void *driver, void *owner, const char *module_name, int status);
void kb_usb_subsystem_driver_deregister(void *driver);
int kb_usb_subsystem_driver_snapshot(const void *driver, kb_usb_driver_snapshot_t *out_snapshot);
size_t kb_usb_subsystem_driver_count(void);
int kb_usb_subsystem_for_each_driver(
    int (*callback)(const kb_usb_driver_snapshot_t *snapshot, void *ctx),
    void *ctx);
