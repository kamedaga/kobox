#include "host_interface/linux_interface_common.h"

#include "kobox/interface_linux.h"

static const kb_interface_ops_t linux_ipc_interface_ops = {
    kb_linux_host_interface_destroy,
    kb_linux_host_interface_kind,
    kb_linux_host_interface_name,
    kb_linux_host_interface_subsystem,
    kb_linux_host_interface_bind,
    kb_linux_host_interface_unbind,
    kb_linux_host_interface_poll,
    kb_linux_host_interface_dispatch,
};

kb_status_t kb_linux_ipc_interface_create(
    const kb_linux_interface_desc_t *desc,
    kb_interface_t **out_interface)
{
    return kb_linux_host_interface_create(KB_INTERFACE_IPC, desc, &linux_ipc_interface_ops, out_interface);
}
