#include "linux_subsystem/block/block.h"
#include "linux_subsystem/usb/usb.h"
#include "linux_subsystem/usb/storage.h"

#include <stdio.h>
#include <string.h>

static int count_device(const kb_usb_device_snapshot_t *snapshot, void *ctx)
{
    int *count = ctx;
    if (snapshot != 0 && count != 0) {
        *count += 1;
    }
    return 0;
}

static int count_interface(const kb_usb_interface_snapshot_t *snapshot, void *ctx)
{
    int *count = ctx;
    if (snapshot != 0 && count != 0) {
        *count += 1;
    }
    return 0;
}

static int count_endpoint(const kb_usb_endpoint_snapshot_t *snapshot, void *ctx)
{
    int *count = ctx;
    if (snapshot != 0 && count != 0) {
        *count += 1;
    }
    return 0;
}

static int count_driver(const kb_usb_driver_snapshot_t *snapshot, void *ctx)
{
    int *count = ctx;
    if (snapshot != 0 && count != 0) {
        *count += 1;
    }
    return 0;
}

static int count_storage(const kb_usb_storage_snapshot_t *snapshot, void *ctx)
{
    int *count = ctx;
    if (snapshot != 0 && count != 0) {
        *count += 1;
    }
    return 0;
}

static int expect_event(
    kb_usb_hub_event_update_t *event,
    unsigned long bits,
    unsigned long new_bits,
    unsigned long injected_before,
    unsigned long injected_after)
{
    if (event->bits != bits ||
        event->new_bits != new_bits ||
        event->injected_before != injected_before ||
        event->injected_after != injected_after)
    {
        return 1;
    }
    return event->slot < 0 ? 2 : 0;
}

static void write_le32(unsigned char *value, unsigned int data)
{
    value[0] = (unsigned char)data;
    value[1] = (unsigned char)(data >> 8);
    value[2] = (unsigned char)(data >> 16);
    value[3] = (unsigned char)(data >> 24);
}

static unsigned int read_le32(const unsigned char *value)
{
    return (unsigned int)value[0] |
           ((unsigned int)value[1] << 8) |
           ((unsigned int)value[2] << 16) |
           ((unsigned int)value[3] << 24);
}

static void build_cbw(
    unsigned char *cbw,
    unsigned int tag,
    unsigned int transfer_len,
    int data_in,
    const unsigned char *cdb,
    size_t cdb_len)
{
    memset(cbw, 0, KB_USB_STORAGE_BOT_CBW_SIZE);
    write_le32(cbw, 0x43425355u);
    write_le32(cbw + 4, tag);
    write_le32(cbw + 8, transfer_len);
    cbw[12] = data_in ? 0x80 : 0;
    cbw[14] = (unsigned char)cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
}

