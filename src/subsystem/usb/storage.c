#include "subsystem/usb/storage.h"

#include "subsystem/block/block.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_USB_STORAGE_MAX = 32,
    KB_USB_STORAGE_DEFAULT_SECTOR_SIZE = 512,
    KB_USB_STORAGE_DEFAULT_SECTORS = 131072,
    KB_USB_STORAGE_CLASS_MASS_STORAGE = 0x08,
    KB_USB_ENDPOINT_TYPE_BULK = 2,
    KB_USB_STORAGE_EXTENT_SIZE = 4096,
    KB_USB_STORAGE_BOT_CBW_SIGNATURE = 0x43425355u,
    KB_USB_STORAGE_BOT_CSW_SIGNATURE = 0x53425355u,
    KB_USB_STORAGE_BOT_FLAG_DATA_IN = 0x80,
    KB_SCSI_OP_TEST_UNIT_READY = 0x00,
    KB_SCSI_OP_REQUEST_SENSE = 0x03,
    KB_SCSI_OP_INQUIRY = 0x12,
    KB_SCSI_OP_MODE_SENSE_6 = 0x1a,
    KB_SCSI_OP_START_STOP_UNIT = 0x1b,
    KB_SCSI_OP_READ_CAPACITY_10 = 0x25,
    KB_SCSI_OP_READ_10 = 0x28,
    KB_SCSI_OP_WRITE_10 = 0x2a,
    KB_SCSI_OP_SYNCHRONIZE_CACHE_10 = 0x35,
    KB_SCSI_SENSE_NO_SENSE = 0x00,
    KB_SCSI_SENSE_NOT_READY = 0x02,
    KB_SCSI_SENSE_ILLEGAL_REQUEST = 0x05,
    KB_SCSI_ASC_INVALID_COMMAND = 0x20,
    KB_SCSI_ASC_LBA_OUT_OF_RANGE = 0x21,
    KB_SCSI_ASC_INVALID_FIELD_IN_CDB = 0x24,
};

typedef struct kb_usb_storage_extent {
    uint64_t index;
    unsigned char data[KB_USB_STORAGE_EXTENT_SIZE];
    struct kb_usb_storage_extent *next;
} kb_usb_storage_extent_t;

typedef struct kb_usb_storage_record {
    int active;
    kb_usb_storage_snapshot_t snapshot;
    kb_usb_storage_extent_t *extents;
} kb_usb_storage_record_t;

static kb_usb_storage_record_t storage_records[KB_USB_STORAGE_MAX];
static int storage_driver_registered;

static uint64_t default_capacity_sectors(void)
{
    const char *value = getenv("KOBOX_USB_STORAGE_SECTORS");
    if (value == NULL || value[0] == '\0') {
        return KB_USB_STORAGE_DEFAULT_SECTORS;
    }
    char *end = NULL;
    unsigned long long sectors = strtoull(value, &end, 0);
    if (end == value || sectors == 0) {
        return KB_USB_STORAGE_DEFAULT_SECTORS;
    }
    return sectors;
}

static kb_usb_storage_record_t *record_find_by_interface(const void *interface)
{
    if (interface == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active && storage_records[i].snapshot.interface == interface) {
            return &storage_records[i];
        }
    }
    return NULL;
}

static kb_usb_storage_record_t *record_find_by_host_private(const void *host_private)
{
    if (host_private == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active && storage_records[i].snapshot.driver_data == host_private) {
            return &storage_records[i];
        }
    }
    return NULL;
}

static kb_usb_storage_record_t *record_find_by_host(const void *host)
{
    if (host == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active && storage_records[i].snapshot.scsi_host == host) {
            return &storage_records[i];
        }
    }
    return NULL;
}

static kb_usb_storage_record_t *record_find_by_linux_device(const void *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active &&
            (storage_records[i].snapshot.linux_device == dev ||
             storage_records[i].snapshot.parent_linux_device == dev))
        {
            return &storage_records[i];
        }
    }
    return NULL;
}

static kb_usb_storage_record_t *record_find_by_disk(const void *disk)
{
    if (disk == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active && storage_records[i].snapshot.disk == disk) {
            return &storage_records[i];
        }
    }
    return NULL;
}

static kb_usb_storage_record_t *record_only_without_host(void)
{
    kb_usb_storage_record_t *found = NULL;
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active && storage_records[i].snapshot.scsi_host == NULL) {
            if (found != NULL) {
                return NULL;
            }
            found = &storage_records[i];
        }
    }
    return found;
}

static kb_usb_storage_record_t *record_for_interface(void *interface)
{
    kb_usb_storage_record_t *record = record_find_by_interface(interface);
    if (record != NULL || interface == NULL) {
        return record;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (!storage_records[i].active) {
            memset(&storage_records[i], 0, sizeof(storage_records[i]));
            storage_records[i].active = 1;
            storage_records[i].snapshot.interface = interface;
            storage_records[i].snapshot.sector_size = KB_USB_STORAGE_DEFAULT_SECTOR_SIZE;
            storage_records[i].snapshot.capacity_sectors = default_capacity_sectors();
            storage_records[i].snapshot.medium_present = 1;
            return &storage_records[i];
        }
    }
    return NULL;
}

