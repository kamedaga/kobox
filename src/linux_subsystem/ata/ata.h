#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct kb_ata_disk_snapshot {
    void *host;
    void *scsi_host;
    void *disk;
    void *queue;
    void *part0;
    uint64_t capacity_sectors;
    uint32_t sector_size;
    uint32_t registered;
    uint32_t scsi_scan_count;
    uint64_t identify_count;
    uint64_t ata_read_count;
    uint64_t ata_write_count;
    uint64_t scsi_command_count;
    uint64_t scsi_read_count;
    uint64_t scsi_write_count;
    uint64_t ahci_identify_count;
    uint64_t ahci_read_count;
    uint64_t ahci_write_count;
    uint64_t ahci_completion_count;
    uint64_t ahci_error_count;
    uint64_t ahci_prdt_count;
    uint64_t ahci_byte_count;
    uint64_t ahci_irq_dispatch_count;
    uint64_t ahci_irq_error_count;
    uint64_t ahci_block_read_count;
    uint64_t ahci_block_write_count;
    uint64_t scsi_queue_count;
    uint64_t scsi_done_count;
    uint64_t scsi_synthetic_queue_count;
    uint64_t scsi_linux_view_queue_count;
    uint64_t block_read_count;
    uint64_t block_write_count;
} kb_ata_disk_snapshot_t;

enum {
    KB_ATA_SYNTHETIC_SCSI_CMD_MAGIC = 0x41544153u,
    KB_ATA_LINUX_SCSI_CMD_VIEW_MAGIC = 0x4c534353u,
};

typedef struct kb_ata_synthetic_scsi_cmd {
    uint32_t magic;
    uint32_t cdb_len;
    unsigned char cdb[16];
    void *buffer;
    size_t buffer_len;
    uint32_t data_out;
    uint8_t status;
    uint32_t residue;
    size_t data_transferred;
    int result;
    uint32_t done_called;
    void (*done)(void *cmd);
} kb_ata_synthetic_scsi_cmd_t;

typedef struct kb_ata_linux_scsi_cmd_view {
    uint32_t magic;
    uint32_t cmd_len;
    unsigned char *cmnd;
    void *buffer;
    size_t buffer_len;
    uint32_t data_out;
    uint8_t status;
    uint32_t residue;
    size_t data_transferred;
    int result;
    uint32_t done_called;
    void (*scsi_done)(void *cmd);
} kb_ata_linux_scsi_cmd_view_t;

int kb_ata_subsystem_register_host(void *host, const void *sht);
void *kb_ata_subsystem_first_disk(void);
int kb_ata_subsystem_resize_disk(void *disk, uint64_t sectors);
int kb_ata_subsystem_snapshot(const void *host, kb_ata_disk_snapshot_t *out_snapshot);
int kb_ata_subsystem_snapshot_by_disk(const void *disk, kb_ata_disk_snapshot_t *out_snapshot);
int kb_ata_subsystem_process_ahci_port(void *host, unsigned port_no);
int kb_ata_subsystem_queue_scsi_command(void *scsi_host, void *scmd);
int kb_ata_subsystem_run_io_smoke(FILE *out);
