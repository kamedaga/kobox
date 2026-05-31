#include "kobox/shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_LINUX_DEVICE_KOBJ_NAME_OFFSET = 0,
    KB_LINUX_6_8_DEVICE_BUS_OFFSET = 0x60,
    KB_LINUX_6_8_DEVICE_DRIVER_OFFSET = 0x68,
    KB_LINUX_6_8_DEVICE_DRIVER_DATA_OFFSET = 0x78,
    KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET = 0x08,
    KB_LINUX_6_8_DEVICE_DRIVER_PROBE_OFFSET = 0x38,
    KB_LINUX_6_8_BUS_MATCH_OFFSET = 0x28,
    KB_LINUX_6_8_BUS_PROBE_OFFSET = 0x38,
    KB_DRIVER_CORE_RECORD_MAX = 256,
    KB_SYSFS_DIRENT_MAX = 256,
};

typedef struct kb_device_record {
    int active;
    void *dev;
    void *bus;
} kb_device_record_t;

typedef struct kb_driver_record {
    int active;
    void *driver;
    void *bus;
} kb_driver_record_t;

static kb_device_record_t device_records[KB_DRIVER_CORE_RECORD_MAX];
static kb_driver_record_t driver_records[KB_DRIVER_CORE_RECORD_MAX];

typedef struct kb_sysfs_dirent_record {
    int active;
    void *parent;
    const void *ns;
    char name[64];
    unsigned char storage[128];
} kb_sysfs_dirent_record_t;

static kb_sysfs_dirent_record_t sysfs_dirent_records[KB_SYSFS_DIRENT_MAX];

static int trace_device_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_DEVICE");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int sysfs_dirent_enabled(void)
{
    const char *value = getenv("KOBOX_ENABLE_SYSFS_DIRENT");
    return value == NULL || value[0] == '\0' || strcmp(value, "0") != 0;
}

static void *read_ptr(const void *base, size_t offset)
{
    void *value = NULL;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    return value;
}

static void write_ptr(void *base, size_t offset, void *value)
{
    if (base != NULL) {
        memcpy((unsigned char *)base + offset, &value, sizeof(value));
    }
}

static kb_device_record_t *record_device(void *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    void *bus = read_ptr(dev, KB_LINUX_6_8_DEVICE_BUS_OFFSET);
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (device_records[i].active && device_records[i].dev == dev) {
            device_records[i].bus = bus;
            return &device_records[i];
        }
    }
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (!device_records[i].active) {
            device_records[i].active = 1;
            device_records[i].dev = dev;
            device_records[i].bus = bus;
            return &device_records[i];
        }
    }
    return NULL;
}

static kb_driver_record_t *record_driver(void *driver)
{
    if (driver == NULL) {
        return NULL;
    }
    void *bus = read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET);
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (driver_records[i].active && driver_records[i].driver == driver) {
            driver_records[i].bus = bus;
            return &driver_records[i];
        }
    }
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (!driver_records[i].active) {
            driver_records[i].active = 1;
            driver_records[i].driver = driver;
            driver_records[i].bus = bus;
            return &driver_records[i];
        }
    }
    return NULL;
}

static int device_matches_driver(void *dev, void *driver)
{
    void *dev_bus = read_ptr(dev, KB_LINUX_6_8_DEVICE_BUS_OFFSET);
    void *driver_bus = read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET);
    if (dev_bus == NULL || dev_bus != driver_bus) {
        return 0;
    }

    int (*match)(void *, void *) = (int (*)(void *, void *))read_ptr(dev_bus, KB_LINUX_6_8_BUS_MATCH_OFFSET);
    if (match == NULL) {
        return 1;
    }
    return match(dev, driver) > 0;
}

static int probe_device_with_driver(void *dev, void *driver)
{
    if (!device_matches_driver(dev, driver)) {
        return 0;
    }
    if (read_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_OFFSET) != NULL) {
        return 0;
    }

    void *bus = read_ptr(dev, KB_LINUX_6_8_DEVICE_BUS_OFFSET);
    int (*probe)(void *) = (int (*)(void *))read_ptr(bus, KB_LINUX_6_8_BUS_PROBE_OFFSET);
    int (*driver_probe)(void *) = (int (*)(void *))read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_PROBE_OFFSET);
    write_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_OFFSET, driver);
    int result = probe != NULL ? probe(dev) : (driver_probe != NULL ? driver_probe(dev) : 0);
    if (trace_device_enabled()) {
        fprintf(stderr, "kobox device: probe dev=%p driver=%p result=%d\n", dev, driver, result);
    }
    if (result != 0) {
        write_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_OFFSET, NULL);
    }
    return result;
}

