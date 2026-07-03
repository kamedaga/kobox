#include "kobox/shim.h"
#include "linux_subsystem/net/net_device.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_LINUX_DEVICE_KOBJ_NAME_OFFSET = 0,
    KB_LINUX_6_8_DEVICE_PARENT_OFFSET = 0x40,
    KB_LINUX_6_8_DEVICE_TYPE_OFFSET = 0x58,
    KB_LINUX_6_8_DEVICE_BUS_OFFSET = 0x60,
    KB_LINUX_6_8_DEVICE_DRIVER_OFFSET = 0x68,
    KB_LINUX_6_8_DEVICE_DRIVER_DATA_OFFSET = 0x78,
    KB_LINUX_3_10_DEVICE_BUS_OFFSET = 0x88,
    KB_LINUX_3_10_DEVICE_DRIVER_OFFSET = 0x90,
    KB_LINUX_6_8_DEVICE_DRIVER_NAME_OFFSET = 0x00,
    KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET = 0x08,
    KB_LINUX_6_8_DEVICE_DRIVER_PROBE_OFFSET = 0x38,
    KB_LINUX_6_8_BUS_NAME_OFFSET = 0x00,
    KB_LINUX_6_8_BUS_MATCH_OFFSET = 0x28,
    KB_LINUX_6_8_BUS_PROBE_OFFSET = 0x38,
    KB_LINUX_3_10_BUS_MATCH_OFFSET = 0x48,
    KB_LINUX_3_10_BUS_PROBE_OFFSET = 0x58,
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

static int pointer_is_error_or_low(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    return value < 4096u || value >= UINTPTR_MAX - 4095u;
}

static const char *read_string_ptr(const void *base, size_t offset)
{
    const char *value = NULL;
    if (base != NULL) {
        memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    }
    if (pointer_is_error_or_low(value)) {
        return NULL;
    }
    return value;
}

static const char *trace_name_or_null(const char *value)
{
    return value != NULL ? value : "(null)";
}

static const char *device_name(void *dev)
{
    return read_string_ptr(dev, KB_LINUX_DEVICE_KOBJ_NAME_OFFSET);
}

static const char *driver_name(void *driver)
{
    return read_string_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_NAME_OFFSET);
}

static const char *bus_name(void *bus)
{
    return read_string_ptr(bus, KB_LINUX_6_8_BUS_NAME_OFFSET);
}

static void *device_bus_ptr(void *dev)
{
    void *bus = read_ptr(dev, KB_LINUX_6_8_DEVICE_BUS_OFFSET);
    if (bus == NULL) {
        bus = read_ptr(dev, KB_LINUX_3_10_DEVICE_BUS_OFFSET);
    }
    return bus;
}

static void *device_driver_ptr(void *dev)
{
    void *driver = read_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_OFFSET);
    if (driver == NULL) {
        driver = read_ptr(dev, KB_LINUX_3_10_DEVICE_DRIVER_OFFSET);
    }
    return driver;
}

static void set_device_driver_ptr(void *dev, void *driver)
{
    write_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_OFFSET, driver);
    write_ptr(dev, KB_LINUX_3_10_DEVICE_DRIVER_OFFSET, driver);
}

static int (*bus_match_ptr(void *bus))(void *, void *)
{
    int (*match)(void *, void *) = (int (*)(void *, void *))read_ptr(bus, KB_LINUX_6_8_BUS_MATCH_OFFSET);
    if (match == NULL) {
        match = (int (*)(void *, void *))read_ptr(bus, KB_LINUX_3_10_BUS_MATCH_OFFSET);
    }
    return match;
}

static int (*bus_probe_ptr(void *bus))(void *)
{
    int (*probe)(void *) = (int (*)(void *))read_ptr(bus, KB_LINUX_6_8_BUS_PROBE_OFFSET);
    if (probe == NULL) {
        probe = (int (*)(void *))read_ptr(bus, KB_LINUX_3_10_BUS_PROBE_OFFSET);
    }
    return probe;
}

static int enter_function_gs(const void *function, unsigned long *old_gs)
{
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(function);
    if (kernel_gs == 0) {
        kernel_gs = kb_shim_current_kernel_gs();
    }
    if (kernel_gs == 0) {
        return 0;
    }
    return kb_shim_enter_kernel_gs(kernel_gs, old_gs) == 0;
}

