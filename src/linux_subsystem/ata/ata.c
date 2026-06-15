#include "kobox/shim.h"
#include "linux_subsystem/ata/ata.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/dma/dma.h"
#include "linux_subsystem/scsi/scsi.h"

#include <stdlib.h>
#include <string.h>

enum {
    KB_ATA_DISK_MAX = 8,
    KB_ATA_HOST_DEV_OFFSET = 0x08,
    KB_ATA_HOST_N_PORTS_OFFSET = 0x18,
    KB_ATA_HOST_HPRIV_OFFSET = 0x20,
    KB_ATA_SECTOR_SIZE = 512,
    KB_ATA_DEFAULT_SECTORS = 4096,

    KB_ATA_OP_IDENTIFY_DEVICE = 0xec,
    KB_ATA_OP_READ_DMA_EXT = 0x25,
    KB_ATA_OP_WRITE_DMA_EXT = 0x35,
    KB_ATA_OP_READ_FPDMA_QUEUED = 0x60,
    KB_ATA_OP_WRITE_FPDMA_QUEUED = 0x61,

    KB_AHCI_HOST_PRIV_MMIO_OFFSET = 0x08,
    KB_AHCI_HOST_IRQ_STAT = 0x08,
    KB_AHCI_PORT0_OFFSET = 0x100,
    KB_AHCI_PORT_STRIDE = 0x80,
    KB_AHCI_PXCLB = 0x00,
    KB_AHCI_PXCLBU = 0x04,
    KB_AHCI_PXIS = 0x10,
    KB_AHCI_PXIE = 0x14,
    KB_AHCI_PXTFD = 0x20,
    KB_AHCI_PXSERR = 0x30,
    KB_AHCI_PXSACT = 0x34,
    KB_AHCI_PXCI = 0x38,
    KB_AHCI_CMD_HEADER_SIZE = 32,
    KB_AHCI_COMMAND_LIST_BYTES = 1024,
    KB_AHCI_CMD_TABLE_CFIS = 0x00,
    KB_AHCI_CMD_TABLE_PRDT = 0x80,
    KB_AHCI_PRDT_ENTRY_SIZE = 16,
    KB_AHCI_PXIS_DHRS = 1u << 0,
    KB_AHCI_PXIS_TFES = 1u << 30,
    KB_AHCI_PXTFD_STS_DRDY = 1u << 6,
    KB_AHCI_PXTFD_STS_ERR = 1u << 0,
    KB_AHCI_FIS_TYPE_REG_H2D = 0x27,
    KB_AHCI_FIS_COMMAND = 1u << 7,
    KB_AHCI_MMIO_HOOK_SIZE = 4096,
    KB_AHCI_SMOKE_PRDTS = 2,
    KB_AHCI_SMOKE_RW_SECTORS = 2,

    KB_SCSI_OP_INQUIRY = 0x12,
    KB_SCSI_OP_READ_CAPACITY_10 = 0x25,
    KB_SCSI_OP_READ_10 = 0x28,
    KB_SCSI_OP_WRITE_10 = 0x2a,
    KB_SCSI_STATUS_GOOD = 0x00,
    KB_SCSI_STATUS_CHECK_CONDITION = 0x02,
};

typedef struct kb_ata_disk_record {
    int active;
    void *host;
    void *scsi_host;
    void *disk;
    void *queue;
    void *part0;
    void *dev;
    void *hpriv;
    uint32_t n_ports;
    int ahci_mmio_hook_registered;
    unsigned char *backing;
    uint64_t capacity_sectors;
    uint32_t sector_size;
    uint64_t identify_count;
    uint64_t ata_read_count;
    uint64_t ata_write_count;
    uint64_t scsi_command_count;
    uint64_t scsi_read_count;
    uint64_t scsi_write_count;
    uint64_t scsi_queue_count;
    uint64_t scsi_done_count;
    uint64_t scsi_synthetic_queue_count;
    uint64_t scsi_linux_view_queue_count;
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
} kb_ata_disk_record_t;

typedef struct kb_ata_scsi_result {
    uint8_t status;
    size_t data_transferred;
    uint32_t residue;
} kb_ata_scsi_result_t;

typedef enum kb_ata_queuecmd_layout {
    KB_ATA_QUEUECMD_LAYOUT_NONE = 0,
    KB_ATA_QUEUECMD_LAYOUT_SYNTHETIC = 1,
    KB_ATA_QUEUECMD_LAYOUT_LINUX_VIEW = 2,
} kb_ata_queuecmd_layout_t;

typedef struct kb_ata_queuecmd_view {
    kb_ata_queuecmd_layout_t layout;
    const unsigned char *cdb;
    size_t cdb_len;
    void *buffer;
    size_t buffer_len;
    int data_out;
    uint8_t *status;
    uint32_t *residue;
    size_t *data_transferred;
    int *result;
    uint32_t *done_called;
    void (*done)(void *cmd);
    void *done_arg;
} kb_ata_queuecmd_view_t;

static kb_ata_disk_record_t ata_disks[KB_ATA_DISK_MAX];

static int trace_ata_enabled(void);

static uint32_t read_u32_local(const unsigned char *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

static void *read_ptr_local(const unsigned char *base, size_t offset)
{
    uintptr_t raw = 0;
    memcpy(&raw, base + offset, sizeof(raw));
    return (void *)raw;
}

static uint16_t read_le16(const unsigned char *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t read_le32(const unsigned char *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint64_t read_le64_addr(const unsigned char *src)
{
    return (uint64_t)read_le32(src) | ((uint64_t)read_le32(src + 4) << 32);
}

static void write_le16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)value;
    dst[1] = (unsigned char)(value >> 8);
}

static void write_le32(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)value;
    dst[1] = (unsigned char)(value >> 8);
    dst[2] = (unsigned char)(value >> 16);
    dst[3] = (unsigned char)(value >> 24);
}

static void write_le64_addr(unsigned char *dst, uint64_t value)
{
    write_le32(dst, (uint32_t)value);
    write_le32(dst + 4, (uint32_t)(value >> 32));
}

static void write_be32(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)(value >> 24);
    dst[1] = (unsigned char)(value >> 16);
    dst[2] = (unsigned char)(value >> 8);
    dst[3] = (unsigned char)value;
}

static uint16_t read_be16(const unsigned char *src)
{
    return ((uint16_t)src[0] << 8) | src[1];
}

static uint32_t read_be32(const unsigned char *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) |
           src[3];
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

static kb_ata_disk_record_t *record_find(const void *host)
{
    if (host == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_ATA_DISK_MAX; i++) {
        if (ata_disks[i].active && ata_disks[i].host == host) {
            return &ata_disks[i];
        }
    }
    return NULL;
}

static kb_ata_disk_record_t *record_find_scsi_host(const void *scsi_host)
{
    if (scsi_host == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_ATA_DISK_MAX; i++) {
        if (ata_disks[i].active && ata_disks[i].scsi_host == scsi_host) {
            return &ata_disks[i];
        }
    }
    return NULL;
}

static kb_ata_disk_record_t *record_find_disk(const void *disk)
{
    if (disk == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_ATA_DISK_MAX; i++) {
        if (ata_disks[i].active && ata_disks[i].disk == disk) {
            return &ata_disks[i];
        }
    }
    return NULL;
}

static kb_ata_disk_record_t *record_alloc(void *host)
{
    kb_ata_disk_record_t *existing = record_find(host);
    if (existing != NULL) {
        return existing;
    }
    for (size_t i = 0; i < KB_ATA_DISK_MAX; i++) {
        if (!ata_disks[i].active) {
            memset(&ata_disks[i], 0, sizeof(ata_disks[i]));
            ata_disks[i].active = 1;
            ata_disks[i].host = host;
            ata_disks[i].sector_size = KB_ATA_SECTOR_SIZE;
            ata_disks[i].capacity_sectors = KB_ATA_DEFAULT_SECTORS;
            return &ata_disks[i];
        }
    }
    return NULL;
}