static int module_name_is_usb_storage(const char *module_name)
{
    if (module_name == NULL) {
        return 0;
    }
    return strstr(module_name, "usb-storage") != NULL ||
           strstr(module_name, "usb_storage") != NULL;
}

static uint16_t read_be16(const unsigned char *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static uint32_t read_be32(const unsigned char *value)
{
    return ((uint32_t)value[0] << 24) |
           ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) |
           (uint32_t)value[3];
}

static uint32_t read_le32(const unsigned char *value)
{
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) |
           ((uint32_t)value[3] << 24);
}

static void write_be32(unsigned char *value, uint32_t data)
{
    value[0] = (uint8_t)(data >> 24);
    value[1] = (uint8_t)(data >> 16);
    value[2] = (uint8_t)(data >> 8);
    value[3] = (uint8_t)data;
}

static void write_le32(unsigned char *value, uint32_t data)
{
    value[0] = (uint8_t)data;
    value[1] = (uint8_t)(data >> 8);
    value[2] = (uint8_t)(data >> 16);
    value[3] = (uint8_t)(data >> 24);
}

static void result_good(kb_usb_storage_scsi_result_t *result, size_t transferred, size_t buffer_len)
{
    if (result == NULL) {
        return;
    }
    result->status = KB_USB_STORAGE_SCSI_STATUS_GOOD;
    result->sense_key = KB_SCSI_SENSE_NO_SENSE;
    result->asc = 0;
    result->ascq = 0;
    result->data_transferred = transferred;
    uint64_t residue = transferred <= buffer_len ? (uint64_t)(buffer_len - transferred) : 0;
    result->residue = residue > UINT32_MAX ? UINT32_MAX : (uint32_t)residue;
}

static void result_check_condition(
    kb_usb_storage_record_t *record,
    kb_usb_storage_scsi_result_t *result,
    uint8_t sense_key,
    uint8_t asc,
    uint8_t ascq,
    size_t buffer_len)
{
    if (record != NULL) {
        record->snapshot.last_sense_key = sense_key;
        record->snapshot.last_asc = asc;
        record->snapshot.last_ascq = ascq;
    }
    if (result == NULL) {
        return;
    }
    result->status = KB_USB_STORAGE_SCSI_STATUS_CHECK_CONDITION;
    result->sense_key = sense_key;
    result->asc = asc;
    result->ascq = ascq;
    result->data_transferred = 0;
    result->residue = buffer_len > UINT32_MAX ? UINT32_MAX : (uint32_t)buffer_len;
}

static void bot_fill_csw(kb_usb_storage_bot_result_t *result, uint32_t tag, uint32_t residue, uint8_t status)
{
    if (result == NULL) {
        return;
    }
    result->tag = tag;
    result->residue = residue;
    result->status = status;
    memset(result->csw, 0, sizeof(result->csw));
    write_le32(result->csw, KB_USB_STORAGE_BOT_CSW_SIGNATURE);
    write_le32(result->csw + 4, tag);
    write_le32(result->csw + 8, residue);
    result->csw[12] = status;
}

static void bot_record_status(
    kb_usb_storage_record_t *record,
    uint32_t tag,
    uint32_t residue,
    uint8_t status)
{
    if (record == NULL) {
        return;
    }
    record->snapshot.last_bot_tag = tag;
    record->snapshot.last_bot_residue = residue;
    record->snapshot.last_bot_status = status;
    switch (status) {
    case KB_USB_STORAGE_BOT_STATUS_PASSED:
        record->snapshot.bot_success_count++;
        break;
    case KB_USB_STORAGE_BOT_STATUS_FAILED:
        record->snapshot.bot_failed_count++;
        break;
    case KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR:
        record->snapshot.bot_phase_error_count++;
        break;
    default:
        break;
    }
}

static size_t copy_limited(void *buffer, size_t buffer_len, const unsigned char *data, size_t data_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return 0;
    }
    size_t transferred = data_len < buffer_len ? data_len : buffer_len;
    memcpy(buffer, data, transferred);
    return transferred;
}

static void storage_attach_endpoint(kb_usb_storage_record_t *record, const kb_usb_endpoint_snapshot_t *endpoint)
{
    if (record == NULL || endpoint == NULL || endpoint->type != KB_USB_ENDPOINT_TYPE_BULK) {
        return;
    }
    if (endpoint->direction_in != 0) {
        record->snapshot.bulk_in_endpoint = endpoint->address;
    } else {
        record->snapshot.bulk_out_endpoint = endpoint->address;
    }
}

static int endpoint_matches_record(const kb_usb_endpoint_snapshot_t *endpoint, void *ctx)
{
    kb_usb_storage_record_t *record = ctx;
    if (endpoint == NULL || record == NULL || endpoint->interface != record->snapshot.interface) {
        return 0;
    }
    storage_attach_endpoint(record, endpoint);
    return 0;
}

static void storage_collect_endpoints(kb_usb_storage_record_t *record)
{
    (void)kb_usb_subsystem_for_each_endpoint(endpoint_matches_record, record);
}

