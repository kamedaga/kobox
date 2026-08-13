#pragma once

#include "kobox/device.h"

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_pachaos_capsule_dma_profile {
    uint64_t copy_back_calls;
    uint64_t copy_back_bytes;
    uint64_t copy_back_cycles;
} kb_pachaos_capsule_dma_profile_t;

typedef struct kb_pachaos_capsule_irq_profile {
    uint64_t wait_calls;
    uint64_t wait_cycles;
    uint64_t fd_wait_calls;
    uint64_t fd_wait_cycles;
    uint64_t fd_wait_ready;
    uint64_t poll_calls;
    uint64_t poll_cycles;
    uint64_t poll_ready;
    uint64_t pre_poll_calls;
    uint64_t pre_poll_cycles;
    uint64_t pre_poll_ready;
    uint64_t post_poll_calls;
    uint64_t post_poll_cycles;
    uint64_t post_poll_ready;
    uint64_t handler_calls;
    uint64_t handler_cycles;
} kb_pachaos_capsule_irq_profile_t;

kb_status_t kb_pachaos_capsule_parse_token(const char *text, uint64_t *out_token);
kb_status_t kb_pachaos_capsule_device_create(uint64_t device_capsule, kb_device_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_devices_create(
    const uint64_t *device_capsules,
    size_t device_count,
    kb_device_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_device_create_from_env(kb_device_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_dump_catalog(FILE *out);
size_t kb_pachaos_capsule_report_residuals(kb_device_backend_t *backend, FILE *out, const char *label);
void kb_pachaos_capsule_dma_profile_snapshot(kb_pachaos_capsule_dma_profile_t *out_profile);
void kb_pachaos_capsule_irq_profile_snapshot(kb_pachaos_capsule_irq_profile_t *out_profile);

#ifdef __cplusplus
}
#endif
