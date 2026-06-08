#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_backend kb_backend_t;
typedef struct kb_device kb_device_t;
typedef struct kb_irq kb_irq_t;

typedef enum kb_status {
    KB_OK = 0,
    KB_ERR_INVALID = -1,
    KB_ERR_NOT_FOUND = -2,
    KB_ERR_DENIED = -3,
    KB_ERR_NOMEM = -4,
    KB_ERR_IO = -5,
    KB_ERR_UNSUPPORTED = -6,
    KB_ERR_PCI_CONFIG = -7,
} kb_status_t;

typedef enum kb_dma_dir {
    KB_DMA_TO_DEVICE = 1,
    KB_DMA_FROM_DEVICE = 2,
    KB_DMA_BIDIRECTIONAL = 3,
} kb_dma_dir_t;

typedef struct kb_pci_id {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
} kb_pci_id_t;

typedef struct kb_pci_location {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} kb_pci_location_t;

typedef struct kb_mmio_region {
    void *addr;
    uint64_t size;
    uint64_t backend_phys;
    uint32_t flags;
} kb_mmio_region_t;

typedef struct kb_pci_bar_info {
    uint64_t start;
    uint64_t end;
    uint64_t size;
    uint64_t flags;
    int present;
} kb_pci_bar_info_t;

typedef struct kb_dma_buffer {
    void *cpu_addr;
    uint64_t dma_addr;
    uint64_t size;
    uint32_t flags;
} kb_dma_buffer_t;

typedef void (*kb_irq_handler_t)(void *ctx);

typedef struct kb_backend_ops {
    void (*destroy)(kb_backend_t *backend);

    kb_status_t (*device_count)(kb_backend_t *backend, size_t *out_count);
    kb_status_t (*device_at)(kb_backend_t *backend, size_t index, kb_device_t **out_device);

    kb_status_t (*device_pci_id)(kb_device_t *device, kb_pci_id_t *out_id);
    kb_status_t (*device_pci_location)(kb_device_t *device, kb_pci_location_t *out_location);

    kb_status_t (*pci_config_read)(kb_device_t *device, uint16_t offset, void *dst, size_t len);
    kb_status_t (*pci_config_write)(kb_device_t *device, uint16_t offset, const void *src, size_t len);
    kb_status_t (*pci_bar_info)(kb_device_t *device, unsigned bar_index, kb_pci_bar_info_t *out_info);

    kb_status_t (*map_bar)(kb_device_t *device, unsigned bar_index, kb_mmio_region_t *out_region);
    void (*unmap_bar)(kb_device_t *device, kb_mmio_region_t *region);

    kb_status_t (*dma_alloc)(
        kb_device_t *device,
        uint64_t size,
        uint64_t alignment,
        kb_dma_dir_t direction,
        kb_dma_buffer_t *out_buffer);
    void (*dma_free)(kb_device_t *device, kb_dma_buffer_t *buffer);

    kb_status_t (*dma_map)(
        kb_device_t *device,
        void *cpu_addr,
        uint64_t size,
        kb_dma_dir_t direction,
        uint64_t *out_dma_addr);
    void (*dma_unmap)(kb_device_t *device, uint64_t dma_addr, uint64_t size, kb_dma_dir_t direction);
    kb_status_t (*dma_set_mask)(kb_device_t *device, uint64_t mask, int coherent);

    kb_status_t (*irq_register)(
        kb_device_t *device,
        unsigned vector,
        kb_irq_handler_t handler,
        void *ctx,
        kb_irq_t **out_irq);
    void (*irq_unregister)(kb_device_t *device, kb_irq_t *irq);
    kb_status_t (*irq_wait)(kb_device_t *device, kb_irq_t *irq, uint64_t timeout_ns);

    uint64_t (*monotonic_ns)(kb_backend_t *backend);
    void (*log)(kb_backend_t *backend, int level, const char *message);
} kb_backend_ops_t;

const kb_backend_ops_t *kb_backend_get_ops(const kb_backend_t *backend);
void kb_backend_destroy(kb_backend_t *backend);

#ifdef __cplusplus
}
#endif