static int ata_disk_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    kb_ata_disk_record_t *record = ctx;
    if (record == NULL || buffer == NULL) {
        return -22;
    }
    uint64_t offset = sector * (uint64_t)record->sector_size;
    if (offset + byte_count > record->capacity_sectors * (uint64_t)record->sector_size) {
        return -34;
    }
    memcpy(buffer, record->backing + offset, byte_count);
    return 0;
}

static int ata_disk_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    kb_ata_disk_record_t *record = ctx;
    if (record == NULL || buffer == NULL) {
        return -22;
    }
    uint64_t offset = sector * (uint64_t)record->sector_size;
    if (offset + byte_count > record->capacity_sectors * (uint64_t)record->sector_size) {
        return -34;
    }
    memcpy(record->backing + offset, buffer, byte_count);
    return 0;
}

static int ata_identify(kb_ata_disk_record_t *record, void *buffer, size_t buffer_len)
{
    if (record == NULL || buffer == NULL || buffer_len < KB_ATA_SECTOR_SIZE) {
        return -22;
    }
    unsigned char identify[KB_ATA_SECTOR_SIZE];
    memset(identify, 0, sizeof(identify));
    identify[0] = 0x40;
    identify[1] = 0x00;
    memcpy(identify + 54, "KOBOX SATA DISK     ", 20);
    identify[98] = 0x00;
    identify[99] = 0x02;
    uint64_t sectors = record->capacity_sectors;
    for (unsigned i = 0; i < 4; i++) {
        identify[200 + (i * 2)] = (unsigned char)(sectors >> (16 * i));
        identify[201 + (i * 2)] = (unsigned char)(sectors >> ((16 * i) + 8));
    }
    memcpy(buffer, identify, sizeof(identify));
    record->identify_count++;
    return 0;
}

static int ata_command(
    kb_ata_disk_record_t *record,
    uint8_t opcode,
    uint64_t lba,
    uint16_t sectors,
    void *buffer,
    size_t buffer_len,
    int data_out)
{
    if (record == NULL) {
        return -22;
    }
    size_t byte_count = (size_t)sectors * record->sector_size;
    switch (opcode) {
    case KB_ATA_OP_IDENTIFY_DEVICE:
        if (data_out) {
            return -22;
        }
        return ata_identify(record, buffer, buffer_len);
    case KB_ATA_OP_READ_DMA_EXT:
        if (data_out || buffer == NULL || buffer_len < byte_count) {
            return -22;
        }
        if (ata_disk_read(record, lba, buffer, byte_count) != 0) {
            return -34;
        }
        record->ata_read_count++;
        return 0;
    case KB_ATA_OP_WRITE_DMA_EXT:
        if (!data_out || buffer == NULL || buffer_len < byte_count) {
            return -22;
        }
        if (ata_disk_write(record, lba, buffer, byte_count) != 0) {
            return -34;
        }
        record->ata_write_count++;
        return 0;
    default:
        return -95;
    }
}

static unsigned char *ahci_dma_cpu_addr(uint64_t dma_addr, size_t needed, size_t *out_available)
{
    size_t available = 0;
    unsigned char *cpu = kb_subsystem_dma_cpu_addr(dma_addr, &available);
    if (cpu == NULL && dma_addr >= 4096u) {
        cpu = (unsigned char *)(uintptr_t)dma_addr;
        available = needed;
    }
    if (cpu == NULL || available < needed) {
        return NULL;
    }
    if (out_available != NULL) {
        *out_available = available;
    }
    return cpu;
}

static int ahci_copy_to_prdt(const unsigned char *table, uint16_t prdtl, const unsigned char *src, size_t byte_count)
{
    size_t copied = 0;
    for (uint16_t i = 0; i < prdtl && copied < byte_count; i++) {
        const unsigned char *entry = table + KB_AHCI_CMD_TABLE_PRDT + ((size_t)i * KB_AHCI_PRDT_ENTRY_SIZE);
        uint64_t dma = read_le64_addr(entry);
        size_t entry_bytes = (read_le32(entry + 12) & 0x003fffffu) + 1u;
        size_t chunk = byte_count - copied < entry_bytes ? byte_count - copied : entry_bytes;
        unsigned char *dst = ahci_dma_cpu_addr(dma, chunk, NULL);
        if (dst == NULL) {
            return -14;
        }
        memcpy(dst, src + copied, chunk);
        copied += chunk;
    }
    return copied == byte_count ? 0 : -5;
}

static int ahci_copy_from_prdt(unsigned char *dst, size_t byte_count, const unsigned char *table, uint16_t prdtl)
{
    size_t copied = 0;
    for (uint16_t i = 0; i < prdtl && copied < byte_count; i++) {
        const unsigned char *entry = table + KB_AHCI_CMD_TABLE_PRDT + ((size_t)i * KB_AHCI_PRDT_ENTRY_SIZE);
        uint64_t dma = read_le64_addr(entry);
        size_t entry_bytes = (read_le32(entry + 12) & 0x003fffffu) + 1u;
        size_t chunk = byte_count - copied < entry_bytes ? byte_count - copied : entry_bytes;
        unsigned char *src = ahci_dma_cpu_addr(dma, chunk, NULL);
        if (src == NULL) {
            return -14;
        }
        memcpy(dst + copied, src, chunk);
        copied += chunk;
    }
    return copied == byte_count ? 0 : -5;
}

static uint64_t ahci_fis_lba(const unsigned char *fis)
{
    return (uint64_t)fis[4] |
           ((uint64_t)fis[5] << 8) |
           ((uint64_t)fis[6] << 16) |
           ((uint64_t)fis[8] << 24) |
           ((uint64_t)fis[9] << 32) |
           ((uint64_t)fis[10] << 40);
}

static uint16_t ahci_fis_sector_count(const unsigned char *fis)
{
    uint16_t sectors = (uint16_t)fis[12] | ((uint16_t)fis[13] << 8);
    return sectors == 0 ? 65535u : sectors;
}

static uint16_t ahci_fis_ncq_sector_count(const unsigned char *fis)
{
    uint16_t sectors = (uint16_t)fis[3] | ((uint16_t)fis[11] << 8);
    return sectors == 0 ? 65535u : sectors;
}

