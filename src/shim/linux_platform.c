#include "kobox/shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kb_platform_driver_register(void *driver, void *owner, const char *mod_name)
{
    (void)driver;
    (void)owner;
    (void)mod_name;
    return 0;
}

void kb_platform_driver_unregister(void *driver)
{
    (void)driver;
}

int kb_devm_add_action(void *dev, void (*action)(void *), void *data)
{
    (void)dev;
    (void)action;
    (void)data;
    return 0;
}

int kb_devm_uio_register_device(void *dev, void *info)
{
    (void)dev;
    (void)info;
    return 0;
}

static void kb_vdev_log(const char *fmt, va_list args)
{
    if (fmt != NULL) {
        (void)kb_vprintk_safe(fmt, args);
    }
}

void kb_dynamic_dev_dbg(void *descriptor, const void *dev, const char *fmt, ...)
{
    (void)descriptor;
    (void)dev;
    va_list args;
    va_start(args, fmt);
    kb_vdev_log(fmt, args);
    va_end(args);
}

void kb_dev_err(const void *dev, const char *fmt, ...)
{
    (void)dev;
    va_list args;
    va_start(args, fmt);
    kb_vdev_log(fmt, args);
    va_end(args);
}

void kb_dev_warn(const void *dev, const char *fmt, ...)
{
    (void)dev;
    va_list args;
    va_start(args, fmt);
    kb_vdev_log(fmt, args);
    va_end(args);
}

void kb_pm_runtime_enable(void *dev)
{
    (void)dev;
}

void kb_pm_runtime_disable(void *dev, int check_resume)
{
    (void)dev;
    (void)check_resume;
}

int kb_pm_runtime_idle(void *dev, int rpmflags)
{
    (void)dev;
    (void)rpmflags;
    return 0;
}

int kb_pm_runtime_resume(void *dev, int rpmflags)
{
    (void)dev;
    (void)rpmflags;
    return 0;
}

void *kb_devm_kmalloc(void *dev, size_t size, unsigned int flags)
{
    (void)dev;
    return kb_kmalloc(size, flags);
}

char *kb_devm_kasprintf(void *dev, unsigned int flags, const char *fmt, ...)
{
    (void)dev;
    (void)flags;
    if (fmt == NULL) {
        return NULL;
    }

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    char *text = malloc((size_t)needed + 1);
    if (text == NULL) {
        va_end(args);
        return NULL;
    }
    (void)vsnprintf(text, (size_t)needed + 1, fmt, args);
    va_end(args);
    return text;
}

int kb_platform_get_irq_optional(void *pdev, unsigned int num)
{
    (void)pdev;
    (void)num;
    return -6;
}

void kb_disable_irq_nosync(unsigned int irq)
{
    (void)irq;
}

void kb_enable_irq(unsigned int irq)
{
    (void)irq;
}

void *kb_irq_get_irq_data(unsigned int irq)
{
    (void)irq;
    return NULL;
}

void kb_irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
    (void)irq;
    (void)clr;
    (void)set;
}

void kb_raw_spin_lock(void *lock)
{
    (void)lock;
}

int kb_raw_spin_trylock(void *lock)
{
    (void)lock;
    return 1;
}

unsigned long kb_raw_spin_lock_irqsave(void *lock)
{
    (void)lock;
    return 0;
}

void kb_raw_spin_unlock(void *lock)
{
    (void)lock;
}

void kb_raw_spin_unlock_irqrestore(void *lock, unsigned long flags)
{
    (void)lock;
    (void)flags;
}

int kb_atomic_notifier_chain_register(void *list, void *notifier)
{
    (void)list;
    (void)notifier;
    return 0;
}

int kb_atomic_notifier_chain_unregister(void *list, void *notifier)
{
    (void)list;
    (void)notifier;
    return 0;
}

int kb_kexec_crash_loaded(void)
{
    return 0;
}

int kb_kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
    if (s == NULL || res == NULL) {
        return -22;
    }
    char *end = NULL;
    unsigned long value = strtoul(s, &end, base);
    if (end == s) {
        return -22;
    }
    *res = (unsigned int)value;
    return 0;
}

