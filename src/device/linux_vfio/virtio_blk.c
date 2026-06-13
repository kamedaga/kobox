#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kobox/device_linux_vfio_virtio_blk.h"

#include "kobox/device.h"
#include "kobox/device_linux_vfio.h"
#include "linux_subsystem/block/block.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PCI_COMMAND_OFFSET = 0x04,
    PCI_COMMAND_MEMORY = 0x0002,
    PCI_COMMAND_BUS_MASTER = 0x0004,
    PCI_STATUS_OFFSET = 0x06,
    PCI_STATUS_CAP_LIST = 0x0010,
    PCI_CAPABILITY_LIST_OFFSET = 0x34,
    PCI_CAP_ID_VENDOR = 0x09,

    VIRTIO_PCI_CAP_COMMON_CFG = 1,
    VIRTIO_PCI_CAP_NOTIFY_CFG = 2,
    VIRTIO_PCI_CAP_ISR_CFG = 3,
    VIRTIO_PCI_CAP_DEVICE_CFG = 4,

    VIRTIO_STATUS_ACKNOWLEDGE = 1,
    VIRTIO_STATUS_DRIVER = 2,
    VIRTIO_STATUS_DRIVER_OK = 4,
    VIRTIO_STATUS_FEATURES_OK = 8,
    VIRTIO_STATUS_FAILED = 128,

    VIRTIO_F_VERSION_1_BIT = 32,
    VIRTIO_F_ACCESS_PLATFORM_BIT = 33,
    VIRTIO_BLK_T_IN = 0,
    VIRTIO_BLK_T_OUT = 1,
    VIRTIO_BLK_S_OK = 0,

    VIRTQ_DESC_F_NEXT = 1,
    VIRTQ_DESC_F_WRITE = 2,

    VIRTIO_COMMON_DEVICE_FEATURE_SELECT = 0x00,
    VIRTIO_COMMON_DEVICE_FEATURE = 0x04,
    VIRTIO_COMMON_DRIVER_FEATURE_SELECT = 0x08,
    VIRTIO_COMMON_DRIVER_FEATURE = 0x0c,
    VIRTIO_COMMON_NUM_QUEUES = 0x12,
    VIRTIO_COMMON_DEVICE_STATUS = 0x14,
    VIRTIO_COMMON_QUEUE_SELECT = 0x16,
    VIRTIO_COMMON_QUEUE_SIZE = 0x18,
    VIRTIO_COMMON_QUEUE_ENABLE = 0x1c,
    VIRTIO_COMMON_QUEUE_NOTIFY_OFF = 0x1e,
    VIRTIO_COMMON_QUEUE_DESC = 0x20,
    VIRTIO_COMMON_QUEUE_DRIVER = 0x28,
    VIRTIO_COMMON_QUEUE_DEVICE = 0x30,

    VIRTIO_BLK_QUEUE_SIZE = 8,
    VIRTIO_BLK_SECTOR_SIZE = 512,
    VIRTIO_BLK_MAX_IO = 65536,
};

typedef struct virtio_pci_cap {
    uint8_t cfg_type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t notify_multiplier;
} virtio_pci_cap_t;

typedef struct virtio_queue {
    kb_dma_buffer_t desc;
    kb_dma_buffer_t avail;
    kb_dma_buffer_t used;
    kb_dma_buffer_t header;
    kb_dma_buffer_t data;
    kb_dma_buffer_t status;
    uint16_t size;
    uint16_t avail_idx;
    uint16_t used_idx;
    uint16_t notify_off;
} virtio_queue_t;

struct kb_linux_vfio_virtio_blk_provider {
    kb_device_backend_t *backend;
    const kb_device_backend_ops_t *ops;
    kb_device_t *device;
    kb_mmio_region_t bars[6];
    int bar_mapped[6];
    void *common_cfg;
    void *notify_cfg;
    void *device_cfg;
    uint32_t notify_multiplier;
    uint64_t capacity_sectors;
    virtio_queue_t queue;
    void *block_queue;
    void *disk;
    void *part0;
};

