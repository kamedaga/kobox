#include "kobox/shim.h"

#include <stddef.h>

kb_backend_t *kb_shim_current_backend(void);

typedef struct shim_pci_driver {
    const char *name;
    const void *id_table;
    int (*probe)(void *dev, const void *id);
    void (*remove)(void *dev);
} shim_pci_driver_t;

typedef struct shim_pci_binding {
    shim_pci_driver_t *driver;
    kb_device_t *device;
    unsigned char pci_dev_storage[512];
    int probed;
} shim_pci_binding_t;

static shim_pci_binding_t binding;

static kb_status_t first_device(kb_backend_t *backend, kb_device_t **out_device)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == NULL || ops->device_at == NULL) {
        return KB_ERR_INVALID;
    }
    return ops->device_at(backend, 0, out_device);
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

    shim_pci_driver_t *pci_driver = driver;
    binding.driver = pci_driver;
    binding.device = device;
    binding.probed = 0;

    if (pci_driver->probe == NULL) {
        return 0;
    }

    int result = pci_driver->probe(binding.pci_dev_storage, pci_driver->id_table);
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
    if (binding.probed && binding.driver->remove != NULL) {
        binding.driver->remove(binding.pci_dev_storage);
    }
    binding.driver = NULL;
    binding.device = NULL;
    binding.probed = 0;
}

int kb_pci_enable_device(void *dev)
{
    (void)dev;
    return 0;
}

int kb_pcim_enable_device(void *dev)
{
    return kb_pci_enable_device(dev);
}

void kb_pci_disable_device(void *dev)
{
    (void)dev;
}

void kb_pci_set_master(void *dev)
{
    (void)dev;
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
