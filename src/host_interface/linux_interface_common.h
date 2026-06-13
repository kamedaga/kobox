#pragma once

#include "host_interface/interface_internal.h"
#include "kobox/interface_linux.h"

typedef struct kb_linux_host_interface {
    kb_interface_t base;
    kb_interface_kind_t kind;
    char *name;
    char *subsystem;
    char *endpoint;
    kb_status_t (*dispatch)(void *ctx, const void *message, size_t message_size);
    void *dispatch_ctx;
    kb_platform_t *platform;
} kb_linux_host_interface_t;

kb_status_t kb_linux_host_interface_create(
    kb_interface_kind_t kind,
    const kb_linux_interface_desc_t *desc,
    const kb_interface_ops_t *ops,
    kb_interface_t **out_interface);

void kb_linux_host_interface_destroy(kb_interface_t *interface);
kb_interface_kind_t kb_linux_host_interface_kind(const kb_interface_t *interface);
const char *kb_linux_host_interface_name(const kb_interface_t *interface);
const char *kb_linux_host_interface_subsystem(const kb_interface_t *interface);
kb_status_t kb_linux_host_interface_bind(kb_interface_t *interface, kb_platform_t *platform);
void kb_linux_host_interface_unbind(kb_interface_t *interface);
kb_status_t kb_linux_host_interface_poll(kb_interface_t *interface, uint64_t timeout_ns);
kb_status_t kb_linux_host_interface_dispatch(kb_interface_t *interface, const void *message, size_t message_size);
