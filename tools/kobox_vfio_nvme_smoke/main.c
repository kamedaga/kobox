#include "kobox/backend.h"
#include "kobox/backend_linux_vfio.h"

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

typedef struct nvme_io_case {
    uint64_t lba;
    uint32_t blocks;
    uint32_t pattern;
} nvme_io_case_t;

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

static uint32_t nvme_doorbell_stride(uint64_t cap)
{
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xfu);
    return 4u << dstrd;
}

static uint32_t nvme_mps_min(uint64_t cap)
{
    return (uint32_t)((cap >> 48) & 0xfu);
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

static void trim_ascii(char *dst, size_t dst_size, const unsigned char *src, size_t src_size)
{
    size_t end = src_size;
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\0')) {
        end--;
    }
    if (end >= dst_size) {
        end = dst_size - 1;
    }
    memcpy(dst, src, end);
    dst[end] = '\0';
}

static int enable_pci_memory_and_bus_master(const kb_backend_ops_t *ops, kb_device_t *device)
{
    uint16_t command = 0;
    kb_status_t status = ops->pci_config_read(device, 0x04, &command, sizeof(command));
    if (status != KB_OK) {
        fprintf(stderr, "pci command read failed: %d\n", status);
        return 1;
    }

    command |= 0x0006u;
    status = ops->pci_config_write(device, 0x04, &command, sizeof(command));
    if (status != KB_OK) {
        fprintf(stderr, "pci command write failed: %d\n", status);
        return 1;
    }
    return 0;
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
    const kb_backend_ops_t *ops,
    kb_device_t *device,
    kb_irq_t *irq,
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
            fprintf(stderr, "nvme command 0x%02x irq_wait failed: %d\n", command->cdw0 & 0xffu, status);
            return 1;
        }
    }

    nvme_completion_t completion;
    memset(&completion, 0, sizeof(completion));
    int completed = 0;
    for (unsigned i = 0; i < 10000000; i++) {
        completion = queue->cq[queue->cq_head];
        if ((completion.status & 1u) == queue->phase) {
            completed = 1;
            break;
        }
    }
    if (!completed) {
        fprintf(stderr, "nvme command 0x%02x completion timeout\n", command->cdw0 & 0xffu);
        return 1;
    }

    queue->cq_head++;
    if (queue->cq_head == queue->depth) {
        queue->cq_head = 0;
        queue->phase ^= 1u;
    }
    mmio_write32(bar, queue->cq_head_db, queue->cq_head);

    if (out_completion != NULL) {
        *out_completion = completion;
    }

    uint16_t status_code = (uint16_t)((completion.status >> 1) & 0x7ffu);
    if (completion.command_id != cid || status_code != 0) {
        fprintf(
            stderr,
            "nvme command 0x%02x failed cid=%u got=%u status=0x%04x result=0x%08x\n",
            command->cdw0 & 0xffu,
            cid,
            completion.command_id,
            completion.status,
            completion.result);
        return 1;
    }
    return 0;
}

static int nvme_identify(
    void *bar,
    nvme_queue_t *admin_queue,
    uint32_t nsid,
    uint32_t cns,
    uint64_t dma_addr,
    nvme_completion_t *out_completion)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_IDENTIFY;
    command.nsid = nsid;
    command.prp1 = dma_addr;
    command.cdw10 = cns;
    return nvme_submit_and_wait(NULL, NULL, NULL, bar, admin_queue, &command, out_completion);
}

static int nvme_create_io_cq(
    void *bar,
    nvme_queue_t *admin_queue,
    uint16_t qid,
    uint16_t depth,
    uint16_t interrupt_vector,
    uint64_t dma_addr)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = NVME_ADMIN_CREATE_CQ;
    command.prp1 = dma_addr;
    command.cdw10 = (uint32_t)qid | ((uint32_t)(depth - 1u) << 16);
    command.cdw11 = 3u | ((uint32_t)interrupt_vector << 16);
    return nvme_submit_and_wait(NULL, NULL, NULL, bar, admin_queue, &command, NULL);
}

