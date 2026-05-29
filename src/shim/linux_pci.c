#include "kobox/shim.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

kb_backend_t *kb_shim_current_backend(void);

typedef struct shim_pci_driver {
    const char *name;
    const void *id_table;
    int (*probe)(void *dev, const void *id);
    void (*remove)(void *dev);
} shim_pci_driver_t;

typedef struct linux_pci_device_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class;
    uint32_t class_mask;
    uintptr_t driver_data;
} linux_pci_device_id_t;

#define KB_PCI_ANY_ID UINT32_C(0xffffffff)

typedef struct shim_pci_binding {
    shim_pci_driver_t *driver;
    kb_device_t *device;
    unsigned char pci_dev_storage[4096];
    uint64_t dma_mask_storage;
    int probed;
} shim_pci_binding_t;

static shim_pci_binding_t binding;

enum {
    KB_PCI_COMMAND_OFFSET = 0x04,
    KB_PCI_COMMAND_MEMORY = 0x0002,
    KB_PCI_COMMAND_MASTER = 0x0004,
    KB_PCI_CLASS_STORAGE = 0x01,
    KB_PCI_SUBCLASS_NVME = 0x08,
    KB_PCI_PROGIF_NVME = 0x02,
    KB_NVME_REG_CC = 0x14,
    KB_NVME_REG_CSTS = 0x1c,

    KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET = 0x0c8,
    KB_LINUX_6_8_PCI_DEV_ENABLE_CNT_OFFSET = 0x0c4,
    KB_LINUX_6_8_PCI_DEV_DMA_MASK_OFFSET = 0x328,
    KB_LINUX_6_8_PCI_RESOURCE0_START_OFFSET = 0x3d0,
    KB_LINUX_6_8_PCI_RESOURCE0_END_OFFSET = 0x3d8,
    KB_LINUX_6_8_DEVICE_POWER_USAGE_COUNT_OFFSET = 0x794,
};

static kb_status_t first_device(kb_backend_t *backend, kb_device_t **out_device)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_at == NULL) {
        return KB_ERR_INVALID;
    }
    return ops->device_at(backend, 0, out_device);
}

static kb_status_t update_pci_command(uint16_t set_bits, uint16_t clear_bits)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return KB_ERR_NOT_FOUND;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->pci_config_read == NULL || ops->pci_config_write == NULL) {
        return KB_ERR_UNSUPPORTED;
    }

    uint16_t command = 0;
    kb_status_t status = ops->pci_config_read(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command));
    if (status != KB_OK) {
        return status;
    }
    command = (uint16_t)((command | set_bits) & (uint16_t)~clear_bits);
    return ops->pci_config_write(device, KB_PCI_COMMAND_OFFSET, &command, sizeof(command));
}

static int trace_pci_enabled(void)
{
    return getenv("KOBOX_TRACE_PCI") != NULL;
}

static void write_u32(void *ptr, uint32_t value)
{
    memcpy(ptr, &value, sizeof(value));
}

static int pci_id_field_matches(uint32_t expected, uint32_t actual)
{
    return expected == KB_PCI_ANY_ID || expected == actual;
}

static const linux_pci_device_id_t *match_pci_id_table(const void *id_table, const kb_pci_id_t *device_id)
{
    if (id_table == NULL || device_id == NULL) {
        return NULL;
    }

    const uint32_t class_value =
        ((uint32_t)device_id->class_code << 16) |
        ((uint32_t)device_id->subclass << 8) |
        (uint32_t)device_id->prog_if;
    const linux_pci_device_id_t *entry = id_table;
    for (size_t i = 0; i < 4096; i++, entry++) {
        if (entry->vendor == 0 && entry->device == 0 && entry->subvendor == 0 &&
            entry->subdevice == 0 && entry->class == 0 && entry->class_mask == 0)
        {
            return NULL;
        }
        if (!pci_id_field_matches(entry->vendor, device_id->vendor_id) ||
            !pci_id_field_matches(entry->device, device_id->device_id) ||
            !pci_id_field_matches(entry->subvendor, device_id->subsystem_vendor_id) ||
            !pci_id_field_matches(entry->subdevice, device_id->subsystem_device_id))
        {
            continue;
        }
        if (((class_value ^ entry->class) & entry->class_mask) != 0) {
            continue;
        }
        return entry;
    }
    return NULL;
}