static void storage_release_backing(kb_usb_storage_record_t *record)
{
    if (record == NULL) {
        return;
    }
    kb_usb_storage_extent_t *extent = record->extents;
    while (extent != NULL) {
        kb_usb_storage_extent_t *next = extent->next;
        free(extent);
        extent = next;
    }
    record->extents = NULL;
    record->snapshot.backing_bytes = 0;
}

static kb_usb_storage_extent_t *storage_find_extent(kb_usb_storage_record_t *record, uint64_t index)
{
    if (record == NULL) {
        return NULL;
    }
    for (kb_usb_storage_extent_t *extent = record->extents; extent != NULL; extent = extent->next) {
        if (extent->index == index) {
            return extent;
        }
    }
    return NULL;
}

static kb_usb_storage_extent_t *storage_ensure_extent(kb_usb_storage_record_t *record, uint64_t index)
{
    kb_usb_storage_extent_t *extent = storage_find_extent(record, index);
    if (extent != NULL) {
        return extent;
    }
    extent = calloc(1, sizeof(*extent));
    if (extent == NULL) {
        return NULL;
    }
    extent->index = index;
    extent->next = record->extents;
    record->extents = extent;
    record->snapshot.backing_bytes += KB_USB_STORAGE_EXTENT_SIZE;
    return extent;
}

static int storage_backing_transfer(
    kb_usb_storage_record_t *record,
    uint64_t sector,
    void *buffer,
    size_t byte_count,
    int write)
{
    if (record == NULL || buffer == NULL || byte_count == 0) {
        return -22;
    }
    uint32_t sector_size = record->snapshot.sector_size != 0 ?
        record->snapshot.sector_size :
        KB_USB_STORAGE_DEFAULT_SECTOR_SIZE;
    uint64_t offset = sector * (uint64_t)sector_size;
    size_t remaining = byte_count;
    unsigned char *cursor = buffer;
    while (remaining != 0) {
        uint64_t extent_index = offset / KB_USB_STORAGE_EXTENT_SIZE;
        size_t extent_offset = (size_t)(offset % KB_USB_STORAGE_EXTENT_SIZE);
        size_t chunk = KB_USB_STORAGE_EXTENT_SIZE - extent_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        kb_usb_storage_extent_t *extent = write ?
            storage_ensure_extent(record, extent_index) :
            storage_find_extent(record, extent_index);
        if (write) {
            if (extent == NULL) {
                return -12;
            }
            memcpy(extent->data + extent_offset, cursor, chunk);
        } else if (extent != NULL) {
            memcpy(cursor, extent->data + extent_offset, chunk);
        } else {
            memset(cursor, 0, chunk);
        }

        cursor += chunk;
        offset += chunk;
        remaining -= chunk;
    }
    return 0;
}

static int storage_block_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    kb_usb_storage_record_t *record = ctx;
    int result = storage_backing_transfer(record, sector, buffer, byte_count, 0);
    if (result == 0) {
        record->snapshot.read_count++;
        record->snapshot.bytes_read += byte_count;
    }
    return result;
}

static int storage_block_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    kb_usb_storage_record_t *record = ctx;
    int result = storage_backing_transfer(record, sector, (void *)buffer, byte_count, 1);
    if (result == 0) {
        record->snapshot.write_count++;
        record->snapshot.bytes_written += byte_count;
    }
    return result;
}

static void storage_release_block(kb_usb_storage_record_t *record)
{
    if (record == NULL) {
        return;
    }
    if (record->snapshot.disk != NULL) {
        kb_block_subsystem_disk_unregister(record->snapshot.disk);
    }
    kb_block_subsystem_object_free(record->snapshot.disk);
    kb_block_subsystem_object_free(record->snapshot.part0);
    kb_block_subsystem_object_free(record->snapshot.queue);
    free(record->snapshot.tag_set);
    record->snapshot.disk = NULL;
    record->snapshot.part0 = NULL;
    record->snapshot.queue = NULL;
    record->snapshot.tag_set = NULL;
    record->snapshot.disk_number = 0;
    record->snapshot.registered = 0;
    storage_release_backing(record);
}

static int storage_ensure_block(kb_usb_storage_record_t *record)
{
    if (record == NULL) {
        return -22;
    }
    if (record->snapshot.disk != NULL) {
        return 0;
    }

    void *tag_set = calloc(1, 256);
    void *queue = kb_block_subsystem_queue_alloc(tag_set);
    void *disk = kb_block_subsystem_disk_alloc();
    void *part0 = kb_block_subsystem_block_device_alloc();
    if (tag_set == NULL || queue == NULL || disk == NULL || part0 == NULL) {
        free(tag_set);
        kb_block_subsystem_object_free(queue);
        kb_block_subsystem_object_free(disk);
        kb_block_subsystem_object_free(part0);
        return -12;
    }

    kb_block_subsystem_queue_set_logical_block_size(queue, record->snapshot.sector_size);
    kb_block_subsystem_queue_set_physical_block_size(queue, record->snapshot.sector_size);
    kb_block_subsystem_queue_set_io_min(queue, record->snapshot.sector_size);
    kb_block_subsystem_queue_set_max_hw_sectors(queue, 240);
    kb_block_subsystem_queue_set_max_segments(queue, 128);

    if (kb_block_subsystem_disk_attach(disk, queue, part0) != 0 ||
        kb_block_subsystem_disk_register(record->snapshot.interface, disk, NULL) != 0)
    {
        free(tag_set);
        kb_block_subsystem_object_free(queue);
        kb_block_subsystem_object_free(disk);
        kb_block_subsystem_object_free(part0);
        return -22;
    }
    kb_block_subsystem_disk_set_capacity(disk, record->snapshot.capacity_sectors);
    kb_block_subsystem_disk_set_io(disk, record, storage_block_read, storage_block_write);

    kb_block_disk_snapshot_t disk_snapshot;
    if (kb_block_subsystem_disk_snapshot(disk, &disk_snapshot) == 0) {
        record->snapshot.disk_number = disk_snapshot.disk_number;
    }
    record->snapshot.tag_set = tag_set;
    record->snapshot.queue = queue;
    record->snapshot.disk = disk;
    record->snapshot.part0 = part0;
    record->snapshot.registered = 1;
    return 0;
}

