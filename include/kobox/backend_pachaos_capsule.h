#pragma once

#include "kobox/backend.h"

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

kb_status_t kb_pachaos_capsule_parse_token(const char *text, uint64_t *out_token);
kb_status_t kb_pachaos_capsule_create(uint64_t device_capsule, kb_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_create_from_env(kb_backend_t **out_backend);
kb_status_t kb_pachaos_capsule_dump_catalog(FILE *out);

#ifdef __cplusplus
}
#endif
