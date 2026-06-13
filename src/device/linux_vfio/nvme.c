#include "kobox/device_linux_vfio_nvme.h"

#include "kobox/device.h"
#include "kobox/device_linux_vfio.h"
#include "linux_subsystem/block/block.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NVME_REG_CAP = 0x00,
    NVME_REG_VS = 0x08,
    NVME_REG_CC = 0x14,
    NVME_REG_CSTS = 0x1c,
    NVME_REG_AQA = 0x24,
    NVME_REG_ASQ = 0x28,
    NVME_REG_ACQ = 0x30,

    NVME_ADMIN_CREATE_SQ = 0x01,
    NVME_ADMIN_CREATE_CQ = 0x05,
    NVME_ADMIN_IDENTIFY = 0x06,
    NVME_CMD_WRITE = 0x01,
    NVME_CMD_READ = 0x02,

    NVME_ADMIN_QUEUE_DEPTH = 16,
    NVME_IO_QUEUE_DEPTH = 16,
    NVME_IDENTIFY_NAMESPACE = 0,
    NVME_IDENTIFY_CONTROLLER = 1,
    NVME_IDENTIFY_NAMESPACE_LIST = 2,
    NVME_PROVIDER_MAX_IO = 65536,
};

typedef struct nvme_command {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t reserved0;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_command_t;

typedef struct nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} nvme_completion_t;

typedef struct nvme_queue {
    nvme_command_t *sq;
    nvme_completion_t *cq;
    uint16_t qid;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t next_cid;
    uint8_t phase;
    uint32_t sq_tail_db;
    uint32_t cq_head_db;
} nvme_queue_t;

typedef struct irq_state {
    unsigned count;
} irq_state_t;

struct kb_linux_vfio_nvme_provider {
    kb_device_backend_t *backend;
    const kb_device_backend_ops_t *ops;
    kb_device_t *device;
    kb_mmio_region_t bar;
    kb_dma_buffer_t admin_sq;
    kb_dma_buffer_t admin_cq;
    kb_dma_buffer_t identify;
    kb_dma_buffer_t io_sq;
    kb_dma_buffer_t io_cq;
    kb_dma_buffer_t io_buffer;
    kb_device_irq_t *io_irq;
    irq_state_t irq_state;
    nvme_queue_t admin_queue;
    nvme_queue_t io_queue;
    uint32_t doorbell_stride;
    uint32_t mps_min;
    uint64_t page_size;
    uint32_t nsid;
    uint64_t nsze;
    uint32_t lba_size;
    void *block_queue;
    void *disk;
    void *part0;
};

static void irq_handler(void *ctx)
{
    irq_state_t *state = ctx;
    state->count++;
}

static uint32_t mmio_read32(void *base, size_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    return *reg;
}

static uint64_t mmio_read64(void *base, size_t offset)
{
    volatile uint64_t *reg = (volatile uint64_t *)((unsigned char *)base + offset);
    return *reg;
}

static void mmio_write32(void *base, size_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
    *reg = value;
}

static void mmio_write64(void *base, size_t offset, uint64_t value)
{
    volatile uint64_t *reg = (volatile uint64_t *)((unsigned char *)base + offset);
    *reg = value;
}