void kb_usb_storage_subsystem_observe_interface(const kb_usb_interface_snapshot_t *interface)
{
    if (interface == NULL || interface->interface_class != KB_USB_STORAGE_CLASS_MASS_STORAGE) {
        return;
    }

    kb_usb_storage_record_t *record = record_for_interface(interface->interface);
    if (record == NULL) {
        return;
    }
    record->snapshot.udev = interface->udev;
    record->snapshot.interface = interface->interface;
    record->snapshot.linux_device = interface->linux_device;
    record->snapshot.parent_linux_device = interface->parent_linux_device;
    record->snapshot.driver = interface->driver;
    record->snapshot.driver_data = interface->driver_data;
    record->snapshot.interface_number = interface->interface_number;
    record->snapshot.subclass = interface->interface_subclass;
    record->snapshot.protocol = interface->interface_protocol;
    storage_collect_endpoints(record);

    if (storage_driver_registered || interface->driver != NULL || interface->driver_data != NULL) {
        (void)storage_ensure_block(record);
    }
}

void kb_usb_storage_subsystem_observe_endpoint(const kb_usb_endpoint_snapshot_t *endpoint)
{
    if (endpoint == NULL || endpoint->interface == NULL) {
        return;
    }
    kb_usb_storage_record_t *record = record_find_by_interface(endpoint->interface);
    if (record == NULL) {
        return;
    }
    storage_attach_endpoint(record, endpoint);
    if (storage_driver_registered || record->snapshot.driver != NULL || record->snapshot.driver_data != NULL) {
        (void)storage_ensure_block(record);
    }
}

void kb_usb_storage_subsystem_remove_interface(void *interface)
{
    kb_usb_storage_record_t *record = record_find_by_interface(interface);
    if (record == NULL) {
        return;
    }
    storage_release_block(record);
    memset(record, 0, sizeof(*record));
}

void kb_usb_storage_subsystem_note_driver_registered(void *driver, const char *module_name)
{
    if (!module_name_is_usb_storage(module_name)) {
        return;
    }
    storage_driver_registered = 1;
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active) {
            storage_records[i].snapshot.driver = driver;
            storage_collect_endpoints(&storage_records[i]);
            (void)storage_ensure_block(&storage_records[i]);
        }
    }
}

void kb_usb_storage_subsystem_note_driver_deregistered(void *driver)
{
    (void)driver;
    storage_driver_registered = 0;
}

void kb_usb_storage_subsystem_note_scsi_host(void *host, void *host_private, void *dev, void *dma_dev)
{
    kb_usb_storage_record_t *record = record_find_by_host_private(host_private);
    if (record == NULL) {
        record = record_find_by_linux_device(dev);
    }
    if (record == NULL) {
        record = record_find_by_linux_device(dma_dev);
    }
    if (record == NULL) {
        record = record_only_without_host();
    }
    if (record == NULL) {
        return;
    }
    record->snapshot.scsi_host = host;
    record->snapshot.scsi_host_private = host_private;
    if (record->snapshot.driver_data == NULL) {
        record->snapshot.driver_data = host_private;
    }
    record->snapshot.add_host_count++;
    (void)storage_ensure_block(record);
}

void kb_usb_storage_subsystem_note_scsi_scan(void *host)
{
    kb_usb_storage_record_t *record = record_find_by_host(host);
    if (record == NULL) {
        return;
    }
    record->snapshot.scan_count++;
    (void)storage_ensure_block(record);
}

void kb_usb_storage_subsystem_remove_scsi_host(void *host)
{
    kb_usb_storage_record_t *record = record_find_by_host(host);
    if (record == NULL) {
        return;
    }
    storage_release_block(record);
    record->snapshot.scsi_host = NULL;
    record->snapshot.scsi_host_private = NULL;
}

int kb_usb_storage_subsystem_snapshot(const void *interface, kb_usb_storage_snapshot_t *out_snapshot)
{
    kb_usb_storage_record_t *record = record_find_by_interface(interface);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}

int kb_usb_storage_subsystem_snapshot_by_disk(const void *disk, kb_usb_storage_snapshot_t *out_snapshot)
{
    kb_usb_storage_record_t *record = record_find_by_disk(disk);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    *out_snapshot = record->snapshot;
    return 0;
}

