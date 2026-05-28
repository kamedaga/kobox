#include "kobox/backend.h"
#include "kobox/backend_linux_vfio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    EDU_REG_ID = 0x00,
    EDU_REG_LIVENESS = 0x04,
    EDU_REG_IRQ_STATUS = 0x24,
    EDU_REG_IRQ_RAISE = 0x60,
    EDU_REG_IRQ_ACK = 0x64,
    EDU_REG_DMA_SRC = 0x80,
    EDU_REG_DMA_DST = 0x88,
    EDU_REG_DMA_COUNT = 0x90,
    EDU_REG_DMA_CMD = 0x98,
    EDU_DEVICE_DMA_BUFFER = 0x40000,
    EDU_DMA_SIZE = 128,
};

typedef struct irq_state {
    int fired;
} irq_state_t;

static void irq_handler(void *ctx)
{
    irq_state_t *state = ctx;
    state->fired = 1;
}

static uint32_t mmio_read32(void *base, size_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((unsigned char *)base + offset);
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

static int wait_dma_idle(void *bar)
{
    for (unsigned i = 0; i < 1000000; i++) {
        if ((mmio_read32(bar, EDU_REG_DMA_CMD) & 1u) == 0) {
            return 0;
        }
    }
    return 1;
}

static int run_edu_smoke(const char *bdf)
{
    kb_backend_t *backend = NULL;
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
        kb_backend_destroy(backend);
        return 1;
    }

    uint16_t command = 0;
    status = ops->pci_config_read(device, 0x04, &command, sizeof(command));
    if (status != KB_OK) {
        fprintf(stderr, "pci command read failed: %d\n", status);
        kb_backend_destroy(backend);
        return 1;
    }
    command |= 0x0006u;
    status = ops->pci_config_write(device, 0x04, &command, sizeof(command));
    if (status != KB_OK) {
        fprintf(stderr, "pci command write failed: %d\n", status);
        kb_backend_destroy(backend);
        return 1;
    }

    kb_mmio_region_t bar;
    memset(&bar, 0, sizeof(bar));
    status = ops->map_bar(device, 0, &bar);
    if (status != KB_OK) {
        fprintf(stderr, "map_bar failed: %d\n", status);
        kb_backend_destroy(backend);
        return 1;
    }

    uint32_t id = mmio_read32(bar.addr, EDU_REG_ID);
    printf("edu id=0x%08x\n", id);
    mmio_write32(bar.addr, EDU_REG_LIVENESS, 0x5a5aa5a5u);
    uint32_t liveness = mmio_read32(bar.addr, EDU_REG_LIVENESS);
    if (liveness != ~0x5a5aa5a5u) {
        fprintf(stderr, "liveness failed: 0x%08x\n", liveness);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }

    irq_state_t irq_state;
    memset(&irq_state, 0, sizeof(irq_state));
    kb_irq_t *irq = NULL;
    status = ops->irq_register(device, 0, irq_handler, &irq_state, &irq);
    if (status != KB_OK) {
        fprintf(stderr, "irq_register failed: %d\n", status);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }
    mmio_write32(bar.addr, EDU_REG_IRQ_RAISE, 0x55u);
    status = ops->irq_wait(device, irq, 1000000000ull);
    if (status != KB_OK || !irq_state.fired) {
        fprintf(stderr, "irq_wait failed: %d fired=%d\n", status, irq_state.fired);
        ops->irq_unregister(device, irq);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }
    uint32_t irq_status = mmio_read32(bar.addr, EDU_REG_IRQ_STATUS);
    mmio_write32(bar.addr, EDU_REG_IRQ_ACK, irq_status);
    ops->irq_unregister(device, irq);
    printf("edu irq status=0x%08x\n", irq_status);

    kb_dma_buffer_t dma;
    memset(&dma, 0, sizeof(dma));
    status = ops->dma_alloc(device, 4096, 4096, KB_DMA_BIDIRECTIONAL, &dma);
    if (status != KB_OK) {
        fprintf(stderr, "dma_alloc failed: %d\n", status);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }
    unsigned char *bytes = dma.cpu_addr;
    for (size_t i = 0; i < EDU_DMA_SIZE; i++) {
        bytes[i] = (unsigned char)(0x30u + i);
        bytes[EDU_DMA_SIZE + i] = 0;
    }

    mmio_write64(bar.addr, EDU_REG_DMA_SRC, dma.dma_addr);
    mmio_write64(bar.addr, EDU_REG_DMA_DST, EDU_DEVICE_DMA_BUFFER);
    mmio_write64(bar.addr, EDU_REG_DMA_COUNT, EDU_DMA_SIZE);
    mmio_write32(bar.addr, EDU_REG_DMA_CMD, 1);
    if (wait_dma_idle(bar.addr) != 0) {
        fprintf(stderr, "dma ram-to-device timeout\n");
        ops->dma_free(device, &dma);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }

    mmio_write64(bar.addr, EDU_REG_DMA_SRC, EDU_DEVICE_DMA_BUFFER);
    mmio_write64(bar.addr, EDU_REG_DMA_DST, dma.dma_addr + EDU_DMA_SIZE);
    mmio_write64(bar.addr, EDU_REG_DMA_COUNT, EDU_DMA_SIZE);
    mmio_write32(bar.addr, EDU_REG_DMA_CMD, 3);
    if (wait_dma_idle(bar.addr) != 0) {
        fprintf(stderr, "dma device-to-ram timeout\n");
        ops->dma_free(device, &dma);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }

    if (memcmp(bytes, bytes + EDU_DMA_SIZE, EDU_DMA_SIZE) != 0) {
        fprintf(stderr, "dma compare failed\n");
        ops->dma_free(device, &dma);
        ops->unmap_bar(device, &bar);
        kb_backend_destroy(backend);
        return 1;
    }
    printf("edu dma bytes=%u\n", (unsigned)EDU_DMA_SIZE);

    ops->dma_free(device, &dma);
    ops->unmap_bar(device, &bar);
    kb_backend_destroy(backend);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: kobox-vfio-edu-smoke <BDF>\n");
        return 1;
    }
    return run_edu_smoke(argv[1]);
}