static int ahci_process_slot(kb_ata_disk_record_t *record, unsigned char *port, uint32_t slot)
{
    uint64_t command_list_dma = read_le64_addr(port + KB_AHCI_PXCLB);
    unsigned char *command_list = ahci_dma_cpu_addr(
        command_list_dma,
        (size_t)(slot + 1u) * KB_AHCI_CMD_HEADER_SIZE,
        NULL);
    if (command_list == NULL) {
        return -14;
    }

    unsigned char *header = command_list + ((size_t)slot * KB_AHCI_CMD_HEADER_SIZE);
    uint16_t prdtl = read_le16(header + 2);
    uint64_t command_table_dma = read_le64_addr(header + 8);
    size_t table_bytes = KB_AHCI_CMD_TABLE_PRDT + ((size_t)prdtl * KB_AHCI_PRDT_ENTRY_SIZE);
    unsigned char *table = ahci_dma_cpu_addr(command_table_dma, table_bytes, NULL);
    if (table == NULL) {
        return -14;
    }

    const unsigned char *fis = table + KB_AHCI_CMD_TABLE_CFIS;
    if (fis[0] != KB_AHCI_FIS_TYPE_REG_H2D || (fis[1] & KB_AHCI_FIS_COMMAND) == 0) {
        return -22;
    }

    uint8_t opcode = fis[2];
    uint64_t lba = ahci_fis_lba(fis);
    int ncq_read = opcode == KB_ATA_OP_READ_FPDMA_QUEUED;
    int ncq_write = opcode == KB_ATA_OP_WRITE_FPDMA_QUEUED;
    uint16_t sectors = (ncq_read || ncq_write) ? ahci_fis_ncq_sector_count(fis) : ahci_fis_sector_count(fis);
    size_t byte_count = opcode == KB_ATA_OP_IDENTIFY_DEVICE ? KB_ATA_SECTOR_SIZE : (size_t)sectors * record->sector_size;
    unsigned char stack_sector[KB_ATA_SECTOR_SIZE];
    unsigned char *scratch = stack_sector;
    if (byte_count > sizeof(stack_sector)) {
        scratch = malloc(byte_count);
        if (scratch == NULL) {
            return -12;
        }
    }

    int result = 0;
    if (opcode == KB_ATA_OP_IDENTIFY_DEVICE || opcode == KB_ATA_OP_READ_DMA_EXT || ncq_read) {
        memset(scratch, 0, byte_count);
        result = ata_command(
            record,
            ncq_read ? KB_ATA_OP_READ_DMA_EXT : opcode,
            lba,
            sectors,
            scratch,
            byte_count,
            0);
        if (result == 0) {
            result = ahci_copy_to_prdt(table, prdtl, scratch, byte_count);
        }
        if (result == 0) {
            if (opcode == KB_ATA_OP_IDENTIFY_DEVICE) {
                record->ahci_identify_count++;
            } else {
                record->ahci_read_count++;
            }
        }
    } else if (opcode == KB_ATA_OP_WRITE_DMA_EXT || ncq_write) {
        result = ahci_copy_from_prdt(scratch, byte_count, table, prdtl);
        if (result == 0) {
            result = ata_command(
                record,
                ncq_write ? KB_ATA_OP_WRITE_DMA_EXT : opcode,
                lba,
                sectors,
                scratch,
                byte_count,
                1);
        }
        if (result == 0) {
            record->ahci_write_count++;
        }
    } else {
        result = -95;
    }

    if (scratch != stack_sector) {
        free(scratch);
    }
    if (result == 0) {
        write_le32(header + 4, (uint32_t)byte_count);
        record->ahci_prdt_count += prdtl;
        record->ahci_byte_count += byte_count;
    }
    return result;
}

static int ahci_process_port(kb_ata_disk_record_t *record, unsigned char *bar, unsigned port_no)
{
    if (record == NULL || bar == NULL) {
        return -22;
    }
    unsigned char *port = bar + KB_AHCI_PORT0_OFFSET + ((size_t)port_no * KB_AHCI_PORT_STRIDE);
    uint32_t ci = read_le32(port + KB_AHCI_PXCI);
    uint32_t sact = read_le32(port + KB_AHCI_PXSACT);
    uint32_t remaining = ci;
    uint32_t remaining_sact = sact;
    uint32_t is = read_le32(port + KB_AHCI_PXIS);
    int completed = 0;
    for (uint32_t slot = 0; slot < 32; slot++) {
        if ((ci & (1u << slot)) == 0) {
            continue;
        }
        int result = ahci_process_slot(record, port, slot);
        remaining &= ~(1u << slot);
        remaining_sact &= ~(1u << slot);
        if (result == 0) {
            is |= KB_AHCI_PXIS_DHRS;
            write_le32(port + KB_AHCI_PXTFD, KB_AHCI_PXTFD_STS_DRDY);
            record->ahci_completion_count++;
            completed = 1;
        } else {
            is |= KB_AHCI_PXIS_TFES;
            write_le32(port + KB_AHCI_PXTFD, KB_AHCI_PXTFD_STS_ERR);
            record->ahci_error_count++;
        }
    }
    write_le32(port + KB_AHCI_PXCI, remaining);
    write_le32(port + KB_AHCI_PXSACT, remaining_sact);
    write_le32(port + KB_AHCI_PXIS, is);
    if (completed) {
        write_le32(bar + KB_AHCI_HOST_IRQ_STAT, 0);
        int irq_result = kb_trigger_irq_for_dev_id(record->host);
        if (irq_result == 0) {
            record->ahci_irq_dispatch_count++;
        } else if (irq_result != -19) {
            record->ahci_irq_error_count++;
        }
        if (trace_ata_enabled()) {
            fprintf(
                stderr,
                "kobox ahci: irq dispatch host=%p port=%u result=%d\n",
                record->host,
                port_no,
                irq_result);
        }
    }
    return (is & KB_AHCI_PXIS_TFES) != 0 ? -5 : 0;
}

static int trace_ata_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_ATA");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int ahci_mmio_write32_hook(void *user, void *addr, uint32_t value)
{
    kb_ata_disk_record_t *record = user;
    if (record == NULL || record->hpriv == NULL) {
        return -22;
    }
    unsigned char *bar = read_ptr_local(record->hpriv, KB_AHCI_HOST_PRIV_MMIO_OFFSET);
    if (bar == NULL) {
        return -19;
    }
    uintptr_t base = (uintptr_t)bar;
    uintptr_t target = (uintptr_t)addr;
    if (target == base + KB_AHCI_HOST_IRQ_STAT) {
        uint32_t current = read_le32(bar + KB_AHCI_HOST_IRQ_STAT);
        write_le32(bar + KB_AHCI_HOST_IRQ_STAT, current & ~value);
        return 1;
    }
    if (target < base + KB_AHCI_PORT0_OFFSET) {
        return 0;
    }
    size_t port_area_offset = target - base - KB_AHCI_PORT0_OFFSET;
    unsigned port_no = (unsigned)(port_area_offset / KB_AHCI_PORT_STRIDE);
    size_t port_register = port_area_offset % KB_AHCI_PORT_STRIDE;
    if (port_no >= record->n_ports) {
        return 0;
    }
    unsigned char *port = bar + KB_AHCI_PORT0_OFFSET + ((size_t)port_no * KB_AHCI_PORT_STRIDE);
    if (port_register == KB_AHCI_PXIS || port_register == KB_AHCI_PXSERR) {
        uint32_t current = read_le32(port + port_register);
        write_le32(port + port_register, current & ~value);
        return 1;
    }
    if (port_register != KB_AHCI_PXCI) {
        return 0;
    }
    write_le32(port + KB_AHCI_PXCI, value);
    if (value == 0) {
        return 1;
    }

    int result = ahci_process_port(record, bar, port_no);
    if (trace_ata_enabled()) {
        fprintf(
            stderr,
            "kobox ahci: pxci write host=%p port=%u value=0x%08x result=%d\n",
            record->host,
            port_no,
            value,
            result);
    }
    return 1;
}

int kb_ata_subsystem_process_ahci_port(void *host, unsigned port_no)
{
    kb_ata_disk_record_t *record = record_find(host);
    if (record == NULL || record->hpriv == NULL) {
        return -22;
    }
    unsigned char *bar = read_ptr_local(record->hpriv, KB_AHCI_HOST_PRIV_MMIO_OFFSET);
    if (bar == NULL) {
        return -19;
    }
    return ahci_process_port(record, bar, port_no);
}

static void ahci_prepare_h2d(
    unsigned char *table,
    uint8_t opcode,
    uint64_t lba,
    uint16_t sectors)
{
    unsigned char *fis = table + KB_AHCI_CMD_TABLE_CFIS;
    memset(fis, 0, 20);
    fis[0] = KB_AHCI_FIS_TYPE_REG_H2D;
    fis[1] = KB_AHCI_FIS_COMMAND;
    fis[2] = opcode;
    if (opcode == KB_ATA_OP_READ_FPDMA_QUEUED || opcode == KB_ATA_OP_WRITE_FPDMA_QUEUED) {
        fis[3] = (unsigned char)sectors;
        fis[11] = (unsigned char)(sectors >> 8);
    }
    fis[4] = (unsigned char)lba;
    fis[5] = (unsigned char)(lba >> 8);
    fis[6] = (unsigned char)(lba >> 16);
    fis[7] = 1u << 6;
    fis[8] = (unsigned char)(lba >> 24);
    fis[9] = (unsigned char)(lba >> 32);
    fis[10] = (unsigned char)(lba >> 40);
    if (opcode == KB_ATA_OP_READ_FPDMA_QUEUED || opcode == KB_ATA_OP_WRITE_FPDMA_QUEUED) {
        fis[12] = 0;
        fis[13] = 0;
    } else {
        fis[12] = (unsigned char)sectors;
        fis[13] = (unsigned char)(sectors >> 8);
    }
}