size_t kb_usb_storage_subsystem_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (storage_records[i].active) {
            count++;
        }
    }
    return count;
}

int kb_usb_storage_subsystem_for_each(
    int (*callback)(const kb_usb_storage_snapshot_t *snapshot, void *ctx),
    void *ctx)
{
    if (callback == NULL) {
        return 0;
    }
    int visited = 0;
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (!storage_records[i].active) {
            continue;
        }
        visited++;
        kb_usb_storage_snapshot_t snapshot = storage_records[i].snapshot;
        if (callback(&snapshot, ctx) != 0) {
            break;
        }
    }
    return visited;
}

int kb_usb_storage_subsystem_scsi_command(
    const void *interface,
    const void *cdb,
    size_t cdb_len,
    void *buffer,
    size_t buffer_len,
    int data_out,
    kb_usb_storage_scsi_result_t *out_result)
{
    kb_usb_storage_record_t *record = record_find_by_interface(interface);
    if (record == NULL || cdb == NULL || cdb_len == 0) {
        return -22;
    }
    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }
    const unsigned char *command = cdb;
    uint8_t opcode = command[0];
    record->snapshot.scsi_command_count++;
    record->snapshot.last_scsi_opcode = opcode;

    if (record->snapshot.medium_present == 0) {
        result_check_condition(record, out_result, KB_SCSI_SENSE_NOT_READY, 0x3a, 0x00, buffer_len);
        return 0;
    }

    switch (opcode) {
    case KB_SCSI_OP_TEST_UNIT_READY:
    case KB_SCSI_OP_START_STOP_UNIT:
    case KB_SCSI_OP_SYNCHRONIZE_CACHE_10:
        result_good(out_result, 0, buffer_len);
        return 0;

    case KB_SCSI_OP_REQUEST_SENSE: {
        unsigned char sense[18];
        memset(sense, 0, sizeof(sense));
        sense[0] = 0x70;
        sense[2] = record->snapshot.last_sense_key;
        sense[7] = 10;
        sense[12] = record->snapshot.last_asc;
        sense[13] = record->snapshot.last_ascq;
        size_t transferred = copy_limited(buffer, buffer_len, sense, sizeof(sense));
        result_good(out_result, transferred, buffer_len);
        return 0;
    }

    case KB_SCSI_OP_INQUIRY: {
        if (data_out) {
            result_check_condition(
                record,
                out_result,
                KB_SCSI_SENSE_ILLEGAL_REQUEST,
                KB_SCSI_ASC_INVALID_FIELD_IN_CDB,
                0,
                buffer_len);
            return 0;
        }
        unsigned char inquiry[36];
        memset(inquiry, 0, sizeof(inquiry));
        inquiry[0] = 0x00;
        inquiry[1] = 0x80;
        inquiry[2] = 0x06;
        inquiry[3] = 0x02;
        inquiry[4] = 31;
        memcpy(inquiry + 8, "KOBOX   ", 8);
        memcpy(inquiry + 16, "USB STORAGE     ", 16);
        memcpy(inquiry + 32, "0001", 4);
        size_t transferred = copy_limited(buffer, buffer_len, inquiry, sizeof(inquiry));
        result_good(out_result, transferred, buffer_len);
        return 0;
    }

    case KB_SCSI_OP_MODE_SENSE_6: {
        unsigned char mode[4] = { 3, 0, 0, 0 };
        size_t transferred = copy_limited(buffer, buffer_len, mode, sizeof(mode));
        result_good(out_result, transferred, buffer_len);
        return 0;
    }

    case KB_SCSI_OP_READ_CAPACITY_10: {
        if (data_out) {
            result_check_condition(
                record,
                out_result,
                KB_SCSI_SENSE_ILLEGAL_REQUEST,
                KB_SCSI_ASC_INVALID_FIELD_IN_CDB,
                0,
                buffer_len);
            return 0;
        }
        unsigned char capacity[8];
        uint64_t sectors = record->snapshot.capacity_sectors;
        uint32_t last_lba = sectors == 0 ? 0 : (sectors - 1 > UINT32_MAX ? UINT32_MAX : (uint32_t)(sectors - 1));
        write_be32(capacity, last_lba);
        write_be32(capacity + 4, record->snapshot.sector_size);
        size_t transferred = copy_limited(buffer, buffer_len, capacity, sizeof(capacity));
        result_good(out_result, transferred, buffer_len);
        return 0;
    }

    case KB_SCSI_OP_READ_10:
    case KB_SCSI_OP_WRITE_10: {
        if (cdb_len < 10) {
            result_check_condition(
                record,
                out_result,
                KB_SCSI_SENSE_ILLEGAL_REQUEST,
                KB_SCSI_ASC_INVALID_FIELD_IN_CDB,
                0,
                buffer_len);
            return 0;
        }
        int write = opcode == KB_SCSI_OP_WRITE_10;
        if ((write && !data_out) || (!write && data_out)) {
            result_check_condition(
                record,
                out_result,
                KB_SCSI_SENSE_ILLEGAL_REQUEST,
                KB_SCSI_ASC_INVALID_FIELD_IN_CDB,
                0,
                buffer_len);
            return 0;
        }
        uint32_t lba = read_be32(command + 2);
        uint16_t blocks = read_be16(command + 7);
        size_t byte_count = (size_t)blocks * record->snapshot.sector_size;
        if (blocks == 0) {
            result_good(out_result, 0, buffer_len);
            return 0;
        }
        if (buffer == NULL || buffer_len < byte_count ||
            (uint64_t)lba + blocks > record->snapshot.capacity_sectors)
        {
            result_check_condition(
                record,
                out_result,
                KB_SCSI_SENSE_ILLEGAL_REQUEST,
                (uint64_t)lba + blocks > record->snapshot.capacity_sectors ?
                    KB_SCSI_ASC_LBA_OUT_OF_RANGE :
                    KB_SCSI_ASC_INVALID_FIELD_IN_CDB,
                0,
                buffer_len);
            return 0;
        }

        int result = write ?
            kb_block_subsystem_disk_write(record->snapshot.disk, lba, buffer, byte_count) :
            kb_block_subsystem_disk_read(record->snapshot.disk, lba, buffer, byte_count);
        if (result != 0) {
            result_check_condition(
                record,
                out_result,
                KB_SCSI_SENSE_ILLEGAL_REQUEST,
                result == -34 ? KB_SCSI_ASC_LBA_OUT_OF_RANGE : KB_SCSI_ASC_INVALID_FIELD_IN_CDB,
                0,
                buffer_len);
            return 0;
        }
        if (write) {
            record->snapshot.scsi_write_command_count++;
        } else {
            record->snapshot.scsi_read_command_count++;
        }
        result_good(out_result, byte_count, buffer_len);
        return 0;
    }

    default:
        result_check_condition(
            record,
            out_result,
            KB_SCSI_SENSE_ILLEGAL_REQUEST,
            KB_SCSI_ASC_INVALID_COMMAND,
            0,
            buffer_len);
        return 0;
    }
}

