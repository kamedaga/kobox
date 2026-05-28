#pragma once

#include "kobox/backend.h"

struct kb_backend {
    const kb_backend_ops_t *ops;
};