static int ahci_issue_command(
    kb_ata_disk_record_t *record,
    unsigned char *bar,
    unsigned char *command_list,
    unsigned char *command_table,
    unsigned char *data,
    size_t data_bytes,
    uint16_t prdt_count,
    uint8_t opcode,
    uint64_t lba,
    uint16_t sectors,
    int data_out)
{
    (void)record;
    unsigned char *port = bar + KB_AHCI_PORT0_OFFSET;
    memset(command_list, 0, KB_AHCI_COMMAND_LIST_BYTES);
    memset(command_table, 0, KB_AHCI_CMD_TABLE_PRDT + ((size_t)prdt_count * KB_AHCI_PRDT_ENTRY_SIZE));
    write_le16(command_list, data_out ? (5u | (1u << 6)) : 5u);
    write_le16(command_list + 2, prdt_count);
    write_le64_addr(command_list + 8, (uint64_t)(uintptr_t)command_table);
    size_t remaining = data_bytes;
    size_t offset = 0;
    for (uint16_t i = 0; i < prdt_count; i++) {
        size_t entries_left = (size_t)prdt_count - i;
        size_t chunk = remaining / entries_left;
        unsigned char *entry = command_table + KB_AHCI_CMD_TABLE_PRDT + ((size_t)i * KB_AHCI_PRDT_ENTRY_SIZE);
        write_le64_addr(entry, (uint64_t)(uintptr_t)(data + offset));
        write_le32(entry + 12, (uint32_t)(chunk - 1u));
        offset += chunk;
        remaining -= chunk;
    }
    ahci_prepare_h2d(command_table, opcode, lba, sectors);
    write_le64_addr(port + KB_AHCI_PXCLB, (uint64_t)(uintptr_t)command_list);
    kb_iowrite32(UINT32_MAX, port + KB_AHCI_PXIS);
    kb_iowrite32(UINT32_MAX, port + KB_AHCI_PXSERR);
    if (opcode == KB_ATA_OP_READ_FPDMA_QUEUED || opcode == KB_ATA_OP_WRITE_FPDMA_QUEUED) {
        write_le32(port + KB_AHCI_PXSACT, read_le32(port + KB_AHCI_PXSACT) | 1u);
    }
    kb_iowrite32(1, port + KB_AHCI_PXCI);
    if (read_le32(port + KB_AHCI_PXCI) != 0 ||
        read_le32(port + KB_AHCI_PXSACT) != 0 ||
        read_le32(command_list + 4) != data_bytes ||
        (read_le32(port + KB_AHCI_PXTFD) & KB_AHCI_PXTFD_STS_DRDY) == 0)
    {
        return -5;
    }
    return 0;
}

static unsigned char *record_ahci_bar(kb_ata_disk_record_t *record)
{
    if (record == NULL || record->hpriv == NULL) {
        return NULL;
    }
    return read_ptr_local(record->hpriv, KB_AHCI_HOST_PRIV_MMIO_OFFSET);
}

static int ahci_block_io(kb_ata_disk_record_t *record, uint64_t sector, void *buffer, size_t byte_count, int data_out)
{
    if (record == NULL || buffer == NULL || byte_count == 0 || (byte_count % record->sector_size) != 0) {
        return -22;
    }
    unsigned char *bar = record_ahci_bar(record);
    if (bar == NULL) {
        return -19;
    }

    unsigned char *command_list = calloc(1, KB_AHCI_COMMAND_LIST_BYTES);
    unsigned char *command_table = calloc(1, KB_AHCI_CMD_TABLE_PRDT + KB_AHCI_PRDT_ENTRY_SIZE);
    if (command_list == NULL || command_table == NULL) {
        free(command_list);
        free(command_table);
        return -12;
    }

    unsigned char *bytes = buffer;
    size_t remaining = byte_count;
    uint64_t lba = sector;
    int result = 0;
    while (remaining > 0) {
        size_t chunk_bytes = remaining;
        size_t max_chunk = 8192u * record->sector_size;
        if (chunk_bytes > max_chunk) {
            chunk_bytes = max_chunk;
        }
        uint16_t sectors = (uint16_t)(chunk_bytes / record->sector_size);
        result = ahci_issue_command(
            record,
            bar,
            command_list,
            command_table,
            bytes,
            chunk_bytes,
            1,
            data_out ? KB_ATA_OP_WRITE_DMA_EXT : KB_ATA_OP_READ_DMA_EXT,
            lba,
            sectors,
            data_out);
        if (result != 0) {
            break;
        }
        remaining -= chunk_bytes;
        bytes += chunk_bytes;
        lba += sectors;
    }

    free(command_list);
    free(command_table);
    return result;
}

static int ahci_block_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    kb_ata_disk_record_t *record = ctx;
    int result = ahci_block_io(record, sector, buffer, byte_count, 0);
    if (result == 0) {
        record->ahci_block_read_count++;
    }
    return result;
}

static int ahci_block_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    kb_ata_disk_record_t *record = ctx;
    int result = ahci_block_io(record, sector, (void *)buffer, byte_count, 1);
    if (result == 0) {
        record->ahci_block_write_count++;
    }
    return result;
}

static int ahci_engine_smoke(kb_ata_disk_record_t *record, FILE *out)
{
    if (record == NULL || record->hpriv == NULL) {
        return 0;
    }
    unsigned char *bar = read_ptr_local(record->hpriv, KB_AHCI_HOST_PRIV_MMIO_OFFSET);
    if (bar == NULL) {
        return 0;
    }
    const size_t rw_bytes = KB_ATA_SECTOR_SIZE * KB_AHCI_SMOKE_RW_SECTORS;
    unsigned char *command_list = calloc(1, KB_AHCI_COMMAND_LIST_BYTES);
    unsigned char *command_table = calloc(1, KB_AHCI_CMD_TABLE_PRDT + (KB_AHCI_SMOKE_PRDTS * KB_AHCI_PRDT_ENTRY_SIZE));
    unsigned char *data = calloc(1, rw_bytes);
    unsigned char *expected = calloc(1, rw_bytes);
    if (command_list == NULL || command_table == NULL || data == NULL || expected == NULL) {
        free(command_list);
        free(command_table);
        free(data);
        free(expected);
        return -12;
    }

    int result = ahci_issue_command(
        record,
        bar,
        command_list,
        command_table,
        data,
        KB_ATA_SECTOR_SIZE,
        1,
        KB_ATA_OP_IDENTIFY_DEVICE,
        0,
        1,
        0);
    if (result == 0 && memcmp(data + 54, "KOBOX SATA DISK", 15) != 0) {
        result = -5;
    }
    for (size_t i = 0; i < rw_bytes; i++) {
        expected[i] = (unsigned char)((i * 37u) ^ 0xc3u);
        data[i] = expected[i];
    }
    if (result == 0) {
        result = ahci_issue_command(
            record,
            bar,
            command_list,
            command_table,
            data,
            rw_bytes,
            KB_AHCI_SMOKE_PRDTS,
            KB_ATA_OP_WRITE_DMA_EXT,
            20,
            KB_AHCI_SMOKE_RW_SECTORS,
            1);
    }
    memset(data, 0, rw_bytes);
    if (result == 0) {
        result = ahci_issue_command(
            record,
            bar,
            command_list,
            command_table,
            data,
            rw_bytes,
            KB_AHCI_SMOKE_PRDTS,
            KB_ATA_OP_READ_DMA_EXT,
            20,
            KB_AHCI_SMOKE_RW_SECTORS,
            0);
    }
    if (result == 0 && memcmp(data, expected, rw_bytes) != 0) {
        result = -5;
    }
    for (size_t i = 0; i < rw_bytes; i++) {
        expected[i] = (unsigned char)((i * 41u) ^ 0x7du);
        data[i] = expected[i];
    }
    if (result == 0) {
        result = ahci_issue_command(
            record,
            bar,
            command_list,
            command_table,
            data,
            rw_bytes,
            KB_AHCI_SMOKE_PRDTS,
            KB_ATA_OP_WRITE_FPDMA_QUEUED,
            30,
            KB_AHCI_SMOKE_RW_SECTORS,
            1);
    }
    memset(data, 0, rw_bytes);
    if (result == 0) {
        result = ahci_issue_command(
            record,
            bar,
            command_list,
            command_table,
            data,
            rw_bytes,
            KB_AHCI_SMOKE_PRDTS,
            KB_ATA_OP_READ_FPDMA_QUEUED,
            30,
            KB_AHCI_SMOKE_RW_SECTORS,
            0);
    }
    if (result == 0 && memcmp(data, expected, rw_bytes) != 0) {
        result = -5;
    }
    uint32_t pxci = read_le32(bar + KB_AHCI_PORT0_OFFSET + KB_AHCI_PXCI);
    uint32_t pxis = read_le32(bar + KB_AHCI_PORT0_OFFSET + KB_AHCI_PXIS);
    if (out != NULL) {
        fprintf(
            out,
            "kobox-ahci-engine: host=%p identify=%llu reads=%llu writes=%llu completions=%llu errors=%llu prdts=%llu bytes=%llu irq_dispatches=%llu irq_errors=%llu pxci=0x%08x pxis=0x%08x result=%d\n",
            record->host,
            (unsigned long long)record->ahci_identify_count,
            (unsigned long long)record->ahci_read_count,
            (unsigned long long)record->ahci_write_count,
            (unsigned long long)record->ahci_completion_count,
            (unsigned long long)record->ahci_error_count,
            (unsigned long long)record->ahci_prdt_count,
            (unsigned long long)record->ahci_byte_count,
            (unsigned long long)record->ahci_irq_dispatch_count,
            (unsigned long long)record->ahci_irq_error_count,
            pxci,
            pxis,
            result == 0 ? 0 : -1);
    }

    free(command_list);
    free(command_table);
    free(data);
    free(expected);
    return result;
}