static uint16_t read_le16(const unsigned char *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t read_le32(const unsigned char *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint64_t read_le64(const unsigned char *src)
{
    uint64_t low = read_le32(src);
    uint64_t high = read_le32(src + 4);
    return low | (high << 32);
}

static uint32_t nvme_doorbell_stride(uint64_t cap)
{
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xfu);
    return 4u << dstrd;
}

static uint32_t nvme_mps_min(uint64_t cap)
{
    return (uint32_t)((cap >> 48) & 0xfu);
}

static int wait_csts_ready(void *bar, int ready)
{
    for (unsigned i = 0; i < 10000000; i++) {
        uint32_t csts = mmio_read32(bar, NVME_REG_CSTS);
        if (((csts & 1u) != 0u) == ready) {
            return 0;
        }
    }
    return 1;
}

static int enable_pci_memory_and_bus_master(const kb_device_backend_ops_t *ops, kb_device_t *device)
{
    uint16_t command = 0;
    kb_status_t status = ops->pci_config_read(device, 0x04, &command, sizeof(command));
    if (status != KB_OK) {
        return 1;
    }
    command |= 0x0006u;
    return ops->pci_config_write(device, 0x04, &command, sizeof(command)) == KB_OK ? 0 : 1;
}

static void nvme_queue_init(nvme_queue_t *queue, uint16_t qid, uint16_t depth, uint32_t doorbell_stride, void *sq, void *cq)
{
    memset(queue, 0, sizeof(*queue));
    queue->sq = sq;
    queue->cq = cq;
    queue->qid = qid;
    queue->depth = depth;
    queue->phase = 1;
    queue->next_cid = 1;
    queue->sq_tail_db = 0x1000u + ((uint32_t)qid * 2u * doorbell_stride);
    queue->cq_head_db = queue->sq_tail_db + doorbell_stride;
}

static int nvme_submit_and_wait(
    const kb_device_backend_ops_t *ops,
    kb_device_t *device,
    kb_device_irq_t *irq,
    void *bar,
    nvme_queue_t *queue,
    nvme_command_t *command,
    nvme_completion_t *out_completion)
{
    uint16_t cid = queue->next_cid++;
    command->cdw0 |= (uint32_t)cid << 16;
    queue->sq[queue->sq_tail] = *command;
    queue->sq_tail++;
    if (queue->sq_tail == queue->depth) {
        queue->sq_tail = 0;
    }
    mmio_write32(bar, queue->sq_tail_db, queue->sq_tail);

    if (irq != NULL) {
        kb_status_t status = ops->irq_wait(device, irq, 1000000000ull);
        if (status != KB_OK) {
            fprintf(stderr, "nvme provider: irq_wait failed status=%d\n", status);
            return 1;
        }
    }

    nvme_completion_t completion;
    memset(&completion, 0, sizeof(completion));
    for (unsigned i = 0; i < 10000000; i++) {
        completion = queue->cq[queue->cq_head];
        if ((completion.status & 1u) == queue->phase) {
            queue->cq_head++;
            if (queue->cq_head == queue->depth) {
                queue->cq_head = 0;
                queue->phase ^= 1u;
            }
            mmio_write32(bar, queue->cq_head_db, queue->cq_head);
            uint16_t status_code = (uint16_t)((completion.status >> 1) & 0x7ffu);
            if (completion.command_id != cid || status_code != 0) {
                fprintf(
                    stderr,
                    "nvme provider: command 0x%02x failed cid=%u got=%u status=0x%04x result=0x%08x\n",
                    command->cdw0 & 0xffu,
                    cid,
                    completion.command_id,
                    completion.status,
                    completion.result);
                return 1;
            }
            if (out_completion != NULL) {
                *out_completion = completion;
            }
            return 0;
        }
    }
    fprintf(stderr, "nvme provider: command 0x%02x completion timeout\n", command->cdw0 & 0xffu);
    return 1;
}

static int nvme_identify(void *bar, nvme_queue_t *admin_queue, uint32_t nsid, uint32_t cns, uint64_t dma_addr)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_IDENTIFY;
    command.nsid = nsid;
    command.prp1 = dma_addr;
    command.cdw10 = cns;
    return nvme_submit_and_wait(NULL, NULL, NULL, bar, admin_queue, &command, NULL);
}

static int nvme_create_io_cq(void *bar, nvme_queue_t *admin_queue, uint16_t qid, uint16_t depth, uint64_t dma_addr)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_CREATE_CQ;
    command.prp1 = dma_addr;
    command.cdw10 = (uint32_t)qid | ((uint32_t)(depth - 1u) << 16);
    command.cdw11 = 3u;
    return nvme_submit_and_wait(NULL, NULL, NULL, bar, admin_queue, &command, NULL);
}

static int nvme_create_io_sq(void *bar, nvme_queue_t *admin_queue, uint16_t qid, uint16_t cqid, uint16_t depth, uint64_t dma_addr)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_CREATE_SQ;
    command.prp1 = dma_addr;
    command.cdw10 = (uint32_t)qid | ((uint32_t)(depth - 1u) << 16);
    command.cdw11 = 1u | ((uint32_t)cqid << 16);
    return nvme_submit_and_wait(NULL, NULL, NULL, bar, admin_queue, &command, NULL);
}