static uint16_t read_le16(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(void *ptr, uint16_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(void *ptr, uint32_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_le64(void *ptr, uint64_t value)
{
    write_le32(ptr, (uint32_t)value);
    write_le32((uint8_t *)ptr + 4, (uint32_t)(value >> 32));
}

static uint8_t mmio_read8(void *base, size_t offset)
{
    volatile uint8_t *reg = (volatile uint8_t *)((uint8_t *)base + offset);
    return *reg;
}

static uint16_t mmio_read16(void *base, size_t offset)
{
    volatile uint16_t *reg = (volatile uint16_t *)((uint8_t *)base + offset);
    return *reg;
}

static uint32_t mmio_read32(void *base, size_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)base + offset);
    return *reg;
}

static uint64_t mmio_read64(void *base, size_t offset)
{
    volatile uint64_t *reg = (volatile uint64_t *)((uint8_t *)base + offset);
    return *reg;
}

static void mmio_write8(void *base, size_t offset, uint8_t value)
{
    volatile uint8_t *reg = (volatile uint8_t *)((uint8_t *)base + offset);
    *reg = value;
}

static void mmio_write16(void *base, size_t offset, uint16_t value)
{
    volatile uint16_t *reg = (volatile uint16_t *)((uint8_t *)base + offset);
    *reg = value;
}

static void mmio_write32(void *base, size_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)base + offset);
    *reg = value;
}

static void mmio_write64(void *base, size_t offset, uint64_t value)
{
    volatile uint64_t *reg = (volatile uint64_t *)((uint8_t *)base + offset);
    *reg = value;
}

static int map_cap_bar(kb_linux_vfio_virtio_blk_provider_t *provider, const virtio_pci_cap_t *cap, void **out_ptr)
{
    if (provider == NULL || cap == NULL || out_ptr == NULL || cap->bar >= 6) {
        return -1;
    }
    if (!provider->bar_mapped[cap->bar]) {
        kb_status_t status = provider->ops->map_bar(provider->device, cap->bar, &provider->bars[cap->bar]);
        if (status != KB_OK || provider->bars[cap->bar].addr == NULL) {
            fprintf(stderr, "virtio-blk vfio: map BAR%u failed status=%d\n", cap->bar, status);
            return -1;
        }
        provider->bar_mapped[cap->bar] = 1;
    }
    if ((uint64_t)cap->offset + cap->length > provider->bars[cap->bar].size) {
        fprintf(stderr, "virtio-blk vfio: cap type=%u outside BAR%u\n", cap->cfg_type, cap->bar);
        return -1;
    }
    *out_ptr = (uint8_t *)provider->bars[cap->bar].addr + cap->offset;
    return 0;
}

static int read_pci8(kb_linux_vfio_virtio_blk_provider_t *provider, uint16_t offset, uint8_t *out_value)
{
    return provider->ops->pci_config_read(provider->device, offset, out_value, sizeof(*out_value)) == KB_OK ? 0 : -1;
}

static int read_pci16(kb_linux_vfio_virtio_blk_provider_t *provider, uint16_t offset, uint16_t *out_value)
{
    return provider->ops->pci_config_read(provider->device, offset, out_value, sizeof(*out_value)) == KB_OK ? 0 : -1;
}

static int read_pci_cap(kb_linux_vfio_virtio_blk_provider_t *provider, uint8_t cap_offset, virtio_pci_cap_t *out_cap)
{
    uint8_t raw[20];
    memset(raw, 0, sizeof(raw));
    if (provider->ops->pci_config_read(provider->device, cap_offset, raw, 16) != KB_OK) {
        return -1;
    }
    if (raw[0] != PCI_CAP_ID_VENDOR || raw[2] < 16) {
        return -1;
    }
    out_cap->cfg_type = raw[3];
    out_cap->bar = raw[4];
    out_cap->offset = read_le32(raw + 8);
    out_cap->length = read_le32(raw + 12);
    out_cap->notify_multiplier = 0;
    if (out_cap->cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && raw[2] >= 20 &&
        provider->ops->pci_config_read(provider->device, cap_offset, raw, 20) == KB_OK)
    {
        out_cap->notify_multiplier = read_le32(raw + 16);
    }
    return 0;
}

