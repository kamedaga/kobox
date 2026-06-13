#include <stdlib.h>
#include <string.h>

void *kb_hwmon_device_register_with_info(void *dev, const char *name, void *data, const void *chip, const void *groups)
{
    (void)dev;
    (void)name;
    (void)chip;
    (void)groups;
    unsigned char *hwmon = calloc(1, 256);
    if (hwmon == NULL) {
        return NULL;
    }
    memcpy(hwmon + 0x78, &data, sizeof(data));
    return hwmon;
}
