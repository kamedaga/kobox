#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "kobox/device_pachaos_capsule.h"

#include "pacha/capsule.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static kb_status_t parse_fd_text(const char *text, int *out_fd)
{
    if (text == NULL || text[0] == '\0' || out_fd == NULL) {
        return KB_ERR_INVALID;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value > INT32_MAX) {
        return KB_ERR_INVALID;
    }
    if (!pacha_capsule_is_fd((int)value)) {
        return KB_ERR_INVALID;
    }

    *out_fd = (int)value;
    return KB_OK;
}

kb_status_t kb_pachaos_capsule_parse_token(const char *text, uint64_t *out_token)
{
    int fd = -1;
    const kb_status_t status = parse_fd_text(text, &fd);
    if (status != KB_OK) {
        return status;
    }
    *out_token = (uint64_t)(uint32_t)fd;
    return KB_OK;
}

kb_status_t kb_pachaos_capsule_device_create_from_env(kb_device_backend_t **out_backend)
{
    if (out_backend == NULL) {
        return KB_ERR_INVALID;
    }

    const char *text = getenv("KOBOX_PACHAOS_DEVICE_FD");
    if (text == NULL || text[0] == '\0') {
        text = getenv("PACHA_EXEC_DEVICE_FD");
    }
    if (text == NULL || text[0] == '\0') {
        text = getenv("KOBOX_PACHAOS_DEVICE_CAPSULE");
    }

    uint64_t fd = 0;
    kb_status_t status = kb_pachaos_capsule_parse_token(text, &fd);
    if (status != KB_OK) {
        return status;
    }
    return kb_pachaos_capsule_device_create(fd, out_backend);
}

kb_status_t kb_pachaos_capsule_dump_catalog(FILE *out)
{
    if (out == NULL) {
        return KB_ERR_INVALID;
    }

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create_from_env(&backend);
    if (status != KB_OK) {
        return status;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops == NULL || ops->device_at == NULL || ops->device_pci_id == NULL || ops->device_pci_location == NULL) {
        kb_device_backend_destroy(backend);
        return KB_ERR_UNSUPPORTED;
    }

    kb_device_t *device = NULL;
    status = ops->device_at(backend, 0, &device);
    if (status != KB_OK) {
        kb_device_backend_destroy(backend);
        return status;
    }

    kb_pci_id_t pci_id;
    kb_pci_location_t location;
    status = ops->device_pci_id(device, &pci_id);
    if (status == KB_OK) {
        status = ops->device_pci_location(device, &location);
    }
    if (status != KB_OK) {
        kb_device_backend_destroy(backend);
        return status;
    }

    fprintf(
        out,
        "PachaOS capsule device: pci=%04x:%04x class=%02x:%02x:%02x bdf=%04x:%02x:%02x.%u\n",
        pci_id.vendor_id,
        pci_id.device_id,
        pci_id.class_code,
        pci_id.subclass,
        pci_id.prog_if,
        location.segment,
        location.bus,
        location.device,
        location.function);

    kb_device_backend_destroy(backend);
    return KB_OK;
}