int kb_usb_storage_subsystem_bot_transfer(
    const void *interface,
    const void *cbw,
    size_t cbw_len,
    void *data,
    size_t data_len,
    kb_usb_storage_bot_result_t *out_result)
{
    kb_usb_storage_record_t *record = record_find_by_interface(interface);
    if (record == NULL || cbw == NULL || cbw_len < KB_USB_STORAGE_BOT_CBW_SIZE) {
        return -22;
    }
    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }

    const unsigned char *packet = cbw;
    uint32_t signature = read_le32(packet);
    uint32_t tag = read_le32(packet + 4);
    uint32_t transfer_len = read_le32(packet + 8);
    uint8_t flags = packet[12];
    uint8_t lun = packet[13];
    uint8_t cdb_len = packet[14];
    if (signature != KB_USB_STORAGE_BOT_CBW_SIGNATURE || lun != 0 || cdb_len == 0 || cdb_len > 16) {
        return -22;
    }

    record->snapshot.bot_command_count++;

    int data_out = transfer_len != 0 && ((flags & KB_USB_STORAGE_BOT_FLAG_DATA_IN) == 0);
    if (transfer_len > data_len || (transfer_len != 0 && data == NULL)) {
        uint32_t residue = transfer_len;
        bot_record_status(record, tag, residue, KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR);
        bot_fill_csw(out_result, tag, residue, KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR);
        return 0;
    }

    kb_usb_storage_scsi_result_t scsi_result;
    int result = kb_usb_storage_subsystem_scsi_command(
        interface,
        packet + 15,
        cdb_len,
        data,
        transfer_len,
        data_out,
        &scsi_result);
    if (result != 0) {
        uint32_t residue = transfer_len;
        bot_record_status(record, tag, residue, KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR);
        bot_fill_csw(out_result, tag, residue, KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR);
        return 0;
    }

    uint32_t residue = scsi_result.residue;
    if (residue > transfer_len) {
        residue = transfer_len;
    }
    uint8_t status = scsi_result.status == KB_USB_STORAGE_SCSI_STATUS_GOOD ?
        KB_USB_STORAGE_BOT_STATUS_PASSED :
        KB_USB_STORAGE_BOT_STATUS_FAILED;
    bot_record_status(record, tag, residue, status);
    if (out_result != NULL) {
        out_result->data_transferred = scsi_result.data_transferred;
    }
    bot_fill_csw(out_result, tag, residue, status);
    return 0;
}

void kb_usb_storage_subsystem_bot_reset(const void *interface)
{
    kb_usb_storage_record_t *record = record_find_by_interface(interface);
    if (record == NULL) {
        return;
    }
    record->snapshot.bot_reset_count++;
    record->snapshot.last_bot_status = KB_USB_STORAGE_BOT_STATUS_PASSED;
    record->snapshot.last_bot_residue = 0;
}

static void storage_build_bot_cbw(
    unsigned char *cbw,
    uint32_t tag,
    uint32_t transfer_len,
    int data_in,
    const unsigned char *cdb,
    size_t cdb_len)
{
    memset(cbw, 0, KB_USB_STORAGE_BOT_CBW_SIZE);
    write_le32(cbw, KB_USB_STORAGE_BOT_CBW_SIGNATURE);
    write_le32(cbw + 4, tag);
    write_le32(cbw + 8, transfer_len);
    cbw[12] = data_in ? KB_USB_STORAGE_BOT_FLAG_DATA_IN : 0;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
}