static int nvme_rw_blocks(
    kb_linux_vfio_nvme_provider_t *provider,
    uint8_t opcode,
    uint64_t lba,
    uint32_t blocks,
    uint64_t dma_addr)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = opcode;
    command.nsid = provider->nsid;
    command.prp1 = dma_addr;
    command.cdw10 = (uint32_t)lba;
    command.cdw11 = (uint32_t)(lba >> 32);
    command.cdw12 = blocks - 1u;
    return nvme_submit_and_wait(
        provider->ops,
        provider->device,
        provider->io_irq,
        provider->bar.addr,
        &provider->io_queue,
        &command,
        NULL);
}

static int disable_controller(void *bar)
{
    uint32_t cc = mmio_read32(bar, NVME_REG_CC);
    if ((cc & 1u) == 0u) {
        return 0;
    }
    mmio_write32(bar, NVME_REG_CC, cc & ~1u);
    if (wait_csts_ready(bar, 0) != 0) {
        fprintf(stderr, "nvme provider: controller disable timeout\n");
        return 1;
    }
    return 0;
}

static int enable_controller(
    void *bar,
    uint32_t mps_min,
    uint64_t admin_sq_dma,
    uint64_t admin_cq_dma,
    nvme_queue_t *admin_queue,
    void *admin_sq,
    void *admin_cq,
    uint32_t doorbell_stride)
{
    mmio_write32(bar, NVME_REG_AQA, (NVME_ADMIN_QUEUE_DEPTH - 1u) | ((NVME_ADMIN_QUEUE_DEPTH - 1u) << 16));
    mmio_write64(bar, NVME_REG_ASQ, admin_sq_dma);
    mmio_write64(bar, NVME_REG_ACQ, admin_cq_dma);
    uint32_t cc = 1u | (mps_min << 7) | (6u << 16) | (4u << 20);
    mmio_write32(bar, NVME_REG_CC, cc);
    if (wait_csts_ready(bar, 1) != 0) {
        fprintf(stderr, "nvme provider: controller enable timeout\n");
        return 1;
    }
    nvme_queue_init(admin_queue, 0, NVME_ADMIN_QUEUE_DEPTH, doorbell_stride, admin_sq, admin_cq);
    return 0;
}

static int create_io_queue_pair(kb_linux_vfio_nvme_provider_t *provider)
{
    memset(provider->io_cq.cpu_addr, 0, NVME_IO_QUEUE_DEPTH * sizeof(nvme_completion_t));
    memset(provider->io_sq.cpu_addr, 0, NVME_IO_QUEUE_DEPTH * sizeof(nvme_command_t));
    if (nvme_create_io_cq(provider->bar.addr, &provider->admin_queue, 1, NVME_IO_QUEUE_DEPTH, provider->io_cq.dma_addr) != 0) {
        return 1;
    }
    if (nvme_create_io_sq(provider->bar.addr, &provider->admin_queue, 1, 1, NVME_IO_QUEUE_DEPTH, provider->io_sq.dma_addr) != 0) {
        return 1;
    }
    nvme_queue_init(
        &provider->io_queue,
        1,
        NVME_IO_QUEUE_DEPTH,
        provider->doorbell_stride,
        provider->io_sq.cpu_addr,
        provider->io_cq.cpu_addr);
    return 0;
}

static int nvme_provider_io(void *ctx, uint64_t sector, void *buffer, size_t byte_count, uint8_t opcode)
{
    kb_linux_vfio_nvme_provider_t *provider = (kb_linux_vfio_nvme_provider_t *)ctx;
    if (provider == NULL || buffer == NULL || byte_count == 0 || byte_count > NVME_PROVIDER_MAX_IO ||
        provider->lba_size != 512u || (byte_count % provider->lba_size) != 0)
    {
        return -22;
    }
    uint32_t blocks = (uint32_t)(byte_count / provider->lba_size);
    if (sector + blocks > provider->nsze) {
        return -34;
    }
    if (opcode == NVME_CMD_WRITE) {
        memcpy(provider->io_buffer.cpu_addr, buffer, byte_count);
    } else {
        memset(provider->io_buffer.cpu_addr, 0, byte_count);
    }
    if (nvme_rw_blocks(provider, opcode, sector, blocks, provider->io_buffer.dma_addr) != 0) {
        return -5;
    }
    if (opcode == NVME_CMD_READ) {
        memcpy(buffer, provider->io_buffer.cpu_addr, byte_count);
    }
    return 0;
}

