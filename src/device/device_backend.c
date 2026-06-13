#include "device/device_backend_internal.h"

const kb_device_backend_ops_t *kb_device_backend_get_ops(const kb_device_backend_t *backend)
{
    if (backend == 0) {
        return 0;
    }
    return backend->ops;
}

void kb_device_backend_destroy(kb_device_backend_t *backend)
{
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops != 0 && ops->destroy != 0) {
        ops->destroy(backend);
    }
}