void kb_usb_storage_subsystem_print_summary(FILE *out)
{
    if (out == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (!storage_records[i].active) {
            continue;
        }
        const kb_usb_storage_snapshot_t *s = &storage_records[i].snapshot;
        fprintf(out,
            "kobox-usb-storage: interface=%p udev=%p disk=%p disk_number=%u sectors=%llu sector_size=%u bulk_in=0x%02x bulk_out=0x%02x scsi_host=%p registered=%u scans=%u reads=%llu writes=%llu scsi_cmds=%llu scsi_reads=%llu scsi_writes=%llu bot_cmds=%llu bot_ok=%llu bot_fail=%llu bot_phase=%llu backing_bytes=%llu\n",
            s->interface,
            s->udev,
            s->disk,
            s->disk_number,
            (unsigned long long)s->capacity_sectors,
            s->sector_size,
            s->bulk_in_endpoint,
            s->bulk_out_endpoint,
            s->scsi_host,
            s->registered,
            s->scan_count,
            (unsigned long long)s->read_count,
            (unsigned long long)s->write_count,
            (unsigned long long)s->scsi_command_count,
            (unsigned long long)s->scsi_read_command_count,
            (unsigned long long)s->scsi_write_command_count,
            (unsigned long long)s->bot_command_count,
            (unsigned long long)s->bot_success_count,
            (unsigned long long)s->bot_failed_count,
            (unsigned long long)s->bot_phase_error_count,
            (unsigned long long)s->backing_bytes);
    }
}

