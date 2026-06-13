#include "host_interface/interface_internal.h"

const kb_interface_ops_t *kb_interface_get_ops(const kb_interface_t *interface)
{
    if (interface == 0) {
        return 0;
    }
    return interface->ops;
}

kb_interface_kind_t kb_interface_kind(const kb_interface_t *interface)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops == 0 || ops->kind == 0) {
        return KB_INTERFACE_NONE;
    }
    return ops->kind(interface);
}

const char *kb_interface_name(const kb_interface_t *interface)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops == 0 || ops->name == 0) {
        return 0;
    }
    return ops->name(interface);
}

const char *kb_interface_subsystem(const kb_interface_t *interface)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops == 0 || ops->subsystem == 0) {
        return 0;
    }
    return ops->subsystem(interface);
}

kb_status_t kb_interface_bind(kb_interface_t *interface, kb_platform_t *platform)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops == 0 || ops->bind == 0) {
        return KB_ERR_UNSUPPORTED;
    }
    return ops->bind(interface, platform);
}

void kb_interface_unbind(kb_interface_t *interface)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops != 0 && ops->unbind != 0) {
        ops->unbind(interface);
    }
}

kb_status_t kb_interface_poll(kb_interface_t *interface, uint64_t timeout_ns)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops == 0 || ops->poll == 0) {
        return KB_ERR_UNSUPPORTED;
    }
    return ops->poll(interface, timeout_ns);
}

kb_status_t kb_interface_dispatch(kb_interface_t *interface, const void *message, size_t message_size)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops == 0 || ops->dispatch == 0) {
        return KB_ERR_UNSUPPORTED;
    }
    return ops->dispatch(interface, message, message_size);
}

void kb_interface_destroy(kb_interface_t *interface)
{
    const kb_interface_ops_t *ops = kb_interface_get_ops(interface);
    if (ops != 0 && ops->destroy != 0) {
        ops->destroy(interface);
    }
}