static int call_module_int_ptr(int (*fn)(void *), void *arg0)
{
    if (fn == NULL) {
        return 0;
    }
    unsigned long old_gs = 0;
    int has_gs = enter_function_gs((const void *)fn, &old_gs);
    int result = kb_linux_call_int_ptr(fn, arg0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static int call_module_int_ptr_ptr(int (*fn)(void *, void *), void *arg0, void *arg1)
{
    if (fn == NULL) {
        return 0;
    }
    unsigned long old_gs = 0;
    int has_gs = enter_function_gs((const void *)fn, &old_gs);
    int result = kb_linux_call_int_ptr_ptr_raw(fn, arg0, arg1);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

void *kb_dev_get_drvdata(void *dev)
{
    return read_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_DATA_OFFSET);
}

void kb_dev_set_drvdata(void *dev, void *data)
{
    write_ptr(dev, KB_LINUX_6_8_DEVICE_DRIVER_DATA_OFFSET, data);
}

static kb_device_record_t *record_device(void *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    void *bus = device_bus_ptr(dev);
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
    void *dev_bus = device_bus_ptr(dev);
    void *driver_bus = read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET);
    if (dev_bus == NULL || dev_bus != driver_bus) {
        if (trace_device_enabled()) {
            fprintf(stderr,
                "kobox device: no-match dev=%p name=%s bus=%p/%s type=%p driver=%p name=%s bus=%p/%s\n",
                dev,
                trace_name_or_null(device_name(dev)),
                dev_bus,
                trace_name_or_null(bus_name(dev_bus)),
                read_ptr(dev, KB_LINUX_6_8_DEVICE_TYPE_OFFSET),
                driver,
                trace_name_or_null(driver_name(driver)),
                driver_bus,
                trace_name_or_null(bus_name(driver_bus)));
        }
        return 0;
    }

    int (*match)(void *, void *) = bus_match_ptr(dev_bus);
    if (match == NULL) {
        if (trace_device_enabled()) {
            fprintf(stderr,
                "kobox device: match dev=%p name=%s driver=%p name=%s bus=%p/%s result=1 no-bus-match\n",
                dev,
                trace_name_or_null(device_name(dev)),
                driver,
                trace_name_or_null(driver_name(driver)),
                dev_bus,
                trace_name_or_null(bus_name(dev_bus)));
        }
        return 1;
    }
    int result = call_module_int_ptr_ptr(match, dev, driver) > 0;
    if (trace_device_enabled()) {
        fprintf(stderr,
            "kobox device: match dev=%p name=%s driver=%p name=%s bus=%p/%s result=%d\n",
            dev,
            trace_name_or_null(device_name(dev)),
            driver,
            trace_name_or_null(driver_name(driver)),
            dev_bus,
            trace_name_or_null(bus_name(dev_bus)),
            result);
    }
    return result;
}

static int probe_device_with_driver(void *dev, void *driver)
{
    if (!device_matches_driver(dev, driver)) {
        return 0;
    }
    if (device_driver_ptr(dev) != NULL) {
        return 0;
    }

    void *bus = device_bus_ptr(dev);
    int (*probe)(void *) = bus_probe_ptr(bus);
    int (*driver_probe)(void *) = (int (*)(void *))read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_PROBE_OFFSET);
    set_device_driver_ptr(dev, driver);
    int result = probe != NULL ?
        call_module_int_ptr(probe, dev) :
        (driver_probe != NULL ? call_module_int_ptr(driver_probe, dev) : 0);
    if (trace_device_enabled()) {
        fprintf(stderr,
            "kobox device: probe dev=%p name=%s driver=%p name=%s result=%d\n",
            dev,
            trace_name_or_null(device_name(dev)),
            driver,
            trace_name_or_null(driver_name(driver)),
            result);
    }
    if (result != 0) {
        set_device_driver_ptr(dev, NULL);
    } else {
        kb_usb_observe_linux_device(dev);
        kb_net_device_run_pending_smokes();
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
    char stack_name[256];
    int needed = kb_vsnprintf_safe(stack_name, sizeof(stack_name), fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return -22;
    }

    char *name = kb_kzalloc((size_t)needed + 1, 0);
    if (name == NULL) {
        va_end(args);
        return -12;
    }
    (void)kb_vsnprintf_safe(name, (size_t)needed + 1, fmt, args);
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
        void *bus = device_bus_ptr(dev);
        fprintf(stderr,
            "kobox device: add dev=%p name=%s parent=%p type=%p bus=%p/%s driver=%p\n",
            dev,
            trace_name_or_null(device_name(dev)),
            read_ptr(dev, KB_LINUX_6_8_DEVICE_PARENT_OFFSET),
            read_ptr(dev, KB_LINUX_6_8_DEVICE_TYPE_OFFSET),
            bus,
            trace_name_or_null(bus_name(bus)),
            device_driver_ptr(dev));
    }
    if (trace_device_enabled()) {
        fprintf(stderr, "kobox device: observe begin dev=%p\n", dev);
    }
    kb_usb_observe_linux_device(dev);
    if (trace_device_enabled()) {
        fprintf(stderr, "kobox device: observe done dev=%p\n", dev);
        fprintf(stderr, "kobox device: attach begin dev=%p\n", dev);
    }
    attach_device(dev);
    if (trace_device_enabled()) {
        fprintf(stderr, "kobox device: attach done dev=%p\n", dev);
    }
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
        void *bus = read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_BUS_OFFSET);
        fprintf(stderr, "kobox device: driver_register driver=%p name=%s bus=%p/%s probe=%p\n",
            driver,
            trace_name_or_null(driver_name(driver)),
            bus,
            trace_name_or_null(bus_name(bus)),
            read_ptr(driver, KB_LINUX_6_8_DEVICE_DRIVER_PROBE_OFFSET));
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
        int result = call_module_int_ptr_ptr(fn, device_records[i].dev, data);
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
        int result = call_module_int_ptr_ptr(fn, driver_records[i].driver, data);
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