static void attach_device(void *dev)
{
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (driver_records[i].active) {
            (void)probe_device_with_driver(dev, driver_records[i].driver);
        }
    }
}

static void attach_driver(void *driver)
{
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (device_records[i].active) {
            (void)probe_device_with_driver(device_records[i].dev, driver);
        }
    }
}

int kb_dev_set_name(void *dev, const char *fmt, ...)
{
    if (dev == NULL || fmt == NULL) {
        return -22;
    }

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return -22;
    }

    char *name = calloc(1, (size_t)needed + 1);
    if (name == NULL) {
        va_end(args);
        return -12;
    }
    (void)vsnprintf(name, (size_t)needed + 1, fmt, args);
    va_end(args);

    memcpy((unsigned char *)dev + KB_LINUX_DEVICE_KOBJ_NAME_OFFSET, &name, sizeof(name));
    return 0;
}

int kb_device_add(void *dev)
{
    if (record_device(dev) == NULL) {
        return -12;
    }
    if (trace_device_enabled()) {
        fprintf(stderr, "kobox device: add dev=%p bus=%p\n", dev, read_ptr(dev, KB_LINUX_6_8_DEVICE_BUS_OFFSET));
    }
    attach_device(dev);
    return 0;
}

int kb_device_register(void *dev)
{
    return kb_device_add(dev);
}

int kb_driver_register(void *driver)
{
    if (record_driver(driver) == NULL) {
        return -12;
    }
    if (trace_device_enabled()) {
        fprintf(stderr, "kobox device: driver_register driver=%p bus=%p\n",
            driver,
            read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET));
    }
    attach_driver(driver);
    return 0;
}

void kb_driver_unregister(void *driver)
{
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (driver_records[i].active && driver_records[i].driver == driver) {
            memset(&driver_records[i], 0, sizeof(driver_records[i]));
        }
    }
}

int kb_driver_attach(void *driver)
{
    (void)record_driver(driver);
    attach_driver(driver);
    return 0;
}

int kb_bus_for_each_dev(void *bus, void *start, void *data, int (*fn)(void *dev, void *data))
{
    if (fn == NULL) {
        return -22;
    }
    int after_start = start == NULL;
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (!device_records[i].active || (bus != NULL && device_records[i].bus != bus)) {
            continue;
        }
        if (!after_start) {
            after_start = device_records[i].dev == start;
            continue;
        }
        int result = fn(device_records[i].dev, data);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

int kb_bus_for_each_drv(void *bus, void *start, void *data, int (*fn)(void *drv, void *data))
{
    if (fn == NULL) {
        return -22;
    }
    int after_start = start == NULL;
    for (size_t i = 0; i < KB_DRIVER_CORE_RECORD_MAX; i++) {
        if (!driver_records[i].active || (bus != NULL && driver_records[i].bus != bus)) {
            continue;
        }
        if (!after_start) {
            after_start = driver_records[i].driver == start;
            continue;
        }
        int result = fn(driver_records[i].driver, data);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

void *kb_kernfs_find_and_get_ns(void *parent, const char *name, const void *ns)
{
    if (name == NULL || !sysfs_dirent_enabled()) {
        return NULL;
    }
    for (size_t i = 0; i < KB_SYSFS_DIRENT_MAX; i++) {
        if (sysfs_dirent_records[i].active &&
            sysfs_dirent_records[i].parent == parent &&
            sysfs_dirent_records[i].ns == ns &&
            strcmp(sysfs_dirent_records[i].name, name) == 0)
        {
            return sysfs_dirent_records[i].storage;
        }
    }
    for (size_t i = 0; i < KB_SYSFS_DIRENT_MAX; i++) {
        if (!sysfs_dirent_records[i].active) {
            sysfs_dirent_records[i].active = 1;
            sysfs_dirent_records[i].parent = parent;
            sysfs_dirent_records[i].ns = ns;
            snprintf(sysfs_dirent_records[i].name, sizeof(sysfs_dirent_records[i].name), "%s", name);
            if (trace_device_enabled()) {
                fprintf(stderr,
                    "kobox device: sysfs_dirent parent=%p ns=%p name=%s node=%p\n",
                    parent,
                    ns,
                    sysfs_dirent_records[i].name,
                    (void *)sysfs_dirent_records[i].storage);
            }
            return sysfs_dirent_records[i].storage;
        }
    }
    return NULL;
}

void *kb_sysfs_get_dirent(void *parent, const char *name)
{
    return kb_kernfs_find_and_get_ns(parent, name, NULL);
}
