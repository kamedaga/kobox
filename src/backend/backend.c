#include "backend/backend_internal.h"

const kb_backend_ops_t *kb_backend_get_ops(const kb_backend_t *backend)
{
    if (backend == 0) {
        return 0;
    }
    return backend->ops;
}

void kb_backend_destroy(kb_backend_t *backend)
{
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops != 0 && ops->destroy != 0) {
        ops->destroy(backend);
    }
}