int kb_sysfs_emit(char *buf, const char *fmt, ...)
{
    if (buf == NULL || fmt == NULL) {
        return -22;
    }
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buf, 4096, fmt, args);
    va_end(args);
    return result;
}

int __platform_driver_register(void *driver, void *owner, const char *mod_name)
{
    return kb_platform_driver_register(driver, owner, mod_name);
}

void platform_driver_unregister(void *driver)
{
    kb_platform_driver_unregister(driver);
}

int __devm_add_action(void *dev, void (*action)(void *), void *data)
{
    return kb_devm_add_action(dev, action, data);
}

int __devm_uio_register_device(void *dev, void *info)
{
    return kb_devm_uio_register_device(dev, info);
}

void __dynamic_dev_dbg(void *descriptor, const void *dev, const char *fmt, ...)
{
    (void)descriptor;
    (void)dev;
    va_list args;
    va_start(args, fmt);
    kb_vdev_log(fmt, args);
    va_end(args);
}

void _dev_err(const void *dev, const char *fmt, ...)
{
    (void)dev;
    va_list args;
    va_start(args, fmt);
    kb_vdev_log(fmt, args);
    va_end(args);
}

void _dev_warn(const void *dev, const char *fmt, ...)
{
    (void)dev;
    va_list args;
    va_start(args, fmt);
    kb_vdev_log(fmt, args);
    va_end(args);
}

void pm_runtime_enable(void *dev)
{
    kb_pm_runtime_enable(dev);
}

void __pm_runtime_disable(void *dev, int check_resume)
{
    kb_pm_runtime_disable(dev, check_resume);
}

int __pm_runtime_idle(void *dev, int rpmflags)
{
    return kb_pm_runtime_idle(dev, rpmflags);
}

int __pm_runtime_resume(void *dev, int rpmflags)
{
    return kb_pm_runtime_resume(dev, rpmflags);
}

void *devm_kmalloc(void *dev, size_t size, unsigned int flags)
{
    return kb_devm_kmalloc(dev, size, flags);
}

char *devm_kasprintf(void *dev, unsigned int flags, const char *fmt, ...)
{
    (void)dev;
    (void)flags;
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = fmt == NULL ? -1 : vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }
    char *text = malloc((size_t)needed + 1);
    if (text != NULL) {
        (void)vsnprintf(text, (size_t)needed + 1, fmt, args);
    }
    va_end(args);
    return text;
}

int platform_get_irq_optional(void *pdev, unsigned int num)
{
    return kb_platform_get_irq_optional(pdev, num);
}

void disable_irq_nosync(unsigned int irq)
{
    kb_disable_irq_nosync(irq);
}

void enable_irq(unsigned int irq)
{
    kb_enable_irq(irq);
}

void *irq_get_irq_data(unsigned int irq)
{
    return kb_irq_get_irq_data(irq);
}

void irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
    kb_irq_modify_status(irq, clr, set);
}

void _raw_spin_lock(void *lock)
{
    kb_raw_spin_lock(lock);
}

int _raw_spin_trylock(void *lock)
{
    return kb_raw_spin_trylock(lock);
}

unsigned long _raw_spin_lock_irqsave(void *lock)
{
    return kb_raw_spin_lock_irqsave(lock);
}

void _raw_spin_unlock(void *lock)
{
    kb_raw_spin_unlock(lock);
}

void _raw_spin_unlock_irqrestore(void *lock, unsigned long flags)
{
    kb_raw_spin_unlock_irqrestore(lock, flags);
}

int atomic_notifier_chain_register(void *list, void *notifier)
{
    return kb_atomic_notifier_chain_register(list, notifier);
}

int atomic_notifier_chain_unregister(void *list, void *notifier)
{
    return kb_atomic_notifier_chain_unregister(list, notifier);
}

int kexec_crash_loaded(void)
{
    return kb_kexec_crash_loaded();
}

int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
    return kb_kstrtouint(s, base, res);
}

int sysfs_emit(char *buf, const char *fmt, ...)
{
    if (buf == NULL || fmt == NULL) {
        return -22;
    }
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buf, 4096, fmt, args);
    va_end(args);
    return result;
}
