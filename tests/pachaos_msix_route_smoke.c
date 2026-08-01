#include "../src/device/pachaos_capsule/device_pachaos_capsule.c"

static unsigned derived_irq_kind;
static unsigned derived_irq_vector;

int pacha_capsule_device_derive_irq(
    int device_fd,
    unsigned kind,
    unsigned vector,
    uint64_t flags,
    struct pacha_capsule_irq *out)
{
    (void)device_fd;
    (void)flags;
    derived_irq_kind = kind;
    derived_irq_vector = vector;
    if (out == NULL) {
        return -1;
    }
    out->fd = 16;
    out->count = 0;
    return 0;
}

int pacha_capsule_close(int fd)
{
    (void)fd;
    return 0;
}

int pacha_capsule_is_fd(int fd)
{
    return fd >= 16 && fd < 256;
}

static void ignore_irq(void *ctx)
{
    (void)ctx;
}

int main(void)
{
    kb_device_t device = {0};
    kb_pachaos_capsule_backend_t backend = {0};
    uint32_t vector = 0;

    device.backend = &backend;

    device.info.index = 0x50;
    if (pachaos_capsule_msix_delivery_vector(&device, 0, &vector) != KB_OK || vector != 0x50) {
        return 1;
    }
    if (pachaos_capsule_msix_delivery_vector(&device, 15, &vector) != KB_OK || vector != 0x5f) {
        return 2;
    }
    device.info.index = 0xc0;
    if (pachaos_capsule_msix_delivery_vector(&device, 15, &vector) != KB_OK || vector != 0xcf) {
        return 3;
    }
    if (pachaos_capsule_msix_delivery_vector(&device, 16, &vector) != KB_ERR_INVALID) {
        return 4;
    }

    device.info.index = 0x51;
    if (pachaos_capsule_msix_delivery_vector(&device, 0, &vector) != KB_ERR_INVALID) {
        return 5;
    }
    device.info.index = 0xd0;
    if (pachaos_capsule_msix_delivery_vector(&device, 0, &vector) != KB_ERR_INVALID) {
        return 6;
    }
    device.info.index = UINT64_MAX - 15;
    if (pachaos_capsule_msix_delivery_vector(&device, 0, &vector) != KB_ERR_INVALID) {
        return 7;
    }

    device.info.index = 0x50;
    kb_device_irq_t *irq = NULL;
    if (pachaos_capsule_irq_register(&device, 0, ignore_irq, NULL, &irq) != KB_ERR_UNSUPPORTED) {
        return 8;
    }
    const unsigned msi = KB_DEVICE_IRQ_BACKEND_KIND_MSI << KB_DEVICE_IRQ_BACKEND_KIND_SHIFT;
    if (pachaos_capsule_irq_register(&device, msi, ignore_irq, NULL, &irq) != KB_ERR_UNSUPPORTED) {
        return 9;
    }
    const unsigned invalid_msix =
        (KB_DEVICE_IRQ_BACKEND_KIND_MSIX << KB_DEVICE_IRQ_BACKEND_KIND_SHIFT) |
        KB_DEVICE_ROUTED_MSIX_ENTRY_COUNT;
    if (pachaos_capsule_irq_register(&device, invalid_msix, ignore_irq, NULL, &irq) != KB_ERR_INVALID) {
        return 10;
    }

    device.info.index = 0;
    const unsigned msix = KB_DEVICE_IRQ_BACKEND_KIND_MSIX << KB_DEVICE_IRQ_BACKEND_KIND_SHIFT;
    if (pachaos_capsule_irq_register(&device, msix, ignore_irq, NULL, &irq) != KB_ERR_INVALID) {
        return 11;
    }

    device.info.index = 0x60;
    const unsigned valid_msix = msix | 7u;
    if (pachaos_capsule_irq_register(&device, valid_msix, ignore_irq, NULL, &irq) != KB_OK ||
        irq == NULL ||
        derived_irq_kind != PACHA_CAPSULE_IRQ_MSIX ||
        derived_irq_vector != 7)
    {
        return 12;
    }
    pachaos_capsule_irq_unregister(&device, irq);
    return 0;
}
