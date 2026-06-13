#pragma once

#include "kobox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb_interface kb_interface_t;
typedef struct kb_platform kb_platform_t;

typedef enum kb_interface_kind {
    KB_INTERFACE_NONE = 0,
    KB_INTERFACE_IPC = 1,
} kb_interface_kind_t;

typedef struct kb_interface_ops {
    void (*destroy)(kb_interface_t *interface);
    kb_interface_kind_t (*kind)(const kb_interface_t *interface);
    const char *(*name)(const kb_interface_t *interface);
    const char *(*subsystem)(const kb_interface_t *interface);
    kb_status_t (*bind)(kb_interface_t *interface, kb_platform_t *platform);
    void (*unbind)(kb_interface_t *interface);
    kb_status_t (*poll)(kb_interface_t *interface, uint64_t timeout_ns);
    kb_status_t (*dispatch)(kb_interface_t *interface, const void *message, size_t message_size);
} kb_interface_ops_t;

const kb_interface_ops_t *kb_interface_get_ops(const kb_interface_t *interface);
kb_interface_kind_t kb_interface_kind(const kb_interface_t *interface);
const char *kb_interface_name(const kb_interface_t *interface);
const char *kb_interface_subsystem(const kb_interface_t *interface);
kb_status_t kb_interface_bind(kb_interface_t *interface, kb_platform_t *platform);
void kb_interface_unbind(kb_interface_t *interface);
kb_status_t kb_interface_poll(kb_interface_t *interface, uint64_t timeout_ns);
kb_status_t kb_interface_dispatch(kb_interface_t *interface, const void *message, size_t message_size);
void kb_interface_destroy(kb_interface_t *interface);

#ifdef __cplusplus
}
#endif