static int find_virtio_caps(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    uint16_t status = 0;
    if (read_pci16(provider, PCI_STATUS_OFFSET, &status) != 0 || (status & PCI_STATUS_CAP_LIST) == 0) {
        fprintf(stderr, "virtio-blk vfio: PCI capability list not present\n");
        return -1;
    }
    uint8_t cap_offset = 0;
    if (read_pci8(provider, PCI_CAPABILITY_LIST_OFFSET, &cap_offset) != 0) {
        return -1;
    }

    virtio_pci_cap_t common;
    virtio_pci_cap_t notify;
    virtio_pci_cap_t device;
    memset(&common, 0, sizeof(common));
    memset(&notify, 0, sizeof(notify));
    memset(&device, 0, sizeof(device));
    int have_common = 0;
    int have_notify = 0;
    int have_device = 0;

    for (unsigned guard = 0; cap_offset >= 0x40 && guard < 48; guard++) {
        uint8_t header[2];
        if (provider->ops->pci_config_read(provider->device, cap_offset, header, sizeof(header)) != KB_OK) {
            return -1;
        }
        if (header[0] == PCI_CAP_ID_VENDOR) {
            virtio_pci_cap_t cap;
            if (read_pci_cap(provider, cap_offset, &cap) == 0) {
                if (cap.cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                    common = cap;
                    have_common = 1;
                } else if (cap.cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    notify = cap;
                    have_notify = 1;
                } else if (cap.cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    device = cap;
                    have_device = 1;
                }
            }
        }
        cap_offset = header[1];
    }

    if (!have_common || !have_notify || !have_device) {
        fprintf(stderr,
            "virtio-blk vfio: missing caps common=%d notify=%d device=%d\n",
            have_common,
            have_notify,
            have_device);
        return -1;
    }
    if (map_cap_bar(provider, &common, &provider->common_cfg) != 0 ||
        map_cap_bar(provider, &notify, &provider->notify_cfg) != 0 ||
        map_cap_bar(provider, &device, &provider->device_cfg) != 0)
    {
        return -1;
    }
    provider->notify_multiplier = notify.notify_multiplier;
    return 0;
}

static void virtio_set_status(kb_linux_vfio_virtio_blk_provider_t *provider, uint8_t status)
{
    mmio_write8(provider->common_cfg, VIRTIO_COMMON_DEVICE_STATUS, status);
}

static uint8_t virtio_get_status(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    return mmio_read8(provider->common_cfg, VIRTIO_COMMON_DEVICE_STATUS);
}

static int enable_pci(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    uint16_t command = 0;
    if (provider->ops->pci_config_read(provider->device, PCI_COMMAND_OFFSET, &command, sizeof(command)) != KB_OK) {
        return -1;
    }
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    return provider->ops->pci_config_write(provider->device, PCI_COMMAND_OFFSET, &command, sizeof(command)) == KB_OK ? 0 : -1;
}

static int dma_alloc_zero(
    kb_linux_vfio_virtio_blk_provider_t *provider,
    uint64_t size,
    uint64_t alignment,
    kb_dma_buffer_t *out_buffer)
{
    kb_status_t status = provider->ops->dma_alloc(
        provider->device,
        size,
        alignment,
        KB_DMA_BIDIRECTIONAL,
        out_buffer);
    if (status != KB_OK) {
        fprintf(stderr, "virtio-blk vfio: dma_alloc size=%llu failed status=%d\n", (unsigned long long)size, status);
        return -1;
    }
    memset(out_buffer->cpu_addr, 0, (size_t)out_buffer->size);
    return 0;
}

static void virtqueue_desc_write(void *desc, uint16_t index, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next)
{
    uint8_t *entry = (uint8_t *)desc + ((size_t)index * 16u);
    write_le64(entry, addr);
    write_le32(entry + 8, len);
    write_le16(entry + 12, flags);
    write_le16(entry + 14, next);
}

static void virtio_notify(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    uint32_t notify_offset = (uint32_t)provider->queue.notify_off * provider->notify_multiplier;
    volatile uint16_t *notify = (volatile uint16_t *)((uint8_t *)provider->notify_cfg + notify_offset);
    *notify = 0;
}

