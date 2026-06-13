#include "kobox/device.h"
#include "kobox/device_linux_sysfs.h"
#include "kobox/device_linux_vfio.h"
#include "kobox/device_pachaos_capsule.h"

#include <stdio.h>
#include <string.h>

static const char *status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return "KB_OK";
    case KB_ERR_INVALID:
        return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND:
        return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED:
        return "KB_ERR_DENIED";
    case KB_ERR_NOMEM:
        return "KB_ERR_NOMEM";
    case KB_ERR_IO:
        return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED:
        return "KB_ERR_UNSUPPORTED";
    case KB_ERR_PCI_CONFIG:
        return "KB_ERR_PCI_CONFIG";
    default:
        return "KB_ERR_UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    const char *device_backend_name = "sysfs";
    const char *pci_bdf = NULL;
    const char *capsule_text = NULL;
    int dump_pachaos_catalog = 0;
    if (argc == 3 && strcmp(argv[1], "vfio") == 0) {
        device_backend_name = "vfio";
        pci_bdf = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "pachaos") == 0 && strcmp(argv[2], "--catalog") == 0) {
        device_backend_name = "pachaos";
        dump_pachaos_catalog = 1;
    } else if (argc == 3 && strcmp(argv[1], "pachaos") == 0) {
        device_backend_name = "pachaos";
        capsule_text = argv[2];
    } else if (argc == 2 && strcmp(argv[1], "pachaos") == 0) {
        device_backend_name = "pachaos";
    } else if (argc != 1) {
        fprintf(stderr, "usage: kobox-ls-devices [vfio <BDF>|pachaos [DeviceCapsule|--catalog]]\n");
        return 1;
    }

    if (dump_pachaos_catalog) {
        kb_status_t status = kb_pachaos_capsule_dump_catalog(stdout);
        if (status != KB_OK) {
            fprintf(stderr, "pachaos catalog failed: %s (%d)\n", status_name(status), status);
            return 1;
        }
        return 0;
    }

    kb_device_backend_t *backend = NULL;
    kb_status_t status = KB_OK;
    if (strcmp(device_backend_name, "vfio") == 0) {
        status = kb_linux_vfio_device_create(pci_bdf, &backend);
    } else if (strcmp(device_backend_name, "pachaos") == 0) {
        if (capsule_text != NULL) {
            uint64_t capsule = 0;
            status = kb_pachaos_capsule_parse_token(capsule_text, &capsule);
            if (status == KB_OK) {
                status = kb_pachaos_capsule_device_create(capsule, &backend);
            }
        } else {
            status = kb_pachaos_capsule_device_create_from_env(&backend);
        }
    } else {
        status = kb_linux_sysfs_device_create(&backend);
    }
    if (status != KB_OK) {
        fprintf(stderr, "%s device backend failed: %s (%d)\n", device_backend_name, status_name(status), status);
        return 1;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops == NULL) {
        kb_device_backend_destroy(backend);
        return 2;
    }

    size_t count = 0;
    status = ops->device_count(backend, &count);
    if (status != KB_OK) {
        fprintf(stderr, "device_count failed: %s (%d)\n", status_name(status), status);
        kb_device_backend_destroy(backend);
        return 3;
    }

    printf("PCI devices: %zu\n", count);
    for (size_t i = 0; i < count; i++) {
        kb_device_t *device = NULL;
        kb_pci_id_t id;
        kb_pci_location_t location;

        status = ops->device_at(backend, i, &device);
        if (status != KB_OK) {
            fprintf(stderr, "device_at(%zu) failed: %s (%d)\n", i, status_name(status), status);
            kb_device_backend_destroy(backend);
            return 4;
        }
        status = ops->device_pci_id(device, &id);
        if (status != KB_OK) {
            fprintf(stderr, "device_pci_id(%zu) failed: %s (%d)\n", i, status_name(status), status);
            kb_device_backend_destroy(backend);
            return 5;
        }
        status = ops->device_pci_location(device, &location);
        if (status != KB_OK) {
            fprintf(stderr, "device_pci_location(%zu) failed: %s (%d)\n", i, status_name(status), status);
            kb_device_backend_destroy(backend);
            return 6;
        }

        printf(
            "%04x:%02x:%02x.%u vendor=%04x device=%04x class=%02x:%02x:%02x subsystem=%04x:%04x\n",
            location.segment,
            location.bus,
            location.device,
            location.function,
            id.vendor_id,
            id.device_id,
            id.class_code,
            id.subclass,
            id.prog_if,
            id.subsystem_vendor_id,
            id.subsystem_device_id);

        if (ops->pci_config_read != NULL) {
            unsigned char config_id[4];
            status = ops->pci_config_read(device, 0, config_id, sizeof(config_id));
            if (status == KB_OK) {
                unsigned config_vendor = (unsigned)config_id[0] | ((unsigned)config_id[1] << 8);
                unsigned config_device = (unsigned)config_id[2] | ((unsigned)config_id[3] << 8);
                printf("  config vendor=%04x device=%04x\n", config_vendor, config_device);
            }
        }

        if (ops->pci_bar_info != NULL) {
            for (unsigned bar = 0; bar < 6; bar++) {
                kb_pci_bar_info_t info;
                status = ops->pci_bar_info(device, bar, &info);
                if (status == KB_ERR_NOT_FOUND) {
                    continue;
                }
                if (status != KB_OK) {
                    fprintf(stderr, "pci_bar_info(%zu, %u) failed: %s (%d)\n", i, bar, status_name(status), status);
                    kb_device_backend_destroy(backend);
                    return 7;
                }
                printf(
                    "  BAR%u start=0x%016llx end=0x%016llx size=0x%llx flags=0x%llx\n",
                    bar,
                    (unsigned long long)info.start,
                    (unsigned long long)info.end,
                    (unsigned long long)info.size,
                    (unsigned long long)info.flags);
                if (ops->map_bar != NULL && ops->unmap_bar != NULL) {
                    kb_mmio_region_t region;
                    status = ops->map_bar(device, bar, &region);
                    if (status == KB_OK) {
                        printf("    mapped size=0x%llx flags=0x%x\n", (unsigned long long)region.size, region.flags);
                        ops->unmap_bar(device, &region);
                    }
                }
            }
        }
    }

    kb_device_backend_destroy(backend);
    return 0;
}