static int buffer_is_zero(const unsigned char *buffer, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buffer[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    int fake_hcd_storage;
    void *hcd = &fake_hcd_storage;
    unsigned char port1_changed[1] = { 0x02 };
    unsigned char no_change[1] = { 0x00 };
    kb_usb_hub_event_update_t event;

    if (kb_usb_subsystem_hub_event_prepare(hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 1;
    }
    if (event.status_len != 1 || event.status[0] != port1_changed[0]) {
        return 2;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x00) != 0) {
        return 3;
    }

    if (kb_usb_subsystem_hub_event_commit(hcd, event.new_bits, &event) != 0) {
        return 4;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x02) != 0) {
        return 5;
    }

    if (kb_usb_subsystem_hub_event_prepare(hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 6;
    }
    if (expect_event(&event, 0x02, 0x00, 0x02, 0x02) != 0) {
        return 7;
    }

    if (kb_usb_subsystem_hub_event_prepare(hcd, no_change, sizeof(no_change), &event) != 0) {
        return 8;
    }
    if (expect_event(&event, 0x00, 0x00, 0x02, 0x00) != 0) {
        return 9;
    }

    if (kb_usb_subsystem_hub_event_prepare(hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 10;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x00) != 0) {
        return 11;
    }

    kb_usb_hcd_record_t *record = kb_usb_subsystem_hcd_alloc(64);
    if (record == 0 || record->hcd == 0) {
        return 12;
    }
    void *allocated_hcd = record->hcd;

    if (kb_usb_subsystem_hub_event_prepare(allocated_hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 13;
    }
    if (kb_usb_subsystem_hub_event_commit(allocated_hcd, event.new_bits, &event) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 14;
    }

    kb_usb_subsystem_hcd_note_irq(allocated_hcd);
    kb_usb_subsystem_hcd_note_died(allocated_hcd);
    kb_usb_subsystem_hcd_note_lost_power(allocated_hcd);
    kb_usb_subsystem_hcd_note_root_hub_poll(allocated_hcd);
    kb_usb_subsystem_hcd_note_root_hub_resume(allocated_hcd);
    kb_usb_subsystem_hcd_note_port_resume_start(allocated_hcd);
    kb_usb_subsystem_hcd_note_port_resume_end(allocated_hcd);
    kb_usb_subsystem_hcd_note_wakeup_notification(allocated_hcd);
    kb_usb_subsystem_hcd_note_remote_wakeup_quirk(allocated_hcd);

    kb_usb_hcd_snapshot_t hcd_snapshot;
    if (kb_usb_subsystem_hcd_snapshot(allocated_hcd, &hcd_snapshot) != 0 ||
        hcd_snapshot.irq_count != 1 ||
        hcd_snapshot.died_count != 1 ||
        hcd_snapshot.lost_power_count != 1 ||
        hcd_snapshot.root_hub_poll_count != 1 ||
        hcd_snapshot.root_hub_resume_count != 1 ||
        hcd_snapshot.port_resume_start_count != 1 ||
        hcd_snapshot.port_resume_end_count != 1 ||
        hcd_snapshot.wakeup_notification_count != 1 ||
        hcd_snapshot.remote_wakeup_quirk_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 15;
    }

    int fake_udev_storage;
    int fake_linux_device_storage;
    int fake_config_storage;
    kb_usb_device_update_t device_update = {
        .hcd = allocated_hcd,
        .udev = &fake_udev_storage,
        .linux_device = &fake_linux_device_storage,
        .active_config = &fake_config_storage,
        .devnum = 2,
        .portnum = 1,
        .speed = 3,
        .state = 7,
        .vendor_id = 0x0627,
        .product_id = 0x0001,
        .bcd_device = 0x0111,
        .device_class = 0,
        .configuration_value = 1,
        .interface_count = 1,
    };
    (void)snprintf(device_update.devpath, sizeof(device_update.devpath), "%s", "1");
    if (kb_usb_subsystem_device_observe(&device_update) != 0 || kb_usb_subsystem_device_count() != 1) {
        kb_usb_subsystem_hcd_release(record);
        return 16;
    }

    int fake_interface_storage;
    int fake_driver_data_storage;
    int fake_altsetting_storage;
    kb_usb_interface_update_t interface_update = {
        .udev = &fake_udev_storage,
        .interface = &fake_interface_storage,
        .linux_device = &fake_linux_device_storage,
        .driver_data = &fake_driver_data_storage,
        .cur_altsetting = &fake_altsetting_storage,
        .interface_number = 0,
        .alternate_setting = 0,
        .endpoint_count = 1,
        .interface_class = 3,
        .interface_subclass = 1,
        .interface_protocol = 1,
    };
    if (kb_usb_subsystem_interface_observe(&interface_update) != 0 ||
        kb_usb_subsystem_interface_count() != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 17;
    }

    int fake_endpoint_storage;
    kb_usb_endpoint_update_t endpoint_update = {
        .udev = &fake_udev_storage,
        .interface = &fake_interface_storage,
        .endpoint = &fake_endpoint_storage,
        .address = 0x81,
        .attributes = 0x03,
        .interval = 10,
        .type = 3,
        .direction_in = 1,
        .max_packet_size = 8,
    };
    if (kb_usb_subsystem_endpoint_observe(&endpoint_update) != 0 ||
        kb_usb_subsystem_endpoint_count() != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 18;
    }

    int fake_driver_storage;
    kb_usb_subsystem_driver_register(&fake_driver_storage, 0, "usbhid", 0);
    if (kb_usb_subsystem_driver_count() != 1) {
        kb_usb_subsystem_hcd_release(record);
        return 19;
    }

    kb_usb_device_snapshot_t device_snapshot;
    kb_usb_interface_snapshot_t interface_snapshot;
    kb_usb_endpoint_snapshot_t endpoint_snapshot;
    kb_usb_driver_snapshot_t driver_snapshot;
    int iter_count = 0;
    if (kb_usb_subsystem_device_snapshot(&fake_udev_storage, &device_snapshot) != 0 ||
        device_snapshot.hcd != allocated_hcd ||
        device_snapshot.udev != &fake_udev_storage ||
        device_snapshot.devnum != 2 ||
        device_snapshot.vendor_id != 0x0627 ||
        strcmp(device_snapshot.devpath, "1") != 0 ||
        kb_usb_subsystem_for_each_device(count_device, &iter_count) != 1 ||
        iter_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 20;
    }

    iter_count = 0;
    if (kb_usb_subsystem_interface_snapshot(&fake_interface_storage, &interface_snapshot) != 0 ||
        interface_snapshot.udev != &fake_udev_storage ||
        interface_snapshot.interface_class != 3 ||
        interface_snapshot.endpoint_count != 1 ||
        kb_usb_subsystem_for_each_interface(count_interface, &iter_count) != 1 ||
        iter_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 21;
    }

    iter_count = 0;
    if (kb_usb_subsystem_endpoint_snapshot(&fake_endpoint_storage, &endpoint_snapshot) != 0 ||
        endpoint_snapshot.interface != &fake_interface_storage ||
        endpoint_snapshot.address != 0x81 ||
        endpoint_snapshot.direction_in != 1 ||
        endpoint_snapshot.max_packet_size != 8 ||
        kb_usb_subsystem_for_each_endpoint(count_endpoint, &iter_count) != 1 ||
        iter_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 22;
    }

    iter_count = 0;
    if (kb_usb_subsystem_driver_snapshot(&fake_driver_storage, &driver_snapshot) != 0 ||
        driver_snapshot.driver != &fake_driver_storage ||
        driver_snapshot.registered == 0 ||
        driver_snapshot.register_count != 1 ||
        strcmp(driver_snapshot.module_name, "usbhid") != 0 ||
        kb_usb_subsystem_for_each_driver(count_driver, &iter_count) != 1 ||
        iter_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 23;
    }

    kb_usb_subsystem_driver_deregister(&fake_driver_storage);
    if (kb_usb_subsystem_driver_snapshot(&fake_driver_storage, &driver_snapshot) != 0 ||
        driver_snapshot.registered != 0 ||
        driver_snapshot.deregister_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 24;
    }

    int fake_storage_udev;
    int fake_storage_linux_device;
    int fake_storage_config;
    kb_usb_device_update_t storage_device_update = {
        .hcd = allocated_hcd,
        .udev = &fake_storage_udev,
        .linux_device = &fake_storage_linux_device,
        .active_config = &fake_storage_config,
        .devnum = 3,
        .portnum = 2,
        .speed = 3,
        .state = 7,
        .vendor_id = 0x46f4,
        .product_id = 0x0001,
        .bcd_device = 0x0001,
        .device_class = 0,
        .configuration_value = 1,
        .interface_count = 1,
    };
    (void)snprintf(storage_device_update.devpath, sizeof(storage_device_update.devpath), "%s", "2");
    if (kb_usb_subsystem_device_observe(&storage_device_update) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 31;
    }

    int fake_storage_interface;
    int fake_storage_driver_data;
    int fake_storage_altsetting;
    kb_usb_interface_update_t storage_interface_update = {
        .udev = &fake_storage_udev,
        .interface = &fake_storage_interface,
        .linux_device = &fake_storage_linux_device,
        .driver_data = &fake_storage_driver_data,
        .cur_altsetting = &fake_storage_altsetting,
        .interface_number = 0,
        .alternate_setting = 0,
        .endpoint_count = 2,
        .interface_class = 8,
        .interface_subclass = 6,
        .interface_protocol = 0x50,
    };
    if (kb_usb_subsystem_interface_observe(&storage_interface_update) != 0 ||
        kb_usb_storage_subsystem_count() != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 32;
    }

    int fake_storage_bulk_in;
    int fake_storage_bulk_out;
    kb_usb_endpoint_update_t storage_bulk_in_update = {
        .udev = &fake_storage_udev,
        .interface = &fake_storage_interface,
        .endpoint = &fake_storage_bulk_in,
        .address = 0x81,
        .attributes = 0x02,
        .interval = 0,
        .type = 2,
        .direction_in = 1,
        .max_packet_size = 512,
    };
    kb_usb_endpoint_update_t storage_bulk_out_update = {
        .udev = &fake_storage_udev,
        .interface = &fake_storage_interface,
        .endpoint = &fake_storage_bulk_out,
        .address = 0x02,
        .attributes = 0x02,
        .interval = 0,
        .type = 2,
        .direction_in = 0,
        .max_packet_size = 512,
    };
    if (kb_usb_subsystem_endpoint_observe(&storage_bulk_in_update) != 0 ||
        kb_usb_subsystem_endpoint_observe(&storage_bulk_out_update) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 33;
    }

    int fake_usb_storage_driver;
    kb_usb_subsystem_driver_register(&fake_usb_storage_driver, 0, "usb-storage", 0);
    kb_usb_storage_snapshot_t storage_snapshot;
    iter_count = 0;
    if (kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.udev != &fake_storage_udev ||
        storage_snapshot.interface != &fake_storage_interface ||
        storage_snapshot.driver != &fake_usb_storage_driver ||
        storage_snapshot.driver_data != &fake_storage_driver_data ||
        storage_snapshot.disk == 0 ||
        storage_snapshot.queue == 0 ||
        storage_snapshot.part0 == 0 ||
        storage_snapshot.tag_set == 0 ||
        storage_snapshot.registered != 1 ||
        storage_snapshot.capacity_sectors != 131072 ||
        storage_snapshot.sector_size != 512 ||
        storage_snapshot.subclass != 6 ||
        storage_snapshot.protocol != 0x50 ||
        storage_snapshot.bulk_in_endpoint != 0x81 ||
        storage_snapshot.bulk_out_endpoint != 0x02 ||
        kb_usb_storage_subsystem_for_each(count_storage, &iter_count) != 1 ||
        iter_count != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 34;
    }

    int fake_scsi_host;
    kb_usb_storage_subsystem_note_scsi_host(
        &fake_scsi_host,
        &fake_storage_driver_data,
        &fake_storage_linux_device,
        &fake_storage_linux_device);
    kb_usb_storage_subsystem_note_scsi_scan(&fake_scsi_host);
    if (kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.scsi_host != &fake_scsi_host ||
        storage_snapshot.scsi_host_private != &fake_storage_driver_data ||
        storage_snapshot.add_host_count != 1 ||
        storage_snapshot.scan_count != 1 ||
        storage_snapshot.registered != 1)
    {
        kb_usb_subsystem_hcd_release(record);
        return 35;
    }

    unsigned char storage_write_buffer[1024];
    unsigned char storage_read_buffer[sizeof(storage_write_buffer)];
    for (size_t i = 0; i < sizeof(storage_write_buffer); i++) {
        storage_write_buffer[i] = (unsigned char)((i * 7u) ^ 0x5au);
        storage_read_buffer[i] = 0;
    }
    if (kb_block_subsystem_disk_write(storage_snapshot.disk, 8, storage_write_buffer, sizeof(storage_write_buffer)) != 0 ||
        kb_block_subsystem_disk_read(storage_snapshot.disk, 8, storage_read_buffer, sizeof(storage_read_buffer)) != 0 ||
        memcmp(storage_write_buffer, storage_read_buffer, sizeof(storage_write_buffer)) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 36;
    }
    if (kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.read_count != 1 ||
        storage_snapshot.write_count != 1 ||
        storage_snapshot.bytes_read != sizeof(storage_read_buffer) ||
        storage_snapshot.bytes_written != sizeof(storage_write_buffer) ||
        storage_snapshot.backing_bytes < sizeof(storage_write_buffer))
    {
        kb_usb_subsystem_hcd_release(record);
        return 37;
    }

    kb_usb_storage_scsi_result_t scsi_result;
    unsigned char inquiry[36];
    unsigned char inquiry_cdb[6] = { 0x12, 0, 0, 0, sizeof(inquiry), 0 };
    memset(inquiry, 0, sizeof(inquiry));
    if (kb_usb_storage_subsystem_scsi_command(
            &fake_storage_interface,
            inquiry_cdb,
            sizeof(inquiry_cdb),
            inquiry,
            sizeof(inquiry),
            0,
            &scsi_result) != 0 ||
        scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
        scsi_result.data_transferred != sizeof(inquiry) ||
        memcmp(inquiry + 8, "KOBOX   ", 8) != 0 ||
        memcmp(inquiry + 16, "USB STORAGE     ", 16) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 38;
    }

    unsigned char capacity[8];
    unsigned char capacity_cdb[10] = { 0x25 };
    memset(capacity, 0, sizeof(capacity));
    if (kb_usb_storage_subsystem_scsi_command(
            &fake_storage_interface,
            capacity_cdb,
            sizeof(capacity_cdb),
            capacity,
            sizeof(capacity),
            0,
            &scsi_result) != 0 ||
        scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
        memcmp(capacity, "\x00\x01\xff\xff\x00\x00\x02\x00", sizeof(capacity)) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 39;
    }

    unsigned char scsi_write_buffer[1024];
    unsigned char scsi_read_buffer[sizeof(scsi_write_buffer)];
    for (size_t i = 0; i < sizeof(scsi_write_buffer); i++) {
        scsi_write_buffer[i] = (unsigned char)((i * 11u) ^ 0xc3u);
        scsi_read_buffer[i] = 0;
    }
    unsigned char write10_cdb[10] = { 0x2a, 0, 0, 0, 0, 12, 0, 0, 2, 0 };
    unsigned char read10_cdb[10] = { 0x28, 0, 0, 0, 0, 12, 0, 0, 2, 0 };
    if (kb_usb_storage_subsystem_scsi_command(
            &fake_storage_interface,
            write10_cdb,
            sizeof(write10_cdb),
            scsi_write_buffer,
            sizeof(scsi_write_buffer),
            1,
            &scsi_result) != 0 ||
        scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
        scsi_result.data_transferred != sizeof(scsi_write_buffer) ||
        kb_usb_storage_subsystem_scsi_command(
            &fake_storage_interface,
            read10_cdb,
            sizeof(read10_cdb),
            scsi_read_buffer,
            sizeof(scsi_read_buffer),
            0,
            &scsi_result) != 0 ||
        scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
        scsi_result.data_transferred != sizeof(scsi_read_buffer) ||
        memcmp(scsi_write_buffer, scsi_read_buffer, sizeof(scsi_write_buffer)) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 40;
    }

    unsigned char invalid_cdb[6] = { 0xff };
    if (kb_usb_storage_subsystem_scsi_command(
            &fake_storage_interface,
            invalid_cdb,
            sizeof(invalid_cdb),
            0,
            0,
            0,
            &scsi_result) != 0 ||
        scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_CHECK_CONDITION ||
        scsi_result.sense_key != 0x05 ||
        scsi_result.asc != 0x20)
    {
        kb_usb_subsystem_hcd_release(record);
        return 41;
    }

    unsigned char sense[18];
    unsigned char request_sense_cdb[6] = { 0x03, 0, 0, 0, sizeof(sense), 0 };
    memset(sense, 0, sizeof(sense));
    if (kb_usb_storage_subsystem_scsi_command(
            &fake_storage_interface,
            request_sense_cdb,
            sizeof(request_sense_cdb),
            sense,
            sizeof(sense),
            0,
            &scsi_result) != 0 ||
        scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
        sense[2] != 0x05 ||
        sense[12] != 0x20)
    {
        kb_usb_subsystem_hcd_release(record);
        return 42;
    }

    if (kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.scsi_command_count != 6 ||
        storage_snapshot.scsi_read_command_count != 1 ||
        storage_snapshot.scsi_write_command_count != 1 ||
        storage_snapshot.read_count != 2 ||
        storage_snapshot.write_count != 2 ||
        storage_snapshot.last_scsi_opcode != 0x03 ||
        storage_snapshot.last_sense_key != 0x05 ||
        storage_snapshot.last_asc != 0x20)
    {
        kb_usb_subsystem_hcd_release(record);
        return 43;
    }

    unsigned char cbw[KB_USB_STORAGE_BOT_CBW_SIZE];
    unsigned char bot_inquiry[36];
    unsigned char bot_capacity[8];
    unsigned char bot_write_buffer[512];
    unsigned char bot_read_buffer[sizeof(bot_write_buffer)];
    kb_usb_storage_bot_result_t bot_result;
    memset(bot_inquiry, 0, sizeof(bot_inquiry));
    memset(bot_capacity, 0, sizeof(bot_capacity));
    for (size_t i = 0; i < sizeof(bot_write_buffer); i++) {
        bot_write_buffer[i] = (unsigned char)((i * 5u) ^ 0x39u);
        bot_read_buffer[i] = 0;
    }

    build_cbw(cbw, 0x101, sizeof(bot_inquiry), 1, inquiry_cdb, sizeof(inquiry_cdb));
    if (kb_usb_storage_subsystem_bot_transfer(
            &fake_storage_interface,
            cbw,
            sizeof(cbw),
            bot_inquiry,
            sizeof(bot_inquiry),
            &bot_result) != 0 ||
        bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
        bot_result.tag != 0x101 ||
        bot_result.residue != 0 ||
        bot_result.data_transferred != sizeof(bot_inquiry) ||
        read_le32(bot_result.csw) != 0x53425355u ||
        read_le32(bot_result.csw + 4) != 0x101 ||
        bot_result.csw[12] != KB_USB_STORAGE_BOT_STATUS_PASSED ||
        memcmp(bot_inquiry + 8, "KOBOX   ", 8) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 44;
    }

    build_cbw(cbw, 0x102, sizeof(bot_capacity), 1, capacity_cdb, sizeof(capacity_cdb));
    if (kb_usb_storage_subsystem_bot_transfer(
            &fake_storage_interface,
            cbw,
            sizeof(cbw),
            bot_capacity,
            sizeof(bot_capacity),
            &bot_result) != 0 ||
        bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
        memcmp(bot_capacity, "\x00\x01\xff\xff\x00\x00\x02\x00", sizeof(bot_capacity)) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 45;
    }

    unsigned char bot_write10_cdb[10] = { 0x2a, 0, 0, 0, 0, 20, 0, 0, 1, 0 };
    unsigned char bot_read10_cdb[10] = { 0x28, 0, 0, 0, 0, 20, 0, 0, 1, 0 };
    build_cbw(cbw, 0x103, sizeof(bot_write_buffer), 0, bot_write10_cdb, sizeof(bot_write10_cdb));
    if (kb_usb_storage_subsystem_bot_transfer(
            &fake_storage_interface,
            cbw,
            sizeof(cbw),
            bot_write_buffer,
            sizeof(bot_write_buffer),
            &bot_result) != 0 ||
        bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
        bot_result.data_transferred != sizeof(bot_write_buffer))
    {
        kb_usb_subsystem_hcd_release(record);
        return 46;
    }

    build_cbw(cbw, 0x104, sizeof(bot_read_buffer), 1, bot_read10_cdb, sizeof(bot_read10_cdb));
    if (kb_usb_storage_subsystem_bot_transfer(
            &fake_storage_interface,
            cbw,
            sizeof(cbw),
            bot_read_buffer,
            sizeof(bot_read_buffer),
            &bot_result) != 0 ||
        bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
        bot_result.data_transferred != sizeof(bot_read_buffer) ||
        memcmp(bot_write_buffer, bot_read_buffer, sizeof(bot_write_buffer)) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 47;
    }

    build_cbw(cbw, 0x105, 0, 0, invalid_cdb, sizeof(invalid_cdb));
    if (kb_usb_storage_subsystem_bot_transfer(
            &fake_storage_interface,
            cbw,
            sizeof(cbw),
            0,
            0,
            &bot_result) != 0 ||
        bot_result.status != KB_USB_STORAGE_BOT_STATUS_FAILED ||
        bot_result.csw[12] != KB_USB_STORAGE_BOT_STATUS_FAILED)
    {
        kb_usb_subsystem_hcd_release(record);
        return 48;
    }

    build_cbw(cbw, 0x106, 1024, 1, bot_read10_cdb, sizeof(bot_read10_cdb));
    if (kb_usb_storage_subsystem_bot_transfer(
            &fake_storage_interface,
            cbw,
            sizeof(cbw),
            bot_read_buffer,
            sizeof(bot_read_buffer),
            &bot_result) != 0 ||
        bot_result.status != KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR ||
        bot_result.residue != 1024 ||
        bot_result.csw[12] != KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR)
    {
        kb_usb_subsystem_hcd_release(record);
        return 49;
    }

    if (kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.scsi_command_count != 11 ||
        storage_snapshot.scsi_read_command_count != 2 ||
        storage_snapshot.scsi_write_command_count != 2 ||
        storage_snapshot.bot_command_count != 6 ||
        storage_snapshot.bot_success_count != 4 ||
        storage_snapshot.bot_failed_count != 1 ||
        storage_snapshot.bot_phase_error_count != 1 ||
        storage_snapshot.last_bot_tag != 0x106 ||
        storage_snapshot.last_bot_residue != 1024 ||
        storage_snapshot.last_bot_status != KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR ||
        storage_snapshot.read_count != 3 ||
        storage_snapshot.write_count != 3)
    {
        kb_usb_subsystem_hcd_release(record);
        return 50;
    }
    kb_usb_storage_subsystem_bot_reset(&fake_storage_interface);
    if (kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.bot_reset_count != 1 ||
        storage_snapshot.last_bot_status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
        storage_snapshot.last_bot_residue != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 51;
    }

    void *first_storage_disk = storage_snapshot.disk;
    unsigned int first_storage_disk_number = storage_snapshot.disk_number;

    int fake_storage2_udev;
    int fake_storage2_linux_device;
    int fake_storage2_active_config;
    kb_usb_device_update_t storage2_device_update = {
        .hcd = allocated_hcd,
        .udev = &fake_storage2_udev,
        .linux_device = &fake_storage2_linux_device,
        .active_config = &fake_storage2_active_config,
        .devnum = 3,
        .portnum = 2,
        .speed = 3,
        .state = 7,
        .vendor_id = 0x46f4,
        .product_id = 0x0002,
        .bcd_device = 0x0001,
        .device_class = 0,
        .configuration_value = 1,
        .interface_count = 1,
    };
    (void)snprintf(storage2_device_update.devpath, sizeof(storage2_device_update.devpath), "%s", "3");
    if (kb_usb_subsystem_device_observe(&storage2_device_update) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 52;
    }

    int fake_storage2_interface;
    int fake_storage2_altsetting;
    kb_usb_interface_update_t storage2_interface_update = {
        .udev = &fake_storage2_udev,
        .interface = &fake_storage2_interface,
        .linux_device = &fake_storage2_linux_device,
        .driver = &fake_usb_storage_driver,
        .cur_altsetting = &fake_storage2_altsetting,
        .interface_number = 0,
        .alternate_setting = 0,
        .endpoint_count = 2,
        .interface_class = 8,
        .interface_subclass = 6,
        .interface_protocol = 0x50,
    };
    int fake_storage2_bulk_in;
    int fake_storage2_bulk_out;
    kb_usb_endpoint_update_t storage2_bulk_in_update = {
        .udev = &fake_storage2_udev,
        .interface = &fake_storage2_interface,
        .endpoint = &fake_storage2_bulk_in,
        .address = 0x83,
        .attributes = 0x02,
        .type = 2,
        .direction_in = 1,
        .max_packet_size = 512,
    };
    kb_usb_endpoint_update_t storage2_bulk_out_update = {
        .udev = &fake_storage2_udev,
        .interface = &fake_storage2_interface,
        .endpoint = &fake_storage2_bulk_out,
        .address = 0x04,
        .attributes = 0x02,
        .type = 2,
        .direction_in = 0,
        .max_packet_size = 512,
    };
    if (kb_usb_subsystem_interface_observe(&storage2_interface_update) != 0 ||
        kb_usb_subsystem_endpoint_observe(&storage2_bulk_in_update) != 0 ||
        kb_usb_subsystem_endpoint_observe(&storage2_bulk_out_update) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 53;
    }

    kb_usb_storage_snapshot_t storage2_snapshot;
    kb_usb_storage_snapshot_t by_disk_snapshot;
    iter_count = 0;
    if (kb_usb_storage_subsystem_count() != 2 ||
        kb_usb_storage_subsystem_for_each(count_storage, &iter_count) != 2 ||
        iter_count != 2 ||
        kb_usb_storage_subsystem_snapshot(&fake_storage2_interface, &storage2_snapshot) != 0 ||
        storage2_snapshot.udev != &fake_storage2_udev ||
        storage2_snapshot.interface != &fake_storage2_interface ||
        storage2_snapshot.linux_device != &fake_storage2_linux_device ||
        storage2_snapshot.driver != &fake_usb_storage_driver ||
        storage2_snapshot.driver_data != NULL ||
        storage2_snapshot.disk == 0 ||
        storage2_snapshot.disk == first_storage_disk ||
        storage2_snapshot.disk_number == first_storage_disk_number ||
        storage2_snapshot.bulk_in_endpoint != 0x83 ||
        storage2_snapshot.bulk_out_endpoint != 0x04 ||
        kb_usb_storage_subsystem_snapshot_by_disk(storage2_snapshot.disk, &by_disk_snapshot) != 0 ||
        by_disk_snapshot.interface != &fake_storage2_interface)
    {
        kb_usb_subsystem_hcd_release(record);
        return 54;
    }

    unsigned char storage1_multi_write[512];
    unsigned char storage1_multi_read[sizeof(storage1_multi_write)];
    unsigned char storage2_multi_write[512];
    unsigned char storage2_multi_read[sizeof(storage2_multi_write)];
    for (size_t i = 0; i < sizeof(storage1_multi_write); i++) {
        storage1_multi_write[i] = (unsigned char)((i * 3u) ^ 0xa5u);
        storage1_multi_read[i] = 0;
        storage2_multi_write[i] = (unsigned char)((i * 9u) ^ 0x4cu);
        storage2_multi_read[i] = 0;
    }
    if (kb_block_subsystem_disk_write(first_storage_disk, 30, storage1_multi_write, sizeof(storage1_multi_write)) != 0 ||
        kb_block_subsystem_disk_write(storage2_snapshot.disk, 30, storage2_multi_write, sizeof(storage2_multi_write)) != 0 ||
        kb_block_subsystem_disk_read(first_storage_disk, 30, storage1_multi_read, sizeof(storage1_multi_read)) != 0 ||
        kb_block_subsystem_disk_read(storage2_snapshot.disk, 30, storage2_multi_read, sizeof(storage2_multi_read)) != 0 ||
        memcmp(storage1_multi_write, storage1_multi_read, sizeof(storage1_multi_write)) != 0 ||
        memcmp(storage2_multi_write, storage2_multi_read, sizeof(storage2_multi_write)) != 0 ||
        memcmp(storage1_multi_read, storage2_multi_read, sizeof(storage1_multi_read)) == 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 55;
    }

    int fake_scsi_host2;
    int fake_storage2_host_private;
    kb_usb_storage_subsystem_note_scsi_host(
        &fake_scsi_host2,
        &fake_storage2_host_private,
        &fake_storage2_linux_device,
        &fake_storage2_linux_device);
    if (kb_usb_storage_subsystem_snapshot(&fake_storage2_interface, &storage2_snapshot) != 0 ||
        storage2_snapshot.scsi_host != &fake_scsi_host2 ||
        storage2_snapshot.scsi_host_private != &fake_storage2_host_private ||
        storage2_snapshot.driver_data != &fake_storage2_host_private ||
        kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) != 0 ||
        storage_snapshot.scsi_host != &fake_scsi_host ||
        storage_snapshot.disk != first_storage_disk)
    {
        kb_usb_subsystem_hcd_release(record);
        return 56;
    }

    void *old_storage2_disk = storage2_snapshot.disk;
    unsigned int old_storage2_disk_number = storage2_snapshot.disk_number;
    kb_usb_subsystem_interface_remove(&fake_storage2_interface);
    if (kb_usb_storage_subsystem_count() != 1 ||
        kb_usb_storage_subsystem_snapshot(&fake_storage2_interface, &storage2_snapshot) == 0 ||
        kb_block_subsystem_disk_read(old_storage2_disk, 30, storage2_multi_read, sizeof(storage2_multi_read)) == 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 57;
    }

    int fake_storage2_replug_udev;
    int fake_storage2_replug_linux_device;
    int fake_storage2_replug_active_config;
    kb_usb_device_update_t storage2_replug_device_update = {
        .hcd = allocated_hcd,
        .udev = &fake_storage2_replug_udev,
        .linux_device = &fake_storage2_replug_linux_device,
        .active_config = &fake_storage2_replug_active_config,
        .devnum = 4,
        .portnum = 2,
        .speed = 3,
        .state = 7,
        .vendor_id = 0x46f4,
        .product_id = 0x0002,
        .bcd_device = 0x0002,
        .device_class = 0,
        .configuration_value = 1,
        .interface_count = 1,
    };
    (void)snprintf(
        storage2_replug_device_update.devpath,
        sizeof(storage2_replug_device_update.devpath),
        "%s",
        "4");
    if (kb_usb_subsystem_device_observe(&storage2_replug_device_update) != 0) {
        kb_usb_subsystem_hcd_release(record);
        return 58;
    }

    int fake_storage2_replug_interface;
    int fake_storage2_replug_altsetting;
    kb_usb_interface_update_t storage2_replug_interface_update = {
        .udev = &fake_storage2_replug_udev,
        .interface = &fake_storage2_replug_interface,
        .linux_device = &fake_storage2_replug_linux_device,
        .driver = &fake_usb_storage_driver,
        .cur_altsetting = &fake_storage2_replug_altsetting,
        .interface_number = 0,
        .alternate_setting = 0,
        .endpoint_count = 2,
        .interface_class = 8,
        .interface_subclass = 6,
        .interface_protocol = 0x50,
    };
    int fake_storage2_replug_bulk_in;
    int fake_storage2_replug_bulk_out;
    kb_usb_endpoint_update_t storage2_replug_bulk_in_update = {
        .udev = &fake_storage2_replug_udev,
        .interface = &fake_storage2_replug_interface,
        .endpoint = &fake_storage2_replug_bulk_in,
        .address = 0x85,
        .attributes = 0x02,
        .type = 2,
        .direction_in = 1,
        .max_packet_size = 512,
    };
    kb_usb_endpoint_update_t storage2_replug_bulk_out_update = {
        .udev = &fake_storage2_replug_udev,
        .interface = &fake_storage2_replug_interface,
        .endpoint = &fake_storage2_replug_bulk_out,
        .address = 0x06,
        .attributes = 0x02,
        .type = 2,
        .direction_in = 0,
        .max_packet_size = 512,
    };
    if (kb_usb_subsystem_interface_observe(&storage2_replug_interface_update) != 0 ||
        kb_usb_subsystem_endpoint_observe(&storage2_replug_bulk_in_update) != 0 ||
        kb_usb_subsystem_endpoint_observe(&storage2_replug_bulk_out_update) != 0 ||
        kb_usb_storage_subsystem_snapshot(&fake_storage2_replug_interface, &storage2_snapshot) != 0 ||
        kb_usb_storage_subsystem_count() != 2 ||
        storage2_snapshot.disk == 0 ||
        storage2_snapshot.disk_number <= old_storage2_disk_number ||
        storage2_snapshot.bulk_in_endpoint != 0x85 ||
        storage2_snapshot.bulk_out_endpoint != 0x06)
    {
        kb_usb_subsystem_hcd_release(record);
        return 59;
    }

    memset(storage2_multi_read, 0xa5, sizeof(storage2_multi_read));
    if (kb_block_subsystem_disk_read(storage2_snapshot.disk, 30, storage2_multi_read, sizeof(storage2_multi_read)) != 0 ||
        !buffer_is_zero(storage2_multi_read, sizeof(storage2_multi_read)))
    {
        kb_usb_subsystem_hcd_release(record);
        return 60;
    }

    kb_usb_subsystem_device_remove(&fake_storage_udev);
    if (kb_usb_storage_subsystem_count() != 1 ||
        kb_usb_storage_subsystem_snapshot(&fake_storage_interface, &storage_snapshot) == 0 ||
        kb_usb_storage_subsystem_snapshot(&fake_storage2_replug_interface, &storage2_snapshot) != 0 ||
        storage2_snapshot.disk == 0 ||
        storage2_snapshot.udev != &fake_storage2_replug_udev)
    {
        kb_usb_subsystem_hcd_release(record);
        return 61;
    }
    kb_usb_subsystem_driver_deregister(&fake_usb_storage_driver);

    int fake_urb_storage;
    void *urb = &fake_urb_storage;
    int fake_transfer_buffer;
    int fake_setup_packet;
    kb_usb_urb_dma_update_t dma_update = {
        .transfer_buffer = &fake_transfer_buffer,
        .setup_packet = &fake_setup_packet,
        .transfer_dma = 0x12345000,
        .setup_dma = 0x12346000,
        .transfer_buffer_length = 64,
        .actual_length = 16,
        .transfer_flags = 0x10200,
        .dma_map_flags = 0x10000,
        .mem_flags = 0x20,
    };
    if (kb_usb_subsystem_urb_link(allocated_hcd, urb) != 0 ||
        kb_usb_subsystem_urb_map_dma(allocated_hcd, urb, &dma_update) != 0 ||
        kb_usb_subsystem_urb_check_unlink(allocated_hcd, urb, -104) != 0)
    {
        kb_usb_subsystem_hcd_release(record);
        return 25;
    }
    kb_usb_subsystem_urb_submit(allocated_hcd, urb, 0x40, 0);
    kb_usb_subsystem_urb_unlink(allocated_hcd, urb, -104);
    kb_usb_subsystem_urb_unmap_dma(allocated_hcd, urb);
    kb_usb_subsystem_urb_kill(allocated_hcd, urb);
    kb_usb_subsystem_urb_giveback(allocated_hcd, urb, -104);

    kb_usb_urb_snapshot_t urb_snapshot;
    if (kb_usb_subsystem_urb_snapshot(urb, &urb_snapshot) != 0 ||
        urb_snapshot.hcd != allocated_hcd ||
        urb_snapshot.urb != urb ||
        urb_snapshot.transfer_buffer != &fake_transfer_buffer ||
        urb_snapshot.setup_packet != &fake_setup_packet ||
        urb_snapshot.transfer_dma != 0x12345000 ||
        urb_snapshot.setup_dma != 0x12346000 ||
        urb_snapshot.transfer_buffer_length != 64 ||
        urb_snapshot.actual_length != 16 ||
        urb_snapshot.transfer_flags != 0x10200 ||
        urb_snapshot.linked != 0 ||
        urb_snapshot.mapped != 0 ||
        urb_snapshot.link_count != 1 ||
        urb_snapshot.unlink_count != 1 ||
        urb_snapshot.map_count != 1 ||
        urb_snapshot.unmap_count != 1 ||
        urb_snapshot.submit_count != 1 ||
        urb_snapshot.kill_count != 1 ||
        urb_snapshot.giveback_count != 1 ||
        urb_snapshot.last_unlink_status != -104 ||
        urb_snapshot.last_submit_status != 0 ||
        urb_snapshot.last_giveback_status != -104 ||
        urb_snapshot.status != -104 ||
        urb_snapshot.last_mem_flags != 0x20 ||
        urb_snapshot.last_submit_mem_flags != 0x40)
    {
        kb_usb_subsystem_hcd_release(record);
        return 26;
    }

    kb_usb_subsystem_hcd_release(record);

    if (kb_usb_subsystem_hub_event_prepare(allocated_hcd, port1_changed, sizeof(port1_changed), &event) != 0) {
        return 27;
    }
    if (expect_event(&event, 0x02, 0x02, 0x00, 0x00) != 0) {
        return 28;
    }
    if (kb_usb_subsystem_urb_snapshot(urb, &urb_snapshot) == 0) {
        return 29;
    }
    if (kb_usb_subsystem_device_count() != 0 ||
        kb_usb_subsystem_interface_count() != 0 ||
        kb_usb_subsystem_endpoint_count() != 0 ||
        kb_usb_storage_subsystem_count() != 0)
    {
        return 30;
    }

    return 0;
}