static int virtio_wait_used(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    uint8_t *used = (uint8_t *)provider->queue.used.cpu_addr;
    for (unsigned i = 0; i < 10000000; i++) {
        __sync_synchronize();
        uint16_t used_idx = read_le16(used + 2);
        if (used_idx != provider->queue.used_idx) {
            uint16_t slot = provider->queue.used_idx % provider->queue.size;
            uint8_t *elem = used + 4 + ((size_t)slot * 8u);
            uint32_t id = read_le32(elem);
            provider->queue.used_idx++;
            if (id != 0) {
                fprintf(stderr, "virtio-blk vfio: unexpected used id=%u\n", id);
                return -1;
            }
            return 0;
        }
    }
    fprintf(stderr, "virtio-blk vfio: used ring timeout\n");
    return -1;
}

static int virtio_blk_rw(kb_linux_vfio_virtio_blk_provider_t *provider, uint32_t type, uint64_t sector, void *buffer, size_t byte_count)
{
    if (provider == NULL || buffer == NULL || byte_count == 0 || byte_count > VIRTIO_BLK_MAX_IO ||
        (byte_count % VIRTIO_BLK_SECTOR_SIZE) != 0)
    {
        return -22;
    }

    uint8_t *header = (uint8_t *)provider->queue.header.cpu_addr;
    uint8_t *status = (uint8_t *)provider->queue.status.cpu_addr;
    memset(header, 0, 16);
    memset(status, 0xff, 1);
    write_le32(header, type);
    write_le64(header + 8, sector);
    if (type == VIRTIO_BLK_T_OUT) {
        memcpy(provider->queue.data.cpu_addr, buffer, byte_count);
    } else {
        memset(provider->queue.data.cpu_addr, 0, byte_count);
    }

    uint16_t data_flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN) {
        data_flags |= VIRTQ_DESC_F_WRITE;
    }
    virtqueue_desc_write(provider->queue.desc.cpu_addr, 0, provider->queue.header.dma_addr, 16, VIRTQ_DESC_F_NEXT, 1);
    virtqueue_desc_write(provider->queue.desc.cpu_addr, 1, provider->queue.data.dma_addr, (uint32_t)byte_count, data_flags, 2);
    virtqueue_desc_write(provider->queue.desc.cpu_addr, 2, provider->queue.status.dma_addr, 1, VIRTQ_DESC_F_WRITE, 0);

    uint8_t *avail = (uint8_t *)provider->queue.avail.cpu_addr;
    uint16_t slot = provider->queue.avail_idx % provider->queue.size;
    write_le16(avail + 4 + ((size_t)slot * 2u), 0);
    __sync_synchronize();
    provider->queue.avail_idx++;
    write_le16(avail + 2, provider->queue.avail_idx);
    __sync_synchronize();
    virtio_notify(provider);

    if (virtio_wait_used(provider) != 0) {
        return -5;
    }
    __sync_synchronize();
    if (status[0] != VIRTIO_BLK_S_OK) {
        fprintf(stderr, "virtio-blk vfio: request status=%u\n", status[0]);
        return -5;
    }
    if (type == VIRTIO_BLK_T_IN) {
        memcpy(buffer, provider->queue.data.cpu_addr, byte_count);
    }
    return 0;
}

static int provider_disk_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    return virtio_blk_rw((kb_linux_vfio_virtio_blk_provider_t *)ctx, VIRTIO_BLK_T_IN, sector, buffer, byte_count);
}

static int provider_disk_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    return virtio_blk_rw((kb_linux_vfio_virtio_blk_provider_t *)ctx, VIRTIO_BLK_T_OUT, sector, (void *)buffer, byte_count);
}