int kb_usb_storage_subsystem_run_io_smoke(FILE *out)
{
    int visited = 0;
    for (size_t i = 0; i < KB_USB_STORAGE_MAX; i++) {
        if (!storage_records[i].active || storage_records[i].snapshot.disk == NULL ||
            storage_records[i].snapshot.registered == 0)
        {
            continue;
        }
        kb_usb_storage_record_t *record = &storage_records[i];
        visited++;
        if (record->snapshot.capacity_sectors < 16) {
            return -34;
        }

        unsigned char write_buffer[1024];
        unsigned char read_buffer[sizeof(write_buffer)];
        for (size_t j = 0; j < sizeof(write_buffer); j++) {
            write_buffer[j] = (unsigned char)((j * 13u) ^ record->snapshot.disk_number);
            read_buffer[j] = 0;
        }

        uint64_t sector = 8;
        int result = kb_block_subsystem_disk_write(
            record->snapshot.disk,
            sector,
            write_buffer,
            sizeof(write_buffer));
        if (result != 0) {
            return result;
        }
        result = kb_block_subsystem_disk_read(record->snapshot.disk, sector, read_buffer, sizeof(read_buffer));
        if (result != 0) {
            return result;
        }
        if (memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0) {
            return -5;
        }

        unsigned char inquiry[36];
        unsigned char capacity[8];
        unsigned char scsi_write_buffer[512];
        unsigned char scsi_read_buffer[sizeof(scsi_write_buffer)];
        kb_usb_storage_scsi_result_t scsi_result;
        memset(inquiry, 0, sizeof(inquiry));
        memset(capacity, 0, sizeof(capacity));
        for (size_t j = 0; j < sizeof(scsi_write_buffer); j++) {
            scsi_write_buffer[j] = (unsigned char)((j * 17u) ^ (record->snapshot.disk_number + 0x31u));
            scsi_read_buffer[j] = 0;
        }

        unsigned char inquiry_cdb[6] = { KB_SCSI_OP_INQUIRY, 0, 0, 0, sizeof(inquiry), 0 };
        result = kb_usb_storage_subsystem_scsi_command(
            record->snapshot.interface,
            inquiry_cdb,
            sizeof(inquiry_cdb),
            inquiry,
            sizeof(inquiry),
            0,
            &scsi_result);
        if (result != 0 || scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
            scsi_result.data_transferred != sizeof(inquiry) ||
            memcmp(inquiry + 8, "KOBOX   ", 8) != 0)
        {
            return result != 0 ? result : -5;
        }

        unsigned char capacity_cdb[10] = { KB_SCSI_OP_READ_CAPACITY_10 };
        result = kb_usb_storage_subsystem_scsi_command(
            record->snapshot.interface,
            capacity_cdb,
            sizeof(capacity_cdb),
            capacity,
            sizeof(capacity),
            0,
            &scsi_result);
        if (result != 0 || scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
            read_be32(capacity + 4) != record->snapshot.sector_size)
        {
            return result != 0 ? result : -5;
        }

        unsigned char write_cdb[10] = { KB_SCSI_OP_WRITE_10 };
        write_cdb[5] = 10;
        write_cdb[8] = 1;
        result = kb_usb_storage_subsystem_scsi_command(
            record->snapshot.interface,
            write_cdb,
            sizeof(write_cdb),
            scsi_write_buffer,
            sizeof(scsi_write_buffer),
            1,
            &scsi_result);
        if (result != 0 || scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
            scsi_result.data_transferred != sizeof(scsi_write_buffer))
        {
            return result != 0 ? result : -5;
        }

        unsigned char read_cdb[10] = { KB_SCSI_OP_READ_10 };
        read_cdb[5] = 10;
        read_cdb[8] = 1;
        result = kb_usb_storage_subsystem_scsi_command(
            record->snapshot.interface,
            read_cdb,
            sizeof(read_cdb),
            scsi_read_buffer,
            sizeof(scsi_read_buffer),
            0,
            &scsi_result);
        if (result != 0 || scsi_result.status != KB_USB_STORAGE_SCSI_STATUS_GOOD ||
            scsi_result.data_transferred != sizeof(scsi_read_buffer) ||
            memcmp(scsi_write_buffer, scsi_read_buffer, sizeof(scsi_write_buffer)) != 0)
        {
            return result != 0 ? result : -5;
        }

        unsigned char cbw[KB_USB_STORAGE_BOT_CBW_SIZE];
        unsigned char bot_inquiry[36];
        unsigned char bot_capacity[8];
        unsigned char bot_write_buffer[512];
        unsigned char bot_read_buffer[sizeof(bot_write_buffer)];
        kb_usb_storage_bot_result_t bot_result;
        memset(bot_inquiry, 0, sizeof(bot_inquiry));
        memset(bot_capacity, 0, sizeof(bot_capacity));
        for (size_t j = 0; j < sizeof(bot_write_buffer); j++) {
            bot_write_buffer[j] = (unsigned char)((j * 19u) ^ (record->snapshot.disk_number + 0x55u));
            bot_read_buffer[j] = 0;
        }

        storage_build_bot_cbw(cbw, 0x1001, sizeof(bot_inquiry), 1, inquiry_cdb, sizeof(inquiry_cdb));
        result = kb_usb_storage_subsystem_bot_transfer(
            record->snapshot.interface,
            cbw,
            sizeof(cbw),
            bot_inquiry,
            sizeof(bot_inquiry),
            &bot_result);
        if (result != 0 || bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
            bot_result.data_transferred != sizeof(bot_inquiry) ||
            read_le32(bot_result.csw) != KB_USB_STORAGE_BOT_CSW_SIGNATURE ||
            read_le32(bot_result.csw + 4) != 0x1001 ||
            memcmp(bot_inquiry + 8, "KOBOX   ", 8) != 0)
        {
            return result != 0 ? result : -5;
        }

        storage_build_bot_cbw(cbw, 0x1002, sizeof(bot_capacity), 1, capacity_cdb, sizeof(capacity_cdb));
        result = kb_usb_storage_subsystem_bot_transfer(
            record->snapshot.interface,
            cbw,
            sizeof(cbw),
            bot_capacity,
            sizeof(bot_capacity),
            &bot_result);
        if (result != 0 || bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
            read_be32(bot_capacity + 4) != record->snapshot.sector_size)
        {
            return result != 0 ? result : -5;
        }

        storage_build_bot_cbw(cbw, 0x1003, sizeof(bot_write_buffer), 0, write_cdb, sizeof(write_cdb));
        result = kb_usb_storage_subsystem_bot_transfer(
            record->snapshot.interface,
            cbw,
            sizeof(cbw),
            bot_write_buffer,
            sizeof(bot_write_buffer),
            &bot_result);
        if (result != 0 || bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
            bot_result.data_transferred != sizeof(bot_write_buffer))
        {
            return result != 0 ? result : -5;
        }

        storage_build_bot_cbw(cbw, 0x1004, sizeof(bot_read_buffer), 1, read_cdb, sizeof(read_cdb));
        result = kb_usb_storage_subsystem_bot_transfer(
            record->snapshot.interface,
            cbw,
            sizeof(cbw),
            bot_read_buffer,
            sizeof(bot_read_buffer),
            &bot_result);
        if (result != 0 || bot_result.status != KB_USB_STORAGE_BOT_STATUS_PASSED ||
            bot_result.data_transferred != sizeof(bot_read_buffer) ||
            memcmp(bot_write_buffer, bot_read_buffer, sizeof(bot_write_buffer)) != 0)
        {
            return result != 0 ? result : -5;
        }

        if (out != NULL) {
            fprintf(
                out,
                "kobox-usb-storage-io: disk=%p disk_number=%u sector=%llu bytes=%zu result=0\n",
                record->snapshot.disk,
                record->snapshot.disk_number,
                (unsigned long long)sector,
                sizeof(write_buffer));
            fprintf(
                out,
                "kobox-usb-storage-scsi: disk=%p disk_number=%u inquiry_vendor=%.8s last_lba=%u block_size=%u bytes=%zu result=0\n",
                record->snapshot.disk,
                record->snapshot.disk_number,
                inquiry + 8,
                read_be32(capacity),
                read_be32(capacity + 4),
                sizeof(scsi_write_buffer));
            fprintf(
                out,
                "kobox-usb-storage-bot: disk=%p disk_number=%u tag=%u residue=%u status=%u bytes=%zu result=0\n",
                record->snapshot.disk,
                record->snapshot.disk_number,
                bot_result.tag,
                bot_result.residue,
                bot_result.status,
                sizeof(bot_write_buffer));
        }
    }
    return visited == 0 ? -19 : 0;
}
