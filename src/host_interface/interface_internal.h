#pragma once

#include "kobox/interface.h"

struct kb_interface {
    const kb_interface_ops_t *ops;
};
