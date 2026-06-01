#pragma once

#include "subsystem/usb/usb.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct kb_usb_storage_snapshot {
    void *udev;
    void *interface;
    void *linux_device;
    void *parent_linux_device;
    void *driver;
    void *driver_data;
    void *scsi_host;
    void *scsi_host_private;
    void *disk;
    void *queue;
    void *part0;
    void *tag_set;
    uint64_t capacity_sectors;
    uint32_t sector_size;
    uint32_t disk_number;
    uint32_t registered;
    uint32_t medium_present;
    uint32_t scan_count;
    uint32_t add_host_count;
    uint64_t backing_bytes;
    uint64_t read_count;
    uint64_t write_count;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t scsi_command_count;
    uint64_t scsi_read_command_count;
    uint64_t scsi_write_command_count;
    uint64_t bot_command_count;
    uint64_t bot_success_count;
    uint64_t bot_failed_count;
    uint64_t bot_phase_error_count;
    uint64_t bot_reset_count;
    uint32_t last_bot_tag;
    uint32_t last_bot_residue;
    uint8_t last_scsi_opcode;
    uint8_t last_sense_key;
    uint8_t last_asc;
    uint8_t last_ascq;
    uint8_t last_bot_status;
    uint8_t interface_number;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
} kb_usb_storage_snapshot_t;

enum {
    KB_USB_STORAGE_SCSI_STATUS_GOOD = 0x00,
    KB_USB_STORAGE_SCSI_STATUS_CHECK_CONDITION = 0x02,
    KB_USB_STORAGE_BOT_CBW_SIZE = 31,
    KB_USB_STORAGE_BOT_CSW_SIZE = 13,
    KB_USB_STORAGE_BOT_STATUS_PASSED = 0x00,
    KB_USB_STORAGE_BOT_STATUS_FAILED = 0x01,
    KB_USB_STORAGE_BOT_STATUS_PHASE_ERROR = 0x02,
};

typedef struct kb_usb_storage_scsi_result {
    uint8_t status;
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;
    size_t data_transferred;
    uint32_t residue;
} kb_usb_storage_scsi_result_t;

typedef struct kb_usb_storage_bot_result {
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
    size_t data_transferred;
    unsigned char csw[KB_USB_STORAGE_BOT_CSW_SIZE];
} kb_usb_storage_bot_result_t;

void kb_usb_storage_subsystem_observe_interface(const kb_usb_interface_snapshot_t *interface);
void kb_usb_storage_subsystem_observe_endpoint(const kb_usb_endpoint_snapshot_t *endpoint);
void kb_usb_storage_subsystem_remove_interface(void *interface);
void kb_usb_storage_subsystem_note_driver_registered(void *driver, const char *module_name);
void kb_usb_storage_subsystem_note_driver_deregistered(void *driver);
void kb_usb_storage_subsystem_note_scsi_host(void *host, void *host_private, void *dev, void *dma_dev);
void kb_usb_storage_subsystem_note_scsi_scan(void *host);
void kb_usb_storage_subsystem_remove_scsi_host(void *host);
int kb_usb_storage_subsystem_snapshot(const void *interface, kb_usb_storage_snapshot_t *out_snapshot);
int kb_usb_storage_subsystem_snapshot_by_disk(const void *disk, kb_usb_storage_snapshot_t *out_snapshot);
size_t kb_usb_storage_subsystem_count(void);
int kb_usb_storage_subsystem_for_each(
    int (*callback)(const kb_usb_storage_snapshot_t *snapshot, void *ctx),
    void *ctx);
void kb_usb_storage_subsystem_print_summary(FILE *out);
int kb_usb_storage_subsystem_scsi_command(
    const void *interface,
    const void *cdb,
    size_t cdb_len,
    void *buffer,
    size_t buffer_len,
    int data_out,
    kb_usb_storage_scsi_result_t *out_result);
int kb_usb_storage_subsystem_bot_transfer(
    const void *interface,
    const void *cbw,
    size_t cbw_len,
    void *data,
    size_t data_len,
    kb_usb_storage_bot_result_t *out_result);
void kb_usb_storage_subsystem_bot_reset(const void *interface);
int kb_usb_storage_subsystem_run_io_smoke(FILE *out);