static int is_nvidia_driver_name(const char *name)
{
    return name != NULL && strncmp(name, "nvidia", 6) == 0;
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

static void quiesce_nvme_controller(void)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_pci_id == NULL || ops->map_bar == NULL || ops->unmap_bar == NULL) {
        return;
    }

    kb_pci_id_t id;
    memset(&id, 0, sizeof(id));
    if (ops->device_pci_id(device, &id) != KB_OK ||
        id.class_code != KB_PCI_CLASS_STORAGE ||
        id.subclass != KB_PCI_SUBCLASS_NVME ||
        id.prog_if != KB_PCI_PROGIF_NVME)
    {
        return;
    }

    kb_mmio_region_t bar;
    memset(&bar, 0, sizeof(bar));
    if (ops->map_bar(device, 0, &bar) != KB_OK || bar.addr == NULL) {
        return;
    }

    uint32_t cc = mmio_read32(bar.addr, KB_NVME_REG_CC);
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: nvme quiesce cc=0x%x csts=0x%x\n", cc, mmio_read32(bar.addr, KB_NVME_REG_CSTS));
    }
    if ((cc & 1u) != 0) {
        mmio_write32(bar.addr, KB_NVME_REG_CC, cc & ~1u);
        for (unsigned i = 0; i < 10000000u; i++) {
            if ((mmio_read32(bar.addr, KB_NVME_REG_CSTS) & 1u) == 0) {
                break;
            }
        }
    }
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: nvme quiesced cc=0x%x csts=0x%x\n", mmio_read32(bar.addr, KB_NVME_REG_CC), mmio_read32(bar.addr, KB_NVME_REG_CSTS));
    }
    ops->unmap_bar(device, &bar);
}

int kb_pci_register_driver(void *driver, void *owner, const char *mod_name)
{
    (void)owner;
    (void)mod_name;
    if (driver == NULL) {
        return -22;
    }

    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = NULL;
    if (first_device(backend, &device) != KB_OK) {
        return -19;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_pci_id == NULL) {
        return -19;
    }
    kb_pci_id_t device_id;
    memset(&device_id, 0, sizeof(device_id));
    if (ops->device_pci_id(device, &device_id) != KB_OK) {
        return -19;
    }

    shim_pci_driver_t *pci_driver = driver;
    const linux_pci_device_id_t *matched_id = match_pci_id_table(pci_driver->id_table, &device_id);
    if (matched_id == NULL) {
        if (trace_pci_enabled()) {
            fprintf(
                stderr,
                "kobox pci: pci_register_driver no match driver=%s vendor=0x%x device=0x%x class=0x%x%02x%02x\n",
                pci_driver->name == NULL ? "" : pci_driver->name,
                device_id.vendor_id,
                device_id.device_id,
                device_id.class_code,
                device_id.subclass,
                device_id.prog_if);
        }
        if (is_nvidia_driver_name(pci_driver->name)) {
            return 0;
        }
        matched_id = pci_driver->id_table;
    }
    binding.driver = pci_driver;
    binding.device = device;
    binding.probed = 0;
    memset(binding.pci_dev_storage, 0, sizeof(binding.pci_dev_storage));
    binding.dma_mask_storage = UINT64_MAX;
    uintptr_t dma_mask_ptr = (uintptr_t)&binding.dma_mask_storage;
    memcpy(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DMA_MASK_OFFSET,
        &dma_mask_ptr,
        sizeof(dma_mask_ptr));

    if (ops != NULL && ops->pci_bar_info != NULL) {
        kb_pci_bar_info_t bar;
        if (ops->pci_bar_info(device, 0, &bar) == KB_OK && bar.present) {
            memcpy(
                binding.pci_dev_storage + KB_LINUX_6_8_PCI_RESOURCE0_START_OFFSET,
                &bar.start,
                sizeof(bar.start));
            memcpy(
                binding.pci_dev_storage + KB_LINUX_6_8_PCI_RESOURCE0_END_OFFSET,
                &bar.end,
                sizeof(bar.end));
        }
    }

    if (pci_driver->probe == NULL) {
        return 0;
    }

    int result = pci_driver->probe(binding.pci_dev_storage, matched_id);
    if (result != 0) {
        binding.driver = NULL;
        binding.device = NULL;
        return result;
    }
    binding.probed = 1;
    return 0;
}

