#include "device/device_backend_internal.h"

#include <stdint.h>

static int pointer_is_error_value(const void *ptr)
{
    return (uintptr_t)ptr < 4096u;
}

const kb_device_backend_ops_t *kb_device_backend_get_ops(const kb_device_backend_t *backend)
{
    if (backend == 0 || pointer_is_error_value(backend)) {
        return 0;
    }
    const kb_device_backend_ops_t *ops = backend->ops;
    if (ops == 0 || pointer_is_error_value(ops)) {
        return 0;
    }
    return ops;
}

void kb_device_backend_destroy(kb_device_backend_t *backend)
{
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops != 0 && ops->destroy != 0) {
        ops->destroy(backend);
    }
}
