#pragma once

#include "kobox/interface.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_linux_interface_desc {
    const char *name;
    const char *subsystem;
    const char *endpoint;
    kb_status_t (*dispatch)(void *ctx, const void *message, size_t message_size);
    void *dispatch_ctx;
} kb_linux_interface_desc_t;

kb_status_t kb_linux_ipc_interface_create(
    const kb_linux_interface_desc_t *desc,
    kb_interface_t **out_interface);

#ifdef __cplusplus
}
#endif