static void synthetic_scsi_done(void *cmd)
{
    (void)cmd;
}

static int scsi_command(
    kb_ata_disk_record_t *record,
    const void *cdb,
    size_t cdb_len,
    void *buffer,
    size_t buffer_len,
    int data_out,
    kb_ata_scsi_result_t *out_result)
{
    if (record == NULL || cdb == NULL || cdb_len == 0) {
        return -22;
    }
    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }
    const unsigned char *command = cdb;
    record->scsi_command_count++;

    switch (command[0]) {
    case KB_SCSI_OP_INQUIRY: {
        if (data_out) {
            if (out_result != NULL) {
                out_result->status = KB_SCSI_STATUS_CHECK_CONDITION;
                out_result->residue = (uint32_t)buffer_len;
            }
            return 0;
        }
        unsigned char inquiry[36];
        memset(inquiry, 0, sizeof(inquiry));
        inquiry[2] = 0x06;
        inquiry[3] = 0x02;
        inquiry[4] = 31;
        memcpy(inquiry + 8, "KOBOX   ", 8);
        memcpy(inquiry + 16, "SATA DISK       ", 16);
        memcpy(inquiry + 32, "0001", 4);
        size_t transferred = copy_limited(buffer, buffer_len, inquiry, sizeof(inquiry));
        if (out_result != NULL) {
            out_result->status = KB_SCSI_STATUS_GOOD;
            out_result->data_transferred = transferred;
            out_result->residue = (uint32_t)(buffer_len - transferred);
        }
        return 0;
    }
    case KB_SCSI_OP_READ_CAPACITY_10: {
        unsigned char capacity[8];
        uint32_t last_lba = record->capacity_sectors == 0 ? 0 :
            (record->capacity_sectors - 1 > UINT32_MAX ? UINT32_MAX : (uint32_t)(record->capacity_sectors - 1));
        write_be32(capacity, last_lba);
        write_be32(capacity + 4, record->sector_size);
        size_t transferred = copy_limited(buffer, buffer_len, capacity, sizeof(capacity));
        if (out_result != NULL) {
            out_result->status = KB_SCSI_STATUS_GOOD;
            out_result->data_transferred = transferred;
            out_result->residue = (uint32_t)(buffer_len - transferred);
        }
        return 0;
    }
    case KB_SCSI_OP_READ_10:
    case KB_SCSI_OP_WRITE_10: {
        if (cdb_len < 10) {
            return -22;
        }
        int write = command[0] == KB_SCSI_OP_WRITE_10;
        uint32_t lba = read_be32(command + 2);
        uint16_t blocks = read_be16(command + 7);
        uint8_t ata_opcode = write ? KB_ATA_OP_WRITE_DMA_EXT : KB_ATA_OP_READ_DMA_EXT;
        int result = ata_command(record, ata_opcode, lba, blocks, buffer, buffer_len, write);
        if (result != 0) {
            return result;
        }
        if (write) {
            record->scsi_write_count++;
        } else {
            record->scsi_read_count++;
        }
        if (out_result != NULL) {
            out_result->status = KB_SCSI_STATUS_GOOD;
            out_result->data_transferred = (size_t)blocks * record->sector_size;
            out_result->residue = (uint32_t)(buffer_len - out_result->data_transferred);
        }
        return 0;
    }
    default:
        if (out_result != NULL) {
            out_result->status = KB_SCSI_STATUS_CHECK_CONDITION;
            out_result->residue = (uint32_t)buffer_len;
        }
        return 0;
    }
}

static int decode_synthetic_queuecmd(void *scmd, kb_ata_queuecmd_view_t *out_view)
{
    kb_ata_synthetic_scsi_cmd_t *cmd = scmd;
    if (cmd == NULL || out_view == NULL) {
        return -22;
    }
    if (cmd->magic != KB_ATA_SYNTHETIC_SCSI_CMD_MAGIC ||
        cmd->cdb_len == 0 ||
        cmd->cdb_len > sizeof(cmd->cdb))
    {
        return -95;
    }

    memset(out_view, 0, sizeof(*out_view));
    out_view->layout = KB_ATA_QUEUECMD_LAYOUT_SYNTHETIC;
    out_view->cdb = cmd->cdb;
    out_view->cdb_len = cmd->cdb_len;
    out_view->buffer = cmd->buffer;
    out_view->buffer_len = cmd->buffer_len;
    out_view->data_out = cmd->data_out != 0;
    out_view->status = &cmd->status;
    out_view->residue = &cmd->residue;
    out_view->data_transferred = &cmd->data_transferred;
    out_view->result = &cmd->result;
    out_view->done_called = &cmd->done_called;
    out_view->done = cmd->done;
    out_view->done_arg = cmd;
    return 0;
}

static int decode_linux_view_queuecmd(void *scmd, kb_ata_queuecmd_view_t *out_view)
{
    kb_ata_linux_scsi_cmd_view_t *cmd = scmd;
    if (cmd == NULL || out_view == NULL) {
        return -22;
    }
    if (cmd->magic != KB_ATA_LINUX_SCSI_CMD_VIEW_MAGIC ||
        cmd->cmnd == NULL ||
        cmd->cmd_len == 0 ||
        cmd->cmd_len > 16)
    {
        return -95;
    }

    memset(out_view, 0, sizeof(*out_view));
    out_view->layout = KB_ATA_QUEUECMD_LAYOUT_LINUX_VIEW;
    out_view->cdb = cmd->cmnd;
    out_view->cdb_len = cmd->cmd_len;
    out_view->buffer = cmd->buffer;
    out_view->buffer_len = cmd->buffer_len;
    out_view->data_out = cmd->data_out != 0;
    out_view->status = &cmd->status;
    out_view->residue = &cmd->residue;
    out_view->data_transferred = &cmd->data_transferred;
    out_view->result = &cmd->result;
    out_view->done_called = &cmd->done_called;
    out_view->done = cmd->scsi_done;
    out_view->done_arg = cmd;
    return 0;
}

