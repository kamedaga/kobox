#include "host_interface/linux_interface_common.h"

#include "kobox/interface_linux.h"

#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value, const char *fallback)
{
    if (value == NULL) {
        value = fallback;
    }
    if (value == NULL) {
        value = "";
    }
    const size_t len = strlen(value);
    char *copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

static kb_linux_host_interface_t *linux_from_interface(kb_interface_t *interface)
{
    return (kb_linux_host_interface_t *)interface;
}

static const kb_linux_host_interface_t *linux_from_const_interface(const kb_interface_t *interface)
{
    return (const kb_linux_host_interface_t *)interface;
}

kb_status_t kb_linux_host_interface_create(
    kb_interface_kind_t kind,
    const kb_linux_interface_desc_t *desc,
    const kb_interface_ops_t *ops,
    kb_interface_t **out_interface)
{
    if (desc == NULL || ops == NULL || out_interface == NULL || desc->subsystem == NULL) {
        return KB_ERR_INVALID;
    }
    *out_interface = NULL;

    kb_linux_host_interface_t *interface = calloc(1, sizeof(*interface));
    if (interface == NULL) {
        return KB_ERR_NOMEM;
    }

    interface->base.ops = ops;
    interface->kind = kind;
    interface->name = copy_string(desc->name, "linux-interface");
    interface->subsystem = copy_string(desc->subsystem, NULL);
    interface->endpoint = copy_string(desc->endpoint, NULL);
    interface->dispatch = desc->dispatch;
    interface->dispatch_ctx = desc->dispatch_ctx;
    if (interface->name == NULL || interface->subsystem == NULL || interface->endpoint == NULL) {
        kb_linux_host_interface_destroy(&interface->base);
        return KB_ERR_NOMEM;
    }

    *out_interface = &interface->base;
    return KB_OK;
}

void kb_linux_host_interface_destroy(kb_interface_t *interface)
{
    kb_linux_host_interface_t *linux_interface = linux_from_interface(interface);
    if (linux_interface == NULL) {
        return;
    }
    free(linux_interface->endpoint);
    free(linux_interface->subsystem);
    free(linux_interface->name);
    free(linux_interface);
}

kb_interface_kind_t kb_linux_host_interface_kind(const kb_interface_t *interface)
{
    const kb_linux_host_interface_t *linux_interface = linux_from_const_interface(interface);
    return linux_interface == NULL ? KB_INTERFACE_NONE : linux_interface->kind;
}

const char *kb_linux_host_interface_name(const kb_interface_t *interface)
{
    const kb_linux_host_interface_t *linux_interface = linux_from_const_interface(interface);
    return linux_interface == NULL ? NULL : linux_interface->name;
}

const char *kb_linux_host_interface_subsystem(const kb_interface_t *interface)
{
    const kb_linux_host_interface_t *linux_interface = linux_from_const_interface(interface);
    return linux_interface == NULL ? NULL : linux_interface->subsystem;
}

kb_status_t kb_linux_host_interface_bind(kb_interface_t *interface, kb_platform_t *platform)
{
    kb_linux_host_interface_t *linux_interface = linux_from_interface(interface);
    if (linux_interface == NULL || platform == NULL) {
        return KB_ERR_INVALID;
    }
    linux_interface->platform = platform;
    return KB_OK;
}

void kb_linux_host_interface_unbind(kb_interface_t *interface)
{
    kb_linux_host_interface_t *linux_interface = linux_from_interface(interface);
    if (linux_interface != NULL) {
        linux_interface->platform = NULL;
    }
}

kb_status_t kb_linux_host_interface_poll(kb_interface_t *interface, uint64_t timeout_ns)
{
    kb_linux_host_interface_t *linux_interface = linux_from_interface(interface);
    if (linux_interface == NULL || linux_interface->platform == NULL) {
        return KB_ERR_INVALID;
    }
    (void)timeout_ns;
    return KB_OK;
}

kb_status_t kb_linux_host_interface_dispatch(kb_interface_t *interface, const void *message, size_t message_size)
{
    kb_linux_host_interface_t *linux_interface = linux_from_interface(interface);
    if (linux_interface == NULL || linux_interface->platform == NULL) {
        return KB_ERR_INVALID;
    }
    if (message == NULL && message_size != 0) {
        return KB_ERR_INVALID;
    }
    if (linux_interface->dispatch != NULL) {
        return linux_interface->dispatch(linux_interface->dispatch_ctx, message, message_size);
    }
    return KB_OK;
}
