#pragma once

#include <stddef.h>
#include "kobox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_module kb_module_t;

typedef struct kb_module_image {
    const void *data;
    size_t size;
    const char *name;
} kb_module_image_t;

const char *kb_module_loader_version(void);

kb_status_t kb_module_open_image(
    const kb_module_image_t *image,
    kb_device_backend_t *backend,
    kb_module_t **out_module);

kb_status_t kb_module_call_init(kb_module_t *module, int *out_result);
kb_status_t kb_module_call_cleanup(kb_module_t *module);
kb_status_t kb_module_find_symbol(kb_module_t *module, const char *name, void **out_address);
kb_module_t *kb_module_find_owner_for_address(const void *address);
void kb_module_debug_describe_address(const void *address);
int kb_module_run_registered_ops_smoke(void);
void kb_module_close(kb_module_t *module);

#ifdef __cplusplus
}
#endif