static int decode_queuecmd(void *scmd, kb_ata_queuecmd_view_t *out_view)
{
    int result = decode_synthetic_queuecmd(scmd, out_view);
    if (result == 0) {
        return 0;
    }
    return decode_linux_view_queuecmd(scmd, out_view);
}

int kb_ata_subsystem_queue_scsi_command(void *scsi_host, void *scmd)
{
    kb_ata_disk_record_t *record = record_find_scsi_host(scsi_host);
    kb_ata_queuecmd_view_t view;
    if (record == NULL || scmd == NULL) {
        return -22;
    }
    int decode_result = decode_queuecmd(scmd, &view);
    if (decode_result != 0) {
        return decode_result;
    }

    kb_ata_scsi_result_t scsi_result;
    memset(&scsi_result, 0, sizeof(scsi_result));
    int result = scsi_command(
        record,
        view.cdb,
        view.cdb_len,
        view.buffer,
        view.buffer_len,
        view.data_out,
        &scsi_result);
    *view.status = scsi_result.status;
    *view.residue = scsi_result.residue;
    *view.data_transferred = scsi_result.data_transferred;
    *view.result = result;
    record->scsi_queue_count++;
    if (view.layout == KB_ATA_QUEUECMD_LAYOUT_SYNTHETIC) {
        record->scsi_synthetic_queue_count++;
    } else if (view.layout == KB_ATA_QUEUECMD_LAYOUT_LINUX_VIEW) {
        record->scsi_linux_view_queue_count++;
    }
    if (view.done != NULL) {
        view.done(view.done_arg);
        (*view.done_called)++;
        record->scsi_done_count++;
    }
    return result;
}

static int scsi_queuecmd_smoke(kb_ata_disk_record_t *record, FILE *out)
{
    if (record == NULL || record->scsi_host == NULL) {
        return -22;
    }
    unsigned char inquiry[36];
    unsigned char linux_inquiry[36];
    unsigned char write_buffer[KB_ATA_SECTOR_SIZE];
    unsigned char read_buffer[KB_ATA_SECTOR_SIZE];
    unsigned char linux_write_buffer[KB_ATA_SECTOR_SIZE];
    unsigned char linux_read_buffer[KB_ATA_SECTOR_SIZE];
    memset(inquiry, 0, sizeof(inquiry));
    memset(linux_inquiry, 0, sizeof(linux_inquiry));
    for (size_t i = 0; i < sizeof(write_buffer); i++) {
        write_buffer[i] = (unsigned char)((i * 43u) ^ 0x91u);
        read_buffer[i] = 0;
        linux_write_buffer[i] = (unsigned char)((i * 47u) ^ 0x52u);
        linux_read_buffer[i] = 0;
    }

    kb_ata_synthetic_scsi_cmd_t inquiry_cmd;
    memset(&inquiry_cmd, 0, sizeof(inquiry_cmd));
    inquiry_cmd.magic = KB_ATA_SYNTHETIC_SCSI_CMD_MAGIC;
    inquiry_cmd.cdb_len = 6;
    inquiry_cmd.cdb[0] = KB_SCSI_OP_INQUIRY;
    inquiry_cmd.cdb[4] = sizeof(inquiry);
    inquiry_cmd.buffer = inquiry;
    inquiry_cmd.buffer_len = sizeof(inquiry);
    inquiry_cmd.done = synthetic_scsi_done;

    kb_ata_synthetic_scsi_cmd_t write_cmd;
    memset(&write_cmd, 0, sizeof(write_cmd));
    write_cmd.magic = KB_ATA_SYNTHETIC_SCSI_CMD_MAGIC;
    write_cmd.cdb_len = 10;
    write_cmd.cdb[0] = KB_SCSI_OP_WRITE_10;
    write_cmd.cdb[5] = 40;
    write_cmd.cdb[8] = 1;
    write_cmd.buffer = write_buffer;
    write_cmd.buffer_len = sizeof(write_buffer);
    write_cmd.data_out = 1;
    write_cmd.done = synthetic_scsi_done;

    kb_ata_synthetic_scsi_cmd_t read_cmd;
    memset(&read_cmd, 0, sizeof(read_cmd));
    read_cmd.magic = KB_ATA_SYNTHETIC_SCSI_CMD_MAGIC;
    read_cmd.cdb_len = 10;
    read_cmd.cdb[0] = KB_SCSI_OP_READ_10;
    read_cmd.cdb[5] = 40;
    read_cmd.cdb[8] = 1;
    read_cmd.buffer = read_buffer;
    read_cmd.buffer_len = sizeof(read_buffer);
    read_cmd.done = synthetic_scsi_done;

    unsigned char linux_inquiry_cdb[6] = { KB_SCSI_OP_INQUIRY, 0, 0, 0, sizeof(linux_inquiry), 0 };
    unsigned char linux_write_cdb[10] = { KB_SCSI_OP_WRITE_10, 0, 0, 0, 0, 41, 0, 0, 1, 0 };
    unsigned char linux_read_cdb[10] = { KB_SCSI_OP_READ_10, 0, 0, 0, 0, 41, 0, 0, 1, 0 };

    kb_ata_linux_scsi_cmd_view_t linux_inquiry_cmd;
    memset(&linux_inquiry_cmd, 0, sizeof(linux_inquiry_cmd));
    linux_inquiry_cmd.magic = KB_ATA_LINUX_SCSI_CMD_VIEW_MAGIC;
    linux_inquiry_cmd.cmd_len = sizeof(linux_inquiry_cdb);
    linux_inquiry_cmd.cmnd = linux_inquiry_cdb;
    linux_inquiry_cmd.buffer = linux_inquiry;
    linux_inquiry_cmd.buffer_len = sizeof(linux_inquiry);
    linux_inquiry_cmd.scsi_done = synthetic_scsi_done;

    kb_ata_linux_scsi_cmd_view_t linux_write_cmd;
    memset(&linux_write_cmd, 0, sizeof(linux_write_cmd));
    linux_write_cmd.magic = KB_ATA_LINUX_SCSI_CMD_VIEW_MAGIC;
    linux_write_cmd.cmd_len = sizeof(linux_write_cdb);
    linux_write_cmd.cmnd = linux_write_cdb;
    linux_write_cmd.buffer = linux_write_buffer;
    linux_write_cmd.buffer_len = sizeof(linux_write_buffer);
    linux_write_cmd.data_out = 1;
    linux_write_cmd.scsi_done = synthetic_scsi_done;

    kb_ata_linux_scsi_cmd_view_t linux_read_cmd;
    memset(&linux_read_cmd, 0, sizeof(linux_read_cmd));
    linux_read_cmd.magic = KB_ATA_LINUX_SCSI_CMD_VIEW_MAGIC;
    linux_read_cmd.cmd_len = sizeof(linux_read_cdb);
    linux_read_cmd.cmnd = linux_read_cdb;
    linux_read_cmd.buffer = linux_read_buffer;
    linux_read_cmd.buffer_len = sizeof(linux_read_buffer);
    linux_read_cmd.scsi_done = synthetic_scsi_done;

    if (kb_ata_subsystem_queue_scsi_command(record->scsi_host, &inquiry_cmd) != 0 ||
        inquiry_cmd.status != KB_SCSI_STATUS_GOOD ||
        inquiry_cmd.done_called != 1 ||
        kb_ata_subsystem_queue_scsi_command(record->scsi_host, &write_cmd) != 0 ||
        write_cmd.status != KB_SCSI_STATUS_GOOD ||
        write_cmd.done_called != 1 ||
        kb_ata_subsystem_queue_scsi_command(record->scsi_host, &read_cmd) != 0 ||
        read_cmd.status != KB_SCSI_STATUS_GOOD ||
        read_cmd.done_called != 1 ||
        memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0 ||
        kb_ata_subsystem_queue_scsi_command(record->scsi_host, &linux_inquiry_cmd) != 0 ||
        linux_inquiry_cmd.status != KB_SCSI_STATUS_GOOD ||
        linux_inquiry_cmd.done_called != 1 ||
        kb_ata_subsystem_queue_scsi_command(record->scsi_host, &linux_write_cmd) != 0 ||
        linux_write_cmd.status != KB_SCSI_STATUS_GOOD ||
        linux_write_cmd.done_called != 1 ||
        kb_ata_subsystem_queue_scsi_command(record->scsi_host, &linux_read_cmd) != 0 ||
        linux_read_cmd.status != KB_SCSI_STATUS_GOOD ||
        linux_read_cmd.done_called != 1 ||
        memcmp(linux_write_buffer, linux_read_buffer, sizeof(linux_write_buffer)) != 0)
    {
        return -5;
    }

    if (out != NULL) {
        fprintf(
            out,
            "kobox-ata-scsi-queuecmd: host=%p scsi_host=%p queued=%llu done=%llu synthetic=%llu linux_view=%llu inquiry=%.8s linux_inquiry=%.8s bytes=%zu linux_bytes=%zu result=0\n",
            record->host,
            record->scsi_host,
            (unsigned long long)record->scsi_queue_count,
            (unsigned long long)record->scsi_done_count,
            (unsigned long long)record->scsi_synthetic_queue_count,
            (unsigned long long)record->scsi_linux_view_queue_count,
            inquiry + 8,
            linux_inquiry + 8,
            read_cmd.data_transferred,
            linux_read_cmd.data_transferred);
    }
    return 0;
}