static int provider_disk_read(void *ctx, uint64_t sector, void *buffer, size_t byte_count)
{
    return nvme_provider_io(ctx, sector, buffer, byte_count, NVME_CMD_READ);
}

static int provider_disk_write(void *ctx, uint64_t sector, const void *buffer, size_t byte_count)
{
    return nvme_provider_io(ctx, sector, (void *)buffer, byte_count, NVME_CMD_WRITE);
}

static int dma_alloc_zero(
    kb_linux_vfio_nvme_provider_t *provider,
    uint64_t size,
    uint64_t alignment,
    kb_dma_buffer_t *out_buffer)
{
    kb_status_t status = provider->ops->dma_alloc(provider->device, size, alignment, KB_DMA_BIDIRECTIONAL, out_buffer);
    if (status != KB_OK) {
        fprintf(stderr, "nvme provider: dma_alloc size=%llu failed status=%d\n", (unsigned long long)size, status);
        return 1;
    }
    memset(out_buffer->cpu_addr, 0, (size_t)out_buffer->size);
    return 0;
}

static int setup_block_disk(kb_linux_vfio_nvme_provider_t *provider)
{
    provider->block_queue = kb_block_subsystem_queue_alloc(NULL);
    provider->disk = kb_block_subsystem_disk_alloc();
    provider->part0 = kb_block_subsystem_block_device_alloc();
    if (provider->block_queue == NULL || provider->disk == NULL || provider->part0 == NULL) {
        return 1;
    }
    if (kb_block_subsystem_disk_attach(provider->disk, provider->block_queue, provider->part0) != 0) {
        return 1;
    }
    kb_block_subsystem_disk_set_capacity(provider->disk, provider->nsze);
    kb_block_subsystem_disk_set_io(provider->disk, provider, provider_disk_read, provider_disk_write);
    return kb_block_subsystem_disk_register(NULL, provider->disk, NULL) == 0 ? 0 : 1;
}