static int nvme_create_io_sq(
    void *bar,
    nvme_queue_t *admin_queue,
    uint16_t qid,
    uint16_t cqid,
    uint16_t depth,
    uint64_t dma_addr)
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
    const kb_backend_ops_t *ops,
    kb_device_t *device,
    kb_irq_t *irq,
    void *bar,
    nvme_queue_t *io_queue,
    uint8_t opcode,
    uint32_t nsid,
    uint64_t lba,
    uint32_t blocks,
    uint64_t dma_addr)
{
    nvme_command_t command;
    memset(&command, 0, sizeof(command));
    command.cdw0 = opcode;
    command.nsid = nsid;
    command.prp1 = dma_addr;
    command.cdw10 = (uint32_t)lba;
    command.cdw11 = (uint32_t)(lba >> 32);
    command.cdw12 = blocks - 1u;
    return nvme_submit_and_wait(ops, device, irq, bar, io_queue, &command, NULL);
}

static void fill_pattern(unsigned char *dst, uint32_t size, uint64_t lba, uint32_t blocks, uint32_t pattern)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t value = (uint32_t)(lba * 17u) ^ (blocks * 29u) ^ (pattern * 53u) ^ (i * 31u);
        switch (pattern % 4u) {
        case 0:
            dst[i] = (unsigned char)value;
            break;
        case 1:
            dst[i] = (unsigned char)~value;
            break;
        case 2:
            dst[i] = (unsigned char)((i & 1u) != 0u ? 0xa5u : 0x5au);
            break;
        default:
            dst[i] = (unsigned char)((value >> ((i % 4u) * 8u)) & 0xffu);
            break;
        }
    }
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
        fprintf(stderr, "controller enable timeout\n");
        return 1;
    }

    nvme_queue_init(admin_queue, 0, NVME_ADMIN_QUEUE_DEPTH, doorbell_stride, admin_sq, admin_cq);
    return 0;
}

static int disable_controller(void *bar)
{
    uint32_t cc = mmio_read32(bar, NVME_REG_CC);
    if ((cc & 1u) == 0u) {
        return 0;
    }
    mmio_write32(bar, NVME_REG_CC, cc & ~1u);
    if (wait_csts_ready(bar, 0) != 0) {
        fprintf(stderr, "controller disable timeout\n");
        return 1;
    }
    return 0;
}

static int create_io_queue_pair(
    void *bar,
    nvme_queue_t *admin_queue,
    uint32_t doorbell_stride,
    void *io_sq_cpu,
    uint64_t io_sq_dma,
    void *io_cq_cpu,
    uint64_t io_cq_dma,
    nvme_queue_t *io_queue)
{
    memset(io_cq_cpu, 0, NVME_IO_QUEUE_DEPTH * sizeof(nvme_completion_t));
    memset(io_sq_cpu, 0, NVME_IO_QUEUE_DEPTH * sizeof(nvme_command_t));
    if (nvme_create_io_cq(bar, admin_queue, 1, NVME_IO_QUEUE_DEPTH, 0, io_cq_dma) != 0) {
        return 1;
    }
    if (nvme_create_io_sq(bar, admin_queue, 1, 1, NVME_IO_QUEUE_DEPTH, io_sq_dma) != 0) {
        return 1;
    }
    nvme_queue_init(io_queue, 1, NVME_IO_QUEUE_DEPTH, doorbell_stride, io_sq_cpu, io_cq_cpu);
    return 0;
}