static int setup_queue(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    virtio_queue_t *queue = &provider->queue;
    mmio_write16(provider->common_cfg, VIRTIO_COMMON_QUEUE_SELECT, 0);
    uint16_t device_queue_size = mmio_read16(provider->common_cfg, VIRTIO_COMMON_QUEUE_SIZE);
    if (device_queue_size < 3) {
        fprintf(stderr, "virtio-blk vfio: invalid queue size=%u\n", device_queue_size);
        return -1;
    }
    queue->size = device_queue_size < VIRTIO_BLK_QUEUE_SIZE ? device_queue_size : VIRTIO_BLK_QUEUE_SIZE;
    mmio_write16(provider->common_cfg, VIRTIO_COMMON_QUEUE_SIZE, queue->size);
    queue->notify_off = mmio_read16(provider->common_cfg, VIRTIO_COMMON_QUEUE_NOTIFY_OFF);

    if (dma_alloc_zero(provider, 4096, 4096, &queue->desc) != 0 ||
        dma_alloc_zero(provider, 4096, 4096, &queue->avail) != 0 ||
        dma_alloc_zero(provider, 4096, 4096, &queue->used) != 0 ||
        dma_alloc_zero(provider, 4096, 4096, &queue->header) != 0 ||
        dma_alloc_zero(provider, VIRTIO_BLK_MAX_IO, 4096, &queue->data) != 0 ||
        dma_alloc_zero(provider, 4096, 4096, &queue->status) != 0)
    {
        return -1;
    }

    mmio_write64(provider->common_cfg, VIRTIO_COMMON_QUEUE_DESC, queue->desc.dma_addr);
    mmio_write64(provider->common_cfg, VIRTIO_COMMON_QUEUE_DRIVER, queue->avail.dma_addr);
    mmio_write64(provider->common_cfg, VIRTIO_COMMON_QUEUE_DEVICE, queue->used.dma_addr);
    mmio_write16(provider->common_cfg, VIRTIO_COMMON_QUEUE_ENABLE, 1);
    return 0;
}