kb_status_t kb_linux_vfio_nvme_provider_create(const char *bdf, kb_linux_vfio_nvme_provider_t **out_provider)
{
    if (bdf == NULL || out_provider == NULL) {
        return KB_ERR_INVALID;
    }
    *out_provider = NULL;
    kb_linux_vfio_nvme_provider_t *provider = calloc(1, sizeof(*provider));
    if (provider == NULL) {
        return KB_ERR_NOMEM;
    }

    kb_status_t status = kb_linux_vfio_device_create(bdf, &provider->backend);
    if (status != KB_OK) {
        fprintf(stderr, "nvme provider: vfio backend create failed status=%d\n", status);
        free(provider);
        return status;
    }
    provider->ops = kb_device_backend_get_ops(provider->backend);
    status = provider->ops->device_at(provider->backend, 0, &provider->device);
    if (status != KB_OK || provider->device == NULL) {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return status == KB_OK ? KB_ERR_IO : status;
    }

    kb_pci_id_t pci_id;
    memset(&pci_id, 0, sizeof(pci_id));
    if (provider->ops->device_pci_id(provider->device, &pci_id) == KB_OK) {
        printf(
            "nvme vfio: pci=%04x:%04x class=%02x%02x%02x\n",
            pci_id.vendor_id,
            pci_id.device_id,
            pci_id.class_code,
            pci_id.subclass,
            pci_id.prog_if);
    }
    if (enable_pci_memory_and_bus_master(provider->ops, provider->device) != 0 ||
        provider->ops->map_bar(provider->device, 0, &provider->bar) != KB_OK ||
        provider->bar.addr == NULL)
    {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return KB_ERR_IO;
    }

    uint64_t cap = mmio_read64(provider->bar.addr, NVME_REG_CAP);
    uint32_t version = mmio_read32(provider->bar.addr, NVME_REG_VS);
    provider->doorbell_stride = nvme_doorbell_stride(cap);
    provider->mps_min = nvme_mps_min(cap);
    provider->page_size = 1ull << (12u + provider->mps_min);
    printf(
        "nvme vfio: cap=0x%016llx vs=0x%08x page=%llu doorbell=%u\n",
        (unsigned long long)cap,
        version,
        (unsigned long long)provider->page_size,
        provider->doorbell_stride);

    if (disable_controller(provider->bar.addr) != 0 ||
        dma_alloc_zero(provider, provider->page_size, provider->page_size, &provider->admin_sq) != 0 ||
        dma_alloc_zero(provider, provider->page_size, provider->page_size, &provider->admin_cq) != 0 ||
        dma_alloc_zero(provider, provider->page_size, provider->page_size, &provider->identify) != 0 ||
        enable_controller(
            provider->bar.addr,
            provider->mps_min,
            provider->admin_sq.dma_addr,
            provider->admin_cq.dma_addr,
            &provider->admin_queue,
            provider->admin_sq.cpu_addr,
            provider->admin_cq.cpu_addr,
            provider->doorbell_stride) != 0)
    {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return KB_ERR_IO;
    }

    if (nvme_identify(
            provider->bar.addr,
            &provider->admin_queue,
            0,
            NVME_IDENTIFY_NAMESPACE_LIST,
            provider->identify.dma_addr) != 0)
    {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return KB_ERR_IO;
    }
    provider->nsid = read_le32((const unsigned char *)provider->identify.cpu_addr);
    if (provider->nsid == 0) {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return KB_ERR_IO;
    }

    memset(provider->identify.cpu_addr, 0, (size_t)provider->identify.size);
    if (nvme_identify(
            provider->bar.addr,
            &provider->admin_queue,
            provider->nsid,
            NVME_IDENTIFY_NAMESPACE,
            provider->identify.dma_addr) != 0)
    {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return KB_ERR_IO;
    }
    const unsigned char *ns = provider->identify.cpu_addr;
    provider->nsze = read_le64(ns);
    uint8_t flbas = ns[26];
    uint8_t lbaf_index = flbas & 0x0fu;
    const unsigned char *lbaf = ns + 128 + ((size_t)lbaf_index * 4u);
    uint16_t metadata_size = read_le16(lbaf);
    uint8_t lbads = lbaf[2];
    provider->lba_size = 1u << lbads;
    printf(
        "nvme vfio: namespace nsid=%u nsze=%llu lba_size=%u metadata=%u\n",
        provider->nsid,
        (unsigned long long)provider->nsze,
        provider->lba_size,
        metadata_size);
    if (provider->nsze == 0 || provider->lba_size != 512u || metadata_size != 0) {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return KB_ERR_UNSUPPORTED;
    }

    status = provider->ops->irq_register(provider->device, 0, irq_handler, &provider->irq_state, &provider->io_irq);
    if (status != KB_OK ||
        dma_alloc_zero(provider, provider->page_size, provider->page_size, &provider->io_cq) != 0 ||
        dma_alloc_zero(provider, provider->page_size, provider->page_size, &provider->io_sq) != 0 ||
        dma_alloc_zero(provider, NVME_PROVIDER_MAX_IO, provider->page_size, &provider->io_buffer) != 0 ||
        create_io_queue_pair(provider) != 0 ||
        setup_block_disk(provider) != 0)
    {
        kb_linux_vfio_nvme_provider_destroy(provider);
        return status == KB_OK ? KB_ERR_IO : status;
    }

    printf("nvme vfio: capacity_sectors=%llu\n", (unsigned long long)provider->nsze);
    *out_provider = provider;
    return KB_OK;
}

void *kb_linux_vfio_nvme_provider_disk(kb_linux_vfio_nvme_provider_t *provider)
{
    return provider == NULL ? NULL : provider->disk;
}

void kb_linux_vfio_nvme_provider_destroy(kb_linux_vfio_nvme_provider_t *provider)
{
    if (provider == NULL) {
        return;
    }
    if (provider->disk != NULL) {
        kb_block_subsystem_disk_unregister(provider->disk);
    }
    if (provider->ops != NULL && provider->device != NULL) {
        if (provider->io_irq != NULL) {
            provider->ops->irq_unregister(provider->device, provider->io_irq);
        }
        if (provider->bar.addr != NULL) {
            (void)disable_controller(provider->bar.addr);
        }
        provider->ops->dma_free(provider->device, &provider->io_buffer);
        provider->ops->dma_free(provider->device, &provider->io_sq);
        provider->ops->dma_free(provider->device, &provider->io_cq);
        provider->ops->dma_free(provider->device, &provider->identify);
        provider->ops->dma_free(provider->device, &provider->admin_cq);
        provider->ops->dma_free(provider->device, &provider->admin_sq);
        if (provider->bar.addr != NULL) {
            provider->ops->unmap_bar(provider->device, &provider->bar);
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
