#pragma once

#include "kobox/device.h"

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

kb_status_t kb_pachaos_capsule_parse_token(const char *text, uint64_t *out_token);
kb_status_t kb_pachaos_capsule_device_create(uint64_t device_capsule, kb_device_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_devices_create(
    const uint64_t *device_capsules,
    size_t device_count,
    kb_device_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_device_create_from_env(kb_device_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_dump_catalog(FILE *out);
size_t kb_pachaos_capsule_report_residuals(kb_device_backend_t *backend, FILE *out, const char *label);

#ifdef __cplusplus
}
#endif