static int setup_virtio_device(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    virtio_set_status(provider, 0);
    virtio_set_status(provider, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(provider, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    mmio_write32(provider->common_cfg, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 1);
    uint32_t high_features = mmio_read32(provider->common_cfg, VIRTIO_COMMON_DEVICE_FEATURE);
    if ((high_features & (1u << (VIRTIO_F_VERSION_1_BIT - 32))) == 0) {
        fprintf(stderr, "virtio-blk vfio: modern VERSION_1 feature missing high=0x%08x\n", high_features);
        return -1;
    }
    uint32_t driver_high_features = 1u << (VIRTIO_F_VERSION_1_BIT - 32);
    if ((high_features & (1u << (VIRTIO_F_ACCESS_PLATFORM_BIT - 32))) != 0) {
        driver_high_features |= 1u << (VIRTIO_F_ACCESS_PLATFORM_BIT - 32);
    }
    mmio_write32(provider->common_cfg, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 0);
    mmio_write32(provider->common_cfg, VIRTIO_COMMON_DRIVER_FEATURE, 0);
    mmio_write32(provider->common_cfg, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 1);
    mmio_write32(provider->common_cfg, VIRTIO_COMMON_DRIVER_FEATURE, driver_high_features);

    virtio_set_status(provider, virtio_get_status(provider) | VIRTIO_STATUS_FEATURES_OK);
    if ((virtio_get_status(provider) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        fprintf(stderr, "virtio-blk vfio: FEATURES_OK rejected status=0x%02x\n", virtio_get_status(provider));
        return -1;
    }
    uint16_t num_queues = mmio_read16(provider->common_cfg, VIRTIO_COMMON_NUM_QUEUES);
    if (num_queues == 0) {
        fprintf(stderr, "virtio-blk vfio: no virtqueues\n");
        return -1;
    }
    provider->capacity_sectors = mmio_read64(provider->device_cfg, 0);
    if (provider->capacity_sectors == 0) {
        fprintf(stderr, "virtio-blk vfio: zero capacity\n");
        return -1;
    }
    if (setup_queue(provider) != 0) {
        return -1;
    }
    virtio_set_status(provider, virtio_get_status(provider) | VIRTIO_STATUS_DRIVER_OK);
    if ((virtio_get_status(provider) & VIRTIO_STATUS_FAILED) != 0) {
        fprintf(stderr, "virtio-blk vfio: device failed status=0x%02x\n", virtio_get_status(provider));
        return -1;
    }
    return 0;
}

static int setup_block_disk(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    provider->block_queue = kb_block_subsystem_queue_alloc(NULL);
    provider->disk = kb_block_subsystem_disk_alloc();
    provider->part0 = kb_block_subsystem_block_device_alloc();
    if (provider->block_queue == NULL || provider->disk == NULL || provider->part0 == NULL) {
        return -1;
    }
    if (kb_block_subsystem_disk_attach(provider->disk, provider->block_queue, provider->part0) != 0) {
        return -1;
    }
    kb_block_subsystem_disk_set_capacity(provider->disk, provider->capacity_sectors);
    kb_block_subsystem_disk_set_io(provider->disk, provider, provider_disk_read, provider_disk_write);
    if (kb_block_subsystem_disk_register(NULL, provider->disk, NULL) != 0) {
        return -1;
    }
    return 0;
}

kb_status_t kb_linux_vfio_virtio_blk_provider_create(
    const char *bdf,
    kb_linux_vfio_virtio_blk_provider_t **out_provider)
{
    if (bdf == NULL || out_provider == NULL) {
        return KB_ERR_INVALID;
    }
    *out_provider = NULL;
    kb_linux_vfio_virtio_blk_provider_t *provider = calloc(1, sizeof(*provider));
    if (provider == NULL) {
        return KB_ERR_NOMEM;
    }
    kb_status_t status = kb_linux_vfio_device_create(bdf, &provider->backend);
    if (status != KB_OK) {
        fprintf(stderr, "virtio-blk vfio: backend create failed status=%d\n", status);
        free(provider);
        return status;
    }
    provider->ops = kb_device_backend_get_ops(provider->backend);
    status = provider->ops->device_at(provider->backend, 0, &provider->device);
    if (status != KB_OK || provider->device == NULL) {
        kb_linux_vfio_virtio_blk_provider_destroy(provider);
        return status == KB_OK ? KB_ERR_IO : status;
    }
    kb_pci_id_t pci_id;
    memset(&pci_id, 0, sizeof(pci_id));
    if (provider->ops->device_pci_id(provider->device, &pci_id) == KB_OK) {
        printf(
            "virtio-blk vfio: pci=%04x:%04x class=%02x%02x%02x\n",
            pci_id.vendor_id,
            pci_id.device_id,
            pci_id.class_code,
            pci_id.subclass,
            pci_id.prog_if);
    }
    if (enable_pci(provider) != 0 ||
        find_virtio_caps(provider) != 0 ||
        setup_virtio_device(provider) != 0 ||
        setup_block_disk(provider) != 0)
    {
        kb_linux_vfio_virtio_blk_provider_destroy(provider);
        return KB_ERR_IO;
    }
    printf("virtio-blk vfio: capacity_sectors=%llu\n", (unsigned long long)provider->capacity_sectors);
    *out_provider = provider;
    return KB_OK;
}

void *kb_linux_vfio_virtio_blk_provider_disk(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    return provider == NULL ? NULL : provider->disk;
}

void kb_linux_vfio_virtio_blk_provider_destroy(kb_linux_vfio_virtio_blk_provider_t *provider)
{
    if (provider == NULL) {
        return;
    }
    if (provider->common_cfg != NULL) {
        virtio_set_status(provider, 0);
    }
    if (provider->disk != NULL) {
        kb_block_subsystem_disk_unregister(provider->disk);
    }
    if (provider->ops != NULL && provider->device != NULL) {
        provider->ops->dma_free(provider->device, &provider->queue.status);
        provider->ops->dma_free(provider->device, &provider->queue.data);
        provider->ops->dma_free(provider->device, &provider->queue.header);
        provider->ops->dma_free(provider->device, &provider->queue.used);
        provider->ops->dma_free(provider->device, &provider->queue.avail);
        provider->ops->dma_free(provider->device, &provider->queue.desc);
        for (unsigned i = 0; i < 6; i++) {
            if (provider->bar_mapped[i]) {
                provider->ops->unmap_bar(provider->device, &provider->bars[i]);
            }
        }
    }
    if (provider->part0 != NULL) {
        kb_block_subsystem_object_free(provider->part0);
    }
    if (provider->disk != NULL) {
        kb_block_subsystem_object_free(provider->disk);
    }
    if (provider->block_queue != NULL) {
        kb_block_subsystem_object_free(provider->block_queue);
    }
    kb_device_backend_destroy(provider->backend);
    free(provider);
}