void kb_pci_unregister_driver(void *driver)
{
    if (binding.driver == NULL || binding.driver != (shim_pci_driver_t *)driver) {
        return;
    }
    if (trace_pci_enabled()) {
        fprintf(
            stderr,
            "kobox pci: pci_unregister_driver probed=%d remove=%p\n",
            binding.probed,
            (void *)binding.driver->remove);
    }
    if (binding.probed && binding.driver->remove != NULL) {
        binding.driver->remove(binding.pci_dev_storage);
        if (trace_pci_enabled()) {
            fprintf(stderr, "kobox pci: pci_unregister_driver remove returned\n");
        }
    }
    binding.driver = NULL;
    binding.device = NULL;
    binding.probed = 0;
}

int kb_pci_enable_device(void *dev)
{
    (void)dev;
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_enable_device\n");
    }
    kb_status_t status = update_pci_command(KB_PCI_COMMAND_MEMORY, 0);
    write_u32(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_ENABLE_CNT_OFFSET, 1);
    write_u32(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET + KB_LINUX_6_8_DEVICE_POWER_USAGE_COUNT_OFFSET,
        1);
    return status == KB_OK ? 0 : -5;
}

int kb_pcim_enable_device(void *dev)
{
    return kb_pci_enable_device(dev);
}

void kb_pci_disable_device(void *dev)
{
    (void)dev;
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_disable_device\n");
    }
    quiesce_nvme_controller();
    write_u32(binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_ENABLE_CNT_OFFSET, 0);
    write_u32(
        binding.pci_dev_storage + KB_LINUX_6_8_PCI_DEV_DEVICE_OFFSET + KB_LINUX_6_8_DEVICE_POWER_USAGE_COUNT_OFFSET,
        0);
    (void)update_pci_command(0, (uint16_t)(KB_PCI_COMMAND_MEMORY | KB_PCI_COMMAND_MASTER));
}

void kb_pci_set_master(void *dev)
{
    (void)dev;
    if (trace_pci_enabled()) {
        fprintf(stderr, "kobox pci: pci_set_master\n");
    }
    (void)update_pci_command(KB_PCI_COMMAND_MASTER, 0);
}

void *kb_pci_iomap(void *dev, int bar, unsigned long max)
{
    (void)dev;
    (void)max;
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return NULL;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->map_bar == NULL) {
        return NULL;
    }

    kb_mmio_region_t region;
    if (ops->map_bar(device, (unsigned)bar, &region) != KB_OK) {
        return NULL;
    }
    return region.addr;
}

void *kb_pcim_iomap(void *dev, int bar, unsigned long max)
{
    return kb_pci_iomap(dev, bar, max);
}

void kb_pci_iounmap(void *dev, void *addr)
{
    (void)dev;
    (void)addr;
}

void *kb_ioremap(uint64_t phys_addr, size_t size)
{
    kb_backend_t *backend = kb_shim_current_backend();
    kb_device_t *device = binding.device;
    if (device == NULL && first_device(backend, &device) != KB_OK) {
        return NULL;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->pci_bar_info == NULL || ops->map_bar == NULL) {
        return NULL;
    }

    kb_pci_bar_info_t bar;
    if (ops->pci_bar_info(device, 0, &bar) != KB_OK || !bar.present) {
        return NULL;
    }
    if (phys_addr < bar.start || size > bar.size || phys_addr + size - 1u > bar.end) {
        return NULL;
    }

    kb_mmio_region_t region;
    if (ops->map_bar(device, 0, &region) != KB_OK) {
        return NULL;
    }
    return (unsigned char *)region.addr + (phys_addr - bar.start);
}

void kb_iounmap(void *addr)
{
    (void)addr;
}

int __pci_register_driver(void *driver, void *owner, const char *mod_name)
{
    return kb_pci_register_driver(driver, owner, mod_name);
}

void pci_unregister_driver(void *driver)
{
    kb_pci_unregister_driver(driver);
}

int pci_enable_device(void *dev)
{
    return kb_pci_enable_device(dev);
}

int pcim_enable_device(void *dev)
{
    return kb_pcim_enable_device(dev);
}

void pci_disable_device(void *dev)
{
    kb_pci_disable_device(dev);
}

void pci_set_master(void *dev)
{
    kb_pci_set_master(dev);
}

void *pci_iomap(void *dev, int bar, unsigned long max)
{
    return kb_pci_iomap(dev, bar, max);
}

void *pcim_iomap(void *dev, int bar, unsigned long max)
{
    return kb_pcim_iomap(dev, bar, max);
}

void pci_iounmap(void *dev, void *addr)
{
    kb_pci_iounmap(dev, addr);
}

void *ioremap(uint64_t phys_addr, size_t size)
{
    return kb_ioremap(phys_addr, size);
}

void iounmap(void *addr)
{
    kb_iounmap(addr);
}