int kb_ata_subsystem_register_host(void *host, const void *sht)
{
    if (host == NULL) {
        return -22;
    }
    const unsigned char *host_bytes = host;
    uint32_t n_ports = read_u32_local(host_bytes, KB_ATA_HOST_N_PORTS_OFFSET);
    if (n_ports == 0) {
        return -22;
    }
    kb_ata_disk_record_t *record = record_alloc(host);
    if (record == NULL) {
        return -12;
    }
    record->dev = read_ptr_local(host_bytes, KB_ATA_HOST_DEV_OFFSET);
    record->hpriv = read_ptr_local(host_bytes, KB_ATA_HOST_HPRIV_OFFSET);
    record->n_ports = n_ports;

    if (record->hpriv != NULL && !record->ahci_mmio_hook_registered) {
        void *bar = read_ptr_local(record->hpriv, KB_AHCI_HOST_PRIV_MMIO_OFFSET);
        if (bar != NULL) {
            int hook_result = kb_mmio_register_write32_hook(
                bar,
                KB_AHCI_MMIO_HOOK_SIZE,
                ahci_mmio_write32_hook,
                record);
            if (hook_result != 0) {
                return hook_result;
            }
            record->ahci_mmio_hook_registered = 1;
        }
    }

    if (record->backing == NULL) {
        size_t backing_bytes = (size_t)record->capacity_sectors * record->sector_size;
        record->backing = calloc(1, backing_bytes);
        if (record->backing == NULL) {
            return -12;
        }
    }
    if (record->queue == NULL) {
        record->queue = kb_block_subsystem_queue_alloc(NULL);
        if (record->queue == NULL) {
            return -12;
        }
        kb_block_subsystem_queue_set_logical_block_size(record->queue, record->sector_size);
        kb_block_subsystem_queue_set_physical_block_size(record->queue, record->sector_size);
        kb_block_subsystem_queue_set_max_hw_sectors(record->queue, 1024);
        kb_block_subsystem_queue_set_max_segments(record->queue, 128);
    }
    if (record->disk == NULL) {
        record->disk = kb_block_subsystem_disk_alloc();
        record->part0 = kb_block_subsystem_block_device_alloc();
        if (record->disk == NULL || record->part0 == NULL) {
            return -12;
        }
        if (kb_block_subsystem_disk_attach(record->disk, record->queue, record->part0) != 0) {
            return -22;
        }
        kb_block_subsystem_disk_set_capacity(record->disk, record->capacity_sectors);
        if (kb_block_subsystem_disk_register(record->dev, record->disk, NULL) != 0) {
            return -22;
        }
    }
    if (record->disk != NULL) {
        kb_block_subsystem_disk_set_io(
            record->disk,
            record,
            record_ahci_bar(record) != NULL ? ahci_block_read : ata_disk_read,
            record_ahci_bar(record) != NULL ? ahci_block_write : ata_disk_write);
    }
    if (record->scsi_host == NULL) {
        record->scsi_host = kb_scsi_subsystem_host_alloc((void *)sht, 0);
        if (record->scsi_host == NULL) {
            return -12;
        }
        int result = kb_scsi_subsystem_host_add(record->scsi_host, record->dev, record->dev);
        if (result != 0) {
            return result;
        }
        kb_scsi_subsystem_host_scan(record->scsi_host);
    }
    return 0;
}

void *kb_ata_subsystem_first_disk(void)
{
    for (size_t i = 0; i < KB_ATA_DISK_MAX; i++) {
        if (ata_disks[i].active && ata_disks[i].disk != NULL) {
            return ata_disks[i].disk;
        }
    }
    return NULL;
}

int kb_ata_subsystem_resize_disk(void *disk, uint64_t sectors)
{
    kb_ata_disk_record_t *record = record_find_disk(disk);
    if (record == NULL || sectors == 0) {
        return -22;
    }
    uint64_t new_bytes64 = 0;
    if (__builtin_mul_overflow(sectors, (uint64_t)record->sector_size, &new_bytes64) ||
        new_bytes64 > SIZE_MAX)
    {
        return -34;
    }
    size_t new_bytes = (size_t)new_bytes64;
    unsigned char *new_backing = calloc(1, new_bytes);
    if (new_backing == NULL) {
        return -12;
    }
    if (record->backing != NULL) {
        size_t old_bytes = (size_t)record->capacity_sectors * record->sector_size;
        size_t copy_bytes = old_bytes < new_bytes ? old_bytes : new_bytes;
        memcpy(new_backing, record->backing, copy_bytes);
        free(record->backing);
    }
    record->backing = new_backing;
    record->capacity_sectors = sectors;
    kb_block_subsystem_disk_set_capacity(record->disk, sectors);
    return 0;
}

int kb_ata_subsystem_snapshot(const void *host, kb_ata_disk_snapshot_t *out_snapshot)
{
    kb_ata_disk_record_t *record = record_find(host);
    if (record == NULL || out_snapshot == NULL) {
        return -22;
    }
    kb_block_disk_snapshot_t disk_snapshot;
    memset(&disk_snapshot, 0, sizeof(disk_snapshot));
    (void)kb_block_subsystem_disk_snapshot(record->disk, &disk_snapshot);
    kb_scsi_host_snapshot_t scsi_snapshot;
    memset(&scsi_snapshot, 0, sizeof(scsi_snapshot));
    (void)kb_scsi_subsystem_host_snapshot(record->scsi_host, &scsi_snapshot);

    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->host = record->host;
    out_snapshot->scsi_host = record->scsi_host;
    out_snapshot->disk = record->disk;
    out_snapshot->queue = record->queue;
    out_snapshot->part0 = record->part0;
    out_snapshot->capacity_sectors = record->capacity_sectors;
    out_snapshot->sector_size = record->sector_size;
    out_snapshot->registered = disk_snapshot.registered;
    out_snapshot->scsi_scan_count = scsi_snapshot.scan_count;
    out_snapshot->identify_count = record->identify_count;
    out_snapshot->ata_read_count = record->ata_read_count;
    out_snapshot->ata_write_count = record->ata_write_count;
    out_snapshot->scsi_command_count = record->scsi_command_count;
    out_snapshot->scsi_read_count = record->scsi_read_count;
    out_snapshot->scsi_write_count = record->scsi_write_count;
    out_snapshot->ahci_identify_count = record->ahci_identify_count;
    out_snapshot->ahci_read_count = record->ahci_read_count;
    out_snapshot->ahci_write_count = record->ahci_write_count;
    out_snapshot->ahci_completion_count = record->ahci_completion_count;
    out_snapshot->ahci_error_count = record->ahci_error_count;
    out_snapshot->ahci_prdt_count = record->ahci_prdt_count;
    out_snapshot->ahci_byte_count = record->ahci_byte_count;
    out_snapshot->ahci_irq_dispatch_count = record->ahci_irq_dispatch_count;
    out_snapshot->ahci_irq_error_count = record->ahci_irq_error_count;
    out_snapshot->ahci_block_read_count = record->ahci_block_read_count;
    out_snapshot->ahci_block_write_count = record->ahci_block_write_count;
    out_snapshot->scsi_queue_count = record->scsi_queue_count;
    out_snapshot->scsi_done_count = record->scsi_done_count;
    out_snapshot->scsi_synthetic_queue_count = record->scsi_synthetic_queue_count;
    out_snapshot->scsi_linux_view_queue_count = record->scsi_linux_view_queue_count;
    out_snapshot->block_read_count = disk_snapshot.read_count;
    out_snapshot->block_write_count = disk_snapshot.write_count;
    return 0;
}

