#pragma once

#include "kobox/device.h"

struct kb_device_backend {
    const kb_device_backend_ops_t *ops;
};
