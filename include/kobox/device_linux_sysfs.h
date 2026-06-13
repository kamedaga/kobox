#pragma once

#include "kobox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

kb_status_t kb_linux_sysfs_device_create(kb_device_backend_t **out_backend);

#ifdef __cplusplus
}
#endif