int kb_ata_subsystem_snapshot_by_disk(const void *disk, kb_ata_disk_snapshot_t *out_snapshot)
{
    kb_ata_disk_record_t *record = record_find_disk(disk);
    if (record == NULL) {
        return -22;
    }
    return kb_ata_subsystem_snapshot(record->host, out_snapshot);
}

int kb_ata_subsystem_run_io_smoke(FILE *out)
{
    int visited = 0;
    for (size_t i = 0; i < KB_ATA_DISK_MAX; i++) {
        kb_ata_disk_record_t *record = &ata_disks[i];
        if (!record->active || record->disk == NULL || record->scsi_host == NULL) {
            continue;
        }
        visited++;

        unsigned char identify[KB_ATA_SECTOR_SIZE];
        unsigned char write_buffer[KB_ATA_SECTOR_SIZE];
        unsigned char read_buffer[KB_ATA_SECTOR_SIZE];
        for (size_t j = 0; j < sizeof(write_buffer); j++) {
            write_buffer[j] = (unsigned char)((j * 23u) ^ 0x5au);
            read_buffer[j] = 0;
        }
        if (ata_command(record, KB_ATA_OP_IDENTIFY_DEVICE, 0, 1, identify, sizeof(identify), 0) != 0 ||
            ata_command(record, KB_ATA_OP_WRITE_DMA_EXT, 10, 1, write_buffer, sizeof(write_buffer), 1) != 0 ||
            ata_command(record, KB_ATA_OP_READ_DMA_EXT, 10, 1, read_buffer, sizeof(read_buffer), 0) != 0 ||
            memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0)
        {
            return -5;
        }

        unsigned char inquiry[36];
        unsigned char capacity[8];
        unsigned char scsi_write[KB_ATA_SECTOR_SIZE];
        unsigned char scsi_read[KB_ATA_SECTOR_SIZE];
        unsigned char block_write[KB_ATA_SECTOR_SIZE];
        unsigned char block_read[KB_ATA_SECTOR_SIZE];
        kb_ata_scsi_result_t scsi_result;
        memset(inquiry, 0, sizeof(inquiry));
        memset(capacity, 0, sizeof(capacity));
        for (size_t j = 0; j < sizeof(scsi_write); j++) {
            scsi_write[j] = (unsigned char)((j * 29u) ^ 0xa5u);
            scsi_read[j] = 0;
            block_write[j] = (unsigned char)((j * 31u) ^ 0x3cu);
            block_read[j] = 0;
        }

        unsigned char inquiry_cdb[6] = { KB_SCSI_OP_INQUIRY, 0, 0, 0, sizeof(inquiry), 0 };
        unsigned char capacity_cdb[10] = { KB_SCSI_OP_READ_CAPACITY_10 };
        unsigned char write_cdb[10] = { KB_SCSI_OP_WRITE_10, 0, 0, 0, 0, 12, 0, 0, 1, 0 };
        unsigned char read_cdb[10] = { KB_SCSI_OP_READ_10, 0, 0, 0, 0, 12, 0, 0, 1, 0 };
        if (scsi_command(record, inquiry_cdb, sizeof(inquiry_cdb), inquiry, sizeof(inquiry), 0, &scsi_result) != 0 ||
            scsi_result.status != KB_SCSI_STATUS_GOOD ||
            scsi_command(record, capacity_cdb, sizeof(capacity_cdb), capacity, sizeof(capacity), 0, &scsi_result) != 0 ||
            scsi_result.status != KB_SCSI_STATUS_GOOD ||
            read_be32(capacity + 4) != record->sector_size ||
            scsi_command(record, write_cdb, sizeof(write_cdb), scsi_write, sizeof(scsi_write), 1, &scsi_result) != 0 ||
            scsi_result.status != KB_SCSI_STATUS_GOOD ||
            scsi_command(record, read_cdb, sizeof(read_cdb), scsi_read, sizeof(scsi_read), 0, &scsi_result) != 0 ||
            scsi_result.status != KB_SCSI_STATUS_GOOD ||
            memcmp(scsi_write, scsi_read, sizeof(scsi_write)) != 0)
        {
            return -5;
        }
        if (kb_block_subsystem_disk_write(record->disk, 14, block_write, sizeof(block_write)) != 0 ||
            kb_block_subsystem_disk_read(record->disk, 14, block_read, sizeof(block_read)) != 0 ||
            memcmp(block_write, block_read, sizeof(block_write)) != 0)
        {
            return -5;
        }
        if (ahci_engine_smoke(record, out) != 0) {
            return -5;
        }
        if (scsi_queuecmd_smoke(record, out) != 0) {
            return -5;
        }

        kb_block_disk_snapshot_t disk_snapshot;
        if (kb_block_subsystem_disk_snapshot(record->disk, &disk_snapshot) != 0 || disk_snapshot.registered == 0) {
            return -5;
        }
        if (out != NULL) {
            fprintf(
                out,
                "kobox-ata-scsi-block: host=%p scsi_host=%p disk=%p capacity=%llu sector_size=%u scans=%u registered=%u result=0\n",
                record->host,
                record->scsi_host,
                record->disk,
                (unsigned long long)record->capacity_sectors,
                record->sector_size,
                1u,
                disk_snapshot.registered);
            fprintf(
                out,
                "kobox-ahci-block: host=%p disk=%p block_reads=%llu block_writes=%llu ahci_block_reads=%llu ahci_block_writes=%llu result=0\n",
                record->host,
                record->disk,
                (unsigned long long)disk_snapshot.read_count,
                (unsigned long long)disk_snapshot.write_count,
                (unsigned long long)record->ahci_block_read_count,
                (unsigned long long)record->ahci_block_write_count);
            fprintf(
                out,
                "kobox-ata-identify: host=%p model=%.20s sectors=%llu sector_size=%u result=0\n",
                record->host,
                identify + 54,
                (unsigned long long)record->capacity_sectors,
                record->sector_size);
            fprintf(
                out,
                "kobox-ata-io: host=%p ata_reads=%llu ata_writes=%llu scsi_reads=%llu scsi_writes=%llu block_reads=%llu block_writes=%llu bytes=%u result=0\n",
                record->host,
                (unsigned long long)record->ata_read_count,
                (unsigned long long)record->ata_write_count,
                (unsigned long long)record->scsi_read_count,
                (unsigned long long)record->scsi_write_count,
                (unsigned long long)disk_snapshot.read_count,
                (unsigned long long)disk_snapshot.write_count,
                KB_ATA_SECTOR_SIZE);
        }
    }
    return visited == 0 ? -19 : 0;
}
