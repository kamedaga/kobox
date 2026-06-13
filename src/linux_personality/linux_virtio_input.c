#include "kobox/shim.h"
#include "linux_subsystem/input/input.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kb_kmalloc_alias(size_t size, unsigned int flags)
{
    return kb_kmalloc(size, flags);
}

void kb_might_resched(void)
{
}

void kb_ubsan_handle_load_invalid_value(void *data, void *ptr)
{
    (void)data;
    (void)ptr;
}

void kb_ubsan_handle_out_of_bounds(void *data, void *index)
{
    (void)data;
    (void)index;
}

int kb_register_virtio_driver(void *driver)
{
    (void)driver;
    return 0;
}

void kb_unregister_virtio_driver(void *driver)
{
    (void)driver;
}

void kb_virtio_reset_device(void *device)
{
    (void)device;
}

int kb_virtqueue_add_inbuf(void *vq, void *sgs, unsigned int num, void *data, unsigned int gfp)
{
    (void)vq;
    (void)sgs;
    (void)num;
    (void)data;
    (void)gfp;
    return 0;
}

int kb_virtqueue_add_outbuf(void *vq, void *sgs, unsigned int num, void *data, unsigned int gfp)
{
    (void)vq;
    (void)sgs;
    (void)num;
    (void)data;
    (void)gfp;
    return 0;
}

void *kb_virtqueue_detach_unused_buf(void *vq)
{
    (void)vq;
    return NULL;
}

void *kb_virtqueue_get_buf(void *vq, unsigned int *len)
{
    (void)vq;
    if (len != NULL) {
        *len = 0;
    }
    return NULL;
}

unsigned int kb_virtqueue_get_vring_size(void *vq)
{
    (void)vq;
    return 0;
}

int kb_virtqueue_kick(void *vq)
{
    (void)vq;
    return 1;
}

void kb_sg_init_one(void *sg, const void *buf, unsigned int buflen)
{
    (void)buf;
    if (sg != NULL) {
        memset(sg, 0, buflen > 64 ? 64 : buflen);
    }
}

void *kb_input_allocate_device(void)
{
    return kb_input_subsystem_allocate_device();
}

void kb_input_free_device(void *dev)
{
    kb_input_subsystem_free_device(dev);
}

int kb_input_register_device(void *dev)
{
    return kb_input_subsystem_register_device(dev);
}

void kb_input_unregister_device(void *dev)
{
    kb_input_subsystem_unregister_device(dev);
}

void kb_input_event(void *dev, unsigned int type, unsigned int code, int value)
{
    kb_input_subsystem_record_event(dev, type, code, value);
}

void kb_input_set_abs_params(void *dev, unsigned int axis, int min, int max, int fuzz, int flat)
{
    kb_input_subsystem_set_abs_params(dev, axis, min, max, fuzz, flat);
}

void kb_input_alloc_absinfo(void *dev)
{
    kb_input_subsystem_alloc_absinfo(dev);
}

int kb_input_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags)
{
    return kb_input_subsystem_mt_init_slots(dev, num_slots, flags);
}

void *__kmalloc(size_t size, unsigned int flags)
{
    return kb_kmalloc_alias(size, flags);
}

void __SCT__might_resched(void)
{
    kb_might_resched();
}

void __ubsan_handle_load_invalid_value(void *data, void *ptr)
{
    kb_ubsan_handle_load_invalid_value(data, ptr);
}

void __ubsan_handle_out_of_bounds(void *data, void *index)
{
    kb_ubsan_handle_out_of_bounds(data, index);
}

int register_virtio_driver(void *driver)
{
    return kb_register_virtio_driver(driver);
}

void unregister_virtio_driver(void *driver)
{
    kb_unregister_virtio_driver(driver);
}

void virtio_reset_device(void *device)
{
    kb_virtio_reset_device(device);
}

int virtqueue_add_inbuf(void *vq, void *sgs, unsigned int num, void *data, unsigned int gfp)
{
    return kb_virtqueue_add_inbuf(vq, sgs, num, data, gfp);
}

int virtqueue_add_outbuf(void *vq, void *sgs, unsigned int num, void *data, unsigned int gfp)
{
    return kb_virtqueue_add_outbuf(vq, sgs, num, data, gfp);
}

void *virtqueue_detach_unused_buf(void *vq)
{
    return kb_virtqueue_detach_unused_buf(vq);
}

void *virtqueue_get_buf(void *vq, unsigned int *len)
{
    return kb_virtqueue_get_buf(vq, len);
}

unsigned int virtqueue_get_vring_size(void *vq)
{
    return kb_virtqueue_get_vring_size(vq);
}

int virtqueue_kick(void *vq)
{
    return kb_virtqueue_kick(vq);
}

void sg_init_one(void *sg, const void *buf, unsigned int buflen)
{
    kb_sg_init_one(sg, buf, buflen);
}

void *input_allocate_device(void)
{
    return kb_input_allocate_device();
}

void input_free_device(void *dev)
{
    kb_input_free_device(dev);
}

int input_register_device(void *dev)
{
    return kb_input_register_device(dev);
}

void input_unregister_device(void *dev)
{
    kb_input_unregister_device(dev);
}

void input_event(void *dev, unsigned int type, unsigned int code, int value)
{
    kb_input_event(dev, type, code, value);
}

void input_set_abs_params(void *dev, unsigned int axis, int min, int max, int fuzz, int flat)
{
    kb_input_set_abs_params(dev, axis, min, max, fuzz, flat);
}

void input_alloc_absinfo(void *dev)
{
    kb_input_alloc_absinfo(dev);
}

int input_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags)
{
    return kb_input_mt_init_slots(dev, num_slots, flags);
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}