static int run_nvme_smoke(const char *bdf)
{
    int exit_code = 1;
    kb_backend_t *backend = NULL;
    kb_mmio_region_t bar;
    kb_dma_buffer_t admin_sq;
    kb_dma_buffer_t admin_cq;
    kb_dma_buffer_t identify;
    kb_dma_buffer_t io_sq;
    kb_dma_buffer_t io_cq;
    kb_dma_buffer_t write_buffer;
    kb_dma_buffer_t read_buffer;
    kb_irq_t *io_irq = NULL;
    irq_state_t irq_state;
    memset(&bar, 0, sizeof(bar));
    memset(&admin_sq, 0, sizeof(admin_sq));
    memset(&admin_cq, 0, sizeof(admin_cq));
    memset(&identify, 0, sizeof(identify));
    memset(&io_sq, 0, sizeof(io_sq));
    memset(&io_cq, 0, sizeof(io_cq));
    memset(&write_buffer, 0, sizeof(write_buffer));
    memset(&read_buffer, 0, sizeof(read_buffer));
    memset(&irq_state, 0, sizeof(irq_state));

    kb_status_t status = kb_linux_vfio_create(bdf, &backend);
    if (status != KB_OK) {
        fprintf(stderr, "vfio create failed: %d\n", status);
        return 1;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    kb_device_t *device = NULL;
    status = ops->device_at(backend, 0, &device);
    if (status != KB_OK) {
        fprintf(stderr, "device_at failed: %d\n", status);
        goto cleanup;
    }

    kb_pci_id_t pci_id;
    memset(&pci_id, 0, sizeof(pci_id));
    status = ops->device_pci_id(device, &pci_id);
    if (status != KB_OK) {
        fprintf(stderr, "device_pci_id failed: %d\n", status);
        goto cleanup;
    }
    printf(
        "nvme pci=%04x:%04x class=%02x%02x%02x\n",
        pci_id.vendor_id,
        pci_id.device_id,
        pci_id.class_code,
        pci_id.subclass,
        pci_id.prog_if);

    if (enable_pci_memory_and_bus_master(ops, device) != 0) {
        goto cleanup;
    }

    status = ops->map_bar(device, 0, &bar);
    if (status != KB_OK) {
        fprintf(stderr, "map_bar failed: %d\n", status);
        goto cleanup;
    }

    uint64_t cap = mmio_read64(bar.addr, NVME_REG_CAP);
    uint32_t version = mmio_read32(bar.addr, NVME_REG_VS);
    uint32_t csts = mmio_read32(bar.addr, NVME_REG_CSTS);
    uint32_t stride = nvme_doorbell_stride(cap);
    uint32_t mps_min = nvme_mps_min(cap);
    uint64_t page_size = 1ull << (12u + mps_min);
    printf(
        "nvme cap=0x%016llx vs=0x%08x csts=0x%08x page=%llu doorbell=%u\n",
        (unsigned long long)cap,
        version,
        csts,
        (unsigned long long)page_size,
        stride);

    if (disable_controller(bar.addr) != 0) {
        goto cleanup;
    }

    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &admin_sq);
    if (status != KB_OK) {
        fprintf(stderr, "admin sq dma_alloc failed: %d\n", status);
        goto cleanup;
    }
    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &admin_cq);
    if (status != KB_OK) {
        fprintf(stderr, "admin cq dma_alloc failed: %d\n", status);
        goto cleanup;
    }
    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &identify);
    if (status != KB_OK) {
        fprintf(stderr, "identify dma_alloc failed: %d\n", status);
        goto cleanup;
    }

    memset(admin_sq.cpu_addr, 0, (size_t)admin_sq.size);
    memset(admin_cq.cpu_addr, 0, (size_t)admin_cq.size);
    memset(identify.cpu_addr, 0, (size_t)identify.size);

    nvme_queue_t admin_queue;
    if (enable_controller(
            bar.addr,
            mps_min,
            admin_sq.dma_addr,
            admin_cq.dma_addr,
            &admin_queue,
            admin_sq.cpu_addr,
            admin_cq.cpu_addr,
            stride) != 0)
    {
        goto cleanup;
    }

    nvme_completion_t completion;
    memset(identify.cpu_addr, 0, (size_t)identify.size);
    if (nvme_identify(bar.addr, &admin_queue, 0, NVME_IDENTIFY_CONTROLLER, identify.dma_addr, &completion) != 0) {
        goto cleanup;
    }
    printf(
        "nvme identify-controller cid=%u sqid=%u status=0x%04x result=0x%08x\n",
        completion.command_id,
        completion.sq_id,
        completion.status,
        completion.result);

    char serial[21];
    char model[41];
    char firmware[9];
    const unsigned char *id = identify.cpu_addr;
    trim_ascii(serial, sizeof(serial), id + 4, 20);
    trim_ascii(model, sizeof(model), id + 24, 40);
    trim_ascii(firmware, sizeof(firmware), id + 64, 8);
    printf("nvme controller serial=\"%s\" model=\"%s\" firmware=\"%s\"\n", serial, model, firmware);

    memset(identify.cpu_addr, 0, (size_t)identify.size);
    if (nvme_identify(bar.addr, &admin_queue, 0, NVME_IDENTIFY_NAMESPACE_LIST, identify.dma_addr, NULL) != 0) {
        goto cleanup;
    }
    uint32_t nsid = read_le32((const unsigned char *)identify.cpu_addr);
    if (nsid == 0) {
        fprintf(stderr, "nvme namespace list is empty\n");
        goto cleanup;
    }
    printf("nvme namespace-list first-nsid=%u\n", nsid);

    memset(identify.cpu_addr, 0, (size_t)identify.size);
    if (nvme_identify(bar.addr, &admin_queue, nsid, NVME_IDENTIFY_NAMESPACE, identify.dma_addr, NULL) != 0) {
        goto cleanup;
    }
    const unsigned char *ns = identify.cpu_addr;
    uint64_t nsze = read_le64(ns);
    uint64_t ncap = read_le64(ns + 8);
    uint8_t flbas = ns[26];
    uint8_t lbaf_index = flbas & 0x0fu;
    const unsigned char *lbaf = ns + 128 + ((size_t)lbaf_index * 4u);
    uint16_t metadata_size = read_le16(lbaf);
    uint8_t lbads = lbaf[2];
    uint32_t lba_size = 1u << lbads;
    printf(
        "nvme namespace nsid=%u nsze=%llu ncap=%llu flbas=%u lba_size=%u metadata=%u\n",
        nsid,
        (unsigned long long)nsze,
        (unsigned long long)ncap,
        lbaf_index,
        lba_size,
        metadata_size);
    if (nsze == 0 || lba_size == 0 || lba_size > page_size) {
        fprintf(stderr, "unsupported namespace geometry\n");
        goto cleanup;
    }

    status = ops->irq_register(device, 0, irq_handler, &irq_state, &io_irq);
    if (status != KB_OK) {
        fprintf(stderr, "msix irq_register failed: %d\n", status);
        goto cleanup;
    }
    printf("nvme msix vector=0 registered\n");

    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &io_cq);
    if (status != KB_OK) {
        fprintf(stderr, "io cq dma_alloc failed: %d\n", status);
        goto cleanup;
    }
    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &io_sq);
    if (status != KB_OK) {
        fprintf(stderr, "io sq dma_alloc failed: %d\n", status);
        goto cleanup;
    }
    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &write_buffer);
    if (status != KB_OK) {
        fprintf(stderr, "write buffer dma_alloc failed: %d\n", status);
        goto cleanup;
    }
    status = ops->dma_alloc(device, page_size, page_size, KB_DMA_BIDIRECTIONAL, &read_buffer);
    if (status != KB_OK) {
        fprintf(stderr, "read buffer dma_alloc failed: %d\n", status);
        goto cleanup;
    }

    if (lba_size * 4u > page_size) {
        fprintf(stderr, "unsupported lba size for multi-block smoke\n");
        goto cleanup;
    }

    nvme_queue_t io_queue;
    if (create_io_queue_pair(
            bar.addr,
            &admin_queue,
            stride,
            io_sq.cpu_addr,
            io_sq.dma_addr,
            io_cq.cpu_addr,
            io_cq.dma_addr,
            &io_queue) != 0)
    {
        goto cleanup;
    }

    nvme_io_case_t cases[] = {
        {.lba = 7, .blocks = 1, .pattern = 0},
        {.lba = nsze / 3u, .blocks = 2, .pattern = 1},
        {.lba = nsze > 32u ? nsze - 17u : 11u, .blocks = 4, .pattern = 2},
    };
    const size_t case_count = sizeof(cases) / sizeof(cases[0]);

    unsigned char *write_bytes = write_buffer.cpu_addr;
    unsigned char *read_bytes = read_buffer.cpu_addr;
    for (size_t i = 0; i < case_count; i++) {
        if (cases[i].lba + cases[i].blocks > nsze) {
            fprintf(stderr, "nvme smoke case out of range lba=%llu blocks=%u\n", (unsigned long long)cases[i].lba, cases[i].blocks);
            goto cleanup;
        }
        uint32_t bytes = cases[i].blocks * lba_size;
        memset(write_bytes, 0, (size_t)write_buffer.size);
        fill_pattern(write_bytes, bytes, cases[i].lba, cases[i].blocks, cases[i].pattern);
        if (nvme_rw_blocks(
                ops,
                device,
                io_irq,
                bar.addr,
                &io_queue,
                NVME_CMD_WRITE,
                nsid,
                cases[i].lba,
                cases[i].blocks,
                write_buffer.dma_addr) != 0)
        {
            goto cleanup;
        }
        printf(
            "nvme write lba=%llu blocks=%u bytes=%u irq_count=%u\n",
            (unsigned long long)cases[i].lba,
            cases[i].blocks,
            bytes,
            irq_state.count);
    }

    if (disable_controller(bar.addr) != 0) {
        goto cleanup;
    }
    memset(admin_sq.cpu_addr, 0, (size_t)admin_sq.size);
    memset(admin_cq.cpu_addr, 0, (size_t)admin_cq.size);
    memset(io_sq.cpu_addr, 0, (size_t)io_sq.size);
    memset(io_cq.cpu_addr, 0, (size_t)io_cq.size);
    if (enable_controller(
            bar.addr,
            mps_min,
            admin_sq.dma_addr,
            admin_cq.dma_addr,
            &admin_queue,
            admin_sq.cpu_addr,
            admin_cq.cpu_addr,
            stride) != 0)
    {
        goto cleanup;
    }
    if (create_io_queue_pair(
            bar.addr,
            &admin_queue,
            stride,
            io_sq.cpu_addr,
            io_sq.dma_addr,
            io_cq.cpu_addr,
            io_cq.dma_addr,
            &io_queue) != 0)
    {
        goto cleanup;
    }
    printf("nvme controller reset complete, reading data back\n");

    for (size_t i = 0; i < case_count; i++) {
        uint32_t bytes = cases[i].blocks * lba_size;
        memset(write_bytes, 0, (size_t)write_buffer.size);
        memset(read_bytes, 0, (size_t)read_buffer.size);
        fill_pattern(write_bytes, bytes, cases[i].lba, cases[i].blocks, cases[i].pattern);
        if (nvme_rw_blocks(
                ops,
                device,
                io_irq,
                bar.addr,
                &io_queue,
                NVME_CMD_READ,
                nsid,
                cases[i].lba,
                cases[i].blocks,
                read_buffer.dma_addr) != 0)
        {
            goto cleanup;
        }
        if (memcmp(write_bytes, read_bytes, bytes) != 0) {
            fprintf(
                stderr,
                "nvme read/write compare failed lba=%llu blocks=%u\n",
                (unsigned long long)cases[i].lba,
                cases[i].blocks);
            goto cleanup;
        }
        printf(
            "nvme read-after-reset lba=%llu blocks=%u bytes=%u compare=ok irq_count=%u\n",
            (unsigned long long)cases[i].lba,
            cases[i].blocks,
            bytes,
            irq_state.count);
    }

    exit_code = 0;

cleanup:
    if (io_irq != NULL) {
        ops->irq_unregister(device, io_irq);
    }
    if (bar.addr != NULL) {
        (void)disable_controller(bar.addr);
    }
    if (read_buffer.cpu_addr != NULL) {
        ops->dma_free(device, &read_buffer);
    }
    if (write_buffer.cpu_addr != NULL) {
        ops->dma_free(device, &write_buffer);
    }
    if (io_sq.cpu_addr != NULL) {
        ops->dma_free(device, &io_sq);
    }
    if (io_cq.cpu_addr != NULL) {
        ops->dma_free(device, &io_cq);
    }
    if (identify.cpu_addr != NULL) {
        ops->dma_free(device, &identify);
    }
    if (admin_cq.cpu_addr != NULL) {
        ops->dma_free(device, &admin_cq);
    }
    if (admin_sq.cpu_addr != NULL) {
        ops->dma_free(device, &admin_sq);
    }
    if (bar.addr != NULL) {
        ops->unmap_bar(device, &bar);
    }
    kb_backend_destroy(backend);
    return exit_code;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: kobox-vfio-nvme-smoke <BDF>\n");
        return 1;
    }
    return run_nvme_smoke(argv[1]);
}
